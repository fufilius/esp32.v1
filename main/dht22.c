#include <stdint.h>
#include <string.h>

#include "dht22.h"
#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define DHT22_GPIO GPIO_NUM_27
#define DHT22_RMT_RESOLUTION_HZ 1000000
#define DHT22_RMT_SYMBOLS 64
#define DHT22_RX_TIMEOUT_MS 30
#define DHT22_START_SIGNAL_US 18000
#define DHT22_START_RELEASE_US 40
#define DHT22_ONE_THRESHOLD_US 50

static const char *TAG = "dht22";
static rmt_channel_handle_t s_rx_channel;
static QueueHandle_t s_rx_queue;
static rmt_symbol_word_t s_raw_symbols[DHT22_RMT_SYMBOLS];

static bool dht22_rx_done_callback(rmt_channel_handle_t channel,
                                   const rmt_rx_done_event_data_t *edata,
                                   void *user_data)
{
    (void)channel;

    BaseType_t high_task_wakeup = pdFALSE;
    QueueHandle_t queue = (QueueHandle_t)user_data;
    xQueueSendFromISR(queue, edata, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

static uint32_t symbol_high_duration_us(const rmt_symbol_word_t *symbol)
{
    if (symbol->level0 == 1) {
        return symbol->duration0;
    }
    if (symbol->level1 == 1) {
        return symbol->duration1;
    }
    return 0;
}

static bool symbol_has_dht_bit_shape(const rmt_symbol_word_t *symbol)
{
    const uint32_t high_us = symbol_high_duration_us(symbol);
    return high_us >= 15 && high_us <= 100;
}

static esp_err_t parse_dht22_symbols(const rmt_symbol_word_t *symbols,
                                     size_t symbol_count,
                                     dht22_reading_t *reading)
{
    uint8_t data[5] = {0};
    size_t start = 0;

    while (start < symbol_count && !symbol_has_dht_bit_shape(&symbols[start])) {
        start++;
    }

    if (symbol_count - start < 40) {
        ESP_LOGD(TAG, "RMT received only %u usable symbols", (unsigned)(symbol_count - start));
        return ESP_ERR_TIMEOUT;
    }

    for (int bit = 0; bit < 40; ++bit) {
        const uint32_t high_us = symbol_high_duration_us(&symbols[start + bit]);
        if (high_us == 0) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (high_us > DHT22_ONE_THRESHOLD_US) {
            data[bit / 8] |= (uint8_t)(1 << (7 - (bit % 8)));
        }
    }

    const uint8_t checksum = data[0] + data[1] + data[2] + data[3];
    if (checksum != data[4]) {
        ESP_LOGD(TAG, "checksum mismatch: got 0x%02x expected 0x%02x",
                 data[4], checksum);
        return ESP_ERR_INVALID_CRC;
    }

    const uint16_t humidity_raw = ((uint16_t)data[0] << 8) | data[1];
    const uint16_t temperature_raw = (((uint16_t)data[2] & 0x7f) << 8) | data[3];
    const float temperature_sign = (data[2] & 0x80) ? -1.0f : 1.0f;

    reading->humidity_percent = humidity_raw / 10.0f;
    reading->temperature_c = temperature_sign * (temperature_raw / 10.0f);
    reading->is_valid = true;
    reading->error = ESP_OK;
    reading->timestamp_us = esp_timer_get_time();
    return ESP_OK;
}

void dht22_init(void)
{
    gpio_reset_pin(DHT22_GPIO);
    gpio_set_direction(DHT22_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(DHT22_GPIO, GPIO_PULLUP_ONLY);

    s_rx_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    if (s_rx_queue == NULL) {
        ESP_LOGE(TAG, "failed to create RMT RX queue");
        return;
    }

    rmt_rx_channel_config_t rx_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = DHT22_RMT_RESOLUTION_HZ,
        .mem_block_symbols = DHT22_RMT_SYMBOLS,
        .gpio_num = DHT22_GPIO,
    };
    esp_err_t err = rmt_new_rx_channel(&rx_config, &s_rx_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to create RMT RX channel: %s", esp_err_to_name(err));
        return;
    }

    rmt_rx_event_callbacks_t callbacks = {
        .on_recv_done = dht22_rx_done_callback,
    };
    err = rmt_rx_register_event_callbacks(s_rx_channel, &callbacks, s_rx_queue);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to register RMT RX callback: %s", esp_err_to_name(err));
        return;
    }

    err = rmt_enable(s_rx_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to enable RMT RX channel: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "ready on GPIO%d using RMT", DHT22_GPIO);
}

esp_err_t dht22_read(dht22_reading_t *reading)
{
    if (reading == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_rx_channel == NULL || s_rx_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(reading, 0, sizeof(*reading));
    reading->error = ESP_OK;
    reading->timestamp_us = esp_timer_get_time();

    rmt_rx_done_event_data_t rx_data;
    while (xQueueReceive(s_rx_queue, &rx_data, 0) == pdTRUE) {
    }
    memset(s_raw_symbols, 0, sizeof(s_raw_symbols));

    rmt_receive_config_t receive_config = {
        .signal_range_min_ns = 1000,
        .signal_range_max_ns = 1000000,
    };

    gpio_set_direction(DHT22_GPIO, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(DHT22_GPIO, 0);
    esp_rom_delay_us(DHT22_START_SIGNAL_US);
    gpio_set_level(DHT22_GPIO, 1);
    gpio_set_direction(DHT22_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(DHT22_GPIO, GPIO_PULLUP_ONLY);

    esp_err_t err = rmt_receive(s_rx_channel, s_raw_symbols, sizeof(s_raw_symbols),
                                &receive_config);
    if (err != ESP_OK) {
        return err;
    }

    esp_rom_delay_us(DHT22_START_RELEASE_US);

    if (xQueueReceive(s_rx_queue, &rx_data, pdMS_TO_TICKS(DHT22_RX_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    err = parse_dht22_symbols(rx_data.received_symbols, rx_data.num_symbols, reading);
    reading->error = err;
    reading->is_valid = err == ESP_OK;
    return err;
}
