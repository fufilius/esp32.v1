#pragma once

#include "esp_err.h"

#define BH1750_MEASUREMENT_DELAY_MS 180

esp_err_t bh1750_init(void);
esp_err_t bh1750_read_lux(float *lux);
