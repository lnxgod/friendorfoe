/**
 * Friend or Foe -- BLE Scanner Status LED Implementation
 *
 * Drives an addressable WS2812 RGB LED via the RMT-based led_strip driver.
 *
 * GPIO:
 *   ESP32-S3: GPIO48 (built-in RGB LED on DevKitC-1)
 *   ESP32-C5: GPIO27 (RGB LED pin on C5 dev boards)
 *   ESP32-C3: GPIO8  (built-in RGB LED on DevKitM-1)
 */

#include "led_status.h"
#include "core/task_priorities.h"

#include <stdatomic.h>
#include "led_strip.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "led";

#if CONFIG_IDF_TARGET_ESP32S3
#define LED_GPIO    48
#elif CONFIG_IDF_TARGET_ESP32C5
#define LED_GPIO    27
#elif CONFIG_IDF_TARGET_ESP32C3
#define LED_GPIO    8
#else
#define LED_GPIO    48
#endif

/* ── RGB colour for each pattern ──────────────────────────────────────── */

typedef struct {
    uint8_t r, g, b;
} rgb_t;

static const rgb_t s_colours[] = {
    [LED_BOOT]      = {  0,   0,  30 },   /* blue   */
    [LED_IDLE]      = {  0,  15,   0 },   /* dim green */
    [LED_SCANNING]  = {  0,  30,   0 },   /* green  */
    [LED_DETECTION] = { 30,   0,   0 },   /* red    */
    [LED_UART_OK]   = {  0,  15,  15 },   /* cyan   */
    [LED_ERROR]     = { 30,   0,   0 },   /* red    */
};

/* ── Pattern timing ───────────────────────────────────────────────────── */

typedef struct {
    uint16_t on_ms;
    uint16_t off_ms;
} led_step_t;

static const led_step_t pattern_boot[] = {
    { 100, 100 }, { 100, 100 }, { 100, 1500 }, { 0, 0 },
};
static const led_step_t pattern_idle[] = {
    { 500, 500 }, { 0, 0 },
};
static const led_step_t pattern_scanning[] = {
    { 125, 125 }, { 0, 0 },
};
static const led_step_t pattern_detection[] = {
    { 2000, 100 }, { 0, 0 },
};
static const led_step_t pattern_uart_ok[] = {
    { 100, 1900 }, { 0, 0 },
};
static const led_step_t pattern_error[] = {
    { 100, 100 }, { 100, 100 }, { 100, 700 }, { 0, 0 },
};

static const led_step_t *const s_patterns[] = {
    [LED_BOOT]      = pattern_boot,
    [LED_IDLE]      = pattern_idle,
    [LED_SCANNING]  = pattern_scanning,
    [LED_DETECTION] = pattern_detection,
    [LED_UART_OK]   = pattern_uart_ok,
    [LED_ERROR]     = pattern_error,
};

static atomic_int s_current_pattern = LED_IDLE;
static led_strip_handle_t s_strip = NULL;

/* ── Helpers ──────────────────────────────────────────────────────────── */

static void led_on(led_pattern_t pat)
{
    if (!s_strip) return;
    const rgb_t *c = &s_colours[pat];
    led_strip_set_pixel(s_strip, 0, c->r, c->g, c->b);
    led_strip_refresh(s_strip);
}

static void led_off(void)
{
    if (!s_strip) return;
    led_strip_clear(s_strip);
}

/* ── LED task ──────────────────────────────────────────────────────────── */

static void led_task(void *arg)
{
    ESP_LOGI(TAG, "LED task started");

    while (1) {
        led_pattern_t pat = (led_pattern_t)atomic_load(&s_current_pattern);
        const led_step_t *steps = s_patterns[pat];
        int step = 0;

        while (1) {
            led_pattern_t current = (led_pattern_t)atomic_load(&s_current_pattern);
            if (current != pat) {
                break;
            }

            if (steps[step].on_ms == 0 && steps[step].off_ms == 0) {
                step = 0;
                continue;
            }

            /* ON phase */
            led_on(pat);
            vTaskDelay(pdMS_TO_TICKS(steps[step].on_ms));

            /* Check again before OFF phase */
            current = (led_pattern_t)atomic_load(&s_current_pattern);
            if (current != pat) {
                led_off();
                break;
            }

            /* OFF phase */
            led_off();
            vTaskDelay(pdMS_TO_TICKS(steps[step].off_ms));

            step++;
        }
    }
}

/* ── Public API ────────────────────────────────────────────────────────── */

void led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,  /* 10 MHz */
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED strip init failed: %s", esp_err_to_name(err));
        return;
    }

    led_off();
    ESP_LOGI(TAG, "LED initialized (WS2812 on GPIO%d)", LED_GPIO);
}

void led_set_pattern(led_pattern_t pattern)
{
    atomic_store(&s_current_pattern, (int)pattern);
}

void led_start(void)
{
#ifdef CONFIG_FREERTOS_UNICORE
    xTaskCreate(led_task, "led", LED_TASK_STACK_SIZE,
                NULL, LED_TASK_PRIORITY, NULL);
#else
    xTaskCreatePinnedToCore(led_task, "led", LED_TASK_STACK_SIZE,
                            NULL, LED_TASK_PRIORITY, NULL, CORE_PROCESSING);
#endif
    ESP_LOGI(TAG, "LED task created (priority=%d, stack=%d)",
             LED_TASK_PRIORITY, LED_TASK_STACK_SIZE);
}
