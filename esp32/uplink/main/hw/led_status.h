#pragma once

/**
 * Friend or Foe -- Uplink Status LED
 *
 * ESP32-S3 WS2812 RGB LED on GPIO48 with colour-coded status.
 *
 * Colour key (S3 RGB):
 *   SOLID GREEN        = WiFi + server + scanners all good
 *   YELLOW pulse       = WiFi up, server unreachable
 *   GREEN blink        = WiFi up, no server, scanners OK
 *   RED/YELLOW flash   = WiFi down
 *   PURPLE pulse       = Scanner detected drone
 *   BLUE blink         = No scanners connected
 *   RED triple blink   = Error
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_IDLE,           /* Slow blink / dim green           */
    LED_SCANNING,       /* Active scanning / green blink    */
    LED_DETECTION,      /* Drone detected / purple pulse    */
    LED_UPLOADING,      /* Double blink                     */
    LED_ERROR,          /* Red triple blink                 */
    LED_NO_GPS,         /* Very slow blink                  */
    LED_NO_SCANNER,     /* Blue blink — no scanners         */
    LED_ALL_GOOD,       /* Solid green — everything OK      */
    LED_NO_SERVER,      /* Yellow pulse — WiFi but no server */
    LED_WIFI_DOWN,      /* Red/yellow alternating           */
} led_pattern_t;

void led_init(void);
void led_set_pattern(led_pattern_t pattern);
void led_start(void);

#ifdef __cplusplus
}
#endif
