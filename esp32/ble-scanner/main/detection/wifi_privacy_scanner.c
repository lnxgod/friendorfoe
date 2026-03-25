/**
 * @file wifi_privacy_scanner.c
 * @brief WiFi privacy scanner — detects hidden cameras and Meta hotspots.
 *
 * Runs periodic WiFi scans and matches SSIDs against a database of
 * known hidden camera, spy camera, and smart glasses hotspot patterns.
 */

#include "wifi_privacy_scanner.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"

#include <string.h>
#include <stdbool.h>

static const char *TAG = "wifi_priv";

/* ── SSID pattern database ─────────────────────────────────────────────── */

typedef struct {
    const char *prefix;
    const char *device_type;
    const char *manufacturer;
    float       confidence;
    bool        has_camera;
} wifi_ssid_pattern_t;

static const wifi_ssid_pattern_t s_patterns[] = {
    /* Hidden cameras / spy cameras */
    { "MV",          "Hidden Camera",  "V380",       0.85f, true  },
    { "V380-",       "Hidden Camera",  "V380",       0.85f, true  },
    { "YDXJ_",       "IP Camera",      "YI",         0.85f, true  },
    { "IPC-",        "IP Camera",      "Generic",    0.75f, true  },
    { "IPCAM-",      "IP Camera",      "Generic",    0.75f, true  },
    { "IP_CAM_",     "IP Camera",      "Generic",    0.80f, true  },
    { "HDWiFiCam",   "Hidden Camera",  "Generic",    0.85f, true  },
    { "CLOUDCAM",    "Hidden Camera",  "Generic",    0.80f, true  },
    { "HIDVCAM",     "Hidden Camera",  "Generic",    0.90f, true  },
    { "XM-",         "IP Camera",      "XMeye",      0.80f, true  },
    { "BVCAM-",      "Hidden Camera",  "BVCAM",      0.80f, true  },
    { "P2PCam-",     "Hidden Camera",  "P2PCam",     0.80f, true  },
    { "TUTK-",       "IP Camera",      "ThroughTek", 0.75f, true  },
    { "JXLCAM",      "Spy Camera",     "Generic",    0.85f, true  },
    { "CareCam-",    "Hidden Camera",  "CareCam",    0.80f, true  },
    { "iCam-",       "Hidden Camera",  "iCam365",    0.80f, true  },
    { "AI_",         "Hidden Camera",  "TinyCam",    0.80f, true  },
    { "WIFI-CAM",    "Hidden Camera",  "Generic",    0.75f, true  },
    { "DEPSTECH_",   "Endoscope",      "DEPSTECH",   0.85f, true  },
    { "Hik-",        "IP Camera",      "Hikvision",  0.85f, true  },
    { "HIKVISION_",  "IP Camera",      "Hikvision",  0.85f, true  },
    { "DH_",         "IP Camera",      "Dahua",      0.80f, true  },
    { "Tapo_",       "IP Camera",      "TP-Link",    0.80f, true  },
    { "TAPO-",       "IP Camera",      "TP-Link",    0.80f, true  },
    { "Reolink_",    "IP Camera",      "Reolink",    0.80f, true  },
    { "Wyze_",       "IP Camera",      "Wyze",       0.80f, true  },
    { "Amcrest_",    "IP Camera",      "Amcrest",    0.80f, true  },
    /* Action cameras / dashcams */
    { "GoPro",       "Action Camera",  "GoPro",      0.90f, true  },
    { "Insta360",    "Action Camera",  "Insta360",   0.90f, true  },
    { "OsmoAction",  "Action Camera",  "DJI",        0.90f, true  },
    { "BlackVue",    "Dash Camera",    "BlackVue",   0.90f, true  },
    { "VIOFO_",      "Dash Camera",    "Viofo",      0.85f, true  },
    { "70mai_",      "Dash Camera",    "70mai",      0.85f, true  },
    { "Nextbase",    "Dash Camera",    "Nextbase",   0.85f, true  },
    { "Thinkware",   "Dash Camera",    "Thinkware",  0.85f, true  },
    /* Body cameras */
    { "Axon Body",   "Body Camera",    "Axon",       0.90f, true  },
    { "WGVISTA",     "Body Camera",    "Motorola",   0.85f, true  },
    /* Attack tools */
    { "Pineapple",   "Attack Tool",    "Hak5",       0.90f, false },
    /* Smart home cameras */
    { "Ring Setup",  "Doorbell Cam",   "Ring",       0.85f, true  },
    { "Blink-",      "IP Camera",      "Amazon",     0.80f, true  },
    { "Furbo_",      "Pet Camera",     "Furbo",      0.80f, true  },
};
#define PATTERN_COUNT (sizeof(s_patterns) / sizeof(s_patterns[0]))

/* ── Meta WiFi OUI prefixes (Luxottica/Meta/Oculus hardware) ────────── */
/* These are checked against BSSID when a Meta BLE detection is active */
static const char *s_meta_oui_prefixes[] = {
    "78:C4:FA",   /* Luxottica (confirmed) */
    "48:05:60",   /* Meta/Luxottica */
    "00:1D:BA",   /* Luxottica */
    "2C:26:17",   /* Oculus VR */
    "E0:D5:5E",   /* Meta Platforms */
};
#define META_OUI_COUNT (sizeof(s_meta_oui_prefixes) / sizeof(s_meta_oui_prefixes[0]))

static bool s_initialized = false;
static bool s_meta_transfer = false;

/* ── Init ──────────────────────────────────────────────────────────────── */

void wifi_privacy_init(void)
{
    if (s_initialized) return;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi privacy scanner initialized");
}

/* ── Scan ──────────────────────────────────────────────────────────────── */

int wifi_privacy_scan(wifi_privacy_result_t *results, int max_results)
{
    if (!s_initialized || !results || max_results <= 0) return 0;

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 100, .max = 300 } },
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true /* blocking */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        esp_wifi_scan_get_ap_records(&ap_count, NULL);
        return 0;
    }

    uint16_t fetch = ap_count < 30 ? ap_count : 30;
    wifi_ap_record_t *ap_list = calloc(fetch, sizeof(wifi_ap_record_t));
    if (!ap_list) return 0;

    esp_wifi_scan_get_ap_records(&fetch, ap_list);

    int match_count = 0;
    s_meta_transfer = false;

    for (int i = 0; i < fetch && match_count < max_results; i++) {
        const char *ssid = (const char *)ap_list[i].ssid;
        if (ssid[0] == '\0') continue;

        /* Check against SSID pattern database */
        for (int p = 0; p < (int)PATTERN_COUNT; p++) {
            int prefix_len = strlen(s_patterns[p].prefix);
            if (strncasecmp(ssid, s_patterns[p].prefix, prefix_len) == 0) {
                wifi_privacy_result_t *r = &results[match_count++];
                strncpy(r->ssid, ssid, sizeof(r->ssid) - 1);
                snprintf(r->bssid, sizeof(r->bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
                    ap_list[i].bssid[0], ap_list[i].bssid[1], ap_list[i].bssid[2],
                    ap_list[i].bssid[3], ap_list[i].bssid[4], ap_list[i].bssid[5]);
                r->rssi = ap_list[i].rssi;
                strncpy(r->device_type, s_patterns[p].device_type, sizeof(r->device_type) - 1);
                strncpy(r->manufacturer, s_patterns[p].manufacturer, sizeof(r->manufacturer) - 1);
                r->confidence = s_patterns[p].confidence;
                r->has_camera = s_patterns[p].has_camera;
                r->is_open = (ap_list[i].authmode == WIFI_AUTH_OPEN);

                ESP_LOGI(TAG, "WiFi MATCH: %s (%s) RSSI=%d open=%d [%s]",
                    ssid, r->device_type, r->rssi, r->is_open, r->manufacturer);
                break;
            }
        }

        /* Check for Meta WiFi transfer hotspot by OUI */
        char bssid_str[18];
        snprintf(bssid_str, sizeof(bssid_str), "%02X:%02X:%02X",
            ap_list[i].bssid[0], ap_list[i].bssid[1], ap_list[i].bssid[2]);
        for (int m = 0; m < (int)META_OUI_COUNT; m++) {
            if (strncasecmp(bssid_str, s_meta_oui_prefixes[m], 8) == 0) {
                s_meta_transfer = true;
                ESP_LOGW(TAG, "META WiFi HOTSPOT detected! SSID=%s BSSID=%s RSSI=%d",
                    ssid, bssid_str, ap_list[i].rssi);

                if (match_count < max_results) {
                    wifi_privacy_result_t *r = &results[match_count++];
                    strncpy(r->ssid, ssid, sizeof(r->ssid) - 1);
                    snprintf(r->bssid, sizeof(r->bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
                        ap_list[i].bssid[0], ap_list[i].bssid[1], ap_list[i].bssid[2],
                        ap_list[i].bssid[3], ap_list[i].bssid[4], ap_list[i].bssid[5]);
                    r->rssi = ap_list[i].rssi;
                    strncpy(r->device_type, "Meta Transfer", sizeof(r->device_type) - 1);
                    strncpy(r->manufacturer, "Meta", sizeof(r->manufacturer) - 1);
                    r->confidence = 0.95f;
                    r->has_camera = true;
                    r->is_open = (ap_list[i].authmode == WIFI_AUTH_OPEN);
                }
                break;
            }
        }
    }

    free(ap_list);
    ESP_LOGI(TAG, "WiFi scan: %d APs found, %d privacy matches", (int)fetch, match_count);
    return match_count;
}

bool wifi_privacy_meta_transfer_detected(void)
{
    return s_meta_transfer;
}

void wifi_privacy_deinit(void)
{
    if (!s_initialized) return;
    esp_wifi_stop();
    esp_wifi_deinit();
    s_initialized = false;
}
