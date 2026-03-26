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
    /* Google CID 0x00E0 removed — too broad, matches Nest/Chromecast/Pixel */
    { 0x060C, "Vuzix",          "Smart Glasses", 0.85f, true  }, /* Vuzix Corporation */
    { 0x009E, "Bose",           "Audio Glasses", 0.75f, false }, /* Bose Corporation */
    { 0x009F, "Bose",           "Audio Glasses", 0.75f, false }, /* Bose Corporation (alt) */
    { 0x034D, "Axon",           "Body Camera",   0.85f, true  }, /* TASER/Axon Enterprise */

    /* MEDIUM confidence */
    { 0x09B1, "Brilliant Labs", "Smart Glasses", 0.80f, true  }, /* Brilliant Labs Ltd. */
    { 0x0BC6, "TCL",            "Smart Glasses", 0.70f, true  }, /* TCL Communication */
    { 0x0962, "Rokid",          "Smart Glasses", 0.75f, true  }, /* Shanghai Lingban Tech */

    /* LOW confidence — too broad, many non-glasses devices */
    /* Amazon CID 0x0171 removed — too broad, matches Echo/Fire/Kindle/Ring */

    /* Trackers removed from ESP32 — focus on glasses + drones, not FindMy/AirTag/Tile */

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

    /* Trackers removed from ESP32 — focus on glasses + drones */
    { 0xFE2C, "Google",  "Fast Pair",     0.50f, false }, /* Google Fast Pair — below threshold */
    { 0xFEAA, "Google",  "Eddystone Beacon", 0.50f, false }, /* below threshold */
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
    { "Ray-Ban Meta",    "Meta",            "Smart Glasses", 0.95f, true,  false },
    { "Ray-Ban Stories", "Meta",            "Smart Glasses", 0.95f, true,  false },
    { "Oakley Meta",     "Meta",            "Smart Glasses", 0.95f, true,  false },
    { "Meta Neural",     "Meta",            "Smart Glasses", 0.90f, false, false },
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
    { "Glass EE",        "Google",          "Smart Glasses", 0.85f, true,  false },
    { "Bose Frames",     "Bose",            "Audio Glasses", 0.90f, false, false },

    /* Body cameras / spy cameras (BLE names) */
    { "Axon Body",       "Axon",            "Body Camera",   0.90f, true,  false },
    { "Axon Signal",     "Axon",            "Body Camera",   0.85f, true,  false },
    { "VISTA_",          "Motorola",        "Body Camera",   0.85f, true,  false },

    /* Surveillance / security cameras (BLE setup mode) */
    { "Nest Cam",        "Google",          "Surveillance",  0.85f, true,  false },
    { "Nest Hello",      "Google",          "Doorbell Cam",  0.85f, true,  false },
    { "Nest Doorbell",   "Google",          "Doorbell Cam",  0.85f, true,  false },
    { "Arlo ",           "Arlo",            "Surveillance",  0.80f, true,  false },
    { "Wyze Cam",        "Wyze",            "Surveillance",  0.80f, true,  false },
    { "eufy Indoor",     "Eufy",            "Surveillance",  0.80f, true,  false },
    { "eufy Doorbell",   "Eufy",            "Doorbell Cam",  0.80f, true,  false },
    { "SimpliSafe",      "SimpliSafe",      "Surveillance",  0.75f, true,  false },
    { "Verkada",         "Verkada",         "Surveillance",  0.90f, true,  false },
    { "Rhombus",         "Rhombus",         "Surveillance",  0.85f, true,  false },
    { "Reolink",         "Reolink",         "Surveillance",  0.75f, true,  false },

    /* ALPR / license plate readers */
    { "Flock",           "Flock Safety",    "ALPR Camera",   0.90f, true,  false },
    { "FLK-",            "Flock Safety",    "ALPR Camera",   0.85f, true,  false },
    { "ELSAG",           "Leonardo",        "ALPR Camera",   0.90f, true,  false },
    { "AutoVu",          "Genetec",         "ALPR Camera",   0.85f, true,  false },
    { "Vigilant",        "Motorola",        "ALPR Camera",   0.80f, true,  false },

    /* Police / fleet cameras */
    { "Axon Fleet",      "Axon",            "Police Camera", 0.90f, true,  false },
    { "WatchGuard",      "Motorola",        "Police Camera", 0.85f, true,  false },

    /* BLE hidden cameras / spy cameras */
    { "V380_",           "Generic",         "Spy Camera",    0.75f, true,  false },
    { "IPC_",            "Generic",         "Spy Camera",    0.70f, true,  false },
    { "LookCam_",        "Generic",         "Spy Camera",    0.70f, true,  false },
    { "CLOUDCAM-",       "Generic",         "Spy Camera",    0.80f, true,  false },
    { "HIDVCAM-",        "Generic",         "Hidden Camera", 0.90f, true,  false },
    { "HDWiFiCam-",      "Generic",         "Hidden Camera", 0.85f, true,  false },

    /* Vehicles with cameras */
    { "Tesla ",          "Tesla",           "Vehicle Camera", 0.90f, true,  false },

    /* Attack / hacking tools */
    { "Flipper ",        "Flipper Zero",    "Attack Tool",   0.90f, false, false },

    /* Smart speakers (always-listening) */
    { "Sonos Move",      "Sonos",           "Smart Speaker", 0.85f, false, false },
    { "Sonos Roam",      "Sonos",           "Smart Speaker", 0.85f, false, false },
    { "Sonos ",          "Sonos",           "Smart Speaker", 0.75f, false, false },

    /* Smart home hubs (always-listening microphones) */
    { "Google Home",     "Google",          "Smart Hub",     0.80f, false, false },
    { "Google Nest",     "Google",          "Smart Hub",     0.80f, false, false },
    { "Nest Mini",       "Google",          "Smart Hub",     0.85f, false, false },
    { "Nest Audio",      "Google",          "Smart Hub",     0.85f, false, false },
    { "Nest Hub",        "Google",          "Smart Hub",     0.85f, false, false },
    { "HomePod",         "Apple",           "Smart Hub",     0.85f, false, false },

    /* Smart locks (physical security) */
    { "August ",         "August",          "Smart Lock",    0.70f, false, false },
    { "Schlage",         "Schlage",         "Smart Lock",    0.75f, false, false },
    { "Level Lock",      "Level",           "Smart Lock",    0.75f, false, false },

    /* Baby monitors (camera + microphone) */
    { "Owlet",           "Owlet",           "Baby Monitor",  0.85f, true,  false },
    { "Miku-",           "Miku",            "Baby Monitor",  0.85f, true,  false },
    { "CuboAi-",         "CuboAi",          "Baby Monitor",  0.85f, true,  false },
    { "Lollipop-",       "Lollipop",        "Baby Monitor",  0.80f, true,  false },
    { "iBaby-",          "iBaby",           "Baby Monitor",  0.80f, true,  false },

    /* More security cameras */
    { "EZVIZ_",          "EZVIZ",           "Surveillance",  0.80f, true,  false },
    { "Lorex_",          "Lorex",           "Surveillance",  0.80f, true,  false },
    { "ZOSI_",           "ZOSI",            "Surveillance",  0.75f, true,  false },
    { "Swann",           "Swann",           "Surveillance",  0.80f, true,  false },

    /* OBD2 / car diagnostic dongles */
    { "ELM327",          "Generic",         "OBD Tracker",   0.80f, false, false },
    { "VEEPEAK",         "Veepeak",         "OBD Tracker",   0.85f, false, false },
    { "BlueDriver",      "BlueDriver",      "OBD Tracker",   0.85f, false, false },
    { "FIXD",            "Fixd",            "OBD Tracker",   0.85f, false, false },
    { "OBDLink",         "OBDLink",         "OBD Tracker",   0.85f, false, false },
    { "OBDII",           "Generic",         "OBD Tracker",   0.75f, false, false },

    /* GPS trackers */
    { "Tracki_",         "Tracki",          "GPS Tracker",   0.85f, false, false },
    { "Bouncie_",        "Bouncie",         "GPS Tracker",   0.85f, false, false },
    { "Invoxia_",        "Invoxia",         "GPS Tracker",   0.85f, false, false },
    { "LandAirSea",      "LandAirSea",      "GPS Tracker",   0.85f, false, false },

    /* Thermal cameras (see through walls / in dark) */
    { "FLIR-ONE-",       "FLIR",            "Thermal Cam",   0.90f, true,  false },
    { "InfiRay-",        "InfiRay",         "Thermal Cam",   0.85f, true,  false },
    { "Seek-",           "Seek Thermal",    "Thermal Cam",   0.85f, true,  false },

    /* Trail / game cameras (outdoor surveillance) */
    { "Spypoint-",       "Spypoint",        "Trail Camera",  0.85f, true,  false },
    { "Bushnell-",       "Bushnell",        "Trail Camera",  0.80f, true,  false },
    { "Moultrie-",       "Moultrie",        "Trail Camera",  0.80f, true,  false },

    /* Smart TVs (ACR tracking) */
    { "[TV] Samsung",    "Samsung",         "Smart TV",      0.85f, false, false },
    { "[LG] webOS TV",   "LG",             "Smart TV",      0.85f, false, false },
    { "BRAVIA",          "Sony",            "Smart TV",      0.80f, false, false },

    /* DJI drone controllers */
    { "DJI-RC-",         "DJI",             "Drone Ctrl",    0.90f, false, false },

    /* E-Scooters (location tracking) */
    { "Lime-",           "Lime",            "E-Scooter",     0.80f, false, false },
    { "Bird ",           "Bird",            "E-Scooter",     0.75f, false, false },
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

    /* Apple AirTag/FindMy removed from ESP32 — focus on glasses + drones */

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

    if (best_conf < 0.60f) return false;

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
