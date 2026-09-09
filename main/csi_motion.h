/*
 * MiBee Cam — ESPectre WiFi CSI motion sensing (optional pilot module)
 *
 * Copyright (C) 2026 MiBee Cam Authors
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

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start the ESPectre sensing runtime (single-owner task). No-op unless
 * CONFIG_MIBEE_CSI_MOTION is set. Must be called after wifi_init() — the
 * runtime hooks WIFI_EVENT/IP_EVENT itself and handles the already-connected
 * case. Pilot scope: motion events go to the log only (no API/contract yet).
 */
esp_err_t csi_motion_init(void);

/* 契约 v1.6：最新感知快照（GET /api/status 的 "csi" 字段，与 /ws csi_status
 * 心跳同形同值）。CSI 未编译或运行时尚未产出首个周期更新时返回 false。
 * portMUX 单写者快照，非阻塞，可在任意任务上下文调用。 */
typedef struct {
    char  state[10];   /* "MOTION" / "IDLE" / "warming" */
    float score;       /* 0-1 精细分值 */
    float thr;         /* 0-1 判决阈值 */
} csi_motion_status_t;

bool csi_motion_get_status(csi_motion_status_t *out);

#ifdef __cplusplus
}
#endif
