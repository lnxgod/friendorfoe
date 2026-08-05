#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "backend_ota_operation_id.h"
#include "../support/backend_test_main.h"

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
#include <unistd.h>

#include "backend_json_reader.h"
#include "backend_ota_command_client.h"
#endif

void setUp(void) {}
void tearDown(void) {}

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)

#define RECEIPT_FIXTURE_FILE_CAPACITY 16384U
#define RECEIPT_FIXTURE_TOKENS 256U

typedef struct {
    backend_ota_operation_id_t operation_id;
    backend_ota_receipt_command_t command;
    backend_ota_receipt_end_t end;
    uint32_t sequence;
    uint32_t next_sequence;
    char catalog_name[65];
    char expected_sha256[65];
    char state[32];
    char decision[32];
    char error[32];
    char target[65];
    char project[65];
    char hardware[65];
    char version[65];
    char preimage_utf8[BACKEND_OTA_RECEIPT_PREIMAGE_MAX_BYTES];
    char preimage_hex[BACKEND_OTA_RECEIPT_PREIMAGE_MAX_BYTES * 2U + 1U];
    char receipt_sha256[65];
    char probe_receipt_sha256[65];
} receipt_fixture_vector_t;

typedef struct {
    receipt_fixture_vector_t probe;
    receipt_fixture_vector_t apply;
} receipt_fixture_t;

static void fixture_vector_rebind(receipt_fixture_vector_t *vector);

static bool fixture_find(
    const char *json,
    const backend_json_token_t *tokens,
    size_t count,
    size_t object,
    const char *name,
    size_t *out)
{
    return backend_json_object_find(json, tokens, count, object, name, out);
}

static bool fixture_string(
    const char *json,
    const backend_json_token_t *tokens,
    size_t count,
    size_t object,
    const char *name,
    char *out,
    size_t capacity)
{
    size_t value = 0U;
    return fixture_find(json, tokens, count, object, name, &value) &&
           backend_json_copy_string(json, &tokens[value], out, capacity);
}

static bool fixture_u32(
    const char *json,
    const backend_json_token_t *tokens,
    size_t count,
    size_t object,
    const char *name,
    uint32_t *out)
{
    size_t value = 0U;
    uint64_t parsed = 0U;
    return out != NULL && fixture_find(json, tokens, count, object, name, &value) &&
           backend_json_get_u64(json, &tokens[value], &parsed) &&
           parsed <= UINT32_MAX && ((*out = (uint32_t)parsed), true);
}

static bool fixture_bool(
    const char *json,
    const backend_json_token_t *tokens,
    size_t count,
    size_t object,
    const char *name,
    bool *out)
{
    size_t value = 0U;
    return out != NULL && fixture_find(json, tokens, count, object, name, &value) &&
           backend_json_get_bool(json, &tokens[value], out);
}

static bool fixture_object(
    const char *json,
    const backend_json_token_t *tokens,
    size_t count,
    size_t parent,
    const char *name,
    size_t *out)
{
    return fixture_find(json, tokens, count, parent, name, out) &&
           tokens[*out].kind == BACKEND_JSON_OBJECT;
}

static bool fixture_exact_keys(
    const char *json,
    const backend_json_token_t *tokens,
    size_t count,
    size_t object,
    const char *const *names,
    size_t name_count)
{
    if (tokens[object].kind != BACKEND_JSON_OBJECT ||
        tokens[object].child_count != name_count * 2U) {
        return false;
    }
    for (size_t index = 0U; index < name_count; ++index) {
        size_t ignored = 0U;
        if (!fixture_find(json, tokens, count, object, names[index], &ignored)) {
            return false;
        }
    }
    return true;
}

static bool fixture_lower_hex(const char *value, size_t length)
{
    if (value == NULL || strlen(value) != length) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        if (!((value[index] >= '0' && value[index] <= '9') ||
              (value[index] >= 'a' && value[index] <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool fixture_hex_matches_text(const char *hex, const char *text)
{
    const size_t text_length = text == NULL ? 0U : strlen(text);
    if (text_length == 0U || !fixture_lower_hex(hex, text_length * 2U)) {
        return false;
    }
    for (size_t index = 0U; index < text_length; ++index) {
        const char high = hex[index * 2U];
        const char low = hex[index * 2U + 1U];
        const uint8_t high_value = (uint8_t)(
            high <= '9' ? high - '0' : high - 'a' + 10);
        const uint8_t low_value = (uint8_t)(
            low <= '9' ? low - '0' : low - 'a' + 10);
        if ((uint8_t)text[index] != (uint8_t)((high_value << 4U) | low_value)) {
            return false;
        }
    }
    return true;
}

static bool fixture_mac(const char *text, uint8_t output[6])
{
    if (text == NULL || output == NULL || strlen(text) != 17U) {
        return false;
    }
    for (size_t index = 0U; index < 6U; ++index) {
        const size_t offset = index * 3U;
        unsigned byte = 0U;
        for (size_t digit = 0U; digit < 2U; ++digit) {
            const char value = text[offset + digit];
            if (value >= '0' && value <= '9') {
                byte = byte * 16U + (unsigned)(value - '0');
            } else if (value >= 'A' && value <= 'F') {
                byte = byte * 16U + (unsigned)(value - 'A' + 10);
            } else {
                return false;
            }
        }
        if ((index < 5U && text[offset + 2U] != ':') ||
            (index == 5U && text[offset + 2U] != '\0')) {
            return false;
        }
        output[index] = (uint8_t)byte;
    }
    return true;
}

static bool fixture_component(const char *name, backend_ota_component_t *out)
{
    if (name == NULL || out == NULL) {
        return false;
    }
    if (strcmp(name, "scanner0") == 0) {
        *out = BACKEND_OTA_COMPONENT_SCANNER0;
    } else if (strcmp(name, "scanner1") == 0) {
        *out = BACKEND_OTA_COMPONENT_SCANNER1;
    } else if (strcmp(name, "uplink") == 0) {
        *out = BACKEND_OTA_COMPONENT_UPLINK;
    } else {
        return false;
    }
    return true;
}

static bool fixture_mode(const char *name, backend_ota_apply_mode_t *out)
{
    if (name == NULL || out == NULL) {
        return false;
    }
    if (strcmp(name, "newer_only") == 0) {
        *out = BACKEND_OTA_NEWER_ONLY;
    } else if (strcmp(name, "same_version_recovery") == 0) {
        *out = BACKEND_OTA_SAME_VERSION_RECOVERY;
    } else {
        return false;
    }
    return true;
}

static bool fixture_vector_load(
    const char *json,
    const backend_json_token_t *tokens,
    size_t count,
    size_t object,
    bool is_apply,
    receipt_fixture_vector_t *out)
{
    static const char *const vector_keys[] = {
        "command", "end", "preimage_utf8", "preimage_hex", "receipt_sha256",
    };
    static const char *const probe_command_keys[] = {
        "schema", "operation_id", "type", "component", "catalog_name",
        "expected_sha256", "expected_size", "expected_uplink_mac",
        "expected_uplink_boot_id", "expected_target_mac",
        "expected_target_boot_id", "expected_topology_generation", "apply_mode",
        "next_sequence",
    };
    static const char *const apply_command_keys[] = {
        "schema", "operation_id", "type", "component", "catalog_name",
        "expected_sha256", "expected_size", "expected_uplink_mac",
        "expected_uplink_boot_id", "expected_target_mac",
        "expected_target_boot_id", "expected_topology_generation", "apply_mode",
        "next_sequence", "probe_receipt_sha256",
    };
    static const char *const end_keys[] = {
        "schema", "operation_id", "sequence", "type", "component",
        "catalog_name", "state", "decision", "error", "image_writes",
        "target", "project", "hardware", "version", "actual_mac",
        "actual_boot_id", "actual_topology_generation", "role_healthy",
        "radio_healthy", "rollback_clear",
    };
    if (out == NULL || !fixture_exact_keys(
            json, tokens, count, object, vector_keys,
            sizeof(vector_keys) / sizeof(vector_keys[0]))) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    size_t command = 0U;
    size_t end = 0U;
    char operation_id[33] = {0};
    char end_operation_id[33] = {0};
    char type[32] = {0};
    char end_catalog[65] = {0};
    char component[16] = {0};
    char end_component[16] = {0};
    char mode[32] = {0};
    char uplink_mac[18] = {0};
    char target_mac[18] = {0};
    char actual_mac[18] = {0};
    uint32_t schema = 0U;
    uint32_t end_schema = 0U;
    if (!fixture_object(json, tokens, count, object, "command", &command) ||
        !fixture_object(json, tokens, count, object, "end", &end) ||
        !fixture_exact_keys(json, tokens, count, command,
            is_apply ? apply_command_keys : probe_command_keys,
            is_apply ? sizeof(apply_command_keys) / sizeof(apply_command_keys[0]) :
                sizeof(probe_command_keys) / sizeof(probe_command_keys[0])) ||
        !fixture_exact_keys(json, tokens, count, end, end_keys,
            sizeof(end_keys) / sizeof(end_keys[0])) ||
        !fixture_u32(json, tokens, count, command, "schema", &schema) ||
        schema != 1U ||
        !fixture_string(json, tokens, count, command, "operation_id", operation_id,
            sizeof(operation_id)) ||
        !backend_ota_operation_id_decode(operation_id, &out->operation_id) ||
        !fixture_string(json, tokens, count, command, "type", type, sizeof(type)) ||
        strcmp(type, is_apply ? "backend_ota_apply" : "backend_ota_probe") != 0 ||
        !fixture_string(json, tokens, count, command, "component", component,
            sizeof(component)) ||
        !fixture_component(component, &out->command.component) ||
        !fixture_string(json, tokens, count, command, "catalog_name", out->catalog_name,
            sizeof(out->catalog_name)) ||
        !fixture_string(json, tokens, count, command, "expected_sha256",
            out->expected_sha256, sizeof(out->expected_sha256)) ||
        !fixture_lower_hex(out->expected_sha256, 64U) ||
        !fixture_u32(json, tokens, count, command, "expected_size",
            &out->command.expected_size) ||
        out->command.expected_size == 0U ||
        !fixture_string(json, tokens, count, command, "expected_uplink_mac",
            uplink_mac, sizeof(uplink_mac)) ||
        !fixture_mac(uplink_mac, out->command.binding.uplink_mac) ||
        !fixture_u32(json, tokens, count, command, "expected_uplink_boot_id",
            &out->command.binding.uplink_boot_id) ||
        !fixture_string(json, tokens, count, command, "expected_target_mac",
            target_mac, sizeof(target_mac)) ||
        !fixture_mac(target_mac, out->command.binding.target_mac) ||
        !fixture_u32(json, tokens, count, command, "expected_target_boot_id",
            &out->command.binding.target_boot_id) ||
        !fixture_u32(json, tokens, count, command, "expected_topology_generation",
            &out->command.binding.topology_generation) ||
        !fixture_string(json, tokens, count, command, "apply_mode", mode, sizeof(mode)) ||
        !fixture_mode(mode, &out->command.apply_mode) ||
        !fixture_u32(json, tokens, count, command, "next_sequence", &out->next_sequence) ||
        !fixture_u32(json, tokens, count, end, "schema", &end_schema) ||
        end_schema != 1U ||
        !fixture_string(json, tokens, count, end, "operation_id", end_operation_id,
            sizeof(end_operation_id)) ||
        strcmp(operation_id, end_operation_id) != 0 ||
        !fixture_u32(json, tokens, count, end, "sequence", &out->sequence) ||
        !fixture_string(json, tokens, count, end, "type", type, sizeof(type)) ||
        strcmp(type, "backend_ota_end") != 0 ||
        !fixture_string(json, tokens, count, end, "component", end_component,
            sizeof(end_component)) ||
        strcmp(component, end_component) != 0 ||
        !fixture_string(json, tokens, count, end, "catalog_name", end_catalog,
            sizeof(end_catalog)) ||
        strcmp(end_catalog, out->catalog_name) != 0 ||
        !fixture_string(json, tokens, count, end, "state", out->state,
            sizeof(out->state)) ||
        !fixture_string(json, tokens, count, end, "decision", out->decision,
            sizeof(out->decision)) ||
        !fixture_string(json, tokens, count, end, "error", out->error,
            sizeof(out->error)) ||
        !fixture_u32(json, tokens, count, end, "image_writes", &out->end.image_writes) ||
        !fixture_string(json, tokens, count, end, "target", out->target,
            sizeof(out->target)) ||
        !fixture_string(json, tokens, count, end, "project", out->project,
            sizeof(out->project)) ||
        !fixture_string(json, tokens, count, end, "hardware", out->hardware,
            sizeof(out->hardware)) ||
        !fixture_string(json, tokens, count, end, "version", out->version,
            sizeof(out->version)) ||
        !fixture_string(json, tokens, count, end, "actual_mac", actual_mac,
            sizeof(actual_mac)) ||
        !fixture_mac(actual_mac, out->end.actual_mac) ||
        !fixture_u32(json, tokens, count, end, "actual_boot_id", &out->end.actual_boot_id) ||
        !fixture_u32(json, tokens, count, end, "actual_topology_generation",
            &out->end.actual_topology_generation) ||
        !fixture_bool(json, tokens, count, end, "role_healthy", &out->end.role_healthy) ||
        !fixture_bool(json, tokens, count, end, "radio_healthy", &out->end.radio_healthy) ||
        !fixture_bool(json, tokens, count, end, "rollback_clear", &out->end.rollback_clear) ||
        !fixture_string(json, tokens, count, object, "preimage_utf8",
            out->preimage_utf8, sizeof(out->preimage_utf8)) ||
        !fixture_string(json, tokens, count, object, "preimage_hex",
            out->preimage_hex, sizeof(out->preimage_hex)) ||
        !fixture_hex_matches_text(out->preimage_hex, out->preimage_utf8) ||
        !fixture_string(json, tokens, count, object, "receipt_sha256",
            out->receipt_sha256, sizeof(out->receipt_sha256)) ||
        !fixture_lower_hex(out->receipt_sha256, 64U)) {
        return false;
    }
    const size_t preimage_length = strlen(out->preimage_utf8);
    if (preimage_length == 0U ||
        out->preimage_utf8[preimage_length - 1U] != '\n' ||
        (preimage_length > 1U && out->preimage_utf8[preimage_length - 2U] == '\n')) {
        return false;
    }
    if (is_apply &&
        (!fixture_string(json, tokens, count, command, "probe_receipt_sha256",
             out->probe_receipt_sha256, sizeof(out->probe_receipt_sha256)) ||
         !fixture_lower_hex(out->probe_receipt_sha256, 64U))) {
        return false;
    }
    out->command.is_apply = is_apply;
    out->command.catalog_name = out->catalog_name;
    out->command.expected_sha256 = out->expected_sha256;
    out->end.state = out->state;
    out->end.decision = out->decision;
    out->end.error = out->error;
    out->end.target = out->target;
    out->end.project = out->project;
    out->end.hardware = out->hardware;
    out->end.version = out->version;
    return true;
}

static bool receipt_fixture_load(const char *path, receipt_fixture_t *out)
{
    static const char *const root_keys[] = {"schema", "probe", "apply"};
    char json[RECEIPT_FIXTURE_FILE_CAPACITY];
    backend_json_token_t tokens[RECEIPT_FIXTURE_TOKENS];
    FILE *file = NULL;
    size_t length = 0U;
    size_t count = 0U;
    size_t probe = 0U;
    size_t apply = 0U;
    uint32_t schema = 0U;
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (path == NULL || (file = fopen(path, "rb")) == NULL) {
        return false;
    }
    length = fread(json, 1U, sizeof(json) - 1U, file);
    const bool read_ok = !ferror(file) && !feof(file) ? false : !ferror(file);
    if (fclose(file) != 0 || !read_ok || length == 0U ||
        length == sizeof(json) - 1U) {
        return false;
    }
    json[length] = '\0';
    const backend_json_result_t parsed = backend_json_parse(
        json, length, tokens, RECEIPT_FIXTURE_TOKENS, &count);
    if (parsed != BACKEND_JSON_OK ||
        !fixture_exact_keys(json, tokens, count, 0U, root_keys,
            sizeof(root_keys) / sizeof(root_keys[0])) ||
        !fixture_u32(json, tokens, count, 0U, "schema", &schema) || schema != 1U ||
        !fixture_object(json, tokens, count, 0U, "probe", &probe) ||
        !fixture_object(json, tokens, count, 0U, "apply", &apply)) {
        return false;
    }
    receipt_fixture_t candidate;
    memset(&candidate, 0, sizeof(candidate));
    const bool probe_ok = fixture_vector_load(
        json, tokens, count, probe, false, &candidate.probe);
    const bool apply_ok = fixture_vector_load(
        json, tokens, count, apply, true, &candidate.apply);
    if (!probe_ok || !apply_ok) {
        return false;
    }
    *out = candidate;
    fixture_vector_rebind(&out->probe);
    fixture_vector_rebind(&out->apply);
    return true;
}

static bool fixture_hex_decode(
    const char *text,
    uint8_t *out,
    size_t capacity,
    size_t *out_length)
{
    const size_t length = text == NULL ? 0U : strlen(text);
    if (out == NULL || out_length == NULL || (length & 1U) != 0U ||
        length / 2U > capacity || !fixture_lower_hex(text, length)) {
        return false;
    }
    for (size_t index = 0U; index < length / 2U; ++index) {
        const char high = text[index * 2U];
        const char low = text[index * 2U + 1U];
        const uint8_t high_value = (uint8_t)(high <= '9' ? high - '0' : high - 'a' + 10);
        const uint8_t low_value = (uint8_t)(low <= '9' ? low - '0' : low - 'a' + 10);
        out[index] = (uint8_t)((high_value << 4U) | low_value);
    }
    *out_length = length / 2U;
    return true;
}

static backend_ota_command_local_t local_scanner0(void)
{
    backend_ota_command_local_t local = {
        .component = BACKEND_OTA_COMPONENT_SCANNER0,
        .catalog_name = "scanner-s3-combo-fullsize-backend",
        .target = "scanner-s3-combo-fullsize-backend",
        .project = "fof_backend_scanner_fullsize",
        .hardware = "esp32s3_n16r8_fullsize",
        .binding = {
            .uplink_mac = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01},
            .uplink_boot_id = 101U,
            .target_mac = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02},
            .target_boot_id = 202U,
            .topology_generation = 7U,
        },
    };
    local.max_expected_size = FOF_BACKEND_SCANNER_CACHE_CAPACITY;
    return local;
}

static backend_ota_command_local_t local_uplink(void)
{
    backend_ota_command_local_t local = local_scanner0();
    local.component = BACKEND_OTA_COMPONENT_UPLINK;
    local.catalog_name = "uplink-s3-fullsize-backend";
    local.target = "uplink-s3-fullsize-backend";
    local.project = "fof_backend_uplink_fullsize";
    memcpy(local.binding.target_mac, local.binding.uplink_mac, 6U);
    local.binding.target_boot_id = local.binding.uplink_boot_id;
    local.max_expected_size = FOF_BACKEND_UPLINK_APP_CAPACITY;
    return local;
}

static const char PROBE[] =
    "{\"schema\":1,\"operation_id\":\"0123456789abcdef0123456789abcdef\","
    "\"type\":\"backend_ota_probe\",\"component\":\"scanner0\","
    "\"catalog_name\":\"scanner-s3-combo-fullsize-backend\","
    "\"expected_sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
    "\"expected_size\":1048576,\"expected_uplink_mac\":\"AA:BB:CC:DD:EE:01\","
    "\"expected_uplink_boot_id\":101,\"expected_target_mac\":\"AA:BB:CC:DD:EE:02\","
    "\"expected_target_boot_id\":202,\"expected_topology_generation\":7,"
    "\"apply_mode\":\"newer_only\",\"next_sequence\":0}";

static const char APPLY[] =
    "{\"schema\":1,\"operation_id\":\"0123456789abcdef0123456789abcdef\","
    "\"type\":\"backend_ota_apply\",\"component\":\"scanner0\","
    "\"catalog_name\":\"scanner-s3-combo-fullsize-backend\","
    "\"expected_sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
    "\"expected_size\":1048576,\"expected_uplink_mac\":\"AA:BB:CC:DD:EE:01\","
    "\"expected_uplink_boot_id\":101,\"expected_target_mac\":\"AA:BB:CC:DD:EE:02\","
    "\"expected_target_boot_id\":202,\"expected_topology_generation\":7,"
    "\"apply_mode\":\"newer_only\",\"next_sequence\":2,"
    "\"probe_receipt_sha256\":\"fe251f806c754ce75978ea3da2dba33787f61e033423f60271449a2873365778\"}";

typedef struct {
    const char *name;
    const char *value;
} command_json_field_t;

static const command_json_field_t PROBE_FIELDS[] = {
    {"schema", "1"},
    {"operation_id", "\"0123456789abcdef0123456789abcdef\""},
    {"type", "\"backend_ota_probe\""},
    {"component", "\"scanner0\""},
    {"catalog_name", "\"scanner-s3-combo-fullsize-backend\""},
    {"expected_sha256", "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\""},
    {"expected_size", "1048576"},
    {"expected_uplink_mac", "\"AA:BB:CC:DD:EE:01\""},
    {"expected_uplink_boot_id", "101"},
    {"expected_target_mac", "\"AA:BB:CC:DD:EE:02\""},
    {"expected_target_boot_id", "202"},
    {"expected_topology_generation", "7"},
    {"apply_mode", "\"newer_only\""},
    {"next_sequence", "0"},
};

static const command_json_field_t APPLY_FIELDS[] = {
    {"schema", "1"},
    {"operation_id", "\"0123456789abcdef0123456789abcdef\""},
    {"type", "\"backend_ota_apply\""},
    {"component", "\"scanner0\""},
    {"catalog_name", "\"scanner-s3-combo-fullsize-backend\""},
    {"expected_sha256", "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\""},
    {"expected_size", "1048576"},
    {"expected_uplink_mac", "\"AA:BB:CC:DD:EE:01\""},
    {"expected_uplink_boot_id", "101"},
    {"expected_target_mac", "\"AA:BB:CC:DD:EE:02\""},
    {"expected_target_boot_id", "202"},
    {"expected_topology_generation", "7"},
    {"apply_mode", "\"newer_only\""},
    {"next_sequence", "2"},
    {"probe_receipt_sha256", "\"fe251f806c754ce75978ea3da2dba33787f61e033423f60271449a2873365778\""},
};

static const command_json_field_t ACK_FIELDS[] = {
    {"ok", "true"},
    {"operation_id", "\"0123456789abcdef0123456789abcdef\""},
    {"accepted_sequence", "3"},
    {"next_sequence", "4"},
    {"current_component", "\"scanner0\""},
    {"current_action", "\"probe\""},
    {"terminal", "false"},
    {"duplicate", "false"},
};

static size_t build_command_json(
    char *out,
    size_t capacity,
    const command_json_field_t *fields,
    size_t field_count,
    size_t omit,
    size_t replace,
    const char *replacement,
    size_t duplicate)
{
    size_t used = 0U;
    if (out == NULL || capacity == 0U || fields == NULL) {
        return 0U;
    }
    out[used++] = '{';
    for (size_t index = 0U; index < field_count; ++index) {
        if (index == omit) {
            continue;
        }
        const char *value = index == replace ? replacement : fields[index].value;
        const int length = snprintf(out + used, capacity - used,
            "%s\"%s\":%s", used == 1U ? "" : ",", fields[index].name, value);
        if (length < 0 || (size_t)length >= capacity - used) {
            return 0U;
        }
        used += (size_t)length;
    }
    if (duplicate < field_count) {
        const int length = snprintf(out + used, capacity - used,
            "%s\"%s\":%s", used == 1U ? "" : ",",
            fields[duplicate].name, fields[duplicate].value);
        if (length < 0 || (size_t)length >= capacity - used) {
            return 0U;
        }
        used += (size_t)length;
    }
    if (used + 1U >= capacity) {
        return 0U;
    }
    out[used++] = '}';
    out[used] = '\0';
    return used;
}

static size_t append_unknown_json_field(char *json, size_t capacity)
{
    const size_t length = strlen(json);
    if (length == 0U || json[length - 1U] != '}' || length + 11U >= capacity) {
        return 0U;
    }
    const int written = snprintf(json + length - 1U, capacity - length + 1U,
        ",\"unknown\":1}");
    return written < 0 || (size_t)written >= capacity - length + 1U
        ? 0U : length - 1U + (size_t)written;
}

static void assert_command_rejected_and_cleared(
    const char *json,
    size_t length,
    const backend_ota_command_local_t *local)
{
    backend_ota_command_envelope_t output;
    memset(&output, 0xA5, sizeof(output));
    TEST_ASSERT_NOT_EQUAL(BACKEND_OTA_COMMAND_DECODE_OK,
        backend_ota_command_decode(json, length, local, &output));
    TEST_ASSERT_EQUAL_MEMORY(&(backend_ota_command_envelope_t){0}, &output,
        sizeof(output));
}

static void assert_ack_rejected_and_cleared(
    const char *json,
    size_t length,
    const backend_ota_operation_id_t *operation_id,
    uint32_t expected_sequence,
    backend_ota_component_t expected_component,
    const char *expected_action,
    bool expected_terminal)
{
    backend_ota_command_ack_t output;
    memset(&output, 0xA5, sizeof(output));
    TEST_ASSERT_FALSE(backend_ota_command_ack_decode(
        json, length, operation_id, expected_sequence, expected_component,
        expected_action, expected_terminal, &output));
    TEST_ASSERT_EQUAL_MEMORY(&(backend_ota_command_ack_t){0}, &output,
        sizeof(output));
}

static void accept_canonical_probe_for_apply(
    backend_ota_command_local_t *local,
    backend_ota_command_envelope_t *probe_out)
{
    backend_ota_command_envelope_t probe;
    TEST_ASSERT_EQUAL(BACKEND_OTA_COMMAND_DECODE_OK,
        backend_ota_command_decode(PROBE, strlen(PROBE), local, &probe));
    const backend_ota_command_ack_t transition = {
        .ok = true,
        .has_operation_id = true,
        .operation_id = probe.operation_id,
        .accepted_sequence = 1U,
        .next_sequence = 2U,
        .current_component = BACKEND_OTA_COMPONENT_SCANNER0,
        .current_action = "apply",
        .terminal = false,
        .duplicate = false,
    };
    TEST_ASSERT_TRUE(backend_ota_accepted_probe_capture(
        &probe,
        "fe251f806c754ce75978ea3da2dba33787f61e033423f60271449a2873365778",
        1U, &transition, &local->accepted_probe));
    local->has_accepted_probe = true;
    local->has_expected_next_sequence = true;
    local->expected_next_sequence = transition.next_sequence;
    if (probe_out != NULL) {
        *probe_out = probe;
    }
}

void test_fullsize_decoder_requires_exact_amended_probe_and_apply_shapes(void)
{
    backend_ota_command_envelope_t envelope;
    backend_ota_command_local_t local = local_scanner0();
    accept_canonical_probe_for_apply(&local, &envelope);
    backend_ota_command_envelope_t expected = {0};
    expected.has_operation_id = true;
    TEST_ASSERT_TRUE(backend_ota_operation_id_decode(
        "0123456789abcdef0123456789abcdef", &expected.operation_id));
    expected.component = BACKEND_OTA_COMPONENT_SCANNER0;
    strcpy(expected.catalog_name, "scanner-s3-combo-fullsize-backend");
    strcpy(expected.expected_sha256,
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    expected.expected_size = 1048576U;
    memcpy(expected.binding.uplink_mac,
        (uint8_t[]){0xAA,0xBB,0xCC,0xDD,0xEE,0x01}, 6U);
    expected.binding.uplink_boot_id = 101U;
    memcpy(expected.binding.target_mac,
        (uint8_t[]){0xAA,0xBB,0xCC,0xDD,0xEE,0x02}, 6U);
    expected.binding.target_boot_id = 202U;
    expected.binding.topology_generation = 7U;
    expected.apply_mode = BACKEND_OTA_NEWER_ONLY;
    expected.next_sequence = 0U;
    TEST_ASSERT_EQUAL_MEMORY(&expected, &envelope, sizeof(expected));

    TEST_ASSERT_EQUAL(BACKEND_OTA_COMMAND_DECODE_OK,
        backend_ota_command_decode(APPLY, strlen(APPLY), &local, &envelope));
    expected.is_apply = true;
    expected.next_sequence = 2U;
    strcpy(expected.probe_receipt_sha256,
        "fe251f806c754ce75978ea3da2dba33787f61e033423f60271449a2873365778");
    TEST_ASSERT_EQUAL_MEMORY(&expected, &envelope, sizeof(expected));
}

void test_accepted_probe_capture_and_match_own_complete_apply_binding(void)
{
    backend_ota_command_local_t local = local_scanner0();
    backend_ota_command_envelope_t probe;
    backend_ota_command_envelope_t apply;
    TEST_ASSERT_EQUAL(BACKEND_OTA_COMMAND_DECODE_OK,
        backend_ota_command_decode(PROBE, strlen(PROBE), &local, &probe));
    backend_ota_command_ack_t transition = {
        .ok = true,
        .has_operation_id = true,
        .operation_id = probe.operation_id,
        .accepted_sequence = 1U,
        .next_sequence = 2U,
        .current_component = BACKEND_OTA_COMPONENT_SCANNER0,
        .current_action = "apply",
        .terminal = false,
        .duplicate = false,
    };
    backend_ota_accepted_probe_t accepted;
    memset(&accepted, 0xA5, sizeof(accepted));
    TEST_ASSERT_TRUE(backend_ota_accepted_probe_capture(
        &probe,
        "fe251f806c754ce75978ea3da2dba33787f61e033423f60271449a2873365778",
        1U, &transition, &accepted));
    TEST_ASSERT_EQUAL_MEMORY(probe.operation_id.bytes,
        accepted.probe.operation_id.bytes, sizeof(probe.operation_id.bytes));
    TEST_ASSERT_EQUAL(BACKEND_OTA_COMPONENT_SCANNER0, accepted.probe.component);
    TEST_ASSERT_EQUAL_STRING("scanner-s3-combo-fullsize-backend",
        accepted.probe.catalog_name);
    TEST_ASSERT_EQUAL_UINT32(1048576U, accepted.probe.expected_size);
    TEST_ASSERT_EQUAL_UINT32(2U, accepted.apply_start_sequence);
    local.has_accepted_probe = true;
    local.accepted_probe = accepted;
    local.has_expected_next_sequence = true;
    local.expected_next_sequence = 2U;
    TEST_ASSERT_EQUAL(BACKEND_OTA_COMMAND_DECODE_OK,
        backend_ota_command_decode(APPLY, strlen(APPLY), &local, &apply));
    TEST_ASSERT_TRUE(backend_ota_apply_matches_accepted_probe(
        &apply, &accepted, 2U));

    transition.operation_id.bytes[15] ^= 1U;
    memset(&accepted, 0xA5, sizeof(accepted));
    TEST_ASSERT_FALSE(backend_ota_accepted_probe_capture(
        &probe,
        "fe251f806c754ce75978ea3da2dba33787f61e033423f60271449a2873365778",
        1U, &transition, &accepted));
    TEST_ASSERT_EQUAL_MEMORY(&(backend_ota_accepted_probe_t){0}, &accepted,
        sizeof(accepted));

    transition.operation_id = probe.operation_id;
    memset(&accepted, 0xA5, sizeof(accepted));
    TEST_ASSERT_FALSE(backend_ota_accepted_probe_capture(
        &probe,
        "FE251F806C754CE75978EA3DA2DBA33787F61E033423F60271449A2873365778",
        1U, &transition, &accepted));
    TEST_ASSERT_EQUAL_MEMORY(&(backend_ota_accepted_probe_t){0}, &accepted,
        sizeof(accepted));

    transition.duplicate = true;
    TEST_ASSERT_TRUE(backend_ota_accepted_probe_capture(
        &probe,
        "fe251f806c754ce75978ea3da2dba33787f61e033423f60271449a2873365778",
        1U, &transition, &accepted));
    transition.accepted_sequence = UINT32_MAX - 1U;
    transition.next_sequence = UINT32_MAX;
    memset(&accepted, 0xA5, sizeof(accepted));
    TEST_ASSERT_FALSE(backend_ota_accepted_probe_capture(
        &probe,
        "fe251f806c754ce75978ea3da2dba33787f61e033423f60271449a2873365778",
        UINT32_MAX - 1U, &transition, &accepted));
    TEST_ASSERT_EQUAL_MEMORY(&(backend_ota_accepted_probe_t){0}, &accepted,
        sizeof(accepted));
}

void test_apply_rejects_every_mutated_accepted_field_and_cursor(void)
{
    backend_ota_command_local_t local = local_scanner0();
    accept_canonical_probe_for_apply(&local, NULL);
    backend_ota_command_local_t candidate;

    for (size_t byte = 0U;
         byte < sizeof(local.accepted_probe.probe.operation_id.bytes); ++byte) {
        candidate = local;
        candidate.accepted_probe.probe.operation_id.bytes[byte] ^= 1U;
        assert_command_rejected_and_cleared(APPLY, strlen(APPLY), &candidate);
    }
    candidate = local;
    candidate.accepted_probe.probe.component = BACKEND_OTA_COMPONENT_SCANNER1;
    assert_command_rejected_and_cleared(APPLY, strlen(APPLY), &candidate);
    candidate = local;
    candidate.accepted_probe.probe.catalog_name[0] = 'x';
    assert_command_rejected_and_cleared(APPLY, strlen(APPLY), &candidate);
    for (size_t byte = 0U; byte < 32U; ++byte) {
        candidate = local;
        candidate.accepted_probe.probe.expected_sha256[byte] ^= 1U;
        assert_command_rejected_and_cleared(APPLY, strlen(APPLY), &candidate);
        candidate = local;
        candidate.accepted_probe.receipt_sha256[byte] ^= 1U;
        assert_command_rejected_and_cleared(APPLY, strlen(APPLY), &candidate);
    }
    candidate = local;
    candidate.accepted_probe.probe.expected_size++;
    assert_command_rejected_and_cleared(APPLY, strlen(APPLY), &candidate);
    for (size_t byte = 0U; byte < 6U; ++byte) {
        candidate = local;
        candidate.accepted_probe.probe.binding.uplink_mac[byte] ^= 1U;
        assert_command_rejected_and_cleared(APPLY, strlen(APPLY), &candidate);
        candidate = local;
        candidate.accepted_probe.probe.binding.target_mac[byte] ^= 1U;
        assert_command_rejected_and_cleared(APPLY, strlen(APPLY), &candidate);
    }
    candidate = local;
    candidate.accepted_probe.probe.binding.uplink_boot_id++;
    assert_command_rejected_and_cleared(APPLY, strlen(APPLY), &candidate);
    candidate = local;
    candidate.accepted_probe.probe.binding.target_boot_id++;
    assert_command_rejected_and_cleared(APPLY, strlen(APPLY), &candidate);
    candidate = local;
    candidate.accepted_probe.probe.binding.topology_generation++;
    assert_command_rejected_and_cleared(APPLY, strlen(APPLY), &candidate);
    candidate = local;
    candidate.accepted_probe.probe.apply_mode = BACKEND_OTA_SAME_VERSION_RECOVERY;
    assert_command_rejected_and_cleared(APPLY, strlen(APPLY), &candidate);
    candidate = local;
    candidate.accepted_probe.apply_start_sequence++;
    assert_command_rejected_and_cleared(APPLY, strlen(APPLY), &candidate);

    char json[2048];
    static const char *const cursor_values[] = {"1", "3", "4294967295"};
    for (size_t index = 0U;
         index < sizeof(cursor_values) / sizeof(cursor_values[0]); ++index) {
        const size_t length = build_command_json(
            json, sizeof(json), APPLY_FIELDS,
            sizeof(APPLY_FIELDS) / sizeof(APPLY_FIELDS[0]), SIZE_MAX, 13U,
            cursor_values[index], SIZE_MAX);
        assert_command_rejected_and_cleared(json, length, &local);
    }
}

void test_receipt_fixture_path_is_runtime_readable(void)
{
    FILE *fixture = fopen(BACKEND_OTA_RECEIPT_FIXTURE_PATH, "rb");
    TEST_ASSERT_NOT_NULL(fixture);
    TEST_ASSERT_EQUAL_INT(0, fclose(fixture));
}

void test_runtime_receipt_fixture_drives_both_canonical_vectors(void)
{
    receipt_fixture_t fixture;
    TEST_ASSERT_TRUE(receipt_fixture_load(BACKEND_OTA_RECEIPT_FIXTURE_PATH, &fixture));

    receipt_fixture_vector_t *const vectors[] = {&fixture.probe, &fixture.apply};
    for (size_t index = 0U; index < sizeof(vectors) / sizeof(vectors[0]); ++index) {
        receipt_fixture_vector_t *vector = vectors[index];
        const size_t expected_length = strlen(vector->preimage_utf8);
        uint8_t expected[BACKEND_OTA_RECEIPT_PREIMAGE_MAX_BYTES];
        uint8_t actual[BACKEND_OTA_RECEIPT_PREIMAGE_MAX_BYTES + 1U];
        size_t decoded_length = 0U;
        memset(actual, 0xA5, sizeof(actual));
        TEST_ASSERT_TRUE(fixture_hex_decode(
            vector->preimage_hex, expected, sizeof(expected), &decoded_length));
        TEST_ASSERT_EQUAL_UINT(expected_length, decoded_length);
        TEST_ASSERT_EQUAL_MEMORY(vector->preimage_utf8, expected, expected_length);
        TEST_ASSERT_EQUAL_CHAR('\n', vector->preimage_utf8[expected_length - 1U]);
        TEST_ASSERT_NOT_EQUAL('\n', vector->preimage_utf8[expected_length - 2U]);
        TEST_ASSERT_EQUAL_UINT(0U, backend_ota_receipt_v1_preimage(
            &vector->operation_id, &vector->command, &vector->end, actual,
            expected_length - 1U));
        for (size_t byte = 0U; byte < sizeof(actual); ++byte) {
            TEST_ASSERT_EQUAL_UINT8(0xA5U, actual[byte]);
        }
        TEST_ASSERT_EQUAL_UINT(0U, backend_ota_receipt_v1_preimage(
            &vector->operation_id, &vector->command, &vector->end, actual,
            expected_length));
        for (size_t byte = 0U; byte < sizeof(actual); ++byte) {
            TEST_ASSERT_EQUAL_UINT8(0xA5U, actual[byte]);
        }
        TEST_ASSERT_EQUAL_UINT(expected_length, backend_ota_receipt_v1_preimage(
            &vector->operation_id, &vector->command, &vector->end, actual,
            expected_length + 1U));
        TEST_ASSERT_EQUAL_MEMORY(expected, actual, expected_length);
        TEST_ASSERT_EQUAL_UINT8(0U, actual[expected_length]);
        char digest[65];
        TEST_ASSERT_TRUE(backend_ota_receipt_v1_sha256(
            &vector->operation_id, &vector->command, &vector->end, digest));
        TEST_ASSERT_EQUAL_STRING(vector->receipt_sha256, digest);
        TEST_ASSERT_TRUE(backend_ota_receipt_v1_verify(
            &vector->operation_id, &vector->command, &vector->end,
            vector->receipt_sha256));
    }
    TEST_ASSERT_EQUAL_STRING(
        fixture.probe.receipt_sha256, fixture.apply.probe_receipt_sha256);
}

static backend_ota_command_envelope_t fixture_terminal_command(
    const receipt_fixture_vector_t *vector)
{
    backend_ota_command_envelope_t command = {
        .is_apply = vector->command.is_apply,
        .has_operation_id = true,
        .operation_id = vector->operation_id,
        .component = vector->command.component,
        .expected_size = vector->command.expected_size,
        .binding = vector->command.binding,
        .apply_mode = vector->command.apply_mode,
        .next_sequence = vector->next_sequence,
    };
    (void)strcpy(command.catalog_name, vector->catalog_name);
    (void)strcpy(command.expected_sha256, vector->expected_sha256);
    if (vector->command.is_apply) {
        (void)strcpy(command.probe_receipt_sha256,
            vector->probe_receipt_sha256);
    }
    return command;
}

static backend_ota_terminal_evidence_t fixture_terminal_evidence(
    const receipt_fixture_vector_t *vector)
{
    backend_ota_terminal_evidence_t evidence = {
        .outcome = vector->command.is_apply
            ? BACKEND_OTA_TERMINAL_APPLIED : BACKEND_OTA_TERMINAL_ELIGIBLE,
        .error = BACKEND_OTA_TERMINAL_ERROR_NONE,
        .relation = FOF_VERSION_NEWER,
        .complete_image_validated = true,
        .validated_image_bytes = vector->command.expected_size,
        .image_writes = vector->end.image_writes,
        .actual_binding = {
            .component = vector->command.component,
            .component_slot = backend_ota_component_slot(
                vector->command.component),
            .target_boot_id = vector->end.actual_boot_id,
            .topology_generation = vector->end.actual_topology_generation,
        },
        .identity_exact = true,
        .command_ingress_healthy = true,
        .role_acked = true,
        .profile_correct = true,
        .radio_healthy = true,
        .rollback_clear = true,
    };
    (void)strcpy(evidence.candidate.target, vector->target);
    (void)strcpy(evidence.candidate.project, vector->project);
    (void)strcpy(evidence.candidate.hardware, vector->hardware);
    (void)strcpy(evidence.candidate.version, vector->version);
    evidence.candidate.image_size = vector->command.expected_size;
    (void)strcpy(evidence.candidate.sha256, vector->expected_sha256);
    (void)memcpy(evidence.actual_binding.target_mac, vector->end.actual_mac,
        sizeof(evidence.actual_binding.target_mac));
    return evidence;
}

static bool build_fixture_terminal(
    const receipt_fixture_vector_t *vector,
    backend_ota_command_envelope_t *command,
    backend_ota_terminal_evidence_t *evidence,
    char *body,
    size_t capacity,
    backend_ota_built_end_t *result)
{
    *command = fixture_terminal_command(vector);
    *evidence = fixture_terminal_evidence(vector);
    const backend_ota_progress_state_t progress = {
        .initialized = true,
        .stage = BACKEND_OTA_PROGRESS_VALIDATE,
        .received = vector->command.expected_size,
        .total = vector->command.expected_size,
    };
    return backend_ota_event_end_build(
        command, &progress, vector->sequence, evidence,
        body, capacity, result);
}

void test_bound_end_builder_matches_both_task_six_vectors(void)
{
    static const char *const expected_bodies[] = {
        "{\"schema\":1,\"operation_id\":\"0123456789abcdef0123456789abcdef\",\"sequence\":1,\"type\":\"backend_ota_end\",\"component\":\"scanner0\",\"catalog_name\":\"scanner-s3-combo-fullsize-backend\",\"state\":\"complete\",\"decision\":\"eligible\",\"error\":\"none\",\"image_writes\":0,\"target\":\"scanner-s3-combo-fullsize-backend\",\"project\":\"fof_backend_scanner_fullsize\",\"hardware\":\"esp32s3_n16r8_fullsize\",\"version\":\"0.2.1-backend\",\"actual_mac\":\"AA:BB:CC:DD:EE:02\",\"actual_boot_id\":202,\"actual_topology_generation\":7,\"role_healthy\":true,\"radio_healthy\":true,\"rollback_clear\":true,\"receipt_sha256\":\"fe251f806c754ce75978ea3da2dba33787f61e033423f60271449a2873365778\"}",
        "{\"schema\":1,\"operation_id\":\"0123456789abcdef0123456789abcdef\",\"sequence\":3,\"type\":\"backend_ota_end\",\"component\":\"scanner0\",\"catalog_name\":\"scanner-s3-combo-fullsize-backend\",\"state\":\"complete\",\"decision\":\"applied\",\"error\":\"none\",\"image_writes\":1048576,\"target\":\"scanner-s3-combo-fullsize-backend\",\"project\":\"fof_backend_scanner_fullsize\",\"hardware\":\"esp32s3_n16r8_fullsize\",\"version\":\"0.2.1-backend\",\"actual_mac\":\"AA:BB:CC:DD:EE:02\",\"actual_boot_id\":303,\"actual_topology_generation\":7,\"role_healthy\":true,\"radio_healthy\":true,\"rollback_clear\":true,\"receipt_sha256\":\"24b481545a5be0c3e995031b1c46b50ee7dfe150f0a895a7b6997b309cfa9038\"}",
    };
    receipt_fixture_t fixture;
    TEST_ASSERT_TRUE(receipt_fixture_load(BACKEND_OTA_RECEIPT_FIXTURE_PATH, &fixture));
    const receipt_fixture_vector_t *const vectors[] = {
        &fixture.probe, &fixture.apply,
    };
    for (size_t index = 0U; index < 2U; ++index) {
        backend_ota_command_envelope_t command;
        backend_ota_terminal_evidence_t evidence;
        backend_ota_built_end_t result;
        char body[1024];
        TEST_ASSERT_TRUE(build_fixture_terminal(
            vectors[index], &command, &evidence, body, sizeof(body), &result));
        TEST_ASSERT_EQUAL_STRING(expected_bodies[index], body);
        TEST_ASSERT_EQUAL_UINT(strlen(expected_bodies[index]), result.body_length);
        TEST_ASSERT_EQUAL_STRING(vectors[index]->receipt_sha256,
            result.receipt_sha256);
    }
}

static void assert_fixture_zeroed(const receipt_fixture_t *fixture)
{
    const uint8_t *const bytes = (const uint8_t *)fixture;
    for (size_t index = 0U; index < sizeof(*fixture); ++index) {
        TEST_ASSERT_EQUAL_UINT8(0U, bytes[index]);
    }
}

static void assert_fixture_path_rejected_and_cleared(const char *path)
{
    receipt_fixture_t poisoned;
    memset(&poisoned, 0xA5, sizeof(poisoned));
    TEST_ASSERT_FALSE(receipt_fixture_load(path, &poisoned));
    assert_fixture_zeroed(&poisoned);
}

static void assert_fixture_text_rejected_and_cleared(
    const char *text, size_t length)
{
    char path[] = "/tmp/fof_receipt_fixture_XXXXXX";
    const int descriptor = mkstemp(path);
    if (descriptor < 0) {
        TEST_FAIL_MESSAGE("mkstemp failed");
        return;
    }
    const ssize_t written = write(descriptor, text, length);
    const int close_result = close(descriptor);
    receipt_fixture_t poisoned;
    memset(&poisoned, 0xA5, sizeof(poisoned));
    const bool accepted = written == (ssize_t)length && close_result == 0 &&
        receipt_fixture_load(path, &poisoned);
    const int unlink_result = unlink(path);

    TEST_ASSERT_EQUAL_INT((int)length, (int)written);
    TEST_ASSERT_EQUAL_INT(0, close_result);
    TEST_ASSERT_EQUAL_INT(0, unlink_result);
    TEST_ASSERT_FALSE(accepted);
    assert_fixture_zeroed(&poisoned);
}

typedef enum {
    FIXTURE_EMPTY_PREIMAGE = 0,
    FIXTURE_DOUBLE_FINAL_LF,
    FIXTURE_MISMATCHED_APPLY_PREIMAGE_HEX,
} receipt_fixture_mutation_t;

static void assert_runtime_fixture_mutation_rejected(
    receipt_fixture_mutation_t mutation)
{
    char json[RECEIPT_FIXTURE_FILE_CAPACITY];
    FILE *file = fopen(BACKEND_OTA_RECEIPT_FIXTURE_PATH, "rb");
    TEST_ASSERT_NOT_NULL(file);
    size_t length = fread(json, 1U, sizeof(json) - 5U, file);
    const int close_result = fclose(file);
    TEST_ASSERT_EQUAL_INT(0, close_result);
    TEST_ASSERT_GREATER_THAN(0U, length);
    json[length] = '\0';

    const char *const field = mutation == FIXTURE_MISMATCHED_APPLY_PREIMAGE_HEX
        ? "\"preimage_hex\": \"" : "\"preimage_utf8\": \"";
    char *marker = strstr(json, field);
    TEST_ASSERT_NOT_NULL(marker);
    if (mutation == FIXTURE_MISMATCHED_APPLY_PREIMAGE_HEX) {
        marker = strstr(marker + strlen(field), field);
        TEST_ASSERT_NOT_NULL(marker);
    }
    char *const value = marker + strlen(field);
    char *const end = strchr(value, '"');
    TEST_ASSERT_NOT_NULL(end);

    if (mutation == FIXTURE_EMPTY_PREIMAGE) {
        const size_t removed = (size_t)(end - value);
        memmove(value, end, length - (size_t)(end - json) + 1U);
        length -= removed;
        char *const hex_marker = strstr(json, "\"preimage_hex\": \"");
        TEST_ASSERT_NOT_NULL(hex_marker);
        char *const hex_value = hex_marker + strlen("\"preimage_hex\": \"");
        char *const hex_end = strchr(hex_value, '"');
        TEST_ASSERT_NOT_NULL(hex_end);
        const size_t hex_removed = (size_t)(hex_end - hex_value);
        memmove(hex_value, hex_end,
            length - (size_t)(hex_end - json) + 1U);
        length -= hex_removed;
    } else if (mutation == FIXTURE_DOUBLE_FINAL_LF) {
        TEST_ASSERT_GREATER_OR_EQUAL(2, (int)(end - value));
        TEST_ASSERT_EQUAL_CHAR('\\', end[-2]);
        TEST_ASSERT_EQUAL_CHAR('n', end[-1]);
        memmove(end + 2, end, length - (size_t)(end - json) + 1U);
        end[0] = '\\';
        end[1] = 'n';
        length += 2U;
        char *const hex_marker = strstr(json, "\"preimage_hex\": \"");
        TEST_ASSERT_NOT_NULL(hex_marker);
        char *const hex_value = hex_marker + strlen("\"preimage_hex\": \"");
        char *const hex_end = strchr(hex_value, '"');
        TEST_ASSERT_NOT_NULL(hex_end);
        memmove(hex_end + 2, hex_end,
            length - (size_t)(hex_end - json) + 1U);
        hex_end[0] = '0';
        hex_end[1] = 'a';
        length += 2U;
    } else {
        TEST_ASSERT_TRUE(*value >= '0' && *value <= 'f');
        *value = *value == '0' ? '1' : '0';
    }
    assert_fixture_text_rejected_and_cleared(json, length);
}

void test_receipt_fixture_load_is_transactional_and_rejects_malformed_sources(void)
{
    assert_fixture_path_rejected_and_cleared(NULL);
    assert_fixture_path_rejected_and_cleared("/definitely-missing-receipt.json");
    assert_fixture_path_rejected_and_cleared("/");
    assert_fixture_path_rejected_and_cleared("/dev/null");

    static const char malformed[] = "{";
    static const char wrong_type[] =
        "{\"schema\":\"1\",\"probe\":{},\"apply\":{}}";
    static const char unknown_key[] =
        "{\"schema\":1,\"probe\":{},\"apply\":{},\"unknown\":1}";
    assert_fixture_text_rejected_and_cleared(malformed, strlen(malformed));
    assert_fixture_text_rejected_and_cleared(wrong_type, strlen(wrong_type));
    assert_fixture_text_rejected_and_cleared(unknown_key, strlen(unknown_key));
    assert_runtime_fixture_mutation_rejected(FIXTURE_EMPTY_PREIMAGE);
    assert_runtime_fixture_mutation_rejected(FIXTURE_DOUBLE_FINAL_LF);
    /* The late apply-vector failure proves a valid probe is never published. */
    assert_runtime_fixture_mutation_rejected(FIXTURE_MISMATCHED_APPLY_PREIMAGE_HEX);
}

void test_receipt_allows_only_explicit_early_probe_failed_empty_identity(void)
{
    receipt_fixture_t fixture;
    TEST_ASSERT_TRUE(receipt_fixture_load(BACKEND_OTA_RECEIPT_FIXTURE_PATH, &fixture));
    receipt_fixture_vector_t candidate = fixture.probe;
    fixture_vector_rebind(&candidate);
    candidate.end.state = "failed";
    candidate.end.decision = "rejected";
    candidate.end.error = "internal";
    candidate.end.image_writes = 0U;
    candidate.end.target = "";
    candidate.end.project = "";
    candidate.end.hardware = "";
    candidate.end.version = "";
    candidate.end.failed_before_identity = true;
    uint8_t preimage[BACKEND_OTA_RECEIPT_PREIMAGE_MAX_BYTES];
    TEST_ASSERT_GREATER_THAN(0U, backend_ota_receipt_v1_preimage(
        &candidate.operation_id, &candidate.command, &candidate.end,
        preimage, sizeof(preimage)));

    candidate.end.failed_before_identity = false;
    TEST_ASSERT_EQUAL_UINT(0U, backend_ota_receipt_v1_preimage(
        &candidate.operation_id, &candidate.command, &candidate.end,
        preimage, sizeof(preimage)));
    candidate.end.failed_before_identity = true;
    candidate.end.target = "scanner-s3-combo-fullsize-backend";
    TEST_ASSERT_EQUAL_UINT(0U, backend_ota_receipt_v1_preimage(
        &candidate.operation_id, &candidate.command, &candidate.end,
        preimage, sizeof(preimage)));
    candidate.end.target = "";
    candidate.command.is_apply = true;
    TEST_ASSERT_EQUAL_UINT(0U, backend_ota_receipt_v1_preimage(
        &candidate.operation_id, &candidate.command, &candidate.end,
        preimage, sizeof(preimage)));
    candidate.command.is_apply = false;
    candidate.end.image_writes = 1U;
    TEST_ASSERT_EQUAL_UINT(0U, backend_ota_receipt_v1_preimage(
        &candidate.operation_id, &candidate.command, &candidate.end,
        preimage, sizeof(preimage)));
    candidate.end.image_writes = 0U;
    candidate.end.state = "rolled_back";
    candidate.end.decision = "rolled_back";
    candidate.end.error = "rollback";
    TEST_ASSERT_EQUAL_UINT(0U, backend_ota_receipt_v1_preimage(
        &candidate.operation_id, &candidate.command, &candidate.end,
        preimage, sizeof(preimage)));
}

static void fixture_vector_rebind(receipt_fixture_vector_t *vector)
{
    vector->command.catalog_name = vector->catalog_name;
    vector->command.expected_sha256 = vector->expected_sha256;
    vector->end.state = vector->state;
    vector->end.decision = vector->decision;
    vector->end.error = vector->error;
    vector->end.target = vector->target;
    vector->end.project = vector->project;
    vector->end.hardware = vector->hardware;
    vector->end.version = vector->version;
}

static void fixture_vector_set_failed(receipt_fixture_vector_t *vector)
{
    strcpy(vector->state, "failed");
    strcpy(vector->decision, "rejected");
    strcpy(vector->error, "internal");
    vector->end.failed_before_identity = false;
}

static void assert_canonical_receipts_differ(
    receipt_fixture_vector_t left,
    receipt_fixture_vector_t right)
{
    fixture_vector_rebind(&left);
    fixture_vector_rebind(&right);
    uint8_t left_preimage[BACKEND_OTA_RECEIPT_PREIMAGE_MAX_BYTES];
    uint8_t right_preimage[BACKEND_OTA_RECEIPT_PREIMAGE_MAX_BYTES];
    char left_digest[65];
    char right_digest[65];
    const size_t left_length = backend_ota_receipt_v1_preimage(
        &left.operation_id, &left.command, &left.end,
        left_preimage, sizeof(left_preimage));
    const size_t right_length = backend_ota_receipt_v1_preimage(
        &right.operation_id, &right.command, &right.end,
        right_preimage, sizeof(right_preimage));
    TEST_ASSERT_GREATER_THAN(0U, left_length);
    TEST_ASSERT_GREATER_THAN(0U, right_length);
    const size_t shared_length = left_length < right_length
        ? left_length : right_length;
    TEST_ASSERT_TRUE(left_length != right_length ||
        memcmp(left_preimage, right_preimage, shared_length) != 0);
    TEST_ASSERT_TRUE(backend_ota_receipt_v1_sha256(
        &left.operation_id, &left.command, &left.end, left_digest));
    TEST_ASSERT_TRUE(backend_ota_receipt_v1_sha256(
        &right.operation_id, &right.command, &right.end, right_digest));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(left_digest, right_digest));
}

static void assert_canonical_receipt_mutation(
    const receipt_fixture_vector_t *source,
    receipt_fixture_vector_t candidate)
{
    assert_canonical_receipts_differ(*source, candidate);
}

void test_runtime_fixture_receipt_binds_every_preimage_field_with_canonical_mutations(void)
{
    receipt_fixture_t fixture;
    TEST_ASSERT_TRUE(receipt_fixture_load(BACKEND_OTA_RECEIPT_FIXTURE_PATH, &fixture));
    const receipt_fixture_vector_t *const vectors[] = {&fixture.probe, &fixture.apply};
    for (size_t index = 0U; index < sizeof(vectors) / sizeof(vectors[0]); ++index) {
        const receipt_fixture_vector_t *source = vectors[index];
        receipt_fixture_vector_t candidate;

        for (size_t byte = 0U; byte < sizeof(source->operation_id.bytes); ++byte) {
            candidate = *source;
            candidate.operation_id.bytes[byte] ^= 1U;
            assert_canonical_receipt_mutation(source, candidate);
        }

        candidate = *source;
        candidate.command.is_apply = !source->command.is_apply;
        if (candidate.command.is_apply) {
            strcpy(candidate.state, "complete");
            strcpy(candidate.decision, "applied");
            strcpy(candidate.error, "none");
            candidate.end.image_writes = 1U;
        } else {
            strcpy(candidate.state, "complete");
            strcpy(candidate.decision, "eligible");
            strcpy(candidate.error, "none");
            candidate.end.image_writes = 0U;
        }
        assert_canonical_receipt_mutation(source, candidate);

        candidate = *source;
        candidate.command.apply_mode = source->command.apply_mode == BACKEND_OTA_NEWER_ONLY
            ? BACKEND_OTA_SAME_VERSION_RECOVERY : BACKEND_OTA_NEWER_ONLY;
        assert_canonical_receipt_mutation(source, candidate);

        candidate = *source;
        candidate.command.component = BACKEND_OTA_COMPONENT_SCANNER1;
        assert_canonical_receipt_mutation(source, candidate);

        /* Catalog and component form the smallest canonical catalog tuple. */
        candidate = *source;
        candidate.command.component = BACKEND_OTA_COMPONENT_UPLINK;
        strcpy(candidate.catalog_name, "uplink-s3-fullsize-backend");
        assert_canonical_receipt_mutation(source, candidate);

        candidate = *source;
        candidate.expected_sha256[0] = candidate.expected_sha256[0] == 'a' ? 'b' : 'a';
        assert_canonical_receipt_mutation(source, candidate);
        candidate = *source;
        candidate.command.expected_size++;
        assert_canonical_receipt_mutation(source, candidate);

        for (size_t byte = 0U; byte < 6U; ++byte) {
            candidate = *source;
            candidate.command.binding.uplink_mac[byte] ^=
                byte == 0U ? 2U : 1U;
            assert_canonical_receipt_mutation(source, candidate);
            candidate = *source;
            candidate.command.binding.target_mac[byte] ^=
                byte == 0U ? 2U : 1U;
            assert_canonical_receipt_mutation(source, candidate);
            candidate = *source;
            candidate.end.actual_mac[byte] ^= byte == 0U ? 2U : 1U;
            assert_canonical_receipt_mutation(source, candidate);
        }

        candidate = *source;
        candidate.command.binding.uplink_boot_id++;
        assert_canonical_receipt_mutation(source, candidate);
        candidate = *source;
        candidate.command.binding.target_boot_id++;
        assert_canonical_receipt_mutation(source, candidate);
        candidate = *source;
        candidate.command.binding.topology_generation++;
        assert_canonical_receipt_mutation(source, candidate);

        /* Terminal state and decision cannot change independently; this is the
         * smallest canonical terminal tuple for the fixture's command type. */
        candidate = *source;
        if (!source->command.is_apply) {
            strcpy(candidate.state, "no_update");
            strcpy(candidate.decision, "no_update");
        } else {
            fixture_vector_set_failed(&candidate);
        }
        assert_canonical_receipt_mutation(source, candidate);

        receipt_fixture_vector_t failed = *source;
        fixture_vector_set_failed(&failed);
        candidate = failed;
        strcpy(candidate.error, "download");
        assert_canonical_receipts_differ(failed, candidate);
        candidate = failed;
        candidate.end.image_writes++;
        assert_canonical_receipts_differ(failed, candidate);

        candidate = *source;
        candidate.target[0] = candidate.target[0] == 'x' ? 'y' : 'x';
        assert_canonical_receipt_mutation(source, candidate);
        candidate = *source;
        candidate.project[0] = candidate.project[0] == 'x' ? 'y' : 'x';
        assert_canonical_receipt_mutation(source, candidate);
        candidate = *source;
        candidate.hardware[0] = candidate.hardware[0] == 'x' ? 'y' : 'x';
        assert_canonical_receipt_mutation(source, candidate);
        candidate = *source;
        candidate.version[0] = candidate.version[0] == 'x' ? 'y' : 'x';
        assert_canonical_receipt_mutation(source, candidate);

        candidate = *source;
        candidate.end.actual_boot_id++;
        assert_canonical_receipt_mutation(source, candidate);
        candidate = *source;
        candidate.end.actual_topology_generation++;
        assert_canonical_receipt_mutation(source, candidate);

        candidate = failed;
        candidate.end.role_healthy = !failed.end.role_healthy;
        assert_canonical_receipts_differ(failed, candidate);
        candidate = failed;
        candidate.end.radio_healthy = !failed.end.radio_healthy;
        assert_canonical_receipts_differ(failed, candidate);
        candidate = failed;
        candidate.end.rollback_clear = !failed.end.rollback_clear;
        assert_canonical_receipts_differ(failed, candidate);
    }
}

static void assert_receipt_arguments_invalid(
    const backend_ota_operation_id_t *operation_id,
    const backend_ota_receipt_command_t *command,
    const backend_ota_receipt_end_t *end)
{
    uint8_t preimage[BACKEND_OTA_RECEIPT_PREIMAGE_MAX_BYTES];
    memset(preimage, 0xA5, sizeof(preimage));
    TEST_ASSERT_EQUAL_UINT(0U, backend_ota_receipt_v1_preimage(
        operation_id, command, end, preimage, sizeof(preimage)));
    for (size_t index = 0U; index < sizeof(preimage); ++index) {
        TEST_ASSERT_EQUAL_UINT8(0xA5U, preimage[index]);
    }

    char digest[65];
    memset(digest, 0xA5, sizeof(digest));
    TEST_ASSERT_FALSE(backend_ota_receipt_v1_sha256(
        operation_id, command, end, digest));
    for (size_t index = 0U; index < sizeof(digest); ++index) {
        TEST_ASSERT_EQUAL_UINT8(0U, (uint8_t)digest[index]);
    }
}

static void assert_bound_receipt_invalid(receipt_fixture_vector_t candidate)
{
    fixture_vector_rebind(&candidate);
    assert_receipt_arguments_invalid(
        &candidate.operation_id, &candidate.command, &candidate.end);
}

void test_runtime_fixture_receipt_rejects_invalid_inputs_and_clears_digest(void)
{
    receipt_fixture_t fixture;
    TEST_ASSERT_TRUE(receipt_fixture_load(BACKEND_OTA_RECEIPT_FIXTURE_PATH, &fixture));
    const receipt_fixture_vector_t *const vectors[] = {&fixture.probe, &fixture.apply};
    for (size_t index = 0U; index < sizeof(vectors) / sizeof(vectors[0]); ++index) {
        const receipt_fixture_vector_t *source = vectors[index];
        receipt_fixture_vector_t candidate = *source;

        candidate.command.apply_mode = (backend_ota_apply_mode_t)99;
        assert_bound_receipt_invalid(candidate);
        candidate = *source;
        candidate.command.component = (backend_ota_component_t)99;
        assert_bound_receipt_invalid(candidate);
        candidate = *source;
        candidate.catalog_name[0] = 'x';
        assert_bound_receipt_invalid(candidate);

        candidate = *source;
        candidate.expected_sha256[0] = 'A';
        assert_bound_receipt_invalid(candidate);
        candidate = *source;
        candidate.expected_sha256[63] = '\0';
        assert_bound_receipt_invalid(candidate);
        candidate = *source;
        candidate.expected_sha256[0] = 'g';
        assert_bound_receipt_invalid(candidate);
        candidate = *source;
        candidate.command.expected_size = 0U;
        assert_bound_receipt_invalid(candidate);
        candidate = *source;
        candidate.command.expected_size = UINT32_MAX;
        assert_bound_receipt_invalid(candidate);

        candidate = *source;
        candidate.command.binding.uplink_boot_id = 0U;
        assert_bound_receipt_invalid(candidate);
        candidate = *source;
        candidate.command.binding.target_boot_id = 0U;
        assert_bound_receipt_invalid(candidate);
        candidate = *source;
        candidate.command.binding.topology_generation = 0U;
        assert_bound_receipt_invalid(candidate);

        candidate = *source;
        candidate.state[0] = 'x';
        assert_bound_receipt_invalid(candidate);
        candidate = *source;
        candidate.decision[0] = 'x';
        assert_bound_receipt_invalid(candidate);
        candidate = *source;
        candidate.error[0] = 'x';
        assert_bound_receipt_invalid(candidate);
        candidate = *source;
        candidate.end.image_writes = source->command.is_apply ? 0U : 1U;
        assert_bound_receipt_invalid(candidate);

        candidate = *source;
        candidate.target[0] = '\0';
        assert_bound_receipt_invalid(candidate);
        candidate = *source;
        candidate.target[0] = '\n';
        assert_bound_receipt_invalid(candidate);
        candidate = *source;
        candidate.end.actual_boot_id = 0U;
        assert_bound_receipt_invalid(candidate);
        candidate = *source;
        candidate.end.actual_topology_generation = 0U;
        assert_bound_receipt_invalid(candidate);
        candidate = *source;
        candidate.end.role_healthy = false;
        assert_bound_receipt_invalid(candidate);
        candidate = *source;
        candidate.end.radio_healthy = false;
        assert_bound_receipt_invalid(candidate);
        candidate = *source;
        candidate.end.rollback_clear = false;
        assert_bound_receipt_invalid(candidate);
        candidate = *source;
        candidate.end.failed_before_identity = true;
        assert_bound_receipt_invalid(candidate);

        char too_long[66];
        memset(too_long, 'a', 65U);
        too_long[65] = '\0';
        candidate = *source;
        fixture_vector_rebind(&candidate);
        candidate.end.target = too_long;
        assert_receipt_arguments_invalid(
            &candidate.operation_id, &candidate.command, &candidate.end);

        candidate = *source;
        fixture_vector_rebind(&candidate);
        candidate.command.expected_sha256 = NULL;
        assert_receipt_arguments_invalid(
            &candidate.operation_id, &candidate.command, &candidate.end);
        candidate = *source;
        fixture_vector_rebind(&candidate);
        candidate.end.error = NULL;
        assert_receipt_arguments_invalid(
            &candidate.operation_id, &candidate.command, &candidate.end);

        assert_receipt_arguments_invalid(NULL, &source->command, &source->end);
        assert_receipt_arguments_invalid(&source->operation_id, NULL, &source->end);
        assert_receipt_arguments_invalid(&source->operation_id, &source->command, NULL);
        TEST_ASSERT_EQUAL_UINT(0U, backend_ota_receipt_v1_preimage(
            &source->operation_id, &source->command, &source->end, NULL,
            BACKEND_OTA_RECEIPT_PREIMAGE_MAX_BYTES));

        TEST_ASSERT_FALSE(backend_ota_receipt_v1_verify(
            &source->operation_id, &source->command, &source->end,
            "FE251F806C754CE75978EA3DA2DBA33787F61E033423F60271449A2873365778"));
        TEST_ASSERT_FALSE(backend_ota_receipt_v1_verify(
            &source->operation_id, &source->command, &source->end, "abc"));
        TEST_ASSERT_FALSE(backend_ota_receipt_v1_verify(
            &source->operation_id, &source->command, &source->end,
            "fe251f806c754ce75978ea3da2dba33787f61e033423f60271449a287336577g"));
    }
}

void test_fullsize_decoder_rejects_extra_duplicate_ble_and_bad_binding(void)
{
    backend_ota_command_envelope_t envelope;
    backend_ota_command_local_t local = local_scanner0();
    char extra[sizeof(PROBE) + 32U];
    snprintf(extra, sizeof(extra), "%.*s,\"extra\":1}",
        (int)strlen(PROBE) - 1, PROBE);
    TEST_ASSERT_NOT_EQUAL(BACKEND_OTA_COMMAND_DECODE_OK,
        backend_ota_command_decode(extra, strlen(extra), &local, &envelope));
    TEST_ASSERT_NOT_EQUAL(BACKEND_OTA_COMMAND_DECODE_OK,
        backend_ota_command_decode(
            "{\"schema\":1,\"schema\":1}", 23U, &local, &envelope));
    TEST_ASSERT_NOT_EQUAL(BACKEND_OTA_COMMAND_DECODE_OK,
        backend_ota_command_decode(
            "{\"type\":\"ble_investigate\"}", 26U, &local, &envelope));
    local.binding.topology_generation++;
    TEST_ASSERT_EQUAL(BACKEND_OTA_COMMAND_DECODE_BINDING,
        backend_ota_command_decode(PROBE, strlen(PROBE), &local, &envelope));
}

void test_command_shape_type_and_boundary_tables_are_exhaustive(void)
{
    const struct {
        const command_json_field_t *fields;
        size_t count;
    } command_sets[] = {
        {PROBE_FIELDS, sizeof(PROBE_FIELDS) / sizeof(PROBE_FIELDS[0])},
        {APPLY_FIELDS, sizeof(APPLY_FIELDS) / sizeof(APPLY_FIELDS[0])},
    };
    static const char *const wrong_types[] = {"null", "true", "\"1\""};
    char json[4096];
    backend_ota_command_local_t local = local_scanner0();

    for (size_t set = 0U; set < sizeof(command_sets) / sizeof(command_sets[0]); ++set) {
        local = local_scanner0();
        if (set == 1U) {
            accept_canonical_probe_for_apply(&local, NULL);
        }
        const command_json_field_t *fields = command_sets[set].fields;
        const size_t count = command_sets[set].count;
        const size_t valid_length = build_command_json(
            json, sizeof(json), fields, count, SIZE_MAX, SIZE_MAX, NULL, SIZE_MAX);
        backend_ota_command_envelope_t output;
        TEST_ASSERT_GREATER_THAN(0U, valid_length);
        TEST_ASSERT_EQUAL(BACKEND_OTA_COMMAND_DECODE_OK,
            backend_ota_command_decode(json, valid_length, &local, &output));
        const size_t complete_duplicate = build_command_json(
            json, sizeof(json), fields, count, SIZE_MAX, SIZE_MAX, NULL, 1U);
        assert_command_rejected_and_cleared(
            json, complete_duplicate, &local);
        for (size_t index = 0U; index < count; ++index) {
            const size_t missing_length = build_command_json(
                json, sizeof(json), fields, count, index, SIZE_MAX, NULL, SIZE_MAX);
            TEST_ASSERT_GREATER_THAN(0U, missing_length);
            assert_command_rejected_and_cleared(json, missing_length, &local);

            const size_t duplicate_length = build_command_json(
                json, sizeof(json), fields, count, (index + 1U) % count,
                SIZE_MAX, NULL, index);
            TEST_ASSERT_GREATER_THAN(0U, duplicate_length);
            assert_command_rejected_and_cleared(json, duplicate_length, &local);

            for (size_t type = 0U;
                 type < sizeof(wrong_types) / sizeof(wrong_types[0]); ++type) {
                const size_t wrong_length = build_command_json(
                    json, sizeof(json), fields, count, SIZE_MAX, index,
                    wrong_types[type], SIZE_MAX);
                TEST_ASSERT_GREATER_THAN(0U, wrong_length);
                assert_command_rejected_and_cleared(json, wrong_length, &local);
            }
        }
        TEST_ASSERT_GREATER_THAN(0U, build_command_json(
            json, sizeof(json), fields, count, SIZE_MAX, SIZE_MAX, NULL, SIZE_MAX));
        const size_t extra_length = append_unknown_json_field(json, sizeof(json));
        TEST_ASSERT_GREATER_THAN(0U, extra_length);
        assert_command_rejected_and_cleared(json, extra_length, &local);
    }

    local = local_scanner0();

    static const struct {
        size_t field;
        const char *value;
    } numeric_rejections[] = {
        {0U, "0"}, {0U, "-1"}, {0U, "1.0"}, {0U, "1e0"}, {0U, "4294967296"},
        {6U, "0"}, {6U, "-1"}, {6U, "1.0"}, {6U, "1e0"}, {6U, "4294967296"},
        {8U, "0"}, {8U, "4294967296"},
        {10U, "0"}, {10U, "4294967296"},
        {11U, "0"}, {11U, "4294967296"},
        {13U, "-1"}, {13U, "1.0"}, {13U, "1e0"}, {13U, "4294967296"},
    };
    for (size_t index = 0U;
         index < sizeof(numeric_rejections) / sizeof(numeric_rejections[0]); ++index) {
        const size_t length = build_command_json(
            json, sizeof(json), PROBE_FIELDS,
            sizeof(PROBE_FIELDS) / sizeof(PROBE_FIELDS[0]), SIZE_MAX,
            numeric_rejections[index].field, numeric_rejections[index].value, SIZE_MAX);
        assert_command_rejected_and_cleared(json, length, &local);
    }
    const size_t max_sequence = build_command_json(
        json, sizeof(json), PROBE_FIELDS,
        sizeof(PROBE_FIELDS) / sizeof(PROBE_FIELDS[0]), SIZE_MAX, 13U,
        "4294967295", SIZE_MAX);
    assert_command_rejected_and_cleared(json, max_sequence, &local);

    static const struct {
        size_t field;
        const char *value;
    } string_rejections[] = {
        {1U, "\"0123456789ABCDEF0123456789ABCDEF\""},
        {1U, "\"0123456789abcdef0123456789abcde\""},
        {1U, "\"0123456789abcdef0123456789abcdef0\""},
        {1U, "\"0123456789abcdef0123456789abcdeg\""},
        {2U, "\"ble_investigate\""}, {3U, "\"scanner2\""},
        {4U, "\"not-the-catalog\""}, {12U, "\"invalid\""},
        {5U, "\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\""},
        {5U, "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\""},
        {5U, "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\""},
        {5U, "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag\""},
        {7U, "\"aa:bb:cc:dd:ee:01\""}, {7U, "\"AA-BB-CC-DD-EE-01\""},
        {7U, "\"AA:BB:CC:DD:EE:1\""}, {7U, "\"AA:BB:CC:DD:EE:001\""},
        {9U, "\"AA:BB:CC:DD:EE:0G\""},
    };
    for (size_t index = 0U;
         index < sizeof(string_rejections) / sizeof(string_rejections[0]); ++index) {
        const size_t length = build_command_json(
            json, sizeof(json), PROBE_FIELDS,
            sizeof(PROBE_FIELDS) / sizeof(PROBE_FIELDS[0]), SIZE_MAX,
            string_rejections[index].field, string_rejections[index].value, SIZE_MAX);
        assert_command_rejected_and_cleared(json, length, &local);
    }
    static const char bad_receipt[] =
        "\"FE251F806C754CE75978EA3DA2DBA33787F61E033423F60271449A2873365778\"";
    local = local_scanner0();
    accept_canonical_probe_for_apply(&local, NULL);
    const size_t receipt_length = build_command_json(
        json, sizeof(json), APPLY_FIELDS,
        sizeof(APPLY_FIELDS) / sizeof(APPLY_FIELDS[0]), SIZE_MAX, 14U,
        bad_receipt, SIZE_MAX);
    assert_command_rejected_and_cleared(json, receipt_length, &local);
    const size_t short_receipt = build_command_json(
        json, sizeof(json), APPLY_FIELDS,
        sizeof(APPLY_FIELDS) / sizeof(APPLY_FIELDS[0]), SIZE_MAX, 14U,
        "\"fe251f806c754ce75978ea3da2dba33787f61e033423f60271449a287336577\"",
        SIZE_MAX);
    assert_command_rejected_and_cleared(json, short_receipt, &local);
    const size_t long_receipt = build_command_json(
        json, sizeof(json), APPLY_FIELDS,
        sizeof(APPLY_FIELDS) / sizeof(APPLY_FIELDS[0]), SIZE_MAX, 14U,
        "\"fe251f806c754ce75978ea3da2dba33787f61e033423f60271449a28733657780\"",
        SIZE_MAX);
    assert_command_rejected_and_cleared(json, long_receipt, &local);
}

void test_command_local_binding_catalog_capacity_and_whitespace_tables(void)
{
    char json[BACKEND_OTA_COMMAND_MAX_JSON + 2U];
    backend_ota_command_local_t local = local_scanner0();
    const size_t command_length = build_command_json(
        json, sizeof(json), PROBE_FIELDS,
        sizeof(PROBE_FIELDS) / sizeof(PROBE_FIELDS[0]), SIZE_MAX, SIZE_MAX,
        NULL, SIZE_MAX);
    TEST_ASSERT_GREATER_THAN(0U, command_length);
    const size_t leading_space = BACKEND_OTA_COMMAND_MAX_JSON - command_length;
    memmove(json + leading_space, json, command_length + 1U);
    memset(json, ' ', leading_space);
    TEST_ASSERT_EQUAL(BACKEND_OTA_COMMAND_DECODE_OK,
        backend_ota_command_decode(json, BACKEND_OTA_COMMAND_MAX_JSON, &local,
            &(backend_ota_command_envelope_t){0}));
    json[BACKEND_OTA_COMMAND_MAX_JSON] = ' ';
    assert_command_rejected_and_cleared(
        json, BACKEND_OTA_COMMAND_MAX_JSON + 1U, &local);

    const uint8_t original_uplink[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
    const uint8_t original_target[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};
    for (size_t index = 0U; index < 6U; ++index) {
        local.binding.uplink_mac[index] ^= 1U;
        assert_command_rejected_and_cleared(PROBE, strlen(PROBE), &local);
        local.binding.uplink_mac[index] = original_uplink[index];
        local.binding.target_mac[index] ^= 1U;
        assert_command_rejected_and_cleared(PROBE, strlen(PROBE), &local);
        local.binding.target_mac[index] = original_target[index];
    }
    local.binding.uplink_boot_id++;
    assert_command_rejected_and_cleared(PROBE, strlen(PROBE), &local);
    local = local_scanner0();
    local.binding.target_boot_id++;
    assert_command_rejected_and_cleared(PROBE, strlen(PROBE), &local);
    local = local_scanner0();
    local.binding.topology_generation++;
    assert_command_rejected_and_cleared(PROBE, strlen(PROBE), &local);

    local = local_scanner0();
    local.component = BACKEND_OTA_COMPONENT_SCANNER1;
    assert_command_rejected_and_cleared(PROBE, strlen(PROBE), &local);
    local = local_scanner0();
    local.target = "wrong";
    assert_command_rejected_and_cleared(PROBE, strlen(PROBE), &local);
    local = local_scanner0();
    local.project = "wrong";
    assert_command_rejected_and_cleared(PROBE, strlen(PROBE), &local);
    local = local_scanner0();
    local.hardware = "wrong";
    assert_command_rejected_and_cleared(PROBE, strlen(PROBE), &local);
    local = local_scanner0();
    local.catalog_name = "wrong";
    assert_command_rejected_and_cleared(PROBE, strlen(PROBE), &local);
    local = local_scanner0();
    local.catalog_name = NULL;
    assert_command_rejected_and_cleared(PROBE, strlen(PROBE), &local);
    local = local_scanner0();
    local.target = NULL;
    assert_command_rejected_and_cleared(PROBE, strlen(PROBE), &local);
    local = local_scanner0();
    local.project = NULL;
    assert_command_rejected_and_cleared(PROBE, strlen(PROBE), &local);
    local = local_scanner0();
    local.hardware = NULL;
    assert_command_rejected_and_cleared(PROBE, strlen(PROBE), &local);
    local = local_scanner0();
    local.max_expected_size = 1048575U;
    assert_command_rejected_and_cleared(PROBE, strlen(PROBE), &local);
    local = local_scanner0();
    local.max_expected_size = FOF_BACKEND_SCANNER_CACHE_CAPACITY + 1U;
    assert_command_rejected_and_cleared(PROBE, strlen(PROBE), &local);
    local = local_scanner0();
    const size_t cache_exact = build_command_json(
        json, sizeof(json), PROBE_FIELDS,
        sizeof(PROBE_FIELDS) / sizeof(PROBE_FIELDS[0]), SIZE_MAX, 6U,
        "3145728", SIZE_MAX);
    TEST_ASSERT_EQUAL(BACKEND_OTA_COMMAND_DECODE_OK,
        backend_ota_command_decode(json, cache_exact, &local,
            &(backend_ota_command_envelope_t){0}));
    const size_t cache_plus_one = build_command_json(
        json, sizeof(json), PROBE_FIELDS,
        sizeof(PROBE_FIELDS) / sizeof(PROBE_FIELDS[0]), SIZE_MAX, 6U,
        "3145729", SIZE_MAX);
    assert_command_rejected_and_cleared(json, cache_plus_one, &local);

    command_json_field_t uplink_fields[
        sizeof(PROBE_FIELDS) / sizeof(PROBE_FIELDS[0])];
    memcpy(uplink_fields, PROBE_FIELDS, sizeof(uplink_fields));
    uplink_fields[3].value = "\"uplink\"";
    uplink_fields[4].value = "\"uplink-s3-fullsize-backend\"";
    uplink_fields[6].value = "2097152";
    uplink_fields[9].value = "\"AA:BB:CC:DD:EE:01\"";
    uplink_fields[10].value = "101";
    local = local_uplink();
    const size_t uplink_exact = build_command_json(
        json, sizeof(json), uplink_fields,
        sizeof(uplink_fields) / sizeof(uplink_fields[0]), SIZE_MAX, SIZE_MAX,
        NULL, SIZE_MAX);
    backend_ota_command_envelope_t uplink_output;
    TEST_ASSERT_EQUAL(BACKEND_OTA_COMMAND_DECODE_OK,
        backend_ota_command_decode(json, uplink_exact, &local, &uplink_output));
    backend_ota_command_envelope_t expected_uplink = {0};
    expected_uplink.has_operation_id = true;
    TEST_ASSERT_TRUE(backend_ota_operation_id_decode(
        "0123456789abcdef0123456789abcdef", &expected_uplink.operation_id));
    expected_uplink.component = BACKEND_OTA_COMPONENT_UPLINK;
    strcpy(expected_uplink.catalog_name, "uplink-s3-fullsize-backend");
    strcpy(expected_uplink.expected_sha256,
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    expected_uplink.expected_size = 2097152U;
    memcpy(expected_uplink.binding.uplink_mac, local.binding.uplink_mac, 6U);
    expected_uplink.binding.uplink_boot_id = local.binding.uplink_boot_id;
    memcpy(expected_uplink.binding.target_mac, local.binding.target_mac, 6U);
    expected_uplink.binding.target_boot_id = local.binding.target_boot_id;
    expected_uplink.binding.topology_generation =
        local.binding.topology_generation;
    expected_uplink.apply_mode = BACKEND_OTA_NEWER_ONLY;
    TEST_ASSERT_EQUAL_MEMORY(
        &expected_uplink, &uplink_output, sizeof(expected_uplink));
    uplink_fields[6].value = "2097153";
    const size_t uplink_plus_one = build_command_json(
        json, sizeof(json), uplink_fields,
        sizeof(uplink_fields) / sizeof(uplink_fields[0]), SIZE_MAX, SIZE_MAX,
        NULL, SIZE_MAX);
    assert_command_rejected_and_cleared(json, uplink_plus_one, &local);

    local = local_scanner0();
    const size_t wrong_catalog = build_command_json(
        json, sizeof(json), PROBE_FIELDS,
        sizeof(PROBE_FIELDS) / sizeof(PROBE_FIELDS[0]), SIZE_MAX, 4U,
        "\"wrong\"", SIZE_MAX);
    local.catalog_name = "wrong";
    assert_command_rejected_and_cleared(json, wrong_catalog, &local);
}

void test_command_binding_requires_nonzero_unicast_and_component_relationships(void)
{
    static const struct {
        const char *uplink_mac;
        const char *target_mac;
        bool accepted;
    } scanner_cases[] = {
        {"02:00:00:00:00:01", "02:00:00:00:00:02", true},
        {"00:00:00:00:00:00", "02:00:00:00:00:02", false},
        {"03:00:00:00:00:01", "02:00:00:00:00:02", false},
        {"02:00:00:00:00:01", "00:00:00:00:00:00", false},
        {"02:00:00:00:00:01", "03:00:00:00:00:02", false},
        {"02:00:00:00:00:01", "02:00:00:00:00:01", false},
    };
    char json[2048];
    for (size_t index = 0U;
         index < sizeof(scanner_cases) / sizeof(scanner_cases[0]); ++index) {
        command_json_field_t fields[
            sizeof(PROBE_FIELDS) / sizeof(PROBE_FIELDS[0])];
        memcpy(fields, PROBE_FIELDS, sizeof(fields));
        char uplink_text[20];
        char target_text[20];
        snprintf(uplink_text, sizeof(uplink_text), "\"%s\"",
            scanner_cases[index].uplink_mac);
        snprintf(target_text, sizeof(target_text), "\"%s\"",
            scanner_cases[index].target_mac);
        fields[7].value = uplink_text;
        fields[9].value = target_text;
        const size_t length = build_command_json(
            json, sizeof(json), fields,
            sizeof(fields) / sizeof(fields[0]), SIZE_MAX, SIZE_MAX, NULL,
            SIZE_MAX);
        backend_ota_command_local_t local = local_scanner0();
        TEST_ASSERT_TRUE(fixture_mac(
            scanner_cases[index].uplink_mac, local.binding.uplink_mac));
        TEST_ASSERT_TRUE(fixture_mac(
            scanner_cases[index].target_mac, local.binding.target_mac));
        if (scanner_cases[index].accepted) {
            TEST_ASSERT_EQUAL(BACKEND_OTA_COMMAND_DECODE_OK,
                backend_ota_command_decode(
                    json, length, &local,
                    &(backend_ota_command_envelope_t){0}));
        } else {
            assert_command_rejected_and_cleared(json, length, &local);
        }
    }

    command_json_field_t uplink_fields[
        sizeof(PROBE_FIELDS) / sizeof(PROBE_FIELDS[0])];
    memcpy(uplink_fields, PROBE_FIELDS, sizeof(uplink_fields));
    uplink_fields[3].value = "\"uplink\"";
    uplink_fields[4].value = "\"uplink-s3-fullsize-backend\"";
    uplink_fields[6].value = "2097152";
    uplink_fields[9].value = "\"AA:BB:CC:DD:EE:02\"";
    uplink_fields[10].value = "101";
    size_t length = build_command_json(
        json, sizeof(json), uplink_fields,
        sizeof(uplink_fields) / sizeof(uplink_fields[0]), SIZE_MAX, SIZE_MAX,
        NULL, SIZE_MAX);
    backend_ota_command_local_t local = local_uplink();
    memcpy(local.binding.target_mac,
        (uint8_t[]){0xAA,0xBB,0xCC,0xDD,0xEE,0x02}, 6U);
    assert_command_rejected_and_cleared(json, length, &local);

    uplink_fields[9].value = "\"AA:BB:CC:DD:EE:01\"";
    uplink_fields[10].value = "202";
    length = build_command_json(
        json, sizeof(json), uplink_fields,
        sizeof(uplink_fields) / sizeof(uplink_fields[0]), SIZE_MAX, SIZE_MAX,
        NULL, SIZE_MAX);
    local = local_uplink();
    local.binding.target_boot_id = 202U;
    assert_command_rejected_and_cleared(json, length, &local);
}

void test_fullsize_probe_mode_matrix_is_explicit(void)
{
    TEST_ASSERT_EQUAL(BACKEND_OTA_PROBE_ELIGIBLE,
        backend_ota_fullsize_probe_mode(FOF_VERSION_NEWER, BACKEND_OTA_NEWER_ONLY));
    TEST_ASSERT_EQUAL(BACKEND_OTA_PROBE_ELIGIBLE,
        backend_ota_fullsize_probe_mode(FOF_VERSION_NEWER, BACKEND_OTA_SAME_VERSION_RECOVERY));
    TEST_ASSERT_EQUAL(BACKEND_OTA_PROBE_NO_UPDATE,
        backend_ota_fullsize_probe_mode(FOF_VERSION_EQUAL, BACKEND_OTA_NEWER_ONLY));
    TEST_ASSERT_EQUAL(BACKEND_OTA_PROBE_ELIGIBLE,
        backend_ota_fullsize_probe_mode(FOF_VERSION_EQUAL, BACKEND_OTA_SAME_VERSION_RECOVERY));
    TEST_ASSERT_EQUAL(BACKEND_OTA_PROBE_REJECTED,
        backend_ota_fullsize_probe_mode(FOF_VERSION_OLDER, BACKEND_OTA_NEWER_ONLY));
    TEST_ASSERT_EQUAL(BACKEND_OTA_PROBE_REJECTED,
        backend_ota_fullsize_probe_mode(FOF_VERSION_UNORDERED, BACKEND_OTA_SAME_VERSION_RECOVERY));
    TEST_ASSERT_EQUAL(BACKEND_OTA_PROBE_REJECTED,
        backend_ota_fullsize_probe_mode(
            FOF_VERSION_NEWER, (backend_ota_apply_mode_t)99));
}

void test_apply_requires_exact_explicitly_accepted_probe_receipt(void)
{
    backend_ota_command_envelope_t envelope;
    backend_ota_command_local_t local = local_scanner0();
    TEST_ASSERT_NOT_EQUAL(BACKEND_OTA_COMMAND_DECODE_OK,
        backend_ota_command_decode(APPLY, strlen(APPLY), &local, &envelope));
    TEST_ASSERT_EQUAL_MEMORY(&(backend_ota_command_envelope_t){0}, &envelope,
        sizeof(envelope));
    accept_canonical_probe_for_apply(&local, NULL);
    TEST_ASSERT_EQUAL(BACKEND_OTA_COMMAND_DECODE_OK,
        backend_ota_command_decode(APPLY, strlen(APPLY), &local, &envelope));

    local.has_accepted_probe = false;
    assert_command_rejected_and_cleared(APPLY, strlen(APPLY), &local);
    local = local_scanner0();
    accept_canonical_probe_for_apply(&local, NULL);
    local.has_expected_next_sequence = false;
    assert_command_rejected_and_cleared(APPLY, strlen(APPLY), &local);
    local = local_scanner0();
    accept_canonical_probe_for_apply(&local, NULL);
    local.accepted_probe.receipt_sha256[0] ^= 1U;
    assert_command_rejected_and_cleared(APPLY, strlen(APPLY), &local);
    local = local_scanner0();
    accept_canonical_probe_for_apply(&local, NULL);
    assert_command_rejected_and_cleared(PROBE, strlen(PROBE), &local);
}

void test_exact_eight_key_ack_binds_operation_sequence_component_and_action(void)
{
    backend_ota_operation_id_t id;
    backend_ota_command_ack_t ack;
    TEST_ASSERT_TRUE(backend_ota_operation_id_decode(
        "0123456789abcdef0123456789abcdef", &id));
    static const char body[] =
        "{\"ok\":true,\"operation_id\":\"0123456789abcdef0123456789abcdef\","
        "\"accepted_sequence\":3,\"next_sequence\":4,\"current_component\":\"scanner0\","
        "\"current_action\":\"probe\",\"terminal\":false,\"duplicate\":false}";
    TEST_ASSERT_TRUE(backend_ota_command_ack_decode(
        body, strlen(body), &id, 3U, BACKEND_OTA_COMPONENT_SCANNER0,
        "probe", false, &ack));
    TEST_ASSERT_EQUAL_UINT32(4U, ack.next_sequence);
    static const char extra_body[] =
        "{\"ok\":true,\"operation_id\":\"0123456789abcdef0123456789abcdef\","
        "\"accepted_sequence\":3,\"next_sequence\":4,\"current_component\":\"scanner0\","
        "\"current_action\":\"probe\",\"terminal\":false,\"duplicate\":false,\"x\":1}";
    TEST_ASSERT_FALSE(backend_ota_command_ack_decode(
        extra_body, strlen(extra_body), &id, 3U, BACKEND_OTA_COMPONENT_SCANNER0,
        "probe", false, &ack));
}

void test_ack_shape_binding_terminal_and_type_tables_are_exhaustive(void)
{
    backend_ota_operation_id_t id;
    TEST_ASSERT_TRUE(backend_ota_operation_id_decode(
        "0123456789abcdef0123456789abcdef", &id));
    char json[2048];
    const size_t count = sizeof(ACK_FIELDS) / sizeof(ACK_FIELDS[0]);
    const size_t valid_length = build_command_json(
        json, sizeof(json), ACK_FIELDS, count, SIZE_MAX, SIZE_MAX, NULL, SIZE_MAX);
    backend_ota_command_ack_t output;
    TEST_ASSERT_TRUE(backend_ota_command_ack_decode(
        json, valid_length, &id, 3U, BACKEND_OTA_COMPONENT_SCANNER0,
        "probe", false, &output));
    TEST_ASSERT_FALSE(output.duplicate);
    const size_t duplicate_true = build_command_json(
        json, sizeof(json), ACK_FIELDS, count, SIZE_MAX, 7U, "true", SIZE_MAX);
    TEST_ASSERT_TRUE(backend_ota_command_ack_decode(
        json, duplicate_true, &id, 3U, BACKEND_OTA_COMPONENT_SCANNER0,
        "probe", false, &output));
    TEST_ASSERT_TRUE(output.duplicate);
    const size_t complete_key_duplicate = build_command_json(
        json, sizeof(json), ACK_FIELDS, count, SIZE_MAX, SIZE_MAX, NULL, 1U);
    assert_ack_rejected_and_cleared(
        json, complete_key_duplicate, &id, 3U,
        BACKEND_OTA_COMPONENT_SCANNER0, "probe", false);
    command_json_field_t terminal_duplicate_fields[
        sizeof(ACK_FIELDS) / sizeof(ACK_FIELDS[0])];
    memcpy(terminal_duplicate_fields, ACK_FIELDS, sizeof(terminal_duplicate_fields));
    terminal_duplicate_fields[6].value = "true";
    terminal_duplicate_fields[7].value = "true";
    const size_t terminal_duplicate = build_command_json(
        json, sizeof(json), terminal_duplicate_fields,
        sizeof(terminal_duplicate_fields) / sizeof(terminal_duplicate_fields[0]),
        SIZE_MAX, SIZE_MAX, NULL, SIZE_MAX);
    TEST_ASSERT_TRUE(backend_ota_command_ack_decode(
        json, terminal_duplicate, &id, 3U, BACKEND_OTA_COMPONENT_SCANNER0,
        "probe", true, &output));
    TEST_ASSERT_TRUE(output.terminal);
    TEST_ASSERT_TRUE(output.duplicate);

    static const char *const wrong_types[] = {"null", "1", "\"true\""};
    for (size_t index = 0U; index < count; ++index) {
        const size_t missing = build_command_json(
            json, sizeof(json), ACK_FIELDS, count, index, SIZE_MAX, NULL, SIZE_MAX);
        assert_ack_rejected_and_cleared(
            json, missing, &id, 3U, BACKEND_OTA_COMPONENT_SCANNER0, "probe", false);
        const size_t duplicate = build_command_json(
            json, sizeof(json), ACK_FIELDS, count, (index + 1U) % count,
            SIZE_MAX, NULL, index);
        assert_ack_rejected_and_cleared(
            json, duplicate, &id, 3U, BACKEND_OTA_COMPONENT_SCANNER0, "probe", false);
        for (size_t type = 0U;
             type < sizeof(wrong_types) / sizeof(wrong_types[0]); ++type) {
            const size_t wrong = build_command_json(
                json, sizeof(json), ACK_FIELDS, count, SIZE_MAX, index,
                wrong_types[type], SIZE_MAX);
            assert_ack_rejected_and_cleared(
                json, wrong, &id, 3U, BACKEND_OTA_COMPONENT_SCANNER0, "probe", false);
        }
    }
    TEST_ASSERT_GREATER_THAN(0U, build_command_json(
        json, sizeof(json), ACK_FIELDS, count, SIZE_MAX, SIZE_MAX, NULL, SIZE_MAX));
    const size_t extra = append_unknown_json_field(json, sizeof(json));
    assert_ack_rejected_and_cleared(
        json, extra, &id, 3U, BACKEND_OTA_COMPONENT_SCANNER0, "probe", false);

    static const struct {
        size_t field;
        const char *value;
    } mismatches[] = {
        {0U, "false"},
        {1U, "\"1123456789abcdef0123456789abcdef\""},
        {1U, "\"0123456789ABCDEF0123456789abcdef\""},
        {1U, "\"0123456789abcdef0123456789abcde\""},
        {1U, "\"0123456789abcdef0123456789abcdef0\""},
        {1U, "\"0123456789abcdef0123456789abcdeg\""},
        {2U, "2"}, {2U, "4294967296"},
        {3U, "3"}, {3U, "4294967296"},
        {4U, "\"scanner1\""}, {5U, "\"apply\""},
        {6U, "true"},
    };
    for (size_t index = 0U; index < sizeof(mismatches) / sizeof(mismatches[0]); ++index) {
        const size_t length = build_command_json(
            json, sizeof(json), ACK_FIELDS, count, SIZE_MAX,
            mismatches[index].field, mismatches[index].value, SIZE_MAX);
        assert_ack_rejected_and_cleared(
            json, length, &id, 3U, BACKEND_OTA_COMPONENT_SCANNER0, "probe", false);
    }
    TEST_ASSERT_GREATER_THAN(0U, build_command_json(
        json, sizeof(json), ACK_FIELDS, count, SIZE_MAX, SIZE_MAX, NULL, SIZE_MAX));
    assert_ack_rejected_and_cleared(
        json, strlen(json), &id, UINT32_MAX,
        BACKEND_OTA_COMPONENT_SCANNER0, "probe", false);
    assert_ack_rejected_and_cleared(
        json, valid_length, &id, 3U, BACKEND_OTA_COMPONENT_SCANNER0, "invalid", false);
    assert_ack_rejected_and_cleared(
        json, valid_length, &id, 3U, (backend_ota_component_t)99, "probe", false);
}

void test_progress_state_rejects_regression_and_total_change(void)
{
    backend_ota_progress_state_t state = {0};
    TEST_ASSERT_TRUE(backend_ota_progress_accept(
        &state, BACKEND_OTA_PROGRESS_DOWNLOAD, 10U, 100U, 0U));
    TEST_ASSERT_TRUE(backend_ota_progress_accept(
        &state, BACKEND_OTA_PROGRESS_DOWNLOAD, 20U, 100U, 1U));
    TEST_ASSERT_FALSE(backend_ota_progress_accept(
        &state, BACKEND_OTA_PROGRESS_DOWNLOAD, 19U, 100U, 1U));
    TEST_ASSERT_FALSE(backend_ota_progress_accept(
        &state, BACKEND_OTA_PROGRESS_VALIDATE, 20U, 101U, 1U));
    TEST_ASSERT_FALSE(backend_ota_progress_accept(
        &state, BACKEND_OTA_PROGRESS_METADATA, 20U, 100U, 1U));
    backend_ota_progress_state_t zero = {0};
    TEST_ASSERT_TRUE(backend_ota_progress_accept(
        &zero, BACKEND_OTA_PROGRESS_METADATA, 0U, 0U, 0U));
    TEST_ASSERT_FALSE(backend_ota_progress_accept(
        &zero, BACKEND_OTA_PROGRESS_METADATA, 1U, 0U, 0U));
}

void test_canonical_event_encoders_and_capacity_boundary(void)
{
    backend_ota_operation_id_t id;
    TEST_ASSERT_TRUE(backend_ota_operation_id_decode(
        "0123456789abcdef0123456789abcdef", &id));
    const backend_ota_event_prefix_t prefix = {
        .has_operation_id = true, .operation_id = id, .is_apply = false, .sequence = 1U,
        .component = BACKEND_OTA_COMPONENT_SCANNER0,
        .catalog_name = "scanner-s3-combo-fullsize-backend",
    };
    char body[1024];
    const size_t begin = backend_ota_event_begin_encode(&prefix,body,sizeof(body));
    TEST_ASSERT_GREATER_THAN(0U,begin);
    TEST_ASSERT_EQUAL_STRING(
        "{\"schema\":1,\"operation_id\":\"0123456789abcdef0123456789abcdef\",\"sequence\":1,\"type\":\"backend_ota_begin\",\"component\":\"scanner0\",\"catalog_name\":\"scanner-s3-combo-fullsize-backend\"}",body);
    char tight[256];
    TEST_ASSERT_EQUAL_UINT(0U,backend_ota_event_begin_encode(&prefix,tight,begin));
    TEST_ASSERT_EQUAL_UINT(begin,backend_ota_event_begin_encode(&prefix,tight,begin+1U));
    backend_ota_progress_state_t state={0};
    const backend_ota_progress_event_t progress={.prefix=prefix,.stage=BACKEND_OTA_PROGRESS_DOWNLOAD,.received=1U,.total=2U,.retry_count=0U};
    const size_t progress_length = backend_ota_event_progress_encode(
        &state, &progress, body, sizeof(body));
    TEST_ASSERT_GREATER_THAN(0U, progress_length);
    TEST_ASSERT_EQUAL_STRING(
        "{\"schema\":1,\"operation_id\":\"0123456789abcdef0123456789abcdef\",\"sequence\":1,\"type\":\"backend_ota_progress\",\"component\":\"scanner0\",\"catalog_name\":\"scanner-s3-combo-fullsize-backend\",\"stage\":\"download\",\"received\":1,\"total\":2,\"retry_count\":0}",
        body);
    const backend_ota_progress_state_t committed = state;
    char progress_tight[512];
    TEST_ASSERT_EQUAL_UINT(0U, backend_ota_event_progress_encode(
        &state, &progress, progress_tight, progress_length - 1U));
    TEST_ASSERT_EQUAL_MEMORY(&committed, &state, sizeof(state));
    TEST_ASSERT_EQUAL_UINT(0U, backend_ota_event_progress_encode(
        &state, &progress, progress_tight, progress_length));
    TEST_ASSERT_EQUAL_MEMORY(&committed, &state, sizeof(state));
    TEST_ASSERT_EQUAL_UINT(progress_length, backend_ota_event_progress_encode(
        &state, &progress, progress_tight, progress_length + 1U));
    TEST_ASSERT_EQUAL_UINT(0U, backend_ota_event_progress_encode(
        &state, &progress, body, 1U));
    TEST_ASSERT_EQUAL_MEMORY(&committed, &state, sizeof(state));
}


void test_event_context_uses_explicit_presence_and_action_stage_rules(void)
{
    backend_ota_operation_id_t zero;
    TEST_ASSERT_TRUE(backend_ota_operation_id_decode(
        "00000000000000000000000000000000", &zero));
    TEST_ASSERT_TRUE(backend_ota_operation_id_is_zero(&zero));
    backend_ota_event_prefix_t prefix = {
        .has_operation_id = false, .operation_id = zero, .is_apply = false,
        .sequence = 1U, .component = BACKEND_OTA_COMPONENT_SCANNER0,
        .catalog_name = "scanner-s3-combo-fullsize-backend",
    };
    char body[512];
    TEST_ASSERT_EQUAL_UINT(0U, backend_ota_event_begin_encode(&prefix, body, sizeof(body)));
    prefix.has_operation_id = true;
    TEST_ASSERT_GREATER_THAN(0U, backend_ota_event_begin_encode(&prefix, body, sizeof(body)));
    backend_ota_progress_state_t state = {0};
    backend_ota_progress_event_t progress = {
        .prefix = prefix, .stage = BACKEND_OTA_PROGRESS_REBOOT_WAIT,
        .received = 1U, .total = 1U,
    };
    TEST_ASSERT_EQUAL_UINT(0U, backend_ota_event_progress_encode(&state,&progress,body,sizeof(body)));
    progress.prefix.is_apply = true;
    progress.prefix.component = BACKEND_OTA_COMPONENT_UPLINK;
    progress.prefix.catalog_name = "uplink-s3-fullsize-backend";
    progress.stage = BACKEND_OTA_PROGRESS_UART_RELAY;
    TEST_ASSERT_EQUAL_UINT(0U, backend_ota_event_progress_encode(&state,&progress,body,sizeof(body)));
}


void test_progress_stage_and_prefix_boundary_table(void)
{
    backend_ota_operation_id_t id;
    TEST_ASSERT_TRUE(backend_ota_operation_id_decode(
        "0123456789abcdef0123456789abcdef", &id));
    const backend_ota_event_prefix_t scanner = {
        .has_operation_id = true, .operation_id = id, .is_apply = false,
        .sequence = 1U, .component = BACKEND_OTA_COMPONENT_SCANNER0,
        .catalog_name = "scanner-s3-combo-fullsize-backend",
    };
    char body[512];
    for (backend_ota_progress_stage_t stage = BACKEND_OTA_PROGRESS_METADATA;
         stage <= BACKEND_OTA_PROGRESS_CONVERGENCE; ++stage) {
        backend_ota_progress_state_t state = {0};
        backend_ota_progress_event_t event = {
            .prefix = scanner, .stage = stage, .received = 0U, .total = 0U,
        };
        const size_t encoded = backend_ota_event_progress_encode(
            &state, &event, body, sizeof(body));
        if (stage == BACKEND_OTA_PROGRESS_REBOOT_WAIT) {
            TEST_ASSERT_EQUAL_UINT(0U, encoded);
        } else {
            TEST_ASSERT_GREATER_THAN(0U, encoded);
        }
    }
    backend_ota_progress_state_t state = {0};
    backend_ota_progress_event_t event = {
        .prefix = scanner, .stage = BACKEND_OTA_PROGRESS_DOWNLOAD,
        .received = 1U, .total = 0U,
    };
    TEST_ASSERT_EQUAL_UINT(0U,
        backend_ota_event_progress_encode(&state, &event, body, sizeof(body)));
    event.prefix.sequence = UINT32_MAX;
    TEST_ASSERT_EQUAL_UINT(0U,
        backend_ota_event_progress_encode(&state, &event, body, sizeof(body)));
    event.prefix.sequence = 1U;
    event.prefix.catalog_name = "wrong";
    TEST_ASSERT_EQUAL_UINT(0U,
        backend_ota_event_progress_encode(&state, &event, body, sizeof(body)));
}

#else

void test_lite_command_client_retains_numeric_operation_id_boundary(void)
{
    backend_ota_operation_id_t operation_id = UINT32_C(7);
    TEST_ASSERT_EQUAL_UINT(4U, sizeof(operation_id));
    TEST_ASSERT_EQUAL_UINT32(7U, operation_id);
}

#endif

int main(void)
{
    UNITY_BEGIN();
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    BACKEND_RUN_TEST(test_fullsize_decoder_requires_exact_amended_probe_and_apply_shapes);
    BACKEND_RUN_TEST(test_accepted_probe_capture_and_match_own_complete_apply_binding);
    BACKEND_RUN_TEST(test_apply_rejects_every_mutated_accepted_field_and_cursor);
    BACKEND_RUN_TEST(test_receipt_fixture_path_is_runtime_readable);
    BACKEND_RUN_TEST(test_runtime_receipt_fixture_drives_both_canonical_vectors);
    BACKEND_RUN_TEST(test_bound_end_builder_matches_both_task_six_vectors);
    BACKEND_RUN_TEST(
        test_receipt_fixture_load_is_transactional_and_rejects_malformed_sources);
    BACKEND_RUN_TEST(test_receipt_allows_only_explicit_early_probe_failed_empty_identity);
    BACKEND_RUN_TEST(
        test_runtime_fixture_receipt_binds_every_preimage_field_with_canonical_mutations);
    BACKEND_RUN_TEST(
        test_runtime_fixture_receipt_rejects_invalid_inputs_and_clears_digest);
    BACKEND_RUN_TEST(test_fullsize_decoder_rejects_extra_duplicate_ble_and_bad_binding);
    BACKEND_RUN_TEST(test_command_shape_type_and_boundary_tables_are_exhaustive);
    BACKEND_RUN_TEST(test_command_local_binding_catalog_capacity_and_whitespace_tables);
    BACKEND_RUN_TEST(
        test_command_binding_requires_nonzero_unicast_and_component_relationships);
    BACKEND_RUN_TEST(test_fullsize_probe_mode_matrix_is_explicit);
    BACKEND_RUN_TEST(test_apply_requires_exact_explicitly_accepted_probe_receipt);
    BACKEND_RUN_TEST(test_exact_eight_key_ack_binds_operation_sequence_component_and_action);
    BACKEND_RUN_TEST(test_ack_shape_binding_terminal_and_type_tables_are_exhaustive);
    BACKEND_RUN_TEST(test_progress_state_rejects_regression_and_total_change);
    BACKEND_RUN_TEST(test_canonical_event_encoders_and_capacity_boundary);
    BACKEND_RUN_TEST(test_event_context_uses_explicit_presence_and_action_stage_rules);
    BACKEND_RUN_TEST(test_progress_stage_and_prefix_boundary_table);
#else
    BACKEND_RUN_TEST(test_lite_command_client_retains_numeric_operation_id_boundary);
#endif
    return UNITY_END();
}
