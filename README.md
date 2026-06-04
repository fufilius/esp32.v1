# ESP32-C3 sensor hub and RGB light indicator

ESP-IDF project for ESP32-C3 Super Mini. The app reads ambient light from a
BH1750 I2C sensor, reads temperature and humidity from a DHT22 sensor, and
shows the current light level with an RGB LED.

## Hardware

- ESP32-C3 Super Mini
- BH1750 I2C light sensor
- DHT22 temperature and humidity sensor
- RGB LED or three separate LEDs

BH1750 wiring:

```text
BH1750 VCC  -> 3V3
BH1750 GND  -> GND
BH1750 SDA  -> GPIO6
BH1750 SCL  -> GPIO7
BH1750 ADDR -> GND or not connected
```

DHT22 wiring:

```text
DHT22 VCC  -> 3V3
DHT22 GND  -> GND
DHT22 DATA -> GPIO10
```

If the DHT22 is not a ready-made module, add a pull-up resistor of about `10k`
between DATA and `3V3`.

RGB LED wiring used by the firmware:

```text
Red   -> GPIO0
Green -> GPIO1
Blue  -> GPIO2
```

If the LED is common-anode or otherwise active-low, update this macro in
`main/main.c`:

```c
#define LED_ACTIVE_LEVEL 0
```

## Behavior

The project uses two background workers and two FreeRTOS queues:

- BH1750 worker: sends the latest light state to an overwrite queue with
  `xQueueOverwrite()`.
- DHT22 worker: sends temperature/humidity readings to a regular queue with
  `xQueueSend()`.

The BH1750 is polled once per second. The RGB LED blinks according to the
measured light level:

- green: enough light
- blue: weak light
- red: no light

The firmware uses hysteresis to avoid unstable switching near threshold values:

```c
#define LIGHT_NONE_ENTER_LUX 1.0f
#define LIGHT_NONE_EXIT_LUX 2.0f
#define LIGHT_WEAK_ENTER_LUX 40.0f
#define LIGHT_WEAK_EXIT_LUX 60.0f
```

The DHT22 is sampled every 3 seconds. Its readings are currently printed to the
serial monitor and stored in a regular queue prepared for a future display task.

The project also has an asynchronous IP/LwIP event handler. It listens for
ESP-IDF `IP_EVENT` events and forwards simplified network events to the main
controller through a FreeRTOS queue.

## Main controller

The main application logic runs in a dedicated FreeRTOS task. It switches
between these states:

- `ST_INIT`: sends the initial critical RGB state while the system starts.
- `ST_CONNECTING`: waits briefly for network/IP events before continuing the
  sensor loop.
- `ST_WAIT_SENSOR_DATA`: waits for the latest BH1750 reading and drains pending
  DHT22 readings from the regular queue.
- `ST_PROCESS_SENSOR_DATA`: validates sensor readings and calculates the RGB
  state from the light level.
- `ST_UPDATE_OUTPUT`: sends the calculated RGB state to the RGB overwrite queue.
- `ST_RECOVERY`: enters network recovery mode after an IP loss event.
- `ST_ERROR`: switches the RGB indicator to the critical state when sensor data
  is invalid.

Both sensor reading structures contain an `is_valid` flag. If the BH1750 or
DHT22 reports invalid data, the controller logs the error and enters
`ST_ERROR`. On the next loop the controller returns to waiting for fresh sensor
data, so a temporary sensor failure can recover automatically when valid
readings appear again.

When the network event handler receives an IP loss event, the controller moves
to `ST_RECOVERY`, switches the RGB indicator to the critical state, waits for a
short recovery delay, and then returns to `ST_CONNECTING`.

## Build and flash

```powershell
idf.py set-target esp32c3
idf.py build
idf.py -p COMx flash monitor
```

The project is configured for a 4 MB flash chip.

## Development Environment

This project keeps VS Code ESP-IDF settings in Git. Install ESP-IDF v5.5.4 on
each Windows PC to the same path:

```text
C:\esp\v5.5.4\esp-idf
```

When opening the project in VS Code, the default terminal profile runs
`export.ps1` automatically. After opening a new terminal, `idf.py` should be
ready to use without manually activating a Python virtual environment.

The serial port can differ between PCs. If needed, update `COM5` in
`.vscode/settings.json` or pass a port explicitly:

```powershell
idf.py -p COMx flash monitor
```

## Git Workflow

The `main` branch is protected. Make changes in a feature branch, push it to
GitHub, and merge through a pull request:

```powershell
git switch -c feature/my-change
git add .
git commit -m "Describe change"
git push -u origin feature/my-change
```
