#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "backend_ota_journal.h"
#include "../support/backend_test_main.h"

void setUp(void) {}
void tearDown(void) {}

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
#define TEST_JOURNAL_CRC_OFFSET 502U
#define TEST_JOURNAL_PROFILE_SHIFT 159U
#else
#define TEST_JOURNAL_CRC_OFFSET 343U
#define TEST_JOURNAL_PROFILE_SHIFT 0U
#endif

static backend_ota_operation_id_t test_operation_id(uint8_t seed)
{
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    backend_ota_operation_id_t operation_id;
    for (size_t index = 0U; index < sizeof(operation_id.bytes); ++index) {
        operation_id.bytes[index] = (uint8_t)(seed + index);
    }
    return operation_id;
#else
    return UINT32_C(0x04030200) | seed;
#endif
}

static void set_record_operation(
    backend_ota_journal_record_t *record, uint8_t seed)
{
    record->operation_id = test_operation_id(seed);
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    record->has_operation_id = true;
#endif
}

static void clear_record_operation(backend_ota_journal_record_t *record)
{
    memset(&record->operation_id, 0, sizeof(record->operation_id));
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    record->has_operation_id = false;
#endif
}

static void change_record_operation(backend_ota_journal_record_t *record)
{
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    record->operation_id.bytes[15] ^= UINT8_C(1);
#else
    ++record->operation_id;
#endif
}

static void assert_operation_equal(
    const backend_ota_operation_id_t *expected,
    const backend_ota_operation_id_t *actual)
{
    TEST_ASSERT_TRUE(backend_ota_operation_id_equal(expected, actual));
}

static backend_ota_journal_record_t valid_uplink_record(void)
{
    backend_ota_journal_record_t record;
    memset(&record, 0, sizeof(record));
    record.schema = BACKEND_OTA_JOURNAL_SCHEMA;
    set_record_operation(&record, 1U);
    record.component = BACKEND_OTA_COMPONENT_UPLINK;
    record.component_slot = -1;
    record.apply_mode = BACKEND_OTA_NEWER_ONLY;
    strcpy(record.catalog_name, FOF_BACKEND_UPLINK_TARGET);
    strcpy(record.manifest.target, FOF_BACKEND_UPLINK_TARGET);
    strcpy(record.manifest.project, FOF_BACKEND_UPLINK_PROJECT);
    strcpy(record.manifest.hardware, FOF_BACKEND_HARDWARE);
    strcpy(record.manifest.version, "0.1.1-backend");
    record.manifest.image_size = UINT32_C(0x00123456);
    record.manifest.crc32 = 0U;
    strcpy(record.manifest.sha256,
           "0123456789abcdef0123456789abcdef"
           "0123456789abcdef0123456789abcdef");
    record.manifest.generation = 9U;
    record.manifest.allow_same_version = false;
    const uint8_t uplink_mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xF0};
    memcpy(record.uplink_mac, uplink_mac, sizeof(uplink_mac));
    record.uplink_boot_id = 77U;
    memcpy(record.expected_target_mac, uplink_mac, sizeof(uplink_mac));
    memcpy(record.actual_target_mac, uplink_mac, sizeof(uplink_mac));
    record.expected_target_boot_id = 77U;
    record.actual_target_boot_id = 77U;
    record.expected_topology_generation = 4U;
    record.actual_topology_generation = 4U;
    record.phase = BACKEND_OTA_PHASE_ACCEPTED;
    record.image_writes_before = 11U;
    record.image_writes_after = 11U;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    record.action = BACKEND_OTA_JOURNAL_ACTION_APPLY;
    record.expected_size = record.manifest.image_size;
    memcpy(record.expected_sha256, record.manifest.sha256,
           sizeof(record.expected_sha256));
    memcpy(record.expected_uplink_mac, record.uplink_mac,
           sizeof(record.expected_uplink_mac));
    record.expected_uplink_boot_id = record.uplink_boot_id;
    record.has_accepted_probe_receipt = true;
    for (size_t index = 0U;
         index < sizeof(record.accepted_probe_receipt_sha256); ++index) {
        record.accepted_probe_receipt_sha256[index] =
            (uint8_t)(UINT8_C(0x80) + index);
    }
    record.command_next_sequence = 2U;
    record.event_sequence = 2U;
    record.progress_initialized = true;
    record.progress_stage = BACKEND_OTA_JOURNAL_PROGRESS_STAGE;
    record.progress_received = record.expected_size;
    record.progress_total = record.expected_size;
    record.checkpoint = BACKEND_OTA_JOURNAL_CHECKPOINT_IMAGE_STAGED;
    record.has_manifest = true;
#endif
    return record;
}

static backend_ota_journal_record_t valid_scanner_record(
    backend_ota_component_t component)
{
    backend_ota_journal_record_t record = valid_uplink_record();
    record.component = component;
    record.component_slot = component == BACKEND_OTA_COMPONENT_SCANNER0 ? 0 : 1;
    strcpy(record.catalog_name, FOF_BACKEND_SCANNER_TARGET);
    strcpy(record.manifest.target, FOF_BACKEND_SCANNER_TARGET);
    strcpy(record.manifest.project, FOF_BACKEND_SCANNER_PROJECT);
    const uint8_t scanner_mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};
    memcpy(record.expected_target_mac, scanner_mac, sizeof(scanner_mac));
    memcpy(record.actual_target_mac, scanner_mac, sizeof(scanner_mac));
    record.expected_target_boot_id = 91U;
    record.actual_target_boot_id = 91U;
    return record;
}

static void set_record_phase(
    backend_ota_journal_record_t *record, backend_ota_phase_t phase)
{
    record->phase = phase;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    switch (phase) {
    case BACKEND_OTA_PHASE_ACCEPTED:
        record->checkpoint = BACKEND_OTA_JOURNAL_CHECKPOINT_IMAGE_STAGED;
        break;
    case BACKEND_OTA_PHASE_WRITING:
        record->checkpoint = BACKEND_OTA_JOURNAL_CHECKPOINT_IMAGE_STAGED;
        break;
    case BACKEND_OTA_PHASE_REBOOT_PENDING:
        record->checkpoint = BACKEND_OTA_JOURNAL_CHECKPOINT_REBOOT_WAIT;
        break;
    case BACKEND_OTA_PHASE_CONVERGENCE_PENDING:
        record->checkpoint = BACKEND_OTA_JOURNAL_CHECKPOINT_CONVERGENCE;
        break;
    case BACKEND_OTA_PHASE_COMPLETE:
    case BACKEND_OTA_PHASE_FAILED:
        record->checkpoint = BACKEND_OTA_JOURNAL_CHECKPOINT_TERMINAL;
        break;
    default:
        break;
    }
#endif
}

void test_profile_record_encodes_fixed_width_without_struct_padding(void)
{
    backend_ota_journal_record_t record = valid_uplink_record();
    backend_ota_journal_blob_t blob;
    memset(&blob, 0xA5, sizeof(blob));

    TEST_ASSERT_TRUE(backend_ota_journal_encode(&record, &blob));
    TEST_ASSERT_EQUAL_UINT(BACKEND_OTA_JOURNAL_CANONICAL_SIZE, blob.length);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BACKEND_OTA_JOURNAL_SCHEMA, blob.bytes[0]);
    TEST_ASSERT_EQUAL_UINT8(0U, blob.bytes[1]);
    TEST_ASSERT_EQUAL_UINT8(0U, blob.bytes[2]);
    TEST_ASSERT_EQUAL_UINT8(0U, blob.bytes[3]);
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    TEST_ASSERT_EQUAL_UINT8(1U, blob.bytes[4]);
    for (size_t index = 0U; index < 16U; ++index) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(index + 1U), blob.bytes[5U + index]);
    }
    TEST_ASSERT_EQUAL_UINT8(BACKEND_OTA_JOURNAL_ACTION_APPLY, blob.bytes[21]);
    TEST_ASSERT_EQUAL_UINT8(1U, blob.bytes[104]);
    TEST_ASSERT_EQUAL_UINT8(1U, blob.bytes[166]);
    TEST_ASSERT_EQUAL_INT8(-1, (int8_t)blob.bytes[171]);
    TEST_ASSERT_EQUAL_UINT8(0x56U, blob.bytes[368]);
    TEST_ASSERT_EQUAL_UINT8(0x34U, blob.bytes[369]);
    TEST_ASSERT_EQUAL_UINT8(0x12U, blob.bytes[370]);
    TEST_ASSERT_EQUAL_UINT8(0x00U, blob.bytes[371]);
#else
    TEST_ASSERT_EQUAL_UINT8(1U, blob.bytes[4]);
    TEST_ASSERT_EQUAL_UINT8(2U, blob.bytes[5]);
    TEST_ASSERT_EQUAL_UINT8(3U, blob.bytes[6]);
    TEST_ASSERT_EQUAL_UINT8(4U, blob.bytes[7]);
    TEST_ASSERT_EQUAL_INT8(-1, (int8_t)blob.bytes[12]);
    TEST_ASSERT_EQUAL_UINT8(0x56U, blob.bytes[209]);
    TEST_ASSERT_EQUAL_UINT8(0x34U, blob.bytes[210]);
    TEST_ASSERT_EQUAL_UINT8(0x12U, blob.bytes[211]);
    TEST_ASSERT_EQUAL_UINT8(0x00U, blob.bytes[212]);
    TEST_ASSERT_EQUAL_UINT8(0U, blob.bytes[213]);
    TEST_ASSERT_EQUAL_UINT8(0U, blob.bytes[214]);
    TEST_ASSERT_EQUAL_UINT8(0U, blob.bytes[215]);
    TEST_ASSERT_EQUAL_UINT8(0U, blob.bytes[216]);
#endif
}

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
void test_fullsize_presence_allows_zero_id_and_rejects_numeric_schema(void)
{
    backend_ota_journal_record_t record = valid_uplink_record();
    memset(&record.operation_id, 0, sizeof(record.operation_id));
    TEST_ASSERT_TRUE(record.has_operation_id);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_VALID, backend_ota_journal_validate(&record));
    backend_ota_journal_blob_t blob;
    TEST_ASSERT_TRUE(backend_ota_journal_encode(&record, &blob));
    TEST_ASSERT_EQUAL_UINT8(1U, blob.bytes[4]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        record.operation_id.bytes, blob.bytes + 5U,
        sizeof(record.operation_id.bytes));

    backend_ota_journal_record_t decoded;
    uint8_t numeric_schema[347] = {0U};
    TEST_ASSERT_EQUAL(BACKEND_OTA_JOURNAL_INVALID_LENGTH,
        backend_ota_journal_decode(
            numeric_schema, sizeof(numeric_schema), &decoded));
}
#endif

static void assert_invalid(const backend_ota_journal_record_t *record)
{
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_INVALID_FIELD,
        backend_ota_journal_validate(record));
    backend_ota_journal_blob_t blob;
    memset(&blob, 0xA5, sizeof(blob));
    TEST_ASSERT_FALSE(backend_ota_journal_encode(record, &blob));
    TEST_ASSERT_EQUAL_UINT(0U, blob.length);
}

void test_required_identifiers_and_known_enums_are_strict(void)
{
    backend_ota_journal_record_t record = valid_uplink_record();
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_VALID, backend_ota_journal_validate(&record));

    clear_record_operation(&record);
    assert_invalid(&record);
    record = valid_uplink_record();
    record.manifest.generation = 0U;
    assert_invalid(&record);
    record = valid_uplink_record();
    record.manifest.image_size = 0U;
    assert_invalid(&record);
    record = valid_uplink_record();
    record.uplink_boot_id = 0U;
    assert_invalid(&record);
    record = valid_uplink_record();
    record.expected_target_boot_id = 0U;
    assert_invalid(&record);
    record = valid_uplink_record();
    record.actual_target_boot_id = 0U;
    assert_invalid(&record);
    record = valid_uplink_record();
    record.expected_topology_generation = 0U;
    assert_invalid(&record);
    record = valid_uplink_record();
    record.actual_topology_generation = 0U;
    assert_invalid(&record);
    record = valid_uplink_record();
    record.component = (backend_ota_component_t)3;
    assert_invalid(&record);
    record = valid_uplink_record();
    record.apply_mode = (backend_ota_apply_mode_t)2;
    assert_invalid(&record);
    record = valid_uplink_record();
    record.phase = (backend_ota_phase_t)6;
    assert_invalid(&record);
}

void test_component_slot_catalog_and_backend_identity_are_exact(void)
{
    backend_ota_journal_record_t scanner0 = valid_scanner_record(
        BACKEND_OTA_COMPONENT_SCANNER0);
    backend_ota_journal_record_t scanner1 = valid_scanner_record(
        BACKEND_OTA_COMPONENT_SCANNER1);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_VALID, backend_ota_journal_validate(&scanner0));
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_VALID, backend_ota_journal_validate(&scanner1));

    backend_ota_journal_record_t record = valid_uplink_record();
    record.component_slot = 0;
    assert_invalid(&record);
    record = scanner0;
    record.component_slot = 1;
    assert_invalid(&record);
    record = scanner1;
    record.component_slot = 0;
    assert_invalid(&record);
    record = valid_uplink_record();
    strcpy(record.manifest.target, "scanner-s3-combo-backend");
    assert_invalid(&record);
    record = scanner0;
    strcpy(record.manifest.project, "fof_backend_uplink");
    assert_invalid(&record);
    record = scanner0;
    strcpy(record.manifest.hardware, "xiao_esp32s3");
    assert_invalid(&record);
    record = scanner0;
    strcpy(record.catalog_name, "uplink-s3-backend");
    assert_invalid(&record);
    record = scanner0;
    record.catalog_name[strlen(record.catalog_name) + 1U] = 'x';
    assert_invalid(&record);
}

void test_version_and_sha_are_canonical_and_manifest_crc_zero_is_valid(void)
{
    backend_ota_journal_record_t record = valid_uplink_record();
    TEST_ASSERT_EQUAL_UINT32(0U, record.manifest.crc32);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_VALID, backend_ota_journal_validate(&record));

    record.manifest.version[0] = '\0';
    assert_invalid(&record);
    record = valid_uplink_record();
    memset(record.manifest.version, 'v', sizeof(record.manifest.version));
    assert_invalid(&record);
    record = valid_uplink_record();
    record.manifest.sha256[0] = 'A';
    assert_invalid(&record);
    record = valid_uplink_record();
    record.manifest.sha256[0] = 'g';
    assert_invalid(&record);
    record = valid_uplink_record();
    record.manifest.sha256[63] = '\0';
    assert_invalid(&record);
    record = valid_uplink_record();
    record.manifest.sha256[64] = '0';
    assert_invalid(&record);
}

void test_apply_mode_and_same_version_authority_must_match(void)
{
    backend_ota_journal_record_t record = valid_uplink_record();
    record.manifest.allow_same_version = true;
    assert_invalid(&record);
    record = valid_uplink_record();
    record.apply_mode = BACKEND_OTA_SAME_VERSION_RECOVERY;
    assert_invalid(&record);
    record.manifest.allow_same_version = true;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_VALID, backend_ota_journal_validate(&record));
}

void test_mac_and_target_binding_snapshots_are_exact(void)
{
    backend_ota_journal_record_t record = valid_uplink_record();
    memset(record.uplink_mac, 0, sizeof(record.uplink_mac));
    assert_invalid(&record);
    record = valid_uplink_record();
    record.uplink_mac[0] |= UINT8_C(1);
    assert_invalid(&record);
    record = valid_uplink_record();
    record.actual_target_mac[5] ^= UINT8_C(1);
    assert_invalid(&record);
    record = valid_uplink_record();
    ++record.actual_target_boot_id;
    assert_invalid(&record);
    record = valid_uplink_record();
    ++record.actual_topology_generation;
    assert_invalid(&record);
    record = valid_uplink_record();
    record.expected_target_mac[5] ^= UINT8_C(2);
    record.actual_target_mac[5] ^= UINT8_C(2);
    assert_invalid(&record);
    record = valid_uplink_record();
    ++record.expected_target_boot_id;
    ++record.actual_target_boot_id;
    assert_invalid(&record);

    backend_ota_journal_record_t scanner = valid_scanner_record(
        BACKEND_OTA_COMPONENT_SCANNER0);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_VALID, backend_ota_journal_validate(&scanner));
    memcpy(scanner.expected_target_mac, scanner.uplink_mac,
           sizeof(scanner.expected_target_mac));
    memcpy(scanner.actual_target_mac, scanner.uplink_mac,
           sizeof(scanner.actual_target_mac));
    assert_invalid(&scanner);
    scanner = valid_scanner_record(BACKEND_OTA_COMPONENT_SCANNER0);
    memset(scanner.expected_target_mac, 0xFF,
           sizeof(scanner.expected_target_mac));
    memcpy(scanner.actual_target_mac, scanner.expected_target_mac,
           sizeof(scanner.actual_target_mac));
    assert_invalid(&scanner);
}

void test_each_phase_has_strict_non_replayable_evidence(void)
{
    backend_ota_journal_record_t record = valid_uplink_record();
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_VALID, backend_ota_journal_validate(&record));

    set_record_phase(&record, BACKEND_OTA_PHASE_WRITING);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_VALID, backend_ota_journal_validate(&record));

    set_record_phase(&record, BACKEND_OTA_PHASE_REBOOT_PENDING);
    record.image_writes_after = 12U;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_VALID, backend_ota_journal_validate(&record));

    set_record_phase(&record, BACKEND_OTA_PHASE_CONVERGENCE_PENDING);
    record.boot_id_after = 88U;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_VALID, backend_ota_journal_validate(&record));

    set_record_phase(&record, BACKEND_OTA_PHASE_COMPLETE);
    record.rollback_clear = true;
    record.converged = true;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_VALID, backend_ota_journal_validate(&record));

    record = valid_uplink_record();
    set_record_phase(&record, BACKEND_OTA_PHASE_FAILED);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_VALID, backend_ota_journal_validate(&record));

    record = valid_uplink_record();
    record.image_writes_after = 12U;
    assert_invalid(&record);
    record = valid_uplink_record();
    set_record_phase(&record, BACKEND_OTA_PHASE_WRITING);
    record.image_writes_after = 12U;
    assert_invalid(&record);
    record = valid_uplink_record();
    set_record_phase(&record, BACKEND_OTA_PHASE_REBOOT_PENDING);
    assert_invalid(&record);
    record = valid_uplink_record();
    set_record_phase(&record, BACKEND_OTA_PHASE_REBOOT_PENDING);
    record.image_writes_after = 12U;
    record.boot_id_after = 88U;
    assert_invalid(&record);
    record = valid_uplink_record();
    set_record_phase(&record, BACKEND_OTA_PHASE_CONVERGENCE_PENDING);
    record.image_writes_after = 12U;
    assert_invalid(&record);
    record = valid_uplink_record();
    set_record_phase(&record, BACKEND_OTA_PHASE_CONVERGENCE_PENDING);
    record.image_writes_after = 12U;
    record.boot_id_after = record.actual_target_boot_id;
    assert_invalid(&record);
    record = valid_uplink_record();
    set_record_phase(&record, BACKEND_OTA_PHASE_COMPLETE);
    record.image_writes_after = 12U;
    record.boot_id_after = 88U;
    record.rollback_clear = true;
    assert_invalid(&record);
    record = valid_uplink_record();
    set_record_phase(&record, BACKEND_OTA_PHASE_FAILED);
    record.image_writes_after = 10U;
    assert_invalid(&record);
    record = valid_uplink_record();
    set_record_phase(&record, BACKEND_OTA_PHASE_FAILED);
    record.converged = true;
    assert_invalid(&record);
    record = valid_uplink_record();
    set_record_phase(&record, BACKEND_OTA_PHASE_FAILED);
    record.rollback_clear = true;
    record.converged = true;
    assert_invalid(&record);
}

static uint32_t test_crc32(const uint8_t *bytes, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0U; index < length; ++index) {
        crc ^= bytes[index];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask =
                (uint32_t)-(int32_t)(crc & UINT32_C(1));
            crc = (crc >> 1U) ^ (UINT32_C(0xEDB88320) & mask);
        }
    }
    return ~crc;
}

static void write_u32_le(uint8_t bytes[4], uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
}

static void repair_blob_crc(backend_ota_journal_blob_t *blob)
{
    write_u32_le(
        blob->bytes + TEST_JOURNAL_CRC_OFFSET,
        test_crc32(blob->bytes, TEST_JOURNAL_CRC_OFFSET));
}

typedef struct {
    uint8_t bytes[BACKEND_OTA_JOURNAL_CANONICAL_SIZE + 1U];
    size_t length;
    size_t load_calls;
    size_t store_calls;
    bool present;
    bool fail_load;
    bool fail_store;
} fake_storage_t;

static backend_ota_journal_io_result_t fake_load(
    void *context, uint8_t *out, size_t capacity, size_t *out_length)
{
    fake_storage_t *storage = context;
    ++storage->load_calls;
    if (storage->fail_load || out == NULL || out_length == NULL ||
        storage->length > capacity) {
        return BACKEND_OTA_JOURNAL_IO_ERROR;
    }
    if (!storage->present) {
        return BACKEND_OTA_JOURNAL_IO_NOT_FOUND;
    }
    memcpy(out, storage->bytes, storage->length);
    *out_length = storage->length;
    return BACKEND_OTA_JOURNAL_IO_OK;
}

static bool fake_store(
    void *context, const uint8_t *bytes, size_t length)
{
    fake_storage_t *storage = context;
    ++storage->store_calls;
    if (storage->fail_store || bytes == NULL || length > sizeof(storage->bytes)) {
        return false;
    }
    memcpy(storage->bytes, bytes, length);
    storage->length = length;
    storage->present = true;
    return true;
}

static backend_ota_journal_storage_t storage_adapter(fake_storage_t *storage)
{
    const backend_ota_journal_storage_t adapter = {
        .context = storage,
        .load = fake_load,
        .store = fake_store,
    };
    return adapter;
}

void test_decode_round_trip_is_exact_and_preserves_zero_manifest_crc(void)
{
    backend_ota_journal_record_t record = valid_scanner_record(
        BACKEND_OTA_COMPONENT_SCANNER1);
    record.apply_mode = BACKEND_OTA_SAME_VERSION_RECOVERY;
    record.manifest.allow_same_version = true;
    backend_ota_journal_blob_t first;
    TEST_ASSERT_TRUE(backend_ota_journal_encode(&record, &first));

    backend_ota_journal_record_t decoded;
    memset(&decoded, 0xA5, sizeof(decoded));
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_VALID,
        backend_ota_journal_decode(first.bytes, first.length, &decoded));
    TEST_ASSERT_EQUAL_UINT32(0U, decoded.manifest.crc32);
    TEST_ASSERT_EQUAL(BACKEND_OTA_COMPONENT_SCANNER1, decoded.component);
    TEST_ASSERT_EQUAL_INT8(1, decoded.component_slot);
    TEST_ASSERT_EQUAL(BACKEND_OTA_SAME_VERSION_RECOVERY, decoded.apply_mode);
    TEST_ASSERT_EQUAL_STRING(record.manifest.sha256, decoded.manifest.sha256);
    TEST_ASSERT_EQUAL_UINT32(
        test_crc32(first.bytes, TEST_JOURNAL_CRC_OFFSET),
        decoded.record_crc32);

    backend_ota_journal_blob_t second;
    TEST_ASSERT_TRUE(backend_ota_journal_encode(&decoded, &second));
    TEST_ASSERT_EQUAL_UINT(first.length, second.length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(first.bytes, second.bytes, first.length);
}

#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
void test_computed_zero_record_crc_is_accepted_by_exact_equality(void)
{
    backend_ota_journal_record_t record = valid_uplink_record();
    /* Hand-derived CRC patch for this fixed fixture. It makes the CRC32 over
     * the 343-byte canonical payload exactly zero. */
    record.manifest.crc32 = UINT32_C(0xD68E1C0D);
    backend_ota_journal_blob_t blob;
    TEST_ASSERT_TRUE(backend_ota_journal_encode(&record, &blob));
    TEST_ASSERT_EQUAL_UINT8(0U, blob.bytes[343]);
    TEST_ASSERT_EQUAL_UINT8(0U, blob.bytes[344]);
    TEST_ASSERT_EQUAL_UINT8(0U, blob.bytes[345]);
    TEST_ASSERT_EQUAL_UINT8(0U, blob.bytes[346]);

    backend_ota_journal_record_t decoded;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_VALID,
        backend_ota_journal_decode(blob.bytes, blob.length, &decoded));
    TEST_ASSERT_EQUAL_UINT32(0U, decoded.record_crc32);
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(0xD68E1C0D), decoded.manifest.crc32);
}
#endif

void test_decode_rejects_corrupt_truncated_and_extra_records(void)
{
    backend_ota_journal_record_t record = valid_uplink_record();
    backend_ota_journal_blob_t blob;
    TEST_ASSERT_TRUE(backend_ota_journal_encode(&record, &blob));
    backend_ota_journal_record_t decoded;

    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_INVALID_LENGTH,
        backend_ota_journal_decode(blob.bytes, blob.length - 1U, &decoded));
    uint8_t extra[BACKEND_OTA_JOURNAL_CANONICAL_SIZE + 1U];
    memcpy(extra, blob.bytes, blob.length);
    extra[blob.length] = 0U;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_INVALID_LENGTH,
        backend_ota_journal_decode(extra, sizeof(extra), &decoded));

    blob.bytes[100] ^= UINT8_C(1);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_INVALID_CRC,
        backend_ota_journal_decode(blob.bytes, blob.length, &decoded));
    for (size_t index = 0U; index < sizeof(decoded); ++index) {
        TEST_ASSERT_EQUAL_UINT8(0U, ((const uint8_t *)&decoded)[index]);
    }
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_INVALID_ARGUMENT,
        backend_ota_journal_decode(NULL, blob.length, &decoded));
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_INVALID_ARGUMENT,
        backend_ota_journal_decode(blob.bytes, blob.length, NULL));
}

void test_decode_rejects_noncanonical_fields_even_with_repaired_crc(void)
{
    backend_ota_journal_record_t record = valid_uplink_record();
    backend_ota_journal_blob_t original;
    TEST_ASSERT_TRUE(backend_ota_journal_encode(&record, &original));
    backend_ota_journal_record_t decoded;

    backend_ota_journal_blob_t blob = original;
    blob.bytes[286U + TEST_JOURNAL_PROFILE_SHIFT] = 2U;
    repair_blob_crc(&blob);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_INVALID_FIELD,
        backend_ota_journal_decode(blob.bytes, blob.length, &decoded));

    blob = original;
    blob.bytes[341U + TEST_JOURNAL_PROFILE_SHIFT] = 2U;
    repair_blob_crc(&blob);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_INVALID_FIELD,
        backend_ota_journal_decode(blob.bytes, blob.length, &decoded));

    blob = original;
    blob.bytes[8U + TEST_JOURNAL_PROFILE_SHIFT] = 3U;
    repair_blob_crc(&blob);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_INVALID_FIELD,
        backend_ota_journal_decode(blob.bytes, blob.length, &decoded));

    blob = original;
    blob.bytes[17U + TEST_JOURNAL_PROFILE_SHIFT +
               strlen(FOF_BACKEND_UPLINK_TARGET) + 1U] = 'x';
    repair_blob_crc(&blob);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_INVALID_FIELD,
        backend_ota_journal_decode(blob.bytes, blob.length, &decoded));

    blob = original;
    blob.bytes[217U + TEST_JOURNAL_PROFILE_SHIFT] = 'A';
    repair_blob_crc(&blob);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_INVALID_FIELD,
        backend_ota_journal_decode(blob.bytes, blob.length, &decoded));

    blob = original;
    blob.bytes[303] ^= UINT8_C(1);
    repair_blob_crc(&blob);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_INVALID_FIELD,
        backend_ota_journal_decode(blob.bytes, blob.length, &decoded));
}

void test_callback_load_distinguishes_missing_io_and_corrupt_records(void)
{
    fake_storage_t fake;
    memset(&fake, 0, sizeof(fake));
    backend_ota_journal_storage_t storage = storage_adapter(&fake);
    backend_ota_journal_record_t loaded;
    memset(&loaded, 0xA5, sizeof(loaded));

    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_LOAD_NOT_FOUND,
        backend_ota_journal_load(&storage, &loaded));
    TEST_ASSERT_EQUAL_UINT(1U, fake.load_calls);
    for (size_t index = 0U; index < sizeof(loaded); ++index) {
        TEST_ASSERT_EQUAL_UINT8(0U, ((const uint8_t *)&loaded)[index]);
    }

    backend_ota_journal_record_t record = valid_uplink_record();
    backend_ota_journal_blob_t blob;
    TEST_ASSERT_TRUE(backend_ota_journal_encode(&record, &blob));
    memcpy(fake.bytes, blob.bytes, blob.length);
    fake.length = blob.length;
    fake.present = true;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_LOAD_PRESENT,
        backend_ota_journal_load(&storage, &loaded));
    assert_operation_equal(&record.operation_id, &loaded.operation_id);

    fake.length = blob.length - 1U;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_LOAD_CORRUPT,
        backend_ota_journal_load(&storage, &loaded));
    fake.length = blob.length + 1U;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_LOAD_CORRUPT,
        backend_ota_journal_load(&storage, &loaded));
    fake.length = blob.length;
    fake.bytes[10] ^= UINT8_C(1);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_LOAD_CORRUPT,
        backend_ota_journal_load(&storage, &loaded));

    fake.fail_load = true;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_LOAD_IO_ERROR,
        backend_ota_journal_load(&storage, &loaded));
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_LOAD_IO_ERROR,
        backend_ota_journal_load(NULL, &loaded));
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_LOAD_IO_ERROR,
        backend_ota_journal_load(&storage, NULL));
}

static backend_ota_journal_record_t complete_record(void)
{
    backend_ota_journal_record_t record = valid_uplink_record();
    set_record_phase(&record, BACKEND_OTA_PHASE_COMPLETE);
    record.image_writes_after = record.image_writes_before + 1U;
    record.boot_id_after = record.actual_target_boot_id + 1U;
    record.rollback_clear = true;
    record.converged = true;
    return record;
}

static void set_durable_record(
    fake_storage_t *fake, const backend_ota_journal_record_t *record)
{
    backend_ota_journal_blob_t blob;
    TEST_ASSERT_TRUE(backend_ota_journal_encode(record, &blob));
    memcpy(fake->bytes, blob.bytes, blob.length);
    fake->length = blob.length;
    fake->present = true;
}

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
void test_startup_writing_image_staged_routes_post_write_without_replay(void)
{
    fake_storage_t fake = {0};
    backend_ota_journal_record_t record = valid_scanner_record(
        BACKEND_OTA_COMPONENT_SCANNER0);
    set_record_phase(&record, BACKEND_OTA_PHASE_WRITING);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_CHECKPOINT_IMAGE_STAGED, record.checkpoint);
    set_durable_record(&fake, &record);
    backend_ota_journal_storage_t storage = storage_adapter(&fake);
    backend_ota_journal_record_t recovered;

    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_STARTUP_ROLL_BACK,
        backend_ota_journal_startup_recover(&storage, &recovered));
    TEST_ASSERT_EQUAL(BACKEND_OTA_PHASE_WRITING, recovered.phase);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_CHECKPOINT_IMAGE_STAGED,
        recovered.checkpoint);

    set_record_phase(&record, BACKEND_OTA_PHASE_ACCEPTED);
    set_durable_record(&fake, &record);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_STARTUP_RESTART_DOWNLOAD,
        backend_ota_journal_startup_recover(&storage, &recovered));
}
#endif

void test_accepted_persistence_is_durable_idempotent_and_blocks_active_replay(void)
{
    fake_storage_t fake;
    memset(&fake, 0, sizeof(fake));
    backend_ota_journal_storage_t storage = storage_adapter(&fake);
    backend_ota_journal_record_t accepted = valid_uplink_record();

    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_COMMITTED,
        backend_ota_journal_persist_accepted(&storage, &accepted));
    TEST_ASSERT_TRUE(fake.present);
    TEST_ASSERT_EQUAL_UINT(1U, fake.load_calls);
    TEST_ASSERT_EQUAL_UINT(1U, fake.store_calls);

    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_ALREADY_DURABLE,
        backend_ota_journal_persist_accepted(&storage, &accepted));
    TEST_ASSERT_EQUAL_UINT(2U, fake.load_calls);
    TEST_ASSERT_EQUAL_UINT(1U, fake.store_calls);

    backend_ota_journal_record_t different = accepted;
    change_record_operation(&different);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_CONFLICT,
        backend_ota_journal_persist_accepted(&storage, &different));
    TEST_ASSERT_EQUAL_UINT(1U, fake.store_calls);

    different.phase = BACKEND_OTA_PHASE_WRITING;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_INVALID,
        backend_ota_journal_persist_accepted(&storage, &different));
    TEST_ASSERT_EQUAL_UINT(1U, fake.store_calls);
}

void test_terminal_record_is_replaced_only_by_a_new_authorized_operation(void)
{
    fake_storage_t fake;
    memset(&fake, 0, sizeof(fake));
    backend_ota_journal_storage_t storage = storage_adapter(&fake);
    backend_ota_journal_record_t terminal = complete_record();
    backend_ota_journal_blob_t blob;
    TEST_ASSERT_TRUE(backend_ota_journal_encode(&terminal, &blob));
    memcpy(fake.bytes, blob.bytes, blob.length);
    fake.length = blob.length;
    fake.present = true;

    backend_ota_journal_record_t next = valid_scanner_record(
        BACKEND_OTA_COMPONENT_SCANNER0);
    next.operation_id = terminal.operation_id;
    change_record_operation(&next);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_COMMITTED,
        backend_ota_journal_persist_accepted(&storage, &next));
    TEST_ASSERT_EQUAL_UINT(1U, fake.store_calls);
    backend_ota_journal_record_t loaded;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_LOAD_PRESENT,
        backend_ota_journal_load(&storage, &loaded));
    assert_operation_equal(&next.operation_id, &loaded.operation_id);
    TEST_ASSERT_EQUAL(BACKEND_OTA_PHASE_ACCEPTED, loaded.phase);

    terminal = complete_record();
    TEST_ASSERT_TRUE(backend_ota_journal_encode(&terminal, &blob));
    memcpy(fake.bytes, blob.bytes, blob.length);
    fake.length = blob.length;
    next.operation_id = terminal.operation_id;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_CONFLICT,
        backend_ota_journal_persist_accepted(&storage, &next));
}

void test_accepted_persistence_never_claims_commit_on_io_or_corruption(void)
{
    fake_storage_t fake;
    memset(&fake, 0, sizeof(fake));
    backend_ota_journal_storage_t storage = storage_adapter(&fake);
    backend_ota_journal_record_t accepted = valid_uplink_record();

    fake.fail_load = true;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_IO_ERROR,
        backend_ota_journal_persist_accepted(&storage, &accepted));
    TEST_ASSERT_EQUAL_UINT(0U, fake.store_calls);

    fake.fail_load = false;
    fake.fail_store = true;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_IO_ERROR,
        backend_ota_journal_persist_accepted(&storage, &accepted));
    TEST_ASSERT_FALSE(fake.present);

    fake.fail_store = false;
    fake.present = true;
    fake.length = BACKEND_OTA_JOURNAL_CANONICAL_SIZE;
    memset(fake.bytes, 0, fake.length);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_CONFLICT,
        backend_ota_journal_persist_accepted(&storage, &accepted));

    backend_ota_journal_storage_t missing_store = storage;
    missing_store.store = NULL;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_IO_ERROR,
        backend_ota_journal_persist_accepted(&missing_store, &accepted));
}

void test_phase_commits_gate_one_mutation_and_recovery_never_replays_it(void)
{
    fake_storage_t fake;
    memset(&fake, 0, sizeof(fake));
    backend_ota_journal_storage_t storage = storage_adapter(&fake);
    backend_ota_journal_record_t record = valid_uplink_record();
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_COMMITTED,
        backend_ota_journal_persist_accepted(&storage, &record));
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_RECOVERY_FAIL_BEFORE_MUTATION,
        backend_ota_journal_recovery_action(&record));

    set_record_phase(&record, BACKEND_OTA_PHASE_WRITING);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_MUTATION_AUTHORIZED,
        backend_ota_journal_persist_transition(
            &storage, &record, record.uplink_boot_id));
    const size_t stores_after_authorization = fake.store_calls;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_ALREADY_DURABLE,
        backend_ota_journal_persist_transition(
            &storage, &record, record.uplink_boot_id));
    TEST_ASSERT_EQUAL_UINT(stores_after_authorization, fake.store_calls);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_RECOVERY_POST_WRITE_CHECKS,
        backend_ota_journal_recovery_action(&record));

    set_record_phase(&record, BACKEND_OTA_PHASE_REBOOT_PENDING);
    record.image_writes_after = record.image_writes_before + 1U;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_COMMITTED,
        backend_ota_journal_persist_transition(
            &storage, &record, record.uplink_boot_id));
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_RECOVERY_REBOOT_CHECKS,
        backend_ota_journal_recovery_action(&record));

    set_record_phase(&record, BACKEND_OTA_PHASE_CONVERGENCE_PENDING);
    record.boot_id_after = record.actual_target_boot_id + 1U;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_COMMITTED,
        backend_ota_journal_persist_transition(
            &storage, &record, record.boot_id_after));
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_RECOVERY_CONVERGENCE_CHECKS,
        backend_ota_journal_recovery_action(&record));

    set_record_phase(&record, BACKEND_OTA_PHASE_COMPLETE);
    record.rollback_clear = true;
    record.converged = true;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_COMMITTED,
        backend_ota_journal_persist_transition(
            &storage, &record, record.boot_id_after));
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_RECOVERY_COMPLETE,
        backend_ota_journal_recovery_action(&record));
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_ALREADY_DURABLE,
        backend_ota_journal_persist_transition(
            &storage, &record, record.boot_id_after));

    backend_ota_journal_record_t failed = record;
    set_record_phase(&failed, BACKEND_OTA_PHASE_FAILED);
    failed.rollback_clear = false;
    failed.converged = false;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_CONFLICT,
        backend_ota_journal_persist_transition(
            &storage, &failed, failed.boot_id_after));
}

void test_wrong_boot_illegal_skip_and_store_failure_never_authorize_mutation(void)
{
    fake_storage_t fake;
    memset(&fake, 0, sizeof(fake));
    backend_ota_journal_storage_t storage = storage_adapter(&fake);
    backend_ota_journal_record_t accepted = valid_uplink_record();
    set_durable_record(&fake, &accepted);
    const size_t stores_before = fake.store_calls;

    backend_ota_journal_record_t writing = accepted;
    set_record_phase(&writing, BACKEND_OTA_PHASE_WRITING);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_CONFLICT,
        backend_ota_journal_persist_transition(
            &storage, &writing, accepted.uplink_boot_id + 1U));
    TEST_ASSERT_EQUAL_UINT(stores_before, fake.store_calls);

    backend_ota_journal_record_t skipped = accepted;
    set_record_phase(&skipped, BACKEND_OTA_PHASE_REBOOT_PENDING);
    skipped.image_writes_after = skipped.image_writes_before + 1U;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_CONFLICT,
        backend_ota_journal_persist_transition(
            &storage, &skipped, accepted.uplink_boot_id));

    backend_ota_journal_record_t impossible_failure = accepted;
    set_record_phase(&impossible_failure, BACKEND_OTA_PHASE_FAILED);
    impossible_failure.image_writes_after =
        impossible_failure.image_writes_before + 1U;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_CONFLICT,
        backend_ota_journal_persist_transition(
            &storage, &impossible_failure, accepted.uplink_boot_id));

    fake.fail_store = true;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_IO_ERROR,
        backend_ota_journal_persist_transition(
            &storage, &writing, accepted.uplink_boot_id));
    fake.fail_store = false;
    backend_ota_journal_record_t loaded;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_LOAD_PRESENT,
        backend_ota_journal_load(&storage, &loaded));
    TEST_ASSERT_EQUAL(BACKEND_OTA_PHASE_ACCEPTED, loaded.phase);

    fake.present = false;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_CONFLICT,
        backend_ota_journal_persist_transition(
            &storage, &writing, accepted.uplink_boot_id));
}

void test_transition_rejects_every_immutable_identity_or_binding_drift(void)
{
    fake_storage_t fake;
    memset(&fake, 0, sizeof(fake));
    backend_ota_journal_storage_t storage = storage_adapter(&fake);
    backend_ota_journal_record_t accepted = valid_scanner_record(
        BACKEND_OTA_COMPONENT_SCANNER0);
    set_durable_record(&fake, &accepted);
    backend_ota_journal_record_t next = accepted;
    set_record_phase(&next, BACKEND_OTA_PHASE_WRITING);

    change_record_operation(&next);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_CONFLICT,
        backend_ota_journal_persist_transition(
            &storage, &next, accepted.uplink_boot_id));
    next = accepted;
    set_record_phase(&next, BACKEND_OTA_PHASE_WRITING);
    next.manifest.sha256[0] = '1';
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    next.expected_sha256[0] = '1';
#endif
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_CONFLICT,
        backend_ota_journal_persist_transition(
            &storage, &next, accepted.uplink_boot_id));
    next = accepted;
    set_record_phase(&next, BACKEND_OTA_PHASE_WRITING);
    next.expected_target_mac[5] ^= UINT8_C(2);
    next.actual_target_mac[5] ^= UINT8_C(2);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_CONFLICT,
        backend_ota_journal_persist_transition(
            &storage, &next, accepted.uplink_boot_id));
    next = accepted;
    set_record_phase(&next, BACKEND_OTA_PHASE_WRITING);
    ++next.expected_target_boot_id;
    ++next.actual_target_boot_id;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_CONFLICT,
        backend_ota_journal_persist_transition(
            &storage, &next, accepted.uplink_boot_id));
    next = accepted;
    set_record_phase(&next, BACKEND_OTA_PHASE_WRITING);
    ++next.expected_topology_generation;
    ++next.actual_topology_generation;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_PERSIST_CONFLICT,
        backend_ota_journal_persist_transition(
            &storage, &next, accepted.uplink_boot_id));
}

void test_every_nonterminal_phase_can_fail_durably_without_write_authority(void)
{
    const backend_ota_phase_t phases[] = {
        BACKEND_OTA_PHASE_ACCEPTED,
        BACKEND_OTA_PHASE_WRITING,
        BACKEND_OTA_PHASE_REBOOT_PENDING,
        BACKEND_OTA_PHASE_CONVERGENCE_PENDING,
    };
    for (size_t index = 0U; index < sizeof(phases) / sizeof(phases[0]); ++index) {
        backend_ota_journal_record_t current = valid_uplink_record();
        set_record_phase(&current, phases[index]);
        if (current.phase >= BACKEND_OTA_PHASE_REBOOT_PENDING) {
            current.image_writes_after = current.image_writes_before + 1U;
        }
        if (current.phase == BACKEND_OTA_PHASE_CONVERGENCE_PENDING) {
            current.boot_id_after = current.actual_target_boot_id + 1U;
        }
        fake_storage_t fake;
        memset(&fake, 0, sizeof(fake));
        set_durable_record(&fake, &current);
        backend_ota_journal_storage_t storage = storage_adapter(&fake);
        backend_ota_journal_record_t failed = current;
        set_record_phase(&failed, BACKEND_OTA_PHASE_FAILED);
        TEST_ASSERT_EQUAL(
            BACKEND_OTA_JOURNAL_PERSIST_COMMITTED,
            backend_ota_journal_persist_transition(
                &storage, &failed, current.uplink_boot_id));
        TEST_ASSERT_EQUAL(
            BACKEND_OTA_JOURNAL_RECOVERY_FAILED,
            backend_ota_journal_recovery_action(&failed));
    }

    backend_ota_journal_record_t invalid = valid_uplink_record();
    clear_record_operation(&invalid);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_RECOVERY_INVALID,
        backend_ota_journal_recovery_action(&invalid));
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_profile_record_encodes_fixed_width_without_struct_padding);
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    BACKEND_RUN_TEST(
        test_fullsize_presence_allows_zero_id_and_rejects_numeric_schema);
    BACKEND_RUN_TEST(
        test_startup_writing_image_staged_routes_post_write_without_replay);
#endif
    BACKEND_RUN_TEST(test_required_identifiers_and_known_enums_are_strict);
    BACKEND_RUN_TEST(
        test_component_slot_catalog_and_backend_identity_are_exact);
    BACKEND_RUN_TEST(
        test_version_and_sha_are_canonical_and_manifest_crc_zero_is_valid);
    BACKEND_RUN_TEST(test_apply_mode_and_same_version_authority_must_match);
    BACKEND_RUN_TEST(test_mac_and_target_binding_snapshots_are_exact);
    BACKEND_RUN_TEST(test_each_phase_has_strict_non_replayable_evidence);
    BACKEND_RUN_TEST(
        test_decode_round_trip_is_exact_and_preserves_zero_manifest_crc);
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    BACKEND_RUN_TEST(
        test_computed_zero_record_crc_is_accepted_by_exact_equality);
#endif
    BACKEND_RUN_TEST(test_decode_rejects_corrupt_truncated_and_extra_records);
    BACKEND_RUN_TEST(
        test_decode_rejects_noncanonical_fields_even_with_repaired_crc);
    BACKEND_RUN_TEST(
        test_callback_load_distinguishes_missing_io_and_corrupt_records);
    BACKEND_RUN_TEST(
        test_accepted_persistence_is_durable_idempotent_and_blocks_active_replay);
    BACKEND_RUN_TEST(
        test_terminal_record_is_replaced_only_by_a_new_authorized_operation);
    BACKEND_RUN_TEST(
        test_accepted_persistence_never_claims_commit_on_io_or_corruption);
    BACKEND_RUN_TEST(
        test_phase_commits_gate_one_mutation_and_recovery_never_replays_it);
    BACKEND_RUN_TEST(
        test_wrong_boot_illegal_skip_and_store_failure_never_authorize_mutation);
    BACKEND_RUN_TEST(
        test_transition_rejects_every_immutable_identity_or_binding_drift);
    BACKEND_RUN_TEST(
        test_every_nonterminal_phase_can_fail_durably_without_write_authority);
    return UNITY_END();
}
