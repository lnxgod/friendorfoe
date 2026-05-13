#include "unity.h"

#include "badge_threat_policy.h"
#include "detection_types.h"

#include <string.h>

static drone_detection_t make_detection(uint8_t source,
                                        const char *id,
                                        const char *mfr,
                                        float confidence,
                                        int rssi)
{
    drone_detection_t det = {0};
    det.source = source;
    det.confidence = confidence;
    det.rssi = (int8_t)rssi;
    if (id) {
        strncpy(det.drone_id, id, sizeof(det.drone_id) - 1);
    }
    if (mfr) {
        strncpy(det.manufacturer, mfr, sizeof(det.manufacturer) - 1);
    }
    return det;
}

void test_badge_repeated_drone_stays_single_entity(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t det = make_detection(
        DETECTION_SRC_BLE_RID,
        "RID-1234",
        "OpenDroneID",
        0.95f,
        -58
    );

    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &det, 1000 + i * 100, NULL));
    }

    badge_threat_state_snapshot(&state, 3000, &snapshot);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.active_counts[BADGE_THREAT_DRONE]);
    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_TRUE(snapshot.threat_score >= 70.0f);
}

void test_badge_meta_plus_drone_raises_score(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t meta = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "FP:AAAA",
        "Meta Glasses",
        0.80f,
        -48
    );
    drone_detection_t drone = make_detection(
        DETECTION_SRC_WIFI_DJI_IE,
        "DJI-ONE",
        "DJI",
        0.90f,
        -55
    );

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &meta, 1000, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &drone, 1100, NULL));

    badge_threat_state_snapshot(&state, 1500, &snapshot);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.active_counts[BADGE_THREAT_META]);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.active_counts[BADGE_THREAT_DRONE]);
    TEST_ASSERT_TRUE(snapshot.threat_score >= 90.0f);
}

void test_badge_meta_stays_active_for_live_window(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t meta = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:META:FRAME",
        "Meta Glasses",
        0.80f,
        -46
    );
    strncpy(meta.model, "FP:FACE1234", sizeof(meta.model) - 1);

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &meta, 1000, NULL));
    badge_threat_state_snapshot(&state, 1000 + 25000, &snapshot);

    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_EQUAL(BADGE_THREAT_META, snapshot.entities[0].cls);
    TEST_ASSERT_FALSE(snapshot.entities[0].stale);
    TEST_ASSERT_TRUE(snapshot.entities[0].score >= 70);
}

void test_badge_meta_last_seen_memory_outlives_active_window(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t meta = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:META:FRAME",
        "Ray-Ban Meta Glasses",
        0.80f,
        -46
    );
    strncpy(meta.model, "FP:FACE4321", sizeof(meta.model) - 1);
    strncpy(meta.ble_name, "Ray-Ban Meta", sizeof(meta.ble_name) - 1);

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &meta, 1000, NULL));
    badge_threat_state_snapshot(&state, 1000 + 420000, &snapshot);

    TEST_ASSERT_EQUAL_UINT32(0, snapshot.active_counts[BADGE_THREAT_META]);
    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_EQUAL(BADGE_THREAT_META, snapshot.entities[0].cls);
    TEST_ASSERT_TRUE(snapshot.entities[0].stale);
    TEST_ASSERT_EQUAL_STRING("Meta Glasses", snapshot.entities[0].label);
    TEST_ASSERT_TRUE(snapshot.entities[0].score >= 10);
}

void test_badge_meta_count_drops_after_live_window(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t meta = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:ABCD1234:Meta Glasses",
        "Meta Glasses",
        0.82f,
        -44
    );
    strncpy(meta.model, "FP:ABCD1234", sizeof(meta.model) - 1);

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &meta, 1000, NULL));
    badge_threat_state_snapshot(&state, 1000 + 80000, &snapshot);

    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.active_counts[BADGE_THREAT_META]);
    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_snapshot_meta_glasses_count(&snapshot));
    TEST_ASSERT_FALSE(snapshot.entities[0].stale);

    badge_threat_state_snapshot(&state, 1000 + 90001, &snapshot);

    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.active_counts[BADGE_THREAT_META]);
    TEST_ASSERT_EQUAL_UINT32(0, badge_threat_snapshot_meta_glasses_count(&snapshot));
    TEST_ASSERT_TRUE(snapshot.entities[0].stale);
}

void test_badge_meta_memory_expires_after_fifteen_minutes(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t meta = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:META:FRAME",
        "Meta Glasses",
        0.80f,
        -46
    );

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &meta, 1000, NULL));
    badge_threat_state_snapshot(&state, 1000 + 920000, &snapshot);

    TEST_ASSERT_EQUAL_INT(0, snapshot.entity_count);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.active_counts[BADGE_THREAT_META]);
}

void test_badge_drone_priority_beats_close_glasses(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t meta = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:META:FACE",
        "Meta Glasses",
        0.60f,
        -42
    );
    drone_detection_t drone = make_detection(
        DETECTION_SRC_WIFI_DJI_IE,
        "DJI-LOUD",
        "DJI",
        0.95f,
        -70
    );

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &drone, 1000, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &meta, 1100, NULL));
    badge_threat_state_snapshot(&state, 1600, &snapshot);

    TEST_ASSERT_EQUAL(BADGE_THREAT_DRONE, snapshot.entities[0].cls);
    TEST_ASSERT_EQUAL(BADGE_THREAT_CATEGORY_DRONE, snapshot.entities[0].category);
    TEST_ASSERT_EQUAL(BADGE_THREAT_META, snapshot.entities[1].cls);
    TEST_ASSERT_EQUAL(BADGE_THREAT_CATEGORY_GLASS, snapshot.entities[1].category);
    TEST_ASSERT_EQUAL(BADGE_THREAT_PROX_CLOSE, snapshot.entities[1].proximity_level);
    TEST_ASSERT_TRUE(snapshot.threat_score >= 90.0f);
}

void test_badge_close_airtag_drives_privacy_alert(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t tag = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:TAG:NEAR",
        "AirTag",
        0.40f,
        -43
    );

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &tag, 1000, NULL));
    badge_threat_state_snapshot(&state, 1200, &snapshot);

    TEST_ASSERT_EQUAL(BADGE_THREAT_TRACKER, snapshot.entities[0].cls);
    TEST_ASSERT_EQUAL(BADGE_THREAT_CATEGORY_TAG_CLOSE, snapshot.entities[0].category);
    TEST_ASSERT_EQUAL(BADGE_THREAT_PROX_CLOSE, snapshot.entities[0].proximity_level);
    TEST_ASSERT_TRUE(snapshot.entities[0].score >= 35);
}

void test_badge_close_airtag_survives_when_generic_wifi_noise_is_ignored(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t wifi = make_detection(
        DETECTION_SRC_WIFI_ASSOC,
        "STA:AA:BB:CC:DD:EE:FF",
        "WiFi-Assoc",
        0.10f,
        -42
    );
    drone_detection_t tag = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:TAG:CLOSE",
        "AirTag",
        0.40f,
        -43
    );

    TEST_ASSERT_FALSE(badge_threat_state_ingest(&state, &wifi, 1000, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &tag, 1100, NULL));
    badge_threat_state_snapshot(&state, 1500, &snapshot);

    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_EQUAL(BADGE_THREAT_TRACKER, snapshot.entities[0].cls);
    TEST_ASSERT_EQUAL_STRING("AirTag", snapshot.entities[0].label);
    TEST_ASSERT_EQUAL(BADGE_THREAT_PROX_CLOSE, snapshot.entities[0].proximity_level);
}

void test_badge_repeated_meta_groups_seen_count_and_best_rssi(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t meta = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:META:ROTATE1",
        "Meta Glasses",
        0.60f,
        -64
    );
    strncpy(meta.ble_name, "Ray-Ban Meta", sizeof(meta.ble_name) - 1);

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &meta, 1000, NULL));
    meta.rssi = -41;
    strncpy(meta.drone_id, "BLE:META:ROTATE2", sizeof(meta.drone_id) - 1);
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &meta, 1500, NULL));
    badge_threat_state_snapshot(&state, 2000, &snapshot);

    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_EQUAL_UINT32(2, snapshot.entities[0].seen_count);
    TEST_ASSERT_EQUAL_INT(-41, snapshot.entities[0].best_rssi);
    TEST_ASSERT_EQUAL(BADGE_THREAT_PROX_CLOSE, snapshot.entities[0].proximity_level);
}

void test_badge_meta_variants_merge_into_one_display_entity(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t service = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:META:SERVICE",
        "Meta Device",
        0.55f,
        -58
    );
    service.ble_service_uuids[0] = 0xFD5F;
    service.ble_svc_uuid_count = 1;
    strncpy(service.class_reason, "uuid16:0xFD5F", sizeof(service.class_reason) - 1);

    drone_detection_t frame = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:META:FRAME",
        "Tracker (Generic)",
        0.50f,
        -42
    );
    frame.ble_company_id = 0x0D53;
    strncpy(frame.class_reason, "mfr_cid:0x0D53", sizeof(frame.class_reason) - 1);

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &service, 1000, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &frame, 2000, NULL));
    badge_threat_state_snapshot(&state, 2500, &snapshot);

    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_EQUAL(BADGE_THREAT_META, snapshot.entities[0].cls);
    TEST_ASSERT_EQUAL(BADGE_THREAT_CATEGORY_GLASS, snapshot.entities[0].category);
    TEST_ASSERT_EQUAL_STRING("Meta Glasses", snapshot.entities[0].label);
    TEST_ASSERT_EQUAL_UINT32(2, snapshot.entities[0].seen_count);
    TEST_ASSERT_EQUAL_INT(-42, snapshot.entities[0].best_rssi);
}

void test_badge_strong_unknown_ble_is_suppressed(void)
{
    badge_threat_event_t event;
    drone_detection_t ble = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:UNKNOWN:CLOSE",
        "Unknown",
        0.18f,
        -44
    );
    strncpy(ble.class_reason, "mfr_cid:0x1234", sizeof(ble.class_reason) - 1);

    TEST_ASSERT_FALSE(badge_threat_classify_detection(&ble, &event));
}

void test_badge_structured_nearby_ble_is_suppressed(void)
{
    badge_threat_event_t event;
    drone_detection_t ble = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:UNKNOWN:STRUCT",
        "Unknown",
        0.18f,
        -62
    );
    ble.ble_ad_type_count = 4;
    ble.ble_payload_len = 18;
    strncpy(ble.class_reason, "mfr_cid:0x1234", sizeof(ble.class_reason) - 1);

    TEST_ASSERT_FALSE(badge_threat_classify_detection(&ble, &event));
}

void test_badge_far_unknown_ble_does_not_flood_display(void)
{
    badge_threat_event_t event;
    drone_detection_t ble = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:UNKNOWN:FAR",
        "Unknown",
        0.10f,
        -82
    );
    ble.ble_ad_type_count = 4;
    ble.ble_payload_len = 18;

    TEST_ASSERT_FALSE(badge_threat_classify_detection(&ble, &event));
}

void test_badge_unknown_ble_is_suppressed_behind_wifi_anomaly(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t wifi = make_detection(
        DETECTION_SRC_WIFI_ASSOC,
        "wifi:suspicious",
        "Suspicious Wi-Fi",
        0.60f,
        -42
    );
    strncpy(wifi.class_reason, "suspicious assoc", sizeof(wifi.class_reason) - 1);
    drone_detection_t ble = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:UNKNOWN:CLOSE",
        "Unknown",
        0.18f,
        -44
    );

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &wifi, 1000, NULL));
    TEST_ASSERT_FALSE(badge_threat_state_ingest(&state, &ble, 1100, NULL));
    badge_threat_state_snapshot(&state, 1500, &snapshot);

    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_EQUAL(BADGE_THREAT_WIFI_ANOMALY, snapshot.entities[0].cls);
}

void test_badge_glasses_rank_above_wifi_anomaly_even_when_stale(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t meta = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:META:STALE",
        "Meta Glasses",
        0.80f,
        -42
    );
    drone_detection_t deauth = make_detection(
        DETECTION_SRC_WIFI_ASSOC,
        "wifi:deauth",
        "Deauth x5",
        0.80f,
        -42
    );
    strncpy(deauth.class_reason, "deauth count:5", sizeof(deauth.class_reason) - 1);

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &meta, 1000, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &deauth, 1000 + 420000, NULL));
    badge_threat_state_snapshot(&state, 1000 + 421000, &snapshot);

    TEST_ASSERT_TRUE(snapshot.entity_count >= 2);
    TEST_ASSERT_EQUAL(BADGE_THREAT_META, snapshot.entities[0].cls);
    TEST_ASSERT_EQUAL(BADGE_THREAT_CATEGORY_GLASS, snapshot.entities[0].category);
    TEST_ASSERT_EQUAL_STRING("Meta Glasses", snapshot.entities[0].label);
    TEST_ASSERT_EQUAL(BADGE_THREAT_WIFI_ANOMALY, snapshot.entities[1].cls);
    TEST_ASSERT_EQUAL_STRING("Deauth", snapshot.entities[1].label);
}

void test_badge_meta_sorts_above_louder_generic_ble(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t meta = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:META:FRAME",
        "Meta Glasses",
        0.80f,
        -72
    );
    drone_detection_t nearby = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:LOUD:NEARBY",
        "Nearby BLE",
        0.12f,
        -38
    );
    strncpy(nearby.class_reason, "nearby ble service", sizeof(nearby.class_reason) - 1);

    TEST_ASSERT_FALSE(badge_threat_state_ingest(&state, &nearby, 1000, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &meta, 1100, NULL));
    badge_threat_state_snapshot(&state, 2000, &snapshot);

    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_EQUAL(BADGE_THREAT_META, snapshot.entities[0].cls);
    TEST_ASSERT_EQUAL_STRING("Meta Glasses", snapshot.entities[0].label);
}

void test_badge_score_fades_after_activity_stales(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t fresh;
    badge_threat_snapshot_t stale;
    badge_threat_state_init(&state);

    drone_detection_t drone = make_detection(
        DETECTION_SRC_WIFI_BEACON,
        "RID-FADE",
        "Remote ID",
        0.90f,
        -60
    );

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &drone, 1000, NULL));
    badge_threat_state_snapshot(&state, 2000, &fresh);
    badge_threat_state_snapshot(&state, 120000, &stale);

    TEST_ASSERT_TRUE(fresh.threat_score > stale.threat_score);
    TEST_ASSERT_EQUAL_INT(1, stale.entity_count);
    TEST_ASSERT_TRUE(stale.entities[0].stale);
    TEST_ASSERT_EQUAL(BADGE_THREAT_DRONE, stale.entities[0].cls);
}

void test_badge_drone_memory_expires_after_demo_window(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t drone = make_detection(
        DETECTION_SRC_WIFI_BEACON,
        "RID-FADE",
        "Remote ID",
        0.90f,
        -60
    );

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &drone, 1000, NULL));
    badge_threat_state_snapshot(&state, 1000 + 320000, &snapshot);

    TEST_ASSERT_EQUAL_INT(0, snapshot.entity_count);
    TEST_ASSERT_EQUAL_STRING("Watching", snapshot.top_label);
}

void test_badge_wifi_anomaly_memory_outlives_active_window(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t deauth = make_detection(
        DETECTION_SRC_WIFI_ASSOC,
        "wifi:deauth",
        "Deauth x2",
        0.75f,
        -48
    );
    strncpy(deauth.class_reason, "deauth count:2", sizeof(deauth.class_reason) - 1);

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &deauth, 1000, NULL));
    badge_threat_state_snapshot(&state, 1000 + 90000, &snapshot);

    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_EQUAL(BADGE_THREAT_WIFI_ANOMALY, snapshot.entities[0].cls);
    TEST_ASSERT_TRUE(snapshot.entities[0].stale);
}

void test_badge_multiple_drones_raise_score(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t drone1 = make_detection(
        DETECTION_SRC_WIFI_BEACON,
        "RID-ONE",
        "Remote ID",
        0.90f,
        -68
    );
    drone_detection_t drone2 = make_detection(
        DETECTION_SRC_WIFI_SSID,
        "RID-TWO",
        "DJI",
        0.70f,
        -50
    );
    strncpy(drone2.ssid, "DJI-Mini-Test", sizeof(drone2.ssid) - 1);

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &drone1, 1000, NULL));
    badge_threat_state_snapshot(&state, 1200, &snapshot);
    float single_score = snapshot.threat_score;

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &drone2, 1400, NULL));
    badge_threat_state_snapshot(&state, 1800, &snapshot);

    TEST_ASSERT_EQUAL_UINT32(2, snapshot.active_counts[BADGE_THREAT_DRONE]);
    TEST_ASSERT_TRUE(snapshot.threat_score > single_score);
}

void test_badge_wifi_noise_is_ignored(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t ap = make_detection(
        DETECTION_SRC_WIFI_AP_INVENTORY,
        "HomeAP",
        "Netgear",
        0.05f,
        -35
    );
    strncpy(ap.bssid, "AA:BB:CC:DD:EE:FF", sizeof(ap.bssid) - 1);

    drone_detection_t probe = make_detection(
        DETECTION_SRC_WIFI_PROBE_REQUEST,
        "probe-phone",
        "Unknown",
        0.05f,
        -70
    );
    strncpy(probe.bssid, "11:22:33:44:55:66", sizeof(probe.bssid) - 1);

    TEST_ASSERT_FALSE(badge_threat_state_ingest(&state, &ap, 1000, NULL));
    TEST_ASSERT_FALSE(badge_threat_state_ingest(&state, &probe, 1100, NULL));
    badge_threat_state_snapshot(&state, 1500, &snapshot);

    TEST_ASSERT_EQUAL_UINT32(0, snapshot.active_counts[BADGE_THREAT_WIFI_ANOMALY]);
    TEST_ASSERT_EQUAL_INT(0, snapshot.entity_count);
}

void test_badge_labels_are_lcd_safe_friendly_names(void)
{
    badge_threat_event_t event;
    drone_detection_t meta = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "AA:BB:CC:DD:EE:FF",
        "Ray-Ban Meta Glasses",
        0.80f,
        -48
    );

    TEST_ASSERT_TRUE(badge_threat_classify_detection(&meta, &event));
    TEST_ASSERT_EQUAL_STRING("Meta Glasses", event.label);
    TEST_ASSERT_TRUE(badge_threat_label_is_lcd_safe(event.label));
    TEST_ASSERT_FALSE(badge_threat_label_is_lcd_safe("AA:BB:CC:DD:EE:FF"));
}

void test_badge_drone_ssid_label_keeps_detail(void)
{
    badge_threat_event_t event;
    drone_detection_t drone = make_detection(
        DETECTION_SRC_WIFI_SSID,
        "DRONE:AA:BB:CC:DD:EE:FF",
        "Unknown",
        0.60f,
        -55
    );
    strncpy(drone.ssid, "DJI-Mini-Office", sizeof(drone.ssid) - 1);

    TEST_ASSERT_TRUE(badge_threat_classify_detection(&drone, &event));
    TEST_ASSERT_EQUAL(BADGE_THREAT_DRONE, event.cls);
    TEST_ASSERT_EQUAL(BADGE_THREAT_CATEGORY_SSID, event.category);
    TEST_ASSERT_EQUAL_STRING("Drone SSID", event.label);
    TEST_ASSERT_EQUAL_STRING("ssid DJI-Mini-Office", event.detail);
}

void test_badge_fof_drone_ssid_is_not_suppressed(void)
{
    badge_threat_event_t event;
    drone_detection_t drone = make_detection(
        DETECTION_SRC_WIFI_SSID,
        "FOF-Drone-TEST",
        "FriendOrFoe",
        0.35f,
        -54
    );
    strncpy(drone.ssid, "FOF-Drone-TEST", sizeof(drone.ssid) - 1);

    TEST_ASSERT_TRUE(badge_threat_classify_detection(&drone, &event));
    TEST_ASSERT_EQUAL(BADGE_THREAT_DRONE, event.cls);
    TEST_ASSERT_EQUAL(BADGE_THREAT_CATEGORY_SSID, event.category);
    TEST_ASSERT_EQUAL_STRING("Drone SSID", event.label);
    TEST_ASSERT_EQUAL_STRING("ssid FOF-Drone-TEST", event.detail);
}

void test_badge_meta_structured_evidence_beats_tracker_label(void)
{
    badge_threat_event_t event;
    drone_detection_t meta = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:12345678:Tracker",
        "Tracker (Generic)",
        0.50f,
        -44
    );
    meta.ble_company_id = 0x0D53;
    strncpy(meta.class_reason, "mfr_cid:0x0D53", sizeof(meta.class_reason) - 1);

    TEST_ASSERT_TRUE(badge_threat_classify_detection(&meta, &event));
    TEST_ASSERT_EQUAL(BADGE_THREAT_META, event.cls);
    TEST_ASSERT_EQUAL_STRING("Meta Glasses", event.label);
    TEST_ASSERT_EQUAL_STRING("Meta frame signal", event.detail);
    TEST_ASSERT_TRUE(event.base_score >= 80.0f);
}

void test_badge_meta_uuid_detail_is_human_not_raw_hex(void)
{
    badge_threat_event_t event;
    drone_detection_t meta = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:12345678:Meta",
        "Meta Device",
        0.30f,
        -52
    );
    meta.ble_service_uuids[0] = 0xFEB8;
    meta.ble_svc_uuid_count = 1;
    strncpy(meta.class_reason, "uuid16:0xFEB8", sizeof(meta.class_reason) - 1);

    TEST_ASSERT_TRUE(badge_threat_classify_detection(&meta, &event));
    TEST_ASSERT_EQUAL(BADGE_THREAT_META, event.cls);
    TEST_ASSERT_EQUAL_STRING("Meta Glasses", event.label);
    TEST_ASSERT_EQUAL_STRING("Meta service", event.detail);
    TEST_ASSERT_NULL(strstr(event.detail, "0x"));
    TEST_ASSERT_NULL(strstr(event.detail, "cid"));
}

void test_badge_remote_id_without_human_fields_hides_raw_hex_detail(void)
{
    badge_threat_event_t event;
    drone_detection_t rid = make_detection(
        DETECTION_SRC_WIFI_BEACON,
        "rid_AA:BB:CC:DD:EE:FF",
        "OpenDroneID",
        0.82f,
        -52
    );

    TEST_ASSERT_TRUE(badge_threat_classify_detection(&rid, &event));
    TEST_ASSERT_EQUAL(BADGE_THREAT_DRONE, event.cls);
    TEST_ASSERT_EQUAL_STRING("Remote ID", event.label);
    TEST_ASSERT_EQUAL_STRING("Remote ID signal", event.detail);
    TEST_ASSERT_NULL(strstr(event.detail, "AA"));
    TEST_ASSERT_NULL(strstr(event.detail, ":"));
}

void test_badge_generic_nearby_ble_is_not_a_display_row(void)
{
    badge_threat_event_t event;
    drone_detection_t nearby = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:DEADBEEF:Nearby BLE",
        "Nearby BLE",
        0.18f,
        -42
    );
    strncpy(nearby.class_reason, "nearby ble service", sizeof(nearby.class_reason) - 1);

    TEST_ASSERT_FALSE(badge_threat_classify_detection(&nearby, &event));
}

void test_badge_tracker_label_names_airtag(void)
{
    badge_threat_event_t event;
    drone_detection_t tag = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:12345678:AirTag",
        "AirTag",
        0.50f,
        -44
    );

    TEST_ASSERT_TRUE(badge_threat_classify_detection(&tag, &event));
    TEST_ASSERT_EQUAL(BADGE_THREAT_TRACKER, event.cls);
    TEST_ASSERT_EQUAL(BADGE_THREAT_CATEGORY_TAG_CLOSE, event.category);
    TEST_ASSERT_EQUAL_STRING("AirTag", event.label);
}

void test_badge_far_airtag_is_hidden_unless_close(void)
{
    badge_threat_event_t event;
    drone_detection_t tag = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:12345678:AirTag",
        "AirTag",
        0.50f,
        -68
    );

    TEST_ASSERT_FALSE(badge_threat_classify_detection(&tag, &event));
}

void test_badge_remote_id_with_human_evidence_ranks_first(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t glass = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:META:FRAME",
        "Ray-Ban Meta Glasses",
        0.80f,
        -42
    );
    drone_detection_t ssid = make_detection(
        DETECTION_SRC_WIFI_ASSOC,
        "ssid:watch",
        "Notable SSID",
        0.55f,
        -50
    );
    strncpy(ssid.ssid, "DEFCON-FPV-WATCH", sizeof(ssid.ssid) - 1);
    drone_detection_t rid = make_detection(
        DETECTION_SRC_BLE_RID,
        "RID-DEFCON-01",
        "OpenDroneID",
        0.95f,
        -48
    );
    strncpy(rid.model, "DJI Mini 4", sizeof(rid.model) - 1);
    strncpy(rid.self_id_text, "demo flight", sizeof(rid.self_id_text) - 1);

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &glass, 1000, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &ssid, 1100, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &rid, 1200, NULL));
    badge_threat_state_snapshot(&state, 1500, &snapshot);

    TEST_ASSERT_TRUE(snapshot.entity_count >= 3);
    TEST_ASSERT_EQUAL(BADGE_THREAT_CATEGORY_DRONE, snapshot.entities[0].category);
    TEST_ASSERT_EQUAL_STRING("Remote ID", snapshot.entities[0].label);
    TEST_ASSERT_TRUE(strstr(snapshot.entities[0].detail, "DJI Mini") != NULL);
}

void test_badge_notable_ssid_ranks_above_flock_and_glasses(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t glass = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:GLASS",
        "Ray-Ban Meta Glasses",
        0.80f,
        -42
    );
    drone_detection_t flock = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:FLOCK",
        "Flock Safety",
        0.70f,
        -50
    );
    strncpy(flock.ble_name, "Flock Camera", sizeof(flock.ble_name) - 1);
    drone_detection_t ssid = make_detection(
        DETECTION_SRC_WIFI_ASSOC,
        "ssid:defcon",
        "Notable SSID",
        0.55f,
        -55
    );
    strncpy(ssid.ssid, "DEFCON-FPV-WATCH", sizeof(ssid.ssid) - 1);

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &glass, 1000, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &flock, 1100, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &ssid, 1200, NULL));
    badge_threat_state_snapshot(&state, 1500, &snapshot);

    TEST_ASSERT_TRUE(snapshot.entity_count >= 3);
    TEST_ASSERT_EQUAL(BADGE_THREAT_CATEGORY_SSID, snapshot.entities[0].category);
    TEST_ASSERT_EQUAL_STRING("DEFCON-FPV-WATCH", snapshot.entities[0].label);
    TEST_ASSERT_EQUAL(BADGE_THREAT_CATEGORY_FLOCK, snapshot.entities[1].category);
    TEST_ASSERT_EQUAL(BADGE_THREAT_CATEGORY_GLASS, snapshot.entities[2].category);
}

void test_badge_notable_ssid_suffixes_group_for_readability(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t ssid = make_detection(
        DETECTION_SRC_WIFI_ASSOC,
        "ssid:defcon-2",
        "Notable SSID",
        0.55f,
        -78
    );
    strncpy(ssid.ssid, "DEFCON-FPV-WATCH-2", sizeof(ssid.ssid) - 1);
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &ssid, 1000, NULL));

    strncpy(ssid.ssid, "DEFCON-FPV-WATCH-3", sizeof(ssid.ssid) - 1);
    ssid.rssi = -69;
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &ssid, 2000, NULL));

    badge_threat_state_snapshot(&state, 2500, &snapshot);
    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_EQUAL(BADGE_THREAT_CATEGORY_SSID, snapshot.entities[0].category);
    TEST_ASSERT_EQUAL_STRING("DEFCON-FPV-WATCH", snapshot.entities[0].label);
    TEST_ASSERT_EQUAL_UINT32(2, snapshot.entities[0].seen_count);
    TEST_ASSERT_EQUAL_INT(-69, snapshot.entities[0].best_rssi);
}

void test_badge_teamcharitycase_ssid_is_suppressed(void)
{
    badge_threat_event_t event;
    drone_detection_t ssid = make_detection(
        DETECTION_SRC_WIFI_ASSOC,
        "ssid:teamcharitycase",
        "Notable SSID",
        0.70f,
        -42
    );
    strncpy(ssid.ssid, "TeamCharityCase-DC33", sizeof(ssid.ssid) - 1);
    strncpy(ssid.class_reason, "Notable SSID", sizeof(ssid.class_reason) - 1);

    TEST_ASSERT_FALSE(badge_threat_classify_detection(&ssid, &event));
}

void test_badge_plain_fof_ssid_is_suppressed_as_ambient(void)
{
    badge_threat_event_t event;
    drone_detection_t ssid = make_detection(
        DETECTION_SRC_WIFI_SSID,
        "ssid:fof-live",
        "Notable SSID",
        0.70f,
        -42
    );
    strncpy(ssid.ssid, "FOF-Live", sizeof(ssid.ssid) - 1);
    strncpy(ssid.class_reason, "Notable SSID", sizeof(ssid.class_reason) - 1);

    TEST_ASSERT_FALSE(badge_threat_classify_detection(&ssid, &event));
}

void test_badge_flock_ble_name_produces_flock_camera(void)
{
    badge_threat_event_t event;
    drone_detection_t flock = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:FLOCK:CAM",
        "Flock Safety",
        0.70f,
        -52
    );
    strncpy(flock.ble_name, "Flock Camera", sizeof(flock.ble_name) - 1);

    TEST_ASSERT_TRUE(badge_threat_classify_detection(&flock, &event));
    TEST_ASSERT_EQUAL(BADGE_THREAT_OTHER, event.cls);
    TEST_ASSERT_EQUAL(BADGE_THREAT_CATEGORY_FLOCK, event.category);
    TEST_ASSERT_EQUAL_STRING("FLOCK Camera", event.label);
    TEST_ASSERT_TRUE(strstr(event.detail, "Flock") != NULL);
}

void test_badge_meta_rayban_category_is_glass(void)
{
    badge_threat_event_t event;
    drone_detection_t glass = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:RAYBAN",
        "Ray-Ban Meta",
        0.80f,
        -50
    );
    strncpy(glass.ble_name, "Ray-Ban Meta", sizeof(glass.ble_name) - 1);

    TEST_ASSERT_TRUE(badge_threat_classify_detection(&glass, &event));
    TEST_ASSERT_EQUAL(BADGE_THREAT_META, event.cls);
    TEST_ASSERT_EQUAL(BADGE_THREAT_CATEGORY_GLASS, event.category);
    TEST_ASSERT_EQUAL_STRING("Meta Glasses", event.label);
}

void test_badge_skimmer_names_produce_skim_rows(void)
{
    badge_threat_event_t event;
    drone_detection_t skim = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:HC05",
        "Unknown",
        0.60f,
        -46
    );
    strncpy(skim.ble_name, "HC-05", sizeof(skim.ble_name) - 1);

    TEST_ASSERT_TRUE(badge_threat_classify_detection(&skim, &event));
    TEST_ASSERT_EQUAL(BADGE_THREAT_OTHER, event.cls);
    TEST_ASSERT_EQUAL(BADGE_THREAT_CATEGORY_SKIM, event.category);
    TEST_ASSERT_EQUAL_STRING("Skimmer", event.label);
    TEST_ASSERT_EQUAL_STRING("HC-05", event.detail);
}

void test_badge_generic_drone_without_evidence_is_hidden(void)
{
    badge_threat_event_t event;
    drone_detection_t generic = make_detection(
        DETECTION_SRC_WIFI_ASSOC,
        "wifi:generic",
        "Drone",
        0.40f,
        -45
    );
    strncpy(generic.class_reason, "drone", sizeof(generic.class_reason) - 1);

    TEST_ASSERT_FALSE(badge_threat_classify_detection(&generic, &event));
}

void test_badge_deauth_status_event_gets_wifi_label(void)
{
    badge_threat_event_t event;
    drone_detection_t deauth = make_detection(
        DETECTION_SRC_WIFI_ASSOC,
        "wifi:deauth",
        "Deauth x1",
        0.70f,
        0
    );
    strncpy(deauth.class_reason, "deauth count:1", sizeof(deauth.class_reason) - 1);

    TEST_ASSERT_TRUE(badge_threat_classify_detection(&deauth, &event));
    TEST_ASSERT_EQUAL(BADGE_THREAT_WIFI_ANOMALY, event.cls);
    TEST_ASSERT_EQUAL_STRING("Deauth", event.label);
}

void test_badge_disassoc_status_event_gets_specific_wifi_label(void)
{
    badge_threat_event_t event;
    drone_detection_t disassoc = make_detection(
        DETECTION_SRC_WIFI_ASSOC,
        "wifi:disassoc",
        "Disassoc x4",
        0.65f,
        0
    );
    strncpy(disassoc.class_reason, "disassoc count:4", sizeof(disassoc.class_reason) - 1);

    TEST_ASSERT_TRUE(badge_threat_classify_detection(&disassoc, &event));
    TEST_ASSERT_EQUAL(BADGE_THREAT_WIFI_ANOMALY, event.cls);
    TEST_ASSERT_EQUAL_STRING("Disassoc", event.label);
}

void test_badge_strong_normal_wifi_assoc_is_ignored(void)
{
    badge_threat_event_t event;
    drone_detection_t assoc = make_detection(
        DETECTION_SRC_WIFI_ASSOC,
        "STA:AA:BB:CC:DD:EE:FF",
        "WiFi-Assoc",
        0.10f,
        -18
    );

    TEST_ASSERT_FALSE(badge_threat_classify_detection(&assoc, &event));
}

void test_badge_status_meta_evidence_is_diagnostic_only(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t meta = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "status:ble:meta:0",
        "Meta Glasses",
        0.78f,
        -58
    );
    strncpy(meta.class_reason, "status:meta", sizeof(meta.class_reason) - 1);

    TEST_ASSERT_FALSE(badge_threat_state_ingest(&state, &meta, 1000, NULL));
    badge_threat_state_snapshot(&state, 1200, &snapshot);

    TEST_ASSERT_EQUAL_INT(0, snapshot.entity_count);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.active_counts[BADGE_THREAT_META]);
}

void test_badge_status_wifi_drone_ssid_creates_ssid_entity(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t drone = make_detection(
        DETECTION_SRC_WIFI_SSID,
        "status:wifi:ssid:1",
        "Drone SSID",
        0.35f,
        0
    );
    strncpy(drone.ssid, "DJI-Mini-Status", sizeof(drone.ssid) - 1);

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &drone, 1000, NULL));
    badge_threat_state_snapshot(&state, 1200, &snapshot);

    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.active_counts[BADGE_THREAT_DRONE]);
    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_snapshot_drone_evidence_count(&snapshot));
    TEST_ASSERT_EQUAL_STRING("Drone SSID", snapshot.entities[0].label);
    TEST_ASSERT_EQUAL_STRING("ssid DJI-Mini-Status", snapshot.entities[0].detail);
}

void test_badge_wifi_drone_ssid_from_two_slots_merges_entity(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t drone = make_detection(
        DETECTION_SRC_WIFI_SSID,
        "status:wifi:ssid:0",
        "Drone SSID",
        0.35f,
        -58
    );
    strncpy(drone.ssid, "DJI-Mini-Status", sizeof(drone.ssid) - 1);

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &drone, 1000, NULL));
    strncpy(drone.drone_id, "status:wifi:ssid:1", sizeof(drone.drone_id) - 1);
    drone.rssi = -51;
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &drone, 1500, NULL));
    badge_threat_state_snapshot(&state, 1800, &snapshot);

    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.active_counts[BADGE_THREAT_DRONE]);
    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_snapshot_drone_evidence_count(&snapshot));
    TEST_ASSERT_EQUAL_UINT32(2, snapshot.entities[0].seen_count);
    TEST_ASSERT_EQUAL_INT(-51, snapshot.entities[0].best_rssi);
    TEST_ASSERT_EQUAL_STRING("Drone SSID", snapshot.entities[0].label);
    TEST_ASSERT_EQUAL_STRING("ssid DJI-Mini-Status", snapshot.entities[0].detail);
}

void test_badge_status_and_live_wifi_drone_ssid_merge_entity(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t status = make_detection(
        DETECTION_SRC_WIFI_SSID,
        "status:wifi:ssid:1",
        "Drone SSID",
        0.35f,
        -60
    );
    strncpy(status.ssid, "DJI-Mini-Status", sizeof(status.ssid) - 1);

    drone_detection_t live = make_detection(
        DETECTION_SRC_WIFI_SSID,
        "DRONE:AA:BB:CC:DD:EE:FF",
        "DJI",
        0.68f,
        -47
    );
    strncpy(live.ssid, "DJI-Mini-Status", sizeof(live.ssid) - 1);

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &status, 1000, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &live, 1600, NULL));
    badge_threat_state_snapshot(&state, 2000, &snapshot);

    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.active_counts[BADGE_THREAT_DRONE]);
    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_snapshot_drone_evidence_count(&snapshot));
    TEST_ASSERT_EQUAL_UINT32(2, snapshot.entities[0].seen_count);
    TEST_ASSERT_EQUAL_STRING("Drone SSID", snapshot.entities[0].label);
    TEST_ASSERT_EQUAL_STRING("ssid DJI-Mini-Status", snapshot.entities[0].detail);
}

void test_badge_status_wifi_drone_detail_names_network(void)
{
    badge_threat_event_t event;
    drone_detection_t drone = make_detection(
        DETECTION_SRC_WIFI_SSID,
        "status:wifi:ssid:1",
        "Drone SSID",
        0.35f,
        0
    );
    strncpy(drone.ssid, "DJI-Mini-Status", sizeof(drone.ssid) - 1);

    TEST_ASSERT_TRUE(badge_threat_classify_detection(&drone, &event));
    TEST_ASSERT_EQUAL_STRING("Drone SSID", event.label);
    TEST_ASSERT_EQUAL_STRING("ssid DJI-Mini-Status", event.detail);
}

void test_badge_remote_id_title_helper_says_drone_near(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    char title[24];
    badge_threat_state_init(&state);

    drone_detection_t rid = make_detection(
        DETECTION_SRC_BLE_RID,
        "RID-ONE",
        "OpenDroneID",
        0.90f,
        -52
    );

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &rid, 1000, NULL));
    badge_threat_state_snapshot(&state, 1200, &snapshot);
    badge_threat_format_drone_near_title(&snapshot, title, sizeof(title));

    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_snapshot_count_active(
        &snapshot,
        BADGE_THREAT_DRONE,
        BADGE_THREAT_CATEGORY_DRONE,
        false
    ));
    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_snapshot_drone_evidence_count(&snapshot));
    TEST_ASSERT_EQUAL_STRING("DRONE NEAR", title);
}

void test_badge_remote_id_title_helper_counts_multiple_drones(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    char title[24];
    char row_title[24];
    badge_threat_state_init(&state);

    drone_detection_t rid1 = make_detection(
        DETECTION_SRC_BLE_RID,
        "RID-ONE",
        "OpenDroneID",
        0.90f,
        -52
    );
    drone_detection_t rid2 = make_detection(
        DETECTION_SRC_WIFI_BEACON,
        "RID-TWO",
        "Remote ID",
        0.88f,
        -57
    );

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &rid1, 1000, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &rid2, 1100, NULL));
    badge_threat_state_snapshot(&state, 1300, &snapshot);
    badge_threat_format_drone_near_title(&snapshot, title, sizeof(title));

    TEST_ASSERT_EQUAL_INT(2, snapshot.entity_count);
    TEST_ASSERT_EQUAL_UINT32(2, badge_threat_snapshot_count_active(
        &snapshot,
        BADGE_THREAT_DRONE,
        BADGE_THREAT_CATEGORY_DRONE,
        false
    ));
    TEST_ASSERT_EQUAL_STRING("DRONE NEAR", title);
    TEST_ASSERT_EQUAL_UINT32(4, strlen(snapshot.entities[0].display_id));
    TEST_ASSERT_EQUAL_UINT32(4, strlen(snapshot.entities[1].display_id));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(snapshot.entities[0].display_id,
                                   snapshot.entities[1].display_id));

    badge_threat_format_drone_entity_title(
        &snapshot,
        &snapshot.entities[0],
        row_title,
        sizeof(row_title)
    );
    TEST_ASSERT_EQUAL_STRING("DRONE #1", row_title);
    badge_threat_format_drone_entity_title(
        &snapshot,
        &snapshot.entities[1],
        row_title,
        sizeof(row_title)
    );
    TEST_ASSERT_EQUAL_STRING("DRONE #2", row_title);
}

void test_badge_drone_evidence_count_includes_rid_and_ssid(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t rid = make_detection(
        DETECTION_SRC_BLE_RID,
        "RID-EVIDENCE-ONE",
        "OpenDroneID",
        0.90f,
        -50
    );
    drone_detection_t ssid = make_detection(
        DETECTION_SRC_WIFI_SSID,
        "status:wifi:ssid:0",
        "Drone SSID",
        0.60f,
        -54
    );
    strncpy(ssid.ssid, "DJI-Mini-Evidence", sizeof(ssid.ssid) - 1);

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &rid, 1000, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &ssid, 1100, NULL));
    badge_threat_state_snapshot(&state, 1300, &snapshot);

    TEST_ASSERT_EQUAL_INT(2, snapshot.entity_count);
    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_snapshot_count_active(
        &snapshot,
        BADGE_THREAT_DRONE,
        BADGE_THREAT_CATEGORY_DRONE,
        false
    ));
    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_snapshot_count_active(
        &snapshot,
        BADGE_THREAT_DRONE,
        BADGE_THREAT_CATEGORY_SSID,
        false
    ));
    TEST_ASSERT_EQUAL_UINT32(2, badge_threat_snapshot_drone_evidence_count(&snapshot));
}

void test_badge_lower_lane_hides_single_drone_evidence(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t rid = make_detection(
        DETECTION_SRC_BLE_RID,
        "RID-LOWER-ONLY",
        "OpenDroneID",
        0.90f,
        -50
    );

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &rid, 1000, NULL));
    badge_threat_state_snapshot(&state, 1300, &snapshot);

    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_snapshot_drone_evidence_count(&snapshot));
    TEST_ASSERT_FALSE(badge_threat_snapshot_should_show_lower_drone_evidence(
        &snapshot,
        &snapshot.entities[0]
    ));
}

void test_badge_lower_lane_shows_multiple_drone_evidence(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t rid = make_detection(
        DETECTION_SRC_BLE_RID,
        "RID-LOWER-MIX",
        "OpenDroneID",
        0.90f,
        -50
    );
    drone_detection_t ssid = make_detection(
        DETECTION_SRC_WIFI_SSID,
        "status:wifi:ssid:0",
        "Drone SSID",
        0.60f,
        -54
    );
    strncpy(ssid.ssid, "DJI-Mini-Lower", sizeof(ssid.ssid) - 1);

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &rid, 1000, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &ssid, 1100, NULL));
    badge_threat_state_snapshot(&state, 1300, &snapshot);

    TEST_ASSERT_EQUAL_UINT32(2, badge_threat_snapshot_drone_evidence_count(&snapshot));
    for (int i = 0; i < snapshot.entity_count; i++) {
        const badge_threat_snapshot_entity_t *item = &snapshot.entities[i];
        if (item->cls == BADGE_THREAT_DRONE) {
            TEST_ASSERT_TRUE(badge_threat_snapshot_should_show_lower_drone_evidence(
                &snapshot,
                item
            ));
        }
    }
}

void test_badge_drone_evidence_count_includes_two_rid_and_ssid(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t rid1 = make_detection(
        DETECTION_SRC_BLE_RID,
        "RID-EVIDENCE-ONE",
        "OpenDroneID",
        0.90f,
        -50
    );
    drone_detection_t rid2 = make_detection(
        DETECTION_SRC_WIFI_BEACON,
        "RID-EVIDENCE-TWO",
        "Remote ID",
        0.88f,
        -58
    );
    drone_detection_t ssid = make_detection(
        DETECTION_SRC_WIFI_SSID,
        "status:wifi:ssid:1",
        "Drone SSID",
        0.60f,
        -54
    );
    strncpy(ssid.ssid, "DJI-Mini-Evidence", sizeof(ssid.ssid) - 1);

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &rid1, 1000, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &rid2, 1100, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &ssid, 1200, NULL));
    badge_threat_state_snapshot(&state, 1400, &snapshot);

    TEST_ASSERT_EQUAL_INT(3, snapshot.entity_count);
    TEST_ASSERT_EQUAL_UINT32(2, badge_threat_snapshot_count_active(
        &snapshot,
        BADGE_THREAT_DRONE,
        BADGE_THREAT_CATEGORY_DRONE,
        false
    ));
    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_snapshot_count_active(
        &snapshot,
        BADGE_THREAT_DRONE,
        BADGE_THREAT_CATEGORY_SSID,
        false
    ));
    TEST_ASSERT_EQUAL_UINT32(3, badge_threat_snapshot_drone_evidence_count(&snapshot));
}

void test_badge_display_lane_splits_ble_and_wifi_items(void)
{
    badge_threat_snapshot_entity_t meta = {
        .active = true,
        .cls = BADGE_THREAT_META,
        .category = BADGE_THREAT_CATEGORY_GLASS,
    };
    badge_threat_snapshot_entity_t tag = {
        .active = true,
        .cls = BADGE_THREAT_TRACKER,
        .category = BADGE_THREAT_CATEGORY_TAG_CLOSE,
    };
    badge_threat_snapshot_entity_t rid = {
        .active = true,
        .cls = BADGE_THREAT_DRONE,
        .category = BADGE_THREAT_CATEGORY_DRONE,
    };
    strncpy(rid.label, "Remote ID", sizeof(rid.label) - 1);
    badge_threat_snapshot_entity_t ssid = {
        .active = true,
        .cls = BADGE_THREAT_DRONE,
        .category = BADGE_THREAT_CATEGORY_SSID,
    };
    badge_threat_snapshot_entity_t wifi = {
        .active = true,
        .cls = BADGE_THREAT_WIFI_ANOMALY,
        .category = BADGE_THREAT_CATEGORY_WIFI,
    };
    badge_threat_snapshot_entity_t inactive = {
        .active = false,
        .cls = BADGE_THREAT_META,
        .category = BADGE_THREAT_CATEGORY_GLASS,
    };

    TEST_ASSERT_EQUAL(BADGE_THREAT_DISPLAY_LANE_BLE,
                      badge_threat_snapshot_entity_display_lane(&meta));
    TEST_ASSERT_EQUAL(BADGE_THREAT_DISPLAY_LANE_BLE,
                      badge_threat_snapshot_entity_display_lane(&tag));
    TEST_ASSERT_EQUAL(BADGE_THREAT_DISPLAY_LANE_WIFI,
                      badge_threat_snapshot_entity_display_lane(&rid));
    TEST_ASSERT_EQUAL(BADGE_THREAT_DISPLAY_LANE_WIFI,
                      badge_threat_snapshot_entity_display_lane(&ssid));
    TEST_ASSERT_EQUAL(BADGE_THREAT_DISPLAY_LANE_WIFI,
                      badge_threat_snapshot_entity_display_lane(&wifi));
    TEST_ASSERT_EQUAL(BADGE_THREAT_DISPLAY_LANE_NONE,
                      badge_threat_snapshot_entity_display_lane(&inactive));
}

void test_badge_drone_evidence_count_excludes_stale_ssid(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t rid = make_detection(
        DETECTION_SRC_BLE_RID,
        "RID-EVIDENCE-LIVE",
        "OpenDroneID",
        0.90f,
        -50
    );
    drone_detection_t ssid = make_detection(
        DETECTION_SRC_WIFI_SSID,
        "status:wifi:ssid:0",
        "Drone SSID",
        0.60f,
        -54
    );
    strncpy(ssid.ssid, "DJI-Mini-Fades", sizeof(ssid.ssid) - 1);

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &rid, 1000, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &ssid, 1100, NULL));
    badge_threat_state_snapshot(&state, 17001, &snapshot);

    TEST_ASSERT_EQUAL_INT(2, snapshot.entity_count);
    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_snapshot_count_active(
        &snapshot,
        BADGE_THREAT_DRONE,
        BADGE_THREAT_CATEGORY_DRONE,
        false
    ));
    TEST_ASSERT_EQUAL_UINT32(0, badge_threat_snapshot_count_active(
        &snapshot,
        BADGE_THREAT_DRONE,
        BADGE_THREAT_CATEGORY_SSID,
        false
    ));
    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_snapshot_drone_evidence_count(&snapshot));
}

void test_badge_view_key_uses_remote_id_display_id(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    char key[BADGE_THREAT_VIEW_KEY_LEN];
    badge_threat_state_init(&state);

    drone_detection_t rid = make_detection(
        DETECTION_SRC_BLE_RID,
        "RID-VIEW-ONE",
        "OpenDroneID",
        0.90f,
        -52
    );

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &rid, 1000, NULL));
    badge_threat_state_snapshot(&state, 1200, &snapshot);

    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_TRUE(badge_threat_snapshot_entity_is_remote_id_drone(
        &snapshot.entities[0]
    ));
    TEST_ASSERT_TRUE(badge_threat_snapshot_entity_view_key(
        &snapshot.entities[0],
        key,
        sizeof(key)
    ));
    TEST_ASSERT_EQUAL_UINT32(4, strlen(snapshot.entities[0].display_id));
    TEST_ASSERT_TRUE(strncmp(key, "RID:", 4) == 0);
    TEST_ASSERT_TRUE(strstr(key, snapshot.entities[0].display_id) != NULL);
}

void test_badge_meta_glasses_title_count_and_display_ids(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    char title[24];
    badge_threat_state_init(&state);

    drone_detection_t meta1 = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:11112222:Meta Glasses",
        "Meta Glasses",
        0.85f,
        -44
    );
    strncpy(meta1.bssid, "AA:BB:CC:DD:EE:01", sizeof(meta1.bssid) - 1);
    strncpy(meta1.model, "FP:11112222", sizeof(meta1.model) - 1);
    drone_detection_t meta2 = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:33334444:Meta Glasses",
        "Meta Glasses",
        0.82f,
        -57
    );
    strncpy(meta2.bssid, "AA:BB:CC:DD:EE:02", sizeof(meta2.bssid) - 1);
    strncpy(meta2.model, "FP:33334444", sizeof(meta2.model) - 1);

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &meta1, 1000, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &meta2, 1100, NULL));
    badge_threat_state_snapshot(&state, 1300, &snapshot);
    badge_threat_format_meta_glasses_title(title, sizeof(title));

    TEST_ASSERT_EQUAL_STRING("META GLASSES", title);
    TEST_ASSERT_EQUAL_UINT32(2, badge_threat_snapshot_meta_glasses_count(&snapshot));
    TEST_ASSERT_EQUAL_INT(2, snapshot.entity_count);
    TEST_ASSERT_TRUE(badge_threat_snapshot_entity_is_meta_glasses(&snapshot.entities[0]));
    TEST_ASSERT_TRUE(badge_threat_snapshot_entity_is_meta_glasses(&snapshot.entities[1]));
    TEST_ASSERT_EQUAL_PTR(&snapshot.entities[0],
                          badge_threat_snapshot_best_meta_glasses(&snapshot));
    TEST_ASSERT_EQUAL_UINT32(4, strlen(snapshot.entities[0].display_id));
    TEST_ASSERT_EQUAL_UINT32(4, strlen(snapshot.entities[1].display_id));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(snapshot.entities[0].display_id,
                                   snapshot.entities[1].display_id));
}

void test_badge_meta_count_ignores_scanner_seen_group_count(void)
{
    badge_threat_snapshot_t snapshot = {0};
    snapshot.entity_count = 1;
    snapshot.entities[0].active = true;
    snapshot.entities[0].stale = false;
    snapshot.entities[0].cls = BADGE_THREAT_META;
    snapshot.entities[0].category = BADGE_THREAT_CATEGORY_GLASS;
    strncpy(snapshot.entities[0].label, "Meta Glasses",
            sizeof(snapshot.entities[0].label) - 1);
    strncpy(snapshot.entities[0].display_id, "CAFE",
            sizeof(snapshot.entities[0].display_id) - 1);
    snapshot.entities[0].group_count = 999;
    snapshot.entities[0].seen_count = 999;
    snapshot.entities[0].event_count = 999;

    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_snapshot_meta_glasses_count(&snapshot));

    snapshot.entities[0].display_id[0] = '\0';
    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_snapshot_meta_glasses_count(&snapshot));
}

void test_badge_meta_same_fingerprint_from_two_scanners_counts_once(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t meta0 = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:CAFEBABE:Meta Glasses",
        "Meta Glasses",
        0.86f,
        -50
    );
    strncpy(meta0.model, "FP:CAFEBABE", sizeof(meta0.model) - 1);
    strncpy(meta0.bssid, "AA:BB:CC:DD:EE:01", sizeof(meta0.bssid) - 1);
    meta0.scanner_slot = 0;
    meta0.scanner_slots_seen = 1;

    drone_detection_t meta1 = meta0;
    strncpy(meta1.bssid, "AA:BB:CC:DD:EE:02", sizeof(meta1.bssid) - 1);
    meta1.rssi = -42;
    meta1.scanner_slot = 1;
    meta1.scanner_slots_seen = 2;

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &meta0, 1000, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &meta1, 1100, NULL));
    badge_threat_state_snapshot(&state, 1300, &snapshot);

    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_snapshot_meta_glasses_count(&snapshot));
    TEST_ASSERT_EQUAL_UINT32(2, snapshot.entities[0].seen_count);
    TEST_ASSERT_EQUAL_INT(-42, snapshot.entities[0].best_rssi);
}

void test_badge_meta_rotated_mac_same_fingerprint_counts_once(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t first = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:1234ABCD:Meta Glasses",
        "Meta Glasses",
        0.84f,
        -55
    );
    strncpy(first.model, "FP:1234ABCD", sizeof(first.model) - 1);
    strncpy(first.bssid, "10:20:30:40:50:60", sizeof(first.bssid) - 1);

    drone_detection_t rotated = first;
    strncpy(rotated.bssid, "10:20:30:40:50:99", sizeof(rotated.bssid) - 1);
    rotated.rssi = -46;

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &first, 1000, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &rotated, 1200, NULL));
    badge_threat_state_snapshot(&state, 1400, &snapshot);

    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_snapshot_meta_glasses_count(&snapshot));
    TEST_ASSERT_EQUAL_UINT32(2, snapshot.entities[0].seen_count);
    TEST_ASSERT_EQUAL_INT(-46, snapshot.entities[0].best_rssi);
}

void test_badge_two_weak_meta_signals_count_as_single_presence(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t weak_a = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "meta:weak:glasses:a",
        "Meta Glasses",
        0.55f,
        -54
    );
    strncpy(weak_a.class_reason, "weak_meta:glasses_detector",
            sizeof(weak_a.class_reason) - 1);

    drone_detection_t weak_b = weak_a;
    strncpy(weak_b.drone_id, "meta:weak:glasses:b",
            sizeof(weak_b.drone_id) - 1);
    weak_b.rssi = -47;

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &weak_a, 1000, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &weak_b, 1100, NULL));
    badge_threat_state_snapshot(&state, 1300, &snapshot);

    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_snapshot_meta_glasses_count(&snapshot));
    TEST_ASSERT_EQUAL_STRING("Meta Glasses", snapshot.entities[0].label);
    TEST_ASSERT_EQUAL_UINT32(0, strlen(snapshot.entities[0].display_id));
    TEST_ASSERT_EQUAL_INT(-47, snapshot.entities[0].best_rssi);
}

void test_badge_strong_meta_plus_weak_duplicate_counts_once(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t strong = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:CAFE1234:Meta Glasses",
        "Meta Glasses",
        0.86f,
        -46
    );
    strncpy(strong.model, "FP:CAFE1234", sizeof(strong.model) - 1);

    drone_detection_t weak = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "meta:weak:glasses",
        "Meta Glasses",
        0.55f,
        -42
    );
    strncpy(weak.class_reason, "weak_meta:glasses_detector",
            sizeof(weak.class_reason) - 1);

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &strong, 1000, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &weak, 1100, NULL));
    badge_threat_state_snapshot(&state, 1300, &snapshot);

    TEST_ASSERT_EQUAL_INT(2, snapshot.entity_count);
    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_snapshot_meta_glasses_count(&snapshot));
    TEST_ASSERT_TRUE(badge_threat_snapshot_best_meta_glasses(&snapshot)->display_id[0] != '\0');
}

void test_badge_weak_meta_presence_expires_after_live_window(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t weak = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "meta:weak:glasses",
        "Meta Glasses",
        0.55f,
        -48
    );
    strncpy(weak.class_reason, "weak_meta:glasses_detector",
            sizeof(weak.class_reason) - 1);

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &weak, 1000, NULL));
    badge_threat_state_snapshot(&state, 1000 + 80000, &snapshot);

    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_FALSE(snapshot.entities[0].stale);
    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_snapshot_meta_glasses_count(&snapshot));

    badge_threat_state_snapshot(&state, 1000 + 90001, &snapshot);

    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_TRUE(snapshot.entities[0].stale);
    TEST_ASSERT_EQUAL_UINT32(0, badge_threat_snapshot_meta_glasses_count(&snapshot));
}

void test_badge_three_meta_fingerprints_count_three(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    const char *hashes[3] = {"FP:11112222", "FP:33334444", "FP:55556666"};
    for (int i = 0; i < 3; i++) {
        drone_detection_t meta = make_detection(
            DETECTION_SRC_BLE_FINGERPRINT,
            i == 0 ? "BLE:11112222:Meta Glasses" :
            i == 1 ? "BLE:33334444:Meta Glasses" :
                     "BLE:55556666:Meta Glasses",
            "Meta Glasses",
            0.82f,
            -48 - i
        );
        strncpy(meta.model, hashes[i], sizeof(meta.model) - 1);
        TEST_ASSERT_TRUE(badge_threat_state_ingest(
            &state,
            &meta,
            1000 + i * 100,
            NULL
        ));
    }
    badge_threat_state_snapshot(&state, 1400, &snapshot);

    TEST_ASSERT_EQUAL_INT(3, snapshot.entity_count);
    TEST_ASSERT_EQUAL_UINT32(3, badge_threat_snapshot_meta_glasses_count(&snapshot));
}

void test_badge_meta_view_key_uses_display_id_for_distinct_glasses(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    char viewed[1][BADGE_THREAT_VIEW_KEY_LEN] = {{0}};
    badge_threat_state_init(&state);

    drone_detection_t meta1 = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:AAAA1111:Meta Glasses",
        "Meta Glasses",
        0.85f,
        -44
    );
    strncpy(meta1.bssid, "AA:BB:CC:DD:EE:11", sizeof(meta1.bssid) - 1);
    drone_detection_t meta2 = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "BLE:BBBB2222:Meta Glasses",
        "Meta Glasses",
        0.82f,
        -55
    );
    strncpy(meta2.bssid, "AA:BB:CC:DD:EE:22", sizeof(meta2.bssid) - 1);

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &meta1, 1000, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &meta2, 1100, NULL));
    badge_threat_state_snapshot(&state, 1300, &snapshot);

    TEST_ASSERT_EQUAL_INT(2, snapshot.entity_count);
    TEST_ASSERT_TRUE(badge_threat_snapshot_entity_view_key(
        &snapshot.entities[0],
        viewed[0],
        sizeof(viewed[0])
    ));
    TEST_ASSERT_TRUE(strncmp(viewed[0], "META:", 5) == 0);
    TEST_ASSERT_TRUE(badge_threat_snapshot_entity_view_key_seen(
        &snapshot.entities[0],
        viewed,
        1
    ));
    TEST_ASSERT_FALSE(badge_threat_snapshot_entity_view_key_seen(
        &snapshot.entities[1],
        viewed,
        1
    ));
}

void test_badge_meta_proximity_percent_tracks_rssi(void)
{
    badge_threat_snapshot_entity_t far = {
        .active = true,
        .cls = BADGE_THREAT_META,
        .category = BADGE_THREAT_CATEGORY_GLASS,
        .best_rssi = -88,
        .proximity_level = BADGE_THREAT_PROX_PRESENT,
    };
    badge_threat_snapshot_entity_t close = far;
    close.best_rssi = -42;
    close.proximity_level = BADGE_THREAT_PROX_CLOSE;

    uint8_t far_pct = badge_threat_snapshot_entity_proximity_percent(&far);
    uint8_t close_pct = badge_threat_snapshot_entity_proximity_percent(&close);

    TEST_ASSERT_TRUE(far_pct < 20);
    TEST_ASSERT_TRUE(close_pct > 90);
    TEST_ASSERT_NOT_EQUAL(
        badge_threat_proximity_percent_to_rgb565(far_pct),
        badge_threat_proximity_percent_to_rgb565(close_pct)
    );

    close.stale = true;
    TEST_ASSERT_EQUAL_UINT8(0, badge_threat_snapshot_entity_proximity_percent(&close));
}

void test_badge_heat_percent_combines_proximity_and_count(void)
{
    uint8_t single_far = badge_threat_heat_percent(20, 1);
    uint8_t crowded_far = badge_threat_heat_percent(20, 4);
    uint8_t crowded_close = badge_threat_heat_percent(90, 4);

    TEST_ASSERT_EQUAL_UINT8(20, single_far);
    TEST_ASSERT_EQUAL_UINT8(50, crowded_far);
    TEST_ASSERT_TRUE(crowded_far > single_far);
    TEST_ASSERT_EQUAL_UINT8(100, crowded_close);
}

void test_badge_heat_percent_no_live_count_means_no_bar(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, badge_threat_heat_percent(80, 0));
}

void test_badge_marquee_offset_advances_for_long_lower_lane_text(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, badge_threat_marquee_offset(8, 8, 100, 2));
    TEST_ASSERT_EQUAL_UINT32(0, badge_threat_marquee_offset(24, 10, 0, 2));
    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_marquee_offset(24, 10, 2, 2));
    TEST_ASSERT_NOT_EQUAL(0, badge_threat_marquee_offset(24, 10, 6, 2));
    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_marquee_offset(24, 10, 1, 0));
}

void test_badge_viewed_remote_id_suppresses_only_same_drone(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    char viewed[1][BADGE_THREAT_VIEW_KEY_LEN] = {{0}};
    char row_title[24];
    badge_threat_state_init(&state);

    drone_detection_t rid1 = make_detection(
        DETECTION_SRC_BLE_RID,
        "RID-VIEW-ONE",
        "OpenDroneID",
        0.90f,
        -52
    );
    drone_detection_t rid2 = make_detection(
        DETECTION_SRC_WIFI_BEACON,
        "RID-VIEW-TWO",
        "Remote ID",
        0.88f,
        -57
    );

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &rid1, 1000, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &rid2, 1100, NULL));
    badge_threat_state_snapshot(&state, 1300, &snapshot);

    TEST_ASSERT_EQUAL_INT(2, snapshot.entity_count);
    TEST_ASSERT_TRUE(badge_threat_snapshot_entity_view_key(
        &snapshot.entities[0],
        viewed[0],
        sizeof(viewed[0])
    ));
    TEST_ASSERT_TRUE(badge_threat_snapshot_entity_view_key_seen(
        &snapshot.entities[0],
        viewed,
        1
    ));
    TEST_ASSERT_FALSE(badge_threat_snapshot_entity_view_key_seen(
        &snapshot.entities[1],
        viewed,
        1
    ));

    int unviewed_rid = 0;
    const badge_threat_snapshot_entity_t *unviewed = NULL;
    for (int i = 0; i < snapshot.entity_count; i++) {
        const badge_threat_snapshot_entity_t *item = &snapshot.entities[i];
        if (badge_threat_snapshot_entity_is_remote_id_drone(item) &&
            !item->stale &&
            !badge_threat_snapshot_entity_view_key_seen(item, viewed, 1)) {
            unviewed_rid++;
            unviewed = item;
        }
    }
    TEST_ASSERT_EQUAL_INT(1, unviewed_rid);
    TEST_ASSERT_NOT_NULL(unviewed);
    badge_threat_format_drone_entity_title(
        &snapshot,
        unviewed,
        row_title,
        sizeof(row_title)
    );
    TEST_ASSERT_EQUAL_STRING("DRONE #2", row_title);
}

void test_badge_viewed_single_remote_id_leaves_no_unviewed_drone(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    char viewed[1][BADGE_THREAT_VIEW_KEY_LEN] = {{0}};
    badge_threat_state_init(&state);

    drone_detection_t rid = make_detection(
        DETECTION_SRC_BLE_RID,
        "RID-VIEW-ONLY",
        "OpenDroneID",
        0.90f,
        -52
    );

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &rid, 1000, NULL));
    badge_threat_state_snapshot(&state, 1200, &snapshot);
    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_TRUE(badge_threat_snapshot_entity_view_key(
        &snapshot.entities[0],
        viewed[0],
        sizeof(viewed[0])
    ));

    int unviewed_rid = 0;
    for (int i = 0; i < snapshot.entity_count; i++) {
        const badge_threat_snapshot_entity_t *item = &snapshot.entities[i];
        if (badge_threat_snapshot_entity_is_remote_id_drone(item) &&
            !item->stale &&
            !badge_threat_snapshot_entity_view_key_seen(item, viewed, 1)) {
            unviewed_rid++;
        }
    }
    TEST_ASSERT_EQUAL_INT(0, unviewed_rid);
}

void test_badge_view_key_non_rid_uses_category_title_detail(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    char key[BADGE_THREAT_VIEW_KEY_LEN];
    badge_threat_state_init(&state);

    drone_detection_t ssid = make_detection(
        DETECTION_SRC_WIFI_SSID,
        "status:wifi:ssid:1",
        "Drone SSID",
        0.60f,
        -55
    );
    strncpy(ssid.ssid, "DJI-Mini-Live", sizeof(ssid.ssid) - 1);

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &ssid, 1000, NULL));
    badge_threat_state_snapshot(&state, 1200, &snapshot);

    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_TRUE(badge_threat_snapshot_entity_view_key(
        &snapshot.entities[0],
        key,
        sizeof(key)
    ));
    TEST_ASSERT_TRUE(strstr(key, "DRONE SSID") != NULL);
    TEST_ASSERT_TRUE(strstr(key, "ssid DJI-Mini-Live") != NULL);
}

void test_badge_drone_ssid_uses_short_display_lifetime(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t ssid = make_detection(
        DETECTION_SRC_WIFI_SSID,
        "status:wifi:ssid:1",
        "Drone SSID",
        0.60f,
        -55
    );
    strncpy(ssid.ssid, "DJI-Mini-Live", sizeof(ssid.ssid) - 1);

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &ssid, 1000, NULL));
    badge_threat_state_snapshot(&state, 16000, &snapshot);
    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_FALSE(snapshot.entities[0].stale);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.active_counts[BADGE_THREAT_DRONE]);
    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_snapshot_drone_evidence_count(&snapshot));

    badge_threat_state_snapshot(&state, 17001, &snapshot);
    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_TRUE(snapshot.entities[0].stale);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.active_counts[BADGE_THREAT_DRONE]);
    TEST_ASSERT_EQUAL_UINT32(0, badge_threat_snapshot_drone_evidence_count(&snapshot));

    badge_threat_state_snapshot(&state, 47001, &snapshot);
    TEST_ASSERT_EQUAL_INT(0, snapshot.entity_count);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.active_counts[BADGE_THREAT_DRONE]);
    TEST_ASSERT_EQUAL_UINT32(0, badge_threat_snapshot_drone_evidence_count(&snapshot));
}

void test_badge_remote_id_keeps_longer_drone_lifetime(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t rid = make_detection(
        DETECTION_SRC_BLE_RID,
        "RID-LONG",
        "OpenDroneID",
        0.90f,
        -52
    );

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &rid, 1000, NULL));
    badge_threat_state_snapshot(&state, 47001, &snapshot);

    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_FALSE(snapshot.entities[0].stale);
    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_snapshot_count_active(
        &snapshot,
        BADGE_THREAT_DRONE,
        BADGE_THREAT_CATEGORY_DRONE,
        false
    ));
    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_snapshot_drone_evidence_count(&snapshot));
}

void test_badge_remote_id_stale_second_drops_from_active_count(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t rid1 = make_detection(
        DETECTION_SRC_BLE_RID,
        "RID-ACTIVE",
        "OpenDroneID",
        0.92f,
        -50
    );
    drone_detection_t rid2 = make_detection(
        DETECTION_SRC_WIFI_BEACON,
        "RID-STALE",
        "Remote ID",
        0.90f,
        -52
    );

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &rid1, 1000, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &rid2, 1100, NULL));
    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &rid1, 92000, NULL));
    badge_threat_state_snapshot(&state, 92000, &snapshot);

    TEST_ASSERT_EQUAL_INT(2, snapshot.entity_count);
    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_snapshot_count_active(
        &snapshot,
        BADGE_THREAT_DRONE,
        BADGE_THREAT_CATEGORY_DRONE,
        false
    ));
    TEST_ASSERT_EQUAL_UINT32(1, badge_threat_snapshot_drone_evidence_count(&snapshot));
}

void test_badge_status_ssid_freshness_requires_recent_nonempty_value(void)
{
    TEST_ASSERT_TRUE(badge_threat_status_ssid_is_fresh("DJI-Mini-Live", 0));
    TEST_ASSERT_TRUE(badge_threat_status_ssid_is_fresh("DJI-Mini-Live", 30));
    TEST_ASSERT_FALSE(badge_threat_status_ssid_is_fresh("DJI-Mini-Live", 31));
    TEST_ASSERT_FALSE(badge_threat_status_ssid_is_fresh("DJI-Mini-Live", -1));
    TEST_ASSERT_FALSE(badge_threat_status_ssid_is_fresh("", 0));
    TEST_ASSERT_FALSE(badge_threat_status_ssid_is_fresh(NULL, 0));
}

void test_badge_status_notable_wifi_ssid_creates_wifi_entity(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t ssid = make_detection(
        DETECTION_SRC_WIFI_ASSOC,
        "status:wifi:notable:1",
        "Notable SSID",
        0.64f,
        0
    );
    strncpy(ssid.ssid, "DEFCON-FPV-WATCH", sizeof(ssid.ssid) - 1);
    strncpy(ssid.class_reason, "notable ssid", sizeof(ssid.class_reason) - 1);

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &ssid, 1000, NULL));
    badge_threat_state_snapshot(&state, 1200, &snapshot);

    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_EQUAL(BADGE_THREAT_WIFI_ANOMALY, snapshot.entities[0].cls);
    TEST_ASSERT_EQUAL_STRING("DEFCON-FPV-WATCH", snapshot.entities[0].label);
}

void test_badge_status_generic_noise_remains_suppressed(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t ble = make_detection(
        DETECTION_SRC_BLE_FINGERPRINT,
        "status:ble:near:0",
        "Nearby BLE",
        0.18f,
        -42
    );
    strncpy(ble.class_reason, "nearby ble service", sizeof(ble.class_reason) - 1);

    drone_detection_t wifi = make_detection(
        DETECTION_SRC_WIFI_ASSOC,
        "status:wifi:normal:0",
        "WiFi-Assoc",
        0.10f,
        -18
    );

    TEST_ASSERT_FALSE(badge_threat_state_ingest(&state, &ble, 1000, NULL));
    TEST_ASSERT_FALSE(badge_threat_state_ingest(&state, &wifi, 1100, NULL));
    badge_threat_state_snapshot(&state, 1200, &snapshot);

    TEST_ASSERT_EQUAL_INT(0, snapshot.entity_count);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.active_counts[BADGE_THREAT_META]);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.active_counts[BADGE_THREAT_DRONE]);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.active_counts[BADGE_THREAT_WIFI_ANOMALY]);
}

void test_badge_drone_snapshot_preserves_drone_and_operator_coords(void)
{
    badge_threat_state_t state;
    badge_threat_snapshot_t snapshot;
    badge_threat_state_init(&state);

    drone_detection_t drone = make_detection(
        DETECTION_SRC_BLE_RID,
        "RID-COORD",
        "OpenDroneID",
        0.95f,
        -45
    );
    drone.latitude = 37.3341;
    drone.longitude = -122.4452;
    drone.altitude_m = 41.0f;
    drone.operator_lat = 37.3300;
    drone.operator_lon = -122.4400;
    strncpy(drone.operator_id, "OP-123", sizeof(drone.operator_id) - 1);

    TEST_ASSERT_TRUE(badge_threat_state_ingest(&state, &drone, 1000, NULL));
    badge_threat_state_snapshot(&state, 1200, &snapshot);

    TEST_ASSERT_EQUAL_INT(1, snapshot.entity_count);
    TEST_ASSERT_TRUE(snapshot.entities[0].has_location);
    TEST_ASSERT_TRUE(snapshot.entities[0].has_operator_location);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 37.3341, snapshot.entities[0].latitude);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, -122.4452, snapshot.entities[0].longitude);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 37.3300, snapshot.entities[0].operator_lat);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, -122.4400, snapshot.entities[0].operator_lon);
    TEST_ASSERT_EQUAL_STRING("OP-123", snapshot.entities[0].operator_id);
}
