#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "backend_detection_router.h"
#include "../support/backend_test_main.h"

void setUp(void) {}
void tearDown(void) {}

static backend_detection_observation_t fixture_observation(
    const char *identity,
    uint8_t slot,
    int8_t rssi,
    int64_t epoch_ms)
{
    backend_detection_observation_t item = {0};
    snprintf(item.detection.drone_id,
             sizeof(item.detection.drone_id),
             "%s",
             identity);
    item.detection.source = DETECTION_SRC_BLE_RID;
    item.detection.confidence = 0.50f;
    item.detection.rssi = rssi;
    item.detection.scanner_slot = slot;
    item.detection.scanner_slots_seen = (uint8_t)(UINT8_C(1) << slot);
    item.timestamp_valid = epoch_ms >= BACKEND_DETECTION_EPOCH_MIN_MS;
    item.timestamp_epoch_ms = item.timestamp_valid ? epoch_ms : 0;
    return item;
}

static size_t used_bucket_count(const backend_detection_router_t *router)
{
    size_t count = 0U;
    for (size_t index = 0U;
         index < BACKEND_DEDUPE_BUCKET_CAPACITY;
         ++index) {
        if (router->buckets[index].used) {
            ++count;
        }
    }
    return count;
}

void test_every_valid_detection_reaches_upload_regardless_of_led_policy(void)
{
    backend_detection_router_t router;
    backend_detection_router_init(&router);
    backend_detection_observation_t low_rank =
        fixture_observation("low-rank-privacy", 0U, -84, 1785600000100LL);
    low_rank.detection.source = DETECTION_SRC_BLE_FINGERPRINT;
    low_rank.detection.confidence = 0.01f;

    backend_detection_route_result_t result =
        backend_detection_router_ingest(&router, &low_rank, 1000);

    TEST_ASSERT_TRUE(result.accepted_for_upload);
    TEST_ASSERT_TRUE(result.update_local_threat);
    TEST_ASSERT_FALSE(result.backpressure);
    TEST_ASSERT_EQUAL_UINT32(1U, used_bucket_count(&router));
}

void test_cross_slot_copy_merges_mask_strongest_record_and_earliest_time(void)
{
    backend_detection_router_t router;
    backend_detection_router_init(&router);
    backend_detection_observation_t first =
        fixture_observation("drone-A", 0U, -60, 1785600000100LL);
    snprintf(first.detection.probed_ssids,
             sizeof(first.detection.probed_ssids),
             "alpha,beta");
    first.detection.confidence = 0.55f;
    backend_detection_observation_t second =
        fixture_observation("drone-A", 1U, -40, 1785600000200LL);
    snprintf(second.detection.probed_ssids,
             sizeof(second.detection.probed_ssids),
             "beta,gamma");
    second.detection.confidence = 0.91f;

    TEST_ASSERT_TRUE(backend_detection_router_ingest(
        &router, &first, 1000).accepted_for_upload);
    TEST_ASSERT_TRUE(backend_detection_router_ingest(
        &router, &second, 1100).accepted_for_upload);
    TEST_ASSERT_EQUAL_UINT32(1U, backend_detection_router_tick(&router, 1500));

    backend_detection_observation_t upload = {0};
    TEST_ASSERT_TRUE(backend_detection_router_next_upload(&router, &upload));
    TEST_ASSERT_EQUAL_HEX8(UINT8_C(0x03),
                           upload.detection.scanner_slots_seen);
    TEST_ASSERT_EQUAL_INT8(-40, upload.detection.rssi);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.91f, upload.detection.confidence);
    TEST_ASSERT_EQUAL_INT64(1785600000100LL, upload.timestamp_epoch_ms);
    TEST_ASSERT_EQUAL_STRING("alpha,beta,gamma",
                             upload.detection.probed_ssids);
    TEST_ASSERT_FALSE(backend_detection_router_next_upload(&router, &upload));
}

void test_existing_full_probe_list_is_never_truncated_by_union(void)
{
    backend_detection_router_t router;
    backend_detection_router_init(&router);
    backend_detection_observation_t first =
        fixture_observation("full-probes", 0U, -60, 1785600000100LL);
    backend_detection_observation_t second =
        fixture_observation("full-probes", 1U, -40, 1785600000200LL);
    memset(first.detection.probed_ssids, 'a',
           sizeof(first.detection.probed_ssids) - 1U);
    first.detection.probed_ssids[
        sizeof(first.detection.probed_ssids) - 1U] = '\0';
    snprintf(second.detection.probed_ssids,
             sizeof(second.detection.probed_ssids),
             "new-network");

    backend_detection_router_ingest(&router, &first, 1000);
    backend_detection_router_ingest(&router, &second, 1100);
    backend_detection_router_tick(&router, 1500);
    backend_detection_observation_t upload = {0};
    TEST_ASSERT_TRUE(backend_detection_router_next_upload(&router, &upload));
    TEST_ASSERT_EQUAL_STRING(first.detection.probed_ssids,
                             upload.detection.probed_ssids);
}

void test_scanner_observation_time_wins_and_invalid_scanner_uses_uplink(void)
{
    drone_detection_t detection = {0};
    backend_scanner_stamp_t scanner = {
        .time_valid = true,
        .observed_epoch_ms = 1785600000123LL,
    };
    backend_detection_observation_t observed = {0};

    backend_observation_resolve(
        &detection, &scanner, 1785600000999LL, &observed);
    TEST_ASSERT_TRUE(observed.timestamp_valid);
    TEST_ASSERT_EQUAL_INT64(1785600000123LL, observed.timestamp_epoch_ms);

    scanner.time_valid = false;
    scanner.observed_epoch_ms = 9000;
    backend_observation_resolve(
        &detection, &scanner, 1785600000999LL, &observed);
    TEST_ASSERT_TRUE(observed.timestamp_valid);
    TEST_ASSERT_EQUAL_INT64(1785600000999LL, observed.timestamp_epoch_ms);

    backend_observation_resolve(&detection, &scanner, 12000, &observed);
    TEST_ASSERT_FALSE(observed.timestamp_valid);
    TEST_ASSERT_EQUAL_INT64(0, observed.timestamp_epoch_ms);
}

void test_last_bucket_flushes_once_at_exact_500_ms_boundary(void)
{
    backend_detection_router_t router;
    backend_detection_router_init(&router);
    backend_detection_observation_t item =
        fixture_observation("boundary", 0U, -60, 1785600000100LL);
    TEST_ASSERT_TRUE(backend_detection_router_ingest(
        &router, &item, 1000).accepted_for_upload);
    TEST_ASSERT_EQUAL_UINT32(0U, backend_detection_router_tick(&router, 1499));
    TEST_ASSERT_EQUAL_UINT32(1U, backend_detection_router_tick(&router, 1500));
    TEST_ASSERT_EQUAL_UINT32(0U, backend_detection_router_tick(&router, 1501));

    backend_detection_observation_t upload = {0};
    TEST_ASSERT_TRUE(backend_detection_router_next_upload(&router, &upload));
    TEST_ASSERT_FALSE(backend_detection_router_next_upload(&router, &upload));
}

void test_same_key_ingest_at_exact_boundary_closes_old_bucket_first(void)
{
    backend_detection_router_t router;
    backend_detection_router_init(&router);
    backend_detection_observation_t first =
        fixture_observation("boundary-key", 0U, -60, 1785600000100LL);
    backend_detection_observation_t next =
        fixture_observation("boundary-key", 1U, -40, 1785600000200LL);

    TEST_ASSERT_TRUE(backend_detection_router_ingest(
        &router, &first, 1000).accepted_for_upload);
    backend_detection_route_result_t accepted =
        backend_detection_router_ingest(&router, &next, 1500);
    TEST_ASSERT_TRUE(accepted.accepted_for_upload);
    TEST_ASSERT_FALSE(accepted.backpressure);

    backend_detection_observation_t upload = {0};
    TEST_ASSERT_TRUE(backend_detection_router_next_upload(&router, &upload));
    TEST_ASSERT_EQUAL_INT8(-60, upload.detection.rssi);
    TEST_ASSERT_EQUAL_HEX8(UINT8_C(0x01),
                           upload.detection.scanner_slots_seen);
    TEST_ASSERT_FALSE(backend_detection_router_next_upload(&router, &upload));

    TEST_ASSERT_EQUAL_UINT32(0U, backend_detection_router_tick(&router, 1999));
    TEST_ASSERT_EQUAL_UINT32(1U, backend_detection_router_tick(&router, 2000));
    TEST_ASSERT_TRUE(backend_detection_router_next_upload(&router, &upload));
    TEST_ASSERT_EQUAL_INT8(-40, upload.detection.rssi);
    TEST_ASSERT_EQUAL_HEX8(UINT8_C(0x02),
                           upload.detection.scanner_slots_seen);
    TEST_ASSERT_FALSE(backend_detection_router_next_upload(&router, &upload));
}

void test_expired_same_key_backpressures_without_mutating_either_observation(void)
{
    backend_detection_router_t router;
    backend_detection_router_init(&router);
    char identity[32];

    for (uint32_t index = 0U;
         index < BACKEND_DEDUPE_READY_CAPACITY;
         ++index) {
        snprintf(identity, sizeof(identity), "ready-%u", (unsigned)index);
        backend_detection_observation_t item =
            fixture_observation(identity, 0U, -70, 1785600000100LL);
        TEST_ASSERT_TRUE(backend_detection_router_ingest(
            &router, &item, (int64_t)index).accepted_for_upload);
    }
    TEST_ASSERT_EQUAL_UINT32(BACKEND_DEDUPE_READY_CAPACITY,
        backend_detection_router_tick(&router, 1000));

    backend_detection_observation_t first =
        fixture_observation("blocked-key", 0U, -60, 1785600000200LL);
    backend_detection_observation_t next =
        fixture_observation("blocked-key", 1U, -40, 1785600000300LL);
    const backend_detection_observation_t unchanged = next;
    TEST_ASSERT_TRUE(backend_detection_router_ingest(
        &router, &first, 2000).accepted_for_upload);

    backend_detection_route_result_t blocked =
        backend_detection_router_ingest(&router, &next, 2500);
    TEST_ASSERT_TRUE(blocked.backpressure);
    TEST_ASSERT_FALSE(blocked.accepted_for_upload);
    TEST_ASSERT_FALSE(blocked.update_local_threat);
    TEST_ASSERT_EQUAL_MEMORY(&unchanged, &next, sizeof(next));

    backend_detection_observation_t upload = {0};
    TEST_ASSERT_TRUE(backend_detection_router_next_upload(&router, &upload));
    backend_detection_route_result_t retried =
        backend_detection_router_ingest(&router, &next, 2501);
    TEST_ASSERT_TRUE(retried.accepted_for_upload);
    TEST_ASSERT_FALSE(retried.backpressure);

    while (backend_detection_router_next_upload(&router, &upload)) {
    }
    TEST_ASSERT_EQUAL_UINT32(1U, backend_detection_router_tick(&router, 3001));
    TEST_ASSERT_TRUE(backend_detection_router_next_upload(&router, &upload));
    TEST_ASSERT_EQUAL_INT8(-40, upload.detection.rssi);
    TEST_ASSERT_EQUAL_HEX8(UINT8_C(0x02),
                           upload.detection.scanner_slots_seen);
}

void test_interleaved_identities_flush_in_first_insertion_order(void)
{
    backend_detection_router_t router;
    backend_detection_router_init(&router);
    backend_detection_observation_t a0 =
        fixture_observation("A", 0U, -60, 1785600000100LL);
    backend_detection_observation_t b0 =
        fixture_observation("B", 0U, -55, 1785600000200LL);
    backend_detection_observation_t a1 =
        fixture_observation("A", 1U, -40, 1785600000300LL);
    backend_detection_router_ingest(&router, &a0, 1000);
    backend_detection_router_ingest(&router, &b0, 1100);
    backend_detection_router_ingest(&router, &a1, 1200);

    TEST_ASSERT_EQUAL_UINT32(2U, backend_detection_router_tick(&router, 1600));
    backend_detection_observation_t upload = {0};
    TEST_ASSERT_TRUE(backend_detection_router_next_upload(&router, &upload));
    TEST_ASSERT_EQUAL_STRING("A", upload.detection.drone_id);
    TEST_ASSERT_EQUAL_HEX8(UINT8_C(0x03),
                           upload.detection.scanner_slots_seen);
    TEST_ASSERT_TRUE(backend_detection_router_next_upload(&router, &upload));
    TEST_ASSERT_EQUAL_STRING("B", upload.detection.drone_id);
    TEST_ASSERT_FALSE(backend_detection_router_next_upload(&router, &upload));
}

void test_sixty_fifth_identity_backpressures_only_when_ready_queue_is_full(void)
{
    backend_detection_router_t router;
    backend_detection_router_init(&router);
    char identity[32];

    for (uint32_t index = 0U;
         index < BACKEND_DEDUPE_BUCKET_CAPACITY;
         ++index) {
        snprintf(identity, sizeof(identity), "ready-%u", (unsigned)index);
        backend_detection_observation_t item =
            fixture_observation(identity, 0U, -60, 1785600000100LL);
        TEST_ASSERT_TRUE(backend_detection_router_ingest(
            &router, &item, (int64_t)index).accepted_for_upload);
    }
    TEST_ASSERT_EQUAL_UINT32(BACKEND_DEDUPE_READY_CAPACITY,
        backend_detection_router_tick(&router, 1000));

    for (uint32_t index = 0U;
         index < BACKEND_DEDUPE_BUCKET_CAPACITY;
         ++index) {
        snprintf(identity, sizeof(identity), "pending-%u", (unsigned)index);
        backend_detection_observation_t item =
            fixture_observation(identity, 0U, -60, 1785600000100LL);
        TEST_ASSERT_TRUE(backend_detection_router_ingest(
            &router, &item, 2000 + (int64_t)index).accepted_for_upload);
    }

    backend_detection_observation_t extra =
        fixture_observation("retry-me", 0U, -50, 1785600000100LL);
    backend_detection_route_result_t blocked =
        backend_detection_router_ingest(&router, &extra, 3000);
    TEST_ASSERT_TRUE(blocked.backpressure);
    TEST_ASSERT_FALSE(blocked.accepted_for_upload);
    TEST_ASSERT_FALSE(blocked.update_local_threat);
    TEST_ASSERT_EQUAL_UINT32(BACKEND_DEDUPE_BUCKET_CAPACITY,
                             used_bucket_count(&router));

    backend_detection_observation_t drained = {0};
    TEST_ASSERT_TRUE(backend_detection_router_next_upload(&router, &drained));
    backend_detection_route_result_t retried =
        backend_detection_router_ingest(&router, &extra, 3001);
    TEST_ASSERT_FALSE(retried.backpressure);
    TEST_ASSERT_TRUE(retried.accepted_for_upload);
    TEST_ASSERT_TRUE(retried.update_local_threat);
    TEST_ASSERT_EQUAL_UINT32(BACKEND_DEDUPE_BUCKET_CAPACITY,
                             used_bucket_count(&router));

    char last_ready_identity[64] = {0};
    while (backend_detection_router_next_upload(&router, &drained)) {
        snprintf(last_ready_identity,
                 sizeof(last_ready_identity),
                 "%s",
                 drained.detection.drone_id);
    }
    TEST_ASSERT_EQUAL_STRING("pending-0", last_ready_identity);
    TEST_ASSERT_EQUAL_UINT32(63U, backend_detection_router_tick(&router, 3001));
    TEST_ASSERT_TRUE(backend_detection_router_next_upload(&router, &drained));
    TEST_ASSERT_EQUAL_STRING("pending-1", drained.detection.drone_id);
}

void test_invalid_identity_and_invalid_arguments_are_not_consumed(void)
{
    backend_detection_router_t router;
    backend_detection_router_init(&router);
    backend_detection_observation_t invalid = {0};
    invalid.detection.source = DETECTION_SRC_BLE_RID;

    backend_detection_route_result_t result =
        backend_detection_router_ingest(&router, &invalid, 0);
    TEST_ASSERT_FALSE(result.accepted_for_upload);
    TEST_ASSERT_FALSE(result.update_local_threat);
    TEST_ASSERT_FALSE(result.backpressure);
    TEST_ASSERT_EQUAL_UINT32(0U, used_bucket_count(&router));
    TEST_ASSERT_EQUAL_UINT32(0U, backend_detection_router_tick(NULL, 1000));
    TEST_ASSERT_FALSE(backend_detection_router_next_upload(NULL, &invalid));
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_every_valid_detection_reaches_upload_regardless_of_led_policy);
    BACKEND_RUN_TEST(
        test_cross_slot_copy_merges_mask_strongest_record_and_earliest_time);
    BACKEND_RUN_TEST(
        test_existing_full_probe_list_is_never_truncated_by_union);
    BACKEND_RUN_TEST(
        test_scanner_observation_time_wins_and_invalid_scanner_uses_uplink);
    BACKEND_RUN_TEST(
        test_last_bucket_flushes_once_at_exact_500_ms_boundary);
    BACKEND_RUN_TEST(
        test_same_key_ingest_at_exact_boundary_closes_old_bucket_first);
    BACKEND_RUN_TEST(
        test_expired_same_key_backpressures_without_mutating_either_observation);
    BACKEND_RUN_TEST(
        test_interleaved_identities_flush_in_first_insertion_order);
    BACKEND_RUN_TEST(
        test_sixty_fifth_identity_backpressures_only_when_ready_queue_is_full);
    BACKEND_RUN_TEST(
        test_invalid_identity_and_invalid_arguments_are_not_consumed);
    return UNITY_END();
}
