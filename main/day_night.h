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
#include <stdint.h>
#include "esp_err.h"

/* day_night_mode=2（自动）的实现（契约 §3.1 该键 2=自动由预留转实现）。
 *
 * 判定：帧广播器缓存帧（零拷贝）→ esp_jpeg 1/8 缩放解码（传感器无关——
 * 任何能出 JPEG 的头都一样）→ 平均 luma → 迟滞状态机 → 应用 grayscale
 * 特效切换黑白/彩色。传感器支持度运行时探测：set_special_effect 不支持
 * 则降级恒彩色并上报（换传感器自动适配，无型号白名单）。
 *
 * 仅 day_night_mode==2 时采样生效；0/1 为手动模式，本模块不干预
 * （退出自动模式时若曾施加过黑白会恢复彩色一次）。 */

typedef enum {
    DN_EFFECT_COLOR = 0,     /* 彩色（含自动模式判亮） */
    DN_EFFECT_BW    = 1,     /* 黑白（自动模式判暗） */
} dn_effective_t;

typedef struct {
    uint8_t       configured;    /* 配置的 day_night_mode（0/1/2） */
    dn_effective_t effective;    /* 当前实际生效色彩模式 */
    bool          bw_supported;  /* 传感器 grayscale 特效支持度（探测结果） */
    int           luma;          /* 最近一次采样平均亮度 0-255（-1=尚无） */
    uint32_t      samples;       /* 累计采样数 */
    uint32_t      switches;      /* 累计切换次数（抖动观测用） */
} day_night_state_t;

/** 启动采样任务（幂等；在 frame_broadcaster 启动后调用） */
esp_err_t day_night_start(void);

/** 当前状态快照（/api/camera 上报用） */
void day_night_get_state(day_night_state_t *out);
