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

#include "flash_led.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "flash_led";

#define IR_LEDC_TIMER    LEDC_TIMER_1     /* TIMER_0 为 camera XCLK 占用（camera_driver.c） */
#define IR_LEDC_CHANNEL  LEDC_CHANNEL_1   /* 同上，CHANNEL_0 不可用 */
#define IR_LEDC_SPEED    LEDC_LOW_SPEED_MODE
#define IR_PWM_FREQ      2000
#define IR_PWM_RES       LEDC_TIMER_8_BIT
#define IR_PWM_MAX       255              /* 8-bit 满量程 */

static bool s_initialized = false;
static int  s_gpio        = 5;    /* config 键 ir_gpio（默认 5 = 丝印 D4） */
static int  s_brightness  = 0;    /* 0-100（0=灭） */

bool flash_led_gpio_allowed(int gpio)
{
    /* XIAO 边缘排针空闲脚 D0-D5（GPIO3 strapping 注脚见 flash_led.h） */
    return gpio >= 1 && gpio <= 6;
}

static void apply_duty(int duty)
{
    ledc_set_duty(IR_LEDC_SPEED, IR_LEDC_CHANNEL, duty);
    ledc_update_duty(IR_LEDC_SPEED, IR_LEDC_CHANNEL);
}

void flash_led_init(void)
{
    if (s_initialized) return;

    ledc_timer_config_t timer_conf = {
        .speed_mode      = IR_LEDC_SPEED,
        .duty_resolution = IR_PWM_RES,
        .timer_num       = IR_LEDC_TIMER,
        .freq_hz         = IR_PWM_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&timer_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed: %s", esp_err_to_name(ret));
        return;
    }

    ledc_channel_config_t ch_conf = {
        .gpio_num   = s_gpio,
        .speed_mode = IR_LEDC_SPEED,
        .channel    = IR_LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = IR_LEDC_TIMER,
        .duty       = 0,   /* 起步熄灭（day_night 判暗确认后点亮） */
        .hpoint     = 0,
    };
    ret = ledc_channel_config(&ch_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LEDC channel config failed: %s", esp_err_to_name(ret));
        return;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "IR illuminator ready (GPIO%d, %uHz %d-bit PWM)",
             s_gpio, IR_PWM_FREQ, 8);
}

void flash_led_set_gpio(int gpio)
{
    if (!flash_led_gpio_allowed(gpio)) {
        ESP_LOGW(TAG, "GPIO%d not in IR whitelist — ignored", gpio);
        return;
    }
    if (gpio == s_gpio) {
        flash_led_init();   /* 同脚：确保已初始化即可 */
        return;
    }
    flash_led_init();       /* 通道/定时器须先存在才能改脚 */
    if (!s_initialized) return;

    ledc_stop(IR_LEDC_SPEED, IR_LEDC_CHANNEL, 0);   /* idle=0：换脚瞬间不虚亮 */
    gpio_reset_pin((gpio_num_t)s_gpio);              /* 旧脚复位为浮空输入 */
    int prev = s_brightness;
    s_gpio = gpio;
    ledc_channel_config_t ch_conf = {
        .gpio_num   = s_gpio,
        .speed_mode = IR_LEDC_SPEED,
        .channel    = IR_LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = IR_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    if (ledc_channel_config(&ch_conf) != ESP_OK) {
        ESP_LOGE(TAG, "LEDC channel re-config to GPIO%d failed", gpio);
        return;
    }
    if (prev > 0) apply_duty(prev * IR_PWM_MAX / 100);   /* 恢复换脚前亮度 */
    ESP_LOGI(TAG, "IR illuminator moved to GPIO%d", gpio);
}

int flash_led_get_gpio(void)
{
    return s_gpio;
}

void flash_led_set_brightness(int brightness)
{
    if (brightness < 0)   brightness = 0;
    if (brightness > 100) brightness = 100;
    flash_led_init();
    if (!s_initialized) return;

    bool was_on = s_brightness > 0;
    apply_duty(brightness * IR_PWM_MAX / 100);
    s_brightness = brightness;
    if ((brightness > 0) != was_on) {
        ESP_LOGI(TAG, "IR %s (brightness=%d)", brightness ? "ON" : "OFF", brightness);
    }
}

int flash_led_get_brightness(void)
{
    return s_brightness;
}

bool flash_led_is_on(void)
{
    return s_brightness > 0;
}

void flash_led_on(void)
{
    flash_led_set_brightness(100);
}

void flash_led_off(void)
{
    flash_led_set_brightness(0);
}
