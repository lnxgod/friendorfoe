#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ble_investigation_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_UART_INVESTIGATION_MAX_JSON 4095U

typedef enum {
    BACKEND_UART_INVESTIGATION_DECODE_OK = 0,
    BACKEND_UART_INVESTIGATION_DECODE_MALFORMED,
    BACKEND_UART_INVESTIGATION_DECODE_SCHEMA_MISMATCH,
    BACKEND_UART_INVESTIGATION_DECODE_TOO_LARGE,
} backend_uart_investigation_decode_result_t;

/* Decode one scanner ble_inv_* JSON object, optionally followed by one LF. */
backend_uart_investigation_decode_result_t
backend_uart_investigation_decode(
    const uint8_t *line,
    size_t length,
    ble_investigation_chunk_t *out);

#ifdef __cplusplus
}
#endif
