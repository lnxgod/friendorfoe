#include "badge_usb_control_schema.h"

#include "badge_display_policy.h"
#include "badge_theme.h"
#if defined(FOF_DC34_GAME_CANARY)
#include "badge_update_maintenance_policy.h"
#endif

#include <stdbool.h>
#include <string.h>

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))
#define TOKEN_MEMBER(member_name)                                           \
    {member_name, FOF_JSON_STRING,                                          \
     FOF_JSON_STRING_POLICY_ASCII_TOKEN_NO_ESCAPE}
#define TEXT_MEMBER(member_name)                                            \
    {member_name, FOF_JSON_STRING,                                          \
     FOF_JSON_STRING_POLICY_PRINTABLE_UTF8}
#define NULLABLE_TOKEN_MEMBER(member_name)                                  \
    {member_name, FOF_JSON_NULLABLE_STRING,                                 \
     FOF_JSON_STRING_POLICY_ASCII_TOKEN_NO_ESCAPE}
#define BOOL_MEMBER(member_name)                                            \
    {member_name, FOF_JSON_BOOL, FOF_JSON_STRING_POLICY_NONE}
#define INT32_MEMBER(member_name)                                           \
    {member_name, FOF_JSON_INT32, FOF_JSON_STRING_POLICY_NONE}
#define UINT32_MEMBER(member_name)                                          \
    {member_name, FOF_JSON_UINT32, FOF_JSON_STRING_POLICY_NONE}
#define OBJECT_MEMBER(member_name)                                          \
    {member_name, FOF_JSON_OBJECT, FOF_JSON_STRING_POLICY_NONE}

enum {
    BADGE_USB_CONTROL_MAX_MEMBERS = 8,
};

static const fof_json_member_spec_t CMD_ONLY[] = {
    TOKEN_MEMBER("cmd"),
};
#if defined(FOF_DC34_GAME_CANARY)
static const fof_json_member_spec_t UPDATE_SESSION[] = {
    TOKEN_MEMBER("cmd"),
    TOKEN_MEMBER("session"),
};
#endif
static const fof_json_member_spec_t POWER_MODE[] = {
    TOKEN_MEMBER("cmd"),
    TOKEN_MEMBER("mode"),
};
static const fof_json_member_spec_t SET_MODE_PERSISTENT[] = {
    TOKEN_MEMBER("cmd"),
    TOKEN_MEMBER("mode"),
    BOOL_MEMBER("persist"),
};
static const fof_json_member_spec_t SET_MODE_SESSION[] = {
    TOKEN_MEMBER("cmd"),
    TOKEN_MEMBER("mode"),
    INT32_MEMBER("ttl_s"),
};
static const fof_json_member_spec_t SET_BACKEND[] = {
    TOKEN_MEMBER("cmd"),
    TEXT_MEMBER("url"),
    TEXT_MEMBER("wifi_ssid"),
    TEXT_MEMBER("wifi_pass"),
    BOOL_MEMBER("enable"),
    INT32_MEMBER("ttl_s"),
};
static const fof_json_member_spec_t CMD_ENABLED[] = {
    TOKEN_MEMBER("cmd"),
    BOOL_MEMBER("enabled"),
};
static const fof_json_member_spec_t NETWORK[] = {
    TOKEN_MEMBER("cmd"),
    TOKEN_MEMBER("mode"),
    INT32_MEMBER("ttl_s"),
};
static const fof_json_member_spec_t SAFE_MODE[] = {
    TOKEN_MEMBER("cmd"),
    BOOL_MEMBER("enabled"),
    TEXT_MEMBER("reason"),
};
static const fof_json_member_spec_t BLE_INVESTIGATE[] = {
    TOKEN_MEMBER("cmd"),
    TOKEN_MEMBER("request_id"),
    TOKEN_MEMBER("mode"),
    NULLABLE_TOKEN_MEMBER("target"),
    UINT32_MEMBER("timeout_ms"),
};
static const fof_json_member_spec_t BLE_CHUNK[] = {
    TOKEN_MEMBER("cmd"),
    TOKEN_MEMBER("request_id"),
    UINT32_MEMBER("seq"),
};
static const fof_json_member_spec_t DISPLAY_POLICY[] = {
    TOKEN_MEMBER("cmd"),
    OBJECT_MEMBER("policy"),
    BOOL_MEMBER("persist"),
};
static const fof_json_member_spec_t THEME[] = {
    TOKEN_MEMBER("cmd"),
    OBJECT_MEMBER("theme"),
    BOOL_MEMBER("persist"),
};
static const fof_json_member_spec_t CMD_PERSIST[] = {
    TOKEN_MEMBER("cmd"),
    BOOL_MEMBER("persist"),
};
static const fof_json_member_spec_t DISPLAY_NAV[] = {
    TOKEN_MEMBER("cmd"),
    TOKEN_MEMBER("action"),
};
static const fof_json_member_spec_t SCANNER_DISPLAY_BUTTON[] = {
    TOKEN_MEMBER("cmd"),
    TOKEN_MEMBER("uart"),
    BOOL_MEMBER("button_enabled"),
    TOKEN_MEMBER("view"),
    INT32_MEMBER("page"),
    BOOL_MEMBER("page_lock"),
    BOOL_MEMBER("auto_page"),
};
static const fof_json_member_spec_t SCANNER_DISPLAY_TRIGGER[] = {
    TOKEN_MEMBER("cmd"),
    TOKEN_MEMBER("uart"),
    BOOL_MEMBER("trigger_enabled"),
    TOKEN_MEMBER("view"),
    INT32_MEMBER("page"),
    BOOL_MEMBER("page_lock"),
    BOOL_MEMBER("auto_page"),
};
static const fof_json_member_spec_t SCANNER_DISPLAY_BOOT[] = {
    TOKEN_MEMBER("cmd"),
    TOKEN_MEMBER("uart"),
    BOOL_MEMBER("boot_enabled"),
    TOKEN_MEMBER("view"),
    INT32_MEMBER("page"),
    BOOL_MEMBER("page_lock"),
    BOOL_MEMBER("auto_page"),
};
static const fof_json_member_spec_t SCANNER_BOOLEAN[] = {
    TOKEN_MEMBER("cmd"),
    TOKEN_MEMBER("uart"),
    BOOL_MEMBER("enabled"),
};

#define DESCRIPTOR(schema_id, handler, schema_name, selector_value, array)   \
    [schema_id] = {                                                         \
        .id = schema_id,                                                    \
        .handler_kind = handler,                                            \
        .name = schema_name,                                                \
        .selector = selector_value,                                         \
        .members = array,                                                   \
        .member_count = ARRAY_SIZE(array),                                  \
    }

static const badge_usb_control_schema_descriptor_t
CONTROL_SCHEMAS[BADGE_USB_CONTROL_SCHEMA_COUNT] = {
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_STATUS,
        BADGE_USB_CONTROL_HANDLER_STATUS,
        "status", "status", CMD_ONLY),
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_POWER_MODE,
        BADGE_USB_CONTROL_HANDLER_POWER_MODE,
        "power_mode", "power_mode", POWER_MODE),
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_SET_MODE_PERSISTENT,
        BADGE_USB_CONTROL_HANDLER_SET_MODE,
        "set_mode_persistent", "set_mode", SET_MODE_PERSISTENT),
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_SET_MODE_SESSION,
        BADGE_USB_CONTROL_HANDLER_SET_MODE,
        "set_mode_session", "set_mode", SET_MODE_SESSION),
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_SET_BACKEND,
        BADGE_USB_CONTROL_HANDLER_SET_BACKEND,
        "set_backend", "set_backend", SET_BACKEND),
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_DISPLAY_DEBUG,
        BADGE_USB_CONTROL_HANDLER_DISPLAY_DEBUG,
        "set_display_debug", "set_display_debug", CMD_ENABLED),
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_NETWORK,
        BADGE_USB_CONTROL_HANDLER_NETWORK,
        "network", "network", NETWORK),
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_SAFE_MODE,
        BADGE_USB_CONTROL_HANDLER_SAFE_MODE,
        "safe_mode", "safe_mode", SAFE_MODE),
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_BLE_INVESTIGATE,
        BADGE_USB_CONTROL_HANDLER_BLE_INVESTIGATE,
        "ble_investigate", "ble_investigate", BLE_INVESTIGATE),
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_BLE_CHUNK,
        BADGE_USB_CONTROL_HANDLER_BLE_CHUNK,
        "ble_investigation_chunk", "ble_investigation_chunk", BLE_CHUNK),
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_DISPLAY_POLICY,
        BADGE_USB_CONTROL_HANDLER_DISPLAY_POLICY,
        "badge_display_policy", "badge_display_policy", DISPLAY_POLICY),
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_DISPLAY_POLICY_RESET,
        BADGE_USB_CONTROL_HANDLER_DISPLAY_POLICY_RESET,
        "badge_display_policy_reset", "badge_display_policy_reset",
        CMD_PERSIST),
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_THEME,
        BADGE_USB_CONTROL_HANDLER_THEME,
        "badge_theme", "badge_theme", THEME),
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_THEME_RESET,
        BADGE_USB_CONTROL_HANDLER_THEME_RESET,
        "badge_theme_reset", "badge_theme_reset", CMD_PERSIST),
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_DISPLAY_NAV,
        BADGE_USB_CONTROL_HANDLER_DISPLAY_NAV,
        "display_nav", "display_nav", DISPLAY_NAV),
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_SCANNER_DISPLAY_BUTTON,
        BADGE_USB_CONTROL_HANDLER_SCANNER_DISPLAY,
        "scanner_display_button", "scanner_display",
        SCANNER_DISPLAY_BUTTON),
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_SCANNER_DISPLAY_TRIGGER,
        BADGE_USB_CONTROL_HANDLER_SCANNER_DISPLAY,
        "scanner_display_trigger", "scanner_display",
        SCANNER_DISPLAY_TRIGGER),
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_SCANNER_DISPLAY_BOOT,
        BADGE_USB_CONTROL_HANDLER_SCANNER_DISPLAY,
        "scanner_display_boot", "scanner_display",
        SCANNER_DISPLAY_BOOT),
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_SCANNER_TRIGGER,
        BADGE_USB_CONTROL_HANDLER_SCANNER_TRIGGER,
        "scanner_trigger", "scanner_trigger", SCANNER_BOOLEAN),
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_TRIGGER,
        BADGE_USB_CONTROL_HANDLER_SCANNER_TRIGGER,
        "trigger", "trigger", SCANNER_BOOLEAN),
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_SCANNER_SAFE_MODE,
        BADGE_USB_CONTROL_HANDLER_SCANNER_SAFE_MODE,
        "scanner_safe_mode", "scanner_safe_mode", SCANNER_BOOLEAN),
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_SCANNER_RECOVERY,
        BADGE_USB_CONTROL_HANDLER_SCANNER_SAFE_MODE,
        "scanner_recovery", "scanner_recovery", SCANNER_BOOLEAN),
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_REBOOT,
        BADGE_USB_CONTROL_HANDLER_REBOOT,
        "reboot", "reboot", CMD_ONLY),
#if defined(FOF_DC34_GAME_CANARY)
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_PREPARE_UPDATE,
        BADGE_USB_CONTROL_HANDLER_UPDATE_MODE,
        "prepare_update", "prepare_update", UPDATE_SESSION),
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_FINISH_UPDATE,
        BADGE_USB_CONTROL_HANDLER_UPDATE_MODE,
        "finish_update", "finish_update", UPDATE_SESSION),
    DESCRIPTOR(
        BADGE_USB_CONTROL_SCHEMA_ABORT_UPDATE,
        BADGE_USB_CONTROL_HANDLER_UPDATE_MODE,
        "abort_update", "abort_update", UPDATE_SESSION),
#endif
};

static bool token_equals(const fof_json_value_span_t *raw,
                         const char *literal)
{
    fof_json_value_span_t token = {0};
    return fof_json_value_span_parse_ascii_token(raw, &token) &&
           token.byte_len == strlen(literal) &&
           memcmp(token.bytes, literal, token.byte_len) == 0;
}

static bool token_one_of(const fof_json_value_span_t *raw,
                         const char *const *values,
                         size_t value_count)
{
    for (size_t i = 0U; i < value_count; ++i) {
        if (token_equals(raw, values[i])) {
            return true;
        }
    }
    return false;
}

static bool request_id_valid(const fof_json_value_span_t *raw)
{
    fof_json_value_span_t token = {0};
    return fof_json_value_span_parse_ascii_token(raw, &token) &&
           token.byte_len <= 32U;
}

static bool mac_token_valid(const fof_json_value_span_t *token)
{
    if (!token || !token->bytes || token->byte_len != 17U) {
        return false;
    }
    for (size_t i = 0U; i < token->byte_len; ++i) {
        uint8_t byte = token->bytes[i];
        if ((i + 1U) % 3U == 0U) {
            if (byte != ':') {
                return false;
            }
        } else if (!((byte >= '0' && byte <= '9') ||
                     (byte >= 'a' && byte <= 'f') ||
                     (byte >= 'A' && byte <= 'F'))) {
            return false;
        }
    }
    return true;
}

static char upper_hex_byte(uint8_t byte)
{
    return byte >= 'a' && byte <= 'f'
        ? (char)(byte - 'a' + 'A') : (char)byte;
}

static bool control_semantics_valid(
    badge_usb_control_schema_id_t id,
    const fof_json_value_span_t *values)
{
    static const char *const network_modes[] = {
        "off", "usb", "usb_only", "local_ap", "ap", "backend",
    };
    static const char *const power_modes[] = {
        "active", "quiet", "on", "off",
    };
    static const char *const nav_actions[] = {
        "next", "detail", "page", "back",
    };
    static const char *const scanner_uarts[] = {
        "ble", "wifi", "all", "0", "1", "*",
    };
    static const char *const scanner_views[] = {
        "privacy", "prv", "glasses", "rf",
        "activity", "drone", "wifi",
    };

    switch (id) {
        case BADGE_USB_CONTROL_SCHEMA_POWER_MODE:
            return token_one_of(
                &values[1], power_modes, ARRAY_SIZE(power_modes));
        case BADGE_USB_CONTROL_SCHEMA_SET_MODE_PERSISTENT:
        case BADGE_USB_CONTROL_SCHEMA_SET_MODE_SESSION:
        case BADGE_USB_CONTROL_SCHEMA_NETWORK:
            return token_one_of(
                &values[1], network_modes, ARRAY_SIZE(network_modes));
        case BADGE_USB_CONTROL_SCHEMA_BLE_INVESTIGATE: {
            if (!request_id_valid(&values[1])) {
                return false;
            }
            uint32_t timeout_ms = 0U;
            if (!fof_json_value_span_parse_uint32(
                    &values[4], &timeout_ms) ||
                timeout_ms == 0U || timeout_ms > 12000U) {
                return false;
            }
            bool target_is_null = false;
            fof_json_value_span_t target = {0};
            if (!fof_json_value_span_parse_nullable_ascii_token(
                    &values[3], &target_is_null, &target)) {
                return false;
            }
            if (token_equals(&values[2], "gatt")) {
                return !target_is_null && mac_token_valid(&target);
            }
            return token_equals(&values[2], "passive_capture") &&
                   target_is_null;
        }
        case BADGE_USB_CONTROL_SCHEMA_BLE_CHUNK: {
            uint32_t seq = 0U;
            return request_id_valid(&values[1]) &&
                   fof_json_value_span_parse_uint32(&values[2], &seq) &&
                   seq <= 63U;
        }
        case BADGE_USB_CONTROL_SCHEMA_DISPLAY_POLICY: {
            badge_display_policy_t policy;
            return badge_display_policy_parse_json_span(
                values[1].bytes, values[1].byte_len,
                &policy, NULL, 0U);
        }
        case BADGE_USB_CONTROL_SCHEMA_THEME: {
            badge_theme_t theme;
            return badge_theme_parse_json_span(
                values[1].bytes, values[1].byte_len,
                &theme, NULL, 0U);
        }
        case BADGE_USB_CONTROL_SCHEMA_DISPLAY_NAV:
            return token_one_of(
                &values[1], nav_actions, ARRAY_SIZE(nav_actions));
        case BADGE_USB_CONTROL_SCHEMA_SCANNER_DISPLAY_BUTTON:
        case BADGE_USB_CONTROL_SCHEMA_SCANNER_DISPLAY_TRIGGER:
        case BADGE_USB_CONTROL_SCHEMA_SCANNER_DISPLAY_BOOT:
            return token_one_of(
                       &values[1], scanner_uarts,
                       ARRAY_SIZE(scanner_uarts)) &&
                   token_one_of(
                       &values[3], scanner_views,
                       ARRAY_SIZE(scanner_views));
        case BADGE_USB_CONTROL_SCHEMA_SCANNER_TRIGGER:
        case BADGE_USB_CONTROL_SCHEMA_TRIGGER:
        case BADGE_USB_CONTROL_SCHEMA_SCANNER_SAFE_MODE:
        case BADGE_USB_CONTROL_SCHEMA_SCANNER_RECOVERY:
            return token_one_of(
                &values[1], scanner_uarts, ARRAY_SIZE(scanner_uarts));
#if defined(FOF_DC34_GAME_CANARY)
        case BADGE_USB_CONTROL_SCHEMA_PREPARE_UPDATE:
        case BADGE_USB_CONTROL_SCHEMA_FINISH_UPDATE:
        case BADGE_USB_CONTROL_SCHEMA_ABORT_UPDATE: {
            fof_json_value_span_t session = {0};
            return fof_json_value_span_parse_ascii_token(
                       &values[1], &session) &&
                   badge_update_session_valid(
                       (const char *)session.bytes,
                       session.byte_len);
        }
#endif
        case BADGE_USB_CONTROL_SCHEMA_STATUS:
        case BADGE_USB_CONTROL_SCHEMA_SET_BACKEND:
        case BADGE_USB_CONTROL_SCHEMA_DISPLAY_DEBUG:
        case BADGE_USB_CONTROL_SCHEMA_SAFE_MODE:
        case BADGE_USB_CONTROL_SCHEMA_DISPLAY_POLICY_RESET:
        case BADGE_USB_CONTROL_SCHEMA_THEME_RESET:
        case BADGE_USB_CONTROL_SCHEMA_REBOOT:
            return true;
        case BADGE_USB_CONTROL_SCHEMA_NONE:
        case BADGE_USB_CONTROL_SCHEMA_COUNT:
        default:
            return false;
    }
}

const badge_usb_control_schema_descriptor_t *
badge_usb_control_schema_descriptor(badge_usb_control_schema_id_t id)
{
    if (id <= BADGE_USB_CONTROL_SCHEMA_NONE ||
        id >= BADGE_USB_CONTROL_SCHEMA_COUNT ||
        CONTROL_SCHEMAS[id].id != id) {
        return NULL;
    }
    return &CONTROL_SCHEMAS[id];
}

badge_usb_control_registry_result_t
badge_usb_control_select_and_validate(
    const uint8_t *bytes,
    size_t byte_len,
    badge_usb_control_schema_id_t *schema_id_out,
    badge_usb_control_handler_kind_t *handler_kind_out)
{
    if (schema_id_out) {
        *schema_id_out = BADGE_USB_CONTROL_SCHEMA_NONE;
    }
    if (handler_kind_out) {
        *handler_kind_out = BADGE_USB_CONTROL_HANDLER_NONE;
    }
    if (!bytes || byte_len == 0U ||
        !schema_id_out || !handler_kind_out) {
        return BADGE_USB_CONTROL_REGISTRY_INVALID_ARGUMENT;
    }

    char selector[64] = {0};
    size_t selector_len = 0U;
    if (fof_json_extract_unique_ascii_token_member(
            bytes, byte_len, "cmd", selector, sizeof(selector),
            &selector_len) != FOF_JSON_SCHEMA_OK) {
        return BADGE_USB_CONTROL_REGISTRY_SELECTOR_REJECTED;
    }

    size_t candidate_count = 0U;
    size_t exact_count = 0U;
    badge_usb_control_schema_id_t exact_id =
        BADGE_USB_CONTROL_SCHEMA_NONE;
    fof_json_value_span_t exact_values[BADGE_USB_CONTROL_MAX_MEMBERS] = {0};
    for (int raw_id = 1; raw_id < BADGE_USB_CONTROL_SCHEMA_COUNT; ++raw_id) {
        const badge_usb_control_schema_descriptor_t *descriptor =
            &CONTROL_SCHEMAS[raw_id];
        if (strlen(descriptor->selector) != selector_len ||
            memcmp(descriptor->selector, selector, selector_len) != 0) {
            continue;
        }
        candidate_count++;

        fof_json_value_span_t values[BADGE_USB_CONTROL_MAX_MEMBERS] = {0};
        if (descriptor->member_count > ARRAY_SIZE(values) ||
            fof_json_validate_exact_object_capture(
                bytes, byte_len, descriptor->members,
                descriptor->member_count, values,
                ARRAY_SIZE(values)) != FOF_JSON_SCHEMA_OK) {
            continue;
        }
        exact_count++;
        exact_id = descriptor->id;
        memcpy(exact_values, values, sizeof(exact_values));
    }

    if (candidate_count == 0U) {
        return BADGE_USB_CONTROL_REGISTRY_UNKNOWN_SELECTOR;
    }
    if (exact_count == 0U) {
        return BADGE_USB_CONTROL_REGISTRY_NO_EXACT_SCHEMA;
    }
    if (exact_count != 1U) {
        return BADGE_USB_CONTROL_REGISTRY_AMBIGUOUS_SCHEMA;
    }
    if (!control_semantics_valid(exact_id, exact_values)) {
        return BADGE_USB_CONTROL_REGISTRY_SEMANTIC_REJECTED;
    }

    const badge_usb_control_schema_descriptor_t *descriptor =
        badge_usb_control_schema_descriptor(exact_id);
    if (!descriptor) {
        return BADGE_USB_CONTROL_REGISTRY_INVALID_ARGUMENT;
    }
    *schema_id_out = exact_id;
    *handler_kind_out = descriptor->handler_kind;
    return BADGE_USB_CONTROL_REGISTRY_OK;
}

bool badge_usb_control_decode_ble_investigate(
    const uint8_t *bytes,
    size_t byte_len,
    ble_investigation_request_t *request_out)
{
    if (request_out) {
        memset(request_out, 0, sizeof(*request_out));
    }
    if (!request_out) {
        return false;
    }

    badge_usb_control_schema_id_t schema_id =
        BADGE_USB_CONTROL_SCHEMA_NONE;
    badge_usb_control_handler_kind_t handler_kind =
        BADGE_USB_CONTROL_HANDLER_NONE;
    if (badge_usb_control_select_and_validate(
            bytes, byte_len, &schema_id, &handler_kind) !=
            BADGE_USB_CONTROL_REGISTRY_OK ||
        schema_id != BADGE_USB_CONTROL_SCHEMA_BLE_INVESTIGATE ||
        handler_kind != BADGE_USB_CONTROL_HANDLER_BLE_INVESTIGATE) {
        return false;
    }

    fof_json_value_span_t values[ARRAY_SIZE(BLE_INVESTIGATE)] = {0};
    if (fof_json_validate_exact_object_capture(
            bytes, byte_len, BLE_INVESTIGATE,
            ARRAY_SIZE(BLE_INVESTIGATE), values,
            ARRAY_SIZE(values)) != FOF_JSON_SCHEMA_OK) {
        return false;
    }

    fof_json_value_span_t request_id = {0};
    fof_json_value_span_t mode = {0};
    fof_json_value_span_t target = {0};
    bool target_is_null = false;
    uint32_t timeout_ms = 0U;
    if (!fof_json_value_span_parse_ascii_token(
            &values[1], &request_id) ||
        request_id.byte_len >= BLE_INV_REQUEST_ID_LEN ||
        !fof_json_value_span_parse_ascii_token(&values[2], &mode) ||
        !fof_json_value_span_parse_nullable_ascii_token(
            &values[3], &target_is_null, &target) ||
        !fof_json_value_span_parse_uint32(
            &values[4], &timeout_ms)) {
        return false;
    }

    ble_investigation_request_t decoded = {0};
    memcpy(decoded.request_id, request_id.bytes, request_id.byte_len);
    decoded.timeout_ms = timeout_ms;
    if (mode.byte_len == sizeof("gatt") - 1U &&
        memcmp(mode.bytes, "gatt", sizeof("gatt") - 1U) == 0) {
        if (target_is_null ||
            target.byte_len >= sizeof(decoded.target_mac)) {
            return false;
        }
        decoded.mode = BLE_INV_MODE_GATT;
        for (size_t i = 0U; i < target.byte_len; ++i) {
            decoded.target_mac[i] = upper_hex_byte(target.bytes[i]);
        }
    } else if (
        mode.byte_len == sizeof("passive_capture") - 1U &&
        memcmp(
            mode.bytes, "passive_capture",
            sizeof("passive_capture") - 1U) == 0) {
        if (!target_is_null) {
            return false;
        }
        decoded.mode = BLE_INV_MODE_PASSIVE_CAPTURE;
    } else {
        return false;
    }

    *request_out = decoded;
    return true;
}
