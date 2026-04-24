/*
 * Copyright (C) 2024 ParrotCam Authors
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

#include "storage_manager.h"

#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "storage";

// SD card pin config for XIAO ESP32-S3 Sense
#define SD_PIN_CLK   7
#define SD_PIN_CMD   10
#define SD_PIN_D0    8

#define RECORDINGS_PATH  "/sdcard/recordings"
#define RECORDINGS_SUFFIX ".avi"
#define CLEANUP_LOW_PCT  20.0f
#define CLEANUP_HIGH_PCT 30.0f

static bool s_sd_available = false;
static SemaphoreHandle_t s_sd_mutex = NULL;
static sdmmc_card_t *s_card = NULL;
/* ---- internal helpers ---- */

static int compare_file_info(const void *a, const void *b)
{
    // Sort by name ascending (oldest timestamp first)
    return strcmp(((const file_info_t *)a)->name, ((const file_info_t *)b)->name);
}

/* ---- public API ---- */

esp_err_t storage_init(void)
{
    esp_err_t ret;

    /* Create SD mutex for concurrent access protection */
    s_sd_mutex = xSemaphoreCreateMutex();
    if (!s_sd_mutex) {
        ESP_LOGE(TAG, "Failed to create SD mutex");
        return ESP_ERR_NO_MEM;
    }

    // SDMMC host defaults: 1-line mode
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;   // 1-line mode to share GPIO10 with camera
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = SD_PIN_CLK;
    slot_config.cmd = SD_PIN_CMD;
    slot_config.width = 1;   // 1-line mode

    // Enable internal pullups (external pullups recommended for production)
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    // VFS fat mount config
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 64 * 1024,
    };

    ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &s_card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount SD card (format or filesystem issue)");
        } else {
            ESP_LOGE(TAG, "SD card init failed: %s", esp_err_to_name(ret));
        }
        s_sd_available = false;
        return ret;
    }

    s_sd_available = true;

    // Print card info
    ESP_LOGI(TAG, "SD card mounted OK");
    ESP_LOGI(TAG, "  Type: %s", s_card->is_mmc ? "MMC" : "SD");
    ESP_LOGI(TAG, "  Name: %s", s_card->cid.name);
    ESP_LOGI(TAG, "  Size: %lluMB", ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size / (1024 * 1024));

    // Create recordings directory if it doesn't exist
    struct stat st;
    if (stat(RECORDINGS_PATH, &st) != 0) {
        mkdir(RECORDINGS_PATH, 0775);
        ESP_LOGI(TAG, "Created recordings directory");
    }

    return ESP_OK;
}

float storage_get_free_percent(void)
{
    struct statvfs vfs;
    if (statvfs("/sdcard", &vfs) != 0) {
        ESP_LOGE(TAG, "statvfs failed: %s", strerror(errno));
        s_sd_available = false;
        return 0.0f;
    }
    uint64_t total = (uint64_t)vfs.f_blocks * vfs.f_frsize;
    uint64_t free_space = (uint64_t)vfs.f_bfree * vfs.f_frsize;
    if (total == 0) return 0.0f;
    return (float)free_space / (float)total * 100.0f;
}

static void list_files_recursive(const char *dirpath, file_info_t *files, int max_count, int *count)
{
    DIR *dir = opendir(dirpath);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && *count < max_count) {
        if (entry->d_name[0] == '.') continue;

        char fullpath[300];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, entry->d_name);

        struct stat st;
        if (stat(fullpath, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            list_files_recursive(fullpath, files, max_count, count);
            continue;
        }

        size_t nlen = strlen(entry->d_name);
        if (nlen < 5 || strcmp(entry->d_name + nlen - 4, RECORDINGS_SUFFIX) != 0) continue;

        /* Store relative path from RECORDINGS_PATH (e.g. 2026-04/24/REC_xxx.avi) */
        const char *relpath = fullpath + strlen(RECORDINGS_PATH) + 1; /* skip "/sdcard/recordings/" */
        strncpy(files[*count].name, relpath, sizeof(files[*count].name) - 1);
        files[*count].name[sizeof(files[*count].name) - 1] = '\0';
        files[*count].size = (uint32_t)st.st_size;

        struct tm *tm_info = localtime(&st.st_mtime);
        strftime(files[*count].time_str, sizeof(files[*count].time_str),
                 "%Y-%m-%d %H:%M:%S", tm_info);

        (*count)++;
    }
    closedir(dir);
}

int storage_list_files(file_info_t *files, int max_count)
{
    if (!s_sd_available) return 0;

    xSemaphoreTake(s_sd_mutex, portMAX_DELAY);

    int count = 0;
    list_files_recursive(RECORDINGS_PATH, files, max_count, &count);

    xSemaphoreGive(s_sd_mutex);

    if (count > 1) {
        qsort(files, count, sizeof(file_info_t), compare_file_info);
    }

    return count;
}

static bool find_oldest_recursive(const char *dirpath, char *oldest_name, size_t name_size,
                                  char *oldest_fullpath, size_t fullpath_size, uint32_t *oldest_size)
{
    DIR *dir = opendir(dirpath);
    if (!dir) return false;

    bool found = false;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char fullpath[300];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, entry->d_name);

        struct stat st;
        if (stat(fullpath, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (find_oldest_recursive(fullpath, oldest_name, name_size,
                                      oldest_fullpath, fullpath_size, oldest_size)) {
                found = true;
            }
            continue;
        }

        size_t nlen = strlen(entry->d_name);
        if (nlen < 5 || strcmp(entry->d_name + nlen - 4, RECORDINGS_SUFFIX) != 0) continue;

        if (!found || strcmp(entry->d_name, oldest_name) < 0) {
            strncpy(oldest_name, entry->d_name, name_size - 1);
            oldest_name[name_size - 1] = '\0';
            strncpy(oldest_fullpath, fullpath, fullpath_size - 1);
            oldest_fullpath[fullpath_size - 1] = '\0';
            *oldest_size = (uint32_t)st.st_size;
            found = true;
        }
    }
    closedir(dir);
    return found;
}

esp_err_t storage_delete_oldest(void)
{
    if (!s_sd_available) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_sd_mutex, portMAX_DELAY);

    char oldest_name[64] = {0};
    char oldest_fullpath[300] = {0};
    uint32_t oldest_size = 0;

    bool found = find_oldest_recursive(RECORDINGS_PATH, oldest_name, sizeof(oldest_name),
                                       oldest_fullpath, sizeof(oldest_fullpath), &oldest_size);

    if (!found) {
        xSemaphoreGive(s_sd_mutex);
        ESP_LOGW(TAG, "No recordings to delete");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t result;
    if (remove(oldest_fullpath) == 0) {
        ESP_LOGI(TAG, "Deleted oldest: %s (%lu bytes)", oldest_name, (unsigned long)oldest_size);
        result = ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to delete %s: %s", oldest_name, strerror(errno));
        result = ESP_FAIL;
    }

    xSemaphoreGive(s_sd_mutex);
    return result;
}

esp_err_t storage_cleanup(void)
{
    if (!s_sd_available) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_sd_mutex, portMAX_DELAY);

    float free_pct = storage_get_free_percent();
    ESP_LOGI(TAG, "Storage free: %.1f%%", free_pct);

    if (free_pct >= CLEANUP_LOW_PCT) {
        xSemaphoreGive(s_sd_mutex);
        return ESP_OK;   // plenty of space
    }

    ESP_LOGW(TAG, "Storage low (%.1f%%), starting cleanup", free_pct);
    xSemaphoreGive(s_sd_mutex);

    int deleted = 0;
    while (storage_get_free_percent() < CLEANUP_HIGH_PCT) {
        esp_err_t err = storage_delete_oldest();
        if (err != ESP_OK) break;
        deleted++;
        if (deleted > 100) break;   // safety limit
    }

    float new_pct = storage_get_free_percent();
    ESP_LOGI(TAG, "Cleanup done: deleted %d files, free now %.1f%%", deleted, new_pct);
    return ESP_OK;
}

bool storage_is_available(void)
{
    return s_sd_available;
}

esp_err_t storage_check(void)
{
    struct stat st;
    if (stat("/sdcard", &st) == 0) {
        return ESP_OK;
    }
    s_sd_available = false;
    return ESP_FAIL;
}

void storage_set_unavailable(void)
{
    s_sd_available = false;
    ESP_LOGW(TAG, "SD card marked unavailable");
}

esp_err_t storage_remount(void)
{
    ESP_LOGI(TAG, "Attempting SD card remount...");

    /* Unmount old (may fail if already gone, that's OK) */
    esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
    s_sd_available = false;

    /* Re-mount with same config */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = SD_PIN_CLK;
    slot_config.cmd = SD_PIN_CMD;
    slot_config.width = 1;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 64 * 1024,
    };

    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD remount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_sd_available = true;
    ESP_LOGI(TAG, "SD card remounted successfully");
    return ESP_OK;
}
