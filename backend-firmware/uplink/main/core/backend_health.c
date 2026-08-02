#include "backend_health.h"

#include <string.h>

void backend_health_evaluate(
    const backend_health_inputs_t *inputs,
    backend_health_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (inputs == NULL) {
        out->level = BACKEND_HEALTH_FATAL;
        out->led_state = BACKEND_LED_FATAL;
        return;
    }

    const unsigned usable =
        (inputs->scanner_usable[0] ? 1U : 0U) +
        (inputs->scanner_usable[1] ? 1U : 0U);
    const bool fatal = inputs->fatal_runtime || usable == 0U;
    const bool degraded = !fatal &&
        (usable < 2U || !inputs->wifi_connected ||
         !inputs->backend_reachable);
    out->level = fatal
        ? BACKEND_HEALTH_FATAL
        : degraded ? BACKEND_HEALTH_DEGRADED : BACKEND_HEALTH_HEALTHY;

    const backend_led_inputs_t led_inputs = {
        .fatal = fatal,
        .network_degraded = degraded,
        .drone_live = inputs->threats.drone_live,
        .meta_live = inputs->threats.meta_live,
    };
    out->led_state = backend_led_select(&led_inputs);
}
