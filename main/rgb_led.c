#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_state.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "rgb_led.h"

#define LED_R_GPIO GPIO_NUM_25
#define LED_G_GPIO GPIO_NUM_26
#define LED_B_GPIO GPIO_NUM_33
#define LED_ACTIVE_LEVEL 1

#define LED_INACTIVE_LEVEL (!LED_ACTIVE_LEVEL)

typedef struct {
    system_state_t state;
    gpio_num_t gpio;
    uint32_t frequency_hz;
    const char *name;
} blink_profile_t;

static const char *TAG = "rgb_led";

static const blink_profile_t BLINK_PROFILES[SYSTEM_STATE_COUNT] = {
    [SYSTEM_STATE_OK] = {
        .state = SYSTEM_STATE_OK,
        .gpio = LED_G_GPIO,
        .frequency_hz = 1,
        .name = "WIFI CONNECTED: green 1 Hz",
    },
    [SYSTEM_STATE_WARNING] = {
        .state = SYSTEM_STATE_WARNING,
        .gpio = LED_B_GPIO,
        .frequency_hz = 2,
        .name = "WIFI SETUP AP: blue 2 Hz",
    },
    [SYSTEM_STATE_CRITICAL] = {
        .state = SYSTEM_STATE_CRITICAL,
        .gpio = LED_R_GPIO,
        .frequency_hz = 4,
        .name = "WIFI UNAVAILABLE: red 4 Hz",
    },
};

static void rgb_led_off(void)
{
    gpio_set_level(LED_R_GPIO, LED_INACTIVE_LEVEL);
    gpio_set_level(LED_G_GPIO, LED_INACTIVE_LEVEL);
    gpio_set_level(LED_B_GPIO, LED_INACTIVE_LEVEL);
}

void rgb_led_init(void)
{
    const gpio_num_t leds[] = {LED_R_GPIO, LED_G_GPIO, LED_B_GPIO};

    for (size_t i = 0; i < sizeof(leds) / sizeof(leds[0]); ++i) {
        gpio_reset_pin(leds[i]);
        gpio_set_direction(leds[i], GPIO_MODE_OUTPUT);
        gpio_set_level(leds[i], LED_INACTIVE_LEVEL);
    }
}

void rgb_blink_task(void *arg)
{
    QueueHandle_t state_queue = (QueueHandle_t)arg;
    system_state_t current_state = SYSTEM_STATE_CRITICAL;
    bool led_is_on = false;

    ESP_LOGD(TAG, "blink profile: %s", BLINK_PROFILES[current_state].name);

    while (true) {
        const blink_profile_t *profile = &BLINK_PROFILES[current_state];
        const TickType_t half_period_ticks =
            pdMS_TO_TICKS(1000 / (profile->frequency_hz * 2));
        system_state_t next_state;

        led_is_on = !led_is_on;
        gpio_set_level(profile->gpio, led_is_on ? LED_ACTIVE_LEVEL : LED_INACTIVE_LEVEL);

        if (xQueueReceive(state_queue, &next_state, half_period_ticks) == pdTRUE &&
            next_state != current_state) {
            rgb_led_off();
            led_is_on = false;
            current_state = next_state;
            ESP_LOGD(TAG, "blink profile: %s", BLINK_PROFILES[current_state].name);
        }
    }
}
