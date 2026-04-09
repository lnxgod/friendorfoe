/**
 * Friend or Foe -- Uplink Status LED Implementation
 *
 * ESP32-S3: WS2812 RGB LED on GPIO48 with colour-coded status.
 * ESP32-C3/ESP32: Plain GPIO LED with blink patterns (fallback).
 */

#include "led_status.h"
#include "config.h"

#include <stdatomic.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "led";

static atomic_int s_current_pattern = LED_IDLE;

/* ── Pattern timing ──────────────────────────────────────────────────── */

typedef struct { uint16_t on_ms; uint16_t off_ms; } led_step_t;

static const led_step_t pat_idle[]       = { {500, 500}, {0,0} };
static const led_step_t pat_scanning[]   = { {125, 125}, {0,0} };
static const led_step_t pat_detection[]  = { {2000, 100}, {0,0} };
static const led_step_t pat_uploading[]  = { {100,100}, {100,700}, {0,0} };
static const led_step_t pat_error[]      = { {100,100}, {100,100}, {100,700}, {0,0} };
static const led_step_t pat_no_gps[]     = { {1000, 1000}, {0,0} };
static const led_step_t pat_no_scanner[] = { {300, 300}, {0,0} };
static const led_step_t pat_all_good[]   = { {2000, 100}, {0,0} };      /* mostly on */
static const led_step_t pat_no_server[]  = { {800, 200}, {0,0} };       /* slow pulse */
static const led_step_t pat_wifi_down[]  = { {200, 200}, {200, 400}, {0,0} }; /* fast alt */

static const led_step_t *const s_patterns[] = {
    [LED_IDLE]        = pat_idle,
    [LED_SCANNING]    = pat_scanning,
    [LED_DETECTION]   = pat_detection,
    [LED_UPLOADING]   = pat_uploading,
    [LED_ERROR]       = pat_error,
    [LED_NO_GPS]      = pat_no_gps,
    [LED_NO_SCANNER]  = pat_no_scanner,
    [LED_ALL_GOOD]    = pat_all_good,
    [LED_NO_SERVER]   = pat_no_server,
    [LED_WIFI_DOWN]   = pat_wifi_down,
};

/* ═══════════════════════════════════════════════════════════════════════
 * S3 RGB LED (WS2812 on GPIO48)
 * ═══════════════════════════════════════════════════════════════════════ */
#if defined(UPLINK_ESP32S3)

#include "led_strip.h"

#define LED_GPIO    48

typedef struct { uint8_t r, g, b; } rgb_t;

/* Colours per pattern — on phase */
static const rgb_t s_on_colours[] = {
    [LED_IDLE]        = {  0, 10,  0 },   /* dim green */
    [LED_SCANNING]    = {  0, 25,  0 },   /* green */
    [LED_DETECTION]   = { 20,  0, 20 },   /* purple */
    [LED_UPLOADING]   = {  0, 15, 15 },   /* cyan */
    [LED_ERROR]       = { 30,  0,  0 },   /* red */
    [LED_NO_GPS]      = { 10, 10,  0 },   /* dim yellow */
    [LED_NO_SCANNER]  = {  0,  0, 25 },   /* blue */
    [LED_ALL_GOOD]    = {  0, 30,  0 },   /* bright green */
    [LED_NO_SERVER]   = { 25, 20,  0 },   /* yellow */
    [LED_WIFI_DOWN]   = { 30,  0,  0 },   /* red */
};

/* Alternate colour for patterns that flash between two colours */
static const rgb_t s_alt_colours[] = {
    [LED_WIFI_DOWN]   = { 25, 15,  0 },   /* yellow (alternates with red) */
};

static led_strip_handle_t s_strip = NULL;

static void rgb_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return;
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

static void rgb_off(void)
{
    if (!s_strip) return;
    led_strip_clear(s_strip);
}

static void led_task(void *arg)
{
    ESP_LOGI(TAG, "RGB LED task started (GPIO%d)", LED_GPIO);
    int alt_phase = 0;

    while (1) {
        led_pattern_t pat = (led_pattern_t)atomic_load(&s_current_pattern);
        const led_step_t *steps = s_patterns[pat];
        int step = 0;

        while (1) {
            led_pattern_t cur = (led_pattern_t)atomic_load(&s_current_pattern);
            if (cur != pat) break;
            if (steps[step].on_ms == 0) { step = 0; continue; }

            /* ON phase — use alt colour on odd cycles for alternating patterns */
            const rgb_t *c;
            if ((pat == LED_WIFI_DOWN) && (alt_phase & 1)) {
                c = &s_alt_colours[pat];
            } else {
                c = &s_on_colours[pat];
            }
            rgb_set(c->r, c->g, c->b);
            vTaskDelay(pdMS_TO_TICKS(steps[step].on_ms));

            cur = (led_pattern_t)atomic_load(&s_current_pattern);
            if (cur != pat) { rgb_off(); break; }

            /* OFF phase */
            rgb_off();
            vTaskDelay(pdMS_TO_TICKS(steps[step].off_ms));

            step++;
            alt_phase++;
        }
    }
}

void led_init(void)
{
    led_strip_config_t cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    esp_err_t err = led_strip_new_rmt_device(&cfg, &rmt, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RGB LED init failed: %s", esp_err_to_name(err));
        return;
    }
    rgb_off();
    ESP_LOGI(TAG, "RGB LED initialized (WS2812 GPIO%d)", LED_GPIO);
}

/* ═══════════════════════════════════════════════════════════════════════
 * C3 / plain ESP32 — simple GPIO LED
 * ═══════════════════════════════════════════════════════════════════════ */
#else

#include "driver/gpio.h"

#if defined(UPLINK_ESP32)
#define LED_GPIO    GPIO_NUM_2
#else
#define LED_GPIO    GPIO_NUM_8
#endif

static void led_task(void *arg)
{
    ESP_LOGI(TAG, "LED task started");

    while (1) {
        led_pattern_t pat = (led_pattern_t)atomic_load(&s_current_pattern);
        const led_step_t *steps = s_patterns[pat];
        int step = 0;

        while (1) {
            led_pattern_t cur = (led_pattern_t)atomic_load(&s_current_pattern);
            if (cur != pat) break;
            if (steps[step].on_ms == 0) { step = 0; continue; }

            gpio_set_level(LED_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(steps[step].on_ms));

            cur = (led_pattern_t)atomic_load(&s_current_pattern);
            if (cur != pat) { gpio_set_level(LED_GPIO, 0); break; }

            gpio_set_level(LED_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(steps[step].off_ms));

            step++;
        }
    }
}

void led_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(LED_GPIO, 0);
    ESP_LOGI(TAG, "LED initialized (GPIO%d)", LED_GPIO);
}

#endif /* UPLINK_ESP32S3 */

/* ── Shared API ──────────────────────────────────────────────────────── */

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
