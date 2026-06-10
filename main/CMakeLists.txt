idf_component_register(SRCS "dht22.c" "main.c" "bh1750.c" "rgb_led.c" "network_events.c" "mqtt_manager.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES driver esp_timer esp_event esp_netif esp_wifi esp_http_server nvs_flash mqtt)
