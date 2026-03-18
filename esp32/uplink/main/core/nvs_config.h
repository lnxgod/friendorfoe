#pragma once

/**
 * Friend or Foe -- Uplink NVS Configuration
 *
 * Persistent key-value storage for runtime-configurable parameters.
 * Falls back to compile-time defaults in config.h when a key is
 * not found in NVS.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize NVS configuration subsystem.
 * Opens (or creates) the "fof_config" NVS namespace.
 */
void nvs_config_init(void);

/**
 * Read a string value from NVS.
 *
 * @param key       NVS key name
 * @param buf       Output buffer
 * @param buf_size  Size of output buffer
 * @return true if the key was found in NVS, false if using default or error
 */
bool nvs_config_get_string(const char *key, char *buf, size_t buf_size);

/**
 * Write a string value to NVS.
 *
 * @param key    NVS key name
 * @param value  Null-terminated string to store
 * @return true on success
 */
bool nvs_config_set_string(const char *key, const char *value);

/** Get WiFi SSID (NVS key "wifi_ssid", default CONFIG_WIFI_SSID) */
bool nvs_config_get_wifi_ssid(char *buf, size_t buf_size);

/** Get WiFi password (NVS key "wifi_pass", default CONFIG_WIFI_PASSWORD) */
bool nvs_config_get_wifi_password(char *buf, size_t buf_size);

/** Get backend URL (NVS key "backend_url", default CONFIG_BACKEND_URL) */
bool nvs_config_get_backend_url(char *buf, size_t buf_size);

/** Get device ID (NVS key "device_id", default CONFIG_DEVICE_ID) */
bool nvs_config_get_device_id(char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif
