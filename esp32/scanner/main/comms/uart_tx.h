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

#include <stdbool.h>
#include <stdint.h>
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

/**
 * Summary of the most recent detection, cached for display.
 */
typedef struct {
    char    drone_id[64];
    char    manufacturer[32];
    float   confidence;
    int8_t  rssi;
    int64_t timestamp_ms;
} scanner_detection_summary_t;

/**
 * Send scanner identity info over UART (thread-safe, uses mutex).
 * Called at boot and periodically so the uplink always knows what's connected.
 */
void uart_tx_send_scanner_info(const char *ver, const char *board,
                                const char *chip, const char *caps);

/**
 * Store scanner identity for deferred sending after TX startup delay.
 * Call this before uart_tx_start(). The TX task sends it after its
 * 10s delay to avoid flooding the uplink during boot.
 */
void uart_tx_set_identity(const char *board, const char *chip, const char *caps);

/** Get cumulative BLE detection count. */
int uart_tx_get_ble_count(void);

/** Get cumulative WiFi detection count. */
int uart_tx_get_wifi_count(void);

/** Get total detection count (BLE + WiFi). */
int uart_tx_get_total_count(void);

/** Get current WiFi channel being scanned. */
uint8_t uart_tx_get_current_channel(void);

/**
 * Copy the most recent detection summary.
 * @return true if a detection has been cached, false if none yet
 */
bool uart_tx_get_recent_detection(scanner_detection_summary_t *out);

/** Maximum number of cached detections for display scoreboard. */
#define DETECTION_CACHE_SIZE 10

/**
 * Copy all cached detections into @p out, sorted most-recent first.
 * @param out        Caller-allocated array of at least @p max_count entries
 * @param max_count  Maximum entries to copy
 * @return           Number of entries actually copied (0..max_count)
 */
int uart_tx_get_cached_detections(scanner_detection_summary_t *out, int max_count);

#ifdef __cplusplus
}
#endif
