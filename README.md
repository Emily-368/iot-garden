# IoT Garden

Arduino Uno reads soil moisture and temperature, shows them on an LCD and sets
an RGB LED by temperature. A Raspberry Pi reads the Arduino over USB, logs the
data, publishes it to MQTT and serves a phone dashboard on a web port (`GARDEN_WEB_PORT`).

```
arduino/iot_garden/iot_garden.ino   Arduino sketch
arduino/iot_garden/calibration.h    Calibration values and watering thresholds
pi/garden_bridge.py                 Serial -> SQLite + MQTT + web dashboard
pi/garden-bridge.service            systemd template (installed by update.sh)
pi/requirements.txt                 Python packages for the Pi
update.sh                           Run on the Pi to pull and apply changes
CLAUDE.md                           Project context for Claude Code
To Do List.md                       Ideas and next steps
```

## Updating

1. Edit, commit and push from your computer.
2. On the Pi: `~/iot-garden/update.sh`

`update.sh` restarts the bridge if Pi code changed, and compiles and flashes the
Arduino from the Pi if the sketch changed. Use `--all` to force everything.

Pi-specific settings (serial port, MQTT broker) live in `/etc/default/garden-bridge`
on the Pi, not in this repo.

## Remote access (Tailscale)

The dashboard has no login, so it is only reachable from outside the house
through Tailscale. Never port-forward the dashboard port on the router.

In the commands below, replace `your-pi-name` with the Pi's hostname,
`your-username` with its login user, and `<port>` with the dashboard port
(`GARDEN_WEB_PORT`).

### 1. Pi (one time)

SSH in on the home network (`ssh your-username@your-pi-name.local`), then:

```bash
curl -fsSL https://tailscale.com/install.sh | sh
sudo tailscale up --ssh
```

`tailscale up` prints a login link. Open it and sign in with the account you
will use on your phone. `--ssh` lets you SSH to the Pi from anywhere on your
tailnet, so you can run `update.sh` while away.

Check it worked:

```bash
tailscale status
tailscale ip -4
```

The installer enables `tailscaled` in systemd, so it comes back after a reboot.
The bridge already listens on all interfaces (`0.0.0.0:<port>`), so nothing in
this repo needs changing.

### 2. Tailscale admin console (one time)

At https://login.tailscale.com/admin:

- **Machines → your-pi-name → ⋯ → Disable key expiry.** Otherwise the Pi drops off
  the tailnet after about 6 months and you need to be at home to log it in again.
- **DNS → MagicDNS** should be on (it is by default). This is what makes the
  name `your-pi-name` work instead of an IP.
- If the machine shows up with a different name (e.g. `your-pi-name-1`), rename it to
  `your-pi-name` there.

### 3. Phone and PC

Install the Tailscale app, sign in with the same account and turn it on.
Then open:

```
http://your-pi-name:<port>
```

Use `http://`, not `https://`: the dashboard port is plain HTTP, and
an `https://your-pi-name:<port>` link fails. For HTTPS, see step 4.

On a phone, use **Add to Home Screen** for an app-like icon. Turn Wi-Fi off and
try on mobile data to confirm it really works away from home.

### 4. Optional: HTTPS address

For a proper `https://` link (no browser "not secure" warning), run on the Pi:

```bash
sudo tailscale serve --bg <port>
```

The first time, it asks you to enable HTTPS in the admin console. The dashboard
is then also at `https://your-pi-name.<your-tailnet>.ts.net`, visible only to your
tailnet. `--bg` makes it persist across reboots. Undo with
`sudo tailscale serve reset`.

### Security

This setup opens no ports on the router: Tailscale makes outgoing connections
only, and the dashboard and SSH are reachable just from devices logged in to
your tailnet. Keep it that way:

- **Never port-forward** the dashboard or SSH port on the router.
- **Never run `tailscale funnel`.** Funnel (unlike `serve`) puts the
  dashboard on the public internet, with no login.
- **Don't share `your-pi-name` or invite other users** to the tailnet unless you want
  them to see the garden and SSH to the Pi.
- **Only add your own devices.** If a phone or PC is lost, remove it under
  Machines in the admin console. Do the same for the Pi if it's ever stolen,
  since its key expiry is disabled.
- **Keep Tailscale updated.** The installer adds Tailscale's apt repository, so
  `sudo apt update && sudo apt full-upgrade` on the Pi keeps it patched.

### Remote updates

With Tailscale SSH on, the normal workflow works from anywhere:

```bash
ssh your-username@your-pi-name
bash ~/iot-garden/update.sh
```

### Troubleshooting

- **"Can't reach the Pi"**: check Tailscale is on in the phone app, then
  `tailscale status` on the Pi. `sudo tailscale up --ssh` again if it's logged out.
- **`your-pi-name` doesn't resolve**: MagicDNS is off, or the machine has another name.
  `tailscale status` on the Pi shows the name and the 100.x address.
- **Works on Wi-Fi, not mobile data**: Tailscale isn't actually connected on
  the phone; the LAN was doing the work.
