#include "scanner_command_ingress.h"

#include <string.h>

static scanner_command_ingress_result_t empty_result(void)
{
    scanner_command_ingress_result_t result = {
        .last_line_reject = SCANNER_UART_LINE_REJECT_NONE,
        .last_registry_result = FOF_SCANNER_COMMAND_REGISTRY_OK,
    };
    return result;
}

bool scanner_command_ingress_init(
    scanner_command_ingress_t *ingress,
    uint8_t *storage,
    size_t storage_size,
    fof_scanner_deployment_t deployment,
    const scanner_command_ingress_callbacks_t *callbacks,
    void *callback_context)
{
    if (!ingress || !callbacks ||
        !callbacks->binary_active ||
        !callbacks->binary_write ||
        !callbacks->authorized_frame ||
        (deployment != FOF_SCANNER_DEPLOYMENT_BADGE &&
         deployment != FOF_SCANNER_DEPLOYMENT_NON_BADGE)) {
        return false;
    }

    memset(ingress, 0, sizeof(*ingress));
    if (!scanner_uart_line_framer_init(
            &ingress->framer, storage, storage_size)) {
        return false;
    }
    ingress->deployment = deployment;
    ingress->callbacks = *callbacks;
    ingress->callback_context = callback_context;
    return true;
}

scanner_command_ingress_result_t scanner_command_ingress_consume(
    scanner_command_ingress_t *ingress,
    const uint8_t *bytes,
    size_t byte_len)
{
    scanner_command_ingress_result_t result = empty_result();
    if (!ingress || !ingress->framer.buffer ||
        (byte_len > 0U && !bytes)) {
        result.invalid_argument = true;
        return result;
    }

    size_t offset = 0U;
    while (offset < byte_len) {
        if (ingress->callbacks.binary_active(
                ingress->callback_context)) {
            size_t remainder_len = byte_len - offset;
            if (!ingress->callbacks.binary_write(
                    ingress->callback_context,
                    bytes + offset,
                    remainder_len)) {
                result.binary_write_failed = true;
            }
            result.binary_bytes += remainder_len;
            break;
        }

        size_t consumed = 0U;
        scanner_uart_line_event_t event =
            scanner_uart_line_framer_consume(
                &ingress->framer,
                bytes + offset,
                byte_len - offset,
                &consumed);
        if (consumed == 0U) {
            result.invalid_argument = true;
            break;
        }
        offset += consumed;

        if (event.kind == SCANNER_UART_LINE_EVENT_NONE) {
            continue;
        }
        if (event.kind == SCANNER_UART_LINE_EVENT_INVALID_ARGUMENT) {
            result.invalid_argument = true;
            break;
        }
        if (event.kind == SCANNER_UART_LINE_EVENT_FRAME_REJECTED) {
            result.rejected_frames++;
            result.last_line_reject = event.reject_reason;
            continue;
        }

        fof_scanner_command_decision_t decision = {0};
        fof_scanner_command_registry_result_t registry_result =
            fof_scanner_command_select_and_validate(
                event.bytes,
                event.byte_len,
                ingress->deployment,
                &decision);
        result.last_registry_result = registry_result;
        if (registry_result != FOF_SCANNER_COMMAND_REGISTRY_OK) {
            result.rejected_frames++;
            continue;
        }

        /*
         * The line framer reserves one byte beyond the 4095-byte maximum.
         * Project to a C string only after exact raw-span authorization.
         */
        ingress->framer.buffer[event.byte_len] = '\0';
        ingress->callbacks.authorized_frame(
            ingress->callback_context,
            &decision,
            event.bytes,
            event.byte_len);
        result.authorized_frames++;

        /*
         * Bytes coalesced after ota_begin are firmware bytes, never a second
         * JSON dialect. If the authorized begin did not actually arm binary
         * reception, drop the same-read remainder rather than reinterpreting
         * attacker-controlled binary as commands.
         */
        if (decision.route == FOF_SCANNER_COMMAND_ROUTE_FIRMWARE &&
            decision.firmware_schema_id ==
                FOF_FW_JSON_SCHEMA_SCANNER_OTA_BEGIN &&
            !ingress->callbacks.binary_active(
                ingress->callback_context)) {
            result.discarded_after_failed_ota_begin =
                byte_len - offset;
            break;
        }
    }

    return result;
}

scanner_command_ingress_result_t scanner_command_ingress_expire_partial(
    scanner_command_ingress_t *ingress)
{
    scanner_command_ingress_result_t result = empty_result();
    if (!ingress || !ingress->framer.buffer) {
        result.invalid_argument = true;
        return result;
    }

    scanner_uart_line_event_t event =
        scanner_uart_line_framer_expire_partial(&ingress->framer);
    if (event.kind == SCANNER_UART_LINE_EVENT_INVALID_ARGUMENT) {
        result.invalid_argument = true;
    } else if (event.kind == SCANNER_UART_LINE_EVENT_FRAME_REJECTED) {
        result.rejected_frames = 1U;
        result.last_line_reject = event.reject_reason;
    }
    return result;
}

bool scanner_command_ingress_has_partial(
    const scanner_command_ingress_t *ingress)
{
    return ingress &&
           scanner_uart_line_framer_has_partial(&ingress->framer);
}
