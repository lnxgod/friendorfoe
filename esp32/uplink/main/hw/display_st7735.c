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
 *   CS         = GPIO8 primary, GPIO44 legacy/bring-up pad also driven
 *   RES        = GPIO6
 *   DC         = GPIO5
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
#include "esp_rom_sys.h"  /* esp_rom_delay_us for bit-bang fallback */

static const char *TAG = "st7735";

/* ── Hardware ──────────────────────────────────────────────────────────── */

#define ST7735_PIN_MOSI     9
#define ST7735_PIN_SCK      7
#define ST7735_PIN_CS_PRIMARY 8
#define ST7735_PIN_CS_LEGACY  44
#define ST7735_PIN_DC       5
#define ST7735_PIN_RES      6
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

/* ── State ─────────────────────────────────────────────────────────────── */

static spi_device_handle_t s_spi = NULL;
static uint16_t           *s_fb = NULL;       /* big endian RGB565 */
static uint8_t            *s_tx_chunk = NULL; /* internal DMA-safe SPI staging */
static bool                s_initialized = false;
static SemaphoreHandle_t   s_display_mutex = NULL;
static int                 s_spi_error_count = 0;
static uint32_t s_anim_frame = 0;
static uint32_t s_queue_page_frame = 0;

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

/* Drive both known badge CS pads. Early bring-up notes and boards disagree
 * between GPIO8 and GPIO44; with no MISO line there is no reliable way to
 * probe the wired pad, so selecting both keeps either wiring alive. */
static inline void st_cs_set(int level)
{
    gpio_set_level(ST7735_PIN_CS_PRIMARY, level);
    gpio_set_level(ST7735_PIN_CS_LEGACY, level);
}

static inline uint64_t st_cs_pin_mask(void)
{
    return (1ULL << ST7735_PIN_CS_PRIMARY) | (1ULL << ST7735_PIN_CS_LEGACY);
}

/* SPI callbacks use transaction.user as a 0/1 flag to drive DC and manually
 * assert CS. Manual CS is intentional: it lets us select GPIO8 and GPIO44
 * together instead of trusting one stale pin map. */
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
        { ST7735_PIN_CS_PRIMARY, "CS primary GPIO8" },
        { ST7735_PIN_CS_LEGACY,  "CS legacy  GPIO44" },
        { ST7735_PIN_DC,   "DC/A0     GPIO5" },
        { ST7735_PIN_RES,  "RES/RST   GPIO6" },
#if ST7735_PIN_BL >= 0
        { ST7735_PIN_BL,   "BL/LED    GPIO8" },
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
    /* MADCTL: MX | MY | RGB order (0xC8 = MX|MY|BGR for typical red-tab portrait).
     * If colors look wrong on the panel we have, try 0xC0 / 0x00 / 0xA0. */
    st_write_cmd(ST_CMD_MADCTL); st_write_data_byte(0xC8);
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
    st_write_cmd(0x36);  st_write_data_byte(0xC8);          /* MADCTL  */
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
    gpio_reset_pin(ST7735_PIN_CS_PRIMARY);
    gpio_reset_pin(ST7735_PIN_CS_LEGACY);
    gpio_reset_pin(ST7735_PIN_DC);
    /* RES already configured as output earlier; re-set direction in case
     * gpio_reset_pin elsewhere wiped it. */
    gpio_set_direction(ST7735_PIN_MOSI, GPIO_MODE_OUTPUT);
    gpio_set_direction(ST7735_PIN_SCK,  GPIO_MODE_OUTPUT);
    gpio_set_direction(ST7735_PIN_CS_PRIMARY, GPIO_MODE_OUTPUT);
    gpio_set_direction(ST7735_PIN_CS_LEGACY,  GPIO_MODE_OUTPUT);
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
    bb_write_cmd(0x36); bb_write_data_byte(0xC8);          /* MADCTL  */
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
    bb_write_cmd(0x36); bb_write_data_byte(0xC8);          /* MADCTL  */
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

static void splash_show(void)
{
    fb_clear(COL_BLACK);

    /* Subtle starfield-ish dot pattern in upper-right corner — keeps it
     * from looking too empty without burning a lot of pixels. */
    for (int i = 0; i < 12; i++) {
        int sx = (i * 17 + 7)  % 120 + 4;
        int sy = (i * 11 + 3)  % 12  + 2;
        fb_set_pixel(sx, sy, COL_GRAY);
    }

    draw_triforce(COL_LINK_GREEN);

    /* Title */
    fb_draw_string_centered(LCD_W / 2, 108, "FRIEND OR FOE", COL_WHITE, COL_BLACK, 1);

    /* Version line — dim, small */
    fb_draw_string_centered(LCD_W / 2, 122, "v" FOF_VERSION, COL_GRAY, COL_BLACK, 1);

    /* Light tagline */
    fb_draw_string_centered(LCD_W / 2, 140, "FIELD SIGNALS", COL_SOFT_GREEN, COL_BLACK, 1);

    st_flush();

    /* ~1.8s hold with a small pulse on the bottom-right Triforce piece. */
    const uint16_t pulse_seq[] = { COL_LINK_GREEN, COL_LINK_BRIGHT, COL_LINK_GREEN, COL_LINK_DARK, COL_LINK_GREEN };
    for (size_t i = 0; i < sizeof(pulse_seq) / sizeof(pulse_seq[0]); i++) {
        fb_fill_triangle(88, 55, 112, 92, 64, 92, pulse_seq[i]);
        st_flush();
        vTaskDelay(pdMS_TO_TICKS(360));
    }
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

typedef struct {
    bool active;
    badge_ui_domain_t domain;
    char label[24];
    char detail[44];
    char stat[12];
    bool severe;
} badge_display_diag_t;

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
        snprintf(detail, sizeof(detail), "%.9s", src);
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
            snprintf(line, sizeof(line), "%.8s %ddB x%lu %s",
                     detail, item->best_rssi, (unsigned long)item->seen_count, age);
        } else if (prox[0]) {
            snprintf(line, sizeof(line), "%s %ddB x%lu %s",
                     prox, item->best_rssi, (unsigned long)item->seen_count, age);
        } else {
            snprintf(line, sizeof(line), "%ddB x%lu %s",
                     item->best_rssi, (unsigned long)item->seen_count, age);
        }
    } else {
        int count = 0;
        const char *p = item->detail[0] ? strstr(item->detail, "count:") : NULL;
        if (p) {
            count = atoi(p + 6);
        }
        if (count > 0) {
            snprintf(line, sizeof(line), "count %d age %s", count, age);
        } else if (item->seen_count > 1) {
            snprintf(line, sizeof(line), "seen %lu age %s",
                     (unsigned long)item->seen_count, age);
        } else if (detail[0]) {
            snprintf(line, sizeof(line), "%.12s age %s", detail, age);
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
    const char *code = ui_domain_code(diag->domain);
    fb_fill_rect(0, y, LCD_W, h, bg);
    fb_fill_rect(0, y, 5, h, color);
    fb_draw_tiny_string(8, y + 4, code, color, bg);
    int label_x = 8 + tiny_pixel_width(code) + 6;
    int sw = diag->stat[0] ? tiny_pixel_width(diag->stat) : 0;
    int label_max = LCD_W - label_x - (sw > 0 ? sw + 9 : 5);
    if (label_max < 40) label_max = 40;
    fb_draw_string_fit(label_x, y + 4, diag->label, label_max,
                       diag->severe ? COL_WHITE : COL_GRAY, bg);
    if (diag->stat[0]) {
        fb_draw_tiny_string(LCD_W - sw - 4, y + 6, diag->stat, color, bg);
    }
    if (diag->detail[0]) {
        fb_draw_string_fit(label_x, y + 19, diag->detail,
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

    uint32_t known_privacy = ble ? (ble->ble_tracker_seen + ble->ble_meta_seen + ble->rid_emit) : 0;
    uint32_t meta_seen = ble ? ble->ble_meta_seen : 0;
    uint32_t drone_rid_seen = wifi ? wifi->rid_emit : 0;
    uint32_t wifi_anom = wifi ? ((uint32_t)wifi->deauth_count +
                                 (uint32_t)wifi->disassoc_count +
                                 (wifi->beacon_spam ? 1U : 0U)) : 0U;
    const char *ble_state = ble_proof_label(ble, ble_scanner_ok, ble_uart.raw_seen, ble_role_ok,
                                            ble_cmd_fresh, ble_old);
    const char *wifi_state = wifi_proof_label(wifi_scanner_ok, wifi_uart.raw_seen, wifi_role_ok,
                                              wifi_cmd_fresh, wifi_old);
    char line[48];
    const char *ssid = (wifi && wifi->wifi_last_drone_ssid[0])
        ? wifi->wifi_last_drone_ssid
        : ((wifi && wifi->wifi_last_notable_ssid[0])
            ? wifi->wifi_last_notable_ssid
            : NULL);
    if (ssid) {
        snprintf(line, sizeof(line), "B %s tag%lu m%lu W %s %.13s",
                 ble_state,
                 (unsigned long)cap3(known_privacy),
                 (unsigned long)cap3(meta_seen),
                 wifi_state,
                 ssid);
    } else {
        snprintf(line, sizeof(line), "B %s tag%lu m%lu W %s rid%lu an%lu",
                 ble_state,
                 (unsigned long)cap3(known_privacy),
                 (unsigned long)cap3(meta_seen),
                 wifi_state,
                 (unsigned long)cap3(drone_rid_seen),
                 (unsigned long)cap3(wifi_anom));
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
        snprintf(detail, sizeof(detail), "TAG%lu META%lu NEAR%lu RID%lu",
                 (unsigned long)cap3(ble ? ble->ble_tracker_seen : 0),
                 (unsigned long)cap3(ble ? ble->ble_meta_seen : 0),
                 (unsigned long)cap3(ble ? ble->ble_near_unknown_seen : 0),
                 (unsigned long)cap3(ble ? ble->rid_emit : 0));
        add_diag_row(rows, max_rows, &count, BADGE_UI_DOMAIN_PRIVACY,
                     "BLE", detail, "OK", false);
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
        uint32_t wifi_anom = wifi ? ((uint32_t)wifi->deauth_count +
                                     (uint32_t)wifi->disassoc_count +
                                     (wifi->beacon_spam ? 1U : 0U)) : 0U;
        snprintf(detail, sizeof(detail), "D%lu N%lu AN%lu %s",
                 (unsigned long)cap3(wifi ? wifi->wifi_drone_ssid_emit : 0),
                 (unsigned long)cap3(wifi ? wifi->wifi_notable_ssid_emit : 0),
                 (unsigned long)cap3(wifi_anom),
                 wifi_actual[0] ? wifi_actual : "wifi");
        add_diag_row(rows, max_rows, &count, BADGE_UI_DOMAIN_WIFI,
                     "Wi-Fi", detail, "OK", false);
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
    fb_draw_tiny_string_fit(4, y + 4, line, LCD_W - 8, fg, bg);
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
    uint32_t ble_evidence = ble
        ? (ble->ble_meta_seen + ble->ble_tracker_seen +
           ble->ble_near_unknown_seen + ble->rid_emit)
        : 0U;
    uint32_t wifi_anom = wifi
        ? ((uint32_t)wifi->deauth_count + (uint32_t)wifi->disassoc_count +
           (wifi->beacon_spam ? 1U : 0U))
        : 0U;
    uint32_t wifi_evidence = wifi
        ? (wifi->rid_emit + wifi->wifi_drone_ssid_emit +
           wifi->wifi_notable_ssid_emit + wifi_anom)
        : 0U;
    return ble_evidence > 0 || wifi_evidence > 0;
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
    draw_threat_background(snapshot);

    const badge_threat_snapshot_entity_t *primary =
        (snapshot && snapshot->entity_count > 0) ? &snapshot->entities[0] : NULL;
    uint16_t header_seed = primary ? ui_category_deep_color(primary->category)
                                   : ui_domain_deep_color(ui_dominant_domain(snapshot));
    uint16_t header_bg = rgb565_mix_color(header_seed, COL_BLACK, 128);
    fb_fill_rect(0, 0, LCD_W, 12, header_bg);
    fb_draw_string_fit(3, 2, "FoF", 30, COL_WHITE, header_bg);
    draw_triforce_watermark(drone_color, privacy_color, wifi_color);
    fb_fill_rect(0, 12, LCD_W, 1, rgb565_scale_color(accent, 125));

    draw_evidence_queue(snapshot, ble_scanner_ok, wifi_scanner_ok,
                        backend_ok, wifi_network_ok);
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

    /* Reset every pin we're about to use — ESP32-S3 boot ROM leaves
     * GPIO43/44 tied to U0TXD/U0RXD with internal pullups even when the
     * console is routed to USB-JTAG. gpio_reset_pin() detaches them from
     * any prior peripheral so the SPI matrix gets a clean slate. */
    gpio_reset_pin(ST7735_PIN_MOSI);
    gpio_reset_pin(ST7735_PIN_SCK);
    gpio_reset_pin(ST7735_PIN_CS_PRIMARY);
    gpio_reset_pin(ST7735_PIN_CS_LEGACY);
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
    ESP_LOGI(TAG, "ST7735 initialized (128x160, SPI3 @ %d MHz, CS GPIO%d+GPIO%d)",
             ST7735_SPI_HZ / 1000000, ST7735_PIN_CS_PRIMARY, ST7735_PIN_CS_LEGACY);

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

    badge_threat_snapshot_t snapshot;
    uart_rx_get_badge_threat_snapshot(&snapshot);

    fb_clear(COL_BLACK);

    uint16_t accent = snapshot.color_rgb565;
    uint16_t drone_color = ui_domain_color(&snapshot, BADGE_UI_DOMAIN_DRONE);
    uint16_t privacy_color = ui_domain_color(&snapshot, BADGE_UI_DOMAIN_PRIVACY);
    uint16_t wifi_color = ui_domain_color(&snapshot, BADGE_UI_DOMAIN_WIFI);

    (void)device_id;
    s_anim_frame++;
    draw_badge_dashboard(&snapshot, ble_scanner_ok, wifi_scanner_ok,
                         backend_ok, wifi_network_ok,
                         drone_color, privacy_color, wifi_color, accent);
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
