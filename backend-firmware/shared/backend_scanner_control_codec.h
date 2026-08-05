#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_scanner_topology.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_SCANNER_WIRE_MAX_LINE 4095U

typedef enum {
    BACKEND_SCANNER_CONTROL_ROLE = 0,
    BACKEND_SCANNER_CONTROL_TIME,
    BACKEND_SCANNER_CONTROL_FLOW,
    BACKEND_SCANNER_CONTROL_LED_STATE,
    BACKEND_SCANNER_CONTROL_HEALTH_REQUEST,
    BACKEND_SCANNER_CONTROL_RECOVERY,
    BACKEND_SCANNER_CONTROL_INVESTIGATE,
    BACKEND_SCANNER_CONTROL_CANCEL,
    BACKEND_SCANNER_CONTROL_OTA_BEGIN,
    BACKEND_SCANNER_CONTROL_OTA_END,
    BACKEND_SCANNER_CONTROL_OTA_ABORT,
} backend_scanner_control_kind_t;

typedef enum {
    BACKEND_SCANNER_TIME_NONE = 0,
    BACKEND_SCANNER_TIME_SNTP,
    BACKEND_SCANNER_TIME_BACKEND,
} backend_scanner_time_source_t;

typedef enum {
    BACKEND_SCANNER_RECOVERY_RESTART_RADIOS = 0,
} backend_scanner_recovery_action_t;

typedef enum {
    BACKEND_SCANNER_INVESTIGATE_GATT = 0,
    BACKEND_SCANNER_INVESTIGATE_PASSIVE_CAPTURE,
} backend_scanner_investigate_mode_t;

typedef struct {
    uint32_t boot_id;
    uint32_t generation;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    /* Uplink-owned topology epoch; independent of role command generation. */
    uint32_t topology_generation;
#endif
    backend_scan_profile_t profile;
} backend_scanner_role_control_t;

typedef struct {
    uint32_t generation;
    bool valid;
    int64_t epoch_ms;
    backend_scanner_time_source_t source;
} backend_scanner_time_control_t;

typedef struct {
    uint32_t generation;
    bool paused;
} backend_scanner_flow_control_t;

typedef struct {
    char state[16];
    uint32_t generation;
    uint32_t ttl_ms;
} backend_scanner_led_control_t;

typedef struct {
    uint32_t sequence;
} backend_scanner_health_request_t;

typedef struct {
    uint32_t boot_id;
    uint32_t generation;
    backend_scanner_recovery_action_t action;
} backend_scanner_recovery_control_t;

typedef struct {
    char command_id[33];
    bool has_mac;
    char mac[18];
    backend_scanner_investigate_mode_t mode;
    uint32_t timeout_ms;
} backend_scanner_investigate_control_t;

typedef struct {
    char command_id[33];
} backend_scanner_cancel_control_t;

typedef struct {
    uint32_t session_id;
    /* Monotonic replay epoch for this UART session. */
    uint32_t generation;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    /* Immutable catalog/store generation; independent of UART replay. */
    uint32_t manifest_generation;
#endif
    uint8_t component_slot;
    char expected_mac[18];
    uint32_t expected_boot_id;
    uint32_t expected_topology_generation;
    char target[40];
    char project[40];
    char hardware[40];
    char version[32];
    uint32_t image_size;
    uint32_t crc32;
    char sha256[65];
    bool allow_same_version;
    bool dry_run;
} backend_scanner_ota_begin_control_t;

typedef struct {
    uint32_t session_id;
    uint32_t generation;
    char reason[48];
} backend_scanner_ota_finish_control_t;

typedef struct {
    backend_scanner_control_kind_t type;
    union {
        backend_scanner_role_control_t role;
        backend_scanner_time_control_t time;
        backend_scanner_flow_control_t flow;
        backend_scanner_led_control_t led;
        backend_scanner_health_request_t health_request;
        backend_scanner_recovery_control_t recovery;
        backend_scanner_investigate_control_t investigate;
        backend_scanner_cancel_control_t cancel;
        backend_scanner_ota_begin_control_t ota_begin;
        backend_scanner_ota_finish_control_t ota_finish;
    } payload;
} backend_scanner_control_t;

typedef enum {
    BACKEND_SCANNER_CONTROL_DECODE_OK = 0,
    BACKEND_SCANNER_CONTROL_MALFORMED,
    BACKEND_SCANNER_CONTROL_SCHEMA_MISMATCH,
    BACKEND_SCANNER_CONTROL_TOO_LARGE,
} backend_scanner_control_decode_result_t;

size_t backend_scanner_control_encode(
    const backend_scanner_control_t *control,
    char *output,
    size_t capacity);

backend_scanner_control_decode_result_t backend_scanner_control_decode(
    const char *json,
    size_t length,
    backend_scanner_control_t *out);

#ifdef __cplusplus
}
#endif
