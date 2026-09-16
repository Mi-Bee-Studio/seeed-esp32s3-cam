/*
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 录制/照片水印（issue #11）：sensor JPEG → 解码 RGB888 → 绘制
 * （自定义文案 + 实时时戳）→ 重编码 JPEG。
 *
 * 回退保证：watermark_apply() 失败（内存不足/超分辨率上限/编解码错）
 * 时调用方必须写原始帧——水印永不吞帧。契约默认全关（wm_enable=0），
 * 关闭态本模块零路径、零分配，与无此特性固件行为一致（回退要求）。 */

/** 照片水印是否启用（/api/capture 等单帧路径用） */
bool watermark_photo_enabled(void);

/** 视频轨水印是否启用（wm_enable && wm_video，sd_writer_task 用） */
bool watermark_video_enabled(void);

/**
 * @brief 对一帧 sensor JPEG 施加水印
 *
 * 内部复用持久缓冲（PSRAM，按首次帧分辨率惰性分配），线程安全（互斥）。
 * 输出指针指向模块内部缓冲，仅在下一次调用前有效，调用方不得释放。
 *
 * @param jpeg_in 原始 JPEG（不得为 NULL）
 * @param in_len  长度
 * @param jpeg_out [out] 成功=水印帧；失败=原样指回 jpeg_in（回退语义）
 * @param out_len [out] 对应长度
 * @return ESP_OK（含回退原帧情形时返回 ESP_FAIL，但 *jpeg_out 已指回原帧）
 */
esp_err_t watermark_apply(const uint8_t *jpeg_in, size_t in_len,
                          const uint8_t **jpeg_out, size_t *out_len);

#ifdef __cplusplus
}
#endif
