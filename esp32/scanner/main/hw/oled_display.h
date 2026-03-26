#pragma once

/**
 * Friend or Foe -- Scanner OLED Display
 *
 * Drives an SSD1306 128x64 OLED over I2C to show scanner status,
 * detection counts, channel info, and detection overlays.
 *
 * Hardware:
 *   ESP32-S3: GPIO4 (SDA) / GPIO5 (SCL)
 *   ESP32-C5: GPIO6 (SDA) / GPIO7 (SCL)  (GPIO4/5 are UART on C5)
 *   Address: 0x3C
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize I2C and the SSD1306 display controller.
 */
void oled_init(void);

/**
 * Set the firmware version string shown in the title line.
 * Call once before oled_update(). The string is copied internally.
 *
 * @param version  Version string, e.g. "0.10.0-beta"
 */
void oled_set_version(const char *version);

/**
 * Redraw the main scanner status screen.
 *
 * @param detection_count  Total detections since boot
 * @param active_drones    Currently tracked drone count (Bayesian slots in use)
 * @param wifi_channel     Current WiFi scan channel
 * @param ble_count        Cumulative BLE detections
 * @param wifi_count       Cumulative WiFi detections
 * @param uptime_s         System uptime in seconds
 */
void oled_update(int detection_count, int active_drones, uint8_t wifi_channel,
                 int ble_count, int wifi_count, uint32_t uptime_s);

/**
 * Briefly show the latest detection on screen (bottom half overlay).
 *
 * @param drone_id      Drone serial number or generated ID
 * @param manufacturer  Manufacturer name (may be NULL/empty)
 * @param confidence    Detection confidence 0.0-1.0
 * @param rssi          Signal strength in dBm
 */
void oled_show_detection(const char *drone_id, const char *manufacturer,
                         float confidence, int rssi);

/**
 * Show a detection with a page indicator for multi-drone scoreboard.
 *
 * When page_total == 1, behaves identically to oled_show_detection
 * (full 17-char ID + signal bars). When page_total > 1, truncates
 * drone_id to 14 chars and draws a right-aligned "N/M" indicator.
 *
 * @param drone_id      Drone serial number or generated ID
 * @param manufacturer  Manufacturer name (may be NULL/empty)
 * @param confidence    Detection confidence 0.0-1.0
 * @param rssi          Signal strength in dBm
 * @param page_current  Current page (1-based)
 * @param page_total    Total number of pages
 */
void oled_show_detection_paged(const char *drone_id, const char *manufacturer,
                               float confidence, int rssi,
                               int page_current, int page_total);

/**
 * Show a privacy/glasses detection with paged view.
 * Full-screen layout: title "Privacy Scan", device info, page indicator.
 *
 * @param device_type   e.g. "Smart Glasses", "Body Camera"
 * @param manufacturer  e.g. "Meta", "Snap"
 * @param device_name   BLE advertised name (may be NULL)
 * @param confidence    Detection confidence 0.0-1.0
 * @param rssi          Signal strength in dBm
 * @param has_camera    true if device has camera
 * @param page_current  Current page (1-based)
 * @param page_total    Total number of pages
 */
void oled_show_glasses_paged(const char *device_type, const char *manufacturer,
                              const char *device_name, float confidence,
                              int rssi, bool has_camera,
                              int page_current, int page_total);

/**
 * Show privacy scan status header (no detections).
 *
 * @param glasses_count  Number of privacy devices detected
 * @param uptime_s       System uptime in seconds
 */
void oled_show_privacy_status(int glasses_count, uint32_t uptime_s);

/**
 * Clear the display to all black.
 */
void oled_clear(void);

#ifdef __cplusplus
}
#endif
