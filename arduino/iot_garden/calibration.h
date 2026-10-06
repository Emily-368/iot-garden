// =====================================================================
//  IoT Garden calibration
// =====================================================================
//  Enter your calibration values here. To apply them: commit, push, then
//  run update.sh on the Pi, which reflashes the Arduino.
//
//  Raw readings for calibration are shown on the dashboard under
//  "Raw sensor readings" at the bottom of the page.
// =====================================================================

#pragma once

// ---------------- Temperature ----------------
// Added to every temperature reading (degrees C).
// Put a thermometer next to the sensor and compare it with the LCD:
//   LCD reads 10 C too high  ->  -10.0
//   LCD reads 10 C too low   ->   10.0
const float TEMP_OFFSET = 10.0;

// ---------------- Soil moisture sensor ----------------
// Raw readings at each end of the scale (0 to 1023).
//   MOISTURE_DRY: probe clean and dry, held in the air
//   MOISTURE_WET: probe in a glass of water, up to the line on the probe
// The two values must be different. Either can be the larger one.
const int MOISTURE_DRY = 0;
const int MOISTURE_WET = 950;

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
