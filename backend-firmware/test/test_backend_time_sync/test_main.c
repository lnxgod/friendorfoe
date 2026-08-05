#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "backend_time_sync.h"
#include "../support/backend_test_main.h"

void setUp(void) {}
void tearDown(void) {}

void test_backend_time_requires_exact_single_ms_object(void)
{
    int64_t epoch_ms = 0;
    static const char exact[] = "{\"ms\":1785600000123}";
    static const char whitespace[] =
        " \n{ \"ms\" : 1785600000123 }\t";

    TEST_ASSERT_TRUE(backend_time_parse_response(
        exact, sizeof(exact) - 1U, &epoch_ms));
    TEST_ASSERT_EQUAL_INT64(INT64_C(1785600000123), epoch_ms);
    epoch_ms = 0;
    TEST_ASSERT_TRUE(backend_time_parse_response(
        whitespace, sizeof(whitespace) - 1U, &epoch_ms));
    TEST_ASSERT_EQUAL_INT64(INT64_C(1785600000123), epoch_ms);
}

void test_backend_time_rejects_extra_duplicate_and_non_object_members(void)
{
    static const char *const invalid[] = {
        "{\"ms\":1785600000123,\"x\":1}",
        "{\"x\":1,\"ms\":1785600000123}",
        "{\"ms\":1785600000123,\"ms\":1785600000123}",
        "1785600000123",
        "[1785600000123]",
        "{}",
        "{\"MS\":1785600000123}",
    };
    for (size_t index = 0U;
         index < sizeof(invalid) / sizeof(invalid[0]);
         ++index) {
        int64_t epoch_ms = INT64_C(1234);
        TEST_ASSERT_FALSE(backend_time_parse_response(
            invalid[index], strlen(invalid[index]), &epoch_ms));
        TEST_ASSERT_EQUAL_INT64(INT64_C(1234), epoch_ms);
    }
}

void test_backend_time_rejects_noninteger_range_and_pre_epoch_values(void)
{
    static const char *const invalid[] = {
        "{\"ms\":1785600000123.0}",
        "{\"ms\":1.785600000123e12}",
        "{\"ms\":\"1785600000123\"}",
        "{\"ms\":9223372036854775808}",
        "{\"ms\":1700000000000}",
        "{\"ms\":1785600000}",
        "{\"ms\":-1785600000123}",
    };
    for (size_t index = 0U;
         index < sizeof(invalid) / sizeof(invalid[0]);
         ++index) {
        int64_t epoch_ms = INT64_C(5678);
        TEST_ASSERT_FALSE(backend_time_parse_response(
            invalid[index], strlen(invalid[index]), &epoch_ms));
        TEST_ASSERT_EQUAL_INT64(INT64_C(5678), epoch_ms);
    }
}

void test_backend_time_rejects_embedded_nul_and_trailing_bytes(void)
{
    static const char embedded_nul[] = {
        '{', '"', 'm', 's', '"', ':',
        '1', '7', '8', '5', '6', '0', '0', '0', '0', '0', '1', '2', '3',
        '}', '\0', 'x',
    };
    static const char trailing[] = "{\"ms\":1785600000123}x";
    int64_t epoch_ms = INT64_C(9012);

    TEST_ASSERT_FALSE(backend_time_parse_response(
        embedded_nul, sizeof(embedded_nul), &epoch_ms));
    TEST_ASSERT_EQUAL_INT64(INT64_C(9012), epoch_ms);
    TEST_ASSERT_FALSE(backend_time_parse_response(
        trailing, sizeof(trailing) - 1U, &epoch_ms));
    TEST_ASSERT_FALSE(backend_time_parse_response(NULL, 0U, &epoch_ms));
    TEST_ASSERT_FALSE(backend_time_parse_response(trailing, 1U, NULL));
}

void test_time_source_prefers_valid_sntp_then_falls_back_to_backend(void)
{
    int64_t epoch_ms = -1;
    TEST_ASSERT_EQUAL(
        BACKEND_TIME_SOURCE_SNTP,
        backend_time_select_source(
            true,
            INT64_C(1785600001000),
            true,
            INT64_C(1785600002000),
            &epoch_ms));
    TEST_ASSERT_EQUAL_INT64(INT64_C(1785600001000), epoch_ms);

    TEST_ASSERT_EQUAL(
        BACKEND_TIME_SOURCE_BACKEND,
        backend_time_select_source(
            false,
            INT64_C(1785600001000),
            true,
            INT64_C(1785600002000),
            &epoch_ms));
    TEST_ASSERT_EQUAL_INT64(INT64_C(1785600002000), epoch_ms);

    TEST_ASSERT_EQUAL(
        BACKEND_TIME_SOURCE_BACKEND,
        backend_time_select_source(
            true,
            INT64_C(1700000000000),
            true,
            INT64_C(1785600003000),
            &epoch_ms));
    TEST_ASSERT_EQUAL_INT64(INT64_C(1785600003000), epoch_ms);

    TEST_ASSERT_EQUAL(
        BACKEND_TIME_SOURCE_NONE,
        backend_time_select_source(
            false, 0, false, 0, &epoch_ms));
    TEST_ASSERT_EQUAL_INT64(0, epoch_ms);
    TEST_ASSERT_EQUAL(
        BACKEND_TIME_SOURCE_NONE,
        backend_time_select_source(
            true, INT64_C(1785600001000), false, 0, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_backend_time_requires_exact_single_ms_object);
    BACKEND_RUN_TEST(
        test_backend_time_rejects_extra_duplicate_and_non_object_members);
    BACKEND_RUN_TEST(
        test_backend_time_rejects_noninteger_range_and_pre_epoch_values);
    BACKEND_RUN_TEST(
        test_backend_time_rejects_embedded_nul_and_trailing_bytes);
    BACKEND_RUN_TEST(
        test_time_source_prefers_valid_sntp_then_falls_back_to_backend);
    return UNITY_END();
}
