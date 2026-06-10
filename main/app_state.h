#pragma once

typedef enum {
    SYSTEM_STATE_OK = 0,       // Green: enough light
    SYSTEM_STATE_WARNING,      // Blue: weak light
    SYSTEM_STATE_CRITICAL,     // Red: no light
    SYSTEM_STATE_COUNT
} system_state_t;

static inline const char *system_state_name(system_state_t state)
{
    switch (state) {
    case SYSTEM_STATE_OK:
        return "LIGHT";
    case SYSTEM_STATE_WARNING:
        return "WEAK LIGHT";
    case SYSTEM_STATE_CRITICAL:
        return "NO LIGHT";
    default:
        return "UNKNOWN";
    }
}
