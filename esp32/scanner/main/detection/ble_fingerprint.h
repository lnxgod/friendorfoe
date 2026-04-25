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
    /* Catch-all for Apple Continuity messages whose type doesn't reveal
     * the device model (0x10 Nearby Info, 0x0F Nearby Action, 0x05 AirDrop).
     * Apple does not broadcast iPhone-vs-iPad-vs-Mac in those — labelling
     * everything "iPhone" fabricated a phantom-device problem in v0.57. */
    BLE_DEV_APPLE_GENERIC,
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
    BLE_DEV_PEBBLEBEE,          /* Pebblebee Clip / Card / Tag (CID 0x015E) */
    BLE_DEV_CHIPOLO,            /* Chipolo ONE / CARD trackers */
    BLE_DEV_CARD_SKIMMER,       /* Generic BT chipset on gas-pump/ATM skimmers — HC-05/HC-06/BT05/JDY-08/HM-10 */
    BLE_DEV_HIDDEN_CAMERA,      /* Cheap LED-strip / smart-bulb cover (ELK-BLEDOM, BT_BPM, etc.) */
    BLE_DEV_FLOCK_SAFETY,       /* Flock Safety license-plate-reader / surveillance camera */
    BLE_DEV_META_GLASSES,       /* Meta Ray-Ban / Oakley smart glasses */
    BLE_DEV_META_DEVICE,        /* Meta Quest VR headset, Portal, etc. */
    BLE_DEV_FLIPPER_ZERO,       /* Flipper Zero hacking tool */
    BLE_DEV_AUDIO_DEVICE,       /* Bose, JBL, Sony headphones/speakers */
    BLE_DEV_SMART_HOME,         /* Amazon Echo, Sonos, smart home devices */
    BLE_DEV_VEHICLE,            /* Tesla, BMW, etc. */
    BLE_DEV_CAMERA,             /* GoPro, Canon, Nikon */
    BLE_DEV_ESCOOTER,           /* Segway, Ninebot */
    BLE_DEV_MEDICAL,            /* Dexcom, Medtronic */
    BLE_DEV_GAMING,             /* Nintendo, PlayStation */
    BLE_DEV_DRONE_OTHER,        /* Parrot, Autel, Skydio (non-DJI drones) */
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
    uint8_t             apple_flags;    /* Nearby Info/Action data-flags byte (was apple_info) */
    uint8_t             raw_mfr[20];    /* First 20 bytes of manufacturer data (for analysis) */
    uint8_t             raw_mfr_len;    /* Captured length */

    /* Collected service UUIDs (for UART serialization) */
    uint16_t            service_uuids[4];
    uint8_t             svc_uuid_count;

    /* 128-bit service UUIDs (AD types 0x06/0x07). Captured as-transmitted
     * in little-endian byte order; uart_tx reverses to the standard
     * big-endian hyphenated string on emit. Needed so BLE peripherals
     * with custom 128-bit services are detectable — including the
     * phone-driven calibration walk's session UUID (cafe...-...). */
    uint8_t             service_uuids_128[2][16];
    uint8_t             svc_uuid_128_count;
} ble_fingerprint_t;

/**
 * Compute a fingerprint from raw BLE advertisement data.
 *
 * @param data          Raw advertisement payload
 * @param length        Payload length
 * @param addr_type     Address type (public, random, etc.)
 * @param props         Advertisement properties (BLE4/ext, connectable, etc.)
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
