#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Initialize the MJPEG streamer (creates mutex, resets client count).
 * Call once before registering with a server.
 */
esp_err_t mjpeg_streamer_init(void);

/**
 * Register the /stream URI handler on the given HTTP server.
 */
esp_err_t mjpeg_streamer_register(httpd_handle_t server);

/**
 * Return the number of currently connected streaming clients.
 */
int mjpeg_streamer_client_count(void);
