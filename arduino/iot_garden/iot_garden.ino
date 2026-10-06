/*
  IoT Garden Monitor
  ------------------
  - Reads Keyestudio KS0033 analog temperature sensor (10k NTC thermistor) on A0
  - Reads Keyestudio soil moisture sensor on A1
  - Displays temperature, soil moisture and watering status on a 16x2 LCD
  - Sets the RGB LED colour by temperature band:
        temp >= 30 C         -> RED
        24 C <= temp < 30 C  -> GREEN
        temp < 24 C          -> BLUE
  - Sends one JSON line per update over USB serial (9600 baud) for the
    Raspberry Pi bridge, e.g.
    {"temp_c":25.3,"moisture_pct":45,"water":"ok","water_below":30,
     "wet_above":80,"temp_raw":512,"moisture_raw":400}

  Sensor calibration and watering thresholds are in calibration.h.
*/

#include <LiquidCrystal.h>
#include <math.h>
#include "calibration.h"

// ---------------- Pins ----------------
const int rs = 12, en = 11, d4 = 5, d5 = 4, d6 = 3, d7 = 2;
LiquidCrystal lcd(rs, en, d4, d5, d6, d7);

const int sensorPin   = A0;   // temperature sensor
const int moisturePin = A1;   // soil moisture sensor
const int redPin   = 9;       // PWM
const int greenPin = 10;      // PWM
const int bluePin  = 6;       // PWM

// ---------------- Settings ----------------
// Set to true if your RGB LED is common ANODE (common leg to 5V).
// Leave false for common CATHODE (common leg to GND).
const bool COMMON_ANODE = false;

// Temperature thresholds for the LED in degrees C
const float HOT_THRESHOLD  = 30.0;   // at or above -> red
const float COOL_THRESHOLD = 24.0;   // below -> blue, otherwise green

const int NUM_SAMPLES = 10;                    // readings averaged per update
const unsigned long UPDATE_INTERVAL_MS = 1000; // screen/LED refresh rate

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

// 5-character label for the LCD
const char* waterLcdLabel(int state) {
  if (state == WATER_NEEDED)  return "WATER";
  if (state == WATER_TOO_WET) return "WET  ";
  return "OK   ";
}

// Set LED colour (0-255 per channel)
void setColor(int r, int g, int b) {
  if (COMMON_ANODE) {
    r = 255 - r;
    g = 255 - g;
    b = 255 - b;
  }
  analogWrite(redPin, r);
  analogWrite(greenPin, g);
  analogWrite(bluePin, b);
}

// Pick LED colour from temperature
void updateLed(float tempC) {
  if (tempC >= HOT_THRESHOLD) {
    setColor(255, 0, 0);        // red - hot
  } else if (tempC >= COOL_THRESHOLD) {
    setColor(0, 0, 255);        // green - ideal
  } else {
    setColor(0, 255, 0);        // blue - cool
  }
}

// Write values to the LCD (trailing spaces clear leftover characters)
//   Line 1: "Temp:  25.3°C"
//   Line 2: "Soil: 45%  OK"  /  "Soil: 22%  WATER"
void updateLcd(float tempC, int moisturePct, int state) {
  lcd.setCursor(0, 0);
  lcd.print("Temp:  ");
  lcd.print(tempC, 1);
  lcd.print((char)223);         // degree symbol
  lcd.print("C   ");

  lcd.setCursor(0, 1);
  lcd.print("Soil: ");
  lcd.print(moisturePct);
  lcd.print("%     ");
  lcd.setCursor(11, 1);
  lcd.print(waterLcdLabel(state));
}

// ---------------- Main ----------------

void setup() {
  Serial.begin(9600);

  pinMode(redPin, OUTPUT);
  pinMode(greenPin, OUTPUT);
  pinMode(bluePin, OUTPUT);
  setColor(0, 0, 0);

  lcd.begin(16, 2);
  lcd.print("IoT Garden");
  lcd.setCursor(0, 1);
  lcd.print("Starting...");
  delay(1500);
  lcd.clear();
}

void loop() {
  if (millis() - lastUpdate >= UPDATE_INTERVAL_MS) {
    lastUpdate = millis();

    int tempRaw, moistureRaw;
    float tempC     = readTemperatureC(tempRaw);
    int moisturePct = readMoisturePercent(moistureRaw);
    int state       = waterState(moisturePct);

    updateLcd(tempC, moisturePct, state);
    updateLed(tempC);

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
