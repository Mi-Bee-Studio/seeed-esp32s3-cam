/*
 * Copyright (C) 2024 MiBeeHomeCam Authors
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
#include <dirent.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include "status_led.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "ff.h"  /* FatFs: f_getfree for free space query */
#include "driver/sdspi_host.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "storage";

// SD card pin config for XIAO ESP32-S3 Sense (SPI mode, per Seeed Studio wiki)
#define SD_PIN_CS    21
#define SD_PIN_CLK   7
#define SD_PIN_MOSI  9
#define SD_PIN_MISO  8

#define RECORDINGS_PATH  "/sdcard/recordings"
#define RECORDINGS_SUFFIX ".avi"
#define CLEANUP_LOW_PCT  20.0f
#define CLEANUP_HIGH_PCT 30.0f

static bool s_sd_available = false;
static SemaphoreHandle_t s_sd_mutex = NULL;
static sdmmc_card_t *s_card = NULL;

/* ---- In-memory file cache (ring buffer) ---- */
#define FILE_CACHE_SIZE  64  /* max cached file entries */
static file_info_t s_file_cache[FILE_CACHE_SIZE];
static int s_file_cache_count = 0;
static int s_file_cache_head = 0;  /* oldest entry index (for ring eviction) */
/* ---- internal helpers ---- */

/* Forward declarations */
static void list_files_recursive(const char *dirpath, file_info_t *files, int max_count, int *count);
static void storage_rebuild_cache(void);


/**
 * @brief 文件信息比较函数，按文件名升序排列（即最旧时间戳优先）
 */
static int compare_file_info(const void *a, const void *b)
{
    // Sort by name ascending (oldest timestamp first)
    return strcmp(((const file_info_t *)a)->name, ((const file_info_t *)b)->name);
}

/* ---- public API ---- */

/**
 * @brief 初始化SD卡，配置SPI模式、挂载FAT文件系统、创建录像目录
 *
 * XIAO ESP32-S3 Sense 的 microSD 卡槽使用 SPI 模式连接:
 *   CS=GPIO21  SCK=GPIO7  MOSI=GPIO9  MISO=GPIO8
 * 参考: https://wiki.seeedstudio.com/cn/xiao_esp32s3_sense_filesystem/
 */
esp_err_t storage_init(void)
{
    esp_err_t ret;

    /* Create SD mutex for concurrent access protection */
    s_sd_mutex = xSemaphoreCreateMutex();
    if (!s_sd_mutex) {
        ESP_LOGE(TAG, "Failed to create SD mutex");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Initializing SD card (SPI mode): CS=%d SCK=%d MOSI=%d MISO=%d",
             SD_PIN_CS, SD_PIN_CLK, SD_PIN_MOSI, SD_PIN_MISO);

    /* Disable LED on GPIO21 — now repurposed as SD card SPI CS */
    led_disable();

    /* Reset SD card GPIO pins to default state */
    gpio_reset_pin(SD_PIN_CS);
    gpio_reset_pin(SD_PIN_CLK);
    gpio_reset_pin(SD_PIN_MOSI);
    gpio_reset_pin(SD_PIN_MISO);

    /* Allow SD card power to stabilize */
    vTaskDelay(pdMS_TO_TICKS(200));

    /* ---- Initialize SPI bus (IDF v6.0: must call before sdspi_mount) ---- */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;  /* Use SPI3 to avoid SPI2 conflict with flash/camera */
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_CLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = 4096,
    };
    ret = spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* SPI device (slot) configuration — IDF v6.0: only CS pin here */
    sdspi_device_config_t device_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    device_config.gpio_cs = SD_PIN_CS;
    device_config.host_id = SPI3_HOST;

    /* VFS fat mount config */
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 64 * 1024,
    };

    /*
     * Retry SD card init up to 5 times with increasing delay.
     * Some TF cards need extra time to stabilize after power-on.
     */
    const int max_attempts = 5;
    const int delays_ms[] = {300, 500, 1000, 1000, 1000};
    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        ESP_LOGI(TAG, "SD card init attempt %d/%d...", attempt, max_attempts);
        ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &device_config, &mount_config, &s_card);
        if (ret == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "SD card init attempt %d/%d failed: %s (0x%x)",
                 attempt, max_attempts, esp_err_to_name(ret), ret);
        if (attempt < max_attempts) {
            vTaskDelay(pdMS_TO_TICKS(delays_ms[attempt - 1]));
        }
    }

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount SD card (format or filesystem issue)");
        } else {
            ESP_LOGE(TAG, "SD card init failed: %s", esp_err_to_name(ret));
        }
        spi_bus_free(SPI3_HOST);
        s_sd_available = false;
        return ret;
    }

    s_sd_available = true;

    // Print card info
    ESP_LOGI(TAG, "SD card mounted OK (SPI mode)");
    ESP_LOGI(TAG, "  Type: %s", s_card->is_mmc ? "MMC" : "SD");
    ESP_LOGI(TAG, "  Name: %s", s_card->cid.name);
    ESP_LOGI(TAG, "  Size: %lluMB", ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size / (1024 * 1024));

    /* Create recordings directory if it doesn't exist */
    struct stat st;
    if (stat(RECORDINGS_PATH, &st) != 0) {
        mkdir(RECORDINGS_PATH, 0775);
        ESP_LOGI(TAG, "Created recordings directory");
    }
    
    /* Rebuild file cache from existing files on SD card */
    storage_rebuild_cache();

    return ESP_OK;
}

/**
 * @brief 查询SD卡剩余空间占总空间的百分比
 */
float storage_get_free_percent(void)
{
    if (!s_sd_available || !s_card) {
        return 0.0f;
    }
    /* Use FatFs f_getfree since IDF v6.0 FAT VFS does not support statvfs */
    DWORD free_clusters = 0;
    FATFS *fs = NULL;
    FRESULT res = f_getfree("0:", &free_clusters, &fs);
    if (res != FR_OK || !fs) {
        ESP_LOGE(TAG, "f_getfree failed: %d", res);
        return 0.0f;
    }
    uint32_t total_clusters = (fs->n_fatent - 2);  /* FAT has 2 reserved entries */
    uint64_t sector_size = fs->ssize;
    uint64_t sectors_per_cluster = fs->csize;
    uint64_t cluster_size = sector_size * sectors_per_cluster;
    uint64_t total = (uint64_t)total_clusters * cluster_size;
    uint64_t free_space = (uint64_t)free_clusters * cluster_size;
    if (total == 0) return 0.0f;
    return (float)free_space / (float)total * 100.0f;
}

/**
 * @brief 获取SD卡存储信息（总容量和剩余空间）
 */
esp_err_t storage_get_info(storage_info_t *info)
{
    if (!s_sd_available || !s_card || !info) {
        return ESP_ERR_INVALID_STATE;
    }
    
    /* Use FatFs f_getfree to get total and free space */
    DWORD free_clusters = 0;
    FATFS *fs = NULL;
    FRESULT res = f_getfree("0:", &free_clusters, &fs);
    if (res != FR_OK || !fs) {
        ESP_LOGE(TAG, "f_getfree failed: %d", res);
        return ESP_FAIL;
    }
    
    uint32_t total_clusters = (fs->n_fatent - 2);  /* FAT has 2 reserved entries */
    uint64_t sector_size = fs->ssize;
    uint64_t sectors_per_cluster = fs->csize;
    uint64_t cluster_size = sector_size * sectors_per_cluster;
    
    info->total_bytes = (uint64_t)total_clusters * cluster_size;
    info->free_bytes = (uint64_t)free_clusters * cluster_size;
    
    return ESP_OK;
}

/**
 * @brief 递归扫描目录，收集所有.avi录像文件的信息
 */
static void list_files_recursive(const char *dirpath, file_info_t *files, int max_count, int *count)
{
    DIR *dir = opendir(dirpath);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && *count < max_count) {
        if (entry->d_name[0] == '.') continue;

        /* Use d_type to skip stat() for directories — huge speedup on SPI SD */
        if (entry->d_type == DT_DIR) {
            char fullpath[300];
            snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, entry->d_name);
            list_files_recursive(fullpath, files, max_count, count);
            continue;
        }

        /* Skip non-.avi files early — no stat() needed */
        size_t nlen = strlen(entry->d_name);
        if (nlen < 5 || strcmp(entry->d_name + nlen - 4, RECORDINGS_SUFFIX) != 0) continue;

        /* Only stat() .avi files (for size and mtime) */
        char fullpath[300];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, entry->d_name);

        struct stat st;
        if (stat(fullpath, &st) != 0) continue;

        /* Store relative path from RECORDINGS_PATH */
        const char *relpath = fullpath + strlen(RECORDINGS_PATH) + 1;
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

/**
 * @brief Rebuild the file cache from SD card (one-time operation)
 *
 * Scans all .avi files and populates s_file_cache so file list
 * is available immediately after boot, not only after first segment completes.
 */
static void storage_rebuild_cache(void)
{
    if (!s_sd_available) {
        ESP_LOGW(TAG, "SD card not available, skipping cache rebuild");
        return;
    }
    
    xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    
    /* Scan all .avi files into heap buffer (too large for stack) */
    file_info_t *temp_files = malloc(FILE_CACHE_SIZE * sizeof(file_info_t));
    if (!temp_files) {
        xSemaphoreGive(s_sd_mutex);
        ESP_LOGE(TAG, "No memory for file cache rebuild");
        return;
    }
    int count = 0;
    list_files_recursive(RECORDINGS_PATH, temp_files, FILE_CACHE_SIZE, &count);
    
    /* Sort files by name (oldest first) */
    qsort(temp_files, count, sizeof(file_info_t), compare_file_info);
    
    /* Copy to cache (truncate if too many) */
    s_file_cache_count = (count < FILE_CACHE_SIZE) ? count : FILE_CACHE_SIZE;
    for (int i = 0; i < s_file_cache_count; i++) {
        s_file_cache[i] = temp_files[i];
    }
    free(temp_files);
    
    xSemaphoreGive(s_sd_mutex);
    
    ESP_LOGI(TAG, "Rebuilt file cache: %d files found", s_file_cache_count);
}

/**
 * @brief 注册完成的录像文件到内存缓存
 *
 * 由 video_recorder 的分段完成回调调用，避免后续文件列表需要扫描 SD 卡。
 * 环形缓冲区满时覆盖最旧条目。
 */
void storage_register_file(const char *filepath, size_t size)
{
    xSemaphoreTake(s_sd_mutex, portMAX_DELAY);

    int idx;
    if (s_file_cache_count < FILE_CACHE_SIZE) {
        idx = s_file_cache_count;
        s_file_cache_count++;
    } else {
        /* Ring buffer full — evict oldest, shift remaining */
        idx = s_file_cache_count - 1;
        for (int i = 0; i < s_file_cache_count - 1; i++) {
            s_file_cache[i] = s_file_cache[i + 1];
        }
    }

    /* Store relative path from /sdcard/recordings/ */
    const char *relpath = filepath;
    const char *prefix = RECORDINGS_PATH "/";
    if (strncmp(filepath, prefix, strlen(prefix)) == 0) {
        relpath = filepath + strlen(prefix);
    }
    strncpy(s_file_cache[idx].name, relpath, sizeof(s_file_cache[idx].name) - 1);
    s_file_cache[idx].name[sizeof(s_file_cache[idx].name) - 1] = '\0';
    s_file_cache[idx].size = (uint32_t)size;

    /* Extract timestamp from filename REC_YYYYMMDD_HHMMSS.avi */
    int y = 0, m = 0, d = 0, H = 0, M = 0, S = 0;
    if (sscanf(relpath, "%*[^R]REC_%4d%2d%2d_%2d%2d%2d", &y, &m, &d, &H, &M, &S) == 6) {
        snprintf(s_file_cache[idx].time_str, sizeof(s_file_cache[idx].time_str),
                 "%04d-%02d-%02d %02d:%02d:%02d", y, m, d, H, M, S);
    } else {
        s_file_cache[idx].time_str[0] = '\0';
    }

    xSemaphoreGive(s_sd_mutex);
}

/**
 * @brief 从内存缓存移除已删除的文件
 */
void storage_unregister_file(const char *name)
{
    xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    for (int i = 0; i < s_file_cache_count; i++) {
        if (strcmp(s_file_cache[i].name, name) == 0) {
            for (int j = i; j < s_file_cache_count - 1; j++) {
                s_file_cache[j] = s_file_cache[j + 1];
            }
            s_file_cache_count--;
            break;
        }
    }
    xSemaphoreGive(s_sd_mutex);
}

/**
 * @brief 获取录像文件列表
 *
 * 优先从内存缓存读取（毫秒级），仅缓存为空时回退到 SD 卡目录扫描。
 */
int storage_list_files(file_info_t *files, int max_count)
{
    if (!s_sd_available) return 0;

    xSemaphoreTake(s_sd_mutex, portMAX_DELAY);

    if (s_file_cache_count > 0) {
        /* Serve from in-memory cache — zero SD card I/O */
        int count = s_file_cache_count < max_count ? s_file_cache_count : max_count;
        /* Return newest first (copy in reverse order) */
        for (int i = 0; i < count; i++) {
            files[i] = s_file_cache[s_file_cache_count - 1 - i];
        }
        xSemaphoreGive(s_sd_mutex);
        return count;
    }
    xSemaphoreGive(s_sd_mutex);

    /* Cold start: cache empty. Return empty list — cache fills as segments complete. */
    return 0;
}

/**
 * @brief 递归查找目录中最旧的录像文件
 */
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

/**
 * @brief 删除最旧的录像文件，线程安全
 */
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
        /* Evict from in-memory cache */
        for (int i = 0; i < s_file_cache_count; i++) {
            if (strcmp(s_file_cache[i].name, oldest_name) == 0) {
                for (int j = i; j < s_file_cache_count - 1; j++) {
                    s_file_cache[j] = s_file_cache[j + 1];
                }
                s_file_cache_count--;
                break;
            }
        }
        result = ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to delete %s: %s", oldest_name, strerror(errno));
        result = ESP_FAIL;
    }

    xSemaphoreGive(s_sd_mutex);
    return result;
}

/**
 * @brief 存储空间自动清理，低于20%时循环删除旧文件直到剩余30%以上
 */
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

/**
 * @brief 查询SD卡是否可用
 */
bool storage_is_available(void)
{
    return s_sd_available;
}

/**
 * @brief 检查SD卡是否仍然挂载，通过stat /sdcard目录判断
 */
esp_err_t storage_check(void)
{
    struct stat st;
    if (stat("/sdcard", &st) == 0) {
        return ESP_OK;
    }
    s_sd_available = false;
    return ESP_FAIL;
}

/**
 * @brief 标记SD卡为不可用状态
 */
void storage_set_unavailable(void)
{
    s_sd_available = false;
    ESP_LOGW(TAG, "SD card marked unavailable");
}

/**
 * @brief 格式化SD卡，擦除所有数据并重新创建FAT32文件系统
 *
 * 实现步骤：
 * 1. 向SD卡前两个扇区写入零，破坏FAT文件系统引导扇区
 * 2. 卸载当前挂载的文件系统
 * 3. 以 format_if_mount_failed=true 重新挂载，触发自动格式化
 * 4. 重新创建录像目录
 */
/* Helper: initialize SPI bus and mount SD card */
static esp_err_t sdspi_mount(bool format_if_failed)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_CLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = 4096,
    };
    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    sdspi_device_config_t device_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    device_config.gpio_cs = SD_PIN_CS;
    device_config.host_id = SPI3_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = format_if_failed,
        .max_files = 8,
        .allocation_unit_size = 64 * 1024,
    };

    ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &device_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        spi_bus_free(SPI3_HOST);
    }
    return ret;
}

/* Helper: unmount SD card and free SPI bus */
static void sdspi_unmount(void)
{
    esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
    spi_bus_free(SPI3_HOST);
    s_sd_available = false;
}

/**
 * @brief 格式化SD卡，擦除所有数据并重新创建FAT32文件系统
 */
esp_err_t storage_format(void)
{
    if (!s_sd_available || !s_card) {
        ESP_LOGE(TAG, "SD card not available for formatting");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Formatting SD card...");

    /* Wipe first 2 sectors to destroy FAT superblock */
    uint8_t zeros[512 * 2];
    memset(zeros, 0, sizeof(zeros));
    esp_err_t ret = sdmmc_write_sectors(s_card, zeros, 0, 2);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to wipe SD card sectors: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Unmount and free SPI bus */
    sdspi_unmount();

    /* Re-mount with format flag (SPI bus re-initialized by helper) */
    ret = sdspi_mount(true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD format remount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_sd_available = true;

    /* Recreate recordings directory */
    mkdir(RECORDINGS_PATH, 0775);
    ESP_LOGI(TAG, "SD card formatted and remounted OK");
    return ESP_OK;
}

/**
 * @brief 卸载并重新挂载SD卡，用于热插拔恢复
 */
esp_err_t storage_remount(void)
{
    ESP_LOGI(TAG, "Attempting SD card remount...");

    /* Unmount old (may fail if already gone, that's OK) */
    sdspi_unmount();

    /* Reset GPIO pins */
    gpio_reset_pin(SD_PIN_CS);
    gpio_reset_pin(SD_PIN_CLK);
    gpio_reset_pin(SD_PIN_MOSI);
    gpio_reset_pin(SD_PIN_MISO);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Re-mount (SPI bus re-initialized by helper) */
    esp_err_t ret = sdspi_mount(false);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD remount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_sd_available = true;
    ESP_LOGI(TAG, "SD card remounted successfully");
    return ESP_OK;
}
