#pragma once

/**
 * Friend or Foe -- Uplink UART RX Module
 *
 * Receives newline-delimited JSON messages from scanner boards over the two
 * ESP32-S3 uplink UART slots.
 */

#include "detection_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize UART hardware for receiving scanner messages.
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
    char chip[12];      /* "esp32s3" */
    char caps[32];      /* "ble,wifi" */
    bool received;

    /* Attack / anomaly counters (latest delta from scanner status) */
    uint16_t deauth_count;
    uint16_t disassoc_count;
    uint16_t auth_count;
    bool     deauth_flood;
    bool     beacon_spam;
    char     fc_hist[128];   /* comma-separated frame subtype histogram */
    uint32_t uart_tx_dropped;
    uint32_t uart_tx_high_water;
    uint32_t tx_queue_depth;
    uint32_t tx_queue_capacity;
    uint32_t tx_queue_pressure_pct;
    uint32_t noise_drop_ble;
    uint32_t noise_drop_wifi;
    uint32_t probe_seen;
    uint32_t probe_sent;
    uint32_t probe_drop_low_value;
    uint32_t probe_drop_rate_limit;
    uint32_t probe_drop_pressure;

    /* Time-sync diagnostic (v0.60+): scanner's epoch-ms offset against its
     * monotonic clock. Stays at 0 until the scanner has received a usable
     * time broadcast from the uplink. */
    int64_t  toff_ms;
    /* Counter — increments on every time message the scanner sees, even
     * if the value was bad. tcnt > 0 with toff_ms == 0 means UART is fine
     * but the uplink is sending bogus epochs. Both 0 = UART path broken. */
    uint32_t tcnt;
    uint32_t time_valid_count;
    int64_t  time_last_valid_age_s;
    char     time_sync_state[12];
    char     scan_mode[16];
    char     calibration_uuid[48];
    bool     calibration_mode_acked;
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

void uart_rx_set_node_calibration_mode(bool active,
                                       const char *session_id,
                                       const char *calibration_uuid);
bool uart_rx_is_node_calibration_mode(void);
const char *uart_rx_get_node_scan_mode(void);
const char *uart_rx_get_node_calibration_uuid(void);
const char *uart_rx_get_node_calibration_session_id(void);
bool uart_rx_node_mode_allows_detection(const drone_detection_t *det);

#ifdef __cplusplus
}
#endif
