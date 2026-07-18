#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FOF_FW_COORDINATOR_MAGIC 0xF0F34C01u
#define FOF_FW_COORDINATOR_SCHEMA_V2 2u
#define FOF_FW_COORDINATOR_SCHEMA_V3 3u
#define FOF_FW_COORDINATOR_SCANNER_COUNT 2
#define FOF_FW_COORDINATOR_TARGET_ALL 0x03u
#define FOF_FW_COORD_RELAY_MAX_ATTEMPTS 3u
#define FOF_FW_COORD_READY_MAX_PROBES 3u

typedef enum {
    FOF_FW_COORD_SLOT_EXCLUDED = 0,
    FOF_FW_COORD_SLOT_AWAITING_CHECK = 1,
    FOF_FW_COORD_SLOT_OFFERED = 2,
    FOF_FW_COORD_SLOT_READY_QUEUED = 3,
    FOF_FW_COORD_SLOT_RELAYING = 4,
    FOF_FW_COORD_SLOT_CONVERGED = 5,
    FOF_FW_COORD_SLOT_CURRENT = 6,
    FOF_FW_COORD_SLOT_REFUSED = 7,
    FOF_FW_COORD_SLOT_FAILED = 8,
    FOF_FW_COORD_SLOT_NEWER_SKIPPED = 9,
    FOF_FW_COORD_SLOT_RECOVERING = 10,
} fof_fw_coord_slot_state_t;

/* Exact schema written by released coordinator schema 2 firmware. */
typedef struct {
    uint32_t magic;
    uint16_t schema;
    uint16_t record_size;
    uint32_t generation;
    uint32_t manifest_crc32;
    uint8_t target_slot_mask;
    uint8_t pending_mask;
    uint8_t fail_closed;
    uint8_t reserved0;
    uint8_t relay_attempts[FOF_FW_COORDINATOR_SCANNER_COUNT];
    uint8_t readiness_probe_attempts[FOF_FW_COORDINATOR_SCANNER_COUNT];
    uint8_t slot_state[FOF_FW_COORDINATOR_SCANNER_COUNT];
    uint8_t reserved[2];
    uint32_t crc32;
} fof_fw_coord_v2_t;

typedef struct {
    uint32_t magic;
    uint16_t schema;
    uint16_t record_size;
    uint32_t generation;
    uint32_t manifest_crc32;
    uint8_t target_slot_mask;
    uint8_t pending_mask;
    uint8_t fail_closed;
    uint8_t reserved0;
    uint8_t relay_attempts[FOF_FW_COORDINATOR_SCANNER_COUNT];
    uint8_t readiness_probe_attempts[FOF_FW_COORDINATOR_SCANNER_COUNT];
    uint8_t slot_state[FOF_FW_COORDINATOR_SCANNER_COUNT];
    uint8_t reserved[2];
    char bound_hardware_id[FOF_FW_COORDINATOR_SCANNER_COUNT][18];
    uint32_t crc32;
} fof_fw_coord_v3_t;

uint32_t fof_fw_coordinator_crc32(const void *data, size_t size);

bool fof_fw_coordinator_v2_blob_valid(const void *serialized,
                                      size_t serialized_size);

bool fof_fw_coordinator_migrate_v2(const void *serialized,
                                   size_t serialized_size,
                                   uint32_t expected_generation,
                                   uint32_t expected_manifest_crc32,
                                   uint8_t expected_target_slot_mask,
                                   fof_fw_coord_v3_t *out);

#ifdef __cplusplus
}
#endif
