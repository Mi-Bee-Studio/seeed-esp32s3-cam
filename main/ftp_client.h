#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char host[64];
    uint16_t port;
    char user[32];
    char pass[32];
} ftp_config_t;

esp_err_t ftp_connect(const ftp_config_t *cfg);
esp_err_t ftp_upload(const char *remote_path, const char *local_path);
esp_err_t ftp_mkdir_recursive(const char *path);
void ftp_disconnect(void);
bool ftp_is_connected(void);
