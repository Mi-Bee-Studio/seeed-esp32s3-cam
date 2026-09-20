/*
 * onvif-c ESP-IDF port — internal shared state (not part of public API).
 */

#ifndef ONVIF_C_PORT_H
#define ONVIF_C_PORT_H

#include "../include/onvif_c.h"

/** Effective config (defaults resolved); NULL before onvif_c_start(). */
const onvif_c_config_t *onvif_c_cfg(void);

/** Store + default-resolve a copy of the integrator config. */
void onvif_c_cfg_set(const onvif_c_config_t *cfg);

/** Current IP or the placeholder "0.0.0.0" (never NULL after start). */
const char *onvif_c_cfg_ip(void);

/** Default WS-Discovery scopes string (uses cfg->model). */
const char *onvif_c_cfg_scopes(void);

/** Events feature present (cfg->events_enabled != NULL)? */
bool onvif_c_cfg_has_events(void);

/* Lifecycle halves implemented in onvif_c_discovery.c (called by
 * onvif_c_start / onvif_c_stop in onvif_c_service.c). */
esp_err_t onvif_c_discovery_start(void);
esp_err_t onvif_c_discovery_stop(void);

#endif /* ONVIF_C_PORT_H */
