#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_scanner_topology.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PRODUCTION_SCANNER_MESSAGE_NONE = 0,
    PRODUCTION_SCANNER_MESSAGE_INFO,
    PRODUCTION_SCANNER_MESSAGE_STATUS,
    PRODUCTION_SCANNER_MESSAGE_PROFILE_ACK,
    PRODUCTION_SCANNER_MESSAGE_STOP_ACK,
    PRODUCTION_SCANNER_MESSAGE_FW_CHECK,
    PRODUCTION_SCANNER_MESSAGE_FW_READY,
    PRODUCTION_SCANNER_MESSAGE_OTA,
} production_scanner_message_kind_t;

typedef struct {
    production_scanner_message_kind_t kind;
    char version[32];
    char board[40];
    char chip[24];
    char capabilities[40];
    char firmware_name[40];
    char app_project[40];
    char hardware_type[40];
    char hardware_id[18];
    char ota_state[24];
    char rollback_state[24];
    backend_scan_profile_t profile;
    uint32_t boot_id;
    uint32_t sequence;
    uint32_t command_rx_count;
    uint32_t tx_drops;
    uint64_t uptime_ms;
    bool identity_present;
    bool identity_valid;
    bool management_identity_present;
    bool management_identity_valid;
    bool boot_id_present;
    bool profile_present;
    bool slot_role_ok_present;
    bool slot_role_ok;
    bool ble_initialized_present;
    bool ble_initialized;
    bool ble_scanning_present;
    bool ble_scanning;
    bool ble_host_active_present;
    bool ble_host_active;
    bool ble_host_synced_present;
    bool ble_host_synced;
    bool wifi_initialized_present;
    bool wifi_initialized;
    bool wifi_active_present;
    bool wifi_active;
    bool wifi_paused_present;
    bool wifi_paused;
} production_scanner_message_t;

bool production_scanner_uart_decode(
    const char *json,
    size_t length,
    production_scanner_message_t *out);

bool production_scanner_identity_valid(
    const char *board,
    const char *chip,
    const char *capabilities,
    const char *version);

size_t production_scanner_encode_ready(char *output, size_t capacity);
size_t production_scanner_encode_stop(char *output, size_t capacity);
size_t production_scanner_encode_profile(
    backend_scan_profile_t profile,
    char *output,
    size_t capacity);
size_t production_scanner_encode_time(
    int64_t epoch_ms,
    const char *source,
    char *output,
    size_t capacity);

#ifdef __cplusplus
}
#endif
