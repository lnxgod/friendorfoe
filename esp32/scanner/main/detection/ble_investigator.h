#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ble_fingerprint.h"
#include "ble_investigation_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_INV_CONN_HANDLE_NONE       UINT16_MAX
#define BLE_INV_PEER_CACHE_FRESH_MS    30000

typedef enum {
    BLE_INVESTIGATOR_EVENT_CONNECTED = 0,
    BLE_INVESTIGATOR_EVENT_CONNECT_FAILED,
    BLE_INVESTIGATOR_EVENT_SERVICE,
    BLE_INVESTIGATOR_EVENT_CHARACTERISTIC,
    BLE_INVESTIGATOR_EVENT_READING_STARTED,
    BLE_INVESTIGATOR_EVENT_READ,
    BLE_INVESTIGATOR_EVENT_AUTH_REQUIRED,
    BLE_INVESTIGATOR_EVENT_PROCEDURE_FAILED,
    BLE_INVESTIGATOR_EVENT_SCANNER_UNAVAILABLE,
    BLE_INVESTIGATOR_EVENT_DISCOVERY_COMPLETE,
    BLE_INVESTIGATOR_EVENT_DISCONNECTED,
} ble_investigator_event_kind_t;

typedef struct {
    ble_investigator_event_kind_t kind;
    char service_uuid[BLE_INV_UUID_LEN];
    char uuid[BLE_INV_UUID_LEN];
    uint16_t properties;
    const uint8_t *value;
    size_t value_len;
    int status;
} ble_investigator_event_t;

typedef struct {
    ble_investigation_state_t state;
    ble_investigation_request_t request;
    ble_investigation_result_t result;
    int64_t deadline_ms;
    bool busy;
    bool connected;
    bool resume_scan_required;
    bool result_pending;
    uint16_t passive_apple_count;
    uint16_t passive_fast_pair_count;
    uint16_t passive_swift_pair_count;
} ble_investigator_t;

typedef enum {
    BLE_INV_RADIO_IDLE = 0,
    BLE_INV_RADIO_CONNECTING,
    BLE_INV_RADIO_CANCEL_PENDING,
    BLE_INV_RADIO_CONNECTED,
    BLE_INV_RADIO_TERMINATE_PENDING,
} ble_investigator_radio_state_t;

typedef enum {
    BLE_INV_CLEANUP_NONE = 0,
    BLE_INV_CLEANUP_CANCEL_CONNECT,
    BLE_INV_CLEANUP_TERMINATE_CONNECTION,
    BLE_INV_CLEANUP_RESUME_SCAN,
} ble_investigator_cleanup_action_t;

typedef enum {
    BLE_INV_PEER_LOOKUP_CONNECTED = 0,
    BLE_INV_PEER_LOOKUP_NOT_CONNECTED,
    BLE_INV_PEER_LOOKUP_INDETERMINATE,
} ble_investigator_peer_lookup_result_t;

typedef enum {
    BLE_INV_REQUEST_AVAILABLE = 0,
    BLE_INV_REQUEST_RETRANSMIT,
    BLE_INV_REQUEST_BUSY_REJECTION,
    BLE_INV_REQUEST_INVALID,
} ble_investigator_request_decision_t;

typedef struct {
    uint32_t generation;
    uint16_t expected_conn_handle;
    ble_investigator_radio_state_t radio_state;
    bool active;
    bool cleanup_pending;
    bool end_emitted;
    bool scan_resume_pending;
    bool operation_in_progress;
    bool deferred_discovery_pending;
} ble_investigator_runtime_fence_t;

typedef struct {
    uint32_t generation;
    ble_investigation_chunk_kind_t in_progress_kind;
    bool active;
    bool begin_emitted;
    bool emission_in_progress;
    bool end_started;
    bool end_emitted;
} ble_investigator_chunk_fence_t;

void ble_investigator_init(ble_investigator_t *state);
bool ble_investigator_start(ble_investigator_t *state,
                            const ble_investigation_request_t *request,
                            int64_t now_ms);
void ble_investigator_handle_event(ble_investigator_t *state,
                                   const ble_investigator_event_t *event,
                                   int64_t now_ms);
bool ble_investigator_prepare_procedure(
    ble_investigator_t *state,
    ble_investigation_state_t required_state,
    int64_t now_ms);
void ble_investigator_tick(ble_investigator_t *state, int64_t now_ms);
void ble_investigator_cancel(ble_investigator_t *state, int64_t now_ms);
bool ble_investigator_take_result(ble_investigator_t *state,
                                  ble_investigation_result_t *out);
bool ble_investigator_request_id_is_valid(const char *request_id);
ble_investigator_request_decision_t ble_investigator_decide_request(
    bool runtime_busy,
    const char *active_request_id,
    const char *incoming_request_id);
bool ble_investigator_parse_target_mac(const char *text, uint8_t out[6]);
bool ble_investigator_build_rejection_chunks(
    const char *request_id,
    ble_investigation_mode_t mode,
    const char *target_mac,
    const char *error,
    ble_investigation_chunk_t chunks[2]);
bool ble_investigator_passive_start_is_ready(
    const ble_investigation_request_t *request,
    bool scanner_scanning);
bool ble_investigator_host_start_is_allowed(bool investigation_gatt_active,
                                            bool resume_pending);
bool ble_investigator_scan_start_is_allowed(bool investigation_active,
                                            bool investigation_resume);
bool ble_investigator_fingerprint_is_swift_pair(
    const ble_fingerprint_t *fingerprint);
bool ble_investigator_peer_cache_is_fresh(int64_t last_seen_ms,
                                          int64_t now_ms);
void ble_investigator_note_advertisement(ble_investigator_t *state,
                                         const uint8_t mac[6],
                                         const ble_fingerprint_t *fingerprint,
                                         int8_t rssi,
                                         uint8_t properties,
                                         int64_t now_ms);

void ble_investigator_runtime_fence_init(
    ble_investigator_runtime_fence_t *fence);
uint32_t ble_investigator_runtime_fence_begin(
    ble_investigator_runtime_fence_t *fence);
bool ble_investigator_runtime_fence_mark_connecting(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation);
bool ble_investigator_runtime_fence_begin_operation(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation);
bool ble_investigator_runtime_fence_finish_operation(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation);
bool ble_investigator_runtime_reserve_operation(
    ble_investigator_t *state,
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation,
    ble_investigation_state_t required_state,
    int64_t now_ms);
bool ble_investigator_runtime_fence_defer_discovery(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation,
    uint16_t conn_handle);
bool ble_investigator_runtime_fence_take_deferred_discovery(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation,
    bool launch_allowed,
    uint16_t *conn_handle_out);
bool ble_investigator_runtime_fence_begin_cleanup(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation,
    bool scan_resume_required);
bool ble_investigator_runtime_fence_note_end_emitted(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation);
ble_investigator_cleanup_action_t
ble_investigator_runtime_fence_next_cleanup_action(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation,
    uint16_t *conn_handle_out);
bool ble_investigator_runtime_fence_cleanup_action_failed(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation,
    ble_investigator_cleanup_action_t action,
    bool radio_absent);
bool ble_investigator_runtime_fence_reconcile_cancel(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation,
    ble_investigator_peer_lookup_result_t lookup_result,
    uint16_t conn_handle);
bool ble_investigator_runtime_fence_note_connect_result(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation,
    bool connected,
    uint16_t conn_handle);
bool ble_investigator_runtime_fence_accepts_gatt(
    const ble_investigator_runtime_fence_t *fence,
    uint32_t generation,
    uint16_t conn_handle);
bool ble_investigator_runtime_fence_note_disconnected(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation,
    uint16_t conn_handle);
bool ble_investigator_runtime_fence_note_scan_resumed(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation);
bool ble_investigator_runtime_fence_can_release(
    const ble_investigator_runtime_fence_t *fence,
    uint32_t generation);
bool ble_investigator_runtime_fence_release(
    ble_investigator_runtime_fence_t *fence,
    uint32_t generation);

void ble_investigator_chunk_fence_init(
    ble_investigator_chunk_fence_t *fence);
bool ble_investigator_chunk_fence_open(
    ble_investigator_chunk_fence_t *fence,
    uint32_t generation);
bool ble_investigator_chunk_fence_begin_emit(
    ble_investigator_chunk_fence_t *fence,
    uint32_t generation,
    ble_investigation_chunk_kind_t kind);
bool ble_investigator_chunk_fence_finish_emit(
    ble_investigator_chunk_fence_t *fence,
    uint32_t generation,
    ble_investigation_chunk_kind_t kind,
    bool success);

bool ble_investigator_runtime_start(
    const ble_investigation_request_t *request,
    int64_t now_ms);
ble_investigator_request_decision_t
ble_investigator_runtime_decide_request(const char *request_id);
void ble_investigator_runtime_tick(int64_t now_ms);
bool ble_investigator_runtime_cancel(const char *request_id, int64_t now_ms);
bool ble_investigator_runtime_is_busy(void);
bool ble_investigator_runtime_is_gatt_active(void);
void ble_investigator_runtime_note_advertisement(
    const uint8_t mac[6],
    const ble_fingerprint_t *fingerprint,
    int8_t rssi,
    uint8_t properties,
    int64_t now_ms);
void ble_investigator_runtime_emit_rejection(
    const char *request_id,
    ble_investigation_mode_t mode,
    const char *target_mac,
    const char *error);

#ifdef __cplusplus
}
#endif
