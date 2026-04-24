#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[64];
    char ftp_host[64];
    uint16_t ftp_port;
    char ftp_user[32];
    char ftp_pass[32];
    char ftp_path[128];
    bool ftp_enabled;
    char webdav_url[128];
    char webdav_user[32];
    char webdav_pass[32];
    bool webdav_enabled;
    uint8_t resolution;    // 0=VGA, 1=SVGA, 2=XGA
    uint8_t fps;            // 1-30
    uint16_t segment_sec;   // seconds per segment
    uint8_t jpeg_quality;   // 1-63
    char web_password[32];
    char device_name[32];
} cam_config_t;

esp_err_t config_init(void);         // Load from NVS, apply defaults if empty
cam_config_t* config_get(void);      // Returns pointer to current config in PSRAM
esp_err_t config_save(void);         // Save current config to NVS
esp_err_t config_reset(void);        // Restore factory defaults
esp_err_t config_load_from_sd(void); // Read config/wifi.txt + config/nas.txt from SD
