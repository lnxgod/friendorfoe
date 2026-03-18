/**
 * Friend or Foe — Unit Tests for Bayesian Sensor Fusion Engine
 *
 * Tests the log-odds based Bayesian evidence combination including
 * initial probability, single/multi source updates, time decay, and
 * pruning of stale candidates.
 *
 * Build: PlatformIO native test environment (env:test)
 */

#include "unity.h"
#include "bayesian_fusion.h"
#include "constants.h"
#include "detection_types.h"

#include <math.h>

/* ── Test: Fresh candidate returns PRIOR probability ───────────────────── */

void test_initial_probability(void)
{
    bayesian_fusion_init();

    float prob = bayesian_fusion_get_probability("unknown_drone", 1000);

    TEST_ASSERT_FLOAT_WITHIN(0.001f, (float)PRIOR_PROBABILITY, prob);
}

/* ── Test: Single BLE RID detection at high confidence ─────────────────── */

void test_single_ble_update(void)
{
    bayesian_fusion_init();

    /*
     * BLE Remote ID has LR_BLE_RID = 50.0 (very strong evidence).
     * With sensor_confidence = 0.9:
     *   scaled_lr = 1 + (50 - 1) * 0.9 = 45.1
     *   log_lr = ln(45.1) = ~3.81
     *   prior_lo = ln(0.1/0.9) = ~-2.20
     *   new_lo = -2.20 + 3.81 = ~1.61
     *   new_prob = 1/(1+exp(-1.61)) = ~0.83
     */
    float prob = bayesian_fusion_update("ble_drone_1",
                                         DETECTION_SRC_BLE_RID,
                                         0.9f,
                                         1000);

    TEST_ASSERT_TRUE(prob > 0.8f);
    TEST_ASSERT_TRUE(prob < 1.0f);
}

/* ── Test: WiFi SSID with low confidence yields moderate probability ──── */

void test_wifi_ssid_low_confidence(void)
{
    bayesian_fusion_init();

    /*
     * WiFi SSID has LR_WIFI_SSID = 3.0 (weak evidence).
     * With sensor_confidence = 0.3:
     *   scaled_lr = 1 + (3 - 1) * 0.3 = 1.6
     *   log_lr = ln(1.6) = ~0.47
     *   prior_lo = ln(0.1/0.9) = ~-2.20
     *   new_lo = -2.20 + 0.47 = ~-1.73
     *   new_prob = 1/(1+exp(1.73)) = ~0.15
     */
    float prob = bayesian_fusion_update("wifi_drone_1",
                                         DETECTION_SRC_WIFI_SSID,
                                         0.3f,
                                         1000);

    /* Should be above prior (0.1) but not dramatically high */
    TEST_ASSERT_TRUE(prob > (float)PRIOR_PROBABILITY);
    TEST_ASSERT_TRUE(prob < 0.5f);
}

/* ── Test: Multi-source boost: WiFi + BLE exceeds either alone ─────────── */

void test_multi_source_boost(void)
{
    bayesian_fusion_init();

    /* First: WiFi SSID at 0.3 confidence */
    float prob_wifi = bayesian_fusion_update("multi_drone_1",
                                              DETECTION_SRC_WIFI_SSID,
                                              0.3f,
                                              1000);

    /* Then: BLE RID at 0.9 confidence on the same candidate */
    float prob_multi = bayesian_fusion_update("multi_drone_1",
                                               DETECTION_SRC_BLE_RID,
                                               0.9f,
                                               1000);

    /* Also get BLE-only for comparison */
    float prob_ble_only = bayesian_fusion_update("ble_only_drone",
                                                  DETECTION_SRC_BLE_RID,
                                                  0.9f,
                                                  1000);

    /* Multi-source should exceed either individual source */
    TEST_ASSERT_TRUE(prob_multi > prob_wifi);
    TEST_ASSERT_TRUE(prob_multi > prob_ble_only);

    /* Multi-source with good evidence should be very high */
    TEST_ASSERT_TRUE(prob_multi > 0.9f);
}

/* ── Test: Time decay pulls probability toward prior ───────────────────── */

void test_time_decay(void)
{
    bayesian_fusion_init();

    /* Strong initial evidence at t=0ms */
    float prob_initial = bayesian_fusion_update("decay_drone_1",
                                                 DETECTION_SRC_BLE_RID,
                                                 0.9f,
                                                 0);

    /* Get probability 60 seconds later (2 half-lives) with no new evidence */
    float prob_decayed = bayesian_fusion_get_probability("decay_drone_1",
                                                          60000);

    /* After 60s (2 half-lives at 30s), probability should have decayed
     * significantly toward the prior */
    TEST_ASSERT_TRUE(prob_decayed < prob_initial);
    TEST_ASSERT_TRUE(prob_decayed > (float)PRIOR_PROBABILITY);

    /* After a very long time (300s = 10 half-lives), should be near prior */
    float prob_very_decayed = bayesian_fusion_get_probability("decay_drone_1",
                                                               300000);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, (float)PRIOR_PROBABILITY, prob_very_decayed);
}

/* ── Test: Prune removes entries older than DETECTION_STALE_MS ─────────── */

void test_prune(void)
{
    bayesian_fusion_init();

    /* Create an entry at t=0 */
    bayesian_fusion_update("stale_drone_1",
                            DETECTION_SRC_BLE_RID,
                            0.9f,
                            0);

    /* Create a fresh entry at t=100000ms */
    bayesian_fusion_update("fresh_drone_1",
                            DETECTION_SRC_BLE_RID,
                            0.9f,
                            100000);

    /* Prune at t = DETECTION_STALE_MS + 1000 (stale_drone_1 is >120s old) */
    int64_t prune_time = DETECTION_STALE_MS + 1000;
    bayesian_fusion_prune(prune_time);

    /* stale_drone_1 should have been pruned (returns prior) */
    float prob_stale = bayesian_fusion_get_probability("stale_drone_1", prune_time);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, (float)PRIOR_PROBABILITY, prob_stale);

    /* fresh_drone_1 should still exist (not yet stale) */
    float prob_fresh = bayesian_fusion_get_probability("fresh_drone_1", prune_time);
    TEST_ASSERT_TRUE(prob_fresh > (float)PRIOR_PROBABILITY);
}

/* ── Unity runner ──────────────────────────────────────────────────────── */

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_initial_probability);
    RUN_TEST(test_single_ble_update);
    RUN_TEST(test_wifi_ssid_low_confidence);
    RUN_TEST(test_multi_source_boost);
    RUN_TEST(test_time_decay);
    RUN_TEST(test_prune);

    return UNITY_END();
}
