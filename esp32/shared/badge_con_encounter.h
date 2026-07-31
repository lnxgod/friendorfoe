#pragma once

#include "badge_con_protocol.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BADGE_CON_PEER_CAPACITY 8U
#define BADGE_CON_QUORUM_PACKETS 3U
#define BADGE_CON_QUORUM_WINDOW_MS 6000U
#define BADGE_CON_EFFECT_RATE_MS 8000U
#define BADGE_CON_MIN_RSSI (-60)

typedef enum {
    BADGE_CON_OBSERVE_DROPPED_WEAK = 0,
    BADGE_CON_OBSERVE_DROPPED_SELF,
    BADGE_CON_OBSERVE_DROPPED_DUPLICATE,
    BADGE_CON_OBSERVE_DROPPED_TABLE_FULL,
    BADGE_CON_OBSERVE_COUNTED,
    BADGE_CON_OBSERVE_QUALIFIED,
    BADGE_CON_OBSERVE_RATE_LIMITED,
} badge_con_observe_result_t;

typedef struct {
    bool used;
    uint32_t peer;
    uint8_t session;
    uint8_t recent_sequence[BADGE_CON_QUORUM_PACKETS];
    uint32_t recent_ms[BADGE_CON_QUORUM_PACKETS];
    uint8_t recent_count;
    uint32_t last_seen_ms;
    uint8_t last_emitted_sequence;
    uint32_t last_emitted_ms;
    bool emitted;
} badge_con_peer_entry_t;

typedef struct {
    uint32_t self_peer;
    uint8_t self_session;
    bool self_valid;
    badge_con_peer_entry_t peer[BADGE_CON_PEER_CAPACITY];
} badge_con_encounter_table_t;

void badge_con_encounter_init(badge_con_encounter_table_t *table);
bool badge_con_encounter_set_self(badge_con_encounter_table_t *table,
                                  uint32_t peer,
                                  uint8_t session);
badge_con_observe_result_t badge_con_encounter_consume(
    badge_con_encounter_table_t *table,
    const badge_con_packet_t *packet,
    uint32_t now_ms);

#ifdef __cplusplus
}
#endif
