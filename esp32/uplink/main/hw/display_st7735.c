/**
 * Friend or Foe -- Uplink Display (ST7735, FoF Badge variant)
 *
 * Drives a Waveshare 1.8" 128x160 ST7735 panel over SPI3 (HSPI).
 * Mirrors the oled_* public API so display_task and main app code are
 * unchanged between production (SSD1306 OLED) and badge (this) builds.
 *
 * Build-time selection: only compiled into the FoF Badge build via the
 * FOF_BADGE_VARIANT macro from the platformio uplink-s3-fof_badge env.
 *
 * Pinout (Seeed XIAO ESP32-S3 edge):
 *   SDA (MOSI) = GPIO9
 *   SCK        = GPIO7
 *   CS         = GPIO44
 *   RES        = GPIO6
 *   DC         = GPIO5
 *   BTN1       = GPIO8  to GND (active-low)
 *   BTN2       = GPIO43 to GND (active-low)
 *   BL/LED     = tied high (no PWM control)
 *
 * Framebuffer: 128 * 160 * 2 = 40 960 bytes, allocated from PSRAM.
 */

#ifdef FOF_BADGE_VARIANT

#include "oled_display.h"
#include "uart_rx.h"
#include "version.h"
#include "detection_types.h"
#include "badge_threat_policy.h"
#include "badge_runtime.h"
#include "badge_display_policy_runtime.h"
#include "badge_button_gesture.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"  /* esp_rom_delay_us for bit-bang fallback */

static const char *TAG = "st7735";

/* ── Hardware ──────────────────────────────────────────────────────────── */

#define ST7735_PIN_MOSI     9
#define ST7735_PIN_SCK      7
#define ST7735_PIN_CS       44
#define ST7735_PIN_DC       5
#define ST7735_PIN_RES      6
#define BADGE_BUTTON_TRIFORCE_PIN 8
#define BADGE_BUTTON_QR_PIN       43
#define BADGE_BUTTON_TRIFORCE_ACTIVE_HIGH 0
#define BADGE_BUTTON_QR_ACTIVE_HIGH       0
#define BADGE_BUTTON_POLL_MS      20
#define BADGE_BUTTON_DEBOUNCE_MS  60
#define BADGE_BUTTON_LONG_MS      850
#define BADGE_BUTTON_DOUBLE_TAP_MS 320
#define BADGE_DETAIL_TIMEOUT_MS   30000
#define BADGE_BUTTON_STACK_WORDS  4096
/* Backlight: panel BL pin is wired directly to 3V3 on this badge build,
 * so the firmware does not drive it. -1 disables the GPIO drive path. */
#define ST7735_PIN_BL       (-1)
#define ST7735_SPI_HOST     SPI3_HOST
/* 8 MHz keeps the animated badge view fluid while staying below the speeds
 * that tend to punish spicy jumper wiring on small ST7735 panels. */
#define ST7735_SPI_HZ       (8 * 1000 * 1000)

#define LCD_W               128
#define LCD_H               160
#define LCD_PIXELS          (LCD_W * LCD_H)
#define LCD_FB_BYTES        (LCD_PIXELS * 2)

/* RGB565 colors */
#define COL_BLACK           0x0000
#define COL_WHITE           0xFFFF
#define COL_RED             0xF800
#define COL_GREEN           0x07E0
#define COL_BLUE            0x001F
#define COL_YELLOW          0xFFE0
#define COL_CYAN            0x07FF
#define COL_MAGENTA         0xF81F
#define COL_ROSE            0xF833
#define COL_VIOLET          0xA81F
#define COL_DEEP_VIOLET     0x4010
#define COL_SKIM_RED        0xF940
#define COL_DEEP_SKIM       0x5000
#define COL_DEEP_ROSE       0x780D
#define COL_DEEP_CYAN       0x0213
#define COL_DEEP_GOLD       0x6200
#define COL_GOLD            0xFEA0   /* Triforce gold (rich amber) */
#define COL_GOLD_DIM        0x9C40
#define COL_GOLD_DARK       0x6A20
#define COL_GRAY            0x8410
#define COL_DARKGRAY        0x4208
#define COL_DIMRED          0x6000
#define COL_DIMGREEN        0x0320
#define COL_LINK_GREEN      0x05E0
#define COL_LINK_BRIGHT     0x57EA
#define COL_LINK_DARK       0x01E0
#define COL_PANEL           0x1082
#define COL_PANEL_2         0x2104
#define COL_SOFT_GREEN      0x2F65

/* ST7735 commands */
#define ST_CMD_SWRESET      0x01
#define ST_CMD_SLPOUT       0x11
#define ST_CMD_NORON        0x13
#define ST_CMD_INVOFF       0x20
#define ST_CMD_DISPON       0x29
#define ST_CMD_CASET        0x2A
#define ST_CMD_RASET        0x2B
#define ST_CMD_RAMWR        0x2C
#define ST_CMD_MADCTL       0x36
#define ST_CMD_COLMOD       0x3A
#define ST_CMD_FRMCTR1      0xB1
#define ST_CMD_FRMCTR2      0xB2
#define ST_CMD_FRMCTR3      0xB3
#define ST_CMD_INVCTR       0xB4
#define ST_CMD_PWCTR1       0xC0
#define ST_CMD_PWCTR2       0xC1
#define ST_CMD_PWCTR3       0xC2
#define ST_CMD_PWCTR4       0xC3
#define ST_CMD_PWCTR5       0xC4
#define ST_CMD_VMCTR1       0xC5
#define ST_CMD_GMCTRP1      0xE0
#define ST_CMD_GMCTRN1      0xE1

#define BADGE_LOWER_PAGE_FRAMES         10U
#define BADGE_LOWER_MARQUEE_CHARS        3U
#define BADGE_LOWER_MARQUEE_FRAMES       5U

/* ── State ─────────────────────────────────────────────────────────────── */

static spi_device_handle_t s_spi = NULL;
static uint16_t           *s_fb = NULL;       /* big endian RGB565 */
static uint8_t            *s_tx_chunk = NULL; /* internal DMA-safe SPI staging */
static bool                s_initialized = false;
static SemaphoreHandle_t   s_display_mutex = NULL;
static int                 s_spi_error_count = 0;
static uint32_t s_anim_frame = 0;
static uint32_t s_queue_page_frame = 0;

typedef enum {
    BADGE_BUTTON_OVERLAY_NONE = 0,
    BADGE_BUTTON_OVERLAY_TRIFORCE,
    BADGE_BUTTON_OVERLAY_QR,
} badge_button_overlay_t;

typedef enum {
    BADGE_BUTTON_ID_NEXT = 0,
    BADGE_BUTTON_ID_DETAIL = 1,
} badge_button_id_t;

static volatile badge_button_overlay_t s_button_overlay = BADGE_BUTTON_OVERLAY_NONE;
static TaskHandle_t s_button_task = NULL;

static volatile int s_focus_index = 0;
static volatile bool s_detail_mode = false;
static volatile int s_detail_page = 0;
static volatile int64_t s_last_button_ms = 0;
static oled_badge_button_state_t s_button_diag = {0};
static badge_button_gesture_t s_b2_gesture = {0};

typedef struct {
    bool active;
    bool is_entity;
    bool severe;
    int y;
    int h;
    char lane[16];
    char title[32];
    char detail[OLED_BADGE_STATE_TEXT_LEN];
    char stat[12];
    char key[OLED_BADGE_STATE_KEY_LEN];
    int item_index;
    int item_total;
    badge_threat_snapshot_entity_t entity;
} badge_focus_entry_t;

#define BADGE_FOCUS_ENTRY_MAX 4

typedef struct {
    badge_focus_entry_t entries[BADGE_FOCUS_ENTRY_MAX];
    int count;
    int focus_index;
    bool detail_mode;
    int detail_page;
    uint32_t generation;
} badge_focus_model_t;

static badge_focus_model_t s_focus_model = {0};

static bool display_lock(TickType_t wait_ticks)
{
    if (!s_display_mutex) return true;
    return xSemaphoreTake(s_display_mutex, wait_ticks) == pdTRUE;
}

static void display_unlock(void)
{
    if (s_display_mutex) {
        xSemaphoreGive(s_display_mutex);
    }
}

static inline void st_cs_set(int level)
{
    gpio_set_level(ST7735_PIN_CS, level);
}

static inline uint64_t st_cs_pin_mask(void)
{
    return (1ULL << ST7735_PIN_CS);
}

/* SPI callbacks use transaction.user as a 0/1 flag to drive DC and manually
 * assert CS on the badge's GPIO44 panel select line. */
static void st7735_spi_pre_cb(spi_transaction_t *t)
{
    int dc = (int)(intptr_t)t->user;
    gpio_set_level(ST7735_PIN_DC, dc);
    st_cs_set(0);
}

static void st7735_spi_post_cb(spi_transaction_t *t)
{
    (void)t;
    st_cs_set(1);
}

typedef struct {
    int pin;
    int diag_index;
    bool active_high;
    badge_button_id_t id;
    badge_button_overlay_t overlay;
    bool last_raw_pressed;
    bool stable_pressed;
    bool long_sent;
    bool boot_ignored;
    TickType_t changed_tick;
    TickType_t pressed_tick;
} badge_button_state_t;

static int64_t badge_now_ms(void)
{
    return (int64_t)(esp_timer_get_time() / 1000LL);
}

static void badge_button_note_activity(void)
{
    s_last_button_ms = badge_now_ms();
}

static void badge_button_diag_note_raw(const badge_button_state_t *button,
                                       bool raw_pressed,
                                       int raw_level)
{
    if (!button) {
        return;
    }
    if (button->diag_index == 0) {
        s_button_diag.b1_active_high = button->active_high;
        s_button_diag.b1_raw_level = raw_level;
        s_button_diag.b1_raw_pressed = raw_pressed;
        s_button_diag.b1_stable_pressed = button->stable_pressed;
        s_button_diag.b1_boot_ignored = button->boot_ignored;
    } else {
        s_button_diag.b2_active_high = button->active_high;
        s_button_diag.b2_raw_level = raw_level;
        s_button_diag.b2_raw_pressed = raw_pressed;
        s_button_diag.b2_stable_pressed = button->stable_pressed;
        s_button_diag.b2_boot_ignored = button->boot_ignored;
    }
}

static void badge_button_diag_note_edge(const badge_button_state_t *button)
{
    if (!button) {
        return;
    }
    if (button->diag_index == 0) {
        s_button_diag.b1_raw_edges++;
        s_button_diag.b1_last_event_ms = badge_now_ms();
    } else {
        s_button_diag.b2_raw_edges++;
        s_button_diag.b2_last_event_ms = badge_now_ms();
    }
}

static void badge_button_diag_note_stable(const badge_button_state_t *button,
                                          bool pressed)
{
    if (!button) {
        return;
    }
    if (button->diag_index == 0) {
        s_button_diag.b1_stable_pressed = pressed;
        s_button_diag.b1_boot_ignored = button->boot_ignored;
        if (!pressed) {
            s_button_diag.b1_releases++;
        }
        s_button_diag.b1_last_event_ms = badge_now_ms();
    } else {
        s_button_diag.b2_stable_pressed = pressed;
        s_button_diag.b2_boot_ignored = button->boot_ignored;
        if (!pressed) {
            s_button_diag.b2_releases++;
        }
        s_button_diag.b2_last_event_ms = badge_now_ms();
    }
}

static void badge_button_diag_note_short(const badge_button_state_t *button)
{
    if (!button) {
        return;
    }
    if (button->diag_index == 0) {
        s_button_diag.b1_short_presses++;
        s_button_diag.b1_last_event_ms = badge_now_ms();
    } else {
        s_button_diag.b2_short_presses++;
        s_button_diag.b2_last_event_ms = badge_now_ms();
    }
}

static void badge_button_diag_note_long(const badge_button_state_t *button)
{
    if (!button) {
        return;
    }
    if (button->diag_index == 0) {
        s_button_diag.b1_long_presses++;
        s_button_diag.b1_last_event_ms = badge_now_ms();
    } else {
        s_button_diag.b2_long_presses++;
        s_button_diag.b2_last_event_ms = badge_now_ms();
    }
}

static void badge_button_diag_note_b2_double(void)
{
    s_button_diag.b2_double_taps++;
    s_button_diag.b2_last_event_ms = badge_now_ms();
}

static void badge_button_diag_set_b2_pending(void)
{
    s_button_diag.b2_pending_single =
        badge_button_gesture_pending_single(&s_b2_gesture);
}

static void badge_button_diag_set_b2_gesture(const char *gesture)
{
    snprintf(s_button_diag.b2_last_gesture,
             sizeof(s_button_diag.b2_last_gesture),
             "%s",
             gesture ? gesture : "");
    badge_button_diag_set_b2_pending();
}

static bool badge_button_is_pressed(int pin, bool active_high)
{
    int level = gpio_get_level(pin);
    return active_high ? (level != 0) : (level == 0);
}

static bool badge_button_level_is_pressed(int level, bool active_high)
{
    return active_high ? (level != 0) : (level == 0);
}

static void badge_display_nav_next(void)
{
    badge_button_note_activity();
    s_button_overlay = BADGE_BUTTON_OVERLAY_NONE;
    s_detail_page = 0;
    int count = s_focus_model.count > 0 ? s_focus_model.count : BADGE_FOCUS_ENTRY_MAX;
    if (count <= 0) {
        count = 1;
    }
    s_focus_index = (s_focus_index + 1) % count;
}

static void badge_display_nav_detail(void)
{
    badge_button_note_activity();
    s_button_overlay = BADGE_BUTTON_OVERLAY_NONE;
    if (s_detail_mode) {
        s_detail_mode = false;
        s_detail_page = 0;
    } else {
        s_detail_mode = true;
        s_detail_page = 0;
    }
}

static void badge_display_nav_page(void)
{
    badge_button_note_activity();
    s_button_overlay = BADGE_BUTTON_OVERLAY_NONE;
    s_detail_page = (s_detail_page + 1) % 3;
}

static void badge_button_toggle_overlay(badge_button_overlay_t overlay)
{
    badge_button_note_activity();
    s_detail_mode = false;
    s_detail_page = 0;
    badge_button_overlay_t current = s_button_overlay;
    s_button_overlay = (current == overlay) ? BADGE_BUTTON_OVERLAY_NONE : overlay;
}

static void badge_button_working_single_press(void)
{
    if (s_button_overlay != BADGE_BUTTON_OVERLAY_NONE) {
        badge_button_note_activity();
        s_button_overlay = BADGE_BUTTON_OVERLAY_NONE;
        s_detail_mode = false;
        s_detail_page = 0;
        return;
    }
    if (s_detail_mode) {
        badge_display_nav_page();
    } else {
        badge_display_nav_next();
    }
}

static void badge_button_working_double_press(void)
{
    if (s_button_overlay != BADGE_BUTTON_OVERLAY_NONE) {
        badge_button_note_activity();
        s_button_overlay = BADGE_BUTTON_OVERLAY_NONE;
        s_detail_mode = false;
        s_detail_page = 0;
        return;
    }
    badge_display_nav_detail();
}

static void badge_button_working_long_press(void)
{
    badge_button_note_activity();
    s_detail_mode = false;
    s_detail_page = 0;
    if (s_button_overlay == BADGE_BUTTON_OVERLAY_QR) {
        s_button_overlay = BADGE_BUTTON_OVERLAY_TRIFORCE;
    } else {
        s_button_overlay = BADGE_BUTTON_OVERLAY_QR;
    }
}

static void badge_button_dispatch_b2_gesture(badge_button_gesture_event_t event)
{
    switch (event) {
    case BADGE_BUTTON_GESTURE_SINGLE:
        badge_button_diag_set_b2_gesture("single");
        badge_button_working_single_press();
        break;
    case BADGE_BUTTON_GESTURE_DOUBLE:
        badge_button_diag_note_b2_double();
        badge_button_diag_set_b2_gesture("double");
        badge_button_working_double_press();
        break;
    case BADGE_BUTTON_GESTURE_LONG:
        badge_button_diag_set_b2_gesture("long");
        badge_button_working_long_press();
        break;
    case BADGE_BUTTON_GESTURE_NONE:
    default:
        badge_button_diag_set_b2_pending();
        break;
    }
}

static void badge_button_short_press(badge_button_id_t id)
{
    if (s_button_overlay != BADGE_BUTTON_OVERLAY_NONE) {
        badge_button_note_activity();
        s_button_overlay = BADGE_BUTTON_OVERLAY_NONE;
        return;
    }
    if (s_detail_mode) {
        if (id == BADGE_BUTTON_ID_NEXT) {
            badge_display_nav_page();
        } else {
            badge_display_nav_detail();
        }
        return;
    }
    if (id == BADGE_BUTTON_ID_NEXT) {
        badge_display_nav_next();
    } else {
        badge_display_nav_detail();
    }
}

static void badge_button_poll_one(badge_button_state_t *button, TickType_t now)
{
    int raw_level = gpio_get_level(button->pin);
    bool raw_pressed = badge_button_level_is_pressed(raw_level,
                                                     button->active_high);
    badge_button_diag_note_raw(button, raw_pressed, raw_level);
    TickType_t debounce_ticks = pdMS_TO_TICKS(BADGE_BUTTON_DEBOUNCE_MS);
    if (debounce_ticks == 0) debounce_ticks = 1;
    TickType_t long_ticks = pdMS_TO_TICKS(BADGE_BUTTON_LONG_MS);
    if (long_ticks == 0) long_ticks = 1;

    if (raw_pressed != button->last_raw_pressed) {
        button->last_raw_pressed = raw_pressed;
        button->changed_tick = now;
        badge_button_diag_note_edge(button);
        return;
    }

    if (raw_pressed == button->stable_pressed) {
        return;
    }
    if ((TickType_t)(now - button->changed_tick) < debounce_ticks) {
        return;
    }

    button->stable_pressed = raw_pressed;
    if (button->stable_pressed) {
        button->pressed_tick = now;
        button->long_sent = false;
        button->boot_ignored = false;
        badge_button_diag_note_stable(button, true);
        return;
    }

    bool boot_ignored = button->boot_ignored;
    TickType_t held_ticks = now - button->pressed_tick;
    bool long_press = held_ticks >= long_ticks;
    button->boot_ignored = false;
    badge_button_diag_note_stable(button, false);
    if (boot_ignored) {
        button->long_sent = false;
    } else if (long_press) {
        button->long_sent = true;
        badge_button_diag_note_long(button);
        if (button->diag_index == 1) {
            badge_button_dispatch_b2_gesture(
                badge_button_gesture_note_long(&s_b2_gesture,
                                               (uint32_t)badge_now_ms()));
        } else {
            badge_button_toggle_overlay(button->overlay);
        }
    } else if (!button->long_sent) {
        badge_button_diag_note_short(button);
        if (button->diag_index == 1) {
            badge_button_gesture_event_t event =
                badge_button_gesture_note_tap(&s_b2_gesture,
                                              (uint32_t)badge_now_ms());
            if (event == BADGE_BUTTON_GESTURE_NONE) {
                badge_button_diag_set_b2_gesture("pending");
            } else {
                badge_button_dispatch_b2_gesture(event);
            }
        } else {
            badge_button_short_press(button->id);
        }
    }
}

static void badge_button_poll_hold(badge_button_state_t *button, TickType_t now)
{
    (void)button;
    (void)now;
}

static void badge_button_task(void *arg)
{
    (void)arg;
    badge_button_gesture_init(&s_b2_gesture, BADGE_BUTTON_DOUBLE_TAP_MS);
    badge_button_diag_set_b2_gesture("");
    TickType_t now = xTaskGetTickCount();
    bool triforce_pressed_at_boot = badge_button_is_pressed(
        BADGE_BUTTON_TRIFORCE_PIN,
        BADGE_BUTTON_TRIFORCE_ACTIVE_HIGH);
    bool qr_pressed_at_boot = badge_button_is_pressed(
        BADGE_BUTTON_QR_PIN,
        BADGE_BUTTON_QR_ACTIVE_HIGH);
    badge_button_state_t buttons[] = {
        {
            .pin = BADGE_BUTTON_TRIFORCE_PIN,
            .diag_index = 0,
            .active_high = BADGE_BUTTON_TRIFORCE_ACTIVE_HIGH,
            .id = BADGE_BUTTON_ID_DETAIL,
            .overlay = BADGE_BUTTON_OVERLAY_TRIFORCE,
            .last_raw_pressed = triforce_pressed_at_boot,
            .stable_pressed = triforce_pressed_at_boot,
            .long_sent = triforce_pressed_at_boot,
            .boot_ignored = triforce_pressed_at_boot,
            .changed_tick = now,
            .pressed_tick = now,
        },
        {
            .pin = BADGE_BUTTON_QR_PIN,
            .diag_index = 1,
            .active_high = BADGE_BUTTON_QR_ACTIVE_HIGH,
            .id = BADGE_BUTTON_ID_NEXT,
            .overlay = BADGE_BUTTON_OVERLAY_QR,
            .last_raw_pressed = qr_pressed_at_boot,
            .stable_pressed = qr_pressed_at_boot,
            .long_sent = qr_pressed_at_boot,
            .boot_ignored = qr_pressed_at_boot,
            .changed_tick = now,
            .pressed_tick = now,
        },
    };
    s_button_diag.b1_raw_pressed = triforce_pressed_at_boot;
    s_button_diag.b1_stable_pressed = triforce_pressed_at_boot;
    s_button_diag.b1_boot_ignored = triforce_pressed_at_boot;
    s_button_diag.b1_active_high = BADGE_BUTTON_TRIFORCE_ACTIVE_HIGH;
    s_button_diag.b1_raw_level = gpio_get_level(BADGE_BUTTON_TRIFORCE_PIN);
    s_button_diag.b2_raw_pressed = qr_pressed_at_boot;
    s_button_diag.b2_stable_pressed = qr_pressed_at_boot;
    s_button_diag.b2_boot_ignored = qr_pressed_at_boot;
    s_button_diag.b2_active_high = BADGE_BUTTON_QR_ACTIVE_HIGH;
    s_button_diag.b2_raw_level = gpio_get_level(BADGE_BUTTON_QR_PIN);

    while (true) {
        now = xTaskGetTickCount();
        for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
            badge_button_poll_one(&buttons[i], now);
            badge_button_poll_hold(&buttons[i], now);
            if (buttons[i].diag_index == 1 &&
                !buttons[i].stable_pressed &&
                !buttons[i].last_raw_pressed) {
                badge_button_dispatch_b2_gesture(
                    badge_button_gesture_poll(&s_b2_gesture,
                                              (uint32_t)badge_now_ms()));
            }
        }
        badge_button_diag_set_b2_pending();
        vTaskDelay(pdMS_TO_TICKS(BADGE_BUTTON_POLL_MS));
    }
}

static void badge_buttons_start(void)
{
    if (s_button_task) return;

    gpio_reset_pin(BADGE_BUTTON_TRIFORCE_PIN);
    gpio_reset_pin(BADGE_BUTTON_QR_PIN);

    gpio_config_t button_io = {
        .pin_bit_mask = (1ULL << BADGE_BUTTON_TRIFORCE_PIN) |
                        (1ULL << BADGE_BUTTON_QR_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&button_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Badge button GPIO config failed: %d", err);
        return;
    }

    BaseType_t ok = xTaskCreate(badge_button_task, "badge_buttons",
                                BADGE_BUTTON_STACK_WORDS,
                                NULL, 3, &s_button_task);
    if (ok != pdPASS) {
        s_button_task = NULL;
        ESP_LOGE(TAG, "Badge button task start failed");
        return;
    }
    ESP_LOGI(TAG, "Badge buttons active: GPIO%d=detail/Triforce active-%s GPIO%d=next/QR active-low",
             BADGE_BUTTON_TRIFORCE_PIN,
             BADGE_BUTTON_TRIFORCE_ACTIVE_HIGH ? "high" : "low",
             BADGE_BUTTON_QR_PIN);
}

/* ── 5x7 ASCII font (printable chars 0x20-0x7E) ───────────────────────── */
/* Same column-major layout as oled_display.c: 5 columns per char, bit N
 * of each column is row N (top = bit 0). */
static const uint8_t font_5x7[][5] = {
    /* 0x20 ' ' */ {0x00, 0x00, 0x00, 0x00, 0x00},
    /* 0x21 '!' */ {0x00, 0x00, 0x5F, 0x00, 0x00},
    /* 0x22 '"' */ {0x00, 0x07, 0x00, 0x07, 0x00},
    /* 0x23 '#' */ {0x14, 0x7F, 0x14, 0x7F, 0x14},
    /* 0x24 '$' */ {0x24, 0x2A, 0x7F, 0x2A, 0x12},
    /* 0x25 '%' */ {0x23, 0x13, 0x08, 0x64, 0x62},
    /* 0x26 '&' */ {0x36, 0x49, 0x55, 0x22, 0x50},
    /* 0x27 ''' */ {0x00, 0x05, 0x03, 0x00, 0x00},
    /* 0x28 '(' */ {0x00, 0x1C, 0x22, 0x41, 0x00},
    /* 0x29 ')' */ {0x00, 0x41, 0x22, 0x1C, 0x00},
    /* 0x2A '*' */ {0x14, 0x08, 0x3E, 0x08, 0x14},
    /* 0x2B '+' */ {0x08, 0x08, 0x3E, 0x08, 0x08},
    /* 0x2C ',' */ {0x00, 0x50, 0x30, 0x00, 0x00},
    /* 0x2D '-' */ {0x08, 0x08, 0x08, 0x08, 0x08},
    /* 0x2E '.' */ {0x00, 0x60, 0x60, 0x00, 0x00},
    /* 0x2F '/' */ {0x20, 0x10, 0x08, 0x04, 0x02},
    /* 0x30 '0' */ {0x3E, 0x51, 0x49, 0x45, 0x3E},
    /* 0x31 '1' */ {0x00, 0x42, 0x7F, 0x40, 0x00},
    /* 0x32 '2' */ {0x42, 0x61, 0x51, 0x49, 0x46},
    /* 0x33 '3' */ {0x21, 0x41, 0x45, 0x4B, 0x31},
    /* 0x34 '4' */ {0x18, 0x14, 0x12, 0x7F, 0x10},
    /* 0x35 '5' */ {0x27, 0x45, 0x45, 0x45, 0x39},
    /* 0x36 '6' */ {0x3C, 0x4A, 0x49, 0x49, 0x30},
    /* 0x37 '7' */ {0x01, 0x71, 0x09, 0x05, 0x03},
    /* 0x38 '8' */ {0x36, 0x49, 0x49, 0x49, 0x36},
    /* 0x39 '9' */ {0x06, 0x49, 0x49, 0x29, 0x1E},
    /* 0x3A ':' */ {0x00, 0x36, 0x36, 0x00, 0x00},
    /* 0x3B ';' */ {0x00, 0x56, 0x36, 0x00, 0x00},
    /* 0x3C '<' */ {0x08, 0x14, 0x22, 0x41, 0x00},
    /* 0x3D '=' */ {0x14, 0x14, 0x14, 0x14, 0x14},
    /* 0x3E '>' */ {0x00, 0x41, 0x22, 0x14, 0x08},
    /* 0x3F '?' */ {0x02, 0x01, 0x51, 0x09, 0x06},
    /* 0x40 '@' */ {0x32, 0x49, 0x79, 0x41, 0x3E},
    /* 0x41 'A' */ {0x7E, 0x11, 0x11, 0x11, 0x7E},
    /* 0x42 'B' */ {0x7F, 0x49, 0x49, 0x49, 0x36},
    /* 0x43 'C' */ {0x3E, 0x41, 0x41, 0x41, 0x22},
    /* 0x44 'D' */ {0x7F, 0x41, 0x41, 0x22, 0x1C},
    /* 0x45 'E' */ {0x7F, 0x49, 0x49, 0x49, 0x41},
    /* 0x46 'F' */ {0x7F, 0x09, 0x09, 0x09, 0x01},
    /* 0x47 'G' */ {0x3E, 0x41, 0x49, 0x49, 0x7A},
    /* 0x48 'H' */ {0x7F, 0x08, 0x08, 0x08, 0x7F},
    /* 0x49 'I' */ {0x00, 0x41, 0x7F, 0x41, 0x00},
    /* 0x4A 'J' */ {0x20, 0x40, 0x41, 0x3F, 0x01},
    /* 0x4B 'K' */ {0x7F, 0x08, 0x14, 0x22, 0x41},
    /* 0x4C 'L' */ {0x7F, 0x40, 0x40, 0x40, 0x40},
    /* 0x4D 'M' */ {0x7F, 0x02, 0x0C, 0x02, 0x7F},
    /* 0x4E 'N' */ {0x7F, 0x04, 0x08, 0x10, 0x7F},
    /* 0x4F 'O' */ {0x3E, 0x41, 0x41, 0x41, 0x3E},
    /* 0x50 'P' */ {0x7F, 0x09, 0x09, 0x09, 0x06},
    /* 0x51 'Q' */ {0x3E, 0x41, 0x51, 0x21, 0x5E},
    /* 0x52 'R' */ {0x7F, 0x09, 0x19, 0x29, 0x46},
    /* 0x53 'S' */ {0x46, 0x49, 0x49, 0x49, 0x31},
    /* 0x54 'T' */ {0x01, 0x01, 0x7F, 0x01, 0x01},
    /* 0x55 'U' */ {0x3F, 0x40, 0x40, 0x40, 0x3F},
    /* 0x56 'V' */ {0x1F, 0x20, 0x40, 0x20, 0x1F},
    /* 0x57 'W' */ {0x3F, 0x40, 0x38, 0x40, 0x3F},
    /* 0x58 'X' */ {0x63, 0x14, 0x08, 0x14, 0x63},
    /* 0x59 'Y' */ {0x07, 0x08, 0x70, 0x08, 0x07},
    /* 0x5A 'Z' */ {0x61, 0x51, 0x49, 0x45, 0x43},
    /* 0x5B '[' */ {0x00, 0x7F, 0x41, 0x41, 0x00},
    /* 0x5C '\' */ {0x02, 0x04, 0x08, 0x10, 0x20},
    /* 0x5D ']' */ {0x00, 0x41, 0x41, 0x7F, 0x00},
    /* 0x5E '^' */ {0x04, 0x02, 0x01, 0x02, 0x04},
    /* 0x5F '_' */ {0x40, 0x40, 0x40, 0x40, 0x40},
    /* 0x60 '`' */ {0x00, 0x01, 0x02, 0x04, 0x00},
    /* 0x61 'a' */ {0x20, 0x54, 0x54, 0x54, 0x78},
    /* 0x62 'b' */ {0x7F, 0x48, 0x44, 0x44, 0x38},
    /* 0x63 'c' */ {0x38, 0x44, 0x44, 0x44, 0x20},
    /* 0x64 'd' */ {0x38, 0x44, 0x44, 0x48, 0x7F},
    /* 0x65 'e' */ {0x38, 0x54, 0x54, 0x54, 0x18},
    /* 0x66 'f' */ {0x08, 0x7E, 0x09, 0x01, 0x02},
    /* 0x67 'g' */ {0x0C, 0x52, 0x52, 0x52, 0x3E},
    /* 0x68 'h' */ {0x7F, 0x08, 0x04, 0x04, 0x78},
    /* 0x69 'i' */ {0x00, 0x44, 0x7D, 0x40, 0x00},
    /* 0x6A 'j' */ {0x20, 0x40, 0x44, 0x3D, 0x00},
    /* 0x6B 'k' */ {0x7F, 0x10, 0x28, 0x44, 0x00},
    /* 0x6C 'l' */ {0x00, 0x41, 0x7F, 0x40, 0x00},
    /* 0x6D 'm' */ {0x7C, 0x04, 0x18, 0x04, 0x78},
    /* 0x6E 'n' */ {0x7C, 0x08, 0x04, 0x04, 0x78},
    /* 0x6F 'o' */ {0x38, 0x44, 0x44, 0x44, 0x38},
    /* 0x70 'p' */ {0x7C, 0x14, 0x14, 0x14, 0x08},
    /* 0x71 'q' */ {0x08, 0x14, 0x14, 0x18, 0x7C},
    /* 0x72 'r' */ {0x7C, 0x08, 0x04, 0x04, 0x08},
    /* 0x73 's' */ {0x48, 0x54, 0x54, 0x54, 0x20},
    /* 0x74 't' */ {0x04, 0x3F, 0x44, 0x40, 0x20},
    /* 0x75 'u' */ {0x3C, 0x40, 0x40, 0x20, 0x7C},
    /* 0x76 'v' */ {0x1C, 0x20, 0x40, 0x20, 0x1C},
    /* 0x77 'w' */ {0x3C, 0x40, 0x30, 0x40, 0x3C},
    /* 0x78 'x' */ {0x44, 0x28, 0x10, 0x28, 0x44},
    /* 0x79 'y' */ {0x0C, 0x50, 0x50, 0x50, 0x3C},
    /* 0x7A 'z' */ {0x44, 0x64, 0x54, 0x4C, 0x44},
    /* 0x7B '{' */ {0x00, 0x08, 0x36, 0x41, 0x00},
    /* 0x7C '|' */ {0x00, 0x00, 0x7F, 0x00, 0x00},
    /* 0x7D '}' */ {0x00, 0x41, 0x36, 0x08, 0x00},
    /* 0x7E '~' */ {0x10, 0x08, 0x08, 0x10, 0x08},
};

/* ── Low-level SPI helpers ─────────────────────────────────────────────── */

static inline uint16_t swap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

static void st_write_cmd(uint8_t cmd)
{
    spi_transaction_t t = {
        .length    = 8,
        .flags     = SPI_TRANS_USE_TXDATA,
        .tx_data   = { cmd, 0, 0, 0 },
        .user      = (void *)0,    /* DC=0 (command) */
    };
    esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    if (err != ESP_OK && s_spi_error_count++ < 8) {
        ESP_LOGE(TAG, "SPI cmd 0x%02X failed: %s", cmd, esp_err_to_name(err));
    }
}

static void st_write_data(const uint8_t *buf, size_t len)
{
    if (len == 0) return;
    spi_transaction_t t = {
        .length    = len * 8,
        .user      = (void *)1,    /* DC=1 (data) */
    };

    if (len <= sizeof(t.tx_data)) {
        t.flags = SPI_TRANS_USE_TXDATA;
        memcpy(t.tx_data, buf, len);
    } else {
        const uint8_t *tx = buf;
        if (s_tx_chunk && buf != s_tx_chunk && !esp_ptr_dma_capable(buf) &&
            len <= (20 * LCD_W * 2)) {
            memcpy(s_tx_chunk, buf, len);
            tx = s_tx_chunk;
        }
        t.tx_buffer = tx;
    }

    esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    if (err != ESP_OK && s_spi_error_count++ < 8) {
        ESP_LOGE(TAG, "SPI data len=%u failed: %s", (unsigned)len, esp_err_to_name(err));
    }
}

static void st_write_data_byte(uint8_t b)
{
    st_write_data(&b, 1);
}

static void st_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t cas[4] = { 0, (uint8_t)(x0 & 0xFF), 0, (uint8_t)(x1 & 0xFF) };
    uint8_t ras[4] = { 0, (uint8_t)(y0 & 0xFF), 0, (uint8_t)(y1 & 0xFF) };
    st_write_cmd(ST_CMD_CASET);
    st_write_data(cas, 4);
    st_write_cmd(ST_CMD_RASET);
    st_write_data(ras, 4);
    st_write_cmd(ST_CMD_RAMWR);
}

/* Push the entire framebuffer to the panel.  Chunked so each transaction
 * stays within the SPI master DMA limit (max_transfer_sz). */
static void st_flush(void)
{
    st_set_window(0, 0, LCD_W - 1, LCD_H - 1);

    /* 8 chunks of 20 rows each = 20 * 128 * 2 = 5120 bytes per chunk. */
    const int rows_per_chunk = 20;
    const int bytes_per_chunk = rows_per_chunk * LCD_W * 2;
    for (int row = 0; row < LCD_H; row += rows_per_chunk) {
        const uint8_t *src = (const uint8_t *)(s_fb + row * LCD_W);
        if (s_tx_chunk) {
            memcpy(s_tx_chunk, src, bytes_per_chunk);
            st_write_data(s_tx_chunk, bytes_per_chunk);
        } else {
            st_write_data(src, bytes_per_chunk);
        }
    }
}

/* ── Framebuffer drawing primitives (RGB565 big-endian) ────────────────── */

static inline void fb_set_pixel(int x, int y, uint16_t color)
{
    if ((unsigned)x >= LCD_W || (unsigned)y >= LCD_H) return;
    s_fb[y * LCD_W + x] = swap16(color);
}

static void fb_clear(uint16_t color)
{
    uint16_t be = swap16(color);
    for (int i = 0; i < LCD_PIXELS; i++) s_fb[i] = be;
}

static void fb_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0)            { w += x; x = 0; }
    if (y < 0)            { h += y; y = 0; }
    if (x + w > LCD_W)    w = LCD_W - x;
    if (y + h > LCD_H)    h = LCD_H - y;
    if (w <= 0 || h <= 0) return;
    uint16_t be = swap16(color);
    for (int yy = y; yy < y + h; yy++) {
        uint16_t *row = s_fb + yy * LCD_W + x;
        for (int xx = 0; xx < w; xx++) row[xx] = be;
    }
}

static void fb_hline(int x0, int x1, int y, uint16_t color)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y < 0 || y >= LCD_H) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= LCD_W) x1 = LCD_W - 1;
    uint16_t be = swap16(color);
    uint16_t *row = s_fb + y * LCD_W;
    for (int x = x0; x <= x1; x++) row[x] = be;
}

static void fb_draw_line(int x0, int y0, int x1, int y1, uint16_t color)
{
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (1) {
        fb_set_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void fb_fill_ellipse(int cx, int cy, int rx, int ry, uint16_t color)
{
    if (rx <= 0 || ry <= 0) {
        return;
    }
    for (int yy = -ry; yy <= ry; yy++) {
        int y2 = yy * yy;
        int ry2 = ry * ry;
        int rx2 = rx * rx;
        int inside = ry2 - y2;
        if (inside < 0) {
            continue;
        }
        int xx = rx;
        while (xx > 0 && (xx * xx * ry2 + y2 * rx2) > rx2 * ry2) {
            xx--;
        }
        fb_hline(cx - xx, cx + xx, cy + yy, color);
    }
}

/* Standard scanline triangle fill: split into flat-bottom + flat-top by the
 * middle vertex's y, interpolate left/right edges, hline-fill each row. */
static void fb_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2,
                             uint16_t color)
{
    /* Sort vertices by y ascending. */
    if (y1 < y0) { int tx = x0, ty = y0; x0 = x1; y0 = y1; x1 = tx; y1 = ty; }
    if (y2 < y0) { int tx = x0, ty = y0; x0 = x2; y0 = y2; x2 = tx; y2 = ty; }
    if (y2 < y1) { int tx = x1, ty = y1; x1 = x2; y1 = y2; x2 = tx; y2 = ty; }

    if (y2 == y0) {
        int xmin = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
        int xmax = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
        fb_hline(xmin, xmax, y0, color);
        return;
    }

    int total_h = y2 - y0;
    for (int y = y0; y <= y2; y++) {
        bool second_half = (y > y1) || (y1 == y0);
        int seg_h = second_half ? (y2 - y1) : (y1 - y0);
        int alpha_num = y - y0;
        int beta_num  = second_half ? (y - y1) : (y - y0);
        if (seg_h == 0) seg_h = 1;
        if (total_h == 0) total_h = 1;

        int ax = x0 + ((x2 - x0) * alpha_num) / total_h;
        int bx = second_half
            ? x1 + ((x2 - x1) * beta_num) / seg_h
            : x0 + ((x1 - x0) * beta_num) / seg_h;
        if (ax > bx) { int t = ax; ax = bx; bx = t; }
        fb_hline(ax, bx, y, color);
    }
}

static void fb_fill_triangle_scanlined(int x0, int y0, int x1, int y1,
                                       int x2, int y2, uint16_t fill,
                                       uint16_t hi, uint16_t lo,
                                       uint8_t stripe_phase)
{
    if (y1 < y0) { int tx = x0, ty = y0; x0 = x1; y0 = y1; x1 = tx; y1 = ty; }
    if (y2 < y0) { int tx = x0, ty = y0; x0 = x2; y0 = y2; x2 = tx; y2 = ty; }
    if (y2 < y1) { int tx = x1, ty = y1; x1 = x2; y1 = y2; x2 = tx; y2 = ty; }

    if (y2 == y0) {
        int xmin = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
        int xmax = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
        fb_hline(xmin, xmax, y0, fill);
        return;
    }

    int total_h = y2 - y0;
    for (int y = y0; y <= y2; y++) {
        bool second_half = (y > y1) || (y1 == y0);
        int seg_h = second_half ? (y2 - y1) : (y1 - y0);
        int alpha_num = y - y0;
        int beta_num  = second_half ? (y - y1) : (y - y0);
        if (seg_h == 0) seg_h = 1;
        if (total_h == 0) total_h = 1;

        int ax = x0 + ((x2 - x0) * alpha_num) / total_h;
        int bx = second_half
            ? x1 + ((x2 - x1) * beta_num) / seg_h
            : x0 + ((x1 - x0) * beta_num) / seg_h;
        if (ax > bx) { int t = ax; ax = bx; bx = t; }

        uint16_t row_color = fill;
        uint8_t stripe = (uint8_t)((y + stripe_phase) & 0x07);
        if (stripe == 0 || stripe == 1) {
            row_color = hi;
        } else if (stripe == 4) {
            row_color = lo;
        }
        fb_hline(ax, bx, y, row_color);
    }
}

static uint16_t rgb565_scale_color(uint16_t color, uint8_t scale)
{
    uint16_t r = (uint16_t)(((color >> 11) & 0x1F) * scale / 255);
    uint16_t g = (uint16_t)(((color >> 5)  & 0x3F) * scale / 255);
    uint16_t b = (uint16_t)(( color        & 0x1F) * scale / 255);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint16_t rgb565_mix_color(uint16_t a, uint16_t b, uint8_t t)
{
    uint8_t inv = (uint8_t)(255 - t);
    uint16_t ar = (a >> 11) & 0x1F;
    uint16_t ag = (a >> 5)  & 0x3F;
    uint16_t ab =  a        & 0x1F;
    uint16_t br = (b >> 11) & 0x1F;
    uint16_t bg = (b >> 5)  & 0x3F;
    uint16_t bb =  b        & 0x1F;
    uint16_t r = (uint16_t)((ar * inv + br * t) / 255);
    uint16_t g = (uint16_t)((ag * inv + bg * t) / 255);
    uint16_t bl = (uint16_t)((ab * inv + bb * t) / 255);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

/* Draw a single character at (x,y), with optional integer scale (1, 2, 3).
 * fg is foreground color, bg is background — pass bg == fg to skip bg fill. */
static int fb_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg, int scale)
{
    if (scale < 1) scale = 1;
    if (c < 0x20 || c > 0x7E) c = '?';
    int idx = c - 0x20;

    bool draw_bg = (bg != fg);
    for (int col = 0; col < 5; col++) {
        uint8_t column = font_5x7[idx][col];
        for (int row = 0; row < 7; row++) {
            uint16_t color = (column & (1 << row)) ? fg : bg;
            if (!draw_bg && color == bg) continue;
            fb_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
        }
    }
    return (5 + 1) * scale;  /* advance, including 1-pixel spacing */
}

static int fb_draw_string(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    int x0 = x;
    while (*s) {
        x += fb_draw_char(x, y, *s, fg, bg, scale);
        s++;
    }
    return x - x0;  /* total advance */
}

static int str_pixel_width(const char *s, int scale)
{
    int n = 0;
    while (*s++) n++;
    if (n == 0) return 0;
    return n * 6 * scale - scale;  /* last char doesn't need trailing space */
}

static void fb_draw_string_centered(int cx, int y, const char *s,
                                    uint16_t fg, uint16_t bg, int scale)
{
    int w = str_pixel_width(s, scale);
    fb_draw_string(cx - w / 2, y, s, fg, bg, scale);
}

/* ── Pin-blink diagnostic ──────────────────────────────────────────────── */

/* Slow, multimeter-readable blink on each output pin in turn. Hold a DMM
 * in DC-volts mode on the XIAO pad (or on the wire at the panel side, to
 * also catch broken jumpers): a working pin should oscillate between ~0V
 * and ~3.3V on the slow cadence below. A pin that stays at 0V or 3.3V is
 * not actually being driven (wrong GPIO, severed wire, or bridged short).
 *
 * Pulses 6 pins × 4 cycles × 500 ms = ~12 s. Watch the serial log so you
 * know which pin is being driven. */
static void diag_pin_blink(void)
{
    const struct { int pin; const char *name; } pins[] = {
        { ST7735_PIN_MOSI, "MOSI/SDA  GPIO9" },
        { ST7735_PIN_SCK,  "SCK       GPIO7" },
        { ST7735_PIN_CS,   "CS        GPIO44" },
        { ST7735_PIN_DC,   "DC/A0     GPIO5" },
        { ST7735_PIN_RES,  "RES/RST   GPIO6" },
#if ST7735_PIN_BL >= 0
        { ST7735_PIN_BL,   "BL/LED" },
#endif
    };
    int n = sizeof(pins) / sizeof(pins[0]);
    for (int i = 0; i < n; i++) {
        gpio_reset_pin(pins[i].pin);
        gpio_set_direction(pins[i].pin, GPIO_MODE_OUTPUT);
        gpio_set_level(pins[i].pin, 0);
    }

    for (int i = 0; i < n; i++) {
        ESP_LOGW(TAG, "PIN BLINK >>> %s   (4 cycles, 500ms each — measure now)",
                 pins[i].name);
        for (int j = 0; j < 4; j++) {
            gpio_set_level(pins[i].pin, 1); vTaskDelay(pdMS_TO_TICKS(500));
            gpio_set_level(pins[i].pin, 0); vTaskDelay(pdMS_TO_TICKS(500));
        }
        gpio_set_level(pins[i].pin, 1);   /* leave HIGH so SPI init starts clean */
    }
    ESP_LOGW(TAG, "PIN BLINK done — proceeding with SPI init");
}

/* ── ST7735 init sequence (red-tab 1.8" 128x160 — Waveshare default) ──── */

static void st7735_hw_reset(void)
{
    gpio_set_level(ST7735_PIN_RES, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(ST7735_PIN_RES, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(ST7735_PIN_RES, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
}

static void st7735_panel_init(void)
{
    st_write_cmd(ST_CMD_SWRESET);  vTaskDelay(pdMS_TO_TICKS(150));
    st_write_cmd(ST_CMD_SLPOUT);   vTaskDelay(pdMS_TO_TICKS(255));

    static const uint8_t fr[3]  = {0x01, 0x2C, 0x2D};
    static const uint8_t fr3[6] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};
    st_write_cmd(ST_CMD_FRMCTR1); st_write_data(fr,  3);
    st_write_cmd(ST_CMD_FRMCTR2); st_write_data(fr,  3);
    st_write_cmd(ST_CMD_FRMCTR3); st_write_data(fr3, 6);
    st_write_cmd(ST_CMD_INVCTR);  st_write_data_byte(0x07);

    static const uint8_t pwr1[3] = {0xA2, 0x02, 0x84};
    st_write_cmd(ST_CMD_PWCTR1); st_write_data(pwr1, 3);
    st_write_cmd(ST_CMD_PWCTR2); st_write_data_byte(0xC5);
    static const uint8_t pwr3[2] = {0x0A, 0x00};
    st_write_cmd(ST_CMD_PWCTR3); st_write_data(pwr3, 2);
    static const uint8_t pwr4[2] = {0x8A, 0x2A};
    st_write_cmd(ST_CMD_PWCTR4); st_write_data(pwr4, 2);
    static const uint8_t pwr5[2] = {0x8A, 0xEE};
    st_write_cmd(ST_CMD_PWCTR5); st_write_data(pwr5, 2);
    st_write_cmd(ST_CMD_VMCTR1); st_write_data_byte(0x0E);

    st_write_cmd(ST_CMD_INVOFF);
    /* MADCTL: MX | MY with RGB color order. Keep the orientation bits from
     * the red-tab portrait setup, but clear BGR so RGB565 constants draw true
     * red/blue on the badge heat bars. */
    st_write_cmd(ST_CMD_MADCTL); st_write_data_byte(0xC0);
    st_write_cmd(ST_CMD_COLMOD); st_write_data_byte(0x05);  /* 16-bit RGB565 */

    static const uint8_t gp[16] = {
        0x02, 0x1c, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2d,
        0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10
    };
    static const uint8_t gn[16] = {
        0x03, 0x1d, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
        0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10
    };
    st_write_cmd(ST_CMD_GMCTRP1); st_write_data(gp, 16);
    st_write_cmd(ST_CMD_GMCTRN1); st_write_data(gn, 16);

    st_write_cmd(ST_CMD_NORON);  vTaskDelay(pdMS_TO_TICKS(10));
    st_write_cmd(ST_CMD_DISPON); vTaskDelay(pdMS_TO_TICKS(100));
}

/* ── Alternate panel init: ILI9163C ────────────────────────────────────── */

/* Many "ST7735 1.8 inch 128x160" modules sold cheaply on Amazon/Aliexpress
 * are actually ILI9163C silicon. The protocol is similar but the gamma /
 * frame-rate / power-control registers differ. If the ST7735R init makes
 * the panel ignore us but ILI9163C makes it light up, we know what we have. */
static void panel_init_ili9163c_peripheral(void)
{
    st_write_cmd(0x01);  vTaskDelay(pdMS_TO_TICKS(150));   /* SWRESET */
    st_write_cmd(0x11);  vTaskDelay(pdMS_TO_TICKS(120));   /* SLPOUT  */
    st_write_cmd(0x26);  st_write_data_byte(0x04);          /* GAMSET  */
    st_write_cmd(0xF2);  st_write_data_byte(0x01);          /* gamma function en */
    st_write_cmd(0x36);  st_write_data_byte(0xC0);          /* MADCTL RGB */
    st_write_cmd(0x3A);  st_write_data_byte(0x05);          /* COLMOD  16-bit */
    st_write_cmd(0xB1);                                     /* FRMCTR1 */
    {   uint8_t d[2] = {0x08, 0x02}; st_write_data(d, 2); }
    st_write_cmd(0xB4);  st_write_data_byte(0x07);          /* INVCTR  */
    st_write_cmd(0xC0);                                     /* PWCTR1  */
    {   uint8_t d[2] = {0x0A, 0x02}; st_write_data(d, 2); }
    st_write_cmd(0xC1);  st_write_data_byte(0x02);          /* PWCTR2  */
    st_write_cmd(0xC5);                                     /* VMCTR1  */
    {   uint8_t d[2] = {0x50, 0x5B}; st_write_data(d, 2); }
    st_write_cmd(0xC7);  st_write_data_byte(0x40);          /* VMOFCTR */
    st_write_cmd(0x29);  vTaskDelay(pdMS_TO_TICKS(100));    /* DISPON  */
}

/* ── Bit-bang SPI fallback ─────────────────────────────────────────────── */

/* Used when we suspect the SPI3 peripheral isn't actually clocking data
 * out (config quirk, pin matrix issue, etc.). At ~500 kHz this is much
 * slower than the peripheral but it's bulletproof — every transition is
 * a software gpio_set_level().
 *
 * The peripheral SPI device must be released (spi_bus_remove_device +
 * spi_bus_free) before bb_pin_setup_outputs is called, so the pins are
 * available as plain GPIOs. */

#define BB_HALF_PERIOD_US  1   /* gives ~500 kHz SCK */

static bool s_bb_active = false;

static void bb_pin_setup_outputs(void)
{
    gpio_reset_pin(ST7735_PIN_MOSI);
    gpio_reset_pin(ST7735_PIN_SCK);
    gpio_reset_pin(ST7735_PIN_CS);
    gpio_reset_pin(ST7735_PIN_DC);
    /* RES already configured as output earlier; re-set direction in case
     * gpio_reset_pin elsewhere wiped it. */
    gpio_set_direction(ST7735_PIN_MOSI, GPIO_MODE_OUTPUT);
    gpio_set_direction(ST7735_PIN_SCK,  GPIO_MODE_OUTPUT);
    gpio_set_direction(ST7735_PIN_CS,   GPIO_MODE_OUTPUT);
    gpio_set_direction(ST7735_PIN_DC,   GPIO_MODE_OUTPUT);
    gpio_set_direction(ST7735_PIN_RES,  GPIO_MODE_OUTPUT);
    st_cs_set(1);
    gpio_set_level(ST7735_PIN_SCK, 0);
    gpio_set_level(ST7735_PIN_DC,  1);
    s_bb_active = true;
}

static inline void bb_send_byte(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(ST7735_PIN_MOSI, (b >> i) & 1);
        gpio_set_level(ST7735_PIN_SCK, 1);
        esp_rom_delay_us(BB_HALF_PERIOD_US);
        gpio_set_level(ST7735_PIN_SCK, 0);
        esp_rom_delay_us(BB_HALF_PERIOD_US);
    }
}

static void bb_write_cmd(uint8_t c)
{
    gpio_set_level(ST7735_PIN_DC, 0);
    st_cs_set(0);
    bb_send_byte(c);
    st_cs_set(1);
}

static void bb_write_data_buf(const uint8_t *buf, size_t len)
{
    gpio_set_level(ST7735_PIN_DC, 1);
    st_cs_set(0);
    for (size_t i = 0; i < len; i++) bb_send_byte(buf[i]);
    st_cs_set(1);
}

static void bb_write_data_byte(uint8_t b) { bb_write_data_buf(&b, 1); }

static void bb_hw_reset(void)
{
    gpio_set_level(ST7735_PIN_RES, 1); vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(ST7735_PIN_RES, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(ST7735_PIN_RES, 1); vTaskDelay(pdMS_TO_TICKS(150));
}

static void bb_init_st7735r(void)
{
    bb_hw_reset();
    bb_write_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(150));    /* SWRESET */
    bb_write_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(120));    /* SLPOUT  */
    bb_write_cmd(0x36); bb_write_data_byte(0xC0);          /* MADCTL RGB */
    bb_write_cmd(0x3A); bb_write_data_byte(0x05);          /* COLMOD 16-bit */
    bb_write_cmd(0x29); vTaskDelay(pdMS_TO_TICKS(100));    /* DISPON  */
}

static void bb_init_ili9163c(void)
{
    bb_hw_reset();
    bb_write_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(150));    /* SWRESET */
    bb_write_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(120));    /* SLPOUT  */
    bb_write_cmd(0x26); bb_write_data_byte(0x04);          /* GAMSET  */
    bb_write_cmd(0xF2); bb_write_data_byte(0x01);          /* gamma fn */
    bb_write_cmd(0x36); bb_write_data_byte(0xC0);          /* MADCTL RGB */
    bb_write_cmd(0x3A); bb_write_data_byte(0x05);          /* COLMOD  */
    bb_write_cmd(0x29); vTaskDelay(pdMS_TO_TICKS(100));    /* DISPON  */
}

/* Fill the top NROWS rows with a solid color via bit-bang.  Filling all
 * 160 rows at 500 kHz takes ~1.3s; we limit to NROWS to keep the test
 * snappy but still visibly cover ~25-50% of the screen. */
static void bb_fill_top(uint16_t color, int nrows)
{
    if (nrows > LCD_H) nrows = LCD_H;
    uint8_t hi = (color >> 8) & 0xFF;
    uint8_t lo = color & 0xFF;

    bb_write_cmd(0x2A);  /* CASET */
    uint8_t ca[4] = {0, 0, 0, (uint8_t)(LCD_W - 1)};
    bb_write_data_buf(ca, 4);

    bb_write_cmd(0x2B);  /* RASET */
    uint8_t ra[4] = {0, 0, 0, (uint8_t)(nrows - 1)};
    bb_write_data_buf(ra, 4);

    bb_write_cmd(0x2C);  /* RAMWR */
    gpio_set_level(ST7735_PIN_DC, 1);
    st_cs_set(0);
    int total = LCD_W * nrows;
    for (int i = 0; i < total; i++) {
        bb_send_byte(hi);
        bb_send_byte(lo);
    }
    st_cs_set(1);
}

/* ── Diagnostic test pattern (MADCTL + INVERT sweep) ───────────────────── */

/* For a panel showing "backlight on, all white" we need to figure out
 * whether SPI is getting through at all. This routine sweeps through
 * common MADCTL orientations and inversion flags, painting a different
 * solid color at each step.
 *
 * If you see ANY of these colors flash on the panel, the SPI link works
 * and the only problem is which init flags the panel needs. Note the
 * label of the color you saw and we'll lock that combo in.
 *
 * If the panel stays solid white through the whole sweep, the SPI traffic
 * isn't reaching the controller at all — suspect a missing wire (MOSI,
 * SCK, CS, DC, or RST) or a controller that isn't ST7735-compatible. */
static void try_combo(uint8_t madctl, bool invert, uint16_t color, const char *label)
{
    ESP_LOGI(TAG, "diag: madctl=0x%02X invert=%d  -> %s", madctl, invert, label);
    st_write_cmd(ST_CMD_MADCTL); st_write_data_byte(madctl);
    st_write_cmd(invert ? 0x21 /* INVON */ : ST_CMD_INVOFF);
    st_write_cmd(ST_CMD_DISPON);
    fb_clear(color);
    st_flush();
    vTaskDelay(pdMS_TO_TICKS(800));
}

static void splash_test_pattern(void)
{
    /* === PHASE 1: Peripheral SPI, alternate controllers ================ */
    /* The previous splash already proved peripheral SPI + ST7735R init
     * doesn't paint anything visible. Two alternates to try here:
     *  - ILI9163C silicon (very common in cheap "ST7735" modules)
     *  - ST7735 with green-tab col/row offset (visible window starts at
     *    (2,1) instead of (0,0)) */

    ESP_LOGW(TAG, "DIAG-A: peripheral SPI + ILI9163C init -> RED top half");
    panel_init_ili9163c_peripheral();
    st_write_cmd(ST_CMD_CASET);
    {   uint8_t d[4] = {0, 0, 0, LCD_W - 1};       st_write_data(d, 4); }
    st_write_cmd(ST_CMD_RASET);
    {   uint8_t d[4] = {0, 0, 0, (LCD_H / 2) - 1}; st_write_data(d, 4); }
    st_write_cmd(ST_CMD_RAMWR);
    {
        uint8_t pix[2] = { (COL_RED >> 8) & 0xFF, COL_RED & 0xFF };
        for (int i = 0; i < LCD_W * (LCD_H / 2); i++) st_write_data(pix, 2);
    }
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGW(TAG, "DIAG-B: peripheral SPI + ST7735R green-tab offset(2,1) -> GREEN top half");
    st7735_panel_init();
    st_write_cmd(ST_CMD_CASET);
    {   uint8_t d[4] = {0, 2, 0, 2 + LCD_W - 1};       st_write_data(d, 4); }
    st_write_cmd(ST_CMD_RASET);
    {   uint8_t d[4] = {0, 1, 0, 1 + (LCD_H / 2) - 1}; st_write_data(d, 4); }
    st_write_cmd(ST_CMD_RAMWR);
    {
        uint8_t pix[2] = { (COL_GREEN >> 8) & 0xFF, COL_GREEN & 0xFF };
        for (int i = 0; i < LCD_W * (LCD_H / 2); i++) st_write_data(pix, 2);
    }
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* === PHASE 2: Tear down peripheral SPI, switch to bit-bang ========= */
    ESP_LOGW(TAG, "DIAG-C: tearing down SPI3 peripheral, switching to bit-bang...");
    if (s_spi) {
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
    }
    spi_bus_free(ST7735_SPI_HOST);
    bb_pin_setup_outputs();

    ESP_LOGW(TAG, "DIAG-D: bit-bang + ST7735R init -> BLUE (top 80 rows)");
    bb_init_st7735r();
    bb_fill_top(COL_BLUE, 80);
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGW(TAG, "DIAG-E: bit-bang + ILI9163C init -> YELLOW (top 80 rows)");
    bb_init_ili9163c();
    bb_fill_top(COL_YELLOW, 80);
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* === PHASE 3: Restore peripheral SPI for the rest of the firmware == */
    ESP_LOGW(TAG, "DIAG-F: restoring SPI3 peripheral for normal operation");
    s_bb_active = false;

    spi_bus_config_t buscfg = {
        .miso_io_num     = -1,
        .mosi_io_num     = ST7735_PIN_MOSI,
        .sclk_io_num     = ST7735_PIN_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 20 * LCD_W * 2,
    };
    spi_bus_initialize(ST7735_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = ST7735_SPI_HZ,
        .mode           = 0,
        .spics_io_num   = -1,
        .queue_size     = 4,
        .pre_cb         = st7735_spi_pre_cb,
        .post_cb        = st7735_spi_post_cb,
    };
    spi_bus_add_device(ST7735_SPI_HOST, &devcfg, &s_spi);
    st7735_hw_reset();
    st7735_panel_init();
}

/* ── Triforce splash ───────────────────────────────────────────────────── */

static void draw_triforce(uint16_t fill_color)
{
    /* Outer triangle: apex top center, base wide near vertical center.
     *   apex   = (64, 18)
     *   left   = (16, 92)
     *   right  = (112, 92)
     * Sub-triangles share midpoints of the outer sides:
     *   top-mid     = (40, 55)
     *   right-mid   = (88, 55)
     *   bottom-mid  = (64, 92)
     * Three filled gold triangles; central inverted triangle stays black. */
    fb_fill_triangle(64, 18, 40, 55, 88, 55, fill_color);    /* top */
    fb_fill_triangle(16, 92, 40, 55, 64, 92, fill_color);    /* bottom-left */
    fb_fill_triangle(88, 55, 112, 92, 64, 92, fill_color);   /* bottom-right */
}

static void draw_triforce_scaled(int cx, int cy, int w, int h, int skew,
                                 uint16_t fill_color, uint16_t shadow_color)
{
    int ax = cx + skew;
    int ay = cy - h / 2;
    int lx = cx - w / 2 - skew / 2;
    int ly = cy + h / 2;
    int rx = cx + w / 2 - skew / 2;
    int ry = ly;
    int lmx = (ax + lx) / 2;
    int lmy = (ay + ly) / 2;
    int rmx = (ax + rx) / 2;
    int rmy = (ay + ry) / 2;
    int bmx = (lx + rx) / 2;
    int bmy = ly;

    if (w < 12) {
        fb_fill_rect(cx - 2, ay, 4, h, fill_color);
        return;
    }

    /* Drop-shadow first, then the three visible pieces. */
    fb_fill_triangle(ax + 2, ay + 3, lmx + 2, lmy + 3, rmx + 2, rmy + 3, shadow_color);
    fb_fill_triangle(lx + 2, ly + 3, lmx + 2, lmy + 3, bmx + 2, bmy + 3, shadow_color);
    fb_fill_triangle(rmx + 2, rmy + 3, rx + 2, ry + 3, bmx + 2, bmy + 3, shadow_color);

    fb_fill_triangle(ax, ay, lmx, lmy, rmx, rmy, fill_color);
    fb_fill_triangle(lx, ly, lmx, lmy, bmx, bmy, fill_color);
    fb_fill_triangle(rmx, rmy, rx, ry, bmx, bmy, fill_color);

    /* Small highlights sell the pseudo-3D flip without needing line drawing. */
    fb_fill_triangle(ax, ay, (ax + lmx) / 2, (ay + lmy) / 2,
                     (ax + rmx) / 2, (ay + rmy) / 2, COL_LINK_BRIGHT);
}

static void draw_triforce_flat_scaled(int cx, int cy, int w, int h, int skew,
                                      uint16_t fill_color)
{
    int ax = cx + skew;
    int ay = cy - h / 2;
    int lx = cx - w / 2 - skew / 2;
    int ly = cy + h / 2;
    int rx = cx + w / 2 - skew / 2;
    int ry = ly;
    int lmx = (ax + lx) / 2;
    int lmy = (ay + ly) / 2;
    int rmx = (ax + rx) / 2;
    int rmy = (ay + ry) / 2;
    int bmx = (lx + rx) / 2;
    int bmy = ly;

    fb_fill_triangle(ax, ay, lmx, lmy, rmx, rmy, fill_color);
    fb_fill_triangle(lx, ly, lmx, lmy, bmx, bmy, fill_color);
    fb_fill_triangle(rmx, rmy, rx, ry, bmx, bmy, fill_color);
}

static void draw_panel_triforce_mark(int y, int h, uint16_t bg, uint16_t tint)
{
    uint16_t mark = rgb565_mix_color(bg, rgb565_scale_color(tint, 72), 30);
    draw_triforce_flat_scaled(LCD_W - 22, y + h / 2 + 3, 42, 32, 0, mark);
}

typedef struct {
    int cx;
    int apex_y;
    int base_y;
    int half_w;
    uint8_t phase_offset;
} triforce_piece_t;

static int project_flip_x(int pivot, int x, int q8)
{
    return pivot + ((x - pivot) * q8) / 256;
}

static void draw_flip_shadow(int cx, int y, int half_w)
{
    for (int row = 0; row < 5; row++) {
        fb_hline(cx - half_w + row * 5, cx + half_w - row * 5,
                 y + row, rgb565_scale_color(COL_DIMGREEN, 80));
    }
}

static void draw_flipping_triforce_piece(const triforce_piece_t *piece,
                                         uint8_t phase,
                                         uint16_t threat_color)
{
    static const int16_t flip_q8[16] = {
        256, 228, 176, 96, 18, -96, -176, -228,
        -256, -228, -176, -96, -18, 96, 176, 228,
    };

    int q8 = flip_q8[phase & 0x0F];
    int abs_q8 = q8 < 0 ? -q8 : q8;
    bool back_face = q8 < 0;

    int apex_x = project_flip_x(piece->cx, piece->cx, q8);
    int left_x = project_flip_x(piece->cx, piece->cx - piece->half_w, q8);
    int right_x = project_flip_x(piece->cx, piece->cx + piece->half_w, q8);
    int apex_y = piece->apex_y;
    int base_y = piece->base_y;

    uint16_t base = threat_color == COL_BLACK ? COL_LINK_GREEN : threat_color;
    uint16_t fill = back_face
        ? rgb565_mix_color(rgb565_scale_color(base, 112), COL_GOLD_DARK, 48)
        : rgb565_mix_color(base, COL_GOLD, 22);
    uint16_t hi = back_face
        ? rgb565_mix_color(fill, COL_GOLD_DIM, 36)
        : rgb565_mix_color(fill, COL_WHITE, 70);
    uint16_t lo = back_face
        ? rgb565_scale_color(fill, 78)
        : rgb565_scale_color(fill, 138);

    if (abs_q8 < 28) {
        int x = piece->cx + (back_face ? 1 : -1);
        uint16_t edge = back_face ? lo : hi;
        fb_fill_rect(x - 1, apex_y + 1, 3, base_y - apex_y - 1, edge);
        fb_draw_line(x + 2, apex_y + 2, x + 2, base_y - 1, lo);
        fb_draw_line(x - 2, apex_y + 3, x - 2, base_y - 2, hi);
        return;
    }

    fb_fill_triangle_scanlined(apex_x + 2, apex_y + 3,
                               left_x + 2, base_y + 3,
                               right_x + 2, base_y + 3,
                               COL_BLACK, COL_BLACK, COL_BLACK, phase);
    fb_fill_triangle_scanlined(apex_x + 1, apex_y + 2,
                               left_x + 1, base_y + 2,
                               right_x + 1, base_y + 2,
                               rgb565_scale_color(fill, 70),
                               rgb565_scale_color(fill, 85),
                               COL_BLACK,
                               phase);
    fb_fill_triangle_scanlined(apex_x, apex_y, left_x, base_y, right_x, base_y,
                               fill, hi, lo, phase);

    if (back_face) {
        fb_draw_line(apex_x, apex_y, left_x, base_y, lo);
        fb_draw_line(apex_x, apex_y, right_x, base_y, hi);
    } else {
        fb_draw_line(apex_x, apex_y, left_x, base_y, hi);
        fb_draw_line(apex_x, apex_y, right_x, base_y, lo);
    }
    fb_draw_line(left_x, base_y, right_x, base_y, rgb565_scale_color(fill, 95));
}

static void draw_triforce_flip(uint32_t frame, uint16_t drone_color,
                               uint16_t privacy_color, uint16_t wifi_color)
{
    static const triforce_piece_t pieces[] = {
        { 64, 11, 32, 16, 0 },
        { 47, 34, 56, 16, 5 },
        { 81, 34, 56, 16, 10 },
    };
    const uint16_t colors[] = {
        drone_color,
        privacy_color,
        wifi_color,
    };

    uint8_t phase = (uint8_t)(frame & 0x0F);
    draw_flip_shadow(LCD_W / 2, 55, 34);
    for (size_t i = 0; i < sizeof(pieces) / sizeof(pieces[0]); i++) {
        draw_flipping_triforce_piece(&pieces[i],
                                     (uint8_t)(phase + pieces[i].phase_offset),
                                     colors[i]);
    }
}

static void draw_triforce_splash_static(uint16_t triforce_color)
{
    fb_clear(COL_BLACK);

    /* Subtle starfield-ish dot pattern in upper-right corner — keeps it
     * from looking too empty without burning a lot of pixels. */
    for (int i = 0; i < 12; i++) {
        int sx = (i * 17 + 7)  % 120 + 4;
        int sy = (i * 11 + 3)  % 12  + 2;
        fb_set_pixel(sx, sy, COL_GRAY);
    }

    draw_triforce(triforce_color);

    /* Title */
    fb_draw_string_centered(LCD_W / 2, 108, "FRIEND OR FOE", COL_WHITE, COL_BLACK, 1);

    /* Version line — dim, small */
    fb_draw_string_centered(LCD_W / 2, 122, "v" FOF_VERSION, COL_GRAY, COL_BLACK, 1);

    /* Light tagline */
    fb_draw_string_centered(LCD_W / 2, 140, "FIELD SIGNALS", COL_SOFT_GREEN, COL_BLACK, 1);
}

static void splash_show(void)
{
    draw_triforce_splash_static(COL_LINK_GREEN);

    st_flush();

    /* ~1.8s hold with a small pulse on the bottom-right Triforce piece. */
    const uint16_t pulse_seq[] = { COL_LINK_GREEN, COL_LINK_BRIGHT, COL_LINK_GREEN, COL_LINK_DARK, COL_LINK_GREEN };
    for (size_t i = 0; i < sizeof(pulse_seq) / sizeof(pulse_seq[0]); i++) {
        fb_fill_triangle(88, 55, 112, 92, 64, 92, pulse_seq[i]);
        st_flush();
        vTaskDelay(pdMS_TO_TICKS(360));
    }
}

static const char *const s_gamechangers_qr_rows[] = {
    "1111111011100110001111111",
    "1000001010011100101000001",
    "1011101011110110001011101",
    "1011101001001100101011101",
    "1011101011100000101011101",
    "1000001001111100101000001",
    "1111111010101010101111111",
    "0000000001001111100000000",
    "1001111110110010110010111",
    "1011100110111001000111110",
    "0111101000000111111101001",
    "0000010000110010101101111",
    "1000011111101001101100001",
    "1101010100001111100010010",
    "1100101111100101110011111",
    "1000100000110000111101101",
    "1010101101011100111110110",
    "0000000011000100100010110",
    "1111111011111110101010001",
    "1000001011111101100010000",
    "1011101010100001111110000",
    "1011101010110111011000011",
    "1011101000110101010011111",
    "1000001000000000001110111",
    "1111111011101000101001001",
};

static void draw_gamechangers_qr_screen(void)
{
    enum {
        QR_MODULES = 25,
        QR_QUIET_MODULES = 4,
        QR_SCALE = 3,
    };
    const int qr_px = (QR_MODULES + QR_QUIET_MODULES * 2) * QR_SCALE;
    const int x0 = (LCD_W - qr_px) / 2;
    const int y0 = 9;
    const int module_x0 = x0 + QR_QUIET_MODULES * QR_SCALE;
    const int module_y0 = y0 + QR_QUIET_MODULES * QR_SCALE;

    fb_clear(COL_BLACK);
    fb_fill_rect(x0, y0, qr_px, qr_px, COL_WHITE);
    for (int row = 0; row < QR_MODULES; row++) {
        const char *bits = s_gamechangers_qr_rows[row];
        for (int col = 0; col < QR_MODULES; col++) {
            if (bits[col] == '1') {
                fb_fill_rect(module_x0 + col * QR_SCALE,
                             module_y0 + row * QR_SCALE,
                             QR_SCALE, QR_SCALE, COL_BLACK);
            }
        }
    }

    fb_draw_string_centered(LCD_W / 2, 116, "SCAN BADGE", COL_LINK_BRIGHT, COL_BLACK, 1);
    fb_draw_string_centered(LCD_W / 2, 131, "gamechangersai.org", COL_WHITE, COL_BLACK, 1);
    fb_draw_string_centered(LCD_W / 2, 146, "BTN2 QR", COL_GRAY, COL_BLACK, 1);
}

/* ── Status layout helpers ─────────────────────────────────────────────── */

typedef enum {
    BADGE_UI_DOMAIN_DRONE = 0,
    BADGE_UI_DOMAIN_PRIVACY,
    BADGE_UI_DOMAIN_WIFI,
} badge_ui_domain_t;

static badge_ui_domain_t ui_domain_for_class(badge_threat_class_t cls)
{
    if (cls == BADGE_THREAT_DRONE) {
        return BADGE_UI_DOMAIN_DRONE;
    }
    if (cls == BADGE_THREAT_WIFI_ANOMALY) {
        return BADGE_UI_DOMAIN_WIFI;
    }
    return BADGE_UI_DOMAIN_PRIVACY;
}

static badge_ui_domain_t ui_domain_for_category(badge_threat_category_t category)
{
    switch (category) {
        case BADGE_THREAT_CATEGORY_DRONE:
        case BADGE_THREAT_CATEGORY_SSID:
            return BADGE_UI_DOMAIN_DRONE;
        case BADGE_THREAT_CATEGORY_WIFI:
            return BADGE_UI_DOMAIN_WIFI;
        default:
            return BADGE_UI_DOMAIN_PRIVACY;
    }
}

static const char *ui_domain_code(badge_ui_domain_t domain)
{
    switch (domain) {
        case BADGE_UI_DOMAIN_DRONE:   return "DRN";
        case BADGE_UI_DOMAIN_WIFI:    return "WIFI";
        case BADGE_UI_DOMAIN_PRIVACY:
        default:                      return "PRV";
    }
}

static const char *ui_item_code(const badge_threat_snapshot_entity_t *item)
{
    if (!item) {
        return "FOF";
    }
    return badge_threat_category_code(item->category);
}

static uint32_t ui_domain_count(const badge_threat_snapshot_t *snapshot,
                                badge_ui_domain_t domain)
{
    if (!snapshot) return 0;
    switch (domain) {
        case BADGE_UI_DOMAIN_DRONE:
            return snapshot->active_counts[BADGE_THREAT_DRONE];
        case BADGE_UI_DOMAIN_WIFI:
            return snapshot->active_counts[BADGE_THREAT_WIFI_ANOMALY];
        case BADGE_UI_DOMAIN_PRIVACY:
        default:
            return snapshot->active_counts[BADGE_THREAT_META] +
                   snapshot->active_counts[BADGE_THREAT_TRACKER] +
                   snapshot->active_counts[BADGE_THREAT_BLE] +
                   snapshot->active_counts[BADGE_THREAT_OTHER];
    }
}

static float ui_domain_score(const badge_threat_snapshot_t *snapshot,
                             badge_ui_domain_t domain)
{
    if (!snapshot) return 0.0f;
    switch (domain) {
        case BADGE_UI_DOMAIN_DRONE:
            return snapshot->class_scores[BADGE_THREAT_DRONE];
        case BADGE_UI_DOMAIN_WIFI:
            return snapshot->class_scores[BADGE_THREAT_WIFI_ANOMALY];
        case BADGE_UI_DOMAIN_PRIVACY:
        default: {
            float score = snapshot->class_scores[BADGE_THREAT_META];
            if (snapshot->class_scores[BADGE_THREAT_TRACKER] > score) {
                score = snapshot->class_scores[BADGE_THREAT_TRACKER];
            }
            if (snapshot->class_scores[BADGE_THREAT_BLE] > score) {
                score = snapshot->class_scores[BADGE_THREAT_BLE];
            }
            if (snapshot->class_scores[BADGE_THREAT_OTHER] > score) {
                score = snapshot->class_scores[BADGE_THREAT_OTHER];
            }
            return score;
        }
    }
}

static uint16_t ui_domain_color(const badge_threat_snapshot_t *snapshot,
                                badge_ui_domain_t domain)
{
    float score = ui_domain_score(snapshot, domain);
    if (score <= 0.0f && ui_domain_count(snapshot, domain) == 0) {
        return COL_LINK_DARK;
    }
    return badge_threat_score_to_rgb565(score);
}

static uint16_t ui_domain_base_color(badge_ui_domain_t domain)
{
    switch (domain) {
        case BADGE_UI_DOMAIN_DRONE:   return COL_GOLD;
        case BADGE_UI_DOMAIN_PRIVACY: return COL_ROSE;
        case BADGE_UI_DOMAIN_WIFI:    return COL_CYAN;
        default:                      return COL_SOFT_GREEN;
    }
}

static uint16_t ui_category_base_color(badge_threat_category_t category)
{
    switch (category) {
        case BADGE_THREAT_CATEGORY_DRONE:     return COL_GOLD;
        case BADGE_THREAT_CATEGORY_SSID:      return COL_YELLOW;
        case BADGE_THREAT_CATEGORY_FLOCK:     return COL_VIOLET;
        case BADGE_THREAT_CATEGORY_GLASS:     return COL_ROSE;
        case BADGE_THREAT_CATEGORY_SKIM:      return COL_SKIM_RED;
        case BADGE_THREAT_CATEGORY_CAMERA:    return COL_SKIM_RED;
        case BADGE_THREAT_CATEGORY_BEACON:    return rgb565_scale_color(COL_CYAN, 82);
        case BADGE_THREAT_CATEGORY_EVENT_BADGE: return rgb565_scale_color(COL_VIOLET, 120);
        case BADGE_THREAT_CATEGORY_LOCK:      return COL_GOLD;
        case BADGE_THREAT_CATEGORY_HID:       return COL_CYAN;
        case BADGE_THREAT_CATEGORY_AUDIO:     return rgb565_scale_color(COL_LINK_BRIGHT, 90);
        case BADGE_THREAT_CATEGORY_WIFI:      return COL_CYAN;
        case BADGE_THREAT_CATEGORY_TAG_CLOSE: return rgb565_scale_color(COL_ROSE, 105);
        case BADGE_THREAT_CATEGORY_PRIVACY:   return COL_ROSE;
        default:                              return COL_SOFT_GREEN;
    }
}

static uint16_t ui_domain_deep_color(badge_ui_domain_t domain)
{
    switch (domain) {
        case BADGE_UI_DOMAIN_DRONE:   return COL_DEEP_GOLD;
        case BADGE_UI_DOMAIN_PRIVACY: return COL_DEEP_ROSE;
        case BADGE_UI_DOMAIN_WIFI:    return COL_DEEP_CYAN;
        default:                      return 0x0108;
    }
}

static uint16_t ui_category_deep_color(badge_threat_category_t category)
{
    switch (category) {
        case BADGE_THREAT_CATEGORY_DRONE:     return COL_DEEP_GOLD;
        case BADGE_THREAT_CATEGORY_SSID:      return COL_GOLD_DARK;
        case BADGE_THREAT_CATEGORY_FLOCK:     return COL_DEEP_VIOLET;
        case BADGE_THREAT_CATEGORY_GLASS:     return COL_DEEP_ROSE;
        case BADGE_THREAT_CATEGORY_SKIM:      return COL_DEEP_SKIM;
        case BADGE_THREAT_CATEGORY_CAMERA:    return COL_DEEP_SKIM;
        case BADGE_THREAT_CATEGORY_BEACON:    return COL_DEEP_CYAN;
        case BADGE_THREAT_CATEGORY_EVENT_BADGE: return COL_DEEP_VIOLET;
        case BADGE_THREAT_CATEGORY_LOCK:      return COL_GOLD_DARK;
        case BADGE_THREAT_CATEGORY_HID:       return COL_DEEP_CYAN;
        case BADGE_THREAT_CATEGORY_AUDIO:     return COL_DEEP_CYAN;
        case BADGE_THREAT_CATEGORY_WIFI:      return COL_DEEP_CYAN;
        case BADGE_THREAT_CATEGORY_TAG_CLOSE: return rgb565_scale_color(COL_DEEP_ROSE, 88);
        case BADGE_THREAT_CATEGORY_PRIVACY:   return COL_DEEP_ROSE;
        default:                              return 0x0108;
    }
}

static badge_ui_domain_t ui_dominant_domain(const badge_threat_snapshot_t *snapshot)
{
    if (!snapshot) {
        return BADGE_UI_DOMAIN_PRIVACY;
    }
    if (snapshot->entity_count > 0) {
        return ui_domain_for_category(snapshot->entities[0].category);
    }
    if (snapshot->dominant_class != BADGE_THREAT_IGNORE) {
        return ui_domain_for_class(snapshot->dominant_class);
    }
    if (snapshot->active_counts[BADGE_THREAT_META] > 0 ||
        snapshot->active_counts[BADGE_THREAT_TRACKER] > 0) {
        return BADGE_UI_DOMAIN_PRIVACY;
    }
    float privacy = ui_domain_score(snapshot, BADGE_UI_DOMAIN_PRIVACY);
    float drone = ui_domain_score(snapshot, BADGE_UI_DOMAIN_DRONE);
    float wifi = ui_domain_score(snapshot, BADGE_UI_DOMAIN_WIFI);
    if (privacy >= drone && privacy >= wifi && privacy > 0.0f) {
        return BADGE_UI_DOMAIN_PRIVACY;
    }
    if (drone >= wifi && drone > 0.0f) {
        return BADGE_UI_DOMAIN_DRONE;
    }
    if (wifi > 0.0f) {
        return BADGE_UI_DOMAIN_WIFI;
    }
    return BADGE_UI_DOMAIN_PRIVACY;
}

static void draw_threat_background(const badge_threat_snapshot_t *snapshot)
{
    const badge_threat_snapshot_entity_t *primary =
        (snapshot && snapshot->entity_count > 0) ? &snapshot->entities[0] : NULL;
    badge_ui_domain_t domain = primary ? ui_domain_for_category(primary->category)
                                       : ui_dominant_domain(snapshot);
    float score = primary ? (float)primary->score : (snapshot ? snapshot->threat_score : 0.0f);
    if (score < 0.0f) score = 0.0f;
    if (score > 100.0f) score = 100.0f;

    uint16_t deep = primary ? ui_category_deep_color(primary->category)
                            : ui_domain_deep_color(domain);
    uint16_t base = primary ? ui_category_base_color(primary->category)
                            : ui_domain_base_color(domain);
    badge_threat_proximity_t prox = primary ? primary->proximity_level
                                            : (snapshot ? snapshot->dominant_proximity
                                                        : BADGE_THREAT_PROX_UNKNOWN);
    if (prox == BADGE_THREAT_PROX_CLOSE) {
        base = rgb565_mix_color(base, COL_RED, 112);
        deep = rgb565_mix_color(deep, COL_DIMRED, 92);
        if (score < 88.0f) score = 88.0f;
    } else if (prox == BADGE_THREAT_PROX_NEARBY && score < 72.0f) {
        score = 72.0f;
    } else if (prox == BADGE_THREAT_PROX_PRESENT && score < 45.0f) {
        score = 45.0f;
    }
    uint8_t mix = (uint8_t)(26 + (score * 0.72f));
    uint16_t bg = rgb565_mix_color(rgb565_scale_color(deep, 120),
                                   rgb565_scale_color(base, 70),
                                   mix);
    fb_fill_rect(0, 0, LCD_W, LCD_H, bg);

    uint16_t band = rgb565_mix_color(bg, rgb565_scale_color(base, 110), 52);
    for (int y = 0; y < LCD_H; y += 8) {
        fb_fill_rect(0, y, LCD_W, 1, band);
    }
    uint16_t mark = rgb565_mix_color(bg, rgb565_scale_color(COL_GOLD_DARK, 72), 26);
    draw_triforce_flat_scaled(LCD_W / 2, 82, 118, 104, -4, mark);
    if (prox == BADGE_THREAT_PROX_CLOSE) {
        uint16_t hot = rgb565_mix_color(base, COL_WHITE, 52);
        fb_fill_rect(0, 14, LCD_W, 2, hot);
        fb_fill_rect(0, LCD_H - 3, LCD_W, 3, hot);
    }
    fb_fill_rect(0, 0, LCD_W, 15, rgb565_mix_color(bg, COL_BLACK, 92));
}

static const char *proximity_label(badge_threat_proximity_t prox)
{
    switch (prox) {
        case BADGE_THREAT_PROX_CLOSE:   return "CLOSE";
        case BADGE_THREAT_PROX_NEARBY:  return "NEAR";
        case BADGE_THREAT_PROX_PRESENT: return "PRES";
        default:                        return "SIG";
    }
}

static bool lcd_hex_digit(char ch)
{
    return (ch >= '0' && ch <= '9') ||
           (ch >= 'a' && ch <= 'f') ||
           (ch >= 'A' && ch <= 'F');
}

static bool lcd_text_looks_raw_id(const char *text)
{
    if (!text || text[0] == '\0') {
        return false;
    }
    if (strncmp(text, "probe_", 6) == 0 ||
        strncmp(text, "STA:", 4) == 0 ||
        strncmp(text, "AP:", 3) == 0 ||
        strncmp(text, "FP:", 3) == 0) {
        return true;
    }

    int hex_run = 0;
    int colon_count = 0;
    for (const char *p = text; *p; p++) {
        if (lcd_hex_digit(*p)) {
            hex_run++;
        } else {
            hex_run = 0;
        }
        if (*p == ':') {
            colon_count++;
        }
        if (hex_run >= 8 || colon_count >= 2) {
            return true;
        }
    }
    return false;
}

typedef struct {
    bool active;
    badge_ui_domain_t domain;
    char label[24];
    char detail[44];
    char stat[12];
    bool severe;
} badge_display_diag_t;

#define BADGE_DISPLAY_VIEWED_MAX 8

typedef struct {
    char keys[BADGE_DISPLAY_VIEWED_MAX][BADGE_THREAT_VIEW_KEY_LEN];
    size_t count;
} badge_display_viewed_t;

static bool badge_viewed_key_seen(const badge_display_viewed_t *viewed,
                                  const char *key)
{
    if (!viewed || !key || key[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < viewed->count; i++) {
        if (strcmp(viewed->keys[i], key) == 0) {
            return true;
        }
    }
    return false;
}

static void badge_viewed_mark_key(badge_display_viewed_t *viewed,
                                  const char *key)
{
    if (!viewed || !key || key[0] == '\0' ||
        badge_viewed_key_seen(viewed, key) ||
        viewed->count >= BADGE_DISPLAY_VIEWED_MAX) {
        return;
    }
    snprintf(viewed->keys[viewed->count],
             sizeof(viewed->keys[viewed->count]),
             "%s",
             key);
    viewed->count++;
}

static bool badge_viewed_entity_seen(
    const badge_display_viewed_t *viewed,
    const badge_threat_snapshot_entity_t *item)
{
    if (!viewed || !item) {
        return false;
    }
    char key[BADGE_THREAT_VIEW_KEY_LEN] = {0};
    if (!badge_threat_snapshot_entity_view_key(item, key, sizeof(key))) {
        return false;
    }
    return badge_viewed_key_seen(viewed, key);
}

static void badge_viewed_mark_entity(
    badge_display_viewed_t *viewed,
    const badge_threat_snapshot_entity_t *item)
{
    if (!viewed || !item) {
        return;
    }
    char key[BADGE_THREAT_VIEW_KEY_LEN] = {0};
    if (badge_threat_snapshot_entity_view_key(item, key, sizeof(key))) {
        badge_viewed_mark_key(viewed, key);
    }
    if (badge_threat_snapshot_entity_is_meta_glasses(item)) {
        badge_viewed_mark_key(viewed, "META:*");
    }
}

static bool badge_viewed_has_remote_id(
    const badge_display_viewed_t *viewed)
{
    if (!viewed) {
        return false;
    }
    for (size_t i = 0; i < viewed->count; i++) {
        if (strncmp(viewed->keys[i], "RID:", 4) == 0) {
            return true;
        }
    }
    return false;
}

static bool badge_viewed_has_meta_glasses(
    const badge_display_viewed_t *viewed)
{
    if (!viewed) {
        return false;
    }
    for (size_t i = 0; i < viewed->count; i++) {
        if (strncmp(viewed->keys[i], "META:", 5) == 0) {
            return true;
        }
    }
    return false;
}

static void badge_status_view_key(char *out, size_t out_len,
                                  badge_ui_domain_t domain,
                                  const char *label,
                                  const char *detail,
                                  const char *stat)
{
    if (!out || out_len == 0) {
        return;
    }
    snprintf(out, out_len, "STAT:%d:%s:%s:%s",
             (int)domain,
             label ? label : "",
             detail ? detail : "",
             stat ? stat : "");
}

static bool badge_viewed_status_seen(const badge_display_viewed_t *viewed,
                                     badge_ui_domain_t domain,
                                     const char *label,
                                     const char *detail,
                                     const char *stat)
{
    char key[BADGE_THREAT_VIEW_KEY_LEN] = {0};
    badge_status_view_key(key, sizeof(key), domain, label, detail, stat);
    return badge_viewed_key_seen(viewed, key);
}

static void badge_viewed_mark_status(badge_display_viewed_t *viewed,
                                     badge_ui_domain_t domain,
                                     const char *label,
                                     const char *detail,
                                     const char *stat)
{
    char key[BADGE_THREAT_VIEW_KEY_LEN] = {0};
    badge_status_view_key(key, sizeof(key), domain, label, detail, stat);
    badge_viewed_mark_key(viewed, key);
}

static void fb_draw_string_fit(int x, int y, const char *text, int max_w,
                               uint16_t fg, uint16_t bg)
{
    if (!text || max_w <= 0) return;
    int max_chars = (max_w + 1) / 6;
    if (max_chars <= 0) return;

    char buf[32];
    int n = 0;
    while (text[n] && n < (int)sizeof(buf) - 1 && n < max_chars) {
        buf[n] = text[n];
        n++;
    }
    buf[n] = '\0';
    if (text[n] && n >= 2) {
        buf[n - 1] = '.';
    }
    fb_draw_string(x, y, buf, fg, bg, 1);
}

static void fb_draw_string_fit_scaled(int x, int y, const char *text, int max_w,
                                      uint16_t fg, uint16_t bg, int scale)
{
    if (!text || max_w <= 0 || scale <= 0) return;
    int max_chars = (max_w + scale) / (6 * scale);
    if (max_chars <= 0) return;

    char buf[32];
    int n = 0;
    while (text[n] && n < (int)sizeof(buf) - 1 && n < max_chars) {
        buf[n] = text[n];
        n++;
    }
    buf[n] = '\0';
    if (text[n] && n >= 2) {
        buf[n - 1] = '.';
    }
    fb_draw_string(x, y, buf, fg, bg, scale);
}

static void build_marquee_window(char *out, size_t out_len,
                                 const char *text, int max_chars,
                                 uint32_t frame, uint32_t step_chars,
                                 uint32_t step_frames,
                                 bool uppercase)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!text || max_chars <= 0) {
        return;
    }

    size_t text_len = strlen(text);
    size_t visible = (size_t)max_chars;
    if (visible > out_len - 1) {
        visible = out_len - 1;
    }
    if (visible == 0) {
        return;
    }

    if (text_len <= visible) {
        size_t n = text_len < visible ? text_len : visible;
        for (size_t i = 0; i < n; i++) {
            char ch = text[i];
            out[i] = uppercase && ch >= 'a' && ch <= 'z'
                ? (char)(ch - 'a' + 'A')
                : ch;
        }
        out[n] = '\0';
        return;
    }

    size_t offset = badge_threat_marquee_offset_rate(text_len, visible,
                                                     frame, step_chars,
                                                     step_frames);
    size_t cycle = text_len + 3U;
    for (size_t i = 0; i < visible; i++) {
        size_t idx = (offset + i) % cycle;
        char ch = idx < text_len ? text[idx] : ' ';
        out[i] = uppercase && ch >= 'a' && ch <= 'z'
            ? (char)(ch - 'a' + 'A')
            : ch;
    }
    out[visible] = '\0';
}

static void fb_draw_string_fast_marquee(int x, int y, const char *text,
                                        int max_w, uint16_t fg, uint16_t bg)
{
    if (!text || max_w <= 0) {
        return;
    }
    int max_chars = (max_w + 1) / 6;
    char buf[40];
    build_marquee_window(buf, sizeof(buf), text, max_chars,
                          s_queue_page_frame,
                          BADGE_LOWER_MARQUEE_CHARS,
                          BADGE_LOWER_MARQUEE_FRAMES,
                          false);
    fb_draw_string(x, y, buf, fg, bg, 1);
}

static uint16_t tiny_glyph_rows(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    switch (c) {
        case '0': return 0x7B6F; /* 111 101 101 101 111 */
        case '1': return 0x2C92; /* 010 110 010 010 111 */
        case '2': return 0x73E7; /* 111 001 111 100 111 */
        case '3': return 0x73CF; /* 111 001 111 001 111 */
        case '4': return 0x5BC9; /* 101 101 111 001 001 */
        case '5': return 0x79CF; /* 111 100 111 001 111 */
        case '6': return 0x79EF; /* 111 100 111 101 111 */
        case '7': return 0x7249; /* 111 001 010 010 010 */
        case '8': return 0x7BEF; /* 111 101 111 101 111 */
        case '9': return 0x7BCF; /* 111 101 111 001 111 */
        case 'A': return 0x5BEA; /* 101 101 111 101 101 */
        case 'B': return 0x7BEE; /* 111 101 110 101 110 */
        case 'C': return 0x798F; /* 111 100 100 100 111 */
        case 'D': return 0x7B6E; /* 111 101 101 101 110 */
        case 'E': return 0x79CF; /* 111 100 111 100 111 */
        case 'F': return 0x79C8; /* 111 100 111 100 100 */
        case 'G': return 0x79AF; /* 111 100 101 101 111 */
        case 'H': return 0x5BEA; /* 101 101 111 101 101 */
        case 'I': return 0x7497; /* 111 010 010 010 111 */
        case 'J': return 0x124F; /* 001 001 001 101 111 */
        case 'K': return 0x5DCA; /* 101 110 100 110 101 */
        case 'L': return 0x4927; /* 100 100 100 100 111 */
        case 'M': return 0x5FFA; /* 101 111 111 101 101 */
        case 'N': return 0x5F6A; /* 101 111 101 101 101 */
        case 'O': return 0x7B6F; /* 111 101 101 101 111 */
        case 'P': return 0x7BE8; /* 111 101 111 100 100 */
        case 'Q': return 0x7B7B; /* 111 101 101 111 011 */
        case 'R': return 0x7BEA; /* 111 101 111 101 101 */
        case 'S': return 0x79CF; /* 111 100 111 001 111 */
        case 'T': return 0x7492; /* 111 010 010 010 010 */
        case 'U': return 0x5B6F; /* 101 101 101 101 111 */
        case 'V': return 0x5B54; /* 101 101 101 010 010 */
        case 'W': return 0x5BFF; /* 101 101 111 111 101 */
        case 'X': return 0x5D5A; /* 101 101 010 101 101 */
        case 'Y': return 0x5D52; /* 101 101 010 010 010 */
        case 'Z': return 0x72A7; /* 111 001 010 100 111 */
        case '-': return 0x01C0; /* 000 000 111 000 000 */
        case ':': return 0x0A00; /* 000 010 000 010 000 */
        case '.': return 0x0002; /* 000 000 000 000 010 */
        case '/': return 0x12A4; /* 001 001 010 100 100 */
        case '_': return 0x0007; /* 000 000 000 000 111 */
        case ' ': return 0x0000;
        default:  return 0x01C0;
    }
}

static int tiny_pixel_width(const char *s)
{
    int n = 0;
    while (s && *s++) n++;
    return n > 0 ? n * 4 - 1 : 0;
}

static int fb_draw_tiny_string(int x, int y, const char *s,
                               uint16_t fg, uint16_t bg)
{
    int x0 = x;
    while (s && *s) {
        uint16_t rows = tiny_glyph_rows(*s++);
        for (int row = 0; row < 5; row++) {
            uint8_t bits = (uint8_t)((rows >> ((4 - row) * 3)) & 0x7);
            for (int col = 0; col < 3; col++) {
                uint16_t color = (bits & (1 << (2 - col))) ? fg : bg;
                if (color != bg || fg != bg) {
                    fb_set_pixel(x + col, y + row, color);
                }
            }
        }
        x += 4;
    }
    return x - x0;
}

static void fb_draw_tiny_string_fit(int x, int y, const char *text, int max_w,
                                    uint16_t fg, uint16_t bg)
{
    if (!text || max_w <= 0) return;
    int max_chars = (max_w + 1) / 4;
    if (max_chars <= 0) return;
    char buf[48];
    int n = 0;
    while (text[n] && n < (int)sizeof(buf) - 1 && n < max_chars) {
        char ch = text[n];
        buf[n] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
        n++;
    }
    buf[n] = '\0';
    if (text[n] && n >= 2) {
        buf[n - 1] = '.';
    }
    fb_draw_tiny_string(x, y, buf, fg, bg);
}

static int ui_domain_entity_count(const badge_threat_snapshot_t *snapshot,
                                  badge_ui_domain_t domain)
{
    if (!snapshot) return 0;
    int count = 0;
    for (int i = 0; i < snapshot->entity_count; i++) {
        if (ui_domain_for_class(snapshot->entities[i].cls) == domain) {
            count++;
        }
    }
    return count;
}

static const badge_threat_snapshot_entity_t *ui_domain_entity_at(
    const badge_threat_snapshot_t *snapshot,
    badge_ui_domain_t domain,
    int pos)
{
    if (!snapshot || pos < 0) return NULL;
    int seen = 0;
    for (int i = 0; i < snapshot->entity_count; i++) {
        const badge_threat_snapshot_entity_t *item = &snapshot->entities[i];
        if (ui_domain_for_class(item->cls) != domain) {
            continue;
        }
        if (seen == pos) {
            return item;
        }
        seen++;
    }
    return NULL;
}

static const char *clear_label_for_domain(badge_ui_domain_t domain,
                                          bool ble_scanner_ok,
                                          bool wifi_scanner_ok,
                                          bool backend_ok,
                                          bool wifi_network_ok)
{
    switch (domain) {
        case BADGE_UI_DOMAIN_DRONE:
            return ble_scanner_ok || wifi_scanner_ok ? "No drone" : "Wait scanner";
        case BADGE_UI_DOMAIN_WIFI:
            if (!wifi_scanner_ok) return "WiFi scanner?";
            if (backend_ok) return "Backend ok";
            if (wifi_network_ok) return "WiFi ok";
            return "AP local";
        case BADGE_UI_DOMAIN_PRIVACY:
        default:
            return ble_scanner_ok ? "No privacy" : "BLE scanner?";
    }
}

static void draw_signal_lane(int y, const badge_threat_snapshot_t *snapshot,
                             badge_ui_domain_t domain,
                             bool ble_scanner_ok, bool wifi_scanner_ok,
                             bool backend_ok, bool wifi_network_ok)
{
    uint16_t color = ui_domain_color(snapshot, domain);
    int visible_count = ui_domain_entity_count(snapshot, domain);
    const badge_threat_snapshot_entity_t *item = NULL;
    if (visible_count > 0) {
        int pos = (int)((s_queue_page_frame / 28) % (uint32_t)visible_count);
        item = ui_domain_entity_at(snapshot, domain, pos);
    }

    float score = ui_domain_score(snapshot, domain);
    int bar_w = (int)(score * 68.0f / 100.0f);
    if (bar_w < 0) bar_w = 0;
    if (bar_w > 68) bar_w = 68;

    fb_fill_rect(0, y, LCD_W, 32, COL_PANEL_2);
    fb_fill_rect(0, y, 3, 32, color);
    fb_draw_string(6, y + 3, ui_domain_code(domain), color, COL_PANEL_2, 1);

    const char *label = item
        ? item->label
        : clear_label_for_domain(domain, ble_scanner_ok, wifi_scanner_ok,
                                 backend_ok, wifi_network_ok);
    int label_x = domain == BADGE_UI_DOMAIN_WIFI ? 34 : 28;

    char meta[14];
    if (item && item->rssi < 0) {
        snprintf(meta, sizeof(meta), "%ddBm", item->rssi);
    } else {
        snprintf(meta, sizeof(meta), "s%02d", item ? item->score : (int)(score + 0.5f));
    }
    int mw = str_pixel_width(meta, 1);
    int label_max = LCD_W - label_x - mw - 8;
    if (label_max < 36) label_max = 36;
    fb_draw_string_fit(label_x, y + 3, label, label_max, COL_WHITE, COL_PANEL_2);
    fb_draw_string(LCD_W - mw - 3, y + 3, meta, COL_GRAY, COL_PANEL_2, 1);

    char detail[32];
    if (item && item->detail[0] != '\0') {
        snprintf(detail, sizeof(detail), "%s", item->detail);
    } else if (item) {
        snprintf(detail, sizeof(detail), "seen %lu age %ds",
                 (unsigned long)item->event_count, item->age_s);
    } else {
        snprintf(detail, sizeof(detail), "score %02d", (int)(score + 0.5f));
    }
    char stat[12];
    if (item) {
        snprintf(stat, sizeof(stat), "s%02d n%lu", item->score,
                 (unsigned long)item->event_count);
        int sw = str_pixel_width(stat, 1);
        int detail_max = LCD_W - label_x - sw - 8;
        if (detail_max < 36) detail_max = 36;
        fb_draw_string_fit(label_x, y + 14, detail, detail_max, COL_GRAY, COL_PANEL_2);
        fb_draw_string(LCD_W - sw - 3, y + 14, stat, color, COL_PANEL_2, 1);
    } else {
        fb_draw_string_fit(label_x, y + 14, detail, 82, COL_GRAY, COL_PANEL_2);
    }

    fb_fill_rect(28, y + 28, 68, 2, COL_DARKGRAY);
    if (bar_w > 0) {
        fb_fill_rect(28, y + 28, bar_w, 2, color);
    }
    if (visible_count > 1) {
        char page[5];
        int pos = (int)((s_queue_page_frame / 28) % (uint32_t)visible_count) + 1;
        snprintf(page, sizeof(page), "%d/%d", pos, visible_count);
        int pw = str_pixel_width(page, 1);
        fb_draw_string(LCD_W - pw - 3, y + 24, page, COL_DARKGRAY, COL_PANEL_2, 1);
    }
}

static void draw_signal_lanes(const badge_threat_snapshot_t *snapshot,
                              bool ble_scanner_ok, bool wifi_scanner_ok,
                              bool backend_ok, bool wifi_network_ok)
{
    fb_fill_rect(0, 58, LCD_W, 102, COL_BLACK);
    draw_signal_lane(58, snapshot, BADGE_UI_DOMAIN_DRONE,
                     ble_scanner_ok, wifi_scanner_ok, backend_ok, wifi_network_ok);
    draw_signal_lane(92, snapshot, BADGE_UI_DOMAIN_PRIVACY,
                     ble_scanner_ok, wifi_scanner_ok, backend_ok, wifi_network_ok);
    draw_signal_lane(126, snapshot, BADGE_UI_DOMAIN_WIFI,
                     ble_scanner_ok, wifi_scanner_ok, backend_ok, wifi_network_ok);
}

static void draw_triforce_watermark(uint16_t drone_color,
                                    uint16_t privacy_color,
                                    uint16_t wifi_color)
{
    uint16_t drone_dim = rgb565_scale_color(drone_color, 92);
    uint16_t privacy_dim = rgb565_scale_color(privacy_color, 92);
    uint16_t wifi_dim = rgb565_scale_color(wifi_color, 92);
    uint16_t edge = rgb565_scale_color(COL_LINK_GREEN, 72);

    /* Tiny header TriForce: drone top, privacy left, Wi-Fi right. */
    fb_fill_triangle(118, 1, 114, 7, 122, 7, drone_dim);
    fb_fill_triangle(110, 12, 114, 7, 118, 12, privacy_dim);
    fb_fill_triangle(122, 7, 126, 12, 118, 12, wifi_dim);

    fb_draw_line(118, 1, 114, 7, edge);
    fb_draw_line(118, 1, 122, 7, edge);
    fb_draw_line(110, 12, 114, 7, edge);
    fb_draw_line(126, 12, 122, 7, edge);
    fb_draw_line(110, 12, 126, 12, edge);
}

static void draw_domain_chip(int x, int y, int w,
                             const badge_threat_snapshot_t *snapshot,
                             badge_ui_domain_t domain)
{
    uint16_t color = ui_domain_color(snapshot, domain);
    float score = ui_domain_score(snapshot, domain);
    int bar_w = (int)(score * (float)(w - 4) / 100.0f);
    if (bar_w < 0) bar_w = 0;
    if (bar_w > w - 4) bar_w = w - 4;

    uint16_t bg = rgb565_mix_color(rgb565_scale_color(color, 44), COL_BLACK, 120);
    fb_fill_rect(x, y, w, 11, bg);
    fb_fill_rect(x, y, 2, 11, color);
    fb_draw_tiny_string(x + 4, y + 2, ui_domain_code(domain), color, bg);

    char meta[8];
    snprintf(meta, sizeof(meta), "%02d", (int)(score + 0.5f));
    int mw = tiny_pixel_width(meta);
    fb_draw_tiny_string(x + w - mw - 3, y + 2, meta, COL_GRAY, bg);

    fb_fill_rect(x + 2, y + 9, w - 4, 2, rgb565_scale_color(color, 50));
    if (bar_w > 0) {
        fb_fill_rect(x + 2, y + 9, bar_w, 2, color);
    }
}

static uint16_t diag_color(const badge_display_diag_t *diag)
{
    if (!diag || !diag->active) {
        return COL_SOFT_GREEN;
    }
    uint16_t color = ui_domain_base_color(diag->domain);
    if (diag->severe) {
        color = rgb565_mix_color(color, COL_RED, 120);
    }
    return color;
}

static void format_coord_pair(char *out, size_t out_len, double lat, double lon)
{
    if (!out || out_len == 0) {
        return;
    }
    char ns = lat >= 0.0 ? 'N' : 'S';
    char ew = lon >= 0.0 ? 'E' : 'W';
    double alat = lat >= 0.0 ? lat : -lat;
    double alon = lon >= 0.0 ? lon : -lon;
    snprintf(out, out_len, "%.4f%c %.4f%c", alat, ns, alon, ew);
}

static void draw_primary_alert_row(const badge_threat_snapshot_t *snapshot,
                                   const badge_display_diag_t *diag)
{
    const int y0 = 14;
    const int h0 = 40;
    const badge_threat_snapshot_entity_t *item =
        (snapshot && snapshot->entity_count > 0) ? &snapshot->entities[0] : NULL;
    badge_ui_domain_t domain = item ? ui_domain_for_category(item->category)
                                    : (diag && diag->active ? diag->domain
                                                            : BADGE_UI_DOMAIN_PRIVACY);
    uint16_t color = item ? ui_category_base_color(item->category) : diag_color(diag);
    if (item && item->proximity_level == BADGE_THREAT_PROX_CLOSE) {
        color = rgb565_mix_color(color, COL_RED, 112);
    }
    uint16_t bg = rgb565_mix_color(rgb565_scale_color(color, 54), COL_BLACK, 92);

    fb_fill_rect(0, y0, LCD_W, h0, bg);
    fb_fill_rect(0, y0, 4, h0, color);

    if (!item) {
        const char *label = (diag && diag->active) ? diag->label : "Watching";
        const char *detail = (diag && diag->active) ? diag->detail
                             : "RID FLOCK GLASS SKIM";
        const char *code = ui_domain_code(domain);
        fb_draw_tiny_string(7, y0 + 7, code, color, bg);
        int label_x = 7 + tiny_pixel_width(code) + 5;
        fb_draw_string_fit(label_x, y0 + 6, label, LCD_W - label_x - 8,
                           COL_WHITE, bg);
        if (diag && diag->active && diag->stat[0]) {
            int sw = tiny_pixel_width(diag->stat);
            fb_draw_tiny_string(LCD_W - sw - 4, y0 + 20, diag->stat, color, bg);
            fb_draw_tiny_string_fit(7, y0 + 20, detail, LCD_W - sw - 14,
                                    COL_GRAY, bg);
        } else {
            fb_draw_tiny_string_fit(7, y0 + 20, detail, LCD_W - 12, COL_GRAY, bg);
        }
        return;
    }

    const char *code = ui_item_code(item);
    fb_draw_tiny_string(7, y0 + 7, code, color, bg);
    int label_x = 7 + tiny_pixel_width(code) + 5;
    char age[16];
    if (item->stale) {
        snprintf(age, sizeof(age), "last %dm", item->age_s / 60);
    } else if (item->age_s >= 60) {
        snprintf(age, sizeof(age), "%dm", item->age_s / 60);
    } else {
        snprintf(age, sizeof(age), "%ds", item->age_s);
    }
    int aw = tiny_pixel_width(age);
    fb_draw_string_fit(label_x, y0 + 6, item->label,
                       LCD_W - label_x - aw - 8, COL_WHITE, bg);
    fb_draw_tiny_string(LCD_W - aw - 4, y0 + 8, age,
                        item->stale ? COL_GRAY : color, bg);

    char summary[48];
    if (item->has_location) {
        format_coord_pair(summary, sizeof(summary), item->latitude, item->longitude);
    } else if (item->rssi < 0) {
        snprintf(summary, sizeof(summary), "%s RSSI %d seen %lu",
                 proximity_label(item->proximity_level),
                 item->best_rssi,
                 (unsigned long)item->seen_count);
    } else {
        snprintf(summary, sizeof(summary), "seen %lu score %02d",
                 (unsigned long)item->seen_count, item->score);
    }
    fb_draw_tiny_string_fit(7, y0 + 20, summary, LCD_W - 12,
                            item->stale ? COL_DARKGRAY : COL_GRAY, bg);
    if (item->has_operator_location) {
        char op_line[48];
        char coords[32];
        format_coord_pair(coords, sizeof(coords),
                          item->operator_lat, item->operator_lon);
        if (item->operator_id[0]) {
            snprintf(op_line, sizeof(op_line), "op %.10s %s",
                     item->operator_id, coords);
        } else {
            snprintf(op_line, sizeof(op_line), "op %s", coords);
        }
        fb_draw_tiny_string_fit(7, y0 + 31, op_line, LCD_W - 12,
                                item->stale ? COL_DARKGRAY : COL_GRAY, bg);
    } else if (item->detail[0]) {
        fb_draw_tiny_string_fit(7, y0 + 31, item->detail, LCD_W - 12,
                                item->stale ? COL_DARKGRAY : COL_GRAY, bg);
    }
}

static void draw_queue_row(int y, int h,
                           const badge_threat_snapshot_entity_t *item)
{
    uint16_t color = item->stale ? COL_GRAY : ui_category_base_color(item->category);
    if (!item->stale && item->proximity_level == BADGE_THREAT_PROX_CLOSE) {
        color = rgb565_mix_color(color, COL_RED, 112);
    }
    const char *code = ui_item_code(item);
    uint16_t bg = item->stale ? rgb565_scale_color(COL_PANEL_2, 92) : COL_PANEL_2;

    fb_fill_rect(0, y, LCD_W, h, bg);
    fb_fill_rect(0, y, 5, h, color);

    char title[40];
    snprintf(title, sizeof(title), "%s %s", code, item->label);
    fb_draw_string_fit(8, y + 4, title, LCD_W - 12,
                       item->stale ? COL_GRAY : COL_WHITE, bg);

    char detail[20] = {0};
    if (item->detail[0]) {
        const char *src = item->detail;
        if (strncmp(src, "model ", 6) == 0) src += 6;
        else if (strncmp(src, "self ", 5) == 0) src += 5;
        else if (strncmp(src, "ssid ", 5) == 0) src += 5;
        else if (strncmp(src, "op ", 3) == 0) src += 3;
        if (!lcd_text_looks_raw_id(src)) {
            snprintf(detail, sizeof(detail), "%.9s", src);
        }
    }

    char age[8];
    if (item->age_s >= 60) {
        snprintf(age, sizeof(age), "%dm", item->age_s / 60);
    } else {
        snprintf(age, sizeof(age), "%ds", item->age_s);
    }

    char line[48];
    if (item->rssi < 0) {
        const char *prox = item->proximity_level == BADGE_THREAT_PROX_CLOSE ? "CLOSE" :
                           item->proximity_level == BADGE_THREAT_PROX_NEARBY ? "NEAR" :
                           item->proximity_level == BADGE_THREAT_PROX_PRESENT ? "PRES" : "";
        if (detail[0]) {
            snprintf(line, sizeof(line), "%.7s %ddB seen %lu",
                     detail, item->best_rssi, (unsigned long)item->seen_count);
        } else if (prox[0]) {
            snprintf(line, sizeof(line), "%s %ddB seen %lu",
                     prox, item->best_rssi, (unsigned long)item->seen_count);
        } else {
            snprintf(line, sizeof(line), "%ddB seen %lu %s",
                     item->best_rssi, (unsigned long)item->seen_count, age);
        }
    } else {
        int count = 0;
        const char *p = item->detail[0] ? strstr(item->detail, "count:") : NULL;
        if (p) {
            count = atoi(p + 6);
        }
        if (count > 0) {
            snprintf(line, sizeof(line), "events %d %s", count, age);
        } else if (item->seen_count > 1) {
            snprintf(line, sizeof(line), "seen %lu %s",
                     (unsigned long)item->seen_count, age);
        } else if (detail[0]) {
            snprintf(line, sizeof(line), "%.12s %s", detail, age);
        } else {
            snprintf(line, sizeof(line), "age %s", age);
        }
    }
    fb_draw_string_fit(8, y + 18, line, LCD_W - 12,
                       item->stale ? COL_DARKGRAY : COL_GRAY, bg);
}

static void draw_diag_row(int y, int h, const badge_display_diag_t *diag)
{
    if (!diag || !diag->active) {
        return;
    }
    uint16_t color = diag_color(diag);
    uint16_t bg = diag->severe
        ? rgb565_mix_color(COL_PANEL_2, COL_DIMRED, 72)
        : rgb565_scale_color(COL_PANEL_2, 96);
    fb_fill_rect(0, y, LCD_W, h, bg);
    fb_fill_rect(0, y, 5, h, color);
    bool show_stat = diag->stat[0] != '\0' &&
                     strcmp(diag->stat, "BLE") != 0 &&
                     strcmp(diag->stat, "WiFi") != 0 &&
                     strcmp(diag->stat, "SSID") != 0 &&
                     strcmp(diag->stat, "RID") != 0 &&
                     strcmp(diag->stat, "OK") != 0;
    int label_x = 9;
    int sw = show_stat ? tiny_pixel_width(diag->stat) : 0;
    int label_max = LCD_W - label_x - (sw > 0 ? sw + 9 : 5);
    if (label_max < 40) label_max = 40;
    fb_draw_string_fast_marquee(label_x, y + 4, diag->label, label_max,
                                diag->severe ? COL_WHITE : COL_GRAY, bg);
    if (show_stat) {
        fb_draw_tiny_string(LCD_W - sw - 4, y + 6, diag->stat, color, bg);
    }
    if (diag->detail[0]) {
        fb_draw_string_fast_marquee(label_x, y + 19, diag->detail,
                                    LCD_W - label_x - 5,
                                    diag->severe ? COL_GOLD : COL_DARKGRAY,
                                    bg);
    }
}

static void draw_idle_row(int y, int h, const char *label, const char *detail)
{
    uint16_t bg = rgb565_scale_color(COL_PANEL, 86);
    fb_fill_rect(0, y, LCD_W, h, bg);
    fb_fill_rect(0, y, 3, h, COL_LINK_DARK);
    fb_draw_tiny_string(6, y + 1, "SA", COL_LINK_DARK, bg);
    fb_draw_string_fit(18, y + 1, label, LCD_W - 23, COL_GRAY, bg);
    fb_draw_tiny_string_fit(18, y + 9, detail, LCD_W - 23,
                            COL_DARKGRAY, bg);
}

static uint32_t cap3(uint32_t value)
{
    return value > 999 ? 999 : value;
}

static bool parse_semver3(const char *version, int *major, int *minor, int *patch)
{
    if (!version) {
        return false;
    }
    if (version[0] == 'v') {
        version++;
    }
    int a = 0;
    int b = 0;
    int c = 0;
    if (sscanf(version, "%d.%d.%d", &a, &b, &c) != 3) {
        return false;
    }
    if (major) *major = a;
    if (minor) *minor = b;
    if (patch) *patch = c;
    return true;
}

static bool scanner_version_is_old(const scanner_info_t *info)
{
    if (!info || !info->received || info->version[0] == '\0') {
        return false;
    }
    int amaj = 0, amin = 0, apat = 0;
    int emaj = 0, emin = 0, epat = 0;
    if (parse_semver3(info->version, &amaj, &amin, &apat) &&
        parse_semver3(FOF_VERSION, &emaj, &emin, &epat)) {
        if (amaj != emaj) return amaj < emaj;
        if (amin != emin) return amin < emin;
        return apat < epat;
    }
    const char *actual = info->version[0] == 'v' ? info->version + 1 : info->version;
    const char *expected = FOF_VERSION[0] == 'v' ? FOF_VERSION + 1 : FOF_VERSION;
    return strcmp(actual, expected) != 0;
}

static const char *ble_proof_label(const scanner_info_t *ble,
                                   bool connected,
                                   bool raw_seen,
                                   bool role_ok,
                                   bool cmd_fresh,
                                   bool old)
{
    if (!connected) return raw_seen ? "frame" : "no";
    if (old) return "old";
    if (!role_ok || !cmd_fresh) return "wait";
    if (!ble || !ble->ble_scanning) return "off";
    if (ble->ble_adv_seen == 0) return "quiet";
    return "ok";
}

static const char *wifi_proof_label(bool connected,
                                    bool raw_seen,
                                    bool role_ok,
                                    bool cmd_fresh,
                                    bool old)
{
    if (!connected) return raw_seen ? "frame" : "no";
    if (old) return "old";
    if (!role_ok || !cmd_fresh) return "wait";
    return "ok";
}

static bool scanner_status_ssid_fresh(const char *ssid, int64_t age_s)
{
    return badge_threat_status_ssid_is_fresh(ssid, age_s);
}

static bool scanner_status_drone_ssid_fresh(const scanner_info_t *info)
{
    return scanner_status_ssid_fresh(
        info ? info->wifi_last_drone_ssid : NULL,
        info ? info->wifi_last_drone_ssid_age_s : -1
    );
}

static bool scanner_status_notable_ssid_fresh(const scanner_info_t *info)
{
    return scanner_status_ssid_fresh(
        info ? info->wifi_last_notable_ssid : NULL,
        info ? info->wifi_last_notable_ssid_age_s : -1
    );
}

static bool scanner_status_meta_fresh(const scanner_info_t *info)
{
    if (!info ||
        info->ble_meta_seen == 0 ||
        info->ble_meta_last_seen_age_s < 0 ||
        info->ble_meta_last_seen_age_s > 90 ||
        info->ble_meta_last_hash == 0) {
        return false;
    }
    if (strstr(info->ble_meta_last_reason, "weak_meta") != NULL) {
        return false;
    }
    return strcmp(info->ble_meta_identity, "strong_fp") == 0 ||
           strcmp(info->ble_meta_identity, "detector_fp") == 0;
}

static uint32_t scanner_status_max_u32(uint32_t a, uint32_t b)
{
    return a > b ? a : b;
}

static const scanner_info_t *scanner_status_freshest_drone_ssid_info(void)
{
    const scanner_info_t *infos[2] = {
        uart_rx_get_ble_scanner_info(),
        uart_rx_get_wifi_scanner_info(),
    };
    const scanner_info_t *best = NULL;
    int64_t best_age = -1;
    for (int i = 0; i < 2; i++) {
        const scanner_info_t *info = infos[i];
        if (!scanner_status_drone_ssid_fresh(info)) {
            continue;
        }
        if (best_age < 0 || info->wifi_last_drone_ssid_age_s < best_age) {
            best = info;
            best_age = info->wifi_last_drone_ssid_age_s;
        }
    }
    return best;
}

static const scanner_info_t *scanner_status_freshest_notable_ssid_info(void)
{
    const scanner_info_t *infos[2] = {
        uart_rx_get_ble_scanner_info(),
        uart_rx_get_wifi_scanner_info(),
    };
    const scanner_info_t *best = NULL;
    int64_t best_age = -1;
    for (int i = 0; i < 2; i++) {
        const scanner_info_t *info = infos[i];
        if (!scanner_status_notable_ssid_fresh(info)) {
            continue;
        }
        if (best_age < 0 || info->wifi_last_notable_ssid_age_s < best_age) {
            best = info;
            best_age = info->wifi_last_notable_ssid_age_s;
        }
    }
    return best;
}

static const scanner_info_t *scanner_status_freshest_meta_info(void)
{
    const scanner_info_t *infos[2] = {
        uart_rx_get_ble_scanner_info(),
        uart_rx_get_wifi_scanner_info(),
    };
    const scanner_info_t *best = NULL;
    int64_t best_age = -1;
    for (int i = 0; i < 2; i++) {
        const scanner_info_t *info = infos[i];
        if (!scanner_status_meta_fresh(info)) {
            continue;
        }
        if (best_age < 0 || info->ble_meta_last_seen_age_s < best_age) {
            best = info;
            best_age = info->ble_meta_last_seen_age_s;
        }
    }
    return best;
}

static uint32_t scanner_status_rid_seen_max(void)
{
    const scanner_info_t *ble = uart_rx_get_ble_scanner_info();
    const scanner_info_t *wifi = uart_rx_get_wifi_scanner_info();
    return scanner_status_max_u32(ble ? ble->rid_emit : 0U,
                                  wifi ? wifi->rid_emit : 0U);
}

static uint32_t scanner_status_ble_tracker_seen_max(void)
{
    const scanner_info_t *ble = uart_rx_get_ble_scanner_info();
    const scanner_info_t *wifi = uart_rx_get_wifi_scanner_info();
    return scanner_status_max_u32(ble ? ble->ble_tracker_seen : 0U,
                                  wifi ? wifi->ble_tracker_seen : 0U);
}

static bool scanner_status_wifi_anomaly_summary(uint32_t *deauth_out,
                                                uint32_t *disassoc_out,
                                                bool *beacon_out)
{
    const scanner_info_t *ble = uart_rx_get_ble_scanner_info();
    const scanner_info_t *wifi = uart_rx_get_wifi_scanner_info();
    uint32_t deauth = scanner_status_max_u32(ble ? ble->deauth_count : 0U,
                                             wifi ? wifi->deauth_count : 0U);
    uint32_t disassoc = scanner_status_max_u32(ble ? ble->disassoc_count : 0U,
                                               wifi ? wifi->disassoc_count : 0U);
    bool beacon = (ble && ble->beacon_spam) || (wifi && wifi->beacon_spam);
    if (deauth_out) *deauth_out = deauth;
    if (disassoc_out) *disassoc_out = disassoc;
    if (beacon_out) *beacon_out = beacon;
    return deauth > 0 || disassoc > 0 || beacon;
}

static const scanner_info_t *scanner_status_best_ble_live_info(uint32_t *payload_out,
                                                               uint32_t *empty_out)
{
    const scanner_info_t *infos[2] = {
        uart_rx_get_ble_scanner_info(),
        uart_rx_get_wifi_scanner_info(),
    };
    const scanner_info_t *best = NULL;
    uint32_t payload = 0;
    uint32_t empty = 0;
    for (int i = 0; i < 2; i++) {
        const scanner_info_t *info = infos[i];
        if (!info || info->ble_any_seen == 0 || info->ble_any_best_rssi >= 0) {
            continue;
        }
        payload = scanner_status_max_u32(payload, info->ble_any_with_payload_seen);
        empty = scanner_status_max_u32(empty, info->ble_any_empty_seen);
        if (!best || info->ble_any_best_rssi > best->ble_any_best_rssi) {
            best = info;
        }
    }
    if (payload_out) *payload_out = payload;
    if (empty_out) *empty_out = empty;
    return best;
}

static bool badge_text_has_value(const char *s)
{
    return s && s[0] != '\0' && strcmp(s, "?") != 0;
}

static bool badge_text_contains_nocase(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !needle[0]) {
        return false;
    }
    size_t needle_len = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < needle_len && p[i]) {
            char a = p[i];
            char b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) {
                break;
            }
            i++;
        }
        if (i == needle_len) {
            return true;
        }
    }
    return false;
}

static bool badge_ble_label_is_generic(const char *label)
{
    return !badge_text_has_value(label) ||
           strcmp(label, "BLE") == 0 ||
           strcmp(label, "BLE Nearby") == 0 ||
           badge_text_contains_nocase(label, "unknown");
}

static void format_ble_signal_status(const scanner_info_t *info,
                                     char *label_out,
                                     size_t label_len,
                                     char *detail_out,
                                     size_t detail_len)
{
    if (!label_out || label_len == 0 || !detail_out || detail_len == 0) {
        return;
    }
    snprintf(label_out, label_len, "BLE SIGNAL");
    detail_out[0] = '\0';
    if (!info) {
        snprintf(detail_out, detail_len, "strong BLE");
        return;
    }

    const char *guess = info->ble_dbg_near_label;
    const char *name = info->ble_dbg_near_name;
    const char *reason = info->ble_dbg_near_reason;
    int rssi = info->ble_dbg_near_rssi < 0
        ? info->ble_dbg_near_rssi
        : info->ble_any_best_rssi;

    if (!badge_ble_label_is_generic(guess)) {
        if (badge_text_contains_nocase(guess, "tracker") ||
            badge_text_contains_nocase(guess, "airtag") ||
            badge_text_contains_nocase(guess, "tile") ||
            badge_text_contains_nocase(guess, "tag")) {
            snprintf(label_out, label_len, "TRACKER?");
        } else if (badge_text_contains_nocase(guess, "hid") ||
                   badge_text_contains_nocase(guess, "keyboard") ||
                   badge_text_contains_nocase(guess, "mouse")) {
            snprintf(label_out, label_len, "HID?");
        } else if (badge_text_contains_nocase(guess, "camera")) {
            snprintf(label_out, label_len, "CAMERA?");
        } else if (badge_text_contains_nocase(guess, "lock")) {
            snprintf(label_out, label_len, "LOCK?");
        } else {
            snprintf(label_out, label_len, "%.23s", guess);
        }
    }

    char hint[28] = {0};
    if (badge_text_has_value(name)) {
        snprintf(hint, sizeof(hint), "%.20s", name);
    } else if (!badge_ble_label_is_generic(guess)) {
        snprintf(hint, sizeof(hint), "%.20s", guess);
    } else if (info->ble_dbg_near_svc0 != 0) {
        snprintf(hint, sizeof(hint), "svc %04X", (unsigned)info->ble_dbg_near_svc0);
    } else if (info->ble_dbg_near_cid != 0) {
        snprintf(hint, sizeof(hint), "cid %04X", (unsigned)info->ble_dbg_near_cid);
    } else if (badge_text_has_value(reason)) {
        snprintf(hint, sizeof(hint), "%.20s", reason);
    } else {
        snprintf(hint, sizeof(hint), "unknown BLE");
    }

    if (badge_text_has_value(name) && info->ble_dbg_near_svc0 != 0) {
        snprintf(detail_out, detail_len, "%.18s svc%04X %ddB",
                 name, (unsigned)info->ble_dbg_near_svc0, rssi);
    } else if (rssi < 0) {
        snprintf(detail_out, detail_len, "%s %ddB", hint, rssi);
    } else {
        snprintf(detail_out, detail_len, "%s active", hint);
    }
}

static void draw_scanner_health_line(int y, bool ble_scanner_ok,
                                     bool wifi_scanner_ok)
{
    const scanner_info_t *ble = uart_rx_get_ble_scanner_info();
    const scanner_info_t *wifi = uart_rx_get_wifi_scanner_info();
    scanner_uart_diag_t ble_uart = {0};
    scanner_uart_diag_t wifi_uart = {0};
    uart_rx_get_scanner_uart_diag(0, &ble_uart);
    uart_rx_get_scanner_uart_diag(1, &wifi_uart);
    const char *ble_expected = wifi_scanner_ok ? "ble_primary" : "hybrid_failover";
    const char *wifi_expected = ble_scanner_ok ? "wifi_primary" : "hybrid_failover";
    const char *ble_actual = (ble && ble->scan_profile[0]) ? ble->scan_profile : "";
    const char *wifi_actual = (wifi && wifi->scan_profile[0]) ? wifi->scan_profile : "";
    bool ble_role_ok = ble_scanner_ok && strcmp(ble_actual, ble_expected) == 0;
    bool wifi_role_ok = wifi_scanner_ok && strcmp(wifi_actual, wifi_expected) == 0;
    bool ble_old = ble_scanner_ok && scanner_version_is_old(ble);
    bool wifi_old = wifi_scanner_ok && scanner_version_is_old(wifi);
    bool ble_cmd_fresh = ble_scanner_ok && ble &&
                         (ble->cmd_rx_count > 0 &&
                          ble->cmd_last_age_s >= 0 && ble->cmd_last_age_s <= 30);
    bool wifi_cmd_fresh = wifi_scanner_ok && wifi &&
                          (wifi->cmd_rx_count > 0 &&
                           wifi->cmd_last_age_s >= 0 && wifi->cmd_last_age_s <= 30);

    const char *ble_state = ble_proof_label(ble, ble_scanner_ok, ble_uart.raw_seen, ble_role_ok,
                                            ble_cmd_fresh, ble_old);
    const char *wifi_state = wifi_proof_label(wifi_scanner_ok, wifi_uart.raw_seen, wifi_role_ok,
                                              wifi_cmd_fresh, wifi_old);
    const scanner_info_t *meta = scanner_status_freshest_meta_info();
    uint32_t tracker_seen = scanner_status_ble_tracker_seen_max();
    uint32_t drone_rid_seen = scanner_status_rid_seen_max();
    uint32_t deauth = 0;
    uint32_t disassoc = 0;
    bool beacon = false;
    bool wifi_anom = scanner_status_wifi_anomaly_summary(&deauth, &disassoc, &beacon);
    char ble_hint[12];
    if (meta) {
        snprintf(ble_hint, sizeof(ble_hint), "m%llds",
                 (long long)meta->ble_meta_last_seen_age_s);
    } else if (tracker_seen > 0) {
        snprintf(ble_hint, sizeof(ble_hint), "tag");
    } else {
        snprintf(ble_hint, sizeof(ble_hint), "scan");
    }
    char wifi_hint[12];
    if (drone_rid_seen > 0) {
        snprintf(wifi_hint, sizeof(wifi_hint), "rid");
    } else if (wifi_anom) {
        snprintf(wifi_hint, sizeof(wifi_hint), "atk");
    } else {
        snprintf(wifi_hint, sizeof(wifi_hint), "scan");
    }
    char line[48];
    const scanner_info_t *ssid_info = scanner_status_freshest_drone_ssid_info();
    if (!ssid_info) {
        ssid_info = scanner_status_freshest_notable_ssid_info();
    }
    if (ssid_info) {
        const char *ssid = scanner_status_drone_ssid_fresh(ssid_info)
            ? ssid_info->wifi_last_drone_ssid
            : ssid_info->wifi_last_notable_ssid;
        snprintf(line, sizeof(line), "B %s %s W %s %.13s",
                 ble_state, ble_hint, wifi_state, ssid);
    } else {
        snprintf(line, sizeof(line), "B %s %s W %s %s",
                 ble_state, ble_hint, wifi_state, wifi_hint);
    }
    bool proof_ok = ble_role_ok && wifi_role_ok && ble_cmd_fresh && wifi_cmd_fresh &&
                    !ble_old && !wifi_old && strcmp(ble_state, "quiet") != 0 &&
                    strcmp(ble_state, "off") != 0 &&
                    strcmp(ble_state, "notag") != 0;
    fb_draw_tiny_string_fit(4, y, line, LCD_W - 27,
                            proof_ok ? COL_SOFT_GREEN : COL_GOLD,
                            COL_BLACK);
}

static void add_diag_row(badge_display_diag_t *rows, int max_rows, int *count,
                         badge_ui_domain_t domain, const char *label,
                         const char *detail, const char *stat, bool severe)
{
    if (!rows || !count || *count >= max_rows) {
        return;
    }
    badge_display_diag_t *row = &rows[*count];
    memset(row, 0, sizeof(*row));
    row->active = true;
    row->domain = domain;
    row->severe = severe;
    snprintf(row->label, sizeof(row->label), "%s", label ? label : "Scanner");
    snprintf(row->detail, sizeof(row->detail), "%s", detail ? detail : "");
    snprintf(row->stat, sizeof(row->stat), "%s", stat ? stat : "");
    (*count)++;
}

static int build_scanner_diag_rows(badge_display_diag_t *rows, int max_rows,
                                   bool ble_scanner_ok, bool wifi_scanner_ok,
                                   bool show_ok_rows)
{
    if (!rows || max_rows <= 0) {
        return 0;
    }
    int count = 0;
    const scanner_info_t *ble = uart_rx_get_ble_scanner_info();
    const scanner_info_t *wifi = uart_rx_get_wifi_scanner_info();
    scanner_uart_diag_t ble_uart = {0};
    scanner_uart_diag_t wifi_uart = {0};
    uart_rx_get_scanner_uart_diag(0, &ble_uart);
    uart_rx_get_scanner_uart_diag(1, &wifi_uart);
    bool ble_old = ble_scanner_ok && scanner_version_is_old(ble);
    bool wifi_old = wifi_scanner_ok && scanner_version_is_old(wifi);
    bool ble_cmd_fresh = ble_scanner_ok && ble &&
                         (ble->cmd_rx_count > 0 &&
                          ble->cmd_last_age_s >= 0 && ble->cmd_last_age_s <= 30);
    bool wifi_cmd_fresh = wifi_scanner_ok && wifi &&
                          (wifi->cmd_rx_count > 0 &&
                           wifi->cmd_last_age_s >= 0 && wifi->cmd_last_age_s <= 30);
    const char *ble_expected = wifi_scanner_ok ? "ble_primary" : "hybrid_failover";
    const char *wifi_expected = ble_scanner_ok ? "wifi_primary" : "hybrid_failover";
    const char *ble_actual = (ble && ble->scan_profile[0]) ? ble->scan_profile : "";
    const char *wifi_actual = (wifi && wifi->scan_profile[0]) ? wifi->scan_profile : "";
    bool ble_role_ok = ble_scanner_ok && strcmp(ble_actual, ble_expected) == 0;
    bool wifi_role_ok = wifi_scanner_ok && strcmp(wifi_actual, wifi_expected) == 0;
    const char *ble_state = ble_proof_label(ble, ble_scanner_ok, ble_uart.raw_seen, ble_role_ok,
                                            ble_cmd_fresh, ble_old);
    const char *wifi_state = wifi_proof_label(wifi_scanner_ok, wifi_uart.raw_seen, wifi_role_ok,
                                              wifi_cmd_fresh, wifi_old);

    char detail[44];
    if (!ble_scanner_ok && ble_uart.raw_seen) {
        add_diag_row(rows, max_rows, &count, BADGE_UI_DOMAIN_PRIVACY,
                     "BLE data bad", "Flash scanner", "FIX", true);
    } else if (!ble_scanner_ok) {
        add_diag_row(rows, max_rows, &count, BADGE_UI_DOMAIN_PRIVACY,
                     "BLE offline", "Check cable", "FIX", true);
    } else if (strcmp(ble_state, "old") == 0) {
        add_diag_row(rows, max_rows, &count, BADGE_UI_DOMAIN_PRIVACY,
                     "Update BLE", "Flash scanner", "OLD", true);
    } else if (strcmp(ble_state, "wait") == 0) {
        add_diag_row(rows, max_rows, &count, BADGE_UI_DOMAIN_PRIVACY,
                     "BLE starting", "Waiting ack", "WAIT", true);
    } else if (strcmp(ble_state, "off") == 0) {
        snprintf(detail, sizeof(detail), "scan%d sync%d rst%lu",
                 ble ? ble->ble_scan_last_rc : 0,
                 ble ? ble->ble_sync_last_rc : 0,
                 (unsigned long)(ble ? ble->ble_host_restart_count : 0));
        add_diag_row(rows, max_rows, &count, BADGE_UI_DOMAIN_PRIVACY,
                     "BLE radio off", detail, "FIX", true);
    } else if (strcmp(ble_state, "quiet") == 0) {
        add_diag_row(rows, max_rows, &count, BADGE_UI_DOMAIN_PRIVACY,
                     "BLE scanning", "No ads yet", "SCAN", true);
    } else if (strcmp(ble_state, "notag") == 0) {
        snprintf(detail, sizeof(detail), "Seen %lu ads",
                 (unsigned long)cap3(ble ? ble->ble_adv_seen : 0));
        add_diag_row(rows, max_rows, &count, BADGE_UI_DOMAIN_PRIVACY,
                     "No BLE threat", detail, "OK", false);
    } else if (show_ok_rows) {
        const scanner_info_t *meta = scanner_status_freshest_meta_info();
        uint32_t payload = 0;
        const scanner_info_t *ble_live =
            scanner_status_best_ble_live_info(&payload, NULL);
        uint32_t tracker_seen = scanner_status_ble_tracker_seen_max();
        char ble_label_buf[24];
        const char *label = "BLE CLEAR";
        if (meta && meta->ble_focus_active) {
            label = "META SIGNAL";
            snprintf(detail, sizeof(detail), "seen %llds focus %llds",
                     (long long)meta->ble_meta_last_seen_age_s,
                     (long long)meta->ble_focus_age_s);
        } else if (meta && meta->ble_meta_last_rssi < 0) {
            label = "META SIGNAL";
            snprintf(detail, sizeof(detail), "seen %llds  %ddB",
                     (long long)meta->ble_meta_last_seen_age_s,
                     (int)meta->ble_meta_last_rssi);
        } else if (meta) {
            label = "META SIGNAL";
            snprintf(detail, sizeof(detail), "seen %llds ago",
                     (long long)meta->ble_meta_last_seen_age_s);
        } else if (tracker_seen > 0) {
            label = "TAG SIGNAL";
            snprintf(detail, sizeof(detail), "tag signals active");
        } else if (ble_live && ble_live->ble_any_best_rssi >= -60) {
            (void)payload;
            format_ble_signal_status(ble_live, ble_label_buf, sizeof(ble_label_buf),
                                     detail, sizeof(detail));
            label = ble_label_buf;
        } else {
            snprintf(detail, sizeof(detail), "scanning BLE ads");
        }
        add_diag_row(rows, max_rows, &count, BADGE_UI_DOMAIN_PRIVACY,
                     label, detail, "", false);
    }

    if (!wifi_scanner_ok && wifi_uart.raw_seen) {
        add_diag_row(rows, max_rows, &count, BADGE_UI_DOMAIN_WIFI,
                     "WiFi data bad", "Flash scanner", "FIX", true);
    } else if (!wifi_scanner_ok) {
        add_diag_row(rows, max_rows, &count, BADGE_UI_DOMAIN_WIFI,
                     "WiFi offline", "Check cable", "FIX", true);
    } else if (strcmp(wifi_state, "old") == 0) {
        add_diag_row(rows, max_rows, &count, BADGE_UI_DOMAIN_WIFI,
                     "Update WiFi", "Flash scanner", "OLD", true);
    } else if (strcmp(wifi_state, "wait") == 0) {
        add_diag_row(rows, max_rows, &count, BADGE_UI_DOMAIN_WIFI,
                     "WiFi starting", "Waiting ack", "WAIT", true);
    } else if (show_ok_rows) {
        uint32_t deauth = 0;
        uint32_t disassoc = 0;
        bool beacon = false;
        bool wifi_anom = scanner_status_wifi_anomaly_summary(&deauth, &disassoc, &beacon);
        const scanner_info_t *ssid = scanner_status_freshest_drone_ssid_info();
        if (!ssid) {
            ssid = scanner_status_freshest_notable_ssid_info();
        }
        const char *label = "WIFI CLEAR";
        if (wifi_anom) {
            label = "WIFI ATTACK";
            snprintf(detail, sizeof(detail), "deauth %lu disassoc %lu%s",
                     (unsigned long)cap3(deauth),
                     (unsigned long)cap3(disassoc),
                     beacon ? " beacon" : "");
        } else if (ssid) {
            label = scanner_status_drone_ssid_fresh(ssid)
                ? "DRONE SSID"
                : "SSID SEEN";
            const char *name = scanner_status_drone_ssid_fresh(ssid)
                ? ssid->wifi_last_drone_ssid
                : ssid->wifi_last_notable_ssid;
            snprintf(detail, sizeof(detail), "ssid %.31s", name);
        } else {
            snprintf(detail, sizeof(detail), "scanning WiFi frames");
        }
        add_diag_row(rows, max_rows, &count, BADGE_UI_DOMAIN_WIFI,
                     label, detail, "", false);
    }

    return count;
}

static const char *proof_word(const char *state)
{
    if (strcmp(state, "ok") == 0) return "OK";
    if (strcmp(state, "old") == 0) return "OLD";
    if (strcmp(state, "wait") == 0) return "WAIT";
    if (strcmp(state, "off") == 0) return "OFF";
    if (strcmp(state, "quiet") == 0) return "QUIET";
    if (strcmp(state, "frame") == 0) return "BAD";
    if (strcmp(state, "no") == 0) return "NO";
    return "CHECK";
}

static void draw_scanner_bottom_strip(int y, bool ble_scanner_ok,
                                      bool wifi_scanner_ok)
{
    const scanner_info_t *ble = uart_rx_get_ble_scanner_info();
    const scanner_info_t *wifi = uart_rx_get_wifi_scanner_info();
    scanner_uart_diag_t ble_uart = {0};
    scanner_uart_diag_t wifi_uart = {0};
    uart_rx_get_scanner_uart_diag(0, &ble_uart);
    uart_rx_get_scanner_uart_diag(1, &wifi_uart);
    bool ble_old = ble_scanner_ok && scanner_version_is_old(ble);
    bool wifi_old = wifi_scanner_ok && scanner_version_is_old(wifi);
    bool ble_cmd_fresh = ble_scanner_ok && ble &&
                         (ble->cmd_rx_count > 0 &&
                          ble->cmd_last_age_s >= 0 && ble->cmd_last_age_s <= 45);
    bool wifi_cmd_fresh = wifi_scanner_ok && wifi &&
                          (wifi->cmd_rx_count > 0 &&
                           wifi->cmd_last_age_s >= 0 && wifi->cmd_last_age_s <= 45);
    const char *ble_expected = wifi_scanner_ok ? "ble_primary" : "hybrid_failover";
    const char *wifi_expected = ble_scanner_ok ? "wifi_primary" : "hybrid_failover";
    const char *ble_actual = (ble && ble->scan_profile[0]) ? ble->scan_profile : "";
    const char *wifi_actual = (wifi && wifi->scan_profile[0]) ? wifi->scan_profile : "";
    bool ble_role_ok = ble_scanner_ok && strcmp(ble_actual, ble_expected) == 0;
    bool wifi_role_ok = wifi_scanner_ok && strcmp(wifi_actual, wifi_expected) == 0;
    const char *ble_state = ble_proof_label(ble, ble_scanner_ok, ble_uart.raw_seen, ble_role_ok,
                                            ble_cmd_fresh, ble_old);
    const char *wifi_state = wifi_proof_label(wifi_scanner_ok, wifi_uart.raw_seen, wifi_role_ok,
                                              wifi_cmd_fresh, wifi_old);
    bool ok = strcmp(ble_state, "ok") == 0 && strcmp(wifi_state, "ok") == 0;
    uint16_t bg = ok ? rgb565_scale_color(COL_PANEL, 82)
                     : rgb565_mix_color(COL_PANEL_2, COL_DIMRED, 62);
    uint16_t fg = ok ? COL_SOFT_GREEN : COL_GOLD;

    fb_fill_rect(0, y, LCD_W, LCD_H - y, bg);
    fb_fill_rect(0, y, LCD_W, 1, rgb565_scale_color(fg, 110));
    char line[36];
    snprintf(line, sizeof(line), "BLE %s  WIFI %s",
             proof_word(ble_state), proof_word(wifi_state));
    int64_t uptime_s = esp_timer_get_time() / 1000000LL;
    char uptime_label[16];
    if (uptime_s < 3600) {
        snprintf(uptime_label, sizeof(uptime_label), "U%lld:%02lld",
                 (long long)(uptime_s / 60),
                 (long long)(uptime_s % 60));
    } else {
        snprintf(uptime_label, sizeof(uptime_label), "U%lldh%02lld",
                 (long long)(uptime_s / 3600),
                 (long long)((uptime_s / 60) % 60));
    }
    const bool safe_usb = badge_runtime_is_safe_mode();
    const bool usb_alive = badge_runtime_usb_control_alive();
    const char *usb_label = safe_usb ? "SAFE" : (usb_alive ? "USBC" : "USB?");
    char right_label[24];
    snprintf(right_label, sizeof(right_label), "%s %s", uptime_label, usb_label);
    uint16_t usb_color = safe_usb ? COL_GOLD :
                         (usb_alive ? COL_LINK_BRIGHT : COL_DARKGRAY);
    int usb_w = tiny_pixel_width(right_label);
    int left_w = LCD_W - usb_w - 14;
    if (left_w < 40) left_w = 40;
    fb_draw_tiny_string_fit(4, y + 4, line, left_w, fg, bg);
    fb_draw_tiny_string(LCD_W - usb_w - 4, y + 4, right_label, usb_color, bg);
}

static void draw_watch_eye(int cx, int cy, int w, int h, int gaze,
                           bool blink, uint16_t eye_color, uint16_t accent,
                           uint16_t bg)
{
    int left = cx - w / 2;
    int right = cx + w / 2;
    int open_h = blink ? 3 : h;
    int top = cy - open_h / 2;
    int bottom = cy + open_h / 2;

    uint16_t eye_fill = blink ? rgb565_scale_color(accent, 72)
                              : rgb565_mix_color(eye_color, bg, 42);
    uint16_t eye_edge = rgb565_mix_color(accent, COL_WHITE, 48);

    fb_fill_triangle(left, cy, cx, top, right, cy, eye_fill);
    fb_fill_triangle(left, cy, cx, bottom, right, cy, eye_fill);
    fb_draw_line(left, cy, cx, top, eye_edge);
    fb_draw_line(cx, top, right, cy, eye_edge);
    fb_draw_line(left, cy, cx, bottom, rgb565_scale_color(eye_edge, 88));
    fb_draw_line(cx, bottom, right, cy, rgb565_scale_color(eye_edge, 88));

    if (blink) {
        fb_draw_line(left + 3, cy, right - 3, cy, eye_edge);
        return;
    }

    int pupil_x = cx + gaze;
    fb_fill_ellipse(pupil_x, cy, 7, 8, rgb565_scale_color(COL_BLACK, 170));
    fb_fill_ellipse(pupil_x, cy, 4, 5, accent);
    fb_set_pixel(pupil_x - 2, cy - 3, COL_WHITE);
    fb_set_pixel(pupil_x - 1, cy - 3, COL_WHITE);
}

static void draw_empty_watch_state(int y, int h, bool ble_scanner_ok,
                                   bool wifi_scanner_ok)
{
    uint16_t bg = rgb565_mix_color(COL_PANEL, COL_BLACK, 68);
    uint16_t sweep = (ble_scanner_ok && wifi_scanner_ok) ? COL_SOFT_GREEN : COL_GOLD;
    uint16_t eye = rgb565_mix_color(COL_LINK_BRIGHT, COL_WHITE, 36);
    int phase = (int)(s_anim_frame % 96);
    int gaze = phase < 24 ? -8 : phase < 48 ? 0 : phase < 72 ? 8 : 0;
    bool blink = (s_anim_frame % 118) > 111;
    int cy = y + 52;

    fb_fill_rect(0, y, LCD_W, h, rgb565_scale_color(COL_BLACK, 210));
    for (int i = 0; i < 4; i++) {
        int sy = y + 18 + i * 20 + (int)((s_anim_frame / 3 + i * 9) % 16);
        uint16_t c = rgb565_scale_color(sweep, (uint8_t)(35 + i * 18));
        fb_draw_line(14, sy, LCD_W - 15, sy + 9, c);
    }
    fb_fill_rect(8, y + 12, LCD_W - 16, h - 30, bg);
    fb_fill_rect(8, y + 12, LCD_W - 16, 1, rgb565_scale_color(sweep, 90));
    fb_fill_rect(8, y + h - 19, LCD_W - 16, 1, rgb565_scale_color(sweep, 65));

    draw_watch_eye(43, cy, 40, 24, gaze, blink, eye, sweep, bg);
    draw_watch_eye(85, cy, 40, 24, gaze, blink, eye, sweep, bg);

    int dot_phase = (int)((s_anim_frame / 8) % 4);
    for (int i = 0; i < 4; i++) {
        uint16_t c = i == dot_phase ? COL_WHITE : rgb565_scale_color(sweep, 86);
        fb_fill_rect(48 + i * 10, y + 84, 4, 4, c);
    }

    fb_draw_string_centered(LCD_W / 2, y + 98, "WATCHING",
                            COL_WHITE, bg, 2);
    const char *health = (ble_scanner_ok && wifi_scanner_ok)
        ? "SCANNERS LIVE"
        : "CHECK SCANNERS";
    fb_draw_string_centered(LCD_W / 2, y + 121, health,
                            (ble_scanner_ok && wifi_scanner_ok) ? COL_GRAY : COL_GOLD,
                            bg, 1);
}

static bool scanner_status_has_badge_evidence(void)
{
    const scanner_info_t *ble = uart_rx_get_ble_scanner_info();
    const scanner_info_t *wifi = uart_rx_get_wifi_scanner_info();
    const scanner_info_t *infos[2] = {ble, wifi};
    uint32_t ble_evidence = 0;
    for (int i = 0; i < 2; i++) {
        const scanner_info_t *info = infos[i];
        if (!info) {
            continue;
        }
        ble_evidence += scanner_status_meta_fresh(info) ? 1U : 0U;
        ble_evidence += info->ble_tracker_seen > 0 ? 1U : 0U;
        ble_evidence += info->ble_near_unknown_seen > 0 ? 1U : 0U;
    }
    uint32_t deauth = 0;
    uint32_t disassoc = 0;
    bool beacon = false;
    bool wifi_anom = scanner_status_wifi_anomaly_summary(&deauth, &disassoc, &beacon);
    uint32_t wifi_evidence = wifi_anom ? 1U : 0U;
    if (scanner_status_freshest_drone_ssid_info()) {
        wifi_evidence++;
    }
    if (scanner_status_freshest_notable_ssid_info()) {
        wifi_evidence++;
    }
    return ble_evidence > 0 || wifi_evidence > 0;
}

static bool badge_item_is_drone_ssid(
    const badge_threat_snapshot_entity_t *item)
{
    return item &&
           item->cls == BADGE_THREAT_DRONE &&
           item->category == BADGE_THREAT_CATEGORY_SSID;
}

static bool badge_item_is_meta_glasses(
    const badge_threat_snapshot_entity_t *item)
{
    return badge_threat_snapshot_entity_is_meta_glasses(item);
}

static bool badge_item_is_weak_meta_glasses(
    const badge_threat_snapshot_entity_t *item)
{
    return badge_item_is_meta_glasses(item) && item->display_id[0] == '\0';
}

static bool badge_item_is_remote_id_drone(
    const badge_threat_snapshot_entity_t *item);
static bool badge_item_is_drone_evidence(
    const badge_threat_snapshot_entity_t *item);
static uint32_t badge_remote_id_drone_count(
    const badge_threat_snapshot_t *snapshot);
static uint32_t badge_drone_ssid_count(
    const badge_threat_snapshot_t *snapshot);
static uint32_t badge_drone_evidence_count(
    const badge_threat_snapshot_t *snapshot);
static uint32_t badge_meta_glasses_count(
    const badge_threat_snapshot_t *snapshot);
static int badge_top_tile_rank(const badge_threat_snapshot_t *snapshot,
                               const badge_threat_snapshot_entity_t *item);
static uint8_t badge_top_item_heat_percent(
    const badge_threat_snapshot_t *snapshot,
    const badge_threat_snapshot_entity_t *item);

static badge_display_min_proximity_t badge_display_policy_entity_prox(
    const badge_threat_snapshot_entity_t *item)
{
    if (!item) {
        return BADGE_DISPLAY_PROX_PRESENT;
    }
    switch (item->proximity_level) {
        case BADGE_THREAT_PROX_CLOSE:  return BADGE_DISPLAY_PROX_CLOSE;
        case BADGE_THREAT_PROX_NEARBY: return BADGE_DISPLAY_PROX_NEAR;
        default:                       return BADGE_DISPLAY_PROX_PRESENT;
    }
}

static badge_display_policy_class_t badge_display_policy_entity_class(
    const badge_threat_snapshot_entity_t *item)
{
    if (!item) {
        return BADGE_DISPLAY_CLASS_SCANNER_STATUS;
    }
    switch (item->category) {
        case BADGE_THREAT_CATEGORY_DRONE:
        case BADGE_THREAT_CATEGORY_SSID:
            return BADGE_DISPLAY_CLASS_DRONE;
        case BADGE_THREAT_CATEGORY_GLASS:
            return BADGE_DISPLAY_CLASS_META;
        case BADGE_THREAT_CATEGORY_TAG_CLOSE:
            return BADGE_DISPLAY_CLASS_TRACKER;
        case BADGE_THREAT_CATEGORY_WIFI:
            return BADGE_DISPLAY_CLASS_WIFI_ATTACK;
        case BADGE_THREAT_CATEGORY_SKIM:
            return BADGE_DISPLAY_CLASS_SKIMMER;
        case BADGE_THREAT_CATEGORY_CAMERA:
            return BADGE_DISPLAY_CLASS_CAMERA;
        case BADGE_THREAT_CATEGORY_FLOCK:
            return BADGE_DISPLAY_CLASS_FLOCK;
        case BADGE_THREAT_CATEGORY_LOCK:
            return BADGE_DISPLAY_CLASS_LOCK;
        case BADGE_THREAT_CATEGORY_HID:
            return BADGE_DISPLAY_CLASS_HID;
        case BADGE_THREAT_CATEGORY_BEACON:
            return BADGE_DISPLAY_CLASS_BEACON;
        case BADGE_THREAT_CATEGORY_EVENT_BADGE:
            return BADGE_DISPLAY_CLASS_EVENT_BADGE;
        case BADGE_THREAT_CATEGORY_AUDIO:
            return BADGE_DISPLAY_CLASS_AURACAST;
        default:
            break;
    }
    switch (item->cls) {
        case BADGE_THREAT_DRONE:
            return BADGE_DISPLAY_CLASS_DRONE;
        case BADGE_THREAT_META:
            return BADGE_DISPLAY_CLASS_META;
        case BADGE_THREAT_TRACKER:
            return BADGE_DISPLAY_CLASS_TRACKER;
        case BADGE_THREAT_WIFI_ANOMALY:
            return BADGE_DISPLAY_CLASS_WIFI_ATTACK;
        default:
            return BADGE_DISPLAY_CLASS_SCANNER_STATUS;
    }
}

static bool badge_display_policy_lane_includes(badge_display_lane_t cfg_lane,
                                               bool top_lane)
{
    if (cfg_lane == BADGE_DISPLAY_LANE_BOTH) {
        return true;
    }
    return top_lane
        ? cfg_lane == BADGE_DISPLAY_LANE_TOP
        : cfg_lane == BADGE_DISPLAY_LANE_LOWER;
}

static bool badge_display_policy_allows_entity_lane(
    const badge_threat_snapshot_entity_t *item,
    bool top_lane)
{
    if (!item) {
        return false;
    }
    badge_display_policy_class_t cls = badge_display_policy_entity_class(item);
    bool safety = false;
    const badge_display_policy_t *policy = badge_display_policy_runtime_get();
    if (!badge_display_policy_allows_class(policy, cls,
                                           badge_display_policy_entity_prox(item),
                                           item->score, &safety)) {
        return false;
    }
    if (safety) {
        return true;
    }
    const badge_display_class_policy_t *cfg = &policy->classes[cls];
    return badge_display_policy_lane_includes(cfg->lane, top_lane);
}

static badge_display_policy_class_t badge_display_policy_status_class(
    badge_ui_domain_t domain, const char *label, bool severe)
{
    (void)severe;
    if (!label) {
        return BADGE_DISPLAY_CLASS_SCANNER_STATUS;
    }
    if (strstr(label, "DRONE")) return BADGE_DISPLAY_CLASS_DRONE;
    if (strstr(label, "DEAUTH") || strstr(label, "DISASSOC") ||
        strstr(label, "BEACON SPAM") || strstr(label, "WIFI ATTACK")) {
        return BADGE_DISPLAY_CLASS_WIFI_ATTACK;
    }
    if (strstr(label, "META")) return BADGE_DISPLAY_CLASS_META;
    if (strstr(label, "TAG") || strstr(label, "TRACKER")) return BADGE_DISPLAY_CLASS_TRACKER;
    if (strstr(label, "SKIM")) return BADGE_DISPLAY_CLASS_SKIMMER;
    if (strstr(label, "FLOCK")) return BADGE_DISPLAY_CLASS_FLOCK;
    if (strstr(label, "CAM")) return BADGE_DISPLAY_CLASS_CAMERA;
    if (strstr(label, "LOCK")) return BADGE_DISPLAY_CLASS_LOCK;
    if (strstr(label, "HID")) return BADGE_DISPLAY_CLASS_HID;
    if (strstr(label, "BEACON")) return BADGE_DISPLAY_CLASS_BEACON;
    if (strstr(label, "BADGE")) return BADGE_DISPLAY_CLASS_EVENT_BADGE;
    if (strstr(label, "AUDIO")) return BADGE_DISPLAY_CLASS_AURACAST;
    (void)domain;
    return BADGE_DISPLAY_CLASS_SCANNER_STATUS;
}

static bool badge_display_policy_allows_status_lane(badge_ui_domain_t domain,
                                                    const char *label,
                                                    bool severe)
{
    badge_display_policy_class_t cls =
        badge_display_policy_status_class(domain, label, severe);
    bool safety = false;
    badge_display_min_proximity_t prox = severe
        ? BADGE_DISPLAY_PROX_CLOSE
        : BADGE_DISPLAY_PROX_PRESENT;
    int score = severe ? 85 : 0;
    const badge_display_policy_t *policy = badge_display_policy_runtime_get();
    if (!badge_display_policy_allows_class(policy, cls, prox, score, &safety)) {
        return false;
    }
    if (safety) {
        return true;
    }
    return badge_display_policy_lane_includes(policy->classes[cls].lane, false);
}

static bool badge_top_item_same_view(
    const badge_threat_snapshot_entity_t *a,
    const badge_threat_snapshot_entity_t *b)
{
    if (!a || !b) {
        return false;
    }
    if (a == b) {
        return true;
    }
    char key_a[BADGE_THREAT_VIEW_KEY_LEN] = {0};
    char key_b[BADGE_THREAT_VIEW_KEY_LEN] = {0};
    if (!badge_threat_snapshot_entity_view_key(a, key_a, sizeof(key_a)) ||
        !badge_threat_snapshot_entity_view_key(b, key_b, sizeof(key_b))) {
        return false;
    }
    return strcmp(key_a, key_b) == 0;
}

static bool badge_top_items_conflict(
    const badge_threat_snapshot_entity_t *candidate,
    const badge_threat_snapshot_entity_t *selected)
{
    if (!candidate || !selected) {
        return false;
    }
    if (badge_top_item_same_view(candidate, selected)) {
        return true;
    }
    /* The large drone tile is an aggregate alert. Keep it to one top row, then
     * let the protocol-specific lower rows carry any additional RID/SSID detail. */
    if (badge_item_is_drone_evidence(candidate) &&
        badge_item_is_drone_evidence(selected)) {
        return true;
    }
    return false;
}

static bool badge_top_candidate_allowed(
    const badge_threat_snapshot_entity_t *item)
{
    if (!item || !item->active || item->stale) {
        return false;
    }
    if (badge_threat_snapshot_entity_display_lane(item) ==
        BADGE_THREAT_DISPLAY_LANE_NONE) {
        return false;
    }
    return badge_display_policy_allows_entity_lane(item, true);
}

static bool badge_top_candidate_better(
    const badge_threat_snapshot_t *snapshot,
    const badge_threat_snapshot_entity_t *candidate,
    const badge_threat_snapshot_entity_t *best)
{
    if (!candidate) {
        return false;
    }
    if (!best) {
        return true;
    }
    int candidate_rank = badge_top_tile_rank(snapshot, candidate);
    int best_rank = badge_top_tile_rank(snapshot, best);
    if (candidate_rank != best_rank) {
        return candidate_rank > best_rank;
    }
    uint8_t candidate_heat = badge_top_item_heat_percent(snapshot, candidate);
    uint8_t best_heat = badge_top_item_heat_percent(snapshot, best);
    if (candidate_heat != best_heat) {
        return candidate_heat > best_heat;
    }
    int8_t candidate_rssi = candidate->rssi < 0 ? candidate->rssi
                                                 : candidate->best_rssi;
    int8_t best_rssi = best->rssi < 0 ? best->rssi : best->best_rssi;
    if (candidate_rssi < 0 && best_rssi < 0 && candidate_rssi != best_rssi) {
        return candidate_rssi > best_rssi;
    }
    return candidate->display_rank > best->display_rank;
}

static int badge_top_item_position(
    const badge_threat_snapshot_t *snapshot,
    const badge_threat_snapshot_entity_t *item,
    int *total_out)
{
    if (total_out) {
        *total_out = 1;
    }
    if (!snapshot || !item) {
        return 0;
    }
    if (badge_item_is_drone_evidence(item)) {
        uint32_t count = badge_drone_evidence_count(snapshot);
        if (total_out) {
            *total_out = (int)count;
        }
        return (int)badge_threat_snapshot_entity_ordinal(
            snapshot,
            item,
            BADGE_THREAT_DRONE,
            BADGE_THREAT_CATEGORY_NONE,
            false);
    }
    if (badge_item_is_meta_glasses(item)) {
        uint32_t count = badge_meta_glasses_count(snapshot);
        if (total_out) {
            *total_out = (int)count;
        }
        return (int)badge_threat_snapshot_entity_ordinal(
            snapshot,
            item,
            BADGE_THREAT_META,
            BADGE_THREAT_CATEGORY_GLASS,
            false);
    }
    return 1;
}

static void select_top_global_badge_items(
    const badge_threat_snapshot_t *snapshot,
    const badge_threat_snapshot_entity_t *out_items[2],
    int out_pos[2],
    int out_total[2])
{
    if (out_items) {
        out_items[0] = NULL;
        out_items[1] = NULL;
    }
    if (out_pos) {
        out_pos[0] = 0;
        out_pos[1] = 0;
    }
    if (out_total) {
        out_total[0] = 0;
        out_total[1] = 0;
    }
    if (!snapshot || !out_items) {
        return;
    }

    for (int slot = 0; slot < 2; slot++) {
        const badge_threat_snapshot_entity_t *best = NULL;
        for (int i = 0; i < snapshot->entity_count; i++) {
            const badge_threat_snapshot_entity_t *item = &snapshot->entities[i];
            if (!badge_top_candidate_allowed(item)) {
                continue;
            }
            bool conflicts = false;
            for (int prior = 0; prior < slot; prior++) {
                if (badge_top_items_conflict(item, out_items[prior])) {
                    conflicts = true;
                    break;
                }
            }
            if (conflicts) {
                continue;
            }
            if (badge_top_candidate_better(snapshot, item, best)) {
                best = item;
            }
        }
        out_items[slot] = best;
        if (best) {
            int total = 1;
            int pos = badge_top_item_position(snapshot, best, &total);
            if (out_pos) {
                out_pos[slot] = pos > 0 ? pos : 1;
            }
            if (out_total) {
                out_total[slot] = total > 0 ? total : 1;
            }
        }
    }
}

static badge_ui_domain_t badge_domain_for_display_lane(
    badge_threat_display_lane_t lane)
{
    return lane == BADGE_THREAT_DISPLAY_LANE_WIFI
        ? BADGE_UI_DOMAIN_WIFI
        : BADGE_UI_DOMAIN_PRIVACY;
}

static badge_threat_display_lane_t badge_top_item_lane(
    const badge_threat_snapshot_entity_t *item)
{
    badge_threat_display_lane_t lane =
        badge_threat_snapshot_entity_display_lane(item);
    return lane == BADGE_THREAT_DISPLAY_LANE_NONE
        ? BADGE_THREAT_DISPLAY_LANE_BLE
        : lane;
}

static const char *badge_top_lane_label(badge_threat_display_lane_t lane)
{
    return lane == BADGE_THREAT_DISPLAY_LANE_WIFI ? "WiFi" : "BLE";
}

static const char *badge_top_focus_label(badge_threat_display_lane_t lane)
{
    return lane == BADGE_THREAT_DISPLAY_LANE_WIFI ? "TOP WIFI" : "TOP BLE";
}

static void format_badge_age(char *out, size_t out_len,
                             const badge_threat_snapshot_entity_t *item)
{
    if (!out || out_len == 0) {
        return;
    }
    if (!item) {
        snprintf(out, out_len, "now");
    } else if (item->stale && item->age_s >= 60) {
        snprintf(out, out_len, "last %dm", item->age_s / 60);
    } else if (item->age_s >= 60) {
        snprintf(out, out_len, "%dm", item->age_s / 60);
    } else {
        snprintf(out, out_len, "%ds", item->age_s);
    }
}

static void clean_badge_detail(char *out, size_t out_len,
                               const badge_threat_snapshot_entity_t *item)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!item || item->detail[0] == '\0') {
        return;
    }

    const char *src = item->detail;
    if (strncmp(src, "model ", 6) == 0) src += 6;
    else if (strncmp(src, "self ", 5) == 0) src += 5;
    else if (strncmp(src, "ssid ", 5) == 0 &&
             !(item->cls == BADGE_THREAT_DRONE &&
               item->category == BADGE_THREAT_CATEGORY_SSID)) src += 5;
    else if (strncmp(src, "op ", 3) == 0) src += 3;

    if (!lcd_text_looks_raw_id(src)) {
        snprintf(out, out_len, "%s", src);
    }
}

static bool badge_item_is_remote_id_drone(
    const badge_threat_snapshot_entity_t *item)
{
    return badge_threat_snapshot_entity_is_remote_id_drone(item);
}

static bool badge_item_is_drone_evidence(
    const badge_threat_snapshot_entity_t *item)
{
    return badge_item_is_remote_id_drone(item) || badge_item_is_drone_ssid(item);
}

static uint32_t badge_remote_id_drone_count(
    const badge_threat_snapshot_t *snapshot)
{
    return badge_threat_snapshot_count_active(
        snapshot,
        BADGE_THREAT_DRONE,
        BADGE_THREAT_CATEGORY_DRONE,
        false
    );
}

static uint32_t badge_drone_ssid_count(
    const badge_threat_snapshot_t *snapshot)
{
    return badge_threat_snapshot_count_active(
        snapshot,
        BADGE_THREAT_DRONE,
        BADGE_THREAT_CATEGORY_SSID,
        false
    );
}

static uint32_t badge_drone_evidence_count(
    const badge_threat_snapshot_t *snapshot)
{
    return badge_threat_snapshot_drone_evidence_count(snapshot);
}

static uint32_t badge_meta_glasses_count(
    const badge_threat_snapshot_t *snapshot)
{
    return badge_threat_snapshot_meta_glasses_count(snapshot);
}

static int badge_top_tile_rank(const badge_threat_snapshot_t *snapshot,
                               const badge_threat_snapshot_entity_t *item)
{
    if (!item || !item->active) {
        return 0;
    }

    int base = 0;
    switch (item->category) {
        case BADGE_THREAT_CATEGORY_FLOCK:
            base = 1200;
            break;
        case BADGE_THREAT_CATEGORY_SKIM:
            base = 1160;
            break;
        case BADGE_THREAT_CATEGORY_WIFI:
            base = 1120;
            break;
        case BADGE_THREAT_CATEGORY_CAMERA:
            base = 1080;
            break;
        case BADGE_THREAT_CATEGORY_LOCK:
            base = 980;
            break;
        case BADGE_THREAT_CATEGORY_HID:
            base = 940;
            break;
        case BADGE_THREAT_CATEGORY_TAG_CLOSE:
            base = 920;
            break;
        case BADGE_THREAT_CATEGORY_DRONE:
        case BADGE_THREAT_CATEGORY_SSID:
            base = 900;
            break;
        case BADGE_THREAT_CATEGORY_GLASS:
            base = 700;
            break;
        case BADGE_THREAT_CATEGORY_PRIVACY:
            base = 620;
            break;
        case BADGE_THREAT_CATEGORY_EVENT_BADGE:
            base = 560;
            break;
        case BADGE_THREAT_CATEGORY_BEACON:
            base = 500;
            break;
        case BADGE_THREAT_CATEGORY_AUDIO:
            base = 460;
            break;
        default:
            base = 320;
            break;
    }

    if (badge_item_is_drone_evidence(item)) {
        uint32_t count = badge_drone_evidence_count(snapshot);
        if (count > 1U) {
            base += (int)((count - 1U) * 40U);
        }
    } else if (badge_item_is_meta_glasses(item) &&
               item->display_id[0] == '\0') {
        base -= 140;
    }
    if (item->stale) {
        base -= 500;
    }
    base += item->score;
    base += (int)item->proximity_level * 28;
    base += (int)item->evidence_quality * 4;
    return base;
}

static uint32_t badge_item_heat_count(
    const badge_threat_snapshot_t *snapshot,
    const badge_threat_snapshot_entity_t *item)
{
    if (!item || !item->active || item->stale) {
        return 0;
    }
    if (badge_item_is_drone_evidence(item)) {
        return badge_drone_evidence_count(snapshot);
    }
    if (badge_item_is_meta_glasses(item)) {
        return badge_meta_glasses_count(snapshot);
    }
    if (item->group_count > 1) {
        return item->group_count;
    }
    return 1;
}

static uint8_t badge_item_heat_percent(
    const badge_threat_snapshot_t *snapshot,
    const badge_threat_snapshot_entity_t *item)
{
    if (!item || !item->active || item->stale) {
        return 0;
    }
    return badge_threat_snapshot_entity_heat_percent(
        item,
        badge_item_heat_count(snapshot, item));
}

static uint16_t badge_item_heat_color(
    const badge_threat_snapshot_entity_t *item,
    uint8_t heat)
{
    if (badge_item_is_meta_glasses(item)) {
        return badge_threat_heat_percent_to_rgb565(heat);
    }
    return badge_threat_proximity_percent_to_rgb565(heat);
}

static uint16_t badge_item_heat_color_for_snapshot(
    const badge_threat_snapshot_t *snapshot,
    const badge_threat_snapshot_entity_t *item)
{
    return badge_threat_snapshot_entity_heat_color_rgb565(
        item,
        badge_item_heat_count(snapshot, item));
}

static uint8_t badge_top_item_heat_percent(
    const badge_threat_snapshot_t *snapshot,
    const badge_threat_snapshot_entity_t *item)
{
    if (badge_item_is_drone_evidence(item)) {
        return badge_threat_snapshot_drone_aggregate_heat_percent(snapshot);
    }
    return badge_item_heat_percent(snapshot, item);
}

static uint16_t badge_top_item_heat_color_for_snapshot(
    const badge_threat_snapshot_t *snapshot,
    const badge_threat_snapshot_entity_t *item)
{
    if (badge_item_is_drone_evidence(item)) {
        return badge_threat_snapshot_drone_aggregate_heat_color_rgb565(snapshot);
    }
    return badge_item_heat_color_for_snapshot(snapshot, item);
}

static char badge_ascii_upper(char ch)
{
    return (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
}

static void badge_copy_upper(char *out, size_t out_len, const char *src)
{
    if (!out || out_len == 0) {
        return;
    }
    if (!src) {
        src = "";
    }
    size_t i = 0;
    while (src[i] && i < out_len - 1) {
        out[i] = badge_ascii_upper(src[i]);
        i++;
    }
    out[i] = '\0';
}

static void format_tracker_title_for_lcd(
    char *out,
    size_t out_len,
    const badge_threat_snapshot_entity_t *item)
{
    if (!out || out_len == 0) {
        return;
    }
    const char *label = item ? item->label : NULL;
    if (label && label[0] != '\0' &&
        strcmp(label, "Tracker") != 0 &&
        !lcd_text_looks_raw_id(label)) {
        badge_copy_upper(out, out_len, label);
    } else {
        snprintf(out, out_len, "TRACKER");
    }
}

static void format_badge_title_for_snapshot(
    char *out, size_t out_len,
    const badge_threat_snapshot_entity_t *item,
    const badge_threat_snapshot_t *snapshot)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!item) {
        return;
    }
    if (snapshot && badge_item_is_drone_evidence(item) &&
        badge_drone_evidence_count(snapshot) > 0) {
        badge_threat_format_drone_near_title(snapshot, out, out_len);
        return;
    }

    switch (item->category) {
        case BADGE_THREAT_CATEGORY_GLASS:
            badge_threat_format_meta_glasses_title(out, out_len);
            return;
        case BADGE_THREAT_CATEGORY_FLOCK:
            snprintf(out, out_len, "FLOCK CAM");
            return;
        case BADGE_THREAT_CATEGORY_SKIM:
            snprintf(out, out_len, "SKIMMER");
            return;
        case BADGE_THREAT_CATEGORY_CAMERA:
            snprintf(out, out_len, "CAMERA NEAR");
            return;
        case BADGE_THREAT_CATEGORY_BEACON:
            snprintf(out, out_len, "BEACON AREA");
            return;
        case BADGE_THREAT_CATEGORY_EVENT_BADGE:
            snprintf(out, out_len, "EVENT BADGE");
            return;
        case BADGE_THREAT_CATEGORY_LOCK:
            snprintf(out, out_len, "LOCK NEAR");
            return;
        case BADGE_THREAT_CATEGORY_HID:
            snprintf(out, out_len, "HID NEAR");
            return;
        case BADGE_THREAT_CATEGORY_AUDIO:
            snprintf(out, out_len, "AURACAST");
            return;
        case BADGE_THREAT_CATEGORY_TAG_CLOSE:
            format_tracker_title_for_lcd(out, out_len, item);
            return;
        case BADGE_THREAT_CATEGORY_SSID:
            snprintf(out, out_len, "%s", item->cls == BADGE_THREAT_DRONE
                     ? "DRONE SSID" : "WIFI SSID");
            return;
        case BADGE_THREAT_CATEGORY_DRONE:
            if (badge_item_is_remote_id_drone(item)) {
                badge_threat_format_drone_near_title(snapshot, out, out_len);
            } else {
                snprintf(out, out_len, "DRONE");
            }
            return;
        case BADGE_THREAT_CATEGORY_WIFI:
            snprintf(out, out_len, "WIFI ALERT");
            return;
        case BADGE_THREAT_CATEGORY_PRIVACY:
            snprintf(out, out_len, "PRIVACY");
            return;
        default:
            break;
    }
    badge_copy_upper(out, out_len, item->label);
}

static void format_badge_title(char *out, size_t out_len,
                               const badge_threat_snapshot_entity_t *item)
{
    format_badge_title_for_snapshot(out, out_len, item, NULL);
}

static void format_badge_summary(char *out, size_t out_len,
                                 const badge_threat_snapshot_entity_t *item)
{
    if (!out || out_len == 0) {
        return;
    }
    if (!item) {
        snprintf(out, out_len, "watching");
        return;
    }

    char age[10];
    format_badge_age(age, sizeof(age), item);
    if (item->has_location) {
        char coords[32];
        format_coord_pair(coords, sizeof(coords), item->latitude, item->longitude);
        snprintf(out, out_len, "%s", coords);
    } else if (item->rssi < 0) {
        snprintf(out, out_len, "%s %ddB seen %lu %s",
                 proximity_label(item->proximity_level),
                 item->best_rssi,
                 (unsigned long)item->seen_count,
                 age);
    } else {
        snprintf(out, out_len, "score %02d seen %lu %s",
                 item->score, (unsigned long)item->seen_count, age);
    }
}

static void format_top_badge_detail(char *out, size_t out_len,
                                    const badge_threat_snapshot_entity_t *item,
                                    const badge_threat_snapshot_t *snapshot)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!item) {
        return;
    }

    if (badge_threat_format_top_detail(snapshot, item, out, out_len)) {
        return;
    }

    char clean[32];
    clean_badge_detail(clean, sizeof(clean), item);

    char summary[36];
    format_badge_summary(summary, sizeof(summary), item);
    if (clean[0]) {
        snprintf(out, out_len, "%.18s  %.26s", clean, summary);
    } else {
        snprintf(out, out_len, "%s", summary);
    }
}

static void draw_top_concern_tile(int y, badge_ui_domain_t domain,
                                  const char *lane,
                                  const badge_threat_snapshot_entity_t *item,
                                  int item_pos,
                                  int item_total,
                                  const badge_threat_snapshot_t *snapshot,
                                  bool scanner_ok,
                                  const char *clear_label,
                                  const char *clear_detail)
{
    const int h = 37;
    uint16_t color = item ? ui_category_base_color(item->category)
                          : ui_domain_base_color(domain);
    if (item && item->stale) {
        color = COL_GRAY;
    }
    uint16_t bg = item
        ? rgb565_mix_color(rgb565_scale_color(color, 54), COL_BLACK, 100)
        : rgb565_scale_color(COL_PANEL, scanner_ok ? 82 : 60);
    if (!scanner_ok && !item) {
        bg = rgb565_mix_color(COL_PANEL_2, COL_DIMRED, 58);
    }
    int bar_percent = item ? (int)badge_top_item_heat_percent(snapshot, item) : 0;
    uint16_t bar_color = item ? badge_top_item_heat_color_for_snapshot(snapshot, item)
                              : color;

    fb_fill_rect(0, y, LCD_W, h, bg);
    draw_panel_triforce_mark(y, h, bg, color);
    fb_fill_rect(0, y, 5, h, color);

    if (item) {
        char title[24];
        format_badge_title_for_snapshot(title, sizeof(title), item, snapshot);

        bool show_drone_count = badge_item_is_drone_evidence(item) &&
            badge_drone_evidence_count(snapshot) > 0;
        bool show_meta_count = badge_item_is_meta_glasses(item) &&
            badge_meta_glasses_count(snapshot) > 0;
        char count_text[8] = {0};
        int count_w = 0;
        if (show_drone_count) {
            uint32_t drone_count = badge_drone_evidence_count(snapshot);
            if (drone_count == 0) {
                drone_count = 1;
            }
            snprintf(count_text, sizeof(count_text), "%lu",
                     (unsigned long)drone_count);
            count_w = str_pixel_width(count_text, 3);
        } else if (show_meta_count) {
            uint32_t meta_count = badge_meta_glasses_count(snapshot);
            snprintf(count_text, sizeof(count_text), "%lu",
                     (unsigned long)meta_count);
            count_w = str_pixel_width(count_text, 3);
        }

        int title_max = LCD_W - 11;
        if (count_w > 0) {
            title_max -= count_w + 6;
            if (title_max < 72) {
                title_max = 72;
            }
        }
        int title_scale = str_pixel_width(title, 2) <= title_max ? 2 : 1;
        fb_draw_string_fit_scaled(7, y + (title_scale == 2 ? 4 : 8),
                                  title, title_max,
                                  COL_WHITE, bg, title_scale);
        if (count_w > 0) {
            int count_x = LCD_W - count_w - 5;
            uint16_t count_bg = rgb565_mix_color(bg, COL_BLACK, 138);
            fb_fill_rect(count_x - 2, y + 1, count_w + 5, 23, count_bg);
            fb_draw_string(count_x + 1, y + 3, count_text,
                           rgb565_scale_color(COL_BLACK, 150), count_bg, 3);
            fb_draw_string(count_x, y + 2, count_text,
                           bar_color, count_bg, 3);
        }

        char detail[56];
        format_top_badge_detail(detail, sizeof(detail), item, snapshot);

        char tag[12];
        if (badge_item_is_drone_evidence(item)) {
            uint32_t rid_count = badge_remote_id_drone_count(snapshot);
            uint32_t ssid_count = badge_drone_ssid_count(snapshot);
            if (rid_count > 0 && ssid_count > 0) {
                snprintf(tag, sizeof(tag), "RID+SSID");
            } else if (rid_count > 0) {
                snprintf(tag, sizeof(tag), "RID");
            } else {
                snprintf(tag, sizeof(tag), "SSID");
            }
        } else if (item_total > 1) {
            snprintf(tag, sizeof(tag), "%s %d/%d", lane, item_pos, item_total);
        } else {
            snprintf(tag, sizeof(tag), "%s", lane);
        }
        int tw = tiny_pixel_width(tag);
        int detail_max = LCD_W - 14;
        uint16_t detail_color = item->stale
            ? COL_DARKGRAY
            : rgb565_mix_color(COL_WHITE, bar_color, 84);
        size_t large_chars = (size_t)(detail_max / 6);
        if (badge_threat_top_detail_uses_large_text(detail, large_chars)) {
            fb_draw_string_fit(8, y + 23, detail, detail_max,
                               detail_color, bg);
        } else {
            int tiny_detail_max = LCD_W - 12 - tw - 5;
            if (tiny_detail_max < 64) tiny_detail_max = LCD_W - 12;
            fb_draw_tiny_string_fit(8, y + 24, detail, tiny_detail_max,
                                    detail_color, bg);
            fb_draw_tiny_string(LCD_W - tw - 4, y + 24, tag,
                                rgb565_mix_color(color, bg, 90), bg);
        }
    } else {
        fb_draw_tiny_string(8, y + 5, lane, color, bg);

        char meta[14];
        snprintf(meta, sizeof(meta), "%s", scanner_ok ? "OK" : "CHECK");
        int mw = str_pixel_width(meta, 1);

        fb_draw_string_fit(30, y + 4, clear_label, LCD_W - 35 - mw,
                           COL_GRAY, bg);
        fb_draw_string(LCD_W - mw - 4, y + 4, meta,
                       scanner_ok ? COL_SOFT_GREEN : COL_GOLD, bg, 1);

        char detail[48];
        snprintf(detail, sizeof(detail), "%s", clear_detail ? clear_detail : "");
        fb_draw_string_fit(30, y + 17, detail, LCD_W - 35, COL_DARKGRAY, bg);
    }

    int bar_w = item ? (bar_percent * (LCD_W - 40) / 100) : 0;
    if (bar_w < 0) bar_w = 0;
    if (bar_w > LCD_W - 40) bar_w = LCD_W - 40;
    fb_fill_rect(30, y + 32, LCD_W - 40, 2, rgb565_scale_color(COL_DARKGRAY, 120));
    if (bar_w > 0) {
        fb_fill_rect(30, y + 32, bar_w, 2, bar_color);
    }
}

static bool badge_board_item_visible(const badge_threat_snapshot_entity_t *item,
                                     const badge_threat_snapshot_entity_t *pinned_ble,
                                     const badge_threat_snapshot_entity_t *pinned_wifi,
                                     const badge_display_viewed_t *viewed)
{
    if (!item || !item->active || item->cls == BADGE_THREAT_BLE ||
        item == pinned_ble) {
        return false;
    }
    if (item == pinned_wifi) {
        return false;
    }
    if (badge_viewed_entity_seen(viewed, item)) {
        return false;
    }
    if (badge_item_is_weak_meta_glasses(item)) {
        return false;
    }
    if (badge_item_is_drone_evidence(item) && item->stale) {
        return false;
    }
    return true;
}

static bool badge_board_items_present_same(
    const badge_threat_snapshot_entity_t *a,
    const badge_threat_snapshot_entity_t *b)
{
    if (!a || !b) {
        return false;
    }
    if (a == b) {
        return true;
    }
    if (a->cls != b->cls || a->category != b->category) {
        return false;
    }
    char key_a[BADGE_THREAT_VIEW_KEY_LEN] = {0};
    char key_b[BADGE_THREAT_VIEW_KEY_LEN] = {0};
    if (!badge_threat_snapshot_entity_view_key(a, key_a, sizeof(key_a)) ||
        !badge_threat_snapshot_entity_view_key(b, key_b, sizeof(key_b))) {
        return false;
    }
    return strcmp(key_a, key_b) == 0;
}

static bool badge_board_item_visible_at(
    const badge_threat_snapshot_t *snapshot,
    int index,
    const badge_threat_snapshot_entity_t *pinned_ble,
    const badge_threat_snapshot_entity_t *pinned_wifi,
    const badge_display_viewed_t *viewed)
{
    if (!snapshot || index < 0 || index >= snapshot->entity_count) {
        return false;
    }
    const badge_threat_snapshot_entity_t *item = &snapshot->entities[index];
    if (!badge_board_item_visible(item, pinned_ble, pinned_wifi, viewed) ||
        badge_board_items_present_same(item, pinned_ble) ||
        badge_board_items_present_same(item, pinned_wifi)) {
        return false;
    }
    for (int i = 0; i < index; i++) {
        const badge_threat_snapshot_entity_t *prior = &snapshot->entities[i];
        if (badge_board_item_visible(prior, pinned_ble, pinned_wifi, viewed) &&
            !badge_board_items_present_same(prior, pinned_ble) &&
            !badge_board_items_present_same(prior, pinned_wifi) &&
            badge_board_items_present_same(item, prior)) {
            return false;
        }
    }
    return true;
}

static void draw_billboard_row(int y, int h,
                               const badge_threat_snapshot_t *snapshot,
                               const badge_threat_snapshot_entity_t *item)
{
    uint16_t color = item->stale ? COL_GRAY : ui_category_base_color(item->category);
    uint16_t bg = item->stale ? rgb565_scale_color(COL_PANEL_2, 86) : COL_PANEL_2;
    fb_fill_rect(0, y, LCD_W, h, bg);
    draw_panel_triforce_mark(y, h, bg, color);
    fb_fill_rect(0, y, 5, h, color);

    const char *code = ui_item_code(item);
    fb_draw_tiny_string(8, y + 5, code, color, bg);
    int label_x = 8 + tiny_pixel_width(code) + 6;

    char age[10];
    format_badge_age(age, sizeof(age), item);
    int aw = tiny_pixel_width(age);
    char title[24];
    if (badge_item_is_remote_id_drone(item)) {
        badge_threat_format_drone_entity_title(snapshot, item, title, sizeof(title));
    } else {
        format_badge_title(title, sizeof(title), item);
    }
    if ((item->category == BADGE_THREAT_CATEGORY_BEACON ||
         item->category == BADGE_THREAT_CATEGORY_EVENT_BADGE) &&
        item->group_count > 1U) {
        char counted[24];
        snprintf(counted, sizeof(counted), "%.15s x%lu",
                 title,
                 (unsigned long)item->group_count);
        snprintf(title, sizeof(title), "%s", counted);
    }
    fb_draw_string_fast_marquee(label_x, y + 3, title,
                                LCD_W - label_x - aw - 8,
                                item->stale ? COL_GRAY : COL_WHITE, bg);
    fb_draw_tiny_string(LCD_W - aw - 4, y + 6, age,
                        item->stale ? COL_DARKGRAY : color, bg);

    char detail[80];
    char clean[32];
    if (item->cls == BADGE_THREAT_TRACKER &&
        badge_threat_format_top_detail(snapshot, item, detail, sizeof(detail))) {
        clean[0] = '\0';
    } else if (badge_item_is_remote_id_drone(item) && item->display_id[0]) {
        snprintf(clean, sizeof(clean), "RID #%s", item->display_id);
    } else if (badge_item_is_remote_id_drone(item)) {
        snprintf(clean, sizeof(clean), "RID signal");
    } else if (badge_item_is_meta_glasses(item) && item->display_id[0]) {
        snprintf(clean, sizeof(clean), "META #%s", item->display_id);
    } else {
        clean_badge_detail(clean, sizeof(clean), item);
    }
    if (item->cls != BADGE_THREAT_TRACKER || detail[0] == '\0') {
        char summary[36];
        format_badge_summary(summary, sizeof(summary), item);
        if (clean[0]) {
            snprintf(detail, sizeof(detail), "%.31s  %.36s", clean, summary);
        } else {
            snprintf(detail, sizeof(detail), "%s", summary);
        }
    }
    fb_draw_string_fast_marquee(8, y + 19, detail, LCD_W - 12,
                                item->stale ? COL_DARKGRAY
                                            : rgb565_mix_color(COL_WHITE, color, 85),
                                bg);

    uint8_t heat = badge_item_heat_percent(snapshot, item);
    uint16_t heat_color = badge_item_heat_color(item, heat);
    int heat_w = (int)heat * (LCD_W - 16) / 100;
    fb_fill_rect(8, y + h - 3, LCD_W - 16, 2,
                 rgb565_scale_color(COL_DARKGRAY, 110));
    if (heat_w > 0) {
        fb_fill_rect(8, y + h - 3, heat_w, 2, heat_color);
    }
}

static void draw_idle_billboard(int y, int h,
                                const char *label, const char *detail,
                                bool ok)
{
    uint16_t color = ok ? COL_SOFT_GREEN : COL_GOLD;
    uint16_t bg = ok ? rgb565_scale_color(COL_PANEL, 78)
                     : rgb565_mix_color(COL_PANEL_2, COL_DIMRED, 48);
    fb_fill_rect(0, y, LCD_W, h, bg);
    draw_panel_triforce_mark(y, h, bg, color);
    fb_fill_rect(0, y, 5, h, color);
    fb_draw_string_fit(10, y + 4, label, LCD_W - 16,
                       ok ? COL_GRAY : COL_WHITE, bg);
    fb_draw_string_fit(10, y + 18, detail, LCD_W - 16,
                       ok ? COL_DARKGRAY : COL_GOLD, bg);
}

static bool badge_board_text_visible(const badge_threat_snapshot_t *snapshot,
                                     const badge_threat_snapshot_entity_t *pinned_ble,
                                     const badge_threat_snapshot_entity_t *pinned_wifi,
                                     const badge_display_viewed_t *viewed,
                                     const char *label,
                                     const char *detail)
{
    if (!snapshot || !label || label[0] == '\0') {
        return false;
    }
    for (int i = 0; i < snapshot->entity_count; i++) {
        if (!badge_board_item_visible_at(snapshot, i, pinned_ble, pinned_wifi, viewed)) {
            continue;
        }
        const badge_threat_snapshot_entity_t *item = &snapshot->entities[i];
        char title[24];
        char clean[32];
        format_badge_title(title, sizeof(title), item);
        clean_badge_detail(clean, sizeof(clean), item);
        if (strcmp(title, label) != 0) {
            continue;
        }
        if (!detail || detail[0] == '\0' || clean[0] == '\0' ||
            strcmp(clean, detail) == 0 || strstr(detail, clean) != NULL) {
            return true;
        }
    }
    return false;
}

static bool badge_diag_text_seen(const badge_display_diag_t *rows, int count,
                                 const char *label, const char *detail)
{
    if (!rows || !label) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        if (strcmp(rows[i].label, label) != 0) {
            continue;
        }
        if (!detail || detail[0] == '\0' ||
            strcmp(rows[i].detail, detail) == 0) {
            return true;
        }
    }
    return false;
}

static void add_billboard_status_row(
    badge_display_diag_t *rows, int max_rows, int *count,
    const badge_threat_snapshot_t *snapshot,
    const badge_threat_snapshot_entity_t *pinned_ble,
    const badge_threat_snapshot_entity_t *pinned_wifi,
    badge_display_viewed_t *viewed,
    badge_ui_domain_t domain, const char *label,
    const char *detail, const char *stat, bool severe)
{
    if (!rows || !count || *count >= max_rows || !label || label[0] == '\0') {
        return;
    }
    if (!badge_display_policy_allows_status_lane(domain, label, severe)) {
        return;
    }
    if (badge_diag_text_seen(rows, *count, label, detail) ||
        badge_viewed_status_seen(viewed, domain, label, detail, stat) ||
        badge_board_text_visible(snapshot, pinned_ble, pinned_wifi, viewed, label, detail)) {
        return;
    }
    add_diag_row(rows, max_rows, count, domain, label, detail, stat, severe);
    badge_viewed_mark_status(viewed, domain, label, detail, stat);
}

static bool badge_snapshot_has_live_wifi_anomaly(
    const badge_threat_snapshot_t *snapshot)
{
    if (!snapshot) {
        return false;
    }
    for (int i = 0; i < snapshot->entity_count; i++) {
        const badge_threat_snapshot_entity_t *item = &snapshot->entities[i];
        if (item->active && !item->stale &&
            item->cls == BADGE_THREAT_WIFI_ANOMALY) {
            return true;
        }
    }
    return false;
}

static int build_billboard_status_rows(
    badge_display_diag_t *rows, int max_rows,
    const badge_threat_snapshot_t *snapshot,
    bool ble_scanner_ok, bool wifi_scanner_ok,
    const badge_threat_snapshot_entity_t *pinned_ble,
    const badge_threat_snapshot_entity_t *pinned_wifi,
    badge_display_viewed_t *viewed)
{
    if (!rows || max_rows <= 0) {
        return 0;
    }
    int count = 0;
    char detail[44];

    const scanner_info_t *drone_ssid = scanner_status_freshest_drone_ssid_info();
    const scanner_info_t *notable_ssid = scanner_status_freshest_notable_ssid_info();
    if (drone_ssid && badge_drone_evidence_count(snapshot) > 1) {
        snprintf(detail, sizeof(detail), "ssid %.31s", drone_ssid->wifi_last_drone_ssid);
        add_billboard_status_row(rows, max_rows, &count, snapshot,
                                 pinned_ble, pinned_wifi,
                                 viewed,
                                 BADGE_UI_DOMAIN_WIFI,
                                 "DRONE SSID", detail, "", false);
    } else if (notable_ssid) {
        snprintf(detail, sizeof(detail), "ssid %.31s", notable_ssid->wifi_last_notable_ssid);
        add_billboard_status_row(rows, max_rows, &count, snapshot,
                                 pinned_ble, pinned_wifi,
                                 viewed,
                                 BADGE_UI_DOMAIN_WIFI,
                                 "SSID SEEN", detail, "", false);
    }

    uint32_t deauth = 0;
    uint32_t disassoc = 0;
    bool beacon = false;
    if (scanner_status_wifi_anomaly_summary(&deauth, &disassoc, &beacon) &&
        !badge_snapshot_has_live_wifi_anomaly(snapshot)) {
        const char *label = deauth > 0 ? "DEAUTH ATTACK" :
                            disassoc > 0 ? "DISASSOC ATTACK" :
                            beacon ? "BEACON SPAM" : "WIFI ALERT";
        if (deauth > 0 && disassoc > 0) {
            snprintf(detail, sizeof(detail), "deauth %lu disassoc %lu",
                     (unsigned long)cap3(deauth),
                     (unsigned long)cap3(disassoc));
        } else if (deauth > 0) {
            snprintf(detail, sizeof(detail), "%lu deauth frames",
                     (unsigned long)cap3(deauth));
        } else if (disassoc > 0) {
            snprintf(detail, sizeof(detail), "%lu disassoc frames",
                     (unsigned long)cap3(disassoc));
        } else {
            snprintf(detail, sizeof(detail), "beacon spam active");
        }
        add_billboard_status_row(rows, max_rows, &count, snapshot,
                                 pinned_ble, pinned_wifi,
                                 viewed,
                                 BADGE_UI_DOMAIN_WIFI,
                                 label, detail, "", true);
    }

    if (!badge_viewed_has_meta_glasses(viewed) &&
        badge_meta_glasses_count(snapshot) == 0) {
        const scanner_info_t *meta = scanner_status_freshest_meta_info();
        if (meta) {
            if (meta->ble_focus_active) {
                snprintf(detail, sizeof(detail), "seen %llds focus %llds",
                         (long long)meta->ble_meta_last_seen_age_s,
                         (long long)meta->ble_focus_age_s);
            } else if (meta->ble_meta_last_rssi < 0) {
                snprintf(detail, sizeof(detail), "seen %llds  %ddB",
                         (long long)meta->ble_meta_last_seen_age_s,
                         (int)meta->ble_meta_last_rssi);
            } else {
                snprintf(detail, sizeof(detail), "seen %llds ago",
                         (long long)meta->ble_meta_last_seen_age_s);
            }
            add_billboard_status_row(rows, max_rows, &count, snapshot,
                                     pinned_ble, pinned_wifi,
                                     viewed,
                                     BADGE_UI_DOMAIN_PRIVACY,
                                     "META SIGNAL", detail, "", false);
        }
    }

    uint32_t empty_seen = 0;
    const scanner_info_t *ble_live =
        scanner_status_best_ble_live_info(NULL, &empty_seen);
    uint32_t tracker_seen = scanner_status_ble_tracker_seen_max();
    if (badge_meta_glasses_count(snapshot) == 0 && ble_live &&
        (!snapshot || snapshot->active_counts[BADGE_THREAT_TRACKER] == 0) &&
        tracker_seen == 0 &&
        ble_live->ble_any_best_rssi >= -60) {
        (void)empty_seen;
        char label[24];
        format_ble_signal_status(ble_live, label, sizeof(label),
                                 detail, sizeof(detail));
        add_billboard_status_row(rows, max_rows, &count, snapshot,
                                 pinned_ble, pinned_wifi,
                                 viewed,
                                 BADGE_UI_DOMAIN_PRIVACY,
                                 label, detail, "", false);
    }

    if (count < max_rows) {
        badge_display_diag_t diag_rows[4];
        int diag_count = build_scanner_diag_rows(
            diag_rows,
            (int)(sizeof(diag_rows) / sizeof(diag_rows[0])),
            ble_scanner_ok,
            wifi_scanner_ok,
            false
        );
        if (diag_count == 0 && count == 0 && scanner_status_has_badge_evidence()) {
            diag_count = build_scanner_diag_rows(
                diag_rows,
                (int)(sizeof(diag_rows) / sizeof(diag_rows[0])),
                ble_scanner_ok,
                wifi_scanner_ok,
                true
            );
        }
        for (int i = 0; i < diag_count && count < max_rows; i++) {
            add_billboard_status_row(rows, max_rows, &count, snapshot,
                                     pinned_ble, pinned_wifi,
                                     viewed,
                                     diag_rows[i].domain,
                                     diag_rows[i].label,
                                     diag_rows[i].detail,
                                     diag_rows[i].stat,
                                     diag_rows[i].severe);
        }
    }

    return count;
}

typedef struct {
    bool is_entity;
    badge_threat_display_lane_t lane;
    const badge_threat_snapshot_entity_t *entity;
    badge_display_diag_t diag;
    char key[BADGE_THREAT_VIEW_KEY_LEN];
} badge_billboard_candidate_t;

static badge_threat_display_lane_t badge_diag_display_lane(
    const badge_display_diag_t *diag)
{
    if (!diag || !diag->active) {
        return BADGE_THREAT_DISPLAY_LANE_NONE;
    }
    return diag->domain == BADGE_UI_DOMAIN_PRIVACY
        ? BADGE_THREAT_DISPLAY_LANE_BLE
        : BADGE_THREAT_DISPLAY_LANE_WIFI;
}

static bool badge_diag_is_noise_summary(const badge_display_diag_t *diag)
{
    if (!diag || diag->severe) {
        return false;
    }
    return strcmp(diag->label, "BLE SIGNAL") == 0 ||
           strcmp(diag->label, "BLE CLEAR") == 0 ||
           strcmp(diag->label, "WIFI CLEAR") == 0;
}

static bool badge_billboard_candidate_key_seen(
    const badge_billboard_candidate_t *candidates,
    int count,
    const char *key)
{
    if (!candidates || !key || key[0] == '\0') {
        return false;
    }
    for (int i = 0; i < count; i++) {
        if (strcmp(candidates[i].key, key) == 0) {
            return true;
        }
    }
    return false;
}

static void badge_billboard_add_entity_candidate(
    badge_billboard_candidate_t *candidates,
    int max_candidates,
    int *count,
    badge_threat_display_lane_t lane,
    const badge_threat_snapshot_entity_t *item)
{
    if (!candidates || !count || *count >= max_candidates || !item) {
        return;
    }
    if (lane == BADGE_THREAT_DISPLAY_LANE_NONE ||
        badge_threat_snapshot_entity_display_lane(item) != lane) {
        return;
    }
    char key[BADGE_THREAT_VIEW_KEY_LEN] = {0};
    if (!badge_threat_snapshot_entity_view_key(item, key, sizeof(key)) ||
        badge_billboard_candidate_key_seen(candidates, *count, key)) {
        return;
    }
    badge_billboard_candidate_t *candidate = &candidates[*count];
    memset(candidate, 0, sizeof(*candidate));
    candidate->is_entity = true;
    candidate->lane = lane;
    candidate->entity = item;
    snprintf(candidate->key, sizeof(candidate->key), "%s", key);
    (*count)++;
}

static void badge_billboard_add_diag_candidate(
    badge_billboard_candidate_t *candidates,
    int max_candidates,
    int *count,
    badge_threat_display_lane_t lane,
    const badge_display_diag_t *diag)
{
    if (!candidates || !count || *count >= max_candidates ||
        !diag || !diag->active) {
        return;
    }
    if (lane == BADGE_THREAT_DISPLAY_LANE_NONE ||
        badge_diag_display_lane(diag) != lane) {
        return;
    }
    char key[BADGE_THREAT_VIEW_KEY_LEN] = {0};
    if (badge_diag_is_noise_summary(diag)) {
        snprintf(key, sizeof(key), "STAT:%d:%s",
                 (int)diag->domain,
                 diag->label);
    } else {
        badge_status_view_key(key, sizeof(key), diag->domain,
                              diag->label, diag->detail, diag->stat);
    }
    if (badge_billboard_candidate_key_seen(candidates, *count, key)) {
        return;
    }
    badge_billboard_candidate_t *candidate = &candidates[*count];
    memset(candidate, 0, sizeof(*candidate));
    candidate->is_entity = false;
    candidate->lane = lane;
    candidate->diag = *diag;
    snprintf(candidate->key, sizeof(candidate->key), "%s", key);
    (*count)++;
}

static bool badge_board_item_visible_in_lane(
    const badge_threat_snapshot_t *snapshot,
    int index,
    badge_threat_display_lane_t lane,
    const badge_threat_snapshot_entity_t *pinned_ble,
    const badge_threat_snapshot_entity_t *pinned_wifi,
    const badge_display_viewed_t *viewed)
{
    if (!badge_board_item_visible_at(snapshot, index, pinned_ble, pinned_wifi, viewed)) {
        return false;
    }
    const badge_threat_snapshot_entity_t *item = &snapshot->entities[index];
    if (badge_threat_snapshot_entity_display_lane(item) != lane) {
        return false;
    }
    if (!badge_display_policy_allows_entity_lane(item, false)) {
        return false;
    }
    if (badge_item_is_drone_evidence(item) &&
        !badge_threat_snapshot_should_show_lower_drone_evidence(snapshot, item)) {
        return false;
    }
    if (badge_item_is_meta_glasses(item) &&
        !badge_threat_snapshot_should_show_lower_meta_evidence(
            snapshot,
            item,
            badge_viewed_has_meta_glasses(viewed))) {
        return false;
    }
    return true;
}

static int build_billboard_candidates_for_lane(
    badge_billboard_candidate_t *candidates,
    int max_candidates,
    badge_threat_display_lane_t lane,
    const badge_threat_snapshot_t *snapshot,
    bool ble_scanner_ok,
    bool wifi_scanner_ok,
    const badge_threat_snapshot_entity_t *pinned_ble,
    const badge_threat_snapshot_entity_t *pinned_wifi,
    const badge_display_viewed_t *viewed)
{
    if (!candidates || max_candidates <= 0) {
        return 0;
    }
    int count = 0;
    if (snapshot) {
        if (lane == BADGE_THREAT_DISPLAY_LANE_WIFI) {
            for (int i = 0; i < snapshot->entity_count && count < max_candidates; i++) {
                const badge_threat_snapshot_entity_t *item = &snapshot->entities[i];
                if (!badge_item_is_drone_evidence(item) ||
                    !badge_board_item_visible_in_lane(snapshot, i, lane, pinned_ble,
                                                      pinned_wifi, viewed)) {
                    continue;
                }
                badge_billboard_add_entity_candidate(candidates, max_candidates,
                                                     &count, lane, item);
            }
        } else if (lane == BADGE_THREAT_DISPLAY_LANE_BLE) {
            for (int i = 0; i < snapshot->entity_count && count < max_candidates; i++) {
                const badge_threat_snapshot_entity_t *item = &snapshot->entities[i];
                if (!badge_item_is_meta_glasses(item) ||
                    !badge_board_item_visible_in_lane(snapshot, i, lane, pinned_ble,
                                                      pinned_wifi, viewed)) {
                    continue;
                }
                badge_billboard_add_entity_candidate(candidates, max_candidates,
                                                     &count, lane, item);
            }
        }
        for (int i = 0; i < snapshot->entity_count && count < max_candidates; i++) {
            const badge_threat_snapshot_entity_t *item = &snapshot->entities[i];
            if ((lane == BADGE_THREAT_DISPLAY_LANE_WIFI &&
                 badge_item_is_drone_evidence(item)) ||
                (lane == BADGE_THREAT_DISPLAY_LANE_BLE &&
                 badge_item_is_meta_glasses(item))) {
                continue;
            }
            if (!badge_board_item_visible_in_lane(snapshot, i, lane, pinned_ble,
                                                  pinned_wifi, viewed)) {
                continue;
            }
            badge_billboard_add_entity_candidate(candidates, max_candidates,
                                                 &count,
                                                 lane,
                                                 item);
        }
    }

    badge_display_viewed_t status_viewed = viewed ? *viewed
                                                  : (badge_display_viewed_t){0};
    static badge_display_diag_t status_rows[8];
    memset(status_rows, 0, sizeof(status_rows));
    int status_total = build_billboard_status_rows(
        status_rows,
        (int)(sizeof(status_rows) / sizeof(status_rows[0])),
        snapshot,
        ble_scanner_ok,
        wifi_scanner_ok,
        pinned_ble,
        pinned_wifi,
        &status_viewed
    );
    for (int i = 0; i < status_total && count < max_candidates; i++) {
        badge_billboard_add_diag_candidate(candidates, max_candidates,
                                           &count, lane, &status_rows[i]);
    }
    return count;
}

static const badge_billboard_candidate_t *badge_billboard_select_candidate(
    const badge_billboard_candidate_t *candidates,
    int total,
    uint32_t phase_offset,
    int *pos_out)
{
    if (pos_out) {
        *pos_out = 0;
    }
    if (!candidates || total <= 0) {
        return NULL;
    }
    int pos = 0;
    if (total > 1) {
        pos = (int)(((s_queue_page_frame / BADGE_LOWER_PAGE_FRAMES) + phase_offset) %
                    (uint32_t)total);
    }
    if (pos_out) {
        *pos_out = pos;
    }
    return &candidates[pos];
}

static void badge_focus_model_reset(void)
{
    memset(&s_focus_model, 0, sizeof(s_focus_model));
}

static void badge_focus_entry_common(badge_focus_entry_t *entry,
                                     const char *lane,
                                     const char *title,
                                     const char *detail,
                                     int y,
                                     int h,
                                     int item_index,
                                     int item_total)
{
    if (!entry) {
        return;
    }
    entry->active = true;
    entry->y = y;
    entry->h = h;
    entry->item_index = item_index;
    entry->item_total = item_total;
    snprintf(entry->lane, sizeof(entry->lane), "%s", lane ? lane : "");
    snprintf(entry->title, sizeof(entry->title), "%s", title ? title : "");
    snprintf(entry->detail, sizeof(entry->detail), "%s", detail ? detail : "");
}

static void badge_focus_add_idle(const char *lane,
                                 const char *title,
                                 const char *detail,
                                 int y,
                                 int h)
{
    if (s_focus_model.count >= BADGE_FOCUS_ENTRY_MAX) {
        return;
    }
    badge_focus_entry_t *entry = &s_focus_model.entries[s_focus_model.count++];
    memset(entry, 0, sizeof(*entry));
    badge_focus_entry_common(entry, lane, title, detail, y, h, 0, 0);
    snprintf(entry->key, sizeof(entry->key), "IDLE:%s", lane ? lane : "");
}

static void badge_focus_add_entity(const badge_threat_snapshot_t *snapshot,
                                   const badge_threat_snapshot_entity_t *item,
                                   const char *lane,
                                   int y,
                                   int h,
                                   int item_index,
                                   int item_total)
{
    if (!item || s_focus_model.count >= BADGE_FOCUS_ENTRY_MAX) {
        badge_focus_add_idle(lane, "CLEAR", "No focused signal", y, h);
        return;
    }
    badge_focus_entry_t *entry = &s_focus_model.entries[s_focus_model.count++];
    memset(entry, 0, sizeof(*entry));
    entry->is_entity = true;
    entry->entity = *item;

    char title[32];
    char detail[OLED_BADGE_STATE_TEXT_LEN];
    format_badge_title_for_snapshot(title, sizeof(title), item, snapshot);
    if (!badge_threat_format_top_detail(snapshot, item, detail, sizeof(detail))) {
        char clean[32];
        char summary[36];
        clean_badge_detail(clean, sizeof(clean), item);
        format_badge_summary(summary, sizeof(summary), item);
        if (clean[0]) {
            snprintf(detail, sizeof(detail), "%.31s  %.36s", clean, summary);
        } else {
            snprintf(detail, sizeof(detail), "%s", summary);
        }
    }
    badge_focus_entry_common(entry, lane, title, detail, y, h,
                             item_index, item_total);
    if (!badge_threat_snapshot_entity_view_key(item, entry->key,
                                               sizeof(entry->key))) {
        snprintf(entry->key, sizeof(entry->key), "ENT:%s:%s",
                 title, detail);
    }
}

static void badge_focus_add_candidate(const badge_threat_snapshot_t *snapshot,
                                      const badge_billboard_candidate_t *candidate,
                                      const char *lane,
                                      const char *idle_title,
                                      const char *idle_detail,
                                      int y,
                                      int h,
                                      int item_index,
                                      int item_total)
{
    if (!candidate) {
        badge_focus_add_idle(lane, idle_title, idle_detail, y, h);
        return;
    }
    if (candidate->is_entity && candidate->entity) {
        badge_focus_add_entity(snapshot, candidate->entity, lane, y, h,
                               item_index, item_total);
        return;
    }
    if (s_focus_model.count >= BADGE_FOCUS_ENTRY_MAX) {
        return;
    }
    badge_focus_entry_t *entry = &s_focus_model.entries[s_focus_model.count++];
    memset(entry, 0, sizeof(*entry));
    entry->severe = candidate->diag.severe;
    entry->stat[0] = '\0';
    snprintf(entry->stat, sizeof(entry->stat), "%s", candidate->diag.stat);
    badge_focus_entry_common(entry, lane,
                             candidate->diag.label,
                             candidate->diag.detail,
                             y, h, item_index, item_total);
    snprintf(entry->key, sizeof(entry->key), "%s",
             candidate->key[0] ? candidate->key : candidate->diag.label);
}

static void badge_focus_model_finish(void)
{
    if (s_focus_model.count <= 0) {
        s_focus_model.count = 0;
        s_focus_index = 0;
    } else if (s_focus_index < 0 || s_focus_index >= s_focus_model.count) {
        s_focus_index = 0;
    }
    s_focus_model.focus_index = s_focus_index;
    s_focus_model.detail_mode = s_detail_mode;
    s_focus_model.detail_page = s_detail_page;
    s_focus_model.generation++;
}

static const badge_focus_entry_t *badge_focus_current_entry(void)
{
    int idx = s_focus_model.focus_index;
    if (idx < 0 || idx >= s_focus_model.count) {
        return NULL;
    }
    return &s_focus_model.entries[idx];
}

static void draw_focus_outline(void)
{
    if (s_button_overlay != BADGE_BUTTON_OVERLAY_NONE || s_detail_mode) {
        return;
    }
    const badge_focus_entry_t *entry = badge_focus_current_entry();
    if (!entry || !entry->active || entry->h <= 0) {
        return;
    }
    uint16_t color = entry->severe ? COL_GOLD : COL_LINK_BRIGHT;
    fb_fill_rect(1, entry->y + 1, LCD_W - 2, 1, color);
    fb_fill_rect(1, entry->y + entry->h - 2, LCD_W - 2, 1, color);
    fb_fill_rect(1, entry->y + 1, 1, entry->h - 2, color);
    fb_fill_rect(LCD_W - 2, entry->y + 1, 1, entry->h - 2, color);
    fb_draw_tiny_string(LCD_W - 18, entry->y + 3, "SEL", color, COL_BLACK);
}

static void draw_billboard_candidate_lane(
    int y,
    int h,
    const badge_threat_snapshot_t *snapshot,
    const badge_billboard_candidate_t *candidate)
{
    if (!candidate) {
        return;
    }
    if (candidate->is_entity && candidate->entity) {
        draw_billboard_row(y, h, snapshot, candidate->entity);
    } else {
        draw_diag_row(y, h, &candidate->diag);
    }
}

static void draw_badge_billboards(const badge_threat_snapshot_t *snapshot,
                                  bool ble_scanner_ok, bool wifi_scanner_ok,
                                  const badge_threat_snapshot_entity_t *pinned_ble,
                                  const badge_threat_snapshot_entity_t *pinned_wifi,
                                  badge_display_viewed_t *viewed,
                                  bool backend_ok, bool wifi_network_ok)
{
    const int y0 = 78;
    const int bottom_y = 148;
    const int row_h = 34;
    const int row_gap = 1;

    fb_fill_rect(0, y0, LCD_W, bottom_y - y0, COL_BLACK);
    static badge_billboard_candidate_t ble_candidates[8];
    static badge_billboard_candidate_t wifi_candidates[8];
    memset(ble_candidates, 0, sizeof(ble_candidates));
    memset(wifi_candidates, 0, sizeof(wifi_candidates));
    int ble_total = build_billboard_candidates_for_lane(
        ble_candidates,
        (int)(sizeof(ble_candidates) / sizeof(ble_candidates[0])),
        BADGE_THREAT_DISPLAY_LANE_BLE,
        snapshot,
        ble_scanner_ok,
        wifi_scanner_ok,
        pinned_ble,
        pinned_wifi,
        viewed
    );
    int wifi_total = build_billboard_candidates_for_lane(
        wifi_candidates,
        (int)(sizeof(wifi_candidates) / sizeof(wifi_candidates[0])),
        BADGE_THREAT_DISPLAY_LANE_WIFI,
        snapshot,
        ble_scanner_ok,
        wifi_scanner_ok,
        pinned_ble,
        pinned_wifi,
        viewed
    );

    int ble_pos = 0;
    int wifi_pos = 0;
    const badge_billboard_candidate_t *ble_candidate =
        badge_billboard_select_candidate(ble_candidates, ble_total, 0, &ble_pos);
    const badge_billboard_candidate_t *wifi_candidate =
        badge_billboard_select_candidate(wifi_candidates, wifi_total, 1, &wifi_pos);

    badge_focus_add_candidate(snapshot, ble_candidate, "LOWER BLE",
                              ble_scanner_ok ? "BLE CLEAR" : "BLE OFFLINE",
                              ble_scanner_ok ? "No extra glasses or tags" : "Check BLE scanner",
                              y0, row_h, ble_pos, ble_total);
    badge_focus_add_candidate(snapshot, wifi_candidate, "LOWER WIFI",
                              wifi_scanner_ok ? "WIFI CLEAR" : "WIFI OFFLINE",
                              wifi_scanner_ok ? "No SSID or attack" : "Check WiFi scanner",
                              y0 + row_h + row_gap, row_h, wifi_pos, wifi_total);

    if (ble_candidate) {
        draw_billboard_candidate_lane(y0, row_h, snapshot, ble_candidate);
    } else {
        draw_idle_billboard(y0, row_h,
                            ble_scanner_ok ? "BLE CLEAR" : "BLE OFFLINE",
                            ble_scanner_ok ? "No extra glasses or tags" : "Check BLE scanner",
                            ble_scanner_ok);
    }
    if (wifi_candidate) {
        draw_billboard_candidate_lane(y0 + row_h + row_gap, row_h,
                                      snapshot, wifi_candidate);
    } else {
        draw_idle_billboard(y0 + row_h + row_gap, row_h,
                            wifi_scanner_ok ? "WIFI CLEAR" : "WIFI OFFLINE",
                            wifi_scanner_ok ? "No SSID or attack" :
                            backend_ok ? "Backend connected" :
                            wifi_network_ok ? "No SSID or attack" :
                            "Check WiFi scanner",
                            wifi_scanner_ok);
    }

    if (ble_total > 1) {
        char page[8];
        snprintf(page, sizeof(page), "%d/%d", ble_pos + 1, ble_total);
        fb_draw_tiny_string(LCD_W - tiny_pixel_width(page) - 4,
                            y0 + row_h - 8, page, COL_DARKGRAY, COL_PANEL_2);
    }
    if (wifi_total > 1) {
        char page[8];
        snprintf(page, sizeof(page), "%d/%d", wifi_pos + 1, wifi_total);
        fb_draw_tiny_string(LCD_W - tiny_pixel_width(page) - 4,
                            bottom_y - 8, page, COL_DARKGRAY, COL_PANEL_2);
    }
    badge_focus_model_finish();
}

static void draw_evidence_queue(const badge_threat_snapshot_t *snapshot,
                                bool ble_scanner_ok, bool wifi_scanner_ok,
                                bool backend_ok, bool wifi_network_ok)
{
    const int list_y = 15;
    const int bottom_y = 148;
    const int row_h = 32;
    const int row_gap = 1;
    const int visible_rows = 4;

    (void)backend_ok;
    (void)wifi_network_ok;

    int entity_count = snapshot ? snapshot->entity_count : 0;
    int visible_entity_count = 0;
    for (int i = 0; i < entity_count; i++) {
        if (snapshot->entities[i].cls != BADGE_THREAT_BLE) {
            visible_entity_count++;
        }
    }

    if (visible_entity_count == 0) {
        badge_display_diag_t diag_rows[4];
        int diag_count = build_scanner_diag_rows(
            diag_rows,
            (int)(sizeof(diag_rows) / sizeof(diag_rows[0])),
            ble_scanner_ok,
            wifi_scanner_ok,
            false
        );
        if (diag_count == 0 && scanner_status_has_badge_evidence()) {
            diag_count = build_scanner_diag_rows(
                diag_rows,
                (int)(sizeof(diag_rows) / sizeof(diag_rows[0])),
                ble_scanner_ok,
                wifi_scanner_ok,
                true
            );
        }
        if (diag_count > 0) {
            fb_fill_rect(0, list_y, LCD_W, bottom_y - list_y, COL_BLACK);
            int rows_to_draw = diag_count < visible_rows ? diag_count : visible_rows;
            for (int row = 0; row < rows_to_draw; row++) {
                int y = list_y + row * (row_h + row_gap);
                draw_diag_row(y, row_h, &diag_rows[row]);
            }
            draw_scanner_bottom_strip(bottom_y, ble_scanner_ok, wifi_scanner_ok);
            return;
        }
        draw_empty_watch_state(list_y, bottom_y - list_y, ble_scanner_ok,
                               wifi_scanner_ok);
        draw_scanner_bottom_strip(bottom_y, ble_scanner_ok, wifi_scanner_ok);
        return;
    }

    int start = 0;
    if (visible_entity_count > visible_rows) {
        start = (int)((s_queue_page_frame / 36) % (uint32_t)visible_entity_count);
    }

    int rows_to_draw = visible_entity_count < visible_rows
        ? visible_entity_count
        : visible_rows;
    for (int row = 0; row < rows_to_draw; row++) {
        int visible_idx = start + row;
        if (visible_idx >= visible_entity_count) visible_idx -= visible_entity_count;
        int idx = -1;
        int seen = 0;
        for (int i = 0; i < entity_count; i++) {
            if (snapshot->entities[i].cls == BADGE_THREAT_BLE) {
                continue;
            }
            if (seen == visible_idx) {
                idx = i;
                break;
            }
            seen++;
        }

        int y = list_y + row * (row_h + row_gap);
        if (idx >= 0) {
            draw_queue_row(y, row_h, &snapshot->entities[idx]);
        }
    }
    for (int row = rows_to_draw; row < visible_rows; row++) {
        int y = list_y + row * (row_h + row_gap);
        fb_fill_rect(0, y, LCD_W, row_h, rgb565_scale_color(COL_PANEL, 45));
    }

    if (visible_entity_count > visible_rows) {
        char page[8];
        snprintf(page, sizeof(page), "%d/%d", start + 1, visible_entity_count);
        fb_draw_tiny_string(LCD_W - tiny_pixel_width(page) - 4,
                            bottom_y - 7, page, COL_DARKGRAY, COL_BLACK);
    }
    draw_scanner_bottom_strip(bottom_y, ble_scanner_ok, wifi_scanner_ok);
}

static void draw_badge_dashboard(const badge_threat_snapshot_t *snapshot,
                                 bool ble_scanner_ok, bool wifi_scanner_ok,
                                 bool backend_ok, bool wifi_network_ok,
                                 uint16_t drone_color,
                                 uint16_t privacy_color,
                                 uint16_t wifi_color,
                                 uint16_t accent)
{
    (void)drone_color;
    (void)privacy_color;
    (void)wifi_color;
    (void)accent;

    draw_threat_background(snapshot);

    const badge_threat_snapshot_entity_t *top_items[2] = {0};
    int top_pos[2] = {0};
    int top_total[2] = {0};
    select_top_global_badge_items(snapshot, top_items, top_pos, top_total);
    badge_display_viewed_t viewed = {0};
    badge_viewed_mark_entity(&viewed, top_items[0]);
    badge_viewed_mark_entity(&viewed, top_items[1]);

    badge_focus_model_reset();
    badge_threat_display_lane_t other_lane = badge_top_item_lane(top_items[0]);
    for (int slot = 0; slot < 2; slot++) {
        const int y = slot == 0 ? 0 : 39;
        const badge_threat_snapshot_entity_t *item = top_items[slot];
        badge_threat_display_lane_t lane = item
            ? badge_top_item_lane(item)
            : (slot == 0
                ? BADGE_THREAT_DISPLAY_LANE_BLE
                : (other_lane == BADGE_THREAT_DISPLAY_LANE_WIFI
                    ? BADGE_THREAT_DISPLAY_LANE_BLE
                    : BADGE_THREAT_DISPLAY_LANE_WIFI));
        badge_ui_domain_t domain = badge_domain_for_display_lane(lane);
        bool scanner_ok = lane == BADGE_THREAT_DISPLAY_LANE_WIFI
            ? wifi_scanner_ok
            : ble_scanner_ok;
        const char *lane_label = badge_top_lane_label(lane);
        const char *focus_label = badge_top_focus_label(lane);
        const char *clear_label = lane == BADGE_THREAT_DISPLAY_LANE_WIFI
            ? (wifi_scanner_ok ? "WIFI CLEAR" : "WIFI OFFLINE")
            : (ble_scanner_ok ? "BLE CLEAR" : "BLE OFFLINE");
        const char *clear_detail = lane == BADGE_THREAT_DISPLAY_LANE_WIFI
            ? (wifi_scanner_ok ? "No SSID/attack" : "Check WiFi scanner")
            : (ble_scanner_ok ? "No tags/glasses" : "Check BLE scanner");

        if (item) {
            badge_focus_add_entity(snapshot, item, focus_label, y, 37,
                                   top_pos[slot], top_total[slot]);
        } else {
            badge_focus_add_idle(focus_label, clear_label, clear_detail, y, 37);
        }
        draw_top_concern_tile(
            y,
            domain,
            lane_label,
            item,
            top_pos[slot],
            top_total[slot],
            snapshot,
            scanner_ok,
            clear_label,
            clear_detail
        );
    }

    draw_badge_billboards(snapshot, ble_scanner_ok, wifi_scanner_ok,
                          top_items[0],
                          top_items[1],
                          &viewed,
                          backend_ok, wifi_network_ok);
    draw_focus_outline();
    draw_scanner_bottom_strip(148, ble_scanner_ok, wifi_scanner_ok);
}

static void draw_detail_pair(int y, const char *label, const char *value)
{
    if (!value || value[0] == '\0') {
        return;
    }
    fb_draw_tiny_string(8, y, label ? label : "", COL_DARKGRAY, COL_BLACK);
    fb_draw_string_fit(45, y - 1, value, LCD_W - 50, COL_WHITE, COL_BLACK);
}

static void draw_badge_focused_detail_screen(void)
{
    const badge_focus_entry_t *entry = badge_focus_current_entry();
    fb_clear(COL_BLACK);
    if (!entry || !entry->active) {
        fb_draw_string_centered(LCD_W / 2, 58, "NO SIGNAL", COL_GRAY, COL_BLACK, 1);
        fb_draw_string_centered(LCD_W / 2, 78, "Press B1", COL_DARKGRAY, COL_BLACK, 1);
        return;
    }

    uint16_t color = entry->severe ? COL_GOLD :
        entry->is_entity ? ui_category_base_color(entry->entity.category) : COL_LINK_BRIGHT;
    fb_fill_rect(0, 0, LCD_W, 15, rgb565_mix_color(COL_PANEL_2, color, 45));
    fb_draw_tiny_string(6, 4, entry->lane, color, COL_PANEL_2);
    char page[16];
    snprintf(page, sizeof(page), "%d/3", s_detail_page + 1);
    fb_draw_tiny_string(LCD_W - tiny_pixel_width(page) - 5, 4,
                        page, COL_GRAY, COL_PANEL_2);
    fb_draw_string_fit(6, 20, entry->title, LCD_W - 12, COL_WHITE, COL_BLACK);
    fb_draw_string_fast_marquee(6, 34, entry->detail, LCD_W - 12,
                                rgb565_mix_color(COL_WHITE, color, 86),
                                COL_BLACK);

    if (s_detail_page == 0) {
        char value[48];
        snprintf(value, sizeof(value), "score %d  rssi %ddB",
                 entry->is_entity ? entry->entity.score : 0,
                 entry->is_entity ? entry->entity.rssi : 0);
        draw_detail_pair(58, "HEAT", value);
        if (entry->is_entity) {
            snprintf(value, sizeof(value), "best %ddB  prox %d",
                     entry->entity.best_rssi,
                     (int)entry->entity.proximity_level);
            draw_detail_pair(74, "RF", value);
            snprintf(value, sizeof(value), "age %ds  seen %ds",
                     entry->entity.age_s, entry->entity.last_seen_s);
            draw_detail_pair(90, "TIME", value);
            snprintf(value, sizeof(value), "events %lu  count %lu",
                     (unsigned long)entry->entity.event_count,
                     (unsigned long)entry->entity.seen_count);
            draw_detail_pair(106, "OBS", value);
        } else {
            draw_detail_pair(74, "STAT", entry->stat);
        }
    } else if (s_detail_page == 1) {
        if (entry->is_entity && entry->entity.has_location) {
            char value[48];
            snprintf(value, sizeof(value), "%.6f", entry->entity.latitude);
            draw_detail_pair(58, "LAT", value);
            snprintf(value, sizeof(value), "%.6f", entry->entity.longitude);
            draw_detail_pair(74, "LON", value);
            snprintf(value, sizeof(value), "%.1fm", entry->entity.altitude_m);
            draw_detail_pair(90, "ALT", value);
        } else {
            draw_detail_pair(58, "GPS", "not in signal");
        }
        if (entry->is_entity && entry->entity.has_operator_location) {
            char value[48];
            snprintf(value, sizeof(value), "%.6f", entry->entity.operator_lat);
            draw_detail_pair(106, "OPLAT", value);
            snprintf(value, sizeof(value), "%.6f", entry->entity.operator_lon);
            draw_detail_pair(122, "OPLON", value);
        }
        if (entry->is_entity && entry->entity.operator_id[0] != '\0') {
            draw_detail_pair(138, "OPID", entry->entity.operator_id);
        }
    } else {
        if (entry->is_entity) {
            draw_detail_pair(58, "CLASS", badge_threat_class_name(entry->entity.cls));
            draw_detail_pair(74, "CAT", badge_threat_category_name(entry->entity.category));
            draw_detail_pair(90, "CODE", badge_threat_category_code(entry->entity.category));
            draw_detail_pair(106, "SRC", badge_threat_source_code(entry->entity.source));
            draw_detail_pair(122, "WHY",
                             entry->entity.evidence[0] ? entry->entity.evidence :
                             entry->entity.detail);
            draw_detail_pair(138, entry->entity.display_id[0] ? "ID" : "KEY",
                             entry->entity.display_id[0] ? entry->entity.display_id :
                             entry->key);
        } else {
            draw_detail_pair(58, "TYPE", "status row");
            draw_detail_pair(74, "STAT", entry->stat);
            draw_detail_pair(90, "KEY", entry->key);
        }
    }

    fb_draw_tiny_string(6, 151, "B2 page  dbl back", COL_DARKGRAY, COL_BLACK);
}

bool oled_badge_get_display_state(oled_badge_display_state_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!display_lock(pdMS_TO_TICKS(80))) {
        return false;
    }
    const badge_focus_entry_t *entry = badge_focus_current_entry();
    out->detail_mode = s_detail_mode;
    out->detail_page = s_detail_page;
    out->focus_index = s_focus_model.focus_index;
    out->focus_total = s_focus_model.count;
    if (!entry || !entry->active) {
        display_unlock();
        return false;
    }

    out->active = true;
    out->item_index = entry->item_index;
    out->item_total = entry->item_total;
    snprintf(out->lane, sizeof(out->lane), "%s", entry->lane);
    snprintf(out->title, sizeof(out->title), "%s", entry->title);
    snprintf(out->detail, sizeof(out->detail), "%s", entry->detail);
    snprintf(out->entity_key, sizeof(out->entity_key), "%s", entry->key);
    if (entry->is_entity) {
        const badge_threat_snapshot_entity_t *entity = &entry->entity;
        snprintf(out->evidence, sizeof(out->evidence), "%s",
                 entity->evidence);
        snprintf(out->display_id, sizeof(out->display_id), "%s", entity->display_id);
        snprintf(out->threat_class, sizeof(out->threat_class), "%s",
                 badge_threat_class_name(entity->cls));
        snprintf(out->category, sizeof(out->category), "%s",
                 badge_threat_category_name(entity->category));
        snprintf(out->code, sizeof(out->code), "%s",
                 badge_threat_category_code(entity->category));
        snprintf(out->source, sizeof(out->source), "%s",
                 badge_threat_source_code(entity->source));
        out->score = entity->score;
        out->confidence_pct = entity->confidence_pct;
        out->evidence_quality = entity->evidence_quality;
        out->display_rank = entity->display_rank;
        out->age_s = entity->age_s;
        out->last_seen_s = entity->last_seen_s;
        out->rssi = entity->rssi;
        out->best_rssi = entity->best_rssi;
        out->events = entity->event_count;
        out->seen_count = entity->seen_count;
        out->group_count = entity->group_count;
        out->proximity_level = entity->proximity_level;
        out->stale = entity->stale;
        out->has_location = entity->has_location;
        out->latitude = entity->latitude;
        out->longitude = entity->longitude;
        out->altitude_m = entity->altitude_m;
        out->has_operator_location = entity->has_operator_location;
        out->operator_lat = entity->operator_lat;
        out->operator_lon = entity->operator_lon;
        snprintf(out->operator_id, sizeof(out->operator_id), "%s",
                 entity->operator_id);
    } else {
        snprintf(out->threat_class, sizeof(out->threat_class), "status");
        snprintf(out->category, sizeof(out->category), "%s", entry->stat);
    }
    display_unlock();
    return true;
}

bool oled_badge_get_button_state(oled_badge_button_state_t *out)
{
    if (!out) {
        return false;
    }
    *out = s_button_diag;
    return true;
}

bool oled_badge_handle_nav_command(const char *action)
{
    if (!action || action[0] == '\0') {
        return false;
    }
    if (strcmp(action, "next") == 0) {
        if (s_detail_mode) {
            badge_display_nav_page();
        } else {
            badge_display_nav_next();
        }
        return true;
    }
    if (strcmp(action, "detail") == 0 || strcmp(action, "select") == 0) {
        badge_display_nav_detail();
        return true;
    }
    if (strcmp(action, "page") == 0) {
        badge_display_nav_page();
        return true;
    }
    if (strcmp(action, "back") == 0 || strcmp(action, "close") == 0) {
        badge_button_note_activity();
        s_detail_mode = false;
        s_detail_page = 0;
        s_button_overlay = BADGE_BUTTON_OVERLAY_NONE;
        return true;
    }
    return false;
}

static void draw_boot_screen(const char *stage, const char *mode, const char *line)
{
    if (!s_initialized) return;

    fb_clear(COL_BLACK);
    draw_triforce_scaled(LCD_W / 2, 48, 64, 54, 0, COL_LINK_GREEN, COL_LINK_DARK);
    fb_draw_string_centered(LCD_W / 2, 92, "FoF Badge", COL_WHITE, COL_BLACK, 1);
    fb_draw_string_centered(LCD_W / 2, 106, "v" FOF_VERSION, COL_GRAY, COL_BLACK, 1);
    if (mode && mode[0]) {
        fb_draw_string_centered(LCD_W / 2, 122, mode, COL_SOFT_GREEN, COL_BLACK, 1);
    }
    if (stage && stage[0]) {
        fb_draw_string_centered(LCD_W / 2, 138, stage, COL_LINK_BRIGHT, COL_BLACK, 1);
    }
    if (line && line[0]) {
        fb_draw_string_centered(LCD_W / 2, 150, line, COL_GRAY, COL_BLACK, 1);
    }
    st_flush();
}

/* ── Public API ────────────────────────────────────────────────────────── */

void oled_init(void)
{
    if (s_initialized) return;
    if (!s_display_mutex) {
        s_display_mutex = xSemaphoreCreateMutex();
        if (!s_display_mutex) {
            ESP_LOGE(TAG, "Display mutex allocation failed");
            return;
        }
    }

    /* Reset every panel pin we're about to use so the SPI matrix gets a
     * clean slate before the badge buttons claim GPIO8/GPIO43 as inputs. */
    gpio_reset_pin(ST7735_PIN_MOSI);
    gpio_reset_pin(ST7735_PIN_SCK);
    gpio_reset_pin(ST7735_PIN_CS);
    gpio_reset_pin(ST7735_PIN_DC);
    gpio_reset_pin(ST7735_PIN_RES);
#if ST7735_PIN_BL >= 0
    gpio_reset_pin(ST7735_PIN_BL);
#endif

    /* (Pin blink skipped — user has verified wiring by hand. Re-enable
     * with diag_pin_blink() if a future panel/wiring change needs it.) */

    /* Manual CS + DC + RES (+ optional BL) pins as outputs. */
    uint64_t pin_mask = st_cs_pin_mask() | (1ULL << ST7735_PIN_DC) | (1ULL << ST7735_PIN_RES);
#if ST7735_PIN_BL >= 0
    pin_mask |= (1ULL << ST7735_PIN_BL);
#endif
    gpio_config_t io = {
        .pin_bit_mask = pin_mask,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    st_cs_set(1);
    gpio_set_level(ST7735_PIN_DC,  1);
    gpio_set_level(ST7735_PIN_RES, 1);
#if ST7735_PIN_BL >= 0
    gpio_set_level(ST7735_PIN_BL, 1);   /* backlight ON */
    ESP_LOGI(TAG, "Backlight pin GPIO%d driven HIGH", ST7735_PIN_BL);
#endif

    /* Allocate framebuffer in PSRAM. */
    s_fb = heap_caps_malloc(LCD_FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fb) {
        /* Fall back to internal heap — 40 KB is large but the badge has
         * plenty since BT is disabled.  Better than aborting boot. */
        s_fb = heap_caps_malloc(LCD_FB_BYTES, MALLOC_CAP_8BIT);
    }
    if (!s_fb) {
        ESP_LOGE(TAG, "Framebuffer allocation failed (%d bytes)", LCD_FB_BYTES);
        return;
    }
    memset(s_fb, 0, LCD_FB_BYTES);

    s_tx_chunk = heap_caps_malloc(20 * LCD_W * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_tx_chunk) {
        ESP_LOGW(TAG, "DMA staging allocation failed; falling back to direct framebuffer SPI");
    }

    /* SPI bus + device. */
    spi_bus_config_t buscfg = {
        .miso_io_num     = -1,
        .mosi_io_num     = ST7735_PIN_MOSI,
        .sclk_io_num     = ST7735_PIN_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 20 * LCD_W * 2,   /* matches st_flush chunk size */
    };
    esp_err_t err = spi_bus_initialize(ST7735_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %d", err);
        return;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = ST7735_SPI_HZ,
        .mode           = 0,
        .spics_io_num   = -1,
        .queue_size     = 4,
        .pre_cb         = st7735_spi_pre_cb,
        .post_cb        = st7735_spi_post_cb,
    };
    err = spi_bus_add_device(ST7735_SPI_HOST, &devcfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %d", err);
        return;
    }

    /* Bring up panel. */
    st7735_hw_reset();
    st7735_panel_init();

    s_initialized = true;
    ESP_LOGI(TAG, "ST7735 initialized (128x160, SPI3 @ %d MHz, CS GPIO%d)",
             ST7735_SPI_HZ / 1000000, ST7735_PIN_CS);

    badge_buttons_start();

    /* Show the badge splash once the panel is configured. The heavy color
     * diagnostics stay compiled in for bench bring-up, but normal boot keeps
     * the LCD experience clean. */
    splash_show();
}

void oled_update(int detection_count, bool ble_scanner_ok, bool wifi_scanner_ok,
                 bool backend_ok, int upload_count, bool wifi_network_ok,
                 float battery_pct, uint32_t uptime_s, const char *device_id)
{
    (void)detection_count;
    (void)upload_count;
    (void)battery_pct;  /* badge has no battery monitor */
    (void)uptime_s;
    if (!s_initialized) return;
    if (!display_lock(pdMS_TO_TICKS(30))) return;

    s_anim_frame++;
    if (s_detail_mode && s_last_button_ms > 0 &&
        (badge_now_ms() - s_last_button_ms) > BADGE_DETAIL_TIMEOUT_MS) {
        s_detail_mode = false;
        s_detail_page = 0;
    }
    badge_button_overlay_t overlay = s_button_overlay;
    if (overlay == BADGE_BUTTON_OVERLAY_TRIFORCE) {
        draw_triforce_splash_static(COL_LINK_GREEN);
        s_queue_page_frame++;
        st_flush();
        display_unlock();
        return;
    }
    if (overlay == BADGE_BUTTON_OVERLAY_QR) {
        draw_gamechangers_qr_screen();
        s_queue_page_frame++;
        st_flush();
        display_unlock();
        return;
    }

    static badge_threat_snapshot_t snapshot;
    uart_rx_get_badge_threat_snapshot(&snapshot);

    fb_clear(COL_BLACK);

    uint16_t accent = snapshot.color_rgb565;
    uint16_t drone_color = ui_domain_color(&snapshot, BADGE_UI_DOMAIN_DRONE);
    uint16_t privacy_color = ui_domain_color(&snapshot, BADGE_UI_DOMAIN_PRIVACY);
    uint16_t wifi_color = ui_domain_color(&snapshot, BADGE_UI_DOMAIN_WIFI);

    (void)device_id;
    draw_badge_dashboard(&snapshot, ble_scanner_ok, wifi_scanner_ok,
                         backend_ok, wifi_network_ok,
                         drone_color, privacy_color, wifi_color, accent);
    if (s_detail_mode) {
        draw_badge_focused_detail_screen();
    }
    s_queue_page_frame++;

    st_flush();
    display_unlock();
}

void oled_show_detection(const char *detection_id, const char *manufacturer,
                         uint8_t source, float confidence, int rssi)
{
    (void)detection_id;
    (void)manufacturer;
    (void)source;
    (void)confidence;
    (void)rssi;
}

void oled_show_boot_status(const char *stage, const char *mode, const char *line)
{
    if (!s_initialized) return;
    if (!display_lock(pdMS_TO_TICKS(300))) return;
    draw_boot_screen(stage, mode, line);
    display_unlock();
}

void oled_clear(void)
{
    if (!s_initialized) return;
    if (!display_lock(pdMS_TO_TICKS(300))) return;
    fb_clear(COL_BLACK);
    st_flush();
    display_unlock();
}

#endif /* FOF_BADGE_VARIANT */
