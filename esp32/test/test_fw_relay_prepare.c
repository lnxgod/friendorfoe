#include "unity.h"

#include "fw_manifest_store.h"
#include "fw_relay_prepare_adapter.h"

#include <stdint.h>
#include <string.h>

typedef struct {
    bool present_valid;
    bool present_generation;
    bool present_crc;
    uint32_t valid;
    uint32_t generation;
    uint32_t manifest_crc32;
    uint32_t size;
    int open_calls;
    int get_calls;
    int set_calls;
    int close_calls;
    int fail_open;
    int fail_get_call;
    int fail_set_call;
} manifest_model_t;

static manifest_model_t s_nvs;

static fw_manifest_io_result_t model_open(
    const char *namespace_name, bool readwrite,
    fw_manifest_handle_t *out_handle)
{
    s_nvs.open_calls++;
    TEST_ASSERT_EQUAL_STRING(FW_MANIFEST_NVS_NAMESPACE, namespace_name);
    TEST_ASSERT_TRUE(readwrite);
    if (s_nvs.fail_open) {
        return FW_MANIFEST_IO_ERROR;
    }
    *out_handle = 41U;
    return FW_MANIFEST_IO_OK;
}

static fw_manifest_io_result_t model_get_u32(
    fw_manifest_handle_t handle, const char *key, uint32_t *out_value)
{
    TEST_ASSERT_EQUAL_UINT32(41U, handle);
    s_nvs.get_calls++;
    if (s_nvs.fail_get_call == s_nvs.get_calls) {
        return FW_MANIFEST_IO_ERROR;
    }
    if (strcmp(key, FW_MANIFEST_KEY_VALID) == 0) {
        if (!s_nvs.present_valid) {
            return FW_MANIFEST_IO_NOT_FOUND;
        }
        *out_value = s_nvs.valid;
        return FW_MANIFEST_IO_OK;
    }
    if (strcmp(key, FW_MANIFEST_KEY_GENERATION) == 0) {
        if (!s_nvs.present_generation) {
            return FW_MANIFEST_IO_NOT_FOUND;
        }
        *out_value = s_nvs.generation;
        return FW_MANIFEST_IO_OK;
    }
    if (strcmp(key, FW_MANIFEST_KEY_MANIFEST_CRC32) == 0) {
        if (!s_nvs.present_crc) {
            return FW_MANIFEST_IO_NOT_FOUND;
        }
        *out_value = s_nvs.manifest_crc32;
        return FW_MANIFEST_IO_OK;
    }
    return FW_MANIFEST_IO_ERROR;
}

static fw_manifest_io_result_t model_set_u32(
    fw_manifest_handle_t handle, const char *key, uint32_t value)
{
    TEST_ASSERT_EQUAL_UINT32(41U, handle);
    s_nvs.set_calls++;
    if (s_nvs.fail_set_call == s_nvs.set_calls) {
        return FW_MANIFEST_IO_ERROR;
    }
    if (strcmp(key, FW_MANIFEST_KEY_VALID) == 0) {
        s_nvs.present_valid = true;
        s_nvs.valid = value;
        return FW_MANIFEST_IO_OK;
    }
    return FW_MANIFEST_IO_ERROR;
}

static void model_close(fw_manifest_handle_t handle)
{
    TEST_ASSERT_EQUAL_UINT32(41U, handle);
    s_nvs.close_calls++;
}

static const fw_manifest_store_ops_t s_manifest_ops = {
    .open = model_open,
    .get_u32 = model_get_u32,
    .set_u32 = model_set_u32,
    .close = model_close,
};

static void model_committed(uint32_t generation, uint32_t manifest_crc32)
{
    memset(&s_nvs, 0, sizeof(s_nvs));
    s_nvs.present_valid = true;
    s_nvs.present_generation = true;
    s_nvs.present_crc = true;
    s_nvs.valid = FW_MANIFEST_COMMITTED_MAGIC;
    s_nvs.generation = generation;
    s_nvs.manifest_crc32 = manifest_crc32;
    s_nvs.size = 123456U;
    fw_manifest_store_set_ops_for_test(&s_manifest_ops);
}

typedef struct {
    bool token_ok;
    bool lease_ok;
    fw_store_read_result_t read_result;
    bool partition_ok;
    bool validation_ok;
    bool restage_during_token;
    bool restage_before_clear;
    bool clear_manifest_before_clear;
    uint32_t restaged_generation;
    uint32_t restaged_crc32;
    int token_acquire_calls;
    int token_release_calls;
    int token_release_failures_remaining;
    int lease_acquire_calls;
    int lease_release_calls;
    int read_calls;
    int partition_calls;
    int validate_calls;
    int clear_calls;
    int release_order[2];
    int release_order_count;
    fw_store_info_t manifest;
    esp_partition_t partition;
} adapter_model_t;

static adapter_model_t s_adapter;

static bool adapter_token_acquire(fw_operation_token_t *out_token)
{
    s_adapter.token_acquire_calls++;
    if (!s_adapter.token_ok) {
        return false;
    }
    out_token->owner = FW_OPERATION_OWNER_SCANNER_RELAY;
    out_token->generation = 7U;
    out_token->valid = true;
    if (s_adapter.restage_during_token) {
        s_adapter.manifest.generation = s_adapter.restaged_generation;
        s_adapter.manifest.manifest_crc32 = s_adapter.restaged_crc32;
        model_committed(s_adapter.restaged_generation,
                        s_adapter.restaged_crc32);
    }
    return true;
}

static bool adapter_token_release(fw_operation_token_t token)
{
    TEST_ASSERT_TRUE(token.valid);
    TEST_ASSERT_EQUAL(FW_OPERATION_OWNER_SCANNER_RELAY, token.owner);
    s_adapter.token_release_calls++;
    if (s_adapter.token_release_failures_remaining > 0) {
        s_adapter.token_release_failures_remaining--;
        return false;
    }
    s_adapter.release_order[s_adapter.release_order_count++] = 2;
    return true;
}

static bool adapter_lease_acquire(int scanner_id)
{
    TEST_ASSERT_TRUE(scanner_id == 0 || scanner_id == 1);
    s_adapter.lease_acquire_calls++;
    return s_adapter.lease_ok;
}

static void adapter_lease_release(int scanner_id)
{
    TEST_ASSERT_TRUE(scanner_id == 0 || scanner_id == 1);
    s_adapter.lease_release_calls++;
    s_adapter.release_order[s_adapter.release_order_count++] = 1;
}

static fw_store_read_result_t adapter_read(fw_store_info_t *out)
{
    s_adapter.read_calls++;
    if (s_adapter.read_result == FW_STORE_READ_COMMITTED) {
        *out = s_adapter.manifest;
    }
    return s_adapter.read_result;
}

static const esp_partition_t *adapter_partition(
    const fw_store_info_t *snapshot)
{
    s_adapter.partition_calls++;
    TEST_ASSERT_EQUAL_UINT32(
        s_adapter.manifest.generation, snapshot->generation);
    TEST_ASSERT_EQUAL_UINT32(
        s_adapter.manifest.manifest_crc32, snapshot->manifest_crc32);
    return s_adapter.partition_ok ? &s_adapter.partition : NULL;
}

static bool adapter_validate(
    const esp_partition_t *partition, const fw_store_info_t *snapshot)
{
    s_adapter.validate_calls++;
    TEST_ASSERT_EQUAL_PTR(&s_adapter.partition, partition);
    TEST_ASSERT_EQUAL_UINT32(
        s_adapter.manifest.generation, snapshot->generation);
    if (s_adapter.restage_before_clear) {
        model_committed(s_adapter.restaged_generation,
                        s_adapter.restaged_crc32);
    } else if (s_adapter.clear_manifest_before_clear) {
        s_nvs.present_valid = false;
    }
    return s_adapter.validation_ok;
}

static fw_manifest_clear_result_t adapter_clear(
    uint32_t expected_generation, uint32_t expected_manifest_crc32)
{
    s_adapter.clear_calls++;
    return fw_store_clear_if_current(
        expected_generation, expected_manifest_crc32);
}

static const fw_relay_prepare_hooks_t s_adapter_hooks = {
    .token_acquire = adapter_token_acquire,
    .token_release = adapter_token_release,
    .uart_lease_acquire = adapter_lease_acquire,
    .uart_lease_release = adapter_lease_release,
    .read_committed = adapter_read,
    .partition_for_snapshot = adapter_partition,
    .validate_image = adapter_validate,
    .clear_if_current = adapter_clear,
};

static void adapter_reset(void)
{
    memset(&s_adapter, 0, sizeof(s_adapter));
    s_adapter.token_ok = true;
    s_adapter.lease_ok = true;
    s_adapter.read_result = FW_STORE_READ_COMMITTED;
    s_adapter.partition_ok = true;
    s_adapter.validation_ok = true;
    s_adapter.manifest.stored = true;
    s_adapter.manifest.generation = 19U;
    s_adapter.manifest.manifest_crc32 = 0xA1B2C3D4U;
    s_adapter.manifest.size = 123456U;
    strcpy(s_adapter.manifest.partition, "fw_scanner_s3");
    strcpy(s_adapter.partition.label, "fw_scanner_s3");
    s_adapter.partition.size = 2097152U;
    model_committed(
        s_adapter.manifest.generation,
        s_adapter.manifest.manifest_crc32);
    fw_relay_prepare_set_hooks_for_test(&s_adapter_hooks);
}

static void assert_no_owned_resources(void)
{
    int expected_token_releases =
        s_adapter.token_ok && s_adapter.token_acquire_calls > 0 ? 1 : 0;
    int expected_lease_releases =
        expected_token_releases && s_adapter.lease_ok &&
            s_adapter.lease_acquire_calls > 0
        ? 1 : 0;
    TEST_ASSERT_EQUAL_INT(
        expected_token_releases, s_adapter.token_release_calls);
    TEST_ASSERT_EQUAL_INT(
        expected_lease_releases, s_adapter.lease_release_calls);
}

void test_fw_manifest_clear_exact_tuple_clears_only_valid_once(void)
{
    model_committed(9U, 0x1234U);
    TEST_ASSERT_EQUAL(
        FW_MANIFEST_CLEARED, fw_store_clear_if_current(9U, 0x1234U));
    TEST_ASSERT_EQUAL_INT(1, s_nvs.open_calls);
    TEST_ASSERT_EQUAL_INT(3, s_nvs.get_calls);
    TEST_ASSERT_EQUAL_INT(1, s_nvs.set_calls);
    TEST_ASSERT_EQUAL_INT(1, s_nvs.close_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, s_nvs.valid);
    TEST_ASSERT_EQUAL_UINT32(123456U, s_nvs.size);
}

void test_fw_manifest_clear_missing_invalid_and_newer_do_not_write(void)
{
    model_committed(9U, 0x1234U);
    s_nvs.present_valid = false;
    TEST_ASSERT_EQUAL(
        FW_MANIFEST_ALREADY_INVALID,
        fw_store_clear_if_current(9U, 0x1234U));
    TEST_ASSERT_EQUAL_INT(0, s_nvs.set_calls);
    TEST_ASSERT_EQUAL_INT(1, s_nvs.close_calls);

    model_committed(9U, 0x1234U);
    s_nvs.valid = 0U;
    TEST_ASSERT_EQUAL(
        FW_MANIFEST_ALREADY_INVALID,
        fw_store_clear_if_current(9U, 0x1234U));
    TEST_ASSERT_EQUAL_INT(0, s_nvs.set_calls);

    model_committed(10U, 0x5678U);
    TEST_ASSERT_EQUAL(
        FW_MANIFEST_NOT_CURRENT,
        fw_store_clear_if_current(9U, 0x1234U));
    TEST_ASSERT_EQUAL_INT(0, s_nvs.set_calls);
    TEST_ASSERT_EQUAL_UINT32(10U, s_nvs.generation);
    TEST_ASSERT_EQUAL_UINT32(0x5678U, s_nvs.manifest_crc32);
}

void test_fw_manifest_clear_read_errors_close_once_without_writes(void)
{
    for (int fail_get = 1; fail_get <= 3; ++fail_get) {
        model_committed(9U, 0x1234U);
        manifest_model_t before = s_nvs;
        s_nvs.fail_get_call = fail_get;
        TEST_ASSERT_EQUAL(
            FW_MANIFEST_IO_ERROR_RESULT,
            fw_store_clear_if_current(9U, 0x1234U));
        TEST_ASSERT_EQUAL_INT(0, s_nvs.set_calls);
        TEST_ASSERT_EQUAL_INT(1, s_nvs.close_calls);
        TEST_ASSERT_EQUAL_UINT32(before.valid, s_nvs.valid);
        TEST_ASSERT_EQUAL_UINT32(before.size, s_nvs.size);
        TEST_ASSERT_EQUAL_UINT32(before.generation, s_nvs.generation);
        TEST_ASSERT_EQUAL_UINT32(
            before.manifest_crc32, s_nvs.manifest_crc32);
    }

    for (int missing = 0; missing < 2; ++missing) {
        model_committed(9U, 0x1234U);
        if (missing == 0) {
            s_nvs.present_generation = false;
        } else {
            s_nvs.present_crc = false;
        }
        TEST_ASSERT_EQUAL(
            FW_MANIFEST_IO_ERROR_RESULT,
            fw_store_clear_if_current(9U, 0x1234U));
        TEST_ASSERT_EQUAL_INT(0, s_nvs.set_calls);
        TEST_ASSERT_EQUAL_INT(1, s_nvs.close_calls);
    }
}

void test_fw_manifest_clear_write_error_fails_closed_without_size_claim(void)
{
    model_committed(9U, 0x1234U);
    s_nvs.fail_set_call = 1;
    TEST_ASSERT_EQUAL(
        FW_MANIFEST_IO_ERROR_RESULT,
        fw_store_clear_if_current(9U, 0x1234U));
    TEST_ASSERT_EQUAL_INT(1, s_nvs.set_calls);
    TEST_ASSERT_EQUAL_INT(1, s_nvs.close_calls);
    TEST_ASSERT_EQUAL_UINT32(123456U, s_nvs.size);
    TEST_ASSERT_EQUAL_UINT32(9U, s_nvs.generation);
    TEST_ASSERT_EQUAL_UINT32(0x1234U, s_nvs.manifest_crc32);

    model_committed(9U, 0x1234U);
    s_nvs.fail_open = 1;
    TEST_ASSERT_EQUAL(
        FW_MANIFEST_IO_ERROR_RESULT,
        fw_store_clear_if_current(9U, 0x1234U));
    TEST_ASSERT_EQUAL_INT(0, s_nvs.close_calls);
}

void test_fw_relay_prepare_success_retains_then_releases_exactly_once(void)
{
    adapter_reset();
    fw_relay_prepared_t prepared = {0};
    TEST_ASSERT_EQUAL(
        FW_RELAY_PREPARED,
        fw_relay_prepare_for_scanner(
            1, s_adapter.manifest.generation, &prepared));
    TEST_ASSERT_TRUE(prepared.token_owned);
    TEST_ASSERT_TRUE(prepared.uart_lease_owned);
    TEST_ASSERT_EQUAL_UINT32(
        s_adapter.manifest.generation, prepared.generation);
    TEST_ASSERT_EQUAL_UINT32(
        s_adapter.manifest.manifest_crc32, prepared.manifest_crc32);
    TEST_ASSERT_EQUAL_PTR(&s_adapter.partition, prepared.partition);
    TEST_ASSERT_EQUAL_INT(0, s_adapter.token_release_calls);
    TEST_ASSERT_EQUAL_INT(0, s_adapter.lease_release_calls);

    TEST_ASSERT_TRUE(fw_relay_prepared_release(&prepared));
    TEST_ASSERT_TRUE(fw_relay_prepared_release(&prepared));
    TEST_ASSERT_FALSE(prepared.token_owned);
    TEST_ASSERT_FALSE(prepared.uart_lease_owned);
    TEST_ASSERT_EQUAL_INT(1, s_adapter.token_release_calls);
    TEST_ASSERT_EQUAL_INT(1, s_adapter.lease_release_calls);
    TEST_ASSERT_EQUAL_INT(1, s_adapter.release_order[0]);
    TEST_ASSERT_EQUAL_INT(2, s_adapter.release_order[1]);
}

void test_fw_relay_prepared_release_retains_token_when_end_rejected(void)
{
    adapter_reset();
    fw_relay_prepared_t prepared = {0};
    TEST_ASSERT_EQUAL(
        FW_RELAY_PREPARED,
        fw_relay_prepare_for_scanner(0, 19U, &prepared));
    s_adapter.token_release_failures_remaining = 1;

    TEST_ASSERT_FALSE(fw_relay_prepared_release(&prepared));
    TEST_ASSERT_FALSE(prepared.uart_lease_owned);
    TEST_ASSERT_TRUE(prepared.token_owned);
    TEST_ASSERT_TRUE(prepared.operation_token.valid);
    TEST_ASSERT_EQUAL_INT(1, s_adapter.lease_release_calls);
    TEST_ASSERT_EQUAL_INT(1, s_adapter.token_release_calls);

    TEST_ASSERT_TRUE(fw_relay_prepared_release(&prepared));
    TEST_ASSERT_FALSE(prepared.token_owned);
    TEST_ASSERT_FALSE(prepared.operation_token.valid);
    TEST_ASSERT_EQUAL_INT(1, s_adapter.lease_release_calls);
    TEST_ASSERT_EQUAL_INT(2, s_adapter.token_release_calls);
    TEST_ASSERT_EQUAL_INT(1, s_adapter.release_order[0]);
    TEST_ASSERT_EQUAL_INT(2, s_adapter.release_order[1]);
}

void test_fw_relay_prepare_failure_preserves_rejected_token_for_retry(void)
{
    adapter_reset();
    s_adapter.lease_ok = false;
    s_adapter.token_release_failures_remaining = 1;
    fw_relay_prepared_t prepared = {0};

    TEST_ASSERT_EQUAL(
        FW_RELAY_BUSY, fw_relay_prepare_for_scanner(0, 19U, &prepared));
    TEST_ASSERT_TRUE(prepared.token_owned);
    TEST_ASSERT_TRUE(prepared.operation_token.valid);
    TEST_ASSERT_EQUAL_INT(1, s_adapter.token_release_calls);
    TEST_ASSERT_TRUE(fw_relay_prepared_release(&prepared));
    TEST_ASSERT_FALSE(prepared.token_owned);
    TEST_ASSERT_EQUAL_INT(2, s_adapter.token_release_calls);
}

void test_fw_relay_prepare_busy_paths_release_only_owned_resources(void)
{
    adapter_reset();
    s_adapter.token_ok = false;
    fw_relay_prepared_t prepared = {0};
    TEST_ASSERT_EQUAL(
        FW_RELAY_BUSY, fw_relay_prepare_for_scanner(0, 19U, &prepared));
    TEST_ASSERT_EQUAL_INT(0, s_adapter.lease_acquire_calls);
    assert_no_owned_resources();

    adapter_reset();
    s_adapter.lease_ok = false;
    TEST_ASSERT_EQUAL(
        FW_RELAY_BUSY, fw_relay_prepare_for_scanner(0, 19U, &prepared));
    TEST_ASSERT_EQUAL_INT(1, s_adapter.token_release_calls);
    TEST_ASSERT_EQUAL_INT(0, s_adapter.lease_release_calls);
    assert_no_owned_resources();
}

void test_fw_relay_prepare_restage_during_token_observes_new_generation(void)
{
    for (int different_bytes = 0; different_bytes <= 1; ++different_bytes) {
        adapter_reset();
        s_adapter.restage_during_token = true;
        s_adapter.restaged_generation = 20U;
        s_adapter.restaged_crc32 =
            different_bytes ? 0x55667788U
                            : s_adapter.manifest.manifest_crc32;
        fw_relay_prepared_t prepared = {0};
        TEST_ASSERT_EQUAL(
            FW_RELAY_GENERATION_CHANGED,
            fw_relay_prepare_for_scanner(0, 19U, &prepared));
        TEST_ASSERT_EQUAL_INT(0, s_adapter.partition_calls);
        TEST_ASSERT_EQUAL_INT(0, s_adapter.validate_calls);
        assert_no_owned_resources();
    }
}

void test_fw_relay_prepare_read_and_partition_failures_release_all(void)
{
    const fw_store_read_result_t read_results[] = {
        FW_STORE_READ_NO_MANIFEST,
        FW_STORE_READ_ERROR,
    };
    const fw_relay_prepare_result_t expected[] = {
        FW_RELAY_NO_MANIFEST,
        FW_RELAY_STORAGE_ERROR,
    };
    for (size_t i = 0; i < 2; ++i) {
        adapter_reset();
        s_adapter.read_result = read_results[i];
        fw_relay_prepared_t prepared = {0};
        TEST_ASSERT_EQUAL(
            expected[i],
            fw_relay_prepare_for_scanner(0, 19U, &prepared));
        TEST_ASSERT_EQUAL_INT(0, s_adapter.partition_calls);
        assert_no_owned_resources();
    }

    adapter_reset();
    s_adapter.partition_ok = false;
    fw_relay_prepared_t prepared = {0};
    TEST_ASSERT_EQUAL(
        FW_RELAY_PARTITION_INVALID,
        fw_relay_prepare_for_scanner(0, 19U, &prepared));
    TEST_ASSERT_EQUAL_INT(0, s_adapter.validate_calls);
    assert_no_owned_resources();
}

void test_fw_relay_prepare_exact_validation_failure_compare_clears(void)
{
    adapter_reset();
    s_adapter.validation_ok = false;
    fw_relay_prepared_t prepared = {0};
    TEST_ASSERT_EQUAL(
        FW_RELAY_IMAGE_INVALID,
        fw_relay_prepare_for_scanner(0, 19U, &prepared));
    TEST_ASSERT_EQUAL_INT(1, s_adapter.clear_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, s_nvs.valid);
    TEST_ASSERT_EQUAL_UINT32(123456U, s_nvs.size);
    assert_no_owned_resources();
}

void test_fw_relay_prepare_never_clears_newer_or_missing_manifest(void)
{
    fw_relay_prepared_t prepared = {0};
    for (int crc_only_change = 0; crc_only_change <= 1; ++crc_only_change) {
        adapter_reset();
        s_adapter.validation_ok = false;
        s_adapter.restage_before_clear = true;
        s_adapter.restaged_generation = crc_only_change ? 19U : 20U;
        s_adapter.restaged_crc32 = 0x55667788U;
        TEST_ASSERT_EQUAL(
            FW_RELAY_CLEAR_STALE,
            fw_relay_prepare_for_scanner(0, 19U, &prepared));
        TEST_ASSERT_EQUAL_UINT32(
            FW_MANIFEST_COMMITTED_MAGIC, s_nvs.valid);
        TEST_ASSERT_EQUAL_UINT32(
            crc_only_change ? 19U : 20U, s_nvs.generation);
        TEST_ASSERT_EQUAL_UINT32(0x55667788U, s_nvs.manifest_crc32);
        TEST_ASSERT_EQUAL_INT(0, s_nvs.set_calls);
        assert_no_owned_resources();
    }

    adapter_reset();
    s_adapter.validation_ok = false;
    s_adapter.clear_manifest_before_clear = true;
    TEST_ASSERT_EQUAL(
        FW_RELAY_IMAGE_INVALID,
        fw_relay_prepare_for_scanner(0, 19U, &prepared));
    TEST_ASSERT_EQUAL_INT(0, s_nvs.set_calls);
    assert_no_owned_resources();
}

void test_fw_relay_prepare_clear_io_failure_is_storage_error(void)
{
    adapter_reset();
    s_adapter.validation_ok = false;
    s_nvs.fail_set_call = 1;
    fw_relay_prepared_t prepared = {0};
    TEST_ASSERT_EQUAL(
        FW_RELAY_STORAGE_ERROR,
        fw_relay_prepare_for_scanner(0, 19U, &prepared));
    TEST_ASSERT_EQUAL_UINT32(123456U, s_nvs.size);
    TEST_ASSERT_EQUAL_INT(1, s_nvs.set_calls);
    assert_no_owned_resources();
}

void test_fw_relay_prepare_invalid_arguments_touch_no_hooks(void)
{
    adapter_reset();
    TEST_ASSERT_EQUAL(
        FW_RELAY_STORAGE_ERROR,
        fw_relay_prepare_for_scanner(-1, 19U, NULL));
    TEST_ASSERT_EQUAL_INT(0, s_adapter.token_acquire_calls);
}
