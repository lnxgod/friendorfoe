#ifndef BACKEND_SCANNER_RELAY_H
#define BACKEND_SCANNER_RELAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_detection_codec.h"
#include "backend_firmware_store.h"
#include "backend_scanner_control_codec.h"
#include "backend_scanner_status_codec.h"
#include "backend_uart_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_SCANNER_RELAY_RESPONSE_TIMEOUT_MS INT64_C(5000)
#define BACKEND_SCANNER_RELAY_CONVERGENCE_TIMEOUT_MS INT64_C(60000)
#define BACKEND_SCANNER_RELAY_MAX_RETRIES 3U
#define BACKEND_SCANNER_RELAY_MAX_FRAME \
    (OTA_CHUNK_HEADER_SIZE + OTA_CHUNK_MAX_DATA + OTA_CHUNK_CRC_SIZE)

typedef enum {
    BACKEND_SCANNER_RELAY_IDLE = 0,
    BACKEND_SCANNER_RELAY_QUIET_REQUESTED,
    BACKEND_SCANNER_RELAY_BEGIN_SENT,
    BACKEND_SCANNER_RELAY_STREAMING,
    BACKEND_SCANNER_RELAY_IMAGE_STAGED,
    BACKEND_SCANNER_RELAY_END_SENT,
    BACKEND_SCANNER_RELAY_REBOOT_WAIT,
    BACKEND_SCANNER_RELAY_CONVERGENCE_WAIT,
    BACKEND_SCANNER_RELAY_ABORT_REQUESTED,
    BACKEND_SCANNER_RELAY_RESTORE_REQUESTED,
    BACKEND_SCANNER_RELAY_RESTORE_WAIT,
    BACKEND_SCANNER_RELAY_COMPLETE,
    BACKEND_SCANNER_RELAY_FAILED,
} backend_scanner_relay_state_kind_t;

typedef enum {
    BACKEND_SCANNER_RELAY_ACTION_NONE = 0,
    BACKEND_SCANNER_RELAY_ACTION_SEND_QUIET,
    BACKEND_SCANNER_RELAY_ACTION_SEND_BEGIN,
    BACKEND_SCANNER_RELAY_ACTION_SEND_CHUNK,
    BACKEND_SCANNER_RELAY_ACTION_SEND_END,
    BACKEND_SCANNER_RELAY_ACTION_REQUEST_STATUS,
    BACKEND_SCANNER_RELAY_ACTION_SEND_ABORT,
    BACKEND_SCANNER_RELAY_ACTION_SEND_RESTORE,
} backend_scanner_relay_action_kind_t;

typedef struct {
    backend_scanner_relay_action_kind_t kind;
    backend_scanner_slot_t slot;
    uint32_t session_id;
    uint32_t generation;
    uint32_t topology_generation;
    backend_scanner_control_t control;
    uint32_t sequence;
    uint32_t next_sequence;
    size_t image_offset;
    size_t image_length;
    uint8_t frame[BACKEND_SCANNER_RELAY_MAX_FRAME];
    size_t frame_length;
    bool dry_run;
} backend_scanner_relay_action_t;

typedef enum {
    BACKEND_SCANNER_RELAY_RECEIPT_QUIET_ACK = 0,
    BACKEND_SCANNER_RELAY_RECEIPT_ACK,
    BACKEND_SCANNER_RELAY_RECEIPT_NACK,
    BACKEND_SCANNER_RELAY_RECEIPT_STAGED,
    BACKEND_SCANNER_RELAY_RECEIPT_DONE,
    BACKEND_SCANNER_RELAY_RECEIPT_ERROR,
} backend_scanner_relay_receipt_kind_t;

typedef struct {
    backend_scanner_relay_receipt_kind_t kind;
    uint32_t session_id;
    uint32_t generation;
    uint32_t sequence;
    uint32_t next_sequence;
    uint32_t received;
    bool dry_run;
    char reason[48];
} backend_scanner_relay_receipt_t;

typedef enum {
    BACKEND_SCANNER_RELAY_EVENT_ACCEPTED = 0,
    BACKEND_SCANNER_RELAY_EVENT_RETRY_SCHEDULED,
    BACKEND_SCANNER_RELAY_EVENT_IGNORED_STALE,
    BACKEND_SCANNER_RELAY_EVENT_WAITING,
    BACKEND_SCANNER_RELAY_EVENT_COMPLETE,
    BACKEND_SCANNER_RELAY_EVENT_FAILED,
    BACKEND_SCANNER_RELAY_EVENT_INVALID_ARGUMENT,
    BACKEND_SCANNER_RELAY_EVENT_INVALID_TRANSITION,
} backend_scanner_relay_event_result_t;

typedef struct {
    backend_scanner_relay_state_kind_t state;
    backend_firmware_store_t *store;
    backend_ota_manifest_t manifest;
    backend_scanner_slot_t slot;
    backend_scan_profile_t expected_profile;
    uint8_t expected_mac[6];
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    backend_ota_operation_id_t operation_id;
    bool has_operation_id;
    uint32_t session_generation;
#endif
    uint32_t session_id;
    uint32_t generation;
    uint32_t old_boot_id;
    uint32_t new_boot_id;
    uint32_t expected_topology_generation;
    uint32_t expected_role_generation;
    uint32_t next_sequence;
    size_t acknowledged_bytes;
    uint8_t retry_count;
    bool dry_run;
    bool quiet_sent;
    bool remote_begin_sent;
    bool cleanup_success;
    bool awaiting_receipt;
    bool action_pending;
    bool retry_pending;
    int64_t response_deadline_ms;
    int64_t convergence_deadline_ms;
    int64_t next_status_poll_ms;
    backend_scanner_relay_action_t pending_action;
    backend_scanner_relay_action_t last_action;
} backend_scanner_relay_t;

void backend_scanner_relay_init(backend_scanner_relay_t *relay);

bool backend_scanner_relay_can_begin(
    backend_scanner_slot_t slot,
    const backend_ota_manifest_t *manifest,
    const uint8_t expected_mac[6],
    uint32_t generation);

bool backend_scanner_relay_begin(
    backend_scanner_relay_t *relay,
    backend_firmware_store_t *store,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    bool has_operation_id,
    const backend_ota_operation_id_t *operation_id,
    uint32_t session_generation,
#endif
    backend_scanner_slot_t slot,
    const backend_ota_manifest_t *manifest,
    const uint8_t expected_mac[6],
    uint32_t session_id,
    uint32_t generation,
    uint32_t old_boot_id,
    uint32_t expected_topology_generation,
    backend_scan_profile_t expected_profile,
    uint32_t expected_role_generation,
    bool dry_run);

bool backend_scanner_relay_take_action(
    backend_scanner_relay_t *relay,
    int64_t now_ms,
    backend_scanner_relay_action_t *out);

backend_scanner_relay_event_result_t backend_scanner_relay_receive(
    backend_scanner_relay_t *relay,
    const backend_scanner_relay_receipt_t *receipt,
    int64_t now_ms);

backend_scanner_relay_event_result_t backend_scanner_relay_tick(
    backend_scanner_relay_t *relay,
    int64_t now_ms);

backend_scanner_relay_event_result_t backend_scanner_relay_on_status(
    backend_scanner_relay_t *relay,
    const backend_scanner_status_t *status,
    uint32_t live_topology_generation,
    int64_t now_ms);

backend_scanner_relay_event_result_t backend_scanner_relay_abort(
    backend_scanner_relay_t *relay,
    int64_t now_ms);

bool backend_scanner_relay_reset(backend_scanner_relay_t *relay);

#ifdef __cplusplus
}
#endif

#endif
