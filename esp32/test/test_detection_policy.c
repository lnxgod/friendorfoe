#include "unity.h"

#include "ble_fingerprint.h"
#include "detection_policy.h"
#include "detection_types.h"
#include "privacy_rf_signatures.h"
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
    const uint8_t flock_field_oui[3] = {0x14, 0x5A, 0xFC};
    const uint8_t flock_wildcard_oui[3] = {0x82, 0x6B, 0xF2};
    const oui_entry_t *entry = wifi_oui_lookup_raw(flock_oui);
    const oui_entry_t *field = wifi_oui_lookup_raw(flock_field_oui);
    const oui_entry_t *wildcard = wifi_oui_lookup_raw(flock_wildcard_oui);

    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_EQUAL_STRING("Flock Safety", entry->manufacturer);
    TEST_ASSERT_FALSE(entry->high_false_positive);
    if (field) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("Flock Safety", field->manufacturer));
    }
    if (wildcard) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("Flock Safety", wildcard->manufacturer));
    }
}

void test_wifi_oui_database_normalizes_flock_safety_mac_formats(void)
{
    const char *valid_bssids[] = {
        " b4:1e:52:aa:bb:cc ",
        "B4-1E-52-AA-BB-CC",
        "B41E52AABBCC",
        "B41E.52AA.BBCC",
    };
    const char *invalid_bssids[] = {
        "XB4:1E:52:AA:BB:CC",
        "B4:1E:5Z:AA:BB:CC",
    };

    for (size_t i = 0; i < sizeof(valid_bssids) / sizeof(valid_bssids[0]); ++i) {
        const oui_entry_t *entry = wifi_oui_lookup(valid_bssids[i]);
        TEST_ASSERT_NOT_NULL(entry);
        TEST_ASSERT_EQUAL_STRING("Flock Safety", entry->manufacturer);
    }

    for (size_t i = 0; i < sizeof(invalid_bssids) / sizeof(invalid_bssids[0]); ++i) {
        TEST_ASSERT_NULL(wifi_oui_lookup(invalid_bssids[i]));
    }
}

void test_unverified_flock_ssid_patterns_are_not_notable_for_badge(void)
{
    TEST_ASSERT_FALSE(fof_policy_ssid_is_notable("Flock-Field-Bridge"));
    TEST_ASSERT_FALSE(fof_policy_ssid_is_notable("FlockOS-Field-Bridge"));
    TEST_ASSERT_FALSE(fof_policy_ssid_is_notable("FLK-Field"));
    TEST_ASSERT_FALSE(fof_policy_ssid_is_notable("Penguin-1234567890"));
    TEST_ASSERT_FALSE(fof_policy_ssid_is_notable("ALPR-maint"));
    TEST_ASSERT_FALSE(fof_policy_ssid_is_notable("FlockGuest"));
    TEST_ASSERT_FALSE(fof_policy_ssid_is_notable("ALPRmaint"));
    TEST_ASSERT_FALSE(fof_policy_ssid_is_notable("1234567890"));
    TEST_ASSERT_FALSE(fof_policy_ssid_is_notable("123456789"));
}

void test_notable_ssid_camera_token_avoids_campus_false_positive(void)
{
    TEST_ASSERT_FALSE(fof_policy_ssid_is_notable("Campus-WiFi"));
    TEST_ASSERT_FALSE(fof_policy_ssid_is_notable("CambridgeGuest"));
    TEST_ASSERT_TRUE(fof_policy_ssid_is_notable("Lobby-Cam"));
    TEST_ASSERT_EQUAL_STRING("Camera SSID",
                             fof_policy_notable_ssid_label("Lobby-Cam"));
}

void test_notable_ssid_attack_tool_labels(void)
{
    TEST_ASSERT_TRUE(fof_policy_ssid_is_notable("pwned"));
    TEST_ASSERT_EQUAL_STRING("Deauther",
                             fof_policy_notable_ssid_label("pwned"));
    TEST_ASSERT_TRUE(fof_policy_ssid_is_notable("Advanced-Deauther"));
    TEST_ASSERT_EQUAL_STRING("Deauther",
                             fof_policy_notable_ssid_label("Advanced-Deauther"));
}

void test_privacy_wifi_signature_catalog_matches_key_ssids(void)
{
    const fof_privacy_wifi_signature_t *tapo =
        fof_privacy_match_wifi_ssid("Tapo_Cam_ABCD");
    TEST_ASSERT_NOT_NULL(tapo);
    TEST_ASSERT_EQUAL_STRING("TP-Link", tapo->manufacturer);
    TEST_ASSERT_EQUAL_STRING("privacy:camera:tapo", tapo->class_reason);
    TEST_ASSERT_FALSE(tapo->attack_tool);

    const fof_privacy_wifi_signature_t *ring =
        fof_privacy_match_wifi_ssid("Ring Setup 12");
    TEST_ASSERT_NOT_NULL(ring);
    TEST_ASSERT_EQUAL_STRING("Ring", ring->manufacturer);
    TEST_ASSERT_EQUAL_STRING("privacy:doorbell:ring", ring->class_reason);

    const fof_privacy_wifi_signature_t *tool =
        fof_privacy_match_wifi_ssid("Advanced-Deauther");
    TEST_ASSERT_NOT_NULL(tool);
    TEST_ASSERT_TRUE(tool->attack_tool);
    TEST_ASSERT_EQUAL_STRING("attack_tool:deauther", tool->class_reason);

    const fof_privacy_wifi_signature_t *elsag =
        fof_privacy_match_wifi_ssid("ELSAG-Field-Bridge");
    TEST_ASSERT_NOT_NULL(elsag);
    TEST_ASSERT_EQUAL_STRING("Leonardo", elsag->manufacturer);
    TEST_ASSERT_EQUAL_STRING("privacy:alpr:elsag", elsag->class_reason);
}

void test_privacy_wifi_signature_catalog_rejects_broad_patterns(void)
{
    TEST_ASSERT_NULL(fof_privacy_match_wifi_ssid("HolyCowGuest"));
    TEST_ASSERT_NULL(fof_privacy_match_wifi_ssid("UFO-Arcade"));
    TEST_ASSERT_NULL(fof_privacy_match_wifi_ssid("Campus-WiFi"));
    TEST_ASSERT_NULL(fof_privacy_match_wifi_ssid("MVP Guest"));
    TEST_ASSERT_NULL(fof_privacy_match_wifi_ssid("FlockGuest"));
    TEST_ASSERT_NULL(fof_privacy_match_wifi_ssid("Flock-Field-Bridge"));
    TEST_ASSERT_NULL(fof_privacy_match_wifi_ssid("FlockOS-Field-Bridge"));
    TEST_ASSERT_NULL(fof_privacy_match_wifi_ssid("FLK-Field"));
    TEST_ASSERT_NULL(fof_privacy_match_wifi_ssid("Penguin-1234567890"));
    TEST_ASSERT_NULL(fof_privacy_match_wifi_ssid("ALPR-maint"));
    TEST_ASSERT_NULL(fof_privacy_match_wifi_ssid("ALPRmaint"));

    size_t count = 0;
    const fof_privacy_wifi_signature_t *signatures =
        fof_privacy_wifi_signatures(&count);
    TEST_ASSERT_NOT_NULL(signatures);
    TEST_ASSERT_TRUE(count > 0);
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_NOT_NULL(signatures[i].pattern);
        TEST_ASSERT_NOT_NULL(signatures[i].manufacturer);
        TEST_ASSERT_NOT_NULL(signatures[i].device_type);
        TEST_ASSERT_NOT_NULL(signatures[i].privacy_kind);
        TEST_ASSERT_NOT_NULL(signatures[i].class_reason);
        TEST_ASSERT_FALSE_MESSAGE(
            fof_privacy_pattern_is_banned_broad(signatures[i].pattern),
            signatures[i].pattern
        );
        TEST_ASSERT_TRUE(signatures[i].confidence >= 0.70f);
    }
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

static size_t test_make_beacon(uint8_t *frame,
                               size_t frame_len,
                               uint16_t capabilities,
                               const uint8_t *ies,
                               size_t ies_len)
{
    memset(frame, 0, frame_len);
    frame[0] = 0x80;
    frame[34] = (uint8_t)(capabilities & 0xFFu);
    frame[35] = (uint8_t)(capabilities >> 8);
    if (ies && ies_len > 0) {
        memcpy(&frame[36], ies, ies_len);
    }
    return 36u + ies_len;
}

void test_wifi_beacon_auth_mode_extracts_open_wpa_and_rsn(void)
{
    uint8_t frame[64];
    const uint8_t wpa_ie[] = {221, 4, 0x00, 0x50, 0xF2, 0x01};
    const uint8_t rsn_ie[] = {48, 2, 0x01, 0x00};

    size_t len = test_make_beacon(frame, sizeof(frame), 0, NULL, 0);
    TEST_ASSERT_EQUAL_UINT8(0, fof_policy_wifi_beacon_auth_mode(frame, len));

    len = test_make_beacon(frame, sizeof(frame), 0x0010u, NULL, 0);
    TEST_ASSERT_EQUAL_UINT8(1, fof_policy_wifi_beacon_auth_mode(frame, len));

    len = test_make_beacon(frame, sizeof(frame), 0x0010u,
                           wpa_ie, sizeof(wpa_ie));
    TEST_ASSERT_EQUAL_UINT8(2, fof_policy_wifi_beacon_auth_mode(frame, len));

    len = test_make_beacon(frame, sizeof(frame), 0x0010u,
                           rsn_ie, sizeof(rsn_ie));
    TEST_ASSERT_EQUAL_UINT8(3, fof_policy_wifi_beacon_auth_mode(frame, len));
    TEST_ASSERT_EQUAL_UINT8(0xFF, fof_policy_wifi_beacon_auth_mode(NULL, 0));
}

void test_evil_twin_open_clone_same_ssid_alerts_once(void)
{
    fof_policy_evil_twin_state_t state;
    fof_policy_evil_twin_alert_t alert;
    const uint8_t secured[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    const uint8_t open[6] = {0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB};

    fof_policy_evil_twin_state_init(&state);
    TEST_ASSERT_FALSE(fof_policy_evil_twin_observe(
        &state, "CafeWiFi", secured, -52, 6, 3, 1000, &alert));
    TEST_ASSERT_TRUE(fof_policy_evil_twin_observe(
        &state, "CafeWiFi", open, -49, 6, 0, 2000, &alert));
    TEST_ASSERT_EQUAL_STRING("CafeWiFi", alert.ssid);
    TEST_ASSERT_TRUE(alert.mixed_open);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(open, alert.suspect_bssid, 6);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(secured, alert.reference_bssid, 6);
    TEST_ASSERT_EQUAL_STRING("open clone vs WPA2", alert.detail);

    TEST_ASSERT_FALSE(fof_policy_evil_twin_observe(
        &state, "CafeWiFi", open, -48, 6, 0, 3000, &alert));
}

void test_evil_twin_same_oui_secured_mesh_does_not_alert(void)
{
    fof_policy_evil_twin_state_t state;
    fof_policy_evil_twin_alert_t alert;
    const uint8_t ap1[6] = {0x00, 0x11, 0x22, 0x01, 0x02, 0x03};
    const uint8_t ap2[6] = {0x00, 0x11, 0x22, 0x04, 0x05, 0x06};

    fof_policy_evil_twin_state_init(&state);
    TEST_ASSERT_FALSE(fof_policy_evil_twin_observe(
        &state, "HomeMesh", ap1, -42, 1, 3, 1000, &alert));
    TEST_ASSERT_FALSE(fof_policy_evil_twin_observe(
        &state, "HomeMesh", ap2, -45, 6, 3, 2000, &alert));
}

void test_evil_twin_hidden_and_stale_observations_do_not_alert(void)
{
    fof_policy_evil_twin_state_t state;
    fof_policy_evil_twin_alert_t alert;
    const uint8_t secured[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    const uint8_t open[6] = {0x70, 0x80, 0x90, 0xA0, 0xB0, 0xC0};

    fof_policy_evil_twin_state_init(&state);
    TEST_ASSERT_FALSE(fof_policy_evil_twin_observe(
        &state, "", secured, -50, 1, 3, 1000, &alert));
    TEST_ASSERT_FALSE(fof_policy_evil_twin_observe(
        &state, "Lobby", secured, -50, 1, 3, 1000, &alert));
    TEST_ASSERT_FALSE(fof_policy_evil_twin_observe(
        &state, "Lobby", open, -48, 1, 0, 62001, &alert));
}

void test_evil_twin_accepts_full_length_ssid_without_overread(void)
{
    fof_policy_evil_twin_state_t state;
    fof_policy_evil_twin_alert_t alert;
    char ssid[FOF_POLICY_EVIL_TWIN_MAX_SSID_LEN];
    const uint8_t secured[6] = {0x12, 0x34, 0x56, 0x10, 0x20, 0x30};
    const uint8_t open[6] = {0x98, 0x76, 0x54, 0x40, 0x50, 0x60};

    memset(ssid, 'A', 32);
    ssid[32] = '\0';
    fof_policy_evil_twin_state_init(&state);
    TEST_ASSERT_FALSE(fof_policy_evil_twin_observe(
        &state, ssid, secured, -54, 6, 3, 1000, &alert));
    TEST_ASSERT_TRUE(fof_policy_evil_twin_observe(
        &state, ssid, open, -50, 6, 0, 2000, &alert));
    TEST_ASSERT_EQUAL_STRING(ssid, alert.ssid);
}

void test_evil_twin_strong_different_oui_security_clone_alerts(void)
{
    fof_policy_evil_twin_state_t state;
    fof_policy_evil_twin_alert_t alert;
    const uint8_t ap1[6] = {0x00, 0x11, 0x22, 0x01, 0x02, 0x03};
    const uint8_t ap2[6] = {0x66, 0x77, 0x88, 0x04, 0x05, 0x06};

    fof_policy_evil_twin_state_init(&state);
    TEST_ASSERT_FALSE(fof_policy_evil_twin_observe(
        &state, "CorpSecure", ap1, -56, 11, 3, 1000, &alert));
    TEST_ASSERT_TRUE(fof_policy_evil_twin_observe(
        &state, "CorpSecure", ap2, -54, 11, 6, 2000, &alert));
    TEST_ASSERT_TRUE(alert.strong_clone);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ap2, alert.suspect_bssid, 6);
    TEST_ASSERT_EQUAL_STRING("WPA3 clone vs WPA2", alert.detail);
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

void test_ble_fingerprint_meta_service_uuid_keeps_generic_meta_reason(void)
{
    static const uint8_t adv[] = {
        4, 0x16, 0xB7, 0xFE, 0x00
    };
    ble_fingerprint_t fp;

    ble_fingerprint_compute(adv, sizeof(adv), 1, 0, &fp);

    TEST_ASSERT_EQUAL(BLE_DEV_META_DEVICE, fp.device_type);
    TEST_ASSERT_EQUAL_STRING("Meta Device", fp.type_name);
    TEST_ASSERT_EQUAL_STRING("uuid16:0xFEB7", fp.class_reason);
    TEST_ASSERT_EQUAL_UINT16(0xFEB7, fp.service_uuids[0]);
}

void test_ble_fingerprint_meta_feb8_is_generic_meta_device(void)
{
    static const uint8_t adv[] = {
        3, 0x03, 0xB8, 0xFE
    };
    ble_fingerprint_t fp;

    ble_fingerprint_compute(adv, sizeof(adv), 1, 0, &fp);

    TEST_ASSERT_EQUAL(BLE_DEV_META_DEVICE, fp.device_type);
    TEST_ASSERT_EQUAL_STRING("Meta Device", fp.type_name);
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

void test_ble_fingerprint_findmy_uuid_is_tracker(void)
{
    static const uint8_t adv[] = {
        3, 0x03, 0x44, 0xFD
    };
    ble_fingerprint_t fp;

    ble_fingerprint_compute(adv, sizeof(adv), 1, 0, &fp);

    TEST_ASSERT_EQUAL(BLE_DEV_APPLE_FINDMY, fp.device_type);
    TEST_ASSERT_TRUE(fp.is_tracker);
    TEST_ASSERT_EQUAL_STRING("uuid16:0xFD44", fp.class_reason);
    TEST_ASSERT_EQUAL_UINT16(0xFD44, fp.service_uuids[0]);
}

void test_ble_fingerprint_exposure_notification_is_not_findmy_tracker(void)
{
    static const uint8_t adv[] = {
        3, 0x03, 0x6F, 0xFD
    };
    ble_fingerprint_t fp;

    ble_fingerprint_compute(adv, sizeof(adv), 1, 0, &fp);

    TEST_ASSERT_EQUAL(BLE_DEV_UNKNOWN, fp.device_type);
    TEST_ASSERT_FALSE(fp.is_tracker);
    TEST_ASSERT_EQUAL_UINT16(0xFD6F, fp.service_uuids[0]);
}

void test_ble_fingerprint_apple_ibeacon_is_venue_beacon(void)
{
    static const uint8_t adv[] = {
        2, 0x01, 0x06,
        26, 0xFF,
        0x4C, 0x00,
        0x02, 0x15,
        0xE2, 0xC5, 0x6D, 0xB5, 0xDF, 0xFB, 0x48, 0xD2,
        0xB0, 0x60, 0xD0, 0xF5, 0xA7, 0x10, 0x96, 0xE0,
        0x12, 0x34,
        0xAB, 0xCD,
        0xC5
    };
    ble_fingerprint_t fp;

    ble_fingerprint_compute(adv, sizeof(adv), 1, 0, &fp);

    TEST_ASSERT_EQUAL_UINT16(0x004C, fp.company_id);
    TEST_ASSERT_EQUAL_UINT8(0x02, fp.apple_type);
    TEST_ASSERT_EQUAL(BLE_DEV_VENUE_BEACON, fp.device_type);
    TEST_ASSERT_EQUAL_STRING("Venue Beacon", fp.type_name);
    TEST_ASSERT_EQUAL_STRING("ibeacon:0x02", fp.class_reason);
}

void test_ble_fingerprint_flock_name_is_not_alpr_evidence(void)
{
    static const uint8_t adv[] = {
        2, 0x01, 0x06,
        13, 0x09, 'F', 'l', 'o', 'c', 'k', ' ', 'C', 'a', 'm', 'e', 'r', 'a'
    };
    ble_fingerprint_t fp;

    ble_fingerprint_compute(adv, sizeof(adv), 1, 0, &fp);

    TEST_ASSERT_NOT_EQUAL(BLE_DEV_FLOCK_SAFETY, fp.device_type);
    TEST_ASSERT_FALSE(fp.is_tracker);
    TEST_ASSERT_NOT_EQUAL(0, strcmp("flock_ble_name", fp.class_reason));
}

void test_ble_fingerprint_chipolo_member_uuid_is_tracker(void)
{
    static const uint8_t adv[] = {
        3, 0x03, 0x33, 0xFE
    };
    ble_fingerprint_t fp;

    ble_fingerprint_compute(adv, sizeof(adv), 1, 0, &fp);

    TEST_ASSERT_EQUAL(BLE_DEV_CHIPOLO, fp.device_type);
    TEST_ASSERT_TRUE(fp.is_tracker);
    TEST_ASSERT_EQUAL_STRING("uuid16:0xFE33", fp.class_reason);
    TEST_ASSERT_EQUAL_UINT16(0xFE33, fp.service_uuids[0]);
}

void test_ble_fingerprint_chipolo_company_id_is_tracker(void)
{
    static const uint8_t adv[] = {
        3, 0xFF, 0xC3, 0x08
    };
    ble_fingerprint_t fp;

    ble_fingerprint_compute(adv, sizeof(adv), 1, 0, &fp);

    TEST_ASSERT_EQUAL(BLE_DEV_CHIPOLO, fp.device_type);
    TEST_ASSERT_TRUE(fp.is_tracker);
}

void test_ble_fingerprint_nordic_company_id_is_not_tile_tracker(void)
{
    static const uint8_t adv[] = {
        3, 0xFF, 0x59, 0x00
    };
    ble_fingerprint_t fp;

    ble_fingerprint_compute(adv, sizeof(adv), 1, 0, &fp);

    TEST_ASSERT_EQUAL(BLE_DEV_UNKNOWN, fp.device_type);
    TEST_ASSERT_FALSE(fp.is_tracker);
}

void test_ble_fingerprint_unikey_company_id_is_not_pebblebee_tracker(void)
{
    static const uint8_t adv[] = {
        3, 0xFF, 0x5E, 0x01
    };
    ble_fingerprint_t fp;

    ble_fingerprint_compute(adv, sizeof(adv), 1, 0, &fp);

    TEST_ASSERT_EQUAL(BLE_DEV_UNKNOWN, fp.device_type);
    TEST_ASSERT_FALSE(fp.is_tracker);
}

void test_ble_fingerprint_serial_uuids_are_not_static_skimmers(void)
{
    const uint8_t serial_uuids[][4] = {
        {3, 0x16, 0xE0, 0xFF},
        {3, 0x16, 0xF0, 0xFF},
    };

    TEST_ASSERT_EQUAL_INT(25, BLE_DEV_CARD_SKIMMER);
    TEST_ASSERT_EQUAL_INT(38, BLE_DEV_DRONE_OTHER);
    TEST_ASSERT_EQUAL_INT(39, BLE_DEV_PAIRING_SPAM);
    TEST_ASSERT_EQUAL_INT(40, BLE_DEV_SERIAL_SKIMMER);

    for (size_t i = 0; i < sizeof(serial_uuids) / sizeof(serial_uuids[0]); i++) {
        ble_fingerprint_t fp;
        ble_fingerprint_compute(serial_uuids[i], sizeof(serial_uuids[i]),
                                1, 0, &fp);

        TEST_ASSERT_EQUAL(BLE_DEV_UNKNOWN, fp.device_type);
        TEST_ASSERT_NOT_EQUAL(BLE_DEV_SERIAL_SKIMMER, fp.device_type);
    }
}

void test_hidden_camera_ble_is_priority_not_low_value(void)
{
    TEST_ASSERT_TRUE(fof_policy_is_priority_ble_fingerprint("Hidden Camera (suspect)"));
    TEST_ASSERT_FALSE(fof_policy_is_priority_ble_fingerprint("Flock Surveillance"));
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
