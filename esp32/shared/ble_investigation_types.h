#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_INV_REQUEST_ID_LEN 33
#define BLE_INV_UUID_LEN 37
#define BLE_INV_SUMMARY_LEN 128
#define BLE_INV_ERROR_LEN 64
#define BLE_INV_MAX_SERVICES 16
#define BLE_INV_MAX_CHARS 32
#define BLE_INV_MAX_READS 8
#define BLE_INV_READ_HEX_LEN 129

#define BLE_INV_PROP_BROADCAST                    ((uint16_t)0x0001)
#define BLE_INV_PROP_READ                         ((uint16_t)0x0002)
#define BLE_INV_PROP_WRITE_WITHOUT_RESPONSE       ((uint16_t)0x0004)
#define BLE_INV_PROP_WRITE                        ((uint16_t)0x0008)
#define BLE_INV_PROP_NOTIFY                       ((uint16_t)0x0010)
#define BLE_INV_PROP_INDICATE                     ((uint16_t)0x0020)
#define BLE_INV_PROP_AUTHENTICATED_SIGNED_WRITES  ((uint16_t)0x0040)
#define BLE_INV_PROP_EXTENDED_PROPERTIES          ((uint16_t)0x0080)

typedef enum {
    BLE_INV_MODE_GATT = 0,
    BLE_INV_MODE_PASSIVE_CAPTURE,
} ble_investigation_mode_t;

typedef enum {
    BLE_INV_IDLE = 0,
    BLE_INV_QUEUED,
    BLE_INV_SCANNING,
    BLE_INV_CONNECTING,
    BLE_INV_DISCOVERING,
    BLE_INV_READING,
    BLE_INV_COMPLETE,
    BLE_INV_FAILED,
    BLE_INV_CANCELLED,
} ble_investigation_state_t;

typedef struct {
    char request_id[BLE_INV_REQUEST_ID_LEN];
    ble_investigation_mode_t mode;
    char target_mac[18];
    uint32_t timeout_ms;
} ble_investigation_request_t;

typedef struct {
    char service_uuid[BLE_INV_UUID_LEN];
    char uuid[BLE_INV_UUID_LEN];
    uint16_t properties;
} ble_investigation_characteristic_t;

typedef struct {
    char uuid[BLE_INV_UUID_LEN];
    char value_hex[BLE_INV_READ_HEX_LEN];
} ble_investigation_read_t;

typedef struct {
    char request_id[BLE_INV_REQUEST_ID_LEN];
    ble_investigation_mode_t mode;
    ble_investigation_state_t state;
    char target_mac[18];
    bool connectable;
    bool bonded;
    bool encrypted;
    bool authentication_required;
    bool truncated;
    char services[BLE_INV_MAX_SERVICES][BLE_INV_UUID_LEN];
    uint8_t service_count;
    ble_investigation_characteristic_t characteristics[BLE_INV_MAX_CHARS];
    uint8_t characteristic_count;
    ble_investigation_read_t reads[BLE_INV_MAX_READS];
    uint8_t read_count;
    char summary[BLE_INV_SUMMARY_LEN];
    char error[BLE_INV_ERROR_LEN];
} ble_investigation_result_t;

typedef enum {
    BLE_INV_CHUNK_BEGIN = 0,
    BLE_INV_CHUNK_PROGRESS,
    BLE_INV_CHUNK_SERVICE,
    BLE_INV_CHUNK_CHARACTERISTIC,
    BLE_INV_CHUNK_READ,
    BLE_INV_CHUNK_END,
} ble_investigation_chunk_kind_t;

typedef struct {
    ble_investigation_chunk_kind_t kind;
    char request_id[BLE_INV_REQUEST_ID_LEN];
    int index;
    ble_investigation_state_t state;
    ble_investigation_mode_t mode;
    char target_mac[18];
    char service_uuid[BLE_INV_UUID_LEN];
    char uuid[BLE_INV_UUID_LEN];
    uint16_t properties;
    char value_hex[BLE_INV_READ_HEX_LEN];
    char summary[BLE_INV_SUMMARY_LEN];
    char error[BLE_INV_ERROR_LEN];
    bool authentication_required;
    bool truncated;
} ble_investigation_chunk_t;

#ifdef __cplusplus
}
#endif
