#pragma once

/**
 * Friend or Foe -- BLE Scanner OLED Display
 *
 * Drives an SSD1306 128x64 OLED over I2C to show scanner status,
 * detection counts, and detection overlays.
 *
 * Hardware:
 *   ESP32-S3: GPIO4 (SDA) / GPIO5 (SCL)
 *   ESP32-C5: GPIO6 (SDA) / GPIO7 (SCL)
 *   ESP32-C3: GPIO4 (SDA) / GPIO5 (SCL)
 *   Address: 0x3C
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void oled_init(void);
void oled_set_version(const char *version);
void oled_update(int detection_count, int active_drones, uint8_t wifi_channel,
                 int ble_count, int wifi_count, uint32_t uptime_s);
void oled_show_detection(const char *drone_id, const char *manufacturer,
                         float confidence, int rssi);
void oled_show_detection_paged(const char *drone_id, const char *manufacturer,
                               float confidence, int rssi,
                               int page_current, int page_total);
void oled_clear(void);

#ifdef __cplusplus
}
#endif
