#include "scanner_command_schema_registry.h"

#include <stddef.h>
#include <string.h>

#include "firmware_json_schema.h"
#include "mac_address_policy.h"

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))
#define TOKEN_MEMBER(member_name)                                           \
    {member_name, FOF_JSON_STRING,                                          \
     FOF_JSON_STRING_POLICY_ASCII_TOKEN_NO_ESCAPE}
#define NULLABLE_TOKEN_MEMBER(member_name)                                  \
    {member_name, FOF_JSON_NULLABLE_STRING,                                 \
     FOF_JSON_STRING_POLICY_ASCII_TOKEN_NO_ESCAPE}
#define BOOL_MEMBER(member_name)                                            \
    {member_name, FOF_JSON_BOOL, FOF_JSON_STRING_POLICY_NONE}
#define INT32_MEMBER(member_name)                                           \
    {member_name, FOF_JSON_INT32, FOF_JSON_STRING_POLICY_NONE}
#define INT64_MEMBER(member_name)                                           \
    {member_name, FOF_JSON_INT64, FOF_JSON_STRING_POLICY_NONE}
#define UINT32_MEMBER(member_name)                                          \
    {member_name, FOF_JSON_UINT32, FOF_JSON_STRING_POLICY_NONE}
#define OBJECT_MEMBER(member_name)                                          \
    {member_name, FOF_JSON_OBJECT, FOF_JSON_STRING_POLICY_NONE}

enum {
    SCANNER_COMMAND_MAX_MEMBERS = 6,
};

typedef struct {
    fof_scanner_command_id_t id;
    const char *selector;
    const fof_json_member_spec_t *members;
    size_t member_count;
} scanner_command_schema_t;

static const fof_json_member_spec_t TYPE_ONLY_MEMBERS[] = {
    TOKEN_MEMBER("type"),
};

static const fof_json_member_spec_t SCANNER_QUIET_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    BOOL_MEMBER("enabled"),
    UINT32_MEMBER("generation"),
};

static const fof_json_member_spec_t BLE_INVESTIGATE_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("request_id"),
    TOKEN_MEMBER("mode"),
    NULLABLE_TOKEN_MEMBER("target"),
    INT32_MEMBER("timeout_ms"),
};

static const fof_json_member_spec_t BLE_INVESTIGATE_CANCEL_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("request_id"),
};

static const fof_json_member_spec_t WIFI_LOCKON_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    INT32_MEMBER("ch"),
    INT32_MEMBER("dur"),
    TOKEN_MEMBER("bssid"),
};

static const fof_json_member_spec_t BLE_LOCKON_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("mac"),
    INT32_MEMBER("dur"),
};

static const fof_json_member_spec_t CAL_MODE_START_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("session_id"),
    TOKEN_MEMBER("calibration_uuid"),
};

static const fof_json_member_spec_t CAL_MODE_STOP_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("session_id"),
};

static const fof_json_member_spec_t SCAN_PROFILE_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("scan_profile"),
    TOKEN_MEMBER("slot_role"),
};

static const fof_json_member_spec_t DISPLAY_CONTROL_FULL_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    BOOL_MEMBER("button_enabled"),
    TOKEN_MEMBER("view"),
    INT32_MEMBER("page"),
    BOOL_MEMBER("page_lock"),
    BOOL_MEMBER("auto_page"),
};

static const fof_json_member_spec_t DISPLAY_CONTROL_BUTTON_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    BOOL_MEMBER("button_enabled"),
};

static const fof_json_member_spec_t DISPLAY_POLICY_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    UINT32_MEMBER("version"),
    UINT32_MEMBER("hash"),
    OBJECT_MEMBER("policy"),
};

static const fof_json_member_spec_t TIME_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    INT64_MEMBER("ms"),
    BOOL_MEMBER("ok"),
    TOKEN_MEMBER("src"),
};

static const fof_json_member_spec_t SAFE_MODE_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    BOOL_MEMBER("enabled"),
};

#if defined(FOF_DC34_GAME_CANARY)
static const fof_json_member_spec_t CRUD_SELF_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    UINT32_MEMBER("v"),
    UINT32_MEMBER("round"),
    TOKEN_MEMBER("peer"),
    TOKEN_MEMBER("session"),
};
#endif

#define COMMAND_SCHEMA(command_id, selector_value, member_array)             \
    [command_id] = {                                                         \
        .id = command_id,                                                     \
        .selector = selector_value,                                           \
        .members = member_array,                                              \
        .member_count = ARRAY_SIZE(member_array),                             \
    }

static const scanner_command_schema_t
SCANNER_COMMAND_SCHEMAS[FOF_SCANNER_COMMAND_COUNT] = {
    COMMAND_SCHEMA(
        FOF_SCANNER_COMMAND_READY, "ready", TYPE_ONLY_MEMBERS),
    COMMAND_SCHEMA(
        FOF_SCANNER_COMMAND_START, "start", TYPE_ONLY_MEMBERS),
    COMMAND_SCHEMA(
        FOF_SCANNER_COMMAND_STOP, "stop", TYPE_ONLY_MEMBERS),
    COMMAND_SCHEMA(
        FOF_SCANNER_COMMAND_SCANNER_QUIET,
        "scanner_quiet", SCANNER_QUIET_MEMBERS),
    COMMAND_SCHEMA(
        FOF_SCANNER_COMMAND_BLE_INVESTIGATE,
        "ble_investigate", BLE_INVESTIGATE_MEMBERS),
    COMMAND_SCHEMA(
        FOF_SCANNER_COMMAND_BLE_INVESTIGATE_CANCEL,
        "ble_investigate_cancel", BLE_INVESTIGATE_CANCEL_MEMBERS),
    COMMAND_SCHEMA(
        FOF_SCANNER_COMMAND_WIFI_LOCKON, "lockon", WIFI_LOCKON_MEMBERS),
    COMMAND_SCHEMA(
        FOF_SCANNER_COMMAND_WIFI_LOCKON_CANCEL,
        "lockon_cancel", TYPE_ONLY_MEMBERS),
    COMMAND_SCHEMA(
        FOF_SCANNER_COMMAND_BLE_LOCKON,
        "ble_lockon", BLE_LOCKON_MEMBERS),
    COMMAND_SCHEMA(
        FOF_SCANNER_COMMAND_BLE_LOCKON_CANCEL,
        "ble_lockon_cancel", TYPE_ONLY_MEMBERS),
    COMMAND_SCHEMA(
        FOF_SCANNER_COMMAND_CAL_MODE_START,
        "cal_mode_start", CAL_MODE_START_MEMBERS),
    COMMAND_SCHEMA(
        FOF_SCANNER_COMMAND_CAL_MODE_STOP,
        "cal_mode_stop", CAL_MODE_STOP_MEMBERS),
    COMMAND_SCHEMA(
        FOF_SCANNER_COMMAND_SCAN_PROFILE,
        "scan_profile", SCAN_PROFILE_MEMBERS),
    COMMAND_SCHEMA(
        FOF_SCANNER_COMMAND_DISPLAY_CONTROL_FULL,
        "display_control", DISPLAY_CONTROL_FULL_MEMBERS),
    COMMAND_SCHEMA(
        FOF_SCANNER_COMMAND_DISPLAY_CONTROL_BUTTON,
        "display_control", DISPLAY_CONTROL_BUTTON_MEMBERS),
    COMMAND_SCHEMA(
        FOF_SCANNER_COMMAND_DISPLAY_POLICY,
        "display_policy", DISPLAY_POLICY_MEMBERS),
    COMMAND_SCHEMA(
        FOF_SCANNER_COMMAND_TIME, "time", TIME_MEMBERS),
    COMMAND_SCHEMA(
        FOF_SCANNER_COMMAND_SAFE_MODE, "safe_mode", SAFE_MODE_MEMBERS),
    COMMAND_SCHEMA(
        FOF_SCANNER_COMMAND_REBOOT, "reboot", TYPE_ONLY_MEMBERS),
#if defined(FOF_DC34_GAME_CANARY)
    COMMAND_SCHEMA(
        FOF_SCANNER_COMMAND_CRUD_SELF, "crud_self", CRUD_SELF_MEMBERS),
#endif
};

static bool span_equals(const fof_json_value_span_t *span,
                        const char *literal)
{
    if (!span || !span->bytes || !literal) {
        return false;
    }
    size_t literal_len = strlen(literal);
    return span->byte_len == literal_len &&
           memcmp(span->bytes, literal, literal_len) == 0;
}

static bool parse_token(const fof_json_value_span_t *raw_value,
                        fof_json_value_span_t *token_out)
{
    return fof_json_value_span_parse_ascii_token(raw_value, token_out);
}

static bool copy_token(const fof_json_value_span_t *raw_value,
                       char *output,
                       size_t output_capacity)
{
    fof_json_value_span_t token = {0};
    if (!output || output_capacity == 0U ||
        !parse_token(raw_value, &token) ||
        token.byte_len >= output_capacity) {
        if (output && output_capacity > 0U) {
            output[0] = '\0';
        }
        return false;
    }
    memcpy(output, token.bytes, token.byte_len);
    output[token.byte_len] = '\0';
    return true;
}

static bool copy_token_or_empty(const fof_json_value_span_t *raw_value,
                                char *output,
                                size_t output_capacity)
{
    if (!output || output_capacity == 0U || !raw_value ||
        !raw_value->bytes) {
        return false;
    }
    if (raw_value->byte_len == 2U &&
        raw_value->bytes[0] == '"' && raw_value->bytes[1] == '"') {
        output[0] = '\0';
        return true;
    }
    return copy_token(raw_value, output, output_capacity);
}

static bool token_is_lower_hex(const char *token, size_t expected_len)
{
    if (!token || strlen(token) != expected_len) {
        return false;
    }
    for (size_t i = 0U; i < expected_len; ++i) {
        char ch = token[i];
        if (!((ch >= '0' && ch <= '9') ||
              (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool request_id_is_valid(const char *request_id)
{
    if (!request_id) {
        return false;
    }
    size_t length = strlen(request_id);
    return length >= 1U && length <= 32U;
}

static bool calibration_uuid_matches_session(const char *uuid,
                                             const char *session_id)
{
    if (!uuid || !session_id || strlen(uuid) != 36U ||
        !token_is_lower_hex(session_id, 12U)) {
        return false;
    }
    return memcmp(uuid, "cafe", 4U) == 0 &&
           memcmp(uuid + 4U, session_id, 4U) == 0 &&
           memcmp(uuid + 8U, "-0000-1000-8000-", 16U) == 0 &&
           memcmp(uuid + 24U, session_id, 12U) == 0;
}

static bool scan_role_is_fixed(const fof_json_value_span_t *role)
{
    return span_equals(role, "ble_primary") ||
           span_equals(role, "wifi_primary");
}

static bool display_view_is_valid(const fof_json_value_span_t *view)
{
    return span_equals(view, "privacy") ||
           span_equals(view, "prv") ||
           span_equals(view, "glasses") ||
           span_equals(view, "rf") ||
           span_equals(view, "activity") ||
           span_equals(view, "drone") ||
           span_equals(view, "wifi");
}

static bool wifi_lockon_duration_is_valid(int32_t duration_s)
{
    return duration_s == 30 || duration_s == 45 ||
           duration_s == 60 || duration_s == 90;
}

static bool validate_scanner_quiet(
    const fof_json_value_span_t values[SCANNER_COMMAND_MAX_MEMBERS],
    fof_scanner_command_t *command)
{
    return fof_json_value_span_parse_bool(
               &values[1], &command->data.scanner_quiet.enabled) &&
           fof_json_value_span_parse_uint32(
               &values[2], &command->data.scanner_quiet.generation);
}

static bool validate_ble_investigate(
    const fof_json_value_span_t values[SCANNER_COMMAND_MAX_MEMBERS],
    fof_scanner_command_t *command)
{
    fof_scanner_ble_investigate_command_t *out =
        &command->data.ble_investigate;
    fof_json_value_span_t mode = {0};
    fof_json_value_span_t target = {0};
    bool target_is_null = false;
    if (!copy_token(
            &values[1], out->request_id, sizeof(out->request_id)) ||
        !request_id_is_valid(out->request_id) ||
        !parse_token(&values[2], &mode) ||
        !fof_json_value_span_parse_nullable_ascii_token(
            &values[3], &target_is_null, &target) ||
        !fof_json_value_span_parse_int32(
            &values[4], &out->timeout_ms) ||
        out->timeout_ms < 1 || out->timeout_ms > 12000) {
        return false;
    }

    out->target_is_null = target_is_null;
    if (span_equals(&mode, "gatt")) {
        out->mode = FOF_SCANNER_BLE_INVESTIGATION_GATT;
        if (target_is_null || target.byte_len >= sizeof(out->target_mac)) {
            return false;
        }
        memcpy(out->target_mac, target.bytes, target.byte_len);
        out->target_mac[target.byte_len] = '\0';
        return fof_mac_is_canonical_upper(out->target_mac, false);
    }
    if (span_equals(&mode, "passive_capture")) {
        out->mode = FOF_SCANNER_BLE_INVESTIGATION_PASSIVE_CAPTURE;
        return target_is_null;
    }
    return false;
}

static bool validate_ble_investigate_cancel(
    const fof_json_value_span_t values[SCANNER_COMMAND_MAX_MEMBERS],
    fof_scanner_command_t *command)
{
    char *request_id =
        command->data.ble_investigate_cancel.request_id;
    return copy_token(
               &values[1], request_id,
               sizeof(command->data.ble_investigate_cancel.request_id)) &&
           request_id_is_valid(request_id);
}

static bool validate_wifi_lockon(
    const fof_json_value_span_t values[SCANNER_COMMAND_MAX_MEMBERS],
    fof_scanner_command_t *command)
{
    fof_scanner_wifi_lockon_command_t *out =
        &command->data.wifi_lockon;
    return fof_json_value_span_parse_int32(
               &values[1], &out->channel) &&
           out->channel >= 1 && out->channel <= 13 &&
           fof_json_value_span_parse_int32(
               &values[2], &out->duration_s) &&
           wifi_lockon_duration_is_valid(out->duration_s) &&
           copy_token_or_empty(
               &values[3], out->bssid, sizeof(out->bssid)) &&
           fof_mac_is_canonical_upper(out->bssid, true);
}

static bool validate_ble_lockon(
    const fof_json_value_span_t values[SCANNER_COMMAND_MAX_MEMBERS],
    fof_scanner_command_t *command)
{
    fof_scanner_ble_lockon_command_t *out =
        &command->data.ble_lockon;
    return copy_token(&values[1], out->mac, sizeof(out->mac)) &&
           fof_mac_is_canonical_upper(out->mac, false) &&
           fof_json_value_span_parse_int32(
               &values[2], &out->duration_s) &&
           out->duration_s == 45;
}

static bool validate_cal_mode_start(
    const fof_json_value_span_t values[SCANNER_COMMAND_MAX_MEMBERS],
    fof_scanner_command_t *command)
{
    fof_scanner_calibration_command_t *out =
        &command->data.calibration;
    return copy_token(
               &values[1], out->session_id, sizeof(out->session_id)) &&
           copy_token(
               &values[2], out->advertise_uuid,
               sizeof(out->advertise_uuid)) &&
           calibration_uuid_matches_session(
               out->advertise_uuid, out->session_id);
}

static bool validate_cal_mode_stop(
    const fof_json_value_span_t values[SCANNER_COMMAND_MAX_MEMBERS],
    fof_scanner_command_t *command)
{
    char *session_id = command->data.calibration.session_id;
    return copy_token(
               &values[1], session_id,
               sizeof(command->data.calibration.session_id)) &&
           (strcmp(session_id, "stale") == 0 ||
            token_is_lower_hex(session_id, 12U));
}

static bool validate_scan_profile(
    const fof_json_value_span_t values[SCANNER_COMMAND_MAX_MEMBERS],
    fof_scanner_deployment_t deployment,
    fof_scanner_command_t *command)
{
    fof_json_value_span_t profile = {0};
    fof_json_value_span_t role = {0};
    if (!parse_token(&values[1], &profile) ||
        !parse_token(&values[2], &role) ||
        !scan_role_is_fixed(&role) ||
        !copy_token(
            &values[1],
            command->data.scan_profile.scan_profile,
            sizeof(command->data.scan_profile.scan_profile)) ||
        !copy_token(
            &values[2],
            command->data.scan_profile.slot_role,
            sizeof(command->data.scan_profile.slot_role))) {
        return false;
    }

    if (span_equals(&profile, "hybrid_failover")) {
        return deployment == FOF_SCANNER_DEPLOYMENT_NON_BADGE;
    }
    return profile.byte_len == role.byte_len &&
           memcmp(profile.bytes, role.bytes, profile.byte_len) == 0;
}

static bool validate_display_control_full(
    const fof_json_value_span_t values[SCANNER_COMMAND_MAX_MEMBERS],
    fof_scanner_command_t *command)
{
    fof_scanner_display_control_command_t *out =
        &command->data.display_control;
    fof_json_value_span_t view = {0};
    return fof_json_value_span_parse_bool(
               &values[1], &out->button_enabled) &&
           parse_token(&values[2], &view) &&
           display_view_is_valid(&view) &&
           copy_token(&values[2], out->view, sizeof(out->view)) &&
           fof_json_value_span_parse_int32(&values[3], &out->page) &&
           fof_json_value_span_parse_bool(
               &values[4], &out->page_lock) &&
           fof_json_value_span_parse_bool(
               &values[5], &out->auto_page);
}

static bool validate_display_control_button(
    const fof_json_value_span_t values[SCANNER_COMMAND_MAX_MEMBERS],
    fof_scanner_command_t *command)
{
    return fof_json_value_span_parse_bool(
        &values[1], &command->data.display_control.button_enabled);
}

static bool validate_display_policy(
    const fof_json_value_span_t values[SCANNER_COMMAND_MAX_MEMBERS],
    fof_scanner_command_t *command)
{
    fof_scanner_display_policy_command_t *out =
        &command->data.display_policy;
    char error[64] = {0};
    return fof_json_value_span_parse_uint32(
               &values[1], &out->version) &&
           out->version == BADGE_DISPLAY_POLICY_VERSION &&
           fof_json_value_span_parse_uint32(
               &values[2], &out->hash) &&
           badge_display_policy_parse_json_span(
               values[3].bytes, values[3].byte_len,
               &out->policy, error, sizeof(error)) &&
           out->policy.version == out->version &&
           badge_display_policy_hash(&out->policy) == out->hash;
}

static bool validate_time(
    const fof_json_value_span_t values[SCANNER_COMMAND_MAX_MEMBERS],
    fof_scanner_command_t *command)
{
    fof_scanner_time_command_t *out = &command->data.time;
    fof_json_value_span_t source = {0};
    if (!fof_json_value_span_parse_int64(
            &values[1], &out->epoch_ms) ||
        !fof_json_value_span_parse_bool(&values[2], &out->ok) ||
        !parse_token(&values[3], &source)) {
        return false;
    }
    if (!out->ok) {
        out->source = FOF_SCANNER_TIME_SOURCE_NONE;
        return out->epoch_ms == -1 && span_equals(&source, "none");
    }
    if (out->epoch_ms <= INT64_C(1700000000000)) {
        return false;
    }
    if (span_equals(&source, "backend")) {
        out->source = FOF_SCANNER_TIME_SOURCE_BACKEND;
        return true;
    }
    if (span_equals(&source, "local")) {
        out->source = FOF_SCANNER_TIME_SOURCE_LOCAL;
        return true;
    }
    return false;
}

static bool validate_safe_mode(
    const fof_json_value_span_t values[SCANNER_COMMAND_MAX_MEMBERS],
    fof_scanner_command_t *command)
{
    return fof_json_value_span_parse_bool(
        &values[1], &command->data.safe_mode.enabled);
}

#if defined(FOF_DC34_GAME_CANARY)
static bool parse_nonzero_upper_hex(
    const fof_json_value_span_t *raw_value,
    size_t expected_digits,
    uint32_t *value_out)
{
    fof_json_value_span_t token = {0};
    if (!value_out || !parse_token(raw_value, &token) ||
        token.byte_len != expected_digits) {
        return false;
    }

    uint32_t value = 0U;
    for (size_t i = 0U; i < token.byte_len; ++i) {
        uint8_t nibble;
        uint8_t ch = token.bytes[i];
        if (ch >= (uint8_t)'0' && ch <= (uint8_t)'9') {
            nibble = (uint8_t)(ch - (uint8_t)'0');
        } else if (ch >= (uint8_t)'A' && ch <= (uint8_t)'F') {
            nibble = (uint8_t)(ch - (uint8_t)'A' + 10U);
        } else {
            return false;
        }
        value = (value << 4U) | nibble;
    }
    if (value == 0U) {
        return false;
    }
    *value_out = value;
    return true;
}

static bool validate_crud_self(
    const fof_json_value_span_t values[SCANNER_COMMAND_MAX_MEMBERS],
    fof_scanner_command_t *command)
{
    uint32_t version = 0U;
    uint32_t round = 0U;
    uint32_t peer = 0U;
    uint32_t session = 0U;
    if (!fof_json_value_span_parse_uint32(&values[1], &version) ||
        version != 1U ||
        !fof_json_value_span_parse_uint32(&values[2], &round) ||
        round != 34U ||
        !parse_nonzero_upper_hex(&values[3], 6U, &peer) ||
        !parse_nonzero_upper_hex(&values[4], 2U, &session)) {
        return false;
    }

    command->data.crud_self.peer = peer;
    command->data.crud_self.session = (uint8_t)session;
    return true;
}
#endif

static bool validate_semantics(
    const scanner_command_schema_t *schema,
    const fof_json_value_span_t values[SCANNER_COMMAND_MAX_MEMBERS],
    fof_scanner_deployment_t deployment,
    fof_scanner_command_t *command_out)
{
    if (!schema || !values || !command_out) {
        return false;
    }
    fof_scanner_command_t command = {
        .id = schema->id,
    };
    bool valid = false;
    switch (schema->id) {
        case FOF_SCANNER_COMMAND_READY:
        case FOF_SCANNER_COMMAND_START:
        case FOF_SCANNER_COMMAND_STOP:
        case FOF_SCANNER_COMMAND_WIFI_LOCKON_CANCEL:
        case FOF_SCANNER_COMMAND_BLE_LOCKON_CANCEL:
        case FOF_SCANNER_COMMAND_REBOOT:
            valid = true;
            break;
        case FOF_SCANNER_COMMAND_SCANNER_QUIET:
            valid = validate_scanner_quiet(values, &command);
            break;
        case FOF_SCANNER_COMMAND_BLE_INVESTIGATE:
            valid = validate_ble_investigate(values, &command);
            break;
        case FOF_SCANNER_COMMAND_BLE_INVESTIGATE_CANCEL:
            valid = validate_ble_investigate_cancel(values, &command);
            break;
        case FOF_SCANNER_COMMAND_WIFI_LOCKON:
            valid = validate_wifi_lockon(values, &command);
            break;
        case FOF_SCANNER_COMMAND_BLE_LOCKON:
            valid = validate_ble_lockon(values, &command);
            break;
        case FOF_SCANNER_COMMAND_CAL_MODE_START:
            valid = validate_cal_mode_start(values, &command);
            break;
        case FOF_SCANNER_COMMAND_CAL_MODE_STOP:
            valid = validate_cal_mode_stop(values, &command);
            break;
        case FOF_SCANNER_COMMAND_SCAN_PROFILE:
            valid = validate_scan_profile(values, deployment, &command);
            break;
        case FOF_SCANNER_COMMAND_DISPLAY_CONTROL_FULL:
            valid = validate_display_control_full(values, &command);
            break;
        case FOF_SCANNER_COMMAND_DISPLAY_CONTROL_BUTTON:
            valid = validate_display_control_button(values, &command);
            break;
        case FOF_SCANNER_COMMAND_DISPLAY_POLICY:
            valid = validate_display_policy(values, &command);
            break;
        case FOF_SCANNER_COMMAND_TIME:
            valid = validate_time(values, &command);
            break;
        case FOF_SCANNER_COMMAND_SAFE_MODE:
            valid = validate_safe_mode(values, &command);
            break;
#if defined(FOF_DC34_GAME_CANARY)
        case FOF_SCANNER_COMMAND_CRUD_SELF:
            valid = validate_crud_self(values, &command);
            break;
#endif
        case FOF_SCANNER_COMMAND_NONE:
        case FOF_SCANNER_COMMAND_COUNT:
        default:
            valid = false;
            break;
    }
    if (!valid) {
        return false;
    }
    *command_out = command;
    return true;
}

static bool selector_equals(const char *selector,
                            size_t selector_len,
                            const char *expected)
{
    return selector && expected &&
           strlen(expected) == selector_len &&
           memcmp(selector, expected, selector_len) == 0;
}

static bool mutation_selector_is_known(const char *selector,
                                       size_t selector_len)
{
    return selector_equals(selector, selector_len, "bootloader") ||
           selector_equals(selector, selector_len, "ota");
}

static fof_scanner_command_registry_result_t route_firmware_first(
    const uint8_t *bytes,
    size_t byte_len,
    fof_scanner_command_decision_t *decision_out,
    bool *continue_nonfirmware_out)
{
    *continue_nonfirmware_out = false;
    fof_fw_json_schema_id_t firmware_schema = FOF_FW_JSON_SCHEMA_NONE;
    fof_fw_json_registry_result_t firmware_result =
        fof_fw_json_select_and_validate(
            FOF_FW_JSON_INGRESS_UPLINK_TO_SCANNER_UART,
            bytes, byte_len, &firmware_schema);
    switch (firmware_result) {
        case FOF_FW_JSON_REGISTRY_OK:
            decision_out->route = FOF_SCANNER_COMMAND_ROUTE_FIRMWARE;
            decision_out->firmware_schema_id = firmware_schema;
            return FOF_SCANNER_COMMAND_REGISTRY_OK;
        case FOF_FW_JSON_REGISTRY_UNKNOWN_SELECTOR:
            *continue_nonfirmware_out = true;
            return FOF_SCANNER_COMMAND_REGISTRY_OK;
        case FOF_FW_JSON_REGISTRY_SELECTOR_REJECTED:
            return FOF_SCANNER_COMMAND_REGISTRY_SELECTOR_REJECTED;
        case FOF_FW_JSON_REGISTRY_NO_EXACT_SCHEMA:
        case FOF_FW_JSON_REGISTRY_AMBIGUOUS_SCHEMA:
            return FOF_SCANNER_COMMAND_REGISTRY_FIRMWARE_SCHEMA_REJECTED;
        case FOF_FW_JSON_REGISTRY_INVALID_ARGUMENT:
        default:
            return FOF_SCANNER_COMMAND_REGISTRY_INVALID_ARGUMENT;
    }
}

fof_scanner_command_registry_result_t
fof_scanner_command_select_and_validate(
    const uint8_t *bytes,
    size_t byte_len,
    fof_scanner_deployment_t deployment,
    fof_scanner_command_decision_t *decision_out)
{
    if (decision_out) {
        memset(decision_out, 0, sizeof(*decision_out));
    }
    if (!bytes || byte_len == 0U || !decision_out ||
        (deployment != FOF_SCANNER_DEPLOYMENT_BADGE &&
         deployment != FOF_SCANNER_DEPLOYMENT_NON_BADGE)) {
        return FOF_SCANNER_COMMAND_REGISTRY_INVALID_ARGUMENT;
    }

    bool continue_nonfirmware = false;
    fof_scanner_command_registry_result_t firmware_result =
        route_firmware_first(
            bytes, byte_len, decision_out, &continue_nonfirmware);
    if (firmware_result != FOF_SCANNER_COMMAND_REGISTRY_OK ||
        !continue_nonfirmware) {
        return firmware_result;
    }

    char selector[64] = {0};
    size_t selector_len = 0U;
    if (fof_json_extract_unique_ascii_token_member(
            bytes, byte_len, "type",
            selector, sizeof(selector), &selector_len) !=
        FOF_JSON_SCHEMA_OK) {
        return FOF_SCANNER_COMMAND_REGISTRY_SELECTOR_REJECTED;
    }

    if (mutation_selector_is_known(selector, selector_len)) {
        if (fof_json_validate_exact_object(
                bytes, byte_len,
                TYPE_ONLY_MEMBERS, ARRAY_SIZE(TYPE_ONLY_MEMBERS)) ==
            FOF_JSON_SCHEMA_OK) {
            return FOF_SCANNER_COMMAND_REGISTRY_MUTATION_REFUSED;
        }
        return FOF_SCANNER_COMMAND_REGISTRY_NONFIRMWARE_SCHEMA_REJECTED;
    }

    size_t candidate_count = 0U;
    size_t exact_match_count = 0U;
    const scanner_command_schema_t *exact_schema = NULL;
    fof_json_value_span_t exact_values[SCANNER_COMMAND_MAX_MEMBERS] = {0};

    for (int raw_id = 1; raw_id < FOF_SCANNER_COMMAND_COUNT; ++raw_id) {
        const scanner_command_schema_t *schema =
            &SCANNER_COMMAND_SCHEMAS[raw_id];
        if (!selector_equals(
                selector, selector_len, schema->selector)) {
            continue;
        }
        candidate_count++;
        if (schema->member_count > SCANNER_COMMAND_MAX_MEMBERS) {
            return FOF_SCANNER_COMMAND_REGISTRY_INVALID_ARGUMENT;
        }
        fof_json_value_span_t values[SCANNER_COMMAND_MAX_MEMBERS] = {0};
        if (fof_json_validate_exact_object_capture(
                bytes, byte_len,
                schema->members, schema->member_count,
                values, ARRAY_SIZE(values)) != FOF_JSON_SCHEMA_OK) {
            continue;
        }
        exact_match_count++;
        exact_schema = schema;
        memcpy(exact_values, values, sizeof(exact_values));
    }

    if (candidate_count == 0U) {
        return FOF_SCANNER_COMMAND_REGISTRY_UNKNOWN_SELECTOR;
    }
    if (exact_match_count != 1U || !exact_schema) {
        return FOF_SCANNER_COMMAND_REGISTRY_NONFIRMWARE_SCHEMA_REJECTED;
    }

    fof_scanner_command_t command = {0};
    if (!validate_semantics(
            exact_schema, exact_values, deployment, &command)) {
        return FOF_SCANNER_COMMAND_REGISTRY_SEMANTIC_REJECTED;
    }

    decision_out->route = FOF_SCANNER_COMMAND_ROUTE_NON_FIRMWARE;
    decision_out->firmware_schema_id = FOF_FW_JSON_SCHEMA_NONE;
    decision_out->command = command;
    return FOF_SCANNER_COMMAND_REGISTRY_OK;
}
