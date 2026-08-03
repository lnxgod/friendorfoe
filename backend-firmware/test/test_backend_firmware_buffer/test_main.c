#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <unity.h>

#include "backend_firmware_buffer.h"
#include "../support/backend_test_main.h"

typedef struct {
    uint32_t calls;
    size_t last_size;
    void *last_context;
    bool fail;
} counting_allocator_t;

static uint8_t firmware_storage[BACKEND_FIRMWARE_BUFFER_CAPACITY];

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
static backend_ota_operation_id_t test_operation_id(uint32_t value)
{
    backend_ota_operation_id_t operation_id = {0};
    operation_id.bytes[12] = (uint8_t)(value >> 24U);
    operation_id.bytes[13] = (uint8_t)(value >> 16U);
    operation_id.bytes[14] = (uint8_t)(value >> 8U);
    operation_id.bytes[15] = (uint8_t)value;
    return operation_id;
}
#endif

static bool test_buffer_acquire(
    backend_firmware_buffer_t *buffer, uint32_t owner)
{
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    const backend_ota_operation_id_t operation_id = test_operation_id(owner);
    return backend_firmware_buffer_acquire(buffer, true, &operation_id);
#else
    return backend_firmware_buffer_acquire(buffer, owner);
#endif
}

static void test_buffer_release(
    backend_firmware_buffer_t *buffer, uint32_t owner)
{
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    const backend_ota_operation_id_t operation_id = test_operation_id(owner);
    backend_firmware_buffer_release(buffer, true, &operation_id);
#else
    backend_firmware_buffer_release(buffer, owner);
#endif
}

void setUp(void) {}
void tearDown(void) {}

static void *counting_psram_alloc(size_t size, void *context)
{
    counting_allocator_t *allocator = context;
    allocator->calls++;
    allocator->last_size = size;
    allocator->last_context = context;
    return allocator->fail ? NULL : firmware_storage;
}

void test_firmware_buffer_allocates_once_and_serializes_generations(void)
{
    counting_allocator_t allocator = {0};
    backend_firmware_buffer_t buffer = {0};
    TEST_ASSERT_TRUE(backend_firmware_buffer_init_once(
        &buffer, counting_psram_alloc, &allocator));
    TEST_ASSERT_EQUAL_UINT32(1U, allocator.calls);
    TEST_ASSERT_EQUAL_UINT32(
        BACKEND_FIRMWARE_BUFFER_CAPACITY, allocator.last_size);
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(0x200000), allocator.last_size);
#else
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(0x300000), allocator.last_size);
#endif
    TEST_ASSERT_EQUAL_PTR(&allocator, allocator.last_context);
    TEST_ASSERT_EQUAL_PTR(
        firmware_storage, backend_firmware_buffer_data(&buffer));
    TEST_ASSERT_EQUAL_UINT32(
        BACKEND_FIRMWARE_BUFFER_CAPACITY,
        backend_firmware_buffer_capacity(&buffer));

    TEST_ASSERT_TRUE(test_buffer_acquire(&buffer, 7U));
    TEST_ASSERT_FALSE(test_buffer_acquire(&buffer, 8U));
    TEST_ASSERT_FALSE(test_buffer_acquire(&buffer, 7U));
    test_buffer_release(&buffer, 6U);
    TEST_ASSERT_FALSE(test_buffer_acquire(&buffer, 8U));
    test_buffer_release(&buffer, 7U);
    TEST_ASSERT_TRUE(test_buffer_acquire(&buffer, 8U));

    TEST_ASSERT_TRUE(backend_firmware_buffer_init_once(
        &buffer, counting_psram_alloc, &allocator));
    TEST_ASSERT_EQUAL_UINT32(1U, allocator.calls);
    TEST_ASSERT_TRUE(buffer.acquired);
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    const backend_ota_operation_id_t expected_owner = test_operation_id(8U);
    TEST_ASSERT_TRUE(buffer.has_owner_operation_id);
    TEST_ASSERT_TRUE(backend_ota_operation_id_equal(
        &expected_owner, &buffer.owner_operation_id));
#else
    TEST_ASSERT_EQUAL_UINT32(8U, buffer.owner_generation);
#endif
}

void test_allocation_failure_is_sticky_and_never_retries_per_request(void)
{
    counting_allocator_t allocator = {.fail = true};
    backend_firmware_buffer_t buffer = {0};

    TEST_ASSERT_FALSE(backend_firmware_buffer_init_once(
        &buffer, counting_psram_alloc, &allocator));
    TEST_ASSERT_TRUE(buffer.initialized);
    TEST_ASSERT_EQUAL_UINT32(1U, allocator.calls);
    TEST_ASSERT_NULL(backend_firmware_buffer_data(&buffer));
    TEST_ASSERT_EQUAL_UINT32(
        0U, backend_firmware_buffer_capacity(&buffer));
    TEST_ASSERT_FALSE(test_buffer_acquire(&buffer, 1U));

    allocator.fail = false;
    TEST_ASSERT_FALSE(backend_firmware_buffer_init_once(
        &buffer, counting_psram_alloc, &allocator));
    TEST_ASSERT_EQUAL_UINT32(1U, allocator.calls);
    TEST_ASSERT_NULL(backend_firmware_buffer_data(&buffer));
}

void test_zero_generation_and_invalid_arguments_never_claim_the_arena(void)
{
    counting_allocator_t allocator = {0};
    backend_firmware_buffer_t buffer = {0};

    TEST_ASSERT_FALSE(backend_firmware_buffer_init_once(
        NULL, counting_psram_alloc, &allocator));
    TEST_ASSERT_FALSE(backend_firmware_buffer_init_once(
        &buffer, NULL, &allocator));
    TEST_ASSERT_EQUAL_UINT32(0U, allocator.calls);
    TEST_ASSERT_FALSE(test_buffer_acquire(NULL, 1U));
    TEST_ASSERT_FALSE(test_buffer_acquire(&buffer, 1U));
    test_buffer_release(NULL, 1U);
    TEST_ASSERT_NULL(backend_firmware_buffer_data(NULL));
    TEST_ASSERT_EQUAL_UINT32(
        0U, backend_firmware_buffer_capacity(NULL));

    TEST_ASSERT_TRUE(backend_firmware_buffer_init_once(
        &buffer, counting_psram_alloc, &allocator));
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    const backend_ota_operation_id_t zero = {0};
    TEST_ASSERT_FALSE(backend_firmware_buffer_acquire(
        &buffer, false, &zero));
#else
    TEST_ASSERT_FALSE(test_buffer_acquire(&buffer, 0U));
#endif
    TEST_ASSERT_FALSE(buffer.acquired);
}

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
void test_fullsize_buffer_owner_compares_all_operation_id_bytes(void)
{
    counting_allocator_t allocator = {0};
    backend_firmware_buffer_t buffer = {0};
    backend_ota_operation_id_t owner;
    TEST_ASSERT_TRUE(backend_ota_operation_id_decode(
        "0123456789abcdef0123456789abcdef", &owner));
    TEST_ASSERT_TRUE(backend_firmware_buffer_init_once(
        &buffer, counting_psram_alloc, &allocator));
    TEST_ASSERT_FALSE(backend_firmware_buffer_acquire(
        &buffer, false, &owner));
    TEST_ASSERT_TRUE(backend_firmware_buffer_acquire(
        &buffer, true, &owner));
    for (size_t byte = 0U; byte < sizeof(owner.bytes); ++byte) {
        backend_ota_operation_id_t different = owner;
        different.bytes[byte] ^= UINT8_C(1);
        backend_firmware_buffer_release(&buffer, true, &different);
        TEST_ASSERT_TRUE(buffer.acquired);
    }
    backend_firmware_buffer_release(&buffer, true, &owner);
    TEST_ASSERT_FALSE(buffer.acquired);

    backend_ota_operation_id_t zero = {0};
    TEST_ASSERT_TRUE(backend_firmware_buffer_acquire(
        &buffer, true, &zero));
    backend_firmware_buffer_release(&buffer, true, &zero);
    TEST_ASSERT_FALSE(buffer.acquired);
}
#endif

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_firmware_buffer_allocates_once_and_serializes_generations);
    BACKEND_RUN_TEST(
        test_allocation_failure_is_sticky_and_never_retries_per_request);
    BACKEND_RUN_TEST(
        test_zero_generation_and_invalid_arguments_never_claim_the_arena);
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    BACKEND_RUN_TEST(test_fullsize_buffer_owner_compares_all_operation_id_bytes);
#endif
    return UNITY_END();
}
