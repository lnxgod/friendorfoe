/**
 * Friend or Foe -- Embedded HTTP Status Page Implementation
 *
 * Serves a dark-themed, mobile-responsive HTML page with live system
 * status and a JSON API endpoint.  Uses chunked responses to keep
 * stack usage low (~4KB).
 */

#include "http_status.h"
#include "fw_store.h"
#include "config.h"
#include "psram_alloc.h"
#include "esp_heap_caps.h"

#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "uart_protocol.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

/* Subsystem headers for status getters */
#include "uart_rx.h"
#include "gps.h"
#include "wifi_sta.h"
#include "wifi_ap.h"
#include "battery.h"
#include "http_upload.h"
#include "oled_display.h"
#include "nvs_config.h"
#include "badge_mode.h"
#include "version.h"
#include "detection_policy.h"
#ifdef FOF_BADGE_VARIANT
#include "badge_runtime.h"
#include "badge_display_policy_runtime.h"
#include "badge_theme_runtime.h"
#include "badge_ble_control.h"
#endif

static const char *TAG = "http_status";

static void json_chunk_string(httpd_req_t *req, const char *value);

/* ── Source name lookup ─────────────────────────────────────────────────── */

static const char *source_name(uint8_t src)
{
    switch (src) {
        case DETECTION_SRC_BLE_RID:            return "BLE RID";
        case DETECTION_SRC_WIFI_SSID:          return "WiFi SSID";
        case DETECTION_SRC_WIFI_DJI_IE:        return "DJI IE";
        case DETECTION_SRC_WIFI_BEACON:        return "WiFi Beacon";
        case DETECTION_SRC_WIFI_OUI:           return "WiFi OUI";
        case DETECTION_SRC_WIFI_PROBE_REQUEST: return "WiFi Probe";
        case DETECTION_SRC_BLE_FINGERPRINT:    return "BLE FP";
        case DETECTION_SRC_WIFI_ASSOC:         return "WiFi Assoc";
        case DETECTION_SRC_WIFI_AP_INVENTORY:  return "WiFi AP Inv";
        default: return "Unknown";
    }
}

static void badge_status_chunk_scanner(httpd_req_t *req,
                                       const char *name,
                                       uint8_t scanner_id,
                                       bool connected,
                                       bool peer_connected,
                                       const scanner_info_t *info,
                                       bool first)
{
    enum { SCANNER_STATUS_BUF_LEN = 2048 };
    char *buf = (char *)psram_alloc(SCANNER_STATUS_BUF_LEN);
    if (!buf) {
        httpd_resp_send_chunk(req, first ? "{}" : ",{}", HTTPD_RESP_USE_STRLEN);
        return;
    }
    const bool calibration = info && strcmp(info->scan_mode, "calibration") == 0;
    const char *expected = calibration
        ? fof_policy_scan_profile_for_slot(scanner_id, true)
        : (peer_connected ? fof_policy_scan_profile_for_slot(scanner_id, false)
                          : "hybrid_failover");
    const char *actual = (info && info->scan_profile[0])
        ? info->scan_profile
        : "";
    const bool role_acked = connected && actual[0] && strcmp(actual, expected) == 0;
    const bool cmd_fresh = connected && info && info->cmd_rx_count > 0 &&
                           info->cmd_last_age_s >= 0 && info->cmd_last_age_s <= 45;
    const char *health = !connected ? "missing" :
        (!role_acked ? "role_wait" :
         (!cmd_fresh ? "cmd_wait" :
          (info && scanner_id == 0 && !info->ble_scanning ? "ble_off" : "ok")));

    snprintf(buf, SCANNER_STATUS_BUF_LEN,
             "%s{\"slot\":%u,\"uart\":\"%s\",\"connected\":%s,"
             "\"slot_role\":\"%s\",\"expected_scan_profile\":\"%s\","
             "\"scan_profile\":\"%s\",\"role_acked\":%s,\"health\":\"%s\"",
             first ? "" : ",",
             (unsigned)scanner_id,
             name,
             connected ? "true" : "false",
             fof_policy_slot_role_for_slot(scanner_id),
             expected,
             actual,
             role_acked ? "true" : "false",
             health);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);

    scanner_uart_diag_t uart_diag = {0};
    uart_rx_get_scanner_uart_diag(scanner_id, &uart_diag);
    snprintf(buf, SCANNER_STATUS_BUF_LEN,
             ",\"uart_raw_seen\":%s,\"uart_raw_age_s\":%lld,"
             "\"uart_raw_bytes\":%lu,\"uart_line_overflow\":%lu,"
             "\"uart_json_err\":%lu",
             uart_diag.raw_seen ? "true" : "false",
             (long long)uart_diag.raw_age_s,
             (unsigned long)uart_diag.raw_bytes,
             (unsigned long)uart_diag.line_overflow_count,
             (unsigned long)uart_diag.json_parse_error_count);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);

    if (info) {
        snprintf(buf, SCANNER_STATUS_BUF_LEN,
                 ",\"ver\":\"%s\",\"board\":\"%s\",\"cmd_rx\":%lu,"
                 "\"cmd_last_age_s\":%lld,\"cmd_parse_err\":%lu,"
                 "\"cmd_overflow\":%lu,\"ble_scanning\":%s,"
                 "\"ble_host_active\":%s,\"ble_host_synced\":%s,"
                 "\"wifi_paused\":%s,"
                 "\"ble_adv_seen\":%lu,\"ble_any_seen\":%lu,"
                 "\"ble_any_with_payload_seen\":%lu,"
                 "\"ble_any_empty_seen\":%lu,"
                 "\"uart_tx_dropped\":%lu,\"uart_tx_high_water\":%lu,"
                 "\"tx_queue_depth\":%lu,\"tx_queue_capacity\":%lu,"
                 "\"tx_queue_pressure_pct\":%lu,"
                 "\"ble_any_last_rssi\":%d,\"ble_any_best_rssi\":%d,"
                 "\"ble_any_last_len\":%u,\"ble_any_last_props\":%u,"
                 "\"ble_any_last_addr_type\":%u,"
                 "\"ble_fp_emit\":%lu,"
                 "\"ble_meta_seen\":%lu,"
                 "\"ble_meta_last_seen_age_s\":%lld,"
                 "\"ble_meta_last_emit_age_s\":%lld,"
                 "\"ble_meta_last_hash\":%lu,"
                 "\"ble_meta_last_rssi\":%d,"
                 "\"ble_meta_weak_age_s\":%lld,"
                 "\"ble_meta_reacquire_count\":%lu,"
                 "\"ble_tracker_seen\":%lu,"
                 "\"ble_privacy_candidate_seen\":%lu,\"ble_near_unknown_seen\":%lu,"
                 "\"ble_drop_profile\":%lu,\"ble_drop_rate\":%lu,"
                 "\"ble_host_restart_count\":%lu,"
                 "\"ble_scan_start_count\":%lu,\"ble_scan_start_ok\":%lu,"
                 "\"ble_scan_last_rc\":%d,\"ble_sync_last_rc\":%d,"
                 "\"ble_focus_active\":%s,\"ble_focus_age_s\":%lld,"
                 "\"ble_focus_target_adv_count\":%lu,"
                 "\"rid_service_seen\":%lu,\"rid_emit\":%lu,"
                 "\"rid_queue_drop\":%lu,\"rid_queue_evict\":%lu,"
                 "\"privacy_seen\":%lu,"
                 "\"wifi_total_frames\":%lu,\"wifi_beacon_frames\":%lu,"
                 "\"wifi_full_scan_count\":%lu,\"wifi_full_scan_ok\":%lu,"
                 "\"wifi_full_scan_err\":%lu,\"wifi_full_scan_last_rc\":%d,"
                 "\"wifi_last_ap_count\":%lu,\"wifi_last_scan_age_s\":%lld,"
                 "\"wifi_oui_emit\":%lu,\"wifi_soft_ssid_emit\":%lu,"
                 "\"wifi_hot_ch\":%lu,"
                 "\"fw_state\":\"%s\",\"target_ver\":\"%s\","
                 "\"last_fw_error\":\"%s\",\"ota_state\":\"%s\","
                 "\"ota_session_id\":\"%s\",\"ota_received\":%lu,"
                 "\"ota_total\":%lu,\"recovery_mode\":\"%s\","
                 "\"safe_reason\":\"%s\",\"rollback_pending\":%s,"
                 "\"crash_count\":%lu,\"radio_restart_count\":%lu,"
                 "\"wifi_drone_ssid_emit\":%lu,\"wifi_notable_ssid_emit\":%lu,"
                 "\"wifi_last_drone_ssid_age_s\":%lld,"
                 "\"wifi_last_notable_ssid_age_s\":%lld",
                 info->version,
                 info->board,
                 (unsigned long)info->cmd_rx_count,
                 (long long)info->cmd_last_age_s,
                 (unsigned long)info->cmd_parse_error_count,
                 (unsigned long)info->cmd_overflow_count,
                 info->ble_scanning ? "true" : "false",
                 info->ble_host_active ? "true" : "false",
                 info->ble_host_synced ? "true" : "false",
                 info->wifi_paused ? "true" : "false",
                 (unsigned long)info->ble_adv_seen,
                 (unsigned long)info->ble_any_seen,
                 (unsigned long)info->ble_any_with_payload_seen,
                 (unsigned long)info->ble_any_empty_seen,
                 (unsigned long)info->uart_tx_dropped,
                 (unsigned long)info->uart_tx_high_water,
                 (unsigned long)info->tx_queue_depth,
                 (unsigned long)info->tx_queue_capacity,
                 (unsigned long)info->tx_queue_pressure_pct,
                 (int)info->ble_any_last_rssi,
                 (int)info->ble_any_best_rssi,
                 (unsigned)info->ble_any_last_len,
                 (unsigned)info->ble_any_last_props,
                 (unsigned)info->ble_any_last_addr_type,
                 (unsigned long)info->ble_fp_emit,
                 (unsigned long)info->ble_meta_seen,
                 (long long)info->ble_meta_last_seen_age_s,
                 (long long)info->ble_meta_last_emit_age_s,
                 (unsigned long)info->ble_meta_last_hash,
                 (int)info->ble_meta_last_rssi,
                 (long long)info->ble_meta_weak_age_s,
                 (unsigned long)info->ble_meta_reacquire_count,
                 (unsigned long)info->ble_tracker_seen,
                 (unsigned long)info->ble_privacy_candidate_seen,
                 (unsigned long)info->ble_near_unknown_seen,
                 (unsigned long)info->ble_drop_profile,
                 (unsigned long)info->ble_drop_rate,
                 (unsigned long)info->ble_host_restart_count,
                 (unsigned long)info->ble_scan_start_count,
                 (unsigned long)info->ble_scan_start_ok,
                 info->ble_scan_last_rc,
                 info->ble_sync_last_rc,
                 info->ble_focus_active ? "true" : "false",
                 (long long)info->ble_focus_age_s,
                 (unsigned long)info->ble_focus_target_adv_count,
                 (unsigned long)info->rid_service_seen,
                 (unsigned long)info->rid_emit,
                 (unsigned long)info->rid_queue_drop,
                 (unsigned long)info->rid_queue_evict,
                 (unsigned long)info->privacy_seen,
                 (unsigned long)info->wifi_total_frames,
                 (unsigned long)info->wifi_beacon_frames,
                 (unsigned long)info->wifi_full_scan_count,
                 (unsigned long)info->wifi_full_scan_ok,
                 (unsigned long)info->wifi_full_scan_err,
                 info->wifi_full_scan_last_rc,
                 (unsigned long)info->wifi_last_ap_count,
                 (long long)info->wifi_last_scan_age_s,
                 (unsigned long)info->wifi_oui_emit,
                 (unsigned long)info->wifi_soft_ssid_emit,
                 (unsigned long)info->wifi_hot_ch,
                 info->fw_update_state[0] ? info->fw_update_state : "idle",
                 info->fw_target_version,
                 info->last_fw_error,
                 info->ota_state[0] ? info->ota_state : "idle",
                 info->ota_session_id,
                 (unsigned long)info->ota_received,
                 (unsigned long)info->ota_total,
                 info->recovery_mode[0] ? info->recovery_mode : "normal",
                 info->safe_reason,
                 info->rollback_pending ? "true" : "false",
                 (unsigned long)info->crash_count,
                 (unsigned long)info->radio_restart_count,
                 (unsigned long)info->wifi_drone_ssid_emit,
                 (unsigned long)info->wifi_notable_ssid_emit,
                 (long long)info->wifi_last_drone_ssid_age_s,
                 (long long)info->wifi_last_notable_ssid_age_s);
        httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
        httpd_resp_send_chunk(req, ",\"wifi_last_drone_ssid\":", HTTPD_RESP_USE_STRLEN);
        json_chunk_string(req, info->wifi_last_drone_ssid);
        httpd_resp_send_chunk(req, ",\"wifi_last_notable_ssid\":", HTTPD_RESP_USE_STRLEN);
        json_chunk_string(req, info->wifi_last_notable_ssid);
        httpd_resp_send_chunk(req, ",\"ble_meta_last_reason\":", HTTPD_RESP_USE_STRLEN);
        json_chunk_string(req, info->ble_meta_last_reason);
        httpd_resp_send_chunk(req, ",\"ble_meta_identity\":", HTTPD_RESP_USE_STRLEN);
        json_chunk_string(req, info->ble_meta_identity);
        httpd_resp_send_chunk(req, ",\"ble_dbg_near_label\":", HTTPD_RESP_USE_STRLEN);
        json_chunk_string(req, info->ble_dbg_near_label);
        httpd_resp_send_chunk(req, ",\"ble_dbg_near_name\":", HTTPD_RESP_USE_STRLEN);
        json_chunk_string(req, info->ble_dbg_near_name);
        httpd_resp_send_chunk(req, ",\"ble_dbg_near_reason\":", HTTPD_RESP_USE_STRLEN);
        json_chunk_string(req, info->ble_dbg_near_reason);
        httpd_resp_send_chunk(req, ",\"ble_dbg_priv_label\":", HTTPD_RESP_USE_STRLEN);
        json_chunk_string(req, info->ble_dbg_priv_label);
        httpd_resp_send_chunk(req, ",\"ble_dbg_priv_name\":", HTTPD_RESP_USE_STRLEN);
        json_chunk_string(req, info->ble_dbg_priv_name);
        httpd_resp_send_chunk(req, ",\"ble_dbg_priv_reason\":", HTTPD_RESP_USE_STRLEN);
        json_chunk_string(req, info->ble_dbg_priv_reason);
        snprintf(buf, SCANNER_STATUS_BUF_LEN,
                 ",\"ble_dbg_near_seen\":%lu,\"ble_dbg_near_rssi\":%d,"
                 "\"ble_dbg_near_cid\":%u,\"ble_dbg_near_svc0\":%u,"
                 "\"ble_dbg_near_svc_count\":%u,"
                 "\"ble_dbg_near_payload_len\":%u,"
                 "\"ble_dbg_priv_seen\":%lu,\"ble_dbg_priv_rssi\":%d,"
                 "\"ble_dbg_priv_cid\":%u,\"ble_dbg_priv_svc0\":%u,"
                 "\"ble_dbg_priv_svc_count\":%u,"
                 "\"ble_dbg_priv_payload_len\":%u",
                 (unsigned long)info->ble_dbg_near_seen,
                 (int)info->ble_dbg_near_rssi,
                 (unsigned)info->ble_dbg_near_cid,
                 (unsigned)info->ble_dbg_near_svc0,
                 (unsigned)info->ble_dbg_near_svc_count,
                 (unsigned)info->ble_dbg_near_payload_len,
                 (unsigned long)info->ble_dbg_priv_seen,
                 (int)info->ble_dbg_priv_rssi,
                 (unsigned)info->ble_dbg_priv_cid,
                 (unsigned)info->ble_dbg_priv_svc0,
                 (unsigned)info->ble_dbg_priv_svc_count,
                 (unsigned)info->ble_dbg_priv_payload_len);
        httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
        snprintf(buf, SCANNER_STATUS_BUF_LEN,
                 ",\"display_policy_hash\":%lu,"
                 "\"display_policy_ack_hash\":%lu,\"filtered_counts\":{",
                 (unsigned long)info->display_policy_hash,
                 (unsigned long)info->display_policy_ack_hash);
        httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
        for (int i = 0; i < BADGE_DISPLAY_POLICY_CLASS_COUNT; i++) {
            badge_display_policy_class_t cls = (badge_display_policy_class_t)i;
            snprintf(buf, SCANNER_STATUS_BUF_LEN, "%s\"%s\":%lu",
                     i == 0 ? "" : ",",
                     badge_display_policy_class_key(cls),
                     (unsigned long)info->display_policy_filtered[i]);
            httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
        }
        httpd_resp_send_chunk(req, "}", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(req, "}", HTTPD_RESP_USE_STRLEN);
    psram_free(buf);
}

static const char *time_source_name(int source_mode)
{
    switch (source_mode) {
        case 1: return "backend";
        case 2: return "sntp";
        case 3: return "local";
        default: return "none";
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

static bool scanner_mode_matches(const scanner_info_t *info,
                                 const char *expected_mode,
                                 const char *expected_uuid,
                                 bool require_ack)
{
    if (!info || !expected_mode) {
        return false;
    }
    if (require_ack && !info->calibration_mode_acked) {
        return false;
    }
    const char *mode = info->scan_mode[0] ? info->scan_mode : "normal";
    if (strcmp(mode, expected_mode) != 0) {
        return false;
    }
    if (expected_uuid && expected_uuid[0] != '\0') {
        return strcmp(info->calibration_uuid, expected_uuid) == 0;
    }
    return true;
}

static bool scanner_connected_mode_matches(bool connected,
                                           const scanner_info_t *info,
                                           const char *expected_mode,
                                           const char *expected_uuid,
                                           bool require_ack)
{
    if (!connected) {
        return true;
    }
    return scanner_mode_matches(info, expected_mode, expected_uuid, require_ack);
}

static const char *aggregate_calibration_scan_mode(void)
{
    const char *uuid = uart_rx_get_node_calibration_uuid();
    bool root_cal = uart_rx_is_node_calibration_mode();
    bool ble_conn = uart_rx_is_ble_scanner_connected();
#if CONFIG_DUAL_SCANNER
    bool wifi_conn = uart_rx_is_wifi_scanner_connected();
#else
    bool wifi_conn = false;
#endif

    bool slots_normal =
        scanner_connected_mode_matches(
            ble_conn, uart_rx_get_ble_scanner_info(), "normal", "", false) &&
        scanner_connected_mode_matches(
            wifi_conn, uart_rx_get_wifi_scanner_info(), "normal", "", false);

    bool slots_calibration =
        scanner_connected_mode_matches(
            ble_conn, uart_rx_get_ble_scanner_info(), "calibration", uuid, true) &&
        scanner_connected_mode_matches(
            wifi_conn, uart_rx_get_wifi_scanner_info(), "calibration", uuid, true);

    if (!root_cal && slots_normal) {
        return "normal";
    }
    if (root_cal && slots_calibration) {
        return "calibration";
    }
    return "degraded";
}

static bool wait_for_node_mode(const char *expected_mode,
                               const char *expected_uuid,
                               int timeout_ms,
                               bool require_ack)
{
    bool ble_required = uart_rx_is_ble_scanner_connected();
#if CONFIG_DUAL_SCANNER
    bool wifi_required = uart_rx_is_wifi_scanner_connected();
#else
    bool wifi_required = false;
#endif

    if (!ble_required && !wifi_required) {
        return false;
    }

    int waited_ms = 0;
    while (waited_ms <= timeout_ms) {
        bool ble_ok = !ble_required ||
                      scanner_mode_matches(
                          uart_rx_get_ble_scanner_info(),
                          expected_mode,
                          expected_uuid,
                          require_ack
                      );
#if CONFIG_DUAL_SCANNER
        bool wifi_ok = !wifi_required ||
                       scanner_mode_matches(
                           uart_rx_get_wifi_scanner_info(),
                           expected_mode,
                           expected_uuid,
                           require_ack
                       );
#else
        bool wifi_ok = true;
#endif
        if (ble_ok && wifi_ok) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        waited_ms += 100;
    }
    return false;
}

/* ── HTML status page handler ──────────────────────────────────────────── */

static esp_err_t status_html_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");

    /* Gather state */
    char device_id[32] = {0};
    nvs_config_get_device_id(device_id, sizeof(device_id));

    int64_t uptime_sec = esp_timer_get_time() / 1000000;
    int up_h = (int)(uptime_sec / 3600);
    int up_m = (int)((uptime_sec % 3600) / 60);
    int up_s = (int)(uptime_sec % 60);

    gps_position_t gps = {0};
    bool gps_fix = gps_get_position(&gps);

    bool wifi_ok = wifi_sta_is_connected();
    float batt_pct = battery_get_percentage();
    float batt_v   = battery_get_voltage();

    int det_count   = uart_rx_get_detection_count();
    int upload_ok   = http_upload_get_success_count();
    int upload_fail = http_upload_get_fail_count();

    int ap_clients = wifi_ap_get_station_count();
    bool standalone = wifi_sta_is_standalone();
    bool scanner_ok = uart_rx_is_scanner_connected();

    char buf[512];

    /* ── Head + CSS ────────────────────────────────────────────────────── */
    httpd_resp_send_chunk(req,
        "<!DOCTYPE html><html><head>"
        "<meta charset=\"UTF-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta http-equiv=\"refresh\" content=\"3\">"
        "<title>FoF Status</title>"
        "<style>"
        "*{box-sizing:border-box;margin:0;padding:0}"
        "body{font-family:-apple-system,system-ui,sans-serif;"
        "background:#0d1117;color:#e6edf3;padding:1rem}"
        ".c{max-width:600px;margin:0 auto}"
        "h1{font-size:1.3rem;margin-bottom:.5rem;color:#58a6ff}"
        "h2{font-size:1rem;margin:1rem 0 .5rem;color:#8b949e;"
        "border-bottom:1px solid #30363d;padding-bottom:.25rem}"
        ".g{display:grid;grid-template-columns:1fr 1fr;gap:.5rem}"
        ".r{background:#161b22;border:1px solid #30363d;border-radius:8px;"
        "padding:.6rem .8rem}"
        ".r .l{font-size:.75rem;color:#8b949e}"
        ".r .v{font-size:1.1rem;font-weight:600;margin-top:.15rem}"
        ".ok{color:#3fb950}.warn{color:#d29922}.err{color:#f85149}"
        "table{width:100%;border-collapse:collapse;margin-top:.5rem;font-size:.8rem}"
        "th{text-align:left;color:#8b949e;padding:.3rem .4rem;"
        "border-bottom:1px solid #30363d}"
        "td{padding:.3rem .4rem;border-bottom:1px solid #21262d}"
        ".foot{text-align:center;color:#484f58;font-size:.7rem;margin-top:1.5rem}"
        "</style></head><body><div class=\"c\">",
        HTTPD_RESP_USE_STRLEN);

    /* ── Header ────────────────────────────────────────────────────────── */
    snprintf(buf, sizeof(buf),
        "<h1>Friend or Foe</h1>"
        "<div style=\"font-size:.8rem;color:#8b949e\">%s &mdash; up %02d:%02d:%02d</div>",
        device_id, up_h, up_m, up_s);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);

    /* ── Standalone banner ────────────────────────────────────────────── */
    if (standalone) {
        httpd_resp_send_chunk(req,
            "<div style=\"background:#3d2e00;border:1px solid #d29922;"
            "border-radius:8px;padding:.6rem .8rem;margin:.75rem 0;"
            "color:#d29922;font-size:.85rem\">"
            "Standalone Mode &mdash; "
            "<a href=\"/setup\" style=\"color:#58a6ff;font-weight:700\">"
            "Configure WiFi</a> to connect to your network</div>",
            HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_chunk(req,
            "<div style=\"text-align:right;margin:.5rem 0;font-size:.8rem\">"
            "<a href=\"/setup\" style=\"color:#484f58\">Setup</a></div>",
            HTTPD_RESP_USE_STRLEN);
    }

    /* ── GPS section ───────────────────────────────────────────────────── */
    httpd_resp_send_chunk(req, "<h2>GPS</h2><div class=\"g\">", HTTPD_RESP_USE_STRLEN);

    snprintf(buf, sizeof(buf),
        "<div class=\"r\"><div class=\"l\">Fix</div>"
        "<div class=\"v %s\">%s</div></div>",
        gps_fix ? "ok" : "err",
        gps_fix ? "Yes" : "No Fix");
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);

    snprintf(buf, sizeof(buf),
        "<div class=\"r\"><div class=\"l\">Satellites</div>"
        "<div class=\"v\">%d</div></div>",
        gps.satellites);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);

    if (gps_fix) {
        snprintf(buf, sizeof(buf),
            "<div class=\"r\"><div class=\"l\">Latitude</div>"
            "<div class=\"v\">%.6f</div></div>"
            "<div class=\"r\"><div class=\"l\">Longitude</div>"
            "<div class=\"v\">%.6f</div></div>",
            gps.latitude, gps.longitude);
        httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);

    /* ── Scanner section ──────────────────────────────────────────────── */
    httpd_resp_send_chunk(req, "<h2>Scanner</h2><div class=\"g\">", HTTPD_RESP_USE_STRLEN);

    snprintf(buf, sizeof(buf),
        "<div class=\"r\"><div class=\"l\">Status</div>"
        "<div class=\"v %s\">%s</div></div>",
        scanner_ok ? "ok" : "err",
        scanner_ok ? "Connected" : "Disconnected");
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);

    snprintf(buf, sizeof(buf),
        "<div class=\"r\"><div class=\"l\">Detections</div>"
        "<div class=\"v\">%d</div></div>",
        det_count);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);

    /* ── Network section ───────────────────────────────────────────────── */
    httpd_resp_send_chunk(req, "<h2>Network</h2><div class=\"g\">", HTTPD_RESP_USE_STRLEN);

    if (standalone) {
        httpd_resp_send_chunk(req,
            "<div class=\"r\"><div class=\"l\">WiFi STA</div>"
            "<div class=\"v warn\">Standalone</div></div>",
            HTTPD_RESP_USE_STRLEN);
    } else {
        snprintf(buf, sizeof(buf),
            "<div class=\"r\"><div class=\"l\">WiFi STA</div>"
            "<div class=\"v %s\">%s</div></div>",
            wifi_ok ? "ok" : "err",
            wifi_ok ? "Connected" : "Disconnected");
        httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    }

    snprintf(buf, sizeof(buf),
        "<div class=\"r\"><div class=\"l\">AP Clients</div>"
        "<div class=\"v\">%d</div></div>",
        ap_clients);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);

    /* ── Battery section ───────────────────────────────────────────────── */
    httpd_resp_send_chunk(req, "<h2>Battery</h2><div class=\"g\">", HTTPD_RESP_USE_STRLEN);

    const char *batt_cls = batt_pct > 30.0f ? "ok" : (batt_pct > 10.0f ? "warn" : "err");
    snprintf(buf, sizeof(buf),
        "<div class=\"r\"><div class=\"l\">Charge</div>"
        "<div class=\"v %s\">%.0f%%</div></div>"
        "<div class=\"r\"><div class=\"l\">Voltage</div>"
        "<div class=\"v\">%.2fV</div></div>",
        batt_cls, batt_pct, batt_v);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);

    /* ── Uploads section (hidden in standalone mode) ─────────────────── */
    if (!standalone) {
        httpd_resp_send_chunk(req, "<h2>Uploads</h2><div class=\"g\">", HTTPD_RESP_USE_STRLEN);

        snprintf(buf, sizeof(buf),
            "<div class=\"r\"><div class=\"l\">Uploaded</div>"
            "<div class=\"v ok\">%d</div></div>"
            "<div class=\"r\"><div class=\"l\">Failed</div>"
            "<div class=\"v %s\">%d</div></div>",
            upload_ok,
            upload_fail > 0 ? "err" : "ok", upload_fail);
        httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);

        httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
    }

    /* ── Recent detections table ───────────────────────────────────────── */
    detection_summary_t recent[8];
    int n = uart_rx_get_recent_detections(recent, 8);

    if (n > 0) {
        httpd_resp_send_chunk(req,
            "<h2>Recent</h2>"
            "<table><tr><th>Drone ID</th><th>Source</th>"
            "<th>Conf</th><th>RSSI</th><th>Age</th></tr>",
            HTTPD_RESP_USE_STRLEN);

        int64_t now_ms = esp_timer_get_time() / 1000;
        for (int i = 0; i < n; i++) {
            int age_sec = (int)((now_ms - recent[i].timestamp_ms) / 1000);
            const char *age_unit = "s";
            int age_val = age_sec;
            if (age_sec >= 60) {
                age_val = age_sec / 60;
                age_unit = "m";
            }
            /* Truncate long drone IDs for display */
            char short_id[20];
            if (strlen(recent[i].drone_id) > 18) {
                snprintf(short_id, sizeof(short_id), "%.15s...",
                         recent[i].drone_id);
            } else {
                strncpy(short_id, recent[i].drone_id, sizeof(short_id) - 1);
                short_id[sizeof(short_id) - 1] = '\0';
            }

            snprintf(buf, sizeof(buf),
                "<tr><td>%s</td><td>%s</td>"
                "<td>%.0f%%</td><td>%d</td><td>%d%s</td></tr>",
                short_id, source_name(recent[i].source),
                recent[i].confidence * 100.0f,
                recent[i].rssi, age_val, age_unit);
            httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
        }

        httpd_resp_send_chunk(req, "</table>", HTTPD_RESP_USE_STRLEN);
    }

    /* ── Footer ────────────────────────────────────────────────────────── */
    httpd_resp_send_chunk(req,
        "<div class=\"foot\">Auto-refreshes every 3s</div>"
        "</div></body></html>",
        HTTPD_RESP_USE_STRLEN);

    /* Finish chunked response */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── JSON API handler ──────────────────────────────────────────────────── */

static esp_err_t status_json_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    char device_id[32] = {0};
    nvs_config_get_device_id(device_id, sizeof(device_id));

    int64_t uptime_sec = esp_timer_get_time() / 1000000;

    gps_position_t gps = {0};
    bool gps_fix = gps_get_position(&gps);

    bool wifi_ok    = wifi_sta_is_connected();
    float batt_pct  = battery_get_percentage();
    float batt_v    = battery_get_voltage();
    int det_count   = uart_rx_get_detection_count();
    int upload_ok   = http_upload_get_success_count();
    int upload_fail = http_upload_get_fail_count();
    int ap_clients  = wifi_ap_get_station_count();
    bool standalone = wifi_sta_is_standalone();
    bool scanner_ok = uart_rx_is_scanner_connected();

    enum { STATUS_JSON_BUF_LEN = 1400 };
    char *buf = (char *)psram_alloc(STATUS_JSON_BUF_LEN);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status scratch alloc failed");
        return ESP_OK;
    }

    /* Heap / PSRAM snapshot for S3 N16R8 boards. */
    size_t heap_internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t heap_internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t psram_free_bytes  = psram_free_size();
    size_t psram_total = psram_total_size();
    extern volatile int64_t g_last_backend_epoch_ms;
    extern volatile int g_last_time_fetch_perf;
    extern volatile int g_last_time_fetch_status;
    extern volatile int g_last_time_fetch_nread;
    extern volatile int g_last_time_fetch_clen;
    extern volatile uint32_t g_time_broadcast_count;
    extern volatile bool g_last_time_fetch_ok;
    extern volatile int64_t g_last_time_attempt_monotonic_ms;
    extern volatile int64_t g_last_time_success_monotonic_ms;
    extern volatile uint32_t g_time_fetch_ok_count;
    extern volatile uint32_t g_time_fetch_fail_count;
    extern volatile uint32_t g_time_fetch_fail_streak;
    extern volatile int64_t g_last_broadcast_epoch_ms;
    extern volatile uint32_t g_time_broadcast_valid_count;
    extern volatile uint32_t g_time_broadcast_invalid_count;
    extern volatile int g_time_source_mode;
    extern char g_last_time_fetch_url_source[16];
    extern char g_last_time_fetch_url[96];
    extern char g_last_time_fetch_ip[16];
    extern volatile int g_last_time_fetch_port;

    /* Open JSON object */
    snprintf(buf, STATUS_JSON_BUF_LEN,
        "{\"device_id\":\"%s\",\"uptime_s\":%lld,"
        "\"gps\":{\"fix\":%s,\"lat\":%.6f,\"lon\":%.6f,\"satellites\":%d},"
        "\"wifi_sta\":%s,\"ap_clients\":%d,"
        "\"standalone\":%s,\"scanner_connected\":%s,"
        "\"scan_mode\":\"%s\",\"calibration_uuid\":\"%s\","
        "\"battery\":{\"percent\":%.1f,\"voltage\":%.2f},"
        "\"heap\":{\"internal_free\":%u,\"internal_total\":%u,"
                  "\"psram_free\":%u,\"psram_total\":%u},"
        "\"offline_queue\":{\"depth\":%d,\"capacity\":%d},"
        "\"time_sync\":{\"time_source\":\"%s\",\"last_fetch_ok\":%s,"
                     "\"last_fetch_url_source\":\"%s\",\"last_fetch_url\":\"%s\","
                     "\"last_fetch_ip\":\"%s\",\"last_fetch_port\":%d,"
                     "\"last_attempt_age_s\":%lld,\"last_success_age_s\":%lld,"
                     "\"fetch_ok_count\":%u,\"fetch_fail_count\":%u,\"fetch_fail_streak\":%u,"
                     "\"last_epoch_ms\":%lld,\"last_broadcast_epoch_ms\":%lld,"
                     "\"perf\":%d,\"status\":%d,\"clen\":%d,\"nread\":%d,"
                     "\"bcasts\":%u,\"broadcast_valid_count\":%u,\"broadcast_invalid_count\":%u},"
        "\"detections\":%d,\"uploads_ok\":%d,\"uploads_fail\":%d,",
        device_id, (long long)uptime_sec,
        gps_fix ? "true" : "false", gps.latitude, gps.longitude, gps.satellites,
        wifi_ok ? "true" : "false", ap_clients,
        standalone ? "true" : "false",
        scanner_ok ? "true" : "false",
        uart_rx_get_node_scan_mode(),
        uart_rx_get_node_calibration_uuid(),
        batt_pct, batt_v,
        (unsigned)heap_internal_free, (unsigned)heap_internal_total,
        (unsigned)psram_free_bytes, (unsigned)psram_total,
        http_upload_get_offline_count(), http_upload_get_offline_capacity(),
        time_source_name(g_time_source_mode),
        g_last_time_fetch_ok ? "true" : "false",
        g_last_time_fetch_url_source,
        g_last_time_fetch_url,
        g_last_time_fetch_ip,
        g_last_time_fetch_port,
        (long long)monotonic_age_s(g_last_time_attempt_monotonic_ms),
        (long long)monotonic_age_s(g_last_time_success_monotonic_ms),
        (unsigned)g_time_fetch_ok_count,
        (unsigned)g_time_fetch_fail_count,
        (unsigned)g_time_fetch_fail_streak,
        (long long)g_last_backend_epoch_ms,
        (long long)g_last_broadcast_epoch_ms,
        g_last_time_fetch_perf, g_last_time_fetch_status,
        g_last_time_fetch_clen, g_last_time_fetch_nread,
        (unsigned)g_time_broadcast_count,
        (unsigned)g_time_broadcast_valid_count,
        (unsigned)g_time_broadcast_invalid_count,
        det_count, upload_ok, upload_fail);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);

    int64_t last_upload_ms = http_upload_get_last_success_ms();
    int64_t last_upload_age_s = -1;
    if (last_upload_ms > 0) {
        int64_t now_monotonic_ms = esp_timer_get_time() / 1000;
        last_upload_age_s = now_monotonic_ms >= last_upload_ms
            ? (now_monotonic_ms - last_upload_ms) / 1000
            : 0;
    }
    httpd_resp_send_chunk(req, "\"reporting\":{\"network_mode\":", HTTPD_RESP_USE_STRLEN);
#ifdef FOF_BADGE_VARIANT
    json_chunk_string(req, badge_runtime_network_mode_name(
        badge_runtime_get_network_mode()));
    snprintf(buf, STATUS_JSON_BUF_LEN,
             ",\"backend_enabled\":%s,\"network_ttl_s\":%d,"
             "\"reset_reason\":",
             badge_runtime_get_network_mode() == BADGE_RUNTIME_NETWORK_BACKEND ? "true" : "false",
             badge_runtime_get_network_ttl_s());
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    json_chunk_string(req, badge_runtime_last_reset_reason_name());
    snprintf(buf, STATUS_JSON_BUF_LEN,
             ",\"reset_reason_code\":%lu,\"reset_expected\":%s,"
             "\"usb_control_age_s\":%lld,\"recovery_mode\":",
             (unsigned long)badge_runtime_last_reset_reason(),
             badge_runtime_last_reset_expected() ? "true" : "false",
             (long long)badge_runtime_usb_control_age_s());
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    json_chunk_string(req, badge_runtime_recovery_mode());
    snprintf(buf, STATUS_JSON_BUF_LEN,
             ",\"crash_count\":%lu,\"stack_main_free\":%lu,"
             "\"stack_display_free\":%lu,\"stack_usb_free\":%lu,"
             "\"stack_uart_ble_free\":%lu,\"stack_uart_wifi_free\":%lu",
             (unsigned long)badge_runtime_crash_count(),
             (unsigned long)badge_runtime_main_stack_free(),
             (unsigned long)badge_runtime_display_stack_free(),
             (unsigned long)badge_runtime_usb_stack_free(),
             (unsigned long)badge_runtime_uart_ble_stack_free(),
             (unsigned long)badge_runtime_uart_wifi_stack_free());
#else
    json_chunk_string(req, standalone ? "standalone" : "backend");
    snprintf(buf, STATUS_JSON_BUF_LEN,
             ",\"backend_enabled\":%s,\"network_ttl_s\":0",
             standalone ? "false" : "true");
#endif
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    snprintf(buf, STATUS_JSON_BUF_LEN,
             ",\"wifi_sta\":%s,\"standalone\":%s,"
             "\"uploads_ok\":%d,\"uploads_fail\":%d,"
             "\"last_upload_age_s\":%lld},",
             wifi_ok ? "true" : "false",
             standalone ? "true" : "false",
             upload_ok,
             upload_fail,
             (long long)last_upload_age_s);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);

    /* Recent detections array */
    httpd_resp_send_chunk(req, "\"recent\":[", HTTPD_RESP_USE_STRLEN);

    detection_summary_t recent[8];
    int n = uart_rx_get_recent_detections(recent, 8);
    int64_t now_ms = esp_timer_get_time() / 1000;

    for (int i = 0; i < n; i++) {
        int age_sec = (int)((now_ms - recent[i].timestamp_ms) / 1000);
        snprintf(buf, STATUS_JSON_BUF_LEN,
            "%s{\"drone_id\":\"%s\",\"source\":\"%s\","
            "\"confidence\":%.2f,\"rssi\":%d,\"age_s\":%d}",
            i > 0 ? "," : "",
            recent[i].drone_id, source_name(recent[i].source),
            recent[i].confidence, recent[i].rssi, age_sec);
        httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_send_chunk(req, "],\"scanners\":[", HTTPD_RESP_USE_STRLEN);
    bool ble_connected = uart_rx_is_ble_scanner_connected();
    bool wifi_connected = uart_rx_is_wifi_scanner_connected();
    badge_status_chunk_scanner(req, "ble", 0,
                               ble_connected,
                               wifi_connected,
                               uart_rx_get_ble_scanner_info(),
                               true);
#if CONFIG_DUAL_SCANNER
    badge_status_chunk_scanner(req, "wifi", 1,
                               wifi_connected,
                               ble_connected,
                               uart_rx_get_wifi_scanner_info(),
                               false);
#endif
    httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN);

    /* Finish */
    httpd_resp_send_chunk(req, NULL, 0);
    psram_free(buf);
    return ESP_OK;
}

/* ── WiFi Setup Page ─────────────────────────────────────────────────── */

static esp_err_t setup_html_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");

    char cur_ssid[33] = {0};
    nvs_config_get_wifi_ssid(cur_ssid, sizeof(cur_ssid));
    char cur_url[128] = {0};
    nvs_config_get_backend_url(cur_url, sizeof(cur_url));
    char dev_id[32] = {0};
    nvs_config_get_device_id(dev_id, sizeof(dev_id));

    httpd_resp_send_chunk(req,
        "<!DOCTYPE html><html><head>"
        "<meta charset=\"UTF-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>FoF Setup</title>"
        "<style>"
        "*{box-sizing:border-box;margin:0;padding:0}"
        "body{font-family:-apple-system,system-ui,sans-serif;"
        "background:#0d1117;color:#e6edf3;padding:1rem}"
        ".c{max-width:500px;margin:0 auto}"
        "h1{font-size:1.3rem;margin-bottom:.3rem;color:#58a6ff}"
        "h2{font-size:1rem;margin:1rem 0 .5rem;color:#8b949e;"
        "border-bottom:1px solid #30363d;padding-bottom:.25rem}"
        ".card{background:#161b22;border:1px solid #30363d;border-radius:8px;"
        "padding:.8rem;margin-bottom:.75rem}"
        "label{display:block;font-size:.8rem;color:#8b949e;margin-bottom:.2rem}"
        "input[type=text],input[type=password]{width:100%;padding:.5rem;"
        "background:#0d1117;border:1px solid #30363d;border-radius:6px;"
        "color:#e6edf3;font-size:.9rem;margin-bottom:.5rem}"
        "input:focus{border-color:#58a6ff;outline:none}"
        "button{padding:.5rem 1rem;border:none;border-radius:6px;"
        "font-weight:600;cursor:pointer;font-size:.85rem;margin-right:.5rem}"
        ".btn-scan{background:#1f6feb;color:#fff}"
        ".btn-save{background:#238636;color:#fff}"
        ".btn-sec{background:#30363d;color:#c9d1d9}"
        ".net{display:flex;justify-content:space-between;align-items:center;"
        "padding:.4rem .6rem;border-bottom:1px solid #21262d;cursor:pointer}"
        ".net:hover{background:#1c2333}.net:last-child{border:none}"
        ".rssi{font-size:.75rem;font-weight:700}"
        ".ok{color:#3fb950}.warn{color:#d29922}.err{color:#f85149}"
        "#scan-list{max-height:250px;overflow-y:auto}"
        "#msg{margin-top:.5rem;padding:.4rem .6rem;border-radius:6px;font-size:.85rem;"
        "display:none}"
        ".msg-ok{background:#0d2818;border:1px solid #238636;color:#3fb950;display:block}"
        ".msg-err{background:#2d0a0e;border:1px solid #da3633;color:#f85149;display:block}"
        ".msg-info{background:#0d1d30;border:1px solid #1f6feb;color:#58a6ff;display:block}"
        "</style></head><body><div class=\"c\">",
        HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req,
        "<h1>Friend or Foe</h1>"
        "<div style=\"font-size:.8rem;color:#8b949e;margin-bottom:.5rem\">"
        "WiFi &amp; Network Setup</div>",
        HTTPD_RESP_USE_STRLEN);

    /* WiFi scan + select */
    httpd_resp_send_chunk(req,
        "<div class=\"card\">"
        "<h2 style=\"margin-top:0\">WiFi Network</h2>"
        "<button class=\"btn-scan\" onclick=\"doScan()\">Scan Networks</button>"
        "<div id=\"scan-list\" style=\"margin-top:.5rem\"></div>"
        "<label style=\"margin-top:.5rem\">SSID</label>"
        "<input type=\"text\" id=\"ssid\" placeholder=\"Select from scan or type\""
        , HTTPD_RESP_USE_STRLEN);

    char buf[512];
    snprintf(buf, sizeof(buf), " value=\"%s\">", cur_ssid);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req,
        "<label>Password</label>"
        "<input type=\"password\" id=\"pass\" placeholder=\"WiFi password\">"
        "</div>",
        HTTPD_RESP_USE_STRLEN);

    /* Backend URL */
    snprintf(buf, sizeof(buf),
        "<div class=\"card\">"
        "<h2 style=\"margin-top:0\">Backend Server</h2>"
        "<label>URL</label>"
        "<input type=\"text\" id=\"url\" value=\"%s\">"
        "</div>", cur_url);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);

    /* Device ID (read-only, MAC-based) */
    snprintf(buf, sizeof(buf),
        "<div class=\"card\">"
        "<h2 style=\"margin-top:0\">Device</h2>"
        "<label>Device ID (auto from MAC)</label>"
        "<input type=\"text\" id=\"devid\" value=\"%s\" readonly "
        "style=\"opacity:0.6;cursor:not-allowed\">"
        "</div>", dev_id);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);

    /* Save + status */
    httpd_resp_send_chunk(req,
        "<button class=\"btn-save\" onclick=\"doSave()\">Save &amp; Connect</button>"
        "<button class=\"btn-sec\" onclick=\"location.href='/'\">Status Page</button>"
        "<div id=\"msg\"></div>",
        HTTPD_RESP_USE_STRLEN);

    /* JavaScript */
    httpd_resp_send_chunk(req,
        "<script>"
        "function msg(t,c){var m=document.getElementById('msg');"
        "m.className=c;m.textContent=t;m.style.display='block'}"
        "function doScan(){"
        "msg('Scanning...','msg-info');"
        "fetch('/api/scan').then(r=>r.json()).then(d=>{"
        "var h='';d.networks.forEach(n=>{"
        "var rc=n.rssi>-50?'ok':n.rssi>-70?'warn':'err';"
        "h+='<div class=\"net\" onclick=\"document.getElementById(\\'ssid\\').value=\\''+n.ssid+'\\'\">'"
        "+'<span>'+n.ssid+(n.secure?' &#x1f512;':'')+'</span>'"
        "+'<span class=\"rssi '+rc+'\">'+n.rssi+'</span></div>';"
        "});document.getElementById('scan-list').innerHTML=h||'<div style=\"color:#484f58;padding:.5rem\">No networks found</div>';"
        "msg('Found '+d.networks.length+' networks','msg-ok');"
        "}).catch(e=>msg('Scan failed: '+e,'msg-err'))}"
        "function doSave(){"
        "var s=document.getElementById('ssid').value;"
        "var p=document.getElementById('pass').value;"
        "var u=document.getElementById('url').value;"
        "var d=document.getElementById('devid').value;"
        "if(!s){msg('SSID required','msg-err');return;}"
        "msg('Saving...','msg-info');"
        "fetch('/api/connect',{method:'POST',"
        "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
        "body:'ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)"
        "+'&url='+encodeURIComponent(u)+'&devid='+encodeURIComponent(d)"
        "}).then(r=>r.json()).then(d=>{"
        "if(d.ok){msg('Saved! Rebooting to connect to '+s+'...','msg-ok');"
        "setTimeout(()=>location.reload(),5000);}"
        "else msg('Error: '+(d.error||'unknown'),'msg-err');"
        "}).catch(e=>msg('Failed: '+e,'msg-err'))}"
        "</script>",
        HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req,
        "</div></body></html>",
        HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── WiFi Scan API ────────────────────────────────────────────────────── */

/* Escape a string for JSON embedding — handles " and \ */
static int json_escape_ssid(char *dst, size_t dst_sz, const char *src)
{
    size_t di = 0;
    for (const char *s = src; *s && di + 2 < dst_sz; s++) {
        if (*s == '"' || *s == '\\') {
            dst[di++] = '\\';
        }
        dst[di++] = *s;
    }
    dst[di] = '\0';
    return (int)di;
}

static esp_err_t scan_json_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time   = { .active = { .min = 100, .max = 500 } },
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        httpd_resp_sendstr(req, "{\"error\":\"scan_failed\",\"networks\":[]}");
        return ESP_OK;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 30) ap_count = 30;

    wifi_ap_record_t *ap_list = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_list) {
        esp_wifi_scan_get_ap_records(&ap_count, NULL);
        httpd_resp_sendstr(req, "{\"error\":\"oom\",\"networks\":[]}");
        return ESP_OK;
    }
    esp_wifi_scan_get_ap_records(&ap_count, ap_list);

    /* Deduplicate by SSID (keep strongest) */
    char buf[160];
    char escaped_ssid[66];  /* 32 chars * 2 (worst case all escaped) + NUL */
    httpd_resp_send_chunk(req, "{\"networks\":[", HTTPD_RESP_USE_STRLEN);

    int sent = 0;
    for (int i = 0; i < ap_count; i++) {
        if (ap_list[i].ssid[0] == '\0') continue;

        /* Skip if we already sent a stronger copy of this SSID */
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp((char *)ap_list[i].ssid, (char *)ap_list[j].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) continue;

        json_escape_ssid(escaped_ssid, sizeof(escaped_ssid), (const char *)ap_list[i].ssid);
        snprintf(buf, sizeof(buf),
            "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
            sent > 0 ? "," : "",
            escaped_ssid,
            ap_list[i].rssi,
            ap_list[i].authmode != WIFI_AUTH_OPEN ? "true" : "false");
        httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
        sent++;
    }

    free(ap_list);
    httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── WiFi Connect API (save creds + reboot) ──────────────────────────── */

static esp_err_t connect_post_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    char body[512] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no body\"}");
        return ESP_OK;
    }

    /* Parse URL-encoded form: ssid=X&pass=Y&url=Z&devid=W */
    char ssid[33] = {0}, pass[65] = {0}, url[128] = {0}, devid[32] = {0};

    /* Simple URL-decode parser */
    char *p = body;
    while (p && *p) {
        char *eq = strchr(p, '=');
        if (!eq) break;
        *eq = '\0';
        char *val = eq + 1;
        char *amp = strchr(val, '&');
        if (amp) *amp = '\0';

        /* URL-decode value in-place (just handle %XX and +) */
        char *r = val, *w = val;
        while (*r) {
            if (*r == '+') { *w++ = ' '; r++; }
            else if (*r == '%' && r[1] && r[2]) {
                unsigned int ch;
                sscanf(r + 1, "%2x", &ch);
                *w++ = (char)ch;
                r += 3;
            } else { *w++ = *r++; }
        }
        *w = '\0';

        if (strcmp(p, "ssid") == 0) strncpy(ssid, val, sizeof(ssid) - 1);
        else if (strcmp(p, "pass") == 0) strncpy(pass, val, sizeof(pass) - 1);
        else if (strcmp(p, "url") == 0) strncpy(url, val, sizeof(url) - 1);
        else if (strcmp(p, "devid") == 0) strncpy(devid, val, sizeof(devid) - 1);

        p = amp ? amp + 1 : NULL;
    }

    if (ssid[0] == '\0') {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"ssid required\"}");
        return ESP_OK;
    }

    /* Save to NVS */
    nvs_config_set_string("wifi_ssid", ssid);
    if (pass[0]) nvs_config_set_string("wifi_pass", pass);
    if (url[0]) nvs_config_set_string("backend_url", url);
    /* device_id is always MAC-based — ignore any devid from form */

    ESP_LOGI(TAG, "WiFi config saved: SSID='%s' URL='%s' ID='%s'", ssid, url, devid);

    httpd_resp_sendstr(req, "{\"ok\":true}");

    /* Reboot after short delay to apply new WiFi config */
#ifdef FOF_BADGE_VARIANT
    badge_runtime_arm_expected_reboot("http_wifi_config");
#endif
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

/* ── OTA Firmware Update Handler ──────────────────────────────────────── */

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "OTA update started, content_len=%d heap=%lu",
             req->content_len, (unsigned long)esp_get_free_heap_size());

    /* Pause all scanner input + HTTP uploads during OTA to free heap.
     * Without this, scanner UART floods eat heap and crash the OTA. */
    http_upload_pause();
    uart_rx_pause_scanner(0);
#if CONFIG_DUAL_SCANNER
    uart_rx_pause_scanner(1);
#endif
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "UART + uploads paused for OTA, heap=%lu",
             (unsigned long)esp_get_free_heap_size());

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        goto ota_fail;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        goto ota_fail;
    }

    /* Increase socket timeout for large uploads on slow ESP32 WiFi */
    {
        int fd = httpd_req_to_sockfd(req);
        struct timeval tv = { .tv_sec = 120, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    /* Receive firmware in chunks */
    static char buf[4096];
    int received = 0;
    int total = req->content_len;
    int remaining = total;
    int consecutive_timeouts = 0;

    while (remaining > 0) {
        int to_read = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int read_len = httpd_req_recv(req, buf, to_read);
        if (read_len <= 0) {
            if (read_len == HTTPD_SOCK_ERR_TIMEOUT) {
                consecutive_timeouts++;
                if (consecutive_timeouts > 3) {  /* 3 x 60s recv timeout = 3 min max */
                    ESP_LOGE(TAG, "OTA recv: %d consecutive timeouts at %d/%d — aborting",
                             consecutive_timeouts, received, total);
                    esp_ota_abort(ota_handle);
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Timeout");
                    goto ota_fail;
                }
                ESP_LOGW(TAG, "OTA recv timeout %d at %d/%d, retrying...",
                         consecutive_timeouts, received, total);
                continue;
            }
            ESP_LOGE(TAG, "OTA recv error at %d/%d", received, total);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            goto ota_fail;
        }

        consecutive_timeouts = 0;  /* Reset on successful receive */

        err = esp_ota_write(ota_handle, buf, read_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed at %d: %s", received, esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write error");
            goto ota_fail;
        }

        received += read_len;
        remaining -= read_len;

        /* Progress log every 100KB */
        if (received % (100 * 1024) < (int)sizeof(buf)) {
            ESP_LOGI(TAG, "OTA progress: %d/%d bytes (%.0f%%) heap=%lu",
                     received, total, (float)received / total * 100,
                     (unsigned long)esp_get_free_heap_size());
        }
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA validation failed");
        goto ota_fail;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        goto ota_fail;
    }

    ESP_LOGW(TAG, "OTA update successful! %d bytes written to %s. Rebooting...",
             received, update_partition->label);

#ifdef FOF_BADGE_VARIANT
    (void)badge_runtime_arm_reboot_network_hold(
        badge_runtime_get_network_mode(),
        badge_runtime_post_ota_hold_ttl_s(badge_runtime_get_network_mode(), 0)
    );
    badge_runtime_arm_expected_reboot("http_ota");
#endif

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"OTA complete, rebooting...\"}");

    /* Reboot after short delay — no need to resume, we're rebooting */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;

ota_fail:
    /* Resume UART + uploads on failure so the node keeps working */
    uart_rx_resume_scanner(0);
#if CONFIG_DUAL_SCANNER
    uart_rx_resume_scanner(1);
#endif
    http_upload_resume();
    ESP_LOGW(TAG, "OTA failed — UART + uploads resumed");
    return ESP_FAIL;
}

/* ── OTA info endpoint (returns partition state) ─────────────────────── */

static esp_err_t ota_info_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update  = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t app_desc;
    esp_ota_get_partition_description(running, &app_desc);
    const char *app_desc_version = app_desc.version[0] ? app_desc.version : "";

    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\"running_partition\":\"%s\",\"next_partition\":\"%s\","
        "\"app_version\":\"%s\",\"app_desc_version\":\"%s\",\"idf_version\":\"%s\","
        "\"compile_date\":\"%s\",\"compile_time\":\"%s\"}",
        running ? running->label : "?",
        update ? update->label : "?",
        FOF_VERSION, app_desc_version, app_desc.idf_ver,
        app_desc.date, app_desc.time);
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static int wait_for_ota_response_since(int64_t start_ms,
                                       const char *expected_type,
                                       int timeout_ms,
                                       ota_response_t *out)
{
    int64_t deadline_ms = start_ms + timeout_ms;
    while ((esp_timer_get_time() / 1000) < deadline_ms) {
        ota_response_t resp = uart_rx_get_last_ota_response();
        if (resp.type[0] != '\0' && resp.timestamp >= start_ms) {
            if (out) *out = resp;
            if (strcmp(resp.type, expected_type) == 0) {
                return 0;
            }
            if (strcmp(resp.type, "ota_error") == 0 ||
                strcmp(resp.type, "ota_nack") == 0) {
                return -2;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (out) memset(out, 0, sizeof(*out));
    return -1;
}

/* ── OTA Relay: stream firmware to scanner via UART ───────────────────── */
/* Current scanner relay uses CRC32 + ACK + retransmit framing only. */

static esp_err_t ota_relay_handler(httpd_req_t *req)
{
    /* Parse ?uart=ble or ?uart=wifi query param */
    char query[96] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char uart_target[8] = "ble";
    httpd_query_key_value(query, "uart", uart_target, sizeof(uart_target));
    char mode[16] = {0};
    char legacy_param[8] = {0};
    httpd_query_key_value(query, "mode", mode, sizeof(mode));
    httpd_query_key_value(query, "legacy", legacy_param, sizeof(legacy_param));
    bool legacy_mode =
        strcmp(mode, "legacy") == 0 ||
        strcmp(legacy_param, "1") == 0 ||
        strcmp(legacy_param, "true") == 0;

    /* Select UART port */
    uart_port_t uart_num;
    if (strcmp(uart_target, "wifi") == 0) {
        uart_num = CONFIG_WIFI_SCANNER_UART;
    } else {
        uart_num = CONFIG_BLE_SCANNER_UART;
    }
    int scanner_id = (strcmp(uart_target, "wifi") == 0) ? 1 : 0;

    int total = req->content_len;
    ESP_LOGW(TAG, "OTA relay: %d bytes to scanner (uart=%s port=%d legacy=%s) heap=%lu",
             total, uart_target, uart_num, legacy_mode ? "true" : "false",
             (unsigned long)esp_get_free_heap_size());

    if (total < 1024 || total > 2 * 1024 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid firmware size");
        return ESP_FAIL;
    }

    /* Pause uploads to free heap for relay. Keep UART RX alive so ota_ack /
     * ota_done / ota_error responses can still be observed. */
    http_upload_pause();
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_LOGI(TAG, "Uploads paused for streaming relay, heap=%lu",
             (unsigned long)esp_get_free_heap_size());

    /* Increase socket timeout for large uploads */
    {
        int fd = httpd_req_to_sockfd(req);
        struct timeval tv = { .tv_sec = 180, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    /* Step 0: Stop scanner TX to prevent data collision during flash. */
    uart_rx_clear_ota_response();
    int64_t stop_wait_start_ms = esp_timer_get_time() / 1000;
    uart_rx_send_command_to_scanner(scanner_id, "{\"type\":\"stop\"}");
    ESP_LOGI(TAG, "Sent stop command to scanner before OTA relay");
    ota_response_t stop_resp = {0};
    int stop_wait = wait_for_ota_response_since(stop_wait_start_ms,
                                                "stop_ack",
                                                3000,
                                                &stop_resp);
    if (stop_wait != 0) {
        char err_msg[192];
        snprintf(err_msg, sizeof(err_msg),
                 "{\"ok\":false,\"stage\":\"stop\",\"error\":\"command_ingress_unhealthy\","
                 "\"detail\":\"stop_ack_timeout\"}");
        http_upload_resume();
        uart_rx_send_command_to_scanner(scanner_id, "{\"type\":\"start\"}");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, err_msg);
        return ESP_OK;
    }

    /* Step 1: Send OTA begin command */
    uart_rx_clear_ota_response();
    int64_t begin_wait_start_ms = esp_timer_get_time() / 1000;
    uart_write_bytes(uart_num, "\n", 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "{\"type\":\"ota_begin\",\"size\":%d}", total);
    ESP_LOGI(TAG, "OTA relay: sending ota_begin (%d bytes) to UART%d", total, uart_num);
    uart_rx_send_command_to_scanner(scanner_id, cmd);

    ota_response_t ota_resp = {0};
    if (legacy_mode) {
        ESP_LOGW(TAG, "OTA relay legacy parameter ignored: strict ota_ack proof required");
    }
    int ack_wait = wait_for_ota_response_since(begin_wait_start_ms, "ota_ack", 3000, &ota_resp);
    if (ack_wait != 0) {
        char err_msg[192];
        snprintf(err_msg, sizeof(err_msg),
                 "{\"ok\":false,\"stage\":\"begin\",\"error\":\"%s\",\"detail\":\"%s\"}",
                 ack_wait == -2 ? "scanner_error" : "ota_ack_timeout",
                 ota_resp.error[0] ? ota_resp.error : "");
        http_upload_resume();
        uart_rx_send_command_to_scanner(scanner_id, "{\"type\":\"start\"}");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, err_msg);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "OTA relay: scanner acknowledged ota_begin");

    int received = 0;
    int remaining = total;
    uint16_t seq = 0;
    int consecutive_timeouts = 0;
    bool relay_ok = true;

    /* Step 2: Stream HTTP→UART one chunk at a time.
     * Read HTTP in small bites matching UART speed to avoid backpressure. */
    #define RELAY_CHUNK_SIZE  512
    static uint8_t accum_buf[RELAY_CHUNK_SIZE];
    static uint8_t uart_frame[5 + RELAY_CHUNK_SIZE + 4];  /* header + data + CRC32 */
    int accum_pos = 0;

    while (remaining > 0) {
        /* Read only what we need to fill one UART chunk — prevents buffering ahead */
        int need = RELAY_CHUNK_SIZE - accum_pos;
        if (need > remaining) need = remaining;
        int read_len = httpd_req_recv(req, (char *)(accum_buf + accum_pos), need);
        if (read_len <= 0) {
            if (read_len == HTTPD_SOCK_ERR_TIMEOUT) {
                consecutive_timeouts++;
                if (consecutive_timeouts > 3) {
                    ESP_LOGE(TAG, "OTA relay: %d consecutive timeouts at %d/%d",
                             consecutive_timeouts, received, total);
                    uart_rx_send_command_to_scanner(scanner_id, "{\"type\":\"ota_abort\"}");
                    relay_ok = false;
                    break;
                }
                continue;
            }
            ESP_LOGE(TAG, "OTA relay: HTTP recv error at %d/%d", received, total);
            uart_rx_send_command_to_scanner(scanner_id, "{\"type\":\"ota_abort\"}");
            relay_ok = false;
            break;
        }
        consecutive_timeouts = 0;
        accum_pos += read_len;
        remaining -= read_len;

        /* Send when we have a full chunk, or this is the last data */
        if (accum_pos >= RELAY_CHUNK_SIZE || remaining == 0) {
            /* Build frame with CRC32 for integrity */
            uart_frame[0] = OTA_CHUNK_MAGIC;
            uart_frame[1] = (uint8_t)(seq >> 8);
            uart_frame[2] = (uint8_t)(seq & 0xFF);
            uart_frame[3] = (uint8_t)(accum_pos >> 8);
            uart_frame[4] = (uint8_t)(accum_pos & 0xFF);
            memcpy(uart_frame + 5, accum_buf, accum_pos);
            uint32_t crc = esp_rom_crc32_le(0, accum_buf, accum_pos);
            int co = 5 + accum_pos;
            uart_frame[co + 0] = (uint8_t)(crc >> 24);
            uart_frame[co + 1] = (uint8_t)(crc >> 16);
            uart_frame[co + 2] = (uint8_t)(crc >> 8);
            uart_frame[co + 3] = (uint8_t)(crc);

            uart_write_bytes(uart_num, (char *)uart_frame, 5 + accum_pos + 4);
            uart_wait_tx_done(uart_num, pdMS_TO_TICKS(1000));

            received += accum_pos;
            seq++;
            accum_pos = 0;

            /* Pacing: 30ms per chunk.
             * Every 16 chunks (8KB), extra 500ms for scanner flash erase/write.
             * This prevents UART TX buffer from backing up into HTTP recv. */
            vTaskDelay(pdMS_TO_TICKS(30));
            if (seq % 16 == 0) {
                vTaskDelay(pdMS_TO_TICKS(500));
                ESP_LOGI(TAG, "Relay: %d/%d (%.0f%%) seq=%d heap=%lu",
                         received, total, (float)received / total * 100, seq,
                         (unsigned long)esp_get_free_heap_size());
            }
        }
    }

    /* Wait for scanner to process final chunks */
    int64_t final_wait_start_ms = esp_timer_get_time() / 1000;
    http_upload_resume();

    /* Re-enable scanner TX (only on failure — on success scanner is rebooting) */
    if (!relay_ok) {
        uart_rx_send_command_to_scanner(scanner_id, "{\"type\":\"start\"}");
        ESP_LOGI(TAG, "Sent start command to scanner after failed OTA relay");
    }

    ESP_LOGI(TAG, "OTA relay %s: %d bytes, %d chunks to UART%d",
             relay_ok ? "complete" : "FAILED", received, seq, uart_num);

    ota_response_t final_resp = {0};
    if (relay_ok) {
        int done_wait = wait_for_ota_response_since(final_wait_start_ms, "ota_done", 60000, &final_resp);
        if (done_wait != 0) {
            relay_ok = false;
            if (done_wait == -2 && final_resp.error[0] == '\0') {
                snprintf(final_resp.error, sizeof(final_resp.error), "%s", "scanner_error");
            }
        }
    } else {
        final_resp = uart_rx_get_last_ota_response();
    }

    char resp_buf[256];
    snprintf(resp_buf, sizeof(resp_buf),
             "{\"ok\":%s,\"mode\":\"%s\",\"legacy\":%s,\"bytes\":%d,\"chunks\":%d,"
             "\"scanner_response\":\"%s\",\"scanner_error\":\"%s\"}",
             relay_ok ? "true" : "false",
             "streaming",
             legacy_mode ? "true" : "false",
             received, seq,
             final_resp.type[0] ? final_resp.type : (relay_ok ? "ota_done" : "none"),
             final_resp.error[0] ? final_resp.error :
                 (!relay_ok && received == total ? "finalize_timeout" : ""));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp_buf);
    return ESP_OK;
}

/* ── URI registration ──────────────────────────────────────────────────── */

static const httpd_uri_t uri_status_html = {
    .uri      = "/",
    .method   = HTTP_GET,
    .handler  = status_html_handler,
};

static esp_err_t health_json_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static const httpd_uri_t uri_health_json = {
    .uri      = "/health",
    .method   = HTTP_GET,
    .handler  = health_json_handler,
};

static const httpd_uri_t uri_status_json = {
    .uri      = "/api/status",
    .method   = HTTP_GET,
    .handler  = status_json_handler,
};

static const httpd_uri_t uri_setup_html = {
    .uri      = "/setup",
    .method   = HTTP_GET,
    .handler  = setup_html_handler,
};

static const httpd_uri_t uri_scan_json = {
    .uri      = "/api/scan",
    .method   = HTTP_GET,
    .handler  = scan_json_handler,
};

static const httpd_uri_t uri_connect_post = {
    .uri      = "/api/connect",
    .method   = HTTP_POST,
    .handler  = connect_post_handler,
};

static const httpd_uri_t uri_ota_post = {
    .uri      = "/api/ota",
    .method   = HTTP_POST,
    .handler  = ota_post_handler,
};

static const httpd_uri_t uri_ota_info = {
    .uri      = "/api/ota/info",
    .method   = HTTP_GET,
    .handler  = ota_info_handler,
};

static const httpd_uri_t uri_ota_relay = {
    .uri      = "/api/ota/relay",
    .method   = HTTP_POST,
    .handler  = ota_relay_handler,
};

/* ── Badge Control API ───────────────────────────────────────────────── */

static void json_chunk_string(httpd_req_t *req, const char *value)
{
    httpd_resp_send_chunk(req, "\"", 1);
    const char *p = value ? value : "";
    char esc[3] = {'\\', 0, 0};
    char ch[2] = {0, 0};
    while (*p) {
        unsigned char c = (unsigned char)*p++;
        if (c == '"' || c == '\\') {
            esc[1] = (char)c;
            httpd_resp_send_chunk(req, esc, 2);
        } else if (c >= 0x20 && c <= 0x7E) {
            ch[0] = (char)c;
            httpd_resp_send_chunk(req, ch, 1);
        }
    }
    httpd_resp_send_chunk(req, "\"", 1);
}

#ifdef FOF_BADGE_VARIANT
static void badge_status_chunk_display_state(httpd_req_t *req)
{
    oled_badge_display_state_t state;
    bool active = oled_badge_get_display_state(&state);
    char buf[512];

    snprintf(buf, sizeof(buf),
             ",\"display_state\":{\"active\":%s,\"detail_mode\":%s,"
             "\"detail_page\":%d,\"focus_index\":%d,\"focus_total\":%d,"
             "\"item_index\":%d,\"item_total\":%d,\"lane\":",
             active ? "true" : "false",
             state.detail_mode ? "true" : "false",
             state.detail_page,
             state.focus_index,
             state.focus_total,
             state.item_index,
             state.item_total);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    json_chunk_string(req, state.lane);
    httpd_resp_send_chunk(req, ",\"title\":", HTTPD_RESP_USE_STRLEN);
    json_chunk_string(req, state.title);
    httpd_resp_send_chunk(req, ",\"detail\":", HTTPD_RESP_USE_STRLEN);
    json_chunk_string(req, state.detail);
    httpd_resp_send_chunk(req, ",\"evidence\":", HTTPD_RESP_USE_STRLEN);
    json_chunk_string(req, state.evidence);
    httpd_resp_send_chunk(req, ",\"entity_key\":", HTTPD_RESP_USE_STRLEN);
    json_chunk_string(req, state.entity_key);
    httpd_resp_send_chunk(req, ",\"display_id\":", HTTPD_RESP_USE_STRLEN);
    json_chunk_string(req, state.display_id);
    httpd_resp_send_chunk(req, ",\"class\":", HTTPD_RESP_USE_STRLEN);
    json_chunk_string(req, state.threat_class);
    httpd_resp_send_chunk(req, ",\"category\":", HTTPD_RESP_USE_STRLEN);
    json_chunk_string(req, state.category);
    httpd_resp_send_chunk(req, ",\"code\":", HTTPD_RESP_USE_STRLEN);
    json_chunk_string(req, state.code);
    httpd_resp_send_chunk(req, ",\"source\":", HTTPD_RESP_USE_STRLEN);
    json_chunk_string(req, state.source);
    snprintf(buf, sizeof(buf),
             ",\"score\":%d,\"confidence_pct\":%d,\"evidence_quality\":%d,"
             "\"display_rank\":%d,\"age_s\":%d,\"last_seen_s\":%d,"
             "\"rssi\":%d,\"best_rssi\":%d,\"events\":%lu,"
             "\"seen_count\":%lu,\"group_count\":%lu,"
             "\"proximity_level\":%d,\"stale\":%s",
             state.score,
             state.confidence_pct,
             state.evidence_quality,
             state.display_rank,
             state.age_s,
             state.last_seen_s,
             state.rssi,
             state.best_rssi,
             (unsigned long)state.events,
             (unsigned long)state.seen_count,
             (unsigned long)state.group_count,
             state.proximity_level,
             state.stale ? "true" : "false");
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    if (state.has_location) {
        snprintf(buf, sizeof(buf),
                 ",\"lat\":%.7f,\"lon\":%.7f,\"altitude_m\":%.1f",
                 state.latitude, state.longitude, state.altitude_m);
        httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    }
    if (state.has_operator_location) {
        snprintf(buf, sizeof(buf),
                 ",\"operator_lat\":%.7f,\"operator_lon\":%.7f",
                 state.operator_lat, state.operator_lon);
        httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    }
    if (state.operator_id[0] != '\0') {
        httpd_resp_send_chunk(req, ",\"operator_id\":", HTTPD_RESP_USE_STRLEN);
        json_chunk_string(req, state.operator_id);
    }
    httpd_resp_send_chunk(req, "}", HTTPD_RESP_USE_STRLEN);
}
#endif

static esp_err_t badge_status_json_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    badge_threat_snapshot_t snapshot;
    uart_rx_get_badge_threat_snapshot(&snapshot);
    badge_mode_t mode = badge_mode_get();
    char debug_value[8] = {0};
    bool display_debug = nvs_config_get_string("badge_display_debug",
                                               debug_value,
                                               sizeof(debug_value)) &&
                         strcmp(debug_value, "1") == 0;

    char buf[384];
    uint32_t active_remote_id = badge_threat_snapshot_count_active(
        &snapshot,
        BADGE_THREAT_DRONE,
        BADGE_THREAT_CATEGORY_DRONE,
        false
    );
    uint32_t active_drone_ssid = badge_threat_snapshot_count_active(
        &snapshot,
        BADGE_THREAT_DRONE,
        BADGE_THREAT_CATEGORY_SSID,
        false
    );
    snprintf(buf, sizeof(buf),
             "{\"version\":\"%s\",\"mode\":\"%s\",\"mode_label\":",
             FOF_VERSION, badge_mode_to_string(mode));
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    json_chunk_string(req, badge_mode_display_name(mode));

    snprintf(buf, sizeof(buf),
             ",\"display_debug\":%s,"
             "\"ap_enabled\":%s,\"ap_url\":\"http://192.168.4.1\","
             "\"ap_ssid\":",
             display_debug ? "true" : "false",
#ifdef FOF_BADGE_VARIANT
             badge_runtime_get_network_mode() == BADGE_RUNTIME_NETWORK_LOCAL_AP ? "true" : "false"
#else
             badge_mode_ap_enabled(mode) ? "true" : "false"
#endif
             );
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    json_chunk_string(req, wifi_ap_get_ssid());

    snprintf(buf, sizeof(buf),
             ",\"wifi_sta\":%s,\"backend_enabled\":%s,"
             "\"scanner_connected\":%s,\"ble_scanner\":%s,\"wifi_scanner\":%s,"
             "\"threat_score\":%.1f,\"color_rgb565\":%u,"
             "\"dominant_class\":\"%s\",\"dominant_category\":\"%s\","
             "\"dominant_proximity\":%d,"
             "\"counts\":{\"drone\":%lu,\"remote_id\":%lu,"
             "\"drone_ssid\":%lu,\"meta\":%lu,\"tracker\":%lu,"
             "\"wifi_anomaly\":%lu,\"ble\":%lu,\"other\":%lu}",
             wifi_sta_is_connected() ? "true" : "false",
#ifdef FOF_BADGE_VARIANT
             badge_runtime_get_network_mode() == BADGE_RUNTIME_NETWORK_BACKEND ? "true" : "false",
#else
             badge_mode_backend_enabled(mode) ? "true" : "false",
#endif
             uart_rx_is_scanner_connected() ? "true" : "false",
             uart_rx_is_ble_scanner_connected() ? "true" : "false",
             uart_rx_is_wifi_scanner_connected() ? "true" : "false",
             snapshot.threat_score,
             (unsigned)snapshot.color_rgb565,
             badge_threat_class_name(snapshot.dominant_class),
             snapshot.entity_count > 0
                ? badge_threat_category_name(snapshot.entities[0].category)
                : badge_threat_category_name(BADGE_THREAT_CATEGORY_NONE),
             (int)snapshot.dominant_proximity,
             (unsigned long)snapshot.active_counts[BADGE_THREAT_DRONE],
             (unsigned long)active_remote_id,
             (unsigned long)active_drone_ssid,
             (unsigned long)snapshot.active_counts[BADGE_THREAT_META],
             (unsigned long)snapshot.active_counts[BADGE_THREAT_TRACKER],
             (unsigned long)snapshot.active_counts[BADGE_THREAT_WIFI_ANOMALY],
             (unsigned long)snapshot.active_counts[BADGE_THREAT_BLE],
             (unsigned long)snapshot.active_counts[BADGE_THREAT_OTHER]);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);

#ifdef FOF_BADGE_VARIANT
    char policy_json[BADGE_DISPLAY_POLICY_JSON_MAX] = {0};
    badge_display_policy_runtime_json(policy_json, sizeof(policy_json));
    snprintf(buf, sizeof(buf), ",\"display_policy_hash\":%lu,\"display_policy\":",
             (unsigned long)badge_display_policy_runtime_hash());
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req,
                          policy_json[0] ? policy_json : "{\"version\":1,\"classes\":{}}",
                          HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, ",\"filtered_counts\":{", HTTPD_RESP_USE_STRLEN);
    for (int i = 0; i < BADGE_DISPLAY_POLICY_CLASS_COUNT; i++) {
        badge_display_policy_class_t cls = (badge_display_policy_class_t)i;
        snprintf(buf, sizeof(buf), "%s\"%s\":%lu",
                 i == 0 ? "" : ",",
                 badge_display_policy_class_key(cls),
                 (unsigned long)badge_display_policy_runtime_filtered_count(cls));
        httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(req, "}", HTTPD_RESP_USE_STRLEN);
    char theme_json[BADGE_THEME_JSON_MAX] = {0};
    badge_theme_runtime_json(theme_json, sizeof(theme_json));
    snprintf(buf, sizeof(buf), ",\"theme_hash\":%lu,\"theme\":",
             (unsigned long)badge_theme_runtime_hash());
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req,
                          theme_json[0] ? theme_json : "{\"version\":1}",
                          HTTPD_RESP_USE_STRLEN);
    badge_status_chunk_display_state(req);
    char ble_status[192];
    badge_ble_control_status_json(ble_status, sizeof(ble_status));
    httpd_resp_send_chunk(req, ",\"ble_control\":", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req,
                          ble_status[0] ? ble_status : "{\"enabled\":false}",
                          HTTPD_RESP_USE_STRLEN);
#endif

    int64_t last_upload_ms = http_upload_get_last_success_ms();
    int64_t upload_age_s = -1;
    if (last_upload_ms > 0) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        upload_age_s = now_ms >= last_upload_ms ? (now_ms - last_upload_ms) / 1000 : 0;
    }
    httpd_resp_send_chunk(req, ",\"reporting\":{\"network_mode\":", HTTPD_RESP_USE_STRLEN);
#ifdef FOF_BADGE_VARIANT
    json_chunk_string(req, badge_runtime_network_mode_name(
        badge_runtime_get_network_mode()));
    snprintf(buf, sizeof(buf),
             ",\"backend_enabled\":%s,\"network_ttl_s\":%d,"
             "\"reset_reason\":",
             badge_runtime_get_network_mode() == BADGE_RUNTIME_NETWORK_BACKEND ? "true" : "false",
             badge_runtime_get_network_ttl_s());
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    json_chunk_string(req, badge_runtime_last_reset_reason_name());
    snprintf(buf, sizeof(buf),
             ",\"reset_reason_code\":%lu,\"reset_expected\":%s,"
             "\"usb_control_age_s\":%lld,\"recovery_mode\":",
             (unsigned long)badge_runtime_last_reset_reason(),
             badge_runtime_last_reset_expected() ? "true" : "false",
             (long long)badge_runtime_usb_control_age_s());
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    json_chunk_string(req, badge_runtime_recovery_mode());
    snprintf(buf, sizeof(buf),
             ",\"crash_count\":%lu,\"stack_main_free\":%lu,"
             "\"stack_display_free\":%lu,\"stack_usb_free\":%lu,"
             "\"stack_uart_ble_free\":%lu,\"stack_uart_wifi_free\":%lu,"
             "\"heap_internal_free\":%lu,\"heap_internal_min_free\":%lu,"
             "\"heap_internal_largest\":%lu,\"psram_total\":%lu,"
             "\"psram_free\":%lu,\"psram_largest\":%lu",
             (unsigned long)badge_runtime_crash_count(),
             (unsigned long)badge_runtime_main_stack_free(),
             (unsigned long)badge_runtime_display_stack_free(),
             (unsigned long)badge_runtime_usb_stack_free(),
             (unsigned long)badge_runtime_uart_ble_stack_free(),
             (unsigned long)badge_runtime_uart_wifi_stack_free(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
#else
    json_chunk_string(req, badge_mode_to_string(mode));
    snprintf(buf, sizeof(buf),
             ",\"backend_enabled\":%s,\"network_ttl_s\":0",
             badge_mode_backend_enabled(mode) ? "true" : "false");
#endif
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    snprintf(buf, sizeof(buf),
             ",\"wifi_sta\":%s,\"standalone\":%s,"
             "\"uploads_ok\":%d,\"uploads_fail\":%d,"
             "\"last_upload_age_s\":%lld}",
             wifi_sta_is_connected() ? "true" : "false",
             wifi_sta_is_standalone() ? "true" : "false",
             http_upload_get_success_count(),
             http_upload_get_fail_count(),
             (long long)upload_age_s);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);

#ifdef FOF_BADGE_VARIANT
    snprintf(buf, sizeof(buf), ",\"safe_mode\":%s,\"safe_reason\":",
             badge_runtime_is_safe_mode() ? "true" : "false");
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    json_chunk_string(req, badge_runtime_safe_reason());
#endif
    httpd_resp_send_chunk(req, ",\"entities\":[", HTTPD_RESP_USE_STRLEN);

    for (int i = 0; i < snapshot.entity_count; i++) {
        const badge_threat_snapshot_entity_t *entity = &snapshot.entities[i];
        snprintf(buf, sizeof(buf), "%s{\"label\":", i > 0 ? "," : "");
        httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
        json_chunk_string(req, entity->label);
        httpd_resp_send_chunk(req, ",\"detail\":", HTTPD_RESP_USE_STRLEN);
        json_chunk_string(req, entity->detail);
        httpd_resp_send_chunk(req, ",\"evidence\":", HTTPD_RESP_USE_STRLEN);
        json_chunk_string(req, entity->evidence);
        httpd_resp_send_chunk(req, ",\"display_id\":", HTTPD_RESP_USE_STRLEN);
        json_chunk_string(req, entity->display_id);
        snprintf(buf, sizeof(buf),
                 ",\"class\":\"%s\",\"category\":\"%s\",\"code\":\"%s\","
                 "\"source\":\"%s\",\"source_id\":%u,"
                 "\"score\":%d,\"confidence_pct\":%d,\"evidence_quality\":%u,"
                 "\"display_rank\":%d,\"age_s\":%d,"
                 "\"last_seen_s\":%d,\"rssi\":%d,\"best_rssi\":%d,"
                 "\"events\":%lu,\"seen_count\":%lu,\"group_count\":%lu,"
                 "\"proximity_level\":%d,\"stale\":%s",
                 badge_threat_class_name(entity->cls),
                 badge_threat_category_name(entity->category),
                 badge_threat_category_code(entity->category),
                 badge_threat_source_code(entity->source),
                 (unsigned)entity->source,
                 entity->score,
                 entity->confidence_pct,
                 (unsigned)entity->evidence_quality,
                 entity->display_rank,
                 entity->age_s,
                 entity->last_seen_s,
                 entity->rssi,
                 entity->best_rssi,
                 (unsigned long)entity->event_count,
                 (unsigned long)entity->seen_count,
                 (unsigned long)entity->group_count,
                 (int)entity->proximity_level,
                 entity->stale ? "true" : "false");
        httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
        if (entity->has_location) {
            snprintf(buf, sizeof(buf),
                     ",\"lat\":%.7f,\"lon\":%.7f,\"altitude_m\":%.1f",
                     entity->latitude, entity->longitude, entity->altitude_m);
            httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
        }
        if (entity->has_operator_location) {
            snprintf(buf, sizeof(buf),
                     ",\"operator_lat\":%.7f,\"operator_lon\":%.7f",
                     entity->operator_lat, entity->operator_lon);
            httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
        }
        if (entity->operator_id[0] != '\0') {
            httpd_resp_send_chunk(req, ",\"operator_id\":", HTTPD_RESP_USE_STRLEN);
            json_chunk_string(req, entity->operator_id);
        }
        httpd_resp_send_chunk(req, "}", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_send_chunk(req, "],\"scanners\":[", HTTPD_RESP_USE_STRLEN);
    bool badge_ble_connected = uart_rx_is_ble_scanner_connected();
    bool badge_wifi_connected = uart_rx_is_wifi_scanner_connected();
    badge_status_chunk_scanner(req, "ble", 0,
                               badge_ble_connected,
                               badge_wifi_connected,
                               uart_rx_get_ble_scanner_info(),
                               true);
#if CONFIG_DUAL_SCANNER
    badge_status_chunk_scanner(req, "wifi", 1,
                               badge_wifi_connected,
                               badge_ble_connected,
                               uart_rx_get_wifi_scanner_info(),
                               false);
#endif
    httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

#ifdef FOF_BADGE_VARIANT
static int badge_control_ttl_s(const cJSON *root)
{
    const cJSON *ttl = cJSON_GetObjectItemCaseSensitive(root, "ttl_s");
    return cJSON_IsNumber(ttl) ? ttl->valueint : 0;
}

static bool badge_control_bool(const cJSON *root, const char *key, bool fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!item) {
        return fallback;
    }
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    if (cJSON_IsNumber(item)) {
        return item->valueint != 0;
    }
    return fallback;
}

static void badge_control_send_network_result(httpd_req_t *req,
                                              bool applied)
{
    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"applied\":%s,\"network_mode\":\"%s\","
             "\"network_ttl_s\":%d,\"reboot_required\":false}",
             applied ? "true" : "false",
             badge_runtime_network_mode_name(badge_runtime_get_network_mode()),
             badge_runtime_get_network_ttl_s());
    httpd_resp_sendstr(req, buf);
}

static void badge_control_send_display_policy_result(httpd_req_t *req,
                                                     const char *message,
                                                     bool persisted)
{
    bool ble_sent = false;
    bool wifi_sent = false;
    char cmd[BADGE_DISPLAY_POLICY_JSON_MAX + 128] = {0};
    badge_display_policy_runtime_command_json(cmd, sizeof(cmd));
    if (cmd[0]) {
        ble_sent = uart_rx_send_command_to_scanner_checked(0, cmd);
#if CONFIG_DUAL_SCANNER
        wifi_sent = uart_rx_send_command_to_scanner_checked(1, cmd);
#else
        wifi_sent = true;
#endif
    }
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"message\":\"%s\","
             "\"display_policy_hash\":%lu,\"persisted\":%s,"
             "\"ble_sent\":%s,\"wifi_sent\":%s,"
             "\"reboot_required\":false}",
             message ? message : "display policy",
             (unsigned long)badge_display_policy_runtime_hash(),
             persisted ? "true" : "false",
             ble_sent ? "true" : "false",
             wifi_sent ? "true" : "false");
    httpd_resp_sendstr(req, buf);
}
#endif

static esp_err_t badge_control_post_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    char *body = (char *)psram_calloc(1, 2048);
    if (!body) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"body alloc failed\"}");
        return ESP_OK;
    }
    int received = httpd_req_recv(req, body, 2047);
    if (received <= 0) {
        psram_free(body);
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no body\"}");
        return ESP_OK;
    }

    cJSON *root = cJSON_ParseWithLength(body, received);
    psram_free(body);
    if (!root) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid json\"}");
        return ESP_OK;
    }

    const cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    const char *cmd = cJSON_IsString(cmd_item) ? cmd_item->valuestring : "";

    if (strcmp(cmd, "status") == 0) {
        cJSON_Delete(root);
        return badge_status_json_handler(req);
    }
    if (strcmp(cmd, "network") == 0) {
#ifdef FOF_BADGE_VARIANT
        const cJSON *mode_item = cJSON_GetObjectItemCaseSensitive(root, "mode");
        badge_runtime_network_mode_t runtime_mode;
        if (!cJSON_IsString(mode_item) ||
            !badge_runtime_parse_network_mode(mode_item->valuestring, &runtime_mode)) {
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid network mode\"}");
        } else {
            bool applied = badge_runtime_request_network(
                runtime_mode,
                badge_control_ttl_s(root),
                "http"
            );
            badge_control_send_network_result(req, applied);
        }
#else
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"network sessions are badge-only\"}");
#endif
        cJSON_Delete(root);
        return ESP_OK;
    }
    if (strcmp(cmd, "set_mode") == 0) {
        const cJSON *mode_item = cJSON_GetObjectItemCaseSensitive(root, "mode");
#ifdef FOF_BADGE_VARIANT
        badge_runtime_network_mode_t runtime_mode;
        bool persist_mode = badge_control_bool(root, "persist", false);
        if (!cJSON_IsString(mode_item) ||
            !badge_runtime_parse_network_mode(mode_item->valuestring, &runtime_mode)) {
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid mode\"}");
        } else {
            if (persist_mode) {
                badge_mode_t persisted_mode;
                if (badge_mode_parse(mode_item->valuestring, &persisted_mode)) {
                    badge_mode_set(persisted_mode);
                }
            }
            bool applied = badge_runtime_request_network(
                runtime_mode,
                persist_mode ? -1 : badge_control_ttl_s(root),
                "http_set_mode"
            );
            badge_control_send_network_result(req, applied);
        }
#else
        badge_mode_t mode;
        if (!cJSON_IsString(mode_item) ||
            !badge_mode_parse(mode_item->valuestring, &mode)) {
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid mode\"}");
        } else if (badge_mode_set(mode)) {
            httpd_resp_sendstr(req, "{\"ok\":true,\"reboot_required\":true}");
        } else {
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"mode save failed\"}");
        }
#endif
    } else if (strcmp(cmd, "set_backend") == 0) {
        const cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "url");
        const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "wifi_ssid");
        const cJSON *pass = cJSON_GetObjectItemCaseSensitive(root, "wifi_pass");
        const cJSON *enable = cJSON_GetObjectItemCaseSensitive(root, "enable");
        if (cJSON_IsString(url) && url->valuestring[0]) {
            nvs_config_set_string("backend_url", url->valuestring);
        }
        if (cJSON_IsString(ssid) && ssid->valuestring[0]) {
            nvs_config_set_string("wifi_ssid", ssid->valuestring);
        }
        if (cJSON_IsString(pass) && pass->valuestring[0]) {
            nvs_config_set_string("wifi_pass", pass->valuestring);
        }
        if ((cJSON_IsBool(enable) && cJSON_IsTrue(enable)) ||
            (cJSON_IsNumber(enable) && enable->valueint != 0)) {
#ifdef FOF_BADGE_VARIANT
            badge_mode_set(BADGE_MODE_BACKEND);
            bool applied = badge_runtime_request_network(
                BADGE_RUNTIME_NETWORK_BACKEND,
                badge_control_ttl_s(root) > 0 ? badge_control_ttl_s(root) : -1,
                "http_set_backend"
            );
            badge_control_send_network_result(req, applied);
            cJSON_Delete(root);
            return ESP_OK;
#else
            badge_mode_set(BADGE_MODE_BACKEND);
#endif
        }
        httpd_resp_sendstr(req, "{\"ok\":true,\"reboot_required\":true}");
    } else if (strcmp(cmd, "set_display_debug") == 0) {
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        nvs_config_set_string("badge_display_debug",
                              (cJSON_IsBool(enabled) && cJSON_IsTrue(enabled)) ? "1" : "0");
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else if (strcmp(cmd, "badge_display_policy") == 0) {
#ifdef FOF_BADGE_VARIANT
        const cJSON *policy_item = cJSON_GetObjectItemCaseSensitive(root, "policy");
        if (!policy_item) {
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing policy\"}");
        } else {
            char *policy_json = cJSON_PrintUnformatted(policy_item);
            badge_display_policy_t policy;
            char err[64] = {0};
            bool parsed = policy_json &&
                badge_display_policy_parse_json(policy_json, &policy, err, sizeof(err));
            if (policy_json) {
                cJSON_free(policy_json);
            }
            if (!parsed) {
                char resp[128];
                snprintf(resp, sizeof(resp),
                         "{\"ok\":false,\"error\":\"%s\"}",
                         err[0] ? err : "invalid display policy");
                httpd_resp_sendstr(req, resp);
            } else {
                bool persist = badge_control_bool(root, "persist", false);
                if (!badge_display_policy_runtime_set(&policy, persist)) {
                    httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"policy save failed\"}");
                } else {
                    badge_control_send_display_policy_result(req,
                                                            "display policy updated",
                                                            persist);
                }
            }
        }
#else
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"badge-only command\"}");
#endif
    } else if (strcmp(cmd, "badge_display_policy_reset") == 0) {
#ifdef FOF_BADGE_VARIANT
        bool persist = badge_control_bool(root, "persist", false);
        badge_display_policy_runtime_reset(persist);
        badge_control_send_display_policy_result(req,
                                                "display policy reset",
                                                persist);
#else
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"badge-only command\"}");
#endif
    } else if (strcmp(cmd, "badge_theme") == 0) {
#ifdef FOF_BADGE_VARIANT
        const cJSON *theme_item = cJSON_GetObjectItemCaseSensitive(root, "theme");
        if (!theme_item) {
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing theme\"}");
        } else {
            char *theme_json = cJSON_PrintUnformatted(theme_item);
            badge_theme_t theme;
            char err[64] = {0};
            bool parsed = theme_json &&
                badge_theme_parse_json(theme_json, &theme, err, sizeof(err));
            if (theme_json) {
                cJSON_free(theme_json);
            }
            if (!parsed) {
                char resp[128];
                snprintf(resp, sizeof(resp),
                         "{\"ok\":false,\"error\":\"%s\"}",
                         err[0] ? err : "invalid badge theme");
                httpd_resp_sendstr(req, resp);
            } else {
                bool persist = badge_control_bool(root, "persist", false);
                if (!badge_theme_runtime_set(&theme, persist)) {
                    httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"theme save failed\"}");
                } else {
                    char resp[160];
                    snprintf(resp, sizeof(resp),
                             "{\"ok\":true,\"message\":\"badge theme updated\","
                             "\"theme_hash\":%lu,\"persisted\":%s,"
                             "\"reboot_required\":false}",
                             (unsigned long)badge_theme_runtime_hash(),
                             persist ? "true" : "false");
                    httpd_resp_sendstr(req, resp);
                }
            }
        }
#else
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"badge-only command\"}");
#endif
    } else if (strcmp(cmd, "badge_theme_reset") == 0) {
#ifdef FOF_BADGE_VARIANT
        bool persist = badge_control_bool(root, "persist", false);
        badge_theme_runtime_reset(persist);
        char resp[160];
        snprintf(resp, sizeof(resp),
                 "{\"ok\":true,\"message\":\"badge theme reset\","
                 "\"theme_hash\":%lu,\"persisted\":%s,"
                 "\"reboot_required\":false}",
                 (unsigned long)badge_theme_runtime_hash(),
                 persist ? "true" : "false");
        httpd_resp_sendstr(req, resp);
#else
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"badge-only command\"}");
#endif
    } else if (strcmp(cmd, "display_nav") == 0) {
#ifdef FOF_BADGE_VARIANT
        const cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
        if (!cJSON_IsString(action) ||
            !oled_badge_handle_nav_command(action->valuestring)) {
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid display nav action\"}");
        } else {
            httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"display nav updated\",\"reboot_required\":false}");
        }
#else
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"badge-only command\"}");
#endif
    } else if (strcmp(cmd, "reboot") == 0) {
        httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"rebooting\"}");
        cJSON_Delete(root);
#ifdef FOF_BADGE_VARIANT
        badge_runtime_arm_expected_reboot("http_reboot");
#endif
        vTaskDelay(pdMS_TO_TICKS(250));
        esp_restart();
        return ESP_OK;
    } else if (strcmp(cmd, "bootloader") == 0) {
        httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"bootloader\"}");
        cJSON_Delete(root);
#ifdef FOF_BADGE_VARIANT
        badge_runtime_arm_expected_reboot("http_bootloader");
#endif
        vTaskDelay(pdMS_TO_TICKS(250));
        REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
        esp_restart();
        return ESP_OK;
    } else {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"unknown command\"}");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t badge_html_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>FoF Badge</title><style>"
        "body{font-family:-apple-system,system-ui,sans-serif;background:#0d1117;color:#e6edf3;margin:0;padding:16px}"
        ".c{max-width:560px;margin:auto}.r{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:12px;margin:10px 0}"
        "button,select{font:inherit;padding:9px 12px;border-radius:6px;border:1px solid #30363d;background:#1f6feb;color:white;margin:4px}"
        "select{background:#0d1117;color:#e6edf3}.muted{color:#8b949e}.ok{color:#3fb950}.warn{color:#d29922}.err{color:#f85149}"
        "</style></head><body><div class=\"c\"><h1>FoF Badge</h1>"
        "<div class=\"r\"><div id=\"status\" class=\"muted\">Loading...</div></div>"
        "<div class=\"r\"><label>Mode </label><select id=\"mode\"><option value=\"local_ap\">Local AP</option><option value=\"backend\">Backend</option><option value=\"usb_only\">USB Only</option></select>"
        "<button onclick=\"setMode()\">Save Mode</button><button onclick=\"ctl('reboot')\">Reboot</button><button onclick=\"ctl('bootloader')\">Bootloader</button></div>"
        "<div class=\"r\"><label><input id=\"dbg\" type=\"checkbox\"> Display debug</label><button onclick=\"setDebug()\">Save Debug</button></div>"
        "<div class=\"r\"><input id=\"fw\" type=\"file\"><button onclick=\"ota()\">OTA Update</button><div id=\"otaStatus\" class=\"muted\"></div></div>"
        "<div class=\"r\"><a style=\"color:#58a6ff\" href=\"/setup\">Wi-Fi/backend setup</a> · <a style=\"color:#58a6ff\" href=\"/api/status\">debug JSON</a></div>"
        "<script>"
        "async function load(){let r=await fetch('/api/badge/status');let d=await r.json();mode.value=d.mode;dbg.checked=!!d.display_debug;"
        "let ents=(d.entities||[]).map(e=>e.label+' '+e.score).join(' · ')||'Clear';"
        "status.innerHTML='<b>'+d.mode_label+'</b><br>Threat '+Math.round(d.threat_score)+'<br>DRN '+d.counts.drone+' META '+d.counts.meta+' TAG '+d.counts.tracker+'<br>'+ents+'<br><span class=\"muted\">AP '+d.ap_ssid+' · '+d.ap_url+'</span>'}"
        "async function ctl(cmd){await fetch('/api/badge/control',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({cmd})});setTimeout(load,700)}"
        "async function setMode(){await fetch('/api/badge/control',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({cmd:'set_mode',mode:mode.value,persist:true})});load()}"
        "async function setDebug(){await fetch('/api/badge/control',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({cmd:'set_display_debug',enabled:dbg.checked})});load()}"
        "async function ota(){let f=fw.files[0];if(!f){otaStatus.textContent='Choose a firmware .bin';return;}otaStatus.textContent='Uploading...';let r=await fetch('/api/ota',{method:'POST',body:f});otaStatus.textContent=await r.text()}"
        "load();setInterval(load,2000)</script></div></body></html>");
    return ESP_OK;
}

static const httpd_uri_t uri_badge_html = {
    .uri      = "/badge",
    .method   = HTTP_GET,
    .handler  = badge_html_handler,
};

static const httpd_uri_t uri_badge_status_json = {
    .uri      = "/api/badge/status",
    .method   = HTTP_GET,
    .handler  = badge_status_json_handler,
};

static const httpd_uri_t uri_badge_control_post = {
    .uri      = "/api/badge/control",
    .method   = HTTP_POST,
    .handler  = badge_control_post_handler,
};

/* ── Public API ────────────────────────────────────────────────────────── */

static esp_err_t calibration_mode_status_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    const scanner_info_t *ble = uart_rx_get_ble_scanner_info();
#if CONFIG_DUAL_SCANNER
    const scanner_info_t *wifi = uart_rx_get_wifi_scanner_info();
#else
    const scanner_info_t *wifi = NULL;
#endif

    const char *aggregate_mode = aggregate_calibration_scan_mode();
    char resp[720];
    snprintf(
        resp,
        sizeof(resp),
        "{\"ok\":true,\"scan_mode\":\"%s\",\"session_id\":\"%s\",\"calibration_uuid\":\"%s\","
        "\"root_scan_mode\":\"%s\","
        "\"ble\":{\"connected\":%s,\"acked\":%s,\"scan_mode\":\"%s\",\"calibration_uuid\":\"%s\"},"
        "\"wifi\":{\"connected\":%s,\"acked\":%s,\"scan_mode\":\"%s\",\"calibration_uuid\":\"%s\"}}",
        aggregate_mode,
        uart_rx_get_node_calibration_session_id(),
        uart_rx_get_node_calibration_uuid(),
        uart_rx_get_node_scan_mode(),
        uart_rx_is_ble_scanner_connected() ? "true" : "false",
        (ble && ble->calibration_mode_acked) ? "true" : "false",
        (ble && ble->scan_mode[0]) ? ble->scan_mode : "unknown",
        (ble && ble->calibration_uuid[0]) ? ble->calibration_uuid : "",
        uart_rx_is_wifi_scanner_connected() ? "true" : "false",
        (wifi && wifi->calibration_mode_acked) ? "true" : "false",
        (wifi && wifi->scan_mode[0]) ? wifi->scan_mode : "unknown",
        (wifi && wifi->calibration_uuid[0]) ? wifi->calibration_uuid : ""
    );
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t calibration_mode_start_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    const char *aggregate_mode = aggregate_calibration_scan_mode();
    if (strcmp(aggregate_mode, "normal") != 0) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"calibration_mode_not_normal\"}");
        return ESP_OK;
    }

    char body[256] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no_body\"}");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad_json\"}");
        return ESP_OK;
    }

    const char *session_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, JSON_KEY_SESSION_ID));
    const char *cal_uuid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "advertise_uuid"));
    if (!session_id || !cal_uuid || session_id[0] == '\0' || cal_uuid[0] == '\0') {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"session_id_and_advertise_uuid_required\"}");
        return ESP_OK;
    }

    uart_rx_set_node_calibration_mode(true, session_id, cal_uuid);

    char cmd[192];
    snprintf(
        cmd,
        sizeof(cmd),
        "{\"type\":\"%s\",\"session_id\":\"%s\",\"calibration_uuid\":\"%s\"}",
        MSG_TYPE_CAL_MODE_START,
        session_id,
        cal_uuid
    );
    uart_rx_send_command(cmd);

    bool ok = wait_for_node_mode("calibration", cal_uuid, 2500, true);
    if (!ok) {
        snprintf(
            cmd,
            sizeof(cmd),
            "{\"type\":\"%s\",\"session_id\":\"%s\"}",
            MSG_TYPE_CAL_MODE_STOP,
            session_id
        );
        uart_rx_send_command(cmd);
        wait_for_node_mode("normal", "", 1500, false);
        uart_rx_set_node_calibration_mode(false, "", "");
        cJSON_Delete(root);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"calibration_mode_ack_timeout\",\"stage\":\"cal_mode_start\"}");
        return ESP_OK;
    }

    cJSON_Delete(root);
    char resp[320];
    snprintf(
        resp,
        sizeof(resp),
        "{\"ok\":true,\"scan_mode\":\"%s\",\"session_id\":\"%s\",\"calibration_uuid\":\"%s\","
        "\"ble_ack\":%s,\"wifi_ack\":%s}",
        uart_rx_get_node_scan_mode(),
        uart_rx_get_node_calibration_session_id(),
        uart_rx_get_node_calibration_uuid(),
        (uart_rx_get_ble_scanner_info() && uart_rx_get_ble_scanner_info()->calibration_mode_acked) ? "true" : "false",
#if CONFIG_DUAL_SCANNER
        (uart_rx_get_wifi_scanner_info() && uart_rx_get_wifi_scanner_info()->calibration_mode_acked) ? "true" : "false"
#else
        "false"
#endif
    );
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t calibration_mode_stop_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    const char *aggregate_mode = aggregate_calibration_scan_mode();
    if (!uart_rx_is_node_calibration_mode() && strcmp(aggregate_mode, "normal") == 0) {
        httpd_resp_sendstr(req, "{\"ok\":true,\"scan_mode\":\"normal\"}");
        return ESP_OK;
    }

    char cmd[128];
    const char *session_id = uart_rx_get_node_calibration_session_id();
    if (!session_id || session_id[0] == '\0') {
        session_id = "stale";
    }
    snprintf(
        cmd,
        sizeof(cmd),
        "{\"type\":\"%s\",\"session_id\":\"%s\"}",
        MSG_TYPE_CAL_MODE_STOP,
        session_id
    );
    uart_rx_send_command(cmd);
    if (!wait_for_node_mode("normal", "", 2500, false)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(
            req,
            "{\"ok\":false,\"scan_mode\":\"degraded\","
            "\"error\":\"scanner_calibration_stop_ack_timeout\"}"
        );
        return ESP_OK;
    }
    uart_rx_set_node_calibration_mode(false, "", "");
    httpd_resp_sendstr(req, "{\"ok\":true,\"scan_mode\":\"normal\"}");
    return ESP_OK;
}

void http_status_init(void)
{
    static bool s_started = false;
    if (s_started) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port    = CONFIG_HTTP_STATUS_PORT;
    config.task_priority  = CONFIG_HTTP_STATUS_PRIORITY;
    config.stack_size     = CONFIG_HTTP_STATUS_STACK;
    config.max_uri_handlers = 26;   /* status/setup/scan/connect/ota*3/fw*3/cal-mode*3/badge*3/health + headroom */
    config.max_open_sockets = 4;    /* Browsers often hold status, setup, favicon, and API sockets briefly. */
    config.lru_purge_enable = true; /* Close idle connections aggressively */
    config.recv_wait_timeout  = 60;  /* 60s timeout for large OTA uploads */
    config.send_wait_timeout  = 60;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP status server: %s",
                 esp_err_to_name(err));
        return;
    }
    s_started = true;

    esp_err_t r;
    r = httpd_register_uri_handler(server, &uri_status_html);  if (r != ESP_OK) ESP_LOGE(TAG, "Failed /: %s", esp_err_to_name(r));
    r = httpd_register_uri_handler(server, &uri_health_json);  if (r != ESP_OK) ESP_LOGE(TAG, "Failed /health: %s", esp_err_to_name(r));
    r = httpd_register_uri_handler(server, &uri_status_json);  if (r != ESP_OK) ESP_LOGE(TAG, "Failed /api/status: %s", esp_err_to_name(r));
    r = httpd_register_uri_handler(server, &uri_setup_html);   if (r != ESP_OK) ESP_LOGE(TAG, "Failed /setup: %s", esp_err_to_name(r));
    r = httpd_register_uri_handler(server, &uri_scan_json);    if (r != ESP_OK) ESP_LOGE(TAG, "Failed /api/scan: %s", esp_err_to_name(r));
    r = httpd_register_uri_handler(server, &uri_connect_post); if (r != ESP_OK) ESP_LOGE(TAG, "Failed /api/connect: %s", esp_err_to_name(r));
    r = httpd_register_uri_handler(server, &uri_ota_post);     if (r != ESP_OK) ESP_LOGE(TAG, "Failed /api/ota: %s", esp_err_to_name(r));
    r = httpd_register_uri_handler(server, &uri_ota_info);     if (r != ESP_OK) ESP_LOGE(TAG, "Failed /api/ota/info: %s", esp_err_to_name(r));
    r = httpd_register_uri_handler(server, &uri_ota_relay);    if (r != ESP_OK) ESP_LOGE(TAG, "Failed /api/ota/relay: %s", esp_err_to_name(r));
    r = httpd_register_uri_handler(server, &uri_badge_html);   if (r != ESP_OK) ESP_LOGE(TAG, "Failed /badge: %s", esp_err_to_name(r));
    r = httpd_register_uri_handler(server, &uri_badge_status_json); if (r != ESP_OK) ESP_LOGE(TAG, "Failed /api/badge/status: %s", esp_err_to_name(r));
    r = httpd_register_uri_handler(server, &uri_badge_control_post); if (r != ESP_OK) ESP_LOGE(TAG, "Failed /api/badge/control: %s", esp_err_to_name(r));

    /* Scanner firmware store + relay endpoints */
    fw_store_register(server);

    static const httpd_uri_t uri_cal_mode_get = {
        .uri = "/api/calibration/mode", .method = HTTP_GET, .handler = calibration_mode_status_handler };
    static const httpd_uri_t uri_cal_mode_start = {
        .uri = "/api/calibration/mode/start", .method = HTTP_POST, .handler = calibration_mode_start_handler };
    static const httpd_uri_t uri_cal_mode_stop = {
        .uri = "/api/calibration/mode/stop", .method = HTTP_POST, .handler = calibration_mode_stop_handler };
    httpd_register_uri_handler(server, &uri_cal_mode_get);
    httpd_register_uri_handler(server, &uri_cal_mode_start);
    httpd_register_uri_handler(server, &uri_cal_mode_stop);

    ESP_LOGI(TAG, "HTTP status server started on port %d (setup at /setup, calibration mode at /api/calibration/mode*)",
             CONFIG_HTTP_STATUS_PORT);
}
