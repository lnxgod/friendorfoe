#pragma once

#include "badge_con_protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BADGE_CON_VHCI_COMMAND_DEADLINE_MS 250U
#define BADGE_CON_VHCI_MAX_RETRIES 2U
#define BADGE_CON_VHCI_PAYLOAD_EPOCH_MS 1000U

typedef enum {
    BADGE_CON_VHCI_OFF = 0,
    BADGE_CON_VHCI_INIT_CONTROLLER,
    BADGE_CON_VHCI_SET_PARAMS,
    BADGE_CON_VHCI_SET_DATA,
    BADGE_CON_VHCI_ENABLE,
    BADGE_CON_VHCI_ADVERTISING,
    BADGE_CON_VHCI_DISABLE,
    BADGE_CON_VHCI_FAILED,
} badge_con_vhci_state_t;

/**
 * Cross-task radio admission gate.
 *
 * The firmware-operation epoch is monotonically advanced at every ownership
 * or inhibit edge. A newer epoch may replace the current decision, while an
 * equal epoch may only become more restrictive. This makes a delayed
 * display-loop permit incapable of undoing an updater's inhibit.
 */
typedef struct {
    uint32_t epoch;
    bool observed;
    bool inhibited;
} badge_con_vhci_epoch_gate_t;

typedef struct {
    badge_con_vhci_state_t state;
    bool controller_initialized;
    bool advertising;
    bool inhibited;
    bool command_in_flight;
    bool radio_state_uncertain;
    uint8_t sequence;
    uint32_t last_frame_ms;
    uint32_t command_deadline_ms;
    uint8_t retries;
    const char *failure;
} badge_con_vhci_snapshot_t;

typedef bool (*badge_con_vhci_send_fn)(
    void *context, const uint8_t *bytes, size_t size);

typedef struct {
    void *context;
    badge_con_vhci_send_fn send;
} badge_con_vhci_transport_t;

typedef struct {
    badge_con_vhci_state_t state;
    badge_con_vhci_transport_t transport;
    badge_con_role_t role;
    uint32_t peer;
    uint32_t last_frame_ms;
    uint32_t command_deadline_ms;
    uint16_t pending_opcode;
    uint8_t session;
    uint8_t sequence;
    uint8_t retries;
    bool super;
    bool controller_initialized;
    bool game_active;
    bool self_ready;
    bool inhibited;
    bool advertising;
    bool command_in_flight;
    bool refresh_while_advertising;
    bool radio_state_uncertain;
    const char *failure;
} badge_con_vhci_policy_t;

void badge_con_vhci_policy_init(
    badge_con_vhci_policy_t *policy,
    uint32_t peer,
    uint8_t session,
    uint8_t initial_sequence,
    const badge_con_vhci_transport_t *transport);
void badge_con_vhci_policy_set_controller_initialized(
    badge_con_vhci_policy_t *policy, bool initialized);
void badge_con_vhci_policy_set_identity_state(
    badge_con_vhci_policy_t *policy,
    badge_con_role_t role,
    bool super);
void badge_con_vhci_policy_set_game_active(
    badge_con_vhci_policy_t *policy, bool active);
void badge_con_vhci_policy_set_self_ready(
    badge_con_vhci_policy_t *policy, bool ready);
void badge_con_vhci_policy_set_inhibited(
    badge_con_vhci_policy_t *policy, bool inhibited);
void badge_con_vhci_policy_on_hci_event(
    badge_con_vhci_policy_t *policy,
    const uint8_t *bytes,
    size_t size,
    uint32_t now_ms);
void badge_con_vhci_policy_poll(
    badge_con_vhci_policy_t *policy, uint32_t now_ms);
void badge_con_vhci_policy_fail(
    badge_con_vhci_policy_t *policy, const char *failure);
void badge_con_vhci_policy_snapshot(
    const badge_con_vhci_policy_t *policy,
    badge_con_vhci_snapshot_t *out);
bool badge_con_vhci_policy_radio_quiesced(
    const badge_con_vhci_policy_t *policy);
bool badge_con_vhci_epoch_gate_apply(
    badge_con_vhci_epoch_gate_t *gate,
    uint32_t epoch,
    bool inhibited);
bool badge_con_vhci_epoch_gate_matches_inhibit(
    const badge_con_vhci_epoch_gate_t *gate,
    uint32_t epoch);

#ifdef __cplusplus
}
#endif
