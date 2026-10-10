/*
 * Host test stub: freertos/task.h — pthread-backed tasks.
 *
 * vTaskDelete cannot kill a pthread; the discovery loop self-exits when it
 * sees its handle cleared, so stop ordering works out. vTaskDelay sleeps
 * at most 10 ms regardless of ticks to keep retry paths fast in tests.
 */

#ifndef STUB_TASK_H
#define STUB_TASK_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"

typedef struct onvif_fake_task *TaskHandle_t;
typedef void (*TaskFunction_t)(void *arg);

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                   UBaseType_t prio, TaskHandle_t *handle, BaseType_t core);
void       vTaskDelete(TaskHandle_t task);
void       vTaskDelay(UBaseType_t ticks);

#endif /* STUB_TASK_H */
