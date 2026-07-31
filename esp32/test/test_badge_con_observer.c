#include "unity.h"

#include "badge_con_observer.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static atomic_uint s_observer_critical_depth;
static atomic_uint s_observer_max_critical_depth;
static atomic_bool s_observer_work_outside_critical;
static _Thread_local unsigned s_observer_thread_critical_depth;

void badge_con_observer_test_note_critical(bool entering)
{
    if (entering) {
        s_observer_thread_critical_depth++;
        unsigned depth =
            atomic_fetch_add_explicit(
                &s_observer_critical_depth, 1U, memory_order_relaxed) + 1U;
        unsigned prior =
            atomic_load_explicit(
                &s_observer_max_critical_depth, memory_order_relaxed);
        while (depth > prior &&
               !atomic_compare_exchange_weak_explicit(
                   &s_observer_max_critical_depth,
                   &prior,
                   depth,
                   memory_order_relaxed,
                   memory_order_relaxed)) {
        }
        return;
    }
    s_observer_thread_critical_depth--;
    atomic_fetch_sub_explicit(
        &s_observer_critical_depth, 1U, memory_order_relaxed);
}

void badge_con_observer_test_note_work(void)
{
    if (s_observer_thread_critical_depth != 0U) {
        atomic_store_explicit(
            &s_observer_work_outside_critical,
            false,
            memory_order_relaxed);
    }
}

static void observer_test_reset(void)
{
    badge_con_observer_init(true);
    atomic_store(&s_observer_critical_depth, 0U);
    atomic_store(&s_observer_max_critical_depth, 0U);
    atomic_store(&s_observer_work_outside_critical, true);
    s_observer_thread_critical_depth = 0U;
}

static void build_advertisement(
    badge_con_role_t role,
    uint32_t peer,
    uint8_t session,
    uint8_t sequence,
    uint8_t out[BADGE_CON_LEGACY_ADV_BYTES])
{
    TEST_ASSERT_TRUE(badge_con_build_legacy_advertisement(
        role, false, peer, session, sequence, out));
}

static badge_con_frame_result_t consume_advertisement(
    badge_con_role_t role,
    uint32_t peer,
    uint8_t session,
    uint8_t sequence,
    int8_t rssi,
    uint32_t now_ms,
    badge_con_observe_result_t *observe_out)
{
    uint8_t advertisement[BADGE_CON_LEGACY_ADV_BYTES];
    build_advertisement(role, peer, session, sequence, advertisement);
    return badge_con_observer_consume(
        advertisement,
        sizeof(advertisement),
        rssi,
        now_ms,
        observe_out);
}

void test_badge_con_observer_classifies_game_and_non_game_frames(void)
{
    observer_test_reset();
    static const uint8_t non_game[] = {
        0x02, 0x01, 0x06, 0x04, 0x09, 'F', 'o', 'F',
    };
    badge_con_observe_result_t observe =
        BADGE_CON_OBSERVE_QUALIFIED;

    TEST_ASSERT_EQUAL(
        BADGE_CON_FRAME_NOT_GAME,
        badge_con_observer_consume(
            non_game, sizeof(non_game), -40, 1000U, &observe));
    TEST_ASSERT_EQUAL(BADGE_CON_OBSERVE_QUALIFIED, observe);

    uint8_t malformed[BADGE_CON_LEGACY_ADV_BYTES];
    build_advertisement(
        BADGE_CON_ROLE_INFECTED, 0x010203U, 0x04U, 1U, malformed);
    malformed[30] ^= 1U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_FRAME_INVALID,
        badge_con_observer_consume(
            malformed, sizeof(malformed), -40, 1000U, &observe));
    TEST_ASSERT_EQUAL(BADGE_CON_OBSERVE_QUALIFIED, observe);

    TEST_ASSERT_EQUAL(
        BADGE_CON_FRAME_VALID,
        consume_advertisement(
            BADGE_CON_ROLE_INFECTED,
            0x010203U,
            0x04U,
            1U,
            -60,
            1000U,
            &observe));
    TEST_ASSERT_EQUAL(BADGE_CON_OBSERVE_COUNTED, observe);
}

void test_badge_con_observer_filters_self_weak_and_duplicate_packets(void)
{
    observer_test_reset();
    TEST_ASSERT_TRUE(
        badge_con_observer_set_self(0xA1B2C3U, 0x07U));
    badge_con_observe_result_t observe;

    TEST_ASSERT_EQUAL(
        BADGE_CON_FRAME_VALID,
        consume_advertisement(
            BADGE_CON_ROLE_NORMAL,
            0xA1B2C3U,
            0x07U,
            1U,
            -30,
            1000U,
            &observe));
    TEST_ASSERT_EQUAL(BADGE_CON_OBSERVE_DROPPED_SELF, observe);

    TEST_ASSERT_EQUAL(
        BADGE_CON_FRAME_VALID,
        consume_advertisement(
            BADGE_CON_ROLE_INFECTED,
            0x010203U,
            0x04U,
            1U,
            BADGE_CON_MIN_RSSI - 1,
            1000U,
            &observe));
    TEST_ASSERT_EQUAL(BADGE_CON_OBSERVE_DROPPED_WEAK, observe);

    TEST_ASSERT_EQUAL(
        BADGE_CON_FRAME_VALID,
        consume_advertisement(
            BADGE_CON_ROLE_INFECTED,
            0x010203U,
            0x04U,
            1U,
            -50,
            1100U,
            &observe));
    TEST_ASSERT_EQUAL(BADGE_CON_OBSERVE_COUNTED, observe);
    TEST_ASSERT_EQUAL(
        BADGE_CON_FRAME_VALID,
        consume_advertisement(
            BADGE_CON_ROLE_INFECTED,
            0x010203U,
            0x04U,
            1U,
            -50,
            1200U,
            &observe));
    TEST_ASSERT_EQUAL(BADGE_CON_OBSERVE_DROPPED_DUPLICATE, observe);
}

void test_badge_con_observer_qualifies_quorum_and_coalesces_latest_packet(void)
{
    observer_test_reset();
    badge_con_observe_result_t observe;
    badge_con_packet_t pending;

    for (uint8_t sequence = 1U; sequence <= 3U; ++sequence) {
        TEST_ASSERT_EQUAL(
            BADGE_CON_FRAME_VALID,
            consume_advertisement(
                BADGE_CON_ROLE_INFECTED,
                0x010203U,
                0x04U,
                sequence,
                -50,
                (uint32_t)sequence * 1000U,
                &observe));
    }
    TEST_ASSERT_EQUAL(BADGE_CON_OBSERVE_QUALIFIED, observe);

    for (uint8_t sequence = 1U; sequence <= 3U; ++sequence) {
        TEST_ASSERT_EQUAL(
            BADGE_CON_FRAME_VALID,
            consume_advertisement(
                BADGE_CON_ROLE_IMMUNE,
                0xA1B2C3U,
                0x07U,
                sequence,
                -45,
                4000U + (uint32_t)sequence * 1000U,
                &observe));
    }
    TEST_ASSERT_EQUAL(BADGE_CON_OBSERVE_QUALIFIED, observe);
    TEST_ASSERT_TRUE(badge_con_observer_take_pending(&pending));
    TEST_ASSERT_EQUAL_HEX32(0xA1B2C3U, pending.peer);
    TEST_ASSERT_EQUAL_HEX8(0x07U, pending.session);
    TEST_ASSERT_EQUAL_HEX8(3U, pending.sequence);
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_IMMUNE, pending.role);
    TEST_ASSERT_FALSE(badge_con_observer_take_pending(&pending));
}

void test_badge_con_observer_bounds_peer_table_and_critical_work(void)
{
    observer_test_reset();
    badge_con_observe_result_t observe;
    for (uint32_t index = 0U; index < BADGE_CON_PEER_CAPACITY; ++index) {
        TEST_ASSERT_EQUAL(
            BADGE_CON_FRAME_VALID,
            consume_advertisement(
                BADGE_CON_ROLE_NORMAL,
                0x100000U + index,
                0x01U,
                1U,
                -40,
                1000U,
                &observe));
        TEST_ASSERT_EQUAL(BADGE_CON_OBSERVE_COUNTED, observe);
    }
    TEST_ASSERT_EQUAL(
        BADGE_CON_FRAME_VALID,
        consume_advertisement(
            BADGE_CON_ROLE_NORMAL,
            0x200000U,
            0x01U,
            1U,
            -40,
            1000U,
            &observe));
    TEST_ASSERT_EQUAL(BADGE_CON_OBSERVE_DROPPED_TABLE_FULL, observe);

    TEST_ASSERT_TRUE(atomic_load(&s_observer_work_outside_critical));
    TEST_ASSERT_LESS_OR_EQUAL_UINT(
        1U, atomic_load(&s_observer_max_critical_depth));
    TEST_ASSERT_EQUAL_UINT(
        0U, atomic_load(&s_observer_critical_depth));
}

typedef struct {
    atomic_bool publisher_done;
    atomic_bool failed;
    atomic_uint max_sequence_taken;
} observer_concurrency_t;

static void *observer_publish_thread(void *opaque)
{
    observer_concurrency_t *state = opaque;
    for (uint8_t sequence = 1U; sequence <= 40U; ++sequence) {
        badge_con_observe_result_t observe;
        badge_con_frame_result_t frame = consume_advertisement(
            BADGE_CON_ROLE_INFECTED,
            0x112233U,
            0x44U,
            sequence,
            -40,
            (uint32_t)sequence * 1000U,
            &observe);
        bool should_qualify =
            sequence >= 3U && ((sequence - 3U) % 8U) == 0U;
        badge_con_observe_result_t expected =
            sequence < 3U
                ? BADGE_CON_OBSERVE_COUNTED
                : (should_qualify
                       ? BADGE_CON_OBSERVE_QUALIFIED
                       : BADGE_CON_OBSERVE_RATE_LIMITED);
        if (frame != BADGE_CON_FRAME_VALID ||
            observe != expected) {
            atomic_store(&state->failed, true);
            break;
        }
    }
    atomic_store_explicit(
        &state->publisher_done, true, memory_order_release);
    return NULL;
}

static void *observer_take_thread(void *opaque)
{
    observer_concurrency_t *state = opaque;
    while (!atomic_load_explicit(
               &state->publisher_done, memory_order_acquire)) {
        badge_con_packet_t packet;
        if (badge_con_observer_take_pending(&packet)) {
            unsigned prior = atomic_load(&state->max_sequence_taken);
            while (packet.sequence > prior &&
                   !atomic_compare_exchange_weak(
                       &state->max_sequence_taken,
                       &prior,
                       packet.sequence)) {
            }
        }
    }
    badge_con_packet_t packet;
    while (badge_con_observer_take_pending(&packet)) {
        unsigned prior = atomic_load(&state->max_sequence_taken);
        while (packet.sequence > prior &&
               !atomic_compare_exchange_weak(
                   &state->max_sequence_taken,
                   &prior,
                   packet.sequence)) {
        }
    }
    return NULL;
}

void test_badge_con_observer_concurrent_publish_take_keeps_latest_evidence(void)
{
    observer_test_reset();
    observer_concurrency_t state;
    atomic_init(&state.publisher_done, false);
    atomic_init(&state.failed, false);
    atomic_init(&state.max_sequence_taken, 0U);
    pthread_t publisher;
    pthread_t consumer;

    TEST_ASSERT_EQUAL_INT(
        0, pthread_create(
               &publisher, NULL, observer_publish_thread, &state));
    TEST_ASSERT_EQUAL_INT(
        0, pthread_create(
               &consumer, NULL, observer_take_thread, &state));
    TEST_ASSERT_EQUAL_INT(0, pthread_join(publisher, NULL));
    TEST_ASSERT_EQUAL_INT(0, pthread_join(consumer, NULL));

    TEST_ASSERT_FALSE(atomic_load(&state.failed));
    TEST_ASSERT_EQUAL_UINT(35U, atomic_load(&state.max_sequence_taken));
    TEST_ASSERT_TRUE(atomic_load(&s_observer_work_outside_critical));
    TEST_ASSERT_EQUAL_UINT(
        0U, atomic_load(&s_observer_critical_depth));
}
