#include "network_events.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"

static const char *TAG = "network_events";
static QueueHandle_t s_event_queue;

static const char *network_event_type_name(network_event_type_t type)
{
    switch (type) {
    case NETWORK_EVENT_CONNECTING:
        return "CONNECTING";
    case NETWORK_EVENT_CONNECTED:
        return "CONNECTED";
    case NETWORK_EVENT_LOST:
        return "LOST";
    default:
        return "UNKNOWN";
    }
}

static network_event_type_t network_event_type_from_ip_event(int32_t event_id)
{
    switch (event_id) {
    case IP_EVENT_STA_GOT_IP:
    case IP_EVENT_ETH_GOT_IP:
    case IP_EVENT_GOT_IP6:
        return NETWORK_EVENT_CONNECTED;
    case IP_EVENT_STA_LOST_IP:
    case IP_EVENT_ETH_LOST_IP:
        return NETWORK_EVENT_LOST;
    default:
        return NETWORK_EVENT_CONNECTING;
    }
}

static void queue_network_event(network_event_type_t type, int32_t event_id)
{
    if (s_event_queue == NULL) {
        return;
    }

    network_event_t event = {
        .type = type,
        .event_id = event_id,
        .timestamp_us = esp_timer_get_time(),
    };

    if (xQueueSend(s_event_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "network event queue is full; dropped %s",
                 network_event_type_name(type));
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base != IP_EVENT) {
        return;
    }

    const network_event_type_t type = network_event_type_from_ip_event(event_id);
    ESP_LOGD(TAG, "IP event %ld -> %s", event_id, network_event_type_name(type));
    queue_network_event(type, event_id);
}

esp_err_t network_events_init(QueueHandle_t event_queue)
{
    if (event_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_event_queue = event_queue;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                     ip_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IP event handler register failed: %s", esp_err_to_name(err));
        return err;
    }

    queue_network_event(NETWORK_EVENT_CONNECTING, -1);
    ESP_LOGD(TAG, "IP event handler registered");

    return ESP_OK;
}
