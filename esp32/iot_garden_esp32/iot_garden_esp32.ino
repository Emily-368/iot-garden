/*
  IoT Garden Monitor: Seeed Studio XIAO ESP32-C6
  ----------------------------------------------
  Battery-powered outdoor sensor node. Each cycle it:
    1. Powers the sensors, averages their readings, then powers them off
    2. Joins Wi-Fi through the external U.FL antenna
    3. Publishes one JSON reading to MQTT on the Pi, e.g.
       {"temp_c":25.3,"moisture_pct":45,"water":"ok","water_below":30,
        "wet_above":80,"temp_raw":1650,"moisture_raw":1200}
    4. Deep sleeps until the next reading

  temp_raw and moisture_raw are in millivolts, not Arduino ADC counts.

  Wiring (3.3 V only: the pins are not 5 V tolerant)
    Temperature sensor (KS0033)   Signal -> A1   VCC -> D3   GND -> GND
    Soil sensor (KS0049)          Signal -> A2   VCC -> D3   GND -> GND
    Battery                       + -> BAT+ pad (near D5)
                                  - -> BAT- pad (near D8)
    A0 is left free for a future battery voltage divider.

  Setup
    Arduino IDE board: XIAO_ESP32C6 (esp32 by Espressif core, version 3.x)
    Library: PubSubClient by Nick O'Leary
    Copy secrets.example.h to secrets.h and fill it in.
    Sensor calibration and timing are in calibration.h.

  Keep BENCH_MODE true while testing. In deep sleep the USB port disappears, so
  to upload afterwards: hold BOOT, tap RESET, release BOOT, then upload.
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_sleep.h>
#include <math.h>
#include "calibration.h"
#include "secrets.h"

// ---------------- Pins ----------------
const int sensorPin      = A1;  // temperature sensor
const int moisturePin    = A2;  // soil moisture sensor
const int sensorPowerPin = D3;  // powers both sensors only while reading

// RF switch on the XIAO ESP32-C6 (not exposed on the header pins)
const int rfSwitchEnablePin = 3;   // LOW = RF switch on
const int antennaSelectPin  = 14;  // LOW = built-in antenna, HIGH = U.FL antenna

// ---------------- MQTT ----------------
const char* MQTT_TOPIC     = "garden/esp32/reading";
const char* MQTT_CLIENT_ID = "iot-garden-esp32";

// ---------------- Settings ----------------
const int NUM_SAMPLES = 10;                       // readings averaged per update
const unsigned long SENSOR_SETTLE_MS = 100;       // wait after powering the sensors
const unsigned long WIFI_TIMEOUT_MS  = 15000;     // give up and sleep after this
const unsigned long MQTT_TIMEOUT_MS  = 5000;
const unsigned long MQTT_FLUSH_MS    = 100;       // lets the message leave before Wi-Fi stops
const float SENSOR_SUPPLY_MV         = 3300.0;    // voltage on sensorPowerPin

// Watering states
const int WATER_OK      = 0;
const int WATER_NEEDED  = 1;
const int WATER_TOO_WET = 2;

// RTC memory survives deep sleep, so hysteresis still works between wake-ups
RTC_DATA_ATTR int savedWaterState = WATER_OK;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// ---------------- Helpers ----------------

void logLine(const char* message) {
  if (BENCH_MODE) Serial.println(message);
}

// Use the external antenna. Must run before Wi-Fi starts.
void selectExternalAntenna() {
  pinMode(rfSwitchEnablePin, OUTPUT);
  digitalWrite(rfSwitchEnablePin, LOW);
  delay(100);
  pinMode(antennaSelectPin, OUTPUT);
  digitalWrite(antennaSelectPin, HIGH);
}

// Average several readings to smooth out noise, in millivolts
int readAverageMv(int pin) {
  long total = 0;
  for (int i = 0; i < NUM_SAMPLES; i++) {
    total += analogReadMilliVolts(pin);
    delay(5);
  }
  return total / NUM_SAMPLES;
}

// Convert the KS0033 thermistor reading to Celsius (Steinhart-Hart equation)
float readTemperatureC(int &rawMv) {
  rawMv = readAverageMv(sensorPin);
  // Avoid divide-by-zero / log(0) if the sensor is disconnected
  float ratio = constrain(rawMv / SENSOR_SUPPLY_MV, 0.001f, 0.999f);
  double resistance = 10000.0 * (1.0 / ratio - 1.0);
  double logR = log(resistance);
  double kelvin = 1.0 / (0.001129148 +
                         (0.000234125 + 0.0000000876741 * logR * logR) * logR);
  return kelvin - 273.15 + TEMP_OFFSET;
}

// Convert the moisture reading to 0-100 % using the calibration values
int readMoisturePercent(int &rawMv) {
  rawMv = readAverageMv(moisturePin);
  if (MOISTURE_WET == MOISTURE_DRY) return 0;   // calibration not valid
  int percent = map(rawMv, MOISTURE_DRY, MOISTURE_WET, 0, 100);
  return constrain(percent, 0, 100);
}

// Decide the watering status, with hysteresis so it doesn't flicker
int waterState(int moisturePct) {
  if (moisturePct < WATER_BELOW_PCT) {
    savedWaterState = WATER_NEEDED;
  } else if (moisturePct > TOO_WET_ABOVE_PCT) {
    savedWaterState = WATER_TOO_WET;
  } else if (savedWaterState == WATER_NEEDED &&
             moisturePct >= WATER_BELOW_PCT + MOISTURE_HYSTERESIS) {
    savedWaterState = WATER_OK;
  } else if (savedWaterState == WATER_TOO_WET &&
             moisturePct <= TOO_WET_ABOVE_PCT - MOISTURE_HYSTERESIS) {
    savedWaterState = WATER_OK;
  }
  return savedWaterState;
}

// Short code sent to the Pi
const char* waterCode(int state) {
  if (state == WATER_NEEDED)  return "dry";
  if (state == WATER_TOO_WET) return "wet";
  return "ok";
}

bool connectWifi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start >= WIFI_TIMEOUT_MS) return false;
    delay(100);
  }
  return true;
}

bool publishReading(const char* payload) {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);

  unsigned long start = millis();
  while (!mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
    if (millis() - start >= MQTT_TIMEOUT_MS) return false;
    delay(250);
  }

  bool sent = mqtt.publish(MQTT_TOPIC, payload);
  mqtt.loop();
  delay(MQTT_FLUSH_MS);
  mqtt.disconnect();
  return sent;
}

void stopWifi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// Read the sensors once and send the result to the Pi
void readAndPublish() {
  digitalWrite(sensorPowerPin, HIGH);
  delay(SENSOR_SETTLE_MS);

  int tempRaw, moistureRaw;
  float tempC     = readTemperatureC(tempRaw);
  int moisturePct = readMoisturePercent(moistureRaw);
  int state       = waterState(moisturePct);

  digitalWrite(sensorPowerPin, LOW);

  char payload[160];
  snprintf(payload, sizeof(payload),
           "{\"temp_c\":%.1f,\"moisture_pct\":%d,\"water\":\"%s\","
           "\"water_below\":%d,\"wet_above\":%d,"
           "\"temp_raw\":%d,\"moisture_raw\":%d}",
           tempC, moisturePct, waterCode(state),
           WATER_BELOW_PCT, TOO_WET_ABOVE_PCT, tempRaw, moistureRaw);
  logLine(payload);

  if (!connectWifi()) {
    logLine("Wi-Fi connection failed");
  } else if (!publishReading(payload)) {
    logLine("MQTT publish failed");
  } else {
    logLine("Published");
  }
  stopWifi();
}

// ---------------- Main ----------------

void setup() {
  if (BENCH_MODE) Serial.begin(115200);

  pinMode(sensorPowerPin, OUTPUT);
  digitalWrite(sensorPowerPin, LOW);

  analogReadResolution(12);
  selectExternalAntenna();
}

void loop() {
  readAndPublish();

  if (BENCH_MODE) {
    delay(BENCH_INTERVAL_S * 1000UL);
  } else {
    // Deep sleep restarts the board from setup() when the timer ends
    esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_INTERVAL_S * 1000000ULL);
    esp_deep_sleep_start();
  }
}
