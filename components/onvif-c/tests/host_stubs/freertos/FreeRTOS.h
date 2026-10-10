/*
 * Host test stub: freertos/FreeRTOS.h — base types only.
 */

#ifndef STUB_FREERTOS_H
#define STUB_FREERTOS_H

typedef int          BaseType_t;
typedef unsigned int UBaseType_t;

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0

#define portMAX_DELAY     0xFFFFFFFFU
#define pdMS_TO_TICKS(ms) ((UBaseType_t)(ms))

#endif /* STUB_FREERTOS_H */
