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
#include <stdint.h>

/**
 * @brief FTP连接配置结构体
 */
typedef struct {
    char host[64];     /* FTP服务器地址 */
    uint16_t port;     /* FTP服务器端口 */
    char user[32];     /* 登录用户名 */
    char pass[32];     /* 登录密码 */
} ftp_config_t;

/**
 * @brief 连接到FTP服务器
 * 执行DNS解析、TCP连接、登录认证，设置二进制传输模式
 * @param cfg FTP连接配置
 * @return ESP_OK 成功，ESP_FAIL 连接或认证失败
 */
esp_err_t ftp_connect(const ftp_config_t *cfg);
/**
 * @brief 通过FTP上传本地文件到远程路径
 * 使用PASV模式建立数据连接，分块传输
 * @param remote_path 远程文件路径
 * @param local_path 本地文件路径
 * @return ESP_OK 成功，ESP_FAIL 上传失败
 */
esp_err_t ftp_upload(const char *remote_path, const char *local_path);
/**
 * @brief 递归创建FTP远程目录
 * 按路径层级逐级发送MKD命令，已存在的目录会被忽略
 * @param path 远程目录路径
 * @return ESP_OK 成功
 */
esp_err_t ftp_mkdir_recursive(const char *path);
/**
 * @brief 断开FTP连接
 * 发送QUIT命令并关闭控制socket
 */
void ftp_disconnect(void);
/**
 * @brief 检查FTP是否已连接
 * @return true 已连接，false 未连接
 */
bool ftp_is_connected(void);
