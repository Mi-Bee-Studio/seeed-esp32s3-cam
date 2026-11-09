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

/* IR 补光灯（契约 /api/led + flash_led 能力位，2026-11-09 上线）。
 *
 * 硬件：外接 2×红外灯珠（220Ω 限流，owner 手工接线，默认 GPIO5=丝印 D4）。
 * 驱动脚经契约 config 键 `ir_gpio` 可配（白名单见 flash_led_gpio_allowed，
 * Web 相机页下拉热生效）。LEDC PWM 驱动，TIMER_1/CHANNEL_1——TIMER_0/
 * CHANNEL_0 被 camera XCLK 占用（同 ai-thinker flash_led.c 的通道布局）。
 * brightness 0-100 线性映射占空比 0-100%（红外补光无调光需求，满档=最大电流）。
 *
 * 归属语义（与 day_night.c mode=2 联动）：
 *   - day_night_mode==2：自动引擎每采样周期重申（判暗=亮、判亮=灭），
 *     手动 /api/led 的改动存活 ≤1 个采样周期（10s）；
 *   - day_night_mode 0/1：LED 完全归 /api/led 手动控制（volatile，重启=灭）。 */

/** 幂等初始化（LEDC 配置，duty=0 起步；boot 期调用保证灯脚不悬空发虚亮） */
void flash_led_init(void);

/** 换驱动脚（config 键 ir_gpio，白名单见 flash_led_gpio_allowed）。
 *  运行中调用 = 停 PWM → 旧脚复位浮空 → 新脚接管并恢复先前亮度（热生效） */
void flash_led_set_gpio(int gpio);

/** 当前驱动脚号 */
int flash_led_get_gpio(void);

/** 允许的 IR 脚白名单：XIAO 边缘排针空闲脚（GPIO 1/2/4/5/6）。
 *  排除：GPIO3=strapping、7/8/9/21=SD、43/44=UART0、10-18/38-40/47/48=相机、
 *  41/42=麦克风、19/20=USB、26-37=flash/Octal PSRAM——误配会撞车甚至砖化 */
bool flash_led_gpio_allowed(int gpio);

/** 设置亮度 0-100（0=灭；钳制越界值） */
void flash_led_set_brightness(int brightness);

/** 当前亮度 0-100 */
int flash_led_get_brightness(void);

/** 灯是否点亮（brightness > 0） */
bool flash_led_is_on(void);

/** 满档点亮（等价 set_brightness(100)，day_night 联动用） */
void flash_led_on(void);

/** 熄灭（等价 set_brightness(0)） */
void flash_led_off(void);
