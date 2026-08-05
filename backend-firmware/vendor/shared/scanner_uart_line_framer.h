#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCANNER_UART_LINE_MAX_PAYLOAD 4095U
#define SCANNER_UART_LINE_BUFFER_SIZE (SCANNER_UART_LINE_MAX_PAYLOAD + 1U)

typedef enum {
    SCANNER_UART_LINE_EVENT_NONE = 0,
    SCANNER_UART_LINE_EVENT_FRAME_READY,
    SCANNER_UART_LINE_EVENT_FRAME_REJECTED,
    SCANNER_UART_LINE_EVENT_INVALID_ARGUMENT,
} scanner_uart_line_event_kind_t;

typedef enum {
    SCANNER_UART_LINE_REJECT_NONE = 0,
    SCANNER_UART_LINE_REJECT_BARE_CR,
    SCANNER_UART_LINE_REJECT_TOO_LONG,
    SCANNER_UART_LINE_REJECT_STALE_PARTIAL,
} scanner_uart_line_reject_reason_t;

typedef enum {
    SCANNER_UART_LINE_STATE_COLLECTING = 0,
    SCANNER_UART_LINE_STATE_PENDING_CR,
    SCANNER_UART_LINE_STATE_DISCARDING,
} scanner_uart_line_state_t;

typedef struct {
    uint8_t *buffer;
    size_t byte_len;
    scanner_uart_line_state_t state;
} scanner_uart_line_framer_t;

typedef struct {
    scanner_uart_line_event_kind_t kind;
    scanner_uart_line_reject_reason_t reject_reason;
    const uint8_t *bytes;
    size_t byte_len;
} scanner_uart_line_event_t;

/**
 * Initialize a caller-owned raw line framer.
 *
 * `storage` must remain alive and provide at least 4096 bytes. The framer
 * accepts payloads through 4095 bytes and leaves the final byte untouched.
 * It never NUL-terminates an unauthorised frame; consumers must use the
 * returned span and may project it to a C string only after authorization.
 */
bool scanner_uart_line_framer_init(scanner_uart_line_framer_t *framer,
                                   uint8_t *storage,
                                   size_t storage_size);

/** Reset collection state while retaining the caller-owned storage. */
void scanner_uart_line_framer_reset(scanner_uart_line_framer_t *framer);

/**
 * Consume bytes through the first completed or rejected line.
 *
 * Empty LF/CRLF lines are skipped. `consumed_out` identifies the exact input
 * prefix consumed, so callers can retain a coalesced remainder or hand it to
 * a binary protocol after a dispatch state transition.
 */
scanner_uart_line_event_t scanner_uart_line_framer_consume(
    scanner_uart_line_framer_t *framer,
    const uint8_t *bytes,
    size_t byte_len,
    size_t *consumed_out);

/**
 * Reject a timed-out partial frame and discard input through its next LF.
 * This prevents a later suffix from being resurrected as a fresh command.
 */
scanner_uart_line_event_t scanner_uart_line_framer_expire_partial(
    scanner_uart_line_framer_t *framer);

bool scanner_uart_line_framer_has_partial(
    const scanner_uart_line_framer_t *framer);

#ifdef __cplusplus
}
#endif
