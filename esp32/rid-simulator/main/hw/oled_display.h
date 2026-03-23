#pragma once

/**
 * Friend or Foe — RID Simulator OLED Display
 *
 * Drives an SSD1306 128x64 OLED over I2C to show simulator status.
 *
 * Hardware:
 *   ESP32-S3: GPIO4 (SDA) / GPIO5 (SCL)
 *   ESP32-C5: GPIO6 (SDA) / GPIO7 (SCL)
 *   ESP32-C3: GPIO4 (SDA) / GPIO5 (SCL)
 *   Address: 0x3C
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void oled_init(void);

/**
 * Update OLED with simulator state.
 *
 * @param lat       Current drone latitude
 * @param lon       Current drone longitude
 * @param alt_m     Current altitude in meters
 * @param heading   Current heading in degrees
 * @param adv_count Total advertisements sent
 * @param uptime_s  Uptime in seconds
 */
void oled_sim_update(double lat, double lon, double alt_m,
                      float heading, uint32_t adv_count, uint32_t uptime_s);

void oled_clear(void);

#ifdef __cplusplus
}
#endif
