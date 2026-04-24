#include "unity.h"

#include "detection_policy.h"
#include "detection_types.h"

#include <string.h>

void test_probe_broadcasts_still_drop(void)
{
    TEST_ASSERT_TRUE(fof_policy_probe_should_ignore_broadcast(""));
    TEST_ASSERT_FALSE(fof_policy_probe_should_ignore_broadcast("DroneNet"));
}

void test_hard_probe_matches_keep_elevated_confidence(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.50f, fof_policy_probe_confidence(true));
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.05f, fof_policy_probe_confidence(false));
}

void test_generic_targeted_probes_are_not_low_value_dropped(void)
{
    TEST_ASSERT_FALSE(fof_policy_should_drop_low_value(
        DETECTION_SRC_WIFI_PROBE_REQUEST,
        0.05f,
        "Unknown",
        NULL,
        0
    ));
}

void test_probe_rate_aux_changes_when_identity_changes(void)
{
    char aux_a[16];
    char aux_b[16];
    char aux_c[16];

    fof_policy_probe_rate_aux(0xAABBCCDD, "DJI-1234", aux_a, sizeof(aux_a));
    fof_policy_probe_rate_aux(0xAABBCCDD, "DJI-1234", aux_b, sizeof(aux_b));
    fof_policy_probe_rate_aux(0x11223344, "DJI-1234", aux_c, sizeof(aux_c));

    TEST_ASSERT_EQUAL_STRING(aux_a, aux_b);
    TEST_ASSERT_NOT_EQUAL(0, strcmp(aux_a, aux_c));
}

void test_queue_shedding_prefers_diagnostic_sources_first(void)
{
    TEST_ASSERT_TRUE(fof_policy_should_shed_low_priority(
        DETECTION_SRC_WIFI_PROBE_REQUEST, "", NULL, 0, 60, 100));
    TEST_ASSERT_FALSE(fof_policy_should_shed_low_priority(
        DETECTION_SRC_BLE_FINGERPRINT, "Drone Controller", NULL, 0, 70, 100));
    TEST_ASSERT_TRUE(fof_policy_should_shed_low_priority(
        DETECTION_SRC_BLE_FINGERPRINT, "Apple Device", NULL, 0, 70, 100));
    TEST_ASSERT_TRUE(fof_policy_should_shed_low_priority(
        DETECTION_SRC_WIFI_ASSOC, "WiFi-Assoc", NULL, 0, 80, 100));
}

void test_calibration_ble_uuid_is_recognized_and_kept(void)
{
    static const uint8_t calibration_uuid[1][16] = {
        { 0xAA, 0x68, 0xF0, 0x07, 0x16, 0xA2, 0x00, 0x80,
          0x00, 0x10, 0x00, 0x00, 0x86, 0x9A, 0xFE, 0xCA }
    };

    TEST_ASSERT_TRUE(fof_policy_ble_uuid128_is_calibration_le(calibration_uuid[0]));
    TEST_ASSERT_TRUE(fof_policy_ble_has_calibration_uuid_le(calibration_uuid, 1));
    TEST_ASSERT_FALSE(fof_policy_should_drop_low_value(
        DETECTION_SRC_BLE_FINGERPRINT,
        0.02f,
        "Unknown",
        calibration_uuid,
        1
    ));
    TEST_ASSERT_FALSE(fof_policy_should_shed_low_priority(
        DETECTION_SRC_BLE_FINGERPRINT,
        "Unknown",
        calibration_uuid,
        1,
        95,
        100
    ));
    TEST_ASSERT_TRUE(fof_policy_ble_uuid128_matches_token_le(
        calibration_uuid[0],
        "cafe9a86-0000-1000-8000-a21607f068aa"
    ));
    TEST_ASSERT_TRUE(fof_policy_ble_has_exact_uuid128_le(
        calibration_uuid,
        1,
        "CAFE9A86-0000-1000-8000-A21607F068AA"
    ));
    TEST_ASSERT_FALSE(fof_policy_ble_has_exact_uuid128_le(
        calibration_uuid,
        1,
        "cafe1111-0000-1000-8000-a21607f068aa"
    ));
    TEST_ASSERT_TRUE(fof_policy_ble_svc_raw_contains_uuid(
        "180f,cafe9a86-0000-1000-8000-a21607f068aa,abcd",
        "CAFE9A86-0000-1000-8000-A21607F068AA"
    ));
    TEST_ASSERT_FALSE(fof_policy_ble_svc_raw_contains_uuid(
        "180f,cafe9a86-0000-1000-8000-a21607f068aa,abcd",
        "cafe1111-0000-1000-8000-a21607f068aa"
    ));
}
