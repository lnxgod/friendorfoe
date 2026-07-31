#include "unity.h"

#include "badge_usb_uplink_ota.h"

#include <string.h>

static uplink_usb_ota_result_t adapter_result(
    bool ok, bool retryable, uplink_usb_ota_phase_t phase,
    uint32_t received, uint32_t total, uint32_t credit)
{
    uplink_usb_ota_result_t result = {
        .ok = ok,
        .retryable = retryable,
        .emit_required = phase != UPLINK_USB_OTA_PHASE_PROGRESS,
        .reboot_required = phase == UPLINK_USB_OTA_PHASE_COMMITTED,
        .phase = phase,
        .received = received,
        .total = total,
        .credit_bytes = credit,
    };
    strcpy(result.partition, "ota_1");
    if (!ok) {
        strcpy(result.error, retryable ? "adapter_busy" : "write_failed");
    }
    return result;
}

static badge_usb_uplink_ota_flow_t ready_flow(uint32_t total)
{
    badge_usb_uplink_ota_flow_t flow;
    badge_usb_uplink_ota_flow_init(&flow);
    uint32_t first_credit = total < UPLINK_OTA_CREDIT_BYTES
        ? total : UPLINK_OTA_CREDIT_BYTES;
    uplink_usb_ota_result_t ready = adapter_result(
        true, false, UPLINK_USB_OTA_PHASE_READY,
        0U, total, first_credit);
    TEST_ASSERT_EQUAL(
        BADGE_USB_UPLINK_ACTION_WAIT_RECEIPT,
        badge_usb_uplink_ota_flow_begin_result(&flow, &ready));
    TEST_ASSERT_EQUAL(
        BADGE_USB_UPLINK_ACTION_CONTINUE,
        badge_usb_uplink_ota_flow_receipt_result(&flow, true));
    return flow;
}

static badge_usb_uplink_action_t accept_write(
    badge_usb_uplink_ota_flow_t *flow, uint32_t bytes,
    uplink_usb_ota_phase_t phase, uint32_t next_credit)
{
    uplink_usb_ota_result_t result = adapter_result(
        true, false, phase,
        flow->transport_received + bytes,
        flow->total,
        next_credit);
    return badge_usb_uplink_ota_flow_write_result(
        flow, bytes, true, &result);
}

void test_badge_usb_uplink_manifest_fields_are_exact_and_bounded(void)
{
    badge_usb_uplink_manifest_fields_t fields = {
        .target = UPLINK_OTA_TARGET,
        .project = UPLINK_OTA_PROJECT,
        .hardware = UPLINK_OTA_HARDWARE,
        .version = "0.64.69",
        .sha256 = "1111111111111111111111111111111111111111111111111111111111111111",
        .flow_control = "credit-v1",
        .size = 5000U,
        .crc32 = 0x12345678U,
        .recovery_rewrite_same_version = false,
    };
    uplink_ota_manifest_t manifest = {0};
    const char *error = NULL;

    TEST_ASSERT_TRUE(badge_usb_uplink_ota_manifest_from_fields(
        &fields, &manifest, &error));
    TEST_ASSERT_EQUAL_STRING(UPLINK_OTA_TARGET, manifest.target);
    TEST_ASSERT_EQUAL_STRING(UPLINK_OTA_PROJECT, manifest.project);
    TEST_ASSERT_EQUAL_STRING(UPLINK_OTA_HARDWARE, manifest.hardware);
    TEST_ASSERT_EQUAL_STRING("0.64.69", manifest.version);
    TEST_ASSERT_EQUAL_UINT32(5000U, manifest.size);
    TEST_ASSERT_EQUAL_UINT32(0x12345678U, manifest.crc32);
    TEST_ASSERT_FALSE(manifest.recovery_rewrite_same_version);

    fields.flow_control = "credit-v2";
    TEST_ASSERT_FALSE(badge_usb_uplink_ota_manifest_from_fields(
        &fields, &manifest, &error));
    TEST_ASSERT_EQUAL_STRING("invalid_flow_control", error);
    fields.flow_control = "credit-v1";
    fields.target = "scanner-s3-combo-fof_badge";
    TEST_ASSERT_FALSE(badge_usb_uplink_ota_manifest_from_fields(
        &fields, &manifest, &error));
    TEST_ASSERT_EQUAL_STRING("invalid_target", error);
    fields.target = UPLINK_OTA_TARGET;
    fields.crc32 = 0U;
    TEST_ASSERT_FALSE(badge_usb_uplink_ota_manifest_from_fields(
        &fields, &manifest, &error));
    TEST_ASSERT_EQUAL_STRING("invalid_crc", error);
}

void test_badge_usb_uplink_credit_4095_plus_1_and_final_short_window(void)
{
    badge_usb_uplink_ota_flow_t flow = ready_flow(5000U);
    size_t allowed = 0U;

    for (unsigned i = 0U; i < 7U; ++i) {
        TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_CONTINUE,
                          badge_usb_uplink_ota_flow_plan_read(
                              &flow, 512U, &allowed));
        TEST_ASSERT_EQUAL_UINT(512U, allowed);
        TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_CONTINUE,
                          accept_write(&flow, 512U,
                                       UPLINK_USB_OTA_PHASE_PROGRESS, 0U));
    }
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_CONTINUE,
                      badge_usb_uplink_ota_flow_plan_read(&flow, 511U, &allowed));
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_CONTINUE,
                      accept_write(&flow, 511U,
                                   UPLINK_USB_OTA_PHASE_PROGRESS, 0U));
    TEST_ASSERT_EQUAL_UINT32(4095U, flow.transport_received);
    TEST_ASSERT_EQUAL_UINT32(1U, flow.credit_remaining);

    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_CONTINUE,
                      badge_usb_uplink_ota_flow_plan_read(&flow, 1U, &allowed));
    TEST_ASSERT_EQUAL_UINT(1U, allowed);
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_WAIT_RECEIPT,
                      accept_write(&flow, 1U, UPLINK_USB_OTA_PHASE_CREDIT,
                                   904U));
    TEST_ASSERT_EQUAL_UINT32(0U, flow.credit_remaining);
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_WAIT_RECEIPT,
                      badge_usb_uplink_ota_flow_plan_read(&flow, 1U, &allowed));
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_CONTINUE,
                      badge_usb_uplink_ota_flow_receipt_result(&flow, true));
    TEST_ASSERT_EQUAL_UINT32(904U, flow.credit_remaining);

    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_CONTINUE,
                      accept_write(&flow, 512U,
                                   UPLINK_USB_OTA_PHASE_PROGRESS, 0U));
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_FINISH,
                      accept_write(&flow, 392U,
                                   UPLINK_USB_OTA_PHASE_PROGRESS, 0U));
    TEST_ASSERT_EQUAL_UINT32(5000U, flow.transport_received);
    TEST_ASSERT_TRUE(badge_usb_uplink_ota_flow_take_finish(&flow));
    TEST_ASSERT_FALSE(badge_usb_uplink_ota_flow_take_finish(&flow));
}

void test_badge_usb_uplink_exact_credit_multiple_finishes_without_zero_credit(void)
{
    badge_usb_uplink_ota_flow_t flow = ready_flow(8192U);
    for (unsigned i = 0U; i < 7U; ++i) {
        TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_CONTINUE,
                          accept_write(&flow, 512U,
                                       UPLINK_USB_OTA_PHASE_PROGRESS, 0U));
    }
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_WAIT_RECEIPT,
                      accept_write(&flow, 512U, UPLINK_USB_OTA_PHASE_CREDIT,
                                   4096U));
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_CONTINUE,
                      badge_usb_uplink_ota_flow_receipt_result(&flow, true));
    for (unsigned i = 0U; i < 7U; ++i) {
        TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_CONTINUE,
                          accept_write(&flow, 512U,
                                       UPLINK_USB_OTA_PHASE_PROGRESS, 0U));
    }
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_FINISH,
                      accept_write(&flow, 512U,
                                   UPLINK_USB_OTA_PHASE_PROGRESS, 0U));
    TEST_ASSERT_EQUAL_UINT32(8192U, flow.transport_received);
}

void test_badge_usb_uplink_extra_same_read_and_complete_plus_line_abort_drop(void)
{
    size_t allowed = 99U;
    badge_usb_uplink_ota_flow_t boundary = ready_flow(5000U);
    boundary.transport_received = 4095U;
    boundary.credit_remaining = 1U;
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_ABORT_DROP,
                      badge_usb_uplink_ota_flow_plan_read(
                          &boundary, 2U, &allowed));
    TEST_ASSERT_EQUAL_UINT(0U, allowed);

    badge_usb_uplink_ota_flow_t final = ready_flow(1024U);
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_ABORT_DROP,
                      badge_usb_uplink_ota_flow_plan_read(
                          &final, 1027U, &allowed));
    TEST_ASSERT_EQUAL_UINT(0U, allowed);
}

void test_badge_usb_uplink_retryable_write_retains_exact_pending_chunk(void)
{
    badge_usb_uplink_ota_flow_t flow = ready_flow(5000U);
    uplink_usb_ota_result_t busy = adapter_result(
        false, true, UPLINK_USB_OTA_PHASE_NONE, 0U, 0U, 0U);
    size_t allowed = 0U;
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_CONTINUE,
                      badge_usb_uplink_ota_flow_plan_read(&flow, 512U, &allowed));
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_RETRY_PENDING,
                      badge_usb_uplink_ota_flow_write_result(
                          &flow, 512U, false, &busy));
    TEST_ASSERT_EQUAL_UINT32(0U, flow.transport_received);
    TEST_ASSERT_EQUAL_UINT32(4096U, flow.credit_remaining);
    TEST_ASSERT_EQUAL_UINT32(512U, flow.pending_retry_bytes);
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_RETRY_PENDING,
                      badge_usb_uplink_ota_flow_plan_read(&flow, 1U, &allowed));

    uplink_usb_ota_result_t accepted = adapter_result(
        true, false, UPLINK_USB_OTA_PHASE_PROGRESS, 512U, 5000U, 0U);
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_CONTINUE,
                      badge_usb_uplink_ota_flow_write_result(
                          &flow, 512U, true, &accepted));
    TEST_ASSERT_EQUAL_UINT32(0U, flow.pending_retry_bytes);
    TEST_ASSERT_EQUAL_UINT32(512U, flow.transport_received);
}

void test_badge_usb_uplink_durable_progress_is_monotonic_and_bounded(void)
{
    badge_usb_uplink_ota_flow_t flow = ready_flow(5000U);
    uplink_usb_ota_result_t buffered = adapter_result(
        true, false, UPLINK_USB_OTA_PHASE_PROGRESS, 0U, 5000U, 0U);
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_CONTINUE,
                      badge_usb_uplink_ota_flow_write_result(
                          &flow, 100U, true, &buffered));
    TEST_ASSERT_EQUAL_UINT32(100U, flow.transport_received);
    TEST_ASSERT_EQUAL_UINT32(0U, flow.durable_received);

    uplink_usb_ota_result_t flushed = adapter_result(
        true, false, UPLINK_USB_OTA_PHASE_PROGRESS, 144U, 5000U, 0U);
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_CONTINUE,
                      badge_usb_uplink_ota_flow_write_result(
                          &flow, 44U, true, &flushed));
    TEST_ASSERT_EQUAL_UINT32(144U, flow.durable_received);

    uplink_usb_ota_result_t regressed = adapter_result(
        true, false, UPLINK_USB_OTA_PHASE_PROGRESS, 143U, 5000U, 0U);
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_ABORT_DROP,
                      badge_usb_uplink_ota_flow_write_result(
                          &flow, 1U, true, &regressed));

    flow = ready_flow(5000U);
    uplink_usb_ota_result_t overshot = adapter_result(
        true, false, UPLINK_USB_OTA_PHASE_PROGRESS, 101U, 5000U, 0U);
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_ABORT_DROP,
                      badge_usb_uplink_ota_flow_write_result(
                          &flow, 100U, true, &overshot));
}

void test_badge_usb_uplink_requires_durable_equality_at_credit_and_final(void)
{
    badge_usb_uplink_ota_flow_t credit = ready_flow(4097U);
    credit.transport_received = 4095U;
    credit.durable_received = 4095U;
    credit.credit_remaining = 1U;
    uplink_usb_ota_result_t lagging_credit = adapter_result(
        true, false, UPLINK_USB_OTA_PHASE_CREDIT, 4095U, 4097U, 1U);
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_ABORT_DROP,
                      badge_usb_uplink_ota_flow_write_result(
                          &credit, 1U, true, &lagging_credit));

    badge_usb_uplink_ota_flow_t final = ready_flow(144U);
    uplink_usb_ota_result_t lagging_final = adapter_result(
        true, false, UPLINK_USB_OTA_PHASE_PROGRESS, 0U, 144U, 0U);
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_ABORT_DROP,
                      badge_usb_uplink_ota_flow_write_result(
                          &final, 144U, true, &lagging_final));

    badge_usb_uplink_ota_flow_t commit = ready_flow(144U);
    commit.transport_received = 144U;
    commit.durable_received = 143U;
    commit.credit_remaining = 0U;
    uplink_usb_ota_result_t committed = adapter_result(
        true, false, UPLINK_USB_OTA_PHASE_COMMITTED, 144U, 144U, 0U);
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_ABORT_DROP,
                      badge_usb_uplink_ota_flow_finish_result(
                          &commit, true, &committed));
}

void test_badge_usb_uplink_retry_budget_is_bounded_without_advancement(void)
{
    badge_usb_uplink_ota_flow_t flow = ready_flow(5000U);
    uplink_usb_ota_result_t busy = adapter_result(
        false, true, UPLINK_USB_OTA_PHASE_NONE, 0U, 0U, 0U);
    for (unsigned attempt = 1U; attempt < BADGE_USB_UPLINK_OTA_RETRY_LIMIT;
         ++attempt) {
        TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_RETRY_PENDING,
                          badge_usb_uplink_ota_flow_write_result(
                              &flow, 100U, false, &busy));
        TEST_ASSERT_EQUAL_UINT32(0U, flow.transport_received);
        TEST_ASSERT_EQUAL_UINT32(0U, flow.durable_received);
    }
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_RECOVERY_RESTART,
                      badge_usb_uplink_ota_flow_write_result(
                          &flow, 100U, false, &busy));
    TEST_ASSERT_EQUAL_UINT32(0U, flow.transport_received);
    TEST_ASSERT_EQUAL_UINT32(0U, flow.durable_received);
}

void test_badge_usb_uplink_begin_cleanup_classification_is_exact(void)
{
    uplink_usb_ota_result_t result = adapter_result(
        false, true, UPLINK_USB_OTA_PHASE_NONE, 0U, 0U, 0U);
    strcpy(result.error, "operation_release_failed");
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_RETRY_CLEANUP,
                      badge_usb_uplink_ota_begin_failure_action(&result));

    strcpy(result.error, "operation_active");
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_CONTINUE,
                      badge_usb_uplink_ota_begin_failure_action(&result));
    strcpy(result.error, "adapter_busy");
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_CONTINUE,
                      badge_usb_uplink_ota_begin_failure_action(&result));
}

void test_badge_usb_uplink_terminal_delivery_never_duplicates_ambiguous_output(void)
{
    badge_usb_uplink_ota_flow_t flow = ready_flow(144U);
    (void)badge_usb_uplink_ota_flow_abort(&flow);
    for (unsigned attempt = 1U; attempt < BADGE_USB_UPLINK_OTA_RETRY_LIMIT;
         ++attempt) {
        TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_RETRY_TERMINAL,
                          badge_usb_uplink_ota_flow_terminal_emit_result(
                              &flow, BADGE_USB_EMIT_FAILED));
        TEST_ASSERT_TRUE(flow.terminal_available);
    }
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_RECOVERY_RESTART,
                      badge_usb_uplink_ota_flow_terminal_emit_result(
                          &flow, BADGE_USB_EMIT_FAILED));
    TEST_ASSERT_FALSE(flow.terminal_available);

    flow = ready_flow(144U);
    (void)badge_usb_uplink_ota_flow_abort(&flow);
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_CONTINUE,
                      badge_usb_uplink_ota_flow_terminal_emit_result(
                          &flow, BADGE_USB_EMIT_ENQUEUED));
    TEST_ASSERT_FALSE(flow.terminal_available);

    flow = ready_flow(144U);
    (void)badge_usb_uplink_ota_flow_abort(&flow);
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_RECOVERY_RESTART,
                      badge_usb_uplink_ota_flow_terminal_emit_result(
                          &flow, BADGE_USB_EMIT_POISONED));
    TEST_ASSERT_FALSE(flow.terminal_available);
}

void test_badge_usb_uplink_receipt_policy_distinguishes_safe_and_ambiguous_output(void)
{
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_RECEIPT_ACKNOWLEDGED,
                      badge_usb_uplink_ota_receipt_decide(
                          BADGE_USB_EMIT_COMPLETED, false));
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_RECEIPT_ACKNOWLEDGED,
                      badge_usb_uplink_ota_receipt_decide(
                          BADGE_USB_EMIT_ENQUEUED, true));
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_RECEIPT_CLEANUP_RECOVERY,
                      badge_usb_uplink_ota_receipt_decide(
                          BADGE_USB_EMIT_ENQUEUED, false));
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_RECEIPT_CLEANUP_RECOVERY,
                      badge_usb_uplink_ota_receipt_decide(
                          BADGE_USB_EMIT_POISONED, false));
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_RECEIPT_ABORT_TERMINAL,
                      badge_usb_uplink_ota_receipt_decide(
                          BADGE_USB_EMIT_FAILED, false));
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_RECEIPT_ABORT_TERMINAL,
                      badge_usb_uplink_ota_receipt_decide(
                          BADGE_USB_EMIT_DROPPED, false));
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_RECEIPT_ACKNOWLEDGED,
                      badge_usb_uplink_ota_receipt_finalize(
                          BADGE_USB_UPLINK_RECEIPT_ACKNOWLEDGED, true));
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_RECEIPT_CLEANUP_RECOVERY,
                      badge_usb_uplink_ota_receipt_finalize(
                          BADGE_USB_UPLINK_RECEIPT_ACKNOWLEDGED, false));
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_RECEIPT_ABORT_TERMINAL,
                      badge_usb_uplink_ota_receipt_finalize(
                          BADGE_USB_UPLINK_RECEIPT_ABORT_TERMINAL, false));
}

void test_badge_usb_uplink_receipt_failure_and_latches_are_one_shot(void)
{
    badge_usb_uplink_ota_flow_t flow;
    badge_usb_uplink_ota_flow_init(&flow);
    uplink_usb_ota_result_t ready = adapter_result(
        true, false, UPLINK_USB_OTA_PHASE_READY, 0U, 5000U, 4096U);
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_WAIT_RECEIPT,
                      badge_usb_uplink_ota_flow_begin_result(&flow, &ready));
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_ABORT_DROP,
                      badge_usb_uplink_ota_flow_receipt_result(&flow, false));
    TEST_ASSERT_TRUE(badge_usb_uplink_ota_flow_take_cleanup(&flow));
    TEST_ASSERT_FALSE(badge_usb_uplink_ota_flow_take_cleanup(&flow));
    TEST_ASSERT_TRUE(badge_usb_uplink_ota_flow_take_terminal(&flow));
    TEST_ASSERT_FALSE(badge_usb_uplink_ota_flow_take_terminal(&flow));
}

void test_badge_usb_uplink_finish_result_is_typed_and_terminal_once(void)
{
    badge_usb_uplink_ota_flow_t flow = ready_flow(1024U);
    flow.transport_received = 1024U;
    flow.durable_received = 1024U;
    flow.credit_remaining = 0U;
    uplink_usb_ota_result_t committed = adapter_result(
        true, false, UPLINK_USB_OTA_PHASE_COMMITTED, 1024U, 1024U, 0U);
    TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_COMMITTED_RESTART,
                      badge_usb_uplink_ota_flow_finish_result(
                          &flow, true, &committed));
    TEST_ASSERT_TRUE(badge_usb_uplink_ota_flow_take_cleanup(&flow));
    TEST_ASSERT_FALSE(badge_usb_uplink_ota_flow_take_cleanup(&flow));
    TEST_ASSERT_TRUE(badge_usb_uplink_ota_flow_take_terminal(&flow));
    TEST_ASSERT_FALSE(badge_usb_uplink_ota_flow_take_terminal(&flow));
}

void test_badge_usb_uplink_result_frame_is_single_bounded_prefix(void)
{
    uplink_usb_ota_result_t result = adapter_result(
        true, false, UPLINK_USB_OTA_PHASE_CREDIT, 4096U, 5000U, 904U);
    char frame[BADGE_USB_UPLINK_OTA_FRAME_BYTES];
    size_t length = badge_usb_uplink_ota_render_result(
        &result, frame, sizeof(frame));

    TEST_ASSERT_GREATER_THAN_UINT(0U, length);
    TEST_ASSERT_LESS_THAN_UINT(sizeof(frame), length);
    TEST_ASSERT_EQUAL_STRING_LEN("FOF_UPLINK_OTA:", frame, 15U);
    TEST_ASSERT_EQUAL_PTR(NULL, strstr(frame + 15U, "FOF_UPLINK_OTA:"));
    TEST_ASSERT_EQUAL_PTR(NULL, strstr(frame, "FOF_UPLINK_UPLOAD"));
    TEST_ASSERT_NOT_NULL(strstr(frame, "\"phase\":\"credit\""));
    TEST_ASSERT_NOT_NULL(strstr(frame, "\"partition\":\"ota_1\""));
    TEST_ASSERT_NOT_NULL(strstr(frame, "\"received\":4096"));
    TEST_ASSERT_NOT_NULL(strstr(frame, "\"total\":5000"));
    TEST_ASSERT_NOT_NULL(strstr(frame, "\"credit_bytes\":904"));
    TEST_ASSERT_EQUAL_PTR(NULL, strstr(frame, "\"credit\":"));
    TEST_ASSERT_NOT_NULL(strstr(frame, "\"retryable\":false"));
    TEST_ASSERT_NOT_NULL(strstr(frame, "\"reboot_required\":false"));
    TEST_ASSERT_EQUAL_CHAR('\n', frame[length - 1U]);
}

void test_badge_usb_uplink_maintenance_required_frame_is_exact(void)
{
    uplink_usb_ota_result_t result;
    badge_usb_uplink_ota_maintenance_required_result(&result);
    static const char expected[] =
        "FOF_UPLINK_OTA:{\"ok\":false,\"phase\":\"error\","
        "\"partition\":\"none\",\"received\":0,\"total\":0,"
        "\"credit_bytes\":0,\"retryable\":true,"
        "\"reboot_required\":true,"
        "\"error\":\"update_maintenance_required\"}\n";
    char frame[BADGE_USB_UPLINK_OTA_FRAME_BYTES];
    size_t length = badge_usb_uplink_ota_render_result(
        &result, frame, sizeof(frame));

    TEST_ASSERT_EQUAL_UINT(sizeof(expected) - 1U, length);
    TEST_ASSERT_EQUAL_STRING(expected, frame);
}

typedef struct {
    bool emit_result;
    bool drain_result;
    bool restart_result;
    unsigned calls;
    unsigned emit_order;
    unsigned drain_order;
    unsigned restart_order;
} commit_fake_t;

static bool commit_emit(void *context)
{
    commit_fake_t *fake = context;
    fake->emit_order = ++fake->calls;
    return fake->emit_result;
}

static bool commit_drain(void *context)
{
    commit_fake_t *fake = context;
    fake->drain_order = ++fake->calls;
    return fake->drain_result;
}

static bool commit_restart(void *context)
{
    commit_fake_t *fake = context;
    fake->restart_order = ++fake->calls;
    return fake->restart_result;
}

void test_badge_usb_uplink_committed_restarts_after_emit_and_drain_failures(void)
{
    for (unsigned mask = 0U; mask < 4U; ++mask) {
        commit_fake_t fake = {
            .emit_result = (mask & 1U) != 0U,
            .drain_result = (mask & 2U) != 0U,
            .restart_result = true,
        };
        badge_usb_uplink_ota_commit_hooks_t hooks = {
            .context = &fake,
            .emit_committed = commit_emit,
            .drain = commit_drain,
            .restart = commit_restart,
        };
        TEST_ASSERT_TRUE(badge_usb_uplink_ota_run_committed(&hooks));
        TEST_ASSERT_EQUAL_UINT(1U, fake.emit_order);
        TEST_ASSERT_EQUAL_UINT(2U, fake.drain_order);
        TEST_ASSERT_EQUAL_UINT(3U, fake.restart_order);
    }
}

void test_badge_usb_uplink_committed_reports_rejected_restart(void)
{
    commit_fake_t fake = {
        .emit_result = true,
        .drain_result = true,
        .restart_result = false,
    };
    badge_usb_uplink_ota_commit_hooks_t hooks = {
        .context = &fake,
        .emit_committed = commit_emit,
        .drain = commit_drain,
        .restart = commit_restart,
    };

    TEST_ASSERT_FALSE(badge_usb_uplink_ota_run_committed(&hooks));
    TEST_ASSERT_EQUAL_UINT(1U, fake.emit_order);
    TEST_ASSERT_EQUAL_UINT(2U, fake.drain_order);
    TEST_ASSERT_EQUAL_UINT(3U, fake.restart_order);
}
