#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BADGE_THEME_VERSION 1
#define BADGE_THEME_JSON_MAX 640
#define BADGE_THEME_NAME_MAX 16
#define BADGE_THEME_ACCENT_COUNT 6

typedef enum {
    BADGE_THEME_ACCENT_DRONE = 0,
    BADGE_THEME_ACCENT_META,
    BADGE_THEME_ACCENT_TRACKER,
    BADGE_THEME_ACCENT_FLOCK,
    BADGE_THEME_ACCENT_WIFI_ATTACK,
    BADGE_THEME_ACCENT_CLEAR,
} badge_theme_accent_t;

typedef enum {
    BADGE_THEME_CHROME_CANVAS = 0,
    BADGE_THEME_CHROME_PANEL,
    BADGE_THEME_CHROME_PANEL_ALT,
    BADGE_THEME_CHROME_TEXT_PRIMARY,
    BADGE_THEME_CHROME_TEXT_SECONDARY,
    BADGE_THEME_CHROME_SELECTION,
    BADGE_THEME_CHROME_SCANNER_DOWN,
    BADGE_THEME_CHROME_ROLE_COUNT,
} badge_theme_chrome_role_t;

typedef struct {
    uint8_t version;
    char palette[BADGE_THEME_NAME_MAX];
    char background[BADGE_THEME_NAME_MAX];
    uint8_t brightness;
    uint16_t accents[BADGE_THEME_ACCENT_COUNT];
} badge_theme_t;

void badge_theme_defaults(badge_theme_t *theme);
const char *badge_theme_accent_key(badge_theme_accent_t accent);
bool badge_theme_accent_from_key(const char *key, badge_theme_accent_t *out);
uint32_t badge_theme_hash(const badge_theme_t *theme);
bool badge_theme_parse_json(const char *json, badge_theme_t *out,
                            char *err, size_t err_len);
size_t badge_theme_to_json(const badge_theme_t *theme, char *out, size_t out_len);
uint16_t badge_theme_accent_color(const badge_theme_t *theme,
                                  badge_theme_accent_t accent);
uint16_t badge_theme_background_color(const badge_theme_t *theme);
uint16_t badge_theme_apply_brightness(const badge_theme_t *theme, uint16_t rgb565);
uint16_t badge_theme_chrome_color(const badge_theme_t *theme,
                                  badge_theme_chrome_role_t role);
uint16_t badge_theme_contrast_floor(uint16_t foreground, uint16_t background);

#ifdef __cplusplus
}
#endif
