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

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize I2C and the SSD1306 display controller.
 */
void oled_init(void);

/**
 * Redraw the main status screen.
 *
 * @param drone_count     Number of currently tracked drones
 * @param gps_fix         Whether GPS has a valid fix
 * @param wifi_connected  Whether WiFi STA is connected
 * @param battery_pct     Battery level 0.0-100.0
 * @param upload_count    Total successful uploads
 */
void oled_update(int drone_count, bool gps_fix, bool wifi_connected,
                 float battery_pct, int upload_count);

/**
 * Briefly show the latest detection on screen.
 *
 * @param drone_id      Drone serial number or generated ID
 * @param manufacturer  Manufacturer name (may be empty)
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
