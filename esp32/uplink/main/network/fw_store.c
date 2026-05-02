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

#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
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
    char stage[24];
    char error[64];
    int chunks;
    int nacks;
    int retries;
    int64_t elapsed_s;
    int64_t finished_ms;
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

static uint8_t  s_fw_stage_fallback[FW_STAGE_BUF_FALLBACK];
static uint8_t *s_fw_stage_buf = NULL;     /* resolved on first use */
static size_t   s_fw_stage_cap = 0;

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

/* Wait for a JSON line whose payload contains `needle` (usually the "type"
 * value). Returns 0 on success, -1 on timeout, -2 if "ota_error" seen (fills
 * reason_out with the "reason":"X" string if present). Other line types are
 * silently consumed and ignored. */
static int relay_wait_for(uart_port_t uart_num, const char *needle,
                         int timeout_ms, char *reason_out, size_t reason_size)
{
    char line[160];
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
                                      int timeout_ms,
                                      int resend_ms,
                                      char *reason_out,
                                      size_t reason_size)
{
    char line[160];
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

/* Non-blocking peek for a NACK line. Returns seq number on NACK hit, -1 if
 * nothing relevant read within timeout_ms. Silently consumes other lines. */
static int relay_poll_nack(uart_port_t uart_num, int timeout_ms)
{
    char line[160];
    int64_t deadline_ms = (esp_timer_get_time() / 1000) + timeout_ms;
    while ((esp_timer_get_time() / 1000) < deadline_ms) {
        int remaining = (int)(deadline_ms - (esp_timer_get_time() / 1000));
        if (remaining <= 0) break;
        int n = relay_read_line(uart_num, line, sizeof(line), remaining);
        if (n < 0) break;
        if (strstr(line, "ota_nack")) return relay_extract_seq(line);
        /* consume other lines and keep polling */
    }
    return -1;
}

/* ── Firmware offer + relay core ───────────────────────────────────────── */

typedef struct {
    bool ok;
    bool legacy;
    uint32_t size;
    int chunks;
    int nacks;
    int retries;
    int64_t elapsed_s;
    char stage[16];
    char error[64];
} fw_relay_result_t;

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
                                       fw_relay_result_t *result)
{
    fw_relay_result_t local = {0};
    if (!result) result = &local;
    memset(result, 0, sizeof(*result));
    snprintf(result->stage, sizeof(result->stage), "init");

    if (s_operation_active) {
        snprintf(result->error, sizeof(result->error), "operation_active");
        return false;
    }

    fw_store_info_t info = {0};
    if (!fw_store_get_info(&info)) {
        snprintf(result->error, sizeof(result->error), "no_firmware_stored");
        return false;
    }

    const esp_partition_t *p = find_fw_partition();
    if (!p) {
        snprintf(result->error, sizeof(result->error), "partition_not_found");
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

    http_upload_pause();
    vTaskDelay(pdMS_TO_TICKS(500));
    uart_rx_pause_scanner(scanner_id);

    bool relay_ok = true;
    char error_msg[64] = {0};
    char stage[16] = "init";
    int64_t start_ms = esp_timer_get_time() / 1000;
    int64_t overall_timeout_ms = 10 * 60 * 1000;
    uint16_t seq = 0;
    int nack_count = 0;
    int total_retries = 0;

    snprintf(stage, sizeof(stage), "stop");
    {
        if (!scanner_already_quiet) {
            int r = relay_wait_for_with_resend(
                scanner_id,
                uart_num,
                "{\"type\":\"stop\"}",
                "stop_ack",
                FW_RELAY_STOP_STORM_MS,
                FW_RELAY_STOP_STORM_STEP_MS,
                NULL,
                0
            );
            if (r == 0) {
                ESP_LOGI(TAG, "Stage stop: stop_ack received");
            } else if (legacy_mode) {
                ESP_LOGW(TAG, "Stage stop: stop_ack missing after stop storm, continuing in legacy mode");
            } else {
                snprintf(error_msg, sizeof(error_msg), "stop_ack_timeout");
                relay_ok = false;
                goto relay_done;
            }
        } else {
            ESP_LOGW(TAG, "Stage stop: scanner pre-quiet via fw_ready");
        }
        char drain[160];
        while (relay_read_line(uart_num, drain, sizeof(drain), 300) >= 0) {}
    }

    snprintf(stage, sizeof(stage), "begin");
    {
        uint32_t fw_crc32 = 0;
        bool has_crc = nvs_config_get_u32(NVS_FW_CRC32, &fw_crc32);
        char cmd[96];
        if (has_crc) {
            snprintf(cmd, sizeof(cmd), "{\"type\":\"ota_begin\",\"size\":%lu,\"crc\":%lu}",
                     (unsigned long)info.size, (unsigned long)fw_crc32);
        } else {
            snprintf(cmd, sizeof(cmd), "{\"type\":\"ota_begin\",\"size\":%lu}",
                     (unsigned long)info.size);
        }
        uart_rx_send_command_to_scanner(scanner_id, cmd);
        ESP_LOGI(TAG, "Stage begin: sent ota_begin (%lu bytes, CRC=%s%08lX)",
                 (unsigned long)info.size, has_crc ? "" : "none/", (unsigned long)fw_crc32);

        if (legacy_mode) {
            ESP_LOGW(TAG, "Stage begin: skipping ota_ack wait in legacy mode");
            vTaskDelay(pdMS_TO_TICKS(2500));
        } else {
            char reason[48] = {0};
            int r = relay_wait_for(uart_num, "ota_ack", 3000, reason, sizeof(reason));
            if (r != 0) {
                snprintf(error_msg, sizeof(error_msg),
                         r == -2 ? "scanner_error:%s" : "ota_ack_timeout",
                         r == -2 ? reason : "");
                relay_ok = false;
                goto relay_done;
            }
            ESP_LOGI(TAG, "Stage begin: ota_ack received, entering chunk stream");
        }
    }

    snprintf(stage, sizeof(stage), "chunks");
    {
        static uint8_t read_buf[OTA_CHUNK_MAX_DATA];
        static uint8_t frame[OTA_CHUNK_HEADER_SIZE + OTA_CHUNK_MAX_DATA + OTA_CHUNK_CRC_SIZE];
        uint32_t offset = 0;
        uint32_t remaining = info.size;
        uint8_t retry_for_seq[8] = {0};

        while (remaining > 0) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            if ((now_ms - start_ms) > overall_timeout_ms) {
                snprintf(error_msg, sizeof(error_msg), "overall_timeout");
                relay_ok = false;
                goto relay_done;
            }

            uint32_t chunk_len = remaining > OTA_CHUNK_MAX_DATA ? OTA_CHUNK_MAX_DATA : remaining;
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

            int nack_seq = relay_poll_nack(uart_num, 50);
            if (nack_seq >= 0) {
                if (nack_seq > (int)seq) {
                    ESP_LOGW(TAG, "NACK seq=%d > current seq=%d, ignoring", nack_seq, seq);
                } else {
                    int behind = (int)seq - nack_seq;
                    uint32_t rewind_bytes = (uint32_t)behind * OTA_CHUNK_MAX_DATA;
                    if (rewind_bytes > offset) rewind_bytes = offset;
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
                    remaining += rewind_bytes;
                    seq = (uint16_t)nack_seq;
                    ESP_LOGW(TAG, "NACK seq=%d -> rewind %lu bytes (retry %u)",
                             nack_seq, (unsigned long)rewind_bytes,
                             retry_for_seq[slot]);
                    continue;
                }
            }

            offset += chunk_len;
            remaining -= chunk_len;
            seq++;

            if ((info.size - remaining) % (100 * 1024) < OTA_CHUNK_MAX_DATA) {
                ESP_LOGI(TAG, "Relay: %lu/%lu (%.0f%%) seq=%d nacks=%d heap=%lu",
                         (unsigned long)(info.size - remaining), (unsigned long)info.size,
                         (float)(info.size - remaining) / info.size * 100,
                         seq, nack_count,
                         (unsigned long)esp_get_free_heap_size());
            }
        }
    }

    snprintf(stage, sizeof(stage), "end");
    {
        uart_rx_send_command_to_scanner(scanner_id, "{\"type\":\"ota_end\"}");

        if (legacy_mode) {
            char line[192];
            int64_t deadline = (esp_timer_get_time() / 1000) + 60000;
            bool verified = false;
            bool saw_identity = false;
            ESP_LOGW(TAG, "Stage end: legacy relay sent ota_end, waiting for reboot/version proof");
            while ((esp_timer_get_time() / 1000) < deadline) {
                int remaining_ms = (int)(deadline - (esp_timer_get_time() / 1000));
                if (remaining_ms <= 0) break;
                int n = relay_read_line(uart_num, line, sizeof(line), remaining_ms);
                if (n < 0) break;
                if (strstr(line, "ota_done")) {
                    verified = true;
                    ESP_LOGW(TAG, "Stage end: legacy scanner emitted ota_done");
                    break;
                }
                if (relay_line_is_scanner_identity(line)) {
                    saw_identity = true;
                    if (relay_line_matches_staged_version(line, &info)) {
                        verified = true;
                        ESP_LOGW(TAG, "Stage end: legacy scanner identity matches staged version");
                        break;
                    }
                    ESP_LOGW(TAG, "Stage end: scanner identity did not match staged version yet");
                }
            }
            if (!verified) {
                snprintf(error_msg, sizeof(error_msg), "%s",
                         saw_identity ? "legacy_version_not_updated" : "legacy_verify_timeout");
                relay_ok = false;
                goto relay_done;
            }
            goto relay_done;
        }

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
    uart_rx_resume_scanner(scanner_id);

    int64_t elapsed_s = ((esp_timer_get_time() / 1000) - start_ms) / 1000;
    if (!relay_ok) {
        uart_rx_send_command_to_scanner(scanner_id, "{\"type\":\"start\"}");
    }

    if (relay_ok) {
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
    result->chunks = seq;
    result->nacks = nack_count;
    result->retries = total_retries;
    result->elapsed_s = elapsed_s;
    strncpy(result->stage, stage, sizeof(result->stage) - 1);
    result->stage[sizeof(result->stage) - 1] = '\0';
    strncpy(result->error, error_msg, sizeof(result->error) - 1);
    result->error[sizeof(result->error) - 1] = '\0';
    if (scanner_id >= 0 && scanner_id < 2) {
        s_last_relay[scanner_id].ok = result->ok;
        strncpy(s_last_relay[scanner_id].stage, result->stage, sizeof(s_last_relay[scanner_id].stage) - 1);
        s_last_relay[scanner_id].stage[sizeof(s_last_relay[scanner_id].stage) - 1] = '\0';
        strncpy(s_last_relay[scanner_id].error, result->error, sizeof(s_last_relay[scanner_id].error) - 1);
        s_last_relay[scanner_id].error[sizeof(s_last_relay[scanner_id].error) - 1] = '\0';
        s_last_relay[scanner_id].chunks = result->chunks;
        s_last_relay[scanner_id].nacks = result->nacks;
        s_last_relay[scanner_id].retries = result->retries;
        s_last_relay[scanner_id].elapsed_s = result->elapsed_s;
        s_last_relay[scanner_id].finished_ms = esp_timer_get_time() / 1000;
    }
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
    httpd_query_key_value(query, "mode", mode, sizeof(mode));
    httpd_query_key_value(query, "legacy", legacy_param, sizeof(legacy_param));
    bool legacy_mode =
        strcmp(mode, "legacy") == 0 ||
        strcmp(legacy_param, "1") == 0 ||
        strcmp(legacy_param, "true") == 0;
    int scanner_id = (strcmp(uart_target, "wifi") == 0) ? 1 : 0;

    fw_relay_result_t result = {0};
    fw_relay_stored_to_scanner(scanner_id, false, legacy_mode, &result);

    char resp_buf[320];
    snprintf(resp_buf, sizeof(resp_buf),
             "{\"ok\":%s,\"legacy\":%s,\"size\":%lu,\"chunks\":%d,\"nacks\":%d,"
             "\"retries\":%d,\"elapsed_s\":%lld,\"stage\":\"%s\","
             "\"error\":\"%s\"}",
             result.ok ? "true" : "false",
             result.legacy ? "true" : "false",
             (unsigned long)result.size, result.chunks, result.nacks, result.retries,
             (long long)result.elapsed_s, result.stage, result.error);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp_buf);
    return ESP_OK;
}

#define FW_AUTO_RELAY_COOLDOWN_MS (10 * 60 * 1000)

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
    fw_relay_stored_to_scanner(scanner_id, true, false, &result);
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
    char resp[768];
    if (has_fw) {
        snprintf(resp, sizeof(resp),
                 "{\"stored\":true,\"size\":%lu,\"checksum\":%lu,"
                 "\"version\":\"%s\",\"name\":\"%s\",\"partition\":\"%s\","
                 "\"auto_update_enabled\":true,\"auto_relay_cooldown_s\":%d,"
                 "\"last_relay\":{\"ble\":{\"ok\":%s,\"stage\":\"%s\",\"error\":\"%s\","
                 "\"chunks\":%d,\"nacks\":%d,\"retries\":%d,\"elapsed_s\":%lld,\"age_s\":%lld},"
                 "\"wifi\":{\"ok\":%s,\"stage\":\"%s\",\"error\":\"%s\","
                 "\"chunks\":%d,\"nacks\":%d,\"retries\":%d,\"elapsed_s\":%lld,\"age_s\":%lld}}}",
                 (unsigned long)size, (unsigned long)cksum,
                 ver[0] ? ver : "", name[0] ? name : "", part[0] ? part : "",
                 (int)(FW_AUTO_RELAY_COOLDOWN_MS / 1000),
                 s_last_relay[0].ok ? "true" : "false",
                 s_last_relay[0].stage, s_last_relay[0].error,
                 s_last_relay[0].chunks, s_last_relay[0].nacks, s_last_relay[0].retries,
                 (long long)s_last_relay[0].elapsed_s,
                 (long long)(s_last_relay[0].finished_ms > 0 ? (now_ms - s_last_relay[0].finished_ms) / 1000 : -1),
                 s_last_relay[1].ok ? "true" : "false",
                 s_last_relay[1].stage, s_last_relay[1].error,
                 s_last_relay[1].chunks, s_last_relay[1].nacks, s_last_relay[1].retries,
                 (long long)s_last_relay[1].elapsed_s,
                 (long long)(s_last_relay[1].finished_ms > 0 ? (now_ms - s_last_relay[1].finished_ms) / 1000 : -1));
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
