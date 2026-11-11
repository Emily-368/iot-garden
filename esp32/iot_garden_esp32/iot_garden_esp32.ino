/*
  IoT Garden Monitor: Seeed Studio XIAO ESP32-C6
  ----------------------------------------------
  Battery-powered outdoor sensor node. Each cycle it:
    1. Powers the sensors, averages their readings, then powers them off
    2. Joins Wi-Fi through the external U.FL antenna
    3. Publishes one JSON reading to MQTT on the Pi, e.g.
       {"temp_c":25.3,"moisture_pct":45,"water":"ok","water_below":30,
        "wet_above":80,"temp_raw":1650,"moisture_raw":1200,
        "interval_s":600,"awake_ms":1180,"rssi":-58}
    4. Deep sleeps until the next reading

  temp_raw and moisture_raw are in millivolts, not Arduino ADC counts.
  awake_ms is how long the previous cycle stayed awake (for battery testing).

  Battery use is almost all Wi-Fi time, so the sketch keeps each wake short:
    - Remembers the router's channel and address, so it can skip the scan
    - Optional fixed IP address (secrets.h), so it can skip DHCP
    - Short timeouts, and longer sleeps while the router or Pi is unreachable
    - Skips Wi-Fi and sleeps for an hour when the battery is nearly empty
  See ../BATTERY_TESTING.md for how to measure it.

  Wiring (3.3 V only: the pins are not 5 V tolerant)
    Temperature sensor (KS0033)   Signal -> A1   VCC -> D3   GND -> GND
    Soil sensor (KS0049)          Signal -> A2   VCC -> D3   GND -> GND
    Battery                       + -> BAT+ pad (near D5)
                                  - -> BAT- pad (near D8)
    Battery level (optional, set BATTERY_DIVIDER_FITTED in calibration.h)
                                  BAT+ pad -> 200k -> A0 -> 200k -> GND

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
#include <driver/gpio.h>
#include <math.h>
#include <string.h>
#include "calibration.h"
#include "secrets.h"

// ---------------- Pins ----------------
const int batteryPin     = A0;  // battery voltage divider
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
const int NUM_SAMPLES = 10;                        // readings averaged per update
const unsigned long SENSOR_SETTLE_MS     = 100;    // wait after powering the sensors
const unsigned long WIFI_FAST_TIMEOUT_MS = 4000;   // reconnecting to the remembered router
const unsigned long WIFI_TIMEOUT_MS      = 10000;  // full scan and connect
const unsigned long MQTT_TIMEOUT_MS      = 4000;
const unsigned long MQTT_FLUSH_MS        = 100;    // lets the message leave before Wi-Fi stops

// After this many failed cycles in a row, each sleep doubles (up to 8x, and
// never more than MAX_SLEEP_S), so an unreachable router doesn't drain the battery.
const int FAILS_BEFORE_BACKOFF  = 3;
const unsigned long MAX_SLEEP_S = 3600;

// Watering states
const int WATER_OK      = 0;
const int WATER_NEEDED  = 1;
const int WATER_TOO_WET = 2;

// RTC memory survives deep sleep (but not a reset or power loss)
RTC_DATA_ATTR int savedWaterState = WATER_OK;   // keeps hysteresis working between wake-ups
RTC_DATA_ATTR int32_t savedChannel = 0;         // router's Wi-Fi channel, 0 = not known
RTC_DATA_ATTR uint8_t savedBssid[6] = {0};      // router's address
RTC_DATA_ATTR uint32_t lastAwakeMs = 0;         // how long the previous cycle was awake
RTC_DATA_ATTR uint8_t failedCycles = 0;         // failed cycles in a row

struct Reading {
  float tempC;
  int tempRaw;
  int moisturePct;
  int moistureRaw;
  int waterState;
  int batteryMv;   // -1 when no divider is fitted
};

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
  pinMode(antennaSelectPin, OUTPUT);
  digitalWrite(antennaSelectPin, HIGH);
}

// Average several readings to smooth out noise, in millivolts
int readAverageMv(int pin) {
  analogReadMilliVolts(pin);   // the first reading after switching pins can be off; discard it
  long total = 0;
  for (int i = 0; i < NUM_SAMPLES; i++) {
    total += analogReadMilliVolts(pin);
    delay(2);
  }
  return total / NUM_SAMPLES;
}

// Convert the KS0033 thermistor reading to Celsius (Steinhart-Hart equation)
float temperatureFromMv(int rawMv) {
  // Avoid divide-by-zero / log(0) if the sensor is disconnected
  float ratio = constrain(rawMv / SENSOR_SUPPLY_MV, 0.001f, 0.999f);
  double resistance = 10000.0 * (1.0 / ratio - 1.0);
  double logR = log(resistance);
  double kelvin = 1.0 / (0.001129148 +
                         (0.000234125 + 0.0000000876741 * logR * logR) * logR);
  return kelvin - 273.15 + TEMP_OFFSET;
}

// Convert the moisture reading to 0-100 % using the calibration values
int moisturePercentFromMv(int rawMv) {
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

// Approximate LiPo charge from its voltage at light load
int batteryPercent(int batteryMv) {
  const int points[][2] = {
    {3000, 0}, {3450, 5}, {3680, 10}, {3740, 20}, {3770, 30}, {3790, 40},
    {3820, 50}, {3870, 60}, {3920, 70}, {3980, 80}, {4060, 90}, {4200, 100}
  };
  const int count = sizeof(points) / sizeof(points[0]);

  if (batteryMv <= points[0][0]) return 0;
  for (int i = 1; i < count; i++) {
    if (batteryMv <= points[i][0]) {
      return map(batteryMv, points[i - 1][0], points[i][0], points[i - 1][1], points[i][1]);
    }
  }
  return 100;
}

// Short code sent to the Pi
const char* waterCode(int state) {
  if (state == WATER_NEEDED)  return "dry";
  if (state == WATER_TOO_WET) return "wet";
  return "ok";
}

// Power the sensors, read everything, then power them off again.
// This runs before Wi-Fi starts: radio bursts add noise to analog readings,
// and the battery reads truest while the load is light.
Reading readSensors() {
  Reading r;

  digitalWrite(sensorPowerPin, HIGH);
  delay(SENSOR_SETTLE_MS);
  r.tempRaw     = readAverageMv(sensorPin);
  r.moistureRaw = readAverageMv(moisturePin);
  digitalWrite(sensorPowerPin, LOW);

  r.tempC       = temperatureFromMv(r.tempRaw);
  r.moisturePct = moisturePercentFromMv(r.moistureRaw);
  r.waterState  = waterState(r.moisturePct);

  r.batteryMv = -1;
  if (BATTERY_DIVIDER_FITTED) {
    r.batteryMv = readAverageMv(batteryPin) * BATTERY_DIVIDER_RATIO;
  }
  return r;
}

// Build the JSON message. rssi is 0 when Wi-Fi isn't connected.
void formatPayload(char* out, size_t size, const Reading& r, int rssi) {
  int n = snprintf(out, size,
                   "{\"temp_c\":%.1f,\"moisture_pct\":%d,\"water\":\"%s\","
                   "\"water_below\":%d,\"wet_above\":%d,"
                   "\"temp_raw\":%d,\"moisture_raw\":%d,"
                   "\"interval_s\":%lu,\"awake_ms\":%lu",
                   r.tempC, r.moisturePct, waterCode(r.waterState),
                   WATER_BELOW_PCT, TOO_WET_ABOVE_PCT, r.tempRaw, r.moistureRaw,
                   (unsigned long)SLEEP_INTERVAL_S, (unsigned long)lastAwakeMs);
  if (r.batteryMv >= 0 && n > 0 && (size_t)n < size) {
    n += snprintf(out + n, size - n, ",\"battery_mv\":%d,\"battery_pct\":%d",
                  r.batteryMv, batteryPercent(r.batteryMv));
  }
  if (rssi != 0 && n > 0 && (size_t)n < size) {
    n += snprintf(out + n, size - n, ",\"rssi\":%d", rssi);
  }
  if (n > 0 && (size_t)n < size) {
    snprintf(out + n, size - n, "}");
  }
}

bool waitForWifi(unsigned long timeoutMs) {
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start >= timeoutMs) return false;
    delay(20);
  }
  return true;
}

bool connectWifi() {
  WiFi.persistent(false);   // don't wear out the flash by saving settings each wake
  WiFi.mode(WIFI_STA);
#ifdef USE_STATIC_IP
  WiFi.config(STATIC_IP, STATIC_GATEWAY, STATIC_SUBNET, STATIC_DNS);   // skips DHCP
#endif

  // Fast path: go straight to the router we used last time, without scanning
  if (savedChannel > 0) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, savedChannel, savedBssid, true);
#ifdef WIFI_TX_POWER
    WiFi.setTxPower(WIFI_TX_POWER);
#endif
    if (waitForWifi(WIFI_FAST_TIMEOUT_MS)) return true;
    savedChannel = 0;       // the router moved channel or was replaced
    WiFi.disconnect();
    delay(50);
  }

  // Slow path: scan for the network by name, then remember where it was
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
#ifdef WIFI_TX_POWER
  WiFi.setTxPower(WIFI_TX_POWER);
#endif
  if (!waitForWifi(WIFI_TIMEOUT_MS)) return false;

  const uint8_t* bssid = WiFi.BSSID();
  if (bssid != nullptr) {
    memcpy(savedBssid, bssid, sizeof(savedBssid));
    savedChannel = WiFi.channel();
  }
  return true;
}

bool publishReading(const char* payload) {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);     // the default 256 bytes is too close to the message size
  mqtt.setSocketTimeout(3);

  unsigned long start = millis();
  while (!mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
    if (millis() - start >= MQTT_TIMEOUT_MS) return false;
    delay(100);
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

// Read the sensors once and send the result to the Pi.
// Returns how long to sleep before the next cycle, in seconds.
unsigned long runCycle() {
  Reading r = readSensors();
  char payload[320];

  // A nearly empty battery sags under Wi-Fi load and the board can restart
  // over and over, so leave the radio off and wait for a recharge.
  if (r.batteryMv >= 0 && r.batteryMv > BATTERY_PRESENT_MV && r.batteryMv < BATTERY_CRITICAL_MV) {
    formatPayload(payload, sizeof(payload), r, 0);
    logLine(payload);
    logLine("Battery critical: skipping Wi-Fi");
    return MAX_SLEEP_S;
  }

  bool sent = false;
  if (!connectWifi()) {
    formatPayload(payload, sizeof(payload), r, 0);
    logLine(payload);
    logLine("Wi-Fi connection failed");
  } else {
    formatPayload(payload, sizeof(payload), r, WiFi.RSSI());
    logLine(payload);
    sent = publishReading(payload);
    logLine(sent ? "Published" : "MQTT publish failed");
  }
  stopWifi();

  if (sent) {
    failedCycles = 0;
    return SLEEP_INTERVAL_S;
  }

  if (failedCycles < 255) failedCycles++;
  unsigned long sleepS = SLEEP_INTERVAL_S;
  if (failedCycles >= FAILS_BEFORE_BACKOFF) {
    int doublings = failedCycles - FAILS_BEFORE_BACKOFF + 1;
    if (doublings > 3) doublings = 3;
    sleepS <<= doublings;
    if (sleepS > MAX_SLEEP_S && SLEEP_INTERVAL_S <= MAX_SLEEP_S) sleepS = MAX_SLEEP_S;
  }
  return sleepS;
}

// Turn everything off and sleep. The board restarts from setup() afterwards.
void deepSleep(unsigned long seconds) {
  // Switch the RF switch off and keep the sensors unpowered. Without a hold,
  // the pins float during deep sleep.
  digitalWrite(antennaSelectPin, LOW);
  digitalWrite(rfSwitchEnablePin, HIGH);
  digitalWrite(sensorPowerPin, LOW);
  gpio_hold_en((gpio_num_t)rfSwitchEnablePin);
  gpio_hold_en((gpio_num_t)sensorPowerPin);

  lastAwakeMs = millis();
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_start();
}

// ---------------- Main ----------------

void setup() {
  setCpuFrequencyMhz(80);   // the slowest speed Wi-Fi allows; lowers current while awake

  if (BENCH_MODE) Serial.begin(115200);

  // Release the pins that were held during deep sleep
  gpio_hold_dis((gpio_num_t)rfSwitchEnablePin);
  gpio_hold_dis((gpio_num_t)sensorPowerPin);

  pinMode(sensorPowerPin, OUTPUT);
  digitalWrite(sensorPowerPin, LOW);

  analogReadResolution(12);
  selectExternalAntenna();
}

void loop() {
  unsigned long sleepS = runCycle();

  if (BENCH_MODE) {
    lastAwakeMs = 0;   // not meaningful while the board never sleeps
    delay(BENCH_INTERVAL_S * 1000UL);
  } else {
    deepSleep(sleepS);
  }
}
