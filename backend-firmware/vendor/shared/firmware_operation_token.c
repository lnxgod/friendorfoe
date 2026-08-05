#include "firmware_operation_token.h"

#include <string.h>

static bool owner_is_valid(fw_operation_owner_t owner)
{
    return owner == FW_OPERATION_OWNER_SCANNER_STAGING ||
           owner == FW_OPERATION_OWNER_SCANNER_RELAY ||
           owner == FW_OPERATION_OWNER_UPLINK_OTA ||
           owner == FW_OPERATION_OWNER_RUNTIME_STARTUP;
}

static uint32_t next_epoch(uint32_t current)
{
    current++;
    return current == 0U ? 1U : current;
}

static bool token_matches(const fw_operation_state_t *state,
                          fw_operation_token_t token)
{
    return state && state->active && token.valid &&
           token.owner == state->owner &&
           token.generation != 0U &&
           token.generation == state->generation;
}

void fw_operation_state_init(fw_operation_state_t *state)
{
    if (state) {
        memset(state, 0, sizeof(*state));
    }
}

bool fw_operation_state_try_begin(fw_operation_state_t *state,
                                  fw_operation_owner_t owner,
                                  fw_operation_token_t *out_token)
{
    if (out_token) {
        memset(out_token, 0, sizeof(*out_token));
    }
    if (!state || !out_token || !owner_is_valid(owner) || state->active ||
        state->recovery_restart_reserved || state->preemption_requested ||
        state->generation == UINT32_MAX) {
        return false;
    }

    uint32_t generation = state->generation + 1U;
    state->active = true;
    state->owner = owner;
    state->generation = generation;
    state->operation_epoch = next_epoch(state->operation_epoch);
    state->uart_lease = false;
    state->radio_inhibited = true;
    out_token->owner = owner;
    out_token->generation = generation;
    out_token->valid = true;
    return true;
}

bool fw_operation_state_try_begin_quiesced(
    fw_operation_state_t *state,
    fw_operation_owner_t owner,
    uint32_t acknowledged_inhibit_epoch,
    fw_operation_token_t *out_token)
{
    if (out_token) {
        memset(out_token, 0, sizeof(*out_token));
    }
    if (!state || !state->radio_inhibited ||
        state->operation_epoch != acknowledged_inhibit_epoch) {
        return false;
    }
    return fw_operation_state_try_begin(state, owner, out_token);
}

bool fw_operation_state_attach_uart_lease(
    fw_operation_state_t *state, fw_operation_token_t token)
{
    if (!token_matches(state, token) || state->uart_lease) {
        return false;
    }
    state->uart_lease = true;
    return true;
}

bool fw_operation_state_end(fw_operation_state_t *state,
                            fw_operation_token_t token,
                            bool *release_uart_lease)
{
    if (release_uart_lease) {
        *release_uart_lease = false;
    }
    if (!token_matches(state, token)) {
        return false;
    }

    if (release_uart_lease) {
        *release_uart_lease = state->uart_lease;
    }
    state->active = false;
    state->owner = FW_OPERATION_OWNER_NONE;
    state->operation_epoch = next_epoch(state->operation_epoch);
    state->uart_lease = false;
    return true;
}

bool fw_operation_state_try_reserve_recovery_restart(
    fw_operation_state_t *state)
{
    if (!state || state->active || state->recovery_restart_reserved ||
        state->preemption_requested) {
        return false;
    }
    state->recovery_restart_reserved = true;
    state->operation_epoch = next_epoch(state->operation_epoch);
    return true;
}

void fw_operation_state_snapshot(const fw_operation_state_t *state,
                                 fw_operation_snapshot_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!state) {
        out->active = true;
        out->radio_inhibited = true;
        return;
    }
    out->active = state->active;
    out->owner = state->owner;
    out->operation_epoch = state->operation_epoch;
    out->radio_inhibited = state->radio_inhibited;
    out->preemption_requested = state->preemption_requested;
    out->recovery_restart_reserved = state->recovery_restart_reserved;
}

bool fw_operation_state_request_radio_inhibit(
    fw_operation_state_t *state)
{
    if (!state || state->radio_inhibited) {
        return false;
    }
    state->radio_inhibited = true;
    state->operation_epoch = next_epoch(state->operation_epoch);
    return true;
}

bool fw_operation_state_request_preemption(
    fw_operation_state_t *state)
{
    if (!state || state->recovery_restart_reserved ||
        (state->preemption_requested && state->radio_inhibited)) {
        return false;
    }
    state->preemption_requested = true;
    state->radio_inhibited = true;
    state->operation_epoch = next_epoch(state->operation_epoch);
    return true;
}

bool fw_operation_state_clear_radio_inhibit(
    fw_operation_state_t *state)
{
    if (!state || state->active || state->preemption_requested ||
        !state->radio_inhibited) {
        return false;
    }
    state->radio_inhibited = false;
    state->operation_epoch = next_epoch(state->operation_epoch);
    return true;
}
