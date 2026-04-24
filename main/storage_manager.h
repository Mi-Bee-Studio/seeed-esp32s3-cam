#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    char name[64];
    uint32_t size;
    char time_str[32];
} file_info_t;

esp_err_t storage_init(void);
float storage_get_free_percent(void);
int storage_list_files(file_info_t *files, int max_count);
esp_err_t storage_delete_oldest(void);
esp_err_t storage_cleanup(void);   // Called after each segment write
bool storage_is_available(void);
esp_err_t storage_check(void);           // Check SD still mounted
esp_err_t storage_remount(void);         // Unmount + remount SD card
void storage_set_unavailable(void);      // Mark SD as removed
