#include "badge_theme.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *key;
    uint16_t color;
} badge_theme_accent_default_t;

typedef struct {
    const char *palette;
    uint16_t colors[BADGE_THEME_CHROME_ROLE_COUNT];
} badge_theme_chrome_palette_t;

static const badge_theme_accent_default_t ACCENTS[BADGE_THEME_ACCENT_COUNT] = {
    [BADGE_THEME_ACCENT_DRONE]       = {"drone", 0xFEA0},
    [BADGE_THEME_ACCENT_META]        = {"meta", 0xF833},
    [BADGE_THEME_ACCENT_TRACKER]     = {"tracker", 0xF81F},
    [BADGE_THEME_ACCENT_FLOCK]       = {"flock", 0xA81F},
    [BADGE_THEME_ACCENT_WIFI_ATTACK] = {"wifi_attack", 0x07FF},
    [BADGE_THEME_ACCENT_CLEAR]       = {"clear", 0x2F65},
};

static const badge_theme_chrome_palette_t CHROME_PALETTES[] = {
    {
        .palette = "field",
        .colors = {
            [BADGE_THEME_CHROME_CANVAS]         = 0x0000,
            [BADGE_THEME_CHROME_PANEL]          = 0x1082,
            [BADGE_THEME_CHROME_PANEL_ALT]      = 0x2104,
            [BADGE_THEME_CHROME_TEXT_PRIMARY]   = 0xFFFF,
            [BADGE_THEME_CHROME_TEXT_SECONDARY] = 0x8410,
            [BADGE_THEME_CHROME_SELECTION]      = 0x57EA,
            [BADGE_THEME_CHROME_SCANNER_DOWN]   = 0xFEA0,
        },
    },
    {
        .palette = "night",
        .colors = {
            [BADGE_THEME_CHROME_CANVAS]         = 0x0800,
            [BADGE_THEME_CHROME_PANEL]          = 0x1800,
            [BADGE_THEME_CHROME_PANEL_ALT]      = 0x3000,
            [BADGE_THEME_CHROME_TEXT_PRIMARY]   = 0xFFE7,
            [BADGE_THEME_CHROME_TEXT_SECONDARY] = 0xAC4D,
            [BADGE_THEME_CHROME_SELECTION]      = 0xFD20,
            [BADGE_THEME_CHROME_SCANNER_DOWN]   = 0xFEA0,
        },
    },
    {
        .palette = "neon",
        .colors = {
            [BADGE_THEME_CHROME_CANVAS]         = 0x080C,
            [BADGE_THEME_CHROME_PANEL]          = 0x2015,
            [BADGE_THEME_CHROME_PANEL_ALT]      = 0x4018,
            [BADGE_THEME_CHROME_TEXT_PRIMARY]   = 0xF7FF,
            [BADGE_THEME_CHROME_TEXT_SECONDARY] = 0x87FF,
            [BADGE_THEME_CHROME_SELECTION]      = 0xF81F,
            [BADGE_THEME_CHROME_SCANNER_DOWN]   = 0xFB38,
        },
    },
    {
        .palette = "mono",
        .colors = {
            [BADGE_THEME_CHROME_CANVAS]         = 0x0000,
            [BADGE_THEME_CHROME_PANEL]          = 0x0821,
            [BADGE_THEME_CHROME_PANEL_ALT]      = 0x1082,
            [BADGE_THEME_CHROME_TEXT_PRIMARY]   = 0xEFFF,
            [BADGE_THEME_CHROME_TEXT_SECONDARY] = 0x7BEF,
            [BADGE_THEME_CHROME_SELECTION]      = 0x07FF,
            [BADGE_THEME_CHROME_SCANNER_DOWN]   = 0xFFFF,
        },
    },
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

static bool theme_name_allowed(const char *value, const char *const *allowed,
                               size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(value, allowed[i]) == 0) return true;
    }
    return false;
}

typedef struct {
    const char *next;
} badge_theme_json_cursor_t;

static void json_skip_ws(badge_theme_json_cursor_t *cursor)
{
    while (*cursor->next == ' ' || *cursor->next == '\t' ||
           *cursor->next == '\n' || *cursor->next == '\r') {
        cursor->next++;
    }
}

static bool json_consume(badge_theme_json_cursor_t *cursor, char expected)
{
    json_skip_ws(cursor);
    if (*cursor->next != expected) return false;
    cursor->next++;
    return true;
}

static bool json_parse_plain_string(badge_theme_json_cursor_t *cursor,
                                    char *out, size_t out_len)
{
    if (!out || out_len == 0 || !json_consume(cursor, '"')) return false;

    size_t used = 0;
    while (*cursor->next && *cursor->next != '"') {
        unsigned char ch = (unsigned char)*cursor->next++;
        if (ch < 0x20 || ch == '\\' || used + 1 >= out_len) return false;
        out[used++] = (char)ch;
    }
    if (*cursor->next != '"') return false;
    cursor->next++;
    out[used] = '\0';
    return true;
}

static bool json_parse_uint32(badge_theme_json_cursor_t *cursor, uint32_t *out)
{
    json_skip_ws(cursor);
    const char *p = cursor->next;
    if (*p < '0' || *p > '9') return false;
    if (*p == '0' && p[1] >= '0' && p[1] <= '9') return false;

    uint32_t value = 0;
    do {
        uint32_t digit = (uint32_t)(*p - '0');
        if (value > (UINT32_MAX - digit) / 10U) return false;
        value = value * 10U + digit;
        p++;
    } while (*p >= '0' && *p <= '9');

    cursor->next = p;
    if (out) *out = value;
    return true;
}

static int accent_index_from_exact_key(const char *key)
{
    for (int i = 0; i < BADGE_THEME_ACCENT_COUNT; i++) {
        if (strcmp(key, ACCENTS[i].key) == 0) return i;
    }
    return -1;
}

static bool json_finish_member(badge_theme_json_cursor_t *cursor,
                               bool *object_done)
{
    json_skip_ws(cursor);
    if (*cursor->next == ',') {
        cursor->next++;
        *object_done = false;
        return true;
    }
    if (*cursor->next == '}') {
        cursor->next++;
        *object_done = true;
        return true;
    }
    return false;
}

static bool parse_accents_object(badge_theme_json_cursor_t *cursor,
                                 badge_theme_t *theme,
                                 char *err, size_t err_len)
{
    const uint32_t required = (1U << BADGE_THEME_ACCENT_COUNT) - 1U;
    uint32_t seen = 0;
    if (!json_consume(cursor, '{')) {
        set_err(err, err_len, "invalid accents");
        return false;
    }
    json_skip_ws(cursor);
    if (*cursor->next == '}') {
        set_err(err, err_len, "missing accent");
        return false;
    }

    bool done = false;
    while (!done) {
        char key[BADGE_THEME_NAME_MAX];
        if (!json_parse_plain_string(cursor, key, sizeof(key))) {
            set_err(err, err_len, "invalid accent key");
            return false;
        }
        int index = accent_index_from_exact_key(key);
        if (index < 0) {
            set_err(err, err_len, "unknown accent");
            return false;
        }
        uint32_t bit = 1U << (unsigned)index;
        if ((seen & bit) != 0) {
            set_err(err, err_len, "duplicate accent");
            return false;
        }
        if (!json_consume(cursor, ':')) {
            set_err(err, err_len, "invalid accent");
            return false;
        }

        uint32_t color = 0;
        if (!json_parse_uint32(cursor, &color) || color > 0xffffU) {
            set_err(err, err_len, "invalid accent");
            return false;
        }
        theme->accents[index] = (uint16_t)color;
        seen |= bit;
        if (!json_finish_member(cursor, &done)) {
            set_err(err, err_len, "invalid accents");
            return false;
        }
    }
    if (seen != required) {
        set_err(err, err_len, "missing accent");
        return false;
    }
    return true;
}

static bool badge_theme_parse_json_projected(
    const char *json,
    badge_theme_t *out,
    char *err,
    size_t err_len)
{
    if (!json || !out) {
        set_err(err, err_len, "missing theme");
        return false;
    }
    size_t json_len = 0;
    while (json_len < BADGE_THEME_JSON_MAX && json[json_len] != '\0') json_len++;
    if (json_len == BADGE_THEME_JSON_MAX) {
        set_err(err, err_len, "theme too large");
        return false;
    }

    enum {
        SEEN_VERSION = 1U << 0,
        SEEN_PALETTE = 1U << 1,
        SEEN_BACKGROUND = 1U << 2,
        SEEN_BRIGHTNESS = 1U << 3,
        SEEN_ACCENTS = 1U << 4,
        REQUIRED_FIELDS = SEEN_VERSION | SEEN_PALETTE | SEEN_BACKGROUND |
                          SEEN_BRIGHTNESS | SEEN_ACCENTS,
    };
    static const char *const palettes[] = {"field", "night", "neon", "mono"};
    static const char *const backgrounds[] = {"dark", "dim", "scanline"};

    badge_theme_t parsed;
    badge_theme_defaults(&parsed);
    badge_theme_json_cursor_t cursor = {.next = json};
    if (!json_consume(&cursor, '{')) {
        set_err(err, err_len, "theme must be object");
        return false;
    }
    json_skip_ws(&cursor);
    if (*cursor.next == '}') {
        set_err(err, err_len, "missing theme fields");
        return false;
    }

    uint32_t seen = 0;
    bool done = false;
    while (!done) {
        char key[BADGE_THEME_NAME_MAX];
        if (!json_parse_plain_string(&cursor, key, sizeof(key)) ||
            !json_consume(&cursor, ':')) {
            set_err(err, err_len, "invalid theme field");
            return false;
        }

        uint32_t field = 0;
        if (strcmp(key, "version") == 0) {
            field = SEEN_VERSION;
        } else if (strcmp(key, "palette") == 0) {
            field = SEEN_PALETTE;
        } else if (strcmp(key, "background") == 0) {
            field = SEEN_BACKGROUND;
        } else if (strcmp(key, "brightness") == 0) {
            field = SEEN_BRIGHTNESS;
        } else if (strcmp(key, "accents") == 0) {
            field = SEEN_ACCENTS;
        } else {
            set_err(err, err_len, "unknown theme field");
            return false;
        }
        if ((seen & field) != 0) {
            set_err(err, err_len, "duplicate theme field");
            return false;
        }

        if (field == SEEN_VERSION) {
            uint32_t version = 0;
            if (!json_parse_uint32(&cursor, &version)) {
                set_err(err, err_len, "invalid version");
                return false;
            }
            if (version != BADGE_THEME_VERSION) {
                set_err(err, err_len, "unsupported version");
                return false;
            }
            parsed.version = (uint8_t)version;
        } else if (field == SEEN_PALETTE) {
            char value[BADGE_THEME_NAME_MAX];
            if (!json_parse_plain_string(&cursor, value, sizeof(value)) ||
                !theme_name_allowed(value, palettes,
                                    sizeof(palettes) / sizeof(palettes[0]))) {
                set_err(err, err_len, "invalid palette");
                return false;
            }
            snprintf(parsed.palette, sizeof(parsed.palette), "%s", value);
        } else if (field == SEEN_BACKGROUND) {
            char value[BADGE_THEME_NAME_MAX];
            if (!json_parse_plain_string(&cursor, value, sizeof(value)) ||
                !theme_name_allowed(value, backgrounds,
                                    sizeof(backgrounds) / sizeof(backgrounds[0]))) {
                set_err(err, err_len, "invalid background");
                return false;
            }
            snprintf(parsed.background, sizeof(parsed.background), "%s", value);
        } else if (field == SEEN_BRIGHTNESS) {
            uint32_t brightness = 0;
            if (!json_parse_uint32(&cursor, &brightness) ||
                brightness < 25U || brightness > 100U) {
                set_err(err, err_len, "invalid brightness");
                return false;
            }
            parsed.brightness = (uint8_t)brightness;
        } else if (!parse_accents_object(&cursor, &parsed, err, err_len)) {
            return false;
        }
        seen |= field;

        if (!json_finish_member(&cursor, &done)) {
            set_err(err, err_len, "invalid theme object");
            return false;
        }
    }
    json_skip_ws(&cursor);
    if (*cursor.next != '\0') {
        set_err(err, err_len, "trailing theme data");
        return false;
    }
    if (seen != REQUIRED_FIELDS) {
        set_err(err, err_len, "missing theme fields");
        return false;
    }

    *out = parsed;
    if (err && err_len > 0) err[0] = '\0';
    return true;
}

bool badge_theme_parse_json_span(const uint8_t *json,
                                 size_t json_len,
                                 badge_theme_t *out,
                                 char *err,
                                 size_t err_len)
{
    if (!json || !out) {
        set_err(err, err_len, "missing theme");
        return false;
    }
    if (json_len == 0U || json_len >= BADGE_THEME_JSON_MAX) {
        set_err(err, err_len, "theme too large");
        return false;
    }
    if (memchr(json, '\0', json_len) != NULL) {
        set_err(err, err_len, "invalid theme");
        return false;
    }

    char projected[BADGE_THEME_JSON_MAX];
    memcpy(projected, json, json_len);
    projected[json_len] = '\0';
    return badge_theme_parse_json_projected(
        projected, out, err, err_len);
}

bool badge_theme_parse_json(const char *json, badge_theme_t *out,
                            char *err, size_t err_len)
{
    if (!json || !out) {
        set_err(err, err_len, "missing theme");
        return false;
    }
    size_t json_len = 0U;
    while (json_len < BADGE_THEME_JSON_MAX &&
           json[json_len] != '\0') {
        json_len++;
    }
    if (json_len == BADGE_THEME_JSON_MAX) {
        set_err(err, err_len, "theme too large");
        return false;
    }
    return badge_theme_parse_json_span(
        (const uint8_t *)json, json_len, out, err, err_len);
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

uint16_t badge_theme_chrome_color(const badge_theme_t *theme,
                                  badge_theme_chrome_role_t role)
{
    badge_theme_t fallback;
    if (!theme) {
        badge_theme_defaults(&fallback);
        theme = &fallback;
    }
    if ((int)role < 0 || role >= BADGE_THEME_CHROME_ROLE_COUNT) {
        role = BADGE_THEME_CHROME_TEXT_PRIMARY;
    }

    const badge_theme_chrome_palette_t *palette = &CHROME_PALETTES[0];
    for (size_t i = 0; i < sizeof(CHROME_PALETTES) / sizeof(CHROME_PALETTES[0]); i++) {
        if (eq_nocase(theme->palette, CHROME_PALETTES[i].palette)) {
            palette = &CHROME_PALETTES[i];
            break;
        }
    }
    return badge_theme_apply_brightness(theme, palette->colors[role]);
}

static uint16_t rgb565_luminance(uint16_t color)
{
    uint32_t red = ((color >> 11) & 0x1f) * 255U / 31U;
    uint32_t green = ((color >> 5) & 0x3f) * 255U / 63U;
    uint32_t blue = (color & 0x1f) * 255U / 31U;
    return (uint16_t)((red * 54U + green * 183U + blue * 19U) / 256U);
}

uint16_t badge_theme_contrast_floor(uint16_t foreground, uint16_t background)
{
    enum {
        BADGE_THEME_MIN_SAFE_LUMINANCE = 72,
        BADGE_THEME_MIN_SAFE_CONTRAST = 64,
    };
    uint16_t fg_luminance = rgb565_luminance(foreground);
    uint16_t bg_luminance = rgb565_luminance(background);
    uint16_t contrast = fg_luminance > bg_luminance
        ? (uint16_t)(fg_luminance - bg_luminance)
        : (uint16_t)(bg_luminance - fg_luminance);

    if (fg_luminance >= BADGE_THEME_MIN_SAFE_LUMINANCE &&
        contrast >= BADGE_THEME_MIN_SAFE_CONTRAST) {
        return foreground;
    }
    return bg_luminance < 128U ? 0xFFFF : 0x0000;
}

#if defined(FOF_DC34_GAME_CANARY)
void badge_theme_derive_con_palette(
    const badge_theme_t *selected,
    badge_con_present_state_t state,
    badge_con_render_palette_t *out)
{
    if (!out) {
        return;
    }

    badge_theme_t fallback;
    if (!selected) {
        badge_theme_defaults(&fallback);
        selected = &fallback;
    }

    if (state < BADGE_CON_PRESENT_HUMAN ||
        state > BADGE_CON_PRESENT_DEAD_SUPER) {
        *out = (badge_con_render_palette_t) {
            .chrome_primary = badge_theme_chrome_color(
                selected, BADGE_THEME_CHROME_PANEL),
            .chrome_secondary = badge_theme_chrome_color(
                selected, BADGE_THEME_CHROME_PANEL_ALT),
            .chrome_accent = badge_theme_chrome_color(
                selected, BADGE_THEME_CHROME_SELECTION),
            .chrome_text = badge_theme_chrome_color(
                selected, BADGE_THEME_CHROME_TEXT_PRIMARY),
        };
        return;
    }

    static const uint16_t primary[] = {
        0x07E0, 0x07FF, 0x79DD, 0xF9F5, 0xF81F, 0xF800, 0xF81F,
    };
    static const uint16_t accent[] = {
        0xAFE5, 0x7FFF, 0x3FE2, 0xF81F, 0xFFE0, 0xFFFF, 0xFFE0,
    };
    const size_t index = (size_t)(state - BADGE_CON_PRESENT_HUMAN);
    out->chrome_primary =
        badge_theme_apply_brightness(selected, primary[index]);
    out->chrome_secondary =
        badge_theme_apply_brightness(selected, accent[index]);
    out->chrome_accent =
        badge_theme_apply_brightness(selected, accent[index]);
    out->chrome_text = badge_theme_contrast_floor(
        badge_theme_apply_brightness(selected, 0xFFFF),
        out->chrome_primary);
}
#endif
