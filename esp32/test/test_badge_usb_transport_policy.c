#include "unity.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "badge_usb_transport_policy.h"

typedef struct {
    char events[16];
    size_t event_count;
    uint32_t delay_ms;
} fake_app_reenumerate_t;

static void app_reenumerate_event(fake_app_reenumerate_t *fake, char event)
{
    TEST_ASSERT_LESS_THAN_UINT32(
        sizeof(fake->events), (uint32_t)fake->event_count);
    fake->events[fake->event_count++] = event;
}

static void fake_app_enable_bus_clock(void *context)
{
    app_reenumerate_event(context, 'C');
}

static void fake_app_set_pad_enabled(void *context, bool enabled)
{
    app_reenumerate_event(context, enabled ? 'A' : 'D');
}

static void fake_app_delay_ms(void *context, uint32_t delay_ms)
{
    fake_app_reenumerate_t *fake = context;
    app_reenumerate_event(fake, 'W');
    fake->delay_ms = delay_ms;
}

static void fake_app_select_internal_phy(void *context)
{
    app_reenumerate_event(context, 'I');
}

void test_badge_usb_app_boot_forces_clean_host_visible_reenumeration(void)
{
    fake_app_reenumerate_t fake = {0};
    badge_usb_app_reenumerate_hooks_t hooks = {
        .context = &fake,
        .enable_bus_clock = fake_app_enable_bus_clock,
        .set_pad_enabled = fake_app_set_pad_enabled,
        .delay_ms = fake_app_delay_ms,
        .select_internal_phy = fake_app_select_internal_phy,
    };

    TEST_ASSERT_TRUE(badge_usb_app_reenumerate(&hooks));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(500U, fake.delay_ms);
    TEST_ASSERT_EQUAL_UINT32(4U, (uint32_t)fake.event_count);
    /* Preserve the controller/FIFO/interrupt state that ESP-IDF's driver
     * deliberately adopts. The host-visible detach and subsequent USB bus
     * reset provide the clean endpoint handoff without a peripheral reset. */
    TEST_ASSERT_EQUAL_MEMORY("CDIW", fake.events, fake.event_count);
}

void test_badge_usb_app_boot_reenumeration_rejects_incomplete_hooks(void)
{
    fake_app_reenumerate_t fake = {0};
    badge_usb_app_reenumerate_hooks_t hooks = {
        .context = &fake,
        .enable_bus_clock = fake_app_enable_bus_clock,
        .set_pad_enabled = fake_app_set_pad_enabled,
        .delay_ms = fake_app_delay_ms,
        .select_internal_phy = NULL,
    };

    TEST_ASSERT_FALSE(badge_usb_app_reenumerate(NULL));
    TEST_ASSERT_FALSE(badge_usb_app_reenumerate(&hooks));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)fake.event_count);
}

typedef struct {
    bool connected;
    bool lock_succeeds;
    bool drain_succeeds;
    bool poison_while_waiting;
    bool trigger_nested_log;
    bool nested_triggered;
    uintptr_t owner;
    uint32_t now_ticks;
    size_t write_calls;
    size_t write_sizes[8];
    int forced_write_result;
    badge_usb_emit_result_t nested_result;
    badge_usb_output_policy_t *policy;
    badge_usb_output_hooks_t *hooks;
} fake_output_t;

static bool fake_host_connected(void *context)
{
    return ((fake_output_t *)context)->connected;
}

static bool fake_lock(void *context, uint32_t timeout_ticks)
{
    (void)timeout_ticks;
    fake_output_t *fake = context;
    if (fake->poison_while_waiting && fake->policy) {
        fake->policy->poisoned = true;
    }
    return fake->lock_succeeds;
}

static void fake_unlock(void *context)
{
    (void)context;
}

static uintptr_t fake_current_owner(void *context)
{
    return ((fake_output_t *)context)->owner;
}

static uint32_t fake_now_ticks(void *context)
{
    return ((fake_output_t *)context)->now_ticks++;
}

static int fake_write(void *context, const uint8_t *data, size_t len,
                      uint32_t timeout_ticks)
{
    (void)data;
    (void)timeout_ticks;
    fake_output_t *fake = context;
    TEST_ASSERT_LESS_THAN_UINT32(8U, (uint32_t)fake->write_calls);
    fake->write_sizes[fake->write_calls++] = len;
    if (fake->trigger_nested_log && !fake->nested_triggered) {
        fake->nested_triggered = true;
        static const uint8_t log_line[] = "nested log\n";
        fake->nested_result = badge_usb_output_emit(
            fake->policy, fake->hooks, log_line, sizeof(log_line) - 1U,
            BADGE_USB_FRAME_OPTIONAL, 0U);
    }
    return fake->forced_write_result >= 0 ? fake->forced_write_result : (int)len;
}

static bool fake_drain(void *context, uint32_t timeout_ticks)
{
    (void)timeout_ticks;
    return ((fake_output_t *)context)->drain_succeeds;
}

static void setup_output(fake_output_t *fake,
                         badge_usb_output_policy_t *policy,
                         badge_usb_output_hooks_t *hooks)
{
    memset(fake, 0, sizeof(*fake));
    memset(policy, 0, sizeof(*policy));
    memset(hooks, 0, sizeof(*hooks));
    fake->lock_succeeds = true;
    fake->drain_succeeds = true;
    fake->owner = 0x1234U;
    fake->forced_write_result = -1;
    fake->policy = policy;
    fake->hooks = hooks;
    hooks->context = fake;
    hooks->host_connected = fake_host_connected;
    hooks->lock = fake_lock;
    hooks->unlock = fake_unlock;
    hooks->current_owner = fake_current_owner;
    hooks->now_ticks = fake_now_ticks;
    hooks->write = fake_write;
    hooks->drain = fake_drain;
}

typedef struct {
    char events[32];
    size_t event_count;
    const char *write_receipt;
    const char *final_receipt;
    bool write_succeeds;
    bool commit_succeeds;
    bool finalize_succeeds;
    badge_usb_emit_result_t emit_result;
    bool drain_succeeds;
    bool completion_succeeds;
    bool activated;
    size_t emit_calls;
    size_t drain_calls;
    size_t completion_calls;
} fake_scanner_credit_t;

static void scanner_credit_event(fake_scanner_credit_t *fake, char event)
{
    TEST_ASSERT_LESS_THAN_UINT32(
        sizeof(fake->events), (uint32_t)fake->event_count);
    fake->events[fake->event_count++] = event;
}

static bool fake_scanner_write_durable(
    void *context, const uint8_t *data, size_t len,
    char *out_receipt, size_t out_receipt_len)
{
    fake_scanner_credit_t *fake = context;
    scanner_credit_event(fake, 'W');
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_GREATER_THAN_UINT32(0U, (uint32_t)len);
    if (fake->write_receipt && out_receipt && out_receipt_len > 0U) {
        snprintf(out_receipt, out_receipt_len, "%s", fake->write_receipt);
    }
    return fake->write_succeeds;
}

static bool fake_scanner_commit_transport(void *context)
{
    fake_scanner_credit_t *fake = context;
    scanner_credit_event(fake, 'C');
    return fake->commit_succeeds;
}

static bool fake_scanner_finalize_durable(
    void *context, char *out_receipt, size_t out_receipt_len)
{
    fake_scanner_credit_t *fake = context;
    scanner_credit_event(fake, 'F');
    if (fake->final_receipt && out_receipt && out_receipt_len > 0U) {
        snprintf(out_receipt, out_receipt_len, "%s", fake->final_receipt);
    }
    return fake->finalize_succeeds;
}

static badge_usb_emit_result_t fake_scanner_emit_required(
    void *context, const char *receipt)
{
    fake_scanner_credit_t *fake = context;
    scanner_credit_event(fake, 'E');
    TEST_ASSERT_NOT_NULL(receipt);
    TEST_ASSERT_NOT_EQUAL('\0', receipt[0]);
    fake->emit_calls++;
    return fake->emit_result;
}

static bool fake_scanner_drain_required(void *context)
{
    fake_scanner_credit_t *fake = context;
    scanner_credit_event(fake, 'D');
    fake->drain_calls++;
    return fake->drain_succeeds;
}

static bool fake_scanner_complete_terminal(void *context, bool delivered)
{
    fake_scanner_credit_t *fake = context;
    scanner_credit_event(fake, 'T');
    fake->completion_calls++;
    fake->activated = delivered && fake->completion_succeeds;
    return fake->activated;
}

static void setup_scanner_credit(
    fake_scanner_credit_t *fake,
    badge_usb_scanner_credit_hooks_t *hooks)
{
    memset(fake, 0, sizeof(*fake));
    memset(hooks, 0, sizeof(*hooks));
    fake->write_succeeds = true;
    fake->commit_succeeds = true;
    fake->finalize_succeeds = true;
    fake->emit_result = BADGE_USB_EMIT_COMPLETED;
    fake->drain_succeeds = true;
    fake->completion_succeeds = true;
    hooks->context = fake;
    hooks->write_durable = fake_scanner_write_durable;
    hooks->commit_transport = fake_scanner_commit_transport;
    hooks->finalize_durable = fake_scanner_finalize_durable;
    hooks->emit_required = fake_scanner_emit_required;
    hooks->drain_required = fake_scanner_drain_required;
    hooks->complete_terminal = fake_scanner_complete_terminal;
}

void test_badge_usb_disconnected_logs_do_not_poison_later_ping(void)
{
    fake_output_t fake;
    badge_usb_output_policy_t policy;
    badge_usb_output_hooks_t hooks;
    setup_output(&fake, &policy, &hooks);
    static const uint8_t log_line[] = "boot log\n";
    static const uint8_t pong[] = "FOF_PONG:test\n";

    TEST_ASSERT_EQUAL(BADGE_USB_EMIT_DROPPED,
                      badge_usb_output_emit(&policy, &hooks, log_line,
                                            sizeof(log_line) - 1U,
                                            BADGE_USB_FRAME_OPTIONAL, 0U));
    TEST_ASSERT_FALSE(policy.poisoned);
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)fake.write_calls);

    fake.connected = true;
    TEST_ASSERT_EQUAL(BADGE_USB_EMIT_COMPLETED,
                      badge_usb_output_emit(&policy, &hooks, pong,
                                            sizeof(pong) - 1U,
                                            BADGE_USB_FRAME_REQUIRED, 250U));
    TEST_ASSERT_FALSE(policy.poisoned);
}

void test_badge_usb_fully_enqueued_optional_drain_pending_is_not_partial(void)
{
    fake_output_t fake;
    badge_usb_output_policy_t policy;
    badge_usb_output_hooks_t hooks;
    setup_output(&fake, &policy, &hooks);
    fake.connected = true;
    fake.drain_succeeds = false;
    static const uint8_t frame[] = "optional\n";

    TEST_ASSERT_EQUAL(BADGE_USB_EMIT_ENQUEUED,
                      badge_usb_output_emit(&policy, &hooks, frame,
                                            sizeof(frame) - 1U,
                                            BADGE_USB_FRAME_OPTIONAL, 0U));
    TEST_ASSERT_FALSE(policy.poisoned);

    fake.drain_succeeds = true;
    TEST_ASSERT_EQUAL(BADGE_USB_EMIT_COMPLETED,
                      badge_usb_output_emit(&policy, &hooks, frame,
                                            sizeof(frame) - 1U,
                                            BADGE_USB_FRAME_REQUIRED, 250U));
}

void test_badge_usb_fully_enqueued_required_drain_timeout_is_recoverable(void)
{
    fake_output_t fake;
    badge_usb_output_policy_t policy;
    badge_usb_output_hooks_t hooks;
    setup_output(&fake, &policy, &hooks);
    fake.connected = true;
    fake.drain_succeeds = false;
    static const uint8_t frame[] = "FOF_PONG:test\n";

    TEST_ASSERT_EQUAL(BADGE_USB_EMIT_ENQUEUED,
                      badge_usb_output_emit(&policy, &hooks, frame,
                                            sizeof(frame) - 1U,
                                            BADGE_USB_FRAME_REQUIRED, 250U));
    TEST_ASSERT_FALSE(policy.poisoned);

    fake.drain_succeeds = true;
    TEST_ASSERT_EQUAL(BADGE_USB_EMIT_COMPLETED,
                      badge_usb_output_emit(&policy, &hooks, frame,
                                            sizeof(frame) - 1U,
                                            BADGE_USB_FRAME_REQUIRED, 250U));
}

void test_badge_usb_partial_optional_write_poisons_output(void)
{
    fake_output_t fake;
    badge_usb_output_policy_t policy;
    badge_usb_output_hooks_t hooks;
    setup_output(&fake, &policy, &hooks);
    fake.connected = true;
    fake.forced_write_result = 1;
    static const uint8_t frame[] = "optional\n";

    TEST_ASSERT_EQUAL(BADGE_USB_EMIT_POISONED,
                      badge_usb_output_emit(&policy, &hooks, frame,
                                            sizeof(frame) - 1U,
                                            BADGE_USB_FRAME_OPTIONAL, 0U));
    TEST_ASSERT_TRUE(policy.poisoned);
    TEST_ASSERT_EQUAL(BADGE_USB_EMIT_POISONED,
                      badge_usb_output_emit(&policy, &hooks, frame,
                                            sizeof(frame) - 1U,
                                            BADGE_USB_FRAME_REQUIRED, 250U));
}

void test_badge_usb_large_required_frame_is_chunked_at_2048(void)
{
    fake_output_t fake;
    badge_usb_output_policy_t policy;
    badge_usb_output_hooks_t hooks;
    setup_output(&fake, &policy, &hooks);
    fake.connected = true;
    uint8_t frame[5000] = {0};

    TEST_ASSERT_EQUAL(BADGE_USB_EMIT_COMPLETED,
                      badge_usb_output_emit(&policy, &hooks, frame,
                                            sizeof(frame),
                                            BADGE_USB_FRAME_REQUIRED, 1000U));
    TEST_ASSERT_EQUAL_UINT32(3U, (uint32_t)fake.write_calls);
    TEST_ASSERT_EQUAL_UINT32(2048U, (uint32_t)fake.write_sizes[0]);
    TEST_ASSERT_EQUAL_UINT32(2048U, (uint32_t)fake.write_sizes[1]);
    TEST_ASSERT_EQUAL_UINT32(904U, (uint32_t)fake.write_sizes[2]);
}

void test_badge_usb_waiter_rechecks_poison_after_lock(void)
{
    fake_output_t fake;
    badge_usb_output_policy_t policy;
    badge_usb_output_hooks_t hooks;
    setup_output(&fake, &policy, &hooks);
    fake.connected = true;
    fake.poison_while_waiting = true;
    static const uint8_t frame[] = "FOF_PONG:test\n";

    TEST_ASSERT_EQUAL(BADGE_USB_EMIT_POISONED,
                      badge_usb_output_emit(&policy, &hooks, frame,
                                            sizeof(frame) - 1U,
                                            BADGE_USB_FRAME_REQUIRED, 250U));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)fake.write_calls);
}

void test_badge_usb_nested_optional_log_is_dropped_during_large_required(void)
{
    fake_output_t fake;
    badge_usb_output_policy_t policy;
    badge_usb_output_hooks_t hooks;
    setup_output(&fake, &policy, &hooks);
    fake.connected = true;
    fake.trigger_nested_log = true;
    uint8_t frame[4097] = {0};

    TEST_ASSERT_EQUAL(BADGE_USB_EMIT_COMPLETED,
                      badge_usb_output_emit(&policy, &hooks, frame,
                                            sizeof(frame),
                                            BADGE_USB_FRAME_REQUIRED, 1000U));
    TEST_ASSERT_TRUE(fake.nested_triggered);
    TEST_ASSERT_EQUAL(BADGE_USB_EMIT_DROPPED, fake.nested_result);
    TEST_ASSERT_EQUAL_UINT32(3U, (uint32_t)fake.write_calls);
    TEST_ASSERT_FALSE(policy.poisoned);
}

void test_badge_usb_readiness_gates_contract_commands_and_rejects_unknown(void)
{
    TEST_ASSERT_EQUAL(BADGE_USB_COMMAND_BOOTING,
                      badge_usb_command_decide(true, false, false, false));
    TEST_ASSERT_EQUAL(BADGE_USB_COMMAND_DISPATCH,
                      badge_usb_command_decide(true, true, false, false));
    TEST_ASSERT_EQUAL(BADGE_USB_COMMAND_BOOTING,
                      badge_usb_command_decide(false, false, false, false));
    TEST_ASSERT_EQUAL(BADGE_USB_COMMAND_UNKNOWN,
                      badge_usb_command_decide(false, true, false, false));
    TEST_ASSERT_EQUAL(BADGE_USB_COMMAND_RECOVERY_ONLY,
                      badge_usb_command_decide(true, true, true, false));
    TEST_ASSERT_EQUAL(BADGE_USB_COMMAND_DISPATCH,
                      badge_usb_command_decide(true, true, true, true));
    TEST_ASSERT_EQUAL(BADGE_USB_COMMAND_DISPATCH,
                      badge_usb_command_decide(false, true, true, true));
}

void test_badge_usb_upload_terminal_failure_clears_health_and_blocks_activation(void)
{
    badge_usb_upload_policy_t upload;
    badge_usb_upload_policy_init(&upload);
    TEST_ASSERT_TRUE(badge_usb_upload_begin(&upload,
                                            BADGE_USB_BINARY_SCANNER, 4U));
    TEST_ASSERT_TRUE(badge_usb_upload_note_bytes(&upload, 4U));
    badge_usb_upload_note_durable_finalize(&upload);

    TEST_ASSERT_FALSE(badge_usb_upload_terminal_result(&upload, false));
    TEST_ASSERT_EQUAL(BADGE_USB_BINARY_NONE, upload.target);
    TEST_ASSERT_EQUAL_UINT32(0U, upload.received);
    TEST_ASSERT_EQUAL_UINT32(0U, upload.size);
    TEST_ASSERT_FALSE(upload.durable_finalized);
}

void test_badge_usb_upload_activates_only_after_successful_terminal(void)
{
    badge_usb_upload_policy_t upload;
    badge_usb_upload_policy_init(&upload);
    TEST_ASSERT_TRUE(badge_usb_upload_begin(&upload,
                                            BADGE_USB_BINARY_SCANNER, 4U));
    TEST_ASSERT_TRUE(badge_usb_upload_note_bytes(&upload, 4U));
    badge_usb_upload_note_durable_finalize(&upload);

    TEST_ASSERT_TRUE(badge_usb_upload_terminal_result(&upload, true));
    TEST_ASSERT_EQUAL(BADGE_USB_BINARY_NONE, upload.target);
    TEST_ASSERT_EQUAL_UINT32(0U, upload.received);
    TEST_ASSERT_EQUAL_UINT32(0U, upload.size);
}

void test_badge_usb_upload_abort_is_target_aware_and_clears_health(void)
{
    badge_usb_upload_policy_t upload;
    badge_usb_upload_policy_init(&upload);
    TEST_ASSERT_TRUE(badge_usb_upload_begin(&upload,
                                            BADGE_USB_BINARY_UPLINK, 9U));
    TEST_ASSERT_TRUE(badge_usb_upload_note_bytes(&upload, 3U));

    TEST_ASSERT_EQUAL(BADGE_USB_BINARY_UPLINK,
                      badge_usb_upload_abort(&upload));
    TEST_ASSERT_EQUAL(BADGE_USB_BINARY_NONE, upload.target);
    TEST_ASSERT_EQUAL_UINT32(0U, upload.received);
    TEST_ASSERT_EQUAL_UINT32(0U, upload.size);
}

void test_badge_usb_scanner_credit_v1_waits_at_exact_durable_windows(void)
{
    badge_usb_upload_policy_t upload;
    badge_usb_upload_policy_init(&upload);
    TEST_ASSERT_TRUE(badge_usb_upload_begin_credit_v1(
        &upload, BADGE_USB_BINARY_SCANNER, 5000U));
    TEST_ASSERT_TRUE(upload.credit_v1);
    TEST_ASSERT_EQUAL_UINT32(4096U, upload.credit_remaining);

    size_t allowed = 0U;
    TEST_ASSERT_TRUE(badge_usb_upload_plan_credit_bytes(
        &upload, 4096U, &allowed));
    TEST_ASSERT_EQUAL_UINT(4096U, allowed);
    TEST_ASSERT_TRUE(badge_usb_upload_note_bytes(&upload, 4096U));
    TEST_ASSERT_TRUE(badge_usb_upload_credit_pending(&upload));
    TEST_ASSERT_EQUAL_UINT32(
        904U, badge_usb_upload_pending_credit(&upload));
    TEST_ASSERT_FALSE(badge_usb_upload_plan_credit_bytes(
        &upload, 1U, &allowed));

    TEST_ASSERT_TRUE(badge_usb_upload_credit_result(&upload, true));
    TEST_ASSERT_FALSE(badge_usb_upload_credit_pending(&upload));
    TEST_ASSERT_EQUAL_UINT32(904U, upload.credit_remaining);
    TEST_ASSERT_TRUE(badge_usb_upload_plan_credit_bytes(
        &upload, 904U, &allowed));
    TEST_ASSERT_TRUE(badge_usb_upload_note_bytes(&upload, 904U));
    TEST_ASSERT_EQUAL_UINT32(5000U, upload.received);
    TEST_ASSERT_FALSE(badge_usb_upload_credit_pending(&upload));
    TEST_ASSERT_EQUAL_UINT32(0U, upload.credit_remaining);
}

void test_badge_usb_scanner_credit_v1_rejects_overrun_and_stale_receipt(void)
{
    badge_usb_upload_policy_t upload;
    badge_usb_upload_policy_init(&upload);
    TEST_ASSERT_TRUE(badge_usb_upload_begin_credit_v1(
        &upload, BADGE_USB_BINARY_SCANNER, 8192U));

    size_t allowed = 0U;
    TEST_ASSERT_FALSE(badge_usb_upload_plan_credit_bytes(
        &upload, 4097U, &allowed));
    TEST_ASSERT_EQUAL_UINT(0U, allowed);
    TEST_ASSERT_FALSE(badge_usb_upload_credit_result(&upload, true));

    TEST_ASSERT_TRUE(badge_usb_upload_plan_credit_bytes(
        &upload, 4096U, &allowed));
    TEST_ASSERT_TRUE(badge_usb_upload_note_bytes(&upload, 4096U));
    TEST_ASSERT_FALSE(badge_usb_upload_credit_result(&upload, false));
    TEST_ASSERT_TRUE(badge_usb_upload_credit_pending(&upload));

    TEST_ASSERT_EQUAL(
        BADGE_USB_BINARY_SCANNER, badge_usb_upload_abort(&upload));
    TEST_ASSERT_TRUE(badge_usb_upload_begin_credit_v1(
        &upload, BADGE_USB_BINARY_SCANNER, 8192U));
    TEST_ASSERT_TRUE(badge_usb_upload_note_bytes(&upload, 4096U));
    TEST_ASSERT_TRUE(badge_usb_upload_credit_result(&upload, true));
    TEST_ASSERT_TRUE(badge_usb_upload_note_bytes(&upload, 4096U));
    TEST_ASSERT_EQUAL_UINT32(8192U, upload.received);
    TEST_ASSERT_FALSE(badge_usb_upload_credit_pending(&upload));
    TEST_ASSERT_EQUAL_UINT32(0U, upload.credit_remaining);
}

void test_badge_usb_scanner_credit_wiring_writes_before_receipt_drain(void)
{
    badge_usb_upload_policy_t upload;
    badge_usb_upload_policy_init(&upload);
    TEST_ASSERT_TRUE(badge_usb_upload_begin_credit_v1(
        &upload, BADGE_USB_BINARY_SCANNER, 8192U));
    fake_scanner_credit_t fake;
    badge_usb_scanner_credit_hooks_t hooks;
    setup_scanner_credit(&fake, &hooks);
    fake.write_receipt = "{\"phase\":\"credit\"}";
    fake.emit_result = BADGE_USB_EMIT_ENQUEUED;
    uint8_t bytes[4096] = {0};
    char receipt[64];

    TEST_ASSERT_EQUAL(
        BADGE_USB_SCANNER_CREDIT_CONTINUE,
        badge_usb_scanner_credit_process(
            &upload, &hooks, bytes, sizeof(bytes), false,
            receipt, sizeof(receipt)));
    TEST_ASSERT_EQUAL_MEMORY("WCED", fake.events, 4U);
    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)fake.emit_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)fake.drain_calls);
}

void test_badge_usb_scanner_credit_wiring_failed_drain_grants_no_credit(void)
{
    badge_usb_upload_policy_t upload;
    badge_usb_upload_policy_init(&upload);
    TEST_ASSERT_TRUE(badge_usb_upload_begin_credit_v1(
        &upload, BADGE_USB_BINARY_SCANNER, 8192U));
    fake_scanner_credit_t fake;
    badge_usb_scanner_credit_hooks_t hooks;
    setup_scanner_credit(&fake, &hooks);
    fake.write_receipt = "{\"phase\":\"credit\"}";
    fake.emit_result = BADGE_USB_EMIT_ENQUEUED;
    fake.drain_succeeds = false;
    uint8_t bytes[4096] = {0};
    char receipt[64];

    TEST_ASSERT_EQUAL(
        BADGE_USB_SCANNER_CREDIT_RECEIPT_FAILED,
        badge_usb_scanner_credit_process(
            &upload, &hooks, bytes, sizeof(bytes), false,
            receipt, sizeof(receipt)));
    TEST_ASSERT_EQUAL(BADGE_USB_BINARY_NONE, upload.target);
    TEST_ASSERT_FALSE(badge_usb_upload_credit_pending(&upload));
    TEST_ASSERT_EQUAL_UINT32(0U, upload.credit_remaining);
    TEST_ASSERT_FALSE(badge_usb_upload_credit_result(&upload, true));
    TEST_ASSERT_EQUAL_UINT32(0U, upload.credit_remaining);
}

void test_badge_usb_scanner_credit_wiring_success_grants_one_next_window(void)
{
    badge_usb_upload_policy_t upload;
    badge_usb_upload_policy_init(&upload);
    TEST_ASSERT_TRUE(badge_usb_upload_begin_credit_v1(
        &upload, BADGE_USB_BINARY_SCANNER, 12288U));
    fake_scanner_credit_t fake;
    badge_usb_scanner_credit_hooks_t hooks;
    setup_scanner_credit(&fake, &hooks);
    fake.write_receipt = "{\"phase\":\"credit\"}";
    fake.emit_result = BADGE_USB_EMIT_ENQUEUED;
    uint8_t bytes[4096] = {0};
    char receipt[64];

    TEST_ASSERT_EQUAL(
        BADGE_USB_SCANNER_CREDIT_CONTINUE,
        badge_usb_scanner_credit_process(
            &upload, &hooks, bytes, sizeof(bytes), false,
            receipt, sizeof(receipt)));
    TEST_ASSERT_FALSE(badge_usb_upload_credit_pending(&upload));
    TEST_ASSERT_EQUAL_UINT32(4096U, upload.credit_remaining);
    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)fake.emit_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)fake.drain_calls);
    TEST_ASSERT_FALSE(badge_usb_upload_credit_result(&upload, true));
    TEST_ASSERT_EQUAL_UINT32(4096U, upload.credit_remaining);
}

void test_badge_usb_scanner_credit_wiring_finalize_failure_blocks_activation(void)
{
    badge_usb_upload_policy_t upload;
    badge_usb_upload_policy_init(&upload);
    TEST_ASSERT_TRUE(badge_usb_upload_begin_credit_v1(
        &upload, BADGE_USB_BINARY_SCANNER, 32U));
    fake_scanner_credit_t fake;
    badge_usb_scanner_credit_hooks_t hooks;
    setup_scanner_credit(&fake, &hooks);
    fake.finalize_succeeds = false;
    fake.final_receipt = "{\"ok\":false}";
    uint8_t bytes[32] = {0};
    char receipt[64];

    TEST_ASSERT_EQUAL(
        BADGE_USB_SCANNER_CREDIT_FINALIZE_FAILED,
        badge_usb_scanner_credit_process(
            &upload, &hooks, bytes, sizeof(bytes), true,
            receipt, sizeof(receipt)));
    TEST_ASSERT_FALSE(fake.activated);
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)fake.completion_calls);
    TEST_ASSERT_EQUAL(BADGE_USB_BINARY_NONE, upload.target);
}

void test_badge_usb_scanner_credit_wiring_terminal_failure_blocks_activation(void)
{
    badge_usb_upload_policy_t upload;
    badge_usb_upload_policy_init(&upload);
    TEST_ASSERT_TRUE(badge_usb_upload_begin_credit_v1(
        &upload, BADGE_USB_BINARY_SCANNER, 32U));
    fake_scanner_credit_t fake;
    badge_usb_scanner_credit_hooks_t hooks;
    setup_scanner_credit(&fake, &hooks);
    fake.final_receipt = "{\"phase\":\"final\"}";
    fake.emit_result = BADGE_USB_EMIT_ENQUEUED;
    fake.drain_succeeds = false;
    uint8_t bytes[32] = {0};
    char receipt[64];

    TEST_ASSERT_EQUAL(
        BADGE_USB_SCANNER_CREDIT_TERMINAL_FAILED,
        badge_usb_scanner_credit_process(
            &upload, &hooks, bytes, sizeof(bytes), true,
            receipt, sizeof(receipt)));
    TEST_ASSERT_FALSE(fake.activated);
    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)fake.completion_calls);
    TEST_ASSERT_EQUAL_MEMORY("WCFEDT", fake.events, 6U);
}

void test_badge_usb_scanner_credit_wiring_activates_after_durable_terminal(void)
{
    badge_usb_upload_policy_t upload;
    badge_usb_upload_policy_init(&upload);
    TEST_ASSERT_TRUE(badge_usb_upload_begin_credit_v1(
        &upload, BADGE_USB_BINARY_SCANNER, 32U));
    fake_scanner_credit_t fake;
    badge_usb_scanner_credit_hooks_t hooks;
    setup_scanner_credit(&fake, &hooks);
    fake.final_receipt = "{\"phase\":\"final\"}";
    fake.emit_result = BADGE_USB_EMIT_ENQUEUED;
    uint8_t bytes[32] = {0};
    char receipt[64];

    TEST_ASSERT_EQUAL(
        BADGE_USB_SCANNER_CREDIT_COMPLETE,
        badge_usb_scanner_credit_process(
            &upload, &hooks, bytes, sizeof(bytes), true,
            receipt, sizeof(receipt)));
    TEST_ASSERT_TRUE(fake.activated);
    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)fake.completion_calls);
    TEST_ASSERT_EQUAL_MEMORY("WCFEDT", fake.events, 6U);
}

void test_badge_usb_binary_idle_timeout_contract_is_exactly_five_seconds(void)
{
    TEST_ASSERT_EQUAL_UINT32(5000U, BADGE_USB_BINARY_IDLE_TIMEOUT_MS);
}
