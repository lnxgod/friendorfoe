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
 * Clear the display to all black.
 */
void oled_clear(void);

#ifdef __cplusplus
}
#endif
