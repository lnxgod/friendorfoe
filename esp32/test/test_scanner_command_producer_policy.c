#include "unity.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "scanner_command_producer_policy.h"
#include "scanner_command_schema_registry.h"

static void assert_produced_command(
    const char *json,
    fof_scanner_command_id_t expected_id)
{
    fof_scanner_command_decision_t decision;
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_OK,
        fof_scanner_command_select_and_validate(
            (const uint8_t *)json,
            strlen(json),
            FOF_SCANNER_DEPLOYMENT_BADGE,
            &decision));
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_ROUTE_NON_FIRMWARE, decision.route);
    TEST_ASSERT_EQUAL_INT(expected_id, decision.command.id);
}

static void assert_rejected_output_is_cleared(
    bool accepted,
    const char *output)
{
    TEST_ASSERT_FALSE(accepted);
    TEST_ASSERT_EQUAL_STRING("", output);
}

void test_scanner_command_producer_normalizes_backend_lockon_frames(void)
{
    char command[FOF_SCANNER_PRODUCER_JSON_CAPACITY] = {0};

    TEST_ASSERT_TRUE(fof_scanner_wifi_lockon_command_json(
        13, 90, "aa-bb-cc-dd-ee-ff", command, sizeof(command)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"lockon\",\"ch\":13,\"dur\":90,"
        "\"bssid\":\"AA:BB:CC:DD:EE:FF\"}",
        command);
    assert_produced_command(command, FOF_SCANNER_COMMAND_WIFI_LOCKON);

    TEST_ASSERT_TRUE(fof_scanner_wifi_lockon_command_json(
        1, 30, "", command, sizeof(command)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"lockon\",\"ch\":1,\"dur\":30,\"bssid\":\"\"}",
        command);
    assert_produced_command(command, FOF_SCANNER_COMMAND_WIFI_LOCKON);

    TEST_ASSERT_TRUE(fof_scanner_ble_lockon_command_json(
        "aabb.ccdd.eeff", 45, command, sizeof(command)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"ble_lockon\",\"mac\":\"AA:BB:CC:DD:EE:FF\","
        "\"dur\":45}",
        command);
    assert_produced_command(command, FOF_SCANNER_COMMAND_BLE_LOCKON);
}

void test_scanner_command_producer_rejects_invalid_lockon_without_output(void)
{
    char command[FOF_SCANNER_PRODUCER_JSON_CAPACITY] = "stale";

    assert_rejected_output_is_cleared(
        fof_scanner_wifi_lockon_command_json(
            0, 45, "AA:BB:CC:DD:EE:FF", command, sizeof(command)),
        command);
    memcpy(command, "stale", sizeof("stale"));
    assert_rejected_output_is_cleared(
        fof_scanner_wifi_lockon_command_json(
            14, 45, "AA:BB:CC:DD:EE:FF", command, sizeof(command)),
        command);
    memcpy(command, "stale", sizeof("stale"));
    assert_rejected_output_is_cleared(
        fof_scanner_wifi_lockon_command_json(
            6, 29, "AA:BB:CC:DD:EE:FF", command, sizeof(command)),
        command);
    memcpy(command, "stale", sizeof("stale"));
    assert_rejected_output_is_cleared(
        fof_scanner_wifi_lockon_command_json(
            6, 46, "AA:BB:CC:DD:EE:FF", command, sizeof(command)),
        command);
    memcpy(command, "stale", sizeof("stale"));
    assert_rejected_output_is_cleared(
        fof_scanner_wifi_lockon_command_json(
            6, 45, "AA:BB:CC:DD:EE:F\"", command, sizeof(command)),
        command);
    memcpy(command, "stale", sizeof("stale"));
    assert_rejected_output_is_cleared(
        fof_scanner_wifi_lockon_command_json(
            6, 45, NULL, command, sizeof(command)),
        command);

    memcpy(command, "stale", sizeof("stale"));
    assert_rejected_output_is_cleared(
        fof_scanner_ble_lockon_command_json(
            "AA:BB:CC:DD:EE:FF", 44, command, sizeof(command)),
        command);
    memcpy(command, "stale", sizeof("stale"));
    assert_rejected_output_is_cleared(
        fof_scanner_ble_lockon_command_json(
            "", 45, command, sizeof(command)),
        command);
    memcpy(command, "stale", sizeof("stale"));
    assert_rejected_output_is_cleared(
        fof_scanner_ble_lockon_command_json(
            NULL, 45, command, sizeof(command)),
        command);

    char short_command[8] = "stale";
    assert_rejected_output_is_cleared(
        fof_scanner_wifi_lockon_command_json(
            6, 45, "", short_command, sizeof(short_command)),
        short_command);
}

void test_scanner_command_producer_builds_correlated_calibration_frames(void)
{
    static const char session[] = "a1b2c3d4e5f6";
    static const char uuid[] =
        "cafea1b2-0000-1000-8000-a1b2c3d4e5f6";
    char command[FOF_SCANNER_PRODUCER_JSON_CAPACITY] = {0};

    TEST_ASSERT_TRUE(fof_scanner_calibration_start_command_json(
        session, uuid, command, sizeof(command)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"cal_mode_start\",\"session_id\":\"a1b2c3d4e5f6\","
        "\"calibration_uuid\":\"cafea1b2-0000-1000-8000-"
        "a1b2c3d4e5f6\"}",
        command);
    assert_produced_command(command, FOF_SCANNER_COMMAND_CAL_MODE_START);

    TEST_ASSERT_TRUE(fof_scanner_calibration_stop_command_json(
        session, command, sizeof(command)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"cal_mode_stop\",\"session_id\":\"a1b2c3d4e5f6\"}",
        command);
    assert_produced_command(command, FOF_SCANNER_COMMAND_CAL_MODE_STOP);

    TEST_ASSERT_TRUE(fof_scanner_calibration_stop_command_json(
        "stale", command, sizeof(command)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"cal_mode_stop\",\"session_id\":\"stale\"}",
        command);
    assert_produced_command(command, FOF_SCANNER_COMMAND_CAL_MODE_STOP);
}

void test_scanner_command_producer_rejects_uncorrelated_calibration_identity(void)
{
    static const char valid_uuid[] =
        "cafea1b2-0000-1000-8000-a1b2c3d4e5f6";
    char command[FOF_SCANNER_PRODUCER_JSON_CAPACITY] = "stale";

    assert_rejected_output_is_cleared(
        fof_scanner_calibration_start_command_json(
            "A1b2c3d4e5f6", valid_uuid, command, sizeof(command)),
        command);
    memcpy(command, "stale", sizeof("stale"));
    assert_rejected_output_is_cleared(
        fof_scanner_calibration_start_command_json(
            "a1", valid_uuid, command, sizeof(command)),
        command);
    memcpy(command, "stale", sizeof("stale"));
    assert_rejected_output_is_cleared(
        fof_scanner_calibration_start_command_json(
            "a1b2c3d4e5f6", "cafe", command, sizeof(command)),
        command);
    memcpy(command, "stale", sizeof("stale"));
    assert_rejected_output_is_cleared(
        fof_scanner_calibration_start_command_json(
            "a1b2c3d4e5f6",
            "cafefeed-0000-1000-8000-a1b2c3d4e5f6",
            command,
            sizeof(command)),
        command);
    memcpy(command, "stale", sizeof("stale"));
    assert_rejected_output_is_cleared(
        fof_scanner_calibration_start_command_json(
            "a1b2c3d4e5f\"",
            valid_uuid,
            command,
            sizeof(command)),
        command);
    memcpy(command, "stale", sizeof("stale"));
    assert_rejected_output_is_cleared(
        fof_scanner_calibration_start_command_json(
            NULL, valid_uuid, command, sizeof(command)),
        command);
    memcpy(command, "stale", sizeof("stale"));
    assert_rejected_output_is_cleared(
        fof_scanner_calibration_start_command_json(
            "a1b2c3d4e5f6", NULL, command, sizeof(command)),
        command);
    memcpy(command, "stale", sizeof("stale"));
    assert_rejected_output_is_cleared(
        fof_scanner_calibration_stop_command_json(
            "a1b2c3d4e5f\"", command, sizeof(command)),
        command);
}

void test_scanner_command_producer_builds_only_closed_display_shapes(void)
{
    char command[FOF_SCANNER_PRODUCER_JSON_CAPACITY] = {0};

    TEST_ASSERT_TRUE(fof_scanner_display_full_command_json(
        true, "privacy", INT32_MIN, false, true,
        command, sizeof(command)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"display_control\",\"button_enabled\":true,"
        "\"view\":\"privacy\",\"page\":-2147483648,"
        "\"page_lock\":false,\"auto_page\":true}",
        command);
    assert_produced_command(
        command, FOF_SCANNER_COMMAND_DISPLAY_CONTROL_FULL);

    TEST_ASSERT_TRUE(fof_scanner_display_button_command_json(
        false, command, sizeof(command)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"display_control\",\"button_enabled\":false}",
        command);
    assert_produced_command(
        command, FOF_SCANNER_COMMAND_DISPLAY_CONTROL_BUTTON);

    memcpy(command, "stale", sizeof("stale"));
    assert_rejected_output_is_cleared(
        fof_scanner_display_full_command_json(
            true, "unknown", 0, false, true,
            command, sizeof(command)),
        command);
    memcpy(command, "stale", sizeof("stale"));
    assert_rejected_output_is_cleared(
        fof_scanner_display_full_command_json(
            true, NULL, 0, false, true,
            command, sizeof(command)),
        command);
}
