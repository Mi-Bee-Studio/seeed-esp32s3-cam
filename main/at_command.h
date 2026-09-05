/*
 * at_command.h — MiBee Cam 家族 AT 控制台核心（契约 v1.1）
 *
 * 核心文件（at_command.c / at_command.h / at_port.h）四仓 md5 一致；
 * 板差异见各仓 main/at_port.c（接口契约：at_port.h）。
 */
#ifndef AT_COMMAND_H
#define AT_COMMAND_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动家族 AT 控制台（初始化 port IO 后台 + 分发任务）
 *
 * 各板在 main.c 启动序列中调用一次；重复调用返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t at_command_init(void);

#ifdef __cplusplus
}
#endif

#endif /* AT_COMMAND_H */
