/**
 * Friend or Foe — Scanner Firmware Store + Reliable UART Relay
 *
 * Store-then-forward OTA: upload scanner firmware to flash via HTTP,
 * then relay to scanner UART with CRC32 + ACK/NACK retransmission.
 *
 * Uses esp_ota_begin/write/abort for flash storage — the OTA API
 * handles erase scheduling internally, preventing TCP drops that
 * killed the raw partition approach on heap-constrained ESP32.
 */

#include "fw_store.h"
#include "config.h"
#include "nvs_config.h"
#include "uart_protocol.h"
#include "uart_rx.h"
#include "http_upload.h"
#include "version.h"

#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "psram_alloc.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>

static const char *TAG = "fw_store";

/* ── State ───────────────────────────────────────────────────────────────── */

static volatile bool s_operation_active = false;  /* Prevents concurrent upload/relay */

typedef struct {
    bool ok;
    uint32_t size;
    uint32_t bytes;
    char stage[24];
    char error[64];
    int chunks;
    int nacks;
    int retries;
    int64_t elapsed_s;
    int64_t finished_ms;
    uint32_t cmd_rx_before;
    uint32_t cmd_rx_after;
    uint32_t fw_check_before;
    uint32_t fw_check_after;
    int64_t cmd_age_after_s;
    char scanner_version[32];
    char scanner_fw_state[16];
} fw_last_relay_state_t;

static fw_last_relay_state_t s_last_relay[2] = {0};

/* ── OTA upload staging buffer (P4) ──────────────────────────────────────
 * Lazily allocated on first upload. 64 KB on PSRAM collapses the
 * recv/esp_ota_write loop from ~280 iterations per 1.1 MB scanner firmware
 * down to ~18, visibly cutting upload latency. Falls back to a 4 KB static
 * on boards without PSRAM — matches the heap-stability baseline. */
#define FW_STAGE_BUF_PSRAM   (64 * 1024)
#define FW_STAGE_BUF_FALLBACK 4096

/* Busy scanners can miss a single control line while their TX queue is full.
 * Keep re-sending the quiet request before relay, but still require proof. */
#define FW_RELAY_STOP_STORM_MS       8000
#define FW_RELAY_STOP_STORM_STEP_MS  250
#ifdef FOF_BADGE_VARIANT
#define FW_RELAY_NACK_POLL_MS        OTA_RELAY_BADGE_NACK_DRAIN_MS
#define FW_RELAY_CHUNK_DATA          OTA_CHUNK_BADGE_MAX_DATA
#else
#define FW_RELAY_NACK_POLL_MS        120
#define FW_RELAY_CHUNK_DATA          256
#endif

static uint8_t  s_fw_stage_fallback[FW_STAGE_BUF_FALLBACK];
static uint8_t *s_fw_stage_buf = NULL;     /* resolved on first use */
static size_t   s_fw_stage_cap = 0;

typedef struct {
    bool active;
    esp_ota_handle_t handle;
    const esp_partition_t *partition;
    uint32_t size;
    uint32_t received;
    uint32_t crc32;
    uint32_t expected_crc32;
    char name[32];
    char version[32];
} serial_upload_state_t;

static serial_upload_state_t s_serial_upload;

static void fw_stage_buf_ensure(void)
{
    if (s_fw_stage_buf) return;
    uint8_t *p = (uint8_t *)psram_alloc_strict(FW_STAGE_BUF_PSRAM);
    if (p) {
        s_fw_stage_buf = p;
        s_fw_stage_cap = FW_STAGE_BUF_PSRAM;
        ESP_LOGW(TAG, "OTA upload stage buffer: %u KB in PSRAM",
                 (unsigned)(FW_STAGE_BUF_PSRAM / 1024));
    } else {
        s_fw_stage_buf = s_fw_stage_fallback;
        s_fw_stage_cap = FW_STAGE_BUF_FALLBACK;
        ESP_LOGI(TAG, "OTA upload stage buffer: %u KB internal (no PSRAM)",
                 (unsigned)(FW_STAGE_BUF_FALLBACK / 1024));
    }
}

bool fw_store_is_relay_active(void) { return s_operation_active; }

/* ── NVS metadata keys ───────────────────────────────────────────────────── */

#define NVS_FW_SIZE     "fw_size"
#define NVS_FW_CKSUM    "fw_cksum"
#define NVS_FW_CRC32    "fw_crc32"
#define NVS_FW_VER      "fw_ver"
#define NVS_FW_NAME     "fw_name"
#define NVS_FW_PART     "fw_part"

/** Read stored firmware metadata from NVS. Returns false if nothing stored. */
static bool read_fw_metadata(uint32_t *size, uint32_t *checksum,
                             char *version, size_t ver_len,
                             char *part_label, size_t label_len)
{
    if (!nvs_config_get_u32(NVS_FW_SIZE, size) || *size == 0) return false;
    if (checksum) nvs_config_get_u32(NVS_FW_CKSUM, checksum);
    if (version)  nvs_config_get_string(NVS_FW_VER, version, ver_len);
    if (part_label) nvs_config_get_string(NVS_FW_PART, part_label, label_len);
    return true;
}

/* Forward decl — defined below near the http handlers. */
static const esp_partition_t *get_store_partition(void);
static void resume_all_tasks(void);

const esp_partition_t *fw_store_get_target_partition(void)
{
    return get_store_partition();
}

void fw_store_persist_metadata(const char *name, const char *version,
                               const esp_partition_t *partition,
                               uint32_t size, uint32_t crc32)
{
    nvs_config_set_u32(NVS_FW_SIZE, size);
    nvs_config_set_u32(NVS_FW_CKSUM, crc32);
    nvs_config_set_u32(NVS_FW_CRC32, crc32);
    nvs_config_set_string(NVS_FW_VER,  version && version[0] ? version : "?");
    nvs_config_set_string(NVS_FW_NAME, name && name[0] ? name : "");
    if (partition && partition->label[0]) {
        nvs_config_set_string(NVS_FW_PART, partition->label);
    }
}

bool fw_store_serial_upload_active(void)
{
    return s_serial_upload.active;
}

uint32_t fw_store_serial_upload_remaining(void)
{
    if (!s_serial_upload.active || s_serial_upload.received >= s_serial_upload.size) {
        return 0;
    }
    return s_serial_upload.size - s_serial_upload.received;
}

void fw_store_serial_upload_abort(const char *reason)
{
    if (!s_serial_upload.active) {
        return;
    }
    ESP_LOGW(TAG, "USB firmware staging aborted at %lu/%lu: %s",
             (unsigned long)s_serial_upload.received,
             (unsigned long)s_serial_upload.size,
             reason ? reason : "?");
    if (s_serial_upload.handle) {
        esp_ota_abort(s_serial_upload.handle);
    }
    memset(&s_serial_upload, 0, sizeof(s_serial_upload));
    resume_all_tasks();
    s_operation_active = false;
}

bool fw_store_serial_upload_begin(const char *name,
                                  const char *version,
                                  uint32_t size,
                                  uint32_t expected_crc32,
                                  char *out_json,
                                  size_t out_json_len)
{
    if (s_operation_active || s_serial_upload.active) {
        if (out_json && out_json_len) {
            snprintf(out_json, out_json_len,
                     "{\"ok\":false,\"error\":\"operation_active\"}");
        }
        return false;
    }

    const esp_partition_t *p = get_store_partition();
    if (!p || size < 1024 || size > p->size || expected_crc32 == 0) {
        if (out_json && out_json_len) {
            snprintf(out_json, out_json_len,
                     "{\"ok\":false,\"error\":\"invalid_image\",\"max\":%lu}",
                     p ? (unsigned long)p->size : 0UL);
        }
        return false;
    }

    memset(&s_serial_upload, 0, sizeof(s_serial_upload));
    s_operation_active = true;
    http_upload_pause();
    uart_rx_pause_scanner(0);
#if CONFIG_DUAL_SCANNER
    uart_rx_pause_scanner(1);
#endif
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_err_t err = esp_ota_begin(p, OTA_WITH_SEQUENTIAL_WRITES,
                                  &s_serial_upload.handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB staging esp_ota_begin failed: %s on '%s'",
                 esp_err_to_name(err), p->label);
        resume_all_tasks();
        s_operation_active = false;
        if (out_json && out_json_len) {
            snprintf(out_json, out_json_len,
                     "{\"ok\":false,\"error\":\"esp_ota_begin:%s\"}",
                     esp_err_to_name(err));
        }
        return false;
    }

    s_serial_upload.active = true;
    s_serial_upload.partition = p;
    s_serial_upload.size = size;
    s_serial_upload.expected_crc32 = expected_crc32;
    strncpy(s_serial_upload.name,
            (name && name[0]) ? name : "scanner-s3-combo-fof_badge",
            sizeof(s_serial_upload.name) - 1);
    strncpy(s_serial_upload.version,
            (version && version[0]) ? version : FOF_VERSION,
            sizeof(s_serial_upload.version) - 1);

    ESP_LOGW(TAG, "USB staging scanner firmware: %lu bytes to '%s' crc=%08lX",
             (unsigned long)size, p->label, (unsigned long)expected_crc32);
    if (out_json && out_json_len) {
        snprintf(out_json, out_json_len,
                 "{\"ok\":true,\"partition\":\"%s\",\"size\":%lu,"
                 "\"crc32\":%lu,\"name\":\"%s\",\"version\":\"%s\"}",
                 p->label, (unsigned long)size,
                 (unsigned long)expected_crc32,
                 s_serial_upload.name, s_serial_upload.version);
    }
    return true;
}

bool fw_store_serial_upload_write(const uint8_t *data,
                                  size_t len,
                                  char *out_json,
                                  size_t out_json_len)
{
    if (!s_serial_upload.active || !data || len == 0) {
        return true;
    }
    uint32_t remaining = s_serial_upload.size - s_serial_upload.received;
    if (len > remaining) {
        fw_store_serial_upload_abort("too_many_bytes");
        if (out_json && out_json_len) {
            snprintf(out_json, out_json_len,
                     "{\"ok\":false,\"error\":\"too_many_bytes\"}");
        }
        return false;
    }
    esp_err_t err = esp_ota_write(s_serial_upload.handle, data, len);
    if (err != ESP_OK) {
        fw_store_serial_upload_abort("write_failed");
        if (out_json && out_json_len) {
            snprintf(out_json, out_json_len,
                     "{\"ok\":false,\"error\":\"write:%s\"}",
                     esp_err_to_name(err));
        }
        return false;
    }
    s_serial_upload.crc32 = esp_rom_crc32_le(s_serial_upload.crc32, data, len);
    s_serial_upload.received += (uint32_t)len;
    return true;
}

bool fw_store_serial_upload_end(char *out_json, size_t out_json_len)
{
    if (!s_serial_upload.active) {
        if (out_json && out_json_len) {
            snprintf(out_json, out_json_len,
                     "{\"ok\":false,\"error\":\"not_active\"}");
        }
        return false;
    }
    if (s_serial_upload.received != s_serial_upload.size) {
        if (out_json && out_json_len) {
            snprintf(out_json, out_json_len,
                     "{\"ok\":false,\"error\":\"incomplete\",\"received\":%lu,"
                     "\"size\":%lu}",
                     (unsigned long)s_serial_upload.received,
                     (unsigned long)s_serial_upload.size);
        }
        fw_store_serial_upload_abort("incomplete");
        return false;
    }
    if (s_serial_upload.crc32 != s_serial_upload.expected_crc32) {
        uint32_t got = s_serial_upload.crc32;
        uint32_t expected = s_serial_upload.expected_crc32;
        if (out_json && out_json_len) {
            snprintf(out_json, out_json_len,
                     "{\"ok\":false,\"error\":\"crc_mismatch\","
                     "\"expected\":%lu,\"got\":%lu}",
                     (unsigned long)expected, (unsigned long)got);
        }
        fw_store_serial_upload_abort("crc_mismatch");
        return false;
    }

    esp_ota_abort(s_serial_upload.handle);
    fw_store_persist_metadata(s_serial_upload.name,
                              s_serial_upload.version,
                              s_serial_upload.partition,
                              s_serial_upload.size,
                              s_serial_upload.crc32);
    if (out_json && out_json_len) {
        snprintf(out_json, out_json_len,
                 "{\"ok\":true,\"partition\":\"%s\",\"size\":%lu,"
                 "\"crc32\":%lu,\"name\":\"%s\",\"version\":\"%s\"}",
                 s_serial_upload.partition ? s_serial_upload.partition->label : "?",
                 (unsigned long)s_serial_upload.size,
                 (unsigned long)s_serial_upload.crc32,
                 s_serial_upload.name,
                 s_serial_upload.version);
    }
    ESP_LOGW(TAG, "USB firmware staged: %lu bytes CRC=%08lX name=%s version=%s",
             (unsigned long)s_serial_upload.size,
             (unsigned long)s_serial_upload.crc32,
             s_serial_upload.name,
             s_serial_upload.version);
    memset(&s_serial_upload, 0, sizeof(s_serial_upload));
    resume_all_tasks();
    s_operation_active = false;
    return true;
}

bool fw_store_get_info(fw_store_info_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->stored = read_fw_metadata(&out->size, &out->checksum,
                                   out->version, sizeof(out->version),
                                   out->partition, sizeof(out->partition));
    if (!out->stored) return false;
    nvs_config_get_string(NVS_FW_NAME, out->name, sizeof(out->name));
    return true;
}

/** Clear stored firmware metadata from NVS. */
static void clear_fw_metadata(void)
{
    nvs_config_set_u32(NVS_FW_SIZE, 0);
}

/** Find the partition where firmware is stored, by label from NVS. */
static const esp_partition_t *find_fw_partition(void)
{
    char label[16] = {0};
    uint32_t size = 0;
    if (!read_fw_metadata(&size, NULL, NULL, 0, label, sizeof(label))) return NULL;
    if (!label[0]) return NULL;

    /* Find by label — works for both OTA and data partitions */
    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, label);
    const esp_partition_t *p = it ? esp_partition_get(it) : NULL;
    if (it) esp_partition_iterator_release(it);
    return p;
}

/** Get the best partition for storing scanner firmware. Prefers inactive OTA. */
static const esp_partition_t *get_store_partition(void)
{
#if defined(FOF_BADGE_VARIANT)
    const esp_partition_t *dedicated = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, "fw_scanner_s3");
    if (dedicated) {
        ESP_LOGI(TAG, "Store target: fw_scanner_s3 '%s' (%luKB)",
                 dedicated->label, (unsigned long)(dedicated->size / 1024));
        return dedicated;
    }
#endif

    /* Prefer inactive OTA partition — esp_ota_begin works natively with it */
    const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
    if (p) {
        ESP_LOGI(TAG, "Store target: inactive OTA '%s' (%luKB)",
                 p->label, (unsigned long)(p->size / 1024));
        return p;
    }

    /* Fall back to dedicated fw_store partition */
    p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, NULL);
    if (p) {
        ESP_LOGI(TAG, "Store target: fw_store '%s' (%luKB)",
                 p->label, (unsigned long)(p->size / 1024));
    } else {
        ESP_LOGE(TAG, "No storage partition available");
    }
    return p;
}

/** Resume all paused tasks after upload/relay completes or fails. */
static void resume_all_tasks(void)
{
    uart_rx_resume_scanner(0);
#if CONFIG_DUAL_SCANNER
    uart_rx_resume_scanner(1);
#endif
    http_upload_resume();
}

/* ── POST /api/fw/upload — store firmware using OTA API ──────────────────── */

static esp_err_t fw_upload_handler(httpd_req_t *req)
{
    if (s_operation_active) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Operation in progress");
        return ESP_FAIL;
    }

    const esp_partition_t *p = get_store_partition();
    if (!p) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No store partition");
        return ESP_FAIL;
    }

    int total = req->content_len;
    if (total < 1024 || total > (int)p->size) {
        char msg[80];
        snprintf(msg, sizeof(msg), "Invalid size: %d (max %lu)",
                 total, (unsigned long)p->size);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return ESP_FAIL;
    }

    s_operation_active = true;

    /* Pause HTTP upload + UART RX tasks to free heap and CPU.
     * The scanners flood with huge BLE JSON that consumes resources
     * needed for the 1MB firmware upload. */
    http_upload_pause();
    uart_rx_pause_scanner(0);  /* BLE scanner */
#if CONFIG_DUAL_SCANNER
    uart_rx_pause_scanner(1);  /* WiFi scanner */
#endif
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGW(TAG, "Storing scanner firmware: %d bytes to '%s' heap=%lu (uploads+UART paused)",
             total, p->label, (unsigned long)esp_get_free_heap_size());

    /* Increase socket timeout for large uploads */
    {
        int fd = httpd_req_to_sockfd(req);
        struct timeval tv = { .tv_sec = 120, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    /* Use OTA API for flash writes — handles erase scheduling internally.
     * This is the key fix: raw esp_partition_erase_range blocked for 50-200ms
     * per 64KB block, causing TCP drops at ~47%. The OTA API defers erases
     * to small 4KB sectors and never blocks long enough to stall TCP. */
    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(p, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s on partition '%s'",
                 esp_err_to_name(err), p ? p->label : "?");
        char msg[96];
        snprintf(msg, sizeof(msg), "esp_ota_begin: %s on '%s'",
                 esp_err_to_name(err), p ? p->label : "?");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
        resume_all_tasks();
        s_operation_active = false;
        return ESP_FAIL;
    }

    fw_stage_buf_ensure();
    uint8_t *buf = s_fw_stage_buf;
    const size_t buf_cap = s_fw_stage_cap;
    int received = 0;
    uint32_t checksum = 0;
    int consecutive_timeouts = 0;

    while (received < total) {
        int to_read = total - received;
        if (to_read > (int)buf_cap) to_read = (int)buf_cap;

        int len = httpd_req_recv(req, (char *)buf, to_read);
        if (len <= 0) {
            if (len == HTTPD_SOCK_ERR_TIMEOUT) {
                consecutive_timeouts++;
                if (consecutive_timeouts > 3) {
                    ESP_LOGE(TAG, "Upload timeout at %d/%d", received, total);
                    esp_ota_abort(ota_handle);
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Timeout");
                    resume_all_tasks();
                    s_operation_active = false;
                    return ESP_FAIL;
                }
                continue;
            }
            ESP_LOGE(TAG, "HTTP recv error at %d/%d", received, total);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            resume_all_tasks();
            s_operation_active = false;
            return ESP_FAIL;
        }

        consecutive_timeouts = 0;

        err = esp_ota_write(ota_handle, buf, len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed at %d: %s", received, esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            resume_all_tasks();
            s_operation_active = false;
            return ESP_FAIL;
        }

        checksum = esp_rom_crc32_le(checksum, buf, len);
        received += len;

        if (received % (100 * 1024) < (int)buf_cap) {
            ESP_LOGI(TAG, "Upload: %d/%d (%.0f%%) heap=%lu",
                     received, total, (float)received / total * 100,
                     (unsigned long)esp_get_free_heap_size());
        }
    }

    /* Abort OTA handle — data persists on flash, but partition is NOT
     * marked as bootable. This is safe: we never call esp_ota_end()
     * or esp_ota_set_boot_partition(). */
    esp_ota_abort(ota_handle);

    /* Store metadata in NVS (survives power cycle, partition swaps) */
    char version[32] = {0};
    char fw_name[32] = {0};
    char query[96] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "version", version, sizeof(version));
    httpd_query_key_value(query, "name", fw_name, sizeof(fw_name));

    nvs_config_set_u32(NVS_FW_SIZE, (uint32_t)total);
    nvs_config_set_u32(NVS_FW_CKSUM, checksum);
    nvs_config_set_u32(NVS_FW_CRC32, checksum);  /* Now CRC32, same key stores it */
    nvs_config_set_string(NVS_FW_VER, version[0] ? version : "?");
    nvs_config_set_string(NVS_FW_NAME, fw_name[0] ? fw_name : "");
    nvs_config_set_string(NVS_FW_PART, p->label);

    ESP_LOGW(TAG, "Firmware stored: %d bytes, CRC32=%08lX, partition=%s, name=%s version=%s",
             total, (unsigned long)checksum, p->label,
             fw_name[0] ? fw_name : "?",
             version[0] ? version : "?");

    resume_all_tasks();
    s_operation_active = false;

    char resp[192];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"size\":%d,\"checksum\":%lu,\"partition\":\"%s\","
             "\"name\":\"%s\",\"version\":\"%s\"}",
             total, (unsigned long)checksum, p->label,
             fw_name[0] ? fw_name : "",
             version[0] ? version : "");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* ── Line-based UART read helpers (v0.59 staged handshakes) ──────────────── */

/* Read bytes from UART until '\n', a printable-ASCII line, or timeout.
 * Returns >=0 = line length, -1 = timeout. Skips non-printable bytes so
 * leftover binary OTA-frame fragments don't corrupt the line buffer. */
/* Read bytes from UART until '\n' or timeout.
 *
 * Scanner detection JSON lines can exceed our line buffer (full enrichment
 * blobs run 200–600 chars: local name + raw mfr hex + auth tag + JA3 + etc.).
 * When the buffer fills mid-line, we MUST keep reading to consume the rest
 * of the line through the terminating '\n' — otherwise we'd just exit, then
 * the next call starts mid-stream + sees the trailing bytes as a new "line"
 * and we'd never resync to real JSON line boundaries.
 *
 * Behavior:
 *   - Collect printable ASCII into line[] up to size-1 chars.
 *   - If a line overflows, go into "drain" mode until the next '\n', then
 *     continue waiting for a NEW line from the start.
 *   - Return the first complete line that fits. Return -1 on deadline hit.
 */
static int relay_read_line(uart_port_t uart_num, char *line, size_t size, int timeout_ms)
{
    int pos = 0;
    bool draining = false;
    int lines_overflowed = 0;
    int64_t start_ms = esp_timer_get_time() / 1000;
    int64_t deadline_ms = start_ms + timeout_ms;
    while ((esp_timer_get_time() / 1000) < deadline_ms) {
        uint8_t b;
        int n = uart_read_bytes(uart_num, &b, 1, pdMS_TO_TICKS(10));
        if (n <= 0) continue;
        if (b == '\n') {
            if (draining) {
                /* Reached end of the too-long line; start fresh on the next. */
                draining = false;
                pos = 0;
                continue;
            }
            line[pos] = '\0';
            if (pos == 0) continue;   /* empty line — keep reading */
            return pos;
        }
        if (draining) continue;
        if (b >= 0x20 && b <= 0x7E) {
            if (pos < (int)size - 1) {
                line[pos++] = (char)b;
            } else {
                /* Buffer full before '\n' — discard this line, keep waiting. */
                draining = true;
                lines_overflowed++;
            }
        } else {
            pos = 0;  /* binary garbage — reset */
        }
    }
    if (lines_overflowed > 0) {
        ESP_LOGW(TAG, "relay_read_line: timeout with %d oversized lines dropped",
                 lines_overflowed);
    }
    line[pos] = '\0';
    return -1;
}

static bool relay_line_session_matches_or_legacy(const char *line,
                                                 const char *session_id)
{
    if (!session_id || !session_id[0]) {
        return true;
    }
    if (!line || !strstr(line, "\"session_id\"")) {
        return true;
    }
    char needle[48];
    snprintf(needle, sizeof(needle), "\"session_id\":\"%s\"", session_id);
    return line && strstr(line, needle) != NULL;
}

/* Wait for a JSON line whose payload contains `needle` (usually the "type"
 * value). Returns 0 on success, -1 on timeout, -2 if "ota_error" seen (fills
 * reason_out with the "reason":"X" string if present). Other line types are
 * silently consumed and ignored. */
static int relay_wait_for(uart_port_t uart_num, const char *needle,
                         int timeout_ms, char *reason_out, size_t reason_size)
{
    char line[512];
    int64_t start_ms = esp_timer_get_time() / 1000;
    int64_t deadline_ms = start_ms + timeout_ms;
    int lines_seen = 0;
    while (true) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        int remaining_ms = (int)(deadline_ms - now_ms);
        if (remaining_ms <= 0) {
            ESP_LOGW(TAG, "relay_wait_for(%s) TIMEOUT after %lldms — %d lines seen",
                     needle, (long long)(now_ms - start_ms), lines_seen);
            return -1;
        }
        int n = relay_read_line(uart_num, line, sizeof(line), remaining_ms);
        if (n < 0) {
            int64_t after_ms = esp_timer_get_time() / 1000;
            ESP_LOGW(TAG, "relay_wait_for(%s) read_line returned -1 after %lldms (remaining was %d) — %d lines",
                     needle, (long long)(after_ms - start_ms), remaining_ms, lines_seen);
            return -1;
        }
        lines_seen++;
        ESP_LOGI(TAG, "relay_wait_for(%s) line[%d]: %.80s", needle, lines_seen, line);
        if (strstr(line, needle)) return 0;
        if (strstr(line, "ota_error")) {
            if (reason_out && reason_size) {
                const char *r = strstr(line, "\"reason\":\"");
                if (r) {
                    r += strlen("\"reason\":\"");
                    const char *e = strchr(r, '"');
                    size_t rlen = e ? (size_t)(e - r) : strlen(r);
                    if (rlen >= reason_size) rlen = reason_size - 1;
                    memcpy(reason_out, r, rlen);
                    reason_out[rlen] = 0;
                } else {
                    reason_out[0] = 0;
                }
            }
            return -2;
        }
        /* Other line (e.g. a stray detection JSON from pre-stop backlog,
         * or an ota_progress) — ignore and keep reading. */
    }
}

static int relay_wait_for_with_resend(int scanner_id,
                                      uart_port_t uart_num,
                                      const char *cmd,
                                      const char *needle,
                                      const char *session_id,
                                      int timeout_ms,
                                      int resend_ms,
                                      char *reason_out,
                                      size_t reason_size)
{
    char line[512];
    int64_t start_ms = esp_timer_get_time() / 1000;
    int64_t deadline_ms = start_ms + timeout_ms;
    int64_t next_send_ms = start_ms;
    int lines_seen = 0;

    ESP_LOGW(TAG, "relay_wait_for_with_resend(%s): timeout=%dms resend=%dms",
             needle, timeout_ms, resend_ms);

    while (true) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms >= deadline_ms) {
            ESP_LOGW(TAG, "relay_wait_for_with_resend(%s) TIMEOUT after %lldms — %d lines seen",
                     needle, (long long)(now_ms - start_ms), lines_seen);
            return -1;
        }

        if (now_ms >= next_send_ms) {
            uart_rx_send_command_to_scanner(scanner_id, cmd);
            next_send_ms = now_ms + resend_ms;
        }

        int remaining_ms = (int)(deadline_ms - now_ms);
        int until_resend_ms = (int)(next_send_ms - now_ms);
        int read_timeout_ms = remaining_ms;
        if (until_resend_ms > 0 && until_resend_ms < read_timeout_ms) {
            read_timeout_ms = until_resend_ms;
        }
        if (read_timeout_ms > 50) {
            read_timeout_ms = 50;
        }
        if (read_timeout_ms <= 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        int n = relay_read_line(uart_num, line, sizeof(line), read_timeout_ms);
        if (n < 0) {
            continue;
        }
        lines_seen++;
        ESP_LOGI(TAG, "relay_wait_for_with_resend(%s) line[%d]: %.80s",
                 needle, lines_seen, line);
        if (strstr(line, needle)) {
#ifdef FOF_BADGE_VARIANT
            if (session_id && session_id[0] &&
                !relay_line_session_matches_or_legacy(line, session_id)) {
                ESP_LOGW(TAG,
                         "relay_wait_for_with_resend(%s) ignored stale session line: %.80s",
                         needle, line);
                continue;
            }
#else
            (void)session_id;
#endif
            return 0;
        }
        if (strstr(line, "ota_error")) {
            if (reason_out && reason_size) {
                const char *r = strstr(line, "\"reason\":\"");
                if (r) {
                    r += strlen("\"reason\":\"");
                    const char *e = strchr(r, '"');
                    size_t rlen = e ? (size_t)(e - r) : strlen(r);
                    if (rlen >= reason_size) rlen = reason_size - 1;
                    memcpy(reason_out, r, rlen);
                    reason_out[rlen] = 0;
                } else {
                    reason_out[0] = 0;
                }
            }
            return -2;
        }
    }
}

/* Extract an integer "seq" field from a NACK line. Returns -1 if not parseable. */
static int relay_extract_seq(const char *line)
{
    const char *s = strstr(line, "\"seq\":");
    if (!s) return -1;
    s += strlen("\"seq\":");
    return (int)strtol(s, NULL, 10);
}

static int64_t relay_timeout_ms_for_size(uint32_t size_bytes)
{
#ifdef FOF_BADGE_VARIANT
    return (int64_t)OTA_RELAY_TIMEOUT_FOR_SIZE_MS(size_bytes);
#else
    (void)size_bytes;
    return 15 * 60 * 1000;
#endif
}

#ifdef FOF_BADGE_VARIANT
static void relay_emit_progress(int scanner_id,
                                const char *stage,
                                uint32_t bytes,
                                uint32_t size,
                                int chunks,
                                int nacks,
                                int retries,
                                int64_t start_ms,
                                const char *error)
{
    int percent = size ? (int)(((uint64_t)bytes * 100ULL) / size) : 0;
    int64_t elapsed_s = ((esp_timer_get_time() / 1000) - start_ms) / 1000;
    printf("FOF_FW_RELAY_PROGRESS:{\"uart\":\"%s\",\"stage\":\"%s\","
           "\"bytes\":%lu,\"size\":%lu,\"percent\":%d,"
           "\"chunks\":%d,\"nacks\":%d,\"retries\":%d,"
           "\"elapsed_s\":%lld,\"error\":\"%s\"}\n",
           scanner_id == 1 ? "wifi" : "ble",
           stage ? stage : "relay",
           (unsigned long)bytes,
           (unsigned long)size,
           percent,
           chunks,
           nacks,
           retries,
           (long long)elapsed_s,
           error ? error : "");
    fflush(stdout);
}
#else
static void relay_emit_progress(int scanner_id,
                                const char *stage,
                                uint32_t bytes,
                                uint32_t size,
                                int chunks,
                                int nacks,
                                int retries,
                                int64_t start_ms,
                                const char *error)
{
    (void)scanner_id;
    (void)stage;
    (void)bytes;
    (void)size;
    (void)chunks;
    (void)nacks;
    (void)retries;
    (void)start_ms;
    (void)error;
}
#endif

/* Non-blocking peek for a NACK line. Returns seq number on NACK hit, -1 if
 * nothing relevant read within timeout_ms. Silently consumes other lines. */
static int relay_poll_nack(uart_port_t uart_num,
                           const char *session_id,
                           int timeout_ms)
{
    char line[512];
    size_t buffered = 0;
    if (uart_get_buffered_data_len(uart_num, &buffered) != ESP_OK ||
        buffered == 0) {
        return -1;
    }
    int64_t deadline_ms = (esp_timer_get_time() / 1000) + timeout_ms;
    while ((esp_timer_get_time() / 1000) < deadline_ms) {
        int remaining = (int)(deadline_ms - (esp_timer_get_time() / 1000));
        if (remaining <= 0) break;
        int n = relay_read_line(uart_num, line, sizeof(line), remaining);
        if (n < 0) break;
        if (strstr(line, "ota_nack")) {
            if (!relay_line_session_matches_or_legacy(line, session_id)) {
                ESP_LOGW(TAG, "Ignoring stale OTA NACK: %.80s", line);
                continue;
            }
            return relay_extract_seq(line);
        }
        /* consume other lines and keep polling */
    }
    return -1;
}

/* ── Firmware offer + relay core ───────────────────────────────────────── */

typedef struct {
    bool ok;
    bool legacy;
    uint32_t size;
    uint32_t bytes;
    int chunks;
    int nacks;
    int retries;
    int64_t elapsed_s;
    char stage[16];
    char error[64];
    uint32_t cmd_rx_before;
    uint32_t cmd_rx_after;
    uint32_t fw_check_before;
    uint32_t fw_check_after;
    int64_t cmd_age_after_s;
    char scanner_version[32];
    char scanner_fw_state[16];
} fw_relay_result_t;

typedef struct {
    bool received;
    uint32_t cmd_rx;
    uint32_t fw_check;
    int64_t cmd_age_s;
    char version[32];
    char board[40];
    char fw_state[16];
} fw_command_health_t;

static void remember_relay_result(int scanner_id, const fw_relay_result_t *result)
{
    if (!result || scanner_id < 0 || scanner_id >= 2) {
        return;
    }
    s_last_relay[scanner_id].ok = result->ok;
    s_last_relay[scanner_id].size = result->size;
    s_last_relay[scanner_id].bytes = result->bytes;
    strncpy(s_last_relay[scanner_id].stage, result->stage,
            sizeof(s_last_relay[scanner_id].stage) - 1);
    s_last_relay[scanner_id].stage[sizeof(s_last_relay[scanner_id].stage) - 1] = '\0';
    strncpy(s_last_relay[scanner_id].error, result->error,
            sizeof(s_last_relay[scanner_id].error) - 1);
    s_last_relay[scanner_id].error[sizeof(s_last_relay[scanner_id].error) - 1] = '\0';
    s_last_relay[scanner_id].chunks = result->chunks;
    s_last_relay[scanner_id].nacks = result->nacks;
    s_last_relay[scanner_id].retries = result->retries;
    s_last_relay[scanner_id].elapsed_s = result->elapsed_s;
    s_last_relay[scanner_id].finished_ms = esp_timer_get_time() / 1000;
    s_last_relay[scanner_id].cmd_rx_before = result->cmd_rx_before;
    s_last_relay[scanner_id].cmd_rx_after = result->cmd_rx_after;
    s_last_relay[scanner_id].fw_check_before = result->fw_check_before;
    s_last_relay[scanner_id].fw_check_after = result->fw_check_after;
    s_last_relay[scanner_id].cmd_age_after_s = result->cmd_age_after_s;
    strncpy(s_last_relay[scanner_id].scanner_version, result->scanner_version,
            sizeof(s_last_relay[scanner_id].scanner_version) - 1);
    s_last_relay[scanner_id].scanner_version[sizeof(s_last_relay[scanner_id].scanner_version) - 1] = '\0';
    strncpy(s_last_relay[scanner_id].scanner_fw_state, result->scanner_fw_state,
            sizeof(s_last_relay[scanner_id].scanner_fw_state) - 1);
    s_last_relay[scanner_id].scanner_fw_state[sizeof(s_last_relay[scanner_id].scanner_fw_state) - 1] = '\0';
}

static void capture_command_health(int scanner_id, fw_command_health_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->cmd_age_s = -1;
    const scanner_info_t *info = (scanner_id == 0)
        ? uart_rx_get_ble_scanner_info()
        : uart_rx_get_wifi_scanner_info();
    if (!info) {
        return;
    }
    out->received = info->received;
    out->cmd_rx = info->cmd_rx_count;
    out->fw_check = info->fw_check_count;
    out->cmd_age_s = info->cmd_last_age_s;
    strncpy(out->version, info->version, sizeof(out->version) - 1);
    strncpy(out->board, info->board, sizeof(out->board) - 1);
    strncpy(out->fw_state, info->fw_update_state, sizeof(out->fw_state) - 1);
}

static bool command_health_moved(const fw_command_health_t *before,
                                 const fw_command_health_t *after)
{
    if (!after || !after->received) {
        return false;
    }
    if (before && after->cmd_rx > before->cmd_rx) {
        return true;
    }
    if (before && after->fw_check > before->fw_check) {
        return true;
    }
    return after->cmd_age_s >= 0 && after->cmd_age_s <= 3;
}

static bool probe_scanner_command_ingress(int scanner_id,
                                          fw_command_health_t *before,
                                          fw_command_health_t *after)
{
    capture_command_health(scanner_id, before);
    const scanner_info_t *info = (scanner_id == 0)
        ? uart_rx_get_ble_scanner_info()
        : uart_rx_get_wifi_scanner_info();
    const char *profile = (info && info->scan_profile[0])
        ? info->scan_profile
        : "hybrid_failover";
    char probe_cmd[96];
    snprintf(probe_cmd, sizeof(probe_cmd),
             "{\"type\":\"scan_profile\",\"%s\":\"%s\"}",
             JSON_KEY_SCAN_PROFILE, profile);
    (void)uart_rx_send_command_to_scanner_checked(scanner_id, probe_cmd);

    int64_t deadline_ms = (esp_timer_get_time() / 1000) + 2500;
    do {
        vTaskDelay(pdMS_TO_TICKS(250));
        capture_command_health(scanner_id, after);
        if (command_health_moved(before, after)) {
            return true;
        }
    } while ((esp_timer_get_time() / 1000) < deadline_ms);

    capture_command_health(scanner_id, after);
    return command_health_moved(before, after);
}

#ifdef FOF_BADGE_VARIANT
static bool badge_candidate_seen(const int *pins, int count, int pin)
{
    for (int i = 0; i < count; i++) {
        if (pins[i] == pin) {
            return true;
        }
    }
    return false;
}

static int badge_default_tx_pin_for_scanner(int scanner_id)
{
    return scanner_id == 1 ? CONFIG_WIFI_SCANNER_TX_PIN : CONFIG_BLE_SCANNER_TX_PIN;
}

static int badge_peer_tx_pin_for_scanner(int scanner_id)
{
    return scanner_id == 1 ? CONFIG_BLE_SCANNER_TX_PIN : CONFIG_WIFI_SCANNER_TX_PIN;
}

static bool badge_try_heal_command_tx_pin(int scanner_id,
                                          fw_command_health_t *before,
                                          fw_command_health_t *after,
                                          int *healed_pin)
{
    int candidates[8];
    int count = 0;
    const int raw[] = {
        badge_default_tx_pin_for_scanner(scanner_id),
        badge_peer_tx_pin_for_scanner(scanner_id),
        scanner_id == 1 ? 15 : 17,
        scanner_id == 1 ? 17 : 15,
        scanner_id == 1 ? 16 : 18,
        scanner_id == 1 ? 18 : 16,
    };
    const int rx_pin = scanner_id == 1 ? CONFIG_WIFI_SCANNER_RX_PIN : CONFIG_BLE_SCANNER_RX_PIN;
    const int peer_rx_pin = scanner_id == 1 ? CONFIG_BLE_SCANNER_RX_PIN : CONFIG_WIFI_SCANNER_RX_PIN;

    for (int i = 0; i < (int)(sizeof(raw) / sizeof(raw[0])); i++) {
        if (raw[i] >= 0 &&
            raw[i] != rx_pin &&
            raw[i] != peer_rx_pin &&
            !badge_candidate_seen(candidates, count, raw[i])) {
            candidates[count++] = raw[i];
        }
    }

    ESP_LOGW(TAG, "Badge scanner[%d] command ingress unhealthy; probing %d TX pins",
             scanner_id, count);
    for (int i = 0; i < count; i++) {
        int pin = candidates[i];
        fw_command_health_t pin_before = {0};
        fw_command_health_t pin_after = {0};
        if (!uart_rx_set_scanner_tx_pin_for_badge_probe(scanner_id, pin)) {
            continue;
        }
        if (probe_scanner_command_ingress(scanner_id, &pin_before, &pin_after)) {
            if (before) *before = pin_before;
            if (after) *after = pin_after;
            if (healed_pin) *healed_pin = pin;
            ESP_LOGW(TAG,
                     "Badge scanner[%d] command TX pin healed: GPIO%d "
                     "(cmd_rx %lu->%lu fw_check %lu->%lu)",
                     scanner_id,
                     pin,
                     (unsigned long)pin_before.cmd_rx,
                     (unsigned long)pin_after.cmd_rx,
                     (unsigned long)pin_before.fw_check,
                     (unsigned long)pin_after.fw_check);
            return true;
        }
    }

    (void)uart_rx_set_scanner_tx_pin_for_badge_probe(
        scanner_id,
        badge_default_tx_pin_for_scanner(scanner_id)
    );
    ESP_LOGE(TAG, "Badge scanner[%d] command TX pin probe failed; restored GPIO%d",
             scanner_id, badge_default_tx_pin_for_scanner(scanner_id));
    return false;
}
#endif

static const char *normalized_version(const char *v)
{
    if (!v) return "";
    return (v[0] == 'v' || v[0] == 'V') ? v + 1 : v;
}

static bool staged_version_is_known(const fw_store_info_t *info)
{
    return info && info->version[0] != '\0';
}

static bool relay_line_matches_staged_version(const char *line,
                                              const fw_store_info_t *info)
{
    if (!line || !staged_version_is_known(info)) {
        return !staged_version_is_known(info);
    }
    const char *normalized = normalized_version(info->version);
    return (normalized[0] && strstr(line, normalized)) ||
           (info->version[0] && strstr(line, info->version));
}

static bool relay_line_is_scanner_identity(const char *line)
{
    if (!line) return false;
    return strstr(line, "scanner_info") ||
           strstr(line, "\"scanner-s3-combo\"") ||
           strstr(line, "\"scanner-s3-combo-seed\"") ||
           strstr(line, "\"scanner-s3-combo-fof_badge\"");
}

static bool staged_firmware_matches_scanner(const fw_store_info_t *info,
                                            const char *scanner_board)
{
    if (!info || !info->stored) return false;
    if (!info->name[0] || !scanner_board || !scanner_board[0]) {
        return false;
    }
    return strcmp(info->name, scanner_board) == 0;
}

static bool staged_firmware_is_newer_for_scanner(const fw_store_info_t *info,
                                                 const char *scanner_board,
                                                 const char *scanner_version)
{
    if (!staged_firmware_matches_scanner(info, scanner_board)) return false;
    if (!info->version[0] || !scanner_version || !scanner_version[0]) return false;
    return strcmp(normalized_version(info->version),
                  normalized_version(scanner_version)) != 0;
}

static void send_fw_offer(int scanner_id, bool update, const fw_store_info_t *info,
                          const char *reason)
{
    char cmd[240];
    snprintf(cmd, sizeof(cmd),
             "{\"type\":\"%s\",\"update\":%s,\"target_ver\":\"%s\","
             "\"fw_name\":\"%s\",\"size\":%lu,\"crc\":%lu,\"reason\":\"%s\"}",
             MSG_TYPE_FW_OFFER,
             update ? "true" : "false",
             (info && info->version[0]) ? info->version : "",
             (info && info->name[0]) ? info->name : "",
             (unsigned long)((info) ? info->size : 0),
             (unsigned long)((info) ? info->checksum : 0),
             reason ? reason : "");
    uart_rx_send_command_to_scanner(scanner_id, cmd);
}

static bool fw_relay_stored_to_scanner(int scanner_id,
                                       bool scanner_already_quiet,
                                       bool legacy_mode,
                                       bool force_probe_skip,
                                       bool allow_same_version,
                                       fw_relay_result_t *result)
{
    fw_relay_result_t local = {0};
    if (!result) result = &local;
    memset(result, 0, sizeof(*result));
    snprintf(result->stage, sizeof(result->stage), "init");

    if (s_operation_active) {
        snprintf(result->error, sizeof(result->error), "operation_active");
        remember_relay_result(scanner_id, result);
        return false;
    }

    fw_store_info_t info = {0};
    if (!fw_store_get_info(&info)) {
        snprintf(result->error, sizeof(result->error), "no_firmware_stored");
        remember_relay_result(scanner_id, result);
        return false;
    }
    result->legacy = legacy_mode;
    result->size = info.size;

    const esp_partition_t *p = find_fw_partition();
    if (!p) {
        snprintf(result->error, sizeof(result->error), "partition_not_found");
        remember_relay_result(scanner_id, result);
        return false;
    }

    uart_port_t uart_num = scanner_id == 1 ? CONFIG_WIFI_SCANNER_UART : CONFIG_BLE_SCANNER_UART;
    const char *uart_target = scanner_id == 1 ? "wifi" : "ble";
    s_operation_active = true;

    ESP_LOGW(TAG, "Relay v2: %lu bytes from '%s' to UART%d (uart=%s quiet=%s legacy=%s) heap=%lu",
             (unsigned long)info.size, p->label, uart_num, uart_target,
             scanner_already_quiet ? "true" : "false",
             legacy_mode ? "true" : "false",
             (unsigned long)esp_get_free_heap_size());

    fw_command_health_t cmd_before = {0};
    fw_command_health_t cmd_after = {0};
    bool cmd_ingress_ok = false;
    if (force_probe_skip) {
        capture_command_health(scanner_id, &cmd_before);
        cmd_after = cmd_before;
        cmd_ingress_ok = true;
        ESP_LOGW(TAG, "Relay force mode: skipping command-health probe for scanner[%d]",
                 scanner_id);
    } else {
        cmd_ingress_ok = probe_scanner_command_ingress(
            scanner_id, &cmd_before, &cmd_after);
#ifdef FOF_BADGE_VARIANT
        if (!cmd_ingress_ok) {
            int healed_pin = -1;
            cmd_ingress_ok = badge_try_heal_command_tx_pin(
                scanner_id, &cmd_before, &cmd_after, &healed_pin);
            if (cmd_ingress_ok) {
                ESP_LOGW(TAG, "Relay using badge scanner[%d] healed TX GPIO%d",
                         scanner_id, healed_pin);
            }
        }
#endif
    }
    strncpy(result->scanner_version, cmd_after.version[0] ? cmd_after.version : cmd_before.version,
            sizeof(result->scanner_version) - 1);
    strncpy(result->scanner_fw_state, cmd_after.fw_state[0] ? cmd_after.fw_state : cmd_before.fw_state,
            sizeof(result->scanner_fw_state) - 1);
    result->cmd_rx_before = cmd_before.cmd_rx;
    result->cmd_rx_after = cmd_after.cmd_rx;
    result->fw_check_before = cmd_before.fw_check;
    result->fw_check_after = cmd_after.fw_check;
    result->cmd_age_after_s = cmd_after.cmd_age_s;

    if (!cmd_ingress_ok) {
        snprintf(result->stage, sizeof(result->stage), "probe");
        snprintf(result->error, sizeof(result->error), "command_ingress_unhealthy");
        ESP_LOGE(TAG,
                 "Relay refused: scanner[%d] command ingress unhealthy "
                 "(cmd_rx %lu->%lu fw_check %lu->%lu cmd_age=%lld ver=%s fw=%s)",
                 scanner_id,
                 (unsigned long)cmd_before.cmd_rx,
                 (unsigned long)cmd_after.cmd_rx,
                 (unsigned long)cmd_before.fw_check,
                 (unsigned long)cmd_after.fw_check,
                 (long long)cmd_after.cmd_age_s,
                 result->scanner_version[0] ? result->scanner_version : "?",
                 result->scanner_fw_state[0] ? result->scanner_fw_state : "?");
        s_operation_active = false;
        remember_relay_result(scanner_id, result);
        return false;
    }

    const char *scanner_board = cmd_after.board[0] ? cmd_after.board : cmd_before.board;
    const char *scanner_version = cmd_after.version[0] ? cmd_after.version : cmd_before.version;
    if (scanner_board && scanner_board[0] &&
        !staged_firmware_matches_scanner(&info, scanner_board)) {
        snprintf(result->stage, sizeof(result->stage), "version");
        snprintf(result->error, sizeof(result->error), "board_mismatch");
        ESP_LOGE(TAG, "Relay refused: staged board=%s scanner board=%s",
                 info.name[0] ? info.name : "?",
                 scanner_board);
        s_operation_active = false;
        remember_relay_result(scanner_id, result);
        return false;
    }
    if (!allow_same_version &&
        scanner_board && scanner_board[0] &&
        scanner_version && scanner_version[0] &&
        staged_firmware_matches_scanner(&info, scanner_board) &&
        !staged_firmware_is_newer_for_scanner(&info, scanner_board, scanner_version)) {
        snprintf(result->stage, sizeof(result->stage), "version");
        snprintf(result->error, sizeof(result->error), "same_version_refused");
        ESP_LOGW(TAG,
                 "Relay refused: scanner[%d] already has %s (%s). "
                 "Use explicit same-version override only for recovery.",
                 scanner_id,
                 scanner_version,
                 scanner_board);
        s_operation_active = false;
        remember_relay_result(scanner_id, result);
        return false;
    }

    http_upload_pause();
    vTaskDelay(pdMS_TO_TICKS(500));
    uart_rx_pause_scanner(scanner_id);

    bool relay_ok = true;
    char error_msg[64] = {0};
    char stage[16] = "init";
    int64_t start_ms = esp_timer_get_time() / 1000;
    uint16_t seq = 0;
    int nack_count = 0;
    int total_retries = 0;
    char relay_session_id[16] = {0};
    int64_t overall_timeout_ms = relay_timeout_ms_for_size(info.size);

    relay_emit_progress(scanner_id, "stop", 0, info.size, 0, 0, 0,
                        start_ms, "");

    snprintf(stage, sizeof(stage), "stop");
    {
        if (!scanner_already_quiet) {
            int r = relay_wait_for_with_resend(
                scanner_id,
                uart_num,
                "{\"type\":\"stop\"}",
                "stop_ack",
                NULL,
                FW_RELAY_STOP_STORM_MS,
                FW_RELAY_STOP_STORM_STEP_MS,
                NULL,
                0
            );
            if (r == 0) {
                ESP_LOGI(TAG, "Stage stop: stop_ack received");
            } else {
                if (force_probe_skip) {
                    ESP_LOGW(TAG,
                             "Stage stop: no stop_ack in recovery force mode; "
                             "flushing scanner UART and trying ota_begin");
                } else {
                    snprintf(error_msg, sizeof(error_msg), "command_ingress_unhealthy");
                    relay_ok = false;
                    goto relay_done;
                }
            }
        } else {
            ESP_LOGW(TAG, "Stage stop: scanner pre-quiet via fw_ready");
        }
        uart_flush_input(uart_num);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    snprintf(stage, sizeof(stage), "begin");
    relay_emit_progress(scanner_id, "begin", 0, info.size, 0, 0, 0,
                        start_ms, "");
    {
        uint32_t fw_crc32 = 0;
        bool has_crc = nvs_config_get_u32(NVS_FW_CRC32, &fw_crc32);
        char session_id[16];
        snprintf(session_id, sizeof(session_id), "r%08lx",
                 (unsigned long)esp_random());
        strncpy(relay_session_id, session_id, sizeof(relay_session_id) - 1);
        char cmd[144];
        if (has_crc) {
            snprintf(cmd, sizeof(cmd),
                     "{\"type\":\"ota_begin\",\"size\":%lu,\"crc\":%lu,"
                     "\"session_id\":\"%s\"}",
                     (unsigned long)info.size, (unsigned long)fw_crc32,
                     session_id);
        } else {
            snprintf(cmd, sizeof(cmd),
                     "{\"type\":\"ota_begin\",\"size\":%lu,"
                     "\"session_id\":\"%s\"}",
                     (unsigned long)info.size,
                     session_id);
        }
        uart_rx_send_command_to_scanner(scanner_id, cmd);
        ESP_LOGI(TAG, "Stage begin: sent ota_begin (%lu bytes, session=%s CRC=%s%08lX)",
                 (unsigned long)info.size, session_id,
                 has_crc ? "" : "none/", (unsigned long)fw_crc32);

        char reason[48] = {0};
        int r = relay_wait_for_with_resend(
            scanner_id,
            uart_num,
            cmd,
            "ota_ack",
            session_id,
            15000,
            500,
            reason,
            sizeof(reason)
        );
        if (r != 0) {
            snprintf(error_msg, sizeof(error_msg),
                     r == -2 ? "scanner_error:%s" : "ota_ack_timeout",
                     r == -2 ? reason : "");
            relay_ok = false;
            goto relay_done;
        }
        ESP_LOGI(TAG, "Stage begin: ota_ack received, entering chunk stream");
    }

    snprintf(stage, sizeof(stage), "chunks");
    relay_emit_progress(scanner_id, "chunks", 0, info.size, 0, nack_count,
                        total_retries, start_ms, "");
    {
        static uint8_t read_buf[OTA_CHUNK_MAX_DATA];
        static uint8_t frame[OTA_CHUNK_HEADER_SIZE + OTA_CHUNK_MAX_DATA + OTA_CHUNK_CRC_SIZE];
        uint32_t offset = 0;
        uint32_t remaining = info.size;
        uint8_t retry_for_seq[8] = {0};
        const uint32_t relay_chunk_data =
            FW_RELAY_CHUNK_DATA < OTA_CHUNK_MAX_DATA ? FW_RELAY_CHUNK_DATA : OTA_CHUNK_MAX_DATA;
        uint32_t next_progress_bytes = OTA_RELAY_PROGRESS_INTERVAL_BYTES;
        int64_t next_progress_ms = (esp_timer_get_time() / 1000) + 5000;

        while (remaining > 0) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            if ((now_ms - start_ms) > overall_timeout_ms) {
                snprintf(error_msg, sizeof(error_msg), "overall_timeout");
                relay_ok = false;
                goto relay_done;
            }

            uint32_t chunk_len = remaining > relay_chunk_data ? relay_chunk_data : remaining;
            esp_err_t err = esp_partition_read(p, offset, read_buf, chunk_len);
            if (err != ESP_OK) {
                snprintf(error_msg, sizeof(error_msg), "flash_read_%lu", (unsigned long)offset);
                relay_ok = false;
                goto relay_done;
            }

            uint32_t crc = esp_rom_crc32_le(0, read_buf, chunk_len);
            frame[0] = OTA_CHUNK_MAGIC;
            frame[1] = (uint8_t)(seq >> 8);
            frame[2] = (uint8_t)(seq & 0xFF);
            frame[3] = (uint8_t)(chunk_len >> 8);
            frame[4] = (uint8_t)(chunk_len & 0xFF);
            memcpy(frame + OTA_CHUNK_HEADER_SIZE, read_buf, chunk_len);
            int crc_off = OTA_CHUNK_HEADER_SIZE + chunk_len;
            frame[crc_off + 0] = (uint8_t)(crc >> 24);
            frame[crc_off + 1] = (uint8_t)(crc >> 16);
            frame[crc_off + 2] = (uint8_t)(crc >> 8);
            frame[crc_off + 3] = (uint8_t)(crc);
            int frame_len = OTA_CHUNK_HEADER_SIZE + chunk_len + OTA_CHUNK_CRC_SIZE;

            uart_write_bytes(uart_num, (char *)frame, frame_len);
            uart_wait_tx_done(uart_num, pdMS_TO_TICKS(1000));

            int nack_seq = relay_poll_nack(uart_num, relay_session_id,
                                           FW_RELAY_NACK_POLL_MS);
            if (nack_seq >= 0) {
                if (nack_seq > (int)seq) {
                    ESP_LOGW(TAG, "NACK seq=%d > current seq=%d, ignoring", nack_seq, seq);
                } else {
                    uint32_t nack_offset = (uint32_t)nack_seq * relay_chunk_data;
                    uint32_t rewind_bytes = offset > nack_offset ? (offset - nack_offset) : 0;
                    uint8_t slot = (uint8_t)(nack_seq & 0x7);
                    if (++retry_for_seq[slot] > 3) {
                        snprintf(error_msg, sizeof(error_msg),
                                 "chunk_%d_crc_retries_exhausted", nack_seq);
                        relay_ok = false;
                        goto relay_done;
                    }
                    nack_count++;
                    total_retries++;
                    offset -= rewind_bytes;
                    remaining = info.size - offset;
                    seq = (uint16_t)nack_seq;
                    result->bytes = offset;
                    ESP_LOGW(TAG, "NACK seq=%d -> rewind %lu bytes (retry %u)",
                             nack_seq, (unsigned long)rewind_bytes,
                             retry_for_seq[slot]);
                    continue;
                }
            }

            offset += chunk_len;
            remaining -= chunk_len;
            seq++;
            result->bytes = offset;

            if ((info.size - remaining) % (100 * 1024) < relay_chunk_data) {
                ESP_LOGI(TAG, "Relay: %lu/%lu (%.0f%%) seq=%d nacks=%d heap=%lu",
                         (unsigned long)(info.size - remaining), (unsigned long)info.size,
                         (float)(info.size - remaining) / info.size * 100,
                         seq, nack_count,
                         (unsigned long)esp_get_free_heap_size());
            }
            now_ms = esp_timer_get_time() / 1000;
            if (offset >= next_progress_bytes || now_ms >= next_progress_ms ||
                remaining == 0) {
                relay_emit_progress(scanner_id, "chunks", offset, info.size,
                                    seq, nack_count, total_retries,
                                    start_ms, "");
                while (next_progress_bytes <= offset) {
                    next_progress_bytes += OTA_RELAY_PROGRESS_INTERVAL_BYTES;
                }
                next_progress_ms = now_ms + 5000;
            }
        }
    }

    snprintf(stage, sizeof(stage), "end");
    relay_emit_progress(scanner_id, "end", info.size, info.size, seq,
                        nack_count, total_retries, start_ms, "");
    {
        uart_rx_send_command_to_scanner(scanner_id, "{\"type\":\"ota_end\"}");

        char line[192];
        int64_t deadline = (esp_timer_get_time() / 1000) + 60000;
        bool saw_done_or_boot = false;
        int result_code = -1;
        char reason[48] = {0};
        while ((esp_timer_get_time() / 1000) < deadline) {
            int remaining_ms = (int)(deadline - (esp_timer_get_time() / 1000));
            if (remaining_ms <= 0) break;
            int n = relay_read_line(uart_num, line, sizeof(line), remaining_ms);
            if (n < 0) break;
            if (strstr(line, "ota_done")) { result_code = 0; saw_done_or_boot = true; break; }
            if (relay_line_is_scanner_identity(line)) {
                if (relay_line_matches_staged_version(line, &info)) {
                    result_code = 0;
                    saw_done_or_boot = true;
                    ESP_LOGW(TAG, "Stage end: scanner identity/version after reboot -> implicit ota_done");
                    break;
                }
                ESP_LOGW(TAG, "Stage end: scanner identity did not match staged version yet");
            }
            if (strstr(line, "ota_error")) {
                const char *r = strstr(line, "\"reason\":\"");
                if (r) {
                    r += strlen("\"reason\":\"");
                    const char *e = strchr(r, '"');
                    size_t rlen = e ? (size_t)(e - r) : strlen(r);
                    if (rlen >= sizeof(reason)) rlen = sizeof(reason) - 1;
                    memcpy(reason, r, rlen); reason[rlen] = 0;
                }
                result_code = -2; break;
            }
        }

        if (result_code == -2) {
            snprintf(error_msg, sizeof(error_msg), "scanner_error:%s",
                     reason[0] ? reason : "unknown");
            relay_ok = false;
            goto relay_done;
        } else if (!saw_done_or_boot) {
            snprintf(error_msg, sizeof(error_msg), "finalize_timeout");
            relay_ok = false;
            goto relay_done;
        }
        ESP_LOGW(TAG, "Stage end: scanner acknowledged, rebooting");
    }

relay_done:;
    if (!relay_ok) {
        relay_emit_progress(scanner_id, stage, result->bytes, info.size, seq,
                            nack_count, total_retries, start_ms, error_msg);
        char abort_cmd[96];
        if (relay_session_id[0]) {
            snprintf(abort_cmd, sizeof(abort_cmd),
                     "{\"type\":\"ota_abort\",\"session_id\":\"%s\"}",
                     relay_session_id);
        } else {
            snprintf(abort_cmd, sizeof(abort_cmd), "{\"type\":\"ota_abort\"}");
        }
        uart_rx_send_command_to_scanner(scanner_id, abort_cmd);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    uart_rx_resume_scanner(scanner_id);

    int64_t elapsed_s = ((esp_timer_get_time() / 1000) - start_ms) / 1000;
    if (!relay_ok) {
        uart_rx_send_command_to_scanner(scanner_id, "{\"type\":\"start\"}");
    }

    if (relay_ok) {
        relay_emit_progress(scanner_id, "done", info.size, info.size, seq,
                            nack_count, total_retries, start_ms, "");
        ESP_LOGW(TAG, "Relay complete: %lu bytes, %d chunks, %d nacks in %llds",
                 (unsigned long)info.size, seq, nack_count, (long long)elapsed_s);
    } else {
        ESP_LOGE(TAG, "Relay FAILED @ %s: %s (%d chunks in %llds)",
                 stage, error_msg, seq, (long long)elapsed_s);
    }

    http_upload_resume();
    s_operation_active = false;

    result->ok = relay_ok;
    result->legacy = legacy_mode;
    result->size = info.size;
    if (relay_ok) {
        result->bytes = info.size;
    }
    result->chunks = seq;
    result->nacks = nack_count;
    result->retries = total_retries;
    result->elapsed_s = elapsed_s;
    strncpy(result->stage, stage, sizeof(result->stage) - 1);
    result->stage[sizeof(result->stage) - 1] = '\0';
    strncpy(result->error, error_msg, sizeof(result->error) - 1);
    result->error[sizeof(result->error) - 1] = '\0';
    remember_relay_result(scanner_id, result);
    return relay_ok;
}

/* ── POST /api/fw/relay — staged handshake + fire-and-forget with NACK ──── */

static esp_err_t fw_relay_handler(httpd_req_t *req)
{
    char query[96] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char uart_target[8] = "ble";
    httpd_query_key_value(query, "uart", uart_target, sizeof(uart_target));
    char mode[16] = {0};
    char legacy_param[8] = {0};
    char force_param[8] = {0};
    char allow_same_param[8] = {0};
    httpd_query_key_value(query, "mode", mode, sizeof(mode));
    httpd_query_key_value(query, "legacy", legacy_param, sizeof(legacy_param));
    httpd_query_key_value(query, "force", force_param, sizeof(force_param));
    httpd_query_key_value(query, "allow_same_version",
                          allow_same_param, sizeof(allow_same_param));
    bool legacy_mode =
        strcmp(mode, "legacy") == 0 ||
        strcmp(legacy_param, "1") == 0 ||
        strcmp(legacy_param, "true") == 0;
    bool force_probe_skip =
        strcmp(force_param, "1") == 0 ||
        strcmp(force_param, "true") == 0;
    bool allow_same_version =
        strcmp(allow_same_param, "1") == 0 ||
        strcmp(allow_same_param, "true") == 0;
    int scanner_id = (strcmp(uart_target, "wifi") == 0) ? 1 : 0;

    fw_relay_result_t result = {0};
    fw_relay_stored_to_scanner(scanner_id, false, legacy_mode, force_probe_skip,
                               allow_same_version, &result);

    char resp_buf[900];
    snprintf(resp_buf, sizeof(resp_buf),
             "{\"ok\":%s,\"legacy\":%s,\"size\":%lu,\"bytes\":%lu,"
             "\"chunks\":%d,\"nacks\":%d,"
             "\"retries\":%d,\"elapsed_s\":%lld,\"stage\":\"%s\","
             "\"error\":\"%s\",\"cmd_rx_before\":%lu,\"cmd_rx_after\":%lu,"
             "\"fw_check_before\":%lu,\"fw_check_after\":%lu,"
             "\"cmd_age_after_s\":%lld,\"scanner_ver\":\"%s\","
             "\"scanner_fw_state\":\"%s\"}",
             result.ok ? "true" : "false",
             result.legacy ? "true" : "false",
             (unsigned long)result.size,
             (unsigned long)result.bytes,
             result.chunks, result.nacks, result.retries,
             (long long)result.elapsed_s, result.stage, result.error,
             (unsigned long)result.cmd_rx_before,
             (unsigned long)result.cmd_rx_after,
             (unsigned long)result.fw_check_before,
             (unsigned long)result.fw_check_after,
             (long long)result.cmd_age_after_s,
             result.scanner_version,
             result.scanner_fw_state);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp_buf);
    return ESP_OK;
}

bool fw_store_relay_staged_to_scanner(int scanner_id,
                                      char *out_json,
                                      size_t out_json_len)
{
    return fw_store_relay_staged_to_scanner_ex(scanner_id, false, false,
                                               out_json, out_json_len);
}

bool fw_store_relay_staged_to_scanner_ex(int scanner_id,
                                         bool force_probe_skip,
                                         bool allow_same_version,
                                         char *out_json,
                                         size_t out_json_len)
{
    if (scanner_id < 0 || scanner_id > 1) {
        if (out_json && out_json_len > 0) {
            snprintf(out_json, out_json_len,
                     "{\"ok\":false,\"error\":\"invalid_scanner\"}");
        }
        return false;
    }

    fw_relay_result_t result = {0};
    bool ok = fw_relay_stored_to_scanner(scanner_id, false, false,
                                         force_probe_skip,
                                         allow_same_version,
                                         &result);
    if (out_json && out_json_len > 0) {
        snprintf(out_json, out_json_len,
                 "{\"ok\":%s,\"uart\":\"%s\",\"size\":%lu,\"bytes\":%lu,"
                 "\"chunks\":%d,"
                 "\"nacks\":%d,\"retries\":%d,\"elapsed_s\":%lld,"
                 "\"stage\":\"%s\",\"error\":\"%s\","
                 "\"cmd_rx_before\":%lu,\"cmd_rx_after\":%lu,"
                 "\"fw_check_before\":%lu,\"fw_check_after\":%lu,"
                 "\"cmd_age_after_s\":%lld,\"scanner_ver\":\"%s\","
                 "\"scanner_fw_state\":\"%s\"}",
                 ok ? "true" : "false",
                 scanner_id == 1 ? "wifi" : "ble",
                 (unsigned long)result.size,
                 (unsigned long)result.bytes,
                 result.chunks,
                 result.nacks,
                 result.retries,
                 (long long)result.elapsed_s,
                 result.stage,
                 result.error,
                 (unsigned long)result.cmd_rx_before,
                 (unsigned long)result.cmd_rx_after,
                 (unsigned long)result.fw_check_before,
                 (unsigned long)result.fw_check_after,
                 (long long)result.cmd_age_after_s,
                 result.scanner_version,
                 result.scanner_fw_state);
    }
    return ok;
}

#ifdef FOF_BADGE_VARIANT
#define FW_AUTO_RELAY_COOLDOWN_MS (2 * 60 * 1000)
#else
#define FW_AUTO_RELAY_COOLDOWN_MS (10 * 60 * 1000)
#endif

static int64_t s_last_auto_relay_ms[2] = {0, 0};

typedef struct {
    int scanner_id;
} fw_auto_relay_args_t;

static void fw_auto_relay_task(void *arg)
{
    fw_auto_relay_args_t *args = (fw_auto_relay_args_t *)arg;
    int scanner_id = args ? args->scanner_id : 0;
    if (args) free(args);

    vTaskDelay(pdMS_TO_TICKS(250));
    fw_relay_result_t result = {0};
    fw_relay_stored_to_scanner(scanner_id, true, false, false, false, &result);
    ESP_LOGW(TAG, "Auto scanner relay[%d] ok=%d stage=%s error=%s chunks=%d",
             scanner_id, result.ok ? 1 : 0, result.stage, result.error, result.chunks);
    vTaskDelete(NULL);
}

void fw_store_handle_scanner_check(int scanner_id,
                                   const char *scanner_board,
                                   const char *scanner_version)
{
    fw_store_info_t info = {0};
    if (!fw_store_get_info(&info)) {
        send_fw_offer(scanner_id, false, NULL, "no_staged_firmware");
        return;
    }
    if (!info.name[0]) {
        send_fw_offer(scanner_id, false, &info, "staged_firmware_missing_name");
        return;
    }
    if (!staged_firmware_matches_scanner(&info, scanner_board)) {
        send_fw_offer(scanner_id, false, &info, "board_mismatch");
        return;
    }
    if (!staged_firmware_is_newer_for_scanner(&info, scanner_board, scanner_version)) {
        send_fw_offer(scanner_id, false, &info, "current");
        return;
    }
    send_fw_offer(scanner_id, true, &info, "staged_update_available");
}

void fw_store_handle_scanner_ready(int scanner_id,
                                   const char *scanner_board,
                                   const char *scanner_version)
{
    fw_store_info_t info = {0};
    int64_t now_ms = esp_timer_get_time() / 1000;

    if (scanner_id < 0 || scanner_id > 1) {
        return;
    }
    if (!fw_store_get_info(&info) ||
        !info.name[0] ||
        !staged_firmware_is_newer_for_scanner(&info, scanner_board, scanner_version)) {
        ESP_LOGW(TAG, "Scanner[%d] fw_ready ignored: no matching newer staged image",
                 scanner_id);
        uart_rx_send_command_to_scanner(scanner_id, "{\"type\":\"start\"}");
        return;
    }
    if (s_operation_active) {
        ESP_LOGW(TAG, "Scanner[%d] fw_ready deferred: operation active", scanner_id);
        uart_rx_send_command_to_scanner(scanner_id, "{\"type\":\"start\"}");
        return;
    }
    if (s_last_auto_relay_ms[scanner_id] != 0 &&
        (now_ms - s_last_auto_relay_ms[scanner_id]) < FW_AUTO_RELAY_COOLDOWN_MS) {
        ESP_LOGW(TAG, "Scanner[%d] fw_ready deferred: auto relay cooldown", scanner_id);
        uart_rx_send_command_to_scanner(scanner_id, "{\"type\":\"start\"}");
        return;
    }

    s_last_auto_relay_ms[scanner_id] = now_ms;
    fw_auto_relay_args_t *args = calloc(1, sizeof(*args));
    if (!args) {
        uart_rx_send_command_to_scanner(scanner_id, "{\"type\":\"start\"}");
        return;
    }
    args->scanner_id = scanner_id;
    BaseType_t ok = xTaskCreate(fw_auto_relay_task, "fw_auto_relay", 6144,
                                args, 5, NULL);
    if (ok != pdPASS) {
        free(args);
        uart_rx_send_command_to_scanner(scanner_id, "{\"type\":\"start\"}");
    }
}

/* ── GET /api/fw/info — check stored firmware status ─────────────────────── */

static esp_err_t fw_info_handler(httpd_req_t *req)
{
    uint32_t size = 0, cksum = 0;
    char ver[32] = {0}, name[32] = {0}, part[16] = {0};
    bool has_fw = read_fw_metadata(&size, &cksum, ver, sizeof(ver), part, sizeof(part));
    if (has_fw) nvs_config_get_string(NVS_FW_NAME, name, sizeof(name));

    int64_t now_ms = esp_timer_get_time() / 1000;
    char resp[1800];
    if (has_fw) {
        snprintf(resp, sizeof(resp),
                 "{\"stored\":true,\"size\":%lu,\"checksum\":%lu,"
                 "\"version\":\"%s\",\"name\":\"%s\",\"partition\":\"%s\","
                 "\"auto_update_enabled\":true,\"auto_relay_cooldown_s\":%d,"
                 "\"last_relay\":{\"ble\":{\"ok\":%s,\"stage\":\"%s\",\"error\":\"%s\","
                 "\"size\":%lu,\"bytes\":%lu,"
                 "\"chunks\":%d,\"nacks\":%d,\"retries\":%d,\"elapsed_s\":%lld,\"age_s\":%lld,"
                 "\"cmd_rx_before\":%lu,\"cmd_rx_after\":%lu,"
                 "\"fw_check_before\":%lu,\"fw_check_after\":%lu,"
                 "\"cmd_age_after_s\":%lld,\"scanner_ver\":\"%s\","
                 "\"scanner_fw_state\":\"%s\"},"
                 "\"wifi\":{\"ok\":%s,\"stage\":\"%s\",\"error\":\"%s\","
                 "\"size\":%lu,\"bytes\":%lu,"
                 "\"chunks\":%d,\"nacks\":%d,\"retries\":%d,\"elapsed_s\":%lld,\"age_s\":%lld,"
                 "\"cmd_rx_before\":%lu,\"cmd_rx_after\":%lu,"
                 "\"fw_check_before\":%lu,\"fw_check_after\":%lu,"
                 "\"cmd_age_after_s\":%lld,\"scanner_ver\":\"%s\","
                 "\"scanner_fw_state\":\"%s\"}}}",
                 (unsigned long)size, (unsigned long)cksum,
                 ver[0] ? ver : "", name[0] ? name : "", part[0] ? part : "",
                 (int)(FW_AUTO_RELAY_COOLDOWN_MS / 1000),
                 s_last_relay[0].ok ? "true" : "false",
                 s_last_relay[0].stage, s_last_relay[0].error,
                 (unsigned long)s_last_relay[0].size,
                 (unsigned long)s_last_relay[0].bytes,
                 s_last_relay[0].chunks, s_last_relay[0].nacks, s_last_relay[0].retries,
                 (long long)s_last_relay[0].elapsed_s,
                 (long long)(s_last_relay[0].finished_ms > 0 ? (now_ms - s_last_relay[0].finished_ms) / 1000 : -1),
                 (unsigned long)s_last_relay[0].cmd_rx_before,
                 (unsigned long)s_last_relay[0].cmd_rx_after,
                 (unsigned long)s_last_relay[0].fw_check_before,
                 (unsigned long)s_last_relay[0].fw_check_after,
                 (long long)s_last_relay[0].cmd_age_after_s,
                 s_last_relay[0].scanner_version,
                 s_last_relay[0].scanner_fw_state,
                 s_last_relay[1].ok ? "true" : "false",
                 s_last_relay[1].stage, s_last_relay[1].error,
                 (unsigned long)s_last_relay[1].size,
                 (unsigned long)s_last_relay[1].bytes,
                 s_last_relay[1].chunks, s_last_relay[1].nacks, s_last_relay[1].retries,
                 (long long)s_last_relay[1].elapsed_s,
                 (long long)(s_last_relay[1].finished_ms > 0 ? (now_ms - s_last_relay[1].finished_ms) / 1000 : -1),
                 (unsigned long)s_last_relay[1].cmd_rx_before,
                 (unsigned long)s_last_relay[1].cmd_rx_after,
                 (unsigned long)s_last_relay[1].fw_check_before,
                 (unsigned long)s_last_relay[1].fw_check_after,
                 (long long)s_last_relay[1].cmd_age_after_s,
                 s_last_relay[1].scanner_version,
                 s_last_relay[1].scanner_fw_state);
    } else {
        snprintf(resp, sizeof(resp),
                 "{\"stored\":false,\"auto_update_enabled\":true,"
                 "\"auto_relay_cooldown_s\":%d}",
                 (int)(FW_AUTO_RELAY_COOLDOWN_MS / 1000));
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* ── POST /api/fw/trigger — ask scanner(s) to run fw_check now ─────────── */

static esp_err_t fw_trigger_handler(httpd_req_t *req)
{
    char query[96] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char uart_target[8] = "both";
    httpd_query_key_value(query, "uart", uart_target, sizeof(uart_target));

    bool target_ble = strcmp(uart_target, "ble") == 0 || strcmp(uart_target, "both") == 0;
    bool target_wifi = strcmp(uart_target, "wifi") == 0 || strcmp(uart_target, "both") == 0;
    if (!target_ble && !target_wifi) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "uart must be ble, wifi, or both");
        return ESP_OK;
    }

    const char *cmd = "{\"type\":\"fw_check_now\"}";
    bool ble_sent = target_ble ? uart_rx_send_command_to_scanner_checked(0, cmd) : false;
    bool wifi_sent = target_wifi ? uart_rx_send_command_to_scanner_checked(1, cmd) : false;
    bool ok = (!target_ble || ble_sent) && (!target_wifi || wifi_sent);

    char resp[192];
    snprintf(resp, sizeof(resp),
             "{\"ok\":%s,\"uart\":\"%s\",\"ble_sent\":%s,\"wifi_sent\":%s,"
             "\"error\":\"%s\"}",
             ok ? "true" : "false",
             uart_target,
             ble_sent ? "true" : "false",
             wifi_sent ? "true" : "false",
             ok ? "" : "scanner_command_ingress_unreachable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* ── Registration ────────────────────────────────────────────────────────── */

static const httpd_uri_t uri_fw_upload = {
    .uri = "/api/fw/upload", .method = HTTP_POST, .handler = fw_upload_handler,
};
static const httpd_uri_t uri_fw_relay = {
    .uri = "/api/fw/relay", .method = HTTP_POST, .handler = fw_relay_handler,
};
static const httpd_uri_t uri_fw_info = {
    .uri = "/api/fw/info", .method = HTTP_GET, .handler = fw_info_handler,
};
static const httpd_uri_t uri_fw_trigger = {
    .uri = "/api/fw/trigger", .method = HTTP_POST, .handler = fw_trigger_handler,
};

void fw_store_register(httpd_handle_t server)
{
    esp_err_t r;
    r = httpd_register_uri_handler(server, &uri_fw_upload);
    if (r != ESP_OK) ESP_LOGE(TAG, "Failed /api/fw/upload: %s", esp_err_to_name(r));
    r = httpd_register_uri_handler(server, &uri_fw_relay);
    if (r != ESP_OK) ESP_LOGE(TAG, "Failed /api/fw/relay: %s", esp_err_to_name(r));
    r = httpd_register_uri_handler(server, &uri_fw_info);
    if (r != ESP_OK) ESP_LOGE(TAG, "Failed /api/fw/info: %s", esp_err_to_name(r));
    r = httpd_register_uri_handler(server, &uri_fw_trigger);
    if (r != ESP_OK) ESP_LOGE(TAG, "Failed /api/fw/trigger: %s", esp_err_to_name(r));

    ESP_LOGI(TAG, "Firmware store endpoints registered");
}
