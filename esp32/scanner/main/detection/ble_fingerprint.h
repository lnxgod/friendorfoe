#pragma once

/**
 * Friend or Foe -- BLE Advertisement Fingerprinting
 *
 * JA3-style fingerprinting for BLE devices. Creates a stable fingerprint
 * from invariant advertisement fields that persists across MAC rotations.
 * Identifies device types: AirTag, Tile, SmartTag, iPhone, drone controller, etc.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Known device categories */
typedef enum {
    BLE_DEV_UNKNOWN = 0,
    BLE_DEV_APPLE_IPHONE,
    BLE_DEV_APPLE_IPAD,
    BLE_DEV_APPLE_MACBOOK,
    BLE_DEV_APPLE_WATCH,
    BLE_DEV_APPLE_AIRPODS,
    BLE_DEV_APPLE_AIRTAG,
    BLE_DEV_APPLE_FINDMY,       /* Generic Find My accessory */
    BLE_DEV_SAMSUNG_PHONE,
    BLE_DEV_SAMSUNG_SMARTTAG,
    BLE_DEV_TILE_TRACKER,
    BLE_DEV_GOOGLE_FINDMY,
    BLE_DEV_DRONE_CONTROLLER,   /* DJI/Skydio controller BLE */
    BLE_DEV_FITNESS_TRACKER,
    BLE_DEV_SMARTWATCH,
    BLE_DEV_BEACON,             /* iBeacon, Eddystone */
    BLE_DEV_TRACKER_GENERIC,    /* Unknown tracker type */
    BLE_DEV_COUNT
} ble_device_type_t;

/* Fingerprint result */
typedef struct {
    uint32_t            hash;           /* 32-bit fingerprint hash */
    ble_device_type_t   device_type;    /* Classified device type */
    const char         *type_name;      /* Human-readable type name */
    uint16_t            company_id;     /* BLE company ID (0 if none) */
    uint8_t             apple_type;     /* Apple Continuity sub-type (0 if not Apple) */
    bool                is_tracker;     /* True if likely a tracking device */
    uint8_t             ad_type_count;  /* Number of AD structures */
    uint8_t             payload_len;    /* Total adv payload length */

    /* Apple Continuity deep fields */
    uint8_t             apple_auth[3];  /* Auth tag — rotates slower than MAC (entity linking key) */
    uint8_t             apple_activity; /* Activity code: 0=idle, 1=audio, 2=phone, 3=video */
    uint8_t             apple_info;     /* Status/info byte */
    uint8_t             raw_mfr[20];    /* First 20 bytes of manufacturer data (for analysis) */
    uint8_t             raw_mfr_len;    /* Captured length */
} ble_fingerprint_t;

/**
 * Compute a fingerprint from raw BLE advertisement data.
 *
 * @param data          Raw advertisement payload
 * @param length        Payload length
 * @param addr_type     Address type (public, random, etc.)
 * @param props         Advertisement properties (legacy, connectable, etc.)
 * @param fp            Output fingerprint
 */
void ble_fingerprint_compute(const uint8_t *data, int length,
                             uint8_t addr_type, uint8_t props,
                             ble_fingerprint_t *fp);

/**
 * Get a human-readable name for a device type.
 */
const char *ble_device_type_name(ble_device_type_t type);

#ifdef __cplusplus
}
#endif
