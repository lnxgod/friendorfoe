#include "scanner_uart_line_framer.h"

static scanner_uart_line_event_t line_event(
    scanner_uart_line_event_kind_t kind,
    scanner_uart_line_reject_reason_t reject_reason,
    const uint8_t *bytes,
    size_t byte_len)
{
    scanner_uart_line_event_t event = {
        .kind = kind,
        .reject_reason = reject_reason,
        .bytes = bytes,
        .byte_len = byte_len,
    };
    return event;
}

static scanner_uart_line_event_t no_event(void)
{
    return line_event(
        SCANNER_UART_LINE_EVENT_NONE,
        SCANNER_UART_LINE_REJECT_NONE,
        NULL,
        0U);
}

static scanner_uart_line_event_t invalid_argument_event(void)
{
    return line_event(
        SCANNER_UART_LINE_EVENT_INVALID_ARGUMENT,
        SCANNER_UART_LINE_REJECT_NONE,
        NULL,
        0U);
}

static scanner_uart_line_event_t rejected_event(
    scanner_uart_line_reject_reason_t reason)
{
    return line_event(
        SCANNER_UART_LINE_EVENT_FRAME_REJECTED,
        reason,
        NULL,
        0U);
}

bool scanner_uart_line_framer_init(scanner_uart_line_framer_t *framer,
                                   uint8_t *storage,
                                   size_t storage_size)
{
    if (!framer || !storage ||
        storage_size < SCANNER_UART_LINE_BUFFER_SIZE) {
        return false;
    }
    framer->buffer = storage;
    framer->byte_len = 0U;
    framer->state = SCANNER_UART_LINE_STATE_COLLECTING;
    return true;
}

void scanner_uart_line_framer_reset(scanner_uart_line_framer_t *framer)
{
    if (!framer || !framer->buffer) {
        return;
    }
    framer->byte_len = 0U;
    framer->state = SCANNER_UART_LINE_STATE_COLLECTING;
}

scanner_uart_line_event_t scanner_uart_line_framer_consume(
    scanner_uart_line_framer_t *framer,
    const uint8_t *bytes,
    size_t byte_len,
    size_t *consumed_out)
{
    if (consumed_out) {
        *consumed_out = 0U;
    }
    if (!framer || !framer->buffer || (byte_len > 0U && !bytes)) {
        return invalid_argument_event();
    }

    size_t consumed = 0U;
    while (consumed < byte_len) {
        uint8_t byte = bytes[consumed++];
        if (consumed_out) {
            *consumed_out = consumed;
        }

        if (framer->state == SCANNER_UART_LINE_STATE_DISCARDING) {
            if (byte == '\n') {
                framer->state = SCANNER_UART_LINE_STATE_COLLECTING;
                framer->byte_len = 0U;
            }
            continue;
        }

        if (framer->state == SCANNER_UART_LINE_STATE_PENDING_CR) {
            if (byte == '\n') {
                framer->state = SCANNER_UART_LINE_STATE_COLLECTING;
                if (framer->byte_len == 0U) {
                    continue;
                }
                size_t frame_len = framer->byte_len;
                framer->byte_len = 0U;
                return line_event(
                    SCANNER_UART_LINE_EVENT_FRAME_READY,
                    SCANNER_UART_LINE_REJECT_NONE,
                    framer->buffer,
                    frame_len);
            }

            framer->state = SCANNER_UART_LINE_STATE_DISCARDING;
            framer->byte_len = 0U;
            return rejected_event(SCANNER_UART_LINE_REJECT_BARE_CR);
        }

        if (byte == '\n') {
            if (framer->byte_len == 0U) {
                continue;
            }
            size_t frame_len = framer->byte_len;
            framer->byte_len = 0U;
            return line_event(
                SCANNER_UART_LINE_EVENT_FRAME_READY,
                SCANNER_UART_LINE_REJECT_NONE,
                framer->buffer,
                frame_len);
        }
        if (byte == '\r') {
            framer->state = SCANNER_UART_LINE_STATE_PENDING_CR;
            continue;
        }
        if (framer->byte_len >= SCANNER_UART_LINE_MAX_PAYLOAD) {
            framer->state = SCANNER_UART_LINE_STATE_DISCARDING;
            framer->byte_len = 0U;
            return rejected_event(SCANNER_UART_LINE_REJECT_TOO_LONG);
        }
        framer->buffer[framer->byte_len++] = byte;
    }

    return no_event();
}

scanner_uart_line_event_t scanner_uart_line_framer_expire_partial(
    scanner_uart_line_framer_t *framer)
{
    if (!framer || !framer->buffer) {
        return invalid_argument_event();
    }
    if (!scanner_uart_line_framer_has_partial(framer)) {
        return no_event();
    }

    framer->byte_len = 0U;
    framer->state = SCANNER_UART_LINE_STATE_DISCARDING;
    return rejected_event(SCANNER_UART_LINE_REJECT_STALE_PARTIAL);
}

bool scanner_uart_line_framer_has_partial(
    const scanner_uart_line_framer_t *framer)
{
    if (!framer || !framer->buffer) {
        return false;
    }
    return framer->state == SCANNER_UART_LINE_STATE_PENDING_CR ||
           (framer->state == SCANNER_UART_LINE_STATE_COLLECTING &&
            framer->byte_len > 0U);
}
