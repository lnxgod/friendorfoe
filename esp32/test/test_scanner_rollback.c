/*
 * Tests for the pure scanner rollback policy decision function.
 *
 * Exercises every (reset_reason × pending_verify × prior_count) combination
 * we care about. The runtime side (NVS persistence, esp_ota_*) is intentionally
 * not exercised here — that's covered by post-flash heartbeat verification.
 */

#include "unity.h"
#include "scanner_rollback.h"

#include <stdint.h>

/* Mirrors of esp_reset_reason_t values used by the pure policy. */
#define RST_POWERON   1
#define RST_EXT       2
#define RST_PANIC     3
#define RST_INT_WDT   4
#define RST_TASK_WDT  5
#define RST_WDT       6
#define RST_DEEPSLEEP 7
#define RST_BROWNOUT  8
#define RST_SDIO      9

/* Default crash-loop threshold matches CRASH_LOOP_THRESHOLD in the .c. */
#define THRESHOLD 3u

void test_clean_boot_keeps_counter_carried_no_action(void)
{
    rollback_decision_t d = scanner_rollback_decide(
        RST_POWERON, /* pending_verify=*/false, /* prior=*/2, THRESHOLD);
    TEST_ASSERT_EQUAL(ROLLBACK_ACTION_NONE, d.action);
    TEST_ASSERT_FALSE(d.reset_was_crash);
    TEST_ASSERT_EQUAL_UINT32(2, d.new_crash_count); /* unchanged on clean boot */
}

void test_brownout_does_not_count_as_crash(void)
{
    /* Brownouts are power problems, not firmware bugs. Don't bump counter. */
    rollback_decision_t d = scanner_rollback_decide(
        RST_BROWNOUT, /* pending_verify=*/false, /* prior=*/0, THRESHOLD);
    TEST_ASSERT_EQUAL(ROLLBACK_ACTION_NONE, d.action);
    TEST_ASSERT_FALSE(d.reset_was_crash);
    TEST_ASSERT_EQUAL_UINT32(0, d.new_crash_count);
}

void test_panic_increments_counter(void)
{
    rollback_decision_t d = scanner_rollback_decide(
        RST_PANIC, /* pending_verify=*/false, /* prior=*/0, THRESHOLD);
    TEST_ASSERT_TRUE(d.reset_was_crash);
    TEST_ASSERT_EQUAL_UINT32(1, d.new_crash_count);
    /* Below threshold + validated image → no action yet. */
    TEST_ASSERT_EQUAL(ROLLBACK_ACTION_NONE, d.action);
}

void test_panic_on_pending_verify_forces_rollback_immediately(void)
{
    /* Even one panic on a fresh OTA is enough to trigger rollback —
     * the previous slot is known good and we have nothing to gain by
     * letting the bad image keep crashing. */
    rollback_decision_t d = scanner_rollback_decide(
        RST_PANIC, /* pending_verify=*/true, /* prior=*/0, THRESHOLD);
    TEST_ASSERT_EQUAL(ROLLBACK_ACTION_FORCE_ROLLBACK, d.action);
    TEST_ASSERT_TRUE(d.reset_was_crash);
    TEST_ASSERT_EQUAL_UINT32(1, d.new_crash_count);
}

void test_task_wdt_on_pending_verify_forces_rollback(void)
{
    rollback_decision_t d = scanner_rollback_decide(
        RST_TASK_WDT, /* pending_verify=*/true, /* prior=*/0, THRESHOLD);
    TEST_ASSERT_EQUAL(ROLLBACK_ACTION_FORCE_ROLLBACK, d.action);
}

void test_int_wdt_on_pending_verify_forces_rollback(void)
{
    rollback_decision_t d = scanner_rollback_decide(
        RST_INT_WDT, /* pending_verify=*/true, /* prior=*/0, THRESHOLD);
    TEST_ASSERT_EQUAL(ROLLBACK_ACTION_FORCE_ROLLBACK, d.action);
}

void test_three_panics_on_validated_marks_crash_loop(void)
{
    /* Already at threshold-1, this panic pushes to threshold → crash loop. */
    rollback_decision_t d = scanner_rollback_decide(
        RST_PANIC, /* pending_verify=*/false, /* prior=*/2, THRESHOLD);
    TEST_ASSERT_EQUAL(ROLLBACK_ACTION_MARK_CRASH_LOOP, d.action);
    TEST_ASSERT_EQUAL_UINT32(3, d.new_crash_count);
}

void test_well_above_threshold_still_marks_crash_loop_not_rollback(void)
{
    /* Validated image, no rollback target. Even at 99 prior crashes, action
     * stays MARK_CRASH_LOOP — the right move is to ask the uplink for a
     * fresh firmware via fw_check, not to roll back to nothing. */
    rollback_decision_t d = scanner_rollback_decide(
        RST_PANIC, /* pending_verify=*/false, /* prior=*/99, THRESHOLD);
    TEST_ASSERT_EQUAL(ROLLBACK_ACTION_MARK_CRASH_LOOP, d.action);
    TEST_ASSERT_EQUAL_UINT32(100, d.new_crash_count);
}

void test_pending_verify_takes_priority_over_threshold(void)
{
    /* If we're PENDING_VERIFY *and* over the threshold, we should still roll
     * back rather than ask for fresh firmware — rollback is the safer move
     * because the previous slot is known good. */
    rollback_decision_t d = scanner_rollback_decide(
        RST_PANIC, /* pending_verify=*/true, /* prior=*/5, THRESHOLD);
    TEST_ASSERT_EQUAL(ROLLBACK_ACTION_FORCE_ROLLBACK, d.action);
}

void test_clean_boot_does_not_lower_existing_count(void)
{
    /* Counter only resets on mark_valid (not modeled in pure policy).
     * Clean boots carry the count forward — important so a transient
     * stable boot between two crashes doesn't mask a real crash loop. */
    rollback_decision_t d = scanner_rollback_decide(
        RST_POWERON, /* pending_verify=*/false, /* prior=*/4, THRESHOLD);
    TEST_ASSERT_EQUAL(ROLLBACK_ACTION_NONE, d.action);
    TEST_ASSERT_EQUAL_UINT32(4, d.new_crash_count);
}
