#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "backend_firmware_store.h"
#include "backend_identity.h"
#include "backend_scanner_relay.h"
#include "../support/backend_test_main.h"

#define IMAGE_LENGTH 1072U
#define TEST_PARTITION_BYTES 2048U

typedef struct {
    uint8_t bytes[TEST_PARTITION_BYTES];
    size_t length;
    bool fail_read;
} source_t;

typedef struct {
    uint8_t bytes[TEST_PARTITION_BYTES];
    size_t length;
    uint32_t erase_calls;
    uint32_t write_calls;
    uint32_t read_calls;
    bool fail_erase;
    bool fail_write;
    bool fail_read;
    char last_label[24];
    size_t erased_capacity;
} partition_t;

static source_t s_source;
static partition_t s_partition;

static void write_le16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static uint32_t read_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static void fixed_field(uint8_t *output, size_t capacity, const char *value)
{
    size_t length = strlen(value);
    TEST_ASSERT_LESS_THAN(capacity, length);
    memset(output, 0, capacity);
    memcpy(output, value, length);
}

static backend_ota_manifest_t build_valid_image(source_t *source)
{
    static const char version[] = "0.1.1-backend";
    memset(source, 0, sizeof(*source));
    memset(source->bytes, 0xA5, IMAGE_LENGTH);
    source->length = IMAGE_LENGTH;

    source->bytes[0] = 0xE9U;
    source->bytes[1] = 1U;
    source->bytes[2] = 2U;
    source->bytes[3] = 0x20U;
    write_le32(source->bytes + 4U, UINT32_C(0x40370000));
    source->bytes[8] = 0xEEU;
    write_le16(source->bytes + 12U, UINT16_C(9));
    source->bytes[23] = 0U;
    write_le32(source->bytes + 24U, UINT32_C(0x3C000020));
    write_le32(source->bytes + 28U, UINT32_C(1024));
    write_le32(source->bytes + 32U, UINT32_C(0xABCD5432));
    fixed_field(source->bytes + 48U, 32U, version);
    fixed_field(source->bytes + 80U, 32U, FOF_BACKEND_SCANNER_PROJECT);
    for (size_t index = 288U; index < 1056U; ++index) {
        source->bytes[index] = (uint8_t)(index * 37U + 11U);
    }

    uint8_t *record = source->bytes + 352U;
    memset(record, 0, sizeof(backend_embedded_identity_record_t));
    write_le32(record, FOF_BACKEND_IDENTITY_MAGIC);
    write_le16(record + 4U, FOF_BACKEND_IDENTITY_SCHEMA);
    write_le16(record + 6U, BACKEND_IMAGE_SCANNER);
    fixed_field(record + 8U, 40U, FOF_BACKEND_SCANNER_TARGET);
    fixed_field(record + 48U, 40U, FOF_BACKEND_SCANNER_PROJECT);
    fixed_field(record + 88U, 40U, FOF_BACKEND_HARDWARE);
    fixed_field(record + 128U, 32U, version);
    write_le32(record + 160U, backend_identity_crc32(record, 160U));

    memset(source->bytes + 1056U, 0, 16U);
    uint8_t checksum = 0xEFU;
    for (size_t index = 32U; index < 1056U; ++index) {
        checksum ^= source->bytes[index];
    }
    source->bytes[IMAGE_LENGTH - 1U] = checksum;

    backend_ota_manifest_t manifest = {0};
    strcpy(manifest.target, FOF_BACKEND_SCANNER_TARGET);
    strcpy(manifest.project, FOF_BACKEND_SCANNER_PROJECT);
    strcpy(manifest.hardware, FOF_BACKEND_HARDWARE);
    strcpy(manifest.version, version);
    manifest.image_size = IMAGE_LENGTH;
    manifest.crc32 = UINT32_C(0xA473F422);
    strcpy(manifest.sha256,
        "e786a9b17b2b9c8d3043fe237294ecdfb7e504987a12dd5eae4301b0146d9fe7");
    manifest.generation = 7U;
    return manifest;
}

static bool source_read(
    void *context, size_t offset, uint8_t *output, size_t length)
{
    source_t *source = context;
    if (source == NULL || output == NULL || length == 0U ||
        source->fail_read || offset > source->length ||
        length > source->length - offset) {
        return false;
    }
    memcpy(output, source->bytes + offset, length);
    return true;
}

static bool partition_erase(
    void *context, const char *label, size_t capacity)
{
    partition_t *partition = context;
    ++partition->erase_calls;
    strncpy(partition->last_label, label, sizeof(partition->last_label) - 1U);
    partition->erased_capacity = capacity;
    if (partition->fail_erase) {
        return false;
    }
    memset(partition->bytes, 0xFF, sizeof(partition->bytes));
    partition->length = 0U;
    return true;
}

static bool partition_write(
    void *context, const char *label, size_t offset,
    const uint8_t *bytes, size_t length)
{
    partition_t *partition = context;
    ++partition->write_calls;
    strncpy(partition->last_label, label, sizeof(partition->last_label) - 1U);
    if (partition->fail_write || bytes == NULL ||
        offset > sizeof(partition->bytes) ||
        length > sizeof(partition->bytes) - offset) {
        return false;
    }
    memcpy(partition->bytes + offset, bytes, length);
    if (partition->length < offset + length) {
        partition->length = offset + length;
    }
    return true;
}

static bool partition_read(
    void *context, const char *label, size_t offset,
    uint8_t *output, size_t length)
{
    partition_t *partition = context;
    ++partition->read_calls;
    strncpy(partition->last_label, label, sizeof(partition->last_label) - 1U);
    if (partition->fail_read || output == NULL || length == 0U ||
        offset > partition->length || length > partition->length - offset) {
        return false;
    }
    memcpy(output, partition->bytes + offset, length);
    return true;
}

static backend_firmware_store_partition_t partition_adapter(void)
{
    backend_firmware_store_partition_t adapter = {
        .context = &s_partition,
        .erase = partition_erase,
        .write = partition_write,
        .read = partition_read,
    };
    return adapter;
}

static backend_firmware_store_t staged_store(
    backend_ota_manifest_t *manifest, bool persist)
{
    *manifest = build_valid_image(&s_source);
    memset(&s_partition, 0, sizeof(s_partition));
    backend_firmware_store_partition_t adapter = partition_adapter();
    backend_firmware_store_t store;
    backend_firmware_store_init(&store, &adapter);
    TEST_ASSERT_EQUAL(BACKEND_FIRMWARE_STORE_OK,
        backend_firmware_store_stage(
            &store, manifest, source_read, &s_source, persist));
    return store;
}

static const uint8_t SCANNER0_MAC[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};

static bool relay_begin(
    backend_scanner_relay_t *relay,
    backend_firmware_store_t *store,
    const backend_ota_manifest_t *manifest,
    bool dry_run)
{
    backend_scanner_relay_init(relay);
    return backend_scanner_relay_begin(
        relay, store, BACKEND_SCANNER_SLOT_BLE, manifest, SCANNER0_MAC,
        77U, manifest->generation, 100U, 4U,
        BACKEND_SCAN_PROFILE_BLE_PRIMARY, 9U, dry_run);
}

static backend_scanner_relay_action_t take_action(
    backend_scanner_relay_t *relay, int64_t now_ms)
{
    backend_scanner_relay_action_t action;
    memset(&action, 0, sizeof(action));
    TEST_ASSERT_TRUE(backend_scanner_relay_take_action(
        relay, now_ms, &action));
    return action;
}

static backend_scanner_relay_receipt_t receipt(
    backend_scanner_relay_receipt_kind_t kind,
    const backend_scanner_relay_t *relay)
{
    backend_scanner_relay_receipt_t value = {
        .kind = kind,
        .session_id = relay->session_id,
        .generation = relay->generation,
        .sequence = relay->next_sequence,
        .next_sequence = relay->next_sequence,
        .received = (uint32_t)relay->acknowledged_bytes,
        .dry_run = relay->dry_run,
    };
    return value;
}

static void reach_streaming(backend_scanner_relay_t *relay)
{
    backend_scanner_relay_action_t quiet = take_action(relay, 0);
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_ACTION_SEND_QUIET, quiet.kind);
    backend_scanner_relay_receipt_t quiet_ack =
        receipt(BACKEND_SCANNER_RELAY_RECEIPT_QUIET_ACK, relay);
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_ACCEPTED,
        backend_scanner_relay_receive(relay, &quiet_ack, 1));
    backend_scanner_relay_action_t begin = take_action(relay, 2);
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_ACTION_SEND_BEGIN, begin.kind);
    TEST_ASSERT_EQUAL_STRING(FOF_BACKEND_SCANNER_TARGET,
        begin.control.payload.ota_begin.target);
    TEST_ASSERT_EQUAL_STRING(FOF_BACKEND_SCANNER_PROJECT,
        begin.control.payload.ota_begin.project);
    TEST_ASSERT_EQUAL_STRING(FOF_BACKEND_HARDWARE,
        begin.control.payload.ota_begin.hardware);
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:01",
        begin.control.payload.ota_begin.expected_mac);
    TEST_ASSERT_EQUAL_UINT8(BACKEND_SCANNER_SLOT_BLE,
        begin.control.payload.ota_begin.component_slot);
    TEST_ASSERT_EQUAL_UINT32(relay->session_id,
        begin.control.payload.ota_begin.session_id);
    TEST_ASSERT_EQUAL_UINT32(relay->generation,
        begin.control.payload.ota_begin.generation);
    TEST_ASSERT_EQUAL_UINT32(relay->expected_topology_generation,
        begin.control.payload.ota_begin.expected_topology_generation);
    TEST_ASSERT_EQUAL_UINT32(relay->manifest.crc32,
        begin.control.payload.ota_begin.crc32);
    TEST_ASSERT_EQUAL_STRING(relay->manifest.sha256,
        begin.control.payload.ota_begin.sha256);
    char encoded[BACKEND_SCANNER_WIRE_MAX_LINE + 1U];
    size_t encoded_length = backend_scanner_control_encode(
        &begin.control, encoded, sizeof(encoded));
    TEST_ASSERT_GREATER_THAN(0U, encoded_length);
    backend_scanner_control_t decoded;
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_CONTROL_DECODE_OK,
        backend_scanner_control_decode(encoded, encoded_length, &decoded));
    TEST_ASSERT_EQUAL_UINT32(relay->session_id,
        decoded.payload.ota_begin.session_id);
    backend_scanner_relay_receipt_t begin_ack =
        receipt(BACKEND_SCANNER_RELAY_RECEIPT_ACK, relay);
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_ACCEPTED,
        backend_scanner_relay_receive(relay, &begin_ack, 3));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_STREAMING, relay->state);
}

static void acknowledge_all_chunks(backend_scanner_relay_t *relay)
{
    while (relay->acknowledged_bytes < relay->manifest.image_size) {
        backend_scanner_relay_action_t chunk = take_action(relay, 10);
        TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_ACTION_SEND_CHUNK, chunk.kind);
        TEST_ASSERT_EQUAL_UINT8(OTA_CHUNK_MAGIC, chunk.frame[0]);
        TEST_ASSERT_EQUAL_UINT32(chunk.sequence,
            ((uint32_t)chunk.frame[1] << 8) | chunk.frame[2]);
        TEST_ASSERT_EQUAL_UINT32(chunk.image_length,
            ((uint32_t)chunk.frame[3] << 8) | chunk.frame[4]);
        TEST_ASSERT_EQUAL_HEX32(
            backend_identity_crc32(
                chunk.frame + OTA_CHUNK_HEADER_SIZE, chunk.image_length),
            read_be32(chunk.frame + OTA_CHUNK_HEADER_SIZE +
                      chunk.image_length));
        backend_scanner_relay_receipt_t ack =
            receipt(BACKEND_SCANNER_RELAY_RECEIPT_ACK, relay);
        ack.sequence = chunk.sequence;
        ack.next_sequence = chunk.next_sequence;
        ack.received = (uint32_t)(chunk.image_offset + chunk.image_length);
        TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_ACCEPTED,
            backend_scanner_relay_receive(relay, &ack, 11));
    }
}

static void reach_end_sent(backend_scanner_relay_t *relay)
{
    reach_streaming(relay);
    acknowledge_all_chunks(relay);
    backend_scanner_relay_receipt_t staged =
        receipt(BACKEND_SCANNER_RELAY_RECEIPT_STAGED, relay);
    staged.sequence = relay->next_sequence - 1U;
    staged.next_sequence = relay->next_sequence;
    staged.received = relay->manifest.image_size;
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_ACCEPTED,
        backend_scanner_relay_receive(relay, &staged, 20));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_IMAGE_STAGED, relay->state);
    backend_scanner_relay_action_t end = take_action(relay, 21);
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_ACTION_SEND_END, end.kind);
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_END_SENT, relay->state);
    char encoded[BACKEND_SCANNER_WIRE_MAX_LINE + 1U];
    size_t encoded_length = backend_scanner_control_encode(
        &end.control, encoded, sizeof(encoded));
    TEST_ASSERT_GREATER_THAN(0U, encoded_length);
    backend_scanner_control_t decoded;
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_CONTROL_DECODE_OK,
        backend_scanner_control_decode(encoded, encoded_length, &decoded));
    TEST_ASSERT_EQUAL_UINT32(relay->generation,
        decoded.payload.ota_finish.generation);
}

static backend_scanner_status_t converged_status(
    const backend_scanner_relay_t *relay)
{
    backend_scanner_status_t status = {
        .schema = BACKEND_SCANNER_STATUS_SCHEMA,
        .sequence = 1U,
        .boot_id = 101U,
        .profile = relay->expected_profile,
        .role_generation = relay->expected_role_generation,
        .role_acked = true,
        .command_ingress = true,
        .ble_healthy = true,
        .wifi_healthy = false,
    };
    strcpy(status.mac, "AA:BB:CC:DD:EE:01");
    strcpy(status.target, FOF_BACKEND_SCANNER_TARGET);
    strcpy(status.project, FOF_BACKEND_SCANNER_PROJECT);
    strcpy(status.hardware, FOF_BACKEND_HARDWARE);
    strcpy(status.version, relay->manifest.version);
    strcpy(status.ota_state, "idle");
    strcpy(status.rollback_state, "valid");
    return status;
}

void setUp(void)
{
    memset(&s_source, 0, sizeof(s_source));
    memset(&s_partition, 0, sizeof(s_partition));
}

void tearDown(void)
{
}

void test_store_accepts_only_validated_backend_scanner_and_exact_partition(void)
{
    backend_ota_manifest_t manifest;
    backend_firmware_store_t store = staged_store(&manifest, true);
    TEST_ASSERT_TRUE(store.available);
    TEST_ASSERT_TRUE(store.persisted);
    TEST_ASSERT_EQUAL_STRING(BACKEND_FIRMWARE_STORE_PARTITION_LABEL,
                             s_partition.last_label);
    TEST_ASSERT_EQUAL_UINT32(1U, s_partition.erase_calls);
    TEST_ASSERT_EQUAL_UINT32(3U, s_partition.write_calls);
    TEST_ASSERT_EQUAL_UINT32(BACKEND_FIRMWARE_STORE_CAPACITY,
                             s_partition.erased_capacity);
    TEST_ASSERT_EQUAL_UINT32(4U,
        backend_firmware_store_image_mutation_count(&store));
    uint8_t bytes[17];
    TEST_ASSERT_TRUE(backend_firmware_store_read(
        &store, manifest.generation, 500U, bytes, sizeof(bytes)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_source.bytes + 500U,
                                 bytes, sizeof(bytes));

    backend_firmware_store_t rejected;
    backend_firmware_store_partition_t adapter = partition_adapter();
    backend_firmware_store_init(&rejected, &adapter);
    strcpy(manifest.target, "scanner-s3-combo-fof_badge");
    TEST_ASSERT_EQUAL(BACKEND_FIRMWARE_STORE_REJECT_IDENTITY,
        backend_firmware_store_stage(
            &rejected, &manifest, source_read, &s_source, true));
    TEST_ASSERT_EQUAL_UINT32(1U, s_partition.erase_calls);
}

void test_dry_run_store_binds_validated_arena_without_partition_mutation(void)
{
    backend_ota_manifest_t manifest;
    backend_firmware_store_t store = staged_store(&manifest, false);
    TEST_ASSERT_TRUE(store.available);
    TEST_ASSERT_FALSE(store.persisted);
    TEST_ASSERT_EQUAL_UINT32(0U,
        backend_firmware_store_image_mutation_count(&store));
    TEST_ASSERT_EQUAL_UINT32(0U, s_partition.erase_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, s_partition.write_calls);
    TEST_ASSERT_TRUE(backend_firmware_store_claim_relay(
        &store, manifest.generation, 77U));
    TEST_ASSERT_FALSE(backend_firmware_store_claim_relay(
        &store, manifest.generation, 78U));
    TEST_ASSERT_FALSE(backend_firmware_store_discard(
        &store, manifest.generation));
    backend_firmware_store_release_relay(
        &store, manifest.generation, 77U);
    TEST_ASSERT_TRUE(backend_firmware_store_discard(
        &store, manifest.generation));
}

void test_store_failures_never_expose_partial_image(void)
{
    backend_ota_manifest_t manifest = build_valid_image(&s_source);
    backend_firmware_store_partition_t adapter = partition_adapter();
    backend_firmware_store_t store;
    backend_firmware_store_init(&store, &adapter);
    s_partition.fail_erase = true;
    TEST_ASSERT_EQUAL(BACKEND_FIRMWARE_STORE_ERASE_FAILED,
        backend_firmware_store_stage(
            &store, &manifest, source_read, &s_source, true));
    TEST_ASSERT_FALSE(store.available);
    TEST_ASSERT_EQUAL_UINT32(1U,
        backend_firmware_store_image_mutation_count(&store));

    backend_firmware_store_init(&store, &adapter);
    memset(&s_partition, 0, sizeof(s_partition));
    s_partition.fail_write = true;
    TEST_ASSERT_EQUAL(BACKEND_FIRMWARE_STORE_WRITE_FAILED,
        backend_firmware_store_stage(
            &store, &manifest, source_read, &s_source, true));
    TEST_ASSERT_FALSE(store.available);
    TEST_ASSERT_EQUAL_UINT32(2U,
        backend_firmware_store_image_mutation_count(&store));
}

void test_relay_binds_one_physical_scanner_and_immutable_manifest(void)
{
    backend_ota_manifest_t manifest;
    backend_firmware_store_t store = staged_store(&manifest, false);
    TEST_ASSERT_TRUE(backend_scanner_relay_can_begin(
        BACKEND_SCANNER_SLOT_BLE, &manifest, SCANNER0_MAC,
        manifest.generation));
    backend_scanner_relay_t first;
    TEST_ASSERT_TRUE(relay_begin(&first, &store, &manifest, true));
    backend_scanner_relay_t second;
    TEST_ASSERT_FALSE(relay_begin(&second, &store, &manifest, true));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_QUIET_REQUESTED, first.state);

    backend_ota_manifest_t changed = manifest;
    changed.crc32 ^= 1U;
    backend_scanner_relay_t wrong;
    backend_scanner_relay_init(&wrong);
    TEST_ASSERT_FALSE(backend_scanner_relay_begin(
        &wrong, &store, BACKEND_SCANNER_SLOT_BLE, &changed, SCANNER0_MAC,
        78U, changed.generation, 100U, 4U,
        BACKEND_SCAN_PROFILE_BLE_PRIMARY, 9U, true));
    TEST_ASSERT_FALSE(backend_scanner_relay_can_begin(
        (backend_scanner_slot_t)9, &manifest, SCANNER0_MAC,
        manifest.generation));
}

void test_chunk_ack_nack_and_timeout_never_advance_or_change_retry_bytes(void)
{
    backend_ota_manifest_t manifest;
    backend_firmware_store_t store = staged_store(&manifest, false);
    backend_scanner_relay_t relay;
    TEST_ASSERT_TRUE(relay_begin(&relay, &store, &manifest, true));
    reach_streaming(&relay);
    backend_scanner_relay_action_t chunk = take_action(&relay, 100);
    backend_scanner_relay_action_t original = chunk;

    backend_scanner_relay_receipt_t stale =
        receipt(BACKEND_SCANNER_RELAY_RECEIPT_ACK, &relay);
    stale.session_id++;
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_IGNORED_STALE,
        backend_scanner_relay_receive(&relay, &stale, 101));
    TEST_ASSERT_EQUAL_UINT32(0U, relay.next_sequence);
    stale = receipt(BACKEND_SCANNER_RELAY_RECEIPT_ACK, &relay);
    stale.sequence = 9U;
    stale.next_sequence = 10U;
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_IGNORED_STALE,
        backend_scanner_relay_receive(&relay, &stale, 101));
    TEST_ASSERT_EQUAL_UINT32(0U, relay.next_sequence);

    backend_scanner_relay_receipt_t nack =
        receipt(BACKEND_SCANNER_RELAY_RECEIPT_NACK, &relay);
    nack.sequence = chunk.sequence;
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_RETRY_SCHEDULED,
        backend_scanner_relay_receive(&relay, &nack, 102));
    backend_scanner_relay_action_t retry = take_action(&relay, 103);
    TEST_ASSERT_EQUAL_MEMORY(&original, &retry, sizeof(original));

    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_WAITING,
        backend_scanner_relay_tick(&relay, 5102));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_RETRY_SCHEDULED,
        backend_scanner_relay_tick(&relay, 5103));
    retry = take_action(&relay, 5103);
    TEST_ASSERT_EQUAL_MEMORY(&original, &retry, sizeof(original));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_RETRY_SCHEDULED,
        backend_scanner_relay_tick(&relay, 10103));
    retry = take_action(&relay, 10103);
    TEST_ASSERT_EQUAL_MEMORY(&original, &retry, sizeof(original));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_FAILED,
        backend_scanner_relay_tick(&relay, 15103));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_FAILED, relay.state);
    TEST_ASSERT_FALSE(store.relay_claimed);
}

void test_duplicate_ack_and_wrong_staged_sequence_are_stale_not_fatal(void)
{
    backend_ota_manifest_t manifest;
    backend_firmware_store_t store = staged_store(&manifest, false);
    backend_scanner_relay_t relay;
    TEST_ASSERT_TRUE(relay_begin(&relay, &store, &manifest, true));
    reach_streaming(&relay);

    backend_scanner_relay_action_t first = take_action(&relay, 100);
    backend_scanner_relay_receipt_t ack =
        receipt(BACKEND_SCANNER_RELAY_RECEIPT_ACK, &relay);
    ack.sequence = first.sequence;
    ack.next_sequence = first.next_sequence;
    ack.received = (uint32_t)(first.image_offset + first.image_length);
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_ACCEPTED,
        backend_scanner_relay_receive(&relay, &ack, 101));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_IGNORED_STALE,
        backend_scanner_relay_receive(&relay, &ack, 102));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_STREAMING, relay.state);

    acknowledge_all_chunks(&relay);
    backend_scanner_relay_receipt_t staged =
        receipt(BACKEND_SCANNER_RELAY_RECEIPT_STAGED, &relay);
    staged.sequence = relay.next_sequence;
    staged.next_sequence = relay.next_sequence;
    staged.received = manifest.image_size;
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_IGNORED_STALE,
        backend_scanner_relay_receive(&relay, &staged, 200));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_STREAMING, relay.state);
    staged.sequence = relay.next_sequence - 1U;
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_ACCEPTED,
        backend_scanner_relay_receive(&relay, &staged, 201));
}

void test_late_matching_ack_cancels_an_unsent_timeout_retry(void)
{
    backend_ota_manifest_t manifest;
    backend_firmware_store_t store = staged_store(&manifest, false);
    backend_scanner_relay_t relay;
    TEST_ASSERT_TRUE(relay_begin(&relay, &store, &manifest, true));
    reach_streaming(&relay);

    backend_scanner_relay_action_t first = take_action(&relay, 100);
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_RETRY_SCHEDULED,
        backend_scanner_relay_tick(&relay, 5100));
    TEST_ASSERT_TRUE(relay.action_pending);
    backend_scanner_relay_receipt_t ack =
        receipt(BACKEND_SCANNER_RELAY_RECEIPT_ACK, &relay);
    ack.sequence = first.sequence;
    ack.next_sequence = first.next_sequence;
    ack.received = (uint32_t)(first.image_offset + first.image_length);
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_ACCEPTED,
        backend_scanner_relay_receive(&relay, &ack, 5101));
    TEST_ASSERT_EQUAL_UINT32(first.next_sequence, relay.next_sequence);
    TEST_ASSERT_EQUAL_UINT32(first.image_length, relay.acknowledged_bytes);
    backend_scanner_relay_action_t second = take_action(&relay, 5102);
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_ACTION_SEND_CHUNK, second.kind);
    TEST_ASSERT_EQUAL_UINT32(first.next_sequence, second.sequence);
}

void test_dry_run_reaches_complete_without_reboot_or_partition_mutation(void)
{
    backend_ota_manifest_t manifest;
    backend_firmware_store_t store = staged_store(&manifest, false);
    backend_scanner_relay_t relay;
    TEST_ASSERT_TRUE(relay_begin(&relay, &store, &manifest, true));
    reach_end_sent(&relay);
    backend_scanner_relay_receipt_t done =
        receipt(BACKEND_SCANNER_RELAY_RECEIPT_DONE, &relay);
    done.sequence = relay.next_sequence - 1U;
    done.received = manifest.image_size;
    done.next_sequence = relay.next_sequence;
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_COMPLETE,
        backend_scanner_relay_receive(&relay, &done, 22));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_COMPLETE, relay.state);
    TEST_ASSERT_EQUAL_UINT32(0U,
        backend_firmware_store_image_mutation_count(&store));
    TEST_ASSERT_FALSE(store.relay_claimed);
}

void test_apply_requires_changed_boot_exact_identity_and_full_convergence(void)
{
    backend_ota_manifest_t manifest;
    backend_firmware_store_t store = staged_store(&manifest, true);
    backend_scanner_relay_t relay;
    TEST_ASSERT_TRUE(relay_begin(&relay, &store, &manifest, false));
    reach_end_sent(&relay);
    backend_scanner_relay_receipt_t done =
        receipt(BACKEND_SCANNER_RELAY_RECEIPT_DONE, &relay);
    done.sequence = relay.next_sequence - 1U;
    done.received = manifest.image_size;
    done.next_sequence = relay.next_sequence;
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_ACCEPTED,
        backend_scanner_relay_receive(&relay, &done, 22));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_REBOOT_WAIT, relay.state);

    backend_scanner_status_t status = converged_status(&relay);
    status.boot_id = relay.old_boot_id;
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_WAITING,
        backend_scanner_relay_on_status(&relay, &status, 4U, 23));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_REBOOT_WAIT, relay.state);

    status = converged_status(&relay);
    status.role_acked = false;
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_WAITING,
        backend_scanner_relay_on_status(&relay, &status, 4U, 24));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_CONVERGENCE_WAIT, relay.state);
    status.role_acked = true;
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_COMPLETE,
        backend_scanner_relay_on_status(&relay, &status, 4U, 25));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_COMPLETE, relay.state);
}

void test_wrong_binding_and_illegal_transitions_fail_closed(void)
{
    backend_ota_manifest_t manifest;
    backend_firmware_store_t store = staged_store(&manifest, true);
    backend_scanner_relay_t relay;
    TEST_ASSERT_TRUE(relay_begin(&relay, &store, &manifest, false));
    backend_scanner_relay_receipt_t staged =
        receipt(BACKEND_SCANNER_RELAY_RECEIPT_STAGED, &relay);
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_INVALID_TRANSITION,
        backend_scanner_relay_receive(&relay, &staged, 0));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_FAILED, relay.state);

    TEST_ASSERT_TRUE(backend_scanner_relay_reset(&relay));
    TEST_ASSERT_TRUE(relay_begin(&relay, &store, &manifest, false));
    reach_end_sent(&relay);
    backend_scanner_relay_receipt_t done =
        receipt(BACKEND_SCANNER_RELAY_RECEIPT_DONE, &relay);
    done.sequence = relay.next_sequence - 1U;
    done.received = manifest.image_size;
    done.next_sequence = relay.next_sequence;
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_ACCEPTED,
        backend_scanner_relay_receive(&relay, &done, 22));
    backend_scanner_status_t status = converged_status(&relay);
    strcpy(status.mac, "AA:BB:CC:DD:EE:02");
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_FAILED,
        backend_scanner_relay_on_status(&relay, &status, 4U, 23));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_FAILED, relay.state);

    TEST_ASSERT_TRUE(backend_scanner_relay_reset(&relay));
    TEST_ASSERT_TRUE(relay_begin(&relay, &store, &manifest, false));
    reach_end_sent(&relay);
    done = receipt(BACKEND_SCANNER_RELAY_RECEIPT_DONE, &relay);
    done.sequence = relay.next_sequence - 1U;
    done.received = manifest.image_size;
    done.next_sequence = relay.next_sequence;
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_ACCEPTED,
        backend_scanner_relay_receive(&relay, &done, 22));
    status = converged_status(&relay);
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_EVENT_FAILED,
        backend_scanner_relay_on_status(&relay, &status, 5U, 23));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_RELAY_FAILED, relay.state);
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_store_accepts_only_validated_backend_scanner_and_exact_partition);
    BACKEND_RUN_TEST(
        test_dry_run_store_binds_validated_arena_without_partition_mutation);
    BACKEND_RUN_TEST(test_store_failures_never_expose_partial_image);
    BACKEND_RUN_TEST(
        test_relay_binds_one_physical_scanner_and_immutable_manifest);
    BACKEND_RUN_TEST(
        test_chunk_ack_nack_and_timeout_never_advance_or_change_retry_bytes);
    BACKEND_RUN_TEST(
        test_duplicate_ack_and_wrong_staged_sequence_are_stale_not_fatal);
    BACKEND_RUN_TEST(
        test_late_matching_ack_cancels_an_unsent_timeout_retry);
    BACKEND_RUN_TEST(
        test_dry_run_reaches_complete_without_reboot_or_partition_mutation);
    BACKEND_RUN_TEST(
        test_apply_requires_changed_boot_exact_identity_and_full_convergence);
    BACKEND_RUN_TEST(
        test_wrong_binding_and_illegal_transitions_fail_closed);
    return UNITY_END();
}
