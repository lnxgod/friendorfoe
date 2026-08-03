#include <string.h>

#include <unity.h>

#include "backend_hardware_profile.h"
#include "backend_test_main.h"

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
#include "backend_ota_workflow.h"

static backend_ota_command_envelope_t probe_command(void)
{
    backend_ota_command_envelope_t command;
    memset(&command, 0, sizeof(command));
    command.has_operation_id = true;
    TEST_ASSERT_TRUE(backend_ota_operation_id_decode(
        "0123456789abcdef0123456789abcdef", &command.operation_id));
    command.component = BACKEND_OTA_COMPONENT_SCANNER0;
    strcpy(command.catalog_name, FOF_BACKEND_SCANNER_TARGET);
    strcpy(command.expected_sha256,
           "0123456789abcdef0123456789abcdef"
           "0123456789abcdef0123456789abcdef");
    command.expected_size = 1048576U;
    const uint8_t uplink[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x00};
    const uint8_t scanner[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};
    memcpy(command.binding.uplink_mac, uplink, 6U);
    command.binding.uplink_boot_id = 101U;
    memcpy(command.binding.target_mac, scanner, 6U);
    command.binding.target_boot_id = 202U;
    command.binding.topology_generation = 7U;
    command.apply_mode = BACKEND_OTA_NEWER_ONLY;
    return command;
}

static backend_ota_command_ack_t ack_for(
    const backend_ota_command_envelope_t *command,
    uint32_t sequence,
    backend_ota_component_t component,
    const char *action,
    bool terminal)
{
    backend_ota_command_ack_t ack = {
        .ok = true,
        .has_operation_id = true,
        .operation_id = command->operation_id,
        .accepted_sequence = sequence,
        .next_sequence = sequence + 1U,
        .current_component = component,
        .terminal = terminal,
    };
    strcpy(ack.current_action, action);
    return ack;
}

static backend_ota_progress_event_t progress_for(
    const backend_ota_command_envelope_t *command,
    uint32_t sequence,
    backend_ota_progress_stage_t stage,
    uint32_t received,
    uint32_t retry_count)
{
    return (backend_ota_progress_event_t) {
        .prefix = {
            .has_operation_id = true,
            .operation_id = command->operation_id,
            .is_apply = command->is_apply,
            .sequence = sequence,
            .component = command->component,
            .catalog_name = command->catalog_name,
        },
        .stage = stage,
        .received = received,
        .total = command->expected_size,
        .retry_count = retry_count,
    };
}

static void test_duplicate_and_ack_order_gate_scanner_progression(void)
{
    backend_ota_workflow_t workflow;
    backend_ota_workflow_init(&workflow);
    backend_ota_command_envelope_t probe = probe_command();
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_WORKFLOW_ADMITTED,
        backend_ota_workflow_admit(&workflow, &probe));
    backend_ota_command_envelope_t work;
    TEST_ASSERT_TRUE(backend_ota_workflow_take_work(&workflow, &work));
    TEST_ASSERT_EQUAL_MEMORY(&probe, &work, sizeof(work));
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_WORKFLOW_DUPLICATE,
        backend_ota_workflow_admit(&workflow, &probe));
    backend_ota_command_envelope_t drift = probe;
    drift.expected_size++;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_WORKFLOW_BUSY,
        backend_ota_workflow_admit(&workflow, &drift));

    backend_ota_command_ack_t begin = ack_for(
        &probe, 0U, BACKEND_OTA_COMPONENT_SCANNER0, "probe", false);
    TEST_ASSERT_TRUE(backend_ota_workflow_note_begin_ack(&workflow, &begin));
    backend_ota_workflow_ack_prediction_t prediction;
    TEST_ASSERT_TRUE(backend_ota_workflow_predict_terminal_ack(
        &workflow, BACKEND_OTA_TERMINAL_ELIGIBLE, &prediction));
    TEST_ASSERT_EQUAL(BACKEND_OTA_COMPONENT_SCANNER0, prediction.component);
    TEST_ASSERT_TRUE(prediction.is_apply);
    TEST_ASSERT_FALSE(prediction.terminal);

    const char receipt[] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    backend_ota_command_ack_t eligible = ack_for(
        &probe, 1U, BACKEND_OTA_COMPONENT_SCANNER0, "apply", false);
    TEST_ASSERT_TRUE(backend_ota_workflow_note_terminal_ack(
        &workflow, BACKEND_OTA_TERMINAL_ELIGIBLE, receipt, &eligible));
    TEST_ASSERT_TRUE(workflow.has_accepted_probe);

    backend_ota_command_envelope_t apply = probe;
    apply.is_apply = true;
    apply.next_sequence = 2U;
    strcpy(apply.probe_receipt_sha256, receipt);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_WORKFLOW_ADMITTED,
        backend_ota_workflow_admit(&workflow, &apply));
    TEST_ASSERT_TRUE(backend_ota_workflow_take_work(&workflow, &work));
    begin = ack_for(
        &apply, 2U, BACKEND_OTA_COMPONENT_SCANNER0, "apply", false);
    TEST_ASSERT_TRUE(backend_ota_workflow_note_begin_ack(&workflow, &begin));
    backend_ota_command_ack_t applied = ack_for(
        &apply, 3U, BACKEND_OTA_COMPONENT_SCANNER1, "probe", false);
    TEST_ASSERT_TRUE(backend_ota_workflow_note_terminal_ack(
        &workflow, BACKEND_OTA_TERMINAL_APPLIED, NULL, &applied));
    TEST_ASSERT_EQUAL_HEX8(1U, workflow.completed_scanner_mask);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_COMPONENT_SCANNER1, workflow.expected_component);
    TEST_ASSERT_FALSE(workflow.expected_apply);
    TEST_ASSERT_TRUE(workflow.has_expected_sequence);
    TEST_ASSERT_EQUAL_UINT32(4U, workflow.expected_sequence);
}

static void test_progress_ack_advances_only_monotonic_current_event(void)
{
    backend_ota_workflow_t workflow;
    backend_ota_workflow_init(&workflow);
    backend_ota_command_envelope_t probe = probe_command();
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_WORKFLOW_ADMITTED,
        backend_ota_workflow_admit(&workflow, &probe));

    backend_ota_command_ack_t begin = ack_for(
        &probe, 0U, BACKEND_OTA_COMPONENT_SCANNER0, "probe", false);
    TEST_ASSERT_TRUE(backend_ota_workflow_note_begin_ack(&workflow, &begin));

    backend_ota_workflow_ack_prediction_t prediction;
    TEST_ASSERT_TRUE(backend_ota_workflow_predict_progress_ack(
        &workflow, &prediction));
    TEST_ASSERT_EQUAL(BACKEND_OTA_COMPONENT_SCANNER0, prediction.component);
    TEST_ASSERT_FALSE(prediction.is_apply);
    TEST_ASSERT_FALSE(prediction.terminal);

    backend_ota_progress_event_t metadata = progress_for(
        &probe, 1U, BACKEND_OTA_PROGRESS_METADATA, 0U, 0U);
    backend_ota_command_ack_t progress_ack = ack_for(
        &probe, 1U, BACKEND_OTA_COMPONENT_SCANNER0, "probe", false);
    TEST_ASSERT_TRUE(backend_ota_workflow_note_progress_ack(
        &workflow, &metadata, &progress_ack));
    TEST_ASSERT_EQUAL_UINT32(2U, workflow.expected_sequence);
    TEST_ASSERT_TRUE(workflow.progress.initialized);
    TEST_ASSERT_EQUAL(BACKEND_OTA_PROGRESS_METADATA, workflow.progress.stage);

    backend_ota_progress_event_t download = progress_for(
        &probe, 2U, BACKEND_OTA_PROGRESS_DOWNLOAD, 4096U, 1U);
    backend_ota_command_ack_t stale_ack = ack_for(
        &probe, 1U, BACKEND_OTA_COMPONENT_SCANNER0, "probe", false);
    TEST_ASSERT_FALSE(backend_ota_workflow_note_progress_ack(
        &workflow, &download, &stale_ack));
    TEST_ASSERT_EQUAL_UINT32(2U, workflow.expected_sequence);
    TEST_ASSERT_EQUAL(BACKEND_OTA_PROGRESS_METADATA, workflow.progress.stage);

    backend_ota_progress_event_t regression = progress_for(
        &probe, 2U, BACKEND_OTA_PROGRESS_METADATA, 0U, 0U);
    regression.total--;
    progress_ack = ack_for(
        &probe, 2U, BACKEND_OTA_COMPONENT_SCANNER0, "probe", false);
    TEST_ASSERT_FALSE(backend_ota_workflow_note_progress_ack(
        &workflow, &regression, &progress_ack));
    TEST_ASSERT_EQUAL_UINT32(2U, workflow.expected_sequence);
    TEST_ASSERT_EQUAL_UINT32(probe.expected_size, workflow.progress.total);

    TEST_ASSERT_TRUE(backend_ota_workflow_note_progress_ack(
        &workflow, &download, &progress_ack));
    TEST_ASSERT_EQUAL_UINT32(3U, workflow.expected_sequence);
    TEST_ASSERT_EQUAL(BACKEND_OTA_PROGRESS_DOWNLOAD, workflow.progress.stage);
    TEST_ASSERT_EQUAL_UINT32(4096U, workflow.progress.received);
    TEST_ASSERT_EQUAL_UINT32(1U, workflow.progress.retry_count);

    const char receipt[] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    backend_ota_command_ack_t terminal = ack_for(
        &probe, 3U, BACKEND_OTA_COMPONENT_SCANNER0, "apply", false);
    TEST_ASSERT_TRUE(backend_ota_workflow_note_terminal_ack(
        &workflow, BACKEND_OTA_TERMINAL_ELIGIBLE, receipt, &terminal));
    TEST_ASSERT_TRUE(workflow.has_expected_sequence);
    TEST_ASSERT_EQUAL_UINT32(4U, workflow.expected_sequence);
    TEST_ASSERT_TRUE(workflow.has_accepted_probe);
}

static void test_terminal_rollout_clears_cursor_for_next_operation(void)
{
    backend_ota_workflow_t workflow;
    backend_ota_workflow_init(&workflow);
    backend_ota_command_envelope_t uplink = probe_command();
    uplink.component = BACKEND_OTA_COMPONENT_UPLINK;
    strcpy(uplink.catalog_name, FOF_BACKEND_UPLINK_TARGET);
    memcpy(uplink.binding.target_mac, uplink.binding.uplink_mac, 6U);
    uplink.binding.target_boot_id = uplink.binding.uplink_boot_id;
    uplink.next_sequence = 10U;
    workflow.has_rollout_operation = true;
    workflow.rollout_operation_id = uplink.operation_id;
    workflow.expected_component = BACKEND_OTA_COMPONENT_UPLINK;
    workflow.has_expected_sequence = true;
    workflow.expected_sequence = 10U;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_WORKFLOW_ADMITTED,
        backend_ota_workflow_admit(&workflow, &uplink));

    backend_ota_command_ack_t begin = ack_for(
        &uplink, 10U, BACKEND_OTA_COMPONENT_UPLINK, "probe", false);
    TEST_ASSERT_TRUE(backend_ota_workflow_note_begin_ack(&workflow, &begin));
    backend_ota_command_ack_t terminal = ack_for(
        &uplink, 11U, BACKEND_OTA_COMPONENT_UPLINK, "probe", true);
    TEST_ASSERT_TRUE(backend_ota_workflow_note_terminal_ack(
        &workflow, BACKEND_OTA_TERMINAL_NO_UPDATE, NULL, &terminal));
    TEST_ASSERT_FALSE(workflow.has_rollout_operation);
    TEST_ASSERT_FALSE(workflow.has_expected_sequence);
    TEST_ASSERT_EQUAL_UINT32(0U, workflow.expected_sequence);

    backend_ota_command_envelope_t next = probe_command();
    TEST_ASSERT_TRUE(backend_ota_operation_id_decode(
        "fedcba9876543210fedcba9876543210", &next.operation_id));
    TEST_ASSERT_EQUAL_UINT32(0U, next.next_sequence);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_WORKFLOW_ADMITTED,
        backend_ota_workflow_admit(&workflow, &next));
}
#else
static void test_lite_build_does_not_link_fullsize_workflow(void)
{
    TEST_PASS();
}
#endif

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    BACKEND_RUN_TEST(test_duplicate_and_ack_order_gate_scanner_progression);
    BACKEND_RUN_TEST(
        test_progress_ack_advances_only_monotonic_current_event);
    BACKEND_RUN_TEST(test_terminal_rollout_clears_cursor_for_next_operation);
#else
    BACKEND_RUN_TEST(test_lite_build_does_not_link_fullsize_workflow);
#endif
    return UNITY_END();
}
