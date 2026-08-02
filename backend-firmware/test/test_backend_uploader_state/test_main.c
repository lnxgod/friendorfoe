#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "backend_identity.h"
#include "backend_upload_fifo.h"
#include "backend_uploader.h"
#include "../support/backend_test_main.h"

void setUp(void) {}
void tearDown(void) {}

static backend_upload_batch_t fixture_batch(
    uint32_t sequence,
    const char *json)
{
    backend_upload_batch_t batch = {0};
    batch.sequence = sequence;
    batch.item_count = 1U;
    batch.json_len = (uint16_t)strlen(json);
    memcpy(batch.json, json, (size_t)batch.json_len + 1U);
    batch.json_crc32 = backend_identity_crc32(
        batch.json, batch.json_len);
    return batch;
}

static void push_and_note(
    backend_uploader_state_t *state,
    backend_upload_fifo_t *fifo,
    const backend_upload_batch_t *batch)
{
    uint32_t dropped_sequence = 0U;
    uint32_t dropped_crc32 = 0U;
    const backend_upload_batch_t *oldest =
        backend_upload_fifo_peek(fifo);
    if (oldest && fifo->count == fifo->capacity) {
        dropped_sequence = oldest->sequence;
        dropped_crc32 = oldest->json_crc32;
    }

    bool dropped = false;
    TEST_ASSERT_TRUE(backend_upload_fifo_push(fifo, batch, &dropped));
    TEST_ASSERT_EQUAL(dropped_sequence != 0U, dropped);
    TEST_ASSERT_TRUE(backend_uploader_note_enqueued(
        state,
        fifo->count,
        dropped,
        dropped_sequence,
        dropped_crc32));
}

static bool begin_current_head(
    backend_uploader_state_t *state,
    const backend_upload_fifo_t *fifo,
    int64_t now_ms)
{
    return backend_uploader_begin_head(
        state,
        backend_upload_fifo_peek(fifo),
        fifo->count,
        now_ms);
}

static backend_uploader_queue_result_t remove_exact_head(
    backend_upload_fifo_t *fifo,
    uint32_t request_sequence,
    uint32_t request_crc32,
    bool quarantine)
{
    const backend_upload_batch_t *head =
        backend_upload_fifo_peek(fifo);
    if (!head || head->sequence != request_sequence ||
        head->json_crc32 != request_crc32 ||
        !backend_upload_fifo_pop_acked(fifo, request_sequence)) {
        return BACKEND_UPLOADER_QUEUE_UNCHANGED;
    }
    return quarantine ? BACKEND_UPLOADER_QUEUE_QUARANTINED :
                        BACKEND_UPLOADER_QUEUE_POPPED;
}

void test_ack_a_then_explicit_locked_peek_arms_b(void)
{
    backend_upload_batch_t storage[3];
    backend_upload_fifo_t fifo;
    backend_upload_fifo_init(&fifo, storage, 3U);
    backend_uploader_state_t state;
    backend_uploader_state_init(&state);
    const backend_upload_batch_t a = fixture_batch(1U, "{\"a\":1}");
    const backend_upload_batch_t b = fixture_batch(2U, "{\"b\":2}");

    push_and_note(&state, &fifo, &a);
    TEST_ASSERT_TRUE(begin_current_head(&state, &fifo, 100));
    push_and_note(&state, &fifo, &b);
    TEST_ASSERT_EQUAL_UINT32(2U, state.queue_depth);
    TEST_ASSERT_EQUAL_UINT32(2U, state.queued_count);

    const backend_uploader_queue_result_t pop_result = remove_exact_head(
        &fifo, a.sequence, a.json_crc32, false);
    TEST_ASSERT_EQUAL(BACKEND_UPLOADER_QUEUE_POPPED, pop_result);
    TEST_ASSERT_EQUAL(
        BACKEND_UPLOADER_ACKED,
        backend_uploader_note_response(
            &state,
            a.sequence,
            a.json_crc32,
            BACKEND_HTTP_ACK,
            200,
            pop_result,
            fifo.count,
            0U,
            200));
    TEST_ASSERT_EQUAL_UINT32(1U, state.queue_depth);
    TEST_ASSERT_EQUAL_UINT32(1U, state.ack_count);
    TEST_ASSERT_EQUAL_INT64(200, state.last_backend_success_ms);
    TEST_ASSERT_FALSE(state.in_flight);

    TEST_ASSERT_TRUE(begin_current_head(&state, &fifo, 201));
    TEST_ASSERT_TRUE(state.in_flight);
    TEST_ASSERT_EQUAL_UINT32(b.sequence, state.in_flight_sequence);
    TEST_ASSERT_EQUAL_HEX32(b.json_crc32, state.in_flight_crc32);
}

void test_active_overflow_orphans_a_and_then_arms_real_current_head(void)
{
    backend_upload_batch_t storage[2];
    backend_upload_fifo_t fifo;
    backend_upload_fifo_init(&fifo, storage, 2U);
    backend_uploader_state_t state;
    backend_uploader_state_init(&state);
    const backend_upload_batch_t a = fixture_batch(1U, "{\"a\":1}");
    const backend_upload_batch_t b = fixture_batch(2U, "{\"b\":2}");
    const backend_upload_batch_t c = fixture_batch(3U, "{\"c\":3}");

    push_and_note(&state, &fifo, &a);
    TEST_ASSERT_TRUE(begin_current_head(&state, &fifo, 10));
    push_and_note(&state, &fifo, &b);
    push_and_note(&state, &fifo, &c);

    TEST_ASSERT_EQUAL_UINT32(2U, fifo.count);
    TEST_ASSERT_EQUAL_UINT32(2U, state.queue_depth);
    TEST_ASSERT_EQUAL_UINT32(1U, state.overflow_dropped_count);
    TEST_ASSERT_TRUE(state.in_flight);
    TEST_ASSERT_TRUE(state.in_flight_orphaned);
    TEST_ASSERT_EQUAL_UINT32(a.sequence, state.in_flight_sequence);
    TEST_ASSERT_EQUAL_HEX32(a.json_crc32, state.in_flight_crc32);
    TEST_ASSERT_EQUAL_UINT32(
        b.sequence, backend_upload_fifo_peek(&fifo)->sequence);

    const backend_uploader_queue_result_t pop_result = remove_exact_head(
        &fifo, a.sequence, a.json_crc32, false);
    TEST_ASSERT_EQUAL(BACKEND_UPLOADER_QUEUE_UNCHANGED, pop_result);
    TEST_ASSERT_EQUAL(
        BACKEND_UPLOADER_ACKED,
        backend_uploader_note_response(
            &state,
            a.sequence,
            a.json_crc32,
            BACKEND_HTTP_ACK,
            200,
            pop_result,
            fifo.count,
            0U,
            20));
    TEST_ASSERT_EQUAL_UINT32(2U, state.queue_depth);
    TEST_ASSERT_EQUAL_UINT32(1U, state.ack_count);
    TEST_ASSERT_FALSE(state.in_flight);

    TEST_ASSERT_TRUE(begin_current_head(&state, &fifo, 21));
    TEST_ASSERT_EQUAL_UINT32(b.sequence, state.in_flight_sequence);
    TEST_ASSERT_EQUAL_HEX32(b.json_crc32, state.in_flight_crc32);
}

void test_retry_wait_overflow_cancels_a_and_arms_real_current_head(void)
{
    backend_upload_batch_t storage[2];
    backend_upload_fifo_t fifo;
    backend_upload_fifo_init(&fifo, storage, 2U);
    backend_uploader_state_t state;
    backend_uploader_state_init(&state);
    const backend_upload_batch_t a = fixture_batch(11U, "{\"a\":11}");
    const backend_upload_batch_t b = fixture_batch(12U, "{\"b\":12}");
    const backend_upload_batch_t c = fixture_batch(13U, "{\"c\":13}");

    push_and_note(&state, &fifo, &a);
    TEST_ASSERT_TRUE(begin_current_head(&state, &fifo, 100));
    push_and_note(&state, &fifo, &b);
    TEST_ASSERT_EQUAL(
        BACKEND_UPLOADER_RETRY,
        backend_uploader_note_response(
            &state,
            a.sequence,
            a.json_crc32,
            BACKEND_HTTP_RETRY,
            503,
            BACKEND_UPLOADER_QUEUE_UNCHANGED,
            fifo.count,
            0U,
            200));
    TEST_ASSERT_FALSE(state.in_flight);
    TEST_ASSERT_EQUAL_UINT32(a.sequence, state.in_flight_sequence);
    TEST_ASSERT_EQUAL_INT64(700, state.next_attempt_ms);

    push_and_note(&state, &fifo, &c);
    TEST_ASSERT_EQUAL_UINT32(1U, state.overflow_dropped_count);
    TEST_ASSERT_EQUAL_UINT32(0U, state.in_flight_sequence);
    TEST_ASSERT_EQUAL_HEX32(0U, state.in_flight_crc32);
    TEST_ASSERT_EQUAL_INT64(-1, state.next_attempt_ms);
    TEST_ASSERT_EQUAL_UINT32(
        b.sequence, backend_upload_fifo_peek(&fifo)->sequence);

    TEST_ASSERT_TRUE(begin_current_head(&state, &fifo, 201));
    TEST_ASSERT_EQUAL_UINT32(b.sequence, state.in_flight_sequence);
    TEST_ASSERT_EQUAL_HEX32(b.json_crc32, state.in_flight_crc32);
}

void test_response_sequence_and_crc_must_both_match_request(void)
{
    backend_upload_batch_t storage[1];
    backend_upload_fifo_t fifo;
    backend_upload_fifo_init(&fifo, storage, 1U);
    backend_uploader_state_t state;
    backend_uploader_state_init(&state);
    const backend_upload_batch_t a = fixture_batch(7U, "{\"a\":7}");

    push_and_note(&state, &fifo, &a);
    TEST_ASSERT_TRUE(begin_current_head(&state, &fifo, 10));

    const backend_uploader_queue_result_t wrong_crc_pop = remove_exact_head(
        &fifo, a.sequence, a.json_crc32 ^ UINT32_C(1), false);
    TEST_ASSERT_EQUAL(
        BACKEND_UPLOADER_QUEUE_UNCHANGED, wrong_crc_pop);
    TEST_ASSERT_EQUAL(
        BACKEND_UPLOADER_IGNORED,
        backend_uploader_note_response(
            &state,
            a.sequence,
            a.json_crc32 ^ UINT32_C(1),
            BACKEND_HTTP_ACK,
            200,
            wrong_crc_pop,
            fifo.count,
            0U,
            20));
    TEST_ASSERT_EQUAL_UINT16(1U, fifo.count);
    TEST_ASSERT_EQUAL_UINT32(a.sequence, state.in_flight_sequence);
    TEST_ASSERT_TRUE(state.in_flight);
    TEST_ASSERT_EQUAL_UINT32(0U, state.ack_count);
    TEST_ASSERT_EQUAL_INT64(0, state.last_backend_success_ms);

    TEST_ASSERT_EQUAL(
        BACKEND_UPLOADER_IGNORED,
        backend_uploader_note_response(
            &state,
            a.sequence + 1U,
            a.json_crc32,
            BACKEND_HTTP_ACK,
            200,
            BACKEND_UPLOADER_QUEUE_UNCHANGED,
            fifo.count,
            0U,
            21));
    TEST_ASSERT_EQUAL_UINT16(1U, fifo.count);
    TEST_ASSERT_TRUE(state.in_flight);
}

void test_non_orphan_ack_without_locked_pop_is_ignored(void)
{
    backend_upload_batch_t storage[1];
    backend_upload_fifo_t fifo;
    backend_upload_fifo_init(&fifo, storage, 1U);
    backend_uploader_state_t state;
    backend_uploader_state_init(&state);
    const backend_upload_batch_t a = fixture_batch(31U, "{\"a\":31}");

    push_and_note(&state, &fifo, &a);
    TEST_ASSERT_TRUE(begin_current_head(&state, &fifo, 1));
    TEST_ASSERT_EQUAL(
        BACKEND_UPLOADER_IGNORED,
        backend_uploader_note_response(
            &state,
            a.sequence,
            a.json_crc32,
            BACKEND_HTTP_ACK,
            200,
            BACKEND_UPLOADER_QUEUE_UNCHANGED,
            fifo.count,
            0U,
            2));

    TEST_ASSERT_EQUAL_UINT16(1U, fifo.count);
    TEST_ASSERT_EQUAL_UINT32(1U, state.queue_depth);
    TEST_ASSERT_TRUE(state.in_flight);
    TEST_ASSERT_EQUAL_UINT32(0U, state.ack_count);
    TEST_ASSERT_EQUAL_INT64(0, state.last_backend_success_ms);
    TEST_ASSERT_FALSE(begin_current_head(&state, &fifo, 3));
    TEST_ASSERT_EQUAL_UINT32(a.sequence, state.in_flight_sequence);
}

void test_quarantine_counter_follows_exact_locked_quarantine_result(void)
{
    backend_upload_batch_t storage[1];
    backend_upload_fifo_t fifo;
    backend_upload_fifo_init(&fifo, storage, 1U);
    backend_uploader_state_t state;
    backend_uploader_state_init(&state);
    const backend_upload_batch_t a = fixture_batch(41U, "{\"a\":41}");

    push_and_note(&state, &fifo, &a);
    TEST_ASSERT_TRUE(begin_current_head(&state, &fifo, 10));
    const backend_uploader_queue_result_t quarantine_result =
        remove_exact_head(
            &fifo, a.sequence, a.json_crc32, true);
    TEST_ASSERT_EQUAL(
        BACKEND_UPLOADER_QUEUE_QUARANTINED, quarantine_result);
    TEST_ASSERT_EQUAL(
        BACKEND_UPLOADER_QUARANTINED,
        backend_uploader_note_response(
            &state,
            a.sequence,
            a.json_crc32,
            BACKEND_HTTP_QUARANTINE,
            400,
            quarantine_result,
            fifo.count,
            0U,
            20));
    TEST_ASSERT_EQUAL_UINT32(0U, state.queue_depth);
    TEST_ASSERT_EQUAL_UINT32(1U, state.quarantine_count);
    TEST_ASSERT_EQUAL_UINT32(0U, state.ack_count);
    TEST_ASSERT_EQUAL_INT64(0, state.last_backend_success_ms);
}

void test_orphan_quarantine_completion_keeps_current_head(void)
{
    backend_upload_batch_t storage[1];
    backend_upload_fifo_t fifo;
    backend_upload_fifo_init(&fifo, storage, 1U);
    backend_uploader_state_t state;
    backend_uploader_state_init(&state);
    const backend_upload_batch_t a = fixture_batch(45U, "{\"a\":45}");
    const backend_upload_batch_t b = fixture_batch(46U, "{\"b\":46}");

    push_and_note(&state, &fifo, &a);
    TEST_ASSERT_TRUE(begin_current_head(&state, &fifo, 10));
    push_and_note(&state, &fifo, &b);
    TEST_ASSERT_TRUE(state.in_flight_orphaned);
    TEST_ASSERT_EQUAL_UINT32(
        b.sequence, backend_upload_fifo_peek(&fifo)->sequence);

    TEST_ASSERT_EQUAL(
        BACKEND_UPLOADER_QUARANTINED,
        backend_uploader_note_response(
            &state,
            a.sequence,
            a.json_crc32,
            BACKEND_HTTP_QUARANTINE,
            400,
            BACKEND_UPLOADER_QUEUE_UNCHANGED,
            fifo.count,
            0U,
            20));
    TEST_ASSERT_EQUAL_UINT32(0U, state.quarantine_count);
    TEST_ASSERT_EQUAL_UINT16(1U, fifo.count);
    TEST_ASSERT_TRUE(begin_current_head(&state, &fifo, 21));
    TEST_ASSERT_EQUAL_UINT32(b.sequence, state.in_flight_sequence);
}

void test_inconsistent_locked_result_is_ignored_without_state_change(void)
{
    backend_upload_batch_t storage[1];
    backend_upload_fifo_t fifo;
    backend_upload_fifo_init(&fifo, storage, 1U);
    backend_uploader_state_t state;
    backend_uploader_state_init(&state);
    const backend_upload_batch_t a = fixture_batch(51U, "{\"a\":51}");

    push_and_note(&state, &fifo, &a);
    TEST_ASSERT_TRUE(begin_current_head(&state, &fifo, 10));
    TEST_ASSERT_EQUAL(
        BACKEND_UPLOADER_IGNORED,
        backend_uploader_note_response(
            &state,
            a.sequence,
            a.json_crc32,
            BACKEND_HTTP_ACK,
            200,
            BACKEND_UPLOADER_QUEUE_QUARANTINED,
            fifo.count,
            0U,
            20));
    TEST_ASSERT_TRUE(state.in_flight);
    TEST_ASSERT_EQUAL_UINT32(0U, state.ack_count);
    TEST_ASSERT_EQUAL_UINT32(0U, state.quarantine_count);
    TEST_ASSERT_EQUAL_UINT32(1U, state.queue_depth);

    TEST_ASSERT_EQUAL(
        BACKEND_UPLOADER_IGNORED,
        backend_uploader_note_response(
            &state,
            a.sequence,
            a.json_crc32,
            BACKEND_HTTP_QUARANTINE,
            400,
            BACKEND_UPLOADER_QUEUE_UNCHANGED,
            fifo.count,
            0U,
            21));
    TEST_ASSERT_TRUE(state.in_flight);
    TEST_ASSERT_EQUAL_UINT32(0U, state.quarantine_count);

    TEST_ASSERT_EQUAL(
        BACKEND_UPLOADER_IGNORED,
        backend_uploader_note_response(
            &state,
            a.sequence,
            a.json_crc32,
            BACKEND_HTTP_ACK,
            200,
            (backend_uploader_queue_result_t)99,
            fifo.count,
            0U,
            21));
    TEST_ASSERT_TRUE(state.in_flight);
    TEST_ASSERT_EQUAL_UINT32(0U, state.ack_count);
}

void test_retry_jitter_boundaries_cap_and_int64_saturation(void)
{
    backend_upload_batch_t storage[1];
    backend_upload_fifo_t fifo;
    backend_upload_fifo_init(&fifo, storage, 1U);
    backend_uploader_state_t state;
    backend_uploader_state_init(&state);
    const backend_upload_batch_t a = fixture_batch(61U, "{\"a\":61}");
    push_and_note(&state, &fifo, &a);
    TEST_ASSERT_TRUE(begin_current_head(&state, &fifo, 0));

    TEST_ASSERT_EQUAL(
        BACKEND_UPLOADER_RETRY,
        backend_uploader_note_response(
            &state,
            a.sequence,
            a.json_crc32,
            BACKEND_HTTP_RETRY,
            503,
            BACKEND_UPLOADER_QUEUE_UNCHANGED,
            fifo.count,
            125U,
            1000));
    TEST_ASSERT_EQUAL_INT64(1625, state.next_attempt_ms);
    TEST_ASSERT_FALSE(begin_current_head(&state, &fifo, 1624));
    TEST_ASSERT_TRUE(begin_current_head(&state, &fifo, 1625));

    int64_t now_ms = 2000;
    const int64_t base_delays[] = {
        1000, 2000, 4000, 8000, 16000, 32000, 60000,
    };
    for (size_t index = 0U;
         index < sizeof(base_delays) / sizeof(base_delays[0]);
         ++index) {
        TEST_ASSERT_EQUAL(
            BACKEND_UPLOADER_RETRY,
            backend_uploader_note_response(
                &state,
                a.sequence,
                a.json_crc32,
                BACKEND_HTTP_RETRY,
                503,
                BACKEND_UPLOADER_QUEUE_UNCHANGED,
                fifo.count,
                0U,
                now_ms));
        TEST_ASSERT_EQUAL_INT64(
            now_ms + base_delays[index], state.next_attempt_ms);
        TEST_ASSERT_TRUE(begin_current_head(
            &state, &fifo, state.next_attempt_ms));
        now_ms = state.next_attempt_ms + 1;
    }
    TEST_ASSERT_EQUAL_UINT8(7U, state.retry_exponent);

    TEST_ASSERT_EQUAL(
        BACKEND_UPLOADER_RETRY,
        backend_uploader_note_response(
            &state,
            a.sequence,
            a.json_crc32,
            BACKEND_HTTP_RETRY,
            503,
            BACKEND_UPLOADER_QUEUE_UNCHANGED,
            fifo.count,
            15000U,
            now_ms));
    TEST_ASSERT_EQUAL_INT64(now_ms + 75000, state.next_attempt_ms);
    TEST_ASSERT_EQUAL_UINT8(7U, state.retry_exponent);
    TEST_ASSERT_TRUE(begin_current_head(
        &state, &fifo, state.next_attempt_ms));

    TEST_ASSERT_EQUAL(
        BACKEND_UPLOADER_RETRY,
        backend_uploader_note_response(
            &state,
            a.sequence,
            a.json_crc32,
            BACKEND_HTTP_RETRY,
            503,
            BACKEND_UPLOADER_QUEUE_UNCHANGED,
            fifo.count,
            0U,
            INT64_MAX - 100));
    TEST_ASSERT_EQUAL_INT64(INT64_MAX, state.next_attempt_ms);
    TEST_ASSERT_FALSE(begin_current_head(
        &state, &fifo, INT64_MAX - 1));
    TEST_ASSERT_TRUE(begin_current_head(
        &state, &fifo, INT64_MAX));
    TEST_ASSERT_EQUAL_UINT32(1U, state.queue_depth);
}

void test_queue_counters_use_explicit_events_and_saturate(void)
{
    backend_uploader_state_t state;
    backend_uploader_state_init(&state);

    TEST_ASSERT_FALSE(backend_uploader_note_enqueued(
        &state, 0U, false, 0U, 0U));
    TEST_ASSERT_FALSE(backend_uploader_note_enqueued(
        &state, 1U, false, 1U, UINT32_C(0x1234)));
    TEST_ASSERT_FALSE(backend_uploader_note_enqueued(
        &state, 1U, true, 0U, UINT32_C(0x1234)));
    TEST_ASSERT_EQUAL_UINT32(0U, state.queued_count);
    TEST_ASSERT_EQUAL_UINT32(0U, state.overflow_dropped_count);

    state.queued_count = UINT32_MAX;
    state.overflow_dropped_count = UINT32_MAX;
    TEST_ASSERT_TRUE(backend_uploader_note_enqueued(
        &state, 2U, true, 7U, 0U));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, state.queued_count);
    TEST_ASSERT_EQUAL_UINT32(
        UINT32_MAX, state.overflow_dropped_count);
    TEST_ASSERT_EQUAL_UINT32(2U, state.queue_depth);
}

void test_null_and_invalid_head_events_do_not_mutate_state(void)
{
    backend_uploader_state_t state;
    backend_uploader_state_init(&state);
    const backend_upload_batch_t invalid = fixture_batch(0U, "{}");

    TEST_ASSERT_FALSE(backend_uploader_note_enqueued(
        NULL, 1U, false, 0U, 0U));
    TEST_ASSERT_FALSE(backend_uploader_begin_head(
        NULL, NULL, 0U, 0));
    TEST_ASSERT_FALSE(backend_uploader_begin_head(
        &state, NULL, 0U, 0));
    TEST_ASSERT_FALSE(backend_uploader_begin_head(
        &state, &invalid, 1U, 0));
    TEST_ASSERT_EQUAL(
        BACKEND_UPLOADER_IGNORED,
        backend_uploader_note_response(
            NULL,
            1U,
            1U,
            BACKEND_HTTP_ACK,
            200,
            BACKEND_UPLOADER_QUEUE_UNCHANGED,
            0U,
            0U,
            0));
    TEST_ASSERT_EQUAL_UINT32(0U, state.queue_depth);
    TEST_ASSERT_FALSE(state.in_flight);
    TEST_ASSERT_EQUAL_INT64(-1, state.next_attempt_ms);
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(test_ack_a_then_explicit_locked_peek_arms_b);
    BACKEND_RUN_TEST(
        test_active_overflow_orphans_a_and_then_arms_real_current_head);
    BACKEND_RUN_TEST(
        test_retry_wait_overflow_cancels_a_and_arms_real_current_head);
    BACKEND_RUN_TEST(
        test_response_sequence_and_crc_must_both_match_request);
    BACKEND_RUN_TEST(
        test_non_orphan_ack_without_locked_pop_is_ignored);
    BACKEND_RUN_TEST(
        test_quarantine_counter_follows_exact_locked_quarantine_result);
    BACKEND_RUN_TEST(
        test_orphan_quarantine_completion_keeps_current_head);
    BACKEND_RUN_TEST(
        test_inconsistent_locked_result_is_ignored_without_state_change);
    BACKEND_RUN_TEST(
        test_retry_jitter_boundaries_cap_and_int64_saturation);
    BACKEND_RUN_TEST(
        test_queue_counters_use_explicit_events_and_saturate);
    BACKEND_RUN_TEST(
        test_null_and_invalid_head_events_do_not_mutate_state);
    return UNITY_END();
}
