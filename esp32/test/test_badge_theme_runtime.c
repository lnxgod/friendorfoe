#include "unity.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static bool s_nvs_write_ok;
static unsigned s_nvs_write_count;

#define FOF_BADGE_VARIANT 1
#include "../uplink/main/core/badge_theme_runtime.c"

bool nvs_config_get_string(const char *key, char *buf, size_t buf_size)
{
    (void)key;
    (void)buf;
    (void)buf_size;
    return false;
}

bool nvs_config_set_string(const char *key, const char *value)
{
    (void)key;
    (void)value;
    s_nvs_write_count++;
    return s_nvs_write_ok;
}

static badge_theme_t custom_theme(void)
{
    badge_theme_t theme;
    badge_theme_defaults(&theme);
    strcpy(theme.palette, "purple");
    theme.brightness = 73;
    theme.accents[BADGE_THEME_ACCENT_META] ^= 0x001f;
    return theme;
}

void test_badge_theme_failed_persist_keeps_visible_theme_and_hash(void)
{
    s_nvs_write_ok = false;
    s_nvs_write_count = 0;
    badge_theme_runtime_init();

    badge_theme_t before = *badge_theme_runtime_get();
    uint32_t hash_before = badge_theme_runtime_hash();
    badge_theme_t requested = custom_theme();

    TEST_ASSERT_FALSE(badge_theme_runtime_set(&requested, true));
    TEST_ASSERT_EQUAL_UINT(1, s_nvs_write_count);
    TEST_ASSERT_EQUAL_MEMORY(&before, badge_theme_runtime_get(), sizeof(before));
    TEST_ASSERT_EQUAL_UINT32(hash_before, badge_theme_runtime_hash());
}

void test_badge_theme_failed_persisted_reset_keeps_visible_custom_theme(void)
{
    s_nvs_write_ok = false;
    s_nvs_write_count = 0;
    badge_theme_runtime_init();

    badge_theme_t requested = custom_theme();
    TEST_ASSERT_TRUE(badge_theme_runtime_set(&requested, false));
    badge_theme_t before = *badge_theme_runtime_get();
    uint32_t hash_before = badge_theme_runtime_hash();

    badge_theme_runtime_reset(true);
    TEST_ASSERT_EQUAL_UINT(1, s_nvs_write_count);
    TEST_ASSERT_EQUAL_MEMORY(&before, badge_theme_runtime_get(), sizeof(before));
    TEST_ASSERT_EQUAL_UINT32(hash_before, badge_theme_runtime_hash());
}
