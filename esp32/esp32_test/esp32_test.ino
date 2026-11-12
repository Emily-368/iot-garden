/*
  XIAO ESP32-C6 bench test
  ------------------------
  A simple check that the board, both sensors and the Wi-Fi radio work, with
  the board plugged into a laptop over USB-C. It needs no Wi-Fi password, no
  Pi and no battery. It never sleeps, so the USB port stays available.

  What it does
    - Blinks the orange LED next to the USB port, so you can see it running
    - Lists the Wi-Fi networks it can see, once, at start-up
    - Prints both sensor readings to the serial monitor every 2 seconds

  Wiring (same as the main sketch, so nothing moves afterwards)

    Board pin   Temperature sensor (KS0033)   Soil sensor (KS0049)
    ---------   ---------------------------   --------------------
    D1          S (signal)                    -
    D2          -                             S (signal)
    D3          + / V (power)                 + / V (power)
    GND         - / G (ground)                - / G (ground)

    D3 is switched on by this sketch and supplies 3.3 V. The 3V3 pin also
    works for a quick test. Never power the sensors from the 5V pin: their
    signal could then exceed 3.3 V and damage the board.

  Setup
    Arduino IDE board: XIAO_ESP32C6 (esp32 by Espressif core, version 3.x)
    Tools > USB CDC On Boot: Enabled
    Serial monitor: 115200 baud
*/

#include <WiFi.h>
#include <math.h>

// ---------------- Pins ----------------
const int tempPin        = D1;  // temperature sensor signal
const int moisturePin    = D2;  // soil moisture sensor signal
const int sensorPowerPin = D3;  // 3.3 V supply for both sensors

// RF switch on the XIAO ESP32-C6 (not exposed on the header pins)
const int rfSwitchEnablePin = 3;   // LOW = RF switch on
const int antennaSelectPin  = 14;  // LOW = built-in antenna, HIGH = U.FL antenna

// ---------------- Settings ----------------
// Set to true once the external antenna is plugged into the U.FL socket.
const bool USE_EXTERNAL_ANTENNA = false;

const unsigned long PRINT_INTERVAL_MS = 2000;
const unsigned long BLINK_INTERVAL_MS = 500;
const int NUM_SAMPLES = 10;

unsigned long lastPrint = 0;
unsigned long lastBlink = 0;
bool ledOn = false;

// ---------------- Helpers ----------------

// Average several readings to smooth out noise, in millivolts
int readAverageMv(int pin) {
  analogReadMilliVolts(pin);   // discard the first reading after switching pins
  long total = 0;
  for (int i = 0; i < NUM_SAMPLES; i++) {
    total += analogReadMilliVolts(pin);
    delay(2);
  }
  return total / NUM_SAMPLES;
}

// Uncalibrated temperature from the thermistor (no TEMP_OFFSET applied)
float temperatureFromMv(int mv) {
  float ratio = constrain(mv / 3300.0f, 0.001f, 0.999f);
  double logR = log(10000.0 * (1.0 / ratio - 1.0));
  double kelvin = 1.0 / (0.001129148 +
                         (0.000234125 + 0.0000000876741 * logR * logR) * logR);
  return kelvin - 273.15;
}

// A reading stuck at either end usually means a loose or missing wire
const char* wiringHint(int mv) {
  if (mv < 50)   return "  <- near 0 V: check the signal and power wires";
  if (mv > 3200) return "  <- near 3.3 V: check the signal and ground wires";
  return "";
}

void scanWifi() {
  pinMode(rfSwitchEnablePin, OUTPUT);
  digitalWrite(rfSwitchEnablePin, LOW);
  pinMode(antennaSelectPin, OUTPUT);
  digitalWrite(antennaSelectPin, USE_EXTERNAL_ANTENNA ? HIGH : LOW);

  Serial.print("Scanning for Wi-Fi with the ");
  Serial.println(USE_EXTERNAL_ANTENNA ? "external antenna..." : "built-in antenna...");

  WiFi.mode(WIFI_STA);
  int found = WiFi.scanNetworks();
  if (found <= 0) {
    Serial.println("No networks found. Check the antenna setting and try again.");
  }
  for (int i = 0; i < found; i++) {
    Serial.print("  ");
    Serial.print(WiFi.RSSI(i));
    Serial.print(" dBm  ");
    Serial.println(WiFi.SSID(i));
  }
  Serial.println("Closer to zero is stronger: -55 is good, -80 is poor.");
  Serial.println();

  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);   // radio off, so it doesn't add noise to the sensor readings
}

// ---------------- Main ----------------

void setup() {
  Serial.begin(115200);
  delay(3000);   // time to open the serial monitor after plugging in

  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(sensorPowerPin, OUTPUT);
  digitalWrite(sensorPowerPin, HIGH);
  analogReadResolution(12);

  Serial.println();
  Serial.println("XIAO ESP32-C6 bench test");
  Serial.println("------------------------");
  scanWifi();
  Serial.println("Sensor readings every 2 seconds:");
}

void loop() {
  unsigned long now = millis();

  if (now - lastBlink >= BLINK_INTERVAL_MS) {
    lastBlink = now;
    ledOn = !ledOn;
    digitalWrite(LED_BUILTIN, ledOn ? LOW : HIGH);   // the LED lights when the pin is LOW
  }

  if (now - lastPrint >= PRINT_INTERVAL_MS) {
    lastPrint = now;

    int tempMv     = readAverageMv(tempPin);
    int moistureMv = readAverageMv(moisturePin);

    Serial.print("Temperature: ");
    Serial.print(tempMv);
    Serial.print(" mV (about ");
    Serial.print(temperatureFromMv(tempMv), 1);
    Serial.print(" C, uncalibrated)");
    Serial.println(wiringHint(tempMv));

    Serial.print("Moisture:    ");
    Serial.print(moistureMv);
    Serial.println(" mV");
  }
}
