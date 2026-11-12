# MQTT quick setup: testing the ESP32 over Wi-Fi

Gets Mosquitto on the Pi ready to receive readings from the XIAO ESP32-C6, so
you can check the board's Wi-Fi link from end to end. About 15 minutes.

The Uno and the dashboard keep working throughout. Step 7, which switches the
dashboard over to the ESP32, is optional.

```
ESP32 --Wi-Fi--> router --> Pi: Mosquitto (port 1883) --> garden bridge --> dashboard
```

Out of the box, Mosquitto only accepts connections from the Pi itself. Steps 1
to 3 open it to your home network with a login.

In the commands below, replace `garden` with a username of your choice if you
like. Run everything on the Pi (`ssh raspi4@ras-4`) unless a step says otherwise.

## 1. Check Mosquitto is installed

```bash
sudo apt update
```

```bash
sudo apt install -y mosquitto mosquitto-clients
```

```bash
systemctl is-active mosquitto
```

It should print `active`. `mosquitto-clients` adds the test tools used in
steps 3 and 6.

## 2. Create a login and open Mosquitto to the network

Create the login. You'll be asked to choose a password; use a new one, not your
Wi-Fi password.

```bash
sudo mosquitto_passwd -c /etc/mosquitto/passwd garden
```

Let Mosquitto read the password file:

```bash
sudo chown mosquitto:mosquitto /etc/mosquitto/passwd
```

```bash
sudo chmod 640 /etc/mosquitto/passwd
```

Create the settings file:

```bash
sudo nano /etc/mosquitto/conf.d/garden.conf
```

Put these three lines in it, then save with Ctrl+O, Enter, Ctrl+X:

```
listener 1883
allow_anonymous false
password_file /etc/mosquitto/passwd
```

Restart and check it came back:

```bash
sudo systemctl restart mosquitto
```

```bash
systemctl is-active mosquitto
```

If it prints `failed`, see Troubleshooting.

## 3. Test the login on the Pi

Open two terminal windows on the Pi.

**Window 1** listens for every garden message. Leave it running:

```bash
mosquitto_sub -h localhost -u garden -P 'your-password' -t 'garden/#' -v
```

**Window 2** sends a test message:

```bash
mosquitto_pub -h localhost -u garden -P 'your-password' -t garden/test -m hello
```

**Pass:** Window 1 prints `garden/test hello`.

Typing the password in a command saves it in the shell history. To avoid that,
put a space before the command.

## 4. Give the garden bridge the login

Once a login is required, the bridge needs it too. Without this step the
dashboard keeps working, but the bridge can no longer publish to MQTT and its
log fills with "connection refused".

```bash
sudo nano /etc/default/garden-bridge
```

Add these two lines (create the file if it's empty):

```
GARDEN_MQTT_USER=garden
GARDEN_MQTT_PASS=your-password
```

Keep other people from reading the password, then restart the bridge:

```bash
sudo chmod 600 /etc/default/garden-bridge
```

```bash
sudo systemctl restart garden-bridge
```

```bash
journalctl -u garden-bridge -n 20 --no-pager
```

**Pass:** the log shows `MQTT connected to localhost:1883`. Window 1 from step 3
also starts printing `garden/state` messages every 10 seconds while the Uno is
plugged in.

## 5. Point the ESP32 at the Pi

Find the Pi's address on your home network:

```bash
hostname -I
```

Use the address that starts with `192.168.`. Ignore one starting with `100.`:
that is Tailscale, and the ESP32 can't reach it.

In your router, reserve that address for the Pi so it doesn't change. If it
does change later, the ESP32 stops getting through until `secrets.h` is updated.

On the Mac, edit `esp32/iot_garden_esp32/secrets.h` (not `secrets.example.h`):

```cpp
const char* MQTT_HOST     = "192.168.1.xx";   // the Pi's address from above
const int   MQTT_PORT     = 1883;

const char* MQTT_USER     = "garden";
const char* MQTT_PASSWORD = "your-password";
```

Git ignores `secrets.h`, so the passwords stay off GitHub.

## 6. Flash the ESP32 and watch it arrive

1. Check `BENCH_MODE = true` in `calibration.h`. The board then stays awake and
   sends every 10 seconds.
2. In Arduino IDE: board **XIAO_ESP32C6**, **USB CDC On Boot: Enabled**, then
   upload `esp32/iot_garden_esp32`.
3. Open the Serial Monitor at 115200 baud.
4. Keep Window 1 from step 3 running on the Pi.

**Pass, on the Serial Monitor:** a JSON line followed by `Published`, every 10
seconds.

**Pass, on the Pi:** Window 1 prints a line like this every 10 seconds:

```
garden/esp32/reading {"temp_c":21.4,"moisture_pct":0,"water":"dry",...,"rssi":-58}
```

### Checking the Wi-Fi link

`rssi` at the end of each message is the signal strength at the board.

| `rssi` | Link |
|---|---|
| -30 to -60 | Strong |
| -60 to -70 | Fine |
| -70 to -80 | Weak: expect some missed readings and shorter battery life |
| Below -80 | Too weak to rely on |

To test the real position, power the board from a USB power bank or the
battery, carry it to where it will live outside, and watch Window 1 on the Pi.
Count the messages over a few minutes: at one every 10 seconds, any gaps show
up quickly. To compare the built-in antenna with the external one, change
`USE_EXTERNAL_ANTENNA` in `calibration.h`, upload, and compare `rssi` at the
same spot.

## 7. Optional: show the ESP32 on the dashboard

Until now the bridge has ignored the ESP32's messages and kept reading the Uno.
To switch:

```bash
sudo nano /etc/default/garden-bridge
```

Add:

```
GARDEN_INPUT=mqtt
```

```bash
sudo systemctl restart garden-bridge
```

The dashboard header changes to `ESP32: Battery --%`, and readings come from
the ESP32. Wi-Fi signal and wake time appear under **Raw sensor readings**.

To go back to the Uno, delete that line and restart the bridge again.

## Troubleshooting

| What you see | Likely cause | Fix |
|---|---|---|
| `systemctl is-active mosquitto` prints `failed` | A typo in `garden.conf`, or Mosquitto can't read the password file | `journalctl -u mosquitto -n 20 --no-pager` shows the reason. Re-check step 2 |
| `Connection error: Connection Refused: not authorised` | Wrong username or password | Re-run the `mosquitto_passwd` command without `-c` to reset the password |
| Serial: `Wi-Fi connection failed` | Wrong Wi-Fi name or password, or a 5 GHz-only network | Check `secrets.h`. The board only uses 2.4 GHz |
| Serial: `MQTT publish failed` | Wrong Pi address, wrong MQTT login, or step 2 not done | From another computer on the Wi-Fi, check the Pi's port 1883 is reachable, then re-check `secrets.h` |
| Works at the desk, fails outside | Weak signal | Check `rssi`, then move or change the antenna |
| Nothing in Window 1, but Serial says `Published` | Window 1 is subscribed with a different topic | Use `-t 'garden/#'` exactly |
| Worked yesterday, `MQTT publish failed` today | The Pi's address changed | Reserve the address in the router, then update `secrets.h` |

To check from the Mac that the Pi's MQTT port is open:

```bash
nc -z -G 3 ras-4 1883 && echo open || echo closed
```

## Security

- **Never port-forward 1883 on the router.** This setup is for your home network
  and Tailscale only. MQTT on this port is not encrypted.
- The MQTT password travels unencrypted over your Wi-Fi, so don't reuse one
  that matters elsewhere.
- Passwords belong in `secrets.h` on the Mac and `/etc/default/garden-bridge` on
  the Pi. Neither is in the Git repository.

## When the test is done

- [ ] `rssi` at the outdoor position written down
- [ ] Pi's address reserved in the router
- [ ] `GARDEN_INPUT` set the way you want it (step 7)
- [ ] For battery use: `BENCH_MODE = false`, then follow `BATTERY_TESTING.md`
