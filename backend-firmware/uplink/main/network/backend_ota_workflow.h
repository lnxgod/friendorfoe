#ifndef BACKEND_OTA_WORKFLOW_H
#define BACKEND_OTA_WORKFLOW_H

#include <stdbool.h>
#include <stdint.h>

#include "backend_ota_command_client.h"

#if !defined(FOF_BACKEND_PROFILE_S3_FULLSIZE) || \
    defined(FOF_BACKEND_PROFILE_BADGE_LITE)
#error "backend OTA workflow requires the S3 Fullsize profile"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BACKEND_OTA_WORKFLOW_ADMITTED = 0,
    BACKEND_OTA_WORKFLOW_DUPLICATE,
    BACKEND_OTA_WORKFLOW_BUSY,
    BACKEND_OTA_WORKFLOW_CONFLICT,
} backend_ota_workflow_admit_result_t;

typedef struct {
    backend_ota_component_t component;
    bool is_apply;
    bool terminal;
} backend_ota_workflow_ack_prediction_t;

typedef struct {
    bool has_rollout_operation;
    backend_ota_operation_id_t rollout_operation_id;
    backend_ota_component_t expected_component;
    bool expected_apply;
    bool has_expected_sequence;
    uint32_t expected_sequence;
    bool has_accepted_probe;
    backend_ota_accepted_probe_t accepted_probe;
    uint8_t completed_scanner_mask;
    bool command_active;
    bool work_available;
    bool work_inflight;
    bool begin_acked;
    backend_ota_command_envelope_t command;
    backend_ota_progress_state_t progress;
} backend_ota_workflow_t;

void backend_ota_workflow_init(backend_ota_workflow_t *state);

backend_ota_workflow_admit_result_t backend_ota_workflow_admit(
    backend_ota_workflow_t *state,
    const backend_ota_command_envelope_t *command);

bool backend_ota_workflow_take_work(
    backend_ota_workflow_t *state,
    backend_ota_command_envelope_t *out);

bool backend_ota_workflow_predict_begin_ack(
    const backend_ota_workflow_t *state,
    backend_ota_workflow_ack_prediction_t *out);

bool backend_ota_workflow_note_begin_ack(
    backend_ota_workflow_t *state,
    const backend_ota_command_ack_t *ack);

bool backend_ota_workflow_predict_progress_ack(
    const backend_ota_workflow_t *state,
    backend_ota_workflow_ack_prediction_t *out);

/* Call only after the strict server ACK, the acknowledged progress/sequence
 * advance is durable in the journal, and the matching outbox body is durably
 * tombstoned.  A rejected event or ACK leaves all workflow state unchanged. */
bool backend_ota_workflow_note_progress_ack(
    backend_ota_workflow_t *state,
    const backend_ota_progress_event_t *event,
    const backend_ota_command_ack_t *ack);

bool backend_ota_workflow_predict_terminal_ack(
    const backend_ota_workflow_t *state,
    backend_ota_terminal_outcome_t outcome,
    backend_ota_workflow_ack_prediction_t *out);

/* Call only after a strict server ACK and durable outbox tombstone. */
bool backend_ota_workflow_note_terminal_ack(
    backend_ota_workflow_t *state,
    backend_ota_terminal_outcome_t outcome,
    const char receipt_sha256[65],
    const backend_ota_command_ack_t *ack);

#ifdef __cplusplus
}
#endif

#endif
