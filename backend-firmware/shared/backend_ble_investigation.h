#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "backend_detection_codec.h"
#include "ble_investigation_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_COMMAND_RESULT_QUEUE_CAPACITY 64U
#define BACKEND_COMMAND_RESULT_MAX_JSON 512U

typedef struct {
    uint32_t sequence;
    char type[24];
    char state[16];
    uint16_t json_length;
    char json[BACKEND_COMMAND_RESULT_MAX_JSON + 1U];
} backend_command_result_t;

/* Keep this state in application-static storage on device. */
typedef struct {
    char command_id[BLE_INV_REQUEST_ID_LEN];
    ble_investigation_request_t request;
    backend_scanner_slot_t scanner_slot;
    bool active;
    bool radio_active;
    bool scanner_assigned;
    bool began;
    bool terminal_queued;
    bool cancel_requested;
    bool progress_seen;
    ble_investigation_state_t last_progress_state;
    uint8_t service_count;
    uint8_t characteristic_count;
    uint8_t read_count;
    uint8_t queue_head;
    uint8_t queue_count;
    uint32_t next_sequence;
    uint32_t radio_start_count;
    int64_t started_ms;
    backend_command_result_t queue[BACKEND_COMMAND_RESULT_QUEUE_CAPACITY];
} backend_ble_investigation_state_t;

void backend_ble_investigation_init(
    backend_ble_investigation_state_t *state);

/* Returns true only when new radio work starts. While a slot is occupied,
 * duplicate and conflicting deliveries both return false without mutation;
 * callers distinguish them using the retained command_id/request fields. */
bool backend_ble_investigation_start(
    backend_ble_investigation_state_t *state,
    const char *command_id,
    const ble_investigation_request_t *request,
    backend_scanner_slot_t scanner_slot,
    int64_t now_ms);

bool backend_ble_investigation_accept_chunk(
    backend_ble_investigation_state_t *state,
    backend_scanner_slot_t scanner_slot,
    const ble_investigation_chunk_t *chunk);

bool backend_ble_investigation_next_result(
    const backend_ble_investigation_state_t *state,
    backend_command_result_t *out);

bool backend_ble_investigation_mark_acked(
    backend_ble_investigation_state_t *state,
    const char *command_id,
    uint32_t result_sequence);

bool backend_ble_investigation_check_timeout(
    backend_ble_investigation_state_t *state,
    int64_t now_ms);

bool backend_ble_investigation_cancel_first_seen(
    backend_ble_investigation_state_t *state,
    const char *command_id,
    const ble_investigation_request_t *original_request,
    int64_t now_ms);

bool backend_ble_investigation_request_cancel(
    backend_ble_investigation_state_t *state,
    const char *command_id);

bool backend_ble_investigation_cancel_pending(
    const backend_ble_investigation_state_t *state,
    backend_scanner_slot_t *scanner_slot);

bool backend_ble_investigation_resume_after_reboot(
    backend_ble_investigation_state_t *state,
    const char *command_id,
    const ble_investigation_request_t *original_request,
    uint32_t next_sequence,
    bool cancel_pending);

#ifdef __cplusplus
}
#endif
