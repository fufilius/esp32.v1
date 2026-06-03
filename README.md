# ESP32-C3 BH1750 RGB light indicator

ESP-IDF project for ESP32-C3 Super Mini. The app reads ambient light from a
BH1750 I2C sensor and shows the current light level with an RGB LED.

## Hardware

- ESP32-C3 Super Mini
- BH1750 I2C light sensor
- RGB LED or three separate LEDs

BH1750 wiring:

```text
BH1750 VCC  -> 3V3
BH1750 GND  -> GND
BH1750 SDA  -> GPIO6
BH1750 SCL  -> GPIO7
BH1750 ADDR -> GND or not connected
```

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
