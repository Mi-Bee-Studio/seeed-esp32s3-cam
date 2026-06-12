#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

/**
 * @brief Register ONVIF SOAP service handlers on the HTTP server.
 * Registers POST handlers for /onvif/device_service and /onvif/media_service.
 * @param server HTTP server handle
 * @return ESP_OK on success
 */
esp_err_t onvif_register_handlers(httpd_handle_t server);
