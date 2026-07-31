#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ble_investigation_ingress_schema.h"
#include "firmware_json_schema_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FOF_SCANNER_UPLINK_ROUTE_NONE = 0,
    FOF_SCANNER_UPLINK_ROUTE_FIRMWARE,
    FOF_SCANNER_UPLINK_ROUTE_BLE_INVESTIGATION,
    FOF_SCANNER_UPLINK_ROUTE_DETECTION,
    FOF_SCANNER_UPLINK_ROUTE_STATUS,
    FOF_SCANNER_UPLINK_ROUTE_SCANNER_INFO,
    FOF_SCANNER_UPLINK_ROUTE_CAL_MODE_ACK,
    FOF_SCANNER_UPLINK_ROUTE_SCAN_PROFILE_ACK,
    FOF_SCANNER_UPLINK_ROUTE_DISPLAY_CONTROL_ACK,
    FOF_SCANNER_UPLINK_ROUTE_DISPLAY_POLICY_ACK,
    FOF_SCANNER_UPLINK_ROUTE_SCANNER_QUIET_ACK,
    FOF_SCANNER_UPLINK_ROUTE_RECOVERY_ACK,
    FOF_SCANNER_UPLINK_ROUTE_SCANNER_RECOVERY,
#if defined(FOF_DC34_GAME_CANARY)
    FOF_SCANNER_UPLINK_ROUTE_CRUD_SELF_ACK,
#endif
} fof_scanner_uplink_route_t;

typedef struct {
    fof_scanner_uplink_route_t route;
    fof_fw_json_schema_id_t firmware_schema_id;
    fof_ble_inv_ingress_schema_id_t ble_schema_id;
#if defined(FOF_DC34_GAME_CANARY)
    uint32_t crud_peer;
    uint8_t crud_session;
#endif
} fof_scanner_uplink_decision_t;

typedef enum {
    FOF_SCANNER_UPLINK_INGRESS_OK = 0,
    FOF_SCANNER_UPLINK_INGRESS_INVALID_ARGUMENT,
    FOF_SCANNER_UPLINK_INGRESS_SELECTOR_REJECTED,
    FOF_SCANNER_UPLINK_INGRESS_UNKNOWN_SELECTOR,
    FOF_SCANNER_UPLINK_INGRESS_FIRMWARE_SCHEMA_REJECTED,
    FOF_SCANNER_UPLINK_INGRESS_BLE_SCHEMA_REJECTED,
    FOF_SCANNER_UPLINK_INGRESS_TELEMETRY_SCHEMA_REJECTED,
    FOF_SCANNER_UPLINK_INGRESS_EASTER_FRAME_REQUIRED,
} fof_scanner_uplink_ingress_result_t;

/**
 * Validate and route one complete raw scanner-to-uplink JSON span.
 *
 * Firmware receipts are delegated to the closed C4 registry. BLE
 * investigation chunks are delegated to C9c and remain slot-0-only. Small
 * control/recovery acknowledgements use exact closed schemas. High-volume
 * detection, status, and scanner-info telemetry receive a complete-object,
 * unique-exact-selector envelope gate before their existing semantic parser.
 */
fof_scanner_uplink_ingress_result_t
fof_scanner_uplink_ingress_select_and_validate(
    const uint8_t *bytes,
    size_t byte_len,
    int scanner_slot,
    fof_scanner_uplink_decision_t *decision_out);

#ifdef __cplusplus
}
#endif
