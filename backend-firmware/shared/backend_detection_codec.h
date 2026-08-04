#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "detection_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_DETECTION_UART_MAX_LINE 4095U
#define BACKEND_DETECTION_EPOCH_MIN_MS INT64_C(1700000000000)

typedef enum {
    BACKEND_SCANNER_SLOT_BLE = 0,
    BACKEND_SCANNER_SLOT_WIFI = 1,
} backend_scanner_slot_t;

typedef struct {
    uint32_t sequence;
    bool time_valid;
    int64_t observed_epoch_ms;
} backend_scanner_stamp_t;

typedef struct {
    drone_detection_t detection;
    bool timestamp_valid;
    int64_t timestamp_epoch_ms;
} backend_detection_observation_t;

typedef enum {
    BACKEND_DECODE_OK,
    BACKEND_DECODE_MALFORMED,
    BACKEND_DECODE_SCHEMA_MISMATCH,
    BACKEND_DECODE_TOO_LARGE,
} backend_detection_decode_result_t;

size_t backend_detection_uart_encode(
    const drone_detection_t *detection,
    const backend_scanner_stamp_t *stamp,
    char *output,
    size_t capacity);

backend_detection_decode_result_t backend_detection_uart_decode(
    const char *json,
    size_t length,
    backend_scanner_slot_t slot,
    drone_detection_t *out_detection,
    backend_scanner_stamp_t *out_stamp);

/* Decode the newline-delimited JSON emitted by the unchanged production
 * combo scanner firmware.  That dialect reports first/last as scanner
 * monotonic milliseconds, reports an unsynchronised ts as uptime, encodes
 * probed SSIDs as either a string or an array, and reports ble_ival in
 * milliseconds.  Monotonic timestamps are normalised to zero at this
 * boundary; an epoch-valued ts remains authoritative in out_stamp. */
backend_detection_decode_result_t backend_detection_uart_decode_production(
    const char *json,
    size_t length,
    backend_scanner_slot_t slot,
    drone_detection_t *out_detection,
    backend_scanner_stamp_t *out_stamp);

/* Exact Task-3 frequency-to-channel mapping; zero means not derivable. */
int backend_detection_wifi_channel(int32_t freq_mhz);

#ifdef __cplusplus
}
#endif
