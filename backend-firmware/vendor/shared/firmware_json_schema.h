#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FOF_JSON_STRING,
    FOF_JSON_NULLABLE_STRING,
    FOF_JSON_BOOL,
    FOF_JSON_INT32,
    FOF_JSON_INT64,
    FOF_JSON_UINT32,
    FOF_JSON_OBJECT,
    FOF_JSON_ARRAY,
} fof_json_wire_type_t;

typedef enum {
    FOF_JSON_STRING_POLICY_NONE = 0,
    FOF_JSON_STRING_POLICY_PRINTABLE_UTF8,
    FOF_JSON_STRING_POLICY_ASCII_TOKEN_NO_ESCAPE,
} fof_json_string_policy_t;

typedef struct {
    const char *name;
    fof_json_wire_type_t type;
    fof_json_string_policy_t string_policy;
} fof_json_member_spec_t;

typedef struct {
    const uint8_t *bytes;
    size_t byte_len;
} fof_json_value_span_t;

typedef enum {
    FOF_JSON_SCHEMA_OK = 0,
    FOF_JSON_SCHEMA_MALFORMED,
    FOF_JSON_SCHEMA_EMBEDDED_NUL,
    FOF_JSON_SCHEMA_TRAILING_DATA,
    FOF_JSON_SCHEMA_DUPLICATE,
    FOF_JSON_SCHEMA_UNKNOWN,
    FOF_JSON_SCHEMA_MISSING,
    FOF_JSON_SCHEMA_WRONG_TYPE,
} fof_json_schema_result_t;

fof_json_schema_result_t fof_json_validate_exact_object(
    const uint8_t *bytes,
    size_t byte_len,
    const fof_json_member_spec_t *members,
    size_t member_count);

fof_json_schema_result_t fof_json_validate_exact_object_capture(
    const uint8_t *bytes,
    size_t byte_len,
    const fof_json_member_spec_t *members,
    size_t member_count,
    fof_json_value_span_t *values,
    size_t value_capacity);

bool fof_json_value_span_parse_bool(
    const fof_json_value_span_t *span,
    bool *value_out);
bool fof_json_value_span_parse_int32(
    const fof_json_value_span_t *span,
    int32_t *value_out);
bool fof_json_value_span_parse_uint32(
    const fof_json_value_span_t *span,
    uint32_t *value_out);
bool fof_json_value_span_parse_int64(
    const fof_json_value_span_t *span,
    int64_t *value_out);
bool fof_json_value_span_parse_ascii_token(
    const fof_json_value_span_t *span,
    fof_json_value_span_t *token_out);
bool fof_json_value_span_parse_nullable_ascii_token(
    const fof_json_value_span_t *span,
    bool *is_null_out,
    fof_json_value_span_t *token_out);
bool fof_json_value_span_parse_printable_utf8_length(
    const fof_json_value_span_t *span,
    size_t *decoded_byte_len_out);

fof_json_schema_result_t fof_json_extract_unique_ascii_token_member(
    const uint8_t *bytes,
    size_t byte_len,
    const char *member_name,
    char *value_out,
    size_t value_capacity,
    size_t *value_len_out);

#ifdef __cplusplus
}
#endif
