#include "unity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "firmware_json_schema_registry.h"

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))

typedef struct {
    fof_fw_json_schema_id_t id;
    fof_fw_json_ingress_t ingress;
    const char *name;
    const char *selector_name;
    const char *selector_value;
    const char *wire;
} registry_fixture_t;

static const registry_fixture_t REGISTRY_FIXTURES[] = {
    {
        FOF_FW_JSON_SCHEMA_USB_FW_RELAY_BASE,
        FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
        "usb_fw_relay_base",
        "cmd",
        "fw_relay",
        "{\"cmd\":\"fw_relay\",\"uart\":\"uart1\","
        "\"expected_generation\":1,\"expected_hardware_id\":\"hw\","
        "\"allow_same_version\":false}",
    },
    {
        FOF_FW_JSON_SCHEMA_USB_FW_RELAY_FORCED,
        FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
        "usb_fw_relay_forced",
        "cmd",
        "fw_relay",
        "{\"cmd\":\"fw_relay\",\"uart\":\"uart1\","
        "\"expected_generation\":1,\"expected_hardware_id\":\"hw\","
        "\"allow_same_version\":false,\"force\":true,"
        "\"skip_command_probe\":true}",
    },
    {
        FOF_FW_JSON_SCHEMA_USB_FW_UPLOAD_BEGIN,
        FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
        "usb_fw_upload_begin",
        "cmd",
        "fw_upload_begin",
        "{\"cmd\":\"fw_upload_begin\",\"name\":\"scanner\","
        "\"target\":\"scanner\",\"project\":\"fof_scanner\","
        "\"hardware_type\":\"esp32s3\",\"version\":\"1\","
        "\"size\":1,\"crc32\":2,\"sha256\":\"aa\","
        "\"slot_mask\":3,\"flow_control\":\"credit-v1\"}",
    },
    {
        FOF_FW_JSON_SCHEMA_USB_FW_UPLOAD_BEGIN_SESSION,
        FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
        "usb_fw_upload_begin_session",
        "cmd",
        "fw_upload_begin",
        "{\"cmd\":\"fw_upload_begin\",\"name\":\"scanner\","
        "\"target\":\"scanner\",\"project\":\"fof_scanner\","
        "\"hardware_type\":\"esp32s3\",\"version\":\"1\","
        "\"size\":1,\"crc32\":2,\"sha256\":\"aa\","
        "\"slot_mask\":3,\"flow_control\":\"credit-v1\","
        "\"session\":\"0123456789ABCDEF\"}",
    },
    {
        FOF_FW_JSON_SCHEMA_USB_UPLINK_OTA_BEGIN,
        FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
        "usb_uplink_ota_begin",
        "cmd",
        "uplink_ota_begin",
        "{\"cmd\":\"uplink_ota_begin\",\"target\":\"uplink\","
        "\"project\":\"fof_uplink\",\"hardware_type\":\"esp32s3\","
        "\"version\":\"1\",\"size\":1,\"crc32\":2,"
        "\"sha256\":\"aa\",\"flow_control\":\"credit-v1\","
        "\"recovery_rewrite_same_version\":false}",
    },
    {
        FOF_FW_JSON_SCHEMA_USB_UPLINK_OTA_BEGIN_SESSION,
        FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
        "usb_uplink_ota_begin_session",
        "cmd",
        "uplink_ota_begin",
        "{\"cmd\":\"uplink_ota_begin\",\"target\":\"uplink\","
        "\"project\":\"fof_uplink\",\"hardware_type\":\"esp32s3\","
        "\"version\":\"1\",\"size\":1,\"crc32\":2,"
        "\"sha256\":\"aa\",\"flow_control\":\"credit-v1\","
        "\"recovery_rewrite_same_version\":false,"
        "\"session\":\"0123456789ABCDEF\"}",
    },
    {
        FOF_FW_JSON_SCHEMA_USB_FW_CHECK,
        FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
        "usb_fw_check",
        "cmd",
        "fw_check",
        "{\"cmd\":\"fw_check\"}",
    },
    {
        FOF_FW_JSON_SCHEMA_USB_FW_CHECK_UART,
        FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
        "usb_fw_check_uart",
        "cmd",
        "fw_check",
        "{\"cmd\":\"fw_check\",\"uart\":\"uart1\"}",
    },
    {
        FOF_FW_JSON_SCHEMA_USB_FW_CHECK_NOW,
        FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
        "usb_fw_check_now",
        "cmd",
        "fw_check_now",
        "{\"cmd\":\"fw_check_now\"}",
    },
    {
        FOF_FW_JSON_SCHEMA_USB_FW_CHECK_NOW_UART,
        FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
        "usb_fw_check_now_uart",
        "cmd",
        "fw_check_now",
        "{\"cmd\":\"fw_check_now\",\"uart\":\"uart2\"}",
    },
    {
        FOF_FW_JSON_SCHEMA_SCANNER_FW_OFFER,
        FOF_FW_JSON_INGRESS_UPLINK_TO_SCANNER_UART,
        "scanner_fw_offer",
        "type",
        "fw_offer",
        "{\"type\":\"fw_offer\",\"update\":true,"
        "\"target_ver\":\"1\",\"fw_name\":\"scanner\","
        "\"app_project\":\"fof_scanner\",\"hardware_type\":\"esp32s3\","
        "\"sha256\":\"aa\",\"generation\":1,\"size\":2,\"crc\":3,"
        "\"reason\":\"new\"}",
    },
    {
        FOF_FW_JSON_SCHEMA_SCANNER_FW_CHECK_NOW,
        FOF_FW_JSON_INGRESS_UPLINK_TO_SCANNER_UART,
        "scanner_fw_check_now",
        "type",
        "fw_check_now",
        "{\"type\":\"fw_check_now\"}",
    },
    {
        FOF_FW_JSON_SCHEMA_SCANNER_OTA_BEGIN,
        FOF_FW_JSON_INGRESS_UPLINK_TO_SCANNER_UART,
        "scanner_ota_begin",
        "type",
        "ota_begin",
        "{\"type\":\"ota_begin\",\"session_id\":\"s\","
        "\"size\":2,\"crc\":3,\"sha256\":\"aa\",\"target_ver\":\"1\","
        "\"fw_name\":\"scanner\",\"app_project\":\"fof_scanner\","
        "\"hardware_type\":\"esp32s3\",\"generation\":1,"
        "\"allow_same_version\":false}",
    },
    {
        FOF_FW_JSON_SCHEMA_SCANNER_OTA_END,
        FOF_FW_JSON_INGRESS_UPLINK_TO_SCANNER_UART,
        "scanner_ota_end",
        "type",
        "ota_end",
        "{\"type\":\"ota_end\",\"session_id\":\"s\","
        "\"size\":2,\"crc\":3,\"sha256\":\"aa\",\"target_ver\":\"1\","
        "\"fw_name\":\"scanner\",\"app_project\":\"fof_scanner\","
        "\"hardware_type\":\"esp32s3\",\"generation\":1,"
        "\"allow_same_version\":false}",
    },
    {
        FOF_FW_JSON_SCHEMA_SCANNER_OTA_ABORT_ACTIVE,
        FOF_FW_JSON_INGRESS_UPLINK_TO_SCANNER_UART,
        "scanner_ota_abort_active",
        "type",
        "ota_abort",
        "{\"type\":\"ota_abort\",\"session_id\":\"s\"}",
    },
    {
        FOF_FW_JSON_SCHEMA_SCANNER_OTA_ABORT_UNBOUND,
        FOF_FW_JSON_INGRESS_UPLINK_TO_SCANNER_UART,
        "scanner_ota_abort_unbound",
        "type",
        "ota_abort",
        "{\"type\":\"ota_abort\"}",
    },
    {
        FOF_FW_JSON_SCHEMA_RECEIPT_FW_CHECK,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_fw_check",
        "type",
        "fw_check",
        "{\"type\":\"fw_check\",\"board\":\"scanner\",\"ver\":\"1\","
        "\"caps\":\"ota\",\"fw_state\":\"idle\",\"fw_check_count\":1,"
        "\"last_fw_error\":\"\",\"reason\":\"boot\","
        "\"ota_state\":\"idle\",\"recovery_mode\":\"none\","
        "\"rollback_pending\":false,\"crash_count\":0}",
    },
    {
        FOF_FW_JSON_SCHEMA_RECEIPT_FW_READY_STRICT,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_fw_ready_strict",
        "type",
        "fw_ready",
        "{\"type\":\"fw_ready\",\"board\":\"scanner\",\"ver\":\"1\","
        "\"target_ver\":\"2\",\"fw_name\":\"scanner\","
        "\"app_project\":\"fof_scanner\",\"hardware_type\":\"esp32s3\","
        "\"sha256\":\"aa\",\"generation\":1,\"size\":2,\"crc\":3,"
        "\"allow_same_version\":false}",
    },
    {
        FOF_FW_JSON_SCHEMA_RECEIPT_FW_READY_LEGACY_68,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_fw_ready_legacy_68",
        "type",
        "fw_ready",
        "{\"type\":\"fw_ready\",\"board\":\"scanner\",\"ver\":\"1\","
        "\"target_ver\":\"2\",\"size\":2,\"crc\":3}",
    },
    {
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_ACK_MODERN,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_ota_ack_modern",
        "type",
        "ota_ack",
        "{\"type\":\"ota_ack\",\"session_id\":\"s\","
        "\"target_ver\":\"2\",\"fw_name\":\"scanner\","
        "\"app_project\":\"fof_scanner\",\"hardware_type\":\"esp32s3\","
        "\"sha256\":\"aa\",\"generation\":1,\"size\":2,\"crc\":3,"
        "\"allow_same_version\":false,\"received\":0}",
    },
    {
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_ACK_LEGACY_68,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_ota_ack_legacy_68",
        "type",
        "ota_ack",
        "{\"type\":\"ota_ack\",\"session_id\":\"s\"}",
    },
    {
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_STAGED_MODERN,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_ota_staged_modern",
        "type",
        "ota_staged",
        "{\"type\":\"ota_staged\",\"session_id\":\"s\","
        "\"target_ver\":\"2\",\"fw_name\":\"scanner\","
        "\"app_project\":\"fof_scanner\",\"hardware_type\":\"esp32s3\","
        "\"sha256\":\"aa\",\"generation\":1,\"size\":2,\"crc\":3,"
        "\"allow_same_version\":false,\"received\":2}",
    },
    {
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_DONE_MODERN,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_ota_done_modern",
        "type",
        "ota_done",
        "{\"type\":\"ota_done\",\"session_id\":\"s\","
        "\"target_ver\":\"2\",\"fw_name\":\"scanner\","
        "\"app_project\":\"fof_scanner\",\"hardware_type\":\"esp32s3\","
        "\"sha256\":\"aa\",\"generation\":1,\"size\":2,\"crc\":3,"
        "\"allow_same_version\":false,\"received\":2}",
    },
    {
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_DONE_LEGACY_68,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_ota_done_legacy_68",
        "type",
        "ota_done",
        "{\"type\":\"ota_done\",\"session_id\":\"s\",\"received\":2}",
    },
    {
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_PROGRESS_ACTIVE_SHARED,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_ota_progress_active_shared",
        "type",
        "ota_progress",
        "{\"type\":\"ota_progress\",\"session_id\":\"s\","
        "\"received\":1,\"total\":2,\"percent\":50}",
    },
    {
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_PROGRESS_UNBOUND_MODERN,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_ota_progress_unbound_modern",
        "type",
        "ota_progress",
        "{\"type\":\"ota_progress\",\"received\":1,"
        "\"total\":2,\"percent\":50}",
    },
    {
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_NACK_ACTIVE_SHARED,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_ota_nack_active_shared",
        "type",
        "ota_nack",
        "{\"type\":\"ota_nack\",\"session_id\":\"s\",\"seq\":1}",
    },
    {
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_NACK_UNBOUND_SHARED,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_ota_nack_unbound_shared",
        "type",
        "ota_nack",
        "{\"type\":\"ota_nack\",\"seq\":1}",
    },
    {
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_ERROR_ACTIVE_SHARED,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_ota_error_active_shared",
        "type",
        "ota_error",
        "{\"type\":\"ota_error\",\"session_id\":\"s\","
        "\"reason\":\"bad\",\"received\":1}",
    },
    {
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_ERROR_PRESESSION_MODERN,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_ota_error_presession_modern",
        "type",
        "ota_error",
        "{\"type\":\"ota_error\",\"reason\":\"bad\"}",
    },
    {
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_ERROR_PRESESSION_LEGACY_68,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_ota_error_presession_legacy_68",
        "type",
        "ota_error",
        "{\"type\":\"ota_error\",\"reason\":\"bad\",\"received\":0}",
    },
    {
        FOF_FW_JSON_SCHEMA_RECEIPT_STOP_ACK_SHARED,
        FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
        "receipt_stop_ack_shared",
        "type",
        "stop_ack",
        "{\"type\":\"stop_ack\"}",
    },
};

static fof_fw_json_registry_result_t select_fixture(
    const registry_fixture_t *fixture,
    fof_fw_json_schema_id_t *id_out)
{
    return fof_fw_json_select_and_validate(
        fixture->ingress,
        (const uint8_t *)fixture->wire,
        strlen(fixture->wire),
        id_out);
}

static void assert_registry_rejects(
    const char *label,
    fof_fw_json_ingress_t ingress,
    const char *wire,
    fof_fw_json_registry_result_t expected)
{
    fof_fw_json_schema_id_t id = FOF_FW_JSON_SCHEMA_USB_FW_RELAY_BASE;
    fof_fw_json_registry_result_t result =
        fof_fw_json_select_and_validate(
            ingress, (const uint8_t *)wire, strlen(wire), &id);

    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, result, label);
    TEST_ASSERT_EQUAL_INT_MESSAGE(FOF_FW_JSON_SCHEMA_NONE, id, label);
}

void test_firmware_json_registry_accepts_all_30_exact_schema_fixtures(void)
{
    TEST_ASSERT_EQUAL_UINT(32U, ARRAY_SIZE(REGISTRY_FIXTURES));
    TEST_ASSERT_EQUAL_INT(33, FOF_FW_JSON_SCHEMA_COUNT);

    for (size_t i = 0U; i < ARRAY_SIZE(REGISTRY_FIXTURES); ++i) {
        const registry_fixture_t *fixture = &REGISTRY_FIXTURES[i];
        fof_fw_json_schema_id_t id = FOF_FW_JSON_SCHEMA_NONE;
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            FOF_FW_JSON_REGISTRY_OK,
            select_fixture(fixture, &id),
            fixture->name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(fixture->id, id, fixture->name);

        const fof_fw_json_schema_descriptor_t *descriptor =
            fof_fw_json_schema_descriptor(id);
        TEST_ASSERT_NOT_NULL_MESSAGE(descriptor, fixture->name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            fixture->ingress, descriptor->ingress, fixture->name);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(
            fixture->name, descriptor->name, fixture->name);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(
            fixture->selector_name,
            descriptor->selector_name,
            fixture->name);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(
            fixture->selector_value,
            descriptor->selector_value,
            fixture->name);
    }
}

void test_firmware_json_registry_rejects_uplink_ota_session_shape_corruption(
    void)
{
    static const char *cases[] = {
        "{\"cmd\":\"uplink_ota_begin\",\"target\":\"uplink\","
        "\"project\":\"fof_uplink\",\"hardware_type\":\"esp32s3\","
        "\"version\":\"1\",\"size\":1,\"crc32\":2,\"sha256\":\"aa\","
        "\"flow_control\":\"credit-v1\","
        "\"recovery_rewrite_same_version\":false,"
        "\"session\":\"0123456789ABCDEF\",\"extra\":false}",
        "{\"cmd\":\"uplink_ota_begin\","
        "\"project\":\"fof_uplink\",\"hardware_type\":\"esp32s3\","
        "\"version\":\"1\",\"size\":1,\"crc32\":2,\"sha256\":\"aa\","
        "\"flow_control\":\"credit-v1\","
        "\"recovery_rewrite_same_version\":false,"
        "\"session\":\"0123456789ABCDEF\"}",
        "{\"cmd\":\"uplink_ota_begin\",\"target\":\"uplink\","
        "\"project\":\"fof_uplink\",\"hardware_type\":\"esp32s3\","
        "\"version\":\"1\",\"size\":1,\"crc32\":2,\"sha256\":\"aa\","
        "\"flow_control\":\"credit-v1\","
        "\"recovery_rewrite_same_version\":false,\"session\":1}",
        "{\"cmd\":\"uplink_ota_begin\",\"target\":\"uplink\","
        "\"project\":\"fof_uplink\",\"hardware_type\":\"esp32s3\","
        "\"version\":\"1\",\"size\":1,\"crc32\":2,\"sha256\":\"aa\","
        "\"flow_control\":\"credit-v1\","
        "\"recovery_rewrite_same_version\":false,"
        "\"session\":\"0123456789ABCDEF\","
        "\"session\":\"0123456789ABCDEF\"}",
        "{\"cmd\":\"uplink_ota_begin\",\"target\":\"uplink\","
        "\"target\":\"uplink\",\"project\":\"fof_uplink\","
        "\"hardware_type\":\"esp32s3\",\"version\":\"1\","
        "\"size\":1,\"crc32\":2,\"sha256\":\"aa\","
        "\"flow_control\":\"credit-v1\","
        "\"recovery_rewrite_same_version\":false,"
        "\"session\":\"0123456789ABCDEF\"}",
        "{\"cmd\":\"uplink_ota_begin\",\"target\":\"uplink\","
        "\"project\":\"fof_uplink\",\"hardware_type\":\"esp32s3\","
        "\"version\":\"1\",\"size\":1,\"crc32\":2,\"sha256\":\"aa\","
        "\"flow_control\":\"credit-v1\","
        "\"recovery_rewrite_same_version\":false,"
        "\"other\":\"0123456789ABCDEF\"}",
    };

    for (size_t i = 0U; i < ARRAY_SIZE(cases); ++i) {
        assert_registry_rejects(
            "uplink ota session corruption",
            FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
            cases[i],
            FOF_FW_JSON_REGISTRY_NO_EXACT_SCHEMA);
    }
}

static bool descriptor_member_sets_equal(
    const fof_fw_json_schema_descriptor_t *left,
    const fof_fw_json_schema_descriptor_t *right)
{
    if (!left || !right || left->member_count != right->member_count) {
        return false;
    }
    for (size_t i = 0U; i < left->member_count; ++i) {
        bool found = false;
        for (size_t j = 0U; j < right->member_count; ++j) {
            if (left->members[i].type == right->members[j].type &&
                left->members[i].string_policy ==
                    right->members[j].string_policy &&
                strcmp(left->members[i].name,
                       right->members[j].name) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

void test_firmware_json_registry_descriptors_are_closed_unique_and_exact(void)
{
    TEST_ASSERT_NULL(
        fof_fw_json_schema_descriptor(FOF_FW_JSON_SCHEMA_NONE));
    TEST_ASSERT_NULL(
        fof_fw_json_schema_descriptor(FOF_FW_JSON_SCHEMA_COUNT));
    TEST_ASSERT_NULL(
        fof_fw_json_schema_descriptor((fof_fw_json_schema_id_t)-1));

    for (int raw_id = 1; raw_id < FOF_FW_JSON_SCHEMA_COUNT; ++raw_id) {
        fof_fw_json_schema_id_t id = (fof_fw_json_schema_id_t)raw_id;
        const fof_fw_json_schema_descriptor_t *descriptor =
            fof_fw_json_schema_descriptor(id);
        TEST_ASSERT_NOT_NULL(descriptor);
        TEST_ASSERT_EQUAL_INT(id, descriptor->id);
        TEST_ASSERT_NOT_NULL(descriptor->name);
        TEST_ASSERT_NOT_NULL(descriptor->selector_name);
        TEST_ASSERT_NOT_NULL(descriptor->selector_value);
        TEST_ASSERT_NOT_NULL(descriptor->members);
        TEST_ASSERT_GREATER_THAN_UINT(0U, descriptor->member_count);

        const char *expected_selector =
            descriptor->ingress ==
                    FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB
                ? "cmd"
                : "type";
        TEST_ASSERT_EQUAL_STRING(
            expected_selector, descriptor->selector_name);

        size_t selector_member_count = 0U;
        for (size_t member_index = 0U;
             member_index < descriptor->member_count;
             ++member_index) {
            const fof_json_member_spec_t *member =
                &descriptor->members[member_index];
            TEST_ASSERT_NOT_NULL(member->name);
            bool diagnostic =
                strcmp(member->name, "reason") == 0 ||
                strcmp(member->name, "last_fw_error") == 0;
            if (member->type == FOF_JSON_STRING ||
                member->type == FOF_JSON_NULLABLE_STRING) {
                TEST_ASSERT_EQUAL_INT_MESSAGE(
                    diagnostic
                        ? FOF_JSON_STRING_POLICY_PRINTABLE_UTF8
                        : FOF_JSON_STRING_POLICY_ASCII_TOKEN_NO_ESCAPE,
                    member->string_policy,
                    descriptor->name);
            } else {
                TEST_ASSERT_EQUAL_INT_MESSAGE(
                    FOF_JSON_STRING_POLICY_NONE,
                    member->string_policy,
                    descriptor->name);
            }
            if (strcmp(member->name, descriptor->selector_name) == 0) {
                selector_member_count++;
                TEST_ASSERT_EQUAL_INT(FOF_JSON_STRING, member->type);
                TEST_ASSERT_EQUAL_INT(
                    FOF_JSON_STRING_POLICY_ASCII_TOKEN_NO_ESCAPE,
                    member->string_policy);
            }
            for (size_t prior = 0U; prior < member_index; ++prior) {
                TEST_ASSERT_NOT_EQUAL(
                    0, strcmp(member->name,
                              descriptor->members[prior].name));
            }
        }
        TEST_ASSERT_EQUAL_UINT(1U, selector_member_count);

        for (int other_raw = raw_id + 1;
             other_raw < FOF_FW_JSON_SCHEMA_COUNT;
             ++other_raw) {
            const fof_fw_json_schema_descriptor_t *other =
                fof_fw_json_schema_descriptor(
                    (fof_fw_json_schema_id_t)other_raw);
            TEST_ASSERT_NOT_NULL(other);
            TEST_ASSERT_NOT_EQUAL(
                0, strcmp(descriptor->name, other->name));

            bool duplicate_row =
                descriptor->ingress == other->ingress &&
                strcmp(descriptor->selector_value,
                       other->selector_value) == 0 &&
                descriptor_member_sets_equal(descriptor, other);
            TEST_ASSERT_FALSE_MESSAGE(
                duplicate_row,
                "duplicate context/selector/exact-member-set row");
        }
    }
}

static const registry_fixture_t *fixture_for_id(
    fof_fw_json_schema_id_t id)
{
    for (size_t i = 0U; i < ARRAY_SIZE(REGISTRY_FIXTURES); ++i) {
        if (REGISTRY_FIXTURES[i].id == id) {
            return &REGISTRY_FIXTURES[i];
        }
    }
    return NULL;
}

static void replace_first_field(
    const registry_fixture_t *fixture,
    const char *replacement,
    char *out,
    size_t out_capacity)
{
    const char *separator = strchr(fixture->wire, ',');
    int written;
    if (separator) {
        written = snprintf(
            out, out_capacity, "{%s%s", replacement, separator);
    } else {
        written = snprintf(out, out_capacity, "{%s}", replacement);
    }
    TEST_ASSERT_GREATER_THAN_INT(0, written);
    TEST_ASSERT_LESS_THAN_UINT(out_capacity, (size_t)written);
}

static void remove_first_field(
    const registry_fixture_t *fixture,
    char *out,
    size_t out_capacity)
{
    const char *separator = strchr(fixture->wire, ',');
    int written = separator
        ? snprintf(out, out_capacity, "{%s", separator + 1)
        : snprintf(out, out_capacity, "{}");
    TEST_ASSERT_GREATER_THAN_INT(0, written);
    TEST_ASSERT_LESS_THAN_UINT(out_capacity, (size_t)written);
}

static void append_field(
    const registry_fixture_t *fixture,
    const char *field,
    char *out,
    size_t out_capacity)
{
    size_t wire_len = strlen(fixture->wire);
    TEST_ASSERT_GREATER_THAN_UINT(1U, wire_len);
    int written = snprintf(
        out, out_capacity, "%.*s,%s}",
        (int)(wire_len - 1U), fixture->wire, field);
    TEST_ASSERT_GREATER_THAN_INT(0, written);
    TEST_ASSERT_LESS_THAN_UINT(out_capacity, (size_t)written);
}

static void replace_string_member_content(
    const registry_fixture_t *fixture,
    const char *member_name,
    const char *replacement,
    char *out,
    size_t out_capacity)
{
    char needle[96];
    int needle_len = snprintf(
        needle, sizeof(needle), "\"%s\":\"", member_name);
    TEST_ASSERT_GREATER_THAN_INT(0, needle_len);
    TEST_ASSERT_LESS_THAN_UINT(sizeof(needle), (size_t)needle_len);

    const char *member = strstr(fixture->wire, needle);
    TEST_ASSERT_NOT_NULL_MESSAGE(member, fixture->name);
    const char *value_start = member + (size_t)needle_len;
    const char *value_end = strchr(value_start, '"');
    TEST_ASSERT_NOT_NULL_MESSAGE(value_end, fixture->name);

    size_t prefix_len = (size_t)(value_start - fixture->wire);
    size_t replacement_len = strlen(replacement);
    size_t suffix_len = strlen(value_end);
    TEST_ASSERT_LESS_THAN_UINT(
        out_capacity, prefix_len + replacement_len + suffix_len);

    memcpy(out, fixture->wire, prefix_len);
    memcpy(out + prefix_len, replacement, replacement_len);
    memcpy(out + prefix_len + replacement_len, value_end, suffix_len + 1U);
}

static void assert_registry_policy_rejects(
    const registry_fixture_t *fixture,
    const char *member_name,
    const char *replacement)
{
    char wire[1024];
    replace_string_member_content(
        fixture, member_name, replacement, wire, sizeof(wire));

    fof_fw_json_schema_id_t id = FOF_FW_JSON_SCHEMA_USB_FW_RELAY_BASE;
    TEST_ASSERT_NOT_EQUAL_MESSAGE(
        FOF_FW_JSON_REGISTRY_OK,
        fof_fw_json_select_and_validate(
            fixture->ingress,
            (const uint8_t *)wire,
            strlen(wire),
            &id),
        fixture->name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        FOF_FW_JSON_SCHEMA_NONE, id, fixture->name);
}

void test_firmware_json_registry_enforces_explicit_string_policies_for_all_30_descriptors(
    void)
{
    static const char *token_rejections[] = {
        "bad\\u0041",
        "bad\xE2\x82\xAC",
        "bad token",
    };
    static const char *diagnostic_acceptances[] = {
        "raw \xE2\x82\xAC \xF0\x9F\x98\x80",
        "escaped \\u20ac quote \\\" slash \\/ backslash \\\\",
    };
    static const char *control_rejections[] = {
        "bad\x1f",
        "bad\\u0000",
        "bad\\u001f",
        "bad\\u007f",
    };
    size_t token_members = 0U;
    size_t diagnostic_members = 0U;

    TEST_ASSERT_EQUAL_UINT(32U, ARRAY_SIZE(REGISTRY_FIXTURES));
    for (size_t fixture_index = 0U;
         fixture_index < ARRAY_SIZE(REGISTRY_FIXTURES);
         ++fixture_index) {
        const registry_fixture_t *fixture =
            &REGISTRY_FIXTURES[fixture_index];
        const fof_fw_json_schema_descriptor_t *descriptor =
            fof_fw_json_schema_descriptor(fixture->id);
        TEST_ASSERT_NOT_NULL_MESSAGE(descriptor, fixture->name);

        for (size_t member_index = 0U;
             member_index < descriptor->member_count;
             ++member_index) {
            const fof_json_member_spec_t *member =
                &descriptor->members[member_index];
            if (member->type != FOF_JSON_STRING &&
                member->type != FOF_JSON_NULLABLE_STRING) {
                TEST_ASSERT_EQUAL_INT_MESSAGE(
                    FOF_JSON_STRING_POLICY_NONE,
                    member->string_policy,
                    fixture->name);
                continue;
            }

            bool diagnostic =
                strcmp(member->name, "reason") == 0 ||
                strcmp(member->name, "last_fw_error") == 0;
            if (diagnostic) {
                diagnostic_members++;
                TEST_ASSERT_EQUAL_INT_MESSAGE(
                    FOF_JSON_STRING_POLICY_PRINTABLE_UTF8,
                    member->string_policy,
                    fixture->name);
                for (size_t accepted = 0U;
                     accepted < ARRAY_SIZE(diagnostic_acceptances);
                     ++accepted) {
                    char wire[1024];
                    replace_string_member_content(
                        fixture, member->name,
                        diagnostic_acceptances[accepted],
                        wire, sizeof(wire));
                    fof_fw_json_schema_id_t id =
                        FOF_FW_JSON_SCHEMA_NONE;
                    TEST_ASSERT_EQUAL_INT_MESSAGE(
                        FOF_FW_JSON_REGISTRY_OK,
                        fof_fw_json_select_and_validate(
                            fixture->ingress,
                            (const uint8_t *)wire,
                            strlen(wire),
                            &id),
                        fixture->name);
                    TEST_ASSERT_EQUAL_INT_MESSAGE(
                        fixture->id, id, fixture->name);
                }
            } else {
                token_members++;
                TEST_ASSERT_EQUAL_INT_MESSAGE(
                    FOF_JSON_STRING_POLICY_ASCII_TOKEN_NO_ESCAPE,
                    member->string_policy,
                    fixture->name);
                for (size_t rejected = 0U;
                     rejected < ARRAY_SIZE(token_rejections);
                     ++rejected) {
                    assert_registry_policy_rejects(
                        fixture, member->name,
                        token_rejections[rejected]);
                }
            }

            for (size_t rejected = 0U;
                 rejected < ARRAY_SIZE(control_rejections);
                 ++rejected) {
                assert_registry_policy_rejects(
                    fixture, member->name,
                    control_rejections[rejected]);
            }
        }
    }

    TEST_ASSERT_GREATER_THAN_UINT(0U, token_members);
    TEST_ASSERT_GREATER_THAN_UINT(0U, diagnostic_members);
}

void test_firmware_json_registry_rejects_selector_attacks_for_all_19_groups(
    void)
{
    static const fof_fw_json_schema_id_t group_ids[] = {
        FOF_FW_JSON_SCHEMA_USB_FW_RELAY_BASE,
        FOF_FW_JSON_SCHEMA_USB_FW_UPLOAD_BEGIN,
        FOF_FW_JSON_SCHEMA_USB_UPLINK_OTA_BEGIN,
        FOF_FW_JSON_SCHEMA_USB_FW_CHECK,
        FOF_FW_JSON_SCHEMA_USB_FW_CHECK_NOW,
        FOF_FW_JSON_SCHEMA_SCANNER_FW_OFFER,
        FOF_FW_JSON_SCHEMA_SCANNER_FW_CHECK_NOW,
        FOF_FW_JSON_SCHEMA_SCANNER_OTA_BEGIN,
        FOF_FW_JSON_SCHEMA_SCANNER_OTA_END,
        FOF_FW_JSON_SCHEMA_SCANNER_OTA_ABORT_ACTIVE,
        FOF_FW_JSON_SCHEMA_RECEIPT_FW_CHECK,
        FOF_FW_JSON_SCHEMA_RECEIPT_FW_READY_STRICT,
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_ACK_MODERN,
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_STAGED_MODERN,
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_DONE_MODERN,
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_PROGRESS_ACTIVE_SHARED,
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_NACK_ACTIVE_SHARED,
        FOF_FW_JSON_SCHEMA_RECEIPT_OTA_ERROR_ACTIVE_SHARED,
        FOF_FW_JSON_SCHEMA_RECEIPT_STOP_ACK_SHARED,
    };
    static const char *wrong_types[] = {
        "true", "null", "1", "{}", "[]",
    };

    TEST_ASSERT_EQUAL_UINT(19U, ARRAY_SIZE(group_ids));
    for (size_t i = 0U; i < ARRAY_SIZE(group_ids); ++i) {
        const registry_fixture_t *fixture = fixture_for_id(group_ids[i]);
        TEST_ASSERT_NOT_NULL(fixture);

        char wire[768];
        char field[256];

        snprintf(
            field, sizeof(field), "\"%s\":false",
            fixture->selector_name);
        append_field(fixture, field, wire, sizeof(wire));
        assert_registry_rejects(
            fixture->name, fixture->ingress, wire,
            FOF_FW_JSON_REGISTRY_SELECTOR_REJECTED);

        int written = snprintf(
            wire, sizeof(wire), "{\"%s\":false,%s",
            fixture->selector_name, fixture->wire + 1);
        TEST_ASSERT_GREATER_THAN_INT(0, written);
        TEST_ASSERT_LESS_THAN_UINT(sizeof(wire), (size_t)written);
        assert_registry_rejects(
            fixture->name, fixture->ingress, wire,
            FOF_FW_JSON_REGISTRY_SELECTOR_REJECTED);

        const char *escaped_name =
            strcmp(fixture->selector_name, "cmd") == 0
                ? "c\\u006dd"
                : "t\\u0079pe";
        snprintf(
            field, sizeof(field), "\"%s\":\"%s\"",
            escaped_name, fixture->selector_value);
        replace_first_field(fixture, field, wire, sizeof(wire));
        assert_registry_rejects(
            fixture->name, fixture->ingress, wire,
            FOF_FW_JSON_REGISTRY_SELECTOR_REJECTED);

        snprintf(
            field, sizeof(field), "\"%s\":\"\\u%04x%s\"",
            fixture->selector_name,
            (unsigned)(uint8_t)fixture->selector_value[0],
            fixture->selector_value + 1);
        replace_first_field(fixture, field, wire, sizeof(wire));
        assert_registry_rejects(
            fixture->name, fixture->ingress, wire,
            FOF_FW_JSON_REGISTRY_SELECTOR_REJECTED);

        remove_first_field(fixture, wire, sizeof(wire));
        assert_registry_rejects(
            fixture->name, fixture->ingress, wire,
            FOF_FW_JSON_REGISTRY_SELECTOR_REJECTED);

        for (size_t wrong = 0U; wrong < ARRAY_SIZE(wrong_types); ++wrong) {
            snprintf(
                field, sizeof(field), "\"%s\":%s",
                fixture->selector_name, wrong_types[wrong]);
            replace_first_field(fixture, field, wire, sizeof(wire));
            assert_registry_rejects(
                fixture->name, fixture->ingress, wire,
                FOF_FW_JSON_REGISTRY_SELECTOR_REJECTED);
        }

        snprintf(
            field, sizeof(field), "\"%s\":\"unknown_selector\"",
            fixture->selector_name);
        replace_first_field(fixture, field, wire, sizeof(wire));
        assert_registry_rejects(
            fixture->name, fixture->ingress, wire,
            FOF_FW_JSON_REGISTRY_UNKNOWN_SELECTOR);

        snprintf(
            field, sizeof(field),
            "\"nested\":{\"%s\":\"%s\"}",
            fixture->selector_name, fixture->selector_value);
        replace_first_field(fixture, field, wire, sizeof(wire));
        assert_registry_rejects(
            fixture->name, fixture->ingress, wire,
            FOF_FW_JSON_REGISTRY_SELECTOR_REJECTED);

        const char *alternate =
            strcmp(fixture->selector_name, "cmd") == 0
                ? "type"
                : "cmd";
        snprintf(
            field, sizeof(field), "\"%s\":\"%s\"",
            alternate, fixture->selector_value);
        replace_first_field(fixture, field, wire, sizeof(wire));
        assert_registry_rejects(
            fixture->name, fixture->ingress, wire,
            FOF_FW_JSON_REGISTRY_SELECTOR_REJECTED);

        for (int ingress_raw =
                 FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB;
             ingress_raw <=
                 FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART;
             ++ingress_raw) {
            fof_fw_json_ingress_t other =
                (fof_fw_json_ingress_t)ingress_raw;
            if (other == fixture->ingress) {
                continue;
            }
            fof_fw_json_schema_id_t id =
                FOF_FW_JSON_SCHEMA_USB_FW_RELAY_BASE;
            TEST_ASSERT_NOT_EQUAL_MESSAGE(
                FOF_FW_JSON_REGISTRY_OK,
                fof_fw_json_select_and_validate(
                    other,
                    (const uint8_t *)fixture->wire,
                    strlen(fixture->wire),
                    &id),
                fixture->name);
            TEST_ASSERT_EQUAL_INT_MESSAGE(
                FOF_FW_JSON_SCHEMA_NONE, id, fixture->name);
        }
    }
}

void test_firmware_json_registry_rejects_cross_dialect_partial_and_extra_shapes(
    void)
{
    static const struct {
        const char *label;
        fof_fw_json_ingress_t ingress;
        const char *wire;
        fof_fw_json_registry_result_t expected;
    } cases[] = {
        {
            "relay force only",
            FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
            "{\"cmd\":\"fw_relay\",\"uart\":\"uart1\","
            "\"expected_generation\":1,\"expected_hardware_id\":\"hw\","
            "\"allow_same_version\":false,\"force\":true}",
            FOF_FW_JSON_REGISTRY_NO_EXACT_SCHEMA,
        },
        {
            "relay skip only",
            FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
            "{\"cmd\":\"fw_relay\",\"uart\":\"uart1\","
            "\"expected_generation\":1,\"expected_hardware_id\":\"hw\","
            "\"allow_same_version\":false,\"skip_command_probe\":true}",
            FOF_FW_JSON_REGISTRY_NO_EXACT_SCHEMA,
        },
        {
            "forced relay extra",
            FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
            "{\"cmd\":\"fw_relay\",\"uart\":\"uart1\","
            "\"expected_generation\":1,\"expected_hardware_id\":\"hw\","
            "\"allow_same_version\":false,\"force\":true,"
            "\"skip_command_probe\":true,\"extra\":false}",
            FOF_FW_JSON_REGISTRY_NO_EXACT_SCHEMA,
        },
        {
            "ready legacy plus strict field",
            FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
            "{\"type\":\"fw_ready\",\"board\":\"scanner\",\"ver\":\"1\","
            "\"target_ver\":\"2\",\"size\":2,\"crc\":3,"
            "\"fw_name\":\"scanner\"}",
            FOF_FW_JSON_REGISTRY_NO_EXACT_SCHEMA,
        },
        {
            "ready strict missing manifest field",
            FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
            "{\"type\":\"fw_ready\",\"board\":\"scanner\",\"ver\":\"1\","
            "\"target_ver\":\"2\",\"fw_name\":\"scanner\","
            "\"app_project\":\"fof_scanner\","
            "\"hardware_type\":\"esp32s3\",\"generation\":1,"
            "\"size\":2,\"crc\":3,\"allow_same_version\":false}",
            FOF_FW_JSON_REGISTRY_NO_EXACT_SCHEMA,
        },
        {
            "ack legacy plus modern field",
            FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
            "{\"type\":\"ota_ack\",\"session_id\":\"s\",\"received\":0}",
            FOF_FW_JSON_REGISTRY_NO_EXACT_SCHEMA,
        },
        {
            "ack modern partial manifest",
            FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
            "{\"type\":\"ota_ack\",\"session_id\":\"s\","
            "\"target_ver\":\"2\",\"fw_name\":\"scanner\","
            "\"app_project\":\"fof_scanner\","
            "\"hardware_type\":\"esp32s3\",\"sha256\":\"aa\","
            "\"generation\":1,\"size\":2,\"crc\":3,"
            "\"allow_same_version\":false}",
            FOF_FW_JSON_REGISTRY_NO_EXACT_SCHEMA,
        },
        {
            "done legacy plus modern field",
            FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
            "{\"type\":\"ota_done\",\"session_id\":\"s\","
            "\"received\":2,\"generation\":1}",
            FOF_FW_JSON_REGISTRY_NO_EXACT_SCHEMA,
        },
        {
            "done modern partial manifest",
            FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
            "{\"type\":\"ota_done\",\"session_id\":\"s\","
            "\"target_ver\":\"2\",\"fw_name\":\"scanner\","
            "\"app_project\":\"fof_scanner\","
            "\"hardware_type\":\"esp32s3\",\"sha256\":\"aa\","
            "\"generation\":1,\"size\":2,\"crc\":3,\"received\":2}",
            FOF_FW_JSON_REGISTRY_NO_EXACT_SCHEMA,
        },
        {
            "legacy shaped staged",
            FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
            "{\"type\":\"ota_staged\",\"session_id\":\"s\","
            "\"received\":2}",
            FOF_FW_JSON_REGISTRY_NO_EXACT_SCHEMA,
        },
        {
            "abort extra",
            FOF_FW_JSON_INGRESS_UPLINK_TO_SCANNER_UART,
            "{\"type\":\"ota_abort\",\"session_id\":\"s\","
            "\"reason\":\"bad\"}",
            FOF_FW_JSON_REGISTRY_NO_EXACT_SCHEMA,
        },
        {
            "abort missing session with extra",
            FOF_FW_JSON_INGRESS_UPLINK_TO_SCANNER_UART,
            "{\"type\":\"ota_abort\",\"reason\":\"bad\"}",
            FOF_FW_JSON_REGISTRY_NO_EXACT_SCHEMA,
        },
        {
            "presession error half active",
            FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
            "{\"type\":\"ota_error\",\"session_id\":\"s\","
            "\"reason\":\"bad\"}",
            FOF_FW_JSON_REGISTRY_NO_EXACT_SCHEMA,
        },
        {
            "presession error dialect extra",
            FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
            "{\"type\":\"ota_error\",\"reason\":\"bad\","
            "\"received\":0,\"generation\":1}",
            FOF_FW_JSON_REGISTRY_NO_EXACT_SCHEMA,
        },
    };

    for (size_t i = 0U; i < ARRAY_SIZE(cases); ++i) {
        assert_registry_rejects(
            cases[i].label,
            cases[i].ingress,
            cases[i].wire,
            cases[i].expected);
    }
}

void test_firmware_json_registry_shared_shapes_have_one_id_and_no_value_policy(
    void)
{
    static const struct {
        const char *label;
        fof_fw_json_ingress_t ingress;
        const char *wire;
        fof_fw_json_schema_id_t expected_id;
    } cases[] = {
        {
            "forced booleans need not agree in C4",
            FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
            "{\"cmd\":\"fw_relay\",\"uart\":\"\","
            "\"expected_generation\":0,\"expected_hardware_id\":\"\","
            "\"allow_same_version\":true,\"force\":true,"
            "\"skip_command_probe\":false}",
            FOF_FW_JSON_SCHEMA_USB_FW_RELAY_FORCED,
        },
        {
            "nonempty and canonical values wait for C5",
            FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
            "{\"cmd\":\"fw_upload_begin\","
            "\"name\":\"\",\"target\":\"\",\"project\":\"\","
            "\"hardware_type\":\"\",\"version\":\"\",\"size\":0,"
            "\"crc32\":0,\"sha256\":\"\",\"slot_mask\":0,"
            "\"flow_control\":\"not-credit\"}",
            FOF_FW_JSON_SCHEMA_USB_FW_UPLOAD_BEGIN,
        },
        {
            "strict values wait for transaction policy",
            FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
            "{\"type\":\"fw_ready\",\"board\":\"\",\"ver\":\"\","
            "\"target_ver\":\"\",\"fw_name\":\"\","
            "\"app_project\":\"\",\"hardware_type\":\"\","
            "\"sha256\":\"\",\"generation\":0,\"size\":0,\"crc\":0,"
            "\"allow_same_version\":true}",
            FOF_FW_JSON_SCHEMA_RECEIPT_FW_READY_STRICT,
        },
        {
            "shared active progress",
            FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
            "{\"type\":\"ota_progress\",\"session_id\":\"s\","
            "\"received\":0,\"total\":0,\"percent\":999}",
            FOF_FW_JSON_SCHEMA_RECEIPT_OTA_PROGRESS_ACTIVE_SHARED,
        },
        {
            "shared active nack",
            FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
            "{\"type\":\"ota_nack\",\"session_id\":\"s\",\"seq\":0}",
            FOF_FW_JSON_SCHEMA_RECEIPT_OTA_NACK_ACTIVE_SHARED,
        },
        {
            "shared unbound nack",
            FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
            "{\"type\":\"ota_nack\",\"seq\":0}",
            FOF_FW_JSON_SCHEMA_RECEIPT_OTA_NACK_UNBOUND_SHARED,
        },
        {
            "shared active error",
            FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
            "{\"type\":\"ota_error\",\"session_id\":\"s\","
            "\"reason\":\"bad\",\"received\":0}",
            FOF_FW_JSON_SCHEMA_RECEIPT_OTA_ERROR_ACTIVE_SHARED,
        },
        {
            "legacy presession structural selection",
            FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
            "{\"type\":\"ota_error\",\"reason\":\"bad\",\"received\":0}",
            FOF_FW_JSON_SCHEMA_RECEIPT_OTA_ERROR_PRESESSION_LEGACY_68,
        },
        {
            "shared stop ack",
            FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART,
            "{\"type\":\"stop_ack\"}",
            FOF_FW_JSON_SCHEMA_RECEIPT_STOP_ACK_SHARED,
        },
    };

    for (size_t i = 0U; i < ARRAY_SIZE(cases); ++i) {
        fof_fw_json_schema_id_t id = FOF_FW_JSON_SCHEMA_NONE;
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            FOF_FW_JSON_REGISTRY_OK,
            fof_fw_json_select_and_validate(
                cases[i].ingress,
                (const uint8_t *)cases[i].wire,
                strlen(cases[i].wire),
                &id),
            cases[i].label);
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            cases[i].expected_id, id, cases[i].label);
    }
}

void test_firmware_json_registry_resets_output_on_all_failure_classes(void)
{
    static const uint8_t valid[] = "{\"cmd\":\"fw_check\"}";
    static const uint8_t malformed[] = "{\"cmd\":\"fw_check\"";
    fof_fw_json_schema_id_t id = FOF_FW_JSON_SCHEMA_USB_FW_RELAY_BASE;

    TEST_ASSERT_EQUAL_INT(
        FOF_FW_JSON_REGISTRY_INVALID_ARGUMENT,
        fof_fw_json_select_and_validate(
            FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
            NULL, 0U, &id));
    TEST_ASSERT_EQUAL_INT(FOF_FW_JSON_SCHEMA_NONE, id);

    id = FOF_FW_JSON_SCHEMA_USB_FW_RELAY_BASE;
    TEST_ASSERT_EQUAL_INT(
        FOF_FW_JSON_REGISTRY_INVALID_ARGUMENT,
        fof_fw_json_select_and_validate(
            (fof_fw_json_ingress_t)99,
            valid, sizeof(valid) - 1U, &id));
    TEST_ASSERT_EQUAL_INT(FOF_FW_JSON_SCHEMA_NONE, id);

    TEST_ASSERT_EQUAL_INT(
        FOF_FW_JSON_REGISTRY_INVALID_ARGUMENT,
        fof_fw_json_select_and_validate(
            FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
            valid, sizeof(valid) - 1U, NULL));

    id = FOF_FW_JSON_SCHEMA_USB_FW_RELAY_BASE;
    TEST_ASSERT_EQUAL_INT(
        FOF_FW_JSON_REGISTRY_SELECTOR_REJECTED,
        fof_fw_json_select_and_validate(
            FOF_FW_JSON_INGRESS_HOST_TO_UPLINK_USB,
            malformed, sizeof(malformed) - 1U, &id));
    TEST_ASSERT_EQUAL_INT(FOF_FW_JSON_SCHEMA_NONE, id);
}
