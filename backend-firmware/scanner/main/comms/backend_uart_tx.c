#include "backend_uart_tx.h"

#include <stddef.h>
#include <string.h>

#include "ble_investigation_protocol.h"

#define BACKEND_UART_TX_KNOWN_PROPERTIES ((uint16_t)( \
    BLE_INV_PROP_BROADCAST | BLE_INV_PROP_READ | \
    BLE_INV_PROP_WRITE_WITHOUT_RESPONSE | BLE_INV_PROP_WRITE | \
    BLE_INV_PROP_NOTIFY | BLE_INV_PROP_INDICATE | \
    BLE_INV_PROP_AUTHENTICATED_SIGNED_WRITES | \
    BLE_INV_PROP_EXTENDED_PROPERTIES))

static size_t bounded_length(const char *value, size_t capacity)
{
    if (value == NULL) {
        return capacity;
    }
    size_t length = 0U;
    while (length < capacity && value[length] != '\0') {
        ++length;
    }
    return length;
}

static bool valid_command_id(const char *command_id)
{
    const size_t length = bounded_length(command_id, BLE_INV_REQUEST_ID_LEN);
    if (length != BLE_INV_REQUEST_ID_LEN - 1U) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const char value = command_id[index];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool hex_character(char value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

static bool canonical_mac(const char *value)
{
    if (bounded_length(value, 18U) != 17U) {
        return false;
    }
    for (size_t index = 0U; index < 17U; ++index) {
        if ((index + 1U) % 3U == 0U) {
            if (value[index] != ':') {
                return false;
            }
        } else if (!((value[index] >= '0' && value[index] <= '9') ||
                     (value[index] >= 'A' && value[index] <= 'F'))) {
            return false;
        }
    }
    return true;
}

static bool valid_uuid(const char *uuid, size_t capacity)
{
    const size_t length = bounded_length(uuid, capacity);
    if (length != 4U && length != 8U && length != 36U) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        if (length == 36U &&
            (index == 8U || index == 13U || index == 18U || index == 23U)) {
            if (uuid[index] != '-') {
                return false;
            }
        } else if (!hex_character(uuid[index])) {
            return false;
        }
    }
    return true;
}

static bool valid_value_hex(const char *value, size_t capacity)
{
    const size_t length = bounded_length(value, capacity);
    if (length >= capacity || length > 128U || (length % 2U) != 0U) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        if (!hex_character(value[index])) {
            return false;
        }
    }
    return true;
}

static bool valid_chunk(const ble_investigation_chunk_t *chunk)
{
    if (chunk == NULL || !valid_command_id(chunk->request_id)) {
        return false;
    }
    switch (chunk->kind) {
    case BLE_INV_CHUNK_BEGIN:
        if (chunk->mode == BLE_INV_MODE_GATT) {
            return canonical_mac(chunk->target_mac);
        }
        return chunk->mode == BLE_INV_MODE_PASSIVE_CAPTURE &&
               bounded_length(chunk->target_mac,
                              sizeof(chunk->target_mac)) == 0U;
    case BLE_INV_CHUNK_PROGRESS:
        return chunk->state >= BLE_INV_QUEUED &&
               chunk->state <= BLE_INV_READING;
    case BLE_INV_CHUNK_SERVICE:
        return chunk->index >= 0 &&
               chunk->index < BLE_INV_MAX_SERVICES &&
               valid_uuid(chunk->uuid, sizeof(chunk->uuid));
    case BLE_INV_CHUNK_CHARACTERISTIC:
        return chunk->index >= 0 && chunk->index < BLE_INV_MAX_CHARS &&
               valid_uuid(chunk->service_uuid,
                          sizeof(chunk->service_uuid)) &&
               valid_uuid(chunk->uuid, sizeof(chunk->uuid)) &&
               (chunk->properties &
                (uint16_t)~BACKEND_UART_TX_KNOWN_PROPERTIES) == 0U;
    case BLE_INV_CHUNK_READ:
        return chunk->index >= 0 && chunk->index < BLE_INV_MAX_READS &&
               valid_uuid(chunk->uuid, sizeof(chunk->uuid)) &&
               valid_value_hex(chunk->value_hex,
                               sizeof(chunk->value_hex));
    case BLE_INV_CHUNK_END:
        return chunk->state >= BLE_INV_COMPLETE &&
               chunk->state <= BLE_INV_CANCELLED &&
               bounded_length(chunk->summary, sizeof(chunk->summary)) <
                   sizeof(chunk->summary) &&
               bounded_length(chunk->error, sizeof(chunk->error)) <
                   sizeof(chunk->error);
    default:
        return false;
    }
}

static void increment_saturated(uint32_t *value)
{
    if (*value != UINT32_MAX) {
        ++*value;
    }
}

bool backend_uart_tx_init(
    backend_uart_tx_t *tx,
    backend_uart_tx_write_fn write,
    void *write_context)
{
    if (tx == NULL || write == NULL) {
        return false;
    }
    memset(tx, 0, sizeof(*tx));
    tx->write = write;
    tx->write_context = write_context;
    return true;
}

size_t backend_uart_tx_encode_investigation(
    const ble_investigation_chunk_t *chunk,
    char *output,
    size_t capacity)
{
    if (output != NULL && capacity > 0U) {
        output[0] = '\0';
    }
    if (!valid_chunk(chunk) || output == NULL || capacity < 2U) {
        return 0U;
    }

    const size_t json_length = ble_investigation_chunk_to_json(
        chunk, output, capacity - 1U);
    if (json_length == 0U || json_length + 2U > capacity) {
        output[0] = '\0';
        return 0U;
    }
    output[json_length] = '\n';
    output[json_length + 1U] = '\0';
    return json_length + 1U;
}

bool backend_uart_tx_send_investigation(
    backend_uart_tx_t *tx,
    const ble_investigation_chunk_t *chunk)
{
    if (tx == NULL || tx->write == NULL) {
        return false;
    }
    const size_t length = backend_uart_tx_encode_investigation(
        chunk, tx->line, sizeof(tx->line));
    tx->last_length = length;
    if (length == 0U || !tx->write(
            tx->write_context, (const uint8_t *)tx->line, length)) {
        increment_saturated(&tx->dropped_chunks);
        return false;
    }
    increment_saturated(&tx->sent_chunks);
    return true;
}
