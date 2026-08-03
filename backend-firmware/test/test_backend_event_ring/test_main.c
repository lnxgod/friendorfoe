#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "backend_event_ring.h"
#include "../support/backend_test_main.h"

void setUp(void) {}
void tearDown(void) {}

static backend_dashboard_event_t storage[128];
static backend_dashboard_event_t output[128];

static backend_dashboard_event_t fixture_event(uint32_t value)
{
    backend_dashboard_event_t event = {0};
    snprintf(event.id, sizeof(event.id), "event-%u", (unsigned)value);
    event.threat_score = (uint8_t)(value % 101U);
    return event;
}

void test_ring_requires_exact_caller_owned_capacity_and_starts_at_one(void)
{
    backend_event_ring_t ring;
    memset(&ring, 0xA5, sizeof(ring));
    memset(storage, 0, sizeof(storage));

    TEST_ASSERT_FALSE(backend_event_ring_init(NULL, storage, 128U));
    TEST_ASSERT_FALSE(backend_event_ring_init(&ring, NULL, 128U));
    TEST_ASSERT_FALSE(backend_event_ring_init(&ring, storage, 127U));
    TEST_ASSERT_FALSE(backend_event_ring_init(&ring, storage, 129U));
    TEST_ASSERT_TRUE(backend_event_ring_init(&ring, storage, 128U));
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(1), ring.next_sequence);
    TEST_ASSERT_EQUAL_size_t(0U, ring.count);

    backend_dashboard_event_t event = fixture_event(1U);
    TEST_ASSERT_TRUE(backend_event_ring_append(&ring, &event));
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(1), storage[0].sequence);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(2), ring.next_sequence);
}

void test_ring_overwrites_oldest_at_128_records(void)
{
    backend_event_ring_t ring;
    TEST_ASSERT_TRUE(backend_event_ring_init(&ring, storage, 128U));
    for (uint32_t value = 1U; value <= 129U; ++value) {
        backend_dashboard_event_t event = fixture_event(value);
        TEST_ASSERT_TRUE(backend_event_ring_append(&ring, &event));
    }

    backend_event_ring_snapshot_t snapshot = {0};
    TEST_ASSERT_TRUE(backend_event_ring_snapshot(
        &ring, 0U, 128U, output, 128U, &snapshot));
    TEST_ASSERT_EQUAL_size_t(128U, snapshot.count);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(2), snapshot.oldest_sequence);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(129), snapshot.newest_sequence);
    TEST_ASSERT_TRUE(snapshot.cursor_reset);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(2), output[0].sequence);
    TEST_ASSERT_EQUAL_STRING("event-2", output[0].id);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(129), output[127].sequence);
    TEST_ASSERT_EQUAL_STRING("event-129", output[127].id);
}

void test_ring_clips_to_limit_and_output_capacity(void)
{
    backend_event_ring_t ring;
    TEST_ASSERT_TRUE(backend_event_ring_init(&ring, storage, 128U));
    for (uint32_t value = 1U; value <= 6U; ++value) {
        backend_dashboard_event_t event = fixture_event(value);
        TEST_ASSERT_TRUE(backend_event_ring_append(&ring, &event));
    }

    backend_event_ring_snapshot_t snapshot = {0};
    TEST_ASSERT_TRUE(backend_event_ring_snapshot(
        &ring, UINT64_C(1), 3U, output, 10U, &snapshot));
    TEST_ASSERT_EQUAL_size_t(3U, snapshot.count);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(2), output[0].sequence);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(4), output[2].sequence);

    TEST_ASSERT_TRUE(backend_event_ring_snapshot(
        &ring, UINT64_C(1), 10U, output, 2U, &snapshot));
    TEST_ASSERT_EQUAL_size_t(2U, snapshot.count);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(2), output[0].sequence);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(3), output[1].sequence);

    TEST_ASSERT_TRUE(backend_event_ring_snapshot(
        &ring, UINT64_C(1), 0U, NULL, 0U, &snapshot));
    TEST_ASSERT_EQUAL_size_t(0U, snapshot.count);
}

void test_ring_stale_cursor_resets_but_ahead_cursor_is_empty(void)
{
    backend_event_ring_t ring;
    TEST_ASSERT_TRUE(backend_event_ring_init(&ring, storage, 128U));
    for (uint32_t value = 1U; value <= 130U; ++value) {
        backend_dashboard_event_t event = fixture_event(value);
        TEST_ASSERT_TRUE(backend_event_ring_append(&ring, &event));
    }

    backend_event_ring_snapshot_t snapshot = {0};
    TEST_ASSERT_TRUE(backend_event_ring_snapshot(
        &ring, UINT64_C(1), 2U, output, 2U, &snapshot));
    TEST_ASSERT_TRUE(snapshot.cursor_reset);
    TEST_ASSERT_EQUAL_size_t(2U, snapshot.count);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(3), output[0].sequence);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(4), output[1].sequence);

    TEST_ASSERT_TRUE(backend_event_ring_snapshot(
        &ring, UINT64_C(1), 0U, NULL, 0U, &snapshot));
    TEST_ASSERT_TRUE(snapshot.cursor_reset);
    TEST_ASSERT_EQUAL_size_t(0U, snapshot.count);

    TEST_ASSERT_TRUE(backend_event_ring_snapshot(
        &ring, UINT64_C(999), 10U, output, 10U, &snapshot));
    TEST_ASSERT_FALSE(snapshot.cursor_reset);
    TEST_ASSERT_EQUAL_size_t(0U, snapshot.count);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(3), snapshot.oldest_sequence);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(130), snapshot.newest_sequence);
}

void test_ring_rejects_unavailable_or_malformed_storage(void)
{
    backend_event_ring_t ring = {0};
    backend_dashboard_event_t event = fixture_event(1U);
    backend_event_ring_snapshot_t snapshot;
    memset(&snapshot, 0xA5, sizeof(snapshot));

    TEST_ASSERT_FALSE(backend_event_ring_append(&ring, &event));
    TEST_ASSERT_FALSE(backend_event_ring_snapshot(
        &ring, 0U, 1U, output, 1U, &snapshot));
    TEST_ASSERT_EQUAL_size_t(0U, snapshot.count);
    TEST_ASSERT_EQUAL_UINT64(0U, snapshot.oldest_sequence);
    TEST_ASSERT_EQUAL_UINT64(0U, snapshot.newest_sequence);
    TEST_ASSERT_FALSE(snapshot.cursor_reset);
}

void test_ring_clears_before_uint64_wrap_and_restarts_at_one(void)
{
    backend_event_ring_t ring;
    TEST_ASSERT_TRUE(backend_event_ring_init(&ring, storage, 128U));
    backend_dashboard_event_t old_event = fixture_event(1U);
    TEST_ASSERT_TRUE(backend_event_ring_append(&ring, &old_event));
    ring.next_sequence = UINT64_MAX;

    backend_dashboard_event_t restarted = fixture_event(2U);
    TEST_ASSERT_TRUE(backend_event_ring_append(&ring, &restarted));
    TEST_ASSERT_EQUAL_size_t(1U, ring.count);
    TEST_ASSERT_EQUAL_size_t(0U, ring.start);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(1), storage[0].sequence);
    TEST_ASSERT_EQUAL_STRING("event-2", storage[0].id);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(2), ring.next_sequence);
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_ring_requires_exact_caller_owned_capacity_and_starts_at_one);
    BACKEND_RUN_TEST(test_ring_overwrites_oldest_at_128_records);
    BACKEND_RUN_TEST(test_ring_clips_to_limit_and_output_capacity);
    BACKEND_RUN_TEST(
        test_ring_stale_cursor_resets_but_ahead_cursor_is_empty);
    BACKEND_RUN_TEST(test_ring_rejects_unavailable_or_malformed_storage);
    BACKEND_RUN_TEST(
        test_ring_clears_before_uint64_wrap_and_restarts_at_one);
    return UNITY_END();
}
