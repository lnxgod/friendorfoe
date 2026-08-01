#include "fw_manifest_store.h"

#ifndef UNIT_TESTING
#include "nvs.h"
#endif

#include <stddef.h>

#ifndef UNIT_TESTING
typedef nvs_handle_t fw_manifest_handle_t;

typedef enum {
    FW_MANIFEST_IO_OK = 0,
    FW_MANIFEST_IO_NOT_FOUND,
    FW_MANIFEST_IO_ERROR,
} fw_manifest_io_result_t;

typedef struct {
    fw_manifest_io_result_t (*open)(
        const char *namespace_name, bool readwrite,
        fw_manifest_handle_t *out_handle);
    fw_manifest_io_result_t (*get_u32)(
        fw_manifest_handle_t handle, const char *key, uint32_t *out_value);
    fw_manifest_io_result_t (*set_u32)(
        fw_manifest_handle_t handle, const char *key, uint32_t value);
    void (*close)(fw_manifest_handle_t handle);
} fw_manifest_store_ops_t;

static fw_manifest_io_result_t production_open(
    const char *namespace_name, bool readwrite,
    fw_manifest_handle_t *out_handle)
{
    esp_err_t result = nvs_open(
        namespace_name, readwrite ? NVS_READWRITE : NVS_READONLY,
        out_handle);
    return result == ESP_OK ? FW_MANIFEST_IO_OK : FW_MANIFEST_IO_ERROR;
}

static fw_manifest_io_result_t production_get_u32(
    fw_manifest_handle_t handle, const char *key, uint32_t *out_value)
{
    esp_err_t result = nvs_get_u32(handle, key, out_value);
    if (result == ESP_OK) {
        return FW_MANIFEST_IO_OK;
    }
    return result == ESP_ERR_NVS_NOT_FOUND
        ? FW_MANIFEST_IO_NOT_FOUND : FW_MANIFEST_IO_ERROR;
}

static fw_manifest_io_result_t production_set_u32(
    fw_manifest_handle_t handle, const char *key, uint32_t value)
{
    return nvs_set_u32(handle, key, value) == ESP_OK
        ? FW_MANIFEST_IO_OK : FW_MANIFEST_IO_ERROR;
}

static void production_close(fw_manifest_handle_t handle)
{
    nvs_close(handle);
}

static const fw_manifest_store_ops_t s_production_ops = {
    .open = production_open,
    .get_u32 = production_get_u32,
    .set_u32 = production_set_u32,
    .close = production_close,
};

static const fw_manifest_store_ops_t *s_ops = &s_production_ops;
#else
static const fw_manifest_store_ops_t *s_ops;
#endif

fw_manifest_clear_result_t fw_store_clear_if_current(
    uint32_t expected_generation,
    uint32_t expected_manifest_crc32)
{
    const fw_manifest_store_ops_t *ops = s_ops;
    fw_manifest_handle_t handle = 0;
    fw_manifest_clear_result_t result = FW_MANIFEST_IO_ERROR_RESULT;
    uint32_t valid = 0;
    uint32_t generation = 0;
    uint32_t manifest_crc32 = 0;
    bool opened = false;

    if (!ops || !ops->open || !ops->get_u32 || !ops->set_u32 ||
        !ops->close) {
        return FW_MANIFEST_IO_ERROR_RESULT;
    }
    if (ops->open(
            FW_MANIFEST_NVS_NAMESPACE, true, &handle) !=
        FW_MANIFEST_IO_OK) {
        return FW_MANIFEST_IO_ERROR_RESULT;
    }
    opened = true;

    fw_manifest_io_result_t io = ops->get_u32(
        handle, FW_MANIFEST_KEY_VALID, &valid);
    if (io == FW_MANIFEST_IO_NOT_FOUND) {
        result = FW_MANIFEST_ALREADY_INVALID;
        goto cleanup;
    }
    if (io != FW_MANIFEST_IO_OK) {
        goto cleanup;
    }
    if (valid != FW_MANIFEST_COMMITTED_MAGIC) {
        result = FW_MANIFEST_ALREADY_INVALID;
        goto cleanup;
    }

    if (ops->get_u32(
            handle, FW_MANIFEST_KEY_GENERATION, &generation) !=
            FW_MANIFEST_IO_OK ||
        ops->get_u32(
            handle, FW_MANIFEST_KEY_MANIFEST_CRC32, &manifest_crc32) !=
            FW_MANIFEST_IO_OK) {
        goto cleanup;
    }
    if (generation != expected_generation ||
        manifest_crc32 != expected_manifest_crc32) {
        result = FW_MANIFEST_NOT_CURRENT;
        goto cleanup;
    }

    if (ops->set_u32(
            handle, FW_MANIFEST_KEY_VALID, 0U) != FW_MANIFEST_IO_OK) {
        goto cleanup;
    }
    result = FW_MANIFEST_CLEARED;

cleanup:
    if (opened) {
        ops->close(handle);
    }
    return result;
}

#ifdef UNIT_TESTING
void fw_manifest_store_set_ops_for_test(
    const fw_manifest_store_ops_t *ops)
{
    s_ops = ops;
}
#endif
