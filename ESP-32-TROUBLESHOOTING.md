# TROUBLESHOOTING FOR ESP32-C6

## Stuck In Deepsleep
If the main iot_garden_esp32.ino has been flashed without a physical trigger to reset deepsleep state.

### 1. Install esptool
Install esptool in terminal:

```bash
pip install esptool
```

### 2. Erase Flash Memory via Terminal

Unplug ESP32 from power, type command without hitting enter:

``` bash
python -m esptool --chip esp32c6 erase-flash
```

Once command loaded, plug in ESP32 then IMMEDIATELY press enter:

## Output
Once erased terminal should display

``` bash
Flash memory erased successfully in X.X seconds.
Hard resetting via RTS pin...
```