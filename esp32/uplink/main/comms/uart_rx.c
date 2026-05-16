/**
 * Friend or Foe -- Uplink UART RX Implementation
 *
 * Receives newline-delimited JSON from the Scanner board over UART1,
 * parses detection and status messages, and enqueues drone_detection_t
 * structs for the HTTP upload task.
 */

#include "uart_rx.h"
#include "uart_protocol.h"
#include "config.h"
#include "detection_policy.h"
#include "badge_threat_policy.h"
#include "led_status.h"
#include "fw_store.h"
#ifdef FOF_BADGE_VARIANT
#include "badge_runtime.h"
#endif

#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

static const char *TAG = "uart_rx";

#define LINE_BUF_SIZE       4096
#define READ_BUF_SIZE       256

/* Scanner connection tracking */
#define SCANNER_TIMEOUT_MS  15000
static atomic_int_fast64_t s_last_rx_time_ble = 0;
static atomic_int_fast64_t s_last_rx_time_wifi = 0;
static atomic_int_fast64_t s_last_raw_rx_time_ble = 0;
static atomic_int_fast64_t s_last_raw_rx_time_wifi = 0;
static atomic_uint_fast32_t s_raw_rx_bytes_ble = 0;
static atomic_uint_fast32_t s_raw_rx_bytes_wifi = 0;
static atomic_uint_fast32_t s_line_overflow_ble = 0;
static atomic_uint_fast32_t s_line_overflow_wifi = 0;
static atomic_uint_fast32_t s_json_parse_error_ble = 0;
static atomic_uint_fast32_t s_json_parse_error_wifi = 0;
static bool s_first_status_received = false;

static QueueHandle_t s_detection_queue = NULL;
static int           s_detection_count = 0;
static SemaphoreHandle_t s_uart_tx_lock = NULL;

/* Scanner identity (populated from scanner_info UART messages) */
static scanner_info_t s_ble_scanner_info = {0};
static scanner_info_t s_wifi_scanner_info = {0};

/* OTA response tracking (set by UART RX, read by relay handler) */
static volatile ota_response_t s_last_ota_response = {0};
static portMUX_TYPE s_ota_response_lock = portMUX_INITIALIZER_UNLOCKED;

/* Pause flags for OTA relay — lets relay handler read UART directly */
static volatile bool s_rx_paused_ble = false;
static volatile bool s_rx_paused_wifi = false;

/* Per-scanner backpressure flags — BLE noise should not be able to pause the
 * WiFi scanner, and each scanner must be able to resume independently. */
static _Atomic bool s_backpressure_ble = false;
static _Atomic bool s_backpressure_wifi = false;

static bool s_node_calibration_mode = false;
static char s_node_scan_mode[16] = "normal";
static char s_node_calibration_session_id[24] = {0};
static char s_node_calibration_uuid[48] = {0};

/* ── Recent detections ring buffer ─────────────────────────────────────── */

#define RECENT_RING_SIZE  8

static detection_summary_t s_recent_ring[RECENT_RING_SIZE];
static int                 s_recent_head = 0;   /* next write index */
static int                 s_recent_count = 0;
static portMUX_TYPE        s_recent_lock = portMUX_INITIALIZER_UNLOCKED;

#ifdef FOF_BADGE_VARIANT
static badge_threat_state_t s_badge_threat_state;
static bool                 s_badge_threat_state_ready = false;
static SemaphoreHandle_t    s_badge_threat_mutex = NULL;
static portMUX_TYPE         s_badge_threat_init_lock = portMUX_INITIALIZER_UNLOCKED;

static bool badge_threat_lock_state(TickType_t wait_ticks)
{
    if (!s_badge_threat_mutex) {
        SemaphoreHandle_t created = xSemaphoreCreateMutex();
        if (!created) {
            ESP_LOGE(TAG, "Failed to create badge threat mutex");
            return false;
        }
        portENTER_CRITICAL(&s_badge_threat_init_lock);
        if (!s_badge_threat_mutex) {
            s_badge_threat_mutex = created;
            created = NULL;
        }
        portEXIT_CRITICAL(&s_badge_threat_init_lock);
        if (created) {
            vSemaphoreDelete(created);
        }
    }
    return xSemaphoreTake(s_badge_threat_mutex, wait_ticks) == pdTRUE;
}

static void badge_threat_unlock_state(void)
{
    if (s_badge_threat_mutex) {
        xSemaphoreGive(s_badge_threat_mutex);
    }
}

static void badge_threat_ensure_ready(void)
{
    if (!s_badge_threat_state_ready) {
        badge_threat_state_init(&s_badge_threat_state);
        s_badge_threat_state_ready = true;
    }
}

static bool badge_ingest_detection(const drone_detection_t *det,
                                   badge_threat_event_t *event_out)
{
    if (!det) {
        return false;
    }
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (!badge_threat_lock_state(pdMS_TO_TICKS(50))) {
        return false;
    }
    badge_threat_ensure_ready();
    bool visible = badge_threat_state_ingest(
        &s_badge_threat_state,
        det,
        now_ms,
        event_out
    );
    badge_threat_unlock_state();
    return visible;
}

static void badge_ingest_wifi_status_event(const char *label,
                                           const char *reason,
                                           float confidence,
                                           uint16_t count)
{
    if (!label || count == 0) {
        return;
    }

    drone_detection_t det = {0};
    det.source = DETECTION_SRC_WIFI_ASSOC;
    det.confidence = confidence;
    det.rssi = 0;
    snprintf(det.drone_id, sizeof(det.drone_id), "wifi:%s", reason ? reason : label);
    if (count > 1) {
        snprintf(det.manufacturer, sizeof(det.manufacturer), "%s x%u",
                 label, (unsigned)count);
    } else {
        strncpy(det.manufacturer, label, sizeof(det.manufacturer) - 1);
    }
    snprintf(det.class_reason, sizeof(det.class_reason), "%s count:%u",
             reason ? reason : label, (unsigned)count);
    (void)badge_ingest_detection(&det, NULL);
}

typedef struct {
    uint32_t ble_meta_seen;
    uint32_t ble_tracker_seen;
    uint32_t ble_near_unknown_seen;
    uint32_t wifi_drone_ssid_emit;
    uint32_t wifi_notable_ssid_emit;
} badge_status_seen_t;

static badge_status_seen_t s_badge_status_seen[2] = {0};

static bool badge_status_counter_advanced(uint32_t current, uint32_t previous)
{
    return current > previous;
}

static bool scanner_status_ssid_is_fresh(const char *ssid, int64_t age_s)
{
    return badge_threat_status_ssid_is_fresh(ssid, age_s);
}

static bool scanner_wifi_drone_ssid_is_fresh(const scanner_info_t *info)
{
    return scanner_status_ssid_is_fresh(
        info ? info->wifi_last_drone_ssid : NULL,
        info ? info->wifi_last_drone_ssid_age_s : -1
    );
}

static bool scanner_wifi_notable_ssid_is_fresh(const scanner_info_t *info)
{
    return scanner_status_ssid_is_fresh(
        info ? info->wifi_last_notable_ssid : NULL,
        info ? info->wifi_last_notable_ssid_age_s : -1
    );
}

static bool scanner_meta_status_is_fresh(const scanner_info_t *info)
{
    return info &&
           info->ble_meta_seen > 0 &&
           info->ble_meta_last_seen_age_s >= 0 &&
           info->ble_meta_last_seen_age_s <= 90;
}

static bool scanner_meta_status_has_strong_identity(const scanner_info_t *info)
{
    if (!info || !scanner_meta_status_is_fresh(info)) {
        return false;
    }
    if (info->ble_meta_last_hash == 0) {
        return false;
    }
    if (strstr(info->ble_meta_last_reason, "weak_meta") != NULL) {
        return false;
    }
    return strcmp(info->ble_meta_identity, "strong_fp") == 0 ||
           strcmp(info->ble_meta_identity, "detector_fp") == 0;
}

static int8_t badge_status_rssi_or(int8_t rssi, int8_t fallback)
{
    return rssi < 0 ? rssi : fallback;
}

static void badge_ingest_ble_tracker_status_event(int scanner_id,
                                                  const scanner_info_t *info)
{
    int8_t rssi = badge_status_rssi_or(info ? info->ble_dbg_priv_rssi : 0, -64);
    if (rssi < -50) {
        return;
    }

    drone_detection_t det = {0};
    det.source = DETECTION_SRC_BLE_FINGERPRINT;
    det.confidence = 0.62f;
    det.rssi = rssi;
    snprintf(det.drone_id, sizeof(det.drone_id), "status:ble:tracker:%d", scanner_id);
    strncpy(det.manufacturer,
            (info && info->ble_dbg_priv_label[0]) ? info->ble_dbg_priv_label : "Tracker",
            sizeof(det.manufacturer) - 1);
    if (info) {
        strncpy(det.ble_name, info->ble_dbg_priv_name, sizeof(det.ble_name) - 1);
        strncpy(det.class_reason,
                info->ble_dbg_priv_reason[0] ? info->ble_dbg_priv_reason : "status:tracker",
                sizeof(det.class_reason) - 1);
    }
    (void)badge_ingest_detection(&det, NULL);
}

static void badge_ingest_ble_near_status_event(int scanner_id,
                                               const scanner_info_t *info)
{
    drone_detection_t det = {0};
    det.source = DETECTION_SRC_BLE_FINGERPRINT;
    det.confidence = 0.18f;
    det.rssi = badge_status_rssi_or(info ? info->ble_dbg_near_rssi : 0, -48);
    snprintf(det.drone_id, sizeof(det.drone_id), "status:ble:near:%d", scanner_id);
    strncpy(det.manufacturer, "BLE Nearby", sizeof(det.manufacturer) - 1);
    if (info) {
        strncpy(det.ble_name, info->ble_dbg_near_name, sizeof(det.ble_name) - 1);
        strncpy(det.class_reason,
                info->ble_dbg_near_reason[0] ? info->ble_dbg_near_reason : "strong BLE near",
                sizeof(det.class_reason) - 1);
    }
    (void)badge_ingest_detection(&det, NULL);
}

static void badge_ingest_ble_meta_status_event(const scanner_info_t *info)
{
    if (!scanner_meta_status_has_strong_identity(info)) {
        return;
    }

    drone_detection_t det = {0};
    det.source = DETECTION_SRC_BLE_FINGERPRINT;
    det.confidence = 0.72f;
    det.rssi = badge_status_rssi_or(info->ble_meta_last_rssi, -58);
    snprintf(det.drone_id, sizeof(det.drone_id), "BLE:%08lX:Meta Glasses",
             (unsigned long)info->ble_meta_last_hash);
    strncpy(det.manufacturer, "Meta Glasses", sizeof(det.manufacturer) - 1);
    snprintf(det.model, sizeof(det.model), "FP:%08lX",
             (unsigned long)info->ble_meta_last_hash);
    strncpy(det.class_reason,
            info->ble_meta_last_reason[0]
                ? info->ble_meta_last_reason
                : "scanner_status:meta_fp",
            sizeof(det.class_reason) - 1);
    (void)badge_ingest_detection(&det, NULL);
}

static void badge_ingest_wifi_drone_status_event(int scanner_id,
                                                 const scanner_info_t *info)
{
    const char *ssid = scanner_wifi_drone_ssid_is_fresh(info)
        ? info->wifi_last_drone_ssid
        : NULL;
    if (!ssid) {
        return;
    }
    drone_detection_t det = {0};
    det.source = DETECTION_SRC_WIFI_SSID;
    det.confidence = 0.35f;
    det.rssi = 0;
    snprintf(det.drone_id, sizeof(det.drone_id), "status:wifi:ssid:%d", scanner_id);
    strncpy(det.manufacturer, "Drone SSID", sizeof(det.manufacturer) - 1);
    strncpy(det.ssid, ssid, sizeof(det.ssid) - 1);
    (void)badge_ingest_detection(&det, NULL);
}

static void badge_ingest_wifi_notable_status_event(int scanner_id,
                                                   const scanner_info_t *info)
{
    const char *ssid = scanner_wifi_notable_ssid_is_fresh(info)
        ? info->wifi_last_notable_ssid
        : "Notable SSID";
    const char *label = fof_policy_ssid_is_notable(ssid)
        ? fof_policy_notable_ssid_label(ssid)
        : "ssid anomaly";
    drone_detection_t det = {0};
    det.source = DETECTION_SRC_WIFI_ASSOC;
    det.confidence = 0.64f;
    det.rssi = 0;
    snprintf(det.drone_id, sizeof(det.drone_id), "status:wifi:notable:%d", scanner_id);
    strncpy(det.manufacturer, label, sizeof(det.manufacturer) - 1);
    strncpy(det.class_reason, label, sizeof(det.class_reason) - 1);
    strncpy(det.ssid, ssid, sizeof(det.ssid) - 1);
    (void)badge_ingest_detection(&det, NULL);
}

static void badge_ingest_scanner_status_evidence(int scanner_id,
                                                 const scanner_info_t *info)
{
    if (!info || scanner_id < 0 ||
        scanner_id >= (int)(sizeof(s_badge_status_seen) / sizeof(s_badge_status_seen[0]))) {
        return;
    }

    badge_status_seen_t *seen = &s_badge_status_seen[scanner_id];
    /* Scanner Meta status is allowed to refresh exactly one weak presence
     * bucket. Strong fingerprint identities still drive real multi-pair
     * counts, and the weak bucket expires as soon as scanner Meta freshness
     * goes quiet.
     */
    badge_ingest_ble_meta_status_event(info);
    if (badge_status_counter_advanced(info->ble_tracker_seen, seen->ble_tracker_seen)) {
        badge_ingest_ble_tracker_status_event(scanner_id, info);
    }
    if (badge_status_counter_advanced(info->ble_near_unknown_seen, seen->ble_near_unknown_seen)) {
        badge_ingest_ble_near_status_event(scanner_id, info);
    }
    if (badge_status_counter_advanced(info->wifi_drone_ssid_emit,
                                      seen->wifi_drone_ssid_emit)) {
        badge_ingest_wifi_drone_status_event(scanner_id, info);
    }
    if (badge_status_counter_advanced(info->wifi_notable_ssid_emit,
                                      seen->wifi_notable_ssid_emit)) {
        badge_ingest_wifi_notable_status_event(scanner_id, info);
    }

    seen->ble_meta_seen = info->ble_meta_seen;
    seen->ble_tracker_seen = info->ble_tracker_seen;
    seen->ble_near_unknown_seen = info->ble_near_unknown_seen;
    seen->wifi_drone_ssid_emit = info->wifi_drone_ssid_emit;
    seen->wifi_notable_ssid_emit = info->wifi_notable_ssid_emit;
}
#endif

static void push_recent(const drone_detection_t *det)
{
    badge_threat_event_t badge_event = {0};
    bool badge_visible = false;
#ifdef FOF_BADGE_VARIANT
    badge_visible = badge_threat_classify_detection(det, &badge_event);
#endif

    portENTER_CRITICAL(&s_recent_lock);
    detection_summary_t *slot = &s_recent_ring[s_recent_head];
    strncpy(slot->drone_id, det->drone_id, sizeof(slot->drone_id) - 1);
    slot->drone_id[sizeof(slot->drone_id) - 1] = '\0';
    strncpy(slot->manufacturer, det->manufacturer, sizeof(slot->manufacturer) - 1);
    slot->manufacturer[sizeof(slot->manufacturer) - 1] = '\0';
    if (badge_visible) {
        strncpy(slot->badge_label, badge_event.label, sizeof(slot->badge_label) - 1);
        strncpy(slot->badge_entity_key, badge_event.key, sizeof(slot->badge_entity_key) - 1);
        strncpy(slot->badge_class_name,
                badge_threat_class_name(badge_event.cls),
                sizeof(slot->badge_class_name) - 1);
        slot->threat_score = badge_event.base_score;
    } else {
        slot->badge_label[0] = '\0';
        slot->badge_entity_key[0] = '\0';
        slot->badge_class_name[0] = '\0';
        slot->threat_score = 0.0f;
    }
    slot->source       = det->source;
    slot->confidence   = det->confidence;
    slot->rssi         = det->rssi;
    slot->timestamp_ms = esp_timer_get_time() / 1000;
    s_recent_head = (s_recent_head + 1) % RECENT_RING_SIZE;
    if (s_recent_count < RECENT_RING_SIZE) {
        s_recent_count++;
    }
    portEXIT_CRITICAL(&s_recent_lock);
}

void uart_rx_get_badge_threat_snapshot(badge_threat_snapshot_t *out)
{
    if (!out) {
        return;
    }
#ifdef FOF_BADGE_VARIANT
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (!badge_threat_lock_state(pdMS_TO_TICKS(50))) {
        memset(out, 0, sizeof(*out));
        out->color_rgb565 = badge_threat_score_to_rgb565(0.0f);
        strncpy(out->top_label, "Watching", sizeof(out->top_label) - 1);
        strncpy(out->ticker, "Watching", sizeof(out->ticker) - 1);
        return;
    }
    badge_threat_ensure_ready();
    badge_threat_state_snapshot(&s_badge_threat_state, now_ms, out);
    badge_threat_unlock_state();
#else
    memset(out, 0, sizeof(*out));
    out->color_rgb565 = badge_threat_score_to_rgb565(0.0f);
    strncpy(out->top_label, "Clear", sizeof(out->top_label) - 1);
    strncpy(out->ticker, "Clear", sizeof(out->ticker) - 1);
#endif
}

static _Atomic bool *backpressure_flag_for_scanner(int scanner_id)
{
    return (scanner_id == 0) ? &s_backpressure_ble : &s_backpressure_wifi;
}

static uart_port_t scanner_uart_for_id(int scanner_id)
{
#if CONFIG_DUAL_SCANNER
    return (scanner_id == 0) ? CONFIG_BLE_SCANNER_UART : CONFIG_WIFI_SCANNER_UART;
#else
    (void)scanner_id;
    return CONFIG_BLE_SCANNER_UART;
#endif
}

bool uart_rx_set_scanner_tx_pin_for_badge_probe(int scanner_id, int tx_pin)
{
#ifdef FOF_BADGE_VARIANT
    uart_port_t uart = scanner_uart_for_id(scanner_id);
    int rx_pin = scanner_id == 1 ? CONFIG_WIFI_SCANNER_RX_PIN : CONFIG_BLE_SCANNER_RX_PIN;

    SemaphoreHandle_t lock = s_uart_tx_lock;
    if (lock && xSemaphoreTake(lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "badge tx-pin probe lock timeout: scanner=%d tx=%d",
                 scanner_id, tx_pin);
        return false;
    }

    uart_wait_tx_done(uart, pdMS_TO_TICKS(150));
    esp_err_t err = uart_set_pin(uart, tx_pin, rx_pin,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    if (lock) {
        xSemaphoreGive(lock);
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "badge tx-pin probe failed: scanner=%d uart=%d tx=%d rx=%d err=%s",
                 scanner_id, uart, tx_pin, rx_pin, esp_err_to_name(err));
        return false;
    }

    ESP_LOGW(TAG, "badge tx-pin probe set scanner[%d] UART%d TX=GPIO%d RX=GPIO%d",
             scanner_id, uart, tx_pin, rx_pin);
    return true;
#else
    (void)scanner_id;
    (void)tx_pin;
    return false;
#endif
}

static bool send_json_line_to_scanner_locked(int scanner_id, const char *json_cmd)
{
    if (!json_cmd) {
        return false;
    }

    char line[BADGE_DISPLAY_POLICY_JSON_MAX + 160];
    size_t len = strlen(json_cmd);
    while (len > 0 && (json_cmd[len - 1] == '\n' || json_cmd[len - 1] == '\r')) {
        len--;
    }
    if (len == 0 || len >= sizeof(line) - 1) {
        ESP_LOGW(TAG, "scanner command rejected: len=%u scanner=%d",
                 (unsigned)len, scanner_id);
        return false;
    }

    memcpy(line, json_cmd, len);
    line[len++] = '\n';

    SemaphoreHandle_t lock = s_uart_tx_lock;
    if (lock && xSemaphoreTake(lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "scanner command lock timeout: scanner=%d", scanner_id);
        return false;
    }

    uart_port_t uart = scanner_uart_for_id(scanner_id);
    int written = uart_write_bytes(uart, line, len);
    if (written == (int)len) {
        uart_wait_tx_done(uart, pdMS_TO_TICKS(150));
    }

    if (lock) {
        xSemaphoreGive(lock);
    }

    if (written != (int)len) {
        ESP_LOGW(TAG, "scanner command short write: scanner=%d uart=%d wrote=%d/%u",
                 scanner_id, uart, written, (unsigned)len);
        return false;
    }
    return true;
}

static void send_scanner_flow_cmd(int scanner_id, const char *type)
{
    char cmd[24];
    int n = snprintf(cmd, sizeof(cmd), "{\"type\":\"%s\"}", type);
    if (n > 0) {
        send_json_line_to_scanner_locked(scanner_id, cmd);
    }
}

static bool is_low_value_ble_detection(const drone_detection_t *det)
{
    if (det->source == DETECTION_SRC_BLE_FINGERPRINT) {
        return det->confidence < 0.10f;
    }
    return det->source == DETECTION_SRC_BLE_RID &&
           det->confidence < 0.10f &&
           strncmp(det->drone_id, "rid_", 4) != 0 &&
           det->latitude == 0.0 &&
           det->longitude == 0.0 &&
           det->operator_lat == 0.0 &&
           det->operator_lon == 0.0;
}

static void maybe_resume_scanner(int scanner_id)
{
    _Atomic bool *flag = backpressure_flag_for_scanner(scanner_id);
    if (!atomic_load(flag) || s_detection_queue == NULL) {
        return;
    }

    UBaseType_t queue_count = uxQueueMessagesWaiting(s_detection_queue);
    if (queue_count <= (CONFIG_DETECTION_QUEUE_SIZE * 4 / 10)) {
        send_scanner_flow_cmd(scanner_id, "start");
        ESP_LOGI(TAG, "Queue drained %d/%d — resuming scanner[%d]",
                 (int)queue_count, CONFIG_DETECTION_QUEUE_SIZE, scanner_id);
        atomic_store(flag, false);
    }
}

static void note_scanner_activity(int scanner_id, int_fast64_t now_ms)
{
    if (scanner_id == 0) {
        atomic_store(&s_last_rx_time_ble, now_ms);
    } else {
        atomic_store(&s_last_rx_time_wifi, now_ms);
    }
}

static void note_scanner_raw_activity(int scanner_id, int_fast64_t now_ms,
                                      int bytes_read)
{
    if (scanner_id == 0) {
        atomic_store(&s_last_raw_rx_time_ble, now_ms);
        atomic_fetch_add(&s_raw_rx_bytes_ble, (uint32_t)bytes_read);
    } else {
        atomic_store(&s_last_raw_rx_time_wifi, now_ms);
        atomic_fetch_add(&s_raw_rx_bytes_wifi, (uint32_t)bytes_read);
    }
}

static void note_scanner_line_overflow(int scanner_id)
{
    if (scanner_id == 0) {
        atomic_fetch_add(&s_line_overflow_ble, 1);
    } else {
        atomic_fetch_add(&s_line_overflow_wifi, 1);
    }
}

static void note_scanner_json_parse_error(int scanner_id)
{
    if (scanner_id == 0) {
        atomic_fetch_add(&s_json_parse_error_ble, 1);
    } else {
        atomic_fetch_add(&s_json_parse_error_wifi, 1);
    }
}

static bool msg_type_is_scanner_originated(const char *msg_type)
{
    if (!msg_type) {
        return false;
    }
    return strcmp(msg_type, MSG_TYPE_DETECTION) == 0 ||
           strcmp(msg_type, MSG_TYPE_STATUS) == 0 ||
           strcmp(msg_type, "scanner_info") == 0 ||
           strcmp(msg_type, MSG_TYPE_CAL_MODE_ACK) == 0 ||
           strcmp(msg_type, "scan_profile_ack") == 0 ||
           strcmp(msg_type, "display_control_ack") == 0 ||
           strcmp(msg_type, "recovery_ack") == 0 ||
           strcmp(msg_type, "scanner_recovery") == 0 ||
           strcmp(msg_type, MSG_TYPE_FW_CHECK) == 0 ||
           strcmp(msg_type, MSG_TYPE_FW_READY) == 0 ||
           strncmp(msg_type, "ota_", 4) == 0;
}

void uart_rx_set_node_calibration_mode(bool active,
                                       const char *session_id,
                                       const char *calibration_uuid)
{
    s_node_calibration_mode = active;
    strncpy(s_node_scan_mode, active ? "calibration" : "normal", sizeof(s_node_scan_mode) - 1);
    s_node_scan_mode[sizeof(s_node_scan_mode) - 1] = '\0';

    strncpy(s_node_calibration_session_id, session_id ? session_id : "", sizeof(s_node_calibration_session_id) - 1);
    s_node_calibration_session_id[sizeof(s_node_calibration_session_id) - 1] = '\0';
    strncpy(s_node_calibration_uuid, calibration_uuid ? calibration_uuid : "", sizeof(s_node_calibration_uuid) - 1);
    s_node_calibration_uuid[sizeof(s_node_calibration_uuid) - 1] = '\0';

    s_ble_scanner_info.calibration_mode_acked = false;
    s_wifi_scanner_info.calibration_mode_acked = false;
}

bool uart_rx_is_node_calibration_mode(void)
{
    return s_node_calibration_mode;
}

const char *uart_rx_get_node_scan_mode(void)
{
    return s_node_scan_mode;
}

const char *uart_rx_get_node_calibration_uuid(void)
{
    return s_node_calibration_uuid;
}

const char *uart_rx_get_node_calibration_session_id(void)
{
    return s_node_calibration_session_id;
}

bool uart_rx_node_mode_allows_detection(const drone_detection_t *det)
{
    if (!s_node_calibration_mode) {
        return true;
    }
    if (!det || det->source != DETECTION_SRC_BLE_FINGERPRINT) {
        return false;
    }
    if (fof_policy_ble_svc_raw_contains_uuid(det->ble_svc_uuids_raw, s_node_calibration_uuid)) {
        return true;
    }
    return fof_policy_ble_has_exact_uuid128_le(
        det->ble_service_uuids_128,
        det->ble_svc_uuid_128_count,
        s_node_calibration_uuid
    );
}

/* ── Source string to DETECTION_SRC mapping ────────────────────────────── */

static bool parse_source_value(int src_int, uint8_t *out_source)
{
    if (!out_source) {
        return false;
    }

    switch (src_int) {
        case DETECTION_SRC_BLE_RID:
        case DETECTION_SRC_BLE_FINGERPRINT:
        case DETECTION_SRC_WIFI_SSID:
        case DETECTION_SRC_WIFI_DJI_IE:
        case DETECTION_SRC_WIFI_BEACON:
        case DETECTION_SRC_WIFI_OUI:
        case DETECTION_SRC_WIFI_PROBE_REQUEST:
        case DETECTION_SRC_WIFI_ASSOC:
        case DETECTION_SRC_WIFI_AP_INVENTORY:
            *out_source = (uint8_t)src_int;
            return true;
        default:
            return false;
    }
}

/* ── JSON helpers ──────────────────────────────────────────────────────── */

static double json_get_double(const cJSON *obj, const char *key, double def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        return item->valuedouble;
    }
    return def;
}

static int json_get_int(const cJSON *obj, const char *key, int def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return def;
}

static const char *json_get_string(const cJSON *obj, const char *key,
                                   const char *def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }
    return def;
}

static void json_copy_string(const cJSON *obj, const char *key,
                             char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    const char *value = json_get_string(obj, key, out);
    if (!value) {
        return;
    }
    strncpy(out, value, out_len - 1);
    out[out_len - 1] = '\0';
}

static int64_t scanner_status_ssid_age_s(int64_t now_ms, int64_t seen_ms)
{
    if (seen_ms <= 0 || now_ms < seen_ms) {
        return -1;
    }
    return (now_ms - seen_ms) / 1000;
}

static void json_update_scanner_ssid_freshness(const cJSON *obj,
                                               const char *key,
                                               char *out,
                                               size_t out_len,
                                               uint32_t previous_emit,
                                               uint32_t current_emit,
                                               int64_t now_ms,
                                               int64_t *seen_ms,
                                               int64_t *age_s)
{
    if (!out || out_len == 0 || !seen_ms || !age_s) {
        return;
    }

    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!item) {
        *age_s = scanner_status_ssid_age_s(now_ms, *seen_ms);
        return;
    }

    const char *value = (cJSON_IsString(item) && item->valuestring)
        ? item->valuestring
        : "";
    if (value[0] == '\0') {
        out[0] = '\0';
        *seen_ms = 0;
        *age_s = -1;
        return;
    }

    bool changed = strcmp(out, value) != 0;
    strncpy(out, value, out_len - 1);
    out[out_len - 1] = '\0';
    if (changed || current_emit > previous_emit || *seen_ms <= 0) {
        *seen_ms = now_ms;
    }
    *age_s = scanner_status_ssid_age_s(now_ms, *seen_ms);
}

static void json_get_string_or_array_csv(const cJSON *obj,
                                         const char *key,
                                         char *out,
                                         size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(out, item->valuestring, out_len - 1);
        out[out_len - 1] = '\0';
        return;
    }
    if (!cJSON_IsArray(item)) {
        return;
    }

    size_t off = 0;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, item) {
        if (!cJSON_IsString(entry) || !entry->valuestring ||
            entry->valuestring[0] == '\0') {
            continue;
        }
        if (off > 0 && off < out_len - 1) {
            out[off++] = ',';
        }
        for (const char *p = entry->valuestring; *p && off < out_len - 1; ++p) {
            out[off++] = *p;
        }
        if (off >= out_len - 1) {
            break;
        }
    }
    out[off] = '\0';
}

/* ── Parse a detection message ─────────────────────────────────────────── */

static bool parse_detection(const cJSON *root, drone_detection_t *det)
{
    memset(det, 0, sizeof(*det));

    /* Required field: drone_id */
    const char *drone_id = json_get_string(root, JSON_KEY_DRONE_ID, NULL);
    if (!drone_id) {
        ESP_LOGW(TAG, "Detection missing drone_id");
        return false;
    }
    strncpy(det->drone_id, drone_id, sizeof(det->drone_id) - 1);

    /* Source — fail closed. Missing/unknown source codes must not silently
     * downgrade into the BLE RID lane because that makes garbage look
     * authoritative to the backend and operator UI. */
    const cJSON *src_item = cJSON_GetObjectItemCaseSensitive(root, JSON_KEY_SOURCE);
    if (!cJSON_IsNumber(src_item)) {
        ESP_LOGW(TAG, "Detection %s missing numeric src", det->drone_id);
        return false;
    }
    if (!parse_source_value(src_item->valueint, &det->source)) {
        ESP_LOGW(TAG, "Detection %s has unknown src=%d", det->drone_id, src_item->valueint);
        return false;
    }

    /* Confidence */
    det->confidence = (float)json_get_double(root, JSON_KEY_CONFIDENCE, 0.0);

    /* Position */
    det->latitude    = json_get_double(root, JSON_KEY_LATITUDE, 0.0);
    det->longitude   = json_get_double(root, JSON_KEY_LONGITUDE, 0.0);
    det->altitude_m  = json_get_double(root, JSON_KEY_ALTITUDE, 0.0);

    /* RF */
    det->rssi = (int8_t)json_get_int(root, JSON_KEY_RSSI, 0);

    /* Kinematics */
    det->speed_mps   = (float)json_get_double(root, JSON_KEY_SPEED, 0.0);
    det->heading_deg  = (float)json_get_double(root, JSON_KEY_HEADING, 0.0);
    det->vertical_speed_mps = (float)json_get_double(root, JSON_KEY_VSPEED, 0.0);

    /* Distance estimate */
    det->estimated_distance_m = json_get_double(root, JSON_KEY_DISTANCE, 0.0);

    /* Metadata */
    const char *mfr = json_get_string(root, JSON_KEY_MANUFACTURER, "");
    strncpy(det->manufacturer, mfr, sizeof(det->manufacturer) - 1);

    const char *model = json_get_string(root, JSON_KEY_MODEL, "");
    strncpy(det->model, model, sizeof(det->model) - 1);

    /* Operator info */
    det->operator_lat = json_get_double(root, JSON_KEY_OPERATOR_LAT, 0.0);
    det->operator_lon = json_get_double(root, JSON_KEY_OPERATOR_LON, 0.0);
    const char *op_id = json_get_string(root, JSON_KEY_OPERATOR_ID, "");
    strncpy(det->operator_id, op_id, sizeof(det->operator_id) - 1);

    /* ASTM fields */
    det->ua_type       = (uint8_t)json_get_int(root, JSON_KEY_UA_TYPE, 0);
    det->id_type       = (uint8_t)json_get_int(root, JSON_KEY_ID_TYPE, 0);
    const char *self_id = json_get_string(root, JSON_KEY_SELF_ID, "");
    strncpy(det->self_id_text, self_id, sizeof(det->self_id_text) - 1);
    det->height_agl_m   = json_get_double(root, JSON_KEY_HEIGHT_AGL, 0.0);
    det->geodetic_alt_m = json_get_double(root, JSON_KEY_GEODETIC_ALT, 0.0);
    det->h_accuracy_m   = (float)json_get_double(root, JSON_KEY_H_ACCURACY, 0.0);
    det->v_accuracy_m   = (float)json_get_double(root, JSON_KEY_V_ACCURACY, 0.0);

    /* WiFi-specific */
    const char *ssid = json_get_string(root, JSON_KEY_SSID, "");
    strncpy(det->ssid, ssid, sizeof(det->ssid) - 1);
    const char *bssid = json_get_string(root, JSON_KEY_BSSID, "");
    strncpy(det->bssid, bssid, sizeof(det->bssid) - 1);
    det->freq_mhz          = json_get_int(root, JSON_KEY_FREQ, 0);
    det->channel_width_mhz = json_get_int(root, JSON_KEY_CHANNEL_WIDTH, 0);
    det->wifi_auth_mode    = (uint8_t)json_get_int(root, JSON_KEY_WIFI_AUTH_MODE, 0xFF);

    /* Probe request: extract probed SSIDs and fingerprint */
    json_get_string_or_array_csv(
        root,
        JSON_KEY_PROBED_SSIDS,
        det->probed_ssids,
        sizeof(det->probed_ssids)
    );
    const char *ie_hash_str = json_get_string(root, "ie_hash", NULL);
    if (ie_hash_str) {
        det->probe_ie_hash = (uint32_t)strtoul(ie_hash_str, NULL, 16);
    }
    det->wifi_generation = (uint8_t)json_get_int(root, "wifi_gen", 0);

    /* Timestamps */
    det->first_seen_ms    = (int64_t)json_get_double(root, JSON_KEY_FIRST_SEEN, 0.0);
    det->last_updated_ms  = (int64_t)json_get_double(root, JSON_KEY_LAST_UPDATED, 0.0);

    /* Fused confidence */
    det->fused_confidence = (float)json_get_double(root, JSON_KEY_FUSED_CONFIDENCE, 0.0);

    /* BLE fingerprinting fields */
    det->ble_company_id = (uint16_t)json_get_double(root, JSON_KEY_BLE_COMPANY_ID, 0);
    det->ble_apple_type = (uint8_t)json_get_double(root, JSON_KEY_BLE_APPLE_TYPE, 0);
    det->ble_ad_type_count = (uint8_t)json_get_double(root, JSON_KEY_BLE_AD_TYPES, 0);
    det->ble_payload_len = (uint8_t)json_get_double(root, JSON_KEY_BLE_PAYLOAD_LEN, 0);
    det->ble_addr_type = (uint8_t)json_get_double(root, JSON_KEY_BLE_ADDR_TYPE, 0);

    /* BLE-JA3 structural profile hash */
    const char *ja3_str = json_get_string(root, JSON_KEY_BLE_JA3, NULL);
    if (ja3_str) {
        det->ble_ja3_hash = (uint32_t)strtoul(ja3_str, NULL, 16);
    }
    const char *ble_name = json_get_string(root, JSON_KEY_BLE_NAME, "");
    strncpy(det->ble_name, ble_name, sizeof(det->ble_name) - 1);
    const char *class_reason = json_get_string(root, JSON_KEY_CLASS_REASON, "");
    strncpy(det->class_reason, class_reason, sizeof(det->class_reason) - 1);

    /* Apple Continuity deep fields (previously dropped — fixes entity resolution) */
    const char *auth_str = json_get_string(root, JSON_KEY_BLE_APPLE_AUTH, NULL);
    if (auth_str && strlen(auth_str) == 6) {
        for (int i = 0; i < 3; i++) {
            char hex2[3] = { auth_str[i*2], auth_str[i*2+1], '\0' };
            det->ble_apple_auth[i] = (uint8_t)strtoul(hex2, NULL, 16);
        }
    }
    det->ble_apple_activity = (uint8_t)json_get_int(root, JSON_KEY_BLE_ACTIVITY, 0);
    det->ble_apple_flags = (uint8_t)json_get_int(root, JSON_KEY_BLE_APPLE_FLAGS, 0);

    /* Raw manufacturer data (hex string → byte array) */
    const char *mfr_hex = json_get_string(root, JSON_KEY_BLE_RAW_MFR, NULL);
    if (mfr_hex) {
        int hex_len = strlen(mfr_hex);
        int byte_count = hex_len / 2;
        if (byte_count > 20) byte_count = 20;
        for (int i = 0; i < byte_count; i++) {
            char hex2[3] = { mfr_hex[i*2], mfr_hex[i*2+1], '\0' };
            det->ble_raw_mfr[i] = (uint8_t)strtoul(hex2, NULL, 16);
        }
        det->ble_raw_mfr_len = (uint8_t)byte_count;
    }

    /* Advertisement interval */
    double ival_ms = json_get_double(root, JSON_KEY_BLE_ADV_INTERVAL, 0.0);
    if (ival_ms > 0) {
        det->ble_adv_interval_us = (int64_t)(ival_ms * 1000);
    }

    /* BLE service UUIDs may include 16-bit hex ("fd5f") and/or 128-bit
     * hyphenated UUIDs ("cafe9a86-0000-1000-8000-..."). Preserve the raw
     * scanner string for backend matching, and mirror 16-bit tokens into
     * det->ble_service_uuids for internal summaries that still read it. */
    const char *svc_str = json_get_string(root, JSON_KEY_BLE_SVC_UUIDS, NULL);
    if (svc_str) {
        /* Pass-through raw copy — this is what http_upload actually sends. */
        strncpy(det->ble_svc_uuids_raw, svc_str, sizeof(det->ble_svc_uuids_raw) - 1);
        det->ble_svc_uuids_raw[sizeof(det->ble_svc_uuids_raw) - 1] = '\0';

        /* Best-effort 16-bit mirror. strtoul stops at '-' for 128-bit tokens. */
        char svc_buf[160];
        strncpy(svc_buf, svc_str, sizeof(svc_buf) - 1);
        svc_buf[sizeof(svc_buf) - 1] = '\0';
        char *tok = svc_buf;
        while (*tok && det->ble_svc_uuid_count < 4) {
            det->ble_service_uuids[det->ble_svc_uuid_count++] =
                (uint16_t)strtoul(tok, NULL, 16);
            char *comma = strchr(tok, ',');
            if (comma) { tok = comma + 1; } else { break; }
        }
    }

    /* Timestamp fallback */
    if (det->last_updated_ms == 0) {
        det->last_updated_ms = (int64_t)json_get_double(root, JSON_KEY_TIMESTAMP, 0.0);
    }

    return true;
}

/* ── Handle a parsed status message ────────────────────────────────────── */

static void handle_status(const cJSON *root, int scanner_id)
{
    int ble_count  = json_get_int(root, "ble_count", 0);
    int wifi_count = json_get_int(root, "wifi_count", 0);
    int channel    = json_get_int(root, "ch", 0);
    int uptime     = json_get_int(root, "uptime_s", 0);

    const char *status_profile = json_get_string(root, JSON_KEY_SCAN_PROFILE, "");
    ESP_LOGI(TAG, "Scanner[%d] status: BLE=%d WiFi=%d ch=%d uptime=%ds profile=%s",
             scanner_id, ble_count, wifi_count, channel, uptime,
             status_profile && status_profile[0] ? status_profile : "?");

    /* Extract scanner identity if present (piggybacks on status message) */
    const char *ver = json_get_string(root, "ver", NULL);
    if (ver) {
        scanner_info_t *info = (scanner_id == 0) ? &s_ble_scanner_info : &s_wifi_scanner_info;
        const char *board = json_get_string(root, "board", "?");
        const char *chip = json_get_string(root, "chip", "?");
        const char *caps = json_get_string(root, "caps", "?");
        strncpy(info->version, ver, sizeof(info->version) - 1);
        strncpy(info->board, board, sizeof(info->board) - 1);
        strncpy(info->chip, chip, sizeof(info->chip) - 1);
        strncpy(info->caps, caps, sizeof(info->caps) - 1);
        info->version[sizeof(info->version) - 1] = '\0';
        info->board[sizeof(info->board) - 1] = '\0';
        info->chip[sizeof(info->chip) - 1] = '\0';
        info->caps[sizeof(info->caps) - 1] = '\0';
        if (!info->received) {
            info->received = true;
            ESP_LOGI(TAG, "Scanner[%d] identity from status: %s v%s (%s)",
                     scanner_id, board, ver, caps);
        }
    }

    /* Attack / anomaly counters */
    {
        scanner_info_t *info = (scanner_id == 0) ? &s_ble_scanner_info : &s_wifi_scanner_info;
        int64_t now_ms = esp_timer_get_time() / 1000;
        info->deauth_count  = (uint16_t)json_get_int(root, "deauth", 0);
        info->disassoc_count = (uint16_t)json_get_int(root, "disassoc", 0);
        info->auth_count    = (uint16_t)json_get_int(root, "auth_fr", 0);
        info->uart_tx_dropped = (uint32_t)json_get_double(root, "uart_tx_dropped", 0);
        info->uart_tx_high_water = (uint32_t)json_get_double(root, "uart_tx_high_water", 0);
        info->tx_queue_depth = (uint32_t)json_get_double(root, "tx_queue_depth", 0);
        info->tx_queue_capacity = (uint32_t)json_get_double(root, "tx_queue_capacity", 0);
        info->tx_queue_pressure_pct = (uint32_t)json_get_double(root, "tx_queue_pressure_pct", 0);
        info->noise_drop_ble = (uint32_t)json_get_double(root, "noise_drop_ble", 0);
        info->noise_drop_wifi = (uint32_t)json_get_double(root, "noise_drop_wifi", 0);
        info->probe_seen = (uint32_t)json_get_double(root, "probe_seen", 0);
        info->probe_sent = (uint32_t)json_get_double(root, "probe_sent", 0);
        info->probe_drop_low_value = (uint32_t)json_get_double(root, "probe_drop_low_value", 0);
        info->probe_drop_rate_limit = (uint32_t)json_get_double(root, "probe_drop_rate_limit", 0);
        info->probe_drop_pressure = (uint32_t)json_get_double(root, "probe_drop_pressure", 0);
        const cJSON *ble_scanning = cJSON_GetObjectItemCaseSensitive(root, "ble_scanning");
        info->ble_scanning = (ble_scanning && cJSON_IsTrue(ble_scanning)) ||
                             (ble_scanning && cJSON_IsNumber(ble_scanning) &&
                              ble_scanning->valueint != 0);
        const cJSON *ble_host_active = cJSON_GetObjectItemCaseSensitive(root, "ble_host_active");
        info->ble_host_active = (ble_host_active && cJSON_IsTrue(ble_host_active)) ||
                                (ble_host_active && cJSON_IsNumber(ble_host_active) &&
                                 ble_host_active->valueint != 0);
        const cJSON *ble_host_synced = cJSON_GetObjectItemCaseSensitive(root, "ble_host_synced");
        info->ble_host_synced = (ble_host_synced && cJSON_IsTrue(ble_host_synced)) ||
                                (ble_host_synced && cJSON_IsNumber(ble_host_synced) &&
                                 ble_host_synced->valueint != 0);
        const cJSON *wifi_paused = cJSON_GetObjectItemCaseSensitive(root, "wifi_paused");
        info->wifi_paused = (wifi_paused && cJSON_IsTrue(wifi_paused)) ||
                            (wifi_paused && cJSON_IsNumber(wifi_paused) &&
                             wifi_paused->valueint != 0);
        info->wifi_total_frames = (uint32_t)json_get_double(root, "wifi_total_frames",
                                                            (double)info->wifi_total_frames);
        info->wifi_beacon_frames = (uint32_t)json_get_double(root, "wifi_beacon_frames",
                                                             (double)info->wifi_beacon_frames);
        info->wifi_full_scan_count = (uint32_t)json_get_double(root, "wifi_full_scan_count",
                                                               (double)info->wifi_full_scan_count);
        info->wifi_full_scan_ok = (uint32_t)json_get_double(root, "wifi_full_scan_ok",
                                                            (double)info->wifi_full_scan_ok);
        info->wifi_full_scan_err = (uint32_t)json_get_double(root, "wifi_full_scan_err",
                                                             (double)info->wifi_full_scan_err);
        info->wifi_full_scan_last_rc = (int)json_get_double(root, "wifi_full_scan_last_rc",
                                                            (double)info->wifi_full_scan_last_rc);
        info->wifi_last_ap_count = (uint32_t)json_get_double(root, "wifi_last_ap_count",
                                                             (double)info->wifi_last_ap_count);
        info->wifi_last_scan_age_s = (int64_t)json_get_double(root, "wifi_last_scan_age_s",
                                                              (double)info->wifi_last_scan_age_s);
        uint32_t previous_drone_ssid_emit = info->wifi_drone_ssid_emit;
        uint32_t previous_notable_ssid_emit = info->wifi_notable_ssid_emit;
        info->wifi_drone_ssid_emit = (uint32_t)json_get_double(root, "wifi_drone_ssid_emit",
                                                               (double)info->wifi_drone_ssid_emit);
        info->wifi_notable_ssid_emit = (uint32_t)json_get_double(root, "wifi_notable_ssid_emit",
                                                                 (double)info->wifi_notable_ssid_emit);
        json_update_scanner_ssid_freshness(
            root,
            "wifi_last_drone_ssid",
            info->wifi_last_drone_ssid,
            sizeof(info->wifi_last_drone_ssid),
            previous_drone_ssid_emit,
            info->wifi_drone_ssid_emit,
            now_ms,
            &info->wifi_last_drone_ssid_seen_ms,
            &info->wifi_last_drone_ssid_age_s
        );
        json_update_scanner_ssid_freshness(
            root,
            "wifi_last_notable_ssid",
            info->wifi_last_notable_ssid,
            sizeof(info->wifi_last_notable_ssid),
            previous_notable_ssid_emit,
            info->wifi_notable_ssid_emit,
            now_ms,
            &info->wifi_last_notable_ssid_seen_ms,
            &info->wifi_last_notable_ssid_age_s
        );
        info->wifi_oui_emit = (uint32_t)json_get_double(root, "wifi_oui_emit",
                                                        (double)info->wifi_oui_emit);
        info->wifi_soft_ssid_emit = (uint32_t)json_get_double(root, "wifi_soft_ssid_emit",
                                                              (double)info->wifi_soft_ssid_emit);
        info->wifi_hot_ch = (uint32_t)json_get_double(root, "wifi_hot_ch",
                                                      (double)info->wifi_hot_ch);
        info->ble_adv_seen = (uint32_t)json_get_double(root, "ble_adv_seen", (double)info->ble_adv_seen);
        info->ble_any_seen = (uint32_t)json_get_double(root, "ble_any_seen", (double)info->ble_any_seen);
        info->ble_any_with_payload_seen = (uint32_t)json_get_double(root, "ble_any_with_payload_seen", (double)info->ble_any_with_payload_seen);
        info->ble_any_empty_seen = (uint32_t)json_get_double(root, "ble_any_empty_seen", (double)info->ble_any_empty_seen);
        info->ble_any_last_rssi = (int8_t)json_get_double(root, "ble_any_last_rssi", (double)info->ble_any_last_rssi);
        info->ble_any_best_rssi = (int8_t)json_get_double(root, "ble_any_best_rssi", (double)info->ble_any_best_rssi);
        info->ble_any_last_len = (uint8_t)json_get_double(root, "ble_any_last_len", (double)info->ble_any_last_len);
        info->ble_any_last_props = (uint8_t)json_get_double(root, "ble_any_last_props", (double)info->ble_any_last_props);
        info->ble_any_last_addr_type = (uint8_t)json_get_double(root, "ble_any_last_addr_type", (double)info->ble_any_last_addr_type);
        info->ble_fp_emit = (uint32_t)json_get_double(root, "ble_fp_emit", (double)info->ble_fp_emit);
        info->ble_meta_seen = (uint32_t)json_get_double(root, "ble_meta_seen", (double)info->ble_meta_seen);
        info->ble_meta_last_seen_age_s = (int64_t)json_get_double(
            root, "ble_meta_last_seen_age_s", -1.0
        );
        info->ble_meta_last_emit_age_s = (int64_t)json_get_double(
            root, "ble_meta_last_emit_age_s", -1.0
        );
        info->ble_meta_last_hash = (uint32_t)json_get_double(
            root, "ble_meta_last_hash", (double)info->ble_meta_last_hash
        );
        info->ble_meta_last_rssi = (int8_t)json_get_double(
            root, "ble_meta_last_rssi", (double)info->ble_meta_last_rssi
        );
        json_copy_string(root, "ble_meta_last_reason",
                         info->ble_meta_last_reason,
                         sizeof(info->ble_meta_last_reason));
        json_copy_string(root, "ble_meta_identity",
                         info->ble_meta_identity,
                         sizeof(info->ble_meta_identity));
        info->ble_meta_weak_age_s = (int64_t)json_get_double(
            root, "ble_meta_weak_age_s", -1.0
        );
        info->ble_meta_reacquire_count = (uint32_t)json_get_double(
            root, "ble_meta_reacquire_count",
            (double)info->ble_meta_reacquire_count
        );
        info->ble_tracker_seen = (uint32_t)json_get_double(root, "ble_tracker_seen", (double)info->ble_tracker_seen);
        info->ble_privacy_candidate_seen = (uint32_t)json_get_double(
            root, "ble_privacy_candidate_seen", (double)info->ble_privacy_candidate_seen
        );
        info->ble_near_unknown_seen = (uint32_t)json_get_double(
            root, "ble_near_unknown_seen", (double)info->ble_near_unknown_seen
        );
        info->ble_drop_profile = (uint32_t)json_get_double(root, "ble_drop_profile", (double)info->ble_drop_profile);
        info->ble_drop_rate = (uint32_t)json_get_double(root, "ble_drop_rate", (double)info->ble_drop_rate);
        info->ble_dbg_near_seen = (uint32_t)json_get_double(root, "ble_dbg_near_seen", (double)info->ble_dbg_near_seen);
        info->ble_dbg_near_rssi = (int8_t)json_get_double(root, "ble_dbg_near_rssi", (double)info->ble_dbg_near_rssi);
        json_copy_string(root, "ble_dbg_near_label", info->ble_dbg_near_label, sizeof(info->ble_dbg_near_label));
        json_copy_string(root, "ble_dbg_near_name", info->ble_dbg_near_name, sizeof(info->ble_dbg_near_name));
        json_copy_string(root, "ble_dbg_near_reason", info->ble_dbg_near_reason, sizeof(info->ble_dbg_near_reason));
        info->ble_dbg_near_cid = (uint16_t)json_get_double(root, "ble_dbg_near_cid", (double)info->ble_dbg_near_cid);
        info->ble_dbg_near_svc0 = (uint16_t)json_get_double(root, "ble_dbg_near_svc0", (double)info->ble_dbg_near_svc0);
        info->ble_dbg_near_svc_count = (uint8_t)json_get_double(root, "ble_dbg_near_svc_count", (double)info->ble_dbg_near_svc_count);
        info->ble_dbg_near_payload_len = (uint8_t)json_get_double(root, "ble_dbg_near_payload_len", (double)info->ble_dbg_near_payload_len);
        info->ble_dbg_priv_seen = (uint32_t)json_get_double(root, "ble_dbg_priv_seen", (double)info->ble_dbg_priv_seen);
        info->ble_dbg_priv_rssi = (int8_t)json_get_double(root, "ble_dbg_priv_rssi", (double)info->ble_dbg_priv_rssi);
        json_copy_string(root, "ble_dbg_priv_label", info->ble_dbg_priv_label, sizeof(info->ble_dbg_priv_label));
        json_copy_string(root, "ble_dbg_priv_name", info->ble_dbg_priv_name, sizeof(info->ble_dbg_priv_name));
        json_copy_string(root, "ble_dbg_priv_reason", info->ble_dbg_priv_reason, sizeof(info->ble_dbg_priv_reason));
        info->ble_dbg_priv_cid = (uint16_t)json_get_double(root, "ble_dbg_priv_cid", (double)info->ble_dbg_priv_cid);
        info->ble_dbg_priv_svc0 = (uint16_t)json_get_double(root, "ble_dbg_priv_svc0", (double)info->ble_dbg_priv_svc0);
        info->ble_dbg_priv_svc_count = (uint8_t)json_get_double(root, "ble_dbg_priv_svc_count", (double)info->ble_dbg_priv_svc_count);
        info->ble_dbg_priv_payload_len = (uint8_t)json_get_double(root, "ble_dbg_priv_payload_len", (double)info->ble_dbg_priv_payload_len);
        info->ble_host_restart_count = (uint32_t)json_get_double(root, "ble_host_restart_count",
                                                                 (double)info->ble_host_restart_count);
        info->ble_scan_start_count = (uint32_t)json_get_double(
            root, "ble_scan_start_count", (double)info->ble_scan_start_count
        );
        info->ble_scan_start_ok = (uint32_t)json_get_double(
            root, "ble_scan_start_ok", (double)info->ble_scan_start_ok
        );
        info->ble_scan_last_rc = (int)json_get_double(
            root, "ble_scan_last_rc", (double)info->ble_scan_last_rc
        );
        info->ble_sync_last_rc = (int)json_get_double(
            root, "ble_sync_last_rc", (double)info->ble_sync_last_rc
        );
        const cJSON *ble_focus_active = cJSON_GetObjectItemCaseSensitive(root, "ble_focus_active");
        if (ble_focus_active) {
            info->ble_focus_active = cJSON_IsTrue(ble_focus_active) ||
                                     (cJSON_IsNumber(ble_focus_active) &&
                                      ble_focus_active->valueint != 0);
        }
        info->ble_focus_age_s = (int64_t)json_get_double(
            root, "ble_focus_age_s", -1.0
        );
        info->ble_focus_target_adv_count = (uint32_t)json_get_double(
            root, "ble_focus_target_adv_count", (double)info->ble_focus_target_adv_count
        );
        info->rid_service_seen = (uint32_t)json_get_double(root, "rid_service_seen", (double)info->rid_service_seen);
        info->rid_emit = (uint32_t)json_get_double(root, "rid_emit", (double)info->rid_emit);
        info->rid_queue_drop = (uint32_t)json_get_double(root, "rid_queue_drop", (double)info->rid_queue_drop);
        info->rid_queue_evict = (uint32_t)json_get_double(root, "rid_queue_evict", (double)info->rid_queue_evict);
        info->privacy_seen = (uint32_t)json_get_double(root, "privacy_seen", (double)info->privacy_seen);
        info->toff_ms = (int64_t)json_get_double(root, "toff", (double)info->toff_ms);
        info->tcnt = (uint32_t)json_get_int(root, "tcnt", (int)info->tcnt);
        info->time_valid_count = (uint32_t)json_get_double(root, "time_valid_count", (double)info->time_valid_count);
        info->time_last_valid_age_s = (int64_t)json_get_double(
            root, "time_last_valid_age_s", (double)info->time_last_valid_age_s
        );
        const char *time_state = json_get_string(root, "time_sync_state", info->time_sync_state);
        if (time_state) {
            strncpy(info->time_sync_state, time_state, sizeof(info->time_sync_state) - 1);
            info->time_sync_state[sizeof(info->time_sync_state) - 1] = '\0';
        }
        info->cmd_rx_count = (uint32_t)json_get_double(root, "cmd_rx", (double)info->cmd_rx_count);
        info->cmd_parse_error_count = (uint32_t)json_get_double(
            root, "cmd_parse_err", (double)info->cmd_parse_error_count
        );
        info->cmd_overflow_count = (uint32_t)json_get_double(root, "cmd_overflow", (double)info->cmd_overflow_count);
        info->cmd_stale_count = (uint32_t)json_get_double(root, "cmd_stale", (double)info->cmd_stale_count);
        info->cmd_last_age_s = (int64_t)json_get_double(root, "cmd_last_age_s", (double)info->cmd_last_age_s);
        info->display_policy_hash = (uint32_t)json_get_double(
            root, "display_policy_hash", (double)info->display_policy_hash
        );
        info->display_policy_ack_hash = (uint32_t)json_get_double(
            root, "display_policy_ack_hash",
            (double)(info->display_policy_ack_hash
                         ? info->display_policy_ack_hash
                         : info->display_policy_hash)
        );
        const cJSON *filtered = cJSON_GetObjectItemCaseSensitive(root, "filtered_counts");
        if (cJSON_IsObject(filtered)) {
            for (int i = 0; i < BADGE_DISPLAY_POLICY_CLASS_COUNT; i++) {
                badge_display_policy_class_t cls = (badge_display_policy_class_t)i;
                info->display_policy_filtered[i] = (uint32_t)json_get_double(
                    filtered,
                    badge_display_policy_class_key(cls),
                    (double)info->display_policy_filtered[i]
                );
            }
        }
        const char *scan_mode = json_get_string(root, JSON_KEY_SCAN_MODE, info->scan_mode[0] ? info->scan_mode : "normal");
        strncpy(info->scan_mode, scan_mode, sizeof(info->scan_mode) - 1);
        info->scan_mode[sizeof(info->scan_mode) - 1] = '\0';
        const char *scan_profile = json_get_string(root, JSON_KEY_SCAN_PROFILE, info->scan_profile[0] ? info->scan_profile : "");
        strncpy(info->scan_profile, scan_profile ? scan_profile : "", sizeof(info->scan_profile) - 1);
        info->scan_profile[sizeof(info->scan_profile) - 1] = '\0';
        const char *cal_uuid = json_get_string(root, JSON_KEY_CALIBRATION_UUID, info->calibration_uuid);
        strncpy(info->calibration_uuid, cal_uuid ? cal_uuid : "", sizeof(info->calibration_uuid) - 1);
        info->calibration_uuid[sizeof(info->calibration_uuid) - 1] = '\0';
        info->calibration_mode_acked =
            strcmp(info->scan_mode, "calibration") == 0 &&
            info->calibration_uuid[0] != '\0';
        const cJSON *need_fw = cJSON_GetObjectItemCaseSensitive(root, "need_firmware");
        info->need_firmware = (need_fw && cJSON_IsTrue(need_fw)) ||
                              (need_fw && cJSON_IsNumber(need_fw) && need_fw->valueint != 0);
        const char *fw_state = json_get_string(root, JSON_KEY_FW_STATE, info->fw_update_state);
        strncpy(info->fw_update_state, fw_state ? fw_state : "", sizeof(info->fw_update_state) - 1);
        info->fw_update_state[sizeof(info->fw_update_state) - 1] = '\0';
        const char *target_ver = json_get_string(root, JSON_KEY_FW_TARGET_VERSION, info->fw_target_version);
        strncpy(info->fw_target_version, target_ver ? target_ver : "", sizeof(info->fw_target_version) - 1);
        info->fw_target_version[sizeof(info->fw_target_version) - 1] = '\0';
        info->fw_check_count = (uint32_t)json_get_double(root, "fw_check_count", (double)info->fw_check_count);
        info->fw_backoff_s = (int64_t)json_get_double(root, "fw_backoff_s", (double)info->fw_backoff_s);
        const char *last_fw_error = json_get_string(root, "last_fw_error", info->last_fw_error);
        strncpy(info->last_fw_error, last_fw_error ? last_fw_error : "", sizeof(info->last_fw_error) - 1);
        info->last_fw_error[sizeof(info->last_fw_error) - 1] = '\0';
        json_copy_string(root, "ota_state", info->ota_state, sizeof(info->ota_state));
        json_copy_string(root, "ota_session_id", info->ota_session_id, sizeof(info->ota_session_id));
        info->ota_received = (uint32_t)json_get_double(root, "ota_received", (double)info->ota_received);
        info->ota_total = (uint32_t)json_get_double(root, "ota_total", (double)info->ota_total);
        json_copy_string(root, "recovery_mode", info->recovery_mode, sizeof(info->recovery_mode));
        json_copy_string(root, "safe_reason", info->safe_reason, sizeof(info->safe_reason));
        const cJSON *rollback_pending = cJSON_GetObjectItemCaseSensitive(root, "rollback_pending");
        if (rollback_pending) {
            info->rollback_pending = cJSON_IsTrue(rollback_pending) ||
                                     (cJSON_IsNumber(rollback_pending) &&
                                      rollback_pending->valueint != 0);
        }
        info->crash_count = (uint32_t)json_get_double(root, "crash_count", (double)info->crash_count);
        info->radio_restart_count = (uint32_t)json_get_double(root, "radio_restart_count",
                                                              (double)info->radio_restart_count);

        const cJSON *flood_item = cJSON_GetObjectItemCaseSensitive(root, "flood");
        info->deauth_flood = (flood_item && cJSON_IsTrue(flood_item));

        const cJSON *spam_item = cJSON_GetObjectItemCaseSensitive(root, "bcn_spam");
        info->beacon_spam = (spam_item && cJSON_IsTrue(spam_item));

        const char *hist = json_get_string(root, "fc_hist", NULL);
        if (hist) {
            strncpy(info->fc_hist, hist, sizeof(info->fc_hist) - 1);
            info->fc_hist[sizeof(info->fc_hist) - 1] = '\0';
        }

        if (info->deauth_flood) {
            ESP_LOGW(TAG, "Scanner[%d] DEAUTH FLOOD detected! deauth=%d",
                     scanner_id, info->deauth_count);
        }
        if (info->beacon_spam) {
            ESP_LOGW(TAG, "Scanner[%d] BEACON SPAM detected!", scanner_id);
        }
#ifdef FOF_BADGE_VARIANT
        if (info->deauth_flood || info->deauth_count >= 3) {
            badge_ingest_wifi_status_event(
                info->deauth_flood ? "Deauth Flood" : "Deauth",
                "deauth",
                info->deauth_flood ? 0.90f : 0.70f,
                info->deauth_count > 0 ? info->deauth_count : 1
            );
        }
        if (info->disassoc_count >= 3) {
            badge_ingest_wifi_status_event("Disassoc", "disassoc", 0.65f,
                                           info->disassoc_count);
        }
        if (info->beacon_spam) {
            badge_ingest_wifi_status_event("Beacon Spam", "beacon spam", 0.75f, 1);
        }
        badge_ingest_scanner_status_evidence(scanner_id, info);
#endif
    }

    /* First status message from scanner: flash "connected!" */
    if (!s_first_status_received) {
        s_first_status_received = true;
        led_set_pattern(LED_DETECTION);
        ESP_LOGI(TAG, "Scanner connected (first status received)");
    }
}

/* ── Process one complete JSON line ────────────────────────────────────── */

static void process_line(const char *line, size_t len, int scanner_id)
{
    int_fast64_t now_ms = (int_fast64_t)(esp_timer_get_time() / 1000);

    cJSON *root = cJSON_ParseWithLength(line, len);
    if (!root) {
        note_scanner_json_parse_error(scanner_id);
        ESP_LOGW(TAG, "Scanner[%d] JSON parse error: %.64s...",
                 scanner_id, line);
        return;
    }

    const char *msg_type = json_get_string(root, JSON_KEY_TYPE, NULL);
    if (!msg_type) {
        ESP_LOGW(TAG, "Scanner[%d] message missing 'type' field", scanner_id);
        cJSON_Delete(root);
        return;
    }

    if (!msg_type_is_scanner_originated(msg_type)) {
        ESP_LOGW(TAG, "Scanner[%d] echoed/non-scanner msg type='%s' ignored",
                 scanner_id, msg_type);
        cJSON_Delete(root);
        return;
    }

    note_scanner_activity(scanner_id, now_ms);

    /* A stopped scanner still emits identity / status, so use scanner-originated
     * traffic as a chance to release backpressure once the queue has drained. */
    maybe_resume_scanner(scanner_id);

    if (strcmp(msg_type, MSG_TYPE_DETECTION) == 0) {
        drone_detection_t det;
        if (parse_detection(root, &det)) {
            det.scanner_slot = (uint8_t)scanner_id;
            det.scanner_slots_seen = (uint8_t)(1U << scanner_id);

            /* Skip BLE background noise (0.02 confidence) to reduce queue pressure.
             * WiFi APs (0.05) and phones (0.05) are still useful for the backend. */
            if (det.confidence < 0.04f &&
                det.source != DETECTION_SRC_WIFI_AP_INVENTORY) {
                push_recent(&det);  /* Still show in recent list */
                cJSON_Delete(root);
                return;
            }

            if (!uart_rx_node_mode_allows_detection(&det)) {
                cJSON_Delete(root);
                return;
            }

#ifdef FOF_BADGE_VARIANT
            badge_threat_event_t badge_event = {0};
            (void)badge_ingest_detection(&det, &badge_event);
            s_detection_count++;
            push_recent(&det);
            cJSON_Delete(root);
            return;
#endif

            UBaseType_t queue_count = uxQueueMessagesWaiting(s_detection_queue);
            bool low_value_ble = is_low_value_ble_detection(&det);

            /* Under sustained queue pressure, shed low-value BLE fingerprints
             * before they can starve WiFi scans or real RID packets. */
            if (low_value_ble &&
                queue_count >= (CONFIG_DETECTION_QUEUE_SIZE / 2)) {
                push_recent(&det);
                cJSON_Delete(root);
                return;
            }

            /* Backpressure: if queue is nearly full, pause only the scanner
             * that is contributing this traffic. Global stop/start caused BLE
             * bursts to starve the WiFi scanner and left scanners stuck until
             * the watchdog re-sent "ready". */
            _Atomic bool *bp_flag = backpressure_flag_for_scanner(scanner_id);
            if (queue_count >= (CONFIG_DETECTION_QUEUE_SIZE * 8 / 10)) {
                if (!atomic_load(bp_flag)) {
                    send_scanner_flow_cmd(scanner_id, "stop");
                    ESP_LOGW(TAG, "Queue pressure %d/%d — throttling scanner[%d]",
                             (int)queue_count, CONFIG_DETECTION_QUEUE_SIZE, scanner_id);
                    atomic_store(bp_flag, true);
                }
            } else if (atomic_load(bp_flag) &&
                       queue_count <= (CONFIG_DETECTION_QUEUE_SIZE * 4 / 10)) {
                send_scanner_flow_cmd(scanner_id, "start");
                ESP_LOGI(TAG, "Queue drained %d/%d — resuming scanner[%d]",
                         (int)queue_count, CONFIG_DETECTION_QUEUE_SIZE, scanner_id);
                atomic_store(bp_flag, false);
            }

            if (xQueueSend(s_detection_queue, &det, pdMS_TO_TICKS(10)) == pdTRUE) {
                s_detection_count++;
            } else {
                ESP_LOGW(TAG, "Detection queue full, dropping: %s", det.drone_id);
            }
            push_recent(&det);
        }
    } else if (strcmp(msg_type, MSG_TYPE_STATUS) == 0) {
        handle_status(root, scanner_id);
    } else if (strcmp(msg_type, "scanner_info") == 0) {
        /* Scanner identity: version, board type, chip, capabilities, time-offset */
        scanner_info_t *info = (scanner_id == 0) ? &s_ble_scanner_info : &s_wifi_scanner_info;
        const char *ver = json_get_string(root, "ver", "?");
        const char *board = json_get_string(root, "board", "?");
        const char *chip = json_get_string(root, "chip", "?");
        const char *caps = json_get_string(root, "caps", "?");
        strncpy(info->version, ver, sizeof(info->version) - 1);
        strncpy(info->board, board, sizeof(info->board) - 1);
        strncpy(info->chip, chip, sizeof(info->chip) - 1);
        strncpy(info->caps, caps, sizeof(info->caps) - 1);
        info->version[sizeof(info->version) - 1] = '\0';
        info->board[sizeof(info->board) - 1] = '\0';
        info->chip[sizeof(info->chip) - 1] = '\0';
        info->caps[sizeof(info->caps) - 1] = '\0';
        info->toff_ms = (int64_t)json_get_double(root, "toff", 0.0);
        info->tcnt    = (uint32_t)json_get_int(root, "tcnt", 0);
        info->time_valid_count = (uint32_t)json_get_double(root, "time_valid_count", 0.0);
        info->time_last_valid_age_s = (int64_t)json_get_double(root, "time_last_valid_age_s", -1.0);
        const char *time_state = json_get_string(root, "time_sync_state", "unknown");
        strncpy(info->time_sync_state, time_state, sizeof(info->time_sync_state) - 1);
        info->time_sync_state[sizeof(info->time_sync_state) - 1] = '\0';
        info->cmd_rx_count = (uint32_t)json_get_double(root, "cmd_rx", 0.0);
        info->cmd_parse_error_count = (uint32_t)json_get_double(root, "cmd_parse_err", 0.0);
        info->cmd_overflow_count = (uint32_t)json_get_double(root, "cmd_overflow", 0.0);
        info->cmd_stale_count = (uint32_t)json_get_double(root, "cmd_stale", 0.0);
        info->cmd_last_age_s = (int64_t)json_get_double(root, "cmd_last_age_s", -1.0);
        const cJSON *ble_scanning = cJSON_GetObjectItemCaseSensitive(root, "ble_scanning");
        if (ble_scanning) {
            info->ble_scanning = cJSON_IsTrue(ble_scanning) ||
                                 (cJSON_IsNumber(ble_scanning) &&
                                  ble_scanning->valueint != 0);
        }
        const cJSON *ble_host_active = cJSON_GetObjectItemCaseSensitive(root, "ble_host_active");
        if (ble_host_active) {
            info->ble_host_active = cJSON_IsTrue(ble_host_active) ||
                                    (cJSON_IsNumber(ble_host_active) &&
                                     ble_host_active->valueint != 0);
        }
        const cJSON *ble_host_synced = cJSON_GetObjectItemCaseSensitive(root, "ble_host_synced");
        if (ble_host_synced) {
            info->ble_host_synced = cJSON_IsTrue(ble_host_synced) ||
                                    (cJSON_IsNumber(ble_host_synced) &&
                                     ble_host_synced->valueint != 0);
        }
        const cJSON *wifi_paused = cJSON_GetObjectItemCaseSensitive(root, "wifi_paused");
        if (wifi_paused) {
            info->wifi_paused = cJSON_IsTrue(wifi_paused) ||
                                (cJSON_IsNumber(wifi_paused) &&
                                 wifi_paused->valueint != 0);
        }
        info->wifi_full_scan_count = (uint32_t)json_get_double(root, "wifi_full_scan_count",
                                                               (double)info->wifi_full_scan_count);
        info->wifi_full_scan_ok = (uint32_t)json_get_double(root, "wifi_full_scan_ok",
                                                            (double)info->wifi_full_scan_ok);
        info->wifi_last_ap_count = (uint32_t)json_get_double(root, "wifi_last_ap_count",
                                                             (double)info->wifi_last_ap_count);
        info->wifi_last_scan_age_s = (int64_t)json_get_double(root, "wifi_last_scan_age_s",
                                                              (double)info->wifi_last_scan_age_s);
        uint32_t previous_drone_ssid_emit = info->wifi_drone_ssid_emit;
        uint32_t previous_notable_ssid_emit = info->wifi_notable_ssid_emit;
        int64_t now_ms = esp_timer_get_time() / 1000;
        info->wifi_drone_ssid_emit = (uint32_t)json_get_double(root, "wifi_drone_ssid_emit",
                                                               (double)info->wifi_drone_ssid_emit);
        info->wifi_notable_ssid_emit = (uint32_t)json_get_double(root, "wifi_notable_ssid_emit",
                                                                 (double)info->wifi_notable_ssid_emit);
        json_update_scanner_ssid_freshness(
            root,
            "wifi_last_drone_ssid",
            info->wifi_last_drone_ssid,
            sizeof(info->wifi_last_drone_ssid),
            previous_drone_ssid_emit,
            info->wifi_drone_ssid_emit,
            now_ms,
            &info->wifi_last_drone_ssid_seen_ms,
            &info->wifi_last_drone_ssid_age_s
        );
        json_update_scanner_ssid_freshness(
            root,
            "wifi_last_notable_ssid",
            info->wifi_last_notable_ssid,
            sizeof(info->wifi_last_notable_ssid),
            previous_notable_ssid_emit,
            info->wifi_notable_ssid_emit,
            now_ms,
            &info->wifi_last_notable_ssid_seen_ms,
            &info->wifi_last_notable_ssid_age_s
        );
        info->ble_adv_seen = (uint32_t)json_get_double(root, "ble_adv_seen", (double)info->ble_adv_seen);
        info->ble_any_seen = (uint32_t)json_get_double(root, "ble_any_seen", (double)info->ble_any_seen);
        info->ble_any_with_payload_seen = (uint32_t)json_get_double(root, "ble_any_with_payload_seen", (double)info->ble_any_with_payload_seen);
        info->ble_any_empty_seen = (uint32_t)json_get_double(root, "ble_any_empty_seen", (double)info->ble_any_empty_seen);
        info->ble_any_last_rssi = (int8_t)json_get_double(root, "ble_any_last_rssi", (double)info->ble_any_last_rssi);
        info->ble_any_best_rssi = (int8_t)json_get_double(root, "ble_any_best_rssi", (double)info->ble_any_best_rssi);
        info->ble_any_last_len = (uint8_t)json_get_double(root, "ble_any_last_len", (double)info->ble_any_last_len);
        info->ble_any_last_props = (uint8_t)json_get_double(root, "ble_any_last_props", (double)info->ble_any_last_props);
        info->ble_any_last_addr_type = (uint8_t)json_get_double(root, "ble_any_last_addr_type", (double)info->ble_any_last_addr_type);
        info->ble_fp_emit = (uint32_t)json_get_double(root, "ble_fp_emit", (double)info->ble_fp_emit);
        info->ble_meta_seen = (uint32_t)json_get_double(root, "ble_meta_seen", (double)info->ble_meta_seen);
        info->ble_meta_last_seen_age_s = (int64_t)json_get_double(
            root, "ble_meta_last_seen_age_s", -1.0
        );
        info->ble_meta_last_emit_age_s = (int64_t)json_get_double(
            root, "ble_meta_last_emit_age_s", -1.0
        );
        info->ble_meta_last_hash = (uint32_t)json_get_double(
            root, "ble_meta_last_hash", (double)info->ble_meta_last_hash
        );
        info->ble_meta_last_rssi = (int8_t)json_get_double(
            root, "ble_meta_last_rssi", (double)info->ble_meta_last_rssi
        );
        json_copy_string(root, "ble_meta_last_reason",
                         info->ble_meta_last_reason,
                         sizeof(info->ble_meta_last_reason));
        json_copy_string(root, "ble_meta_identity",
                         info->ble_meta_identity,
                         sizeof(info->ble_meta_identity));
        info->ble_meta_weak_age_s = (int64_t)json_get_double(
            root, "ble_meta_weak_age_s", -1.0
        );
        info->ble_meta_reacquire_count = (uint32_t)json_get_double(
            root, "ble_meta_reacquire_count",
            (double)info->ble_meta_reacquire_count
        );
        info->ble_privacy_candidate_seen = (uint32_t)json_get_double(
            root, "ble_privacy_candidate_seen", (double)info->ble_privacy_candidate_seen
        );
        info->ble_tracker_seen = (uint32_t)json_get_double(root, "ble_tracker_seen", (double)info->ble_tracker_seen);
        info->ble_near_unknown_seen = (uint32_t)json_get_double(
            root, "ble_near_unknown_seen", (double)info->ble_near_unknown_seen
        );
        info->ble_dbg_near_seen = (uint32_t)json_get_double(root, "ble_dbg_near_seen", (double)info->ble_dbg_near_seen);
        info->ble_dbg_near_rssi = (int8_t)json_get_double(root, "ble_dbg_near_rssi", (double)info->ble_dbg_near_rssi);
        json_copy_string(root, "ble_dbg_near_label", info->ble_dbg_near_label, sizeof(info->ble_dbg_near_label));
        json_copy_string(root, "ble_dbg_near_name", info->ble_dbg_near_name, sizeof(info->ble_dbg_near_name));
        json_copy_string(root, "ble_dbg_near_reason", info->ble_dbg_near_reason, sizeof(info->ble_dbg_near_reason));
        info->ble_dbg_near_cid = (uint16_t)json_get_double(root, "ble_dbg_near_cid", (double)info->ble_dbg_near_cid);
        info->ble_dbg_near_svc0 = (uint16_t)json_get_double(root, "ble_dbg_near_svc0", (double)info->ble_dbg_near_svc0);
        info->ble_dbg_near_svc_count = (uint8_t)json_get_double(root, "ble_dbg_near_svc_count", (double)info->ble_dbg_near_svc_count);
        info->ble_dbg_near_payload_len = (uint8_t)json_get_double(root, "ble_dbg_near_payload_len", (double)info->ble_dbg_near_payload_len);
        info->ble_dbg_priv_seen = (uint32_t)json_get_double(root, "ble_dbg_priv_seen", (double)info->ble_dbg_priv_seen);
        info->ble_dbg_priv_rssi = (int8_t)json_get_double(root, "ble_dbg_priv_rssi", (double)info->ble_dbg_priv_rssi);
        json_copy_string(root, "ble_dbg_priv_label", info->ble_dbg_priv_label, sizeof(info->ble_dbg_priv_label));
        json_copy_string(root, "ble_dbg_priv_name", info->ble_dbg_priv_name, sizeof(info->ble_dbg_priv_name));
        json_copy_string(root, "ble_dbg_priv_reason", info->ble_dbg_priv_reason, sizeof(info->ble_dbg_priv_reason));
        info->ble_dbg_priv_cid = (uint16_t)json_get_double(root, "ble_dbg_priv_cid", (double)info->ble_dbg_priv_cid);
        info->ble_dbg_priv_svc0 = (uint16_t)json_get_double(root, "ble_dbg_priv_svc0", (double)info->ble_dbg_priv_svc0);
        info->ble_dbg_priv_svc_count = (uint8_t)json_get_double(root, "ble_dbg_priv_svc_count", (double)info->ble_dbg_priv_svc_count);
        info->ble_dbg_priv_payload_len = (uint8_t)json_get_double(root, "ble_dbg_priv_payload_len", (double)info->ble_dbg_priv_payload_len);
        info->ble_scan_start_count = (uint32_t)json_get_double(
            root, "ble_scan_start_count", (double)info->ble_scan_start_count
        );
        info->ble_scan_start_ok = (uint32_t)json_get_double(
            root, "ble_scan_start_ok", (double)info->ble_scan_start_ok
        );
        info->ble_scan_last_rc = (int)json_get_double(
            root, "ble_scan_last_rc", (double)info->ble_scan_last_rc
        );
        info->ble_host_restart_count = (uint32_t)json_get_double(root, "ble_host_restart_count",
                                                                 (double)info->ble_host_restart_count);
        info->ble_sync_last_rc = (int)json_get_double(
            root, "ble_sync_last_rc", (double)info->ble_sync_last_rc
        );
        const cJSON *ble_focus_active = cJSON_GetObjectItemCaseSensitive(root, "ble_focus_active");
        if (ble_focus_active) {
            info->ble_focus_active = cJSON_IsTrue(ble_focus_active) ||
                                     (cJSON_IsNumber(ble_focus_active) &&
                                      ble_focus_active->valueint != 0);
        }
        info->ble_focus_age_s = (int64_t)json_get_double(
            root, "ble_focus_age_s", -1.0
        );
        info->ble_focus_target_adv_count = (uint32_t)json_get_double(
            root, "ble_focus_target_adv_count", (double)info->ble_focus_target_adv_count
        );
        const char *scan_mode = json_get_string(root, JSON_KEY_SCAN_MODE, "normal");
        strncpy(info->scan_mode, scan_mode, sizeof(info->scan_mode) - 1);
        info->scan_mode[sizeof(info->scan_mode) - 1] = '\0';
        const char *scan_profile = json_get_string(root, JSON_KEY_SCAN_PROFILE, "");
        strncpy(info->scan_profile, scan_profile, sizeof(info->scan_profile) - 1);
        info->scan_profile[sizeof(info->scan_profile) - 1] = '\0';
        const char *cal_uuid = json_get_string(root, JSON_KEY_CALIBRATION_UUID, "");
        strncpy(info->calibration_uuid, cal_uuid, sizeof(info->calibration_uuid) - 1);
        info->calibration_uuid[sizeof(info->calibration_uuid) - 1] = '\0';
        info->calibration_mode_acked =
            strcmp(info->scan_mode, "calibration") == 0 &&
            info->calibration_uuid[0] != '\0';
        const cJSON *need_fw = cJSON_GetObjectItemCaseSensitive(root, "need_firmware");
        info->need_firmware = (need_fw && cJSON_IsTrue(need_fw)) ||
                              (need_fw && cJSON_IsNumber(need_fw) && need_fw->valueint != 0);
        const char *fw_state = json_get_string(root, JSON_KEY_FW_STATE, "");
        strncpy(info->fw_update_state, fw_state, sizeof(info->fw_update_state) - 1);
        info->fw_update_state[sizeof(info->fw_update_state) - 1] = '\0';
        const char *target_ver = json_get_string(root, JSON_KEY_FW_TARGET_VERSION, "");
        strncpy(info->fw_target_version, target_ver, sizeof(info->fw_target_version) - 1);
        info->fw_target_version[sizeof(info->fw_target_version) - 1] = '\0';
        info->fw_check_count = (uint32_t)json_get_double(root, "fw_check_count", 0.0);
        info->fw_backoff_s = (int64_t)json_get_double(root, "fw_backoff_s", 0.0);
        const char *last_fw_error = json_get_string(root, "last_fw_error", "");
        strncpy(info->last_fw_error, last_fw_error, sizeof(info->last_fw_error) - 1);
        info->last_fw_error[sizeof(info->last_fw_error) - 1] = '\0';
        json_copy_string(root, "ota_state", info->ota_state, sizeof(info->ota_state));
        json_copy_string(root, "ota_session_id", info->ota_session_id, sizeof(info->ota_session_id));
        info->ota_received = (uint32_t)json_get_double(root, "ota_received", (double)info->ota_received);
        info->ota_total = (uint32_t)json_get_double(root, "ota_total", (double)info->ota_total);
        json_copy_string(root, "recovery_mode", info->recovery_mode, sizeof(info->recovery_mode));
        json_copy_string(root, "safe_reason", info->safe_reason, sizeof(info->safe_reason));
        const cJSON *rollback_pending = cJSON_GetObjectItemCaseSensitive(root, "rollback_pending");
        if (rollback_pending) {
            info->rollback_pending = cJSON_IsTrue(rollback_pending) ||
                                     (cJSON_IsNumber(rollback_pending) &&
                                      rollback_pending->valueint != 0);
        }
        info->crash_count = (uint32_t)json_get_double(root, "crash_count", (double)info->crash_count);
        info->radio_restart_count = (uint32_t)json_get_double(root, "radio_restart_count",
                                                              (double)info->radio_restart_count);
        info->received = true;
#ifdef FOF_BADGE_VARIANT
        badge_ingest_scanner_status_evidence(scanner_id, info);
#endif
        ESP_LOGI(TAG, "Scanner[%d] identity: %s v%s (%s) chip=%s toff=%lld tcnt=%u valid=%u state=%s mode=%s",
                 scanner_id, board, ver, caps, chip,
                 (long long)info->toff_ms, info->tcnt,
                 info->time_valid_count, info->time_sync_state,
                 info->scan_mode[0] ? info->scan_mode : "normal");
    } else if (strcmp(msg_type, "recovery_ack") == 0 ||
               strcmp(msg_type, "scanner_recovery") == 0) {
        scanner_info_t *info = (scanner_id == 0) ? &s_ble_scanner_info : &s_wifi_scanner_info;
        const char *mode = json_get_string(root, "recovery_mode", NULL);
        if (!mode) {
            mode = json_get_string(root, "mode", NULL);
        }
        if (mode && mode[0]) {
            strncpy(info->recovery_mode, mode, sizeof(info->recovery_mode) - 1);
            info->recovery_mode[sizeof(info->recovery_mode) - 1] = '\0';
        }
        json_copy_string(root, "safe_reason", info->safe_reason, sizeof(info->safe_reason));
        info->crash_count = (uint32_t)json_get_double(root, "crash_count", (double)info->crash_count);
        ESP_LOGW(TAG, "Scanner[%d] recovery: type=%s mode=%s reason=%s crash=%lu",
                 scanner_id,
                 msg_type,
                 info->recovery_mode[0] ? info->recovery_mode : "?",
                 info->safe_reason,
                 (unsigned long)info->crash_count);
    } else if (strcmp(msg_type, MSG_TYPE_FW_CHECK) == 0) {
        const char *board = json_get_string(root, "board", "");
        const char *ver = json_get_string(root, "ver", "");
        scanner_info_t *info = (scanner_id == 0) ? &s_ble_scanner_info : &s_wifi_scanner_info;
        info->fw_check_count = (uint32_t)json_get_double(root, "fw_check_count", (double)info->fw_check_count);
        const char *fw_state = json_get_string(root, JSON_KEY_FW_STATE, info->fw_update_state);
        strncpy(info->fw_update_state, fw_state ? fw_state : "", sizeof(info->fw_update_state) - 1);
        info->fw_update_state[sizeof(info->fw_update_state) - 1] = '\0';
        const char *last_fw_error = json_get_string(root, "last_fw_error", info->last_fw_error);
        strncpy(info->last_fw_error, last_fw_error ? last_fw_error : "", sizeof(info->last_fw_error) - 1);
        info->last_fw_error[sizeof(info->last_fw_error) - 1] = '\0';
        json_copy_string(root, "ota_state", info->ota_state, sizeof(info->ota_state));
        json_copy_string(root, "recovery_mode", info->recovery_mode, sizeof(info->recovery_mode));
        const cJSON *rollback_pending = cJSON_GetObjectItemCaseSensitive(root, "rollback_pending");
        if (rollback_pending) {
            info->rollback_pending = cJSON_IsTrue(rollback_pending) ||
                                     (cJSON_IsNumber(rollback_pending) &&
                                      rollback_pending->valueint != 0);
        }
        info->crash_count = (uint32_t)json_get_double(root, "crash_count", (double)info->crash_count);
        ESP_LOGI(TAG, "Scanner[%d] firmware check: board=%s ver=%s",
                 scanner_id,
                 board[0] ? board : "?",
                 ver[0] ? ver : "?");
        fw_store_handle_scanner_check(scanner_id, board, ver);
    } else if (strcmp(msg_type, MSG_TYPE_FW_READY) == 0) {
        const char *board = json_get_string(root, "board", "");
        const char *ver = json_get_string(root, "ver", "");
        const char *target = json_get_string(root, JSON_KEY_FW_TARGET_VERSION, "");
        scanner_info_t *info = (scanner_id == 0) ? &s_ble_scanner_info : &s_wifi_scanner_info;
        info->need_firmware = true;
        strncpy(info->fw_update_state, "ready", sizeof(info->fw_update_state) - 1);
        info->fw_update_state[sizeof(info->fw_update_state) - 1] = '\0';
        strncpy(info->fw_target_version, target, sizeof(info->fw_target_version) - 1);
        info->fw_target_version[sizeof(info->fw_target_version) - 1] = '\0';
        ESP_LOGW(TAG, "Scanner[%d] firmware ready: board=%s current=%s target=%s",
                 scanner_id,
                 board[0] ? board : "?",
                 ver[0] ? ver : "?",
                 target[0] ? target : "?");
        fw_store_handle_scanner_ready(scanner_id, board, ver);
    } else if (strcmp(msg_type, MSG_TYPE_CAL_MODE_ACK) == 0) {
        scanner_info_t *info = (scanner_id == 0) ? &s_ble_scanner_info : &s_wifi_scanner_info;
        const char *scan_mode = json_get_string(root, JSON_KEY_SCAN_MODE, "normal");
        const char *cal_uuid = json_get_string(root, JSON_KEY_CALIBRATION_UUID, "");
        const cJSON *ok_item = cJSON_GetObjectItemCaseSensitive(root, "ok");
        bool ok = (ok_item && cJSON_IsTrue(ok_item)) ||
                  (ok_item && cJSON_IsNumber(ok_item) && ok_item->valueint != 0);
        strncpy(info->scan_mode, scan_mode, sizeof(info->scan_mode) - 1);
        info->scan_mode[sizeof(info->scan_mode) - 1] = '\0';
        strncpy(info->calibration_uuid, cal_uuid, sizeof(info->calibration_uuid) - 1);
        info->calibration_uuid[sizeof(info->calibration_uuid) - 1] = '\0';
        info->calibration_mode_acked = ok;
        info->received = true;
        ESP_LOGW(TAG, "Scanner[%d] calibration ack: ok=%d mode=%s uuid=%s",
                 scanner_id,
                 ok ? 1 : 0,
                 info->scan_mode[0] ? info->scan_mode : "normal",
                 info->calibration_uuid[0] ? info->calibration_uuid : "-");
    } else if (strcmp(msg_type, "scan_profile_ack") == 0) {
        scanner_info_t *info = (scanner_id == 0) ? &s_ble_scanner_info : &s_wifi_scanner_info;
        const char *scan_profile = json_get_string(root, JSON_KEY_SCAN_PROFILE, "");
        strncpy(info->scan_profile, scan_profile, sizeof(info->scan_profile) - 1);
        info->scan_profile[sizeof(info->scan_profile) - 1] = '\0';
        info->cmd_rx_count++;
        info->cmd_last_age_s = 0;
        info->received = true;
        ESP_LOGI(TAG, "Scanner[%d] scan profile ack: %s",
                 scanner_id, info->scan_profile[0] ? info->scan_profile : "?");
    } else if (strcmp(msg_type, "display_control_ack") == 0) {
        const char *view = json_get_string(root, "view", "?");
        const cJSON *button_j = cJSON_GetObjectItemCaseSensitive(root, "button_enabled");
        const cJSON *page_lock_j = cJSON_GetObjectItemCaseSensitive(root, "page_lock");
        const cJSON *page_j = cJSON_GetObjectItemCaseSensitive(root, "page");
        bool button_enabled = (button_j && cJSON_IsTrue(button_j)) ||
                              (button_j && cJSON_IsNumber(button_j) && button_j->valueint != 0);
        bool page_lock = (page_lock_j && cJSON_IsTrue(page_lock_j)) ||
                         (page_lock_j && cJSON_IsNumber(page_lock_j) && page_lock_j->valueint != 0);
        ESP_LOGI(TAG, "Scanner[%d] display ack: button=%d view=%s page_lock=%d page=%d",
                 scanner_id,
                 button_enabled ? 1 : 0,
                 view,
                 page_lock ? 1 : 0,
                 (page_j && cJSON_IsNumber(page_j)) ? page_j->valueint : -1);
    } else if (strcmp(msg_type, "display_policy_ack") == 0) {
        scanner_info_t *info = (scanner_id == 0) ? &s_ble_scanner_info : &s_wifi_scanner_info;
        info->display_policy_ack_hash = (uint32_t)json_get_double(
            root, "hash", (double)info->display_policy_ack_hash
        );
        info->cmd_rx_count++;
        info->cmd_last_age_s = 0;
        info->received = true;
        ESP_LOGI(TAG, "Scanner[%d] display policy ack hash=%lu",
                 scanner_id, (unsigned long)info->display_policy_ack_hash);
    } else if (strncmp(msg_type, "ota_", 4) == 0 ||
               strcmp(msg_type, "stop_ack") == 0) {
        /* OTA/control response from scanner — capture for relay diagnostics. */
        portENTER_CRITICAL(&s_ota_response_lock);
        strncpy((char *)s_last_ota_response.type, msg_type, sizeof(s_last_ota_response.type) - 1);
        const char *err = json_get_string(root, "error", "");
        strncpy((char *)s_last_ota_response.error, err, sizeof(s_last_ota_response.error) - 1);
        cJSON *rcv = cJSON_GetObjectItem(root, "received");
        s_last_ota_response.received = rcv ? (uint32_t)rcv->valuedouble : 0;
        cJSON *seq_j = cJSON_GetObjectItem(root, "seq");
        s_last_ota_response.seq = seq_j ? seq_j->valueint : -1;
        s_last_ota_response.timestamp = esp_timer_get_time() / 1000;
        portEXIT_CRITICAL(&s_ota_response_lock);
    } else {
        ESP_LOGW(TAG, "Scanner[%d] msg type='%s' (unhandled)", scanner_id, msg_type);
    }

    cJSON_Delete(root);
}

/* ── UART RX task (parameterized for dual-scanner support) ─────────────── */

typedef struct {
    int uart_num;
    int scanner_id;     /* 0=BLE scanner, 1=WiFi scanner */
    const char *label;
    char *line_buf;
    uint8_t *read_buf;
} uart_rx_task_params_t;

static void *uart_rx_alloc_persistent_buffer(size_t size, const char *label)
{
    void *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate %s (%lu bytes)",
                 label ? label : "UART RX buffer", (unsigned long)size);
    }
    return buf;
}

static bool uart_rx_ensure_task_buffers(uart_rx_task_params_t *params)
{
    if (!params) {
        return false;
    }
    if (!params->line_buf) {
        params->line_buf = (char *)uart_rx_alloc_persistent_buffer(
            LINE_BUF_SIZE,
            params->label ? params->label : "UART line buffer"
        );
    }
    if (!params->read_buf) {
        params->read_buf = (uint8_t *)uart_rx_alloc_persistent_buffer(
            READ_BUF_SIZE,
            params->label ? params->label : "UART read buffer"
        );
    }
    return params->line_buf && params->read_buf;
}

#ifdef FOF_BADGE_VARIANT
static void note_uart_rx_stack_free(int scanner_id)
{
    badge_runtime_note_uart_stack_free((uint8_t)scanner_id,
        (uint32_t)uxTaskGetStackHighWaterMark(NULL));
}
#else
static void note_uart_rx_stack_free(int scanner_id)
{
    (void)scanner_id;
}
#endif

static void uart_rx_task(void *arg)
{
    uart_rx_task_params_t *params = (uart_rx_task_params_t *)arg;
    int uart_num = params->uart_num;
    int scanner_id = params->scanner_id;

    char *line_buf = params->line_buf;
    uint8_t *read_buf = params->read_buf;
    int  line_pos = 0;

    if (!line_buf || !read_buf) {
        ESP_LOGE(TAG, "UART RX task missing buffers: %s", params->label);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "UART RX task started: %s (UART%d)", params->label, uart_num);

    int debug_dumps = 3;  /* dump first 3 reads for diagnostics */
    int64_t last_heartbeat_ms = esp_timer_get_time() / 1000;
    int total_bytes = 0;

    while (1) {
        /* Periodic heartbeat so we know the task is alive */
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - last_heartbeat_ms > 5000) {
            note_uart_rx_stack_free(scanner_id);
            ESP_LOGI(TAG, "[%s] heartbeat: %d total bytes received stack=%lu",
                     params->label, total_bytes,
                     (unsigned long)uxTaskGetStackHighWaterMark(NULL));
            last_heartbeat_ms = now_ms;
        }
        /* During OTA relay, pause reading so relay handler can read ACKs directly */
        volatile bool *paused = (scanner_id == 0) ? &s_rx_paused_ble : &s_rx_paused_wifi;
        if (*paused) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int bytes_read = uart_read_bytes(uart_num, read_buf, READ_BUF_SIZE,
                                         pdMS_TO_TICKS(100));
        if (bytes_read <= 0) {
            continue;
        }
        note_uart_rx_stack_free(scanner_id);
        total_bytes += bytes_read;
        note_scanner_raw_activity(scanner_id, now_ms, bytes_read);

        if (debug_dumps > 0) {
            debug_dumps--;
            char hex[128];
            int hlen = 0;
            for (int j = 0; j < bytes_read && hlen < 120; j++) {
                hlen += snprintf(hex + hlen, sizeof(hex) - hlen, "%02X ", read_buf[j]);
            }
            ESP_LOGI(TAG, "[%s] RX %d bytes: %s", params->label, bytes_read, hex);
        }

        for (int i = 0; i < bytes_read; i++) {
            char c = (char)read_buf[i];

            if (c == UART_MSG_DELIMITER) {
                if (line_pos > 0) {
                    line_buf[line_pos] = '\0';
                    process_line(line_buf, line_pos, scanner_id);
                    line_pos = 0;
                }
            } else {
                if (line_pos < LINE_BUF_SIZE - 1) {
                    line_buf[line_pos++] = c;
                } else {
                    /* Dump first 20 bytes for diagnostics */
                    char hex[64];
                    int hlen = 0;
                    for (int j = 0; j < 20 && j < LINE_BUF_SIZE; j++) {
                        hlen += snprintf(hex + hlen, sizeof(hex) - hlen, "%02X ", (uint8_t)line_buf[j]);
                    }
                    ESP_LOGW(TAG, "[%s] Line buffer overflow (%d bytes, no newline). First 20: %s",
                             params->label, line_pos, hex);
                    note_scanner_line_overflow(scanner_id);
                    line_pos = 0;
                }
            }
        }
    }
}

/* Static params (must outlive the tasks) */
static uart_rx_task_params_t s_ble_task_params = { .uart_num = 0, .scanner_id = 0, .label = "BLE" };
#if CONFIG_DUAL_SCANNER
static uart_rx_task_params_t s_wifi_task_params = { .uart_num = 0, .scanner_id = 1, .label = "WiFi" };
#endif

/* ── Public API ────────────────────────────────────────────────────────── */

static void init_uart_port(int uart_num, int rx_pin, int tx_pin, const char *label)
{
    const uart_config_t uart_config = {
        .baud_rate  = UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(uart_num, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(uart_num, tx_pin, rx_pin,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(uart_num, UART_BUF_SIZE * 2,
                                        UART_BUF_SIZE * 2, 0, NULL, 0));

    ESP_LOGI(TAG, "%s scanner UART%d: %d baud, RX=GPIO%d, TX=GPIO%d",
             label, uart_num, UART_BAUD_RATE, rx_pin, tx_pin);
}

void uart_rx_init(QueueHandle_t detection_queue)
{
    s_detection_queue = detection_queue;
    if (!s_uart_tx_lock) {
        s_uart_tx_lock = xSemaphoreCreateMutex();
        if (!s_uart_tx_lock) {
            ESP_LOGE(TAG, "Failed to create scanner UART TX lock");
        }
    }
#ifdef FOF_BADGE_VARIANT
    if (badge_threat_lock_state(pdMS_TO_TICKS(100))) {
        badge_threat_ensure_ready();
        badge_threat_unlock_state();
    }
#endif

    /* BLE scanner on UART1 (always) */
    s_ble_task_params.uart_num = CONFIG_BLE_SCANNER_UART;
    (void)uart_rx_ensure_task_buffers(&s_ble_task_params);
    init_uart_port(CONFIG_BLE_SCANNER_UART,
                   CONFIG_BLE_SCANNER_RX_PIN, CONFIG_BLE_SCANNER_TX_PIN, "BLE");

#if CONFIG_DUAL_SCANNER
    /* WiFi scanner on UART2 */
    s_wifi_task_params.uart_num = CONFIG_WIFI_SCANNER_UART;
    (void)uart_rx_ensure_task_buffers(&s_wifi_task_params);
    init_uart_port(CONFIG_WIFI_SCANNER_UART,
                   CONFIG_WIFI_SCANNER_RX_PIN, CONFIG_WIFI_SCANNER_TX_PIN, "WiFi");
#endif
}

void uart_rx_start(void)
{
    xTaskCreate(uart_rx_task, "uart_rx_ble", CONFIG_UART_RX_STACK,
                &s_ble_task_params, CONFIG_UART_RX_PRIORITY, NULL);
    ESP_LOGI(TAG, "BLE scanner RX task created");

#if CONFIG_DUAL_SCANNER
    xTaskCreate(uart_rx_task, "uart_rx_wifi", CONFIG_UART_RX_STACK,
                &s_wifi_task_params, CONFIG_UART_RX_PRIORITY, NULL);
    ESP_LOGI(TAG, "WiFi scanner RX task created");
#endif
}

int uart_rx_get_detection_count(void)
{
    return s_detection_count;
}

int uart_rx_get_recent_detections(detection_summary_t *out, int max)
{
    if (!out || max <= 0) {
        return 0;
    }

    int copied = 0;
    portENTER_CRITICAL(&s_recent_lock);
    int count = s_recent_count < max ? s_recent_count : max;
    /* Copy newest first */
    for (int i = 0; i < count; i++) {
        int idx = (s_recent_head - 1 - i + RECENT_RING_SIZE) % RECENT_RING_SIZE;
        out[copied++] = s_recent_ring[idx];
    }
    portEXIT_CRITICAL(&s_recent_lock);
    return copied;
}

static bool _check_connected(atomic_int_fast64_t *ts)
{
    int_fast64_t last = atomic_load(ts);
    if (last == 0) return false;
    int_fast64_t now_ms = (int_fast64_t)(esp_timer_get_time() / 1000);
    return (now_ms - last) < SCANNER_TIMEOUT_MS;
}

bool uart_rx_is_scanner_connected(void)
{
    return _check_connected(&s_last_rx_time_ble)
#if CONFIG_DUAL_SCANNER
        || _check_connected(&s_last_rx_time_wifi)
#endif
        ;
}

bool uart_rx_is_ble_scanner_connected(void)
{
    return _check_connected(&s_last_rx_time_ble);
}

bool uart_rx_is_wifi_scanner_connected(void)
{
#if CONFIG_DUAL_SCANNER
    return _check_connected(&s_last_rx_time_wifi);
#else
    return false;
#endif
}

void uart_rx_get_scanner_uart_diag(int scanner_id, scanner_uart_diag_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    atomic_int_fast64_t *raw_ts = scanner_id == 0
        ? &s_last_raw_rx_time_ble
        : &s_last_raw_rx_time_wifi;
    atomic_uint_fast32_t *raw_bytes = scanner_id == 0
        ? &s_raw_rx_bytes_ble
        : &s_raw_rx_bytes_wifi;
    atomic_uint_fast32_t *line_overflow = scanner_id == 0
        ? &s_line_overflow_ble
        : &s_line_overflow_wifi;
    atomic_uint_fast32_t *json_error = scanner_id == 0
        ? &s_json_parse_error_ble
        : &s_json_parse_error_wifi;
    int_fast64_t last = atomic_load(raw_ts);
    int_fast64_t now_ms = (int_fast64_t)(esp_timer_get_time() / 1000);
    out->raw_seen = last > 0 && (now_ms - last) < SCANNER_TIMEOUT_MS;
    out->raw_age_s = last > 0 ? (now_ms - last) / 1000 : -1;
    out->raw_bytes = (uint32_t)atomic_load(raw_bytes);
    out->line_overflow_count = (uint32_t)atomic_load(line_overflow);
    out->json_parse_error_count = (uint32_t)atomic_load(json_error);
}

/* Route a command to only one UART by scanning the JSON for markers.
 *
 * Scanner delegation rule (default operating mode):
 *   - BLE-specific commands (ble_lockon / ble_lockon_cancel) → BLE UART only.
 *     While the BLE scanner focuses on an RID drone, the WiFi scanner keeps
 *     doing wide passive scanning for other detections.
 *   - WiFi-specific commands (lockon / lockon_cancel, which carry a "ch"
 *     field) → WiFi UART only. Symmetric.
 *   - Everything else (start/stop/ready/ota_*) still broadcasts to both.
 *
 * The backend also passes "type":"ble" / "type":"wifi" hints in per-node
 * lockon commands (see drone_tracker._issue_lockon); we honor those too as
 * a primary signal, falling back to the command-type string.
 */
static bool cmd_is_ble_only(const char *json_cmd)
{
    if (!json_cmd) return false;
    if (strstr(json_cmd, "\"type\":\"ble\"")) return true;           /* backend hint */
    if (strstr(json_cmd, "\"type\":\"ble_lockon\"")) return true;
    if (strstr(json_cmd, "\"type\":\"ble_lockon_cancel\"")) return true;
    return false;
}

static bool cmd_is_wifi_only(const char *json_cmd)
{
    if (!json_cmd) return false;
    if (strstr(json_cmd, "\"type\":\"wifi\"")) return true;          /* backend hint */
    /* Plain "lockon" / "lockon_cancel" commands are WiFi channel locks. */
    if (strstr(json_cmd, "\"type\":\"lockon\"")) return true;
    if (strstr(json_cmd, "\"type\":\"lockon_cancel\"")) return true;
    return false;
}

void uart_rx_send_command(const char *json_cmd)
{
    if (!json_cmd) return;
    if (s_node_calibration_mode &&
        (strstr(json_cmd, "\"type\":\"lockon\"") ||
         strstr(json_cmd, "\"type\":\"lockon_cancel\"") ||
         strstr(json_cmd, "\"type\":\"ble_lockon\"") ||
         strstr(json_cmd, "\"type\":\"ble_lockon_cancel\""))) {
        ESP_LOGW(TAG, "Rejecting scan-control command while calibration mode is active");
        return;
    }
    const bool ble_only  = cmd_is_ble_only(json_cmd);
#if CONFIG_DUAL_SCANNER
    const bool wifi_only = cmd_is_wifi_only(json_cmd);
#else
    const bool wifi_only = false;  /* single-scanner build: always the BLE UART */
#endif

    /* BLE scanner: gets the command unless it's WiFi-specific. */
    bool ble_ok = true;
    if (!wifi_only) {
        ble_ok = send_json_line_to_scanner_locked(0, json_cmd);
    }

#if CONFIG_DUAL_SCANNER
    /* WiFi scanner: gets the command unless it's BLE-specific. */
    bool wifi_ok = true;
    if (!ble_only) {
        wifi_ok = send_json_line_to_scanner_locked(1, json_cmd);
    }
#else
    bool wifi_ok = true;
#endif

    ESP_LOGI("uart_tx_cmd", "cmd -> %s (%d bytes) ok=%d/%d",
             ble_only  ? "BLE only"  :
             wifi_only ? "WiFi only" : "broadcast",
             (int)strlen(json_cmd),
             ble_ok ? 1 : 0,
             wifi_ok ? 1 : 0);
}

bool uart_rx_send_command_to_scanner_checked(int scanner_id, const char *json_cmd)
{
    if (!json_cmd) {
        return false;
    }
    if (s_node_calibration_mode &&
        (strstr(json_cmd, "\"type\":\"lockon\"") ||
         strstr(json_cmd, "\"type\":\"lockon_cancel\"") ||
         strstr(json_cmd, "\"type\":\"ble_lockon\"") ||
         strstr(json_cmd, "\"type\":\"ble_lockon_cancel\""))) {
        ESP_LOGW(TAG, "Rejecting scan-control command while calibration mode is active");
        return false;
    }

    return send_json_line_to_scanner_locked(scanner_id, json_cmd);
}

void uart_rx_send_command_to_scanner(int scanner_id, const char *json_cmd)
{
    (void)uart_rx_send_command_to_scanner_checked(scanner_id, json_cmd);
}

const scanner_info_t *uart_rx_get_ble_scanner_info(void)
{
    return s_ble_scanner_info.received ? &s_ble_scanner_info : NULL;
}

const scanner_info_t *uart_rx_get_wifi_scanner_info(void)
{
    return s_wifi_scanner_info.received ? &s_wifi_scanner_info : NULL;
}

ota_response_t uart_rx_get_last_ota_response(void)
{
    ota_response_t copy;
    portENTER_CRITICAL(&s_ota_response_lock);
    copy = *(const ota_response_t *)&s_last_ota_response;
    portEXIT_CRITICAL(&s_ota_response_lock);
    return copy;
}

void uart_rx_clear_ota_response(void)
{
    portENTER_CRITICAL(&s_ota_response_lock);
    memset((void *)&s_last_ota_response, 0, sizeof(s_last_ota_response));
    portEXIT_CRITICAL(&s_ota_response_lock);
}

void uart_rx_pause_scanner(int scanner_id)
{
    if (scanner_id == 0) s_rx_paused_ble = true;
    else s_rx_paused_wifi = true;
    ESP_LOGW(TAG, "UART RX paused for scanner %d (OTA relay)", scanner_id);
}

void uart_rx_resume_scanner(int scanner_id)
{
    if (scanner_id == 0) s_rx_paused_ble = false;
    else s_rx_paused_wifi = false;
    ESP_LOGW(TAG, "UART RX resumed for scanner %d", scanner_id);
}
