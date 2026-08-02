#pragma once

#include <stddef.h>

#include "ble_investigation_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MSG_TYPE_BLE_INVESTIGATE  "ble_investigate"
#define MSG_TYPE_BLE_INV_BEGIN    "ble_inv_begin"
#define MSG_TYPE_BLE_INV_PROGRESS "ble_inv_progress"
#define MSG_TYPE_BLE_INV_SERVICE  "ble_inv_service"
#define MSG_TYPE_BLE_INV_CHAR     "ble_inv_char"
#define MSG_TYPE_BLE_INV_READ     "ble_inv_read"
#define MSG_TYPE_BLE_INV_END      "ble_inv_end"

const char *ble_investigation_mode_name(ble_investigation_mode_t mode);
const char *ble_investigation_state_name(ble_investigation_state_t state);
bool ble_investigation_mode_from_name(const char *name,
                                      ble_investigation_mode_t *out);
bool ble_investigation_state_from_name(const char *name,
                                       ble_investigation_state_t *out);

void ble_investigation_result_init(ble_investigation_result_t *result);
bool ble_investigation_result_accept(ble_investigation_result_t *result,
                                     const ble_investigation_chunk_t *chunk);

size_t ble_investigation_request_to_json(
    const ble_investigation_request_t *request,
    char *out,
    size_t out_len);
size_t ble_investigation_chunk_to_json(const ble_investigation_chunk_t *chunk,
                                       char *out,
                                       size_t out_len);

#ifdef __cplusplus
}
#endif
