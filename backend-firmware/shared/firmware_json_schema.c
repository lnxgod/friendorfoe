#include "firmware_json_schema.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
    FOF_JSON_SCHEMA_MAX_MEMBERS = 64,
    FOF_JSON_SCHEMA_MAX_NESTING = 32,
    FOF_JSON_SCHEMA_MAX_MEMBER_NAME = 63,
};

typedef struct {
    const uint8_t *bytes;
    size_t byte_len;
    size_t pos;
} fof_json_cursor_t;

typedef enum {
    FOF_JSON_VALUE_NULL = 0,
    FOF_JSON_VALUE_BOOL,
    FOF_JSON_VALUE_STRING,
    FOF_JSON_VALUE_NUMBER,
    FOF_JSON_VALUE_OBJECT,
    FOF_JSON_VALUE_ARRAY,
} fof_json_value_kind_t;

typedef struct {
    const uint8_t *bytes;
    size_t byte_len;
    size_t decoded_byte_len;
    bool had_escape;
    bool ascii_token_eligible;
} fof_json_string_span_t;

typedef struct {
    fof_json_value_kind_t kind;
    bool number_negative;
    bool number_integer;
    bool number_overflow;
    uint64_t number_magnitude;
    fof_json_string_span_t string_span;
    const uint8_t *raw_bytes;
    size_t raw_byte_len;
} fof_json_value_t;

static bool json_is_whitespace(uint8_t byte)
{
    return byte == 0x20U || byte == 0x09U ||
           byte == 0x0aU || byte == 0x0dU;
}

static void json_skip_whitespace(fof_json_cursor_t *cursor)
{
    while (cursor->pos < cursor->byte_len &&
           json_is_whitespace(cursor->bytes[cursor->pos])) {
        cursor->pos++;
    }
}

static bool json_consume(fof_json_cursor_t *cursor, uint8_t expected)
{
    if (cursor->pos >= cursor->byte_len ||
        cursor->bytes[cursor->pos] != expected) {
        return false;
    }
    cursor->pos++;
    return true;
}

static bool json_hex_nibble(uint8_t byte, uint8_t *value_out)
{
    uint8_t value;
    if (byte >= '0' && byte <= '9') {
        value = (uint8_t)(byte - '0');
    } else if (byte >= 'a' && byte <= 'f') {
        value = (uint8_t)(byte - 'a' + 10U);
    } else if (byte >= 'A' && byte <= 'F') {
        value = (uint8_t)(byte - 'A' + 10U);
    } else {
        return false;
    }
    if (value_out) {
        *value_out = value;
    }
    return true;
}

static bool json_decode_hex4(const uint8_t *bytes, size_t byte_len,
                             size_t offset, uint16_t *value_out)
{
    if (!bytes || offset > byte_len || byte_len - offset < 4U) {
        return false;
    }
    uint16_t value = 0U;
    for (size_t i = 0U; i < 4U; ++i) {
        uint8_t nibble = 0U;
        if (!json_hex_nibble(bytes[offset + i], &nibble)) {
            return false;
        }
        value = (uint16_t)((value << 4U) | nibble);
    }
    if (value_out) {
        *value_out = value;
    }
    return true;
}

static bool json_unicode_escape(fof_json_cursor_t *cursor,
                                size_t *decoded_byte_len_out)
{
    if (cursor->pos > cursor->byte_len ||
        cursor->byte_len - cursor->pos < 6U ||
        cursor->bytes[cursor->pos] != '\\' ||
        cursor->bytes[cursor->pos + 1U] != 'u') {
        return false;
    }

    uint16_t first = 0U;
    if (!json_decode_hex4(
            cursor->bytes, cursor->byte_len, cursor->pos + 2U, &first)) {
        return false;
    }
    if (first <= 0x001fU || first == 0x007fU) {
        return false;
    }
    if (first >= 0xdc00U && first <= 0xdfffU) {
        return false;
    }
    if (first >= 0xd800U && first <= 0xdbffU) {
        if (cursor->byte_len - cursor->pos < 12U ||
            cursor->bytes[cursor->pos + 6U] != '\\' ||
            cursor->bytes[cursor->pos + 7U] != 'u') {
            return false;
        }
        uint16_t second = 0U;
        if (!json_decode_hex4(
                cursor->bytes, cursor->byte_len,
                cursor->pos + 8U, &second) ||
            second < 0xdc00U || second > 0xdfffU) {
            return false;
        }
        cursor->pos += 12U;
        if (decoded_byte_len_out) {
            *decoded_byte_len_out = 4U;
        }
        return true;
    }

    cursor->pos += 6U;
    if (decoded_byte_len_out) {
        if (first <= 0x007fU) {
            *decoded_byte_len_out = 1U;
        } else if (first <= 0x07ffU) {
            *decoded_byte_len_out = 2U;
        } else {
            *decoded_byte_len_out = 3U;
        }
    }
    return true;
}

static bool json_consume_utf8_scalar(fof_json_cursor_t *cursor)
{
    if (!cursor || cursor->pos >= cursor->byte_len) {
        return false;
    }

    uint8_t first = cursor->bytes[cursor->pos];
    size_t sequence_len = 0U;
    uint8_t second_min = 0x80U;
    uint8_t second_max = 0xbfU;

    if (first >= 0xc2U && first <= 0xdfU) {
        sequence_len = 2U;
    } else if (first == 0xe0U) {
        sequence_len = 3U;
        second_min = 0xa0U;
    } else if ((first >= 0xe1U && first <= 0xecU) ||
               (first >= 0xeeU && first <= 0xefU)) {
        sequence_len = 3U;
    } else if (first == 0xedU) {
        sequence_len = 3U;
        second_max = 0x9fU;
    } else if (first == 0xf0U) {
        sequence_len = 4U;
        second_min = 0x90U;
    } else if (first >= 0xf1U && first <= 0xf3U) {
        sequence_len = 4U;
    } else if (first == 0xf4U) {
        sequence_len = 4U;
        second_max = 0x8fU;
    } else {
        return false;
    }

    if (cursor->byte_len - cursor->pos < sequence_len) {
        return false;
    }
    uint8_t second = cursor->bytes[cursor->pos + 1U];
    if (second < second_min || second > second_max) {
        return false;
    }
    for (size_t i = 2U; i < sequence_len; ++i) {
        uint8_t continuation = cursor->bytes[cursor->pos + i];
        if (continuation < 0x80U || continuation > 0xbfU) {
            return false;
        }
    }

    cursor->pos += sequence_len;
    return true;
}

static bool json_parse_string(fof_json_cursor_t *cursor,
                              fof_json_string_span_t *span_out)
{
    if (!json_consume(cursor, '"')) {
        return false;
    }
    size_t start = cursor->pos;
    size_t decoded_byte_len = 0U;
    bool had_escape = false;
    bool ascii_token_eligible = true;

    while (cursor->pos < cursor->byte_len) {
        uint8_t byte = cursor->bytes[cursor->pos];
        if (byte == '"') {
            if (span_out) {
                span_out->bytes = cursor->bytes + start;
                span_out->byte_len = cursor->pos - start;
                span_out->decoded_byte_len = decoded_byte_len;
                span_out->had_escape = had_escape;
                span_out->ascii_token_eligible =
                    ascii_token_eligible;
            }
            cursor->pos++;
            return true;
        }
        if (byte < 0x21U || byte > 0x7eU) {
            ascii_token_eligible = false;
        }
        if (byte < 0x20U || byte == 0x7fU) {
            return false;
        }
        if (byte >= 0x80U) {
            size_t scalar_start = cursor->pos;
            if (!json_consume_utf8_scalar(cursor)) {
                return false;
            }
            decoded_byte_len += cursor->pos - scalar_start;
            continue;
        }
        if (byte != '\\') {
            cursor->pos++;
            decoded_byte_len++;
            continue;
        }

        had_escape = true;
        ascii_token_eligible = false;
        if (cursor->byte_len - cursor->pos < 2U) {
            return false;
        }
        uint8_t escape = cursor->bytes[cursor->pos + 1U];
        if (escape == '"' || escape == '\\' || escape == '/') {
            cursor->pos += 2U;
            decoded_byte_len++;
            continue;
        }
        if (escape == 'u') {
            size_t unicode_byte_len = 0U;
            if (!json_unicode_escape(cursor, &unicode_byte_len)) {
                return false;
            }
            decoded_byte_len += unicode_byte_len;
            continue;
        }

        /* JSON's other short escapes decode to forbidden C0 controls. */
        return false;
    }
    return false;
}

static bool json_parse_number(fof_json_cursor_t *cursor,
                              fof_json_value_t *value_out)
{
    size_t pos = cursor->pos;
    bool negative = false;
    bool integer = true;
    bool overflow = false;
    uint64_t magnitude = 0U;

    if (pos < cursor->byte_len && cursor->bytes[pos] == '-') {
        negative = true;
        pos++;
    }
    if (pos >= cursor->byte_len ||
        cursor->bytes[pos] < '0' || cursor->bytes[pos] > '9') {
        return false;
    }

    if (cursor->bytes[pos] == '0') {
        pos++;
        if (pos < cursor->byte_len &&
            cursor->bytes[pos] >= '0' && cursor->bytes[pos] <= '9') {
            return false;
        }
    } else {
        while (pos < cursor->byte_len &&
               cursor->bytes[pos] >= '0' &&
               cursor->bytes[pos] <= '9') {
            uint64_t digit = (uint64_t)(cursor->bytes[pos] - '0');
            if (magnitude > (UINT64_MAX - digit) / 10U) {
                overflow = true;
            } else if (!overflow) {
                magnitude = magnitude * 10U + digit;
            }
            pos++;
        }
    }

    if (pos < cursor->byte_len && cursor->bytes[pos] == '.') {
        integer = false;
        pos++;
        if (pos >= cursor->byte_len ||
            cursor->bytes[pos] < '0' || cursor->bytes[pos] > '9') {
            return false;
        }
        while (pos < cursor->byte_len &&
               cursor->bytes[pos] >= '0' &&
               cursor->bytes[pos] <= '9') {
            pos++;
        }
    }

    if (pos < cursor->byte_len &&
        (cursor->bytes[pos] == 'e' || cursor->bytes[pos] == 'E')) {
        integer = false;
        pos++;
        if (pos < cursor->byte_len &&
            (cursor->bytes[pos] == '+' || cursor->bytes[pos] == '-')) {
            pos++;
        }
        if (pos >= cursor->byte_len ||
            cursor->bytes[pos] < '0' || cursor->bytes[pos] > '9') {
            return false;
        }
        while (pos < cursor->byte_len &&
               cursor->bytes[pos] >= '0' &&
               cursor->bytes[pos] <= '9') {
            pos++;
        }
    }

    cursor->pos = pos;
    if (value_out) {
        value_out->kind = FOF_JSON_VALUE_NUMBER;
        value_out->number_negative = negative;
        value_out->number_integer = integer;
        value_out->number_overflow = overflow;
        value_out->number_magnitude = magnitude;
    }
    return true;
}

static bool json_parse_value(fof_json_cursor_t *cursor, size_t parent_depth,
                             fof_json_value_t *value_out);

static bool json_parse_array(fof_json_cursor_t *cursor, size_t depth)
{
    if (depth > FOF_JSON_SCHEMA_MAX_NESTING ||
        !json_consume(cursor, '[')) {
        return false;
    }
    json_skip_whitespace(cursor);
    if (json_consume(cursor, ']')) {
        return true;
    }

    while (true) {
        fof_json_value_t ignored = {0};
        if (!json_parse_value(cursor, depth, &ignored)) {
            return false;
        }
        json_skip_whitespace(cursor);
        if (json_consume(cursor, ']')) {
            return true;
        }
        if (!json_consume(cursor, ',')) {
            return false;
        }
        json_skip_whitespace(cursor);
    }
}

static bool json_parse_object(fof_json_cursor_t *cursor, size_t depth)
{
    if (depth > FOF_JSON_SCHEMA_MAX_NESTING ||
        !json_consume(cursor, '{')) {
        return false;
    }
    json_skip_whitespace(cursor);
    if (json_consume(cursor, '}')) {
        return true;
    }

    while (true) {
        fof_json_string_span_t ignored_key = {0};
        if (!json_parse_string(cursor, &ignored_key)) {
            return false;
        }
        json_skip_whitespace(cursor);
        if (!json_consume(cursor, ':')) {
            return false;
        }
        json_skip_whitespace(cursor);
        fof_json_value_t ignored_value = {0};
        if (!json_parse_value(cursor, depth, &ignored_value)) {
            return false;
        }
        json_skip_whitespace(cursor);
        if (json_consume(cursor, '}')) {
            return true;
        }
        if (!json_consume(cursor, ',')) {
            return false;
        }
        json_skip_whitespace(cursor);
    }
}

static bool json_consume_literal(fof_json_cursor_t *cursor,
                                 const char *literal, size_t literal_len)
{
    if (!literal || cursor->pos > cursor->byte_len ||
        cursor->byte_len - cursor->pos < literal_len ||
        memcmp(cursor->bytes + cursor->pos, literal, literal_len) != 0) {
        return false;
    }
    cursor->pos += literal_len;
    return true;
}

static bool json_parse_value(fof_json_cursor_t *cursor, size_t parent_depth,
                             fof_json_value_t *value_out)
{
    if (!cursor || !value_out || cursor->pos >= cursor->byte_len) {
        return false;
    }
    memset(value_out, 0, sizeof(*value_out));

    size_t raw_start = cursor->pos;
    uint8_t first = cursor->bytes[cursor->pos];
    bool parsed = false;
    if (first == '"') {
        parsed = json_parse_string(cursor, &value_out->string_span);
        if (parsed) {
            value_out->kind = FOF_JSON_VALUE_STRING;
        }
    } else if (first == '{') {
        parsed = parent_depth < FOF_JSON_SCHEMA_MAX_NESTING &&
                 json_parse_object(cursor, parent_depth + 1U);
        if (parsed) {
            value_out->kind = FOF_JSON_VALUE_OBJECT;
        }
    } else if (first == '[') {
        parsed = parent_depth < FOF_JSON_SCHEMA_MAX_NESTING &&
                 json_parse_array(cursor, parent_depth + 1U);
        if (parsed) {
            value_out->kind = FOF_JSON_VALUE_ARRAY;
        }
    } else if (first == 'n') {
        parsed = json_consume_literal(cursor, "null", 4U);
        if (parsed) {
            value_out->kind = FOF_JSON_VALUE_NULL;
        }
    } else if (first == 't') {
        parsed = json_consume_literal(cursor, "true", 4U);
        if (parsed) {
            value_out->kind = FOF_JSON_VALUE_BOOL;
        }
    } else if (first == 'f') {
        parsed = json_consume_literal(cursor, "false", 5U);
        if (parsed) {
            value_out->kind = FOF_JSON_VALUE_BOOL;
        }
    } else if (first == '-' || (first >= '0' && first <= '9')) {
        parsed = json_parse_number(cursor, value_out);
    }
    if (!parsed) {
        return false;
    }
    value_out->raw_bytes = cursor->bytes + raw_start;
    value_out->raw_byte_len = cursor->pos - raw_start;
    return true;
}

static bool json_schema_member_name_length(const char *name,
                                           uint8_t *name_len_out)
{
    if (!name) {
        return false;
    }
    size_t len = 0U;
    while (len <= FOF_JSON_SCHEMA_MAX_MEMBER_NAME && name[len] != '\0') {
        uint8_t byte = (uint8_t)name[len];
        if (byte < 0x20U || byte > 0x7eU ||
            byte == '"' || byte == '\\') {
            return false;
        }
        len++;
    }
    if (len == 0U || len > FOF_JSON_SCHEMA_MAX_MEMBER_NAME) {
        return false;
    }
    if (name_len_out) {
        *name_len_out = (uint8_t)len;
    }
    return true;
}

static bool json_schema_type_valid(fof_json_wire_type_t type)
{
    return type >= FOF_JSON_STRING && type <= FOF_JSON_ARRAY;
}

static bool json_schema_string_policy_valid(
    fof_json_wire_type_t type,
    fof_json_string_policy_t string_policy)
{
    if (type == FOF_JSON_STRING || type == FOF_JSON_NULLABLE_STRING) {
        return string_policy == FOF_JSON_STRING_POLICY_PRINTABLE_UTF8 ||
               string_policy ==
                   FOF_JSON_STRING_POLICY_ASCII_TOKEN_NO_ESCAPE;
    }
    return string_policy == FOF_JSON_STRING_POLICY_NONE;
}

static bool json_schema_specs_valid(
    const fof_json_member_spec_t *members,
    size_t member_count,
    uint8_t name_lengths[FOF_JSON_SCHEMA_MAX_MEMBERS])
{
    if (member_count > FOF_JSON_SCHEMA_MAX_MEMBERS ||
        (member_count > 0U && !members)) {
        return false;
    }
    for (size_t i = 0U; i < member_count; ++i) {
        if (!json_schema_type_valid(members[i].type) ||
            !json_schema_string_policy_valid(
                members[i].type, members[i].string_policy) ||
            !json_schema_member_name_length(
                members[i].name, &name_lengths[i])) {
            return false;
        }
        for (size_t prior = 0U; prior < i; ++prior) {
            if (name_lengths[prior] == name_lengths[i] &&
                memcmp(members[prior].name, members[i].name,
                       name_lengths[i]) == 0) {
                return false;
            }
        }
    }
    return true;
}

static bool json_value_matches_type(const fof_json_value_t *value,
                                    fof_json_wire_type_t type)
{
    if (!value) {
        return false;
    }
    switch (type) {
        case FOF_JSON_STRING:
            return value->kind == FOF_JSON_VALUE_STRING;
        case FOF_JSON_NULLABLE_STRING:
            return value->kind == FOF_JSON_VALUE_STRING ||
                   value->kind == FOF_JSON_VALUE_NULL;
        case FOF_JSON_BOOL:
            return value->kind == FOF_JSON_VALUE_BOOL;
        case FOF_JSON_INT32:
            if (value->kind != FOF_JSON_VALUE_NUMBER ||
                !value->number_integer || value->number_overflow) {
                return false;
            }
            if (value->number_negative) {
                return value->number_magnitude <=
                       (uint64_t)INT32_MAX + 1U;
            }
            return value->number_magnitude <= (uint64_t)INT32_MAX;
        case FOF_JSON_INT64:
            if (value->kind != FOF_JSON_VALUE_NUMBER ||
                !value->number_integer || value->number_overflow) {
                return false;
            }
            if (value->number_negative) {
                return value->number_magnitude <=
                       (uint64_t)INT64_MAX + UINT64_C(1);
            }
            return value->number_magnitude <= (uint64_t)INT64_MAX;
        case FOF_JSON_UINT32:
            return value->kind == FOF_JSON_VALUE_NUMBER &&
                   value->number_integer &&
                   !value->number_overflow &&
                   !value->number_negative &&
                   value->number_magnitude <= (uint64_t)UINT32_MAX;
        case FOF_JSON_OBJECT:
            return value->kind == FOF_JSON_VALUE_OBJECT;
        case FOF_JSON_ARRAY:
            return value->kind == FOF_JSON_VALUE_ARRAY;
        default:
            return false;
    }
}

static bool json_string_matches_policy(
    bool ascii_token_eligible,
    fof_json_string_policy_t policy)
{
    if (policy == FOF_JSON_STRING_POLICY_PRINTABLE_UTF8) {
        /* json_parse_string() already proved valid UTF-8 and rejected every
         * raw or decoded C0/DEL control before this visitor runs. */
        return true;
    }
    return policy == FOF_JSON_STRING_POLICY_ASCII_TOKEN_NO_ESCAPE &&
           ascii_token_eligible;
}

static bool json_value_matches_spec(
    const fof_json_value_t *value,
    const fof_json_member_spec_t *spec)
{
    if (!value || !spec || !json_value_matches_type(value, spec->type)) {
        return false;
    }
    if (value->kind != FOF_JSON_VALUE_STRING) {
        return true;
    }
    return json_string_matches_policy(
        value->string_span.ascii_token_eligible,
        spec->string_policy);
}

typedef fof_json_schema_result_t (*json_top_member_visitor_t)(
    const fof_json_string_span_t *key,
    const fof_json_value_t *value,
    void *context);

static fof_json_schema_result_t json_walk_complete_top_object(
    const uint8_t *bytes,
    size_t byte_len,
    json_top_member_visitor_t visitor,
    void *context)
{
    if (!bytes || byte_len == 0U || !visitor) {
        return FOF_JSON_SCHEMA_MALFORMED;
    }
    if (memchr(bytes, '\0', byte_len) != NULL) {
        return FOF_JSON_SCHEMA_EMBEDDED_NUL;
    }

    fof_json_cursor_t cursor = {
        .bytes = bytes,
        .byte_len = byte_len,
        .pos = 0U,
    };
    json_skip_whitespace(&cursor);
    if (!json_consume(&cursor, '{')) {
        return FOF_JSON_SCHEMA_MALFORMED;
    }
    json_skip_whitespace(&cursor);

    if (!json_consume(&cursor, '}')) {
        while (true) {
            fof_json_string_span_t key = {0};
            if (!json_parse_string(&cursor, &key) || key.had_escape) {
                return FOF_JSON_SCHEMA_MALFORMED;
            }

            json_skip_whitespace(&cursor);
            if (!json_consume(&cursor, ':')) {
                return FOF_JSON_SCHEMA_MALFORMED;
            }
            json_skip_whitespace(&cursor);

            fof_json_value_t value = {0};
            if (!json_parse_value(&cursor, 1U, &value)) {
                return FOF_JSON_SCHEMA_MALFORMED;
            }
            fof_json_schema_result_t visit_result =
                visitor(&key, &value, context);
            if (visit_result != FOF_JSON_SCHEMA_OK) {
                return visit_result;
            }

            json_skip_whitespace(&cursor);
            if (json_consume(&cursor, '}')) {
                break;
            }
            if (!json_consume(&cursor, ',')) {
                return FOF_JSON_SCHEMA_MALFORMED;
            }
            json_skip_whitespace(&cursor);
        }
    }

    json_skip_whitespace(&cursor);
    if (cursor.pos != cursor.byte_len) {
        return FOF_JSON_SCHEMA_TRAILING_DATA;
    }
    return FOF_JSON_SCHEMA_OK;
}

typedef struct {
    const fof_json_member_spec_t *members;
    size_t member_count;
    const uint8_t *name_lengths;
    fof_json_value_span_t *values;
    uint64_t seen;
} json_exact_object_context_t;

static fof_json_schema_result_t json_visit_exact_object_member(
    const fof_json_string_span_t *key,
    const fof_json_value_t *value,
    void *opaque_context)
{
    json_exact_object_context_t *context =
        (json_exact_object_context_t *)opaque_context;
    if (!key || !value || !context) {
        return FOF_JSON_SCHEMA_MALFORMED;
    }

    size_t member_index = context->member_count;
    for (size_t i = 0U; i < context->member_count; ++i) {
        if (key->byte_len == context->name_lengths[i] &&
            memcmp(key->bytes, context->members[i].name, key->byte_len) == 0) {
            member_index = i;
            break;
        }
    }
    if (member_index == context->member_count) {
        return FOF_JSON_SCHEMA_UNKNOWN;
    }

    uint64_t member_bit = UINT64_C(1) << member_index;
    if ((context->seen & member_bit) != 0U) {
        return FOF_JSON_SCHEMA_DUPLICATE;
    }
    if (!json_value_matches_spec(
            value, &context->members[member_index])) {
        return FOF_JSON_SCHEMA_WRONG_TYPE;
    }
    if (context->values) {
        context->values[member_index].bytes = value->raw_bytes;
        context->values[member_index].byte_len = value->raw_byte_len;
    }
    context->seen |= member_bit;
    return FOF_JSON_SCHEMA_OK;
}

fof_json_schema_result_t fof_json_validate_exact_object_capture(
    const uint8_t *bytes,
    size_t byte_len,
    const fof_json_member_spec_t *members,
    size_t member_count,
    fof_json_value_span_t *values,
    size_t value_capacity)
{
    if (value_capacity > FOF_JSON_SCHEMA_MAX_MEMBERS ||
        (values == NULL && value_capacity != 0U) ||
        (values != NULL && value_capacity < member_count)) {
        return FOF_JSON_SCHEMA_MALFORMED;
    }
    if (values && value_capacity > 0U) {
        memset(values, 0, value_capacity * sizeof(*values));
    }

    uint8_t name_lengths[FOF_JSON_SCHEMA_MAX_MEMBERS] = {0};
    if (!json_schema_specs_valid(members, member_count, name_lengths)) {
        return FOF_JSON_SCHEMA_MALFORMED;
    }
    json_exact_object_context_t context = {
        .members = members,
        .member_count = member_count,
        .name_lengths = name_lengths,
        .values = values,
        .seen = 0U,
    };
    fof_json_schema_result_t result = json_walk_complete_top_object(
        bytes, byte_len, json_visit_exact_object_member, &context);
    if (result != FOF_JSON_SCHEMA_OK) {
        if (values && value_capacity > 0U) {
            memset(values, 0, value_capacity * sizeof(*values));
        }
        return result;
    }

    uint64_t required = member_count == FOF_JSON_SCHEMA_MAX_MEMBERS
        ? UINT64_MAX
        : ((UINT64_C(1) << member_count) - 1U);
    if (context.seen != required) {
        if (values && value_capacity > 0U) {
            memset(values, 0, value_capacity * sizeof(*values));
        }
        return FOF_JSON_SCHEMA_MISSING;
    }
    return FOF_JSON_SCHEMA_OK;
}

fof_json_schema_result_t fof_json_validate_exact_object(
    const uint8_t *bytes,
    size_t byte_len,
    const fof_json_member_spec_t *members,
    size_t member_count)
{
    return fof_json_validate_exact_object_capture(
        bytes, byte_len, members, member_count, NULL, 0U);
}

static bool json_parse_complete_value_span(
    const fof_json_value_span_t *span,
    fof_json_value_t *value_out)
{
    if (!span || !span->bytes || span->byte_len == 0U || !value_out ||
        memchr(span->bytes, '\0', span->byte_len) != NULL) {
        return false;
    }
    fof_json_cursor_t cursor = {
        .bytes = span->bytes,
        .byte_len = span->byte_len,
        .pos = 0U,
    };
    return json_parse_value(&cursor, 0U, value_out) &&
           cursor.pos == cursor.byte_len;
}

bool fof_json_value_span_parse_bool(
    const fof_json_value_span_t *span,
    bool *value_out)
{
    if (value_out) {
        *value_out = false;
    }
    fof_json_value_t value = {0};
    if (!value_out ||
        !json_parse_complete_value_span(span, &value) ||
        value.kind != FOF_JSON_VALUE_BOOL) {
        return false;
    }
    *value_out = span->byte_len == 4U &&
                 memcmp(span->bytes, "true", 4U) == 0;
    return true;
}

bool fof_json_value_span_parse_int32(
    const fof_json_value_span_t *span,
    int32_t *value_out)
{
    if (value_out) {
        *value_out = 0;
    }
    fof_json_value_t value = {0};
    if (!value_out ||
        !json_parse_complete_value_span(span, &value) ||
        !json_value_matches_type(&value, FOF_JSON_INT32)) {
        return false;
    }
    if (!value.number_negative) {
        *value_out = (int32_t)value.number_magnitude;
    } else if (value.number_magnitude == (uint64_t)INT32_MAX + 1U) {
        *value_out = INT32_MIN;
    } else {
        *value_out = -(int32_t)value.number_magnitude;
    }
    return true;
}

bool fof_json_value_span_parse_uint32(
    const fof_json_value_span_t *span,
    uint32_t *value_out)
{
    if (value_out) {
        *value_out = 0U;
    }
    fof_json_value_t value = {0};
    if (!value_out ||
        !json_parse_complete_value_span(span, &value) ||
        !json_value_matches_type(&value, FOF_JSON_UINT32)) {
        return false;
    }
    *value_out = (uint32_t)value.number_magnitude;
    return true;
}

bool fof_json_value_span_parse_int64(
    const fof_json_value_span_t *span,
    int64_t *value_out)
{
    if (value_out) {
        *value_out = 0;
    }
    fof_json_value_t value = {0};
    if (!value_out ||
        !json_parse_complete_value_span(span, &value) ||
        !json_value_matches_type(&value, FOF_JSON_INT64)) {
        return false;
    }
    if (!value.number_negative) {
        *value_out = (int64_t)value.number_magnitude;
    } else if (value.number_magnitude ==
               (uint64_t)INT64_MAX + UINT64_C(1)) {
        *value_out = INT64_MIN;
    } else {
        *value_out = -(int64_t)value.number_magnitude;
    }
    return true;
}

bool fof_json_value_span_parse_ascii_token(
    const fof_json_value_span_t *span,
    fof_json_value_span_t *token_out)
{
    if (token_out) {
        memset(token_out, 0, sizeof(*token_out));
    }
    fof_json_value_t value = {0};
    if (!token_out ||
        !json_parse_complete_value_span(span, &value) ||
        value.kind != FOF_JSON_VALUE_STRING ||
        value.string_span.byte_len == 0U ||
        !value.string_span.ascii_token_eligible) {
        return false;
    }
    token_out->bytes = value.string_span.bytes;
    token_out->byte_len = value.string_span.byte_len;
    return true;
}

bool fof_json_value_span_parse_nullable_ascii_token(
    const fof_json_value_span_t *span,
    bool *is_null_out,
    fof_json_value_span_t *token_out)
{
    if (is_null_out) {
        *is_null_out = false;
    }
    if (token_out) {
        memset(token_out, 0, sizeof(*token_out));
    }
    if (!is_null_out || !token_out) {
        return false;
    }

    fof_json_value_t value = {0};
    if (!json_parse_complete_value_span(span, &value)) {
        return false;
    }
    if (value.kind == FOF_JSON_VALUE_NULL) {
        *is_null_out = true;
        return true;
    }
    if (value.kind != FOF_JSON_VALUE_STRING ||
        value.string_span.byte_len == 0U ||
        !value.string_span.ascii_token_eligible) {
        return false;
    }
    token_out->bytes = value.string_span.bytes;
    token_out->byte_len = value.string_span.byte_len;
    return true;
}

bool fof_json_value_span_parse_printable_utf8_length(
    const fof_json_value_span_t *span,
    size_t *decoded_byte_len_out)
{
    if (decoded_byte_len_out) {
        *decoded_byte_len_out = 0U;
    }
    fof_json_value_t value = {0};
    if (!decoded_byte_len_out ||
        !json_parse_complete_value_span(span, &value) ||
        value.kind != FOF_JSON_VALUE_STRING) {
        return false;
    }
    *decoded_byte_len_out = value.string_span.decoded_byte_len;
    return true;
}

typedef struct {
    const char *member_name;
    size_t member_name_len;
    size_t match_count;
    fof_json_schema_result_t value_result;
    fof_json_string_span_t value_span;
} json_ascii_token_context_t;

static fof_json_schema_result_t json_visit_ascii_token_member(
    const fof_json_string_span_t *key,
    const fof_json_value_t *value,
    void *opaque_context)
{
    json_ascii_token_context_t *context =
        (json_ascii_token_context_t *)opaque_context;
    if (!key || !value || !context) {
        return FOF_JSON_SCHEMA_MALFORMED;
    }
    if (key->byte_len != context->member_name_len ||
        memcmp(key->bytes, context->member_name, key->byte_len) != 0) {
        return FOF_JSON_SCHEMA_OK;
    }

    context->match_count++;
    if (context->match_count != 1U) {
        return FOF_JSON_SCHEMA_OK;
    }
    if (value->kind != FOF_JSON_VALUE_STRING) {
        context->value_result = FOF_JSON_SCHEMA_WRONG_TYPE;
        return FOF_JSON_SCHEMA_OK;
    }
    if (value->string_span.had_escape) {
        context->value_result = FOF_JSON_SCHEMA_MALFORMED;
        return FOF_JSON_SCHEMA_OK;
    }
    if (value->string_span.byte_len == 0U) {
        context->value_result = FOF_JSON_SCHEMA_WRONG_TYPE;
        return FOF_JSON_SCHEMA_OK;
    }
    for (size_t i = 0U; i < value->string_span.byte_len; ++i) {
        uint8_t byte = value->string_span.bytes[i];
        if (byte < 0x21U || byte > 0x7eU) {
            context->value_result = FOF_JSON_SCHEMA_WRONG_TYPE;
            return FOF_JSON_SCHEMA_OK;
        }
    }

    context->value_span = value->string_span;
    context->value_result = FOF_JSON_SCHEMA_OK;
    return FOF_JSON_SCHEMA_OK;
}

fof_json_schema_result_t fof_json_extract_unique_ascii_token_member(
    const uint8_t *bytes,
    size_t byte_len,
    const char *member_name,
    char *value_out,
    size_t value_capacity,
    size_t *value_len_out)
{
    if (value_out && value_capacity > 0U) {
        value_out[0] = '\0';
    }
    if (value_len_out) {
        *value_len_out = 0U;
    }

    uint8_t member_name_len = 0U;
    if (!value_out || value_capacity == 0U || !value_len_out ||
        !json_schema_member_name_length(member_name, &member_name_len)) {
        return FOF_JSON_SCHEMA_MALFORMED;
    }

    json_ascii_token_context_t context = {
        .member_name = member_name,
        .member_name_len = member_name_len,
        .match_count = 0U,
        .value_result = FOF_JSON_SCHEMA_MISSING,
        .value_span = {0},
    };
    fof_json_schema_result_t result = json_walk_complete_top_object(
        bytes, byte_len, json_visit_ascii_token_member, &context);
    if (result != FOF_JSON_SCHEMA_OK) {
        return result;
    }
    if (context.match_count == 0U) {
        return FOF_JSON_SCHEMA_MISSING;
    }
    if (context.match_count != 1U) {
        return FOF_JSON_SCHEMA_DUPLICATE;
    }
    if (context.value_result != FOF_JSON_SCHEMA_OK) {
        return context.value_result;
    }
    if (context.value_span.byte_len >= value_capacity) {
        return FOF_JSON_SCHEMA_WRONG_TYPE;
    }

    memcpy(value_out, context.value_span.bytes, context.value_span.byte_len);
    value_out[context.value_span.byte_len] = '\0';
    *value_len_out = context.value_span.byte_len;
    return FOF_JSON_SCHEMA_OK;
}
