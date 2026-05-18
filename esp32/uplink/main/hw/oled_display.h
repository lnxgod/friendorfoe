#pragma once

/**
 * Friend or Foe -- Uplink OLED Display
 *
 * Drives an SSD1306 128x64 OLED over I2C to show system status,
 * detection counts, GPS/WiFi state, and battery level.
 *
 * Hardware: I2C, SDA=GPIO4, SCL=GPIO5, address 0x3C
 */

#include <stdint.h>
#include <stdbool.h>

#define OLED_BADGE_STATE_TEXT_LEN 80
#define OLED_BADGE_STATE_KEY_LEN 64
#define OLED_BADGE_STATE_EVIDENCE_LEN 48

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize I2C and the SSD1306 display controller.
 */
void oled_init(void);

/**
 * Redraw the main status screen with uplink + node + detection info.
 *
 * @param detection_count   Total detections/events since boot
 * @param ble_scanner_ok    Whether BLE scanner board is connected via UART
 * @param wifi_scanner_ok   Whether WiFi scanner board is connected via UART
 * @param backend_ok        Whether backend server is reachable
 * @param upload_count      Total successful uploads
 * @param wifi_network_ok   Whether WiFi STA is connected to the network
 * @param battery_pct       Battery level 0.0-100.0
 * @param uptime_s          System uptime in seconds
 * @param device_id         This node's device ID (e.g. "fof_node_1")
 */
void oled_update(int detection_count, bool ble_scanner_ok, bool wifi_scanner_ok,
                 bool backend_ok, int upload_count, bool wifi_network_ok,
                 float battery_pct, uint32_t uptime_s, const char *device_id);

/**
 * Show an early boot/status screen. Badge builds render this before Wi-Fi
 * comes up; non-badge OLED builds may ignore it.
 */
void oled_show_boot_status(const char *stage, const char *mode, const char *line);

/**
 * Briefly show the latest detection on screen.
 *
 * @param detection_id  Detection identifier
 * @param manufacturer  Manufacturer name (may be empty)
 * @param source        DETECTION_SRC_* source code
 * @param confidence    Detection confidence 0.0-1.0
 * @param rssi          Signal strength in dBm
 */
void oled_show_detection(const char *detection_id, const char *manufacturer,
                         uint8_t source, float confidence, int rssi);

typedef struct {
    bool active;
    bool detail_mode;
    int detail_page;
    int focus_index;
    int focus_total;
    int item_index;
    int item_total;
    char lane[16];
    char title[32];
    char detail[OLED_BADGE_STATE_TEXT_LEN];
    char evidence[OLED_BADGE_STATE_EVIDENCE_LEN];
    char entity_key[OLED_BADGE_STATE_KEY_LEN];
    char display_id[24];
    char threat_class[16];
    char category[16];
    char code[8];
    char source[24];
    int score;
    int confidence_pct;
    int evidence_quality;
    int display_rank;
    int age_s;
    int last_seen_s;
    int rssi;
    int best_rssi;
    uint32_t events;
    uint32_t seen_count;
    uint32_t group_count;
    int proximity_level;
    bool stale;
    bool has_location;
    double latitude;
    double longitude;
    float altitude_m;
    bool has_operator_location;
    double operator_lat;
    double operator_lon;
    char operator_id[32];
} oled_badge_display_state_t;

typedef struct {
    bool b1_active_high;
    int b1_raw_level;
    bool b1_raw_pressed;
    bool b1_stable_pressed;
    bool b1_boot_ignored;
    uint32_t b1_raw_edges;
    uint32_t b1_short_presses;
    uint32_t b1_long_presses;
    uint32_t b1_releases;
    int64_t b1_last_event_ms;
    bool b2_active_high;
    int b2_raw_level;
    bool b2_raw_pressed;
    bool b2_stable_pressed;
    bool b2_boot_ignored;
    uint32_t b2_raw_edges;
    uint32_t b2_short_presses;
    uint32_t b2_double_taps;
    uint32_t b2_long_presses;
    uint32_t b2_releases;
    int64_t b2_last_event_ms;
    bool b2_pending_single;
    char b2_last_gesture[16];
} oled_badge_button_state_t;

bool oled_badge_get_display_state(oled_badge_display_state_t *out);
bool oled_badge_get_button_state(oled_badge_button_state_t *out);
bool oled_badge_handle_nav_command(const char *action);

/**
 * Clear the display to all black.
 */
void oled_clear(void);

#ifdef __cplusplus
}
#endif
