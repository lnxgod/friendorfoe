#include "unity.h"

#include "badge_usb_control_schema.h"
#include "badge_ble_investigation_state.h"
#include "ble_investigation_protocol.h"
#include "serial_config.h"
#include "serial_config_ingress.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))

#define VALID_POLICY_JSON                                                \
    "{\"version\":1,\"classes\":{"                                    \
    "\"drone\":{\"enabled\":true,\"lane\":\"both\","                 \
    "\"min_proximity\":\"present\",\"priority\":100},"                \
    "\"meta\":{\"enabled\":true,\"lane\":\"top\","                    \
    "\"min_proximity\":\"near\",\"priority\":95},"                    \
    "\"tracker\":{\"enabled\":true,\"lane\":\"lower\","               \
    "\"min_proximity\":\"close\",\"priority\":90},"                   \
    "\"wifi_attack\":{\"enabled\":false,\"lane\":\"off\","            \
    "\"min_proximity\":\"present\",\"priority\":85},"                 \
    "\"skimmer\":{\"enabled\":false,\"lane\":\"off\","                \
    "\"min_proximity\":\"close\",\"priority\":0},"                    \
    "\"camera\":{\"enabled\":true,\"lane\":\"lower\","                \
    "\"min_proximity\":\"near\",\"priority\":70},"                    \
    "\"flock\":{\"enabled\":true,\"lane\":\"top\","                   \
    "\"min_proximity\":\"present\",\"priority\":80},"                 \
    "\"lock\":{\"enabled\":true,\"lane\":\"lower\","                  \
    "\"min_proximity\":\"near\",\"priority\":60},"                    \
    "\"hid\":{\"enabled\":true,\"lane\":\"lower\","                   \
    "\"min_proximity\":\"close\",\"priority\":55},"                   \
    "\"beacon\":{\"enabled\":false,\"lane\":\"off\","                 \
    "\"min_proximity\":\"present\",\"priority\":40},"                 \
    "\"event_badge\":{\"enabled\":true,\"lane\":\"lower\","           \
    "\"min_proximity\":\"present\",\"priority\":35},"                 \
    "\"auracast\":{\"enabled\":true,\"lane\":\"lower\","              \
    "\"min_proximity\":\"near\",\"priority\":30},"                    \
    "\"scanner_status\":{\"enabled\":true,\"lane\":\"lower\","        \
    "\"min_proximity\":\"present\",\"priority\":20},"                 \
    "\"ble_attack\":{\"enabled\":true,\"lane\":\"both\","             \
    "\"min_proximity\":\"present\",\"priority\":92}}}"

#define VALID_THEME_JSON                                                 \
    "{\"version\":1,\"palette\":\"neon\",\"background\":\"scanline\"," \
    "\"brightness\":75,\"accents\":{\"drone\":65184,\"meta\":63539,"   \
    "\"tracker\":63519,\"flock\":43039,\"wifi_attack\":2047,"          \
    "\"clear\":12133}}"

typedef struct {
    const char *json;
    badge_usb_control_schema_id_t schema_id;
    badge_usb_control_handler_kind_t handler_kind;
} control_case_t;

typedef struct {
    const char *name;
    badge_usb_control_schema_id_t schema_id;
} android_fixture_case_t;

static const android_fixture_case_t ANDROID_FIXTURE_CASES[] = {
    {"set_mode_local_ap", BADGE_USB_CONTROL_SCHEMA_SET_MODE_PERSISTENT},
    {"set_mode_backend", BADGE_USB_CONTROL_SCHEMA_SET_MODE_PERSISTENT},
    {"set_mode_usb_only", BADGE_USB_CONTROL_SCHEMA_SET_MODE_PERSISTENT},
    {"reboot", BADGE_USB_CONTROL_SCHEMA_REBOOT},
    {"display_policy", BADGE_USB_CONTROL_SCHEMA_DISPLAY_POLICY},
    {"display_policy_reset",
     BADGE_USB_CONTROL_SCHEMA_DISPLAY_POLICY_RESET},
    {"theme", BADGE_USB_CONTROL_SCHEMA_THEME},
    {"theme_reset", BADGE_USB_CONTROL_SCHEMA_THEME_RESET},
    {"display_nav_next", BADGE_USB_CONTROL_SCHEMA_DISPLAY_NAV},
    {"display_nav_detail", BADGE_USB_CONTROL_SCHEMA_DISPLAY_NAV},
    {"display_nav_page", BADGE_USB_CONTROL_SCHEMA_DISPLAY_NAV},
    {"display_nav_back", BADGE_USB_CONTROL_SCHEMA_DISPLAY_NAV},
    {"ble_investigate_gatt",
     BADGE_USB_CONTROL_SCHEMA_BLE_INVESTIGATE},
    {"ble_investigate_passive",
     BADGE_USB_CONTROL_SCHEMA_BLE_INVESTIGATE},
    {"ble_chunk_first", BADGE_USB_CONTROL_SCHEMA_BLE_CHUNK},
    {"ble_chunk_last", BADGE_USB_CONTROL_SCHEMA_BLE_CHUNK},
};

static void assert_control_ok(const control_case_t *test_case)
{
    badge_usb_control_schema_id_t schema_id =
        BADGE_USB_CONTROL_SCHEMA_NONE;
    badge_usb_control_handler_kind_t handler_kind =
        BADGE_USB_CONTROL_HANDLER_NONE;
    TEST_ASSERT_EQUAL(
        BADGE_USB_CONTROL_REGISTRY_OK,
        badge_usb_control_select_and_validate(
            (const uint8_t *)test_case->json,
            strlen(test_case->json),
            &schema_id,
            &handler_kind));
    TEST_ASSERT_EQUAL(test_case->schema_id, schema_id);
    TEST_ASSERT_EQUAL(test_case->handler_kind, handler_kind);
}

static void assert_control_rejected_span(
    const uint8_t *bytes,
    size_t byte_len)
{
    badge_usb_control_schema_id_t schema_id =
        BADGE_USB_CONTROL_SCHEMA_REBOOT;
    badge_usb_control_handler_kind_t handler_kind =
        BADGE_USB_CONTROL_HANDLER_REBOOT;
    TEST_ASSERT_NOT_EQUAL(
        BADGE_USB_CONTROL_REGISTRY_OK,
        badge_usb_control_select_and_validate(
            bytes, byte_len,
            &schema_id, &handler_kind));
    TEST_ASSERT_EQUAL(BADGE_USB_CONTROL_SCHEMA_NONE, schema_id);
    TEST_ASSERT_EQUAL(BADGE_USB_CONTROL_HANDLER_NONE, handler_kind);
}

static void assert_control_rejected(const char *json)
{
    assert_control_rejected_span(
        (const uint8_t *)json, strlen(json));
}

typedef struct {
    size_t member_start;
    size_t value_start;
    size_t value_end;
    size_t member_end;
} json_member_range_t;

static bool test_skip_json_string(
    const char *json,
    size_t json_len,
    size_t *position)
{
    if (!json || !position || *position >= json_len ||
        json[*position] != '"') {
        return false;
    }
    (*position)++;
    while (*position < json_len) {
        char byte = json[*position];
        (*position)++;
        if (byte == '"') {
            return true;
        }
        if (byte == '\\') {
            if (*position >= json_len) {
                return false;
            }
            (*position)++;
        }
    }
    return false;
}

static bool test_skip_json_value(
    const char *json,
    size_t json_len,
    size_t *position)
{
    if (!json || !position || *position >= json_len) {
        return false;
    }
    if (json[*position] == '"') {
        return test_skip_json_string(json, json_len, position);
    }
    if (json[*position] == '{' || json[*position] == '[') {
        char open = json[*position];
        char close = open == '{' ? '}' : ']';
        size_t depth = 0U;
        while (*position < json_len) {
            if (json[*position] == '"') {
                if (!test_skip_json_string(json, json_len, position)) {
                    return false;
                }
                continue;
            }
            if (json[*position] == open) {
                depth++;
            } else if (json[*position] == close) {
                depth--;
                (*position)++;
                if (depth == 0U) {
                    return true;
                }
                continue;
            }
            (*position)++;
        }
        return false;
    }
    while (*position < json_len &&
           json[*position] != ',' && json[*position] != '}') {
        (*position)++;
    }
    return true;
}

static bool test_find_top_member(
    const char *json,
    const char *member_name,
    json_member_range_t *range)
{
    if (!json || !member_name || !range) {
        return false;
    }
    size_t json_len = strlen(json);
    size_t position = 0U;
    if (json_len < 2U || json[position++] != '{') {
        return false;
    }
    while (position < json_len && json[position] != '}') {
        size_t member_start = position;
        if (json[position] != '"') {
            return false;
        }
        size_t key_start = ++position;
        while (position < json_len && json[position] != '"') {
            if (json[position] == '\\') {
                return false;
            }
            position++;
        }
        if (position >= json_len) {
            return false;
        }
        size_t key_len = position - key_start;
        position++;
        if (position >= json_len || json[position++] != ':') {
            return false;
        }
        size_t value_start = position;
        if (!test_skip_json_value(json, json_len, &position)) {
            return false;
        }
        size_t value_end = position;
        size_t member_end = position;
        if (position < json_len && json[position] == ',') {
            member_end = ++position;
        }
        if (strlen(member_name) == key_len &&
            memcmp(json + key_start, member_name, key_len) == 0) {
            *range = (json_member_range_t) {
                .member_start = member_start,
                .value_start = value_start,
                .value_end = value_end,
                .member_end = member_end,
            };
            return true;
        }
    }
    return false;
}

static bool test_remove_member(
    const char *json,
    const char *member_name,
    char *out,
    size_t out_len)
{
    json_member_range_t range;
    size_t json_len = strlen(json);
    if (!test_find_top_member(json, member_name, &range) ||
        !out || out_len <= json_len) {
        return false;
    }
    size_t remove_start = range.member_start;
    size_t remove_end = range.member_end;
    if (remove_end == range.value_end && remove_start > 1U) {
        remove_start--;
    }
    memcpy(out, json, remove_start);
    memcpy(out + remove_start, json + remove_end, json_len - remove_end + 1U);
    return true;
}

static bool test_replace_member_value(
    const char *json,
    const char *member_name,
    const char *replacement,
    char *out,
    size_t out_len)
{
    json_member_range_t range;
    size_t json_len = strlen(json);
    size_t replacement_len = strlen(replacement);
    if (!test_find_top_member(json, member_name, &range) || !out ||
        range.value_start + replacement_len +
            (json_len - range.value_end) + 1U > out_len) {
        return false;
    }
    memcpy(out, json, range.value_start);
    memcpy(out + range.value_start, replacement, replacement_len);
    memcpy(
        out + range.value_start + replacement_len,
        json + range.value_end,
        json_len - range.value_end + 1U);
    return true;
}

static const control_case_t CONTROL_CASES[] = {
        {"{\"cmd\":\"status\"}",
         BADGE_USB_CONTROL_SCHEMA_STATUS,
         BADGE_USB_CONTROL_HANDLER_STATUS},
        {"{\"cmd\":\"power_mode\",\"mode\":\"quiet\"}",
         BADGE_USB_CONTROL_SCHEMA_POWER_MODE,
         BADGE_USB_CONTROL_HANDLER_POWER_MODE},
        {"{\"cmd\":\"set_mode\",\"mode\":\"usb\",\"persist\":true}",
         BADGE_USB_CONTROL_SCHEMA_SET_MODE_PERSISTENT,
         BADGE_USB_CONTROL_HANDLER_SET_MODE},
        {"{\"cmd\":\"set_mode\",\"mode\":\"backend\",\"ttl_s\":7500}",
         BADGE_USB_CONTROL_SCHEMA_SET_MODE_SESSION,
         BADGE_USB_CONTROL_HANDLER_SET_MODE},
        {"{\"cmd\":\"set_backend\",\"url\":\"https://example.test\","
         "\"wifi_ssid\":\"FoF\",\"wifi_pass\":\"secret\","
         "\"enable\":true,\"ttl_s\":7500}",
         BADGE_USB_CONTROL_SCHEMA_SET_BACKEND,
         BADGE_USB_CONTROL_HANDLER_SET_BACKEND},
        {"{\"cmd\":\"set_display_debug\",\"enabled\":false}",
         BADGE_USB_CONTROL_SCHEMA_DISPLAY_DEBUG,
         BADGE_USB_CONTROL_HANDLER_DISPLAY_DEBUG},
        {"{\"cmd\":\"network\",\"mode\":\"local_ap\",\"ttl_s\":7500}",
         BADGE_USB_CONTROL_SCHEMA_NETWORK,
         BADGE_USB_CONTROL_HANDLER_NETWORK},
        {"{\"cmd\":\"safe_mode\",\"enabled\":true,"
         "\"reason\":\"USB recovery\"}",
         BADGE_USB_CONTROL_SCHEMA_SAFE_MODE,
         BADGE_USB_CONTROL_HANDLER_SAFE_MODE},
        {"{\"cmd\":\"ble_investigate\",\"request_id\":\"req-1\","
         "\"mode\":\"gatt\",\"target\":\"AA:BB:CC:DD:EE:FF\","
         "\"timeout_ms\":7500}",
         BADGE_USB_CONTROL_SCHEMA_BLE_INVESTIGATE,
         BADGE_USB_CONTROL_HANDLER_BLE_INVESTIGATE},
        {"{\"cmd\":\"ble_investigation_chunk\","
         "\"request_id\":\"req-1\",\"seq\":0}",
         BADGE_USB_CONTROL_SCHEMA_BLE_CHUNK,
         BADGE_USB_CONTROL_HANDLER_BLE_CHUNK},
        {"{\"cmd\":\"badge_display_policy\",\"policy\":"
         VALID_POLICY_JSON ",\"persist\":false}",
         BADGE_USB_CONTROL_SCHEMA_DISPLAY_POLICY,
         BADGE_USB_CONTROL_HANDLER_DISPLAY_POLICY},
        {"{\"cmd\":\"badge_display_policy_reset\",\"persist\":false}",
         BADGE_USB_CONTROL_SCHEMA_DISPLAY_POLICY_RESET,
         BADGE_USB_CONTROL_HANDLER_DISPLAY_POLICY_RESET},
        {"{\"cmd\":\"badge_theme\",\"theme\":"
         VALID_THEME_JSON ",\"persist\":true}",
         BADGE_USB_CONTROL_SCHEMA_THEME,
         BADGE_USB_CONTROL_HANDLER_THEME},
        {"{\"cmd\":\"badge_theme_reset\",\"persist\":false}",
         BADGE_USB_CONTROL_SCHEMA_THEME_RESET,
         BADGE_USB_CONTROL_HANDLER_THEME_RESET},
        {"{\"cmd\":\"display_nav\",\"action\":\"next\"}",
         BADGE_USB_CONTROL_SCHEMA_DISPLAY_NAV,
         BADGE_USB_CONTROL_HANDLER_DISPLAY_NAV},
        {"{\"cmd\":\"scanner_display\",\"uart\":\"all\","
         "\"button_enabled\":true,\"view\":\"privacy\",\"page\":-1,"
         "\"page_lock\":false,\"auto_page\":true}",
         BADGE_USB_CONTROL_SCHEMA_SCANNER_DISPLAY_BUTTON,
         BADGE_USB_CONTROL_HANDLER_SCANNER_DISPLAY},
        {"{\"cmd\":\"scanner_display\",\"uart\":\"0\","
         "\"trigger_enabled\":true,\"view\":\"prv\",\"page\":0,"
         "\"page_lock\":true,\"auto_page\":false}",
         BADGE_USB_CONTROL_SCHEMA_SCANNER_DISPLAY_TRIGGER,
         BADGE_USB_CONTROL_HANDLER_SCANNER_DISPLAY},
        {"{\"cmd\":\"scanner_display\",\"uart\":\"1\","
         "\"boot_enabled\":false,\"view\":\"drone\",\"page\":1,"
         "\"page_lock\":false,\"auto_page\":false}",
         BADGE_USB_CONTROL_SCHEMA_SCANNER_DISPLAY_BOOT,
         BADGE_USB_CONTROL_HANDLER_SCANNER_DISPLAY},
        {"{\"cmd\":\"scanner_trigger\",\"uart\":\"ble\","
         "\"enabled\":true}",
         BADGE_USB_CONTROL_SCHEMA_SCANNER_TRIGGER,
         BADGE_USB_CONTROL_HANDLER_SCANNER_TRIGGER},
        {"{\"cmd\":\"trigger\",\"uart\":\"wifi\",\"enabled\":false}",
         BADGE_USB_CONTROL_SCHEMA_TRIGGER,
         BADGE_USB_CONTROL_HANDLER_SCANNER_TRIGGER},
        {"{\"cmd\":\"scanner_safe_mode\",\"uart\":\"*\","
         "\"enabled\":true}",
         BADGE_USB_CONTROL_SCHEMA_SCANNER_SAFE_MODE,
         BADGE_USB_CONTROL_HANDLER_SCANNER_SAFE_MODE},
        {"{\"cmd\":\"scanner_recovery\",\"uart\":\"all\","
         "\"enabled\":false}",
         BADGE_USB_CONTROL_SCHEMA_SCANNER_RECOVERY,
         BADGE_USB_CONTROL_HANDLER_SCANNER_SAFE_MODE},
        {"{\"cmd\":\"reboot\"}",
         BADGE_USB_CONTROL_SCHEMA_REBOOT,
         BADGE_USB_CONTROL_HANDLER_REBOOT},
        {"{\"cmd\":\"prepare_update\","
         "\"session\":\"0123456789ABCDEF\"}",
         BADGE_USB_CONTROL_SCHEMA_PREPARE_UPDATE,
         BADGE_USB_CONTROL_HANDLER_UPDATE_MODE},
        {"{\"cmd\":\"finish_update\","
         "\"session\":\"0123456789ABCDEF\"}",
         BADGE_USB_CONTROL_SCHEMA_FINISH_UPDATE,
         BADGE_USB_CONTROL_HANDLER_UPDATE_MODE},
        {"{\"cmd\":\"abort_update\","
         "\"session\":\"0123456789ABCDEF\"}",
         BADGE_USB_CONTROL_SCHEMA_ABORT_UPDATE,
         BADGE_USB_CONTROL_HANDLER_UPDATE_MODE},
};

static const control_case_t *test_control_case_for_id(
    badge_usb_control_schema_id_t id)
{
    for (size_t i = 0U; i < ARRAY_SIZE(CONTROL_CASES); ++i) {
        if (CONTROL_CASES[i].schema_id == id) {
            return &CONTROL_CASES[i];
        }
    }
    return NULL;
}

static const char *test_wrong_wire_type(fof_json_wire_type_t type)
{
    switch (type) {
        case FOF_JSON_STRING:
        case FOF_JSON_NULLABLE_STRING:
            return "true";
        case FOF_JSON_BOOL:
        case FOF_JSON_INT32:
        case FOF_JSON_INT64:
        case FOF_JSON_UINT32:
            return "\"wrong\"";
        case FOF_JSON_OBJECT:
            return "[]";
        default:
            return "null";
    }
}

static bool test_duplicate_member(
    const char *json,
    const char *member_name,
    char *out,
    size_t out_len)
{
    json_member_range_t range;
    size_t json_len = strlen(json);
    if (!test_find_top_member(json, member_name, &range) ||
        json_len == 0U || json[json_len - 1U] != '}') {
        return false;
    }
    size_t member_len = range.value_end - range.member_start;
    if (json_len + 1U + member_len + 1U > out_len) {
        return false;
    }
    memcpy(out, json, json_len - 1U);
    out[json_len - 1U] = ',';
    memcpy(out + json_len, json + range.member_start, member_len);
    out[json_len + member_len] = '}';
    out[json_len + member_len + 1U] = '\0';
    return true;
}

static bool test_add_unknown_member(
    const char *json,
    char *out,
    size_t out_len)
{
    static const char suffix[] = ",\"unknown\":true}";
    size_t json_len = strlen(json);
    if (json_len == 0U || json[json_len - 1U] != '}' ||
        json_len - 1U + sizeof(suffix) > out_len) {
        return false;
    }
    memcpy(out, json, json_len - 1U);
    memcpy(out + json_len - 1U, suffix, sizeof(suffix));
    return true;
}

static bool test_escape_cmd_key(
    const char *json,
    char *out,
    size_t out_len)
{
    static const char escaped_key[] = "\"c\\u006dd\":";
    json_member_range_t range;
    size_t json_len = strlen(json);
    if (!test_find_top_member(json, "cmd", &range) ||
        range.member_start + sizeof(escaped_key) - 1U +
            (json_len - range.value_start) + 1U > out_len) {
        return false;
    }
    memcpy(out, json, range.member_start);
    memcpy(
        out + range.member_start,
        escaped_key, sizeof(escaped_key) - 1U);
    memcpy(
        out + range.member_start + sizeof(escaped_key) - 1U,
        json + range.value_start,
        json_len - range.value_start + 1U);
    return true;
}

static bool test_escape_selector_value(
    const char *json,
    const char *selector,
    char *out,
    size_t out_len)
{
    if (!selector || selector[0] == '\0') {
        return false;
    }
    char escaped[96];
    int escaped_len = snprintf(
        escaped, sizeof(escaped), "\"\\u%04x%s\"",
        (unsigned)(uint8_t)selector[0], selector + 1);
    return escaped_len > 0 && (size_t)escaped_len < sizeof(escaped) &&
           test_replace_member_value(
               json, "cmd", escaped, out, out_len);
}

void test_badge_usb_control_registry_accepts_all_26_exact_schemas(void)
{
    TEST_ASSERT_EQUAL_UINT(26U, ARRAY_SIZE(CONTROL_CASES));
    for (size_t i = 0U; i < ARRAY_SIZE(CONTROL_CASES); ++i) {
        assert_control_ok(&CONTROL_CASES[i]);
    }
}

void test_badge_usb_control_registry_descriptors_are_closed_and_unique(void)
{
    uint32_t handler_mask = 0U;
    for (int raw_id = 1; raw_id < BADGE_USB_CONTROL_SCHEMA_COUNT; ++raw_id) {
        const badge_usb_control_schema_descriptor_t *descriptor =
            badge_usb_control_schema_descriptor(
                (badge_usb_control_schema_id_t)raw_id);
        TEST_ASSERT_NOT_NULL(descriptor);
        TEST_ASSERT_EQUAL(raw_id, descriptor->id);
        TEST_ASSERT_NOT_NULL(descriptor->name);
        TEST_ASSERT_NOT_NULL(descriptor->selector);
        TEST_ASSERT_NOT_NULL(descriptor->members);
        TEST_ASSERT_GREATER_THAN_UINT(0U, descriptor->member_count);
        TEST_ASSERT_NOT_EQUAL(
            BADGE_USB_CONTROL_HANDLER_NONE, descriptor->handler_kind);
        handler_mask |= UINT32_C(1) << descriptor->handler_kind;

        for (int prior = 1; prior < raw_id; ++prior) {
            const badge_usb_control_schema_descriptor_t *other =
                badge_usb_control_schema_descriptor(
                    (badge_usb_control_schema_id_t)prior);
            TEST_ASSERT_NOT_EQUAL(0, strcmp(descriptor->name, other->name));
        }
    }
    TEST_ASSERT_EQUAL_UINT(26U, BADGE_USB_CONTROL_SCHEMA_COUNT - 1U);
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, handler_mask);
    TEST_ASSERT_NULL(badge_usb_control_schema_descriptor(
        BADGE_USB_CONTROL_SCHEMA_NONE));
    TEST_ASSERT_NULL(badge_usb_control_schema_descriptor(
        BADGE_USB_CONTROL_SCHEMA_COUNT));
}

void test_badge_usb_control_registry_rejects_per_descriptor_shape_mutations(
    void)
{
    char mutated[4096];
    for (int raw_id = 1;
         raw_id < BADGE_USB_CONTROL_SCHEMA_COUNT;
         ++raw_id) {
        badge_usb_control_schema_id_t id =
            (badge_usb_control_schema_id_t)raw_id;
        const badge_usb_control_schema_descriptor_t *descriptor =
            badge_usb_control_schema_descriptor(id);
        const control_case_t *test_case = test_control_case_for_id(id);
        TEST_ASSERT_NOT_NULL(descriptor);
        TEST_ASSERT_NOT_NULL(test_case);

        for (size_t member = 0U;
             member < descriptor->member_count;
             ++member) {
            TEST_ASSERT_TRUE_MESSAGE(
                test_remove_member(
                    test_case->json,
                    descriptor->members[member].name,
                    mutated, sizeof(mutated)),
                descriptor->name);
            assert_control_rejected(mutated);

            TEST_ASSERT_TRUE_MESSAGE(
                test_replace_member_value(
                    test_case->json,
                    descriptor->members[member].name,
                    test_wrong_wire_type(
                        descriptor->members[member].type),
                    mutated, sizeof(mutated)),
                descriptor->name);
            assert_control_rejected(mutated);
        }

        const char *duplicate_name =
            descriptor->members[
                descriptor->member_count > 1U ? 1U : 0U].name;
        TEST_ASSERT_TRUE_MESSAGE(
            test_duplicate_member(
                test_case->json, duplicate_name,
                mutated, sizeof(mutated)),
            descriptor->name);
        assert_control_rejected(mutated);

        TEST_ASSERT_TRUE_MESSAGE(
            test_add_unknown_member(
                test_case->json, mutated, sizeof(mutated)),
            descriptor->name);
        assert_control_rejected(mutated);

        TEST_ASSERT_TRUE_MESSAGE(
            test_escape_cmd_key(
                test_case->json, mutated, sizeof(mutated)),
            descriptor->name);
        assert_control_rejected(mutated);

        TEST_ASSERT_TRUE_MESSAGE(
            test_escape_selector_value(
                test_case->json, descriptor->selector,
                mutated, sizeof(mutated)),
            descriptor->name);
        assert_control_rejected(mutated);

        int trailing_len = snprintf(
            mutated, sizeof(mutated), "%s{}",
            test_case->json);
        TEST_ASSERT_GREATER_THAN_INT(0, trailing_len);
        TEST_ASSERT_LESS_THAN_INT((int)sizeof(mutated), trailing_len);
        assert_control_rejected(mutated);
    }
}

void test_badge_usb_control_registry_rejects_shape_and_selector_confusion(void)
{
    static const char *const rejected[] = {
        "{\"cmd\":\"power_mode\"}",
        "{\"cmd\":\"power_mode\",\"mode\":\"quiet\",\"extra\":true}",
        "{\"cmd\":\"power_mode\",\"cmd\":\"power_mode\","
        "\"mode\":\"quiet\"}",
        "{\"mode\":\"quiet\",\"cmd\":\"power_mode\",\"mode\":\"active\"}",
        "{\"c\\u006dd\":\"power_mode\",\"mode\":\"quiet\"}",
        "{\"cmd\":\"power\\u005fmode\",\"mode\":\"quiet\"}",
        "{\"cmd\":\"set_mode\",\"mode\":\"usb\","
        "\"persist\":true,\"ttl_s\":7500}",
        "{\"cmd\":\"scanner_display\",\"uart\":\"all\","
        "\"button_enabled\":true,\"trigger_enabled\":true,"
        "\"view\":\"privacy\",\"page\":0,\"page_lock\":false,"
        "\"auto_page\":true}",
        "{\"cmd\":\"not_registered\"}",
        "{\"cmd\":\"status\"}{}",
    };
    for (size_t i = 0U; i < ARRAY_SIZE(rejected); ++i) {
        assert_control_rejected(rejected[i]);
    }
}

void test_badge_usb_control_registry_enforces_scalar_semantics_and_boundaries(
    void)
{
    static const control_case_t accepted[] = {
        {"{\"cmd\":\"ble_investigate\",\"request_id\":\"req-12000\","
         "\"mode\":\"passive_capture\",\"target\":null,"
         "\"timeout_ms\":12000}",
         BADGE_USB_CONTROL_SCHEMA_BLE_INVESTIGATE,
         BADGE_USB_CONTROL_HANDLER_BLE_INVESTIGATE},
        {"{\"cmd\":\"network\",\"mode\":\"off\","
         "\"ttl_s\":-2147483648}",
         BADGE_USB_CONTROL_SCHEMA_NETWORK,
         BADGE_USB_CONTROL_HANDLER_NETWORK},
        {"{\"cmd\":\"scanner_display\",\"uart\":\"*\","
         "\"button_enabled\":true,\"view\":\"wifi\","
         "\"page\":-2147483648,\"page_lock\":true,"
         "\"auto_page\":false}",
         BADGE_USB_CONTROL_SCHEMA_SCANNER_DISPLAY_BUTTON,
         BADGE_USB_CONTROL_HANDLER_SCANNER_DISPLAY},
    };
    for (size_t i = 0U; i < ARRAY_SIZE(accepted); ++i) {
        assert_control_ok(&accepted[i]);
    }

    static const char *const rejected[] = {
        "{\"cmd\":\"power_mode\",\"mode\":\"LOUD\"}",
        "{\"cmd\":\"set_mode\",\"mode\":\"wireless\",\"ttl_s\":1}",
        "{\"cmd\":\"network\",\"mode\":\"off\",\"ttl_s\":1.0}",
        "{\"cmd\":\"ble_investigate\",\"request_id\":\"\","
        "\"mode\":\"gatt\",\"target\":\"AA:BB:CC:DD:EE:FF\","
        "\"timeout_ms\":7500}",
        "{\"cmd\":\"ble_investigate\",\"request_id\":\"req\","
        "\"mode\":\"gatt\",\"target\":null,\"timeout_ms\":7500}",
        "{\"cmd\":\"ble_investigate\",\"request_id\":\"req\","
        "\"mode\":\"passive_capture\","
        "\"target\":\"AA:BB:CC:DD:EE:FF\",\"timeout_ms\":7500}",
        "{\"cmd\":\"ble_investigate\",\"request_id\":\"req\","
        "\"mode\":\"gatt\",\"target\":\"AA-BB-CC-DD-EE-FF\","
        "\"timeout_ms\":7500}",
        "{\"cmd\":\"ble_investigate\",\"request_id\":\"req\","
        "\"mode\":\"gatt\",\"target\":\"AA:BB:CC:DD:EE:FF\","
        "\"timeout_ms\":0}",
        "{\"cmd\":\"ble_investigate\",\"request_id\":\"req\","
        "\"mode\":\"gatt\",\"target\":\"AA:BB:CC:DD:EE:FF\","
        "\"timeout_ms\":12001}",
        "{\"cmd\":\"ble_investigation_chunk\","
        "\"request_id\":\"req\",\"seq\":64}",
        "{\"cmd\":\"display_nav\",\"action\":\"forward\"}",
        "{\"cmd\":\"scanner_trigger\",\"uart\":\"both\","
        "\"enabled\":true}",
        "{\"cmd\":\"scanner_display\",\"uart\":\"all\","
        "\"button_enabled\":true,\"view\":\"unknown\",\"page\":0,"
        "\"page_lock\":false,\"auto_page\":true}",
        "{\"cmd\":\"prepare_update\","
        "\"session\":\"0123456789abcdef\"}",
        "{\"cmd\":\"prepare_update\","
        "\"session\":\"0000000000000000\"}",
        "{\"cmd\":\"finish_update\","
        "\"session\":\"0123456789ABCDE\"}",
        "{\"cmd\":\"abort_update\","
        "\"session\":\"0123456789ABCDEFG\"}",
    };
    for (size_t i = 0U; i < ARRAY_SIZE(rejected); ++i) {
        assert_control_rejected(rejected[i]);
    }
}

void test_badge_usb_control_registry_rejects_invalid_nested_policy_and_theme(
    void)
{
    assert_control_rejected(
        "{\"cmd\":\"badge_display_policy\",\"policy\":{"
        "\"version\":1,\"classes\":{}},\"persist\":false}");
    assert_control_rejected(
        "{\"cmd\":\"badge_theme\",\"theme\":{"
        "\"version\":1,\"palette\":\"NEON\",\"background\":\"dark\","
        "\"brightness\":75,\"accents\":{\"drone\":65184,\"meta\":63539,"
        "\"tracker\":63519,\"flock\":43039,\"wifi_attack\":2047,"
        "\"clear\":12133}},\"persist\":false}");
    assert_control_rejected(
        "{\"cmd\":\"badge_theme\",\"theme\":{"
        "\"version\":1,\"palette\":\"neon\",\"background\":\"dark\","
        "\"brightness\":75,\"accents\":{\"drone\":65184,\"meta\":63539,"
        "\"tracker\":63519,\"flock\":43039,\"wifi_attack\":2047}},"
        "\"persist\":false}");
}

void test_badge_usb_control_registry_uses_explicit_length_for_nul_and_controls(
    void)
{
    static const uint8_t embedded_nul[] =
        "{\"cmd\":\"status\"}\0{\"cmd\":\"reboot\"}";
    static const uint8_t raw_c0[] =
        "{\"cmd\":\"sta\x01tus\"}";
    static const uint8_t raw_del[] =
        "{\"cmd\":\"sta\x7ftus\"}";

    assert_control_rejected_span(
        embedded_nul, sizeof(embedded_nul) - 1U);
    assert_control_rejected_span(raw_c0, sizeof(raw_c0) - 1U);
    assert_control_rejected_span(raw_del, sizeof(raw_del) - 1U);
}

void test_badge_usb_control_registry_accepts_semantic_min_max_boundaries(void)
{
    static const control_case_t accepted[] = {
        {"{\"cmd\":\"ble_investigate\",\"request_id\":\"x\","
         "\"mode\":\"gatt\",\"target\":\"aa:bb:cc:dd:ee:ff\","
         "\"timeout_ms\":1}",
         BADGE_USB_CONTROL_SCHEMA_BLE_INVESTIGATE,
         BADGE_USB_CONTROL_HANDLER_BLE_INVESTIGATE},
        {"{\"cmd\":\"ble_investigate\","
         "\"request_id\":\"12345678901234567890123456789012\","
         "\"mode\":\"passive_capture\",\"target\":null,"
         "\"timeout_ms\":12000}",
         BADGE_USB_CONTROL_SCHEMA_BLE_INVESTIGATE,
         BADGE_USB_CONTROL_HANDLER_BLE_INVESTIGATE},
        {"{\"cmd\":\"ble_investigation_chunk\","
         "\"request_id\":\"x\",\"seq\":63}",
         BADGE_USB_CONTROL_SCHEMA_BLE_CHUNK,
         BADGE_USB_CONTROL_HANDLER_BLE_CHUNK},
        {"{\"cmd\":\"network\",\"mode\":\"backend\","
         "\"ttl_s\":2147483647}",
         BADGE_USB_CONTROL_SCHEMA_NETWORK,
         BADGE_USB_CONTROL_HANDLER_NETWORK},
        {"{\"cmd\":\"set_backend\",\"url\":\"\",\"wifi_ssid\":\"\","
         "\"wifi_pass\":\"\",\"enable\":false,\"ttl_s\":0}",
         BADGE_USB_CONTROL_SCHEMA_SET_BACKEND,
         BADGE_USB_CONTROL_HANDLER_SET_BACKEND},
        {"{\"cmd\":\"safe_mode\",\"enabled\":false,\"reason\":\"\"}",
         BADGE_USB_CONTROL_SCHEMA_SAFE_MODE,
         BADGE_USB_CONTROL_HANDLER_SAFE_MODE},
        {"{\"cmd\":\"badge_theme\",\"theme\":{"
         "\"version\":1,\"palette\":\"field\",\"background\":\"dark\","
         "\"brightness\":25,\"accents\":{\"drone\":0,\"meta\":65535,"
         "\"tracker\":0,\"flock\":65535,\"wifi_attack\":0,"
         "\"clear\":65535}},\"persist\":false}",
         BADGE_USB_CONTROL_SCHEMA_THEME,
         BADGE_USB_CONTROL_HANDLER_THEME},
        {"{\"cmd\":\"badge_theme\",\"theme\":{"
         "\"version\":1,\"palette\":\"mono\",\"background\":\"dim\","
         "\"brightness\":100,\"accents\":{\"drone\":65535,\"meta\":0,"
         "\"tracker\":65535,\"flock\":0,\"wifi_attack\":65535,"
         "\"clear\":0}},\"persist\":true}",
         BADGE_USB_CONTROL_SCHEMA_THEME,
         BADGE_USB_CONTROL_HANDLER_THEME},
    };
    for (size_t i = 0U; i < ARRAY_SIZE(accepted); ++i) {
        assert_control_ok(&accepted[i]);
    }

    static const char *const rejected[] = {
        "{\"cmd\":\"ble_investigate\","
        "\"request_id\":\"123456789012345678901234567890123\","
        "\"mode\":\"gatt\",\"target\":\"AA:BB:CC:DD:EE:FF\","
        "\"timeout_ms\":1}",
        "{\"cmd\":\"network\",\"mode\":\"backend\","
        "\"ttl_s\":2147483648}",
        "{\"cmd\":\"network\",\"mode\":\"backend\","
        "\"ttl_s\":-2147483649}",
        "{\"cmd\":\"badge_theme\",\"theme\":{"
        "\"version\":1,\"palette\":\"field\",\"background\":\"dark\","
        "\"brightness\":24,\"accents\":{\"drone\":0,\"meta\":0,"
        "\"tracker\":0,\"flock\":0,\"wifi_attack\":0,\"clear\":0}},"
        "\"persist\":false}",
        "{\"cmd\":\"badge_theme\",\"theme\":{"
        "\"version\":1,\"palette\":\"field\",\"background\":\"dark\","
        "\"brightness\":101,\"accents\":{\"drone\":0,\"meta\":0,"
        "\"tracker\":0,\"flock\":0,\"wifi_attack\":0,\"clear\":0}},"
        "\"persist\":false}",
        "{\"cmd\":\"badge_theme\",\"theme\":{"
        "\"version\":1,\"palette\":\"field\",\"background\":\"dark\","
        "\"brightness\":25,\"accents\":{\"drone\":65536,\"meta\":0,"
        "\"tracker\":0,\"flock\":0,\"wifi_attack\":0,\"clear\":0}},"
        "\"persist\":false}",
    };
    for (size_t i = 0U; i < ARRAY_SIZE(rejected); ++i) {
        assert_control_rejected(rejected[i]);
    }
}

void test_badge_usb_control_registry_accepts_every_closed_enum_alias(void)
{
    static const char *const power_modes[] = {
        "active", "quiet", "on", "off",
    };
    static const char *const network_modes[] = {
        "off", "usb", "usb_only", "local_ap", "ap", "backend",
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
    char json[256];

    for (size_t i = 0U; i < ARRAY_SIZE(power_modes); ++i) {
        int length = snprintf(
            json, sizeof(json),
            "{\"cmd\":\"power_mode\",\"mode\":\"%s\"}",
            power_modes[i]);
        TEST_ASSERT_GREATER_THAN_INT(0, length);
        control_case_t test_case = {
            json, BADGE_USB_CONTROL_SCHEMA_POWER_MODE,
            BADGE_USB_CONTROL_HANDLER_POWER_MODE};
        assert_control_ok(&test_case);
    }
    for (size_t i = 0U; i < ARRAY_SIZE(network_modes); ++i) {
        int length = snprintf(
            json, sizeof(json),
            "{\"cmd\":\"network\",\"mode\":\"%s\",\"ttl_s\":1}",
            network_modes[i]);
        TEST_ASSERT_GREATER_THAN_INT(0, length);
        control_case_t test_case = {
            json, BADGE_USB_CONTROL_SCHEMA_NETWORK,
            BADGE_USB_CONTROL_HANDLER_NETWORK};
        assert_control_ok(&test_case);
    }
    for (size_t i = 0U; i < ARRAY_SIZE(nav_actions); ++i) {
        int length = snprintf(
            json, sizeof(json),
            "{\"cmd\":\"display_nav\",\"action\":\"%s\"}",
            nav_actions[i]);
        TEST_ASSERT_GREATER_THAN_INT(0, length);
        control_case_t test_case = {
            json, BADGE_USB_CONTROL_SCHEMA_DISPLAY_NAV,
            BADGE_USB_CONTROL_HANDLER_DISPLAY_NAV};
        assert_control_ok(&test_case);
    }
    for (size_t uart = 0U; uart < ARRAY_SIZE(scanner_uarts); ++uart) {
        for (size_t view = 0U; view < ARRAY_SIZE(scanner_views); ++view) {
            int length = snprintf(
                json, sizeof(json),
                "{\"cmd\":\"scanner_display\",\"uart\":\"%s\","
                "\"button_enabled\":true,\"view\":\"%s\",\"page\":0,"
                "\"page_lock\":false,\"auto_page\":true}",
                scanner_uarts[uart], scanner_views[view]);
            TEST_ASSERT_GREATER_THAN_INT(0, length);
            control_case_t test_case = {
                json, BADGE_USB_CONTROL_SCHEMA_SCANNER_DISPLAY_BUTTON,
                BADGE_USB_CONTROL_HANDLER_SCANNER_DISPLAY};
            assert_control_ok(&test_case);
        }
    }
}

void test_badge_usb_control_ble_request_preserves_timeout_to_uart_and_state(
    void)
{
    static const uint32_t timeouts[] = {1U, 7500U, 12000U};
    for (size_t i = 0U; i < ARRAY_SIZE(timeouts); ++i) {
        char json[192];
        int length = snprintf(
            json, sizeof(json),
            "{\"cmd\":\"ble_investigate\",\"request_id\":\"req-%lu\","
            "\"mode\":\"gatt\",\"target\":\"aa:bb:cc:dd:ee:ff\","
            "\"timeout_ms\":%lu}",
            (unsigned long)timeouts[i],
            (unsigned long)timeouts[i]);
        TEST_ASSERT_GREATER_THAN_INT(0, length);

        ble_investigation_request_t request;
        TEST_ASSERT_TRUE(badge_usb_control_decode_ble_investigate(
            (const uint8_t *)json, (size_t)length, &request));
        TEST_ASSERT_EQUAL_UINT32(timeouts[i], request.timeout_ms);
        TEST_ASSERT_EQUAL_STRING(
            "AA:BB:CC:DD:EE:FF", request.target_mac);

        ble_investigation_request_t normalized;
        TEST_ASSERT_TRUE(badge_ble_investigation_request_validate(
            &request, &normalized));
        TEST_ASSERT_EQUAL_STRING(
            "AA:BB:CC:DD:EE:FF", normalized.target_mac);

        char uart_json[256];
        TEST_ASSERT_GREATER_THAN_UINT(
            0U, ble_investigation_request_to_json(
                    &normalized, uart_json, sizeof(uart_json)));
        char timeout_fragment[48];
        snprintf(
            timeout_fragment, sizeof(timeout_fragment),
            "\"timeout_ms\":%lu", (unsigned long)timeouts[i]);
        TEST_ASSERT_NOT_NULL(strstr(uart_json, timeout_fragment));

        badge_ble_investigation_state_t state;
        badge_ble_investigation_state_init(&state);
        int scanner_slot = -1;
        const int64_t now_ms = 1000;
        TEST_ASSERT_TRUE(badge_ble_investigation_state_start_at(
            &state, &normalized, true, now_ms, &scanner_slot));
        TEST_ASSERT_EQUAL(
            BADGE_BLE_INVESTIGATION_SCANNER_SLOT, scanner_slot);
        TEST_ASSERT_EQUAL_INT64(
            now_ms + (int64_t)timeouts[i] +
                BADGE_BLE_INVESTIGATION_TRANSPORT_GRACE_MS,
            state.deadline_ms);
    }
}

void test_badge_usb_control_ble_request_invalid_timeout_has_zero_downstream_effects(
    void)
{
    static const char *const invalid[] = {
        "{\"cmd\":\"ble_investigate\",\"request_id\":\"req-0\","
        "\"mode\":\"gatt\",\"target\":\"AA:BB:CC:DD:EE:FF\","
        "\"timeout_ms\":0}",
        "{\"cmd\":\"ble_investigate\",\"request_id\":\"req-12001\","
        "\"mode\":\"gatt\",\"target\":\"AA:BB:CC:DD:EE:FF\","
        "\"timeout_ms\":12001}",
    };
    uint32_t uart_writes = 0U;
    uint32_t state_starts = 0U;
    for (size_t i = 0U; i < ARRAY_SIZE(invalid); ++i) {
        ble_investigation_request_t request;
        memset(&request, 0xa5, sizeof(request));
        if (badge_usb_control_decode_ble_investigate(
                (const uint8_t *)invalid[i], strlen(invalid[i]),
                &request)) {
            char uart_json[256];
            if (ble_investigation_request_to_json(
                    &request, uart_json, sizeof(uart_json)) > 0U) {
                uart_writes++;
            }
            badge_ble_investigation_state_t state;
            badge_ble_investigation_state_init(&state);
            int scanner_slot = -1;
            if (badge_ble_investigation_state_start_at(
                    &state, &request, true, 1000, &scanner_slot)) {
                state_starts++;
            }
        }
        ble_investigation_request_t zero = {0};
        TEST_ASSERT_EQUAL_MEMORY(&zero, &request, sizeof(request));
    }
    TEST_ASSERT_EQUAL_UINT32(0U, uart_writes);
    TEST_ASSERT_EQUAL_UINT32(0U, state_starts);
}

void test_badge_usb_control_ingress_routes_authorized_schema_and_handler_ids(
    void)
{
    static const uint8_t line[] =
        "FOF_CTL:{\"cmd\":\"network\",\"mode\":\"backend\","
        "\"ttl_s\":7500}";
    serial_config_ingress_result_t result;

    TEST_ASSERT_TRUE(serial_config_ingress_authorize(
        line, sizeof(line) - 1U, &result));
    TEST_ASSERT_EQUAL(SERIAL_CONFIG_INGRESS_CTL_COMPAT, result.kind);
    TEST_ASSERT_EQUAL(
        BADGE_USB_CONTROL_SCHEMA_NETWORK, result.control_schema_id);
    TEST_ASSERT_EQUAL(
        BADGE_USB_CONTROL_HANDLER_NETWORK, result.control_handler_kind);
    TEST_ASSERT_EQUAL(
        FOF_FW_JSON_SCHEMA_NONE, result.firmware_schema_id);

    static const uint8_t firmware_line[] =
        "FOF_CTL:{\"cmd\":\"fw_check\"}";
    TEST_ASSERT_TRUE(serial_config_ingress_authorize(
        firmware_line, sizeof(firmware_line) - 1U, &result));
    TEST_ASSERT_EQUAL(SERIAL_CONFIG_INGRESS_CTL_FIRMWARE, result.kind);
    TEST_ASSERT_EQUAL(
        FOF_FW_JSON_SCHEMA_USB_FW_CHECK, result.firmware_schema_id);
    TEST_ASSERT_EQUAL(
        BADGE_USB_CONTROL_SCHEMA_NONE, result.control_schema_id);
    TEST_ASSERT_EQUAL(
        BADGE_USB_CONTROL_HANDLER_NONE, result.control_handler_kind);
}

void test_badge_usb_control_ingress_keeps_firmware_first_without_fallback(void)
{
    static const uint8_t malformed_firmware[] =
        "FOF_CTL:{\"cmd\":\"fw_check\",\"mode\":\"quiet\"}";
    static const uint8_t malformed_control[] =
        "FOF_CTL:{\"cmd\":\"power_mode\"}";
    serial_config_ingress_result_t result;

    memset(&result, 0xa5, sizeof(result));
    TEST_ASSERT_FALSE(serial_config_ingress_authorize(
        malformed_firmware, sizeof(malformed_firmware) - 1U, &result));
    TEST_ASSERT_EQUAL(SERIAL_CONFIG_INGRESS_REJECTED, result.kind);
    TEST_ASSERT_EQUAL(
        FOF_FW_JSON_SCHEMA_NONE, result.firmware_schema_id);
    TEST_ASSERT_EQUAL(
        BADGE_USB_CONTROL_SCHEMA_NONE, result.control_schema_id);
    TEST_ASSERT_EQUAL(
        BADGE_USB_CONTROL_HANDLER_NONE, result.control_handler_kind);

    memset(&result, 0xa5, sizeof(result));
    TEST_ASSERT_FALSE(serial_config_ingress_authorize(
        malformed_control, sizeof(malformed_control) - 1U, &result));
    TEST_ASSERT_EQUAL(SERIAL_CONFIG_INGRESS_REJECTED, result.kind);
    TEST_ASSERT_EQUAL(
        BADGE_USB_CONTROL_HANDLER_NONE, result.control_handler_kind);
}

void test_badge_usb_control_android_shared_fixtures_resolve_exact_schemas(void)
{
    static const char *const paths[] = {
        "test/fixtures/android_badge_usb_controls_v1.tsv",
        "../test/fixtures/android_badge_usb_controls_v1.tsv",
        "../../test/fixtures/android_badge_usb_controls_v1.tsv",
    };
    FILE *fixture = NULL;
    for (size_t i = 0U; i < ARRAY_SIZE(paths) && !fixture; ++i) {
        fixture = fopen(paths[i], "rb");
    }
    TEST_ASSERT_NOT_NULL(fixture);

    uint8_t bytes[16384];
    size_t byte_len = fread(bytes, 1U, sizeof(bytes), fixture);
    TEST_ASSERT_EQUAL_INT(0, ferror(fixture));
    TEST_ASSERT_EQUAL_INT(EOF, fgetc(fixture));
    TEST_ASSERT_EQUAL_INT(0, fclose(fixture));
    TEST_ASSERT_GREATER_THAN_UINT(0U, byte_len);
    TEST_ASSERT_EQUAL_UINT8('\n', bytes[byte_len - 1U]);

    for (size_t i = 0U; i < byte_len; ++i) {
        uint8_t byte = bytes[i];
        TEST_ASSERT_NOT_EQUAL('\r', byte);
        TEST_ASSERT_NOT_EQUAL('\0', byte);
        TEST_ASSERT_NOT_EQUAL(0x7f, byte);
        TEST_ASSERT_TRUE(
            byte >= 0x20U || byte == '\t' || byte == '\n');
    }

    uint32_t seen = 0U;
    size_t row_count = 0U;
    size_t line_start = 0U;
    while (line_start < byte_len) {
        size_t line_end = line_start;
        while (line_end < byte_len && bytes[line_end] != '\n') {
            line_end++;
        }
        TEST_ASSERT_LESS_THAN_UINT(byte_len, line_end);
        TEST_ASSERT_GREATER_THAN_UINT(line_start, line_end);

        size_t tab = line_start;
        size_t tab_count = 0U;
        for (size_t i = line_start; i < line_end; ++i) {
            if (bytes[i] == '\t') {
                tab = i;
                tab_count++;
            }
        }
        TEST_ASSERT_EQUAL_UINT(1U, tab_count);
        TEST_ASSERT_GREATER_THAN_UINT(line_start, tab);
        TEST_ASSERT_GREATER_THAN_UINT(tab + 1U, line_end);

        size_t fixture_index = ARRAY_SIZE(ANDROID_FIXTURE_CASES);
        for (size_t i = 0U; i < ARRAY_SIZE(ANDROID_FIXTURE_CASES); ++i) {
            size_t name_len = strlen(ANDROID_FIXTURE_CASES[i].name);
            if (name_len == tab - line_start &&
                memcmp(
                    bytes + line_start,
                    ANDROID_FIXTURE_CASES[i].name,
                    name_len) == 0) {
                fixture_index = i;
                break;
            }
        }
        TEST_ASSERT_LESS_THAN_UINT(
            ARRAY_SIZE(ANDROID_FIXTURE_CASES), fixture_index);
        uint32_t fixture_bit = UINT32_C(1) << fixture_index;
        TEST_ASSERT_EQUAL_UINT32(0U, seen & fixture_bit);

        badge_usb_control_schema_id_t schema_id =
            BADGE_USB_CONTROL_SCHEMA_NONE;
        badge_usb_control_handler_kind_t handler_kind =
            BADGE_USB_CONTROL_HANDLER_NONE;
        TEST_ASSERT_EQUAL(
            BADGE_USB_CONTROL_REGISTRY_OK,
            badge_usb_control_select_and_validate(
                bytes + tab + 1U,
                line_end - tab - 1U,
                &schema_id,
                &handler_kind));
        TEST_ASSERT_EQUAL(
            ANDROID_FIXTURE_CASES[fixture_index].schema_id,
            schema_id);
        const badge_usb_control_schema_descriptor_t *descriptor =
            badge_usb_control_schema_descriptor(schema_id);
        TEST_ASSERT_NOT_NULL(descriptor);
        TEST_ASSERT_EQUAL(descriptor->handler_kind, handler_kind);

        seen |= fixture_bit;
        row_count++;
        line_start = line_end + 1U;
    }

    TEST_ASSERT_EQUAL_UINT(
        ARRAY_SIZE(ANDROID_FIXTURE_CASES), row_count);
    TEST_ASSERT_EQUAL_UINT32(
        (UINT32_C(1) << ARRAY_SIZE(ANDROID_FIXTURE_CASES)) - 1U,
        seen);
}

void test_badge_usb_control_recovery_uses_exact_authorized_schema_ids(void)
{
    static const uint8_t status[] =
        "FOF_CTL:{\"cmd\":\"status\"}";
    static const uint8_t reboot[] =
        "FOF_CTL:{\"cmd\":\"reboot\"}";
    static const uint8_t malformed[] =
        "FOF_CTL:{\"cmd\":\"reboot\",\"extra\":true}";
    serial_config_ingress_result_t result;

    TEST_ASSERT_TRUE(serial_config_ingress_authorize(
        status, sizeof(status) - 1U, &result));
    TEST_ASSERT_EQUAL(
        SERIAL_CONFIG_INGRESS_CTL_COMPAT, result.kind);
    TEST_ASSERT_EQUAL(
        BADGE_USB_CONTROL_SCHEMA_STATUS, result.control_schema_id);
    TEST_ASSERT_EQUAL(
        BADGE_USB_CONTROL_HANDLER_STATUS, result.control_handler_kind);

    TEST_ASSERT_TRUE(serial_config_ingress_authorize(
        reboot, sizeof(reboot) - 1U, &result));
    TEST_ASSERT_EQUAL(
        SERIAL_CONFIG_INGRESS_CTL_COMPAT, result.kind);
    TEST_ASSERT_EQUAL(
        BADGE_USB_CONTROL_SCHEMA_REBOOT, result.control_schema_id);
    TEST_ASSERT_EQUAL(
        BADGE_USB_CONTROL_HANDLER_REBOOT, result.control_handler_kind);

    TEST_ASSERT_EQUAL(
        SERIAL_CONFIG_RECOVERY_STATUS,
        serial_config_recovery_command_classify(
            status, sizeof(status) - 1U));
    TEST_ASSERT_EQUAL(
        SERIAL_CONFIG_RECOVERY_APP_REBOOT,
        serial_config_recovery_command_classify(
            reboot, sizeof(reboot) - 1U));
    TEST_ASSERT_EQUAL(
        SERIAL_CONFIG_RECOVERY_DENIED,
        serial_config_recovery_command_classify(
            malformed, sizeof(malformed) - 1U));
}
