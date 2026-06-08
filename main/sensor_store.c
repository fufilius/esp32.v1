#include "sensor_store.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static portMUX_TYPE s_sensor_store_lock = portMUX_INITIALIZER_UNLOCKED;
static sensor_snapshot_t s_snapshot;

void sensor_store_update_light(float lux, bool is_valid, esp_err_t error,
                               int64_t timestamp_us)
{
    portENTER_CRITICAL(&s_sensor_store_lock);
    s_snapshot.light_valid = is_valid;
    s_snapshot.light_lux = lux;
    s_snapshot.light_error = error;
    s_snapshot.light_timestamp_us = timestamp_us;
    portEXIT_CRITICAL(&s_sensor_store_lock);
}

void sensor_store_update_dht22(float temperature_c, float humidity_percent,
                               bool is_valid, esp_err_t error,
                               int64_t timestamp_us)
{
    portENTER_CRITICAL(&s_sensor_store_lock);
    s_snapshot.dht22_valid = is_valid;
    s_snapshot.temperature_c = temperature_c;
    s_snapshot.humidity_percent = humidity_percent;
    s_snapshot.dht22_error = error;
    s_snapshot.dht22_timestamp_us = timestamp_us;
    portEXIT_CRITICAL(&s_sensor_store_lock);
}

sensor_snapshot_t sensor_store_get_snapshot(void)
{
    sensor_snapshot_t snapshot;

    portENTER_CRITICAL(&s_sensor_store_lock);
    snapshot = s_snapshot;
    portEXIT_CRITICAL(&s_sensor_store_lock);

    return snapshot;
}
