#include "backend_firmware_store.h"

#include <limits.h>
#include <string.h>

#include "backend_identity.h"

typedef struct {
    const backend_firmware_store_t *store;
    size_t image_size;
} partition_reader_t;

static bool fixed_string_is_bounded(
    const char *value, size_t capacity, bool allow_empty)
{
    if (value == NULL) {
        return false;
    }
    const char *end = memchr(value, '\0', capacity);
    return end != NULL && (allow_empty || end != value);
}

static bool fixed_string_equals(
    const char *left, size_t left_capacity,
    const char *right, size_t right_capacity)
{
    const char *left_end = left == NULL ? NULL :
        memchr(left, '\0', left_capacity);
    const char *right_end = right == NULL ? NULL :
        memchr(right, '\0', right_capacity);
    if (left_end == NULL || right_end == NULL) {
        return false;
    }
    size_t left_length = (size_t)(left_end - left);
    size_t right_length = (size_t)(right_end - right);
    return left_length == right_length &&
           memcmp(left, right, left_length) == 0;
}

static bool fixed_string_equals_literal(
    const char *value, size_t capacity, const char *expected)
{
    if (value == NULL || expected == NULL) {
        return false;
    }
    const char *end = memchr(value, '\0', capacity);
    size_t expected_length = strlen(expected);
    return end != NULL && (size_t)(end - value) == expected_length &&
           memcmp(value, expected, expected_length) == 0;
}

static bool sha256_is_bounded_hex(const char sha256[65])
{
    if (sha256 == NULL || sha256[64] != '\0') {
        return false;
    }
    for (size_t index = 0U; index < 64U; ++index) {
        char value = sha256[index];
        bool digit = value >= '0' && value <= '9';
        bool lower = value >= 'a' && value <= 'f';
        bool upper = value >= 'A' && value <= 'F';
        if (!digit && !lower && !upper) {
            return false;
        }
    }
    return true;
}

static bool manifest_is_backend_scanner(
    const backend_ota_manifest_t *manifest)
{
    return manifest != NULL &&
           fixed_string_equals_literal(
               manifest->target, sizeof(manifest->target),
               FOF_BACKEND_SCANNER_TARGET) &&
           fixed_string_equals_literal(
               manifest->project, sizeof(manifest->project),
               FOF_BACKEND_SCANNER_PROJECT) &&
           fixed_string_equals_literal(
               manifest->hardware, sizeof(manifest->hardware),
               FOF_BACKEND_HARDWARE) &&
           fixed_string_is_bounded(
               manifest->version, sizeof(manifest->version), false) &&
           sha256_is_bounded_hex(manifest->sha256) &&
           manifest->generation != 0U;
}

static bool manifests_equal(
    const backend_ota_manifest_t *left,
    const backend_ota_manifest_t *right)
{
    return left != NULL && right != NULL &&
           fixed_string_equals(left->target, sizeof(left->target),
                               right->target, sizeof(right->target)) &&
           fixed_string_equals(left->project, sizeof(left->project),
                               right->project, sizeof(right->project)) &&
           fixed_string_equals(left->hardware, sizeof(left->hardware),
                               right->hardware, sizeof(right->hardware)) &&
           fixed_string_equals(left->version, sizeof(left->version),
                               right->version, sizeof(right->version)) &&
           fixed_string_equals(left->sha256, sizeof(left->sha256),
                               right->sha256, sizeof(right->sha256)) &&
           left->image_size == right->image_size &&
           left->crc32 == right->crc32 &&
           left->generation == right->generation &&
           left->allow_same_version == right->allow_same_version;
}

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
static bool store_operation_matches(
    const backend_firmware_store_t *store,
    bool has_operation_id,
    const backend_ota_operation_id_t *operation_id)
{
    return store != NULL && store->has_operation_id && has_operation_id &&
           operation_id != NULL && backend_ota_operation_id_equal(
               &store->operation_id, operation_id);
}
#endif

static bool read_persisted_image(
    void *context, size_t offset, uint8_t *output, size_t length)
{
    partition_reader_t *reader = context;
    if (reader == NULL || reader->store == NULL || output == NULL ||
        length == 0U || offset > reader->image_size ||
        length > reader->image_size - offset ||
        reader->store->partition.read == NULL) {
        return false;
    }
    return reader->store->partition.read(
        reader->store->partition.context,
        BACKEND_FIRMWARE_STORE_PARTITION_LABEL,
        offset, output, length);
}

void backend_firmware_store_init(
    backend_firmware_store_t *store,
    const backend_firmware_store_partition_t *partition)
{
    if (store == NULL) {
        return;
    }
    memset(store, 0, sizeof(*store));
    if (partition != NULL) {
        store->partition = *partition;
    }
}

backend_firmware_store_result_t backend_firmware_store_stage(
    backend_firmware_store_t *store,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    bool has_operation_id,
    const backend_ota_operation_id_t *operation_id,
#endif
    const backend_ota_manifest_t *manifest,
    backend_ota_read_fn source_read,
    void *source_context,
    bool persist)
{
    if (store == NULL || manifest == NULL || source_read == NULL
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        || !has_operation_id || operation_id == NULL
#endif
        ) {
        return BACKEND_FIRMWARE_STORE_INVALID_ARGUMENT;
    }
    if (store->available || store->relay_claimed) {
        return BACKEND_FIRMWARE_STORE_BUSY;
    }
    const backend_ota_manifest_t admitted = *manifest;
    if (!manifest_is_backend_scanner(&admitted)) {
        return BACKEND_FIRMWARE_STORE_REJECT_IDENTITY;
    }
    if (admitted.image_size == 0U ||
        admitted.image_size > FOF_BACKEND_SCANNER_CACHE_CAPACITY) {
        return BACKEND_FIRMWARE_STORE_REJECT_CAPACITY;
    }
    if (persist && (store->partition.erase == NULL ||
                    store->partition.write == NULL ||
                    store->partition.read == NULL)) {
        return BACKEND_FIRMWARE_STORE_INVALID_ARGUMENT;
    }

    store->last_image_result = backend_ota_image_validate(
        &admitted, BACKEND_IMAGE_SCANNER, source_read, source_context);
    if (store->last_image_result != BACKEND_OTA_IMAGE_OK) {
        return BACKEND_FIRMWARE_STORE_IMAGE_INVALID;
    }

    if (!persist) {
        store->manifest = admitted;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        store->operation_id = *operation_id;
        store->has_operation_id = true;
#endif
        store->source_read = source_read;
        store->source_context = source_context;
        store->persisted = false;
        store->available = true;
        return BACKEND_FIRMWARE_STORE_OK;
    }

    ++store->erase_count;
    if (!store->partition.erase(
            store->partition.context,
            BACKEND_FIRMWARE_STORE_PARTITION_LABEL,
            FOF_BACKEND_SCANNER_CACHE_CAPACITY)) {
        return BACKEND_FIRMWARE_STORE_ERASE_FAILED;
    }

    uint8_t buffer[BACKEND_FIRMWARE_STORE_COPY_CHUNK];
    size_t offset = 0U;
    while (offset < admitted.image_size) {
        size_t remaining = (size_t)admitted.image_size - offset;
        size_t length = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        if (!source_read(source_context, offset, buffer, length)) {
            return BACKEND_FIRMWARE_STORE_SOURCE_READ_FAILED;
        }
        ++store->write_count;
        if (!store->partition.write(
                store->partition.context,
                BACKEND_FIRMWARE_STORE_PARTITION_LABEL,
                offset, buffer, length)) {
            return BACKEND_FIRMWARE_STORE_WRITE_FAILED;
        }
        offset += length;
    }

    store->manifest = admitted;
    partition_reader_t reader = {
        .store = store,
        .image_size = admitted.image_size,
    };
    store->last_image_result = backend_ota_image_validate(
        &store->manifest, BACKEND_IMAGE_SCANNER,
        read_persisted_image, &reader);
    if (store->last_image_result != BACKEND_OTA_IMAGE_OK) {
        memset(&store->manifest, 0, sizeof(store->manifest));
        return BACKEND_FIRMWARE_STORE_VERIFY_FAILED;
    }

    store->source_read = NULL;
    store->source_context = NULL;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    store->operation_id = *operation_id;
    store->has_operation_id = true;
#endif
    store->persisted = true;
    store->available = true;
    return BACKEND_FIRMWARE_STORE_OK;
}

bool backend_firmware_store_matches(
    const backend_firmware_store_t *store,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    bool has_operation_id,
    const backend_ota_operation_id_t *operation_id,
#endif
    const backend_ota_manifest_t *manifest)
{
    return store != NULL && store->available &&
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
           store_operation_matches(store, has_operation_id, operation_id) &&
#endif
           manifests_equal(&store->manifest, manifest);
}

bool backend_firmware_store_read(
    const backend_firmware_store_t *store,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    bool has_operation_id,
    const backend_ota_operation_id_t *operation_id,
#endif
    uint32_t generation,
    size_t offset,
    uint8_t *output,
    size_t length)
{
    if (store == NULL || !store->available || output == NULL || length == 0U ||
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        !store_operation_matches(store, has_operation_id, operation_id) ||
#endif
        generation == 0U || generation != store->manifest.generation ||
        offset > store->manifest.image_size ||
        length > (size_t)store->manifest.image_size - offset) {
        return false;
    }
    if (store->persisted) {
        return store->partition.read != NULL && store->partition.read(
            store->partition.context,
            BACKEND_FIRMWARE_STORE_PARTITION_LABEL,
            offset, output, length);
    }
    return store->source_read != NULL && store->source_read(
        store->source_context, offset, output, length);
}

bool backend_firmware_store_claim_relay(
    backend_firmware_store_t *store,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    bool has_operation_id,
    const backend_ota_operation_id_t *operation_id,
#endif
    uint32_t generation,
    uint32_t session_id)
{
    if (store == NULL || !store->available || store->relay_claimed ||
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        !store_operation_matches(store, has_operation_id, operation_id) ||
#endif
        generation == 0U || generation != store->manifest.generation ||
        session_id == 0U) {
        return false;
    }
    store->relay_claimed = true;
    store->relay_session_id = session_id;
    return true;
}

bool backend_firmware_store_relay_claim_matches(
    const backend_firmware_store_t *store,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    bool has_operation_id,
    const backend_ota_operation_id_t *operation_id,
#endif
    uint32_t generation,
    uint32_t session_id)
{
    return store != NULL && store->available && store->relay_claimed &&
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
           store_operation_matches(store, has_operation_id, operation_id) &&
#endif
           generation != 0U && generation == store->manifest.generation &&
           session_id != 0U && session_id == store->relay_session_id;
}

void backend_firmware_store_release_relay(
    backend_firmware_store_t *store,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    bool has_operation_id,
    const backend_ota_operation_id_t *operation_id,
#endif
    uint32_t generation,
    uint32_t session_id)
{
    if (backend_firmware_store_relay_claim_matches(
            store,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
            has_operation_id, operation_id,
#endif
            generation, session_id)) {
        store->relay_claimed = false;
        store->relay_session_id = 0U;
    }
}

bool backend_firmware_store_discard(
    backend_firmware_store_t *store,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    bool has_operation_id,
    const backend_ota_operation_id_t *operation_id,
#endif
    uint32_t generation)
{
    if (store == NULL || !store->available || store->relay_claimed ||
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        !store_operation_matches(store, has_operation_id, operation_id) ||
#endif
        generation == 0U || generation != store->manifest.generation) {
        return false;
    }
    memset(&store->manifest, 0, sizeof(store->manifest));
    store->source_read = NULL;
    store->source_context = NULL;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    memset(&store->operation_id, 0, sizeof(store->operation_id));
    store->has_operation_id = false;
#endif
    store->available = false;
    store->persisted = false;
    return true;
}

uint32_t backend_firmware_store_image_mutation_count(
    const backend_firmware_store_t *store)
{
    if (store == NULL) {
        return 0U;
    }
    if (UINT32_MAX - store->erase_count < store->write_count) {
        return UINT32_MAX;
    }
    return store->erase_count + store->write_count;
}
