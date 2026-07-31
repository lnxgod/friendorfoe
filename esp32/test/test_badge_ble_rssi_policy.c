#include "unity.h"

#include "badge_ble_rssi_policy.h"

void test_badge_ble_low_effort_rssi_gate_is_exact_minus_50(void)
{
    TEST_ASSERT_FALSE(badge_ble_low_effort_rssi_allowed(-127));
    TEST_ASSERT_FALSE(badge_ble_low_effort_rssi_allowed(-51));
    TEST_ASSERT_TRUE(badge_ble_low_effort_rssi_allowed(-50));
    TEST_ASSERT_TRUE(badge_ble_low_effort_rssi_allowed(-1));
    TEST_ASSERT_FALSE(badge_ble_low_effort_rssi_allowed(0));
    TEST_ASSERT_FALSE(badge_ble_low_effort_rssi_allowed(127));

    TEST_ASSERT_FALSE(
        badge_ble_low_effort_detection_allowed(true, -51));
    TEST_ASSERT_TRUE(
        badge_ble_low_effort_detection_allowed(true, -50));
    TEST_ASSERT_TRUE(
        badge_ble_low_effort_detection_allowed(false, -127));
}
