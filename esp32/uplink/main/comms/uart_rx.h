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

/**
 * Send a JSON command to all connected scanners via UART TX.
 * Used for lock-on commands from the backend.
 */
void uart_rx_send_command(const char *json_cmd);

/** Scanner identity info (received via UART scanner_info message). */
typedef struct {
    char version[16];
    char board[24];     /* firmware catalog name: "scanner-s3-combo" */
    char chip[12];      /* "esp32s3", "esp32", "esp32c5" */
    char caps[32];      /* "ble,wifi", "wifi", "ble" */
    bool received;

    /* Attack / anomaly counters (latest delta from scanner status) */
    uint16_t deauth_count;
    uint16_t disassoc_count;
    uint16_t auth_count;
    bool     deauth_flood;
    bool     beacon_spam;
    char     fc_hist[128];   /* comma-separated frame subtype histogram */
} scanner_info_t;

/** Get scanner info for the BLE scanner (UART slot). */
const scanner_info_t *uart_rx_get_ble_scanner_info(void);

/** Get scanner info for the WiFi scanner (UART slot). */
const scanner_info_t *uart_rx_get_wifi_scanner_info(void);

/** Last OTA response received from a scanner (for relay diagnostics). */
typedef struct {
    char type[20];      /* "ota_ack", "ota_ok", "ota_nack", "ota_error", "ota_done" */
    char error[64];     /* error message if type==ota_error */
    uint32_t received;  /* bytes received (from progress/done) */
    int64_t timestamp;  /* when response was received (esp_timer) */
    int32_t seq;        /* chunk seq number (from ota_ok/ota_nack), -1 if N/A */
} ota_response_t;

/** Get the last OTA response from any scanner. Resets after read. */
ota_response_t uart_rx_get_last_ota_response(void);

/** Clear the OTA response buffer (call before starting relay). */
void uart_rx_clear_ota_response(void);

/** Pause/resume the UART RX task for a specific scanner during OTA relay.
 *  When paused, the relay handler can read ACKs directly from the UART. */
void uart_rx_pause_scanner(int scanner_id);
void uart_rx_resume_scanner(int scanner_id);

#ifdef __cplusplus
}
#endif
