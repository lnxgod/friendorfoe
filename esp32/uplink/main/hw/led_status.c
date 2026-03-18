/**
 * Friend or Foe -- Uplink Status LED Implementation
 *
 * Drives a single LED on GPIO8 with configurable blink patterns.
 * A FreeRTOS task cycles through timing arrays to produce the
 * desired visual pattern.
 */

#include "led_status.h"
#include "config.h"

#include <stdatomic.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "led";

#define LED_GPIO    GPIO_NUM_8

/*
 * Pattern definitions: arrays of {on_ms, off_ms} pairs.
 * A pattern cycle repeats from the beginning after the last pair.
 * An entry with on_ms == 0 marks the end of the pattern.
 */
typedef struct {
    uint16_t on_ms;
    uint16_t off_ms;
} led_step_t;

/* LED_IDLE: Slow blink 1Hz (500ms on, 500ms off) */
static const led_step_t pattern_idle[] = {
    { 500, 500 },
    { 0, 0 },
};

/* LED_SCANNING: Fast blink 4Hz (125ms on, 125ms off) */
static const led_step_t pattern_scanning[] = {
    { 125, 125 },
    { 0, 0 },
};

/* LED_DETECTION: Solid on for 2s, then off briefly */
static const led_step_t pattern_detection[] = {
    { 2000, 100 },
    { 0, 0 },
};

/* LED_UPLOADING: Double blink (100 on, 100 off, 100 on, 700 off) */
static const led_step_t pattern_uploading[] = {
    { 100, 100 },
    { 100, 700 },
    { 0, 0 },
};

/* LED_ERROR: Triple blink (100 on, 100 off, x3, then 700 off) */
static const led_step_t pattern_error[] = {
    { 100, 100 },
    { 100, 100 },
    { 100, 700 },
    { 0, 0 },
};

/* LED_NO_GPS: Very slow blink 0.5Hz (1000ms on, 1000ms off) */
static const led_step_t pattern_no_gps[] = {
    { 1000, 1000 },
    { 0, 0 },
};

static const led_step_t *const s_patterns[] = {
    [LED_IDLE]      = pattern_idle,
    [LED_SCANNING]  = pattern_scanning,
    [LED_DETECTION] = pattern_detection,
    [LED_UPLOADING] = pattern_uploading,
    [LED_ERROR]     = pattern_error,
    [LED_NO_GPS]    = pattern_no_gps,
};

static atomic_int s_current_pattern = LED_IDLE;

/* ── LED task ──────────────────────────────────────────────────────────── */

static void led_task(void *arg)
{
    ESP_LOGI(TAG, "LED task started");

    while (1) {
        led_pattern_t pat = (led_pattern_t)atomic_load(&s_current_pattern);
        const led_step_t *steps = s_patterns[pat];
        int step = 0;

        while (1) {
            /* Check if pattern changed */
            led_pattern_t current = (led_pattern_t)atomic_load(&s_current_pattern);
            if (current != pat) {
                break; /* Restart with new pattern */
            }

            /* End of pattern array -> loop back */
            if (steps[step].on_ms == 0 && steps[step].off_ms == 0) {
                step = 0;
                continue;
            }

            /* ON phase */
            gpio_set_level(LED_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(steps[step].on_ms));

            /* Check again before OFF phase */
            current = (led_pattern_t)atomic_load(&s_current_pattern);
            if (current != pat) {
                gpio_set_level(LED_GPIO, 0);
                break;
            }

            /* OFF phase */
            gpio_set_level(LED_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(steps[step].off_ms));

            step++;
        }
    }
}

/* ── Public API ────────────────────────────────────────────────────────── */

void led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    gpio_set_level(LED_GPIO, 0);

    ESP_LOGI(TAG, "LED initialized (GPIO%d)", LED_GPIO);
}

void led_set_pattern(led_pattern_t pattern)
{
    atomic_store(&s_current_pattern, (int)pattern);
}

void led_start(void)
{
    xTaskCreate(led_task, "led", CONFIG_LED_STACK,
                NULL, CONFIG_LED_PRIORITY, NULL);
    ESP_LOGI(TAG, "LED task created (priority=%d, stack=%d)",
             CONFIG_LED_PRIORITY, CONFIG_LED_STACK);
}
