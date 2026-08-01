#include "unity.h"

#include <string.h>

#include "fw_store.h"

#define SHA_UPPER \
    "ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789"
#define SHA_LOWER \
    "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"

static fw_store_scanner_stage_status_t receiving_status(void)
{
    fw_store_scanner_stage_status_t status = {
        .phase = FW_SCANNER_STAGE_RECEIVING,
        .generation = 0U,
        .size = 8192U,
        .received = 4096U,
        .slot_mask = FW_AUTO_UPDATE_SLOT_ALL,
    };
    strcpy(status.target, "scanner-s3-combo-fof_badge");
    strcpy(status.sha256, SHA_UPPER);
    return status;
}

static void assert_zero_idle(
    const fw_store_scanner_stage_status_t *status)
{
    TEST_ASSERT_NOT_NULL(status);
    TEST_ASSERT_EQUAL_INT(FW_SCANNER_STAGE_IDLE, status->phase);
    TEST_ASSERT_EQUAL_UINT32(0U, status->generation);
    TEST_ASSERT_EQUAL_UINT32(0U, status->size);
    TEST_ASSERT_EQUAL_UINT32(0U, status->received);
    TEST_ASSERT_EQUAL_UINT8(0U, status->slot_mask);
    TEST_ASSERT_EQUAL_CHAR('\0', status->target[0]);
    TEST_ASSERT_EQUAL_CHAR('\0', status->sha256[0]);
}

void test_fw_scanner_stage_receiving_preserves_exact_parser_progress(void)
{
    fw_store_scanner_stage_status_t live = receiving_status();
    fw_store_scanner_stage_status_t out = {0};

    TEST_ASSERT_TRUE(fw_store_scanner_stage_status_reconcile(
        true, &live, FW_STORE_READ_ERROR, NULL, &out));
    TEST_ASSERT_EQUAL_INT(FW_SCANNER_STAGE_RECEIVING, out.phase);
    TEST_ASSERT_EQUAL_STRING("scanner-s3-combo-fof_badge", out.target);
    TEST_ASSERT_EQUAL_STRING(SHA_LOWER, out.sha256);
    TEST_ASSERT_EQUAL_UINT32(8192U, out.size);
    TEST_ASSERT_EQUAL_UINT8(FW_AUTO_UPDATE_SLOT_ALL, out.slot_mask);
    TEST_ASSERT_EQUAL_UINT32(4096U, out.received);
    TEST_ASSERT_EQUAL_UINT32(0U, out.generation);
}

void test_fw_scanner_stage_durable_finalized_is_committed(void)
{
    fw_store_scanner_stage_status_t live = receiving_status();
    live.phase = FW_SCANNER_STAGE_COMMITTED;
    live.received = live.size;
    live.generation = 37U;
    fw_store_scanner_stage_status_t out = {0};

    TEST_ASSERT_TRUE(fw_store_scanner_stage_status_reconcile(
        true, &live, FW_STORE_READ_ERROR, NULL, &out));
    TEST_ASSERT_EQUAL_INT(FW_SCANNER_STAGE_COMMITTED, out.phase);
    TEST_ASSERT_EQUAL_STRING("scanner-s3-combo-fof_badge", out.target);
    TEST_ASSERT_EQUAL_STRING(SHA_LOWER, out.sha256);
    TEST_ASSERT_EQUAL_UINT32(8192U, out.size);
    TEST_ASSERT_EQUAL_UINT8(FW_AUTO_UPDATE_SLOT_ALL, out.slot_mask);
    TEST_ASSERT_EQUAL_UINT32(8192U, out.received);
    TEST_ASSERT_EQUAL_UINT32(37U, out.generation);
}

void test_fw_scanner_stage_committed_manifest_rehydrates_exact_stage(void)
{
    fw_store_info_t manifest = {
        .stored = true,
        .generation = 91U,
        .target_slot_mask = FW_AUTO_UPDATE_SLOT_WIFI,
        .size = 16384U,
    };
    strcpy(manifest.name, "scanner-s3-combo-fof_badge");
    strcpy(manifest.sha256, SHA_UPPER);
    fw_store_scanner_stage_status_t out = {0};

    TEST_ASSERT_TRUE(fw_store_scanner_stage_status_reconcile(
        true, NULL, FW_STORE_READ_COMMITTED, &manifest, &out));
    TEST_ASSERT_EQUAL_INT(FW_SCANNER_STAGE_COMMITTED, out.phase);
    TEST_ASSERT_EQUAL_STRING("scanner-s3-combo-fof_badge", out.target);
    TEST_ASSERT_EQUAL_STRING(SHA_LOWER, out.sha256);
    TEST_ASSERT_EQUAL_UINT32(16384U, out.size);
    TEST_ASSERT_EQUAL_UINT8(FW_AUTO_UPDATE_SLOT_WIFI, out.slot_mask);
    TEST_ASSERT_EQUAL_UINT32(16384U, out.received);
    TEST_ASSERT_EQUAL_UINT32(91U, out.generation);
}

void test_fw_scanner_stage_no_manifest_is_zero_idle(void)
{
    fw_store_scanner_stage_status_t out = {
        .phase = FW_SCANNER_STAGE_COMMITTED,
        .generation = UINT32_MAX,
        .size = UINT32_MAX,
        .received = UINT32_MAX,
        .slot_mask = UINT8_MAX,
        .target = "dirty",
        .sha256 = "dirty",
    };

    TEST_ASSERT_TRUE(fw_store_scanner_stage_status_reconcile(
        true, NULL, FW_STORE_READ_NO_MANIFEST, NULL, &out));
    assert_zero_idle(&out);
}

void test_fw_scanner_stage_ambiguity_and_inconsistency_fail_closed(void)
{
    fw_store_scanner_stage_status_t live = receiving_status();
    fw_store_scanner_stage_status_t out = {
        .phase = FW_SCANNER_STAGE_COMMITTED,
        .generation = UINT32_MAX,
        .size = UINT32_MAX,
        .received = UINT32_MAX,
        .slot_mask = UINT8_MAX,
        .target = "dirty",
        .sha256 = "dirty",
    };

    TEST_ASSERT_FALSE(fw_store_scanner_stage_status_reconcile(
        false, NULL, FW_STORE_READ_NO_MANIFEST, NULL, &out));
    assert_zero_idle(&out);

    TEST_ASSERT_FALSE(fw_store_scanner_stage_status_reconcile(
        true, NULL, FW_STORE_READ_ERROR, NULL, &out));
    assert_zero_idle(&out);

    live.generation = 1U;
    TEST_ASSERT_FALSE(fw_store_scanner_stage_status_reconcile(
        true, &live, FW_STORE_READ_ERROR, NULL, &out));
    assert_zero_idle(&out);

    live = receiving_status();
    live.received = live.size + 1U;
    TEST_ASSERT_FALSE(fw_store_scanner_stage_status_reconcile(
        true, &live, FW_STORE_READ_ERROR, NULL, &out));
    assert_zero_idle(&out);

    live = receiving_status();
    live.received = live.size;
    TEST_ASSERT_FALSE(fw_store_scanner_stage_status_reconcile(
        true, &live, FW_STORE_READ_ERROR, NULL, &out));
    assert_zero_idle(&out);

    live = receiving_status();
    memset(live.target, 'X', sizeof(live.target));
    TEST_ASSERT_FALSE(fw_store_scanner_stage_status_reconcile(
        true, &live, FW_STORE_READ_ERROR, NULL, &out));
    assert_zero_idle(&out);

    fw_store_info_t manifest = {
        .stored = true,
        .generation = 0U,
        .target_slot_mask = FW_AUTO_UPDATE_SLOT_BLE,
        .size = 1024U,
    };
    strcpy(manifest.name, "scanner-s3-combo-fof_badge");
    strcpy(manifest.sha256, SHA_LOWER);
    TEST_ASSERT_FALSE(fw_store_scanner_stage_status_reconcile(
        true, NULL, FW_STORE_READ_COMMITTED, &manifest, &out));
    assert_zero_idle(&out);

    TEST_ASSERT_FALSE(fw_store_scanner_stage_status_reconcile(
        true, NULL, FW_STORE_READ_COMMITTED, NULL, &out));
    assert_zero_idle(&out);

    TEST_ASSERT_FALSE(fw_store_scanner_stage_status_reconcile(
        true, &live, FW_STORE_READ_ERROR, NULL, NULL));
}
