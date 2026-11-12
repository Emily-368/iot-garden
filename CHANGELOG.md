# Changelog

Bugs fixed and improvements made, with the reasoning behind each one.

## 12 November 2026: ESP32 service button

Added an optional push button to the ESP32 sketch (`BUTTON_FITTED` and
`BUTTON_GPIO` in `calibration.h`), wired between the MTCK pad (GPIO6) and GND.

- **Short press: troubleshooting mode.** Wakes the board from deep sleep and
  keeps it awake with Wi-Fi connected in modem sleep, sending a reading every
  10 seconds and logging to USB serial. Reason: in deep sleep the board can't
  be reached or watched, which made faults hard to diagnose.
- **Hold 3 seconds: update mode.** Wi-Fi at full power with wireless (OTA)
  upload from Arduino IDE. Needs `OTA_PASSWORD` in `secrets.h`. Reason: avoids
  the BOOT/RESET routine and opening the housing to upload.
- Both modes end after `SERVICE_TIMEOUT_S` (10 minutes) or another press, and
  are refused when the battery is critical, to protect the battery.
- The button is on GPIO6 because only GPIO 0 to 7 can wake this chip from deep
  sleep, and D0 to D2 are taken by the battery divider and sensors.

Checked: compiled with the real ESP32 core (3.3.12), with and without
`OTA_PASSWORD`. **Not yet run on the board**, and the sleep current with the
button's pull-up kept on has not been measured.

## 11 November 2026: code review and ESP32 battery work

A review of the whole project: the Pi bridge, the web dashboard, `update.sh`,
both sketches and the iOS app.

### How these changes were checked

- **Bridge:** tested with stand-in versions of Flask, pyserial and paho-mqtt,
  because they aren't installed on the Mac. Not yet run on the Pi.
- **Web dashboard:** rendered in a browser with test data.
- **iOS app:** built and run in the iPhone 17 simulator with test data.
- **`update.sh`:** run against a throwaway Git repository, including a failed
  flash.
- **ESP32 sketch:** run on the Mac as a simulation of wake cycles, with
  stand-ins for the Wi-Fi, MQTT and ESP32 libraries. It has **not** been
  compiled with the real ESP32 core or run on a board, so expect small fixes on
  the first real build. Timings in the simulation are made up; only the logic
  was tested.
- **Arduino sketch:** syntax-checked only. Its first real compile happens on the
  Pi during `update.sh`.

### Bugs fixed

#### Pi bridge (`pi/garden_bridge.py`)

1. **Dashboard said "No readings yet" after every bridge restart.**
   The latest reading was only kept in memory, so a restart or update wiped it.
   With the Uno that lasted a second. With the ESP32 it would last up to 10
   minutes and look like a broken sensor.
   *Fix:* the latest reading is saved to the database with each logged row and
   restored on start-up, with its real age. An old reading still shows as old.

2. **The "no data" warning assumed the ESP32 always sends every 10 minutes.**
   The limit was fixed at 25 minutes. Changing `SLEEP_INTERVAL_S` to save
   battery would have made the dashboard warn constantly, or hide a real fault.
   *Fix:* the ESP32 now reports its interval, and the limit is twice that plus
   5 minutes (still 25 minutes at the default). The web page and the iOS app
   both use the value from the bridge.

3. **A clock change on the Pi could make a fresh reading look hours old.**
   The Pi has no battery-backed clock, so its time can jump forward after
   booting once it syncs. Age was worked out from the wall clock.
   *Fix:* age is now measured with a clock that only counts forward.

4. **Home Assistant always described the device as an Arduino Uno.**
   *Fix:* the description follows the board in use.

5. **Old rows were deleted on every database write.**
   Not wrong, but it meant two writes to the SD card every minute instead of
   one. *Fix:* the insert and the saved latest reading share one write, and old
   rows are pruned once an hour.

#### `update.sh`

6. **A failed update looked finished on the next run.**
   If the script failed after pulling (for example while flashing), the next
   run compared the new code with itself, printed "Already up to date" and
   skipped the flash and restart.
   *Fix:* the script records the last commit that was fully applied and
   compares against that, so running it again retries what failed.

7. **Updates would fail once the Uno is unplugged.**
   Any change under `arduino/` made the script try to flash a board that will
   no longer be there after the move to the ESP32.
   *Fix:* flashing is skipped when the Pi is set to `GARDEN_INPUT=mqtt`.

#### Arduino sketch (`arduino/iot_garden/iot_garden.ino`)

8. **The first analog reading after switching pins can be off.**
   The Uno has one converter shared between pins, and the sketch alternates
   between the temperature and moisture pins. The first sample after a switch
   can carry over some of the previous pin's voltage, especially from a
   high-resistance sensor.
   *Fix:* one reading is discarded before each average. This is a precaution.
   It is not confirmed to be the cause of the temperature dips on the chart; a
   loose wire on A0 is still the more likely cause of those.

#### ESP32 sketch (`esp32/iot_garden_esp32/`)

9. **An unreachable router or Pi would drain the battery about four times
   faster.** Every wake waited the full 15 seconds for Wi-Fi with the radio on,
   then tried again 10 minutes later, indefinitely.
   *Fix:* shorter timeouts, and after 3 failed wakes in a row each sleep
   doubles, up to an hour.

10. **A nearly flat battery could cause a restart loop.**
    Wi-Fi draws short bursts of high current. A low battery sags under that
    load, the board resets, and it tries again straight away.
    *Fix:* below 3.3 V the board leaves Wi-Fi off and sleeps for an hour at a
    time. This needs the battery divider fitted.

11. **Pins were left floating during deep sleep.**
    The RF switch and the sensor power pin weren't held in a known state, which
    can leak current.
    *Fix:* the RF switch is turned off and both pins are held during sleep,
    then released on wake. Whether this lowers the sleep current on this board
    needs measuring (Test 2 in the battery procedure).

12. **The temperature maths assumed the sensor supply is exactly 3.3 V.**
    The sensors are powered from a pin, which sags a little under load, and
    that shifts the temperature.
    *Fix:* `SENSOR_SUPPLY_MV` is now a calibration value to measure and enter.

13. **The MQTT message was close to the library's size limit.**
    PubSubClient drops messages over 256 bytes without an error. The old
    message fitted, but the new fields bring it to about 225 bytes with the
    topic. *Fix:* the limit is raised to 512.

### ESP32 battery optimisations

Almost all the battery goes on Wi-Fi time, so the aim is the shortest possible
wake. The figures below are estimates until measured with
`esp32/BATTERY_TESTING.md`.

| Change | Why |
|---|---|
| Remember the router's channel and address between wakes | Skips the Wi-Fi scan, usually the longest part of connecting. Falls back to a full scan if the router changes |
| Optional fixed IP address (`secrets.h`) | Skips asking the router for an address on every wake |
| Processor at 80 MHz | The slowest speed Wi-Fi allows; lowers current for the whole wake |
| Wi-Fi timeouts cut from 15 s to 4 s (fast) and 10 s (scan) | A failed connection costs less |
| Shorter waits between analog samples and connection checks | Small savings on every wake |
| Sensors read before Wi-Fi starts | Kept on purpose: radio bursts add noise to analog readings, and the battery reads truest under light load |
| Optional lower transmit power (`WIFI_TX_POWER`) | Off by default, because a weak link that retries costs far more than it saves |
| Backoff and low-battery sleep | See bugs 9 and 10 |

**Not changed:** `SLEEP_INTERVAL_S` stays at 10 minutes. It has the biggest
effect of any setting, but how often you want readings is your call. Fifteen
minutes would use about a third less.

**New in each message:** `interval_s`, `awake_ms` (how long the previous wake
lasted) and `rssi` (Wi-Fi signal). These make it possible to measure battery
use without special equipment.

### Dashboard improvements

| Change | Where | Why |
|---|---|---|
| Polling stops while the page is hidden, and refreshes when it returns | Web | A phone with the dashboard open in the background was fetching every 5 seconds for nothing |
| Battery (mV), Wi-Fi signal and last wake time under "Raw sensor readings" | Web and iOS | Needed for the battery tests. Hidden when the board doesn't send them, so the Uno view is unchanged |
| `GET /api/export.csv?hours=N` | Bridge | Downloads every logged reading, including battery voltage, for the rundown test or a spreadsheet |
| Battery voltage, signal and wake time logged to the database | Bridge | So battery drain can be charted over days. Existing databases are upgraded in place |

### New files

- `esp32/BATTERY_TESTING.md`: step-by-step battery test procedure.
- `CHANGELOG.md`: this file.

### Found but not changed

- **Temperature dips on the chart** (sudden drops to about -7 °C): most likely
  a loose connection on A0. Check the wiring.
- **Soil moisture at 0 %:** `MOISTURE_DRY` and `MOISTURE_WET` are still
  placeholders, so the watering status isn't reliable yet.
- **`TEMP_OFFSET` sign** is still unconfirmed against a thermometer, on both
  boards.
- **Implausible readings are still shown.** A disconnected temperature sensor
  reads as roughly -70 °C or +280 °C. The bridge could reject values like that,
  but it would also discard the moisture reading that came with them.
- **Charts draw a straight line across gaps** when the sensor or Pi was off,
  which can look like real data.
- **Resistive soil probes corrode** when powered all the time, as on the Uno.
  The ESP32 sketch only powers the probe while reading.
- **`To Do List.md` says ESP-32-C3**; the board ordered is the ESP32-C6.
