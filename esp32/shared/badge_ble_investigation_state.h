#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ble_investigation_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BADGE_BLE_INVESTIGATION_SCANNER_SLOT 0
#define BADGE_BLE_INVESTIGATION_MAX_CHUNKS 64
#define BADGE_BLE_INVESTIGATION_STATUS_JSON_MAX 512

typedef struct {
    ble_investigation_result_t result;
    bool active;
    bool begin_received;
    bool end_received;
    int selected_chunk;
    uint8_t chunk_count;
    ble_investigation_chunk_t chunks[BADGE_BLE_INVESTIGATION_MAX_CHUNKS];
} badge_ble_investigation_state_t;

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
bool badge_ble_investigation_state_select_chunk(
    badge_ble_investigation_state_t *state,
    const char *request_id,
    int seq);
bool badge_ble_investigation_state_get_selected_chunk(
    const badge_ble_investigation_state_t *state,
    ble_investigation_chunk_t *out);
size_t badge_ble_investigation_state_status_json(
    const badge_ble_investigation_state_t *state,
    char *out,
    size_t out_len);
bool badge_ble_investigation_chunk_read_authorized(bool encrypted,
                                                   bool bonded);

#ifdef __cplusplus
}
#endif
