#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "backend_ota_operation_id.h"
#include "../support/backend_test_main.h"

void setUp(void) {}
void tearDown(void) {}

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)

void test_fullsize_operation_id_round_trips_only_canonical_lower_hex(void)
{
    backend_ota_operation_id_t operation_id;
    char encoded[33] = {0};

    TEST_ASSERT_TRUE(backend_ota_operation_id_decode(
        "0123456789abcdef0123456789abcdef", &operation_id));
    TEST_ASSERT_TRUE(backend_ota_operation_id_encode(
        &operation_id, encoded, sizeof(encoded)));
    TEST_ASSERT_EQUAL_STRING(
        "0123456789abcdef0123456789abcdef", encoded);
    TEST_ASSERT_FALSE(backend_ota_operation_id_decode(
        "0123456789ABCDEF0123456789abcdef", &operation_id));
    TEST_ASSERT_FALSE(backend_ota_operation_id_decode(
        "0123456789abcdef0123456789abcdeg", &operation_id));
}

void test_fullsize_operation_id_equality_compares_all_sixteen_bytes(void)
{
    backend_ota_operation_id_t first;
    backend_ota_operation_id_t equal;
    backend_ota_operation_id_t different;

    TEST_ASSERT_TRUE(backend_ota_operation_id_decode(
        "0123456789abcdef0123456789abcdef", &first));
    TEST_ASSERT_TRUE(backend_ota_operation_id_decode(
        "0123456789abcdef0123456789abcdef", &equal));
    TEST_ASSERT_TRUE(backend_ota_operation_id_equal(&first, &equal));
    for (size_t index = 0U; index < sizeof(first.bytes); ++index) {
        different = first;
        different.bytes[index] ^= UINT8_C(0x01);
        TEST_ASSERT_FALSE(backend_ota_operation_id_equal(&first, &different));
    }
}

#else

void test_lite_operation_id_remains_a_four_byte_numeric_abi(void)
{
    const backend_ota_operation_id_t operation_id = UINT32_C(0x04030201);
    TEST_ASSERT_EQUAL_UINT(4U, sizeof(operation_id));
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(0x04030201), operation_id);
}

#endif

int main(void)
{
    UNITY_BEGIN();
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    BACKEND_RUN_TEST(
        test_fullsize_operation_id_round_trips_only_canonical_lower_hex);
    BACKEND_RUN_TEST(
        test_fullsize_operation_id_equality_compares_all_sixteen_bytes);
#else
    BACKEND_RUN_TEST(test_lite_operation_id_remains_a_four_byte_numeric_abi);
#endif
    return UNITY_END();
}
