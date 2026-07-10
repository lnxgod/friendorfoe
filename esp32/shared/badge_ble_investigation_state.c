#include "badge_ble_investigation_state.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static bool request_id_equal(const char *left, const char *right)
{
    if (!left || !right) return false;
    return strncmp(left, right, BLE_INV_REQUEST_ID_LEN) == 0 &&
           memchr(left, '\0', BLE_INV_REQUEST_ID_LEN) != NULL &&
           memchr(right, '\0', BLE_INV_REQUEST_ID_LEN) != NULL;
}

static bool copy_text(char *out, size_t out_len, const char *text)
{
    if (!out || out_len == 0 || !text) return false;
    size_t len = strnlen(text, out_len);
    if (len >= out_len) {
        out[0] = '\0';
        return false;
    }
    memcpy(out, text, len + 1);
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
    va_list args;
    va_start(args, format);
    int written = vsnprintf(out, out_len, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= out_len) {
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
    state->selected_chunk = -1;
}

bool badge_ble_investigation_state_start(
    badge_ble_investigation_state_t *state,
    const ble_investigation_request_t *request,
    bool scanner_available,
    int *scanner_slot_out)
{
    if (scanner_slot_out) *scanner_slot_out = -1;
    if (!state || !request || !scanner_available || state->active ||
        request->request_id[0] == '\0' ||
        !ble_investigation_mode_name(request->mode) ||
        memchr(request->request_id, '\0', sizeof(request->request_id)) == NULL ||
        memchr(request->target_mac, '\0', sizeof(request->target_mac)) == NULL) {
        return false;
    }

    badge_ble_investigation_state_t next;
    badge_ble_investigation_state_init(&next);
    if (!copy_text(next.result.request_id, sizeof(next.result.request_id),
                   request->request_id) ||
        !copy_text(next.result.target_mac, sizeof(next.result.target_mac),
                   request->target_mac)) {
        return false;
    }
    next.result.mode = request->mode;
    next.result.state = BLE_INV_QUEUED;
    next.active = true;
    *state = next;
    if (scanner_slot_out) {
        *scanner_slot_out = BADGE_BLE_INVESTIGATION_SCANNER_SLOT;
    }
    return true;
}

bool badge_ble_investigation_state_accept(
    badge_ble_investigation_state_t *state,
    const ble_investigation_chunk_t *chunk)
{
    if (!state || !chunk || !state->active || state->end_received ||
        state->chunk_count >= BADGE_BLE_INVESTIGATION_MAX_CHUNKS ||
        !request_id_equal(state->result.request_id, chunk->request_id)) {
        return false;
    }

    badge_ble_investigation_state_t next = *state;
    if (chunk->kind == BLE_INV_CHUNK_BEGIN) {
        if (next.begin_received || chunk->mode != next.result.mode ||
            strncmp(chunk->target_mac, next.result.target_mac,
                    sizeof(next.result.target_mac)) != 0) {
            return false;
        }
        ble_investigation_result_t assembled;
        ble_investigation_result_init(&assembled);
        if (!ble_investigation_result_accept(&assembled, chunk)) return false;
        next.result = assembled;
        next.begin_received = true;
    } else {
        if (!next.begin_received ||
            !ble_investigation_result_accept(&next.result, chunk)) {
            return false;
        }
        if (chunk->kind == BLE_INV_CHUNK_END) {
            next.end_received = true;
            next.active = false;
        }
    }

    next.chunks[next.chunk_count++] = *chunk;
    *state = next;
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

bool badge_ble_investigation_state_select_chunk(
    badge_ble_investigation_state_t *state,
    const char *request_id,
    int seq)
{
    if (!state || !request_id ||
        !request_id_equal(state->result.request_id, request_id) ||
        seq < 0 || seq >= state->chunk_count) {
        return false;
    }
    state->selected_chunk = seq;
    return true;
}

bool badge_ble_investigation_state_get_selected_chunk(
    const badge_ble_investigation_state_t *state,
    ble_investigation_chunk_t *out)
{
    if (!state || !out || state->selected_chunk < 0 ||
        state->selected_chunk >= state->chunk_count) {
        return false;
    }
    *out = state->chunks[state->selected_chunk];
    return true;
}

size_t badge_ble_investigation_state_status_json(
    const badge_ble_investigation_state_t *state,
    char *out,
    size_t out_len)
{
    if (!out || out_len == 0) return 0;
    out[0] = '\0';
    if (!state) return 0;

    char request_id[BLE_INV_REQUEST_ID_LEN * 2];
    char summary[BLE_INV_SUMMARY_LEN * 2];
    char error[BLE_INV_ERROR_LEN * 2];
    if (!json_escape(request_id, sizeof(request_id), state->result.request_id,
                     sizeof(state->result.request_id)) ||
        !json_escape(summary, sizeof(summary), state->result.summary,
                     sizeof(state->result.summary)) ||
        !json_escape(error, sizeof(error), state->result.error,
                     sizeof(state->result.error))) {
        return 0;
    }
    const char *mode = ble_investigation_mode_name(state->result.mode);
    const char *result_state = ble_investigation_state_name(state->result.state);
    if (!mode || !result_state) return 0;

    return write_json(
        out, out_len,
        "{\"request_id\":\"%s\",\"state\":\"%s\",\"mode\":\"%s\","
        "\"summary\":\"%s\",\"error\":\"%s\",\"service_count\":%u,"
        "\"characteristic_count\":%u,\"authentication_required\":%s,"
        "\"truncated\":%s}",
        request_id, result_state, mode, summary, error,
        (unsigned)state->result.service_count,
        (unsigned)state->result.characteristic_count,
        state->result.authentication_required ? "true" : "false",
        state->result.truncated ? "true" : "false");
}

bool badge_ble_investigation_chunk_read_authorized(bool encrypted,
                                                   bool bonded)
{
    return encrypted && bonded;
}
