#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include <vector>
#include "secrets.h"
#include "web_page.h"
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

static const char* MDNS_HOSTNAME = "weather"; // reachable at http://weather.local/

// Bright palette mirroring the web dashboard's sky-blue theme
static const uint16_t CARD_BG     = RGB565(247, 251, 255);
static const uint16_t INK         = RGB565(15, 34, 56);
static const uint16_t INK_DIM     = RGB565(88, 114, 138);
static const uint16_t ACCENT_BLUE = RGB565(16, 122, 202);
static const uint16_t OFFLINE_RED = RGB565(200, 68, 60);
static const uint16_t DIVIDER     = RGB565(210, 225, 240);

static const int8_t TFT_SCLK = 4;
static const int8_t TFT_MOSI = 6;
static const int8_t TFT_CS = 7;
static const int8_t TFT_DC = 3;
static const int8_t TFT_RST = 1;

// The display's backlight pin is wired straight to 3V3 (no spare GPIO drives
// it), so there's no hardware dimming. DimmingGFX wraps the real panel and
// scales every color at the primitive draw calls (writePixelPreclipped,
// writeFillRectPreclipped, writeFastHLine/VLine, writeLine) before forwarding
// to it, so brightness/off apply to everything drawn through it without
// needing a full off-screen framebuffer (an Arduino_Canvas of this size
// reliably failed to allocate on this ESP32-C3 core and crashed on boot).
class DimmingGFX : public Arduino_GFX {
public:
  DimmingGFX(Arduino_GFX* target)
    : Arduino_GFX(target->width(), target->height()), _target(target) {}

  bool begin(int32_t speed = GFX_NOT_DEFINED) override { return _target->begin(speed); }
  void startWrite() override { _target->startWrite(); }
  void endWrite() override { _target->endWrite(); }
  void writePixelPreclipped(int16_t x, int16_t y, uint16_t color) override {
    _target->writePixelPreclipped(x, y, dim(color));
  }
  void writeFillRectPreclipped(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
    _target->writeFillRectPreclipped(x, y, w, h, dim(color));
  }
  void writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override {
    _target->writeFastHLine(x, y, w, dim(color));
  }
  void writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override {
    _target->writeFastVLine(x, y, h, dim(color));
  }
  void writeLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) override {
    _target->writeLine(x0, y0, x1, y1, dim(color));
  }

  void setOn(bool on) { _on = on; }
  void setBrightnessPct(uint8_t pct) { _brightnessPct = pct; }

private:
  Arduino_GFX* _target;
  bool _on = true;
  uint8_t _brightnessPct = 100;

  uint16_t dim(uint16_t c) {
    if (!_on) return 0x0000;
    if (_brightnessPct >= 100) return c;
    uint16_t scale = ((uint16_t)_brightnessPct * 256) / 100;
    uint8_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    r = (uint8_t)((r * scale) >> 8);
    g = (uint8_t)((g * scale) >> 8);
    b = (uint8_t)((b * scale) >> 8);
    return (uint16_t)((r << 11) | (g << 5) | b);
  }
};

Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, GFX_NOT_DEFINED);
Arduino_GC9A01 *panel = new Arduino_GC9A01(bus, TFT_RST, 0 /* rotation */, true /* IPS */);
DimmingGFX *gfx = new DimmingGFX(panel);

// ---- Display power: on/off, brightness, and a weekday/weekend schedule ----
// There's no spare GPIO on the backlight, so "brightness" and "off" are both
// applied by DimmingGFX (see above) rather than in hardware.
bool displayOn = true;
uint8_t displayBrightnessPct = 100; // 5-100

struct PowerSchedule {
  bool enabled = false;
  uint8_t weekdayOnHour = 7,  weekdayOnMinute = 0;
  uint8_t weekdayOffHour = 22, weekdayOffMinute = 0;
  uint8_t weekendOnHour = 8,  weekendOnMinute = 0;
  uint8_t weekendOffHour = 23, weekendOffMinute = 0;
};
PowerSchedule powerSchedule;

// Local wall-clock offset from UTC, refreshed from Open-Meteo's
// utc_offset_seconds each weather fetch (handles DST automatically without
// needing a hardcoded timezone).
long utcOffsetSeconds = -14400; // default: US Eastern

// Tracks the schedule's last computed on/off state so tickPowerSchedule()
// only forces displayOn at an actual transition, not every time it runs.
bool scheduleInitialized = false;
bool lastScheduledOn = true;

void centerText(const char* text, int16_t cx, int16_t cy, uint8_t size, const GFXfont* font = nullptr) {
  gfx->setFont(font);
  gfx->setTextSize(size);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor(cx - (int16_t)(w / 2) - x1, cy - (int16_t)(h / 2) - y1);
  gfx->print(text);
}

// Pushes the current on/off + brightness settings into the DimmingGFX layer.
// Call whenever either changes; the next draw calls pick it up immediately
// (existing on-screen pixels aren't retroactively changed, so pair this with
// a renderCurrentScreen() to refresh what's currently shown).
void applyDisplayPower() {
  gfx->setOn(displayOn);
  gfx->setBrightnessPct(displayBrightnessPct);
}

static const unsigned long UPDATE_INTERVAL_MS = 10UL * 60UL * 1000UL; // 10 minutes
static const unsigned long LOCAL_SENSOR_INTERVAL_MS = 60UL * 1000UL; // 1 minute; local + cheap, no rate limit to worry about
static const unsigned long AIR_QUALITY_INTERVAL_MS = 60UL * 1000UL; // same cadence as the local sensor poll

// ---- Location + unit settings (persisted, editable from the web UI) ----
String zipCode = "32082"; // Ponte Vedra Beach, FL
String latLon;            // resolved from zipCode at boot / whenever it changes
String placeName;         // resolved city name, e.g. "Ponte Vedra Beach"
bool useFahrenheit = false;
unsigned long screenCycleMs = 5000;
String localSensorHost = "localsensor.local"; // AHT20+BMP280 node's mDNS hostname or IP
String airQualityHost = "airquality.local";   // PMS5003 node's mDNS hostname or IP
Preferences settingsPrefs;

// ---- Which stats show on the round display (web UI can pick up to 4) ----
struct DisplayToggles {
  bool humidity = true;
  bool wind = true;
  bool rainChance = true;
  bool feelsLike = false;
  bool highLow = true;
  bool pressure = false;
  bool uv = false;
};
DisplayToggles displaySettings;
static const int MAX_DISPLAY_STATS = 4;

enum StatKind { STAT_HUMIDITY, STAT_WIND, STAT_RAIN, STAT_FEEL, STAT_HILO, STAT_PRESSURE, STAT_UV };
static const StatKind STAT_ORDER[] = { STAT_HUMIDITY, STAT_WIND, STAT_RAIN, STAT_FEEL, STAT_HILO, STAT_PRESSURE, STAT_UV };
static const int STAT_ORDER_COUNT = 7;

bool statEnabled(StatKind k) {
  switch (k) {
    case STAT_HUMIDITY: return displaySettings.humidity;
    case STAT_WIND:      return displaySettings.wind;
    case STAT_RAIN:      return displaySettings.rainChance;
    case STAT_FEEL:      return displaySettings.feelsLike;
    case STAT_HILO:      return displaySettings.highLow;
    case STAT_PRESSURE:  return displaySettings.pressure;
    case STAT_UV:        return displaySettings.uv;
    default:             return false;
  }
}

void setStatEnabled(StatKind k, bool v) {
  switch (k) {
    case STAT_HUMIDITY: displaySettings.humidity = v; break;
    case STAT_WIND:      displaySettings.wind = v; break;
    case STAT_RAIN:      displaySettings.rainChance = v; break;
    case STAT_FEEL:      displaySettings.feelsLike = v; break;
    case STAT_HILO:      displaySettings.highLow = v; break;
    case STAT_PRESSURE:  displaySettings.pressure = v; break;
    case STAT_UV:        displaySettings.uv = v; break;
  }
}

// Keeps at most MAX_DISPLAY_STATS toggles on, dropping overflow in a fixed
// priority order so the round display never gets asked to cram in more than
// it has room for.
void clampDisplayToggles() {
  int count = 0;
  for (int i = 0; i < STAT_ORDER_COUNT; i++) {
    if (statEnabled(STAT_ORDER[i])) {
      count++;
      if (count > MAX_DISPLAY_STATS) setStatEnabled(STAT_ORDER[i], false);
    }
  }
}

static const int HOURLY_COUNT = 4; // shown at +3h, +6h, +9h, +12h from now
static const int DAILY_COUNT = 5;  // today + next 4 days
static const int NUM_FIXED_SCREENS = 5; // 0=weather, 1=hourly, 2=5-day, 3=local sensor, 4=air quality; notes follow

struct WeatherData {
  float temperatureC = NAN;
  float feelsLikeC = NAN;
  float humidityPct = NAN;
  float pressureHpa = NAN;
  float windKph = NAN;
  float windGustKph = NAN;
  float rainChancePct = NAN;
  float uvIndex = NAN;
  float highC = NAN;
  float lowC = NAN;
  int weatherCode = -1;
  bool isDay = true;
  String condition;

  struct HourPoint {
    float tempC = NAN;
    int code = -1;
    bool isDay = true;
    int hourOfDay = 0; // 0-23, local
  };
  HourPoint hourly[HOURLY_COUNT];

  struct DayPoint {
    float highC = NAN;
    float lowC = NAN;
    int code = -1;
    int weekday = 0; // 0=Sun..6=Sat
  };
  DayPoint daily[DAILY_COUNT];
};

WeatherData currentWeather;
bool haveData = false;
unsigned long lastFetchMs = 0;

// ---- Local sensor (AHT20+BMP280 node), fetched from its own tiny HTTP API ----
struct LocalSensorReading {
  float tempC = NAN;
  float humidityPct = NAN;
  float pressureHpa = NAN;
  bool online = false;
  // Cached Supabase forecast (pressure-history-based rain likelihood), as
  // last relayed by the sensor node's own /data response. Not always
  // present -- the node only has this once it's posted enough telemetry.
  bool haveForecast = false;
  String forecastState;
  int rainProbabilityPct = -1;
  String pressureTrend;
};
LocalSensorReading localSensor;
bool haveLocalSensor = false;
unsigned long lastLocalFetchMs = 0;

// ---- Air quality (PMS5003 particulate node), fetched the same way ----
struct AirQualityReading {
  bool online = false;
  int pm1_0 = 0;   // ug/m3, atmospheric
  int pm2_5 = 0;   // ug/m3, atmospheric
  int pm10 = 0;    // ug/m3, atmospheric
  int aqi = -1;    // US EPA PM2.5 AQI, as computed by the node
  String aqiCategory;
};
AirQualityReading airQuality;
bool haveAirQuality = false;
unsigned long lastAirFetchMs = 0;

// ---- Notes feature ----
static const size_t MAX_NOTES = 10;
static const size_t MAX_NOTE_LEN = 80;

std::vector<String> notes;
Preferences prefs;
WebServer server(80);

int currentScreen = 0; // 0 = weather, 1..N = notes[screen-1]
unsigned long lastScreenSwitchMs = 0;

void renderCurrentScreen();

void saveNotes() {
  String joined;
  for (size_t i = 0; i < notes.size(); i++) {
    if (i) joined += "\n";
    joined += notes[i];
  }
  prefs.putString("list", joined);
}

void loadNotes() {
  notes.clear();
  String joined = prefs.getString("list", "");
  int start = 0;
  while (start <= (int)joined.length()) {
    int nl = joined.indexOf('\n', start);
    if (nl < 0) nl = joined.length();
    String line = joined.substring(start, nl);
    if (line.length()) notes.push_back(line);
    start = nl + 1;
  }
}

bool addNote(String text) {
  text.trim();
  if (text.isEmpty()) return false;
  if (text.length() > MAX_NOTE_LEN) text = text.substring(0, MAX_NOTE_LEN);
  if (notes.size() >= MAX_NOTES) notes.erase(notes.begin());
  notes.push_back(text);
  saveNotes();
  return true;
}

void deleteNote(int index) {
  if (index < 0 || index >= (int)notes.size()) return;
  notes.erase(notes.begin() + index);
  saveNotes();
}

// Keeps currentScreen valid after notes are added/removed.
void clampCurrentScreen() {
  int screenCount = NUM_FIXED_SCREENS + (int)notes.size();
  if (currentScreen < 0 || currentScreen >= screenCount) currentScreen = 0;
}

void saveSettings() {
  settingsPrefs.putString("zip", zipCode);
  settingsPrefs.putBool("fahrenheit", useFahrenheit);
  settingsPrefs.putULong("cycleMs", screenCycleMs);
  settingsPrefs.putString("localHost", localSensorHost);
  settingsPrefs.putString("airHost", airQualityHost);
  settingsPrefs.putBool("s_hum", displaySettings.humidity);
  settingsPrefs.putBool("s_wind", displaySettings.wind);
  settingsPrefs.putBool("s_rain", displaySettings.rainChance);
  settingsPrefs.putBool("s_feel", displaySettings.feelsLike);
  settingsPrefs.putBool("s_hilo", displaySettings.highLow);
  settingsPrefs.putBool("s_pres", displaySettings.pressure);
  settingsPrefs.putBool("s_uv", displaySettings.uv);
  settingsPrefs.putBool("pwr_on", displayOn);
  settingsPrefs.putUChar("pwr_bright", displayBrightnessPct);
  settingsPrefs.putBool("sch_en", powerSchedule.enabled);
  settingsPrefs.putUChar("sch_wd_onH", powerSchedule.weekdayOnHour);
  settingsPrefs.putUChar("sch_wd_onM", powerSchedule.weekdayOnMinute);
  settingsPrefs.putUChar("sch_wd_offH", powerSchedule.weekdayOffHour);
  settingsPrefs.putUChar("sch_wd_offM", powerSchedule.weekdayOffMinute);
  settingsPrefs.putUChar("sch_we_onH", powerSchedule.weekendOnHour);
  settingsPrefs.putUChar("sch_we_onM", powerSchedule.weekendOnMinute);
  settingsPrefs.putUChar("sch_we_offH", powerSchedule.weekendOffHour);
  settingsPrefs.putUChar("sch_we_offM", powerSchedule.weekendOffMinute);
}

void loadSettings() {
  zipCode = settingsPrefs.getString("zip", zipCode);
  useFahrenheit = settingsPrefs.getBool("fahrenheit", useFahrenheit);
  screenCycleMs = settingsPrefs.getULong("cycleMs", screenCycleMs);
  localSensorHost = settingsPrefs.getString("localHost", localSensorHost);
  airQualityHost = settingsPrefs.getString("airHost", airQualityHost);
  displaySettings.humidity = settingsPrefs.getBool("s_hum", displaySettings.humidity);
  displaySettings.wind = settingsPrefs.getBool("s_wind", displaySettings.wind);
  displaySettings.rainChance = settingsPrefs.getBool("s_rain", displaySettings.rainChance);
  displaySettings.feelsLike = settingsPrefs.getBool("s_feel", displaySettings.feelsLike);
  displaySettings.highLow = settingsPrefs.getBool("s_hilo", displaySettings.highLow);
  displaySettings.pressure = settingsPrefs.getBool("s_pres", displaySettings.pressure);
  displaySettings.uv = settingsPrefs.getBool("s_uv", displaySettings.uv);
  clampDisplayToggles();

  displayOn = settingsPrefs.getBool("pwr_on", displayOn);
  displayBrightnessPct = settingsPrefs.getUChar("pwr_bright", displayBrightnessPct);
  powerSchedule.enabled = settingsPrefs.getBool("sch_en", powerSchedule.enabled);
  powerSchedule.weekdayOnHour = settingsPrefs.getUChar("sch_wd_onH", powerSchedule.weekdayOnHour);
  powerSchedule.weekdayOnMinute = settingsPrefs.getUChar("sch_wd_onM", powerSchedule.weekdayOnMinute);
  powerSchedule.weekdayOffHour = settingsPrefs.getUChar("sch_wd_offH", powerSchedule.weekdayOffHour);
  powerSchedule.weekdayOffMinute = settingsPrefs.getUChar("sch_wd_offM", powerSchedule.weekdayOffMinute);
  powerSchedule.weekendOnHour = settingsPrefs.getUChar("sch_we_onH", powerSchedule.weekendOnHour);
  powerSchedule.weekendOnMinute = settingsPrefs.getUChar("sch_we_onM", powerSchedule.weekendOnMinute);
  powerSchedule.weekendOffHour = settingsPrefs.getUChar("sch_we_offH", powerSchedule.weekendOffHour);
  powerSchedule.weekendOffMinute = settingsPrefs.getUChar("sch_we_offM", powerSchedule.weekendOffMinute);
}

bool isValidZip(const String& zip) {
  if (zip.length() != 5) return false;
  for (size_t i = 0; i < zip.length(); i++) {
    if (!isDigit(zip[i])) return false;
  }
  return true;
}

bool httpGetJson(const String& url, JsonDocument& doc, const JsonDocument* filter);

// Looks up lat/lon (and a human place name) for a US zip code via the free
// Zippopotam.us API, since the weather API needs coordinates, not a zip.
bool geocodeZip(const String& zip, String& outLatLon, String& outPlace) {
  String url = "https://api.zippopotam.us/us/" + zip;
  JsonDocument filter;
  filter["places"][0]["latitude"] = true;
  filter["places"][0]["longitude"] = true;
  filter["places"][0]["place name"] = true;
  JsonDocument doc;
  if (!httpGetJson(url, doc, &filter)) return false;

  const char* lat = doc["places"][0]["latitude"];
  const char* lon = doc["places"][0]["longitude"];
  if (!lat || !lon) return false;
  outLatLon = String(lat) + "," + String(lon);

  const char* place = doc["places"][0]["place name"];
  outPlace = place ? String(place) : String("");
  return true;
}

bool httpGetJson(const String& url, JsonDocument& doc, const JsonDocument* filter = nullptr) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    return false;
  }
  http.addHeader("User-Agent", "esp32-weather-station/2.0 (shoene623@gmail.com)");
  http.addHeader("Accept", "application/json");
  http.setTimeout(10000);

  int code = http.GET();
  Serial.printf("GET %s -> code=%d\n", url.c_str(), code);
  bool ok = false;
  if (code == HTTP_CODE_OK) {
    // Buffer the body into a String rather than deserializing straight from
    // http.getStream(): with a Filter applied, parsing directly off the
    // HTTPClient stream silently yields an empty document for chunked
    // responses (e.g. Open-Meteo) even though the GET itself succeeds.
    String payload = http.getString();
    DeserializationError err = filter
      ? deserializeJson(doc, payload, DeserializationOption::Filter(*filter))
      : deserializeJson(doc, payload);
    ok = (err == DeserializationError::Ok);
    if (!ok) {
      Serial.printf("JSON parse failed for %s: %s\n", url.c_str(), err.c_str());
    }
  }
  http.end();
  return ok;
}

// Plain-HTTP GET for the local sensor node (it's on the LAN, no TLS), kept
// separate from httpGetJson() above which always speaks TLS to the outdoor
// weather/geocoding APIs.
bool httpGetJsonPlain(const String& url, JsonDocument& doc) {
  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  http.setTimeout(5000);
  int code = http.GET();
  bool ok = false;
  if (code == HTTP_CODE_OK) {
    String payload = http.getString();
    ok = (deserializeJson(doc, payload) == DeserializationError::Ok);
  }
  http.end();
  return ok;
}

// Polls the AHT20+BMP280 sensor node's tiny JSON API for hyper-local
// temperature/humidity/pressure. The node is expected at localSensorHost
// (mDNS hostname or IP), serving GET /data.
bool fetchLocalSensor(const String& host, LocalSensorReading& out) {
  if (host.isEmpty()) return false;
  String url = "http://" + host + "/data";
  JsonDocument doc;
  if (!httpGetJsonPlain(url, doc)) return false;
  out.tempC = doc["tempC"] | NAN;
  out.humidityPct = doc["humidityPct"] | NAN;
  out.pressureHpa = doc["pressureHpa"] | NAN;
  out.online = doc["online"] | false;

  JsonVariant fc = doc["forecast"];
  out.haveForecast = !fc.isNull();
  if (out.haveForecast) {
    out.forecastState = fc["state"] | "";
    out.rainProbabilityPct = fc["rainProbabilityPct"] | -1;
    out.pressureTrend = fc["pressureTrend"] | "";
  }
  return true;
}

// Polls the PMS5003 particulate node's tiny JSON API for PM2.5/PM10
// (atmospheric/"env" concentrations) and the US EPA PM2.5 AQI the node
// derives from them. Expected at airQualityHost (mDNS hostname or IP),
// serving GET /data, mirroring fetchLocalSensor() above.
bool fetchAirQuality(const String& host, AirQualityReading& out) {
  if (host.isEmpty()) return false;
  String url = "http://" + host + "/data";
  JsonDocument doc;
  if (!httpGetJsonPlain(url, doc)) return false;
  out.online = doc["online"] | false;
  out.pm1_0 = doc["pm1_0"] | 0;
  out.pm2_5 = doc["pm2_5"] | 0;
  out.pm10 = doc["pm10"] | 0;
  out.aqi = doc["aqi"] | -1;
  out.aqiCategory = doc["aqiCategory"] | "";
  return true;
}

enum SkyCategory {
  SKY_CLEAR,
  SKY_PARTLY_CLOUDY,
  SKY_CLOUDY,
  SKY_RAIN,
  SKY_STORM,
  SKY_SNOW,
  SKY_FOG,
  SKY_UNKNOWN
};

struct CodeInfo { SkyCategory cat; const char* text; };

// Maps Open-Meteo's WMO weather codes to a sky category (for the artwork)
// and a short human-readable label.
CodeInfo wmoInfo(int code) {
  switch (code) {
    case 0:  return { SKY_CLEAR, "Clear sky" };
    case 1:  return { SKY_CLEAR, "Mostly clear" };
    case 2:  return { SKY_PARTLY_CLOUDY, "Partly cloudy" };
    case 3:  return { SKY_CLOUDY, "Overcast" };
    case 45: case 48: return { SKY_FOG, "Fog" };
    case 51: case 53: case 55: return { SKY_RAIN, "Drizzle" };
    case 56: case 57: return { SKY_RAIN, "Freezing drizzle" };
    case 61: case 63: case 65: return { SKY_RAIN, "Rain" };
    case 66: case 67: return { SKY_RAIN, "Freezing rain" };
    case 71: case 73: case 75: case 77: return { SKY_SNOW, "Snow" };
    case 80: case 81: case 82: return { SKY_RAIN, "Rain showers" };
    case 85: case 86: return { SKY_SNOW, "Snow showers" };
    case 95: return { SKY_STORM, "Thunderstorm" };
    case 96: case 99: return { SKY_STORM, "Thunderstorm, hail" };
    default: return { SKY_UNKNOWN, "--" };
  }
}

// Single-call weather source: current conditions plus today's high/low, peak
// rain chance and UV index, all from Open-Meteo (free, no API key).
bool fetchWeather(const String& latLonStr, WeatherData& weather) {
  int comma = latLonStr.indexOf(',');
  if (comma < 0) return false;
  String lat = latLonStr.substring(0, comma);
  String lon = latLonStr.substring(comma + 1);

  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + lat + "&longitude=" + lon +
    "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m,wind_gusts_10m,surface_pressure,is_day"
    "&hourly=temperature_2m,weather_code,is_day"
    "&daily=temperature_2m_max,temperature_2m_min,weather_code,precipitation_probability_max,uv_index_max"
    "&timezone=auto&temperature_unit=celsius&wind_speed_unit=kmh&forecast_days=5&forecast_hours=36";

  JsonDocument filter;
  filter["utc_offset_seconds"] = true;
  filter["current"]["temperature_2m"] = true;
  filter["current"]["relative_humidity_2m"] = true;
  filter["current"]["apparent_temperature"] = true;
  filter["current"]["weather_code"] = true;
  filter["current"]["wind_speed_10m"] = true;
  filter["current"]["wind_gusts_10m"] = true;
  filter["current"]["surface_pressure"] = true;
  filter["current"]["is_day"] = true;
  filter["hourly"]["temperature_2m"] = true;
  filter["hourly"]["weather_code"] = true;
  filter["hourly"]["is_day"] = true;
  filter["daily"]["temperature_2m_max"] = true;
  filter["daily"]["temperature_2m_min"] = true;
  filter["daily"]["weather_code"] = true;
  filter["daily"]["precipitation_probability_max"] = true;
  filter["daily"]["uv_index_max"] = true;

  JsonDocument doc;
  if (!httpGetJson(url, doc, &filter)) return false;

  JsonVariant cur = doc["current"];
  if (cur.isNull()) return false;

  utcOffsetSeconds = doc["utc_offset_seconds"] | utcOffsetSeconds;

  weather.temperatureC = cur["temperature_2m"] | NAN;
  weather.feelsLikeC = cur["apparent_temperature"] | NAN;
  weather.humidityPct = cur["relative_humidity_2m"] | NAN;
  weather.pressureHpa = cur["surface_pressure"] | NAN;
  weather.windKph = cur["wind_speed_10m"] | NAN;
  weather.windGustKph = cur["wind_gusts_10m"] | NAN;
  weather.weatherCode = cur["weather_code"] | -1;
  weather.isDay = (cur["is_day"] | 1) != 0;

  JsonVariant daily = doc["daily"];
  weather.highC = daily["temperature_2m_max"][0] | NAN;
  weather.lowC = daily["temperature_2m_min"][0] | NAN;
  weather.rainChancePct = daily["precipitation_probability_max"][0] | NAN;
  weather.uvIndex = daily["uv_index_max"][0] | NAN;

  weather.condition = wmoInfo(weather.weatherCode).text;

  // Local hour/weekday, used to index into the hourly/daily arrays below
  // (the arrays themselves aren't fetched with timestamps, to save memory).
  int nowHour = 12, nowWeekday = 0;
  time_t now;
  time(&now);
  if (now >= 1700000000L) {
    time_t local = now + utcOffsetSeconds;
    struct tm t;
    gmtime_r(&local, &t);
    nowHour = t.tm_hour;
    nowWeekday = t.tm_wday;
  }

  JsonArray hTemp = doc["hourly"]["temperature_2m"];
  JsonArray hCode = doc["hourly"]["weather_code"];
  JsonArray hDay = doc["hourly"]["is_day"];
  int hourlyLen = hTemp.size();
  for (int i = 0; i < HOURLY_COUNT; i++) {
    int idx = nowHour + 3 * (i + 1);
    if (hourlyLen == 0) idx = 0;
    else if (idx >= hourlyLen) idx = hourlyLen - 1;
    weather.hourly[i].hourOfDay = idx % 24;
    weather.hourly[i].tempC = hourlyLen > 0 ? (hTemp[idx] | NAN) : NAN;
    weather.hourly[i].code = hourlyLen > 0 ? (hCode[idx] | -1) : -1;
    weather.hourly[i].isDay = hourlyLen > 0 ? ((hDay[idx] | 1) != 0) : true;
  }

  JsonArray dMax = doc["daily"]["temperature_2m_max"];
  JsonArray dMin = doc["daily"]["temperature_2m_min"];
  JsonArray dCode = doc["daily"]["weather_code"];
  for (int i = 0; i < DAILY_COUNT; i++) {
    weather.daily[i].highC = dMax[i] | NAN;
    weather.daily[i].lowC = dMin[i] | NAN;
    weather.daily[i].code = dCode[i] | -1;
    weather.daily[i].weekday = (nowWeekday + i) % 7;
  }

  return true;
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("Connecting to WiFi \"%s\"", WIFI_SSID);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi connected, IP=%s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("WiFi connect timed out");
  }
}

// ---------------------------------------------------------------------------
// Round-display artwork
// ---------------------------------------------------------------------------

// Current gradient's end colors, cached so foreground text (e.g. the "updated
// N min ago" caption) can be given a background that matches the sky exactly
// instead of sitting in an ugly opaque box.
uint16_t bgTopColor = RGB565_BLACK;
uint16_t bgBottomColor = RGB565_BLACK;

uint16_t lerpColor565(uint16_t topColor, uint16_t bottomColor, float t) {
  int16_t tr = (topColor >> 11) & 0x1F, tg = (topColor >> 5) & 0x3F, tb = topColor & 0x1F;
  int16_t br = (bottomColor >> 11) & 0x1F, bg = (bottomColor >> 5) & 0x3F, bb = bottomColor & 0x1F;
  uint16_t r = tr + (int16_t)((br - tr) * t);
  uint16_t g = tg + (int16_t)((bg - tg) * t);
  uint16_t b = tb + (int16_t)((bb - tb) * t);
  return (r << 11) | (g << 5) | b;
}

void drawVerticalGradient(uint16_t topColor, uint16_t bottomColor) {
  bgTopColor = topColor;
  bgBottomColor = bottomColor;
  for (int y = 0; y < 240; y++) {
    gfx->drawFastHLine(0, y, 240, lerpColor565(topColor, bottomColor, y / 239.0f));
  }
}

void drawCloud(int16_t cx, int16_t cy, int16_t scale, uint16_t color) {
  gfx->fillCircle(cx - scale, cy, (int16_t)(scale * 0.6f), color);
  gfx->fillCircle(cx + scale, cy, (int16_t)(scale * 0.6f), color);
  gfx->fillCircle(cx, cy - (int16_t)(scale * 0.4f), (int16_t)(scale * 0.75f), color);
  gfx->fillRoundRect(cx - scale, cy, scale * 2, (int16_t)(scale * 0.6f), (int16_t)(scale * 0.3f), color);
}

void drawSun(int16_t cx, int16_t cy, int16_t r, uint16_t color) {
  gfx->fillCircle(cx, cy, r, color);
  for (int i = 0; i < 8; i++) {
    float ang = i * PI / 4.0f;
    int16_t x1 = cx + (int16_t)(cos(ang) * (r + 6));
    int16_t y1 = cy + (int16_t)(sin(ang) * (r + 6));
    int16_t x2 = cx + (int16_t)(cos(ang) * (r + 14));
    int16_t y2 = cy + (int16_t)(sin(ang) * (r + 14));
    gfx->drawLine(x1, y1, x2, y2, color);
  }
}

// Crescent moon: a filled disc with a second disc, tinted to the local sky
// color, biting a chunk out of it.
void drawMoon(int16_t cx, int16_t cy, int16_t r, uint16_t color, uint16_t biteColor) {
  gfx->fillCircle(cx, cy, r, color);
  gfx->fillCircle(cx + (int16_t)(r * 0.5f), cy - (int16_t)(r * 0.2f), (int16_t)(r * 0.85f), biteColor);
}

void drawStars(uint16_t color) {
  static const int16_t pts[][2] = {
    {40, 30}, {70, 20}, {100, 35}, {150, 25}, {198, 42}, {205, 75},
    {35, 82}, {60, 55}, {178, 62}, {20, 52}, {212, 98}, {130, 15}, {90, 45}
  };
  for (size_t i = 0; i < sizeof(pts) / sizeof(pts[0]); i++) {
    gfx->drawPixel(pts[i][0], pts[i][1], color);
  }
  gfx->fillCircle(70, 20, 1, color);
  gfx->fillCircle(198, 42, 1, color);
  gfx->fillCircle(20, 52, 1, color);
}

void drawRain(uint16_t color, int count) {
  for (int i = 0; i < count; i++) {
    int16_t x = random(15, 225);
    int16_t y = random(65, 220);
    gfx->drawLine(x, y, x - 4, y + 10, color);
  }
}

void drawSnowDots(uint16_t color, int count) {
  for (int i = 0; i < count; i++) {
    int16_t x = random(15, 225);
    int16_t y = random(65, 220);
    gfx->fillCircle(x, y, 2, color);
  }
}

void drawBackground(SkyCategory cat, bool isDay) {
  switch (cat) {
    case SKY_CLEAR:
      if (isDay) {
        drawVerticalGradient(RGB565(20, 140, 255), RGB565(120, 210, 255));
        drawSun(178, 52, 22, RGB565(255, 210, 30));
      } else {
        drawVerticalGradient(RGB565(8, 10, 38), RGB565(38, 42, 82));
        drawStars(RGB565(225, 228, 245));
        drawMoon(172, 50, 20, RGB565(235, 238, 245), bgTopColor);
      }
      break;
    case SKY_PARTLY_CLOUDY:
      if (isDay) {
        drawVerticalGradient(RGB565(30, 150, 245), RGB565(150, 210, 250));
        drawSun(168, 48, 18, RGB565(255, 210, 30));
        drawCloud(85, 68, 26, RGB565_WHITE);
        drawCloud(150, 92, 20, RGB565(225, 240, 250));
      } else {
        drawVerticalGradient(RGB565(10, 14, 42), RGB565(48, 52, 92));
        drawStars(RGB565(210, 215, 235));
        drawMoon(162, 46, 16, RGB565(230, 233, 240), bgTopColor);
        drawCloud(85, 70, 26, RGB565(75, 82, 105));
        drawCloud(150, 94, 20, RGB565(58, 65, 88));
      }
      break;
    case SKY_CLOUDY:
      drawVerticalGradient(RGB565(90, 115, 160), RGB565(170, 190, 215));
      drawCloud(80, 58, 30, RGB565(225, 232, 240));
      drawCloud(150, 48, 24, RGB565(200, 210, 225));
      drawCloud(118, 88, 34, RGB565(170, 182, 200));
      break;
    case SKY_RAIN:
      drawVerticalGradient(RGB565(20, 60, 140), RGB565(70, 110, 180));
      drawCloud(100, 55, 34, RGB565(210, 220, 235));
      drawCloud(150, 45, 24, RGB565(185, 200, 220));
      drawRain(RGB565(120, 210, 255), 26);
      break;
    case SKY_STORM:
      drawVerticalGradient(RGB565(35, 20, 90), RGB565(80, 55, 140));
      drawCloud(110, 55, 36, RGB565(60, 50, 90));
      drawRain(RGB565(150, 190, 255), 18);
      gfx->fillTriangle(120, 88, 106, 128, 118, 126, RGB565(255, 225, 40));
      gfx->fillTriangle(118, 126, 130, 126, 108, 168, RGB565(255, 225, 40));
      break;
    case SKY_SNOW:
      drawVerticalGradient(RGB565(120, 190, 235), RGB565(210, 235, 250));
      drawCloud(110, 55, 32, RGB565(225, 235, 245));
      drawSnowDots(RGB565_WHITE, 30);
      break;
    case SKY_FOG:
      drawVerticalGradient(RGB565(140, 155, 190), RGB565(205, 215, 230));
      for (int i = 0; i < 5; i++) {
        gfx->fillRoundRect(20, 60 + i * 20, 200, 8, 4, RGB565(220, 225, 235));
      }
      break;
    default:
      drawVerticalGradient(RGB565(196, 210, 226), RGB565(224, 233, 244));
      break;
  }
}

// Accent color used for the bezel ring and highlights, matched to each condition.
uint16_t categoryAccent(SkyCategory cat) {
  switch (cat) {
    case SKY_CLEAR:          return RGB565(255, 200, 20);
    case SKY_PARTLY_CLOUDY:  return RGB565(255, 210, 60);
    case SKY_CLOUDY:         return RGB565(200, 215, 235);
    case SKY_RAIN:           return RGB565(80, 200, 255);
    case SKY_STORM:          return RGB565(255, 210, 60);
    case SKY_SNOW:           return RGB565_WHITE;
    case SKY_FOG:            return RGB565(225, 230, 240);
    default:                 return RGB565(255, 110, 60);
  }
}

// Solid, single-tone color for each condition's small forecast-tile glyph
// (categoryAccent's bezel colors are too close to white/CARD_BG to read at
// icon size, so forecast tiles get their own darker palette).
uint16_t forecastIconColor(SkyCategory cat) {
  switch (cat) {
    case SKY_CLEAR:          return RGB565(230, 160, 20);
    case SKY_PARTLY_CLOUDY:  return RGB565(130, 155, 185);
    case SKY_CLOUDY:         return RGB565(140, 155, 175);
    case SKY_RAIN:           return RGB565(20, 130, 220);
    case SKY_STORM:          return RGB565(120, 95, 210);
    case SKY_SNOW:           return RGB565(90, 170, 215);
    case SKY_FOG:            return RGB565(150, 160, 175);
    default:                 return INK_DIM;
  }
}

// Compact ~20x24px condition glyph for the hourly/5-day forecast tiles,
// where there isn't room for the full drawBackground() artwork.
void drawForecastIcon(SkyCategory cat, bool isDay, int16_t cx, int16_t cy, uint16_t bg) {
  uint16_t color = forecastIconColor(cat);
  switch (cat) {
    case SKY_CLEAR:
      if (isDay) {
        gfx->fillCircle(cx, cy, 6, color);
        for (int i = 0; i < 8; i++) {
          float ang = i * PI / 4.0f;
          int16_t x1 = cx + (int16_t)(cos(ang) * 9), y1 = cy + (int16_t)(sin(ang) * 9);
          int16_t x2 = cx + (int16_t)(cos(ang) * 13), y2 = cy + (int16_t)(sin(ang) * 13);
          gfx->drawLine(x1, y1, x2, y2, color);
        }
      } else {
        gfx->fillCircle(cx - 2, cy, 7, color);
        gfx->fillCircle(cx + 3, cy - 3, 6, bg);
      }
      break;
    case SKY_PARTLY_CLOUDY:
      if (isDay) gfx->fillCircle(cx + 5, cy - 6, 5, RGB565(230, 170, 30));
      gfx->fillCircle(cx - 5, cy + 2, 6, color);
      gfx->fillCircle(cx + 2, cy + 2, 7, color);
      gfx->fillRoundRect(cx - 8, cy + 2, 16, 6, 3, color);
      break;
    case SKY_CLOUDY:
      gfx->fillCircle(cx - 6, cy, 6, color);
      gfx->fillCircle(cx + 3, cy - 2, 8, color);
      gfx->fillRoundRect(cx - 9, cy, 20, 7, 3, color);
      break;
    case SKY_RAIN:
      gfx->fillCircle(cx - 5, cy - 3, 6, color);
      gfx->fillCircle(cx + 3, cy - 5, 7, color);
      gfx->fillRoundRect(cx - 8, cy - 3, 18, 6, 3, color);
      gfx->drawLine(cx - 4, cy + 6, cx - 6, cy + 12, color);
      gfx->drawLine(cx + 1, cy + 6, cx - 1, cy + 12, color);
      gfx->drawLine(cx + 6, cy + 6, cx + 4, cy + 12, color);
      break;
    case SKY_STORM:
      gfx->fillCircle(cx - 3, cy - 4, 7, color);
      gfx->fillRoundRect(cx - 8, cy - 4, 18, 6, 3, color);
      gfx->fillTriangle(cx + 2, cy + 3, cx - 4, cy + 11, cx + 1, cy + 9, RGB565(255, 200, 20));
      gfx->fillTriangle(cx + 1, cy + 9, cx + 5, cy + 9, cx - 1, cy + 16, RGB565(255, 200, 20));
      break;
    case SKY_SNOW:
      gfx->fillCircle(cx - 5, cy - 3, 6, color);
      gfx->fillCircle(cx + 3, cy - 5, 7, color);
      gfx->fillRoundRect(cx - 8, cy - 3, 18, 6, 3, color);
      gfx->fillCircle(cx - 5, cy + 11, 2, color);
      gfx->fillCircle(cx + 1, cy + 9, 2, color);
      gfx->fillCircle(cx + 6, cy + 11, 2, color);
      break;
    case SKY_FOG:
      for (int i = 0; i < 3; i++) gfx->fillRoundRect(cx - 9, cy - 6 + i * 6, 18, 3, 1, color);
      break;
    default:
      gfx->drawCircle(cx, cy, 8, color);
      break;
  }
}

// ---- Small vector glyphs for the stat tiles ----

uint16_t statColor(StatKind k) {
  switch (k) {
    case STAT_HUMIDITY: return RGB565(10, 150, 160);
    case STAT_WIND:      return RGB565(205, 120, 20);
    case STAT_RAIN:      return RGB565(20, 130, 220);
    case STAT_FEEL:      return RGB565(215, 90, 90);
    case STAT_HILO:      return RGB565(120, 95, 210);
    case STAT_PRESSURE:  return RGB565(70, 100, 175);
    case STAT_UV:        return RGB565(195, 140, 10);
    default:             return INK;
  }
}

const char* statCaption(StatKind k) {
  switch (k) {
    case STAT_HUMIDITY: return "H";
    case STAT_WIND:      return "W";
    case STAT_RAIN:      return "R";
    case STAT_FEEL:      return "F";
    case STAT_HILO:      return "H/L";
    case STAT_PRESSURE:  return "P";
    case STAT_UV:        return "UV";
    default:             return "";
  }
}

// Draws a line twice, offset by one pixel perpendicular to its direction,
// to fake a ~2px stroke (Arduino_GFX's drawLine is always hairline-thin,
// which reads poorly at the tiny sizes these stat icons render at).
void drawThickLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
  gfx->drawLine(x0, y0, x1, y1, color);
  if (abs(x1 - x0) > abs(y1 - y0)) {
    gfx->drawLine(x0, y0 + 1, x1, y1 + 1, color);
  } else {
    gfx->drawLine(x0 + 1, y0, x1 + 1, y1, color);
  }
}

void drawStatIcon(StatKind k, int16_t cx, int16_t cy, uint16_t color, uint16_t bg) {
  switch (k) {
    case STAT_HUMIDITY:
      gfx->fillTriangle(cx, cy - 10, cx - 7, cy + 3, cx + 7, cy + 3, color);
      gfx->fillCircle(cx, cy + 3, 7, color);
      break;
    case STAT_WIND:
      gfx->fillRoundRect(cx - 10, cy - 6, 16, 3, 1, color);
      gfx->fillRoundRect(cx - 11, cy - 1, 20, 3, 1, color);
      gfx->fillRoundRect(cx - 9, cy + 4, 13, 3, 1, color);
      break;
    case STAT_RAIN:
      gfx->fillCircle(cx, cy - 4, 9, color);
      gfx->fillRect(cx - 10, cy - 4, 20, 9, bg);
      gfx->fillRoundRect(cx - 10, cy - 5, 20, 3, 1, color);
      gfx->fillRoundRect(cx - 2, cy - 5, 4, 11, 2, color);
      gfx->drawLine(cx, cy + 8, cx - 4, cy + 11, color);
      gfx->drawLine(cx + 1, cy + 8, cx - 3, cy + 11, color);
      break;
    case STAT_FEEL:
      gfx->fillRoundRect(cx - 3, cy - 9, 6, 15, 3, color);
      gfx->fillCircle(cx, cy + 6, 6, color);
      break;
    case STAT_HILO:
      gfx->fillTriangle(cx - 7, cy, cx, cy - 9, cx + 7, cy, color);
      gfx->fillTriangle(cx - 7, cy + 1, cx, cy + 10, cx + 7, cy + 1, color);
      break;
    case STAT_PRESSURE:
      gfx->drawCircle(cx, cy, 9, color);
      gfx->drawCircle(cx, cy, 8, color);
      drawThickLine(cx, cy, cx + 5, cy - 5, color);
      gfx->fillCircle(cx, cy, 2, color);
      break;
    case STAT_UV:
      gfx->fillCircle(cx, cy, 5, color);
      for (int i = 0; i < 8; i++) {
        float ang = i * PI / 4.0f;
        int16_t x1 = cx + (int16_t)(cosf(ang) * 8), y1 = cy + (int16_t)(sinf(ang) * 8);
        int16_t x2 = cx + (int16_t)(cosf(ang) * 12), y2 = cy + (int16_t)(sinf(ang) * 12);
        drawThickLine(x1, y1, x2, y2, color);
      }
      break;
  }
}

String statValueText(StatKind k, const WeatherData& w) {
  char buf[16];
  switch (k) {
    case STAT_HUMIDITY:
      if (isnan(w.humidityPct)) return "--";
      snprintf(buf, sizeof(buf), "%.0f%%", w.humidityPct);
      return String(buf);
    case STAT_WIND:
      if (isnan(w.windKph)) return "--";
      snprintf(buf, sizeof(buf), "%.0f", w.windKph);
      return String(buf);
    case STAT_RAIN:
      if (isnan(w.rainChancePct)) return "--";
      snprintf(buf, sizeof(buf), "%.0f%%", w.rainChancePct);
      return String(buf);
    case STAT_FEEL: {
      if (isnan(w.feelsLikeC)) return "--";
      float v = useFahrenheit ? (w.feelsLikeC * 9.0f / 5.0f + 32.0f) : w.feelsLikeC;
      snprintf(buf, sizeof(buf), "%.0f", v);
      return String(buf);
    }
    case STAT_HILO: {
      if (isnan(w.highC) || isnan(w.lowC)) return "--";
      float hi = useFahrenheit ? (w.highC * 9.0f / 5.0f + 32.0f) : w.highC;
      float lo = useFahrenheit ? (w.lowC * 9.0f / 5.0f + 32.0f) : w.lowC;
      snprintf(buf, sizeof(buf), "%.0f/%.0f", hi, lo);
      return String(buf);
    }
    case STAT_PRESSURE:
      if (isnan(w.pressureHpa)) return "--";
      snprintf(buf, sizeof(buf), "%.0f", w.pressureHpa);
      return String(buf);
    case STAT_UV:
      if (isnan(w.uvIndex)) return "--";
      snprintf(buf, sizeof(buf), "%.1f", w.uvIndex);
      return String(buf);
    default:
      return "";
  }
}

void renderWeather(const WeatherData& weather, bool online) {
  CodeInfo info = online ? wmoInfo(weather.weatherCode) : CodeInfo{ SKY_UNKNOWN, "--" };
  bool isDay = online ? weather.isDay : true;
  drawBackground(info.cat, isDay);
  uint16_t ringAccent = categoryAccent(info.cat);

  // Bezel ring to frame the round face, tinted to match the current condition
  gfx->drawCircle(120, 120, 118, RGB565_BLACK);
  gfx->drawCircle(120, 120, 117, ringAccent);

  const uint16_t chip = CARD_BG;

  // Top status chip: ZIP code rather than the resolved place name, since it's
  // always short enough to fit within the round display's visible width.
  gfx->fillRoundRect(44, 18, 152, 27, 13, chip);
  gfx->setTextColor(online ? INK : OFFLINE_RED, chip);
  String statusText = online ? zipCode : String("OFFLINE");
  statusText.toUpperCase();
  centerText(statusText.c_str(), 120, 32, 1, &FreeSansBold9pt7b);

  // Center card: condition + big temperature + stat tiles
  const int16_t cardX = 32, cardY = 50, cardW = 176, cardH = 146;
  gfx->fillRoundRect(cardX, cardY, cardW, cardH, 22, chip);

  String condText = weather.condition.length() ? weather.condition : String(online ? "No data" : "--");
  gfx->setTextColor(INK_DIM, chip);
  centerText(condText.c_str(), 120, cardY + 16, 1, &FreeSans9pt7b);

  char tempText[16];
  if (isnan(weather.temperatureC)) {
    snprintf(tempText, sizeof(tempText), useFahrenheit ? "--F" : "--C");
  } else if (useFahrenheit) {
    float f = weather.temperatureC * 9.0f / 5.0f + 32.0f;
    snprintf(tempText, sizeof(tempText), "%.0f\xF8", f); // 0xF8 is the degree glyph in this font
  } else {
    snprintf(tempText, sizeof(tempText), "%.0f\xF8", weather.temperatureC); // 0xF8 is the degree glyph in this font
  }
  gfx->setTextColor(ACCENT_BLUE, chip);
  centerText(tempText, 120, cardY + 60, 1, &FreeSansBold24pt7b);

  gfx->drawFastHLine(cardX + 16, cardY + 86, cardW - 32, DIVIDER);

  // Up to MAX_DISPLAY_STATS tiles, evenly spread, in a fixed priority order.
  StatKind active[MAX_DISPLAY_STATS];
  int activeCount = 0;
  for (int i = 0; i < STAT_ORDER_COUNT && activeCount < MAX_DISPLAY_STATS; i++) {
    if (statEnabled(STAT_ORDER[i])) active[activeCount++] = STAT_ORDER[i];
  }

  if (activeCount > 0) {
    const int16_t rowLeft = cardX + 24, rowWidth = cardW - 48;
    const int16_t iconY = cardY + 100, valueY = cardY + 119, capY = cardY + 136;
    int colw = rowWidth / activeCount;
    for (int i = 0; i < activeCount; i++) {
      int16_t cx = rowLeft + colw * i + colw / 2;
      uint16_t color = statColor(active[i]);
      drawStatIcon(active[i], cx, iconY, color, chip);
      gfx->setTextColor(color, chip);
      centerText(statValueText(active[i], weather).c_str(), cx, valueY, 1, &FreeSansBold9pt7b);
      gfx->setTextColor(INK_DIM, chip);
      centerText(statCaption(active[i]), cx, capY, 1, &FreeSans9pt7b);
    }
  }

  // "Updated N min ago" sits directly on the sky, tinted to match it exactly.
  if (online && lastFetchMs > 0) {
    unsigned long agoMin = (millis() - lastFetchMs) / 60000UL;
    char upd[24];
    snprintf(upd, sizeof(upd), agoMin < 1 ? "Updated just now" : "Updated %lum ago", agoMin);
    uint16_t skyAtY = lerpColor565(bgTopColor, bgBottomColor, 213 / 239.0f);
    gfx->setTextColor(RGB565_DARKGREY, skyAtY);
    centerText(upd, 120, 213, 1, &FreeSans9pt7b);
  }
}

// Splits text into lines that fit within maxWidth at the given text size.
int wrapText(const String& text, uint8_t size, int16_t maxWidth, String outLines[], int maxLines) {
  gfx->setTextSize(size);
  int count = 0;
  int start = 0;
  int len = text.length();

  while (start < len && count < maxLines) {
    int lastGoodEnd = -1;
    int end = start;
    while (end <= len) {
      // advance to next word boundary
      int next = text.indexOf(' ', end);
      if (next < 0) next = len;
      String candidate = text.substring(start, next);
      int16_t x1, y1;
      uint16_t w, h;
      gfx->getTextBounds(candidate.c_str(), 0, 0, &x1, &y1, &w, &h);
      if ((int16_t)w <= maxWidth) {
        lastGoodEnd = next;
        end = next + 1;
        if (next >= len) break;
      } else {
        if (lastGoodEnd < 0) lastGoodEnd = next; // single word longer than line; take it anyway
        break;
      }
    }
    if (lastGoodEnd < 0) lastGoodEnd = len;
    outLines[count++] = text.substring(start, lastGoodEnd);
    start = lastGoodEnd + 1;
  }
  return count;
}

void renderNote(const String& note, int index, int total) {
  drawVerticalGradient(RGB565(214, 231, 245), RGB565(236, 244, 250));

  gfx->drawCircle(120, 120, 118, RGB565_BLACK);
  gfx->drawCircle(120, 120, 117, RGB565(150, 175, 200));

  gfx->setTextColor(ACCENT_BLUE, bgTopColor);
  centerText("NOTE", 120, 38, 1, &FreeSansBold9pt7b);

  static const int MAX_LINES = 6;
  String lines[MAX_LINES];
  gfx->setFont(&FreeSans9pt7b);
  int lineCount = wrapText(note, 1, 170, lines, MAX_LINES);

  int lineHeight = 22;
  int totalHeight = lineCount * lineHeight;
  int startY = 120 - totalHeight / 2 + lineHeight / 2;

  gfx->setTextColor(INK, lerpColor565(bgTopColor, bgBottomColor, 0.4f));
  for (int i = 0; i < lineCount; i++) {
    centerText(lines[i].c_str(), 120, startY + i * lineHeight, 1, &FreeSans9pt7b);
  }

  // Position dots, one per note, to show which one is showing
  if (total > 1) {
    int dotSpacing = 14;
    int totalWidth = (total - 1) * dotSpacing;
    int startX = 120 - totalWidth / 2;
    for (int i = 0; i < total; i++) {
      uint16_t color = (i == index) ? ACCENT_BLUE : RGB565(190, 205, 220);
      gfx->fillCircle(startX + i * dotSpacing, 200, 3, color);
    }
  }
}

// Shared chip + bezel + card frame used by the hourly/5-day screens, mirroring
// renderWeather()'s layout so all screens read as one consistent set.
void drawForecastFrame(const char* title, int16_t cardX, int16_t cardY, int16_t cardW, int16_t cardH) {
  drawVerticalGradient(RGB565(214, 231, 245), RGB565(236, 244, 250));
  gfx->drawCircle(120, 120, 118, RGB565_BLACK);
  gfx->drawCircle(120, 120, 117, RGB565(150, 175, 200));

  gfx->fillRoundRect(44, 18, 152, 27, 13, CARD_BG);
  gfx->setTextColor(INK, CARD_BG);
  centerText(title, 120, 32, 1, &FreeSansBold9pt7b);

  gfx->fillRoundRect(cardX, cardY, cardW, cardH, 22, CARD_BG);
}

void renderHourly(const WeatherData& weather, bool online) {
  const int16_t cardX = 24, cardY = 52, cardW = 192, cardH = 140;
  drawForecastFrame("HOURLY", cardX, cardY, cardW, cardH);
  const uint16_t chip = CARD_BG;

  if (!online) {
    gfx->setTextColor(OFFLINE_RED, chip);
    centerText("Offline", 120, cardY + cardH / 2, 1, &FreeSans9pt7b);
    return;
  }

  int colw = cardW / HOURLY_COUNT;
  const int16_t labelY = cardY + 22, iconY = cardY + 60, tempY = cardY + 100;
  for (int i = 0; i < HOURLY_COUNT; i++) {
    const WeatherData::HourPoint& hp = weather.hourly[i];
    int16_t cx = cardX + colw * i + colw / 2;
    CodeInfo info = wmoInfo(hp.code);

    int h12 = hp.hourOfDay % 12;
    if (h12 == 0) h12 = 12;
    char label[8];
    snprintf(label, sizeof(label), "%d%s", h12, hp.hourOfDay < 12 ? "a" : "p");
    gfx->setTextColor(INK_DIM, chip);
    centerText(label, cx, labelY, 1, &FreeSans9pt7b);

    drawForecastIcon(info.cat, hp.isDay, cx, iconY, chip);

    char tempTxt[8];
    if (isnan(hp.tempC)) {
      snprintf(tempTxt, sizeof(tempTxt), "--");
    } else {
      float show = useFahrenheit ? (hp.tempC * 9.0f / 5.0f + 32.0f) : hp.tempC;
      snprintf(tempTxt, sizeof(tempTxt), "%.0f\xF8", show);
    }
    gfx->setTextColor(INK, chip);
    centerText(tempTxt, cx, tempY, 1, &FreeSansBold9pt7b);

    if (i > 0) gfx->drawFastVLine(cardX + colw * i, cardY + 14, cardH - 28, DIVIDER);
  }
}

void renderDaily(const WeatherData& weather, bool online) {
  const int16_t cardX = 30, cardY = 50, cardW = 180, cardH = 146;
  drawForecastFrame("5-DAY", cardX, cardY, cardW, cardH);
  const uint16_t chip = CARD_BG;

  if (!online) {
    gfx->setTextColor(OFFLINE_RED, chip);
    centerText("Offline", 120, cardY + cardH / 2, 1, &FreeSans9pt7b);
    return;
  }

  static const char* WD[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
  int rowH = cardH / DAILY_COUNT;
  for (int i = 0; i < DAILY_COUNT; i++) {
    const WeatherData::DayPoint& dp = weather.daily[i];
    int16_t rowY = cardY + rowH * i + rowH / 2;
    CodeInfo info = wmoInfo(dp.code);

    const char* label = (i == 0) ? "Today" : WD[dp.weekday];
    gfx->setTextColor(INK, chip);
    gfx->setFont(&FreeSans9pt7b);
    gfx->setTextSize(1);
    gfx->setCursor(cardX + 14, rowY + 4);
    gfx->print(label);

    drawForecastIcon(info.cat, true, cardX + 74, rowY, chip);

    char hiLo[16];
    if (isnan(dp.highC) || isnan(dp.lowC)) {
      snprintf(hiLo, sizeof(hiLo), "--/--");
    } else {
      float hi = useFahrenheit ? (dp.highC * 9.0f / 5.0f + 32.0f) : dp.highC;
      float lo = useFahrenheit ? (dp.lowC * 9.0f / 5.0f + 32.0f) : dp.lowC;
      snprintf(hiLo, sizeof(hiLo), "%.0f\xF8/%.0f\xF8", hi, lo);
    }
    gfx->setFont(&FreeSansBold9pt7b);
    int16_t x1, y1;
    uint16_t w, h;
    gfx->getTextBounds(hiLo, 0, 0, &x1, &y1, &w, &h);
    gfx->setTextColor(INK, chip);
    gfx->setCursor(cardX + cardW - 14 - w, rowY + 4);
    gfx->print(hiLo);

    if (i > 0) gfx->drawFastHLine(cardX + 14, cardY + rowH * i, cardW - 28, DIVIDER);
  }
}

// Hyper-local comparison screen: what the AHT20+BMP280 node right outside is
// reading, alongside the outdoor API's current temperature for reference.
void renderLocal(const LocalSensorReading& local, bool sensorOnline, const WeatherData& outdoor, bool outdoorOnline) {
  const int16_t cardX = 28, cardY = 44, cardW = 184, cardH = 160;
  drawForecastFrame("LOCAL", cardX, cardY, cardW, cardH);
  const uint16_t chip = CARD_BG;

  if (!sensorOnline) {
    gfx->setTextColor(OFFLINE_RED, chip);
    centerText("Sensor offline", 120, cardY + cardH / 2 - 10, 1, &FreeSans9pt7b);
    gfx->setTextColor(INK_DIM, chip);
    centerText(localSensorHost.c_str(), 120, cardY + cardH / 2 + 12, 1, &FreeSans9pt7b);
    return;
  }

  char tempText[16];
  if (isnan(local.tempC)) {
    snprintf(tempText, sizeof(tempText), useFahrenheit ? "--F" : "--C");
  } else {
    float v = useFahrenheit ? (local.tempC * 9.0f / 5.0f + 32.0f) : local.tempC;
    snprintf(tempText, sizeof(tempText), "%.0f\xF8", v);
  }
  gfx->setTextColor(ACCENT_BLUE, chip);
  centerText(tempText, 120, cardY + 34, 1, &FreeSansBold24pt7b);

  gfx->setTextColor(INK_DIM, chip);
  if (outdoorOnline && !isnan(outdoor.temperatureC)) {
    float ov = useFahrenheit ? (outdoor.temperatureC * 9.0f / 5.0f + 32.0f) : outdoor.temperatureC;
    char cmp[24];
    snprintf(cmp, sizeof(cmp), "Outdoor %.0f\xF8", ov);
    centerText(cmp, 120, cardY + 56, 1, &FreeSans9pt7b);
  } else {
    centerText("Outdoor --", 120, cardY + 56, 1, &FreeSans9pt7b);
  }

  gfx->drawFastHLine(cardX + 16, cardY + 70, cardW - 32, DIVIDER);

  const int16_t iconY = cardY + 90, valueY = cardY + 108, capY = cardY + 124;
  int16_t cx1 = 120 - 44, cx2 = 120 + 44;

  uint16_t humColor = statColor(STAT_HUMIDITY);
  drawStatIcon(STAT_HUMIDITY, cx1, iconY, humColor, chip);
  char humText[8];
  if (isnan(local.humidityPct)) snprintf(humText, sizeof(humText), "--");
  else snprintf(humText, sizeof(humText), "%.0f%%", local.humidityPct);
  gfx->setTextColor(humColor, chip);
  centerText(humText, cx1, valueY, 1, &FreeSansBold9pt7b);
  gfx->setTextColor(INK_DIM, chip);
  centerText("Humidity", cx1, capY, 1, &FreeSans9pt7b);

  uint16_t presColor = statColor(STAT_PRESSURE);
  drawStatIcon(STAT_PRESSURE, cx2, iconY, presColor, chip);
  char presText[8];
  if (isnan(local.pressureHpa)) snprintf(presText, sizeof(presText), "--");
  else snprintf(presText, sizeof(presText), "%.0f", local.pressureHpa);
  gfx->setTextColor(presColor, chip);
  centerText(presText, cx2, valueY, 1, &FreeSansBold9pt7b);
  gfx->setTextColor(INK_DIM, chip);
  centerText("hPa", cx2, capY, 1, &FreeSans9pt7b);

  gfx->drawFastHLine(cardX + 16, cardY + 138, cardW - 32, DIVIDER);

  // Rain likelihood from the Supabase pressure-trend forecast (relayed by
  // the sensor node's /data); only shown once enough telemetry history
  // exists for the RPC to compute a 3h trend.
  if (local.haveForecast && local.rainProbabilityPct >= 0) {
    char rainText[24];
    snprintf(rainText, sizeof(rainText), "Rain chance: %d%%", local.rainProbabilityPct);
    gfx->setTextColor(statColor(STAT_RAIN), chip);
    centerText(rainText, 120, cardY + 152, 1, &FreeSansBold9pt7b);
  } else {
    gfx->setTextColor(INK_DIM, chip);
    centerText("Rain: building history", 120, cardY + 152, 1, &FreeSans9pt7b);
  }

  if (lastLocalFetchMs > 0) {
    unsigned long agoMin = (millis() - lastLocalFetchMs) / 60000UL;
    char upd[24];
    snprintf(upd, sizeof(upd), agoMin < 1 ? "Updated just now" : "Updated %lum ago", agoMin);
    uint16_t skyAtY = lerpColor565(bgTopColor, bgBottomColor, 213 / 239.0f);
    gfx->setTextColor(RGB565_DARKGREY, skyAtY);
    centerText(upd, 120, 213, 1, &FreeSans9pt7b);
  }
}

// AQI color bands, matching the standard EPA/AirNow PM2.5 AQI palette
// (Good/Moderate/USG/Unhealthy/Very Unhealthy/Hazardous) that most consumer
// air quality displays use.
uint16_t aqiColor(int aqi) {
  if (aqi < 0) return INK_DIM;
  if (aqi <= 50) return RGB565(56, 168, 76);
  if (aqi <= 100) return RGB565(220, 190, 40);
  if (aqi <= 150) return RGB565(230, 140, 40);
  if (aqi <= 200) return RGB565(210, 60, 55);
  if (aqi <= 300) return RGB565(140, 60, 150);
  return RGB565(120, 30, 40);
}

// PMS5003 particulate matter screen: PM2.5/PM10 (atmospheric concentration)
// and the derived US EPA PM2.5 AQI, relayed by the sensor node's /data.
void renderAirQuality(const AirQualityReading& air, bool sensorOnline) {
  const int16_t cardX = 28, cardY = 44, cardW = 184, cardH = 160;
  drawForecastFrame("AIR", cardX, cardY, cardW, cardH);
  const uint16_t chip = CARD_BG;

  if (!sensorOnline) {
    gfx->setTextColor(OFFLINE_RED, chip);
    centerText("Sensor offline", 120, cardY + cardH / 2 - 10, 1, &FreeSans9pt7b);
    gfx->setTextColor(INK_DIM, chip);
    centerText(airQualityHost.c_str(), 120, cardY + cardH / 2 + 12, 1, &FreeSans9pt7b);
    return;
  }

  char aqiText[8];
  if (air.aqi < 0) snprintf(aqiText, sizeof(aqiText), "--");
  else snprintf(aqiText, sizeof(aqiText), "%d", air.aqi);
  gfx->setTextColor(aqiColor(air.aqi), chip);
  centerText(aqiText, 120, cardY + 34, 1, &FreeSansBold24pt7b);

  gfx->setTextColor(INK_DIM, chip);
  centerText(air.aqiCategory.length() ? air.aqiCategory.c_str() : "AQI", 120, cardY + 56, 1, &FreeSans9pt7b);

  gfx->drawFastHLine(cardX + 16, cardY + 70, cardW - 32, DIVIDER);

  const int16_t valueY = cardY + 108, capY = cardY + 124;
  int16_t cx1 = 120 - 44, cx2 = 120 + 44;

  char pm25Text[8];
  snprintf(pm25Text, sizeof(pm25Text), "%d", air.pm2_5);
  gfx->setTextColor(INK, chip);
  centerText(pm25Text, cx1, valueY, 1, &FreeSansBold9pt7b);
  gfx->setTextColor(INK_DIM, chip);
  centerText("PM2.5", cx1, capY, 1, &FreeSans9pt7b);

  char pm10Text[8];
  snprintf(pm10Text, sizeof(pm10Text), "%d", air.pm10);
  gfx->setTextColor(INK, chip);
  centerText(pm10Text, cx2, valueY, 1, &FreeSansBold9pt7b);
  gfx->setTextColor(INK_DIM, chip);
  centerText("PM10", cx2, capY, 1, &FreeSans9pt7b);

  gfx->drawFastHLine(cardX + 16, cardY + 138, cardW - 32, DIVIDER);

  char pm1Text[24];
  snprintf(pm1Text, sizeof(pm1Text), "PM1.0: %d ug/m3", air.pm1_0);
  gfx->setTextColor(INK_DIM, chip);
  centerText(pm1Text, 120, cardY + 152, 1, &FreeSans9pt7b);

  if (lastAirFetchMs > 0) {
    unsigned long agoMin = (millis() - lastAirFetchMs) / 60000UL;
    char upd[24];
    snprintf(upd, sizeof(upd), agoMin < 1 ? "Updated just now" : "Updated %lum ago", agoMin);
    uint16_t skyAtY = lerpColor565(bgTopColor, bgBottomColor, 213 / 239.0f);
    gfx->setTextColor(RGB565_DARKGREY, skyAtY);
    centerText(upd, 120, 213, 1, &FreeSans9pt7b);
  }
}

void renderCurrentScreen() {
  if (currentScreen == 0) {
    renderWeather(currentWeather, haveData);
  } else if (currentScreen == 1) {
    renderHourly(currentWeather, haveData);
  } else if (currentScreen == 2) {
    renderDaily(currentWeather, haveData);
  } else if (currentScreen == 3) {
    renderLocal(localSensor, haveLocalSensor && localSensor.online, currentWeather, haveData);
  } else if (currentScreen == 4) {
    renderAirQuality(airQuality, haveAirQuality && airQuality.online);
  } else if (notes.empty()) {
    renderWeather(currentWeather, haveData);
  } else {
    int idx = currentScreen - NUM_FIXED_SCREENS;
    if (idx < 0 || idx >= (int)notes.size()) idx = 0;
    renderNote(notes[idx], idx, notes.size());
  }
}

// ---------------------------------------------------------------------------
// Web UI: one static page (web_page.h) backed by a small JSON API.
// ---------------------------------------------------------------------------

String hhmm(uint8_t h, uint8_t m) {
  char buf[6];
  snprintf(buf, sizeof(buf), "%02u:%02u", h, m);
  return String(buf);
}

void buildStatusJson(JsonDocument& doc) {
  doc["online"] = (WiFi.status() == WL_CONNECTED) && haveData;
  doc["zip"] = zipCode;
  doc["latlon"] = latLon;
  if (placeName.length()) doc["place"] = placeName;
  doc["fahrenheit"] = useFahrenheit;
  doc["cycleSeconds"] = screenCycleMs / 1000UL;
  doc["currentScreen"] = currentScreen;
  doc["localHost"] = localSensorHost;
  doc["airHost"] = airQualityHost;

  JsonArray screens = doc["screens"].to<JsonArray>();
  screens.add("Weather");
  screens.add("Hourly");
  screens.add("5-Day");
  screens.add("Local");
  screens.add("Air Quality");
  for (size_t i = 0; i < notes.size(); i++) screens.add(notes[i]);

  doc["updatedSecondsAgo"] = lastFetchMs ? (millis() - lastFetchMs) / 1000UL : 0;

  JsonObject w = doc["weather"].to<JsonObject>();
  w["code"] = currentWeather.weatherCode;
  w["isDay"] = currentWeather.isDay;
  w["condition"] = currentWeather.condition;
  w["tempC"] = currentWeather.temperatureC;
  w["feelsLikeC"] = currentWeather.feelsLikeC;
  w["humidity"] = currentWeather.humidityPct;
  w["windKph"] = currentWeather.windKph;
  w["windGustKph"] = currentWeather.windGustKph;
  w["pressureHpa"] = currentWeather.pressureHpa;
  w["rainChancePct"] = currentWeather.rainChancePct;
  w["uvIndex"] = currentWeather.uvIndex;
  w["highC"] = currentWeather.highC;
  w["lowC"] = currentWeather.lowC;

  JsonObject local = doc["local"].to<JsonObject>();
  local["online"] = haveLocalSensor && localSensor.online;
  local["tempC"] = localSensor.tempC;
  local["humidityPct"] = localSensor.humidityPct;
  local["pressureHpa"] = localSensor.pressureHpa;
  local["updatedSecondsAgo"] = lastLocalFetchMs ? (millis() - lastLocalFetchMs) / 1000UL : 0;
  if (localSensor.haveForecast) {
    JsonObject forecast = local["forecast"].to<JsonObject>();
    forecast["state"] = localSensor.forecastState;
    forecast["rainProbabilityPct"] = localSensor.rainProbabilityPct;
    forecast["pressureTrend"] = localSensor.pressureTrend;
  }

  JsonObject air = doc["air"].to<JsonObject>();
  air["online"] = haveAirQuality && airQuality.online;
  air["pm1_0"] = airQuality.pm1_0;
  air["pm2_5"] = airQuality.pm2_5;
  air["pm10"] = airQuality.pm10;
  air["aqi"] = airQuality.aqi;
  air["aqiCategory"] = airQuality.aqiCategory;
  air["updatedSecondsAgo"] = lastAirFetchMs ? (millis() - lastAirFetchMs) / 1000UL : 0;

  JsonObject show = doc["show"].to<JsonObject>();
  show["humidity"] = displaySettings.humidity;
  show["wind"] = displaySettings.wind;
  show["rainChance"] = displaySettings.rainChance;
  show["feelsLike"] = displaySettings.feelsLike;
  show["highLow"] = displaySettings.highLow;
  show["pressure"] = displaySettings.pressure;
  show["uv"] = displaySettings.uv;

  JsonObject power = doc["power"].to<JsonObject>();
  power["on"] = displayOn;
  power["brightness"] = displayBrightnessPct;
  JsonObject sched = power["schedule"].to<JsonObject>();
  sched["enabled"] = powerSchedule.enabled;
  sched["weekdayOn"] = hhmm(powerSchedule.weekdayOnHour, powerSchedule.weekdayOnMinute);
  sched["weekdayOff"] = hhmm(powerSchedule.weekdayOffHour, powerSchedule.weekdayOffMinute);
  sched["weekendOn"] = hhmm(powerSchedule.weekendOnHour, powerSchedule.weekendOnMinute);
  sched["weekendOff"] = hhmm(powerSchedule.weekendOffHour, powerSchedule.weekendOffMinute);
}

void sendStatusJson(int code = 200) {
  JsonDocument doc;
  buildStatusJson(doc);
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

void handleRoot() {
  server.send_P(200, "text/html", PAGE_HTML);
}

void handleApiStatus() {
  sendStatusJson();
}

void handleApiRefresh() {
  if (!latLon.isEmpty()) {
    haveData = fetchWeather(latLon, currentWeather);
    lastFetchMs = millis();
  }
  haveLocalSensor = fetchLocalSensor(localSensorHost, localSensor);
  lastLocalFetchMs = millis();
  haveAirQuality = fetchAirQuality(airQualityHost, airQuality);
  lastAirFetchMs = millis();
  if (currentScreen == 0 || currentScreen == 3 || currentScreen == 4) renderCurrentScreen();
  sendStatusJson();
}

// Parses a "HH:MM" string into hour/minute, ignoring anything malformed.
void parseHHMM(JsonVariant v, uint8_t& outHour, uint8_t& outMinute) {
  if (v.isNull()) return;
  String s = v.as<String>();
  int colon = s.indexOf(':');
  if (colon < 1 || colon >= (int)s.length() - 1) return;
  int h = s.substring(0, colon).toInt();
  int m = s.substring(colon + 1).toInt();
  if (h < 0 || h > 23 || m < 0 || m > 59) return;
  outHour = (uint8_t)h;
  outMinute = (uint8_t)m;
}

void handleApiSettings() {
  JsonDocument req;
  if (deserializeJson(req, server.arg("plain")) != DeserializationError::Ok) {
    sendStatusJson(400);
    return;
  }

  JsonVariant vZip = req["zip"];
  if (!vZip.isNull()) {
    String newZip = vZip.as<String>();
    newZip.trim();
    if (isValidZip(newZip) && newZip != zipCode) {
      String newLatLon, newPlace;
      if (geocodeZip(newZip, newLatLon, newPlace)) {
        zipCode = newZip;
        latLon = newLatLon;
        placeName = newPlace;
        haveData = false;
        lastFetchMs = 0; // forces a refetch on the next loop() pass
      } else {
        Serial.printf("Failed to geocode zip %s\n", newZip.c_str());
      }
    }
  }

  JsonVariant vF = req["fahrenheit"];
  if (!vF.isNull()) useFahrenheit = vF.as<bool>();

  JsonVariant vLocalHost = req["localHost"];
  if (!vLocalHost.isNull()) {
    String newHost = vLocalHost.as<String>();
    newHost.trim();
    if (newHost.length() && newHost != localSensorHost) {
      localSensorHost = newHost;
      haveLocalSensor = false;
      lastLocalFetchMs = 0; // forces a refetch on the next loop() pass
    }
  }

  JsonVariant vAirHost = req["airHost"];
  if (!vAirHost.isNull()) {
    String newHost = vAirHost.as<String>();
    newHost.trim();
    if (newHost.length() && newHost != airQualityHost) {
      airQualityHost = newHost;
      haveAirQuality = false;
      lastAirFetchMs = 0; // forces a refetch on the next loop() pass
    }
  }

  JsonVariant vSpeed = req["cycleSeconds"];
  if (!vSpeed.isNull()) {
    long secs = vSpeed.as<long>();
    secs = constrain(secs, 1L, 60L);
    screenCycleMs = (unsigned long)secs * 1000UL;
  }

  JsonVariant vShow = req["show"];
  if (!vShow.isNull()) {
    JsonObject show = vShow.as<JsonObject>();
    if (!show["humidity"].isNull()) displaySettings.humidity = show["humidity"].as<bool>();
    if (!show["wind"].isNull()) displaySettings.wind = show["wind"].as<bool>();
    if (!show["rainChance"].isNull()) displaySettings.rainChance = show["rainChance"].as<bool>();
    if (!show["feelsLike"].isNull()) displaySettings.feelsLike = show["feelsLike"].as<bool>();
    if (!show["highLow"].isNull()) displaySettings.highLow = show["highLow"].as<bool>();
    if (!show["pressure"].isNull()) displaySettings.pressure = show["pressure"].as<bool>();
    if (!show["uv"].isNull()) displaySettings.uv = show["uv"].as<bool>();
    clampDisplayToggles();
  }

  JsonVariant vPower = req["power"];
  if (!vPower.isNull()) {
    JsonObject power = vPower.as<JsonObject>();
    if (!power["on"].isNull()) displayOn = power["on"].as<bool>();
    if (!power["brightness"].isNull()) {
      long b = power["brightness"].as<long>();
      displayBrightnessPct = (uint8_t)constrain(b, 5L, 100L);
    }
    applyDisplayPower();
    JsonVariant vSched = power["schedule"];
    if (!vSched.isNull()) {
      JsonObject sched = vSched.as<JsonObject>();
      if (!sched["enabled"].isNull()) powerSchedule.enabled = sched["enabled"].as<bool>();
      parseHHMM(sched["weekdayOn"], powerSchedule.weekdayOnHour, powerSchedule.weekdayOnMinute);
      parseHHMM(sched["weekdayOff"], powerSchedule.weekdayOffHour, powerSchedule.weekdayOffMinute);
      parseHHMM(sched["weekendOn"], powerSchedule.weekendOnHour, powerSchedule.weekendOnMinute);
      parseHHMM(sched["weekendOff"], powerSchedule.weekendOffHour, powerSchedule.weekendOffMinute);
      scheduleInitialized = false; // re-snap to the (possibly new) schedule right away
    }
  }

  saveSettings();
  renderCurrentScreen();
  sendStatusJson();
}

void handleApiNotesAdd() {
  JsonDocument req;
  deserializeJson(req, server.arg("plain"));
  const char* text = req["text"];
  if (text) addNote(String(text));
  renderCurrentScreen();
  sendStatusJson();
}

void handleApiNotesDelete() {
  JsonDocument req;
  deserializeJson(req, server.arg("plain"));
  int idx = req["index"] | -1;
  deleteNote(idx);
  clampCurrentScreen();
  renderCurrentScreen();
  sendStatusJson();
}

void handleApiGoto() {
  JsonDocument req;
  deserializeJson(req, server.arg("plain"));
  int screenCount = NUM_FIXED_SCREENS + (int)notes.size();
  int page = req["page"] | 0;
  page = constrain(page, 0, screenCount - 1);
  currentScreen = page;
  lastScreenSwitchMs = millis();
  renderCurrentScreen();
  sendStatusJson();
}

// Wall-clock local time, derived from NTP (UTC) plus the offset Open-Meteo
// reports for the station's location. Returns false until both have landed.
bool getLocalTimeInfo(struct tm& out) {
  time_t now;
  time(&now);
  if (now < 1700000000L) return false; // before NTP has synced (~Nov 2023)
  time_t local = now + utcOffsetSeconds;
  gmtime_r(&local, &out);
  return true;
}

// Snaps displayOn to the configured weekday/weekend schedule exactly at each
// on/off boundary crossing, so a manual toggle in between sticks until the
// next scheduled transition rather than being fought every loop iteration.
void tickPowerSchedule() {
  if (!powerSchedule.enabled) return;
  struct tm t;
  if (!getLocalTimeInfo(t)) return;

  bool isWeekend = (t.tm_wday == 0 || t.tm_wday == 6);
  int onMin = (isWeekend ? powerSchedule.weekendOnHour : powerSchedule.weekdayOnHour) * 60 +
              (isWeekend ? powerSchedule.weekendOnMinute : powerSchedule.weekdayOnMinute);
  int offMin = (isWeekend ? powerSchedule.weekendOffHour : powerSchedule.weekdayOffHour) * 60 +
               (isWeekend ? powerSchedule.weekendOffMinute : powerSchedule.weekdayOffMinute);
  int nowMin = t.tm_hour * 60 + t.tm_min;
  bool shouldBeOn = (onMin < offMin) ? (nowMin >= onMin && nowMin < offMin)
                                      : (nowMin >= onMin || nowMin < offMin);

  if (!scheduleInitialized) {
    scheduleInitialized = true;
    lastScheduledOn = shouldBeOn;
    if (displayOn != shouldBeOn) { displayOn = shouldBeOn; applyDisplayPower(); renderCurrentScreen(); }
    return;
  }
  if (shouldBeOn != lastScheduledOn) {
    lastScheduledOn = shouldBeOn;
    displayOn = shouldBeOn;
    applyDisplayPower();
    renderCurrentScreen();
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Booting...");

  gfx->begin();
  gfx->fillScreen(CARD_BG);

  gfx->setTextColor(INK, CARD_BG);
  centerText("Connecting WiFi...", 120, 116, 1, &FreeSans9pt7b);

  prefs.begin("notes", false);
  loadNotes();

  settingsPrefs.begin("settings", false);
  loadSettings();
  applyDisplayPower();

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    centerText("Locating...", 120, 144, 1, &FreeSans9pt7b);

    configTime(0, 0, "pool.ntp.org", "time.nist.gov"); // UTC; local offset comes from Open-Meteo per-fetch

    String newLatLon, newPlace;
    if (geocodeZip(zipCode, newLatLon, newPlace)) {
      latLon = newLatLon;
      placeName = newPlace;
    } else {
      Serial.printf("Failed to geocode zip %s\n", zipCode.c_str());
    }

    if (!latLon.isEmpty()) {
      haveData = fetchWeather(latLon, currentWeather);
      lastFetchMs = millis();
    }

    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/refresh", HTTP_POST, handleApiRefresh);
    server.on("/api/settings", HTTP_POST, handleApiSettings);
    server.on("/api/notes/add", HTTP_POST, handleApiNotesAdd);
    server.on("/api/notes/delete", HTTP_POST, handleApiNotesDelete);
    server.on("/api/goto", HTTP_POST, handleApiGoto);
    server.begin();

    if (MDNS.begin(MDNS_HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("Weather Station web UI started at http://%s.local/ (or http://%s/)\n",
                    MDNS_HOSTNAME, WiFi.localIP().toString().c_str());
    } else {
      Serial.printf("mDNS failed to start; web UI available at http://%s/\n", WiFi.localIP().toString().c_str());
    }
  }

  lastScreenSwitchMs = millis();
  renderCurrentScreen();
  Serial.println("Setup complete");
}

void loop() {
  bool needsRedraw = false;

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (WiFi.status() == WL_CONNECTED) {
    server.handleClient();

    if (latLon.isEmpty()) {
      String newLatLon, newPlace;
      if (geocodeZip(zipCode, newLatLon, newPlace)) {
        latLon = newLatLon;
        placeName = newPlace;
      }
    }

    if (!latLon.isEmpty() && (millis() - lastFetchMs >= UPDATE_INTERVAL_MS || lastFetchMs == 0)) {
      haveData = fetchWeather(latLon, currentWeather);
      lastFetchMs = millis();
      if (currentScreen == 0) needsRedraw = true;
    }

    if (millis() - lastLocalFetchMs >= LOCAL_SENSOR_INTERVAL_MS || lastLocalFetchMs == 0) {
      haveLocalSensor = fetchLocalSensor(localSensorHost, localSensor);
      lastLocalFetchMs = millis();
      if (currentScreen == 3) needsRedraw = true;
    }

    if (millis() - lastAirFetchMs >= AIR_QUALITY_INTERVAL_MS || lastAirFetchMs == 0) {
      haveAirQuality = fetchAirQuality(airQualityHost, airQuality);
      lastAirFetchMs = millis();
      if (currentScreen == 4) needsRedraw = true;
    }

    static unsigned long lastScheduleCheckMs = 0;
    if (millis() - lastScheduleCheckMs >= 15000UL) {
      lastScheduleCheckMs = millis();
      tickPowerSchedule(); // redraws itself on a schedule transition
    }
  }

  int screenCount = NUM_FIXED_SCREENS + (int)notes.size();
  if (millis() - lastScreenSwitchMs >= screenCycleMs) {
    currentScreen = (currentScreen + 1) % screenCount;
    lastScreenSwitchMs = millis();
    needsRedraw = true;
  } else if (currentScreen >= screenCount) {
    // notes were deleted out from under the current screen index
    currentScreen = 0;
    needsRedraw = true;
  }

  if (needsRedraw) {
    renderCurrentScreen();
  }

  delay(50);
}
