#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/*
 * Change these pins for your board/wiring.
 * If your RGB LED is active-low, set LED_ACTIVE_LEVEL to 0.
 */
#define LED_R_GPIO GPIO_NUM_4
#define LED_G_GPIO GPIO_NUM_5
#define LED_B_GPIO GPIO_NUM_6
#define LED_ACTIVE_LEVEL 1

#define LED_INACTIVE_LEVEL (!LED_ACTIVE_LEVEL)

typedef enum {
    SYSTEM_STATE_OK = 0,       // Green, 1 Hz
    SYSTEM_STATE_WARNING,      // Blue, 2 Hz
    SYSTEM_STATE_CRITICAL,     // Red, 4 Hz
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

static const blink_profile_t BLINK_PROFILES[SYSTEM_STATE_COUNT] = {
    [SYSTEM_STATE_OK] = {
        .state = SYSTEM_STATE_OK,
        .gpio = LED_G_GPIO,
        .frequency_hz = 1,
        .name = "OK: green 1 Hz",
    },
    [SYSTEM_STATE_WARNING] = {
        .state = SYSTEM_STATE_WARNING,
        .gpio = LED_B_GPIO,
        .frequency_hz = 2,
        .name = "WARNING: blue 2 Hz",
    },
    [SYSTEM_STATE_CRITICAL] = {
        .state = SYSTEM_STATE_CRITICAL,
        .gpio = LED_R_GPIO,
        .frequency_hz = 4,
        .name = "CRITICAL: red 4 Hz",
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

static void fake_system_state_task(void *arg)
{
    (void)arg;
    system_state_t fake_state = SYSTEM_STATE_OK;

    xQueueOverwrite(s_state_queue, &fake_state);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        fake_state = (fake_state + 1) % SYSTEM_STATE_COUNT;
        xQueueOverwrite(s_state_queue, &fake_state);
        ESP_LOGI(TAG, "fake system state -> %s", BLINK_PROFILES[fake_state].name);
    }
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

void app_main(void)
{
    rgb_leds_init();

    s_state_queue = xQueueCreate(1, sizeof(system_state_t));
    if (s_state_queue == NULL) {
        ESP_LOGE(TAG, "failed to create state queue");
        return;
    }

    xTaskCreate(fake_system_state_task, "fake_system_state", 2048, NULL, 4, NULL);
    xTaskCreate(rgb_blink_task, "rgb_blink", 2048, NULL, 5, NULL);
}
