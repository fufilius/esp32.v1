#include <stdint.h>

#include "dht22.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#define DHT22_GPIO GPIO_NUM_27
#define DHT22_START_SIGNAL_US 1100
#define DHT22_START_RELEASE_US 30
#define DHT22_RESPONSE_TIMEOUT_US 100
#define DHT22_BIT_TIMEOUT_US 120
#define DHT22_ONE_THRESHOLD_US 50

static const char *TAG = "dht22";

static esp_err_t dht22_wait_for_level(int level, uint32_t timeout_us)
{
    const int64_t start_us = esp_timer_get_time();

    while (gpio_get_level(DHT22_GPIO) != level) {
        if ((esp_timer_get_time() - start_us) > timeout_us) {
            return ESP_ERR_TIMEOUT;
        }
    }

    return ESP_OK;
}

void dht22_init(void)
{
    gpio_reset_pin(DHT22_GPIO);
    gpio_set_direction(DHT22_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(DHT22_GPIO, GPIO_PULLUP_ONLY);
    ESP_LOGD(TAG, "ready on GPIO%d", DHT22_GPIO);
}

esp_err_t dht22_read(dht22_reading_t *reading)
{
    if (reading == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    reading->temperature_c = 0.0f;
    reading->humidity_percent = 0.0f;
    reading->is_valid = false;
    reading->error = ESP_OK;
    reading->timestamp_us = esp_timer_get_time();

    uint8_t data[5] = {0};

    gpio_set_direction(DHT22_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT22_GPIO, 0);
    esp_rom_delay_us(DHT22_START_SIGNAL_US);
    gpio_set_level(DHT22_GPIO, 1);
    esp_rom_delay_us(DHT22_START_RELEASE_US);
    gpio_set_direction(DHT22_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(DHT22_GPIO, GPIO_PULLUP_ONLY);

    esp_err_t err = dht22_wait_for_level(0, DHT22_RESPONSE_TIMEOUT_US);
    if (err != ESP_OK) {
        return err;
    }

    err = dht22_wait_for_level(1, DHT22_RESPONSE_TIMEOUT_US);
    if (err != ESP_OK) {
        return err;
    }

    err = dht22_wait_for_level(0, DHT22_RESPONSE_TIMEOUT_US);
    if (err != ESP_OK) {
        return err;
    }

    for (int bit = 0; bit < 40; ++bit) {
        err = dht22_wait_for_level(1, DHT22_BIT_TIMEOUT_US);
        if (err != ESP_OK) {
            return err;
        }

        const int64_t high_start_us = esp_timer_get_time();

        err = dht22_wait_for_level(0, DHT22_BIT_TIMEOUT_US);
        if (err != ESP_OK) {
            return err;
        }

        const int64_t high_time_us = esp_timer_get_time() - high_start_us;
        if (high_time_us > DHT22_ONE_THRESHOLD_US) {
            data[bit / 8] |= (1 << (7 - (bit % 8)));
        }
    }

    const uint8_t checksum = data[0] + data[1] + data[2] + data[3];
    if (checksum != data[4]) {
        return ESP_ERR_INVALID_CRC;
    }

    const uint16_t humidity_raw = ((uint16_t)data[0] << 8) | data[1];
    const uint16_t temperature_raw = (((uint16_t)data[2] & 0x7f) << 8) | data[3];
    const float temperature_sign = (data[2] & 0x80) ? -1.0f : 1.0f;

    reading->humidity_percent = humidity_raw / 10.0f;
    reading->temperature_c = temperature_sign * (temperature_raw / 10.0f);
    reading->is_valid = true;
    reading->error = ESP_OK;
    reading->timestamp_us = esp_timer_get_time();

    return ESP_OK;
}
