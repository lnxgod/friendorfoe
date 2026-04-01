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
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
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

    char buf[512];

    /* Open JSON object */
    snprintf(buf, sizeof(buf),
        "{\"device_id\":\"%s\",\"uptime_s\":%lld,"
        "\"gps\":{\"fix\":%s,\"lat\":%.6f,\"lon\":%.6f,\"satellites\":%d},"
        "\"wifi_sta\":%s,\"ap_clients\":%d,"
        "\"standalone\":%s,\"scanner_connected\":%s,"
        "\"battery\":{\"percent\":%.1f,\"voltage\":%.2f},"
        "\"detections\":%d,\"uploads_ok\":%d,\"uploads_fail\":%d,",
        device_id, (long long)uptime_sec,
        gps_fix ? "true" : "false", gps.latitude, gps.longitude, gps.satellites,
        wifi_ok ? "true" : "false", ap_clients,
        standalone ? "true" : "false",
        scanner_ok ? "true" : "false",
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

    /* Device ID */
    snprintf(buf, sizeof(buf),
        "<div class=\"card\">"
        "<h2 style=\"margin-top:0\">Device</h2>"
        "<label>Device ID</label>"
        "<input type=\"text\" id=\"devid\" value=\"%s\">"
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
    char buf[128];
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

        snprintf(buf, sizeof(buf),
            "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
            sent > 0 ? "," : "",
            (char *)ap_list[i].ssid,
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
    if (devid[0]) nvs_config_set_string("device_id", devid);

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
    ESP_LOGW(TAG, "OTA update started, content_len=%d", req->content_len);

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    /* Receive firmware in chunks */
    char buf[1024];
    int received = 0;
    int total = req->content_len;
    int remaining = total;

    while (remaining > 0) {
        int to_read = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int read_len = httpd_req_recv(req, buf, to_read);
        if (read_len <= 0) {
            if (read_len == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "OTA recv error at %d/%d", received, total);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_handle, buf, read_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed at %d: %s", received, esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write error");
            return ESP_FAIL;
        }

        received += read_len;
        remaining -= read_len;

        /* Progress log every 100KB */
        if (received % (100 * 1024) < 1024) {
            ESP_LOGI(TAG, "OTA progress: %d/%d bytes (%.0f%%)",
                     received, total, (float)received / total * 100);
        }
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA validation failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "OTA update successful! %d bytes written to %s. Rebooting...",
             received, update_partition->label);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"OTA complete, rebooting...\"}");

    /* Reboot after short delay */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
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

/* ── OTA Relay: forward firmware to scanner via UART ─────────────────── */

static esp_err_t ota_relay_handler(httpd_req_t *req)
{
    /* Parse ?uart=ble or ?uart=wifi query param */
    char query[32] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char uart_target[8] = "ble";
    httpd_query_key_value(query, "uart", uart_target, sizeof(uart_target));

    /* Select UART port */
    uart_port_t uart_num;
#ifdef UPLINK_ESP32
    if (strcmp(uart_target, "wifi") == 0) {
        uart_num = CONFIG_WIFI_SCANNER_UART;
    } else {
        uart_num = CONFIG_BLE_SCANNER_UART;
    }
#else
    uart_num = CONFIG_BLE_SCANNER_UART;
#endif

    int total = req->content_len;
    ESP_LOGW(TAG, "OTA relay: %d bytes to scanner (uart=%s port=%d)",
             total, uart_target, uart_num);

    if (total < 1024 || total > 2 * 1024 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid firmware size");
        return ESP_FAIL;
    }

    /* Step 1: Send OTA begin command to scanner as JSON */
    char cmd[80];
    snprintf(cmd, sizeof(cmd), "{\"type\":\"ota_begin\",\"size\":%d}\n", total);
    uart_write_bytes(uart_num, cmd, strlen(cmd));

    /* Step 2: Wait for ACK (up to 5 seconds) */
    vTaskDelay(pdMS_TO_TICKS(2000));  /* Give scanner time to prepare partition */
    /* TODO: read ACK from scanner UART — for now, proceed optimistically */

    /* Step 3: Stream HTTP data as binary chunks to scanner */
    uint8_t http_buf[OTA_CHUNK_MAX_DATA];
    int received = 0;
    int remaining = total;
    uint16_t seq = 0;

    while (remaining > 0) {
        int to_read = remaining > OTA_CHUNK_MAX_DATA ? OTA_CHUNK_MAX_DATA : remaining;
        int read_len = httpd_req_recv(req, (char *)http_buf, to_read);
        if (read_len <= 0) {
            if (read_len == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "OTA relay: HTTP recv error at %d/%d", received, total);
            /* Abort scanner OTA */
            uart_write_bytes(uart_num, "{\"type\":\"ota_abort\"}\n", 20);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }

        /* Send binary chunk: [0xF0][seq_hi][seq_lo][len_hi][len_lo] + data */
        uint8_t hdr[5] = {
            OTA_CHUNK_MAGIC,
            (uint8_t)(seq >> 8), (uint8_t)(seq & 0xFF),
            (uint8_t)(read_len >> 8), (uint8_t)(read_len & 0xFF),
        };
        uart_write_bytes(uart_num, (char *)hdr, 5);
        uart_write_bytes(uart_num, (char *)http_buf, read_len);

        received += read_len;
        remaining -= read_len;
        seq++;

        /* Wait for flow control ACK every 16 chunks */
        if (seq % OTA_ACK_INTERVAL_CHUNKS == 0) {
            /* Brief pause for scanner to process and ACK */
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        /* Progress log */
        if (received % (100 * 1024) < OTA_CHUNK_MAX_DATA) {
            ESP_LOGI(TAG, "OTA relay: %d/%d bytes (%.0f%%)",
                     received, total, (float)received / total * 100);
        }
    }

    /* Step 4: Send OTA end */
    vTaskDelay(pdMS_TO_TICKS(200));
    uart_write_bytes(uart_num, "{\"type\":\"ota_end\"}\n", 18);

    ESP_LOGW(TAG, "OTA relay complete: %d bytes sent to scanner. Scanner rebooting...",
             received);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Firmware relayed to scanner\"}");
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

void http_status_init(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port    = CONFIG_HTTP_STATUS_PORT;
    config.task_priority  = CONFIG_HTTP_STATUS_PRIORITY;
    config.stack_size     = 8192;
    config.max_uri_handlers = 8;
    config.max_open_sockets = 3;  /* Reduce from default 7 to save RAM */

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

    ESP_LOGI(TAG, "HTTP status server started on port %d (setup at /setup)",
             CONFIG_HTTP_STATUS_PORT);
}
