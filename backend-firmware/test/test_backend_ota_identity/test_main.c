#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "backend_identity.h"
#include "backend_ota_identity.h"
#include "../support/backend_test_main.h"

#define FIXTURE_CAPACITY 2048U
#define FIXTURE_SEGMENT_DATA_SIZE 1024U
#define FIXTURE_IDENTITY_OFFSET 352U

typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t block[64];
    size_t block_size;
} reference_sha256_t;

typedef struct {
    uint8_t bytes[FIXTURE_CAPACITY + 8U];
    size_t length;
    size_t max_fragment;
    size_t fail_offset;
    bool fail_enabled;
} image_reader_t;

typedef struct {
    uint8_t bytes[FIXTURE_CAPACITY];
    size_t length;
    backend_ota_manifest_t manifest;
} image_fixture_t;

static uint32_t rotr32(uint32_t value, unsigned amount)
{
    return (value >> amount) | (value << (32U - amount));
}

static uint32_t read_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void write_be32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

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

static void reference_sha256_transform(reference_sha256_t *sha)
{
    static const uint32_t constants[64] = {
        0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U,
        0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U,
        0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U,
        0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U,
        0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU,
        0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
        0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
        0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U,
        0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U,
        0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U,
        0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U,
        0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
        0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U,
        0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
        0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U,
        0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U,
    };
    uint32_t words[64];
    for (size_t index = 0; index < 16; ++index) {
        words[index] = read_be32(sha->block + index * 4U);
    }
    for (size_t index = 16; index < 64; ++index) {
        uint32_t s0 = rotr32(words[index - 15], 7) ^
                      rotr32(words[index - 15], 18) ^
                      (words[index - 15] >> 3);
        uint32_t s1 = rotr32(words[index - 2], 17) ^
                      rotr32(words[index - 2], 19) ^
                      (words[index - 2] >> 10);
        words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }

    uint32_t a = sha->state[0];
    uint32_t b = sha->state[1];
    uint32_t c = sha->state[2];
    uint32_t d = sha->state[3];
    uint32_t e = sha->state[4];
    uint32_t f = sha->state[5];
    uint32_t g = sha->state[6];
    uint32_t h = sha->state[7];
    for (size_t index = 0; index < 64; ++index) {
        uint32_t big1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t choose = (e & f) ^ (~e & g);
        uint32_t first = h + big1 + choose + constants[index] + words[index];
        uint32_t big0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t second = big0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    sha->state[0] += a;
    sha->state[1] += b;
    sha->state[2] += c;
    sha->state[3] += d;
    sha->state[4] += e;
    sha->state[5] += f;
    sha->state[6] += g;
    sha->state[7] += h;
}

static void reference_sha256_init(reference_sha256_t *sha)
{
    memset(sha, 0, sizeof(*sha));
    sha->state[0] = 0x6A09E667U;
    sha->state[1] = 0xBB67AE85U;
    sha->state[2] = 0x3C6EF372U;
    sha->state[3] = 0xA54FF53AU;
    sha->state[4] = 0x510E527FU;
    sha->state[5] = 0x9B05688CU;
    sha->state[6] = 0x1F83D9ABU;
    sha->state[7] = 0x5BE0CD19U;
}

static void reference_sha256_update(
    reference_sha256_t *sha, const uint8_t *bytes, size_t length)
{
    sha->bit_count += (uint64_t)length * 8U;
    while (length > 0) {
        size_t available = sizeof(sha->block) - sha->block_size;
        size_t take = length < available ? length : available;
        memcpy(sha->block + sha->block_size, bytes, take);
        sha->block_size += take;
        bytes += take;
        length -= take;
        if (sha->block_size == sizeof(sha->block)) {
            reference_sha256_transform(sha);
            sha->block_size = 0;
        }
    }
}

static void reference_sha256_finish(reference_sha256_t *sha, uint8_t out[32])
{
    sha->block[sha->block_size++] = 0x80U;
    if (sha->block_size > 56U) {
        memset(sha->block + sha->block_size, 0,
               sizeof(sha->block) - sha->block_size);
        reference_sha256_transform(sha);
        sha->block_size = 0;
    }
    memset(sha->block + sha->block_size, 0, 56U - sha->block_size);
    for (size_t index = 0; index < 8; ++index) {
        sha->block[63U - index] =
            (uint8_t)(sha->bit_count >> (index * 8U));
    }
    reference_sha256_transform(sha);
    for (size_t index = 0; index < 8; ++index) {
        write_be32(out + index * 4U, sha->state[index]);
    }
}

static uint32_t reference_crc32(const uint8_t *bytes, size_t length)
{
    uint32_t crc = UINT32_C(0xFFFFFFFF);
    for (size_t index = 0; index < length; ++index) {
        crc ^= bytes[index];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1U) ? UINT32_C(0xEDB88320) : 0U);
        }
    }
    return ~crc;
}

static void fixture_refresh_digests(image_fixture_t *fixture)
{
    static const char digits[] = "0123456789abcdef";
    reference_sha256_t sha;
    uint8_t digest[32];
    reference_sha256_init(&sha);
    reference_sha256_update(&sha, fixture->bytes, fixture->length);
    reference_sha256_finish(&sha, digest);
    for (size_t index = 0; index < sizeof(digest); ++index) {
        fixture->manifest.sha256[index * 2U] = digits[digest[index] >> 4];
        fixture->manifest.sha256[index * 2U + 1U] =
            digits[digest[index] & 0x0FU];
    }
    fixture->manifest.sha256[64] = '\0';
    fixture->manifest.image_size = (uint32_t)fixture->length;
    fixture->manifest.crc32 = reference_crc32(fixture->bytes, fixture->length);
}

static void copy_fixed(uint8_t *field, size_t width, const char *value)
{
    size_t length = strlen(value);
    TEST_ASSERT_LESS_THAN(width, length);
    memset(field, 0, width);
    memcpy(field, value, length);
}

static void fixture_write_identity(
    image_fixture_t *fixture,
    backend_image_kind_t kind,
    const char *target,
    const char *project,
    const char *hardware,
    const char *version)
{
    uint8_t *record = fixture->bytes + FIXTURE_IDENTITY_OFFSET;
    memset(record, 0, sizeof(backend_embedded_identity_record_t));
    write_le32(record, FOF_BACKEND_IDENTITY_MAGIC);
    write_le16(record + 4, FOF_BACKEND_IDENTITY_SCHEMA);
    write_le16(record + 6, (uint16_t)kind);
    copy_fixed(record + 8, 40, target);
    copy_fixed(record + 48, 40, project);
    copy_fixed(record + 88, 40, hardware);
    copy_fixed(record + 128, 32, version);
    write_le32(record + 160, reference_crc32(record, 160));
}

static void fixture_refresh_esp_checksum(image_fixture_t *fixture)
{
    uint8_t checksum = 0xEFU;
    size_t offset = 24U;
    for (uint8_t segment = 0; segment < fixture->bytes[1]; ++segment) {
        uint32_t length = read_le32(fixture->bytes + offset + 4U);
        offset += 8U;
        for (size_t index = 0; index < length; ++index) {
            checksum ^= fixture->bytes[offset + index];
        }
        offset += length;
    }
    size_t checksum_end = (offset + 16U) & ~(size_t)15U;
    fixture->bytes[checksum_end - 1U] = checksum;
}

static image_fixture_t valid_fixture(backend_image_kind_t kind)
{
    image_fixture_t fixture;
    const char *target = kind == BACKEND_IMAGE_SCANNER
        ? FOF_BACKEND_SCANNER_TARGET : FOF_BACKEND_UPLINK_TARGET;
    const char *project = kind == BACKEND_IMAGE_SCANNER
        ? FOF_BACKEND_SCANNER_PROJECT : FOF_BACKEND_UPLINK_PROJECT;
    const char *version = "0.1.1-backend";
    memset(&fixture, 0, sizeof(fixture));
    memset(fixture.bytes, 0xA5, sizeof(fixture.bytes));

    fixture.bytes[0] = 0xE9U;
    fixture.bytes[1] = 1U;
    fixture.bytes[2] = 2U;
    fixture.bytes[3] = 0x20U;
    write_le32(fixture.bytes + 4, UINT32_C(0x40370000));
    fixture.bytes[8] = 0xEEU;
    write_le16(fixture.bytes + 12, 0x0009U);
    fixture.bytes[23] = 0U;
    write_le32(fixture.bytes + 24, UINT32_C(0x3C000020));
    write_le32(fixture.bytes + 28, FIXTURE_SEGMENT_DATA_SIZE);

    write_le32(fixture.bytes + 32, UINT32_C(0xABCD5432));
    copy_fixed(fixture.bytes + 48, 32, version);
    copy_fixed(fixture.bytes + 80, 32, project);
    for (size_t index = 288; index < 32U + FIXTURE_SEGMENT_DATA_SIZE; ++index) {
        fixture.bytes[index] = (uint8_t)(index * 37U + 11U);
    }
    fixture_write_identity(
        &fixture, kind, target, project, FOF_BACKEND_HARDWARE, version);

    const size_t segments_end = 32U + FIXTURE_SEGMENT_DATA_SIZE;
    fixture.length = (segments_end + 16U) & ~(size_t)15U;
    memset(fixture.bytes + segments_end, 0, fixture.length - segments_end);
    fixture_refresh_esp_checksum(&fixture);

    strcpy(fixture.manifest.target, target);
    strcpy(fixture.manifest.project, project);
    strcpy(fixture.manifest.hardware, FOF_BACKEND_HARDWARE);
    strcpy(fixture.manifest.version, version);
    fixture.manifest.generation = 7U;
    fixture.manifest.allow_same_version = false;
    fixture_refresh_digests(&fixture);
    return fixture;
}

static image_fixture_t fixture_with_appended_hash(backend_image_kind_t kind)
{
    image_fixture_t fixture = valid_fixture(kind);
    reference_sha256_t sha;
    uint8_t digest[32];
    fixture.bytes[23] = 1U;
    reference_sha256_init(&sha);
    reference_sha256_update(&sha, fixture.bytes, fixture.length);
    reference_sha256_finish(&sha, digest);
    memcpy(fixture.bytes + fixture.length, digest, sizeof(digest));
    fixture.length += sizeof(digest);
    fixture_refresh_digests(&fixture);
    return fixture;
}

static image_fixture_t fixture_with_second_segment_identity(void)
{
    image_fixture_t fixture = valid_fixture(BACKEND_IMAGE_SCANNER);
    uint8_t identity[sizeof(backend_embedded_identity_record_t)];
    memcpy(identity, fixture.bytes + FIXTURE_IDENTITY_OFFSET,
           sizeof(identity));

    fixture.bytes[1] = 2U;
    write_le32(fixture.bytes + 28U, 512U);
    memmove(fixture.bytes + 552U, fixture.bytes + 544U, 512U);
    write_le32(fixture.bytes + 544U, UINT32_C(0x3FC80000));
    write_le32(fixture.bytes + 548U, 512U);
    for (size_t index = FIXTURE_IDENTITY_OFFSET;
         index < FIXTURE_IDENTITY_OFFSET + sizeof(identity); ++index) {
        fixture.bytes[index] = (uint8_t)(index * 17U + 3U);
    }
    memcpy(fixture.bytes + 700U, identity, sizeof(identity));
    memset(fixture.bytes + 1064U, 0, 8U);
    fixture.length = 1072U;
    fixture_refresh_esp_checksum(&fixture);
    fixture_refresh_digests(&fixture);
    return fixture;
}

static image_reader_t fixture_reader(
    const image_fixture_t *fixture, size_t max_fragment)
{
    image_reader_t reader;
    memset(&reader, 0, sizeof(reader));
    memcpy(reader.bytes, fixture->bytes, fixture->length);
    reader.length = fixture->length;
    reader.max_fragment = max_fragment;
    return reader;
}

static bool read_image(
    void *context, size_t offset, uint8_t *output, size_t length)
{
    image_reader_t *reader = context;
    if (reader == NULL || output == NULL || length == 0U ||
        offset > reader->length || length > reader->length - offset) {
        return false;
    }
    if (reader->fail_enabled && reader->fail_offset >= offset &&
        reader->fail_offset - offset < length) {
        return false;
    }
    size_t copied = 0;
    while (copied < length) {
        size_t amount = length - copied;
        if (reader->max_fragment != 0U && amount > reader->max_fragment) {
            amount = reader->max_fragment;
        }
        memcpy(output + copied, reader->bytes + offset + copied, amount);
        copied += amount;
    }
    return true;
}

static backend_ota_manifest_t valid_scanner_manifest(void)
{
    image_fixture_t fixture = valid_fixture(BACKEND_IMAGE_SCANNER);
    return fixture.manifest;
}

void setUp(void)
{
}

void tearDown(void)
{
}

void test_backend_ota_manifest_rejects_cross_family_and_invalid_admission(void)
{
    backend_ota_manifest_t manifest = valid_scanner_manifest();
    TEST_ASSERT_EQUAL(BACKEND_OTA_ADMIT, backend_ota_manifest_admit(
        &manifest, BACKEND_IMAGE_SCANNER, "0.1.0-backend", 0x200000U));

    strcpy(manifest.target, "scanner-s3-combo-fof_badge");
    TEST_ASSERT_EQUAL(BACKEND_OTA_REJECT_IDENTITY, backend_ota_manifest_admit(
        &manifest, BACKEND_IMAGE_SCANNER, "0.1.0-backend", 0x200000U));
    manifest = valid_scanner_manifest();
    strcpy(manifest.project, "fof_scanner_seed");
    TEST_ASSERT_EQUAL(BACKEND_OTA_REJECT_IDENTITY, backend_ota_manifest_admit(
        &manifest, BACKEND_IMAGE_SCANNER, "0.1.0-backend", 0x200000U));
    manifest = valid_scanner_manifest();
    strcpy(manifest.hardware, "xiao_esp32s3");
    TEST_ASSERT_EQUAL(BACKEND_OTA_REJECT_IDENTITY, backend_ota_manifest_admit(
        &manifest, BACKEND_IMAGE_SCANNER, "0.1.0-backend", 0x200000U));

    manifest = valid_scanner_manifest();
    manifest.sha256[0] = '\0';
    TEST_ASSERT_EQUAL(BACKEND_OTA_REJECT_DIGEST, backend_ota_manifest_admit(
        &manifest, BACKEND_IMAGE_SCANNER, "0.1.0-backend", 0x200000U));
    manifest = valid_scanner_manifest();
    manifest.sha256[17] = 'g';
    TEST_ASSERT_EQUAL(BACKEND_OTA_REJECT_DIGEST, backend_ota_manifest_admit(
        &manifest, BACKEND_IMAGE_SCANNER, "0.1.0-backend", 0x200000U));
    manifest = valid_scanner_manifest();
    manifest.generation = 0;
    TEST_ASSERT_EQUAL(BACKEND_OTA_REJECT_GENERATION, backend_ota_manifest_admit(
        &manifest, BACKEND_IMAGE_SCANNER, "0.1.0-backend", 0x200000U));
    manifest = valid_scanner_manifest();
    strcpy(manifest.version, "0.0.9-backend");
    TEST_ASSERT_EQUAL(BACKEND_OTA_REJECT_VERSION, backend_ota_manifest_admit(
        &manifest, BACKEND_IMAGE_SCANNER, "0.1.0-backend", 0x200000U));
    manifest = valid_scanner_manifest();
    strcpy(manifest.version, "0.2.0-badge");
    TEST_ASSERT_EQUAL(BACKEND_OTA_REJECT_VERSION, backend_ota_manifest_admit(
        &manifest, BACKEND_IMAGE_SCANNER, "0.1.0-backend", 0x200000U));
    manifest = valid_scanner_manifest();
    strcpy(manifest.version, "0.1.0-backend");
    TEST_ASSERT_EQUAL(BACKEND_OTA_REJECT_VERSION, backend_ota_manifest_admit(
        &manifest, BACKEND_IMAGE_SCANNER, "0.1.0-backend", 0x200000U));
    manifest.allow_same_version = true;
    TEST_ASSERT_EQUAL(BACKEND_OTA_ADMIT, backend_ota_manifest_admit(
        &manifest, BACKEND_IMAGE_SCANNER, "0.1.0-backend", 0x200000U));

    manifest = valid_scanner_manifest();
    manifest.image_size = 0;
    TEST_ASSERT_EQUAL(BACKEND_OTA_REJECT_SIZE, backend_ota_manifest_admit(
        &manifest, BACKEND_IMAGE_SCANNER, "0.1.0-backend", 0x200000U));
    manifest = valid_scanner_manifest();
    manifest.image_size = 0x200001U;
    TEST_ASSERT_EQUAL(BACKEND_OTA_REJECT_CAPACITY, backend_ota_manifest_admit(
        &manifest, BACKEND_IMAGE_SCANNER, "0.1.0-backend", 0x200000U));
    TEST_ASSERT_EQUAL(BACKEND_OTA_REJECT_ARGUMENT, backend_ota_manifest_admit(
        NULL, BACKEND_IMAGE_SCANNER, "0.1.0-backend", 0x200000U));
    TEST_ASSERT_EQUAL(BACKEND_OTA_REJECT_ARGUMENT, backend_ota_manifest_admit(
        &manifest, (backend_image_kind_t)9, "0.1.0-backend", 0x200000U));

    manifest = valid_scanner_manifest();
    manifest.crc32 = 0U;
    TEST_ASSERT_EQUAL(BACKEND_OTA_ADMIT, backend_ota_manifest_admit(
        &manifest, BACKEND_IMAGE_SCANNER, "0.1.0-backend", 0x200000U));
    manifest = valid_scanner_manifest();
    memset(manifest.target, 'x', sizeof(manifest.target));
    TEST_ASSERT_EQUAL(BACKEND_OTA_REJECT_IDENTITY, backend_ota_manifest_admit(
        &manifest, BACKEND_IMAGE_SCANNER, "0.1.0-backend", 0x200000U));
}

void test_backend_ota_metadata_requires_exact_unsigned_crc32_member(void)
{
    static const char base[] =
        "{\"name\":\"scanner-s3-combo-backend\","
        "\"target\":\"scanner-s3-combo-backend\","
        "\"description\":\"Backend Lite scanner\",\"board\":\"esp32s3\","
        "\"project\":\"fof_backend_scanner\","
        "\"hardware\":\"seeed_xiao_esp32s3\","
        "\"version\":\"0.1.1-backend\",\"size\":1056,"
        "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
        "\"crc32\":0,"
        "\"download_url\":\"/nodes/firmware/download/scanner-s3-combo-backend\"}";
    backend_ota_manifest_t manifest;
    TEST_ASSERT_TRUE(backend_ota_manifest_decode_metadata(
        base, sizeof(base) - 1U, 9U, false, &manifest));
    TEST_ASSERT_EQUAL_UINT32(0U, manifest.crc32);
    TEST_ASSERT_EQUAL_UINT32(9U, manifest.generation);

    static const char *invalid[] = {
        "{\"name\":\"scanner-s3-combo-backend\",\"target\":\"scanner-s3-combo-backend\",\"description\":\"x\",\"board\":\"esp32s3\",\"project\":\"fof_backend_scanner\",\"hardware\":\"seeed_xiao_esp32s3\",\"version\":\"0.1.1-backend\",\"size\":1056,\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"download_url\":\"/nodes/firmware/download/scanner-s3-combo-backend\"}",
        "{\"name\":\"scanner-s3-combo-backend\",\"target\":\"scanner-s3-combo-backend\",\"description\":\"x\",\"board\":\"esp32s3\",\"project\":\"fof_backend_scanner\",\"hardware\":\"seeed_xiao_esp32s3\",\"version\":\"0.1.1-backend\",\"size\":1056,\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"crc32\":0,\"crc32\":1,\"download_url\":\"/nodes/firmware/download/scanner-s3-combo-backend\"}",
        "{\"name\":\"scanner-s3-combo-backend\",\"target\":\"scanner-s3-combo-backend\",\"description\":\"x\",\"board\":\"esp32s3\",\"project\":\"fof_backend_scanner\",\"hardware\":\"seeed_xiao_esp32s3\",\"version\":\"0.1.1-backend\",\"size\":1056,\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"crc32\":-1,\"download_url\":\"/nodes/firmware/download/scanner-s3-combo-backend\"}",
        "{\"name\":\"scanner-s3-combo-backend\",\"target\":\"scanner-s3-combo-backend\",\"description\":\"x\",\"board\":\"esp32s3\",\"project\":\"fof_backend_scanner\",\"hardware\":\"seeed_xiao_esp32s3\",\"version\":\"0.1.1-backend\",\"size\":1056,\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"crc32\":1.5,\"download_url\":\"/nodes/firmware/download/scanner-s3-combo-backend\"}",
        "{\"name\":\"scanner-s3-combo-backend\",\"target\":\"scanner-s3-combo-backend\",\"description\":\"x\",\"board\":\"esp32s3\",\"project\":\"fof_backend_scanner\",\"hardware\":\"seeed_xiao_esp32s3\",\"version\":\"0.1.1-backend\",\"size\":1056,\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"crc32\":4294967296,\"download_url\":\"/nodes/firmware/download/scanner-s3-combo-backend\"}",
    };
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        TEST_ASSERT_FALSE(backend_ota_manifest_decode_metadata(
            invalid[index], strlen(invalid[index]), 9U, false, &manifest));
    }

    char extra[sizeof(base) + 16U];
    int extra_length = snprintf(extra, sizeof(extra), "%.*s,\"extra\":1}",
                                (int)sizeof(base) - 2, base);
    TEST_ASSERT_GREATER_THAN(0, extra_length);
    TEST_ASSERT_FALSE(backend_ota_manifest_decode_metadata(
        extra, (size_t)extra_length, 9U, false, &manifest));
    TEST_ASSERT_FALSE(backend_ota_manifest_decode_metadata(
        base, sizeof(base) - 1U, 0U, false, &manifest));
}

void test_backend_ota_complete_image_accepts_fragmented_exact_reader(void)
{
    image_fixture_t fixture = valid_fixture(BACKEND_IMAGE_SCANNER);
    TEST_ASSERT_EQUAL_STRING(
        "e786a9b17b2b9c8d3043fe237294ecdfb7e504987a12dd5eae4301b0146d9fe7",
        fixture.manifest.sha256);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0xA473F422), fixture.manifest.crc32);
    static const size_t fragments[] = {1U, 7U, 511U};
    for (size_t index = 0; index < sizeof(fragments) / sizeof(fragments[0]); ++index) {
        image_reader_t reader = fixture_reader(&fixture, fragments[index]);
        TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_OK, backend_ota_image_validate(
            &fixture.manifest, BACKEND_IMAGE_SCANNER, read_image, &reader));
    }

    fixture = valid_fixture(BACKEND_IMAGE_UPLINK);
    image_reader_t uplink_reader = fixture_reader(&fixture, 7U);
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_OK, backend_ota_image_validate(
        &fixture.manifest, BACKEND_IMAGE_UPLINK, read_image, &uplink_reader));

    fixture = valid_fixture(BACKEND_IMAGE_SCANNER);
    memmove(fixture.bytes + FIXTURE_IDENTITY_OFFSET + 1U,
            fixture.bytes + FIXTURE_IDENTITY_OFFSET,
            sizeof(backend_embedded_identity_record_t));
    fixture.bytes[FIXTURE_IDENTITY_OFFSET] = 0x11U;
    fixture_refresh_esp_checksum(&fixture);
    fixture_refresh_digests(&fixture);
    image_reader_t unaligned_reader = fixture_reader(&fixture, 1U);
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_OK, backend_ota_image_validate(
        &fixture.manifest, BACKEND_IMAGE_SCANNER,
        read_image, &unaligned_reader));

    fixture = valid_fixture(BACKEND_IMAGE_SCANNER);
    uint8_t boundary_identity[sizeof(backend_embedded_identity_record_t)];
    memcpy(boundary_identity, fixture.bytes + FIXTURE_IDENTITY_OFFSET,
           sizeof(boundary_identity));
    for (size_t index = FIXTURE_IDENTITY_OFFSET;
         index < FIXTURE_IDENTITY_OFFSET + sizeof(boundary_identity); ++index) {
        fixture.bytes[index] = (uint8_t)(index * 13U + 5U);
    }
    memcpy(fixture.bytes + 540U, boundary_identity,
           sizeof(boundary_identity));
    fixture_refresh_esp_checksum(&fixture);
    fixture_refresh_digests(&fixture);
    image_reader_t boundary_reader = fixture_reader(&fixture, 511U);
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_OK, backend_ota_image_validate(
        &fixture.manifest, BACKEND_IMAGE_SCANNER,
        read_image, &boundary_reader));

    fixture = fixture_with_appended_hash(BACKEND_IMAGE_SCANNER);
    image_reader_t hashed_reader = fixture_reader(&fixture, 7U);
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_OK, backend_ota_image_validate(
        &fixture.manifest, BACKEND_IMAGE_SCANNER, read_image, &hashed_reader));
    hashed_reader.bytes[hashed_reader.length - 1U] ^= 1U;
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_FORMAT_ERROR,
        backend_ota_image_validate(&fixture.manifest, BACKEND_IMAGE_SCANNER,
                                   read_image, &hashed_reader));

    fixture = fixture_with_second_segment_identity();
    image_reader_t second_segment_reader = fixture_reader(&fixture, 511U);
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_OK, backend_ota_image_validate(
        &fixture.manifest, BACKEND_IMAGE_SCANNER,
        read_image, &second_segment_reader));
}

void test_backend_ota_complete_image_rejects_read_failures_short_and_trailing(void)
{
    image_fixture_t fixture = valid_fixture(BACKEND_IMAGE_SCANNER);
    static const size_t fail_offsets[] = {0U, 24U, 32U, FIXTURE_IDENTITY_OFFSET,
                                         900U, 1055U, 1071U};
    for (size_t index = 0;
         index < sizeof(fail_offsets) / sizeof(fail_offsets[0]); ++index) {
        image_reader_t reader = fixture_reader(&fixture, 7U);
        reader.fail_enabled = true;
        reader.fail_offset = fail_offsets[index];
        TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_READ_ERROR,
            backend_ota_image_validate(&fixture.manifest, BACKEND_IMAGE_SCANNER,
                                       read_image, &reader));
    }

    image_reader_t short_reader = fixture_reader(&fixture, 7U);
    --short_reader.length;
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_READ_ERROR, backend_ota_image_validate(
        &fixture.manifest, BACKEND_IMAGE_SCANNER, read_image, &short_reader));

    image_reader_t trailing_reader = fixture_reader(&fixture, 7U);
    trailing_reader.bytes[trailing_reader.length++] = 0x42U;
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_FORMAT_ERROR, backend_ota_image_validate(
        &fixture.manifest, BACKEND_IMAGE_SCANNER, read_image, &trailing_reader));

    fixture = fixture_with_appended_hash(BACKEND_IMAGE_SCANNER);
    image_reader_t hash_reader = fixture_reader(&fixture, 7U);
    hash_reader.fail_enabled = true;
    hash_reader.fail_offset = fixture.length - 1U;
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_READ_ERROR, backend_ota_image_validate(
        &fixture.manifest, BACKEND_IMAGE_SCANNER, read_image, &hash_reader));
}

void test_backend_ota_complete_image_rejects_malformed_esp_container(void)
{
    image_fixture_t fixture = valid_fixture(BACKEND_IMAGE_SCANNER);
    image_reader_t reader;

    fixture.bytes[0] = 0;
    reader = fixture_reader(&fixture, 511U);
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_FORMAT_ERROR, backend_ota_image_validate(
        &fixture.manifest, BACKEND_IMAGE_SCANNER, read_image, &reader));

    fixture = valid_fixture(BACKEND_IMAGE_SCANNER);
    fixture.bytes[1] = 17U;
    reader = fixture_reader(&fixture, 511U);
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_FORMAT_ERROR, backend_ota_image_validate(
        &fixture.manifest, BACKEND_IMAGE_SCANNER, read_image, &reader));

    fixture = valid_fixture(BACKEND_IMAGE_SCANNER);
    write_le32(fixture.bytes + 28, UINT32_MAX);
    reader = fixture_reader(&fixture, 511U);
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_FORMAT_ERROR, backend_ota_image_validate(
        &fixture.manifest, BACKEND_IMAGE_SCANNER, read_image, &reader));

    fixture = valid_fixture(BACKEND_IMAGE_SCANNER);
    write_le32(fixture.bytes + 28, FIXTURE_SEGMENT_DATA_SIZE - 1U);
    reader = fixture_reader(&fixture, 511U);
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_FORMAT_ERROR, backend_ota_image_validate(
        &fixture.manifest, BACKEND_IMAGE_SCANNER, read_image, &reader));

    fixture = valid_fixture(BACKEND_IMAGE_SCANNER);
    write_le32(fixture.bytes + 24, UINT32_MAX - 1U);
    reader = fixture_reader(&fixture, 511U);
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_FORMAT_ERROR, backend_ota_image_validate(
        &fixture.manifest, BACKEND_IMAGE_SCANNER, read_image, &reader));

    fixture = valid_fixture(BACKEND_IMAGE_SCANNER);
    fixture.bytes[fixture.length - 1U] ^= 1U;
    reader = fixture_reader(&fixture, 511U);
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_FORMAT_ERROR, backend_ota_image_validate(
        &fixture.manifest, BACKEND_IMAGE_SCANNER, read_image, &reader));
}

void test_backend_ota_complete_image_requires_exact_descriptor_and_identity(void)
{
    image_fixture_t fixture = valid_fixture(BACKEND_IMAGE_SCANNER);
    image_reader_t reader;

    fixture.bytes[80] = 'x';
    fixture_refresh_esp_checksum(&fixture);
    reader = fixture_reader(&fixture, 511U);
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_DESCRIPTOR_MISMATCH,
        backend_ota_image_validate(&fixture.manifest, BACKEND_IMAGE_SCANNER,
                                   read_image, &reader));

    fixture = valid_fixture(BACKEND_IMAGE_SCANNER);
    fixture.bytes[48] = '9';
    fixture_refresh_esp_checksum(&fixture);
    reader = fixture_reader(&fixture, 511U);
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_DESCRIPTOR_MISMATCH,
        backend_ota_image_validate(&fixture.manifest, BACKEND_IMAGE_SCANNER,
                                   read_image, &reader));

    fixture = valid_fixture(BACKEND_IMAGE_SCANNER);
    fixture.bytes[32] ^= 1U;
    fixture_refresh_esp_checksum(&fixture);
    reader = fixture_reader(&fixture, 511U);
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_DESCRIPTOR_MISMATCH,
        backend_ota_image_validate(&fixture.manifest, BACKEND_IMAGE_SCANNER,
                                   read_image, &reader));

    fixture = valid_fixture(BACKEND_IMAGE_SCANNER);
    fixture.bytes[FIXTURE_IDENTITY_OFFSET] ^= 1U;
    fixture_refresh_esp_checksum(&fixture);
    reader = fixture_reader(&fixture, 511U);
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_IDENTITY_MISMATCH,
        backend_ota_image_validate(&fixture.manifest, BACKEND_IMAGE_SCANNER,
                                   read_image, &reader));

    fixture = valid_fixture(BACKEND_IMAGE_SCANNER);
    memcpy(fixture.bytes + 700U,
           fixture.bytes + FIXTURE_IDENTITY_OFFSET,
           sizeof(backend_embedded_identity_record_t));
    fixture_refresh_esp_checksum(&fixture);
    reader = fixture_reader(&fixture, 511U);
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_IDENTITY_MISMATCH,
        backend_ota_image_validate(&fixture.manifest, BACKEND_IMAGE_SCANNER,
                                   read_image, &reader));

    fixture = valid_fixture(BACKEND_IMAGE_SCANNER);
    fixture.bytes[FIXTURE_IDENTITY_OFFSET + 160U] ^= 1U;
    fixture_refresh_esp_checksum(&fixture);
    reader = fixture_reader(&fixture, 511U);
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_IDENTITY_MISMATCH,
        backend_ota_image_validate(&fixture.manifest, BACKEND_IMAGE_SCANNER,
                                   read_image, &reader));

    fixture = valid_fixture(BACKEND_IMAGE_SCANNER);
    fixture.bytes[FIXTURE_IDENTITY_OFFSET + 6U] = BACKEND_IMAGE_UPLINK;
    write_le32(fixture.bytes + FIXTURE_IDENTITY_OFFSET + 160U,
        reference_crc32(fixture.bytes + FIXTURE_IDENTITY_OFFSET, 160U));
    fixture_refresh_esp_checksum(&fixture);
    reader = fixture_reader(&fixture, 511U);
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_IDENTITY_MISMATCH,
        backend_ota_image_validate(&fixture.manifest, BACKEND_IMAGE_SCANNER,
                                   read_image, &reader));

    fixture = valid_fixture(BACKEND_IMAGE_SCANNER);
    fixture.bytes[FIXTURE_IDENTITY_OFFSET + 8U] = 'x';
    write_le32(fixture.bytes + FIXTURE_IDENTITY_OFFSET + 160U,
        reference_crc32(fixture.bytes + FIXTURE_IDENTITY_OFFSET, 160U));
    fixture_refresh_esp_checksum(&fixture);
    reader = fixture_reader(&fixture, 511U);
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_IDENTITY_MISMATCH,
        backend_ota_image_validate(&fixture.manifest, BACKEND_IMAGE_SCANNER,
                                   read_image, &reader));

    static const size_t identity_field_offsets[] = {
        4U, 48U, 88U, 128U, 47U,
    };
    for (size_t index = 0;
         index < sizeof(identity_field_offsets) /
                     sizeof(identity_field_offsets[0]); ++index) {
        fixture = valid_fixture(BACKEND_IMAGE_SCANNER);
        fixture.bytes[FIXTURE_IDENTITY_OFFSET +
                      identity_field_offsets[index]] ^= 1U;
        write_le32(fixture.bytes + FIXTURE_IDENTITY_OFFSET + 160U,
            reference_crc32(fixture.bytes + FIXTURE_IDENTITY_OFFSET, 160U));
        fixture_refresh_esp_checksum(&fixture);
        reader = fixture_reader(&fixture, 511U);
        TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_IDENTITY_MISMATCH,
            backend_ota_image_validate(&fixture.manifest,
                                       BACKEND_IMAGE_SCANNER,
                                       read_image, &reader));
    }
}

void test_backend_ota_same_version_recovery_still_validates_complete_image(void)
{
    image_fixture_t fixture = valid_fixture(BACKEND_IMAGE_SCANNER);
    fixture.manifest.allow_same_version = true;
    TEST_ASSERT_EQUAL(BACKEND_OTA_ADMIT, backend_ota_manifest_admit(
        &fixture.manifest, BACKEND_IMAGE_SCANNER,
        fixture.manifest.version, 0x200000U));
    image_reader_t reader = fixture_reader(&fixture, 7U);
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_OK, backend_ota_image_validate(
        &fixture.manifest, BACKEND_IMAGE_SCANNER, read_image, &reader));
    fixture.bytes[900U] ^= 1U;
    fixture_refresh_esp_checksum(&fixture);
    reader = fixture_reader(&fixture, 7U);
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_DIGEST_MISMATCH,
        backend_ota_image_validate(&fixture.manifest, BACKEND_IMAGE_SCANNER,
                                   read_image, &reader));
}

void test_backend_ota_complete_image_checks_whole_digest_and_crc_including_zero(void)
{
    image_fixture_t fixture = valid_fixture(BACKEND_IMAGE_SCANNER);
    image_reader_t reader = fixture_reader(&fixture, 511U);

    fixture.manifest.sha256[0] = fixture.manifest.sha256[0] == '0' ? '1' : '0';
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_DIGEST_MISMATCH,
        backend_ota_image_validate(&fixture.manifest, BACKEND_IMAGE_SCANNER,
                                   read_image, &reader));

    fixture = valid_fixture(BACKEND_IMAGE_SCANNER);
    reader = fixture_reader(&fixture, 511U);
    fixture.manifest.crc32 ^= UINT32_C(1);
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_CRC_MISMATCH,
        backend_ota_image_validate(&fixture.manifest, BACKEND_IMAGE_SCANNER,
                                   read_image, &reader));

    /* Forge four payload bytes so this complete, structured image has CRC 0. */
    fixture = valid_fixture(BACKEND_IMAGE_SCANNER);
    const size_t patch_offset = 900U;
    memset(fixture.bytes + patch_offset, 0, 4U);
    fixture_refresh_esp_checksum(&fixture);
    uint32_t base_crc = reference_crc32(fixture.bytes, fixture.length);
    uint32_t effects[32];
    for (unsigned bit = 0; bit < 32; ++bit) {
        fixture.bytes[patch_offset + bit / 8U] ^= (uint8_t)(1U << (bit % 8U));
        fixture_refresh_esp_checksum(&fixture);
        effects[bit] = reference_crc32(fixture.bytes, fixture.length) ^ base_crc;
        fixture.bytes[patch_offset + bit / 8U] ^= (uint8_t)(1U << (bit % 8U));
        fixture_refresh_esp_checksum(&fixture);
    }
    uint64_t rows[32];
    for (unsigned output_bit = 0; output_bit < 32; ++output_bit) {
        uint64_t row = (uint64_t)((base_crc >> output_bit) & 1U) << 32;
        for (unsigned input_bit = 0; input_bit < 32; ++input_bit) {
            row |= (uint64_t)((effects[input_bit] >> output_bit) & 1U)
                   << input_bit;
        }
        rows[output_bit] = row;
    }
    for (unsigned column = 0; column < 32; ++column) {
        unsigned pivot = column;
        while (pivot < 32 && ((rows[pivot] >> column) & 1U) == 0U) {
            ++pivot;
        }
        TEST_ASSERT_LESS_THAN_UINT32(32U, pivot);
        uint64_t swap = rows[column];
        rows[column] = rows[pivot];
        rows[pivot] = swap;
        for (unsigned row = 0; row < 32; ++row) {
            if (row != column && ((rows[row] >> column) & 1U) != 0U) {
                rows[row] ^= rows[column];
            }
        }
    }
    for (unsigned bit = 0; bit < 32; ++bit) {
        if (((rows[bit] >> 32) & 1U) != 0U) {
            fixture.bytes[patch_offset + bit / 8U] ^=
                (uint8_t)(1U << (bit % 8U));
        }
    }
    fixture_refresh_esp_checksum(&fixture);
    TEST_ASSERT_EQUAL_HEX32(0U, reference_crc32(fixture.bytes, fixture.length));
    fixture_refresh_digests(&fixture);
    TEST_ASSERT_EQUAL_HEX32(0U, fixture.manifest.crc32);
    reader = fixture_reader(&fixture, 1U);
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_OK, backend_ota_image_validate(
        &fixture.manifest, BACKEND_IMAGE_SCANNER, read_image, &reader));
    fixture.manifest.crc32 = 1U;
    TEST_ASSERT_EQUAL(BACKEND_OTA_IMAGE_CRC_MISMATCH,
        backend_ota_image_validate(&fixture.manifest, BACKEND_IMAGE_SCANNER,
                                   read_image, &reader));
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_backend_ota_manifest_rejects_cross_family_and_invalid_admission);
    BACKEND_RUN_TEST(
        test_backend_ota_metadata_requires_exact_unsigned_crc32_member);
    BACKEND_RUN_TEST(
        test_backend_ota_complete_image_accepts_fragmented_exact_reader);
    BACKEND_RUN_TEST(
        test_backend_ota_complete_image_rejects_read_failures_short_and_trailing);
    BACKEND_RUN_TEST(
        test_backend_ota_complete_image_rejects_malformed_esp_container);
    BACKEND_RUN_TEST(
        test_backend_ota_complete_image_requires_exact_descriptor_and_identity);
    BACKEND_RUN_TEST(
        test_backend_ota_same_version_recovery_still_validates_complete_image);
    BACKEND_RUN_TEST(
        test_backend_ota_complete_image_checks_whole_digest_and_crc_including_zero);
    return UNITY_END();
}
