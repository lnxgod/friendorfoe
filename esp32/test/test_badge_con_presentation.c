#include "unity.h"

#include "badge_con_presentation.h"

void test_badge_con_presentation_selects_every_game_state(void)
{
    TEST_ASSERT_EQUAL(
        BADGE_CON_PRESENT_INACTIVE,
        badge_con_presentation_select(NULL));

    badge_con_snapshot_t snapshot = {
        .seed = BADGE_CON_ROLE_NORMAL,
        .role = BADGE_CON_ROLE_NORMAL,
        .active = false,
        .shield = 30U,
        .maximum = 100U,
    };
    TEST_ASSERT_EQUAL(
        BADGE_CON_PRESENT_INACTIVE,
        badge_con_presentation_select(&snapshot));

    snapshot.active = true;
    TEST_ASSERT_EQUAL(
        BADGE_CON_PRESENT_HUMAN,
        badge_con_presentation_select(&snapshot));

    snapshot.cured = true;
    snapshot.scar_level = 1U;
    snapshot.maximum = 50U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_PRESENT_HUMAN,
        badge_con_presentation_select(&snapshot));

    snapshot.cured = false;
    snapshot.role = BADGE_CON_ROLE_INFECTED;
    TEST_ASSERT_EQUAL(
        BADGE_CON_PRESENT_INFECTED,
        badge_con_presentation_select(&snapshot));

    snapshot.role = BADGE_CON_ROLE_IMMUNE;
    TEST_ASSERT_EQUAL(
        BADGE_CON_PRESENT_IMMUNE,
        badge_con_presentation_select(&snapshot));

    snapshot.role = BADGE_CON_ROLE_INFECTED;
    snapshot.super = true;
    TEST_ASSERT_EQUAL(
        BADGE_CON_PRESENT_SUPER,
        badge_con_presentation_select(&snapshot));

    snapshot.super = false;
    snapshot.dead = true;
    TEST_ASSERT_EQUAL(
        BADGE_CON_PRESENT_DEAD,
        badge_con_presentation_select(&snapshot));

    snapshot.super = true;
    TEST_ASSERT_EQUAL(
        BADGE_CON_PRESENT_DEAD_SUPER,
        badge_con_presentation_select(&snapshot));
}

void test_badge_con_presentation_game_led_obeys_system_priority(void)
{
    TEST_ASSERT_TRUE(badge_con_presentation_game_led_allowed(
        false, false, true, false));
    TEST_ASSERT_FALSE(badge_con_presentation_game_led_allowed(
        true, false, true, false));
    TEST_ASSERT_FALSE(badge_con_presentation_game_led_allowed(
        false, true, true, false));
    TEST_ASSERT_FALSE(badge_con_presentation_game_led_allowed(
        false, false, false, false));
    TEST_ASSERT_FALSE(badge_con_presentation_game_led_allowed(
        false, false, true, true));
    TEST_ASSERT_FALSE(badge_con_presentation_game_led_allowed(
        true, true, false, true));
}

void test_badge_con_presentation_maps_live_hud_values_and_colors(void)
{
    badge_con_snapshot_t snapshot = {
        .role = BADGE_CON_ROLE_NORMAL,
        .active = true,
        .shield = 49U,
        .maximum = 50U,
    };

    badge_con_hud_plan_t hud = badge_con_presentation_hud(&snapshot);
    TEST_ASSERT_TRUE(hud.visible);
    TEST_ASSERT_EQUAL(BADGE_CON_PRESENT_HUMAN, hud.state);
    TEST_ASSERT_EQUAL_HEX16(BADGE_CON_HUD_HUMAN_RGB565, hud.color_rgb565);
    TEST_ASSERT_EQUAL_UINT8(49U, hud.current);
    TEST_ASSERT_EQUAL_UINT8(50U, hud.maximum);

    snapshot.cured = true;
    hud = badge_con_presentation_hud(&snapshot);
    TEST_ASSERT_EQUAL(BADGE_CON_PRESENT_HUMAN, hud.state);
    TEST_ASSERT_EQUAL_HEX16(BADGE_CON_HUD_HUMAN_RGB565, hud.color_rgb565);

    snapshot.role = BADGE_CON_ROLE_INFECTED;
    snapshot.shield = 45U;
    snapshot.maximum = 25U;
    hud = badge_con_presentation_hud(&snapshot);
    TEST_ASSERT_EQUAL(BADGE_CON_PRESENT_INFECTED, hud.state);
    TEST_ASSERT_EQUAL_HEX16(BADGE_CON_HUD_INFECTED_RGB565, hud.color_rgb565);
    TEST_ASSERT_EQUAL_UINT8(45U, hud.current);
    TEST_ASSERT_EQUAL_UINT8(100U, hud.maximum);

    snapshot.super = true;
    hud = badge_con_presentation_hud(&snapshot);
    TEST_ASSERT_EQUAL(BADGE_CON_PRESENT_SUPER, hud.state);
    TEST_ASSERT_EQUAL_HEX16(BADGE_CON_HUD_INFECTED_RGB565, hud.color_rgb565);

    snapshot.super = false;
    snapshot.role = BADGE_CON_ROLE_IMMUNE;
    snapshot.shield = 91U;
    snapshot.maximum = 100U;
    hud = badge_con_presentation_hud(&snapshot);
    TEST_ASSERT_EQUAL(BADGE_CON_PRESENT_IMMUNE, hud.state);
    TEST_ASSERT_EQUAL_HEX16(BADGE_CON_HUD_HEALER_RGB565, hud.color_rgb565);
}

void test_badge_con_presentation_hides_inactive_and_dead_hud(void)
{
    badge_con_hud_plan_t hud = badge_con_presentation_hud(NULL);
    TEST_ASSERT_FALSE(hud.visible);

    badge_con_snapshot_t snapshot = {
        .role = BADGE_CON_ROLE_NORMAL,
        .active = false,
        .shield = 100U,
        .maximum = 100U,
    };
    hud = badge_con_presentation_hud(&snapshot);
    TEST_ASSERT_FALSE(hud.visible);

    snapshot.active = true;
    snapshot.dead = true;
    hud = badge_con_presentation_hud(&snapshot);
    TEST_ASSERT_FALSE(hud.visible);

    snapshot.super = true;
    hud = badge_con_presentation_hud(&snapshot);
    TEST_ASSERT_FALSE(hud.visible);
}
