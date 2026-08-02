#include "backend_scanner_control_codec.h"

#include <inttypes.h>
#include <string.h>

#include "backend_identity.h"
#include "backend_json_reader.h"
#include "backend_json_writer.h"
#include "time_sync_policy.h"

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

static bool valid_sha256(const char *value)
{
    if (!value || strlen(value) != 64U) {
        return false;
    }
    for (size_t i = 0; i < 64U; ++i) {
        if (!hex_char(value[i])) {
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

static const char *time_source_name(backend_scanner_time_source_t source)
{
    switch (source) {
    case BACKEND_SCANNER_TIME_NONE:
        return "none";
    case BACKEND_SCANNER_TIME_SNTP:
        return "sntp";
    case BACKEND_SCANNER_TIME_BACKEND:
        return "backend";
    default:
        return NULL;
    }
}

static bool parse_time_source(const char *value,
                              backend_scanner_time_source_t *out)
{
    if (strcmp(value, "none") == 0) {
        *out = BACKEND_SCANNER_TIME_NONE;
    } else if (strcmp(value, "sntp") == 0) {
        *out = BACKEND_SCANNER_TIME_SNTP;
    } else if (strcmp(value, "backend") == 0) {
        *out = BACKEND_SCANNER_TIME_BACKEND;
    } else {
        return false;
    }
    return true;
}

static const char *investigate_mode_name(
    backend_scanner_investigate_mode_t mode)
{
    switch (mode) {
    case BACKEND_SCANNER_INVESTIGATE_GATT:
        return "gatt";
    case BACKEND_SCANNER_INVESTIGATE_PASSIVE_CAPTURE:
        return "passive_capture";
    default:
        return NULL;
    }
}

static bool parse_investigate_mode(
    const char *value, backend_scanner_investigate_mode_t *out)
{
    if (strcmp(value, "gatt") == 0) {
        *out = BACKEND_SCANNER_INVESTIGATE_GATT;
    } else if (strcmp(value, "passive_capture") == 0) {
        *out = BACKEND_SCANNER_INVESTIGATE_PASSIVE_CAPTURE;
    } else {
        return false;
    }
    return true;
}

static bool valid_backend_identity(const char *target,
                                   const char *project,
                                   const char *hardware)
{
    const backend_firmware_identity_t *identity =
        backend_identity_for_image(BACKEND_IMAGE_SCANNER);
    return backend_identity_matches(identity, target, project, hardware);
}

static bool valid_control(const backend_scanner_control_t *control)
{
    if (!control) {
        return false;
    }
    switch (control->type) {
    case BACKEND_SCANNER_CONTROL_ROLE:
        return control->payload.role.boot_id != 0U &&
               control->payload.role.generation != 0U &&
               profile_name(control->payload.role.profile) != NULL;
    case BACKEND_SCANNER_CONTROL_TIME:
        if (control->payload.time.generation == 0U ||
            time_source_name(control->payload.time.source) == NULL) {
            return false;
        }
        return control->payload.time.valid
            ? fof_time_epoch_is_valid(control->payload.time.epoch_ms) &&
              control->payload.time.source != BACKEND_SCANNER_TIME_NONE
            : control->payload.time.epoch_ms == 0 &&
              control->payload.time.source == BACKEND_SCANNER_TIME_NONE;
    case BACKEND_SCANNER_CONTROL_FLOW:
        return control->payload.flow.generation != 0U;
    case BACKEND_SCANNER_CONTROL_LED_STATE:
        return terminated(control->payload.led.state,
                          sizeof(control->payload.led.state)) &&
               control->payload.led.state[0] != '\0' &&
               control->payload.led.generation != 0U;
    case BACKEND_SCANNER_CONTROL_HEALTH_REQUEST:
        return control->payload.health_request.sequence != 0U;
    case BACKEND_SCANNER_CONTROL_RECOVERY:
        return control->payload.recovery.boot_id != 0U &&
               control->payload.recovery.generation != 0U &&
               control->payload.recovery.action ==
                   BACKEND_SCANNER_RECOVERY_RESTART_RADIOS;
    case BACKEND_SCANNER_CONTROL_INVESTIGATE:
        return terminated(control->payload.investigate.command_id,
                          sizeof(control->payload.investigate.command_id)) &&
               control->payload.investigate.command_id[0] != '\0' &&
               terminated(control->payload.investigate.mac,
                          sizeof(control->payload.investigate.mac)) &&
               (!control->payload.investigate.has_mac ||
                valid_mac(control->payload.investigate.mac)) &&
               (!control->payload.investigate.has_mac
                    ? control->payload.investigate.mac[0] == '\0'
                    : true) &&
               investigate_mode_name(
                   control->payload.investigate.mode) != NULL &&
               control->payload.investigate.timeout_ms != 0U;
    case BACKEND_SCANNER_CONTROL_CANCEL:
        return terminated(control->payload.cancel.command_id,
                          sizeof(control->payload.cancel.command_id)) &&
               control->payload.cancel.command_id[0] != '\0';
    case BACKEND_SCANNER_CONTROL_OTA_BEGIN:
        return control->payload.ota_begin.session_id != 0U &&
               control->payload.ota_begin.generation != 0U &&
               control->payload.ota_begin.component_slot < 2U &&
               terminated(control->payload.ota_begin.expected_mac,
                          sizeof(control->payload.ota_begin.expected_mac)) &&
               valid_mac(control->payload.ota_begin.expected_mac) &&
               control->payload.ota_begin.expected_boot_id != 0U &&
               control->payload.ota_begin.expected_topology_generation != 0U &&
               terminated(control->payload.ota_begin.target,
                          sizeof(control->payload.ota_begin.target)) &&
               terminated(control->payload.ota_begin.project,
                          sizeof(control->payload.ota_begin.project)) &&
               terminated(control->payload.ota_begin.hardware,
                          sizeof(control->payload.ota_begin.hardware)) &&
               valid_backend_identity(control->payload.ota_begin.target,
                                      control->payload.ota_begin.project,
                                      control->payload.ota_begin.hardware) &&
               terminated(control->payload.ota_begin.version,
                          sizeof(control->payload.ota_begin.version)) &&
               control->payload.ota_begin.version[0] != '\0' &&
               control->payload.ota_begin.image_size != 0U &&
               terminated(control->payload.ota_begin.sha256,
                          sizeof(control->payload.ota_begin.sha256)) &&
               valid_sha256(control->payload.ota_begin.sha256);
    case BACKEND_SCANNER_CONTROL_OTA_END:
    case BACKEND_SCANNER_CONTROL_OTA_ABORT:
        return control->payload.ota_finish.session_id != 0U &&
               control->payload.ota_finish.generation != 0U &&
               terminated(control->payload.ota_finish.reason,
                          sizeof(control->payload.ota_finish.reason));
    default:
        return false;
    }
}

static void initialize_writer(backend_json_writer_t *writer,
                              char *output,
                              size_t capacity,
                              const char *type)
{
    size_t bounded = capacity;
    if (bounded > BACKEND_SCANNER_WIRE_MAX_LINE + 1U) {
        bounded = BACKEND_SCANNER_WIRE_MAX_LINE + 1U;
    }
    backend_json_writer_init(writer, output, bounded);
    backend_json_append(writer, "{\"type\":");
    backend_json_append_escaped(writer, type);
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

size_t backend_scanner_control_encode(
    const backend_scanner_control_t *control,
    char *output,
    size_t capacity)
{
    if (output && capacity > 0U) {
        output[0] = '\0';
    }
    if (!output || capacity == 0U || !valid_control(control)) {
        return 0U;
    }
    backend_json_writer_t writer;
    switch (control->type) {
    case BACKEND_SCANNER_CONTROL_ROLE:
        initialize_writer(&writer, output, capacity, "role");
        append_u32(&writer, "boot_id", control->payload.role.boot_id);
        append_u32(&writer, "generation", control->payload.role.generation);
        append_string(&writer, "profile",
                      profile_name(control->payload.role.profile));
        break;
    case BACKEND_SCANNER_CONTROL_TIME:
        initialize_writer(&writer, output, capacity, "time");
        append_u32(&writer, "generation", control->payload.time.generation);
        append_bool(&writer, "valid", control->payload.time.valid);
        append_key(&writer, "epoch_ms");
        backend_json_append_format(&writer, "%" PRId64,
                                   control->payload.time.epoch_ms);
        append_string(&writer, "source",
                      time_source_name(control->payload.time.source));
        break;
    case BACKEND_SCANNER_CONTROL_FLOW:
        initialize_writer(&writer, output, capacity, "flow");
        append_u32(&writer, "generation", control->payload.flow.generation);
        append_bool(&writer, "paused", control->payload.flow.paused);
        break;
    case BACKEND_SCANNER_CONTROL_LED_STATE:
        initialize_writer(&writer, output, capacity, "led_state");
        append_string(&writer, "state", control->payload.led.state);
        append_u32(&writer, "generation", control->payload.led.generation);
        append_u32(&writer, "ttl_ms", control->payload.led.ttl_ms);
        break;
    case BACKEND_SCANNER_CONTROL_HEALTH_REQUEST:
        initialize_writer(&writer, output, capacity, "health_request");
        append_u32(&writer, "sequence",
                   control->payload.health_request.sequence);
        break;
    case BACKEND_SCANNER_CONTROL_RECOVERY:
        initialize_writer(&writer, output, capacity, "recovery");
        append_u32(&writer, "boot_id", control->payload.recovery.boot_id);
        append_u32(&writer, "generation",
                   control->payload.recovery.generation);
        append_string(&writer, "action", "restart_radios");
        break;
    case BACKEND_SCANNER_CONTROL_INVESTIGATE:
        initialize_writer(&writer, output, capacity, "investigate");
        append_string(&writer, "command_id",
                      control->payload.investigate.command_id);
        append_key(&writer, "mac");
        if (control->payload.investigate.has_mac) {
            backend_json_append_escaped(
                &writer, control->payload.investigate.mac);
        } else {
            backend_json_append(&writer, "null");
        }
        append_string(&writer, "mode", investigate_mode_name(
            control->payload.investigate.mode));
        append_u32(&writer, "timeout_ms",
                   control->payload.investigate.timeout_ms);
        break;
    case BACKEND_SCANNER_CONTROL_CANCEL:
        initialize_writer(&writer, output, capacity, "cancel");
        append_string(&writer, "command_id",
                      control->payload.cancel.command_id);
        break;
    case BACKEND_SCANNER_CONTROL_OTA_BEGIN:
        initialize_writer(&writer, output, capacity, "ota_begin");
        append_u32(&writer, "session_id",
                   control->payload.ota_begin.session_id);
        append_u32(&writer, "generation",
                   control->payload.ota_begin.generation);
        append_u32(&writer, "component_slot",
                   control->payload.ota_begin.component_slot);
        append_string(&writer, "expected_mac",
                      control->payload.ota_begin.expected_mac);
        append_u32(&writer, "expected_boot_id",
                   control->payload.ota_begin.expected_boot_id);
        append_u32(&writer, "expected_topology_generation",
                   control->payload.ota_begin.expected_topology_generation);
        append_string(&writer, "target", control->payload.ota_begin.target);
        append_string(&writer, "project", control->payload.ota_begin.project);
        append_string(&writer, "hardware",
                      control->payload.ota_begin.hardware);
        append_string(&writer, "version", control->payload.ota_begin.version);
        append_u32(&writer, "image_size",
                   control->payload.ota_begin.image_size);
        append_u32(&writer, "crc32", control->payload.ota_begin.crc32);
        append_string(&writer, "sha256", control->payload.ota_begin.sha256);
        append_bool(&writer, "allow_same_version",
                    control->payload.ota_begin.allow_same_version);
        append_bool(&writer, "dry_run",
                    control->payload.ota_begin.dry_run);
        break;
    case BACKEND_SCANNER_CONTROL_OTA_END:
    case BACKEND_SCANNER_CONTROL_OTA_ABORT:
        initialize_writer(&writer, output, capacity,
            control->type == BACKEND_SCANNER_CONTROL_OTA_END
                ? "ota_end" : "ota_abort");
        append_u32(&writer, "session_id",
                   control->payload.ota_finish.session_id);
        append_u32(&writer, "generation",
                   control->payload.ota_finish.generation);
        append_string(&writer, "reason",
                      control->payload.ota_finish.reason);
        break;
    default:
        return 0U;
    }
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

static size_t control_field_count(backend_scanner_control_kind_t type)
{
    switch (type) {
    case BACKEND_SCANNER_CONTROL_ROLE:
        return 4U;
    case BACKEND_SCANNER_CONTROL_TIME:
        return 5U;
    case BACKEND_SCANNER_CONTROL_FLOW:
        return 3U;
    case BACKEND_SCANNER_CONTROL_LED_STATE:
        return 4U;
    case BACKEND_SCANNER_CONTROL_HEALTH_REQUEST:
        return 2U;
    case BACKEND_SCANNER_CONTROL_RECOVERY:
        return 4U;
    case BACKEND_SCANNER_CONTROL_INVESTIGATE:
        return 5U;
    case BACKEND_SCANNER_CONTROL_CANCEL:
        return 2U;
    case BACKEND_SCANNER_CONTROL_OTA_BEGIN:
        return 16U;
    case BACKEND_SCANNER_CONTROL_OTA_END:
    case BACKEND_SCANNER_CONTROL_OTA_ABORT:
        return 4U;
    default:
        return 0U;
    }
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

static bool read_u32(const char *json,
                     const backend_json_token_t *tokens,
                     size_t token_count,
                     const char *key,
                     uint32_t *output)
{
    size_t index = 0;
    uint64_t value = 0;
    if (!find_required(json, tokens, token_count, key, &index) ||
        !backend_json_get_u64(json, &tokens[index], &value) ||
        value > UINT32_MAX) {
        return false;
    }
    *output = (uint32_t)value;
    return true;
}

static bool read_i64(const char *json,
                     const backend_json_token_t *tokens,
                     size_t token_count,
                     const char *key,
                     int64_t *output)
{
    size_t index = 0;
    return find_required(json, tokens, token_count, key, &index) &&
           backend_json_get_i64(json, &tokens[index], output);
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

backend_scanner_control_decode_result_t backend_scanner_control_decode(
    const char *json,
    size_t length,
    backend_scanner_control_t *out)
{
    if (length > BACKEND_SCANNER_WIRE_MAX_LINE) {
        return BACKEND_SCANNER_CONTROL_TOO_LARGE;
    }
    if (!json || !out || length == 0U) {
        return BACKEND_SCANNER_CONTROL_MALFORMED;
    }
    backend_json_token_t tokens[BACKEND_JSON_MAX_TOKENS];
    size_t token_count = 0;
    if (backend_json_parse(json, length, tokens, BACKEND_JSON_MAX_TOKENS,
                           &token_count) != BACKEND_JSON_OK ||
        token_count == 0U || tokens[0].kind != BACKEND_JSON_OBJECT) {
        return BACKEND_SCANNER_CONTROL_MALFORMED;
    }

    backend_scanner_control_t control = {0};
    char type[24];
    if (!read_string(json, tokens, token_count, "type", type,
                     sizeof(type))) {
        return BACKEND_SCANNER_CONTROL_SCHEMA_MISMATCH;
    }
    if (strcmp(type, "role") == 0) {
        char profile[24];
        control.type = BACKEND_SCANNER_CONTROL_ROLE;
        if (!read_u32(json, tokens, token_count, "boot_id",
                      &control.payload.role.boot_id) ||
            !read_u32(json, tokens, token_count, "generation",
                      &control.payload.role.generation) ||
            !read_string(json, tokens, token_count, "profile", profile,
                         sizeof(profile)) ||
            !parse_profile(profile, &control.payload.role.profile)) {
            return BACKEND_SCANNER_CONTROL_SCHEMA_MISMATCH;
        }
    } else if (strcmp(type, "time") == 0) {
        char source[16];
        control.type = BACKEND_SCANNER_CONTROL_TIME;
        if (!read_u32(json, tokens, token_count, "generation",
                      &control.payload.time.generation) ||
            !read_bool(json, tokens, token_count, "valid",
                       &control.payload.time.valid) ||
            !read_i64(json, tokens, token_count, "epoch_ms",
                      &control.payload.time.epoch_ms) ||
            !read_string(json, tokens, token_count, "source", source,
                         sizeof(source)) ||
            !parse_time_source(source, &control.payload.time.source)) {
            return BACKEND_SCANNER_CONTROL_SCHEMA_MISMATCH;
        }
    } else if (strcmp(type, "flow") == 0) {
        control.type = BACKEND_SCANNER_CONTROL_FLOW;
        if (!read_u32(json, tokens, token_count, "generation",
                      &control.payload.flow.generation) ||
            !read_bool(json, tokens, token_count, "paused",
                       &control.payload.flow.paused)) {
            return BACKEND_SCANNER_CONTROL_SCHEMA_MISMATCH;
        }
    } else if (strcmp(type, "led_state") == 0) {
        control.type = BACKEND_SCANNER_CONTROL_LED_STATE;
        if (!read_string(json, tokens, token_count, "state",
                         control.payload.led.state,
                         sizeof(control.payload.led.state)) ||
            !read_u32(json, tokens, token_count, "generation",
                      &control.payload.led.generation) ||
            !read_u32(json, tokens, token_count, "ttl_ms",
                      &control.payload.led.ttl_ms)) {
            return BACKEND_SCANNER_CONTROL_SCHEMA_MISMATCH;
        }
    } else if (strcmp(type, "health_request") == 0) {
        control.type = BACKEND_SCANNER_CONTROL_HEALTH_REQUEST;
        if (!read_u32(json, tokens, token_count, "sequence",
                      &control.payload.health_request.sequence)) {
            return BACKEND_SCANNER_CONTROL_SCHEMA_MISMATCH;
        }
    } else if (strcmp(type, "recovery") == 0) {
        char action[24];
        control.type = BACKEND_SCANNER_CONTROL_RECOVERY;
        if (!read_u32(json, tokens, token_count, "boot_id",
                      &control.payload.recovery.boot_id) ||
            !read_u32(json, tokens, token_count, "generation",
                      &control.payload.recovery.generation) ||
            !read_string(json, tokens, token_count, "action", action,
                         sizeof(action)) ||
            strcmp(action, "restart_radios") != 0) {
            return BACKEND_SCANNER_CONTROL_SCHEMA_MISMATCH;
        }
        control.payload.recovery.action =
            BACKEND_SCANNER_RECOVERY_RESTART_RADIOS;
    } else if (strcmp(type, "investigate") == 0) {
        char mode[24];
        size_t mac_index = 0;
        control.type = BACKEND_SCANNER_CONTROL_INVESTIGATE;
        if (!read_string(json, tokens, token_count, "command_id",
                         control.payload.investigate.command_id,
                         sizeof(control.payload.investigate.command_id)) ||
            !find_required(json, tokens, token_count, "mac", &mac_index) ||
            !read_string(json, tokens, token_count, "mode", mode,
                         sizeof(mode)) ||
            !parse_investigate_mode(
                mode, &control.payload.investigate.mode) ||
            !read_u32(json, tokens, token_count, "timeout_ms",
                      &control.payload.investigate.timeout_ms)) {
            return BACKEND_SCANNER_CONTROL_SCHEMA_MISMATCH;
        }
        if (tokens[mac_index].kind == BACKEND_JSON_NULL) {
            control.payload.investigate.has_mac = false;
            control.payload.investigate.mac[0] = '\0';
        } else if (backend_json_copy_string(
                       json, &tokens[mac_index],
                       control.payload.investigate.mac,
                       sizeof(control.payload.investigate.mac))) {
            control.payload.investigate.has_mac = true;
        } else {
            return BACKEND_SCANNER_CONTROL_SCHEMA_MISMATCH;
        }
    } else if (strcmp(type, "cancel") == 0) {
        control.type = BACKEND_SCANNER_CONTROL_CANCEL;
        if (!read_string(json, tokens, token_count, "command_id",
                         control.payload.cancel.command_id,
                         sizeof(control.payload.cancel.command_id))) {
            return BACKEND_SCANNER_CONTROL_SCHEMA_MISMATCH;
        }
    } else if (strcmp(type, "ota_begin") == 0) {
        uint32_t component_slot = 0;
        control.type = BACKEND_SCANNER_CONTROL_OTA_BEGIN;
        if (!read_u32(json, tokens, token_count, "session_id",
                      &control.payload.ota_begin.session_id) ||
            !read_u32(json, tokens, token_count, "generation",
                      &control.payload.ota_begin.generation) ||
            !read_u32(json, tokens, token_count, "component_slot",
                      &component_slot) || component_slot > UINT8_MAX ||
            !read_string(json, tokens, token_count, "expected_mac",
                         control.payload.ota_begin.expected_mac,
                         sizeof(control.payload.ota_begin.expected_mac)) ||
            !read_u32(json, tokens, token_count, "expected_boot_id",
                      &control.payload.ota_begin.expected_boot_id) ||
            !read_u32(json, tokens, token_count,
                      "expected_topology_generation",
                      &control.payload.ota_begin
                           .expected_topology_generation) ||
            !read_string(json, tokens, token_count, "target",
                         control.payload.ota_begin.target,
                         sizeof(control.payload.ota_begin.target)) ||
            !read_string(json, tokens, token_count, "project",
                         control.payload.ota_begin.project,
                         sizeof(control.payload.ota_begin.project)) ||
            !read_string(json, tokens, token_count, "hardware",
                         control.payload.ota_begin.hardware,
                         sizeof(control.payload.ota_begin.hardware)) ||
            !read_string(json, tokens, token_count, "version",
                         control.payload.ota_begin.version,
                         sizeof(control.payload.ota_begin.version)) ||
            !read_u32(json, tokens, token_count, "image_size",
                      &control.payload.ota_begin.image_size) ||
            !read_u32(json, tokens, token_count, "crc32",
                      &control.payload.ota_begin.crc32) ||
            !read_string(json, tokens, token_count, "sha256",
                         control.payload.ota_begin.sha256,
                         sizeof(control.payload.ota_begin.sha256)) ||
            !read_bool(json, tokens, token_count, "allow_same_version",
                       &control.payload.ota_begin.allow_same_version) ||
            !read_bool(json, tokens, token_count, "dry_run",
                       &control.payload.ota_begin.dry_run)) {
            return BACKEND_SCANNER_CONTROL_SCHEMA_MISMATCH;
        }
        control.payload.ota_begin.component_slot = (uint8_t)component_slot;
    } else if (strcmp(type, "ota_end") == 0 ||
               strcmp(type, "ota_abort") == 0) {
        control.type = strcmp(type, "ota_end") == 0
            ? BACKEND_SCANNER_CONTROL_OTA_END
            : BACKEND_SCANNER_CONTROL_OTA_ABORT;
        if (!read_u32(json, tokens, token_count, "session_id",
                      &control.payload.ota_finish.session_id) ||
            !read_u32(json, tokens, token_count, "generation",
                      &control.payload.ota_finish.generation) ||
            !read_string(json, tokens, token_count, "reason",
                         control.payload.ota_finish.reason,
                         sizeof(control.payload.ota_finish.reason))) {
            return BACKEND_SCANNER_CONTROL_SCHEMA_MISMATCH;
        }
    } else {
        return BACKEND_SCANNER_CONTROL_SCHEMA_MISMATCH;
    }

    const size_t expected_fields = control_field_count(control.type);
    if (expected_fields == 0U ||
        tokens[0].child_count != expected_fields * 2U ||
        !valid_control(&control)) {
        return BACKEND_SCANNER_CONTROL_SCHEMA_MISMATCH;
    }
    *out = control;
    return BACKEND_SCANNER_CONTROL_DECODE_OK;
}
