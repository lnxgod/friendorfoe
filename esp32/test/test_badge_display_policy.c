#include "unity.h"

#include "badge_display_policy.h"
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
