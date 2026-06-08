idf_component_register(SRCS "main.c" "dht22.c" "bh1750.c" "rgb_led.c" "network_events.c" "wifi_manager.c" "sensor_store.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES driver esp_timer esp_event esp_netif esp_wifi esp_http_server nvs_flash)
