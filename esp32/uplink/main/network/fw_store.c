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

#include <string.h>
#include <stdio.h>
#include <sys/socket.h>

static const char *TAG = "fw_store";

/* ── State ───────────────────────────────────────────────────────────────── */

static volatile bool s_operation_active = false;  /* Prevents concurrent upload/relay */

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
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        resume_all_tasks();
        s_operation_active = false;
        return ESP_FAIL;
    }

    static uint8_t buf[4096];
    int received = 0;
    uint32_t checksum = 0;
    int consecutive_timeouts = 0;

    while (received < total) {
        int to_read = total - received;
        if (to_read > (int)sizeof(buf)) to_read = sizeof(buf);

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

        if (received % (100 * 1024) < (int)sizeof(buf)) {
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

/* ── POST /api/fw/relay — reliable relay with CRC32 + ACK ───────────────── */

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

    /* Find the partition by label */
    const esp_partition_t *p = find_fw_partition();
    if (!p) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Partition not found");
        return ESP_FAIL;
    }

    /* Parse ?uart=ble|wifi&ack=0|1 */
    char query[64] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char uart_target[8] = "ble";
    char ack_str[4] = "0";  /* Default to ack=0 (unidirectional, image CRC verifies) */
    httpd_query_key_value(query, "uart", uart_target, sizeof(uart_target));
    httpd_query_key_value(query, "ack", ack_str, sizeof(ack_str));
    bool use_ack = (ack_str[0] == '1');

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

    s_operation_active = true;

    ESP_LOGW(TAG, "Relay: %lu bytes from '%s' to UART%d (uart=%s, ack=%s) heap=%lu",
             (unsigned long)fw_size, p->label, uart_num, uart_target,
             use_ack ? "yes" : "no", (unsigned long)esp_get_free_heap_size());

    /* Pause HTTP upload task to free heap + prevent concurrent WiFi ops */
    http_upload_pause();
    vTaskDelay(pdMS_TO_TICKS(500));  /* Let upload task release resources */
    ESP_LOGI(TAG, "HTTP uploads paused, heap=%lu", (unsigned long)esp_get_free_heap_size());

    /* Step 0: Stop scanner TX before OTA */
    {
        const char *stop_cmd = "{\"type\":\"stop\"}\n";
        uart_write_bytes(uart_num, stop_cmd, strlen(stop_cmd));
        ESP_LOGI(TAG, "Sent stop command to scanner");
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* Step 1: Send ota_begin with image CRC32 */
    uart_rx_clear_ota_response();
    uart_write_bytes(uart_num, "\n", 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Read CRC32 from NVS (stored during firmware upload) */
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
    ESP_LOGI(TAG, "Sent ota_begin (%lu bytes, CRC=%s%08lX)",
             (unsigned long)fw_size, has_crc ? "" : "none/", (unsigned long)fw_crc32);

    /* Pause UART RX task so we can read ACKs directly */
    int scanner_id = (strcmp(uart_target, "wifi") == 0) ? 1 : 0;
    uart_rx_pause_scanner(scanner_id);
    vTaskDelay(pdMS_TO_TICKS(2000));  /* Let scanner prepare OTA partition */

    /* Flush stale UART data */
    {
        uint8_t flush[256];
        while (uart_read_bytes(uart_num, flush, sizeof(flush), pdMS_TO_TICKS(200)) > 0) {}
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Step 2: Send chunks with CRC32 */
    static uint8_t read_buf[OTA_CHUNK_MAX_DATA];
    static uint8_t frame[OTA_CHUNK_HEADER_SIZE + OTA_CHUNK_MAX_DATA + OTA_CHUNK_CRC_SIZE];
    uint32_t offset = 0;  /* Firmware starts at partition offset 0 (no header) */
    uint32_t remaining = fw_size;
    uint16_t seq = 0;
    int nack_count = 0;
    int total_retries = 0;
    bool relay_ok = true;
    char error_msg[64] = {0};
    int64_t start_ms = esp_timer_get_time() / 1000;
    int64_t timeout_ms = 5 * 60 * 1000;  /* 5-minute overall timeout */

    char ack_line[128];
    int ack_pos = 0;

    while (remaining > 0) {
        /* Overall timeout check */
        int64_t now_ms = esp_timer_get_time() / 1000;
        if ((now_ms - start_ms) > timeout_ms) {
            snprintf(error_msg, sizeof(error_msg), "timeout");
            relay_ok = false;
            break;
        }

        uint32_t chunk_len = remaining > OTA_CHUNK_MAX_DATA ? OTA_CHUNK_MAX_DATA : remaining;

        esp_err_t err = esp_partition_read(p, offset, read_buf, chunk_len);
        if (err != ESP_OK) {
            snprintf(error_msg, sizeof(error_msg), "flash read at %lu", (unsigned long)offset);
            relay_ok = false;
            break;
        }

        /* Build frame: [magic][seq(2)][len(2)] + data + [CRC32(4)] */
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

        /* Send chunk */
        uart_write_bytes(uart_num, (char *)frame, frame_len);
        uart_wait_tx_done(uart_num, pdMS_TO_TICKS(1000));

        if (use_ack) {
            /* Wait for ACK with exponential backoff retry */
            int retries = 0;
            bool acked = false;

            while (retries < 5 && !acked) {
                if (retries > 0) {
                    uart_write_bytes(uart_num, (char *)frame, frame_len);
                    uart_wait_tx_done(uart_num, pdMS_TO_TICKS(1000));
                }

                int wait_ms = 0;
                ack_pos = 0;
                while (wait_ms < 1000) {
                    uint8_t rb;
                    int n = uart_read_bytes(uart_num, &rb, 1, pdMS_TO_TICKS(10));
                    wait_ms += 10;
                    if (n <= 0) continue;

                    if (rb == '\n') {
                        if (ack_pos > 0) {
                            ack_line[ack_pos] = '\0';
                            if (strstr(ack_line, "ota_ok")) {
                                acked = true; break;
                            } else if (strstr(ack_line, "ota_nack")) {
                                nack_count++; total_retries++; break;
                            } else if (strstr(ack_line, "ota_error")) {
                                snprintf(error_msg, sizeof(error_msg), "scanner_error");
                                relay_ok = false; remaining = 0; goto relay_done;
                            } else if (strstr(ack_line, "ota_done")) {
                                acked = true; remaining = 0; break;
                            }
                            ack_pos = 0;
                        }
                    } else if (ack_pos < (int)sizeof(ack_line) - 1) {
                        ack_line[ack_pos++] = (char)rb;
                    }
                }

                if (!acked) {
                    retries++;
                    int backoff_ms = 200 * (1 << (retries - 1));
                    if (backoff_ms > 3200) backoff_ms = 3200;
                    ESP_LOGW(TAG, "Chunk %d: no ACK, retry %d in %dms", seq, retries, backoff_ms);
                    vTaskDelay(pdMS_TO_TICKS(backoff_ms));
                }
            }

            if (!acked && remaining > 0) {
                snprintf(error_msg, sizeof(error_msg), "chunk %d failed", seq);
                relay_ok = false;
                break;
            }
        } else {
            /* Fire-and-forget mode with pacing */
            vTaskDelay(pdMS_TO_TICKS(30));
            if (seq % 16 == 0) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }

        offset += chunk_len;
        remaining -= chunk_len;
        seq++;

        if ((fw_size - remaining) % (100 * 1024) < OTA_CHUNK_MAX_DATA) {
            ESP_LOGI(TAG, "Relay: %lu/%lu (%.0f%%) seq=%d nacks=%d heap=%lu",
                     (unsigned long)(fw_size - remaining), (unsigned long)fw_size,
                     (float)(fw_size - remaining) / fw_size * 100, seq, nack_count,
                     (unsigned long)esp_get_free_heap_size());
        }
    }

relay_done:;
    /* Resume everything */
    uart_rx_resume_scanner(scanner_id);

    int64_t elapsed_s = ((esp_timer_get_time() / 1000) - start_ms) / 1000;

    /* Only send start if relay failed — on success the scanner is rebooting
     * into new firmware and won't process this command. The watchdog's periodic
     * ready signal will bring the scanner back after reboot. */
    if (!relay_ok) {
        const char *start_cmd = "{\"type\":\"start\"}\n";
        uart_write_bytes(uart_num, start_cmd, strlen(start_cmd));
    }

    if (relay_ok) {
        ESP_LOGW(TAG, "Relay complete: %lu bytes, %d chunks, %d nacks in %llds",
                 (unsigned long)fw_size, seq, nack_count, (long long)elapsed_s);
        vTaskDelay(pdMS_TO_TICKS(3000));  /* Wait for scanner to finalize */
    } else {
        ESP_LOGE(TAG, "Relay FAILED: %s (%d chunks in %llds)",
                 error_msg, seq, (long long)elapsed_s);
    }

    /* Resume HTTP uploads */
    http_upload_resume();
    s_operation_active = false;

    char resp_buf[256];
    snprintf(resp_buf, sizeof(resp_buf),
             "{\"ok\":%s,\"size\":%lu,\"chunks\":%d,\"nacks\":%d,"
             "\"retries\":%d,\"elapsed_s\":%lld,\"error\":\"%s\"}",
             relay_ok ? "true" : "false",
             (unsigned long)fw_size, seq, nack_count, total_retries,
             (long long)elapsed_s, error_msg);
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
