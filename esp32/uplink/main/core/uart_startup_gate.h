#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "firmware_operation_token.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UART_STARTUP_GATE_OK = 0,
    UART_STARTUP_GATE_CLAIM_TIMEOUT,
    UART_STARTUP_GATE_START_FAILED,
    UART_STARTUP_GATE_RELEASE_FAILED,
} uart_startup_gate_result_t;

typedef struct {
    void *context;
    bool (*try_claim)(void *context, fw_operation_token_t *out);
    bool (*start)(void *context);
    bool (*release)(void *context, fw_operation_token_t token);
    void (*delay)(void *context, uint32_t delay_ms);
} uart_startup_gate_hooks_t;

uart_startup_gate_result_t uart_startup_gate_run(
    const uart_startup_gate_hooks_t *hooks,
    uint32_t claim_attempts,
    uint32_t claim_retry_ms,
    uint32_t release_attempts,
    uint32_t release_retry_ms);

#ifdef __cplusplus
}
#endif
