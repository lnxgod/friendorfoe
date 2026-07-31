#include "unity.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "scanner_command_schema_registry.h"

#define DEFAULT_POLICY_OBJECT                                                \
    "{\"version\":1,\"classes\":{"                                          \
    "\"drone\":{\"enabled\":true,\"lane\":\"both\","                         \
    "\"min_proximity\":\"present\",\"priority\":100},"                       \
    "\"meta\":{\"enabled\":true,\"lane\":\"both\","                          \
    "\"min_proximity\":\"present\",\"priority\":95},"                        \
    "\"tracker\":{\"enabled\":true,\"lane\":\"lower\","                      \
    "\"min_proximity\":\"near\",\"priority\":70},"                           \
    "\"wifi_attack\":{\"enabled\":true,\"lane\":\"both\","                   \
    "\"min_proximity\":\"present\",\"priority\":90},"                        \
    "\"skimmer\":{\"enabled\":false,\"lane\":\"off\","                       \
    "\"min_proximity\":\"close\",\"priority\":0},"                           \
    "\"camera\":{\"enabled\":true,\"lane\":\"lower\","                       \
    "\"min_proximity\":\"near\",\"priority\":65},"                           \
    "\"flock\":{\"enabled\":true,\"lane\":\"both\","                         \
    "\"min_proximity\":\"present\",\"priority\":85},"                        \
    "\"lock\":{\"enabled\":true,\"lane\":\"lower\","                         \
    "\"min_proximity\":\"near\",\"priority\":55},"                           \
    "\"hid\":{\"enabled\":true,\"lane\":\"lower\","                          \
    "\"min_proximity\":\"close\",\"priority\":45},"                          \
    "\"beacon\":{\"enabled\":true,\"lane\":\"lower\","                       \
    "\"min_proximity\":\"near\",\"priority\":30},"                           \
    "\"event_badge\":{\"enabled\":true,\"lane\":\"lower\","                  \
    "\"min_proximity\":\"near\",\"priority\":35},"                           \
    "\"auracast\":{\"enabled\":true,\"lane\":\"lower\","                     \
    "\"min_proximity\":\"near\",\"priority\":20},"                           \
    "\"scanner_status\":{\"enabled\":true,\"lane\":\"lower\","               \
    "\"min_proximity\":\"present\",\"priority\":10},"                        \
    "\"ble_attack\":{\"enabled\":true,\"lane\":\"both\","                    \
    "\"min_proximity\":\"present\",\"priority\":92}}}"

#define DEFAULT_POLICY_COMMAND                                               \
    "{\"type\":\"display_policy\",\"version\":1,"                            \
    "\"hash\":1262753418,\"policy\":" DEFAULT_POLICY_OBJECT "}"

typedef struct {
    fof_scanner_command_id_t id;
    fof_scanner_deployment_t deployment;
    const char *wire;
} scanner_command_fixture_t;

static const scanner_command_fixture_t COMMAND_FIXTURES[] = {
    {
        FOF_SCANNER_COMMAND_READY,
        FOF_SCANNER_DEPLOYMENT_BADGE,
        "{\"type\":\"ready\"}",
    },
    {
        FOF_SCANNER_COMMAND_START,
        FOF_SCANNER_DEPLOYMENT_BADGE,
        "{\"type\":\"start\"}",
    },
    {
        FOF_SCANNER_COMMAND_STOP,
        FOF_SCANNER_DEPLOYMENT_BADGE,
        "{\"type\":\"stop\"}",
    },
    {
        FOF_SCANNER_COMMAND_SCANNER_QUIET,
        FOF_SCANNER_DEPLOYMENT_BADGE,
        "{\"type\":\"scanner_quiet\",\"enabled\":true,\"generation\":7}",
    },
    {
        FOF_SCANNER_COMMAND_BLE_INVESTIGATE,
        FOF_SCANNER_DEPLOYMENT_BADGE,
        "{\"type\":\"ble_investigate\",\"request_id\":\"req-1\","
        "\"mode\":\"gatt\",\"target\":\"AA:BB:CC:DD:EE:FF\","
        "\"timeout_ms\":12000}",
    },
    {
        FOF_SCANNER_COMMAND_BLE_INVESTIGATE_CANCEL,
        FOF_SCANNER_DEPLOYMENT_BADGE,
        "{\"type\":\"ble_investigate_cancel\",\"request_id\":\"req-1\"}",
    },
    {
        FOF_SCANNER_COMMAND_WIFI_LOCKON,
        FOF_SCANNER_DEPLOYMENT_BADGE,
        "{\"type\":\"lockon\",\"ch\":6,\"dur\":45,\"bssid\":\"\"}",
    },
    {
        FOF_SCANNER_COMMAND_WIFI_LOCKON_CANCEL,
        FOF_SCANNER_DEPLOYMENT_BADGE,
        "{\"type\":\"lockon_cancel\"}",
    },
    {
        FOF_SCANNER_COMMAND_BLE_LOCKON,
        FOF_SCANNER_DEPLOYMENT_BADGE,
        "{\"type\":\"ble_lockon\",\"mac\":\"AA:BB:CC:DD:EE:FF\","
        "\"dur\":45}",
    },
    {
        FOF_SCANNER_COMMAND_BLE_LOCKON_CANCEL,
        FOF_SCANNER_DEPLOYMENT_BADGE,
        "{\"type\":\"ble_lockon_cancel\"}",
    },
    {
        FOF_SCANNER_COMMAND_CAL_MODE_START,
        FOF_SCANNER_DEPLOYMENT_BADGE,
        "{\"type\":\"cal_mode_start\",\"session_id\":\"abcdef123456\","
        "\"calibration_uuid\":\"cafeabcd-0000-1000-8000-abcdef123456\"}",
    },
    {
        FOF_SCANNER_COMMAND_CAL_MODE_STOP,
        FOF_SCANNER_DEPLOYMENT_BADGE,
        "{\"type\":\"cal_mode_stop\",\"session_id\":\"abcdef123456\"}",
    },
    {
        FOF_SCANNER_COMMAND_SCAN_PROFILE,
        FOF_SCANNER_DEPLOYMENT_BADGE,
        "{\"type\":\"scan_profile\",\"scan_profile\":\"ble_primary\","
        "\"slot_role\":\"ble_primary\"}",
    },
    {
        FOF_SCANNER_COMMAND_DISPLAY_CONTROL_FULL,
        FOF_SCANNER_DEPLOYMENT_BADGE,
        "{\"type\":\"display_control\",\"button_enabled\":true,"
        "\"view\":\"privacy\",\"page\":-1,\"page_lock\":false,"
        "\"auto_page\":true}",
    },
    {
        FOF_SCANNER_COMMAND_DISPLAY_CONTROL_BUTTON,
        FOF_SCANNER_DEPLOYMENT_BADGE,
        "{\"type\":\"display_control\",\"button_enabled\":false}",
    },
    {
        FOF_SCANNER_COMMAND_DISPLAY_POLICY,
        FOF_SCANNER_DEPLOYMENT_BADGE,
        DEFAULT_POLICY_COMMAND,
    },
    {
        FOF_SCANNER_COMMAND_TIME,
        FOF_SCANNER_DEPLOYMENT_BADGE,
        "{\"type\":\"time\",\"ms\":1700000000001,\"ok\":true,"
        "\"src\":\"backend\"}",
    },
    {
        FOF_SCANNER_COMMAND_SAFE_MODE,
        FOF_SCANNER_DEPLOYMENT_BADGE,
        "{\"type\":\"safe_mode\",\"enabled\":false}",
    },
    {
        FOF_SCANNER_COMMAND_REBOOT,
        FOF_SCANNER_DEPLOYMENT_BADGE,
        "{\"type\":\"reboot\"}",
    },
    {
        FOF_SCANNER_COMMAND_CRUD_SELF,
        FOF_SCANNER_DEPLOYMENT_BADGE,
        "{\"type\":\"crud_self\",\"v\":1,\"round\":34,"
        "\"peer\":\"A1B2C3\",\"session\":\"07\"}",
    },
};

static fof_scanner_command_registry_result_t select_wire(
    const char *wire,
    fof_scanner_deployment_t deployment,
    fof_scanner_command_decision_t *decision)
{
    return fof_scanner_command_select_and_validate(
        (const uint8_t *)wire, strlen(wire), deployment, decision);
}

static void assert_semantic_rejected(const char *wire,
                                     fof_scanner_deployment_t deployment)
{
    fof_scanner_command_decision_t decision;
    memset(&decision, 0xa5, sizeof(decision));
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_SEMANTIC_REJECTED,
        select_wire(wire, deployment, &decision));
    TEST_ASSERT_EQUAL_INT(FOF_SCANNER_COMMAND_ROUTE_NONE, decision.route);
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_NONE, decision.command.id);
    TEST_ASSERT_EQUAL_INT(
        FOF_FW_JSON_SCHEMA_NONE, decision.firmware_schema_id);
}

void test_scanner_command_registry_accepts_every_exact_nonfirmware_fixture(void)
{
    TEST_ASSERT_EQUAL_UINT(
        FOF_SCANNER_COMMAND_COUNT - 1U,
        sizeof(COMMAND_FIXTURES) / sizeof(COMMAND_FIXTURES[0]));

    for (size_t i = 0U;
         i < sizeof(COMMAND_FIXTURES) / sizeof(COMMAND_FIXTURES[0]);
         ++i) {
        const scanner_command_fixture_t *fixture = &COMMAND_FIXTURES[i];
        fof_scanner_command_decision_t decision;
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            FOF_SCANNER_COMMAND_REGISTRY_OK,
            select_wire(fixture->wire, fixture->deployment, &decision),
            fixture->wire);
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            FOF_SCANNER_COMMAND_ROUTE_NON_FIRMWARE,
            decision.route,
            fixture->wire);
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            fixture->id, decision.command.id, fixture->wire);
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            FOF_FW_JSON_SCHEMA_NONE,
            decision.firmware_schema_id,
            fixture->wire);
    }
}

void test_scanner_command_registry_returns_validated_semantic_payloads(void)
{
    fof_scanner_command_decision_t decision;

    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_OK,
        select_wire(COMMAND_FIXTURES[3].wire,
                    FOF_SCANNER_DEPLOYMENT_BADGE,
                    &decision));
    TEST_ASSERT_TRUE(decision.command.data.scanner_quiet.enabled);
    TEST_ASSERT_EQUAL_UINT32(
        7U, decision.command.data.scanner_quiet.generation);

    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_OK,
        select_wire(COMMAND_FIXTURES[4].wire,
                    FOF_SCANNER_DEPLOYMENT_BADGE,
                    &decision));
    TEST_ASSERT_EQUAL_STRING(
        "req-1", decision.command.data.ble_investigate.request_id);
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_BLE_INVESTIGATION_GATT,
        decision.command.data.ble_investigate.mode);
    TEST_ASSERT_FALSE(
        decision.command.data.ble_investigate.target_is_null);
    TEST_ASSERT_EQUAL_STRING(
        "AA:BB:CC:DD:EE:FF",
        decision.command.data.ble_investigate.target_mac);
    TEST_ASSERT_EQUAL_INT32(
        12000, decision.command.data.ble_investigate.timeout_ms);

    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_OK,
        select_wire(COMMAND_FIXTURES[6].wire,
                    FOF_SCANNER_DEPLOYMENT_BADGE,
                    &decision));
    TEST_ASSERT_EQUAL_INT32(6, decision.command.data.wifi_lockon.channel);
    TEST_ASSERT_EQUAL_INT32(
        45, decision.command.data.wifi_lockon.duration_s);
    TEST_ASSERT_EQUAL_STRING("", decision.command.data.wifi_lockon.bssid);

    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_OK,
        select_wire(COMMAND_FIXTURES[15].wire,
                    FOF_SCANNER_DEPLOYMENT_BADGE,
                    &decision));
    TEST_ASSERT_EQUAL_UINT32(
        1262753418U, decision.command.data.display_policy.hash);
    TEST_ASSERT_EQUAL_UINT8(
        1U, decision.command.data.display_policy.policy.version);
}

void test_scanner_command_registry_routes_c4_firmware_before_nonfirmware(void)
{
    fof_scanner_command_decision_t decision;

    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_OK,
        select_wire("{\"type\":\"fw_check_now\"}",
                    FOF_SCANNER_DEPLOYMENT_BADGE,
                    &decision));
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_ROUTE_FIRMWARE, decision.route);
    TEST_ASSERT_EQUAL_INT(
        FOF_FW_JSON_SCHEMA_SCANNER_FW_CHECK_NOW,
        decision.firmware_schema_id);
    TEST_ASSERT_EQUAL_INT(FOF_SCANNER_COMMAND_NONE, decision.command.id);

    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_FIRMWARE_SCHEMA_REJECTED,
        select_wire("{\"type\":\"fw_check_now\",\"enabled\":true}",
                    FOF_SCANNER_DEPLOYMENT_BADGE,
                    &decision));
    TEST_ASSERT_EQUAL_INT(FOF_SCANNER_COMMAND_ROUTE_NONE, decision.route);

    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_FIRMWARE_SCHEMA_REJECTED,
        select_wire("{\"type\":\"ota_begin\"}",
                    FOF_SCANNER_DEPLOYMENT_BADGE,
                    &decision));
    TEST_ASSERT_EQUAL_INT(FOF_SCANNER_COMMAND_ROUTE_NONE, decision.route);
}

void test_scanner_command_registry_rejects_bad_selectors_without_fallback(void)
{
    static const char *const rejected_selectors[] = {
        "{\"type\":\"ready\",\"type\":\"fw_check_now\"}",
        "{\"type\":\"fw_check_now\",\"type\":\"ready\"}",
        "{\"t\\u0079pe\":\"ready\"}",
        "{\"type\":\"re\\u0061dy\"}",
        "{\"type\":1}",
        "{\"enabled\":true}",
    };

    for (size_t i = 0U;
         i < sizeof(rejected_selectors) / sizeof(rejected_selectors[0]);
         ++i) {
        fof_scanner_command_decision_t decision;
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            FOF_SCANNER_COMMAND_REGISTRY_SELECTOR_REJECTED,
            select_wire(rejected_selectors[i],
                        FOF_SCANNER_DEPLOYMENT_BADGE,
                        &decision),
            rejected_selectors[i]);
        TEST_ASSERT_EQUAL_INT(
            FOF_SCANNER_COMMAND_ROUTE_NONE, decision.route);
    }
}

void test_scanner_command_registry_refuses_routine_rom_mutation_commands(void)
{
    static const char *const refused[] = {
        "{\"type\":\"bootloader\"}",
        "{\"type\":\"ota\"}",
    };

    for (size_t i = 0U; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        fof_scanner_command_decision_t decision;
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            FOF_SCANNER_COMMAND_REGISTRY_MUTATION_REFUSED,
            select_wire(refused[i], FOF_SCANNER_DEPLOYMENT_BADGE, &decision),
            refused[i]);
        TEST_ASSERT_EQUAL_INT(
            FOF_SCANNER_COMMAND_ROUTE_NONE, decision.route);
    }

    fof_scanner_command_decision_t decision;
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_NONFIRMWARE_SCHEMA_REJECTED,
        select_wire("{\"type\":\"bootloader\",\"force\":true}",
                    FOF_SCANNER_DEPLOYMENT_BADGE,
                    &decision));
}

void test_scanner_command_registry_keeps_incoming_easter_frames_separate(void)
{
    fof_scanner_command_decision_t decision;
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_UNKNOWN_SELECTOR,
        select_wire(
            "{\"type\":\"badge_easter_egg\",\"source\":\"wifi_ssid\"}",
            FOF_SCANNER_DEPLOYMENT_BADGE,
            &decision));
    TEST_ASSERT_EQUAL_INT(FOF_SCANNER_COMMAND_ROUTE_NONE, decision.route);
}

void test_scanner_command_registry_enforces_ble_timeout_boundaries(void)
{
    static const int32_t valid_timeouts[] = {1, 2, 7500, 11999, 12000};
    char wire[192];

    for (size_t i = 0U;
         i < sizeof(valid_timeouts) / sizeof(valid_timeouts[0]);
         ++i) {
        snprintf(
            wire, sizeof(wire),
            "{\"type\":\"ble_investigate\",\"request_id\":\"req-1\","
            "\"mode\":\"passive_capture\",\"target\":null,"
            "\"timeout_ms\":%ld}",
            (long)valid_timeouts[i]);
        fof_scanner_command_decision_t decision;
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            FOF_SCANNER_COMMAND_REGISTRY_OK,
            select_wire(wire, FOF_SCANNER_DEPLOYMENT_BADGE, &decision),
            wire);
        TEST_ASSERT_EQUAL_INT32(
            valid_timeouts[i],
            decision.command.data.ble_investigate.timeout_ms);
    }

    static const int32_t invalid_timeouts[] = {-1, 0, 12001};
    for (size_t i = 0U;
         i < sizeof(invalid_timeouts) / sizeof(invalid_timeouts[0]);
         ++i) {
        snprintf(
            wire, sizeof(wire),
            "{\"type\":\"ble_investigate\",\"request_id\":\"req-1\","
            "\"mode\":\"passive_capture\",\"target\":null,"
            "\"timeout_ms\":%ld}",
            (long)invalid_timeouts[i]);
        assert_semantic_rejected(wire, FOF_SCANNER_DEPLOYMENT_BADGE);
    }
}

void test_scanner_command_registry_enforces_ble_request_target_correlation(void)
{
    fof_scanner_command_decision_t decision;
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_OK,
        select_wire(
            "{\"type\":\"ble_investigate\","
            "\"request_id\":\"12345678901234567890123456789012\","
            "\"mode\":\"gatt\",\"target\":\"AA:BB:CC:DD:EE:FF\","
            "\"timeout_ms\":12000}",
            FOF_SCANNER_DEPLOYMENT_BADGE,
            &decision));

    assert_semantic_rejected(
        "{\"type\":\"ble_investigate\","
        "\"request_id\":\"123456789012345678901234567890123\","
        "\"mode\":\"gatt\",\"target\":\"AA:BB:CC:DD:EE:FF\","
        "\"timeout_ms\":12000}",
        FOF_SCANNER_DEPLOYMENT_BADGE);
    assert_semantic_rejected(
        "{\"type\":\"ble_investigate\",\"request_id\":\"req-1\","
        "\"mode\":\"gatt\",\"target\":null,\"timeout_ms\":12000}",
        FOF_SCANNER_DEPLOYMENT_BADGE);
    assert_semantic_rejected(
        "{\"type\":\"ble_investigate\",\"request_id\":\"req-1\","
        "\"mode\":\"gatt\",\"target\":\"aa:bb:cc:dd:ee:ff\","
        "\"timeout_ms\":12000}",
        FOF_SCANNER_DEPLOYMENT_BADGE);
    assert_semantic_rejected(
        "{\"type\":\"ble_investigate\",\"request_id\":\"req-1\","
        "\"mode\":\"passive_capture\",\"target\":\"AA:BB:CC:DD:EE:FF\","
        "\"timeout_ms\":12000}",
        FOF_SCANNER_DEPLOYMENT_BADGE);
}

void test_scanner_command_registry_enforces_lockon_ranges_and_canonical_macs(
    void)
{
    static const char *const valid_wifi[] = {
        "{\"type\":\"lockon\",\"ch\":1,\"dur\":30,\"bssid\":\"\"}",
        ("{\"type\":\"lockon\",\"ch\":13,\"dur\":45,"
         "\"bssid\":\"AA:BB:CC:DD:EE:FF\"}"),
        "{\"type\":\"lockon\",\"ch\":6,\"dur\":60,\"bssid\":\"\"}",
        "{\"type\":\"lockon\",\"ch\":6,\"dur\":90,\"bssid\":\"\"}",
    };
    for (size_t i = 0U;
         i < sizeof(valid_wifi) / sizeof(valid_wifi[0]);
         ++i) {
        fof_scanner_command_decision_t decision;
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            FOF_SCANNER_COMMAND_REGISTRY_OK,
            select_wire(valid_wifi[i],
                        FOF_SCANNER_DEPLOYMENT_BADGE,
                        &decision),
            valid_wifi[i]);
    }

    static const char *const invalid_wifi[] = {
        "{\"type\":\"lockon\",\"ch\":0,\"dur\":60,\"bssid\":\"\"}",
        "{\"type\":\"lockon\",\"ch\":14,\"dur\":60,\"bssid\":\"\"}",
        "{\"type\":\"lockon\",\"ch\":6,\"dur\":29,\"bssid\":\"\"}",
        ("{\"type\":\"lockon\",\"ch\":6,\"dur\":60,"
         "\"bssid\":\"aa:bb:cc:dd:ee:ff\"}"),
    };
    for (size_t i = 0U;
         i < sizeof(invalid_wifi) / sizeof(invalid_wifi[0]);
         ++i) {
        assert_semantic_rejected(
            invalid_wifi[i], FOF_SCANNER_DEPLOYMENT_BADGE);
    }

    assert_semantic_rejected(
        "{\"type\":\"ble_lockon\",\"mac\":\"\",\"dur\":45}",
        FOF_SCANNER_DEPLOYMENT_BADGE);
    assert_semantic_rejected(
        "{\"type\":\"ble_lockon\",\"mac\":\"AA:BB:CC:DD:EE:FF\","
        "\"dur\":44}",
        FOF_SCANNER_DEPLOYMENT_BADGE);
}

void test_scanner_command_registry_enforces_calibration_identity_correlation(
    void)
{
    assert_semantic_rejected(
        "{\"type\":\"cal_mode_start\",\"session_id\":\"ABCDEF123456\","
        "\"calibration_uuid\":\"cafeABCD-0000-1000-8000-ABCDEF123456\"}",
        FOF_SCANNER_DEPLOYMENT_BADGE);
    assert_semantic_rejected(
        "{\"type\":\"cal_mode_start\",\"session_id\":\"abcdef123456\","
        "\"calibration_uuid\":\"cafeabcd-0000-1000-8000-abcdef123457\"}",
        FOF_SCANNER_DEPLOYMENT_BADGE);

    fof_scanner_command_decision_t decision;
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_OK,
        select_wire("{\"type\":\"cal_mode_stop\",\"session_id\":\"stale\"}",
                    FOF_SCANNER_DEPLOYMENT_BADGE,
                    &decision));
    assert_semantic_rejected(
        "{\"type\":\"cal_mode_stop\",\"session_id\":\"not-a-session\"}",
        FOF_SCANNER_DEPLOYMENT_BADGE);
}

void test_scanner_command_registry_enforces_badge_and_nonbadge_profiles(void)
{
    fof_scanner_command_decision_t decision;
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_OK,
        select_wire(
            "{\"type\":\"scan_profile\",\"scan_profile\":\"wifi_primary\","
            "\"slot_role\":\"wifi_primary\"}",
            FOF_SCANNER_DEPLOYMENT_BADGE,
            &decision));
    assert_semantic_rejected(
        "{\"type\":\"scan_profile\",\"scan_profile\":\"wifi_primary\","
        "\"slot_role\":\"ble_primary\"}",
        FOF_SCANNER_DEPLOYMENT_BADGE);
    assert_semantic_rejected(
        "{\"type\":\"scan_profile\",\"scan_profile\":\"hybrid_failover\","
        "\"slot_role\":\"ble_primary\"}",
        FOF_SCANNER_DEPLOYMENT_BADGE);

    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_OK,
        select_wire(
            "{\"type\":\"scan_profile\","
            "\"scan_profile\":\"hybrid_failover\","
            "\"slot_role\":\"ble_primary\"}",
            FOF_SCANNER_DEPLOYMENT_NON_BADGE,
            &decision));
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_NONFIRMWARE_SCHEMA_REJECTED,
        select_wire(
            "{\"type\":\"scan_profile\",\"profile\":\"ble_primary\","
            "\"slot_role\":\"ble_primary\"}",
            FOF_SCANNER_DEPLOYMENT_BADGE,
            &decision));
}

void test_scanner_command_registry_enforces_normalized_display_shapes(void)
{
    static const char *const views[] = {
        "privacy", "prv", "glasses", "rf", "activity", "drone", "wifi",
    };
    char wire[192];
    for (size_t i = 0U; i < sizeof(views) / sizeof(views[0]); ++i) {
        snprintf(
            wire, sizeof(wire),
            "{\"type\":\"display_control\",\"button_enabled\":true,"
            "\"view\":\"%s\",\"page\":-2147483648,\"page_lock\":false,"
            "\"auto_page\":true}",
            views[i]);
        fof_scanner_command_decision_t decision;
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            FOF_SCANNER_COMMAND_REGISTRY_OK,
            select_wire(wire, FOF_SCANNER_DEPLOYMENT_BADGE, &decision),
            wire);
        TEST_ASSERT_EQUAL_INT32(
            INT32_MIN, decision.command.data.display_control.page);
    }

    assert_semantic_rejected(
        "{\"type\":\"display_control\",\"button_enabled\":true,"
        "\"view\":\"menu\",\"page\":0,\"page_lock\":false,"
        "\"auto_page\":true}",
        FOF_SCANNER_DEPLOYMENT_BADGE);

    fof_scanner_command_decision_t decision;
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_NONFIRMWARE_SCHEMA_REJECTED,
        select_wire(
            "{\"type\":\"display_control\",\"button_enabled\":true,"
            "\"view\":\"rf\"}",
            FOF_SCANNER_DEPLOYMENT_BADGE,
            &decision));
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_NONFIRMWARE_SCHEMA_REJECTED,
        select_wire(
            "{\"type\":\"display_control\",\"button_enabled\":1}",
            FOF_SCANNER_DEPLOYMENT_BADGE,
            &decision));
}

void test_scanner_command_registry_enforces_display_policy_hash_and_nested_shape(
    void)
{
    fof_scanner_command_decision_t decision;
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_OK,
        select_wire(DEFAULT_POLICY_COMMAND,
                    FOF_SCANNER_DEPLOYMENT_BADGE,
                    &decision));

    assert_semantic_rejected(
        "{\"type\":\"display_policy\",\"version\":1,"
        "\"hash\":1262753419,\"policy\":" DEFAULT_POLICY_OBJECT "}",
        FOF_SCANNER_DEPLOYMENT_BADGE);
    assert_semantic_rejected(
        "{\"type\":\"display_policy\",\"version\":1,\"hash\":1,"
        "\"policy\":{\"version\":1,\"classes\":{}}}",
        FOF_SCANNER_DEPLOYMENT_BADGE);
    assert_semantic_rejected(
        "{\"type\":\"display_policy\",\"version\":2,"
        "\"hash\":1262753418,\"policy\":" DEFAULT_POLICY_OBJECT "}",
        FOF_SCANNER_DEPLOYMENT_BADGE);
}

void test_scanner_command_registry_enforces_time_value_correlation(void)
{
    fof_scanner_command_decision_t decision;
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_OK,
        select_wire(
            "{\"type\":\"time\",\"ms\":1700000000001,"
            "\"ok\":true,\"src\":\"local\"}",
            FOF_SCANNER_DEPLOYMENT_BADGE,
            &decision));
    TEST_ASSERT_EQUAL_INT64(
        1700000000001LL, decision.command.data.time.epoch_ms);
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_TIME_SOURCE_LOCAL,
        decision.command.data.time.source);

    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_OK,
        select_wire(
            "{\"type\":\"time\",\"ms\":-1,\"ok\":false,\"src\":\"none\"}",
            FOF_SCANNER_DEPLOYMENT_BADGE,
            &decision));
    TEST_ASSERT_FALSE(decision.command.data.time.ok);

    static const char *const invalid[] = {
        "{\"type\":\"time\",\"ms\":1700000000000,"
        "\"ok\":true,\"src\":\"backend\"}",
        "{\"type\":\"time\",\"ms\":1700000000001,"
        "\"ok\":true,\"src\":\"none\"}",
        "{\"type\":\"time\",\"ms\":-1,\"ok\":false,\"src\":\"local\"}",
        "{\"type\":\"time\",\"ms\":0,\"ok\":false,\"src\":\"none\"}",
    };
    for (size_t i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert_semantic_rejected(
            invalid[i], FOF_SCANNER_DEPLOYMENT_BADGE);
    }
}

void test_scanner_command_registry_rejects_structural_corruption_and_raw_nul(
    void)
{
    fof_scanner_command_decision_t decision;
    static const char *const malformed[] = {
        "{\"type\":\"scanner_quiet\",\"enabled\":true}",
        "{\"type\":\"scanner_quiet\",\"enabled\":1,\"generation\":7}",
        "{\"type\":\"ready\",\"extra\":true}",
        "{\"type\":\"ready\"} trailing",
        "{\"type\":\"unknown\"}",
    };
    static const fof_scanner_command_registry_result_t expected[] = {
        FOF_SCANNER_COMMAND_REGISTRY_NONFIRMWARE_SCHEMA_REJECTED,
        FOF_SCANNER_COMMAND_REGISTRY_NONFIRMWARE_SCHEMA_REJECTED,
        FOF_SCANNER_COMMAND_REGISTRY_NONFIRMWARE_SCHEMA_REJECTED,
        FOF_SCANNER_COMMAND_REGISTRY_SELECTOR_REJECTED,
        FOF_SCANNER_COMMAND_REGISTRY_UNKNOWN_SELECTOR,
    };
    for (size_t i = 0U; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            expected[i],
            select_wire(
                malformed[i], FOF_SCANNER_DEPLOYMENT_BADGE, &decision),
            malformed[i]);
        TEST_ASSERT_EQUAL_INT(
            FOF_SCANNER_COMMAND_ROUTE_NONE, decision.route);
    }

    static const uint8_t embedded_nul[] = {
        '{', '"', 't', 'y', 'p', 'e', '"', ':', '"', 'r', 'e', '\0',
        'a', 'd', 'y', '"', '}',
    };
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_SELECTOR_REJECTED,
        fof_scanner_command_select_and_validate(
            embedded_nul,
            sizeof(embedded_nul),
            FOF_SCANNER_DEPLOYMENT_BADGE,
            &decision));
    TEST_ASSERT_EQUAL_INT(FOF_SCANNER_COMMAND_ROUTE_NONE, decision.route);
}

void test_scanner_command_registry_rejects_invalid_api_arguments(void)
{
    fof_scanner_command_decision_t decision;
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_INVALID_ARGUMENT,
        fof_scanner_command_select_and_validate(
            NULL, 1U, FOF_SCANNER_DEPLOYMENT_BADGE, &decision));
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_INVALID_ARGUMENT,
        fof_scanner_command_select_and_validate(
            (const uint8_t *)"{}", 2U,
            (fof_scanner_deployment_t)99,
            &decision));
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_INVALID_ARGUMENT,
        fof_scanner_command_select_and_validate(
            (const uint8_t *)"{}", 2U,
        FOF_SCANNER_DEPLOYMENT_BADGE,
        NULL));
}

void test_scanner_command_registry_validates_exact_crud_self_identity(void)
{
    static const char valid[] =
        "{\"type\":\"crud_self\",\"v\":1,\"round\":34,"
        "\"peer\":\"A1B2C3\",\"session\":\"07\"}";
    fof_scanner_command_decision_t decision;

    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_OK,
        select_wire(valid, FOF_SCANNER_DEPLOYMENT_BADGE, &decision));
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_ROUTE_NON_FIRMWARE, decision.route);
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_CRUD_SELF, decision.command.id);
    TEST_ASSERT_EQUAL_HEX32(
        0xA1B2C3U, decision.command.data.crud_self.peer);
    TEST_ASSERT_EQUAL_HEX8(
        0x07U, decision.command.data.crud_self.session);

    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_OK,
        select_wire(valid, FOF_SCANNER_DEPLOYMENT_NON_BADGE, &decision));
}

void test_scanner_command_registry_rejects_malformed_crud_self_atomically(void)
{
    static const char *const malformed[] = {
        "{\"type\":\"crud_self\",\"v\":1,\"round\":34,"
        "\"peer\":\"A1B2C3\"}",
        "{\"type\":\"crud_self\",\"v\":1,\"round\":34,"
        "\"peer\":\"A1B2C3\",\"session\":\"07\",\"extra\":true}",
        "{\"type\":\"crud_self\",\"type\":\"crud_self\",\"v\":1,"
        "\"round\":34,\"peer\":\"A1B2C3\",\"session\":\"07\"}",
        "{\"type\":\"crud_self\",\"v\":1,\"v\":1,\"round\":34,"
        "\"peer\":\"A1B2C3\",\"session\":\"07\"}",
        "{\"type\":\"crud_self\",\"v\":1,\"round\":34,"
        "\"peer\":\"a1B2C3\",\"session\":\"07\"}",
        "{\"type\":\"crud_self\",\"v\":1,\"round\":34,"
        "\"peer\":\"A1B2C3\",\"session\":\"0a\"}",
        "{\"type\":\"crud_self\",\"v\":1,\"round\":34,"
        "\"peer\":\"000000\",\"session\":\"07\"}",
        "{\"type\":\"crud_self\",\"v\":1,\"round\":34,"
        "\"peer\":\"A1B2C3\",\"session\":\"00\"}",
        "{\"type\":\"crud_self\",\"v\":2,\"round\":34,"
        "\"peer\":\"A1B2C3\",\"session\":\"07\"}",
        "{\"type\":\"crud_self\",\"v\":1,\"round\":33,"
        "\"peer\":\"A1B2C3\",\"session\":\"07\"}",
        "{\"type\":\"crud_self\",\"v\":1,\"round\":34,"
        "\"peer\":\"A1 B2C3\",\"session\":\"07\"}",
        "{\"type\":\"crud_self\",\"v\":1,\"round\":34,"
        "\"peer\":\"A1B2C3\",\"session\":\"0 7\"}",
        "{\"type\":\"crud_self\",\"v\":1,\"round\":34,"
        "\"peer\":\"A1B2C3\\u0000\",\"session\":\"07\"}",
        "{\"type\":\"crud_self\",\"v\":1,\"round\":34,"
        "\"peer\":\"A1B2C3\",\"session\":\"07\"}suffix",
    };

    for (size_t i = 0U; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
        fof_scanner_command_decision_t decision;
        memset(&decision, 0xa5, sizeof(decision));
        TEST_ASSERT_NOT_EQUAL_MESSAGE(
            FOF_SCANNER_COMMAND_REGISTRY_OK,
            select_wire(
                malformed[i], FOF_SCANNER_DEPLOYMENT_BADGE, &decision),
            malformed[i]);
        TEST_ASSERT_EQUAL_INT(
            FOF_SCANNER_COMMAND_ROUTE_NONE, decision.route);
        TEST_ASSERT_EQUAL_INT(
            FOF_SCANNER_COMMAND_NONE, decision.command.id);
    }
}
