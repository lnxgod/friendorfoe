#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "badge_power_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize volatile ACTIVE state. Quiet mode is intentionally not persisted. */
void badge_power_runtime_init(void);

/** Enable scanner convergence after UART RX has started. */
bool badge_power_runtime_start(void);

/** Run display/scanner power convergence work from the badge display task. */
void badge_power_runtime_poll(void);

bool badge_power_runtime_request(bool quiet, const char *source);
bool badge_power_runtime_toggle(const char *source);
bool badge_power_runtime_is_quiet(void);
const char *badge_power_runtime_mode_name(void);
void badge_power_runtime_snapshot(badge_power_state_t *out);

void badge_power_runtime_note_scanner_identity(int scanner_id);
bool badge_power_runtime_note_scanner_ack(int scanner_id,
                                          bool transition_ok,
                                          bool quiet,
                                          uint32_t generation,
                                          bool tx_enabled,
                                          bool ble_scanning,
                                          bool wifi_paused,
                                          bool ble_quiesced,
                                          bool wifi_quiesced,
                                          bool ble_active,
                                          bool wifi_active,
                                          bool radios_ready,
                                          bool tx_restored,
                                          bool uart_commands);

#ifdef __cplusplus
}
#endif
