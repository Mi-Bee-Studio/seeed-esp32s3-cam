#pragma once
#include "esp_err.h"

/**
 * @brief Initialize WS-Discovery UDP multicast listener.
 * Creates a FreeRTOS task listening on UDP port 3702,
 * multicast group 239.255.255.250 for ONVIF Probe messages.
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t onvif_discovery_init(void);
