#pragma once

/**
 * Friend or Foe -- Uplink UART RX Module
 *
 * Receives newline-delimited JSON messages from scanner boards over the two
 * ESP32-S3 uplink UART slots.
 */

#include "detection_types.h"
#include "badge_threat_policy.h"
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
    char    manufacturer[32];
    char    badge_label[BADGE_THREAT_LABEL_LEN];
    char    badge_entity_key[BADGE_THREAT_KEY_LEN];
    char    badge_class_name[16];
    uint8_t source;
    float   confidence;
    float   threat_score;
    int8_t  rssi;
    int64_t timestamp_ms;
} detection_summary_t;

/** Copy most recent detections into caller's buffer (newest first). */
int uart_rx_get_recent_detections(detection_summary_t *out, int max);

/** Snapshot badge-local active threat state for LCD/API surfaces. */
void uart_rx_get_badge_threat_snapshot(badge_threat_snapshot_t *out);

/** True if ANY scanner is connected (sent data within 5s). */
bool uart_rx_is_scanner_connected(void);

/** True if the BLE scanner (UART1) is connected. */
bool uart_rx_is_ble_scanner_connected(void);

/** True if the WiFi scanner (UART2) is connected. */
bool uart_rx_is_wifi_scanner_connected(void);

/** Raw UART diagnostics, including bytes that did not parse into JSON. */
typedef struct {
    bool raw_seen;
    int64_t raw_age_s;
    uint32_t raw_bytes;
    uint32_t line_overflow_count;
    uint32_t json_parse_error_count;
} scanner_uart_diag_t;

void uart_rx_get_scanner_uart_diag(int scanner_id, scanner_uart_diag_t *out);

/**
 * Send a JSON command to all connected scanners via UART TX.
 * Used for lock-on commands from the backend.
 */
void uart_rx_send_command(const char *json_cmd);
void uart_rx_send_command_to_scanner(int scanner_id, const char *json_cmd);
bool uart_rx_send_command_to_scanner_checked(int scanner_id, const char *json_cmd);
bool uart_rx_set_scanner_tx_pin_for_badge_probe(int scanner_id, int tx_pin);

/** Scanner identity info (received via UART scanner_info message). */
typedef struct {
    char version[32];
    char board[40];     /* firmware catalog name, e.g. "scanner-s3-combo-fof_badge" */
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
    bool     ble_scanning;
    bool     ble_host_active;
    bool     ble_host_synced;
    bool     wifi_paused;
    uint32_t wifi_total_frames;
    uint32_t wifi_beacon_frames;
    uint32_t wifi_full_scan_count;
    uint32_t wifi_full_scan_ok;
    uint32_t wifi_full_scan_err;
    int      wifi_full_scan_last_rc;
    uint32_t wifi_last_ap_count;
    int64_t  wifi_last_scan_age_s;
    uint32_t wifi_drone_ssid_emit;
    uint32_t wifi_notable_ssid_emit;
    char     wifi_last_drone_ssid[33];
    char     wifi_last_notable_ssid[33];
    int64_t  wifi_last_drone_ssid_seen_ms;
    int64_t  wifi_last_notable_ssid_seen_ms;
    int64_t  wifi_last_drone_ssid_age_s;
    int64_t  wifi_last_notable_ssid_age_s;
    uint32_t wifi_oui_emit;
    uint32_t wifi_soft_ssid_emit;
    uint32_t wifi_hot_ch;
    uint32_t ble_adv_seen;
    uint32_t ble_any_seen;
    uint32_t ble_any_with_payload_seen;
    uint32_t ble_any_empty_seen;
    int8_t   ble_any_last_rssi;
    int8_t   ble_any_best_rssi;
    uint8_t  ble_any_last_len;
    uint8_t  ble_any_last_props;
    uint8_t  ble_any_last_addr_type;
    uint32_t ble_fp_emit;
    uint32_t ble_meta_seen;
    int64_t  ble_meta_last_seen_age_s;
    int64_t  ble_meta_last_emit_age_s;
    uint32_t ble_meta_last_hash;
    int8_t   ble_meta_last_rssi;
    char     ble_meta_last_reason[32];
    char     ble_meta_identity[16];
    int64_t  ble_meta_weak_age_s;
    uint32_t ble_meta_reacquire_count;
    uint32_t ble_tracker_seen;
    uint32_t ble_privacy_candidate_seen;
    uint32_t ble_near_unknown_seen;
    uint32_t ble_drop_profile;
    uint32_t ble_drop_rate;
    uint32_t ble_dbg_near_seen;
    int8_t   ble_dbg_near_rssi;
    char     ble_dbg_near_label[24];
    char     ble_dbg_near_name[32];
    char     ble_dbg_near_reason[32];
    uint16_t ble_dbg_near_cid;
    uint16_t ble_dbg_near_svc0;
    uint8_t  ble_dbg_near_svc_count;
    uint8_t  ble_dbg_near_payload_len;
    uint32_t ble_dbg_priv_seen;
    int8_t   ble_dbg_priv_rssi;
    char     ble_dbg_priv_label[24];
    char     ble_dbg_priv_name[32];
    char     ble_dbg_priv_reason[32];
    uint16_t ble_dbg_priv_cid;
    uint16_t ble_dbg_priv_svc0;
    uint8_t  ble_dbg_priv_svc_count;
    uint8_t  ble_dbg_priv_payload_len;
    uint32_t ble_host_restart_count;
    uint32_t ble_scan_start_count;
    uint32_t ble_scan_start_ok;
    int      ble_scan_last_rc;
    int      ble_sync_last_rc;
    bool     ble_focus_active;
    int64_t  ble_focus_age_s;
    uint32_t ble_focus_target_adv_count;
    uint32_t rid_service_seen;
    uint32_t rid_emit;
    uint32_t privacy_seen;

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
    uint32_t cmd_rx_count;
    uint32_t cmd_parse_error_count;
    uint32_t cmd_overflow_count;
    uint32_t cmd_stale_count;
    int64_t  cmd_last_age_s;
    char     scan_mode[16];
    char     scan_profile[24];
    char     calibration_uuid[48];
    bool     calibration_mode_acked;
    bool     need_firmware;
    char     fw_target_version[32];
    char     fw_update_state[16];
    uint32_t fw_check_count;
    int64_t  fw_backoff_s;
    char     last_fw_error[48];
    char     ota_state[16];
    char     ota_session_id[24];
    uint32_t ota_received;
    uint32_t ota_total;
    char     recovery_mode[16];
    char     safe_reason[48];
    bool     rollback_pending;
    uint32_t crash_count;
    uint32_t radio_restart_count;
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
