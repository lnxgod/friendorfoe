#pragma once

#include "firmware_operation_token.h"
#include "fw_manifest_store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef UNIT_TESTING
#ifndef FOF_FW_STORE_INFO_T_DEFINED
#define FOF_FW_STORE_INFO_T_DEFINED
typedef struct {
    bool stored;
    uint32_t generation;
    uint8_t target_slot_mask;
    uint32_t manifest_crc32;
    uint32_t size;
    uint32_t checksum;
    char version[32];
    char name[32];
    char project[33];
    char hardware[33];
    char sha256[65];
    char partition[16];
} fw_store_info_t;
#endif

typedef struct esp_partition_t {
    char label[17];
    uint32_t size;
} esp_partition_t;
#else
#include "fw_store.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    fw_store_info_t manifest;
    const esp_partition_t *partition;
    uint32_t generation;
    uint32_t manifest_crc32;
    fw_operation_token_t operation_token;
    int scanner_id;
    bool token_owned;
    bool uart_lease_owned;
} fw_relay_prepared_t;

typedef enum {
    FW_RELAY_PREPARED = 0,
    FW_RELAY_BUSY,
    FW_RELAY_NO_MANIFEST,
    FW_RELAY_GENERATION_CHANGED,
    FW_RELAY_PARTITION_INVALID,
    FW_RELAY_IMAGE_INVALID,
    FW_RELAY_CLEAR_STALE,
    FW_RELAY_STORAGE_ERROR,
} fw_relay_prepare_result_t;

typedef struct {
    bool (*token_acquire)(fw_operation_token_t *out_token);
    bool (*token_release)(fw_operation_token_t token);
    bool (*uart_lease_acquire)(int scanner_id);
    void (*uart_lease_release)(int scanner_id);
    fw_store_read_result_t (*read_committed)(fw_store_info_t *out);
    const esp_partition_t *(*partition_for_snapshot)(
        const fw_store_info_t *snapshot);
    bool (*validate_image)(
        const esp_partition_t *partition,
        const fw_store_info_t *snapshot);
    fw_manifest_clear_result_t (*clear_if_current)(
        uint32_t expected_generation,
        uint32_t expected_manifest_crc32);
} fw_relay_prepare_hooks_t;

fw_relay_prepare_result_t fw_relay_prepare_for_scanner(
    int scanner_id,
    uint32_t expected_generation,
    fw_relay_prepared_t *out);

/**
 * Release in reverse acquisition order. If operation-token release is
 * rejected, the token and ownership flag remain intact so the caller can
 * retry without manufacturing or losing ownership.
 */
bool fw_relay_prepared_release(fw_relay_prepared_t *prepared);

#ifdef UNIT_TESTING
void fw_relay_prepare_set_hooks_for_test(
    const fw_relay_prepare_hooks_t *hooks);
#endif

#ifdef __cplusplus
}
#endif
