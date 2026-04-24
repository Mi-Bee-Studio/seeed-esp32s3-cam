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

#include "ftp_client.h"

#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

static const char *TAG = "ftp";

static int s_ctrl_sock = -1;
static bool s_connected = false;

/* ── Socket helpers ──────────────────────────────────────────────── */

/**
 * @brief 从socket读取FTP响应行
 * 逐字节读取直到\r\n，支持多行响应（NNN-前缀）
 * @param sock socket描述符
 * @param buf 响应缓冲区
 * @param buf_len 缓冲区大小
 * @param code 输出3位响应码（可选）
 * @return ESP_OK 成功，ESP_FAIL 读取失败
 */
static esp_err_t ftp_read_response(int sock, char *buf, size_t buf_len, int *code)
{
    size_t total = 0;

    while (total < buf_len - 1) {
        int n = recv(sock, buf + total, 1, 0);
        if (n <= 0) {
            ESP_LOGE(TAG, "recv failed (errno %d)", errno);
            return ESP_FAIL;
        }
        total += n;

        /* Complete line? Look for \r\n */
        if (total >= 2 && buf[total - 2] == '\r' && buf[total - 1] == '\n') {
            buf[total] = '\0';

            /* Multi-line response: line starts with "NNN-" – keep reading */
            if (total >= 4 && buf[3] == '-') {
                ESP_LOGD(TAG, "<%s", buf);
                total = 0;
                continue;
            }

            ESP_LOGI(TAG, "< %s", buf);

            /* Parse 3-digit code */
            if (total >= 3 && code) {
                *code = (buf[0] - '0') * 100 + (buf[1] - '0') * 10 + (buf[2] - '0');
            }
            return ESP_OK;
        }
    }

    buf[total] = '\0';
    ESP_LOGW(TAG, "response buffer full: %s", buf);
    return ESP_FAIL;
}

/**
 * @brief 通过socket发送FTP命令字符串
 * @param sock socket描述符
 * @param cmd 命令字符串（含\r\n）
 * @return ESP_OK 成功，ESP_FAIL 发送失败
 */
static esp_err_t ftp_send_cmd(int sock, const char *cmd)
{
    ESP_LOGI(TAG, "> %s", cmd);
    int n = send(sock, cmd, strlen(cmd), 0);
    if (n < 0) {
        ESP_LOGE(TAG, "send failed (errno %d)", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief 发送FTP命令并期望指定响应码
 * @param sock socket描述符
 * @param cmd 命令字符串
 * @param expected 期望的3位响应码
 * @param resp 响应缓冲区（可选，传NULL则使用内部缓冲）
 * @param resp_len 响应缓冲区大小
 * @return ESP_OK 响应码匹配，ESP_FAIL 发送或响应不匹配
 */
static esp_err_t ftp_cmd_expect(int sock, const char *cmd, int expected, char *resp, size_t resp_len)
{
    if (ftp_send_cmd(sock, cmd) != ESP_OK) {
        return ESP_FAIL;
    }
    int code = 0;
    if (ftp_read_response(sock, resp ? resp : (char[256]){0},
                          resp ? resp_len : 256, &code) != ESP_OK) {
        return ESP_FAIL;
    }
    if (code != expected) {
        ESP_LOGE(TAG, "expected %d, got %d", expected, code);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── Connect / disconnect ────────────────────────────────────────── */

/**
 * @brief 连接到FTP服务器并完成登录认证
 * 执行DNS解析→TCP连接→等待220欢迎→USER/PASS认证→TYPE I设置
 * @param cfg FTP连接配置
 * @return ESP_OK 成功，ESP_FAIL 连接或认证失败
 */
esp_err_t ftp_connect(const ftp_config_t *cfg)
{
    if (s_connected) {
        ftp_disconnect();
    }

    /* DNS resolve */
    struct hostent *he = gethostbyname(cfg->host);
    if (!he) {
        ESP_LOGE(TAG, "DNS lookup failed for %s", cfg->host);
        return ESP_FAIL;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(cfg->port),
    };
    memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));

    s_ctrl_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (s_ctrl_sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        return ESP_FAIL;
    }

    /* 10 s recv timeout */
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(s_ctrl_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(s_ctrl_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "connect to %s:%u failed (errno %d)", cfg->host, cfg->port, errno);
        close(s_ctrl_sock);
        s_ctrl_sock = -1;
        return ESP_FAIL;
    }

    char buf[256];
    int code = 0;

    /* 220 welcome */
    if (ftp_read_response(s_ctrl_sock, buf, sizeof(buf), &code) != ESP_OK || code != 220) {
        ESP_LOGE(TAG, "bad welcome (code %d)", code);
        goto fail;
    }

    /* USER → 331 */
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "USER %s\r\n", cfg->user);
    if (ftp_cmd_expect(s_ctrl_sock, cmd, 331, NULL, 0) != ESP_OK) {
        goto fail;
    }

    /* PASS → 230 */
    snprintf(cmd, sizeof(cmd), "PASS %s\r\n", cfg->pass);
    if (ftp_cmd_expect(s_ctrl_sock, cmd, 230, NULL, 0) != ESP_OK) {
        goto fail;
    }

    /* TYPE I → 200 */
    if (ftp_cmd_expect(s_ctrl_sock, "TYPE I\r\n", 200, NULL, 0) != ESP_OK) {
        goto fail;
    }

    s_connected = true;
    ESP_LOGI(TAG, "connected to %s:%u", cfg->host, cfg->port);
    return ESP_OK;

fail:
    close(s_ctrl_sock);
    s_ctrl_sock = -1;
    return ESP_FAIL;
}

/**
 * @brief 断开FTP连接
 * 发送QUIT命令，读取221响应，关闭控制socket
 */
void ftp_disconnect(void)
{
    if (s_ctrl_sock >= 0) {
        if (s_connected) {
            ftp_send_cmd(s_ctrl_sock, "QUIT\r\n");
            /* Read 221 response (ignore errors) */
            char buf[64];
            ftp_read_response(s_ctrl_sock, buf, sizeof(buf), NULL);
        }
        close(s_ctrl_sock);
        s_ctrl_sock = -1;
    }
    s_connected = false;
}

/**
 * @brief 检查FTP是否已连接
 * @return true 已连接，false 未连接
 */
bool ftp_is_connected(void)
{
    return s_connected;
}

/* ── PASV helper ─────────────────────────────────────────────────── */

/**
 * @brief 解析PASV响应中的数据连接IP和端口
 * 从227响应中提取 h1,h2,h3,h4,p1,p2 格式参数
 * @param resp PASV响应字符串
 * @param ip 输出数据连接IP地址
 * @param ip_len IP缓冲区大小
 * @param port 输出数据连接端口
 * @return ESP_OK 成功，ESP_FAIL 解析失败
 */
static esp_err_t ftp_parse_pasv(const char *resp, char *ip, size_t ip_len, uint16_t *port)
{
    const char *p = strchr(resp, '(');
    if (!p) {
        return ESP_FAIL;
    }
    p++; /* skip '(' */

    int h1, h2, h3, h4, p1, p2;
    if (sscanf(p, "%d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2) != 6) {
        ESP_LOGE(TAG, "PASV parse failed: %s", resp);
        return ESP_FAIL;
    }

    snprintf(ip, ip_len, "%d.%d.%d.%d", h1, h2, h3, h4);
    *port = (uint16_t)(p1 * 256 + p2);
    return ESP_OK;
}

/* ── Upload ──────────────────────────────────────────────────────── */

/**
 * @brief 通过FTP上传本地文件到远程路径
 * 使用PASV被动模式建立数据连接，以4KB分块传输文件内容
 * @param remote_path 远程文件路径
 * @param local_path 本地文件路径
 * @return ESP_OK 成功，ESP_FAIL 上传失败（失败时自动断开连接）
 */
esp_err_t ftp_upload(const char *remote_path, const char *local_path)
{
    if (!s_connected) {
        ESP_LOGE(TAG, "not connected");
        return ESP_FAIL;
    }

    /* Open local file */
    FILE *fp = fopen(local_path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "cannot open %s", local_path);
        return ESP_FAIL;
    }

    /* Get file size */
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char buf[512];
    esp_err_t ret = ESP_FAIL;

    /* PASV → 227 */
    if (ftp_cmd_expect(s_ctrl_sock, "PASV\r\n", 227, buf, sizeof(buf)) != ESP_OK) {
        goto cleanup;
    }

    /* Parse data endpoint */
    char data_ip[48];
    uint16_t data_port;
    if (ftp_parse_pasv(buf, data_ip, sizeof(data_ip), &data_port) != ESP_OK) {
        goto cleanup;
    }

    /* Connect data socket */
    int data_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (data_sock < 0) {
        goto cleanup;
    }

    struct sockaddr_in daddr = {
        .sin_family = AF_INET,
        .sin_port   = htons(data_port),
        .sin_addr.s_addr = inet_addr(data_ip),
    };

    if (connect(data_sock, (struct sockaddr *)&daddr, sizeof(daddr)) != 0) {
        ESP_LOGE(TAG, "data connect to %s:%u failed", data_ip, data_port);
        close(data_sock);
        goto cleanup;
    }

    /* STOR on control channel */
    {
        char cmd[280];
        snprintf(cmd, sizeof(cmd), "STOR %s\r\n", remote_path);
        if (ftp_cmd_expect(s_ctrl_sock, cmd, 150, NULL, 0) != ESP_OK) {
            close(data_sock);
            goto cleanup;
        }
    }

    /* Transfer data in 4 KB chunks */
    {
        uint8_t *chunk = malloc(4096);
        if (!chunk) {
            close(data_sock);
            goto cleanup;
        }

        size_t total_sent = 0;
        while (total_sent < (size_t)file_size) {
            size_t to_read = 4096;
            if (to_read > (size_t)file_size - total_sent) {
                to_read = (size_t)file_size - total_sent;
            }
            size_t nread = fread(chunk, 1, to_read, fp);
            if (nread == 0) {
                break;
            }

            int nsent = send(data_sock, chunk, nread, 0);
            if (nsent < 0) {
                ESP_LOGE(TAG, "data send failed (errno %d)", errno);
                free(chunk);
                close(data_sock);
                goto cleanup;
            }
            total_sent += nsent;
        }

        free(chunk);
    }

    close(data_sock);

    /* Expect 226 transfer complete */
    {
        int code = 0;
        ftp_read_response(s_ctrl_sock, buf, sizeof(buf), &code);
        if (code != 226) {
            ESP_LOGW(TAG, "expected 226, got %d", code);
        }
    }

    ESP_LOGI(TAG, "uploaded %s (%ld bytes)", remote_path, file_size);
    ret = ESP_OK;

cleanup:
    fclose(fp);
    if (ret != ESP_OK) {
        ftp_disconnect();
    }
    return ret;
}

/* ── Recursive MKD ───────────────────────────────────────────────── */

/**
 * @brief 递归创建FTP远程目录
 * 按路径层级逐级发送MKD命令，257=创建成功，550=已存在（均视为成功）
 * @param path 远程目录路径
 * @return ESP_OK 成功
 */
esp_err_t ftp_mkdir_recursive(const char *path)
{
    if (!s_connected) {
        ESP_LOGE(TAG, "not connected");
        return ESP_FAIL;
    }

    char dir[256];
    char cmd[280];

    /* Walk path components: "/a/b/c" → "/a", "/a/b", "/a/b/c" */
    for (const char *p = path; *p; ) {
        if (*p == '/') {
            p++;
            continue;
        }

        /* Find next '/' or end */
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - path) : strlen(path);
        if (len >= sizeof(dir)) {
            break;
        }
        memcpy(dir, path, len);
        dir[len] = '\0';

        snprintf(cmd, sizeof(cmd), "MKD %s\r\n", dir);
        ftp_send_cmd(s_ctrl_sock, cmd);

        int code = 0;
        char buf[256];
        if (ftp_read_response(s_ctrl_sock, buf, sizeof(buf), &code) == ESP_OK) {
            if (code == 257) {
                ESP_LOGI(TAG, "created dir %s", dir);
            } else if (code == 550) {
                ESP_LOGD(TAG, "dir exists %s", dir);
            } else {
                ESP_LOGW(TAG, "MKD %s returned %d", dir, code);
            }
        }

        p = slash ? slash + 1 : path + strlen(path);
    }

    return ESP_OK;
}
