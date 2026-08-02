/**
 * Friend or Foe — BLE-JA3 Structural Fingerprinting
 *
 * Computes a model-level fingerprint from BLE advertisement structure.
 * Unlike the device fingerprint (which includes MAC-derived bits),
 * this hash is the SAME for all devices of the same model — like JA3
 * for TLS client fingerprinting.
 *
 * Hash inputs (structural, invariant across devices of same model):
 *   - Ordered AD type sequence
 *   - BT SIG Company ID (from manufacturer-specific data)
 *   - Service UUID list (16-bit and 128-bit)
 *   - Advertisement properties (connectable, scannable, BLE4/ext)
 *   - Payload length class (rounded to 4 bytes)
 *   - TX Power level (if present)
 *   - Appearance value (if present)
 *
 * Hash excludes (device-specific, rotating):
 *   - MAC address
 *   - Apple Continuity encrypted keys
 *   - Battery/state bytes
 *   - Timestamps
 */

#include "ble_ja3.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"
#include "nimble/hci_common.h"

/* ── FNV-1a hash helpers ──────────────────────────────────────────────── */

static inline uint32_t fnv1a_init(void) { return 0x811C9DC5u; }

static inline void fnv1a_u8(uint32_t *h, uint8_t v)
{
    *h ^= v;
    *h *= 0x01000193u;
}

static inline void fnv1a_buf(uint32_t *h, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < len; i++) fnv1a_u8(h, p[i]);
}

static inline void fnv1a_u16le(uint32_t *h, uint16_t v)
{
    fnv1a_u8(h, (uint8_t)(v & 0xFF));
    fnv1a_u8(h, (uint8_t)(v >> 8));
}

/* ── Feature extraction ───────────────────────────────────────────────── */

typedef struct {
    uint8_t  ad_types[32];
    uint8_t  ad_type_count;
    bool     company_present;
    uint16_t company_id;
    uint16_t uuids16[16];
    uint8_t  uuid16_count;
    uint8_t  uuids128[4][16];
    uint8_t  uuid128_count;
    bool     tx_power_present;
    int8_t   tx_power;
    bool     appearance_present;
    uint16_t appearance;
} ja3_features_t;

static bool extract_features(const uint8_t *data, uint8_t len, ja3_features_t *f)
{
    memset(f, 0, sizeof(*f));
    if (!data || len == 0) return false;

    size_t off = 0;
    while (off < len) {
        uint8_t field_len = data[off];
        if (field_len == 0) break;
        if (off + 1 + field_len > len) break;

        uint8_t type = data[off + 1];
        const uint8_t *value = &data[off + 2];
        uint8_t value_len = field_len - 1;

        if (f->ad_type_count < 32)
            f->ad_types[f->ad_type_count++] = type;

        switch (type) {
        case 0xFF: /* Manufacturer Specific Data */
            if (!f->company_present && value_len >= 2) {
                f->company_present = true;
                f->company_id = (uint16_t)value[0] | ((uint16_t)value[1] << 8);
            }
            break;

        case 0x02: /* Incomplete 16-bit UUIDs */
        case 0x03: /* Complete 16-bit UUIDs */
            for (uint8_t i = 0; i + 1 < value_len && f->uuid16_count < 16; i += 2) {
                f->uuids16[f->uuid16_count++] = (uint16_t)value[i] | ((uint16_t)value[i+1] << 8);
            }
            break;

        case 0x06: /* Incomplete 128-bit UUIDs */
        case 0x07: /* Complete 128-bit UUIDs */
            for (uint8_t i = 0; i + 15 < value_len && f->uuid128_count < 4; i += 16) {
                memcpy(f->uuids128[f->uuid128_count++], &value[i], 16);
            }
            break;

        case 0x0A: /* TX Power Level */
            if (!f->tx_power_present && value_len >= 1) {
                f->tx_power_present = true;
                f->tx_power = (int8_t)value[0];
            }
            break;

        case 0x19: /* Appearance */
            if (!f->appearance_present && value_len >= 2) {
                f->appearance_present = true;
                f->appearance = (uint16_t)value[0] | ((uint16_t)value[1] << 8);
            }
            break;
        }

        off += 1 + field_len;
    }

    return f->ad_type_count > 0;
}

/* ── Main JA3 hash computation ────────────────────────────────────────── */

bool ble_ja3_from_gap_event(const struct ble_gap_event *event,
                            ble_ja3_hash_t *out)
{
    if (!event || !out) return false;

    const uint8_t *adv_data = NULL;
    uint8_t adv_len = 0;
    uint8_t connectable = 2, scannable = 2, ble4_adv = 2;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        adv_data = event->disc.data;
        adv_len = event->disc.length_data;
        ble4_adv = 1;
        switch (event->disc.event_type) {
        case BLE_HCI_ADV_RPT_EVTYPE_ADV_IND:
            connectable = 1; scannable = 1; break;
        case BLE_HCI_ADV_RPT_EVTYPE_DIR_IND:
            connectable = 1; scannable = 0; break;
        case BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND:
            connectable = 0; scannable = 1; break;
        case BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND:
            connectable = 0; scannable = 0; break;
        default: return false;
        }
        break;

#if MYNEWT_VAL(BLE_EXT_ADV) || CONFIG_BT_NIMBLE_EXT_ADV
    case BLE_GAP_EVENT_EXT_DISC:
        adv_data = event->ext_disc.data;
        adv_len = event->ext_disc.length_data;
        connectable = (event->ext_disc.props & BLE_HCI_ADV_CONN_MASK) ? 1 : 0;
        scannable = (event->ext_disc.props & BLE_HCI_ADV_SCAN_MASK) ? 1 : 0;
        ble4_adv = (event->ext_disc.props & BLE_HCI_ADV_LEGACY_MASK) ? 1 : 0;
        break;
#endif

    default:
        return false;
    }

    ja3_features_t f;
    if (!extract_features(adv_data, adv_len, &f))
        return false;

    uint32_t h = fnv1a_init();

    /* Version tag */
    fnv1a_u8(&h, 0x01);

    /* AD type sequence — ordering-dependent, structural */
    fnv1a_u8(&h, 0x10);
    fnv1a_u8(&h, f.ad_type_count);
    fnv1a_buf(&h, f.ad_types, f.ad_type_count);

    /* Company ID */
    fnv1a_u8(&h, 0x20);
    fnv1a_u8(&h, f.company_present ? 1 : 0);
    if (f.company_present) fnv1a_u16le(&h, f.company_id);

    /* 16-bit Service UUIDs */
    fnv1a_u8(&h, 0x30);
    fnv1a_u8(&h, f.uuid16_count);
    for (int i = 0; i < f.uuid16_count; i++) fnv1a_u16le(&h, f.uuids16[i]);

    /* 128-bit Service UUIDs */
    fnv1a_u8(&h, 0x31);
    fnv1a_u8(&h, f.uuid128_count);
    for (int i = 0; i < f.uuid128_count; i++) fnv1a_buf(&h, f.uuids128[i], 16);

    /* Advertisement properties */
    fnv1a_u8(&h, 0x40);
    fnv1a_u8(&h, connectable);
    fnv1a_u8(&h, scannable);
    fnv1a_u8(&h, ble4_adv);

    /* Payload length class (rounded to 4 bytes — avoids rotating key influence) */
    fnv1a_u8(&h, 0x50);
    fnv1a_u8(&h, (uint8_t)(((uint16_t)adv_len + 2) & 0xFC));

    /* TX Power */
    fnv1a_u8(&h, 0x60);
    fnv1a_u8(&h, f.tx_power_present ? 1 : 0);
    if (f.tx_power_present) fnv1a_u8(&h, (uint8_t)f.tx_power);

    /* Appearance */
    fnv1a_u8(&h, 0x70);
    fnv1a_u8(&h, f.appearance_present ? 1 : 0);
    if (f.appearance_present) fnv1a_u16le(&h, f.appearance);

    out->value = h;
    snprintf(out->hex, sizeof(out->hex), "%08" PRIx32, h);
    return true;
}
