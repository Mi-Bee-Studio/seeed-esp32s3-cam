/*
 * Host test stub: esp_log.h — logging compiled away, but format ARGUMENTS
 * are still evaluated (inside a dead branch) so log-only variables stay
 * "used" under -Werror, exactly as in ESP-IDF builds.
 */

#ifndef STUB_ESP_LOG_H
#define STUB_ESP_LOG_H

#include <stdio.h>

#define ONVIF_STUB_LOG(tag, fmt, ...)                                                              \
    do {                                                                                           \
        (void)(tag);                                                                               \
        if (0) {                                                                                   \
            printf("%s: " fmt "\n", #tag, ##__VA_ARGS__);                                          \
        }                                                                                          \
    } while (0)

#define ESP_LOGE(tag, ...) ONVIF_STUB_LOG(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) ONVIF_STUB_LOG(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) ONVIF_STUB_LOG(tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) ONVIF_STUB_LOG(tag, __VA_ARGS__)
#define ESP_LOGV(tag, ...) ONVIF_STUB_LOG(tag, __VA_ARGS__)

#endif /* STUB_ESP_LOG_H */
