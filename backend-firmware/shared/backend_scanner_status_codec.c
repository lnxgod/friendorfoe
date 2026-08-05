#include "backend_scanner_status_codec.h"

#include <inttypes.h>
#include <string.h>

#include "backend_identity.h"
#include "backend_json_reader.h"
#include "backend_json_writer.h"

static bool terminated(const char *value, size_t capacity)
{
    return value && memchr(value, '\0', capacity) != NULL;
}

static bool hex_char(char value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

static bool valid_mac(const char *value)
{
    if (!value || strlen(value) != 17U) {
        return false;
    }
    for (size_t i = 0; i < 17U; ++i) {
        if ((i + 1U) % 3U == 0U) {
            if (value[i] != ':') {
                return false;
            }
        } else if (!hex_char(value[i])) {
            return false;
        }
    }
    return true;
}

static const char *profile_name(backend_scan_profile_t profile)
{
    switch (profile) {
    case BACKEND_SCAN_PROFILE_QUIESCENT:
        return "quiescent";
    case BACKEND_SCAN_PROFILE_BLE_PRIMARY:
        return "ble_primary";
    case BACKEND_SCAN_PROFILE_WIFI_PRIMARY:
        return "wifi_primary";
    case BACKEND_SCAN_PROFILE_HYBRID_FAILOVER:
        return "hybrid_failover";
    default:
        return NULL;
    }
}

static bool parse_profile(const char *value, backend_scan_profile_t *out)
{
    if (strcmp(value, "quiescent") == 0) {
        *out = BACKEND_SCAN_PROFILE_QUIESCENT;
    } else if (strcmp(value, "ble_primary") == 0) {
        *out = BACKEND_SCAN_PROFILE_BLE_PRIMARY;
    } else if (strcmp(value, "wifi_primary") == 0) {
        *out = BACKEND_SCAN_PROFILE_WIFI_PRIMARY;
    } else if (strcmp(value, "hybrid_failover") == 0) {
        *out = BACKEND_SCAN_PROFILE_HYBRID_FAILOVER;
    } else {
        return false;
    }
    return true;
}

static bool valid_status(const backend_scanner_status_t *status)
{
    if (!status || status->schema != BACKEND_SCANNER_STATUS_SCHEMA ||
        status->sequence == 0U || status->boot_id == 0U ||
        !terminated(status->mac, sizeof(status->mac)) ||
        !valid_mac(status->mac) ||
        !terminated(status->target, sizeof(status->target)) ||
        !terminated(status->project, sizeof(status->project)) ||
        !terminated(status->hardware, sizeof(status->hardware)) ||
        !terminated(status->version, sizeof(status->version)) ||
        status->version[0] == '\0' || profile_name(status->profile) == NULL ||
        !terminated(status->ota_state, sizeof(status->ota_state)) ||
        status->ota_state[0] == '\0' ||
        !terminated(status->rollback_state,
                    sizeof(status->rollback_state)) ||
        status->rollback_state[0] == '\0') {
        return false;
    }
    const backend_firmware_identity_t *identity =
        backend_identity_for_image(BACKEND_IMAGE_SCANNER);
    return backend_identity_matches(identity, status->target,
                                    status->project, status->hardware);
}

static void append_key(backend_json_writer_t *writer, const char *key)
{
    backend_json_append_format(writer, ",\"%s\":", key);
}

static void append_string(backend_json_writer_t *writer,
                          const char *key,
                          const char *value)
{
    append_key(writer, key);
    backend_json_append_escaped(writer, value);
}

static void append_u32(backend_json_writer_t *writer,
                       const char *key,
                       uint32_t value)
{
    append_key(writer, key);
    backend_json_append_format(writer, "%" PRIu32, value);
}

static void append_bool(backend_json_writer_t *writer,
                        const char *key,
                        bool value)
{
    append_key(writer, key);
    backend_json_append(writer, value ? "true" : "false");
}

size_t backend_scanner_status_encode(
    const backend_scanner_status_t *status,
    char *output,
    size_t capacity)
{
    if (output && capacity > 0U) {
        output[0] = '\0';
    }
    if (!output || capacity == 0U || !valid_status(status)) {
        return 0U;
    }
    size_t bounded = capacity;
    if (bounded > BACKEND_SCANNER_STATUS_MAX_LINE + 1U) {
        bounded = BACKEND_SCANNER_STATUS_MAX_LINE + 1U;
    }
    backend_json_writer_t writer;
    backend_json_writer_init(&writer, output, bounded);
    backend_json_append(&writer, "{\"type\":\"scanner_status\"");
    append_u32(&writer, "schema", status->schema);
    append_u32(&writer, "sequence", status->sequence);
    append_u32(&writer, "boot_id", status->boot_id);
    append_string(&writer, "mac", status->mac);
    append_string(&writer, "target", status->target);
    append_string(&writer, "project", status->project);
    append_string(&writer, "hardware", status->hardware);
    append_string(&writer, "version", status->version);
    append_string(&writer, "profile", profile_name(status->profile));
    append_u32(&writer, "role_generation", status->role_generation);
    append_bool(&writer, "role_acked", status->role_acked);
    append_bool(&writer, "command_ingress", status->command_ingress);
    append_bool(&writer, "ble_healthy", status->ble_healthy);
    append_bool(&writer, "wifi_healthy", status->wifi_healthy);
    append_bool(&writer, "flow_paused", status->flow_paused);
    append_string(&writer, "ota_state", status->ota_state);
    append_string(&writer, "rollback_state", status->rollback_state);
    append_u32(&writer, "rx_errors", status->rx_errors);
    append_u32(&writer, "tx_drops", status->tx_drops);
    append_key(&writer, "uptime_ms");
    backend_json_append_format(&writer, "%" PRIu64, status->uptime_ms);
    backend_json_append(&writer, "}");
    return backend_json_writer_finish(&writer);
}

static bool find_required(const char *json,
                          const backend_json_token_t *tokens,
                          size_t token_count,
                          const char *key,
                          size_t *out)
{
    return backend_json_object_find(
        json, tokens, token_count, 0U, key, out);
}

static bool read_string(const char *json,
                        const backend_json_token_t *tokens,
                        size_t token_count,
                        const char *key,
                        char *output,
                        size_t capacity)
{
    size_t index = 0;
    return find_required(json, tokens, token_count, key, &index) &&
           backend_json_copy_string(json, &tokens[index], output, capacity);
}

static bool read_u64(const char *json,
                     const backend_json_token_t *tokens,
                     size_t token_count,
                     const char *key,
                     uint64_t *output)
{
    size_t index = 0;
    return find_required(json, tokens, token_count, key, &index) &&
           backend_json_get_u64(json, &tokens[index], output);
}

static bool read_u32(const char *json,
                     const backend_json_token_t *tokens,
                     size_t token_count,
                     const char *key,
                     uint32_t *output)
{
    uint64_t value = 0;
    if (!read_u64(json, tokens, token_count, key, &value) ||
        value > UINT32_MAX) {
        return false;
    }
    *output = (uint32_t)value;
    return true;
}

static bool read_bool(const char *json,
                      const backend_json_token_t *tokens,
                      size_t token_count,
                      const char *key,
                      bool *output)
{
    size_t index = 0;
    return find_required(json, tokens, token_count, key, &index) &&
           backend_json_get_bool(json, &tokens[index], output);
}

backend_scanner_status_decode_result_t backend_scanner_status_decode(
    const char *json,
    size_t length,
    backend_scanner_status_t *out)
{
    if (length > BACKEND_SCANNER_STATUS_MAX_LINE) {
        return BACKEND_SCANNER_STATUS_TOO_LARGE;
    }
    if (!json || !out || length == 0U) {
        return BACKEND_SCANNER_STATUS_MALFORMED;
    }
    backend_json_token_t tokens[BACKEND_JSON_MAX_TOKENS];
    size_t token_count = 0;
    if (backend_json_parse(json, length, tokens, BACKEND_JSON_MAX_TOKENS,
                           &token_count) != BACKEND_JSON_OK ||
        token_count == 0U || tokens[0].kind != BACKEND_JSON_OBJECT) {
        return BACKEND_SCANNER_STATUS_MALFORMED;
    }

    backend_scanner_status_t status = {0};
    char type[24];
    char profile[24];
    uint32_t schema = 0;
    uint64_t uptime = 0;
    if (!read_string(json, tokens, token_count, "type", type,
                     sizeof(type)) || strcmp(type, "scanner_status") != 0 ||
        !read_u32(json, tokens, token_count, "schema", &schema) ||
        schema > UINT8_MAX ||
        !read_u32(json, tokens, token_count, "sequence", &status.sequence) ||
        !read_u32(json, tokens, token_count, "boot_id", &status.boot_id) ||
        !read_string(json, tokens, token_count, "mac", status.mac,
                     sizeof(status.mac)) ||
        !read_string(json, tokens, token_count, "target", status.target,
                     sizeof(status.target)) ||
        !read_string(json, tokens, token_count, "project", status.project,
                     sizeof(status.project)) ||
        !read_string(json, tokens, token_count, "hardware", status.hardware,
                     sizeof(status.hardware)) ||
        !read_string(json, tokens, token_count, "version", status.version,
                     sizeof(status.version)) ||
        !read_string(json, tokens, token_count, "profile", profile,
                     sizeof(profile)) ||
        !parse_profile(profile, &status.profile) ||
        !read_u32(json, tokens, token_count, "role_generation",
                  &status.role_generation) ||
        !read_bool(json, tokens, token_count, "role_acked",
                   &status.role_acked) ||
        !read_bool(json, tokens, token_count, "command_ingress",
                   &status.command_ingress) ||
        !read_bool(json, tokens, token_count, "ble_healthy",
                   &status.ble_healthy) ||
        !read_bool(json, tokens, token_count, "wifi_healthy",
                   &status.wifi_healthy) ||
        !read_bool(json, tokens, token_count, "flow_paused",
                   &status.flow_paused) ||
        !read_string(json, tokens, token_count, "ota_state", status.ota_state,
                     sizeof(status.ota_state)) ||
        !read_string(json, tokens, token_count, "rollback_state",
                     status.rollback_state,
                     sizeof(status.rollback_state)) ||
        !read_u32(json, tokens, token_count, "rx_errors",
                  &status.rx_errors) ||
        !read_u32(json, tokens, token_count, "tx_drops",
                  &status.tx_drops) ||
        !read_u64(json, tokens, token_count, "uptime_ms", &uptime)) {
        return BACKEND_SCANNER_STATUS_SCHEMA_MISMATCH;
    }
    status.schema = (uint8_t)schema;
    status.uptime_ms = uptime;
    const size_t expected_fields = 21U;
    if (tokens[0].child_count != expected_fields * 2U ||
        !valid_status(&status)) {
        return BACKEND_SCANNER_STATUS_SCHEMA_MISMATCH;
    }
    *out = status;
    return BACKEND_SCANNER_STATUS_DECODE_OK;
}

static bool status_equal(const backend_scanner_status_t *left,
                         const backend_scanner_status_t *right)
{
    return left->schema == right->schema &&
           left->sequence == right->sequence &&
           left->boot_id == right->boot_id &&
           strcmp(left->mac, right->mac) == 0 &&
           strcmp(left->target, right->target) == 0 &&
           strcmp(left->project, right->project) == 0 &&
           strcmp(left->hardware, right->hardware) == 0 &&
           strcmp(left->version, right->version) == 0 &&
           left->profile == right->profile &&
           left->role_generation == right->role_generation &&
           left->role_acked == right->role_acked &&
           left->command_ingress == right->command_ingress &&
           left->ble_healthy == right->ble_healthy &&
           left->wifi_healthy == right->wifi_healthy &&
           left->flow_paused == right->flow_paused &&
           strcmp(left->ota_state, right->ota_state) == 0 &&
           strcmp(left->rollback_state, right->rollback_state) == 0 &&
           left->rx_errors == right->rx_errors &&
           left->tx_drops == right->tx_drops &&
           left->uptime_ms == right->uptime_ms;
}

void backend_scanner_status_tracker_init(
    backend_scanner_status_tracker_t *tracker)
{
    if (tracker) {
        memset(tracker, 0, sizeof(*tracker));
    }
}

backend_scanner_status_accept_result_t backend_scanner_status_tracker_accept(
    backend_scanner_status_tracker_t *tracker,
    const backend_scanner_status_t *status)
{
    if (!tracker || !valid_status(status)) {
        return BACKEND_SCANNER_STATUS_INVALID;
    }
    if (!tracker->initialized) {
        tracker->initialized = true;
        tracker->boot_id = status->boot_id;
        tracker->sequence = status->sequence;
        tracker->status = *status;
        return BACKEND_SCANNER_STATUS_ACCEPTED;
    }
    if (status->boot_id != tracker->boot_id) {
        tracker->boot_id = status->boot_id;
        tracker->sequence = status->sequence;
        tracker->status = *status;
        return BACKEND_SCANNER_STATUS_CHANGED_BOOT;
    }
    if (status->sequence < tracker->sequence) {
        return BACKEND_SCANNER_STATUS_STALE;
    }
    if (status->sequence == tracker->sequence) {
        return status_equal(status, &tracker->status)
            ? BACKEND_SCANNER_STATUS_REFRESHED
            : BACKEND_SCANNER_STATUS_CONFLICT;
    }
    tracker->sequence = status->sequence;
    tracker->status = *status;
    return BACKEND_SCANNER_STATUS_ACCEPTED;
}
