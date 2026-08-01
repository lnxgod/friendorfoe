#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BADGE_POWER_SCANNER_COUNT 2

typedef struct {
    bool connected;
    bool acked;
    bool transition_ok;
    bool quiet;
    bool tx_enabled;
    bool ble_scanning;
    bool wifi_paused;
    bool ble_quiesced;
    bool wifi_quiesced;
    bool ble_active;
    bool wifi_active;
    bool radios_ready;
    bool tx_restored;
    bool uart_commands;
    uint32_t ack_generation;
} badge_power_scanner_state_t;

typedef struct {
    bool quiet;
    uint32_t generation;
    badge_power_scanner_state_t scanners[BADGE_POWER_SCANNER_COUNT];
} badge_power_state_t;

void badge_power_state_init(badge_power_state_t *state);
bool badge_power_state_request(badge_power_state_t *state, bool quiet);
void badge_power_state_note_identity(badge_power_state_t *state, int scanner_id);
void badge_power_state_note_disconnected(badge_power_state_t *state,
                                         int scanner_id);
bool badge_power_state_note_ack(badge_power_state_t *state,
                                int scanner_id,
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
bool badge_power_state_converged(const badge_power_state_t *state);

#ifdef __cplusplus
}
#endif
