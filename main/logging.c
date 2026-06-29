/*
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "logging.h"
#include "time_sync.h"
#include "esp_log.h"
#include "sd_log.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

static const char *TAG = "logging";

/* Maximum JSON line length (conservative for embedded) */
#define LOG_EVENT_BUF_SIZE  256

void log_event_impl(const char *type, const char *msg, ...)
{
    if (!type || !msg) {
        return;
    }

    /* Get ISO-8601 timestamp */
    char ts[32] = "0000-00-00T00:00:00";
    time_get_str(ts, sizeof(ts));
    /* Replace space with 'T' for ISO 8601 format: YYYY-MM-DDTHH:MM:SS */
    for (char *p = ts; *p; p++) {
        if (*p == ' ') {
            *p = 'T';
            break;
        }
    }

    /* Format the user message */
    char formatted[LOG_EVENT_BUF_SIZE - 96];  /* reserve room for JSON envelope */
    va_list args;
    va_start(args, msg);
    int n = vsnprintf(formatted, sizeof(formatted), msg, args);
    va_end(args);
    if (n < 0) {
        /* vsnprintf failure — output a placeholder */
        strncpy(formatted, "format-error", sizeof(formatted) - 1);
        formatted[sizeof(formatted) - 1] = '\0';
    } else if ((size_t)n >= sizeof(formatted)) {
        /* Truncation — append ellipsis */
        const char *suffix = "...";
        size_t slen = strlen(suffix);
        if (sizeof(formatted) > slen + 1) {
            memcpy(formatted + sizeof(formatted) - slen - 1, suffix, slen);
            formatted[sizeof(formatted) - slen - 1] = '\0';
        }
    }

    /* Build the JSON line using manual formatting (no cJSON needed) */
    char line[LOG_EVENT_BUF_SIZE];
    int written = snprintf(line, sizeof(line),
                           "{\"type\":\"%s\",\"ts\":\"%s\",\"msg\":\"%s\"}",
                           type, ts, formatted);

    if (written > 0 && (size_t)written < sizeof(line)) {
        printf("%s\n", line);
        fflush(stdout);
        sd_log_write(line);
        /* SD card log backend (non-blocking, silently drops on failure) */
    } else {
        ESP_LOGW(TAG, "JSON log line too long (type=%s)", type);
    }
}
