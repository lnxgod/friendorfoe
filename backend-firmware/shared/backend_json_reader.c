#include "backend_json_reader.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *json;
    size_t length;
    size_t position;
    backend_json_token_t *tokens;
    size_t capacity;
    size_t count;
    backend_json_result_t result;
} backend_json_parser_t;

static bool is_space(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

static bool is_hex(char value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

static uint32_t hex_value(char value)
{
    if (value >= '0' && value <= '9') {
        return (uint32_t)(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return (uint32_t)(value - 'a' + 10);
    }
    return (uint32_t)(value - 'A' + 10);
}

static bool decode_utf8(const uint8_t *bytes, size_t remaining,
                        uint32_t *codepoint, size_t *consumed)
{
    if (remaining == 0 || bytes == NULL || codepoint == NULL ||
        consumed == NULL) {
        return false;
    }
    const uint8_t first = bytes[0];
    if (first < 0x80U) {
        *codepoint = first;
        *consumed = 1;
        return true;
    }
    if (first >= 0xC2U && first <= 0xDFU && remaining >= 2 &&
        (bytes[1] & 0xC0U) == 0x80U) {
        *codepoint = ((uint32_t)(first & 0x1FU) << 6) |
                     (uint32_t)(bytes[1] & 0x3FU);
        *consumed = 2;
        return true;
    }
    if (first >= 0xE0U && first <= 0xEFU && remaining >= 3 &&
        (bytes[1] & 0xC0U) == 0x80U &&
        (bytes[2] & 0xC0U) == 0x80U &&
        !(first == 0xE0U && bytes[1] < 0xA0U) &&
        !(first == 0xEDU && bytes[1] >= 0xA0U)) {
        *codepoint = ((uint32_t)(first & 0x0FU) << 12) |
                     ((uint32_t)(bytes[1] & 0x3FU) << 6) |
                     (uint32_t)(bytes[2] & 0x3FU);
        *consumed = 3;
        return true;
    }
    if (first >= 0xF0U && first <= 0xF4U && remaining >= 4 &&
        (bytes[1] & 0xC0U) == 0x80U &&
        (bytes[2] & 0xC0U) == 0x80U &&
        (bytes[3] & 0xC0U) == 0x80U &&
        !(first == 0xF0U && bytes[1] < 0x90U) &&
        !(first == 0xF4U && bytes[1] >= 0x90U)) {
        *codepoint = ((uint32_t)(first & 0x07U) << 18) |
                     ((uint32_t)(bytes[1] & 0x3FU) << 12) |
                     ((uint32_t)(bytes[2] & 0x3FU) << 6) |
                     (uint32_t)(bytes[3] & 0x3FU);
        *consumed = 4;
        return true;
    }
    return false;
}

static bool parse_hex_quad(const char *json, size_t length, size_t position,
                           uint32_t *value)
{
    if (position > length || length - position < 4 || value == NULL) {
        return false;
    }
    uint32_t parsed = 0;
    for (size_t index = 0; index < 4; ++index) {
        if (!is_hex(json[position + index])) {
            return false;
        }
        parsed = (parsed << 4) | hex_value(json[position + index]);
    }
    *value = parsed;
    return true;
}

static void skip_space(backend_json_parser_t *parser)
{
    while (parser->position < parser->length &&
           is_space(parser->json[parser->position])) {
        ++parser->position;
    }
}

static bool add_token(backend_json_parser_t *parser,
                      backend_json_token_kind_t kind,
                      int16_t parent, size_t start, size_t end,
                      size_t *out_index)
{
    if (parser->count >= parser->capacity ||
        parser->count >= BACKEND_JSON_MAX_TOKENS) {
        parser->result = BACKEND_JSON_TOO_MANY_TOKENS;
        return false;
    }
    if (start > UINT16_MAX || end > UINT16_MAX) {
        parser->result = BACKEND_JSON_RANGE;
        return false;
    }
    const size_t index = parser->count++;
    parser->tokens[index] = (backend_json_token_t){
        .kind = kind,
        .parent = parent,
        .start = (uint16_t)start,
        .end = (uint16_t)end,
        .child_count = 0,
    };
    if (parent >= 0) {
        if ((size_t)parent >= index ||
            parser->tokens[parent].child_count == UINT16_MAX) {
            parser->result = BACKEND_JSON_RANGE;
            return false;
        }
        ++parser->tokens[parent].child_count;
    }
    if (out_index != NULL) {
        *out_index = index;
    }
    return true;
}

static bool string_next_codepoint(const char *json,
                                  const backend_json_token_t *token,
                                  size_t *position, uint32_t *codepoint)
{
    if (json == NULL || token == NULL || position == NULL ||
        codepoint == NULL || token->kind != BACKEND_JSON_STRING ||
        *position < token->start || *position >= token->end) {
        return false;
    }
    size_t cursor = *position;
    if (json[cursor] != '\\') {
        size_t consumed = 0;
        if (!decode_utf8((const uint8_t *)json + cursor,
                         (size_t)token->end - cursor,
                         codepoint, &consumed)) {
            return false;
        }
        *position = cursor + consumed;
        return true;
    }

    ++cursor;
    if (cursor >= token->end) {
        return false;
    }
    const char escaped = json[cursor++];
    switch (escaped) {
    case '"': *codepoint = '"'; break;
    case '\\': *codepoint = '\\'; break;
    case '/': *codepoint = '/'; break;
    case 'b': *codepoint = '\b'; break;
    case 'f': *codepoint = '\f'; break;
    case 'n': *codepoint = '\n'; break;
    case 'r': *codepoint = '\r'; break;
    case 't': *codepoint = '\t'; break;
    case 'u': {
        uint32_t first = 0;
        if (!parse_hex_quad(json, token->end, cursor, &first)) {
            return false;
        }
        cursor += 4;
        if (first >= 0xD800U && first <= 0xDBFFU) {
            if (cursor + 6 > token->end || json[cursor] != '\\' ||
                json[cursor + 1] != 'u') {
                return false;
            }
            uint32_t second = 0;
            if (!parse_hex_quad(json, token->end, cursor + 2, &second) ||
                second < 0xDC00U || second > 0xDFFFU) {
                return false;
            }
            *codepoint = 0x10000U + ((first - 0xD800U) << 10) +
                         (second - 0xDC00U);
            cursor += 6;
        } else {
            if (first >= 0xDC00U && first <= 0xDFFFU) {
                return false;
            }
            *codepoint = first;
        }
        break;
    }
    default:
        return false;
    }
    *position = cursor;
    return true;
}

static bool strings_equal(const char *json,
                          const backend_json_token_t *left,
                          const backend_json_token_t *right)
{
    size_t left_position = left->start;
    size_t right_position = right->start;
    while (left_position < left->end && right_position < right->end) {
        uint32_t left_codepoint = 0;
        uint32_t right_codepoint = 0;
        if (!string_next_codepoint(
                json, left, &left_position, &left_codepoint) ||
            !string_next_codepoint(
                json, right, &right_position, &right_codepoint) ||
            left_codepoint != right_codepoint) {
            return false;
        }
    }
    return left_position == left->end && right_position == right->end;
}

static bool parse_value(backend_json_parser_t *parser,
                        int16_t parent, unsigned depth);

static bool parse_string_token(backend_json_parser_t *parser,
                               int16_t parent, size_t *out_index)
{
    if (parser->position >= parser->length ||
        parser->json[parser->position] != '"') {
        parser->result = BACKEND_JSON_MALFORMED;
        return false;
    }
    const size_t start = ++parser->position;
    while (parser->position < parser->length) {
        const uint8_t byte = (uint8_t)parser->json[parser->position];
        if (byte == '"') {
            const size_t end = parser->position++;
            return add_token(parser, BACKEND_JSON_STRING, parent,
                             start, end, out_index);
        }
        if (byte < 0x20U) {
            parser->result = BACKEND_JSON_MALFORMED;
            return false;
        }
        if (byte == '\\') {
            ++parser->position;
            if (parser->position >= parser->length) {
                parser->result = BACKEND_JSON_MALFORMED;
                return false;
            }
            const char escaped = parser->json[parser->position++];
            if (strchr("\"\\/bfnrt", escaped) != NULL) {
                continue;
            }
            if (escaped != 'u') {
                parser->result = BACKEND_JSON_MALFORMED;
                return false;
            }
            uint32_t first = 0;
            if (!parse_hex_quad(parser->json, parser->length,
                                parser->position, &first)) {
                parser->result = BACKEND_JSON_MALFORMED;
                return false;
            }
            parser->position += 4;
            if (first == 0) {
                parser->result = BACKEND_JSON_MALFORMED;
                return false;
            }
            if (first >= 0xD800U && first <= 0xDBFFU) {
                if (parser->position + 6 > parser->length ||
                    parser->json[parser->position] != '\\' ||
                    parser->json[parser->position + 1] != 'u') {
                    parser->result = BACKEND_JSON_MALFORMED;
                    return false;
                }
                uint32_t second = 0;
                if (!parse_hex_quad(parser->json, parser->length,
                                    parser->position + 2, &second) ||
                    second < 0xDC00U || second > 0xDFFFU) {
                    parser->result = BACKEND_JSON_MALFORMED;
                    return false;
                }
                parser->position += 6;
            } else if (first >= 0xDC00U && first <= 0xDFFFU) {
                parser->result = BACKEND_JSON_MALFORMED;
                return false;
            }
            continue;
        }
        size_t consumed = 0;
        uint32_t codepoint = 0;
        if (!decode_utf8((const uint8_t *)parser->json + parser->position,
                         parser->length - parser->position,
                         &codepoint, &consumed)) {
            parser->result = BACKEND_JSON_MALFORMED;
            return false;
        }
        parser->position += consumed;
    }
    parser->result = BACKEND_JSON_MALFORMED;
    return false;
}

static bool object_has_duplicate_key(backend_json_parser_t *parser,
                                     size_t object_index,
                                     size_t candidate_index)
{
    size_t direct_child = 0;
    for (size_t index = object_index + 1; index < candidate_index; ++index) {
        if (parser->tokens[index].parent != (int16_t)object_index) {
            continue;
        }
        if ((direct_child & 1U) == 0U &&
            parser->tokens[index].kind == BACKEND_JSON_STRING &&
            strings_equal(parser->json, &parser->tokens[index],
                          &parser->tokens[candidate_index])) {
            return true;
        }
        ++direct_child;
    }
    return false;
}

static bool parse_object(backend_json_parser_t *parser,
                         int16_t parent, unsigned depth)
{
    if (depth > BACKEND_JSON_MAX_DEPTH) {
        parser->result = BACKEND_JSON_TOO_DEEP;
        return false;
    }
    const size_t start = parser->position++;
    size_t object_index = 0;
    if (!add_token(parser, BACKEND_JSON_OBJECT, parent,
                   start, start, &object_index)) {
        return false;
    }
    skip_space(parser);
    if (parser->position < parser->length &&
        parser->json[parser->position] == '}') {
        parser->tokens[object_index].end = (uint16_t)++parser->position;
        return true;
    }
    while (parser->position < parser->length) {
        size_t key_index = 0;
        if (!parse_string_token(parser, (int16_t)object_index, &key_index)) {
            return false;
        }
        if (object_has_duplicate_key(parser, object_index, key_index)) {
            parser->result = BACKEND_JSON_DUPLICATE_KEY;
            return false;
        }
        skip_space(parser);
        if (parser->position >= parser->length ||
            parser->json[parser->position++] != ':') {
            parser->result = BACKEND_JSON_MALFORMED;
            return false;
        }
        skip_space(parser);
        if (!parse_value(parser, (int16_t)object_index, depth + 1U)) {
            return false;
        }
        skip_space(parser);
        if (parser->position >= parser->length) {
            parser->result = BACKEND_JSON_MALFORMED;
            return false;
        }
        const char delimiter = parser->json[parser->position++];
        if (delimiter == '}') {
            parser->tokens[object_index].end =
                (uint16_t)parser->position;
            return true;
        }
        if (delimiter != ',') {
            parser->result = BACKEND_JSON_MALFORMED;
            return false;
        }
        skip_space(parser);
    }
    parser->result = BACKEND_JSON_MALFORMED;
    return false;
}

static bool parse_array(backend_json_parser_t *parser,
                        int16_t parent, unsigned depth)
{
    if (depth > BACKEND_JSON_MAX_DEPTH) {
        parser->result = BACKEND_JSON_TOO_DEEP;
        return false;
    }
    const size_t start = parser->position++;
    size_t array_index = 0;
    if (!add_token(parser, BACKEND_JSON_ARRAY, parent,
                   start, start, &array_index)) {
        return false;
    }
    skip_space(parser);
    if (parser->position < parser->length &&
        parser->json[parser->position] == ']') {
        parser->tokens[array_index].end = (uint16_t)++parser->position;
        return true;
    }
    while (parser->position < parser->length) {
        if (!parse_value(parser, (int16_t)array_index, depth + 1U)) {
            return false;
        }
        skip_space(parser);
        if (parser->position >= parser->length) {
            parser->result = BACKEND_JSON_MALFORMED;
            return false;
        }
        const char delimiter = parser->json[parser->position++];
        if (delimiter == ']') {
            parser->tokens[array_index].end = (uint16_t)parser->position;
            return true;
        }
        if (delimiter != ',') {
            parser->result = BACKEND_JSON_MALFORMED;
            return false;
        }
        skip_space(parser);
    }
    parser->result = BACKEND_JSON_MALFORMED;
    return false;
}

static bool parse_number(backend_json_parser_t *parser, int16_t parent)
{
    const size_t start = parser->position;
    bool integer_literal = true;
    if (parser->json[parser->position] == '-') {
        ++parser->position;
        if (parser->position >= parser->length) {
            parser->result = BACKEND_JSON_MALFORMED;
            return false;
        }
    }
    if (parser->json[parser->position] == '0') {
        ++parser->position;
        if (parser->position < parser->length &&
            parser->json[parser->position] >= '0' &&
            parser->json[parser->position] <= '9') {
            parser->result = BACKEND_JSON_MALFORMED;
            return false;
        }
    } else if (parser->json[parser->position] >= '1' &&
               parser->json[parser->position] <= '9') {
        do {
            ++parser->position;
        } while (parser->position < parser->length &&
                 parser->json[parser->position] >= '0' &&
                 parser->json[parser->position] <= '9');
    } else {
        parser->result = BACKEND_JSON_MALFORMED;
        return false;
    }
    if (parser->position < parser->length &&
        parser->json[parser->position] == '.') {
        integer_literal = false;
        ++parser->position;
        const size_t fraction_start = parser->position;
        while (parser->position < parser->length &&
               parser->json[parser->position] >= '0' &&
               parser->json[parser->position] <= '9') {
            ++parser->position;
        }
        if (parser->position == fraction_start) {
            parser->result = BACKEND_JSON_MALFORMED;
            return false;
        }
    }
    if (parser->position < parser->length &&
        (parser->json[parser->position] == 'e' ||
         parser->json[parser->position] == 'E')) {
        integer_literal = false;
        ++parser->position;
        if (parser->position < parser->length &&
            (parser->json[parser->position] == '+' ||
             parser->json[parser->position] == '-')) {
            ++parser->position;
        }
        const size_t exponent_start = parser->position;
        while (parser->position < parser->length &&
               parser->json[parser->position] >= '0' &&
               parser->json[parser->position] <= '9') {
            ++parser->position;
        }
        if (parser->position == exponent_start) {
            parser->result = BACKEND_JSON_MALFORMED;
            return false;
        }
    }

    if (integer_literal) {
        size_t index = start;
        const bool negative = parser->json[index] == '-';
        if (negative) {
            ++index;
        }
        const uint64_t limit = negative ?
            (uint64_t)INT64_MAX + 1U : UINT64_MAX;
        uint64_t magnitude = 0;
        for (; index < parser->position; ++index) {
            const uint64_t digit =
                (uint64_t)(parser->json[index] - '0');
            if (magnitude > (limit - digit) / 10U) {
                parser->result = BACKEND_JSON_RANGE;
                return false;
            }
            magnitude = magnitude * 10U + digit;
        }
    }

    const size_t number_length = parser->position - start;
    if (number_length >= 96) {
        parser->result = BACKEND_JSON_RANGE;
        return false;
    }
    char number[96];
    memcpy(number, parser->json + start, number_length);
    number[number_length] = '\0';
    errno = 0;
    char *end = NULL;
    const double value = strtod(number, &end);
    if (end != number + number_length || errno == ERANGE ||
        !isfinite(value)) {
        parser->result = BACKEND_JSON_RANGE;
        return false;
    }
    return add_token(parser, BACKEND_JSON_NUMBER, parent,
                     start, parser->position, NULL);
}

static bool parse_literal(backend_json_parser_t *parser, int16_t parent,
                          const char *literal,
                          backend_json_token_kind_t kind)
{
    const size_t literal_length = strlen(literal);
    if (parser->position > parser->length ||
        parser->length - parser->position < literal_length ||
        memcmp(parser->json + parser->position,
               literal, literal_length) != 0) {
        parser->result = BACKEND_JSON_MALFORMED;
        return false;
    }
    const size_t start = parser->position;
    parser->position += literal_length;
    return add_token(parser, kind, parent,
                     start, parser->position, NULL);
}

static bool parse_value(backend_json_parser_t *parser,
                        int16_t parent, unsigned depth)
{
    if (parser->position >= parser->length) {
        parser->result = BACKEND_JSON_MALFORMED;
        return false;
    }
    switch (parser->json[parser->position]) {
    case '{': return parse_object(parser, parent, depth);
    case '[': return parse_array(parser, parent, depth);
    case '"': return parse_string_token(parser, parent, NULL);
    case 't': return parse_literal(parser, parent, "true", BACKEND_JSON_BOOL);
    case 'f': return parse_literal(parser, parent, "false", BACKEND_JSON_BOOL);
    case 'n': return parse_literal(parser, parent, "null", BACKEND_JSON_NULL);
    default:
        if (parser->json[parser->position] == '-' ||
            (parser->json[parser->position] >= '0' &&
             parser->json[parser->position] <= '9')) {
            return parse_number(parser, parent);
        }
        parser->result = BACKEND_JSON_MALFORMED;
        return false;
    }
}

backend_json_result_t backend_json_parse(
    const char *json, size_t length,
    backend_json_token_t *tokens, size_t capacity, size_t *out_count)
{
    if (out_count != NULL) {
        *out_count = 0;
    }
    if (json == NULL || tokens == NULL || out_count == NULL ||
        capacity == 0 || length == 0 || length > UINT16_MAX ||
        memchr(json, '\0', length) != NULL) {
        return length > UINT16_MAX ? BACKEND_JSON_RANGE :
                                    BACKEND_JSON_MALFORMED;
    }
    backend_json_parser_t parser = {
        .json = json,
        .length = length,
        .tokens = tokens,
        .capacity = capacity,
        .result = BACKEND_JSON_OK,
    };
    skip_space(&parser);
    if (!parse_value(&parser, -1, 1)) {
        return parser.result;
    }
    skip_space(&parser);
    if (parser.position != length) {
        return BACKEND_JSON_MALFORMED;
    }
    *out_count = parser.count;
    return BACKEND_JSON_OK;
}

static bool string_equals_cstr(const char *json,
                               const backend_json_token_t *token,
                               const char *value)
{
    size_t token_position = token->start;
    const uint8_t *value_bytes = (const uint8_t *)value;
    size_t value_remaining = strlen(value);
    while (token_position < token->end && value_remaining > 0) {
        uint32_t token_codepoint = 0;
        uint32_t value_codepoint = 0;
        size_t consumed = 0;
        if (!string_next_codepoint(
                json, token, &token_position, &token_codepoint) ||
            !decode_utf8(value_bytes, value_remaining,
                         &value_codepoint, &consumed) ||
            token_codepoint != value_codepoint) {
            return false;
        }
        value_bytes += consumed;
        value_remaining -= consumed;
    }
    return token_position == token->end && value_remaining == 0;
}

bool backend_json_object_find(
    const char *json, const backend_json_token_t *tokens, size_t token_count,
    size_t object_index, const char *key, size_t *out_value_index)
{
    if (json == NULL || tokens == NULL || key == NULL ||
        out_value_index == NULL || object_index >= token_count ||
        tokens[object_index].kind != BACKEND_JSON_OBJECT) {
        return false;
    }
    bool expecting_key = true;
    bool matched = false;
    for (size_t index = object_index + 1; index < token_count; ++index) {
        if (tokens[index].parent != (int16_t)object_index) {
            continue;
        }
        if (expecting_key) {
            if (tokens[index].kind != BACKEND_JSON_STRING) {
                return false;
            }
            matched = string_equals_cstr(json, &tokens[index], key);
        } else if (matched) {
            *out_value_index = index;
            return true;
        }
        expecting_key = !expecting_key;
    }
    return false;
}

static size_t encode_utf8(uint32_t codepoint, char output[4])
{
    if (codepoint <= 0x7FU) {
        output[0] = (char)codepoint;
        return 1;
    }
    if (codepoint <= 0x7FFU) {
        output[0] = (char)(0xC0U | (codepoint >> 6));
        output[1] = (char)(0x80U | (codepoint & 0x3FU));
        return 2;
    }
    if (codepoint <= 0xFFFFU) {
        output[0] = (char)(0xE0U | (codepoint >> 12));
        output[1] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
        output[2] = (char)(0x80U | (codepoint & 0x3FU));
        return 3;
    }
    if (codepoint <= 0x10FFFFU) {
        output[0] = (char)(0xF0U | (codepoint >> 18));
        output[1] = (char)(0x80U | ((codepoint >> 12) & 0x3FU));
        output[2] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
        output[3] = (char)(0x80U | (codepoint & 0x3FU));
        return 4;
    }
    return 0;
}

bool backend_json_copy_string(
    const char *json, const backend_json_token_t *token,
    char *output, size_t capacity)
{
    if (output != NULL && capacity > 0) {
        output[0] = '\0';
    }
    if (json == NULL || token == NULL || output == NULL || capacity == 0 ||
        token->kind != BACKEND_JSON_STRING) {
        return false;
    }
    size_t input_position = token->start;
    size_t output_length = 0;
    while (input_position < token->end) {
        uint32_t codepoint = 0;
        char encoded[4];
        if (!string_next_codepoint(
                json, token, &input_position, &codepoint) ||
            codepoint == 0) {
            output[0] = '\0';
            return false;
        }
        const size_t encoded_length = encode_utf8(codepoint, encoded);
        if (encoded_length == 0 || output_length >= capacity ||
            encoded_length > capacity - output_length - 1U) {
            output[0] = '\0';
            return false;
        }
        memcpy(output + output_length, encoded, encoded_length);
        output_length += encoded_length;
    }
    output[output_length] = '\0';
    return true;
}

bool backend_json_get_bool(
    const char *json, const backend_json_token_t *token, bool *out)
{
    if (json == NULL || token == NULL || out == NULL ||
        token->kind != BACKEND_JSON_BOOL) {
        return false;
    }
    const size_t length = (size_t)token->end - token->start;
    if (length == 4 && memcmp(json + token->start, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (length == 5 && memcmp(json + token->start, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool unsigned_magnitude(const char *json,
                               const backend_json_token_t *token,
                               size_t start, uint64_t limit,
                               uint64_t *out)
{
    if (start >= token->end) {
        return false;
    }
    uint64_t value = 0;
    for (size_t index = start; index < token->end; ++index) {
        const char digit_char = json[index];
        if (digit_char < '0' || digit_char > '9') {
            return false;
        }
        const uint64_t digit = (uint64_t)(digit_char - '0');
        if (value > (limit - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    *out = value;
    return true;
}

bool backend_json_get_i64(
    const char *json, const backend_json_token_t *token, int64_t *out)
{
    if (json == NULL || token == NULL || out == NULL ||
        token->kind != BACKEND_JSON_NUMBER || token->start >= token->end) {
        return false;
    }
    const bool negative = json[token->start] == '-';
    const size_t start = token->start + (negative ? 1U : 0U);
    const uint64_t limit = negative ?
        (uint64_t)INT64_MAX + 1U : (uint64_t)INT64_MAX;
    uint64_t magnitude = 0;
    if (!unsigned_magnitude(json, token, start, limit, &magnitude)) {
        return false;
    }
    if (negative) {
        *out = magnitude == (uint64_t)INT64_MAX + 1U ?
            INT64_MIN : -(int64_t)magnitude;
    } else {
        *out = (int64_t)magnitude;
    }
    return true;
}

bool backend_json_get_u64(
    const char *json, const backend_json_token_t *token, uint64_t *out)
{
    if (json == NULL || token == NULL || out == NULL ||
        token->kind != BACKEND_JSON_NUMBER || token->start >= token->end ||
        json[token->start] == '-') {
        return false;
    }
    return unsigned_magnitude(
        json, token, token->start, UINT64_MAX, out);
}

bool backend_json_get_double(
    const char *json, const backend_json_token_t *token, double *out)
{
    if (json == NULL || token == NULL || out == NULL ||
        token->kind != BACKEND_JSON_NUMBER || token->start >= token->end) {
        return false;
    }
    const size_t length = (size_t)token->end - token->start;
    if (length >= 96) {
        return false;
    }
    char number[96];
    memcpy(number, json + token->start, length);
    number[length] = '\0';
    errno = 0;
    char *end = NULL;
    const double value = strtod(number, &end);
    if (end != number + length || errno == ERANGE || !isfinite(value)) {
        return false;
    }
    *out = value;
    return true;
}
