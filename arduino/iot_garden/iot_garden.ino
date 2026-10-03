/*
  IoT Garden Monitor
  ------------------
  - Reads Keyestudio KS0033 analog temperature sensor (10k NTC thermistor) on A0
  - Reads Keyestudio soil moisture sensor on A1
  - Displays temperature and moisture on a 16x2 LCD
  - Sets the RGB LED colour by temperature band:
        temp >= 30 C         -> RED
        24 C <= temp < 30 C  -> GREEN
        temp < 24 C          -> BLUE
  - Sends one JSON line per update over USB serial (9600 baud) for the
    Raspberry Pi bridge, e.g.
    {"temp_c":25.3,"moisture_pct":45,"temp_raw":512,"moisture_raw":400}
    (raw values are included so you can still calibrate from the Serial Monitor)
*/

#include <LiquidCrystal.h>
#include <math.h>

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

// Temperature thresholds in degrees C
const float HOT_THRESHOLD  = 30.0;   // at or above -> red
const float COOL_THRESHOLD = 24.0;   // below -> blue, otherwise green
const float TEMP_OFFSET = +10.0;     // Temperature sensor is 10 degree less than what should


// Moisture calibration (raw analog values).
// Read the Serial Monitor with the probe in dry air, then in a glass of water,
// and put those numbers here.
const int MOISTURE_DRY = 0;
const int MOISTURE_WET = 950;

const int NUM_SAMPLES = 10;                    // readings averaged per update
const unsigned long UPDATE_INTERVAL_MS = 1000; // screen/LED refresh rate

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

// Convert the moisture reading to 0-100 %
int readMoisturePercent(int &raw) {
  raw = readAverage(moisturePin);
  int percent = map(raw, MOISTURE_DRY, MOISTURE_WET, 0, 100);
  return constrain(percent, 0, 100);
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
    setColor(0, 255, 0);        // green - ideal
  } else {
    setColor(0, 0, 255);        // blue - cool
  }
}

// Write values to the LCD (trailing spaces clear leftover characters)
void updateLcd(float tempC, int moisturePct) {
  lcd.setCursor(0, 0);
  lcd.print("Temp:  ");
  lcd.print(tempC, 1);
  lcd.print((char)223);         // degree symbol
  lcd.print("C   ");

  lcd.setCursor(0, 1);
  lcd.print("Moist: ");
  lcd.print(moisturePct);
  lcd.print("%     ");
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

    updateLcd(tempC, moisturePct);
    updateLed(tempC);

    // One JSON line per update for the Raspberry Pi bridge
    Serial.print("{\"temp_c\":");
    Serial.print(tempC, 1);
    Serial.print(",\"moisture_pct\":");
    Serial.print(moisturePct);
    Serial.print(",\"temp_raw\":");
    Serial.print(tempRaw);
    Serial.print(",\"moisture_raw\":");
    Serial.print(moistureRaw);
    Serial.println("}");
  }
}
