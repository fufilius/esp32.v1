#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define DHT22_SAMPLE_INTERVAL_MS 5000

typedef struct {
    float temperature_c;
    float humidity_percent;
    bool is_valid;
    esp_err_t error;
    int64_t timestamp_us;
} dht22_reading_t;

void dht22_init(void);
esp_err_t dht22_read(dht22_reading_t *reading);
