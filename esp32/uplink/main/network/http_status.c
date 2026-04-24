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
#include "nvs_config.h"
#include "version.h"

static const char *TAG = "http_status";

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
        default: return "Unknown";
    }
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

    char buf[1400];

    /* Heap / PSRAM snapshot. On S3 N16R8 boards this surfaces how much of
     * the 8 MB PSRAM is in use; on legacy / non-PSRAM boards both psram_*
     * values report 0 (graceful degrade per esp32/shared/psram_alloc). */
    size_t heap_internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t heap_internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t psram_free  = psram_free_size();
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
    snprintf(buf, sizeof(buf),
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
        (unsigned)psram_free, (unsigned)psram_total,
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

    /* Recent detections array */
    httpd_resp_send_chunk(req, "\"recent\":[", HTTPD_RESP_USE_STRLEN);

    detection_summary_t recent[8];
    int n = uart_rx_get_recent_detections(recent, 8);
    int64_t now_ms = esp_timer_get_time() / 1000;

    for (int i = 0; i < n; i++) {
        int age_sec = (int)((now_ms - recent[i].timestamp_ms) / 1000);
        snprintf(buf, sizeof(buf),
            "%s{\"drone_id\":\"%s\",\"source\":\"%s\","
            "\"confidence\":%.2f,\"rssi\":%d,\"age_s\":%d}",
            i > 0 ? "," : "",
            recent[i].drone_id, source_name(recent[i].source),
            recent[i].confidence, recent[i].rssi, age_sec);
        httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN);

    /* Finish */
    httpd_resp_send_chunk(req, NULL, 0);
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

    char buf[256];
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
    const char *reported_version =
        (app_desc.version[0] != '\0' && strcmp(app_desc.version, "1") != 0)
            ? app_desc.version
            : FOF_VERSION;

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"running_partition\":\"%s\",\"next_partition\":\"%s\","
        "\"app_version\":\"%s\",\"idf_version\":\"%s\","
        "\"compile_date\":\"%s\",\"compile_time\":\"%s\"}",
        running ? running->label : "?",
        update ? update->label : "?",
        reported_version, app_desc.idf_ver,
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
/* Fallback for nodes without fw_store partition. Prefer /api/fw/upload +
 * /api/fw/relay when available (CRC32 + ACK + retransmit). */

static esp_err_t ota_relay_handler(httpd_req_t *req)
{
    /* Parse ?uart=ble or ?uart=wifi query param */
    char query[32] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char uart_target[8] = "ble";
    char legacy_flag[8] = {0};
    httpd_query_key_value(query, "uart", uart_target, sizeof(uart_target));
    httpd_query_key_value(query, "legacy", legacy_flag, sizeof(legacy_flag));
    const bool legacy_mode =
        (strcmp(legacy_flag, "1") == 0) ||
        (strcasecmp(legacy_flag, "true") == 0) ||
        (strcasecmp(legacy_flag, "yes") == 0);

    /* Select UART port */
    uart_port_t uart_num;
#if defined(UPLINK_ESP32) || defined(UPLINK_ESP32S3)
    if (strcmp(uart_target, "wifi") == 0) {
        uart_num = CONFIG_WIFI_SCANNER_UART;
    } else {
        uart_num = CONFIG_BLE_SCANNER_UART;
    }
#else
    uart_num = CONFIG_BLE_SCANNER_UART;
#endif

    int total = req->content_len;
    ESP_LOGW(TAG, "OTA relay (%s): %d bytes to scanner (uart=%s port=%d) heap=%lu",
             legacy_mode ? "legacy" : "streaming",
             total, uart_target, uart_num, (unsigned long)esp_get_free_heap_size());

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

    if (!legacy_mode) {
        /* Step 0: Stop scanner TX to prevent data collision during flash.
         * Legacy scanners pre-date the explicit stop/stop_ack handshake, so
         * the compatibility path skips this and uses the older plain stream
         * framing they already understand. */
        const char *stop_cmd = "{\"type\":\"stop\"}\n";
        uart_write_bytes(uart_num, stop_cmd, strlen(stop_cmd));
        ESP_LOGI(TAG, "Sent stop command to scanner before OTA relay");
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* Step 1: Send OTA begin command */
    uart_rx_clear_ota_response();
    int64_t begin_wait_start_ms = esp_timer_get_time() / 1000;
    uart_write_bytes(uart_num, "\n", 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "{\"type\":\"ota_begin\",\"size\":%d}\n", total);
    ESP_LOGI(TAG, "OTA relay: sending ota_begin (%d bytes) to UART%d", total, uart_num);
    uart_write_bytes(uart_num, cmd, strlen(cmd));

    if (!legacy_mode) {
        ota_response_t ota_resp = {0};
        int ack_wait = wait_for_ota_response_since(begin_wait_start_ms, "ota_ack", 3000, &ota_resp);
        if (ack_wait != 0) {
            char err_msg[192];
            snprintf(err_msg, sizeof(err_msg),
                     "{\"ok\":false,\"stage\":\"begin\",\"error\":\"%s\",\"detail\":\"%s\"}",
                     ack_wait == -2 ? "scanner_error" : "ota_ack_timeout",
                     ota_resp.error[0] ? ota_resp.error : "");
            http_upload_resume();
            uart_write_bytes(uart_num, "{\"type\":\"start\"}\n", 17);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, err_msg);
            return ESP_OK;
        }
        ESP_LOGI(TAG, "OTA relay: scanner acknowledged ota_begin");
    }

    /* UART RX already paused above — identify target scanner for logging */
    int scanner_id = (strcmp(uart_target, "wifi") == 0) ? 1 : 0;
    (void)scanner_id;  /* Used in resume path */

    int received = 0;
    int remaining = total;
    uint16_t seq = 0;
    int consecutive_timeouts = 0;
    bool relay_ok = true;

    if (legacy_mode) {
        /* Legacy scanner OTA (pre-v0.59):
         *   ota_begin -> [magic][seq][len]+data -> 0xFF abort seq -> ota_end
         * Older field scanners report versions fine but do not understand the
         * newer CRC-trailer framing or the stop/stop_ack handshake. */
        #define LEGACY_RELAY_CHUNK_SIZE 256
        static uint8_t legacy_buf[LEGACY_RELAY_CHUNK_SIZE];
        static uint8_t legacy_hdr[OTA_CHUNK_HEADER_SIZE];

        while (remaining > 0) {
            int to_read = remaining > LEGACY_RELAY_CHUNK_SIZE ? LEGACY_RELAY_CHUNK_SIZE : remaining;
            int read_len = httpd_req_recv(req, (char *)legacy_buf, to_read);
            if (read_len <= 0) {
                if (read_len == HTTPD_SOCK_ERR_TIMEOUT) {
                    consecutive_timeouts++;
                    if (consecutive_timeouts > 3) {
                        ESP_LOGE(TAG, "Legacy OTA relay: %d consecutive timeouts at %d/%d",
                                 consecutive_timeouts, received, total);
                        uart_write_bytes(uart_num, "{\"type\":\"ota_abort\"}\n", 20);
                        relay_ok = false;
                        break;
                    }
                    continue;
                }
                ESP_LOGE(TAG, "Legacy OTA relay: HTTP recv error at %d/%d", received, total);
                uart_write_bytes(uart_num, "{\"type\":\"ota_abort\"}\n", 20);
                relay_ok = false;
                break;
            }

            consecutive_timeouts = 0;
            legacy_hdr[0] = OTA_CHUNK_MAGIC;
            legacy_hdr[1] = (uint8_t)(seq >> 8);
            legacy_hdr[2] = (uint8_t)(seq & 0xFF);
            legacy_hdr[3] = (uint8_t)(read_len >> 8);
            legacy_hdr[4] = (uint8_t)(read_len & 0xFF);

            uart_write_bytes(uart_num, (char *)legacy_hdr, OTA_CHUNK_HEADER_SIZE);
            uart_write_bytes(uart_num, (char *)legacy_buf, read_len);
            uart_wait_tx_done(uart_num, pdMS_TO_TICKS(1000));

            received += read_len;
            remaining -= read_len;
            seq++;

            /* Conservative pacing keeps the legacy direct-to-flash writer from
             * overrunning when it cannot NACK/retransmit the way v0.59+ can. */
            vTaskDelay(pdMS_TO_TICKS(20));
            if (seq % OTA_ACK_INTERVAL_CHUNKS == 0) {
                vTaskDelay(pdMS_TO_TICKS(120));
                ESP_LOGI(TAG, "Legacy relay: %d/%d (%.0f%%) seq=%d heap=%lu",
                         received, total, (float)received / total * 100, seq,
                         (unsigned long)esp_get_free_heap_size());
            }
        }

        if (relay_ok) {
            const char *end_cmd = "{\"type\":\"ota_end\"}\n";
            vTaskDelay(pdMS_TO_TICKS(500));
            uart_write_bytes(uart_num, end_cmd, strlen(end_cmd));
        }
    } else {
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
                        uart_write_bytes(uart_num, "{\"type\":\"ota_abort\"}\n", 20);
                        relay_ok = false;
                        break;
                    }
                    continue;
                }
                ESP_LOGE(TAG, "OTA relay: HTTP recv error at %d/%d", received, total);
                uart_write_bytes(uart_num, "{\"type\":\"ota_abort\"}\n", 20);
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
    }

    /* Wait for scanner to process final chunks */
    int64_t final_wait_start_ms = esp_timer_get_time() / 1000;
    http_upload_resume();

    /* Re-enable scanner TX (only on failure — on success scanner is rebooting) */
    if (!relay_ok) {
        const char *start_cmd = "{\"type\":\"start\"}\n";
        uart_write_bytes(uart_num, start_cmd, strlen(start_cmd));
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
             "{\"ok\":%s,\"mode\":\"%s\",\"bytes\":%d,\"chunks\":%d,"
             "\"scanner_response\":\"%s\",\"scanner_error\":\"%s\"}",
             relay_ok ? "true" : "false",
             legacy_mode ? "legacy" : "streaming",
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

    char resp[640];
    snprintf(
        resp,
        sizeof(resp),
        "{\"ok\":true,\"scan_mode\":\"%s\",\"session_id\":\"%s\",\"calibration_uuid\":\"%s\","
        "\"ble\":{\"connected\":%s,\"acked\":%s,\"scan_mode\":\"%s\",\"calibration_uuid\":\"%s\"},"
        "\"wifi\":{\"connected\":%s,\"acked\":%s,\"scan_mode\":\"%s\",\"calibration_uuid\":\"%s\"}}",
        uart_rx_get_node_scan_mode(),
        uart_rx_get_node_calibration_session_id(),
        uart_rx_get_node_calibration_uuid(),
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
    if (uart_rx_is_node_calibration_mode()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"calibration_mode_already_active\"}");
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
    if (!uart_rx_is_node_calibration_mode()) {
        httpd_resp_sendstr(req, "{\"ok\":true,\"scan_mode\":\"normal\"}");
        return ESP_OK;
    }

    char cmd[128];
    snprintf(
        cmd,
        sizeof(cmd),
        "{\"type\":\"%s\",\"session_id\":\"%s\"}",
        MSG_TYPE_CAL_MODE_STOP,
        uart_rx_get_node_calibration_session_id()
    );
    uart_rx_send_command(cmd);
    wait_for_node_mode("normal", "", 2500, false);
    uart_rx_set_node_calibration_mode(false, "", "");
    httpd_resp_sendstr(req, "{\"ok\":true,\"scan_mode\":\"normal\"}");
    return ESP_OK;
}

void http_status_init(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port    = CONFIG_HTTP_STATUS_PORT;
    config.task_priority  = CONFIG_HTTP_STATUS_PRIORITY;
    config.stack_size     = 6144;   /* Minimal — relay uses static buffers, not stack */
    config.max_uri_handlers = 20;   /* status/setup/scan/connect/ota*3/fw*3/cal-mode*3 + headroom */
    config.max_open_sockets = 1;  /* Single connection only — saves ~4KB per socket */
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

    esp_err_t r;
    r = httpd_register_uri_handler(server, &uri_status_html);  if (r != ESP_OK) ESP_LOGE(TAG, "Failed /: %s", esp_err_to_name(r));
    r = httpd_register_uri_handler(server, &uri_status_json);  if (r != ESP_OK) ESP_LOGE(TAG, "Failed /api/status: %s", esp_err_to_name(r));
    r = httpd_register_uri_handler(server, &uri_setup_html);   if (r != ESP_OK) ESP_LOGE(TAG, "Failed /setup: %s", esp_err_to_name(r));
    r = httpd_register_uri_handler(server, &uri_scan_json);    if (r != ESP_OK) ESP_LOGE(TAG, "Failed /api/scan: %s", esp_err_to_name(r));
    r = httpd_register_uri_handler(server, &uri_connect_post); if (r != ESP_OK) ESP_LOGE(TAG, "Failed /api/connect: %s", esp_err_to_name(r));
    r = httpd_register_uri_handler(server, &uri_ota_post);     if (r != ESP_OK) ESP_LOGE(TAG, "Failed /api/ota: %s", esp_err_to_name(r));
    r = httpd_register_uri_handler(server, &uri_ota_info);     if (r != ESP_OK) ESP_LOGE(TAG, "Failed /api/ota/info: %s", esp_err_to_name(r));
    r = httpd_register_uri_handler(server, &uri_ota_relay);    if (r != ESP_OK) ESP_LOGE(TAG, "Failed /api/ota/relay: %s", esp_err_to_name(r));

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
