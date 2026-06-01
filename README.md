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
