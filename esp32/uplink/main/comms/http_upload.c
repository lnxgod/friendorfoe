/**
 * Friend or Foe -- Uplink HTTP Upload Implementation
 *
 * Collects drone detections from a FreeRTOS queue, batches them into
 * JSON payloads, and POSTs to the FastAPI backend.  When WiFi is down,
 * batches are buffered in a ring buffer and drained on reconnect.
 */

#include "http_upload.h"
#include "uart_rx.h"
#include "uart_protocol.h"
#include "wifi_sta.h"
#include "ring_buffer.h"
#include "nvs_config.h"
#include "config.h"
#include "gps.h"
#include "time_sync.h"
#include "time_sync_policy.h"
#include "version.h"
#include "detection_policy.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"

static const char *TAG = "http_up";

static QueueHandle_t   s_detection_queue   = NULL;
static ring_buffer_t  *s_offline_buffer    = NULL;
static int             s_success_count     = 0;
static int             s_fail_count        = 0;
static int64_t         s_last_success_epoch_ms = 0;
static uint32_t        s_node_dedup_seen = 0;
static uint32_t        s_node_dedup_sent = 0;
static uint32_t        s_node_dedup_collapsed = 0;
static uint32_t        s_cal_seen = 0;
static uint32_t        s_cal_sent = 0;

/* Persistent HTTP client handle (avoids socket exhaustion from rapid open/close) */
/* esp_http_client removed — using raw sockets for zero heap allocation */

/* Pause flag — set during firmware relay to free heap */
static volatile bool s_upload_paused = false;

/* Time-sync diagnostic globals (v0.60+). Surfaced via /api/status →
 * "time_sync":{"last_fetch_ms": N, "last_perf": "ESP_OK", "last_status": 200,
 * "last_nread": 20} so we can see what the uplink saw when fetching
 * /detections/time. Without this, debugging the broadcast pipeline requires
 * serial console access which the deployed nodes don't have. */
volatile int64_t g_last_backend_epoch_ms = 0;
volatile int     g_last_time_fetch_perf  = -999;  /* esp_err_t-like */
volatile int     g_last_time_fetch_status = 0;
volatile int     g_last_time_fetch_nread = 0;
volatile int     g_last_time_fetch_clen  = -2;
volatile uint32_t g_time_broadcast_count = 0;
volatile bool    g_last_time_fetch_ok = false;
volatile int64_t g_last_time_attempt_monotonic_ms = 0;
volatile int64_t g_last_time_success_monotonic_ms = 0;
volatile uint32_t g_time_fetch_ok_count = 0;
volatile uint32_t g_time_fetch_fail_count = 0;
volatile uint32_t g_time_fetch_fail_streak = 0;
volatile int64_t g_last_broadcast_epoch_ms = 0;
volatile uint32_t g_time_broadcast_valid_count = 0;
volatile uint32_t g_time_broadcast_invalid_count = 0;
volatile int g_time_source_mode = 0;
char g_last_time_fetch_url_source[16] = "none";
char g_last_time_fetch_url[96] = "";
char g_last_time_fetch_ip[16] = "";
volatile int g_last_time_fetch_port = 0;

#define TIME_SOURCE_NONE     0
#define TIME_SOURCE_BACKEND  1
#define TIME_SOURCE_SNTP     2
#define TIME_SOURCE_LOCAL    3

/* Maximum JSON payload size for a batch */
#define MAX_PAYLOAD_SIZE    4096

/* Retry config — keep total blocking time well under 30s WDT timeout */
#define HTTP_TIMEOUT_MS     5000    /* 5s per attempt — 3s caused EAGAIN on slow WiFi */
#define MAX_RETRIES         1       /* 1 retry max — fail fast, try next batch */
#define BACKOFF_BASE_MS     500     /* 500ms base (was 1000) */
#define MAX_DRAIN_PER_CYCLE 1       /* drain 1 offline batch per loop — don't block */
#define HEALTH_RESET_SEC    30      /* force reset client if no success for 30s */
#define NODE_DEDUP_BUCKET_MS 500     /* collapse duplicate dual-slot captures */

/* ── Source integer to string mapping ──────────────────────────────────── */

static const char *source_to_string(uint8_t src)
{
    switch (src) {
        case DETECTION_SRC_BLE_RID:            return "ble_rid";
        case DETECTION_SRC_BLE_FINGERPRINT:    return "ble_fingerprint";
        case DETECTION_SRC_WIFI_SSID:          return "wifi_ssid";
        case DETECTION_SRC_WIFI_DJI_IE:        return "wifi_dji_ie";
        case DETECTION_SRC_WIFI_BEACON:        return "wifi_beacon_rid";
        case DETECTION_SRC_WIFI_OUI:           return "wifi_oui";
        case DETECTION_SRC_WIFI_PROBE_REQUEST: return "wifi_probe_request";
        case DETECTION_SRC_WIFI_ASSOC:         return "wifi_assoc";
        case DETECTION_SRC_WIFI_AP_INVENTORY:  return "wifi_ap_inventory";
        default:                               return "unknown";
    }
}

static const char *time_source_to_string(int source_mode)
{
    switch (source_mode) {
        case TIME_SOURCE_BACKEND: return "backend";
        case TIME_SOURCE_SNTP:    return "sntp";
        case TIME_SOURCE_LOCAL:   return "local";
        default:                  return "none";
    }
}

static int64_t monotonic_age_s(int64_t since_ms)
{
    if (since_ms <= 0) {
        return -1;
    }
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms <= since_ms) {
        return 0;
    }
    return (now_ms - since_ms) / 1000;
}

static bool is_calibration_detection(const drone_detection_t *det)
{
    if (!uart_rx_is_node_calibration_mode() ||
        !det || det->source != DETECTION_SRC_BLE_FINGERPRINT) {
        return false;
    }
    const char *uuid = uart_rx_get_node_calibration_uuid();
    if (!uuid || uuid[0] == '\0') {
        return false;
    }
    return fof_policy_ble_svc_raw_contains_uuid(det->ble_svc_uuids_raw, uuid) ||
           fof_policy_ble_has_exact_uuid128_le(
               det->ble_service_uuids_128,
               det->ble_svc_uuid_128_count,
               uuid
           );
}

static int64_t detection_timestamp_ms(const drone_detection_t *det)
{
    if (det && det->last_updated_ms > 0) {
        return det->last_updated_ms;
    }
    return time_sync_get_epoch_ms();
}

static int find_duplicate_in_batch(const drone_detection_t *batch,
                                   int count,
                                   const drone_detection_t *det)
{
    char key[224];
    if (!fof_policy_detection_dedupe_key(
            det,
            detection_timestamp_ms(det),
            NODE_DEDUP_BUCKET_MS,
            key,
            sizeof(key))) {
        return -1;
    }

    for (int i = 0; i < count; i++) {
        char existing_key[224];
        if (!fof_policy_detection_dedupe_key(
                &batch[i],
                detection_timestamp_ms(&batch[i]),
                NODE_DEDUP_BUCKET_MS,
                existing_key,
                sizeof(existing_key))) {
            continue;
        }
        if (strcmp(key, existing_key) == 0) {
            return i;
        }
    }
    return -1;
}

static void merge_duplicate_detection(drone_detection_t *existing,
                                      const drone_detection_t *candidate)
{
    uint8_t slots = existing->scanner_slots_seen | candidate->scanner_slots_seen;
    char merged_ssids[sizeof(existing->probed_ssids)] = {0};
    if (existing->probed_ssids[0]) {
        strncpy(merged_ssids, existing->probed_ssids, sizeof(merged_ssids) - 1);
    }
    if (candidate->probed_ssids[0]) {
        char incoming[sizeof(candidate->probed_ssids)];
        strncpy(incoming, candidate->probed_ssids, sizeof(incoming) - 1);
        incoming[sizeof(incoming) - 1] = '\0';
        char *tok = incoming;
        while (tok && *tok) {
            char *comma = strchr(tok, ',');
            if (comma) {
                *comma = '\0';
            }
            if (tok[0] && !strstr(merged_ssids, tok)) {
                size_t used = strlen(merged_ssids);
                size_t left = sizeof(merged_ssids) - used - 1;
                if (left > strlen(tok) + (used ? 1U : 0U)) {
                    if (used) {
                        strncat(merged_ssids, ",", sizeof(merged_ssids) - strlen(merged_ssids) - 1);
                    }
                    strncat(merged_ssids, tok, sizeof(merged_ssids) - strlen(merged_ssids) - 1);
                }
            }
            tok = comma ? comma + 1 : NULL;
        }
    }
    if (candidate->rssi > existing->rssi) {
        *existing = *candidate;
    }
    existing->scanner_slots_seen = slots;
    if (merged_ssids[0]) {
        strncpy(existing->probed_ssids, merged_ssids, sizeof(existing->probed_ssids) - 1);
        existing->probed_ssids[sizeof(existing->probed_ssids) - 1] = '\0';
    }
}

static size_t estimate_detection_json_size(const drone_detection_t *det)
{
    size_t estimate = 120; /* fixed keys + punctuation */

    if (det->drone_id[0] != '\0') estimate += strlen(det->drone_id);
    if (det->manufacturer[0] != '\0') estimate += strlen(det->manufacturer);
    if (det->model[0] != '\0') estimate += strlen(det->model);
    if (det->ssid[0] != '\0') estimate += strlen(det->ssid);
    if (det->bssid[0] != '\0') estimate += strlen(det->bssid);
    if (det->operator_id[0] != '\0') estimate += strlen(det->operator_id);
    if (det->probed_ssids[0] != '\0') estimate += strlen(det->probed_ssids) + 16;
    if (det->ble_name[0] != '\0') estimate += strlen(det->ble_name);
    if (det->class_reason[0] != '\0') estimate += strlen(det->class_reason);

    if (det->latitude != 0.0 || det->longitude != 0.0) estimate += 32;
    if (det->operator_lat != 0.0 || det->operator_lon != 0.0) estimate += 32;
    if (det->altitude_m != 0.0) estimate += 12;
    if (det->speed_mps != 0.0) estimate += 10;
    if (det->heading_deg != 0.0) estimate += 10;
    if (det->last_updated_ms > 0) estimate += 18;
    return estimate;
}

/* ── Build JSON payload from a batch of detections ─────────────────────── */

/* Payload buffer for building detection batch JSON.
 *
 * On S3 (N16R8) this is a 64 KB PSRAM-backed buffer, allowing larger batches
 * per HTTP round-trip. If PSRAM is unavailable, the static internal-RAM buffer
 * keeps upload behavior deterministic instead of heap-fragmenting.
 *
 * The buffer pointer and size are set once in http_upload_init() and never
 * reallocated, matching the original "ZERO heap allocation in hot path"
 * design of the raw-socket HTTP uploader. */
#include "psram_alloc.h"

#define PAYLOAD_BUF_FALLBACK_SIZE 4096
#define PAYLOAD_BUF_PSRAM_SIZE    (64 * 1024)

static char  s_payload_buf_fallback[PAYLOAD_BUF_FALLBACK_SIZE];
static char *s_payload_buf      = s_payload_buf_fallback;
static int   s_payload_buf_size = PAYLOAD_BUF_FALLBACK_SIZE;

/* Helper: append to buffer with bounds checking (size now runtime-determined) */
#define BUF_APPEND(fmt, ...) do { \
    int _n = snprintf(&s_payload_buf[off], s_payload_buf_size - off, fmt, ##__VA_ARGS__); \
    if (_n > 0 && off + _n < s_payload_buf_size) off += _n; \
} while(0)

static int append_json_escaped_value(int off, const char *value)
{
    if (!value) {
        value = "";
    }
    for (const unsigned char *p = (const unsigned char *)value;
         *p && off < s_payload_buf_size - 1; ++p) {
        unsigned char ch = *p;
        if (ch == '"' || ch == '\\') {
            if (off < s_payload_buf_size - 2) {
                s_payload_buf[off++] = '\\';
                s_payload_buf[off++] = (char)ch;
            }
        } else if (ch >= 0x20) {
            s_payload_buf[off++] = (char)ch;
        }
    }
    if (off < s_payload_buf_size) {
        s_payload_buf[off] = '\0';
    }
    return off;
}

static int append_json_string_field(int off, const char *key, const char *value)
{
    if (!value || value[0] == '\0') {
        return off;
    }
    int n = snprintf(&s_payload_buf[off], s_payload_buf_size - off,
                     ",\"%s\":\"", key);
    if (n <= 0 || off + n >= s_payload_buf_size) {
        return off;
    }
    off += n;
    off = append_json_escaped_value(off, value);
    if (off < s_payload_buf_size - 1) {
        s_payload_buf[off++] = '"';
        s_payload_buf[off] = '\0';
    }
    return off;
}

static int append_json_csv_array_field(int off,
                                       const char *key,
                                       const char *csv,
                                       const char *fallback)
{
    const char *src = (csv && csv[0]) ? csv : fallback;
    if (!src || src[0] == '\0') {
        return off;
    }

    int n = snprintf(&s_payload_buf[off], s_payload_buf_size - off,
                     ",\"%s\":[", key);
    if (n <= 0 || off + n >= s_payload_buf_size) {
        return off;
    }
    off += n;

    bool emitted = false;
    char token[33];
    size_t pos = 0;
    for (const char *p = src;; ++p) {
        if (*p == ',' || *p == '\0') {
            token[pos] = '\0';
            if (pos > 0) {
                if (emitted && off < s_payload_buf_size - 1) {
                    s_payload_buf[off++] = ',';
                }
                if (off < s_payload_buf_size - 1) {
                    s_payload_buf[off++] = '"';
                }
                off = append_json_escaped_value(off, token);
                if (off < s_payload_buf_size - 1) {
                    s_payload_buf[off++] = '"';
                    s_payload_buf[off] = '\0';
                }
                emitted = true;
            }
            pos = 0;
            if (*p == '\0') {
                break;
            }
            continue;
        }
        if (pos < sizeof(token) - 1) {
            token[pos++] = *p;
        }
    }

    if (off < s_payload_buf_size - 1) {
        s_payload_buf[off++] = ']';
        s_payload_buf[off] = '\0';
    }
    return off;
}

/* Helper: append scanner info object */
static int append_scanner_info(char *buf, int off, int max, const char *uart_name, const scanner_info_t *info)
{
    uint8_t scanner_id = (strcmp(uart_name, "wifi") == 0) ? 1 : 0;
    bool scanner_calibration = strcmp(info->scan_mode, "calibration") == 0;
    int n = snprintf(&buf[off], max - off,
        "{\"uart\":\"%s\",\"ver\":\"%s\",\"board\":\"%s\",\"chip\":\"%s\",\"caps\":\"%s\""
        ",\"scan_profile\":\"%s\",\"slot_role\":\"%s\"",
        uart_name, info->version, info->board, info->chip, info->caps,
        fof_policy_scan_profile_for_slot(scanner_id, scanner_calibration),
        fof_policy_slot_role_for_slot(scanner_id));
    if (n > 0) off += n;
    if (info->auth_count > 0) { n = snprintf(&buf[off], max-off, ",\"auth_fr\":%d", info->auth_count); if(n>0) off+=n; }
    if (info->fc_hist[0]) { n = snprintf(&buf[off], max-off, ",\"fc_hist\":\"%s\"", info->fc_hist); if(n>0) off+=n; }
    /* WiFi attack stats (v0.60+): scanner detects deauth/disassoc/auth-frame
     * floods + beacon-spam (Pwnagotchi-style fake APs). Always emitted so
     * the backend can show 0 vs absent — see rf_anomaly.py. */
    n = snprintf(&buf[off], max-off, ",\"deauth\":%d,\"disassoc\":%d,\"flood\":%s,\"bcn_spam\":%s",
                 info->deauth_count, info->disassoc_count,
                 info->deauth_flood ? "true" : "false",
                 info->beacon_spam ? "true" : "false");
    if (n > 0) off += n;
    n = snprintf(&buf[off], max-off,
                 ",\"uart_tx_dropped\":%lu,\"uart_tx_high_water\":%lu"
                 ",\"tx_queue_depth\":%lu,\"tx_queue_capacity\":%lu,\"tx_queue_pressure_pct\":%lu"
                 ",\"noise_drop_ble\":%lu,\"noise_drop_wifi\":%lu"
                 ",\"probe_seen\":%lu,\"probe_sent\":%lu"
                 ",\"probe_drop_low_value\":%lu,\"probe_drop_rate_limit\":%lu,\"probe_drop_pressure\":%lu",
                 (unsigned long)info->uart_tx_dropped,
                 (unsigned long)info->uart_tx_high_water,
                 (unsigned long)info->tx_queue_depth,
                 (unsigned long)info->tx_queue_capacity,
                 (unsigned long)info->tx_queue_pressure_pct,
                 (unsigned long)info->noise_drop_ble,
                 (unsigned long)info->noise_drop_wifi,
                 (unsigned long)info->probe_seen,
                 (unsigned long)info->probe_sent,
                 (unsigned long)info->probe_drop_low_value,
                 (unsigned long)info->probe_drop_rate_limit,
                 (unsigned long)info->probe_drop_pressure);
    if (n > 0) off += n;
    /* v0.60 time-sync diagnostic — surfaces whether scanner has received the
     * uplink's epoch broadcast. tcnt = #broadcasts seen, toff = applied
     * offset (0 means none usable). Visible via /detections/nodes/status. */
    n = snprintf(&buf[off], max-off,
                 ",\"toff\":%lld,\"tcnt\":%u,\"time_valid_count\":%lu"
                 ",\"time_last_valid_age_s\":%lld,\"time_sync_state\":\"%s\""
                 ",\"scan_mode\":\"%s\",\"calibration_uuid\":\"%s\",\"calibration_mode_acked\":%s",
                 (long long)info->toff_ms, info->tcnt,
                 (unsigned long)info->time_valid_count,
                 (long long)info->time_last_valid_age_s,
                 info->time_sync_state[0] ? info->time_sync_state : "unknown",
                 info->scan_mode[0] ? info->scan_mode : "normal",
                 info->calibration_uuid,
                 info->calibration_mode_acked ? "true" : "false");
    if(n>0) off+=n;
    n = snprintf(&buf[off], max-off, "}"); if(n>0) off+=n;
    return off;
}

static char *build_payload(const drone_detection_t *batch, int count, int64_t scan_ts_ms)
{
    char device_id[32] = {0};
    nvs_config_get_device_id(device_id, sizeof(device_id));
    gps_position_t gps_pos = {0};
    gps_get_position(&gps_pos);
    int64_t ts_ms = (scan_ts_ms > 0) ? scan_ts_ms : time_sync_get_epoch_ms();
    const esp_app_desc_t *app = esp_app_get_description();
    const char *wifi_ssid = wifi_sta_get_ssid();

    int off = 0;

    /* Header */
    BUF_APPEND("{\"device_id\":\"%s\",\"device_lat\":%.6f,\"device_lon\":%.6f,\"device_alt\":%.1f,\"timestamp\":%lld",
               device_id, gps_pos.latitude, gps_pos.longitude, gps_pos.altitude_m, (long long)(ts_ms / 1000));
    BUF_APPEND(",\"firmware_version\":\"%s\"", FOF_VERSION);
    BUF_APPEND(",\"board_type\":\"uplink-s3\"");
    if (wifi_ssid[0]) BUF_APPEND(",\"wifi_ssid\":\"%s\",\"wifi_rssi\":%d", wifi_ssid, wifi_sta_get_rssi());
    BUF_APPEND(",\"scan_mode\":\"%s\",\"calibration_uuid\":\"%s\"",
               uart_rx_get_node_scan_mode(),
               uart_rx_get_node_calibration_uuid());
    BUF_APPEND(
        ",\"scan_profile\":\"%s\",\"dedup_seen\":%lu,\"dedup_sent\":%lu"
        ",\"dedup_collapsed\":%lu,\"cal_seen\":%lu,\"cal_sent\":%lu",
        uart_rx_is_node_calibration_mode() ? "calibration" : "normal",
        (unsigned long)s_node_dedup_seen,
        (unsigned long)s_node_dedup_sent,
        (unsigned long)s_node_dedup_collapsed,
        (unsigned long)s_cal_seen,
        (unsigned long)s_cal_sent
    );
    BUF_APPEND(
        ",\"time_sync\":{\"time_source\":\"%s\",\"last_fetch_ok\":%s"
        ",\"last_fetch_url_source\":\"%s\",\"last_fetch_url\":\"%s\""
        ",\"last_fetch_ip\":\"%s\",\"last_fetch_port\":%d"
        ",\"last_attempt_age_s\":%lld,\"last_success_age_s\":%lld"
        ",\"fetch_ok_count\":%lu,\"fetch_fail_count\":%lu,\"fetch_fail_streak\":%lu"
        ",\"last_backend_epoch_ms\":%lld,\"last_broadcast_epoch_ms\":%lld"
        ",\"broadcast_valid_count\":%lu,\"broadcast_invalid_count\":%lu}",
        time_source_to_string(g_time_source_mode),
        g_last_time_fetch_ok ? "true" : "false",
        g_last_time_fetch_url_source,
        g_last_time_fetch_url,
        g_last_time_fetch_ip,
        g_last_time_fetch_port,
        (long long)monotonic_age_s(g_last_time_attempt_monotonic_ms),
        (long long)monotonic_age_s(g_last_time_success_monotonic_ms),
        (unsigned long)g_time_fetch_ok_count,
        (unsigned long)g_time_fetch_fail_count,
        (unsigned long)g_time_fetch_fail_streak,
        (long long)g_last_backend_epoch_ms,
        (long long)g_last_broadcast_epoch_ms,
        (unsigned long)g_time_broadcast_valid_count,
        (unsigned long)g_time_broadcast_invalid_count
    );

    /* Scanners array */
    BUF_APPEND(",\"scanners\":[");
    bool has_scanner = false;
    const scanner_info_t *ble_info = uart_rx_get_ble_scanner_info();
    if (ble_info) {
        off = append_scanner_info(s_payload_buf, off, s_payload_buf_size, "ble", ble_info);
        has_scanner = true;
    }
#if CONFIG_DUAL_SCANNER
    const scanner_info_t *wifi_info = uart_rx_get_wifi_scanner_info();
    if (wifi_info) {
        if (has_scanner) BUF_APPEND(",");
        off = append_scanner_info(s_payload_buf, off, s_payload_buf_size, "wifi", wifi_info);
    }
#endif
    BUF_APPEND("]");

    /* Detections array */
    BUF_APPEND(",\"detections\":[");
    for (int i = 0; i < count; i++) {
        const drone_detection_t *d = &batch[i];
        if (i > 0) BUF_APPEND(",");

        BUF_APPEND("{\"drone_id\":\"");
        off = append_json_escaped_value(off, d->drone_id);
        BUF_APPEND("\",\"source\":\"%s\",\"confidence\":%.8f,\"rssi\":%d",
                   source_to_string(d->source), d->confidence, d->rssi);
        BUF_APPEND(",\"scanner_slot\":%u,\"scanner_slots_seen\":%u",
                   d->scanner_slot, d->scanner_slots_seen);
        if (d->estimated_distance_m > 0.0)
            BUF_APPEND(",\"estimated_distance_m\":%.6f", d->estimated_distance_m);
        if (d->latitude != 0.0 || d->longitude != 0.0)
            BUF_APPEND(",\"latitude\":%.7f,\"longitude\":%.7f", d->latitude, d->longitude);
        if (d->altitude_m != 0.0) BUF_APPEND(",\"altitude_m\":%.1f", d->altitude_m);
        off = append_json_string_field(off, "manufacturer", d->manufacturer);
        off = append_json_string_field(off, "model", d->model);
        off = append_json_string_field(off, "ssid", d->ssid);
        off = append_json_string_field(off, "bssid", d->bssid);
        if (d->freq_mhz != 0) BUF_APPEND(",\"channel\":%d", d->freq_mhz);
        if (d->wifi_auth_mode != 0xFF) BUF_APPEND(",\"auth_m\":%d", d->wifi_auth_mode);
        if (d->operator_lat != 0.0 || d->operator_lon != 0.0)
            BUF_APPEND(",\"operator_lat\":%.7f,\"operator_lon\":%.7f", d->operator_lat, d->operator_lon);
        off = append_json_string_field(off, "operator_id", d->operator_id);
        /* Per-detection scan timestamp (epoch-ms when scanner is time-synced
         * with uplink, v0.60+). Backend uses this instead of batch-level
         * timestamp for cross-node correlation — see triangulation.py. */
        if (d->last_updated_ms > 0)
            BUF_APPEND(",\"timestamp\":%lld", (long long)d->last_updated_ms);

        /* Probe request SSIDs */
        if (d->source == DETECTION_SRC_WIFI_PROBE_REQUEST) {
            off = append_json_csv_array_field(
                off,
                "probed_ssids",
                d->probed_ssids,
                d->ssid
            );
        }

        /* BLE fingerprinting fields (only non-zero) */
        if (d->ble_company_id) BUF_APPEND(",\"ble_company_id\":%u", d->ble_company_id);
        if (d->ble_apple_type) BUF_APPEND(",\"ble_apple_type\":%u", d->ble_apple_type);
        if (d->ble_ad_type_count) BUF_APPEND(",\"ble_ad_type_count\":%u", d->ble_ad_type_count);
        if (d->ble_payload_len) BUF_APPEND(",\"ble_payload_len\":%u", d->ble_payload_len);
        if (d->ble_addr_type) BUF_APPEND(",\"ble_addr_type\":%u", d->ble_addr_type);
        if (d->ble_ja3_hash) BUF_APPEND(",\"ble_ja3\":\"%08lx\"", (unsigned long)d->ble_ja3_hash);
        off = append_json_string_field(off, "ble_name", d->ble_name);
        off = append_json_string_field(off, "class_reason", d->class_reason);
        if (d->ble_apple_auth[0] || d->ble_apple_auth[1] || d->ble_apple_auth[2])
            BUF_APPEND(",\"ble_apple_auth\":\"%02x%02x%02x\"", d->ble_apple_auth[0], d->ble_apple_auth[1], d->ble_apple_auth[2]);
        if (d->ble_apple_activity) BUF_APPEND(",\"ble_activity\":%u", d->ble_apple_activity);
        /* Apple Nearby Info data-flags byte — always emitted (even when 0) so
         * the backend can distinguish "all flags false" from "field absent". */
        BUF_APPEND(",\"ble_apple_flags\":%u", d->ble_apple_flags);
        if (d->probe_ie_hash) BUF_APPEND(",\"ie_hash\":\"%08lx\"", (unsigned long)d->probe_ie_hash);
        if (d->ble_raw_mfr_len > 0) {
            BUF_APPEND(",\"ble_raw_mfr\":\"");
            for (int j = 0; j < d->ble_raw_mfr_len && j < 20; j++)
                BUF_APPEND("%02x", d->ble_raw_mfr[j]);
            BUF_APPEND("\"");
        }
        /* Prefer the pass-through raw string so 128-bit UUIDs survive intact
         * ("cafe9a86-0000-1000-8000-..."). Re-encode the uint16 mirror only
         * when a local detection did not populate the raw field. */
        if (d->ble_svc_uuids_raw[0] != '\0') {
            off = append_json_string_field(off, "ble_svc_uuids", d->ble_svc_uuids_raw);
        } else if (d->ble_svc_uuid_count > 0) {
            BUF_APPEND(",\"ble_svc_uuids\":\"");
            for (int j = 0; j < d->ble_svc_uuid_count && j < 4; j++) {
                if (j > 0) BUF_APPEND(",");
                BUF_APPEND("%04x", d->ble_service_uuids[j]);
            }
            BUF_APPEND("\"");
        }

        BUF_APPEND("}");

        /* Safety: stop if buffer nearly full */
        if (off > s_payload_buf_size - 200) break;
    }
    BUF_APPEND("]}");

    return s_payload_buf;
}

/* ── HTTP POST with retry ──────────────────────────────────────────────── */

/**
 * Ensure the persistent HTTP client is initialized.
 * Reuses the same TCP connection (keep-alive) to avoid socket exhaustion.
 */
static bool s_using_fallback_url = false;
static char s_resolved_url[256] = {0};  /* Cached resolved URL (IP instead of hostname) */

/**
 * Resolve mDNS/hostname to IP and cache it.
 * Falls back to original URL if resolution fails.
 */
static void resolve_and_cache_url(const char *backend_url)
{
    /* Already resolved? */
    if (s_resolved_url[0]) return;

    /* Extract hostname from URL like "http://fof-server.local:8000" */
    const char *host_start = strstr(backend_url, "://");
    if (!host_start) {
        snprintf(s_resolved_url, sizeof(s_resolved_url), "%s%s", backend_url, CONFIG_UPLOAD_ENDPOINT);
        return;
    }
    host_start += 3;

    char hostname[64] = {0};
    const char *port_start = strchr(host_start, ':');
    const char *path_start = strchr(host_start, '/');
    int host_len;
    if (port_start && (!path_start || port_start < path_start))
        host_len = port_start - host_start;
    else if (path_start)
        host_len = path_start - host_start;
    else
        host_len = strlen(host_start);
    if (host_len >= (int)sizeof(hostname)) host_len = sizeof(hostname) - 1;
    memcpy(hostname, host_start, host_len);

    /* Try DNS/mDNS resolution */
    struct addrinfo hints = { .ai_family = AF_INET };
    struct addrinfo *res = NULL;
    int err = getaddrinfo(hostname, NULL, &hints, &res);
    if (err == 0 && res) {
        struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
        char ip_str[16];
        inet_ntoa_r(addr->sin_addr, ip_str, sizeof(ip_str));
        freeaddrinfo(res);

        /* Build URL with resolved IP instead of hostname */
        const char *after_host = host_start + host_len;  /* ":8000/..." */
        snprintf(s_resolved_url, sizeof(s_resolved_url), "http://%s%s%s",
                 ip_str, after_host, CONFIG_UPLOAD_ENDPOINT);
        ESP_LOGW(TAG, "Resolved %s → %s (cached)", hostname, ip_str);
    } else {
        /* Resolution failed — use original URL */
        snprintf(s_resolved_url, sizeof(s_resolved_url), "%s%s", backend_url, CONFIG_UPLOAD_ENDPOINT);
        ESP_LOGW(TAG, "mDNS resolve failed for %s — using as-is", hostname);
        s_resolved_url[0] = '\0';  /* Don't cache failure — retry next time */
    }
}

/* Dead code — replaced by raw_socket_connect() above */
#if 0
static bool ensure_http_client_UNUSED(void)
{
    if (s_http_client) {
        return true;
    }

    char backend_url[128] = {0};
    if (s_using_fallback_url) {
        strncpy(backend_url, CONFIG_BACKEND_URL_FALLBACK, sizeof(backend_url) - 1);
        ESP_LOGI(TAG, "Using fallback backend URL: %s", backend_url);
    } else {
        nvs_config_get_backend_url(backend_url, sizeof(backend_url));
    }

    /* Resolve hostname to IP and cache */
    resolve_and_cache_url(backend_url);

    char url[256];
    if (s_resolved_url[0]) {
        strncpy(url, s_resolved_url, sizeof(url) - 1);
    } else {
        snprintf(url, sizeof(url), "%s%s", backend_url, CONFIG_UPLOAD_ENDPOINT);
    }

    esp_http_client_config_t config = {
        .url              = url,
        .method           = HTTP_METHOD_POST,
        .timeout_ms       = HTTP_TIMEOUT_MS,
        .buffer_size      = 1024,          /* Must fit HTTP response headers from uvicorn */
        .keep_alive_enable = true,         /* Reuse TCP connection to avoid alloc churn */
    };

    s_http_client = esp_http_client_init(&config);
    if (!s_http_client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return false;
    }

    esp_http_client_set_header(s_http_client, "Content-Type", "application/json");
    ESP_LOGI(TAG, "HTTP client created: %s (timeout=%dms, keep-alive)", url, HTTP_TIMEOUT_MS);
    return true;
}

/**
 * Destroy and recreate the HTTP client (call after persistent errors).
 */
static void reset_http_client(void)
{
    raw_socket_close();
}
#endif  /* dead code */

/* ── Raw socket HTTP POST — ZERO heap allocation ─────────────────────────
 * Uses a persistent TCP socket with static buffers instead of esp_http_client
 * in the hot path. This keeps upload memory behavior predictable under load.
 */

static int s_sock = -1;
static char s_http_header[256];  /* Static buffer for HTTP request header */
static char s_http_resp[256];    /* Static buffer for HTTP response */
static struct sockaddr_in s_cached_addr = {0};  /* Cached resolved address — avoid getaddrinfo heap alloc */
static bool s_addr_cached = false;
static char s_cached_backend_url[128] = {0};

static void raw_socket_forget_endpoint_cache(void)
{
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    memset(&s_cached_addr, 0, sizeof(s_cached_addr));
    s_addr_cached = false;
    s_cached_backend_url[0] = '\0';
    s_resolved_url[0] = '\0';
}

static bool parse_backend_host_port(const char *backend_url,
                                    char *host,
                                    size_t host_size,
                                    int *port_out)
{
    if (!backend_url || !host || host_size == 0 || !port_out) {
        return false;
    }
    const char *host_start = strstr(backend_url, "://");
    host_start = host_start ? host_start + 3 : backend_url;
    const char *host_end = host_start;
    while (*host_end && *host_end != ':' && *host_end != '/') {
        host_end++;
    }
    size_t host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= host_size) {
        return false;
    }
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    *port_out = 80;
    if (*host_end == ':') {
        *port_out = atoi(host_end + 1);
    }
    return true;
}

static void remember_time_fetch_endpoint(const char *url_source,
                                         const char *backend_url,
                                         const char *ip,
                                         int port)
{
    strncpy(g_last_time_fetch_url_source,
            url_source ? url_source : "unknown",
            sizeof(g_last_time_fetch_url_source) - 1);
    g_last_time_fetch_url_source[sizeof(g_last_time_fetch_url_source) - 1] = '\0';
    strncpy(g_last_time_fetch_url,
            backend_url ? backend_url : "",
            sizeof(g_last_time_fetch_url) - 1);
    g_last_time_fetch_url[sizeof(g_last_time_fetch_url) - 1] = '\0';
    strncpy(g_last_time_fetch_ip,
            ip ? ip : "",
            sizeof(g_last_time_fetch_ip) - 1);
    g_last_time_fetch_ip[sizeof(g_last_time_fetch_ip) - 1] = '\0';
    g_last_time_fetch_port = port;
}

static bool backend_urls_equal(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }
    return strcmp(a, b) == 0;
}

static bool fetch_backend_time_epoch_ms_single(const char *backend_url,
                                               const char *url_source,
                                               int64_t *epoch_ms_out)
{
    char host[64] = {0};
    int port = 80;
    int sock = -1;
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port_str[8];
    char request[192];
    char response[256];
    int status = 0;
    int clen = -1;
    int nread = 0;
    int perf = -1;
    int64_t parsed_epoch_ms = 0;

    if (!parse_backend_host_port(backend_url, host, sizeof(host), &port)) {
        perf = -1;
        remember_time_fetch_endpoint(url_source, backend_url, "", 0);
        goto done;
    }
    remember_time_fetch_endpoint(url_source, backend_url, "", port);
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        perf = -2;
        goto done;
    }

    bool connected = false;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0) {
            perf = -3;
            continue;
        }

        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(sock, ai->ai_addr, ai->ai_addrlen) == 0) {
            if (ai->ai_family == AF_INET) {
                char ip_str[16] = {0};
                struct sockaddr_in *addr = (struct sockaddr_in *)ai->ai_addr;
                inet_ntoa_r(addr->sin_addr, ip_str, sizeof(ip_str));
                remember_time_fetch_endpoint(url_source, backend_url, ip_str, port);
            }
            connected = true;
            break;
        }

        perf = -4;
        close(sock);
        sock = -1;
    }
    if (!connected) {
        goto done;
    }

    char host_header[80];
    if (port == 80) {
        snprintf(host_header, sizeof(host_header), "%s", host);
    } else {
        snprintf(host_header, sizeof(host_header), "%s:%d", host, port);
    }
    int req_len = snprintf(
        request, sizeof(request),
        "GET /detections/time HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
        host_header
    );
    if (send(sock, request, req_len, 0) != req_len) {
        perf = -5;
        goto done;
    }

    int total = 0;
    while (total < (int)sizeof(response) - 1) {
        int got = recv(sock, response + total, sizeof(response) - 1 - total, 0);
        if (got <= 0) {
            break;
        }
        total += got;
        response[total] = '\0';
        char *body_probe = strstr(response, "\r\n\r\n");
        if (body_probe) {
            char *cl_probe = strstr(response, "Content-Length:");
            if (!cl_probe) cl_probe = strstr(response, "content-length:");
            if (cl_probe) {
                int expected_clen = atoi(cl_probe + 15);
                int header_len = (int)((body_probe + 4) - response);
                if (expected_clen >= 0 && total >= header_len + expected_clen) {
                    break;
                }
            } else {
                break;
            }
        }
    }
    if (total <= 0) {
        perf = -6;
        goto done;
    }
    response[total] = '\0';
    nread = total;

    if (total > 12 && strncmp(response, "HTTP/", 5) == 0) {
        status = atoi(response + 9);
    }
    char *cl = strstr(response, "Content-Length:");
    if (!cl) cl = strstr(response, "content-length:");
    if (cl) {
        clen = atoi(cl + 15);
    }
    char *body = strstr(response, "\r\n\r\n");
    if (!body) {
        perf = -7;
        goto done;
    }
    body += 4;
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        perf = -8;
        goto done;
    }
    cJSON *ms = cJSON_GetObjectItem(root, "ms");
    if (ms && cJSON_IsNumber(ms)) {
        parsed_epoch_ms = (int64_t)ms->valuedouble;
    }
    cJSON_Delete(root);
    perf = 0;

done:
    g_last_time_fetch_perf = perf;
    g_last_time_fetch_status = status;
    g_last_time_fetch_clen = clen;
    g_last_time_fetch_nread = nread;
    if (res) {
        freeaddrinfo(res);
    }
    if (sock >= 0) {
        close(sock);
    }
    if (epoch_ms_out) {
        *epoch_ms_out = parsed_epoch_ms;
    }
    return perf == 0 && status >= 200 && status < 300 && fof_time_epoch_is_valid(parsed_epoch_ms);
}

static bool fetch_backend_time_epoch_ms(int64_t *epoch_ms_out)
{
    char primary_url[96] = {0};
    char fallback_url[96] = {0};
    const char *first_source = "primary";
    const char *second_source = "fallback";
    const char *first_url = primary_url;
    const char *second_url = fallback_url;

    nvs_config_get_backend_url(primary_url, sizeof(primary_url));
    strncpy(fallback_url, CONFIG_BACKEND_URL_FALLBACK, sizeof(fallback_url) - 1);

    if (s_using_fallback_url) {
        first_source = "fallback";
        second_source = "primary";
        first_url = fallback_url;
        second_url = primary_url;
    }

    if (fetch_backend_time_epoch_ms_single(first_url, first_source, epoch_ms_out)) {
        return true;
    }
    if (!backend_urls_equal(first_url, second_url) &&
        fetch_backend_time_epoch_ms_single(second_url, second_source, epoch_ms_out)) {
        bool now_using_fallback = (strcmp(second_source, "fallback") == 0);
        if (s_using_fallback_url != now_using_fallback) {
            s_using_fallback_url = now_using_fallback;
            raw_socket_forget_endpoint_cache();
        }
        return true;
    }
    return false;
}

static bool raw_socket_connect(void)
{
    if (s_sock >= 0) return true;  /* Already connected */

    char backend_url[128] = {0};
    if (s_using_fallback_url) {
        strncpy(backend_url, CONFIG_BACKEND_URL_FALLBACK, sizeof(backend_url) - 1);
    } else {
        nvs_config_get_backend_url(backend_url, sizeof(backend_url));
    }
    if (strncmp(s_cached_backend_url, backend_url, sizeof(s_cached_backend_url)) != 0) {
        raw_socket_forget_endpoint_cache();
        strncpy(s_cached_backend_url, backend_url, sizeof(s_cached_backend_url) - 1);
        s_cached_backend_url[sizeof(s_cached_backend_url) - 1] = '\0';
    }

    /* Parse host and port from URL like "http://192.0.2.1:8000" */
    const char *host_start = strstr(backend_url, "://");
    if (!host_start) host_start = backend_url; else host_start += 3;

    char host[64] = {0};
    int port = 8000;
    const char *colon = strchr(host_start, ':');
    if (colon) {
        int hlen = colon - host_start;
        if (hlen > 0 && hlen < (int)sizeof(host)) {
            strncpy(host, host_start, hlen);
        }
        port = atoi(colon + 1);
    } else {
        strncpy(host, host_start, sizeof(host) - 1);
    }
    /* Strip trailing path */
    char *slash = strchr(host, '/');
    if (slash) *slash = '\0';

    /* Resolve hostname — use cached address to avoid getaddrinfo heap allocation */
    if (!s_addr_cached) {
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
        struct addrinfo *res = NULL;
        char port_str[8];
        snprintf(port_str, sizeof(port_str), "%d", port);

        int err = getaddrinfo(host, port_str, &hints, &res);
        if (err != 0 || !res) {
            ESP_LOGE(TAG, "DNS resolve failed for %s: %d", host, err);
            return false;
        }
        memcpy(&s_cached_addr, res->ai_addr, sizeof(s_cached_addr));
        freeaddrinfo(res);
        s_addr_cached = true;
        ESP_LOGI(TAG, "Resolved %s:%d → %d.%d.%d.%d", host, port,
                 (s_cached_addr.sin_addr.s_addr) & 0xFF,
                 (s_cached_addr.sin_addr.s_addr >> 8) & 0xFF,
                 (s_cached_addr.sin_addr.s_addr >> 16) & 0xFF,
                 (s_cached_addr.sin_addr.s_addr >> 24) & 0xFF);
    }

    s_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "Socket create failed");
        return false;
    }

    /* Set timeouts */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(s_sock, (struct sockaddr *)&s_cached_addr, sizeof(s_cached_addr)) != 0) {
        ESP_LOGE(TAG, "Connect failed");
        raw_socket_forget_endpoint_cache();
        return false;
    }

    ESP_LOGI(TAG, "Raw socket connected (fd=%d)", s_sock);
    return true;
}

static void raw_socket_close(void)
{
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
}

static bool http_post_payload(const char *payload)
{
    if (!wifi_sta_is_connected()) {
        return false;
    }

    int payload_len = strlen(payload);

    for (int attempt = 0; attempt < 2; attempt++) {
        if (!raw_socket_connect()) {
            return false;
        }

        /* Build HTTP request header into static buffer */
        int hdr_len = snprintf(s_http_header, sizeof(s_http_header),
            "POST %s HTTP/1.1\r\n"
            "Host: fof-server.local\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: keep-alive\r\n"
            "\r\n",
            CONFIG_UPLOAD_ENDPOINT, payload_len);

        /* Send header + payload */
        int sent = send(s_sock, s_http_header, hdr_len, 0);
        if (sent != hdr_len) {
            ESP_LOGW(TAG, "Header send failed (%d/%d)", sent, hdr_len);
            raw_socket_close();
            continue;  /* Retry with fresh socket */
        }

        sent = send(s_sock, payload, payload_len, 0);
        if (sent != payload_len) {
            ESP_LOGW(TAG, "Payload send failed (%d/%d)", sent, payload_len);
            raw_socket_close();
            continue;
        }

        /* Read response — drain everything to keep socket clean for reuse */
        int recvd = recv(s_sock, s_http_resp, sizeof(s_http_resp) - 1, 0);
        if (recvd <= 0) {
            ESP_LOGW(TAG, "Response recv failed (%d)", recvd);
            raw_socket_close();
            continue;
        }
        s_http_resp[recvd] = '\0';

        /* Parse HTTP status code from "HTTP/1.1 200 OK" */
        int status = 0;
        if (recvd > 12 && strncmp(s_http_resp, "HTTP/", 5) == 0) {
            status = atoi(s_http_resp + 9);
        }

        /* Drain remaining response body if not fully read.
         * Parse Content-Length from headers to know how much to drain. */
        char *cl = strstr(s_http_resp, "content-length:");
        if (!cl) cl = strstr(s_http_resp, "Content-Length:");
        if (cl) {
            int content_len = atoi(cl + 16);
            /* Find end of headers (\r\n\r\n) */
            char *body = strstr(s_http_resp, "\r\n\r\n");
            if (body) {
                int hdr_len = (body + 4) - s_http_resp;
                int body_read = recvd - hdr_len;
                int remaining = content_len - body_read;
                /* Drain remaining body into discard buffer */
                while (remaining > 0) {
                    char discard[128];
                    int n = recv(s_sock, discard, remaining > 128 ? 128 : remaining, 0);
                    if (n <= 0) { raw_socket_close(); break; }
                    remaining -= n;
                }
            }
        }

        if (status >= 200 && status < 300) {
            return true;
        }

        ESP_LOGW(TAG, "Upload failed: HTTP %d", status);
        if (status == 400 || status == 422) {
            /* Client error — don't retry, payload is bad */
            return false;
        }
        /* Server error or unknown — close and retry */
        raw_socket_close();
    }

    return false;
}

/* ── Upload a payload with exponential backoff retry ───────────────────── */

static bool upload_with_retry(const char *payload)
{
    int retry_delay_ms = BACKOFF_BASE_MS;

    for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
        if (!wifi_sta_is_connected()) {
            return false;
        }

        if (http_post_payload(payload)) {
            s_success_count++;
            return true;
        }

        s_fail_count++;

        if (attempt < MAX_RETRIES) {
            ESP_LOGW(TAG, "Retry %d/%d in %dms...", attempt + 1, MAX_RETRIES, retry_delay_ms);
            vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
            retry_delay_ms *= 2;
            if (retry_delay_ms > 4000) {
                retry_delay_ms = 4000;  /* cap at 4s to stay under WDT */
            }
        }
    }

    return false;
}

/* ── Offline batch storage ─────────────────────────────────────────────── */

/*
 * When WiFi is down, we serialize the batch JSON and store it in the
 * ring buffer.  Each entry is a fixed-size char array.
 */
typedef struct {
    char json[MAX_PAYLOAD_SIZE];
} offline_batch_t;

static void buffer_batch_offline(const char *payload)
{
    if (!s_offline_buffer) {
        return;
    }

    offline_batch_t batch = {0};
    strncpy(batch.json, payload, sizeof(batch.json) - 1);

    bool overwritten = ring_buffer_push(s_offline_buffer, &batch);
    if (overwritten) {
        ESP_LOGW(TAG, "Offline buffer full, oldest batch dropped (count=%d)",
                 ring_buffer_count(s_offline_buffer));
    } else {
        ESP_LOGI(TAG, "Batch stored offline (count=%d)",
                 ring_buffer_count(s_offline_buffer));
    }
}

/**
 * Drain at most MAX_DRAIN_PER_CYCLE batches from the offline buffer.
 * Non-blocking: yields back to the main loop quickly to avoid WDT timeout.
 */
static void drain_offline_buffer(void)
{
    if (!s_offline_buffer || ring_buffer_is_empty(s_offline_buffer)) {
        return;
    }

    int remaining = ring_buffer_count(s_offline_buffer);
    int drained = 0;

    offline_batch_t batch;
    while (drained < MAX_DRAIN_PER_CYCLE && ring_buffer_pop(s_offline_buffer, &batch)) {
        if (!wifi_sta_is_connected()) {
            ring_buffer_push(s_offline_buffer, &batch);
            return;
        }

        if (!upload_with_retry(batch.json)) {
            ring_buffer_push(s_offline_buffer, &batch);
            return;
        }

        drained++;
    }

    if (drained > 0) {
        ESP_LOGI(TAG, "Drained %d offline batches (%d remaining)",
                 drained, ring_buffer_count(s_offline_buffer));
    }
}

/* ── Upload task ───────────────────────────────────────────────────────── */

static void http_upload_task(void *arg)
{
    drone_detection_t batch[CONFIG_MAX_BATCH_SIZE];
    int batch_count = 0;
    size_t estimated_payload_bytes = 0;
    TickType_t first_item_tick = 0;
    TickType_t last_item_tick = 0;
    TickType_t last_send   = 0;  /* tick count of last successful send */
    int64_t scan_ts_ms     = 0;  /* timestamp of first detection in batch */

    ESP_LOGI(TAG, "HTTP upload task started");
    bool was_connected = false;
    TickType_t last_success_tick = xTaskGetTickCount();
    int consecutive_fails = 0;

    while (1) {
        /* Paused during firmware relay — release resources and wait */
        if (s_upload_paused) {
            raw_socket_close();  /* Free HTTP socket + buffers */
            drone_detection_t discard;
            while (xQueueReceive(s_detection_queue, &discard, 0) == pdTRUE) {}
            batch_count = 0;
            estimated_payload_bytes = 0;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /* In standalone mode, just drain the queue without uploading */
        if (wifi_sta_is_standalone()) {
            drone_detection_t det;
            xQueueReceive(s_detection_queue, &det, pdMS_TO_TICKS(500));
            continue;
        }

        /* Reset HTTP client on WiFi reconnect (old socket is dead) */
        bool connected_now = wifi_sta_is_connected();
        if (connected_now && !was_connected) {
            ESP_LOGI(TAG, "WiFi reconnected — resetting HTTP client");
            raw_socket_forget_endpoint_cache();
            consecutive_fails = 0;
        }
        was_connected = connected_now;

        /* ── Health watchdog: force reset if stalled ─────────────────── */
        if (connected_now) {
            TickType_t since_success = xTaskGetTickCount() - last_success_tick;
            if (since_success >= pdMS_TO_TICKS(HEALTH_RESET_SEC * 1000)) {
                ESP_LOGW(TAG, "HEALTH RESET: no success for %ds, resetting client + clearing offline buffer",
                         HEALTH_RESET_SEC);
                raw_socket_forget_endpoint_cache();
                /* Clear stale offline batches — they're probably the cause */
                if (s_offline_buffer) {
                    offline_batch_t discard;
                    while (ring_buffer_pop(s_offline_buffer, &discard)) {}
                }
                consecutive_fails = 0;
                last_success_tick = xTaskGetTickCount();  /* prevent rapid re-triggers */
            }
        }

        /* Try to drain offline buffer when WiFi is up (non-blocking, max 1) */
        if (connected_now && consecutive_fails == 0 && !ring_buffer_is_empty(s_offline_buffer)) {
            drain_offline_buffer();
        }

        /* Collect detections into batch */
        drone_detection_t det;
        TickType_t wait_ticks = (batch_count > 0)
            ? pdMS_TO_TICKS(25)
            : pdMS_TO_TICKS(100);

        if (batch_count < CONFIG_MAX_BATCH_SIZE &&
            xQueueReceive(s_detection_queue, &det, wait_ticks) == pdTRUE) {
            bool cal_detection = is_calibration_detection(&det);
            s_node_dedup_seen++;
            if (cal_detection) {
                s_cal_seen++;
            }

            if (batch_count == 0) {
                /* Start the batch age clock with the first detection. */
                first_item_tick = xTaskGetTickCount();
                estimated_payload_bytes = 64; /* batch envelope */
                scan_ts_ms = det.last_updated_ms > 0 ? det.last_updated_ms : time_sync_get_epoch_ms();
            }
            last_item_tick = xTaskGetTickCount();

            int dup_idx = find_duplicate_in_batch(batch, batch_count, &det);
            if (dup_idx >= 0) {
                merge_duplicate_detection(&batch[dup_idx], &det);
                s_node_dedup_collapsed++;
                if (det.last_updated_ms > 0 &&
                    (scan_ts_ms == 0 || det.last_updated_ms < scan_ts_ms)) {
                    scan_ts_ms = det.last_updated_ms;
                }
                continue;
            }

            batch[batch_count++] = det;
            s_node_dedup_sent++;
            if (cal_detection) {
                s_cal_sent++;
            }
            estimated_payload_bytes += estimate_detection_json_size(&det);
            if (det.last_updated_ms > 0 &&
                (scan_ts_ms == 0 || det.last_updated_ms < scan_ts_ms)) {
                scan_ts_ms = det.last_updated_ms;
            }
        }

        /* Check if batch is ready to send */
        TickType_t now_tick = xTaskGetTickCount();
        TickType_t age = (batch_count > 0) ? (now_tick - first_item_tick) : 0;
        TickType_t idle = (batch_count > 0) ? (now_tick - last_item_tick) : 0;
        bool time_elapsed = (batch_count > 0 &&
                             age >= pdMS_TO_TICKS(CONFIG_BATCH_INTERVAL_MS));
        bool idle_flush = (batch_count > 0 &&
                           idle >= pdMS_TO_TICKS(CONFIG_BATCH_IDLE_FLUSH_MS));
        bool batch_full   = (batch_count >= CONFIG_MAX_BATCH_SIZE);
        bool payload_full = (estimated_payload_bytes >= CONFIG_TARGET_BATCH_BYTES);

        if (batch_count > 0 && (time_elapsed || idle_flush || batch_full || payload_full)) {
            ESP_LOGI(TAG,
                     "Sending batch count=%d age=%" PRIu32 "ms idle=%" PRIu32 "ms bytes=%u",
                     batch_count,
                     (uint32_t)(age * portTICK_PERIOD_MS),
                     (uint32_t)(idle * portTICK_PERIOD_MS),
                     (unsigned)estimated_payload_bytes);

            /* Heap guard: if running low, drop batch instead of crashing */
            size_t free_heap = esp_get_free_heap_size();
            if (free_heap < 10000) {
                ESP_LOGW(TAG, "LOW HEAP (%u bytes) — dropping batch of %d to recover",
                         (unsigned)free_heap, batch_count);
                raw_socket_forget_endpoint_cache();  /* Free HTTP client resources */
                batch_count = 0;
                estimated_payload_bytes = 0;
                scan_ts_ms = 0;
                first_item_tick = 0;
                continue;
            }

            char *payload = build_payload(batch, batch_count, scan_ts_ms);
            if (payload) {
                if (wifi_sta_is_connected()) {
                    if (!upload_with_retry(payload)) {
                        buffer_batch_offline(payload);
                        consecutive_fails++;
                        /* After 5 consecutive failures, try the other URL */
                        if (consecutive_fails >= 5 && consecutive_fails % 5 == 0) {
                            s_using_fallback_url = !s_using_fallback_url;
                            ESP_LOGW(TAG, "Switching to %s URL after %d failures",
                                     s_using_fallback_url ? "fallback" : "primary",
                                     consecutive_fails);
                            /* Force socket recreation with new URL */
                            raw_socket_forget_endpoint_cache();
                        }
                    } else {
                        last_send = xTaskGetTickCount();
                        last_success_tick = last_send;
                        s_last_success_epoch_ms = esp_timer_get_time() / 1000;
                        consecutive_fails = 0;
                    }
                } else {
                    buffer_batch_offline(payload);
                }
                /* payload is static buffer — no free needed */
            } else {
                ESP_LOGE(TAG, "Failed to build JSON payload");
            }

            /* Reset batch */
            batch_count = 0;
            estimated_payload_bytes = 0;
            scan_ts_ms = 0;
            first_item_tick = 0;
            last_item_tick = 0;
        }

        /* ── Heartbeat: send empty batch if idle for 60s ────────────── */
        TickType_t now = xTaskGetTickCount();
        if (batch_count == 0 && wifi_sta_is_connected() &&
            (now - last_send) >= pdMS_TO_TICKS(CONFIG_HEARTBEAT_INTERVAL_MS)) {
            ESP_LOGI(TAG, "Heartbeat (idle %ds) heap=%lu ok=%d fail=%d",
                     CONFIG_HEARTBEAT_INTERVAL_MS / 1000,
                     (unsigned long)esp_get_free_heap_size(),
                     s_success_count, s_fail_count);
            char *payload = build_payload(NULL, 0, 0);
            if (payload) {
                if (upload_with_retry(payload)) {
                    last_send = xTaskGetTickCount();
                    last_success_tick = last_send;
                    consecutive_fails = 0;
                }
                /* payload is static buffer — no free needed */
            }
        }

        /* ── Poll lock-on command from backend every 10s ─────────────── */
        {
            static TickType_t last_lockon_poll = 0;
            static bool lockon_was_active = false;
            TickType_t now2 = xTaskGetTickCount();

            if (wifi_sta_is_connected() &&
                (now2 - last_lockon_poll) >= pdMS_TO_TICKS(10000)) {
                last_lockon_poll = now2;

                /* Unconditional time sync (v0.60+).
                 * Every 10s: fetch authoritative epoch-ms from the backend
                 * and broadcast it to both scanners over UART. If the fetch
                 * fails but the uplink still has a fresh authoritative local
                 * clock (SNTP or recent backend-steered), broadcast that as
                 * a marked local fallback instead of pretending the fetch
                * succeeded. */
                {
                    int64_t backend_epoch_ms = 0;
                    int64_t now_monotonic_ms = esp_timer_get_time() / 1000;
                    g_last_time_attempt_monotonic_ms = now_monotonic_ms;
                    bool fetch_ok = fetch_backend_time_epoch_ms(&backend_epoch_ms);
                    g_last_time_fetch_ok = fetch_ok;

                    int64_t broadcast_ms = -1;
                    bool broadcast_ok = false;
                    const char *broadcast_src = "none";

                    if (fetch_ok) {
                        g_last_backend_epoch_ms = backend_epoch_ms;
                        g_last_time_success_monotonic_ms = now_monotonic_ms;
                        g_time_fetch_ok_count++;
                        g_time_fetch_fail_streak = 0;
                        g_time_source_mode = TIME_SOURCE_BACKEND;
                        time_sync_set_from_backend(backend_epoch_ms);
                        broadcast_ms = backend_epoch_ms;
                        broadcast_ok = true;
                        broadcast_src = "backend";
                    } else {
                        g_time_fetch_fail_count++;
                        g_time_fetch_fail_streak++;
                        if (time_sync_has_fresh_authority(FOF_TIME_SYNC_LOCAL_FRESHNESS_MS)) {
                            int64_t local_epoch_ms = time_sync_get_epoch_ms();
                            if (fof_time_epoch_is_valid(local_epoch_ms)) {
                                broadcast_ms = local_epoch_ms;
                                broadcast_ok = true;
                                broadcast_src = "local";
                                g_time_source_mode = time_sync_is_sntp_synced()
                                    ? TIME_SOURCE_SNTP
                                    : TIME_SOURCE_LOCAL;
                            }
                        } else {
                            g_time_source_mode = TIME_SOURCE_NONE;
                        }
                    }

                    char time_cmd[96];
                    snprintf(time_cmd, sizeof(time_cmd),
                             "{\"type\":\"%s\",\"%s\":%lld,\"%s\":%s,\"%s\":\"%s\"}",
                             MSG_TYPE_TIME, JSON_KEY_EPOCH_MS, (long long)broadcast_ms,
                             JSON_KEY_TIME_OK, broadcast_ok ? "true" : "false",
                             JSON_KEY_TIME_SOURCE, broadcast_src);
                    uart_rx_send_command(time_cmd);
                    g_time_broadcast_count++;
                    g_last_broadcast_epoch_ms = broadcast_ok ? broadcast_ms : 0;
                    if (broadcast_ok) {
                        g_time_broadcast_valid_count++;
                    } else {
                        g_time_broadcast_invalid_count++;
                    }
                    ESP_LOGI(TAG, "TIME BROADCAST: ok=%s src=%s ms=%lld fetch_ok=%s perf=%d",
                             broadcast_ok ? "true" : "false",
                             broadcast_src,
                             (long long)broadcast_ms,
                             fetch_ok ? "true" : "false",
                             g_last_time_fetch_perf);
                }

                /* Build lock-on poll URL (per-node: includes device_id) */
                char backend_url[128] = {0};
                if (s_using_fallback_url) {
                    strncpy(backend_url, CONFIG_BACKEND_URL_FALLBACK, sizeof(backend_url) - 1);
                } else {
                    nvs_config_get_backend_url(backend_url, sizeof(backend_url));
                }
                char dev_id[32] = {0};
                nvs_config_get_device_id(dev_id, sizeof(dev_id));
                char url[256];
                snprintf(url, sizeof(url), "%s/detections/lockon?device_id=%s",
                         backend_url, dev_id);

                esp_http_client_config_t cfg = {
                    .url = url,
                    .method = HTTP_METHOD_GET,
                    .timeout_ms = 3000,
                };
                esp_http_client_handle_t client = esp_http_client_init(&cfg);
                if (client) {
                    esp_err_t err = esp_http_client_perform(client);
                    if (err == ESP_OK) {
                        int len = esp_http_client_get_content_length(client);
                        if (len > 0 && len < 512) {
                            char buf[512] = {0};
                            esp_http_client_read(client, buf, sizeof(buf) - 1);
                            /* Parse lock-on response */
                            cJSON *resp = cJSON_Parse(buf);
                            if (resp) {
                                cJSON *active = cJSON_GetObjectItem(resp, "active");
                                if (active && cJSON_IsTrue(active) && !lockon_was_active) {
                                    cJSON *ch = cJSON_GetObjectItem(resp, "channel");
                                    cJSON *dur = cJSON_GetObjectItem(resp, "duration_s");
                                    cJSON *bssid_j = cJSON_GetObjectItem(resp, "bssid");
                                    cJSON *type_j = cJSON_GetObjectItem(resp, "type");
                                    int lock_ch = ch ? ch->valueint : 6;
                                    int lock_dur = dur ? dur->valueint : 45;
                                    const char *lock_type = (type_j && type_j->valuestring) ? type_j->valuestring : "wifi";
                                    const char *lock_bssid = (bssid_j && bssid_j->valuestring) ? bssid_j->valuestring : "";

                                    lockon_was_active = true;
                                    char cmd[160];

                                    if (strcmp(lock_type, "ble") == 0) {
                                        /* BLE lock-on: focus on specific MAC */
                                        ESP_LOGW(TAG, "BLE LOCK-ON: mac=%s dur=%ds", lock_bssid, lock_dur);
                                        snprintf(cmd, sizeof(cmd),
                                                 "{\"type\":\"ble_lockon\",\"mac\":\"%s\",\"dur\":%d}",
                                                 lock_bssid, lock_dur);
                                    } else {
                                        /* WiFi lock-on: fix channel */
                                        ESP_LOGW(TAG, "WiFi LOCK-ON: ch=%d bssid=%s dur=%ds",
                                                 lock_ch, lock_bssid, lock_dur);
                                        snprintf(cmd, sizeof(cmd),
                                                 "{\"type\":\"lockon\",\"ch\":%d,\"dur\":%d,\"bssid\":\"%s\"}",
                                                 lock_ch, lock_dur, lock_bssid);
                                    }
                                    uart_rx_send_command(cmd);
                                } else if (active && !cJSON_IsTrue(active) && lockon_was_active) {
                                    ESP_LOGI(TAG, "LOCK-ON cancelled by backend");
                                    lockon_was_active = false;
                                    uart_rx_send_command("{\"type\":\"lockon_cancel\"}");
                                    uart_rx_send_command("{\"type\":\"ble_lockon_cancel\"}");
                                }
                                cJSON_Delete(resp);
                            }
                        }
                    }
                    esp_http_client_cleanup(client);
                }
            }
        }
    }
}

/* ── Public API ────────────────────────────────────────────────────────── */

void http_upload_init(QueueHandle_t detection_queue)
{
    s_detection_queue = detection_queue;

    /* Try to relocate the payload buffer into PSRAM. Stay on the 4 KB static
     * buffer if PSRAM is unavailable so the upload path never allocates in
     * the hot loop. */
    char *psram_buf = (char *)psram_alloc_strict(PAYLOAD_BUF_PSRAM_SIZE);
    if (psram_buf) {
        s_payload_buf      = psram_buf;
        s_payload_buf_size = PAYLOAD_BUF_PSRAM_SIZE;
        ESP_LOGW(TAG, "Payload buffer: %d KB in PSRAM", PAYLOAD_BUF_PSRAM_SIZE / 1024);
    } else {
        ESP_LOGI(TAG, "Payload buffer: %d KB internal (no PSRAM)",
                 PAYLOAD_BUF_FALLBACK_SIZE / 1024);
    }

    /* Offline queue prefers PSRAM (2 MB / 512 batches). ring_buffer_create_psram
     * falls back internally if PSRAM is unavailable. */
    s_offline_buffer  = ring_buffer_create_psram(CONFIG_MAX_OFFLINE_BATCHES,
                                                 sizeof(offline_batch_t));
    if (!s_offline_buffer) {
        ESP_LOGE(TAG, "Failed to create offline ring buffer");
    }

    ESP_LOGI(TAG, "HTTP upload initialized (batch=%d, interval=%dms, "
             "idle_flush=%dms, target_bytes=%d, offline_cap=%d)",
             CONFIG_MAX_BATCH_SIZE, CONFIG_BATCH_INTERVAL_MS,
             CONFIG_BATCH_IDLE_FLUSH_MS, CONFIG_TARGET_BATCH_BYTES,
             CONFIG_MAX_OFFLINE_BATCHES);
}

void http_upload_start(void)
{
    xTaskCreate(http_upload_task, "http_upload", CONFIG_HTTP_UPLOAD_STACK,
                NULL, CONFIG_HTTP_UPLOAD_PRIORITY, NULL);
    ESP_LOGI(TAG, "HTTP upload task created (priority=%d, stack=%d)",
             CONFIG_HTTP_UPLOAD_PRIORITY, CONFIG_HTTP_UPLOAD_STACK);
}

int http_upload_get_success_count(void)
{
    return s_success_count;
}

int http_upload_get_fail_count(void)
{
    return s_fail_count;
}

int64_t http_upload_get_last_success_ms(void)
{
    return s_last_success_epoch_ms;
}

void http_upload_pause(void)
{
    s_upload_paused = true;
    ESP_LOGW(TAG, "HTTP upload PAUSED (firmware relay)");
}

void http_upload_resume(void)
{
    s_upload_paused = false;
    ESP_LOGW(TAG, "HTTP upload RESUMED");
}

bool http_upload_is_paused(void)
{
    return s_upload_paused;
}

int http_upload_get_offline_count(void)
{
    return s_offline_buffer ? ring_buffer_count(s_offline_buffer) : 0;
}

int http_upload_get_offline_capacity(void)
{
    return CONFIG_MAX_OFFLINE_BATCHES;
}
