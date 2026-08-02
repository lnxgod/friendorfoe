#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_ota_identity.h"
#include "backend_scanner_control_codec.h"
#include "backend_uart_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UART_OTA_INACTIVE_SLOT_CAPACITY (2U * 1024U * 1024U)
#define UART_OTA_FLASH_WRITE_BYTES 16384U
#define UART_OTA_RECEIPT_REASON_CAPACITY 32U

typedef enum {
    UART_OTA_STATE_IDLE = 0,
    UART_OTA_STATE_STAGING,
    UART_OTA_STATE_IMAGE_STAGED,
    UART_OTA_STATE_WRITING,
    UART_OTA_STATE_PENDING_VERIFY,
    UART_OTA_STATE_DRY_RUN_COMPLETE,
    UART_OTA_STATE_FAILED,
} uart_ota_state_t;

typedef enum {
    UART_OTA_RECEIPT_ACK = 0,
    UART_OTA_RECEIPT_NACK,
    UART_OTA_RECEIPT_STAGED,
    UART_OTA_RECEIPT_DONE,
    UART_OTA_RECEIPT_ERROR,
} uart_ota_receipt_kind_t;

typedef enum {
    UART_OTA_RESULT_OK = 0,
    UART_OTA_RESULT_INVALID_ARGUMENT,
    UART_OTA_RESULT_INVALID_STATE,
    UART_OTA_RESULT_STALE_GENERATION,
    UART_OTA_RESULT_WRONG_SESSION,
    UART_OTA_RESULT_BINDING_MISMATCH,
    UART_OTA_RESULT_MANIFEST_REJECTED,
    UART_OTA_RESULT_NO_PSRAM,
    UART_OTA_RESULT_WIRE_ERROR,
    UART_OTA_RESULT_IMAGE_REJECTED,
    UART_OTA_RESULT_FLASH_ERROR,
    UART_OTA_RESULT_RECEIPT_ERROR,
} uart_ota_result_t;

typedef struct {
    uint8_t component_slot;
    char mac[18];
    uint32_t boot_id;
    uint32_t topology_generation;
} uart_ota_local_binding_t;

typedef struct {
    uart_ota_receipt_kind_t type;
    uint32_t session_id;
    uint32_t generation;
    uint16_t sequence;
    uint16_t next_sequence;
    uint32_t received;
    bool dry_run;
    backend_ota_image_result_t image_result;
    char reason[UART_OTA_RECEIPT_REASON_CAPACITY];
} uart_ota_receipt_t;

typedef struct {
    void *context;
    bool (*read_binding)(void *context, uart_ota_local_binding_t *out);
    uint8_t *(*psram_acquire)(void *context, size_t size);
    void (*psram_release)(
        void *context, uint8_t *buffer, size_t size);
    bool (*emit_receipt)(
        void *context, const uart_ota_receipt_t *receipt);
    /* These callbacks must address only the inactive 2 MiB OTA slot. */
    bool (*inactive_slot_begin)(void *context, size_t size);
    bool (*inactive_slot_write)(
        void *context, size_t offset,
        const uint8_t *bytes, size_t size);
    bool (*inactive_slot_finish)(void *context);
    void (*inactive_slot_abort)(void *context);
    /* Selects that slot as pending-verify; rollback policy clears it later. */
    bool (*inactive_slot_activate_pending_verify)(void *context);
    bool (*request_reboot)(void *context);
} uart_ota_ops_t;

typedef struct {
    const char *running_version;
    size_t inactive_slot_capacity;
    uart_ota_ops_t ops;
} uart_ota_config_t;

typedef enum {
    UART_OTA_FRAME_HEADER = 0,
    UART_OTA_FRAME_DATA,
    UART_OTA_FRAME_CRC,
    UART_OTA_FRAME_DISCARD,
} uart_ota_frame_phase_t;

typedef struct {
    uart_ota_state_t state;
    uart_ota_ops_t ops;
    char running_version[32];
    size_t inactive_slot_capacity;
    backend_ota_manifest_t manifest;
    uart_ota_local_binding_t bound_binding;
    uint8_t *staging;
    size_t staged_size;
    uint32_t session_id;
    uint32_t generation;
    uint32_t highest_generation;
    uint16_t expected_sequence;
    uint16_t frame_sequence;
    uint16_t frame_length;
    uint16_t frame_position;
    uint8_t frame_header[OTA_CHUNK_HEADER_SIZE];
    uint8_t frame_header_position;
    uint8_t frame_data[OTA_CHUNK_MAX_DATA];
    uint8_t frame_crc[OTA_CHUNK_CRC_SIZE];
    uint8_t frame_crc_position;
    uint32_t discard_remaining;
    uint32_t last_accepted_crc;
    uint16_t last_accepted_sequence;
    uint16_t last_accepted_length;
    uart_ota_frame_phase_t frame_phase;
    bool frame_duplicate;
    bool frame_duplicate_matches;
    bool have_last_accepted;
    bool dry_run;
    bool initialized;
} uart_ota_t;

bool uart_ota_init(uart_ota_t *ota, const uart_ota_config_t *config);

/* BEGIN, END, and ABORT keep one immutable session/generation pair. */
uart_ota_result_t uart_ota_begin(
    uart_ota_t *ota,
    const backend_scanner_ota_begin_control_t *begin);

/* While staging, an exact retry of the preceding frame is acknowledged again. */
uart_ota_result_t uart_ota_consume(
    uart_ota_t *ota,
    const uint8_t *bytes,
    size_t size,
    size_t *consumed);

uart_ota_result_t uart_ota_end(
    uart_ota_t *ota,
    const backend_scanner_ota_finish_control_t *end);

uart_ota_result_t uart_ota_abort(
    uart_ota_t *ota,
    const backend_scanner_ota_finish_control_t *abort_control);

void uart_ota_reset(uart_ota_t *ota);

bool uart_ota_is_receiving_binary(const uart_ota_t *ota);
uint32_t uart_ota_received(const uart_ota_t *ota);
uint16_t uart_ota_expected_sequence(const uart_ota_t *ota);

uint32_t uart_ota_chunk_crc32(const uint8_t *bytes, size_t size);

size_t uart_ota_receipt_to_json(
    const uart_ota_receipt_t *receipt,
    char *output,
    size_t capacity);

#ifdef __cplusplus
}
#endif
