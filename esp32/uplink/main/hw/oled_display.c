/**
 * Friend or Foe -- Uplink OLED Display Implementation
 *
 * Drives an SSD1306 128x64 monochrome OLED over I2C.  Uses a 1024-byte
 * frame buffer and a built-in 5x7 pixel font for text rendering.
 *
 * Hardware: I2C, address 0x3C, pins configurable via KConfig
 */

#include "oled_display.h"
#include "uart_rx.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "oled";

/* ── Hardware constants ────────────────────────────────────────────────── */

#define OLED_SDA_PIN    CONFIG_FOF_OLED_SDA_PIN
#define OLED_SCL_PIN    CONFIG_FOF_OLED_SCL_PIN
#define OLED_RST_PIN    CONFIG_FOF_OLED_RST_PIN
#define OLED_ADDR       0x3C
#define OLED_WIDTH      128
#define OLED_HEIGHT     64
#define OLED_PAGES      (OLED_HEIGHT / 8)
#define OLED_BUF_SIZE   (OLED_WIDTH * OLED_PAGES)  /* 1024 bytes */

#define I2C_FREQ_HZ     100000   /* 100 kHz — some OLED boards unreliable at 400 kHz */

/* SSD1306 commands */
#define SSD1306_CMD_DISPLAY_OFF         0xAE
#define SSD1306_CMD_SET_MUX_RATIO       0xA8
#define SSD1306_CMD_SET_DISPLAY_OFFSET  0xD3
#define SSD1306_CMD_SET_START_LINE      0x40
#define SSD1306_CMD_SET_SEG_REMAP       0xA1    /* column 127 = SEG0 */
#define SSD1306_CMD_SET_COM_SCAN_DEC    0xC8    /* scan COM63 to COM0 */
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
#define OLED_CTRL_CMD       0x00    /* Co=0, D/C#=0: command */
#define OLED_CTRL_DATA      0x40    /* Co=0, D/C#=1: data */

/* ── State ─────────────────────────────────────────────────────────────── */

static i2c_master_bus_handle_t    s_bus_handle    = NULL;
static i2c_master_dev_handle_t    s_dev_handle    = NULL;
static uint8_t s_framebuf[OLED_BUF_SIZE];
static bool    s_initialized = false;

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
    /*
     * I2C transmission: [control_byte] [data0] [data1] ...
     * Stack-local buffer avoids heap fragmentation from repeated malloc/free.
     * Max len is OLED_WIDTH (128), so 129 bytes on stack is safe.
     */
    uint8_t buf[OLED_WIDTH + 1];
    if (len > OLED_WIDTH) {
        len = OLED_WIDTH;
    }
    buf[0] = OLED_CTRL_DATA;
    memcpy(buf + 1, data, len);

    return i2c_master_transmit(s_dev_handle, buf, len + 1, 500);
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

/**
 * Draw a character at pixel position (x, y) using the 5x7 font.
 * Returns the x advance (character width + spacing).
 */
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

/**
 * Draw a null-terminated string at pixel position (x, y).
 */
static void fb_draw_string(int x, int y, const char *str)
{
    while (*str) {
        x += fb_draw_char(x, y, *str);
        str++;
    }
}

/**
 * Draw a horizontal line from (x0, y) to (x1, y).
 */
static void fb_draw_hline(int x0, int x1, int y)
{
    for (int x = x0; x <= x1; x++) {
        fb_set_pixel(x, y);
    }
}

/* ── Flush frame buffer to display ─────────────────────────────────────── */

static void oled_flush(void)
{
    /* Set column address range: 0-127 */
    oled_send_cmd(SSD1306_CMD_SET_COL_ADDR);
    oled_send_cmd(0x00);
    oled_send_cmd(0x7F);

    /* Set page address range: 0-7 */
    oled_send_cmd(SSD1306_CMD_SET_PAGE_ADDR);
    oled_send_cmd(0x00);
    oled_send_cmd(0x07);

    /*
     * Send the frame buffer in chunks to avoid exceeding I2C
     * transaction limits.
     */
    for (int page = 0; page < OLED_PAGES; page++) {
        oled_send_data(&s_framebuf[page * OLED_WIDTH], OLED_WIDTH);
    }
}

/* ── Public API ────────────────────────────────────────────────────────── */

/*
 * Common ESP32 OLED I2C pin combinations to try:
 *   Heltec WiFi Kit 32 V2: SDA=4,  SCL=15
 *   TTGO LoRa32 V2:        SDA=21, SCL=22
 *   Generic / default:     SDA=21, SCL=22
 *   Heltec V3 / some:      SDA=17, SCL=18
 */
typedef struct { int sda; int scl; const char *board; } i2c_pin_combo_t;

static const i2c_pin_combo_t s_pin_combos[] = {
    { OLED_SDA_PIN, OLED_SCL_PIN, "configured" },
    { 8,  9,  "ESP32-S3 OLED" },
    { 21, 22, "default/TTGO" },
    /* NOTE: 17/18 removed — those are UART pins on S3 uplink */
    { 5,  4,  "alt wiring" },
};
#define NUM_PIN_COMBOS (sizeof(s_pin_combos) / sizeof(s_pin_combos[0]))

static bool try_i2c_scan(int sda, int scl, const char *label, uint8_t *out_addr)
{
    /* Release any previous bus */
    if (s_dev_handle) {
        i2c_master_bus_rm_device(s_dev_handle);
        s_dev_handle = NULL;
    }
    if (s_bus_handle) {
        i2c_del_master_bus(s_bus_handle);
        s_bus_handle = NULL;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port   = I2C_NUM_0,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "  I2C bus init failed on SDA=%d SCL=%d: %s",
                 sda, scl, esp_err_to_name(err));
        return false;
    }

    /* Probe for SSD1306 at 0x3C and 0x3D */
    for (uint8_t addr = 0x3C; addr <= 0x3D; addr++) {
        err = i2c_master_probe(s_bus_handle, addr, 100);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "  OLED found at 0x%02X on SDA=%d SCL=%d (%s)",
                     addr, sda, scl, label);
            *out_addr = addr;
            return true;
        }
    }

    return false;
}

void oled_init(void)
{
    /* Toggle hardware reset pin if configured */
    int rst_pin = OLED_RST_PIN;
    if (rst_pin >= 0) {
        gpio_config_t rst_cfg = {
            .pin_bit_mask = 1ULL << rst_pin,
            .mode         = GPIO_MODE_OUTPUT,
        };
        gpio_config(&rst_cfg);
        gpio_set_level(rst_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(rst_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* Try each pin combo until we find the OLED */
    ESP_LOGI(TAG, "Scanning for OLED display...");
    uint8_t found_addr = 0;
    bool found = false;
    int found_sda = 0, found_scl = 0;

    for (int i = 0; i < (int)NUM_PIN_COMBOS; i++) {
        const i2c_pin_combo_t *combo = &s_pin_combos[i];
        if (try_i2c_scan(combo->sda, combo->scl, combo->board, &found_addr)) {
            found = true;
            found_sda = combo->sda;
            found_scl = combo->scl;
            break;
        }
    }

    if (!found) {
        ESP_LOGE(TAG, "No OLED found on any I2C pin combination!");
        ESP_LOGE(TAG, "Tried: SDA/SCL = 4/15, 21/22, 17/18, 5/4");
        return;
    }

    /* Add the SSD1306 device at the found address */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = found_addr,
        .scl_speed_hz    = I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(s_bus_handle, &dev_cfg, &s_dev_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C device add failed: %s", esp_err_to_name(err));
        return;
    }

    /* SSD1306 initialization sequence */
    err = oled_send_cmd(SSD1306_CMD_DISPLAY_OFF);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "First SSD1306 command failed: %s — display not responding",
                 esp_err_to_name(err));
        return;
    }

    oled_send_cmd(SSD1306_CMD_SET_MUX_RATIO);
    oled_send_cmd(0x3F);                         /* 64 lines */

    oled_send_cmd(SSD1306_CMD_SET_DISPLAY_OFFSET);
    oled_send_cmd(0x00);

    oled_send_cmd(SSD1306_CMD_SET_START_LINE);   /* start line 0 */

    oled_send_cmd(SSD1306_CMD_SET_SEG_REMAP);    /* segment remap */
    oled_send_cmd(SSD1306_CMD_SET_COM_SCAN_DEC); /* COM scan direction */

    oled_send_cmd(SSD1306_CMD_SET_COM_PINS);
    oled_send_cmd(0x12);                         /* alt COM pin config */

    oled_send_cmd(SSD1306_CMD_SET_CONTRAST);
    oled_send_cmd(0xCF);

    oled_send_cmd(SSD1306_CMD_RESUME_RAM);       /* output follows RAM */
    oled_send_cmd(SSD1306_CMD_NORMAL_DISPLAY);

    oled_send_cmd(SSD1306_CMD_SET_CLOCK_DIV);
    oled_send_cmd(0x80);                         /* default clock */

    oled_send_cmd(SSD1306_CMD_SET_CHARGE_PUMP);
    oled_send_cmd(0x14);                         /* enable charge pump */

    oled_send_cmd(SSD1306_CMD_SET_MEMORY_MODE);
    oled_send_cmd(0x00);                         /* horizontal addressing */

    oled_send_cmd(SSD1306_CMD_DISPLAY_ON);

    /* Clear display */
    fb_clear();
    oled_flush();

    s_initialized = true;
    ESP_LOGI(TAG, "OLED initialized (SSD1306 128x64, addr=0x%02X, SDA=%d SCL=%d)",
             found_addr, found_sda, found_scl);
}

void oled_update(int drone_count, bool ble_scanner_ok, bool wifi_scanner_ok,
                 bool backend_ok, int upload_count, bool wifi_network_ok,
                 float battery_pct, uint32_t uptime_s, const char *device_id)
{
    if (!s_initialized) {
        return;
    }

    fb_clear();

    char line[24];

    /* Line 0: Node ID */
    if (device_id && device_id[0]) {
        snprintf(line, sizeof(line), "%.21s", device_id);
    } else {
        snprintf(line, sizeof(line), "FoF Uplink");
    }
    fb_draw_string(0, 0, line);

    /* Line 1: Separator */
    fb_draw_hline(0, 127, 9);

    /* Line 2: WiFi network + Backend server status */
    snprintf(line, sizeof(line), "Net:%s Svr:%s",
             wifi_network_ok ? "OK" : "--",
             backend_ok ? "OK" : "--");
    fb_draw_string(0, 12, line);

    /* Line 3: Scanner versions (or connection status) */
    {
        const scanner_info_t *ble_info = uart_rx_get_ble_scanner_info();
        const scanner_info_t *wifi_info = uart_rx_get_wifi_scanner_info();
        snprintf(line, sizeof(line), "B:%s W:%s",
                 ble_info ? ble_info->version : (ble_scanner_ok ? "ok" : "--"),
                 wifi_info ? wifi_info->version : (wifi_scanner_ok ? "ok" : "--"));
    }
    fb_draw_string(0, 22, line);

    /* Line 4: Drones + Uploads */
    snprintf(line, sizeof(line), "Drones:%-3d Up:%d",
             drone_count, upload_count);
    fb_draw_string(0, 32, line);

    /* Line 5: Battery + Uptime */
    uint32_t m = (uptime_s % 3600) / 60;
    uint32_t s = uptime_s % 60;
    snprintf(line, sizeof(line), "Bat:%d%% %02lu:%02lu",
             (int)battery_pct, (unsigned long)m, (unsigned long)s);
    fb_draw_string(0, 42, line);

    /* Line 6: Status indicator — prioritized */
    if (!wifi_network_ok) {
        fb_draw_string(0, 55, "! NO NETWORK");
    } else if (!backend_ok && upload_count == 0) {
        fb_draw_string(0, 55, "! SVR OFFLINE");
    } else if (!ble_scanner_ok && !wifi_scanner_ok) {
        fb_draw_string(0, 55, "! NO SCANNERS");
    } else if (drone_count > 0) {
        fb_draw_string(0, 55, "* TRACKING");
    } else {
        fb_draw_string(0, 55, "  SCANNING...");
    }

    oled_flush();
}

void oled_show_detection(const char *drone_id, const char *manufacturer,
                         float confidence, int rssi)
{
    if (!s_initialized) {
        return;
    }

    /* Clear bottom portion of framebuffer (pages 5-7, y=40..63) */
    memset(&s_framebuf[5 * OLED_WIDTH], 0, 3 * OLED_WIDTH);

    char line[24];

    /* Separator at y=41 */
    fb_draw_hline(0, 127, 41);

    /* Drone ID (truncated) */
    snprintf(line, sizeof(line), "ID:%.17s", drone_id ? drone_id : "???");
    fb_draw_string(0, 44, line);

    /* Manufacturer + RSSI */
    snprintf(line, sizeof(line), "%.8s %ddBm %.0f%%",
             (manufacturer && manufacturer[0]) ? manufacturer : "Unknown",
             rssi, confidence * 100.0f);
    fb_draw_string(0, 55, line);

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
