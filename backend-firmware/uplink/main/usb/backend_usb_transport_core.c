#include "backend_usb_transport_core.h"

#include <limits.h>
#include <string.h>

static void increment_saturating(uint64_t *value)
{
    if (*value != UINT64_MAX) {
        ++*value;
    }
}

static void atomic_increment_saturating(atomic_uint_fast64_t *value)
{
    uint_fast64_t current = atomic_load_explicit(
        value, memory_order_relaxed);
    while (current != UINT_FAST64_MAX &&
           !atomic_compare_exchange_weak_explicit(
               value,
               &current,
               current + 1U,
               memory_order_relaxed,
               memory_order_relaxed)) {
    }
}

static bool live_frame_kind(backend_usb_frame_kind_t kind)
{
    return kind == BACKEND_USB_FRAME_LIVE_HEARTBEAT ||
        kind == BACKEND_USB_FRAME_LIVE_READY;
}

static int64_t deadline_after(int64_t now_ms, int64_t delay_ms)
{
    if (delay_ms > 0 && now_ms > INT64_MAX - delay_ms) {
        return INT64_MAX;
    }
    if (delay_ms < 0 && now_ms < INT64_MIN - delay_ms) {
        return INT64_MIN;
    }
    return now_ms + delay_ms;
}

static bool live_session_valid(const char *session_id)
{
    if (session_id == NULL) {
        return false;
    }
    const size_t length = strlen(session_id);
    if (length == 0 || length > 32U) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        const unsigned char byte = (unsigned char)session_id[index];
        if (byte < 0x20U || byte == 0x7FU) {
            return false;
        }
    }
    return true;
}

bool backend_usb_live_start(
    backend_usb_live_state_t *state,
    const char *session_id,
    int64_t now_ms)
{
    if (state == NULL || !live_session_valid(session_id)) {
        return false;
    }
    backend_usb_live_state_t started;
    memset(&started, 0, sizeof(started));
    memcpy(started.session_id, session_id, strlen(session_id) + 1U);
    started.next_heartbeat_ms = now_ms;
    started.started = true;
    *state = started;
    return true;
}

bool backend_usb_live_prepare_heartbeat(
    backend_usb_live_state_t *state,
    int64_t now_ms,
    uint64_t *out_heartbeat_sequence)
{
    if (out_heartbeat_sequence != NULL) {
        *out_heartbeat_sequence = 0;
    }
    if (state == NULL || out_heartbeat_sequence == NULL ||
        !state->started || state->heartbeat_pending ||
        state->last_sent_sequence == UINT64_MAX ||
        now_ms < state->next_heartbeat_ms) {
        return false;
    }
    state->pending_heartbeat_sequence = state->last_sent_sequence + 1U;
    state->heartbeat_pending = true;
    *out_heartbeat_sequence = state->pending_heartbeat_sequence;
    return true;
}

bool backend_usb_live_note_heartbeat_sent(
    backend_usb_live_state_t *state,
    uint64_t sequence,
    int64_t now_ms)
{
    if (state == NULL || !state->started || !state->heartbeat_pending ||
        sequence != state->pending_heartbeat_sequence ||
        sequence != state->last_sent_sequence + 1U) {
        return false;
    }
    state->last_sent_sequence = sequence;
    state->last_sent_ms = now_ms;
    state->pending_heartbeat_sequence = 0;
    state->heartbeat_pending = false;
    state->next_heartbeat_ms = deadline_after(
        now_ms, BACKEND_USB_HEARTBEAT_MS);
    return true;
}

void backend_usb_live_note_heartbeat_failed(
    backend_usb_live_state_t *state,
    uint64_t sequence)
{
    if (state == NULL || !state->started || !state->heartbeat_pending ||
        sequence != state->pending_heartbeat_sequence) {
        return;
    }
    state->pending_heartbeat_sequence = 0;
    state->heartbeat_pending = false;
}

static void expire_confirmation(
    backend_usb_live_state_t *state, int64_t now_ms)
{
    if (state->confirmed && now_ms >= state->lease_expires_ms) {
        state->confirmed = false;
    }
}

bool backend_usb_live_acknowledge(
    backend_usb_live_state_t *state,
    const char *session_id,
    uint64_t sequence,
    int64_t now_ms)
{
    if (state == NULL) {
        return false;
    }
    expire_confirmation(state, now_ms);
    if (!state->started || session_id == NULL ||
        strcmp(session_id, state->session_id) != 0 ||
        sequence <= state->last_ack_sequence ||
        sequence != state->last_sent_sequence ||
        now_ms < state->last_sent_ms ||
        now_ms >= deadline_after(
            state->last_sent_ms, BACKEND_USB_LIVE_LEASE_MS)) {
        return false;
    }
    state->last_ack_sequence = sequence;
    state->confirmed = true;
    state->lease_expires_ms = deadline_after(
        now_ms, BACKEND_USB_LIVE_LEASE_MS);
    return true;
}

void backend_usb_live_stop(backend_usb_live_state_t *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

bool backend_usb_live_confirmed(
    backend_usb_live_state_t *state,
    int64_t now_ms)
{
    if (state == NULL || !state->started) {
        return false;
    }
    expire_confirmation(state, now_ms);
    return state->confirmed;
}

bool backend_usb_transport_init(
    backend_usb_transport_core_t *transport,
    backend_usb_required_frame_t *required_storage,
    size_t required_capacity,
    backend_usb_optional_frame_t *optional_storage,
    size_t optional_capacity)
{
    if (transport == NULL || required_storage == NULL ||
        optional_storage == NULL || required_capacity == 0 ||
        optional_capacity == 0 ||
        required_capacity > BACKEND_USB_REQUIRED_QUEUE_CAPACITY ||
        optional_capacity > BACKEND_USB_OPTIONAL_QUEUE_CAPACITY) {
        return false;
    }
    memset(transport, 0, sizeof(*transport));
    transport->required = required_storage;
    transport->optional = optional_storage;
    transport->required_capacity = required_capacity;
    transport->optional_capacity = optional_capacity;
    atomic_init(&transport->optional_contention_drops, 0U);
    atomic_init(&transport->required_contention_failures, 0U);
    return true;
}

static bool enqueue_frame(
    backend_usb_transport_core_t *transport,
    backend_usb_frame_priority_t priority,
    backend_usb_frame_kind_t kind,
    uint64_t correlation_sequence,
    uint64_t live_generation,
    const char *frame,
    size_t length)
{
    if (transport == NULL || frame == NULL || length == 0) {
        return false;
    }
    if (priority == BACKEND_USB_FRAME_REQUIRED) {
        if (length > BACKEND_USB_STATUS_MAX ||
            transport->required_count >= transport->required_capacity) {
            increment_saturating(&transport->required_failures);
            return false;
        }
        const size_t tail =
            (transport->required_head + transport->required_count) %
            transport->required_capacity;
        backend_usb_required_frame_t *destination =
            &transport->required[tail];
        destination->kind = kind;
        destination->correlation_sequence = correlation_sequence;
        destination->live_generation = live_generation;
        destination->length = length;
        memcpy(destination->bytes, frame, length);
        ++transport->required_count;
        return true;
    }
    if (priority == BACKEND_USB_FRAME_OPTIONAL) {
        if (length > BACKEND_USB_DET_MAX ||
            transport->optional_count >= transport->optional_capacity) {
            increment_saturating(&transport->optional_drops);
            return false;
        }
        const size_t tail =
            (transport->optional_head + transport->optional_count) %
            transport->optional_capacity;
        backend_usb_optional_frame_t *destination =
            &transport->optional[tail];
        destination->kind = kind;
        destination->correlation_sequence = correlation_sequence;
        destination->live_generation = live_generation;
        destination->length = length;
        memcpy(destination->bytes, frame, length);
        ++transport->optional_count;
        return true;
    }
    return false;
}

bool backend_usb_transport_enqueue(
    backend_usb_transport_core_t *transport,
    backend_usb_frame_priority_t priority,
    backend_usb_frame_kind_t kind,
    uint64_t correlation_sequence,
    const char *frame,
    size_t length)
{
    if (live_frame_kind(kind)) {
        return false;
    }
    return enqueue_frame(
        transport,
        priority,
        kind,
        correlation_sequence,
        0U,
        frame,
        length);
}

bool backend_usb_transport_enqueue_live(
    backend_usb_transport_core_t *transport,
    backend_usb_frame_priority_t priority,
    backend_usb_frame_kind_t kind,
    uint64_t correlation_sequence,
    uint64_t live_generation,
    const char *frame,
    size_t length)
{
    if (!live_frame_kind(kind) || live_generation == 0U) {
        return false;
    }
    return enqueue_frame(
        transport,
        priority,
        kind,
        correlation_sequence,
        live_generation,
        frame,
        length);
}

bool backend_usb_transport_pop(
    backend_usb_transport_core_t *transport,
    backend_usb_frame_t *out)
{
    if (transport == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (transport->required_count > 0) {
        const backend_usb_required_frame_t *source =
            &transport->required[transport->required_head];
        out->priority = BACKEND_USB_FRAME_REQUIRED;
        out->kind = source->kind;
        out->correlation_sequence = source->correlation_sequence;
        out->live_generation = source->live_generation;
        out->length = source->length;
        memcpy(out->bytes, source->bytes, source->length);
        transport->required_head =
            (transport->required_head + 1U) % transport->required_capacity;
        --transport->required_count;
        return true;
    }
    if (transport->optional_count > 0) {
        const backend_usb_optional_frame_t *source =
            &transport->optional[transport->optional_head];
        out->priority = BACKEND_USB_FRAME_OPTIONAL;
        out->kind = source->kind;
        out->correlation_sequence = source->correlation_sequence;
        out->live_generation = source->live_generation;
        out->length = source->length;
        memcpy(out->bytes, source->bytes, source->length);
        transport->optional_head =
            (transport->optional_head + 1U) % transport->optional_capacity;
        --transport->optional_count;
        return true;
    }
    return false;
}

bool backend_usb_transport_pop_current(
    backend_usb_transport_core_t *transport,
    uint64_t live_generation,
    backend_usb_frame_t *out)
{
    if (transport == NULL || out == NULL || transport->output_poisoned) {
        return false;
    }
    while (backend_usb_transport_pop(transport, out)) {
        if (!live_frame_kind(out->kind) ||
            out->live_generation == live_generation) {
            return true;
        }
    }
    return false;
}

void backend_usb_transport_note_tx_failed(
    backend_usb_transport_core_t *transport,
    const backend_usb_frame_t *frame,
    uint64_t live_generation,
    bool wrote_any_bytes)
{
    if (transport == NULL || frame == NULL) {
        return;
    }
    if (wrote_any_bytes) {
        transport->output_poisoned = true;
    }
    if (frame->kind == BACKEND_USB_FRAME_LIVE_HEARTBEAT &&
        frame->live_generation == live_generation) {
        backend_usb_live_note_heartbeat_failed(
            &transport->live, frame->correlation_sequence);
    }
}

void backend_usb_transport_note_output_recovered(
    backend_usb_transport_core_t *transport)
{
    if (transport != NULL) {
        transport->output_poisoned = false;
    }
}

void backend_usb_transport_note_lock_failure(
    backend_usb_transport_core_t *transport,
    backend_usb_frame_priority_t priority)
{
    if (transport == NULL) {
        return;
    }
    if (priority == BACKEND_USB_FRAME_REQUIRED) {
        atomic_increment_saturating(
            &transport->required_contention_failures);
    } else if (priority == BACKEND_USB_FRAME_OPTIONAL) {
        atomic_increment_saturating(&transport->optional_contention_drops);
    }
}

uint64_t backend_usb_transport_required_contention_failures(
    const backend_usb_transport_core_t *transport)
{
    return transport == NULL ? 0U : (uint64_t)atomic_load_explicit(
        &transport->required_contention_failures,
        memory_order_relaxed);
}

uint64_t backend_usb_transport_optional_contention_drops(
    const backend_usb_transport_core_t *transport)
{
    return transport == NULL ? 0U : (uint64_t)atomic_load_explicit(
        &transport->optional_contention_drops,
        memory_order_relaxed);
}
