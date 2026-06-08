#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

esp_err_t wifi_manager_init(QueueHandle_t network_event_queue);
esp_err_t wifi_manager_reset_credentials(void);
