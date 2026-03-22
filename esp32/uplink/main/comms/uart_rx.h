#pragma once

/**
 * Friend or Foe -- Uplink UART RX Module
 *
 * Receives newline-delimited JSON messages from the Scanner board over
 * UART1.  Detection messages are parsed into drone_detection_t structs
 * and pushed onto a FreeRTOS queue for the HTTP upload task to consume.
 *
 * Hardware: UART1, RX=GPIO20, TX=GPIO21, 921600 baud, 8N1
 */

#include "detection_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize UART1 hardware for receiving scanner messages.
 * Configures baud rate, pin mapping, and installs the driver.
 *
 * @param detection_queue  FreeRTOS queue to push parsed drone_detection_t items
 */
void uart_rx_init(QueueHandle_t detection_queue);

/**
 * Start the UART RX FreeRTOS task.
 * The task reads bytes from UART, accumulates lines, parses JSON,
 * and enqueues detections.
 */
void uart_rx_start(void);

/**
 * Get the total number of detection messages received since boot.
 */
int uart_rx_get_detection_count(void);

/**
 * Summary of a recent detection for the status page.
 */
typedef struct {
    char    drone_id[64];
    uint8_t source;
    float   confidence;
    int8_t  rssi;
    int64_t timestamp_ms;   /* esp_timer_get_time() / 1000 at detection time */
} detection_summary_t;

/**
 * Copy the most recent detections into the caller's buffer.
 *
 * @param out  Output array
 * @param max  Maximum entries to copy
 * @return Number of entries written (0..max), newest first
 */
int uart_rx_get_recent_detections(detection_summary_t *out, int max);

/**
 * Check if the scanner board is connected (sent a message within the last 5s).
 */
bool uart_rx_is_scanner_connected(void);

#ifdef __cplusplus
}
#endif
