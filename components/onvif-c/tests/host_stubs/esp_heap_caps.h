/*
 * Host test stub: esp_heap_caps.h — no PSRAM on host, plain malloc.
 */

#ifndef STUB_ESP_HEAP_CAPS_H
#define STUB_ESP_HEAP_CAPS_H

#include <stddef.h>

#define MALLOC_CAP_SPIRAM 2

void *heap_caps_malloc(size_t size, unsigned int caps);

#endif /* STUB_ESP_HEAP_CAPS_H */
