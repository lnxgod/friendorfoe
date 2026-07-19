#include "unity.h"

#include "firmware_coordinator_migration.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TEST_GENERATION 42U
#define TEST_MANIFEST_CRC 0x13572468U
#define TEST_TARGET_MASK 0x03U

static fof_fw_coord_v2_t schema2_fixture(void)
{
    fof_fw_coord_v2_t blob = {0};
    blob.magic = FOF_FW_COORDINATOR_MAGIC;
    blob.schema = FOF_FW_COORDINATOR_SCHEMA_V2;
    blob.record_size = sizeof(blob);
    blob.generation = TEST_GENERATION;
    blob.manifest_crc32 = TEST_MANIFEST_CRC;
    blob.target_slot_mask = TEST_TARGET_MASK;
    blob.relay_attempts[0] = 1;
    blob.relay_attempts[1] = 2;
    blob.readiness_probe_attempts[0] = 2;
    blob.readiness_probe_attempts[1] = 1;
    blob.slot_state[0] = FOF_FW_COORD_SLOT_AWAITING_CHECK;
    blob.slot_state[1] = FOF_FW_COORD_SLOT_CURRENT;
    blob.crc32 = fof_fw_coordinator_crc32(
        &blob, offsetof(fof_fw_coord_v2_t, crc32));
    return blob;
}

static bool migrate_serialized(const fof_fw_coord_v2_t *source,
                               fof_fw_coord_v3_t *out)
{
    uint8_t serialized[sizeof(*source)] = {0};
    memcpy(serialized, source, sizeof(serialized));
    return fof_fw_coordinator_migrate_v2(
        serialized, sizeof(serialized), TEST_GENERATION,
        TEST_MANIFEST_CRC, TEST_TARGET_MASK, out);
}

void test_fw_coordinator_schema2_layout_and_crc_are_exact(void)
{
    static const uint8_t released_schema2_blob[] = {
        0x01, 0x4c, 0xf3, 0xf0, 0x02, 0x00, 0x20, 0x00,
        0x2a, 0x00, 0x00, 0x00, 0x68, 0x24, 0x57, 0x13,
        0x03, 0x00, 0x00, 0x00, 0x01, 0x02, 0x02, 0x01,
        0x01, 0x06, 0x00, 0x00, 0x6d, 0xea, 0xcb, 0xc0,
    };
    fof_fw_coord_v2_t blob = schema2_fixture();
    uint8_t serialized[sizeof(blob)] = {0};
    memcpy(serialized, &blob, sizeof(serialized));

    TEST_ASSERT_EQUAL_UINT32(32U, sizeof(fof_fw_coord_v2_t));
    TEST_ASSERT_EQUAL_UINT32(68U, sizeof(fof_fw_coord_v3_t));
    TEST_ASSERT_EQUAL_HEX32(
        0xc0cbea6dU,
        fof_fw_coordinator_crc32(released_schema2_blob, 28U));
    TEST_ASSERT_TRUE(fof_fw_coordinator_v2_blob_valid(
        released_schema2_blob, sizeof(released_schema2_blob)));
    TEST_ASSERT_TRUE(fof_fw_coordinator_v2_blob_valid(
        serialized, sizeof(serialized)));

    serialized[offsetof(fof_fw_coord_v2_t, generation)] ^= 1U;
    TEST_ASSERT_FALSE(fof_fw_coordinator_v2_blob_valid(
        serialized, sizeof(serialized)));
}

void test_fw_coordinator_schema2_migration_preserves_budgets_and_demotes_ready(void)
{
    fof_fw_coord_v2_t blob = schema2_fixture();
    fof_fw_coord_v3_t migrated = {0};
    blob.slot_state[0] = FOF_FW_COORD_SLOT_OFFERED;
    blob.slot_state[1] = FOF_FW_COORD_SLOT_READY_QUEUED;
    blob.pending_mask = 0x02U;
    blob.crc32 = fof_fw_coordinator_crc32(
        &blob, offsetof(fof_fw_coord_v2_t, crc32));

    TEST_ASSERT_TRUE(migrate_serialized(&blob, &migrated));
    TEST_ASSERT_EQUAL_UINT32(TEST_GENERATION, migrated.generation);
    TEST_ASSERT_EQUAL_HEX32(TEST_MANIFEST_CRC, migrated.manifest_crc32);
    TEST_ASSERT_EQUAL_HEX8(TEST_TARGET_MASK, migrated.target_slot_mask);
    TEST_ASSERT_EQUAL_UINT8(0U, migrated.pending_mask);
    TEST_ASSERT_EQUAL_UINT8(1U, migrated.relay_attempts[0]);
    TEST_ASSERT_EQUAL_UINT8(2U, migrated.relay_attempts[1]);
    TEST_ASSERT_EQUAL_UINT8(2U, migrated.readiness_probe_attempts[0]);
    TEST_ASSERT_EQUAL_UINT8(1U, migrated.readiness_probe_attempts[1]);
    TEST_ASSERT_EQUAL_UINT8(FOF_FW_COORD_SLOT_AWAITING_CHECK,
                            migrated.slot_state[0]);
    TEST_ASSERT_EQUAL_UINT8(FOF_FW_COORD_SLOT_AWAITING_CHECK,
                            migrated.slot_state[1]);
    TEST_ASSERT_EQUAL_STRING("", migrated.bound_hardware_id[0]);
    TEST_ASSERT_EQUAL_STRING("", migrated.bound_hardware_id[1]);
}

void test_fw_coordinator_schema2_migration_maps_relaying_to_failed_and_latches_failed(void)
{
    fof_fw_coord_v2_t blob = schema2_fixture();
    fof_fw_coord_v3_t migrated = {0};
    blob.slot_state[0] = FOF_FW_COORD_SLOT_RELAYING;
    blob.slot_state[1] = FOF_FW_COORD_SLOT_FAILED;
    blob.crc32 = fof_fw_coordinator_crc32(
        &blob, offsetof(fof_fw_coord_v2_t, crc32));

    TEST_ASSERT_TRUE(migrate_serialized(&blob, &migrated));
    TEST_ASSERT_EQUAL_UINT8(FOF_FW_COORD_SLOT_FAILED,
                            migrated.slot_state[0]);
    TEST_ASSERT_EQUAL_UINT8(FOF_FW_COORD_SLOT_FAILED,
                            migrated.slot_state[1]);
    TEST_ASSERT_NOT_EQUAL(FOF_FW_COORD_SLOT_RECOVERING,
                          migrated.slot_state[0]);
    TEST_ASSERT_EQUAL_UINT8(1U, migrated.relay_attempts[0]);
    TEST_ASSERT_EQUAL_UINT8(2U, migrated.relay_attempts[1]);
}

void test_fw_coordinator_schema2_migration_preserves_awaiting_terminal_and_fail_closed(void)
{
    fof_fw_coord_v2_t blob = schema2_fixture();
    fof_fw_coord_v3_t migrated = {0};

    TEST_ASSERT_TRUE(migrate_serialized(&blob, &migrated));
    TEST_ASSERT_EQUAL_UINT8(FOF_FW_COORD_SLOT_AWAITING_CHECK,
                            migrated.slot_state[0]);
    TEST_ASSERT_EQUAL_UINT8(FOF_FW_COORD_SLOT_CURRENT,
                            migrated.slot_state[1]);

    blob.target_slot_mask = 0;
    blob.fail_closed = 1;
    blob.slot_state[0] = FOF_FW_COORD_SLOT_FAILED;
    blob.slot_state[1] = FOF_FW_COORD_SLOT_FAILED;
    blob.relay_attempts[0] = FOF_FW_COORD_RELAY_MAX_ATTEMPTS;
    blob.relay_attempts[1] = FOF_FW_COORD_RELAY_MAX_ATTEMPTS;
    blob.readiness_probe_attempts[0] = FOF_FW_COORD_READY_MAX_PROBES;
    blob.readiness_probe_attempts[1] = FOF_FW_COORD_READY_MAX_PROBES;
    blob.crc32 = fof_fw_coordinator_crc32(
        &blob, offsetof(fof_fw_coord_v2_t, crc32));

    TEST_ASSERT_TRUE(migrate_serialized(&blob, &migrated));
    TEST_ASSERT_EQUAL_UINT8(1U, migrated.fail_closed);
    TEST_ASSERT_EQUAL_UINT8(0U, migrated.target_slot_mask);
    TEST_ASSERT_EQUAL_UINT8(FOF_FW_COORD_SLOT_FAILED,
                            migrated.slot_state[0]);
    TEST_ASSERT_EQUAL_UINT8(FOF_FW_COORD_SLOT_FAILED,
                            migrated.slot_state[1]);
}

void test_fw_coordinator_schema2_migration_rejects_unknown_or_mismatched_records(void)
{
    fof_fw_coord_v2_t blob = schema2_fixture();
    fof_fw_coord_v3_t migrated = {0};
    uint8_t serialized[sizeof(blob)] = {0};

    blob.schema = 77U;
    blob.crc32 = fof_fw_coordinator_crc32(
        &blob, offsetof(fof_fw_coord_v2_t, crc32));
    TEST_ASSERT_FALSE(migrate_serialized(&blob, &migrated));

    blob = schema2_fixture();
    memcpy(serialized, &blob, sizeof(serialized));
    TEST_ASSERT_FALSE(fof_fw_coordinator_migrate_v2(
        serialized, sizeof(serialized), TEST_GENERATION + 1U,
        TEST_MANIFEST_CRC, TEST_TARGET_MASK, &migrated));
    TEST_ASSERT_FALSE(fof_fw_coordinator_migrate_v2(
        serialized, sizeof(serialized), TEST_GENERATION,
        TEST_MANIFEST_CRC ^ 1U, TEST_TARGET_MASK, &migrated));
    TEST_ASSERT_FALSE(fof_fw_coordinator_migrate_v2(
        serialized, sizeof(serialized), TEST_GENERATION,
        TEST_MANIFEST_CRC, 0x01U, &migrated));
    TEST_ASSERT_FALSE(fof_fw_coordinator_migrate_v2(
        serialized, sizeof(serialized) - 1U, TEST_GENERATION,
        TEST_MANIFEST_CRC, TEST_TARGET_MASK, &migrated));
}
