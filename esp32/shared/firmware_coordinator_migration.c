#include "firmware_coordinator_migration.h"

#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(fof_fw_coord_v2_t) == 32U,
               "schema 2 coordinator layout changed");
_Static_assert(sizeof(fof_fw_coord_v3_t) == 68U,
               "schema 3 coordinator layout changed");

uint32_t fof_fw_coordinator_crc32(const void *data, size_t size)
{
    if (!data && size != 0U) {
        return 0;
    }

    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8U; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

bool fof_fw_coordinator_v2_blob_valid(const void *serialized,
                                      size_t serialized_size)
{
    if (!serialized || serialized_size != sizeof(fof_fw_coord_v2_t)) {
        return false;
    }

    fof_fw_coord_v2_t blob = {0};
    memcpy(&blob, serialized, sizeof(blob));
    if (blob.magic != FOF_FW_COORDINATOR_MAGIC ||
        blob.schema != FOF_FW_COORDINATOR_SCHEMA_V2 ||
        blob.record_size != sizeof(blob) || blob.generation == 0U ||
        blob.fail_closed > 1U ||
        (blob.target_slot_mask &
         (uint8_t)~FOF_FW_COORDINATOR_TARGET_ALL) != 0U ||
        (blob.pending_mask & (uint8_t)~blob.target_slot_mask) != 0U ||
        (blob.fail_closed && blob.target_slot_mask != 0U) ||
        (!blob.fail_closed && blob.target_slot_mask == 0U) ||
        blob.crc32 != fof_fw_coordinator_crc32(
            &blob, offsetof(fof_fw_coord_v2_t, crc32))) {
        return false;
    }

    for (int scanner_id = 0;
         scanner_id < FOF_FW_COORDINATOR_SCANNER_COUNT; ++scanner_id) {
        uint8_t bit = (uint8_t)(1U << scanner_id);
        uint8_t state = blob.slot_state[scanner_id];
        bool requested = (blob.target_slot_mask & bit) != 0U;
        if (state > FOF_FW_COORD_SLOT_NEWER_SKIPPED ||
            blob.relay_attempts[scanner_id] >
                FOF_FW_COORD_RELAY_MAX_ATTEMPTS ||
            blob.readiness_probe_attempts[scanner_id] >
                FOF_FW_COORD_READY_MAX_PROBES) {
            return false;
        }
        if (blob.fail_closed) {
            if (state != FOF_FW_COORD_SLOT_FAILED) {
                return false;
            }
            continue;
        }
        if ((!requested && state != FOF_FW_COORD_SLOT_EXCLUDED) ||
            (requested && state == FOF_FW_COORD_SLOT_EXCLUDED) ||
            (((blob.pending_mask & bit) != 0U) !=
             (state == FOF_FW_COORD_SLOT_READY_QUEUED))) {
            return false;
        }
    }
    return true;
}

bool fof_fw_coordinator_migrate_v2(const void *serialized,
                                   size_t serialized_size,
                                   uint32_t expected_generation,
                                   uint32_t expected_manifest_crc32,
                                   uint8_t expected_target_slot_mask,
                                   fof_fw_coord_v3_t *out)
{
    if (!out || expected_generation == 0U ||
        expected_target_slot_mask == 0U ||
        (expected_target_slot_mask &
         (uint8_t)~FOF_FW_COORDINATOR_TARGET_ALL) != 0U ||
        !fof_fw_coordinator_v2_blob_valid(serialized, serialized_size)) {
        return false;
    }

    fof_fw_coord_v2_t source = {0};
    memcpy(&source, serialized, sizeof(source));
    if (source.generation != expected_generation ||
        source.manifest_crc32 != expected_manifest_crc32 ||
        (!source.fail_closed &&
         source.target_slot_mask != expected_target_slot_mask)) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->magic = FOF_FW_COORDINATOR_MAGIC;
    out->schema = FOF_FW_COORDINATOR_SCHEMA_V3;
    out->record_size = sizeof(*out);
    out->generation = source.generation;
    out->manifest_crc32 = source.manifest_crc32;
    out->target_slot_mask = source.target_slot_mask;
    out->fail_closed = source.fail_closed;
    memcpy(out->relay_attempts, source.relay_attempts,
           sizeof(out->relay_attempts));
    memcpy(out->readiness_probe_attempts,
           source.readiness_probe_attempts,
           sizeof(out->readiness_probe_attempts));

    for (int scanner_id = 0;
         scanner_id < FOF_FW_COORDINATOR_SCANNER_COUNT; ++scanner_id) {
        uint8_t state = source.slot_state[scanner_id];
        if (state == FOF_FW_COORD_SLOT_OFFERED ||
            state == FOF_FW_COORD_SLOT_READY_QUEUED) {
            state = FOF_FW_COORD_SLOT_AWAITING_CHECK;
        } else if (state == FOF_FW_COORD_SLOT_RELAYING) {
            state = FOF_FW_COORD_SLOT_FAILED;
        }
        out->slot_state[scanner_id] = state;
    }
    out->crc32 = fof_fw_coordinator_crc32(
        out, offsetof(fof_fw_coord_v3_t, crc32));
    return true;
}
