#include "badge_usb_stream.h"

#include <string.h>

static void init_result(badge_usb_stream_result_t *result)
{
    if (result) {
        memset(result, 0, sizeof(*result));
    }
}

static void clear_binary(badge_usb_stream_t *state)
{
    state->target = BADGE_USB_BINARY_NONE;
    state->exact_size = 0U;
    state->received = 0U;
    state->last_activity_ms = 0U;
    state->pending_cr = false;
}

static badge_usb_stream_event_t error_result(badge_usb_stream_result_t *result,
                                              badge_usb_binary_target_t target,
                                              size_t consumed,
                                              const char *error)
{
    if (result) {
        result->event = BADGE_USB_EVENT_ERROR;
        result->target = target;
        result->input_consumed = consumed;
        result->error = error;
    }
    return BADGE_USB_EVENT_ERROR;
}

void badge_usb_stream_init(badge_usb_stream_t *state,
                           char *line, size_t line_capacity)
{
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->line = line;
    state->line_capacity = line_capacity;
    if (line && line_capacity > 0U) {
        line[0] = '\0';
    }
}

bool badge_usb_stream_begin_binary(badge_usb_stream_t *state,
                                   badge_usb_binary_target_t target,
                                   uint32_t exact_size, uint32_t now_ms)
{
    if (!state || state->target != BADGE_USB_BINARY_NONE ||
        (target != BADGE_USB_BINARY_SCANNER &&
         target != BADGE_USB_BINARY_UPLINK) || exact_size == 0U) {
        return false;
    }
    state->target = target;
    state->exact_size = exact_size;
    state->received = 0U;
    state->last_activity_ms = now_ms;
    state->pending_cr = false;
    return true;
}

badge_usb_stream_event_t badge_usb_stream_peek_binary(
    const badge_usb_stream_t *state, const uint8_t *src, size_t src_len,
    size_t max_bytes, badge_usb_stream_result_t *result)
{
    init_result(result);
    if (!state || !result || (!src && src_len != 0U) || max_bytes == 0U ||
        state->target == BADGE_USB_BINARY_NONE ||
        state->received >= state->exact_size) {
        return error_result(result,
                            state ? state->target : BADGE_USB_BINARY_NONE,
                            0U, "invalid binary peek");
    }
    if (src_len == 0U) {
        return BADGE_USB_EVENT_NONE;
    }

    uint32_t remaining = state->exact_size - state->received;
    size_t chunk = src_len;
    if (chunk > max_bytes) {
        chunk = max_bytes;
    }
    if (chunk > (size_t)remaining) {
        chunk = (size_t)remaining;
    }
    result->event = chunk == (size_t)remaining
        ? BADGE_USB_EVENT_BINARY_COMPLETE
        : BADGE_USB_EVENT_BINARY_CHUNK;
    result->target = state->target;
    result->bytes = src;
    result->bytes_len = chunk;
    result->input_consumed = chunk;
    return result->event;
}

bool badge_usb_stream_commit_binary(
    badge_usb_stream_t *state, const badge_usb_stream_result_t *result,
    uint32_t now_ms)
{
    if (!state || !result || state->target == BADGE_USB_BINARY_NONE ||
        result->target != state->target || !result->bytes ||
        result->bytes_len == 0U ||
        result->bytes_len > (size_t)(state->exact_size - state->received) ||
        (result->event != BADGE_USB_EVENT_BINARY_CHUNK &&
         result->event != BADGE_USB_EVENT_BINARY_COMPLETE)) {
        return false;
    }
    bool completes = result->bytes_len ==
        (size_t)(state->exact_size - state->received);
    if ((result->event == BADGE_USB_EVENT_BINARY_COMPLETE) != completes) {
        return false;
    }
    state->received += (uint32_t)result->bytes_len;
    state->last_activity_ms = now_ms;
    return true;
}

void badge_usb_stream_clear_binary(badge_usb_stream_t *state)
{
    if (state) {
        clear_binary(state);
    }
}

bool badge_usb_stream_binary_timed_out(
    const badge_usb_stream_t *state, uint32_t now_ms,
    uint32_t idle_timeout_ms)
{
    if (!state || state->target == BADGE_USB_BINARY_NONE ||
        idle_timeout_ms == 0U) {
        return false;
    }
    int32_t elapsed_ms = (int32_t)(now_ms - state->last_activity_ms);
    return elapsed_ms >= 0 && (uint32_t)elapsed_ms >= idle_timeout_ms;
}

badge_usb_stream_event_t badge_usb_stream_feed(
    badge_usb_stream_t *state, const uint8_t *src, size_t src_len,
    uint32_t now_ms, badge_usb_stream_result_t *result)
{
    init_result(result);
    if (!state || !result || (!src && src_len != 0U)) {
        badge_usb_binary_target_t target = state
            ? state->target
            : BADGE_USB_BINARY_NONE;
        if (state) {
            state->pending_cr = false;
            if (state->target != BADGE_USB_BINARY_NONE) {
                clear_binary(state);
            }
        }
        return error_result(result, target, 0U,
                            "invalid stream input");
    }

    if (state->target != BADGE_USB_BINARY_NONE) {
        uint32_t remaining = state->exact_size - state->received;
        size_t chunk = src_len < (size_t)remaining ? src_len : (size_t)remaining;
        badge_usb_binary_target_t target = state->target;
        if (chunk == 0U) {
            return BADGE_USB_EVENT_NONE;
        }
        state->received += (uint32_t)chunk;
        state->last_activity_ms = now_ms;
        result->target = target;
        result->bytes = src;
        result->bytes_len = chunk;
        result->input_consumed = chunk;
        if (state->received == state->exact_size) {
            clear_binary(state);
            result->event = BADGE_USB_EVENT_BINARY_COMPLETE;
            return result->event;
        }
        result->event = BADGE_USB_EVENT_BINARY_CHUNK;
        return result->event;
    }

    if (!state->line || state->line_capacity == 0U) {
        state->pending_cr = false;
        return error_result(result, BADGE_USB_BINARY_NONE, 0U,
                            "line buffer unavailable");
    }

    for (size_t index = 0U; index < src_len; index++) {
        uint8_t byte = src[index];
        if (state->discarding_oversize_line) {
            state->pending_cr = false;
            if (byte == '\n') {
                state->discarding_oversize_line = false;
                state->line_length = 0U;
                state->line[0] = '\0';
            }
            result->input_consumed = index + 1U;
            if (byte == '\n') {
                return BADGE_USB_EVENT_NONE;
            }
            continue;
        }

        if (state->pending_cr) {
            state->pending_cr = false;
            if (byte != '\n') {
                state->discarding_oversize_line = true;
                state->line_length = 0U;
                state->line[0] = '\0';
                result->input_consumed = index + 1U;
                continue;
            }
        }

        if (byte == '\r') {
            state->pending_cr = true;
            result->input_consumed = index + 1U;
            continue;
        }

        if (byte == '\n') {
            state->pending_cr = false;
            state->line[state->line_length] = '\0';
            result->event = BADGE_USB_EVENT_LINE;
            result->line = state->line;
            result->line_byte_len = state->line_length;
            result->input_consumed = index + 1U;
            state->line_length = 0U;
            return result->event;
        }

        if (state->line_length + 1U >= state->line_capacity) {
            state->pending_cr = false;
            state->discarding_oversize_line = true;
            state->line_length = 0U;
            state->line[0] = '\0';
            return error_result(result, BADGE_USB_BINARY_NONE, index + 1U,
                                "line too long");
        }
        state->line[state->line_length++] = (char)byte;
        result->input_consumed = index + 1U;
    }
    return BADGE_USB_EVENT_NONE;
}

badge_usb_stream_event_t badge_usb_stream_poll_timeout(
    badge_usb_stream_t *state, uint32_t now_ms, uint32_t idle_timeout_ms)
{
    if (!state || state->target == BADGE_USB_BINARY_NONE ||
        idle_timeout_ms == 0U) {
        return BADGE_USB_EVENT_NONE;
    }
    int32_t elapsed_ms = (int32_t)(now_ms - state->last_activity_ms);
    if (elapsed_ms < 0 ||
        (uint32_t)elapsed_ms < idle_timeout_ms) {
        return BADGE_USB_EVENT_NONE;
    }
    clear_binary(state);
    return BADGE_USB_EVENT_ERROR;
}

badge_usb_stream_event_t badge_usb_stream_abort(
    badge_usb_stream_t *state, const char *reason,
    badge_usb_stream_result_t *result)
{
    init_result(result);
    if (!state) {
        return error_result(result, BADGE_USB_BINARY_NONE, 0U,
                            "invalid stream input");
    }
    badge_usb_binary_target_t target = state->target;
    clear_binary(state);
    if (!result) {
        return error_result(NULL, target, 0U, "invalid stream input");
    }
    return error_result(result, target, 0U,
                        reason ? reason : "binary transfer aborted");
}
