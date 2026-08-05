#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_scanner_control_codec.h"
#include "ble_investigation_types.h"
#include "scanner_uart_line_framer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*backend_uart_rx_control_fn)(
    void *context,
    const backend_scanner_control_t *control,
    int64_t now_ms);

typedef bool (*backend_uart_rx_investigate_fn)(
    void *context,
    const ble_investigation_request_t *request,
    int64_t now_ms);

typedef bool (*backend_uart_rx_cancel_fn)(
    void *context,
    const char *command_id,
    int64_t now_ms);

/* Optional synchronous state probe. It must become true only after the OTA
 * begin callback has successfully armed the binary receiver. */
typedef bool (*backend_uart_rx_binary_active_fn)(void *context);

typedef struct {
    backend_uart_rx_control_fn control;
    backend_uart_rx_investigate_fn investigate;
    backend_uart_rx_cancel_fn cancel;
    backend_uart_rx_binary_active_fn binary_active;
} backend_uart_rx_callbacks_t;

/* Keep backend_uart_rx_t in application-static storage and do not copy it
 * after initialization: the vendored framer points at the embedded buffer.
 * Callbacks execute synchronously and must remain bounded/non-HTTP. */
typedef struct {
    scanner_uart_line_framer_t framer;
    uint8_t storage[SCANNER_UART_LINE_BUFFER_SIZE];
    backend_uart_rx_callbacks_t callbacks;
    void *callback_context;
} backend_uart_rx_t;

typedef struct {
    size_t accepted_frames;
    size_t rejected_frames;
    size_t dispatch_failures;
    size_t consumed_bytes;
    size_t discarded_bytes;
    scanner_uart_line_reject_reason_t last_line_reject;
    backend_scanner_control_decode_result_t last_decode_result;
    bool semantic_rejection;
    bool invalid_argument;
    bool binary_handoff;
} backend_uart_rx_result_t;

bool backend_uart_rx_init(
    backend_uart_rx_t *rx,
    const backend_uart_rx_callbacks_t *callbacks,
    void *callback_context);

/* Command ingress is independent of detection flow pause. If an accepted
 * ota_begin arms binary mode, parsing stops at that JSON line boundary,
 * binary_handoff is set, and the caller must retain bytes beginning at
 * consumed_bytes for the OTA binary receiver. If ota_begin does not arm the
 * receiver, its same-read remainder is consumed without reinterpretation and
 * reported in discarded_bytes. */
backend_uart_rx_result_t backend_uart_rx_consume(
    backend_uart_rx_t *rx,
    const uint8_t *bytes,
    size_t byte_length,
    int64_t now_ms);

backend_uart_rx_result_t backend_uart_rx_expire_partial(
    backend_uart_rx_t *rx);

bool backend_uart_rx_has_partial(const backend_uart_rx_t *rx);

#ifdef __cplusplus
}
#endif
