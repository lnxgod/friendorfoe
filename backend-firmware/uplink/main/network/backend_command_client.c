#include "backend_command_client.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "backend_json_reader.h"
#include "backend_json_writer.h"

#define COMMAND_ENVELOPE_FIELD_COUNT 8U
#define COMMAND_ACK_FIELD_COUNT 7U
#define COMMAND_PARSE_TOKEN_CAPACITY 64U

typedef struct {
    ble_investigation_state_t result_state;
    bool terminal;
} pending_expectation_t;

static size_t bounded_length(const char *value, size_t capacity)
{
    if (value == NULL) {
        return capacity;
    }
    size_t length = 0U;
    while (length < capacity && value[length] != '\0') {
        ++length;
    }
    return length;
}

static bool lower_hex_character(char value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f');
}

static bool upper_hex_character(char value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'A' && value <= 'F');
}

static bool valid_command_id(const char *command_id)
{
    if (bounded_length(command_id, BLE_INV_REQUEST_ID_LEN) !=
        BLE_INV_REQUEST_ID_LEN - 1U) {
        return false;
    }
    for (size_t index = 0U; index < BLE_INV_REQUEST_ID_LEN - 1U; ++index) {
        if (!lower_hex_character(command_id[index])) {
            return false;
        }
    }
    return true;
}

static bool valid_device_id(const char *device_id)
{
    const size_t length = bounded_length(
        device_id, BACKEND_COMMAND_DEVICE_ID_CAPACITY);
    if (length == 0U || length >= BACKEND_COMMAND_DEVICE_ID_CAPACITY) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const char value = device_id[index];
        if (!((value >= 'a' && value <= 'z') ||
              (value >= 'A' && value <= 'Z') ||
              (value >= '0' && value <= '9') ||
              value == '_' || value == '-')) {
            return false;
        }
    }
    return true;
}

static bool valid_canonical_mac(const char *mac)
{
    if (bounded_length(mac, 18U) != 17U) {
        return false;
    }
    for (size_t index = 0U; index < 17U; ++index) {
        if ((index + 1U) % 3U == 0U) {
            if (mac[index] != ':') {
                return false;
            }
        } else if (!upper_hex_character(mac[index])) {
            return false;
        }
    }
    return true;
}

static const char *mode_name(ble_investigation_mode_t mode)
{
    switch (mode) {
    case BLE_INV_MODE_GATT:
        return "gatt";
    case BLE_INV_MODE_PASSIVE_CAPTURE:
        return "passive_capture";
    default:
        return NULL;
    }
}

static bool mode_from_name(
    const char *name,
    ble_investigation_mode_t *out)
{
    if (name == NULL || out == NULL) {
        return false;
    }
    if (strcmp(name, "gatt") == 0) {
        *out = BLE_INV_MODE_GATT;
        return true;
    }
    if (strcmp(name, "passive_capture") == 0) {
        *out = BLE_INV_MODE_PASSIVE_CAPTURE;
        return true;
    }
    return false;
}

static bool result_state_from_name(
    const char *name,
    ble_investigation_state_t *out)
{
    if (name == NULL || out == NULL) {
        return false;
    }
    static const struct {
        const char *name;
        ble_investigation_state_t state;
    } states[] = {
        {"queued", BLE_INV_QUEUED},
        {"scanning", BLE_INV_SCANNING},
        {"connecting", BLE_INV_CONNECTING},
        {"discovering", BLE_INV_DISCOVERING},
        {"reading", BLE_INV_READING},
        {"complete", BLE_INV_COMPLETE},
        {"failed", BLE_INV_FAILED},
        {"cancelled", BLE_INV_CANCELLED},
    };
    for (size_t index = 0U;
         index < sizeof(states) / sizeof(states[0]);
         ++index) {
        if (strcmp(name, states[index].name) == 0) {
            *out = states[index].state;
            return true;
        }
    }
    return false;
}

static bool known_result_state(ble_investigation_state_t state)
{
    return state >= BLE_INV_QUEUED && state <= BLE_INV_CANCELLED;
}

static bool nonterminal_result_state(ble_investigation_state_t state)
{
    return state >= BLE_INV_QUEUED && state <= BLE_INV_READING;
}

static bool terminal_result_state(ble_investigation_state_t state)
{
    return state >= BLE_INV_COMPLETE && state <= BLE_INV_CANCELLED;
}

static bool find_field(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    const char *field,
    size_t *out_index)
{
    return backend_json_object_find(
        json, tokens, token_count, 0U, field, out_index);
}

static void increment_saturated(uint32_t *value)
{
    if (value != NULL && *value != UINT32_MAX) {
        ++*value;
    }
}

static bool retryable_status(int status_code)
{
    return status_code == 408 || status_code == 429 ||
           (status_code >= 500 && status_code <= 599);
}

bool backend_command_http_state_init(
    backend_command_http_state_t *state,
    int64_t now_ms)
{
    if (state == NULL || now_ms < 0) {
        return false;
    }
    memset(state, 0, sizeof(*state));
    state->initialized = true;
    state->next_poll_ms = now_ms;
    state->last_action = BACKEND_COMMAND_HTTP_IDLE;
    return true;
}

bool backend_command_poll_due(
    const backend_command_http_state_t *state,
    int64_t now_ms)
{
    return state != NULL && state->initialized && now_ms >= 0 &&
           now_ms >= state->next_poll_ms;
}

bool backend_command_poll_started(
    backend_command_http_state_t *state,
    int64_t now_ms)
{
    if (!backend_command_poll_due(state, now_ms)) {
        return false;
    }
    state->next_poll_ms = now_ms > INT64_MAX - BACKEND_COMMAND_POLL_INTERVAL_MS
        ? INT64_MAX : now_ms + BACKEND_COMMAND_POLL_INTERVAL_MS;
    return true;
}

backend_command_http_action_t backend_command_poll_http_action(
    bool transport_complete,
    int status_code)
{
    if (!transport_complete) {
        return BACKEND_COMMAND_HTTP_RETRY;
    }
    if (status_code == 204) {
        return BACKEND_COMMAND_HTTP_IDLE;
    }
    if (status_code == 200) {
        return BACKEND_COMMAND_HTTP_BODY;
    }
    return retryable_status(status_code)
        ? BACKEND_COMMAND_HTTP_RETRY
        : BACKEND_COMMAND_HTTP_QUARANTINE;
}

backend_command_http_action_t backend_command_result_http_action(
    bool transport_complete,
    int status_code,
    bool ack_valid)
{
    if (!transport_complete || retryable_status(status_code)) {
        return BACKEND_COMMAND_HTTP_RETRY;
    }
    if (status_code == 200 && ack_valid) {
        return BACKEND_COMMAND_HTTP_ACK;
    }
    return BACKEND_COMMAND_HTTP_QUARANTINE;
}

void backend_command_http_note(
    backend_command_http_state_t *state,
    backend_command_http_action_t action,
    int status_code,
    bool result_request)
{
    if (state == NULL || !state->initialized ||
        action < BACKEND_COMMAND_HTTP_RETRY ||
        action > BACKEND_COMMAND_HTTP_QUARANTINE) {
        return;
    }
    state->last_action = action;
    state->last_status_code = status_code;
    if (action == BACKEND_COMMAND_HTTP_RETRY) {
        increment_saturated(&state->retryable_errors);
    } else if (action == BACKEND_COMMAND_HTTP_QUARANTINE) {
        increment_saturated(&state->quarantined_errors);
        if (result_request) {
            state->result_quarantined = true;
        }
    } else if (action == BACKEND_COMMAND_HTTP_ACK && result_request) {
        state->result_quarantined = false;
    }
}

static bool copy_field_string(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    const char *field,
    char *output,
    size_t capacity)
{
    size_t index = 0U;
    return find_field(json, tokens, token_count, field, &index) &&
           backend_json_copy_string(
               json, &tokens[index], output, capacity);
}

static bool envelope_valid(const backend_command_envelope_t *envelope)
{
    if (envelope == NULL ||
        (envelope->kind != BACKEND_COMMAND_KIND_INVESTIGATE &&
         envelope->kind != BACKEND_COMMAND_KIND_CANCEL) ||
        !valid_command_id(envelope->command_id) ||
        !valid_command_id(envelope->request.request_id) ||
        strcmp(envelope->command_id, envelope->request.request_id) != 0 ||
        mode_name(envelope->request.mode) == NULL ||
        envelope->request.timeout_ms == 0U ||
        envelope->request.timeout_ms > BLE_INV_DEFAULT_TIMEOUT_MS ||
        envelope->next_sequence == UINT32_MAX ||
        (envelope->has_result_state &&
         !known_result_state(envelope->result_state)) ||
        (envelope->next_sequence == 0U && envelope->has_result_state) ||
        (envelope->next_sequence > 0U &&
         (!envelope->has_result_state ||
          !nonterminal_result_state(envelope->result_state)))) {
        return false;
    }
    if (envelope->request.mode == BLE_INV_MODE_GATT) {
        return valid_canonical_mac(envelope->request.target_mac);
    }
    return bounded_length(envelope->request.target_mac,
                          sizeof(envelope->request.target_mac)) == 0U;
}

backend_command_decode_result_t backend_command_envelope_decode(
    const char *json,
    size_t length,
    backend_command_envelope_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (json == NULL || out == NULL || length == 0U) {
        return BACKEND_COMMAND_DECODE_MALFORMED;
    }
    if (length > BACKEND_COMMAND_ENVELOPE_MAX_JSON) {
        return BACKEND_COMMAND_DECODE_TOO_LARGE;
    }

    backend_json_token_t tokens[COMMAND_PARSE_TOKEN_CAPACITY];
    size_t token_count = 0U;
    const backend_json_result_t parse_result = backend_json_parse(
        json, length, tokens,
        sizeof(tokens) / sizeof(tokens[0]), &token_count);
    if (parse_result == BACKEND_JSON_DUPLICATE_KEY) {
        return BACKEND_COMMAND_DECODE_SCHEMA_MISMATCH;
    }
    if (parse_result == BACKEND_JSON_TOO_MANY_TOKENS ||
        parse_result == BACKEND_JSON_RANGE) {
        return BACKEND_COMMAND_DECODE_TOO_LARGE;
    }
    if (parse_result != BACKEND_JSON_OK) {
        return BACKEND_COMMAND_DECODE_MALFORMED;
    }
    if (token_count != 1U + COMMAND_ENVELOPE_FIELD_COUNT * 2U ||
        tokens[0].kind != BACKEND_JSON_OBJECT ||
        tokens[0].child_count != COMMAND_ENVELOPE_FIELD_COUNT * 2U) {
        return BACKEND_COMMAND_DECODE_SCHEMA_MISMATCH;
    }

    backend_command_envelope_t envelope;
    memset(&envelope, 0, sizeof(envelope));
    char type[32];
    char mode[24];
    if (!copy_field_string(
            json, tokens, token_count, "command_id",
            envelope.command_id, sizeof(envelope.command_id)) ||
        !copy_field_string(
            json, tokens, token_count, "type", type, sizeof(type)) ||
        !copy_field_string(
            json, tokens, token_count, "request_id",
            envelope.request.request_id,
            sizeof(envelope.request.request_id)) ||
        !copy_field_string(
            json, tokens, token_count, "mode", mode, sizeof(mode)) ||
        !mode_from_name(mode, &envelope.request.mode)) {
        return BACKEND_COMMAND_DECODE_SCHEMA_MISMATCH;
    }
    if (strcmp(type, "ble_investigate") == 0) {
        envelope.kind = BACKEND_COMMAND_KIND_INVESTIGATE;
    } else if (strcmp(type, "ble_investigate_cancel") == 0) {
        envelope.kind = BACKEND_COMMAND_KIND_CANCEL;
    } else {
        return BACKEND_COMMAND_DECODE_SCHEMA_MISMATCH;
    }

    size_t target_index = 0U;
    size_t timeout_index = 0U;
    size_t sequence_index = 0U;
    size_t state_index = 0U;
    uint64_t timeout_ms = 0U;
    uint64_t next_sequence = 0U;
    if (!find_field(
            json, tokens, token_count, "target", &target_index) ||
        !find_field(
            json, tokens, token_count, "timeout_ms", &timeout_index) ||
        !find_field(
            json, tokens, token_count, "next_sequence", &sequence_index) ||
        !find_field(
            json, tokens, token_count, "result_state", &state_index) ||
        !backend_json_get_u64(
            json, &tokens[timeout_index], &timeout_ms) ||
        !backend_json_get_u64(
            json, &tokens[sequence_index], &next_sequence) ||
        timeout_ms == 0U || timeout_ms > BLE_INV_DEFAULT_TIMEOUT_MS ||
        next_sequence >= UINT32_MAX) {
        return BACKEND_COMMAND_DECODE_SCHEMA_MISMATCH;
    }
    envelope.request.timeout_ms = (uint32_t)timeout_ms;
    envelope.next_sequence = (uint32_t)next_sequence;

    if (envelope.request.mode == BLE_INV_MODE_GATT) {
        if (!backend_json_copy_string(
                json, &tokens[target_index], envelope.request.target_mac,
                sizeof(envelope.request.target_mac)) ||
            !valid_canonical_mac(envelope.request.target_mac)) {
            return BACKEND_COMMAND_DECODE_SCHEMA_MISMATCH;
        }
    } else if (tokens[target_index].kind != BACKEND_JSON_NULL) {
        return BACKEND_COMMAND_DECODE_SCHEMA_MISMATCH;
    }

    if (tokens[state_index].kind == BACKEND_JSON_NULL) {
        envelope.has_result_state = false;
    } else {
        char state[16];
        if (!backend_json_copy_string(
                json, &tokens[state_index], state, sizeof(state)) ||
            !result_state_from_name(state, &envelope.result_state)) {
            return BACKEND_COMMAND_DECODE_SCHEMA_MISMATCH;
        }
        envelope.has_result_state = true;
    }

    if (!envelope_valid(&envelope)) {
        return BACKEND_COMMAND_DECODE_SCHEMA_MISMATCH;
    }
    *out = envelope;
    return BACKEND_COMMAND_DECODE_OK;
}

static bool retained_request_matches(
    const backend_ble_investigation_state_t *local_state,
    const backend_command_envelope_t *envelope)
{
    if (local_state == NULL || envelope == NULL || !local_state->active ||
        !valid_command_id(local_state->command_id) ||
        strcmp(local_state->command_id, envelope->command_id) != 0 ||
        strcmp(local_state->request.request_id,
               envelope->request.request_id) != 0 ||
        local_state->request.mode != envelope->request.mode ||
        local_state->request.timeout_ms != envelope->request.timeout_ms) {
        return false;
    }
    return strcmp(local_state->request.target_mac,
                  envelope->request.target_mac) == 0;
}

backend_command_intent_t backend_command_select_intent(
    const backend_command_envelope_t *envelope,
    const backend_ble_investigation_state_t *local_state)
{
    if (!envelope_valid(envelope)) {
        return BACKEND_COMMAND_INTENT_INVALID;
    }
    if (local_state != NULL &&
        (local_state->active || local_state->queue_count != 0U)) {
        if (!retained_request_matches(local_state, envelope)) {
            return BACKEND_COMMAND_INTENT_CONFLICT;
        }
        return envelope->kind == BACKEND_COMMAND_KIND_CANCEL
            ? BACKEND_COMMAND_INTENT_CANCEL_ACTIVE
            : BACKEND_COMMAND_INTENT_ALREADY_ACTIVE;
    }
    if (envelope->next_sequence > 0U) {
        return envelope->kind == BACKEND_COMMAND_KIND_CANCEL
            ? BACKEND_COMMAND_INTENT_RESUME_CANCELLED
            : BACKEND_COMMAND_INTENT_RESUME_FAILED;
    }
    return envelope->kind == BACKEND_COMMAND_KIND_CANCEL
        ? BACKEND_COMMAND_INTENT_CANCEL_FIRST_SEEN
        : BACKEND_COMMAND_INTENT_START;
}

size_t backend_command_scanner_line_encode(
    const backend_command_envelope_t *envelope,
    backend_command_intent_t intent,
    char *output,
    size_t capacity)
{
    if (output != NULL && capacity > 0U) {
        output[0] = '\0';
    }
    if (output == NULL || capacity == 0U || !envelope_valid(envelope)) {
        return 0U;
    }

    backend_json_writer_t writer;
    backend_json_writer_init(&writer, output, capacity);
    if (intent == BACKEND_COMMAND_INTENT_START &&
        envelope->kind == BACKEND_COMMAND_KIND_INVESTIGATE &&
        envelope->next_sequence == 0U) {
        const char *mode = mode_name(envelope->request.mode);
        if (!backend_json_append(&writer, "{\"type\":\"investigate\","
                                          "\"command_id\":") ||
            !backend_json_append_escaped(&writer, envelope->command_id) ||
            !backend_json_append(&writer, ",\"mac\":")) {
            return 0U;
        }
        if (envelope->request.mode == BLE_INV_MODE_GATT) {
            if (!backend_json_append_escaped(
                    &writer, envelope->request.target_mac)) {
                return 0U;
            }
        } else if (!backend_json_append(&writer, "null")) {
            return 0U;
        }
        if (!backend_json_append(&writer, ",\"mode\":") ||
            !backend_json_append_escaped(&writer, mode) ||
            !backend_json_append_format(
                &writer, ",\"timeout_ms\":%" PRIu32 "}",
                envelope->request.timeout_ms)) {
            return 0U;
        }
        return backend_json_writer_finish(&writer);
    }

    if (intent == BACKEND_COMMAND_INTENT_CANCEL_ACTIVE &&
        envelope->kind == BACKEND_COMMAND_KIND_CANCEL &&
        backend_json_append(&writer, "{\"type\":\"cancel\","
                                     "\"command_id\":") &&
        backend_json_append_escaped(&writer, envelope->command_id) &&
        backend_json_append(&writer, "}")) {
        return backend_json_writer_finish(&writer);
    }
    output[0] = '\0';
    return 0U;
}

static bool format_path(
    char *output,
    size_t capacity,
    const char *format,
    const char *device_id,
    const char *command_id)
{
    if (output != NULL && capacity > 0U) {
        output[0] = '\0';
    }
    if (output == NULL || capacity == 0U || format == NULL ||
        !valid_device_id(device_id) ||
        (command_id != NULL && !valid_command_id(command_id))) {
        return false;
    }
    const int written = command_id == NULL
        ? snprintf(output, capacity, format, device_id)
        : snprintf(output, capacity, format, device_id, command_id);
    if (written < 0 || (size_t)written >= capacity) {
        output[0] = '\0';
        return false;
    }
    return true;
}

bool backend_command_build_poll_path(
    const char *device_id,
    char *output,
    size_t capacity)
{
    return format_path(
        output, capacity, "/nodes/%s/commands/next", device_id, NULL);
}

bool backend_command_build_result_path(
    const char *device_id,
    const char *command_id,
    char *output,
    size_t capacity)
{
    return format_path(
        output, capacity, "/nodes/%s/commands/%s/result",
        device_id, command_id);
}

void backend_command_client_init(backend_command_client_state_t *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

bool backend_command_client_bind(
    backend_command_client_state_t *state,
    const char *device_id,
    const backend_command_envelope_t *envelope)
{
    if (state == NULL || state->bound || state->pending ||
        !envelope_valid(envelope)) {
        return false;
    }
    backend_command_client_state_t bound;
    memset(&bound, 0, sizeof(bound));
    if (!backend_command_build_result_path(
            device_id, envelope->command_id,
            bound.result_path, sizeof(bound.result_path))) {
        return false;
    }
    bound.bound = true;
    memcpy(bound.command_id, envelope->command_id,
           sizeof(bound.command_id));
    bound.next_sequence = envelope->next_sequence;
    bound.has_result_state = envelope->has_result_state;
    bound.result_state = envelope->result_state;
    *state = bound;
    return true;
}

static bool pending_body_metadata(
    const backend_command_client_state_t *state,
    const backend_command_result_t *pending,
    pending_expectation_t *expectation)
{
    if (state == NULL || pending == NULL || expectation == NULL ||
        !state->bound || !valid_command_id(state->command_id) ||
        pending->json_length == 0U ||
        pending->json_length > BACKEND_COMMAND_RESULT_MAX_JSON ||
        pending->json[pending->json_length] != '\0' ||
        memchr(pending->json, '\0', pending->json_length) != NULL ||
        bounded_length(pending->type, sizeof(pending->type)) >=
            sizeof(pending->type) ||
        bounded_length(pending->state, sizeof(pending->state)) >=
            sizeof(pending->state)) {
        return false;
    }

    backend_json_token_t tokens[COMMAND_PARSE_TOKEN_CAPACITY];
    size_t token_count = 0U;
    if (backend_json_parse(
            pending->json, pending->json_length, tokens,
            sizeof(tokens) / sizeof(tokens[0]), &token_count) !=
            BACKEND_JSON_OK ||
        token_count == 0U || tokens[0].kind != BACKEND_JSON_OBJECT) {
        return false;
    }

    size_t sequence_index = 0U;
    uint64_t sequence = 0U;
    char type[sizeof(pending->type)];
    char request_id[BLE_INV_REQUEST_ID_LEN];
    if (!find_field(
            pending->json, tokens, token_count,
            "sequence", &sequence_index) ||
        !backend_json_get_u64(
            pending->json, &tokens[sequence_index], &sequence) ||
        sequence > UINT32_MAX || sequence != pending->sequence ||
        !copy_field_string(
            pending->json, tokens, token_count, "type",
            type, sizeof(type)) ||
        strcmp(type, pending->type) != 0 ||
        !copy_field_string(
            pending->json, tokens, token_count, "request_id",
            request_id, sizeof(request_id)) ||
        strcmp(request_id, state->command_id) != 0) {
        return false;
    }

    pending_expectation_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    if (strcmp(pending->type, "ble_inv_begin") == 0) {
        if (pending->sequence != 0U || pending->state[0] != '\0') {
            return false;
        }
        parsed.result_state = BLE_INV_QUEUED;
    } else if (strcmp(pending->type, "ble_inv_progress") == 0) {
        char json_state[sizeof(pending->state)];
        if (!result_state_from_name(
                pending->state, &parsed.result_state) ||
            !nonterminal_result_state(parsed.result_state) ||
            !copy_field_string(
                pending->json, tokens, token_count, "state",
                json_state, sizeof(json_state)) ||
            strcmp(json_state, pending->state) != 0) {
            return false;
        }
    } else if (strcmp(pending->type, "ble_inv_service") == 0 ||
               strcmp(pending->type, "ble_inv_char") == 0 ||
               strcmp(pending->type, "ble_inv_read") == 0) {
        if (pending->state[0] != '\0' || !state->has_result_state ||
            !nonterminal_result_state(state->result_state)) {
            return false;
        }
        parsed.result_state = state->result_state;
    } else if (strcmp(pending->type, "ble_inv_end") == 0) {
        char json_state[sizeof(pending->state)];
        if (!result_state_from_name(
                pending->state, &parsed.result_state) ||
            !terminal_result_state(parsed.result_state) ||
            !copy_field_string(
                pending->json, tokens, token_count, "state",
                json_state, sizeof(json_state)) ||
            strcmp(json_state, pending->state) != 0) {
            return false;
        }
        parsed.terminal = true;
    } else {
        return false;
    }
    *expectation = parsed;
    return true;
}

bool backend_command_result_prepare(
    backend_command_client_state_t *state,
    const backend_command_result_t *pending_result)
{
    pending_expectation_t expectation;
    if (state == NULL || pending_result == NULL || !state->bound ||
        pending_result->sequence != state->next_sequence ||
        !pending_body_metadata(state, pending_result, &expectation)) {
        return false;
    }

    if (state->pending) {
        if (state->pending_sequence != pending_result->sequence ||
            state->post_body_length != pending_result->json_length ||
            memcmp(state->post_body, pending_result->json,
                   (size_t)pending_result->json_length + 1U) != 0 ||
            state->expected_result_state != expectation.result_state ||
            state->expected_terminal != expectation.terminal) {
            return false;
        }
        state->replay_eligible = true;
        if (state->attempt_count != UINT32_MAX) {
            ++state->attempt_count;
        }
        return true;
    }

    state->pending = true;
    state->pending_sequence = pending_result->sequence;
    state->expected_result_state = expectation.result_state;
    state->expected_terminal = expectation.terminal;
    state->replay_eligible = false;
    state->attempt_count = 1U;
    state->post_body_length = pending_result->json_length;
    memcpy(state->post_body, pending_result->json,
           (size_t)pending_result->json_length + 1U);
    return true;
}

static bool ack_correlated(
    const backend_command_client_state_t *state,
    const backend_command_result_ack_t *ack)
{
    return state != NULL && ack != NULL && state->bound && state->pending &&
           valid_command_id(ack->command_id) &&
           strcmp(ack->command_id, state->command_id) == 0 &&
           ack->accepted_sequence == state->pending_sequence &&
           ack->accepted_sequence == state->next_sequence &&
           ack->accepted_sequence != UINT32_MAX &&
           ack->next_sequence == ack->accepted_sequence + 1U &&
           ack->result_state == state->expected_result_state &&
           known_result_state(ack->result_state) &&
           ack->terminal == state->expected_terminal &&
           ack->terminal == terminal_result_state(ack->result_state) &&
           (!ack->duplicate || state->replay_eligible);
}

bool backend_command_result_ack_validate(
    const backend_command_client_state_t *state,
    const char *json,
    size_t length,
    backend_command_result_ack_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (state == NULL || json == NULL || out == NULL ||
        !state->bound || !state->pending || length == 0U ||
        length > BACKEND_COMMAND_ACK_MAX_JSON) {
        return false;
    }

    backend_json_token_t tokens[COMMAND_PARSE_TOKEN_CAPACITY];
    size_t token_count = 0U;
    if (backend_json_parse(
            json, length, tokens,
            sizeof(tokens) / sizeof(tokens[0]), &token_count) !=
            BACKEND_JSON_OK ||
        token_count != 1U + COMMAND_ACK_FIELD_COUNT * 2U ||
        tokens[0].kind != BACKEND_JSON_OBJECT ||
        tokens[0].child_count != COMMAND_ACK_FIELD_COUNT * 2U) {
        return false;
    }

    backend_command_result_ack_t ack;
    memset(&ack, 0, sizeof(ack));
    size_t ok_index = 0U;
    size_t accepted_index = 0U;
    size_t next_index = 0U;
    size_t terminal_index = 0U;
    size_t duplicate_index = 0U;
    uint64_t accepted = 0U;
    uint64_t next = 0U;
    bool ok = false;
    char result_state[16];
    if (!find_field(json, tokens, token_count, "ok", &ok_index) ||
        !find_field(
            json, tokens, token_count,
            "accepted_sequence", &accepted_index) ||
        !find_field(
            json, tokens, token_count,
            "next_sequence", &next_index) ||
        !find_field(
            json, tokens, token_count, "terminal", &terminal_index) ||
        !find_field(
            json, tokens, token_count, "duplicate", &duplicate_index) ||
        !backend_json_get_bool(json, &tokens[ok_index], &ok) || !ok ||
        !backend_json_get_u64(
            json, &tokens[accepted_index], &accepted) ||
        !backend_json_get_u64(json, &tokens[next_index], &next) ||
        accepted > UINT32_MAX || next > UINT32_MAX ||
        !backend_json_get_bool(
            json, &tokens[terminal_index], &ack.terminal) ||
        !backend_json_get_bool(
            json, &tokens[duplicate_index], &ack.duplicate) ||
        !copy_field_string(
            json, tokens, token_count, "command_id",
            ack.command_id, sizeof(ack.command_id)) ||
        !copy_field_string(
            json, tokens, token_count, "result_state",
            result_state, sizeof(result_state)) ||
        !result_state_from_name(result_state, &ack.result_state)) {
        return false;
    }
    ack.accepted_sequence = (uint32_t)accepted;
    ack.next_sequence = (uint32_t)next;
    if (!ack_correlated(state, &ack)) {
        return false;
    }
    *out = ack;
    return true;
}

bool backend_command_result_ack_commit(
    backend_command_client_state_t *state,
    const backend_command_result_ack_t *ack)
{
    if (!ack_correlated(state, ack)) {
        return false;
    }
    if (ack->terminal) {
        backend_command_client_init(state);
        return true;
    }

    state->next_sequence = ack->next_sequence;
    state->has_result_state = true;
    state->result_state = ack->result_state;
    state->pending = false;
    state->pending_sequence = 0U;
    state->expected_result_state = BLE_INV_IDLE;
    state->expected_terminal = false;
    state->replay_eligible = false;
    state->attempt_count = 0U;
    state->post_body_length = 0U;
    state->post_body[0] = '\0';
    return true;
}
