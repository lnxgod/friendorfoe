#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "backend_ingest_ack.h"
#include "../support/backend_test_main.h"

void setUp(void) {}
void tearDown(void) {}

void test_ack_requires_exact_device_count_and_counter_sum(void)
{
    static const char ok[] =
        "{\"status\":\"ok\",\"accepted\":2,\"processed\":1,"
        "\"deduplicated\":1,\"filtered\":0,"
        "\"device_id\":\"uplink_CB77A4\"}";
    TEST_ASSERT_TRUE(backend_ingest_ack_validate(
        ok, sizeof(ok) - 1U, "uplink_CB77A4", 2U));
    TEST_ASSERT_FALSE(backend_ingest_ack_validate(
        ok, sizeof(ok) - 1U, "uplink_OTHER", 2U));
    TEST_ASSERT_FALSE(backend_ingest_ack_validate(
        ok, sizeof(ok) - 1U, "uplink_CB77A4", 1U));

    static const char bad_sum[] =
        "{\"status\":\"ok\",\"accepted\":2,\"processed\":2,"
        "\"deduplicated\":1,\"filtered\":0,"
        "\"device_id\":\"uplink_CB77A4\"}";
    TEST_ASSERT_FALSE(backend_ingest_ack_validate(
        bad_sum, sizeof(bad_sum) - 1U, "uplink_CB77A4", 2U));
}

void test_ack_rejects_missing_duplicate_unknown_and_noninteger_fields(void)
{
    static const char missing[] =
        "{\"status\":\"ok\",\"accepted\":1,\"processed\":1,"
        "\"deduplicated\":0,\"device_id\":\"uplink_CB77A4\"}";
    static const char duplicate[] =
        "{\"status\":\"ok\",\"accepted\":1,\"accepted\":1,"
        "\"processed\":1,\"deduplicated\":0,\"filtered\":0,"
        "\"device_id\":\"uplink_CB77A4\"}";
    static const char unknown[] =
        "{\"status\":\"ok\",\"accepted\":1,\"processed\":1,"
        "\"deduplicated\":0,\"filtered\":0,\"extra\":0,"
        "\"device_id\":\"uplink_CB77A4\"}";
    static const char fractional[] =
        "{\"status\":\"ok\",\"accepted\":1,\"processed\":0.5,"
        "\"deduplicated\":0,\"filtered\":0,"
        "\"device_id\":\"uplink_CB77A4\"}";
    static const char not_ok[] =
        "{\"status\":\"partial\",\"accepted\":1,\"processed\":1,"
        "\"deduplicated\":0,\"filtered\":0,"
        "\"device_id\":\"uplink_CB77A4\"}";

    TEST_ASSERT_FALSE(backend_ingest_ack_validate(
        missing, sizeof(missing) - 1U, "uplink_CB77A4", 1U));
    TEST_ASSERT_FALSE(backend_ingest_ack_validate(
        duplicate, sizeof(duplicate) - 1U, "uplink_CB77A4", 1U));
    TEST_ASSERT_FALSE(backend_ingest_ack_validate(
        unknown, sizeof(unknown) - 1U, "uplink_CB77A4", 1U));
    TEST_ASSERT_FALSE(backend_ingest_ack_validate(
        fractional, sizeof(fractional) - 1U, "uplink_CB77A4", 1U));
    TEST_ASSERT_FALSE(backend_ingest_ack_validate(
        not_ok, sizeof(not_ok) - 1U, "uplink_CB77A4", 1U));
    TEST_ASSERT_FALSE(backend_ingest_ack_validate(NULL, 0U, NULL, 0U));
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(test_ack_requires_exact_device_count_and_counter_sum);
    BACKEND_RUN_TEST(
        test_ack_rejects_missing_duplicate_unknown_and_noninteger_fields);
    return UNITY_END();
}
