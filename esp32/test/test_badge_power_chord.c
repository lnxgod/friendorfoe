#include "unity.h"

#include "badge_power_chord.h"

static badge_power_chord_t chord_at_rest(uint32_t now_ms)
{
    badge_power_chord_t chord;
    badge_power_chord_init(&chord, 9000U, false, false, now_ms);
    return chord;
}

void test_badge_power_chord_fires_at_exactly_nine_seconds(void)
{
    badge_power_chord_t chord = chord_at_rest(1000U);

    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 2000U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 10999U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_TOGGLE,
                      badge_power_chord_update(&chord, true, true, true, 11000U));
}

void test_badge_power_chord_fires_once_per_hold_and_requires_full_release(void)
{
    badge_power_chord_t chord = chord_at_rest(0U);

    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 10U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_TOGGLE,
                      badge_power_chord_update(&chord, true, true, true, 9010U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 18010U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, false, true, true, 18020U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 27020U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, false, false, true, 27040U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 27100U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_TOGGLE,
                      badge_power_chord_update(&chord, true, true, true, 36100U));
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
                      badge_power_chord_update(&chord, true, true, true, 29019U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_TOGGLE,
                      badge_power_chord_update(&chord, true, true, true, 29020U));
}

void test_badge_power_chord_boot_held_is_ignored_until_release(void)
{
    badge_power_chord_t chord;
    badge_power_chord_init(&chord, 9000U, true, true, 0U);

    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 20000U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, false, false, true, 20020U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 20100U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_TOGGLE,
                      badge_power_chord_update(&chord, true, true, true, 29100U));
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
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_TOGGLE,
                      badge_power_chord_update(&chord, true, true, true, 29100U));
}

void test_badge_power_chord_elapsed_time_handles_uint32_wrap(void)
{
    badge_power_chord_t chord = chord_at_rest(UINT32_MAX - 10000U);

    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true,
                                               UINT32_MAX - 8000U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
                      badge_power_chord_update(&chord, true, true, true, 998U));
    TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_TOGGLE,
                      badge_power_chord_update(&chord, true, true, true, 999U));
}
