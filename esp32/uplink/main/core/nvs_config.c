/**
 * Friend or Foe -- Uplink NVS Configuration Implementation
 *
 * Persistent key-value storage backed by ESP-IDF NVS.
 * Falls back to compile-time defaults from config.h when keys are absent.
 */

#include "nvs_config.h"
#include "config.h"

#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "nvs_cfg";

#define NVS_NAMESPACE "fof_config"

static nvs_handle_t s_nvs_handle;
static bool         s_initialized = false;

/* ── Init ──────────────────────────────────────────────────────────────── */

void nvs_config_init(void)
{
    if (s_initialized) {
        return;
    }

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s",
                 NVS_NAMESPACE, esp_err_to_name(err));
        return;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "NVS config initialized (namespace='%s')", NVS_NAMESPACE);
}

/* ── Generic get/set ───────────────────────────────────────────────────── */

bool nvs_config_get_string(const char *key, char *buf, size_t buf_size)
{
    if (!s_initialized || !key || !buf || buf_size == 0) {
        return false;
    }

    size_t required = buf_size;
    esp_err_t err = nvs_get_str(s_nvs_handle, key, buf, &required);
    if (err == ESP_OK) {
        ESP_LOGD(TAG, "NVS get '%s' = '%s'", key, buf);
        return true;
    }

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGD(TAG, "NVS key '%s' not found, using default", key);
    } else {
        ESP_LOGW(TAG, "NVS get '%s' error: %s", key, esp_err_to_name(err));
    }

    return false;
}

bool nvs_config_set_string(const char *key, const char *value)
{
    if (!s_initialized || !key || !value) {
        return false;
    }

    esp_err_t err = nvs_set_str(s_nvs_handle, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS set '%s' failed: %s", key, esp_err_to_name(err));
        return false;
    }

    err = nvs_commit(s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "NVS set '%s' = '%s'", key, value);
    return true;
}

/* ── Convenience getters with defaults ─────────────────────────────────── */

static void get_with_default(const char *nvs_key, const char *default_val,
                             char *buf, size_t buf_size)
{
    if (!nvs_config_get_string(nvs_key, buf, buf_size)) {
        strncpy(buf, default_val, buf_size - 1);
        buf[buf_size - 1] = '\0';
    }
}

bool nvs_config_get_wifi_ssid(char *buf, size_t buf_size)
{
    get_with_default("wifi_ssid", CONFIG_WIFI_SSID, buf, buf_size);
    return true;
}

bool nvs_config_get_wifi_password(char *buf, size_t buf_size)
{
    get_with_default("wifi_pass", CONFIG_WIFI_PASSWORD, buf, buf_size);
    return true;
}

bool nvs_config_get_backend_url(char *buf, size_t buf_size)
{
    get_with_default("backend_url", CONFIG_BACKEND_URL, buf, buf_size);
    return true;
}

bool nvs_config_get_device_id(char *buf, size_t buf_size)
{
    get_with_default("device_id", CONFIG_DEVICE_ID, buf, buf_size);
    return true;
}
