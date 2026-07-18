#include "unity.h"

#include "badge_display_contract.h"

void test_badge_display_contract_keeps_four_fixed_lanes(void)
{
    TEST_ASSERT_EQUAL_INT(4, badge_display_contract_focus_capacity());
    TEST_ASSERT_EQUAL_INT(148, badge_display_contract_health_strip_y());
    TEST_ASSERT_EQUAL(BADGE_LANE_GLOBAL_1, badge_display_contract_lane(0));
    TEST_ASSERT_EQUAL(BADGE_LANE_GLOBAL_2, badge_display_contract_lane(1));
    TEST_ASSERT_EQUAL(BADGE_LANE_BLE, badge_display_contract_lane(2));
    TEST_ASSERT_EQUAL(BADGE_LANE_WIFI, badge_display_contract_lane(3));

    TEST_ASSERT_EQUAL_INT(0, badge_display_contract_lane_y(0));
    TEST_ASSERT_EQUAL_INT(39, badge_display_contract_lane_y(1));
    TEST_ASSERT_EQUAL_INT(78, badge_display_contract_lane_y(2));
    TEST_ASSERT_EQUAL_INT(113, badge_display_contract_lane_y(3));
    TEST_ASSERT_EQUAL_INT(37, badge_display_contract_lane_height(0));
    TEST_ASSERT_EQUAL_INT(37, badge_display_contract_lane_height(1));
    TEST_ASSERT_EQUAL_INT(34, badge_display_contract_lane_height(2));
    TEST_ASSERT_EQUAL_INT(34, badge_display_contract_lane_height(3));

    TEST_ASSERT_EQUAL(BADGE_LANE_INVALID, badge_display_contract_lane(-1));
    TEST_ASSERT_EQUAL(BADGE_LANE_INVALID, badge_display_contract_lane(4));
}
