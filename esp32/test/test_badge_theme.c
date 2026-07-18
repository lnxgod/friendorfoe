#include "unity.h"

#include "badge_theme.h"

#include <stdio.h>
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

void test_badge_palettes_drive_distinct_semantic_chrome(void)
{
    badge_theme_t field, neon;
    badge_theme_defaults(&field);
    neon = field;
    snprintf(neon.palette, sizeof(neon.palette), "neon");
    TEST_ASSERT_NOT_EQUAL(
        badge_theme_chrome_color(&field, BADGE_THEME_CHROME_SELECTION),
        badge_theme_chrome_color(&neon, BADGE_THEME_CHROME_SELECTION));
}

void test_badge_chrome_applies_brightness_after_role_lookup(void)
{
    badge_theme_t full, dim;
    badge_theme_defaults(&full);
    dim = full;
    dim.brightness = 50;

    uint16_t full_selection = badge_theme_chrome_color(
        &full, BADGE_THEME_CHROME_SELECTION);
    TEST_ASSERT_EQUAL_UINT16(
        badge_theme_apply_brightness(&dim, full_selection),
        badge_theme_chrome_color(&dim, BADGE_THEME_CHROME_SELECTION));
}

void test_badge_chrome_unknown_palette_falls_back_to_field(void)
{
    badge_theme_t field, unknown;
    badge_theme_defaults(&field);
    unknown = field;
    snprintf(unknown.palette, sizeof(unknown.palette), "unknown");

    for (int role = 0; role < BADGE_THEME_CHROME_ROLE_COUNT; role++) {
        TEST_ASSERT_EQUAL_UINT16(
            badge_theme_chrome_color(&field, (badge_theme_chrome_role_t)role),
            badge_theme_chrome_color(&unknown, (badge_theme_chrome_role_t)role));
    }
}

void test_badge_theme_contrast_floor_protects_dark_chrome(void)
{
    TEST_ASSERT_EQUAL_UINT16(0xFFFF,
                             badge_theme_contrast_floor(0x0000, 0x0000));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF,
                             badge_theme_contrast_floor(0xFFFF, 0x0000));
}
