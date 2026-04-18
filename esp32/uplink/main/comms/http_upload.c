/**
 * Friend or Foe -- Uplink HTTP Upload Implementation
 *
 * Collects drone detections from a FreeRTOS queue, batches them into
 * JSON payloads, and POSTs to the FastAPI backend.  When WiFi is down,
 * batches are buffered in a ring buffer and drained on reconnect.
 */

#include "http_upload.h"
#include "uart_rx.h"
#include "wifi_sta.h"
#include "ring_buffer.h"
#include "nvs_config.h"
#include "config.h"
#include "gps.h"
#include "time_sync.h"
#include "version.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#if defined(UPLINK_ESP32S3)
#include "esp_http_client.h"  /* Only for S3 lockon polling */
#endif
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <netdb.h>
#include "lwip/inet.h"

static const char *TAG = "http_up";

static QueueHandle_t   s_detection_queue   = NULL;
static ring_buffer_t  *s_offline_buffer    = NULL;
static int             s_success_count     = 0;
static int             s_fail_count        = 0;
static int64_t         s_last_success_epoch_ms = 0;

/* Persistent HTTP client handle (avoids socket exhaustion from rapid open/close) */
/* esp_http_client removed — using raw sockets for zero heap allocation */

/* Pause flag — set during firmware relay to free heap */
static volatile bool s_upload_paused = false;

/* Maximum JSON payload size for a batch */
#define MAX_PAYLOAD_SIZE    4096

/* Retry config — keep total blocking time well under 30s WDT timeout */
#define HTTP_TIMEOUT_MS     5000    /* 5s per attempt — 3s caused EAGAIN on slow WiFi */
#define MAX_RETRIES         1       /* 1 retry max — fail fast, try next batch */
#define BACKOFF_BASE_MS     500     /* 500ms base (was 1000) */
#define MAX_DRAIN_PER_CYCLE 1       /* drain 1 offline batch per loop — don't block */
#define HEALTH_RESET_SEC    30      /* force reset client if no success for 30s */

/* ── Source integer to string mapping ──────────────────────────────────── */

static const char *source_to_string(uint8_t src)
{
    switch (src) {
        case DETECTION_SRC_BLE_RID:            return "ble_rid";
        case DETECTION_SRC_WIFI_SSID:          return "wifi_ssid";
        case DETECTION_SRC_WIFI_DJI_IE:        return "wifi_dji_ie";
        case DETECTION_SRC_WIFI_BEACON:        return "wifi_beacon_rid";
        case DETECTION_SRC_WIFI_OUI:           return "wifi_oui";
        case DETECTION_SRC_WIFI_PROBE_REQUEST: return "wifi_probe_request";
        default:                               return "unknown";
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
 * On S3 (N16R8) this is a 64 KB PSRAM-backed buffer, allowing much larger
 * batches per HTTP round-trip. On legacy ESP32 (no PSRAM) or if PSRAM init
 * failed, we fall back to the original 4 KB static buffer — preserving the
 * heap-stability guarantees from project_heap_stability_solution.md.
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

/* Helper: append scanner info object */
static int append_scanner_info(char *buf, int off, int max, const char *uart_name, const scanner_info_t *info)
{
    int n = snprintf(&buf[off], max - off,
        "{\"uart\":\"%s\",\"ver\":\"%s\",\"board\":\"%s\",\"chip\":\"%s\",\"caps\":\"%s\"",
        uart_name, info->version, info->board, info->chip, info->caps);
    if (n > 0) off += n;
    if (info->auth_count > 0) { n = snprintf(&buf[off], max-off, ",\"auth_fr\":%d", info->auth_count); if(n>0) off+=n; }
    if (info->fc_hist[0]) { n = snprintf(&buf[off], max-off, ",\"fc_hist\":\"%s\"", info->fc_hist); if(n>0) off+=n; }
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
#if defined(UPLINK_ESP32S3)
    BUF_APPEND(",\"board_type\":\"uplink-s3\"");
#elif defined(UPLINK_ESP32)
    BUF_APPEND(",\"board_type\":\"uplink-esp32\"");
#else
    BUF_APPEND(",\"board_type\":\"uplink-c3\"");
#endif
    if (wifi_ssid[0]) BUF_APPEND(",\"wifi_ssid\":\"%s\",\"wifi_rssi\":%d", wifi_ssid, wifi_sta_get_rssi());

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

        BUF_APPEND("{\"drone_id\":\"%s\",\"source\":\"%s\",\"confidence\":%.8f,\"rssi\":%d",
                   d->drone_id, source_to_string(d->source), d->confidence, d->rssi);
        if (d->latitude != 0.0 || d->longitude != 0.0)
            BUF_APPEND(",\"latitude\":%.7f,\"longitude\":%.7f", d->latitude, d->longitude);
        if (d->altitude_m != 0.0) BUF_APPEND(",\"altitude_m\":%.1f", d->altitude_m);
        if (d->manufacturer[0]) BUF_APPEND(",\"manufacturer\":\"%s\"", d->manufacturer);
        if (d->model[0]) BUF_APPEND(",\"model\":\"%s\"", d->model);
        if (d->ssid[0]) BUF_APPEND(",\"ssid\":\"%s\"", d->ssid);
        if (d->bssid[0]) BUF_APPEND(",\"bssid\":\"%s\"", d->bssid);
        if (d->freq_mhz != 0) BUF_APPEND(",\"channel\":%d", d->freq_mhz);
        if (d->operator_lat != 0.0 || d->operator_lon != 0.0)
            BUF_APPEND(",\"operator_lat\":%.7f,\"operator_lon\":%.7f", d->operator_lat, d->operator_lon);
        if (d->operator_id[0]) BUF_APPEND(",\"operator_id\":\"%s\"", d->operator_id);

        /* Probe request SSIDs */
        if (d->source == DETECTION_SRC_WIFI_PROBE_REQUEST && d->ssid[0])
            BUF_APPEND(",\"probed_ssids\":[\"%s\"]", d->ssid);

        /* BLE fingerprinting fields (only non-zero) */
        if (d->ble_company_id) BUF_APPEND(",\"ble_company_id\":%u", d->ble_company_id);
        if (d->ble_apple_type) BUF_APPEND(",\"ble_apple_type\":%u", d->ble_apple_type);
        if (d->ble_ad_type_count) BUF_APPEND(",\"ble_ad_type_count\":%u", d->ble_ad_type_count);
        if (d->ble_payload_len) BUF_APPEND(",\"ble_payload_len\":%u", d->ble_payload_len);
        if (d->ble_addr_type) BUF_APPEND(",\"ble_addr_type\":%u", d->ble_addr_type);
        if (d->ble_ja3_hash) BUF_APPEND(",\"ble_ja3\":\"%08lx\"", (unsigned long)d->ble_ja3_hash);
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
        if (d->ble_svc_uuid_count > 0) {
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
 * Uses a persistent TCP socket with static buffers instead of esp_http_client.
 * This eliminates all malloc/free during uploads, preventing heap fragmentation
 * that causes legacy ESP32 nodes to crash every 30 seconds.
 */

static int s_sock = -1;
static char s_http_header[256];  /* Static buffer for HTTP request header */
static char s_http_resp[256];    /* Static buffer for HTTP response */
static struct sockaddr_in s_cached_addr = {0};  /* Cached resolved address — avoid getaddrinfo heap alloc */
static bool s_addr_cached = false;

static bool raw_socket_connect(void)
{
    if (s_sock >= 0) return true;  /* Already connected */

    char backend_url[128] = {0};
    if (s_using_fallback_url) {
        strncpy(backend_url, CONFIG_BACKEND_URL_FALLBACK, sizeof(backend_url) - 1);
    } else {
        nvs_config_get_backend_url(backend_url, sizeof(backend_url));
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
        close(s_sock);
        s_sock = -1;
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
            raw_socket_close();
            consecutive_fails = 0;
        }
        was_connected = connected_now;

        /* ── Health watchdog: force reset if stalled ─────────────────── */
        if (connected_now) {
            TickType_t since_success = xTaskGetTickCount() - last_success_tick;
            if (since_success >= pdMS_TO_TICKS(HEALTH_RESET_SEC * 1000)) {
                ESP_LOGW(TAG, "HEALTH RESET: no success for %ds, resetting client + clearing offline buffer",
                         HEALTH_RESET_SEC);
                raw_socket_close();
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

        if (xQueueReceive(s_detection_queue, &det, wait_ticks) == pdTRUE) {
            if (batch_count == 0) {
                /* Start the batch age clock with the first detection. */
                first_item_tick = xTaskGetTickCount();
                estimated_payload_bytes = 64; /* batch envelope */
                scan_ts_ms = det.last_updated_ms > 0 ? det.last_updated_ms : time_sync_get_epoch_ms();
            }
            last_item_tick = xTaskGetTickCount();
            batch[batch_count++] = det;
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
                raw_socket_close();  /* Free HTTP client resources */
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
                            raw_socket_close();
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

#if defined(UPLINK_ESP32S3)
                /* Lock-on polling — only on S3 (ESP32 has too little heap for extra HTTP client) */
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
#endif  /* UPLINK_ESP32S3 — lockon polling */
            }
        }
    }
}

/* ── Public API ────────────────────────────────────────────────────────── */

void http_upload_init(QueueHandle_t detection_queue)
{
    s_detection_queue = detection_queue;

    /* Try to relocate the payload buffer into PSRAM (64 KB) on S3 boards
     * with external memory. Silently stays on the 4 KB static buffer when
     * PSRAM isn't available, preserving the legacy ESP32 path unchanged. */
    char *psram_buf = (char *)psram_alloc_strict(PAYLOAD_BUF_PSRAM_SIZE);
    if (psram_buf) {
        s_payload_buf      = psram_buf;
        s_payload_buf_size = PAYLOAD_BUF_PSRAM_SIZE;
        ESP_LOGW(TAG, "Payload buffer: %d KB in PSRAM", PAYLOAD_BUF_PSRAM_SIZE / 1024);
    } else {
        ESP_LOGI(TAG, "Payload buffer: %d KB internal (no PSRAM)",
                 PAYLOAD_BUF_FALLBACK_SIZE / 1024);
    }

    /* Offline queue: PSRAM on S3 (2 MB / 512 batches), internal heap on legacy.
     * Storage-in-PSRAM + header-in-SRAM is handled by ring_buffer_create_psram;
     * it silently falls back to calloc if PSRAM is absent, so the same call
     * works on every build target. */
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
