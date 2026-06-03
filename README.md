# ESP32-C3 FreeRTOS RGB blink example

This folder is the ESP-IDF project root.

The app has two FreeRTOS tasks:

- `fake_system_state_task`: changes a fake system state every 5 seconds.
- `rgb_blink_task`: blinks one RGB LED channel according to the current state.

State mapping:

- `SYSTEM_STATE_OK`: green, 1 Hz
- `SYSTEM_STATE_WARNING`: blue, 2 Hz
- `SYSTEM_STATE_CRITICAL`: red, 4 Hz

Before flashing, edit `main/main.c` and set these macros to match your wiring:

```c
#define LED_R_GPIO GPIO_NUM_4
#define LED_G_GPIO GPIO_NUM_5
#define LED_B_GPIO GPIO_NUM_6
#define LED_ACTIVE_LEVEL 1
```

If your LED is common-anode or otherwise active-low, set:

```c
#define LED_ACTIVE_LEVEL 0
```

Build and flash:

```powershell
idf.py fullclean
idf.py set-target esp32c3
idf.py build
idf.py -p COMx flash monitor
```

## Development environment

This project keeps VS Code ESP-IDF settings in Git. Install ESP-IDF v5.5.4
on each Windows PC to the same path:

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
