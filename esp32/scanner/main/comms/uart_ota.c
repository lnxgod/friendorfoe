/**
 * Friend or Foe — Scanner UART OTA Receiver
 *
 * Receives firmware from the uplink node over UART.
 *
 * Protocol:
 *   1. Uplink sends JSON: {"type":"ota_begin","size":N}
 *   2. Scanner ACKs: {"type":"ota_ack"}
 *   3. Uplink sends binary chunks: [0xF0][seq_hi][seq_lo][len_hi][len_lo] + data
 *   4. Scanner ACKs every 16 chunks: {"type":"ota_progress","received":N}
 *   5. Uplink sends JSON: {"type":"ota_end"}
 *   6. Scanner validates, sets boot partition, reboots
 */

#include "uart_ota.h"
#include "uart_protocol.h"

#include <string.h>
#include <stdio.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
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
    uint16_t                 chunks_since_ack;

    /* Partial chunk accumulator (handles UART fragmentation) */
    uint8_t                  hdr_buf[5];     /* 5-byte header */
    uint8_t                  hdr_pos;
    uint8_t                  chunk_buf[OTA_CHUNK_MAX_DATA];
    uint16_t                 chunk_len;      /* expected data length */
    uint16_t                 chunk_pos;      /* bytes received so far */
    bool                     in_chunk;       /* receiving chunk data */
    int64_t                  start_ms;       /* session start time for timeout */
    uint32_t                 expected_crc;   /* CRC32 from chunk header (validated after data) */
} s_ota = {0};

/* ── Send JSON response back to uplink ─────────────────────────────────── */

static void send_json(const char *json)
{
    uart_write_bytes(s_ota.uart_num, json, strlen(json));
    uart_write_bytes(s_ota.uart_num, "\n", 1);
}

static void send_ack(void)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"type\":\"ota_ack\"}");
    send_json(buf);
}

static void send_progress(void)
{
    char buf[80];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"ota_progress\",\"received\":%lu}",
             (unsigned long)s_ota.received);
    send_json(buf);
}

static void send_error(const char *msg)
{
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"ota_error\",\"error\":\"%s\",\"received\":%lu}",
             msg, (unsigned long)s_ota.received);
    send_json(buf);
}

/* ── Public API ────────────────────────────────────────────────────────── */

static uint32_t compute_crc32(const uint8_t *data, int len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return crc ^ 0xFFFFFFFF;
}

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

    ESP_LOGW(TAG, "UART OTA started: %lu bytes → partition '%s'",
             (unsigned long)total_size, update->label);

    send_ack();
    return true;
}

bool uart_ota_process_data(const uint8_t *data, int len)
{
    if (!s_ota.active) return false;

    for (int i = 0; i < len; i++) {
        uint8_t b = data[i];

        if (!s_ota.in_chunk) {
            /* Abort sequence: 4x 0xFF in a row = force abort OTA */
            static uint8_t ff_count = 0;
            if (b == 0xFF) {
                if (++ff_count >= 4) {
                    ESP_LOGW(TAG, "OTA abort sequence received (4x 0xFF)");
                    uart_ota_abort();
                    return false;
                }
            } else {
                ff_count = 0;
            }

            /* Accumulating 9-byte header: [0xF0][seq(2)][len(2)][crc32(4)] */
            if (s_ota.hdr_pos == 0 && b != OTA_CHUNK_MAGIC) {
                continue;  /* Skip non-chunk bytes */
            }
            s_ota.hdr_buf[s_ota.hdr_pos++] = b;

            if (s_ota.hdr_pos >= OTA_CHUNK_HEADER_SIZE) {
                /* Header complete — parse */
                uint16_t seq = ((uint16_t)s_ota.hdr_buf[1] << 8) | s_ota.hdr_buf[2];
                uint16_t clen = ((uint16_t)s_ota.hdr_buf[3] << 8) | s_ota.hdr_buf[4];
                uint32_t expected_crc = ((uint32_t)s_ota.hdr_buf[5] << 24) |
                                        ((uint32_t)s_ota.hdr_buf[6] << 16) |
                                        ((uint32_t)s_ota.hdr_buf[7] << 8) |
                                        (uint32_t)s_ota.hdr_buf[8];

                if (clen > OTA_CHUNK_MAX_DATA) {
                    ESP_LOGE(TAG, "Chunk too large: %d (seq %d)", clen, seq);
                    /* Don't abort — might be a corrupted header. Reset and look for next magic. */
                    s_ota.hdr_pos = 0;
                    continue;
                }

                s_ota.chunk_len = clen;
                s_ota.chunk_pos = 0;
                s_ota.in_chunk = true;
                s_ota.hdr_pos = 0;

                /* Store expected CRC for validation after data arrives */
                s_ota.expected_crc = expected_crc;

                if (seq != s_ota.expected_seq) {
                    ESP_LOGW(TAG, "Seq mismatch: expected %d got %d", s_ota.expected_seq, seq);
                }
                s_ota.expected_seq = seq + 1;
            }
        } else {
            /* Accumulating chunk data */
            s_ota.chunk_buf[s_ota.chunk_pos++] = b;

            if (s_ota.chunk_pos >= s_ota.chunk_len) {
                /* Chunk complete — validate CRC32 before writing */
                uint32_t actual_crc = compute_crc32(s_ota.chunk_buf, s_ota.chunk_len);
                if (actual_crc != s_ota.expected_crc && s_ota.expected_crc != 0) {
                    ESP_LOGW(TAG, "CRC mismatch seq=%d: expected=%08lx got=%08lx — skipping",
                             s_ota.expected_seq - 1,
                             (unsigned long)s_ota.expected_crc, (unsigned long)actual_crc);
                    /* Don't write corrupted data — skip this chunk.
                     * The received count won't reach total_size, so auto-finalize
                     * won't trigger, and the 90s timeout will abort + reboot.
                     * On the next attempt, this chunk might come through clean. */
                    s_ota.in_chunk = false;
                    continue;
                }

                /* CRC valid — write to OTA partition */
                esp_err_t err = esp_ota_write(s_ota.handle,
                                              s_ota.chunk_buf, s_ota.chunk_len);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "esp_ota_write failed at %lu: %s",
                             (unsigned long)s_ota.received, esp_err_to_name(err));
                    send_error("write_failed");
                    uart_ota_abort();
                    return false;
                }

                s_ota.received += s_ota.chunk_len;
                s_ota.in_chunk = false;
                s_ota.chunks_since_ack++;

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
                    return false;  /* OTA done, exit processing */
                }

                /* ACK every N chunks for flow control */
                if (s_ota.chunks_since_ack >= OTA_ACK_INTERVAL_CHUNKS) {
                    send_progress();
                    s_ota.chunks_since_ack = 0;
                }
            }
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
        /* Restore normal baud rate */
        uart_set_baudrate(s_ota.uart_num, UART_BAUD_RATE);
        ESP_LOGW(TAG, "UART OTA aborted at %lu bytes, baud restored to %d",
                 (unsigned long)s_ota.received, UART_BAUD_RATE);
    }
    s_ota.active = false;
}

bool uart_ota_is_active(void)
{
    if (!s_ota.active) return false;

    /* Failsafe #2: timeout — if OTA has been active >90s, abort and reboot.
     * Prevents permanent stuck state if finalize never arrives. */
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_ota.start_ms > 0 && (now_ms - s_ota.start_ms) > 90000) {
        ESP_LOGE("uart_ota", "OTA TIMEOUT after 90s — aborting to prevent stuck state");
        uart_ota_abort();
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    return true;
}
