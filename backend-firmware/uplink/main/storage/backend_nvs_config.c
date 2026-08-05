#include "backend_nvs_config.h"

#include <stdio.h>
#include <string.h>

#ifndef UNIT_TESTING
#include "esp_err.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#endif

#define BACKEND_NVS_NAMESPACE "fof_config"
#define BACKEND_NVS_CONFIG_KEY "backend_config"
#define BACKEND_NVS_MIGRATION_GENERATION UINT32_C(1)

typedef enum {
    BACKEND_CONFIG_LOAD_PRESENT,
    BACKEND_CONFIG_LOAD_MISSING,
    BACKEND_CONFIG_LOAD_ERROR,
} backend_config_load_result_t;

static backend_nvs_storage_health_t s_storage_health =
    BACKEND_NVS_STORAGE_UNINITIALIZED;

#ifdef UNIT_TESTING
static backend_nvs_config_hooks_t s_hooks;
static bool s_hooks_installed;
#else
static nvs_handle_t s_nvs_handle;
#endif

backend_nvs_storage_health_t backend_nvs_config_storage_health(void)
{
    return s_storage_health;
}

#ifdef UNIT_TESTING
void backend_nvs_config_set_test_hooks(
    const backend_nvs_config_hooks_t *hooks)
{
    memset(&s_hooks, 0, sizeof(s_hooks));
    s_hooks_installed = false;
    s_storage_health = BACKEND_NVS_STORAGE_UNINITIALIZED;
    if (hooks) {
        s_hooks = *hooks;
        s_hooks_installed = true;
    }
}

void backend_nvs_config_reset_test_hooks(void)
{
    memset(&s_hooks, 0, sizeof(s_hooks));
    s_hooks_installed = false;
    s_storage_health = BACKEND_NVS_STORAGE_UNINITIALIZED;
}
#endif

static backend_nvs_io_result_t storage_init(void)
{
#ifdef UNIT_TESTING
    if (!s_hooks_installed || !s_hooks.init) {
        return BACKEND_NVS_IO_ERROR;
    }
    return s_hooks.init(s_hooks.context);
#else
    const esp_err_t init_result = nvs_flash_init();
    if (init_result == ESP_ERR_NVS_NO_FREE_PAGES) {
        return BACKEND_NVS_IO_NO_FREE_PAGES;
    }
    if (init_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        return BACKEND_NVS_IO_NEW_VERSION;
    }
    if (init_result != ESP_OK) {
        return BACKEND_NVS_IO_ERROR;
    }
    const esp_err_t open_result = nvs_open(
        BACKEND_NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    return open_result == ESP_OK ? BACKEND_NVS_IO_OK : BACKEND_NVS_IO_ERROR;
#endif
}

static bool ensure_storage_ready(void)
{
    if (s_storage_health == BACKEND_NVS_STORAGE_READY) {
        return true;
    }
    if (s_storage_health == BACKEND_NVS_STORAGE_FATAL) {
        return false;
    }
    if (storage_init() == BACKEND_NVS_IO_OK) {
        s_storage_health = BACKEND_NVS_STORAGE_READY;
        return true;
    }
    s_storage_health = BACKEND_NVS_STORAGE_FATAL;
    return false;
}

static backend_nvs_io_result_t storage_read_blob(
    uint8_t *out, size_t capacity, size_t *out_length)
{
#ifdef UNIT_TESTING
    if (!s_hooks.read_blob) {
        return BACKEND_NVS_IO_ERROR;
    }
    return s_hooks.read_blob(s_hooks.context, BACKEND_NVS_NAMESPACE,
                             BACKEND_NVS_CONFIG_KEY, out, capacity,
                             out_length);
#else
    size_t required = 0;
    esp_err_t result = nvs_get_blob(
        s_nvs_handle, BACKEND_NVS_CONFIG_KEY, NULL, &required);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return BACKEND_NVS_IO_NOT_FOUND;
    }
    if (result != ESP_OK || required == 0 || required > capacity ||
        required > BACKEND_CONFIG_BLOB_MAX) {
        return BACKEND_NVS_IO_ERROR;
    }
    size_t actual = required;
    result = nvs_get_blob(
        s_nvs_handle, BACKEND_NVS_CONFIG_KEY, out, &actual);
    if (result != ESP_OK || actual != required) {
        return BACKEND_NVS_IO_ERROR;
    }
    *out_length = actual;
    return BACKEND_NVS_IO_OK;
#endif
}

static backend_nvs_io_result_t storage_write_blob(
    const uint8_t *bytes, size_t length)
{
#ifdef UNIT_TESTING
    if (!s_hooks.write_blob) {
        return BACKEND_NVS_IO_ERROR;
    }
    return s_hooks.write_blob(s_hooks.context, BACKEND_NVS_NAMESPACE,
                              BACKEND_NVS_CONFIG_KEY, bytes, length);
#else
    return nvs_set_blob(s_nvs_handle, BACKEND_NVS_CONFIG_KEY, bytes, length) ==
                   ESP_OK
        ? BACKEND_NVS_IO_OK : BACKEND_NVS_IO_ERROR;
#endif
}

static backend_nvs_io_result_t storage_commit(void)
{
#ifdef UNIT_TESTING
    if (!s_hooks.commit) {
        return BACKEND_NVS_IO_ERROR;
    }
    return s_hooks.commit(s_hooks.context);
#else
    return nvs_commit(s_nvs_handle) == ESP_OK
        ? BACKEND_NVS_IO_OK : BACKEND_NVS_IO_ERROR;
#endif
}

static backend_nvs_io_result_t storage_read_string(
    const char *key, char *out, size_t capacity)
{
#ifdef UNIT_TESTING
    if (!s_hooks.read_string) {
        return BACKEND_NVS_IO_ERROR;
    }
    return s_hooks.read_string(s_hooks.context, BACKEND_NVS_NAMESPACE,
                               key, out, capacity);
#else
    size_t required = capacity;
    const esp_err_t result = nvs_get_str(
        s_nvs_handle, key, out, &required);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return BACKEND_NVS_IO_NOT_FOUND;
    }
    if (result != ESP_OK || required == 0 || required > capacity ||
        memchr(out, '\0', required) == NULL) {
        return BACKEND_NVS_IO_ERROR;
    }
    return BACKEND_NVS_IO_OK;
#endif
}

static bool storage_read_sta_mac(uint8_t out[6])
{
#ifdef UNIT_TESTING
    return s_hooks.read_sta_mac &&
           s_hooks.read_sta_mac(s_hooks.context, out);
#else
    return esp_read_mac(out, ESP_MAC_WIFI_STA) == ESP_OK;
#endif
}

static backend_config_load_result_t load_record(
    backend_config_record_t *out)
{
    if (!out || !ensure_storage_ready()) {
        return BACKEND_CONFIG_LOAD_ERROR;
    }
    uint8_t bytes[BACKEND_CONFIG_BLOB_MAX];
    size_t length = 0;
    const backend_nvs_io_result_t read_result = storage_read_blob(
        bytes, sizeof(bytes), &length);
    if (read_result == BACKEND_NVS_IO_NOT_FOUND) {
        return BACKEND_CONFIG_LOAD_MISSING;
    }
    if (read_result != BACKEND_NVS_IO_OK || length == 0 ||
        length > sizeof(bytes)) {
        return BACKEND_CONFIG_LOAD_ERROR;
    }
    backend_config_record_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    if (backend_config_decode_canonical(bytes, length, &decoded) !=
        BACKEND_CONFIG_VALID) {
        return BACKEND_CONFIG_LOAD_ERROR;
    }
    *out = decoded;
    return BACKEND_CONFIG_LOAD_PRESENT;
}

bool backend_config_load(backend_config_record_t *out)
{
    if (!out) {
        return false;
    }
    backend_config_record_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    if (load_record(&loaded) != BACKEND_CONFIG_LOAD_PRESENT) {
        return false;
    }
    *out = loaded;
    return true;
}

bool backend_config_commit(const backend_config_record_t *record)
{
    if (backend_config_validate(record) != BACKEND_CONFIG_VALID) {
        return false;
    }
    backend_config_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    if (!backend_config_encode_canonical(record, &blob) ||
        blob.length == 0 || blob.length > BACKEND_CONFIG_BLOB_MAX ||
        !ensure_storage_ready()) {
        return false;
    }
    if (storage_write_blob(blob.bytes, blob.length) != BACKEND_NVS_IO_OK) {
        return false;
    }
    return storage_commit() == BACKEND_NVS_IO_OK;
}

static bool read_optional_legacy_string(
    const char *key, char *out, size_t capacity, bool *out_present)
{
    memset(out, 0, capacity);
    if (out_present) {
        *out_present = false;
    }
    const backend_nvs_io_result_t result = storage_read_string(
        key, out, capacity);
    if (result == BACKEND_NVS_IO_NOT_FOUND) {
        return true;
    }
    if (result != BACKEND_NVS_IO_OK ||
        memchr(out, '\0', capacity) == NULL) {
        return false;
    }
    if (out_present) {
        *out_present = true;
    }
    return true;
}

static bool read_legacy_config(
    backend_legacy_config_t *legacy,
    bool *wifi_password_present,
    bool *wifi_pass_present)
{
    if (!legacy || !wifi_password_present || !wifi_pass_present) {
        return false;
    }
    memset(legacy, 0, sizeof(*legacy));
    return read_optional_legacy_string(
               "wifi_ssid", legacy->wifi_ssid,
               sizeof(legacy->wifi_ssid), NULL) &&
           read_optional_legacy_string(
               "wifi_password", legacy->wifi_password,
               sizeof(legacy->wifi_password), wifi_password_present) &&
           read_optional_legacy_string(
               "wifi_pass", legacy->wifi_pass,
               sizeof(legacy->wifi_pass), wifi_pass_present) &&
           read_optional_legacy_string(
               "backend_url", legacy->backend_url,
               sizeof(legacy->backend_url), NULL) &&
           read_optional_legacy_string(
               "device_id", legacy->device_id,
               sizeof(legacy->device_id), NULL) &&
           read_optional_legacy_string(
               "ap_pass", legacy->ap_pass,
               sizeof(legacy->ap_pass), NULL);
}

static bool ensure_legacy_device_id(backend_legacy_config_t *legacy)
{
    if (legacy->device_id[0] != '\0') {
        return true;
    }
    uint8_t sta_mac[6] = {0};
    if (!storage_read_sta_mac(sta_mac)) {
        return false;
    }
    const int written = snprintf(
        legacy->device_id, sizeof(legacy->device_id),
        "uplink_%02X%02X%02X", sta_mac[3], sta_mac[4], sta_mac[5]);
    return written == 13;
}

bool backend_config_load_or_migrate(backend_config_record_t *out)
{
    if (!out) {
        return false;
    }
    backend_config_record_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    const backend_config_load_result_t load_result = load_record(&loaded);
    if (load_result == BACKEND_CONFIG_LOAD_PRESENT) {
        *out = loaded;
        return true;
    }
    if (load_result != BACKEND_CONFIG_LOAD_MISSING) {
        return false;
    }

    backend_legacy_config_t legacy;
    bool wifi_password_present = false;
    bool wifi_pass_present = false;
    if (!read_legacy_config(&legacy, &wifi_password_present,
                            &wifi_pass_present)) {
        return false;
    }
    if (wifi_password_present && wifi_pass_present &&
        strcmp(legacy.wifi_password, legacy.wifi_pass) != 0) {
        return false;
    }
    if (!ensure_legacy_device_id(&legacy)) {
        return false;
    }
    backend_config_record_t migrated;
    memset(&migrated, 0, sizeof(migrated));
    if (!backend_config_migrate_legacy(
            &legacy, BACKEND_NVS_MIGRATION_GENERATION, &migrated) ||
        !backend_config_commit(&migrated)) {
        return false;
    }
    *out = migrated;
    return true;
}
