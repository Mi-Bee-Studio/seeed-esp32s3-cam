/*
 * Host test stub: freertos/semphr.h — pthread mutex.
 * xSemaphoreTake with 0 ticks is a trylock (drop-on-contention contract).
 */

#ifndef STUB_SEMPHR_H
#define STUB_SEMPHR_H

#include "freertos/FreeRTOS.h"

typedef struct onvif_fake_mutex *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t        xSemaphoreTake(SemaphoreHandle_t mtx, UBaseType_t ticks);
BaseType_t        xSemaphoreGive(SemaphoreHandle_t mtx);

#endif /* STUB_SEMPHR_H */
