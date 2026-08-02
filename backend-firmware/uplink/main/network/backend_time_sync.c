#include "backend_time_sync.h"

#include "backend_json_reader.h"
#include "time_sync_policy.h"

bool backend_time_parse_response(
    const char *json,
    size_t length,
    int64_t *out_epoch_ms)
{
    if (!json || !out_epoch_ms || length == 0U || length > 4096U) {
        return false;
    }

    backend_json_token_t tokens[3];
    size_t token_count = 0U;
    if (backend_json_parse(
            json, length, tokens, 3U, &token_count) != BACKEND_JSON_OK ||
        token_count != 3U ||
        tokens[0].kind != BACKEND_JSON_OBJECT ||
        tokens[0].child_count != 2U) {
        return false;
    }

    size_t value_index = 0U;
    int64_t parsed_epoch_ms = 0;
    if (!backend_json_object_find(
            json, tokens, token_count, 0U, "ms", &value_index) ||
        !backend_json_get_i64(
            json, &tokens[value_index], &parsed_epoch_ms) ||
        !fof_time_epoch_is_valid(parsed_epoch_ms)) {
        return false;
    }

    *out_epoch_ms = parsed_epoch_ms;
    return true;
}

backend_time_source_t backend_time_select_source(
    bool sntp_synced,
    int64_t sntp_epoch_ms,
    bool backend_response_valid,
    int64_t backend_epoch_ms,
    int64_t *out_epoch_ms)
{
    if (!out_epoch_ms) {
        return BACKEND_TIME_SOURCE_NONE;
    }
    *out_epoch_ms = 0;
    if (sntp_synced && fof_time_epoch_is_valid(sntp_epoch_ms)) {
        *out_epoch_ms = sntp_epoch_ms;
        return BACKEND_TIME_SOURCE_SNTP;
    }
    if (backend_response_valid &&
        fof_time_epoch_is_valid(backend_epoch_ms)) {
        *out_epoch_ms = backend_epoch_ms;
        return BACKEND_TIME_SOURCE_BACKEND;
    }
    return BACKEND_TIME_SOURCE_NONE;
}
