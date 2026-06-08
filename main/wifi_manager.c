#include "wifi_manager.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "network_events.h"
#include "nvs.h"
#include "sensor_store.h"

#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define WIFI_NVS_SSID_KEY "ssid"
#define WIFI_NVS_PASS_KEY "pass"

#define SETUP_AP_SSID "ESP32-Setup"
#define SETUP_AP_PASSWORD "configure123"
#define SETUP_AP_CHANNEL 1
#define SETUP_AP_MAX_CONN 4

#define CONNECT_MAX_RETRIES 8
#define RECOVERY_RED_DELAY_MS 2500
#define WIFI_SCAN_MAX_APS 16
#define WIFI_FORM_MAX_LEN 192

typedef struct {
    char ssid[33];
    char password[65];
    bool has_credentials;
} wifi_credentials_t;

static const char *TAG = "wifi_manager";

static QueueHandle_t s_network_event_queue;
static httpd_handle_t s_http_server;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static int s_retry_count;
static bool s_setup_ap_running;
static bool s_sta_connected;
static bool s_manual_setup_requested;

static void queue_network_event(network_event_type_t type, int32_t event_id)
{
    if (s_network_event_queue == NULL) {
        return;
    }

    network_event_t event = {
        .type = type,
        .event_id = event_id,
        .timestamp_us = esp_timer_get_time(),
    };

    if (xQueueSend(s_network_event_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "network event queue is full; dropped event %d", type);
    }
}

static esp_err_t load_credentials(wifi_credentials_t *credentials)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        credentials->has_credentials = false;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t ssid_len = sizeof(credentials->ssid);
    size_t pass_len = sizeof(credentials->password);
    err = nvs_get_str(nvs, WIFI_NVS_SSID_KEY, credentials->ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, WIFI_NVS_PASS_KEY, credentials->password, &pass_len);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            credentials->password[0] = '\0';
            err = ESP_OK;
        }
    }
    nvs_close(nvs);

    credentials->has_credentials = err == ESP_OK && credentials->ssid[0] != '\0';
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

static esp_err_t save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, WIFI_NVS_SSID_KEY, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, WIFI_NVS_PASS_KEY, password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
}

static esp_err_t clear_credentials(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    esp_err_t erase_ssid_err = nvs_erase_key(nvs, WIFI_NVS_SSID_KEY);
    esp_err_t erase_pass_err = nvs_erase_key(nvs, WIFI_NVS_PASS_KEY);
    err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err != ESP_OK) {
        return err;
    }
    if (erase_ssid_err != ESP_OK && erase_ssid_err != ESP_ERR_NVS_NOT_FOUND) {
        return erase_ssid_err;
    }
    if (erase_pass_err != ESP_OK && erase_pass_err != ESP_ERR_NVS_NOT_FOUND) {
        return erase_pass_err;
    }

    return ESP_OK;
}

static int hex_to_int(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void url_decode(char *value)
{
    char *src = value;
    char *dst = value;

    while (*src != '\0') {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && isxdigit((unsigned char)src[1]) &&
                   isxdigit((unsigned char)src[2])) {
            const int high = hex_to_int(src[1]);
            const int low = hex_to_int(src[2]);
            *dst++ = (char)((high << 4) | low);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }

    *dst = '\0';
}

static bool form_get_value(const char *body, const char *key, char *out, size_t out_size)
{
    const size_t key_len = strlen(key);
    const char *cursor = body;

    while (cursor != NULL && *cursor != '\0') {
        if (strncmp(cursor, key, key_len) == 0 && cursor[key_len] == '=') {
            const char *value_start = cursor + key_len + 1;
            const char *value_end = strchr(value_start, '&');
            const size_t value_len = value_end == NULL ? strlen(value_start)
                                                       : (size_t)(value_end - value_start);
            const size_t copy_len = value_len < out_size - 1 ? value_len : out_size - 1;
            memcpy(out, value_start, copy_len);
            out[copy_len] = '\0';
            url_decode(out);
            return true;
        }

        cursor = strchr(cursor, '&');
        if (cursor != NULL) {
            cursor++;
        }
    }

    return false;
}

static esp_err_t start_sta_connect(const char *ssid, const char *password)
{
    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_config), TAG,
                        "failed to set STA config");

    s_manual_setup_requested = false;
    s_retry_count = 0;
    queue_network_event(NETWORK_EVENT_CONNECTING, WIFI_EVENT_STA_START);
    return esp_wifi_connect();
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    if (!s_setup_ap_running) {
        const sensor_snapshot_t snapshot = sensor_store_get_snapshot();
        char *page = malloc(1400);
        if (page == NULL) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "Not enough memory");
            return ESP_OK;
        }

        snprintf(page, 1400,
                 "<!doctype html><html><head><meta charset=\"utf-8\">"
                 "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                 "<meta http-equiv=\"refresh\" content=\"5\">"
                 "<title>ESP32 Sensors</title>"
                 "<style>body{font-family:sans-serif;max-width:560px;margin:24px auto;padding:0 16px}"
                 "section{border:1px solid #ddd;border-radius:6px;padding:14px;margin:12px 0}"
                 "dt{font-weight:700}dd{margin:0 0 10px}</style></head><body>"
                 "<h1>ESP32 Sensors</h1>"
                 "<section><h2>BH1750</h2><dl>"
                 "<dt>Status</dt><dd>%s</dd>"
                 "<dt>Light</dt><dd>%.2f lx</dd>"
                 "<dt>Error</dt><dd>%s</dd></dl></section>"
                 "<section><h2>DHT22</h2><dl>"
                 "<dt>Status</dt><dd>%s</dd>"
                 "<dt>Temperature</dt><dd>%.1f C</dd>"
                 "<dt>Humidity</dt><dd>%.1f %%</dd>"
                 "<dt>Error</dt><dd>%s</dd></dl></section>"
                 "<p><a href=\"/api/sensors\">JSON</a></p></body></html>",
                 snapshot.light_valid ? "OK" : "ERROR",
                 snapshot.light_lux,
                 esp_err_to_name(snapshot.light_error),
                 snapshot.dht22_valid ? "OK" : "ERROR",
                 snapshot.temperature_c,
                 snapshot.humidity_percent,
                 esp_err_to_name(snapshot.dht22_error));

        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req, page);
        free(page);
        return ESP_OK;
    }

    wifi_ap_record_t *aps = calloc(WIFI_SCAN_MAX_APS, sizeof(wifi_ap_record_t));
    if (aps == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Not enough memory");
        return ESP_OK;
    }

    uint16_t ap_count = WIFI_SCAN_MAX_APS;
    wifi_scan_config_t scan_config = {
        .show_hidden = false,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_records(&ap_count, aps);
    }
    if (err != ESP_OK) {
        ap_count = 0;
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req,
                             "<!doctype html><html><head><meta charset=\"utf-8\">"
                             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                             "<title>ESP32 Wi-Fi setup</title>"
                             "<style>body{font-family:sans-serif;max-width:520px;margin:24px auto;padding:0 16px}"
                             "label,input,select,button{display:block;width:100%;box-sizing:border-box;margin:10px 0}"
                             "input,select,button{font-size:16px;padding:10px}</style></head><body>"
                             "<h1>ESP32 Wi-Fi setup</h1><form method=\"post\" action=\"/connect\">"
                             "<label>Network</label><select name=\"ssid\">");

    for (uint16_t i = 0; i < ap_count; ++i) {
        char option[128];
        snprintf(option, sizeof(option), "<option value=\"%s\">%s (%d dBm)</option>",
                 (const char *)aps[i].ssid, (const char *)aps[i].ssid, aps[i].rssi);
        httpd_resp_sendstr_chunk(req, option);
    }

    httpd_resp_sendstr_chunk(req,
                             "</select><label>Password</label>"
                             "<input name=\"password\" type=\"password\" autocomplete=\"current-password\">"
                             "<button type=\"submit\">Connect</button></form></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    free(aps);
    return ESP_OK;
}

static esp_err_t sensors_get_handler(httpd_req_t *req)
{
    const sensor_snapshot_t snapshot = sensor_store_get_snapshot();
    char response[512];

    snprintf(response, sizeof(response),
             "{"
             "\"bh1750\":{\"valid\":%s,\"lux\":%.2f,\"error\":\"%s\","
             "\"timestamp_us\":%lld},"
             "\"dht22\":{\"valid\":%s,\"temperature_c\":%.1f,"
             "\"humidity_percent\":%.1f,\"error\":\"%s\",\"timestamp_us\":%lld}"
             "}",
             snapshot.light_valid ? "true" : "false",
             snapshot.light_lux,
             esp_err_to_name(snapshot.light_error),
             (long long)snapshot.light_timestamp_us,
             snapshot.dht22_valid ? "true" : "false",
             snapshot.temperature_c,
             snapshot.humidity_percent,
             esp_err_to_name(snapshot.dht22_error),
             (long long)snapshot.dht22_timestamp_us);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

static esp_err_t connect_post_handler(httpd_req_t *req)
{
    char body[WIFI_FORM_MAX_LEN] = {0};
    char ssid[33] = {0};
    char password[65] = {0};
    int received = 0;

    while (received < req->content_len && received < (int)sizeof(body) - 1) {
        const int ret = httpd_req_recv(req, body + received,
                                      sizeof(body) - 1 - received);
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    if (!form_get_value(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID is required");
        return ESP_OK;
    }
    form_get_value(body, "password", password, sizeof(password));

    ESP_LOGI(TAG, "saving Wi-Fi credentials for SSID '%s'", ssid);
    esp_err_t err = save_credentials(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to save credentials: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Failed to save credentials");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
                       "<!doctype html><html><body><h1>Saved</h1>"
                       "<p>ESP32 is connecting. You can close this page.</p></body></html>");

    start_sta_connect(ssid, password);
    return ESP_OK;
}

static esp_err_t start_http_server(void)
{
    if (s_http_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.stack_size = 8192;

    esp_err_t err = httpd_start(&s_http_server, &config);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    const httpd_uri_t connect_uri = {
        .uri = "/connect",
        .method = HTTP_POST,
        .handler = connect_post_handler,
    };
    const httpd_uri_t sensors_uri = {
        .uri = "/api/sensors",
        .method = HTTP_GET,
        .handler = sensors_get_handler,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &root_uri), TAG,
                        "failed to register root handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &connect_uri), TAG,
                        "failed to register connect handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &sensors_uri), TAG,
                        "failed to register sensors handler");

    return ESP_OK;
}

static esp_err_t start_setup_ap(void)
{
    wifi_config_t ap_config = {
        .ap = {
            .ssid = SETUP_AP_SSID,
            .ssid_len = strlen(SETUP_AP_SSID),
            .channel = SETUP_AP_CHANNEL,
            .password = SETUP_AP_PASSWORD,
            .max_connection = SETUP_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };

    if (strlen(SETUP_AP_PASSWORD) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG,
                        "failed to set APSTA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG,
                        "failed to set AP config");
    ESP_RETURN_ON_ERROR(start_http_server(), TAG, "failed to start setup web server");

    s_setup_ap_running = true;
    queue_network_event(NETWORK_EVENT_CONNECTING, WIFI_EVENT_AP_START);
    ESP_LOGI(TAG, "setup AP started: SSID '%s', password '%s'", SETUP_AP_SSID,
             SETUP_AP_PASSWORD);
    return ESP_OK;
}

static void delayed_setup_ap_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(RECOVERY_RED_DELAY_MS));
    if (start_setup_ap() != ESP_OK) {
        ESP_LOGE(TAG, "failed to start setup AP after Wi-Fi failure");
    }
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        wifi_credentials_t credentials = {0};
        if (load_credentials(&credentials) == ESP_OK && credentials.has_credentials) {
            start_sta_connect(credentials.ssid, credentials.password);
        } else {
            start_setup_ap();
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_manual_setup_requested) {
            s_sta_connected = false;
            s_retry_count = 0;
            return;
        }

        if (s_sta_connected) {
            s_sta_connected = false;
        }

        if (s_retry_count < CONNECT_MAX_RETRIES) {
            s_retry_count++;
            ESP_LOGW(TAG, "Wi-Fi disconnected; retry %d/%d", s_retry_count,
                     CONNECT_MAX_RETRIES);
            queue_network_event(NETWORK_EVENT_LOST, event_id);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "saved Wi-Fi is unavailable; starting setup AP");
            queue_network_event(NETWORK_EVENT_LOST, event_id);
            xTaskCreate(delayed_setup_ap_task, "wifi_setup_ap_delay", 3072, NULL, 4, NULL);
        }
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "connected, IP address: " IPSTR, IP2STR(&event->ip_info.ip));

        s_sta_connected = true;
        s_retry_count = 0;
        if (s_setup_ap_running) {
            esp_wifi_set_mode(WIFI_MODE_STA);
            s_setup_ap_running = false;
        }
        if (start_http_server() != ESP_OK) {
            ESP_LOGE(TAG, "failed to start sensor web server");
        }
        queue_network_event(NETWORK_EVENT_CONNECTED, event_id);
    }
}

esp_err_t wifi_manager_init(QueueHandle_t network_event_queue)
{
    if (network_event_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_network_event_queue = network_event_queue;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "failed to init Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   wifi_event_handler, NULL),
                        TAG, "failed to register Wi-Fi event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   ip_event_handler, NULL),
                        TAG, "failed to register IP event handler");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG,
                        "failed to set Wi-Fi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
                        "failed to set STA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "failed to start Wi-Fi");

    return ESP_OK;
}

esp_err_t wifi_manager_reset_credentials(void)
{
    ESP_LOGW(TAG, "manual Wi-Fi reset requested");

    esp_err_t err = clear_credentials();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to clear Wi-Fi credentials: %s", esp_err_to_name(err));
        return err;
    }

    s_manual_setup_requested = true;
    s_sta_connected = false;
    s_retry_count = 0;
    esp_wifi_disconnect();

    err = start_setup_ap();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start setup AP: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}
