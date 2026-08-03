#include "backend_ota_workflow.h"

#include <string.h>

static bool command_equal(
    const backend_ota_command_envelope_t *left,
    const backend_ota_command_envelope_t *right)
{
    return left != NULL && right != NULL &&
           left->is_apply == right->is_apply &&
           left->has_operation_id == right->has_operation_id &&
           backend_ota_operation_id_equal(
               &left->operation_id, &right->operation_id) &&
           left->component == right->component &&
           strcmp(left->catalog_name, right->catalog_name) == 0 &&
           strcmp(left->expected_sha256, right->expected_sha256) == 0 &&
           left->expected_size == right->expected_size &&
           memcmp(&left->binding, &right->binding,
                  sizeof(left->binding)) == 0 &&
           left->apply_mode == right->apply_mode &&
           left->next_sequence == right->next_sequence &&
           strcmp(left->probe_receipt_sha256,
                  right->probe_receipt_sha256) == 0;
}

static bool ack_matches(
    const backend_ota_command_ack_t *ack,
    const backend_ota_operation_id_t *operation_id,
    backend_ota_workflow_ack_prediction_t expected,
    uint32_t accepted_sequence)
{
    const char *action = expected.is_apply ? "apply" : "probe";
    return ack != NULL && ack->ok && ack->has_operation_id &&
           backend_ota_operation_id_equal(
               &ack->operation_id, operation_id) &&
           ack->accepted_sequence == accepted_sequence &&
           accepted_sequence != UINT32_MAX &&
           ack->next_sequence == accepted_sequence + 1U &&
           ack->current_component == expected.component &&
           strcmp(ack->current_action, action) == 0 &&
           ack->terminal == expected.terminal;
}

void backend_ota_workflow_init(backend_ota_workflow_t *state)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->expected_component = BACKEND_OTA_COMPONENT_SCANNER0;
}

backend_ota_workflow_admit_result_t backend_ota_workflow_admit(
    backend_ota_workflow_t *state,
    const backend_ota_command_envelope_t *command)
{
    if (state == NULL || command == NULL || !command->has_operation_id) {
        return BACKEND_OTA_WORKFLOW_CONFLICT;
    }
    if (state->command_active) {
        return command_equal(&state->command, command)
            ? BACKEND_OTA_WORKFLOW_DUPLICATE
            : BACKEND_OTA_WORKFLOW_BUSY;
    }
    if (command->component != state->expected_component ||
        command->is_apply != state->expected_apply ||
        (state->has_expected_sequence &&
         command->next_sequence != state->expected_sequence)) {
        return BACKEND_OTA_WORKFLOW_CONFLICT;
    }
    if (state->has_rollout_operation) {
        if (!backend_ota_operation_id_equal(
                &state->rollout_operation_id, &command->operation_id)) {
            return BACKEND_OTA_WORKFLOW_CONFLICT;
        }
    } else if (command->component != BACKEND_OTA_COMPONENT_SCANNER0 ||
               command->is_apply) {
        return BACKEND_OTA_WORKFLOW_CONFLICT;
    }
    if (command->is_apply &&
        (!state->has_accepted_probe ||
         !backend_ota_apply_matches_accepted_probe(
             command, &state->accepted_probe, state->expected_sequence))) {
        return BACKEND_OTA_WORKFLOW_CONFLICT;
    }
    if (!command->is_apply && command->probe_receipt_sha256[0] != '\0') {
        return BACKEND_OTA_WORKFLOW_CONFLICT;
    }
    if (!state->has_rollout_operation) {
        state->has_rollout_operation = true;
        state->rollout_operation_id = command->operation_id;
    }
    state->command = *command;
    memset(&state->progress, 0, sizeof(state->progress));
    state->command_active = true;
    state->work_available = true;
    state->work_inflight = false;
    state->begin_acked = false;
    state->expected_sequence = command->next_sequence;
    state->has_expected_sequence = true;
    return BACKEND_OTA_WORKFLOW_ADMITTED;
}

bool backend_ota_workflow_take_work(
    backend_ota_workflow_t *state,
    backend_ota_command_envelope_t *out)
{
    if (state == NULL || out == NULL || !state->command_active ||
        !state->work_available || state->work_inflight) {
        return false;
    }
    *out = state->command;
    state->work_available = false;
    state->work_inflight = true;
    return true;
}

bool backend_ota_workflow_predict_begin_ack(
    const backend_ota_workflow_t *state,
    backend_ota_workflow_ack_prediction_t *out)
{
    if (state == NULL || out == NULL || !state->command_active ||
        state->begin_acked) {
        return false;
    }
    *out = (backend_ota_workflow_ack_prediction_t) {
        .component = state->command.component,
        .is_apply = state->command.is_apply,
        .terminal = false,
    };
    return true;
}

bool backend_ota_workflow_note_begin_ack(
    backend_ota_workflow_t *state,
    const backend_ota_command_ack_t *ack)
{
    backend_ota_workflow_ack_prediction_t expected;
    if (!backend_ota_workflow_predict_begin_ack(state, &expected) ||
        !ack_matches(
            ack, &state->command.operation_id, expected,
            state->expected_sequence)) {
        return false;
    }
    state->begin_acked = true;
    state->expected_sequence = ack->next_sequence;
    return true;
}

bool backend_ota_workflow_predict_progress_ack(
    const backend_ota_workflow_t *state,
    backend_ota_workflow_ack_prediction_t *out)
{
    if (state == NULL || out == NULL || !state->command_active ||
        !state->begin_acked) {
        return false;
    }
    *out = (backend_ota_workflow_ack_prediction_t) {
        .component = state->command.component,
        .is_apply = state->command.is_apply,
        .terminal = false,
    };
    return true;
}

bool backend_ota_workflow_note_progress_ack(
    backend_ota_workflow_t *state,
    const backend_ota_progress_event_t *event,
    const backend_ota_command_ack_t *ack)
{
    backend_ota_workflow_ack_prediction_t expected;
    if (!backend_ota_workflow_predict_progress_ack(state, &expected) ||
        event == NULL || !event->prefix.has_operation_id ||
        !backend_ota_operation_id_equal(
            &event->prefix.operation_id, &state->command.operation_id) ||
        event->prefix.is_apply != state->command.is_apply ||
        event->prefix.sequence != state->expected_sequence ||
        event->prefix.component != state->command.component ||
        event->prefix.catalog_name == NULL ||
        strcmp(event->prefix.catalog_name, state->command.catalog_name) != 0 ||
        (event->stage == BACKEND_OTA_PROGRESS_REBOOT_WAIT &&
         !state->command.is_apply) ||
        (event->stage == BACKEND_OTA_PROGRESS_UART_RELAY &&
         state->command.component == BACKEND_OTA_COMPONENT_UPLINK) ||
        !ack_matches(
            ack, &state->command.operation_id, expected,
            state->expected_sequence)) {
        return false;
    }

    backend_ota_progress_state_t progress = state->progress;
    if (!backend_ota_progress_accept(
            &progress, event->stage, event->received, event->total,
            event->retry_count)) {
        return false;
    }
    state->progress = progress;
    state->expected_sequence = ack->next_sequence;
    return true;
}

bool backend_ota_workflow_predict_terminal_ack(
    const backend_ota_workflow_t *state,
    backend_ota_terminal_outcome_t outcome,
    backend_ota_workflow_ack_prediction_t *out)
{
    if (state == NULL || out == NULL || !state->command_active ||
        !state->begin_acked) {
        return false;
    }
    backend_ota_workflow_ack_prediction_t prediction = {
        .component = state->command.component,
        .is_apply = state->command.is_apply,
        .terminal = false,
    };
    if (outcome == BACKEND_OTA_TERMINAL_ELIGIBLE) {
        if (state->command.is_apply) {
            return false;
        }
        prediction.is_apply = true;
    } else if (outcome == BACKEND_OTA_TERMINAL_NO_UPDATE ||
               outcome == BACKEND_OTA_TERMINAL_APPLIED) {
        if ((outcome == BACKEND_OTA_TERMINAL_NO_UPDATE &&
             state->command.is_apply) ||
            (outcome == BACKEND_OTA_TERMINAL_APPLIED &&
             !state->command.is_apply)) {
            return false;
        }
        prediction.is_apply = false;
        if (state->command.component == BACKEND_OTA_COMPONENT_SCANNER0) {
            prediction.component = BACKEND_OTA_COMPONENT_SCANNER1;
        } else if (state->command.component == BACKEND_OTA_COMPONENT_SCANNER1) {
            prediction.component = BACKEND_OTA_COMPONENT_UPLINK;
        } else {
            prediction.terminal = true;
        }
    } else if (outcome == BACKEND_OTA_TERMINAL_FAILED ||
               outcome == BACKEND_OTA_TERMINAL_ROLLED_BACK) {
        prediction.terminal = true;
    } else {
        return false;
    }
    *out = prediction;
    return true;
}

bool backend_ota_workflow_note_terminal_ack(
    backend_ota_workflow_t *state,
    backend_ota_terminal_outcome_t outcome,
    const char receipt_sha256[65],
    const backend_ota_command_ack_t *ack)
{
    backend_ota_workflow_ack_prediction_t expected;
    if (!backend_ota_workflow_predict_terminal_ack(
            state, outcome, &expected) ||
        !ack_matches(
            ack, &state->command.operation_id, expected,
            state->expected_sequence)) {
        return false;
    }
    if (outcome == BACKEND_OTA_TERMINAL_ELIGIBLE) {
        if (receipt_sha256 == NULL ||
            !backend_ota_accepted_probe_capture(
                &state->command, receipt_sha256,
                state->expected_sequence, ack,
                &state->accepted_probe)) {
            return false;
        }
        state->has_accepted_probe = true;
        state->expected_component = state->command.component;
        state->expected_apply = true;
    } else if (outcome == BACKEND_OTA_TERMINAL_NO_UPDATE ||
               outcome == BACKEND_OTA_TERMINAL_APPLIED) {
        if (state->command.component == BACKEND_OTA_COMPONENT_SCANNER0) {
            state->completed_scanner_mask |= UINT8_C(1);
            state->expected_component = BACKEND_OTA_COMPONENT_SCANNER1;
        } else if (state->command.component == BACKEND_OTA_COMPONENT_SCANNER1) {
            state->completed_scanner_mask |= UINT8_C(2);
            state->expected_component = BACKEND_OTA_COMPONENT_UPLINK;
        } else {
            state->has_rollout_operation = false;
            state->completed_scanner_mask = 0U;
            state->expected_component = BACKEND_OTA_COMPONENT_SCANNER0;
        }
        state->has_accepted_probe = false;
        memset(&state->accepted_probe, 0, sizeof(state->accepted_probe));
        state->expected_apply = false;
    } else {
        state->has_rollout_operation = false;
        state->has_accepted_probe = false;
        state->completed_scanner_mask = 0U;
        state->expected_component = BACKEND_OTA_COMPONENT_SCANNER0;
        state->expected_apply = false;
    }
    if (expected.terminal) {
        state->has_expected_sequence = false;
        state->expected_sequence = 0U;
    } else {
        state->has_expected_sequence = true;
        state->expected_sequence = ack->next_sequence;
    }
    state->command_active = false;
    state->work_available = false;
    state->work_inflight = false;
    state->begin_acked = false;
    memset(&state->command, 0, sizeof(state->command));
    memset(&state->progress, 0, sizeof(state->progress));
    return true;
}
