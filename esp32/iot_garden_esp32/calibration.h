// =====================================================================
//  IoT Garden calibration (XIAO ESP32-C6)
// =====================================================================
//  The raw readings are now in millivolts at 3.3 V, so the Arduino
//  moisture values do not carry over. Take new readings on this board.
//
//  Raw readings are shown on the dashboard under "Raw sensor readings",
//  and on the USB serial monitor in BENCH_MODE.
// =====================================================================

#pragma once

// ---------------- Timing ----------------
// true:  stays awake with USB serial on, for testing and uploading.
// false: deep sleeps between readings. Use this on battery.
const bool BENCH_MODE = true;
const unsigned long BENCH_INTERVAL_S = 10;   // time between readings in BENCH_MODE

// Time between readings on battery. This is the biggest battery setting:
// every reading costs one Wi-Fi connection. 600 = 10 minutes. The dashboard
// adjusts its "no data" warning to match automatically.
const unsigned long SLEEP_INTERVAL_S = 600;

// ---------------- Wi-Fi power (optional) ----------------
// Lower transmit power uses less battery, but a weak link fails and retries,
// which costs far more. Only enable this if the dashboard shows a Wi-Fi signal
// stronger than about -65 dBm, then check readings still arrive every time.
// Steps include WIFI_POWER_15dBm, WIFI_POWER_11dBm and WIFI_POWER_8_5dBm.
// #define WIFI_TX_POWER WIFI_POWER_15dBm

// ---------------- Battery level ----------------
// Set to true once two 200k resistors are fitted (BAT+ -> 200k -> D0 -> 200k
// -> GND). While false, the dashboard shows "Battery --%".
const bool BATTERY_DIVIDER_FITTED = false;
// Actual battery voltage / voltage at D0. Adjust to match a multimeter reading.
const float BATTERY_DIVIDER_RATIO = 2.0;
// Below this the board leaves Wi-Fi off and sleeps for an hour at a time,
// until the battery is recharged. Needs the divider fitted.
const int BATTERY_CRITICAL_MV = 3300;
// Readings below this mean no battery is connected (USB power only), so the
// critical check is skipped.
const int BATTERY_PRESENT_MV = 2000;

// ---------------- Sensor supply ----------------
// Voltage on pin D3 while the sensors are powered (millivolts). The pin sags a
// little under load, which shifts the temperature. Measure D3 to GND with a
// multimeter in BENCH_MODE while a reading is taken, and enter it here.
const float SENSOR_SUPPLY_MV = 3300.0;

// ---------------- Temperature ----------------
// Added to every temperature reading (degrees C). Carried over from the
// Arduino and not yet verified on this board.
// Put a thermometer next to the sensor and compare it with the dashboard:
//   Dashboard reads 10 C too high  ->  -10.0
//   Dashboard reads 10 C too low   ->   10.0
const float TEMP_OFFSET = 10.0;

// ---------------- Soil moisture sensor ----------------
// Raw readings in millivolts at each end of the scale. These are placeholders.
//   MOISTURE_DRY: probe clean and dry, held in the air
//   MOISTURE_WET: probe in a glass of water, up to the line on the probe
// The two values must be different. Either can be the larger one.
const int MOISTURE_DRY = 0;
const int MOISTURE_WET = 2300;

// ---------------- Watering thresholds ----------------
// Soil moisture % that decides the watering status.
// A practical way to set these for your herbs:
//   1. When the top 2 to 3 cm of soil feels dry, note the moisture %.
//      Use that as WATER_BELOW_PCT.
//   2. About an hour after a thorough watering, note the moisture %.
//      Set TOO_WET_ABOVE_PCT a little above that.
const int WATER_BELOW_PCT   = 30;  // below this: "Needs watering"
const int TOO_WET_ABOVE_PCT = 80;  // above this: "Too wet"

// Stops the status flickering when moisture sits right on a threshold.
// The status only changes back to "Okay" once moisture has moved this
// many % past the threshold.
const int MOISTURE_HYSTERESIS = 3;
