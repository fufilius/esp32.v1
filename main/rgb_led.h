#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

void rgb_led_init(void);
void rgb_blink_task(void *arg);
