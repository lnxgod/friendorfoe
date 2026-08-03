#ifndef BACKEND_OTA_EVENT_OUTBOX_H
#define BACKEND_OTA_EVENT_OUTBOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_hardware_profile.h"
#include "backend_ota_operation_id.h"

#if !defined(FOF_BACKEND_PROFILE_S3_FULLSIZE) || \
    defined(FOF_BACKEND_PROFILE_BADGE_LITE)
#error "backend OTA event outbox requires the S3 Fullsize profile"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_OTA_EVENT_MAX_BYTES 1536U
#define BACKEND_OTA_EVENT_OUTBOX_SLOT_COUNT 2U
#define BACKEND_OTA_EVENT_OUTBOX_SLOT_MAX_BYTES 1606U

typedef enum {
    BACKEND_OTA_EVENT_OUTBOX_SLOT_0 = 0,
    BACKEND_OTA_EVENT_OUTBOX_SLOT_1 = 1,
} backend_ota_event_outbox_slot_t;

typedef enum {
    BACKEND_OTA_EVENT_OUTBOX_STATE_PENDING = 1,
    BACKEND_OTA_EVENT_OUTBOX_STATE_TOMBSTONE = 2,
} backend_ota_event_outbox_state_t;

typedef struct {
    backend_ota_operation_id_t operation_id;
    uint32_t sequence;
    uint16_t body_length;
    uint8_t body_sha256[32];
    uint8_t body[BACKEND_OTA_EVENT_MAX_BYTES];
} backend_ota_pending_event_t;

typedef struct {
    size_t length;
    uint8_t bytes[BACKEND_OTA_EVENT_OUTBOX_SLOT_MAX_BYTES];
} backend_ota_event_outbox_blob_t;

typedef struct {
    backend_ota_event_outbox_state_t state;
    uint32_t generation;
    backend_ota_pending_event_t event;
} backend_ota_event_outbox_record_t;

typedef enum {
    BACKEND_OTA_EVENT_OUTBOX_IO_OK = 0,
    BACKEND_OTA_EVENT_OUTBOX_IO_NOT_FOUND,
    BACKEND_OTA_EVENT_OUTBOX_IO_ERROR,
} backend_ota_event_outbox_io_result_t;

typedef backend_ota_event_outbox_io_result_t (*
    backend_ota_event_outbox_load_slot_fn)(
        void *context,
        backend_ota_event_outbox_slot_t slot,
        uint8_t *out,
        size_t capacity,
        size_t *out_length);

/* Success means the complete slot body is durably committed. */
typedef bool (*backend_ota_event_outbox_store_slot_fn)(
    void *context,
    backend_ota_event_outbox_slot_t slot,
    const uint8_t *bytes,
    size_t length);

/* This callback can clear only one named OTA outbox slot.  It intentionally
 * cannot erase an NVS namespace, partition, or unrelated application key. */
typedef bool (*backend_ota_event_outbox_clear_exact_slot_fn)(
    void *context, backend_ota_event_outbox_slot_t slot);

typedef struct {
    void *context;
    backend_ota_event_outbox_load_slot_fn load_slot;
    backend_ota_event_outbox_store_slot_fn store_slot;
    backend_ota_event_outbox_clear_exact_slot_fn clear_exact_slot;
} backend_ota_event_outbox_storage_t;

typedef enum {
    BACKEND_OTA_EVENT_OUTBOX_LOAD_EMPTY = 0,
    BACKEND_OTA_EVENT_OUTBOX_LOAD_PENDING,
    BACKEND_OTA_EVENT_OUTBOX_LOAD_CORRUPT,
    BACKEND_OTA_EVENT_OUTBOX_LOAD_IO_ERROR,
} backend_ota_event_outbox_load_result_t;

typedef struct {
    backend_ota_event_outbox_slot_t newest_slot;
    uint32_t generation;
    backend_ota_pending_event_t pending;
} backend_ota_event_outbox_snapshot_t;

typedef enum {
    BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_COMMITTED = 0,
    BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_ALREADY_PENDING,
    BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_BUSY,
    BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_CORRUPT,
    BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_INVALID,
    BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_IO_ERROR,
} backend_ota_event_outbox_enqueue_result_t;

/* ACK fields are supplied only after the strict eight-key event-ACK decoder
 * has validated component/action/terminal context. */
typedef struct {
    bool strict_decoded;
    uint16_t http_status;
    bool ok;
    bool has_operation_id;
    backend_ota_operation_id_t operation_id;
    uint32_t accepted_sequence;
    uint32_t next_sequence;
} backend_ota_event_outbox_ack_t;

typedef enum {
    BACKEND_OTA_EVENT_OUTBOX_ACK_CLEARED = 0,
    BACKEND_OTA_EVENT_OUTBOX_ACK_NO_PENDING,
    BACKEND_OTA_EVENT_OUTBOX_ACK_MISMATCH,
    BACKEND_OTA_EVENT_OUTBOX_ACK_CORRUPT,
    BACKEND_OTA_EVENT_OUTBOX_ACK_INVALID,
    BACKEND_OTA_EVENT_OUTBOX_ACK_IO_ERROR,
} backend_ota_event_outbox_ack_result_t;

bool backend_ota_event_outbox_slot_encode(
    const backend_ota_event_outbox_record_t *record,
    backend_ota_event_outbox_blob_t *out);

bool backend_ota_event_outbox_slot_decode(
    const uint8_t *bytes,
    size_t length,
    backend_ota_event_outbox_record_t *out);

backend_ota_event_outbox_load_result_t backend_ota_event_outbox_load(
    const backend_ota_event_outbox_storage_t *storage,
    backend_ota_event_outbox_snapshot_t *out);

backend_ota_event_outbox_enqueue_result_t backend_ota_event_outbox_enqueue(
    const backend_ota_event_outbox_storage_t *storage,
    const backend_ota_operation_id_t *operation_id,
    uint32_t sequence,
    const uint8_t *body,
    size_t body_length);

backend_ota_event_outbox_ack_result_t backend_ota_event_outbox_acknowledge(
    const backend_ota_event_outbox_storage_t *storage,
    const backend_ota_event_outbox_ack_t *ack);

/* Attended recovery is deliberately separate from normal load. */
bool backend_ota_event_outbox_recover_exact_slots(
    const backend_ota_event_outbox_storage_t *storage);

#ifdef __cplusplus
}
#endif

#endif
