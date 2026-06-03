#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if !CONFIG_IDF_TARGET_ESP32C3
#warning "This example is configured for ESP32-C3. Run: idf.py set-target esp32c3"
#endif

/*
 * ESP32-C3 GPIO example.
 * Change these pins for your board/wiring.
 * If your RGB LED is active-low, set LED_ACTIVE_LEVEL to 0.
 */
#define LED_R_GPIO GPIO_NUM_0
#define LED_G_GPIO GPIO_NUM_1
#define LED_B_GPIO GPIO_NUM_2
#define LED_ACTIVE_LEVEL 1

#define LED_INACTIVE_LEVEL (!LED_ACTIVE_LEVEL)

#define I2C_SDA_GPIO GPIO_NUM_6
#define I2C_SCL_GPIO GPIO_NUM_7
#define I2C_PORT I2C_NUM_0
#define I2C_TIMEOUT_MS 1000

#define BH1750_ADDR 0x23
#define BH1750_POWER_ON 0x01
#define BH1750_RESET 0x07
#define BH1750_CONT_H_RES_MODE 0x10
#define BH1750_MEASUREMENT_DELAY_MS 180

#define LIGHT_NONE_ENTER_LUX 1.0f
#define LIGHT_NONE_EXIT_LUX 2.0f
#define LIGHT_WEAK_ENTER_LUX 40.0f
#define LIGHT_WEAK_EXIT_LUX 60.0f
#define LIGHT_POLL_INTERVAL_MS 1000

typedef enum {
    SYSTEM_STATE_OK = 0,       // Green: enough light
    SYSTEM_STATE_WARNING,      // Blue: weak light
    SYSTEM_STATE_CRITICAL,     // Red: no light
    SYSTEM_STATE_COUNT
} system_state_t;

typedef struct {
    system_state_t state;
    gpio_num_t gpio;
    uint32_t frequency_hz;
    const char *name;
} blink_profile_t;

static const char *TAG = "rgb_state";
static QueueHandle_t s_state_queue;
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_bh1750;

static const blink_profile_t BLINK_PROFILES[SYSTEM_STATE_COUNT] = {
    [SYSTEM_STATE_OK] = {
        .state = SYSTEM_STATE_OK,
        .gpio = LED_G_GPIO,
        .frequency_hz = 1,
        .name = "LIGHT: green 1 Hz",
    },
    [SYSTEM_STATE_WARNING] = {
        .state = SYSTEM_STATE_WARNING,
        .gpio = LED_B_GPIO,
        .frequency_hz = 2,
        .name = "WEAK LIGHT: blue 2 Hz",
    },
    [SYSTEM_STATE_CRITICAL] = {
        .state = SYSTEM_STATE_CRITICAL,
        .gpio = LED_R_GPIO,
        .frequency_hz = 4,
        .name = "NO LIGHT: red 4 Hz",
    },
};

static void rgb_leds_init(void)
{
    const gpio_num_t leds[] = {LED_R_GPIO, LED_G_GPIO, LED_B_GPIO};

    for (size_t i = 0; i < sizeof(leds) / sizeof(leds[0]); ++i) {
        gpio_reset_pin(leds[i]);
        gpio_set_direction(leds[i], GPIO_MODE_OUTPUT);
        gpio_set_level(leds[i], LED_INACTIVE_LEVEL);
    }
}

static void rgb_leds_off(void)
{
    gpio_set_level(LED_R_GPIO, LED_INACTIVE_LEVEL);
    gpio_set_level(LED_G_GPIO, LED_INACTIVE_LEVEL);
    gpio_set_level(LED_B_GPIO, LED_INACTIVE_LEVEL);
}

static esp_err_t bh1750_write_cmd(uint8_t command)
{
    return i2c_master_transmit(s_bh1750, &command, sizeof(command),
                               pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static esp_err_t bh1750_read_lux(float *lux)
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

static esp_err_t bh1750_init(void)
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

    ESP_LOGI(TAG, "BH1750 ready on I2C SDA GPIO%d, SCL GPIO%d, address 0x%02x",
             I2C_SDA_GPIO, I2C_SCL_GPIO, BH1750_ADDR);
    return ESP_OK;
}

static system_state_t light_state_from_lux(float lux, system_state_t current_state)
{
    if (current_state == SYSTEM_STATE_CRITICAL) {
        return lux > LIGHT_NONE_EXIT_LUX ? SYSTEM_STATE_WARNING : SYSTEM_STATE_CRITICAL;
    }

    if (current_state == SYSTEM_STATE_OK) {
        return lux < LIGHT_WEAK_ENTER_LUX ? SYSTEM_STATE_WARNING : SYSTEM_STATE_OK;
    }

    if (lux < LIGHT_NONE_ENTER_LUX) {
        return SYSTEM_STATE_CRITICAL;
    }

    if (lux > LIGHT_WEAK_EXIT_LUX) {
        return SYSTEM_STATE_OK;
    }

    return SYSTEM_STATE_WARNING;
}

static void rgb_blink_task(void *arg)
{
    (void)arg;

    system_state_t current_state = SYSTEM_STATE_OK;
    bool led_is_on = false;

    ESP_LOGI(TAG, "blink profile: %s", BLINK_PROFILES[current_state].name);

    while (true) {
        const blink_profile_t *profile = &BLINK_PROFILES[current_state];
        const TickType_t half_period_ticks =
            pdMS_TO_TICKS(1000 / (profile->frequency_hz * 2));
        system_state_t next_state;

        led_is_on = !led_is_on;
        gpio_set_level(profile->gpio, led_is_on ? LED_ACTIVE_LEVEL : LED_INACTIVE_LEVEL);

        if (xQueueReceive(s_state_queue, &next_state, half_period_ticks) == pdTRUE &&
            next_state != current_state) {
            rgb_leds_off();
            led_is_on = false;
            current_state = next_state;
            ESP_LOGI(TAG, "blink profile: %s", BLINK_PROFILES[current_state].name);
        }
    }
}

static void light_sensor_task(void *arg)
{
    (void)arg;
    system_state_t current_state = SYSTEM_STATE_CRITICAL;

    while (true) {
        float lux = 0.0f;
        esp_err_t err = bh1750_read_lux(&lux);

        if (err == ESP_OK) {
            const system_state_t state = light_state_from_lux(lux, current_state);

            if (state != current_state) {
                xQueueOverwrite(s_state_queue, &state);
                ESP_LOGI(TAG, "BH1750 light: %.2f lx -> %s", lux,
                         BLINK_PROFILES[state].name);
                current_state = state;
            }
        } else {
            const system_state_t state = SYSTEM_STATE_CRITICAL;
            if (state != current_state) {
                xQueueOverwrite(s_state_queue, &state);
                current_state = state;
            }
            ESP_LOGE(TAG, "BH1750 read failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(LIGHT_POLL_INTERVAL_MS));
    }
}

void app_main(void)
{
    rgb_leds_init();

    s_state_queue = xQueueCreate(1, sizeof(system_state_t));
    if (s_state_queue == NULL) {
        ESP_LOGE(TAG, "failed to create state queue");
        return;
    }

    system_state_t initial_state = SYSTEM_STATE_CRITICAL;
    xQueueOverwrite(s_state_queue, &initial_state);

    const bool bh1750_ready = bh1750_init() == ESP_OK;
    if (!bh1750_ready) {
        ESP_LOGE(TAG, "BH1750 initialization failed");
    }

    xTaskCreate(rgb_blink_task, "rgb_blink", 2048, NULL, 5, NULL);
    if (bh1750_ready) {
        vTaskDelay(pdMS_TO_TICKS(BH1750_MEASUREMENT_DELAY_MS));
        xTaskCreate(light_sensor_task, "light_sensor", 3072, NULL, 4, NULL);
    }
}
