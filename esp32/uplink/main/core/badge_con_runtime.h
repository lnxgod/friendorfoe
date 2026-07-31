#pragma once

#include "badge_con_game.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void badge_con_runtime_init(void);
bool badge_con_runtime_set_factory_seed(badge_con_role_t seed);
bool badge_con_runtime_activate_after_easter(void);
badge_con_effect_t badge_con_runtime_apply_qualified_peer(
    const badge_con_packet_t *packet);
bool badge_con_runtime_snapshot(badge_con_snapshot_t *out);
bool badge_con_runtime_identity(uint32_t *peer_out, uint8_t *session_out);
bool badge_con_runtime_sequence_start(uint8_t *sequence_out);
bool badge_con_runtime_self_ack_matches(uint32_t peer, uint8_t session);
void badge_con_runtime_note_self_ack(uint32_t peer, uint8_t session);
void badge_con_runtime_clear_self_ack(void);

#ifdef UNIT_TESTING
void badge_con_runtime_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif
