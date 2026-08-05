#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "backend_http_policy.h"
#include "../support/backend_test_main.h"

void setUp(void) {}
void tearDown(void) {}

void test_http_policy_classifies_retry_ack_and_permanent_errors(void)
{
    TEST_ASSERT_EQUAL(BACKEND_HTTP_RETRY,
        backend_http_classify(false, 0, false));
    TEST_ASSERT_EQUAL(BACKEND_HTTP_RETRY,
        backend_http_classify(true, 408, false));
    TEST_ASSERT_EQUAL(BACKEND_HTTP_RETRY,
        backend_http_classify(true, 429, false));
    TEST_ASSERT_EQUAL(BACKEND_HTTP_RETRY,
        backend_http_classify(true, 503, false));
    TEST_ASSERT_EQUAL(BACKEND_HTTP_RETRY,
        backend_http_classify(true, 600, false));
    TEST_ASSERT_EQUAL(BACKEND_HTTP_QUARANTINE,
        backend_http_classify(true, 400, false));
    TEST_ASSERT_EQUAL(BACKEND_HTTP_RETRY,
        backend_http_classify(true, 200, false));
    TEST_ASSERT_EQUAL(BACKEND_HTTP_ACK,
        backend_http_classify(true, 200, true));
    TEST_ASSERT_EQUAL(BACKEND_HTTP_RETRY,
        backend_http_classify(true, 302, false));
    TEST_ASSERT_EQUAL(BACKEND_HTTP_RETRY,
        backend_http_classify(true, 0, false));
    TEST_ASSERT_EQUAL(BACKEND_HTTP_QUARANTINE,
        backend_http_classify(true, 418, false));
}

typedef struct {
    size_t call;
    size_t total;
    uint8_t captured[16];
} send_fixture_t;

static ssize_t partial_sender(void *context, const void *data, size_t length)
{
    send_fixture_t *fixture = context;
    static const size_t chunks[] = {1U, 3U, 99U};
    size_t amount = chunks[fixture->call++];
    if (amount > length) {
        amount = length;
    }
    memcpy(fixture->captured + fixture->total, data, amount);
    fixture->total += amount;
    return (ssize_t)amount;
}

static ssize_t zero_sender(void *context, const void *data, size_t length)
{
    (void)context;
    (void)data;
    (void)length;
    return 0;
}

void test_send_all_handles_partial_writes_and_fails_on_no_progress(void)
{
    static const uint8_t payload[] = "abcdefgh";
    send_fixture_t fixture = {0};
    TEST_ASSERT_TRUE(backend_http_send_all(
        partial_sender, &fixture, payload, sizeof(payload) - 1U));
    TEST_ASSERT_EQUAL_UINT(sizeof(payload) - 1U, fixture.total);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        payload, fixture.captured, sizeof(payload) - 1U);
    TEST_ASSERT_FALSE(backend_http_send_all(
        zero_sender, NULL, payload, sizeof(payload) - 1U));
    TEST_ASSERT_FALSE(backend_http_send_all(
        NULL, NULL, payload, sizeof(payload) - 1U));
    TEST_ASSERT_TRUE(backend_http_send_all(
        partial_sender, &fixture, NULL, 0U));
}

void test_retry_delay_has_exact_exponential_cap_and_deterministic_jitter(void)
{
    TEST_ASSERT_EQUAL_UINT32(500U, backend_retry_delay_ms(0U, 0U));
    TEST_ASSERT_EQUAL_UINT32(625U, backend_retry_delay_ms(0U, 125U));
    TEST_ASSERT_EQUAL_UINT32(1000U, backend_retry_delay_ms(1U, 0U));
    TEST_ASSERT_EQUAL_UINT32(60000U, backend_retry_delay_ms(7U, 0U));
    TEST_ASSERT_EQUAL_UINT32(75000U, backend_retry_delay_ms(31U, 15000U));
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_http_policy_classifies_retry_ack_and_permanent_errors);
    BACKEND_RUN_TEST(
        test_send_all_handles_partial_writes_and_fails_on_no_progress);
    BACKEND_RUN_TEST(
        test_retry_delay_has_exact_exponential_cap_and_deterministic_jitter);
    return UNITY_END();
}
