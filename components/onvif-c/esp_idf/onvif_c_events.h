/*
 * onvif-c ESP-IDF port — Pull-Point events service (internal).
 */

#ifndef ONVIF_C_EVENTS_H
#define ONVIF_C_EVENTS_H

#include "esp_http_server.h"
#include "esp_err.h"

/** Register /onvif/events_service (called by onvif_c_start when the
 *  events_enabled callback is provided). */
esp_err_t onvif_c_events_register(httpd_handle_t server);

#endif /* ONVIF_C_EVENTS_H */
