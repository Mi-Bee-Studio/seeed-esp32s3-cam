/*
 * Host test stub: esp_task_wdt.h — counting fake for the port layer.
 */

#ifndef STUB_ESP_TASK_WDT_H
#define STUB_ESP_TASK_WDT_H

#include <stdbool.h>
#include "esp_err.h"

/* Counters the tests assert on (implemented in onvif_fake.c). */
extern int onvif_fake_wdt_added;
extern int onvif_fake_wdt_feeds;
extern int onvif_fake_wdt_deleted;

esp_err_t esp_task_wdt_add(void *task_handle);
esp_err_t esp_task_wdt_reset(void);
esp_err_t esp_task_wdt_delete(void *task_handle);

#endif /* STUB_ESP_TASK_WDT_H */
