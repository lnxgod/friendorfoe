/**
 * Friend or Foe -- Embedded HTTP Status Page Implementation
 *
 * Serves a dark-themed, mobile-responsive HTML page with live system
 * status and a JSON API endpoint.  Uses chunked responses to keep
 * stack usage low (~4KB).
 */

#include "http_status.h"
#include "config.h"

#include <string.h>
#include <stdio.h>
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_log.h"

/* Subsystem headers for status getters */
#include "uart_rx.h"
#include "gps.h"
#include "wifi_sta.h"
#include "wifi_ap.h"
#include "battery.h"
#include "http_upload.h"
#include "nvs_config.h"

static const char *TAG = "http_status";

/* ── Source name lookup ─────────────────────────────────────────────────── */

static const char *source_name(uint8_t src)
{
    switch (src) {
        case 0: return "BLE RID";
        case 1: return "WiFi SSID";
        case 2: return "DJI IE";
        case 3: return "WiFi Beacon";
        case 4: return "WiFi OUI";
        default: return "Unknown";
    }
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

    /* ── Network section ───────────────────────────────────────────────── */
    httpd_resp_send_chunk(req, "<h2>Network</h2><div class=\"g\">", HTTPD_RESP_USE_STRLEN);

    snprintf(buf, sizeof(buf),
        "<div class=\"r\"><div class=\"l\">WiFi STA</div>"
        "<div class=\"v %s\">%s</div></div>",
        wifi_ok ? "ok" : "err",
        wifi_ok ? "Connected" : "Disconnected");
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);

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

    /* ── Detections section ────────────────────────────────────────────── */
    httpd_resp_send_chunk(req, "<h2>Detections</h2><div class=\"g\">", HTTPD_RESP_USE_STRLEN);

    snprintf(buf, sizeof(buf),
        "<div class=\"r\"><div class=\"l\">Total Detected</div>"
        "<div class=\"v\">%d</div></div>"
        "<div class=\"r\"><div class=\"l\">Uploaded / Failed</div>"
        "<div class=\"v\"><span class=\"ok\">%d</span> / "
        "<span class=\"%s\">%d</span></div></div>",
        det_count, upload_ok,
        upload_fail > 0 ? "err" : "ok", upload_fail);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);

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

    char buf[512];

    /* Open JSON object */
    snprintf(buf, sizeof(buf),
        "{\"device_id\":\"%s\",\"uptime_s\":%lld,"
        "\"gps\":{\"fix\":%s,\"lat\":%.6f,\"lon\":%.6f,\"satellites\":%d},"
        "\"wifi_sta\":%s,\"ap_clients\":%d,"
        "\"battery\":{\"percent\":%.1f,\"voltage\":%.2f},"
        "\"detections\":%d,\"uploads_ok\":%d,\"uploads_fail\":%d,",
        device_id, (long long)uptime_sec,
        gps_fix ? "true" : "false", gps.latitude, gps.longitude, gps.satellites,
        wifi_ok ? "true" : "false", ap_clients,
        batt_pct, batt_v,
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

/* ── Public API ────────────────────────────────────────────────────────── */

void http_status_init(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port    = CONFIG_HTTP_STATUS_PORT;
    config.task_priority  = CONFIG_HTTP_STATUS_PRIORITY;
    config.stack_size     = CONFIG_HTTP_STATUS_STACK;
    config.max_uri_handlers = 4;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP status server: %s",
                 esp_err_to_name(err));
        return;
    }

    httpd_register_uri_handler(server, &uri_status_html);
    httpd_register_uri_handler(server, &uri_status_json);

    ESP_LOGI(TAG, "HTTP status server started on port %d",
             CONFIG_HTTP_STATUS_PORT);
}
