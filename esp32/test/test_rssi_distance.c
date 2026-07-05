#include "unity.h"
#include "rssi_distance.h"

void test_rssi_distance_uses_field_calibrated_scale(void)
{
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 1.0, rssi_distance_estimate_m(-55));
    TEST_ASSERT_DOUBLE_WITHIN(0.35, 10.0, rssi_distance_estimate_m(-78));
    TEST_ASSERT_DOUBLE_WITHIN(4.0, 100.0, rssi_distance_estimate_m(-101));
}
