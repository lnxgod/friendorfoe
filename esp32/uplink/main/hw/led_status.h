#pragma once

/**
 * Friend or Foe -- Uplink Status LED
 *
 * Drives a single GPIO LED with configurable blink patterns to
 * indicate system state at a glance.
 *
 * Hardware: GPIO8
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_IDLE,       /* Slow blink (1Hz)         */
    LED_SCANNING,   /* Fast blink (4Hz)         */
    LED_DETECTION,  /* Solid on for 2s          */
    LED_UPLOADING,  /* Double blink             */
    LED_ERROR,      /* Triple blink             */
    LED_NO_GPS,     /* Very slow blink (0.5Hz)  */
} led_pattern_t;

/**
 * Initialize GPIO for the status LED.
 */
void led_init(void);

/**
 * Set the current LED blink pattern.
 * Thread-safe (atomic update).
 */
void led_set_pattern(led_pattern_t pattern);

/**
 * Start the LED blink FreeRTOS task.
 */
void led_start(void);

#ifdef __cplusplus
}
#endif
