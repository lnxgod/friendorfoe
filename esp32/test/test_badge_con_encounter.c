#include "unity.h"

#include "badge_con_encounter.h"

static badge_con_packet_t infected_packet(void)
{
    return (badge_con_packet_t) {
        .version = BADGE_CON_PROTOCOL_VERSION,
        .round = BADGE_CON_ROUND,
        .role = BADGE_CON_ROLE_INFECTED,
        .peer = 0x102030U,
        .session = 0x44U,
        .sequence = 1U,
        .rssi = BADGE_CON_MIN_RSSI,
    };
}

static size_t used_peer_count(const badge_con_encounter_table_t *table)
{
    size_t count = 0U;
    for (size_t i = 0; i < BADGE_CON_PEER_CAPACITY; ++i) {
        if (table->peer[i].used) {
            ++count;
        }
    }
    return count;
}

void test_badge_con_encounter_third_distinct_strong_packet_qualifies(void)
{
    badge_con_encounter_table_t table;
    badge_con_packet_t packet = infected_packet();
    badge_con_encounter_init(&table);

    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &packet, 1000U));
    packet.sequence = 2U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &packet, 2000U));
    packet.sequence = 3U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_QUALIFIED,
        badge_con_encounter_consume(&table, &packet, 3000U));
}

void test_badge_con_encounter_minus_61_never_counts(void)
{
    badge_con_encounter_table_t table;
    badge_con_packet_t packet = infected_packet();
    packet.rssi = BADGE_CON_MIN_RSSI - 1;
    badge_con_encounter_init(&table);

    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_DROPPED_WEAK,
        badge_con_encounter_consume(&table, &packet, 1000U));
    TEST_ASSERT_EQUAL_UINT32(0U, used_peer_count(&table));
}

void test_badge_con_encounter_exact_self_never_occupies_table(void)
{
    badge_con_encounter_table_t table;
    badge_con_packet_t packet = infected_packet();
    badge_con_encounter_init(&table);
    TEST_ASSERT_TRUE(badge_con_encounter_set_self(
        &table, packet.peer, packet.session));

    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_DROPPED_SELF,
        badge_con_encounter_consume(&table, &packet, 1000U));
    TEST_ASSERT_EQUAL_UINT32(0U, used_peer_count(&table));
}

void test_badge_con_encounter_packet_two_does_not_qualify(void)
{
    badge_con_encounter_table_t table;
    badge_con_packet_t packet = infected_packet();
    badge_con_encounter_init(&table);
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &packet, 1000U));
    packet.sequence = 2U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &packet, 2000U));
}

void test_badge_con_encounter_six_second_boundary_qualifies(void)
{
    badge_con_encounter_table_t table;
    badge_con_packet_t packet = infected_packet();
    badge_con_encounter_init(&table);
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &packet, 0U));
    packet.sequence = 2U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &packet, 5000U));
    packet.sequence = 3U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_QUALIFIED,
        badge_con_encounter_consume(&table, &packet, 6000U));
}

void test_badge_con_encounter_packet_after_six_seconds_restarts_quorum(void)
{
    badge_con_encounter_table_t table;
    badge_con_packet_t packet = infected_packet();
    badge_con_encounter_init(&table);
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &packet, 1000U));
    packet.sequence = 2U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &packet, 7001U));
    packet.sequence = 3U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &packet, 8001U));
    packet.sequence = 4U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_QUALIFIED,
        badge_con_encounter_consume(&table, &packet, 9001U));
}

void test_badge_con_encounter_duplicate_and_reordered_sequence_do_not_count(void)
{
    badge_con_encounter_table_t table;
    badge_con_packet_t packet = infected_packet();
    badge_con_encounter_init(&table);
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &packet, 1000U));
    packet.sequence = 2U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &packet, 1100U));
    packet.sequence = 1U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_DROPPED_DUPLICATE,
        badge_con_encounter_consume(&table, &packet, 1200U));
    packet.sequence = 2U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_DROPPED_DUPLICATE,
        badge_con_encounter_consume(&table, &packet, 1300U));
    packet.sequence = 3U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_QUALIFIED,
        badge_con_encounter_consume(&table, &packet, 1400U));
}

void test_badge_con_encounter_sequence_wrap_ff_to_zero_counts(void)
{
    badge_con_encounter_table_t table;
    badge_con_packet_t packet = infected_packet();
    packet.sequence = 0xFEU;
    badge_con_encounter_init(&table);
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &packet, 1000U));
    packet.sequence = 0xFFU;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &packet, 2000U));
    packet.sequence = 0x00U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_QUALIFIED,
        badge_con_encounter_consume(&table, &packet, 3000U));
}

void test_badge_con_encounter_new_session_resets_peer_quorum(void)
{
    badge_con_encounter_table_t table;
    badge_con_packet_t packet = infected_packet();
    badge_con_encounter_init(&table);
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &packet, 1000U));
    packet.sequence = 2U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &packet, 2000U));

    packet.session = 0x45U;
    packet.sequence = 3U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &packet, 3000U));
    packet.sequence = 4U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &packet, 4000U));
    packet.sequence = 5U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_QUALIFIED,
        badge_con_encounter_consume(&table, &packet, 5000U));
}

void test_badge_con_encounter_later_effects_are_limited_to_one_per_eight_seconds(void)
{
    badge_con_encounter_table_t table;
    badge_con_packet_t packet = infected_packet();
    badge_con_encounter_init(&table);
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &packet, 0U));
    packet.sequence = 2U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &packet, 500U));
    packet.sequence = 3U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_QUALIFIED,
        badge_con_encounter_consume(&table, &packet, 1000U));
    packet.sequence = 4U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_RATE_LIMITED,
        badge_con_encounter_consume(&table, &packet, 1500U));
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_DROPPED_DUPLICATE,
        badge_con_encounter_consume(&table, &packet, 1600U));
    packet.sequence = 5U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_RATE_LIMITED,
        badge_con_encounter_consume(&table, &packet, 3000U));
    packet.sequence = 6U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_RATE_LIMITED,
        badge_con_encounter_consume(&table, &packet, 5000U));
    packet.sequence = 7U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_RATE_LIMITED,
        badge_con_encounter_consume(&table, &packet, 7000U));
    packet.sequence = 8U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_RATE_LIMITED,
        badge_con_encounter_consume(&table, &packet, 8999U));
    packet.sequence = 9U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_QUALIFIED,
        badge_con_encounter_consume(&table, &packet, 9000U));
}

void test_badge_con_encounter_each_peer_has_an_independent_effect_timer(void)
{
    badge_con_encounter_table_t table;
    badge_con_packet_t peer_a = infected_packet();
    badge_con_packet_t peer_b = infected_packet();
    peer_b.peer = 0x102031U;
    badge_con_encounter_init(&table);

    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &peer_a, 0U));
    peer_a.sequence = 2U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &peer_a, 500U));
    peer_a.sequence = 3U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_QUALIFIED,
        badge_con_encounter_consume(&table, &peer_a, 1000U));

    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &peer_b, 1500U));
    peer_b.sequence = 2U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &peer_b, 2000U));
    peer_b.sequence = 3U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_QUALIFIED,
        badge_con_encounter_consume(&table, &peer_b, 2500U));

    peer_a.sequence = 4U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_RATE_LIMITED,
        badge_con_encounter_consume(&table, &peer_a, 3000U));
}

void test_badge_con_encounter_replaces_expired_entry_before_dropping_new_peer(void)
{
    badge_con_encounter_table_t table;
    badge_con_packet_t packet = infected_packet();
    badge_con_encounter_init(&table);
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &packet, 0U));
    TEST_ASSERT_EQUAL_UINT32(packet.peer, table.peer[0].peer);

    packet.peer = 0x102031U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &packet, 6001U));
    TEST_ASSERT_EQUAL_UINT32(packet.peer, table.peer[0].peer);
    TEST_ASSERT_FALSE(table.peer[1].used);
}

void test_badge_con_encounter_drops_ninth_active_peer(void)
{
    badge_con_encounter_table_t table;
    badge_con_packet_t packet = infected_packet();
    badge_con_encounter_init(&table);
    for (uint32_t i = 0U; i < BADGE_CON_PEER_CAPACITY; ++i) {
        packet.peer = 0x200000U + i;
        TEST_ASSERT_EQUAL(
            BADGE_CON_OBSERVE_COUNTED,
            badge_con_encounter_consume(&table, &packet, 1000U));
    }
    packet.peer = 0x300000U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_DROPPED_TABLE_FULL,
        badge_con_encounter_consume(&table, &packet, 1000U));
    TEST_ASSERT_EQUAL_UINT32(BADGE_CON_PEER_CAPACITY,
                             used_peer_count(&table));
}

void test_badge_con_encounter_uint32_time_wrap_is_safe(void)
{
    badge_con_encounter_table_t table;
    badge_con_packet_t packet = infected_packet();
    badge_con_encounter_init(&table);
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(
            &table, &packet, UINT32_MAX - 3000U));
    packet.sequence = 2U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(
            &table, &packet, UINT32_MAX - 2000U));
    packet.sequence = 3U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_QUALIFIED,
        badge_con_encounter_consume(
            &table, &packet, UINT32_MAX - 1000U));
    packet.sequence = 4U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_RATE_LIMITED,
        badge_con_encounter_consume(&table, &packet, UINT32_MAX));
    packet.sequence = 5U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_RATE_LIMITED,
        badge_con_encounter_consume(&table, &packet, 1999U));
    packet.sequence = 6U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_RATE_LIMITED,
        badge_con_encounter_consume(&table, &packet, 3999U));
    packet.sequence = 7U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_RATE_LIMITED,
        badge_con_encounter_consume(&table, &packet, 5999U));
    packet.sequence = 8U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_RATE_LIMITED,
        badge_con_encounter_consume(&table, &packet, 6998U));
    packet.sequence = 9U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_QUALIFIED,
        badge_con_encounter_consume(&table, &packet, 6999U));
}

void test_badge_con_encounter_duplicates_do_not_extend_peer_admission_lifetime(void)
{
    badge_con_encounter_table_t table;
    badge_con_packet_t packet = infected_packet();
    badge_con_encounter_init(&table);
    for (uint32_t i = 0U; i < BADGE_CON_PEER_CAPACITY; ++i) {
        packet.peer = 0x400000U + i;
        TEST_ASSERT_EQUAL(
            BADGE_CON_OBSERVE_COUNTED,
            badge_con_encounter_consume(&table, &packet, 0U));
    }
    for (uint32_t i = 0U; i < BADGE_CON_PEER_CAPACITY; ++i) {
        packet.peer = 0x400000U + i;
        TEST_ASSERT_EQUAL(
            BADGE_CON_OBSERVE_DROPPED_DUPLICATE,
            badge_con_encounter_consume(&table, &packet, 5000U));
    }

    packet.peer = 0x500000U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_OBSERVE_COUNTED,
        badge_con_encounter_consume(&table, &packet, 6001U));
}
