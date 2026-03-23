#pragma once

/**
 * Friend or Foe — RID Simulator Status LED
 *
 * Drives a single WS2812 RGB LED with configurable blink patterns.
 *
 * Hardware:
 *   ESP32-S3: GPIO48
 *   ESP32-C5: GPIO27
 *   ESP32-C3: GPIO8
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_BOOT,           /* 3 fast blinks then off   */
    LED_IDLE,           /* Slow blink (1Hz)         */
    LED_SIMULATING,     /* Purple slow blink        */
    LED_ERROR,          /* Triple blink             */
} led_pattern_t;

void led_init(void);
void led_set_pattern(led_pattern_t pattern);
void led_start(void);

#ifdef __cplusplus
}
#endif
