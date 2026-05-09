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

#include "http_upload_client.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static const char *TAG = "http_upload";

/* ---- Constants ---- */

#define MAX_RETRIES       3
#define INITIAL_DELAY_MS  1000
#define UPLOAD_CHUNK_SIZE 4096

/* ---- Helpers ---- */

/** @brief Set Basic Auth header on HTTP client */
static void set_auth_header(esp_http_client_handle_t client, const char *user, const char *pass)
{
    char credentials[128];
    snprintf(credentials, sizeof(credentials), "%s:%s", user, pass);

    size_t encoded_len = 0;
    char encoded[160];
    mbedtls_base64_encode((unsigned char *)encoded, sizeof(encoded), &encoded_len,
                          (const unsigned char *)credentials, strlen(credentials));
    encoded[encoded_len] = '\0';

    char auth_header[200];
    snprintf(auth_header, sizeof(auth_header), "Basic %s", encoded);
    esp_http_client_set_header(client, "Authorization", auth_header);
}

/* ---- Public API ---- */

esp_err_t http_upload_file(const http_upload_config_t *config,
                           const char *remote_path,
                           const char *local_path)
{
    /* Build full URL */
    char url[512];
    snprintf(url, sizeof(url), "%s/%s", config->url, remote_path);

    /* Open local file */
    FILE *f = fopen(local_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open local file: %s", local_path);
        return ESP_FAIL;
    }

    /* Get file size */
    struct stat st;
    if (stat(local_path, &st) != 0) {
        ESP_LOGE(TAG, "Cannot stat local file: %s", local_path);
        fclose(f);
        return ESP_FAIL;
    }
    long file_size = st.st_size;

    ESP_LOGI(TAG, "Uploading %s (%ld bytes) -> %s", local_path, file_size, remote_path);

    esp_err_t result = ESP_FAIL;
    bool is_https = (strncmp(config->url, "https://", 8) == 0);

    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        if (attempt > 0) {
            int delay = INITIAL_DELAY_MS * (1 << (attempt - 1));  /* 1s, 2s, 4s */
            ESP_LOGW(TAG, "Retry %d/%d after %d ms", attempt + 1, MAX_RETRIES, delay);
            vTaskDelay(pdMS_TO_TICKS(delay));
            fseek(f, 0, SEEK_SET);  /* Reset file position */
        }

        esp_http_client_config_t http_cfg = {
            .url = url,
            .method = HTTP_METHOD_PUT,
            .timeout_ms = 30000,
            .buffer_size = 2048,
        };

        if (is_https) {
            http_cfg.cert_pem = NULL;
            http_cfg.skip_cert_common_name_check = config->skip_cert_verify;
            http_cfg.use_global_ca_store = false;
        }

        esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
        if (!client) {
            ESP_LOGE(TAG, "Failed to init HTTP client");
            continue;
        }

        /* Set auth header if user is configured */
        if (config->user[0] != '\0') {
            set_auth_header(client, config->user, config->pass);
        }
        esp_http_client_set_header(client, "Content-Type", "video/avi");

        /* Open connection with content length */
        esp_err_t open_err = esp_http_client_open(client, (int)file_size);
        if (open_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(open_err));
            esp_http_client_cleanup(client);
            continue;
        }

        /* Stream file in chunks */
        uint8_t *buf = malloc(UPLOAD_CHUNK_SIZE);
        if (!buf) {
            ESP_LOGE(TAG, "OOM alloc chunk buffer");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            continue;
        }

        size_t total_written = 0;
        bool write_error = false;

        while (total_written < (size_t)file_size) {
            size_t to_read = UPLOAD_CHUNK_SIZE;
            if (total_written + to_read > (size_t)file_size) {
                to_read = (size_t)file_size - total_written;
            }

            size_t nread = fread(buf, 1, to_read, f);
            if (nread == 0) {
                ESP_LOGE(TAG, "fread error at offset %zu", total_written);
                write_error = true;
                break;
            }

            int nwritten = esp_http_client_write(client, (const char *)buf, (int)nread);
            if (nwritten < 0) {
                ESP_LOGE(TAG, "HTTP write error at offset %zu", total_written);
                write_error = true;
                break;
            }

            total_written += (size_t)nwritten;
        }

        free(buf);
        esp_http_client_close(client);

        if (write_error) {
            esp_http_client_cleanup(client);
            continue;
        }

        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        ESP_LOGI(TAG, "Upload HTTP %d (%zu / %ld bytes)", status, total_written, file_size);

        if (status == 200 || status == 201 || status == 204) {
            ESP_LOGI(TAG, "Upload success: %s", remote_path);
            result = ESP_OK;
            break;
        }

        ESP_LOGW(TAG, "Upload failed with HTTP %d, attempt %d/%d", status, attempt + 1, MAX_RETRIES);
    }

    fclose(f);
    return result;
}

esp_err_t http_upload_mkdir(const http_upload_config_t *config,
                            const char *path)
{
    /* Build full URL */
    char url[512];
    snprintf(url, sizeof(url), "%s/%s", config->url, path);

    bool is_https = (strncmp(config->url, "https://", 8) == 0);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .buffer_size = 1024,
    };

    if (is_https) {
        http_cfg.cert_pem = NULL;
        http_cfg.skip_cert_common_name_check = config->skip_cert_verify;
        http_cfg.use_global_ca_store = false;
    }

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for mkdir");
        return ESP_FAIL;
    }

    if (config->user[0] != '\0') {
        set_auth_header(client, config->user, config->pass);
    }

    /* Best-effort: POST with empty body */
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mkdir HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return ESP_OK;  /* best-effort, don't propagate */
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    /* Best-effort: directory may already exist, ignore status */
    ESP_LOGD(TAG, "mkdir %s -> HTTP %d", path, status);
    return ESP_OK;
}
