#include "badge_theme.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *key;
    uint16_t color;
} badge_theme_accent_default_t;

static const badge_theme_accent_default_t ACCENTS[BADGE_THEME_ACCENT_COUNT] = {
    [BADGE_THEME_ACCENT_DRONE]       = {"drone", 0xFEA0},
    [BADGE_THEME_ACCENT_META]        = {"meta", 0xF833},
    [BADGE_THEME_ACCENT_TRACKER]     = {"tracker", 0xF81F},
    [BADGE_THEME_ACCENT_FLOCK]       = {"flock", 0xA81F},
    [BADGE_THEME_ACCENT_WIFI_ATTACK] = {"wifi_attack", 0x07FF},
    [BADGE_THEME_ACCENT_CLEAR]       = {"clear", 0x2F65},
};

static char lower_char(char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
}

static bool eq_nocase(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        if (lower_char(*a++) != lower_char(*b++)) return false;
    }
    return *a == '\0' && *b == '\0';
}

static void set_err(char *err, size_t err_len, const char *msg)
{
    if (!err || err_len == 0) return;
    snprintf(err, err_len, "%s", msg ? msg : "invalid theme");
}

void badge_theme_defaults(badge_theme_t *theme)
{
    if (!theme) return;
    memset(theme, 0, sizeof(*theme));
    theme->version = BADGE_THEME_VERSION;
    snprintf(theme->palette, sizeof(theme->palette), "field");
    snprintf(theme->background, sizeof(theme->background), "dark");
    theme->brightness = 100;
    for (int i = 0; i < BADGE_THEME_ACCENT_COUNT; i++) {
        theme->accents[i] = ACCENTS[i].color;
    }
}

const char *badge_theme_accent_key(badge_theme_accent_t accent)
{
    if ((int)accent < 0 || accent >= BADGE_THEME_ACCENT_COUNT) {
        return "clear";
    }
    return ACCENTS[accent].key;
}

bool badge_theme_accent_from_key(const char *key, badge_theme_accent_t *out)
{
    if (!key) return false;
    for (int i = 0; i < BADGE_THEME_ACCENT_COUNT; i++) {
        if (eq_nocase(key, ACCENTS[i].key)) {
            if (out) *out = (badge_theme_accent_t)i;
            return true;
        }
    }
    return false;
}

static void hash_byte(uint32_t *h, uint8_t byte)
{
    *h ^= byte;
    *h *= 16777619u;
}

uint32_t badge_theme_hash(const badge_theme_t *theme)
{
    badge_theme_t fallback;
    if (!theme) {
        badge_theme_defaults(&fallback);
        theme = &fallback;
    }
    uint32_t h = 2166136261u;
    hash_byte(&h, theme->version);
    hash_byte(&h, theme->brightness);
    for (const char *p = theme->palette; *p; p++) hash_byte(&h, (uint8_t)*p);
    hash_byte(&h, 0);
    for (const char *p = theme->background; *p; p++) hash_byte(&h, (uint8_t)*p);
    hash_byte(&h, 0);
    for (int i = 0; i < BADGE_THEME_ACCENT_COUNT; i++) {
        hash_byte(&h, (uint8_t)(theme->accents[i] >> 8));
        hash_byte(&h, (uint8_t)(theme->accents[i] & 0xff));
    }
    return h;
}

static const char *find_string_value(const char *obj, const char *field,
                                     char *out, size_t out_len)
{
    char pattern[40];
    snprintf(pattern, sizeof(pattern), "\"%s\"", field);
    const char *p = strstr(obj, pattern);
    if (!p) return NULL;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return NULL;
    p++;
    size_t used = 0;
    while (*p && *p != '"') {
        if (used + 1 < out_len) out[used++] = *p;
        p++;
    }
    if (*p != '"') return NULL;
    if (out_len > 0) out[used] = '\0';
    return p + 1;
}

static const char *find_int_value(const char *obj, const char *field, int *out)
{
    char pattern[40];
    snprintf(pattern, sizeof(pattern), "\"%s\"", field);
    const char *p = strstr(obj, pattern);
    if (!p) return NULL;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    int sign = 1;
    if (*p == '-') {
        sign = -1;
        p++;
    }
    if (*p < '0' || *p > '9') return NULL;
    int value = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        p++;
    }
    if (out) *out = value * sign;
    return p;
}

static const char *object_for_key(const char *json, const char *key,
                                  const char **end_out)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '{') return NULL;
    int depth = 0;
    const char *start = p;
    for (; *p; p++) {
        if (*p == '{') depth++;
        if (*p == '}') {
            depth--;
            if (depth == 0) {
                if (end_out) *end_out = p + 1;
                return start;
            }
        }
    }
    return NULL;
}

static bool theme_name_allowed(const char *value, const char *const *allowed,
                               size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (eq_nocase(value, allowed[i])) return true;
    }
    return false;
}

bool badge_theme_parse_json(const char *json, badge_theme_t *out,
                            char *err, size_t err_len)
{
    if (!json || !out) {
        set_err(err, err_len, "missing theme");
        return false;
    }
    badge_theme_defaults(out);

    int version = BADGE_THEME_VERSION;
    if (find_int_value(json, "version", &version) && version != BADGE_THEME_VERSION) {
        set_err(err, err_len, "unsupported version");
        return false;
    }
    out->version = (uint8_t)version;

    char value[BADGE_THEME_NAME_MAX] = {0};
    if (strstr(json, "\"palette\"")) {
        static const char *const palettes[] = {"field", "night", "neon", "mono"};
        if (!find_string_value(json, "palette", value, sizeof(value)) ||
            !theme_name_allowed(value, palettes, sizeof(palettes) / sizeof(palettes[0]))) {
            set_err(err, err_len, "invalid palette");
            return false;
        }
        snprintf(out->palette, sizeof(out->palette), "%s", value);
    }
    if (strstr(json, "\"background\"")) {
        static const char *const backgrounds[] = {"dark", "dim", "scanline"};
        if (!find_string_value(json, "background", value, sizeof(value)) ||
            !theme_name_allowed(value, backgrounds,
                                sizeof(backgrounds) / sizeof(backgrounds[0]))) {
            set_err(err, err_len, "invalid background");
            return false;
        }
        snprintf(out->background, sizeof(out->background), "%s", value);
    }

    int brightness = out->brightness;
    if (strstr(json, "\"brightness\"")) {
        if (!find_int_value(json, "brightness", &brightness) ||
            brightness < 25 || brightness > 100) {
            set_err(err, err_len, "invalid brightness");
            return false;
        }
        out->brightness = (uint8_t)brightness;
    }

    const char *accents_end = NULL;
    const char *accents = object_for_key(json, "accents", &accents_end);
    if (accents) {
        char chunk[320];
        size_t len = accents_end && accents_end > accents
            ? (size_t)(accents_end - accents)
            : strlen(accents);
        if (len >= sizeof(chunk)) len = sizeof(chunk) - 1;
        memcpy(chunk, accents, len);
        chunk[len] = '\0';
        for (int i = 0; i < BADGE_THEME_ACCENT_COUNT; i++) {
            int color = out->accents[i];
            if (strstr(chunk, ACCENTS[i].key)) {
                if (!find_int_value(chunk, ACCENTS[i].key, &color) ||
                    color < 0 || color > 0xffff) {
                    set_err(err, err_len, "invalid accent");
                    return false;
                }
                out->accents[i] = (uint16_t)color;
            }
        }
    }
    return true;
}

size_t badge_theme_to_json(const badge_theme_t *theme, char *out, size_t out_len)
{
    badge_theme_t fallback;
    if (!theme) {
        badge_theme_defaults(&fallback);
        theme = &fallback;
    }
    if (!out || out_len == 0) return 0;
    int n = snprintf(out, out_len,
                     "{\"version\":%u,\"palette\":\"%s\","
                     "\"background\":\"%s\",\"brightness\":%u,\"accents\":{",
                     (unsigned)theme->version,
                     theme->palette,
                     theme->background,
                     (unsigned)theme->brightness);
    if (n < 0) return 0;
    size_t used = (size_t)n < out_len ? (size_t)n : out_len - 1;
    for (int i = 0; i < BADGE_THEME_ACCENT_COUNT && used < out_len; i++) {
        n = snprintf(out + used, out_len - used, "%s\"%s\":%u",
                     i == 0 ? "" : ",",
                     ACCENTS[i].key,
                     (unsigned)theme->accents[i]);
        if (n < 0) break;
        used += (size_t)n;
        if (used >= out_len) {
            out[out_len - 1] = '\0';
            return out_len - 1;
        }
    }
    if (used + 2 < out_len) {
        out[used++] = '}';
        out[used++] = '}';
        out[used] = '\0';
    } else {
        out[out_len - 1] = '\0';
    }
    return used;
}

uint16_t badge_theme_accent_color(const badge_theme_t *theme,
                                  badge_theme_accent_t accent)
{
    badge_theme_t fallback;
    if (!theme) {
        badge_theme_defaults(&fallback);
        theme = &fallback;
    }
    if ((int)accent < 0 || accent >= BADGE_THEME_ACCENT_COUNT) {
        accent = BADGE_THEME_ACCENT_CLEAR;
    }
    return badge_theme_apply_brightness(theme, theme->accents[accent]);
}

uint16_t badge_theme_background_color(const badge_theme_t *theme)
{
    badge_theme_t fallback;
    if (!theme) {
        badge_theme_defaults(&fallback);
        theme = &fallback;
    }
    uint16_t bg = 0x0000;
    if (eq_nocase(theme->background, "dim")) {
        bg = 0x1082;
    } else if (eq_nocase(theme->background, "scanline")) {
        bg = 0x0108;
    }
    return badge_theme_apply_brightness(theme, bg);
}

uint16_t badge_theme_apply_brightness(const badge_theme_t *theme, uint16_t rgb565)
{
    uint8_t brightness = theme ? theme->brightness : 100;
    if (brightness >= 100) return rgb565;
    if (brightness < 25) brightness = 25;
    uint32_t r = (rgb565 >> 11) & 0x1f;
    uint32_t g = (rgb565 >> 5) & 0x3f;
    uint32_t b = rgb565 & 0x1f;
    r = (r * brightness) / 100;
    g = (g * brightness) / 100;
    b = (b * brightness) / 100;
    return (uint16_t)((r << 11) | (g << 5) | b);
}
