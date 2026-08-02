#include "backend_uploader.h"

#include <limits.h>
#include <string.h>

#define BACKEND_UPLOADER_RETRY_MAX_EXPONENT 7U

static void increment_saturated(uint32_t *value)
{
    if (*value < UINT32_MAX) {
        (*value)++;
    }
}

static bool identity_matches(
    const backend_uploader_state_t *state,
    uint32_t sequence,
    uint32_t crc32)
{
    return state->in_flight_sequence == sequence &&
           state->in_flight_crc32 == crc32;
}

static bool batch_identity_is_valid(const backend_upload_batch_t *batch)
{
    return batch && batch->sequence != 0U &&
           batch->json_len >= 2U &&
           batch->json_len <= BACKEND_UPLOAD_MAX_JSON;
}

static bool queue_depth_is_valid(uint32_t depth)
{
    return depth <= BACKEND_UPLOAD_FIFO_CAPACITY;
}

static void clear_request(backend_uploader_state_t *state)
{
    state->in_flight_sequence = 0U;
    state->in_flight_crc32 = 0U;
    state->next_attempt_ms = -1;
    state->retry_exponent = 0U;
    state->in_flight = false;
    state->in_flight_orphaned = false;
}

static int64_t add_delay_saturated(int64_t now_ms, uint32_t delay_ms)
{
    if (now_ms > INT64_MAX - (int64_t)delay_ms) {
        return INT64_MAX;
    }
    return now_ms + (int64_t)delay_ms;
}

static bool response_contract_is_valid(
    backend_http_disposition_t disposition,
    int status_code,
    backend_uploader_queue_result_t queue_result)
{
    if (queue_result != BACKEND_UPLOADER_QUEUE_UNCHANGED &&
        queue_result != BACKEND_UPLOADER_QUEUE_POPPED &&
        queue_result != BACKEND_UPLOADER_QUEUE_QUARANTINED) {
        return false;
    }
    switch (disposition) {
    case BACKEND_HTTP_ACK:
        return status_code >= 200 && status_code <= 299 &&
               queue_result != BACKEND_UPLOADER_QUEUE_QUARANTINED;
    case BACKEND_HTTP_RETRY:
        return queue_result == BACKEND_UPLOADER_QUEUE_UNCHANGED;
    case BACKEND_HTTP_QUARANTINE:
        return status_code >= 400 && status_code <= 499 &&
               status_code != 408 && status_code != 429 &&
               queue_result != BACKEND_UPLOADER_QUEUE_POPPED;
    default:
        return false;
    }
}

static bool removal_result_matches_request(
    const backend_uploader_state_t *state,
    backend_http_disposition_t disposition,
    backend_uploader_queue_result_t queue_result)
{
    if (disposition == BACKEND_HTTP_RETRY) {
        return true;
    }
    if (state->in_flight_orphaned) {
        return queue_result == BACKEND_UPLOADER_QUEUE_UNCHANGED;
    }
    if (disposition == BACKEND_HTTP_ACK) {
        return queue_result == BACKEND_UPLOADER_QUEUE_POPPED;
    }
    return queue_result == BACKEND_UPLOADER_QUEUE_QUARANTINED;
}

void backend_uploader_state_init(backend_uploader_state_t *state)
{
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->next_attempt_ms = -1;
}

bool backend_uploader_note_enqueued(
    backend_uploader_state_t *state,
    uint32_t post_push_depth,
    bool dropped_oldest,
    uint32_t dropped_sequence,
    uint32_t dropped_crc32)
{
    if (!state || post_push_depth == 0U ||
        !queue_depth_is_valid(post_push_depth) ||
        (!dropped_oldest &&
         (dropped_sequence != 0U || dropped_crc32 != 0U)) ||
        (dropped_oldest && dropped_sequence == 0U)) {
        return false;
    }

    state->queue_depth = post_push_depth;
    increment_saturated(&state->queued_count);
    if (!dropped_oldest) {
        return true;
    }

    increment_saturated(&state->overflow_dropped_count);
    if (!identity_matches(state, dropped_sequence, dropped_crc32)) {
        return true;
    }
    if (state->in_flight) {
        state->in_flight_orphaned = true;
    } else {
        clear_request(state);
    }
    return true;
}

bool backend_uploader_begin_head(
    backend_uploader_state_t *state,
    const backend_upload_batch_t *head,
    uint32_t actual_queue_depth,
    int64_t now_ms)
{
    if (!state || !batch_identity_is_valid(head) ||
        actual_queue_depth == 0U ||
        !queue_depth_is_valid(actual_queue_depth) || now_ms < 0) {
        return false;
    }

    state->queue_depth = actual_queue_depth;
    if (state->in_flight) {
        return false;
    }

    const bool have_selected_request =
        state->in_flight_sequence != 0U;
    const bool same_request = have_selected_request &&
        identity_matches(state, head->sequence, head->json_crc32);
    if (have_selected_request && !same_request) {
        clear_request(state);
    } else if (same_request) {
        if (state->in_flight_orphaned || state->next_attempt_ms < 0 ||
            now_ms < state->next_attempt_ms) {
            return false;
        }
        state->in_flight = true;
        return true;
    }

    state->in_flight_sequence = head->sequence;
    state->in_flight_crc32 = head->json_crc32;
    state->next_attempt_ms = now_ms;
    state->retry_exponent = 0U;
    state->in_flight = true;
    state->in_flight_orphaned = false;
    return true;
}

backend_uploader_outcome_t backend_uploader_note_response(
    backend_uploader_state_t *state,
    uint32_t request_sequence,
    uint32_t request_crc32,
    backend_http_disposition_t disposition,
    int status_code,
    backend_uploader_queue_result_t queue_result,
    uint32_t actual_queue_depth,
    uint32_t random_value,
    int64_t now_ms)
{
    if (!state || !state->in_flight || request_sequence == 0U ||
        !identity_matches(state, request_sequence, request_crc32) ||
        !queue_depth_is_valid(actual_queue_depth) || now_ms < 0 ||
        !response_contract_is_valid(
            disposition, status_code, queue_result) ||
        !removal_result_matches_request(
            state, disposition, queue_result)) {
        return BACKEND_UPLOADER_IGNORED;
    }

    state->queue_depth = actual_queue_depth;
    switch (disposition) {
    case BACKEND_HTTP_ACK:
        increment_saturated(&state->ack_count);
        state->last_backend_success_ms = now_ms;
        clear_request(state);
        return BACKEND_UPLOADER_ACKED;
    case BACKEND_HTTP_QUARANTINE:
        if (queue_result == BACKEND_UPLOADER_QUEUE_QUARANTINED) {
            increment_saturated(&state->quarantine_count);
        }
        clear_request(state);
        return BACKEND_UPLOADER_QUARANTINED;
    case BACKEND_HTTP_RETRY:
        increment_saturated(&state->retry_count);
        if (state->in_flight_orphaned) {
            clear_request(state);
            return BACKEND_UPLOADER_RETRY;
        }
        state->in_flight = false;
        {
            const uint32_t delay_ms = backend_retry_delay_ms(
                state->retry_exponent, random_value);
            state->next_attempt_ms = add_delay_saturated(
                now_ms, delay_ms);
            if (state->retry_exponent <
                BACKEND_UPLOADER_RETRY_MAX_EXPONENT) {
                state->retry_exponent++;
            }
        }
        return BACKEND_UPLOADER_RETRY;
    default:
        return BACKEND_UPLOADER_IGNORED;
    }
}

void backend_heartbeat_init(
    backend_heartbeat_state_t *state,
    int64_t now_ms)
{
    if (!state || now_ms < 0) {
        return;
    }
    state->last_queued_ms = now_ms;
    state->initialized = true;
}

bool backend_heartbeat_due(
    const backend_heartbeat_state_t *state,
    int64_t now_ms)
{
    return state && state->initialized &&
           state->last_queued_ms >= 0 &&
           now_ms >= state->last_queued_ms &&
           now_ms - state->last_queued_ms >=
               BACKEND_HEARTBEAT_INTERVAL_MS;
}

void backend_heartbeat_mark_queued(
    backend_heartbeat_state_t *state,
    int64_t now_ms)
{
    if (!state || !state->initialized ||
        now_ms < state->last_queued_ms) {
        return;
    }
    state->last_queued_ms = now_ms;
}
