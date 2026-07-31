#include "unity.h"

#include "badge_con_game.h"

#include <string.h>

static uint32_t test_crc32_ieee(const uint8_t *bytes, size_t byte_count)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < byte_count; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8U; ++bit) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return crc ^ UINT32_MAX;
}

static void test_put_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8U);
    out[2] = (uint8_t)(value >> 16U);
    out[3] = (uint8_t)(value >> 24U);
}

void test_badge_con_game_rssi_multiplier_boundaries_are_exact(void)
{
    TEST_ASSERT_EQUAL_UINT8(3U, badge_con_game_rssi_multiplier(-1));
    TEST_ASSERT_EQUAL_UINT8(3U, badge_con_game_rssi_multiplier(-45));
    TEST_ASSERT_EQUAL_UINT8(2U, badge_con_game_rssi_multiplier(-46));
    TEST_ASSERT_EQUAL_UINT8(2U, badge_con_game_rssi_multiplier(-52));
    TEST_ASSERT_EQUAL_UINT8(1U, badge_con_game_rssi_multiplier(-53));
    TEST_ASSERT_EQUAL_UINT8(1U, badge_con_game_rssi_multiplier(-60));
    TEST_ASSERT_EQUAL_UINT8(0U, badge_con_game_rssi_multiplier(-61));
    TEST_ASSERT_EQUAL_UINT8(0U, badge_con_game_rssi_multiplier(-127));
    TEST_ASSERT_EQUAL_UINT8(0U, badge_con_game_rssi_multiplier(0));
    TEST_ASSERT_EQUAL_UINT8(0U, badge_con_game_rssi_multiplier(1));
}

void test_badge_con_game_roles_defaults_factory_and_seeded_activation(void)
{
    badge_con_role_t role = BADGE_CON_ROLE_IMMUNE;
    TEST_ASSERT_TRUE(badge_con_role_parse_exact("normal", &role));
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_NORMAL, role);
    TEST_ASSERT_TRUE(badge_con_role_parse_exact("infected", &role));
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, role);
    TEST_ASSERT_TRUE(badge_con_role_parse_exact("immune", &role));
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_IMMUNE, role);
    TEST_ASSERT_EQUAL_STRING("normal",
                             badge_con_role_name(BADGE_CON_ROLE_NORMAL));
    TEST_ASSERT_EQUAL_STRING("infected",
                             badge_con_role_name(BADGE_CON_ROLE_INFECTED));
    TEST_ASSERT_EQUAL_STRING("immune",
                             badge_con_role_name(BADGE_CON_ROLE_IMMUNE));
    TEST_ASSERT_NULL(badge_con_role_name((badge_con_role_t)3));

    const char *invalid[] = {
        "Normal", "INFECTED", "immune ", " immune", "normal\n",
        "infected-extra", "", NULL,
    };
    role = BADGE_CON_ROLE_IMMUNE;
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        TEST_ASSERT_FALSE(badge_con_role_parse_exact(invalid[i], &role));
        TEST_ASSERT_EQUAL(BADGE_CON_ROLE_IMMUNE, role);
    }
    TEST_ASSERT_FALSE(badge_con_role_parse_exact("normal", NULL));

    badge_con_game_state_t game = {
        .seed = BADGE_CON_ROLE_IMMUNE,
        .role = BADGE_CON_ROLE_INFECTED,
        .active = true,
        .shield = 88U,
        .scar_level = 5U,
        .dead = true,
        .last_decay_ms = 42U,
    };
    badge_con_game_defaults(&game);
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_NORMAL, game.seed);
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_NORMAL, game.role);
    TEST_ASSERT_FALSE(game.active);
    TEST_ASSERT_EQUAL_UINT8(0U, game.shield);
    TEST_ASSERT_EQUAL_UINT8(0U, game.scar_level);
    TEST_ASSERT_FALSE(game.dead);
    TEST_ASSERT_EQUAL_UINT32(0U, game.last_decay_ms);

    badge_con_game_apply_factory_seed(
        &game, BADGE_CON_ROLE_INFECTED, 1234U);
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, game.seed);
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, game.role);
    TEST_ASSERT_FALSE(game.active);
    TEST_ASSERT_EQUAL_UINT8(0U, game.shield);
    TEST_ASSERT_EQUAL_UINT8(0U, game.scar_level);
    TEST_ASSERT_FALSE(game.dead);
    TEST_ASSERT_EQUAL_UINT32(1234U, game.last_decay_ms);
    TEST_ASSERT_TRUE(badge_con_game_activate(&game, 5678U));
    TEST_ASSERT_TRUE(game.active);
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, game.role);
    TEST_ASSERT_EQUAL_UINT8(45U, game.shield);
    TEST_ASSERT_EQUAL_UINT32(5678U, game.last_decay_ms);
    TEST_ASSERT_FALSE(badge_con_game_activate(&game, 9999U));
    TEST_ASSERT_EQUAL_UINT32(5678U, game.last_decay_ms);

    badge_con_game_apply_factory_seed(
        &game, BADGE_CON_ROLE_IMMUNE, 100U);
    TEST_ASSERT_EQUAL_UINT8(0U, game.shield);
    TEST_ASSERT_TRUE(badge_con_game_activate(&game, 200U));
    TEST_ASSERT_EQUAL_UINT8(100U, game.shield);

    badge_con_game_apply_factory_seed(
        &game, BADGE_CON_ROLE_NORMAL, 250U);
    TEST_ASSERT_TRUE(badge_con_game_activate(&game, 300U));
    TEST_ASSERT_EQUAL_UINT8(30U, game.shield);

    badge_con_game_apply_factory_seed(
        &game, (badge_con_role_t)99, 300U);
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_NORMAL, game.seed);
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_NORMAL, game.role);
}

void test_badge_con_game_human_distance_damage_healing_and_zero_crossing(void)
{
    badge_con_game_state_t human = {
        .seed = BADGE_CON_ROLE_NORMAL,
        .role = BADGE_CON_ROLE_NORMAL,
        .active = true,
        .shield = 80U,
        .scar_level = 0U,
        .dead = false,
        .last_decay_ms = 1000U,
    };

    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_SHIELD_DRAINED,
        badge_con_game_apply_peer(
            &human, BADGE_CON_ROLE_INFECTED, false, -60, 1000U));
    TEST_ASSERT_EQUAL_UINT8(70U, human.shield);

    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_SHIELD_DRAINED,
        badge_con_game_apply_peer(
            &human, BADGE_CON_ROLE_INFECTED, false, -50, 1000U));
    TEST_ASSERT_EQUAL_UINT8(50U, human.shield);

    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_SHIELD_DRAINED,
        badge_con_game_apply_peer(
            &human, BADGE_CON_ROLE_INFECTED, false, -45, 1000U));
    TEST_ASSERT_EQUAL_UINT8(20U, human.shield);

    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_NONE,
        badge_con_game_apply_peer(
            &human, BADGE_CON_ROLE_INFECTED, false, -61, 1000U));
    TEST_ASSERT_EQUAL_UINT8(20U, human.shield);

    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_SHIELD_GAINED,
        badge_con_game_apply_peer(
            &human, BADGE_CON_ROLE_IMMUNE, false, -45, 1000U));
    TEST_ASSERT_EQUAL_UINT8(35U, human.shield);

    human.shield = 20U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_INFECTED,
        badge_con_game_apply_peer(
            &human, BADGE_CON_ROLE_INFECTED, false, -50, 1000U));
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, human.role);
    TEST_ASSERT_EQUAL_UINT8(45U, human.shield);
    TEST_ASSERT_EQUAL_UINT32(1000U, human.last_decay_ms);

    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_NONE,
        badge_con_game_apply_peer(
            &human, BADGE_CON_ROLE_NORMAL, false, -45, 1000U));
}

void test_badge_con_game_super_zombie_doubles_human_damage(void)
{
    badge_con_game_state_t regular = {
        .seed = BADGE_CON_ROLE_NORMAL,
        .role = BADGE_CON_ROLE_NORMAL,
        .active = true,
        .shield = 100U,
        .last_decay_ms = 1000U,
    };
    badge_con_game_state_t super = regular;

    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_SHIELD_DRAINED,
        badge_con_game_apply_peer(
            &regular, BADGE_CON_ROLE_INFECTED, false, -52, 1000U));
    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_SHIELD_DRAINED,
        badge_con_game_apply_peer(
            &super, BADGE_CON_ROLE_INFECTED, true, -52, 1000U));
    TEST_ASSERT_EQUAL_UINT8(80U, regular.shield);
    TEST_ASSERT_EQUAL_UINT8(60U, super.shield);

    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_NONE,
        badge_con_game_apply_peer(
            &regular, BADGE_CON_ROLE_IMMUNE, true, -45, 1000U));
    TEST_ASSERT_EQUAL_UINT8(80U, regular.shield);
}

void test_badge_con_game_cures_follow_scar_table_then_die(void)
{
    badge_con_game_state_t game = {
        .seed = BADGE_CON_ROLE_INFECTED,
        .role = BADGE_CON_ROLE_INFECTED,
        .active = true,
        .shield = 90U,
        .scar_level = 0U,
        .dead = false,
        .last_decay_ms = 0U,
    };
    static const uint8_t expected_maxima[] = {50U, 25U, 12U, 6U, 3U};

    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_CURED,
        badge_con_game_apply_peer(
            &game, BADGE_CON_ROLE_IMMUNE, false, -60, 0U));
    TEST_ASSERT_EQUAL_UINT8(1U, game.scar_level);
    TEST_ASSERT_EQUAL_UINT8(50U, game.shield);

    for (size_t index = 0U; index < sizeof(expected_maxima); ++index) {
        if (index != 0U) {
            TEST_ASSERT_EQUAL(BADGE_CON_ROLE_NORMAL, game.role);
        }
        TEST_ASSERT_EQUAL_UINT8(expected_maxima[index], game.shield);
        TEST_ASSERT_EQUAL_UINT8(
            expected_maxima[index],
            badge_con_game_maximum(game.scar_level));
        game.role = BADGE_CON_ROLE_INFECTED;
        game.shield = 90U;
        TEST_ASSERT_EQUAL(
            index + 1U < sizeof(expected_maxima)
                ? BADGE_CON_EFFECT_CURED
                : BADGE_CON_EFFECT_DIED,
            badge_con_game_apply_peer(
                &game, BADGE_CON_ROLE_IMMUNE, false, -60,
                (uint32_t)(index + 1U)));
    }

    TEST_ASSERT_EQUAL_UINT8(6U, game.scar_level);
    TEST_ASSERT_EQUAL_UINT8(1U, badge_con_game_maximum(game.scar_level));
    TEST_ASSERT_TRUE(game.dead);
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, game.role);
    TEST_ASSERT_EQUAL_UINT8(0U, game.shield);
}

void test_badge_con_game_cured_human_does_not_decay_but_cure_progress_does(void)
{
    badge_con_game_state_t cured = {
        .seed = BADGE_CON_ROLE_NORMAL,
        .role = BADGE_CON_ROLE_NORMAL,
        .active = true,
        .shield = 49U,
        .scar_level = 1U,
        .last_decay_ms = 1000U,
    };
    badge_con_game_state_t infected = {
        .seed = BADGE_CON_ROLE_NORMAL,
        .role = BADGE_CON_ROLE_INFECTED,
        .active = true,
        .shield = 50U,
        .scar_level = 1U,
        .last_decay_ms = 1000U,
    };
    badge_con_snapshot_t snapshot = {0};

    badge_con_game_snapshot(&cured, 301000U, &snapshot);
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_NORMAL, snapshot.role);
    TEST_ASSERT_TRUE(snapshot.cured);
    TEST_ASSERT_EQUAL_UINT8(49U, snapshot.shield);
    TEST_ASSERT_EQUAL_UINT8(50U, snapshot.maximum);

    badge_con_game_snapshot(&infected, 61000U, &snapshot);
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, snapshot.role);
    TEST_ASSERT_EQUAL_UINT8(49U, snapshot.shield);
}

void test_badge_con_game_cured_human_can_be_reinfected(void)
{
    badge_con_game_state_t cured = {
        .seed = BADGE_CON_ROLE_NORMAL,
        .role = BADGE_CON_ROLE_NORMAL,
        .active = true,
        .shield = 50U,
        .scar_level = 1U,
    };

    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_SHIELD_DRAINED,
        badge_con_game_apply_peer(
            &cured, BADGE_CON_ROLE_INFECTED, false, -45, 0U));
    TEST_ASSERT_EQUAL_UINT8(20U, cured.shield);
    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_INFECTED,
        badge_con_game_apply_peer(
            &cured, BADGE_CON_ROLE_INFECTED, false, -45, 0U));
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, cured.role);
    TEST_ASSERT_EQUAL_UINT8(
        BADGE_CON_INFECTED_RESCUE_POINTS, cured.shield);

    badge_con_snapshot_t snapshot = {0};
    badge_con_game_snapshot(&cured, 0U, &snapshot);
    TEST_ASSERT_EQUAL_UINT8(50U, snapshot.maximum);
}

void test_badge_con_game_infected_rescue_countdown_reaches_permanent_death(void)
{
    badge_con_game_state_t game = {0};
    badge_con_game_apply_factory_seed(
        &game, BADGE_CON_ROLE_INFECTED, 1000U);
    TEST_ASSERT_TRUE(badge_con_game_activate(&game, 1000U));
    TEST_ASSERT_EQUAL_UINT8(45U, game.shield);

    badge_con_snapshot_t snapshot = {0};
    badge_con_game_snapshot(&game, 1000U + 44U * 60000U, &snapshot);
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, snapshot.role);
    TEST_ASSERT_EQUAL_UINT8(1U, snapshot.shield);
    TEST_ASSERT_FALSE(snapshot.dead);
    TEST_ASSERT_FALSE(snapshot.super);

    badge_con_game_snapshot(&game, 1000U + 45U * 60000U, &snapshot);
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, snapshot.role);
    TEST_ASSERT_EQUAL_UINT8(0U, snapshot.shield);
    TEST_ASSERT_TRUE(snapshot.dead);
    TEST_ASSERT_FALSE(snapshot.super);
    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_NONE,
        badge_con_game_apply_peer(
            &game, BADGE_CON_ROLE_IMMUNE, false, -45,
            1000U + 45U * 60000U));

    uint32_t wrapped_start = UINT32_MAX - 29999U;
    badge_con_game_state_t wrapped = {
        .seed = BADGE_CON_ROLE_INFECTED,
        .role = BADGE_CON_ROLE_INFECTED,
        .active = true,
        .shield = 45U,
        .last_decay_ms = wrapped_start,
    };
    badge_con_game_snapshot(
        &wrapped, wrapped_start + 44U * 60000U, &snapshot);
    TEST_ASSERT_EQUAL_UINT8(1U, snapshot.shield);
    TEST_ASSERT_FALSE(snapshot.dead);
    badge_con_game_snapshot(
        &wrapped, wrapped_start + 45U * 60000U, &snapshot);
    TEST_ASSERT_EQUAL_UINT8(0U, snapshot.shield);
    TEST_ASSERT_TRUE(snapshot.dead);
}

void test_badge_con_game_infected_pressure_and_healer_race(void)
{
    badge_con_game_state_t game = {
        .seed = BADGE_CON_ROLE_NORMAL,
        .role = BADGE_CON_ROLE_INFECTED,
        .active = true,
        .shield = 45U,
        .last_decay_ms = 1000U,
    };

    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_SHIELD_DRAINED,
        badge_con_game_apply_peer(
            &game, BADGE_CON_ROLE_INFECTED, false, -60, 1000U));
    TEST_ASSERT_EQUAL_UINT8(44U, game.shield);

    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_SHIELD_DRAINED,
        badge_con_game_apply_peer(
            &game, BADGE_CON_ROLE_INFECTED, true, -45, 1000U));
    TEST_ASSERT_EQUAL_UINT8(38U, game.shield);

    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_CURE_GAINED,
        badge_con_game_apply_peer(
            &game, BADGE_CON_ROLE_IMMUNE, false, -45, 1000U));
    TEST_ASSERT_EQUAL_UINT8(74U, game.shield);

    game.shield = 6U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_DIED,
        badge_con_game_apply_peer(
            &game, BADGE_CON_ROLE_INFECTED, true, -45, 1000U));
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, game.role);
    TEST_ASSERT_EQUAL_UINT8(0U, game.shield);
    TEST_ASSERT_TRUE(game.dead);

    badge_con_snapshot_t snapshot = {0};
    badge_con_game_snapshot(&game, 1000U, &snapshot);
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, snapshot.role);
    TEST_ASSERT_TRUE(snapshot.dead);
    TEST_ASSERT_FALSE(snapshot.super);

    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_NONE,
        badge_con_game_apply_peer(
            &game, BADGE_CON_ROLE_IMMUNE, false, -45, 2000U));
    TEST_ASSERT_EQUAL_UINT8(0U, game.shield);
    TEST_ASSERT_TRUE(game.dead);
}

void test_badge_con_game_healer_requires_peer_healing(void)
{
    badge_con_game_state_t healer = {
        .seed = BADGE_CON_ROLE_IMMUNE,
        .role = BADGE_CON_ROLE_IMMUNE,
        .active = true,
        .shield = 90U,
        .last_decay_ms = 1000U,
    };

    badge_con_snapshot_t snapshot = {0};
    badge_con_game_snapshot(&healer, 9000U, &snapshot);
    TEST_ASSERT_EQUAL_UINT8(90U, snapshot.shield);

    healer.last_decay_ms = UINT32_MAX - 999U;
    badge_con_game_snapshot(&healer, 1000U, &snapshot);
    TEST_ASSERT_EQUAL_UINT8(90U, snapshot.shield);

    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_SHIELD_GAINED,
        badge_con_game_apply_peer(
            &healer, BADGE_CON_ROLE_IMMUNE, false, -45, 1000U));
    TEST_ASSERT_EQUAL_UINT8(100U, healer.shield);
}

void test_badge_con_game_healer_and_zombie_pressure_rates(void)
{
    badge_con_game_state_t healer = {
        .seed = BADGE_CON_ROLE_IMMUNE,
        .role = BADGE_CON_ROLE_IMMUNE,
        .active = true,
        .shield = 100U,
    };

    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_SHIELD_DRAINED,
        badge_con_game_apply_peer(
            &healer, BADGE_CON_ROLE_INFECTED, false, -60, 0U));
    TEST_ASSERT_EQUAL_UINT8(90U, healer.shield);
    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_SHIELD_DRAINED,
        badge_con_game_apply_peer(
            &healer, BADGE_CON_ROLE_INFECTED, false, -60, 0U));
    TEST_ASSERT_EQUAL_UINT8(80U, healer.shield);
    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_SHIELD_DRAINED,
        badge_con_game_apply_peer(
            &healer, BADGE_CON_ROLE_INFECTED, true, -45, 0U));
    TEST_ASSERT_EQUAL_UINT8(20U, healer.shield);

    badge_con_game_state_t infected = {
        .seed = BADGE_CON_ROLE_NORMAL,
        .role = BADGE_CON_ROLE_INFECTED,
        .active = true,
        .shield = 20U,
    };
    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_CURE_GAINED,
        badge_con_game_apply_peer(
            &infected, BADGE_CON_ROLE_IMMUNE, false, -60, 0U));
    TEST_ASSERT_EQUAL_UINT8(32U, infected.shield);
    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_CURE_GAINED,
        badge_con_game_apply_peer(
            &infected, BADGE_CON_ROLE_IMMUNE, false, -52, 0U));
    TEST_ASSERT_EQUAL_UINT8(56U, infected.shield);
    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_CURE_GAINED,
        badge_con_game_apply_peer(
            &infected, BADGE_CON_ROLE_IMMUNE, false, -45, 0U));
    TEST_ASSERT_EQUAL_UINT8(92U, infected.shield);
}

void test_badge_con_game_immune_dies_and_becomes_derived_super(void)
{
    badge_con_game_state_t game = {
        .seed = BADGE_CON_ROLE_IMMUNE,
        .role = BADGE_CON_ROLE_IMMUNE,
        .active = true,
        .shield = 5U,
        .last_decay_ms = 1000U,
    };
    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_DIED,
        badge_con_game_apply_peer(
            &game, BADGE_CON_ROLE_INFECTED, true, -45, 1000U));
    TEST_ASSERT_TRUE(game.dead);
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, game.role);
    TEST_ASSERT_EQUAL_UINT8(0U, game.shield);

    badge_con_snapshot_t snapshot = {0};
    badge_con_game_snapshot(&game, 1000U, &snapshot);
    TEST_ASSERT_TRUE(snapshot.dead);
    TEST_ASSERT_TRUE(snapshot.super);
    TEST_ASSERT_EQUAL_UINT8(100U, snapshot.maximum);

    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_NONE,
        badge_con_game_apply_peer(
            &game, BADGE_CON_ROLE_IMMUNE, false, -45, 2000U));
    TEST_ASSERT_TRUE(game.dead);
    TEST_ASSERT_EQUAL_UINT8(0U, game.shield);
}

void test_badge_con_game_lazy_decay_and_uint32_wrap(void)
{
    badge_con_game_state_t human = {
        .seed = BADGE_CON_ROLE_NORMAL,
        .role = BADGE_CON_ROLE_NORMAL,
        .active = true,
        .shield = 3U,
        .last_decay_ms = 1000U,
    };
    badge_con_snapshot_t snapshot = {0};

    badge_con_game_snapshot(&human, 60999U, &snapshot);
    TEST_ASSERT_EQUAL_UINT8(3U, snapshot.shield);
    badge_con_game_snapshot(&human, 61000U, &snapshot);
    TEST_ASSERT_EQUAL_UINT8(3U, snapshot.shield);
    badge_con_game_snapshot(&human, 181000U, &snapshot);
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_NORMAL, snapshot.role);
    TEST_ASSERT_EQUAL_UINT8(3U, snapshot.shield);
    TEST_ASSERT_EQUAL_UINT32(1000U, human.last_decay_ms);

    badge_con_game_state_t infected = {
        .seed = BADGE_CON_ROLE_INFECTED,
        .role = BADGE_CON_ROLE_INFECTED,
        .active = true,
        .shield = 3U,
        .last_decay_ms = UINT32_MAX - 30000U,
    };
    badge_con_game_snapshot(&infected, 29999U, &snapshot);
    TEST_ASSERT_EQUAL_UINT8(2U, snapshot.shield);
    TEST_ASSERT_EQUAL_UINT32(29999U, infected.last_decay_ms);
}

void test_badge_con_game_checkpoints_remain_bounded(void)
{
    TEST_ASSERT_EQUAL_UINT8(0U, badge_con_game_shield_checkpoint(0U));
    TEST_ASSERT_EQUAL_UINT8(0U, badge_con_game_shield_checkpoint(9U));
    TEST_ASSERT_EQUAL_UINT8(10U, badge_con_game_shield_checkpoint(10U));
    TEST_ASSERT_EQUAL_UINT8(90U, badge_con_game_shield_checkpoint(99U));
    TEST_ASSERT_EQUAL_UINT8(100U, badge_con_game_shield_checkpoint(100U));
    TEST_ASSERT_EQUAL_UINT8(100U, badge_con_game_shield_checkpoint(255U));
}

static void assert_seed_only_state(const badge_con_game_state_t *state,
                                   badge_con_role_t seed)
{
    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_EQUAL(seed, state->seed);
    TEST_ASSERT_EQUAL(seed, state->role);
    TEST_ASSERT_FALSE(state->active);
    TEST_ASSERT_EQUAL_UINT8(0U, state->shield);
    TEST_ASSERT_EQUAL_UINT8(0U, state->scar_level);
    TEST_ASSERT_FALSE(state->dead);
    TEST_ASSERT_EQUAL_UINT32(0U, state->last_decay_ms);
}

void test_badge_con_game_nvs_v2_is_seed_only_and_exact(void)
{
    const badge_con_game_state_t live = {
        .seed = BADGE_CON_ROLE_IMMUNE,
        .role = BADGE_CON_ROLE_INFECTED,
        .active = true,
        .shield = 0U,
        .scar_level = 0U,
        .dead = true,
        .last_decay_ms = 0x12345678U,
    };
    const uint8_t expected[BADGE_CON_NVS_RECORD_BYTES] = {
        0xC3U, 0x02U, 0x02U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U,
        0x9FU, 0x2FU, 0x5CU, 0x4BU,
    };
    uint8_t record[BADGE_CON_NVS_RECORD_BYTES] = {0};
    TEST_ASSERT_TRUE(badge_con_game_encode_nvs(&live, record));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, record, sizeof(record));

    badge_con_game_state_t decoded = {0};
    TEST_ASSERT_EQUAL(
        BADGE_CON_NVS_VALID,
        badge_con_game_decode_nvs(record, sizeof(record), &decoded));
    assert_seed_only_state(&decoded, BADGE_CON_ROLE_IMMUNE);
}

void test_badge_con_game_nvs_legacy_v1_discards_live_state(void)
{
    uint8_t legacy[BADGE_CON_NVS_RECORD_BYTES] = {
        0xC3U, 0x01U, (uint8_t)BADGE_CON_ROLE_INFECTED,
        (uint8_t)BADGE_CON_ROLE_NORMAL, 0x01U, 0x5AU, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U,
    };
    test_put_le32(&legacy[8], test_crc32_ieee(legacy, 8U));

    badge_con_game_state_t decoded = {
        .seed = BADGE_CON_ROLE_IMMUNE,
        .role = BADGE_CON_ROLE_IMMUNE,
        .active = true,
        .shield = 88U,
        .scar_level = 4U,
        .dead = true,
        .last_decay_ms = 77U,
    };
    TEST_ASSERT_EQUAL(
        BADGE_CON_NVS_SEED_ONLY,
        badge_con_game_decode_nvs(legacy, sizeof(legacy), &decoded));
    assert_seed_only_state(&decoded, BADGE_CON_ROLE_INFECTED);
}

void test_badge_con_game_nvs_structural_corruption_is_invalid_atomically(void)
{
    const badge_con_game_state_t live = {
        .seed = BADGE_CON_ROLE_NORMAL,
        .role = BADGE_CON_ROLE_INFECTED,
        .active = true,
        .shield = 91U,
        .scar_level = 2U,
        .last_decay_ms = 42U,
    };
    uint8_t base[BADGE_CON_NVS_RECORD_BYTES] = {0};
    TEST_ASSERT_TRUE(badge_con_game_encode_nvs(&live, base));

    const size_t offsets[] = {0U, 1U, 2U, 3U, 7U, 8U};
    const uint8_t bad_values[] = {
        0xC2U, 0x03U, 0x03U, 0x01U, 0x01U, 0x00U,
    };
    for (size_t i = 0U; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        uint8_t record[BADGE_CON_NVS_RECORD_BYTES];
        memcpy(record, base, sizeof(record));
        if (offsets[i] == 8U) {
            record[8] ^= 0x01U;
        } else {
            record[offsets[i]] = bad_values[i];
            test_put_le32(&record[8], test_crc32_ieee(record, 8U));
        }
        badge_con_game_state_t decoded = {
            .seed = BADGE_CON_ROLE_IMMUNE,
            .role = BADGE_CON_ROLE_INFECTED,
            .active = true,
            .shield = 77U,
            .scar_level = 5U,
            .dead = true,
            .last_decay_ms = 88U,
        };
        const badge_con_game_state_t sentinel = decoded;
        TEST_ASSERT_EQUAL(
            BADGE_CON_NVS_INVALID,
            badge_con_game_decode_nvs(record, sizeof(record), &decoded));
        TEST_ASSERT_EQUAL_MEMORY(&sentinel, &decoded, sizeof(decoded));
    }

    badge_con_game_state_t decoded = {0};
    const badge_con_game_state_t sentinel = decoded;
    TEST_ASSERT_EQUAL(
        BADGE_CON_NVS_INVALID,
        badge_con_game_decode_nvs(
            base, BADGE_CON_NVS_RECORD_BYTES - 1U, &decoded));
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &decoded, sizeof(decoded));
}

void test_badge_con_game_rtc_dependency_is_seed_only_and_exact(void)
{
    const badge_con_game_state_t live = {
        .seed = BADGE_CON_ROLE_IMMUNE,
        .role = BADGE_CON_ROLE_INFECTED,
        .active = true,
        .shield = 0U,
        .dead = true,
        .last_decay_ms = 0x12345678U,
    };
    const uint8_t expected[BADGE_CON_RTC_RECORD_BYTES] = {
        0xC4U, 0x02U, 0x02U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U,
        0xEFU, 0xCDU, 0xABU, 0x89U,
        0x3BU, 0x63U, 0x1DU, 0xCDU,
    };
    uint8_t record[BADGE_CON_RTC_RECORD_BYTES] = {0};
    TEST_ASSERT_TRUE(
        badge_con_game_encode_rtc(&live, 0x89ABCDEFU, record));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, record, sizeof(record));

    badge_con_game_state_t decoded = {0};
    TEST_ASSERT_TRUE(badge_con_game_decode_rtc(
        record, sizeof(record), 0x89ABCDEFU, &decoded));
    assert_seed_only_state(&decoded, BADGE_CON_ROLE_IMMUNE);
}

void test_badge_con_game_rtc_legacy_v1_discards_live_state(void)
{
    uint8_t legacy[BADGE_CON_RTC_RECORD_BYTES] = {
        0xC4U, 0x01U, (uint8_t)BADGE_CON_ROLE_NORMAL,
        (uint8_t)BADGE_CON_ROLE_INFECTED, 0x01U, 0x53U, 0x00U, 0x00U,
        0x78U, 0x56U, 0x34U, 0x12U,
        0x07U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U,
    };
    test_put_le32(&legacy[16], test_crc32_ieee(legacy, 16U));

    badge_con_game_state_t decoded = {0};
    TEST_ASSERT_TRUE(badge_con_game_decode_rtc(
        legacy, sizeof(legacy), 7U, &decoded));
    assert_seed_only_state(&decoded, BADGE_CON_ROLE_NORMAL);
}

void test_badge_con_game_rtc_dependency_rejects_generation_and_corruption(void)
{
    const badge_con_game_state_t game = {
        .seed = BADGE_CON_ROLE_INFECTED,
        .role = BADGE_CON_ROLE_INFECTED,
        .active = true,
        .shield = 80U,
        .last_decay_ms = 123U,
    };
    uint8_t base[BADGE_CON_RTC_RECORD_BYTES] = {0};
    TEST_ASSERT_TRUE(badge_con_game_encode_rtc(&game, 7U, base));
    TEST_ASSERT_FALSE(badge_con_game_encode_rtc(&game, 0U, base));

    const size_t offsets[] = {0U, 1U, 2U, 3U, 11U, 12U, 16U};
    const uint8_t bad_values[] = {
        0xC5U, 0x03U, 0x03U, 0x01U, 0x01U, 0x08U, 0x00U,
    };
    for (size_t i = 0U; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        uint8_t record[BADGE_CON_RTC_RECORD_BYTES];
        memcpy(record, base, sizeof(record));
        if (offsets[i] == 16U) {
            record[16] ^= 0x01U;
        } else {
            record[offsets[i]] = bad_values[i];
            test_put_le32(&record[16], test_crc32_ieee(record, 16U));
        }
        badge_con_game_state_t decoded = {
            .seed = BADGE_CON_ROLE_IMMUNE,
            .role = BADGE_CON_ROLE_INFECTED,
            .active = true,
            .shield = 77U,
            .scar_level = 5U,
            .dead = true,
            .last_decay_ms = UINT32_MAX,
        };
        const badge_con_game_state_t sentinel = decoded;
        TEST_ASSERT_FALSE(badge_con_game_decode_rtc(
            record, sizeof(record), 7U, &decoded));
        TEST_ASSERT_EQUAL_MEMORY(&sentinel, &decoded, sizeof(decoded));
    }

    badge_con_game_state_t decoded = {0};
    const badge_con_game_state_t sentinel = decoded;
    TEST_ASSERT_FALSE(badge_con_game_decode_rtc(
        base, sizeof(base), 8U, &decoded));
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &decoded, sizeof(decoded));
    TEST_ASSERT_FALSE(badge_con_game_decode_rtc(
        base, sizeof(base), 0U, &decoded));
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &decoded, sizeof(decoded));
    TEST_ASSERT_FALSE(badge_con_game_decode_rtc(
        base, sizeof(base) - 1U, 7U, &decoded));
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &decoded, sizeof(decoded));
}

void test_badge_con_game_seed_records_reject_invalid_seed(void)
{
    badge_con_game_state_t invalid = {
        .seed = (badge_con_role_t)3,
        .role = BADGE_CON_ROLE_NORMAL,
    };
    uint8_t nvs[BADGE_CON_NVS_RECORD_BYTES];
    uint8_t rtc[BADGE_CON_RTC_RECORD_BYTES];
    memset(nvs, 0xA5, sizeof(nvs));
    memset(rtc, 0x5A, sizeof(rtc));
    uint8_t nvs_sentinel[BADGE_CON_NVS_RECORD_BYTES];
    uint8_t rtc_sentinel[BADGE_CON_RTC_RECORD_BYTES];
    memcpy(nvs_sentinel, nvs, sizeof(nvs));
    memcpy(rtc_sentinel, rtc, sizeof(rtc));
    TEST_ASSERT_FALSE(badge_con_game_encode_nvs(&invalid, nvs));
    TEST_ASSERT_FALSE(badge_con_game_encode_rtc(&invalid, 7U, rtc));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(nvs_sentinel, nvs, sizeof(nvs));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(rtc_sentinel, rtc, sizeof(rtc));
}
