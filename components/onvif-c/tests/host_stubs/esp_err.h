/*
 * Host test stub: esp_err.h — minimal ESP-IDF error surface.
 * Values only need to be distinct; nothing compares them to IDF internals.
 */

#ifndef STUB_ESP_ERR_H
#define STUB_ESP_ERR_H

typedef int esp_err_t;

#define ESP_OK                       0
#define ESP_FAIL                     -1
#define ESP_ERR_NO_MEM               0x101
#define ESP_ERR_INVALID_ARG          0x102
#define ESP_ERR_INVALID_STATE        0x103
#define ESP_ERR_NOT_SUPPORTED        0x106
#define ESP_ERR_HTTPD_HANDLER_EXISTS 0x6604

const char *esp_err_to_name(esp_err_t err);

#endif /* STUB_ESP_ERR_H */
