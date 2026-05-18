#include "unity.h"

#include "badge_button_gesture.h"

void test_badge_button_gesture_delays_single_until_window(void)
{
    badge_button_gesture_t gesture;
    badge_button_gesture_init(&gesture, 320);

    TEST_ASSERT_EQUAL(BADGE_BUTTON_GESTURE_NONE,
                      badge_button_gesture_note_tap(&gesture, 1000));
    TEST_ASSERT_TRUE(badge_button_gesture_pending_single(&gesture));
    TEST_ASSERT_EQUAL(BADGE_BUTTON_GESTURE_NONE,
                      badge_button_gesture_poll(&gesture, 1200));
    TEST_ASSERT_TRUE(badge_button_gesture_pending_single(&gesture));
    TEST_ASSERT_EQUAL(BADGE_BUTTON_GESTURE_SINGLE,
                      badge_button_gesture_poll(&gesture, 1321));
    TEST_ASSERT_FALSE(badge_button_gesture_pending_single(&gesture));
}

void test_badge_button_gesture_double_tap_cancels_single(void)
{
    badge_button_gesture_t gesture;
    badge_button_gesture_init(&gesture, 320);

    TEST_ASSERT_EQUAL(BADGE_BUTTON_GESTURE_NONE,
                      badge_button_gesture_note_tap(&gesture, 1000));
    TEST_ASSERT_EQUAL(BADGE_BUTTON_GESTURE_DOUBLE,
                      badge_button_gesture_note_tap(&gesture, 1250));
    TEST_ASSERT_FALSE(badge_button_gesture_pending_single(&gesture));
    TEST_ASSERT_EQUAL(BADGE_BUTTON_GESTURE_NONE,
                      badge_button_gesture_poll(&gesture, 1600));
}

void test_badge_button_gesture_long_clears_pending_single(void)
{
    badge_button_gesture_t gesture;
    badge_button_gesture_init(&gesture, 320);

    TEST_ASSERT_EQUAL(BADGE_BUTTON_GESTURE_NONE,
                      badge_button_gesture_note_tap(&gesture, 1000));
    TEST_ASSERT_EQUAL(BADGE_BUTTON_GESTURE_LONG,
                      badge_button_gesture_note_long(&gesture, 1120));
    TEST_ASSERT_FALSE(badge_button_gesture_pending_single(&gesture));
    TEST_ASSERT_EQUAL(BADGE_BUTTON_GESTURE_NONE,
                      badge_button_gesture_poll(&gesture, 1500));
}
