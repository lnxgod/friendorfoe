#include "unity.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "scanner_uplink_ingress_registry.h"

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))

typedef struct {
    const char *wire;
    fof_scanner_uplink_route_t route;
} uplink_fixture_t;

static fof_scanner_uplink_ingress_result_t validate(
    const char *wire,
    int scanner_slot,
    fof_scanner_uplink_decision_t *decision)
{
    return fof_scanner_uplink_ingress_select_and_validate(
        (const uint8_t *)wire, strlen(wire), scanner_slot, decision);
}

static void assert_rejected(
    const char *wire,
    fof_scanner_uplink_ingress_result_t expected)
{
    fof_scanner_uplink_decision_t decision = {
        .route = (fof_scanner_uplink_route_t)0x5a,
        .firmware_schema_id = (fof_fw_json_schema_id_t)0x5a,
        .ble_schema_id = (fof_ble_inv_ingress_schema_id_t)0x5a,
    };
    TEST_ASSERT_EQUAL_INT(
        expected, validate(wire, 0, &decision));
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_UPLINK_ROUTE_NONE, decision.route);
    TEST_ASSERT_EQUAL_INT(
        FOF_FW_JSON_SCHEMA_NONE, decision.firmware_schema_id);
    TEST_ASSERT_EQUAL_INT(
        FOF_BLE_INV_INGRESS_NONE, decision.ble_schema_id);
}

void test_scanner_uplink_ingress_routes_real_nonfirmware_variants(void)
{
    static const uplink_fixture_t fixtures[] = {
        {
            "{\"type\":\"detection\",\"source\":1}",
            FOF_SCANNER_UPLINK_ROUTE_DETECTION,
        },
        {
            "{\"type\":\"status\",\"ble_count\":1}",
            FOF_SCANNER_UPLINK_ROUTE_STATUS,
        },
        {
            "{\"type\":\"scanner_info\",\"ver\":\"1\"}",
            FOF_SCANNER_UPLINK_ROUTE_SCANNER_INFO,
        },
        {
            "{\"type\":\"cal_mode_ack\",\"ok\":true,"
            "\"session_id\":\"abc\",\"scan_mode\":\"calibration\","
            "\"calibration_uuid\":\"00112233-4455-6677-8899-AABBCCDDEEFF\"}",
            FOF_SCANNER_UPLINK_ROUTE_CAL_MODE_ACK,
        },
        {
            "{\"type\":\"scan_profile_ack\","
            "\"scan_profile\":\"badge_primary\","
            "\"slot_role\":\"ble_primary\",\"slot_role_ok\":true}",
            FOF_SCANNER_UPLINK_ROUTE_SCAN_PROFILE_ACK,
        },
        {
            "{\"type\":\"scan_profile_ack\","
            "\"scan_profile\":\"hybrid_failover\"}",
            FOF_SCANNER_UPLINK_ROUTE_SCAN_PROFILE_ACK,
        },
        {
            "{\"type\":\"display_control_ack\","
            "\"button_enabled\":true,\"view\":\"privacy\","
            "\"page_lock\":false,\"page\":2}",
            FOF_SCANNER_UPLINK_ROUTE_DISPLAY_CONTROL_ACK,
        },
        {
            "{\"type\":\"display_policy_ack\",\"hash\":123}",
            FOF_SCANNER_UPLINK_ROUTE_DISPLAY_POLICY_ACK,
        },
        {
            "{\"type\":\"display_policy_ack\",\"ok\":false,"
            "\"hash\":123,\"error\":\"invalid_policy\"}",
            FOF_SCANNER_UPLINK_ROUTE_DISPLAY_POLICY_ACK,
        },
        {
            "{\"type\":\"scanner_quiet_ack\",\"ok\":true,"
            "\"enabled\":true,\"generation\":2,"
            "\"ble_scanning\":false,\"wifi_paused\":true,"
            "\"ble_quiesced\":true,\"wifi_quiesced\":true,"
            "\"ble_active\":false,\"wifi_active\":false,"
            "\"radios_ready\":true,\"tx_restored\":false,"
            "\"tx_enabled\":false,\"uart_commands\":true}",
            FOF_SCANNER_UPLINK_ROUTE_SCANNER_QUIET_ACK,
        },
        {
            "{\"type\":\"scanner_quiet_ack\",\"ok\":false,"
            "\"enabled\":false,\"generation\":2,"
            "\"ble_scanning\":false,\"wifi_paused\":false,"
            "\"ble_quiesced\":false,\"wifi_quiesced\":false,"
            "\"ble_active\":true,\"wifi_active\":true,"
            "\"radios_ready\":false,\"tx_restored\":true,"
            "\"tx_enabled\":true,\"uart_commands\":true,"
            "\"error\":\"radios_not_ready\"}",
            FOF_SCANNER_UPLINK_ROUTE_SCANNER_QUIET_ACK,
        },
        {
            "{\"type\":\"recovery_ack\",\"mode\":\"bootloader\"}",
            FOF_SCANNER_UPLINK_ROUTE_RECOVERY_ACK,
        },
        {
            "{\"type\":\"recovery_ack\",\"mode\":\"safe_uart\","
            "\"reboot\":true}",
            FOF_SCANNER_UPLINK_ROUTE_RECOVERY_ACK,
        },
        {
            "{\"type\":\"recovery_ack\",\"mode\":\"normal\","
            "\"reboot\":true,\"cleared\":true,\"crash_count\":0}",
            FOF_SCANNER_UPLINK_ROUTE_RECOVERY_ACK,
        },
        {
            "{\"type\":\"scanner_recovery\","
            "\"recovery_mode\":\"safe_uart\","
            "\"safe_reason\":\"crash_loop\","
            "\"rollback_pending\":false,\"crash_count\":1,"
            "\"ota_state\":\"idle\"}",
            FOF_SCANNER_UPLINK_ROUTE_SCANNER_RECOVERY,
        },
    };

    for (size_t index = 0U; index < ARRAY_SIZE(fixtures); ++index) {
        fof_scanner_uplink_decision_t decision = {0};
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            FOF_SCANNER_UPLINK_INGRESS_OK,
            validate(fixtures[index].wire, 0, &decision),
            fixtures[index].wire);
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            fixtures[index].route, decision.route,
            fixtures[index].wire);
        TEST_ASSERT_EQUAL_INT(
            FOF_FW_JSON_SCHEMA_NONE, decision.firmware_schema_id);
        TEST_ASSERT_EQUAL_INT(
            FOF_BLE_INV_INGRESS_NONE, decision.ble_schema_id);
    }
}

void test_scanner_uplink_ingress_routes_c4_firmware_without_fallback(void)
{
    static const char valid[] =
        "{\"type\":\"fw_check\",\"board\":\"scanner\",\"ver\":\"1\","
        "\"caps\":\"ota\",\"fw_state\":\"idle\",\"fw_check_count\":1,"
        "\"last_fw_error\":\"\",\"reason\":\"boot\","
        "\"ota_state\":\"idle\",\"recovery_mode\":\"none\","
        "\"rollback_pending\":false,\"crash_count\":0}";
    fof_scanner_uplink_decision_t decision = {0};
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_UPLINK_INGRESS_OK,
        validate(valid, 1, &decision));
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_UPLINK_ROUTE_FIRMWARE, decision.route);
    TEST_ASSERT_EQUAL_INT(
        FOF_FW_JSON_SCHEMA_RECEIPT_FW_CHECK,
        decision.firmware_schema_id);

    assert_rejected(
        "{\"type\":\"fw_check\",\"board\":\"scanner\"}",
        FOF_SCANNER_UPLINK_INGRESS_FIRMWARE_SCHEMA_REJECTED);
    assert_rejected(
        "{\"type\":\"ota_progressive\",\"received\":1}",
        FOF_SCANNER_UPLINK_INGRESS_UNKNOWN_SELECTOR);
}

void test_scanner_uplink_ingress_routes_c9c_ble_only_from_slot_zero(void)
{
    static const char valid[] =
        "{\"type\":\"ble_inv_progress\",\"request_id\":\"req\","
        "\"state\":\"scanning\"}";
    fof_scanner_uplink_decision_t decision = {0};
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_UPLINK_INGRESS_OK,
        validate(valid, 0, &decision));
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_UPLINK_ROUTE_BLE_INVESTIGATION,
        decision.route);
    TEST_ASSERT_EQUAL_INT(
        FOF_BLE_INV_INGRESS_PROGRESS, decision.ble_schema_id);

    decision.route = (fof_scanner_uplink_route_t)0x5a;
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_UPLINK_INGRESS_BLE_SCHEMA_REJECTED,
        validate(valid, 1, &decision));
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_UPLINK_ROUTE_NONE, decision.route);
    assert_rejected(
        "{\"type\":\"ble_inv_progress\",\"request_id\":\"req\"}",
        FOF_SCANNER_UPLINK_INGRESS_BLE_SCHEMA_REJECTED);
    assert_rejected(
        "{\"type\":\"ble_inv_progressive\",\"request_id\":\"req\","
        "\"state\":\"scanning\"}",
        FOF_SCANNER_UPLINK_INGRESS_UNKNOWN_SELECTOR);
}

void test_scanner_uplink_ingress_rejects_ack_shape_corruption(void)
{
    static const char *const rejected[] = {
        "{\"type\":\"cal_mode_ack\",\"ok\":true,"
        "\"session_id\":\"abc\",\"scan_mode\":\"normal\"}",
        "{\"type\":\"scan_profile_ack\","
        "\"scan_profile\":\"badge_primary\","
        "\"slot_role\":\"ble_primary\",\"slot_role_ok\":1}",
        "{\"type\":\"display_control_ack\","
        "\"button_enabled\":true,\"view\":\"privacy\","
        "\"page_lock\":false,\"page\":2,\"extra\":true}",
        "{\"type\":\"display_policy_ack\",\"ok\":true,\"hash\":123}",
        "{\"type\":\"scanner_quiet_ack\",\"ok\":true,"
        "\"enabled\":true,\"generation\":2}",
        "{\"type\":\"recovery_ack\",\"mode\":\"normal\","
        "\"cleared\":true}",
        "{\"type\":\"scanner_recovery\","
        "\"recovery_mode\":\"safe_uart\","
        "\"safe_reason\":\"crash_loop\","
        "\"rollback_pending\":false,\"crash_count\":1}",
    };
    for (size_t index = 0U; index < ARRAY_SIZE(rejected); ++index) {
        assert_rejected(
            rejected[index],
            FOF_SCANNER_UPLINK_INGRESS_TELEMETRY_SCHEMA_REJECTED);
    }
}

void test_scanner_uplink_ingress_routes_exact_crud_self_ack_from_slot_zero(void)
{
    static const char valid[] =
        "{\"type\":\"crud_self_ack\",\"v\":1,\"round\":34,"
        "\"peer\":\"A1B2C3\",\"session\":\"07\"}";
    fof_scanner_uplink_decision_t decision = {0};

    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_UPLINK_INGRESS_OK,
        validate(valid, 0, &decision));
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_UPLINK_ROUTE_CRUD_SELF_ACK, decision.route);
    TEST_ASSERT_EQUAL_HEX32(0xA1B2C3U, decision.crud_peer);
    TEST_ASSERT_EQUAL_HEX8(0x07U, decision.crud_session);

    decision = (fof_scanner_uplink_decision_t) {
        .route = (fof_scanner_uplink_route_t)0x5a,
        .crud_peer = 0x5a5a5aU,
        .crud_session = 0x5aU,
    };
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_UPLINK_INGRESS_TELEMETRY_SCHEMA_REJECTED,
        validate(valid, 1, &decision));
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_UPLINK_ROUTE_NONE, decision.route);
    TEST_ASSERT_EQUAL_HEX32(0U, decision.crud_peer);
    TEST_ASSERT_EQUAL_HEX8(0U, decision.crud_session);
}

void test_scanner_uplink_ingress_rejects_malformed_crud_self_ack_atomically(void)
{
    static const char *const malformed[] = {
        "{\"type\":\"crud_self_ack\",\"v\":1,\"round\":34,"
        "\"peer\":\"A1B2C3\"}",
        "{\"type\":\"crud_self_ack\",\"v\":1,\"round\":34,"
        "\"peer\":\"A1B2C3\",\"session\":\"07\",\"extra\":true}",
        "{\"type\":\"crud_self_ack\",\"v\":2,\"round\":34,"
        "\"peer\":\"A1B2C3\",\"session\":\"07\"}",
        "{\"type\":\"crud_self_ack\",\"v\":1,\"round\":33,"
        "\"peer\":\"A1B2C3\",\"session\":\"07\"}",
        "{\"type\":\"crud_self_ack\",\"v\":1,\"round\":34,"
        "\"peer\":\"a1B2C3\",\"session\":\"07\"}",
        "{\"type\":\"crud_self_ack\",\"v\":1,\"round\":34,"
        "\"peer\":\"000000\",\"session\":\"07\"}",
        "{\"type\":\"crud_self_ack\",\"v\":1,\"round\":34,"
        "\"peer\":\"A1B2C3\",\"session\":\"00\"}",
    };

    for (size_t index = 0U; index < ARRAY_SIZE(malformed); ++index) {
        assert_rejected(
            malformed[index],
            FOF_SCANNER_UPLINK_INGRESS_TELEMETRY_SCHEMA_REJECTED);
    }
    assert_rejected(
        "{\"type\":\"crud_self_ack\",\"type\":\"crud_self_ack\","
        "\"v\":1,\"round\":34,\"peer\":\"A1B2C3\","
        "\"session\":\"07\"}",
        FOF_SCANNER_UPLINK_INGRESS_SELECTOR_REJECTED);
}

void test_scanner_uplink_ingress_requires_exact_easter_fastpath(void)
{
    assert_rejected(
        "{\"type\":\"badge_easter_egg\",\"source\":\"wifi_ssid\"}",
        FOF_SCANNER_UPLINK_INGRESS_EASTER_FRAME_REQUIRED);
    assert_rejected(
        "{\"type\":\"badge_easter_egg\",\"source\":\"wifi_ssid\","
        "\"extra\":true}",
        FOF_SCANNER_UPLINK_INGRESS_EASTER_FRAME_REQUIRED);
}

void test_scanner_uplink_ingress_rejects_selector_corruption(void)
{
    static const char *const rejected[] = {
        "{\"type\":\"status\",\"type\":\"detection\"}",
        "{\"t\\u0079pe\":\"status\"}",
        "{\"type\":\"sta\\u0074us\"}",
        "{\"type\":1}",
        "{\"status\":true}",
        "{\"type\":\"status\"} true",
        "{\"type\":\"uplink_command\"}",
    };
    for (size_t index = 0U; index < ARRAY_SIZE(rejected); ++index) {
        assert_rejected(
            rejected[index],
            index == ARRAY_SIZE(rejected) - 1U
                ? FOF_SCANNER_UPLINK_INGRESS_UNKNOWN_SELECTOR
                : FOF_SCANNER_UPLINK_INGRESS_SELECTOR_REJECTED);
    }

    static const uint8_t embedded_nul[] = {
        '{', '"', 't', 'y', 'p', 'e', '"', ':', '"',
        's', 't', 'a', 't', 'u', 's', '"', '}', '\0',
        '{', '"', 't', 'y', 'p', 'e', '"', ':', '"',
        'd', 'e', 't', 'e', 'c', 't', 'i', 'o', 'n', '"', '}',
    };
    fof_scanner_uplink_decision_t decision = {
        .route = (fof_scanner_uplink_route_t)0x5a,
    };
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_UPLINK_INGRESS_SELECTOR_REJECTED,
        fof_scanner_uplink_ingress_select_and_validate(
            embedded_nul, sizeof(embedded_nul), 0, &decision));
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_UPLINK_ROUTE_NONE, decision.route);
}

void test_scanner_uplink_ingress_rejects_invalid_arguments_atomically(void)
{
    fof_scanner_uplink_decision_t decision = {
        .route = (fof_scanner_uplink_route_t)0x5a,
        .firmware_schema_id = (fof_fw_json_schema_id_t)0x5a,
        .ble_schema_id = (fof_ble_inv_ingress_schema_id_t)0x5a,
    };
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_UPLINK_INGRESS_INVALID_ARGUMENT,
        fof_scanner_uplink_ingress_select_and_validate(
            NULL, 0U, 0, &decision));
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_UPLINK_ROUTE_NONE, decision.route);
    TEST_ASSERT_EQUAL_INT(
        FOF_FW_JSON_SCHEMA_NONE, decision.firmware_schema_id);
    TEST_ASSERT_EQUAL_INT(
        FOF_BLE_INV_INGRESS_NONE, decision.ble_schema_id);
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_UPLINK_INGRESS_INVALID_ARGUMENT,
        fof_scanner_uplink_ingress_select_and_validate(
            (const uint8_t *)"{}", 2U, 0, NULL));
}
