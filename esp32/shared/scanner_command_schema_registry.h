#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "badge_display_policy.h"
#include "firmware_json_schema_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FOF_SCANNER_REQUEST_ID_CAPACITY 33U
#define FOF_SCANNER_MAC_CAPACITY 18U
#define FOF_SCANNER_CAL_SESSION_CAPACITY 13U
#define FOF_SCANNER_CAL_UUID_CAPACITY 37U
#define FOF_SCANNER_SCAN_PROFILE_CAPACITY 24U
#define FOF_SCANNER_SLOT_ROLE_CAPACITY 24U
#define FOF_SCANNER_DISPLAY_VIEW_CAPACITY 16U

typedef enum {
    FOF_SCANNER_DEPLOYMENT_BADGE = 0,
    FOF_SCANNER_DEPLOYMENT_NON_BADGE,
} fof_scanner_deployment_t;

typedef enum {
    FOF_SCANNER_COMMAND_ROUTE_NONE = 0,
    FOF_SCANNER_COMMAND_ROUTE_FIRMWARE,
    FOF_SCANNER_COMMAND_ROUTE_NON_FIRMWARE,
} fof_scanner_command_route_t;

typedef enum {
    FOF_SCANNER_COMMAND_NONE = 0,
    FOF_SCANNER_COMMAND_READY,
    FOF_SCANNER_COMMAND_START,
    FOF_SCANNER_COMMAND_STOP,
    FOF_SCANNER_COMMAND_SCANNER_QUIET,
    FOF_SCANNER_COMMAND_BLE_INVESTIGATE,
    FOF_SCANNER_COMMAND_BLE_INVESTIGATE_CANCEL,
    FOF_SCANNER_COMMAND_WIFI_LOCKON,
    FOF_SCANNER_COMMAND_WIFI_LOCKON_CANCEL,
    FOF_SCANNER_COMMAND_BLE_LOCKON,
    FOF_SCANNER_COMMAND_BLE_LOCKON_CANCEL,
    FOF_SCANNER_COMMAND_CAL_MODE_START,
    FOF_SCANNER_COMMAND_CAL_MODE_STOP,
    FOF_SCANNER_COMMAND_SCAN_PROFILE,
    FOF_SCANNER_COMMAND_DISPLAY_CONTROL_FULL,
    FOF_SCANNER_COMMAND_DISPLAY_CONTROL_BUTTON,
    FOF_SCANNER_COMMAND_DISPLAY_POLICY,
    FOF_SCANNER_COMMAND_TIME,
    FOF_SCANNER_COMMAND_SAFE_MODE,
    FOF_SCANNER_COMMAND_REBOOT,
#if defined(FOF_DC34_GAME_CANARY)
    FOF_SCANNER_COMMAND_CRUD_SELF,
#endif
    FOF_SCANNER_COMMAND_COUNT,
} fof_scanner_command_id_t;

typedef enum {
    FOF_SCANNER_BLE_INVESTIGATION_GATT = 0,
    FOF_SCANNER_BLE_INVESTIGATION_PASSIVE_CAPTURE,
} fof_scanner_ble_investigation_mode_t;

typedef enum {
    FOF_SCANNER_TIME_SOURCE_NONE = 0,
    FOF_SCANNER_TIME_SOURCE_BACKEND,
    FOF_SCANNER_TIME_SOURCE_LOCAL,
} fof_scanner_time_source_t;

typedef struct {
    bool enabled;
    uint32_t generation;
} fof_scanner_quiet_command_t;

typedef struct {
    char request_id[FOF_SCANNER_REQUEST_ID_CAPACITY];
    fof_scanner_ble_investigation_mode_t mode;
    bool target_is_null;
    char target_mac[FOF_SCANNER_MAC_CAPACITY];
    int32_t timeout_ms;
} fof_scanner_ble_investigate_command_t;

typedef struct {
    char request_id[FOF_SCANNER_REQUEST_ID_CAPACITY];
} fof_scanner_ble_investigate_cancel_command_t;

typedef struct {
    int32_t channel;
    int32_t duration_s;
    char bssid[FOF_SCANNER_MAC_CAPACITY];
} fof_scanner_wifi_lockon_command_t;

typedef struct {
    char mac[FOF_SCANNER_MAC_CAPACITY];
    int32_t duration_s;
} fof_scanner_ble_lockon_command_t;

typedef struct {
    char session_id[FOF_SCANNER_CAL_SESSION_CAPACITY];
    char advertise_uuid[FOF_SCANNER_CAL_UUID_CAPACITY];
} fof_scanner_calibration_command_t;

typedef struct {
    char scan_profile[FOF_SCANNER_SCAN_PROFILE_CAPACITY];
    char slot_role[FOF_SCANNER_SLOT_ROLE_CAPACITY];
} fof_scanner_scan_profile_command_t;

typedef struct {
    bool button_enabled;
    char view[FOF_SCANNER_DISPLAY_VIEW_CAPACITY];
    int32_t page;
    bool page_lock;
    bool auto_page;
} fof_scanner_display_control_command_t;

typedef struct {
    uint32_t version;
    uint32_t hash;
    badge_display_policy_t policy;
} fof_scanner_display_policy_command_t;

typedef struct {
    int64_t epoch_ms;
    bool ok;
    fof_scanner_time_source_t source;
} fof_scanner_time_command_t;

typedef struct {
    bool enabled;
} fof_scanner_safe_mode_command_t;

#if defined(FOF_DC34_GAME_CANARY)
typedef struct {
    uint32_t peer;
    uint8_t session;
} fof_scanner_crud_self_command_t;
#endif

typedef struct {
    fof_scanner_command_id_t id;
    union {
        fof_scanner_quiet_command_t scanner_quiet;
        fof_scanner_ble_investigate_command_t ble_investigate;
        fof_scanner_ble_investigate_cancel_command_t
            ble_investigate_cancel;
        fof_scanner_wifi_lockon_command_t wifi_lockon;
        fof_scanner_ble_lockon_command_t ble_lockon;
        fof_scanner_calibration_command_t calibration;
        fof_scanner_scan_profile_command_t scan_profile;
        fof_scanner_display_control_command_t display_control;
        fof_scanner_display_policy_command_t display_policy;
        fof_scanner_time_command_t time;
        fof_scanner_safe_mode_command_t safe_mode;
#if defined(FOF_DC34_GAME_CANARY)
        fof_scanner_crud_self_command_t crud_self;
#endif
    } data;
} fof_scanner_command_t;

typedef struct {
    fof_scanner_command_route_t route;
    fof_fw_json_schema_id_t firmware_schema_id;
    fof_scanner_command_t command;
} fof_scanner_command_decision_t;

typedef enum {
    FOF_SCANNER_COMMAND_REGISTRY_OK = 0,
    FOF_SCANNER_COMMAND_REGISTRY_INVALID_ARGUMENT,
    FOF_SCANNER_COMMAND_REGISTRY_SELECTOR_REJECTED,
    FOF_SCANNER_COMMAND_REGISTRY_UNKNOWN_SELECTOR,
    FOF_SCANNER_COMMAND_REGISTRY_FIRMWARE_SCHEMA_REJECTED,
    FOF_SCANNER_COMMAND_REGISTRY_NONFIRMWARE_SCHEMA_REJECTED,
    FOF_SCANNER_COMMAND_REGISTRY_SEMANTIC_REJECTED,
    FOF_SCANNER_COMMAND_REGISTRY_MUTATION_REFUSED,
} fof_scanner_command_registry_result_t;

/**
 * Route and validate one raw uplink-to-scanner JSON span.
 *
 * C4 firmware schemas are always selected first. A recognized but malformed
 * firmware selector is rejected here and never falls through to routine
 * command selection. Routine `bootloader` and legacy `ota` selectors are
 * recognized only to return MUTATION_REFUSED; they never authorize effects.
 */
fof_scanner_command_registry_result_t
fof_scanner_command_select_and_validate(
    const uint8_t *bytes,
    size_t byte_len,
    fof_scanner_deployment_t deployment,
    fof_scanner_command_decision_t *decision_out);

#ifdef __cplusplus
}
#endif
