#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include "secrets.h"

// Reachable at http://localsensor.local/data — this is what the main
// weather station (../../platformio.ini) polls for hyper-local conditions.
// Change the "Local sensor" address in that station's web UI (or its
// localSensorHost default in src/main.cpp) if you rename this host.
static const char* MDNS_HOSTNAME = "localsensor";

// Arduino-ESP32's conventional I2C pins on the ESP32-S3-DevKitC-1 are
// SDA=8/SCL=9, but this board has the sensor's SDA/SCL wires physically
// swapped on those pins, so the assignment below is flipped to match rather
// than re-soldering.
static const int I2C_SDA_PIN = 9;
static const int I2C_SCL_PIN = 8;

static const unsigned long READ_INTERVAL_MS = 10UL * 1000UL;
// The telemetry table exists for a 3h pressure-trend RPC, not a live feed, so
// this can be far less frequent than the local read/serve cadence above.
static const unsigned long TELEMETRY_POST_INTERVAL_MS = 10UL * 60UL * 1000UL; // 10 minutes

Adafruit_AHTX0 aht;
Adafruit_BMP280 bmp;
bool ahtOk = false;
bool bmpOk = false;

WebServer server(80);

struct SensorReading {
  float tempC = NAN;
  float humidityPct = NAN;
  float pressureHpa = NAN;
  bool valid = false;
};
SensorReading reading;

// Cached result of the last successful Supabase telemetry post; the
// "telemetry" edge function's response already includes the freshly
// computed forecast (get_latest_weather_forecast RPC), so this node just
// caches and re-serves it rather than making the main station talk to
// Supabase directly.
struct ForecastCache {
  bool valid = false;
  String state;
  int rainProbabilityPct = -1;
  String pressureTrend;
  float seaLevelPressureHpa = NAN;
};
ForecastCache forecastCache;

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

// Prefers the AHT20's temperature (it's the humidity-paired sensor, so its
// temperature reading is what the RH% is actually relative to) and the
// BMP280's pressure. Falls back to whichever sensor is actually present.
void readSensors() {
  bool ok = false;

  if (ahtOk) {
    sensors_event_t humEvt, tempEvt;
    if (aht.getEvent(&humEvt, &tempEvt)) {
      reading.tempC = tempEvt.temperature;
      reading.humidityPct = humEvt.relative_humidity;
      ok = true;
    }
  }

  if (bmpOk) {
    float p = bmp.readPressure();
    if (!isnan(p) && p > 0) {
      reading.pressureHpa = p / 100.0f;
      if (!ahtOk) {
        reading.tempC = bmp.readTemperature();
        ok = true;
      }
    }
  }

  reading.valid = ok;
}

// Posts the current reading to Supabase for historical storage and reads
// back the derived forecast (dew point, sea-level pressure, 3h pressure
// trend, rain likelihood) that the edge function computes from it.
void postTelemetryAndFetchForecast() {
  if (WiFi.status() != WL_CONNECTED) return;

  JsonDocument reqDoc;
  reqDoc["tempC"] = reading.tempC;
  reqDoc["humidityPct"] = reading.humidityPct;
  reqDoc["pressureHpa"] = reading.pressureHpa;
  reqDoc["rssi"] = WiFi.RSSI();
  reqDoc["online"] = reading.valid;
  reqDoc["ahtFound"] = ahtOk;
  reqDoc["bmpFound"] = bmpOk;
  String body;
  serializeJson(reqDoc, body);

  WiFiClientSecure client;
  client.setInsecure(); // TLS to Supabase; not pinning the cert, matching the main station's approach to outbound HTTPS
  HTTPClient http;
  if (!http.begin(client, SUPABASE_TELEMETRY_URL)) {
    Serial.println("Telemetry POST: begin() failed");
    return;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-key", WEATHER_DEVICE_KEY);
  http.setTimeout(10000);

  int code = http.POST(body);
  if (code != HTTP_CODE_OK) {
    Serial.printf("Telemetry POST failed: code=%d\n", code);
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  JsonDocument filter;
  filter["forecast"]["forecast"]["state"] = true;
  filter["forecast"]["forecast"]["rain_probability_pct"] = true;
  filter["forecast"]["pressure_trend"]["classification"] = true;
  filter["forecast"]["sea_level_pressure_hpa"] = true;

  JsonDocument respDoc;
  if (deserializeJson(respDoc, payload, DeserializationOption::Filter(filter)) != DeserializationError::Ok) {
    Serial.println("Telemetry response parse failed");
    return;
  }

  JsonVariant fc = respDoc["forecast"];
  if (fc.isNull() || fc["forecast"].isNull()) {
    Serial.println("Telemetry response had no forecast (likely not enough history yet)");
    return;
  }

  forecastCache.state = fc["forecast"]["state"] | "";
  forecastCache.rainProbabilityPct = fc["forecast"]["rain_probability_pct"] | -1;
  forecastCache.pressureTrend = fc["pressure_trend"]["classification"] | "";
  forecastCache.seaLevelPressureHpa = fc["sea_level_pressure_hpa"] | NAN;
  forecastCache.valid = forecastCache.rainProbabilityPct >= 0;
}

void handleData() {
  JsonDocument doc;
  doc["online"] = reading.valid;
  doc["tempC"] = reading.tempC;
  doc["humidityPct"] = reading.humidityPct;
  doc["pressureHpa"] = reading.pressureHpa;
  doc["rssi"] = WiFi.RSSI();
  doc["ahtFound"] = ahtOk;
  doc["bmpFound"] = bmpOk;
  if (forecastCache.valid) {
    JsonObject forecast = doc["forecast"].to<JsonObject>();
    forecast["state"] = forecastCache.state;
    forecast["rainProbabilityPct"] = forecastCache.rainProbabilityPct;
    forecast["pressureTrend"] = forecastCache.pressureTrend;
    forecast["seaLevelPressureHpa"] = forecastCache.seaLevelPressureHpa;
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleRoot() {
  server.send(200, "text/plain", "Local weather sensor node. GET /data for JSON readings.");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Booting local sensor node...");

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  ahtOk = aht.begin();
  bmpOk = bmp.begin(0x76) || bmp.begin(0x77); // BMP280 breakouts ship at either I2C address
  if (bmpOk) {
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                     Adafruit_BMP280::SAMPLING_X2,
                     Adafruit_BMP280::SAMPLING_X16,
                     Adafruit_BMP280::FILTER_X16,
                     Adafruit_BMP280::STANDBY_MS_500);
  }
  Serial.printf("AHT20: %s, BMP280: %s\n", ahtOk ? "found" : "MISSING", bmpOk ? "found" : "MISSING");

  connectWiFi();
  readSensors();

  if (WiFi.status() == WL_CONNECTED) {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/data", HTTP_GET, handleData);
    server.begin();

    if (MDNS.begin(MDNS_HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("Sensor node up at http://%s.local/data (or http://%s/data)\n",
                    MDNS_HOSTNAME, WiFi.localIP().toString().c_str());
    } else {
      Serial.printf("mDNS failed to start; sensor node at http://%s/data\n", WiFi.localIP().toString().c_str());
    }
  }

  Serial.println("Setup complete");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  } else {
    server.handleClient();
  }

  static unsigned long lastReadMs = 0;
  if (millis() - lastReadMs >= READ_INTERVAL_MS || lastReadMs == 0) {
    lastReadMs = millis();
    readSensors();
  }

  static unsigned long lastTelemetryPostMs = 0;
  if (reading.valid && (millis() - lastTelemetryPostMs >= TELEMETRY_POST_INTERVAL_MS || lastTelemetryPostMs == 0)) {
    lastTelemetryPostMs = millis();
    postTelemetryAndFetchForecast();
  }

  delay(20);
}
