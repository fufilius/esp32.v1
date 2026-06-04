#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    NETWORK_EVENT_CONNECTING = 0,
    NETWORK_EVENT_CONNECTED,
    NETWORK_EVENT_LOST,
} network_event_type_t;

typedef struct {
    network_event_type_t type;
    int32_t event_id;
    int64_t timestamp_us;
} network_event_t;

esp_err_t network_events_init(QueueHandle_t event_queue);
