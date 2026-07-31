#include "unity.h"

/* This translation unit proves the badge build identity, not host defaults. */
#define UPLINK_BOARD 1
#define FOF_BADGE_VARIANT 1
#include "version.h"
#include "uplink_ota_policy.h"

void test_uplink_ota_badge_identity_constants_match_version_contract(void)
{
    TEST_ASSERT_EQUAL_STRING(FOF_FIRMWARE_TARGET, UPLINK_OTA_TARGET);
    TEST_ASSERT_EQUAL_STRING(FOF_APP_PROJECT, UPLINK_OTA_PROJECT);
    TEST_ASSERT_EQUAL_STRING(FOF_HARDWARE_TYPE, UPLINK_OTA_HARDWARE);
    TEST_ASSERT_EQUAL_UINT32(1024U, UPLINK_OTA_MIN_IMAGE_BYTES);
    TEST_ASSERT_EQUAL_UINT32(4096U, UPLINK_OTA_CREDIT_BYTES);
}
