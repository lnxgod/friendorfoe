#include "backend_glasses_settings.h"

#include <stdint.h>

#include <esp_log.h>
#include <nvs.h>

static const char *TAG = "glasses_settings";

#define NVS_NAMESPACE  "fof_config"
#define NVS_KEY_ENABLE "glasses_det"

static bool s_enabled = true;
static bool s_nvs_loaded;

bool backend_glasses_settings_is_enabled(void)
{
    if (!s_nvs_loaded) {
        nvs_handle_t handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
            uint8_t value = 1;
            (void)nvs_get_u8(handle, NVS_KEY_ENABLE, &value);
            s_enabled = value != 0;
            nvs_close(handle);
        }
        s_nvs_loaded = true;
        ESP_LOGI(TAG, "Glasses detection %s (NVS)",
                 s_enabled ? "ENABLED" : "DISABLED");
    }
    return s_enabled;
}

void backend_glasses_settings_set_enabled(bool enabled)
{
    s_enabled = enabled;
    s_nvs_loaded = true;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        (void)nvs_set_u8(handle, NVS_KEY_ENABLE, enabled ? 1 : 0);
        (void)nvs_commit(handle);
        nvs_close(handle);
    }
    ESP_LOGI(TAG, "Glasses detection set to %s", enabled ? "ENABLED" : "DISABLED");
}
