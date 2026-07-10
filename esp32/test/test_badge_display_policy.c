#include "unity.h"

#include "badge_display_policy.h"
#include "badge_threat_policy.h"
#include "detection_types.h"

#include <string.h>

static drone_detection_t policy_det(uint8_t source,
                                    const char *id,
                                    const char *reason,
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
    if (reason) {
        strncpy(det.class_reason, reason, sizeof(det.class_reason) - 1);
    }
    return det;
}

void test_badge_display_policy_json_round_trips_defaults(void)
{
    badge_display_policy_t policy;
    badge_display_policy_t parsed;
    char json[BADGE_DISPLAY_POLICY_JSON_MAX];
    char err[64];

    badge_display_policy_defaults(&policy);
    TEST_ASSERT_GREATER_THAN(0, badge_display_policy_to_json(&policy, json, sizeof(json)));
    TEST_ASSERT_TRUE(badge_display_policy_parse_json(json, &parsed, err, sizeof(err)));
    TEST_ASSERT_EQUAL_UINT32(badge_display_policy_hash(&policy),
                             badge_display_policy_hash(&parsed));
    TEST_ASSERT_EQUAL_STRING("both",
                             badge_display_lane_name(parsed.classes[BADGE_DISPLAY_CLASS_DRONE].lane));
}

void test_badge_display_policy_rejects_invalid_lane(void)
{
    badge_display_policy_t parsed;
    char err[64];
    const char *json =
        "{\"version\":1,\"classes\":{\"meta\":{\"enabled\":true,"
        "\"lane\":\"sideways\",\"min_proximity\":\"present\",\"priority\":95}}}";

    TEST_ASSERT_FALSE(badge_display_policy_parse_json(json, &parsed,
                                                      err, sizeof(err)));
}

void test_badge_display_policy_disabled_beacon_suppresses_normal_detection(void)
{
    badge_display_policy_t policy;
    bool safety = true;
    badge_display_policy_defaults(&policy);
    policy.classes[BADGE_DISPLAY_CLASS_BEACON].enabled = false;
    policy.classes[BADGE_DISPLAY_CLASS_BEACON].lane = BADGE_DISPLAY_LANE_OFF;

    drone_detection_t det = policy_det(DETECTION_SRC_BLE_FINGERPRINT,
                                       "BLE:BEACON",
                                       "estimote ibeacon",
                                       0.55f,
                                       -58);
    TEST_ASSERT_FALSE(badge_display_policy_allows_detection(&policy, &det,
                                                            &safety, NULL));
    TEST_ASSERT_FALSE(safety);
}

void test_badge_display_policy_drone_breaks_through_disabled_filter(void)
{
    badge_display_policy_t policy;
    bool safety = false;
    badge_display_policy_defaults(&policy);
    policy.classes[BADGE_DISPLAY_CLASS_DRONE].enabled = false;
    policy.classes[BADGE_DISPLAY_CLASS_DRONE].lane = BADGE_DISPLAY_LANE_OFF;

    drone_detection_t det = policy_det(DETECTION_SRC_BLE_RID,
                                       "RID-1528",
                                       "Remote ID",
                                       0.92f,
                                       -78);
    TEST_ASSERT_TRUE(badge_display_policy_allows_detection(&policy, &det,
                                                           &safety, NULL));
    TEST_ASSERT_TRUE(safety);
}

void test_badge_display_policy_close_tracker_breaks_through_disabled_filter(void)
{
    badge_display_policy_t policy;
    bool safety = false;
    badge_display_policy_defaults(&policy);
    policy.classes[BADGE_DISPLAY_CLASS_TRACKER].enabled = false;
    policy.classes[BADGE_DISPLAY_CLASS_TRACKER].lane = BADGE_DISPLAY_LANE_OFF;

    drone_detection_t det = policy_det(DETECTION_SRC_BLE_FINGERPRINT,
                                       "BLE:TILE",
                                       "tile tracker",
                                       0.65f,
                                       -55);
    TEST_ASSERT_TRUE(badge_display_policy_allows_detection(&policy, &det,
                                                           &safety, NULL));
    TEST_ASSERT_TRUE(safety);
}

void test_badge_ble_attack_display_policy_defaults_to_both_lanes(void)
{
    badge_display_policy_t policy;
    badge_display_policy_class_t cls = BADGE_DISPLAY_CLASS_SCANNER_STATUS;
    drone_detection_t det = policy_det(DETECTION_SRC_BLE_FINGERPRINT,
                                       "BLE:A1B2C3D4:BLE Spam",
                                       "behavioral:pairing_spam",
                                       0.82f,
                                       -72);
    det.ble_threat_kind = BLE_THREAT_KIND_PAIRING_SPAM;
    det.ble_prompt_family_mask = 0x03;
    det.ble_unique_macs = 7;
    det.ble_observation_count = 11;

    badge_display_policy_defaults(&policy);

    TEST_ASSERT_EQUAL_INT(12, BADGE_DISPLAY_CLASS_SCANNER_STATUS);
    TEST_ASSERT_EQUAL_INT(13, BADGE_DISPLAY_CLASS_BLE_ATTACK);
    TEST_ASSERT_EQUAL_INT(14, BADGE_DISPLAY_POLICY_CLASS_COUNT);
    TEST_ASSERT_EQUAL_STRING(
        "ble_attack",
        badge_display_policy_class_key(BADGE_DISPLAY_CLASS_BLE_ATTACK));
    TEST_ASSERT_TRUE(policy.classes[BADGE_DISPLAY_CLASS_BLE_ATTACK].enabled);
    TEST_ASSERT_EQUAL(BADGE_DISPLAY_LANE_BOTH,
                      policy.classes[BADGE_DISPLAY_CLASS_BLE_ATTACK].lane);
    TEST_ASSERT_EQUAL(BADGE_DISPLAY_PROX_PRESENT,
                      policy.classes[BADGE_DISPLAY_CLASS_BLE_ATTACK].min_proximity);
    TEST_ASSERT_EQUAL_UINT8(92,
                            policy.classes[BADGE_DISPLAY_CLASS_BLE_ATTACK].priority);
    TEST_ASSERT_TRUE(badge_display_policy_allows_detection(&policy, &det, NULL, &cls));
    TEST_ASSERT_EQUAL(BADGE_DISPLAY_CLASS_BLE_ATTACK, cls);
}

void test_badge_snapshot_categories_map_to_display_classes(void)
{
    static const struct {
        badge_threat_category_t category;
        badge_display_policy_class_t expected;
    } cases[] = {
        {BADGE_THREAT_CATEGORY_NONE, BADGE_DISPLAY_CLASS_SCANNER_STATUS},
        {BADGE_THREAT_CATEGORY_DRONE, BADGE_DISPLAY_CLASS_DRONE},
        {BADGE_THREAT_CATEGORY_SSID, BADGE_DISPLAY_CLASS_DRONE},
        {BADGE_THREAT_CATEGORY_FLOCK, BADGE_DISPLAY_CLASS_FLOCK},
        {BADGE_THREAT_CATEGORY_GLASS, BADGE_DISPLAY_CLASS_META},
        {BADGE_THREAT_CATEGORY_SKIM, BADGE_DISPLAY_CLASS_SKIMMER},
        {BADGE_THREAT_CATEGORY_CAMERA, BADGE_DISPLAY_CLASS_CAMERA},
        {BADGE_THREAT_CATEGORY_BEACON, BADGE_DISPLAY_CLASS_BEACON},
        {BADGE_THREAT_CATEGORY_EVENT_BADGE, BADGE_DISPLAY_CLASS_EVENT_BADGE},
        {BADGE_THREAT_CATEGORY_LOCK, BADGE_DISPLAY_CLASS_LOCK},
        {BADGE_THREAT_CATEGORY_HID, BADGE_DISPLAY_CLASS_HID},
        {BADGE_THREAT_CATEGORY_AUDIO, BADGE_DISPLAY_CLASS_AURACAST},
        {BADGE_THREAT_CATEGORY_LISTENING, BADGE_DISPLAY_CLASS_SCANNER_STATUS},
        {BADGE_THREAT_CATEGORY_WIFI, BADGE_DISPLAY_CLASS_WIFI_ATTACK},
        {BADGE_THREAT_CATEGORY_TAG_CLOSE, BADGE_DISPLAY_CLASS_TRACKER},
        {BADGE_THREAT_CATEGORY_PRIVACY, BADGE_DISPLAY_CLASS_SCANNER_STATUS},
        {BADGE_THREAT_CATEGORY_BLE_SPAM, BADGE_DISPLAY_CLASS_BLE_ATTACK},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        TEST_ASSERT_EQUAL(
            cases[i].expected,
            badge_display_policy_class_for_threat_snapshot(
                cases[i].category,
                BADGE_THREAT_OTHER));
    }

    TEST_ASSERT_EQUAL(
        BADGE_DISPLAY_CLASS_DRONE,
        badge_display_policy_class_for_threat_snapshot(
            BADGE_THREAT_CATEGORY_NONE,
            BADGE_THREAT_DRONE));
    TEST_ASSERT_EQUAL(
        BADGE_DISPLAY_CLASS_META,
        badge_display_policy_class_for_threat_snapshot(
            BADGE_THREAT_CATEGORY_NONE,
            BADGE_THREAT_META));
    TEST_ASSERT_EQUAL(
        BADGE_DISPLAY_CLASS_TRACKER,
        badge_display_policy_class_for_threat_snapshot(
            BADGE_THREAT_CATEGORY_NONE,
            BADGE_THREAT_TRACKER));
    TEST_ASSERT_EQUAL(
        BADGE_DISPLAY_CLASS_WIFI_ATTACK,
        badge_display_policy_class_for_threat_snapshot(
            BADGE_THREAT_CATEGORY_NONE,
            BADGE_THREAT_WIFI_ANOMALY));
}
