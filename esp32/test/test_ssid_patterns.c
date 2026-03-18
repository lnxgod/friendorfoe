/**
 * Friend or Foe — Unit Tests for WiFi SSID Pattern Matching
 *
 * Tests case-insensitive prefix matching of known drone SSID patterns.
 * The pattern table contains ~99 entries covering DJI, Parrot, Autel,
 * HOVERAir, and many other manufacturers plus generic drone SSIDs.
 *
 * Build: PlatformIO native test environment (env:test)
 */

#include "unity.h"
#include "wifi_ssid_patterns.h"

#include <string.h>

/* ── Test: DJI prefix match ────────────────────────────────────────────── */

void test_dji_match(void)
{
    const drone_ssid_pattern_t *result = wifi_ssid_match("DJI-MAVIC3-ABC123");

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_STRING("DJI", result->manufacturer);
}

/* ── Test: TELLO prefix match ──────────────────────────────────────────── */

void test_tello_match(void)
{
    const drone_ssid_pattern_t *result = wifi_ssid_match("TELLO-ABCDEF");

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_STRING("Ryze/DJI", result->manufacturer);
}

/* ── Test: Case-insensitive matching ───────────────────────────────────── */

void test_case_insensitive(void)
{
    /* "dji-mini3" should match "DJI-" prefix case-insensitively */
    const drone_ssid_pattern_t *result = wifi_ssid_match("dji-mini3");

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_STRING("DJI", result->manufacturer);
}

/* ── Test: Non-drone SSID returns NULL ─────────────────────────────────── */

void test_no_match(void)
{
    const drone_ssid_pattern_t *result = wifi_ssid_match("MyHomeWiFi");

    TEST_ASSERT_NULL(result);
}

/* ── Test: HOVERAir match ──────────────────────────────────────────────── */

void test_hover_air(void)
{
    const drone_ssid_pattern_t *result = wifi_ssid_match("HOVERAir X1 Pro");

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_STRING("HOVERAir", result->manufacturer);
}

/* ── Test: Generic DRONE- prefix ───────────────────────────────────────── */

void test_generic_drone(void)
{
    const drone_ssid_pattern_t *result = wifi_ssid_match("DRONE-12345");

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_STRING("Unknown", result->manufacturer);
}

/* ── Test: All patterns have valid prefix and manufacturer ─────────────── */

void test_all_patterns_valid(void)
{
    int count = 0;
    const drone_ssid_pattern_t *patterns = wifi_ssid_get_patterns(&count);

    TEST_ASSERT_NOT_NULL(patterns);
    TEST_ASSERT_TRUE(count > 0);

    for (int i = 0; i < count; i++) {
        TEST_ASSERT_NOT_NULL_MESSAGE(patterns[i].prefix,
            "Pattern prefix should not be NULL");
        TEST_ASSERT_NOT_NULL_MESSAGE(patterns[i].manufacturer,
            "Pattern manufacturer should not be NULL");

        /* Prefix should be non-empty */
        TEST_ASSERT_TRUE_MESSAGE(strlen(patterns[i].prefix) > 0,
            "Pattern prefix should not be empty");

        /* Manufacturer should be non-empty */
        TEST_ASSERT_TRUE_MESSAGE(strlen(patterns[i].manufacturer) > 0,
            "Pattern manufacturer should not be empty");
    }
}

/* ── Test: NULL and empty SSID return NULL ──────────────────────────────── */

void test_null_ssid(void)
{
    TEST_ASSERT_NULL(wifi_ssid_match(NULL));
    TEST_ASSERT_NULL(wifi_ssid_match(""));
}

/* ── Unity runner ──────────────────────────────────────────────────────── */

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_dji_match);
    RUN_TEST(test_tello_match);
    RUN_TEST(test_case_insensitive);
    RUN_TEST(test_no_match);
    RUN_TEST(test_hover_air);
    RUN_TEST(test_generic_drone);
    RUN_TEST(test_all_patterns_valid);
    RUN_TEST(test_null_ssid);

    return UNITY_END();
}
