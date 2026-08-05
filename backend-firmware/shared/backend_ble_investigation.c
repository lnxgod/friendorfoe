#include "backend_ble_investigation.h"

#include <ctype.h>
#include <inttypes.h>
#include <stddef.h>
#include <string.h>

#include "backend_json_writer.h"

#define BACKEND_BLE_KNOWN_PROPERTY_MASK ((uint16_t)( \
    BLE_INV_PROP_BROADCAST | BLE_INV_PROP_READ | \
    BLE_INV_PROP_WRITE_WITHOUT_RESPONSE | BLE_INV_PROP_WRITE | \
    BLE_INV_PROP_NOTIFY | BLE_INV_PROP_INDICATE | \
    BLE_INV_PROP_AUTHENTICATED_SIGNED_WRITES | \
    BLE_INV_PROP_EXTENDED_PROPERTIES))

typedef struct {
    uint16_t mask;
    const char *name;
} backend_ble_property_name_t;

static const backend_ble_property_name_t PROPERTY_NAMES[] = {
    {BLE_INV_PROP_BROADCAST, "broadcast"},
    {BLE_INV_PROP_READ, "read"},
    {BLE_INV_PROP_WRITE_WITHOUT_RESPONSE, "write_without_response"},
    {BLE_INV_PROP_WRITE, "write"},
    {BLE_INV_PROP_NOTIFY, "notify"},
    {BLE_INV_PROP_INDICATE, "indicate"},
    {BLE_INV_PROP_AUTHENTICATED_SIGNED_WRITES,
     "authenticated_signed_writes"},
    {BLE_INV_PROP_EXTENDED_PROPERTIES, "extended_properties"},
};

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

static bool exact_string(const char *left, size_t left_capacity,
                         const char *right, size_t right_capacity)
{
    const size_t left_length = bounded_length(left, left_capacity);
    const size_t right_length = bounded_length(right, right_capacity);
    return left_length < left_capacity && right_length < right_capacity &&
           left_length == right_length &&
           memcmp(left, right, left_length) == 0;
}

static bool valid_command_id(const char *command_id)
{
    const size_t length = bounded_length(command_id, BLE_INV_REQUEST_ID_LEN);
    if (length != BLE_INV_REQUEST_ID_LEN - 1U) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const char value = command_id[index];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool hex_character(char value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

static bool normalize_mac(const char *input, char output[18])
{
    if (bounded_length(input, 18U) != 17U) {
        return false;
    }
    for (size_t index = 0U; index < 17U; ++index) {
        if ((index + 1U) % 3U == 0U) {
            if (input[index] != ':') {
                return false;
            }
            output[index] = ':';
        } else {
            if (!hex_character(input[index])) {
                return false;
            }
            output[index] = (char)toupper((unsigned char)input[index]);
        }
    }
    output[17] = '\0';
    return true;
}

static bool valid_uuid(const char *uuid, size_t capacity)
{
    const size_t length = bounded_length(uuid, capacity);
    if (length != 4U && length != 8U && length != 36U) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        if (length == 36U &&
            (index == 8U || index == 13U || index == 18U || index == 23U)) {
            if (uuid[index] != '-') {
                return false;
            }
        } else if (!hex_character(uuid[index])) {
            return false;
        }
    }
    return true;
}

static bool valid_value_hex(const char *value, size_t capacity)
{
    const size_t length = bounded_length(value, capacity);
    if (length >= capacity || length > 128U || (length % 2U) != 0U) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        if (!hex_character(value[index])) {
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

static const char *progress_name(ble_investigation_state_t state)
{
    switch (state) {
    case BLE_INV_QUEUED:
        return "queued";
    case BLE_INV_SCANNING:
        return "scanning";
    case BLE_INV_CONNECTING:
        return "connecting";
    case BLE_INV_DISCOVERING:
        return "discovering";
    case BLE_INV_READING:
        return "reading";
    default:
        return NULL;
    }
}

static const char *terminal_name(ble_investigation_state_t state)
{
    switch (state) {
    case BLE_INV_COMPLETE:
        return "complete";
    case BLE_INV_FAILED:
        return "failed";
    case BLE_INV_CANCELLED:
        return "cancelled";
    default:
        return NULL;
    }
}

static bool normalize_request(
    const char *command_id,
    const ble_investigation_request_t *request,
    ble_investigation_request_t *output)
{
    if (!valid_command_id(command_id) || request == NULL || output == NULL ||
        !exact_string(request->request_id, sizeof(request->request_id),
                      command_id, BLE_INV_REQUEST_ID_LEN) ||
        mode_name(request->mode) == NULL || request->timeout_ms == 0U ||
        request->timeout_ms > BLE_INV_DEFAULT_TIMEOUT_MS) {
        return false;
    }

    memset(output, 0, sizeof(*output));
    memcpy(output->request_id, command_id, BLE_INV_REQUEST_ID_LEN);
    output->mode = request->mode;
    output->timeout_ms = request->timeout_ms;
    if (request->mode == BLE_INV_MODE_GATT) {
        return normalize_mac(request->target_mac, output->target_mac);
    }
    return bounded_length(request->target_mac, sizeof(request->target_mac)) == 0U;
}

static void prepare_command(
    backend_ble_investigation_state_t *state,
    const char *command_id,
    const ble_investigation_request_t *request,
    int64_t now_ms)
{
    const uint32_t radio_start_count = state->radio_start_count;
    memset(state, 0, sizeof(*state));
    state->radio_start_count = radio_start_count;
    memcpy(state->command_id, command_id, BLE_INV_REQUEST_ID_LEN);
    state->request = *request;
    state->active = true;
    state->started_ms = now_ms;
}

static void clear_after_terminal_ack(
    backend_ble_investigation_state_t *state)
{
    state->command_id[0] = '\0';
    memset(&state->request, 0, sizeof(state->request));
    state->active = false;
    state->radio_active = false;
    state->scanner_assigned = false;
    state->began = false;
    state->terminal_queued = false;
    state->cancel_requested = false;
    state->progress_seen = false;
    state->service_count = 0U;
    state->characteristic_count = 0U;
    state->read_count = 0U;
    state->queue_head = 0U;
    state->queue_count = 0U;
}

static bool begin_result(
    const backend_ble_investigation_state_t *state,
    backend_command_result_t *result,
    backend_json_writer_t *writer,
    const char *type,
    const char *result_state)
{
    if (state == NULL || result == NULL || writer == NULL || type == NULL ||
        state->next_sequence == UINT32_MAX) {
        return false;
    }
    memset(result, 0, sizeof(*result));
    result->sequence = state->next_sequence;
    if (bounded_length(type, sizeof(result->type)) >= sizeof(result->type)) {
        return false;
    }
    memcpy(result->type, type, strlen(type) + 1U);
    if (result_state != NULL) {
        if (bounded_length(result_state, sizeof(result->state)) >=
            sizeof(result->state)) {
            return false;
        }
        memcpy(result->state, result_state, strlen(result_state) + 1U);
    }

    backend_json_writer_init(
        writer, result->json, sizeof(result->json));
    return backend_json_append_format(
               writer, "{\"sequence\":%" PRIu32 ",\"type\":",
               result->sequence) &&
           backend_json_append_escaped(writer, type) &&
           backend_json_append(writer, ",\"request_id\":") &&
           backend_json_append_escaped(writer, state->command_id);
}

static bool finish_result(
    backend_command_result_t *result,
    backend_json_writer_t *writer)
{
    if (!backend_json_append(writer, "}")) {
        return false;
    }
    const size_t length = backend_json_writer_finish(writer);
    if (length == 0U || length > BACKEND_COMMAND_RESULT_MAX_JSON) {
        return false;
    }
    result->json_length = (uint16_t)length;
    return true;
}

static bool encode_begin(
    const backend_ble_investigation_state_t *state,
    backend_command_result_t *result)
{
    backend_json_writer_t writer;
    const char *mode = mode_name(state->request.mode);
    if (mode == NULL ||
        !begin_result(state, result, &writer, "ble_inv_begin", NULL) ||
        !backend_json_append(&writer, ",\"mode\":") ||
        !backend_json_append_escaped(&writer, mode) ||
        !backend_json_append(&writer, ",\"target_mac\":")) {
        return false;
    }
    if (state->request.mode == BLE_INV_MODE_GATT) {
        if (!backend_json_append_escaped(&writer, state->request.target_mac)) {
            return false;
        }
    } else if (!backend_json_append(&writer, "null")) {
        return false;
    }
    return finish_result(result, &writer);
}

static bool encode_progress(
    const backend_ble_investigation_state_t *state,
    ble_investigation_state_t progress,
    backend_command_result_t *result)
{
    backend_json_writer_t writer;
    const char *name = progress_name(progress);
    return name != NULL &&
           begin_result(state, result, &writer, "ble_inv_progress", name) &&
           backend_json_append(&writer, ",\"state\":") &&
           backend_json_append_escaped(&writer, name) &&
           finish_result(result, &writer);
}

static bool encode_service(
    const backend_ble_investigation_state_t *state,
    const ble_investigation_chunk_t *chunk,
    backend_command_result_t *result)
{
    backend_json_writer_t writer;
    return begin_result(state, result, &writer, "ble_inv_service", NULL) &&
           backend_json_append_format(
               &writer, ",\"index\":%d,\"uuid\":", chunk->index) &&
           backend_json_append_escaped(&writer, chunk->uuid) &&
           finish_result(result, &writer);
}

static bool append_properties(backend_json_writer_t *writer,
                              uint16_t properties)
{
    if ((properties & (uint16_t)~BACKEND_BLE_KNOWN_PROPERTY_MASK) != 0U ||
        !backend_json_append(writer, "[")) {
        return false;
    }
    bool first = true;
    for (size_t index = 0U;
         index < sizeof(PROPERTY_NAMES) / sizeof(PROPERTY_NAMES[0]);
         ++index) {
        if ((properties & PROPERTY_NAMES[index].mask) == 0U) {
            continue;
        }
        if ((!first && !backend_json_append(writer, ",")) ||
            !backend_json_append_escaped(writer, PROPERTY_NAMES[index].name)) {
            return false;
        }
        first = false;
    }
    return backend_json_append(writer, "]");
}

static bool encode_characteristic(
    const backend_ble_investigation_state_t *state,
    const ble_investigation_chunk_t *chunk,
    backend_command_result_t *result)
{
    backend_json_writer_t writer;
    return begin_result(state, result, &writer, "ble_inv_char", NULL) &&
           backend_json_append_format(
               &writer, ",\"index\":%d,\"service_uuid\":", chunk->index) &&
           backend_json_append_escaped(&writer, chunk->service_uuid) &&
           backend_json_append(&writer, ",\"uuid\":") &&
           backend_json_append_escaped(&writer, chunk->uuid) &&
           backend_json_append(&writer, ",\"properties\":") &&
           append_properties(&writer, chunk->properties) &&
           finish_result(result, &writer);
}

static bool encode_read(
    const backend_ble_investigation_state_t *state,
    const ble_investigation_chunk_t *chunk,
    backend_command_result_t *result)
{
    backend_json_writer_t writer;
    return begin_result(state, result, &writer, "ble_inv_read", NULL) &&
           backend_json_append_format(
               &writer, ",\"index\":%d,\"uuid\":", chunk->index) &&
           backend_json_append_escaped(&writer, chunk->uuid) &&
           backend_json_append(&writer, ",\"value_hex\":") &&
           backend_json_append_escaped(&writer, chunk->value_hex) &&
           finish_result(result, &writer);
}

static bool encode_end(
    const backend_ble_investigation_state_t *state,
    ble_investigation_state_t terminal_state,
    const char *summary,
    const char *error,
    bool authentication_required,
    bool truncated,
    backend_command_result_t *result)
{
    backend_json_writer_t writer;
    const char *name = terminal_name(terminal_state);
    if (name == NULL ||
        !begin_result(state, result, &writer, "ble_inv_end", name) ||
        !backend_json_append(&writer, ",\"state\":") ||
        !backend_json_append_escaped(&writer, name) ||
        !backend_json_append(&writer, ",\"summary\":") ||
        !backend_json_append_escaped(&writer, summary) ||
        !backend_json_append(&writer, ",\"error\":")) {
        return false;
    }
    if (error != NULL && error[0] != '\0') {
        if (!backend_json_append_escaped(&writer, error)) {
            return false;
        }
    } else if (!backend_json_append(&writer, "null")) {
        return false;
    }
    return backend_json_append_format(
               &writer, ",\"authentication_required\":%s,\"truncated\":%s",
               authentication_required ? "true" : "false",
               truncated ? "true" : "false") &&
           finish_result(result, &writer);
}

static bool push_result(backend_ble_investigation_state_t *state,
                        const backend_command_result_t *result)
{
    if (state == NULL || result == NULL ||
        state->queue_count >= BACKEND_COMMAND_RESULT_QUEUE_CAPACITY ||
        result->sequence != state->next_sequence ||
        state->next_sequence == UINT32_MAX) {
        return false;
    }
    const uint8_t tail = (uint8_t)(
        (state->queue_head + state->queue_count) %
        BACKEND_COMMAND_RESULT_QUEUE_CAPACITY);
    state->queue[tail] = *result;
    ++state->queue_count;
    ++state->next_sequence;
    return true;
}

static bool push_begin(backend_ble_investigation_state_t *state)
{
    backend_command_result_t result;
    return state->queue_count < BACKEND_COMMAND_RESULT_QUEUE_CAPACITY - 1U &&
           encode_begin(state, &result) && push_result(state, &result);
}

static bool push_progress(backend_ble_investigation_state_t *state,
                          ble_investigation_state_t progress)
{
    backend_command_result_t result;
    return state->queue_count < BACKEND_COMMAND_RESULT_QUEUE_CAPACITY - 1U &&
           encode_progress(state, progress, &result) &&
           push_result(state, &result);
}

static bool push_service(backend_ble_investigation_state_t *state,
                         const ble_investigation_chunk_t *chunk)
{
    backend_command_result_t result;
    return state->queue_count < BACKEND_COMMAND_RESULT_QUEUE_CAPACITY - 1U &&
           encode_service(state, chunk, &result) && push_result(state, &result);
}

static bool push_characteristic(backend_ble_investigation_state_t *state,
                                const ble_investigation_chunk_t *chunk)
{
    backend_command_result_t result;
    return state->queue_count < BACKEND_COMMAND_RESULT_QUEUE_CAPACITY - 1U &&
           encode_characteristic(state, chunk, &result) &&
           push_result(state, &result);
}

static bool push_read(backend_ble_investigation_state_t *state,
                      const ble_investigation_chunk_t *chunk)
{
    backend_command_result_t result;
    return state->queue_count < BACKEND_COMMAND_RESULT_QUEUE_CAPACITY - 1U &&
           encode_read(state, chunk, &result) && push_result(state, &result);
}

static bool push_terminal(
    backend_ble_investigation_state_t *state,
    ble_investigation_state_t terminal_state,
    const char *summary,
    const char *error,
    bool authentication_required,
    bool truncated)
{
    backend_command_result_t result;
    if (!encode_end(state, terminal_state, summary, error,
                    authentication_required, truncated, &result) ||
        !push_result(state, &result)) {
        return false;
    }
    state->radio_active = false;
    state->cancel_requested = false;
    state->terminal_queued = true;
    return true;
}

static bool close_failed(backend_ble_investigation_state_t *state,
                         const char *error)
{
    if (state == NULL || !state->active || state->terminal_queued) {
        return false;
    }
    if (!state->began) {
        if (!push_begin(state)) {
            return false;
        }
        state->began = true;
    }
    if (!push_terminal(state, BLE_INV_FAILED, "", error, false, false)) {
        return false;
    }
    return true;
}

static bool chunk_source_matches(
    const backend_ble_investigation_state_t *state,
    backend_scanner_slot_t scanner_slot,
    const ble_investigation_chunk_t *chunk)
{
    return state != NULL && chunk != NULL && state->active &&
           state->radio_active && state->scanner_assigned &&
           scanner_slot == state->scanner_slot &&
           exact_string(chunk->request_id, sizeof(chunk->request_id),
                        state->command_id, sizeof(state->command_id));
}

static bool begin_chunk_matches_request(
    const backend_ble_investigation_state_t *state,
    const ble_investigation_chunk_t *chunk)
{
    if (chunk->mode != state->request.mode) {
        return false;
    }
    if (chunk->mode == BLE_INV_MODE_PASSIVE_CAPTURE) {
        return bounded_length(chunk->target_mac,
                              sizeof(chunk->target_mac)) == 0U;
    }
    char normalized[18];
    return normalize_mac(chunk->target_mac, normalized) &&
           strcmp(normalized, state->request.target_mac) == 0;
}

void backend_ble_investigation_init(
    backend_ble_investigation_state_t *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

bool backend_ble_investigation_start(
    backend_ble_investigation_state_t *state,
    const char *command_id,
    const ble_investigation_request_t *request,
    backend_scanner_slot_t scanner_slot,
    int64_t now_ms)
{
    if (state == NULL || now_ms < 0) {
        return false;
    }

    ble_investigation_request_t normalized;
    if (!normalize_request(command_id, request, &normalized) ||
        (scanner_slot != BACKEND_SCANNER_SLOT_BLE &&
         scanner_slot != BACKEND_SCANNER_SLOT_WIFI)) {
        return false;
    }
    if (state->active || state->queue_count != 0U) {
        /* Duplicate delivery and conflict both leave the occupied slot
         * untouched. The retained command/request let the caller distinguish
         * them without restarting radio work. */
        return false;
    }

    prepare_command(state, command_id, &normalized, now_ms);
    state->scanner_slot = scanner_slot;
    state->scanner_assigned = true;
    state->radio_active = true;
    ++state->radio_start_count;
    return true;
}

bool backend_ble_investigation_accept_chunk(
    backend_ble_investigation_state_t *state,
    backend_scanner_slot_t scanner_slot,
    const ble_investigation_chunk_t *chunk)
{
    if (!chunk_source_matches(state, scanner_slot, chunk)) {
        return false;
    }

    switch (chunk->kind) {
    case BLE_INV_CHUNK_BEGIN:
        if (state->began || !begin_chunk_matches_request(state, chunk) ||
            !push_begin(state)) {
            (void)close_failed(state, "result_overflow");
            return false;
        }
        state->began = true;
        return true;

    case BLE_INV_CHUNK_PROGRESS:
        if (!state->began || progress_name(chunk->state) == NULL ||
            (state->progress_seen &&
             chunk->state <= state->last_progress_state) ||
            !push_progress(state, chunk->state)) {
            (void)close_failed(state, "result_overflow");
            return false;
        }
        state->progress_seen = true;
        state->last_progress_state = chunk->state;
        return true;

    case BLE_INV_CHUNK_SERVICE:
        if (!state->began || chunk->index < 0 ||
            chunk->index != state->service_count ||
            state->service_count >= BLE_INV_MAX_SERVICES ||
            !valid_uuid(chunk->uuid, sizeof(chunk->uuid)) ||
            !push_service(state, chunk)) {
            (void)close_failed(state, "result_overflow");
            return false;
        }
        ++state->service_count;
        return true;

    case BLE_INV_CHUNK_CHARACTERISTIC:
        if (!state->began || chunk->index < 0 ||
            chunk->index != state->characteristic_count ||
            state->characteristic_count >= BLE_INV_MAX_CHARS ||
            !valid_uuid(chunk->service_uuid, sizeof(chunk->service_uuid)) ||
            !valid_uuid(chunk->uuid, sizeof(chunk->uuid)) ||
            (chunk->properties &
             (uint16_t)~BACKEND_BLE_KNOWN_PROPERTY_MASK) != 0U ||
            !push_characteristic(state, chunk)) {
            (void)close_failed(state, "result_overflow");
            return false;
        }
        ++state->characteristic_count;
        return true;

    case BLE_INV_CHUNK_READ:
        if (!state->began || chunk->index < 0 ||
            chunk->index != state->read_count ||
            state->read_count >= BLE_INV_MAX_READS ||
            !valid_uuid(chunk->uuid, sizeof(chunk->uuid)) ||
            !valid_value_hex(chunk->value_hex, sizeof(chunk->value_hex)) ||
            !push_read(state, chunk)) {
            (void)close_failed(state, "result_overflow");
            return false;
        }
        ++state->read_count;
        return true;

    case BLE_INV_CHUNK_END: {
        const char *name = terminal_name(chunk->state);
        if (name == NULL ||
            bounded_length(chunk->summary, sizeof(chunk->summary)) >=
                sizeof(chunk->summary) ||
            bounded_length(chunk->error, sizeof(chunk->error)) >=
                sizeof(chunk->error)) {
            (void)close_failed(state, "result_overflow");
            return false;
        }
        if (!state->began) {
            if (!push_begin(state)) {
                (void)close_failed(state, "result_overflow");
                return false;
            }
            state->began = true;
        }
        if (!push_terminal(state, chunk->state, chunk->summary, chunk->error,
                           chunk->authentication_required,
                           chunk->truncated)) {
            (void)close_failed(state, "result_overflow");
            return false;
        }
        return true;
    }

    default:
        (void)close_failed(state, "result_overflow");
        return false;
    }
}

bool backend_ble_investigation_next_result(
    const backend_ble_investigation_state_t *state,
    backend_command_result_t *out)
{
    if (state == NULL || out == NULL || state->queue_count == 0U ||
        state->queue_head >= BACKEND_COMMAND_RESULT_QUEUE_CAPACITY) {
        return false;
    }
    *out = state->queue[state->queue_head];
    return true;
}

bool backend_ble_investigation_mark_acked(
    backend_ble_investigation_state_t *state,
    const char *command_id,
    uint32_t result_sequence)
{
    if (state == NULL || state->queue_count == 0U ||
        !exact_string(state->command_id, sizeof(state->command_id),
                      command_id, BLE_INV_REQUEST_ID_LEN)) {
        return false;
    }
    const backend_command_result_t *head = &state->queue[state->queue_head];
    if (head->sequence != result_sequence) {
        return false;
    }
    const bool terminal = strcmp(head->type, "ble_inv_end") == 0;
    state->queue_head = (uint8_t)(
        (state->queue_head + 1U) % BACKEND_COMMAND_RESULT_QUEUE_CAPACITY);
    --state->queue_count;
    if (terminal) {
        clear_after_terminal_ack(state);
    }
    return true;
}

bool backend_ble_investigation_check_timeout(
    backend_ble_investigation_state_t *state,
    int64_t now_ms)
{
    if (state == NULL || !state->active || !state->radio_active ||
        state->terminal_queued || now_ms < state->started_ms) {
        return false;
    }
    const uint64_t elapsed =
        (uint64_t)now_ms - (uint64_t)state->started_ms;
    if (elapsed < state->request.timeout_ms) {
        return false;
    }
    return close_failed(state, "timeout");
}

bool backend_ble_investigation_cancel_first_seen(
    backend_ble_investigation_state_t *state,
    const char *command_id,
    const ble_investigation_request_t *original_request,
    int64_t now_ms)
{
    if (state == NULL || now_ms < 0 || state->active ||
        state->queue_count != 0U) {
        return false;
    }
    ble_investigation_request_t normalized;
    if (!normalize_request(command_id, original_request, &normalized)) {
        return false;
    }

    prepare_command(state, command_id, &normalized, now_ms);
    if (!push_begin(state)) {
        clear_after_terminal_ack(state);
        return false;
    }
    state->began = true;
    if (!push_terminal(state, BLE_INV_CANCELLED, "cancelled", NULL,
                       false, false)) {
        clear_after_terminal_ack(state);
        return false;
    }
    return true;
}

bool backend_ble_investigation_request_cancel(
    backend_ble_investigation_state_t *state,
    const char *command_id)
{
    if (state == NULL || !state->active ||
        !exact_string(state->command_id, sizeof(state->command_id),
                      command_id, BLE_INV_REQUEST_ID_LEN)) {
        return false;
    }
    if (state->terminal_queued) {
        return true;
    }
    if (!state->radio_active || !state->scanner_assigned) {
        return false;
    }
    state->cancel_requested = true;
    return true;
}

bool backend_ble_investigation_cancel_pending(
    const backend_ble_investigation_state_t *state,
    backend_scanner_slot_t *scanner_slot)
{
    if (state == NULL || scanner_slot == NULL || !state->active ||
        !state->radio_active || !state->scanner_assigned ||
        !state->cancel_requested || state->terminal_queued) {
        return false;
    }
    *scanner_slot = state->scanner_slot;
    return true;
}

bool backend_ble_investigation_resume_after_reboot(
    backend_ble_investigation_state_t *state,
    const char *command_id,
    const ble_investigation_request_t *original_request,
    uint32_t next_sequence,
    bool cancel_pending)
{
    if (state == NULL || state->active || state->queue_count != 0U ||
        next_sequence == 0U || next_sequence == UINT32_MAX) {
        return false;
    }
    ble_investigation_request_t normalized;
    if (!normalize_request(command_id, original_request, &normalized)) {
        return false;
    }

    prepare_command(state, command_id, &normalized, 0);
    state->began = true; /* Sequence zero is already durable on the backend. */
    state->next_sequence = next_sequence;
    if (!push_terminal(
            state,
            cancel_pending ? BLE_INV_CANCELLED : BLE_INV_FAILED,
            cancel_pending ? "cancelled" : "",
            cancel_pending ? NULL : "device_restarted",
            false,
            false)) {
        clear_after_terminal_ack(state);
        return false;
    }
    return true;
}
