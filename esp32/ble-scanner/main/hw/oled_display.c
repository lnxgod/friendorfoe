/**
 * Friend or Foe -- BLE Scanner OLED Display Implementation
 *
 * Drives an SSD1306 128x64 monochrome OLED over I2C.  Uses a 1024-byte
 * frame buffer and a built-in 5x7 pixel font for text rendering.
 *
 * Hardware:
 *   ESP32-S3: I2C, SDA=GPIO4, SCL=GPIO5, address 0x3C (default)
 *   ESP32-C5: I2C, SDA=GPIO6, SCL=GPIO7, address 0x3C (default)
 *   ESP32-C3: I2C, SDA=GPIO4, SCL=GPIO5, address 0x3C (default)
 *   Pins configurable via KConfig (menuconfig or sdkconfig overrides)
 */

#include "oled_display.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_private/esp_gpio_reserve.h"

static const char *TAG = "oled";

/* ── Hardware constants ────────────────────────────────────────────────── */

/*
 * OLED pin mapping per board:
 *   ESP32 (original): SDA=4, SCL=15, RST=16  (Heltec/TTGO ESP32 OLED)
 *   ESP32-S3:         SDA=4, SCL=5, RST=-1   (standard devkit)
 *   ESP32-C5:         SDA=6, SCL=7, RST=-1   (standard devkit)
 *   ESP32-C3:         SDA=4, SCL=5, RST=-1   (standard devkit)
 *
 * Override via KConfig or use the defaults below.
 */
#if CONFIG_IDF_TARGET_ESP32 && !defined(CONFIG_FOF_OLED_PINS_CUSTOM)
#define OLED_SDA_PIN    21
#define OLED_SCL_PIN    22
#define OLED_RST_PIN    -1
#else
#define OLED_SDA_PIN    CONFIG_FOF_OLED_SDA_PIN
#define OLED_SCL_PIN    CONFIG_FOF_OLED_SCL_PIN
#define OLED_RST_PIN    CONFIG_FOF_OLED_RST_PIN
#endif

#define OLED_ADDR       0x3C
#define OLED_WIDTH      128
#define OLED_HEIGHT     64
#define OLED_PAGES      (OLED_HEIGHT / 8)
#define OLED_BUF_SIZE   (OLED_WIDTH * OLED_PAGES)  /* 1024 bytes */

#define I2C_FREQ_HZ     100000   /* 100 kHz */

/* SSD1306 commands */
#define SSD1306_CMD_DISPLAY_OFF         0xAE
#define SSD1306_CMD_SET_MUX_RATIO       0xA8
#define SSD1306_CMD_SET_DISPLAY_OFFSET  0xD3
#define SSD1306_CMD_SET_START_LINE      0x40
#define SSD1306_CMD_SET_SEG_REMAP       0xA1
#define SSD1306_CMD_SET_COM_SCAN_DEC    0xC8
#define SSD1306_CMD_SET_COM_PINS        0xDA
#define SSD1306_CMD_SET_CONTRAST        0x81
#define SSD1306_CMD_RESUME_RAM          0xA4
#define SSD1306_CMD_NORMAL_DISPLAY      0xA6
#define SSD1306_CMD_SET_CLOCK_DIV       0xD5
#define SSD1306_CMD_SET_CHARGE_PUMP     0x8D
#define SSD1306_CMD_DISPLAY_ON          0xAF
#define SSD1306_CMD_SET_MEMORY_MODE     0x20
#define SSD1306_CMD_SET_COL_ADDR        0x21
#define SSD1306_CMD_SET_PAGE_ADDR       0x22

/* I2C control bytes */
#define OLED_CTRL_CMD       0x00
#define OLED_CTRL_DATA      0x40

/* ── State ─────────────────────────────────────────────────────────────── */

static i2c_master_bus_handle_t    s_bus_handle    = NULL;
static i2c_master_dev_handle_t    s_dev_handle    = NULL;
static uint8_t s_framebuf[OLED_BUF_SIZE];
static bool    s_initialized = false;
static int     s_error_count = 0;
#define OLED_MAX_ERRORS  5
static char    s_version[16] = "";

/* ── 5x7 ASCII font (printable chars 0x20-0x7E) ───────────────────────── */

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

/* ── Low-level I2C helpers ─────────────────────────────────────────────── */

static esp_err_t oled_send_cmd(uint8_t cmd)
{
    uint8_t buf[2] = { OLED_CTRL_CMD, cmd };
    return i2c_master_transmit(s_dev_handle, buf, sizeof(buf), 500);
}

static esp_err_t oled_send_data(const uint8_t *data, size_t len)
{
    uint8_t *buf = malloc(len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    buf[0] = OLED_CTRL_DATA;
    memcpy(buf + 1, data, len);

    esp_err_t err = i2c_master_transmit(s_dev_handle, buf, len + 1, 500);
    free(buf);
    return err;
}

/* ── Frame buffer operations ───────────────────────────────────────────── */

static void fb_clear(void)
{
    memset(s_framebuf, 0, OLED_BUF_SIZE);
}

static void fb_set_pixel(int x, int y)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;
    }
    s_framebuf[x + (y / 8) * OLED_WIDTH] |= (1 << (y % 8));
}

static int fb_draw_char(int x, int y, char c)
{
    if (c < 0x20 || c > 0x7E) {
        c = '?';
    }

    int idx = c - 0x20;
    for (int col = 0; col < 5; col++) {
        uint8_t column = font_5x7[idx][col];
        for (int row = 0; row < 7; row++) {
            if (column & (1 << row)) {
                fb_set_pixel(x + col, y + row);
            }
        }
    }

    return 6; /* 5 pixels + 1 pixel spacing */
}

static void fb_draw_string(int x, int y, const char *str)
{
    while (*str) {
        x += fb_draw_char(x, y, *str);
        str++;
    }
}

static void fb_draw_hline(int x0, int x1, int y)
{
    for (int x = x0; x <= x1; x++) {
        fb_set_pixel(x, y);
    }
}

static void fb_draw_signal_bars(int x, int y_bottom, int rssi)
{
    static const struct { int height; int threshold; } bars[4] = {
        { 3, -90 },
        { 5, -70 },
        { 7, -55 },
        { 9, -40 },
    };

    for (int b = 0; b < 4; b++) {
        if (rssi < bars[b].threshold) {
            continue;
        }
        int bx = x + b * 3;
        for (int row = 0; row < bars[b].height; row++) {
            fb_set_pixel(bx,     y_bottom - row);
            fb_set_pixel(bx + 1, y_bottom - row);
        }
    }
}

/* ── Flush frame buffer to display ─────────────────────────────────────── */

static void oled_flush(void)
{
    if (oled_send_cmd(SSD1306_CMD_SET_COL_ADDR) != ESP_OK) {
        if (++s_error_count >= OLED_MAX_ERRORS) {
            ESP_LOGW(TAG, "OLED disabled after %d I2C errors", s_error_count);
            s_initialized = false;
        }
        return;
    }
    oled_send_cmd(0x00);
    oled_send_cmd(0x7F);

    oled_send_cmd(SSD1306_CMD_SET_PAGE_ADDR);
    oled_send_cmd(0x00);
    oled_send_cmd(0x07);

    for (int page = 0; page < OLED_PAGES; page++) {
        oled_send_data(&s_framebuf[page * OLED_WIDTH], OLED_WIDTH);
    }
    s_error_count = 0;
}

/* ── Public API ────────────────────────────────────────────────────────── */

void oled_init(void)
{
#if CONFIG_IDF_TARGET_ESP32C5
    esp_gpio_revoke(BIT64(OLED_SDA_PIN) | BIT64(OLED_SCL_PIN));
    gpio_reset_pin(OLED_SDA_PIN);
    gpio_reset_pin(OLED_SCL_PIN);
#endif

    if (OLED_RST_PIN >= 0) {
        gpio_config_t rst_cfg = {
            .pin_bit_mask = 1ULL << OLED_RST_PIN,
            .mode         = GPIO_MODE_OUTPUT,
        };
        gpio_config(&rst_cfg);
        gpio_set_level(OLED_RST_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(OLED_RST_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port   = I2C_NUM_0,
        .sda_io_num = OLED_SDA_PIN,
        .scl_io_num = OLED_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = OLED_ADDR,
        .scl_speed_hz    = I2C_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(s_bus_handle, &dev_cfg, &s_dev_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C device add failed: %s", esp_err_to_name(err));
        return;
    }

    if (oled_send_cmd(SSD1306_CMD_DISPLAY_OFF) != ESP_OK) {
        ESP_LOGW(TAG, "No OLED detected at 0x%02X (SDA=%d SCL=%d) — display disabled",
                 OLED_ADDR, OLED_SDA_PIN, OLED_SCL_PIN);
        return;
    }

    oled_send_cmd(SSD1306_CMD_SET_MUX_RATIO);
    oled_send_cmd(0x3F);

    oled_send_cmd(SSD1306_CMD_SET_DISPLAY_OFFSET);
    oled_send_cmd(0x00);

    oled_send_cmd(SSD1306_CMD_SET_START_LINE);

    oled_send_cmd(SSD1306_CMD_SET_SEG_REMAP);
    oled_send_cmd(SSD1306_CMD_SET_COM_SCAN_DEC);

    oled_send_cmd(SSD1306_CMD_SET_COM_PINS);
    oled_send_cmd(0x12);

    oled_send_cmd(SSD1306_CMD_SET_CONTRAST);
    oled_send_cmd(0xCF);

    oled_send_cmd(SSD1306_CMD_RESUME_RAM);
    oled_send_cmd(SSD1306_CMD_NORMAL_DISPLAY);

    oled_send_cmd(SSD1306_CMD_SET_CLOCK_DIV);
    oled_send_cmd(0x80);

    oled_send_cmd(SSD1306_CMD_SET_CHARGE_PUMP);
    oled_send_cmd(0x14);

    oled_send_cmd(SSD1306_CMD_SET_MEMORY_MODE);
    oled_send_cmd(0x00);

    oled_send_cmd(SSD1306_CMD_DISPLAY_ON);

    fb_clear();
    oled_flush();

    s_initialized = true;
    ESP_LOGI(TAG, "OLED initialized (SSD1306 128x64, SDA=%d SCL=%d)",
             OLED_SDA_PIN, OLED_SCL_PIN);
}

void oled_set_version(const char *version)
{
    if (version) {
        snprintf(s_version, sizeof(s_version), "%s", version);
    }
}

void oled_update(int detection_count, int active_drones, uint8_t wifi_channel,
                 int ble_count, int wifi_count, uint32_t uptime_s)
{
    if (!s_initialized) {
        return;
    }

    fb_clear();

    /* Line 0: Title — "FoF BLE Scan" */
    char line[24];
    if (s_version[0]) {
        char short_ver[8];
        int dot_count = 0;
        int i;
        for (i = 0; s_version[i] && i < (int)sizeof(short_ver) - 1; i++) {
            if (s_version[i] == '.') {
                dot_count++;
                if (dot_count >= 2) break;
            }
            short_ver[i] = s_version[i];
        }
        short_ver[i] = '\0';
        snprintf(line, sizeof(line), "FoF BLE Scan v%s", short_ver);
    } else {
        snprintf(line, sizeof(line), "FoF BLE Scan");
    }
    fb_draw_string(0, 0, line);

    /* Line 1: Separator */
    fb_draw_hline(0, 107, 9);

    /* Line 2: Drones active */
    snprintf(line, sizeof(line), "Drones: %d", active_drones);
    fb_draw_string(0, 12, line);

    /* Line 3: BLE count */
    snprintf(line, sizeof(line), "BLE hits: %d", ble_count);
    fb_draw_string(0, 22, line);

    /* Line 4: Total + Uptime */
    uint32_t m = (uptime_s % 3600) / 60;
    uint32_t s = uptime_s % 60;
    if (uptime_s >= 3600) {
        uint32_t h = uptime_s / 3600;
        snprintf(line, sizeof(line), "Tot:%-4d %lu:%02lu:%02lu",
                 detection_count, (unsigned long)h,
                 (unsigned long)m, (unsigned long)s);
    } else {
        snprintf(line, sizeof(line), "Tot:%-5d Up:%02lu:%02lu",
                 detection_count,
                 (unsigned long)m, (unsigned long)s);
    }
    fb_draw_string(0, 32, line);

    oled_flush();
}

void oled_show_detection(const char *drone_id, const char *manufacturer,
                         float confidence, int rssi)
{
    if (!s_initialized) {
        return;
    }

    char line[24];

    fb_draw_hline(0, 127, 42);

    snprintf(line, sizeof(line), "%.17s", drone_id ? drone_id : "???");
    fb_draw_string(0, 45, line);
    fb_draw_signal_bars(116, 52, rssi);

    snprintf(line, sizeof(line), "%.6s %ddBm %d%%",
             (manufacturer && manufacturer[0]) ? manufacturer : "Unkn",
             rssi, (int)(confidence * 100.0f));
    fb_draw_string(0, 55, line);

    oled_flush();
}

void oled_show_detection_paged(const char *drone_id, const char *manufacturer,
                               float confidence, int rssi,
                               int page_current, int page_total)
{
    if (!s_initialized) {
        return;
    }

    char line[24];

    fb_draw_hline(0, 127, 42);

    if (page_total > 1) {
        snprintf(line, sizeof(line), "%.14s", drone_id ? drone_id : "???");
        fb_draw_string(0, 45, line);

        char page_str[8];
        snprintf(page_str, sizeof(page_str), "%d/%d", page_current, page_total);
        int len = (int)strlen(page_str);
        int px = 128 - len * 6;
        fb_draw_string(px, 45, page_str);
    } else {
        snprintf(line, sizeof(line), "%.17s", drone_id ? drone_id : "???");
        fb_draw_string(0, 45, line);
        fb_draw_signal_bars(116, 52, rssi);
    }

    snprintf(line, sizeof(line), "%.6s %ddBm %d%%",
             (manufacturer && manufacturer[0]) ? manufacturer : "Unkn",
             rssi, (int)(confidence * 100.0f));
    fb_draw_string(0, 55, line);

    oled_flush();
}

void oled_draw_drone_list(const oled_drone_entry_t *drones, int count,
                          int page_index)
{
    if (!s_initialized) return;

    fb_clear();
    char line[24];

    /* Header in yellow band (y=0..15): title + drone count */
    snprintf(line, sizeof(line), "FoF Scanner %d drone%s",
             count, count == 1 ? "" : "s");
    fb_draw_string(0, 3, line);

    if (count == 0) {
        fb_draw_string(0, 24, "Scanning...");
        fb_draw_string(0, 38, "No drones detected");
        oled_flush();
        return;
    }

    /* Drone data starts in blue band (y=18+).
     * Each drone: 3 lines x 10px = 30px. Blue area = y18..63 = 46px.
     * Fits 1 drone with room, page through if >1. */
    int start = page_index;
    if (start >= count) start = 0;
    int y = 18;

    for (int i = start; i < count && y < 60; i++) {
        const oled_drone_entry_t *d = &drones[i];

        /* Line 1: Short ID + RSSI */
        const char *short_id = d->id;
        /* Skip "rid_" prefix if present */
        if (short_id && strncmp(short_id, "rid_", 4) == 0) {
            short_id += 4;
        }
        snprintf(line, sizeof(line), "%.15s %ddB", short_id ? short_id : "???", d->rssi);
        fb_draw_string(0, y, line);
        y += 10;

        /* Line 2: Lat/Lon */
        snprintf(line, sizeof(line), "%.4f,%.4f", d->lat, d->lon);
        fb_draw_string(0, y, line);
        y += 10;

        /* Line 3: Alt + Speed */
        snprintf(line, sizeof(line), "Alt:%.0fm Spd:%.0fm/s",
                 d->alt_m, d->speed_mps);
        fb_draw_string(0, y, line);
        y += 12;  /* Extra gap between drones */
    }

    /* Page indicator in yellow header area */
    if (count > 1) {
        snprintf(line, sizeof(line), "%d/%d", start + 1, count);
        int px = 128 - (int)strlen(line) * 6;
        fb_draw_string(px, 3, line);
    }

    oled_flush();
}

void oled_clear(void)
{
    if (!s_initialized) {
        return;
    }

    fb_clear();
    oled_flush();
}

#if CONFIG_FOF_GLASSES_DETECTION
void oled_show_glasses_alert(const char *device_type, const char *manufacturer,
                              const char *device_name, int8_t rssi,
                              bool has_camera)
{
    if (!s_initialized) return;

    fb_clear();
    char line[24];

    /* Header in yellow band: ALERT */
    fb_draw_string(0, 3, "!! PRIVACY ALERT !!");

    /* Device info in blue band */
    int y = 18;

    /* Line 1: Device type */
    snprintf(line, sizeof(line), "%.21s", device_type);
    fb_draw_string(0, y, line);
    y += 10;

    /* Line 2: Manufacturer + RSSI */
    snprintf(line, sizeof(line), "%.14s  %ddB", manufacturer, rssi);
    fb_draw_string(0, y, line);
    y += 10;

    /* Line 3: Device name if available */
    if (device_name && device_name[0]) {
        snprintf(line, sizeof(line), "%.21s", device_name);
        fb_draw_string(0, y, line);
        y += 10;
    }

    /* Line 4: Camera indicator + distance estimate */
    const char *cam_str = has_camera ? "CAM: YES" : "CAM: no";
    const char *dist_str;
    if (rssi > -50)      dist_str = "~2m";
    else if (rssi > -60) dist_str = "~5m";
    else if (rssi > -70) dist_str = "~10m";
    else                 dist_str = ">10m";
    snprintf(line, sizeof(line), "%s  Dist:%s", cam_str, dist_str);
    fb_draw_string(0, y, line);

    oled_flush();
}
#endif

void oled_draw_status_bar(const char *status_text, uint32_t unused)
{
    if (!s_initialized) return;
    (void)unused;

    /* Draw pre-formatted status text at bottom row (y=56) */
    fb_draw_string(0, 56, status_text);
    oled_flush();
}
