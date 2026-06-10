#pragma once

typedef enum {
    SYSTEM_STATE_OK = 0,       // Green: Wi-Fi, MQTT and sensors are OK
    SYSTEM_STATE_WARNING,      // Blue: setup AP is running
    SYSTEM_STATE_CRITICAL,     // Red: network unavailable or sensors invalid
    SYSTEM_STATE_COUNT
} system_state_t;

static inline const char *system_state_name(system_state_t state)
{
    switch (state) {
    case SYSTEM_STATE_OK:
        return "READY";
    case SYSTEM_STATE_WARNING:
        return "SETUP AP";
    case SYSTEM_STATE_CRITICAL:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}
