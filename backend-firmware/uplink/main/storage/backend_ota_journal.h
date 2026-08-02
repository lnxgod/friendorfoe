#ifndef BACKEND_OTA_JOURNAL_H
#define BACKEND_OTA_JOURNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_ota_identity.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_OTA_JOURNAL_SCHEMA UINT32_C(1)
#define BACKEND_OTA_JOURNAL_CANONICAL_SIZE 347U

typedef enum {
    BACKEND_OTA_COMPONENT_UPLINK = 0,
    BACKEND_OTA_COMPONENT_SCANNER0,
    BACKEND_OTA_COMPONENT_SCANNER1,
} backend_ota_component_t;

typedef enum {
    BACKEND_OTA_NEWER_ONLY = 0,
    BACKEND_OTA_SAME_VERSION_RECOVERY,
} backend_ota_apply_mode_t;

typedef enum {
    BACKEND_OTA_PHASE_ACCEPTED = 0,
    BACKEND_OTA_PHASE_WRITING,
    BACKEND_OTA_PHASE_REBOOT_PENDING,
    BACKEND_OTA_PHASE_CONVERGENCE_PENDING,
    BACKEND_OTA_PHASE_COMPLETE,
    BACKEND_OTA_PHASE_FAILED,
} backend_ota_phase_t;

typedef struct {
    uint32_t schema;
    uint32_t operation_id;
    backend_ota_component_t component;
    int8_t component_slot;
    backend_ota_apply_mode_t apply_mode;
    char catalog_name[40];
    backend_ota_manifest_t manifest;
    uint8_t uplink_mac[6];
    uint32_t uplink_boot_id;
    uint8_t expected_target_mac[6];
    uint8_t actual_target_mac[6];
    uint32_t expected_target_boot_id;
    uint32_t actual_target_boot_id;
    uint32_t expected_topology_generation;
    uint32_t actual_topology_generation;
    backend_ota_phase_t phase;
    uint32_t image_writes_before;
    uint32_t image_writes_after;
    uint32_t boot_id_after;
    bool rollback_clear;
    bool converged;
    uint32_t record_crc32;
} backend_ota_journal_record_t;

typedef struct {
    size_t length;
    uint8_t bytes[BACKEND_OTA_JOURNAL_CANONICAL_SIZE];
} backend_ota_journal_blob_t;

typedef enum {
    BACKEND_OTA_JOURNAL_VALID = 0,
    BACKEND_OTA_JOURNAL_INVALID_ARGUMENT,
    BACKEND_OTA_JOURNAL_INVALID_LENGTH,
    BACKEND_OTA_JOURNAL_INVALID_CRC,
    BACKEND_OTA_JOURNAL_INVALID_FIELD,
} backend_ota_journal_validation_t;

backend_ota_journal_validation_t backend_ota_journal_validate(
    const backend_ota_journal_record_t *record);

bool backend_ota_journal_encode(
    const backend_ota_journal_record_t *record,
    backend_ota_journal_blob_t *out);

backend_ota_journal_validation_t backend_ota_journal_decode(
    const uint8_t *bytes,
    size_t length,
    backend_ota_journal_record_t *out);

typedef enum {
    BACKEND_OTA_JOURNAL_IO_OK = 0,
    BACKEND_OTA_JOURNAL_IO_NOT_FOUND,
    BACKEND_OTA_JOURNAL_IO_ERROR,
} backend_ota_journal_io_result_t;

typedef backend_ota_journal_io_result_t (*backend_ota_journal_load_fn)(
    void *context,
    uint8_t *out,
    size_t capacity,
    size_t *out_length);

/* Success means the complete blob is durably committed. There is
 * intentionally no erase callback: journal recovery never erases NVS. */
typedef bool (*backend_ota_journal_store_fn)(
    void *context,
    const uint8_t *bytes,
    size_t length);

typedef struct {
    void *context;
    backend_ota_journal_load_fn load;
    backend_ota_journal_store_fn store;
} backend_ota_journal_storage_t;

typedef enum {
    BACKEND_OTA_JOURNAL_LOAD_PRESENT = 0,
    BACKEND_OTA_JOURNAL_LOAD_NOT_FOUND,
    BACKEND_OTA_JOURNAL_LOAD_CORRUPT,
    BACKEND_OTA_JOURNAL_LOAD_IO_ERROR,
} backend_ota_journal_load_result_t;

backend_ota_journal_load_result_t backend_ota_journal_load(
    const backend_ota_journal_storage_t *storage,
    backend_ota_journal_record_t *out);

typedef enum {
    BACKEND_OTA_JOURNAL_PERSIST_COMMITTED = 0,
    BACKEND_OTA_JOURNAL_PERSIST_MUTATION_AUTHORIZED,
    BACKEND_OTA_JOURNAL_PERSIST_ALREADY_DURABLE,
    BACKEND_OTA_JOURNAL_PERSIST_CONFLICT,
    BACKEND_OTA_JOURNAL_PERSIST_INVALID,
    BACKEND_OTA_JOURNAL_PERSIST_IO_ERROR,
} backend_ota_journal_persist_result_t;

backend_ota_journal_persist_result_t backend_ota_journal_persist_accepted(
    const backend_ota_journal_storage_t *storage,
    const backend_ota_journal_record_t *accepted);

/* Only a freshly committed ACCEPTED -> WRITING transition returns
 * PERSIST_MUTATION_AUTHORIZED. A replay returns ALREADY_DURABLE. */
backend_ota_journal_persist_result_t backend_ota_journal_persist_transition(
    const backend_ota_journal_storage_t *storage,
    const backend_ota_journal_record_t *next,
    uint32_t live_uplink_boot_id);

typedef enum {
    BACKEND_OTA_JOURNAL_RECOVERY_FAIL_BEFORE_MUTATION = 0,
    BACKEND_OTA_JOURNAL_RECOVERY_POST_WRITE_CHECKS,
    BACKEND_OTA_JOURNAL_RECOVERY_REBOOT_CHECKS,
    BACKEND_OTA_JOURNAL_RECOVERY_CONVERGENCE_CHECKS,
    BACKEND_OTA_JOURNAL_RECOVERY_COMPLETE,
    BACKEND_OTA_JOURNAL_RECOVERY_FAILED,
    BACKEND_OTA_JOURNAL_RECOVERY_INVALID,
} backend_ota_journal_recovery_action_t;

backend_ota_journal_recovery_action_t backend_ota_journal_recovery_action(
    const backend_ota_journal_record_t *record);

#ifdef __cplusplus
}
#endif

#endif
