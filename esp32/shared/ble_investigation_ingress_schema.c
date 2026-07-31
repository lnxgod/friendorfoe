#include "ble_investigation_ingress_schema.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ble_investigation_types.h"
#include "firmware_json_schema.h"

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))
#define TOKEN_MEMBER(member_name)                                           \
    {member_name, FOF_JSON_STRING,                                          \
     FOF_JSON_STRING_POLICY_ASCII_TOKEN_NO_ESCAPE}
#define PRINTABLE_MEMBER(member_name)                                       \
    {member_name, FOF_JSON_STRING,                                          \
     FOF_JSON_STRING_POLICY_PRINTABLE_UTF8}
#define NULLABLE_TOKEN_MEMBER(member_name)                                  \
    {member_name, FOF_JSON_NULLABLE_STRING,                                 \
     FOF_JSON_STRING_POLICY_ASCII_TOKEN_NO_ESCAPE}
#define NULLABLE_PRINTABLE_MEMBER(member_name)                              \
    {member_name, FOF_JSON_NULLABLE_STRING,                                 \
     FOF_JSON_STRING_POLICY_PRINTABLE_UTF8}
#define INT32_MEMBER(member_name)                                           \
    {member_name, FOF_JSON_INT32, FOF_JSON_STRING_POLICY_NONE}
#define BOOL_MEMBER(member_name)                                            \
    {member_name, FOF_JSON_BOOL, FOF_JSON_STRING_POLICY_NONE}
#define ARRAY_MEMBER(member_name)                                           \
    {member_name, FOF_JSON_ARRAY, FOF_JSON_STRING_POLICY_NONE}

enum {
    BLE_INV_MAX_SCHEMA_MEMBERS = 7,
};

typedef struct {
    fof_ble_inv_ingress_schema_id_t id;
    const char *selector;
    const fof_json_member_spec_t *members;
    size_t member_count;
} ble_inv_schema_t;

static const fof_json_member_spec_t BEGIN_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("request_id"),
    TOKEN_MEMBER("mode"),
    NULLABLE_TOKEN_MEMBER("target_mac"),
};

static const fof_json_member_spec_t PROGRESS_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("request_id"),
    TOKEN_MEMBER("state"),
};

static const fof_json_member_spec_t SERVICE_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("request_id"),
    INT32_MEMBER("index"),
    TOKEN_MEMBER("uuid"),
};

static const fof_json_member_spec_t CHARACTERISTIC_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("request_id"),
    INT32_MEMBER("index"),
    TOKEN_MEMBER("service_uuid"),
    TOKEN_MEMBER("uuid"),
    ARRAY_MEMBER("properties"),
};

static const fof_json_member_spec_t READ_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("request_id"),
    INT32_MEMBER("index"),
    TOKEN_MEMBER("uuid"),
    TOKEN_MEMBER("value_hex"),
};

static const fof_json_member_spec_t END_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("request_id"),
    TOKEN_MEMBER("state"),
    PRINTABLE_MEMBER("summary"),
    NULLABLE_PRINTABLE_MEMBER("error"),
    BOOL_MEMBER("authentication_required"),
    BOOL_MEMBER("truncated"),
};

#define BLE_INV_SCHEMA(schema_id, selector_value, member_array)             \
    [schema_id] = {                                                         \
        .id = schema_id,                                                     \
        .selector = selector_value,                                          \
        .members = member_array,                                             \
        .member_count = ARRAY_SIZE(member_array),                            \
    }

static const ble_inv_schema_t
BLE_INV_SCHEMAS[FOF_BLE_INV_INGRESS_SCHEMA_COUNT] = {
    BLE_INV_SCHEMA(
        FOF_BLE_INV_INGRESS_BEGIN, "ble_inv_begin", BEGIN_MEMBERS),
    BLE_INV_SCHEMA(
        FOF_BLE_INV_INGRESS_PROGRESS, "ble_inv_progress", PROGRESS_MEMBERS),
    BLE_INV_SCHEMA(
        FOF_BLE_INV_INGRESS_SERVICE, "ble_inv_service", SERVICE_MEMBERS),
    BLE_INV_SCHEMA(
        FOF_BLE_INV_INGRESS_CHARACTERISTIC,
        "ble_inv_char", CHARACTERISTIC_MEMBERS),
    BLE_INV_SCHEMA(
        FOF_BLE_INV_INGRESS_READ, "ble_inv_read", READ_MEMBERS),
    BLE_INV_SCHEMA(
        FOF_BLE_INV_INGRESS_END, "ble_inv_end", END_MEMBERS),
};

static bool span_equals(
    const fof_json_value_span_t *span,
    const char *literal)
{
    if (!span || !span->bytes || !literal) {
        return false;
    }
    size_t literal_len = strlen(literal);
    return span->byte_len == literal_len &&
           memcmp(span->bytes, literal, literal_len) == 0;
}

static bool token_span(
    const fof_json_value_span_t *raw,
    fof_json_value_span_t *token_out)
{
    return fof_json_value_span_parse_ascii_token(raw, token_out);
}

static bool request_id_is_valid(const fof_json_value_span_t *raw)
{
    fof_json_value_span_t request_id = {0};
    return token_span(raw, &request_id) &&
           request_id.byte_len >= 1U &&
           request_id.byte_len <= 32U;
}

static bool canonical_mac_is_valid(
    const fof_json_value_span_t *token)
{
    if (!token || !token->bytes || token->byte_len != 17U) {
        return false;
    }
    for (size_t index = 0U; index < token->byte_len; ++index) {
        uint8_t byte = token->bytes[index];
        if ((index + 1U) % 3U == 0U) {
            if (byte != ':') {
                return false;
            }
        } else if (!((byte >= '0' && byte <= '9') ||
                     (byte >= 'A' && byte <= 'F'))) {
            return false;
        }
    }
    return true;
}

static bool uppercase_hex(uint8_t byte)
{
    return (byte >= '0' && byte <= '9') ||
           (byte >= 'A' && byte <= 'F');
}

static bool uuid_is_valid(const fof_json_value_span_t *raw)
{
    fof_json_value_span_t uuid = {0};
    if (!token_span(raw, &uuid) ||
        (uuid.byte_len != 4U &&
         uuid.byte_len != 8U &&
         uuid.byte_len != 36U)) {
        return false;
    }
    for (size_t index = 0U; index < uuid.byte_len; ++index) {
        if (uuid.byte_len == 36U &&
            (index == 8U || index == 13U ||
             index == 18U || index == 23U)) {
            if (uuid.bytes[index] != '-') {
                return false;
            }
        } else if (!uppercase_hex(uuid.bytes[index])) {
            return false;
        }
    }
    return true;
}

static bool value_hex_is_valid(const fof_json_value_span_t *raw)
{
    fof_json_value_span_t value = {0};
    if (span_equals(raw, "\"\"")) {
        return true;
    }
    if (!token_span(raw, &value) ||
        value.byte_len > BLE_INV_READ_HEX_LEN - 1U ||
        value.byte_len % 2U != 0U) {
        return false;
    }
    for (size_t index = 0U; index < value.byte_len; ++index) {
        if (!uppercase_hex(value.bytes[index])) {
            return false;
        }
    }
    return true;
}

static bool printable_string_fits(
    const fof_json_value_span_t *raw,
    size_t destination_capacity)
{
    size_t decoded_byte_len = 0U;
    return destination_capacity > 0U &&
           fof_json_value_span_parse_printable_utf8_length(
               raw, &decoded_byte_len) &&
           decoded_byte_len < destination_capacity;
}

static bool nullable_printable_string_fits(
    const fof_json_value_span_t *raw,
    size_t destination_capacity)
{
    return span_equals(raw, "null") ||
           printable_string_fits(raw, destination_capacity);
}

static bool json_array_skip_space(
    const fof_json_value_span_t *raw,
    size_t *position)
{
    if (!raw || !position) {
        return false;
    }
    while (*position < raw->byte_len) {
        uint8_t byte = raw->bytes[*position];
        if (byte != ' ' && byte != '\t' &&
            byte != '\r' && byte != '\n') {
            break;
        }
        (*position)++;
    }
    return true;
}

static bool properties_array_is_valid(
    const fof_json_value_span_t *raw)
{
    static const char *const names[] = {
        "broadcast",
        "read",
        "write_without_response",
        "write",
        "notify",
        "indicate",
        "authenticated_signed_writes",
        "extended_properties",
    };
    if (!raw || !raw->bytes || raw->byte_len < 2U ||
        raw->bytes[0] != '[') {
        return false;
    }
    size_t position = 1U;
    uint32_t seen = 0U;
    (void)json_array_skip_space(raw, &position);
    if (position < raw->byte_len && raw->bytes[position] == ']') {
        return position + 1U == raw->byte_len;
    }

    while (position < raw->byte_len) {
        if (raw->bytes[position] != '"') {
            return false;
        }
        position++;
        size_t start = position;
        while (position < raw->byte_len &&
               raw->bytes[position] != '"') {
            uint8_t byte = raw->bytes[position];
            if (byte == '\\' || byte < 0x21U || byte > 0x7eU) {
                return false;
            }
            position++;
        }
        if (position >= raw->byte_len) {
            return false;
        }
        size_t name_length = position - start;
        position++;

        size_t matched = ARRAY_SIZE(names);
        for (size_t index = 0U; index < ARRAY_SIZE(names); ++index) {
            if (strlen(names[index]) == name_length &&
                memcmp(
                    raw->bytes + start, names[index],
                    name_length) == 0) {
                matched = index;
                break;
            }
        }
        if (matched == ARRAY_SIZE(names) ||
            (seen & (UINT32_C(1) << matched)) != 0U) {
            return false;
        }
        seen |= UINT32_C(1) << matched;

        (void)json_array_skip_space(raw, &position);
        if (position >= raw->byte_len) {
            return false;
        }
        if (raw->bytes[position] == ']') {
            return position + 1U == raw->byte_len;
        }
        if (raw->bytes[position] != ',') {
            return false;
        }
        position++;
        (void)json_array_skip_space(raw, &position);
    }
    return false;
}

static bool index_is_valid(
    const fof_json_value_span_t *raw,
    int32_t maximum)
{
    int32_t index = -1;
    return fof_json_value_span_parse_int32(raw, &index) &&
           index >= 0 && index <= maximum;
}

static bool begin_semantics_valid(
    const fof_json_value_span_t *values)
{
    fof_json_value_span_t mode = {0};
    fof_json_value_span_t target = {0};
    bool target_is_null = false;
    if (!request_id_is_valid(&values[1]) ||
        !token_span(&values[2], &mode) ||
        !fof_json_value_span_parse_nullable_ascii_token(
            &values[3], &target_is_null, &target)) {
        return false;
    }
    if (span_equals(&mode, "gatt")) {
        return !target_is_null && canonical_mac_is_valid(&target);
    }
    return span_equals(&mode, "passive_capture") && target_is_null;
}

static bool progress_semantics_valid(
    const fof_json_value_span_t *values)
{
    fof_json_value_span_t state = {0};
    return request_id_is_valid(&values[1]) &&
           token_span(&values[2], &state) &&
           (span_equals(&state, "queued") ||
            span_equals(&state, "scanning") ||
            span_equals(&state, "connecting") ||
            span_equals(&state, "discovering") ||
            span_equals(&state, "reading"));
}

static bool end_semantics_valid(
    const fof_json_value_span_t *values)
{
    fof_json_value_span_t state = {0};
    return request_id_is_valid(&values[1]) &&
           token_span(&values[2], &state) &&
           (span_equals(&state, "complete") ||
            span_equals(&state, "failed") ||
            span_equals(&state, "cancelled")) &&
           printable_string_fits(
               &values[3], BLE_INV_SUMMARY_LEN) &&
           nullable_printable_string_fits(
               &values[4], BLE_INV_ERROR_LEN);
}

static bool schema_semantics_valid(
    fof_ble_inv_ingress_schema_id_t id,
    const fof_json_value_span_t *values)
{
    switch (id) {
        case FOF_BLE_INV_INGRESS_BEGIN:
            return begin_semantics_valid(values);
        case FOF_BLE_INV_INGRESS_PROGRESS:
            return progress_semantics_valid(values);
        case FOF_BLE_INV_INGRESS_SERVICE:
            return request_id_is_valid(&values[1]) &&
                   index_is_valid(
                       &values[2], BLE_INV_MAX_SERVICES - 1) &&
                   uuid_is_valid(&values[3]);
        case FOF_BLE_INV_INGRESS_CHARACTERISTIC:
            return request_id_is_valid(&values[1]) &&
                   index_is_valid(
                       &values[2], BLE_INV_MAX_CHARS - 1) &&
                   uuid_is_valid(&values[3]) &&
                   uuid_is_valid(&values[4]) &&
                   properties_array_is_valid(&values[5]);
        case FOF_BLE_INV_INGRESS_READ:
            return request_id_is_valid(&values[1]) &&
                   index_is_valid(
                       &values[2], BLE_INV_MAX_READS - 1) &&
                   uuid_is_valid(&values[3]) &&
                   value_hex_is_valid(&values[4]);
        case FOF_BLE_INV_INGRESS_END:
            return end_semantics_valid(values);
        case FOF_BLE_INV_INGRESS_NONE:
        case FOF_BLE_INV_INGRESS_SCHEMA_COUNT:
        default:
            return false;
    }
}

fof_ble_inv_ingress_result_t fof_ble_investigation_ingress_validate(
    const uint8_t *bytes,
    size_t byte_len,
    int scanner_slot,
    fof_ble_inv_ingress_schema_id_t *schema_out)
{
    if (schema_out) {
        *schema_out = FOF_BLE_INV_INGRESS_NONE;
    }
    if (!bytes || byte_len == 0U || !schema_out) {
        return FOF_BLE_INV_INGRESS_INVALID_ARGUMENT;
    }
    if (scanner_slot != FOF_BLE_INVESTIGATION_SCANNER_SLOT) {
        return FOF_BLE_INV_INGRESS_WRONG_SCANNER_SLOT;
    }

    char selector[32] = {0};
    size_t selector_len = 0U;
    if (fof_json_extract_unique_ascii_token_member(
            bytes, byte_len, "type", selector, sizeof(selector),
            &selector_len) != FOF_JSON_SCHEMA_OK) {
        return FOF_BLE_INV_INGRESS_SELECTOR_REJECTED;
    }

    const ble_inv_schema_t *schema = NULL;
    for (size_t id = FOF_BLE_INV_INGRESS_BEGIN;
         id < FOF_BLE_INV_INGRESS_SCHEMA_COUNT; ++id) {
        const ble_inv_schema_t *candidate = &BLE_INV_SCHEMAS[id];
        if (candidate->id == (fof_ble_inv_ingress_schema_id_t)id &&
            strlen(candidate->selector) == selector_len &&
            memcmp(candidate->selector, selector, selector_len) == 0) {
            schema = candidate;
            break;
        }
    }
    if (!schema) {
        return FOF_BLE_INV_INGRESS_UNKNOWN_SELECTOR;
    }

    fof_json_value_span_t values[BLE_INV_MAX_SCHEMA_MEMBERS] = {0};
    if (fof_json_validate_exact_object_capture(
            bytes, byte_len, schema->members, schema->member_count,
            values, ARRAY_SIZE(values)) != FOF_JSON_SCHEMA_OK) {
        return FOF_BLE_INV_INGRESS_SCHEMA_REJECTED;
    }
    fof_json_value_span_t captured_selector = {0};
    if (!token_span(&values[0], &captured_selector) ||
        !span_equals(&captured_selector, schema->selector)) {
        return FOF_BLE_INV_INGRESS_SCHEMA_REJECTED;
    }
    if (!schema_semantics_valid(schema->id, values)) {
        return FOF_BLE_INV_INGRESS_SEMANTIC_REJECTED;
    }

    *schema_out = schema->id;
    return FOF_BLE_INV_INGRESS_OK;
}
