#ifdef FOF_BADGE_VARIANT

#include "badge_display_policy_runtime.h"

#include "nvs_config.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define BADGE_DISPLAY_POLICY_NVS_KEY "badge_display_policy_v1"

static const char *TAG = "badge_display_policy";

static badge_display_policy_t s_policy;
static uint32_t s_policy_hash = 0;
static uint32_t s_filtered_counts[BADGE_DISPLAY_POLICY_CLASS_COUNT];
static SemaphoreHandle_t s_policy_lock = NULL;

static void ensure_lock(void)
{
    if (!s_policy_lock) {
        s_policy_lock = xSemaphoreCreateMutex();
    }
}

static void commit_policy_locked(const badge_display_policy_t *policy)
{
    s_policy = *policy;
    s_policy_hash = badge_display_policy_hash(&s_policy);
}

void badge_display_policy_runtime_init(void)
{
    ensure_lock();
    badge_display_policy_t policy;
    badge_display_policy_defaults(&policy);

    char json[BADGE_DISPLAY_POLICY_JSON_MAX] = {0};
    if (nvs_config_get_string(BADGE_DISPLAY_POLICY_NVS_KEY, json, sizeof(json))) {
        char err[48] = {0};
        if (!badge_display_policy_parse_json(json, &policy, err, sizeof(err))) {
            ESP_LOGW(TAG, "Stored display policy invalid (%s), using defaults",
                     err[0] ? err : "parse");
            badge_display_policy_defaults(&policy);
        }
    }

    if (s_policy_lock) {
        xSemaphoreTake(s_policy_lock, portMAX_DELAY);
    }
    commit_policy_locked(&policy);
    memset(s_filtered_counts, 0, sizeof(s_filtered_counts));
    if (s_policy_lock) {
        xSemaphoreGive(s_policy_lock);
    }
    ESP_LOGI(TAG, "Badge display policy active hash=%lu",
             (unsigned long)s_policy_hash);
}

const badge_display_policy_t *badge_display_policy_runtime_get(void)
{
    if (s_policy.version != BADGE_DISPLAY_POLICY_VERSION) {
        badge_display_policy_defaults(&s_policy);
        s_policy_hash = badge_display_policy_hash(&s_policy);
    }
    return &s_policy;
}

uint32_t badge_display_policy_runtime_hash(void)
{
    if (s_policy_hash == 0) {
        s_policy_hash = badge_display_policy_hash(
            badge_display_policy_runtime_get());
    }
    return s_policy_hash;
}

bool badge_display_policy_runtime_set(const badge_display_policy_t *policy,
                                      bool persist)
{
    if (!policy) {
        return false;
    }
    ensure_lock();
    if (s_policy_lock) {
        xSemaphoreTake(s_policy_lock, portMAX_DELAY);
    }
    commit_policy_locked(policy);
    if (s_policy_lock) {
        xSemaphoreGive(s_policy_lock);
    }

    if (persist) {
        char json[BADGE_DISPLAY_POLICY_JSON_MAX] = {0};
        badge_display_policy_to_json(policy, json, sizeof(json));
        if (!nvs_config_set_string(BADGE_DISPLAY_POLICY_NVS_KEY, json)) {
            ESP_LOGW(TAG, "Failed to persist badge display policy");
            return false;
        }
    }
    ESP_LOGI(TAG, "Badge display policy updated hash=%lu persist=%d",
             (unsigned long)s_policy_hash, persist ? 1 : 0);
    return true;
}

void badge_display_policy_runtime_reset(bool persist)
{
    badge_display_policy_t policy;
    badge_display_policy_defaults(&policy);
    (void)badge_display_policy_runtime_set(&policy, persist);
}

size_t badge_display_policy_runtime_json(char *out, size_t out_len)
{
    return badge_display_policy_to_json(badge_display_policy_runtime_get(),
                                        out, out_len);
}

size_t badge_display_policy_runtime_command_json(char *out, size_t out_len)
{
    return badge_display_policy_to_command_json(
        badge_display_policy_runtime_get(),
        badge_display_policy_runtime_hash(),
        out,
        out_len
    );
}

void badge_display_policy_runtime_note_filtered(
    badge_display_policy_class_t cls)
{
    if ((int)cls < 0 || cls >= BADGE_DISPLAY_POLICY_CLASS_COUNT) {
        cls = BADGE_DISPLAY_CLASS_SCANNER_STATUS;
    }
    s_filtered_counts[cls]++;
}

uint32_t badge_display_policy_runtime_filtered_count(
    badge_display_policy_class_t cls)
{
    if ((int)cls < 0 || cls >= BADGE_DISPLAY_POLICY_CLASS_COUNT) {
        cls = BADGE_DISPLAY_CLASS_SCANNER_STATUS;
    }
    return s_filtered_counts[cls];
}

void badge_display_policy_runtime_clear_filtered_counts(void)
{
    memset(s_filtered_counts, 0, sizeof(s_filtered_counts));
}

#endif /* FOF_BADGE_VARIANT */
