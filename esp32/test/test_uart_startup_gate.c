#include "unity.h"

#include "uart_startup_gate.h"

typedef struct {
    fw_operation_token_t claimed_token;
    fw_operation_token_t released_tokens[4];
    unsigned claim_calls;
    unsigned start_calls;
    unsigned release_calls;
    unsigned delay_calls;
    uint32_t delays_ms[8];
    bool claim_succeeds;
    bool start_succeeds;
    bool actual_task_entry;
    unsigned release_failures_remaining;
} fake_uart_startup_t;

static bool fake_claim(void *context, fw_operation_token_t *out)
{
    fake_uart_startup_t *fake = context;
    fake->claim_calls++;
    if (!fake->claim_succeeds) {
        return false;
    }
    *out = fake->claimed_token;
    return true;
}

static bool fake_start(void *context)
{
    fake_uart_startup_t *fake = context;
    fake->start_calls++;
    fake->actual_task_entry = fake->start_succeeds;
    return fake->start_succeeds;
}

static bool fake_release(void *context, fw_operation_token_t token)
{
    fake_uart_startup_t *fake = context;
    fake->released_tokens[fake->release_calls++] = token;
    if (fake->release_failures_remaining > 0U) {
        fake->release_failures_remaining--;
        return false;
    }
    return true;
}

static void fake_delay(void *context, uint32_t delay_ms)
{
    fake_uart_startup_t *fake = context;
    fake->delays_ms[fake->delay_calls++] = delay_ms;
}

static uart_startup_gate_hooks_t hooks(fake_uart_startup_t *fake)
{
    return (uart_startup_gate_hooks_t) {
        .context = fake,
        .try_claim = fake_claim,
        .start = fake_start,
        .release = fake_release,
        .delay = fake_delay,
    };
}

static fake_uart_startup_t fake_startup(void)
{
    fake_uart_startup_t fake = {
        .claimed_token = {
            .owner = FW_OPERATION_OWNER_RUNTIME_STARTUP,
            .generation = 77U,
            .valid = true,
        },
        .claim_succeeds = true,
        .start_succeeds = true,
    };
    return fake;
}

void test_uart_startup_gate_distinguishes_claim_and_start_failures(void)
{
    fake_uart_startup_t fake = fake_startup();
    fake.claim_succeeds = false;
    uart_startup_gate_hooks_t gate_hooks = hooks(&fake);

    TEST_ASSERT_EQUAL(
        UART_STARTUP_GATE_CLAIM_TIMEOUT,
        uart_startup_gate_run(&gate_hooks, 3U, 50U, 3U, 10U));
    TEST_ASSERT_EQUAL_UINT32(3U, fake.claim_calls);
    TEST_ASSERT_EQUAL_UINT32(2U, fake.delay_calls);
    TEST_ASSERT_EQUAL_UINT32(50U, fake.delays_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(0U, fake.start_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, fake.release_calls);

    fake = fake_startup();
    fake.start_succeeds = false;
    gate_hooks = hooks(&fake);
    TEST_ASSERT_EQUAL(
        UART_STARTUP_GATE_START_FAILED,
        uart_startup_gate_run(&gate_hooks, 3U, 50U, 3U, 10U));
    TEST_ASSERT_EQUAL_UINT32(1U, fake.start_calls);
    TEST_ASSERT_FALSE(fake.actual_task_entry);
    TEST_ASSERT_EQUAL_UINT32(1U, fake.release_calls);
}

void test_uart_startup_gate_retries_exact_token_after_actual_task_entry(void)
{
    fake_uart_startup_t fake = fake_startup();
    fake.release_failures_remaining = 3U;
    uart_startup_gate_hooks_t gate_hooks = hooks(&fake);

    TEST_ASSERT_EQUAL(
        UART_STARTUP_GATE_RELEASE_FAILED,
        uart_startup_gate_run(&gate_hooks, 2U, 50U, 3U, 10U));
    TEST_ASSERT_TRUE(fake.actual_task_entry);
    TEST_ASSERT_EQUAL_UINT32(1U, fake.start_calls);
    TEST_ASSERT_EQUAL_UINT32(3U, fake.release_calls);
    TEST_ASSERT_EQUAL_UINT32(2U, fake.delay_calls);
    TEST_ASSERT_EQUAL_UINT32(10U, fake.delays_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(10U, fake.delays_ms[1]);
    for (unsigned attempt = 0U; attempt < fake.release_calls; ++attempt) {
        TEST_ASSERT_EQUAL(fake.claimed_token.owner,
                          fake.released_tokens[attempt].owner);
        TEST_ASSERT_EQUAL_UINT32(fake.claimed_token.generation,
                                 fake.released_tokens[attempt].generation);
        TEST_ASSERT_EQUAL(fake.claimed_token.valid,
                          fake.released_tokens[attempt].valid);
    }
}

void test_uart_startup_gate_release_retry_can_recover_to_success(void)
{
    fake_uart_startup_t fake = fake_startup();
    fake.release_failures_remaining = 2U;
    uart_startup_gate_hooks_t gate_hooks = hooks(&fake);

    TEST_ASSERT_EQUAL(
        UART_STARTUP_GATE_OK,
        uart_startup_gate_run(&gate_hooks, 1U, 50U, 3U, 10U));
    TEST_ASSERT_TRUE(fake.actual_task_entry);
    TEST_ASSERT_EQUAL_UINT32(3U, fake.release_calls);
    TEST_ASSERT_EQUAL_UINT32(2U, fake.delay_calls);
}
