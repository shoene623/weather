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
static const int PMS_RX_PIN = 16; // ESP32 RX <- sensor TXD (classic ESP32's conventional Serial2 RX pin)
static const int PMS_TX_PIN = 17; // ESP32 TX -> sensor RXD (unused in active mode; wired for possible future passive-mode control)

// The PMS5003 pushes a fresh 32-byte frame roughly once per second in active
// mode; if none has parsed successfully in this long, report the node as
// offline (unplugged/miswired/still warming up) rather than serving a
// forever-stale reading.
static const unsigned long STALE_AFTER_MS = 5UL * 1000UL;

Adafruit_PM25AQI pm25Sensor = Adafruit_PM25AQI();
HardwareSerial pmsSerial(1);
bool sensorFound = false;

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
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleRoot() {
  server.send(200, "text/plain", "Air quality sensor node. GET /data for JSON readings.");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Booting air quality node...");

  pmsSerial.begin(9600, SERIAL_8N1, PMS_RX_PIN, PMS_TX_PIN);
  sensorFound = pm25Sensor.begin_UART(&pmsSerial);
  Serial.printf("PMS5003: %s\n", sensorFound ? "found" : "MISSING");

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/data", HTTP_GET, handleData);
    server.begin();

    if (MDNS.begin(MDNS_HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("Air quality node up at http://%s.local/data (or http://%s/data)\n",
                    MDNS_HOSTNAME, WiFi.localIP().toString().c_str());
    } else {
      Serial.printf("mDNS failed to start; air quality node at http://%s/data\n", WiFi.localIP().toString().c_str());
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
  }

  delay(20);
}
