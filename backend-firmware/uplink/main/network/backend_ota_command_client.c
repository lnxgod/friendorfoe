#include "backend_ota_command_client.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "backend_json_reader.h"
#include "firmware_image_contract.h"

#define BACKEND_OTA_PROBE_KEY_COUNT 14U
#define BACKEND_OTA_APPLY_KEY_COUNT 15U
#define BACKEND_OTA_COMMAND_TOKENS 40U

static void mac_text(const uint8_t mac[6], char out[18]);
static const char *component_name(backend_ota_component_t component);

#if !defined(UNIT_TESTING)
typedef struct {
    bool is_apply;
    backend_ota_component_t component;
    const char *catalog_name;
    const char *expected_sha256;
    uint32_t expected_size;
    backend_ota_command_binding_t binding;
    backend_ota_apply_mode_t apply_mode;
} backend_ota_receipt_command_t;

typedef struct {
    const char *state;
    const char *decision;
    const char *error;
    bool failed_before_identity;
    uint32_t image_writes;
    const char *target;
    const char *project;
    const char *hardware;
    const char *version;
    uint8_t actual_mac[6];
    uint32_t actual_boot_id;
    uint32_t actual_topology_generation;
    bool role_healthy;
    bool radio_healthy;
    bool rollback_clear;
} backend_ota_receipt_end_t;
#define BACKEND_OTA_RECEIPT_LINKAGE static
#else
#define BACKEND_OTA_RECEIPT_LINKAGE
#endif

BACKEND_OTA_RECEIPT_LINKAGE size_t backend_ota_receipt_v1_preimage(
    const backend_ota_operation_id_t *operation_id,
    const backend_ota_receipt_command_t *command,
    const backend_ota_receipt_end_t *end,
    uint8_t *out,
    size_t capacity);
BACKEND_OTA_RECEIPT_LINKAGE bool backend_ota_receipt_v1_sha256(
    const backend_ota_operation_id_t *operation_id,
    const backend_ota_receipt_command_t *command,
    const backend_ota_receipt_end_t *end,
    char out_sha256[65]);

static bool find_field(
    const char *json,
    const backend_json_token_t *tokens,
    size_t count,
    const char *name,
    size_t *out)
{
    return backend_json_object_find(json, tokens, count, 0U, name, out);
}

static bool copy_string(
    const char *json,
    const backend_json_token_t *tokens,
    size_t count,
    const char *name,
    char *out,
    size_t capacity)
{
    size_t index = 0U;
    return find_field(json, tokens, count, name, &index) &&
           backend_json_copy_string(json, &tokens[index], out, capacity);
}

static bool lower_hex(const char *value, size_t expected)
{
    if (value == NULL || strlen(value) != expected) {
        return false;
    }
    for (size_t index = 0U; index < expected; ++index) {
        if (!((value[index] >= '0' && value[index] <= '9') ||
              (value[index] >= 'a' && value[index] <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool ascii_identity(const char *value, size_t max_length)
{
    if (value == NULL) {
        return false;
    }
    const size_t length = strlen(value);
    if (length == 0U || length > max_length) {
        return false;
    }
    if (!((value[0] >= 'a' && value[0] <= 'z') ||
          (value[0] >= 'A' && value[0] <= 'Z') ||
          (value[0] >= '0' && value[0] <= '9'))) {
        return false;
    }
    for (size_t index = 1U; index < length; ++index) {
        const char character = value[index];
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') ||
              character == '.' || character == '_' || character == '-')) {
            return false;
        }
    }
    return true;
}

static bool parse_mac(const char *text, uint8_t out[6])
{
    if (text == NULL || out == NULL || strlen(text) != 17U) {
        return false;
    }
    for (size_t index = 0U; index < 6U; ++index) {
        const size_t offset = index * 3U;
        unsigned value = 0U;
        for (size_t digit = 0U; digit < 2U; ++digit) {
            const char character = text[offset + digit];
            unsigned nibble = 0U;
            if (character >= '0' && character <= '9') {
                nibble = (unsigned)(character - '0');
            } else if (character >= 'A' && character <= 'F') {
                nibble = (unsigned)(character - 'A') + 10U;
            } else {
                return false;
            }
            value = value * 16U + nibble;
        }
        if (index < 5U && text[offset + 2U] != ':') {
            return false;
        }
        out[index] = (uint8_t)value;
    }
    return true;
}

static bool parse_nonzero_u32(
    const char *json, const backend_json_token_t *token, uint32_t *out)
{
    uint64_t value = 0U;
    return out != NULL && backend_json_get_u64(json, token, &value) &&
           value != 0U && value <= UINT32_MAX &&
           ((*out = (uint32_t)value), true);
}

static bool component_from_name(
    const char *name, backend_ota_component_t *out)
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

static bool mode_from_name(const char *name, backend_ota_apply_mode_t *out)
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

static bool binding_equals(
    const backend_ota_command_binding_t *left,
    const backend_ota_command_binding_t *right)
{
    return left != NULL && right != NULL &&
           memcmp(left->uplink_mac, right->uplink_mac, 6U) == 0 &&
           left->uplink_boot_id == right->uplink_boot_id &&
           memcmp(left->target_mac, right->target_mac, 6U) == 0 &&
           left->target_boot_id == right->target_boot_id &&
           left->topology_generation == right->topology_generation;
}

static bool lower_hex_decode_32(const char *text, uint8_t output[32])
{
    if (!lower_hex(text, 64U) || output == NULL) {
        return false;
    }
    for (size_t index = 0U; index < 32U; ++index) {
        const char high = text[index * 2U];
        const char low = text[index * 2U + 1U];
        const uint8_t high_value = (uint8_t)(
            high <= '9' ? high - '0' : high - 'a' + 10);
        const uint8_t low_value = (uint8_t)(
            low <= '9' ? low - '0' : low - 'a' + 10);
        output[index] = (uint8_t)((high_value << 4U) | low_value);
    }
    return true;
}

static bool bytes_equal_constant_time(
    const uint8_t *left,
    const uint8_t *right,
    size_t length)
{
    if (left == NULL || right == NULL) {
        return false;
    }
    unsigned difference = 0U;
    for (size_t index = 0U; index < length; ++index) {
        difference |= (unsigned)(left[index] ^ right[index]);
    }
    return difference == 0U;
}

static bool fixed_string_terminated(const char *value, size_t capacity)
{
    return value != NULL && memchr(value, '\0', capacity) != NULL;
}

static uint32_t component_capacity(backend_ota_component_t component)
{
    return component == BACKEND_OTA_COMPONENT_UPLINK
        ? FOF_BACKEND_UPLINK_APP_CAPACITY
        : FOF_BACKEND_SCANNER_CACHE_CAPACITY;
}

static bool action_is_valid(const char *action)
{
    return action != NULL &&
           (strcmp(action, "probe") == 0 || strcmp(action, "apply") == 0);
}

static bool mac_is_nonzero_unicast(const uint8_t mac[6])
{
    static const uint8_t zero[6] = {0};
    return mac != NULL && memcmp(mac, zero, sizeof(zero)) != 0 &&
           (mac[0] & 1U) == 0U;
}

static bool binding_semantics_valid(
    backend_ota_component_t component,
    const backend_ota_command_binding_t *binding)
{
    if (binding == NULL || component_name(component) == NULL ||
        !mac_is_nonzero_unicast(binding->uplink_mac) ||
        !mac_is_nonzero_unicast(binding->target_mac) ||
        binding->uplink_boot_id == 0U || binding->target_boot_id == 0U ||
        binding->topology_generation == 0U) {
        return false;
    }
    const bool same_mac = memcmp(
        binding->uplink_mac, binding->target_mac, 6U) == 0;
    if (component == BACKEND_OTA_COMPONENT_UPLINK) {
        return same_mac &&
               binding->target_boot_id == binding->uplink_boot_id;
    }
    return !same_mac;
}

static bool probe_source_valid(const backend_ota_command_envelope_t *probe)
{
    if (probe == NULL || probe->is_apply || !probe->has_operation_id ||
        !fixed_string_terminated(probe->catalog_name,
            sizeof(probe->catalog_name)) ||
        !fixed_string_terminated(probe->expected_sha256,
            sizeof(probe->expected_sha256)) ||
        !fixed_string_terminated(probe->probe_receipt_sha256,
            sizeof(probe->probe_receipt_sha256)) ||
        probe->probe_receipt_sha256[0] != '\0' ||
        !lower_hex(probe->expected_sha256, 64U) ||
        probe->expected_size == 0U ||
        probe->expected_size > component_capacity(probe->component) ||
        !binding_semantics_valid(probe->component, &probe->binding) ||
        (probe->apply_mode != BACKEND_OTA_NEWER_ONLY &&
         probe->apply_mode != BACKEND_OTA_SAME_VERSION_RECOVERY) ||
        probe->next_sequence == UINT32_MAX) {
        return false;
    }
    const char *const catalog = probe->component == BACKEND_OTA_COMPONENT_UPLINK
        ? FOF_BACKEND_UPLINK_TARGET : FOF_BACKEND_SCANNER_TARGET;
    return strcmp(probe->catalog_name, catalog) == 0;
}

bool backend_ota_accepted_probe_capture(
    const backend_ota_command_envelope_t *probe,
    const char accepted_receipt_sha256[65],
    uint32_t probe_end_sequence,
    const backend_ota_command_ack_t *transition_ack,
    backend_ota_accepted_probe_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    backend_ota_accepted_probe_t candidate;
    memset(&candidate, 0, sizeof(candidate));
    if (!probe_source_valid(probe) || transition_ack == NULL ||
        !fixed_string_terminated(transition_ack->current_action,
            sizeof(transition_ack->current_action)) ||
        !transition_ack->ok || !transition_ack->has_operation_id ||
        !backend_ota_operation_id_equal(
            &transition_ack->operation_id, &probe->operation_id) ||
        transition_ack->accepted_sequence != probe_end_sequence ||
        probe_end_sequence <= probe->next_sequence ||
        probe_end_sequence == UINT32_MAX ||
        transition_ack->next_sequence != probe_end_sequence + 1U ||
        transition_ack->next_sequence == UINT32_MAX ||
        transition_ack->current_component != probe->component ||
        strcmp(transition_ack->current_action, "apply") != 0 ||
        transition_ack->terminal ||
        !lower_hex_decode_32(
            probe->expected_sha256, candidate.probe.expected_sha256) ||
        !lower_hex_decode_32(
            accepted_receipt_sha256, candidate.receipt_sha256)) {
        return false;
    }
    candidate.probe.operation_id = probe->operation_id;
    candidate.probe.component = probe->component;
    memcpy(candidate.probe.catalog_name, probe->catalog_name,
        strlen(probe->catalog_name) + 1U);
    candidate.probe.expected_size = probe->expected_size;
    memcpy(candidate.probe.binding.uplink_mac,
        probe->binding.uplink_mac, 6U);
    candidate.probe.binding.uplink_boot_id =
        probe->binding.uplink_boot_id;
    memcpy(candidate.probe.binding.target_mac,
        probe->binding.target_mac, 6U);
    candidate.probe.binding.target_boot_id =
        probe->binding.target_boot_id;
    candidate.probe.binding.topology_generation =
        probe->binding.topology_generation;
    candidate.probe.apply_mode = probe->apply_mode;
    candidate.apply_start_sequence = transition_ack->next_sequence;
    *out = candidate;
    return true;
}

bool backend_ota_apply_matches_accepted_probe(
    const backend_ota_command_envelope_t *apply,
    const backend_ota_accepted_probe_t *accepted,
    uint32_t expected_next_sequence)
{
    uint8_t apply_sha256[32] = {0};
    uint8_t apply_receipt[32] = {0};
    if (apply == NULL || accepted == NULL || !apply->is_apply ||
        !apply->has_operation_id ||
        !fixed_string_terminated(apply->catalog_name,
            sizeof(apply->catalog_name)) ||
        !fixed_string_terminated(apply->expected_sha256,
            sizeof(apply->expected_sha256)) ||
        !fixed_string_terminated(apply->probe_receipt_sha256,
            sizeof(apply->probe_receipt_sha256)) ||
        !fixed_string_terminated(accepted->probe.catalog_name,
            sizeof(accepted->probe.catalog_name)) ||
        !lower_hex_decode_32(apply->expected_sha256, apply_sha256) ||
        !lower_hex_decode_32(apply->probe_receipt_sha256, apply_receipt)) {
        return false;
    }
    const bool operation_equal = backend_ota_operation_id_equal(
        &apply->operation_id, &accepted->probe.operation_id);
    const bool sha_equal = bytes_equal_constant_time(
        apply_sha256, accepted->probe.expected_sha256, sizeof(apply_sha256));
    const bool receipt_equal = bytes_equal_constant_time(
        apply_receipt, accepted->receipt_sha256, sizeof(apply_receipt));
    return operation_equal && sha_equal && receipt_equal &&
        apply->component == accepted->probe.component &&
        strcmp(apply->catalog_name, accepted->probe.catalog_name) == 0 &&
        apply->expected_size == accepted->probe.expected_size &&
        binding_equals(&apply->binding, &accepted->probe.binding) &&
        apply->apply_mode == accepted->probe.apply_mode &&
        expected_next_sequence != UINT32_MAX &&
        expected_next_sequence >= accepted->apply_start_sequence &&
        apply->next_sequence == expected_next_sequence;
}

static bool exact_object_fields(
    const char *json, const backend_json_token_t *tokens, size_t count,
    const char *const *expected, size_t expected_count)
{
    if (count != 1U + expected_count * 2U ||
        tokens[0].kind != BACKEND_JSON_OBJECT ||
        tokens[0].child_count != expected_count * 2U) {
        return false;
    }
    for (size_t wanted = 0U; wanted < expected_count; ++wanted) {
        size_t found = 0U;
        if (!find_field(json, tokens, count, expected[wanted], &found)) {
            return false;
        }
        for (size_t other = wanted + 1U; other < expected_count; ++other) {
            if (strcmp(expected[wanted], expected[other]) == 0) {
                return false;
            }
        }
    }
    return true;
}

backend_ota_command_decode_result_t backend_ota_command_decode(
    const char *json,
    size_t length,
    const backend_ota_command_local_t *local,
    backend_ota_command_envelope_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (json == NULL || local == NULL || out == NULL || length == 0U) {
        return BACKEND_OTA_COMMAND_DECODE_MALFORMED;
    }
    if (length > BACKEND_OTA_COMMAND_MAX_JSON) {
        return BACKEND_OTA_COMMAND_DECODE_TOO_LARGE;
    }
#if !defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    return BACKEND_OTA_COMMAND_DECODE_SCHEMA;
#else
    backend_json_token_t tokens[BACKEND_OTA_COMMAND_TOKENS];
    size_t count = 0U;
    const backend_json_result_t parsed = backend_json_parse(
        json, length, tokens, BACKEND_OTA_COMMAND_TOKENS, &count);
    if (parsed == BACKEND_JSON_TOO_MANY_TOKENS ||
        parsed == BACKEND_JSON_RANGE) {
        return BACKEND_OTA_COMMAND_DECODE_TOO_LARGE;
    }
    if (parsed != BACKEND_JSON_OK || count < 3U ||
        tokens[0].kind != BACKEND_JSON_OBJECT) {
        return BACKEND_OTA_COMMAND_DECODE_SCHEMA;
    }

    char type[32] = {0};
    if (!copy_string(json, tokens, count, "type", type, sizeof(type))) {
        return BACKEND_OTA_COMMAND_DECODE_SCHEMA;
    }
    const bool is_probe = strcmp(type, "backend_ota_probe") == 0;
    const bool is_apply = strcmp(type, "backend_ota_apply") == 0;
    const size_t fields = is_apply ? BACKEND_OTA_APPLY_KEY_COUNT :
        (is_probe ? BACKEND_OTA_PROBE_KEY_COUNT : 0U);
    static const char *const probe_fields[] = {
        "schema", "operation_id", "type", "component", "catalog_name",
        "expected_sha256", "expected_size", "expected_uplink_mac",
        "expected_uplink_boot_id", "expected_target_mac",
        "expected_target_boot_id", "expected_topology_generation",
        "apply_mode", "next_sequence",
    };
    static const char *const apply_fields[] = {
        "schema", "operation_id", "type", "component", "catalog_name",
        "expected_sha256", "expected_size", "expected_uplink_mac",
        "expected_uplink_boot_id", "expected_target_mac",
        "expected_target_boot_id", "expected_topology_generation",
        "apply_mode", "next_sequence", "probe_receipt_sha256",
    };
    if (fields == 0U || !exact_object_fields(json, tokens, count,
            is_apply ? apply_fields : probe_fields, fields)) {
        return BACKEND_OTA_COMMAND_DECODE_SCHEMA;
    }

    backend_ota_command_envelope_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    decoded.is_apply = is_apply;
    char operation_id[33] = {0};
    char component[16] = {0};
    char mode[32] = {0};
    char uplink_mac[18] = {0};
    char target_mac[18] = {0};
    size_t index = 0U;
    uint32_t schema = 0U;
    if (!copy_string(json, tokens, count, "operation_id", operation_id,
                     sizeof(operation_id)) ||
        !copy_string(json, tokens, count, "component", component,
                     sizeof(component)) ||
        !copy_string(json, tokens, count, "catalog_name", decoded.catalog_name,
                     sizeof(decoded.catalog_name)) ||
        !copy_string(json, tokens, count, "expected_sha256", decoded.expected_sha256,
                     sizeof(decoded.expected_sha256)) ||
        !copy_string(json, tokens, count, "expected_uplink_mac", uplink_mac,
                     sizeof(uplink_mac)) ||
        !copy_string(json, tokens, count, "expected_target_mac", target_mac,
                     sizeof(target_mac)) ||
        !copy_string(json, tokens, count, "apply_mode", mode, sizeof(mode)) ||
        !find_field(json, tokens, count, "schema", &index) ||
        !parse_nonzero_u32(json, &tokens[index], &schema) || schema != 1U ||
        !find_field(json, tokens, count, "expected_size", &index) ||
        !parse_nonzero_u32(json, &tokens[index], &decoded.expected_size) ||
        !find_field(json, tokens, count, "expected_uplink_boot_id", &index) ||
        !parse_nonzero_u32(json, &tokens[index], &decoded.binding.uplink_boot_id) ||
        !find_field(json, tokens, count, "expected_target_boot_id", &index) ||
        !parse_nonzero_u32(json, &tokens[index], &decoded.binding.target_boot_id) ||
        !find_field(json, tokens, count, "expected_topology_generation", &index) ||
        !parse_nonzero_u32(json, &tokens[index], &decoded.binding.topology_generation) ||
        !backend_ota_operation_id_decode(operation_id, &decoded.operation_id) ||
        !component_from_name(component, &decoded.component) ||
        !mode_from_name(mode, &decoded.apply_mode) ||
        !lower_hex(decoded.expected_sha256, 64U) ||
        !ascii_identity(decoded.catalog_name, sizeof(decoded.catalog_name)) ||
        !parse_mac(uplink_mac, decoded.binding.uplink_mac) ||
        !parse_mac(target_mac, decoded.binding.target_mac)) {
        return BACKEND_OTA_COMMAND_DECODE_SCHEMA;
    }
    uint64_t sequence = 0U;
    if (!find_field(json, tokens, count, "next_sequence", &index) ||
        !backend_json_get_u64(json, &tokens[index], &sequence) ||
        sequence >= UINT32_MAX) {
        return BACKEND_OTA_COMMAND_DECODE_SCHEMA;
    }
    decoded.next_sequence = (uint32_t)sequence;
    decoded.has_operation_id = true;
    if (is_apply && (!copy_string(json, tokens, count, "probe_receipt_sha256",
                                   decoded.probe_receipt_sha256,
                                   sizeof(decoded.probe_receipt_sha256)) ||
                     !lower_hex(decoded.probe_receipt_sha256, 64U))) {
        return BACKEND_OTA_COMMAND_DECODE_SCHEMA;
    }
    const char *target = decoded.component == BACKEND_OTA_COMPONENT_UPLINK
        ? FOF_BACKEND_UPLINK_TARGET : FOF_BACKEND_SCANNER_TARGET;
    const char *project = decoded.component == BACKEND_OTA_COMPONENT_UPLINK
        ? FOF_BACKEND_UPLINK_PROJECT : FOF_BACKEND_SCANNER_PROJECT;
    if (local->catalog_name == NULL || local->target == NULL ||
        local->project == NULL || local->hardware == NULL ||
        decoded.component != local->component ||
        strcmp(decoded.catalog_name, target) != 0 ||
        strcmp(decoded.catalog_name, local->catalog_name) != 0 ||
        strcmp(local->target, target) != 0 ||
        strcmp(local->project, project) != 0 ||
        strcmp(local->hardware, FOF_BACKEND_HARDWARE) != 0 ||
        local->max_expected_size == 0U ||
        local->max_expected_size > component_capacity(decoded.component) ||
        decoded.expected_size > local->max_expected_size ||
        !binding_semantics_valid(decoded.component, &decoded.binding) ||
        !binding_semantics_valid(local->component, &local->binding) ||
        !binding_equals(&decoded.binding, &local->binding)) {
        return BACKEND_OTA_COMMAND_DECODE_BINDING;
    }
    if ((is_probe && local->has_accepted_probe) ||
        (is_apply &&
         (!local->has_accepted_probe || !local->has_expected_next_sequence ||
          !backend_ota_apply_matches_accepted_probe(
              &decoded, &local->accepted_probe,
              local->expected_next_sequence)))) {
        return BACKEND_OTA_COMMAND_DECODE_BINDING;
    }
    *out = decoded;
    return BACKEND_OTA_COMMAND_DECODE_OK;
#endif
}

bool backend_ota_command_ack_decode(
    const char *json, size_t length,
    const backend_ota_operation_id_t *expected_operation_id,
    uint32_t expected_sequence,
    backend_ota_component_t expected_component,
    const char *expected_action,
    bool expected_terminal,
    backend_ota_command_ack_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
#if !defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    (void)json;
    (void)length;
    (void)expected_operation_id;
    (void)expected_sequence;
    (void)expected_component;
    (void)expected_action;
    (void)expected_terminal;
    return false;
#else
    if (!json || !out || !expected_operation_id ||
        component_name(expected_component) == NULL ||
        !action_is_valid(expected_action) ||
        expected_sequence == UINT32_MAX || length == 0U ||
        length > BACKEND_OTA_COMMAND_MAX_JSON) {
        return false;
    }
    backend_json_token_t tokens[24];
    size_t count = 0U;
    static const char *const fields[] = {
        "ok", "operation_id", "accepted_sequence", "next_sequence",
        "current_component", "current_action", "terminal", "duplicate",
    };
    if (backend_json_parse(json, length, tokens, 24U, &count) !=
            BACKEND_JSON_OK ||
        !exact_object_fields(json, tokens, count, fields, 8U)) {
        return false;
    }
    backend_ota_command_ack_t ack;
    memset(&ack, 0, sizeof(ack));
    char id[33] = {0};
    char component[16] = {0};
    size_t index = 0U;
    uint64_t number = 0U;
    if (!find_field(json, tokens, count, "ok", &index) ||
        !backend_json_get_bool(json, &tokens[index], &ack.ok) ||
        !copy_string(json, tokens, count, "operation_id", id, sizeof(id)) ||
        !backend_ota_operation_id_decode(id, &ack.operation_id) ||
        !find_field(json, tokens, count, "accepted_sequence", &index) ||
        !backend_json_get_u64(json, &tokens[index], &number) ||
        number != expected_sequence ||
        !find_field(json, tokens, count, "next_sequence", &index) ||
        !backend_json_get_u64(json, &tokens[index], &number) ||
        number != (uint64_t)expected_sequence + 1U ||
        !copy_string(json, tokens, count, "current_component", component,
                     sizeof(component)) ||
        !component_from_name(component, &ack.current_component) ||
        !copy_string(json, tokens, count, "current_action", ack.current_action,
                     sizeof(ack.current_action)) ||
        !find_field(json, tokens, count, "terminal", &index) ||
        !backend_json_get_bool(json, &tokens[index], &ack.terminal) ||
        !find_field(json, tokens, count, "duplicate", &index) ||
        !backend_json_get_bool(json, &tokens[index], &ack.duplicate)) {
        return false;
    }
    ack.accepted_sequence = expected_sequence;
    ack.next_sequence = (uint32_t)number;
    ack.has_operation_id = true;
    if (!ack.ok ||
        !backend_ota_operation_id_equal(&ack.operation_id, expected_operation_id) ||
        ack.current_component != expected_component ||
        !action_is_valid(ack.current_action) ||
        strcmp(ack.current_action, expected_action) != 0 ||
        ack.terminal != expected_terminal) {
        return false;
    }
    *out = ack;
    return true;
#endif
}

bool backend_ota_progress_accept(
    backend_ota_progress_state_t *state,
    backend_ota_progress_stage_t stage,
    uint32_t received,
    uint32_t total,
    uint32_t retry_count)
{
    if (state == NULL || stage < BACKEND_OTA_PROGRESS_METADATA ||
        stage > BACKEND_OTA_PROGRESS_CONVERGENCE ||
        received > total) {
        return false;
    }
    if (state->initialized &&
        (stage < state->stage || total != state->total ||
         received < state->received || retry_count < state->retry_count)) {
        return false;
    }
    state->initialized = true;
    state->stage = stage;
    state->received = received;
    state->total = total;
    state->retry_count = retry_count;
    return true;
}

backend_ota_probe_mode_result_t backend_ota_fullsize_probe_mode(
    fof_firmware_version_relation_t relation,
    backend_ota_apply_mode_t mode)
{
    if (mode != BACKEND_OTA_NEWER_ONLY &&
        mode != BACKEND_OTA_SAME_VERSION_RECOVERY) {
        return BACKEND_OTA_PROBE_REJECTED;
    }
    if (relation == FOF_VERSION_NEWER) {
        return BACKEND_OTA_PROBE_ELIGIBLE;
    }
    if (relation == FOF_VERSION_EQUAL && mode == BACKEND_OTA_NEWER_ONLY) {
        return BACKEND_OTA_PROBE_NO_UPDATE;
    }
    if (relation == FOF_VERSION_EQUAL &&
        mode == BACKEND_OTA_SAME_VERSION_RECOVERY) {
        return BACKEND_OTA_PROBE_ELIGIBLE;
    }
    return BACKEND_OTA_PROBE_REJECTED;
}

static const char *component_name(backend_ota_component_t component)
{
    switch (component) {
    case BACKEND_OTA_COMPONENT_SCANNER0: return "scanner0";
    case BACKEND_OTA_COMPONENT_SCANNER1: return "scanner1";
    case BACKEND_OTA_COMPONENT_UPLINK: return "uplink";
    default: return NULL;
    }
}

static const char *mode_name(backend_ota_apply_mode_t mode)
{
    return mode == BACKEND_OTA_NEWER_ONLY ? "newer_only" :
        (mode == BACKEND_OTA_SAME_VERSION_RECOVERY ?
             "same_version_recovery" : NULL);
}

static const char *stage_name(backend_ota_progress_stage_t stage)
{
    static const char *const names[] = {
        "metadata", "download", "validate", "stage", "uart_relay",
        "reboot_wait", "convergence",
    };
    return stage <= BACKEND_OTA_PROGRESS_CONVERGENCE ? names[stage] : NULL;
}

static size_t event_prefix_encode(
    const backend_ota_event_prefix_t *event, const char *type,
    char *out, size_t capacity)
{
    char id[33];
    const char *catalog = event != NULL &&
        event->component == BACKEND_OTA_COMPONENT_UPLINK
        ? FOF_BACKEND_UPLINK_TARGET : FOF_BACKEND_SCANNER_TARGET;
    if (event == NULL || out == NULL || capacity == 0U || type == NULL ||
        !event->has_operation_id ||
        event->sequence == UINT32_MAX || component_name(event->component) == NULL ||
        !ascii_identity(event->catalog_name, 40U) ||
        strcmp(event->catalog_name, catalog) != 0 ||
        !backend_ota_operation_id_encode(&event->operation_id,id,sizeof(id))) return 0U;
    const int length=snprintf(out,capacity,
        "{\"schema\":1,\"operation_id\":\"%s\",\"sequence\":%" PRIu32 ",\"type\":\"%s\",\"component\":\"%s\",\"catalog_name\":\"%s\"}",
        id,event->sequence,type,component_name(event->component),event->catalog_name);
    return length < 0 || (size_t)length >= capacity ? 0U : (size_t)length;
}

size_t backend_ota_event_begin_encode(
    const backend_ota_event_prefix_t *event, char *out, size_t capacity)
{
    return event_prefix_encode(event,"backend_ota_begin",out,capacity);
}

size_t backend_ota_event_progress_encode(
    backend_ota_progress_state_t *state,
    const backend_ota_progress_event_t *event, char *out, size_t capacity)
{
    if (event == NULL || out == NULL || capacity == 0U || state == NULL ||
        !event->prefix.has_operation_id || event->prefix.sequence == UINT32_MAX ||
        (event->stage == BACKEND_OTA_PROGRESS_REBOOT_WAIT &&
         !event->prefix.is_apply) ||
        (event->stage == BACKEND_OTA_PROGRESS_UART_RELAY &&
         event->prefix.component == BACKEND_OTA_COMPONENT_UPLINK)) {
        return 0U;
    }
    backend_ota_progress_state_t candidate = *state;
    if (!backend_ota_progress_accept(
            &candidate, event->stage, event->received, event->total,
            event->retry_count)) {
        return 0U;
    }
    char id[33];
    if (!backend_ota_operation_id_encode(&event->prefix.operation_id,id,sizeof(id)) ||
        component_name(event->prefix.component)==NULL || stage_name(event->stage)==NULL ||
        !ascii_identity(event->prefix.catalog_name,40U) ||
        strcmp(event->prefix.catalog_name,
            event->prefix.component == BACKEND_OTA_COMPONENT_UPLINK
                ? FOF_BACKEND_UPLINK_TARGET : FOF_BACKEND_SCANNER_TARGET) != 0) return 0U;
    const int length=snprintf(out,capacity,
        "{\"schema\":1,\"operation_id\":\"%s\",\"sequence\":%" PRIu32 ",\"type\":\"backend_ota_progress\",\"component\":\"%s\",\"catalog_name\":\"%s\",\"stage\":\"%s\",\"received\":%" PRIu32 ",\"total\":%" PRIu32 ",\"retry_count\":%" PRIu32 "}",
        id,event->prefix.sequence,component_name(event->prefix.component),event->prefix.catalog_name,stage_name(event->stage),event->received,event->total,event->retry_count);
    if (length < 0 || (size_t)length >= capacity) {
        return 0U;
    }
    *state = candidate;
    return (size_t)length;
}

static const char *terminal_error_name(backend_ota_terminal_error_t error)
{
    switch (error) {
    case BACKEND_OTA_TERMINAL_ERROR_NONE: return "none";
    case BACKEND_OTA_TERMINAL_ERROR_IDENTITY_MISMATCH:
        return "identity_mismatch";
    case BACKEND_OTA_TERMINAL_ERROR_STALE_BINDING: return "stale_binding";
    case BACKEND_OTA_TERMINAL_ERROR_CAPACITY: return "capacity";
    case BACKEND_OTA_TERMINAL_ERROR_DOWNLOAD: return "download";
    case BACKEND_OTA_TERMINAL_ERROR_HASH_MISMATCH: return "hash_mismatch";
    case BACKEND_OTA_TERMINAL_ERROR_UART: return "uart";
    case BACKEND_OTA_TERMINAL_ERROR_REBOOT_TIMEOUT: return "reboot_timeout";
    case BACKEND_OTA_TERMINAL_ERROR_HEALTH: return "health";
    case BACKEND_OTA_TERMINAL_ERROR_ROLLBACK: return "rollback";
    case BACKEND_OTA_TERMINAL_ERROR_INTERNAL: return "internal";
    default: return NULL;
    }
}

static bool terminal_command_valid(
    const backend_ota_command_envelope_t *command)
{
    if (command == NULL || !command->has_operation_id ||
        component_name(command->component) == NULL ||
        !fixed_string_terminated(command->catalog_name,
            sizeof(command->catalog_name)) ||
        !fixed_string_terminated(command->expected_sha256,
            sizeof(command->expected_sha256)) ||
        !fixed_string_terminated(command->probe_receipt_sha256,
            sizeof(command->probe_receipt_sha256)) ||
        !lower_hex(command->expected_sha256, 64U) ||
        command->expected_size == 0U ||
        command->expected_size > component_capacity(command->component) ||
        !binding_semantics_valid(command->component, &command->binding) ||
        mode_name(command->apply_mode) == NULL ||
        command->next_sequence == UINT32_MAX) {
        return false;
    }
    const char *const catalog = command->component == BACKEND_OTA_COMPONENT_UPLINK
        ? FOF_BACKEND_UPLINK_TARGET : FOF_BACKEND_SCANNER_TARGET;
    if (strcmp(command->catalog_name, catalog) != 0) {
        return false;
    }
    return command->is_apply
        ? lower_hex(command->probe_receipt_sha256, 64U)
        : command->probe_receipt_sha256[0] == '\0';
}

static bool terminal_candidate_valid(
    const backend_ota_command_envelope_t *command,
    const backend_ota_manifest_t *candidate)
{
    if (command == NULL || candidate == NULL ||
        !fixed_string_terminated(candidate->target, sizeof(candidate->target)) ||
        !fixed_string_terminated(candidate->project, sizeof(candidate->project)) ||
        !fixed_string_terminated(candidate->hardware, sizeof(candidate->hardware)) ||
        !fixed_string_terminated(candidate->version, sizeof(candidate->version)) ||
        !fixed_string_terminated(candidate->sha256, sizeof(candidate->sha256)) ||
        !ascii_identity(candidate->target, sizeof(candidate->target) - 1U) ||
        !ascii_identity(candidate->project, sizeof(candidate->project) - 1U) ||
        !ascii_identity(candidate->hardware, sizeof(candidate->hardware) - 1U) ||
        !ascii_identity(candidate->version, sizeof(candidate->version) - 1U) ||
        !lower_hex(candidate->sha256, 64U) ||
        candidate->image_size != command->expected_size) {
        return false;
    }
    const char *const target = command->component == BACKEND_OTA_COMPONENT_UPLINK
        ? FOF_BACKEND_UPLINK_TARGET : FOF_BACKEND_SCANNER_TARGET;
    const char *const project = command->component == BACKEND_OTA_COMPONENT_UPLINK
        ? FOF_BACKEND_UPLINK_PROJECT : FOF_BACKEND_SCANNER_PROJECT;
    uint8_t command_sha256[32] = {0};
    uint8_t candidate_sha256[32] = {0};
    return strcmp(command->catalog_name, target) == 0 &&
        strcmp(candidate->target, command->catalog_name) == 0 &&
        strcmp(candidate->target, target) == 0 &&
        strcmp(candidate->project, project) == 0 &&
        strcmp(candidate->hardware, FOF_BACKEND_HARDWARE) == 0 &&
        lower_hex_decode_32(command->expected_sha256, command_sha256) &&
        lower_hex_decode_32(candidate->sha256, candidate_sha256) &&
        bytes_equal_constant_time(
            command_sha256, candidate_sha256, sizeof(command_sha256));
}

static bool terminal_actual_binding_valid(
    const backend_ota_command_envelope_t *command,
    const backend_ota_target_binding_t *actual,
    bool success)
{
    if (command == NULL || actual == NULL ||
        actual->component != command->component ||
        actual->component_slot != backend_ota_component_slot(command->component) ||
        !mac_is_nonzero_unicast(actual->target_mac) ||
        actual->target_boot_id == 0U || actual->topology_generation == 0U) {
        return false;
    }
    return !success ||
        (memcmp(actual->target_mac, command->binding.target_mac, 6U) == 0 &&
         actual->topology_generation == command->binding.topology_generation);
}

static bool terminal_failure_identity_valid(
    const backend_ota_terminal_evidence_t *evidence)
{
    return evidence != NULL &&
        fixed_string_terminated(evidence->observed_target,
            sizeof(evidence->observed_target)) &&
        fixed_string_terminated(evidence->observed_project,
            sizeof(evidence->observed_project)) &&
        fixed_string_terminated(evidence->observed_hardware,
            sizeof(evidence->observed_hardware)) &&
        fixed_string_terminated(evidence->observed_version,
            sizeof(evidence->observed_version)) &&
        ascii_identity(evidence->observed_target, 64U) &&
        ascii_identity(evidence->observed_project, 64U) &&
        ascii_identity(evidence->observed_hardware, 64U) &&
        ascii_identity(evidence->observed_version, 64U);
}

static bool terminal_progress_valid(
    const backend_ota_progress_state_t *progress)
{
    return progress == NULL || !progress->initialized ||
        (progress->stage >= BACKEND_OTA_PROGRESS_METADATA &&
         progress->stage <= BACKEND_OTA_PROGRESS_CONVERGENCE &&
         progress->received <= progress->total);
}

bool backend_ota_event_end_build(
    const backend_ota_command_envelope_t *immutable_command,
    const backend_ota_progress_state_t *last_progress,
    uint32_t sequence,
    const backend_ota_terminal_evidence_t *evidence,
    char *body,
    size_t capacity,
    backend_ota_built_end_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (body != NULL && capacity > 0U) {
        body[0] = '\0';
    }
    if (immutable_command == NULL || evidence == NULL || body == NULL ||
        capacity == 0U || out == NULL || sequence == UINT32_MAX ||
        !terminal_command_valid(immutable_command) ||
        !terminal_progress_valid(last_progress)) {
        return false;
    }

    const bool success =
        evidence->outcome == BACKEND_OTA_TERMINAL_ELIGIBLE ||
        evidence->outcome == BACKEND_OTA_TERMINAL_NO_UPDATE ||
        evidence->outcome == BACKEND_OTA_TERMINAL_APPLIED;
    if (!terminal_actual_binding_valid(
            immutable_command, &evidence->actual_binding, success)) {
        return false;
    }

    const char *state = NULL;
    const char *decision = NULL;
    const char *error = terminal_error_name(evidence->error);
    const char *target = NULL;
    const char *project = NULL;
    const char *hardware = NULL;
    const char *version = NULL;
    bool failed_before_identity = false;
    const bool role_healthy = evidence->identity_exact &&
        evidence->command_ingress_healthy && evidence->role_acked &&
        evidence->profile_correct;

    if (error == NULL) {
        return false;
    }
    if (success) {
        if (evidence->error != BACKEND_OTA_TERMINAL_ERROR_NONE ||
            !terminal_candidate_valid(
                immutable_command, &evidence->candidate) ||
            !role_healthy || !evidence->radio_healthy ||
            !evidence->rollback_clear) {
            return false;
        }
        target = evidence->candidate.target;
        project = evidence->candidate.project;
        hardware = evidence->candidate.hardware;
        version = evidence->candidate.version;
        if (evidence->outcome == BACKEND_OTA_TERMINAL_ELIGIBLE) {
            if (immutable_command->is_apply ||
                backend_ota_fullsize_probe_mode(
                    evidence->relation, immutable_command->apply_mode) !=
                    BACKEND_OTA_PROBE_ELIGIBLE ||
                !evidence->complete_image_validated ||
                evidence->validated_image_bytes !=
                    immutable_command->expected_size ||
                evidence->image_writes != 0U ||
                evidence->actual_binding.target_boot_id !=
                    immutable_command->binding.target_boot_id) {
                return false;
            }
            state = "complete";
            decision = "eligible";
        } else if (evidence->outcome == BACKEND_OTA_TERMINAL_NO_UPDATE) {
            if (immutable_command->is_apply ||
                backend_ota_fullsize_probe_mode(
                    evidence->relation, immutable_command->apply_mode) !=
                    BACKEND_OTA_PROBE_NO_UPDATE ||
                evidence->image_writes != 0U ||
                (evidence->complete_image_validated
                    ? evidence->validated_image_bytes !=
                        immutable_command->expected_size
                    : evidence->validated_image_bytes != 0U) ||
                evidence->actual_binding.target_boot_id !=
                    immutable_command->binding.target_boot_id) {
                return false;
            }
            state = "no_update";
            decision = "no_update";
        } else {
            const bool apply_relation_valid =
                evidence->relation == FOF_VERSION_NEWER ||
                (evidence->relation == FOF_VERSION_EQUAL &&
                 immutable_command->apply_mode ==
                    BACKEND_OTA_SAME_VERSION_RECOVERY);
            if (!immutable_command->is_apply || !apply_relation_valid ||
                !evidence->complete_image_validated ||
                evidence->validated_image_bytes !=
                    immutable_command->expected_size ||
                evidence->image_writes != immutable_command->expected_size ||
                evidence->actual_binding.target_boot_id ==
                    immutable_command->binding.target_boot_id) {
                return false;
            }
            state = "complete";
            decision = "applied";
        }
    } else {
        if (evidence->error == BACKEND_OTA_TERMINAL_ERROR_NONE ||
            (evidence->outcome != BACKEND_OTA_TERMINAL_FAILED &&
             evidence->outcome != BACKEND_OTA_TERMINAL_ROLLED_BACK)) {
            return false;
        }
        if (evidence->has_observed_failure_identity) {
            if (!terminal_failure_identity_valid(evidence)) {
                return false;
            }
            target = evidence->observed_target;
            project = evidence->observed_project;
            hardware = evidence->observed_hardware;
            version = evidence->observed_version;
        } else {
            const bool identity_blank = evidence->observed_target[0] == '\0' &&
                evidence->observed_project[0] == '\0' &&
                evidence->observed_hardware[0] == '\0' &&
                evidence->observed_version[0] == '\0';
            const bool before_validate = last_progress == NULL ||
                !last_progress->initialized ||
                last_progress->stage < BACKEND_OTA_PROGRESS_VALIDATE;
            if (evidence->outcome != BACKEND_OTA_TERMINAL_FAILED ||
                immutable_command->is_apply || evidence->image_writes != 0U ||
                !identity_blank || !before_validate) {
                return false;
            }
            target = "";
            project = "";
            hardware = "";
            version = "";
            failed_before_identity = true;
        }
        state = evidence->outcome == BACKEND_OTA_TERMINAL_FAILED
            ? "failed" : "rolled_back";
        decision = evidence->outcome == BACKEND_OTA_TERMINAL_FAILED
            ? "rejected" : "rolled_back";
    }

    const backend_ota_receipt_command_t receipt_command = {
        .is_apply = immutable_command->is_apply,
        .component = immutable_command->component,
        .catalog_name = immutable_command->catalog_name,
        .expected_sha256 = immutable_command->expected_sha256,
        .expected_size = immutable_command->expected_size,
        .binding = immutable_command->binding,
        .apply_mode = immutable_command->apply_mode,
    };
    backend_ota_receipt_end_t receipt_end = {
        .state = state,
        .decision = decision,
        .error = error,
        .failed_before_identity = failed_before_identity,
        .image_writes = evidence->image_writes,
        .target = target,
        .project = project,
        .hardware = hardware,
        .version = version,
        .actual_boot_id = evidence->actual_binding.target_boot_id,
        .actual_topology_generation = evidence->actual_binding.topology_generation,
        .role_healthy = role_healthy,
        .radio_healthy = evidence->radio_healthy,
        .rollback_clear = evidence->rollback_clear,
    };
    memcpy(receipt_end.actual_mac, evidence->actual_binding.target_mac,
        sizeof(receipt_end.actual_mac));

    char receipt_sha256[65] = {0};
    if (!backend_ota_receipt_v1_sha256(
            &immutable_command->operation_id, &receipt_command, &receipt_end,
            receipt_sha256)) {
        return false;
    }
    char operation_id[33] = {0};
    char actual_mac[18] = {0};
    if (!backend_ota_operation_id_encode(
            &immutable_command->operation_id, operation_id,
            sizeof(operation_id))) {
        return false;
    }
    mac_text(evidence->actual_binding.target_mac, actual_mac);
    char scratch[BACKEND_OTA_COMMAND_MAX_JSON];
    const int length = snprintf(scratch, sizeof(scratch),
        "{\"schema\":1,\"operation_id\":\"%s\",\"sequence\":%" PRIu32 ",\"type\":\"backend_ota_end\",\"component\":\"%s\",\"catalog_name\":\"%s\",\"state\":\"%s\",\"decision\":\"%s\",\"error\":\"%s\",\"image_writes\":%" PRIu32 ",\"target\":\"%s\",\"project\":\"%s\",\"hardware\":\"%s\",\"version\":\"%s\",\"actual_mac\":\"%s\",\"actual_boot_id\":%" PRIu32 ",\"actual_topology_generation\":%" PRIu32 ",\"role_healthy\":%s,\"radio_healthy\":%s,\"rollback_clear\":%s,\"receipt_sha256\":\"%s\"}",
        operation_id, sequence, component_name(immutable_command->component),
        immutable_command->catalog_name, state, decision, error,
        evidence->image_writes, target, project, hardware, version, actual_mac,
        evidence->actual_binding.target_boot_id,
        evidence->actual_binding.topology_generation,
        role_healthy ? "true" : "false",
        evidence->radio_healthy ? "true" : "false",
        evidence->rollback_clear ? "true" : "false", receipt_sha256);
    if (length < 0 || (size_t)length >= sizeof(scratch) ||
        (size_t)length >= capacity) {
        return false;
    }
    memcpy(body, scratch, (size_t)length + 1U);
    out->body_length = (size_t)length;
    memcpy(out->receipt_sha256, receipt_sha256, sizeof(out->receipt_sha256));
    return true;
}

static void mac_text(const uint8_t mac[6], char out[18])
{
    (void)snprintf(out, 18U, "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool receipt_command_valid(const backend_ota_receipt_command_t *command)
{
    if (command == NULL || component_name(command->component) == NULL ||
        mode_name(command->apply_mode) == NULL || command->catalog_name == NULL ||
        command->expected_sha256 == NULL ||
        !lower_hex(command->expected_sha256, 64U) || command->expected_size == 0U ||
        command->expected_size > component_capacity(command->component) ||
        command->binding.uplink_boot_id == 0U ||
        command->binding.target_boot_id == 0U ||
        command->binding.topology_generation == 0U) {
        return false;
    }
    const char *catalog = command->component == BACKEND_OTA_COMPONENT_UPLINK
        ? FOF_BACKEND_UPLINK_TARGET : FOF_BACKEND_SCANNER_TARGET;
    return strcmp(command->catalog_name, catalog) == 0;
}

static bool receipt_end_valid(
    const backend_ota_receipt_command_t *command,
    const backend_ota_receipt_end_t *end)
{
    static const char *const errors[] = {
        "none", "identity_mismatch", "stale_binding", "capacity", "download",
        "hash_mismatch", "uart", "reboot_timeout", "health", "rollback", "internal",
    };
    if (command == NULL || end == NULL || end->state == NULL ||
        end->decision == NULL || end->error == NULL || end->target == NULL ||
        end->project == NULL || end->hardware == NULL || end->version == NULL ||
        end->actual_boot_id == 0U || end->actual_topology_generation == 0U) {
        return false;
    }
    bool known_error = false;
    for (size_t index = 0U; index < sizeof(errors) / sizeof(errors[0]); ++index) {
        known_error |= strcmp(end->error, errors[index]) == 0;
    }
    if (!known_error) {
        return false;
    }
    const bool empty_identity = end->target[0] == '\0' && end->project[0] == '\0' &&
        end->hardware[0] == '\0' && end->version[0] == '\0';
    const bool full_identity = ascii_identity(end->target, 64U) &&
        ascii_identity(end->project, 64U) && ascii_identity(end->hardware, 64U) &&
        ascii_identity(end->version, 64U);
    const bool failed_rejected = strcmp(end->state, "failed") == 0 &&
        strcmp(end->decision, "rejected") == 0 && strcmp(end->error, "none") != 0;
    if ((full_identity && end->failed_before_identity) ||
        !(full_identity ||
          (failed_rejected && !command->is_apply && end->failed_before_identity &&
           empty_identity && end->image_writes == 0U))) {
        return false;
    }
    if (strcmp(end->state, "complete") == 0 &&
        strcmp(end->decision, "eligible") == 0) {
        return !command->is_apply && strcmp(end->error, "none") == 0 &&
            end->image_writes == 0U && end->role_healthy &&
            end->radio_healthy && end->rollback_clear;
    }
    if (strcmp(end->state, "complete") == 0 &&
        strcmp(end->decision, "applied") == 0) {
        return command->is_apply && strcmp(end->error, "none") == 0 &&
            end->image_writes > 0U && end->role_healthy &&
            end->radio_healthy && end->rollback_clear;
    }
    if (strcmp(end->state, "no_update") == 0 &&
        strcmp(end->decision, "no_update") == 0) {
        return !command->is_apply && strcmp(end->error, "none") == 0 &&
            end->image_writes == 0U && end->role_healthy &&
            end->radio_healthy && end->rollback_clear;
    }
    if (failed_rejected) {
        return full_identity ||
            (!command->is_apply && end->failed_before_identity &&
             end->image_writes == 0U);
    }
    return strcmp(end->state, "rolled_back") == 0 &&
        strcmp(end->decision, "rolled_back") == 0 &&
        strcmp(end->error, "none") != 0 && full_identity;
}

BACKEND_OTA_RECEIPT_LINKAGE size_t backend_ota_receipt_v1_preimage(
    const backend_ota_operation_id_t *operation_id,
    const backend_ota_receipt_command_t *command,
    const backend_ota_receipt_end_t *end,
    uint8_t *out,
    size_t capacity)
{
#if !defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    (void)operation_id; (void)command; (void)end; (void)out; (void)capacity;
    return 0U;
#else
    if (operation_id == NULL || command == NULL || end == NULL || out == NULL ||
        !receipt_command_valid(command) || !receipt_end_valid(command, end)) {
        return 0U;
    }
    char id[33], uplink[18], target[18], actual[18];
    char scratch[BACKEND_OTA_RECEIPT_PREIMAGE_MAX_BYTES + 1U];
    if (!backend_ota_operation_id_encode(operation_id, id, sizeof(id))) {
        return 0U;
    }
    mac_text(command->binding.uplink_mac, uplink);
    mac_text(command->binding.target_mac, target);
    mac_text(end->actual_mac, actual);
    const int length = snprintf(scratch, sizeof(scratch),
        "fof-backend-ota-end-receipt-v1\noperation_id=%s\ncommand_type=%s\napply_mode=%s\ncomponent=%s\ncatalog_name=%s\nexpected_sha256=%s\nexpected_size=%" PRIu32 "\nexpected_uplink_mac=%s\nexpected_uplink_boot_id=%" PRIu32 "\nexpected_target_mac=%s\nexpected_target_boot_id=%" PRIu32 "\nexpected_topology_generation=%" PRIu32 "\nstate=%s\ndecision=%s\nerror=%s\nimage_writes=%" PRIu32 "\ntarget=%s\nproject=%s\nhardware=%s\nversion=%s\nactual_mac=%s\nactual_boot_id=%" PRIu32 "\nactual_topology_generation=%" PRIu32 "\nrole_healthy=%u\nradio_healthy=%u\nrollback_clear=%u\n",
        id, command->is_apply ? "backend_ota_apply" : "backend_ota_probe",
        mode_name(command->apply_mode), component_name(command->component),
        command->catalog_name, command->expected_sha256, command->expected_size,
        uplink, command->binding.uplink_boot_id, target, command->binding.target_boot_id,
        command->binding.topology_generation, end->state, end->decision, end->error,
        end->image_writes, end->target, end->project, end->hardware, end->version,
        actual, end->actual_boot_id, end->actual_topology_generation,
        end->role_healthy ? 1U : 0U, end->radio_healthy ? 1U : 0U,
        end->rollback_clear ? 1U : 0U);
    if (length < 0 || (size_t)length >= sizeof(scratch) ||
        (size_t)length >= capacity) {
        return 0U;
    }
    memcpy(out, scratch, (size_t)length + 1U);
    return (size_t)length;
#endif
}

BACKEND_OTA_RECEIPT_LINKAGE bool backend_ota_receipt_v1_sha256(
    const backend_ota_operation_id_t *operation_id,
    const backend_ota_receipt_command_t *command,
    const backend_ota_receipt_end_t *end,
    char out_sha256[65])
{
    if (out_sha256 == NULL) {
        return false;
    }
    memset(out_sha256, 0, 65U);
    uint8_t preimage[BACKEND_OTA_RECEIPT_PREIMAGE_MAX_BYTES];
    uint8_t digest[32];
    const size_t length = backend_ota_receipt_v1_preimage(
        operation_id, command, end, preimage, sizeof(preimage));
    if (length == 0U || !backend_ota_sha256(preimage, length, digest)) {
        return false;
    }
    fof_firmware_sha256_to_hex(digest, out_sha256);
    return true;
}

#if defined(UNIT_TESTING)
bool backend_ota_receipt_v1_verify(
    const backend_ota_operation_id_t *operation_id,
    const backend_ota_receipt_command_t *command,
    const backend_ota_receipt_end_t *end,
    const char *expected_sha256)
{
    char actual_hex[65];
    uint8_t expected[32];
    uint8_t actual[32];
    if (!lower_hex_decode_32(expected_sha256, expected) ||
        !backend_ota_receipt_v1_sha256(
            operation_id, command, end, actual_hex) ||
        !lower_hex_decode_32(actual_hex, actual)) {
        return false;
    }
    return bytes_equal_constant_time(expected, actual, sizeof(expected));
}
#endif
