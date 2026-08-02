#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_scanner_topology.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_SCANNER_STATUS_SCHEMA 1U
#define BACKEND_SCANNER_STATUS_MAX_LINE 4095U

typedef struct {
    uint8_t schema;
    uint32_t sequence;
    uint32_t boot_id;
    char mac[18];
    char target[40];
    char project[40];
    char hardware[40];
    char version[32];
    backend_scan_profile_t profile;
    uint32_t role_generation;
    bool role_acked;
    bool command_ingress;
    bool ble_healthy;
    bool wifi_healthy;
    bool flow_paused;
    char ota_state[24];
    char rollback_state[24];
    uint32_t rx_errors;
    uint32_t tx_drops;
    uint64_t uptime_ms;
} backend_scanner_status_t;

typedef enum {
    BACKEND_SCANNER_STATUS_DECODE_OK = 0,
    BACKEND_SCANNER_STATUS_MALFORMED,
    BACKEND_SCANNER_STATUS_SCHEMA_MISMATCH,
    BACKEND_SCANNER_STATUS_TOO_LARGE,
} backend_scanner_status_decode_result_t;

typedef enum {
    BACKEND_SCANNER_STATUS_ACCEPTED = 0,
    BACKEND_SCANNER_STATUS_REFRESHED,
    BACKEND_SCANNER_STATUS_STALE,
    BACKEND_SCANNER_STATUS_CONFLICT,
    BACKEND_SCANNER_STATUS_CHANGED_BOOT,
    BACKEND_SCANNER_STATUS_INVALID,
} backend_scanner_status_accept_result_t;

typedef struct {
    bool initialized;
    uint32_t boot_id;
    uint32_t sequence;
    backend_scanner_status_t status;
} backend_scanner_status_tracker_t;

size_t backend_scanner_status_encode(
    const backend_scanner_status_t *status,
    char *output,
    size_t capacity);

backend_scanner_status_decode_result_t backend_scanner_status_decode(
    const char *json,
    size_t length,
    backend_scanner_status_t *out);

void backend_scanner_status_tracker_init(
    backend_scanner_status_tracker_t *tracker);

backend_scanner_status_accept_result_t backend_scanner_status_tracker_accept(
    backend_scanner_status_tracker_t *tracker,
    const backend_scanner_status_t *status);

#ifdef __cplusplus
}
#endif
