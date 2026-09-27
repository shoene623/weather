#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <Adafruit_PM25AQI.h>
#include "secrets.h"

// Reachable at http://airquality.local/data — this is what the main weather
// station (../../platformio.ini) polls for particulate readings. Change the
// "Air quality" address in that station's web UI (or its airQualityHost
// default in src/main.cpp) if you rename this host.
static const char* MDNS_HOSTNAME = "airquality";

// PMS5003 UART wiring: sensor TXD (pin 5) -> this RX pin, sensor RXD (pin 4)
// -> this TX pin. Both signal wires are 3.3V logic (no level shifter needed)
// even though the sensor itself is powered from 5V on separate VCC/GND
// wires. SET (pin 3) and RESET (pin 6) are tied to 3.3V rather than left
// floating, which keeps the sensor in continuous active mode -- simplest
// wiring, at the cost of running the fan continuously rather than
// duty-cycling it to extend its life.
// On the ESP32-C3 SuperMini these are the pins silkscreened RX/TX (UART0's
// default pins). Serial is routed to native USB instead (see
// platformio.ini's ARDUINO_USB_CDC_ON_BOOT), leaving these free for the
// sensor on UART1. Avoid GPIO18/19 (native USB D-/D+) and the strapping pins
// GPIO2/8/9 (GPIO8 is also the onboard LED).
static const int PMS_RX_PIN = 20; // ESP32 RX <- sensor TXD
static const int PMS_TX_PIN = 21; // ESP32 TX -> sensor RXD (unused in active mode; wired for possible future passive-mode control)

// The PMS5003 pushes a fresh 32-byte frame roughly once per second in active
// mode; if none has parsed successfully in this long, report the node as
// offline (unplugged/miswired/still warming up) rather than serving a
// forever-stale reading.
static const unsigned long STALE_AFTER_MS = 5UL * 1000UL;

Adafruit_PM25AQI pm25Sensor = Adafruit_PM25AQI();
HardwareSerial pmsSerial(1);
bool sensorFound = false;

// Debug: passes the sensor UART through to the library while counting the
// raw bytes it consumes and remembering the most recent ones, so we can tell
// "no bytes at all" (power/wiring) from "bytes but no valid frames" (baud,
// noise, wrong pin).
class CountingStream : public Stream {
 public:
  explicit CountingStream(Stream& inner) : inner_(inner) {}
  int available() override { return inner_.available(); }
  int peek() override { return inner_.peek(); }
  int read() override {
    int b = inner_.read();
    if (b >= 0) {
      bytesRead++;
      recent[recentPos++ % sizeof(recent)] = (uint8_t)b;
    }
    return b;
  }
  size_t write(uint8_t b) override { return inner_.write(b); }
  void flush() override { inner_.flush(); }

  unsigned long bytesRead = 0;
  uint8_t recent[16] = {0};
  unsigned int recentPos = 0;

 private:
  Stream& inner_;
};
CountingStream pmsDebugStream(pmsSerial);
unsigned long framesOk = 0;
unsigned long lastDebugLogMs = 0;
static const unsigned long DEBUG_LOG_EVERY_MS = 2000;

WebServer server(80);

struct AirReading {
  bool valid = false;
  uint16_t pm1_0 = 0;
  uint16_t pm2_5 = 0;
  uint16_t pm10 = 0;
  uint16_t aqi = 0;
};
AirReading reading;
unsigned long lastValidReadMs = 0;

// If the WiFi driver's own auto-reconnect hasn't gotten us back on within
// this long, kick it with an explicit reconnect().
static const unsigned long RECONNECT_KICK_MS = 30UL * 1000UL;
unsigned long disconnectedSinceMs = 0;
bool mdnsStarted = false;

// Called once from setup(). After that the driver auto-reconnects on its
// own; loop() only nudges it (never tears it down mid-attempt, which caused
// "sta is connecting, cannot set config" and more AP drops).
void startWiFi() {
  WiFi.mode(WIFI_STA);
  // Many ESP32-C3 SuperMini boards have a poorly matched antenna and can't
  // complete a WiFi handshake at the default (max) TX power; 8.5dBm is the
  // commonly cited level that makes them connect reliably. Set before
  // begin() so the very first auth attempt already uses it.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  // Modem sleep made the node miss/delay incoming requests (dropped pings,
  // ~20s first HTTP response); it's USB-powered, so keep the radio awake.
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("Connecting to WiFi \"%s\"", WIFI_SSID);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  // (Success is logged by the GOT_IP handler in setup().)
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected yet; will keep retrying in the background");
  }
}

// mDNS needs an IP, so it's started the first time we're connected --
// whether that's during setup() or minutes later after a failed boot connect.
void startMdnsOnce() {
  if (mdnsStarted) return;
  mdnsStarted = true;
  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Air quality node up at http://%s.local/data (or http://%s/data)\n",
                  MDNS_HOSTNAME, WiFi.localIP().toString().c_str());
  } else {
    Serial.printf("mDNS failed to start; air quality node at http://%s/data\n", WiFi.localIP().toString().c_str());
  }
}

// Labels the library's already-computed US EPA PM2.5 AQI (data.aqi_pm25_us)
// into the standard AirNow categories, so the main station can show more
// than a bare number.
const char* aqiCategory(uint16_t aqiValue) {
  if (aqiValue <= 50) return "Good";
  if (aqiValue <= 100) return "Moderate";
  if (aqiValue <= 150) return "Unhealthy (sensitive)";
  if (aqiValue <= 200) return "Unhealthy";
  if (aqiValue <= 300) return "Very unhealthy";
  return "Hazardous";
}

void handleData() {
  bool fresh = reading.valid && (millis() - lastValidReadMs < STALE_AFTER_MS);
  JsonDocument doc;
  doc["online"] = fresh;
  doc["pm1_0"] = reading.pm1_0;
  doc["pm2_5"] = reading.pm2_5;
  doc["pm10"] = reading.pm10;
  if (fresh) {
    doc["aqi"] = reading.aqi;
    doc["aqiCategory"] = aqiCategory(reading.aqi);
  }
  doc["rssi"] = WiFi.RSSI();
  doc["sensorFound"] = sensorFound;
  doc["debugBytesRead"] = pmsDebugStream.bytesRead;
  doc["debugFramesOk"] = framesOk;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleRoot() {
  server.send(200, "text/plain", "Air quality sensor node. GET /data for JSON readings.");
}

void setup() {
  Serial.begin(115200);
  // Serial is native USB (HWCDC). While plugged into a PC with no monitor
  // open, the default TX timeout makes every print block, stalling loop()
  // so HTTP requests hang. Drop output instead of waiting.
  Serial.setTxTimeoutMs(0);
  delay(1000);
  Serial.println("Booting air quality node...");

  pmsSerial.begin(9600, SERIAL_8N1, PMS_RX_PIN, PMS_TX_PIN);
  sensorFound = pm25Sensor.begin_UART(&pmsDebugStream);
  Serial.printf("PMS5003: %s\n", sensorFound ? "found" : "MISSING");

  // Log why the AP dropped/refused us (esp_wifi reason codes, e.g. 2 = auth
  // expired, 3 = AP kicked us, 4 = AP inactivity timeout, 15 = 4-way
  // handshake timeout, 201 = AP not found) -- the C3 SuperMini's WiFi can be
  // flaky, and this is the only clue to why.
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    Serial.printf("WiFi disconnected, reason=%d\n", info.wifi_sta_disconnected.reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) {
    Serial.printf("WiFi connected, IP=%s\n", WiFi.localIP().toString().c_str());
  }, ARDUINO_EVENT_WIFI_STA_GOT_IP);

  startWiFi();

  // Must come after startWiFi(): WiFi.mode() is what brings up the lwIP
  // stack, and server.begin() before that asserts on a null TCP/IP lock
  // (boot loop). It's fine if we're not connected yet -- it just won't get
  // requests until we have an IP.
  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.begin();

  Serial.println("Setup complete");
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    disconnectedSinceMs = 0;
    startMdnsOnce();
    server.handleClient();
  } else if (disconnectedSinceMs == 0) {
    disconnectedSinceMs = millis();
  } else if (millis() - disconnectedSinceMs > RECONNECT_KICK_MS) {
    Serial.println("Still offline; forcing WiFi reconnect");
    WiFi.reconnect();
    disconnectedSinceMs = millis();
  }

  // read() buffers incoming bytes internally and only returns true once a
  // full, checksum-valid 32-byte frame has arrived, so it's safe (and
  // necessary) to poll it every loop iteration rather than on a timer.
  PM25_AQI_Data data;
  if (pm25Sensor.read(&data)) {
    reading.pm1_0 = data.pm10_env;  // library names PM1.0 "pm10" (1.0um cutoff) -- not a typo
    reading.pm2_5 = data.pm25_env;
    reading.pm10 = data.pm100_env;  // and PM10 "pm100" (10um cutoff)
    reading.aqi = data.aqi_pm25_us;
    reading.valid = true;
    lastValidReadMs = millis();
    framesOk++;
  }

  if (millis() - lastDebugLogMs > DEBUG_LOG_EVERY_MS) {
    lastDebugLogMs = millis();
    Serial.printf("[pms] bytes=%lu framesOk=%lu pm2.5=%u last:",
                  pmsDebugStream.bytesRead, framesOk, reading.pm2_5);
    unsigned int n = min<unsigned int>(pmsDebugStream.recentPos, sizeof(pmsDebugStream.recent));
    for (unsigned int i = 0; i < n; i++) {
      unsigned int idx = (pmsDebugStream.recentPos - n + i) % sizeof(pmsDebugStream.recent);
      Serial.printf(" %02X", pmsDebugStream.recent[idx]);
    }
    Serial.println();
  }

  delay(20);
}
