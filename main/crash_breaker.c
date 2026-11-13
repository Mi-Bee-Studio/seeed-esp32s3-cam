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

#include "crash_breaker.h"

#include "config_manager.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "crash_brk";

/* NVS 命名空间与键（全部 ≤15 字符，PIT-022 红线） */
#define BRK_NS       "cam_brk"
#define BRK_KEY_CNT  "cnt"    /* 连续异常复位计数 */
#define BRK_KEY_LIMP "limp"   /* 安全档生效标志 */
#define BRK_KEY_TRY  "try_lan" /* LAN 试运行标志（安全档下的每小时回归尝试） */
#define BRK_KEY_BAK  "bak"    /* 用户捕获配置已备份标志 */
#define BRK_KEY_B_FS "b_fs"
#define BRK_KEY_B_FPS "b_fps"
#define BRK_KEY_B_Q  "b_q"
#define BRK_KEY_B_DN "b_dn"
#define BRK_KEY_B_IRG "b_irg"
#define BRK_KEY_B_IRB "b_irb"

/* LAN 试运行捕获档：2026-11-10 晚间实测生产档（HD）与安全档（SVGA）在
 * LAN 负载下均已不可持续（劣化加深，见 PIT-062），试运行仍取 SVGA——
 * 但 AP-only 安全态是唯一长期稳定态，本档只是每小时的回归尝试 */
#define SAFE_FRAMESIZE 11   /* SVGA */
#define SAFE_FPS       12
#define SAFE_QUALITY   18
#define SAFE_DAY_NIGHT 0    /* 彩色（IR 不点灯） */
/* IR 熄灭语义由 SAFE_DAY_NIGHT=0 承载（彩色模式永不点灯）；档位取校验下限 1
 * 而非 0——契约 v2.3 规定 ir_brightness ∈1-100（0=拒绝，档位非开关），
 * 写 0 会让 config_save() 在整个 limp 期间 400，连诊断配置都存不进去（PIT-063） */
#define SAFE_IR_BRIGHT 1

#define PANIC_THRESHOLD      3
#define RESTORE_UPTIME_S     (30u * 60u)  /* LAN 试运行干净 30min → 全还原 */
#define LAN_RETRY_UPTIME_S   (60u * 60u)  /* AP-only 干净 60min → 试 LAN */

static bool s_limp    = false;
static bool s_lan_try = false;

static bool reason_is_panic(esp_reset_reason_t rr)
{
    return rr == ESP_RST_PANIC || rr == ESP_RST_TASK_WDT || rr == ESP_RST_BROWNOUT;
}

static void write_safe_capture_profile(cam_config_t *cfg)
{
    cfg->cam_framesize   = SAFE_FRAMESIZE;
    cfg->cam_fps         = SAFE_FPS;
    cfg->cam_quality     = SAFE_QUALITY;
    cfg->day_night_mode  = SAFE_DAY_NIGHT;
    cfg->ir_brightness   = SAFE_IR_BRIGHT;
    /* ir_gpio 保持用户值：彩色模式不点灯，换脚没有额外收益 */
    config_save();
}

bool crash_breaker_check_and_arm(void)
{
    esp_reset_reason_t rr = esp_reset_reason();

    nvs_handle_t h;
    if (nvs_open(BRK_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed — breaker disarmed");
        return false;
    }

    uint8_t cnt = 0, limp = 0, try_lan = 0, bak = 0;
    uint8_t b_fs = 0, b_fps = 0, b_q = 0, b_dn = 0, b_irg = 0, b_irb = 0;
    nvs_get_u8(h, BRK_KEY_CNT, &cnt);
    nvs_get_u8(h, BRK_KEY_LIMP, &limp);
    nvs_get_u8(h, BRK_KEY_TRY, &try_lan);
    nvs_get_u8(h, BRK_KEY_BAK, &bak);
    nvs_get_u8(h, BRK_KEY_B_FS, &b_fs);
    nvs_get_u8(h, BRK_KEY_B_FPS, &b_fps);
    nvs_get_u8(h, BRK_KEY_B_Q, &b_q);
    nvs_get_u8(h, BRK_KEY_B_DN, &b_dn);
    nvs_get_u8(h, BRK_KEY_B_IRG, &b_irg);
    nvs_get_u8(h, BRK_KEY_B_IRB, &b_irb);

    if (reason_is_panic(rr)) {
        if (cnt < 255) cnt++;
        if (cnt >= PANIC_THRESHOLD) {
            limp = 1;
            try_lan = 0;   /* 连续 panic：回安全态，LAN 试运行作废 */
        }
    } else if (rr == ESP_RST_POWERON || rr == ESP_RST_SW) {
        cnt = 0;   /* 正常/OTA 重启不清 limp/try_lan——状态机阶段延续 */
    }

    s_limp    = (limp == 1);
    s_lan_try = s_limp && try_lan == 1;

    if (s_limp) {
        cam_config_t *cfg = config_get();
        if (!bak) {
            /* 首次进入安全档：备份用户捕获配置 */
            b_fs  = cfg->cam_framesize;
            b_fps = cfg->cam_fps;
            b_q   = cfg->cam_quality;
            b_dn  = cfg->day_night_mode;
            b_irg = cfg->ir_gpio;
            b_irb = cfg->ir_brightness;
            bak   = 1;
        }
        write_safe_capture_profile(cfg);
        nvs_set_u8(h, BRK_KEY_CNT, cnt);
        nvs_set_u8(h, BRK_KEY_LIMP, limp);
        nvs_set_u8(h, BRK_KEY_TRY, try_lan);
        nvs_set_u8(h, BRK_KEY_BAK, bak);
        nvs_set_u8(h, BRK_KEY_B_FS, b_fs);
        nvs_set_u8(h, BRK_KEY_B_FPS, b_fps);
        nvs_set_u8(h, BRK_KEY_B_Q, b_q);
        nvs_set_u8(h, BRK_KEY_B_DN, b_dn);
        nvs_set_u8(h, BRK_KEY_B_IRG, b_irg);
        nvs_set_u8(h, BRK_KEY_B_IRB, b_irb);
        nvs_commit(h);
        if (s_lan_try) {
            ESP_LOGW(TAG, "SELF-HEAL: LAN-try boot (limp, panic_cnt=%u) — "
                     "SVGA + STA join as hourly regression attempt; 30min clean "
                     "→ restore user profile, any panic → back to AP-only", cnt);
        } else {
            ESP_LOGW(TAG, "SELF-HEAL: SAFE boot (limp, panic_cnt=%u) — AP-only "
                     "lifeline, no STA join; hourly LAN retry after %umin clean",
                     cnt, LAN_RETRY_UPTIME_S / 60);
        }
    } else {
        nvs_set_u8(h, BRK_KEY_CNT, cnt);
        nvs_commit(h);
        ESP_LOGI(TAG, "breaker armed: panic_cnt=%u (rst_reason=%d)", cnt, rr);
    }

    nvs_close(h);
    return s_limp;
}

void crash_breaker_recovery_poll(uint32_t uptime_s)
{
    if (!s_limp) {
        return;
    }

    nvs_handle_t h;
    if (nvs_open(BRK_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }

    if (!s_lan_try) {
        /* 安全态（AP-only）：干净 1 小时 → 试一次 LAN */
        if (uptime_s < LAN_RETRY_UPTIME_S) {
            nvs_close(h);
            return;
        }
        nvs_set_u8(h, BRK_KEY_TRY, 1);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGW(TAG, "SELF-HEAL: %umin clean AP-only uptime — rebooting into "
                 "LAN-try (SVGA + STA)", LAN_RETRY_UPTIME_S / 60);
        s_lan_try = true;
        esp_restart();
        return;
    }

    /* LAN 试运行：干净 30 分钟 → 还原用户捕获配置，回到全功能 */
    if (uptime_s < RESTORE_UPTIME_S) {
        nvs_close(h);
        return;
    }

    uint8_t bak = 0;
    nvs_get_u8(h, BRK_KEY_BAK, &bak);
    if (bak) {
        cam_config_t *cfg = config_get();
        uint8_t b_fs = SAFE_FRAMESIZE, b_fps = SAFE_FPS, b_q = SAFE_QUALITY;
        uint8_t b_dn = SAFE_DAY_NIGHT, b_irg = 0, b_irb = 0;
        nvs_get_u8(h, BRK_KEY_B_FS, &b_fs);
        nvs_get_u8(h, BRK_KEY_B_FPS, &b_fps);
        nvs_get_u8(h, BRK_KEY_B_Q, &b_q);
        nvs_get_u8(h, BRK_KEY_B_DN, &b_dn);
        nvs_get_u8(h, BRK_KEY_B_IRG, &b_irg);
        nvs_get_u8(h, BRK_KEY_B_IRB, &b_irb);
        cfg->cam_framesize   = b_fs;
        cfg->cam_fps         = b_fps;
        cfg->cam_quality     = b_q;
        cfg->day_night_mode  = b_dn;
        cfg->ir_gpio         = b_irg;
        cfg->ir_brightness   = b_irb;
        config_save();
        ESP_LOGW(TAG, "SELF-HEAL: 30min clean LAN uptime — restoring user capture "
                 "profile (fs=%u q=%u dn=%u ir=%u@%u) and rebooting",
                 b_fs, b_q, b_dn, b_irg, b_irb);
    }
    nvs_set_u8(h, BRK_KEY_LIMP, 0);
    nvs_set_u8(h, BRK_KEY_TRY, 0);
    nvs_set_u8(h, BRK_KEY_BAK, 0);
    nvs_set_u8(h, BRK_KEY_CNT, 0);
    nvs_commit(h);
    nvs_close(h);
    s_limp = false;
    s_lan_try = false;
    esp_restart();
}

void crash_breaker_reassert(void)
{
    if (!s_limp) {
        return;
    }
    cam_config_t *cfg = config_get();
    if (cfg->cam_framesize != SAFE_FRAMESIZE || cfg->day_night_mode != SAFE_DAY_NIGHT) {
        ESP_LOGW(TAG, "limp re-assert: external override touched capture profile — forcing safe values back");
        write_safe_capture_profile(cfg);
    }
}

bool crash_breaker_limp_active(void)
{
    return s_limp;
}

bool crash_breaker_lan_try_active(void)
{
    return s_lan_try;
}
