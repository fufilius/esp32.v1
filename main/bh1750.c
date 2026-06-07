#include <stdint.h>

#include "bh1750.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define I2C_SDA_GPIO GPIO_NUM_21
#define I2C_SCL_GPIO GPIO_NUM_22
#define I2C_PORT I2C_NUM_0
#define I2C_TIMEOUT_MS 1000

#define BH1750_ADDR 0x23
#define BH1750_POWER_ON 0x01
#define BH1750_RESET 0x07
#define BH1750_CONT_H_RES_MODE 0x10

static const char *TAG = "bh1750";
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_bh1750;

static esp_err_t bh1750_write_cmd(uint8_t command)
{
    return i2c_master_transmit(s_bh1750, &command, sizeof(command),
                               pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

esp_err_t bh1750_read_lux(float *lux)
{
    uint8_t data[2] = {0};
    esp_err_t err = i2c_master_receive(s_bh1750, data, sizeof(data),
                                       pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    if (err != ESP_OK) {
        return err;
    }

    const uint16_t raw = ((uint16_t)data[0] << 8) | data[1];
    *lux = raw / 1.2f;
    return ESP_OK;
}

bh1750_reading_t bh1750_read(void)
{
    bh1750_reading_t reading = {
        .lux = 0.0f,
        .is_valid = false,
        .error = ESP_OK,
        .timestamp_us = esp_timer_get_time(),
    };

    reading.error = bh1750_read_lux(&reading.lux);
    reading.is_valid = reading.error == ESP_OK;
    reading.timestamp_us = esp_timer_get_time();

    return reading;
}

esp_err_t bh1750_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &s_i2c_bus), TAG,
                        "failed to create I2C bus");

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BH1750_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_config, &s_bh1750),
                        TAG, "failed to add BH1750 device");

    ESP_RETURN_ON_ERROR(bh1750_write_cmd(BH1750_POWER_ON), TAG,
                        "failed to power on BH1750");
    ESP_RETURN_ON_ERROR(bh1750_write_cmd(BH1750_RESET), TAG,
                        "failed to reset BH1750");
    ESP_RETURN_ON_ERROR(bh1750_write_cmd(BH1750_CONT_H_RES_MODE), TAG,
                        "failed to start BH1750 measurement");

    ESP_LOGI(TAG, "ready on I2C SDA GPIO%d, SCL GPIO%d, address 0x%02x",
             I2C_SDA_GPIO, I2C_SCL_GPIO, BH1750_ADDR);
    return ESP_OK;
}
