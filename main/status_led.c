/*
 * Copyright (C) 2024 MiBeeHomeCam Authors
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

#include "status_led.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_log.h"

static const char *TAG = "status_led";

#define LED_GPIO        21
#define LED_ACTIVE_LOW  1   // 0 = ON, 1 = OFF

// Timer periods in ms
#define PERIOD_SLOW     1000
#define PERIOD_FAST     200
#define PERIOD_ERROR    200   // base tick for double-blink state machine

static led_status_t s_status = LED_STARTING;
static TimerHandle_t s_led_timer = NULL;
static uint8_t s_blink_step = 0;   // state machine counter for double blink
static bool s_disabled = false;    // disabled when GPIO21 is used for SD CS

/** 点亮 LED（设置 GPIO 为低电平，因为低电平有效） */
static void led_on(void)
{
    gpio_set_level(LED_GPIO, LED_ACTIVE_LOW ? 0 : 1);
}

/** 熄灭 LED（设置 GPIO 为高电平，因为低电平有效） */
static void led_off(void)
{
    gpio_set_level(LED_GPIO, LED_ACTIVE_LOW ? 1 : 0);
}

/**
 * @brief LED 定时器回调函数，由 FreeRTOS 软件定时器周期调用
 * 根据当前 LED 状态执行不同的闪烁逻辑：
 *   - AP_MODE: 简单翻转 GPIO 电平（慢闪）
 *   - WIFI_CONNECTING: 简单翻转 GPIO 电平（快闪）
 *   - ERROR: 双闪状态机，亮200ms灭200ms亮200ms灭1000ms
 * @param timer FreeRTOS 定时器句柄（未使用）
 */
static void led_timer_cb(TimerHandle_t timer)
{
    switch (s_status) {
    case LED_STARTING:
    case LED_RUNNING:
        // Timer should be stopped for these, but just in case
        break;

    case LED_AP_MODE:
        // Simple toggle at 1s period
        gpio_set_level(LED_GPIO, !gpio_get_level(LED_GPIO));
        break;

    case LED_WIFI_CONNECTING:
        // Simple toggle at 200ms period
        gpio_set_level(LED_GPIO, !gpio_get_level(LED_GPIO));
        break;

    case LED_ERROR:
        // Double blink: on 200ms, off 200ms, on 200ms, off 1000ms
        // Each timer tick = 200ms
        switch (s_blink_step) {
        case 0: led_on();  s_blink_step = 1; break;   // ON  (first blink)
        case 1: led_off(); s_blink_step = 2; break;   // OFF
        case 2: led_on();  s_blink_step = 3; break;   // ON  (second blink)
        case 3: led_off(); s_blink_step = 4; break;   // OFF start long gap
        default:
        case 4:
            s_blink_step++;
            if (s_blink_step >= 9) {
                s_blink_step = 0;   // ~1000ms gap (5 ticks * 200ms)
            }
            break;
        }
        break;
    }
}

/**
 * @brief 初始化状态 LED
 * 配置 GPIO21 为输出模式（低电平有效），创建 FreeRTOS 定时器
 * 启动时默认进入 LED_STARTING 状态（常亮）
 * @return ESP_OK 成功，ESP_FAIL GPIO 配置失败，ESP_ERR_NO_MEM 定时器创建失败
 */
esp_err_t led_init(void)
{
    // Configure GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(err));
        return err;
    }

    // Create software timer (not started yet)
    s_led_timer = xTimerCreate("led_tmr", pdMS_TO_TICKS(PERIOD_SLOW),
                               pdTRUE, NULL, led_timer_cb);
    if (!s_led_timer) {
        ESP_LOGE(TAG, "Failed to create LED timer");
        return ESP_ERR_NO_MEM;
    }

    // Start with LED on (starting state)
    s_status = LED_STARTING;
    led_on();
    ESP_LOGI(TAG, "Status LED initialized on GPIO%d (active LOW)", LED_GPIO);
    return ESP_OK;
}

/**
 * @brief 设置 LED 状态显示模式
 * 根据不同状态控制 LED 亮灭和闪烁模式，先停止定时器再按需重启
 * @param status 目标 LED 状态
 */
void led_set_status(led_status_t status)
{
    s_status = status;
    s_blink_step = 0;

    if (s_disabled || !s_led_timer) return;

    // Stop timer first
    xTimerStop(s_led_timer, 0);

    switch (status) {
    case LED_STARTING:
        led_on();
        // Timer stays stopped — solid on
        break;

    case LED_AP_MODE:
        led_on();
        xTimerChangePeriod(s_led_timer, pdMS_TO_TICKS(PERIOD_SLOW), 0);
        xTimerStart(s_led_timer, 0);
        break;

    case LED_WIFI_CONNECTING:
        led_on();
        xTimerChangePeriod(s_led_timer, pdMS_TO_TICKS(PERIOD_FAST), 0);
        xTimerStart(s_led_timer, 0);
        break;

    case LED_RUNNING:
        led_off();
        // Timer stays stopped — LED off
        break;

    case LED_ERROR:
        led_on();
        xTimerChangePeriod(s_led_timer, pdMS_TO_TICKS(PERIOD_ERROR), 0);
        xTimerStart(s_led_timer, 0);
        break;
    }

    ESP_LOGI(TAG, "LED status: %d%s", status, s_disabled ? " (disabled, SD CS active)" : "");
}

/** @brief 禁用LED控制，停止定时器，释放GPIO21给SD卡SPI CS */
void led_disable(void)
{
    s_disabled = true;
    if (s_led_timer) {
        xTimerStop(s_led_timer, portMAX_DELAY);
    }
    ESP_LOGI(TAG, "LED disabled (GPIO21 repurposed for SD card SPI CS)");
}
