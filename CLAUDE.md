# IoT Garden: project context for Claude Code

Emily's herb garden monitor. An Arduino Uno reads temperature and soil moisture,
shows them on an LCD and an RGB LED, and streams JSON over USB to a Raspberry Pi 4B.
The Pi logs the data, publishes MQTT and serves a phone dashboard.

## Architecture

```
Sensors -> Arduino Uno --USB serial (9600, one JSON line/s)--> Raspberry Pi 4B
                |                                                  |
           LCD + RGB LED                     SQLite log (1 row/min, 30 days kept)
                                             MQTT (garden/state, HA auto-discovery)
                                             Flask dashboard on :5000
```

## Repo layout

| Path | Purpose |
|---|---|
| `arduino/iot_garden/iot_garden.ino` | Sketch. Folder name must match the .ino name (Arduino rule). |
| `arduino/iot_garden/calibration.h` | All calibration values and watering thresholds. Edit here, not in the sketch. |
| `pi/garden_bridge.py` | Serial reader, SQLite logger, MQTT publisher and dashboard (HTML/CSS/JS embedded in `DASHBOARD_HTML`). |
| `pi/garden-bridge.service` | systemd template. `CHANGE_ME_USER` / `CHANGE_ME_DIR` are filled in by `update.sh`. Don't hardcode them. |
| `pi/requirements.txt` | pyserial, paho-mqtt (2.x callback API), flask |
| `update.sh` | Run on the Pi: pulls, then only reinstalls/reflashes/restarts what changed. `--all` forces everything. |
| `.gitattributes` | Forces LF endings for code/scripts so files work on the Pi (Emily's Git has `core.autocrlf=true`). |

## Hardware

- Arduino Uno, 16x2 parallel LCD, RGB LED (common cathode by default, `COMMON_ANODE` flag in sketch)
- Keyestudio KS0033 analog temp sensor (10k NTC, Steinhart-Hart in `readTemperatureC()`) on A0
- Keyestudio resistive soil moisture sensor on A1

Pins:
```cpp
const int rs = 12, en = 11, d4 = 5, d5 = 4, d6 = 3, d7 = 2;
const int sensorPin = A0;    // temperature
const int moisturePin = A1;  // soil moisture
const int redPin = 9, greenPin = 10, bluePin = 6;  // PWM
```

LED temperature bands: >= 30 C red, 24 to < 30 C green, < 24 C blue.
`HOT_THRESHOLD` / `COOL_THRESHOLD` live in both the sketch and `garden_bridge.py`; keep them in sync.

## Serial protocol (Arduino -> Pi)

One JSON object per line, once a second:
```json
{"temp_c":25.3,"moisture_pct":45,"water":"ok","water_below":30,"wet_above":80,"temp_raw":512,"moisture_raw":400}
```
- `water` is `"dry"`, `"ok"` or `"wet"`, decided on the Arduino with hysteresis (`MOISTURE_HYSTERESIS`).
- The bridge ignores any line that isn't JSON with numeric `temp_c` and `moisture_pct`.
- If the protocol changes, update `parse_line()`, the `reading` dict in `serial_loop()`, the dashboard JS and the HA discovery config together.

## Dashboard API (bridge)

- `GET /` dashboard (phone-first, light/dark, no external JS)
- `GET /api/latest` latest reading plus `age_s`
- `GET /api/history?hours=N` bucketed averages, at most ~400 points

The dashboard has no authentication. Remote access is via Tailscale only; never suggest port-forwarding 5000.
Setup steps are in README "Remote access (Tailscale)": `tailscale up --ssh` on the Pi, key expiry disabled,
MagicDNS name `ras-4`, dashboard at `http://ras-4:5000` (plain HTTP; `https://ras-4:5000` fails) (optional `tailscale serve --bg 5000` for HTTPS).
Keep `WEB_HOST = "0.0.0.0"` so both LAN and tailnet access work.

## Deployment (current state)

- Pi hostname `ras-4` (`ras-4.local` on LAN, `ras-4` over Tailscale), user `raspi4`, now on Wi-Fi.
  Always use the hostname, not an IP; the IP changed when it moved from Ethernet to Wi-Fi.
- Repo: `github.com/Emily-368/iot-garden` (private). Cloned on the Pi at `~/iot-garden`.
- Pi pulls with a read-only deploy key: `~/.ssh/iot_garden_deploy`, SSH host alias `github-iot-garden`.
- `arduino-cli` is installed on the Pi in `~/.local/bin`; `update.sh` flashes the Uno over USB
  (FQBN `arduino:avr:uno`) after stopping the bridge to free the port.
- Pi-specific settings go in `/etc/default/garden-bridge` on the Pi (e.g. `GARDEN_SERIAL_PORT`,
  `GARDEN_MQTT_HOST/USER/PASS`), never in the repo.
- Mosquitto runs locally on the Pi. Home Assistant is not installed yet (planned option: Docker container,
  not HA OS). The bridge already sends HA MQTT auto-discovery for temperature, moisture and watering status.

## Workflow

1. Edit on Emily's Windows PC (repo in `C:\Users\emily\OneDrive\Documents\Projects\iot-garden`, VS Code).
2. Commit and push to `main`.
3. On the Pi: `bash ~/iot-garden/update.sh`

Never edit files directly on the Pi; `git pull --ff-only` will refuse if the Pi tree is dirty.

## Known gotchas

- **Executable bit:** commits from Windows can drop it on `update.sh`. Fix with
  `git update-index --chmod=+x update.sh`, or run it as `bash update.sh`.
- **Divergent branches:** happened once. Emily's global git config now uses `pull.rebase true`. Never force-push.
- **OneDrive:** the repo lives in OneDrive; occasional lock/conflict-copy issues are possible.
- **Serial port:** default `/dev/ttyACM0`; clone boards may be `/dev/ttyUSB0`. User must be in `dialout`.
- Opening the serial port resets the Uno, so the LCD splash shows on every bridge restart. Expected.
- Use `mkdir -p` before installers that write into `~/.local/bin`.

## Calibration status / open items

- Temperature read about 10 C off. `TEMP_OFFSET` in `calibration.h` is the fix. The committed value is
  +10.0, but if the LCD read too high it should be -10.0; confirm the sign against a thermometer.
  Not yet verified that the error is constant across temperatures. If it isn't, refit the thermistor conversion from raw/thermometer pairs.
- `MOISTURE_DRY` / `MOISTURE_WET` are placeholders (0 / 950); take real readings from the dashboard's
  "Raw sensor readings" panel (dry air, glass of water).
- `WATER_BELOW_PCT` (30) / `TOO_WET_ABOVE_PCT` (80) are defaults; tune for the herbs.

## Verifying changes

- Python: `python3 -m py_compile pi/garden_bridge.py`; Flask test client against `/api/latest` and
  `/api/history` with a temp `GARDEN_DB`.
- Sketch: `arduino-cli compile --fqbn arduino:avr:uno arduino/iot_garden` (needs the AVR core and
  LiquidCrystal library).
- `bash -n update.sh`
