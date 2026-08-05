#include "backend_ingest_ack.h"

#include <string.h>

#include "backend_json_reader.h"

static bool read_string(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    const char *key,
    char *output,
    size_t capacity)
{
    size_t index = 0U;
    return backend_json_object_find(
               json, tokens, token_count, 0U, key, &index) &&
           backend_json_copy_string(
               json, &tokens[index], output, capacity);
}

static bool read_u32(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    const char *key,
    uint32_t *output)
{
    size_t index = 0U;
    uint64_t value = 0U;
    if (!backend_json_object_find(
            json, tokens, token_count, 0U, key, &index) ||
        !backend_json_get_u64(json, &tokens[index], &value) ||
        value > UINT32_MAX) {
        return false;
    }
    *output = (uint32_t)value;
    return true;
}

bool backend_ingest_ack_validate(
    const char *json,
    size_t length,
    const char *expected_device_id,
    uint16_t expected_item_count)
{
    if (!json || !expected_device_id || length == 0U || length > 4095U) {
        return false;
    }
    /* Root plus exactly six flat key/value pairs. */
    enum { BACKEND_INGEST_ACK_TOKEN_CAPACITY = 13 };
    backend_json_token_t tokens[BACKEND_INGEST_ACK_TOKEN_CAPACITY];
    size_t token_count = 0U;
    if (backend_json_parse(
            json, length, tokens, BACKEND_INGEST_ACK_TOKEN_CAPACITY,
            &token_count) != BACKEND_JSON_OK ||
        token_count == 0U || tokens[0].kind != BACKEND_JSON_OBJECT ||
        tokens[0].child_count != 12U) {
        return false;
    }

    char status[8] = {0};
    char device_id[33] = {0};
    uint32_t accepted = 0U;
    uint32_t processed = 0U;
    uint32_t deduplicated = 0U;
    uint32_t filtered = 0U;
    if (!read_string(json, tokens, token_count, "status",
                     status, sizeof(status)) ||
        !read_u32(json, tokens, token_count, "accepted", &accepted) ||
        !read_u32(json, tokens, token_count, "processed", &processed) ||
        !read_u32(json, tokens, token_count,
                  "deduplicated", &deduplicated) ||
        !read_u32(json, tokens, token_count, "filtered", &filtered) ||
        !read_string(json, tokens, token_count, "device_id",
                     device_id, sizeof(device_id)) ||
        strcmp(status, "ok") != 0 ||
        strcmp(device_id, expected_device_id) != 0 ||
        accepted != expected_item_count) {
        return false;
    }
    const uint64_t total = (uint64_t)processed + deduplicated + filtered;
    return total == accepted;
}
