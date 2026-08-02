#include "backend_uart_rx.h"

#include <stddef.h>
#include <string.h>

static backend_uart_rx_result_t empty_result(void)
{
    backend_uart_rx_result_t result = {
        .last_line_reject = SCANNER_UART_LINE_REJECT_NONE,
        .last_decode_result = BACKEND_SCANNER_CONTROL_DECODE_OK,
    };
    return result;
}

static size_t bounded_length(const char *value, size_t capacity)
{
    if (value == NULL) {
        return capacity;
    }
    size_t length = 0U;
    while (length < capacity && value[length] != '\0') {
        ++length;
    }
    return length;
}

static bool valid_command_id(const char *command_id)
{
    const size_t length = bounded_length(command_id, BLE_INV_REQUEST_ID_LEN);
    if (length != BLE_INV_REQUEST_ID_LEN - 1U) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const char value = command_id[index];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool canonical_mac(const char *value)
{
    if (bounded_length(value, 18U) != 17U) {
        return false;
    }
    for (size_t index = 0U; index < 17U; ++index) {
        if ((index + 1U) % 3U == 0U) {
            if (value[index] != ':') {
                return false;
            }
        } else if (!((value[index] >= '0' && value[index] <= '9') ||
                     (value[index] >= 'A' && value[index] <= 'F'))) {
            return false;
        }
    }
    return true;
}

static bool translate_investigation(
    const backend_scanner_investigate_control_t *control,
    ble_investigation_request_t *request)
{
    if (control == NULL || request == NULL ||
        !valid_command_id(control->command_id) ||
        control->timeout_ms == 0U ||
        control->timeout_ms > BLE_INV_DEFAULT_TIMEOUT_MS) {
        return false;
    }
    memset(request, 0, sizeof(*request));
    memcpy(request->request_id, control->command_id,
           sizeof(request->request_id));
    request->timeout_ms = control->timeout_ms;

    switch (control->mode) {
    case BACKEND_SCANNER_INVESTIGATE_GATT:
        if (!control->has_mac || !canonical_mac(control->mac)) {
            return false;
        }
        request->mode = BLE_INV_MODE_GATT;
        memcpy(request->target_mac, control->mac,
               sizeof(request->target_mac));
        return true;
    case BACKEND_SCANNER_INVESTIGATE_PASSIVE_CAPTURE:
        if (control->has_mac || control->mac[0] != '\0') {
            return false;
        }
        request->mode = BLE_INV_MODE_PASSIVE_CAPTURE;
        return true;
    default:
        return false;
    }
}

static bool dispatch_generic(
    backend_uart_rx_t *rx,
    const backend_scanner_control_t *control,
    int64_t now_ms)
{
    switch (control->type) {
    case BACKEND_SCANNER_CONTROL_ROLE:
    case BACKEND_SCANNER_CONTROL_TIME:
    case BACKEND_SCANNER_CONTROL_FLOW:
    case BACKEND_SCANNER_CONTROL_LED_STATE:
    case BACKEND_SCANNER_CONTROL_HEALTH_REQUEST:
    case BACKEND_SCANNER_CONTROL_RECOVERY:
    case BACKEND_SCANNER_CONTROL_OTA_BEGIN:
    case BACKEND_SCANNER_CONTROL_OTA_END:
    case BACKEND_SCANNER_CONTROL_OTA_ABORT:
        return rx->callbacks.control(
            rx->callback_context, control, now_ms);
    case BACKEND_SCANNER_CONTROL_INVESTIGATE:
    case BACKEND_SCANNER_CONTROL_CANCEL:
    default:
        return false;
    }
}

static bool dispatch_control(
    backend_uart_rx_t *rx,
    const backend_scanner_control_t *control,
    int64_t now_ms,
    bool *semantic_valid)
{
    *semantic_valid = true;
    switch (control->type) {
    case BACKEND_SCANNER_CONTROL_INVESTIGATE: {
        ble_investigation_request_t request;
        if (!translate_investigation(
                &control->payload.investigate, &request)) {
            *semantic_valid = false;
            return false;
        }
        return rx->callbacks.investigate(
            rx->callback_context, &request, now_ms);
    }
    case BACKEND_SCANNER_CONTROL_CANCEL:
        if (!valid_command_id(control->payload.cancel.command_id)) {
            *semantic_valid = false;
            return false;
        }
        return rx->callbacks.cancel(
            rx->callback_context,
            control->payload.cancel.command_id,
            now_ms);
    case BACKEND_SCANNER_CONTROL_ROLE:
    case BACKEND_SCANNER_CONTROL_TIME:
    case BACKEND_SCANNER_CONTROL_FLOW:
    case BACKEND_SCANNER_CONTROL_LED_STATE:
    case BACKEND_SCANNER_CONTROL_HEALTH_REQUEST:
    case BACKEND_SCANNER_CONTROL_RECOVERY:
    case BACKEND_SCANNER_CONTROL_OTA_BEGIN:
    case BACKEND_SCANNER_CONTROL_OTA_END:
    case BACKEND_SCANNER_CONTROL_OTA_ABORT:
        return dispatch_generic(rx, control, now_ms);
    default:
        *semantic_valid = false;
        return false;
    }
}

bool backend_uart_rx_init(
    backend_uart_rx_t *rx,
    const backend_uart_rx_callbacks_t *callbacks,
    void *callback_context)
{
    if (rx == NULL || callbacks == NULL || callbacks->control == NULL ||
        callbacks->investigate == NULL || callbacks->cancel == NULL) {
        return false;
    }
    const backend_uart_rx_callbacks_t callback_snapshot = *callbacks;
    memset(rx, 0, sizeof(*rx));
    rx->callbacks = callback_snapshot;
    rx->callback_context = callback_context;
    return scanner_uart_line_framer_init(
        &rx->framer, rx->storage, sizeof(rx->storage));
}

backend_uart_rx_result_t backend_uart_rx_consume(
    backend_uart_rx_t *rx,
    const uint8_t *bytes,
    size_t byte_length,
    int64_t now_ms)
{
    backend_uart_rx_result_t result = empty_result();
    if (rx == NULL || rx->framer.buffer == NULL || now_ms < 0 ||
        (byte_length > 0U && bytes == NULL)) {
        result.invalid_argument = true;
        return result;
    }

    size_t offset = 0U;
    while (offset < byte_length) {
        if (rx->callbacks.binary_active != NULL &&
            rx->callbacks.binary_active(rx->callback_context)) {
            result.binary_handoff = true;
            break;
        }
        size_t consumed = 0U;
        const scanner_uart_line_event_t event =
            scanner_uart_line_framer_consume(
                &rx->framer,
                bytes + offset,
                byte_length - offset,
                &consumed);
        if (consumed == 0U) {
            result.invalid_argument = true;
            break;
        }
        offset += consumed;
        result.consumed_bytes = offset;

        if (event.kind == SCANNER_UART_LINE_EVENT_NONE) {
            continue;
        }
        if (event.kind == SCANNER_UART_LINE_EVENT_INVALID_ARGUMENT) {
            result.invalid_argument = true;
            break;
        }
        if (event.kind == SCANNER_UART_LINE_EVENT_FRAME_REJECTED) {
            ++result.rejected_frames;
            result.last_line_reject = event.reject_reason;
            continue;
        }

        backend_scanner_control_t control;
        const backend_scanner_control_decode_result_t decode_result =
            backend_scanner_control_decode(
                (const char *)event.bytes, event.byte_len, &control);
        result.last_decode_result = decode_result;
        if (decode_result != BACKEND_SCANNER_CONTROL_DECODE_OK) {
            ++result.rejected_frames;
            continue;
        }

        bool semantic_valid = false;
        const bool dispatched = dispatch_control(
            rx, &control, now_ms, &semantic_valid);
        if (!semantic_valid) {
            ++result.rejected_frames;
            result.semantic_rejection = true;
            continue;
        }
        ++result.accepted_frames;
        if (!dispatched) {
            ++result.dispatch_failures;
        }
        if (control.type == BACKEND_SCANNER_CONTROL_OTA_BEGIN) {
            const bool binary_active =
                rx->callbacks.binary_active != NULL &&
                rx->callbacks.binary_active(rx->callback_context);
            if (binary_active) {
                result.binary_handoff = offset < byte_length;
            } else {
                result.discarded_bytes = byte_length - offset;
                result.consumed_bytes = byte_length;
            }
            break;
        }
    }
    return result;
}

backend_uart_rx_result_t backend_uart_rx_expire_partial(
    backend_uart_rx_t *rx)
{
    backend_uart_rx_result_t result = empty_result();
    if (rx == NULL || rx->framer.buffer == NULL) {
        result.invalid_argument = true;
        return result;
    }
    const scanner_uart_line_event_t event =
        scanner_uart_line_framer_expire_partial(&rx->framer);
    if (event.kind == SCANNER_UART_LINE_EVENT_FRAME_REJECTED) {
        result.rejected_frames = 1U;
        result.last_line_reject = event.reject_reason;
    } else if (event.kind == SCANNER_UART_LINE_EVENT_INVALID_ARGUMENT) {
        result.invalid_argument = true;
    }
    return result;
}

bool backend_uart_rx_has_partial(const backend_uart_rx_t *rx)
{
    return rx != NULL &&
           scanner_uart_line_framer_has_partial(&rx->framer);
}
