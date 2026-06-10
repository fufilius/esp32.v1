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
#include "mqtt_manager.h"
#include "network_events.h"
#include "nvs_flash.h"
#include "rgb_led.h"
#include "sdkconfig.h"

#if !CONFIG_IDF_TARGET_ESP32
#warning "This example is configured for ESP32. Run: idf.py set-target esp32"
#endif

#define LIGHT_POLL_INTERVAL_MS 1000
#define SENSOR_WAIT_TIMEOUT_MS 1500
#define DHT22_QUEUE_LENGTH 8
#define NETWORK_EVENT_QUEUE_LENGTH 8
#define NETWORK_CONNECT_WAIT_MS 500
#define NETWORK_RECOVERY_DELAY_MS 2000
#define DHT22_STARTUP_DELAY_MS 6000
#define DHT22_MAX_CONSECUTIVE_FAILURES 3
#define SETTINGS_RESET_BUTTON_GPIO GPIO_NUM_0
#define SETTINGS_RESET_BUTTON_ACTIVE_LEVEL 0
#define SETTINGS_RESET_BUTTON_HOLD_MS 3000
#define SETTINGS_RESET_BUTTON_POLL_MS 50

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
    int light_lux;
    int temperature_c;
    int humidity_percent;
    bool light_valid;
    bool dht22_valid;
    bool has_light;
    bool has_dht22;
} rounded_sensor_snapshot_t;

typedef struct {
    bh1750_reading_t light;
    dht22_reading_t dht22;
    bool has_light;
    bool has_dht22;
    bool dht22_updated;
    bool network_connected;
    system_state_t rgb_state;
    system_state_t last_sent_rgb_state;
    rounded_sensor_snapshot_t last_logged_sensors;
    rounded_sensor_snapshot_t last_published_sensors;
    bool has_logged_sensors;
    bool has_published_sensors;
} app_context_t;

static const char *TAG = "app";
static QueueHandle_t s_rgb_state_queue;
static QueueHandle_t s_light_queue;
static QueueHandle_t s_dht22_queue;
static QueueHandle_t s_network_event_queue;

static void send_rgb_state(app_context_t *ctx, system_state_t state);
static system_state_t system_state_from_status(const app_context_t *ctx);

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
    bool has_last_valid = false;
    dht22_reading_t last_valid = {0};
    int consecutive_failures = 0;

    vTaskDelay(pdMS_TO_TICKS(DHT22_STARTUP_DELAY_MS));

    while (true) {
        dht22_reading_t reading;
        esp_err_t err = dht22_read(&reading);

        if (err != ESP_OK) {
            consecutive_failures++;
            ESP_LOGD(TAG, "DHT22 read failed %d/%d: %s",
                     consecutive_failures, DHT22_MAX_CONSECUTIVE_FAILURES,
                     esp_err_to_name(err));

            if (has_last_valid &&
                consecutive_failures < DHT22_MAX_CONSECUTIVE_FAILURES) {
                reading = last_valid;
            } else {
                reading.is_valid = false;
                reading.error = err;
            }
        } else {
            consecutive_failures = 0;
            has_last_valid = true;
            last_valid = reading;
            ESP_LOGD(TAG, "DHT22 temperature: %.1f C, humidity: %.1f %%",
                     reading.temperature_c, reading.humidity_percent);
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

static system_state_t system_state_from_status(const app_context_t *ctx);

static void settings_reset_button_task(void *arg)
{
    (void)arg;

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << SETTINGS_RESET_BUTTON_GPIO,
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
            gpio_get_level(SETTINGS_RESET_BUTTON_GPIO) ==
            SETTINGS_RESET_BUTTON_ACTIVE_LEVEL;

        if (pressed) {
            held_ms += SETTINGS_RESET_BUTTON_POLL_MS;
            if (!reset_sent && held_ms >= SETTINGS_RESET_BUTTON_HOLD_MS) {
                ESP_LOGW(TAG, "BOOT held; clearing Wi-Fi/MQTT settings");
                if (mqtt_manager_reset_settings() != ESP_OK) {
                    ESP_LOGE(TAG, "manual settings reset failed");
                }
                reset_sent = true;
            }
        } else {
            held_ms = 0;
            reset_sent = false;
        }

        vTaskDelay(pdMS_TO_TICKS(SETTINGS_RESET_BUTTON_POLL_MS));
    }
}

static void drain_dht22_queue(app_context_t *ctx)
{
    dht22_reading_t reading;

    ctx->dht22_updated = false;
    while (xQueueReceive(s_dht22_queue, &reading, 0) == pdTRUE) {
        ctx->dht22 = reading;
        ctx->has_dht22 = true;
        ctx->dht22_updated = true;
    }
}

static bool has_invalid_sensor_data(const app_context_t *ctx)
{
    if (ctx->has_light && !ctx->light.is_valid) {
        return true;
    }

    if (ctx->has_dht22 && !ctx->dht22.is_valid) {
        return true;
    }

    return false;
}

static bool handle_network_event(app_context_t *ctx, const network_event_t *event)
{
    switch (event->type) {
    case NETWORK_EVENT_CONNECTING:
        ctx->network_connected = false;
        ctx->rgb_state = system_state_from_status(ctx);
        send_rgb_state(ctx, ctx->rgb_state);
        ESP_LOGD(TAG, "network is connecting");
        return false;

    case NETWORK_EVENT_CONNECTED:
        ctx->network_connected = true;
        ctx->rgb_state = system_state_from_status(ctx);
        send_rgb_state(ctx, ctx->rgb_state);
        ESP_LOGI(TAG, "network connected");
        return false;

    case NETWORK_EVENT_LOST:
        ctx->network_connected = false;
        ctx->rgb_state = system_state_from_status(ctx);
        send_rgb_state(ctx, ctx->rgb_state);
        ESP_LOGD(TAG, "network lost via event %ld", event->event_id);
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

    return network_lost;
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

    return poll_network_events(ctx) || network_lost;
}

static void send_rgb_state(app_context_t *ctx, system_state_t state)
{
    if (ctx->last_sent_rgb_state != state) {
        xQueueOverwrite(s_rgb_state_queue, &state);
        ctx->last_sent_rgb_state = state;
        ESP_LOGD(TAG, "RGB state -> %s", system_state_name(state));
    }
}

static int round_float_to_int(float value)
{
    return value >= 0.0f ? (int)(value + 0.5f) : (int)(value - 0.5f);
}

static rounded_sensor_snapshot_t rounded_sensor_snapshot_from_context(
    const app_context_t *ctx)
{
    return (rounded_sensor_snapshot_t) {
        .light_lux = ctx->has_light && ctx->light.is_valid
            ? round_float_to_int(ctx->light.lux)
            : 0,
        .temperature_c = ctx->has_dht22 && ctx->dht22.is_valid
            ? round_float_to_int(ctx->dht22.temperature_c)
            : 0,
        .humidity_percent = ctx->has_dht22 && ctx->dht22.is_valid
            ? round_float_to_int(ctx->dht22.humidity_percent)
            : 0,
        .light_valid = ctx->has_light && ctx->light.is_valid,
        .dht22_valid = ctx->has_dht22 && ctx->dht22.is_valid,
        .has_light = ctx->has_light,
        .has_dht22 = ctx->has_dht22,
    };
}

static bool rounded_sensor_snapshot_changed(const rounded_sensor_snapshot_t *current,
                                            const rounded_sensor_snapshot_t *previous)
{
    return current->light_lux != previous->light_lux ||
           current->temperature_c != previous->temperature_c ||
           current->humidity_percent != previous->humidity_percent ||
           current->light_valid != previous->light_valid ||
           current->dht22_valid != previous->dht22_valid ||
           current->has_light != previous->has_light ||
           current->has_dht22 != previous->has_dht22;
}

static void log_sensor_summary_if_changed(app_context_t *ctx)
{
    const rounded_sensor_snapshot_t snapshot =
        rounded_sensor_snapshot_from_context(ctx);
    if (ctx->has_logged_sensors &&
        !rounded_sensor_snapshot_changed(&snapshot, &ctx->last_logged_sensors)) {
        return;
    }

    char light_status[80];
    char dht22_status[96];

    if (!snapshot.has_light) {
        snprintf(light_status, sizeof(light_status), "BH1750=pending");
    } else if (snapshot.light_valid) {
        snprintf(light_status, sizeof(light_status), "BH1750=%d lx",
                 snapshot.light_lux);
    } else {
        snprintf(light_status, sizeof(light_status), "BH1750=invalid(%s)",
                 esp_err_to_name(ctx->light.error));
    }

    if (!snapshot.has_dht22) {
        snprintf(dht22_status, sizeof(dht22_status), "DHT22=pending");
    } else if (snapshot.dht22_valid) {
        snprintf(dht22_status, sizeof(dht22_status), "DHT22=%d C %d %%",
                 snapshot.temperature_c, snapshot.humidity_percent);
    } else {
        snprintf(dht22_status, sizeof(dht22_status), "DHT22=invalid(%s)",
                 esp_err_to_name(ctx->dht22.error));
    }

    ESP_LOGI(TAG, "sensors: %s, %s", light_status, dht22_status);
    ctx->last_logged_sensors = snapshot;
    ctx->has_logged_sensors = true;
}

static void publish_sensor_data_if_changed(app_context_t *ctx)
{
    if (!ctx->network_connected || !ctx->has_light || !ctx->has_dht22 ||
        !ctx->light.is_valid || !ctx->dht22.is_valid) {
        return;
    }

    const rounded_sensor_snapshot_t snapshot =
        rounded_sensor_snapshot_from_context(ctx);
    if (ctx->has_published_sensors &&
        !rounded_sensor_snapshot_changed(&snapshot, &ctx->last_published_sensors)) {
        return;
    }

    const mqtt_sensor_payload_t payload = {
        .light_lux = snapshot.light_lux,
        .temperature_c = snapshot.temperature_c,
        .humidity_percent = snapshot.humidity_percent,
    };

    esp_err_t err = mqtt_manager_publish_sensor(&payload);
    if (err == ESP_OK) {
        ctx->last_published_sensors = snapshot;
        ctx->has_published_sensors = true;
    } else if (err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "MQTT publish failed: %s", esp_err_to_name(err));
    }
}

static system_state_t system_state_from_status(const app_context_t *ctx)
{
    if (mqtt_manager_is_setup_ap_running()) {
        return SYSTEM_STATE_WARNING;
    }
    if (!ctx->network_connected || !ctx->has_light || !ctx->has_dht22 ||
        !ctx->light.is_valid || !ctx->dht22.is_valid) {
        return SYSTEM_STATE_CRITICAL;
    }
    if (!mqtt_manager_is_mqtt_connected()) {
        return SYSTEM_STATE_CRITICAL;
    }

    return SYSTEM_STATE_OK;
}

static void app_controller_task(void *arg)
{
    (void)arg;

    app_context_t ctx = {
        .has_light = false,
        .has_dht22 = false,
        .dht22_updated = false,
        .network_connected = false,
        .rgb_state = SYSTEM_STATE_CRITICAL,
        .last_sent_rgb_state = SYSTEM_STATE_COUNT,
        .has_logged_sensors = false,
        .has_published_sensors = false,
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
            } else {
                ctx.light = (bh1750_reading_t) {
                    .lux = 0.0f,
                    .is_valid = false,
                    .error = ESP_ERR_TIMEOUT,
                    .timestamp_us = 0,
                };
                ctx.has_light = true;
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

            if (has_invalid_sensor_data(&ctx)) {
                ctx.rgb_state = system_state_from_status(&ctx);
                send_rgb_state(&ctx, ctx.rgb_state);
                state = transition_to(state, ST_ERROR);
                log_sensor_summary_if_changed(&ctx);
                break;
            }

            ctx.rgb_state = system_state_from_status(&ctx);
            send_rgb_state(&ctx, ctx.rgb_state);
            log_sensor_summary_if_changed(&ctx);
            publish_sensor_data_if_changed(&ctx);
            state = transition_to(state, ST_UPDATE_OUTPUT);
            break;

        case ST_UPDATE_OUTPUT:
            if (poll_network_events(&ctx)) {
                state = transition_to(state, ST_RECOVERY);
                break;
            }

            send_rgb_state(&ctx, ctx.rgb_state);
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
    esp_log_level_set("mqtt_manager", ESP_LOG_INFO);

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
    if (mqtt_manager_init(s_network_event_queue) != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize MQTT manager");
    }

    const bool bh1750_ready = bh1750_init() == ESP_OK;
    if (!bh1750_ready) {
        ESP_LOGE(TAG, "BH1750 initialization failed");
    }

    xTaskCreate(rgb_blink_task, "rgb_blink", 2048, s_rgb_state_queue, 5, NULL);
    xTaskCreate(app_controller_task, "app_controller", 4096, NULL, 6, NULL);
    xTaskCreate(dht22_worker_task, "dht22_worker", 3072, s_dht22_queue, 4, NULL);
    xTaskCreate(settings_reset_button_task, "settings_reset", 2048, NULL, 4, NULL);

    if (bh1750_ready) {
        vTaskDelay(pdMS_TO_TICKS(BH1750_MEASUREMENT_DELAY_MS));
        xTaskCreate(bh1750_worker_task, "bh1750_worker", 3072, s_light_queue, 4, NULL);
    }
}
