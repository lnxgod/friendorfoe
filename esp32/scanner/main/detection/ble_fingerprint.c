/**
 * Friend or Foe -- BLE Advertisement Fingerprinting
 *
 * JA3-style approach: extract invariant features from BLE advertisements
 * and hash them into a compact fingerprint. These features persist across
 * MAC address rotations, allowing device TYPE identification without
 * needing to resolve the identity.
 *
 * Invariant features:
 *   - AD structure type sequence (which AD types, in order)
 *   - Company ID from manufacturer-specific data
 *   - Apple Continuity sub-type byte
 *   - Service UUID list
 *   - Advertisement properties (legacy/ext, connectable/scannable)
 *   - Payload length class
 *
 * Known device signatures:
 *   Apple:    Company 0x004C, Continuity types: 0x12=FindMy, 0x07=AirPods,
 *             0x0F=NearbyAction, 0x10=NearbyInfo, 0x05=AirDrop, 0x09=AirPlay
 *   Samsung:  Company 0x0075
 *   Tile:     Service UUID 0xFEED or 0xFEEC
 *   Google:   Company 0x00E0 (Google), FastPair service 0xFE2C
 *   DJI:      Company 0x2CA5
 */

#include "ble_fingerprint.h"
#include <string.h>

/* ── Apple Continuity sub-types ─────────────────────────────────────────── */

#define APPLE_COMPANY_ID        0x004C
#define APPLE_TYPE_AIRDROP      0x05
#define APPLE_TYPE_HOMEKIT      0x06
#define APPLE_TYPE_AIRPODS      0x07
#define APPLE_TYPE_AIRPLAY      0x09
#define APPLE_TYPE_NEARBY_ACT   0x0F
#define APPLE_TYPE_NEARBY_INFO  0x10
#define APPLE_TYPE_FINDMY       0x12  /* AirTag + Find My accessories */
#define APPLE_TYPE_HANDOFF      0x0C

/* ── Other company IDs ──────────────────────────────────────────────────── */

#define SAMSUNG_COMPANY_ID      0x0075
#define GOOGLE_COMPANY_ID       0x00E0
#define MICROSOFT_COMPANY_ID    0x0006
#define DJI_COMPANY_ID          0x2CA5
#define TILE_COMPANY_ID         0x0059  /* Tile Inc */
#define META_COMPANY_ID         0x01AB  /* Meta Platforms, Inc. */
#define META_TECH_COMPANY_ID    0x058E  /* Meta Platforms Technologies */
#define META_LUXOTTICA_CID      0x0D53  /* Luxottica — Ray-Ban / Oakley Meta frames */
#define FLIPPER_COMPANY_ID      0x0E29  /* Flipper Devices Inc. */
#define BOSE_COMPANY_ID         0x009E  /* Bose Corporation */
#define JBL_COMPANY_ID          0x0057  /* Harman International (JBL) */
#define SONY_COMPANY_ID         0x012D  /* Sony Group Corporation */
#define FITBIT_COMPANY_ID       0x0108  /* Fitbit, Inc. */
#define GARMIN_COMPANY_ID       0x0087  /* Garmin International */
#define XIAOMI_COMPANY_ID       0x038F  /* Anhui Huami (Xiaomi wearables) */
#define HUAWEI_COMPANY_ID       0x027D  /* Huawei Technologies */
#define AMAZON_COMPANY_ID       0x0171  /* Amazon.com Services */
#define SONOS_COMPANY_ID        0x0236  /* Sonos, Inc. */
#define IKEA_COMPANY_ID         0x0311  /* IKEA of Sweden */
/* Vehicles */
#define TESLA_COMPANY_ID        0x04F6  /* Tesla, Inc. */
/* Cameras & Drones */
#define GOPRO_COMPANY_ID        0x02DF  /* GoPro */
#define PARROT_COMPANY_ID       0x0289  /* Parrot Drones SAS */
#define AUTEL_COMPANY_ID        0x0986  /* Autel Robotics */
/* Gaming */
#define NINTENDO_COMPANY_ID     0x0578  /* Nintendo Co., Ltd. */
/* Security */
#define AXON_COMPANY_ID         0x04D8  /* Axon Enterprise (body cams) */
/* E-Scooters */
#define SEGWAY_COMPANY_ID       0x06A1  /* Segway-Ninebot */
/* Medical */
#define DEXCOM_COMPANY_ID       0x0267  /* Dexcom, Inc. */

/* ── Service UUIDs for known trackers ───────────────────────────────────── */

#define TILE_SVC_UUID           0xFEED
#define TILE_SVC_UUID2          0xFEEC
#define GOOGLE_FASTPAIR_UUID    0xFE2C
#define GOOGLE_FMDN_UUID        0xFE2C  /* Google Find My Device Network */
#define APPLE_FINDMY_SVC        0xFD6F  /* Apple Find My network */
#define SAMSUNG_SMARTTAG_SVC1   0xFD59  /* SmartTag factory/non-registered */
#define SAMSUNG_SMARTTAG_SVC2   0xFD5A  /* SmartTag registered */
#define SAMSUNG_SMARTTAG_LOST   0xFD69  /* SmartTag offline finding / lost mode */
#define META_RAYBANGEN2_SVC     0xFD5F  /* Meta Ray-Ban Gen 2 */
#define META_SVC_UUID1          0xFEB7  /* Meta Platforms, Inc. */
#define META_SVC_UUID2          0xFEB8  /* Meta Platforms, Inc. */

/* ── FNV-1a hash ────────────────────────────────────────────────────────── */

static uint32_t fnv1a_init(void) { return 0x811c9dc5; }

static uint32_t fnv1a_byte(uint32_t h, uint8_t b)
{
    h ^= b;
    h *= 0x01000193;
    return h;
}

static uint32_t fnv1a_u16(uint32_t h, uint16_t v)
{
    h = fnv1a_byte(h, (uint8_t)(v & 0xFF));
    h = fnv1a_byte(h, (uint8_t)(v >> 8));
    return h;
}

/* ── Device type names ──────────────────────────────────────────────────── */

static const char *s_type_names[] = {
    [BLE_DEV_UNKNOWN]          = "Unknown",
    [BLE_DEV_APPLE_IPHONE]     = "iPhone",
    [BLE_DEV_APPLE_IPAD]       = "iPad",
    [BLE_DEV_APPLE_MACBOOK]    = "MacBook",
    [BLE_DEV_APPLE_GENERIC]    = "Apple Device",
    [BLE_DEV_APPLE_WATCH]      = "Apple Watch",
    [BLE_DEV_APPLE_AIRPODS]    = "AirPods",
    [BLE_DEV_APPLE_AIRTAG]     = "AirTag",
    [BLE_DEV_APPLE_FINDMY]     = "FindMy Accessory",
    [BLE_DEV_SAMSUNG_PHONE]    = "Samsung Phone",
    [BLE_DEV_SAMSUNG_SMARTTAG] = "SmartTag",
    [BLE_DEV_TILE_TRACKER]     = "Tile Tracker",
    [BLE_DEV_GOOGLE_FINDMY]    = "Google Tracker",
    [BLE_DEV_DRONE_CONTROLLER] = "Drone Controller",
    [BLE_DEV_FITNESS_TRACKER]  = "Fitness Tracker",
    [BLE_DEV_SMARTWATCH]       = "Smartwatch",
    [BLE_DEV_BEACON]           = "Beacon",
    [BLE_DEV_TRACKER_GENERIC]  = "Tracker (Generic)",
    [BLE_DEV_META_GLASSES]    = "Meta Glasses",
    [BLE_DEV_META_DEVICE]     = "Meta Device",
    [BLE_DEV_FLIPPER_ZERO]    = "Flipper Zero",
    [BLE_DEV_AUDIO_DEVICE]    = "Audio Device",
    [BLE_DEV_SMART_HOME]      = "Smart Home",
    [BLE_DEV_VEHICLE]         = "Vehicle",
    [BLE_DEV_CAMERA]          = "Camera",
    [BLE_DEV_ESCOOTER]        = "E-Scooter",
    [BLE_DEV_MEDICAL]         = "Medical",
    [BLE_DEV_GAMING]          = "Gaming",
    [BLE_DEV_DRONE_OTHER]     = "Drone",
};

const char *ble_device_type_name(ble_device_type_t type)
{
    if (type < BLE_DEV_COUNT) return s_type_names[type];
    return "Unknown";
}

/* ── Classify Apple device from Continuity sub-type ─────────────────────── */

static ble_device_type_t classify_apple(uint8_t continuity_type,
                                        const uint8_t *mfr_data, int mfr_len)
{
    switch (continuity_type) {
    case APPLE_TYPE_FINDMY:
        /* Find My — distinguish AirTag from other FindMy accessories.
         *
         * Marauder uses the tighter signature: after the company_id (4C 00)
         * and type byte (0x12), AirTags broadcast a length/status byte of
         * 0x19 (25 bytes of opaque payload). Other FindMy accessories
         * (AirPods case, branded tags) use a different length. This is
         * more reliable than the old `mfr_len <= 8` heuristic, which was
         * wrong — AirTag advertisements are ~30 bytes, not short.
         *
         * Buffer layout when we get here:
         *   mfr_data[0..1] = 0x4C 0x00 (Apple company ID)
         *   mfr_data[2]    = 0x12 (FindMy continuity type)
         *   mfr_data[3]    = 0x19 for AirTag, other values for non-AirTag FindMy
         */
        if (mfr_len >= 4 && mfr_data[3] == 0x19) return BLE_DEV_APPLE_AIRTAG;
        /* Legacy fallback — very short FindMy payload is almost always AirTag. */
        if (mfr_len <= 8) return BLE_DEV_APPLE_AIRTAG;
        return BLE_DEV_APPLE_FINDMY;

    case APPLE_TYPE_AIRPODS:
        return BLE_DEV_APPLE_AIRPODS;

    case APPLE_TYPE_NEARBY_INFO:
    case APPLE_TYPE_NEARBY_ACT:
        /* Nearby Info (0x10) / Nearby Action (0x0F) — the device MODEL is
         * NOT broadcast (iPhone, iPad, and Mac all use these). Previous
         * v0.57 default of IPHONE invented ~22 phantom iPhones on a typical
         * home fleet. Emit the honest "Apple Device" and let the backend
         * enrich via the data-flags byte (apple_flags). */
        return BLE_DEV_APPLE_GENERIC;

    case APPLE_TYPE_HANDOFF:
        /* Handoff advertises from the currently-foregrounded device when
         * the user is actively using something — reliably Mac / iPhone
         * while the user is interacting. We keep the MacBook label because
         * Handoff-as-source is more commonly Mac/Mac-mini. */
        return BLE_DEV_APPLE_MACBOOK;

    case APPLE_TYPE_AIRDROP:
        /* AirDrop requires unlocked screen but doesn't reveal device kind. */
        return BLE_DEV_APPLE_GENERIC;

    case APPLE_TYPE_AIRPLAY:
        /* AirPlay source is almost always a Mac / Apple TV. */
        return BLE_DEV_APPLE_MACBOOK;

    default:
        return BLE_DEV_APPLE_GENERIC;
    }
}

/* ── Main fingerprint computation ───────────────────────────────────────── */

void ble_fingerprint_compute(const uint8_t *data, int length,
                             uint8_t addr_type, uint8_t props,
                             ble_fingerprint_t *fp)
{
    memset(fp, 0, sizeof(*fp));
    fp->payload_len = (uint8_t)length;

    if (!data || length < 2) {
        fp->device_type = BLE_DEV_UNKNOWN;
        fp->type_name = "Unknown";
        return;
    }

    uint32_t hash = fnv1a_init();

    /* Hash: address type + properties (invariant across rotations) */
    hash = fnv1a_byte(hash, addr_type);
    hash = fnv1a_byte(hash, props);

    /* Walk AD structures */
    uint16_t company_id = 0;
    uint8_t  apple_type = 0;
    const uint8_t *mfr_data = NULL;
    int mfr_data_len = 0;
    bool has_findmy_svc = false;
    bool has_tile_svc = false;
    bool has_fastpair_svc = false;
    bool has_smarttag_svc = false;
    bool has_meta_svc = false;
    bool has_meta_rayban_svc = false;  /* 0xFD5F = Ray-Ban specific */
    bool has_meta_quest_svc = false;   /* 0xFEB8 without 0xFD5F = Quest */

    int pos = 0;
    while (pos + 1 < length) {
        uint8_t ad_len = data[pos];
        if (ad_len == 0 || pos + 1 + ad_len > length) break;

        uint8_t ad_type = data[pos + 1];
        const uint8_t *ad_data = &data[pos + 2];
        int ad_data_len = ad_len - 1;

        /* Hash: AD type (ordering-dependent fingerprint) */
        hash = fnv1a_byte(hash, ad_type);
        fp->ad_type_count++;

        switch (ad_type) {
        case 0xFF:  /* Manufacturer Specific Data */
            if (ad_data_len >= 2) {
                company_id = (uint16_t)ad_data[0] | ((uint16_t)ad_data[1] << 8);
                mfr_data = ad_data;
                mfr_data_len = ad_data_len;
                fp->company_id = company_id;

                /* Hash: company ID */
                hash = fnv1a_u16(hash, company_id);

                /* Apple Continuity: deep field extraction */
                if (company_id == APPLE_COMPANY_ID && ad_data_len >= 3) {
                    apple_type = ad_data[2];
                    fp->apple_type = apple_type;
                    hash = fnv1a_byte(hash, apple_type);

                    /* Hash the Continuity sub-length too (varies by type) */
                    if (ad_data_len >= 4) {
                        hash = fnv1a_byte(hash, ad_data[3]);
                    }

                    /* Auth tag: bytes +3..+5 for Nearby Info (0x10) and Nearby Action (0x0F)
                     * This tag rotates SLOWER than the MAC address — key for entity resolution */
                    if ((apple_type == 0x10 || apple_type == 0x0F) && ad_data_len >= 6) {
                        fp->apple_auth[0] = ad_data[3];
                        fp->apple_auth[1] = ad_data[4];
                        fp->apple_auth[2] = ad_data[5];
                    }

                    /* Activity byte: offset varies by type but typically at +6 for 0x10/0x0F */
                    if ((apple_type == 0x10 || apple_type == 0x0F) && ad_data_len >= 7) {
                        fp->apple_activity = ad_data[6];
                    }

                    /* Data-flags byte (renamed from apple_info in v0.58).
                     * Nearby Info (0x10) and Nearby Action (0x0F) share the
                     * same data-flags layout at offset +7. Previously we
                     * only read it for 0x10. Bit semantics documented in
                     * shared/uart_protocol.h next to JSON_KEY_BLE_APPLE_FLAGS. */
                    if ((apple_type == 0x10 || apple_type == 0x0F) && ad_data_len >= 8) {
                        fp->apple_flags = ad_data[7];
                    }
                }

                /* Capture raw manufacturer data (first 20 bytes) for any device */
                {
                    int copy_len = ad_data_len > 20 ? 20 : ad_data_len;
                    memcpy(fp->raw_mfr, ad_data, copy_len);
                    fp->raw_mfr_len = (uint8_t)copy_len;
                }

                /* DJI: hash first few bytes of manufacturer data */
                if (company_id == DJI_COMPANY_ID && ad_data_len >= 4) {
                    hash = fnv1a_byte(hash, ad_data[2]);
                    hash = fnv1a_byte(hash, ad_data[3]);
                }
            }
            break;

        case 0x03:  /* Complete 16-bit Service UUIDs */
        case 0x02:  /* Incomplete 16-bit Service UUIDs */
            for (int i = 0; i + 1 < ad_data_len; i += 2) {
                uint16_t uuid = (uint16_t)ad_data[i] | ((uint16_t)ad_data[i + 1] << 8);
                hash = fnv1a_u16(hash, uuid);

                if (uuid == APPLE_FINDMY_SVC)    has_findmy_svc = true;
                if (uuid == TILE_SVC_UUID || uuid == TILE_SVC_UUID2) has_tile_svc = true;
                if (uuid == GOOGLE_FASTPAIR_UUID || uuid == GOOGLE_FMDN_UUID) has_fastpair_svc = true;
                if (uuid == SAMSUNG_SMARTTAG_SVC1 || uuid == SAMSUNG_SMARTTAG_SVC2 || uuid == SAMSUNG_SMARTTAG_LOST) has_smarttag_svc = true;
                if (uuid == META_RAYBANGEN2_SVC || uuid == META_SVC_UUID1 || uuid == META_SVC_UUID2) has_meta_svc = true;
                if (uuid == META_RAYBANGEN2_SVC) has_meta_rayban_svc = true;
                if (uuid == META_SVC_UUID2 && !has_meta_rayban_svc) has_meta_quest_svc = true;

                /* Collect service UUIDs for UART serialization */
                if (fp->svc_uuid_count < 4) {
                    fp->service_uuids[fp->svc_uuid_count++] = uuid;
                }
            }
            break;

        case 0x16:  /* Service Data - 16-bit UUID */
            if (ad_data_len >= 2) {
                uint16_t svc_uuid = (uint16_t)ad_data[0] | ((uint16_t)ad_data[1] << 8);
                hash = fnv1a_u16(hash, svc_uuid);

                if (svc_uuid == APPLE_FINDMY_SVC) has_findmy_svc = true;
                if (svc_uuid == TILE_SVC_UUID || svc_uuid == TILE_SVC_UUID2) has_tile_svc = true;
                if (svc_uuid == SAMSUNG_SMARTTAG_SVC1 || svc_uuid == SAMSUNG_SMARTTAG_SVC2 || svc_uuid == SAMSUNG_SMARTTAG_LOST) has_smarttag_svc = true;
                if (svc_uuid == META_RAYBANGEN2_SVC || svc_uuid == META_SVC_UUID1 || svc_uuid == META_SVC_UUID2) has_meta_svc = true;
                if (svc_uuid == META_RAYBANGEN2_SVC) has_meta_rayban_svc = true;
                if (svc_uuid == META_SVC_UUID2 && !has_meta_rayban_svc) has_meta_quest_svc = true;

                if (fp->svc_uuid_count < 4) {
                    fp->service_uuids[fp->svc_uuid_count++] = svc_uuid;
                }
            }
            break;

        case 0x01:  /* Flags */
            if (ad_data_len >= 1) {
                hash = fnv1a_byte(hash, ad_data[0]);
            }
            break;

        case 0x0A:  /* TX Power Level */
            if (ad_data_len >= 1) {
                hash = fnv1a_byte(hash, ad_data[0]);
            }
            break;

        default:
            /* Hash the AD type length class (not exact data, which may contain rotating keys) */
            hash = fnv1a_byte(hash, (uint8_t)(ad_data_len & 0xF0));
            break;
        }

        pos += 1 + ad_len;
    }

    /* Hash: total payload length class (rounds to nearest 4) */
    hash = fnv1a_byte(hash, (uint8_t)((length >> 2) << 2));

    fp->hash = hash;

    /* ── Classify device type ──────────────────────────────────────────── */

    if (company_id == APPLE_COMPANY_ID) {
        fp->device_type = classify_apple(apple_type, mfr_data, mfr_data_len);
        fp->is_tracker = (apple_type == APPLE_TYPE_FINDMY);
    } else if (has_findmy_svc) {
        fp->device_type = BLE_DEV_APPLE_FINDMY;
        fp->is_tracker = true;
    } else if (has_tile_svc || company_id == TILE_COMPANY_ID) {
        fp->device_type = BLE_DEV_TILE_TRACKER;
        fp->is_tracker = true;
    } else if (has_smarttag_svc) {
        fp->device_type = BLE_DEV_SAMSUNG_SMARTTAG;
        fp->is_tracker = true;
    } else if (company_id == SAMSUNG_COMPANY_ID) {
        /* Samsung device without SmartTag service UUIDs = phone */
        fp->device_type = BLE_DEV_SAMSUNG_PHONE;
    } else if (has_fastpair_svc || company_id == GOOGLE_COMPANY_ID) {
        fp->device_type = BLE_DEV_GOOGLE_FINDMY;
        fp->is_tracker = (mfr_data_len <= 12);
    } else if (company_id == META_COMPANY_ID || company_id == META_TECH_COMPANY_ID
               || company_id == META_LUXOTTICA_CID || has_meta_svc) {
        /* Meta device classification. Luxottica (0x0D53) is the *frame*
         * manufacturer for Ray-Ban Meta + Oakley Meta glasses and is the
         * signal Marauder's Meta scanner relies on most (not Meta's own
         * 0x01AB/0x058E which many beacons don't carry). So we treat it
         * as a Meta Glasses CID with the same weight as 0xFD5F svc UUID.
         *
         * - Ray-Ban / Oakley frames: Luxottica CID, 0xFD5F svc UUID, or
         *   local name contains "Ray-Ban" / "RB Meta" / "Oakley Meta"
         * - Quest headset: 0xFEB8 without 0xFD5F, or name contains "Quest"
         * - Other Meta: portals, controllers, Neural Band, etc. */
        if (has_meta_rayban_svc || company_id == META_LUXOTTICA_CID) {
            fp->device_type = BLE_DEV_META_GLASSES;
        } else if (has_meta_quest_svc) {
            fp->device_type = BLE_DEV_META_DEVICE;
        } else if (has_meta_svc) {
            /* Generic Meta service — payload size heuristic.
             * Quest tends to have larger advertisements than glasses. */
            fp->device_type = (fp->payload_len > 20) ? BLE_DEV_META_DEVICE : BLE_DEV_META_GLASSES;
        } else {
            fp->device_type = BLE_DEV_META_DEVICE;
        }
    } else if (company_id == FLIPPER_COMPANY_ID) {
        fp->device_type = BLE_DEV_FLIPPER_ZERO;
    } else if (company_id == DJI_COMPANY_ID) {
        fp->device_type = BLE_DEV_DRONE_CONTROLLER;
        fp->is_tracker = false;
    } else if (company_id == BOSE_COMPANY_ID || company_id == JBL_COMPANY_ID ||
               company_id == SONY_COMPANY_ID) {
        fp->device_type = BLE_DEV_AUDIO_DEVICE;
    } else if (company_id == AMAZON_COMPANY_ID || company_id == SONOS_COMPANY_ID ||
               company_id == IKEA_COMPANY_ID) {
        fp->device_type = BLE_DEV_SMART_HOME;
    } else if (company_id == TESLA_COMPANY_ID) {
        fp->device_type = BLE_DEV_VEHICLE;
    } else if (company_id == GOPRO_COMPANY_ID) {
        fp->device_type = BLE_DEV_CAMERA;
    } else if (company_id == PARROT_COMPANY_ID || company_id == AUTEL_COMPANY_ID) {
        fp->device_type = BLE_DEV_DRONE_OTHER;
    } else if (company_id == NINTENDO_COMPANY_ID) {
        fp->device_type = BLE_DEV_GAMING;
    } else if (company_id == AXON_COMPANY_ID) {
        fp->device_type = BLE_DEV_CAMERA;  /* Body camera */
    } else if (company_id == SEGWAY_COMPANY_ID) {
        fp->device_type = BLE_DEV_ESCOOTER;
    } else if (company_id == DEXCOM_COMPANY_ID) {
        fp->device_type = BLE_DEV_MEDICAL;
    } else if (company_id == FITBIT_COMPANY_ID || company_id == GARMIN_COMPANY_ID) {
        fp->device_type = BLE_DEV_FITNESS_TRACKER;
    } else if (company_id == XIAOMI_COMPANY_ID || company_id == HUAWEI_COMPANY_ID) {
        fp->device_type = BLE_DEV_SMARTWATCH;
    } else if (company_id == MICROSOFT_COMPANY_ID) {
        fp->device_type = BLE_DEV_UNKNOWN;  /* Could be Xbox controller, etc */
    } else {
        fp->device_type = BLE_DEV_UNKNOWN;
    }

    fp->type_name = ble_device_type_name(fp->device_type);
}
