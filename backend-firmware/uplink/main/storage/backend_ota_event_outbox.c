#include "backend_ota_event_outbox.h"

#include <string.h>

#include "backend_identity.h"
#include "backend_ota_identity.h"

#define OUTBOX_MAGIC UINT32_C(0x5842544f)
#define OUTBOX_SCHEMA UINT16_C(1)
#define OUTBOX_HEADER_BYTES 66U
#define OUTBOX_CRC_BYTES 4U

typedef enum {
    SLOT_MISSING = 0,
    SLOT_VALID,
    SLOT_INVALID,
    SLOT_IO_ERROR,
} slot_load_result_t;

typedef struct {
    bool present;
    backend_ota_event_outbox_slot_t slot;
    backend_ota_event_outbox_record_t record;
} newest_record_t;

static void write_u16_le(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8U);
}

static void write_u32_le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8U);
    out[2] = (uint8_t)(value >> 16U);
    out[3] = (uint8_t)(value >> 24U);
}

static uint16_t read_u16_le(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8U);
}

static uint32_t read_u32_le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

static bool bytes_equal_constant_time(
    const uint8_t *left, const uint8_t *right, size_t length)
{
    if (left == NULL || right == NULL) {
        return false;
    }
    uint8_t different = 0U;
    for (size_t index = 0U; index < length; ++index) {
        different |= (uint8_t)(left[index] ^ right[index]);
    }
    return different == 0U;
}

static bool event_digest_is_exact(const backend_ota_pending_event_t *event)
{
    uint8_t digest[32] = {0U};
    return event != NULL && event->body_length > 0U &&
           event->body_length <= BACKEND_OTA_EVENT_MAX_BYTES &&
           backend_ota_sha256(event->body, event->body_length, digest) &&
           bytes_equal_constant_time(
               digest, event->body_sha256, sizeof(digest));
}

static bool record_is_valid(const backend_ota_event_outbox_record_t *record)
{
    if (record == NULL || record->generation == 0U ||
        (record->state != BACKEND_OTA_EVENT_OUTBOX_STATE_PENDING &&
         record->state != BACKEND_OTA_EVENT_OUTBOX_STATE_TOMBSTONE)) {
        return false;
    }
    if (record->state == BACKEND_OTA_EVENT_OUTBOX_STATE_PENDING) {
        return event_digest_is_exact(&record->event);
    }
    return record->event.body_length == 0U;
}

bool backend_ota_event_outbox_slot_encode(
    const backend_ota_event_outbox_record_t *record,
    backend_ota_event_outbox_blob_t *out)
{
    if (record == NULL || out == NULL || !record_is_valid(record)) {
        return false;
    }

    backend_ota_event_outbox_blob_t encoded;
    memset(&encoded, 0, sizeof(encoded));
    const size_t body_length = record->event.body_length;
    encoded.length = OUTBOX_HEADER_BYTES + body_length + OUTBOX_CRC_BYTES;

    write_u32_le(encoded.bytes, OUTBOX_MAGIC);
    write_u16_le(encoded.bytes + 4U, OUTBOX_SCHEMA);
    encoded.bytes[6U] = (uint8_t)record->state;
    encoded.bytes[7U] = 1U;
    memcpy(encoded.bytes + 8U, record->event.operation_id.bytes, 16U);
    write_u32_le(encoded.bytes + 24U, record->event.sequence);
    write_u32_le(encoded.bytes + 28U, record->generation);
    write_u16_le(encoded.bytes + 32U, record->event.body_length);
    memcpy(encoded.bytes + 34U, record->event.body_sha256, 32U);
    if (body_length != 0U) {
        memcpy(encoded.bytes + OUTBOX_HEADER_BYTES,
               record->event.body, body_length);
    }
    write_u32_le(
        encoded.bytes + encoded.length - OUTBOX_CRC_BYTES,
        backend_identity_crc32(
            encoded.bytes, encoded.length - OUTBOX_CRC_BYTES));
    *out = encoded;
    return true;
}

bool backend_ota_event_outbox_slot_decode(
    const uint8_t *bytes,
    size_t length,
    backend_ota_event_outbox_record_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (bytes == NULL || out == NULL ||
        length < OUTBOX_HEADER_BYTES + OUTBOX_CRC_BYTES ||
        length > BACKEND_OTA_EVENT_OUTBOX_SLOT_MAX_BYTES ||
        read_u32_le(bytes) != OUTBOX_MAGIC ||
        read_u16_le(bytes + 4U) != OUTBOX_SCHEMA || bytes[7U] != 1U) {
        return false;
    }
    const uint16_t body_length = read_u16_le(bytes + 32U);
    if (body_length > BACKEND_OTA_EVENT_MAX_BYTES ||
        length != OUTBOX_HEADER_BYTES + (size_t)body_length + OUTBOX_CRC_BYTES ||
        read_u32_le(bytes + length - OUTBOX_CRC_BYTES) !=
            backend_identity_crc32(bytes, length - OUTBOX_CRC_BYTES)) {
        return false;
    }

    backend_ota_event_outbox_record_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    decoded.state = (backend_ota_event_outbox_state_t)bytes[6U];
    memcpy(decoded.event.operation_id.bytes, bytes + 8U, 16U);
    decoded.event.sequence = read_u32_le(bytes + 24U);
    decoded.generation = read_u32_le(bytes + 28U);
    decoded.event.body_length = body_length;
    memcpy(decoded.event.body_sha256, bytes + 34U, 32U);
    if (body_length != 0U) {
        memcpy(decoded.event.body, bytes + OUTBOX_HEADER_BYTES, body_length);
    }
    if (!record_is_valid(&decoded)) {
        return false;
    }
    *out = decoded;
    return true;
}

static slot_load_result_t load_one(
    const backend_ota_event_outbox_storage_t *storage,
    backend_ota_event_outbox_slot_t slot,
    backend_ota_event_outbox_record_t *out)
{
    backend_ota_event_outbox_blob_t blob;
    size_t length = 0U;
    const backend_ota_event_outbox_io_result_t result = storage->load_slot(
        storage->context, slot, blob.bytes, sizeof(blob.bytes), &length);
    if (result == BACKEND_OTA_EVENT_OUTBOX_IO_NOT_FOUND) {
        return SLOT_MISSING;
    }
    if (result != BACKEND_OTA_EVENT_OUTBOX_IO_OK) {
        return SLOT_IO_ERROR;
    }
    return backend_ota_event_outbox_slot_decode(blob.bytes, length, out)
        ? SLOT_VALID : SLOT_INVALID;
}

static backend_ota_event_outbox_load_result_t load_newest(
    const backend_ota_event_outbox_storage_t *storage,
    newest_record_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (storage == NULL || storage->load_slot == NULL || out == NULL) {
        return BACKEND_OTA_EVENT_OUTBOX_LOAD_IO_ERROR;
    }
    backend_ota_event_outbox_record_t records[2];
    memset(records, 0, sizeof(records));
    const slot_load_result_t first = load_one(
        storage, BACKEND_OTA_EVENT_OUTBOX_SLOT_0, &records[0]);
    const slot_load_result_t second = load_one(
        storage, BACKEND_OTA_EVENT_OUTBOX_SLOT_1, &records[1]);
    if (first == SLOT_IO_ERROR || second == SLOT_IO_ERROR) {
        return BACKEND_OTA_EVENT_OUTBOX_LOAD_IO_ERROR;
    }
    /* Never guess past a nonempty malformed slot: it may be the torn newest
     * generation. */
    if (first == SLOT_INVALID || second == SLOT_INVALID) {
        return BACKEND_OTA_EVENT_OUTBOX_LOAD_CORRUPT;
    }
    if (first == SLOT_MISSING && second == SLOT_MISSING) {
        return BACKEND_OTA_EVENT_OUTBOX_LOAD_EMPTY;
    }
    size_t newest = first == SLOT_VALID ? 0U : 1U;
    if (first == SLOT_VALID && second == SLOT_VALID) {
        const uint32_t a = records[0].generation;
        const uint32_t b = records[1].generation;
        if (a == b ||
            !((a < UINT32_MAX && b == a + 1U) ||
              (b < UINT32_MAX && a == b + 1U))) {
            return BACKEND_OTA_EVENT_OUTBOX_LOAD_CORRUPT;
        }
        newest = b > a ? 1U : 0U;
    }
    out->present = true;
    out->slot = newest == 0U
        ? BACKEND_OTA_EVENT_OUTBOX_SLOT_0 : BACKEND_OTA_EVENT_OUTBOX_SLOT_1;
    out->record = records[newest];
    return out->record.state == BACKEND_OTA_EVENT_OUTBOX_STATE_PENDING
        ? BACKEND_OTA_EVENT_OUTBOX_LOAD_PENDING
        : BACKEND_OTA_EVENT_OUTBOX_LOAD_EMPTY;
}

backend_ota_event_outbox_load_result_t backend_ota_event_outbox_load(
    const backend_ota_event_outbox_storage_t *storage,
    backend_ota_event_outbox_snapshot_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    newest_record_t newest;
    const backend_ota_event_outbox_load_result_t result =
        load_newest(storage, &newest);
    if ((result == BACKEND_OTA_EVENT_OUTBOX_LOAD_PENDING ||
         (result == BACKEND_OTA_EVENT_OUTBOX_LOAD_EMPTY && newest.present)) &&
        out != NULL) {
        out->newest_slot = newest.slot;
        out->generation = newest.record.generation;
        out->pending = newest.record.event;
    } else if (result == BACKEND_OTA_EVENT_OUTBOX_LOAD_PENDING && out == NULL) {
        return BACKEND_OTA_EVENT_OUTBOX_LOAD_IO_ERROR;
    }
    return result;
}

static bool pending_equal(
    const backend_ota_pending_event_t *left,
    const backend_ota_pending_event_t *right)
{
    return left != NULL && right != NULL &&
           backend_ota_operation_id_equal(
               &left->operation_id, &right->operation_id) &&
           left->sequence == right->sequence &&
           left->body_length == right->body_length &&
           bytes_equal_constant_time(
               left->body_sha256, right->body_sha256,
               sizeof(left->body_sha256)) &&
           memcmp(left->body, right->body, left->body_length) == 0;
}

static bool store_record(
    const backend_ota_event_outbox_storage_t *storage,
    backend_ota_event_outbox_slot_t slot,
    const backend_ota_event_outbox_record_t *record)
{
    backend_ota_event_outbox_blob_t blob;
    return storage != NULL && storage->store_slot != NULL &&
           backend_ota_event_outbox_slot_encode(record, &blob) &&
           storage->store_slot(storage->context, slot, blob.bytes, blob.length);
}

backend_ota_event_outbox_enqueue_result_t backend_ota_event_outbox_enqueue(
    const backend_ota_event_outbox_storage_t *storage,
    const backend_ota_operation_id_t *operation_id,
    uint32_t sequence,
    const uint8_t *body,
    size_t body_length)
{
    if (storage == NULL || storage->load_slot == NULL ||
        storage->store_slot == NULL || operation_id == NULL || body == NULL ||
        body_length == 0U || body_length > BACKEND_OTA_EVENT_MAX_BYTES) {
        return BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_INVALID;
    }
    backend_ota_event_outbox_record_t candidate;
    memset(&candidate, 0, sizeof(candidate));
    candidate.state = BACKEND_OTA_EVENT_OUTBOX_STATE_PENDING;
    candidate.event.operation_id = *operation_id;
    candidate.event.sequence = sequence;
    candidate.event.body_length = (uint16_t)body_length;
    memcpy(candidate.event.body, body, body_length);
    if (!backend_ota_sha256(
            body, body_length, candidate.event.body_sha256)) {
        return BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_INVALID;
    }

    newest_record_t newest;
    const backend_ota_event_outbox_load_result_t loaded =
        load_newest(storage, &newest);
    if (loaded == BACKEND_OTA_EVENT_OUTBOX_LOAD_CORRUPT) {
        return BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_CORRUPT;
    }
    if (loaded == BACKEND_OTA_EVENT_OUTBOX_LOAD_IO_ERROR) {
        return BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_IO_ERROR;
    }
    if (loaded == BACKEND_OTA_EVENT_OUTBOX_LOAD_PENDING) {
        return pending_equal(&candidate.event, &newest.record.event)
            ? BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_ALREADY_PENDING
            : BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_BUSY;
    }
    if (newest.present && newest.record.generation == UINT32_MAX) {
        return BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_CORRUPT;
    }
    candidate.generation = newest.present
        ? newest.record.generation + 1U : 1U;
    const backend_ota_event_outbox_slot_t target = !newest.present ||
            newest.slot == BACKEND_OTA_EVENT_OUTBOX_SLOT_1
        ? BACKEND_OTA_EVENT_OUTBOX_SLOT_0
        : BACKEND_OTA_EVENT_OUTBOX_SLOT_1;
    return store_record(storage, target, &candidate)
        ? BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_COMMITTED
        : BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_IO_ERROR;
}

backend_ota_event_outbox_ack_result_t backend_ota_event_outbox_acknowledge(
    const backend_ota_event_outbox_storage_t *storage,
    const backend_ota_event_outbox_ack_t *ack)
{
    if (storage == NULL || storage->store_slot == NULL || ack == NULL ||
        !ack->strict_decoded || ack->http_status < 200U ||
        ack->http_status >= 300U || !ack->ok || !ack->has_operation_id) {
        return BACKEND_OTA_EVENT_OUTBOX_ACK_INVALID;
    }
    newest_record_t newest;
    const backend_ota_event_outbox_load_result_t loaded =
        load_newest(storage, &newest);
    if (loaded == BACKEND_OTA_EVENT_OUTBOX_LOAD_CORRUPT) {
        return BACKEND_OTA_EVENT_OUTBOX_ACK_CORRUPT;
    }
    if (loaded == BACKEND_OTA_EVENT_OUTBOX_LOAD_IO_ERROR) {
        return BACKEND_OTA_EVENT_OUTBOX_ACK_IO_ERROR;
    }
    if (loaded != BACKEND_OTA_EVENT_OUTBOX_LOAD_PENDING) {
        return BACKEND_OTA_EVENT_OUTBOX_ACK_NO_PENDING;
    }
    const backend_ota_pending_event_t *pending = &newest.record.event;
    if (pending->sequence == UINT32_MAX ||
        !backend_ota_operation_id_equal(
            &pending->operation_id, &ack->operation_id) ||
        ack->accepted_sequence != pending->sequence ||
        ack->next_sequence != pending->sequence + 1U) {
        return BACKEND_OTA_EVENT_OUTBOX_ACK_MISMATCH;
    }

    backend_ota_event_outbox_record_t tombstone;
    memset(&tombstone, 0, sizeof(tombstone));
    tombstone.state = BACKEND_OTA_EVENT_OUTBOX_STATE_TOMBSTONE;
    tombstone.generation = newest.record.generation + 1U;
    if (tombstone.generation == 0U) {
        return BACKEND_OTA_EVENT_OUTBOX_ACK_CORRUPT;
    }
    tombstone.event.operation_id = pending->operation_id;
    tombstone.event.sequence = pending->sequence;
    memcpy(tombstone.event.body_sha256, pending->body_sha256,
           sizeof(tombstone.event.body_sha256));
    const backend_ota_event_outbox_slot_t target =
        newest.slot == BACKEND_OTA_EVENT_OUTBOX_SLOT_0
        ? BACKEND_OTA_EVENT_OUTBOX_SLOT_1
        : BACKEND_OTA_EVENT_OUTBOX_SLOT_0;
    return store_record(storage, target, &tombstone)
        ? BACKEND_OTA_EVENT_OUTBOX_ACK_CLEARED
        : BACKEND_OTA_EVENT_OUTBOX_ACK_IO_ERROR;
}

bool backend_ota_event_outbox_recover_exact_slots(
    const backend_ota_event_outbox_storage_t *storage)
{
    if (storage == NULL || storage->clear_exact_slot == NULL) {
        return false;
    }
    const bool first = storage->clear_exact_slot(
        storage->context, BACKEND_OTA_EVENT_OUTBOX_SLOT_0);
    const bool second = storage->clear_exact_slot(
        storage->context, BACKEND_OTA_EVENT_OUTBOX_SLOT_1);
    return first && second;
}
