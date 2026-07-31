#pragma once

#include "badge_con_protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BADGE_CON_NVS_RECORD_BYTES 12U
#define BADGE_CON_RTC_RECORD_BYTES 20U
#define BADGE_CON_INFECTED_RESCUE_POINTS 45U

typedef struct {
    badge_con_role_t seed;
    badge_con_role_t role;
    bool active;
    uint8_t shield;
    uint8_t scar_level;
    bool dead;
    uint32_t last_decay_ms;
} badge_con_game_state_t;

typedef struct {
    badge_con_role_t seed;
    badge_con_role_t role;
    bool active;
    uint8_t shield;
    uint8_t maximum;
    uint8_t scar_level;
    bool cured;
    bool dead;
    bool super;
} badge_con_snapshot_t;

typedef enum {
    BADGE_CON_EFFECT_NONE = 0,
    BADGE_CON_EFFECT_SHIELD_GAINED,
    BADGE_CON_EFFECT_SHIELD_DRAINED,
    BADGE_CON_EFFECT_ZERO_ABSORBED,
    BADGE_CON_EFFECT_INFECTED,
    BADGE_CON_EFFECT_CURE_GAINED,
    BADGE_CON_EFFECT_CURED,
    BADGE_CON_EFFECT_DIED,
} badge_con_effect_t;

typedef enum {
    BADGE_CON_NVS_INVALID = 0,
    BADGE_CON_NVS_SEED_ONLY,
    BADGE_CON_NVS_VALID,
} badge_con_nvs_decode_result_t;

bool badge_con_role_parse_exact(const char *value, badge_con_role_t *out);
const char *badge_con_role_name(badge_con_role_t role);
void badge_con_game_defaults(badge_con_game_state_t *out);
void badge_con_game_apply_factory_seed(badge_con_game_state_t *state,
                                       badge_con_role_t seed,
                                       uint32_t now_ms);
bool badge_con_game_activate(badge_con_game_state_t *state,
                             uint32_t now_ms);
uint8_t badge_con_game_rssi_multiplier(int8_t rssi);
uint8_t badge_con_game_maximum(uint8_t scar_level);
badge_con_effect_t badge_con_game_apply_peer(
    badge_con_game_state_t *state,
    badge_con_role_t peer_role,
    bool peer_super,
    int8_t peer_rssi,
    uint32_t now_ms);
void badge_con_game_snapshot(badge_con_game_state_t *state,
                             uint32_t now_ms,
                             badge_con_snapshot_t *out);
bool badge_con_game_encode_nvs(
    const badge_con_game_state_t *state,
    uint8_t out[BADGE_CON_NVS_RECORD_BYTES]);
badge_con_nvs_decode_result_t badge_con_game_decode_nvs(
    const uint8_t *record,
    size_t record_size,
    badge_con_game_state_t *out);
bool badge_con_game_encode_rtc(
    const badge_con_game_state_t *state,
    uint32_t expected_reboot_generation,
    uint8_t out[BADGE_CON_RTC_RECORD_BYTES]);
bool badge_con_game_decode_rtc(
    const uint8_t *record,
    size_t record_size,
    uint32_t expected_reboot_generation,
    badge_con_game_state_t *out);
uint8_t badge_con_game_shield_checkpoint(uint8_t shield);

#ifdef __cplusplus
}
#endif
