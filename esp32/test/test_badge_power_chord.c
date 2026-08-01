#include "unity.h"

#include "badge_power_chord.h"

static badge_power_chord_t chord_at_rest(uint32_t now_ms)
{
    badge_power_chord_t chord;
    badge_power_chord_init(&chord, 10000U, false, false, now_ms);
    return chord;
}

void test_badge_power_chord_resets_at_exactly_ten_seconds(void)
{
    badge_power_chord_t chord = chord_at_rest(1000U);

    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 2000U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 11999U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_RESET,
                      badge_power_chord_update(&chord, true, true, true, 12000U));
}

void test_badge_power_chord_release_before_ten_seconds_cancels(void)
{
    badge_power_chord_t chord = chord_at_rest(0U);

    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 10U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 9009U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, false, true, true, 9010U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 9020U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 19019U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_RESET,
                      badge_power_chord_update(&chord, true, true, true, 19020U));
}

void test_badge_power_chord_fires_once_per_hold_and_requires_full_release(void)
{
    badge_power_chord_t chord = chord_at_rest(0U);

    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 10U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_RESET,
                      badge_power_chord_update(&chord, true, true, true, 10010U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 20010U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, false, true, true, 20020U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 30020U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, false, false, true, 30040U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 30100U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_RESET,
                      badge_power_chord_update(&chord, true, true, true, 40100U));
}

void test_badge_power_chord_one_button_never_arms(void)
{
    badge_power_chord_t chord = chord_at_rest(0U);

    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, false, true, 100U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, false, true, 20000U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 20020U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 30019U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_RESET,
                      badge_power_chord_update(&chord, true, true, true, 30020U));
}

void test_badge_power_chord_boot_held_is_ignored_until_release(void)
{
    badge_power_chord_t chord;
    badge_power_chord_init(&chord, 10000U, true, true, 0U);

    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 20000U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, false, false, true, 20020U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 20100U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_RESET,
                      badge_power_chord_update(&chord, true, true, true, 30100U));
}

void test_badge_power_chord_consumed_press_blocks_until_release(void)
{
    badge_power_chord_t chord = chord_at_rest(0U);

    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, false, 100U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 20000U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, false, false, true, 20020U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 20100U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_RESET,
                      badge_power_chord_update(&chord, true, true, true, 30100U));
}

void test_badge_power_chord_elapsed_time_handles_uint32_wrap(void)
{
    badge_power_chord_t chord = chord_at_rest(UINT32_MAX - 10000U);

    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true,
                                               UINT32_MAX - 8000U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 1998U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_RESET,
                      badge_power_chord_update(&chord, true, true, true, 1999U));
}

void test_badge_power_chord_dispatch_gate_discards_chord_edges_until_full_release(void)
{
    badge_power_chord_dispatch_gate_t gate;
    badge_power_chord_dispatch_gate_init(&gate, false, false);

    TEST_ASSERT_FALSE(badge_power_chord_dispatch_gate_update(
        &gate, true, false, BADGE_POWER_CHORD_NONE));
    TEST_ASSERT_TRUE(badge_power_chord_dispatch_gate_update(
        &gate, true, true, BADGE_POWER_CHORD_NONE));
    TEST_ASSERT_TRUE(badge_power_chord_dispatch_gate_update(
        &gate, true, true, BADGE_POWER_CHORD_RESET));
    TEST_ASSERT_TRUE(badge_power_chord_dispatch_gate_update(
        &gate, false, true, BADGE_POWER_CHORD_NONE));
    TEST_ASSERT_TRUE(badge_power_chord_dispatch_gate_update(
        &gate, false, false, BADGE_POWER_CHORD_NONE));
    TEST_ASSERT_FALSE(badge_power_chord_dispatch_gate_update(
        &gate, false, true, BADGE_POWER_CHORD_NONE));
}

void test_badge_power_chord_dispatch_gate_boot_hold_requires_release(void)
{
    badge_power_chord_dispatch_gate_t gate;
    badge_power_chord_dispatch_gate_init(&gate, true, true);

    TEST_ASSERT_TRUE(badge_power_chord_dispatch_gate_update(
        &gate, true, true, BADGE_POWER_CHORD_NONE));
    TEST_ASSERT_TRUE(badge_power_chord_dispatch_gate_update(
        &gate, false, false, BADGE_POWER_CHORD_NONE));
    TEST_ASSERT_FALSE(badge_power_chord_dispatch_gate_update(
        &gate, true, false, BADGE_POWER_CHORD_NONE));
}
