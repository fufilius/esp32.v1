# ESP32 MQTT sensor hub

ESP-IDF project for an ESP32 development board. The firmware reads a BH1750
light sensor and a DHT22 temperature/humidity sensor, publishes rounded sensor
values to MQTT, and shows system status with RGB LEDs.

## Hardware

- ESP32 development board
- BH1750 / GY-302 I2C light sensor
- DHT22 temperature and humidity sensor
- RGB LED or three separate LEDs

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

If the DHT22 is not a ready-made module, add a `10k` pull-up resistor between
DATA and `3V3`.

RGB LED wiring:

```text
Red   -> GPIO25
Green -> GPIO26
Blue  -> GPIO33
```

BOOT / `GPIO0` is used to reset saved Wi-Fi and MQTT settings. Hold it for
about 3 seconds.

## Behavior

On first boot, or after settings reset, ESP32 starts a setup access point:

```text
SSID: ESP32-Setup
Password: configure123
Setup page: http://192.168.4.1/
```

The setup page stores these settings in NVS:

- Wi-Fi SSID
- Wi-Fi password
- MQTT URI, for example `mqtt://172.16.101.38:1883`
- MQTT topic, default `esp32/sensors`
- MQTT username/password, if required

After settings are saved, later boots connect automatically.

## MQTT

Sensor values are rounded to integers before logging and publishing. MQTT
messages are published only when rounded values change.

Example payload:

```json
{"light_lux":791,"temperature_c":24,"humidity_percent":41}
```

For the local Mosquitto broker used during development:

```text
Host: 172.16.101.38
Port: 1883
Username: esp32
Password: esp32pass
Topic: esp32/sensors
```

Local Mosquitto runtime files such as `mosquitto.passwd`, `mosquitto.log`, and
`mosquitto.db` are ignored by Git.

## LED status

- Green: Wi-Fi connected, MQTT connected, and sensors are OK
- Blue: setup access point is running
- Red: Wi-Fi unavailable or sensor data is invalid

## Sensors

BH1750 is read over I2C once per second.

DHT22 is read through the ESP32 RMT peripheral instead of manual GPIO polling.
This keeps DHT22 readings stable while Wi-Fi and MQTT are active.

The serial monitor prints compact sensor lines only when rounded values or
sensor status change:

```text
sensors: BH1750=791 lx, DHT22=24 C 41 %
```

## Build and flash

Install ESP-IDF tools for the ESP32 target:

```powershell
cd C:\esp\v5.5.4\esp-idf
.\install.ps1 esp32
```

Build and flash:

```powershell
cd D:\IDE\esp32.v1-esp32
C:\esp\v5.5.4\esp-idf\export.ps1
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

The project is configured for a 4 MB flash chip.

## Development environment

This project keeps VS Code ESP-IDF settings in Git. Install ESP-IDF v5.5.4 on
each Windows PC to:

```text
C:\esp\v5.5.4\esp-idf
```

The serial port can differ between PCs. Update `COM5` in
`.vscode/settings.json` or pass a port explicitly:

```powershell
idf.py -p COMx flash monitor
```

## Git workflow

The `main` branch is protected on GitHub. Make changes in a feature branch,
push it, and merge through a pull request:

```powershell
git switch -c feature/my-change
git add .
git commit -m "Describe change"
git push -u origin feature/my-change
```
