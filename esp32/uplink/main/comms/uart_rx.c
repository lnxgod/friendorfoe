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
#include "badge_ble_rssi_policy.h"
#include "badge_threat_policy.h"
#include "scanner_command_schema_registry.h"
#include "scanner_uart_line_framer.h"
#include "scanner_uplink_ingress_registry.h"
#include "led_status.h"
#include "fw_store.h"
#if defined(FOF_DC34_GAME_CANARY)
#include "badge_con_protocol.h"
#include "badge_con_runtime.h"
#endif
#ifdef FOF_BADGE_VARIANT
#include "badge_runtime.h"
#include "badge_power_runtime.h"
#include "badge_ble_investigation.h"
#include "badge_easter_egg_runtime.h"
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

#define LINE_BUF_SIZE       SCANNER_UART_LINE_BUFFER_SIZE
#define READ_BUF_SIZE       256
#define UART_RX_PARTIAL_FRAME_TIMEOUT_MS 1000
#define UART_RX_PAUSE_ACK_TIMEOUT_MS 1000
#define UART_RX_PAUSE_POLL_MS          5
#define UART_RX_ENTRY_ACK_TIMEOUT_MS 1000

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
static StaticSemaphore_t s_scanner_info_mutex_storage;
static SemaphoreHandle_t s_scanner_info_mutex = NULL;
static scanner_identity_snapshot_t s_scanner_identity_snapshots[2] = {0};
static StaticSemaphore_t s_scanner_identity_mutex_storage;
static SemaphoreHandle_t s_scanner_identity_mutex = NULL;

static scanner_info_t *scanner_info_for_slot_unlocked(int scanner_id)
{
    if (scanner_id == 0) {
        return &s_ble_scanner_info;
    }
    if (scanner_id == 1) {
        return &s_wifi_scanner_info;
    }
    return NULL;
}

static scanner_info_t *scanner_info_update_begin(int scanner_id)
{
    scanner_info_t *published = scanner_info_for_slot_unlocked(scanner_id);
    if (!published || !s_scanner_info_mutex ||
        xSemaphoreTake(s_scanner_info_mutex, portMAX_DELAY) != pdTRUE) {
        return NULL;
    }
    return published;
}

static void scanner_info_update_commit(void)
{
    xSemaphoreGive(s_scanner_info_mutex);
}

/* OTA response tracking (set by UART RX, read by relay handler) */
static volatile ota_response_t s_last_ota_response = {0};
static portMUX_TYPE s_ota_response_lock = portMUX_INITIALIZER_UNLOCKED;

/* A relay reads ACKs directly from the UART.  The request flag alone is not a
 * handoff: the background task may still be blocked inside uart_read_bytes()
 * and consume the first ACK.  Each task therefore publishes an explicit
 * pause acknowledgement before the relay is allowed to touch the UART. */
static atomic_uint_fast32_t s_rx_pause_request_generation_ble = 0;
static atomic_uint_fast32_t s_rx_pause_request_generation_wifi = 0;
static atomic_uint_fast32_t s_rx_pause_ack_generation_ble = 0;
static atomic_uint_fast32_t s_rx_pause_ack_generation_wifi = 0;
static atomic_uint_fast32_t s_rx_pause_next_generation = 0;
static atomic_bool s_rx_task_started_ble = false;
static atomic_bool s_rx_task_started_wifi = false;
static atomic_bool s_rx_task_entered_ble = false;
static atomic_bool s_rx_task_entered_wifi = false;
static uart_rx_pause_guard_t s_legacy_pause_guards[2] = {0};

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
/* Per-slot evidence copies let status publication release its mutex before
 * threat ingestion, which can legitimately wait on the separate threat lock. */
static scanner_info_t s_badge_status_evidence[2] = {0};

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
    if (strcmp(info->ble_meta_identity, "strong_fp") == 0) {
        return strcmp(info->ble_meta_last_reason, "uuid16:0xFD5F") == 0 ||
               strcmp(info->ble_meta_last_reason, "mfr_cid:0x0D53") == 0 ||
               strcmp(info->ble_meta_last_reason, "name:meta_glasses") == 0;
    }
    if (strcmp(info->ble_meta_identity, "detector_fp") != 0) {
        return false;
    }
    static const char *const detector_reasons[] = {
        "uuid16:0xFD5F",
        "mfr_cid:0x0D53",
        "name:RB Meta",
        "name:Ray-Ban Meta",
        "name:Ray-Ban Stories",
        "name:Ray Ban Meta",
        "name:RayBan Meta",
        "name:Oakley Meta",
        "name:Oakley HSTN",
        "name:Oakley ALPH",
        "name:OAK",
        "name:RB-",
        "name:RAYBAN",
        "name:Wayfarer",
    };
    for (size_t i = 0; i < sizeof(detector_reasons) / sizeof(detector_reasons[0]); ++i) {
        if (strcmp(info->ble_meta_last_reason, detector_reasons[i]) == 0) {
            return true;
        }
    }
    return false;
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
    if (lock &&
        xSemaphoreTakeRecursive(lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "badge tx-pin probe lock timeout: scanner=%d tx=%d",
                 scanner_id, tx_pin);
        return false;
    }

    uart_wait_tx_done(uart, pdMS_TO_TICKS(150));
    esp_err_t err = uart_set_pin(uart, tx_pin, rx_pin,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    if (lock) {
        xSemaphoreGiveRecursive(lock);
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
    if (lock &&
        xSemaphoreTakeRecursive(lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "scanner command lock timeout: scanner=%d", scanner_id);
        return false;
    }

    uart_port_t uart = scanner_uart_for_id(scanner_id);
    int written = uart_write_bytes(uart, line, len);
    if (written == (int)len) {
        uart_wait_tx_done(uart, pdMS_TO_TICKS(150));
    }

    if (lock) {
        xSemaphoreGiveRecursive(lock);
    }

    if (written != (int)len) {
        ESP_LOGW(TAG, "scanner command short write: scanner=%d uart=%d wrote=%d/%u",
                 scanner_id, uart, written, (unsigned)len);
        return false;
    }
    return true;
}

static bool send_scanner_flow_cmd(int scanner_id, const char *type)
{
    char cmd[24];
    int n = snprintf(cmd, sizeof(cmd), "{\"type\":\"%s\"}", type);
    if (n <= 0 || (size_t)n >= sizeof(cmd)) {
        return false;
    }
    return uart_rx_send_command_to_scanner_checked(scanner_id, cmd);
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
#ifdef FOF_BADGE_VARIANT
    if (badge_power_runtime_is_quiet()) {
        return;
    }
#endif
    _Atomic bool *flag = backpressure_flag_for_scanner(scanner_id);
    if (!atomic_load(flag) || s_detection_queue == NULL) {
        return;
    }

    UBaseType_t queue_count = uxQueueMessagesWaiting(s_detection_queue);
    if (queue_count <= (CONFIG_DETECTION_QUEUE_SIZE * 4 / 10)) {
        if (send_scanner_flow_cmd(scanner_id, "start")) {
            ESP_LOGI(TAG, "Queue drained %d/%d — resuming scanner[%d]",
                     (int)queue_count, CONFIG_DETECTION_QUEUE_SIZE,
                     scanner_id);
            atomic_store(flag, false);
        }
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

    for (int scanner_id = 0; scanner_id < 2; scanner_id++) {
        scanner_info_t *info = scanner_info_update_begin(scanner_id);
        if (info) {
            info->calibration_mode_acked = false;
            scanner_info_update_commit();
        }
    }
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

static bool json_get_uint32_exact(const cJSON *obj, const char *key,
                                  uint32_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!out || !cJSON_IsNumber(item) || item->valuedouble < 0.0 ||
        item->valuedouble > (double)UINT32_MAX) {
        return false;
    }
    uint32_t value = (uint32_t)item->valuedouble;
    if ((double)value != item->valuedouble) {
        return false;
    }
    *out = value;
    return true;
}

static int json_get_int(const cJSON *obj, const char *key, int def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return def;
}

static bool json_get_bool(const cJSON *obj, const char *key, bool def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    if (cJSON_IsNumber(item)) {
        return item->valueint != 0;
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

static bool json_get_nonempty_string_exact(const cJSON *obj, const char *key,
                                           const char **out)
{
    const cJSON *item = obj && key
        ? cJSON_GetObjectItemCaseSensitive(obj, key)
        : NULL;
    if (!out || !cJSON_IsString(item) || !item->valuestring ||
        !item->valuestring[0]) {
        return false;
    }
    *out = item->valuestring;
    return true;
}

static bool fw_ready_common_fields_valid(const cJSON *root,
                                         const char **board,
                                         const char **current_version,
                                         const char **target_version,
                                         uint32_t *target_size,
                                         uint32_t *target_crc32)
{
    return json_get_nonempty_string_exact(root, "board", board) &&
        json_get_nonempty_string_exact(root, "ver", current_version) &&
        json_get_nonempty_string_exact(root, JSON_KEY_FW_TARGET_VERSION,
                                       target_version) &&
        json_get_uint32_exact(root, JSON_KEY_FW_SIZE, target_size) &&
        json_get_uint32_exact(root, JSON_KEY_FW_CRC32, target_crc32);
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

static bool scanner_identity_frame_string(const cJSON *root,
                                          const char *key,
                                          const char **out,
                                          size_t out_capacity)
{
    if (!out || out_capacity < 2U) {
        return false;
    }
    *out = "";
    const cJSON *item = root && key
        ? cJSON_GetObjectItemCaseSensitive(root, key)
        : NULL;
    if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0] ||
        strlen(item->valuestring) >= out_capacity) {
        return false;
    }
    *out = item->valuestring;
    return true;
}

static bool scanner_identity_hardware_id_is_canonical(const char *hardware_id)
{
    if (!hardware_id || strlen(hardware_id) != 17U) {
        return false;
    }
    for (size_t i = 0; i < 17U; ++i) {
        if ((i + 1U) % 3U == 0U) {
            if (hardware_id[i] != ':') {
                return false;
            }
            continue;
        }
        char ch = hardware_id[i];
        if (!((ch >= '0' && ch <= '9') ||
              (ch >= 'a' && ch <= 'f') ||
              (ch >= 'A' && ch <= 'F'))) {
            return false;
        }
    }
    return true;
}

static uint32_t publish_scanner_identity_snapshot(int scanner_id,
                                                  const cJSON *root,
                                                  int64_t received_ms,
                                                  bool require_complete)
{
    if (scanner_id < 0 || scanner_id > 1 || !root ||
        !s_scanner_identity_mutex) {
        return 0;
    }

    scanner_identity_snapshot_t snapshot = {0};
    const char *version = "";
    const char *board = "";
    const char *firmware_name = "";
    const char *app_project = "";
    const char *hardware_type = "";
    const char *hardware_id = "";
    bool complete =
        scanner_identity_frame_string(root, "ver", &version,
                                      sizeof(snapshot.version)) &&
        scanner_identity_frame_string(root, "board", &board,
                                      sizeof(snapshot.board)) &&
        scanner_identity_frame_string(root, "firmware_name",
                                      &firmware_name,
                                      sizeof(snapshot.firmware_name)) &&
        scanner_identity_frame_string(root, "app_project", &app_project,
                                      sizeof(snapshot.app_project)) &&
        scanner_identity_frame_string(root, "hardware_type",
                                      &hardware_type,
                                      sizeof(snapshot.hardware_type)) &&
        scanner_identity_frame_string(root, "hardware_id", &hardware_id,
                                      sizeof(snapshot.hardware_id)) &&
        scanner_identity_hardware_id_is_canonical(hardware_id);

    snprintf(snapshot.version, sizeof(snapshot.version), "%s", version);
    snprintf(snapshot.board, sizeof(snapshot.board), "%s", board);
    snprintf(snapshot.firmware_name, sizeof(snapshot.firmware_name), "%s",
             firmware_name);
    snprintf(snapshot.app_project, sizeof(snapshot.app_project), "%s",
             app_project);
    snprintf(snapshot.hardware_type, sizeof(snapshot.hardware_type), "%s",
             hardware_type);
    snprintf(snapshot.hardware_id, sizeof(snapshot.hardware_id), "%s",
             hardware_id);
    uint32_t boot_id = 0;
    if (json_get_uint32_exact(root, "boot_id", &boot_id) && boot_id != 0U) {
        snapshot.boot_id = boot_id;
    }
    snapshot.received_ms = received_ms;
    snapshot.complete = complete;
    if (require_complete && !snapshot.complete) {
        return 0;
    }

    /* Publication is the freshness authority. Waiting for the tiny snapshot
     * copy is safer than timing out and leaving an older complete identity
     * eligible after a current incomplete frame. Readers never hold this
     * mutex across another lock or any I/O. */
    if (xSemaphoreTake(s_scanner_identity_mutex, portMAX_DELAY) != pdTRUE) {
        return 0;
    }
    uint32_t previous =
        s_scanner_identity_snapshots[scanner_id].identity_generation;
    snapshot.identity_generation = previous == UINT32_MAX ? 1U : previous + 1U;
    s_scanner_identity_snapshots[scanner_id] = snapshot;
    xSemaphoreGive(s_scanner_identity_mutex);
    return snapshot.identity_generation;
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

    /* Shared by the BLE and WiFi scanner UART tasks. Missing keys remain the
     * backward-compatible no-threat value after the struct is zeroed above. */
    det->ble_threat_kind = (uint8_t)json_get_int(
        root, JSON_KEY_BLE_THREAT_KIND, BLE_THREAT_KIND_NONE);
    det->ble_prompt_family_mask = (uint8_t)json_get_int(
        root, JSON_KEY_BLE_PROMPT_FAMILIES, 0);
    det->ble_unique_macs = (uint16_t)json_get_int(
        root, JSON_KEY_BLE_UNIQUE_MACS, 0);
    det->ble_observation_count = (uint16_t)json_get_int(
        root, JSON_KEY_BLE_OBSERVATIONS, 0);
    det->ble_serial_service_uuid = (uint16_t)json_get_int(
        root, JSON_KEY_BLE_SERIAL_UUID, 0);
    det->ble_threat_evidence_mask = (uint8_t)json_get_int(
        root, JSON_KEY_BLE_THREAT_EVIDENCE, 0);

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

static void handle_status(const cJSON *root, int scanner_id,
                          uint32_t published_identity_generation)
{
    int ble_count  = json_get_int(root, "ble_count", 0);
    int wifi_count = json_get_int(root, "wifi_count", 0);
    int channel    = json_get_int(root, "ch", 0);
    int uptime     = json_get_int(root, "uptime_s", 0);
    bool log_initial_identity = false;
    bool log_deauth_flood = false;
    bool log_beacon_spam = false;
    int log_deauth_count = 0;

    const char *status_profile = json_get_string(root, JSON_KEY_SCAN_PROFILE, "");
    ESP_LOGI(TAG, "Scanner[%d] status: BLE=%d WiFi=%d ch=%d uptime=%ds profile=%s",
             scanner_id, ble_count, wifi_count, channel, uptime,
             status_profile && status_profile[0] ? status_profile : "?");

    scanner_info_t *info = scanner_info_update_begin(scanner_id);
    if (!info) {
        ESP_LOGE(TAG, "Scanner[%d] status snapshot lock failed", scanner_id);
        return;
    }

    /* Ordinary status carries the same immutable identity tuple as
     * scanner_info. Keep the live view synchronized with the separately
     * published freshness snapshot, clearing missing fields fail-closed. */
    const char *ver = json_get_string(root, "ver", NULL);
    if (published_identity_generation != 0U) {
        uint32_t parsed_boot_id = 0;
        const char *board = json_get_string(root, "board", "");
        const char *firmware_name =
            json_get_string(root, "firmware_name", "");
        const char *app_project =
            json_get_string(root, "app_project", "");
        const char *hardware_type =
            json_get_string(root, "hardware_type", "");
        const char *hardware_id =
            json_get_string(root, "hardware_id", "");
        const char *chip = json_get_string(root, "chip", "");
        const char *caps = json_get_string(root, "caps", "");
        strncpy(info->version, ver ? ver : "",
                sizeof(info->version) - 1);
        strncpy(info->board, board, sizeof(info->board) - 1);
        strncpy(info->firmware_name, firmware_name,
                sizeof(info->firmware_name) - 1);
        strncpy(info->app_project, app_project,
                sizeof(info->app_project) - 1);
        strncpy(info->hardware_type, hardware_type,
                sizeof(info->hardware_type) - 1);
        strncpy(info->hardware_id, hardware_id,
                sizeof(info->hardware_id) - 1);
        strncpy(info->chip, chip, sizeof(info->chip) - 1);
        strncpy(info->caps, caps, sizeof(info->caps) - 1);
        info->version[sizeof(info->version) - 1] = '\0';
        info->board[sizeof(info->board) - 1] = '\0';
        info->firmware_name[sizeof(info->firmware_name) - 1] = '\0';
        info->app_project[sizeof(info->app_project) - 1] = '\0';
        info->hardware_type[sizeof(info->hardware_type) - 1] = '\0';
        info->hardware_id[sizeof(info->hardware_id) - 1] = '\0';
        info->chip[sizeof(info->chip) - 1] = '\0';
        info->caps[sizeof(info->caps) - 1] = '\0';
        info->identity_generation = published_identity_generation;
        info->boot_id = 0;
        if (json_get_uint32_exact(root, "boot_id", &parsed_boot_id) &&
            parsed_boot_id != 0U) {
            info->boot_id = parsed_boot_id;
        }
        if (ver && ver[0] && !info->received) {
            info->received = true;
            log_initial_identity = true;
        }
    }

    /* Attack / anomaly counters */
    {
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
        info->ble_initialized = json_get_bool(root, "ble_initialized", false);
        info->ble_scanning = json_get_bool(root, "ble_scanning", false);
        info->ble_host_active = json_get_bool(root, "ble_host_active", false);
        info->ble_host_synced = json_get_bool(root, "ble_host_synced", false);
        info->ble_quiesced = json_get_bool(root, "ble_quiesced", false);
        info->wifi_initialized = json_get_bool(root, "wifi_initialized", false);
        info->wifi_active = json_get_bool(root, "wifi_active", false);
        info->wifi_quiesced = json_get_bool(root, "wifi_quiesced", false);
        info->wifi_init_rc = json_get_int(root, "wifi_init_rc",
                                         info->wifi_init_rc);
        info->wifi_paused = json_get_bool(root, "wifi_paused", true);
        const char *slot_role = json_get_string(root, JSON_KEY_SLOT_ROLE, "");
        strncpy(info->slot_role, slot_role, sizeof(info->slot_role) - 1);
        info->slot_role[sizeof(info->slot_role) - 1] = '\0';
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

        log_deauth_flood = info->deauth_flood;
        log_deauth_count = info->deauth_count;
        log_beacon_spam = info->beacon_spam;
    }

#ifdef FOF_BADGE_VARIANT
    s_badge_status_evidence[scanner_id] = *info;
#endif
    scanner_info_update_commit();
    if (log_initial_identity) {
        const char *board = json_get_string(root, "board", "?");
        const char *caps = json_get_string(root, "caps", "?");
        ESP_LOGI(TAG, "Scanner[%d] identity from status: %s v%s (%s)",
                 scanner_id, board, ver ? ver : "?", caps);
    }
    if (log_deauth_flood) {
        ESP_LOGW(TAG, "Scanner[%d] DEAUTH FLOOD detected! deauth=%d",
                 scanner_id, log_deauth_count);
    }
    if (log_beacon_spam) {
        ESP_LOGW(TAG, "Scanner[%d] BEACON SPAM detected!", scanner_id);
    }
#ifdef FOF_BADGE_VARIANT
    const scanner_info_t *evidence = &s_badge_status_evidence[scanner_id];
    if (evidence->deauth_flood || evidence->deauth_count >= 3) {
        badge_ingest_wifi_status_event(
            evidence->deauth_flood ? "Deauth Flood" : "Deauth",
            "deauth",
            evidence->deauth_flood ? 0.90f : 0.70f,
            evidence->deauth_count > 0 ? evidence->deauth_count : 1
        );
    }
    if (evidence->disassoc_count >= 3) {
        badge_ingest_wifi_status_event("Disassoc", "disassoc", 0.65f,
                                       evidence->disassoc_count);
    }
    if (evidence->beacon_spam) {
        badge_ingest_wifi_status_event("Beacon Spam", "beacon spam", 0.75f, 1);
    }
    badge_ingest_scanner_status_evidence(scanner_id, evidence);
#endif

    /* First status message from scanner: flash "connected!" */
    if (!s_first_status_received) {
        s_first_status_received = true;
        led_set_pattern(LED_DETECTION);
        ESP_LOGI(TAG, "Scanner connected (first status received)");
    }
}

/* ── Process one complete JSON line ────────────────────────────────────── */

static void process_line(uint8_t *line, size_t len, int scanner_id)
{
    if (!line || len == 0U ||
        len > SCANNER_UART_LINE_MAX_PAYLOAD) {
        note_scanner_json_parse_error(scanner_id);
        return;
    }

#if defined(FOF_DC34_GAME_CANARY)
    if (len >= sizeof("FOF_CRUD:") - 1U &&
        memcmp(line, "FOF_CRUD:", sizeof("FOF_CRUD:") - 1U) == 0) {
        badge_con_packet_t packet = {0};
        uint32_t local_peer = 0U;
        uint8_t local_session = 0U;
        bool local_identity_valid =
            badge_con_runtime_identity(&local_peer, &local_session);
        if (scanner_id != 0 ||
            !badge_con_parse_uart_line(line, len, &packet) ||
            !local_identity_valid ||
            (packet.peer == local_peer &&
             packet.session == local_session)) {
            note_scanner_json_parse_error(scanner_id);
            return;
        }
        (void)badge_con_runtime_apply_qualified_peer(&packet);
        return;
    }
#endif

    badge_easter_egg_source_t easter_source =
        badge_easter_egg_source_from_uart_frame((const char *)line, len);
    if (easter_source != BADGE_EASTER_EGG_SOURCE_NONE) {
#ifdef FOF_BADGE_VARIANT
        (void)badge_easter_egg_runtime_trigger(easter_source);
#endif
        return;
    }

    fof_scanner_uplink_decision_t decision = {0};
    fof_scanner_uplink_ingress_result_t ingress_result =
        fof_scanner_uplink_ingress_select_and_validate(
            line, len, scanner_id, &decision);
    if (ingress_result != FOF_SCANNER_UPLINK_INGRESS_OK) {
        note_scanner_json_parse_error(scanner_id);
        ESP_LOGW(TAG,
                 "Scanner[%d] rejected raw UART frame result=%d len=%lu",
                 scanner_id, (int)ingress_result, (unsigned long)len);
        return;
    }

#if defined(FOF_DC34_GAME_CANARY)
    if (decision.route == FOF_SCANNER_UPLINK_ROUTE_CRUD_SELF_ACK) {
        badge_con_runtime_note_self_ack(
            decision.crud_peer, decision.crud_session);
        return;
    }
#endif

    /* The raw span is authorized before it is projected to a C string. */
    line[len] = '\0';
    const char *json_line = (const char *)line;
    int_fast64_t now_ms = (int_fast64_t)(esp_timer_get_time() / 1000);

    if (memchr(line, '\0', len) != NULL) {
        note_scanner_json_parse_error(scanner_id);
        ESP_LOGW(TAG, "Scanner[%d] JSON contains embedded NUL", scanner_id);
        return;
    }
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithOpts(json_line, &parse_end, true);
    if (!root) {
        note_scanner_json_parse_error(scanner_id);
        ESP_LOGW(TAG, "Scanner[%d] JSON parse error: %.64s...",
                 scanner_id, json_line);
        return;
    }

    const char *msg_type = json_get_string(root, JSON_KEY_TYPE, NULL);
    if (!msg_type) {
        ESP_LOGW(TAG, "Scanner[%d] message missing 'type' field", scanner_id);
        cJSON_Delete(root);
        return;
    }

    if (decision.route ==
        FOF_SCANNER_UPLINK_ROUTE_BLE_INVESTIGATION) {
#ifdef FOF_BADGE_VARIANT
        note_scanner_activity(scanner_id, now_ms);
        if (!badge_ble_investigation_accept_scanner_json(root)) {
            ESP_LOGW(TAG,
                     "Scanner[0] rejected authorized BLE investigation chunk: %s",
                     msg_type);
        }
#else
        ESP_LOGW(TAG, "BLE investigation chunk ignored outside badge build");
#endif
        cJSON_Delete(root);
        return;
    }

    note_scanner_activity(scanner_id, now_ms);

    /* A stopped scanner still emits identity / status, so use scanner-originated
     * traffic as a chance to release backpressure once the queue has drained. */
    maybe_resume_scanner(scanner_id);

    if (decision.route == FOF_SCANNER_UPLINK_ROUTE_DETECTION) {
        drone_detection_t det;
        if (parse_detection(root, &det)) {
            det.scanner_slot = (uint8_t)scanner_id;
            det.scanner_slots_seen = (uint8_t)(1U << scanner_id);

            const bool low_effort_ble =
                det.source == DETECTION_SRC_BLE_FINGERPRINT &&
                (strcmp(det.manufacturer, "BLE Nearby") == 0 ||
                 strcmp(det.manufacturer, "Unknown") == 0);
            if (!badge_ble_low_effort_detection_allowed(low_effort_ble,
                                                        det.rssi)) {
                cJSON_Delete(root);
                return;
            }

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
                    if (send_scanner_flow_cmd(scanner_id, "stop")) {
                        ESP_LOGW(
                            TAG,
                            "Queue pressure %d/%d — throttling scanner[%d]",
                            (int)queue_count, CONFIG_DETECTION_QUEUE_SIZE,
                            scanner_id);
                        atomic_store(bp_flag, true);
                    }
                }
            } else if (atomic_load(bp_flag) &&
                       queue_count <= (CONFIG_DETECTION_QUEUE_SIZE * 4 / 10)
#ifdef FOF_BADGE_VARIANT
                       && !badge_power_runtime_is_quiet()
#endif
            ) {
                if (send_scanner_flow_cmd(scanner_id, "start")) {
                    ESP_LOGI(
                        TAG,
                        "Queue drained %d/%d — resuming scanner[%d]",
                        (int)queue_count, CONFIG_DETECTION_QUEUE_SIZE,
                        scanner_id);
                    atomic_store(bp_flag, false);
                }
            }

            if (xQueueSend(s_detection_queue, &det, pdMS_TO_TICKS(10)) == pdTRUE) {
                s_detection_count++;
            } else {
                ESP_LOGW(TAG, "Detection queue full, dropping: %s", det.drone_id);
            }
            push_recent(&det);
        }
    } else if (decision.route == FOF_SCANNER_UPLINK_ROUTE_STATUS) {
        uint32_t published_identity_generation =
            publish_scanner_identity_snapshot(
                scanner_id, root, esp_timer_get_time() / 1000, true);
        handle_status(root, scanner_id, published_identity_generation);
    } else if (decision.route == FOF_SCANNER_UPLINK_ROUTE_SCANNER_INFO) {
        /* Scanner identity: version, board type, chip, capabilities, time-offset */
        uint32_t published_identity_generation =
            publish_scanner_identity_snapshot(
                scanner_id, root, esp_timer_get_time() / 1000, false);
        scanner_info_t *info = scanner_info_update_begin(scanner_id);
        if (!info) {
            ESP_LOGE(TAG, "Scanner[%d] identity snapshot lock failed", scanner_id);
            cJSON_Delete(root);
            return;
        }
        const char *ver = json_get_string(root, "ver", "?");
        const char *board = json_get_string(root, "board", "?");
        const char *firmware_name = json_get_string(root, "firmware_name", board);
        const char *app_project = json_get_string(root, "app_project", "");
        const char *hardware_type = json_get_string(root, "hardware_type", "");
        const char *hardware_id = json_get_string(root, "hardware_id", "");
        const char *chip = json_get_string(root, "chip", "?");
        const char *caps = json_get_string(root, "caps", "?");
        uint32_t parsed_boot_id = 0;
        strncpy(info->version, ver, sizeof(info->version) - 1);
        strncpy(info->board, board, sizeof(info->board) - 1);
        strncpy(info->firmware_name, firmware_name, sizeof(info->firmware_name) - 1);
        strncpy(info->app_project, app_project, sizeof(info->app_project) - 1);
        strncpy(info->hardware_type, hardware_type, sizeof(info->hardware_type) - 1);
        strncpy(info->hardware_id, hardware_id, sizeof(info->hardware_id) - 1);
        strncpy(info->chip, chip, sizeof(info->chip) - 1);
        strncpy(info->caps, caps, sizeof(info->caps) - 1);
        info->version[sizeof(info->version) - 1] = '\0';
        info->board[sizeof(info->board) - 1] = '\0';
        info->firmware_name[sizeof(info->firmware_name) - 1] = '\0';
        info->app_project[sizeof(info->app_project) - 1] = '\0';
        info->hardware_type[sizeof(info->hardware_type) - 1] = '\0';
        info->hardware_id[sizeof(info->hardware_id) - 1] = '\0';
        info->chip[sizeof(info->chip) - 1] = '\0';
        info->caps[sizeof(info->caps) - 1] = '\0';
        info->boot_id = 0;
        if (json_get_uint32_exact(root, "boot_id", &parsed_boot_id) &&
            parsed_boot_id != 0U) {
            info->boot_id = parsed_boot_id;
        }
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
        info->ble_initialized = json_get_bool(
            root, "ble_initialized", info->ble_initialized);
        info->ble_scanning = json_get_bool(
            root, "ble_scanning", info->ble_scanning);
        info->ble_host_active = json_get_bool(
            root, "ble_host_active", info->ble_host_active);
        info->ble_host_synced = json_get_bool(
            root, "ble_host_synced", info->ble_host_synced);
        info->ble_quiesced = json_get_bool(
            root, "ble_quiesced", info->ble_quiesced);
        info->wifi_initialized = json_get_bool(
            root, "wifi_initialized", info->wifi_initialized);
        info->wifi_active = json_get_bool(
            root, "wifi_active", info->wifi_active);
        info->wifi_quiesced = json_get_bool(
            root, "wifi_quiesced", info->wifi_quiesced);
        info->wifi_init_rc = json_get_int(
            root, "wifi_init_rc", info->wifi_init_rc);
        info->wifi_paused = json_get_bool(
            root, "wifi_paused", info->wifi_paused);
        const char *slot_role = json_get_string(
            root, JSON_KEY_SLOT_ROLE, info->slot_role);
        strncpy(info->slot_role, slot_role, sizeof(info->slot_role) - 1);
        info->slot_role[sizeof(info->slot_role) - 1] = '\0';
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
        const cJSON *quiet_mode = cJSON_GetObjectItemCaseSensitive(root, "quiet_mode");
        if (quiet_mode) {
            info->quiet_mode = cJSON_IsTrue(quiet_mode) ||
                               (cJSON_IsNumber(quiet_mode) &&
                                quiet_mode->valueint != 0);
        }
        info->quiet_generation = (uint32_t)json_get_double(
            root, "quiet_generation", (double)info->quiet_generation);
        if (published_identity_generation != 0) {
            info->identity_generation = published_identity_generation;
        } else {
            info->identity_generation++;
        }
        info->received = true;
#ifdef FOF_BADGE_VARIANT
        s_badge_status_evidence[scanner_id] = *info;
#endif
        uint32_t log_boot_id = info->boot_id;
        int64_t log_toff_ms = info->toff_ms;
        uint32_t log_tcnt = info->tcnt;
        uint32_t log_time_valid_count = info->time_valid_count;
        scanner_info_update_commit();
        ESP_LOGI(TAG, "Scanner[%d] identity: %s v%s (%s) chip=%s boot=%08lx toff=%lld tcnt=%u valid=%u state=%s mode=%s",
                 scanner_id, board, ver, caps, chip,
                 (unsigned long)log_boot_id,
                 (long long)log_toff_ms, log_tcnt,
                 log_time_valid_count, time_state,
                 scan_mode && scan_mode[0] ? scan_mode : "normal");
#ifdef FOF_BADGE_VARIANT
        badge_ingest_scanner_status_evidence(
            scanner_id, &s_badge_status_evidence[scanner_id]);
        badge_power_runtime_note_scanner_identity(scanner_id);
#endif
    } else if (
        decision.route == FOF_SCANNER_UPLINK_ROUTE_RECOVERY_ACK ||
        decision.route == FOF_SCANNER_UPLINK_ROUTE_SCANNER_RECOVERY) {
        scanner_info_t *info = scanner_info_update_begin(scanner_id);
        if (!info) {
            ESP_LOGE(TAG, "Scanner[%d] recovery snapshot lock failed", scanner_id);
            cJSON_Delete(root);
            return;
        }
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
        const char *safe_reason = json_get_string(root, "safe_reason", "");
        uint32_t crash_count = info->crash_count;
        scanner_info_update_commit();
        ESP_LOGW(TAG, "Scanner[%d] recovery: type=%s mode=%s reason=%s crash=%lu",
                 scanner_id,
                 msg_type,
                 mode && mode[0] ? mode : "?",
                 safe_reason && safe_reason[0] ? safe_reason : "",
                 (unsigned long)crash_count);
    } else if (
        decision.route == FOF_SCANNER_UPLINK_ROUTE_FIRMWARE &&
        decision.firmware_schema_id ==
            FOF_FW_JSON_SCHEMA_RECEIPT_FW_CHECK) {
        const char *board = json_get_string(root, "board", "");
        const char *ver = json_get_string(root, "ver", "");
        const char *reason = json_get_string(root, "reason", "");
        scanner_info_t *info = scanner_info_update_begin(scanner_id);
        if (!info) {
            ESP_LOGE(TAG, "Scanner[%d] firmware-check snapshot lock failed", scanner_id);
            cJSON_Delete(root);
            return;
        }
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
        scanner_info_update_commit();
        ESP_LOGI(TAG, "Scanner[%d] firmware check: board=%s ver=%s",
                 scanner_id,
                 board[0] ? board : "?",
                 ver[0] ? ver : "?");
        fw_store_handle_scanner_check(scanner_id, board, ver, reason);
    } else if (
        decision.route == FOF_SCANNER_UPLINK_ROUTE_FIRMWARE &&
        (decision.firmware_schema_id ==
             FOF_FW_JSON_SCHEMA_RECEIPT_FW_READY_STRICT ||
         decision.firmware_schema_id ==
             FOF_FW_JSON_SCHEMA_RECEIPT_FW_READY_LEGACY_68)) {
        const char *board = "";
        const char *ver = "";
        const char *target = "";
        uint32_t target_size = 0;
        uint32_t target_crc32 = 0;
        bool legacy_receipt =
            decision.firmware_schema_id ==
            FOF_FW_JSON_SCHEMA_RECEIPT_FW_READY_LEGACY_68;
        bool ready_frame_valid = fw_ready_common_fields_valid(
            root, &board, &ver, &target, &target_size, &target_crc32);
        bool accepted = false;

        if (legacy_receipt) {
            if (ready_frame_valid) {
                accepted = fw_store_handle_legacy_scanner_ready(
                    scanner_id, board, ver, target, target_size,
                    target_crc32);
            }
        } else {
            const char *target_name = "";
            const char *target_project = "";
            const char *target_hardware = "";
            const char *target_sha256 = "";
            uint32_t target_generation = 0;
            const cJSON *allow_same_j = cJSON_GetObjectItemCaseSensitive(
                root, "allow_same_version");
            ready_frame_valid = ready_frame_valid &&
                json_get_nonempty_string_exact(root, JSON_KEY_FW_NAME,
                                               &target_name) &&
                json_get_nonempty_string_exact(root, "app_project",
                                               &target_project) &&
                json_get_nonempty_string_exact(root, "hardware_type",
                                               &target_hardware) &&
                json_get_nonempty_string_exact(root, "sha256",
                                               &target_sha256) &&
                json_get_uint32_exact(root, "generation",
                                      &target_generation) &&
                cJSON_IsFalse(allow_same_j);
            if (ready_frame_valid) {
                accepted = fw_store_handle_scanner_ready(
                    scanner_id, board, ver, target, target_name,
                    target_project, target_hardware, target_sha256,
                    target_generation, target_size, target_crc32);
            }
        }

        scanner_info_t *info = scanner_info_update_begin(scanner_id);
        if (!info) {
            ESP_LOGE(TAG, "Scanner[%d] firmware-ready snapshot lock failed", scanner_id);
            cJSON_Delete(root);
            return;
        }
        if (!accepted) {
            const char *reject_error =
                ready_frame_valid ? "fw_ready_refused" : "malformed_fw_ready";
            info->need_firmware = false;
            strncpy(info->fw_update_state, "ready_rejected",
                    sizeof(info->fw_update_state) - 1);
            info->fw_update_state[sizeof(info->fw_update_state) - 1] = '\0';
            strncpy(info->last_fw_error, reject_error,
                    sizeof(info->last_fw_error) - 1);
            info->last_fw_error[sizeof(info->last_fw_error) - 1] = '\0';
            scanner_info_update_commit();
            ESP_LOGW(TAG,
                     "Scanner[%d] %s fw_ready rejected (%s); resuming scanner",
                     scanner_id, legacy_receipt ? "legacy" : "strict",
                     reject_error);
            uart_rx_send_command_to_scanner(scanner_id, "{\"type\":\"start\"}");
            cJSON_Delete(root);
            return;
        }
        info->need_firmware = true;
        strncpy(info->fw_update_state, "ready", sizeof(info->fw_update_state) - 1);
        info->fw_update_state[sizeof(info->fw_update_state) - 1] = '\0';
        strncpy(info->fw_target_version, target, sizeof(info->fw_target_version) - 1);
        info->fw_target_version[sizeof(info->fw_target_version) - 1] = '\0';
        scanner_info_update_commit();
        ESP_LOGW(TAG,
                 "Scanner[%d] %s firmware ready durably accepted: "
                 "board=%s current=%s target=%s",
                 scanner_id,
                 legacy_receipt ? "legacy" : "strict",
                 board[0] ? board : "?",
                 ver[0] ? ver : "?",
                 target[0] ? target : "?");
    } else if (
        decision.route == FOF_SCANNER_UPLINK_ROUTE_CAL_MODE_ACK) {
        scanner_info_t *info = scanner_info_update_begin(scanner_id);
        if (!info) {
            ESP_LOGE(TAG, "Scanner[%d] calibration snapshot lock failed", scanner_id);
            cJSON_Delete(root);
            return;
        }
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
        scanner_info_update_commit();
        ESP_LOGW(TAG, "Scanner[%d] calibration ack: ok=%d mode=%s uuid=%s",
                 scanner_id,
                 ok ? 1 : 0,
                 scan_mode && scan_mode[0] ? scan_mode : "normal",
                 cal_uuid && cal_uuid[0] ? cal_uuid : "-");
    } else if (
        decision.route == FOF_SCANNER_UPLINK_ROUTE_SCAN_PROFILE_ACK) {
        scanner_info_t *info = scanner_info_update_begin(scanner_id);
        if (!info) {
            ESP_LOGE(TAG, "Scanner[%d] profile snapshot lock failed", scanner_id);
            cJSON_Delete(root);
            return;
        }
        const char *scan_profile = json_get_string(root, JSON_KEY_SCAN_PROFILE, "");
        strncpy(info->scan_profile, scan_profile, sizeof(info->scan_profile) - 1);
        info->scan_profile[sizeof(info->scan_profile) - 1] = '\0';
        const char *slot_role = json_get_string(root, JSON_KEY_SLOT_ROLE, "");
        if (json_get_bool(root, "slot_role_ok", false)) {
            strncpy(info->slot_role, slot_role, sizeof(info->slot_role) - 1);
            info->slot_role[sizeof(info->slot_role) - 1] = '\0';
        } else {
            info->slot_role[0] = '\0';
        }
        info->scan_profile_ack_generation++;
        info->cmd_rx_count++;
        info->cmd_last_age_s = 0;
        info->received = true;
        scanner_info_update_commit();
        ESP_LOGI(TAG, "Scanner[%d] scan profile ack: %s",
                 scanner_id,
                 scan_profile && scan_profile[0] ? scan_profile : "?");
    } else if (
        decision.route == FOF_SCANNER_UPLINK_ROUTE_DISPLAY_CONTROL_ACK) {
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
    } else if (
        decision.route == FOF_SCANNER_UPLINK_ROUTE_DISPLAY_POLICY_ACK) {
        scanner_info_t *info = scanner_info_update_begin(scanner_id);
        if (!info) {
            ESP_LOGE(TAG, "Scanner[%d] display-policy snapshot lock failed", scanner_id);
            cJSON_Delete(root);
            return;
        }
        info->display_policy_ack_hash = (uint32_t)json_get_double(
            root, "hash", (double)info->display_policy_ack_hash
        );
        info->cmd_rx_count++;
        info->cmd_last_age_s = 0;
        info->received = true;
        uint32_t display_policy_ack_hash = info->display_policy_ack_hash;
        scanner_info_update_commit();
        ESP_LOGI(TAG, "Scanner[%d] display policy ack hash=%lu",
                 scanner_id, (unsigned long)display_policy_ack_hash);
    } else if (
        decision.route == FOF_SCANNER_UPLINK_ROUTE_SCANNER_QUIET_ACK) {
        scanner_info_t *info = scanner_info_update_begin(scanner_id);
        if (!info) {
            ESP_LOGE(TAG, "Scanner[%d] quiet-mode snapshot lock failed", scanner_id);
            cJSON_Delete(root);
            return;
        }
        const cJSON *ok_j = cJSON_GetObjectItemCaseSensitive(root, "ok");
        const cJSON *enabled_j = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        const cJSON *tx_enabled_j = cJSON_GetObjectItemCaseSensitive(
            root, "tx_enabled");
        const cJSON *ble_scanning_j = cJSON_GetObjectItemCaseSensitive(
            root, "ble_scanning");
        const cJSON *wifi_paused_j = cJSON_GetObjectItemCaseSensitive(
            root, "wifi_paused");
        const cJSON *ble_quiesced_j = cJSON_GetObjectItemCaseSensitive(
            root, "ble_quiesced");
        const cJSON *wifi_quiesced_j = cJSON_GetObjectItemCaseSensitive(
            root, "wifi_quiesced");
        const cJSON *ble_active_j = cJSON_GetObjectItemCaseSensitive(
            root, "ble_active");
        const cJSON *wifi_active_j = cJSON_GetObjectItemCaseSensitive(
            root, "wifi_active");
        const cJSON *radios_ready_j = cJSON_GetObjectItemCaseSensitive(
            root, "radios_ready");
        const cJSON *tx_restored_j = cJSON_GetObjectItemCaseSensitive(
            root, "tx_restored");
        const cJSON *uart_commands_j = cJSON_GetObjectItemCaseSensitive(
            root, "uart_commands");
        uint32_t generation = 0;
        bool generation_valid = json_get_uint32_exact(
            root, JSON_KEY_GENERATION, &generation);
        bool quiet_ack_fields_valid = cJSON_IsBool(ok_j) &&
            cJSON_IsBool(enabled_j) && cJSON_IsBool(tx_enabled_j) &&
            cJSON_IsBool(ble_scanning_j) && cJSON_IsBool(wifi_paused_j) &&
            cJSON_IsBool(ble_quiesced_j) &&
            cJSON_IsBool(wifi_quiesced_j) &&
            cJSON_IsBool(ble_active_j) && cJSON_IsBool(wifi_active_j) &&
            cJSON_IsBool(radios_ready_j) && cJSON_IsBool(tx_restored_j) &&
            cJSON_IsBool(uart_commands_j) && generation_valid;
        bool transition_ok = quiet_ack_fields_valid && cJSON_IsTrue(ok_j);
        bool enabled = cJSON_IsTrue(enabled_j);
        bool tx_enabled = cJSON_IsTrue(tx_enabled_j);
        bool ble_scanning = cJSON_IsTrue(ble_scanning_j);
        bool wifi_paused = cJSON_IsTrue(wifi_paused_j);
        bool ble_quiesced = cJSON_IsTrue(ble_quiesced_j);
        bool wifi_quiesced = cJSON_IsTrue(wifi_quiesced_j);
        bool ble_active = cJSON_IsTrue(ble_active_j);
        bool wifi_active = cJSON_IsTrue(wifi_active_j);
        bool radios_ready = cJSON_IsTrue(radios_ready_j);
        bool tx_restored = cJSON_IsTrue(tx_restored_j);
        bool uart_commands = cJSON_IsTrue(uart_commands_j);
        info->quiet_transition_ok = transition_ok;
        info->quiet_mode = enabled;
        info->quiet_tx_enabled = tx_enabled;
        info->quiet_generation = generation;
        info->quiet_uart_commands = uart_commands;
        info->quiet_ble_quiesced = ble_quiesced;
        info->quiet_wifi_quiesced = wifi_quiesced;
        info->quiet_ble_active = ble_active;
        info->quiet_wifi_active = wifi_active;
        info->quiet_radios_ready = radios_ready;
        info->quiet_tx_restored = tx_restored;
        info->ble_scanning = ble_scanning;
        info->wifi_paused = wifi_paused;
        info->cmd_rx_count++;
        info->cmd_last_age_s = 0;
        info->received = true;
        scanner_info_update_commit();
#ifdef FOF_BADGE_VARIANT
        badge_power_runtime_note_scanner_ack(
            scanner_id, transition_ok, enabled, generation,
            tx_enabled, ble_scanning, wifi_paused,
            ble_quiesced, wifi_quiesced, ble_active, wifi_active,
            radios_ready, tx_restored, uart_commands);
#endif
    } else if (
        decision.route == FOF_SCANNER_UPLINK_ROUTE_FIRMWARE) {
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
    uint8_t *line_buf;
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
        params->line_buf = (uint8_t *)uart_rx_alloc_persistent_buffer(
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

    uint8_t *line_buf = params->line_buf;
    uint8_t *read_buf = params->read_buf;

    if (!line_buf || !read_buf) {
        ESP_LOGE(TAG, "UART RX task missing buffers: %s", params->label);
        vTaskDelete(NULL);
        return;
    }

    scanner_uart_line_framer_t framer = {0};
    if (!scanner_uart_line_framer_init(
            &framer, line_buf, LINE_BUF_SIZE)) {
        ESP_LOGE(TAG, "UART RX task failed to initialize framer: %s",
                 params->label);
        vTaskDelete(NULL);
        return;
    }

    atomic_bool *task_entered = scanner_id == 0
        ? &s_rx_task_entered_ble : &s_rx_task_entered_wifi;
    atomic_bool *task_started = scanner_id == 0
        ? &s_rx_task_started_ble : &s_rx_task_started_wifi;
    atomic_store_explicit(task_entered, true, memory_order_release);
    atomic_store_explicit(task_started, true, memory_order_release);

    ESP_LOGI(TAG, "UART RX task started: %s (UART%d)", params->label, uart_num);

    int debug_dumps = 3;  /* dump first 3 reads for diagnostics */
    int64_t last_heartbeat_ms = esp_timer_get_time() / 1000;
    int64_t partial_started_ms = 0;
    int total_bytes = 0;

    while (1) {
        /* Periodic heartbeat so we know the task is alive */
        int64_t now_ms = esp_timer_get_time() / 1000;
#ifdef FOF_BADGE_VARIANT
        badge_runtime_note_scanner_uart_worker_alive((uint8_t)scanner_id);
#endif
        if (now_ms - last_heartbeat_ms > 5000) {
            note_uart_rx_stack_free(scanner_id);
            ESP_LOGI(TAG, "[%s] heartbeat: %d total bytes received stack=%lu",
                     params->label, total_bytes,
                     (unsigned long)uxTaskGetStackHighWaterMark(NULL));
            last_heartbeat_ms = now_ms;
        }
        /* During OTA relay, pause reading so relay handler can read ACKs directly */
        atomic_uint_fast32_t *pause_request = (scanner_id == 0)
            ? &s_rx_pause_request_generation_ble
            : &s_rx_pause_request_generation_wifi;
        atomic_uint_fast32_t *pause_ack = (scanner_id == 0)
            ? &s_rx_pause_ack_generation_ble
            : &s_rx_pause_ack_generation_wifi;
        uint32_t request_generation = (uint32_t)atomic_load_explicit(
            pause_request, memory_order_acquire);
        if (request_generation != 0U) {
            atomic_store_explicit(pause_ack, request_generation,
                                  memory_order_release);
            do {
                vTaskDelay(pdMS_TO_TICKS(UART_RX_PAUSE_POLL_MS));
            } while ((uint32_t)atomic_load_explicit(
                         pause_request, memory_order_acquire) ==
                     request_generation);
            uint_fast32_t acknowledged = request_generation;
            (void)atomic_compare_exchange_strong_explicit(
                pause_ack, &acknowledged, 0U,
                memory_order_acq_rel, memory_order_acquire);
            scanner_uart_line_framer_reset(&framer);
            partial_started_ms = 0;
            continue;
        }

        int bytes_read = uart_read_bytes(uart_num, read_buf, READ_BUF_SIZE,
                                         pdMS_TO_TICKS(100));
        if (bytes_read <= 0) {
            if (partial_started_ms > 0 &&
                now_ms - partial_started_ms >=
                    UART_RX_PARTIAL_FRAME_TIMEOUT_MS) {
                scanner_uart_line_event_t event =
                    scanner_uart_line_framer_expire_partial(&framer);
                if (event.kind ==
                    SCANNER_UART_LINE_EVENT_FRAME_REJECTED) {
                    ESP_LOGW(TAG,
                             "[%s] stale partial UART frame discarded",
                             params->label);
                }
                partial_started_ms = 0;
            }
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

        size_t offset = 0U;
        while (offset < (size_t)bytes_read) {
            bool had_partial =
                scanner_uart_line_framer_has_partial(&framer);
            size_t consumed = 0U;
            scanner_uart_line_event_t event =
                scanner_uart_line_framer_consume(
                    &framer,
                    read_buf + offset,
                    (size_t)bytes_read - offset,
                    &consumed);
            if (consumed == 0U) {
                ESP_LOGE(TAG, "[%s] UART framer made no progress",
                         params->label);
                break;
            }
            offset += consumed;

            if (event.kind == SCANNER_UART_LINE_EVENT_FRAME_READY) {
                partial_started_ms = 0;
                process_line((uint8_t *)event.bytes, event.byte_len,
                             scanner_id);
            } else if (
                event.kind ==
                SCANNER_UART_LINE_EVENT_FRAME_REJECTED) {
                partial_started_ms = 0;
                if (event.reject_reason ==
                    SCANNER_UART_LINE_REJECT_TOO_LONG) {
                    note_scanner_line_overflow(scanner_id);
                }
                ESP_LOGW(TAG,
                         "[%s] rejected UART frame reason=%d",
                         params->label, (int)event.reject_reason);
            } else if (
                !had_partial &&
                scanner_uart_line_framer_has_partial(&framer)) {
                partial_started_ms = now_ms;
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

#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
_Static_assert(
    CONFIG_DETECTION_QUEUE_SIZE * sizeof(drone_detection_t) == 41472U,
    "CON CRUD queue reclamation evidence changed");
#endif

uint32_t uart_rx_detection_queue_capacity(void)
{
#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
    return 0U;
#else
    return CONFIG_DETECTION_QUEUE_SIZE;
#endif
}

uint32_t uart_rx_detection_queue_reclaimed_bytes(void)
{
#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
    return (uint32_t)(
        CONFIG_DETECTION_QUEUE_SIZE * sizeof(drone_detection_t));
#else
    return 0U;
#endif
}

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
    (void)uart_rx_scanner_tx_lease_init();
    if (!s_scanner_info_mutex) {
        s_scanner_info_mutex = xSemaphoreCreateMutexStatic(
            &s_scanner_info_mutex_storage);
        if (!s_scanner_info_mutex) {
            ESP_LOGE(TAG, "Failed to create scanner status snapshot mutex");
        }
    }
    if (!s_scanner_identity_mutex) {
        s_scanner_identity_mutex = xSemaphoreCreateMutexStatic(
            &s_scanner_identity_mutex_storage);
        if (!s_scanner_identity_mutex) {
            ESP_LOGE(TAG, "Failed to create scanner identity snapshot mutex");
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

bool uart_rx_scanner_tx_lease_init(void)
{
    if (s_uart_tx_lock) {
        return true;
    }
    s_uart_tx_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_uart_tx_lock) {
        ESP_LOGE(TAG, "Failed to create recursive scanner UART TX lease");
        return false;
    }
    return true;
}

bool uart_rx_scanner_tx_lease_acquire(TickType_t wait_ticks)
{
    SemaphoreHandle_t lock = s_uart_tx_lock;
    if (!lock) {
        ESP_LOGE(TAG, "Scanner UART TX lease unavailable before UART init");
        return false;
    }
    if (xSemaphoreTakeRecursive(lock, wait_ticks) != pdTRUE) {
        ESP_LOGE(TAG, "Scanner UART TX lease acquisition timed out");
        return false;
    }
    return true;
}

void uart_rx_scanner_tx_lease_release(void)
{
    SemaphoreHandle_t lock = s_uart_tx_lock;
    if (lock) {
        xSemaphoreGiveRecursive(lock);
    }
}

static bool wait_for_rx_task_entries(void)
{
    int64_t entry_deadline_ms = (esp_timer_get_time() / 1000) +
                                UART_RX_ENTRY_ACK_TIMEOUT_MS;
    bool entries_ready = false;
    do {
        entries_ready = atomic_load_explicit(
            &s_rx_task_entered_ble, memory_order_acquire)
#if CONFIG_DUAL_SCANNER
            && atomic_load_explicit(
                &s_rx_task_entered_wifi, memory_order_acquire)
#endif
            ;
        if (!entries_ready) {
            vTaskDelay(pdMS_TO_TICKS(UART_RX_PAUSE_POLL_MS));
        }
    } while (!entries_ready &&
             (esp_timer_get_time() / 1000) < entry_deadline_ms);
    return entries_ready;
}

bool uart_rx_start(void)
{
    if (atomic_load_explicit(&s_rx_task_started_ble, memory_order_acquire)
#if CONFIG_DUAL_SCANNER
        && atomic_load_explicit(&s_rx_task_started_wifi, memory_order_acquire)
#endif
    ) {
        return true;
    }

    if (!s_ble_task_params.line_buf || !s_ble_task_params.read_buf) {
        ESP_LOGE(TAG, "BLE scanner RX buffers are unavailable");
        return false;
    }
#if CONFIG_DUAL_SCANNER
    if (!s_wifi_task_params.line_buf || !s_wifi_task_params.read_buf) {
        ESP_LOGE(TAG, "WiFi scanner RX buffers are unavailable");
        return false;
    }
#endif

    atomic_store_explicit(&s_rx_task_entered_ble, false,
                          memory_order_release);
    atomic_store_explicit(&s_rx_task_entered_wifi, false,
                          memory_order_release);
    atomic_store_explicit(&s_rx_task_started_ble, false,
                          memory_order_release);
    atomic_store_explicit(&s_rx_task_started_wifi, false,
                          memory_order_release);
    TaskHandle_t ble_task = NULL;
    BaseType_t ble_ok = xTaskCreate(
        uart_rx_task, "uart_rx_ble", CONFIG_UART_RX_STACK,
        &s_ble_task_params, CONFIG_UART_RX_PRIORITY, &ble_task);
    if (ble_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create BLE scanner RX task");
        return false;
    }
    ESP_LOGI(TAG, "BLE scanner RX task created");

#if CONFIG_DUAL_SCANNER
    TaskHandle_t wifi_task = NULL;
    BaseType_t wifi_ok = xTaskCreate(
        uart_rx_task, "uart_rx_wifi", CONFIG_UART_RX_STACK,
        &s_wifi_task_params, CONFIG_UART_RX_PRIORITY, &wifi_task);
    if (wifi_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create WiFi scanner RX task");
        vTaskDelete(ble_task);
        atomic_store_explicit(&s_rx_task_entered_ble, false,
                              memory_order_release);
        atomic_store_explicit(&s_rx_task_started_ble, false,
                              memory_order_release);
        return false;
    }
    ESP_LOGI(TAG, "WiFi scanner RX task created");
#endif

    if (!wait_for_rx_task_entries()) {
        ESP_LOGE(TAG, "UART RX task entry acknowledgement timed out");
#if CONFIG_DUAL_SCANNER
        vTaskDelete(wifi_task);
#endif
        vTaskDelete(ble_task);
        atomic_store_explicit(&s_rx_task_entered_ble, false,
                              memory_order_release);
        atomic_store_explicit(&s_rx_task_entered_wifi, false,
                              memory_order_release);
        atomic_store_explicit(&s_rx_task_started_ble, false,
                              memory_order_release);
        atomic_store_explicit(&s_rx_task_started_wifi, false,
                              memory_order_release);
        return false;
    }
    return true;
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

bool uart_rx_ble_investigation_ingress_available(void)
{
    return uart_rx_is_ble_scanner_connected() &&
           !uart_rx_scanner_is_paused(0);
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

static fof_scanner_deployment_t scanner_command_deployment(void)
{
#ifdef FOF_BADGE_VARIANT
    return FOF_SCANNER_DEPLOYMENT_BADGE;
#else
    return FOF_SCANNER_DEPLOYMENT_NON_BADGE;
#endif
}

static bool scanner_command_is_scan_control(
    fof_scanner_command_id_t command_id)
{
    return command_id == FOF_SCANNER_COMMAND_WIFI_LOCKON ||
           command_id == FOF_SCANNER_COMMAND_WIFI_LOCKON_CANCEL ||
           command_id == FOF_SCANNER_COMMAND_BLE_LOCKON ||
           command_id == FOF_SCANNER_COMMAND_BLE_LOCKON_CANCEL;
}

static bool scanner_command_is_ble_only(
    fof_scanner_command_id_t command_id)
{
    return command_id == FOF_SCANNER_COMMAND_BLE_LOCKON ||
           command_id == FOF_SCANNER_COMMAND_BLE_LOCKON_CANCEL;
}

static bool scanner_command_is_wifi_only(
    fof_scanner_command_id_t command_id)
{
    return command_id == FOF_SCANNER_COMMAND_WIFI_LOCKON ||
           command_id == FOF_SCANNER_COMMAND_WIFI_LOCKON_CANCEL;
}

void uart_rx_send_command(const char *json_cmd)
{
    if (!json_cmd) {
        return;
    }
    size_t command_len =
        strnlen(json_cmd, SCANNER_UART_LINE_MAX_PAYLOAD + 1U);
    fof_scanner_command_decision_t decision = {0};
    fof_scanner_command_registry_result_t authorization =
        fof_scanner_command_select_and_validate(
            (const uint8_t *)json_cmd, command_len,
            scanner_command_deployment(), &decision);
    if (command_len == 0U ||
        command_len > SCANNER_UART_LINE_MAX_PAYLOAD ||
        authorization != FOF_SCANNER_COMMAND_REGISTRY_OK) {
        ESP_LOGW(TAG, "Rejecting unauthorized scanner command result=%d",
                 (int)authorization);
        return;
    }

    if (s_node_calibration_mode &&
        scanner_command_is_scan_control(decision.command.id)) {
        ESP_LOGW(TAG, "Rejecting scan-control command while calibration mode is active");
        return;
    }
    const bool ble_only =
        scanner_command_is_ble_only(decision.command.id);
#if CONFIG_DUAL_SCANNER
    const bool wifi_only =
        scanner_command_is_wifi_only(decision.command.id);
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
    size_t command_len =
        strnlen(json_cmd, SCANNER_UART_LINE_MAX_PAYLOAD + 1U);
    fof_scanner_command_decision_t decision = {0};
    fof_scanner_command_registry_result_t authorization =
        fof_scanner_command_select_and_validate(
            (const uint8_t *)json_cmd, command_len,
            scanner_command_deployment(), &decision);
    if (command_len == 0U ||
        command_len > SCANNER_UART_LINE_MAX_PAYLOAD ||
        authorization != FOF_SCANNER_COMMAND_REGISTRY_OK) {
        ESP_LOGW(TAG,
                 "Rejecting unauthorized scanner[%d] command result=%d",
                 scanner_id, (int)authorization);
        return false;
    }

    if (s_node_calibration_mode &&
        scanner_command_is_scan_control(decision.command.id)) {
        ESP_LOGW(TAG, "Rejecting scan-control command while calibration mode is active");
        return false;
    }

    return send_json_line_to_scanner_locked(scanner_id, json_cmd);
}

void uart_rx_send_command_to_scanner(int scanner_id, const char *json_cmd)
{
    (void)uart_rx_send_command_to_scanner_checked(scanner_id, json_cmd);
}

bool uart_rx_get_scanner_info_snapshot(int scanner_id, scanner_info_t *out)
{
    if ((scanner_id != 0 && scanner_id != 1) || !out ||
        !s_scanner_info_mutex ||
        xSemaphoreTake(s_scanner_info_mutex,
                       pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    *out = *scanner_info_for_slot_unlocked(scanner_id);
    xSemaphoreGive(s_scanner_info_mutex);
    return out->received;
}

bool uart_rx_get_scanner_identity_snapshot(
    int scanner_id, scanner_identity_snapshot_t *out)
{
    if (scanner_id < 0 || scanner_id > 1 || !out ||
        !s_scanner_identity_mutex ||
        xSemaphoreTake(s_scanner_identity_mutex,
                       pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    *out = s_scanner_identity_snapshots[scanner_id];
    xSemaphoreGive(s_scanner_identity_mutex);
    return out->identity_generation != 0;
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

bool uart_rx_scanner_task_started(int scanner_id)
{
    if (scanner_id == 0) {
        return atomic_load_explicit(
            &s_rx_task_started_ble, memory_order_acquire);
    }
    if (scanner_id == 1) {
#if CONFIG_DUAL_SCANNER
        return atomic_load_explicit(
            &s_rx_task_started_wifi, memory_order_acquire);
#else
        return false;
#endif
    }
    return false;
}

static atomic_uint_fast32_t *pause_request_for_scanner(int scanner_id)
{
    if (scanner_id == 0) {
        return &s_rx_pause_request_generation_ble;
    }
    if (scanner_id == 1) {
        return &s_rx_pause_request_generation_wifi;
    }
    return NULL;
}

static atomic_uint_fast32_t *pause_ack_for_scanner(int scanner_id)
{
    if (scanner_id == 0) {
        return &s_rx_pause_ack_generation_ble;
    }
    if (scanner_id == 1) {
        return &s_rx_pause_ack_generation_wifi;
    }
    return NULL;
}

bool uart_rx_scanner_is_paused(int scanner_id)
{
    atomic_uint_fast32_t *request = pause_request_for_scanner(scanner_id);
    return request && atomic_load_explicit(
        request, memory_order_acquire) != 0U;
}

static bool next_pause_generation(uint32_t *out)
{
    if (!out) {
        return false;
    }
    uint_fast32_t current = atomic_load_explicit(
        &s_rx_pause_next_generation, memory_order_acquire);
    while (current < UINT32_MAX) {
        uint_fast32_t desired = current + 1U;
        if (atomic_compare_exchange_weak_explicit(
                &s_rx_pause_next_generation, &current, desired,
                memory_order_acq_rel, memory_order_acquire)) {
            *out = (uint32_t)desired;
            return true;
        }
    }
    return false;
}

bool uart_rx_pause_scanner_guarded(int scanner_id,
                                  uart_rx_pause_guard_t *guard)
{
    atomic_uint_fast32_t *request = pause_request_for_scanner(scanner_id);
    atomic_uint_fast32_t *ack = pause_ack_for_scanner(scanner_id);
    if (!request || !ack || !guard || guard->acquired) {
        return false;
    }
    memset(guard, 0, sizeof(*guard));
    if (!uart_rx_scanner_task_started(scanner_id)) {
        return true;
    }

    uint_fast32_t expected = 0U;
    uint32_t request_generation = 0U;
    if (!next_pause_generation(&request_generation)) {
        ESP_LOGE(TAG, "UART RX pause generation exhausted");
        return false;
    }
    if (!atomic_compare_exchange_strong_explicit(
            request, &expected, request_generation,
            memory_order_acq_rel, memory_order_acquire)) {
        ESP_LOGE(TAG,
                 "UART RX pause refused for scanner %d: already paused gen=%lu",
                 scanner_id, (unsigned long)expected);
        return false;
    }
    guard->request_generation = request_generation;
    guard->acquired = true;

    int64_t deadline_ms = (esp_timer_get_time() / 1000) +
        UART_RX_PAUSE_ACK_TIMEOUT_MS;
    uint32_t ack_generation;
    do {
        ack_generation = (uint32_t)atomic_load_explicit(
            ack, memory_order_acquire);
        if (ack_generation == request_generation) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(UART_RX_PAUSE_POLL_MS));
    } while ((esp_timer_get_time() / 1000) < deadline_ms);
    bool acked = ack_generation == request_generation;
    ESP_LOGW(TAG, "UART RX pause for scanner %d gen=%lu: %s",
             scanner_id, (unsigned long)request_generation,
             acked ? "acknowledged" : "timeout");
    return acked;
}

bool uart_rx_discard_scanner_backlog_guarded(int scanner_id,
    const uart_rx_pause_guard_t *guard)
{
    atomic_uint_fast32_t *request =
        pause_request_for_scanner(scanner_id);
    atomic_uint_fast32_t *ack = pause_ack_for_scanner(scanner_id);
    if (!request || !ack || !guard || !guard->acquired ||
        guard->request_generation == 0U) {
        return false;
    }

    uint32_t request_generation = (uint32_t)atomic_load_explicit(
        request, memory_order_acquire);
    uint32_t ack_generation = (uint32_t)atomic_load_explicit(
        ack, memory_order_acquire);
    if (request_generation != guard->request_generation ||
        ack_generation != guard->request_generation) {
        ESP_LOGE(TAG,
                 "UART RX discard refused for scanner %d: "
                 "guard=%lu request=%lu ack=%lu",
                 scanner_id,
                 (unsigned long)guard->request_generation,
                 (unsigned long)request_generation,
                 (unsigned long)ack_generation);
        return false;
    }

    uart_port_t uart = scanner_uart_for_id(scanner_id);
    esp_err_t err = uart_flush_input(uart);

    request_generation = (uint32_t)atomic_load_explicit(
        request, memory_order_acquire);
    ack_generation = (uint32_t)atomic_load_explicit(
        ack, memory_order_acquire);
    bool ownership_retained =
        request_generation == guard->request_generation &&
        ack_generation == guard->request_generation;
    if (err != ESP_OK || !ownership_retained) {
        ESP_LOGE(TAG,
                 "UART RX discard failed for scanner %d UART%d: "
                 "err=%s guard=%lu request=%lu ack=%lu",
                 scanner_id, uart, esp_err_to_name(err),
                 (unsigned long)guard->request_generation,
                 (unsigned long)request_generation,
                 (unsigned long)ack_generation);
        return false;
    }
    return true;
}

void uart_rx_resume_scanner_guarded(int scanner_id,
                                    uart_rx_pause_guard_t *guard)
{
    atomic_uint_fast32_t *request = pause_request_for_scanner(scanner_id);
    if (!request || !guard || !guard->acquired ||
        guard->request_generation == 0U) {
        return;
    }
    uint_fast32_t expected = guard->request_generation;
    bool cleared = atomic_compare_exchange_strong_explicit(
        request, &expected, 0U,
        memory_order_acq_rel, memory_order_acquire);
    ESP_LOGW(TAG, "UART RX resume for scanner %d gen=%lu: %s",
             scanner_id, (unsigned long)guard->request_generation,
             cleared ? "released" : "not_owner");
    memset(guard, 0, sizeof(*guard));
}

bool uart_rx_pause_scanner(int scanner_id)
{
    if (scanner_id < 0 || scanner_id > 1) {
        return false;
    }
    return uart_rx_pause_scanner_guarded(
        scanner_id, &s_legacy_pause_guards[scanner_id]);
}

void uart_rx_resume_scanner(int scanner_id)
{
    if (scanner_id < 0 || scanner_id > 1) {
        return;
    }
    uart_rx_resume_scanner_guarded(
        scanner_id, &s_legacy_pause_guards[scanner_id]);
}
