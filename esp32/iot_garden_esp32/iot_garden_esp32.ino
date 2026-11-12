/*
  IoT Garden Monitor: Seeed Studio XIAO ESP32-C6
  ----------------------------------------------
  Battery-powered outdoor sensor node. Each cycle it:
    1. Powers the sensors, averages their readings, then powers them off
    2. Joins Wi-Fi through the external or built-in antenna (USE_EXTERNAL_ANTENNA)
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
    Temperature sensor (KS0033)   Signal -> D1   VCC -> D3   GND -> GND
    Soil sensor (KS0049)          Signal -> D2   VCC -> D3   GND -> GND
    Battery                       + -> BAT+ pad (near D5)
                                  - -> BAT- pad (near D8)
    Battery level (optional, set BATTERY_DIVIDER_FITTED in calibration.h)
                                  BAT+ pad -> 200k -> D0 -> 200k -> GND

  Setup
    Arduino IDE board: XIAO_ESP32C6 (esp32 by Espressif core, version 3.x)
    Library: PubSubClient by Nick O'Leary
    Copy secrets.example.h to secrets.h and fill it in.
    Sensor calibration and timing are in calibration.h.

  Keep BENCH_MODE true while testing. In deep sleep the USB port disappears, so
  to upload afterwards: hold BOOT, tap RESET, release BOOT, then upload. Or use
  the service button below.

  Service button (optional, BUTTON_FITTED in calibration.h)
    Wiring: one leg to the MTCK pad on the back of the board (GPIO6), the
    other leg to GND. No resistor needed.

    Short press      Troubleshooting mode. The board wakes and stays awake with
                     Wi-Fi connected in modem sleep (the radio naps between
                     router beacons). It sends a reading every 10 seconds, and
                     prints details on USB serial at 115200 baud. The LED
                     blinks slowly.
    Hold 3 seconds   Update mode. Hold until the LED stays on, then let go.
                     Wi-Fi runs at full power and the board appears in Arduino
                     IDE under Tools > Port as "iot-garden-esp32", ready for a
                     wireless (OTA) upload. The LED blinks quickly. Needs
                     OTA_PASSWORD set in secrets.h.
    Press again      Leaves either mode and goes back to sleep.

    Both modes end on their own after SERVICE_TIMEOUT_S (10 minutes), so a
    forgotten board doesn't flatten the battery.
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <math.h>
#include <string.h>
#include "calibration.h"
#include "secrets.h"
#ifdef OTA_PASSWORD
#include <ArduinoOTA.h>
static_assert(sizeof(OTA_PASSWORD) > 1, "OTA_PASSWORD in secrets.h must not be empty");
#endif

// Only GPIO 0 to 7 can wake this chip from deep sleep. 1 and 2 are the sensors.
static_assert(BUTTON_GPIO == 0 || (BUTTON_GPIO >= 4 && BUTTON_GPIO <= 7),
              "BUTTON_GPIO must be 0 (D0), or 4 to 7 (pads on the back)");
static_assert(!(BUTTON_FITTED && BATTERY_DIVIDER_FITTED && BUTTON_GPIO == 0),
              "D0 is used by the battery divider: move the button to the MTCK pad (BUTTON_GPIO = 6)");

// ---------------- Pins ----------------
const int batteryPin     = D0;  // battery voltage divider
const int sensorPin      = D1;  // temperature sensor
const int moisturePin    = D2;  // soil moisture sensor
const int sensorPowerPin = D3;  // powers both sensors only while reading

// RF switch on the XIAO ESP32-C6 (not exposed on the header pins)
const int rfSwitchEnablePin = 3;   // LOW = RF switch on
const int antennaSelectPin  = 14;  // LOW = built-in antenna, HIGH = U.FL antenna

// ---------------- MQTT ----------------
const char* MQTT_TOPIC     = "garden/esp32/reading";
const char* MQTT_CLIENT_ID = "iot-garden-esp32";

// Name shown in Arduino IDE under Tools > Port while in update mode
const char* OTA_HOSTNAME   = "iot-garden-esp32";

// ---------------- Settings ----------------
const int NUM_SAMPLES = 10;                        // readings averaged per update
const unsigned long SENSOR_SETTLE_MS     = 100;    // wait after powering the sensors
const unsigned long WIFI_FAST_TIMEOUT_MS = 4000;   // reconnecting to the remembered router
const unsigned long WIFI_TIMEOUT_MS      = 10000;  // full scan and connect
const unsigned long MQTT_TIMEOUT_MS      = 4000;
const unsigned long MQTT_FLUSH_MS        = 100;    // lets the message leave before Wi-Fi stops
const unsigned long LONG_PRESS_MS        = 3000;   // hold the button this long for update mode

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

// What the board is doing. The service button switches between these.
enum Mode { MODE_NORMAL, MODE_TROUBLESHOOT, MODE_UPDATE };
enum Press { PRESS_NONE, PRESS_SHORT, PRESS_LONG };

Mode mode = MODE_NORMAL;
bool serialOn = false;                // USB serial started
bool usedServiceMode = false;         // this wake included a service mode
bool updateRunning = false;           // a wireless upload is in progress
unsigned long modeStartMs = 0;
unsigned long lastServiceReadingMs = 0;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// ---------------- Helpers ----------------

void logLine(const char* message) {
  if (serialOn) Serial.println(message);
}

void startSerial() {
  if (serialOn) return;
  Serial.begin(115200);
  serialOn = true;
}

// The orange LED next to the USB port. It lights when the pin is LOW.
void setLed(bool on) {
  digitalWrite(LED_BUILTIN, on ? LOW : HIGH);
}

// Pick the antenna set in calibration.h. Must run before Wi-Fi starts.
void selectAntenna() {
  pinMode(rfSwitchEnablePin, OUTPUT);
  digitalWrite(rfSwitchEnablePin, LOW);
  pinMode(antennaSelectPin, OUTPUT);
  digitalWrite(antennaSelectPin, USE_EXTERNAL_ANTENNA ? HIGH : LOW);
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

// Battery voltage in millivolts, or -1 when no divider is fitted
int readBatteryMv() {
  if (!BATTERY_DIVIDER_FITTED) return -1;
  return readAverageMv(batteryPin) * BATTERY_DIVIDER_RATIO;
}

// A nearly empty battery sags under Wi-Fi load and the board can restart
// over and over, so the radio stays off until it is recharged.
bool batteryCritical(int batteryMv) {
  return batteryMv > BATTERY_PRESENT_MV && batteryMv < BATTERY_CRITICAL_MV;
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

  r.batteryMv = readBatteryMv();
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

  if (batteryCritical(r.batteryMv)) {
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

  // Let the service button wake the board. Its pull-up has to stay powered
  // during sleep, or the pin would float and wake the board at random.
  if (BUTTON_FITTED) {
    while (buttonDown()) delay(10);   // a held button would wake it straight away
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    rtc_gpio_pullup_en((gpio_num_t)BUTTON_GPIO);
    rtc_gpio_pulldown_dis((gpio_num_t)BUTTON_GPIO);
    esp_sleep_enable_ext1_wakeup_io(1ULL << BUTTON_GPIO, ESP_EXT1_WAKEUP_ANY_LOW);
  }

  setLed(false);
  // Time spent in a service mode would spoil the battery figures
  lastAwakeMs = usedServiceMode ? 0 : millis();
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_start();
}

// ---------------- Service button ----------------

bool buttonDown() {
  return BUTTON_FITTED && digitalRead(BUTTON_GPIO) == LOW;
}

// Time a press that has already started. Returns once the button is released.
Press timePress() {
  unsigned long start = millis();
  while (buttonDown()) {
    if (millis() - start >= LONG_PRESS_MS) {
      setLed(true);                     // LED on: held long enough, let go now
      while (buttonDown()) delay(10);
      delay(50);
      return PRESS_LONG;
    }
    delay(10);
  }
  delay(50);                            // ignore contact bounce on release
  return PRESS_SHORT;
}

Press checkButton() {
  if (!buttonDown()) return PRESS_NONE;
  delay(30);                            // ignore glitches shorter than this
  if (!buttonDown()) return PRESS_NONE;
  return timePress();
}

void printWifiDetails() {
  if (!serialOn) return;
  Serial.print("Wi-Fi: address ");
  Serial.print(WiFi.localIP());
  Serial.print(", signal ");
  Serial.print(WiFi.RSSI());
  Serial.print(" dBm, channel ");
  Serial.println(WiFi.channel());
}

#ifdef OTA_PASSWORD
void startUpdateServer() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    updateRunning = true;
    logLine("Update starting");
  });
  ArduinoOTA.onEnd([]() {
    logLine("Update finished, restarting");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    updateRunning = false;
    modeStartMs = millis();             // allow a full timeout for another try
    logLine("Update failed. Try the upload again.");
  });
  ArduinoOTA.begin();
}
#endif

// Switch to troubleshooting or update mode. Both keep the board awake.
void enterMode(Mode next) {
  startSerial();
  usedServiceMode = true;

#ifndef OTA_PASSWORD
  if (next == MODE_UPDATE) {
    logLine("Update mode needs OTA_PASSWORD in secrets.h. Using troubleshooting mode.");
    next = MODE_TROUBLESHOOT;
  }
#endif

  // Update mode runs the processor at full speed, and that has to be set
  // while Wi-Fi is off.
  stopWifi();
  setCpuFrequencyMhz(next == MODE_UPDATE ? 160 : 80);

  mode = next;
  modeStartMs = millis();
  lastServiceReadingMs = 0;

  if (!connectWifi()) {
    logLine("Wi-Fi connection failed. Will keep trying.");
  }

  if (mode == MODE_UPDATE) {
    WiFi.setSleep(false);               // radio fully on, for a fast upload
#ifdef OTA_PASSWORD
    startUpdateServer();
#endif
    logLine("Update mode: in Arduino IDE choose Tools > Port > iot-garden-esp32, then upload.");
  } else {
    WiFi.setSleep(true);                // modem sleep: radio naps between router beacons
    logLine("Troubleshooting mode: awake, sending a reading every 10 seconds.");
    logLine("Hold the button 3 seconds for update mode, or press it to sleep.");
  }
  printWifiDetails();
}

// Leave a service mode. On battery this goes back to deep sleep.
void leaveServiceMode(const char* reason) {
  logLine(reason);
#ifdef OTA_PASSWORD
  if (mode == MODE_UPDATE) ArduinoOTA.end();
#endif
  stopWifi();
  setCpuFrequencyMhz(80);
  setLed(false);
  mode = MODE_NORMAL;

  if (!BENCH_MODE) {
    logLine("Going back to sleep");
    Serial.flush();
    deepSleep(SLEEP_INTERVAL_S);
  }
}

// One pass of troubleshooting or update mode. Called over and over from loop().
void serviceLoop() {
  unsigned long now = millis();

  // LED: slow blink = troubleshooting, fast blink = update
  unsigned long blinkMs = (mode == MODE_UPDATE) ? 150 : 1000;
  setLed((now / blinkMs) % 2 == 0);

  if (!updateRunning) {
    Press press = checkButton();
    if (press == PRESS_LONG && mode != MODE_UPDATE) {
      enterMode(MODE_UPDATE);
      return;
    }
    if (press != PRESS_NONE) {
      leaveServiceMode("Button pressed: leaving service mode");
      return;
    }
    if (now - modeStartMs >= SERVICE_TIMEOUT_S * 1000UL) {
      leaveServiceMode("Service mode timed out");
      return;
    }
  }

  if (mode == MODE_UPDATE) {
#ifdef OTA_PASSWORD
    ArduinoOTA.handle();
#endif
    delay(2);
    return;
  }

  // Troubleshooting: Wi-Fi stays connected, with a reading every few seconds
  if (lastServiceReadingMs == 0 || now - lastServiceReadingMs >= BENCH_INTERVAL_S * 1000UL) {
    lastServiceReadingMs = now;

    if (WiFi.status() != WL_CONNECTED) {
      logLine("Wi-Fi not connected, retrying");
      stopWifi();
      if (connectWifi()) WiFi.setSleep(true);
    }

    Reading r = readSensors();
    char payload[320];
    bool connected = WiFi.status() == WL_CONNECTED;
    formatPayload(payload, sizeof(payload), r, connected ? WiFi.RSSI() : 0);
    logLine(payload);
    if (connected) {
      logLine(publishReading(payload) ? "Published" : "MQTT publish failed");
      printWifiDetails();
    }
  }
  delay(10);
}

// Wait between readings in BENCH_MODE, watching the button
void benchWait() {
  unsigned long start = millis();
  while (millis() - start < BENCH_INTERVAL_S * 1000UL) {
    Press press = checkButton();
    if (press != PRESS_NONE) {
      enterMode(press == PRESS_LONG ? MODE_UPDATE : MODE_TROUBLESHOOT);
      return;
    }
    delay(10);
  }
}

// ---------------- Main ----------------

void setup() {
  setCpuFrequencyMhz(80);   // the slowest speed Wi-Fi allows; lowers current while awake

  if (BENCH_MODE) startSerial();

  // Release the pins that were held during deep sleep
  gpio_hold_dis((gpio_num_t)rfSwitchEnablePin);
  gpio_hold_dis((gpio_num_t)sensorPowerPin);

  pinMode(sensorPowerPin, OUTPUT);
  digitalWrite(sensorPowerPin, LOW);

  analogReadResolution(12);
  selectAntenna();
  logLine(USE_EXTERNAL_ANTENNA ? "Antenna: external (U.FL socket)" : "Antenna: built-in");

  pinMode(LED_BUILTIN, OUTPUT);
  setLed(false);

  // Service button: did it wake the board, or is it held down at power-on?
  if (BUTTON_FITTED) {
    bool wokeByButton = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1;
    rtc_gpio_deinit((gpio_num_t)BUTTON_GPIO);   // hand the pin back after sleep
    pinMode(BUTTON_GPIO, INPUT_PULLUP);
    delay(5);

    if (wokeByButton || buttonDown()) {
      Press press = timePress();
      if (batteryCritical(readBatteryMv())) {
        logLine("Battery critical: service modes are off until it is recharged");
      } else {
        enterMode(press == PRESS_LONG ? MODE_UPDATE : MODE_TROUBLESHOOT);
      }
    }
  }
}

void loop() {
  if (mode != MODE_NORMAL) {
    serviceLoop();
    return;
  }

  unsigned long sleepS = runCycle();

  if (BENCH_MODE) {
    lastAwakeMs = 0;   // not meaningful while the board never sleeps
    benchWait();
  } else {
    deepSleep(sleepS);
  }
}
