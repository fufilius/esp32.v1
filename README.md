# ESP32 sensor hub and RGB light indicator

ESP-IDF project for an ESP32 development board. The app reads ambient light
from a BH1750 I2C sensor, reads temperature and humidity from a DHT22 sensor,
connects to Wi-Fi through a first-run setup access point, and exposes the
latest sensor readings through a local web page.

## Hardware

- ESP32 development board
- BH1750 I2C light sensor
- DHT22 temperature and humidity sensor
- RGB LED or three separate LEDs
- Optional reset button, or the built-in BOOT button on `GPIO0`

BH1750 wiring:

```text
BH1750 VCC  -> 3V3
BH1750 GND  -> GND
BH1750 SDA  -> GPIO21
BH1750 SCL  -> GPIO22
BH1750 ADDR -> GND or not connected
```

DHT22 wiring:

```text
DHT22 VCC  -> 3V3
DHT22 GND  -> GND
DHT22 DATA -> GPIO27
```

If the DHT22 is not a ready-made module, add a pull-up resistor of about `10k`
between DATA and `3V3`.

RGB LED wiring used by the firmware:

```text
Red   -> GPIO25
Green -> GPIO26
Blue  -> GPIO33
```

Manual Wi-Fi reset button:

```text
BOOT button or external button -> GPIO0 to GND
```

Hold the button for 3 seconds to clear the saved Wi-Fi credentials and reopen
the setup access point.

If the LED is common-anode or otherwise active-low, update this macro in
`main/main.c`:

```c
#define LED_ACTIVE_LEVEL 0
```

## Wi-Fi Setup

On first boot, or after a manual Wi-Fi reset, the ESP32 starts a setup access
point:

```text
SSID:     ESP32-Setup
Password: configure123
```

Connect to this access point and open:

```text
http://192.168.4.1/
```

The setup page scans for nearby Wi-Fi networks and lets you choose an SSID and
enter a password. Credentials are saved in NVS and reused on the next boot.

If the saved Wi-Fi network is unavailable, the firmware retries connection,
shows the critical LED state, and then starts the setup access point again.

After the ESP32 connects to Wi-Fi, the monitor prints its assigned IP address:

```text
wifi_manager: connected, IP address: 192.168.x.x
```

Open that IP in a browser to view the sensor dashboard:

```text
http://192.168.x.x/
```

The same data is available as JSON:

```text
http://192.168.x.x/api/sensors
```

## LED Behavior

The RGB LED is now used as a system status indicator:

- green: Wi-Fi is connected and sensor data is valid.
- blue: the setup access point is running.
- red: Wi-Fi is unavailable or at least one sensor reports invalid data.

Sensor errors have priority over Wi-Fi status. For example, if Wi-Fi is
connected but the DHT22 starts timing out, the red LED state is shown until
fresh valid sensor data arrives again.

## Sensor Behavior

The project uses two background workers and two FreeRTOS queues:

- BH1750 worker: sends the latest light state to an overwrite queue with
  `xQueueOverwrite()`.
- DHT22 worker: sends temperature/humidity readings to a regular queue with
  `xQueueSend()`.

The BH1750 is polled once per second. The DHT22 is sampled every 3 seconds.
The latest readings and error states are stored in an in-memory sensor snapshot
used by the web dashboard and `/api/sensors`.

The serial monitor prints compact sensor summaries only when values or validity
state changes, for example:

```text
app: sensors: BH1750=108.33 lx, DHT22=23.4 C 43.6 %
app: sensors: BH1750=108.33 lx, DHT22=invalid(ESP_ERR_TIMEOUT)
```

The project also has asynchronous Wi-Fi/IP event handling. ESP-IDF Wi-Fi and
IP events are converted into simplified network events and forwarded to the
main controller through a FreeRTOS queue.

## Main controller

The main application logic runs in a dedicated FreeRTOS task. It switches
between these states:

- `ST_INIT`: sends the initial critical RGB state while the system starts.
- `ST_CONNECTING`: waits briefly for network/IP events before continuing the
  sensor loop.
- `ST_WAIT_SENSOR_DATA`: waits for the latest BH1750 reading and drains pending
  DHT22 readings from the regular queue.
- `ST_PROCESS_SENSOR_DATA`: validates sensor readings and logs a compact
  summary when the values change.
- `ST_UPDATE_OUTPUT`: keeps the RGB output aligned with the current Wi-Fi
  status when sensor data is valid.
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

Install ESP-IDF tools for the ESP32 target before building on a new PC:

```powershell
cd C:\esp\v5.5.4\esp-idf
.\install.ps1 esp32
```

```powershell
idf.py set-target esp32
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
