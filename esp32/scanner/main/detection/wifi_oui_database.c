/**
 * Friend or Foe -- WiFi OUI Database
 *
 * 29 OUI entries ported from Android WifiOuiDatabase.kt.
 * Stored as raw 3-byte OUI prefixes for fast comparison against BSSIDs
 * extracted from WiFi beacon frames.
 *
 * Sources: IEEE OUI registry, FCC filings, community hardware teardowns.
 */

#include "wifi_oui_database.h"
#include <string.h>
#include <stdio.h>

/* ── Internal OUI table entry with raw bytes ─────────────────────────────── */

typedef struct {
    uint8_t     oui[3];
    oui_entry_t entry;
} oui_table_entry_t;

static const oui_table_entry_t OUI_TABLE[] = {
    /* ── DJI Technology ────────────────────────────────────────────────────── */
    { { 0x60, 0x60, 0x1F }, { "DJI",            "DJI Technology Co.",             false } },
    { { 0x34, 0xD2, 0x62 }, { "DJI",            "DJI Technology Co.",             false } },
    { { 0x48, 0x1C, 0xB9 }, { "DJI",            "DJI Innovation Technology",      false } },
    { { 0x08, 0xD4, 0x6A }, { "DJI",            "DJI Technology (Shenzhen)",      false } },
    { { 0xD0, 0x32, 0x9A }, { "DJI",            "DJI Technology Co.",             false } },
    { { 0xC4, 0x2F, 0x90 }, { "DJI",            "DJI Technology Co.",             false } },

    /* ── Parrot SA ─────────────────────────────────────────────────────────── */
    { { 0xA0, 0x14, 0x3D }, { "Parrot",         "Parrot SA",                      false } },
    { { 0x90, 0x03, 0xB7 }, { "Parrot",         "Parrot SA",                      false } },
    { { 0x00, 0x12, 0x1C }, { "Parrot",         "Parrot SA",                      false } },
    { { 0x00, 0x26, 0x7E }, { "Parrot",         "Parrot SA",                      false } },

    /* ── Autel Robotics ────────────────────────────────────────────────────── */
    { { 0x2C, 0xDC, 0xAD }, { "Autel",          "Autel Robotics",                false } },
    { { 0x78, 0x8C, 0xB5 }, { "Autel",          "Autel Intelligent Technology",  false } },

    /* ── Skydio ────────────────────────────────────────────────────────────── */
    { { 0x58, 0xD5, 0x6E }, { "Skydio",         "Skydio Inc.",                    false } },

    /* ── Yuneec ────────────────────────────────────────────────────────────── */
    { { 0xEC, 0xD0, 0x9F }, { "Yuneec",         "Yuneec International",           false } },
    { { 0x64, 0xD4, 0xDA }, { "Yuneec",         "Yuneec International",           false } },

    /* ── HOVERAir (Zero Zero Robotics) ─────────────────────────────────────── */
    { { 0x10, 0xD0, 0x7A }, { "HOVERAir",       "Zero Zero Robotics",             false } },

    /* ── Xiaomi / FIMI ─────────────────────────────────────────────────────── */
    { { 0x28, 0x6C, 0x07 }, { "Xiaomi",         "Xiaomi Communications",          false } },
    { { 0x64, 0xCE, 0x01 }, { "Xiaomi",         "Xiaomi Communications",          false } },
    { { 0x9C, 0x99, 0xA0 }, { "FIMI",           "Xiaomi FIMI",                    false } },

    /* ── Hubsan ────────────────────────────────────────────────────────────── */
    { { 0xD8, 0x96, 0xE0 }, { "Hubsan",         "Hubsan Technology",              false } },

    /* ── Holy Stone ────────────────────────────────────────────────────────── */
    { { 0xCC, 0xDB, 0xA7 }, { "Holy Stone",     "Holy Stone",                     false } },

    /* ── Potensic ──────────────────────────────────────────────────────────── */
    { { 0xB0, 0xA7, 0x32 }, { "Potensic",       "Potensic",                       false } },

    /* ── Walkera ───────────────────────────────────────────────────────────── */
    { { 0xC8, 0x14, 0x51 }, { "Walkera",        "Walkera Technology Co.",          false } },

    /* ── Syma ──────────────────────────────────────────────────────────────── */
    { { 0xE8, 0xAB, 0xFA }, { "Syma",           "Syma",                            false } },

    /* ── Espressif Systems (high false-positive: ESP used in many IoT) ────── */
    { { 0x24, 0x0A, 0xC4 }, { "Generic/ESP",    "Espressif Systems",              true  } },
    { { 0x30, 0xAE, 0xA4 }, { "Generic/ESP",    "Espressif Systems",              true  } },
    { { 0xA4, 0xCF, 0x12 }, { "Generic/ESP",    "Espressif Systems",              true  } },
    { { 0xAC, 0x67, 0xB2 }, { "Generic/ESP",    "Espressif Systems",              true  } },

    /* ── Realtek (high false-positive: used in many budget Chinese drones) ── */
    { { 0x00, 0xE0, 0x4C }, { "Generic/Realtek", "Realtek Semiconductor",         true  } },
};

#define OUI_TABLE_SIZE  (sizeof(OUI_TABLE) / sizeof(OUI_TABLE[0]))

/* ── Helper: parse hex character to nibble value ─────────────────────────── */

static int hex_char_to_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/**
 * Parse the first 3 colon-separated hex pairs from a BSSID string
 * into a raw 3-byte OUI.
 *
 * @param bssid  String like "60:60:1F:AA:BB:CC"
 * @param out    Output buffer for 3 OUI bytes
 * @return true on success, false if parsing fails
 */
static bool parse_oui_from_bssid(const char *bssid, uint8_t out[3])
{
    if (!bssid) return false;

    /* Expect format "XX:XX:XX:..." -- need at least 8 characters for "XX:XX:XX" */
    int byte_idx = 0;
    int i = 0;

    while (byte_idx < 3 && bssid[i] != '\0') {
        int hi = hex_char_to_nibble(bssid[i]);
        if (hi < 0) return false;
        i++;

        int lo = hex_char_to_nibble(bssid[i]);
        if (lo < 0) return false;
        i++;

        out[byte_idx++] = (uint8_t)((hi << 4) | lo);

        /* Skip colon or hyphen separator (except after last byte) */
        if (byte_idx < 3 && (bssid[i] == ':' || bssid[i] == '-')) {
            i++;
        }
    }

    return (byte_idx == 3);
}

const oui_entry_t *wifi_oui_lookup(const char *bssid)
{
    uint8_t oui[3];
    if (!parse_oui_from_bssid(bssid, oui)) {
        return NULL;
    }
    return wifi_oui_lookup_raw(oui);
}

const oui_entry_t *wifi_oui_lookup_raw(const uint8_t oui[3])
{
    for (int i = 0; i < (int)OUI_TABLE_SIZE; i++) {
        if (OUI_TABLE[i].oui[0] == oui[0] &&
            OUI_TABLE[i].oui[1] == oui[1] &&
            OUI_TABLE[i].oui[2] == oui[2]) {
            return &OUI_TABLE[i].entry;
        }
    }
    return NULL;
}
