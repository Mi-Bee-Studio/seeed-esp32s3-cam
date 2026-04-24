#pragma once

#include "esp_err.h"

typedef enum {
    LED_STARTING,         // Solid on
    LED_AP_MODE,          // Slow blink 1s
    LED_WIFI_CONNECTING,  // Fast blink 200ms
    LED_RUNNING,          // Off
    LED_ERROR             // Double blink
} led_status_t;

esp_err_t led_init(void);
void led_set_status(led_status_t status);
