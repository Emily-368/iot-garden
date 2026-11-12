# ESP32 battery testing procedure

How to measure how much battery the XIAO ESP32-C6 garden node uses once it is
built, and how to work out how long a charge will last.

Work through the tests in order. Tests 1 to 3 need about an hour. Test 4 runs
for two days on its own.

## What the battery is spent on

The node is asleep almost all the time. Each cycle it wakes, reads the sensors,
connects to Wi-Fi, sends one message and sleeps again.

| State | Typical current | Time per 10-minute cycle |
|---|---|---|
| Deep sleep | about 15 µA for the board, plus 10 µA for the battery divider | about 599 s |
| Awake, Wi-Fi off (sensors) | about 20 to 40 mA | about 0.2 s |
| Awake, Wi-Fi on | about 80 to 100 mA average, with short peaks above 300 mA | about 1 to 4 s |

These are estimates from datasheets, not measurements of your board. The point
of this procedure is to replace them with your own numbers.

Wi-Fi time is nearly all of the cost. One second less awake per cycle saves
more than removing the sleep current entirely. So the two numbers that matter
are:

- **Awake time per cycle** (Test 1)
- **Sleep current** (Test 2)

## What you need

- The built node with the LiPo connected and the battery divider fitted
  (`BATTERY_DIVIDER_FITTED = true`). Without the divider you can still do
  Tests 1 and 2.
- A multimeter with a µA range and a mA range.
- Two short jumper leads with clips, and one spare jumper wire.
- The Pi running the bridge in MQTT mode (`GARDEN_INPUT=mqtt`).
- Optional: a USB power meter that counts mAh, for Test 5.

## Safety

- Never short the battery leads. Unplug the battery before changing wiring.
- Double-check polarity before reconnecting: reversed polarity can destroy
  the board.
- Don't leave a LiPo charging unattended, and don't charge it below 0 °C.

## Record sheet

Copy this table and fill it in as you go.

| Item | Your value |
|---|---|
| Date | |
| `SLEEP_INTERVAL_S` | |
| Static IP enabled? | |
| `WIFI_TX_POWER` set? | |
| Wi-Fi signal (dBm) | |
| Awake time, first wake after reset (ms) | |
| Awake time, normal wakes: lowest / typical / highest (ms) | |
| Sleep current (µA) | |
| Awake current, average (mA), if measured | |
| Calculated use per day (mAh) | |
| Calculated battery life (days) | |
| Test 4 result: use per day (mAh) | |

---

## Test 0: Check it works on USB first

1. In `calibration.h`, set `BENCH_MODE = true` and upload.
2. Open the serial monitor at 115200 baud. Every 10 seconds you should see a
   JSON line followed by `Published`.
3. On the dashboard, open **Raw sensor readings**. Check that **Wi-Fi signal
   (dBm)** appears.

**Pass:** readings arrive every 10 seconds, and the signal is stronger than
about -70 dBm (closer to zero is stronger; -55 is good, -80 is poor).

If the signal is weak, move the antenna before going further. A weak link makes
every wake longer, and failed wakes are the most expensive thing the node can do.

## Test 1: Awake time per cycle (no instruments needed)

The node times each wake and reports it in the next message as `awake_ms`.

1. In `calibration.h`, set `BENCH_MODE = false` and `SLEEP_INTERVAL_S = 60`,
   then upload. The short interval is only to get results quickly.
2. Unplug USB and run it from the battery. The board resets when power changes.
3. On the dashboard, open **Raw sensor readings** and watch **Last wake time
   (ms)** for 10 or more readings. Or download them all at once:

   ```bash
   curl -o garden.csv "http://ras-4:5000/api/export.csv?hours=1"
   ```

4. Write down the first value after the reset separately. That wake has to scan
   for the router, so it is always the slowest. The first message after a reset
   shows 0, because there was no earlier wake to time.

**What to expect (estimates):**

| Setup | Normal wake |
|---|---|
| Fast reconnect and static IP | about 800 to 1,500 ms |
| Fast reconnect, no static IP | about 1,500 to 3,000 ms |
| Full scan every time (fast reconnect not working) | about 3,000 to 6,000 ms |

`awake_ms` starts counting when the sketch starts, so the real wake is roughly
300 ms longer. Add 300 ms when you use it in the calculation below.

**If it is slow:**

- Normal wakes as slow as the first one: the fast reconnect is failing. Mesh
  systems and routers that change channel often cause this.
- Enable the static IP block in `secrets.h` if you haven't.
- Occasional very long wakes (over 8,000 ms) or missing readings: the link is
  marginal. Improve the antenna position before anything else.

## Test 2: Sleep current (multimeter)

A multimeter on its µA range can't pass enough current for Wi-Fi, and the board
will reset if it tries. The trick is to bypass the meter while the board is
awake.

1. Set `SLEEP_INTERVAL_S = 3600` and upload, so the board stays asleep while
   you measure.
2. Unplug the battery. Connect the multimeter **in series** with the battery's
   positive lead: battery + to the meter, meter to the board's BAT+.
3. Clip the spare jumper wire across the two meter probes. This is the bypass.
4. Set the meter to its **µA** range and reconnect the battery.
5. Wait 15 seconds for the board to finish its first wake and go to sleep.
6. Remove the bypass jumper. The meter now shows the sleep current.
7. Put the bypass jumper back **before** the next wake or before pressing reset.

**Pass:** about 15 to 30 µA with the battery divider fitted.

**If it is higher, find the cause by removing one thing at a time:**

| Change | If the current drops, the cause is |
|---|---|
| Unplug both sensors | A sensor is drawing power while off (check VCC is on D3, not 3V3) |
| Disconnect the battery divider | The divider (expected: about 10 µA for 200k + 200k) |
| Unplug the antenna | Unlikely to change anything; if it does, re-seat the connector |

- Around 100 to 500 µA with everything removed: something on the board is still
  powered. Note the value and compare it with Seeed's figure of about 15 µA.
- Several mA: the board is not reaching deep sleep. Check `BENCH_MODE = false`.

## Test 3: Awake current (optional, multimeter)

This gives a rough average only, because a multimeter is too slow to see the
short Wi-Fi peaks. Skip it and use 90 mA in the calculation if you prefer.

1. Set `SLEEP_INTERVAL_S = 60` and upload.
2. Put the meter in series as in Test 2, on its **mA** range (at least 400 mA),
   with no bypass jumper.
3. Watch the reading during several wakes and note the typical value.

If the board resets during a wake, the meter's resistance is too high on that
range. Use the 10 A range instead.

## Calculating battery life

Use your numbers from Tests 1 to 3.

```
cycles per day     = 86,400 / SLEEP_INTERVAL_S
awake seconds      = (awake_ms + 300) / 1,000
Wi-Fi use per day  = cycles per day × awake seconds × awake mA / 3,600     (mAh)
sleep use per day  = sleep µA × 24 / 1,000                                 (mAh)
total per day      = Wi-Fi use + sleep use                                 (mAh)
battery life       = battery mAh × 0.8 / total per day                     (days)
```

The 0.8 allows for capacity you can't use: the node stops sending below about
3.3 V, and real capacity is usually a little under the label.

**Worked example** with a 2,400 mAh battery, 10-minute readings, 1,200 ms
`awake_ms`, 90 mA awake and 25 µA asleep:

```
cycles per day     = 86,400 / 600            = 144
awake seconds      = (1,200 + 300) / 1,000   = 1.5
Wi-Fi use per day  = 144 × 1.5 × 90 / 3,600  = 5.4 mAh
sleep use per day  = 25 × 24 / 1,000         = 0.6 mAh
total per day      = 6.0 mAh
battery life       = 2,400 × 0.8 / 6.0       = 320 days
```

Treat the result as an upper limit. A LiPo also loses a few percent a month on
its own, and cold weather reduces capacity, so expect noticeably less than the
calculation over many months.

## Test 4: Two-day rundown (checks the calculation)

A normal 10-minute interval would take months to show a clear drop, so this test
speeds things up by sending every minute.

1. Charge the battery fully over USB (the charge light goes out).
2. Set `SLEEP_INTERVAL_S = 60`, `BENCH_MODE = false`, upload, and unplug USB.
3. Leave it running outdoors in its housing for 48 hours.
4. Download the log:

   ```bash
   curl -o rundown.csv "http://ras-4:5000/api/export.csv?hours=48"
   ```

5. Open it in a spreadsheet and chart `battery_mv` against `time_utc`.

**What to check:**

- **Readings received:** 48 hours at one a minute is 2,880. Count the rows. More
  than about 2 % missing means the Wi-Fi link is costing you battery.
- **Voltage drop:** compare the average of the first 10 and last 10
  `battery_mv` values. Convert each to a percentage with the table below, and
  multiply the difference by the battery's capacity.
- **Compare with the calculation:** redo the calculation above with
  `SLEEP_INTERVAL_S = 60`, and multiply by 2 days. The two results should be
  within about 30 % of each other. Voltage is a coarse guide to charge,
  especially between 3.7 and 3.9 V where the curve is flat, so don't expect a
  closer match.

| Battery voltage (mV) | Approximate charge |
|---|---|
| 4,200 | 100 % |
| 4,060 | 90 % |
| 3,980 | 80 % |
| 3,920 | 70 % |
| 3,870 | 60 % |
| 3,820 | 50 % |
| 3,790 | 40 % |
| 3,770 | 30 % |
| 3,740 | 20 % |
| 3,680 | 10 % |
| 3,450 | 5 % |
| 3,000 | 0 % |

This is the same table the sketch uses for the dashboard's battery percentage.

**After the test, set `SLEEP_INTERVAL_S` back to 600.**

## Test 5: Recharge count (optional, most accurate)

If you have a USB power meter that counts mAh:

1. After Test 4, plug the node into USB through the meter and reset the meter's
   counter.
2. Leave it until the charge light goes out. At the board's charge rate, a full
   2,400 mAh charge takes roughly a day.
3. The mAh counted is a little more than what the test used, because charging
   isn't perfectly efficient and the board itself draws some power. Multiply by
   0.85 for a fair estimate.

## Test 6: Failure behaviour

These confirm the node protects the battery when something is wrong.

| Test | How | Expected |
|---|---|---|
| Router unreachable | Set `SLEEP_INTERVAL_S = 60`. Turn off Wi-Fi on the router, or put a wrong password in `secrets.h` | After 3 failed wakes the gaps between attempts double each time, up to 8 minutes at this setting. Use `BENCH_MODE = true` to watch `Wi-Fi connection failed` on serial |
| Pi unreachable | Stop Mosquitto: `sudo systemctl stop mosquitto` | Same backoff. Serial shows `MQTT publish failed` |
| Recovery | Restore the router or Mosquitto | Readings resume at the normal interval within one backed-off sleep |
| Dashboard warning | Leave the node off for 3 cycles | Dashboard shows "Last reading … ago. Check the garden sensor." |
| Low battery | Hard to test safely. Temporarily set `BATTERY_CRITICAL_MV = 4300` in `BENCH_MODE` | Serial shows `Battery critical: skipping Wi-Fi`. Set it back to 3300 afterwards |

## Settings that change battery life

Listed from largest effect to smallest.

| Setting | Where | Effect |
|---|---|---|
| `SLEEP_INTERVAL_S` | `calibration.h` | Biggest. 900 (15 min) uses about a third less than 600. Soil moisture changes slowly, so longer gaps lose little |
| Wi-Fi signal | Antenna position, and `USE_EXTERNAL_ANTENNA` in `calibration.h` | A weak link means long wakes and retries |
| Static IP | `secrets.h` | Skips asking the router for an address on every wake |
| `WIFI_TX_POWER` | `calibration.h` | Lower transmit power. Only worth trying with a strong signal, and re-run Test 1 afterwards |
| Battery divider | Hardware | 200k + 200k draws about 10 µA all the time. Small next to Wi-Fi |

## Before leaving it outside

- [ ] `BENCH_MODE = false`
- [ ] `SLEEP_INTERVAL_S` back to your chosen value (600 by default)
- [ ] `BATTERY_CRITICAL_MV = 3300`
- [ ] Sleep current measured and written down
- [ ] Typical `awake_ms` written down
- [ ] Battery fully charged
