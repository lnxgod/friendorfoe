/**
 * Friend or Foe -- WiFi SSID Pattern Matching
 *
 * 99 known drone SSID prefix patterns ported from Android WifiDroneScanner.kt.
 * Each entry maps an SSID prefix to its manufacturer name.
 * Matching is case-insensitive prefix comparison using strncasecmp.
 */

#include "wifi_ssid_patterns.h"
#include <string.h>
#include <strings.h>  /* strncasecmp */

static const drone_ssid_pattern_t PATTERNS[] = {
    /* ── DJI ───────────────────────────────────────────────────────────────── */
    { "DJI-",       "DJI" },
    { "TELLO-",     "Ryze/DJI" },
    { "MAVIC-",     "DJI" },
    { "PHANTOM-",   "DJI" },
    { "INSPIRE-",   "DJI" },
    { "MINI SE-",   "DJI" },
    { "MINI2-",     "DJI" },
    { "MINI3-",     "DJI" },
    { "MINI4-",     "DJI" },
    { "SPARK-",     "DJI" },
    { "FPV-",       "DJI" },
    { "AVATA-",     "DJI" },
    { "AGRAS-",     "DJI" },
    { "MATRICE-",   "DJI" },
    { "AIR 2S-",    "DJI" },
    { "AIR2-",      "DJI" },
    { "FLIP-",      "DJI" },
    { "DJI NEO-",   "DJI" },

    /* ── Skydio / Parrot / Autel ───────────────────────────────────────────── */
    { "SKYDIO-",    "Skydio" },
    { "PARROT-",    "Parrot" },
    { "ANAFI-",     "Parrot" },
    { "BEBOP-",     "Parrot" },
    { "DISCO-",     "Parrot" },
    { "ARDRONE-",   "Parrot" },
    { "AUTEL-",     "Autel" },
    { "EVO-",       "Autel" },

    /* ── HOVERAir (Zero Zero Robotics) ─────────────────────────────────────── */
    { "HOVERAIR",   "HOVERAir" },
    { "HOVER AIR",  "HOVERAir" },
    { "HOVER_AIR",  "HOVERAir" },
    { "HOVER-AIR",  "HOVERAir" },
    { "HOVERAir",   "HOVERAir" },
    { "HOVER X1",   "HOVERAir" },
    { "HOVER-X1",   "HOVERAir" },
    { "HOVER_X1",   "HOVERAir" },
    { "X1PRO",      "HOVERAir" },
    { "X1-PRO",     "HOVERAir" },
    { "X1 PRO",     "HOVERAir" },

    /* ── Holy Stone ────────────────────────────────────────────────────────── */
    { "HOLY",       "Holy Stone" },
    { "HS-",        "Holy Stone" },

    /* ── Other known brands ────────────────────────────────────────────────── */
    { "SIMREX-",    "SIMREX" },
    { "NEHEME-",    "Neheme" },
    { "AOVO-",      "AOVO" },
    { "TENSSENX-",  "TENSSENX" },
    { "SNAPTAIN-",  "Snaptain" },
    { "POTENSIC-",  "Potensic" },
    { "RUKO-",      "Ruko" },
    { "SYMA-",      "Syma" },
    { "HUBSAN-",    "Hubsan" },
    { "EACHINE-",   "Eachine" },
    { "FIMI-",      "Fimi" },
    { "XIAOMI-",    "Xiaomi" },
    { "YUNEEC-",    "Yuneec" },
    { "TYPHOON-",   "Yuneec" },
    { "MANTIS-",    "Yuneec" },
    { "WINGSLAND-", "Wingsland" },
    { "BETAFPV-",   "BetaFPV" },
    { "GEPRC-",     "GEPRC" },
    { "EMAX-",      "EMAX" },

    /* ── Other brands ──────────────────────────────────────────────────────── */
    { "POWEREGG-",     "PowerVision" },
    { "DOBBY-",        "ZEROTECH" },
    { "SPLASHDRONE-",  "Swellpro" },
    { "CONTIXO-",      "Contixo" },
    { "SKYVIPER-",     "Sky Viper" },
    { "DROCON-",       "Drocon" },

    /* ── Enterprise / commercial ───────────────────────────────────────────── */
    { "FREEFLY-",      "Freefly" },
    { "SENSEFLY-",     "senseFly" },
    { "WINGCOPTER-",   "Wingcopter" },
    { "FLYABILITY-",   "Flyability" },

    /* ── FPV and hobby brands ──────────────────────────────────────────────── */
    { "IFLIGHT-",      "iFlight" },
    { "FLYWOO-",       "Flywoo" },
    { "WALKERA-",      "Walkera" },
    { "BLADE-",        "Blade" },
    { "CADDX-",        "Caddx" },
    { "WALKSNAIL-",    "Walksnail" },
    { "AVATAR-",       "Walksnail" },
    { "RUNCAM-",       "RunCam" },

    /* ── Budget Chinese drones / generic WiFi FPV ──────────────────────────── */
    { "WIFI-UAV",      "Generic" },
    { "WIFI_UAV",      "Generic" },
    { "WIFIUAV",       "Generic" },
    { "WiFi-720P",     "Generic" },
    { "WiFi-1080P",    "Generic" },
    { "WiFi-4K",       "Generic" },
    { "WIFI_CAMERA",   "Generic" },
    { "WiFi_FPV",      "Generic" },
    { "WiFi-FPV",      "Generic" },
    { "RCDrone",       "Generic" },
    { "RC-DRONE",      "Generic" },
    { "RCTOY",         "Generic" },
    { "UFO-",          "Generic" },

    /* ── Chinese brands using WiFi UAV-type apps ───────────────────────────── */
    { "JJRC-",         "JJRC" },
    { "MJX-",          "MJX" },
    { "VISUO-",        "Visuo" },
    { "SJRC-",         "SJRC" },
    { "4DRC-",         "4DRC" },
    { "FLYHAL-",       "Flyhal" },
    { "LYZRC-",        "LYZRC" },
    { "XINLIN-",       "Xinlin" },
    { "E58-",          "Eachine" },
    { "E88-",          "Eachine" },
    { "E99-",          "Eachine" },
    { "V2PRO",         "Generic" },

    /* ── Additional budget brands ────────────────────────────────────────── */
    { "WLTOYS-",       "WLtoys" },
    { "ATTOP-",        "Attop" },
    { "BUGS-",         "MJX" },
    { "EHANG-",        "EHang" },

    /* ── Generic drone SSIDs ───────────────────────────────────────────────── */
    { "DRONE-",        "Unknown" },
    { "UAV-",          "Unknown" },
    { "QUADCOPTER-",   "Unknown" },

    /* ── Cheap Chinese / Temu drones (generic WiFi FPV) ─────────────────── */
    { "FPV_WIFI",      "Generic" },
    { "FPV-WIFI",      "Generic" },
    { "WIFI FPV",      "Generic" },
};

#define PATTERN_COUNT  (sizeof(PATTERNS) / sizeof(PATTERNS[0]))

/* ── Soft-match helpers ───────────────────────────────────────────────────── */

/**
 * Return true if every character in s[0..len-1] is alphanumeric.
 */
static bool all_alnum(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z'))) {
            return false;
        }
    }
    return true;
}

/**
 * Check whether the SSID starts with `prefix` (case-insensitive) and is
 * followed by 1-8 alphanumeric characters and nothing else.
 */
static bool soft_prefix_match(const char *ssid, const char *prefix)
{
    size_t plen = strlen(prefix);
    if (strncasecmp(ssid, prefix, plen) != 0) {
        return false;
    }
    size_t tail = strlen(ssid) - plen;
    return tail >= 1 && tail <= 8 && all_alnum(ssid + plen, tail);
}

bool wifi_ssid_match_soft(const char *ssid)
{
    if (!ssid || ssid[0] == '\0') {
        return false;
    }

    /* Reject long SSIDs — enterprise names like "WIFI_OFFICE_3RD_FLOOR" */
    if (strlen(ssid) > 16) {
        return false;
    }

    if (soft_prefix_match(ssid, "WIFI_"))   return true;
    if (soft_prefix_match(ssid, "FPV_"))    return true;
    if (soft_prefix_match(ssid, "CAMERA_")) return true;

    /* Exact-prefix patterns (case-insensitive) */
    if (strncasecmp(ssid, "4K_CAM", 6) == 0) return true;
    if (strncasecmp(ssid, "4KCAM",  5) == 0) return true;
    if (strncasecmp(ssid, "RCFPV",  5) == 0) return true;

    return false;
}

const drone_ssid_pattern_t *wifi_ssid_match(const char *ssid)
{
    if (!ssid || ssid[0] == '\0') {
        return NULL;
    }

    for (int i = 0; i < (int)PATTERN_COUNT; i++) {
        size_t prefix_len = strlen(PATTERNS[i].prefix);
        if (strncasecmp(ssid, PATTERNS[i].prefix, prefix_len) == 0) {
            return &PATTERNS[i];
        }
    }

    return NULL;
}

const drone_ssid_pattern_t *wifi_ssid_get_patterns(int *count)
{
    if (count) {
        *count = (int)PATTERN_COUNT;
    }
    return PATTERNS;
}
