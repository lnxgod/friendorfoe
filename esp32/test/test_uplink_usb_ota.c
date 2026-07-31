#include "unity.h"

#include "uplink_usb_ota.h"
#include "badge_usb_uplink_ota.h"

#include <string.h>

#define APP_TYPE 0x00U
#define DATA_TYPE 0x01U
#define OTA0_SUBTYPE 0x10U
#define OTA1_SUBTYPE 0x11U
#define TEST_DESCRIPTOR_BYTES 144U

typedef enum {
    FAIL_NONE = 0,
    FAIL_OPERATION_BEGIN,
    FAIL_PAUSE_HTTP,
    FAIL_PAUSE_SCANNER_0,
    FAIL_PAUSE_SCANNER_1,
    FAIL_OTA_BEGIN,
    FAIL_INTEGRITY_START,
    FAIL_OTA_WRITE,
    FAIL_INTEGRITY_UPDATE,
    FAIL_INTEGRITY_FINISH,
    FAIL_OTA_END,
    FAIL_SET_BOOT,
} fake_failure_t;

typedef struct {
    uplink_usb_ota_partition_t running;
    uplink_usb_ota_partition_t next;
    uplink_usb_ota_image_state_t image_state;
    fof_firmware_image_identity_t running_identity;
    fof_firmware_image_identity_t target_identity;
    int operation_begin_calls;
    int operation_end_calls;
    int get_running_calls;
    int get_next_calls;
    int get_state_calls;
    int pause_http_calls;
    int pause_scanner_calls[2];
    int ota_begin_calls;
    int ota_write_calls;
    size_t ota_write_lengths[8];
    uint32_t ota_written;
    int ota_abort_calls;
    int ota_end_calls;
    int set_boot_calls;
    int get_identity_calls;
    int operation_release_order;
    int resume_http_calls;
    int resume_http_order;
    int resume_scanner_calls[2];
    int resume_scanner_order[2];
    int integrity_start_calls;
    int integrity_abort_calls;
    int order;
    fake_failure_t fail;
    bool pause_http_owned;
    bool pause_scanner_owned[2];
    bool drift_after_end;
    int operation_end_failures_remaining;
    bool operation_end_permanent_failure;
    fw_operation_state_t operation_state;
    bool adversarial_end_claim;
    bool competing_claim_before_release;
    bool competing_claim_after_release;
    bool all_owned_workers_resumed_before_release;
} fake_ota_t;

static fake_ota_t s_fake;

static void receive_complete_image(uint8_t image[1024],
                                   uplink_usb_ota_result_t *result);

static uplink_usb_ota_partition_t partition_fixture(
    uintptr_t native_id, const char *label, uint8_t type, uint8_t subtype,
    uint32_t offset, uint32_t size)
{
    uplink_usb_ota_partition_t partition = {
        .native_id = native_id,
        .type = type,
        .subtype = subtype,
        .offset = offset,
        .size = size,
    };
    strcpy(partition.label, label);
    return partition;
}

static uplink_ota_manifest_t manifest_fixture(void)
{
    uplink_ota_manifest_t manifest = {0};
    strcpy(manifest.target, UPLINK_OTA_TARGET);
    strcpy(manifest.project, UPLINK_OTA_PROJECT);
    strcpy(manifest.hardware, UPLINK_OTA_HARDWARE);
    strcpy(manifest.version, "0.64.69");
    memset(manifest.sha256, '1', 64U);
    manifest.sha256[64] = '\0';
    manifest.size = 1024U;
    manifest.crc32 = 1024U;
    return manifest;
}

static void image_fixture(uint8_t image[1024])
{
    memset(image, 0xA5, 1024U);
    image[0] = 0xE9U;
    image[0x20] = 0x32U;
    image[0x21] = 0x54U;
    image[0x22] = 0xCDU;
    image[0x23] = 0xABU;
    memset(image + 0x30, 0, 32U);
    memcpy(image + 0x30, "0.64.69", 8U);
    memset(image + 0x50, 0, 32U);
    memcpy(image + 0x50, UPLINK_OTA_PROJECT,
           sizeof(UPLINK_OTA_PROJECT));
    memcpy(image + 400U, UPLINK_OTA_TARGET,
           sizeof(UPLINK_OTA_TARGET));
    /* Deliberately crosses the 656-byte transport-call boundary. */
    memcpy(image + 650U, UPLINK_OTA_HARDWARE,
           sizeof(UPLINK_OTA_HARDWARE));
}

static bool fake_get_running(void *context, uplink_usb_ota_partition_t *out)
{
    fake_ota_t *fake = context;
    fake->get_running_calls++;
    *out = fake->running;
    return true;
}

static bool fake_get_next(void *context,
                          const uplink_usb_ota_partition_t *running,
                          uplink_usb_ota_partition_t *out)
{
    fake_ota_t *fake = context;
    fake->get_next_calls++;
    TEST_ASSERT_EQUAL_UINT64(fake->running.native_id, running->native_id);
    *out = fake->next;
    return true;
}

static bool fake_get_state(void *context,
                           const uplink_usb_ota_partition_t *partition,
                           uplink_usb_ota_image_state_t *out)
{
    fake_ota_t *fake = context;
    fake->get_state_calls++;
    TEST_ASSERT_EQUAL_UINT64(fake->running.native_id, partition->native_id);
    *out = fake->image_state;
    return true;
}

static bool fake_get_identity(void *context,
                              const uplink_usb_ota_partition_t *partition,
                              fof_firmware_image_identity_t *out)
{
    fake_ota_t *fake = context;
    fake->get_identity_calls++;
    *out = partition->native_id == fake->running.native_id
        ? fake->running_identity : fake->target_identity;
    return true;
}

static bool fake_ota_begin(void *context,
                           const uplink_usb_ota_partition_t *partition,
                           uintptr_t *handle)
{
    fake_ota_t *fake = context;
    fake->ota_begin_calls++;
    TEST_ASSERT_EQUAL_UINT64(fake->next.native_id, partition->native_id);
    *handle = 7U;
    return fake->fail != FAIL_OTA_BEGIN;
}

static bool fake_ota_write(void *context, uintptr_t handle,
                           const uint8_t *bytes, size_t length)
{
    fake_ota_t *fake = context;
    (void)handle;
    (void)bytes;
    if (fake->ota_write_calls < 8) {
        fake->ota_write_lengths[fake->ota_write_calls] = length;
    }
    fake->ota_write_calls++;
    fake->ota_written += (uint32_t)length;
    return fake->fail != FAIL_OTA_WRITE;
}

static bool fake_ota_handle(void *context, uintptr_t handle)
{
    fake_ota_t *fake = context;
    (void)handle;
    fake->ota_end_calls++;
    if (fake->drift_after_end) {
        fake->next.offset++;
    }
    return fake->fail != FAIL_OTA_END;
}

static bool fake_ota_abort(void *context, uintptr_t handle)
{
    fake_ota_t *fake = context;
    (void)handle;
    fake->ota_abort_calls++;
    return true;
}

static bool fake_set_boot(void *context,
                          const uplink_usb_ota_partition_t *partition)
{
    fake_ota_t *fake = context;
    fake->set_boot_calls++;
    TEST_ASSERT_EQUAL_UINT64(fake->next.native_id, partition->native_id);
    return fake->fail != FAIL_SET_BOOT;
}

static bool fake_operation_begin(void *context, fw_operation_token_t *out)
{
    fake_ota_t *fake = context;
    fake->operation_begin_calls++;
    if (fake->fail == FAIL_OPERATION_BEGIN) {
        return false;
    }
    return fw_operation_state_try_begin(
        &fake->operation_state, FW_OPERATION_OWNER_UPLINK_OTA, out);
}

static bool fake_operation_end(void *context, fw_operation_token_t token)
{
    fake_ota_t *fake = context;
    fake->operation_end_calls++;
    fake->operation_release_order = ++fake->order;
    TEST_ASSERT_EQUAL(FW_OPERATION_OWNER_UPLINK_OTA, token.owner);
    TEST_ASSERT_EQUAL_UINT32(42U, token.generation);
    fw_operation_token_t competing = {0};
    if (fake->adversarial_end_claim) {
        fake->all_owned_workers_resumed_before_release =
            fake->resume_scanner_calls[1] == 1 &&
            fake->resume_scanner_calls[0] == 1 &&
            fake->resume_http_calls == 1;
        fake->competing_claim_before_release = fw_operation_state_try_begin(
            &fake->operation_state, FW_OPERATION_OWNER_SCANNER_STAGING,
            &competing);
    }
    if (fake->operation_end_permanent_failure ||
        fake->operation_end_failures_remaining > 0) {
        if (fake->operation_end_failures_remaining > 0) {
            fake->operation_end_failures_remaining--;
        }
        return false;
    }
    bool release_uart = false;
    bool released = fw_operation_state_end(
        &fake->operation_state, token, &release_uart);
    TEST_ASSERT_FALSE(release_uart);
    if (fake->adversarial_end_claim && released) {
        fake->competing_claim_after_release = fw_operation_state_try_begin(
            &fake->operation_state, FW_OPERATION_OWNER_SCANNER_STAGING,
            &competing);
        if (fake->competing_claim_after_release) {
            TEST_ASSERT_TRUE(fw_operation_state_end(
                &fake->operation_state, competing, &release_uart));
        }
    }
    return released;
}

static bool fake_pause_http(void *context, bool *owned)
{
    fake_ota_t *fake = context;
    fake->pause_http_calls++;
    *owned = fake->pause_http_owned;
    return fake->fail != FAIL_PAUSE_HTTP;
}

static bool fake_pause_scanner(void *context, uint8_t slot, bool *owned)
{
    fake_ota_t *fake = context;
    fake->pause_scanner_calls[slot]++;
    *owned = fake->pause_scanner_owned[slot];
    return fake->fail != (slot == 0U
        ? FAIL_PAUSE_SCANNER_0 : FAIL_PAUSE_SCANNER_1);
}

static void fake_resume_http(void *context)
{
    fake_ota_t *fake = context;
    fake->resume_http_calls++;
    fake->resume_http_order = ++fake->order;
}

static void fake_resume_scanner(void *context, uint8_t slot)
{
    fake_ota_t *fake = context;
    fake->resume_scanner_calls[slot]++;
    fake->resume_scanner_order[slot] = ++fake->order;
}

static bool fake_integrity_start(void *context)
{
    fake_ota_t *fake = context;
    fake->integrity_start_calls++;
    return fake->fail != FAIL_INTEGRITY_START;
}

static bool fake_integrity_update(void *context, const uint8_t *bytes,
                                  size_t length, uint32_t *crc32)
{
    fake_ota_t *fake = context;
    (void)bytes;
    if (fake->fail == FAIL_INTEGRITY_UPDATE) {
        return false;
    }
    *crc32 += (uint32_t)length;
    return true;
}

static bool fake_integrity_finish(
    void *context, uint8_t digest[FOF_FIRMWARE_SHA256_SIZE])
{
    fake_ota_t *fake = context;
    if (fake->fail == FAIL_INTEGRITY_FINISH) {
        return false;
    }
    memset(digest, 0x11, FOF_FIRMWARE_SHA256_SIZE);
    return true;
}

static void fake_integrity_abort(void *context)
{
    fake_ota_t *fake = context;
    fake->integrity_abort_calls++;
}

static uplink_usb_ota_hooks_t fake_hooks(void)
{
    return (uplink_usb_ota_hooks_t) {
        .context = &s_fake,
        .get_running = fake_get_running,
        .get_next = fake_get_next,
        .get_image_state = fake_get_state,
        .get_partition_identity = fake_get_identity,
        .ota_begin = fake_ota_begin,
        .ota_write = fake_ota_write,
        .ota_end = fake_ota_handle,
        .ota_abort = fake_ota_abort,
        .set_boot_partition = fake_set_boot,
        .operation_begin = fake_operation_begin,
        .operation_end = fake_operation_end,
        .pause_http = fake_pause_http,
        .pause_scanner = fake_pause_scanner,
        .resume_http = fake_resume_http,
        .resume_scanner = fake_resume_scanner,
        .integrity_start = fake_integrity_start,
        .integrity_update = fake_integrity_update,
        .integrity_finish = fake_integrity_finish,
        .integrity_abort = fake_integrity_abort,
    };
}

static void reset_fixture(void)
{
    memset(&s_fake, 0, sizeof(s_fake));
    s_fake.running = partition_fixture(
        1U, "ota_0", APP_TYPE, OTA0_SUBTYPE, 0x20000U, 0x200000U);
    s_fake.next = partition_fixture(
        2U, "ota_1", APP_TYPE, OTA1_SUBTYPE, 0x220000U, 0x200000U);
    s_fake.image_state = UPLINK_USB_OTA_IMAGE_STATE_VALID;
    s_fake.pause_http_owned = true;
    s_fake.pause_scanner_owned[0] = true;
    s_fake.pause_scanner_owned[1] = true;
    fw_operation_state_init(&s_fake.operation_state);
    s_fake.operation_state.generation = 41U;
    strcpy(s_fake.running_identity.project, UPLINK_OTA_PROJECT);
    strcpy(s_fake.running_identity.version, "0.64.68");
    strcpy(s_fake.target_identity.project, UPLINK_OTA_PROJECT);
    strcpy(s_fake.target_identity.version, "0.64.69");
    uplink_usb_ota_test_reset();
    uplink_usb_ota_hooks_t hooks = fake_hooks();
    TEST_ASSERT_TRUE(uplink_usb_ota_test_install_hooks(&hooks));
}

void test_uplink_usb_ota_adapter_starts_idle(void)
{
    uplink_usb_ota_status_t status = {0};

    uplink_usb_ota_get_status(&status);

    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_IDLE, status.state);
    TEST_ASSERT_EQUAL_UINT32(0U, uplink_usb_ota_remaining());
}

void test_uplink_usb_ota_state_names_are_stable_and_invalid_fails_closed(void)
{
    TEST_ASSERT_EQUAL_STRING("idle",
                             uplink_usb_ota_state_name(UPLINK_USB_OTA_IDLE));
    TEST_ASSERT_EQUAL_STRING("preparing",
                             uplink_usb_ota_state_name(UPLINK_USB_OTA_PREPARING));
    TEST_ASSERT_EQUAL_STRING("receiving",
                             uplink_usb_ota_state_name(UPLINK_USB_OTA_RECEIVING));
    TEST_ASSERT_EQUAL_STRING("verifying",
                             uplink_usb_ota_state_name(UPLINK_USB_OTA_VERIFYING));
    TEST_ASSERT_EQUAL_STRING("committed",
                             uplink_usb_ota_state_name(UPLINK_USB_OTA_COMMITTED));
    TEST_ASSERT_EQUAL_STRING("error",
                             uplink_usb_ota_state_name(UPLINK_USB_OTA_ERROR));
    TEST_ASSERT_EQUAL_STRING(
        "error", uplink_usb_ota_state_name((uplink_usb_ota_state_t)999));
}

void test_uplink_usb_ota_mutator_contention_is_nonblocking_and_touch_free(void)
{
    reset_fixture();
    uplink_ota_manifest_t manifest = manifest_fixture();
    uplink_usb_ota_result_t result = {0};
    uint8_t byte = 0xE9U;
    uplink_usb_ota_test_set_mutator_busy(true);

    TEST_ASSERT_FALSE(uplink_usb_ota_begin(&manifest, &result));
    TEST_ASSERT_EQUAL_STRING("adapter_busy", result.error);
    TEST_ASSERT_TRUE(result.retryable);
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_PHASE_NONE, result.phase);
    TEST_ASSERT_EQUAL_INT(0, s_fake.operation_begin_calls);
    TEST_ASSERT_FALSE(uplink_usb_ota_write(&byte, 1U, 1U, &result));
    TEST_ASSERT_EQUAL_STRING("adapter_busy", result.error);
    TEST_ASSERT_FALSE(uplink_usb_ota_finish(0U, &result));
    TEST_ASSERT_EQUAL_STRING("adapter_busy", result.error);
    TEST_ASSERT_FALSE(uplink_usb_ota_abort("busy", &result));
    TEST_ASSERT_EQUAL_STRING("adapter_busy", result.error);
    TEST_ASSERT_TRUE(result.retryable);
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_PHASE_NONE, result.phase);
    TEST_ASSERT_EQUAL_INT(0, s_fake.operation_end_calls);
    TEST_ASSERT_EQUAL_INT(0, s_fake.ota_abort_calls);

    uplink_usb_ota_test_set_mutator_busy(false);
    TEST_ASSERT_TRUE(uplink_usb_ota_begin(&manifest, &result));
    uplink_usb_ota_status_t receiving = {0};
    TEST_ASSERT_TRUE(uplink_usb_ota_get_status(&receiving));
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_RECEIVING, receiving.state);
    uplink_usb_ota_test_set_mutator_busy(true);
    TEST_ASSERT_FALSE(uplink_usb_ota_write(&byte, 1U, 1U, &result));
    TEST_ASSERT_EQUAL_STRING("adapter_busy", result.error);
    TEST_ASSERT_TRUE(result.retryable);
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_PHASE_NONE, result.phase);
    TEST_ASSERT_EQUAL_INT(0, s_fake.operation_end_calls);
    TEST_ASSERT_EQUAL_INT(0, s_fake.ota_abort_calls);
    TEST_ASSERT_TRUE(uplink_usb_ota_get_status(&receiving));
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_RECEIVING, receiving.state);
    TEST_ASSERT_EQUAL_UINT32(0U, receiving.received);
    uplink_usb_ota_test_set_mutator_busy(false);
    TEST_ASSERT_TRUE(uplink_usb_ota_write(&byte, 1U, 1U, &result));
    TEST_ASSERT_TRUE(result.ok);
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_PHASE_PROGRESS, result.phase);
    TEST_ASSERT_FALSE(uplink_usb_ota_abort("cleanup", &result));
    TEST_ASSERT_EQUAL_INT(1, s_fake.operation_end_calls);
    TEST_ASSERT_EQUAL_INT(1, s_fake.ota_abort_calls);
}

void test_uplink_usb_ota_status_busy_is_bounded_and_remaining_fails_closed(void)
{
    reset_fixture();
    uplink_ota_manifest_t manifest = manifest_fixture();
    uplink_usb_ota_result_t result = {0};
    TEST_ASSERT_TRUE(uplink_usb_ota_begin(&manifest, &result));
    uplink_usb_ota_status_t status = {0};
    uplink_usb_ota_test_set_status_writer_busy(true);

    TEST_ASSERT_FALSE(uplink_usb_ota_get_status(&status));
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_ERROR, status.state);
    TEST_ASSERT_EQUAL_STRING("status_busy", status.last_error);
    TEST_ASSERT_EQUAL_UINT32(UPLINK_USB_OTA_REMAINING_UNKNOWN,
                             uplink_usb_ota_remaining());

    uplink_usb_ota_test_set_status_writer_busy(false);
    TEST_ASSERT_TRUE(uplink_usb_ota_get_status(&status));
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_RECEIVING, status.state);
    TEST_ASSERT_EQUAL_UINT32(1024U, uplink_usb_ota_remaining());
    TEST_ASSERT_FALSE(uplink_usb_ota_abort("cleanup", &result));
}

void test_uplink_usb_ota_rejects_live_pending_verify_before_partition_selection(void)
{
    reset_fixture();
    s_fake.image_state = UPLINK_USB_OTA_IMAGE_STATE_PENDING_VERIFY;
    uplink_ota_manifest_t manifest = manifest_fixture();
    uplink_usb_ota_result_t result = {0};

    TEST_ASSERT_FALSE(uplink_usb_ota_begin(&manifest, &result));

    TEST_ASSERT_EQUAL_STRING("pending_verify", result.error);
    TEST_ASSERT_EQUAL_INT(1, s_fake.operation_begin_calls);
    TEST_ASSERT_EQUAL_INT(1, s_fake.operation_end_calls);
    TEST_ASSERT_EQUAL_INT(1, s_fake.get_running_calls);
    TEST_ASSERT_EQUAL_INT(1, s_fake.get_state_calls);
    TEST_ASSERT_EQUAL_INT(0, s_fake.get_next_calls);
    TEST_ASSERT_EQUAL_INT(0, s_fake.pause_http_calls);
    TEST_ASSERT_EQUAL_INT(0, s_fake.ota_begin_calls);
}

void test_uplink_usb_ota_structurally_rejects_scanner_data_partition(void)
{
    reset_fixture();
    s_fake.next = partition_fixture(
        3U, "fw_scanner_s3", DATA_TYPE, 0x40U, 0x420000U, 0x200000U);
    uplink_ota_manifest_t manifest = manifest_fixture();
    uplink_usb_ota_result_t result = {0};

    TEST_ASSERT_FALSE(uplink_usb_ota_begin(&manifest, &result));

    TEST_ASSERT_EQUAL_STRING("invalid_target_partition", result.error);
    TEST_ASSERT_EQUAL_INT(1, s_fake.operation_begin_calls);
    TEST_ASSERT_EQUAL_INT(1, s_fake.operation_end_calls);
    TEST_ASSERT_EQUAL_INT(0, s_fake.pause_http_calls);
    TEST_ASSERT_EQUAL_INT(0, s_fake.ota_begin_calls);
}

void test_uplink_usb_ota_rejects_unterminated_hook_partition_label_safely(void)
{
    reset_fixture();
    memset(s_fake.next.label, 'X', sizeof(s_fake.next.label));
    uplink_ota_manifest_t manifest = manifest_fixture();
    uplink_usb_ota_result_t result = {0};

    TEST_ASSERT_FALSE(uplink_usb_ota_begin(&manifest, &result));
    TEST_ASSERT_EQUAL_STRING("invalid_target_partition", result.error);
    TEST_ASSERT_EQUAL_STRING("", result.partition);
    TEST_ASSERT_EQUAL_INT(0, s_fake.ota_begin_calls);
}

void test_uplink_usb_ota_rejects_wrong_or_same_ota_slot_geometry(void)
{
    const uplink_usb_ota_partition_t invalid[] = {
        { .native_id = 2U, .label = "ota_1", .type = DATA_TYPE,
          .subtype = OTA1_SUBTYPE, .offset = 0x220000U, .size = 0x200000U },
        { .native_id = 2U, .label = "ota_1", .type = APP_TYPE,
          .subtype = OTA0_SUBTYPE, .offset = 0x220000U, .size = 0x200000U },
        { .native_id = 2U, .label = "ota_1", .type = APP_TYPE,
          .subtype = OTA1_SUBTYPE, .offset = 0x230000U, .size = 0x200000U },
        { .native_id = 2U, .label = "ota_1", .type = APP_TYPE,
          .subtype = OTA1_SUBTYPE, .offset = 0x220000U, .size = 0x1FFFFFU },
        { .native_id = 1U, .label = "ota_0", .type = APP_TYPE,
          .subtype = OTA0_SUBTYPE, .offset = 0x20000U, .size = 0x200000U },
    };

    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        reset_fixture();
        s_fake.next = invalid[i];
        uplink_ota_manifest_t manifest = manifest_fixture();
        uplink_usb_ota_result_t result = {0};
        TEST_ASSERT_FALSE(uplink_usb_ota_begin(&manifest, &result));
        TEST_ASSERT_EQUAL_STRING("invalid_target_partition", result.error);
        TEST_ASSERT_EQUAL_INT(0, s_fake.ota_begin_calls);
    }
}

void test_uplink_usb_ota_begin_pauses_then_returns_typed_ready_credit(void)
{
    reset_fixture();
    uplink_ota_manifest_t manifest = manifest_fixture();
    uplink_usb_ota_result_t result = {0};

    TEST_ASSERT_TRUE(uplink_usb_ota_begin(&manifest, &result));

    TEST_ASSERT_TRUE(result.ok);
    TEST_ASSERT_TRUE(result.emit_required);
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_PHASE_READY, result.phase);
    TEST_ASSERT_EQUAL_STRING("ota_1", result.partition);
    TEST_ASSERT_EQUAL_UINT32(0U, result.received);
    TEST_ASSERT_EQUAL_UINT32(1024U, result.total);
    TEST_ASSERT_EQUAL_UINT32(1024U, result.credit_bytes);
    TEST_ASSERT_EQUAL_INT(1, s_fake.pause_http_calls);
    TEST_ASSERT_EQUAL_INT(1, s_fake.pause_scanner_calls[0]);
    TEST_ASSERT_EQUAL_INT(1, s_fake.pause_scanner_calls[1]);
    TEST_ASSERT_EQUAL_INT(1, s_fake.ota_begin_calls);
    TEST_ASSERT_EQUAL_INT(1, s_fake.integrity_start_calls);
}

void test_uplink_usb_ota_coordinator_accepts_every_split_descriptor_fragment(void)
{
    uint8_t image[1024];
    image_fixture(image);

    /* Exhaustive coverage includes the observed 100 + 44 byte split. */
    for (size_t first = 1U; first < TEST_DESCRIPTOR_BYTES; ++first) {
        reset_fixture();
        uplink_ota_manifest_t manifest = manifest_fixture();
        uplink_usb_ota_result_t result = {0};
        badge_usb_uplink_ota_flow_t flow;
        badge_usb_uplink_ota_flow_init(&flow);

        TEST_ASSERT_TRUE(uplink_usb_ota_begin(&manifest, &result));
        TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_WAIT_RECEIPT,
                          badge_usb_uplink_ota_flow_begin_result(&flow, &result));
        TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_CONTINUE,
                          badge_usb_uplink_ota_flow_receipt_result(&flow, true));

        TEST_ASSERT_TRUE(uplink_usb_ota_write(image, first, (uint32_t)first,
                                              &result));
        TEST_ASSERT_EQUAL_UINT32(0U, result.received);
        TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_CONTINUE,
                          badge_usb_uplink_ota_flow_write_result(
                              &flow, first, true, &result));
        TEST_ASSERT_EQUAL_UINT32((uint32_t)first, flow.transport_received);
        TEST_ASSERT_EQUAL_UINT32(0U, flow.durable_received);

        size_t second = TEST_DESCRIPTOR_BYTES - first;
        TEST_ASSERT_TRUE(uplink_usb_ota_write(image + first, second,
                                              TEST_DESCRIPTOR_BYTES,
                                              &result));
        TEST_ASSERT_EQUAL_UINT32(TEST_DESCRIPTOR_BYTES,
                                 result.received);
        TEST_ASSERT_EQUAL(BADGE_USB_UPLINK_ACTION_CONTINUE,
                          badge_usb_uplink_ota_flow_write_result(
                              &flow, second, true, &result));
        TEST_ASSERT_EQUAL_UINT32(TEST_DESCRIPTOR_BYTES,
                                 flow.transport_received);
        TEST_ASSERT_EQUAL_UINT32(TEST_DESCRIPTOR_BYTES,
                                 flow.durable_received);

        TEST_ASSERT_FALSE(uplink_usb_ota_abort("fragment_test", &result));
    }
}

void test_uplink_usb_ota_operation_busy_remains_idle_and_retryable(void)
{
    reset_fixture();
    uplink_ota_manifest_t manifest = manifest_fixture();
    uplink_usb_ota_result_t busy = {0};
    fw_operation_token_t competing = {0};
    TEST_ASSERT_TRUE(fw_operation_state_try_begin(
        &s_fake.operation_state, FW_OPERATION_OWNER_SCANNER_STAGING,
        &competing));

    TEST_ASSERT_FALSE(uplink_usb_ota_begin(&manifest, &busy));
    TEST_ASSERT_EQUAL_STRING("operation_active", busy.error);
    TEST_ASSERT_TRUE(busy.emit_required);
    TEST_ASSERT_TRUE(busy.retryable);
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_PHASE_NONE, busy.phase);
    TEST_ASSERT_EQUAL_INT(0, s_fake.operation_end_calls);
    TEST_ASSERT_EQUAL_INT(0, s_fake.pause_http_calls);
    TEST_ASSERT_TRUE(s_fake.operation_state.active);
    TEST_ASSERT_EQUAL(FW_OPERATION_OWNER_SCANNER_STAGING,
                      s_fake.operation_state.owner);
    uplink_usb_ota_status_t status = {0};
    uplink_usb_ota_get_status(&status);
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_IDLE, status.state);

    bool release_uart = false;
    TEST_ASSERT_TRUE(fw_operation_state_end(
        &s_fake.operation_state, competing, &release_uart));
    TEST_ASSERT_FALSE(release_uart);
    uplink_usb_ota_result_t retry = {0};
    TEST_ASSERT_TRUE(uplink_usb_ota_begin(&manifest, &retry));
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_PHASE_READY, retry.phase);
    TEST_ASSERT_EQUAL_INT(2, s_fake.operation_begin_calls);
}

void test_uplink_usb_ota_duplicate_begin_preserves_receiving_session(void)
{
    reset_fixture();
    uplink_ota_manifest_t manifest = manifest_fixture();
    memset(manifest.sha256, 'A', FOF_FIRMWARE_SHA256_HEX_LENGTH);
    manifest.sha256[FOF_FIRMWARE_SHA256_HEX_LENGTH] = '\0';
    uplink_usb_ota_result_t result = {0};
    TEST_ASSERT_TRUE(uplink_usb_ota_begin(&manifest, &result));
    int operation_begins = s_fake.operation_begin_calls;

    TEST_ASSERT_FALSE(uplink_usb_ota_begin(&manifest, &result));
    TEST_ASSERT_EQUAL_STRING("invalid_state", result.error);
    TEST_ASSERT_TRUE(result.emit_required);
    TEST_ASSERT_TRUE(result.retryable);
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_PHASE_NONE, result.phase);
    TEST_ASSERT_EQUAL_INT(operation_begins, s_fake.operation_begin_calls);
    TEST_ASSERT_EQUAL_INT(0, s_fake.operation_end_calls);
    TEST_ASSERT_EQUAL_INT(0, s_fake.ota_abort_calls);
    TEST_ASSERT_TRUE(s_fake.operation_state.active);
    TEST_ASSERT_EQUAL(FW_OPERATION_OWNER_UPLINK_OTA,
                      s_fake.operation_state.owner);

    uplink_usb_ota_status_t status = {0};
    TEST_ASSERT_TRUE(uplink_usb_ota_get_status(&status));
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_RECEIVING, status.state);
    TEST_ASSERT_EQUAL_UINT32(0U, status.received);
    TEST_ASSERT_EQUAL_UINT32(manifest.size, status.total);
    TEST_ASSERT_EQUAL_STRING(manifest.version, status.target_version);
    TEST_ASSERT_EQUAL_STRING("ota_1", status.partition);
    char canonical_sha256[FOF_FIRMWARE_SHA256_HEX_SIZE];
    memset(canonical_sha256, 'a', FOF_FIRMWARE_SHA256_HEX_LENGTH);
    canonical_sha256[FOF_FIRMWARE_SHA256_HEX_LENGTH] = '\0';
    TEST_ASSERT_EQUAL_STRING(canonical_sha256, status.target_sha256);

    TEST_ASSERT_FALSE(uplink_usb_ota_abort("cleanup", &result));
    TEST_ASSERT_TRUE(result.emit_required);
    TEST_ASSERT_FALSE(result.retryable);
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_PHASE_ABORTED, result.phase);
    TEST_ASSERT_FALSE(s_fake.operation_state.active);
    TEST_ASSERT_EQUAL_INT(1, s_fake.operation_end_calls);
    TEST_ASSERT_EQUAL_INT(1, s_fake.ota_abort_calls);
}

void test_uplink_usb_ota_streams_split_descriptor_then_bounded_chunks(void)
{
    reset_fixture();
    uplink_ota_manifest_t manifest = manifest_fixture();
    uplink_usb_ota_result_t result = {0};
    uint8_t image[1024];
    image_fixture(image);
    TEST_ASSERT_TRUE(uplink_usb_ota_begin(&manifest, &result));

    TEST_ASSERT_TRUE(uplink_usb_ota_write(image, 100U, 100U, &result));
    TEST_ASSERT_EQUAL_INT(0, s_fake.ota_write_calls);
    TEST_ASSERT_TRUE(uplink_usb_ota_write(image + 100U, 44U, 144U,
                                          &result));
    TEST_ASSERT_EQUAL_INT(1, s_fake.ota_write_calls);
    TEST_ASSERT_EQUAL_UINT32(144U, s_fake.ota_write_lengths[0]);
    TEST_ASSERT_TRUE(uplink_usb_ota_write(image + 144U, 512U, 656U,
                                          &result));
    TEST_ASSERT_TRUE(uplink_usb_ota_write(image + 656U, 368U, 1024U,
                                          &result));
    TEST_ASSERT_EQUAL_INT(3, s_fake.ota_write_calls);
    TEST_ASSERT_EQUAL_UINT32(1024U, s_fake.ota_written);
    TEST_ASSERT_EQUAL_UINT32(1024U, result.received);
    TEST_ASSERT_EQUAL_UINT32(0U, uplink_usb_ota_remaining());
}

void test_uplink_usb_ota_invalid_early_identity_never_writes_flash(void)
{
    reset_fixture();
    uplink_ota_manifest_t manifest = manifest_fixture();
    uplink_usb_ota_result_t result = {0};
    uint8_t image[1024];
    image_fixture(image);
    image[0x50] = 'x';
    TEST_ASSERT_TRUE(uplink_usb_ota_begin(&manifest, &result));
    TEST_ASSERT_TRUE(uplink_usb_ota_write(image, 100U, 100U, &result));

    TEST_ASSERT_FALSE(uplink_usb_ota_write(image + 100U, 44U, 144U,
                                           &result));
    TEST_ASSERT_EQUAL_STRING("project_mismatch", result.error);
    TEST_ASSERT_EQUAL_INT(0, s_fake.ota_write_calls);
    TEST_ASSERT_EQUAL_INT(1, s_fake.ota_abort_calls);
}

void test_uplink_usb_ota_rejects_oversize_and_cumulative_drift_before_write(void)
{
    reset_fixture();
    uplink_ota_manifest_t manifest = manifest_fixture();
    uplink_usb_ota_result_t result = {0};
    uint8_t image[1024];
    image_fixture(image);
    TEST_ASSERT_TRUE(uplink_usb_ota_begin(&manifest, &result));

    TEST_ASSERT_FALSE(uplink_usb_ota_write(image, 513U, 513U, &result));
    TEST_ASSERT_EQUAL_STRING("write_too_large", result.error);
    TEST_ASSERT_EQUAL_INT(0, s_fake.ota_write_calls);

    reset_fixture();
    manifest = manifest_fixture();
    TEST_ASSERT_TRUE(uplink_usb_ota_begin(&manifest, &result));
    TEST_ASSERT_FALSE(uplink_usb_ota_write(image, 100U, 99U, &result));
    TEST_ASSERT_EQUAL_STRING("transport_mismatch", result.error);
    TEST_ASSERT_EQUAL_INT(0, s_fake.ota_write_calls);
}

void test_uplink_usb_ota_credit_overshoot_is_rejected_before_flash_write(void)
{
    reset_fixture();
    uplink_ota_manifest_t manifest = manifest_fixture();
    manifest.size = 5000U;
    uplink_usb_ota_result_t result = {0};
    uint8_t image[5000];
    memset(image, 0xA5, sizeof(image));
    uint8_t prefix_image[1024];
    image_fixture(prefix_image);
    memcpy(image, prefix_image, sizeof(prefix_image));
    TEST_ASSERT_TRUE(uplink_usb_ota_begin(&manifest, &result));
    for (uint32_t cumulative = 500U; cumulative <= 4000U;
         cumulative += 500U) {
        TEST_ASSERT_TRUE(uplink_usb_ota_write(
            image + cumulative - 500U, 500U, cumulative, &result));
    }
    int writes_before = s_fake.ota_write_calls;
    TEST_ASSERT_FALSE(uplink_usb_ota_write(image + 4000U, 97U, 4097U,
                                           &result));
    TEST_ASSERT_EQUAL_STRING("credit_overshoot", result.error);
    TEST_ASSERT_EQUAL_INT(writes_before, s_fake.ota_write_calls);
    TEST_ASSERT_EQUAL_UINT32(4000U, s_fake.ota_written);
}

void test_uplink_usb_ota_active_null_arguments_cleanup_without_leaks(void)
{
    uplink_ota_manifest_t manifest = manifest_fixture();
    uplink_usb_ota_result_t result = {0};
    uint8_t image[1024];
    image_fixture(image);

    reset_fixture();
    TEST_ASSERT_TRUE(uplink_usb_ota_begin(&manifest, &result));
    TEST_ASSERT_FALSE(uplink_usb_ota_write(NULL, 1U, 1U, &result));
    TEST_ASSERT_EQUAL_INT(1, s_fake.ota_abort_calls);
    TEST_ASSERT_EQUAL_INT(1, s_fake.operation_end_calls);

    reset_fixture();
    TEST_ASSERT_TRUE(uplink_usb_ota_begin(&manifest, &result));
    TEST_ASSERT_FALSE(uplink_usb_ota_write(image, 1U, 1U, NULL));
    TEST_ASSERT_EQUAL_INT(1, s_fake.ota_abort_calls);
    TEST_ASSERT_EQUAL_INT(1, s_fake.operation_end_calls);

    reset_fixture();
    TEST_ASSERT_TRUE(uplink_usb_ota_begin(&manifest, &result));
    receive_complete_image(image, &result);
    TEST_ASSERT_FALSE(uplink_usb_ota_finish(1024U, NULL));
    TEST_ASSERT_EQUAL_INT(1, s_fake.ota_abort_calls);
    TEST_ASSERT_EQUAL_INT(1, s_fake.operation_end_calls);
}

static void receive_complete_image(uint8_t image[1024],
                                   uplink_usb_ota_result_t *result)
{
    TEST_ASSERT_TRUE(uplink_usb_ota_write(image, 512U, 512U, result));
    TEST_ASSERT_TRUE(uplink_usb_ota_write(image + 512U, 512U, 1024U,
                                          result));
}

void test_uplink_usb_ota_finish_revalidates_then_latches_committed(void)
{
    reset_fixture();
    uplink_ota_manifest_t manifest = manifest_fixture();
    uplink_usb_ota_result_t result = {0};
    uint8_t image[1024];
    image_fixture(image);
    TEST_ASSERT_TRUE(uplink_usb_ota_begin(&manifest, &result));
    receive_complete_image(image, &result);

    TEST_ASSERT_TRUE(uplink_usb_ota_finish(1024U, &result));
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_PHASE_COMMITTED, result.phase);
    TEST_ASSERT_TRUE(result.reboot_required);
    TEST_ASSERT_EQUAL_INT(1, s_fake.ota_end_calls);
    TEST_ASSERT_EQUAL_INT(1, s_fake.set_boot_calls);
    TEST_ASSERT_EQUAL_INT(2, s_fake.get_running_calls);
    TEST_ASSERT_EQUAL_INT(2, s_fake.get_next_calls);
    TEST_ASSERT_EQUAL_INT(2, s_fake.get_identity_calls);
    TEST_ASSERT_EQUAL_INT(0, s_fake.operation_end_calls);
    uplink_usb_ota_status_t committed_status = {0};
    TEST_ASSERT_TRUE(uplink_usb_ota_get_status(&committed_status));
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_COMMITTED, committed_status.state);
    TEST_ASSERT_EQUAL_UINT32(0U, uplink_usb_ota_remaining());

    int writes = s_fake.ota_write_calls;
    TEST_ASSERT_FALSE(uplink_usb_ota_abort("late", &result));
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_PHASE_COMMITTED, result.phase);
    TEST_ASSERT_TRUE(result.reboot_required);
    TEST_ASSERT_FALSE(uplink_usb_ota_write(image, 1U, 1025U, &result));
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_PHASE_COMMITTED, result.phase);
    TEST_ASSERT_TRUE(result.reboot_required);
    TEST_ASSERT_FALSE(uplink_usb_ota_finish(1024U, &result));
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_PHASE_COMMITTED, result.phase);
    TEST_ASSERT_TRUE(result.reboot_required);
    TEST_ASSERT_FALSE(uplink_usb_ota_begin(&manifest, &result));
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_PHASE_COMMITTED, result.phase);
    TEST_ASSERT_TRUE(result.reboot_required);
    TEST_ASSERT_EQUAL_INT(writes, s_fake.ota_write_calls);
    TEST_ASSERT_EQUAL_INT(1, s_fake.ota_end_calls);
    TEST_ASSERT_EQUAL_INT(1, s_fake.set_boot_calls);
    TEST_ASSERT_EQUAL_INT(0, s_fake.operation_end_calls);
    TEST_ASSERT_EQUAL_INT(0, s_fake.resume_http_calls);
}

void test_uplink_usb_ota_commits_from_ota1_to_exact_ota0_slot(void)
{
    reset_fixture();
    s_fake.running = partition_fixture(
        2U, "ota_1", APP_TYPE, OTA1_SUBTYPE, 0x220000U, 0x200000U);
    s_fake.next = partition_fixture(
        1U, "ota_0", APP_TYPE, OTA0_SUBTYPE, 0x20000U, 0x200000U);
    uplink_ota_manifest_t manifest = manifest_fixture();
    uplink_usb_ota_result_t result = {0};
    uint8_t image[1024];
    image_fixture(image);

    TEST_ASSERT_TRUE(uplink_usb_ota_begin(&manifest, &result));
    TEST_ASSERT_EQUAL_STRING("ota_0", result.partition);
    receive_complete_image(image, &result);
    TEST_ASSERT_TRUE(uplink_usb_ota_finish(1024U, &result));
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_PHASE_COMMITTED, result.phase);
    TEST_ASSERT_EQUAL_STRING("ota_0", result.partition);
    TEST_ASSERT_EQUAL_INT(1, s_fake.set_boot_calls);
}

void test_uplink_usb_ota_finish_fails_closed_on_post_end_partition_drift(void)
{
    reset_fixture();
    uplink_ota_manifest_t manifest = manifest_fixture();
    uplink_usb_ota_result_t result = {0};
    uint8_t image[1024];
    image_fixture(image);
    TEST_ASSERT_TRUE(uplink_usb_ota_begin(&manifest, &result));
    receive_complete_image(image, &result);
    s_fake.drift_after_end = true;

    TEST_ASSERT_FALSE(uplink_usb_ota_finish(1024U, &result));
    TEST_ASSERT_EQUAL_STRING("partition_changed", result.error);
    TEST_ASSERT_EQUAL_INT(0, s_fake.set_boot_calls);
    TEST_ASSERT_EQUAL_INT(0, s_fake.ota_abort_calls);
    TEST_ASSERT_EQUAL_INT(1, s_fake.operation_end_calls);
    TEST_ASSERT_EQUAL_INT(1, s_fake.resume_http_calls);
}

void test_uplink_usb_ota_finish_rejects_post_end_identity_and_set_boot_failure(void)
{
    reset_fixture();
    strcpy(s_fake.target_identity.version, "0.64.70");
    uplink_ota_manifest_t manifest = manifest_fixture();
    uplink_usb_ota_result_t result = {0};
    uint8_t image[1024];
    image_fixture(image);
    TEST_ASSERT_TRUE(uplink_usb_ota_begin(&manifest, &result));
    receive_complete_image(image, &result);
    TEST_ASSERT_FALSE(uplink_usb_ota_finish(1024U, &result));
    TEST_ASSERT_EQUAL_STRING("target_identity_mismatch", result.error);
    TEST_ASSERT_EQUAL_INT(0, s_fake.set_boot_calls);

    reset_fixture();
    s_fake.fail = FAIL_SET_BOOT;
    manifest = manifest_fixture();
    TEST_ASSERT_TRUE(uplink_usb_ota_begin(&manifest, &result));
    receive_complete_image(image, &result);
    TEST_ASSERT_FALSE(uplink_usb_ota_finish(1024U, &result));
    TEST_ASSERT_EQUAL_STRING("set_boot_failed", result.error);
    TEST_ASSERT_EQUAL_INT(1, s_fake.set_boot_calls);
    TEST_ASSERT_EQUAL_INT(1, s_fake.operation_end_calls);
}

void test_uplink_usb_ota_rejects_unterminated_post_end_identity_fields(void)
{
    uint8_t image[1024];
    image_fixture(image);
    for (int field = 0; field < 2; ++field) {
        reset_fixture();
        if (field == 0) {
            memset(s_fake.target_identity.project, 'P',
                   sizeof(s_fake.target_identity.project));
        } else {
            memset(s_fake.target_identity.version, 'V',
                   sizeof(s_fake.target_identity.version));
        }
        uplink_ota_manifest_t manifest = manifest_fixture();
        uplink_usb_ota_result_t result = {0};
        TEST_ASSERT_TRUE(uplink_usb_ota_begin(&manifest, &result));
        receive_complete_image(image, &result);

        TEST_ASSERT_FALSE(uplink_usb_ota_finish(1024U, &result));
        TEST_ASSERT_EQUAL_STRING("target_identity_unterminated",
                                 result.error);
        TEST_ASSERT_EQUAL_INT(0, s_fake.set_boot_calls);
        TEST_ASSERT_EQUAL_INT(1, s_fake.operation_end_calls);
    }
}

void test_uplink_usb_ota_flash_and_integrity_failures_cleanup_once(void)
{
    const fake_failure_t write_failures[] = {
        FAIL_OTA_WRITE,
        FAIL_INTEGRITY_UPDATE,
    };
    uint8_t image[1024];
    image_fixture(image);
    for (size_t i = 0U;
         i < sizeof(write_failures) / sizeof(write_failures[0]); ++i) {
        reset_fixture();
        uplink_ota_manifest_t manifest = manifest_fixture();
        uplink_usb_ota_result_t result = {0};
        TEST_ASSERT_TRUE(uplink_usb_ota_begin(&manifest, &result));
        s_fake.fail = write_failures[i];
        TEST_ASSERT_FALSE(uplink_usb_ota_write(image, 512U, 512U,
                                               &result));
        TEST_ASSERT_EQUAL_INT(1, s_fake.ota_abort_calls);
        TEST_ASSERT_EQUAL_INT(1, s_fake.operation_end_calls);
    }

    const fake_failure_t finish_failures[] = {
        FAIL_INTEGRITY_FINISH,
        FAIL_OTA_END,
    };
    for (size_t i = 0U;
         i < sizeof(finish_failures) / sizeof(finish_failures[0]); ++i) {
        reset_fixture();
        uplink_ota_manifest_t manifest = manifest_fixture();
        uplink_usb_ota_result_t result = {0};
        TEST_ASSERT_TRUE(uplink_usb_ota_begin(&manifest, &result));
        receive_complete_image(image, &result);
        s_fake.fail = finish_failures[i];
        TEST_ASSERT_FALSE(uplink_usb_ota_finish(1024U, &result));
        TEST_ASSERT_EQUAL_INT(finish_failures[i] == FAIL_OTA_END ? 0 : 1,
                              s_fake.ota_abort_calls);
        TEST_ASSERT_EQUAL_INT(1, s_fake.operation_end_calls);
        int released = s_fake.operation_end_calls;
        TEST_ASSERT_FALSE(uplink_usb_ota_abort("again", &result));
        TEST_ASSERT_EQUAL_INT(released, s_fake.operation_end_calls);
    }
}

void test_uplink_usb_ota_precommit_cleanup_is_owned_reverse_and_idempotent(void)
{
    const fake_failure_t failures[] = {
        FAIL_PAUSE_HTTP,
        FAIL_PAUSE_SCANNER_0,
        FAIL_PAUSE_SCANNER_1,
        FAIL_OTA_BEGIN,
        FAIL_INTEGRITY_START,
    };

    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        reset_fixture();
        s_fake.fail = failures[i];
        uplink_ota_manifest_t manifest = manifest_fixture();
        uplink_usb_ota_result_t first = {0};
        TEST_ASSERT_FALSE(uplink_usb_ota_begin(&manifest, &first));
        TEST_ASSERT_TRUE(first.emit_required);

        int operation_end = s_fake.operation_end_calls;
        int ota_abort = s_fake.ota_abort_calls;
        int resume_http = s_fake.resume_http_calls;
        int resume0 = s_fake.resume_scanner_calls[0];
        int resume1 = s_fake.resume_scanner_calls[1];
        int integrity_abort = s_fake.integrity_abort_calls;
        uplink_usb_ota_result_t repeated = {0};
        TEST_ASSERT_FALSE(uplink_usb_ota_abort("second", &repeated));
        TEST_ASSERT_FALSE(uplink_usb_ota_abort("third", &repeated));
        TEST_ASSERT_FALSE(repeated.emit_required);
        TEST_ASSERT_EQUAL_INT(operation_end, s_fake.operation_end_calls);
        TEST_ASSERT_EQUAL_INT(ota_abort, s_fake.ota_abort_calls);
        TEST_ASSERT_EQUAL_INT(resume_http, s_fake.resume_http_calls);
        TEST_ASSERT_EQUAL_INT(resume0, s_fake.resume_scanner_calls[0]);
        TEST_ASSERT_EQUAL_INT(resume1, s_fake.resume_scanner_calls[1]);
        TEST_ASSERT_EQUAL_INT(integrity_abort,
                              s_fake.integrity_abort_calls);
        if (s_fake.operation_end_calls > 0 &&
            s_fake.resume_http_calls > 0) {
            TEST_ASSERT_TRUE(s_fake.operation_release_order >
                             s_fake.resume_http_order);
        }
        if (s_fake.operation_end_calls > 0 &&
            s_fake.resume_scanner_calls[0] > 0) {
            TEST_ASSERT_TRUE(s_fake.operation_release_order >
                             s_fake.resume_scanner_order[0]);
        }
        if (s_fake.operation_end_calls > 0 &&
            s_fake.resume_scanner_calls[1] > 0) {
            TEST_ASSERT_TRUE(s_fake.operation_release_order >
                             s_fake.resume_scanner_order[1]);
        }
        if (s_fake.resume_scanner_calls[0] > 0 &&
            s_fake.resume_scanner_calls[1] > 0) {
            TEST_ASSERT_TRUE(s_fake.resume_scanner_order[1] <
                             s_fake.resume_scanner_order[0]);
        }
    }
}

void test_uplink_usb_ota_operation_end_cannot_compete_before_owned_resumes(void)
{
    reset_fixture();
    s_fake.adversarial_end_claim = true;
    uplink_ota_manifest_t manifest = manifest_fixture();
    uplink_usb_ota_result_t result = {0};

    TEST_ASSERT_TRUE(uplink_usb_ota_begin(&manifest, &result));
    TEST_ASSERT_FALSE(uplink_usb_ota_abort("adversarial", &result));
    TEST_ASSERT_FALSE(s_fake.competing_claim_before_release);
    TEST_ASSERT_TRUE(s_fake.all_owned_workers_resumed_before_release);
    TEST_ASSERT_TRUE(s_fake.competing_claim_after_release);
    TEST_ASSERT_TRUE(s_fake.resume_scanner_order[1] <
                     s_fake.resume_scanner_order[0]);
    TEST_ASSERT_TRUE(s_fake.resume_scanner_order[0] <
                     s_fake.resume_http_order);
    TEST_ASSERT_TRUE(s_fake.resume_http_order <
                     s_fake.operation_release_order);
}

void test_uplink_usb_ota_release_failure_retains_token_until_abort_retry(void)
{
    reset_fixture();
    uplink_ota_manifest_t manifest = manifest_fixture();
    uplink_usb_ota_result_t result = {0};
    TEST_ASSERT_TRUE(uplink_usb_ota_begin(&manifest, &result));
    s_fake.operation_end_failures_remaining = 1;

    TEST_ASSERT_FALSE(uplink_usb_ota_abort("original_failure", &result));
    TEST_ASSERT_EQUAL_STRING("operation_release_failed", result.error);
    TEST_ASSERT_TRUE(result.retryable);
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_PHASE_NONE, result.phase);
    TEST_ASSERT_EQUAL_INT(1, s_fake.operation_end_calls);
    TEST_ASSERT_EQUAL_INT(1, s_fake.resume_http_calls);
    TEST_ASSERT_EQUAL_INT(1, s_fake.resume_scanner_calls[0]);
    TEST_ASSERT_EQUAL_INT(1, s_fake.resume_scanner_calls[1]);
    uplink_usb_ota_status_t status = {0};
    uplink_usb_ota_get_status(&status);
    TEST_ASSERT_EQUAL_STRING("original_failure", status.last_error);

    int operation_begins = s_fake.operation_begin_calls;
    TEST_ASSERT_FALSE(uplink_usb_ota_begin(&manifest, &result));
    TEST_ASSERT_EQUAL_STRING("operation_release_failed", result.error);
    TEST_ASSERT_TRUE(result.emit_required);
    TEST_ASSERT_TRUE(result.retryable);
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_PHASE_NONE, result.phase);
    TEST_ASSERT_EQUAL_INT(operation_begins, s_fake.operation_begin_calls);
    TEST_ASSERT_EQUAL_INT(1, s_fake.operation_end_calls);
    TEST_ASSERT_EQUAL_INT(1, s_fake.ota_abort_calls);
    TEST_ASSERT_TRUE(s_fake.operation_state.active);
    TEST_ASSERT_EQUAL(FW_OPERATION_OWNER_UPLINK_OTA,
                      s_fake.operation_state.owner);
    TEST_ASSERT_TRUE(uplink_usb_ota_get_status(&status));
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_ERROR, status.state);

    TEST_ASSERT_FALSE(uplink_usb_ota_abort("different", &result));
    TEST_ASSERT_EQUAL_INT(2, s_fake.operation_end_calls);
    TEST_ASSERT_TRUE(result.emit_required);
    TEST_ASSERT_EQUAL(UPLINK_USB_OTA_PHASE_ABORTED, result.phase);
    TEST_ASSERT_EQUAL_STRING("original_failure", result.error);
    TEST_ASSERT_EQUAL_INT(1, s_fake.resume_http_calls);
    TEST_ASSERT_EQUAL_INT(1, s_fake.resume_scanner_calls[0]);
    TEST_ASSERT_EQUAL_INT(1, s_fake.resume_scanner_calls[1]);
    TEST_ASSERT_FALSE(uplink_usb_ota_abort("again", &result));
    TEST_ASSERT_FALSE(result.emit_required);
    TEST_ASSERT_EQUAL_INT(2, s_fake.operation_end_calls);
}

void test_uplink_usb_ota_permanent_release_failure_never_resumes_or_rebegins(void)
{
    reset_fixture();
    uplink_ota_manifest_t manifest = manifest_fixture();
    uplink_usb_ota_result_t result = {0};
    TEST_ASSERT_TRUE(uplink_usb_ota_begin(&manifest, &result));
    s_fake.operation_end_permanent_failure = true;

    for (int attempt = 1; attempt <= 3; ++attempt) {
        TEST_ASSERT_FALSE(uplink_usb_ota_abort("fatal", &result));
        TEST_ASSERT_EQUAL_STRING("operation_release_failed", result.error);
        TEST_ASSERT_TRUE(result.retryable);
        TEST_ASSERT_EQUAL(UPLINK_USB_OTA_PHASE_NONE, result.phase);
        TEST_ASSERT_EQUAL_INT(attempt, s_fake.operation_end_calls);
        TEST_ASSERT_EQUAL_INT(1, s_fake.resume_http_calls);
        TEST_ASSERT_EQUAL_INT(1, s_fake.resume_scanner_calls[0]);
        TEST_ASSERT_EQUAL_INT(1, s_fake.resume_scanner_calls[1]);
    }
    int operation_begins = s_fake.operation_begin_calls;
    TEST_ASSERT_FALSE(uplink_usb_ota_begin(&manifest, &result));
    TEST_ASSERT_EQUAL_INT(operation_begins, s_fake.operation_begin_calls);
}

void test_uplink_usb_ota_absent_or_prepaused_workers_are_not_borrowed(void)
{
    reset_fixture();
    s_fake.pause_http_owned = false;
    s_fake.pause_scanner_owned[0] = false;
    s_fake.pause_scanner_owned[1] = false;
    s_fake.fail = FAIL_PAUSE_SCANNER_1;
    uplink_ota_manifest_t manifest = manifest_fixture();
    uplink_usb_ota_result_t result = {0};

    TEST_ASSERT_FALSE(uplink_usb_ota_begin(&manifest, &result));

    TEST_ASSERT_EQUAL_INT(0, s_fake.resume_http_calls);
    TEST_ASSERT_EQUAL_INT(0, s_fake.resume_scanner_calls[0]);
    TEST_ASSERT_EQUAL_INT(0, s_fake.resume_scanner_calls[1]);
    TEST_ASSERT_EQUAL_INT(1, s_fake.operation_end_calls);
}
