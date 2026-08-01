#pragma once

/**
 * Friend or Foe — Scanner Firmware Store
 *
 * Stores scanner firmware on the uplink's fw_store flash partition.
 * The backend uploads firmware in chunks, then triggers a UART relay
 * to flash the scanner at the uplink's own pace — no HTTP timeout pressure.
 *
 * Flow:
 *   1. POST /api/fw/upload   — receive firmware, store to flash
 *   2. POST /api/fw/relay    — read from flash, send to scanner UART
 *   3. GET  /api/fw/info     — check stored firmware status
 */

#ifndef UNIT_TESTING
#include "esp_http_server.h"
#include "esp_partition.h"
#else
typedef void *httpd_handle_t;
typedef struct esp_partition_t esp_partition_t;
#endif
#include "firmware_operation_token.h"
#include "firmware_image_contract.h"
#include "fw_manifest_store.h"
#if defined(FOF_DC34_GAME_CANARY)
#include "firmware_auto_policy.h"
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#if defined(FOF_DC34_GAME_CANARY)
#include <string.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Register firmware store HTTP endpoints on the given server. */
void fw_store_register(httpd_handle_t server);

typedef enum {
    FW_STORE_ACTIVITY_INACTIVE = 0,
    FW_STORE_ACTIVITY_ACTIVE,
    FW_STORE_ACTIVITY_UNKNOWN,
} fw_store_activity_t;

typedef enum {
    FW_CAMPAIGN_IDLE = 0,
    FW_CAMPAIGN_OPERATION_ACTIVE,
    FW_CAMPAIGN_PENDING,
    FW_CAMPAIGN_ALL_TERMINAL,
    FW_CAMPAIGN_DEPENDENCY_DEFERRED,
    FW_CAMPAIGN_UNKNOWN,
} fw_store_campaign_state_t;

typedef struct {
    fw_store_campaign_state_t state;
    fw_operation_owner_t owner;
    uint32_t operation_epoch;
    uint32_t manifest_generation;
    bool radio_inhibited;
} fw_store_campaign_snapshot_t;

typedef enum {
    FW_UPDATE_PREEMPT_QUIESCED = 0,
#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
    FW_UPDATE_PREEMPT_REBOOT_SAFE,
#endif
    FW_UPDATE_PREEMPT_WAITING_FOR_OWNER,
    FW_UPDATE_PREEMPT_BUSY,
} fw_update_preempt_result_t;

/** Positively sample upload/relay activity for recovery policy decisions.
 *  UNKNOWN means coordinator state could not be sampled safely. */
fw_store_activity_t fw_store_activity_sample(void);

/** Consistent, bounded snapshot of operation ownership and durable campaign. */
bool fw_store_campaign_state_sample(fw_store_campaign_snapshot_t *out);

/** Latch game-radio inhibit and request a coordinator safe point. */
fw_update_preempt_result_t fw_store_request_update_preemption(void);

/** Fail-busy radio gate. True on any active, pending, deferred, or unknown work. */
bool fw_store_game_radio_must_yield(void);

/** Claim exclusive firmware mutation ownership. Scanner staging passes
 *  acquire_uart_lease=true. The relay adapter and uplink OTA pass false;
 *  relay preparation acquires its injected UART lease as the next ordered
 *  operation and retains both owners through transfer cleanup. */
bool fw_store_operation_try_begin(fw_operation_owner_t owner,
                                  bool acquire_uart_lease,
                                  fw_operation_token_t *out_token);

/** Release only the exact owner-plus-generation token returned by begin. */
bool fw_store_operation_end(fw_operation_token_t token);

/** Exact lock-protected firmware-operation state for startup gating. */
bool fw_store_operation_is_active(void);

/** Atomically reserve a non-returning recovery restart against firmware work.
 *  Once granted, no new upload/relay operation may begin before restart. */
bool fw_store_try_reserve_recovery_restart(void);

/** Legacy fail-busy activity check for callers that must defer on uncertainty. */
bool fw_store_is_relay_active(void);
int64_t fw_store_last_relay_progress_ms(void);

/**
 * Ask connected scanners to run their firmware check immediately.
 *
 * Returns a bit mask of commands actually accepted by the scanner UARTs
 * (bit 0 = BLE, bit 1 = Wi-Fi).  A firmware upload/relay owns the UART lease,
 * so this returns zero while that operation is active instead of injecting a
 * command into an OTA byte stream.
 */
uint8_t fw_store_request_scanner_checks(uint8_t target_slot_mask);

typedef struct {
    bool     stored;
    uint32_t generation;
    uint8_t  target_slot_mask;
    uint32_t manifest_crc32;
    uint32_t size;
    uint32_t checksum;
    char     version[32];
    char     name[32];
    char     project[33];
    char     hardware[33];
    char     sha256[FOF_FIRMWARE_SHA256_HEX_SIZE];
    char     partition[16];
} fw_store_info_t;
#define FOF_FW_STORE_INFO_T_DEFINED 1

#define FW_AUTO_UPDATE_SCANNER_COUNT 2
#define FW_AUTO_UPDATE_STATE_SIZE 24
#define FW_AUTO_UPDATE_SLOT_BLE  (1u << 0)
#define FW_AUTO_UPDATE_SLOT_WIFI (1u << 1)
#define FW_AUTO_UPDATE_SLOT_ALL  (FW_AUTO_UPDATE_SLOT_BLE | FW_AUTO_UPDATE_SLOT_WIFI)

#if defined(FOF_DC34_GAME_CANARY)
typedef enum {
    FW_CAMPAIGN_COMPLETION_PENDING = 0,
    FW_CAMPAIGN_COMPLETION_SUCCESS,
    FW_CAMPAIGN_COMPLETION_TERMINAL_FAILURE,
    FW_CAMPAIGN_COMPLETION_UNKNOWN,
} fw_store_campaign_completion_t;

typedef struct {
    bool operation_active;
    fw_operation_owner_t operation_owner;
    bool coordinator_sampled;
    bool persistence_certain;
    bool campaign_loaded;
    bool campaign_valid;
    bool campaign_fail_closed;
    uint8_t target_slot_mask;
    fof_auto_slot_state_t slot_state[FW_AUTO_UPDATE_SCANNER_COUNT];
} fw_store_campaign_completion_view_t;

/**
 * Pure completion policy shared by the atomic runtime sampler and native
 * tests. An operation owner takes precedence over coordinator ambiguity.
 */
static inline fw_store_campaign_completion_t
fw_store_campaign_completion_classify(
    const fw_store_campaign_completion_view_t *view)
{
    if (!view) {
        return FW_CAMPAIGN_COMPLETION_UNKNOWN;
    }
    if (view->operation_active ||
        view->operation_owner != FW_OPERATION_OWNER_NONE) {
        return FW_CAMPAIGN_COMPLETION_PENDING;
    }
    if (!view->coordinator_sampled || !view->persistence_certain) {
        return FW_CAMPAIGN_COMPLETION_UNKNOWN;
    }
    if (!view->campaign_loaded) {
        return FW_CAMPAIGN_COMPLETION_SUCCESS;
    }
    if (!view->campaign_valid) {
        return FW_CAMPAIGN_COMPLETION_UNKNOWN;
    }
    if (view->campaign_fail_closed) {
        return FW_CAMPAIGN_COMPLETION_TERMINAL_FAILURE;
    }
    if (view->target_slot_mask == 0U ||
        (view->target_slot_mask &
         (uint8_t)~FW_AUTO_UPDATE_SLOT_ALL) != 0U) {
        return FW_CAMPAIGN_COMPLETION_UNKNOWN;
    }

    bool pending = false;
    bool terminal_failure = false;
    for (int scanner_id = 0;
         scanner_id < FW_AUTO_UPDATE_SCANNER_COUNT; ++scanner_id) {
        uint8_t bit = (uint8_t)(1U << scanner_id);
        if ((view->target_slot_mask & bit) == 0U) {
            continue;
        }
        switch (view->slot_state[scanner_id]) {
            case FOF_AUTO_SLOT_CONVERGED:
            case FOF_AUTO_SLOT_CURRENT:
                break;
            case FOF_AUTO_SLOT_REFUSED:
            case FOF_AUTO_SLOT_FAILED:
            case FOF_AUTO_SLOT_NEWER_SKIPPED:
                terminal_failure = true;
                break;
            case FOF_AUTO_SLOT_AWAITING_CHECK:
            case FOF_AUTO_SLOT_OFFERED:
            case FOF_AUTO_SLOT_READY_QUEUED:
            case FOF_AUTO_SLOT_RELAYING:
            case FOF_AUTO_SLOT_RECOVERING:
                pending = true;
                break;
            case FOF_AUTO_SLOT_EXCLUDED:
            default:
                return FW_CAMPAIGN_COMPLETION_UNKNOWN;
        }
    }
    if (pending) {
        return FW_CAMPAIGN_COMPLETION_PENDING;
    }
    return terminal_failure
        ? FW_CAMPAIGN_COMPLETION_TERMINAL_FAILURE
        : FW_CAMPAIGN_COMPLETION_SUCCESS;
}

#if defined(FOF_BADGE_VARIANT)
/**
 * Atomically classify the durable scanner campaign at a stable no-operation
 * epoch. Lock or persistence uncertainty returns UNKNOWN.
 */
fw_store_campaign_completion_t
fw_store_campaign_completion_sample(void);
#endif

typedef enum {
    FW_SCANNER_STAGE_IDLE = 0,
    FW_SCANNER_STAGE_RECEIVING,
    FW_SCANNER_STAGE_COMMITTED,
} fw_store_scanner_stage_phase_t;

typedef struct {
    fw_store_scanner_stage_phase_t phase;
    uint32_t generation;
    uint32_t size;
    uint32_t received;
    uint8_t slot_mask;
    char target[32];
    char sha256[FOF_FIRMWARE_SHA256_HEX_SIZE];
} fw_store_scanner_stage_status_t;

/*
 * A maintenance boot may restore a terminal campaign left by an older host
 * session.  That stale result must not evict the new host before it can stage
 * firmware.  Operation ownership already fences the commit transition, so a
 * different, durably committed generation is sufficient current-session
 * proof.
 */
static inline bool fw_store_campaign_terminal_exit_allowed(
    bool entry_snapshot_certain,
    uint32_t entry_generation,
    bool stage_snapshot_valid,
    const fw_store_scanner_stage_status_t *stage,
    fw_store_campaign_completion_t completion)
{
    return entry_snapshot_certain &&
           stage_snapshot_valid &&
           stage &&
           stage->phase == FW_SCANNER_STAGE_COMMITTED &&
           stage->generation != 0U &&
           stage->generation != entry_generation &&
           completion == FW_CAMPAIGN_COMPLETION_TERMINAL_FAILURE;
}

static inline bool fw_store_scanner_stage_text_is_bounded(
    const char *text, size_t capacity)
{
    if (!text || capacity == 0U || text[0] == '\0') {
        return false;
    }
    for (size_t i = 1U; i < capacity; ++i) {
        if (text[i] == '\0') {
            return true;
        }
    }
    return false;
}

static inline bool fw_store_scanner_stage_status_normalize(
    const fw_store_scanner_stage_status_t *input,
    fw_store_scanner_stage_status_t *out)
{
    if (!input || !out ||
        !fw_store_scanner_stage_text_is_bounded(
            input->target, sizeof(input->target)) ||
        !fof_firmware_sha256_hex_is_valid(input->sha256) ||
        input->size == 0U ||
        input->slot_mask == 0U ||
        (input->slot_mask & (uint8_t)~FW_AUTO_UPDATE_SLOT_ALL) != 0U ||
        input->received > input->size) {
        return false;
    }
    if (input->phase == FW_SCANNER_STAGE_RECEIVING) {
        if (input->generation != 0U ||
            input->received >= input->size) {
            return false;
        }
    } else if (input->phase == FW_SCANNER_STAGE_COMMITTED) {
        if (input->generation == 0U ||
            input->received != input->size) {
            return false;
        }
    } else {
        return false;
    }

    *out = *input;
    for (size_t i = 0U;
         i < FOF_FIRMWARE_SHA256_HEX_LENGTH; ++i) {
        char ch = out->sha256[i];
        if (ch >= 'A' && ch <= 'F') {
            out->sha256[i] = (char)(ch - 'A' + 'a');
        }
    }
    return true;
}

/**
 * Reconcile one stable live-parser sample with the exact durable manifest
 * read result. All false returns leave a canonical zero/IDLE output.
 */
static inline bool fw_store_scanner_stage_status_reconcile(
    bool live_sample_certain,
    const fw_store_scanner_stage_status_t *live_status,
    fw_store_read_result_t manifest_result,
    const fw_store_info_t *manifest,
    fw_store_scanner_stage_status_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!live_sample_certain) {
        return false;
    }
    if (live_status) {
        return fw_store_scanner_stage_status_normalize(
            live_status, out);
    }
    if (manifest_result == FW_STORE_READ_NO_MANIFEST) {
        return true;
    }
    if (manifest_result != FW_STORE_READ_COMMITTED ||
        !manifest || !manifest->stored) {
        return false;
    }

    fw_store_scanner_stage_status_t committed = {
        .phase = FW_SCANNER_STAGE_COMMITTED,
        .generation = manifest->generation,
        .size = manifest->size,
        .received = manifest->size,
        .slot_mask = manifest->target_slot_mask,
    };
    memcpy(committed.target, manifest->name,
           sizeof(committed.target));
    memcpy(committed.sha256, manifest->sha256,
           sizeof(committed.sha256));
    return fw_store_scanner_stage_status_normalize(
        &committed, out);
}

#if defined(FOF_BADGE_VARIANT)
/**
 * Take an exact, race-free scanner staging snapshot. NVS, shadow, or
 * consistency ambiguity clears out and returns false.
 */
bool fw_store_scanner_stage_status_snapshot(
    fw_store_scanner_stage_status_t *out);
#endif
#endif

typedef struct {
    bool worker_running;
    uint32_t generation;
    uint8_t target_slot_mask;
    uint8_t pending_mask;
    uint8_t attempts[FW_AUTO_UPDATE_SCANNER_COUNT];
    uint8_t readiness_probe_attempts[FW_AUTO_UPDATE_SCANNER_COUNT];
    char state[FW_AUTO_UPDATE_SCANNER_COUNT][FW_AUTO_UPDATE_STATE_SIZE];
} fw_auto_update_status_t;

/** Read staged scanner firmware metadata from the uplink store. */
bool fw_store_get_info(fw_store_info_t *out);

/**
 * Read one fully committed manifest and preserve the difference between an
 * absent/invalid record and an NVS or consistency failure.
 */
fw_store_read_result_t fw_store_read_committed(fw_store_info_t *out);

/**
 * Validate partition bytes against one caller-owned committed manifest
 * snapshot without rereading metadata or resolving a second partition.
 */
bool fw_store_validate_snapshot_image(
    const esp_partition_t *partition,
    const fw_store_info_t *snapshot,
    char *error,
    size_t error_len);

/** Atomic snapshot of the serialized automatic scanner-update queue. */
void fw_store_get_auto_update_status(fw_auto_update_status_t *out);

/**
 * Relay the staged scanner firmware to a scanner UART without HTTP.
 * scanner_id: 0 = BLE scanner slot, 1 = Wi-Fi scanner slot.
 * Writes a compact JSON result into out_json when provided.
 */
bool fw_store_relay_staged_to_scanner(int scanner_id,
                                      char *out_json,
                                      size_t out_json_len);
bool fw_store_relay_staged_to_scanner_ex(int scanner_id,
                                         bool force_probe_skip,
                                         bool allow_same_version,
                                         char *out_json,
                                         size_t out_json_len);
bool fw_store_relay_staged_to_scanner_bound(
    int scanner_id,
    uint32_t expected_generation,
    const char *expected_hardware_id,
    bool force_probe_skip,
    bool allow_same_version,
    char *out_json,
    size_t out_json_len);

/**
 * Pick the partition where staged scanner firmware is written. Returns the
 * same partition the /api/fw/upload handler uses. NULL if nothing suitable
 * (catastrophic on a sane partition table).
 *
 * Public so fw_auto_check (which downloads scanner firmware via HTTP from
 * the backend) can stage to the same place /api/fw/upload would.
 */
const esp_partition_t *fw_store_get_target_partition(void);

/**
 * Persist scanner firmware metadata to NVS so subsequent fw_store_get_info()
 * / fw_check / fw_offer flows see it.
 *
 * Caller has already written `size` bytes via esp_ota_write to `partition`
 * and called esp_ota_abort (intentionally NOT esp_ota_end — see the comment
 * in fw_upload_handler about why we never make scanner firmware bootable
 * for the uplink itself).
 */
bool fw_store_persist_metadata(const char *name, const char *version,
                               const esp_partition_t *partition,
                               uint32_t size, uint32_t crc32);

/**
 * USB serial staging path for badge builds. The caller sends exactly `size`
 * binary bytes after begin; the store computes CRC while writing and persists
 * metadata only when the complete image matches expected_crc32.
 */
bool fw_store_serial_upload_begin(const char *name,
                                  const char *version,
                                  uint32_t size,
                                  uint32_t expected_crc32,
                                  const char *expected_sha256,
                                  uint8_t target_slot_mask,
                                  bool credit_v1,
                                  char *out_json,
                                  size_t out_json_len);
bool fw_store_serial_upload_write(const uint8_t *data,
                                  size_t len,
                                  char *out_json,
                                  size_t out_json_len);
bool fw_store_serial_upload_end(char *out_json, size_t out_json_len);
bool fw_store_serial_upload_complete_terminal(bool delivered);
void fw_store_serial_upload_abort(const char *reason);
bool fw_store_serial_upload_active(void);
uint32_t fw_store_serial_upload_remaining(void);

/**
 * Create the coordinator mutex exactly once during single-threaded boot.
 * Must run before USB control or scanner RX tasks can enter fw_store.
 */
bool fw_store_init_auto_update_coordinator(void);

/**
 * Create the permanent static coordinator worker exactly once, after both
 * scanner UART RX tasks and their shared TX lease exist.
 */
bool fw_store_start_auto_update_coordinator(void);

/**
 * Restore the durable automatic-update coordinator after scanner UART RX is
 * running.  Interrupted relays retain their consumed attempt and resume only
 * when the committed staged manifest generation still matches.
 */
bool fw_store_restore_auto_update_coordinator(void);

/** Handle scanner-originated firmware negotiation messages. */
void fw_store_handle_scanner_check(int scanner_id,
                                   const char *scanner_board,
                                   const char *scanner_version,
                                   const char *check_reason);
bool fw_store_handle_scanner_ready(int scanner_id,
                                   const char *scanner_board,
                                   const char *scanner_version,
                                   const char *target_version,
                                   const char *target_name,
                                   const char *target_project,
                                   const char *target_hardware,
                                   const char *target_sha256,
                                   uint32_t target_generation,
                                   uint32_t target_size,
                                   uint32_t target_crc32);
bool fw_store_handle_legacy_scanner_ready(int scanner_id,
                                          const char *scanner_board,
                                          const char *scanner_version,
                                          const char *target_version,
                                          uint32_t target_size,
                                          uint32_t target_crc32);

#ifdef __cplusplus
}
#endif
