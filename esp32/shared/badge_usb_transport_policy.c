#include "badge_usb_transport_policy.h"

#include <string.h>

bool badge_usb_app_reenumerate(
    const badge_usb_app_reenumerate_hooks_t *hooks)
{
    if (!hooks || !hooks->enable_bus_clock || !hooks->set_pad_enabled ||
        !hooks->delay_ms || !hooks->select_internal_phy) {
        return false;
    }

    hooks->enable_bus_clock(hooks->context);
    hooks->set_pad_enabled(hooks->context, false);
    hooks->select_internal_phy(hooks->context);
    /* ESP-IDF deliberately adopts retained USJ FIFO/interrupt state when its
     * driver starts. A host-visible detach is sufficient; resetting the
     * peripheral destroys the endpoint state while the host may retain it. */
    hooks->delay_ms(
        hooks->context, BADGE_USB_APP_REENUMERATE_DETACH_MS);
    return true;
}

static uint32_t remaining_ticks(const badge_usb_output_hooks_t *hooks,
                                uint32_t started, uint32_t timeout)
{
    uint32_t elapsed = hooks->now_ticks(hooks->context) - started;
    return elapsed >= timeout ? 0U : timeout - elapsed;
}

static bool output_hooks_valid(const badge_usb_output_hooks_t *hooks)
{
    return hooks && hooks->host_connected && hooks->lock && hooks->unlock &&
           hooks->current_owner && hooks->now_ticks && hooks->write &&
           hooks->drain;
}

badge_usb_emit_health_effect_t badge_usb_emit_health_effect_decide(
    badge_usb_emit_result_t result,
    badge_usb_frame_priority_t priority,
    badge_usb_emit_health_mode_t health_mode)
{
    if (health_mode == BADGE_USB_EMIT_HEALTH_NEUTRAL) {
        return BADGE_USB_EMIT_HEALTH_EFFECT_NONE;
    }
    if (result == BADGE_USB_EMIT_COMPLETED) {
        return BADGE_USB_EMIT_HEALTH_EFFECT_COMPLETED;
    }
    if (priority == BADGE_USB_FRAME_REQUIRED) {
        return result == BADGE_USB_EMIT_ENQUEUED
            ? BADGE_USB_EMIT_HEALTH_EFFECT_REQUIRED_ENQUEUED
            : BADGE_USB_EMIT_HEALTH_EFFECT_REQUIRED_HARD_FAILURE;
    }
    if (result == BADGE_USB_EMIT_ENQUEUED) {
        return BADGE_USB_EMIT_HEALTH_EFFECT_NONE;
    }
    return priority == BADGE_USB_FRAME_PROGRESS
        ? BADGE_USB_EMIT_HEALTH_EFFECT_PROGRESS_DROP
        : BADGE_USB_EMIT_HEALTH_EFFECT_OPTIONAL_DROP;
}

badge_usb_emit_result_t badge_usb_output_emit(
    badge_usb_output_policy_t *policy,
    const badge_usb_output_hooks_t *hooks,
    const void *data, size_t len,
    badge_usb_frame_priority_t priority,
    uint32_t timeout_ticks)
{
    if (!policy || !output_hooks_valid(hooks) || !data || len == 0U) {
        return BADGE_USB_EMIT_FAILED;
    }

    uintptr_t owner = hooks->current_owner(hooks->context);
    bool low_priority = priority != BADGE_USB_FRAME_REQUIRED;
    if (owner != 0U &&
        atomic_load_explicit(&policy->emission_owner, memory_order_acquire) == owner) {
        return low_priority ? BADGE_USB_EMIT_DROPPED : BADGE_USB_EMIT_FAILED;
    }
    if (low_priority && !hooks->host_connected(hooks->context)) {
        return BADGE_USB_EMIT_DROPPED;
    }
    if (atomic_load_explicit(&policy->poisoned, memory_order_acquire)) {
        return BADGE_USB_EMIT_POISONED;
    }
    if (!hooks->lock(hooks->context, timeout_ticks)) {
        return BADGE_USB_EMIT_FAILED;
    }

    /* A frame that failed while this caller waited owns the stream outcome.
     * Rechecking under the transaction lock prevents a queued waiter from
     * appending to its partial predecessor. */
    if (atomic_load_explicit(&policy->poisoned, memory_order_acquire)) {
        hooks->unlock(hooks->context);
        return BADGE_USB_EMIT_POISONED;
    }

    atomic_store_explicit(&policy->emission_owner, owner, memory_order_release);
    uint32_t started = hooks->now_ticks(hooks->context);
    size_t offset = 0U;
    badge_usb_emit_result_t result = BADGE_USB_EMIT_COMPLETED;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > BADGE_USB_TX_CHUNK_BYTES) {
            chunk = BADGE_USB_TX_CHUNK_BYTES;
        }
        int written = hooks->write(
            hooks->context, (const uint8_t *)data + offset, chunk,
            remaining_ticks(hooks, started, timeout_ticks));
        if (written != (int)chunk) {
            if (offset > 0U || written > 0) {
                atomic_store_explicit(&policy->poisoned, true,
                                      memory_order_release);
                result = BADGE_USB_EMIT_POISONED;
            } else {
                result = BADGE_USB_EMIT_FAILED;
            }
            break;
        }
        offset += chunk;
    }

    if (result == BADGE_USB_EMIT_COMPLETED &&
        !hooks->drain(hooks->context,
                      remaining_ticks(hooks, started, timeout_ticks))) {
        /* Every byte belongs to one complete frame in the driver ring. A
         * pending drain makes a required response incomplete, but it is not
         * a partial frame and must not permanently poison later commands. */
        result = BADGE_USB_EMIT_ENQUEUED;
    }

    atomic_store_explicit(&policy->emission_owner, 0U, memory_order_release);
    hooks->unlock(hooks->context);
    return result;
}

badge_usb_command_decision_t badge_usb_command_decide(bool recognized,
                                                       bool dispatch_ready,
                                                       bool recovery_only,
                                                       bool recovery_allowed)
{
    if (!dispatch_ready) {
        return BADGE_USB_COMMAND_BOOTING;
    }
    if (recovery_only) {
        return recovery_allowed ? BADGE_USB_COMMAND_DISPATCH
                                : BADGE_USB_COMMAND_RECOVERY_ONLY;
    }
    return recognized ? BADGE_USB_COMMAND_DISPATCH
                      : BADGE_USB_COMMAND_UNKNOWN;
}

static bool line_dispatch_hooks_valid(
    const badge_usb_line_dispatch_hooks_t *hooks)
{
    return hooks && hooks->normal_line_is_recognized &&
           hooks->recovery_line_is_allowed && hooks->note_recognized &&
           hooks->emit_booting && hooks->emit_recovery_only &&
           hooks->emit_unknown && hooks->dispatch_normal_line &&
           hooks->dispatch_recovery_line;
}

bool badge_usb_line_dispatch_run(
    const uint8_t *line,
    size_t line_byte_len,
    bool dispatch_ready,
    bool recovery_only,
    const badge_usb_line_dispatch_hooks_t *hooks)
{
    if (!line_dispatch_hooks_valid(hooks)) {
        return false;
    }

    bool recovery_allowed = false;
    bool recognized = false;
    if (line && line_byte_len > 0U) {
        if (recovery_only) {
            recovery_allowed = hooks->recovery_line_is_allowed(
                hooks->context, line, line_byte_len);
            recognized = recovery_allowed;
        } else {
            recognized = hooks->normal_line_is_recognized(
                hooks->context, line, line_byte_len);
        }
    }

    badge_usb_command_decision_t decision = badge_usb_command_decide(
        recognized, dispatch_ready, recovery_only, recovery_allowed);
    if (recognized && dispatch_ready) {
        hooks->note_recognized(hooks->context);
    }
    if (decision == BADGE_USB_COMMAND_BOOTING) {
        return hooks->emit_booting(hooks->context);
    }
    if (decision == BADGE_USB_COMMAND_RECOVERY_ONLY) {
        return hooks->emit_recovery_only(hooks->context);
    }
    if (decision != BADGE_USB_COMMAND_DISPATCH) {
        return hooks->emit_unknown(hooks->context);
    }
    return recovery_only
        ? hooks->dispatch_recovery_line(
            hooks->context, line, line_byte_len)
        : hooks->dispatch_normal_line(
            hooks->context, line, line_byte_len);
}

void badge_usb_upload_policy_init(badge_usb_upload_policy_t *state)
{
    if (state) {
        memset(state, 0, sizeof(*state));
    }
}

bool badge_usb_upload_begin(badge_usb_upload_policy_t *state,
                            badge_usb_binary_target_t target,
                            uint32_t exact_size)
{
    if (!state || state->target != BADGE_USB_BINARY_NONE || exact_size == 0U ||
        (target != BADGE_USB_BINARY_SCANNER &&
         target != BADGE_USB_BINARY_UPLINK)) {
        return false;
    }
    state->target = target;
    state->size = exact_size;
    return true;
}

bool badge_usb_upload_begin_credit_v1(badge_usb_upload_policy_t *state,
                                      badge_usb_binary_target_t target,
                                      uint32_t exact_size)
{
    if (target != BADGE_USB_BINARY_SCANNER ||
        !badge_usb_upload_begin(state, target, exact_size)) {
        return false;
    }
    state->credit_v1 = true;
    state->credit_remaining = exact_size < BADGE_USB_SCANNER_CREDIT_BYTES
        ? exact_size : BADGE_USB_SCANNER_CREDIT_BYTES;
    return true;
}

bool badge_usb_upload_credit_enabled(
    const badge_usb_upload_policy_t *state)
{
    return state && state->target == BADGE_USB_BINARY_SCANNER &&
           state->credit_v1;
}

bool badge_usb_upload_plan_credit_bytes(
    const badge_usb_upload_policy_t *state,
    size_t available, size_t *allowed)
{
    if (allowed) {
        *allowed = 0U;
    }
    if (!state || !allowed ||
        !badge_usb_upload_credit_enabled(state) ||
        state->waiting_for_credit_receipt || available == 0U ||
        state->received >= state->size ||
        available > (size_t)(state->size - state->received) ||
        available > (size_t)state->credit_remaining) {
        return false;
    }
    *allowed = available;
    return true;
}

bool badge_usb_upload_note_bytes(badge_usb_upload_policy_t *state,
                                 size_t bytes)
{
    if (!state || state->target == BADGE_USB_BINARY_NONE || bytes == 0U ||
        state->received > state->size ||
        (state->credit_v1 &&
         (state->waiting_for_credit_receipt ||
          bytes > (size_t)state->credit_remaining)) ||
        bytes > (size_t)(state->size - state->received)) {
        return false;
    }
    state->received += (uint32_t)bytes;
    if (state->credit_v1) {
        state->credit_remaining -= (uint32_t)bytes;
        if (state->received < state->size &&
            state->credit_remaining == 0U) {
            uint32_t remaining = state->size - state->received;
            state->pending_credit =
                remaining < BADGE_USB_SCANNER_CREDIT_BYTES
                    ? remaining : BADGE_USB_SCANNER_CREDIT_BYTES;
            state->waiting_for_credit_receipt = true;
        }
    }
    return true;
}

bool badge_usb_upload_credit_pending(
    const badge_usb_upload_policy_t *state)
{
    return badge_usb_upload_credit_enabled(state) &&
           state->waiting_for_credit_receipt &&
           state->pending_credit > 0U;
}

uint32_t badge_usb_upload_pending_credit(
    const badge_usb_upload_policy_t *state)
{
    return badge_usb_upload_credit_pending(state)
        ? state->pending_credit : 0U;
}

bool badge_usb_upload_credit_result(badge_usb_upload_policy_t *state,
                                    bool delivered)
{
    if (!state || !delivered ||
        !badge_usb_upload_credit_pending(state)) {
        return false;
    }
    state->credit_remaining = state->pending_credit;
    state->pending_credit = 0U;
    state->waiting_for_credit_receipt = false;
    return true;
}

static bool scanner_credit_hooks_valid(
    const badge_usb_scanner_credit_hooks_t *hooks)
{
    return hooks && hooks->write_durable && hooks->commit_transport &&
           hooks->finalize_durable && hooks->emit_required &&
           hooks->drain_required && hooks->complete_terminal;
}

static bool scanner_credit_deliver_required(
    const badge_usb_scanner_credit_hooks_t *hooks,
    const char *receipt)
{
    if (!hooks || !receipt || receipt[0] == '\0') {
        return false;
    }
    badge_usb_emit_result_t emitted =
        hooks->emit_required(hooks->context, receipt);
    if (emitted == BADGE_USB_EMIT_COMPLETED) {
        return true;
    }
    return emitted == BADGE_USB_EMIT_ENQUEUED &&
           hooks->drain_required(hooks->context);
}

badge_usb_scanner_credit_result_t badge_usb_scanner_credit_process(
    badge_usb_upload_policy_t *state,
    const badge_usb_scanner_credit_hooks_t *hooks,
    const uint8_t *bytes, size_t length, bool final_chunk,
    char *receipt, size_t receipt_capacity)
{
    if (receipt && receipt_capacity > 0U) {
        receipt[0] = '\0';
    }
    size_t allowed = 0U;
    if (!state || !scanner_credit_hooks_valid(hooks) ||
        !bytes || length == 0U || !receipt || receipt_capacity == 0U ||
        !badge_usb_upload_plan_credit_bytes(state, length, &allowed) ||
        allowed != length) {
        (void)badge_usb_upload_abort(state);
        return BADGE_USB_SCANNER_CREDIT_INVALID;
    }

    if (!hooks->write_durable(
            hooks->context, bytes, length, receipt, receipt_capacity)) {
        (void)badge_usb_upload_abort(state);
        return BADGE_USB_SCANNER_CREDIT_WRITE_FAILED;
    }
    if (!badge_usb_upload_note_bytes(state, length)) {
        (void)badge_usb_upload_abort(state);
        return BADGE_USB_SCANNER_CREDIT_ACCOUNTING_FAILED;
    }
    if (!hooks->commit_transport(hooks->context)) {
        (void)badge_usb_upload_abort(state);
        return BADGE_USB_SCANNER_CREDIT_COMMIT_FAILED;
    }

    bool credit_pending = badge_usb_upload_credit_pending(state);
    if (!final_chunk) {
        if (credit_pending) {
            if (receipt[0] == '\0') {
                (void)badge_usb_upload_abort(state);
                return BADGE_USB_SCANNER_CREDIT_RECEIPT_MISSING;
            }
            bool delivered =
                scanner_credit_deliver_required(hooks, receipt);
            if (!badge_usb_upload_credit_result(state, delivered)) {
                (void)badge_usb_upload_abort(state);
                return BADGE_USB_SCANNER_CREDIT_RECEIPT_FAILED;
            }
        } else if (receipt[0] != '\0') {
            (void)badge_usb_upload_abort(state);
            return BADGE_USB_SCANNER_CREDIT_UNEXPECTED_RECEIPT;
        }
        return BADGE_USB_SCANNER_CREDIT_CONTINUE;
    }

    if (state->received != state->size ||
        credit_pending || receipt[0] != '\0') {
        (void)badge_usb_upload_abort(state);
        return BADGE_USB_SCANNER_CREDIT_FINAL_MISMATCH;
    }

    bool durable_finalized = hooks->finalize_durable(
        hooks->context, receipt, receipt_capacity);
    if (!durable_finalized) {
        if (receipt[0] != '\0') {
            (void)scanner_credit_deliver_required(hooks, receipt);
        }
        (void)badge_usb_upload_terminal_result(state, false);
        return BADGE_USB_SCANNER_CREDIT_FINALIZE_FAILED;
    }
    badge_usb_upload_note_durable_finalize(state);

    bool terminal_delivered =
        scanner_credit_deliver_required(hooks, receipt);
    bool activation_allowed = badge_usb_upload_terminal_result(
        state, terminal_delivered);
    bool completion_succeeded = hooks->complete_terminal(
        hooks->context, activation_allowed);
    if (!terminal_delivered || !activation_allowed) {
        return BADGE_USB_SCANNER_CREDIT_TERMINAL_FAILED;
    }
    if (!completion_succeeded) {
        return BADGE_USB_SCANNER_CREDIT_ACTIVATION_FAILED;
    }
    return BADGE_USB_SCANNER_CREDIT_COMPLETE;
}

const char *badge_usb_scanner_credit_result_error(
    badge_usb_scanner_credit_result_t result)
{
    switch (result) {
    case BADGE_USB_SCANNER_CREDIT_CONTINUE:
    case BADGE_USB_SCANNER_CREDIT_COMPLETE:
        return "";
    case BADGE_USB_SCANNER_CREDIT_INVALID:
        return "scanner_credit_overrun";
    case BADGE_USB_SCANNER_CREDIT_WRITE_FAILED:
        return "scanner_write_failed";
    case BADGE_USB_SCANNER_CREDIT_ACCOUNTING_FAILED:
        return "scanner_accounting_failed";
    case BADGE_USB_SCANNER_CREDIT_COMMIT_FAILED:
        return "scanner_binary_commit_failed";
    case BADGE_USB_SCANNER_CREDIT_RECEIPT_MISSING:
        return "scanner_credit_receipt_missing";
    case BADGE_USB_SCANNER_CREDIT_RECEIPT_FAILED:
        return "scanner_credit_receipt_failed";
    case BADGE_USB_SCANNER_CREDIT_UNEXPECTED_RECEIPT:
        return "scanner_unexpected_receipt";
    case BADGE_USB_SCANNER_CREDIT_FINAL_MISMATCH:
        return "scanner_final_credit_mismatch";
    case BADGE_USB_SCANNER_CREDIT_FINALIZE_FAILED:
        return "usb_finalize_failed";
    case BADGE_USB_SCANNER_CREDIT_TERMINAL_FAILED:
        return "scanner_terminal_receipt_failed";
    case BADGE_USB_SCANNER_CREDIT_ACTIVATION_FAILED:
        return "scanner_activation_failed";
    default:
        return "scanner_credit_internal";
    }
}

void badge_usb_upload_note_durable_finalize(badge_usb_upload_policy_t *state)
{
    if (state && state->target != BADGE_USB_BINARY_NONE &&
        state->received == state->size) {
        state->durable_finalized = true;
    }
}

bool badge_usb_upload_terminal_result(badge_usb_upload_policy_t *state,
                                      bool delivered)
{
    if (!state) {
        return false;
    }
    bool activate = state->target == BADGE_USB_BINARY_SCANNER &&
                    state->durable_finalized && delivered;
    badge_usb_upload_policy_init(state);
    return activate;
}

badge_usb_binary_target_t badge_usb_upload_abort(
    badge_usb_upload_policy_t *state)
{
    if (!state) {
        return BADGE_USB_BINARY_NONE;
    }
    badge_usb_binary_target_t target = state->target;
    badge_usb_upload_policy_init(state);
    return target;
}
