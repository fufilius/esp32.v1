#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool light_valid;
    float light_lux;
    esp_err_t light_error;
    int64_t light_timestamp_us;

    bool dht22_valid;
    float temperature_c;
    float humidity_percent;
    esp_err_t dht22_error;
    int64_t dht22_timestamp_us;
} sensor_snapshot_t;

void sensor_store_update_light(float lux, bool is_valid, esp_err_t error,
                               int64_t timestamp_us);
void sensor_store_update_dht22(float temperature_c, float humidity_percent,
                               bool is_valid, esp_err_t error,
                               int64_t timestamp_us);
sensor_snapshot_t sensor_store_get_snapshot(void);
