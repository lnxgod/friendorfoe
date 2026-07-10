#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ble_fingerprint.h"
#include "ble_investigation_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BLE_INVESTIGATOR_EVENT_CONNECTED = 0,
    BLE_INVESTIGATOR_EVENT_CONNECT_FAILED,
    BLE_INVESTIGATOR_EVENT_SERVICE,
    BLE_INVESTIGATOR_EVENT_CHARACTERISTIC,
    BLE_INVESTIGATOR_EVENT_READ,
    BLE_INVESTIGATOR_EVENT_AUTH_REQUIRED,
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

void ble_investigator_init(ble_investigator_t *state);
bool ble_investigator_start(ble_investigator_t *state,
                            const ble_investigation_request_t *request,
                            int64_t now_ms);
void ble_investigator_handle_event(ble_investigator_t *state,
                                   const ble_investigator_event_t *event,
                                   int64_t now_ms);
void ble_investigator_tick(ble_investigator_t *state, int64_t now_ms);
void ble_investigator_cancel(ble_investigator_t *state, int64_t now_ms);
bool ble_investigator_take_result(ble_investigator_t *state,
                                  ble_investigation_result_t *out);
bool ble_investigator_parse_target_mac(const char *text, uint8_t out[6]);
void ble_investigator_note_advertisement(ble_investigator_t *state,
                                         const uint8_t mac[6],
                                         const ble_fingerprint_t *fingerprint,
                                         int8_t rssi,
                                         uint8_t properties,
                                         int64_t now_ms);

bool ble_investigator_runtime_start(
    const ble_investigation_request_t *request,
    int64_t now_ms);
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

#ifdef __cplusplus
}
#endif
