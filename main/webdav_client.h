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

#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief WebDAV连接配置结构体
 */
typedef struct {
    char url[128];     /* WebDAV服务器地址，如 "http://192.168.1.100:5005" */
    char user[32];     /* 登录用户名 */
    char pass[32];     /* 登录密码 */
} webdav_config_t;

/**
 * @brief 通过HTTP PUT上传本地文件到WebDAV远程路径
 * 包含指数退避重试机制（最多3次）
 * @param cfg WebDAV连接配置
 * @param remote_path 远程文件路径
 * @param local_path 本地文件路径
 * @return ESP_OK 成功，ESP_FAIL 上传失败
 */
esp_err_t webdav_upload(const webdav_config_t *cfg, const char *remote_path, const char *local_path);

/**
 * @brief 通过WebDAV MKCOL创建远程目录
 * 201=创建成功，405=已存在，均返回ESP_OK
 * @param cfg WebDAV连接配置
 * @param remote_dir 远程目录路径
 * @return ESP_OK 创建成功或已存在，ESP_FAIL 失败
 */
esp_err_t webdav_mkdir(const webdav_config_t *cfg, const char *remote_dir);

/**
 * @brief 通过HTTP HEAD检查远程资源是否存在
 * @param cfg WebDAV连接配置
 * @param remote_path 远程资源路径
 * @return ESP_OK 存在(200)，ESP_ERR_NOT_FOUND 不存在(404)，ESP_FAIL 其他错误
 */
esp_err_t webdav_exists(const webdav_config_t *cfg, const char *remote_path);

/**
 * @brief 递归创建远程路径中的所有目录层级
 * 如 "/ParrotCam/2026-04/24" 会依次创建 /ParrotCam、/ParrotCam/2026-04、/ParrotCam/2026-04/24
 * @param cfg WebDAV连接配置
 * @param path 远程目录路径
 * @return ESP_OK 全部成功，或最后一个失败目录的错误码
 */
esp_err_t webdav_mkdir_recursive(const webdav_config_t *cfg, const char *path);
