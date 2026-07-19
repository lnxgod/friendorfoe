#include "ble_investigator.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define APPLE_COMPANY_ID       0x004C
#define MICROSOFT_COMPANY_ID   0x0006
#define FAST_PAIR_SERVICE_UUID 0xFE2C
#define APPLE_AIRPODS_TYPE     0x07
#define APPLE_NEARBY_ACTION    0x0F
#define APPLE_NEARBY_INFO      0x10
#define PASSIVE_CONNECTABLE    0x01

static size_t bounded_length(const char *text, size_t bound)
{
    if (!text) return 0;
    size_t len = 0;
    while (len < bound && text[len] != '\0') ++len;
    return len;
}

static bool copy_text(char *out, size_t out_len,
                      const char *text, size_t text_bound)
{
    if (!out || out_len == 0 || !text) return false;
    size_t len = bounded_length(text, text_bound);
    bool complete = len < text_bound && len < out_len;
    if (len >= out_len) len = out_len - 1;
    memcpy(out, text, len);
    out[len] = '\0';
    return complete;
}

bool ble_investigator_request_id_is_valid(const char *request_id)
{
    size_t len = bounded_length(request_id, BLE_INV_REQUEST_ID_LEN);
    if (len == 0 || len >= BLE_INV_REQUEST_ID_LEN) return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)request_id[i];
        if (ch < 0x21 || ch > 0x7E) return false;
    }
    return true;
}

ble_investigator_request_decision_t ble_investigator_decide_request(
    bool runtime_busy,
    const char *active_request_id,
    const char *incoming_request_id)
{
    if (!ble_investigator_request_id_is_valid(incoming_request_id)) {
        return BLE_INV_REQUEST_INVALID;
    }
    if (!runtime_busy) return BLE_INV_REQUEST_AVAILABLE;
    if (ble_investigator_request_id_is_valid(active_request_id)) {
        size_t active_len = bounded_length(
            active_request_id, BLE_INV_REQUEST_ID_LEN);
        size_t incoming_len = bounded_length(
            incoming_request_id, BLE_INV_REQUEST_ID_LEN);
        if (active_len == incoming_len &&
            memcmp(active_request_id, incoming_request_id, active_len) == 0) {
            return BLE_INV_REQUEST_RETRANSMIT;
        }
    }
    return BLE_INV_REQUEST_BUSY_REJECTION;
}

static bool target_is_present(const char target_mac[18])
{
    return bounded_length(target_mac, 18) > 0;
}

static int hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

bool ble_investigator_parse_target_mac(const char *text, uint8_t out[6])
{
    if (!text || !out || strlen(text) != 17) return false;
    for (int index = 0; index < 6; ++index) {
        int offset = index * 3;
        int high = hex_nibble(text[offset]);
        int low = hex_nibble(text[offset + 1]);
        if (high < 0 || low < 0 ||
            (index < 5 && text[offset + 2] != ':')) {
            return false;
        }
        out[5 - index] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static bool error_token_is_valid(const char *error)
{
    size_t len = bounded_length(error, BLE_INV_ERROR_LEN);
    if (len == 0 || len >= BLE_INV_ERROR_LEN) return false;
    for (size_t i = 0; i < len; ++i) {
        char ch = error[i];
        if ((ch < 'a' || ch > 'z') && (ch < '0' || ch > '9') && ch != '_') {
            return false;
        }
    }
    return true;
}

bool ble_investigator_build_rejection_chunks(
    const char *request_id,
    ble_investigation_mode_t mode,
    const char *target_mac,
    const char *error,
    ble_investigation_chunk_t chunks[2])
{
    if (!chunks || !ble_investigator_request_id_is_valid(request_id) ||
        !error_token_is_valid(error)) {
        return false;
    }

    memset(chunks, 0, sizeof(ble_investigation_chunk_t) * 2);
    ble_investigation_mode_t safe_mode =
        mode == BLE_INV_MODE_GATT || mode == BLE_INV_MODE_PASSIVE_CAPTURE
            ? mode
            : BLE_INV_MODE_PASSIVE_CAPTURE;
    chunks[0].kind = BLE_INV_CHUNK_BEGIN;
    chunks[0].mode = safe_mode;
    copy_text(chunks[0].request_id, sizeof(chunks[0].request_id),
              request_id, BLE_INV_REQUEST_ID_LEN);
    if (safe_mode == BLE_INV_MODE_GATT && target_mac) {
        uint8_t parsed_mac[6];
        if (ble_investigator_parse_target_mac(target_mac, parsed_mac)) {
            copy_text(chunks[0].target_mac, sizeof(chunks[0].target_mac),
                      target_mac, 18);
        }
    }

    chunks[1].kind = BLE_INV_CHUNK_END;
    chunks[1].state = BLE_INV_FAILED;
    copy_text(chunks[1].request_id, sizeof(chunks[1].request_id),
              request_id, BLE_INV_REQUEST_ID_LEN);
    snprintf(chunks[1].summary, sizeof(chunks[1].summary),
             "BLE investigation rejected: %s", error);
    copy_text(chunks[1].error, sizeof(chunks[1].error),
              error, BLE_INV_ERROR_LEN);
    return true;
}

bool ble_investigator_passive_start_is_ready(
    const ble_investigation_request_t *request,
    bool scanner_scanning)
{
    if (!request) return false;
    if (request->mode == BLE_INV_MODE_GATT) return true;
    return request->mode == BLE_INV_MODE_PASSIVE_CAPTURE && scanner_scanning;
}

bool ble_investigator_host_start_is_allowed(bool investigation_gatt_active,
                                            bool resume_pending)
{
    return !investigation_gatt_active || resume_pending;
}

bool ble_investigator_scan_start_is_allowed(bool investigation_active,
                                            bool investigation_resume)
{
    return !investigation_active || investigation_resume;
}

bool ble_investigator_fingerprint_is_swift_pair(
    const ble_fingerprint_t *fingerprint)
{
    return fingerprint &&
           fingerprint->company_id == MICROSOFT_COMPANY_ID &&
           fingerprint->raw_mfr_len >= 3 &&
           fingerprint->raw_mfr[2] == 0x03;
}

bool ble_investigator_peer_cache_is_fresh(int64_t last_seen_ms,
                                          int64_t now_ms)
{
    return last_seen_ms >= 0 && now_ms >= last_seen_ms &&
           now_ms - last_seen_ms < BLE_INV_PEER_CACHE_FRESH_MS;
}

static void finish(ble_investigator_t *state,
                   ble_investigation_state_t terminal_state,
                   const char *summary,
                   const char *error)
{
    if (!state || terminal_state < BLE_INV_COMPLETE ||
        terminal_state > BLE_INV_CANCELLED) {
        return;
    }
    state->state = terminal_state;
    state->result.state = terminal_state;
    snprintf(state->result.summary, sizeof(state->result.summary), "%s",
             summary ? summary : "");
    snprintf(state->result.error, sizeof(state->result.error), "%s",
             error ? error : "");
    state->busy = false;
    state->resume_scan_required = true;
    state->result_pending = true;
}

static void fail_with_status(ble_investigator_t *state,
                             const char *error,
                             int status)
{
    char summary[BLE_INV_SUMMARY_LEN];
    snprintf(summary, sizeof(summary), "%s (status=%d)", error, status);
    finish(state, BLE_INV_FAILED, summary, error);
}

static void accept_service(ble_investigator_t *state,
                           const ble_investigator_event_t *event)
{
    ble_investigation_result_t *result = &state->result;
    if (result->service_count >= BLE_INV_MAX_SERVICES) {
        result->truncated = true;
        return;
    }
    if (!copy_text(result->services[result->service_count], BLE_INV_UUID_LEN,
                   event->uuid, sizeof(event->uuid))) {
        result->truncated = true;
    }
    ++result->service_count;
}

static void accept_characteristic(ble_investigator_t *state,
                                  const ble_investigator_event_t *event)
{
    ble_investigation_result_t *result = &state->result;
    if (result->characteristic_count >= BLE_INV_MAX_CHARS) {
        result->truncated = true;
        return;
    }
    ble_investigation_characteristic_t *characteristic =
        &result->characteristics[result->characteristic_count];
    if (!copy_text(characteristic->service_uuid,
                   sizeof(characteristic->service_uuid),
                   event->service_uuid, sizeof(event->service_uuid)) ||
        !copy_text(characteristic->uuid, sizeof(characteristic->uuid),
                   event->uuid, sizeof(event->uuid))) {
        result->truncated = true;
    }
    characteristic->properties = event->properties;
    ++result->characteristic_count;
}

static void accept_read(ble_investigator_t *state,
                        const ble_investigator_event_t *event)
{
    ble_investigation_result_t *result = &state->result;
    if (result->read_count >= BLE_INV_MAX_READS) {
        result->truncated = true;
        return;
    }
    ble_investigation_read_t *read = &result->reads[result->read_count];
    if (!copy_text(read->uuid, sizeof(read->uuid),
                   event->uuid, sizeof(event->uuid))) {
        result->truncated = true;
    }

    size_t value_len = event->value_len;
    size_t max_value_len = (BLE_INV_READ_HEX_LEN - 1) / 2;
    if (value_len > max_value_len) {
        value_len = max_value_len;
        result->truncated = true;
    }
    if (!event->value && value_len > 0) {
        value_len = 0;
        result->truncated = true;
    }
    for (size_t i = 0; i < value_len; ++i) {
        snprintf(&read->value_hex[i * 2], 3, "%02X", event->value[i]);
    }
    read->value_hex[value_len * 2] = '\0';
    ++result->read_count;
}

void ble_investigator_init(ble_investigator_t *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->state = BLE_INV_IDLE;
    state->result.state = BLE_INV_IDLE;
    state->result.mode = BLE_INV_MODE_GATT;
}

bool ble_investigator_start(ble_investigator_t *state,
                            const ble_investigation_request_t *request,
                            int64_t now_ms)
{
    if (!state || !request || state->busy || state->result_pending ||
        !ble_investigator_request_id_is_valid(request->request_id) ||
        (request->mode != BLE_INV_MODE_GATT &&
         request->mode != BLE_INV_MODE_PASSIVE_CAPTURE) ||
        (request->mode == BLE_INV_MODE_GATT && !target_is_present(request->target_mac)) ||
        (request->mode == BLE_INV_MODE_PASSIVE_CAPTURE && target_is_present(request->target_mac))) {
        return false;
    }

    ble_investigator_init(state);
    state->request = *request;
    uint32_t timeout_ms = request->timeout_ms == 0
        ? BLE_INV_DEFAULT_TIMEOUT_MS
        : request->timeout_ms;
    if (timeout_ms > BLE_INV_DEFAULT_TIMEOUT_MS) {
        timeout_ms = BLE_INV_DEFAULT_TIMEOUT_MS;
    }
    state->request.timeout_ms = timeout_ms;
    state->deadline_ms = now_ms + timeout_ms;
    state->busy = true;
    state->state = request->mode == BLE_INV_MODE_GATT
        ? BLE_INV_CONNECTING
        : BLE_INV_SCANNING;

    state->result.mode = request->mode;
    state->result.state = state->state;
    copy_text(state->result.request_id, sizeof(state->result.request_id),
              request->request_id, sizeof(request->request_id));
    copy_text(state->result.target_mac, sizeof(state->result.target_mac),
              request->target_mac, sizeof(request->target_mac));
    return true;
}

void ble_investigator_handle_event(ble_investigator_t *state,
                                   const ble_investigator_event_t *event,
                                   int64_t now_ms)
{
    if (!state || !event || !state->busy) return;
    if (state->request.mode == BLE_INV_MODE_PASSIVE_CAPTURE &&
        event->kind == BLE_INVESTIGATOR_EVENT_SCANNER_UNAVAILABLE &&
        state->state == BLE_INV_SCANNING) {
        finish(state, BLE_INV_FAILED,
               "Passive BLE scanner is unavailable",
               "scanner_unavailable");
        return;
    }
    if (now_ms >= state->deadline_ms) {
        ble_investigator_tick(state, now_ms);
        return;
    }
    if (state->request.mode != BLE_INV_MODE_GATT) {
        return;
    }

    switch (event->kind) {
    case BLE_INVESTIGATOR_EVENT_CONNECTED:
        if (state->state != BLE_INV_CONNECTING) break;
        state->connected = true;
        state->result.connectable = true;
        state->state = BLE_INV_DISCOVERING;
        state->result.state = BLE_INV_DISCOVERING;
        break;
    case BLE_INVESTIGATOR_EVENT_CONNECT_FAILED:
        if (state->state != BLE_INV_CONNECTING) break;
        fail_with_status(state,
                         event->uuid[0] ? event->uuid : "connect_failed",
                         event->status);
        break;
    case BLE_INVESTIGATOR_EVENT_SERVICE:
        if (state->state == BLE_INV_DISCOVERING) {
            accept_service(state, event);
        }
        break;
    case BLE_INVESTIGATOR_EVENT_CHARACTERISTIC:
        if (state->state == BLE_INV_DISCOVERING) {
            accept_characteristic(state, event);
        }
        break;
    case BLE_INVESTIGATOR_EVENT_READING_STARTED:
        if (state->state == BLE_INV_DISCOVERING) {
            state->state = BLE_INV_READING;
            state->result.state = BLE_INV_READING;
        }
        break;
    case BLE_INVESTIGATOR_EVENT_READ:
        if (state->state == BLE_INV_READING) {
            accept_read(state, event);
        }
        break;
    case BLE_INVESTIGATOR_EVENT_AUTH_REQUIRED:
        if (state->state != BLE_INV_DISCOVERING &&
            state->state != BLE_INV_READING) {
            break;
        }
        state->result.authentication_required = true;
        finish(state, BLE_INV_FAILED,
               "Readable attribute requires authentication or encryption",
               "authentication_required");
        break;
    case BLE_INVESTIGATOR_EVENT_PROCEDURE_FAILED:
        if (state->state == BLE_INV_DISCOVERING ||
            state->state == BLE_INV_READING) {
            fail_with_status(
                state,
                event->uuid[0] ? event->uuid : "procedure_failed",
                event->status);
        }
        break;
    case BLE_INVESTIGATOR_EVENT_DISCOVERY_COMPLETE: {
        if (state->state != BLE_INV_DISCOVERING &&
            state->state != BLE_INV_READING) {
            break;
        }
        char summary[BLE_INV_SUMMARY_LEN];
        snprintf(summary, sizeof(summary),
                 "%u services, %u characteristics, %u reads",
                 state->result.service_count,
                 state->result.characteristic_count,
                 state->result.read_count);
        finish(state, BLE_INV_COMPLETE, summary, NULL);
        break;
    }
    case BLE_INVESTIGATOR_EVENT_DISCONNECTED:
        if (!state->connected ||
            (state->state != BLE_INV_DISCOVERING &&
             state->state != BLE_INV_READING)) {
            break;
        }
        state->connected = false;
        fail_with_status(state, "disconnected", event->status);
        break;
    default:
        break;
    }
}

bool ble_investigator_prepare_procedure(
    ble_investigator_t *state,
    ble_investigation_state_t required_state,
    int64_t now_ms)
{
    if (!state || !state->busy || state->request.mode != BLE_INV_MODE_GATT) {
        return false;
    }
    if (now_ms >= state->deadline_ms) {
        ble_investigator_tick(state, now_ms);
        return false;
    }
    if (required_state != BLE_INV_CONNECTING &&
        required_state != BLE_INV_DISCOVERING &&
        required_state != BLE_INV_READING) {
        return false;
    }
    return state->state == required_state;
}

void ble_investigator_tick(ble_investigator_t *state, int64_t now_ms)
{
    if (!state || !state->busy || now_ms < state->deadline_ms) return;
    if (state->request.mode == BLE_INV_MODE_PASSIVE_CAPTURE) {
        char summary[BLE_INV_SUMMARY_LEN];
        snprintf(summary, sizeof(summary),
                 "Prompt families: Apple=%u Fast=%u Swift=%u",
                 state->passive_apple_count,
                 state->passive_fast_pair_count,
                 state->passive_swift_pair_count);
        finish(state, BLE_INV_COMPLETE, summary, NULL);
    } else {
        finish(state, BLE_INV_FAILED, "BLE investigation timed out", "timeout");
    }
}

void ble_investigator_cancel(ble_investigator_t *state, int64_t now_ms)
{
    if (!state || !state->busy) return;
    if (now_ms >= state->deadline_ms) {
        ble_investigator_tick(state, now_ms);
        return;
    }
    finish(state, BLE_INV_CANCELLED, "BLE investigation cancelled", "cancelled");
}

bool ble_investigator_take_result(ble_investigator_t *state,
                                  ble_investigation_result_t *out)
{
    if (!state || !out || !state->result_pending) return false;
    *out = state->result;
    ble_investigator_init(state);
    return true;
}

static bool runtime_fence_matches(
    const ble_investigator_runtime_fence_t *fence,
    uint32_t generation)
{
    return fence && fence->active && generation != 0 &&
           fence->generation == generation;
}

void ble_investigator_runtime_fence_init(
    ble_investigator_runtime_fence_t *fence)
{
    if (!fence) return;
    memset(fence, 0, sizeof(*fence));
    fence->expected_conn_handle = BLE_INV_CONN_HANDLE_NONE;
}

uint32_t ble_investigator_runtime_fence_begin(
    ble_investigator_runtime_fence_t *fence)
{
    if (!fence || fence->active) return 0;
    uint32_t generation = fence->generation + 1;
    if (generation == 0) generation = 1;
    ble_investigator_runtime_fence_init(fence);
    fence->generation = generation;
    fence->active = true;
    return generation;
}

bool ble_investigator_runtime_fence_mark_connecting(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation)
{
    if (!runtime_fence_matches(fence, generation) || fence->cleanup_pending ||
        fence->radio_state != BLE_INV_RADIO_IDLE) {
        return false;
    }
    fence->radio_state = BLE_INV_RADIO_CONNECTING;
    return true;
}

bool ble_investigator_runtime_fence_begin_operation(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation)
{
    if (!runtime_fence_matches(fence, generation) || fence->cleanup_pending ||
        fence->operation_in_progress) {
        return false;
    }
    fence->operation_in_progress = true;
    return true;
}

bool ble_investigator_runtime_fence_finish_operation(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation)
{
    if (!runtime_fence_matches(fence, generation) ||
        !fence->operation_in_progress) {
        return false;
    }
    fence->operation_in_progress = false;
    return true;
}

bool ble_investigator_runtime_reserve_operation(
    ble_investigator_t *state,
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation,
    ble_investigation_state_t required_state,
    int64_t now_ms)
{
    if (!runtime_fence_matches(fence, generation) ||
        fence->cleanup_pending || fence->operation_in_progress) {
        return false;
    }
    return ble_investigator_prepare_procedure(
               state, required_state, now_ms) &&
           ble_investigator_runtime_fence_begin_operation(
               fence, generation);
}

bool ble_investigator_runtime_fence_defer_discovery(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation,
    uint16_t conn_handle)
{
    if (!runtime_fence_matches(fence, generation) ||
        fence->cleanup_pending || !fence->operation_in_progress ||
        fence->deferred_discovery_pending ||
        fence->radio_state != BLE_INV_RADIO_CONNECTED ||
        conn_handle == BLE_INV_CONN_HANDLE_NONE ||
        fence->expected_conn_handle != conn_handle) {
        return false;
    }
    fence->deferred_discovery_pending = true;
    return true;
}

bool ble_investigator_runtime_fence_take_deferred_discovery(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation,
    bool launch_allowed,
    uint16_t *conn_handle_out)
{
    if (conn_handle_out) *conn_handle_out = BLE_INV_CONN_HANDLE_NONE;
    if (!runtime_fence_matches(fence, generation) ||
        !fence->deferred_discovery_pending ||
        fence->operation_in_progress) {
        return false;
    }

    fence->deferred_discovery_pending = false;
    if (!launch_allowed || fence->cleanup_pending ||
        fence->radio_state != BLE_INV_RADIO_CONNECTED ||
        fence->expected_conn_handle == BLE_INV_CONN_HANDLE_NONE) {
        return false;
    }
    if (conn_handle_out) {
        *conn_handle_out = fence->expected_conn_handle;
    }
    return true;
}

bool ble_investigator_runtime_fence_begin_cleanup(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation,
    bool scan_resume_required)
{
    if (!runtime_fence_matches(fence, generation)) return false;
    fence->cleanup_pending = true;
    fence->scan_resume_pending = fence->scan_resume_pending ||
                                 scan_resume_required;
    return true;
}

bool ble_investigator_runtime_fence_note_end_emitted(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation)
{
    if (!runtime_fence_matches(fence, generation) ||
        !fence->cleanup_pending) {
        return false;
    }
    fence->end_emitted = true;
    return true;
}

ble_investigator_cleanup_action_t
ble_investigator_runtime_fence_next_cleanup_action(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation,
    uint16_t *conn_handle_out)
{
    if (conn_handle_out) *conn_handle_out = BLE_INV_CONN_HANDLE_NONE;
    if (!runtime_fence_matches(fence, generation) ||
        !fence->cleanup_pending || !fence->end_emitted) {
        return BLE_INV_CLEANUP_NONE;
    }
    if (fence->operation_in_progress) return BLE_INV_CLEANUP_NONE;
    if (fence->radio_state == BLE_INV_RADIO_CONNECTING) {
        fence->radio_state = BLE_INV_RADIO_CANCEL_PENDING;
        return BLE_INV_CLEANUP_CANCEL_CONNECT;
    }
    if (fence->radio_state == BLE_INV_RADIO_CONNECTED) {
        fence->radio_state = BLE_INV_RADIO_TERMINATE_PENDING;
        if (conn_handle_out) {
            *conn_handle_out = fence->expected_conn_handle;
        }
        return BLE_INV_CLEANUP_TERMINATE_CONNECTION;
    }
    if (fence->radio_state == BLE_INV_RADIO_IDLE &&
        fence->scan_resume_pending) {
        return BLE_INV_CLEANUP_RESUME_SCAN;
    }
    return BLE_INV_CLEANUP_NONE;
}

bool ble_investigator_runtime_fence_cleanup_action_failed(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation,
    ble_investigator_cleanup_action_t action,
    bool radio_absent)
{
    if (!runtime_fence_matches(fence, generation) ||
        !fence->cleanup_pending) {
        return false;
    }
    if (action == BLE_INV_CLEANUP_CANCEL_CONNECT &&
        fence->radio_state == BLE_INV_RADIO_CANCEL_PENDING) {
        fence->radio_state = BLE_INV_RADIO_CONNECTING;
        return true;
    }
    if (action == BLE_INV_CLEANUP_TERMINATE_CONNECTION &&
        fence->radio_state == BLE_INV_RADIO_TERMINATE_PENDING) {
        if (radio_absent) {
            fence->radio_state = BLE_INV_RADIO_IDLE;
            fence->expected_conn_handle = BLE_INV_CONN_HANDLE_NONE;
        } else {
            fence->radio_state = BLE_INV_RADIO_CONNECTED;
        }
        return true;
    }
    return false;
}

bool ble_investigator_runtime_fence_reconcile_cancel(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation,
    ble_investigator_peer_lookup_result_t lookup_result,
    uint16_t conn_handle)
{
    if (!runtime_fence_matches(fence, generation) ||
        !fence->cleanup_pending ||
        fence->radio_state != BLE_INV_RADIO_CANCEL_PENDING) {
        return false;
    }

    fence->expected_conn_handle = BLE_INV_CONN_HANDLE_NONE;
    switch (lookup_result) {
    case BLE_INV_PEER_LOOKUP_CONNECTED:
        if (conn_handle == BLE_INV_CONN_HANDLE_NONE) {
            fence->radio_state = BLE_INV_RADIO_CONNECTING;
        } else {
            fence->radio_state = BLE_INV_RADIO_CONNECTED;
            fence->expected_conn_handle = conn_handle;
        }
        return true;
    case BLE_INV_PEER_LOOKUP_NOT_CONNECTED:
        fence->radio_state = BLE_INV_RADIO_IDLE;
        return true;
    case BLE_INV_PEER_LOOKUP_INDETERMINATE:
        fence->radio_state = BLE_INV_RADIO_CONNECTING;
        return true;
    default:
        return false;
    }
}

bool ble_investigator_runtime_fence_note_connect_result(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation,
    bool connected,
    uint16_t conn_handle)
{
    if (!runtime_fence_matches(fence, generation) ||
        (fence->radio_state != BLE_INV_RADIO_CONNECTING &&
         fence->radio_state != BLE_INV_RADIO_CANCEL_PENDING)) {
        return false;
    }
    if (connected) {
        if (conn_handle == BLE_INV_CONN_HANDLE_NONE) return false;
        fence->radio_state = BLE_INV_RADIO_CONNECTED;
        fence->expected_conn_handle = conn_handle;
    } else {
        fence->radio_state = BLE_INV_RADIO_IDLE;
        fence->expected_conn_handle = BLE_INV_CONN_HANDLE_NONE;
    }
    return true;
}

bool ble_investigator_runtime_fence_accepts_gatt(
    const ble_investigator_runtime_fence_t *fence,
    uint32_t generation,
    uint16_t conn_handle)
{
    return runtime_fence_matches(fence, generation) &&
           !fence->cleanup_pending &&
           !fence->operation_in_progress &&
           fence->radio_state == BLE_INV_RADIO_CONNECTED &&
           conn_handle != BLE_INV_CONN_HANDLE_NONE &&
           fence->expected_conn_handle == conn_handle;
}

bool ble_investigator_runtime_fence_note_disconnected(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation,
    uint16_t conn_handle)
{
    if (!runtime_fence_matches(fence, generation) ||
        conn_handle == BLE_INV_CONN_HANDLE_NONE ||
        fence->expected_conn_handle != conn_handle ||
        (fence->radio_state != BLE_INV_RADIO_CONNECTED &&
         fence->radio_state != BLE_INV_RADIO_TERMINATE_PENDING)) {
        return false;
    }
    fence->radio_state = BLE_INV_RADIO_IDLE;
    fence->expected_conn_handle = BLE_INV_CONN_HANDLE_NONE;
    return true;
}

bool ble_investigator_runtime_fence_note_scan_resumed(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation)
{
    if (!runtime_fence_matches(fence, generation) ||
        !fence->cleanup_pending ||
        fence->radio_state != BLE_INV_RADIO_IDLE ||
        !fence->scan_resume_pending) {
        return false;
    }
    fence->scan_resume_pending = false;
    return true;
}

bool ble_investigator_runtime_fence_can_release(
    const ble_investigator_runtime_fence_t *fence,
    uint32_t generation)
{
    return runtime_fence_matches(fence, generation) &&
           fence->cleanup_pending && fence->end_emitted &&
           !fence->operation_in_progress &&
           !fence->deferred_discovery_pending &&
           fence->radio_state == BLE_INV_RADIO_IDLE &&
           fence->expected_conn_handle == BLE_INV_CONN_HANDLE_NONE &&
           !fence->scan_resume_pending;
}

bool ble_investigator_runtime_fence_release(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation)
{
    if (!ble_investigator_runtime_fence_can_release(fence, generation)) {
        return false;
    }
    uint32_t completed_generation = fence->generation;
    ble_investigator_runtime_fence_init(fence);
    fence->generation = completed_generation;
    return true;
}

void ble_investigator_chunk_fence_init(
    ble_investigator_chunk_fence_t *fence)
{
    if (!fence) return;
    memset(fence, 0, sizeof(*fence));
}

bool ble_investigator_chunk_fence_open(
    ble_investigator_chunk_fence_t *fence,
    uint32_t generation)
{
    if (!fence || generation == 0 || fence->active ||
        fence->emission_in_progress) {
        return false;
    }
    ble_investigator_chunk_fence_init(fence);
    fence->generation = generation;
    fence->active = true;
    return true;
}

bool ble_investigator_chunk_fence_begin_emit(
    ble_investigator_chunk_fence_t *fence,
    uint32_t generation,
    ble_investigation_chunk_kind_t kind)
{
    if (!fence || !fence->active || fence->generation != generation ||
        fence->emission_in_progress) {
        return false;
    }
    if (kind == BLE_INV_CHUNK_BEGIN) {
        if (fence->begin_emitted || fence->end_started) return false;
    } else if (kind == BLE_INV_CHUNK_END) {
        if (!fence->begin_emitted || fence->end_started) return false;
        fence->end_started = true;
    } else if (kind < BLE_INV_CHUNK_PROGRESS || kind > BLE_INV_CHUNK_READ ||
               !fence->begin_emitted || fence->end_started) {
        return false;
    }
    fence->emission_in_progress = true;
    fence->in_progress_kind = kind;
    return true;
}

bool ble_investigator_chunk_fence_finish_emit(
    ble_investigator_chunk_fence_t *fence,
    uint32_t generation,
    ble_investigation_chunk_kind_t kind,
    bool success)
{
    if (!fence || !fence->active || fence->generation != generation ||
        !fence->emission_in_progress || fence->in_progress_kind != kind) {
        return false;
    }
    fence->emission_in_progress = false;
    if (!success) {
        if (kind == BLE_INV_CHUNK_END) fence->end_started = false;
        return true;
    }
    if (kind == BLE_INV_CHUNK_BEGIN) {
        fence->begin_emitted = true;
    } else if (kind == BLE_INV_CHUNK_END) {
        fence->end_emitted = true;
        fence->active = false;
    }
    return true;
}

static void increment_saturating(uint16_t *value)
{
    if (*value < UINT16_MAX) ++*value;
}

void ble_investigator_note_advertisement(ble_investigator_t *state,
                                         const uint8_t mac[6],
                                         const ble_fingerprint_t *fingerprint,
                                         int8_t rssi,
                                         uint8_t properties,
                                         int64_t now_ms)
{
    (void)mac;
    (void)rssi;
    if (!state || !fingerprint || !state->busy ||
        state->request.mode != BLE_INV_MODE_PASSIVE_CAPTURE) {
        return;
    }
    if (now_ms >= state->deadline_ms) {
        ble_investigator_tick(state, now_ms);
        return;
    }

    state->result.connectable = state->result.connectable ||
                                (properties & PASSIVE_CONNECTABLE) != 0;
    if (fingerprint->company_id == APPLE_COMPANY_ID &&
        (fingerprint->apple_type == APPLE_AIRPODS_TYPE ||
         fingerprint->apple_type == APPLE_NEARBY_ACTION ||
         fingerprint->apple_type == APPLE_NEARBY_INFO)) {
        increment_saturating(&state->passive_apple_count);
        return;
    }
    if (ble_investigator_fingerprint_is_swift_pair(fingerprint)) {
        increment_saturating(&state->passive_swift_pair_count);
        return;
    }
    uint8_t count = fingerprint->svc_uuid_count > 4
        ? 4
        : fingerprint->svc_uuid_count;
    for (uint8_t i = 0; i < count; ++i) {
        if (fingerprint->service_uuids[i] == FAST_PAIR_SERVICE_UUID) {
            increment_saturating(&state->passive_fast_pair_count);
            return;
        }
    }
}

#ifndef UNIT_TESTING

#include <ctype.h>

#include "ble_investigation_protocol.h"
#include "ble_remote_id.h"
#include "calibration_mode.h"
#include "comms/uart_tx.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/ble_uuid.h"
#include "nimble/ble.h"
#include "os/os_mbuf.h"
#include "uart_protocol.h"

#define BLE_INV_CLEANUP_RETRY_MS 100

typedef struct {
    uint16_t start_handle;
    uint16_t end_handle;
} runtime_service_t;

typedef struct {
    uint16_t value_handle;
    char uuid[BLE_INV_UUID_LEN];
} runtime_read_t;

typedef struct {
    ble_investigator_t core;
    ble_investigator_runtime_fence_t fence;
    ble_investigator_chunk_fence_t chunks;
    ble_addr_t peer_addr;
    bool peer_addr_valid;
    int64_t cleanup_retry_after_ms;
    runtime_service_t services[BLE_INV_MAX_SERVICES];
    uint8_t service_count;
    uint8_t service_index;
    runtime_read_t reads[BLE_INV_MAX_READS];
    uint8_t read_count;
    uint8_t read_index;
} ble_investigator_runtime_t;

static const char *RUNTIME_TAG = "ble_investigator";
static ble_investigator_runtime_t s_runtime;
static ble_investigation_result_t s_last_result;
static portMUX_TYPE s_runtime_lock = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_emission_mutex_storage;
static SemaphoreHandle_t s_emission_mutex;

static int runtime_gap_event(struct ble_gap_event *event, void *arg);
static int runtime_service_cb(uint16_t conn_handle,
                              const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *service,
                              void *arg);
static int runtime_characteristic_cb(uint16_t conn_handle,
                                     const struct ble_gatt_error *error,
                                     const struct ble_gatt_chr *characteristic,
                                     void *arg);
static int runtime_read_cb(uint16_t conn_handle,
                           const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attribute,
                           void *arg);

static bool request_ids_match(const char *request_id)
{
    if (!request_id) return false;
    size_t incoming_len = bounded_length(request_id, BLE_INV_REQUEST_ID_LEN);
    size_t active_len = bounded_length(
        s_runtime.core.request.request_id, BLE_INV_REQUEST_ID_LEN);
    return incoming_len > 0 && incoming_len < BLE_INV_REQUEST_ID_LEN &&
           incoming_len == active_len &&
           memcmp(request_id, s_runtime.core.request.request_id, incoming_len) == 0;
}

static bool runtime_is_current_locked(uint32_t generation)
{
    return s_runtime.fence.active && generation != 0 &&
           s_runtime.fence.generation == generation;
}

static uint32_t runtime_generation_from_arg(void *arg)
{
    return (uint32_t)(uintptr_t)arg;
}

static void *runtime_generation_arg(uint32_t generation)
{
    return (void *)(uintptr_t)generation;
}

static bool runtime_ensure_emission_mutex(void)
{
    if (!s_emission_mutex) {
        s_emission_mutex = xSemaphoreCreateMutexStatic(
            &s_emission_mutex_storage);
    }
    return s_emission_mutex != NULL;
}

static bool runtime_lock_emission(void)
{
    return runtime_ensure_emission_mutex() &&
           xSemaphoreTake(s_emission_mutex, portMAX_DELAY) == pdTRUE;
}

static void runtime_unlock_emission(void)
{
    if (s_emission_mutex) xSemaphoreGive(s_emission_mutex);
}

static bool runtime_is_active(void)
{
    bool active;
    portENTER_CRITICAL(&s_runtime_lock);
    active = s_runtime.fence.active;
    portEXIT_CRITICAL(&s_runtime_lock);
    return active;
}

static bool runtime_accepts_gatt(uint32_t generation, uint16_t conn_handle)
{
    bool accepts;
    portENTER_CRITICAL(&s_runtime_lock);
    accepts = ble_investigator_runtime_fence_accepts_gatt(
        &s_runtime.fence, generation, conn_handle);
    portEXIT_CRITICAL(&s_runtime_lock);
    return accepts;
}

static bool emit_chunk(const ble_investigation_chunk_t *chunk)
{
    char json[UART_JSON_MAX_SIZE];
    size_t len = ble_investigation_chunk_to_json(chunk, json, sizeof(json));
    if (len == 0 || len >= sizeof(json)) {
        ESP_LOGE(RUNTIME_TAG, "Dropped invalid BLE investigation chunk kind=%d",
                 chunk ? (int)chunk->kind : -1);
        return false;
    }
    uart_tx_send_raw_json(json);
    return true;
}

static bool runtime_finish_reserved_emission(
    uint32_t generation,
    ble_investigation_chunk_kind_t kind,
    bool emitted)
{
    bool finished;
    portENTER_CRITICAL(&s_runtime_lock);
    finished = ble_investigator_chunk_fence_finish_emit(
        &s_runtime.chunks, generation, kind, emitted);
    if (finished && emitted && kind == BLE_INV_CHUNK_END) {
        (void)ble_investigator_runtime_fence_note_end_emitted(
            &s_runtime.fence, generation);
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    return finished && emitted;
}

static bool runtime_emit_begin(uint32_t generation)
{
    if (!runtime_lock_emission()) return false;
    ble_investigation_chunk_t chunk = {0};
    bool reserved = false;
    portENTER_CRITICAL(&s_runtime_lock);
    if (runtime_is_current_locked(generation)) {
        chunk.kind = BLE_INV_CHUNK_BEGIN;
        chunk.mode = s_runtime.core.request.mode;
        snprintf(chunk.request_id, sizeof(chunk.request_id), "%s",
                 s_runtime.core.request.request_id);
        snprintf(chunk.target_mac, sizeof(chunk.target_mac), "%s",
                 s_runtime.core.request.target_mac);
        reserved = ble_investigator_chunk_fence_begin_emit(
            &s_runtime.chunks, generation, chunk.kind);
    }
    portEXIT_CRITICAL(&s_runtime_lock);

    bool emitted = reserved && emit_chunk(&chunk);
    if (reserved) {
        emitted = runtime_finish_reserved_emission(
            generation, chunk.kind, emitted);
    }
    runtime_unlock_emission();
    return emitted;
}

static bool runtime_emit_progress(uint32_t generation,
                                  ble_investigation_state_t state)
{
    if (!runtime_lock_emission()) return false;
    ble_investigation_chunk_t chunk = {0};
    bool reserved = false;
    portENTER_CRITICAL(&s_runtime_lock);
    if (runtime_is_current_locked(generation) &&
        !s_runtime.fence.cleanup_pending && s_runtime.core.busy &&
        s_runtime.core.state == state) {
        chunk.kind = BLE_INV_CHUNK_PROGRESS;
        chunk.state = state;
        snprintf(chunk.request_id, sizeof(chunk.request_id), "%s",
                 s_runtime.core.request.request_id);
        reserved = ble_investigator_chunk_fence_begin_emit(
            &s_runtime.chunks, generation, chunk.kind);
    }
    portEXIT_CRITICAL(&s_runtime_lock);

    bool emitted = reserved && emit_chunk(&chunk);
    if (reserved) {
        emitted = runtime_finish_reserved_emission(
            generation, chunk.kind, emitted);
    }
    runtime_unlock_emission();
    return emitted;
}

static void runtime_cleanup(void)
{
    if (!runtime_lock_emission()) return;

    ble_investigation_chunk_t chunk = {0};
    bool reserved = false;
    uint32_t generation = 0;
    portENTER_CRITICAL(&s_runtime_lock);
    if (s_runtime.fence.active && s_runtime.core.result_pending) {
        generation = s_runtime.fence.generation;
        if (scanner_quiet_mode_is_active()) {
            s_runtime.core.resume_scan_required = false;
            s_runtime.fence.scan_resume_pending = false;
        }
        (void)ble_investigator_runtime_fence_begin_cleanup(
            &s_runtime.fence, generation,
            s_runtime.core.resume_scan_required);
        if (!s_runtime.fence.end_emitted) {
            const ble_investigation_result_t *result = &s_runtime.core.result;
            chunk.kind = BLE_INV_CHUNK_END;
            chunk.state = result->state;
            chunk.authentication_required = result->authentication_required;
            chunk.truncated = result->truncated;
            snprintf(chunk.request_id, sizeof(chunk.request_id), "%s",
                     result->request_id);
            snprintf(chunk.summary, sizeof(chunk.summary), "%s",
                     result->summary);
            snprintf(chunk.error, sizeof(chunk.error), "%s", result->error);
            reserved = ble_investigator_chunk_fence_begin_emit(
                &s_runtime.chunks, generation, chunk.kind);
        }
    }
    portEXIT_CRITICAL(&s_runtime_lock);

    if (reserved) {
        bool emitted = emit_chunk(&chunk);
        (void)runtime_finish_reserved_emission(
            generation, chunk.kind, emitted);
    }

    if (generation == 0) goto cleanup_done;

    for (int step = 0; step < 4; ++step) {
        ble_investigator_cleanup_action_t action;
        uint16_t conn_handle = BLE_INV_CONN_HANDLE_NONE;
        int64_t now_ms = esp_timer_get_time() / 1000;
        portENTER_CRITICAL(&s_runtime_lock);
        if (!runtime_is_current_locked(generation) ||
            !s_runtime.fence.end_emitted ||
            now_ms < s_runtime.cleanup_retry_after_ms) {
            portEXIT_CRITICAL(&s_runtime_lock);
            goto cleanup_done;
        }
        action = ble_investigator_runtime_fence_next_cleanup_action(
            &s_runtime.fence, generation, &conn_handle);
        portEXIT_CRITICAL(&s_runtime_lock);

        if (action == BLE_INV_CLEANUP_CANCEL_CONNECT) {
            int rc = ble_gap_conn_cancel();
            if (rc == 0) goto cleanup_done;
            if (rc == BLE_HS_EALREADY) {
                ble_addr_t peer_addr = {0};
                bool peer_addr_valid = false;
                portENTER_CRITICAL(&s_runtime_lock);
                if (runtime_is_current_locked(generation)) {
                    peer_addr = s_runtime.peer_addr;
                    peer_addr_valid = s_runtime.peer_addr_valid;
                }
                portEXIT_CRITICAL(&s_runtime_lock);

                struct ble_gap_conn_desc description = {0};
                int lookup_rc = BLE_HS_EUNKNOWN;
                if (peer_addr_valid) {
                    lookup_rc = ble_gap_conn_find_by_addr(
                        &peer_addr, &description);
                }
                ble_investigator_peer_lookup_result_t lookup_result =
                    BLE_INV_PEER_LOOKUP_INDETERMINATE;
                if (lookup_rc == 0 &&
                    description.conn_handle != BLE_INV_CONN_HANDLE_NONE) {
                    lookup_result = BLE_INV_PEER_LOOKUP_CONNECTED;
                } else if (lookup_rc == BLE_HS_ENOTCONN) {
                    lookup_result = BLE_INV_PEER_LOOKUP_NOT_CONNECTED;
                }

                bool reconciled = false;
                portENTER_CRITICAL(&s_runtime_lock);
                reconciled =
                    ble_investigator_runtime_fence_reconcile_cancel(
                        &s_runtime.fence, generation, lookup_result,
                        description.conn_handle);
                if (reconciled) {
                    s_runtime.cleanup_retry_after_ms =
                        lookup_result == BLE_INV_PEER_LOOKUP_INDETERMINATE
                            ? now_ms + BLE_INV_CLEANUP_RETRY_MS
                            : 0;
                }
                portEXIT_CRITICAL(&s_runtime_lock);
                if (reconciled &&
                    lookup_result == BLE_INV_PEER_LOOKUP_INDETERMINATE) {
                    ESP_LOGW(RUNTIME_TAG,
                             "Peer lookup after connect cancel returned %d",
                             lookup_rc);
                    goto cleanup_done;
                }
                continue;
            }

            portENTER_CRITICAL(&s_runtime_lock);
            (void)ble_investigator_runtime_fence_cleanup_action_failed(
                &s_runtime.fence, generation, action, false);
            s_runtime.cleanup_retry_after_ms =
                now_ms + BLE_INV_CLEANUP_RETRY_MS;
            portEXIT_CRITICAL(&s_runtime_lock);
            ESP_LOGW(RUNTIME_TAG, "ble_gap_conn_cancel failed: %d", rc);
            goto cleanup_done;
        }

        if (action == BLE_INV_CLEANUP_TERMINATE_CONNECTION) {
            int rc = ble_gap_terminate(
                conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            if (rc == 0) goto cleanup_done;
            bool radio_absent = rc == BLE_HS_ENOTCONN;
            portENTER_CRITICAL(&s_runtime_lock);
            (void)ble_investigator_runtime_fence_cleanup_action_failed(
                &s_runtime.fence, generation, action, radio_absent);
            s_runtime.cleanup_retry_after_ms = radio_absent
                ? 0
                : now_ms + BLE_INV_CLEANUP_RETRY_MS;
            portEXIT_CRITICAL(&s_runtime_lock);
            if (!radio_absent) {
                ESP_LOGW(RUNTIME_TAG, "ble_gap_terminate failed: %d", rc);
                goto cleanup_done;
            }
            continue;
        }

        if (action == BLE_INV_CLEANUP_RESUME_SCAN) {
            if (scanner_quiet_mode_is_active()) {
                portENTER_CRITICAL(&s_runtime_lock);
                if (runtime_is_current_locked(generation)) {
                    s_runtime.core.resume_scan_required = false;
                    s_runtime.fence.scan_resume_pending = false;
                    s_runtime.cleanup_retry_after_ms = 0;
                }
                portEXIT_CRITICAL(&s_runtime_lock);
                continue;
            }
            if (!ble_remote_id_resume_after_investigation()) {
                portENTER_CRITICAL(&s_runtime_lock);
                if (runtime_is_current_locked(generation)) {
                    s_runtime.cleanup_retry_after_ms =
                        now_ms + BLE_INV_CLEANUP_RETRY_MS;
                }
                portEXIT_CRITICAL(&s_runtime_lock);
                goto cleanup_done;
            }
            portENTER_CRITICAL(&s_runtime_lock);
            if (ble_investigator_runtime_fence_note_scan_resumed(
                    &s_runtime.fence, generation)) {
                s_runtime.cleanup_retry_after_ms = 0;
            }
            portEXIT_CRITICAL(&s_runtime_lock);
            continue;
        }

        portENTER_CRITICAL(&s_runtime_lock);
        if (ble_investigator_runtime_fence_can_release(
                &s_runtime.fence, generation) &&
            ble_investigator_take_result(&s_runtime.core, &s_last_result) &&
            ble_investigator_runtime_fence_release(
                &s_runtime.fence, generation)) {
            s_runtime.peer_addr_valid = false;
            s_runtime.cleanup_retry_after_ms = 0;
            s_runtime.service_count = 0;
            s_runtime.service_index = 0;
            s_runtime.read_count = 0;
            s_runtime.read_index = 0;
        }
        portEXIT_CRITICAL(&s_runtime_lock);
        goto cleanup_done;
    }

cleanup_done:
    runtime_unlock_emission();
}

static void runtime_fail(uint32_t generation,
                         int status,
                         const char *error_name)
{
    ble_investigator_event_t event = {.status = status};
    if (error_name) {
        snprintf(event.uuid, sizeof(event.uuid), "%s", error_name);
    }
    portENTER_CRITICAL(&s_runtime_lock);
    if (runtime_is_current_locked(generation) &&
        !s_runtime.fence.cleanup_pending && s_runtime.core.busy) {
        if (s_runtime.core.state == BLE_INV_CONNECTING) {
            event.kind = BLE_INVESTIGATOR_EVENT_CONNECT_FAILED;
        } else if (s_runtime.core.state == BLE_INV_DISCOVERING ||
                   s_runtime.core.state == BLE_INV_READING) {
            event.kind = BLE_INVESTIGATOR_EVENT_PROCEDURE_FAILED;
        } else {
            portEXIT_CRITICAL(&s_runtime_lock);
            return;
        }
        ble_investigator_handle_event(
            &s_runtime.core, &event, esp_timer_get_time() / 1000);
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    runtime_cleanup();
}

static bool status_requires_authentication(uint16_t status)
{
    const uint16_t att_errors[] = {
        BLE_ATT_ERR_INSUFFICIENT_AUTHEN,
        BLE_ATT_ERR_INSUFFICIENT_AUTHOR,
        BLE_ATT_ERR_INSUFFICIENT_KEY_SZ,
        BLE_ATT_ERR_INSUFFICIENT_ENC,
    };
    for (size_t i = 0; i < sizeof(att_errors) / sizeof(att_errors[0]); ++i) {
        if (status == att_errors[i] || status == BLE_HS_ATT_ERR(att_errors[i])) {
            return true;
        }
    }
    return false;
}

static void runtime_auth_required(uint32_t generation, uint16_t status)
{
    ble_investigator_event_t event = {
        .kind = BLE_INVESTIGATOR_EVENT_AUTH_REQUIRED,
        .status = status,
    };
    portENTER_CRITICAL(&s_runtime_lock);
    if (runtime_is_current_locked(generation) &&
        !s_runtime.fence.cleanup_pending && s_runtime.core.busy) {
        ble_investigator_handle_event(
            &s_runtime.core, &event, esp_timer_get_time() / 1000);
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    runtime_cleanup();
}

static void uuid_to_text(const ble_uuid_t *uuid, char out[BLE_INV_UUID_LEN])
{
    if (!uuid || !out) return;
    switch (uuid->type) {
    case BLE_UUID_TYPE_16:
        snprintf(out, BLE_INV_UUID_LEN, "%04X", BLE_UUID16(uuid)->value);
        return;
    case BLE_UUID_TYPE_32:
        snprintf(out, BLE_INV_UUID_LEN, "%08lX",
                 (unsigned long)BLE_UUID32(uuid)->value);
        return;
    default:
        ble_uuid_to_str(uuid, out);
        for (size_t i = 0; out[i] != '\0'; ++i) {
            out[i] = (char)toupper((unsigned char)out[i]);
        }
        return;
    }
}

static bool read_is_allowed(const char *service_uuid, const char *uuid)
{
    return strcmp(service_uuid, "1800") == 0 ||
           strcmp(service_uuid, "180A") == 0 ||
           strcmp(uuid, "FFE1") == 0 ||
           strcmp(uuid, "FFF1") == 0;
}

static bool runtime_begin_gatt_procedure(
    uint32_t generation,
    uint16_t conn_handle,
    ble_investigation_state_t required_state)
{
    bool ready = false;
    bool terminal = false;
    int64_t now_ms = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_runtime_lock);
    if (ble_investigator_runtime_fence_accepts_gatt(
            &s_runtime.fence, generation, conn_handle)) {
        ready = ble_investigator_runtime_reserve_operation(
            &s_runtime.core, &s_runtime.fence, generation,
            required_state, now_ms);
        terminal = s_runtime.core.result_pending;
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    if (terminal) runtime_cleanup();
    return ready;
}

static void runtime_finish_gatt_procedure(uint32_t generation,
                                          int status,
                                          const char *error_name)
{
    bool fail = false;
    bool cleanup = false;
    portENTER_CRITICAL(&s_runtime_lock);
    if (runtime_is_current_locked(generation) &&
        ble_investigator_runtime_fence_finish_operation(
            &s_runtime.fence, generation)) {
        fail = status != 0 && !s_runtime.fence.cleanup_pending &&
               s_runtime.core.busy;
        cleanup = s_runtime.fence.cleanup_pending ||
                  s_runtime.core.result_pending;
    }
    portEXIT_CRITICAL(&s_runtime_lock);

    if (fail) {
        runtime_fail(generation, status, error_name);
    } else if (cleanup) {
        runtime_cleanup();
    }
}

static void runtime_complete(uint32_t generation, uint16_t conn_handle)
{
    ble_investigator_event_t event = {
        .kind = BLE_INVESTIGATOR_EVENT_DISCOVERY_COMPLETE,
    };
    portENTER_CRITICAL(&s_runtime_lock);
    if (ble_investigator_runtime_fence_accepts_gatt(
            &s_runtime.fence, generation, conn_handle) &&
        s_runtime.core.busy) {
        ble_investigator_handle_event(
            &s_runtime.core, &event, esp_timer_get_time() / 1000);
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    runtime_cleanup();
}

static void start_next_read(uint32_t generation, uint16_t conn_handle)
{
    bool complete = false;
    bool start_reading = false;
    portENTER_CRITICAL(&s_runtime_lock);
    if (!ble_investigator_runtime_fence_accepts_gatt(
            &s_runtime.fence, generation, conn_handle)) {
        portEXIT_CRITICAL(&s_runtime_lock);
        return;
    }
    complete = s_runtime.read_index >= s_runtime.read_count;
    if (!complete && s_runtime.read_index == 0 &&
        s_runtime.core.state == BLE_INV_DISCOVERING) {
        ble_investigator_event_t reading = {
            .kind = BLE_INVESTIGATOR_EVENT_READING_STARTED,
        };
        ble_investigator_handle_event(
            &s_runtime.core, &reading, esp_timer_get_time() / 1000);
        start_reading = s_runtime.core.busy &&
                        s_runtime.core.state == BLE_INV_READING;
    }
    portEXIT_CRITICAL(&s_runtime_lock);

    if (complete) {
        runtime_complete(generation, conn_handle);
        return;
    }
    if (start_reading &&
        !runtime_emit_progress(generation, BLE_INV_READING)) {
        runtime_cleanup();
        return;
    }
    uint16_t value_handle = 0;
    portENTER_CRITICAL(&s_runtime_lock);
    if (ble_investigator_runtime_fence_accepts_gatt(
            &s_runtime.fence, generation, conn_handle) &&
        s_runtime.read_index < s_runtime.read_count) {
        value_handle = s_runtime.reads[s_runtime.read_index].value_handle;
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    if (value_handle == 0) {
        runtime_fail(generation, BLE_HS_EINVAL, "read_start_failed");
        return;
    }
    if (!runtime_begin_gatt_procedure(
            generation, conn_handle, BLE_INV_READING)) {
        return;
    }
    int rc = ble_gattc_read(conn_handle, value_handle,
                            runtime_read_cb,
                            runtime_generation_arg(generation));
    runtime_finish_gatt_procedure(
        generation, rc, "read_start_failed");
}

static void start_next_characteristic_discovery(uint32_t generation,
                                                uint16_t conn_handle)
{
    bool reads_next = false;
    runtime_service_t service = {0};
    portENTER_CRITICAL(&s_runtime_lock);
    if (!ble_investigator_runtime_fence_accepts_gatt(
            &s_runtime.fence, generation, conn_handle)) {
        portEXIT_CRITICAL(&s_runtime_lock);
        return;
    }
    reads_next = s_runtime.service_index >= s_runtime.service_count;
    if (!reads_next) {
        service = s_runtime.services[s_runtime.service_index];
    }
    portEXIT_CRITICAL(&s_runtime_lock);

    if (reads_next) {
        start_next_read(generation, conn_handle);
        return;
    }
    if (!runtime_begin_gatt_procedure(
            generation, conn_handle, BLE_INV_DISCOVERING)) {
        return;
    }
    int rc = ble_gattc_disc_all_chrs(
        conn_handle,
        service.start_handle,
        service.end_handle,
        runtime_characteristic_cb,
        runtime_generation_arg(generation));
    runtime_finish_gatt_procedure(
        generation, rc, "characteristic_discovery_failed");
}

static int runtime_service_cb(uint16_t conn_handle,
                              const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *service,
                              void *arg)
{
    uint32_t generation = runtime_generation_from_arg(arg);
    if (!runtime_accepts_gatt(generation, conn_handle)) return 0;
    uint16_t status = error ? error->status : BLE_HS_EUNKNOWN;
    if (status_requires_authentication(status)) {
        runtime_auth_required(generation, status);
        return 0;
    }
    if (status == BLE_HS_EDONE) {
        bool ready = false;
        bool terminal = false;
        portENTER_CRITICAL(&s_runtime_lock);
        if (ble_investigator_runtime_fence_accepts_gatt(
                &s_runtime.fence, generation, conn_handle)) {
            ready = ble_investigator_prepare_procedure(
                &s_runtime.core, BLE_INV_DISCOVERING,
                esp_timer_get_time() / 1000);
            terminal = s_runtime.core.result_pending;
            if (ready) s_runtime.service_index = 0;
        }
        portEXIT_CRITICAL(&s_runtime_lock);
        if (terminal) runtime_cleanup();
        if (ready) {
            start_next_characteristic_discovery(generation, conn_handle);
        }
        return 0;
    }
    if (status != 0 || !service) {
        runtime_fail(generation, status, "service_discovery_failed");
        return 0;
    }

    ble_investigator_event_t event = {
        .kind = BLE_INVESTIGATOR_EVENT_SERVICE,
    };
    uuid_to_text(&service->uuid.u, event.uuid);
    if (!runtime_lock_emission()) {
        runtime_fail(generation, BLE_HS_ENOMEM, "chunk_emission_unavailable");
        return 0;
    }
    ble_investigation_chunk_t chunk = {0};
    uint8_t before = 0;
    bool accepted = false;
    bool reserved = false;
    portENTER_CRITICAL(&s_runtime_lock);
    if (ble_investigator_runtime_fence_accepts_gatt(
            &s_runtime.fence, generation, conn_handle)) {
        before = s_runtime.core.result.service_count;
        ble_investigator_handle_event(
            &s_runtime.core, &event, esp_timer_get_time() / 1000);
        accepted = s_runtime.core.result.service_count > before;
        if (accepted && before < BLE_INV_MAX_SERVICES) {
            s_runtime.services[before].start_handle = service->start_handle;
            s_runtime.services[before].end_handle = service->end_handle;
            s_runtime.service_count = (uint8_t)(before + 1);
            chunk.kind = BLE_INV_CHUNK_SERVICE;
            chunk.index = before;
            snprintf(chunk.request_id, sizeof(chunk.request_id), "%s",
                     s_runtime.core.request.request_id);
            snprintf(chunk.uuid, sizeof(chunk.uuid), "%s", event.uuid);
            reserved = ble_investigator_chunk_fence_begin_emit(
                &s_runtime.chunks, generation, chunk.kind);
        }
    }
    portEXIT_CRITICAL(&s_runtime_lock);

    bool emitted = reserved && emit_chunk(&chunk);
    if (reserved) {
        emitted = runtime_finish_reserved_emission(
            generation, chunk.kind, emitted);
    }
    runtime_unlock_emission();
    if (accepted && !emitted) {
        runtime_fail(generation, BLE_HS_EUNKNOWN, "chunk_order_failed");
    }
    runtime_cleanup();
    return 0;
}

static int runtime_characteristic_cb(uint16_t conn_handle,
                                     const struct ble_gatt_error *error,
                                     const struct ble_gatt_chr *characteristic,
                                     void *arg)
{
    uint32_t generation = runtime_generation_from_arg(arg);
    if (!runtime_accepts_gatt(generation, conn_handle)) return 0;
    uint16_t status = error ? error->status : BLE_HS_EUNKNOWN;
    if (status_requires_authentication(status)) {
        runtime_auth_required(generation, status);
        return 0;
    }
    if (status == BLE_HS_EDONE) {
        bool ready = false;
        bool terminal = false;
        portENTER_CRITICAL(&s_runtime_lock);
        if (ble_investigator_runtime_fence_accepts_gatt(
                &s_runtime.fence, generation, conn_handle)) {
            ready = ble_investigator_prepare_procedure(
                &s_runtime.core, BLE_INV_DISCOVERING,
                esp_timer_get_time() / 1000);
            terminal = s_runtime.core.result_pending;
            if (ready && s_runtime.service_index < s_runtime.service_count) {
                ++s_runtime.service_index;
            }
        }
        portEXIT_CRITICAL(&s_runtime_lock);
        if (terminal) runtime_cleanup();
        if (ready) {
            start_next_characteristic_discovery(generation, conn_handle);
        }
        return 0;
    }
    if (status != 0 || !characteristic) {
        runtime_fail(generation, status, "characteristic_discovery_failed");
        return 0;
    }

    ble_investigator_event_t event = {
        .kind = BLE_INVESTIGATOR_EVENT_CHARACTERISTIC,
        .properties = characteristic->properties,
    };
    uuid_to_text(&characteristic->uuid.u, event.uuid);

    if (!runtime_lock_emission()) {
        runtime_fail(generation, BLE_HS_ENOMEM, "chunk_emission_unavailable");
        return 0;
    }
    ble_investigation_chunk_t chunk = {0};
    uint8_t before = 0;
    bool accepted = false;
    bool reserved = false;
    bool invalid_service = false;
    portENTER_CRITICAL(&s_runtime_lock);
    if (ble_investigator_runtime_fence_accepts_gatt(
            &s_runtime.fence, generation, conn_handle)) {
        invalid_service = s_runtime.service_index >= s_runtime.service_count;
        if (!invalid_service) {
            snprintf(event.service_uuid, sizeof(event.service_uuid), "%s",
                     s_runtime.core.result.services[s_runtime.service_index]);
            before = s_runtime.core.result.characteristic_count;
            ble_investigator_handle_event(
                &s_runtime.core, &event, esp_timer_get_time() / 1000);
            accepted = s_runtime.core.result.characteristic_count > before;
            if (accepted &&
                (characteristic->properties & BLE_INV_PROP_READ) != 0 &&
                read_is_allowed(event.service_uuid, event.uuid)) {
                if (s_runtime.read_count < BLE_INV_MAX_READS) {
                    runtime_read_t *read =
                        &s_runtime.reads[s_runtime.read_count++];
                    read->value_handle = characteristic->val_handle;
                    snprintf(read->uuid, sizeof(read->uuid), "%s", event.uuid);
                } else {
                    s_runtime.core.result.truncated = true;
                }
            }
            if (accepted) {
                chunk.kind = BLE_INV_CHUNK_CHARACTERISTIC;
                chunk.index = before;
                chunk.properties = event.properties;
                snprintf(chunk.request_id, sizeof(chunk.request_id), "%s",
                         s_runtime.core.request.request_id);
                snprintf(chunk.service_uuid, sizeof(chunk.service_uuid), "%s",
                         event.service_uuid);
                snprintf(chunk.uuid, sizeof(chunk.uuid), "%s", event.uuid);
                reserved = ble_investigator_chunk_fence_begin_emit(
                    &s_runtime.chunks, generation, chunk.kind);
            }
        }
    }
    portEXIT_CRITICAL(&s_runtime_lock);

    bool emitted = reserved && emit_chunk(&chunk);
    if (reserved) {
        emitted = runtime_finish_reserved_emission(
            generation, chunk.kind, emitted);
    }
    runtime_unlock_emission();
    if (invalid_service) {
        runtime_fail(generation, BLE_HS_EINVAL,
                     "characteristic_discovery_failed");
    } else if (accepted && !emitted) {
        runtime_fail(generation, BLE_HS_EUNKNOWN, "chunk_order_failed");
    }
    runtime_cleanup();
    return 0;
}

static int runtime_read_cb(uint16_t conn_handle,
                           const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attribute,
                           void *arg)
{
    uint32_t generation = runtime_generation_from_arg(arg);
    if (!runtime_accepts_gatt(generation, conn_handle)) return 0;
    uint16_t status = error ? error->status : BLE_HS_EUNKNOWN;
    if (status_requires_authentication(status)) {
        runtime_auth_required(generation, status);
        return 0;
    }
    if (status != 0 || !attribute || !attribute->om) {
        runtime_fail(generation, status, "read_failed");
        return 0;
    }

    uint8_t value[(BLE_INV_READ_HEX_LEN - 1) / 2];
    uint16_t packet_len = OS_MBUF_PKTLEN(attribute->om);
    uint16_t value_len = packet_len > sizeof(value) ? sizeof(value) : packet_len;
    if (os_mbuf_copydata(attribute->om, 0, value_len, value) != 0) {
        runtime_fail(generation, BLE_HS_EINVAL, "read_copy_failed");
        return 0;
    }
    ble_investigator_event_t event = {
        .kind = BLE_INVESTIGATOR_EVENT_READ,
        .value = value,
        .value_len = value_len,
    };
    if (!runtime_lock_emission()) {
        runtime_fail(generation, BLE_HS_ENOMEM, "chunk_emission_unavailable");
        return 0;
    }
    ble_investigation_chunk_t chunk = {0};
    uint8_t before = 0;
    bool accepted = false;
    bool reserved = false;
    bool invalid_read = false;
    portENTER_CRITICAL(&s_runtime_lock);
    if (ble_investigator_runtime_fence_accepts_gatt(
            &s_runtime.fence, generation, conn_handle)) {
        invalid_read = s_runtime.read_index >= s_runtime.read_count;
        if (!invalid_read) {
            snprintf(event.uuid, sizeof(event.uuid), "%s",
                     s_runtime.reads[s_runtime.read_index].uuid);
            before = s_runtime.core.result.read_count;
            ble_investigator_handle_event(
                &s_runtime.core, &event, esp_timer_get_time() / 1000);
            accepted = s_runtime.core.result.read_count > before;
            if (accepted) {
                if (packet_len > value_len) {
                    s_runtime.core.result.truncated = true;
                }
                chunk.kind = BLE_INV_CHUNK_READ;
                chunk.index = before;
                snprintf(chunk.request_id, sizeof(chunk.request_id), "%s",
                         s_runtime.core.request.request_id);
                snprintf(chunk.uuid, sizeof(chunk.uuid), "%s",
                         s_runtime.core.result.reads[before].uuid);
                snprintf(chunk.value_hex, sizeof(chunk.value_hex), "%s",
                         s_runtime.core.result.reads[before].value_hex);
                reserved = ble_investigator_chunk_fence_begin_emit(
                    &s_runtime.chunks, generation, chunk.kind);
                ++s_runtime.read_index;
            }
        }
    }
    portEXIT_CRITICAL(&s_runtime_lock);

    bool emitted = reserved && emit_chunk(&chunk);
    if (reserved) {
        emitted = runtime_finish_reserved_emission(
            generation, chunk.kind, emitted);
    }
    runtime_unlock_emission();
    if (invalid_read) {
        runtime_fail(generation, BLE_HS_EINVAL, "read_failed");
    } else if (accepted && !emitted) {
        runtime_fail(generation, BLE_HS_EUNKNOWN, "chunk_order_failed");
    }
    runtime_cleanup();
    if (accepted) start_next_read(generation, conn_handle);
    return 0;
}

static void runtime_start_service_discovery(uint32_t generation,
                                            uint16_t conn_handle)
{
    if (!runtime_emit_progress(generation, BLE_INV_DISCOVERING)) {
        runtime_cleanup();
        return;
    }
    if (!runtime_begin_gatt_procedure(
            generation, conn_handle, BLE_INV_DISCOVERING)) {
        return;
    }
    int discovery_rc = ble_gattc_disc_all_svcs(
        conn_handle, runtime_service_cb,
        runtime_generation_arg(generation));
    runtime_finish_gatt_procedure(
        generation, discovery_rc, "service_discovery_failed");
}

static int runtime_gap_event(struct ble_gap_event *event, void *arg)
{
    uint32_t generation = runtime_generation_from_arg(arg);
    if (!event || generation == 0) return 0;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            portENTER_CRITICAL(&s_runtime_lock);
            bool accepted =
                ble_investigator_runtime_fence_note_connect_result(
                    &s_runtime.fence, generation, false,
                    BLE_INV_CONN_HANDLE_NONE);
            bool cleanup_pending = accepted &&
                                   s_runtime.fence.cleanup_pending;
            if (accepted) s_runtime.cleanup_retry_after_ms = 0;
            portEXIT_CRITICAL(&s_runtime_lock);
            if (!accepted) return 0;
            if (cleanup_pending) {
                runtime_cleanup();
            } else {
                runtime_fail(generation, event->connect.status,
                             "connect_failed");
            }
            return 0;
        }
        ble_investigator_event_t connected = {
            .kind = BLE_INVESTIGATOR_EVENT_CONNECTED,
        };
        struct ble_gap_conn_desc description;
        bool has_description =
            ble_gap_conn_find(event->connect.conn_handle, &description) == 0;
        bool accepted = false;
        bool launch_discovery = false;
        bool deferred_discovery = false;
        bool terminal = false;
        portENTER_CRITICAL(&s_runtime_lock);
        accepted = ble_investigator_runtime_fence_note_connect_result(
            &s_runtime.fence, generation, true,
            event->connect.conn_handle);
        if (accepted) {
            s_runtime.cleanup_retry_after_ms = 0;
            if (!s_runtime.fence.cleanup_pending && s_runtime.core.busy) {
                ble_investigator_handle_event(
                    &s_runtime.core, &connected,
                    esp_timer_get_time() / 1000);
                if (s_runtime.core.busy && has_description) {
                    s_runtime.core.result.bonded =
                        description.sec_state.bonded;
                    s_runtime.core.result.encrypted =
                        description.sec_state.encrypted;
                }
            }
            bool discovery_ready = !s_runtime.fence.cleanup_pending &&
                                   s_runtime.core.busy &&
                                   s_runtime.core.state == BLE_INV_DISCOVERING;
            if (discovery_ready &&
                s_runtime.fence.operation_in_progress) {
                deferred_discovery =
                    ble_investigator_runtime_fence_defer_discovery(
                        &s_runtime.fence, generation,
                        event->connect.conn_handle);
            } else {
                launch_discovery = discovery_ready;
            }
            terminal = s_runtime.fence.cleanup_pending ||
                       s_runtime.core.result_pending;
        }
        portEXIT_CRITICAL(&s_runtime_lock);
        if (!accepted) return 0;
        if (terminal || (!launch_discovery && !deferred_discovery)) {
            runtime_cleanup();
            return 0;
        }
        if (deferred_discovery) return 0;
        runtime_start_service_discovery(
            generation, event->connect.conn_handle);
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        ble_investigator_event_t disconnected = {
            .kind = BLE_INVESTIGATOR_EVENT_DISCONNECTED,
            .status = event->disconnect.reason,
        };
        bool accepted = false;
        portENTER_CRITICAL(&s_runtime_lock);
        accepted = ble_investigator_runtime_fence_note_disconnected(
            &s_runtime.fence, generation,
            event->disconnect.conn.conn_handle);
        if (accepted) {
            s_runtime.cleanup_retry_after_ms = 0;
        }
        if (accepted && !s_runtime.fence.cleanup_pending &&
            s_runtime.core.busy) {
            ble_investigator_handle_event(
                &s_runtime.core, &disconnected, esp_timer_get_time() / 1000);
        }
        portEXIT_CRITICAL(&s_runtime_lock);
        if (accepted) runtime_cleanup();
        return 0;
    }

    default:
        return 0;
    }
}

bool ble_investigator_runtime_start(
    const ble_investigation_request_t *request,
    int64_t now_ms)
{
    if (!request) return false;
    if (!ble_investigator_passive_start_is_ready(
            request, ble_remote_id_is_scanning()) ||
        !runtime_ensure_emission_mutex()) {
        return false;
    }
    uint8_t target_mac[6] = {0};
    if (request->mode == BLE_INV_MODE_GATT &&
        !ble_investigator_parse_target_mac(request->target_mac, target_mac)) {
        return false;
    }

    uint32_t generation = 0;
    ble_investigation_state_t initial_state = BLE_INV_IDLE;
    portENTER_CRITICAL(&s_runtime_lock);
    if (s_runtime.fence.active ||
        !ble_investigator_start(&s_runtime.core, request, now_ms)) {
        portEXIT_CRITICAL(&s_runtime_lock);
        return false;
    }
    generation = ble_investigator_runtime_fence_begin(&s_runtime.fence);
    if (generation == 0 ||
        !ble_investigator_chunk_fence_open(&s_runtime.chunks, generation)) {
        ble_investigator_init(&s_runtime.core);
        ble_investigator_runtime_fence_init(&s_runtime.fence);
        portEXIT_CRITICAL(&s_runtime_lock);
        return false;
    }
    s_runtime.peer_addr_valid = false;
    s_runtime.cleanup_retry_after_ms = 0;
    s_runtime.service_count = 0;
    s_runtime.service_index = 0;
    s_runtime.read_count = 0;
    s_runtime.read_index = 0;
    initial_state = s_runtime.core.state;
    portEXIT_CRITICAL(&s_runtime_lock);

    if (!runtime_emit_begin(generation)) {
        portENTER_CRITICAL(&s_runtime_lock);
        ble_investigator_init(&s_runtime.core);
        ble_investigator_runtime_fence_init(&s_runtime.fence);
        ble_investigator_chunk_fence_init(&s_runtime.chunks);
        portEXIT_CRITICAL(&s_runtime_lock);
        return false;
    }
    if (!runtime_emit_progress(generation, initial_state)) {
        runtime_fail(generation, BLE_HS_EUNKNOWN, "chunk_order_failed");
        return true;
    }
    if (request->mode == BLE_INV_MODE_PASSIVE_CAPTURE) {
        return true;
    }

    uint8_t peer_addr_type = BLE_ADDR_PUBLIC;
    (void)ble_remote_id_lookup_peer_addr_type(
        target_mac, now_ms, &peer_addr_type);

    bool pause_started = false;
    int64_t pause_reservation_ms = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_runtime_lock);
    if (runtime_is_current_locked(generation)) {
        pause_started = ble_investigator_runtime_reserve_operation(
            &s_runtime.core, &s_runtime.fence, generation,
            BLE_INV_CONNECTING, pause_reservation_ms);
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    if (!pause_started) {
        runtime_cleanup();
        return true;
    }

    bool pause_call_allowed = false;
    int64_t pause_call_ms = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_runtime_lock);
    if (runtime_is_current_locked(generation) &&
        s_runtime.fence.operation_in_progress) {
        pause_call_allowed = ble_investigator_prepare_procedure(
            &s_runtime.core, BLE_INV_CONNECTING, pause_call_ms);
        if (!pause_call_allowed) {
            (void)ble_investigator_runtime_fence_finish_operation(
                &s_runtime.fence, generation);
        }
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    if (!pause_call_allowed) {
        runtime_cleanup();
        return true;
    }

    bool scan_paused = ble_remote_id_pause_for_investigation();
    bool pause_failed = false;
    portENTER_CRITICAL(&s_runtime_lock);
    if (runtime_is_current_locked(generation)) {
        (void)ble_investigator_runtime_fence_finish_operation(
            &s_runtime.fence, generation);
        pause_failed = !scan_paused && !s_runtime.fence.cleanup_pending &&
                       s_runtime.core.busy;
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    if (!scan_paused) {
        if (pause_failed) {
            runtime_fail(generation, BLE_HS_EBUSY, "scan_pause_failed");
        } else {
            runtime_cleanup();
        }
        return true;
    }
    runtime_cleanup();

    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        runtime_fail(generation, rc, "local_address_unavailable");
        return true;
    }
    ble_addr_t peer_addr = {
        .type = peer_addr_type,
    };
    memcpy(peer_addr.val, target_mac, sizeof(peer_addr.val));

    int64_t launch_ms = esp_timer_get_time() / 1000;
    int64_t deadline_ms = 0;
    bool connect_started = false;
    portENTER_CRITICAL(&s_runtime_lock);
    if (runtime_is_current_locked(generation) &&
        ble_investigator_prepare_procedure(
            &s_runtime.core, BLE_INV_CONNECTING, launch_ms) &&
        ble_investigator_runtime_fence_begin_operation(
            &s_runtime.fence, generation)) {
        if (ble_investigator_runtime_fence_mark_connecting(
                &s_runtime.fence, generation)) {
            s_runtime.peer_addr = peer_addr;
            s_runtime.peer_addr_valid = true;
            deadline_ms = s_runtime.core.deadline_ms;
            connect_started = true;
        } else {
            (void)ble_investigator_runtime_fence_finish_operation(
                &s_runtime.fence, generation);
        }
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    if (!connect_started) {
        runtime_cleanup();
        return true;
    }

    launch_ms = esp_timer_get_time() / 1000;
    if (launch_ms >= deadline_ms) {
        portENTER_CRITICAL(&s_runtime_lock);
        if (runtime_is_current_locked(generation)) {
            (void)ble_investigator_runtime_fence_note_connect_result(
                &s_runtime.fence, generation, false,
                BLE_INV_CONN_HANDLE_NONE);
            ble_investigator_tick(&s_runtime.core, launch_ms);
            (void)ble_investigator_runtime_fence_finish_operation(
                &s_runtime.fence, generation);
        }
        portEXIT_CRITICAL(&s_runtime_lock);
        runtime_cleanup();
        return true;
    }

    int32_t connect_timeout_ms = (int32_t)(deadline_ms - launch_ms);
    rc = ble_gap_connect(own_addr_type, &peer_addr,
                         connect_timeout_ms, NULL, runtime_gap_event,
                         runtime_generation_arg(generation));
    bool connect_failed = false;
    bool cleanup = false;
    bool launch_deferred_discovery = false;
    uint16_t deferred_conn_handle = BLE_INV_CONN_HANDLE_NONE;
    int64_t connect_complete_ms = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_runtime_lock);
    if (runtime_is_current_locked(generation)) {
        if (rc != 0) {
            (void)ble_investigator_runtime_fence_note_connect_result(
                &s_runtime.fence, generation, false,
                BLE_INV_CONN_HANDLE_NONE);
        }
        (void)ble_investigator_runtime_fence_finish_operation(
            &s_runtime.fence, generation);
        bool launch_allowed = false;
        if (s_runtime.fence.deferred_discovery_pending &&
            !s_runtime.fence.cleanup_pending && s_runtime.core.busy) {
            launch_allowed = ble_investigator_prepare_procedure(
                &s_runtime.core, BLE_INV_DISCOVERING,
                connect_complete_ms);
        }
        launch_deferred_discovery =
            ble_investigator_runtime_fence_take_deferred_discovery(
                &s_runtime.fence, generation, launch_allowed,
                &deferred_conn_handle);
        connect_failed = rc != 0 && !s_runtime.fence.cleanup_pending &&
                         s_runtime.core.busy;
        cleanup = s_runtime.fence.cleanup_pending ||
                  s_runtime.core.result_pending;
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    if (connect_failed) {
        runtime_fail(generation, rc, "connect_start_failed");
    } else if (launch_deferred_discovery) {
        runtime_start_service_discovery(
            generation, deferred_conn_handle);
    } else if (cleanup) {
        runtime_cleanup();
    }
    return true;
}

ble_investigator_request_decision_t
ble_investigator_runtime_decide_request(const char *request_id)
{
    ble_investigator_request_decision_t decision;
    portENTER_CRITICAL(&s_runtime_lock);
    decision = ble_investigator_decide_request(
        s_runtime.fence.active,
        s_runtime.fence.active ? s_runtime.core.request.request_id : NULL,
        request_id);
    portEXIT_CRITICAL(&s_runtime_lock);
    return decision;
}

void ble_investigator_runtime_tick(int64_t now_ms)
{
    bool scanner_scanning = ble_remote_id_is_scanning();
    portENTER_CRITICAL(&s_runtime_lock);
    if (s_runtime.fence.active && s_runtime.core.busy) {
        if (s_runtime.core.request.mode == BLE_INV_MODE_PASSIVE_CAPTURE &&
            !scanner_scanning) {
            ble_investigator_event_t unavailable = {
                .kind = BLE_INVESTIGATOR_EVENT_SCANNER_UNAVAILABLE,
            };
            ble_investigator_handle_event(
                &s_runtime.core, &unavailable, now_ms);
        } else {
            ble_investigator_tick(&s_runtime.core, now_ms);
        }
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    runtime_cleanup();
}

bool ble_investigator_runtime_cancel(const char *request_id, int64_t now_ms)
{
    bool cancelled = false;
    portENTER_CRITICAL(&s_runtime_lock);
    if (s_runtime.fence.active && !s_runtime.fence.cleanup_pending &&
        s_runtime.core.busy && request_ids_match(request_id)) {
        ble_investigator_cancel(&s_runtime.core, now_ms);
        cancelled = s_runtime.core.result_pending;
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    if (cancelled) runtime_cleanup();
    return cancelled;
}

void ble_investigator_runtime_quiesce(int64_t now_ms)
{
    bool cleanup_required = false;
    portENTER_CRITICAL(&s_runtime_lock);
    if (s_runtime.fence.active) {
        if (!s_runtime.fence.cleanup_pending && s_runtime.core.busy) {
            ble_investigator_cancel(&s_runtime.core, now_ms);
        }
        /* Quiet mode owns the radio state. Investigation cleanup may still
         * terminate an in-flight connection, but it must never restart scan. */
        s_runtime.core.resume_scan_required = false;
        s_runtime.fence.scan_resume_pending = false;
        cleanup_required = s_runtime.core.result_pending ||
                           s_runtime.fence.cleanup_pending;
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    if (cleanup_required) runtime_cleanup();
}

bool ble_investigator_runtime_is_busy(void)
{
    return runtime_is_active();
}

bool ble_investigator_runtime_is_gatt_active(void)
{
    bool active;
    portENTER_CRITICAL(&s_runtime_lock);
    active = s_runtime.fence.active &&
             s_runtime.core.request.mode == BLE_INV_MODE_GATT;
    portEXIT_CRITICAL(&s_runtime_lock);
    return active;
}

void ble_investigator_runtime_note_advertisement(
    const uint8_t mac[6],
    const ble_fingerprint_t *fingerprint,
    int8_t rssi,
    uint8_t properties,
    int64_t now_ms)
{
    portENTER_CRITICAL(&s_runtime_lock);
    if (s_runtime.fence.active && !s_runtime.fence.cleanup_pending &&
        s_runtime.core.busy) {
        ble_investigator_note_advertisement(
            &s_runtime.core, mac, fingerprint, rssi, properties, now_ms);
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    runtime_cleanup();
}

void ble_investigator_runtime_emit_rejection(
    const char *request_id,
    ble_investigation_mode_t mode,
    const char *target_mac,
    const char *error)
{
    ble_investigation_chunk_t chunks[2];
    if (!ble_investigator_build_rejection_chunks(
            request_id, mode, target_mac, error, chunks) ||
        !runtime_lock_emission()) {
        return;
    }
    (void)emit_chunk(&chunks[0]);
    (void)emit_chunk(&chunks[1]);
    runtime_unlock_emission();
}

#endif
