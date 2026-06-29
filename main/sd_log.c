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

#include "sd_log.h"
#include "config_manager.h"
#include "storage_manager.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "sd_log";

/* -------------------------------------------------------------------
 * Configuration constants
 * ------------------------------------------------------------------- */
#define LOGS_PATH              "/sdcard/logs"
#define SD_LOG_MAX_FILE_SIZE   (512 * 1024)  /* 512 KB per file */
#define SD_LOG_RETENTION_DAYS  7             /* keep last 7 days */
#define SD_LOG_FLUSH_INTERVAL  10            /* flush every ~10 lines */
#define SD_LOG_CLEANUP_EVERY   100           /* cleanup every ~100 writes */

/* -------------------------------------------------------------------
 * Module state
 * ------------------------------------------------------------------- */
static SemaphoreHandle_t s_sd_log_mutex = NULL;
static FILE             *s_current_file      = NULL;
static char              s_current_filename[64];
static uint32_t          s_lines_since_flush = 0;
static bool              s_paused            = false;
static bool              s_initialized       = false;
static uint32_t          s_write_count       = 0;

/* -------------------------------------------------------------------
 * Forward declarations of internal helpers
 * ------------------------------------------------------------------- */
static void sd_log_rotate(void);
static void sd_log_cleanup_old_files(void);

/* -------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------- */

esp_err_t sd_log_init(void)
{
    s_sd_log_mutex = xSemaphoreCreateMutex();
    if (!s_sd_log_mutex) {
        return ESP_ERR_NO_MEM;
    }

    /* Create the logs directory — harmless if it already exists */
    mkdir(LOGS_PATH, 0775);

    s_initialized       = true;
    s_current_file      = NULL;
    s_current_filename[0] = '\0';
    s_lines_since_flush = 0;
    s_paused            = false;
    s_write_count       = 0;

    ESP_LOGI(TAG, "SD log initialized (path=%s, max_size=%u, retention=%dd)",
             LOGS_PATH, SD_LOG_MAX_FILE_SIZE, SD_LOG_RETENTION_DAYS);
    return ESP_OK;
}

/* ------------------------------------------------------------------- */

void sd_log_deinit(void)
{
    if (!s_initialized) {
        return;
    }

    if (xSemaphoreTake(s_sd_log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_current_file) {
            fflush(s_current_file);
            fclose(s_current_file);
            s_current_file = NULL;
        }
        s_initialized = false;
        xSemaphoreGive(s_sd_log_mutex);
    }

    if (s_sd_log_mutex) {
        vSemaphoreDelete(s_sd_log_mutex);
        s_sd_log_mutex = NULL;
    }
}

/* ------------------------------------------------------------------- */

void sd_log_write(const char *json_line)
{
    /* ── quick guards (no lock needed) ── */
    if (!s_initialized || s_paused || !json_line) {
        return;
    }

    /* check config: only write when sd_log_enabled is true */
    cam_config_t *cfg = config_get();
    if (!cfg || !cfg->sd_log_enabled) {
        return;
    }

    /* SD card must be mounted */
    if (!storage_is_available()) {
        return;
    }

    /* ── try to acquire mutex; never block the caller ── */
    if (xSemaphoreTake(s_sd_log_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;     /* mutex busy → silent drop */
    }

    /* ── open / rotate file if needed ── */
    if (s_current_file == NULL) {
        /* get today's date string */
        time_t now = time(NULL);
        struct tm tm_info;
        localtime_r(&now, &tm_info);
        char date_str[16];
        strftime(date_str, sizeof(date_str), "%Y-%m-%d", &tm_info);

        /* build the base filename: /sdcard/logs/YYYY-MM-DD.log */
        snprintf(s_current_filename, sizeof(s_current_filename),
                 "%s/%s.log", LOGS_PATH, date_str);

        /* if base file exists and is already full → find a sequence number */
        struct stat st;
        if (stat(s_current_filename, &st) == 0 &&
            st.st_size >= SD_LOG_MAX_FILE_SIZE) {
            int seq = 1;
            while (seq < 1000) {
                snprintf(s_current_filename, sizeof(s_current_filename),
                         "%s/%s_%03d.log", LOGS_PATH, date_str, seq);
                if (stat(s_current_filename, &st) != 0) {
                    break;                          /* file does not exist → use it */
                }
                if (st.st_size < SD_LOG_MAX_FILE_SIZE) {
                    break;                          /* exists but under limit → reuse */
                }
                seq++;
            }
        }

        /* open in append mode */
        s_current_file = fopen(s_current_filename, "a");
        if (!s_current_file) {
            s_current_filename[0] = '\0';
            xSemaphoreGive(s_sd_log_mutex);
            return;
        }
    }

    /* ── check if the currently open file needs rotation ── */
    struct stat st;
    if (stat(s_current_filename, &st) == 0 &&
        st.st_size >= SD_LOG_MAX_FILE_SIZE) {
        sd_log_rotate();
        if (!s_current_file) {
            /* rotation failed — nothing more we can do */
            xSemaphoreGive(s_sd_log_mutex);
            return;
        }
    }

    /* ── write the line ── */
    fprintf(s_current_file, "%s\n", json_line);
    s_lines_since_flush++;

    /* Always flush to prevent 0KB files if system resets before batch flush */
    fflush(s_current_file);
    s_lines_since_flush = 0;

    /* periodic cleanup of old log files (every ~100 writes) */
    s_write_count++;
    if (s_write_count % SD_LOG_CLEANUP_EVERY == 0) {
        sd_log_cleanup_old_files();
    }

    xSemaphoreGive(s_sd_log_mutex);
}

/* ------------------------------------------------------------------- */

void sd_log_pause(void)
{
    if (!s_initialized) {
        return;
    }

    if (xSemaphoreTake(s_sd_log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_current_file) {
            fflush(s_current_file);
            fclose(s_current_file);
            s_current_file = NULL;
        }
        s_paused              = true;
        s_current_filename[0] = '\0';
        s_lines_since_flush   = 0;
        xSemaphoreGive(s_sd_log_mutex);
    }
}

/* ------------------------------------------------------------------- */

void sd_log_resume(void)
{
    if (!s_initialized) {
        return;
    }

    if (xSemaphoreTake(s_sd_log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_paused              = false;
        s_current_file        = NULL;   /* will re-open on next write */
        s_current_filename[0] = '\0';
        s_lines_since_flush   = 0;
        xSemaphoreGive(s_sd_log_mutex);
    }
}

/* ===================================================================
 *  Internal helpers
 * =================================================================== */

/**
 * @brief Rotate to the next sequenced log file.
 *
 * Called while holding s_sd_log_mutex.  Closes the current file, builds
 * the next _NNN filename, and opens it.  On failure s_current_file is
 * set to NULL.
 */
static void sd_log_rotate(void)
{
    if (!s_current_file) {
        return;
    }

    fflush(s_current_file);
    fclose(s_current_file);
    s_current_file = NULL;

    /* Parse the date and current sequence from the filename.
     * Filename is either:
     *   /sdcard/logs/YYYY-MM-DD.log          → base name, first rotation
     *   /sdcard/logs/YYYY-MM-DD_001.log      → already rotated
     */
    const char *base = s_current_filename + strlen(LOGS_PATH) + 1;
    size_t base_len  = strlen(base);

    if (base_len < 10) {
        s_current_filename[0] = '\0';
        return;
    }

    /* extract YYYY-MM-DD (first 10 chars) */
    char date_str[16];
    memcpy(date_str, base, 10);
    date_str[10] = '\0';

    /* determine next sequence number */
    int seq = 1;
    if (base_len > 10 && base[10] == '_') {
        seq = atoi(base + 11) + 1;
    }
    if (seq > 999) {
        seq = 1;   /* wrap around (extremely unlikely) */
    }

    snprintf(s_current_filename, sizeof(s_current_filename),
             "%s/%s_%03d.log", LOGS_PATH, date_str, seq);

    s_current_file = fopen(s_current_filename, "a");
    if (!s_current_file) {
        s_current_filename[0] = '\0';
    }
}

/**
 * @brief Remove log files older than SD_LOG_RETENTION_DAYS.
 *
 * Called periodically from sd_log_write() while holding the mutex.
 * Scans all .log files in LOGS_PATH, parses the YYYY-MM-DD prefix,
 * and deletes those whose date is before the cutoff.
 */
static void sd_log_cleanup_old_files(void)
{
    /* calculate cutoff date */
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    tm_info.tm_mday -= SD_LOG_RETENTION_DAYS;
    mktime(&tm_info);   /* normalise */

    char cutoff_str[16];
    strftime(cutoff_str, sizeof(cutoff_str), "%Y-%m-%d", &tm_info);

    DIR *dir = opendir(LOGS_PATH);
    if (!dir) {
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        /* only touch .log files */
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext || strcmp(ext, ".log") != 0) {
            continue;
        }

        /* need at least "YYYY-MM-DD" prefix */
        if (strlen(entry->d_name) < 10) {
            continue;
        }

        /* extract date prefix */
        char file_date[16];
        memcpy(file_date, entry->d_name, 10);
        file_date[10] = '\0';

        /* delete if older than retention */
        if (strcmp(file_date, cutoff_str) < 0) {
            char full_path[384];
            int plen = snprintf(full_path, sizeof(full_path), "%s/%s",
                                LOGS_PATH, entry->d_name);
            if (plen < 0 || (size_t)plen >= sizeof(full_path)) {
                continue;
            }
            remove(full_path);
    }
    }

    closedir(dir);
}
