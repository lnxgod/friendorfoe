#include "firmware_json_schema_registry.h"

#include <string.h>

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))
#define TOKEN_MEMBER(member_name)                                           \
    {member_name, FOF_JSON_STRING,                                          \
     FOF_JSON_STRING_POLICY_ASCII_TOKEN_NO_ESCAPE}
#define DIAGNOSTIC_MEMBER(member_name)                                      \
    {member_name, FOF_JSON_STRING, FOF_JSON_STRING_POLICY_PRINTABLE_UTF8}
#define UINT32_MEMBER(member_name)                                          \
    {member_name, FOF_JSON_UINT32, FOF_JSON_STRING_POLICY_NONE}
#define BOOL_MEMBER(member_name)                                            \
    {member_name, FOF_JSON_BOOL, FOF_JSON_STRING_POLICY_NONE}

static const fof_json_member_spec_t CMD_ONLY_MEMBERS[] = {
    TOKEN_MEMBER("cmd"),
};

static const fof_json_member_spec_t CMD_UART_MEMBERS[] = {
    TOKEN_MEMBER("cmd"),
    TOKEN_MEMBER("uart"),
};

static const fof_json_member_spec_t TYPE_ONLY_MEMBERS[] = {
    TOKEN_MEMBER("type"),
};

static const fof_json_member_spec_t USB_FW_RELAY_BASE_MEMBERS[] = {
    TOKEN_MEMBER("cmd"),
    TOKEN_MEMBER("uart"),
    UINT32_MEMBER("expected_generation"),
    TOKEN_MEMBER("expected_hardware_id"),
    BOOL_MEMBER("allow_same_version"),
};

static const fof_json_member_spec_t USB_FW_RELAY_FORCED_MEMBERS[] = {
    TOKEN_MEMBER("cmd"),
    TOKEN_MEMBER("uart"),
    UINT32_MEMBER("expected_generation"),
    TOKEN_MEMBER("expected_hardware_id"),
    BOOL_MEMBER("allow_same_version"),
    BOOL_MEMBER("force"),
    BOOL_MEMBER("skip_command_probe"),
};

static const fof_json_member_spec_t USB_FW_UPLOAD_BEGIN_MEMBERS[] = {
    TOKEN_MEMBER("cmd"),
    TOKEN_MEMBER("name"),
    TOKEN_MEMBER("target"),
    TOKEN_MEMBER("project"),
    TOKEN_MEMBER("hardware_type"),
    TOKEN_MEMBER("version"),
    UINT32_MEMBER("size"),
    UINT32_MEMBER("crc32"),
    TOKEN_MEMBER("sha256"),
    UINT32_MEMBER("slot_mask"),
    TOKEN_MEMBER("flow_control"),
};

static const fof_json_member_spec_t
USB_FW_UPLOAD_BEGIN_SESSION_MEMBERS[] = {
    TOKEN_MEMBER("cmd"),
    TOKEN_MEMBER("name"),
    TOKEN_MEMBER("target"),
    TOKEN_MEMBER("project"),
    TOKEN_MEMBER("hardware_type"),
    TOKEN_MEMBER("version"),
    UINT32_MEMBER("size"),
    UINT32_MEMBER("crc32"),
    TOKEN_MEMBER("sha256"),
    UINT32_MEMBER("slot_mask"),
    TOKEN_MEMBER("flow_control"),
    TOKEN_MEMBER("session"),
};

static const fof_json_member_spec_t USB_UPLINK_OTA_BEGIN_MEMBERS[] = {
    TOKEN_MEMBER("cmd"),
    TOKEN_MEMBER("target"),
    TOKEN_MEMBER("project"),
    TOKEN_MEMBER("hardware_type"),
    TOKEN_MEMBER("version"),
    UINT32_MEMBER("size"),
    UINT32_MEMBER("crc32"),
    TOKEN_MEMBER("sha256"),
    TOKEN_MEMBER("flow_control"),
    BOOL_MEMBER("recovery_rewrite_same_version"),
};

static const fof_json_member_spec_t
USB_UPLINK_OTA_BEGIN_SESSION_MEMBERS[] = {
    TOKEN_MEMBER("cmd"),
    TOKEN_MEMBER("target"),
    TOKEN_MEMBER("project"),
    TOKEN_MEMBER("hardware_type"),
    TOKEN_MEMBER("version"),
    UINT32_MEMBER("size"),
    UINT32_MEMBER("crc32"),
    TOKEN_MEMBER("sha256"),
    TOKEN_MEMBER("flow_control"),
    BOOL_MEMBER("recovery_rewrite_same_version"),
    TOKEN_MEMBER("session"),
};

static const fof_json_member_spec_t SCANNER_FW_OFFER_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    BOOL_MEMBER("update"),
    TOKEN_MEMBER("target_ver"),
    TOKEN_MEMBER("fw_name"),
    TOKEN_MEMBER("app_project"),
    TOKEN_MEMBER("hardware_type"),
    TOKEN_MEMBER("sha256"),
    UINT32_MEMBER("generation"),
    UINT32_MEMBER("size"),
    UINT32_MEMBER("crc"),
    DIAGNOSTIC_MEMBER("reason"),
};

static const fof_json_member_spec_t SCANNER_OTA_COMMAND_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("session_id"),
    UINT32_MEMBER("size"),
    UINT32_MEMBER("crc"),
    TOKEN_MEMBER("sha256"),
    TOKEN_MEMBER("target_ver"),
    TOKEN_MEMBER("fw_name"),
    TOKEN_MEMBER("app_project"),
    TOKEN_MEMBER("hardware_type"),
    UINT32_MEMBER("generation"),
    BOOL_MEMBER("allow_same_version"),
};

static const fof_json_member_spec_t TYPE_SESSION_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("session_id"),
};

static const fof_json_member_spec_t RECEIPT_FW_CHECK_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("board"),
    TOKEN_MEMBER("ver"),
    TOKEN_MEMBER("caps"),
    TOKEN_MEMBER("fw_state"),
    UINT32_MEMBER("fw_check_count"),
    DIAGNOSTIC_MEMBER("last_fw_error"),
    DIAGNOSTIC_MEMBER("reason"),
    TOKEN_MEMBER("ota_state"),
    TOKEN_MEMBER("recovery_mode"),
    BOOL_MEMBER("rollback_pending"),
    UINT32_MEMBER("crash_count"),
};

static const fof_json_member_spec_t RECEIPT_FW_READY_STRICT_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("board"),
    TOKEN_MEMBER("ver"),
    TOKEN_MEMBER("target_ver"),
    TOKEN_MEMBER("fw_name"),
    TOKEN_MEMBER("app_project"),
    TOKEN_MEMBER("hardware_type"),
    TOKEN_MEMBER("sha256"),
    UINT32_MEMBER("generation"),
    UINT32_MEMBER("size"),
    UINT32_MEMBER("crc"),
    BOOL_MEMBER("allow_same_version"),
};

static const fof_json_member_spec_t RECEIPT_FW_READY_LEGACY_68_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("board"),
    TOKEN_MEMBER("ver"),
    TOKEN_MEMBER("target_ver"),
    UINT32_MEMBER("size"),
    UINT32_MEMBER("crc"),
};

static const fof_json_member_spec_t RECEIPT_FULL_MANIFEST_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("session_id"),
    TOKEN_MEMBER("target_ver"),
    TOKEN_MEMBER("fw_name"),
    TOKEN_MEMBER("app_project"),
    TOKEN_MEMBER("hardware_type"),
    TOKEN_MEMBER("sha256"),
    UINT32_MEMBER("generation"),
    UINT32_MEMBER("size"),
    UINT32_MEMBER("crc"),
    BOOL_MEMBER("allow_same_version"),
    UINT32_MEMBER("received"),
};

static const fof_json_member_spec_t RECEIPT_OTA_DONE_LEGACY_68_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("session_id"),
    UINT32_MEMBER("received"),
};

static const fof_json_member_spec_t RECEIPT_OTA_PROGRESS_ACTIVE_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("session_id"),
    UINT32_MEMBER("received"),
    UINT32_MEMBER("total"),
    UINT32_MEMBER("percent"),
};

static const fof_json_member_spec_t RECEIPT_OTA_PROGRESS_UNBOUND_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    UINT32_MEMBER("received"),
    UINT32_MEMBER("total"),
    UINT32_MEMBER("percent"),
};

static const fof_json_member_spec_t RECEIPT_OTA_NACK_ACTIVE_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("session_id"),
    UINT32_MEMBER("seq"),
};

static const fof_json_member_spec_t RECEIPT_OTA_NACK_UNBOUND_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    UINT32_MEMBER("seq"),
};

static const fof_json_member_spec_t RECEIPT_OTA_ERROR_ACTIVE_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    TOKEN_MEMBER("session_id"),
    DIAGNOSTIC_MEMBER("reason"),
    UINT32_MEMBER("received"),
};

static const fof_json_member_spec_t RECEIPT_OTA_ERROR_PRESESSION_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    DIAGNOSTIC_MEMBER("reason"),
};

static const fof_json_member_spec_t
RECEIPT_OTA_ERROR_PRESESSION_LEGACY_68_MEMBERS[] = {
    TOKEN_MEMBER("type"),
    DIAGNOSTIC_MEMBER("reason"),
    UINT32_MEMBER("received"),
};

#define SCHEMA_DESCRIPTOR(schema_id, ingress_value, schema_name,             \
                          selector_key, selector_token, member_array)         \
    [schema_id] = {                                                           \
        .id = schema_id,                                                      \
        .ingress = ingress_value,                                             \
        .name = schema_name,                                                  \
        .selector_name = selector_key,                                        \
        .selector_value = selector_token,                                     \
        .members = member_array,                                              \
        .member_count = ARRAY_SIZE(member_array),                             \
    }

static const fof_fw_json_schema_descriptor_t
FIRMWARE_SCHEMAS[FOF_FW_JSON_SCHEMA_COUNT] = {
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_USB_FW_RELAY_BASE,
        FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
        "usb_fw_relay_base", "cmd", "fw_relay",
        USB_FW_RELAY_BASE_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_USB_FW_RELAY_FORCED,
        FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
        "usb_fw_relay_forced", "cmd", "fw_relay",
        USB_FW_RELAY_FORCED_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_USB_FW_UPLOAD_BEGIN,
        FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
        "usb_fw_upload_begin", "cmd", "fw_upload_begin",
        USB_FW_UPLOAD_BEGIN_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_USB_FW_UPLOAD_BEGIN_SESSION,
        FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
        "usb_fw_upload_begin_session", "cmd", "fw_upload_begin",
        USB_FW_UPLOAD_BEGIN_SESSION_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_USB_UPLINK_OTA_BEGIN,
        FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
        "usb_uplink_ota_begin", "cmd", "uplink_ota_begin",
        USB_UPLINK_OTA_BEGIN_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_USB_UPLINK_OTA_BEGIN_SESSION,
        FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
        "usb_uplink_ota_begin_session", "cmd", "uplink_ota_begin",
        USB_UPLINK_OTA_BEGIN_SESSION_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_USB_FW_CHECK,
        FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
        "usb_fw_check", "cmd", "fw_check", CMD_ONLY_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_USB_FW_CHECK_UART,
        FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
        "usb_fw_check_uart", "cmd", "fw_check", CMD_UART_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_USB_FW_CHECK_NOW,
        FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
        "usb_fw_check_now", "cmd", "fw_check_now", CMD_ONLY_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_USB_FW_CHECK_NOW_UART,
        FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
        "usb_fw_check_now_uart", "cmd", "fw_check_now",
        CMD_UART_MEMBERS),

    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_SCANNER_FW_OFFER,
        FOF_FW_JSON_INGRESS_UPLINK_TO_SCANNER_UART,
        "scanner_fw_offer", "type", "fw_offer",
        SCANNER_FW_OFFER_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_SCANNER_FW_CHECK_NOW,
        FOF_FW_JSON_INGRESS_UPLINK_TO_SCANNER_UART,
        "scanner_fw_check_now", "type", "fw_check_now",
        TYPE_ONLY_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_SCANNER_OTA_BEGIN,
        FOF_FW_JSON_INGRESS_UPLINK_TO_SCANNER_UART,
        "scanner_ota_begin", "type", "ota_begin",
        SCANNER_OTA_COMMAND_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_SCANNER_OTA_END,
        FOF_FW_JSON_INGRESS_UPLINK_TO_SCANNER_UART,
        "scanner_ota_end", "type", "ota_end",
        SCANNER_OTA_COMMAND_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_SCANNER_OTA_ABORT_ACTIVE,
        FOF_FW_JSON_INGRESS_UPLINK_TO_SCANNER_UART,
        "scanner_ota_abort_active", "type", "ota_abort",
        TYPE_SESSION_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_SCANNER_OTA_ABORT_UNBOUND,
        FOF_FW_JSON_INGRESS_UPLINK_TO_SCANNER_UART,
        "scanner_ota_abort_unbound", "type", "ota_abort",
        TYPE_ONLY_MEMBERS),

    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_RECEIPT_FW_CHECK,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_fw_check", "type", "fw_check",
        RECEIPT_FW_CHECK_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_RECEIPT_FW_READY_STRICT,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_fw_ready_strict", "type", "fw_ready",
        RECEIPT_FW_READY_STRICT_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_RECEIPT_FW_READY_LEGACY_68,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_fw_ready_legacy_68", "type", "fw_ready",
        RECEIPT_FW_READY_LEGACY_68_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_ACK_MODERN,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_ota_ack_modern", "type", "ota_ack",
        RECEIPT_FULL_MANIFEST_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_ACK_LEGACY_68,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_ota_ack_legacy_68", "type", "ota_ack",
        TYPE_SESSION_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_STAGED_MODERN,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_ota_staged_modern", "type", "ota_staged",
        RECEIPT_FULL_MANIFEST_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_DONE_MODERN,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_ota_done_modern", "type", "ota_done",
        RECEIPT_FULL_MANIFEST_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_DONE_LEGACY_68,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_ota_done_legacy_68", "type", "ota_done",
        RECEIPT_OTA_DONE_LEGACY_68_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_PROGRESS_ACTIVE_SHARED,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_ota_progress_active_shared", "type", "ota_progress",
        RECEIPT_OTA_PROGRESS_ACTIVE_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_PROGRESS_UNBOUND_MODERN,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_ota_progress_unbound_modern", "type", "ota_progress",
        RECEIPT_OTA_PROGRESS_UNBOUND_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_NACK_ACTIVE_SHARED,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_ota_nack_active_shared", "type", "ota_nack",
        RECEIPT_OTA_NACK_ACTIVE_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_NACK_UNBOUND_SHARED,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_ota_nack_unbound_shared", "type", "ota_nack",
        RECEIPT_OTA_NACK_UNBOUND_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_ERROR_ACTIVE_SHARED,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_ota_error_active_shared", "type", "ota_error",
        RECEIPT_OTA_ERROR_ACTIVE_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_ERROR_PRESESSION_MODERN,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_ota_error_presession_modern", "type", "ota_error",
        RECEIPT_OTA_ERROR_PRESESSION_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_ERROR_PRESESSION_LEGACY_68,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_ota_error_presession_legacy_68", "type", "ota_error",
        RECEIPT_OTA_ERROR_PRESESSION_LEGACY_68_MEMBERS),
    SCHEMA_DESCRIPTOR(
        FOF_FW_JSON_SCHEMA_RECEIPT_STOP_ACK_SHARED,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_stop_ack_shared", "type", "stop_ack",
        TYPE_ONLY_MEMBERS),
};

static const char *selector_name_for_ingress(fof_fw_json_ingress_t ingress)
{
    switch (ingress) {
        case FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB:
            return "cmd";
        case FOF_FW_JSON_INGRESS_UPLINK_TO_SCANNER_UART:
        case FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART:
            return "type";
        default:
            return NULL;
    }
}

const fof_fw_json_schema_descriptor_t *
fof_fw_json_schema_descriptor(fof_fw_json_schema_id_t id)
{
    if (id <= FOF_FW_JSON_SCHEMA_NONE ||
        id >= FOF_FW_JSON_SCHEMA_COUNT ||
        FIRMWARE_SCHEMAS[id].id != id) {
        return NULL;
    }
    return &FIRMWARE_SCHEMAS[id];
}

fof_fw_json_registry_result_t fof_fw_json_select_and_validate(
    fof_fw_json_ingress_t ingress,
    const uint8_t *bytes,
    size_t byte_len,
    fof_fw_json_schema_id_t *schema_id_out)
{
    if (schema_id_out) {
        *schema_id_out = FOF_FW_JSON_SCHEMA_NONE;
    }

    const char *selector_name = selector_name_for_ingress(ingress);
    if (!schema_id_out || !bytes || byte_len == 0U || !selector_name) {
        return FOF_FW_JSON_REGISTRY_INVALID_ARGUMENT;
    }

    char selector_value[64] = {0};
    size_t selector_value_len = 0U;
    if (fof_json_extract_unique_ascii_token_member(
            bytes, byte_len, selector_name,
            selector_value, sizeof(selector_value),
            &selector_value_len) != FOF_JSON_SCHEMA_OK) {
        return FOF_FW_JSON_REGISTRY_SELECTOR_REJECTED;
    }

    size_t candidate_count = 0U;
    size_t exact_match_count = 0U;
    fof_fw_json_schema_id_t exact_match = FOF_FW_JSON_SCHEMA_NONE;
    for (int raw_id = 1; raw_id < FOF_FW_JSON_SCHEMA_COUNT; ++raw_id) {
        const fof_fw_json_schema_descriptor_t *descriptor =
            &FIRMWARE_SCHEMAS[raw_id];
        if (descriptor->ingress != ingress ||
            strlen(descriptor->selector_value) != selector_value_len ||
            memcmp(descriptor->selector_value,
                   selector_value,
                   selector_value_len) != 0) {
            continue;
        }
        candidate_count++;
        if (fof_json_validate_exact_object(
                bytes, byte_len,
                descriptor->members,
                descriptor->member_count) == FOF_JSON_SCHEMA_OK) {
            exact_match_count++;
            exact_match = descriptor->id;
        }
    }

    if (candidate_count == 0U) {
        return FOF_FW_JSON_REGISTRY_UNKNOWN_SELECTOR;
    }
    if (exact_match_count == 0U) {
        return FOF_FW_JSON_REGISTRY_NO_EXACT_SCHEMA;
    }
    if (exact_match_count != 1U) {
        return FOF_FW_JSON_REGISTRY_AMBIGUOUS_SCHEMA;
    }

    *schema_id_out = exact_match;
    return FOF_FW_JSON_REGISTRY_OK;
}
