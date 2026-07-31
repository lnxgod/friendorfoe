#include "uart_startup_gate.h"

uart_startup_gate_result_t uart_startup_gate_run(
    const uart_startup_gate_hooks_t *hooks,
    uint32_t claim_attempts,
    uint32_t claim_retry_ms,
    uint32_t release_attempts,
    uint32_t release_retry_ms)
{
    if (!hooks || !hooks->try_claim || !hooks->start || !hooks->release ||
        !hooks->delay || claim_attempts == 0U || release_attempts == 0U) {
        return UART_STARTUP_GATE_CLAIM_TIMEOUT;
    }

    fw_operation_token_t token = {0};
    bool claimed = false;
    for (uint32_t attempt = 0U; attempt < claim_attempts; ++attempt) {
        if (hooks->try_claim(hooks->context, &token)) {
            claimed = true;
            break;
        }
        if (attempt + 1U < claim_attempts) {
            hooks->delay(hooks->context, claim_retry_ms);
        }
    }
    if (!claimed) {
        return UART_STARTUP_GATE_CLAIM_TIMEOUT;
    }

    bool started = hooks->start(hooks->context);
    for (uint32_t attempt = 0U; attempt < release_attempts; ++attempt) {
        if (hooks->release(hooks->context, token)) {
            return started ? UART_STARTUP_GATE_OK
                           : UART_STARTUP_GATE_START_FAILED;
        }
        if (attempt + 1U < release_attempts) {
            hooks->delay(hooks->context, release_retry_ms);
        }
    }
    return UART_STARTUP_GATE_RELEASE_FAILED;
}
