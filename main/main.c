#include <stdbool.h>

#include "app_state.h"
#include "bh1750.h"
#include "dht22.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "network_events.h"
#include "nvs_flash.h"
#include "rgb_led.h"
#include "sdkconfig.h"
#include "sensor_store.h"
#include "wifi_manager.h"

#if !CONFIG_IDF_TARGET_ESP32
#warning "This example is configured for ESP32. Run: idf.py set-target esp32"
#endif

#define LIGHT_POLL_INTERVAL_MS 1000
#define SENSOR_WAIT_TIMEOUT_MS 1500
#define DHT22_QUEUE_LENGTH 8
#define NETWORK_EVENT_QUEUE_LENGTH 8
#define NETWORK_CONNECT_WAIT_MS 500
#define NETWORK_RECOVERY_DELAY_MS 2000
#define WIFI_RESET_BUTTON_GPIO GPIO_NUM_0
#define WIFI_RESET_BUTTON_ACTIVE_LEVEL 0
#define WIFI_RESET_BUTTON_HOLD_MS 3000
#define WIFI_RESET_BUTTON_POLL_MS 50

typedef enum {
    ST_INIT = 0,
    ST_CONNECTING,
    ST_WAIT_SENSOR_DATA,
    ST_PROCESS_SENSOR_DATA,
    ST_UPDATE_OUTPUT,
    ST_RECOVERY,
    ST_ERROR,
    ST_COUNT
} app_run_state_t;

typedef struct {
    bh1750_reading_t light;
    dht22_reading_t dht22;
    bool has_light;
    bool has_dht22;
    bool has_reported_light;
    bool has_reported_dht22;
    bh1750_reading_t last_reported_light;
    dht22_reading_t last_reported_dht22;
    bool network_connected;
    system_state_t rgb_state;
    system_state_t last_sent_rgb_state;
} app_context_t;

static const char *TAG = "app";
static QueueHandle_t s_rgb_state_queue;
static QueueHandle_t s_light_queue;
static QueueHandle_t s_dht22_queue;
static QueueHandle_t s_network_event_queue;

static void send_rgb_state(app_context_t *ctx, system_state_t state);

static const char *run_state_name(app_run_state_t state)
{
    switch (state) {
    case ST_INIT:
        return "ST_INIT";
    case ST_CONNECTING:
        return "ST_CONNECTING";
    case ST_WAIT_SENSOR_DATA:
        return "ST_WAIT_SENSOR_DATA";
    case ST_PROCESS_SENSOR_DATA:
        return "ST_PROCESS_SENSOR_DATA";
    case ST_UPDATE_OUTPUT:
        return "ST_UPDATE_OUTPUT";
    case ST_RECOVERY:
        return "ST_RECOVERY";
    case ST_ERROR:
        return "ST_ERROR";
    default:
        return "ST_UNKNOWN";
    }
}

static app_run_state_t transition_to(app_run_state_t current_state,
                                     app_run_state_t next_state)
{
    if (current_state != next_state) {
        ESP_LOGD(TAG, "%s -> %s", run_state_name(current_state),
                 run_state_name(next_state));
    }

    return next_state;
}

static void bh1750_worker_task(void *arg)
{
    QueueHandle_t light_queue = (QueueHandle_t)arg;

    while (true) {
        bh1750_reading_t reading = bh1750_read();
        xQueueOverwrite(light_queue, &reading);

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

        if (err != ESP_OK) {
            reading.is_valid = false;
            reading.error = err;
            reading.timestamp_us = esp_timer_get_time();
            ESP_LOGD(TAG, "DHT22 read failed: %s", esp_err_to_name(err));
            sensor_store_update_dht22(0.0f, 0.0f, false, err, esp_timer_get_time());
        } else {
            ESP_LOGD(TAG, "DHT22 temperature: %.1f C, humidity: %.1f %%",
                     reading.temperature_c, reading.humidity_percent);
            sensor_store_update_dht22(reading.temperature_c, reading.humidity_percent,
                                      reading.is_valid, reading.error,
                                      reading.timestamp_us);
        }

        if (xQueueSend(reading_queue, &reading, 0) != pdTRUE) {
            if (!queue_full_reported) {
                ESP_LOGW(TAG, "DHT22 queue is full; readings will be dropped");
                queue_full_reported = true;
            }
        } else {
            queue_full_reported = false;
        }

        vTaskDelay(pdMS_TO_TICKS(DHT22_SAMPLE_INTERVAL_MS));
    }
}

static void wifi_reset_button_task(void *arg)
{
    (void)arg;

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << WIFI_RESET_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&config);

    int held_ms = 0;
    bool reset_sent = false;

    while (true) {
        const bool pressed =
            gpio_get_level(WIFI_RESET_BUTTON_GPIO) == WIFI_RESET_BUTTON_ACTIVE_LEVEL;

        if (pressed) {
            held_ms += WIFI_RESET_BUTTON_POLL_MS;
            if (!reset_sent && held_ms >= WIFI_RESET_BUTTON_HOLD_MS) {
                ESP_LOGW(TAG, "Wi-Fi reset button held; clearing saved Wi-Fi");
                if (wifi_manager_reset_credentials() != ESP_OK) {
                    ESP_LOGE(TAG, "manual Wi-Fi reset failed");
                }
                reset_sent = true;
            }
        } else {
            held_ms = 0;
            reset_sent = false;
        }

        vTaskDelay(pdMS_TO_TICKS(WIFI_RESET_BUTTON_POLL_MS));
    }
}

static void drain_dht22_queue(app_context_t *ctx)
{
    dht22_reading_t reading;

    while (xQueueReceive(s_dht22_queue, &reading, 0) == pdTRUE) {
        ctx->dht22 = reading;
        ctx->has_dht22 = true;
    }
}

static bool has_invalid_sensor_data(const app_context_t *ctx)
{
    if (ctx->has_light && !ctx->light.is_valid) {
        ESP_LOGE(TAG, "invalid BH1750 data: %s", esp_err_to_name(ctx->light.error));
        return true;
    }

    if (ctx->has_dht22 && !ctx->dht22.is_valid) {
        ESP_LOGE(TAG, "invalid DHT22 data: %s", esp_err_to_name(ctx->dht22.error));
        return true;
    }

    return false;
}

static bool float_changed(float current, float previous, float threshold)
{
    const float diff = current > previous ? current - previous : previous - current;
    return diff >= threshold;
}

static bool light_report_changed(const app_context_t *ctx)
{
    if (!ctx->has_light) {
        return false;
    }
    if (!ctx->has_reported_light) {
        return true;
    }
    if (ctx->light.is_valid != ctx->last_reported_light.is_valid ||
        ctx->light.error != ctx->last_reported_light.error) {
        return true;
    }

    return ctx->light.is_valid &&
           float_changed(ctx->light.lux, ctx->last_reported_light.lux, 0.5f);
}

static bool dht22_report_changed(const app_context_t *ctx)
{
    if (!ctx->has_dht22) {
        return false;
    }
    if (!ctx->has_reported_dht22) {
        return true;
    }
    if (ctx->dht22.is_valid != ctx->last_reported_dht22.is_valid ||
        ctx->dht22.error != ctx->last_reported_dht22.error) {
        return true;
    }

    return ctx->dht22.is_valid &&
           (float_changed(ctx->dht22.temperature_c,
                          ctx->last_reported_dht22.temperature_c, 0.2f) ||
            float_changed(ctx->dht22.humidity_percent,
                          ctx->last_reported_dht22.humidity_percent, 0.5f));
}

static void log_sensor_summary_if_changed(app_context_t *ctx)
{
    if (!light_report_changed(ctx) && !dht22_report_changed(ctx)) {
        return;
    }

    char light_status[80];
    char dht22_status[96];

    if (!ctx->has_light) {
        snprintf(light_status, sizeof(light_status), "BH1750=pending");
    } else if (ctx->light.is_valid) {
        snprintf(light_status, sizeof(light_status), "BH1750=%.2f lx", ctx->light.lux);
    } else {
        snprintf(light_status, sizeof(light_status), "BH1750=invalid(%s)",
                 esp_err_to_name(ctx->light.error));
    }

    if (!ctx->has_dht22) {
        snprintf(dht22_status, sizeof(dht22_status), "DHT22=pending");
    } else if (ctx->dht22.is_valid) {
        snprintf(dht22_status, sizeof(dht22_status), "DHT22=%.1f C %.1f %%",
                 ctx->dht22.temperature_c, ctx->dht22.humidity_percent);
    } else {
        snprintf(dht22_status, sizeof(dht22_status), "DHT22=invalid(%s)",
                 esp_err_to_name(ctx->dht22.error));
    }

    ESP_LOGI(TAG, "sensors: %s, %s", light_status, dht22_status);

    if (ctx->has_light) {
        ctx->last_reported_light = ctx->light;
        ctx->has_reported_light = true;
    }
    if (ctx->has_dht22) {
        ctx->last_reported_dht22 = ctx->dht22;
        ctx->has_reported_dht22 = true;
    }
}

static bool handle_network_event(app_context_t *ctx, const network_event_t *event)
{
    switch (event->type) {
    case NETWORK_EVENT_CONNECTING:
        ctx->network_connected = false;
        ctx->rgb_state = SYSTEM_STATE_WARNING;
        send_rgb_state(ctx, ctx->rgb_state);
        ESP_LOGD(TAG, "network is connecting");
        return false;

    case NETWORK_EVENT_CONNECTED:
        ctx->network_connected = true;
        ctx->rgb_state = SYSTEM_STATE_OK;
        send_rgb_state(ctx, ctx->rgb_state);
        ESP_LOGD(TAG, "network connected via event %ld", event->event_id);
        return false;

    case NETWORK_EVENT_LOST:
        ctx->network_connected = false;
        ctx->rgb_state = SYSTEM_STATE_CRITICAL;
        send_rgb_state(ctx, ctx->rgb_state);
        ESP_LOGW(TAG, "network lost via event %ld", event->event_id);
        return true;

    default:
        return false;
    }
}

static bool poll_network_events(app_context_t *ctx)
{
    network_event_t event;
    bool network_lost = false;

    while (xQueueReceive(s_network_event_queue, &event, 0) == pdTRUE) {
        if (handle_network_event(ctx, &event)) {
            network_lost = true;
        }
    }

    return network_lost && !ctx->network_connected;
}

static bool wait_for_network_event(app_context_t *ctx, TickType_t timeout_ticks)
{
    network_event_t event;
    bool network_lost = false;

    if (xQueueReceive(s_network_event_queue, &event, timeout_ticks) == pdTRUE) {
        if (handle_network_event(ctx, &event)) {
            network_lost = true;
        }
    }

    return (poll_network_events(ctx) || network_lost) && !ctx->network_connected;
}

static void send_rgb_state(app_context_t *ctx, system_state_t state)
{
    if (ctx->last_sent_rgb_state != state) {
        xQueueOverwrite(s_rgb_state_queue, &state);
        ctx->last_sent_rgb_state = state;
        ESP_LOGD(TAG, "RGB state -> %s", system_state_name(state));
    }
}

static void app_controller_task(void *arg)
{
    (void)arg;

    app_context_t ctx = {
        .has_light = false,
        .has_dht22 = false,
        .has_reported_light = false,
        .has_reported_dht22 = false,
        .network_connected = false,
        .rgb_state = SYSTEM_STATE_CRITICAL,
        .last_sent_rgb_state = SYSTEM_STATE_COUNT,
    };
    app_run_state_t state = ST_INIT;

    while (true) {
        switch (state) {
        case ST_INIT:
            send_rgb_state(&ctx, SYSTEM_STATE_CRITICAL);
            state = transition_to(state, ST_CONNECTING);
            break;

        case ST_CONNECTING:
            if (wait_for_network_event(&ctx, pdMS_TO_TICKS(NETWORK_CONNECT_WAIT_MS))) {
                state = transition_to(state, ST_RECOVERY);
                break;
            }

            if (!ctx.network_connected) {
                ESP_LOGD(TAG, "network is not connected yet; continuing sensor loop");
            }

            state = transition_to(state, ST_WAIT_SENSOR_DATA);
            break;

        case ST_WAIT_SENSOR_DATA:
            if (poll_network_events(&ctx)) {
                state = transition_to(state, ST_RECOVERY);
                break;
            }

            if (xQueueReceive(s_light_queue, &ctx.light,
                              pdMS_TO_TICKS(SENSOR_WAIT_TIMEOUT_MS)) == pdTRUE) {
                ctx.has_light = true;
                sensor_store_update_light(ctx.light.lux, ctx.light.is_valid,
                                          ctx.light.error, ctx.light.timestamp_us);
            } else {
                ctx.light = (bh1750_reading_t) {
                    .lux = 0.0f,
                    .is_valid = false,
                    .error = ESP_ERR_TIMEOUT,
                    .timestamp_us = 0,
                };
                ctx.has_light = true;
                sensor_store_update_light(ctx.light.lux, ctx.light.is_valid,
                                          ctx.light.error, ctx.light.timestamp_us);
            }

            drain_dht22_queue(&ctx);
            state = transition_to(state, poll_network_events(&ctx) ? ST_RECOVERY
                                                                    : ST_PROCESS_SENSOR_DATA);
            break;

        case ST_PROCESS_SENSOR_DATA:
            if (poll_network_events(&ctx)) {
                state = transition_to(state, ST_RECOVERY);
                break;
            }

            log_sensor_summary_if_changed(&ctx);

            if (has_invalid_sensor_data(&ctx)) {
                state = transition_to(state, ST_ERROR);
                break;
            }

            send_rgb_state(&ctx, ctx.rgb_state);
            ESP_LOGD(TAG, "BH1750 light: %.2f lx", ctx.light.lux);
            state = transition_to(state, ST_UPDATE_OUTPUT);
            break;

        case ST_UPDATE_OUTPUT:
            if (poll_network_events(&ctx)) {
                state = transition_to(state, ST_RECOVERY);
                break;
            }

            state = transition_to(state, ST_WAIT_SENSOR_DATA);
            break;

        case ST_RECOVERY:
            send_rgb_state(&ctx, SYSTEM_STATE_CRITICAL);
            ESP_LOGW(TAG, "network recovery mode");
            vTaskDelay(pdMS_TO_TICKS(NETWORK_RECOVERY_DELAY_MS));
            state = transition_to(state, ST_CONNECTING);
            break;

        case ST_ERROR:
            send_rgb_state(&ctx, SYSTEM_STATE_CRITICAL);
            state = transition_to(state, ST_WAIT_SENSOR_DATA);
            break;

        default:
            state = transition_to(state, ST_ERROR);
            break;
        }
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("app", ESP_LOG_INFO);
    esp_log_level_set("wifi_manager", ESP_LOG_INFO);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize NVS: %s", esp_err_to_name(err));
        return;
    }

    rgb_led_init();
    dht22_init();

    s_rgb_state_queue = xQueueCreate(1, sizeof(system_state_t));
    if (s_rgb_state_queue == NULL) {
        ESP_LOGE(TAG, "failed to create RGB state queue");
        return;
    }

    s_light_queue = xQueueCreate(1, sizeof(bh1750_reading_t));
    if (s_light_queue == NULL) {
        ESP_LOGE(TAG, "failed to create BH1750 queue");
        return;
    }

    s_dht22_queue = xQueueCreate(DHT22_QUEUE_LENGTH, sizeof(dht22_reading_t));
    if (s_dht22_queue == NULL) {
        ESP_LOGE(TAG, "failed to create DHT22 queue");
        return;
    }

    s_network_event_queue = xQueueCreate(NETWORK_EVENT_QUEUE_LENGTH,
                                         sizeof(network_event_t));
    if (s_network_event_queue == NULL) {
        ESP_LOGE(TAG, "failed to create network event queue");
        return;
    }

    if (network_events_init(s_network_event_queue) != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize network events");
    }
    if (wifi_manager_init(s_network_event_queue) != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize Wi-Fi manager");
    }

    const bool bh1750_ready = bh1750_init() == ESP_OK;
    if (!bh1750_ready) {
        ESP_LOGE(TAG, "BH1750 initialization failed");
    }

    xTaskCreate(rgb_blink_task, "rgb_blink", 2048, s_rgb_state_queue, 5, NULL);
    xTaskCreate(app_controller_task, "app_controller", 4096, NULL, 6, NULL);
    xTaskCreate(dht22_worker_task, "dht22_worker", 3072, s_dht22_queue, 4, NULL);
    xTaskCreate(wifi_reset_button_task, "wifi_reset_button", 2048, NULL, 4, NULL);

    if (bh1750_ready) {
        vTaskDelay(pdMS_TO_TICKS(BH1750_MEASUREMENT_DELAY_MS));
        xTaskCreate(bh1750_worker_task, "bh1750_worker", 3072, s_light_queue, 4, NULL);
    }
}
