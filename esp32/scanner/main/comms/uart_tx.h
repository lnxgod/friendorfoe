#pragma once

/**
 * Friend or Foe -- Scanner UART TX Module
 *
 * Sends drone detections and status messages to the Uplink board over
 * UART1 as newline-delimited JSON.  The TX task reads from the shared
 * detection queue, applies Bayesian fusion, serializes to JSON, and
 * writes bytes to the wire.
 *
 * Hardware: UART1, TX=GPIO17, RX=GPIO18, 921600 baud, 8N1
 */

#include "detection_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize UART1 hardware for scanner-to-uplink communication.
 * Configures baud rate, pin mapping, and installs the driver.
 * Must be called before uart_tx_start().
 */
void uart_tx_init(void);

/**
 * Serialize a single drone detection to JSON and transmit over UART.
 * The JSON object is terminated with '\n'.
 *
 * Thread-safe: uses uart_write_bytes which is internally locked.
 */
void uart_tx_send_detection(const drone_detection_t *detection);

/**
 * Send a periodic status heartbeat over UART.
 * Format: {"type":"status","ble_count":N,"wifi_count":N,"ch":N,"uptime_s":N}\n
 */
void uart_tx_send_status(int ble_count, int wifi_count,
                         uint8_t current_channel, uint32_t uptime_s);

/**
 * Start the UART TX FreeRTOS task.
 *
 * The task blocks on @p detection_queue, applies Bayesian fusion to each
 * incoming detection, serializes the result, and transmits it.  Every
 * PRUNE_INTERVAL_MS it also prunes stale tracks and sends a status message.
 *
 * @param detection_queue  FreeRTOS queue carrying drone_detection_t items
 *                         produced by WiFi and BLE scanner tasks.
 */
void uart_tx_start(QueueHandle_t detection_queue);

#ifdef __cplusplus
}
#endif
