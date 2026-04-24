#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <time.h>

/**
 * Start SNTP time synchronization (call after WiFi connected).
 * Blocks up to 5 seconds waiting for initial sync, then returns.
 * Async sync continues in background if not yet synced.
 */
esp_err_t time_sync_init(void);

/** True if NTP or manual time has been set successfully. */
bool time_is_synced(void);

/** Format current time as "YYYY-MM-DD HH:MM:SS" into buf. */
void time_get_str(char *buf, size_t len);

/** Manually set system time (useful when no NTP available). */
esp_err_t time_set_manual(int year, int month, int day, int hour, int min, int sec);
