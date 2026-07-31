#include "fw_relay_prepare_adapter.h"

#ifndef UNIT_TESTING
#include "uart_rx.h"

#include "freertos/FreeRTOS.h"

#include <string.h>

static bool production_token_acquire(fw_operation_token_t *out_token)
{
    return fw_store_operation_try_begin(
        FW_OPERATION_OWNER_SCANNER_RELAY, false, out_token);
}

static bool production_token_release(fw_operation_token_t token)
{
    return fw_store_operation_end(token);
}

static bool production_uart_lease_acquire(int scanner_id)
{
    (void)scanner_id;
    return uart_rx_scanner_tx_lease_acquire(pdMS_TO_TICKS(2000));
}

static void production_uart_lease_release(int scanner_id)
{
    (void)scanner_id;
    uart_rx_scanner_tx_lease_release();
}

static fw_store_read_result_t production_read_committed(
    fw_store_info_t *out)
{
    return fw_store_read_committed(out);
}

static const esp_partition_t *production_partition_for_snapshot(
    const fw_store_info_t *snapshot)
{
    if (!snapshot || !snapshot->partition[0]) {
        return NULL;
    }
    return esp_partition_find_first(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY,
        snapshot->partition);
}

static bool production_validate_image(
    const esp_partition_t *partition,
    const fw_store_info_t *snapshot)
{
    char error[48] = {0};
    return fw_store_validate_snapshot_image(
        partition, snapshot, error, sizeof(error));
}

static const fw_relay_prepare_hooks_t s_production_hooks = {
    .token_acquire = production_token_acquire,
    .token_release = production_token_release,
    .uart_lease_acquire = production_uart_lease_acquire,
    .uart_lease_release = production_uart_lease_release,
    .read_committed = production_read_committed,
    .partition_for_snapshot = production_partition_for_snapshot,
    .validate_image = production_validate_image,
    .clear_if_current = fw_store_clear_if_current,
};

static const fw_relay_prepare_hooks_t *s_hooks = &s_production_hooks;
#else
#include <string.h>

static const fw_relay_prepare_hooks_t *s_hooks;
#endif

static bool hooks_complete(const fw_relay_prepare_hooks_t *hooks)
{
    return hooks && hooks->token_acquire && hooks->token_release &&
        hooks->uart_lease_acquire && hooks->uart_lease_release &&
        hooks->read_committed && hooks->partition_for_snapshot &&
        hooks->validate_image && hooks->clear_if_current;
}

fw_relay_prepare_result_t fw_relay_prepare_for_scanner(
    int scanner_id,
    uint32_t expected_generation,
    fw_relay_prepared_t *out)
{
    const fw_relay_prepare_hooks_t *hooks = s_hooks;
    fw_relay_prepare_result_t result = FW_RELAY_STORAGE_ERROR;

    if (out) {
        memset(out, 0, sizeof(*out));
        out->scanner_id = scanner_id;
    }
    if (!out || scanner_id < 0 || scanner_id > 1 ||
        !hooks_complete(hooks)) {
        return FW_RELAY_STORAGE_ERROR;
    }

    if (!hooks->token_acquire(&out->operation_token)) {
        return FW_RELAY_BUSY;
    }
    out->token_owned = true;

    if (!hooks->uart_lease_acquire(scanner_id)) {
        result = FW_RELAY_BUSY;
        goto failure;
    }
    out->uart_lease_owned = true;

    fw_store_read_result_t read_result =
        hooks->read_committed(&out->manifest);
    if (read_result == FW_STORE_READ_NO_MANIFEST) {
        result = FW_RELAY_NO_MANIFEST;
        goto failure;
    }
    if (read_result != FW_STORE_READ_COMMITTED ||
        !out->manifest.stored) {
        result = FW_RELAY_STORAGE_ERROR;
        goto failure;
    }

    out->generation = out->manifest.generation;
    out->manifest_crc32 = out->manifest.manifest_crc32;
    if (expected_generation != 0U &&
        out->generation != expected_generation) {
        result = FW_RELAY_GENERATION_CHANGED;
        goto failure;
    }

    out->partition =
        hooks->partition_for_snapshot(&out->manifest);
    if (!out->partition) {
        result = FW_RELAY_PARTITION_INVALID;
        goto failure;
    }

    if (!hooks->validate_image(out->partition, &out->manifest)) {
        fw_manifest_clear_result_t clear_result =
            hooks->clear_if_current(
                out->generation, out->manifest_crc32);
        if (clear_result == FW_MANIFEST_NOT_CURRENT) {
            result = FW_RELAY_CLEAR_STALE;
        } else if (clear_result == FW_MANIFEST_IO_ERROR_RESULT) {
            result = FW_RELAY_STORAGE_ERROR;
        } else {
            result = FW_RELAY_IMAGE_INVALID;
        }
        goto failure;
    }

    return FW_RELAY_PREPARED;

failure:
    (void)fw_relay_prepared_release(out);
    return result;
}

bool fw_relay_prepared_release(fw_relay_prepared_t *prepared)
{
    const fw_relay_prepare_hooks_t *hooks = s_hooks;
    if (!prepared || !hooks_complete(hooks)) {
        return false;
    }
    if (prepared->uart_lease_owned) {
        prepared->uart_lease_owned = false;
        hooks->uart_lease_release(prepared->scanner_id);
    }
    if (prepared->token_owned) {
        if (!hooks->token_release(prepared->operation_token)) {
            return false;
        }
        prepared->token_owned = false;
        memset(&prepared->operation_token, 0,
               sizeof(prepared->operation_token));
    }
    return true;
}

#ifdef UNIT_TESTING
void fw_relay_prepare_set_hooks_for_test(
    const fw_relay_prepare_hooks_t *hooks)
{
    s_hooks = hooks;
}
#endif
