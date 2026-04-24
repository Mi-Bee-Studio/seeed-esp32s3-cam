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

#include "webdav_client.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static const char *TAG = "webdav";

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

/**
 * @brief 设置HTTP客户端的Basic认证头
 * 将 user:pass 进行Base64编码后设置为Authorization头
 * @param client HTTP客户端句柄
 * @param user 用户名
 * @param pass 密码
 */
static void set_auth_header(esp_http_client_handle_t client, const char *user, const char *pass)
{
    char credentials[96];
    snprintf(credentials, sizeof(credentials), "%s:%s", user, pass);

    size_t encoded_len = 0;
    char encoded[128];
    mbedtls_base64_encode((unsigned char *)encoded, sizeof(encoded), &encoded_len,
                          (const unsigned char *)credentials, strlen(credentials));
    encoded[encoded_len] = '\0';

    char auth_header[160];
    snprintf(auth_header, sizeof(auth_header), "Basic %s", encoded);
    esp_http_client_set_header(client, "Authorization", auth_header);
}

/**
 * Build the full URL: cfg->url + remote_path into a malloc'd buffer.
 * Caller must free.
 */
/**
 * @brief 构建完整的WebDAV URL
 * 将配置中的服务器URL和远程路径拼接为完整URL
 * @param cfg WebDAV连接配置
 * @param remote_path 远程资源路径
 * @return malloc分配的URL字符串，调用者需释放
 */
static char *build_url(const webdav_config_t *cfg, const char *remote_path)
{
    /* url(128) + remote_path(256) + slack */
    size_t len = strlen(cfg->url) + strlen(remote_path) + 2;
    char *url = malloc(len);
    if (!url) return NULL;

    snprintf(url, len, "%s%s", cfg->url, remote_path);
    return url;
}

/* ------------------------------------------------------------------ */
/*  webdav_exists  (HTTP HEAD)                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief 通过HTTP HEAD检查远程资源是否存在
 * @param cfg WebDAV连接配置
 * @param remote_path 远程资源路径
 * @return ESP_OK 存在(200)，ESP_ERR_NOT_FOUND 不存在(404)，ESP_FAIL 其他错误
 */
esp_err_t webdav_exists(const webdav_config_t *cfg, const char *remote_path)
{
    char *url = build_url(cfg, remote_path);
    if (!url) {
        ESP_LOGE(TAG, "OOM building URL");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_HEAD,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        free(url);
        return ESP_FAIL;
    }

    set_auth_header(client, cfg->user, cfg->pass);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);
    free(url);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HEAD %s transport error: %s", remote_path, esp_err_to_name(err));
        return err;
    }

    if (status == 200) {
        ESP_LOGD(TAG, "HEAD %s -> 200 (exists)", remote_path);
        return ESP_OK;
    }

    if (status == 404) {
        ESP_LOGD(TAG, "HEAD %s -> 404 (not found)", remote_path);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGW(TAG, "HEAD %s unexpected status %d", remote_path, status);
    return ESP_FAIL;
}

/* ------------------------------------------------------------------ */
/*  webdav_mkdir  (MKCOL via raw HTTP over TCP)                       */
/* ------------------------------------------------------------------ */

/**
 * ESP-IDF's esp_http_client doesn't natively support MKCOL.
 * We send a raw HTTP request over a TCP socket using esp_transport.
 */
#include "esp_transport_tcp.h"

/**
 * @brief 发送原始HTTP请求（支持非标准方法如MKCOL）
 * 通过TCP socket直接发送HTTP请求并解析响应状态行
 * @param host 目标主机名
 * @param port 目标端口
 * @param method HTTP方法（如MKCOL）
 * @param path 请求路径
 * @param auth_header 认证头值
 * @param out_status 输出HTTP状态码
 * @return ESP_OK 成功，ESP_FAIL TCP连接或解析失败
 */
static esp_err_t raw_http_request(const char *host, int port,
                                  const char *method, const char *path,
                                  const char *auth_header,
                                  int *out_status)
{
    esp_transport_handle_t tcp = esp_transport_tcp_init();
    if (!tcp) {
        ESP_LOGE(TAG, "Failed to init TCP transport");
        return ESP_FAIL;
    }

    esp_transport_tcp_set_keep_alive(tcp, &(esp_transport_keep_alive_t){
        .keep_alive_enable = true,
        .keep_alive_idle = 5,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
    });

    /* Timeout 10 seconds */

    int sock = esp_transport_connect(tcp, host, port, 10000);
    if (sock < 0) {
        ESP_LOGE(TAG, "TCP connect to %s:%d failed", host, port);
        esp_transport_destroy(tcp);
        return ESP_FAIL;
    }

    /* Build HTTP request */
    char req[512];
    int req_len = snprintf(req, sizeof(req),
                           "%s %s HTTP/1.1\r\n"
                           "Host: %s:%d\r\n"
                           "Authorization: %s\r\n"
                           "Content-Length: 0\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           method, path, host, port, auth_header);

    int written = esp_transport_write(tcp, req, req_len, 5000);
    if (written < 0) {
        ESP_LOGE(TAG, "TCP write failed");
        esp_transport_close(tcp);
        esp_transport_destroy(tcp);
        return ESP_FAIL;
    }

    /* Read response — we only need the status line */
    char resp[256] = {0};
    int rlen = esp_transport_read(tcp, resp, sizeof(resp) - 1, 5000);

    esp_transport_close(tcp);
    esp_transport_destroy(tcp);

    if (rlen <= 0) {
        ESP_LOGW(TAG, "No response for %s %s", method, path);
        return ESP_FAIL;
    }

    /* Parse "HTTP/1.x NNN ..." */
    int status = 0;
    if (sscanf(resp, "HTTP/%*d.%*d %d", &status) != 1) {
        ESP_LOGW(TAG, "Can't parse status from: %.80s", resp);
        return ESP_FAIL;
    }

    if (out_status) *out_status = status;
    ESP_LOGD(TAG, "%s %s -> %d", method, path, status);
    return ESP_OK;
}

/**
 * Parse host/port from cfg->url.
 * Returns malloc'd host, sets *port.
 * Caller must free host.
 */
/**
 * @brief 从配置URL中解析主机名和端口
 * 支持格式："http://host:port" 或 "http://host"（默认端口80）
 * @param cfg WebDAV连接配置
 * @param port 输出解析得到的端口号
 * @return malloc分配的主机名字符串，调用者需释放
 */
static char *parse_host_port(const webdav_config_t *cfg, int *port)
{
    /* Expect "http://host:port" */
    const char *start = cfg->url;
    if (strncmp(start, "http://", 7) == 0) start += 7;

    const char *colon = strchr(start, ':');
    const char *slash = strchr(start, '/');
    const char *end = slash ? slash : start + strlen(start);

    char *host = NULL;
    if (colon && colon < end) {
        /* host:port */
        size_t hlen = colon - start;
        host = malloc(hlen + 1);
        memcpy(host, start, hlen);
        host[hlen] = '\0';
        *port = atoi(colon + 1);
    } else {
        size_t hlen = end - start;
        host = malloc(hlen + 1);
        memcpy(host, start, hlen);
        host[hlen] = '\0';
        *port = 80;
    }
    return host;
}

/**
 * Build the Basic auth header value (static buffer).
 */
/**
 * @brief 构建Basic认证值字符串
 * 将 user:pass 进行Base64编码
 * @param cfg WebDAV连接配置
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 */
static void build_auth_value(const webdav_config_t *cfg, char *buf, size_t buf_size)
{
    char credentials[96];
    snprintf(credentials, sizeof(credentials), "%s:%s", cfg->user, cfg->pass);

    size_t encoded_len = 0;
    mbedtls_base64_encode((unsigned char *)buf, buf_size, &encoded_len,
                          (const unsigned char *)credentials, strlen(credentials));
    buf[encoded_len] = '\0';
}

/**
 * @brief 通过MKCOL请求创建WebDAV远程目录
 * 201=创建成功，405=已存在，200/204=部分服务器已存在响应，均返回ESP_OK
 * @param cfg WebDAV连接配置
 * @param remote_dir 远程目录路径
 * @return ESP_OK 创建成功或已存在，ESP_FAIL 失败
 */
esp_err_t webdav_mkdir(const webdav_config_t *cfg, const char *remote_dir)
{
    int port = 80;
    char *host = parse_host_port(cfg, &port);
    if (!host) return ESP_ERR_NO_MEM;

    char auth[128];
    build_auth_value(cfg, auth, sizeof(auth));

    /* Build auth header string "Basic xxxxx" */
    char auth_header[160];
    snprintf(auth_header, sizeof(auth_header), "Basic %s", auth);

    int status = 0;
    esp_err_t err = raw_http_request(host, port, "MKCOL", remote_dir, auth_header, &status);
    free(host);

    if (err != ESP_OK) return err;

    /* 201 = Created, 405 = Already exists, 301 = redirect (dir exists) */
    if (status == 201 || status == 405) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "MKCOL %s unexpected status %d", remote_dir, status);
    /* Some servers return 200 or 204 for existing dirs */
    if (status == 200 || status == 204) return ESP_OK;

    return ESP_FAIL;
}

/* ------------------------------------------------------------------ */
/*  webdav_mkdir_recursive                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief 递归创建远程路径中的所有目录层级
 * 按路径分隔符逐级创建，如 /A/B/C → 依次创建 /A、/A/B、/A/B/C
 * @param cfg WebDAV连接配置
 * @param path 远程目录路径
 * @return ESP_OK 全部成功，或最后一个失败目录的错误码
 */
esp_err_t webdav_mkdir_recursive(const webdav_config_t *cfg, const char *path)
{
    /* Work on a copy so we can NUL-terminate at each '/' */
    char tmp[256];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    esp_err_t last_err = ESP_OK;

    /* Walk through path components: /A/B/C -> create /A, /A/B, /A/B/C */
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            ESP_LOGD(TAG, "mkdir_recursive: creating %s", tmp);
            esp_err_t err = webdav_mkdir(cfg, tmp);
            if (err != ESP_OK) last_err = err;
            *p = '/';
        }
    }

    /* Create final component */
    if (strlen(tmp) > 1) {
        ESP_LOGD(TAG, "mkdir_recursive: creating %s", tmp);
        esp_err_t err = webdav_mkdir(cfg, tmp);
        if (err != ESP_OK) last_err = err;
    }

    return last_err;
}

/* ------------------------------------------------------------------ */
/*  webdav_upload  (HTTP PUT with retry)                              */
/* ------------------------------------------------------------------ */

#define UPLOAD_CHUNK_SIZE  4096
#define MAX_RETRIES        3
#define INITIAL_DELAY_MS   1000

/**
 * @brief 通过HTTP PUT上传本地文件到WebDAV远程路径
 * 使用4KB分块流式传输，支持指数退避重试（最多3次，延迟1s/2s/4s）
 * @param cfg WebDAV连接配置
 * @param remote_path 远程文件路径
 * @param local_path 本地文件路径
 * @return ESP_OK 成功（HTTP 200/201/204），ESP_FAIL 上传失败
 */
esp_err_t webdav_upload(const webdav_config_t *cfg, const char *remote_path, const char *local_path)
{
    char *url = build_url(cfg, remote_path);
    if (!url) {
        ESP_LOGE(TAG, "OOM building URL");
        return ESP_ERR_NO_MEM;
    }

    /* Open local file */
    FILE *f = fopen(local_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open local file: %s", local_path);
        free(url);
        return ESP_ERR_NOT_FOUND;
    }

    /* Get file size */
    struct stat st;
    if (stat(local_path, &st) != 0) {
        ESP_LOGE(TAG, "Cannot stat local file: %s", local_path);
        fclose(f);
        free(url);
        return ESP_FAIL;
    }
    long file_size = st.st_size;

    ESP_LOGI(TAG, "Uploading %s (%ld bytes) -> %s", local_path, file_size, remote_path);

    esp_err_t result = ESP_FAIL;

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
            .timeout_ms = 60000,
        };

        esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
        if (!client) {
            ESP_LOGE(TAG, "Failed to init HTTP client");
            continue;
        }

        set_auth_header(client, cfg->user, cfg->pass);
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

        if (status == 201 || status == 200 || status == 204) {
            ESP_LOGI(TAG, "Upload success: %s", remote_path);
            result = ESP_OK;
            break;
        }

        ESP_LOGW(TAG, "Upload failed with HTTP %d, attempt %d/%d", status, attempt + 1, MAX_RETRIES);
    }

    fclose(f);
    free(url);
    return result;
}
