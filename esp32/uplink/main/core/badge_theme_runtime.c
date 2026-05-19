#ifdef FOF_BADGE_VARIANT

#include "badge_theme_runtime.h"

#include "nvs_config.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define BADGE_THEME_NVS_KEY "badge_theme_v1"

static const char *TAG = "badge_theme";

static badge_theme_t s_theme;
static uint32_t s_theme_hash = 0;
static SemaphoreHandle_t s_theme_lock = NULL;

static void ensure_lock(void)
{
    if (!s_theme_lock) {
        s_theme_lock = xSemaphoreCreateMutex();
    }
}

static void commit_theme_locked(const badge_theme_t *theme)
{
    s_theme = *theme;
    s_theme_hash = badge_theme_hash(&s_theme);
}

void badge_theme_runtime_init(void)
{
    ensure_lock();
    badge_theme_t theme;
    badge_theme_defaults(&theme);

    char json[BADGE_THEME_JSON_MAX] = {0};
    if (nvs_config_get_string(BADGE_THEME_NVS_KEY, json, sizeof(json))) {
        char err[48] = {0};
        if (!badge_theme_parse_json(json, &theme, err, sizeof(err))) {
            ESP_LOGW(TAG, "Stored badge theme invalid (%s), using defaults",
                     err[0] ? err : "parse");
            badge_theme_defaults(&theme);
        }
    }

    if (s_theme_lock) {
        xSemaphoreTake(s_theme_lock, portMAX_DELAY);
    }
    commit_theme_locked(&theme);
    if (s_theme_lock) {
        xSemaphoreGive(s_theme_lock);
    }
    ESP_LOGI(TAG, "Badge theme active hash=%lu palette=%s background=%s",
             (unsigned long)s_theme_hash, s_theme.palette, s_theme.background);
}

const badge_theme_t *badge_theme_runtime_get(void)
{
    if (s_theme.version != BADGE_THEME_VERSION) {
        badge_theme_defaults(&s_theme);
        s_theme_hash = badge_theme_hash(&s_theme);
    }
    return &s_theme;
}

uint32_t badge_theme_runtime_hash(void)
{
    if (s_theme_hash == 0) {
        s_theme_hash = badge_theme_hash(badge_theme_runtime_get());
    }
    return s_theme_hash;
}

bool badge_theme_runtime_set(const badge_theme_t *theme, bool persist)
{
    if (!theme) {
        return false;
    }
    ensure_lock();
    if (s_theme_lock) {
        xSemaphoreTake(s_theme_lock, portMAX_DELAY);
    }
    commit_theme_locked(theme);
    if (s_theme_lock) {
        xSemaphoreGive(s_theme_lock);
    }

    if (persist) {
        char json[BADGE_THEME_JSON_MAX] = {0};
        badge_theme_to_json(theme, json, sizeof(json));
        if (!nvs_config_set_string(BADGE_THEME_NVS_KEY, json)) {
            ESP_LOGW(TAG, "Failed to persist badge theme");
            return false;
        }
    }
    ESP_LOGI(TAG, "Badge theme updated hash=%lu persist=%d",
             (unsigned long)s_theme_hash, persist ? 1 : 0);
    return true;
}

void badge_theme_runtime_reset(bool persist)
{
    badge_theme_t theme;
    badge_theme_defaults(&theme);
    (void)badge_theme_runtime_set(&theme, persist);
}

size_t badge_theme_runtime_json(char *out, size_t out_len)
{
    return badge_theme_to_json(badge_theme_runtime_get(), out, out_len);
}

#endif /* FOF_BADGE_VARIANT */
