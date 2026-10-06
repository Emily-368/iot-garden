#!/usr/bin/env python3
"""
IoT Garden bridge for Raspberry Pi
==================================

Reads JSON lines from the Arduino over USB serial, e.g.
    {"temp_c":25.3,"moisture_pct":45,"water":"ok","water_below":30,
     "wet_above":80,"temp_raw":512,"moisture_raw":400}

Watering status ("dry", "ok", "wet") and its thresholds come from the Arduino,
set in arduino/iot_garden/calibration.h, so there is one place to calibrate.

and then:
  - keeps the latest reading in memory
  - logs one reading per minute to SQLite (garden.db next to this script)
  - publishes to MQTT (topic garden/state) with Home Assistant auto-discovery
  - serves a phone-friendly dashboard at http://<pi-address>:5000

Settings can be overridden with environment variables (see CONFIG).
"""

import json
import logging
import os
import sqlite3
import threading
import time
from pathlib import Path

import serial
from flask import Flask, Response, jsonify, request

try:
    import paho.mqtt.client as mqtt
except ImportError:  # MQTT is optional
    mqtt = None

# ---------------- CONFIG ----------------
SERIAL_PORT = os.environ.get("GARDEN_SERIAL_PORT", "/dev/ttyACM0")
SERIAL_BAUD = int(os.environ.get("GARDEN_SERIAL_BAUD", "9600"))

DB_PATH = Path(os.environ.get("GARDEN_DB", Path(__file__).resolve().parent / "garden.db"))
LOG_INTERVAL_S = 60      # how often a reading is saved to the database
KEEP_DAYS = 30           # rows older than this are deleted

MQTT_ENABLED = os.environ.get("GARDEN_MQTT", "1") == "1"
MQTT_HOST = os.environ.get("GARDEN_MQTT_HOST", "localhost")
MQTT_PORT = int(os.environ.get("GARDEN_MQTT_PORT", "1883"))
MQTT_USER = os.environ.get("GARDEN_MQTT_USER")
MQTT_PASS = os.environ.get("GARDEN_MQTT_PASS")
MQTT_BASE = "garden"
MQTT_INTERVAL_S = 10     # how often the state is published
HA_DISCOVERY_PREFIX = "homeassistant"

WEB_HOST = "0.0.0.0"
WEB_PORT = int(os.environ.get("GARDEN_WEB_PORT", "5000"))

# Keep these matching the Arduino sketch so the dashboard agrees with the LED
HOT_THRESHOLD = 30.0
COOL_THRESHOLD = 24.0

STALE_AFTER_S = 30       # dashboard warns if no data for this long
# ----------------------------------------

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("garden")

WATER_LABELS = {"dry": "Needs watering", "ok": "Okay", "wet": "Too wet"}

latest = {}
latest_lock = threading.Lock()


# ---------------- Database ----------------
def db_execute(sql, params=(), fetch=False):
    conn = sqlite3.connect(DB_PATH, timeout=10)
    try:
        with conn:
            cur = conn.execute(sql, params)
            return cur.fetchall() if fetch else None
    finally:
        conn.close()


def db_init():
    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(DB_PATH)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.close()
    db_execute(
        "CREATE TABLE IF NOT EXISTS readings ("
        " ts INTEGER NOT NULL, temp_c REAL, moisture_pct REAL)"
    )
    db_execute("CREATE INDEX IF NOT EXISTS idx_readings_ts ON readings(ts)")


def db_insert(reading):
    db_execute(
        "INSERT INTO readings (ts, temp_c, moisture_pct) VALUES (?, ?, ?)",
        (reading["ts"], reading["temp_c"], reading["moisture_pct"]),
    )
    db_execute("DELETE FROM readings WHERE ts < ?", (int(time.time()) - KEEP_DAYS * 86400,))


# ---------------- MQTT ----------------
class MqttPublisher:
    def __init__(self):
        self.client = None
        if not MQTT_ENABLED:
            log.info("MQTT disabled")
            return
        if mqtt is None:
            log.warning("paho-mqtt not installed; MQTT disabled")
            return

        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="iot-garden-bridge")
        if MQTT_USER:
            self.client.username_pw_set(MQTT_USER, MQTT_PASS)
        self.client.will_set(f"{MQTT_BASE}/status", "offline", retain=True)
        self.client.on_connect = self._on_connect
        self.client.reconnect_delay_set(min_delay=1, max_delay=60)
        self.client.connect_async(MQTT_HOST, MQTT_PORT, keepalive=60)
        self.client.loop_start()

    def _on_connect(self, client, userdata, flags, reason_code, properties):
        if reason_code.is_failure:
            log.warning("MQTT connection refused: %s", reason_code)
            return
        log.info("MQTT connected to %s:%s", MQTT_HOST, MQTT_PORT)
        client.publish(f"{MQTT_BASE}/status", "online", retain=True)
        self._publish_discovery(client)

    def _publish_discovery(self, client):
        """Lets Home Assistant create the sensors automatically (ignored if you don't use HA)."""
        device = {
            "identifiers": ["iot_garden"],
            "name": "IoT Garden",
            "model": "Arduino Uno + Raspberry Pi 4B",
        }
        sensors = [
            ("temperature", "temp_c", {"name": "Temperature", "unit_of_measurement": "°C",
                                       "device_class": "temperature", "state_class": "measurement"}),
            ("moisture", "moisture_pct", {"name": "Soil moisture", "unit_of_measurement": "%",
                                          "device_class": "moisture", "state_class": "measurement"}),
            ("watering", "water_status", {"name": "Watering", "icon": "mdi:watering-can"}),
        ]
        for key, field, extra in sensors:
            config = {
                **extra,
                "unique_id": f"iot_garden_{key}",
                "state_topic": f"{MQTT_BASE}/state",
                "value_template": f"{{{{ value_json.{field} }}}}",
                "availability_topic": f"{MQTT_BASE}/status",
                "device": device,
            }
            client.publish(
                f"{HA_DISCOVERY_PREFIX}/sensor/iot_garden/{key}/config",
                json.dumps(config),
                retain=True,
            )

    def publish_state(self, reading):
        if self.client is None or not self.client.is_connected():
            return
        self.client.publish(f"{MQTT_BASE}/state", json.dumps(reading), retain=True)


# ---------------- Serial ----------------
def parse_line(line):
    """Return a dict if the line is a valid reading from the Arduino, else None."""
    if not line.startswith("{"):
        return None
    try:
        data = json.loads(line)
    except json.JSONDecodeError:
        return None
    if not all(isinstance(data.get(k), (int, float)) for k in ("temp_c", "moisture_pct")):
        return None
    return data


def serial_loop(publisher):
    last_logged = 0
    last_published = 0
    while True:
        try:
            log.info("Opening serial port %s", SERIAL_PORT)
            with serial.Serial(SERIAL_PORT, SERIAL_BAUD, timeout=5) as ser:
                while True:
                    raw = ser.readline()
                    if not raw:
                        continue
                    data = parse_line(raw.decode("utf-8", errors="ignore").strip())
                    if data is None:
                        continue

                    now = int(time.time())
                    water = data.get("water")
                    if water not in WATER_LABELS:
                        water = None   # older sketch without watering status
                    reading = {
                        "ts": now,
                        "temp_c": round(float(data["temp_c"]), 1),
                        "moisture_pct": round(float(data["moisture_pct"])),
                        "water": water,
                        "water_status": WATER_LABELS.get(water, "Unknown"),
                        "water_below": data.get("water_below"),
                        "wet_above": data.get("wet_above"),
                        "temp_raw": data.get("temp_raw"),
                        "moisture_raw": data.get("moisture_raw"),
                    }
                    with latest_lock:
                        latest.clear()
                        latest.update(reading)

                    if now - last_logged >= LOG_INTERVAL_S:
                        try:
                            db_insert(reading)
                            last_logged = now
                        except sqlite3.Error as exc:
                            log.warning("Database write failed: %s", exc)

                    if now - last_published >= MQTT_INTERVAL_S:
                        publisher.publish_state(reading)
                        last_published = now

        except serial.SerialException as exc:
            log.warning("Serial error: %s (retrying in 5 s)", exc)
            time.sleep(5)
        except Exception:
            log.exception("Unexpected error in serial loop (retrying in 5 s)")
            time.sleep(5)


# ---------------- Web dashboard ----------------
app = Flask(__name__)


@app.get("/")
def index():
    return Response(DASHBOARD_HTML, mimetype="text/html")


@app.get("/api/latest")
def api_latest():
    with latest_lock:
        data = dict(latest)
    if data:
        data["age_s"] = int(time.time()) - data["ts"]
    return jsonify(data)


@app.get("/api/history")
def api_history():
    hours = request.args.get("hours", default=24, type=int)
    hours = max(1, min(hours, KEEP_DAYS * 24))
    since = int(time.time()) - hours * 3600
    bucket = max(LOG_INTERVAL_S, hours * 3600 // 400)   # at most ~400 points per chart
    rows = db_execute(
        "SELECT (ts / ?) * ? AS b, ROUND(AVG(temp_c), 1), ROUND(AVG(moisture_pct), 1) "
        "FROM readings WHERE ts >= ? GROUP BY b ORDER BY b",
        (bucket, bucket, since),
        fetch=True,
    )
    return jsonify({"hours": hours, "points": rows})


DASHBOARD_HTML = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#e9eee6" media="(prefers-color-scheme: light)">
<meta name="theme-color" content="#121612" media="(prefers-color-scheme: dark)">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-title" content="Garden">
<title>IoT Garden</title>
<link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><text y='.9em' font-size='90'>🌱</text></svg>">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Atkinson+Hyperlegible:wght@400;700&display=swap" rel="stylesheet">
<style>
:root {
  --bg: #e9eee6; --panel: #f8faf6; --ink: #1c261c; --muted: #5a6657; --grid: #d3dbcd;
  --hot: #c23b2b; --ideal: #2e8547; --cool: #2d5fb3;
  --soil: #7a5a3f; --water: #2b7889; --temp-line: #b5562a; --warn: #9a6512; --dry: #b8650f;
  --on-band: #ffffff;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #121612; --panel: #1b211b; --ink: #e5eae1; --muted: #93a08f; --grid: #2b342b;
    --hot: #d9503f; --ideal: #3a9a57; --cool: #4073c7;
    --soil: #a07d5e; --water: #4aaabd; --temp-line: #e0834f; --warn: #e2a74a; --dry: #e8963c;
  }
}
* { box-sizing: border-box; }
html { height: 100%; }
body {
  margin: 0; min-height: 100%; background: var(--bg); color: var(--ink);
  font: 17px/1.45 "Atkinson Hyperlegible", system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
  padding: max(18px, env(safe-area-inset-top)) 16px max(28px, env(safe-area-inset-bottom));
}
main { max-width: 540px; margin: 0 auto; }

header { display: flex; justify-content: space-between; align-items: baseline; gap: 12px; margin-bottom: 14px; }
h1 { font-size: 1.2rem; margin: 0; }
#status { font-size: .85rem; color: var(--muted); text-align: right; }
#status.stale { color: var(--warn); font-weight: 700; }

/* The one bold element: a panel lit in the same colour as the garden LED */
#led {
  background: var(--muted); color: var(--on-band);
  border-radius: 22px; padding: 22px 22px 20px;
  transition: background-color .8s ease;
}
#led .reading { font-size: 4.6rem; font-weight: 700; line-height: 1; letter-spacing: -0.03em; font-variant-numeric: tabular-nums; }
#led .reading span { font-size: 1.6rem; font-weight: 400; letter-spacing: 0; margin-left: 4px; vertical-align: .9em; opacity: .85; }
#led .band { margin-top: 10px; font-size: 1rem; opacity: .95; }

.moisture { margin-top: 22px; }
.moisture .row { display: flex; justify-content: space-between; align-items: baseline; }
.moisture .row strong { font-size: 1.9rem; font-variant-numeric: tabular-nums; }
.moisture .row strong span { font-size: 1rem; font-weight: 400; color: var(--muted); margin-left: 2px; }
.soil {
  position: relative; height: 18px; margin-top: 8px; border-radius: 9px; overflow: hidden;
  background: repeating-linear-gradient(135deg, var(--soil) 0 6px, color-mix(in srgb, var(--soil) 80%, var(--bg)) 6px 12px);
  opacity: .9;
}
.soil > div { position: absolute; inset: 0 auto 0 0; width: 0; background: var(--water); transition: width .8s ease; }
.soil > i { position: absolute; top: 0; bottom: 0; width: 2px; margin-left: -1px; background: #fff; opacity: .85; display: none; }
.scale { position: relative; height: 1.2em; font-size: .75rem; color: var(--muted); }
.scale span { position: absolute; transform: translateX(-50%); white-space: nowrap; }
.hint { font-size: .85rem; color: var(--muted); margin-top: 6px; }

/* Watering status */
.water {
  --state: var(--muted);
  margin-top: 10px; padding: 12px 14px; border-radius: 12px;
  border-left: 6px solid var(--state);
  background: color-mix(in srgb, var(--state) 14%, var(--panel));
}
.water[data-state="dry"] { --state: var(--dry); }
.water[data-state="ok"]  { --state: var(--ideal); }
.water[data-state="wet"] { --state: var(--water); }
.water strong { display: block; font-size: 1.2rem; }
.water span { display: block; font-size: .9rem; color: var(--muted); margin-top: 2px; }
.water[hidden] { display: none; }

.raw { margin-top: 26px; font-size: .9rem; color: var(--muted); }
.raw summary { cursor: pointer; }
.raw summary:focus-visible { outline: 2px solid var(--ink); outline-offset: 2px; }
.raw dl { display: grid; grid-template-columns: auto 1fr; gap: 4px 14px; margin: 10px 0 6px; }
.raw dt { color: var(--muted); }
.raw dd { margin: 0; color: var(--ink); font-variant-numeric: tabular-nums; font-weight: 700; }

.history { margin-top: 30px; }
.history header { margin-bottom: 8px; }
h2 { font-size: 1.05rem; margin: 0; }
.range { display: flex; gap: 2px; background: var(--panel); padding: 3px; border-radius: 10px; }
.range button {
  border: 0; background: none; color: var(--muted); font: inherit; font-size: .9rem;
  padding: 5px 12px; border-radius: 8px; cursor: pointer;
}
.range button[aria-pressed="true"] { background: var(--bg); color: var(--ink); font-weight: 700; }
.range button:focus-visible { outline: 2px solid var(--ink); outline-offset: 1px; }
figure { margin: 10px 0 0; background: var(--panel); border-radius: 14px; padding: 12px 12px 8px; }
figcaption { font-size: .9rem; color: var(--muted); margin-bottom: 4px; }
canvas { width: 100%; height: 150px; display: block; }

@media (prefers-reduced-motion: reduce) {
  #led, .soil > div { transition: none; }
}
</style>
</head>
<body>
<main>
  <header>
    <h1>🌱 IoT Garden</h1>
    <span id="status" role="status">Connecting to the Pi…</span>
  </header>

  <section id="led" aria-label="Temperature">
    <div class="reading"><b id="temp">--</b><span>°C</span></div>
    <div class="band" id="band">Waiting for the first reading</div>
  </section>

  <section class="moisture" aria-label="Soil moisture">
    <div class="row"><h2>Soil moisture</h2><strong><b id="moist">--</b><span>%</span></strong></div>
    <div class="soil" aria-hidden="true"><div id="moistBar"></div><i id="dryMark"></i><i id="wetMark"></i></div>
    <div class="scale" aria-hidden="true"><span id="dryLabel"></span><span id="wetLabel"></span></div>
    <div class="water" id="water" role="status" hidden>
      <strong id="waterTitle"></strong>
      <span id="waterDetail"></span>
    </div>
  </section>

  <section class="history">
    <header>
      <h2>History</h2>
      <div class="range" id="range" role="group" aria-label="Time range">
        <button type="button" data-h="6" aria-pressed="false">6 h</button>
        <button type="button" data-h="24" aria-pressed="true">24 h</button>
        <button type="button" data-h="168" aria-pressed="false">7 days</button>
      </div>
    </header>
    <figure><figcaption>Temperature (°C)</figcaption><canvas id="tempChart"></canvas></figure>
    <figure><figcaption>Soil moisture (%). Dashed lines mark the watering thresholds.</figcaption><canvas id="moistChart"></canvas></figure>
  </section>

  <details class="raw">
    <summary>Raw sensor readings</summary>
    <dl>
      <dt>Temperature sensor</dt><dd id="tempRaw">--</dd>
      <dt>Moisture sensor</dt><dd id="moistRaw">--</dd>
    </dl>
    <p class="hint">Use these values when filling in calibration.h. They update every few seconds.</p>
  </details>
</main>

<script>
const HOT = __HOT__, COOL = __COOL__, STALE_AFTER = __STALE__;
let rangeHours = 24, history = null, current = null;
const $ = id => document.getElementById(id);
const cssVar = name => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

function band(t) {
  if (t >= HOT)  return { color: '--hot',   text: `Hot, LED red (${HOT} °C or above)` };
  if (t >= COOL) return { color: '--ideal', text: `In range, LED green (${COOL} to ${HOT} °C)` };
  return            { color: '--cool',  text: `Cool, LED blue (below ${COOL} °C)` };
}

function ago(s) {
  if (s < 60) return `${s} s`;
  if (s < 3600) return `${Math.round(s / 60)} min`;
  return `${Math.round(s / 3600)} h`;
}

async function loadLatest() {
  const status = $('status');
  try {
    const d = await (await fetch('api/latest', { cache: 'no-store' })).json();
    if (d.ts === undefined) {
      status.textContent = 'No readings yet. Check the Arduino USB cable.';
      status.classList.add('stale');
      return;
    }
    $('temp').textContent = d.temp_c.toFixed(1);
    $('moist').textContent = Math.round(d.moisture_pct);
    $('moistBar').style.width = Math.max(0, Math.min(100, d.moisture_pct)) + '%';
    current = d;
    showWatering(d);
    $('tempRaw').textContent = d.temp_raw ?? '--';
    $('moistRaw').textContent = d.moisture_raw ?? '--';
    const b = band(d.temp_c);
    $('led').style.backgroundColor = cssVar(b.color);
    $('band').textContent = b.text;

    const stale = d.age_s > STALE_AFTER;
    status.classList.toggle('stale', stale);
    status.textContent = stale ? `Last reading ${ago(d.age_s)} ago. Check the Arduino.` : `Updated ${ago(d.age_s)} ago`;
  } catch (e) {
    status.textContent = "Can't reach the Pi";
    status.classList.add('stale');
  }
}

function showWatering(d) {
  const box = $('water');
  const below = d.water_below, above = d.wet_above;
  const copy = {
    dry: ['Needs watering', `Soil is below ${below}%. Water the herbs.`],
    ok:  ['Okay, no water needed', `Water when the soil drops below ${below}%.`],
    wet: ['Too wet', `Soil is above ${above}%. Hold off watering until it dries out.`],
  }[d.water];
  if (!copy) { box.hidden = true; return; }
  box.hidden = false;
  box.dataset.state = d.water;
  $('waterTitle').textContent = copy[0];
  $('waterDetail').textContent = copy[1];

  placeMark('dryMark', 'dryLabel', below, `Water below ${below}%`);
  placeMark('wetMark', 'wetLabel', above, `Too wet above ${above}%`);
}

function placeMark(markId, labelId, pct, text) {
  const ok = typeof pct === 'number';
  const pos = ok ? Math.max(8, Math.min(92, pct)) : 0;
  $(markId).style.display = ok ? 'block' : 'none';
  $(markId).style.left = (ok ? pct : 0) + '%';
  $(labelId).style.left = pos + '%';
  $(labelId).textContent = ok ? text : '';
}

async function loadHistory() {
  try {
    history = await (await fetch(`api/history?hours=${rangeHours}`, { cache: 'no-store' })).json();
    drawAll();
  } catch (e) { /* keep the last chart */ }
}

function drawAll() {
  if (!history) return;
  const pts = history.points.filter(p => p[1] !== null && p[2] !== null);
  const times = pts.map(p => p[0]);
  drawChart($('tempChart'), times, pts.map(p => p[1]), cssVar('--temp-line'), 1);
  const guides = current ? [current.water_below, current.wet_above].filter(v => typeof v === 'number') : [];
  drawChart($('moistChart'), times, pts.map(p => p[2]), cssVar('--water'), 0, guides);
}

function drawChart(canvas, times, values, color, decimals, guides = []) {
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth, h = canvas.clientHeight;
  canvas.width = w * dpr; canvas.height = h * dpr;
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);

  const muted = cssVar('--muted'), grid = cssVar('--grid');
  const font = getComputedStyle(document.body).fontFamily;
  const pad = { l: 34, r: 6, t: 8, b: 20 };
  ctx.font = `12px ${font}`;

  if (values.length < 2) {
    ctx.fillStyle = muted;
    ctx.fillText('The chart fills in as readings are logged (one per minute).', 4, h / 2);
    return;
  }

  const all = values.concat(guides);
  let min = Math.min(...all), max = Math.max(...all);
  if (max - min < 1) { min -= 0.5; max += 0.5; }
  const span = max - min; min -= span * 0.1; max += span * 0.1;
  const t0 = times[0], t1 = times[times.length - 1];
  const x = t => pad.l + (t - t0) / ((t1 - t0) || 1) * (w - pad.l - pad.r);
  const y = v => pad.t + (1 - (v - min) / (max - min)) * (h - pad.t - pad.b);

  ctx.strokeStyle = grid; ctx.fillStyle = muted; ctx.lineWidth = 1;
  for (let i = 0; i <= 3; i++) {
    const v = min + (max - min) * i / 3, yy = Math.round(y(v)) + 0.5;
    ctx.beginPath(); ctx.moveTo(pad.l, yy); ctx.lineTo(w - pad.r, yy); ctx.stroke();
    ctx.fillText(v.toFixed(decimals), 0, yy + 4);
  }

  if (guides.length) {
    ctx.save();
    ctx.setLineDash([5, 4]); ctx.strokeStyle = muted; ctx.lineWidth = 1.5;
    guides.forEach(g => {
      const yy = Math.round(y(g)) + 0.5;
      ctx.beginPath(); ctx.moveTo(pad.l, yy); ctx.lineTo(w - pad.r, yy); ctx.stroke();
    });
    ctx.restore();
  }

  const opts = rangeHours >= 24 ? { weekday: 'short', hour: 'numeric' } : { hour: 'numeric', minute: '2-digit' };
  const fmt = t => new Date(t * 1000).toLocaleString([], opts);
  ctx.fillText(fmt(t0), pad.l, h - 4);
  const end = fmt(t1);
  ctx.fillText(end, w - pad.r - ctx.measureText(end).width, h - 4);

  ctx.strokeStyle = color; ctx.lineWidth = 2; ctx.lineJoin = 'round'; ctx.lineCap = 'round';
  ctx.beginPath();
  values.forEach((v, i) => {
    const px = x(times[i]), py = y(v);
    if (i === 0) ctx.moveTo(px, py); else ctx.lineTo(px, py);
  });
  ctx.stroke();
}

$('range').addEventListener('click', e => {
  const btn = e.target.closest('button');
  if (!btn) return;
  rangeHours = Number(btn.dataset.h);
  document.querySelectorAll('#range button').forEach(b => b.setAttribute('aria-pressed', b === btn));
  loadHistory();
});
window.addEventListener('resize', drawAll);
window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => { loadLatest(); drawAll(); });

loadLatest().then(loadHistory);
setInterval(loadLatest, 5000);
setInterval(loadHistory, 60000);
</script>
</body>
</html>
"""
DASHBOARD_HTML = (
    DASHBOARD_HTML.replace("__HOT__", str(HOT_THRESHOLD))
    .replace("__COOL__", str(COOL_THRESHOLD))
    .replace("__STALE__", str(STALE_AFTER_S))
)


# ---------------- Main ----------------
def main():
    db_init()
    publisher = MqttPublisher()
    threading.Thread(target=serial_loop, args=(publisher,), daemon=True).start()
    log.info("Dashboard running on port %s", WEB_PORT)
    app.run(host=WEB_HOST, port=WEB_PORT, threaded=True)


if __name__ == "__main__":
    main()
