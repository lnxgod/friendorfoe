#include "unity.h"

#include "badge_ok_button_policy.h"
#include "badge_power_chord.h"

void test_badge_ok_button_policy_maps_tap_dead_zone_and_hold(void)
{
    badge_ok_button_policy_t policy;
    badge_ok_button_policy_init(&policy, 850U, 6000U, false);

    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_NONE,
        badge_ok_button_policy_update(&policy, true, true, 1000U));
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_DETAIL,
        badge_ok_button_policy_update(&policy, false, true, 1849U));

    badge_ok_button_policy_update(&policy, true, true, 2000U);
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_NONE,
        badge_ok_button_policy_update(&policy, false, true, 2850U));

    badge_ok_button_policy_update(&policy, true, true, 3000U);
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_NONE,
        badge_ok_button_policy_update(&policy, true, true, 8999U));
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_EASTER,
        badge_ok_button_policy_update(&policy, true, true, 9000U));
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_NONE,
        badge_ok_button_policy_update(&policy, true, true, 10000U));
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_NONE,
        badge_ok_button_policy_update(&policy, false, true, 10001U));
}

void test_badge_ok_button_policy_release_at_hold_threshold_dispatches_once(void)
{
    badge_ok_button_policy_t policy;
    badge_ok_button_policy_init(&policy, 850U, 6000U, false);
    badge_ok_button_policy_update(&policy, true, true, 1000U);
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_EASTER,
        badge_ok_button_policy_update(&policy, false, true, 7000U));
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_NONE,
        badge_ok_button_policy_update(&policy, false, true, 7001U));
}

void test_badge_ok_button_policy_suppresses_chord_until_fresh_press(void)
{
    badge_ok_button_policy_t policy;
    badge_ok_button_policy_init(&policy, 850U, 6000U, false);
    badge_ok_button_policy_update(&policy, true, true, 1000U);
    badge_ok_button_policy_update(&policy, true, false, 2000U);
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_NONE,
        badge_ok_button_policy_update(&policy, false, true, 8000U));
    badge_ok_button_policy_update(&policy, true, true, 9000U);
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_DETAIL,
        badge_ok_button_policy_update(&policy, false, true, 9800U));
}

void test_badge_ok_button_policy_ignores_boot_hold_and_handles_wrap(void)
{
    badge_ok_button_policy_t policy;
    badge_ok_button_policy_init(&policy, 850U, 6000U, true);
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_NONE,
        badge_ok_button_policy_update(&policy, true, true, 9000U));
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_NONE,
        badge_ok_button_policy_update(&policy, false, true, 9010U));

    badge_ok_button_policy_update(
        &policy, true, true, UINT32_MAX - 2999U);
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_EASTER,
        badge_ok_button_policy_update(&policy, true, true, 3000U));
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_NONE,
        badge_ok_button_policy_update(&policy, false, true, 3001U));
}

void test_badge_ok_button_policy_preserves_ten_second_chord_priority(void)
{
    badge_ok_button_policy_t policy;
    badge_power_chord_t chord;
    badge_power_chord_dispatch_gate_t gate;
    badge_power_chord_init(&chord, 10000U, false, false, 0U);
    badge_power_chord_dispatch_gate_init(&gate, false, false);
    badge_ok_button_policy_init(&policy, 850U, 6000U, false);

    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
        badge_power_chord_update(&chord, true, true, true, 1000U));
    bool suppress = badge_power_chord_dispatch_gate_update(
        &gate, true, true, BADGE_POWER_CHORD_NONE);
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_NONE,
        badge_ok_button_policy_update(&policy, true, !suppress, 1000U));
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_NONE,
        badge_ok_button_policy_update(&policy, true, false, 7000U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_RESET,
        badge_power_chord_update(&chord, true, true, true, 11000U));
}
