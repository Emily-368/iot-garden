# IoT Garden: project context for Claude Code

Emily's herb garden monitor. An Arduino Uno reads temperature and soil moisture
and streams JSON over USB to a Raspberry Pi 4B.
The Pi logs the data, publishes MQTT and serves a phone dashboard.

## Architecture

```
Sensors -> Arduino Uno --USB serial (9600, one JSON line/s)--> Raspberry Pi 4B
                                                                   |
                                             SQLite log (1 row/min, 30 days kept)
                                             MQTT (garden/state, HA auto-discovery)
                                             Flask dashboard on :5000
```

## Repo layout

| Path | Purpose |
|---|---|
| `arduino/iot_garden/iot_garden.ino` | Sketch. Folder name must match the .ino name (Arduino rule). |
| `arduino/iot_garden/calibration.h` | All calibration values and watering thresholds. Edit here, not in the sketch. |
| `esp32/iot_garden_esp32/` | Sketch for the planned XIAO ESP32-C6 move (Wi-Fi/MQTT, deep sleep). Not deployed yet. Set `GARDEN_INPUT=mqtt` on the Pi to switch the bridge to it. Passwords go in `secrets.h`, which Git ignores. |
| `IoT Garden/` | SwiftUI iOS app (Xcode project) that mirrors the web dashboard using `/api/latest` and `/api/history`. Default Pi address `http://ras-4:5000`, changeable in the app. `update.sh` ignores it. |
| `esp32/BATTERY_TESTING.md` | Procedure for measuring the ESP32's battery use once built. |
| `CHANGELOG.md` | Bugs fixed and improvements made, with the reasoning. Add to it when making notable changes. |
| `pi/garden_bridge.py` | Serial or MQTT reading input (`GARDEN_INPUT`, default `serial`), SQLite logger, MQTT publisher and dashboard (HTML/CSS/JS embedded in `DASHBOARD_HTML`). |
| `pi/garden-bridge.service` | systemd template. `CHANGE_ME_USER` / `CHANGE_ME_DIR` are filled in by `update.sh`. Don't hardcode them. |
| `pi/requirements.txt` | pyserial, paho-mqtt (2.x callback API), flask |
| `update.sh` | Run on the Pi: pulls, then only reinstalls/reflashes/restarts what changed. `--all` forces everything. |
| `.gitattributes` | Forces LF endings for code/scripts so files work on the Pi, including from any Windows checkout with `core.autocrlf=true`. |

## Hardware

- Arduino Uno (no LCD or LED; the dashboard is the only display)
- Keyestudio KS0033 analog temp sensor (10k NTC, Steinhart-Hart in `readTemperatureC()`) on A0
- Keyestudio resistive soil moisture sensor on A1

Pins:
```cpp
const int sensorPin = A0;    // temperature
const int moisturePin = A1;  // soil moisture
```

The dashboard header shows `Arduino` in serial mode and `ESP32: Battery N%` in MQTT mode (`DEVICE_NAME`).
Dashboard temperature panel colours: >= 30 C red, 24 to < 30 C green, < 24 C blue.
`HOT_THRESHOLD` / `COOL_THRESHOLD` live in `garden_bridge.py`.

## Serial protocol (Arduino -> Pi)

One JSON object per line, once a second:
```json
{"temp_c":25.3,"moisture_pct":45,"water":"ok","water_below":30,"wet_above":80,"temp_raw":512,"moisture_raw":400}
```
- `water` is `"dry"`, `"ok"` or `"wet"`, decided on the Arduino with hysteresis (`MOISTURE_HYSTERESIS`).
- The bridge ignores any line that isn't JSON with numeric `temp_c` and `moisture_pct`.
- The ESP32 sketch sends the same JSON to MQTT topic `garden/esp32/reading`, with raw values in millivolts, every `SLEEP_INTERVAL_S` (default 10 minutes). It adds `interval_s`, `awake_ms` (how long the previous wake lasted) and `rssi`, and with `BATTERY_DIVIDER_FITTED` also `battery_mv` and `battery_pct`. The bridge treats all of these as optional (`EXTRA_FIELDS`).
- The "no data" warning in MQTT mode is `interval_s * 2 + 300` seconds, so changing `SLEEP_INTERVAL_S` needs no bridge change.
- If the protocol changes, update `parse_line()`, the `reading` dict in `handle_reading()`, the dashboard JS and the HA discovery config together.

## Dashboard API (bridge)

- `GET /` dashboard (phone-first, light/dark, no external JS)
- `GET /api/latest` latest reading plus `age_s`, and the dashboard settings `device`, `stale_after_s`, `hot_threshold`, `cool_threshold` (used by the iOS app)
- `GET /api/history?hours=N` bucketed averages, at most ~400 points
- `GET /api/export.csv?hours=N` every logged row as CSV, including `battery_mv`, `rssi_dbm` and `awake_ms` (used by `esp32/BATTERY_TESTING.md`)

The dashboard has no authentication. Remote access is via Tailscale only; never suggest port-forwarding 5000.
Setup steps are in README "Remote access (Tailscale)": `tailscale up --ssh` on the Pi, key expiry disabled,
MagicDNS name `ras-4`, dashboard at `http://ras-4:5000` (plain HTTP; `https://ras-4:5000` fails) (optional `tailscale serve --bg 5000` for HTTPS).
Keep `WEB_HOST = "0.0.0.0"` so both LAN and tailnet access work.

## Deployment (current state)

- Pi hostname `ras-4` (`ras-4.local` on LAN, `ras-4` over Tailscale), user `raspi4`, now on Wi-Fi.
  Always use the hostname, not an IP; the IP changed when it moved from Ethernet to Wi-Fi.
- Repo: `github.com/Emily-368/iot-garden` (public, so never commit passwords or keys). Cloned on the Pi at `~/iot-garden`.
- Pi pulls over HTTPS with no key: `https://github.com/Emily-368/iot-garden.git`. This stops working if the repo is made private.
- `arduino-cli` is installed on the Pi in `~/.local/bin`; `update.sh` flashes the Uno over USB
  (FQBN `arduino:avr:uno`) after stopping the bridge to free the port.
- Pi-specific settings go in `/etc/default/garden-bridge` on the Pi (e.g. `GARDEN_SERIAL_PORT`,
  `GARDEN_MQTT_HOST/USER/PASS`), never in the repo.
- The ESP32 sketch has not been compiled with the real ESP32 core or run on hardware yet (no `arduino-cli` on the Mac). Expect small fixes on first build.
- Mosquitto runs locally on the Pi. For the ESP32 it needs a network listener and login; keep it LAN/tailnet only and never port-forward it. Home Assistant is not installed yet (planned option: Docker container,
  not HA OS). The bridge already sends HA MQTT auto-discovery for temperature, moisture and watering status.

## Workflow

1. Edit on Emily's Mac (repo in `~/Documents/IoT-Garden Monitor`). Pushes to GitHub over HTTPS using the macOS keychain login.
2. Commit and push to `main`.
3. On the Pi: `bash ~/iot-garden/update.sh` (from the Mac: `ssh raspi4@ras-4 'bash ~/iot-garden/update.sh'`, which needs Tailscale on the Mac)

Never edit files directly on the Pi; `git pull --ff-only` will refuse if the Pi tree is dirty.

## Known gotchas

- **Executable bit:** commits made from Windows can drop it on `update.sh`. Fix with
  `git update-index --chmod=+x update.sh`, or run it as `bash update.sh`.
- **Divergent branches:** happened once. Use `pull.rebase true` (set per repo on the Mac clone, as `git config --local pull.rebase true`). Never force-push.
- **Failed updates:** `update.sh` records the last fully applied commit in `.git/garden-applied`, so after a failure (e.g. during a flash) just run it again. `--all` still forces everything.
- **Uno unplugged:** with `GARDEN_INPUT=mqtt` in `/etc/default/garden-bridge`, `update.sh` skips flashing the Uno.
- **Serial port:** default `/dev/ttyACM0`; clone boards may be `/dev/ttyUSB0`. User must be in `dialout`.
- Opening the serial port resets the Uno on every bridge restart, so readings pause briefly. Expected.
- Use `mkdir -p` before installers that write into `~/.local/bin`.

## Calibration status / open items

- Temperature read about 10 C off. `TEMP_OFFSET` in `calibration.h` is the fix. The committed value is
  +10.0, but if the dashboard reads too high it should be -10.0; confirm the sign against a thermometer.
  Not yet verified that the error is constant across temperatures. If it isn't, refit the thermistor conversion from raw/thermometer pairs.
- `MOISTURE_DRY` / `MOISTURE_WET` are placeholders (0 / 950); take real readings from the dashboard's
  "Raw sensor readings" panel (dry air, glass of water).
- `WATER_BELOW_PCT` (30) / `TOO_WET_ABOVE_PCT` (80) are defaults; tune for the herbs.

## Verifying changes

- Python: `python3 -m py_compile pi/garden_bridge.py`; Flask test client against `/api/latest` and
  `/api/history` with a temp `GARDEN_DB`.
- Sketch: `arduino-cli compile --fqbn arduino:avr:uno arduino/iot_garden` (needs the AVR core).
- `bash -n update.sh`
