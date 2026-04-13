/**
 * Friend or Foe — Scanner Firmware Store + Reliable UART Relay
 *
 * Store-then-forward OTA with per-chunk CRC32 and ACK/NACK retransmission.
 * Firmware is uploaded to a dedicated flash partition, then relayed to
 * the scanner UART with guaranteed data integrity.
 */

#include "fw_store.h"
#include "config.h"
#include "uart_protocol.h"
#include "uart_rx.h"

#include "esp_partition.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "fw_store";

/* ── Stored firmware metadata ────────────────────────────────────────────── */

#define FW_MAGIC        0x464F4646  /* "FOFF" */
#define FW_HEADER_SIZE  32

typedef struct {
    uint32_t magic;
    uint32_t size;          /* firmware size in bytes */
    uint32_t checksum;      /* simple sum of all firmware bytes */
    char     version[16];   /* version string if provided */
    uint32_t reserved;
} fw_header_t;

static const esp_partition_t *s_fw_partition = NULL;
static fw_header_t s_fw_info = {0};

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static const esp_partition_t *get_fw_partition(void)
{
    if (!s_fw_partition) {
        s_fw_partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, 0x40, NULL);
        if (!s_fw_partition) {
            ESP_LOGE(TAG, "fw_store partition not found (type=data, subtype=0x40)");
        }
    }
    return s_fw_partition;
}

static bool read_header(fw_header_t *hdr)
{
    const esp_partition_t *p = get_fw_partition();
    if (!p) return false;
    esp_err_t err = esp_partition_read(p, 0, hdr, sizeof(*hdr));
    return (err == ESP_OK && hdr->magic == FW_MAGIC);
}

/* ── POST /api/fw/upload — store firmware to flash ───────────────────────── */

static esp_err_t fw_upload_handler(httpd_req_t *req)
{
    const esp_partition_t *p = get_fw_partition();
    if (!p) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No fw_store partition");
        return ESP_FAIL;
    }

    int total = req->content_len;
    if (total < 1024 || total > (int)(p->size - FW_HEADER_SIZE)) {
        char msg[80];
        snprintf(msg, sizeof(msg), "Invalid size: %d (max %lu)",
                 total, (unsigned long)(p->size - FW_HEADER_SIZE));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "Storing scanner firmware: %d bytes to partition '%s'",
             total, p->label);

    esp_err_t err = esp_partition_erase_range(p, 0, p->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erase failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erase failed");
        return ESP_FAIL;
    }

    static uint8_t buf[4096];
    int received = 0;
    uint32_t checksum = 0;
    uint32_t write_offset = FW_HEADER_SIZE;

    while (received < total) {
        int to_read = total - received;
        if (to_read > (int)sizeof(buf)) to_read = sizeof(buf);

        int len = httpd_req_recv(req, (char *)buf, to_read);
        if (len <= 0) {
            if (len == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "HTTP recv error at %d/%d", received, total);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }

        err = esp_partition_write(p, write_offset, buf, len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Write failed at offset %lu: %s",
                     (unsigned long)write_offset, esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            return ESP_FAIL;
        }

        for (int i = 0; i < len; i++) checksum += buf[i];

        write_offset += len;
        received += len;

        if (received % (100 * 1024) < (int)sizeof(buf)) {
            ESP_LOGI(TAG, "Upload: %d/%d bytes (%.0f%%)",
                     received, total, (float)received / total * 100);
        }
    }

    fw_header_t hdr = {
        .magic = FW_MAGIC,
        .size = (uint32_t)total,
        .checksum = checksum,
    };

    char query[64] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "version", hdr.version, sizeof(hdr.version));

    err = esp_partition_write(p, 0, &hdr, sizeof(hdr));
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Header write failed");
        return ESP_FAIL;
    }

    s_fw_info = hdr;
    ESP_LOGW(TAG, "Firmware stored: %d bytes, checksum=%lu, version=%s",
             total, (unsigned long)checksum, hdr.version[0] ? hdr.version : "?");

    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"size\":%d,\"checksum\":%lu}",
             total, (unsigned long)checksum);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* ── POST /api/fw/relay — reliable relay with CRC32 + ACK ───────────────── */

static esp_err_t fw_relay_handler(httpd_req_t *req)
{
    const esp_partition_t *p = get_fw_partition();
    if (!p) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No fw_store partition");
        return ESP_FAIL;
    }

    fw_header_t hdr;
    if (!read_header(&hdr)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No firmware stored");
        return ESP_FAIL;
    }

    /* Parse ?uart=ble or ?uart=wifi */
    char query[32] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char uart_target[8] = "ble";
    httpd_query_key_value(query, "uart", uart_target, sizeof(uart_target));

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

    uint32_t fw_size = hdr.size;
    ESP_LOGW(TAG, "Reliable relay: %lu bytes to UART%d (uart=%s)",
             (unsigned long)fw_size, uart_num, uart_target);

    /* Step 0: Stop scanner TX before OTA — prevents data collision during flash */
    {
        const char *stop_cmd = "{\"type\":\"stop\"}\n";
        uart_write_bytes(uart_num, stop_cmd, strlen(stop_cmd));
        ESP_LOGI(TAG, "Sent stop command to scanner before OTA");
        vTaskDelay(pdMS_TO_TICKS(500));  /* Let scanner drain its TX queue */
    }

    /* Step 1: Send ota_begin at normal baud */
    uart_rx_clear_ota_response();
    uart_write_bytes(uart_num, "\n", 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "{\"type\":\"ota_begin\",\"size\":%lu}\n",
             (unsigned long)fw_size);
    uart_write_bytes(uart_num, cmd, strlen(cmd));
    ESP_LOGI(TAG, "Sent ota_begin (%lu bytes)", (unsigned long)fw_size);

    /* Pause UART RX task so we can read ACKs directly */
    int scanner_id = (strcmp(uart_target, "wifi") == 0) ? 1 : 0;
    uart_rx_pause_scanner(scanner_id);
    vTaskDelay(pdMS_TO_TICKS(2000));  /* Let scanner prepare OTA partition */

    /* Flush stale data from UART RX buffer, then settle */
    {
        uint8_t flush[256];
        while (uart_read_bytes(uart_num, flush, sizeof(flush), pdMS_TO_TICKS(200)) > 0) {}
    }
    vTaskDelay(pdMS_TO_TICKS(500));  /* Let scanner fully settle into OTA mode */

    /* Step 2: Send chunks with CRC32, read ACK directly from UART */
    static uint8_t read_buf[OTA_CHUNK_MAX_DATA];
    static uint8_t frame[OTA_CHUNK_HEADER_SIZE + OTA_CHUNK_MAX_DATA + OTA_CHUNK_CRC_SIZE];
    uint32_t offset = FW_HEADER_SIZE;
    uint32_t remaining = fw_size;
    uint16_t seq = 0;
    int nack_count = 0;
    int total_retries = 0;
    int64_t start_ms = esp_timer_get_time() / 1000;

    /* Line buffer for reading scanner JSON responses */
    char ack_line[128];
    int ack_pos = 0;

    while (remaining > 0) {
        uint32_t chunk_len = remaining > OTA_CHUNK_MAX_DATA ? OTA_CHUNK_MAX_DATA : remaining;

        esp_err_t err = esp_partition_read(p, offset, read_buf, chunk_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Flash read failed at %lu", (unsigned long)offset);
            break;
        }

        /* Compute CRC32 over data */
        uint32_t crc = esp_rom_crc32_le(0, read_buf, chunk_len);

        /* Build frame: [header(5)] + [data] + [CRC32(4)] */
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

        /* Send chunk and wait for ACK, retry on NACK */
        int retries = 0;
        bool acked = false;

        while (retries < 5 && !acked) {
            uart_write_bytes(uart_num, (char *)frame, frame_len);
            uart_wait_tx_done(uart_num, pdMS_TO_TICKS(500));

            /* Read ACK/NACK directly from UART (up to 500ms) */
            int wait_ms = 0;
            ack_pos = 0;
            while (wait_ms < 500) {
                uint8_t rb;
                int n = uart_read_bytes(uart_num, &rb, 1, pdMS_TO_TICKS(10));
                wait_ms += 10;
                if (n <= 0) continue;

                if (rb == '\n') {
                    if (ack_pos > 0) {
                        ack_line[ack_pos] = '\0';
                        /* Parse ACK JSON */
                        if (strstr(ack_line, "ota_ok")) {
                            acked = true;
                            break;
                        } else if (strstr(ack_line, "ota_nack")) {
                            ESP_LOGW(TAG, "NACK seq=%d, retry %d", seq, retries);
                            nack_count++;
                            total_retries++;
                            break;
                        } else if (strstr(ack_line, "ota_error")) {
                            ESP_LOGE(TAG, "Scanner error: %s", ack_line);
                            remaining = 0;
                            goto relay_done;
                        } else if (strstr(ack_line, "ota_done")) {
                            ESP_LOGI(TAG, "Scanner reports OTA done!");
                            acked = true;
                            remaining = 0;
                            break;
                        }
                        ack_pos = 0;  /* Unknown line, discard */
                    }
                } else if (ack_pos < (int)sizeof(ack_line) - 1) {
                    ack_line[ack_pos++] = (char)rb;
                }
            }

            if (!acked) {
                retries++;
                vTaskDelay(pdMS_TO_TICKS(100));  /* Pause before retry */
            }
        }

        if (!acked && remaining > 0) {
            ESP_LOGE(TAG, "Chunk %d failed after %d retries", seq, retries);
            break;
        }

        offset += chunk_len;
        remaining -= chunk_len;
        seq++;

        if ((fw_size - remaining) % (100 * 1024) < OTA_CHUNK_MAX_DATA) {
            ESP_LOGI(TAG, "Relay: %lu/%lu (%.0f%%) seq=%d nacks=%d",
                     (unsigned long)(fw_size - remaining), (unsigned long)fw_size,
                     (float)(fw_size - remaining) / fw_size * 100, seq, nack_count);
        }
    }

relay_done:;
    /* Resume UART RX task */
    uart_rx_resume_scanner(scanner_id);

    /* Re-enable scanner TX after OTA (whether success or failure) */
    {
        const char *start_cmd = "{\"type\":\"start\"}\n";
        uart_write_bytes(uart_num, start_cmd, strlen(start_cmd));
        ESP_LOGI(TAG, "Sent start command to scanner after OTA");
    }

    int64_t elapsed_s = ((esp_timer_get_time() / 1000) - start_ms) / 1000;

    /* Wait for scanner to finalize + reboot */
    vTaskDelay(pdMS_TO_TICKS(3000));
    ota_response_t final = {0};

    char resp_buf[256];
    snprintf(resp_buf, sizeof(resp_buf),
             "{\"ok\":true,\"size\":%lu,\"chunks\":%d,\"nacks\":%d,"
             "\"retries\":%d,\"elapsed_s\":%lld,\"scanner\":\"%s\","
             "\"scanner_error\":\"%s\"}",
             (unsigned long)fw_size, seq, nack_count, total_retries,
             (long long)elapsed_s,
             final.type[0] ? final.type : "none",
             final.error[0] ? final.error : "");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp_buf);
    return ESP_OK;
}

/* ── GET /api/fw/info — check stored firmware status ─────────────────────── */

static esp_err_t fw_info_handler(httpd_req_t *req)
{
    fw_header_t hdr;
    bool has_fw = read_header(&hdr);

    char resp[256];
    if (has_fw) {
        snprintf(resp, sizeof(resp),
                 "{\"stored\":true,\"size\":%lu,\"checksum\":%lu,\"version\":\"%s\"}",
                 (unsigned long)hdr.size, (unsigned long)hdr.checksum,
                 hdr.version[0] ? hdr.version : "");
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
