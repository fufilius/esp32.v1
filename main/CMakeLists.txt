idf_component_register(SRCS "dht22.c" "main.c" "bh1750.c" "rgb_led.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES driver esp_timer)
