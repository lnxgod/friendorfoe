#include "unity.h"

#include "badge_theme.h"

#include <string.h>

void test_badge_theme_json_round_trips_defaults(void)
{
    badge_theme_t theme;
    badge_theme_t parsed;
    char json[BADGE_THEME_JSON_MAX];
    char err[64];

    badge_theme_defaults(&theme);
    TEST_ASSERT_GREATER_THAN(0, badge_theme_to_json(&theme, json, sizeof(json)));
    TEST_ASSERT_TRUE(badge_theme_parse_json(json, &parsed, err, sizeof(err)));
    TEST_ASSERT_EQUAL_UINT32(badge_theme_hash(&theme),
                             badge_theme_hash(&parsed));
    TEST_ASSERT_EQUAL_STRING("field", parsed.palette);
    TEST_ASSERT_EQUAL_UINT16(theme.accents[BADGE_THEME_ACCENT_DRONE],
                             parsed.accents[BADGE_THEME_ACCENT_DRONE]);
}

void test_badge_theme_parses_safe_custom_accents(void)
{
    badge_theme_t theme;
    char err[64];
    const char *json =
        "{\"version\":1,\"palette\":\"night\",\"background\":\"scanline\","
        "\"brightness\":75,\"accents\":{\"meta\":63488,\"flock\":2016}}";

    TEST_ASSERT_TRUE(badge_theme_parse_json(json, &theme, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("night", theme.palette);
    TEST_ASSERT_EQUAL_STRING("scanline", theme.background);
    TEST_ASSERT_EQUAL_UINT8(75, theme.brightness);
    TEST_ASSERT_EQUAL_UINT16(63488, theme.accents[BADGE_THEME_ACCENT_META]);
    TEST_ASSERT_EQUAL_UINT16(2016, theme.accents[BADGE_THEME_ACCENT_FLOCK]);
}

void test_badge_theme_rejects_unsafe_values(void)
{
    badge_theme_t theme;
    char err[64];

    TEST_ASSERT_FALSE(badge_theme_parse_json(
        "{\"version\":1,\"palette\":\"wild\",\"brightness\":100}",
        &theme, err, sizeof(err)));
    TEST_ASSERT_FALSE(badge_theme_parse_json(
        "{\"version\":1,\"brightness\":10}",
        &theme, err, sizeof(err)));
    TEST_ASSERT_FALSE(badge_theme_parse_json(
        "{\"version\":1,\"accents\":{\"drone\":999999}}",
        &theme, err, sizeof(err)));
}

void test_badge_theme_brightness_scales_color(void)
{
    badge_theme_t theme;
    badge_theme_defaults(&theme);
    theme.brightness = 50;
    theme.accents[BADGE_THEME_ACCENT_META] = 0xF800;

    TEST_ASSERT_LESS_THAN(0xF800,
                          badge_theme_apply_brightness(&theme, 0xF800));
    TEST_ASSERT_EQUAL_UINT16(badge_theme_apply_brightness(&theme, 0xF800),
                             badge_theme_accent_color(&theme,
                                                      BADGE_THEME_ACCENT_META));
}
