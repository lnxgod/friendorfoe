#include "backend_ota_identity.h"

#include <stdio.h>
#include <string.h>

#include "backend_json_reader.h"
#include "firmware_image_contract.h"
#include "firmware_version_order.h"

#define BACKEND_OTA_METADATA_KEYS 11U
#define BACKEND_OTA_IMAGE_HEADER_SIZE 24U
#define BACKEND_OTA_SEGMENT_HEADER_SIZE 8U
#define BACKEND_OTA_MAX_SEGMENTS 16U
#define BACKEND_OTA_STREAM_CHUNK 511U
#define BACKEND_OTA_ESP32S3_CHIP_ID UINT16_C(0x0009)
#define BACKEND_OTA_ESP_CHECKSUM_INITIAL UINT8_C(0xEF)
#define BACKEND_OTA_APP_PREFIX_SIZE 144U
#define BACKEND_OTA_APP_DESCRIPTOR_BYTES 112U

typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t block[64];
    size_t block_size;
} backend_sha256_t;

typedef struct {
    backend_sha256_t sha256;
    uint32_t crc32;
    uint32_t magic_window;
    size_t bytes_seen;
    size_t identity_count;
    size_t identity_size;
    uint8_t identity[sizeof(backend_embedded_identity_record_t)];
} backend_image_stream_t;

static uint16_t read_u16_le(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t read_u32_le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint32_t read_u32_be(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static void write_u32_be(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static uint32_t rotate_right(uint32_t value, unsigned amount)
{
    return (value >> amount) | (value << (32U - amount));
}

static void sha256_transform(backend_sha256_t *sha)
{
    static const uint32_t constants[64] = {
        UINT32_C(0x428A2F98), UINT32_C(0x71374491),
        UINT32_C(0xB5C0FBCF), UINT32_C(0xE9B5DBA5),
        UINT32_C(0x3956C25B), UINT32_C(0x59F111F1),
        UINT32_C(0x923F82A4), UINT32_C(0xAB1C5ED5),
        UINT32_C(0xD807AA98), UINT32_C(0x12835B01),
        UINT32_C(0x243185BE), UINT32_C(0x550C7DC3),
        UINT32_C(0x72BE5D74), UINT32_C(0x80DEB1FE),
        UINT32_C(0x9BDC06A7), UINT32_C(0xC19BF174),
        UINT32_C(0xE49B69C1), UINT32_C(0xEFBE4786),
        UINT32_C(0x0FC19DC6), UINT32_C(0x240CA1CC),
        UINT32_C(0x2DE92C6F), UINT32_C(0x4A7484AA),
        UINT32_C(0x5CB0A9DC), UINT32_C(0x76F988DA),
        UINT32_C(0x983E5152), UINT32_C(0xA831C66D),
        UINT32_C(0xB00327C8), UINT32_C(0xBF597FC7),
        UINT32_C(0xC6E00BF3), UINT32_C(0xD5A79147),
        UINT32_C(0x06CA6351), UINT32_C(0x14292967),
        UINT32_C(0x27B70A85), UINT32_C(0x2E1B2138),
        UINT32_C(0x4D2C6DFC), UINT32_C(0x53380D13),
        UINT32_C(0x650A7354), UINT32_C(0x766A0ABB),
        UINT32_C(0x81C2C92E), UINT32_C(0x92722C85),
        UINT32_C(0xA2BFE8A1), UINT32_C(0xA81A664B),
        UINT32_C(0xC24B8B70), UINT32_C(0xC76C51A3),
        UINT32_C(0xD192E819), UINT32_C(0xD6990624),
        UINT32_C(0xF40E3585), UINT32_C(0x106AA070),
        UINT32_C(0x19A4C116), UINT32_C(0x1E376C08),
        UINT32_C(0x2748774C), UINT32_C(0x34B0BCB5),
        UINT32_C(0x391C0CB3), UINT32_C(0x4ED8AA4A),
        UINT32_C(0x5B9CCA4F), UINT32_C(0x682E6FF3),
        UINT32_C(0x748F82EE), UINT32_C(0x78A5636F),
        UINT32_C(0x84C87814), UINT32_C(0x8CC70208),
        UINT32_C(0x90BEFFFA), UINT32_C(0xA4506CEB),
        UINT32_C(0xBEF9A3F7), UINT32_C(0xC67178F2),
    };
    uint32_t words[64];
    for (size_t index = 0; index < 16; ++index) {
        words[index] = read_u32_be(sha->block + index * 4U);
    }
    for (size_t index = 16; index < 64; ++index) {
        uint32_t small0 = rotate_right(words[index - 15], 7) ^
                          rotate_right(words[index - 15], 18) ^
                          (words[index - 15] >> 3);
        uint32_t small1 = rotate_right(words[index - 2], 17) ^
                          rotate_right(words[index - 2], 19) ^
                          (words[index - 2] >> 10);
        words[index] = words[index - 16] + small0 + words[index - 7] + small1;
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
        uint32_t big1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
                        rotate_right(e, 25);
        uint32_t choose = (e & f) ^ (~e & g);
        uint32_t first = h + big1 + choose + constants[index] + words[index];
        uint32_t big0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
                        rotate_right(a, 22);
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

static void sha256_init(backend_sha256_t *sha)
{
    memset(sha, 0, sizeof(*sha));
    sha->state[0] = UINT32_C(0x6A09E667);
    sha->state[1] = UINT32_C(0xBB67AE85);
    sha->state[2] = UINT32_C(0x3C6EF372);
    sha->state[3] = UINT32_C(0xA54FF53A);
    sha->state[4] = UINT32_C(0x510E527F);
    sha->state[5] = UINT32_C(0x9B05688C);
    sha->state[6] = UINT32_C(0x1F83D9AB);
    sha->state[7] = UINT32_C(0x5BE0CD19);
}

static void sha256_update(
    backend_sha256_t *sha, const uint8_t *bytes, size_t length)
{
    sha->bit_count += (uint64_t)length * UINT64_C(8);
    while (length > 0) {
        size_t available = sizeof(sha->block) - sha->block_size;
        size_t take = length < available ? length : available;
        memcpy(sha->block + sha->block_size, bytes, take);
        sha->block_size += take;
        bytes += take;
        length -= take;
        if (sha->block_size == sizeof(sha->block)) {
            sha256_transform(sha);
            sha->block_size = 0;
        }
    }
}

static void sha256_finish(backend_sha256_t *sha, uint8_t output[32])
{
    sha->block[sha->block_size++] = UINT8_C(0x80);
    if (sha->block_size > 56U) {
        memset(sha->block + sha->block_size, 0,
               sizeof(sha->block) - sha->block_size);
        sha256_transform(sha);
        sha->block_size = 0;
    }
    memset(sha->block + sha->block_size, 0, 56U - sha->block_size);
    for (size_t index = 0; index < 8; ++index) {
        sha->block[63U - index] =
            (uint8_t)(sha->bit_count >> (index * 8U));
    }
    sha256_transform(sha);
    for (size_t index = 0; index < 8; ++index) {
        write_u32_be(output + index * 4U, sha->state[index]);
    }
}

bool backend_ota_sha256(
    const uint8_t *bytes, size_t length, uint8_t output[32])
{
    if (output == NULL || (bytes == NULL && length != 0U)) {
        return false;
    }
    backend_sha256_t sha;
    sha256_init(&sha);
    sha256_update(&sha, bytes, length);
    sha256_finish(&sha, output);
    return true;
}

static bool fixed_string_is_exact(
    const char *field, size_t capacity, const char *expected)
{
    if (field == NULL || expected == NULL) {
        return false;
    }
    const char *terminator = memchr(field, '\0', capacity);
    if (terminator == NULL) {
        return false;
    }
    size_t length = (size_t)(terminator - field);
    return length == strlen(expected) && memcmp(field, expected, length) == 0;
}

static bool zero_padded_bytes_are_exact(
    const uint8_t *field, size_t capacity, const char *expected)
{
    size_t expected_length = strlen(expected);
    if (field == NULL || expected_length >= capacity ||
        memcmp(field, expected, expected_length) != 0) {
        return false;
    }
    for (size_t index = expected_length; index < capacity; ++index) {
        if (field[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool manifest_identity_is_exact(
    const backend_ota_manifest_t *manifest,
    backend_image_kind_t expected_kind)
{
    const backend_firmware_identity_t *expected =
        backend_identity_for_image(expected_kind);
    return manifest != NULL && expected != NULL &&
           fixed_string_is_exact(
               manifest->target, sizeof(manifest->target), expected->target) &&
           fixed_string_is_exact(
               manifest->project, sizeof(manifest->project), expected->project) &&
           fixed_string_is_exact(
               manifest->hardware, sizeof(manifest->hardware), expected->hardware);
}

static bool manifest_version_is_bounded(const backend_ota_manifest_t *manifest)
{
    const char *terminator = manifest == NULL ? NULL :
        memchr(manifest->version, '\0', sizeof(manifest->version));
    return terminator != NULL && terminator != manifest->version;
}

static bool backend_version_is_canonical(const char *version)
{
    if (version == NULL) {
        return false;
    }
    const char *cursor = version;
    for (unsigned component = 0; component < 3U; ++component) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        do {
            ++cursor;
        } while (*cursor >= '0' && *cursor <= '9');
        if (component < 2U) {
            if (*cursor != '.') {
                return false;
            }
            ++cursor;
        }
    }
    return strcmp(cursor, "-backend") == 0;
}

static bool manifest_digest_is_valid(const backend_ota_manifest_t *manifest)
{
    return manifest != NULL &&
           fof_firmware_sha256_hex_is_valid(manifest->sha256);
}

backend_ota_admission_result_t backend_ota_manifest_admit(
    const backend_ota_manifest_t *manifest,
    backend_image_kind_t expected_kind,
    const char *running_version,
    size_t partition_capacity)
{
    if (manifest == NULL || running_version == NULL ||
        backend_identity_for_image(expected_kind) == NULL) {
        return BACKEND_OTA_REJECT_ARGUMENT;
    }
    if (!manifest_identity_is_exact(manifest, expected_kind)) {
        return BACKEND_OTA_REJECT_IDENTITY;
    }
    if (!manifest_version_is_bounded(manifest) ||
        !backend_version_is_canonical(manifest->version)) {
        return BACKEND_OTA_REJECT_VERSION;
    }
    fof_firmware_version_relation_t relation =
        fof_firmware_version_compare(manifest->version, running_version);
    if (relation != FOF_VERSION_NEWER &&
        !(relation == FOF_VERSION_EQUAL && manifest->allow_same_version)) {
        return BACKEND_OTA_REJECT_VERSION;
    }
    if (!manifest_digest_is_valid(manifest)) {
        return BACKEND_OTA_REJECT_DIGEST;
    }
    if (manifest->image_size == 0U) {
        return BACKEND_OTA_REJECT_SIZE;
    }
    if (manifest->generation == 0U) {
        return BACKEND_OTA_REJECT_GENERATION;
    }
    if ((size_t)manifest->image_size > partition_capacity) {
        return BACKEND_OTA_REJECT_CAPACITY;
    }
    return BACKEND_OTA_ADMIT;
}

static bool metadata_find(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    const char *name,
    size_t *value_index)
{
    return backend_json_object_find(
        json, tokens, token_count, 0U, name, value_index);
}

static bool metadata_copy(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    const char *name,
    char *output,
    size_t capacity)
{
    size_t index = 0;
    return metadata_find(json, tokens, token_count, name, &index) &&
           backend_json_copy_string(json, &tokens[index], output, capacity);
}

static bool metadata_u32(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    const char *name,
    uint32_t *output)
{
    size_t index = 0;
    uint64_t value = 0;
    if (!metadata_find(json, tokens, token_count, name, &index) ||
        !backend_json_get_u64(json, &tokens[index], &value) ||
        value > UINT32_MAX) {
        return false;
    }
    *output = (uint32_t)value;
    return true;
}

bool backend_ota_manifest_decode_metadata(
    const char *json,
    size_t length,
    uint32_t catalog_generation,
    bool allow_same_version,
    backend_ota_manifest_t *out)
{
    backend_json_token_t tokens[32];
    size_t token_count = 0;
    backend_ota_manifest_t decoded;
    char name[40];
    char description[128];
    char board[16];
    char download_url[96];
    char expected_download_url[96];

    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (json == NULL || out == NULL || catalog_generation == 0U ||
        length == 0U || length > 4096U ||
        backend_json_parse(json, length, tokens,
                           sizeof(tokens) / sizeof(tokens[0]),
                           &token_count) != BACKEND_JSON_OK ||
        token_count == 0U || tokens[0].kind != BACKEND_JSON_OBJECT ||
        tokens[0].child_count != BACKEND_OTA_METADATA_KEYS * 2U ||
        token_count != BACKEND_OTA_METADATA_KEYS * 2U + 1U) {
        return false;
    }

    memset(&decoded, 0, sizeof(decoded));
    if (!metadata_copy(json, tokens, token_count, "name",
                       name, sizeof(name)) ||
        !metadata_copy(json, tokens, token_count, "target",
                       decoded.target, sizeof(decoded.target)) ||
        !metadata_copy(json, tokens, token_count, "description",
                       description, sizeof(description)) ||
        !metadata_copy(json, tokens, token_count, "board",
                       board, sizeof(board)) ||
        !metadata_copy(json, tokens, token_count, "project",
                       decoded.project, sizeof(decoded.project)) ||
        !metadata_copy(json, tokens, token_count, "hardware",
                       decoded.hardware, sizeof(decoded.hardware)) ||
        !metadata_copy(json, tokens, token_count, "version",
                       decoded.version, sizeof(decoded.version)) ||
        !metadata_u32(json, tokens, token_count, "size",
                      &decoded.image_size) ||
        !metadata_copy(json, tokens, token_count, "sha256",
                       decoded.sha256, sizeof(decoded.sha256)) ||
        !metadata_u32(json, tokens, token_count, "crc32", &decoded.crc32) ||
        !metadata_copy(json, tokens, token_count, "download_url",
                       download_url, sizeof(download_url))) {
        return false;
    }

    decoded.generation = catalog_generation;
    decoded.allow_same_version = allow_same_version;
    backend_image_kind_t kind;
    if (strcmp(decoded.target, FOF_BACKEND_UPLINK_TARGET) == 0) {
        kind = BACKEND_IMAGE_UPLINK;
    } else if (strcmp(decoded.target, FOF_BACKEND_SCANNER_TARGET) == 0) {
        kind = BACKEND_IMAGE_SCANNER;
    } else {
        return false;
    }
    int path_length = snprintf(
        expected_download_url, sizeof(expected_download_url),
        "/nodes/firmware/download/%s", decoded.target);
    if (description[0] == '\0' || strcmp(board, "esp32s3") != 0 ||
        strcmp(name, decoded.target) != 0 || path_length <= 0 ||
        (size_t)path_length >= sizeof(expected_download_url) ||
        strcmp(download_url, expected_download_url) != 0 ||
        !manifest_identity_is_exact(&decoded, kind) ||
        !manifest_version_is_bounded(&decoded) ||
        !backend_version_is_canonical(decoded.version) ||
        fof_firmware_version_compare(decoded.version, decoded.version) !=
            FOF_VERSION_EQUAL ||
        decoded.image_size == 0U || !manifest_digest_is_valid(&decoded)) {
        return false;
    }
    *out = decoded;
    return true;
}

static void image_stream_init(backend_image_stream_t *stream)
{
    memset(stream, 0, sizeof(*stream));
    sha256_init(&stream->sha256);
    stream->crc32 = UINT32_C(0xFFFFFFFF);
}

static void image_stream_update(
    backend_image_stream_t *stream, const uint8_t *bytes, size_t length)
{
    sha256_update(&stream->sha256, bytes, length);
    for (size_t index = 0; index < length; ++index) {
        uint8_t byte = bytes[index];
        stream->crc32 ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(stream->crc32 & UINT32_C(1));
            stream->crc32 = (stream->crc32 >> 1) ^
                            (UINT32_C(0xEDB88320) & mask);
        }

        if (stream->identity_count == 1U && stream->identity_size >= 4U &&
            stream->identity_size < sizeof(stream->identity)) {
            stream->identity[stream->identity_size++] = byte;
        }
        stream->magic_window = (stream->magic_window >> 8) |
                               ((uint32_t)byte << 24);
        ++stream->bytes_seen;
        if (stream->bytes_seen >= 4U &&
            stream->magic_window == FOF_BACKEND_IDENTITY_MAGIC) {
            ++stream->identity_count;
            if (stream->identity_count == 1U) {
                stream->identity[0] =
                    (uint8_t)FOF_BACKEND_IDENTITY_MAGIC;
                stream->identity[1] =
                    (uint8_t)(FOF_BACKEND_IDENTITY_MAGIC >> 8);
                stream->identity[2] =
                    (uint8_t)(FOF_BACKEND_IDENTITY_MAGIC >> 16);
                stream->identity[3] =
                    (uint8_t)(FOF_BACKEND_IDENTITY_MAGIC >> 24);
                stream->identity_size = 4U;
            }
        }
    }
}

static bool read_and_stream(
    backend_ota_read_fn read_fn,
    void *read_context,
    size_t offset,
    uint8_t *buffer,
    size_t length,
    backend_image_stream_t *stream)
{
    if (length == 0U || !read_fn(read_context, offset, buffer, length)) {
        return false;
    }
    image_stream_update(stream, buffer, length);
    return true;
}

static bool hex_digest_matches(
    const uint8_t digest[32], const char expected[65])
{
    static const char digits[] = "0123456789abcdef";
    unsigned difference = 0;
    for (size_t index = 0; index < 32; ++index) {
        char high = expected[index * 2U];
        char low = expected[index * 2U + 1U];
        if (high >= 'A' && high <= 'F') {
            high = (char)(high - 'A' + 'a');
        }
        if (low >= 'A' && low <= 'F') {
            low = (char)(low - 'A' + 'a');
        }
        difference |= (unsigned)(high ^ digits[digest[index] >> 4]);
        difference |= (unsigned)(low ^ digits[digest[index] & 0x0FU]);
    }
    return difference == 0U;
}

static bool structured_identity_matches(
    const backend_image_stream_t *stream,
    const backend_ota_manifest_t *manifest,
    backend_image_kind_t expected_kind)
{
    const uint8_t *record = stream->identity;
    return stream->identity_count == 1U &&
           stream->identity_size == sizeof(stream->identity) &&
           read_u32_le(record) == FOF_BACKEND_IDENTITY_MAGIC &&
           read_u16_le(record + 4) == FOF_BACKEND_IDENTITY_SCHEMA &&
           read_u16_le(record + 6) == (uint16_t)expected_kind &&
           read_u32_le(record + 160) ==
               backend_identity_crc32(record, 160U) &&
           zero_padded_bytes_are_exact(
               record + 8, 40U, manifest->target) &&
           zero_padded_bytes_are_exact(
               record + 48, 40U, manifest->project) &&
           zero_padded_bytes_are_exact(
               record + 88, 40U, manifest->hardware) &&
           zero_padded_bytes_are_exact(
               record + 128, 32U, manifest->version);
}

backend_ota_image_result_t backend_ota_image_validate(
    const backend_ota_manifest_t *manifest,
    backend_image_kind_t expected_kind,
    backend_ota_read_fn read_fn,
    void *read_context)
{
    uint8_t header[BACKEND_OTA_IMAGE_HEADER_SIZE];
    uint8_t segment_header[BACKEND_OTA_SEGMENT_HEADER_SIZE];
    uint8_t buffer[BACKEND_OTA_STREAM_CHUNK];
    uint8_t prefix[BACKEND_OTA_APP_PREFIX_SIZE];
    uint8_t appended_digest[32];
    backend_image_stream_t stream;
    size_t offset = 0;
    uint8_t image_checksum = BACKEND_OTA_ESP_CHECKSUM_INITIAL;

    if (manifest == NULL || read_fn == NULL ||
        backend_identity_for_image(expected_kind) == NULL) {
        return BACKEND_OTA_IMAGE_FORMAT_ERROR;
    }
    if (!manifest_identity_is_exact(manifest, expected_kind)) {
        return BACKEND_OTA_IMAGE_IDENTITY_MISMATCH;
    }
    if (!manifest_version_is_bounded(manifest) ||
        !backend_version_is_canonical(manifest->version)) {
        return BACKEND_OTA_IMAGE_DESCRIPTOR_MISMATCH;
    }
    if (!manifest_digest_is_valid(manifest)) {
        return BACKEND_OTA_IMAGE_DIGEST_MISMATCH;
    }
    if (manifest->image_size == 0U) {
        return BACKEND_OTA_IMAGE_FORMAT_ERROR;
    }

    memset(prefix, 0, sizeof(prefix));
    image_stream_init(&stream);
    if ((size_t)manifest->image_size < sizeof(header) ||
        !read_and_stream(read_fn, read_context, offset, header,
                         sizeof(header), &stream)) {
        return BACKEND_OTA_IMAGE_READ_ERROR;
    }
    memcpy(prefix, header, sizeof(header));
    offset += sizeof(header);

    const uint8_t segment_count = header[1];
    const uint8_t spi_speed = header[3] & UINT8_C(0x0F);
    const uint8_t spi_size = header[3] >> 4;
    const uint8_t hash_appended = header[23];
    if (header[0] != UINT8_C(0xE9) || segment_count == 0U ||
        segment_count > BACKEND_OTA_MAX_SEGMENTS || header[2] > 5U ||
        (spi_speed > 2U && spi_speed != UINT8_C(0x0F)) || spi_size > 7U ||
        read_u16_le(header + 12) != BACKEND_OTA_ESP32S3_CHIP_ID ||
        hash_appended > 1U) {
        return BACKEND_OTA_IMAGE_FORMAT_ERROR;
    }

    for (uint8_t segment = 0; segment < segment_count; ++segment) {
        if (offset > manifest->image_size ||
            sizeof(segment_header) > (size_t)manifest->image_size - offset) {
            return BACKEND_OTA_IMAGE_FORMAT_ERROR;
        }
        if (!read_and_stream(read_fn, read_context, offset, segment_header,
                             sizeof(segment_header), &stream)) {
            return BACKEND_OTA_IMAGE_READ_ERROR;
        }
        if (segment == 0U) {
            memcpy(prefix + BACKEND_OTA_IMAGE_HEADER_SIZE,
                   segment_header, sizeof(segment_header));
        }
        offset += sizeof(segment_header);
        const uint32_t load_address = read_u32_le(segment_header);
        const uint32_t data_length = read_u32_le(segment_header + 4);
        if ((data_length & UINT32_C(3)) != 0U ||
            (data_length != 0U &&
             load_address > UINT32_MAX - data_length) ||
            (size_t)data_length > (size_t)manifest->image_size - offset ||
            (segment == 0U && data_length < BACKEND_OTA_APP_DESCRIPTOR_BYTES)) {
            return BACKEND_OTA_IMAGE_FORMAT_ERROR;
        }

        size_t segment_position = 0;
        while (segment_position < data_length) {
            size_t amount = (size_t)data_length - segment_position;
            if (amount > sizeof(buffer)) {
                amount = sizeof(buffer);
            }
            if (!read_and_stream(read_fn, read_context,
                                 offset + segment_position,
                                 buffer, amount, &stream)) {
                return BACKEND_OTA_IMAGE_READ_ERROR;
            }
            if (segment == 0U &&
                segment_position < BACKEND_OTA_APP_DESCRIPTOR_BYTES) {
                size_t prefix_amount = amount;
                if (prefix_amount > BACKEND_OTA_APP_DESCRIPTOR_BYTES -
                                        segment_position) {
                    prefix_amount = BACKEND_OTA_APP_DESCRIPTOR_BYTES -
                                    segment_position;
                }
                memcpy(prefix + BACKEND_OTA_IMAGE_HEADER_SIZE +
                           BACKEND_OTA_SEGMENT_HEADER_SIZE + segment_position,
                       buffer, prefix_amount);
            }
            for (size_t index = 0; index < amount; ++index) {
                image_checksum ^= buffer[index];
            }
            segment_position += amount;
        }
        offset += data_length;
    }

    if (offset >= manifest->image_size || offset > SIZE_MAX - 16U) {
        return BACKEND_OTA_IMAGE_FORMAT_ERROR;
    }
    size_t checksum_end = (offset + 16U) & ~(size_t)15U;
    if (checksum_end <= offset || checksum_end > manifest->image_size ||
        checksum_end - offset > 16U) {
        return BACKEND_OTA_IMAGE_FORMAT_ERROR;
    }
    size_t trailer_length = checksum_end - offset;
    if (!read_and_stream(read_fn, read_context, offset, buffer,
                         trailer_length, &stream)) {
        return BACKEND_OTA_IMAGE_READ_ERROR;
    }
    if (buffer[trailer_length - 1U] != image_checksum) {
        return BACKEND_OTA_IMAGE_FORMAT_ERROR;
    }
    offset = checksum_end;

    if (hash_appended != 0U) {
        backend_sha256_t internal_sha = stream.sha256;
        uint8_t expected_internal[32];
        sha256_finish(&internal_sha, expected_internal);
        if (sizeof(appended_digest) >
                (size_t)manifest->image_size - offset) {
            return BACKEND_OTA_IMAGE_FORMAT_ERROR;
        }
        if (!read_and_stream(read_fn, read_context, offset, appended_digest,
                             sizeof(appended_digest), &stream)) {
            return BACKEND_OTA_IMAGE_READ_ERROR;
        }
        if (memcmp(appended_digest, expected_internal,
                   sizeof(appended_digest)) != 0) {
            return BACKEND_OTA_IMAGE_FORMAT_ERROR;
        }
        offset += sizeof(appended_digest);
    }

    if (offset != manifest->image_size) {
        return BACKEND_OTA_IMAGE_FORMAT_ERROR;
    }
    uint8_t trailing_byte = 0;
    if (read_fn(read_context, offset, &trailing_byte, 1U)) {
        return BACKEND_OTA_IMAGE_FORMAT_ERROR;
    }

    fof_firmware_image_identity_t descriptor;
    if (!fof_firmware_image_parse_identity(prefix, sizeof(prefix),
                                            &descriptor) ||
        strcmp(descriptor.project, manifest->project) != 0 ||
        strcmp(descriptor.version, manifest->version) != 0) {
        return BACKEND_OTA_IMAGE_DESCRIPTOR_MISMATCH;
    }
    if (!structured_identity_matches(
            &stream, manifest, expected_kind)) {
        return BACKEND_OTA_IMAGE_IDENTITY_MISMATCH;
    }

    uint8_t digest[32];
    sha256_finish(&stream.sha256, digest);
    if (!hex_digest_matches(digest, manifest->sha256)) {
        return BACKEND_OTA_IMAGE_DIGEST_MISMATCH;
    }
    if (~stream.crc32 != manifest->crc32) {
        return BACKEND_OTA_IMAGE_CRC_MISMATCH;
    }
    return BACKEND_OTA_IMAGE_OK;
}
