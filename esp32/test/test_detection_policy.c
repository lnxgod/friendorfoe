#include "unity.h"

#include "ble_fingerprint.h"
#include "detection_policy.h"
#include "detection_types.h"
#include "wifi_oui_database.h"

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

void test_fof_drone_ssids_are_notable_but_ambient_fof_is_not(void)
{
    TEST_ASSERT_TRUE(fof_policy_ssid_is_notable("FoF Drone"));
    TEST_ASSERT_TRUE(fof_policy_ssid_is_notable("FriendOrFoe Drone Test"));
    TEST_ASSERT_EQUAL_STRING("Drone SSID",
                             fof_policy_notable_ssid_label("FriendOrFoe Drone Test"));
    TEST_ASSERT_FALSE(fof_policy_ssid_is_notable("FoF Badge"));
    TEST_ASSERT_FALSE(fof_policy_ssid_is_notable("TeamCharityCase"));
}

void test_wifi_oui_database_includes_flock_safety(void)
{
    const uint8_t flock_oui[3] = {0xB4, 0x1E, 0x52};
    const oui_entry_t *entry = wifi_oui_lookup_raw(flock_oui);

    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_EQUAL_STRING("Flock Safety", entry->manufacturer);
    TEST_ASSERT_FALSE(entry->high_false_positive);
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
        DETECTION_SRC_WIFI_AP_INVENTORY, "", NULL, 0, 40, 100));
    TEST_ASSERT_TRUE(fof_policy_should_shed_low_priority(
        DETECTION_SRC_WIFI_PROBE_REQUEST, "", NULL, 0, 60, 100));
    TEST_ASSERT_FALSE(fof_policy_should_shed_low_priority(
        DETECTION_SRC_BLE_FINGERPRINT, "Drone Controller", NULL, 0, 70, 100));
    TEST_ASSERT_TRUE(fof_policy_should_shed_low_priority(
        DETECTION_SRC_BLE_FINGERPRINT, "Apple Device", NULL, 0, 70, 100));
    TEST_ASSERT_TRUE(fof_policy_should_shed_low_priority(
        DETECTION_SRC_WIFI_ASSOC, "WiFi-Assoc", NULL, 0, 80, 100));
}

void test_ble_remote_id_is_never_shed_under_queue_pressure(void)
{
    TEST_ASSERT_FALSE(fof_policy_should_drop_low_value(
        DETECTION_SRC_BLE_RID,
        0.60f,
        "OpenDroneID",
        NULL,
        0
    ));
    TEST_ASSERT_FALSE(fof_policy_should_shed_low_priority(
        DETECTION_SRC_BLE_RID,
        "OpenDroneID",
        NULL,
        0,
        100,
        100
    ));
}

void test_ap_inventory_dedupe_key_uses_bssid(void)
{
    drone_detection_t det = {0};
    char key[128];

    det.source = DETECTION_SRC_WIFI_AP_INVENTORY;
    strncpy(det.bssid, "00:11:22:33:44:55", sizeof(det.bssid) - 1);

    TEST_ASSERT_TRUE(fof_policy_detection_identity_key(
        &det, key, sizeof(key)));
    TEST_ASSERT_EQUAL_STRING("WIFI:00:11:22:33:44:55", key);
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

void test_dedupe_key_groups_probe_ie_hash_across_rotated_macs(void)
{
    drone_detection_t a = {0};
    drone_detection_t b = {0};
    char key_a[128];
    char key_b[128];

    a.source = DETECTION_SRC_WIFI_PROBE_REQUEST;
    b.source = DETECTION_SRC_WIFI_PROBE_REQUEST;
    a.probe_ie_hash = 0xAABBCCDD;
    b.probe_ie_hash = 0xAABBCCDD;
    strncpy(a.bssid, "AA:AA:AA:AA:AA:AA", sizeof(a.bssid) - 1);
    strncpy(b.bssid, "BB:BB:BB:BB:BB:BB", sizeof(b.bssid) - 1);

    TEST_ASSERT_TRUE(fof_policy_detection_dedupe_key(
        &a, 1700000000100LL, 500, key_a, sizeof(key_a)));
    TEST_ASSERT_TRUE(fof_policy_detection_dedupe_key(
        &b, 1700000000200LL, 500, key_b, sizeof(key_b)));
    TEST_ASSERT_EQUAL_STRING(key_a, key_b);
}

void test_dedupe_key_changes_across_time_bucket(void)
{
    drone_detection_t det = {0};
    char key_a[128];
    char key_b[128];

    det.source = DETECTION_SRC_BLE_FINGERPRINT;
    strncpy(det.ble_svc_uuids_raw,
            "cafe9a86-0000-1000-8000-a21607f068aa",
            sizeof(det.ble_svc_uuids_raw) - 1);

    TEST_ASSERT_TRUE(fof_policy_detection_dedupe_key(
        &det, 1700000000100LL, 500, key_a, sizeof(key_a)));
    TEST_ASSERT_TRUE(fof_policy_detection_dedupe_key(
        &det, 1700000000700LL, 500, key_b, sizeof(key_b)));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(key_a, key_b));
}

void test_ble_fingerprint_dedupe_keeps_mac_in_identity(void)
{
    drone_detection_t a = {0};
    drone_detection_t b = {0};
    drone_detection_t c = {0};
    char key_a[128];
    char key_b[128];
    char key_c[128];

    a.source = DETECTION_SRC_BLE_FINGERPRINT;
    b.source = DETECTION_SRC_BLE_FINGERPRINT;
    c.source = DETECTION_SRC_BLE_FINGERPRINT;
    strncpy(a.bssid, "AA:AA:AA:AA:AA:AA", sizeof(a.bssid) - 1);
    strncpy(b.bssid, "BB:BB:BB:BB:BB:BB", sizeof(b.bssid) - 1);
    strncpy(c.bssid, "AA:AA:AA:AA:AA:AA", sizeof(c.bssid) - 1);
    strncpy(a.model, "FP:12345678", sizeof(a.model) - 1);
    strncpy(b.model, "FP:12345678", sizeof(b.model) - 1);
    strncpy(c.model, "FP:12345678", sizeof(c.model) - 1);

    TEST_ASSERT_TRUE(fof_policy_detection_dedupe_key(
        &a, 1700000000100LL, 500, key_a, sizeof(key_a)));
    TEST_ASSERT_TRUE(fof_policy_detection_dedupe_key(
        &b, 1700000000100LL, 500, key_b, sizeof(key_b)));
    TEST_ASSERT_TRUE(fof_policy_detection_dedupe_key(
        &c, 1700000000200LL, 500, key_c, sizeof(key_c)));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(key_a, key_b));
    TEST_ASSERT_EQUAL_STRING(key_a, key_c);
}

void test_ble_fingerprint_meta_name_is_case_insensitive(void)
{
    static const uint8_t adv[] = {
        2, 0x01, 0x06,
        13, 0x09, 'r', 'a', 'y', '-', 'b', 'a', 'n', ' ', 'm', 'e', 't', 'a'
    };
    ble_fingerprint_t fp;

    ble_fingerprint_compute(adv, sizeof(adv), 1, 0, &fp);

    TEST_ASSERT_EQUAL(BLE_DEV_META_GLASSES, fp.device_type);
    TEST_ASSERT_EQUAL_STRING("Meta Glasses", fp.type_name);
    TEST_ASSERT_EQUAL_STRING("name:meta_glasses", fp.class_reason);
}

void test_ble_fingerprint_meta_rayban_uuid_is_human_evidence(void)
{
    static const uint8_t adv[] = {
        3, 0x03, 0x5F, 0xFD
    };
    ble_fingerprint_t fp;

    ble_fingerprint_compute(adv, sizeof(adv), 1, 0, &fp);

    TEST_ASSERT_EQUAL(BLE_DEV_META_GLASSES, fp.device_type);
    TEST_ASSERT_EQUAL_STRING("uuid16:0xFD5F", fp.class_reason);
    TEST_ASSERT_EQUAL_UINT16(0xFD5F, fp.service_uuids[0]);
}

void test_ble_fingerprint_meta_service_uuid_keeps_exact_reason(void)
{
    static const uint8_t adv[] = {
        4, 0x16, 0xB7, 0xFE, 0x00
    };
    ble_fingerprint_t fp;

    ble_fingerprint_compute(adv, sizeof(adv), 1, 0, &fp);

    TEST_ASSERT_EQUAL(BLE_DEV_META_GLASSES, fp.device_type);
    TEST_ASSERT_EQUAL_STRING("uuid16:0xFEB7", fp.class_reason);
    TEST_ASSERT_EQUAL_UINT16(0xFEB7, fp.service_uuids[0]);
}

void test_ble_fingerprint_meta_feb8_is_high_recall_glasses(void)
{
    static const uint8_t adv[] = {
        3, 0x03, 0xB8, 0xFE
    };
    ble_fingerprint_t fp;

    ble_fingerprint_compute(adv, sizeof(adv), 1, 0, &fp);

    TEST_ASSERT_EQUAL(BLE_DEV_META_GLASSES, fp.device_type);
    TEST_ASSERT_EQUAL_STRING("uuid16:0xFEB8", fp.class_reason);
}

void test_ble_fingerprint_luxottica_cid_is_meta_glasses(void)
{
    static const uint8_t adv[] = {
        5, 0xFF, 0x53, 0x0D, 0x01, 0x02
    };
    ble_fingerprint_t fp;

    ble_fingerprint_compute(adv, sizeof(adv), 1, 0, &fp);

    TEST_ASSERT_EQUAL(BLE_DEV_META_GLASSES, fp.device_type);
    TEST_ASSERT_EQUAL_STRING("mfr_cid:0x0D53", fp.class_reason);
}

void test_hidden_camera_ble_is_priority_not_low_value(void)
{
    TEST_ASSERT_TRUE(fof_policy_is_priority_ble_fingerprint("Hidden Camera (suspect)"));
    TEST_ASSERT_FALSE(fof_policy_should_drop_low_value(
        DETECTION_SRC_BLE_FINGERPRINT,
        0.02f,
        "Hidden Camera (suspect)",
        NULL,
        0
    ));
}

void test_priority_ble_fingerprint_is_not_shed_under_pressure(void)
{
    TEST_ASSERT_TRUE(fof_policy_should_shed_low_priority(
        DETECTION_SRC_BLE_FINGERPRINT,
        "Generic BLE",
        NULL,
        0,
        70,
        100
    ));
    TEST_ASSERT_FALSE(fof_policy_should_shed_low_priority(
        DETECTION_SRC_BLE_FINGERPRINT,
        "Meta Glasses",
        NULL,
        0,
        100,
        100
    ));
}

void test_priority_ble_fingerprint_uses_short_reemit_window(void)
{
    uint32_t generic_ms = fof_policy_ble_fingerprint_reemit_ms("Generic BLE");
    uint32_t meta_ms = fof_policy_ble_fingerprint_reemit_ms("Meta Glasses");

    TEST_ASSERT_TRUE(meta_ms < generic_ms);
    TEST_ASSERT_TRUE(meta_ms <= 5000U);
}

void test_scan_profiles_assign_slot_roles_and_calibration_override(void)
{
    TEST_ASSERT_EQUAL_STRING("ble_primary", fof_policy_slot_role_for_slot(0));
    TEST_ASSERT_EQUAL_STRING("wifi_primary", fof_policy_slot_role_for_slot(1));
    TEST_ASSERT_EQUAL_STRING("ble_primary", fof_policy_scan_profile_for_slot(0, false));
    TEST_ASSERT_EQUAL_STRING("wifi_primary", fof_policy_scan_profile_for_slot(1, false));
    TEST_ASSERT_EQUAL_STRING("calibration", fof_policy_scan_profile_for_slot(0, true));
    TEST_ASSERT_EQUAL_STRING("calibration", fof_policy_scan_profile_for_slot(1, true));
}

void test_scan_profile_source_gates_normal_lanes(void)
{
    TEST_ASSERT_TRUE(fof_policy_scan_profile_allows_source(
        "ble_primary", DETECTION_SRC_BLE_FINGERPRINT));
    TEST_ASSERT_TRUE(fof_policy_scan_profile_allows_source(
        "ble_primary", DETECTION_SRC_BLE_RID));
    TEST_ASSERT_FALSE(fof_policy_scan_profile_allows_source(
        "ble_primary", DETECTION_SRC_WIFI_PROBE_REQUEST));
    TEST_ASSERT_TRUE(fof_policy_scan_profile_allows_source(
        "wifi_primary", DETECTION_SRC_WIFI_AP_INVENTORY));
    TEST_ASSERT_TRUE(fof_policy_scan_profile_allows_source(
        "wifi_primary", DETECTION_SRC_WIFI_SSID));
    TEST_ASSERT_TRUE(fof_policy_scan_profile_allows_source(
        "wifi_primary", DETECTION_SRC_WIFI_BEACON));
    TEST_ASSERT_FALSE(fof_policy_scan_profile_allows_source(
        "wifi_primary", DETECTION_SRC_BLE_FINGERPRINT));
    TEST_ASSERT_FALSE(fof_policy_scan_profile_allows_source(
        "wifi_primary", DETECTION_SRC_BLE_RID));
    TEST_ASSERT_TRUE(fof_policy_scan_profile_allows_source(
        "hybrid_failover", DETECTION_SRC_BLE_FINGERPRINT));
    TEST_ASSERT_TRUE(fof_policy_scan_profile_allows_source(
        "hybrid_failover", DETECTION_SRC_WIFI_PROBE_REQUEST));
    TEST_ASSERT_TRUE(fof_policy_scan_profile_allows_source(
        "calibration", DETECTION_SRC_BLE_FINGERPRINT));
    TEST_ASSERT_FALSE(fof_policy_scan_profile_allows_source(
        "calibration", DETECTION_SRC_WIFI_AP_INVENTORY));
}

void test_ble_meta_reacquire_triggers_when_stale_and_advancing(void)
{
    TEST_ASSERT_FALSE(fof_policy_ble_meta_should_reacquire(
        true, true, 29, 8, false, false));
    TEST_ASSERT_TRUE(fof_policy_ble_meta_should_reacquire(
        true, true, 30, 1, false, false));
}

void test_ble_meta_reacquire_blocks_calibration_or_ota(void)
{
    TEST_ASSERT_FALSE(fof_policy_ble_meta_should_reacquire(
        true, true, 60, 4, true, false));
    TEST_ASSERT_FALSE(fof_policy_ble_meta_should_reacquire(
        true, true, 60, 4, false, true));
}

void test_ble_meta_reacquire_requires_scan_sync_and_adv_delta(void)
{
    TEST_ASSERT_FALSE(fof_policy_ble_meta_should_reacquire(
        false, true, 60, 4, false, false));
    TEST_ASSERT_FALSE(fof_policy_ble_meta_should_reacquire(
        true, false, 60, 4, false, false));
    TEST_ASSERT_FALSE(fof_policy_ble_meta_should_reacquire(
        true, true, 60, 0, false, false));
}
