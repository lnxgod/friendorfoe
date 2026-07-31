#include "unity.h"

#include "firmware_operation_token.h"

static fw_operation_state_t clean_state(void)
{
    fw_operation_state_t state;
    fw_operation_state_init(&state);
    return state;
}

void test_fw_operation_rejects_wrong_owner_stale_and_double_release(void)
{
    fw_operation_state_t state = clean_state();
    fw_operation_token_t first = {0};
    bool release_uart = true;

    TEST_ASSERT_TRUE(fw_operation_state_try_begin(
        &state, FW_OPERATION_OWNER_SCANNER_STAGING, &first));
    TEST_ASSERT_TRUE(first.valid);
    TEST_ASSERT_NOT_EQUAL(0U, first.generation);
    TEST_ASSERT_TRUE(fw_operation_state_attach_uart_lease(&state, first));

    fw_operation_token_t wrong_owner = first;
    wrong_owner.owner = FW_OPERATION_OWNER_SCANNER_RELAY;
    TEST_ASSERT_FALSE(fw_operation_state_end(
        &state, wrong_owner, &release_uart));
    TEST_ASSERT_FALSE(release_uart);
    TEST_ASSERT_TRUE(state.active);
    TEST_ASSERT_EQUAL(FW_OPERATION_OWNER_SCANNER_STAGING, state.owner);
    TEST_ASSERT_EQUAL_UINT32(first.generation, state.generation);
    TEST_ASSERT_TRUE(state.uart_lease);

    fw_operation_token_t stale = first;
    stale.generation++;
    if (stale.generation == 0U) {
        stale.generation = 1U;
    }
    TEST_ASSERT_FALSE(fw_operation_state_end(&state, stale, &release_uart));
    TEST_ASSERT_FALSE(release_uart);
    TEST_ASSERT_TRUE(state.active);
    TEST_ASSERT_EQUAL(FW_OPERATION_OWNER_SCANNER_STAGING, state.owner);
    TEST_ASSERT_EQUAL_UINT32(first.generation, state.generation);
    TEST_ASSERT_TRUE(state.uart_lease);

    TEST_ASSERT_TRUE(fw_operation_state_end(&state, first, &release_uart));
    TEST_ASSERT_TRUE(release_uart);
    TEST_ASSERT_FALSE(fw_operation_state_end(&state, first, &release_uart));
    TEST_ASSERT_FALSE(release_uart);
}

void test_fw_operation_invalid_wrong_and_stale_tokens_cannot_attach_uart(void)
{
    fw_operation_state_t state = clean_state();
    fw_operation_token_t active = {0};
    fw_operation_token_t invalid = {0};

    TEST_ASSERT_TRUE(fw_operation_state_try_begin(
        &state, FW_OPERATION_OWNER_SCANNER_STAGING, &active));
    fw_operation_owner_t expected_owner = state.owner;
    uint32_t expected_generation = state.generation;

    TEST_ASSERT_FALSE(fw_operation_state_attach_uart_lease(&state, invalid));
    TEST_ASSERT_TRUE(state.active);
    TEST_ASSERT_EQUAL(expected_owner, state.owner);
    TEST_ASSERT_EQUAL_UINT32(expected_generation, state.generation);
    TEST_ASSERT_FALSE(state.uart_lease);

    fw_operation_token_t wrong_owner = active;
    wrong_owner.owner = FW_OPERATION_OWNER_SCANNER_RELAY;
    TEST_ASSERT_FALSE(fw_operation_state_attach_uart_lease(
        &state, wrong_owner));
    TEST_ASSERT_TRUE(state.active);
    TEST_ASSERT_EQUAL(expected_owner, state.owner);
    TEST_ASSERT_EQUAL_UINT32(expected_generation, state.generation);
    TEST_ASSERT_FALSE(state.uart_lease);

    fw_operation_token_t stale = active;
    stale.generation++;
    TEST_ASSERT_FALSE(fw_operation_state_attach_uart_lease(&state, stale));
    TEST_ASSERT_TRUE(state.active);
    TEST_ASSERT_EQUAL(expected_owner, state.owner);
    TEST_ASSERT_EQUAL_UINT32(expected_generation, state.generation);
    TEST_ASSERT_FALSE(state.uart_lease);
}

void test_fw_operation_failed_optional_uart_lease_cleanup_allows_reclaim(void)
{
    fw_operation_state_t state = clean_state();
    fw_operation_token_t failed = {0};
    fw_operation_token_t next = {0};
    bool release_uart = true;

    TEST_ASSERT_TRUE(fw_operation_state_try_begin(
        &state, FW_OPERATION_OWNER_SCANNER_STAGING, &failed));
    /* UART acquisition failed before the acquired lease was attached. */
    TEST_ASSERT_TRUE(fw_operation_state_end(&state, failed, &release_uart));
    TEST_ASSERT_FALSE(release_uart);
    TEST_ASSERT_TRUE(fw_operation_state_try_begin(
        &state, FW_OPERATION_OWNER_SCANNER_STAGING, &next));
    TEST_ASSERT_NOT_EQUAL(failed.generation, next.generation);
}

void test_fw_operation_recovery_restart_collision_is_fail_closed(void)
{
    fw_operation_state_t state = clean_state();
    fw_operation_token_t token = {0};

    TEST_ASSERT_TRUE(fw_operation_state_try_reserve_recovery_restart(&state));
    TEST_ASSERT_FALSE(fw_operation_state_try_begin(
        &state, FW_OPERATION_OWNER_SCANNER_RELAY, &token));
    TEST_ASSERT_FALSE(fw_operation_state_try_reserve_recovery_restart(&state));

    state = clean_state();
    TEST_ASSERT_TRUE(fw_operation_state_try_begin(
        &state, FW_OPERATION_OWNER_SCANNER_RELAY, &token));
    TEST_ASSERT_FALSE(fw_operation_state_try_reserve_recovery_restart(&state));
}

void test_fw_operation_preserves_scanner_staging_and_relay_uart_semantics(void)
{
    fw_operation_state_t state = clean_state();
    fw_operation_token_t staging = {0};
    fw_operation_token_t relay = {0};
    bool release_uart = false;

    TEST_ASSERT_TRUE(fw_operation_state_try_begin(
        &state, FW_OPERATION_OWNER_SCANNER_STAGING, &staging));
    TEST_ASSERT_TRUE(fw_operation_state_attach_uart_lease(&state, staging));
    TEST_ASSERT_FALSE(fw_operation_state_try_begin(
        &state, FW_OPERATION_OWNER_SCANNER_RELAY, &relay));
    TEST_ASSERT_TRUE(fw_operation_state_end(
        &state, staging, &release_uart));
    TEST_ASSERT_TRUE(release_uart);

    TEST_ASSERT_TRUE(fw_operation_state_try_begin(
        &state, FW_OPERATION_OWNER_SCANNER_RELAY, &relay));
    TEST_ASSERT_TRUE(fw_operation_state_attach_uart_lease(&state, relay));
    TEST_ASSERT_TRUE(fw_operation_state_end(&state, relay, &release_uart));
    TEST_ASSERT_TRUE(release_uart);
}

void test_fw_operation_scanner_staging_follows_aborted_non_uart_uplink_owner(void)
{
    fw_operation_state_t state = clean_state();
    fw_operation_token_t uplink = {0};
    fw_operation_token_t scanner = {0};
    bool release_uart = true;

    TEST_ASSERT_TRUE(fw_operation_state_try_begin(
        &state, FW_OPERATION_OWNER_UPLINK_OTA, &uplink));
    TEST_ASSERT_TRUE(fw_operation_state_end(&state, uplink, &release_uart));
    TEST_ASSERT_FALSE(release_uart);
    TEST_ASSERT_TRUE(fw_operation_state_try_begin(
        &state, FW_OPERATION_OWNER_SCANNER_STAGING, &scanner));
}

void test_fw_operation_generation_exhaustion_never_reuses_a_stale_token(void)
{
    fw_operation_state_t state = clean_state();
    fw_operation_token_t final = {0};
    fw_operation_token_t reused = {
        .owner = FW_OPERATION_OWNER_SCANNER_STAGING,
        .generation = 1U,
        .valid = true,
    };
    bool release_uart = false;

    state.generation = UINT32_MAX - 1U;
    TEST_ASSERT_TRUE(fw_operation_state_try_begin(
        &state, FW_OPERATION_OWNER_SCANNER_STAGING, &final));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, final.generation);
    TEST_ASSERT_TRUE(fw_operation_state_end(&state, final, &release_uart));

    TEST_ASSERT_FALSE(fw_operation_state_try_begin(
        &state, FW_OPERATION_OWNER_SCANNER_STAGING, &reused));
    TEST_ASSERT_FALSE(reused.valid);
    TEST_ASSERT_FALSE(state.active);
}

void test_fw_operation_runtime_startup_and_upload_order_atomically(void)
{
    fw_operation_state_t state = clean_state();
    fw_operation_token_t startup = {0};
    fw_operation_token_t upload = {0};
    bool release_uart = true;

    TEST_ASSERT_TRUE(fw_operation_state_try_begin(
        &state, FW_OPERATION_OWNER_RUNTIME_STARTUP, &startup));
    TEST_ASSERT_FALSE(fw_operation_state_try_begin(
        &state, FW_OPERATION_OWNER_SCANNER_STAGING, &upload));
    TEST_ASSERT_FALSE(fw_operation_state_try_begin(
        &state, FW_OPERATION_OWNER_UPLINK_OTA, &upload));
    TEST_ASSERT_TRUE(fw_operation_state_end(
        &state, startup, &release_uart));
    TEST_ASSERT_FALSE(release_uart);
    TEST_ASSERT_TRUE(fw_operation_state_try_begin(
        &state, FW_OPERATION_OWNER_SCANNER_STAGING, &upload));

    state = clean_state();
    TEST_ASSERT_TRUE(fw_operation_state_try_begin(
        &state, FW_OPERATION_OWNER_SCANNER_STAGING, &upload));
    TEST_ASSERT_FALSE(fw_operation_state_try_begin(
        &state, FW_OPERATION_OWNER_RUNTIME_STARTUP, &startup));
    TEST_ASSERT_TRUE(fw_operation_state_end(&state, upload, &release_uart));
    TEST_ASSERT_TRUE(fw_operation_state_try_begin(
        &state, FW_OPERATION_OWNER_RUNTIME_STARTUP, &startup));
}

void test_fw_operation_radio_inhibit_tracks_every_begin_end_edge(void)
{
    fw_operation_state_t state = clean_state();
    fw_operation_snapshot_t snapshot = {0};
    fw_operation_token_t token = {0};
    fw_operation_token_t rejected = {0};
    bool release_uart = true;

    fw_operation_state_snapshot(&state, &snapshot);
    TEST_ASSERT_FALSE(snapshot.active);
    TEST_ASSERT_EQUAL(FW_OPERATION_OWNER_NONE, snapshot.owner);
    TEST_ASSERT_EQUAL_UINT32(0U, snapshot.operation_epoch);
    TEST_ASSERT_FALSE(snapshot.radio_inhibited);

    TEST_ASSERT_TRUE(fw_operation_state_try_begin(
        &state, FW_OPERATION_OWNER_SCANNER_STAGING, &token));
    fw_operation_state_snapshot(&state, &snapshot);
    TEST_ASSERT_TRUE(snapshot.active);
    TEST_ASSERT_EQUAL(FW_OPERATION_OWNER_SCANNER_STAGING, snapshot.owner);
    TEST_ASSERT_EQUAL_UINT32(1U, snapshot.operation_epoch);
    TEST_ASSERT_TRUE(snapshot.radio_inhibited);

    TEST_ASSERT_FALSE(fw_operation_state_try_begin(
        &state, FW_OPERATION_OWNER_SCANNER_RELAY, &rejected));
    fw_operation_state_snapshot(&state, &snapshot);
    TEST_ASSERT_EQUAL_UINT32(1U, snapshot.operation_epoch);
    TEST_ASSERT_TRUE(snapshot.radio_inhibited);

    TEST_ASSERT_TRUE(fw_operation_state_end(&state, token, &release_uart));
    TEST_ASSERT_FALSE(release_uart);
    fw_operation_state_snapshot(&state, &snapshot);
    TEST_ASSERT_FALSE(snapshot.active);
    TEST_ASSERT_EQUAL(FW_OPERATION_OWNER_NONE, snapshot.owner);
    TEST_ASSERT_EQUAL_UINT32(2U, snapshot.operation_epoch);
    TEST_ASSERT_TRUE(snapshot.radio_inhibited);

    TEST_ASSERT_TRUE(fw_operation_state_clear_radio_inhibit(&state));
    fw_operation_state_snapshot(&state, &snapshot);
    TEST_ASSERT_EQUAL_UINT32(3U, snapshot.operation_epoch);
    TEST_ASSERT_FALSE(snapshot.radio_inhibited);
    TEST_ASSERT_FALSE(fw_operation_state_clear_radio_inhibit(&state));
    TEST_ASSERT_EQUAL_UINT32(3U, state.operation_epoch);
}

void test_fw_operation_radio_inhibit_is_fail_busy_and_wrap_safe(void)
{
    fw_operation_state_t state = clean_state();
    fw_operation_snapshot_t snapshot = {0};
    fw_operation_token_t token = {0};

    TEST_ASSERT_TRUE(fw_operation_state_request_radio_inhibit(&state));
    fw_operation_state_snapshot(&state, &snapshot);
    TEST_ASSERT_EQUAL_UINT32(1U, snapshot.operation_epoch);
    TEST_ASSERT_TRUE(snapshot.radio_inhibited);
    TEST_ASSERT_FALSE(fw_operation_state_request_radio_inhibit(&state));
    TEST_ASSERT_EQUAL_UINT32(1U, state.operation_epoch);

    TEST_ASSERT_TRUE(fw_operation_state_try_begin(
        &state, FW_OPERATION_OWNER_UPLINK_OTA, &token));
    TEST_ASSERT_EQUAL_UINT32(2U, state.operation_epoch);
    TEST_ASSERT_FALSE(fw_operation_state_clear_radio_inhibit(&state));
    TEST_ASSERT_EQUAL_UINT32(2U, state.operation_epoch);

    state.operation_epoch = UINT32_MAX;
    TEST_ASSERT_TRUE(fw_operation_state_end(&state, token, NULL));
    fw_operation_state_snapshot(&state, &snapshot);
    TEST_ASSERT_EQUAL_UINT32(1U, snapshot.operation_epoch);
    TEST_ASSERT_TRUE(snapshot.radio_inhibited);
    TEST_ASSERT_TRUE(fw_operation_state_clear_radio_inhibit(&state));
    TEST_ASSERT_EQUAL_UINT32(2U, state.operation_epoch);
}

void test_fw_operation_quiesced_claim_requires_exact_inhibit_epoch(void)
{
    fw_operation_state_t state = clean_state();
    fw_operation_token_t token = {0};

    TEST_ASSERT_TRUE(fw_operation_state_request_radio_inhibit(&state));
    uint32_t inhibit_epoch = state.operation_epoch;
    TEST_ASSERT_FALSE(fw_operation_state_try_begin_quiesced(
        &state, FW_OPERATION_OWNER_SCANNER_STAGING,
        inhibit_epoch - 1U, &token));
    TEST_ASSERT_FALSE(token.valid);
    TEST_ASSERT_FALSE(state.active);

    TEST_ASSERT_TRUE(fw_operation_state_try_begin_quiesced(
        &state, FW_OPERATION_OWNER_SCANNER_STAGING,
        inhibit_epoch, &token));
    TEST_ASSERT_TRUE(token.valid);
    TEST_ASSERT_TRUE(state.active);
    TEST_ASSERT_EQUAL_UINT32(inhibit_epoch + 1U, state.operation_epoch);

    TEST_ASSERT_TRUE(fw_operation_state_end(&state, token, NULL));
    TEST_ASSERT_FALSE(fw_operation_state_try_begin_quiesced(
        &state, FW_OPERATION_OWNER_SCANNER_RELAY,
        inhibit_epoch, &token));

    state = clean_state();
    TEST_ASSERT_FALSE(fw_operation_state_try_begin_quiesced(
        &state, FW_OPERATION_OWNER_UPLINK_OTA, 0U, &token));
}

void test_fw_operation_preemption_latch_closes_the_no_owner_race(void)
{
    fw_operation_state_t state = clean_state();
    fw_operation_token_t active = {0};
    fw_operation_token_t rejected = {0};
    fw_operation_snapshot_t snapshot = {0};

    TEST_ASSERT_TRUE(fw_operation_state_try_begin(
        &state, FW_OPERATION_OWNER_SCANNER_RELAY, &active));
    TEST_ASSERT_TRUE(fw_operation_state_request_preemption(&state));
    fw_operation_state_snapshot(&state, &snapshot);
    TEST_ASSERT_TRUE(snapshot.active);
    TEST_ASSERT_TRUE(snapshot.preemption_requested);
    TEST_ASSERT_TRUE(snapshot.radio_inhibited);

    TEST_ASSERT_TRUE(fw_operation_state_end(&state, active, NULL));
    TEST_ASSERT_FALSE(fw_operation_state_try_begin(
        &state, FW_OPERATION_OWNER_SCANNER_RELAY, &rejected));
    fw_operation_state_snapshot(&state, &snapshot);
    TEST_ASSERT_FALSE(snapshot.active);
    TEST_ASSERT_TRUE(snapshot.preemption_requested);
    TEST_ASSERT_TRUE(snapshot.radio_inhibited);
    TEST_ASSERT_FALSE(fw_operation_state_clear_radio_inhibit(&state));
}

void test_fw_operation_preemption_and_recovery_restart_are_exclusive(void)
{
    fw_operation_state_t recovery_first = clean_state();
    fw_operation_snapshot_t snapshot = {0};

    TEST_ASSERT_TRUE(fw_operation_state_try_reserve_recovery_restart(
        &recovery_first));
    TEST_ASSERT_FALSE(fw_operation_state_request_preemption(
        &recovery_first));
    fw_operation_state_snapshot(&recovery_first, &snapshot);
    TEST_ASSERT_TRUE(snapshot.recovery_restart_reserved);
    TEST_ASSERT_FALSE(snapshot.preemption_requested);
    TEST_ASSERT_FALSE(snapshot.radio_inhibited);

    fw_operation_state_t update_first = clean_state();
    TEST_ASSERT_TRUE(fw_operation_state_request_preemption(&update_first));
    TEST_ASSERT_FALSE(fw_operation_state_try_reserve_recovery_restart(
        &update_first));
    fw_operation_state_snapshot(&update_first, &snapshot);
    TEST_ASSERT_FALSE(snapshot.recovery_restart_reserved);
    TEST_ASSERT_TRUE(snapshot.preemption_requested);
    TEST_ASSERT_TRUE(snapshot.radio_inhibited);
}
