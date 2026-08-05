#include <string.h>

#include <unity.h>

#include "backend_identity.h"
#include "backend_ota_event_outbox.h"
#include "backend_ota_identity.h"
#include "backend_test_main.h"

void setUp(void) {}
void tearDown(void) {}

typedef struct {
    bool present[BACKEND_OTA_EVENT_OUTBOX_SLOT_COUNT];
    size_t length[BACKEND_OTA_EVENT_OUTBOX_SLOT_COUNT];
    uint8_t bytes[BACKEND_OTA_EVENT_OUTBOX_SLOT_COUNT]
                 [BACKEND_OTA_EVENT_OUTBOX_SLOT_MAX_BYTES];
    bool fail_store;
    bool tear_store;
    unsigned store_calls;
    unsigned clear_mask;
} memory_slots_t;

static backend_ota_event_outbox_io_result_t memory_load(
    void *context,
    backend_ota_event_outbox_slot_t slot,
    uint8_t *out,
    size_t capacity,
    size_t *out_length)
{
    memory_slots_t *memory = context;
    if (memory == NULL || out == NULL || out_length == NULL ||
        (unsigned)slot >= BACKEND_OTA_EVENT_OUTBOX_SLOT_COUNT) {
        return BACKEND_OTA_EVENT_OUTBOX_IO_ERROR;
    }
    if (!memory->present[slot]) {
        return BACKEND_OTA_EVENT_OUTBOX_IO_NOT_FOUND;
    }
    if (memory->length[slot] > capacity) {
        return BACKEND_OTA_EVENT_OUTBOX_IO_ERROR;
    }
    memcpy(out, memory->bytes[slot], memory->length[slot]);
    *out_length = memory->length[slot];
    return BACKEND_OTA_EVENT_OUTBOX_IO_OK;
}

static bool memory_store(
    void *context,
    backend_ota_event_outbox_slot_t slot,
    const uint8_t *bytes,
    size_t length)
{
    memory_slots_t *memory = context;
    if (memory == NULL || bytes == NULL ||
        (unsigned)slot >= BACKEND_OTA_EVENT_OUTBOX_SLOT_COUNT ||
        length > BACKEND_OTA_EVENT_OUTBOX_SLOT_MAX_BYTES) {
        return false;
    }
    ++memory->store_calls;
    if (memory->tear_store) {
        const size_t torn = length / 2U;
        memcpy(memory->bytes[slot], bytes, torn);
        memory->length[slot] = torn;
        memory->present[slot] = true;
        return false;
    }
    if (memory->fail_store) {
        return false;
    }
    memcpy(memory->bytes[slot], bytes, length);
    memory->length[slot] = length;
    memory->present[slot] = true;
    return true;
}

static bool memory_clear_exact(
    void *context, backend_ota_event_outbox_slot_t slot)
{
    memory_slots_t *memory = context;
    if (memory == NULL ||
        (unsigned)slot >= BACKEND_OTA_EVENT_OUTBOX_SLOT_COUNT) {
        return false;
    }
    memory->clear_mask |= 1U << (unsigned)slot;
    memory->present[slot] = false;
    memory->length[slot] = 0U;
    return true;
}

static backend_ota_event_outbox_storage_t storage_for(memory_slots_t *memory)
{
    return (backend_ota_event_outbox_storage_t) {
        .context = memory,
        .load_slot = memory_load,
        .store_slot = memory_store,
        .clear_exact_slot = memory_clear_exact,
    };
}

static backend_ota_operation_id_t operation(uint8_t seed)
{
    backend_ota_operation_id_t result;
    for (size_t index = 0U; index < sizeof(result.bytes); ++index) {
        result.bytes[index] = (uint8_t)(seed + index);
    }
    return result;
}

static backend_ota_event_outbox_ack_t matching_ack(
    const backend_ota_operation_id_t *operation_id, uint32_t sequence)
{
    return (backend_ota_event_outbox_ack_t) {
        .strict_decoded = true,
        .http_status = 200U,
        .ok = true,
        .has_operation_id = true,
        .operation_id = *operation_id,
        .accepted_sequence = sequence,
        .next_sequence = sequence + 1U,
    };
}

static void test_exact_body_crash_replay_ack_clear_and_next_event(void)
{
    memory_slots_t memory = {0};
    backend_ota_event_outbox_storage_t storage = storage_for(&memory);
    const backend_ota_operation_id_t op = operation(0x10U);
    const uint8_t first[] =
        "{\"schema\":1,\"sequence\":7,\"type\":\"backend_ota_end\"}";
    const uint8_t later[] =
        "{\"schema\":1,\"sequence\":8,\"type\":\"backend_ota_progress\"}";

    TEST_ASSERT_EQUAL(
        BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_COMMITTED,
        backend_ota_event_outbox_enqueue(
            &storage, &op, 7U, first, sizeof(first) - 1U));
    TEST_ASSERT_EQUAL_UINT(1U, memory.store_calls);
    TEST_ASSERT_TRUE(memory.present[BACKEND_OTA_EVENT_OUTBOX_SLOT_0]);

    const size_t durable_length = memory.length[0];
    uint8_t durable[BACKEND_OTA_EVENT_OUTBOX_SLOT_MAX_BYTES];
    memcpy(durable, memory.bytes[0], durable_length);

    /* Reboot before first send and reboot after server acceptance but before
     * local clear both expose only the original durable bytes. */
    backend_ota_event_outbox_snapshot_t rebooted;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_EVENT_OUTBOX_LOAD_PENDING,
        backend_ota_event_outbox_load(&storage, &rebooted));
    TEST_ASSERT_EQUAL_UINT32(7U, rebooted.pending.sequence);
    TEST_ASSERT_EQUAL_UINT(sizeof(first) - 1U, rebooted.pending.body_length);
    TEST_ASSERT_EQUAL_MEMORY(
        first, rebooted.pending.body, sizeof(first) - 1U);
    TEST_ASSERT_EQUAL_MEMORY(durable, memory.bytes[0], durable_length);

    TEST_ASSERT_EQUAL(
        BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_ALREADY_PENDING,
        backend_ota_event_outbox_enqueue(
            &storage, &op, 7U, first, sizeof(first) - 1U));
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_BUSY,
        backend_ota_event_outbox_enqueue(
            &storage, &op, 8U, later, sizeof(later) - 1U));
    TEST_ASSERT_EQUAL_UINT(1U, memory.store_calls);

    backend_ota_event_outbox_ack_t ack = matching_ack(&op, 7U);
    ack.strict_decoded = false;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_EVENT_OUTBOX_ACK_INVALID,
        backend_ota_event_outbox_acknowledge(&storage, &ack));
    ack.strict_decoded = true;
    ack.http_status = 500U;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_EVENT_OUTBOX_ACK_INVALID,
        backend_ota_event_outbox_acknowledge(&storage, &ack));
    ack.http_status = 204U;
    ++ack.accepted_sequence;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_EVENT_OUTBOX_ACK_MISMATCH,
        backend_ota_event_outbox_acknowledge(&storage, &ack));
    --ack.accepted_sequence;

    memory.fail_store = true;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_EVENT_OUTBOX_ACK_IO_ERROR,
        backend_ota_event_outbox_acknowledge(&storage, &ack));
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_EVENT_OUTBOX_LOAD_PENDING,
        backend_ota_event_outbox_load(&storage, &rebooted));
    memory.fail_store = false;

    TEST_ASSERT_EQUAL(
        BACKEND_OTA_EVENT_OUTBOX_ACK_CLEARED,
        backend_ota_event_outbox_acknowledge(&storage, &ack));
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_EVENT_OUTBOX_LOAD_EMPTY,
        backend_ota_event_outbox_load(&storage, &rebooted));
    TEST_ASSERT_EQUAL_UINT32(2U, rebooted.generation);
    TEST_ASSERT_TRUE(backend_ota_operation_id_equal(
        &op, &rebooted.pending.operation_id));
    TEST_ASSERT_EQUAL_UINT32(7U, rebooted.pending.sequence);
    TEST_ASSERT_EQUAL_UINT(0U, rebooted.pending.body_length);
    uint8_t first_digest[32];
    TEST_ASSERT_TRUE(backend_ota_sha256(
        first, sizeof(first) - 1U, first_digest));
    TEST_ASSERT_EQUAL_MEMORY(
        first_digest, rebooted.pending.body_sha256,
        sizeof(first_digest));
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_COMMITTED,
        backend_ota_event_outbox_enqueue(
            &storage, &op, 8U, later, sizeof(later) - 1U));
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_EVENT_OUTBOX_LOAD_PENDING,
        backend_ota_event_outbox_load(&storage, &rebooted));
    TEST_ASSERT_EQUAL_UINT32(8U, rebooted.pending.sequence);
    TEST_ASSERT_EQUAL_MEMORY(
        later, rebooted.pending.body, sizeof(later) - 1U);
}

static void test_torn_corrupt_and_ambiguous_slots_block_until_exact_recovery(void)
{
    memory_slots_t memory = {0};
    backend_ota_event_outbox_storage_t storage = storage_for(&memory);
    const backend_ota_operation_id_t op = operation(0x20U);
    const uint8_t body[] = "{\"sequence\":1}";

    memory.tear_store = true;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_IO_ERROR,
        backend_ota_event_outbox_enqueue(
            &storage, &op, 1U, body, sizeof(body) - 1U));
    backend_ota_event_outbox_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_EVENT_OUTBOX_LOAD_CORRUPT,
        backend_ota_event_outbox_load(&storage, &snapshot));
    memory.tear_store = false;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_CORRUPT,
        backend_ota_event_outbox_enqueue(
            &storage, &op, 1U, body, sizeof(body) - 1U));

    TEST_ASSERT_TRUE(backend_ota_event_outbox_recover_exact_slots(&storage));
    TEST_ASSERT_EQUAL_HEX32(0x3U, memory.clear_mask);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_EVENT_OUTBOX_LOAD_EMPTY,
        backend_ota_event_outbox_load(&storage, &snapshot));

    TEST_ASSERT_EQUAL(
        BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_COMMITTED,
        backend_ota_event_outbox_enqueue(
            &storage, &op, 1U, body, sizeof(body) - 1U));
    memory.present[1] = true;
    memory.length[1] = memory.length[0];
    memcpy(memory.bytes[1], memory.bytes[0], memory.length[0]);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_EVENT_OUTBOX_LOAD_CORRUPT,
        backend_ota_event_outbox_load(&storage, &snapshot));
}

static void test_manual_slot_layout_has_no_padding_and_binds_sha_crc(void)
{
    backend_ota_event_outbox_record_t record = {0};
    record.state = BACKEND_OTA_EVENT_OUTBOX_STATE_PENDING;
    record.generation = 9U;
    record.event.operation_id = operation(0x30U);
    record.event.sequence = 4U;
    memcpy(record.event.body, "abc", 3U);
    record.event.body_length = 3U;
    TEST_ASSERT_TRUE(backend_ota_sha256(
        record.event.body, record.event.body_length,
        record.event.body_sha256));

    backend_ota_event_outbox_blob_t blob;
    TEST_ASSERT_TRUE(backend_ota_event_outbox_slot_encode(&record, &blob));
    TEST_ASSERT_EQUAL_UINT(73U, blob.length);
    TEST_ASSERT_EQUAL_HEX8(1U, blob.bytes[7U]);
    TEST_ASSERT_EQUAL_MEMORY(record.event.operation_id.bytes, blob.bytes + 8U, 16U);
    TEST_ASSERT_EQUAL_MEMORY("abc", blob.bytes + 66U, 3U);
    TEST_ASSERT_EQUAL_HEX32(
        backend_identity_crc32(blob.bytes, blob.length - 4U),
        (uint32_t)blob.bytes[blob.length - 4U] |
        ((uint32_t)blob.bytes[blob.length - 3U] << 8U) |
        ((uint32_t)blob.bytes[blob.length - 2U] << 16U) |
        ((uint32_t)blob.bytes[blob.length - 1U] << 24U));

    backend_ota_event_outbox_record_t decoded;
    TEST_ASSERT_TRUE(backend_ota_event_outbox_slot_decode(
        blob.bytes, blob.length, &decoded));
    TEST_ASSERT_EQUAL_MEMORY(&record, &decoded, sizeof(record));
    blob.bytes[66U] ^= 1U;
    TEST_ASSERT_FALSE(backend_ota_event_outbox_slot_decode(
        blob.bytes, blob.length, &decoded));
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(test_exact_body_crash_replay_ack_clear_and_next_event);
    BACKEND_RUN_TEST(
        test_torn_corrupt_and_ambiguous_slots_block_until_exact_recovery);
    BACKEND_RUN_TEST(test_manual_slot_layout_has_no_padding_and_binds_sha_crc);
    return UNITY_END();
}
