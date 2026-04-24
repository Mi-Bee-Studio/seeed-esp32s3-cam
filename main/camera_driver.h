#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/** Camera sensor types */
typedef enum {
    CAMERA_SENSOR_UNKNOWN = 0,
    CAMERA_SENSOR_OV2640,
    CAMERA_SENSOR_OV3660,
} camera_sensor_t;

/** Resolution options */
typedef enum {
    CAMERA_RES_VGA  = 0,   /* 640x480  */
    CAMERA_RES_SVGA = 1,   /* 800x600  */
    CAMERA_RES_XGA  = 2,   /* 1024x768 */
} camera_res_t;

/** Captured frame descriptor (buffer owned by esp_camera driver, in PSRAM). */
typedef struct {
    uint8_t *buf;
    size_t   len;
} camera_frame_t;

/**
 * Initialize the camera with given resolution, JPEG quality (0-63, lower=better).
 * Auto-detects sensor (OV2640 / OV3660).
 */
esp_err_t camera_init(camera_res_t res, uint8_t fps, uint8_t quality);

/** Return the detected sensor type. */
camera_sensor_t camera_get_sensor(void);

/** Capture a single JPEG frame. Caller must call camera_return_fb() when done. */
esp_err_t camera_capture(camera_frame_t *frame);

/** Return frame buffer to the driver (required after camera_capture). */
void camera_return_fb(camera_frame_t *frame);

/** Change resolution on the fly. */
esp_err_t camera_set_resolution(camera_res_t res);

/** Get current resolution setting. */
camera_res_t camera_get_resolution(void);

/** Human-readable resolution name. */
const char *camera_res_to_str(camera_res_t res);
