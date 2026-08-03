#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "backend_identity.h"
#include "uart_ota.h"
#include "../support/backend_test_main.h"

#define FIXTURE_CAPACITY 2048U
#define FIXTURE_SEGMENT_DATA_SIZE 1024U
#define FIXTURE_IDENTITY_OFFSET 352U
#define FIXTURE_IMAGE_SIZE 1072U
#define MAX_RECEIPTS 32U

typedef struct {
    uint8_t bytes[FIXTURE_CAPACITY];
    size_t length;
    backend_ota_manifest_t manifest;
} image_fixture_t;

typedef struct {
    uint8_t staging[FIXTURE_CAPACITY];
    uint8_t flashed[FIXTURE_CAPACITY];
    uart_ota_local_binding_t binding;
    uart_ota_receipt_t receipts[MAX_RECEIPTS];
    size_t receipt_count;
    size_t flashed_size;
    unsigned binding_reads;
    unsigned allocations;
    unsigned releases;
    unsigned flash_begins;
    unsigned flash_writes;
    unsigned flash_finishes;
    unsigned flash_aborts;
    unsigned pending_activations;
    unsigned reboots;
    bool binding_read_ok;
    bool allocation_ok;
    bool receipt_ok;
    bool flash_begin_ok;
    bool flash_write_ok;
    bool flash_finish_ok;
    bool activate_ok;
    bool reboot_ok;
} fake_t;

void setUp(void) {}
void tearDown(void) {}

static void write_u16_be(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void write_u32_be(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static void write_u16_le(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void write_u32_le(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static uint32_t read_u32_le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void copy_fixed(uint8_t *field, size_t width, const char *value)
{
    const size_t length = strlen(value);
    TEST_ASSERT_LESS_THAN(width, length);
    memset(field, 0, width);
    memcpy(field, value, length);
}

static void write_structured_identity(image_fixture_t *fixture)
{
    uint8_t *record = fixture->bytes + FIXTURE_IDENTITY_OFFSET;
    memset(record, 0, sizeof(backend_embedded_identity_record_t));
    write_u32_le(record, FOF_BACKEND_IDENTITY_MAGIC);
    write_u16_le(record + 4U, FOF_BACKEND_IDENTITY_SCHEMA);
    write_u16_le(record + 6U, BACKEND_IMAGE_SCANNER);
    copy_fixed(record + 8U, 40U, FOF_BACKEND_SCANNER_TARGET);
    copy_fixed(record + 48U, 40U, FOF_BACKEND_SCANNER_PROJECT);
    copy_fixed(record + 88U, 40U, FOF_BACKEND_HARDWARE);
    copy_fixed(record + 128U, 32U, "0.1.1-backend");
    write_u32_le(record + 160U, backend_identity_crc32(record, 160U));
}

static void refresh_esp_checksum(image_fixture_t *fixture)
{
    uint8_t checksum = 0xEFU;
    size_t offset = 24U;
    for (uint8_t segment = 0U; segment < fixture->bytes[1]; ++segment) {
        const uint32_t length = read_u32_le(fixture->bytes + offset + 4U);
        offset += 8U;
        for (size_t index = 0U; index < length; ++index) {
            checksum ^= fixture->bytes[offset + index];
        }
        offset += length;
    }
    const size_t checksum_end = (offset + 16U) & ~(size_t)15U;
    fixture->bytes[checksum_end - 1U] = checksum;
}

static image_fixture_t valid_image_fixture(void)
{
    image_fixture_t fixture;
    memset(&fixture, 0, sizeof(fixture));
    memset(fixture.bytes, 0xA5, sizeof(fixture.bytes));

    fixture.bytes[0] = 0xE9U;
    fixture.bytes[1] = 1U;
    fixture.bytes[2] = 2U;
    fixture.bytes[3] = 0x20U;
    write_u32_le(fixture.bytes + 4U, UINT32_C(0x40370000));
    fixture.bytes[8] = 0xEEU;
    write_u16_le(fixture.bytes + 12U, UINT16_C(0x0009));
    fixture.bytes[23] = 0U;
    write_u32_le(fixture.bytes + 24U, UINT32_C(0x3C000020));
    write_u32_le(fixture.bytes + 28U, FIXTURE_SEGMENT_DATA_SIZE);
    write_u32_le(fixture.bytes + 32U, UINT32_C(0xABCD5432));
    copy_fixed(fixture.bytes + 48U, 32U, "0.1.1-backend");
    copy_fixed(fixture.bytes + 80U, 32U, FOF_BACKEND_SCANNER_PROJECT);
    for (size_t index = 288U;
         index < 32U + FIXTURE_SEGMENT_DATA_SIZE; ++index) {
        fixture.bytes[index] = (uint8_t)(index * 37U + 11U);
    }
    write_structured_identity(&fixture);
    fixture.length = FIXTURE_IMAGE_SIZE;
    memset(fixture.bytes + 32U + FIXTURE_SEGMENT_DATA_SIZE, 0,
           fixture.length - (32U + FIXTURE_SEGMENT_DATA_SIZE));
    refresh_esp_checksum(&fixture);

    strcpy(fixture.manifest.target, FOF_BACKEND_SCANNER_TARGET);
    strcpy(fixture.manifest.project, FOF_BACKEND_SCANNER_PROJECT);
    strcpy(fixture.manifest.hardware, FOF_BACKEND_HARDWARE);
    strcpy(fixture.manifest.version, "0.1.1-backend");
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    strcpy(fixture.manifest.sha256,
           "e786a9b17b2b9c8d3043fe237294ecdf"
           "b7e504987a12dd5eae4301b0146d9fe7");
#else
    strcpy(fixture.manifest.sha256,
           "39e7f61363e9b33abdfa8c87b1708e53"
           "d5bcd1c044705ba00964400780820f32");
#endif
    fixture.manifest.image_size = (uint32_t)fixture.length;
    fixture.manifest.crc32 = backend_identity_crc32(
        fixture.bytes, fixture.length);
    fixture.manifest.generation = 12U;
    return fixture;
}

static fake_t fresh_fake(void)
{
    fake_t fake;
    memset(&fake, 0, sizeof(fake));
    fake.binding.component_slot = 0U;
    strcpy(fake.binding.mac, "AA:BB:CC:DD:EE:01");
    fake.binding.boot_id = UINT32_C(305419896);
    fake.binding.topology_generation = 4U;
    fake.binding_read_ok = true;
    fake.allocation_ok = true;
    fake.receipt_ok = true;
    fake.flash_begin_ok = true;
    fake.flash_write_ok = true;
    fake.flash_finish_ok = true;
    fake.activate_ok = true;
    fake.reboot_ok = true;
    return fake;
}

static bool read_binding(void *context, uart_ota_local_binding_t *out)
{
    fake_t *fake = context;
    ++fake->binding_reads;
    if (!fake->binding_read_ok) {
        return false;
    }
    *out = fake->binding;
    return true;
}

static uint8_t *psram_acquire(void *context, size_t size)
{
    fake_t *fake = context;
    ++fake->allocations;
    if (!fake->allocation_ok || size > sizeof(fake->staging)) {
        return NULL;
    }
    memset(fake->staging, 0, sizeof(fake->staging));
    return fake->staging;
}

static void psram_release(void *context, uint8_t *buffer, size_t size)
{
    fake_t *fake = context;
    TEST_ASSERT_EQUAL_PTR(fake->staging, buffer);
    TEST_ASSERT_LESS_OR_EQUAL(sizeof(fake->staging), size);
    ++fake->releases;
}

static bool emit_receipt(void *context, const uart_ota_receipt_t *receipt)
{
    fake_t *fake = context;
    if (!fake->receipt_ok || fake->receipt_count >= MAX_RECEIPTS) {
        return false;
    }
    fake->receipts[fake->receipt_count++] = *receipt;
    return true;
}

static bool inactive_begin(void *context, size_t size)
{
    fake_t *fake = context;
    ++fake->flash_begins;
    fake->flashed_size = 0U;
    return fake->flash_begin_ok && size <= sizeof(fake->flashed);
}

static bool inactive_write(
    void *context, size_t offset, const uint8_t *bytes, size_t size)
{
    fake_t *fake = context;
    ++fake->flash_writes;
    if (!fake->flash_write_ok || bytes == NULL ||
        offset > sizeof(fake->flashed) ||
        size > sizeof(fake->flashed) - offset) {
        return false;
    }
    memcpy(fake->flashed + offset, bytes, size);
    if (fake->flashed_size < offset + size) {
        fake->flashed_size = offset + size;
    }
    return true;
}

static bool inactive_finish(void *context)
{
    fake_t *fake = context;
    ++fake->flash_finishes;
    return fake->flash_finish_ok;
}

static void inactive_abort(void *context)
{
    fake_t *fake = context;
    ++fake->flash_aborts;
}

static bool activate_pending(void *context)
{
    fake_t *fake = context;
    ++fake->pending_activations;
    return fake->activate_ok;
}

static bool request_reboot(void *context)
{
    fake_t *fake = context;
    ++fake->reboots;
    return fake->reboot_ok;
}

static uart_ota_config_t config_for(fake_t *fake)
{
    const uart_ota_config_t config = {
        .running_version = "0.1.0-backend",
        .inactive_slot_capacity = UART_OTA_INACTIVE_SLOT_CAPACITY,
        .ops = {
            .context = fake,
            .read_binding = read_binding,
            .psram_acquire = psram_acquire,
            .psram_release = psram_release,
            .emit_receipt = emit_receipt,
            .inactive_slot_begin = inactive_begin,
            .inactive_slot_write = inactive_write,
            .inactive_slot_finish = inactive_finish,
            .inactive_slot_abort = inactive_abort,
            .inactive_slot_activate_pending_verify = activate_pending,
            .request_reboot = request_reboot,
        },
    };
    return config;
}

static backend_scanner_ota_begin_control_t begin_for(
    const image_fixture_t *fixture,
    bool dry_run)
{
    backend_scanner_ota_begin_control_t begin;
    memset(&begin, 0, sizeof(begin));
    begin.session_id = 7U;
    begin.generation = fixture->manifest.generation;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    begin.manifest_generation = fixture->manifest.generation;
#endif
    begin.component_slot = 0U;
    strcpy(begin.expected_mac, "AA:BB:CC:DD:EE:01");
    begin.expected_boot_id = UINT32_C(305419896);
    begin.expected_topology_generation = 4U;
    strcpy(begin.target, fixture->manifest.target);
    strcpy(begin.project, fixture->manifest.project);
    strcpy(begin.hardware, fixture->manifest.hardware);
    strcpy(begin.version, fixture->manifest.version);
    begin.image_size = fixture->manifest.image_size;
    begin.crc32 = fixture->manifest.crc32;
    strcpy(begin.sha256, fixture->manifest.sha256);
    begin.allow_same_version = fixture->manifest.allow_same_version;
    begin.dry_run = dry_run;
    return begin;
}

static void start_ota(
    uart_ota_t *ota,
    fake_t *fake,
    const image_fixture_t *fixture,
    bool dry_run)
{
    const uart_ota_config_t config = config_for(fake);
    backend_scanner_ota_begin_control_t begin = begin_for(fixture, dry_run);
    TEST_ASSERT_TRUE(uart_ota_init(ota, &config));
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_OK, uart_ota_begin(ota, &begin));
}

static size_t make_frame(
    uint16_t sequence,
    const uint8_t *bytes,
    uint16_t size,
    bool corrupt_crc,
    uint8_t output[OTA_CHUNK_HEADER_SIZE + OTA_CHUNK_MAX_DATA +
                   OTA_CHUNK_CRC_SIZE])
{
    output[0] = OTA_CHUNK_MAGIC;
    write_u16_be(output + 1U, sequence);
    write_u16_be(output + 3U, size);
    memcpy(output + OTA_CHUNK_HEADER_SIZE, bytes, size);
    uint32_t crc = uart_ota_chunk_crc32(bytes, size);
    if (corrupt_crc) {
        crc ^= UINT32_C(1);
    }
    write_u32_be(output + OTA_CHUNK_HEADER_SIZE + size, crc);
    return OTA_CHUNK_HEADER_SIZE + size + OTA_CHUNK_CRC_SIZE;
}

static void stage_image(
    uart_ota_t *ota,
    const image_fixture_t *fixture)
{
    uint8_t frame[OTA_CHUNK_HEADER_SIZE + OTA_CHUNK_MAX_DATA +
                  OTA_CHUNK_CRC_SIZE];
    size_t offset = 0U;
    uint16_t sequence = 0U;
    while (offset < fixture->length) {
        size_t amount = fixture->length - offset;
        if (amount > OTA_CHUNK_MAX_DATA) {
            amount = OTA_CHUNK_MAX_DATA;
        }
        const size_t frame_size = make_frame(
            sequence, fixture->bytes + offset, (uint16_t)amount,
            false, frame);
        size_t consumed = 0U;
        TEST_ASSERT_EQUAL(UART_OTA_RESULT_OK,
            uart_ota_consume(ota, frame, frame_size, &consumed));
        TEST_ASSERT_EQUAL_UINT(frame_size, consumed);
        offset += amount;
        ++sequence;
    }
    TEST_ASSERT_EQUAL(UART_OTA_STATE_IMAGE_STAGED, ota->state);
}

static backend_scanner_ota_finish_control_t finish_control(
    uint32_t session_id,
    uint32_t generation)
{
    backend_scanner_ota_finish_control_t finish;
    memset(&finish, 0, sizeof(finish));
    finish.session_id = session_id;
    finish.generation = generation;
    strcpy(finish.reason, "verified");
    return finish;
}

void test_begin_admits_exact_backend_scanner_binding_and_emits_exact_ack(void)
{
    image_fixture_t fixture = valid_image_fixture();
    fake_t fake = fresh_fake();
    uart_ota_t ota;
    start_ota(&ota, &fake, &fixture, true);

    TEST_ASSERT_EQUAL(UART_OTA_STATE_STAGING, ota.state);
    TEST_ASSERT_EQUAL_UINT(1U, fake.allocations);
    TEST_ASSERT_EQUAL_UINT(1U, fake.receipt_count);
    TEST_ASSERT_EQUAL(UART_OTA_RECEIPT_ACK, fake.receipts[0].type);
    TEST_ASSERT_EQUAL_UINT32(7U, fake.receipts[0].session_id);
    TEST_ASSERT_EQUAL_UINT32(12U, fake.receipts[0].generation);
    TEST_ASSERT_EQUAL_UINT16(0U, fake.receipts[0].sequence);
    TEST_ASSERT_EQUAL_UINT16(0U, fake.receipts[0].next_sequence);
    TEST_ASSERT_EQUAL_UINT32(0U, fake.receipts[0].received);
    TEST_ASSERT_TRUE(fake.receipts[0].dry_run);

    char json[256];
    TEST_ASSERT_GREATER_THAN_UINT(0U, uart_ota_receipt_to_json(
        &fake.receipts[0], json, sizeof(json)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"ota_ack\",\"session_id\":7,\"generation\":12,"
        "\"sequence\":0,\"next_sequence\":0,\"received\":0,"
        "\"dry_run\":true,\"reason\":null}", json);
}

void test_begin_rejects_cross_family_binding_capacity_stale_and_no_psram(void)
{
    image_fixture_t fixture = valid_image_fixture();
    fake_t fake = fresh_fake();
    uart_ota_config_t config = config_for(&fake);
    uart_ota_t ota;
    backend_scanner_ota_begin_control_t begin = begin_for(&fixture, true);

    config.inactive_slot_capacity--;
    TEST_ASSERT_FALSE(uart_ota_init(&ota, &config));
    config = config_for(&fake);
    TEST_ASSERT_TRUE(uart_ota_init(&ota, &config));
    strcpy(begin.target, FOF_BACKEND_UPLINK_TARGET);
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_MANIFEST_REJECTED,
        uart_ota_begin(&ota, &begin));
    TEST_ASSERT_EQUAL_UINT(0U, fake.allocations);

    begin = begin_for(&fixture, true);
    fake.binding.boot_id++;
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_BINDING_MISMATCH,
        uart_ota_begin(&ota, &begin));
    TEST_ASSERT_EQUAL_UINT(0U, fake.allocations);
    fake.binding.boot_id--;
    fake.allocation_ok = false;
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_NO_PSRAM,
        uart_ota_begin(&ota, &begin));
    TEST_ASSERT_EQUAL_UINT(0U, fake.flash_begins);

    fake.allocation_ok = true;
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_OK, uart_ota_begin(&ota, &begin));
    uart_ota_reset(&ota);
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_STALE_GENERATION,
        uart_ota_begin(&ota, &begin));
}

void test_profile_slot_capacity_rejects_the_first_oversize_scanner_image(void)
{
    image_fixture_t fixture = valid_image_fixture();
    fake_t fake = fresh_fake();
    uart_ota_config_t config = config_for(&fake);
    uart_ota_t ota;
    TEST_ASSERT_TRUE(uart_ota_init(&ota, &config));

    backend_scanner_ota_begin_control_t begin = begin_for(&fixture, true);
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    begin.image_size = UINT32_C(0x200001);
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_MANIFEST_REJECTED,
        uart_ota_begin(&ota, &begin));
    TEST_ASSERT_EQUAL_UINT(0U, fake.allocations);
#else
    begin.image_size = UINT32_C(0x300000);
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_NO_PSRAM,
        uart_ota_begin(&ota, &begin));
    TEST_ASSERT_EQUAL_UINT(1U, fake.allocations);

    begin.image_size = UINT32_C(0x300001);
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_MANIFEST_REJECTED,
        uart_ota_begin(&ota, &begin));
    TEST_ASSERT_EQUAL_UINT(1U, fake.allocations);
#endif
}

void test_exact_begin_retry_reemits_receipt_without_reallocation_or_reset(void)
{
    image_fixture_t fixture = valid_image_fixture();
    fake_t fake = fresh_fake();
    uart_ota_t ota;
    start_ota(&ota, &fake, &fixture, true);
    backend_scanner_ota_begin_control_t begin = begin_for(&fixture, true);

    TEST_ASSERT_EQUAL(UART_OTA_RESULT_OK, uart_ota_begin(&ota, &begin));
    TEST_ASSERT_EQUAL(UART_OTA_STATE_STAGING, ota.state);
    TEST_ASSERT_EQUAL_UINT(1U, fake.allocations);
    TEST_ASSERT_EQUAL_UINT(2U, fake.receipt_count);
    TEST_ASSERT_EQUAL(UART_OTA_RECEIPT_ACK, fake.receipts[1].type);
    TEST_ASSERT_EQUAL_UINT16(0U, fake.receipts[1].next_sequence);

    begin.sha256[0] = begin.sha256[0] == '0' ? '1' : '0';
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_INVALID_STATE,
        uart_ota_begin(&ota, &begin));
    TEST_ASSERT_EQUAL_UINT(1U, fake.allocations);
}

void test_bad_crc_and_out_of_order_frames_nack_without_advancing(void)
{
    image_fixture_t fixture = valid_image_fixture();
    fake_t fake = fresh_fake();
    uart_ota_t ota;
    start_ota(&ota, &fake, &fixture, true);
    uint8_t frame[OTA_CHUNK_HEADER_SIZE + OTA_CHUNK_MAX_DATA +
                  OTA_CHUNK_CRC_SIZE];
    size_t consumed = 0U;

    size_t frame_size = make_frame(
        0U, fixture.bytes, OTA_CHUNK_MAX_DATA, true, frame);
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_OK,
        uart_ota_consume(&ota, frame, frame_size, &consumed));
    TEST_ASSERT_EQUAL_UINT(frame_size, consumed);
    TEST_ASSERT_EQUAL_UINT32(0U, uart_ota_received(&ota));
    TEST_ASSERT_EQUAL_UINT16(0U, uart_ota_expected_sequence(&ota));
    TEST_ASSERT_EQUAL(UART_OTA_RECEIPT_NACK, fake.receipts[1].type);
    TEST_ASSERT_EQUAL_STRING("chunk_crc", fake.receipts[1].reason);

    frame_size = make_frame(
        1U, fixture.bytes, OTA_CHUNK_MAX_DATA, false, frame);
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_OK,
        uart_ota_consume(&ota, frame, frame_size, &consumed));
    TEST_ASSERT_EQUAL_UINT32(0U, uart_ota_received(&ota));
    TEST_ASSERT_EQUAL(UART_OTA_RECEIPT_NACK, fake.receipts[2].type);
    TEST_ASSERT_EQUAL_UINT16(0U, fake.receipts[2].next_sequence);
    TEST_ASSERT_EQUAL_STRING("sequence", fake.receipts[2].reason);

    frame_size = make_frame(
        0U, fixture.bytes, OTA_CHUNK_MAX_DATA, false, frame);
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_OK,
        uart_ota_consume(&ota, frame, frame_size, &consumed));
    TEST_ASSERT_EQUAL_UINT32(OTA_CHUNK_MAX_DATA, uart_ota_received(&ota));
    TEST_ASSERT_EQUAL_UINT16(1U, uart_ota_expected_sequence(&ota));
    TEST_ASSERT_EQUAL(UART_OTA_RECEIPT_ACK, fake.receipts[3].type);
    TEST_ASSERT_EQUAL_UINT16(0U, fake.receipts[3].sequence);
    TEST_ASSERT_EQUAL_UINT16(1U, fake.receipts[3].next_sequence);
}

void test_exact_chunk_retry_reacks_without_copy_and_changed_retry_nacks(void)
{
    image_fixture_t fixture = valid_image_fixture();
    fake_t fake = fresh_fake();
    uart_ota_t ota;
    start_ota(&ota, &fake, &fixture, true);
    uint8_t frame[OTA_CHUNK_HEADER_SIZE + OTA_CHUNK_MAX_DATA +
                  OTA_CHUNK_CRC_SIZE];
    size_t frame_size = make_frame(
        0U, fixture.bytes, OTA_CHUNK_MAX_DATA, false, frame);
    size_t consumed = 0U;
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_OK,
        uart_ota_consume(&ota, frame, frame_size, &consumed));
    TEST_ASSERT_EQUAL_UINT32(OTA_CHUNK_MAX_DATA, uart_ota_received(&ota));
    const size_t receipt_count = fake.receipt_count;

    TEST_ASSERT_EQUAL(UART_OTA_RESULT_OK,
        uart_ota_consume(&ota, frame, frame_size, &consumed));
    TEST_ASSERT_EQUAL_UINT32(OTA_CHUNK_MAX_DATA, uart_ota_received(&ota));
    TEST_ASSERT_EQUAL_UINT16(1U, uart_ota_expected_sequence(&ota));
    TEST_ASSERT_EQUAL_UINT(receipt_count + 1U, fake.receipt_count);
    TEST_ASSERT_EQUAL(UART_OTA_RECEIPT_ACK,
                      fake.receipts[fake.receipt_count - 1U].type);
    TEST_ASSERT_EQUAL_UINT16(0U,
        fake.receipts[fake.receipt_count - 1U].sequence);
    TEST_ASSERT_EQUAL_UINT16(1U,
        fake.receipts[fake.receipt_count - 1U].next_sequence);

    uint8_t changed[OTA_CHUNK_MAX_DATA];
    memcpy(changed, fixture.bytes, sizeof(changed));
    changed[17U] ^= 1U;
    frame_size = make_frame(
        0U, changed, sizeof(changed), false, frame);
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_OK,
        uart_ota_consume(&ota, frame, frame_size, &consumed));
    TEST_ASSERT_EQUAL_UINT32(OTA_CHUNK_MAX_DATA, uart_ota_received(&ota));
    TEST_ASSERT_EQUAL(UART_OTA_RECEIPT_NACK,
                      fake.receipts[fake.receipt_count - 1U].type);
    TEST_ASSERT_EQUAL_STRING(
        "duplicate_mismatch",
        fake.receipts[fake.receipt_count - 1U].reason);
}

void test_fragmented_complete_image_validates_and_stops_at_frame_boundary(void)
{
    image_fixture_t fixture = valid_image_fixture();
    fake_t fake = fresh_fake();
    uart_ota_t ota;
    start_ota(&ota, &fake, &fixture, true);
    uint8_t frame[OTA_CHUNK_HEADER_SIZE + OTA_CHUNK_MAX_DATA +
                  OTA_CHUNK_CRC_SIZE + 3U];

    size_t frame_size = make_frame(
        0U, fixture.bytes, OTA_CHUNK_MAX_DATA, false, frame);
    for (size_t index = 0U; index < frame_size; ++index) {
        size_t consumed = 0U;
        TEST_ASSERT_EQUAL(UART_OTA_RESULT_OK,
            uart_ota_consume(&ota, frame + index, 1U, &consumed));
        TEST_ASSERT_EQUAL_UINT(1U, consumed);
    }
    frame_size = make_frame(
        1U, fixture.bytes + OTA_CHUNK_MAX_DATA,
        OTA_CHUNK_MAX_DATA, false, frame);
    size_t consumed = 0U;
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_OK,
        uart_ota_consume(&ota, frame, frame_size, &consumed));
    const uint16_t tail = (uint16_t)(fixture.length - 2U * OTA_CHUNK_MAX_DATA);
    frame_size = make_frame(
        2U, fixture.bytes + 2U * OTA_CHUNK_MAX_DATA,
        tail, false, frame);
    frame[frame_size] = '{';
    frame[frame_size + 1U] = '}';
    frame[frame_size + 2U] = '\n';
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_OK,
        uart_ota_consume(&ota, frame, frame_size + 3U, &consumed));
    TEST_ASSERT_EQUAL_UINT(frame_size, consumed);
    TEST_ASSERT_EQUAL(UART_OTA_STATE_IMAGE_STAGED, ota.state);
    TEST_ASSERT_EQUAL_UINT32(fixture.length, uart_ota_received(&ota));
    TEST_ASSERT_EQUAL(UART_OTA_RECEIPT_ACK,
                      fake.receipts[fake.receipt_count - 2U].type);
    TEST_ASSERT_EQUAL(UART_OTA_RECEIPT_STAGED,
                      fake.receipts[fake.receipt_count - 1U].type);
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_OK,
                      fake.receipts[fake.receipt_count - 1U].image_result);
    TEST_ASSERT_EQUAL_UINT(0U, fake.flash_begins);
    TEST_ASSERT_EQUAL_UINT(0U, fake.pending_activations);
}

void test_dry_run_end_requires_exact_session_generation_and_never_mutates(void)
{
    image_fixture_t fixture = valid_image_fixture();
    fake_t fake = fresh_fake();
    uart_ota_t ota;
    start_ota(&ota, &fake, &fixture, true);
    stage_image(&ota, &fixture);

    backend_scanner_ota_finish_control_t finish = finish_control(8U, 12U);
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_WRONG_SESSION,
        uart_ota_end(&ota, &finish));
    finish = finish_control(7U, 13U);
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_STALE_GENERATION,
        uart_ota_end(&ota, &finish));
    finish = finish_control(7U, 12U);
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_OK, uart_ota_end(&ota, &finish));
    TEST_ASSERT_EQUAL(UART_OTA_STATE_DRY_RUN_COMPLETE, ota.state);
    TEST_ASSERT_EQUAL(UART_OTA_RECEIPT_DONE,
                      fake.receipts[fake.receipt_count - 1U].type);
    TEST_ASSERT_TRUE(fake.receipts[fake.receipt_count - 1U].dry_run);
    TEST_ASSERT_EQUAL_UINT(1U, fake.releases);
    TEST_ASSERT_EQUAL_UINT(0U, fake.flash_begins);
    TEST_ASSERT_EQUAL_UINT(0U, fake.flash_writes);
    TEST_ASSERT_EQUAL_UINT(0U, fake.pending_activations);
    TEST_ASSERT_EQUAL_UINT(0U, fake.reboots);

    const size_t receipt_count = fake.receipt_count;
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_OK, uart_ota_end(&ota, &finish));
    TEST_ASSERT_EQUAL_UINT(receipt_count + 1U, fake.receipt_count);
    TEST_ASSERT_EQUAL(UART_OTA_RECEIPT_DONE,
                      fake.receipts[fake.receipt_count - 1U].type);
    TEST_ASSERT_EQUAL_UINT(1U, fake.releases);
    TEST_ASSERT_EQUAL_UINT(0U, fake.flash_begins);
}

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
void test_probe_then_apply_uses_fresh_session_generation_for_same_manifest(void)
{
    image_fixture_t fixture = valid_image_fixture();
    fake_t fake = fresh_fake();
    uart_ota_t ota;
    const uart_ota_config_t config = config_for(&fake);
    backend_scanner_ota_begin_control_t begin = begin_for(&fixture, true);
    begin.generation = 40U;
    begin.manifest_generation = fixture.manifest.generation;
    TEST_ASSERT_TRUE(uart_ota_init(&ota, &config));
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_OK, uart_ota_begin(&ota, &begin));
    TEST_ASSERT_EQUAL_UINT32(
        fixture.manifest.generation, ota.manifest.generation);
    stage_image(&ota, &fixture);
    backend_scanner_ota_finish_control_t finish = finish_control(7U, 40U);
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_OK, uart_ota_end(&ota, &finish));
    TEST_ASSERT_EQUAL(UART_OTA_STATE_DRY_RUN_COMPLETE, ota.state);

    uart_ota_reset(&ota);
    begin.dry_run = false;
    begin.generation = 41U;
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_OK, uart_ota_begin(&ota, &begin));
    TEST_ASSERT_EQUAL(UART_OTA_STATE_STAGING, ota.state);
    TEST_ASSERT_EQUAL_UINT32(41U, ota.highest_generation);
    TEST_ASSERT_EQUAL_UINT32(
        fixture.manifest.generation, ota.manifest.generation);
}
#endif

void test_apply_writes_only_after_revalidation_then_sets_pending_and_reboots(void)
{
    image_fixture_t fixture = valid_image_fixture();
    fake_t fake = fresh_fake();
    uart_ota_t ota;
    start_ota(&ota, &fake, &fixture, false);
    stage_image(&ota, &fixture);
    backend_scanner_ota_finish_control_t finish = finish_control(7U, 12U);

    TEST_ASSERT_EQUAL(UART_OTA_RESULT_OK, uart_ota_end(&ota, &finish));
    TEST_ASSERT_EQUAL(UART_OTA_STATE_PENDING_VERIFY, ota.state);
    TEST_ASSERT_EQUAL_UINT(2U, fake.binding_reads);
    TEST_ASSERT_EQUAL_UINT(1U, fake.flash_begins);
    TEST_ASSERT_EQUAL_UINT(1U, fake.flash_writes);
    TEST_ASSERT_EQUAL_UINT(fixture.length, fake.flashed_size);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        fixture.bytes, fake.flashed, fixture.length);
    TEST_ASSERT_EQUAL_UINT(1U, fake.flash_finishes);
    TEST_ASSERT_EQUAL_UINT(0U, fake.flash_aborts);
    TEST_ASSERT_EQUAL_UINT(1U, fake.pending_activations);
    TEST_ASSERT_EQUAL_UINT(1U, fake.reboots);
    TEST_ASSERT_EQUAL_UINT(1U, fake.releases);
    TEST_ASSERT_EQUAL(UART_OTA_RECEIPT_DONE,
                      fake.receipts[fake.receipt_count - 1U].type);
    TEST_ASSERT_FALSE(fake.receipts[fake.receipt_count - 1U].dry_run);
}

void test_live_binding_or_staged_image_change_rejects_before_flash_mutation(void)
{
    image_fixture_t fixture = valid_image_fixture();
    fake_t fake = fresh_fake();
    uart_ota_t ota;
    start_ota(&ota, &fake, &fixture, false);
    stage_image(&ota, &fixture);
    fake.binding.topology_generation++;
    backend_scanner_ota_finish_control_t finish = finish_control(7U, 12U);
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_BINDING_MISMATCH,
        uart_ota_end(&ota, &finish));
    TEST_ASSERT_EQUAL_UINT(0U, fake.flash_begins);
    TEST_ASSERT_EQUAL_UINT(0U, fake.flash_writes);
    TEST_ASSERT_EQUAL_UINT(0U, fake.pending_activations);

    fake = fresh_fake();
    start_ota(&ota, &fake, &fixture, false);
    stage_image(&ota, &fixture);
    fake.staging[900U] ^= 1U;
    finish = finish_control(7U, 12U);
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_IMAGE_REJECTED,
        uart_ota_end(&ota, &finish));
    TEST_ASSERT_EQUAL_UINT(0U, fake.flash_begins);
    TEST_ASSERT_EQUAL_UINT(0U, fake.flash_writes);
    TEST_ASSERT_EQUAL_UINT(0U, fake.pending_activations);
}

void test_flash_failure_aborts_inactive_slot_without_select_or_reboot(void)
{
    image_fixture_t fixture = valid_image_fixture();
    fake_t fake = fresh_fake();
    uart_ota_t ota;
    start_ota(&ota, &fake, &fixture, false);
    stage_image(&ota, &fixture);
    fake.flash_write_ok = false;
    backend_scanner_ota_finish_control_t finish = finish_control(7U, 12U);

    TEST_ASSERT_EQUAL(UART_OTA_RESULT_FLASH_ERROR,
        uart_ota_end(&ota, &finish));
    TEST_ASSERT_EQUAL(UART_OTA_STATE_FAILED, ota.state);
    TEST_ASSERT_EQUAL_UINT(1U, fake.flash_begins);
    TEST_ASSERT_EQUAL_UINT(1U, fake.flash_writes);
    TEST_ASSERT_EQUAL_UINT(1U, fake.flash_aborts);
    TEST_ASSERT_EQUAL_UINT(0U, fake.flash_finishes);
    TEST_ASSERT_EQUAL_UINT(0U, fake.pending_activations);
    TEST_ASSERT_EQUAL_UINT(0U, fake.reboots);
}

void test_reboot_request_failure_emits_error_but_keeps_pending_verify_state(void)
{
    image_fixture_t fixture = valid_image_fixture();
    fake_t fake = fresh_fake();
    uart_ota_t ota;
    start_ota(&ota, &fake, &fixture, false);
    stage_image(&ota, &fixture);
    fake.reboot_ok = false;
    backend_scanner_ota_finish_control_t finish = finish_control(7U, 12U);

    TEST_ASSERT_EQUAL(UART_OTA_RESULT_FLASH_ERROR,
        uart_ota_end(&ota, &finish));
    TEST_ASSERT_EQUAL(UART_OTA_STATE_PENDING_VERIFY, ota.state);
    TEST_ASSERT_EQUAL_UINT(1U, fake.pending_activations);
    TEST_ASSERT_EQUAL_UINT(1U, fake.reboots);
    TEST_ASSERT_EQUAL_UINT(0U, fake.flash_aborts);
    TEST_ASSERT_EQUAL(UART_OTA_RECEIPT_ERROR,
                      fake.receipts[fake.receipt_count - 1U].type);
    TEST_ASSERT_EQUAL_STRING(
        "reboot_failed", fake.receipts[fake.receipt_count - 1U].reason);

    fake.reboot_ok = true;
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_OK, uart_ota_end(&ota, &finish));
    TEST_ASSERT_EQUAL(UART_OTA_STATE_PENDING_VERIFY, ota.state);
    TEST_ASSERT_EQUAL_UINT(1U, fake.flash_begins);
    TEST_ASSERT_EQUAL_UINT(1U, fake.flash_writes);
    TEST_ASSERT_EQUAL_UINT(1U, fake.pending_activations);
    TEST_ASSERT_EQUAL_UINT(2U, fake.reboots);
    TEST_ASSERT_EQUAL(UART_OTA_RECEIPT_DONE,
                      fake.receipts[fake.receipt_count - 1U].type);
}

void test_abort_and_reset_are_session_bound_and_preserve_replay_floor(void)
{
    image_fixture_t fixture = valid_image_fixture();
    fake_t fake = fresh_fake();
    uart_ota_t ota;
    start_ota(&ota, &fake, &fixture, false);
    backend_scanner_ota_finish_control_t abort_control =
        finish_control(8U, 12U);
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_WRONG_SESSION,
        uart_ota_abort(&ota, &abort_control));
    TEST_ASSERT_EQUAL_UINT(0U, fake.releases);
    abort_control = finish_control(7U, 12U);
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_OK,
        uart_ota_abort(&ota, &abort_control));
    TEST_ASSERT_EQUAL(UART_OTA_STATE_FAILED, ota.state);
    TEST_ASSERT_EQUAL_UINT(1U, fake.releases);
    TEST_ASSERT_EQUAL(UART_OTA_RECEIPT_ERROR,
                      fake.receipts[fake.receipt_count - 1U].type);
    TEST_ASSERT_EQUAL_STRING(
        "aborted", fake.receipts[fake.receipt_count - 1U].reason);

    uart_ota_reset(&ota);
    TEST_ASSERT_EQUAL(UART_OTA_STATE_IDLE, ota.state);
    backend_scanner_ota_begin_control_t begin = begin_for(&fixture, false);
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_STALE_GENERATION,
        uart_ota_begin(&ota, &begin));
}

void test_invalid_length_and_image_overrun_fail_closed(void)
{
    image_fixture_t fixture = valid_image_fixture();
    fake_t fake = fresh_fake();
    uart_ota_t ota;
    start_ota(&ota, &fake, &fixture, true);
    const uint8_t zero_length[OTA_CHUNK_HEADER_SIZE] = {
        OTA_CHUNK_MAGIC, 0U, 0U, 0U, 0U,
    };
    size_t consumed = 0U;
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_WIRE_ERROR,
        uart_ota_consume(
            &ota, zero_length, sizeof(zero_length), &consumed));
    TEST_ASSERT_EQUAL_UINT(sizeof(zero_length), consumed);
    TEST_ASSERT_EQUAL(UART_OTA_STATE_FAILED, ota.state);
    TEST_ASSERT_EQUAL_UINT(1U, fake.releases);

    fake = fresh_fake();
    start_ota(&ota, &fake, &fixture, true);
    uint8_t frame[OTA_CHUNK_HEADER_SIZE + OTA_CHUNK_MAX_DATA +
                  OTA_CHUNK_CRC_SIZE];
    for (uint16_t sequence = 0U; sequence < 2U; ++sequence) {
        const size_t frame_size = make_frame(
            sequence, fixture.bytes + sequence * OTA_CHUNK_MAX_DATA,
            OTA_CHUNK_MAX_DATA, false, frame);
        TEST_ASSERT_EQUAL(UART_OTA_RESULT_OK,
            uart_ota_consume(&ota, frame, frame_size, &consumed));
    }
    const size_t frame_size = make_frame(
        2U, fixture.bytes, OTA_CHUNK_MAX_DATA, false, frame);
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_WIRE_ERROR,
        uart_ota_consume(&ota, frame, frame_size, &consumed));
    TEST_ASSERT_EQUAL_UINT(OTA_CHUNK_HEADER_SIZE, consumed);
    TEST_ASSERT_EQUAL_UINT(0U, fake.flash_begins);
}

void test_null_arguments_and_known_crc_vector_fail_closed(void)
{
    fake_t fake = fresh_fake();
    uart_ota_config_t config = config_for(&fake);
    uart_ota_t ota;
    TEST_ASSERT_FALSE(uart_ota_init(NULL, &config));
    TEST_ASSERT_FALSE(uart_ota_init(&ota, NULL));
    TEST_ASSERT_EQUAL_HEX32(
        UINT32_C(0xCBF43926),
        uart_ota_chunk_crc32((const uint8_t *)"123456789", 9U));
    TEST_ASSERT_EQUAL_HEX32(0U, uart_ota_chunk_crc32(NULL, 1U));
    TEST_ASSERT_TRUE(uart_ota_init(&ota, &config));
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_INVALID_ARGUMENT,
        uart_ota_begin(&ota, NULL));
    size_t consumed = 99U;
    TEST_ASSERT_EQUAL(UART_OTA_RESULT_INVALID_ARGUMENT,
        uart_ota_consume(&ota, NULL, 1U, &consumed));
    TEST_ASSERT_EQUAL_UINT(0U, consumed);
    TEST_ASSERT_EQUAL_UINT(0U,
        uart_ota_receipt_to_json(NULL, NULL, 0U));
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_begin_admits_exact_backend_scanner_binding_and_emits_exact_ack);
    BACKEND_RUN_TEST(
        test_begin_rejects_cross_family_binding_capacity_stale_and_no_psram);
    BACKEND_RUN_TEST(
        test_profile_slot_capacity_rejects_the_first_oversize_scanner_image);
    BACKEND_RUN_TEST(
        test_exact_begin_retry_reemits_receipt_without_reallocation_or_reset);
    BACKEND_RUN_TEST(
        test_bad_crc_and_out_of_order_frames_nack_without_advancing);
    BACKEND_RUN_TEST(
        test_exact_chunk_retry_reacks_without_copy_and_changed_retry_nacks);
    BACKEND_RUN_TEST(
        test_fragmented_complete_image_validates_and_stops_at_frame_boundary);
    BACKEND_RUN_TEST(
        test_dry_run_end_requires_exact_session_generation_and_never_mutates);
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    BACKEND_RUN_TEST(
        test_probe_then_apply_uses_fresh_session_generation_for_same_manifest);
#endif
    BACKEND_RUN_TEST(
        test_apply_writes_only_after_revalidation_then_sets_pending_and_reboots);
    BACKEND_RUN_TEST(
        test_live_binding_or_staged_image_change_rejects_before_flash_mutation);
    BACKEND_RUN_TEST(
        test_flash_failure_aborts_inactive_slot_without_select_or_reboot);
    BACKEND_RUN_TEST(
        test_reboot_request_failure_emits_error_but_keeps_pending_verify_state);
    BACKEND_RUN_TEST(
        test_abort_and_reset_are_session_bound_and_preserve_replay_floor);
    BACKEND_RUN_TEST(test_invalid_length_and_image_overrun_fail_closed);
    BACKEND_RUN_TEST(test_null_arguments_and_known_crc_vector_fail_closed);
    return UNITY_END();
}
