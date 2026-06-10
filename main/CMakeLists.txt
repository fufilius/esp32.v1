idf_component_register(SRCS "main.c" "dht22.c" "bh1750.c" "rgb_led.c" "network_events.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES driver esp_timer esp_event esp_netif)
