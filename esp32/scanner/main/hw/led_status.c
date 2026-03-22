/**
 * Friend or Foe -- Scanner Status LED Implementation
 *
 * Drives a single LED with configurable blink patterns.
 * A FreeRTOS task cycles through timing arrays to produce the
 * desired visual pattern.
 *
 * GPIO:
 *   ESP32-S3: GPIO48 (built-in RGB LED on DevKitC-1)
 *   ESP32-C5: GPIO27 (RGB LED pin on C5 dev boards)
 */

#include "led_status.h"
#include "task_priorities.h"

#include <stdatomic.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "led";

#if CONFIG_IDF_TARGET_ESP32S3
#define LED_GPIO    GPIO_NUM_48
#elif CONFIG_IDF_TARGET_ESP32C5
#define LED_GPIO    GPIO_NUM_27
#else
#define LED_GPIO    GPIO_NUM_48
#endif

/*
 * Pattern definitions: arrays of {on_ms, off_ms} pairs.
 * A pattern cycle repeats from the beginning after the last pair.
 * An entry with on_ms == 0 marks the end of the pattern.
 */
typedef struct {
    uint16_t on_ms;
    uint16_t off_ms;
} led_step_t;

/* LED_BOOT: 3 fast blinks then long off */
static const led_step_t pattern_boot[] = {
    { 100, 100 },
    { 100, 100 },
    { 100, 1500 },
    { 0, 0 },
};

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

/* LED_UART_OK: Single short pulse every 2s */
static const led_step_t pattern_uart_ok[] = {
    { 100, 1900 },
    { 0, 0 },
};

/* LED_ERROR: Triple blink (100 on, 100 off, x3, then 700 off) */
static const led_step_t pattern_error[] = {
    { 100, 100 },
    { 100, 100 },
    { 100, 700 },
    { 0, 0 },
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
