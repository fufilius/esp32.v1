idf_component_register(SRCS "main.c" "dht22.c" "bh1750.c" "rgb_led.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES driver esp_timer)
