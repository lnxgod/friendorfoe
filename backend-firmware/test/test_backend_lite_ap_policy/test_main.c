#include <unity.h>

#include "backend_lite_ap_policy.h"
#include "../support/backend_test_main.h"

void setUp(void)
{
}

void tearDown(void)
{
}

void test_unconfigured_wifi_starts_recovery_ap(void)
{
    backend_lite_ap_policy_t policy;
    backend_lite_ap_policy_init(&policy);
    const backend_lite_ap_input_t input = {
        .wifi_configured = false,
        .wifi_connected = false,
        .wifi_join_failed = false,
        .usb_live_confirmed = false,
    };

    TEST_ASSERT_EQUAL(BACKEND_AP_START,
        backend_lite_ap_policy_tick(&policy, input));
    TEST_ASSERT_TRUE(policy.running);
    TEST_ASSERT_EQUAL(BACKEND_LITE_AP_REASON_WIFI_UNCONFIGURED,
        backend_lite_ap_policy_reason(&policy));
}

void test_initial_configured_join_pass_does_not_start_recovery_ap(void)
{
    backend_lite_ap_policy_t policy;
    backend_lite_ap_policy_init(&policy);
    const backend_lite_ap_input_t input = {
        .wifi_configured = true,
        .wifi_connected = false,
        .wifi_join_failed = false,
        .usb_live_confirmed = false,
    };

    TEST_ASSERT_EQUAL(BACKEND_AP_NO_CHANGE,
        backend_lite_ap_policy_tick(&policy, input));
    TEST_ASSERT_FALSE(policy.running);
    TEST_ASSERT_EQUAL(BACKEND_LITE_AP_REASON_NONE,
        backend_lite_ap_policy_reason(&policy));
}

void test_completed_failed_join_pass_starts_recovery_ap(void)
{
    backend_lite_ap_policy_t policy;
    backend_lite_ap_policy_init(&policy);
    const backend_lite_ap_input_t input = {
        .wifi_configured = true,
        .wifi_connected = false,
        .wifi_join_failed = true,
        .usb_live_confirmed = false,
    };

    TEST_ASSERT_EQUAL(BACKEND_AP_START,
        backend_lite_ap_policy_tick(&policy, input));
    TEST_ASSERT_EQUAL(BACKEND_LITE_AP_REASON_WIFI_JOIN_FAILED,
        backend_lite_ap_policy_reason(&policy));
}

void test_wifi_connection_stops_recovery_ap_immediately(void)
{
    backend_lite_ap_policy_t policy;
    backend_lite_ap_policy_init(&policy);
    backend_lite_ap_input_t input = {
        .wifi_configured = false,
        .wifi_connected = false,
        .wifi_join_failed = false,
        .usb_live_confirmed = false,
    };

    TEST_ASSERT_EQUAL(BACKEND_AP_START,
        backend_lite_ap_policy_tick(&policy, input));
    input.wifi_connected = true;
    TEST_ASSERT_EQUAL(BACKEND_AP_STOP,
        backend_lite_ap_policy_tick(&policy, input));
    TEST_ASSERT_EQUAL(BACKEND_LITE_AP_REASON_WIFI_UNCONFIGURED,
        backend_lite_ap_policy_reason(&policy));
}

void test_only_confirmed_usb_or_wifi_suppresses_recovery(void)
{
    backend_lite_ap_policy_t policy;
    backend_lite_ap_policy_init(&policy);
    backend_lite_ap_input_t input = {
        .wifi_configured = false,
        .wifi_connected = false,
        .wifi_join_failed = false,
        .usb_live_confirmed = false,
    };
    TEST_ASSERT_EQUAL(BACKEND_AP_START,
        backend_lite_ap_policy_tick(&policy, input));
    input.usb_live_confirmed = true;
    TEST_ASSERT_EQUAL(BACKEND_AP_STOP,
        backend_lite_ap_policy_tick(&policy, input));
    TEST_ASSERT_EQUAL(BACKEND_LITE_AP_REASON_WIFI_UNCONFIGURED,
        backend_lite_ap_policy_reason(&policy));
    input.usb_live_confirmed = false;
    TEST_ASSERT_EQUAL(BACKEND_AP_START,
        backend_lite_ap_policy_tick(&policy, input));
}

void test_sta_use_requires_saved_network_manager_and_connection(void)
{
    TEST_ASSERT_FALSE(backend_lite_network_can_use_sta(0, true, true));
    TEST_ASSERT_FALSE(backend_lite_network_can_use_sta(1, false, true));
    TEST_ASSERT_FALSE(backend_lite_network_can_use_sta(1, true, false));
    TEST_ASSERT_TRUE(backend_lite_network_can_use_sta(1, true, true));
    TEST_ASSERT_TRUE(backend_lite_network_can_use_sta(4, true, true));
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(test_unconfigured_wifi_starts_recovery_ap);
    BACKEND_RUN_TEST(test_initial_configured_join_pass_does_not_start_recovery_ap);
    BACKEND_RUN_TEST(test_completed_failed_join_pass_starts_recovery_ap);
    BACKEND_RUN_TEST(test_wifi_connection_stops_recovery_ap_immediately);
    BACKEND_RUN_TEST(test_only_confirmed_usb_or_wifi_suppresses_recovery);
    BACKEND_RUN_TEST(test_sta_use_requires_saved_network_manager_and_connection);
    return UNITY_END();
}
