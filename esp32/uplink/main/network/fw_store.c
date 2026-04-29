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
#include <sys/socket.h>

static const char *TAG = "fw_store";

/* ── State ───────────────────────────────────────────────────────────────── */

static volatile bool s_operation_active = false;  /* Prevents concurrent upload/relay */

/* ── OTA upload staging buffer (P4) ──────────────────────────────────────
 * Lazily allocated on first upload. 64 KB on PSRAM collapses the
 * recv/esp_ota_write loop from ~280 iterations per 1.1 MB scanner firmware
 * down to ~18, visibly cutting upload latency. Falls back to a 4 KB static
 * on boards without PSRAM — matches the heap-stability baseline. */
#define FW_STAGE_BUF_PSRAM   (64 * 1024)
#define FW_STAGE_BUF_FALLBACK 4096

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
    char version[16] = {0};
    char query[64] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "version", version, sizeof(version));

    nvs_config_set_u32(NVS_FW_SIZE, (uint32_t)total);
    nvs_config_set_u32(NVS_FW_CKSUM, checksum);
    nvs_config_set_u32(NVS_FW_CRC32, checksum);  /* Now CRC32, same key stores it */
    nvs_config_set_string(NVS_FW_VER, version[0] ? version : "?");
    nvs_config_set_string(NVS_FW_PART, p->label);

    ESP_LOGW(TAG, "Firmware stored: %d bytes, CRC32=%08lX, partition=%s, version=%s",
             total, (unsigned long)checksum, p->label, version[0] ? version : "?");

    resume_all_tasks();
    s_operation_active = false;

    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"size\":%d,\"checksum\":%lu,\"partition\":\"%s\"}",
             total, (unsigned long)checksum, p->label);
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

/* ── POST /api/fw/relay — staged handshake + fire-and-forget with NACK ──── */

static esp_err_t fw_relay_handler(httpd_req_t *req)
{
    if (s_operation_active) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Operation in progress");
        return ESP_FAIL;
    }

    /* Read firmware metadata from NVS */
    uint32_t fw_size = 0, fw_cksum = 0;
    char fw_ver[16] = {0}, fw_part_label[16] = {0};
    if (!read_fw_metadata(&fw_size, &fw_cksum, fw_ver, sizeof(fw_ver),
                          fw_part_label, sizeof(fw_part_label))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No firmware stored");
        return ESP_FAIL;
    }

    const esp_partition_t *p = find_fw_partition();
    if (!p) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Partition not found");
        return ESP_FAIL;
    }

    /* Parse ?uart=ble|wifi. */
    char query[64] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char uart_target[8] = "ble";
    httpd_query_key_value(query, "uart", uart_target, sizeof(uart_target));

    uart_port_t uart_num;
    if (strcmp(uart_target, "wifi") == 0) {
        uart_num = CONFIG_WIFI_SCANNER_UART;
    } else {
        uart_num = CONFIG_BLE_SCANNER_UART;
    }

    s_operation_active = true;

    ESP_LOGW(TAG, "Relay v2: %lu bytes from '%s' to UART%d (uart=%s) heap=%lu",
             (unsigned long)fw_size, p->label, uart_num, uart_target,
             (unsigned long)esp_get_free_heap_size());

    /* Pause HTTP uploads to free heap + prevent concurrent WiFi ops */
    http_upload_pause();
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Take exclusive control of the scanner UART: pause the RX task so we can
     * read handshake JSON directly from the peripheral. The task's buffered
     * bytes are left alone (not flushed) — we'll consume them as "other"
     * lines during relay_wait_for (which ignores non-matching lines). */
    int scanner_id = (strcmp(uart_target, "wifi") == 0) ? 1 : 0;
    uart_rx_pause_scanner(scanner_id);

    bool relay_ok = true;
    char error_msg[64] = {0};
    char stage[16] = "init";
    int64_t start_ms = esp_timer_get_time() / 1000;
    int64_t overall_timeout_ms = 10 * 60 * 1000;  /* 10-minute hard ceiling */
    uint16_t seq = 0;
    int nack_count = 0;
    int total_retries = 0;

    /* ── Stage 0: Stop scanner TX and wait for ack ─────────────────────── */
    snprintf(stage, sizeof(stage), "stop");
    {
        const char *stop_cmd = "{\"type\":\"stop\"}\n";
        uart_write_bytes(uart_num, stop_cmd, strlen(stop_cmd));
        int r = relay_wait_for(uart_num, "stop_ack", 2000, NULL, 0);
        if (r == 0) {
            ESP_LOGI(TAG, "Stage stop: stop_ack received");
        } else {
            snprintf(error_msg, sizeof(error_msg), "stop_ack_timeout");
            relay_ok = false;
            goto relay_done;
        }
        /* Drain any remaining detection JSON sitting in the FIFO before we send
         * ota_begin. 500ms quiet window = good enough signal the scanner's TX
         * loop is actually halted. */
        char drain[160];
        while (relay_read_line(uart_num, drain, sizeof(drain), 300) >= 0) {}
    }

    /* ── Stage 1: ota_begin + wait for ota_ack ─────────────────────────── */
    snprintf(stage, sizeof(stage), "begin");
    {
        uint32_t fw_crc32 = 0;
        bool has_crc = nvs_config_get_u32(NVS_FW_CRC32, &fw_crc32);
        char cmd[96];
        if (has_crc) {
            snprintf(cmd, sizeof(cmd), "{\"type\":\"ota_begin\",\"size\":%lu,\"crc\":%lu}\n",
                     (unsigned long)fw_size, (unsigned long)fw_crc32);
        } else {
            snprintf(cmd, sizeof(cmd), "{\"type\":\"ota_begin\",\"size\":%lu}\n",
                     (unsigned long)fw_size);
        }
        uart_write_bytes(uart_num, cmd, strlen(cmd));
        ESP_LOGI(TAG, "Stage begin: sent ota_begin (%lu bytes, CRC=%s%08lX)",
                 (unsigned long)fw_size, has_crc ? "" : "none/", (unsigned long)fw_crc32);

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

    /* ── Stage 2: Stream chunks; poll for NACK per-chunk; retransmit ───── */
    snprintf(stage, sizeof(stage), "chunks");
    {
        static uint8_t read_buf[OTA_CHUNK_MAX_DATA];
        static uint8_t frame[OTA_CHUNK_HEADER_SIZE + OTA_CHUNK_MAX_DATA + OTA_CHUNK_CRC_SIZE];
        uint32_t offset = 0;
        uint32_t remaining = fw_size;
        uint8_t  retry_for_seq[8] = {0};   /* small rolling window of retries per seq */

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

            /* Pacing: 50ms between chunks is the sweet spot — gives the
             * scanner time to write the previous chunk to flash and, more
             * importantly, time to emit a NACK if the just-sent chunk's CRC
             * failed. Brief window is enough — scanner replies within ~20 ms. */
            int nack_seq = relay_poll_nack(uart_num, 50);
            if (nack_seq >= 0) {
                /* Scanner reported a CRC failure for chunk `nack_seq`.
                 * Rewind to that seq and retransmit. In fire-and-forget mode,
                 * the NACK seq is always the current or a recent one, so
                 * rewinding by (seq - nack_seq) chunks is safe. */
                if (nack_seq > (int)seq) {
                    ESP_LOGW(TAG, "NACK seq=%d > current seq=%d — ignoring (stale)",
                             nack_seq, seq);
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
                    ESP_LOGW(TAG, "NACK seq=%d → rewind %lu bytes (retry %u)",
                             nack_seq, (unsigned long)rewind_bytes,
                             retry_for_seq[slot]);
                    continue;
                }
            }

            offset += chunk_len;
            remaining -= chunk_len;
            seq++;

            if ((fw_size - remaining) % (100 * 1024) < OTA_CHUNK_MAX_DATA) {
                ESP_LOGI(TAG, "Relay: %lu/%lu (%.0f%%) seq=%d nacks=%d heap=%lu",
                         (unsigned long)(fw_size - remaining), (unsigned long)fw_size,
                         (float)(fw_size - remaining) / fw_size * 100,
                         seq, nack_count,
                         (unsigned long)esp_get_free_heap_size());
            }
        }
    }

    /* ── Stage 3: ota_end + wait for ota_done, ota_error, OR scanner reboot ─ */
    /*
     * Supported S3 scanners emit {"type":"ota_done"} then reboot. As an
     * orthogonal signal, we also watch for scanner_info/identity lines from
     * modern S3 scanner firmware after reboot.
     */
    snprintf(stage, sizeof(stage), "end");
    {
        const char *end_cmd = "{\"type\":\"ota_end\"}\n";
        uart_write_bytes(uart_num, end_cmd, strlen(end_cmd));

        char line[192];
        int64_t deadline = (esp_timer_get_time() / 1000) + 60000;
        bool saw_done_or_boot = false;
        int result = -1;   /* -1 timeout, 0 done/boot, -2 scanner_error */
        char reason[48] = {0};
        while ((esp_timer_get_time() / 1000) < deadline) {
            int remaining = (int)(deadline - (esp_timer_get_time() / 1000));
            if (remaining <= 0) break;
            int n = relay_read_line(uart_num, line, sizeof(line), remaining);
            if (n < 0) break;
            if (strstr(line, "ota_done")) { result = 0; saw_done_or_boot = true; break; }
            if (strstr(line, "scanner_info") ||
                strstr(line, "\"scanner-s3-combo\"") ||
                strstr(line, "\"scanner-s3-combo-fof_badge\"") ||
                strstr(line, "\"scanner-s3-combo-seed\"")) {
                /* Scanner rebooted into new image and is announcing itself. */
                result = 0; saw_done_or_boot = true;
                ESP_LOGW(TAG, "Stage end: scanner identity after reboot → implicit ota_done");
                break;
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
                result = -2; break;
            }
            /* Ignore other lines — could be late NACKs, leaked detections, or watchdog noise. */
        }

        if (result == -2) {
            snprintf(error_msg, sizeof(error_msg), "scanner_error:%s",
                     reason[0] ? reason : "unknown");
            relay_ok = false;
            goto relay_done;
        } else if (!saw_done_or_boot) {
            snprintf(error_msg, sizeof(error_msg), "finalize_timeout");
            relay_ok = false;
            goto relay_done;
        }
        ESP_LOGW(TAG, "Stage end: scanner acknowledged — rebooting");
    }

relay_done:;
    /* Resume UART RX task + HTTP uploads */
    uart_rx_resume_scanner(scanner_id);

    int64_t elapsed_s = ((esp_timer_get_time() / 1000) - start_ms) / 1000;

    /* On failure, wake the scanner's TX loop back up so detections resume.
     * On success the scanner is rebooting into the new image and will come
     * back online on its own; the watchdog's periodic ready signal picks up. */
    if (!relay_ok) {
        uart_write_bytes(uart_num, "{\"type\":\"start\"}\n", 17);
    }

    if (relay_ok) {
        ESP_LOGW(TAG, "Relay complete: %lu bytes, %d chunks, %d nacks in %llds",
                 (unsigned long)fw_size, seq, nack_count, (long long)elapsed_s);
    } else {
        ESP_LOGE(TAG, "Relay FAILED @ %s: %s (%d chunks in %llds)",
                 stage, error_msg, seq, (long long)elapsed_s);
    }

    http_upload_resume();
    s_operation_active = false;

    char resp_buf[320];
    snprintf(resp_buf, sizeof(resp_buf),
             "{\"ok\":%s,\"size\":%lu,\"chunks\":%d,\"nacks\":%d,"
             "\"retries\":%d,\"elapsed_s\":%lld,\"stage\":\"%s\","
             "\"error\":\"%s\"}",
             relay_ok ? "true" : "false",
             (unsigned long)fw_size, seq, nack_count, total_retries,
             (long long)elapsed_s, stage, error_msg);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp_buf);
    return ESP_OK;
}

/* ── GET /api/fw/info — check stored firmware status ─────────────────────── */

static esp_err_t fw_info_handler(httpd_req_t *req)
{
    uint32_t size = 0, cksum = 0;
    char ver[16] = {0}, part[16] = {0};
    bool has_fw = read_fw_metadata(&size, &cksum, ver, sizeof(ver), part, sizeof(part));

    char resp[256];
    if (has_fw) {
        snprintf(resp, sizeof(resp),
                 "{\"stored\":true,\"size\":%lu,\"checksum\":%lu,"
                 "\"version\":\"%s\",\"partition\":\"%s\"}",
                 (unsigned long)size, (unsigned long)cksum,
                 ver[0] ? ver : "", part[0] ? part : "");
    } else {
        snprintf(resp, sizeof(resp), "{\"stored\":false}");
    }

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

void fw_store_register(httpd_handle_t server)
{
    esp_err_t r;
    r = httpd_register_uri_handler(server, &uri_fw_upload);
    if (r != ESP_OK) ESP_LOGE(TAG, "Failed /api/fw/upload: %s", esp_err_to_name(r));
    r = httpd_register_uri_handler(server, &uri_fw_relay);
    if (r != ESP_OK) ESP_LOGE(TAG, "Failed /api/fw/relay: %s", esp_err_to_name(r));
    r = httpd_register_uri_handler(server, &uri_fw_info);
    if (r != ESP_OK) ESP_LOGE(TAG, "Failed /api/fw/info: %s", esp_err_to_name(r));

    ESP_LOGI(TAG, "Firmware store endpoints registered");
}
