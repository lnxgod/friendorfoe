#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ble_investigation_protocol.h"
#include "uart_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BADGE_BLE_INVESTIGATION_SCANNER_SLOT 0
#define BADGE_BLE_INVESTIGATION_MAX_CHUNKS 64
#define BADGE_BLE_INVESTIGATION_STATUS_JSON_MAX UART_JSON_MAX_SIZE
#define BADGE_BLE_INVESTIGATION_USB_FRAME_MAX (UART_JSON_MAX_SIZE + 10)
#define BADGE_BLE_INVESTIGATION_OPERATION_STACK_MAX 2048

typedef struct {
    ble_investigation_result_t result;
    bool active;
    bool begin_received;
    bool end_received;
    uint8_t chunk_count;
    ble_investigation_chunk_t chunks[BADGE_BLE_INVESTIGATION_MAX_CHUNKS];
} badge_ble_investigation_state_t;

typedef struct {
    bool valid;
    char request_id[BLE_INV_REQUEST_ID_LEN];
    int seq;
} badge_ble_investigation_selection_t;

typedef struct {
    uint32_t generation;
    uint32_t pending_generation;
    uint32_t revision_at_reserve;
    bool pending;
} badge_ble_investigation_start_fence_t;

typedef struct {
    char request_id[BLE_INV_REQUEST_ID_LEN];
    ble_investigation_mode_t mode;
    ble_investigation_state_t state;
    char summary[BLE_INV_SUMMARY_LEN];
    char error[BLE_INV_ERROR_LEN];
    uint8_t service_count;
    uint8_t characteristic_count;
    bool authentication_required;
    bool truncated;
} badge_ble_investigation_status_t;

bool badge_ble_investigation_request_validate(
    const ble_investigation_request_t *request,
    ble_investigation_request_t *normalized);
bool badge_ble_investigation_index_from_number(double value, int *out);
bool badge_ble_investigation_uuid_is_canonical(const char *uuid);
bool badge_ble_investigation_value_hex_is_valid(const char *value_hex);

void badge_ble_investigation_state_init(
    badge_ble_investigation_state_t *state);
bool badge_ble_investigation_state_start(
    badge_ble_investigation_state_t *state,
    const ble_investigation_request_t *request,
    bool scanner_available,
    int *scanner_slot_out);
bool badge_ble_investigation_state_accept(
    badge_ble_investigation_state_t *state,
    const ble_investigation_chunk_t *chunk);
void badge_ble_investigation_state_get(
    const badge_ble_investigation_state_t *state,
    ble_investigation_result_t *out);
void badge_ble_investigation_state_transport_lost(
    badge_ble_investigation_state_t *state);
bool badge_ble_investigation_state_get_chunk(
    const badge_ble_investigation_state_t *state,
    const char *request_id,
    int seq,
    ble_investigation_chunk_t *out);

void badge_ble_investigation_selection_init(
    badge_ble_investigation_selection_t *selection);
bool badge_ble_investigation_selection_set(
    badge_ble_investigation_selection_t *selection,
    const badge_ble_investigation_state_t *state,
    const char *request_id,
    int seq);
void badge_ble_investigation_selection_clear(
    badge_ble_investigation_selection_t *selection);
bool badge_ble_investigation_selection_get(
    const badge_ble_investigation_selection_t *selection,
    const badge_ble_investigation_state_t *state,
    ble_investigation_chunk_t *out);

void badge_ble_investigation_start_fence_init(
    badge_ble_investigation_start_fence_t *fence);
uint32_t badge_ble_investigation_start_fence_reserve(
    badge_ble_investigation_start_fence_t *fence,
    uint32_t revision);
bool badge_ble_investigation_start_fence_should_rollback(
    badge_ble_investigation_start_fence_t *fence,
    uint32_t generation,
    uint32_t current_revision,
    bool send_succeeded);

void badge_ble_investigation_state_status(
    const badge_ble_investigation_state_t *state,
    badge_ble_investigation_status_t *out);
size_t badge_ble_investigation_status_to_json(
    const badge_ble_investigation_status_t *status,
    char *out,
    size_t out_len);
size_t badge_ble_investigation_state_status_json(
    const badge_ble_investigation_state_t *state,
    char *out,
    size_t out_len);
size_t badge_ble_investigation_usb_frame(const char *chunk_json,
                                         char *out,
                                         size_t out_len);
bool badge_ble_investigation_chunk_read_authorized(bool encrypted,
                                                   bool bonded);

#ifdef __cplusplus
}
#endif
