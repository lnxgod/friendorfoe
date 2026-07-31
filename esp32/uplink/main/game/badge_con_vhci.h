#pragma once

#include "badge_con_game.h"
#include "badge_con_vhci_policy.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool badge_con_vhci_prepare(void);
bool badge_con_vhci_init(uint32_t peer, uint8_t session);
void badge_con_vhci_set_identity_state(
    badge_con_role_t role, bool super);
void badge_con_vhci_set_game_active(bool active);
void badge_con_vhci_set_self_ready(bool ready);
bool badge_con_vhci_apply_radio_policy(
    bool inhibited, uint32_t operation_epoch);
bool badge_con_vhci_request_quiescence(uint32_t operation_epoch);
void badge_con_vhci_poll(uint32_t now_ms);
void badge_con_vhci_snapshot(badge_con_vhci_snapshot_t *out);
bool badge_con_vhci_radio_quiesced_for_epoch(uint32_t operation_epoch);

#ifdef __cplusplus
}
#endif
