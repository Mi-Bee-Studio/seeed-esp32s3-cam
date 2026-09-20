/*
 * MiBee Cam — ONVIF 板级适配层接口（onvif-c 组件接缝）
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ONVIF_PORT_H
#define ONVIF_PORT_H

#include "esp_err.h"

/** 启动 ONVIF（组件 SOAP 注册 + WS-Discovery；onvif_enable 门控在内）。
 * main.c 第 15 步 STA 连上后调用。 */
esp_err_t onvif_port_start(void);

#endif /* ONVIF_PORT_H */
