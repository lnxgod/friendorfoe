/**
 * Friend or Foe — Scanner UART OTA Receiver (v3: store-then-flash via PSRAM)
 *
 * Goals (from the v0.55 → v0.59 rollout post-mortem):
 *   1. Hard-stop BLE + WiFi scans for the duration of the OTA so nothing
 *      competes for CPU / UART / flash bandwidth.
 *   2. Stage the entire firmware image in PSRAM. Validate the full-image
 *      CRC there, BEFORE touching the OTA partition. A mid-flash abort no
 *      longer leaves the flash in an inconsistent state.
 *   3. Keep a clean state machine (IDLE → STAGING → VALIDATING → FLASHING
 *      → REBOOTING, plus ERROR transients). Every error path calls a single
 *      cleanup routine that frees PSRAM, resumes scans, and returns to IDLE
 *      so the scanner is always recoverable without a physical reset.
 *   4. Idle-watchdog: if no chunk arrives for 30 s while staging, declare
 *      the transfer stuck and recover.
 *   5. Explicit `{"type":"ota_abort"}` support so the uplink can cancel.
 *
 * Protocol on the wire (unchanged from v0.59):
 *   Uplink → Scanner:  {"type":"stop"}            (scanner emits stop_ack in main.c)
 *                      {"type":"ota_begin","size":N,"crc":C}
 *                      [binary chunks: OTA_CHUNK_MAGIC seq(2) len(2) data CRC32(4)]
 *                      {"type":"ota_end"}          (informational; auto-finalize
 *                                                   still fires when bytes received
 *                                                   == total_size)
 *                      {"type":"ota_abort"}        (cancel)
 *   Scanner → Uplink:  {"type":"ota_ack"}          (entered OTA mode OK)
 *                      {"type":"ota_nack","seq":N} (bad chunk CRC — retransmit)
 *                      {"type":"ota_done","received":N}  (image valid, rebooting)
 *                      {"type":"ota_error","reason":"X"} (error, back to IDLE)
 */

#include "uart_ota.h"
#include "uart_protocol.h"
#include "uart_tx.h"
#include "psram_alloc.h"
#include "wifi_scanner.h"
#include "ble_remote_id.h"
#include "calibration_mode.h"

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

/* Per-chunk idle watchdog — if no chunk arrives for this long during
 * staging, recover and return to IDLE. Uplink's per-chunk cadence is
 * ~50-80 ms under normal conditions; 30 s is an order of magnitude over
 * the worst slow-link case. */
#define STAGING_IDLE_TIMEOUT_MS   30000

/* Overall operation ceiling — even a slow 2 MB firmware should flash in
 * well under this window (stage+validate+flash ≈ 3–6 min wall clock). */
#define OPERATION_CEILING_MS     900000   /* 15 min */

/* Keep final flash commits in modest pieces. Some badge scanners in the
 * field run older firmware that staged fine but failed committing one large
 * PSRAM buffer in a single esp_ota_write() call. Chunking is slower but far
 * easier on the flash/OTA stack and gives us useful progress/error context. */
#define FLASH_WRITE_CHUNK_BYTES   16384

/* ── OTA session state ─────────────────────────────────────────────────── */

typedef enum {
    OTA_IDLE = 0,
    OTA_STAGING,      /* receiving chunks into PSRAM buffer */
    OTA_VALIDATING,   /* computing full-image CRC from PSRAM */
    OTA_FLASHING,     /* esp_ota_write from PSRAM to flash partition */
    OTA_REBOOTING     /* esp_restart imminent */
} ota_state_t;

static struct {
    ota_state_t              state;
    uart_port_t              uart_num;

    /* Image metadata (from ota_begin) */
    uint32_t                 total_size;
    uint32_t                 expected_image_crc;
    bool                     has_expected_crc;
    char                     session_id[24];

    /* Staging buffer in PSRAM (NULL when IDLE) */
    uint8_t                 *buffer;
    uint32_t                 received;         /* bytes written to buffer so far */

    /* Binary chunk frame accumulator */
    uint8_t                  hdr_buf[OTA_CHUNK_HEADER_SIZE];
    uint8_t                  hdr_pos;
    uint8_t                  chunk_buf[OTA_CHUNK_MAX_DATA];
    uint16_t                 chunk_len;
    uint16_t                 chunk_pos;
    uint8_t                  crc_buf[OTA_CHUNK_CRC_SIZE];
    uint8_t                  crc_pos;
    uint16_t                 chunk_seq;
    uint16_t                 expected_seq;
    enum { PHASE_HEADER, PHASE_DATA, PHASE_CRC } phase;

    /* Timing */
    int64_t                  start_ms;         /* OTA session start */
    int64_t                  last_chunk_ms;    /* for idle watchdog */
    uint32_t                 next_progress_bytes;
} s_ota = {0};

static bool s_radio_control_enabled = true;

/* ── Wire helpers ──────────────────────────────────────────────────────── */

static void send_json(const char *json)
{
    if (s_ota.uart_num == UART_NUM_MAX) return;
    uart_write_bytes(s_ota.uart_num, json, strlen(json));
    uart_write_bytes(s_ota.uart_num, "\n", 1);
}

static void send_chunk_nack(uint16_t seq)
{
    char buf[96];
    if (s_ota.session_id[0]) {
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"ota_nack\",\"seq\":%u,\"session_id\":\"%s\"}",
                 seq, s_ota.session_id);
    } else {
        snprintf(buf, sizeof(buf), "{\"type\":\"ota_nack\",\"seq\":%u}", seq);
    }
    send_json(buf);
}

static void send_ota_error(const char *reason)
{
    uart_tx_set_firmware_update_state(true, uart_tx_firmware_target_version(), "error");
    uart_tx_set_firmware_error(reason ? reason : "ota_error");
    char buf[176];
    if (s_ota.session_id[0]) {
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"ota_error\",\"reason\":\"%s\",\"received\":%lu,"
                 "\"session_id\":\"%s\"}",
                 reason ? reason : "unknown",
                 (unsigned long)s_ota.received,
                 s_ota.session_id);
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"ota_error\",\"reason\":\"%s\",\"received\":%lu}",
                 reason ? reason : "unknown", (unsigned long)s_ota.received);
    }
    send_json(buf);
}

static void send_ota_errorf(const char *prefix, esp_err_t err)
{
    char reason[64];
    snprintf(reason, sizeof(reason), "%s_%s",
             prefix ? prefix : "ota", esp_err_to_name(err));
    send_ota_error(reason);
}

static void send_ota_progress(void)
{
    if (s_ota.total_size == 0) return;
    int percent = (int)(((uint64_t)s_ota.received * 100ULL) / s_ota.total_size);
    char buf[160];
    if (s_ota.session_id[0]) {
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"ota_progress\",\"received\":%lu,\"total\":%lu,"
                 "\"percent\":%d,\"session_id\":\"%s\"}",
                 (unsigned long)s_ota.received,
                 (unsigned long)s_ota.total_size,
                 percent,
                 s_ota.session_id);
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"ota_progress\",\"received\":%lu,\"total\":%lu,"
                 "\"percent\":%d}",
                 (unsigned long)s_ota.received,
                 (unsigned long)s_ota.total_size,
                 percent);
    }
    send_json(buf);
}

/* ── Scan suspend / resume helpers ─────────────────────────────────────── */

/* Hard-halt BLE + WiFi scanning so the OTA has the chip to itself. Called
 * exactly once on successful ota_begin. Must be paired with resume_scans()
 * on EVERY exit path (success or failure). */
static void halt_scans(void)
{
    if (!s_radio_control_enabled) {
        ESP_LOGW(TAG, "OTA scan halt skipped: radio control disabled");
        return;
    }
    wifi_scanner_pause();
    ble_remote_id_stop();
    /* Give the scan tasks a tick to notice the flags and release shared
     * resources (e.g. the NimBLE lock, the WiFi scan timer). */
    vTaskDelay(pdMS_TO_TICKS(100));
}

static void resume_scans(void)
{
    if (!s_radio_control_enabled) {
        ESP_LOGW(TAG, "OTA scan resume skipped: radio control disabled");
        return;
    }
    scanner_scan_profile_apply();
}

/* ── Cleanup + state reset ─────────────────────────────────────────────── */

/* Single place every error/abort path goes. Frees PSRAM, resumes scans,
 * clears chunk accumulator, returns to IDLE. Leaves s_ota.uart_num so
 * send_json still works for the caller's trailing ota_error emit. */
static void cleanup_and_idle(void)
{
    if (s_ota.buffer) {
        psram_free(s_ota.buffer);
        s_ota.buffer = NULL;
    }
    /* Clear the binary-frame accumulator (but preserve uart_num for any
     * trailing ota_error emit by the caller). */
    uart_port_t u = s_ota.uart_num;
    memset(&s_ota, 0, sizeof(s_ota));
    s_ota.uart_num = u;
    s_ota.state = OTA_IDLE;

    resume_scans();
}

/* ── Public API ────────────────────────────────────────────────────────── */

bool uart_ota_begin(uint32_t total_size, uint32_t expected_crc,
                    bool has_crc, uart_port_t uart_num,
                    const char *session_id)
{
    s_ota.uart_num = uart_num;   /* so error emits go to the right port */

    /* If we're not IDLE, another ota_begin arrived while a previous one was
     * in flight. Abort the old session cleanly and start fresh. Common case
     * after a transient failure: uplink retries and we should accept. */
    if (s_ota.state != OTA_IDLE) {
        ESP_LOGW(TAG, "ota_begin while state=%d — recovering to IDLE", s_ota.state);
        cleanup_and_idle();
    }
    if (session_id && session_id[0]) {
        strncpy(s_ota.session_id, session_id, sizeof(s_ota.session_id) - 1);
        s_ota.session_id[sizeof(s_ota.session_id) - 1] = '\0';
    } else {
        s_ota.session_id[0] = '\0';
    }

    /* Sanity checks */
    if (total_size == 0 || total_size > 8 * 1024 * 1024) {
        ESP_LOGE(TAG, "ota_begin: bad size %lu", (unsigned long)total_size);
        send_ota_error("bad_size");
        return false;
    }

    /* Allocate the staging buffer in PSRAM. This is where chunks land before
     * we touch flash at all. 1 MB firmware → 1 MB PSRAM; headroom to 8 MB. */
    uint8_t *buf = (uint8_t *)psram_alloc_strict(total_size);
    if (!buf) {
        ESP_LOGE(TAG, "ota_begin: PSRAM alloc failed for %lu bytes (free=%u)",
                 (unsigned long)total_size, (unsigned)psram_free_size());
        send_ota_error("no_memory");
        return false;
    }

    /* Halt both scans before we touch any shared state. Scanning continues
     * to compete for CPU & UART otherwise, and a detection JSON flying out
     * of the scanner during OTA mode is annoying to debug. */
    halt_scans();

    char session_copy[sizeof(s_ota.session_id)];
    strncpy(session_copy, s_ota.session_id, sizeof(session_copy) - 1);
    session_copy[sizeof(session_copy) - 1] = '\0';

    s_ota.buffer             = buf;
    s_ota.total_size         = total_size;
    s_ota.expected_image_crc = expected_crc;
    s_ota.has_expected_crc   = has_crc;
    strncpy(s_ota.session_id, session_copy, sizeof(s_ota.session_id) - 1);
    s_ota.session_id[sizeof(s_ota.session_id) - 1] = '\0';
    s_ota.received           = 0;
    s_ota.phase              = PHASE_HEADER;
    s_ota.hdr_pos            = 0;
    s_ota.chunk_pos          = 0;
    s_ota.crc_pos            = 0;
    s_ota.expected_seq       = 0;
    s_ota.start_ms           = esp_timer_get_time() / 1000;
    s_ota.last_chunk_ms      = s_ota.start_ms;
    s_ota.next_progress_bytes = OTA_RELAY_PROGRESS_INTERVAL_BYTES;
    s_ota.state              = OTA_STAGING;

    ESP_LOGW(TAG, "OTA staging: %lu bytes -> PSRAM (session=%s image CRC %s%08lX, PSRAM free=%u KB)",
             (unsigned long)total_size,
             s_ota.session_id[0] ? s_ota.session_id : "none",
             has_crc ? "" : "none/",
             (unsigned long)expected_crc,
             (unsigned)(psram_free_size() / 1024));

    /* Ready for chunks */
    if (s_ota.session_id[0]) {
        char ack[80];
        snprintf(ack, sizeof(ack),
                 "{\"type\":\"ota_ack\",\"session_id\":\"%s\"}",
                 s_ota.session_id);
        send_json(ack);
    } else {
        send_json("{\"type\":\"ota_ack\"}");
    }
    return true;
}

/* Kick off the final flash phase: verify full-image CRC in PSRAM, then
 * esp_ota_begin/write/end/set_boot_partition → ota_done → reboot. Any error
 * cleans up and stays in the running firmware. */
static bool validate_and_flash(void)
{
    s_ota.state = OTA_VALIDATING;

    if (s_ota.has_expected_crc) {
        uint32_t actual = esp_rom_crc32_le(0, s_ota.buffer, s_ota.received);
        if (actual != s_ota.expected_image_crc) {
            ESP_LOGE(TAG, "IMAGE CRC MISMATCH: expected=%08lX got=%08lX",
                     (unsigned long)s_ota.expected_image_crc,
                     (unsigned long)actual);
            send_ota_error("image_crc_mismatch");
            cleanup_and_idle();
            return false;
        }
        ESP_LOGW(TAG, "Image CRC verified: %08lX (%lu bytes)",
                 (unsigned long)actual, (unsigned long)s_ota.received);
    }

    s_ota.state = OTA_FLASHING;

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        send_ota_error("no_partition");
        cleanup_and_idle();
        return false;
    }
    if (s_ota.received > update->size) {
        ESP_LOGE(TAG, "Image too large for OTA partition '%s': image=%lu partition=%lu",
                 update->label,
                 (unsigned long)s_ota.received,
                 (unsigned long)update->size);
        send_ota_error("image_too_large");
        cleanup_and_idle();
        return false;
    }

    /* Pre-emptively make sure no stale handle is hanging around from a
     * previous botched attempt. esp_ota_abort is safe on NULL. */
    esp_ota_handle_t handle;
    esp_err_t err = esp_ota_begin(update, s_ota.received, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        send_ota_errorf("begin", err);
        cleanup_and_idle();
        return false;
    }

    ESP_LOGW(TAG, "Flashing %lu bytes to partition '%s' (%lu bytes) in %u byte chunks...",
             (unsigned long)s_ota.received,
             update->label,
             (unsigned long)update->size,
             (unsigned)FLASH_WRITE_CHUNK_BYTES);

    uint32_t written = 0;
    while (written < s_ota.received) {
        size_t write_len = s_ota.received - written;
        if (write_len > FLASH_WRITE_CHUNK_BYTES) {
            write_len = FLASH_WRITE_CHUNK_BYTES;
        }
        err = esp_ota_write(handle, s_ota.buffer + written, write_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed at %lu/%lu (%u bytes): %s",
                     (unsigned long)written,
                     (unsigned long)s_ota.received,
                     (unsigned)write_len,
                     esp_err_to_name(err));
            send_ota_errorf("write", err);
            esp_ota_abort(handle);
            cleanup_and_idle();
            return false;
        }
        written += (uint32_t)write_len;
        if (written % (128 * 1024) < FLASH_WRITE_CHUNK_BYTES ||
            written >= s_ota.received) {
            ESP_LOGI(TAG, "Flash commit: %lu/%lu (%.0f%%)",
                     (unsigned long)written,
                     (unsigned long)s_ota.received,
                     (float)written / s_ota.received * 100.0f);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        send_ota_errorf("end", err);
        cleanup_and_idle();
        return false;
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        send_ota_errorf("set_boot", err);
        cleanup_and_idle();
        return false;
    }

    s_ota.state = OTA_REBOOTING;

    ESP_LOGW(TAG, "OTA complete — %lu bytes in '%s'. Emitting done + rebooting.",
             (unsigned long)s_ota.received, update->label);

    char buf[112];
    if (s_ota.session_id[0]) {
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"ota_done\",\"received\":%lu,\"session_id\":\"%s\"}",
                 (unsigned long)s_ota.received,
                 s_ota.session_id);
    } else {
        snprintf(buf, sizeof(buf), "{\"type\":\"ota_done\",\"received\":%lu}",
                 (unsigned long)s_ota.received);
    }
    send_json(buf);

    /* Make sure the "done" bytes physically leave the UART TX FIFO before
     * we reset, otherwise the uplink's Stage 3 waits on silence. */
    uart_wait_tx_done(s_ota.uart_num, pdMS_TO_TICKS(1000));

    /* Free PSRAM so esp_restart doesn't leak — not strictly necessary since
     * reboot wipes RAM, but keeps semantics tidy. */
    if (s_ota.buffer) { psram_free(s_ota.buffer); s_ota.buffer = NULL; }

    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return true;   /* unreachable */
}

bool uart_ota_process_data(const uint8_t *data, int len)
{
    if (s_ota.state != OTA_STAGING) return false;

    for (int i = 0; i < len; i++) {
        uint8_t b = data[i];

        switch (s_ota.phase) {

        case PHASE_HEADER:
            if (s_ota.hdr_pos == 0 && b != OTA_CHUNK_MAGIC) {
                continue;  /* skip non-magic filler between chunks */
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
                if (seq != s_ota.expected_seq) {
                    ESP_LOGW(TAG, "Unexpected OTA seq=%u expected=%u -> NACK expected",
                             (unsigned)seq, (unsigned)s_ota.expected_seq);
                    send_chunk_nack(s_ota.expected_seq);
                    s_ota.hdr_pos = 0;
                    continue;
                }
                s_ota.chunk_seq = seq;
                s_ota.chunk_len = clen;
                s_ota.chunk_pos = 0;
                s_ota.crc_pos   = 0;
                s_ota.hdr_pos   = 0;
                s_ota.phase     = PHASE_DATA;
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
                uint32_t expected_crc =
                    ((uint32_t)s_ota.crc_buf[0] << 24) |
                    ((uint32_t)s_ota.crc_buf[1] << 16) |
                    ((uint32_t)s_ota.crc_buf[2] <<  8) |
                    ((uint32_t)s_ota.crc_buf[3]);
                uint32_t actual_crc = esp_rom_crc32_le(0,
                    s_ota.chunk_buf, s_ota.chunk_len);

                if (actual_crc != expected_crc) {
                    ESP_LOGW(TAG, "CRC fail seq=%d (exp=%08lX got=%08lX) → NACK",
                             s_ota.chunk_seq,
                             (unsigned long)expected_crc,
                             (unsigned long)actual_crc);
                    send_chunk_nack(s_ota.chunk_seq);
                    s_ota.phase = PHASE_HEADER;
                    break;
                }

                /* Good chunk: copy into the PSRAM staging buffer at the
                 * current offset. Bounds guard against a runaway sender. */
                uint32_t write_len = s_ota.chunk_len;
                uint32_t space_left = s_ota.total_size - s_ota.received;
                if (write_len > space_left) write_len = space_left;

                memcpy(s_ota.buffer + s_ota.received,
                       s_ota.chunk_buf, write_len);
                s_ota.received     += write_len;
                s_ota.last_chunk_ms = esp_timer_get_time() / 1000;
                s_ota.phase         = PHASE_HEADER;
                s_ota.expected_seq++;

                if (s_ota.received >= s_ota.next_progress_bytes ||
                    s_ota.received >= s_ota.total_size) {
                    send_ota_progress();
                    while (s_ota.next_progress_bytes <= s_ota.received) {
                        s_ota.next_progress_bytes += OTA_RELAY_PROGRESS_INTERVAL_BYTES;
                    }
                }

                if (s_ota.received % (100 * 1024) < OTA_CHUNK_MAX_DATA) {
                    ESP_LOGI(TAG, "Staging: %lu/%lu (%.0f%%) PSRAM_free=%u KB",
                             (unsigned long)s_ota.received,
                             (unsigned long)s_ota.total_size,
                             (float)s_ota.received / s_ota.total_size * 100,
                             (unsigned)(psram_free_size() / 1024));
                }

                /* All bytes staged — validate + flash. No separate
                 * uart_ota_finalize() call needed; we auto-progress. */
                if (s_ota.received >= s_ota.total_size) {
                    ESP_LOGI(TAG, "All %lu bytes staged — validating + flashing",
                             (unsigned long)s_ota.received);
                    validate_and_flash();   /* one-way ticket; reboots on success */
                    return false;
                }
            }
            break;
        }
    }
    return true;
}

/* Informational ota_end entry point. Validation+flash is also driven by the
 * chunk stream reaching total_size in process_data(). */
bool uart_ota_finalize(void)
{
    if (s_ota.state != OTA_STAGING) return false;
    if (s_ota.received != s_ota.total_size) {
        ESP_LOGE(TAG, "finalize called with %lu/%lu bytes — aborting",
                 (unsigned long)s_ota.received, (unsigned long)s_ota.total_size);
        send_ota_error("incomplete");
        cleanup_and_idle();
        return false;
    }
    return validate_and_flash();
}

void uart_ota_abort(void)
{
    if (s_ota.state == OTA_IDLE) return;

    ESP_LOGW(TAG, "OTA aborted at %lu/%lu bytes (state=%d)",
             (unsigned long)s_ota.received,
             (unsigned long)s_ota.total_size,
             s_ota.state);
    send_ota_error("aborted");
    cleanup_and_idle();
}

bool uart_ota_is_active(void)
{
    if (s_ota.state == OTA_IDLE) return false;

    int64_t now_ms = esp_timer_get_time() / 1000;

    /* Idle watchdog during staging — no chunks in 30 s means the link
     * is dead. Clean up instead of sitting in a broken state forever. */
    if (s_ota.state == OTA_STAGING &&
        s_ota.last_chunk_ms > 0 &&
        (now_ms - s_ota.last_chunk_ms) > STAGING_IDLE_TIMEOUT_MS) {
        ESP_LOGE(TAG, "Staging idle for %lldms — aborting",
                 (long long)(now_ms - s_ota.last_chunk_ms));
        send_ota_error("idle_timeout");
        cleanup_and_idle();
        return false;
    }

    /* Overall ceiling — catch anything that slips past the idle watchdog. */
    if (s_ota.start_ms > 0 && (now_ms - s_ota.start_ms) > OPERATION_CEILING_MS) {
        ESP_LOGE(TAG, "OTA overall timeout (%lldms) — aborting",
                 (long long)(now_ms - s_ota.start_ms));
        send_ota_error("overall_timeout");
        cleanup_and_idle();
        return false;
    }
    return true;
}

const char *uart_ota_state_label(void)
{
    switch (s_ota.state) {
        case OTA_IDLE:       return "idle";
        case OTA_STAGING:    return "staging";
        case OTA_VALIDATING: return "validating";
        case OTA_FLASHING:   return "flashing";
        case OTA_REBOOTING:  return "rebooting";
        default:             return "unknown";
    }
}

const char *uart_ota_session_id(void)
{
    return s_ota.session_id;
}

uint32_t uart_ota_received(void)
{
    return s_ota.received;
}

uint32_t uart_ota_total_size(void)
{
    return s_ota.total_size;
}

void uart_ota_set_radio_control_enabled(bool enabled)
{
    s_radio_control_enabled = enabled;
}
