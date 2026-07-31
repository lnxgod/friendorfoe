#include "badge_con_game.h"

#include <string.h>

static bool role_is_valid(badge_con_role_t role)
{
    return role >= BADGE_CON_ROLE_NORMAL &&
        role <= BADGE_CON_ROLE_IMMUNE;
}

static const uint8_t BADGE_CON_MAXIMA[] = {
    100U, 50U, 25U, 12U, 6U, 3U, 1U,
};

uint8_t badge_con_game_maximum(uint8_t scar_level)
{
    if (scar_level >= sizeof(BADGE_CON_MAXIMA)) {
        return 0U;
    }
    return BADGE_CON_MAXIMA[scar_level];
}

static bool state_is_reachable(const badge_con_game_state_t *state)
{
    if (!state || !role_is_valid(state->seed) ||
        !role_is_valid(state->role) || state->shield > 100U ||
        badge_con_game_maximum(state->scar_level) == 0U) {
        return false;
    }
    if (!state->active) {
        return state->role == state->seed && state->shield == 0U &&
            state->scar_level == 0U && !state->dead;
    }
    if (state->dead) {
        return state->role == BADGE_CON_ROLE_INFECTED &&
            state->shield == 0U;
    }
    if (state->role == BADGE_CON_ROLE_IMMUNE) {
        return state->seed == BADGE_CON_ROLE_IMMUNE &&
            state->scar_level == 0U;
    }
    if (state->seed == BADGE_CON_ROLE_IMMUNE ||
        state->scar_level >= 6U) {
        return false;
    }
    if (state->role == BADGE_CON_ROLE_INFECTED) {
        return state->shield > 0U && state->shield < 100U;
    }
    return state->shield > 0U &&
        state->shield <= badge_con_game_maximum(state->scar_level);
}

static uint32_t crc32_ieee(const uint8_t *bytes, size_t byte_count)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < byte_count; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8U; ++bit) {
            uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return crc ^ UINT32_MAX;
}

static void put_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8U);
    out[2] = (uint8_t)(value >> 16U);
    out[3] = (uint8_t)(value >> 24U);
}

static uint32_t get_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8U) |
        ((uint32_t)bytes[2] << 16U) |
        ((uint32_t)bytes[3] << 24U);
}

bool badge_con_role_parse_exact(const char *value, badge_con_role_t *out)
{
    if (!value || !out) {
        return false;
    }
    badge_con_role_t parsed;
    if (strcmp(value, "normal") == 0) {
        parsed = BADGE_CON_ROLE_NORMAL;
    } else if (strcmp(value, "infected") == 0) {
        parsed = BADGE_CON_ROLE_INFECTED;
    } else if (strcmp(value, "immune") == 0) {
        parsed = BADGE_CON_ROLE_IMMUNE;
    } else {
        return false;
    }
    *out = parsed;
    return true;
}

const char *badge_con_role_name(badge_con_role_t role)
{
    switch (role) {
        case BADGE_CON_ROLE_NORMAL:
            return "normal";
        case BADGE_CON_ROLE_INFECTED:
            return "infected";
        case BADGE_CON_ROLE_IMMUNE:
            return "immune";
        default:
            return NULL;
    }
}

void badge_con_game_defaults(badge_con_game_state_t *out)
{
    if (!out) {
        return;
    }
    *out = (badge_con_game_state_t) {
        .seed = BADGE_CON_ROLE_NORMAL,
        .role = BADGE_CON_ROLE_NORMAL,
        .active = false,
        .shield = 0U,
        .scar_level = 0U,
        .dead = false,
        .last_decay_ms = 0U,
    };
}

void badge_con_game_apply_factory_seed(badge_con_game_state_t *state,
                                       badge_con_role_t seed,
                                       uint32_t now_ms)
{
    if (!state) {
        return;
    }
    if (!role_is_valid(seed)) {
        seed = BADGE_CON_ROLE_NORMAL;
    }
    *state = (badge_con_game_state_t) {
        .seed = seed,
        .role = seed,
        .active = false,
        .shield = 0U,
        .scar_level = 0U,
        .dead = false,
        .last_decay_ms = now_ms,
    };
}

bool badge_con_game_activate(badge_con_game_state_t *state,
                             uint32_t now_ms)
{
    if (!state || state->active || !state_is_reachable(state)) {
        return false;
    }
    state->active = true;
    state->last_decay_ms = now_ms;
    if (state->seed == BADGE_CON_ROLE_IMMUNE) {
        state->shield = 100U;
    } else if (state->seed == BADGE_CON_ROLE_NORMAL) {
        state->shield = 30U;
    } else {
        state->shield = BADGE_CON_INFECTED_RESCUE_POINTS;
    }
    return true;
}

uint8_t badge_con_game_rssi_multiplier(int8_t rssi)
{
    if (rssi >= -45 && rssi <= -1) {
        return 3U;
    }
    if (rssi >= -52 && rssi <= -46) {
        return 2U;
    }
    if (rssi >= -60 && rssi <= -53) {
        return 1U;
    }
    return 0U;
}

static void apply_lazy_time(badge_con_game_state_t *state, uint32_t now_ms)
{
    if (!state || !state->active || state->dead) {
        return;
    }

    uint32_t elapsed_ms = now_ms - state->last_decay_ms;
    if (state->role != BADGE_CON_ROLE_INFECTED) {
        return;
    }

    uint32_t whole_minutes = elapsed_ms / 60000U;
    if (whole_minutes == 0U) {
        return;
    }
    uint8_t decay = whole_minutes >= state->shield
        ? state->shield
        : (uint8_t)whole_minutes;
    state->shield = (uint8_t)(state->shield - decay);
    state->last_decay_ms += whole_minutes * 60000U;
    if (state->shield == 0U) {
        state->dead = true;
    }
}

static uint8_t add_clamped(uint8_t value, unsigned increment, uint8_t maximum)
{
    unsigned raised = (unsigned)value + increment;
    return raised > maximum ? maximum : (uint8_t)raised;
}

static bool peer_claim_is_valid(badge_con_role_t role, bool super)
{
    return role_is_valid(role) &&
        (!super || role == BADGE_CON_ROLE_INFECTED);
}

badge_con_effect_t badge_con_game_apply_peer(
    badge_con_game_state_t *state,
    badge_con_role_t peer_role,
    bool peer_super,
    int8_t peer_rssi,
    uint32_t now_ms)
{
    if (!state || !state->active ||
        !state_is_reachable(state) ||
        !peer_claim_is_valid(peer_role, peer_super)) {
        return BADGE_CON_EFFECT_NONE;
    }

    uint8_t multiplier = badge_con_game_rssi_multiplier(peer_rssi);
    if (multiplier == 0U || state->dead) {
        return BADGE_CON_EFFECT_NONE;
    }

    apply_lazy_time(state, now_ms);
    if (state->dead) {
        return BADGE_CON_EFFECT_DIED;
    }
    if (state->role == BADGE_CON_ROLE_IMMUNE) {
        if (peer_role == BADGE_CON_ROLE_IMMUNE) {
            uint8_t before = state->shield;
            state->shield = add_clamped(
                state->shield, 5U * multiplier, 100U);
            return state->shield == before
                ? BADGE_CON_EFFECT_NONE
                : BADGE_CON_EFFECT_SHIELD_GAINED;
        }
        if (peer_role != BADGE_CON_ROLE_INFECTED) {
            return BADGE_CON_EFFECT_NONE;
        }
        unsigned damage =
            (peer_super ? 20U : 10U) * multiplier;
        if (damage >= state->shield) {
            state->shield = 0U;
            state->role = BADGE_CON_ROLE_INFECTED;
            state->dead = true;
            state->last_decay_ms = now_ms;
            return BADGE_CON_EFFECT_DIED;
        }
        state->shield = (uint8_t)(state->shield - damage);
        return BADGE_CON_EFFECT_SHIELD_DRAINED;
    }

    if (state->role == BADGE_CON_ROLE_NORMAL) {
        uint8_t maximum = badge_con_game_maximum(state->scar_level);
        if (peer_role == BADGE_CON_ROLE_IMMUNE) {
            uint8_t before = state->shield;
            state->shield = add_clamped(
                state->shield, 5U * multiplier, maximum);
            return state->shield == before
                ? BADGE_CON_EFFECT_NONE
                : BADGE_CON_EFFECT_SHIELD_GAINED;
        }
        if (peer_role != BADGE_CON_ROLE_INFECTED) {
            return BADGE_CON_EFFECT_NONE;
        }
        unsigned damage =
            (peer_super ? 20U : 10U) * multiplier;
        if (damage >= state->shield) {
            state->shield = BADGE_CON_INFECTED_RESCUE_POINTS;
            state->role = BADGE_CON_ROLE_INFECTED;
            state->last_decay_ms = now_ms;
            return BADGE_CON_EFFECT_INFECTED;
        }
        state->shield = (uint8_t)(state->shield - damage);
        return BADGE_CON_EFFECT_SHIELD_DRAINED;
    }

    if (peer_role == BADGE_CON_ROLE_INFECTED) {
        unsigned damage =
            (peer_super ? 2U : 1U) * multiplier;
        if (damage >= state->shield) {
            state->shield = 0U;
            state->dead = true;
            return BADGE_CON_EFFECT_DIED;
        }
        state->shield = (uint8_t)(state->shield - damage);
        return BADGE_CON_EFFECT_SHIELD_DRAINED;
    }

    if (peer_role != BADGE_CON_ROLE_IMMUNE) {
        return BADGE_CON_EFFECT_NONE;
    }

    unsigned cure = (unsigned)state->shield + 12U * multiplier;
    if (cure < 100U) {
        state->shield = (uint8_t)cure;
        return BADGE_CON_EFFECT_CURE_GAINED;
    }

    state->scar_level++;
    state->last_decay_ms = now_ms;
    uint8_t maximum = badge_con_game_maximum(state->scar_level);
    if (maximum <= 1U) {
        state->shield = 0U;
        state->dead = true;
        return BADGE_CON_EFFECT_DIED;
    }
    state->role = BADGE_CON_ROLE_NORMAL;
    state->shield = maximum;
    return BADGE_CON_EFFECT_CURED;
}

void badge_con_game_snapshot(badge_con_game_state_t *state,
                             uint32_t now_ms,
                             badge_con_snapshot_t *out)
{
    if (!state || !out) {
        return;
    }
    apply_lazy_time(state, now_ms);
    uint8_t maximum = state->seed == BADGE_CON_ROLE_IMMUNE
        ? 100U
        : badge_con_game_maximum(state->scar_level);
    *out = (badge_con_snapshot_t) {
        .seed = state->seed,
        .role = state->role,
        .active = state->active,
        .shield = state->shield,
        .maximum = maximum,
        .scar_level = state->scar_level,
        .cured = state->active && !state->dead &&
            state->role == BADGE_CON_ROLE_NORMAL &&
            state->scar_level > 0U,
        .dead = state->dead,
        .super = state->dead &&
            state->seed == BADGE_CON_ROLE_IMMUNE,
    };
}

bool badge_con_game_encode_nvs(
    const badge_con_game_state_t *state,
    uint8_t out[BADGE_CON_NVS_RECORD_BYTES])
{
    if (!state || !role_is_valid(state->seed) || !out) {
        return false;
    }

    uint8_t record[BADGE_CON_NVS_RECORD_BYTES] = {
        0xC3U,
        0x02U,
        (uint8_t)state->seed,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
    };
    put_le32(&record[8], crc32_ieee(record, 8U));
    memcpy(out, record, sizeof(record));
    return true;
}

badge_con_nvs_decode_result_t badge_con_game_decode_nvs(
    const uint8_t *record,
    size_t record_size,
    badge_con_game_state_t *out)
{
    if (!record || !out || record_size != BADGE_CON_NVS_RECORD_BYTES ||
        record[0] != 0xC3U ||
        !role_is_valid((badge_con_role_t)record[2]) ||
        get_le32(&record[8]) != crc32_ieee(record, 8U)) {
        return BADGE_CON_NVS_INVALID;
    }

    bool current = record[1] == 0x02U &&
        record[3] == 0U && record[4] == 0U &&
        record[5] == 0U && record[6] == 0U && record[7] == 0U;
    bool legacy = record[1] == 0x01U &&
        role_is_valid((badge_con_role_t)record[3]) &&
        record[4] <= 1U && record[5] <= 100U &&
        record[6] == 0U && record[7] == 0U;
    if (!current && !legacy) {
        return BADGE_CON_NVS_INVALID;
    }

    badge_con_role_t seed = (badge_con_role_t)record[2];
    *out = (badge_con_game_state_t) {
        .seed = seed,
        .role = seed,
        .active = false,
        .shield = 0U,
        .scar_level = 0U,
        .dead = false,
        .last_decay_ms = 0U,
    };
    return current ? BADGE_CON_NVS_VALID : BADGE_CON_NVS_SEED_ONLY;
}

bool badge_con_game_encode_rtc(
    const badge_con_game_state_t *state,
    uint32_t expected_reboot_generation,
    uint8_t out[BADGE_CON_RTC_RECORD_BYTES])
{
    if (!state || !role_is_valid(state->seed) || !out ||
        expected_reboot_generation == 0U) {
        return false;
    }

    uint8_t record[BADGE_CON_RTC_RECORD_BYTES] = {
        0xC4U,
        0x02U,
        (uint8_t)state->seed,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
    };
    put_le32(&record[12], expected_reboot_generation);
    put_le32(&record[16], crc32_ieee(record, 16U));
    memcpy(out, record, sizeof(record));
    return true;
}

bool badge_con_game_decode_rtc(
    const uint8_t *record,
    size_t record_size,
    uint32_t expected_reboot_generation,
    badge_con_game_state_t *out)
{
    if (!record || !out || record_size != BADGE_CON_RTC_RECORD_BYTES ||
        expected_reboot_generation == 0U ||
        record[0] != 0xC4U ||
        !role_is_valid((badge_con_role_t)record[2]) ||
        get_le32(&record[12]) != expected_reboot_generation ||
        get_le32(&record[16]) != crc32_ieee(record, 16U)) {
        return false;
    }

    bool current = record[1] == 0x02U &&
        record[3] == 0U && record[4] == 0U &&
        record[5] == 0U && record[6] == 0U && record[7] == 0U &&
        get_le32(&record[8]) == 0U;
    bool legacy = record[1] == 0x01U &&
        role_is_valid((badge_con_role_t)record[3]) &&
        record[4] <= 1U && record[5] <= 100U &&
        record[6] == 0U && record[7] == 0U;
    if (!current && !legacy) {
        return false;
    }

    badge_con_role_t seed = (badge_con_role_t)record[2];
    *out = (badge_con_game_state_t) {
        .seed = seed,
        .role = seed,
        .active = false,
        .shield = 0U,
        .scar_level = 0U,
        .dead = false,
        .last_decay_ms = 0U,
    };
    return true;
}

uint8_t badge_con_game_shield_checkpoint(uint8_t shield)
{
    if (shield > 100U) {
        shield = 100U;
    }
    return (uint8_t)((shield / 10U) * 10U);
}
