/**
 * @file glasses_detector.c
 * @brief BLE smart glasses and privacy device detector.
 *
 * Matches BLE advertisements against a database of known smart glasses
 * and privacy-intrusion devices using three methods:
 *   1. Manufacturer Company ID (from Manufacturer Specific Data)
 *   2. 16-bit Service UUID
 *   3. Device name prefix
 *
 * Sources: Bluetooth SIG Assigned Numbers, Gemini + Codex research,
 *          FCC filings, community BLE captures.
 */

#include "glasses_detector.h"

#include <string.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_timer.h>

static const char *TAG = "glasses_det";

/* ── NVS runtime toggle ──────────────────────────────────────────────── */

#define NVS_NAMESPACE  "fof_config"
#define NVS_KEY_ENABLE "glasses_det"

static bool s_enabled = true;  /* default: on */
static bool s_nvs_loaded = false;

bool glasses_detection_is_enabled(void)
{
    if (!s_nvs_loaded) {
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
            uint8_t val = 1;
            nvs_get_u8(h, NVS_KEY_ENABLE, &val);
            s_enabled = (val != 0);
            nvs_close(h);
        }
        s_nvs_loaded = true;
        ESP_LOGI(TAG, "Glasses detection %s (NVS)", s_enabled ? "ENABLED" : "DISABLED");
    }
    return s_enabled;
}

void glasses_detection_set_enabled(bool enabled)
{
    s_enabled = enabled;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_ENABLE, enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Glasses detection set to %s", enabled ? "ENABLED" : "DISABLED");
}

/* ── Manufacturer Company ID database ────────────────────────────────── */

typedef struct {
    uint16_t    company_id;     /* Bluetooth SIG Company Identifier (LE) */
    const char *manufacturer;
    const char *device_type;
    float       confidence;
    bool        has_camera;
} mfr_cid_entry_t;

static const mfr_cid_entry_t s_mfr_cid_db[] = {
    /* HIGH confidence — verified BLE fingerprints */
    { 0x01AB, "Meta",           "Smart Glasses", 0.90f, true  }, /* Meta Platforms, Inc. */
    { 0x058E, "Meta",           "Smart Glasses", 0.90f, true  }, /* Meta Platforms Technologies */
    { 0x03C2, "Snap",           "Smart Glasses", 0.85f, true  }, /* Snapchat Inc. */
    { 0x00E0, "Google",         "Smart Glasses", 0.80f, true  }, /* Google LLC */
    { 0x060C, "Vuzix",          "Smart Glasses", 0.85f, true  }, /* Vuzix Corporation */
    { 0x009E, "Bose",           "Audio Glasses", 0.75f, false }, /* Bose Corporation */
    { 0x009F, "Bose",           "Audio Glasses", 0.75f, false }, /* Bose Corporation (alt) */
    { 0x034D, "Axon",           "Body Camera",   0.85f, true  }, /* TASER/Axon Enterprise */

    /* MEDIUM confidence */
    { 0x09B1, "Brilliant Labs", "Smart Glasses", 0.80f, true  }, /* Brilliant Labs Ltd. */
    { 0x0BC6, "TCL",            "Smart Glasses", 0.70f, true  }, /* TCL Communication */
    { 0x0962, "Rokid",          "Smart Glasses", 0.75f, true  }, /* Shanghai Lingban Tech */

    /* LOW confidence — too broad, many non-glasses devices */
    { 0x0171, "Amazon",         "Smart Glasses", 0.50f, false }, /* Amazon.com (Echo Frames but also Echo, Fire, etc.) */

    /* ── Trackers / Stalkerware ────────────────────────────────────── */
    { 0x000D, "Tile",           "BLE Tracker",   0.85f, false }, /* Tile Inc. */
    { 0x0075, "Samsung",        "BLE Tracker",   0.80f, false }, /* Samsung SmartTag */
    { 0x067C, "Tile",           "BLE Tracker",   0.85f, false }, /* Tile (alt CID) */

    /* ── Vehicles with cameras ─────────────────────────────────────── */
    /* Tesla CID not in SIG — detected by name/UUID instead */
};
#define MFR_CID_DB_COUNT (sizeof(s_mfr_cid_db) / sizeof(s_mfr_cid_db[0]))

/* ── 16-bit Service UUID database ────────────────────────────────────── */

typedef struct {
    uint16_t    uuid16;
    const char *manufacturer;
    const char *device_type;
    float       confidence;
    bool        has_camera;
} svc_uuid_entry_t;

static const svc_uuid_entry_t s_svc_uuid_db[] = {
    /* Smart Glasses */
    { 0xFD5F, "Meta",    "Smart Glasses", 0.95f, true  }, /* Meta Ray-Ban Gen 2 */
    { 0xFDD2, "Bose",    "Audio Glasses", 0.85f, false }, /* Bose AR wearable */
    { 0xFE45, "Snap",    "Smart Glasses", 0.80f, true  }, /* Snapchat assigned */
    { 0xFE15, "Amazon",  "Smart Glasses", 0.70f, false }, /* Amazon assigned */

    /* Trackers / Stalkerware */
    { 0xFD5A, "Samsung", "BLE Tracker",   0.90f, false }, /* Samsung Offline Finding */
    { 0xFD59, "Samsung", "BLE Tracker",   0.85f, false }, /* Samsung SmartTag pairing */
    { 0xFEED, "Tile",    "BLE Tracker",   0.85f, false }, /* Tile tracker */
    { 0xFEEC, "Tile",    "BLE Tracker",   0.85f, false }, /* Tile tracker (alt) */
    { 0xFCB2, "DULT",    "BLE Tracker",   0.90f, false }, /* DULT unwanted tracker protocol */
    { 0xFE2C, "Google",  "BLE Tracker",   0.85f, false }, /* Google Find My Device network */

    /* Retail Tracking */
    { 0xFEAA, "Google",  "Tracking Beacon", 0.70f, false }, /* Eddystone beacon */
};
#define SVC_UUID_DB_COUNT (sizeof(s_svc_uuid_db) / sizeof(s_svc_uuid_db[0]))

/* ── Device name prefix database ─────────────────────────────────────── */

typedef struct {
    const char *prefix;
    const char *manufacturer;
    const char *device_type;
    float       confidence;
    bool        has_camera;
    bool        exact;          /* true = exact match, false = prefix */
} name_pattern_entry_t;

static const name_pattern_entry_t s_name_db[] = {
    /* HIGH confidence — well-known device names */
    { "RB Meta",         "Meta",            "Smart Glasses", 0.95f, true,  false },
    { "Ray-Ban Stories", "Meta",            "Smart Glasses", 0.95f, true,  false },
    { "Spectacles",      "Snap",            "Smart Glasses", 0.90f, true,  false },
    { "Echo Frames",     "Amazon",          "Smart Glasses", 0.90f, false, true  },
    { "Vuzix",           "Vuzix",           "Smart Glasses", 0.90f, true,  false },

    /* MEDIUM confidence */
    { "XREAL",           "Xreal",           "Smart Glasses", 0.85f, true,  false },
    { "Nreal",           "Xreal",           "Smart Glasses", 0.85f, true,  false },
    { "Rokid",           "Rokid",           "Smart Glasses", 0.85f, true,  false },
    { "RayNeo",          "TCL",             "Smart Glasses", 0.90f, true,  false },
    { "Monocle",         "Brilliant Labs",  "Smart Glasses", 0.85f, true,  false },
    { "Frame",           "Brilliant Labs",  "Smart Glasses", 0.70f, true,  false }, /* generic word — lower conf */
    { "Even Realities",  "Even Realities",  "Smart Glasses", 0.80f, true,  false },
    { "INMO",            "INMO",            "Smart Glasses", 0.80f, true,  false },
    { "IMA0",            "INMO",            "Smart Glasses", 0.80f, true,  false },
    { "Solos AirGo",     "Solos",           "Smart Glasses", 0.80f, false, false },
    { "Glass",           "Google",          "Smart Glasses", 0.70f, true,  false },
    { "Bose Frames",     "Bose",            "Audio Glasses", 0.90f, false, false },

    /* Body cameras / spy cameras (BLE names) */
    { "Axon Body",       "Axon",            "Body Camera",   0.90f, true,  false },
    { "Axon Signal",     "Axon",            "Body Camera",   0.85f, true,  false },
    { "VISTA_",          "Motorola",        "Body Camera",   0.85f, true,  false },

    /* BLE hidden cameras / spy cameras */
    { "V380_",           "Generic",         "Spy Camera",    0.75f, true,  false },
    { "IPC_",            "Generic",         "Spy Camera",    0.70f, true,  false },
    { "LookCam_",        "Generic",         "Spy Camera",    0.70f, true,  false },
    { "Camera-",         "Generic",         "Spy Camera",    0.55f, true,  false },
    { "CLOUDCAM-",       "Generic",         "Spy Camera",    0.80f, true,  false },
    { "HIDVCAM-",        "Generic",         "Hidden Camera", 0.90f, true,  false },
    { "HDWiFiCam-",      "Generic",         "Hidden Camera", 0.85f, true,  false },

    /* Vehicles with cameras */
    { "Tesla ",          "Tesla",           "Vehicle Camera", 0.90f, true,  false },

    /* Attack / hacking tools */
    { "Flipper ",        "Flipper Zero",    "Attack Tool",   0.90f, false, false },

    /* Trackers (BLE name-based — supplement CID/UUID matching) */
    { "Tile",            "Tile",            "BLE Tracker",   0.70f, false, false },
    { "SmartTag",        "Samsung",         "BLE Tracker",   0.80f, false, false },
};
#define NAME_DB_COUNT (sizeof(s_name_db) / sizeof(s_name_db[0]))

/* ── GAP Appearance values ───────────────────────────────────────────── */

#define GAP_APPEARANCE_EYEGLASSES 0x01C0  /* Generic Eye-glasses (category 7) */

/* ── Matching logic ──────────────────────────────────────────────────── */

static bool match_name(const char *adv_name, int adv_name_len,
                       const name_pattern_entry_t *entry)
{
    if (adv_name == NULL || adv_name_len == 0) return false;
    int prefix_len = (int)strlen(entry->prefix);
    if (adv_name_len < prefix_len) return false;

    if (entry->exact) {
        return (adv_name_len == prefix_len &&
                strncasecmp(adv_name, entry->prefix, prefix_len) == 0);
    }
    return strncasecmp(adv_name, entry->prefix, prefix_len) == 0;
}

bool glasses_check_advertisement(
    const uint8_t mac[6],
    const char *adv_name,
    int adv_name_len,
    const uint8_t *mfr_data,
    int mfr_data_len,
    const uint16_t *svc_uuid16s,
    int svc_uuid16_count,
    uint16_t appearance,
    int8_t rssi,
    glasses_detection_t *out)
{
    float best_conf = 0.0f;
    const char *best_mfr = NULL;
    const char *best_type = NULL;
    bool best_camera = false;
    char best_reason[32] = {0};

    /* 1. Check manufacturer Company ID (highest signal) */
    if (mfr_data != NULL && mfr_data_len >= 2) {
        uint16_t cid = (uint16_t)(mfr_data[0] | (mfr_data[1] << 8)); /* LE */
        for (int i = 0; i < (int)MFR_CID_DB_COUNT; i++) {
            if (cid == s_mfr_cid_db[i].company_id) {
                float c = s_mfr_cid_db[i].confidence;
                if (c > best_conf) {
                    best_conf = c;
                    best_mfr = s_mfr_cid_db[i].manufacturer;
                    best_type = s_mfr_cid_db[i].device_type;
                    best_camera = s_mfr_cid_db[i].has_camera;
                    snprintf(best_reason, sizeof(best_reason),
                             "mfr_cid:0x%04X", cid);
                }
                break;
            }
        }
    }

    /* 1b. Special Apple manufacturer data parsing (AirTag, FindMy) */
    if (mfr_data != NULL && mfr_data_len >= 4) {
        uint16_t cid = (uint16_t)(mfr_data[0] | (mfr_data[1] << 8));
        if (cid == 0x004C) { /* Apple Inc. */
            uint8_t apple_type = mfr_data[2];
            if (apple_type == 0x12) {
                /* AirTag / FindMy accessory */
                float c = 0.95f;
                if (c > best_conf) {
                    best_conf = c;
                    best_mfr = "Apple";
                    best_type = "AirTag/FindMy";
                    best_camera = false;
                    snprintf(best_reason, sizeof(best_reason), "apple_findmy:0x12");
                }
            } else if (apple_type == 0x02 && mfr_data_len >= 23) {
                /* iBeacon (retail tracking) — type 0x02 + 21 bytes payload */
                float c = 0.70f;
                if (c > best_conf) {
                    best_conf = c;
                    best_mfr = "Apple";
                    best_type = "Tracking Beacon";
                    best_camera = false;
                    snprintf(best_reason, sizeof(best_reason), "ibeacon:0x02");
                }
            }
        }
    }

    /* 2. Check 16-bit Service UUIDs */
    for (int u = 0; u < svc_uuid16_count; u++) {
        for (int i = 0; i < (int)SVC_UUID_DB_COUNT; i++) {
            if (svc_uuid16s[u] == s_svc_uuid_db[i].uuid16) {
                float c = s_svc_uuid_db[i].confidence;
                if (c > best_conf) {
                    best_conf = c;
                    best_mfr = s_svc_uuid_db[i].manufacturer;
                    best_type = s_svc_uuid_db[i].device_type;
                    best_camera = s_svc_uuid_db[i].has_camera;
                    snprintf(best_reason, sizeof(best_reason),
                             "uuid16:0x%04X", svc_uuid16s[u]);
                }
            }
        }
    }

    /* 3. Check device name prefixes */
    for (int i = 0; i < (int)NAME_DB_COUNT; i++) {
        if (match_name(adv_name, adv_name_len, &s_name_db[i])) {
            float c = s_name_db[i].confidence;
            if (c > best_conf) {
                best_conf = c;
                best_mfr = s_name_db[i].manufacturer;
                best_type = s_name_db[i].device_type;
                best_camera = s_name_db[i].has_camera;
                snprintf(best_reason, sizeof(best_reason),
                         "name:%.*s", 20, s_name_db[i].prefix);
            }
        }
    }

    /* 4. Check GAP Appearance for eyeglasses (low confidence booster) */
    if (appearance == GAP_APPEARANCE_EYEGLASSES && best_conf == 0.0f) {
        best_conf = 0.50f;
        best_mfr = "Unknown";
        best_type = "Smart Glasses";
        best_camera = true;  /* assume camera — safer */
        snprintf(best_reason, sizeof(best_reason), "appearance:0x%04X", appearance);
    } else if (appearance == GAP_APPEARANCE_EYEGLASSES && best_conf > 0.0f) {
        /* Boost existing match confidence */
        best_conf = (best_conf + 0.10f > 1.0f) ? 1.0f : best_conf + 0.10f;
    }

    if (best_conf < 0.50f) return false;

    /* Populate output */
    int64_t now_ms = esp_timer_get_time() / 1000;
    memset(out, 0, sizeof(*out));
    memcpy(out->mac, mac, 6);
    out->rssi = rssi;
    out->confidence = best_conf;
    out->has_camera = best_camera;
    out->first_seen_ms = now_ms;
    out->last_seen_ms = now_ms;

    if (adv_name && adv_name_len > 0) {
        int copy_len = adv_name_len < (int)sizeof(out->device_name) - 1
                       ? adv_name_len : (int)sizeof(out->device_name) - 1;
        memcpy(out->device_name, adv_name, copy_len);
        out->device_name[copy_len] = '\0';
    }

    if (best_mfr) {
        strncpy(out->manufacturer, best_mfr, sizeof(out->manufacturer) - 1);
    }
    if (best_type) {
        strncpy(out->device_type, best_type, sizeof(out->device_type) - 1);
    }
    strncpy(out->match_reason, best_reason, sizeof(out->match_reason) - 1);

    ESP_LOGI(TAG, "Detected %s (%s) RSSI=%d conf=%.2f [%s] cam=%s",
             out->device_type, out->manufacturer, rssi, best_conf,
             best_reason, best_camera ? "YES" : "no");

    return true;
}
