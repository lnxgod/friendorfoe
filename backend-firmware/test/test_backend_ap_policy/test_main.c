#include <stdbool.h>
#include <stdint.h>

#include <unity.h>

#include "backend_ap_policy.h"
#include "../support/backend_test_main.h"

static backend_ap_input_t ap_input(
    bool config_valid,
    uint32_t config_generation,
    bool backend_connected,
    bool usb_start_requested)
{
    const backend_ap_input_t input = {
        .config_valid = config_valid,
        .config_generation = config_generation,
        .backend_connected = backend_connected,
        .usb_start_requested = usb_start_requested,
    };
    return input;
}

void setUp(void)
{
}

void tearDown(void)
{
}

void test_ap_starts_for_invalid_config_usb_or_five_minute_outage(void)
{
    backend_ap_policy_t policy;
    backend_ap_policy_init(&policy, 0);
    TEST_ASSERT_EQUAL(
        BACKEND_AP_START,
        backend_ap_policy_tick(
            &policy, ap_input(false, 0, false, false), 1));

    backend_ap_policy_init(&policy, 0);
    TEST_ASSERT_EQUAL(
        BACKEND_AP_START,
        backend_ap_policy_tick(
            &policy, ap_input(true, 7, true, true), 2000));

    backend_ap_policy_init(&policy, 0);
    TEST_ASSERT_EQUAL(
        BACKEND_AP_NO_CHANGE,
        backend_ap_policy_tick(
            &policy, ap_input(true, 7, false, false), 299999));
    TEST_ASSERT_EQUAL(
        BACKEND_AP_START,
        backend_ap_policy_tick(
            &policy, ap_input(true, 7, false, false), 300000));
}

void test_ap_stops_only_for_current_generation_success_after_this_ap_start(void)
{
    backend_ap_policy_t policy;
    backend_ap_policy_init(&policy, 0);
    TEST_ASSERT_EQUAL(
        BACKEND_AP_START,
        backend_ap_policy_tick(
            &policy, ap_input(true, 8, true, true), 1000));

    backend_ap_policy_note_backend_success(&policy, 7, 2000);
    TEST_ASSERT_EQUAL(
        BACKEND_AP_NO_CHANGE,
        backend_ap_policy_tick(
            &policy, ap_input(true, 8, false, false), 40000));

    backend_ap_policy_note_backend_success(&policy, 8, 100000);
    TEST_ASSERT_EQUAL(
        BACKEND_AP_NO_CHANGE,
        backend_ap_policy_tick(
            &policy, ap_input(true, 8, false, false), 129999));
    TEST_ASSERT_EQUAL(
        BACKEND_AP_STOP,
        backend_ap_policy_tick(
            &policy, ap_input(true, 8, false, false), 130000));
}

void test_ap_rejects_success_recorded_before_manual_start(void)
{
    backend_ap_policy_t policy;
    backend_ap_policy_init(&policy, 0);
    backend_ap_policy_note_backend_success(&policy, 11, 900);

    TEST_ASSERT_EQUAL(
        BACKEND_AP_START,
        backend_ap_policy_tick(
            &policy, ap_input(true, 11, true, true), 1000));
    TEST_ASSERT_EQUAL(
        BACKEND_AP_NO_CHANGE,
        backend_ap_policy_tick(
            &policy, ap_input(true, 11, false, false), 31000));

    backend_ap_policy_note_backend_success(&policy, 11, 31001);
    TEST_ASSERT_EQUAL(
        BACKEND_AP_NO_CHANGE,
        backend_ap_policy_tick(
            &policy, ap_input(true, 11, false, false), 61000));
    TEST_ASSERT_EQUAL(
        BACKEND_AP_STOP,
        backend_ap_policy_tick(
            &policy, ap_input(true, 11, false, false), 61001));
}

void test_config_commit_invalidates_success_and_restarts_outage_clock(void)
{
    backend_ap_policy_t policy;
    backend_ap_policy_init(&policy, 1000);
    backend_ap_policy_note_backend_success(&policy, 4, 2000);

    backend_ap_policy_note_config_commit(&policy, 5, 10000);
    TEST_ASSERT_EQUAL_UINT32(0, policy.last_success_generation);
    TEST_ASSERT_EQUAL_INT64(10000, policy.outage_started_ms);
    TEST_ASSERT_EQUAL(
        BACKEND_AP_NO_CHANGE,
        backend_ap_policy_tick(
            &policy, ap_input(true, 5, false, false), 309999));
    TEST_ASSERT_EQUAL(
        BACKEND_AP_START,
        backend_ap_policy_tick(
            &policy, ap_input(true, 5, false, false), 310000));
}

void test_running_ap_waits_for_post_commit_success_and_exact_grace(void)
{
    backend_ap_policy_t policy;
    backend_ap_policy_init(&policy, 0);
    TEST_ASSERT_EQUAL(
        BACKEND_AP_START,
        backend_ap_policy_tick(
            &policy, ap_input(false, 0, false, false), 1));

    backend_ap_policy_note_config_commit(&policy, 1, 1000);
    backend_ap_policy_note_backend_success(&policy, 0, 1001);
    TEST_ASSERT_EQUAL(
        BACKEND_AP_NO_CHANGE,
        backend_ap_policy_tick(
            &policy, ap_input(true, 1, false, false), 40000));

    backend_ap_policy_note_backend_success(&policy, 1, 50000);
    TEST_ASSERT_EQUAL(
        BACKEND_AP_NO_CHANGE,
        backend_ap_policy_tick(
            &policy, ap_input(true, 1, false, false), 79999));
    TEST_ASSERT_EQUAL(
        BACKEND_AP_STOP,
        backend_ap_policy_tick(
            &policy, ap_input(true, 1, false, false), 80000));
}

void test_invalid_arguments_fail_closed_without_mutating_policy(void)
{
    backend_ap_policy_t policy;
    backend_ap_policy_init(&policy, 50);
    const backend_ap_policy_t before = policy;

    TEST_ASSERT_EQUAL(
        BACKEND_AP_NO_CHANGE,
        backend_ap_policy_tick(NULL, ap_input(false, 0, false, false), 1));
    backend_ap_policy_note_config_commit(NULL, 1, 2);
    backend_ap_policy_note_backend_success(NULL, 1, 2);
    TEST_ASSERT_EQUAL_MEMORY(&before, &policy, sizeof(policy));
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_ap_starts_for_invalid_config_usb_or_five_minute_outage);
    BACKEND_RUN_TEST(
        test_ap_stops_only_for_current_generation_success_after_this_ap_start);
    BACKEND_RUN_TEST(test_ap_rejects_success_recorded_before_manual_start);
    BACKEND_RUN_TEST(
        test_config_commit_invalidates_success_and_restarts_outage_clock);
    BACKEND_RUN_TEST(
        test_running_ap_waits_for_post_commit_success_and_exact_grace);
    BACKEND_RUN_TEST(
        test_invalid_arguments_fail_closed_without_mutating_policy);
    return UNITY_END();
}
