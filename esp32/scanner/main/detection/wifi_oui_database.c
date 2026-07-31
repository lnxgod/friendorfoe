/**
 * Friend or Foe -- WiFi OUI Database
 *
 * OUI entries ported from Android/backend RF reference data.
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

/*
 * All entries below verified against the IEEE MA-L/MA-M/MA-S registry
 * (2026-07). Only exclusive /24 (MA-L) blocks are treated as confident
 * (high_false_positive = false): MA-M(/28) and MA-S(/36) blocks share their
 * first 3 bytes across ~15 unrelated companies, so a 3-byte match on them is
 * unreliable and those vendors (Autel, Yuneec, Hubsan, Quantum-Systems, Silvus)
 * are detected via SSID/RID instead. Several legacy rows were mislabeled and
 * generated false positives; they are corrected to the true IEEE owner and
 * flagged high_false_positive (kept for enrichment, excluded from isDroneOui()).
 * This table is kept in parity with Android WifiOuiDatabase.kt.
 */
static const oui_table_entry_t OUI_TABLE[] = {
    /* ── DJI Technology (SZ DJI, Ronin, Osmo, Baiwang subsidiaries) ────────── */
    { { 0x60, 0x60, 0x1F }, { "DJI",            "SZ DJI Technology Co.",          false, OUI_ROLE_DRONE } },
    { { 0x34, 0xD2, 0x62 }, { "DJI",            "SZ DJI Technology Co.",          false, OUI_ROLE_DRONE } },
    { { 0x48, 0x1C, 0xB9 }, { "DJI",            "SZ DJI Technology Co.",          false, OUI_ROLE_DRONE } },
    { { 0x04, 0xA8, 0x5A }, { "DJI",            "SZ DJI Technology Co.",          false, OUI_ROLE_DRONE } },
    { { 0x0C, 0x9A, 0xE6 }, { "DJI",            "SZ DJI Technology Co.",          false, OUI_ROLE_DRONE } },
    { { 0x4C, 0x43, 0xF6 }, { "DJI",            "SZ DJI Technology Co.",          false, OUI_ROLE_DRONE } },
    { { 0x58, 0xB8, 0x58 }, { "DJI",            "SZ DJI Technology Co.",          false, OUI_ROLE_DRONE } },
    { { 0x88, 0x29, 0x85 }, { "DJI",            "SZ DJI Technology Co.",          false, OUI_ROLE_DRONE } },
    { { 0x8C, 0x58, 0x23 }, { "DJI",            "SZ DJI Technology Co.",          false, OUI_ROLE_DRONE } },
    { { 0xE4, 0x7A, 0x2C }, { "DJI",            "SZ DJI Technology Co.",          false, OUI_ROLE_DRONE } },
    { { 0xF8, 0x40, 0x68 }, { "DJI",            "SZ DJI Ronin Technology Co.",    false, OUI_ROLE_DRONE } },
    { { 0x20, 0x1F, 0x55 }, { "DJI",            "DJI Osmo Technology Co.",        false, OUI_ROLE_DRONE } },
    { { 0x9C, 0x5A, 0x8A }, { "DJI",            "DJI Baiwang Technology Co.",     false, OUI_ROLE_DRONE } },
    { { 0xEC, 0x72, 0xF7 }, { "DJI",            "DJI Baiwang Technology Co.",     false, OUI_ROLE_DRONE } },

    /* ── Parrot SA ─────────────────────────────────────────────────────────── */
    { { 0xA0, 0x14, 0x3D }, { "Parrot",         "Parrot SA",                      false, OUI_ROLE_DRONE } },
    { { 0x90, 0x03, 0xB7 }, { "Parrot",         "Parrot SA",                      false, OUI_ROLE_DRONE } },
    { { 0x00, 0x12, 0x1C }, { "Parrot",         "Parrot SA",                      false, OUI_ROLE_DRONE } },
    { { 0x00, 0x26, 0x7E }, { "Parrot",         "Parrot SA",                      false, OUI_ROLE_DRONE } },
    { { 0x90, 0x3A, 0xE6 }, { "Parrot",         "Parrot SA",                      false, OUI_ROLE_DRONE } },

    /* ── Skydio (58:D5:6E was mislabeled: it is D-Link) ────────────────────── */
    { { 0x38, 0x1D, 0x14 }, { "Skydio",         "Skydio Inc.",                    false, OUI_ROLE_DRONE } },

    /* ── Zero Zero Robotics (HoverAir) ─────────────────────────────────────── */
    { { 0x84, 0x83, 0x19 }, { "HOVERAir",       "Hangzhou Zero Zero Technology",  false, OUI_ROLE_DRONE } },

    /* ── Teal Drones (Red Cat) ─────────────────────────────────────────────── */
    { { 0xB0, 0x30, 0xC8 }, { "Teal",           "Teal Drones, Inc.",              false, OUI_ROLE_DRONE } },

    /* ── Freefly Systems ───────────────────────────────────────────────────── */
    { { 0xEC, 0x71, 0x5E }, { "Freefly",        "Freefly Systems Inc.",           false, OUI_ROLE_DRONE } },

    /* ── PowerVision ───────────────────────────────────────────────────────── */
    { { 0x54, 0x7D, 0x40 }, { "PowerVision",    "Powervision Tech Inc.",          false, OUI_ROLE_DRONE } },

    /* ── Military / tactical UAS + MANET datalinks (verified /24) ──────────── */
    { { 0x14, 0xDD, 0x48 }, { "Shield AI",      "Shield AI Inc.",                 false, OUI_ROLE_DRONE } },
    { { 0x00, 0x1A, 0xF9 }, { "AeroVironment",  "AeroVironment Inc.",             false, OUI_ROLE_DRONE } },
    { { 0x00, 0x18, 0xA6 }, { "Persistent",     "Persistent Systems LLC (MPU5)",  false, OUI_ROLE_DRONE } },
    { { 0x00, 0x0F, 0x92 }, { "Microhard",      "Microhard Systems (Canada)",     false, OUI_ROLE_DRONE } },
    { { 0x00, 0x1E, 0x3F }, { "TrellisWare",    "TrellisWare Technologies",       false, OUI_ROLE_DRONE } },
    { { 0x98, 0x49, 0x9F }, { "DTC",            "Domo Tactical Communications",   false, OUI_ROLE_DRONE } },
    { { 0x00, 0x30, 0x1A }, { "Doodle Labs",    "Doodle Labs (Smartbridges)",     false, OUI_ROLE_DRONE } },
    { { 0x00, 0x13, 0x56 }, { "FLIR",           "Teledyne FLIR",                  false, OUI_ROLE_DRONE } },

    /* ── Privacy infrastructure / ALPR / surveillance cameras ─────────────────
     * B4:1E:52 is the IEEE registered Flock Safety OUI. C4:2F:90 (Hikvision)
     * and E8:AB:FA (Reecam) were mislabeled as drones; corrected to their true
     * camera vendors and flagged (privacy relevance, not drone). */
    { { 0xB4, 0x1E, 0x52 }, { "Flock Safety",   "Flock Safety ALPR/camera registered OUI", false, OUI_ROLE_PRIVACY_FLOCK } },
    { { 0xE0, 0xA7, 0x00 }, { "Verkada",        "Verkada Inc.",                    false, OUI_ROLE_PRIVACY_INFRASTRUCTURE } },
    { { 0xCC, 0x47, 0xBD }, { "Rhombus",        "Rhombus Systems",                 false, OUI_ROLE_PRIVACY_INFRASTRUCTURE } },
    { { 0x00, 0x25, 0xDF }, { "Axon",           "Axon Enterprise",                 false, OUI_ROLE_PRIVACY_INFRASTRUCTURE } },
    { { 0x2C, 0x42, 0x05 }, { "Lytx",           "Lytx Inc.",                       false, OUI_ROLE_PRIVACY_INFRASTRUCTURE } },
    { { 0x50, 0xDF, 0x95 }, { "Lytx",           "Lytx Inc.",                       false, OUI_ROLE_PRIVACY_INFRASTRUCTURE } },
    { { 0x58, 0xA7, 0x48 }, { "Lytx",           "Lytx Inc.",                       false, OUI_ROLE_PRIVACY_INFRASTRUCTURE } },
    { { 0x70, 0xE4, 0x6E }, { "Lytx",           "Lytx Inc.",                       false, OUI_ROLE_PRIVACY_INFRASTRUCTURE } },
    { { 0xC4, 0x2F, 0x90 }, { "Hikvision",      "Hangzhou Hikvision (IP camera)", true, OUI_ROLE_ENRICHMENT_ONLY } },
    { { 0xE8, 0xAB, 0xFA }, { "Reecam",         "Shenzhen Reecam (IP camera)",    true, OUI_ROLE_ENRICHMENT_ONLY } },

    /* ── Generic module makers (high false-positive: seen on many budget
     *    drones AND unrelated IoT). B0:A7:32/CC:DB:A7 were mislabeled as
     *    Potensic/Holy Stone; both are Espressif. 10:D0:7A was HOVERAir
     *    (AMPAK), 2C:DC:AD was Autel (WNC), EC:D0:9F was Yuneec (Xiaomi),
     *    78:8C:B5 was Autel (TP-Link). ───────────────────────────────────── */
    { { 0x24, 0x0A, 0xC4 }, { "Espressif",      "Espressif Systems",              true, OUI_ROLE_ENRICHMENT_ONLY } },
    { { 0x30, 0xAE, 0xA4 }, { "Espressif",      "Espressif Systems",              true, OUI_ROLE_ENRICHMENT_ONLY } },
    { { 0xA4, 0xCF, 0x12 }, { "Espressif",      "Espressif Systems",              true, OUI_ROLE_ENRICHMENT_ONLY } },
    { { 0xAC, 0x67, 0xB2 }, { "Espressif",      "Espressif Systems",              true, OUI_ROLE_ENRICHMENT_ONLY } },
    { { 0x10, 0x06, 0x1C }, { "Espressif",      "Espressif Systems",              true, OUI_ROLE_ENRICHMENT_ONLY } },
    { { 0xB0, 0xA7, 0x32 }, { "Espressif",      "Espressif Systems",              true, OUI_ROLE_ENRICHMENT_ONLY } },
    { { 0xCC, 0xDB, 0xA7 }, { "Espressif",      "Espressif Systems",              true, OUI_ROLE_ENRICHMENT_ONLY } },
    { { 0x00, 0xE0, 0x4C }, { "Realtek",        "Realtek Semiconductor",          true, OUI_ROLE_ENRICHMENT_ONLY } },
    { { 0x08, 0xEA, 0x40 }, { "Bilian",         "Shenzhen Bilian (LB-LINK)",      true, OUI_ROLE_ENRICHMENT_ONLY } },
    { { 0x10, 0xD0, 0x7A }, { "AMPAK",          "AMPAK Technology (WiFi module)", true, OUI_ROLE_ENRICHMENT_ONLY } },
    { { 0x2C, 0xDC, 0xAD }, { "WNC",            "Wistron NeWeb (ODM module)",     true, OUI_ROLE_ENRICHMENT_ONLY } },
    { { 0x28, 0x6C, 0x07 }, { "Xiaomi",         "Xiaomi Communications",          true, OUI_ROLE_ENRICHMENT_ONLY } },
    { { 0xEC, 0xD0, 0x9F }, { "Xiaomi",         "Xiaomi Communications",          true, OUI_ROLE_ENRICHMENT_ONLY } },
    { { 0x9C, 0x99, 0xA0 }, { "Xiaomi/FIMI",    "Xiaomi Communications (FIMI)",   true, OUI_ROLE_ENRICHMENT_ONLY } },
    { { 0x78, 0x8C, 0xB5 }, { "TP-Link",        "TP-Link Systems",                true, OUI_ROLE_ENRICHMENT_ONLY } },

    /* Removed as mislabels (true owner is an unrelated consumer vendor, no
     * detection value): 08:D4:6A=LG, 64:D4:DA=Intel, 58:D5:6E=D-Link,
     * C8:14:51=Huawei, D8:96:E0=Alibaba Cloud; and observed-only prefixes not
     * in the IEEE registry (D0:32:9A, 64:CE:01). Holy Stone 00:0C:BF NOT added:
     * that IEEE record is a Taiwanese capacitor maker, not the drone brand. */
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

static bool is_ascii_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/**
 * Parse the first 3 hex pairs from a BSSID string
 * into a raw 3-byte OUI.
 *
 * @param bssid  String like "60:60:1F:AA:BB:CC"
 * @param out    Output buffer for 3 OUI bytes
 * @return true on success, false if parsing fails
 */
static bool parse_oui_from_bssid(const char *bssid, uint8_t out[3])
{
    if (!bssid) return false;

    int byte_idx = 0;
    int i = 0;

    while (is_ascii_space(bssid[i])) {
        i++;
    }

    while (byte_idx < 3 && bssid[i] != '\0') {
        int hi = hex_char_to_nibble(bssid[i]);
        if (hi < 0) return false;
        i++;

        int lo = hex_char_to_nibble(bssid[i]);
        if (lo < 0) return false;
        i++;

        out[byte_idx++] = (uint8_t)((hi << 4) | lo);

        /* Skip one common MAC separator (except after the parsed OUI). */
        if (byte_idx < 3 &&
            (bssid[i] == ':' || bssid[i] == '-' || bssid[i] == '.')) {
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
