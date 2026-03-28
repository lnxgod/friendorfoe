#pragma once

/**
 * Friend or Foe -- Uplink UART RX Module
 *
 * Receives newline-delimited JSON messages from scanner boards over UART.
 *
 * Dual-scanner mode (plain ESP32 uplink):
 *   UART1 (RX=GPIO25) — BLE scanner (ESP32-S3)
 *   UART2 (RX=GPIO32) — WiFi scanner (ESP32-C5)
 *
 * Single-scanner mode (ESP32-C3 uplink):
 *   UART1 (RX=GPIO20) — combined scanner
 */

#include "detection_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize UART hardware for receiving scanner messages.
 * On plain ESP32: initializes both UART1 (BLE) and UART2 (WiFi).
 * On ESP32-C3: initializes UART1 only.
 */
void uart_rx_init(QueueHandle_t detection_queue);

/**
 * Start the UART RX FreeRTOS task(s).
 */
void uart_rx_start(void);

/** Total detections received since boot. */
int uart_rx_get_detection_count(void);

/** Summary of a recent detection for display. */
typedef struct {
    char    drone_id[64];
    uint8_t source;
    float   confidence;
    int8_t  rssi;
    int64_t timestamp_ms;
} detection_summary_t;

/** Copy most recent detections into caller's buffer (newest first). */
int uart_rx_get_recent_detections(detection_summary_t *out, int max);

/** True if ANY scanner is connected (sent data within 5s). */
bool uart_rx_is_scanner_connected(void);

/** True if the BLE scanner (UART1) is connected. */
bool uart_rx_is_ble_scanner_connected(void);

/** True if the WiFi scanner (UART2) is connected. */
bool uart_rx_is_wifi_scanner_connected(void);

#ifdef __cplusplus
}
#endif
