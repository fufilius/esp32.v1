#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef struct {
    int light_lux;
    int temperature_c;
    int humidity_percent;
} mqtt_sensor_payload_t;

esp_err_t mqtt_manager_init(QueueHandle_t network_event_queue);
esp_err_t mqtt_manager_publish_sensor(const mqtt_sensor_payload_t *payload);
esp_err_t mqtt_manager_reset_settings(void);
bool mqtt_manager_is_setup_ap_running(void);
bool mqtt_manager_is_mqtt_connected(void);
