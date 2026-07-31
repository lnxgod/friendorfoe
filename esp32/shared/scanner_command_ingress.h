#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "scanner_command_schema_registry.h"
#include "scanner_uart_line_framer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*scanner_command_ingress_binary_active_fn)(void *context);
typedef bool (*scanner_command_ingress_binary_write_fn)(
    void *context,
    const uint8_t *bytes,
    size_t byte_len);
typedef void (*scanner_command_ingress_authorized_frame_fn)(
    void *context,
    const fof_scanner_command_decision_t *decision,
    const uint8_t *bytes,
    size_t byte_len);

typedef struct {
    scanner_command_ingress_binary_active_fn binary_active;
    scanner_command_ingress_binary_write_fn binary_write;
    scanner_command_ingress_authorized_frame_fn authorized_frame;
} scanner_command_ingress_callbacks_t;

typedef struct {
    scanner_uart_line_framer_t framer;
    fof_scanner_deployment_t deployment;
    scanner_command_ingress_callbacks_t callbacks;
    void *callback_context;
} scanner_command_ingress_t;

typedef struct {
    size_t authorized_frames;
    size_t rejected_frames;
    size_t binary_bytes;
    size_t discarded_after_failed_ota_begin;
    scanner_uart_line_reject_reason_t last_line_reject;
    fof_scanner_command_registry_result_t last_registry_result;
    bool binary_write_failed;
    bool invalid_argument;
} scanner_command_ingress_result_t;

/**
 * Initialize scanner command ingress over caller-owned 4096-byte storage.
 *
 * The callbacks are copied. No callback is invoked for a framing, schema, or
 * semantic rejection. `authorized_frame` receives a NUL-terminated projection
 * only after the exact raw span is authorized.
 */
bool scanner_command_ingress_init(
    scanner_command_ingress_t *ingress,
    uint8_t *storage,
    size_t storage_size,
    fof_scanner_deployment_t deployment,
    const scanner_command_ingress_callbacks_t *callbacks,
    void *callback_context);

/**
 * Consume an arbitrary UART read. If an authorized JSON frame switches the
 * receiver into binary mode, all remaining bytes from the same read are
 * delivered directly to `binary_write`.
 */
scanner_command_ingress_result_t scanner_command_ingress_consume(
    scanner_command_ingress_t *ingress,
    const uint8_t *bytes,
    size_t byte_len);

scanner_command_ingress_result_t scanner_command_ingress_expire_partial(
    scanner_command_ingress_t *ingress);

bool scanner_command_ingress_has_partial(
    const scanner_command_ingress_t *ingress);

#ifdef __cplusplus
}
#endif
