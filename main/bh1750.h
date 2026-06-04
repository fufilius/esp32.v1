#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define BH1750_MEASUREMENT_DELAY_MS 180

typedef struct {
    float lux;
    bool is_valid;
    esp_err_t error;
    int64_t timestamp_us;
} bh1750_reading_t;

esp_err_t bh1750_init(void);
esp_err_t bh1750_read_lux(float *lux);
bh1750_reading_t bh1750_read(void);
