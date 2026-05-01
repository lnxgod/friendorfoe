/*
 * Tests for the pure decision helpers in fw_auto_check.
 *
 * The runtime side (HTTP, OTA, NVS, FreeRTOS) is gated behind
 * FW_AUTO_CHECK_HOST_TEST and not exercised here — it's covered by the
 * post-flash soak verification on real hardware.
 */

#include "unity.h"
#include "fw_auto_check.h"

#include <stdbool.h>
#include <stdint.h>

void test_decide_skips_when_wifi_down(void)
{
    TEST_ASSERT_FALSE(fw_auto_check_decide(
        /*free_heap_kb=*/200, /*wifi=*/false,
        /*relay=*/false, /*age=*/0, /*interval=*/1800));
}

void test_decide_skips_when_relay_active(void)
{
    /* Manual flash always wins — never let auto-check race a UART relay. */
    TEST_ASSERT_FALSE(fw_auto_check_decide(
        200, true, /*relay=*/true, 0, 1800));
}

void test_decide_skips_when_heap_too_low(void)
{
    /* OTA writes need ~50 KB free internal heap; below that we'll likely
     * fail in the middle. Skip and try again next cycle. */
    TEST_ASSERT_FALSE(fw_auto_check_decide(
        /*free_heap_kb=*/30, true, false, 0, 1800));
}

void test_decide_allows_first_check_when_age_zero(void)
{
    /* "Never checked" is encoded as age=0; first run should always proceed. */
    TEST_ASSERT_TRUE(fw_auto_check_decide(
        200, true, false, /*age=*/0, /*interval=*/1800));
}

void test_decide_skips_when_too_recent(void)
{
    TEST_ASSERT_FALSE(fw_auto_check_decide(
        200, true, false, /*age=*/300, /*interval=*/1800));
}

void test_decide_proceeds_at_interval_boundary(void)
{
    TEST_ASSERT_TRUE(fw_auto_check_decide(
        200, true, false, /*age=*/1800, /*interval=*/1800));
}

void test_decide_proceeds_long_after_interval(void)
{
    TEST_ASSERT_TRUE(fw_auto_check_decide(
        200, true, false, /*age=*/4000, /*interval=*/1800));
}

/* ── Version-differs ─────────────────────────────────────────────────────── */

void test_versions_match_returns_false(void)
{
    TEST_ASSERT_FALSE(fw_auto_check_version_differs(
        "0.63.0-svc155", "0.63.0-svc155"));
}

void test_versions_differ_returns_true(void)
{
    TEST_ASSERT_TRUE(fw_auto_check_version_differs(
        "0.63.0-svc155", "0.63.0-svc156"));
}

void test_remote_unknown_does_not_trigger_update(void)
{
    /* If the backend can't tell us a real version, never blindly update. */
    TEST_ASSERT_FALSE(fw_auto_check_version_differs(
        "0.63.0-svc155", "unknown"));
}

void test_remote_empty_does_not_trigger_update(void)
{
    TEST_ASSERT_FALSE(fw_auto_check_version_differs("0.63.0-svc155", ""));
    TEST_ASSERT_FALSE(fw_auto_check_version_differs("0.63.0-svc155", NULL));
}

void test_local_empty_takes_remote(void)
{
    /* Edge case: if local FOF_VERSION is somehow empty, accept whatever
     * the backend offers (nothing to compare against). */
    TEST_ASSERT_TRUE(fw_auto_check_version_differs("", "0.63.0-svc156"));
    TEST_ASSERT_TRUE(fw_auto_check_version_differs(NULL, "0.63.0-svc156"));
}

void test_versions_differ_across_naming_schemes(void)
{
    /* Project mixes svc-N internal builds with named tag-style releases.
     * Treat any string difference as "needs update" — backend's choice. */
    TEST_ASSERT_TRUE(fw_auto_check_version_differs(
        "0.63.0-svc155", "0.63.18-rf-intel"));
}
