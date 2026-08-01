#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FOF_BLE_INVESTIGATION_SCANNER_SLOT 0

typedef enum {
    FOF_BLE_INV_INGRESS_NONE = 0,
    FOF_BLE_INV_INGRESS_BEGIN,
    FOF_BLE_INV_INGRESS_PROGRESS,
    FOF_BLE_INV_INGRESS_SERVICE,
    FOF_BLE_INV_INGRESS_CHARACTERISTIC,
    FOF_BLE_INV_INGRESS_READ,
    FOF_BLE_INV_INGRESS_END,
    FOF_BLE_INV_INGRESS_SCHEMA_COUNT,
} fof_ble_inv_ingress_schema_id_t;

typedef enum {
    FOF_BLE_INV_INGRESS_OK = 0,
    FOF_BLE_INV_INGRESS_INVALID_ARGUMENT,
    FOF_BLE_INV_INGRESS_WRONG_SCANNER_SLOT,
    FOF_BLE_INV_INGRESS_SELECTOR_REJECTED,
    FOF_BLE_INV_INGRESS_UNKNOWN_SELECTOR,
    FOF_BLE_INV_INGRESS_SCHEMA_REJECTED,
    FOF_BLE_INV_INGRESS_SEMANTIC_REJECTED,
} fof_ble_inv_ingress_result_t;

/**
 * Validate one scanner-originated BLE investigation JSON span before cJSON.
 *
 * Only the dedicated BLE scanner slot is authorized. The output remains
 * NONE for every rejection so a caller cannot accidentally dispatch stale
 * schema state.
 */
fof_ble_inv_ingress_result_t fof_ble_investigation_ingress_validate(
    const uint8_t *bytes,
    size_t byte_len,
    int scanner_slot,
    fof_ble_inv_ingress_schema_id_t *schema_out);

#ifdef __cplusplus
}
#endif
