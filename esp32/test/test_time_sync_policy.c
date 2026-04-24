#include "unity.h"

#include "time_sync_policy.h"

void test_time_message_validity_tracks_ok_flag_and_epoch(void)
{
    TEST_ASSERT_TRUE(fof_time_message_is_valid(false, false, 1700000000001LL));
    TEST_ASSERT_TRUE(fof_time_message_is_valid(true, true, 1700000000001LL));
    TEST_ASSERT_FALSE(fof_time_message_is_valid(true, false, 1700000000001LL));
    TEST_ASSERT_FALSE(fof_time_message_is_valid(false, false, 1699999999999LL));
}

void test_backend_epoch_resteers_only_when_drift_exceeds_threshold(void)
{
    TEST_ASSERT_TRUE(fof_time_should_apply_backend_epoch(
        false, true, 1700000000001LL, 1700000000401LL,
        FOF_TIME_SYNC_BACKEND_RESTEER_THRESHOLD_MS
    ));
    TEST_ASSERT_FALSE(fof_time_should_apply_backend_epoch(
        false, true, 1700000000001LL, 1700000000101LL,
        FOF_TIME_SYNC_BACKEND_RESTEER_THRESHOLD_MS
    ));
}

void test_sntp_synced_uplink_ignores_backend_overwrite(void)
{
    TEST_ASSERT_FALSE(fof_time_should_apply_backend_epoch(
        true, true, 1700000000001LL, 1700000001001LL,
        FOF_TIME_SYNC_BACKEND_RESTEER_THRESHOLD_MS
    ));
}

void test_stale_timeout_marks_scanner_state_stale(void)
{
    TEST_ASSERT_EQUAL_STRING(
        "waiting",
        fof_time_sync_state_label(0, 0, 0, 1000, FOF_TIME_SYNC_STALE_AFTER_MS)
    );
    TEST_ASSERT_EQUAL_STRING(
        "fresh",
        fof_time_sync_state_label(1, 1234, 1000, 20000, FOF_TIME_SYNC_STALE_AFTER_MS)
    );
    TEST_ASSERT_TRUE(fof_time_offset_is_stale(1000, 32001, FOF_TIME_SYNC_STALE_AFTER_MS));
    TEST_ASSERT_EQUAL_STRING(
        "stale",
        fof_time_sync_state_label(1, 1234, 1000, 32001, FOF_TIME_SYNC_STALE_AFTER_MS)
    );
}
