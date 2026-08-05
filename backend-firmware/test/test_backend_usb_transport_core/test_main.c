#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "backend_usb_transport_core.h"
#include "../support/backend_test_main.h"

static backend_usb_required_frame_t
    s_required[BACKEND_USB_REQUIRED_QUEUE_CAPACITY];
static backend_usb_optional_frame_t
    s_optional[BACKEND_USB_OPTIONAL_QUEUE_CAPACITY];
static backend_usb_transport_core_t s_transport;

void setUp(void)
{
    memset(s_required, 0xA5, sizeof(s_required));
    memset(s_optional, 0xA5, sizeof(s_optional));
    memset(&s_transport, 0xA5, sizeof(s_transport));
    TEST_ASSERT_TRUE(backend_usb_transport_init(
        &s_transport,
        s_required, BACKEND_USB_REQUIRED_QUEUE_CAPACITY,
        s_optional, BACKEND_USB_OPTIONAL_QUEUE_CAPACITY));
}

void tearDown(void)
{
}

static void assert_frame(
    const backend_usb_frame_t *frame,
    backend_usb_frame_priority_t priority,
    backend_usb_frame_kind_t kind,
    uint64_t correlation_sequence,
    const char *expected)
{
    TEST_ASSERT_EQUAL(priority, frame->priority);
    TEST_ASSERT_EQUAL(kind, frame->kind);
    TEST_ASSERT_EQUAL_UINT64(correlation_sequence,
                             frame->correlation_sequence);
    TEST_ASSERT_EQUAL_UINT32(strlen(expected), frame->length);
    TEST_ASSERT_EQUAL_MEMORY(expected, frame->bytes, frame->length);
}

void test_required_frames_pop_before_optional_while_each_queue_stays_fifo(void)
{
    TEST_ASSERT_TRUE(backend_usb_transport_enqueue(
        &s_transport, BACKEND_USB_FRAME_OPTIONAL,
        BACKEND_USB_FRAME_GENERIC, 11, "optional-one\n", 13));
    TEST_ASSERT_TRUE(backend_usb_transport_enqueue(
        &s_transport, BACKEND_USB_FRAME_REQUIRED,
        BACKEND_USB_FRAME_GENERIC, 21, "required-one\n", 13));
    TEST_ASSERT_TRUE(backend_usb_transport_enqueue(
        &s_transport, BACKEND_USB_FRAME_OPTIONAL,
        BACKEND_USB_FRAME_GENERIC, 12, "optional-two\n", 13));
    TEST_ASSERT_TRUE(backend_usb_transport_enqueue_live(
        &s_transport, BACKEND_USB_FRAME_REQUIRED,
        BACKEND_USB_FRAME_LIVE_HEARTBEAT, 22, 1U,
        "required-two\n", 13));

    backend_usb_frame_t frame;
    TEST_ASSERT_TRUE(backend_usb_transport_pop(&s_transport, &frame));
    assert_frame(&frame, BACKEND_USB_FRAME_REQUIRED,
                 BACKEND_USB_FRAME_GENERIC, 21, "required-one\n");
    TEST_ASSERT_TRUE(backend_usb_transport_pop(&s_transport, &frame));
    assert_frame(&frame, BACKEND_USB_FRAME_REQUIRED,
                 BACKEND_USB_FRAME_LIVE_HEARTBEAT, 22, "required-two\n");
    TEST_ASSERT_TRUE(backend_usb_transport_pop(&s_transport, &frame));
    assert_frame(&frame, BACKEND_USB_FRAME_OPTIONAL,
                 BACKEND_USB_FRAME_GENERIC, 11, "optional-one\n");
    TEST_ASSERT_TRUE(backend_usb_transport_pop(&s_transport, &frame));
    assert_frame(&frame, BACKEND_USB_FRAME_OPTIONAL,
                 BACKEND_USB_FRAME_GENERIC, 12, "optional-two\n");
    TEST_ASSERT_FALSE(backend_usb_transport_pop(&s_transport, &frame));
}

void test_optional_full_queue_drops_whole_new_frame_without_corrupting_old_frames(void)
{
    char frame[32];
    for (size_t index = 0; index < BACKEND_USB_OPTIONAL_QUEUE_CAPACITY;
         ++index) {
        const int length = snprintf(
            frame, sizeof(frame), "FOF_DET:%02u\n", (unsigned)index);
        TEST_ASSERT_GREATER_THAN(0, length);
        TEST_ASSERT_TRUE(backend_usb_transport_enqueue(
            &s_transport, BACKEND_USB_FRAME_OPTIONAL,
            BACKEND_USB_FRAME_GENERIC, index, frame, (size_t)length));
    }
    TEST_ASSERT_EQUAL_UINT32(
        BACKEND_USB_OPTIONAL_QUEUE_CAPACITY, s_transport.optional_count);
    TEST_ASSERT_FALSE(backend_usb_transport_enqueue(
        &s_transport, BACKEND_USB_FRAME_OPTIONAL,
        BACKEND_USB_FRAME_GENERIC, 99, "DROP-ME\n", 8));
    TEST_ASSERT_EQUAL_UINT64(1, s_transport.optional_drops);
    TEST_ASSERT_EQUAL_UINT32(
        BACKEND_USB_OPTIONAL_QUEUE_CAPACITY, s_transport.optional_count);

    backend_usb_frame_t popped;
    TEST_ASSERT_TRUE(backend_usb_transport_pop(&s_transport, &popped));
    assert_frame(&popped, BACKEND_USB_FRAME_OPTIONAL,
                 BACKEND_USB_FRAME_GENERIC, 0, "FOF_DET:00\n");
}

void test_required_full_queue_fails_bounded_and_counts_failure(void)
{
    for (size_t index = 0; index < BACKEND_USB_REQUIRED_QUEUE_CAPACITY;
         ++index) {
        TEST_ASSERT_TRUE(backend_usb_transport_enqueue(
            &s_transport, BACKEND_USB_FRAME_REQUIRED,
            BACKEND_USB_FRAME_GENERIC, index, "R\n", 2));
    }
    TEST_ASSERT_FALSE(backend_usb_transport_enqueue(
        &s_transport, BACKEND_USB_FRAME_REQUIRED,
        BACKEND_USB_FRAME_GENERIC, 99, "NEVER\n", 6));
    TEST_ASSERT_EQUAL_UINT64(1, s_transport.required_failures);
    TEST_ASSERT_EQUAL_UINT32(
        BACKEND_USB_REQUIRED_QUEUE_CAPACITY, s_transport.required_count);
}

void test_oversize_frames_are_rejected_before_queue_mutation(void)
{
    static char required[BACKEND_USB_STATUS_MAX + 1U];
    static char optional[BACKEND_USB_DET_MAX + 1U];
    memset(required, 'R', sizeof(required));
    memset(optional, 'O', sizeof(optional));

    TEST_ASSERT_FALSE(backend_usb_transport_enqueue(
        &s_transport, BACKEND_USB_FRAME_REQUIRED,
        BACKEND_USB_FRAME_GENERIC, 1,
        required, sizeof(required)));
    TEST_ASSERT_FALSE(backend_usb_transport_enqueue(
        &s_transport, BACKEND_USB_FRAME_OPTIONAL,
        BACKEND_USB_FRAME_GENERIC, 2,
        optional, sizeof(optional)));
    TEST_ASSERT_EQUAL_UINT32(0, s_transport.required_count);
    TEST_ASSERT_EQUAL_UINT32(0, s_transport.optional_count);
    TEST_ASSERT_EQUAL_UINT64(1, s_transport.required_failures);
    TEST_ASSERT_EQUAL_UINT64(1, s_transport.optional_drops);
}

void test_exact_status_ceiling_is_accepted_and_preserved(void)
{
    static char required[BACKEND_USB_STATUS_MAX];
    memset(required, 'R', sizeof(required));
    required[sizeof(required) - 1U] = '\n';

    TEST_ASSERT_TRUE(backend_usb_transport_enqueue(
        &s_transport, BACKEND_USB_FRAME_REQUIRED,
        BACKEND_USB_FRAME_GENERIC, 7,
        required, sizeof(required)));
    backend_usb_frame_t popped;
    TEST_ASSERT_TRUE(backend_usb_transport_pop(&s_transport, &popped));
    TEST_ASSERT_EQUAL_size_t(BACKEND_USB_STATUS_MAX, popped.length);
    TEST_ASSERT_EQUAL_CHAR('R', popped.bytes[0]);
    TEST_ASSERT_EQUAL_CHAR('\n', popped.bytes[popped.length - 1U]);
}

void test_live_start_is_unconfirmed_and_first_heartbeat_is_immediate(void)
{
    backend_usb_live_state_t state;
    memset(&state, 0xA5, sizeof(state));
    TEST_ASSERT_TRUE(backend_usb_live_start(&state, "boot-a1", 100));
    TEST_ASSERT_TRUE(state.started);
    TEST_ASSERT_FALSE(state.confirmed);
    TEST_ASSERT_FALSE(backend_usb_live_confirmed(&state, 100));

    uint64_t sequence = 0;
    TEST_ASSERT_TRUE(backend_usb_live_prepare_heartbeat(
        &state, 100, &sequence));
    TEST_ASSERT_EQUAL_UINT64(1, sequence);
    TEST_ASSERT_TRUE(state.heartbeat_pending);
    TEST_ASSERT_EQUAL_UINT64(1, state.pending_heartbeat_sequence);
    TEST_ASSERT_FALSE(backend_usb_live_prepare_heartbeat(
        &state, 100, &sequence));
}

void test_failed_heartbeat_releases_same_sequence_for_immediate_retry(void)
{
    backend_usb_live_state_t state;
    TEST_ASSERT_TRUE(backend_usb_live_start(&state, "boot-a1", 100));
    uint64_t sequence = 0;
    TEST_ASSERT_TRUE(backend_usb_live_prepare_heartbeat(
        &state, 100, &sequence));
    TEST_ASSERT_EQUAL_UINT64(1, sequence);

    backend_usb_live_note_heartbeat_failed(&state, sequence);
    TEST_ASSERT_FALSE(state.heartbeat_pending);
    sequence = 0;
    TEST_ASSERT_TRUE(backend_usb_live_prepare_heartbeat(
        &state, 101, &sequence));
    TEST_ASSERT_EQUAL_UINT64(1, sequence);
}

void test_completed_heartbeats_schedule_the_next_at_exactly_5000_ms(void)
{
    backend_usb_live_state_t state;
    TEST_ASSERT_TRUE(backend_usb_live_start(&state, "boot-a1", 100));
    uint64_t sequence = 0;
    TEST_ASSERT_TRUE(backend_usb_live_prepare_heartbeat(
        &state, 100, &sequence));
    TEST_ASSERT_TRUE(backend_usb_live_note_heartbeat_sent(
        &state, sequence, 125));
    TEST_ASSERT_EQUAL_UINT64(1, state.last_sent_sequence);
    TEST_ASSERT_EQUAL_INT64(5125, state.next_heartbeat_ms);
    TEST_ASSERT_FALSE(backend_usb_live_prepare_heartbeat(
        &state, 5124, &sequence));
    TEST_ASSERT_TRUE(backend_usb_live_prepare_heartbeat(
        &state, 5125, &sequence));
    TEST_ASSERT_EQUAL_UINT64(2, sequence);
}

void test_enqueue_pop_or_partial_tx_never_makes_heartbeat_acknowledgeable(void)
{
    TEST_ASSERT_TRUE(backend_usb_live_start(
        &s_transport.live, "boot-a1", 0));
    uint64_t sequence = 0;
    TEST_ASSERT_TRUE(backend_usb_live_prepare_heartbeat(
        &s_transport.live, 0, &sequence));
    TEST_ASSERT_TRUE(backend_usb_transport_enqueue_live(
        &s_transport, BACKEND_USB_FRAME_REQUIRED,
        BACKEND_USB_FRAME_LIVE_HEARTBEAT, sequence, 1U,
        "FOF_LIVE_HEARTBEAT:{}\n", 22));
    TEST_ASSERT_FALSE(backend_usb_live_acknowledge(
        &s_transport.live, "boot-a1", sequence, 1));

    backend_usb_frame_t popped;
    TEST_ASSERT_TRUE(backend_usb_transport_pop(&s_transport, &popped));
    TEST_ASSERT_EQUAL(BACKEND_USB_FRAME_LIVE_HEARTBEAT, popped.kind);
    TEST_ASSERT_FALSE(backend_usb_live_acknowledge(
        &s_transport.live, "boot-a1", sequence, 2));

    TEST_ASSERT_TRUE(backend_usb_live_note_heartbeat_sent(
        &s_transport.live, sequence, 3));
    TEST_ASSERT_TRUE(backend_usb_live_acknowledge(
        &s_transport.live, "boot-a1", sequence, 4));
    TEST_ASSERT_TRUE(backend_usb_live_confirmed(&s_transport.live, 4));
}

static uint64_t send_heartbeat(
    backend_usb_live_state_t *state, int64_t prepare_ms, int64_t sent_ms)
{
    uint64_t sequence = 0;
    TEST_ASSERT_TRUE(backend_usb_live_prepare_heartbeat(
        state, prepare_ms, &sequence));
    TEST_ASSERT_TRUE(backend_usb_live_note_heartbeat_sent(
        state, sequence, sent_ms));
    return sequence;
}

void test_ack_rejects_wrong_stale_future_and_duplicate_sequences(void)
{
    backend_usb_live_state_t state;
    TEST_ASSERT_TRUE(backend_usb_live_start(&state, "boot-a1", 0));
    const uint64_t first = send_heartbeat(&state, 0, 0);
    TEST_ASSERT_FALSE(backend_usb_live_acknowledge(
        &state, "boot-old", first, 10));
    TEST_ASSERT_FALSE(backend_usb_live_acknowledge(
        &state, "boot-a1", first + 1U, 10));
    TEST_ASSERT_TRUE(backend_usb_live_acknowledge(
        &state, "boot-a1", first, 10));
    const int64_t lease = state.lease_expires_ms;
    TEST_ASSERT_FALSE(backend_usb_live_acknowledge(
        &state, "boot-a1", first, 20));
    TEST_ASSERT_FALSE(backend_usb_live_acknowledge(
        &state, "boot-a1", 0, 20));
    TEST_ASSERT_EQUAL_INT64(lease, state.lease_expires_ms);
}

void test_lease_is_valid_at_14999_and_fail_open_at_exact_15000_boundary(void)
{
    backend_usb_live_state_t state;
    TEST_ASSERT_TRUE(backend_usb_live_start(&state, "boot-a1", 0));
    const uint64_t first = send_heartbeat(&state, 0, 0);
    TEST_ASSERT_TRUE(backend_usb_live_acknowledge(
        &state, "boot-a1", first, 100));
    TEST_ASSERT_EQUAL_INT64(15100, state.lease_expires_ms);
    TEST_ASSERT_TRUE(backend_usb_live_confirmed(&state, 15099));
    TEST_ASSERT_FALSE(backend_usb_live_confirmed(&state, 15100));
    TEST_ASSERT_FALSE(state.confirmed);
}

void test_only_newer_completed_ack_renews_an_expired_lease(void)
{
    backend_usb_live_state_t state;
    TEST_ASSERT_TRUE(backend_usb_live_start(&state, "boot-a1", 0));
    const uint64_t first = send_heartbeat(&state, 0, 0);
    TEST_ASSERT_TRUE(backend_usb_live_acknowledge(
        &state, "boot-a1", first, 100));
    const uint64_t second = send_heartbeat(&state, 5000, 5000);
    TEST_ASSERT_FALSE(backend_usb_live_confirmed(&state, 15100));
    TEST_ASSERT_FALSE(backend_usb_live_acknowledge(
        &state, "boot-a1", first, 15100));
    TEST_ASSERT_TRUE(backend_usb_live_acknowledge(
        &state, "boot-a1", second, 15100));
    TEST_ASSERT_TRUE(backend_usb_live_confirmed(&state, 30099));
    TEST_ASSERT_FALSE(backend_usb_live_confirmed(&state, 30100));
}

void test_stop_clears_live_state_and_new_session_invalidates_old_ack(void)
{
    backend_usb_live_state_t state;
    TEST_ASSERT_TRUE(backend_usb_live_start(&state, "boot-a1", 0));
    const uint64_t first = send_heartbeat(&state, 0, 0);
    TEST_ASSERT_TRUE(backend_usb_live_acknowledge(
        &state, "boot-a1", first, 1));

    TEST_ASSERT_TRUE(backend_usb_live_start(&state, "boot-b2", 2));
    TEST_ASSERT_FALSE(state.confirmed);
    TEST_ASSERT_EQUAL_UINT64(0, state.last_sent_sequence);
    TEST_ASSERT_FALSE(backend_usb_live_acknowledge(
        &state, "boot-a1", first, 3));
    TEST_ASSERT_FALSE(backend_usb_live_acknowledge(
        &state, "boot-b2", first, 3));

    backend_usb_live_stop(&state);
    TEST_ASSERT_FALSE(state.started);
    TEST_ASSERT_FALSE(state.confirmed);
    TEST_ASSERT_EQUAL_STRING("", state.session_id);
    TEST_ASSERT_FALSE(backend_usb_live_prepare_heartbeat(
        &state, 4, &(uint64_t){0}));
}

void test_deadlines_saturate_without_wrapping_and_still_expire_at_boundary(void)
{
    backend_usb_live_state_t state;
    TEST_ASSERT_TRUE(backend_usb_live_start(
        &state, "boot-a1", INT64_MAX - 100));
    const uint64_t first = send_heartbeat(
        &state, INT64_MAX - 100, INT64_MAX - 100);
    TEST_ASSERT_EQUAL_INT64(INT64_MAX, state.next_heartbeat_ms);
    TEST_ASSERT_TRUE(backend_usb_live_acknowledge(
        &state, "boot-a1", first, INT64_MAX - 50));
    TEST_ASSERT_EQUAL_INT64(INT64_MAX, state.lease_expires_ms);
    TEST_ASSERT_TRUE(backend_usb_live_confirmed(&state, INT64_MAX - 1));
    TEST_ASSERT_FALSE(backend_usb_live_confirmed(&state, INT64_MAX));
}

void test_partial_heartbeat_failure_blocks_later_frames_until_newline_recovery(void)
{
    TEST_ASSERT_TRUE(backend_usb_live_start(
        &s_transport.live, "session-a", 0));
    uint64_t sequence = 0;
    TEST_ASSERT_TRUE(backend_usb_live_prepare_heartbeat(
        &s_transport.live, 0, &sequence));
    TEST_ASSERT_TRUE(backend_usb_transport_enqueue_live(
        &s_transport,
        BACKEND_USB_FRAME_REQUIRED,
        BACKEND_USB_FRAME_LIVE_HEARTBEAT,
        sequence,
        7U,
        "FOF_LIVE_HEARTBEAT:{}\n",
        sizeof("FOF_LIVE_HEARTBEAT:{}\n") - 1U));
    TEST_ASSERT_TRUE(backend_usb_transport_enqueue(
        &s_transport,
        BACKEND_USB_FRAME_REQUIRED,
        BACKEND_USB_FRAME_GENERIC,
        0U,
        "FOF_STATUS:{}\n",
        14U));

    backend_usb_frame_t frame;
    TEST_ASSERT_TRUE(backend_usb_transport_pop_current(
        &s_transport, 7U, &frame));
    TEST_ASSERT_EQUAL(BACKEND_USB_FRAME_LIVE_HEARTBEAT, frame.kind);
    backend_usb_transport_note_tx_failed(
        &s_transport, &frame, 7U, true);

    TEST_ASSERT_TRUE(s_transport.output_poisoned);
    TEST_ASSERT_FALSE(s_transport.live.heartbeat_pending);
    TEST_ASSERT_FALSE(backend_usb_transport_pop_current(
        &s_transport, 7U, &frame));

    backend_usb_transport_note_output_recovered(&s_transport);
    TEST_ASSERT_FALSE(s_transport.output_poisoned);
    TEST_ASSERT_TRUE(backend_usb_transport_pop_current(
        &s_transport, 7U, &frame));
    TEST_ASSERT_EQUAL(BACKEND_USB_FRAME_GENERIC, frame.kind);
}

void test_stale_live_ready_is_rejected_for_restart_and_stop_generations(void)
{
    TEST_ASSERT_TRUE(backend_usb_transport_enqueue_live(
        &s_transport,
        BACKEND_USB_FRAME_REQUIRED,
        BACKEND_USB_FRAME_LIVE_READY,
        0U,
        1U,
        "FOF_LIVE_READY:{\"session_id\":\"old\"}\n",
        sizeof("FOF_LIVE_READY:{\"session_id\":\"old\"}\n") - 1U));
    TEST_ASSERT_TRUE(backend_usb_transport_enqueue_live(
        &s_transport,
        BACKEND_USB_FRAME_REQUIRED,
        BACKEND_USB_FRAME_LIVE_READY,
        0U,
        2U,
        "FOF_LIVE_READY:{\"session_id\":\"new\"}\n",
        sizeof("FOF_LIVE_READY:{\"session_id\":\"new\"}\n") - 1U));

    backend_usb_frame_t frame;
    TEST_ASSERT_TRUE(backend_usb_transport_pop_current(
        &s_transport, 2U, &frame));
    TEST_ASSERT_EQUAL(BACKEND_USB_FRAME_LIVE_READY, frame.kind);
    TEST_ASSERT_EQUAL_UINT64(2U, frame.live_generation);
    TEST_ASSERT_EQUAL_MEMORY(
        "FOF_LIVE_READY:{\"session_id\":\"new\"}\n",
        frame.bytes,
        frame.length);

    TEST_ASSERT_TRUE(backend_usb_transport_enqueue_live(
        &s_transport,
        BACKEND_USB_FRAME_REQUIRED,
        BACKEND_USB_FRAME_LIVE_READY,
        0U,
        3U,
        "FOF_LIVE_READY:{\"session_id\":\"stop\"}\n",
        sizeof("FOF_LIVE_READY:{\"session_id\":\"stop\"}\n") - 1U));
    TEST_ASSERT_FALSE(backend_usb_transport_pop_current(
        &s_transport, 4U, &frame));
}

void test_forced_lock_contention_is_counted_without_taking_queue_lock(void)
{
    backend_usb_transport_note_lock_failure(
        &s_transport, BACKEND_USB_FRAME_REQUIRED);
    backend_usb_transport_note_lock_failure(
        &s_transport, BACKEND_USB_FRAME_REQUIRED);
    backend_usb_transport_note_lock_failure(
        &s_transport, BACKEND_USB_FRAME_OPTIONAL);

    TEST_ASSERT_EQUAL_UINT64(
        2U, backend_usb_transport_required_contention_failures(&s_transport));
    TEST_ASSERT_EQUAL_UINT64(
        1U, backend_usb_transport_optional_contention_drops(&s_transport));
}

void test_delayed_unacked_heartbeat_cannot_mint_a_fresh_lease(void)
{
    backend_usb_live_state_t state;
    TEST_ASSERT_TRUE(backend_usb_live_start(&state, "session-a", 0));
    const uint64_t sequence = send_heartbeat(&state, 0, 10);

    TEST_ASSERT_FALSE(backend_usb_live_acknowledge(
        &state, "session-a", sequence, 15011));
    TEST_ASSERT_FALSE(state.confirmed);
}

void test_older_sent_sequence_is_rejected_after_a_newer_heartbeat(void)
{
    backend_usb_live_state_t state;
    TEST_ASSERT_TRUE(backend_usb_live_start(&state, "session-a", 0));
    const uint64_t first = send_heartbeat(&state, 0, 0);
    const uint64_t second = send_heartbeat(&state, 5000, 5000);

    TEST_ASSERT_FALSE(backend_usb_live_acknowledge(
        &state, "session-a", first, 5001));
    TEST_ASSERT_TRUE(backend_usb_live_acknowledge(
        &state, "session-a", second, 5001));
}

void test_latest_heartbeat_ack_is_accepted_just_before_freshness_deadline(void)
{
    backend_usb_live_state_t state;
    TEST_ASSERT_TRUE(backend_usb_live_start(&state, "session-a", 0));
    const uint64_t sequence = send_heartbeat(&state, 0, 100);

    TEST_ASSERT_TRUE(backend_usb_live_acknowledge(
        &state, "session-a", sequence, 15099));
    TEST_ASSERT_TRUE(state.confirmed);
}

void test_latest_heartbeat_ack_fails_open_at_exact_freshness_deadline(void)
{
    backend_usb_live_state_t state;
    TEST_ASSERT_TRUE(backend_usb_live_start(&state, "session-a", 0));
    const uint64_t sequence = send_heartbeat(&state, 0, 100);

    TEST_ASSERT_FALSE(backend_usb_live_acknowledge(
        &state, "session-a", sequence, 15100));
    TEST_ASSERT_FALSE(state.confirmed);
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(test_required_frames_pop_before_optional_while_each_queue_stays_fifo);
    BACKEND_RUN_TEST(test_optional_full_queue_drops_whole_new_frame_without_corrupting_old_frames);
    BACKEND_RUN_TEST(test_required_full_queue_fails_bounded_and_counts_failure);
    BACKEND_RUN_TEST(test_oversize_frames_are_rejected_before_queue_mutation);
    BACKEND_RUN_TEST(test_exact_status_ceiling_is_accepted_and_preserved);
    BACKEND_RUN_TEST(test_live_start_is_unconfirmed_and_first_heartbeat_is_immediate);
    BACKEND_RUN_TEST(test_failed_heartbeat_releases_same_sequence_for_immediate_retry);
    BACKEND_RUN_TEST(test_completed_heartbeats_schedule_the_next_at_exactly_5000_ms);
    BACKEND_RUN_TEST(test_enqueue_pop_or_partial_tx_never_makes_heartbeat_acknowledgeable);
    BACKEND_RUN_TEST(test_ack_rejects_wrong_stale_future_and_duplicate_sequences);
    BACKEND_RUN_TEST(test_lease_is_valid_at_14999_and_fail_open_at_exact_15000_boundary);
    BACKEND_RUN_TEST(test_only_newer_completed_ack_renews_an_expired_lease);
    BACKEND_RUN_TEST(test_stop_clears_live_state_and_new_session_invalidates_old_ack);
    BACKEND_RUN_TEST(test_deadlines_saturate_without_wrapping_and_still_expire_at_boundary);
    BACKEND_RUN_TEST(test_partial_heartbeat_failure_blocks_later_frames_until_newline_recovery);
    BACKEND_RUN_TEST(test_stale_live_ready_is_rejected_for_restart_and_stop_generations);
    BACKEND_RUN_TEST(test_forced_lock_contention_is_counted_without_taking_queue_lock);
    BACKEND_RUN_TEST(test_delayed_unacked_heartbeat_cannot_mint_a_fresh_lease);
    BACKEND_RUN_TEST(test_older_sent_sequence_is_rejected_after_a_newer_heartbeat);
    BACKEND_RUN_TEST(test_latest_heartbeat_ack_is_accepted_just_before_freshness_deadline);
    BACKEND_RUN_TEST(test_latest_heartbeat_ack_fails_open_at_exact_freshness_deadline);
    return UNITY_END();
}
