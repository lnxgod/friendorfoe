#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "backend_identity.h"
#include "backend_upload_fifo.h"
#include "../support/backend_test_main.h"

void setUp(void) {}
void tearDown(void) {}

static backend_upload_batch_t fixture_batch(uint32_t sequence)
{
    backend_upload_batch_t batch = {
        .sequence = sequence,
        .item_count = 1U,
        .json_len = 7U,
        .json = "{\"x\":1}",
    };
    batch.json_crc32 = backend_identity_crc32(batch.json, batch.json_len);
    return batch;
}

void test_fifo_drops_oldest_whole_batch_and_never_reorders(void)
{
    backend_upload_batch_t storage[3];
    backend_upload_fifo_t fifo;
    backend_upload_fifo_init(&fifo, storage, 3U);
    TEST_ASSERT_TRUE(backend_upload_fifo_is_valid(&fifo));

    for (uint32_t sequence = 1U; sequence <= 4U; ++sequence) {
        backend_upload_batch_t batch = fixture_batch(sequence);
        bool dropped = true;
        TEST_ASSERT_TRUE(backend_upload_fifo_push(&fifo, &batch, &dropped));
        TEST_ASSERT_EQUAL(sequence == 4U, dropped);
    }
    TEST_ASSERT_EQUAL_UINT32(1U, fifo.dropped_batches);
    TEST_ASSERT_EQUAL_UINT16(3U, fifo.count);
    TEST_ASSERT_EQUAL_UINT32(
        2U, backend_upload_fifo_peek(&fifo)->sequence);
    TEST_ASSERT_FALSE(backend_upload_fifo_pop_acked(&fifo, 3U));
    TEST_ASSERT_TRUE(backend_upload_fifo_pop_acked(&fifo, 2U));
    TEST_ASSERT_EQUAL_UINT32(
        3U, backend_upload_fifo_peek(&fifo)->sequence);
    TEST_ASSERT_TRUE(backend_upload_fifo_pop_acked(&fifo, 3U));
    TEST_ASSERT_TRUE(backend_upload_fifo_pop_acked(&fifo, 4U));
    TEST_ASSERT_NULL(backend_upload_fifo_peek(&fifo));
}

void test_fifo_copies_immutable_batches_and_rejects_invalid_inputs(void)
{
    backend_upload_batch_t storage[1];
    backend_upload_fifo_t fifo;
    backend_upload_fifo_init(&fifo, storage, 1U);
    backend_upload_batch_t batch = fixture_batch(7U);
    TEST_ASSERT_TRUE(backend_upload_fifo_push(&fifo, &batch, NULL));
    batch.sequence = 99U;
    batch.json[0] = 'X';
    TEST_ASSERT_EQUAL_UINT32(7U, backend_upload_fifo_peek(&fifo)->sequence);
    TEST_ASSERT_EQUAL_CHAR('{', backend_upload_fifo_peek(&fifo)->json[0]);

    backend_upload_fifo_t invalid;
    backend_upload_fifo_init(&invalid, NULL, 1U);
    TEST_ASSERT_FALSE(backend_upload_fifo_is_valid(&invalid));
    backend_upload_fifo_init(&invalid, storage, 0U);
    TEST_ASSERT_FALSE(backend_upload_fifo_is_valid(&invalid));
    backend_upload_fifo_init(
        &invalid, storage, BACKEND_UPLOAD_FIFO_CAPACITY + 1U);
    TEST_ASSERT_FALSE(backend_upload_fifo_is_valid(&invalid));
    TEST_ASSERT_FALSE(backend_upload_fifo_push(NULL, &batch, NULL));
    TEST_ASSERT_FALSE(backend_upload_fifo_push(&invalid, &batch, NULL));

    backend_upload_batch_t malformed = fixture_batch(0U);
    TEST_ASSERT_FALSE(backend_upload_fifo_push(&fifo, &malformed, NULL));
    malformed = fixture_batch(8U);
    malformed.json_len = BACKEND_UPLOAD_MAX_JSON + 1U;
    TEST_ASSERT_FALSE(backend_upload_fifo_push(&fifo, &malformed, NULL));

    malformed = fixture_batch(8U);
    malformed.json[2] = 'y';
    TEST_ASSERT_FALSE(backend_upload_fifo_push(&fifo, &malformed, NULL));
    malformed = fixture_batch(8U);
    malformed.json_crc32 ^= UINT32_C(1);
    TEST_ASSERT_FALSE(backend_upload_fifo_push(&fifo, &malformed, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(test_fifo_drops_oldest_whole_batch_and_never_reorders);
    BACKEND_RUN_TEST(
        test_fifo_copies_immutable_batches_and_rejects_invalid_inputs);
    return UNITY_END();
}
