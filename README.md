# IoT Garden

Arduino Uno reads soil moisture and temperature, shows them on an LCD and sets
an RGB LED by temperature. A Raspberry Pi reads the Arduino over USB, logs the
data, publishes it to MQTT and serves a phone dashboard on port 5000.

```
arduino/iot_garden/iot_garden.ino   Arduino sketch
pi/garden_bridge.py                 Serial -> SQLite + MQTT + web dashboard
pi/garden-bridge.service            systemd template (installed by update.sh)
pi/requirements.txt                 Python packages for the Pi
update.sh                           Run on the Pi to pull and apply changes
```

## Updating

1. Edit, commit and push from your computer.
2. On the Pi: `~/iot-garden/update.sh`

`update.sh` restarts the bridge if Pi code changed, and compiles and flashes the
Arduino from the Pi if the sketch changed. Use `--all` to force everything.

Pi-specific settings (serial port, MQTT broker) live in `/etc/default/garden-bridge`
on the Pi, not in this repo.
