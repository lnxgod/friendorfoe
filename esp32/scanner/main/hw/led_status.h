#pragma once

/**
 * Friend or Foe -- Scanner Status LED
 *
 * Drives a single GPIO LED with configurable blink patterns to
 * indicate system state at a glance.
 *
 * Hardware: ESP32-S3 GPIO48 built-in RGB LED.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_BOOT,        /* Blue blinks              */
    LED_IDLE,        /* Dim green slow blink     */
    LED_SCANNING,    /* Green fast blink         */
    LED_DETECTION,   /* Red solid                */
    LED_UART_OK,     /* Cyan pulse               */
    LED_ERROR,       /* Red triple blink         */
    LED_UPLINK_OK,   /* Purple pulse — UART flowing to uplink */
    LED_NO_UPLINK,   /* Red slow blink — no UART connection   */
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
