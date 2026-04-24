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

static void led_on(void)
{
    gpio_set_level(LED_GPIO, LED_ACTIVE_LOW ? 0 : 1);
}

static void led_off(void)
{
    gpio_set_level(LED_GPIO, LED_ACTIVE_LOW ? 1 : 0);
}

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

void led_set_status(led_status_t status)
{
    s_status = status;
    s_blink_step = 0;

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

    ESP_LOGI(TAG, "LED status: %d", status);
}
