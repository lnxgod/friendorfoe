#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_uart_protocol.h"
#include "ble_investigation_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Vendored BLE JSON is bounded to UART_JSON_MAX_SIZE including its NUL.
 * One additional byte holds the newline while preserving NUL termination. */
#define BACKEND_UART_TX_LINE_CAPACITY (UART_JSON_MAX_SIZE + 1U)

typedef bool (*backend_uart_tx_write_fn)(
    void *context,
    const uint8_t *bytes,
    size_t length);

typedef struct {
    backend_uart_tx_write_fn write;
    void *write_context;
    char line[BACKEND_UART_TX_LINE_CAPACITY];
    size_t last_length;
    uint32_t sent_chunks;
    uint32_t dropped_chunks;
} backend_uart_tx_t;

bool backend_uart_tx_init(
    backend_uart_tx_t *tx,
    backend_uart_tx_write_fn write,
    void *write_context);

/* Returns the newline-inclusive byte length, or zero on validation/space
 * failure. The returned span is also NUL-terminated for diagnostics. */
size_t backend_uart_tx_encode_investigation(
    const ble_investigation_chunk_t *chunk,
    char *output,
    size_t capacity);

/* Performs exactly one caller-supplied UART write and never waits on HTTP. */
bool backend_uart_tx_send_investigation(
    backend_uart_tx_t *tx,
    const ble_investigation_chunk_t *chunk);

#ifdef __cplusplus
}
#endif
