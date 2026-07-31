#include "serial_config_ingress.h"

#include "firmware_json_schema.h"
#include "serial_config.h"

#include <string.h>

#define SERIAL_CONFIG_MAX_LINE_BYTES 2047U
#define SPAN_LITERAL(value)                                                \
    {(const uint8_t *)(value), sizeof(value) - 1U}

typedef struct {
    const uint8_t *bytes;
    size_t length;
} serial_config_literal_t;

static const uint8_t CMD_PREFIX[] = "FOF_SET:";
static const uint8_t CMD_CTL[] = "FOF_CTL:";
static const uint8_t CMD_STATUS[] = "FOF_STATUS";
static const uint8_t CMD_SAVE[] = "FOF_SAVE";
static const uint8_t CMD_PING[] = "FOF_PING";
static const uint8_t CMD_REBOOT[] = "FOF_REBOOT";
static const uint8_t CMD_BOOTLOADER[] = "FOF_BOOTLOADER";
static const uint8_t CMD_DOWNLOAD[] = "FOF_DOWNLOAD";
static const uint8_t CMD_FLASH[] = "FOF_FLASH";

static const serial_config_literal_t ALLOWED_KEYS[] = {
    SPAN_LITERAL("wifi_ssid"),
    SPAN_LITERAL("wifi_pass"),
    SPAN_LITERAL("backend_url"),
    SPAN_LITERAL("device_id"),
    SPAN_LITERAL("ap_ssid"),
    SPAN_LITERAL("ap_pass"),
    SPAN_LITERAL("badge_mode"),
    SPAN_LITERAL("badge_display_debug"),
    SPAN_LITERAL("badge_display_policy_v1"),
#if defined(FOF_DC34_GAME_CANARY)
    SPAN_LITERAL("game_seed"),
#endif
};

#if defined(FOF_DC34_GAME_CANARY)
static const serial_config_literal_t GAME_SEEDS[] = {
    SPAN_LITERAL("normal"),
    SPAN_LITERAL("infected"),
    SPAN_LITERAL("immune"),
};
#endif

static const serial_config_literal_t FIRMWARE_SELECTORS[] = {
    SPAN_LITERAL("fw_relay"),
    SPAN_LITERAL("fw_upload_begin"),
    SPAN_LITERAL("uplink_ota_begin"),
    SPAN_LITERAL("fw_check"),
    SPAN_LITERAL("fw_check_now"),
};

static bool span_equals(const uint8_t *bytes,
                        size_t length,
                        const uint8_t *literal,
                        size_t literal_length)
{
    return bytes && literal && length == literal_length &&
           memcmp(bytes, literal, literal_length) == 0;
}

static bool span_starts_with(const uint8_t *bytes,
                             size_t length,
                             const uint8_t *prefix,
                             size_t prefix_length)
{
    return bytes && prefix && length >= prefix_length &&
           memcmp(bytes, prefix, prefix_length) == 0;
}

static bool literal_table_contains(
    const serial_config_literal_t *values,
    size_t value_count,
    const uint8_t *candidate,
    size_t candidate_length)
{
    if (!values || !candidate) {
        return false;
    }
    for (size_t i = 0U; i < value_count; i++) {
        if (span_equals(candidate, candidate_length,
                        values[i].bytes, values[i].length)) {
            return true;
        }
    }
    return false;
}

static bool selector_is_firmware(const uint8_t *selector,
                                 size_t selector_length)
{
    return literal_table_contains(
        FIRMWARE_SELECTORS,
        sizeof(FIRMWARE_SELECTORS) / sizeof(FIRMWARE_SELECTORS[0]),
        selector, selector_length);
}

static bool schema_is_uplink_ota_begin(fof_fw_json_schema_id_t schema_id)
{
    return schema_id == FOF_FW_JSON_SCHEMA_USB_UPLINK_OTA_BEGIN ||
           schema_id ==
               FOF_FW_JSON_SCHEMA_USB_UPLINK_OTA_BEGIN_SESSION;
}

static bool ctl_payload(const uint8_t *line,
                        size_t line_byte_len,
                        const uint8_t **json,
                        size_t *json_byte_len)
{
    const size_t prefix_length = sizeof(CMD_CTL) - 1U;
    if (!json || !json_byte_len ||
        !span_starts_with(line, line_byte_len, CMD_CTL, prefix_length) ||
        line_byte_len == prefix_length) {
        return false;
    }
    *json = line + prefix_length;
    *json_byte_len = line_byte_len - prefix_length;
    return true;
}

bool serial_config_ingress_parse_set(
    const uint8_t *line,
    size_t line_byte_len,
    serial_config_set_parts_t *parts)
{
    if (parts) {
        memset(parts, 0, sizeof(*parts));
    }
    const size_t prefix_length = sizeof(CMD_PREFIX) - 1U;
    if (!parts || !line || line_byte_len > SERIAL_CONFIG_MAX_LINE_BYTES ||
        !span_starts_with(line, line_byte_len, CMD_PREFIX, prefix_length)) {
        return false;
    }

    const uint8_t *payload = line + prefix_length;
    size_t payload_length = line_byte_len - prefix_length;
    const uint8_t *separator = memchr(payload, '=', payload_length);
    if (!separator || separator == payload) {
        return false;
    }

    size_t key_length = (size_t)(separator - payload);
    if (key_length > 31U ||
        !literal_table_contains(
            ALLOWED_KEYS, sizeof(ALLOWED_KEYS) / sizeof(ALLOWED_KEYS[0]),
            payload, key_length)) {
        return false;
    }

    const uint8_t *value = separator + 1U;
    size_t value_length =
        line_byte_len - (size_t)(value - line);
    for (size_t i = 0U; i < value_length; i++) {
        uint8_t byte = value[i];
        if (byte < 0x20U || byte == 0x7fU) {
            return false;
        }
    }
#if defined(FOF_DC34_GAME_CANARY)
    static const uint8_t GAME_SEED_KEY[] = "game_seed";
    if (span_equals(payload, key_length,
                    GAME_SEED_KEY, sizeof(GAME_SEED_KEY) - 1U) &&
        !literal_table_contains(
            GAME_SEEDS, sizeof(GAME_SEEDS) / sizeof(GAME_SEEDS[0]),
            value, value_length)) {
        return false;
    }
#endif

    parts->key = payload;
    parts->key_len = key_length;
    parts->value = value;
    parts->value_len = value_length;
    return true;
}

bool serial_config_ingress_authorize(
    const uint8_t *line,
    size_t line_byte_len,
    serial_config_ingress_result_t *result)
{
    if (result) {
        memset(result, 0, sizeof(*result));
    }
    if (!result || !line || line_byte_len == 0U ||
        line_byte_len > SERIAL_CONFIG_MAX_LINE_BYTES) {
        return false;
    }

    if (span_equals(line, line_byte_len, CMD_PING, sizeof(CMD_PING) - 1U)) {
        result->kind = SERIAL_CONFIG_INGRESS_PING;
        return true;
    }
    if (span_equals(
            line, line_byte_len, CMD_STATUS, sizeof(CMD_STATUS) - 1U)) {
        result->kind = SERIAL_CONFIG_INGRESS_STATUS;
        return true;
    }
    if (span_equals(line, line_byte_len, CMD_SAVE, sizeof(CMD_SAVE) - 1U)) {
        result->kind = SERIAL_CONFIG_INGRESS_SAVE;
        return true;
    }
    if (span_equals(
            line, line_byte_len, CMD_REBOOT, sizeof(CMD_REBOOT) - 1U)) {
        result->kind = SERIAL_CONFIG_INGRESS_REBOOT;
        return true;
    }
    serial_config_set_parts_t set_parts;
    if (serial_config_ingress_parse_set(line, line_byte_len, &set_parts)) {
        result->kind = SERIAL_CONFIG_INGRESS_SET;
        return true;
    }

    const uint8_t *json = NULL;
    size_t json_byte_len = 0U;
    if (!ctl_payload(line, line_byte_len, &json, &json_byte_len)) {
        return false;
    }

    char selector[64] = {0};
    size_t selector_length = 0U;
    if (fof_json_extract_unique_ascii_token_member(
            json, json_byte_len, "cmd", selector, sizeof(selector),
            &selector_length) != FOF_JSON_SCHEMA_OK) {
        return false;
    }

    if (selector_is_firmware(
            (const uint8_t *)selector, selector_length)) {
        fof_fw_json_schema_id_t schema_id = FOF_FW_JSON_SCHEMA_NONE;
        if (fof_fw_json_select_and_validate(
                FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
                json, json_byte_len, &schema_id) !=
            FOF_FW_JSON_REGISTRY_OK) {
            return false;
        }
        result->kind = SERIAL_CONFIG_INGRESS_CTL_FIRMWARE;
        result->firmware_schema_id = schema_id;
        return true;
    }

    badge_usb_control_schema_id_t control_schema_id =
        BADGE_USB_CONTROL_SCHEMA_NONE;
    badge_usb_control_handler_kind_t control_handler_kind =
        BADGE_USB_CONTROL_HANDLER_NONE;
    if (badge_usb_control_select_and_validate(
            json, json_byte_len, &control_schema_id,
            &control_handler_kind) != BADGE_USB_CONTROL_REGISTRY_OK) {
        return false;
    }
    result->kind = SERIAL_CONFIG_INGRESS_CTL_COMPAT;
    result->control_schema_id = control_schema_id;
    result->control_handler_kind = control_handler_kind;
    return true;
}

bool serial_config_ingress_is_uplink_ota_begin(
    const uint8_t *line,
    size_t line_byte_len)
{
    serial_config_ingress_result_t result;
    return serial_config_ingress_authorize(
               line, line_byte_len, &result) &&
           result.kind == SERIAL_CONFIG_INGRESS_CTL_FIRMWARE &&
           schema_is_uplink_ota_begin(result.firmware_schema_id);
}

bool serial_config_line_is_recognized(
    const uint8_t *line,
    size_t line_byte_len)
{
    serial_config_ingress_result_t result;
    return serial_config_ingress_authorize(
        line, line_byte_len, &result);
}

serial_config_recovery_command_t serial_config_recovery_command_classify(
    const uint8_t *line,
    size_t line_byte_len)
{
    if (span_equals(
            line, line_byte_len, CMD_BOOTLOADER,
            sizeof(CMD_BOOTLOADER) - 1U) ||
        span_equals(
            line, line_byte_len, CMD_DOWNLOAD,
            sizeof(CMD_DOWNLOAD) - 1U) ||
        span_equals(
            line, line_byte_len, CMD_FLASH, sizeof(CMD_FLASH) - 1U)) {
        return SERIAL_CONFIG_RECOVERY_ROM_BOOT;
    }

    serial_config_ingress_result_t result;
    if (!serial_config_ingress_authorize(
            line, line_byte_len, &result)) {
        return SERIAL_CONFIG_RECOVERY_DENIED;
    }
    switch (result.kind) {
        case SERIAL_CONFIG_INGRESS_PING:
            return SERIAL_CONFIG_RECOVERY_PING;
        case SERIAL_CONFIG_INGRESS_STATUS:
            return SERIAL_CONFIG_RECOVERY_STATUS;
        case SERIAL_CONFIG_INGRESS_REBOOT:
            return SERIAL_CONFIG_RECOVERY_APP_REBOOT;
        case SERIAL_CONFIG_INGRESS_CTL_FIRMWARE:
            return schema_is_uplink_ota_begin(
                       result.firmware_schema_id)
                ? SERIAL_CONFIG_RECOVERY_UPLINK_OTA_BEGIN
                : SERIAL_CONFIG_RECOVERY_DENIED;
        case SERIAL_CONFIG_INGRESS_CTL_COMPAT:
            if (result.control_schema_id ==
                BADGE_USB_CONTROL_SCHEMA_STATUS) {
                return SERIAL_CONFIG_RECOVERY_STATUS;
            }
            if (result.control_schema_id ==
                BADGE_USB_CONTROL_SCHEMA_REBOOT) {
                return SERIAL_CONFIG_RECOVERY_APP_REBOOT;
            }
            return SERIAL_CONFIG_RECOVERY_DENIED;
        case SERIAL_CONFIG_INGRESS_SAVE:
        case SERIAL_CONFIG_INGRESS_SET:
        case SERIAL_CONFIG_INGRESS_REJECTED:
        default:
            return SERIAL_CONFIG_RECOVERY_DENIED;
    }
}
