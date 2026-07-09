#include "privacy_rf_signatures.h"

#include <ctype.h>
#include <string.h>

static char ascii_lower(char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
}

static bool ascii_starts_nocase(const char *text, const char *prefix)
{
    if (!text || !prefix || prefix[0] == '\0') {
        return false;
    }
    while (*prefix) {
        if (*text == '\0' || ascii_lower(*text) != ascii_lower(*prefix)) {
            return false;
        }
        text++;
        prefix++;
    }
    return true;
}

static bool ascii_eq_nocase(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }
    while (*a && *b) {
        if (ascii_lower(*a) != ascii_lower(*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static bool ascii_contains_nocase(const char *haystack, const char *needle)
{
    if (!haystack || !needle || needle[0] == '\0') {
        return false;
    }
    for (const char *h = haystack; *h; h++) {
        const char *a = h;
        const char *b = needle;
        while (*a && *b && ascii_lower(*a) == ascii_lower(*b)) {
            a++;
            b++;
        }
        if (*b == '\0') {
            return true;
        }
    }
    return false;
}

static const fof_privacy_wifi_signature_t WIFI_SIGNATURES[] = {
    /* Hidden/IP camera setup APs */
    { "V380-", FOF_PRIVACY_MATCH_PREFIX, "V380", "Hidden Camera", "CAMERA_NEAR", "privacy:camera:v380", 0.85f, true, false },
    { "YDXJ_", FOF_PRIVACY_MATCH_PREFIX, "YI", "IP Camera", "CAMERA_NEAR", "privacy:camera:yi", 0.85f, true, false },
    { "IPC-", FOF_PRIVACY_MATCH_PREFIX, "Generic", "IP Camera", "CAMERA_NEAR", "privacy:camera:ipcam", 0.75f, true, false },
    { "IPCAM-", FOF_PRIVACY_MATCH_PREFIX, "Generic", "IP Camera", "CAMERA_NEAR", "privacy:camera:ipcam", 0.75f, true, false },
    { "IP_CAM_", FOF_PRIVACY_MATCH_PREFIX, "Generic", "IP Camera", "CAMERA_NEAR", "privacy:camera:ipcam", 0.80f, true, false },
    { "HDWiFiCam", FOF_PRIVACY_MATCH_PREFIX, "Generic", "Hidden Camera", "CAMERA_NEAR", "privacy:camera:hidden", 0.85f, true, false },
    { "CLOUDCAM", FOF_PRIVACY_MATCH_PREFIX, "Generic", "Hidden Camera", "CAMERA_NEAR", "privacy:camera:hidden", 0.80f, true, false },
    { "HIDVCAM", FOF_PRIVACY_MATCH_PREFIX, "Generic", "Hidden Camera", "CAMERA_NEAR", "privacy:camera:hidden", 0.90f, true, false },
    { "JXLCAM", FOF_PRIVACY_MATCH_PREFIX, "Generic", "Spy Camera", "CAMERA_NEAR", "privacy:camera:hidden", 0.85f, true, false },
    { "iCam-", FOF_PRIVACY_MATCH_PREFIX, "iCam365", "Hidden Camera", "CAMERA_NEAR", "privacy:camera:icam", 0.80f, true, false },
    { "iCam365-", FOF_PRIVACY_MATCH_PREFIX, "iCam365", "Hidden Camera", "CAMERA_NEAR", "privacy:camera:icam", 0.80f, true, false },
    { "XM-", FOF_PRIVACY_MATCH_PREFIX, "XMeye", "IP Camera", "CAMERA_NEAR", "privacy:camera:xmeye", 0.80f, true, false },
    { "CareCam-", FOF_PRIVACY_MATCH_PREFIX, "CareCam", "Hidden Camera", "CAMERA_NEAR", "privacy:camera:carecam", 0.80f, true, false },
    { "BVCAM-", FOF_PRIVACY_MATCH_PREFIX, "BVCAM", "Hidden Camera", "CAMERA_NEAR", "privacy:camera:bvcam", 0.80f, true, false },
    { "P2PCam-", FOF_PRIVACY_MATCH_PREFIX, "P2PCam", "Hidden Camera", "CAMERA_NEAR", "privacy:camera:p2pcam", 0.80f, true, false },
    { "TUTK-", FOF_PRIVACY_MATCH_PREFIX, "ThroughTek", "IP Camera", "CAMERA_NEAR", "privacy:camera:tutk", 0.75f, true, false },
    { "WIFI-CAM", FOF_PRIVACY_MATCH_PREFIX, "Generic", "Hidden Camera", "CAMERA_NEAR", "privacy:camera:hidden", 0.75f, true, false },
    { "DEPSTECH_", FOF_PRIVACY_MATCH_PREFIX, "DEPSTECH", "Endoscope Camera", "CAMERA_NEAR", "privacy:camera:endoscope", 0.85f, true, false },

    /* Surveillance, doorbell, body, dash, action, and baby cameras */
    { "Tapo_Cam_", FOF_PRIVACY_MATCH_PREFIX, "TP-Link", "IP Camera", "CAMERA_NEAR", "privacy:camera:tapo", 0.88f, true, false },
    { "Tapo_", FOF_PRIVACY_MATCH_PREFIX, "TP-Link", "IP Camera", "CAMERA_NEAR", "privacy:camera:tapo", 0.80f, true, false },
    { "TAPO-", FOF_PRIVACY_MATCH_PREFIX, "TP-Link", "IP Camera", "CAMERA_NEAR", "privacy:camera:tapo", 0.80f, true, false },
    { "Reolink_", FOF_PRIVACY_MATCH_PREFIX, "Reolink", "IP Camera", "CAMERA_NEAR", "privacy:camera:reolink", 0.80f, true, false },
    { "Wyze_", FOF_PRIVACY_MATCH_PREFIX, "Wyze", "IP Camera", "CAMERA_NEAR", "privacy:camera:wyze", 0.80f, true, false },
    { "Amcrest_", FOF_PRIVACY_MATCH_PREFIX, "Amcrest", "IP Camera", "CAMERA_NEAR", "privacy:camera:amcrest", 0.80f, true, false },
    { "Hik-", FOF_PRIVACY_MATCH_PREFIX, "Hikvision", "IP Camera", "CAMERA_NEAR", "privacy:camera:hikvision", 0.85f, true, false },
    { "HIKVISION_", FOF_PRIVACY_MATCH_PREFIX, "Hikvision", "IP Camera", "CAMERA_NEAR", "privacy:camera:hikvision", 0.85f, true, false },
    { "DH_", FOF_PRIVACY_MATCH_PREFIX, "Dahua", "IP Camera", "CAMERA_NEAR", "privacy:camera:dahua", 0.80f, true, false },
    { "EZVIZ_", FOF_PRIVACY_MATCH_PREFIX, "EZVIZ", "Surveillance Camera", "CAMERA_NEAR", "privacy:camera:ezviz", 0.80f, true, false },
    { "Lorex_", FOF_PRIVACY_MATCH_PREFIX, "Lorex", "Surveillance Camera", "CAMERA_NEAR", "privacy:camera:lorex", 0.80f, true, false },
    { "Swann-SWIFI-", FOF_PRIVACY_MATCH_PREFIX, "Swann", "Surveillance Camera", "CAMERA_NEAR", "privacy:camera:swann", 0.85f, true, false },
    { "Annke_", FOF_PRIVACY_MATCH_PREFIX, "Annke", "Surveillance Camera", "CAMERA_NEAR", "privacy:camera:annke", 0.75f, true, false },
    { "Verkada-", FOF_PRIVACY_MATCH_PREFIX, "Verkada", "Surveillance Camera", "CAMERA_NEAR", "privacy:camera:verkada", 0.90f, true, false },
    { "Rhombus-", FOF_PRIVACY_MATCH_PREFIX, "Rhombus", "Surveillance Camera", "CAMERA_NEAR", "privacy:camera:rhombus", 0.85f, true, false },
    { "Ring Setup", FOF_PRIVACY_MATCH_PREFIX, "Ring", "Doorbell Camera", "CAMERA_NEAR", "privacy:doorbell:ring", 0.85f, true, false },
    { "Ring-", FOF_PRIVACY_MATCH_PREFIX, "Ring", "Doorbell Camera", "CAMERA_NEAR", "privacy:doorbell:ring", 0.80f, true, false },
    { "SimpliSafe-", FOF_PRIVACY_MATCH_PREFIX, "SimpliSafe", "Surveillance Camera", "CAMERA_NEAR", "privacy:camera:simplisafe", 0.80f, true, false },
    { "Blink-", FOF_PRIVACY_MATCH_PREFIX, "Blink", "Surveillance Camera", "CAMERA_NEAR", "privacy:camera:blink", 0.80f, true, false },
    { "BLINK-", FOF_PRIVACY_MATCH_PREFIX, "Blink", "Surveillance Camera", "CAMERA_NEAR", "privacy:camera:blink", 0.80f, true, false },
    { "Arlo-VMB", FOF_PRIVACY_MATCH_PREFIX, "Arlo", "IP Camera", "CAMERA_NEAR", "privacy:camera:arlo", 0.78f, true, false },
    { "RemoBellS_", FOF_PRIVACY_MATCH_PREFIX, "Remo+", "Doorbell Camera", "CAMERA_NEAR", "privacy:doorbell:remobell", 0.80f, true, false },
    { "RemoBellW_", FOF_PRIVACY_MATCH_PREFIX, "Remo+", "Doorbell Camera", "CAMERA_NEAR", "privacy:doorbell:remobell", 0.80f, true, false },
    { "Axon Body", FOF_PRIVACY_MATCH_PREFIX, "Axon", "Body Camera", "CAMERA_NEAR", "privacy:bodycam:axon", 0.90f, true, false },
    { "WGVISTA", FOF_PRIVACY_MATCH_PREFIX, "Motorola", "Body Camera", "CAMERA_NEAR", "privacy:bodycam:motorola", 0.85f, true, false },
    { "BlackVue", FOF_PRIVACY_MATCH_PREFIX, "BlackVue", "Dash Camera", "CAMERA_NEAR", "privacy:dashcam:blackvue", 0.90f, true, false },
    { "DR900", FOF_PRIVACY_MATCH_PREFIX, "BlackVue", "Dash Camera", "CAMERA_NEAR", "privacy:dashcam:blackvue", 0.85f, true, false },
    { "DR750", FOF_PRIVACY_MATCH_PREFIX, "BlackVue", "Dash Camera", "CAMERA_NEAR", "privacy:dashcam:blackvue", 0.85f, true, false },
    { "VIOFO", FOF_PRIVACY_MATCH_PREFIX, "Viofo", "Dash Camera", "CAMERA_NEAR", "privacy:dashcam:viofo", 0.85f, true, false },
    { "VIOFO_", FOF_PRIVACY_MATCH_PREFIX, "Viofo", "Dash Camera", "CAMERA_NEAR", "privacy:dashcam:viofo", 0.85f, true, false },
    { "70mai_", FOF_PRIVACY_MATCH_PREFIX, "70mai", "Dash Camera", "CAMERA_NEAR", "privacy:dashcam:70mai", 0.85f, true, false },
    { "Nextbase", FOF_PRIVACY_MATCH_PREFIX, "Nextbase", "Dash Camera", "CAMERA_NEAR", "privacy:dashcam:nextbase", 0.85f, true, false },
    { "Thinkware", FOF_PRIVACY_MATCH_PREFIX, "Thinkware", "Dash Camera", "CAMERA_NEAR", "privacy:dashcam:thinkware", 0.85f, true, false },
    { "DDPai_", FOF_PRIVACY_MATCH_PREFIX, "DDPai", "Dash Camera", "CAMERA_NEAR", "privacy:dashcam:ddpai", 0.85f, true, false },
    { "GoPro", FOF_PRIVACY_MATCH_PREFIX, "GoPro", "Action Camera", "CAMERA_NEAR", "privacy:actioncam:gopro", 0.90f, true, false },
    { "Insta360", FOF_PRIVACY_MATCH_PREFIX, "Insta360", "Action Camera", "CAMERA_NEAR", "privacy:actioncam:insta360", 0.90f, true, false },
    { "OsmoAction", FOF_PRIVACY_MATCH_PREFIX, "DJI", "Action Camera", "CAMERA_NEAR", "privacy:actioncam:dji", 0.90f, true, false },
    { "OwletCam-", FOF_PRIVACY_MATCH_PREFIX, "Owlet", "Baby Monitor", "CAMERA_NEAR", "privacy:baby_monitor:owlet", 0.85f, true, false },
    { "Miku-", FOF_PRIVACY_MATCH_PREFIX, "Miku", "Baby Monitor", "CAMERA_NEAR", "privacy:baby_monitor:miku", 0.85f, true, false },
    { "CuboAi-", FOF_PRIVACY_MATCH_PREFIX, "CuboAi", "Baby Monitor", "CAMERA_NEAR", "privacy:baby_monitor:cuboai", 0.85f, true, false },

    /* ALPR / fleet surveillance */
    { "ELSAG-", FOF_PRIVACY_MATCH_PREFIX, "Leonardo", "ALPR Camera", "FLOCK_ALPR", "privacy:alpr:elsag", 0.85f, true, false },

    /* Passive attack-tool indicators */
    { "Pineapple_", FOF_PRIVACY_MATCH_PREFIX, "Hak5", "Attack Tool", "WIFI_ATTACK_TOOL", "attack_tool:pineapple", 0.95f, false, true },
    { "Pineapple", FOF_PRIVACY_MATCH_PREFIX, "Hak5", "Attack Tool", "WIFI_ATTACK_TOOL", "attack_tool:pineapple", 0.90f, false, true },
    { "pwned", FOF_PRIVACY_MATCH_PREFIX, "Spacehuhn", "Attack Tool", "WIFI_ATTACK_TOOL", "attack_tool:deauther", 0.95f, false, true },
    { "pwnd", FOF_PRIVACY_MATCH_PREFIX, "Spacehuhn", "Attack Tool", "WIFI_ATTACK_TOOL", "attack_tool:deauther", 0.95f, false, true },
    { "Advanced-Deauther", FOF_PRIVACY_MATCH_PREFIX, "Deauther", "Attack Tool", "WIFI_ATTACK_TOOL", "attack_tool:deauther", 0.95f, false, true },
    { "Marauder", FOF_PRIVACY_MATCH_PREFIX, "ESP32Marauder", "Attack Tool", "WIFI_ATTACK_TOOL", "attack_tool:marauder", 0.90f, false, true },
    { "ESP32Marauder", FOF_PRIVACY_MATCH_PREFIX, "ESP32Marauder", "Attack Tool", "WIFI_ATTACK_TOOL", "attack_tool:marauder", 0.90f, false, true },
    { "pwnagotchi", FOF_PRIVACY_MATCH_PREFIX, "Pwnagotchi", "Attack Tool", "WIFI_ATTACK_TOOL", "attack_tool:pwnagotchi", 0.90f, false, true },
};

const fof_privacy_wifi_signature_t *fof_privacy_wifi_signatures(size_t *count)
{
    if (count) {
        *count = sizeof(WIFI_SIGNATURES) / sizeof(WIFI_SIGNATURES[0]);
    }
    return WIFI_SIGNATURES;
}

bool fof_privacy_pattern_is_banned_broad(const char *pattern)
{
    if (!pattern || pattern[0] == '\0') {
        return true;
    }
    static const char *banned[] = {
        "MV", "HOLY", "UFO-", "CAM", "CAM-", "CAM_", "PORTAL", "EVIL", "TWIN",
    };
    for (size_t i = 0; i < sizeof(banned) / sizeof(banned[0]); i++) {
        if (ascii_eq_nocase(pattern, banned[i])) {
            return true;
        }
    }
    if (strlen(pattern) < 3) {
        return true;
    }
    return false;
}

const fof_privacy_wifi_signature_t *fof_privacy_match_wifi_ssid(const char *ssid)
{
    if (!ssid || ssid[0] == '\0') {
        return NULL;
    }

    size_t count = 0;
    const fof_privacy_wifi_signature_t *signatures =
        fof_privacy_wifi_signatures(&count);
    for (size_t i = 0; i < count; i++) {
        const fof_privacy_wifi_signature_t *sig = &signatures[i];
        switch (sig->match_type) {
            case FOF_PRIVACY_MATCH_EXACT:
                if (ascii_eq_nocase(ssid, sig->pattern)) {
                    return sig;
                }
                break;
            case FOF_PRIVACY_MATCH_CONTAINS:
                if (ascii_contains_nocase(ssid, sig->pattern)) {
                    return sig;
                }
                break;
            case FOF_PRIVACY_MATCH_PREFIX:
            default:
                if (ascii_starts_nocase(ssid, sig->pattern)) {
                    return sig;
                }
                break;
        }
    }
    return NULL;
}
