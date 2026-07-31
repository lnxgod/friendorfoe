#include "unity.h"

#include "fw_store.h"

static fw_store_campaign_completion_view_t idle_view(void)
{
    fw_store_campaign_completion_view_t view = {
        .operation_active = false,
        .operation_owner = FW_OPERATION_OWNER_NONE,
        .coordinator_sampled = true,
        .persistence_certain = true,
        .campaign_loaded = false,
        .campaign_valid = true,
        .campaign_fail_closed = false,
        .target_slot_mask = 0U,
        .slot_state = {
            FOF_AUTO_SLOT_EXCLUDED,
            FOF_AUTO_SLOT_EXCLUDED,
        },
    };
    return view;
}

void test_fw_campaign_completion_operation_ownership_is_always_pending(void)
{
    fw_store_campaign_completion_view_t view = idle_view();
    view.coordinator_sampled = false;
    view.persistence_certain = false;
    view.operation_active = true;
    TEST_ASSERT_EQUAL_INT(
        FW_CAMPAIGN_COMPLETION_PENDING,
        fw_store_campaign_completion_classify(&view));

    view.operation_active = false;
    view.operation_owner = FW_OPERATION_OWNER_SCANNER_STAGING;
    TEST_ASSERT_EQUAL_INT(
        FW_CAMPAIGN_COMPLETION_PENDING,
        fw_store_campaign_completion_classify(&view));
}

void test_fw_campaign_completion_ambiguity_is_unknown(void)
{
    fw_store_campaign_completion_view_t view = idle_view();
    TEST_ASSERT_EQUAL_INT(
        FW_CAMPAIGN_COMPLETION_UNKNOWN,
        fw_store_campaign_completion_classify(NULL));

    view.coordinator_sampled = false;
    TEST_ASSERT_EQUAL_INT(
        FW_CAMPAIGN_COMPLETION_UNKNOWN,
        fw_store_campaign_completion_classify(&view));

    view.coordinator_sampled = true;
    view.persistence_certain = false;
    TEST_ASSERT_EQUAL_INT(
        FW_CAMPAIGN_COMPLETION_UNKNOWN,
        fw_store_campaign_completion_classify(&view));

    view.persistence_certain = true;
    view.campaign_loaded = true;
    view.campaign_valid = false;
    TEST_ASSERT_EQUAL_INT(
        FW_CAMPAIGN_COMPLETION_UNKNOWN,
        fw_store_campaign_completion_classify(&view));
}

void test_fw_campaign_completion_no_scanner_campaign_is_success_only_when_idle(void)
{
    fw_store_campaign_completion_view_t view = idle_view();
    TEST_ASSERT_EQUAL_INT(
        FW_CAMPAIGN_COMPLETION_SUCCESS,
        fw_store_campaign_completion_classify(&view));

    view.operation_owner = FW_OPERATION_OWNER_UPLINK_OTA;
    TEST_ASSERT_EQUAL_INT(
        FW_CAMPAIGN_COMPLETION_PENDING,
        fw_store_campaign_completion_classify(&view));
}

void test_fw_campaign_completion_all_requested_current_or_converged_succeeds(void)
{
    fw_store_campaign_completion_view_t view = idle_view();
    view.campaign_loaded = true;
    view.target_slot_mask = FW_AUTO_UPDATE_SLOT_ALL;
    view.slot_state[0] = FOF_AUTO_SLOT_CONVERGED;
    view.slot_state[1] = FOF_AUTO_SLOT_CURRENT;
    TEST_ASSERT_EQUAL_INT(
        FW_CAMPAIGN_COMPLETION_SUCCESS,
        fw_store_campaign_completion_classify(&view));

    view.target_slot_mask = FW_AUTO_UPDATE_SLOT_BLE;
    view.slot_state[1] = FOF_AUTO_SLOT_EXCLUDED;
    TEST_ASSERT_EQUAL_INT(
        FW_CAMPAIGN_COMPLETION_SUCCESS,
        fw_store_campaign_completion_classify(&view));
}

void test_fw_campaign_completion_terminal_refusal_failure_or_newer_is_failure(void)
{
    const fof_auto_slot_state_t failures[] = {
        FOF_AUTO_SLOT_REFUSED,
        FOF_AUTO_SLOT_FAILED,
        FOF_AUTO_SLOT_NEWER_SKIPPED,
    };
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        fw_store_campaign_completion_view_t view = idle_view();
        view.campaign_loaded = true;
        view.target_slot_mask = FW_AUTO_UPDATE_SLOT_BLE;
        view.slot_state[0] = failures[i];
        TEST_ASSERT_EQUAL_INT(
            FW_CAMPAIGN_COMPLETION_TERMINAL_FAILURE,
            fw_store_campaign_completion_classify(&view));
    }

    fw_store_campaign_completion_view_t mixed = idle_view();
    mixed.campaign_loaded = true;
    mixed.target_slot_mask = FW_AUTO_UPDATE_SLOT_ALL;
    mixed.slot_state[0] = FOF_AUTO_SLOT_CURRENT;
    mixed.slot_state[1] = FOF_AUTO_SLOT_FAILED;
    TEST_ASSERT_EQUAL_INT(
        FW_CAMPAIGN_COMPLETION_TERMINAL_FAILURE,
        fw_store_campaign_completion_classify(&mixed));

    fw_store_campaign_completion_view_t fail_closed = idle_view();
    fail_closed.campaign_loaded = true;
    fail_closed.campaign_fail_closed = true;
    TEST_ASSERT_EQUAL_INT(
        FW_CAMPAIGN_COMPLETION_TERMINAL_FAILURE,
        fw_store_campaign_completion_classify(&fail_closed));
}

void test_fw_campaign_completion_nonterminal_or_invalid_campaign_fails_closed(void)
{
    const fof_auto_slot_state_t pending[] = {
        FOF_AUTO_SLOT_AWAITING_CHECK,
        FOF_AUTO_SLOT_OFFERED,
        FOF_AUTO_SLOT_READY_QUEUED,
        FOF_AUTO_SLOT_RELAYING,
        FOF_AUTO_SLOT_RECOVERING,
    };
    for (size_t i = 0; i < sizeof(pending) / sizeof(pending[0]); ++i) {
        fw_store_campaign_completion_view_t view = idle_view();
        view.campaign_loaded = true;
        view.target_slot_mask = FW_AUTO_UPDATE_SLOT_BLE;
        view.slot_state[0] = pending[i];
        TEST_ASSERT_EQUAL_INT(
            FW_CAMPAIGN_COMPLETION_PENDING,
            fw_store_campaign_completion_classify(&view));
    }

    fw_store_campaign_completion_view_t mixed = idle_view();
    mixed.campaign_loaded = true;
    mixed.target_slot_mask = FW_AUTO_UPDATE_SLOT_ALL;
    mixed.slot_state[0] = FOF_AUTO_SLOT_FAILED;
    mixed.slot_state[1] = FOF_AUTO_SLOT_RECOVERING;
    TEST_ASSERT_EQUAL_INT(
        FW_CAMPAIGN_COMPLETION_PENDING,
        fw_store_campaign_completion_classify(&mixed));

    fw_store_campaign_completion_view_t invalid = idle_view();
    invalid.campaign_loaded = true;
    invalid.target_slot_mask = FW_AUTO_UPDATE_SLOT_BLE;
    invalid.slot_state[0] = FOF_AUTO_SLOT_EXCLUDED;
    TEST_ASSERT_EQUAL_INT(
        FW_CAMPAIGN_COMPLETION_UNKNOWN,
        fw_store_campaign_completion_classify(&invalid));

    invalid.target_slot_mask = (uint8_t)(FW_AUTO_UPDATE_SLOT_ALL | 0x80U);
    invalid.slot_state[0] = FOF_AUTO_SLOT_CURRENT;
    TEST_ASSERT_EQUAL_INT(
        FW_CAMPAIGN_COMPLETION_UNKNOWN,
        fw_store_campaign_completion_classify(&invalid));
}

void test_fw_campaign_terminal_exit_requires_a_newly_staged_generation(void)
{
    fw_store_scanner_stage_status_t stage = {
        .phase = FW_SCANNER_STAGE_COMMITTED,
        .generation = 41U,
        .size = 1024U,
        .received = 1024U,
        .slot_mask = FW_AUTO_UPDATE_SLOT_ALL,
        .target = "scanner-s3-combo-fof_badge",
        .sha256 =
            "0123456789abcdef0123456789abcdef"
            "0123456789abcdef0123456789abcdef",
    };

    TEST_ASSERT_FALSE(
        fw_store_campaign_terminal_exit_allowed(
            true, 41U, true, &stage,
            FW_CAMPAIGN_COMPLETION_TERMINAL_FAILURE));

    stage.generation = 42U;
    TEST_ASSERT_TRUE(
        fw_store_campaign_terminal_exit_allowed(
            true, 41U, true, &stage,
            FW_CAMPAIGN_COMPLETION_TERMINAL_FAILURE));

    stage.phase = FW_SCANNER_STAGE_RECEIVING;
    stage.generation = 0U;
    stage.received = 512U;
    TEST_ASSERT_FALSE(
        fw_store_campaign_terminal_exit_allowed(
            true, 41U, true, &stage,
            FW_CAMPAIGN_COMPLETION_TERMINAL_FAILURE));

    TEST_ASSERT_FALSE(
        fw_store_campaign_terminal_exit_allowed(
            false, 0U, true, &stage,
            FW_CAMPAIGN_COMPLETION_TERMINAL_FAILURE));
}
