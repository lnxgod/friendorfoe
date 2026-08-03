#ifndef BACKEND_OTA_COMMAND_CLIENT_H
#define BACKEND_OTA_COMMAND_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_ota_maintenance.h"
#include "backend_ota_operation_id.h"

#if !defined(FOF_BACKEND_PROFILE_S3_FULLSIZE) || \
    defined(FOF_BACKEND_PROFILE_BADGE_LITE)
#error "backend_ota_command_client is available only on the Fullsize profile"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_OTA_COMMAND_MAX_JSON 2048U
#define BACKEND_OTA_RECEIPT_PREIMAGE_MAX_BYTES 1024U

typedef struct {
    uint8_t uplink_mac[6];
    uint32_t uplink_boot_id;
    uint8_t target_mac[6];
    uint32_t target_boot_id;
    uint32_t topology_generation;
} backend_ota_command_binding_t;

typedef struct {
    bool is_apply;
    bool has_operation_id;
    backend_ota_operation_id_t operation_id;
    backend_ota_component_t component;
    char catalog_name[40];
    char expected_sha256[65];
    uint32_t expected_size;
    backend_ota_command_binding_t binding;
    backend_ota_apply_mode_t apply_mode;
    uint32_t next_sequence;
    char probe_receipt_sha256[65];
} backend_ota_command_envelope_t;

typedef struct {
    backend_ota_operation_id_t operation_id;
    backend_ota_component_t component;
    char catalog_name[40];
    uint8_t expected_sha256[32];
    uint32_t expected_size;
    backend_ota_command_binding_t binding;
    backend_ota_apply_mode_t apply_mode;
} backend_ota_probe_binding_t;

typedef struct {
    backend_ota_probe_binding_t probe;
    uint8_t receipt_sha256[32];
    uint32_t apply_start_sequence;
} backend_ota_accepted_probe_t;

typedef struct {
    backend_ota_component_t component;
    const char *catalog_name;
    const char *target;
    const char *project;
    const char *hardware;
    backend_ota_command_binding_t binding;
    /* The caller's cache limit is an explicit boundary for metadata rechecks. */
    uint32_t max_expected_size;
    bool has_expected_next_sequence;
    uint32_t expected_next_sequence;
    /* Presence is explicit because an all-zero opaque operation ID is valid. */
    bool has_accepted_probe;
    backend_ota_accepted_probe_t accepted_probe;
} backend_ota_command_local_t;

typedef enum {
    BACKEND_OTA_COMMAND_DECODE_OK = 0,
    BACKEND_OTA_COMMAND_DECODE_MALFORMED,
    BACKEND_OTA_COMMAND_DECODE_SCHEMA,
    BACKEND_OTA_COMMAND_DECODE_TOO_LARGE,
    BACKEND_OTA_COMMAND_DECODE_BINDING,
} backend_ota_command_decode_result_t;

typedef struct {
    bool ok;
    bool has_operation_id;
    backend_ota_operation_id_t operation_id;
    uint32_t accepted_sequence;
    uint32_t next_sequence;
    backend_ota_component_t current_component;
    char current_action[24];
    bool terminal;
    bool duplicate;
} backend_ota_command_ack_t;

bool backend_ota_accepted_probe_capture(
    const backend_ota_command_envelope_t *probe,
    const char accepted_receipt_sha256[65],
    uint32_t probe_end_sequence,
    const backend_ota_command_ack_t *transition_ack,
    backend_ota_accepted_probe_t *out);

bool backend_ota_apply_matches_accepted_probe(
    const backend_ota_command_envelope_t *apply,
    const backend_ota_accepted_probe_t *accepted,
    uint32_t expected_next_sequence);

backend_ota_command_decode_result_t backend_ota_command_decode(
    const char *json,
    size_t length,
    const backend_ota_command_local_t *local,
    backend_ota_command_envelope_t *out);

bool backend_ota_command_ack_decode(
    const char *json, size_t length,
    const backend_ota_operation_id_t *expected_operation_id,
    uint32_t expected_sequence,
    backend_ota_component_t expected_component,
    const char *expected_action,
    bool expected_terminal,
    backend_ota_command_ack_t *out);

typedef enum {
    BACKEND_OTA_PROGRESS_METADATA = 0,
    BACKEND_OTA_PROGRESS_DOWNLOAD,
    BACKEND_OTA_PROGRESS_VALIDATE,
    BACKEND_OTA_PROGRESS_STAGE,
    BACKEND_OTA_PROGRESS_UART_RELAY,
    BACKEND_OTA_PROGRESS_REBOOT_WAIT,
    BACKEND_OTA_PROGRESS_CONVERGENCE,
} backend_ota_progress_stage_t;

typedef struct {
    bool initialized;
    backend_ota_progress_stage_t stage;
    uint32_t received;
    uint32_t total;
    uint32_t retry_count;
} backend_ota_progress_state_t;

typedef enum {
    BACKEND_OTA_TERMINAL_ELIGIBLE = 0,
    BACKEND_OTA_TERMINAL_NO_UPDATE,
    BACKEND_OTA_TERMINAL_APPLIED,
    BACKEND_OTA_TERMINAL_FAILED,
    BACKEND_OTA_TERMINAL_ROLLED_BACK,
} backend_ota_terminal_outcome_t;

typedef enum {
    BACKEND_OTA_TERMINAL_ERROR_NONE = 0,
    BACKEND_OTA_TERMINAL_ERROR_IDENTITY_MISMATCH,
    BACKEND_OTA_TERMINAL_ERROR_STALE_BINDING,
    BACKEND_OTA_TERMINAL_ERROR_CAPACITY,
    BACKEND_OTA_TERMINAL_ERROR_DOWNLOAD,
    BACKEND_OTA_TERMINAL_ERROR_HASH_MISMATCH,
    BACKEND_OTA_TERMINAL_ERROR_UART,
    BACKEND_OTA_TERMINAL_ERROR_REBOOT_TIMEOUT,
    BACKEND_OTA_TERMINAL_ERROR_HEALTH,
    BACKEND_OTA_TERMINAL_ERROR_ROLLBACK,
    BACKEND_OTA_TERMINAL_ERROR_INTERNAL,
} backend_ota_terminal_error_t;

typedef struct {
    backend_ota_terminal_outcome_t outcome;
    backend_ota_terminal_error_t error;
    backend_ota_manifest_t candidate;
    fof_firmware_version_relation_t relation;
    bool complete_image_validated;
    uint32_t validated_image_bytes;
    /* Canonical applied-byte evidence, never a low-level write-call count. */
    uint32_t image_writes;
    backend_ota_target_binding_t actual_binding;
    bool identity_exact;
    bool command_ingress_healthy;
    bool role_acked;
    bool profile_correct;
    bool radio_healthy;
    bool rollback_clear;
    bool has_observed_failure_identity;
    char observed_target[65];
    char observed_project[65];
    char observed_hardware[65];
    char observed_version[65];
} backend_ota_terminal_evidence_t;

typedef struct {
    size_t body_length;
    char receipt_sha256[65];
} backend_ota_built_end_t;

bool backend_ota_progress_accept(
    backend_ota_progress_state_t *state,
    backend_ota_progress_stage_t stage,
    uint32_t received,
    uint32_t total,
    uint32_t retry_count);

typedef struct {
    bool has_operation_id;
    backend_ota_operation_id_t operation_id;
    bool is_apply;
    uint32_t sequence;
    backend_ota_component_t component;
    const char *catalog_name;
} backend_ota_event_prefix_t;

typedef struct {
    backend_ota_event_prefix_t prefix;
    backend_ota_progress_stage_t stage;
    uint32_t received;
    uint32_t total;
    uint32_t retry_count;
} backend_ota_progress_event_t;

size_t backend_ota_event_begin_encode(
    const backend_ota_event_prefix_t *event, char *out, size_t capacity);
size_t backend_ota_event_progress_encode(
    backend_ota_progress_state_t *state,
    const backend_ota_progress_event_t *event,
    char *out,
    size_t capacity);

bool backend_ota_event_end_build(
    const backend_ota_command_envelope_t *immutable_command,
    const backend_ota_progress_state_t *last_progress,
    uint32_t sequence,
    const backend_ota_terminal_evidence_t *evidence,
    char *body,
    size_t capacity,
    backend_ota_built_end_t *out);

typedef enum {
    BACKEND_OTA_PROBE_ELIGIBLE = 0,
    BACKEND_OTA_PROBE_NO_UPDATE,
    BACKEND_OTA_PROBE_REJECTED,
} backend_ota_probe_mode_result_t;

backend_ota_probe_mode_result_t backend_ota_fullsize_probe_mode(
    fof_firmware_version_relation_t relation,
    backend_ota_apply_mode_t mode);

#if defined(UNIT_TESTING)
/* Golden-vector API only; production terminal generation uses the bound builder. */
typedef struct {
    bool is_apply;
    backend_ota_component_t component;
    const char *catalog_name;
    const char *expected_sha256;
    uint32_t expected_size;
    backend_ota_command_binding_t binding;
    backend_ota_apply_mode_t apply_mode;
} backend_ota_receipt_command_t;

typedef struct {
    const char *state;
    const char *decision;
    const char *error;
    /* Context only: an early probe failed before target identity was available. */
    bool failed_before_identity;
    uint32_t image_writes;
    const char *target;
    const char *project;
    const char *hardware;
    const char *version;
    uint8_t actual_mac[6];
    uint32_t actual_boot_id;
    uint32_t actual_topology_generation;
    bool role_healthy;
    bool radio_healthy;
    bool rollback_clear;
} backend_ota_receipt_end_t;

size_t backend_ota_receipt_v1_preimage(
    const backend_ota_operation_id_t *operation_id,
    const backend_ota_receipt_command_t *command,
    const backend_ota_receipt_end_t *end,
    uint8_t *out,
    size_t capacity);
bool backend_ota_receipt_v1_sha256(
    const backend_ota_operation_id_t *operation_id,
    const backend_ota_receipt_command_t *command,
    const backend_ota_receipt_end_t *end,
    char out_sha256[65]);
bool backend_ota_receipt_v1_verify(
    const backend_ota_operation_id_t *operation_id,
    const backend_ota_receipt_command_t *command,
    const backend_ota_receipt_end_t *end,
    const char *expected_sha256);
#endif

#ifdef __cplusplus
}
#endif

#endif
