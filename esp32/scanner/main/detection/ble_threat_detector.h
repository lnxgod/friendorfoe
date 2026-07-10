#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BLE_PROMPT_NONE = 0,
    BLE_PROMPT_APPLE = 1,
    BLE_PROMPT_FAST_PAIR = 2,
    BLE_PROMPT_SWIFT_PAIR = 4,
} ble_prompt_family_t;

typedef enum {
    BLE_THREAT_NONE = 0,
    BLE_THREAT_PAIRING_SPAM,
    BLE_THREAT_SERIAL_SKIMMER,
} ble_threat_kind_t;

#define BLE_THREAT_EVIDENCE_SERIAL_UUID  (1U << 0)
#define BLE_THREAT_EVIDENCE_SPARSE       (1U << 1)
#define BLE_THREAT_EVIDENCE_GENERIC_NAME (1U << 2)
#define BLE_THREAT_EVIDENCE_PERSISTENT   (1U << 3)
#define BLE_THREAT_EVIDENCE_CLOSE        (1U << 4)
#define BLE_THREAT_EVIDENCE_CONNECTABLE  (1U << 5)
#define BLE_THREAT_EVIDENCE_UNTRUSTED    (1U << 6)

typedef struct {
    uint8_t mac[6];
    int64_t observed_ms;
    int8_t rssi;
    bool connectable;
    uint8_t addr_type;
    uint32_t structural_hash;
    ble_prompt_family_t prompt_family;
    uint16_t service_uuids[4];
    uint8_t service_uuid_count;
    const char *local_name;
    uint16_t company_id;
    bool trusted_identity;
} ble_threat_observation_t;

typedef struct {
    ble_threat_kind_t kind;
    uint32_t entity_hash;
    uint8_t prompt_family_mask;
    uint16_t unique_macs;
    uint16_t observation_count;
    uint16_t serial_service_uuid;
    uint8_t evidence_mask;
    int8_t strongest_rssi;
    uint8_t rssi_span;
    float confidence;
} ble_threat_signal_t;

void ble_threat_detector_init(void);
bool ble_threat_detector_observe(const ble_threat_observation_t *observation,
                                 ble_threat_signal_t *signal_out);
void ble_threat_detector_reset(void);

#ifdef __cplusplus
}
#endif
