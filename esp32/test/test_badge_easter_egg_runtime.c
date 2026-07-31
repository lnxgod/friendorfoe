#include "unity.h"

#include <stdbool.h>
#include <stdint.h>

static unsigned s_easter_runtime_critical_depth;
static unsigned s_easter_runtime_activation_calls;
static bool s_easter_runtime_activation_ok;
static bool s_easter_runtime_activation_in_critical;

static void easter_runtime_enter_critical(void)
{
    s_easter_runtime_critical_depth++;
}

static void easter_runtime_exit_critical(void)
{
    TEST_ASSERT_GREATER_THAN_UINT(0U, s_easter_runtime_critical_depth);
    s_easter_runtime_critical_depth--;
}

#define FOF_BADGE_VARIANT 1
#define FOF_DC34_GAME_CANARY 1
#define portMUX_TYPE int
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) do { \
    (void)(lock); \
    easter_runtime_enter_critical(); \
} while (0)
#define portEXIT_CRITICAL(lock) do { \
    (void)(lock); \
    easter_runtime_exit_critical(); \
} while (0)

static bool easter_runtime_activate_game(void);
#define BADGE_EASTER_GAME_ACTIVATE_FN easter_runtime_activate_game

#include "../uplink/main/core/badge_easter_egg_runtime.c"

static bool easter_runtime_activate_game(void)
{
    s_easter_runtime_activation_calls++;
    s_easter_runtime_activation_in_critical =
        s_easter_runtime_activation_in_critical ||
        s_easter_runtime_critical_depth != 0U;
    return s_easter_runtime_activation_ok;
}

static void easter_runtime_reset(void)
{
    s_easter_runtime_critical_depth = 0U;
    s_easter_runtime_activation_calls = 0U;
    s_easter_runtime_activation_ok = true;
    s_easter_runtime_activation_in_critical = false;
    badge_easter_egg_runtime_init();
}

void test_badge_easter_runtime_activates_only_on_terminal_advance(void)
{
    easter_runtime_reset();

    TEST_ASSERT_TRUE(badge_easter_egg_runtime_trigger(
        BADGE_EASTER_EGG_SOURCE_WIFI_SSID));
    TEST_ASSERT_EQUAL_UINT(0U, s_easter_runtime_activation_calls);

    TEST_ASSERT_TRUE(badge_easter_egg_runtime_advance());
    TEST_ASSERT_EQUAL_UINT(0U, s_easter_runtime_activation_calls);

    TEST_ASSERT_TRUE(badge_easter_egg_runtime_advance());
    TEST_ASSERT_EQUAL_UINT(1U, s_easter_runtime_activation_calls);
    TEST_ASSERT_FALSE(s_easter_runtime_activation_in_critical);

    TEST_ASSERT_FALSE(badge_easter_egg_runtime_dismiss());
    TEST_ASSERT_EQUAL_UINT(1U, s_easter_runtime_activation_calls);
}

void test_badge_easter_runtime_direct_dismiss_activates_exactly_once(void)
{
    easter_runtime_reset();

    TEST_ASSERT_TRUE(badge_easter_egg_runtime_trigger(
        BADGE_EASTER_EGG_SOURCE_BLE_REMOTE_ID));
    TEST_ASSERT_TRUE(badge_easter_egg_runtime_dismiss());
    TEST_ASSERT_EQUAL_UINT(1U, s_easter_runtime_activation_calls);
    TEST_ASSERT_FALSE(s_easter_runtime_activation_in_critical);

    TEST_ASSERT_FALSE(badge_easter_egg_runtime_dismiss());
    TEST_ASSERT_FALSE(badge_easter_egg_runtime_advance());
    TEST_ASSERT_EQUAL_UINT(1U, s_easter_runtime_activation_calls);
}

void test_badge_easter_runtime_failed_game_commit_is_not_retried(void)
{
    easter_runtime_reset();
    s_easter_runtime_activation_ok = false;

    TEST_ASSERT_TRUE(badge_easter_egg_runtime_trigger(
        BADGE_EASTER_EGG_SOURCE_BUTTON));
    TEST_ASSERT_TRUE(badge_easter_egg_runtime_dismiss());
    TEST_ASSERT_EQUAL_UINT(1U, s_easter_runtime_activation_calls);
    TEST_ASSERT_FALSE(s_easter_runtime_activation_in_critical);

    TEST_ASSERT_FALSE(badge_easter_egg_runtime_dismiss());
    TEST_ASSERT_FALSE(badge_easter_egg_runtime_advance());
    TEST_ASSERT_EQUAL_UINT(1U, s_easter_runtime_activation_calls);
}
