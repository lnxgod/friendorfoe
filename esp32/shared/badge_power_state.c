#include "badge_power_state.h"

#include <string.h>

static bool scanner_id_valid(int scanner_id)
{
    return scanner_id >= 0 && scanner_id < BADGE_POWER_SCANNER_COUNT;
}

void badge_power_state_init(badge_power_state_t *state)
{
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
}

bool badge_power_state_request(badge_power_state_t *state, bool quiet)
{
    if (!state || state->quiet == quiet) {
        return false;
    }

    state->quiet = quiet;
    state->generation++;
    if (state->generation == 0U) {
        state->generation = 1U;
    }
    for (int i = 0; i < BADGE_POWER_SCANNER_COUNT; i++) {
        state->scanners[i].acked = false;
    }
    return true;
}

void badge_power_state_note_identity(badge_power_state_t *state, int scanner_id)
{
    if (!state || !scanner_id_valid(scanner_id)) {
        return;
    }
    state->scanners[scanner_id].connected = true;
    state->scanners[scanner_id].acked = false;
}

void badge_power_state_note_disconnected(badge_power_state_t *state,
                                         int scanner_id)
{
    if (!state || !scanner_id_valid(scanner_id)) {
        return;
    }
    badge_power_scanner_state_t *scanner = &state->scanners[scanner_id];
    scanner->connected = false;
    scanner->acked = false;
}

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
                                bool uart_commands)
{
    if (!state || !scanner_id_valid(scanner_id)) {
        return false;
    }

    badge_power_scanner_state_t *scanner = &state->scanners[scanner_id];
    scanner->connected = true;
    scanner->transition_ok = transition_ok;
    scanner->quiet = quiet;
    scanner->tx_enabled = tx_enabled;
    scanner->ble_scanning = ble_scanning;
    scanner->wifi_paused = wifi_paused;
    scanner->ble_quiesced = ble_quiesced;
    scanner->wifi_quiesced = wifi_quiesced;
    scanner->ble_active = ble_active;
    scanner->wifi_active = wifi_active;
    scanner->radios_ready = radios_ready;
    scanner->tx_restored = tx_restored;
    scanner->uart_commands = uart_commands;
    scanner->ack_generation = generation;
    bool mode_facts_ok = quiet
        ? (!tx_enabled && !ble_scanning && wifi_paused &&
           ble_quiesced && wifi_quiesced && !ble_active && !wifi_active &&
           radios_ready)
        : (radios_ready && tx_restored);
    scanner->acked = transition_ok &&
        generation == state->generation && quiet == state->quiet &&
        uart_commands && mode_facts_ok;
    return scanner->acked;
}

bool badge_power_state_converged(const badge_power_state_t *state)
{
    if (!state) {
        return false;
    }
    for (int i = 0; i < BADGE_POWER_SCANNER_COUNT; i++) {
        if (!state->scanners[i].connected || !state->scanners[i].acked) {
            return false;
        }
    }
    return true;
}
