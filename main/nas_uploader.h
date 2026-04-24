#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t nas_uploader_init(void);
esp_err_t nas_uploader_enqueue(const char *filepath);
void nas_uploader_get_status(char *last_upload, size_t len, int *queue_count, bool *paused);
uint32_t nas_uploader_get_stack_hwm(void);
