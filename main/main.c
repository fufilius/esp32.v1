#include <stdbool.h>

#include "app_state.h"
#include "bh1750.h"
#include "dht22.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "rgb_led.h"
#include "sdkconfig.h"

#if !CONFIG_IDF_TARGET_ESP32C3
#warning "This example is configured for ESP32-C3. Run: idf.py set-target esp32c3"
#endif

#define LIGHT_NONE_ENTER_LUX 1.0f
#define LIGHT_NONE_EXIT_LUX 2.0f
#define LIGHT_WEAK_ENTER_LUX 40.0f
#define LIGHT_WEAK_EXIT_LUX 60.0f
#define LIGHT_POLL_INTERVAL_MS 1000
#define DHT22_QUEUE_LENGTH 8

static const char *TAG = "app";
static QueueHandle_t s_state_queue;
static QueueHandle_t s_dht22_queue;

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

static void light_sensor_task(void *arg)
{
    QueueHandle_t state_queue = (QueueHandle_t)arg;
    system_state_t current_state = SYSTEM_STATE_CRITICAL;

    while (true) {
        float lux = 0.0f;
        esp_err_t err = bh1750_read_lux(&lux);

        if (err == ESP_OK) {
            const system_state_t state = light_state_from_lux(lux, current_state);

            if (state != current_state) {
                xQueueOverwrite(state_queue, &state);
                ESP_LOGI(TAG, "BH1750 light: %.2f lx -> %s", lux,
                         system_state_name(state));
                current_state = state;
            }
        } else {
            const system_state_t state = SYSTEM_STATE_CRITICAL;
            if (state != current_state) {
                xQueueOverwrite(state_queue, &state);
                current_state = state;
            }
            ESP_LOGE(TAG, "BH1750 read failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(LIGHT_POLL_INTERVAL_MS));
    }
}

static void dht22_worker_task(void *arg)
{
    QueueHandle_t reading_queue = (QueueHandle_t)arg;
    bool queue_full_reported = false;

    while (true) {
        dht22_reading_t reading;
        esp_err_t err = dht22_read(&reading);

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "DHT22 temperature: %.1f C, humidity: %.1f %%",
                     reading.temperature_c, reading.humidity_percent);

            if (xQueueSend(reading_queue, &reading, 0) != pdTRUE) {
                if (!queue_full_reported) {
                    ESP_LOGW(TAG, "DHT22 queue is full; readings will be dropped until a display task consumes them");
                    queue_full_reported = true;
                }
            } else {
                queue_full_reported = false;
            }
        } else {
            ESP_LOGE(TAG, "DHT22 read failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(DHT22_SAMPLE_INTERVAL_MS));
    }
}

void app_main(void)
{
    rgb_led_init();
    dht22_init();

    s_state_queue = xQueueCreate(1, sizeof(system_state_t));
    if (s_state_queue == NULL) {
        ESP_LOGE(TAG, "failed to create state queue");
        return;
    }

    s_dht22_queue = xQueueCreate(DHT22_QUEUE_LENGTH, sizeof(dht22_reading_t));
    if (s_dht22_queue == NULL) {
        ESP_LOGE(TAG, "failed to create DHT22 queue");
        return;
    }

    system_state_t initial_state = SYSTEM_STATE_CRITICAL;
    xQueueOverwrite(s_state_queue, &initial_state);

    const bool bh1750_ready = bh1750_init() == ESP_OK;
    if (!bh1750_ready) {
        ESP_LOGE(TAG, "BH1750 initialization failed");
    }

    xTaskCreate(rgb_blink_task, "rgb_blink", 2048, s_state_queue, 5, NULL);
    xTaskCreate(dht22_worker_task, "dht22_worker", 3072, s_dht22_queue, 4, NULL);
    if (bh1750_ready) {
        vTaskDelay(pdMS_TO_TICKS(BH1750_MEASUREMENT_DELAY_MS));
        xTaskCreate(light_sensor_task, "light_sensor", 3072, s_state_queue, 4, NULL);
    }
}
