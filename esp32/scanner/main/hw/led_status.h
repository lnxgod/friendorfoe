#pragma once

/**
 * Friend or Foe -- Scanner Status LED
 *
 * Drives a single GPIO LED with configurable blink patterns to
 * indicate system state at a glance.
 *
 * Hardware:
 *   ESP32-S3: GPIO48 (built-in RGB LED on DevKitC-1, driven as on/off)
 *   ESP32-C5: GPIO27 (RGB LED pin on C5 dev boards)
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_BOOT,       /* 3 fast blinks then off   */
    LED_IDLE,       /* Slow blink (1Hz)         */
    LED_SCANNING,   /* Fast blink (4Hz)         */
    LED_DETECTION,  /* Solid on for 2s          */
    LED_UART_OK,    /* Single short pulse / 2s  */
    LED_ERROR,      /* Triple blink             */
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
