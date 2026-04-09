/**
 * Friend or Foe — Scanner UART OTA Receiver (v2: CRC32 + per-chunk ACK)
 *
 * Protocol:
 *   1. Uplink sends JSON: {"type":"ota_begin","size":N}
 *   2. Scanner ACKs: {"type":"ota_ack"}
 *   3. Uplink sends binary chunks:
 *        [0xF0][seq_hi][seq_lo][len_hi][len_lo] + data + [CRC32 (4 bytes)]
 *   4. Scanner verifies CRC32, writes to flash, sends per-chunk ACK/NACK:
 *        {"type":"ota_ok","seq":N}   or   {"type":"ota_nack","seq":N}
 *   5. On NACK, uplink retransmits the chunk
 *   6. When all bytes received, scanner validates and reboots
 */

#include "uart_ota.h"
#include "uart_protocol.h"
#ifndef BLE_SCANNER_ONLY
#include "wifi_scanner.h"
#endif
#ifndef WIFI_SCANNER_ONLY
#include "ble_remote_id.h"
#endif

#include <string.h>
#include <stdio.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "uart_ota";

/* ── OTA session state ─────────────────────────────────────────────────── */

static struct {
    bool                     active;
    uart_port_t              uart_num;
    esp_ota_handle_t         handle;
    const esp_partition_t   *partition;
    uint32_t                 total_size;
    uint32_t                 received;
    uint16_t                 expected_seq;

    /* Chunk accumulator (handles UART byte fragmentation) */
    uint8_t                  hdr_buf[OTA_CHUNK_HEADER_SIZE];
    uint8_t                  hdr_pos;
    uint8_t                  chunk_buf[OTA_CHUNK_MAX_DATA];
    uint16_t                 chunk_len;      /* expected data length from header */
    uint16_t                 chunk_pos;      /* data bytes received so far */
    uint8_t                  crc_buf[OTA_CHUNK_CRC_SIZE];
    uint8_t                  crc_pos;        /* CRC bytes received */
    uint16_t                 chunk_seq;      /* sequence number of current chunk */
    enum { PHASE_HEADER, PHASE_DATA, PHASE_CRC } phase;

    int64_t                  start_ms;
} s_ota = {0};

/* ── Send JSON response back to uplink ─────────────────────────────────── */

static void send_json(const char *json)
{
    uart_write_bytes(s_ota.uart_num, json, strlen(json));
    uart_write_bytes(s_ota.uart_num, "\n", 1);
}

static void send_error(const char *msg)
{
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"ota_error\",\"error\":\"%s\",\"received\":%lu}",
             msg, (unsigned long)s_ota.received);
    send_json(buf);
}

static void send_chunk_ack(uint16_t seq)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"type\":\"ota_ok\",\"seq\":%u}", seq);
    send_json(buf);
}

static void send_chunk_nack(uint16_t seq)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"type\":\"ota_nack\",\"seq\":%u}", seq);
    send_json(buf);
}

/* ── Public API ────────────────────────────────────────────────────────── */

bool uart_ota_begin(uint32_t total_size, uart_port_t uart_num)
{
    if (s_ota.active) {
        ESP_LOGW(TAG, "OTA already active, aborting previous");
        uart_ota_abort();
    }

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        ESP_LOGE(TAG, "No OTA partition available");
        send_error("no_partition");
        return false;
    }

    esp_ota_handle_t handle;
    esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        send_error("begin_failed");
        return false;
    }

    memset(&s_ota, 0, sizeof(s_ota));
    s_ota.active = true;
    s_ota.start_ms = esp_timer_get_time() / 1000;
    s_ota.uart_num = uart_num;
    s_ota.handle = handle;
    s_ota.partition = update;
    s_ota.total_size = total_size;
    s_ota.phase = PHASE_HEADER;

    ESP_LOGW(TAG, "UART OTA started: %lu bytes → partition '%s'",
             (unsigned long)total_size, update->label);

    /* Pause all scanning to give OTA full CPU + UART bandwidth */
#ifndef BLE_SCANNER_ONLY
    wifi_scanner_pause();
#endif
#ifndef WIFI_SCANNER_ONLY
    ble_remote_id_stop();
#endif

    /* ACK: tell uplink we're ready */
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"type\":\"ota_ack\"}");
    send_json(buf);
    return true;
}

bool uart_ota_process_data(const uint8_t *data, int len)
{
    if (!s_ota.active) return false;

    for (int i = 0; i < len; i++) {
        uint8_t b = data[i];

        switch (s_ota.phase) {

        case PHASE_HEADER:
            /* Accumulating 5-byte header: [0xF0][seq(2)][len(2)] */
            if (s_ota.hdr_pos == 0 && b != OTA_CHUNK_MAGIC) {
                continue;  /* Skip non-magic bytes between chunks */
            }
            s_ota.hdr_buf[s_ota.hdr_pos++] = b;

            if (s_ota.hdr_pos >= OTA_CHUNK_HEADER_SIZE) {
                uint16_t seq  = ((uint16_t)s_ota.hdr_buf[1] << 8) | s_ota.hdr_buf[2];
                uint16_t clen = ((uint16_t)s_ota.hdr_buf[3] << 8) | s_ota.hdr_buf[4];

                if (clen > OTA_CHUNK_MAX_DATA) {
                    ESP_LOGE(TAG, "Chunk too large: %d (seq %d)", clen, seq);
                    s_ota.hdr_pos = 0;
                    continue;
                }

                s_ota.chunk_seq = seq;
                s_ota.chunk_len = clen;
                s_ota.chunk_pos = 0;
                s_ota.crc_pos = 0;
                s_ota.hdr_pos = 0;
                s_ota.phase = PHASE_DATA;
            }
            break;

        case PHASE_DATA:
            s_ota.chunk_buf[s_ota.chunk_pos++] = b;
            if (s_ota.chunk_pos >= s_ota.chunk_len) {
                s_ota.phase = PHASE_CRC;
            }
            break;

        case PHASE_CRC:
            s_ota.crc_buf[s_ota.crc_pos++] = b;
            if (s_ota.crc_pos >= OTA_CHUNK_CRC_SIZE) {
                /* CRC32 complete — verify data integrity */
                uint32_t expected_crc =
                    ((uint32_t)s_ota.crc_buf[0] << 24) |
                    ((uint32_t)s_ota.crc_buf[1] << 16) |
                    ((uint32_t)s_ota.crc_buf[2] <<  8) |
                    ((uint32_t)s_ota.crc_buf[3]);
                uint32_t actual_crc = esp_rom_crc32_le(0,
                    s_ota.chunk_buf, s_ota.chunk_len);

                if (actual_crc != expected_crc) {
                    ESP_LOGW(TAG, "CRC FAIL seq=%d (exp=%08lX got=%08lX)",
                             s_ota.chunk_seq,
                             (unsigned long)expected_crc,
                             (unsigned long)actual_crc);
                    send_chunk_nack(s_ota.chunk_seq);
                    s_ota.phase = PHASE_HEADER;
                    break;
                }

                /* CRC OK — write to flash */
                uint32_t write_len = s_ota.chunk_len;
                uint32_t space_left = s_ota.total_size - s_ota.received;
                if (write_len > space_left) write_len = space_left;

                esp_err_t err = esp_ota_write(s_ota.handle,
                                              s_ota.chunk_buf, write_len);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "esp_ota_write failed at %lu: %s",
                             (unsigned long)s_ota.received, esp_err_to_name(err));
                    send_error("write_failed");
                    uart_ota_abort();
                    return false;
                }

                s_ota.received += write_len;
                s_ota.expected_seq = s_ota.chunk_seq + 1;
                s_ota.phase = PHASE_HEADER;

                /* ACK this chunk */
                send_chunk_ack(s_ota.chunk_seq);

                /* Progress log every 100KB */
                if (s_ota.received % (100 * 1024) < OTA_CHUNK_MAX_DATA) {
                    ESP_LOGI(TAG, "OTA: %lu/%lu bytes (%.0f%%)",
                             (unsigned long)s_ota.received,
                             (unsigned long)s_ota.total_size,
                             (float)s_ota.received / s_ota.total_size * 100);
                }

                /* Auto-finalize when all bytes received */
                if (s_ota.received >= s_ota.total_size) {
                    ESP_LOGI(TAG, "All %lu bytes received — auto-finalizing",
                             (unsigned long)s_ota.received);
                    uart_ota_finalize();
                    return false;
                }
            }
            break;
        }
    }

    return true;
}

bool uart_ota_finalize(void)
{
    if (!s_ota.active) return false;

    ESP_LOGI(TAG, "Finalizing OTA: %lu bytes received", (unsigned long)s_ota.received);

    esp_err_t err = esp_ota_end(s_ota.handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        send_error("validate_failed");
        s_ota.active = false;
        return false;
    }

    err = esp_ota_set_boot_partition(s_ota.partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        send_error("set_boot_failed");
        s_ota.active = false;
        return false;
    }

    ESP_LOGW(TAG, "UART OTA complete! %lu bytes → '%s'. Rebooting...",
             (unsigned long)s_ota.received, s_ota.partition->label);

    char buf[64];
    snprintf(buf, sizeof(buf), "{\"type\":\"ota_done\",\"received\":%lu}",
             (unsigned long)s_ota.received);
    send_json(buf);

    s_ota.active = false;

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return true;  /* unreachable */
}

void uart_ota_abort(void)
{
    if (s_ota.active) {
        esp_ota_abort(s_ota.handle);
        ESP_LOGW(TAG, "UART OTA aborted at %lu bytes",
                 (unsigned long)s_ota.received);
#ifndef BLE_SCANNER_ONLY
        wifi_scanner_resume();
#endif
    }
    s_ota.active = false;
}

bool uart_ota_is_active(void)
{
    if (!s_ota.active) return false;

    /* Timeout: abort after 600s to prevent permanent stuck state.
     * Per-chunk ACK + retransmit can be slow; 10 min is generous. */
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_ota.start_ms > 0 && (now_ms - s_ota.start_ms) > 600000) {
        ESP_LOGE(TAG, "OTA TIMEOUT after 600s — aborting");
        uart_ota_abort();
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    return true;
}
