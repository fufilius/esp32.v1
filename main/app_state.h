#pragma once

typedef enum {
    SYSTEM_STATE_OK = 0,       // Green: Wi-Fi connected
    SYSTEM_STATE_WARNING,      // Blue: setup AP is running
    SYSTEM_STATE_CRITICAL,     // Red: Wi-Fi unavailable
    SYSTEM_STATE_COUNT
} system_state_t;

static inline const char *system_state_name(system_state_t state)
{
    switch (state) {
    case SYSTEM_STATE_OK:
        return "WIFI CONNECTED";
    case SYSTEM_STATE_WARNING:
        return "WIFI SETUP AP";
    case SYSTEM_STATE_CRITICAL:
        return "WIFI UNAVAILABLE";
    default:
        return "UNKNOWN";
    }
}
