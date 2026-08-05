#ifndef BACKEND_OTA_MAINTENANCE_H
#define BACKEND_OTA_MAINTENANCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_ota_identity.h"
#include "backend_ota_journal.h"
#include "backend_firmware_buffer.h"
#include "firmware_version_order.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct backend_ota_maintenance backend_ota_maintenance_t;

typedef struct {
    backend_ota_component_t component;
    int8_t component_slot;
    uint8_t target_mac[6];
    uint32_t target_boot_id;
    uint32_t topology_generation;
} backend_ota_target_binding_t;

typedef struct {
    bool probe;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    bool has_operation_id;
    backend_ota_operation_id_t operation_id;
    uint32_t expected_size;
    bool has_accepted_probe_receipt;
    uint8_t accepted_probe_receipt_sha256[32];
    uint32_t command_next_sequence;
#endif
    backend_ota_component_t component;
    char catalog_name[40];
    char expected_sha256[65];
    backend_ota_apply_mode_t apply_mode;
    uint8_t expected_mac[6];
    uint32_t expected_boot_id;
    uint32_t expected_topology_generation;
} backend_ota_request_t;

typedef enum {
    BACKEND_OTA_DECISION_ADMIT = 0,
    BACKEND_OTA_DECISION_NO_UPDATE,
    BACKEND_OTA_DECISION_REJECT_IDENTITY,
    BACKEND_OTA_DECISION_REJECT_VERSION,
    BACKEND_OTA_DECISION_REJECT_DIGEST,
    BACKEND_OTA_DECISION_REJECT_SIZE,
    BACKEND_OTA_DECISION_REJECT_CAPACITY,
    BACKEND_OTA_DECISION_REJECT_BUSY,
    BACKEND_OTA_DECISION_REJECT_TARGET_BINDING,
    BACKEND_OTA_DECISION_APPLIED,
    BACKEND_OTA_DECISION_FAILED,
} backend_ota_decision_t;

typedef struct {
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    bool has_operation_id;
#endif
    backend_ota_operation_id_t operation_id;
    bool probe;
    backend_ota_component_t component;
    backend_ota_apply_mode_t apply_mode;
    uint8_t uplink_mac[6];
    char catalog_name[40];
    backend_ota_manifest_t manifest;
    backend_ota_decision_t decision;
    size_t partition_capacity;
    int8_t component_slot;
    uint8_t expected_target_mac[6];
    uint8_t actual_target_mac[6];
    uint32_t expected_target_boot_id;
    uint32_t actual_target_boot_id;
    uint32_t expected_topology_generation;
    uint32_t actual_topology_generation;
    bool complete_image_validated;
    uint32_t image_writes_before;
    uint32_t image_writes_after;
    uint32_t boot_id_before;
    uint32_t boot_id_after;
    bool rollback_clear;
    bool converged;
} backend_ota_evidence_t;

typedef struct {
    backend_ota_target_binding_t binding;
    bool identity_exact;
    bool command_ingress_healthy;
    bool role_acked;
    bool profile_correct;
    bool radio_healthy;
    bool rollback_clear;
} backend_ota_convergence_t;

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
#define BACKEND_OTA_PROGRESS_MAX_RETRIES 16U

/* Pointer-free worker-to-client handoff.  The client validates this against
 * the active immutable command before it makes an event durable. */
typedef struct {
    bool has_operation_id;
    backend_ota_operation_id_t operation_id;
    bool probe;
    backend_ota_component_t component;
    char catalog_name[40];
    backend_ota_manifest_t manifest;
    backend_ota_journal_progress_stage_t stage;
    uint32_t received;
    uint32_t total;
    uint32_t retry_count;
} backend_ota_progress_update_t;
#endif

typedef struct {
    void *context;
    bool (*fetch_metadata)(
        void *context,
        const char *catalog_name,
        char *json,
        size_t capacity,
        size_t *out_length,
        uint32_t *out_catalog_generation);
    bool (*download_image)(
        void *context,
        const char *catalog_name,
        uint8_t *destination,
        size_t capacity,
        size_t expected_size,
        size_t *out_size);
    const char *(*running_version)(
        void *context, backend_ota_component_t component);
    size_t (*partition_capacity)(
        void *context, backend_ota_component_t component);
    uint32_t (*image_write_count)(void *context);
    bool (*snapshot_binding)(
        void *context,
        backend_ota_component_t component,
        backend_ota_target_binding_t *out);
    bool (*acquire_target_claim)(
        void *context, backend_ota_component_t component);
    void (*release_target_claim)(
        void *context, backend_ota_component_t component);
    backend_ota_image_result_t (*validate_staged_image)(
        void *context,
        const backend_ota_manifest_t *manifest,
        backend_image_kind_t expected_kind,
        const uint8_t *bytes,
        size_t length);
    bool (*scanner_dry_run)(
        void *context,
        backend_ota_component_t component,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        bool has_operation_id,
        const backend_ota_operation_id_t *operation_id,
#endif
        const backend_ota_manifest_t *manifest,
        const uint8_t *bytes,
        size_t length);
    bool (*mutate_staged_image)(
        void *context,
        backend_ota_component_t component,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        bool has_operation_id,
        const backend_ota_operation_id_t *operation_id,
#endif
        const backend_ota_manifest_t *manifest,
        const uint8_t *bytes,
        size_t length);
    bool (*request_reboot)(
        void *context, backend_ota_component_t component);
    bool (*read_convergence)(
        void *context,
        backend_ota_component_t component,
        const backend_ota_manifest_t *manifest,
        backend_ota_convergence_t *out);
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    bool (*report_progress)(
        void *context, const backend_ota_progress_update_t *update);
    uint32_t (*relay_retry_count)(
        void *context, backend_ota_component_t component);
#endif
    bool (*emit_and_flush)(
        void *context, const char *line, size_t length);
} backend_ota_maintenance_adapters_t;

#define BACKEND_OTA_MAINTENANCE_METADATA_CAPACITY 4096U
#define BACKEND_OTA_MAINTENANCE_EVIDENCE_CAPACITY 2048U
#define BACKEND_OTA_POLL_INTERVAL_MS INT64_C(1800000)

typedef struct {
    int64_t next_due_ms;
    uint8_t failure_count;
    bool initialized;
} backend_ota_poll_state_t;

typedef enum {
    BACKEND_OTA_AUTO_NO_UPDATE = 0,
    BACKEND_OTA_AUTO_READ_ONLY_UPDATE_AVAILABLE,
    BACKEND_OTA_AUTO_APPLY_NEWER,
    BACKEND_OTA_AUTO_REJECT_VERSION,
} backend_ota_auto_decision_t;

void backend_ota_poll_init(backend_ota_poll_state_t *state, int64_t now_ms);
bool backend_ota_poll_due(
    const backend_ota_poll_state_t *state, int64_t now_ms);
void backend_ota_poll_note_failure(
    backend_ota_poll_state_t *state, int64_t now_ms);
void backend_ota_poll_note_success(
    backend_ota_poll_state_t *state, int64_t now_ms);
backend_ota_auto_decision_t backend_ota_auto_policy(
    bool auto_update_enabled, fof_firmware_version_relation_t relation);

struct backend_ota_maintenance {
    backend_ota_maintenance_adapters_t adapters;
    backend_ota_journal_storage_t journal_storage;
    backend_firmware_buffer_t *firmware_buffer;
    uint8_t uplink_mac[6];
    uint32_t uplink_boot_id;
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    uint32_t next_operation_id;
#endif
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    bool restart_authorized;
    backend_ota_journal_record_t restart_record;
#endif
    bool initialized;
    bool busy;
    bool buffer_owned;
    backend_ota_evidence_t last_evidence;
    bool has_last_evidence;
    char metadata[BACKEND_OTA_MAINTENANCE_METADATA_CAPACITY + 1U];
    char evidence_line[BACKEND_OTA_MAINTENANCE_EVIDENCE_CAPACITY];
};

bool backend_ota_maintenance_init(
    backend_ota_maintenance_t *state,
    const backend_ota_maintenance_adapters_t *adapters,
    const backend_ota_journal_storage_t *journal_storage,
    backend_firmware_buffer_t *firmware_buffer,
    const uint8_t uplink_mac[6],
    uint32_t uplink_boot_id);

void backend_ota_maintenance_on_boot(
    backend_ota_maintenance_t *state, uint32_t uplink_boot_id);

bool backend_ota_maintenance_parse_usb(
    const char *line, size_t length, backend_ota_request_t *out);

bool backend_ota_maintenance_is_status_usb(
    const char *line, size_t length);

bool backend_ota_target_binding_matches(
    const backend_ota_request_t *request,
    const backend_ota_target_binding_t *actual);

const char *backend_ota_component_catalog_name(
    backend_ota_component_t component);
int8_t backend_ota_component_slot(backend_ota_component_t component);

/* Encodes the complete USB evidence line without a trailing newline. */
size_t backend_ota_evidence_encode(
    const backend_ota_evidence_t *evidence, char *output, size_t capacity);

size_t backend_ota_accepted_encode(
    const backend_ota_journal_record_t *accepted,
    char *output,
    size_t capacity);

/* The stateful probe/apply worker is implemented below the pure USB contract. */
bool backend_ota_maintenance_run_probe(
    backend_ota_maintenance_t *state,
    backend_ota_component_t component,
    const char *catalog_name,
    const char *expected_sha256_or_null,
    backend_ota_evidence_t *out);

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
/* Fullsize server commands select their mode explicitly.  The existing
 * run_probe API remains Lite's byte-for-byte newer-only path. */
bool backend_ota_maintenance_run_fullsize_probe(
    backend_ota_maintenance_t *state,
    bool has_operation_id,
    const backend_ota_operation_id_t *operation_id,
    uint32_t expected_size,
    backend_ota_component_t component,
    const char *catalog_name,
    const char *expected_sha256_or_null,
    backend_ota_apply_mode_t apply_mode,
    backend_ota_evidence_t *out);

/* Durably records the complete pointer-free command binding before the
 * command is queued or any metadata/download work starts. */
backend_ota_journal_persist_result_t
backend_ota_maintenance_accept_fullsize_command(
    backend_ota_maintenance_t *state,
    const backend_ota_request_t *request);

bool backend_ota_maintenance_restart_fullsize_command(
    backend_ota_maintenance_t *state,
    const backend_ota_request_t *request,
    backend_ota_evidence_t *out);

/* Persist the exact progress body state before enqueue, then durably advance
 * its sequence after a strict ACK and before the outbox tombstone. */
bool backend_ota_maintenance_persist_fullsize_progress(
    backend_ota_maintenance_t *state,
    const backend_ota_request_t *request,
    const backend_ota_progress_update_t *update,
    uint32_t event_sequence);

bool backend_ota_maintenance_ack_fullsize_progress(
    backend_ota_maintenance_t *state,
    const backend_ota_request_t *request,
    const backend_ota_progress_update_t *update,
    uint32_t accepted_sequence,
    uint32_t next_sequence);

bool backend_ota_maintenance_persist_fullsize_terminal(
    backend_ota_maintenance_t *state,
    const backend_ota_evidence_t *evidence,
    uint32_t event_sequence,
    bool complete,
    bool has_accepted_probe_receipt,
    const uint8_t accepted_probe_receipt_sha256[32]);
#endif

bool backend_ota_maintenance_request_apply(
    backend_ota_maintenance_t *state,
    const backend_ota_request_t *request);

bool backend_ota_maintenance_auto_poll(
    backend_ota_maintenance_t *state,
    backend_ota_component_t component,
    bool auto_update_enabled,
    backend_ota_auto_decision_t *out_decision);

bool backend_ota_maintenance_resume(
    backend_ota_maintenance_t *state, bool convergence_deadline_expired);

bool backend_ota_maintenance_emit_status(backend_ota_maintenance_t *state);

bool backend_ota_maintenance_last_evidence(
    const backend_ota_maintenance_t *state,
    backend_ota_evidence_t *out);

bool backend_ota_maintenance_available(
    const backend_ota_maintenance_t *state);

#ifdef __cplusplus
}
#endif

#endif
