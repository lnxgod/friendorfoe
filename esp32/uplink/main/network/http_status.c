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

    char buf[640];

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

    /* Open JSON object */
    snprintf(buf, sizeof(buf),
        "{\"device_id\":\"%s\",\"uptime_s\":%lld,"
        "\"gps\":{\"fix\":%s,\"lat\":%.6f,\"lon\":%.6f,\"satellites\":%d},"
        "\"wifi_sta\":%s,\"ap_clients\":%d,"
        "\"standalone\":%s,\"scanner_connected\":%s,"
        "\"battery\":{\"percent\":%.1f,\"voltage\":%.2f},"
        "\"heap\":{\"internal_free\":%u,\"internal_total\":%u,"
                  "\"psram_free\":%u,\"psram_total\":%u},"
        "\"offline_queue\":{\"depth\":%d,\"capacity\":%d},"
        "\"time_sync\":{\"last_epoch_ms\":%lld,\"perf\":%d,\"status\":%d,\"clen\":%d,\"nread\":%d,\"bcasts\":%u},"
        "\"detections\":%d,\"uploads_ok\":%d,\"uploads_fail\":%d,",
        device_id, (long long)uptime_sec,
        gps_fix ? "true" : "false", gps.latitude, gps.longitude, gps.satellites,
        wifi_ok ? "true" : "false", ap_clients,
        standalone ? "true" : "false",
        scanner_ok ? "true" : "false",
        batt_pct, batt_v,
        (unsigned)heap_internal_free, (unsigned)heap_internal_total,
        (unsigned)psram_free, (unsigned)psram_total,
        http_upload_get_offline_count(), http_upload_get_offline_capacity(),
        (long long)g_last_backend_epoch_ms,
        g_last_time_fetch_perf, g_last_time_fetch_status,
        g_last_time_fetch_clen, g_last_time_fetch_nread,
        (unsigned)g_time_broadcast_count,
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

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"running_partition\":\"%s\",\"next_partition\":\"%s\","
        "\"app_version\":\"%s\",\"idf_version\":\"%s\","
        "\"compile_date\":\"%s\",\"compile_time\":\"%s\"}",
        running ? running->label : "?",
        update ? update->label : "?",
        app_desc.version, app_desc.idf_ver,
        app_desc.date, app_desc.time);
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
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
    httpd_query_key_value(query, "uart", uart_target, sizeof(uart_target));

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
    ESP_LOGW(TAG, "OTA relay (streaming): %d bytes to scanner (uart=%s port=%d) heap=%lu",
             total, uart_target, uart_num, (unsigned long)esp_get_free_heap_size());

    if (total < 1024 || total > 2 * 1024 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid firmware size");
        return ESP_FAIL;
    }

    /* Pause all UART RX + HTTP uploads to free heap for relay */
    http_upload_pause();
    uart_rx_pause_scanner(0);
#if CONFIG_DUAL_SCANNER
    uart_rx_pause_scanner(1);
#endif
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_LOGI(TAG, "UART + uploads paused for streaming relay, heap=%lu",
             (unsigned long)esp_get_free_heap_size());

    /* Increase socket timeout for large uploads */
    {
        int fd = httpd_req_to_sockfd(req);
        struct timeval tv = { .tv_sec = 180, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    /* Step 0: Stop scanner TX to prevent data collision during flash */
    {
        const char *stop_cmd = "{\"type\":\"stop\"}\n";
        uart_write_bytes(uart_num, stop_cmd, strlen(stop_cmd));
        ESP_LOGI(TAG, "Sent stop command to scanner before OTA relay");
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* Step 1: Send OTA begin command */
    uart_rx_clear_ota_response();
    uart_write_bytes(uart_num, "\n", 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "{\"type\":\"ota_begin\",\"size\":%d}\n", total);
    ESP_LOGI(TAG, "OTA relay: sending ota_begin (%d bytes) to UART%d", total, uart_num);
    uart_write_bytes(uart_num, cmd, strlen(cmd));
    vTaskDelay(pdMS_TO_TICKS(2000));  /* Give scanner time to prepare partition */

    /* Check if scanner acknowledged the OTA begin */
    ota_response_t ota_resp = uart_rx_get_last_ota_response();
    if (ota_resp.type[0]) {
        ESP_LOGW(TAG, "OTA relay: scanner responded: type=%s error=%s",
                 ota_resp.type, ota_resp.error);
        if (strcmp(ota_resp.type, "ota_error") == 0) {
            char err_msg[128];
            snprintf(err_msg, sizeof(err_msg),
                     "{\"ok\":false,\"error\":\"scanner_error\",\"detail\":\"%s\"}", ota_resp.error);
            /* Resume everything before returning on error */
            uart_rx_resume_scanner(0);
#if CONFIG_DUAL_SCANNER
            uart_rx_resume_scanner(1);
#endif
            http_upload_resume();
            uart_write_bytes(uart_num, "{\"type\":\"start\"}\n", 17);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, err_msg);
            return ESP_OK;
        }
    } else {
        ESP_LOGW(TAG, "OTA relay: no scanner response (may not support OTA yet)");
    }

    /* UART RX already paused above — identify target scanner for logging */
    int scanner_id = (strcmp(uart_target, "wifi") == 0) ? 1 : 0;
    (void)scanner_id;  /* Used in resume path */

    /* Step 2: Stream HTTP→UART one chunk at a time.
     * Read HTTP in small bites matching UART speed to avoid backpressure. */
    #define RELAY_CHUNK_SIZE  512
    static uint8_t accum_buf[RELAY_CHUNK_SIZE];
    static uint8_t uart_frame[5 + RELAY_CHUNK_SIZE + 4];  /* header + data + CRC32 */
    int received = 0;
    int remaining = total;
    uint16_t seq = 0;
    int accum_pos = 0;
    int consecutive_timeouts = 0;
    bool relay_ok = true;

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

    /* Wait for scanner to process final chunks */
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* Resume ALL paused tasks */
    uart_rx_resume_scanner(0);
#if CONFIG_DUAL_SCANNER
    uart_rx_resume_scanner(1);
#endif
    http_upload_resume();

    /* Re-enable scanner TX (only on failure — on success scanner is rebooting) */
    if (!relay_ok) {
        const char *start_cmd = "{\"type\":\"start\"}\n";
        uart_write_bytes(uart_num, start_cmd, strlen(start_cmd));
        ESP_LOGI(TAG, "Sent start command to scanner after failed OTA relay");
    }

    ESP_LOGI(TAG, "OTA relay %s: %d bytes, %d chunks to UART%d",
             relay_ok ? "complete" : "FAILED", received, seq, uart_num);

    /* Check final OTA response from scanner */
    vTaskDelay(pdMS_TO_TICKS(1000));
    ota_response_t final_resp = uart_rx_get_last_ota_response();

    char resp_buf[256];
    snprintf(resp_buf, sizeof(resp_buf),
             "{\"ok\":%s,\"bytes\":%d,\"chunks\":%d,"
             "\"scanner_response\":\"%s\",\"scanner_error\":\"%s\"}",
             relay_ok ? "true" : "false",
             received, seq,
             final_resp.type[0] ? final_resp.type : "none",
             final_resp.error[0] ? final_resp.error : "");

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

/* ══════════════════════════════════════════════════════════════════════════
 * Calibration Mode — Inter-node RSSI measurement for path loss tuning
 *
 * Protocol:
 *   1. Backend tells one node to broadcast: POST /api/calibrate/start?power=80&channel=6
 *   2. Backend tells all OTHER nodes to measure: POST /api/calibrate/measure?bssid=XX&channel=6&duration=90
 *   3. Measuring nodes pause all scanners, lock to channel, average RSSI for 90s
 *   4. Backend collects results: GET /api/calibrate/result
 *   5. Backend tells broadcaster to change power: POST /api/calibrate/power?level=60
 *   6. Repeat measurements at different power levels
 *   7. POST /api/calibrate/stop → node reboots to free RAM and return to normal
 *
 * Power levels: units of 0.25dBm (8=2dBm, 20=5dBm, 40=10dBm, 60=15dBm, 80=20dBm)
 * ══════════════════════════════════════════════════════════════════════════ */

/* Calibration measurement state (static — no heap allocation) */
#define CAL_MAX_SAMPLES 500
static struct {
    bool     active;
    char     target_bssid[18];
    int      channel;
    int      total_rssi;
    int      sample_count;
    int      min_rssi;
    int      max_rssi;
    int64_t  start_ms;
    int64_t  duration_ms;
} s_cal = {0};

static esp_err_t calibrate_start_handler(httpd_req_t *req)
{
    /* Parse power level and channel from query */
    char query[64] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char power_str[4] = "80";  /* Default 20dBm */
    char ch_str[4] = "6";
    httpd_query_key_value(query, "power", power_str, sizeof(power_str));
    httpd_query_key_value(query, "channel", ch_str, sizeof(ch_str));
    int power = atoi(power_str);
    int channel = atoi(ch_str);
    if (power < 8) power = 8;
    if (power > 84) power = 84;
    if (channel < 1 || channel > 13) channel = 6;

    /* Pause all scanner UART input during calibration */
    http_upload_pause();
    uart_rx_pause_scanner(0);
#if CONFIG_DUAL_SCANNER
    uart_rx_pause_scanner(1);
#endif

    /* Enable AP on specified channel */
    wifi_ap_start();

    /* Set TX power */
    esp_wifi_set_max_tx_power((int8_t)power);

    /* Read back actual power */
    int8_t actual_power = 0;
    esp_wifi_get_max_tx_power(&actual_power);

    /* Get BSSID */
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    const char *ssid = wifi_ap_get_ssid();

    char resp[196];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"bssid\":\"%s\",\"ssid\":\"%s\",\"channel\":%d,"
             "\"power_quarter_dbm\":%d,\"power_dbm\":%.1f}",
             bssid, ssid, channel, actual_power, actual_power * 0.25f);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);

    ESP_LOGW(TAG, "CALIBRATION: AP enabled (SSID=%s BSSID=%s ch=%d power=%.1fdBm)",
             ssid, bssid, channel, actual_power * 0.25f);
    return ESP_OK;
}

static esp_err_t calibrate_stop_handler(httpd_req_t *req)
{
    s_cal.active = false;
    wifi_ap_stop();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");

    ESP_LOGW(TAG, "CALIBRATION: complete — rebooting to free RAM and return to normal mode");

    /* Reboot after brief delay to free all calibration RAM */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK;  /* unreachable */
}

static esp_err_t calibrate_power_handler(httpd_req_t *req)
{
    /* Change TX power while AP is broadcasting */
    char query[32] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char level_str[4] = "80";
    httpd_query_key_value(query, "level", level_str, sizeof(level_str));
    int level = atoi(level_str);
    if (level < 8) level = 8;
    if (level > 84) level = 84;

    esp_wifi_set_max_tx_power((int8_t)level);

    int8_t actual = 0;
    esp_wifi_get_max_tx_power(&actual);

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"power_dbm\":%.1f}", actual * 0.25f);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);

    ESP_LOGI(TAG, "CALIBRATION: TX power → %.1f dBm", actual * 0.25f);
    return ESP_OK;
}

static esp_err_t calibrate_measure_handler(httpd_req_t *req)
{
    /* Parse target BSSID, channel, and duration */
    char query[128] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char target_bssid[18] = {0};
    char ch_str[4] = "6";
    char dur_str[4] = "90";
    httpd_query_key_value(query, "bssid", target_bssid, sizeof(target_bssid));
    httpd_query_key_value(query, "channel", ch_str, sizeof(ch_str));
    httpd_query_key_value(query, "duration", dur_str, sizeof(dur_str));
    int channel = atoi(ch_str);
    int duration = atoi(dur_str);
    if (duration < 5) duration = 5;
    if (duration > 120) duration = 120;

    ESP_LOGW(TAG, "CALIBRATION: measuring BSSID=%s ch=%d for %ds", target_bssid, channel, duration);

    /* Pause all scanner input during measurement */
    http_upload_pause();
    uart_rx_pause_scanner(0);
#if CONFIG_DUAL_SCANNER
    uart_rx_pause_scanner(1);
#endif

    /* Reset measurement state */
    s_cal.active = true;
    strncpy(s_cal.target_bssid, target_bssid, sizeof(s_cal.target_bssid) - 1);
    s_cal.channel = channel;
    s_cal.total_rssi = 0;
    s_cal.sample_count = 0;
    s_cal.min_rssi = 0;
    s_cal.max_rssi = -127;
    s_cal.start_ms = esp_timer_get_time() / 1000;
    s_cal.duration_ms = duration * 1000;

    /* Run passive scans on the target channel for the full duration */
    int64_t end_ms = s_cal.start_ms + s_cal.duration_ms;

    while ((esp_timer_get_time() / 1000) < end_ms && s_cal.active) {
        wifi_scan_config_t cfg = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = (uint8_t)channel,
            .show_hidden = true,
            .scan_type = WIFI_SCAN_TYPE_PASSIVE,
            .scan_time.passive = 500,  /* 500ms dwell for more samples */
        };

        if (esp_wifi_scan_start(&cfg, true) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count > 0 && ap_count < 64) {
            /* Use static buffer to avoid heap allocation */
            static wifi_ap_record_t ap_buf[64];
            uint16_t max_records = ap_count > 64 ? 64 : ap_count;
            esp_wifi_scan_get_ap_records(&max_records, ap_buf);

            for (int i = 0; i < max_records; i++) {
                char found[18];
                snprintf(found, sizeof(found), "%02X:%02X:%02X:%02X:%02X:%02X",
                         ap_buf[i].bssid[0], ap_buf[i].bssid[1], ap_buf[i].bssid[2],
                         ap_buf[i].bssid[3], ap_buf[i].bssid[4], ap_buf[i].bssid[5]);
                if (strcasecmp(found, target_bssid) == 0) {
                    s_cal.total_rssi += ap_buf[i].rssi;
                    s_cal.sample_count++;
                    if (ap_buf[i].rssi < s_cal.min_rssi) s_cal.min_rssi = ap_buf[i].rssi;
                    if (ap_buf[i].rssi > s_cal.max_rssi) s_cal.max_rssi = ap_buf[i].rssi;
                }
            }
        }

        /* Progress log every 10s */
        int64_t elapsed = (esp_timer_get_time() / 1000) - s_cal.start_ms;
        if (elapsed > 0 && (elapsed % 10000) < 600) {
            float avg = s_cal.sample_count > 0 ? (float)s_cal.total_rssi / s_cal.sample_count : -100;
            ESP_LOGI(TAG, "CAL progress: %llds/%ds samples=%d avg=%.1f min=%d max=%d",
                     (long long)(elapsed / 1000), duration,
                     s_cal.sample_count, avg, s_cal.min_rssi, s_cal.max_rssi);
        }
    }

    s_cal.active = false;
    float avg_rssi = s_cal.sample_count > 0 ? (float)s_cal.total_rssi / s_cal.sample_count : -100.0f;

    /* Resume normal operations */
    uart_rx_resume_scanner(0);
#if CONFIG_DUAL_SCANNER
    uart_rx_resume_scanner(1);
#endif
    http_upload_resume();

    char resp[196];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"bssid\":\"%s\",\"channel\":%d,\"avg_rssi\":%.1f,"
             "\"min_rssi\":%d,\"max_rssi\":%d,\"samples\":%d,\"duration_s\":%d}",
             target_bssid, channel, avg_rssi,
             s_cal.min_rssi, s_cal.max_rssi, s_cal.sample_count, duration);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);

    ESP_LOGW(TAG, "CALIBRATION: result → avg=%.1f min=%d max=%d (%d samples in %ds)",
             avg_rssi, s_cal.min_rssi, s_cal.max_rssi, s_cal.sample_count, duration);
    return ESP_OK;
}

void http_status_init(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port    = CONFIG_HTTP_STATUS_PORT;
    config.task_priority  = CONFIG_HTTP_STATUS_PRIORITY;
    config.stack_size     = 6144;   /* Minimal — relay uses static buffers, not stack */
    config.max_uri_handlers = 20;   /* 15 active (status/setup/scan/connect/ota*3/fw*3/cal*4) + headroom */
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

    /* Calibration endpoints */
    static const httpd_uri_t uri_cal_start = {
        .uri = "/api/calibrate/start", .method = HTTP_POST, .handler = calibrate_start_handler };
    static const httpd_uri_t uri_cal_measure = {
        .uri = "/api/calibrate/measure", .method = HTTP_POST, .handler = calibrate_measure_handler };
    static const httpd_uri_t uri_cal_power = {
        .uri = "/api/calibrate/power", .method = HTTP_POST, .handler = calibrate_power_handler };
    static const httpd_uri_t uri_cal_stop = {
        .uri = "/api/calibrate/stop", .method = HTTP_POST, .handler = calibrate_stop_handler };
    httpd_register_uri_handler(server, &uri_cal_start);
    httpd_register_uri_handler(server, &uri_cal_measure);
    httpd_register_uri_handler(server, &uri_cal_power);
    httpd_register_uri_handler(server, &uri_cal_stop);

    ESP_LOGI(TAG, "HTTP status server started on port %d (setup at /setup, calibration at /api/calibrate/*)",
             CONFIG_HTTP_STATUS_PORT);
}
