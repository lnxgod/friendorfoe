#ifndef BACKEND_OTA_JOURNAL_H
#define BACKEND_OTA_JOURNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_ota_identity.h"
#include "backend_ota_operation_id.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_OTA_JOURNAL_STORAGE_KEY "ota_journal"

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
#define BACKEND_OTA_JOURNAL_SCHEMA UINT32_C(2)
#define BACKEND_OTA_JOURNAL_CANONICAL_SIZE 506U
#else
#define BACKEND_OTA_JOURNAL_SCHEMA UINT32_C(1)
#define BACKEND_OTA_JOURNAL_CANONICAL_SIZE 347U
#endif

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

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
typedef enum {
    BACKEND_OTA_JOURNAL_ACTION_PROBE = 0,
    BACKEND_OTA_JOURNAL_ACTION_APPLY,
} backend_ota_journal_action_t;

typedef enum {
    BACKEND_OTA_JOURNAL_PROGRESS_METADATA = 0,
    BACKEND_OTA_JOURNAL_PROGRESS_DOWNLOAD,
    BACKEND_OTA_JOURNAL_PROGRESS_VALIDATE,
    BACKEND_OTA_JOURNAL_PROGRESS_STAGE,
    BACKEND_OTA_JOURNAL_PROGRESS_UART_RELAY,
    BACKEND_OTA_JOURNAL_PROGRESS_REBOOT_WAIT,
    BACKEND_OTA_JOURNAL_PROGRESS_CONVERGENCE,
} backend_ota_journal_progress_stage_t;

typedef enum {
    BACKEND_OTA_JOURNAL_CHECKPOINT_COMMAND_ACCEPTED = 0,
    BACKEND_OTA_JOURNAL_CHECKPOINT_METADATA_VALIDATED,
    BACKEND_OTA_JOURNAL_CHECKPOINT_DOWNLOAD,
    BACKEND_OTA_JOURNAL_CHECKPOINT_IMAGE_STAGED,
    BACKEND_OTA_JOURNAL_CHECKPOINT_UART_RELAY,
    BACKEND_OTA_JOURNAL_CHECKPOINT_REBOOT_WAIT,
    BACKEND_OTA_JOURNAL_CHECKPOINT_CONVERGENCE,
    BACKEND_OTA_JOURNAL_CHECKPOINT_TERMINAL,
} backend_ota_journal_checkpoint_t;
#endif

typedef struct {
    uint32_t schema;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    bool has_operation_id;
#endif
    backend_ota_operation_id_t operation_id;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    backend_ota_journal_action_t action;
    uint32_t expected_size;
    char expected_sha256[65];
    uint8_t expected_uplink_mac[6];
    uint32_t expected_uplink_boot_id;
    bool has_accepted_probe_receipt;
    uint8_t accepted_probe_receipt_sha256[32];
    uint32_t command_next_sequence;
    uint32_t event_sequence;
    bool progress_initialized;
    backend_ota_journal_progress_stage_t progress_stage;
    uint32_t progress_received;
    uint32_t progress_total;
    uint32_t progress_retry_count;
    backend_ota_journal_checkpoint_t checkpoint;
    bool has_manifest;
#endif
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

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
typedef bool (*backend_ota_journal_erase_exact_fn)(
    void *context, const char *key);
#endif

typedef struct {
    void *context;
    backend_ota_journal_load_fn load;
    backend_ota_journal_store_fn store;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    backend_ota_journal_erase_exact_fn erase_exact;
#endif
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

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
typedef enum {
    BACKEND_OTA_JOURNAL_STARTUP_EMPTY = 0,
    BACKEND_OTA_JOURNAL_STARTUP_RESTART_METADATA,
    BACKEND_OTA_JOURNAL_STARTUP_RESTART_DOWNLOAD,
    BACKEND_OTA_JOURNAL_STARTUP_ROLL_BACK,
    BACKEND_OTA_JOURNAL_STARTUP_WAIT_REBOOT,
    BACKEND_OTA_JOURNAL_STARTUP_CHECK_CONVERGENCE,
    BACKEND_OTA_JOURNAL_STARTUP_TERMINAL,
    BACKEND_OTA_JOURNAL_STARTUP_BLOCKED,
} backend_ota_journal_startup_action_t;

backend_ota_journal_startup_action_t backend_ota_journal_startup_recover(
    const backend_ota_journal_storage_t *storage,
    backend_ota_journal_record_t *out);

/* Attended recovery deletes only BACKEND_OTA_JOURNAL_STORAGE_KEY. */
bool backend_ota_journal_attended_recover(
    const backend_ota_journal_storage_t *storage);
#endif

#ifdef __cplusplus
}
#endif

#endif
