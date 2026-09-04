/*
 * at_command.h — 家族统一 AT 指令面（seeed 板，USB-JTAG CDC 通道）
 * 契约：docs/at-command.md v1.0（2026-09-04）
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 AT 任务（USB_SERIAL_JTAG 驱动 + 读任务，Core 0） */
esp_err_t at_command_init(void);

#ifdef __cplusplus
}
#endif
