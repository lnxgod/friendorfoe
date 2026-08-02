#include "backend_uart_investigation.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "backend_json_reader.h"
#include "ble_investigation_protocol.h"

#define BEGIN_FIELD_COUNT 4U
#define PROGRESS_FIELD_COUNT 3U
#define SERVICE_FIELD_COUNT 4U
#define CHARACTERISTIC_FIELD_COUNT 6U
#define READ_FIELD_COUNT 5U
#define END_FIELD_COUNT 7U
#define PARSE_TOKEN_CAPACITY 32U

typedef struct {
    const char *name;
    uint16_t mask;
} property_name_t;

static const property_name_t PROPERTY_NAMES[] = {
    {"broadcast", BLE_INV_PROP_BROADCAST},
    {"read", BLE_INV_PROP_READ},
    {"write_without_response", BLE_INV_PROP_WRITE_WITHOUT_RESPONSE},
    {"write", BLE_INV_PROP_WRITE},
    {"notify", BLE_INV_PROP_NOTIFY},
    {"indicate", BLE_INV_PROP_INDICATE},
    {"authenticated_signed_writes",
     BLE_INV_PROP_AUTHENTICATED_SIGNED_WRITES},
    {"extended_properties", BLE_INV_PROP_EXTENDED_PROPERTIES},
};

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

static bool hex_character(char value)
{
    return lower_hex_character(value) ||
           (value >= 'A' && value <= 'F');
}

static bool valid_request_id(const char *request_id)
{
    if (request_id == NULL ||
        strlen(request_id) != BLE_INV_REQUEST_ID_LEN - 1U) {
        return false;
    }
    for (size_t index = 0U;
         index < BLE_INV_REQUEST_ID_LEN - 1U; ++index) {
        if (!lower_hex_character(request_id[index])) {
            return false;
        }
    }
    return true;
}

static bool valid_canonical_mac(const char *mac)
{
    if (mac == NULL || strlen(mac) != 17U) {
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

static bool valid_uuid(const char *uuid)
{
    if (uuid == NULL) {
        return false;
    }
    const size_t length = strlen(uuid);
    if (length != 4U && length != 8U && length != 36U) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const bool hyphen = length == 36U &&
            (index == 8U || index == 13U ||
             index == 18U || index == 23U);
        if (hyphen ? uuid[index] != '-' : !hex_character(uuid[index])) {
            return false;
        }
    }
    return true;
}

static bool valid_value_hex(const char *value_hex)
{
    if (value_hex == NULL) {
        return false;
    }
    const size_t length = strlen(value_hex);
    if (length > BLE_INV_READ_HEX_LEN - 1U || (length & 1U) != 0U) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        if (!hex_character(value_hex[index])) {
            return false;
        }
    }
    return true;
}

static bool find_field(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    const char *name,
    size_t *out_index)
{
    return backend_json_object_find(
        json, tokens, token_count, 0U, name, out_index);
}

static bool copy_string_field(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    const char *name,
    char *output,
    size_t capacity)
{
    size_t index = 0U;
    return find_field(json, tokens, token_count, name, &index) &&
           backend_json_copy_string(
               json, &tokens[index], output, capacity);
}

static bool exact_object_shape(
    const backend_json_token_t *tokens,
    size_t token_count,
    size_t field_count)
{
    return tokens != NULL && token_count == 1U + field_count * 2U &&
           tokens[0].kind == BACKEND_JSON_OBJECT &&
           tokens[0].child_count == field_count * 2U;
}

static bool decode_request_id(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    ble_investigation_chunk_t *chunk)
{
    return copy_string_field(
               json, tokens, token_count, "request_id",
               chunk->request_id, sizeof(chunk->request_id)) &&
           valid_request_id(chunk->request_id);
}

static bool decode_index(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    int maximum_exclusive,
    int *out)
{
    size_t index = 0U;
    int64_t value = 0;
    if (!find_field(json, tokens, token_count, "index", &index) ||
        !backend_json_get_i64(json, &tokens[index], &value) ||
        value < 0 || value >= maximum_exclusive || value > INT_MAX) {
        return false;
    }
    *out = (int)value;
    return true;
}

static bool decode_state(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    ble_investigation_state_t minimum,
    ble_investigation_state_t maximum,
    ble_investigation_state_t *out)
{
    char state[16];
    return copy_string_field(
               json, tokens, token_count, "state",
               state, sizeof(state)) &&
           ble_investigation_state_from_name(state, out) &&
           *out >= minimum && *out <= maximum;
}

static bool decode_begin(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    ble_investigation_chunk_t *chunk)
{
    if (!exact_object_shape(tokens, token_count, BEGIN_FIELD_COUNT) ||
        !decode_request_id(json, tokens, token_count, chunk)) {
        return false;
    }
    char mode[24];
    size_t target_index = 0U;
    if (!copy_string_field(
            json, tokens, token_count, "mode", mode, sizeof(mode)) ||
        !ble_investigation_mode_from_name(mode, &chunk->mode) ||
        !find_field(
            json, tokens, token_count, "target_mac", &target_index)) {
        return false;
    }
    if (chunk->mode == BLE_INV_MODE_GATT) {
        return backend_json_copy_string(
                   json, &tokens[target_index], chunk->target_mac,
                   sizeof(chunk->target_mac)) &&
               valid_canonical_mac(chunk->target_mac);
    }
    return chunk->mode == BLE_INV_MODE_PASSIVE_CAPTURE &&
           tokens[target_index].kind == BACKEND_JSON_NULL;
}

static bool decode_progress(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    ble_investigation_chunk_t *chunk)
{
    return exact_object_shape(tokens, token_count, PROGRESS_FIELD_COUNT) &&
           decode_request_id(json, tokens, token_count, chunk) &&
           decode_state(
               json, tokens, token_count,
               BLE_INV_QUEUED, BLE_INV_READING, &chunk->state);
}

static bool decode_service(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    ble_investigation_chunk_t *chunk)
{
    return exact_object_shape(tokens, token_count, SERVICE_FIELD_COUNT) &&
           decode_request_id(json, tokens, token_count, chunk) &&
           decode_index(
               json, tokens, token_count,
               BLE_INV_MAX_SERVICES, &chunk->index) &&
           copy_string_field(
               json, tokens, token_count, "uuid",
               chunk->uuid, sizeof(chunk->uuid)) &&
           valid_uuid(chunk->uuid);
}

static bool property_mask_from_name(const char *name, uint16_t *out)
{
    for (size_t index = 0U;
         index < sizeof(PROPERTY_NAMES) / sizeof(PROPERTY_NAMES[0]);
         ++index) {
        if (strcmp(name, PROPERTY_NAMES[index].name) == 0) {
            *out = PROPERTY_NAMES[index].mask;
            return true;
        }
    }
    return false;
}

static bool decode_properties(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    uint16_t *out)
{
    size_t array_index = 0U;
    if (!find_field(
            json, tokens, token_count, "properties", &array_index) ||
        tokens[array_index].kind != BACKEND_JSON_ARRAY ||
        tokens[array_index].child_count >
            sizeof(PROPERTY_NAMES) / sizeof(PROPERTY_NAMES[0]) ||
        token_count != 1U + CHARACTERISTIC_FIELD_COUNT * 2U +
            tokens[array_index].child_count) {
        return false;
    }

    uint16_t properties = 0U;
    size_t found = 0U;
    for (size_t index = array_index + 1U;
         index < token_count; ++index) {
        if (tokens[index].parent != (int16_t)array_index) {
            continue;
        }
        char name[32];
        uint16_t mask = 0U;
        if (!backend_json_copy_string(
                json, &tokens[index], name, sizeof(name)) ||
            !property_mask_from_name(name, &mask) ||
            (properties & mask) != 0U) {
            return false;
        }
        properties = (uint16_t)(properties | mask);
        ++found;
    }
    if (found != tokens[array_index].child_count) {
        return false;
    }
    *out = properties;
    return true;
}

static bool decode_characteristic(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    ble_investigation_chunk_t *chunk)
{
    if (tokens[0].kind != BACKEND_JSON_OBJECT ||
        tokens[0].child_count != CHARACTERISTIC_FIELD_COUNT * 2U ||
        !decode_request_id(json, tokens, token_count, chunk) ||
        !decode_index(
            json, tokens, token_count,
            BLE_INV_MAX_CHARS, &chunk->index) ||
        !copy_string_field(
            json, tokens, token_count, "service_uuid",
            chunk->service_uuid, sizeof(chunk->service_uuid)) ||
        !valid_uuid(chunk->service_uuid) ||
        !copy_string_field(
            json, tokens, token_count, "uuid",
            chunk->uuid, sizeof(chunk->uuid)) ||
        !valid_uuid(chunk->uuid)) {
        return false;
    }
    return decode_properties(
        json, tokens, token_count, &chunk->properties);
}

static bool decode_read(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    ble_investigation_chunk_t *chunk)
{
    return exact_object_shape(tokens, token_count, READ_FIELD_COUNT) &&
           decode_request_id(json, tokens, token_count, chunk) &&
           decode_index(
               json, tokens, token_count,
               BLE_INV_MAX_READS, &chunk->index) &&
           copy_string_field(
               json, tokens, token_count, "uuid",
               chunk->uuid, sizeof(chunk->uuid)) &&
           valid_uuid(chunk->uuid) &&
           copy_string_field(
               json, tokens, token_count, "value_hex",
               chunk->value_hex, sizeof(chunk->value_hex)) &&
           valid_value_hex(chunk->value_hex);
}

static bool decode_end(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    ble_investigation_chunk_t *chunk)
{
    if (!exact_object_shape(tokens, token_count, END_FIELD_COUNT) ||
        !decode_request_id(json, tokens, token_count, chunk) ||
        !decode_state(
            json, tokens, token_count,
            BLE_INV_COMPLETE, BLE_INV_CANCELLED, &chunk->state) ||
        !copy_string_field(
            json, tokens, token_count, "summary",
            chunk->summary, sizeof(chunk->summary))) {
        return false;
    }

    size_t error_index = 0U;
    size_t authentication_index = 0U;
    size_t truncated_index = 0U;
    if (!find_field(
            json, tokens, token_count, "error", &error_index) ||
        !find_field(
            json, tokens, token_count,
            "authentication_required", &authentication_index) ||
        !find_field(
            json, tokens, token_count, "truncated", &truncated_index) ||
        !backend_json_get_bool(
            json, &tokens[authentication_index],
            &chunk->authentication_required) ||
        !backend_json_get_bool(
            json, &tokens[truncated_index], &chunk->truncated)) {
        return false;
    }
    if (tokens[error_index].kind == BACKEND_JSON_NULL) {
        chunk->error[0] = '\0';
        return true;
    }
    return backend_json_copy_string(
        json, &tokens[error_index], chunk->error, sizeof(chunk->error));
}

static backend_uart_investigation_decode_result_t parse_result_to_decode(
    backend_json_result_t result)
{
    if (result == BACKEND_JSON_DUPLICATE_KEY) {
        return BACKEND_UART_INVESTIGATION_DECODE_SCHEMA_MISMATCH;
    }
    if (result == BACKEND_JSON_TOO_MANY_TOKENS ||
        result == BACKEND_JSON_RANGE) {
        return BACKEND_UART_INVESTIGATION_DECODE_TOO_LARGE;
    }
    return BACKEND_UART_INVESTIGATION_DECODE_MALFORMED;
}

backend_uart_investigation_decode_result_t
backend_uart_investigation_decode(
    const uint8_t *line,
    size_t length,
    ble_investigation_chunk_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (line == NULL || out == NULL || length == 0U) {
        return BACKEND_UART_INVESTIGATION_DECODE_MALFORMED;
    }
    if (length > BACKEND_UART_INVESTIGATION_MAX_JSON + 1U) {
        return BACKEND_UART_INVESTIGATION_DECODE_TOO_LARGE;
    }

    size_t json_length = length;
    if (line[json_length - 1U] == '\n') {
        --json_length;
    }
    if (json_length > BACKEND_UART_INVESTIGATION_MAX_JSON) {
        return BACKEND_UART_INVESTIGATION_DECODE_TOO_LARGE;
    }
    if (json_length == 0U || line[0] != '{' ||
        line[json_length - 1U] != '}') {
        return BACKEND_UART_INVESTIGATION_DECODE_MALFORMED;
    }

    const char *json = (const char *)line;
    backend_json_token_t tokens[PARSE_TOKEN_CAPACITY];
    size_t token_count = 0U;
    const backend_json_result_t parsed = backend_json_parse(
        json, json_length, tokens, PARSE_TOKEN_CAPACITY, &token_count);
    if (parsed != BACKEND_JSON_OK) {
        return parse_result_to_decode(parsed);
    }
    if (token_count == 0U || tokens[0].kind != BACKEND_JSON_OBJECT) {
        return BACKEND_UART_INVESTIGATION_DECODE_SCHEMA_MISMATCH;
    }

    char type[24];
    if (!copy_string_field(
            json, tokens, token_count, "type", type, sizeof(type))) {
        return BACKEND_UART_INVESTIGATION_DECODE_SCHEMA_MISMATCH;
    }

    ble_investigation_chunk_t chunk;
    memset(&chunk, 0, sizeof(chunk));
    bool valid = false;
    if (strcmp(type, MSG_TYPE_BLE_INV_BEGIN) == 0) {
        chunk.kind = BLE_INV_CHUNK_BEGIN;
        valid = decode_begin(json, tokens, token_count, &chunk);
    } else if (strcmp(type, MSG_TYPE_BLE_INV_PROGRESS) == 0) {
        chunk.kind = BLE_INV_CHUNK_PROGRESS;
        valid = decode_progress(json, tokens, token_count, &chunk);
    } else if (strcmp(type, MSG_TYPE_BLE_INV_SERVICE) == 0) {
        chunk.kind = BLE_INV_CHUNK_SERVICE;
        valid = decode_service(json, tokens, token_count, &chunk);
    } else if (strcmp(type, MSG_TYPE_BLE_INV_CHAR) == 0) {
        chunk.kind = BLE_INV_CHUNK_CHARACTERISTIC;
        valid = decode_characteristic(json, tokens, token_count, &chunk);
    } else if (strcmp(type, MSG_TYPE_BLE_INV_READ) == 0) {
        chunk.kind = BLE_INV_CHUNK_READ;
        valid = decode_read(json, tokens, token_count, &chunk);
    } else if (strcmp(type, MSG_TYPE_BLE_INV_END) == 0) {
        chunk.kind = BLE_INV_CHUNK_END;
        valid = decode_end(json, tokens, token_count, &chunk);
    }
    if (!valid) {
        return BACKEND_UART_INVESTIGATION_DECODE_SCHEMA_MISMATCH;
    }

    *out = chunk;
    return BACKEND_UART_INVESTIGATION_DECODE_OK;
}
