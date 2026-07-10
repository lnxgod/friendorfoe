#include "badge_ble_investigation_state.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static size_t bounded_length(const char *text, size_t bound)
{
    if (!text) return 0;
    size_t len = 0;
    while (len < bound && text[len] != '\0') ++len;
    return len;
}

static bool bounded_text(const char *text, size_t bound)
{
    return text && bounded_length(text, bound) < bound;
}

static bool request_id_valid(const char *request_id)
{
    size_t len = bounded_length(request_id, BLE_INV_REQUEST_ID_LEN);
    if (len == 0 || len >= BLE_INV_REQUEST_ID_LEN) return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)request_id[i];
        if (ch < 0x21 || ch > 0x7e) return false;
    }
    return true;
}

static bool request_id_equal(const char *left, const char *right)
{
    if (!request_id_valid(left) || !request_id_valid(right)) return false;
    size_t left_len = bounded_length(left, BLE_INV_REQUEST_ID_LEN);
    size_t right_len = bounded_length(right, BLE_INV_REQUEST_ID_LEN);
    return left_len == right_len && memcmp(left, right, left_len) == 0;
}

static bool hex_digit(char ch)
{
    return (ch >= '0' && ch <= '9') ||
           (ch >= 'A' && ch <= 'F') ||
           (ch >= 'a' && ch <= 'f');
}

static char upper_hex(char ch)
{
    return ch >= 'a' && ch <= 'f' ? (char)(ch - 'a' + 'A') : ch;
}

static bool normalize_mac(const char *text, char out[18])
{
    if (!text || strlen(text) != 17) return false;
    for (int i = 0; i < 17; ++i) {
        if ((i + 1) % 3 == 0) {
            if (text[i] != ':') return false;
            out[i] = ':';
        } else {
            if (!hex_digit(text[i])) return false;
            out[i] = upper_hex(text[i]);
        }
    }
    out[17] = '\0';
    return true;
}

static void copy_text(char *out, size_t out_len, const char *text)
{
    size_t len = bounded_length(text, out_len);
    if (len >= out_len) len = out_len - 1;
    if (len > 0) memcpy(out, text, len);
    out[len] = '\0';
}

bool badge_ble_investigation_request_validate(
    const ble_investigation_request_t *request,
    ble_investigation_request_t *normalized)
{
    if (!request || !request_id_valid(request->request_id) ||
        !bounded_text(request->target_mac, sizeof(request->target_mac)) ||
        !ble_investigation_mode_name(request->mode)) {
        return false;
    }

    ble_investigation_request_t checked = *request;
    if (request->mode == BLE_INV_MODE_GATT) {
        if (!normalize_mac(request->target_mac, checked.target_mac)) return false;
    } else if (request->target_mac[0] != '\0') {
        return false;
    }
    if (normalized) *normalized = checked;
    return true;
}

bool badge_ble_investigation_index_from_number(double value, int *out)
{
    if (!out || !isfinite(value) || value < 0.0 || value > (double)INT_MAX) {
        return false;
    }
    double integer = 0.0;
    if (modf(value, &integer) != 0.0) return false;
    *out = (int)integer;
    return true;
}

static bool upper_hex_digit(char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F');
}

bool badge_ble_investigation_uuid_is_canonical(const char *uuid)
{
    if (!uuid) return false;
    size_t len = strlen(uuid);
    if (len == 4 || len == 8) {
        for (size_t i = 0; i < len; ++i) {
            if (!upper_hex_digit(uuid[i])) return false;
        }
        return true;
    }
    if (len != 36) return false;
    for (size_t i = 0; i < len; ++i) {
        bool hyphen = i == 8 || i == 13 || i == 18 || i == 23;
        if (hyphen ? uuid[i] != '-' : !upper_hex_digit(uuid[i])) return false;
    }
    return true;
}

bool badge_ble_investigation_value_hex_is_valid(const char *value_hex)
{
    if (!value_hex) return false;
    size_t len = strlen(value_hex);
    if (len >= BLE_INV_READ_HEX_LEN || (len % 2) != 0) return false;
    for (size_t i = 0; i < len; ++i) {
        if (!hex_digit(value_hex[i])) return false;
    }
    return true;
}

static bool json_escape(char *out,
                        size_t out_len,
                        const char *text,
                        size_t text_bound)
{
    if (!out || out_len == 0 || !text) return false;
    size_t used = 0;
    for (size_t i = 0; i < text_bound; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '\0') {
            out[used] = '\0';
            return true;
        }
        const char *escape = NULL;
        switch (ch) {
        case '"': escape = "\\\""; break;
        case '\\': escape = "\\\\"; break;
        case '\b': escape = "\\b"; break;
        case '\f': escape = "\\f"; break;
        case '\n': escape = "\\n"; break;
        case '\r': escape = "\\r"; break;
        case '\t': escape = "\\t"; break;
        default: break;
        }
        if (escape) {
            if (used + 2 >= out_len) break;
            out[used++] = escape[0];
            out[used++] = escape[1];
        } else {
            if (used + 1 >= out_len) break;
            out[used++] = ch >= 0x20 && ch <= 0x7e ? (char)ch : '?';
        }
    }
    out[0] = '\0';
    return false;
}

static size_t write_json(char *out, size_t out_len, const char *format, ...)
{
    if (!out || out_len == 0 || !format) return 0;
    size_t bounded_out_len = out_len < UART_JSON_MAX_SIZE
        ? out_len
        : UART_JSON_MAX_SIZE;
    va_list args;
    va_start(args, format);
    int written = vsnprintf(out, bounded_out_len, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= bounded_out_len) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)written;
}

void badge_ble_investigation_state_init(
    badge_ble_investigation_state_t *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    ble_investigation_result_init(&state->result);
}

static int64_t investigation_deadline_ms(int64_t now_ms, uint32_t timeout_ms)
{
    if (now_ms < 0) now_ms = 0;
    uint32_t bounded_timeout = timeout_ms == 0
        ? BLE_INV_DEFAULT_TIMEOUT_MS : timeout_ms;
    if (bounded_timeout > BLE_INV_DEFAULT_TIMEOUT_MS) {
        bounded_timeout = BLE_INV_DEFAULT_TIMEOUT_MS;
    }
    int64_t duration_ms = (int64_t)bounded_timeout +
        BADGE_BLE_INVESTIGATION_TRANSPORT_GRACE_MS;
    return now_ms > INT64_MAX - duration_ms
        ? INT64_MAX : now_ms + duration_ms;
}

bool badge_ble_investigation_state_start_at(
    badge_ble_investigation_state_t *state,
    const ble_investigation_request_t *request,
    bool scanner_available,
    int64_t now_ms,
    int *scanner_slot_out)
{
    if (scanner_slot_out) *scanner_slot_out = -1;
    ble_investigation_request_t checked;
    if (!state || !scanner_available || state->active ||
        !badge_ble_investigation_request_validate(request, &checked)) {
        return false;
    }

    badge_ble_investigation_state_init(state);
    copy_text(state->result.request_id, sizeof(state->result.request_id),
              checked.request_id);
    copy_text(state->result.target_mac, sizeof(state->result.target_mac),
              checked.target_mac);
    state->result.mode = checked.mode;
    state->result.state = BLE_INV_QUEUED;
    state->deadline_ms = investigation_deadline_ms(now_ms, checked.timeout_ms);
    state->active = true;
    if (scanner_slot_out) *scanner_slot_out = BADGE_BLE_INVESTIGATION_SCANNER_SLOT;
    return true;
}

bool badge_ble_investigation_state_start(
    badge_ble_investigation_state_t *state,
    const ble_investigation_request_t *request,
    bool scanner_available,
    int *scanner_slot_out)
{
    return badge_ble_investigation_state_start_at(
        state, request, scanner_available, 0, scanner_slot_out);
}

static bool chunk_text_fields_valid(const ble_investigation_chunk_t *chunk)
{
    return chunk && request_id_valid(chunk->request_id) &&
           bounded_text(chunk->target_mac, sizeof(chunk->target_mac)) &&
           bounded_text(chunk->service_uuid, sizeof(chunk->service_uuid)) &&
           bounded_text(chunk->uuid, sizeof(chunk->uuid)) &&
           bounded_text(chunk->value_hex, sizeof(chunk->value_hex)) &&
           bounded_text(chunk->summary, sizeof(chunk->summary)) &&
           bounded_text(chunk->error, sizeof(chunk->error));
}

static bool chunk_valid_for_state(const badge_ble_investigation_state_t *state,
                                  const ble_investigation_chunk_t *chunk)
{
    if (!state || !chunk_text_fields_valid(chunk) || !state->active ||
        state->end_received ||
        !request_id_equal(state->result.request_id, chunk->request_id)) {
        return false;
    }
    switch (chunk->kind) {
    case BLE_INV_CHUNK_BEGIN: {
        ble_investigation_request_t request = {0};
        copy_text(request.request_id, sizeof(request.request_id), chunk->request_id);
        copy_text(request.target_mac, sizeof(request.target_mac), chunk->target_mac);
        request.mode = chunk->mode;
        ble_investigation_request_t checked;
        return !state->begin_received &&
               badge_ble_investigation_request_validate(&request, &checked) &&
               checked.mode == state->result.mode &&
               strcmp(checked.target_mac, state->result.target_mac) == 0;
    }
    case BLE_INV_CHUNK_PROGRESS:
        return state->begin_received &&
               chunk->state >= BLE_INV_QUEUED &&
               chunk->state <= BLE_INV_READING &&
               chunk->state >= state->result.state;
    case BLE_INV_CHUNK_SERVICE:
        return state->begin_received && chunk->index >= 0 &&
               chunk->index == state->result.service_count &&
               badge_ble_investigation_uuid_is_canonical(chunk->uuid);
    case BLE_INV_CHUNK_CHARACTERISTIC:
        return state->begin_received && chunk->index >= 0 &&
               chunk->index == state->result.characteristic_count &&
               badge_ble_investigation_uuid_is_canonical(chunk->service_uuid) &&
               badge_ble_investigation_uuid_is_canonical(chunk->uuid);
    case BLE_INV_CHUNK_READ:
        return state->begin_received && chunk->index >= 0 &&
               chunk->index == state->result.read_count &&
               badge_ble_investigation_uuid_is_canonical(chunk->uuid) &&
               badge_ble_investigation_value_hex_is_valid(chunk->value_hex);
    case BLE_INV_CHUNK_END:
        return state->begin_received && chunk->state >= BLE_INV_COMPLETE &&
               chunk->state <= BLE_INV_CANCELLED;
    default:
        return false;
    }
}

static void apply_chunk_to_result(badge_ble_investigation_state_t *state,
                                  const ble_investigation_chunk_t *chunk)
{
    switch (chunk->kind) {
    case BLE_INV_CHUNK_BEGIN:
        state->begin_received = true;
        return;
    case BLE_INV_CHUNK_PROGRESS:
        state->result.state = chunk->state;
        return;
    case BLE_INV_CHUNK_SERVICE:
        if (state->result.service_count < BLE_INV_MAX_SERVICES) {
            copy_text(state->result.services[state->result.service_count],
                      BLE_INV_UUID_LEN, chunk->uuid);
            state->result.service_count++;
        } else {
            state->result.truncated = true;
        }
        return;
    case BLE_INV_CHUNK_CHARACTERISTIC:
        if (state->result.characteristic_count < BLE_INV_MAX_CHARS) {
            ble_investigation_characteristic_t *out =
                &state->result.characteristics[state->result.characteristic_count++];
            copy_text(out->service_uuid, sizeof(out->service_uuid),
                      chunk->service_uuid);
            copy_text(out->uuid, sizeof(out->uuid), chunk->uuid);
            out->properties = chunk->properties;
        } else {
            state->result.truncated = true;
        }
        return;
    case BLE_INV_CHUNK_READ:
        if (state->result.read_count < BLE_INV_MAX_READS) {
            ble_investigation_read_t *out =
                &state->result.reads[state->result.read_count++];
            copy_text(out->uuid, sizeof(out->uuid), chunk->uuid);
            copy_text(out->value_hex, sizeof(out->value_hex), chunk->value_hex);
        } else {
            state->result.truncated = true;
        }
        return;
    case BLE_INV_CHUNK_END:
        state->result.state = chunk->state;
        state->result.authentication_required = chunk->authentication_required;
        state->result.truncated = state->result.truncated || chunk->truncated;
        copy_text(state->result.summary, sizeof(state->result.summary),
                  chunk->summary);
        copy_text(state->result.error, sizeof(state->result.error), chunk->error);
        state->end_received = true;
        state->active = false;
        state->deadline_ms = 0;
        return;
    default:
        return;
    }
}

bool badge_ble_investigation_state_accept(
    badge_ble_investigation_state_t *state,
    const ble_investigation_chunk_t *chunk)
{
    if (!chunk_valid_for_state(state, chunk)) return false;
    if (chunk->kind == BLE_INV_CHUNK_PROGRESS &&
        chunk->state == state->result.state) {
        return true;
    }

    apply_chunk_to_result(state, chunk);
    bool terminal = chunk->kind == BLE_INV_CHUNK_END;
    if (!terminal &&
        state->chunk_count >= BADGE_BLE_INVESTIGATION_MAX_CHUNKS - 1) {
        state->result.truncated = true;
        return true;
    }
    if (terminal &&
        state->chunk_count >= BADGE_BLE_INVESTIGATION_MAX_CHUNKS) {
        state->result.truncated = true;
        state->chunks[BADGE_BLE_INVESTIGATION_MAX_CHUNKS - 1] = *chunk;
        state->chunks[BADGE_BLE_INVESTIGATION_MAX_CHUNKS - 1].truncated =
            state->result.truncated;
        state->chunk_count = BADGE_BLE_INVESTIGATION_MAX_CHUNKS;
        return true;
    }
    state->chunks[state->chunk_count] = *chunk;
    if (terminal) {
        state->chunks[state->chunk_count].truncated = state->result.truncated;
    }
    state->chunk_count++;
    return true;
}

bool badge_ble_investigation_state_expire(
    badge_ble_investigation_state_t *state,
    int64_t now_ms,
    badge_ble_investigation_expiry_t *expiry_out)
{
    if (expiry_out) memset(expiry_out, 0, sizeof(*expiry_out));
    if (!state || !state->active || state->end_received ||
        state->deadline_ms <= 0 || now_ms < state->deadline_ms) {
        return false;
    }

    uint8_t first_seq = state->chunk_count;
    uint8_t synthesized_count = 0;
    if (!state->begin_received) {
        if (state->chunk_count >= BADGE_BLE_INVESTIGATION_MAX_CHUNKS - 1) {
            state->chunk_count = BADGE_BLE_INVESTIGATION_MAX_CHUNKS - 2;
            state->result.truncated = true;
            first_seq = state->chunk_count;
        }
        ble_investigation_chunk_t *begin =
            &state->chunks[state->chunk_count];
        memset(begin, 0, sizeof(*begin));
        begin->kind = BLE_INV_CHUNK_BEGIN;
        begin->mode = state->result.mode;
        copy_text(begin->request_id, sizeof(begin->request_id),
                  state->result.request_id);
        copy_text(begin->target_mac, sizeof(begin->target_mac),
                  state->result.target_mac);
        if (!badge_ble_investigation_state_accept(state, begin)) return false;
        synthesized_count++;
    }

    uint8_t terminal_seq = state->chunk_count <
            BADGE_BLE_INVESTIGATION_MAX_CHUNKS
        ? state->chunk_count
        : BADGE_BLE_INVESTIGATION_MAX_CHUNKS - 1;
    if (synthesized_count == 0) first_seq = terminal_seq;
    ble_investigation_chunk_t *terminal = &state->chunks[terminal_seq];
    memset(terminal, 0, sizeof(*terminal));
    terminal->kind = BLE_INV_CHUNK_END;
    terminal->state = BLE_INV_FAILED;
    copy_text(terminal->request_id, sizeof(terminal->request_id),
              state->result.request_id);
    copy_text(terminal->summary, sizeof(terminal->summary), "timeout");
    copy_text(terminal->error, sizeof(terminal->error), "timeout");
    if (!badge_ble_investigation_state_accept(state, terminal)) return false;
    synthesized_count++;

    if (expiry_out) {
        copy_text(expiry_out->request_id, sizeof(expiry_out->request_id),
                  state->result.request_id);
        expiry_out->first_seq = first_seq;
        expiry_out->chunk_count = synthesized_count;
    }
    return true;
}

void badge_ble_investigation_pending_queue_init(
    badge_ble_investigation_pending_queue_t *queue)
{
    if (queue) memset(queue, 0, sizeof(*queue));
}

bool badge_ble_investigation_pending_queue_can_accept_expiry(
    const badge_ble_investigation_pending_queue_t *queue)
{
    return queue && queue->count <=
        BADGE_BLE_INVESTIGATION_PENDING_CHUNKS - 2;
}

bool badge_ble_investigation_pending_queue_enqueue_expiry(
    badge_ble_investigation_pending_queue_t *queue,
    const badge_ble_investigation_state_t *state,
    const badge_ble_investigation_expiry_t *expiry)
{
    if (!queue || !state || !expiry || expiry->chunk_count == 0 ||
        expiry->chunk_count > 2 ||
        queue->count > BADGE_BLE_INVESTIGATION_PENDING_CHUNKS ||
        expiry->chunk_count >
            BADGE_BLE_INVESTIGATION_PENDING_CHUNKS - queue->count ||
        expiry->first_seq >= state->chunk_count ||
        expiry->chunk_count > state->chunk_count - expiry->first_seq) {
        return false;
    }

    for (uint8_t i = 0; i < expiry->chunk_count; ++i) {
        const ble_investigation_chunk_t *chunk =
            &state->chunks[expiry->first_seq + i];
        if (!request_id_equal(chunk->request_id, expiry->request_id)) {
            return false;
        }
    }

    for (uint8_t i = 0; i < expiry->chunk_count; ++i) {
        uint8_t tail = (uint8_t)((queue->head + queue->count + i) %
            BADGE_BLE_INVESTIGATION_PENDING_CHUNKS);
        queue->chunks[tail] = state->chunks[expiry->first_seq + i];
    }
    queue->count = (uint8_t)(queue->count + expiry->chunk_count);
    return true;
}

bool badge_ble_investigation_pending_queue_peek(
    const badge_ble_investigation_pending_queue_t *queue,
    ble_investigation_chunk_t *out)
{
    if (!queue || !out || queue->count == 0 ||
        queue->head >= BADGE_BLE_INVESTIGATION_PENDING_CHUNKS) {
        return false;
    }
    *out = queue->chunks[queue->head];
    return true;
}

bool badge_ble_investigation_pending_queue_consume(
    badge_ble_investigation_pending_queue_t *queue)
{
    if (!queue || queue->count == 0 ||
        queue->head >= BADGE_BLE_INVESTIGATION_PENDING_CHUNKS) {
        return false;
    }
    memset(&queue->chunks[queue->head], 0,
           sizeof(queue->chunks[queue->head]));
    queue->head = (uint8_t)((queue->head + 1) %
        BADGE_BLE_INVESTIGATION_PENDING_CHUNKS);
    queue->count--;
    return true;
}

void badge_ble_investigation_state_get(
    const badge_ble_investigation_state_t *state,
    ble_investigation_result_t *out)
{
    if (!out) return;
    if (!state) {
        ble_investigation_result_init(out);
        return;
    }
    *out = state->result;
}

void badge_ble_investigation_state_transport_lost(
    badge_ble_investigation_state_t *state)
{
    (void)state;
}

bool badge_ble_investigation_state_get_chunk(
    const badge_ble_investigation_state_t *state,
    const char *request_id,
    int seq,
    ble_investigation_chunk_t *out)
{
    if (!state || !out || !request_id_equal(state->result.request_id, request_id) ||
        seq < 0 || seq >= state->chunk_count) {
        return false;
    }
    *out = state->chunks[seq];
    return true;
}

void badge_ble_investigation_selection_init(
    badge_ble_investigation_selection_t *selection)
{
    if (selection) memset(selection, 0, sizeof(*selection));
}

bool badge_ble_investigation_selection_set(
    badge_ble_investigation_selection_t *selection,
    const badge_ble_investigation_state_t *state,
    const char *request_id,
    int seq)
{
    ble_investigation_chunk_t chunk;
    if (!selection ||
        !badge_ble_investigation_state_get_chunk(state, request_id, seq, &chunk)) {
        return false;
    }
    copy_text(selection->request_id, sizeof(selection->request_id), request_id);
    selection->seq = seq;
    selection->valid = true;
    return true;
}

void badge_ble_investigation_selection_clear(
    badge_ble_investigation_selection_t *selection)
{
    badge_ble_investigation_selection_init(selection);
}

bool badge_ble_investigation_selection_get(
    const badge_ble_investigation_selection_t *selection,
    const badge_ble_investigation_state_t *state,
    ble_investigation_chunk_t *out)
{
    return selection && selection->valid &&
           badge_ble_investigation_state_get_chunk(
               state, selection->request_id, selection->seq, out);
}

void badge_ble_investigation_start_fence_init(
    badge_ble_investigation_start_fence_t *fence)
{
    if (fence) memset(fence, 0, sizeof(*fence));
}

uint32_t badge_ble_investigation_start_fence_reserve(
    badge_ble_investigation_start_fence_t *fence,
    uint32_t revision)
{
    if (!fence || fence->pending) return 0;
    fence->generation++;
    if (fence->generation == 0) fence->generation++;
    fence->pending_generation = fence->generation;
    fence->revision_at_reserve = revision;
    fence->pending = true;
    return fence->pending_generation;
}

bool badge_ble_investigation_start_fence_should_rollback(
    badge_ble_investigation_start_fence_t *fence,
    uint32_t generation,
    uint32_t current_revision,
    bool send_succeeded)
{
    if (!fence || !fence->pending || generation == 0 ||
        generation != fence->pending_generation) {
        return false;
    }
    fence->pending = false;
    return !send_succeeded && current_revision == fence->revision_at_reserve;
}

void badge_ble_investigation_state_status(
    const badge_ble_investigation_state_t *state,
    badge_ble_investigation_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->mode = BLE_INV_MODE_GATT;
    out->state = BLE_INV_IDLE;
    if (!state) return;
    copy_text(out->request_id, sizeof(out->request_id), state->result.request_id);
    out->mode = state->result.mode;
    out->state = state->result.state;
    copy_text(out->summary, sizeof(out->summary), state->result.summary);
    copy_text(out->error, sizeof(out->error), state->result.error);
    out->service_count = state->result.service_count;
    out->characteristic_count = state->result.characteristic_count;
    out->authentication_required = state->result.authentication_required;
    out->truncated = state->result.truncated;
}

size_t badge_ble_investigation_status_to_json(
    const badge_ble_investigation_status_t *status,
    char *out,
    size_t out_len)
{
    if (!out || out_len == 0) return 0;
    out[0] = '\0';
    if (!status) return 0;
    char request_id[BLE_INV_REQUEST_ID_LEN * 2];
    char summary[BLE_INV_SUMMARY_LEN * 2];
    char error[BLE_INV_ERROR_LEN * 2];
    if (!json_escape(request_id, sizeof(request_id), status->request_id,
                     sizeof(status->request_id)) ||
        !json_escape(summary, sizeof(summary), status->summary,
                     sizeof(status->summary)) ||
        !json_escape(error, sizeof(error), status->error,
                     sizeof(status->error))) {
        return 0;
    }
    const char *mode = ble_investigation_mode_name(status->mode);
    const char *state = ble_investigation_state_name(status->state);
    if (!mode || !state) return 0;
    return write_json(
        out, out_len,
        "{\"request_id\":\"%s\",\"state\":\"%s\",\"mode\":\"%s\","
        "\"summary\":\"%s\",\"error\":\"%s\",\"service_count\":%u,"
        "\"characteristic_count\":%u,\"authentication_required\":%s,"
        "\"truncated\":%s}",
        request_id, state, mode, summary, error,
        (unsigned)status->service_count,
        (unsigned)status->characteristic_count,
        status->authentication_required ? "true" : "false",
        status->truncated ? "true" : "false");
}

size_t badge_ble_investigation_state_status_json(
    const badge_ble_investigation_state_t *state,
    char *out,
    size_t out_len)
{
    badge_ble_investigation_status_t status;
    badge_ble_investigation_state_status(state, &status);
    return badge_ble_investigation_status_to_json(&status, out, out_len);
}

size_t badge_ble_investigation_usb_frame(const char *chunk_json,
                                         char *out,
                                         size_t out_len)
{
    if (!out || out_len == 0) return 0;
    out[0] = '\0';
    if (!chunk_json || strchr(chunk_json, '\n') || strchr(chunk_json, '\r')) return 0;
    size_t json_len = strlen(chunk_json);
    static const char prefix[] = "FOF_INV:";
    size_t frame_len = sizeof(prefix) - 1 + json_len + 1;
    if (json_len == 0 || json_len >= UART_JSON_MAX_SIZE ||
        frame_len + 1 > out_len) {
        return 0;
    }
    memcpy(out, prefix, sizeof(prefix) - 1);
    memcpy(out + sizeof(prefix) - 1, chunk_json, json_len);
    out[frame_len - 1] = '\n';
    out[frame_len] = '\0';
    return frame_len;
}

bool badge_ble_investigation_chunk_read_authorized(bool encrypted,
                                                   bool bonded)
{
    return encrypted && bonded;
}
