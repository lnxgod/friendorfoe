#include "unity.h"

#include "badge_theme.h"

#include <stdio.h>
#include <string.h>

static const char VALID_CUSTOM_THEME_JSON[] =
    "{\"version\":1,\"palette\":\"night\",\"background\":\"scanline\","
    "\"brightness\":75,\"accents\":{\"drone\":65184,\"meta\":63488,"
    "\"tracker\":63519,\"flock\":2016,\"wifi_attack\":2047,\"clear\":12133}}";

static void assert_theme_rejected_atomically(const char *json)
{
    badge_theme_t before;
    badge_theme_t after;
    char err[64] = {0};

    memset(&before, 0xa5, sizeof(before));
    after = before;
    TEST_ASSERT_FALSE(badge_theme_parse_json(json, &after, err, sizeof(err)));
    TEST_ASSERT_EQUAL_MEMORY(&before, &after, sizeof(before));
    TEST_ASSERT_NOT_EQUAL('\0', err[0]);
}

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

    TEST_ASSERT_TRUE(badge_theme_parse_json(VALID_CUSTOM_THEME_JSON, &theme,
                                            err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("night", theme.palette);
    TEST_ASSERT_EQUAL_STRING("scanline", theme.background);
    TEST_ASSERT_EQUAL_UINT8(75, theme.brightness);
    TEST_ASSERT_EQUAL_UINT16(65184, theme.accents[BADGE_THEME_ACCENT_DRONE]);
    TEST_ASSERT_EQUAL_UINT16(63488, theme.accents[BADGE_THEME_ACCENT_META]);
    TEST_ASSERT_EQUAL_UINT16(63519, theme.accents[BADGE_THEME_ACCENT_TRACKER]);
    TEST_ASSERT_EQUAL_UINT16(2016, theme.accents[BADGE_THEME_ACCENT_FLOCK]);
    TEST_ASSERT_EQUAL_UINT16(2047, theme.accents[BADGE_THEME_ACCENT_WIFI_ATTACK]);
    TEST_ASSERT_EQUAL_UINT16(12133, theme.accents[BADGE_THEME_ACCENT_CLEAR]);
}

void test_badge_theme_span_honors_explicit_length_and_is_atomic(void)
{
    badge_theme_t before;
    badge_theme_t parsed;
    char err[64] = {0};

    memset(&before, 0xa5, sizeof(before));
    parsed = before;
    TEST_ASSERT_TRUE(badge_theme_parse_json_span(
        (const uint8_t *)VALID_CUSTOM_THEME_JSON,
        sizeof(VALID_CUSTOM_THEME_JSON) - 1U,
        &parsed, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("night", parsed.palette);
    TEST_ASSERT_EQUAL_UINT8(75, parsed.brightness);

    static const uint8_t embedded_nul[] =
        "{\"version\":1,\"palette\":\"field\",\"background\":\"dark\","
        "\"brightness\":100,\"accents\":{\"drone\":65184,\"meta\":63539,"
        "\"tracker\":63519,\"flock\":43039,\"wifi_attack\":2047,"
        "\"clear\":12133}}\0{}";
    parsed = before;
    TEST_ASSERT_FALSE(badge_theme_parse_json_span(
        embedded_nul, sizeof(embedded_nul) - 1U,
        &parsed, err, sizeof(err)));
    TEST_ASSERT_EQUAL_MEMORY(&before, &parsed, sizeof(before));
}

void test_badge_theme_parser_rejects_case_drift_atomically(void)
{
    assert_theme_rejected_atomically(
        "{\"version\":1,\"palette\":\"NEON\",\"background\":\"dark\","
        "\"brightness\":100,\"accents\":{\"drone\":65184,\"meta\":63539,"
        "\"tracker\":63519,\"flock\":43039,\"wifi_attack\":2047,"
        "\"clear\":12133}}");

    static const uint8_t upper_background[] =
        "{\"version\":1,\"palette\":\"field\",\"background\":\"DARK\","
        "\"brightness\":100,\"accents\":{\"drone\":65184,\"meta\":63539,"
        "\"tracker\":63519,\"flock\":43039,\"wifi_attack\":2047,"
        "\"clear\":12133}}";
    badge_theme_t before;
    badge_theme_t after;
    char err[64] = {0};
    memset(&before, 0xa5, sizeof(before));
    after = before;
    TEST_ASSERT_FALSE(badge_theme_parse_json_span(
        upper_background, sizeof(upper_background) - 1U,
        &after, err, sizeof(err)));
    TEST_ASSERT_EQUAL_MEMORY(&before, &after, sizeof(before));
}
void test_badge_theme_requires_complete_schema_without_mutating_output(void)
{
    assert_theme_rejected_atomically(
        "{\"palette\":\"field\",\"background\":\"dark\",\"brightness\":100,"
        "\"accents\":{\"drone\":65184,\"meta\":63539,\"tracker\":63519,"
        "\"flock\":43039,\"wifi_attack\":2047,\"clear\":12133}}");
    assert_theme_rejected_atomically(
        "{\"version\":1,\"background\":\"dark\",\"brightness\":100,"
        "\"accents\":{\"drone\":65184,\"meta\":63539,\"tracker\":63519,"
        "\"flock\":43039,\"wifi_attack\":2047,\"clear\":12133}}");
    assert_theme_rejected_atomically(
        "{\"version\":1,\"palette\":\"field\",\"brightness\":100,"
        "\"accents\":{\"drone\":65184,\"meta\":63539,\"tracker\":63519,"
        "\"flock\":43039,\"wifi_attack\":2047,\"clear\":12133}}");
    assert_theme_rejected_atomically(
        "{\"version\":1,\"palette\":\"field\",\"background\":\"dark\","
        "\"accents\":{\"drone\":65184,\"meta\":63539,\"tracker\":63519,"
        "\"flock\":43039,\"wifi_attack\":2047,\"clear\":12133}}");
    assert_theme_rejected_atomically(
        "{\"version\":1,\"palette\":\"field\",\"background\":\"dark\","
        "\"brightness\":100}");
    assert_theme_rejected_atomically(
        "{\"version\":1,\"palette\":\"field\",\"background\":\"dark\","
        "\"brightness\":100,\"accents\":{\"drone\":65184,\"meta\":63539,"
        "\"tracker\":63519,\"flock\":43039,\"wifi_attack\":2047}}");
}

void test_badge_theme_rejects_wrong_types_and_nonobjects_atomically(void)
{
    assert_theme_rejected_atomically("[]");
    assert_theme_rejected_atomically("null");
    assert_theme_rejected_atomically(
        "{\"version\":\"1\",\"palette\":\"field\",\"background\":\"dark\","
        "\"brightness\":100,\"accents\":{\"drone\":65184,\"meta\":63539,"
        "\"tracker\":63519,\"flock\":43039,\"wifi_attack\":2047,\"clear\":12133}}");
    assert_theme_rejected_atomically(
        "{\"version\":1,\"palette\":7,\"background\":\"dark\","
        "\"brightness\":100,\"accents\":{\"drone\":65184,\"meta\":63539,"
        "\"tracker\":63519,\"flock\":43039,\"wifi_attack\":2047,\"clear\":12133}}");
    assert_theme_rejected_atomically(
        "{\"version\":1,\"palette\":\"field\",\"background\":false,"
        "\"brightness\":100,\"accents\":{\"drone\":65184,\"meta\":63539,"
        "\"tracker\":63519,\"flock\":43039,\"wifi_attack\":2047,\"clear\":12133}}");
    assert_theme_rejected_atomically(
        "{\"version\":1,\"palette\":\"field\",\"background\":\"dark\","
        "\"brightness\":\"100\",\"accents\":{\"drone\":65184,\"meta\":63539,"
        "\"tracker\":63519,\"flock\":43039,\"wifi_attack\":2047,\"clear\":12133}}");
    assert_theme_rejected_atomically(
        "{\"version\":1,\"palette\":\"field\",\"background\":\"dark\","
        "\"brightness\":100,\"accents\":[]}");
    assert_theme_rejected_atomically(
        "{\"version\":1,\"palette\":\"field\",\"background\":\"dark\","
        "\"brightness\":100,\"accents\":{\"drone\":\"65184\",\"meta\":63539,"
        "\"tracker\":63519,\"flock\":43039,\"wifi_attack\":2047,\"clear\":12133}}");
}

void test_badge_theme_rejects_duplicates_unknowns_and_trailing_data_atomically(void)
{
    assert_theme_rejected_atomically(
        "{\"version\":1,\"version\":1,\"palette\":\"field\","
        "\"background\":\"dark\",\"brightness\":100,\"accents\":{"
        "\"drone\":65184,\"meta\":63539,\"tracker\":63519,\"flock\":43039,"
        "\"wifi_attack\":2047,\"clear\":12133}}");
    assert_theme_rejected_atomically(
        "{\"version\":1,\"palette\":\"field\",\"background\":\"dark\","
        "\"brightness\":100,\"accents\":{\"drone\":65184,\"drone\":65184,"
        "\"meta\":63539,\"tracker\":63519,\"flock\":43039,"
        "\"wifi_attack\":2047,\"clear\":12133}}");
    assert_theme_rejected_atomically(
        "{\"version\":1,\"palette\":\"field\",\"background\":\"dark\","
        "\"brightness\":100,\"unknown\":0,\"accents\":{\"drone\":65184,"
        "\"meta\":63539,\"tracker\":63519,\"flock\":43039,"
        "\"wifi_attack\":2047,\"clear\":12133}}");
    assert_theme_rejected_atomically(
        "{\"version\":1,\"palette\":\"field\",\"background\":\"dark\","
        "\"brightness\":100,\"accents\":{\"drone\":65184,\"meta\":63539,"
        "\"tracker\":63519,\"flock\":43039,\"wifi_attack\":2047,"
        "\"clear\":12133,\"unknown\":0}}");
    assert_theme_rejected_atomically(
        "{\"version\":1,\"palette\":\"field\",\"background\":\"dark\","
        "\"brightness\":100,\"accents\":{\"drone\":65184,\"meta\":63539,"
        "\"tracker\":63519,\"flock\":43039,\"wifi_attack\":2047,"
        "\"clear\":12133}}{}");
    assert_theme_rejected_atomically(
        "{\"version\":1,\"palette\":\"field\",\"background\":\"dark\","
        "\"brightness\":100,\"accents\":{\"drone\":65184,\"meta\":63539,"
        "\"tracker\":63519,\"flock\":43039,\"wifi_attack\":2047,"
        "\"clear\":12133}} true");
}

void test_badge_theme_rejects_noninteger_or_malformed_numbers_atomically(void)
{
    assert_theme_rejected_atomically(
        "{\"version\":1,\"palette\":\"field\",\"background\":\"dark\","
        "\"brightness\":100.0,\"accents\":{\"drone\":65184,\"meta\":63539,"
        "\"tracker\":63519,\"flock\":43039,\"wifi_attack\":2047,\"clear\":12133}}");
    assert_theme_rejected_atomically(
        "{\"version\":1,\"palette\":\"field\",\"background\":\"dark\","
        "\"brightness\":1e2,\"accents\":{\"drone\":65184,\"meta\":63539,"
        "\"tracker\":63519,\"flock\":43039,\"wifi_attack\":2047,\"clear\":12133}}");
    assert_theme_rejected_atomically(
        "{\"version\":1,\"palette\":\"field\",\"background\":\"dark\","
        "\"brightness\":0100,\"accents\":{\"drone\":65184,\"meta\":63539,"
        "\"tracker\":63519,\"flock\":43039,\"wifi_attack\":2047,\"clear\":12133}}");
    assert_theme_rejected_atomically(
        "{\"version\":1,\"palette\":\"field\",\"background\":\"dark\","
        "\"brightness\":100,\"accents\":{\"drone\":42949672960,\"meta\":63539,"
        "\"tracker\":63519,\"flock\":43039,\"wifi_attack\":2047,\"clear\":12133}}");
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

void test_badge_theme_con_palette_normal_preserves_selected_chrome(void)
{
    badge_theme_t theme;
    badge_con_render_palette_t palette;
    badge_theme_defaults(&theme);

    badge_theme_derive_con_palette(
        &theme, BADGE_CON_PRESENT_INACTIVE, &palette);

    TEST_ASSERT_EQUAL_UINT16(
        badge_theme_chrome_color(&theme, BADGE_THEME_CHROME_PANEL),
        palette.chrome_primary);
    TEST_ASSERT_EQUAL_UINT16(
        badge_theme_chrome_color(&theme, BADGE_THEME_CHROME_PANEL_ALT),
        palette.chrome_secondary);
    TEST_ASSERT_EQUAL_UINT16(
        badge_theme_chrome_color(&theme, BADGE_THEME_CHROME_SELECTION),
        palette.chrome_accent);
    TEST_ASSERT_EQUAL_UINT16(
        badge_theme_chrome_color(&theme, BADGE_THEME_CHROME_TEXT_PRIMARY),
        palette.chrome_text);
}

void test_badge_theme_con_palette_derives_role_colors_without_mutating_theme(void)
{
    badge_theme_t theme;
    badge_theme_t before;
    badge_con_render_palette_t infected;
    badge_con_render_palette_t immune;
    badge_theme_defaults(&theme);
    before = theme;

    badge_theme_derive_con_palette(
        &theme, BADGE_CON_PRESENT_INFECTED, &infected);
    badge_theme_derive_con_palette(
        &theme, BADGE_CON_PRESENT_IMMUNE, &immune);

    TEST_ASSERT_EQUAL_UINT16(0x79DD, infected.chrome_primary);
    TEST_ASSERT_EQUAL_UINT16(0x3FE2, infected.chrome_accent);
    TEST_ASSERT_EQUAL_UINT16(0xF9F5, immune.chrome_primary);
    TEST_ASSERT_EQUAL_UINT16(0xF81F, immune.chrome_accent);
    TEST_ASSERT_EQUAL_MEMORY(&before, &theme, sizeof(theme));
}

void test_badge_theme_con_palette_applies_brightness_and_safe_text(void)
{
    badge_theme_t theme;
    badge_con_render_palette_t full;
    badge_con_render_palette_t dim;
    badge_theme_defaults(&theme);

    badge_theme_derive_con_palette(
        &theme, BADGE_CON_PRESENT_INFECTED, &full);
    theme.brightness = 50U;
    badge_theme_derive_con_palette(
        &theme, BADGE_CON_PRESENT_INFECTED, &dim);

    TEST_ASSERT_EQUAL_UINT16(
        badge_theme_apply_brightness(&theme, 0x79DD),
        dim.chrome_primary);
    TEST_ASSERT_EQUAL_UINT16(
        badge_theme_apply_brightness(&theme, 0x3FE2),
        dim.chrome_accent);
    TEST_ASSERT_EQUAL_UINT16(
        badge_theme_contrast_floor(
            badge_theme_apply_brightness(&theme, 0xFFFF),
            dim.chrome_primary),
        dim.chrome_text);
    TEST_ASSERT_NOT_EQUAL(full.chrome_primary, dim.chrome_primary);
}

void test_badge_theme_con_palette_distinguishes_every_game_treatment(void)
{
    static const uint16_t expected_primary[] = {
        0x07E0U,
        0x07FFU,
        0x79DDU,
        0xF9F5U,
        0xF81FU,
        0xF800U,
        0xF81FU,
    };
    static const uint16_t expected_accent[] = {
        0xAFE5U,
        0x7FFFU,
        0x3FE2U,
        0xF81FU,
        0xFFE0U,
        0xFFFFU,
        0xFFE0U,
    };
    badge_theme_t theme;
    badge_theme_defaults(&theme);

    for (unsigned i = 0U;
         i < sizeof(expected_primary) / sizeof(expected_primary[0]);
         ++i) {
        badge_con_render_palette_t palette = {0};
        badge_theme_derive_con_palette(
            &theme,
            (badge_con_present_state_t)(BADGE_CON_PRESENT_HUMAN + i),
            &palette);
        TEST_ASSERT_EQUAL_UINT16(
            expected_primary[i], palette.chrome_primary);
        TEST_ASSERT_EQUAL_UINT16(
            expected_accent[i], palette.chrome_accent);
        TEST_ASSERT_NOT_EQUAL_UINT16(
            palette.chrome_primary, palette.chrome_text);
    }
}
