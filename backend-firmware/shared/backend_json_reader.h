#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_JSON_MAX_TOKENS 256
#define BACKEND_JSON_EXTENDED_MAX_TOKENS 384
#define BACKEND_JSON_MAX_DEPTH 4

typedef enum {
    BACKEND_JSON_OK = 0,
    BACKEND_JSON_MALFORMED,
    BACKEND_JSON_TOO_MANY_TOKENS,
    BACKEND_JSON_TOO_DEEP,
    BACKEND_JSON_DUPLICATE_KEY,
    BACKEND_JSON_RANGE,
} backend_json_result_t;

typedef enum {
    BACKEND_JSON_OBJECT,
    BACKEND_JSON_ARRAY,
    BACKEND_JSON_STRING,
    BACKEND_JSON_NUMBER,
    BACKEND_JSON_BOOL,
    BACKEND_JSON_NULL,
} backend_json_token_kind_t;

typedef struct {
    backend_json_token_kind_t kind;
    int16_t parent;
    uint16_t start;
    uint16_t end;
    uint16_t child_count;
} backend_json_token_t;

backend_json_result_t backend_json_parse(
    const char *json, size_t length,
    backend_json_token_t *tokens, size_t capacity, size_t *out_count);

bool backend_json_object_find(
    const char *json, const backend_json_token_t *tokens, size_t token_count,
    size_t object_index, const char *key, size_t *out_value_index);

bool backend_json_copy_string(
    const char *json, const backend_json_token_t *token,
    char *output, size_t capacity);
bool backend_json_get_bool(
    const char *json, const backend_json_token_t *token, bool *out);
bool backend_json_get_i64(
    const char *json, const backend_json_token_t *token, int64_t *out);
bool backend_json_get_u64(
    const char *json, const backend_json_token_t *token, uint64_t *out);
bool backend_json_get_double(
    const char *json, const backend_json_token_t *token, double *out);

#ifdef __cplusplus
}
#endif
