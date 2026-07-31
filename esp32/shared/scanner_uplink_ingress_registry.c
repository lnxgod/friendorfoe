#include "scanner_uplink_ingress_registry.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "firmware_json_schema.h"
#include "uart_protocol.h"

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))
#define TOKEN_MEMBER(member_name)                                           \
    {member_name, FOF_JSON_STRING,                                          \
     FOF_JSON_STRING_POLICY_ASCII_TOKEN_NO_ESCAPE}
#define DIAGNOSTIC_MEMBER(member_name)                                      \
    {member_name, FOF_JSON_STRING, FOF_JSON_STRING_POLICY_PRINTABLE_UTF8}
#define BOOL_MEMBER(member_name)                                            \
    {member_name, FOF_JSON_BOOL, FOF_JSON_STRING_POLICY_NONE}
#define INT32_MEMBER(member_name)                                           \
    {member_name, FOF_JSON_INT32, FOF_JSON_STRING_POLICY_NONE}
#define UINT32_MEMBER(member_name)                                          \
    {member_name, FOF_JSON_UINT32, FOF_JSON_STRING_POLICY_NONE}

enum {
    SCANNER_UPLINK_SELECTOR_CAPACITY = 32,
    SCANNER_UPLINK_MAX_ACK_MEMBERS = 15,
};

typedef enum {
    ACK_SEMANTICS_NONE = 0,
    ACK_SEMANTICS_DISPLAY_POLICY_FAILURE,
    ACK_SEMANTICS_RECOVERY_SIMPLE,
    ACK_SEMANTICS_RECOVERY_REBOOT,
    ACK_SEMANTICS_RECOVERY_CLEARED,
#if defined(FOF_DC34_GAME_CANARY)
    ACK_SEMANTICS_CRUD_SELF,
#endif
} ack_semantics_t;

typedef struct {
    fof_scanner_uplink_route_t route;
    const char *selector;
    const fof_json_member_spec_t *members;
    size_t member_count;
    ack_semantics_t semantics;
} ack_schema_t;

static const fof_json_member_spec_t CAL_MODE_ACK_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    BOOL_MEMBER("ok"),
    TOKEN_MEMBER("session_id"),
    TOKEN_MEMBER("scan_mode"),
    TOKEN_MEMBER("calibration_uuid"),
};

static const fof_json_member_spec_t SCAN_PROFILE_ACK_FULL_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("scan_profile"),
    TOKEN_MEMBER("slot_role"),
    BOOL_MEMBER("slot_role_ok"),
};

static const fof_json_member_spec_t SCAN_PROFILE_ACK_BASIC_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("scan_profile"),
};

static const fof_json_member_spec_t DISPLAY_CONTROL_ACK_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    BOOL_MEMBER("button_enabled"),
    TOKEN_MEMBER("view"),
    BOOL_MEMBER("page_lock"),
    INT32_MEMBER("page"),
};

static const fof_json_member_spec_t DISPLAY_POLICY_ACK_SUCCESS_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    UINT32_MEMBER("hash"),
};

static const fof_json_member_spec_t DISPLAY_POLICY_ACK_FAILURE_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    BOOL_MEMBER("ok"),
    UINT32_MEMBER("hash"),
    DIAGNOSTIC_MEMBER("error"),
};

static const fof_json_member_spec_t SCANNER_QUIET_ACK_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    BOOL_MEMBER("ok"),
    BOOL_MEMBER("enabled"),
    UINT32_MEMBER("generation"),
    BOOL_MEMBER("ble_scanning"),
    BOOL_MEMBER("wifi_paused"),
    BOOL_MEMBER("ble_quiesced"),
    BOOL_MEMBER("wifi_quiesced"),
    BOOL_MEMBER("ble_active"),
    BOOL_MEMBER("wifi_active"),
    BOOL_MEMBER("radios_ready"),
    BOOL_MEMBER("tx_restored"),
    BOOL_MEMBER("tx_enabled"),
    BOOL_MEMBER("uart_commands"),
};

static const fof_json_member_spec_t SCANNER_QUIET_ACK_ERROR_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    BOOL_MEMBER("ok"),
    BOOL_MEMBER("enabled"),
    UINT32_MEMBER("generation"),
    BOOL_MEMBER("ble_scanning"),
    BOOL_MEMBER("wifi_paused"),
    BOOL_MEMBER("ble_quiesced"),
    BOOL_MEMBER("wifi_quiesced"),
    BOOL_MEMBER("ble_active"),
    BOOL_MEMBER("wifi_active"),
    BOOL_MEMBER("radios_ready"),
    BOOL_MEMBER("tx_restored"),
    BOOL_MEMBER("tx_enabled"),
    BOOL_MEMBER("uart_commands"),
    DIAGNOSTIC_MEMBER("error"),
};

static const fof_json_member_spec_t RECOVERY_ACK_SIMPLE_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("mode"),
};

static const fof_json_member_spec_t RECOVERY_ACK_REBOOT_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("mode"),
    BOOL_MEMBER("reboot"),
};

static const fof_json_member_spec_t RECOVERY_ACK_CLEARED_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("mode"),
    BOOL_MEMBER("reboot"),
    BOOL_MEMBER("cleared"),
    UINT32_MEMBER("crash_count"),
};

static const fof_json_member_spec_t SCANNER_RECOVERY_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("recovery_mode"),
    DIAGNOSTIC_MEMBER("safe_reason"),
    BOOL_MEMBER("rollback_pending"),
    UINT32_MEMBER("crash_count"),
    TOKEN_MEMBER("ota_state"),
};

#if defined(FOF_DC34_GAME_CANARY)
static const fof_json_member_spec_t CRUD_SELF_ACK_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    UINT32_MEMBER("v"),
    UINT32_MEMBER("round"),
    TOKEN_MEMBER("peer"),
    TOKEN_MEMBER("session"),
};
#endif

#define ACK_SCHEMA(route_value, selector_value, members_value, semantics_value) \
    {route_value, selector_value, members_value, ARRAY_SIZE(members_value),    \
     semantics_value}

static const ack_schema_t ACK_SCHEMAS[] = {
    ACK_SCHEMA(
        FOF_SCANNER_UPLINK_ROUTE_CAL_MODE_ACK,
        MSG_TYPE_CAL_MODE_ACK,
        CAL_MODE_ACK_MEMBERS,
        ACK_SEMANTICS_NONE),
    ACK_SCHEMA(
        FOF_SCANNER_UPLINK_ROUTE_SCAN_PROFILE_ACK,
        "scan_profile_ack",
        SCAN_PROFILE_ACK_FULL_MEMBERS,
        ACK_SEMANTICS_NONE),
    ACK_SCHEMA(
        FOF_SCANNER_UPLINK_ROUTE_SCAN_PROFILE_ACK,
        "scan_profile_ack",
        SCAN_PROFILE_ACK_BASIC_MEMBERS,
        ACK_SEMANTICS_NONE),
    ACK_SCHEMA(
        FOF_SCANNER_UPLINK_ROUTE_DISPLAY_CONTROL_ACK,
        "display_control_ack",
        DISPLAY_CONTROL_ACK_MEMBERS,
        ACK_SEMANTICS_NONE),
    ACK_SCHEMA(
        FOF_SCANNER_UPLINK_ROUTE_DISPLAY_POLICY_ACK,
        "display_policy_ack",
        DISPLAY_POLICY_ACK_SUCCESS_MEMBERS,
        ACK_SEMANTICS_NONE),
    ACK_SCHEMA(
        FOF_SCANNER_UPLINK_ROUTE_DISPLAY_POLICY_ACK,
        "display_policy_ack",
        DISPLAY_POLICY_ACK_FAILURE_MEMBERS,
        ACK_SEMANTICS_DISPLAY_POLICY_FAILURE),
    ACK_SCHEMA(
        FOF_SCANNER_UPLINK_ROUTE_SCANNER_QUIET_ACK,
        MSG_TYPE_SCANNER_QUIET_ACK,
        SCANNER_QUIET_ACK_MEMBERS,
        ACK_SEMANTICS_NONE),
    ACK_SCHEMA(
        FOF_SCANNER_UPLINK_ROUTE_SCANNER_QUIET_ACK,
        MSG_TYPE_SCANNER_QUIET_ACK,
        SCANNER_QUIET_ACK_ERROR_MEMBERS,
        ACK_SEMANTICS_NONE),
    ACK_SCHEMA(
        FOF_SCANNER_UPLINK_ROUTE_RECOVERY_ACK,
        "recovery_ack",
        RECOVERY_ACK_SIMPLE_MEMBERS,
        ACK_SEMANTICS_RECOVERY_SIMPLE),
    ACK_SCHEMA(
        FOF_SCANNER_UPLINK_ROUTE_RECOVERY_ACK,
        "recovery_ack",
        RECOVERY_ACK_REBOOT_MEMBERS,
        ACK_SEMANTICS_RECOVERY_REBOOT),
    ACK_SCHEMA(
        FOF_SCANNER_UPLINK_ROUTE_RECOVERY_ACK,
        "recovery_ack",
        RECOVERY_ACK_CLEARED_MEMBERS,
        ACK_SEMANTICS_RECOVERY_CLEARED),
    ACK_SCHEMA(
        FOF_SCANNER_UPLINK_ROUTE_SCANNER_RECOVERY,
        "scanner_recovery",
        SCANNER_RECOVERY_MEMBERS,
        ACK_SEMANTICS_NONE),
#if defined(FOF_DC34_GAME_CANARY)
    ACK_SCHEMA(
        FOF_SCANNER_UPLINK_ROUTE_CRUD_SELF_ACK,
        "crud_self_ack",
        CRUD_SELF_ACK_MEMBERS,
        ACK_SEMANTICS_CRUD_SELF),
#endif
};

static void decision_reset(
    fof_scanner_uplink_decision_t *decision)
{
    if (decision) {
        memset(decision, 0, sizeof(*decision));
    }
}

static bool text_equals(
    const char *value,
    size_t value_len,
    const char *literal)
{
    size_t literal_len = literal ? strlen(literal) : 0U;
    return value && literal && value_len == literal_len &&
           memcmp(value, literal, literal_len) == 0;
}

static bool span_token_equals(
    const fof_json_value_span_t *raw,
    const char *literal)
{
    fof_json_value_span_t token = {0};
    return fof_json_value_span_parse_ascii_token(raw, &token) &&
           text_equals(
               (const char *)token.bytes, token.byte_len, literal);
}

#if defined(FOF_DC34_GAME_CANARY)
static bool parse_nonzero_upper_hex(
    const fof_json_value_span_t *raw,
    size_t digits,
    uint32_t *value_out)
{
    fof_json_value_span_t token = {0};
    if (!value_out ||
        !fof_json_value_span_parse_ascii_token(raw, &token) ||
        token.byte_len != digits) {
        return false;
    }

    uint32_t value = 0U;
    for (size_t index = 0U; index < token.byte_len; ++index) {
        uint8_t ch = token.bytes[index];
        uint8_t nibble;
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
#endif

static bool firmware_selector_claimed(
    const char *selector,
    size_t selector_len)
{
    for (int id = FOF_FW_JSON_SCHEMA_NONE + 1;
         id < FOF_FW_JSON_SCHEMA_COUNT; ++id) {
        const fof_fw_json_schema_descriptor_t *descriptor =
            fof_fw_json_schema_descriptor(
                (fof_fw_json_schema_id_t)id);
        if (descriptor &&
            descriptor->ingress ==
                FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART &&
            strcmp(descriptor->selector_name, "type") == 0 &&
            text_equals(
                selector, selector_len,
                descriptor->selector_value)) {
            return true;
        }
    }
    return false;
}

static bool ble_selector_claimed(
    const char *selector,
    size_t selector_len)
{
    static const char *const selectors[] = {
        "ble_inv_begin",
        "ble_inv_progress",
        "ble_inv_service",
        "ble_inv_char",
        "ble_inv_read",
        "ble_inv_end",
    };
    for (size_t index = 0U; index < ARRAY_SIZE(selectors); ++index) {
        if (text_equals(selector, selector_len, selectors[index])) {
            return true;
        }
    }
    return false;
}

static bool ack_semantics_valid(
    const ack_schema_t *schema,
    const fof_json_value_span_t *values,
    fof_scanner_uplink_decision_t *decision_out)
{
    bool first = false;
    bool second = false;
    uint32_t count = 0U;
    if (!schema || !values) {
        return false;
    }
    switch (schema->semantics) {
        case ACK_SEMANTICS_NONE:
            return true;
        case ACK_SEMANTICS_DISPLAY_POLICY_FAILURE:
            return fof_json_value_span_parse_bool(
                       &values[1], &first) &&
                   !first;
        case ACK_SEMANTICS_RECOVERY_SIMPLE:
            return span_token_equals(&values[1], "bootloader") ||
                   span_token_equals(&values[1], "reboot");
        case ACK_SEMANTICS_RECOVERY_REBOOT:
            return span_token_equals(&values[1], "safe_uart") &&
                   fof_json_value_span_parse_bool(
                       &values[2], &first) &&
                   first;
        case ACK_SEMANTICS_RECOVERY_CLEARED:
            return span_token_equals(&values[1], "normal") &&
                   fof_json_value_span_parse_bool(
                       &values[2], &first) &&
                   fof_json_value_span_parse_bool(
                       &values[3], &second) &&
                   fof_json_value_span_parse_uint32(
                       &values[4], &count) &&
                   first && second && count == 0U;
#if defined(FOF_DC34_GAME_CANARY)
        case ACK_SEMANTICS_CRUD_SELF: {
            uint32_t version = 0U;
            uint32_t round = 0U;
            uint32_t peer = 0U;
            uint32_t session = 0U;
            if (!decision_out ||
                !fof_json_value_span_parse_uint32(
                    &values[1], &version) ||
                version != 1U ||
                !fof_json_value_span_parse_uint32(
                    &values[2], &round) ||
                round != 34U ||
                !parse_nonzero_upper_hex(&values[3], 6U, &peer) ||
                !parse_nonzero_upper_hex(&values[4], 2U, &session)) {
                return false;
            }
            decision_out->crud_peer = peer;
            decision_out->crud_session = (uint8_t)session;
            return true;
        }
#endif
        default:
            return false;
    }
}

static bool ack_selector_claimed(
    const char *selector,
    size_t selector_len)
{
    for (size_t index = 0U; index < ARRAY_SIZE(ACK_SCHEMAS); ++index) {
        if (text_equals(
                selector, selector_len,
                ACK_SCHEMAS[index].selector)) {
            return true;
        }
    }
    return false;
}

static bool ack_select_and_validate(
    const uint8_t *bytes,
    size_t byte_len,
    const char *selector,
    size_t selector_len,
    fof_scanner_uplink_decision_t *decision_out)
{
    if (!decision_out) {
        return false;
    }
    for (size_t index = 0U; index < ARRAY_SIZE(ACK_SCHEMAS); ++index) {
        const ack_schema_t *schema = &ACK_SCHEMAS[index];
        if (!text_equals(
                selector, selector_len, schema->selector)) {
            continue;
        }
        fof_json_value_span_t
            values[SCANNER_UPLINK_MAX_ACK_MEMBERS] = {0};
        if (fof_json_validate_exact_object_capture(
                bytes, byte_len, schema->members,
                schema->member_count, values,
                ARRAY_SIZE(values)) == FOF_JSON_SCHEMA_OK &&
            ack_semantics_valid(schema, values, decision_out)) {
            decision_out->route = schema->route;
            return true;
        }
    }
    return false;
}

static fof_scanner_uplink_route_t envelope_route(
    const char *selector,
    size_t selector_len)
{
    if (text_equals(
            selector, selector_len, MSG_TYPE_DETECTION)) {
        return FOF_SCANNER_UPLINK_ROUTE_DETECTION;
    }
    if (text_equals(selector, selector_len, MSG_TYPE_STATUS)) {
        return FOF_SCANNER_UPLINK_ROUTE_STATUS;
    }
    if (text_equals(selector, selector_len, "scanner_info")) {
        return FOF_SCANNER_UPLINK_ROUTE_SCANNER_INFO;
    }
    return FOF_SCANNER_UPLINK_ROUTE_NONE;
}

fof_scanner_uplink_ingress_result_t
fof_scanner_uplink_ingress_select_and_validate(
    const uint8_t *bytes,
    size_t byte_len,
    int scanner_slot,
    fof_scanner_uplink_decision_t *decision_out)
{
    decision_reset(decision_out);
    if (!bytes || byte_len == 0U || !decision_out ||
        (scanner_slot != 0 && scanner_slot != 1)) {
        return FOF_SCANNER_UPLINK_INGRESS_INVALID_ARGUMENT;
    }

    char selector[SCANNER_UPLINK_SELECTOR_CAPACITY] = {0};
    size_t selector_len = 0U;
    if (fof_json_extract_unique_ascii_token_member(
            bytes, byte_len, "type", selector, sizeof(selector),
            &selector_len) != FOF_JSON_SCHEMA_OK) {
        return FOF_SCANNER_UPLINK_INGRESS_SELECTOR_REJECTED;
    }

    if (firmware_selector_claimed(selector, selector_len)) {
        fof_fw_json_schema_id_t schema_id =
            FOF_FW_JSON_SCHEMA_NONE;
        if (fof_fw_json_select_and_validate(
                FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
                bytes, byte_len, &schema_id) !=
            FOF_FW_JSON_REGISTRY_OK) {
            return
                FOF_SCANNER_UPLINK_INGRESS_FIRMWARE_SCHEMA_REJECTED;
        }
        decision_out->route = FOF_SCANNER_UPLINK_ROUTE_FIRMWARE;
        decision_out->firmware_schema_id = schema_id;
        return FOF_SCANNER_UPLINK_INGRESS_OK;
    }

    if (ble_selector_claimed(selector, selector_len)) {
        fof_ble_inv_ingress_schema_id_t schema_id =
            FOF_BLE_INV_INGRESS_NONE;
        if (fof_ble_investigation_ingress_validate(
                bytes, byte_len, scanner_slot, &schema_id) !=
            FOF_BLE_INV_INGRESS_OK) {
            return FOF_SCANNER_UPLINK_INGRESS_BLE_SCHEMA_REJECTED;
        }
        decision_out->route =
            FOF_SCANNER_UPLINK_ROUTE_BLE_INVESTIGATION;
        decision_out->ble_schema_id = schema_id;
        return FOF_SCANNER_UPLINK_INGRESS_OK;
    }

    if (text_equals(
            selector, selector_len, MSG_TYPE_BADGE_EASTER_EGG)) {
        return FOF_SCANNER_UPLINK_INGRESS_EASTER_FRAME_REQUIRED;
    }

    if (ack_selector_claimed(selector, selector_len)) {
#if defined(FOF_DC34_GAME_CANARY)
        if (text_equals(
                selector, selector_len, "crud_self_ack") &&
            scanner_slot != 0) {
            return
                FOF_SCANNER_UPLINK_INGRESS_TELEMETRY_SCHEMA_REJECTED;
        }
#endif
        if (!ack_select_and_validate(
                bytes, byte_len, selector, selector_len, decision_out)) {
            decision_reset(decision_out);
            return
                FOF_SCANNER_UPLINK_INGRESS_TELEMETRY_SCHEMA_REJECTED;
        }
        return FOF_SCANNER_UPLINK_INGRESS_OK;
    }

    fof_scanner_uplink_route_t route =
        envelope_route(selector, selector_len);
    if (route == FOF_SCANNER_UPLINK_ROUTE_NONE) {
        return FOF_SCANNER_UPLINK_INGRESS_UNKNOWN_SELECTOR;
    }
    decision_out->route = route;
    return FOF_SCANNER_UPLINK_INGRESS_OK;
}
