#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FW_MANIFEST_NVS_NAMESPACE "fof_config"

#define FW_MANIFEST_KEY_SIZE "fw_size"
#define FW_MANIFEST_KEY_CHECKSUM "fw_cksum"
#define FW_MANIFEST_KEY_CRC32 "fw_crc32"
#define FW_MANIFEST_KEY_VERSION "fw_ver"
#define FW_MANIFEST_KEY_NAME "fw_name"
#define FW_MANIFEST_KEY_PARTITION "fw_part"
#define FW_MANIFEST_KEY_VALID "fw_valid"
#define FW_MANIFEST_KEY_GENERATION "fw_gen"
#define FW_MANIFEST_KEY_MANIFEST_CRC32 "fw_mcrc"
#define FW_MANIFEST_KEY_SHA256 "fw_sha256"
#define FW_MANIFEST_KEY_PROJECT "fw_project"
#define FW_MANIFEST_KEY_HARDWARE "fw_hw"
#define FW_MANIFEST_KEY_SLOT_MASK "fw_slotmask"
#define FW_MANIFEST_KEY_COORDINATOR "fw_coord"

#define FW_MANIFEST_COMMITTED_MAGIC 0xF0F34A11u

typedef enum {
    FW_MANIFEST_CLEARED = 0,
    FW_MANIFEST_ALREADY_INVALID,
    FW_MANIFEST_NOT_CURRENT,
    FW_MANIFEST_IO_ERROR_RESULT,
} fw_manifest_clear_result_t;

typedef enum {
    FW_STORE_READ_COMMITTED = 0,
    FW_STORE_READ_NO_MANIFEST,
    FW_STORE_READ_ERROR,
} fw_store_read_result_t;

/**
 * Under the already-held firmware-operation token, invalidate only the exact
 * committed generation/manifest-CRC tuple. FW_MANIFEST_KEY_VALID is the sole
 * authoritative invalidation write; ESP-IDF NVS setters take effect
 * immediately, so this helper makes no multi-key transaction claim.
 */
fw_manifest_clear_result_t fw_store_clear_if_current(
    uint32_t expected_generation,
    uint32_t expected_manifest_crc32);

#ifdef UNIT_TESTING
typedef uint32_t fw_manifest_handle_t;

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

void fw_manifest_store_set_ops_for_test(
    const fw_manifest_store_ops_t *ops);
#endif

#ifdef __cplusplus
}
#endif
