/*
  IoT Garden Monitor
  ------------------
  - Reads Keyestudio KS0033 analog temperature sensor (10k NTC thermistor) on A0
  - Reads Keyestudio soil moisture sensor on A1
  - Sends one JSON line per update over USB serial (9600 baud) for the
    Raspberry Pi bridge, e.g.
    {"temp_c":25.3,"moisture_pct":45,"water":"ok","water_below":30,
     "wet_above":80,"temp_raw":512,"moisture_raw":400}

  Sensor calibration and watering thresholds are in calibration.h.
*/

#include <Arduino.h>
#include <math.h>
#include "calibration.h"

// ---------------- Pins ----------------
const int sensorPin   = A0;   // temperature sensor
const int moisturePin = A1;   // soil moisture sensor

// ---------------- Settings ----------------
const int NUM_SAMPLES = 10;                    // readings averaged per update
const unsigned long UPDATE_INTERVAL_MS = 1000; // how often a reading is sent

// Watering states
const int WATER_OK      = 0;
const int WATER_NEEDED  = 1;
const int WATER_TOO_WET = 2;

unsigned long lastUpdate = 0;

// ---------------- Helpers ----------------

// Average several readings to smooth out noise
int readAverage(int pin) {
  long total = 0;
  for (int i = 0; i < NUM_SAMPLES; i++) {
    total += analogRead(pin);
    delay(5);
  }
  return total / NUM_SAMPLES;
}

// Convert the KS0033 thermistor reading to Celsius (Steinhart-Hart equation)
float readTemperatureC(int &raw) {
  raw = readAverage(sensorPin);
  int r = constrain(raw, 1, 1022);              // avoid divide-by-zero / log(0)
  double resistance = 10000.0 * (1024.0 / r - 1.0);
  double logR = log(resistance);
  double kelvin = 1.0 / (0.001129148 +
                         (0.000234125 + 0.0000000876741 * logR * logR) * logR);
  return kelvin - 273.15 + TEMP_OFFSET;
}

// Convert the moisture reading to 0-100 % using the calibration values
int readMoisturePercent(int &raw) {
  raw = readAverage(moisturePin);
  if (MOISTURE_WET == MOISTURE_DRY) return 0;   // calibration not valid
  int percent = map(raw, MOISTURE_DRY, MOISTURE_WET, 0, 100);
  return constrain(percent, 0, 100);
}

// Decide the watering status, with hysteresis so it doesn't flicker
int waterState(int moisturePct) {
  static int state = WATER_OK;
  if (moisturePct < WATER_BELOW_PCT) {
    state = WATER_NEEDED;
  } else if (moisturePct > TOO_WET_ABOVE_PCT) {
    state = WATER_TOO_WET;
  } else if (state == WATER_NEEDED &&
             moisturePct >= WATER_BELOW_PCT + MOISTURE_HYSTERESIS) {
    state = WATER_OK;
  } else if (state == WATER_TOO_WET &&
             moisturePct <= TOO_WET_ABOVE_PCT - MOISTURE_HYSTERESIS) {
    state = WATER_OK;
  }
  return state;
}

// Short code sent to the Pi
const char* waterCode(int state) {
  if (state == WATER_NEEDED)  return "dry";
  if (state == WATER_TOO_WET) return "wet";
  return "ok";
}

// ---------------- Main ----------------

void setup() {
  Serial.begin(9600);
}

void loop() {
  if (millis() - lastUpdate >= UPDATE_INTERVAL_MS) {
    lastUpdate = millis();

    int tempRaw, moistureRaw;
    float tempC     = readTemperatureC(tempRaw);
    int moisturePct = readMoisturePercent(moistureRaw);
    int state       = waterState(moisturePct);

    // One JSON line per update for the Raspberry Pi bridge
    Serial.print("{\"temp_c\":");
    Serial.print(tempC, 1);
    Serial.print(",\"moisture_pct\":");
    Serial.print(moisturePct);
    Serial.print(",\"water\":\"");
    Serial.print(waterCode(state));
    Serial.print("\",\"water_below\":");
    Serial.print(WATER_BELOW_PCT);
    Serial.print(",\"wet_above\":");
    Serial.print(TOO_WET_ABOVE_PCT);
    Serial.print(",\"temp_raw\":");
    Serial.print(tempRaw);
    Serial.print(",\"moisture_raw\":");
    Serial.print(moistureRaw);
    Serial.println("}");
  }
}
