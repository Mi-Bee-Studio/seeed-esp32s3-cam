/*
 * MiBee Cam — 家族共享任务看门狗核心（四仓 md5 锁文件）
 *
 * Copyright (C) 2024-2026 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * 实现说明：注册表只服务观测面（名字/喂狗计数），TWDT 的判定完全由
 * ESP-IDF 承担。feeds 为宽松计数（多任务并发递增，诊断用途，非精确）。
 */

#include "watchdog.h"

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define WATCHDOG_MAX_TASKS 8

static const char *TAG = "watchdog";

typedef struct {
    char         name[16];
    TaskHandle_t handle;
    uint32_t     feeds;
} watchdog_slot_t;

static watchdog_slot_t s_slots[WATCHDOG_MAX_TASKS];

void watchdog_init(void)
{
    memset(s_slots, 0, sizeof(s_slots));
#ifdef CONFIG_ESP_TASK_WDT
    ESP_LOGI(TAG, "task watchdog armed: timeout=%ds panic=%s",
             (int)CONFIG_ESP_TASK_WDT_TIMEOUT_S,
#ifdef CONFIG_ESP_TASK_WDT_PANIC
             "yes (hang -> reset + backtrace)");
#else
             "NO (hang only logged — set CONFIG_ESP_TASK_WDT_PANIC for reset)");
#endif
#else
    ESP_LOGW(TAG, "CONFIG_ESP_TASK_WDT not set — watchdog capability inactive");
#endif
}

esp_err_t watchdog_register_current(const char *name)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < WATCHDOG_MAX_TASKS; i++) {
        if (s_slots[i].handle == self) {
            return ESP_OK; /* idempotent */
        }
    }
    for (int i = 0; i < WATCHDOG_MAX_TASKS; i++) {
        if (s_slots[i].handle == NULL) {
            esp_err_t err = esp_task_wdt_add(NULL);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "'%s' TWDT subscribe failed: %s", name,
                         esp_err_to_name(err));
                return err;
            }
            strlcpy(s_slots[i].name, name ? name : "?", sizeof(s_slots[i].name));
            s_slots[i].handle = self;
            s_slots[i].feeds = 0;
            ESP_LOGI(TAG, "watching '%s'", s_slots[i].name);
            return ESP_OK;
        }
    }
    ESP_LOGW(TAG, "registry full — '%s' unwatched", name);
    return ESP_ERR_NO_MEM;
}

void watchdog_feed_current(void)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < WATCHDOG_MAX_TASKS; i++) {
        if (s_slots[i].handle == self) {
            esp_task_wdt_reset();
            s_slots[i].feeds++;
            return;
        }
    }
    /* 未注册的任务不喂：喂了未订阅的任务是空转，掩盖注册遗漏。 */
}

void watchdog_attach_status(cJSON *data)
{
    if (!data) {
        return;
    }
    cJSON *wdt = cJSON_CreateObject();
    if (!wdt) {
        return;
    }
#ifdef CONFIG_ESP_TASK_WDT
    cJSON_AddBoolToObject(wdt, "enabled", true);
    cJSON_AddNumberToObject(wdt, "timeout_s", CONFIG_ESP_TASK_WDT_TIMEOUT_S);
    cJSON_AddBoolToObject(wdt, "panic",
#ifdef CONFIG_ESP_TASK_WDT_PANIC
                          true);
#else
                          false);
#endif
#else
    cJSON_AddBoolToObject(wdt, "enabled", false);
#endif
    cJSON *tasks = cJSON_CreateArray();
    if (tasks) {
        for (int i = 0; i < WATCHDOG_MAX_TASKS; i++) {
            if (s_slots[i].handle == NULL) {
                continue;
            }
            cJSON *t = cJSON_CreateObject();
            if (!t) {
                break;
            }
            cJSON_AddStringToObject(t, "name", s_slots[i].name);
            cJSON_AddNumberToObject(t, "feeds", (double)s_slots[i].feeds);
            cJSON_AddItemToArray(tasks, t);
        }
        cJSON_AddItemToObject(wdt, "tasks", tasks);
    }
    cJSON_AddItemToObject(data, "wdt", wdt);
}
