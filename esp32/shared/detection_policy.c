#include "detection_policy.h"

#include "detection_types.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static char ascii_tolower_char(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

static void format_uuid128_be(char out[37], const uint8_t uuid_le[16])
{
    snprintf(
        out,
        37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        uuid_le[15], uuid_le[14], uuid_le[13], uuid_le[12],
        uuid_le[11], uuid_le[10], uuid_le[9], uuid_le[8],
        uuid_le[7], uuid_le[6], uuid_le[5], uuid_le[4],
        uuid_le[3], uuid_le[2], uuid_le[1], uuid_le[0]
    );
}

static bool ascii_eq_nocase(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }
    while (*a && *b) {
        if (ascii_tolower_char(*a) != ascii_tolower_char(*b)) {
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
        while (*a && *b && ascii_tolower_char(*a) == ascii_tolower_char(*b)) {
            a++;
            b++;
        }
        if (*b == '\0') {
            return true;
        }
    }
    return false;
}

static bool ascii_is_alnum_char(char ch)
{
    return (ch >= '0' && ch <= '9') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z');
}

static bool ascii_contains_token_nocase(const char *haystack, const char *needle)
{
    if (!haystack || !needle || needle[0] == '\0') {
        return false;
    }

    size_t nlen = strlen(needle);
    for (const char *h = haystack; *h; h++) {
        size_t i = 0;
        while (i < nlen &&
               h[i] &&
               ascii_tolower_char(h[i]) == ascii_tolower_char(needle[i])) {
            i++;
        }
        if (i != nlen) {
            continue;
        }
        const bool before_ok = h == haystack || !ascii_is_alnum_char(h[-1]);
        const bool after_ok = h[nlen] == '\0' || !ascii_is_alnum_char(h[nlen]);
        if (before_ok && after_ok) {
            return true;
        }
    }
    return false;
}

static bool ssid_mentions_camera(const char *ssid)
{
    return ascii_contains_nocase(ssid, "camera") ||
           ascii_contains_nocase(ssid, "dashcam") ||
           ascii_contains_nocase(ssid, "bodycam") ||
           ascii_contains_nocase(ssid, "ipcam") ||
           ascii_contains_nocase(ssid, "wificam") ||
           ascii_contains_nocase(ssid, "hidvcam") ||
           ascii_contains_nocase(ssid, "hdwificam") ||
           ascii_contains_nocase(ssid, "tapo_cam") ||
           ascii_contains_token_nocase(ssid, "cam");
}

static bool ssid_mentions_flock(const char *ssid)
{
    bool legacy_numeric = false;
    if (ssid) {
        size_t len = strlen(ssid);
        legacy_numeric = len == 10;
        for (size_t i = 0; legacy_numeric && i < len; i++) {
            legacy_numeric = isdigit((unsigned char)ssid[i]) != 0;
        }
    }
    return ascii_contains_nocase(ssid, "flock") ||
           ascii_contains_nocase(ssid, "flockos") ||
           ascii_contains_nocase(ssid, "flk-") ||
           ascii_contains_nocase(ssid, "alpr") ||
           ascii_contains_nocase(ssid, "penguin-") ||
           legacy_numeric;
}

bool fof_policy_probe_should_ignore_broadcast(const char *ssid)
{
    return !ssid || ssid[0] == '\0';
}

float fof_policy_probe_confidence(bool hard_match)
{
    return hard_match ? 0.50f : 0.05f;
}

bool fof_policy_ssid_is_notable(const char *ssid)
{
    if (!ssid || ssid[0] == '\0') {
        return false;
    }

    const bool drone_like =
        ascii_contains_nocase(ssid, "drone") ||
        ascii_contains_nocase(ssid, "uav") ||
        ascii_contains_nocase(ssid, "fpv") ||
        ascii_contains_nocase(ssid, "remoteid") ||
        ascii_contains_nocase(ssid, "remote-id") ||
        ascii_contains_nocase(ssid, "rid-");
    const bool high_value =
        drone_like ||
        ascii_contains_nocase(ssid, "camera") ||
        ssid_mentions_camera(ssid) ||
        ssid_mentions_flock(ssid) ||
        ascii_contains_nocase(ssid, "skimmer");

    /* Badge instrument mode is not an AP inventory.  These are our own/demo
     * ambient broadcasts and they crowd out higher-value SA like drones,
     * Flock, glasses, and skimmer/tooling evidence. */
    if (ascii_contains_nocase(ssid, "teamcharitycase")) {
        return false;
    }
    if (!high_value &&
        (ascii_contains_nocase(ssid, "friendorfoe") ||
         ascii_contains_nocase(ssid, "fof-") ||
         ascii_contains_nocase(ssid, "fof_") ||
         ascii_contains_nocase(ssid, "fof "))) {
        return false;
    }
    if (drone_like) {
        return true;
    }
    if (ssid_mentions_flock(ssid)) {
        return true;
    }
    if (ssid_mentions_camera(ssid)) {
        return true;
    }
    if (ascii_contains_nocase(ssid, "skimmer")) {
        return true;
    }

    static const char *tokens[] = {
        "defcon", "dc33", "drone", "uav", "fpv", "remoteid",
        "remote-id", "camera", "flock", "flockos", "flk-",
        "alpr", "penguin-", "skimmer",
        "pwnagotchi", "marauder", "pineapple", "deauther", "pwned",
        "evil", "twin",
    };
    for (size_t i = 0; i < sizeof(tokens) / sizeof(tokens[0]); i++) {
        if (ascii_contains_nocase(ssid, tokens[i])) {
            return true;
        }
    }
    return false;
}

const char *fof_policy_notable_ssid_label(const char *ssid)
{
    if (ascii_contains_nocase(ssid, "drone") ||
        ascii_contains_nocase(ssid, "uav") ||
        ascii_contains_nocase(ssid, "fpv") ||
        ascii_contains_nocase(ssid, "remoteid") ||
        ascii_contains_nocase(ssid, "remote-id") ||
        ascii_contains_nocase(ssid, "rid-")) return "Drone SSID";
    if (ssid_mentions_flock(ssid)) return "Flock SSID";
    if (ssid_mentions_camera(ssid)) return "Camera SSID";
    if (ascii_contains_nocase(ssid, "skimmer")) return "Skimmer SSID";
    if (ascii_contains_nocase(ssid, "pwnagotchi")) return "Pwnagotchi";
    if (ascii_contains_nocase(ssid, "marauder")) return "Marauder";
    if (ascii_contains_nocase(ssid, "pineapple")) return "Pineapple";
    if (ascii_contains_nocase(ssid, "deauther") ||
        ascii_contains_nocase(ssid, "pwned")) return "Deauther";
    if (ascii_contains_nocase(ssid, "evil") ||
        ascii_contains_nocase(ssid, "twin")) return "EvilTwin SSID";
    return "Notable SSID";
}

bool fof_policy_is_priority_ble_fingerprint(const char *manufacturer)
{
    const char *mfr = manufacturer ? manufacturer : "";
    return strcmp(mfr, "AirTag") == 0 ||
           strcmp(mfr, "FindMy Accessory") == 0 ||
           strcmp(mfr, "Tile Tracker") == 0 ||
           strcmp(mfr, "SmartTag") == 0 ||
           strcmp(mfr, "Google Tracker") == 0 ||
           strcmp(mfr, "Tracker (Generic)") == 0 ||
           strcmp(mfr, "Pebblebee") == 0 ||
           strcmp(mfr, "Chipolo") == 0 ||
           strcmp(mfr, "Drone Controller") == 0 ||
           strcmp(mfr, "Drone") == 0 ||
           strcmp(mfr, "Meta Glasses") == 0 ||
           strcmp(mfr, "Meta Device") == 0 ||
           strcmp(mfr, "Flipper Zero") == 0 ||
           strcmp(mfr, "Card Skimmer (suspect)") == 0 ||
           strcmp(mfr, "Venue Beacon") == 0 ||
           strcmp(mfr, "Event Badge") == 0 ||
           strcmp(mfr, "Mobile Key Lock") == 0 ||
           strcmp(mfr, "BLE HID") == 0 ||
           strcmp(mfr, "Auracast") == 0 ||
           strcmp(mfr, "Camera") == 0 ||
           strcmp(mfr, "Hidden Camera (suspect)") == 0 ||
           strcmp(mfr, "Flock Surveillance") == 0;
}

bool fof_policy_ble_uuid128_is_calibration_le(const uint8_t uuid_le[16])
{
    if (!uuid_le) {
        return false;
    }

    return uuid_le[15] == 0xCA &&
           uuid_le[14] == 0xFE &&
           uuid_le[11] == 0x00 &&
           uuid_le[10] == 0x00 &&
           uuid_le[9] == 0x10 &&
           uuid_le[8] == 0x00 &&
           uuid_le[7] == 0x80 &&
           uuid_le[6] == 0x00;
}

bool fof_policy_ble_has_calibration_uuid_le(const uint8_t uuids[][16],
                                            uint8_t count)
{
    if (!uuids || count == 0) {
        return false;
    }

    for (uint8_t i = 0; i < count; i++) {
        if (fof_policy_ble_uuid128_is_calibration_le(uuids[i])) {
            return true;
        }
    }
    return false;
}

bool fof_policy_ble_uuid128_matches_token_le(const uint8_t uuid_le[16],
                                             const char *uuid_token)
{
    if (!uuid_le || !uuid_token || uuid_token[0] == '\0') {
        return false;
    }
    char formatted[37];
    format_uuid128_be(formatted, uuid_le);
    return ascii_eq_nocase(formatted, uuid_token);
}

bool fof_policy_ble_has_exact_uuid128_le(const uint8_t uuids[][16],
                                         uint8_t count,
                                         const char *uuid_token)
{
    if (!uuids || count == 0 || !uuid_token || uuid_token[0] == '\0') {
        return false;
    }
    for (uint8_t i = 0; i < count; i++) {
        if (fof_policy_ble_uuid128_matches_token_le(uuids[i], uuid_token)) {
            return true;
        }
    }
    return false;
}

bool fof_policy_ble_svc_raw_contains_uuid(const char *svc_raw,
                                          const char *uuid_token)
{
    if (!svc_raw || svc_raw[0] == '\0' || !uuid_token || uuid_token[0] == '\0') {
        return false;
    }

    const char *cursor = svc_raw;
    while (*cursor) {
        char token[48];
        size_t token_len = 0;
        while (*cursor && *cursor != ',' && token_len < sizeof(token) - 1) {
            token[token_len++] = *cursor++;
        }
        token[token_len] = '\0';
        if (ascii_eq_nocase(token, uuid_token)) {
            return true;
        }
        while (*cursor == ',') {
            cursor++;
        }
    }
    return false;
}

bool fof_policy_should_drop_low_value(uint8_t source,
                                      float confidence,
                                      const char *manufacturer,
                                      const uint8_t ble_svc_uuids_128[][16],
                                      uint8_t ble_svc_uuid_128_count)
{
    if (source == DETECTION_SRC_BLE_FINGERPRINT &&
        fof_policy_ble_has_calibration_uuid_le(
            ble_svc_uuids_128,
            ble_svc_uuid_128_count
        )) {
        return false;
    }

    return source == DETECTION_SRC_BLE_FINGERPRINT &&
           confidence < 0.10f &&
           !fof_policy_is_priority_ble_fingerprint(manufacturer);
}

uint32_t fof_policy_ble_fingerprint_reemit_ms(const char *manufacturer)
{
    return fof_policy_is_priority_ble_fingerprint(manufacturer)
        ? 5000U
        : 60000U;
}

bool fof_policy_ble_meta_should_reacquire(bool ble_scanning,
                                          bool host_synced,
                                          int64_t meta_age_s,
                                          uint32_t adv_seen_delta,
                                          bool calibration_active,
                                          bool ota_active)
{
    if (!ble_scanning || !host_synced || calibration_active || ota_active) {
        return false;
    }
    if (meta_age_s < 30 || adv_seen_delta == 0) {
        return false;
    }
    return true;
}

bool fof_policy_is_controller_class_ble(uint8_t source,
                                        const char *manufacturer)
{
    const char *mfr = manufacturer ? manufacturer : "";
    return source == DETECTION_SRC_BLE_FINGERPRINT &&
           (strcmp(mfr, "Drone Controller") == 0 ||
            strcmp(mfr, "Drone") == 0);
}

bool fof_policy_should_shed_low_priority(uint8_t source,
                                         const char *manufacturer,
                                         const uint8_t ble_svc_uuids_128[][16],
                                         uint8_t ble_svc_uuid_128_count,
                                         uint32_t queue_depth,
                                         uint32_t queue_capacity)
{
    if (queue_capacity == 0) {
        return false;
    }

    if (source == DETECTION_SRC_BLE_FINGERPRINT &&
        fof_policy_ble_has_calibration_uuid_le(
            ble_svc_uuids_128,
            ble_svc_uuid_128_count
        )) {
        return false;
    }

    if (source == DETECTION_SRC_WIFI_PROBE_REQUEST &&
        queue_depth >= (queue_capacity * 6U / 10U)) {
        return true;
    }
    if (source == DETECTION_SRC_WIFI_AP_INVENTORY &&
        queue_depth >= (queue_capacity * 4U / 10U)) {
        return true;
    }
    if (source == DETECTION_SRC_BLE_FINGERPRINT &&
        !fof_policy_is_priority_ble_fingerprint(manufacturer) &&
        !fof_policy_is_controller_class_ble(source, manufacturer) &&
        queue_depth >= (queue_capacity * 7U / 10U)) {
        return true;
    }
    if (source == DETECTION_SRC_WIFI_ASSOC &&
        queue_depth >= (queue_capacity * 8U / 10U)) {
        return true;
    }
    return false;
}

uint32_t fof_policy_queue_pressure_pct(uint32_t queue_depth,
                                       uint32_t queue_capacity)
{
    if (queue_capacity == 0) {
        return 0;
    }
    return (uint32_t)(((queue_depth * 100U) + (queue_capacity / 2U)) /
                      queue_capacity);
}

void fof_policy_probe_rate_aux(uint32_t ie_hash,
                               const char *probed_ssids,
                               char *out,
                               size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }

    const char *ssid_list = probed_ssids ? probed_ssids : "";
    if (ie_hash == 0 && ssid_list[0] == '\0') {
        out[0] = '\0';
        return;
    }

    uint32_t hash = ie_hash ? ie_hash : 0x811c9dc5U;
    for (const unsigned char *p = (const unsigned char *)ssid_list; *p; ++p) {
        hash ^= (uint32_t)(*p);
        hash *= 0x01000193U;
    }

    snprintf(out, out_len, "%08lx", (unsigned long)hash);
}

bool fof_policy_detection_identity_key(const drone_detection_t *det,
                                       char *out,
                                       size_t out_len)
{
    if (!det || !out || out_len == 0) {
        return false;
    }

    if (det->source == DETECTION_SRC_WIFI_PROBE_REQUEST) {
        if (det->probe_ie_hash != 0) {
            snprintf(out, out_len, "PROBE:%08lx",
                     (unsigned long)det->probe_ie_hash);
            return true;
        }
        if (det->bssid[0] != '\0') {
            snprintf(out, out_len, "PROBE:%s", det->bssid);
            return true;
        }
    }

    if (det->source == DETECTION_SRC_BLE_FINGERPRINT) {
        if (det->ble_svc_uuids_raw[0] != '\0' &&
            ascii_contains_nocase(det->ble_svc_uuids_raw, "cafe")) {
            snprintf(out, out_len, "CAL:%s", det->ble_svc_uuids_raw);
            return true;
        }
        if (det->bssid[0] != '\0') {
            if (strncmp(det->model, "FP:", 3) == 0) {
                snprintf(out, out_len, "BLEMAC:%s|%s", det->bssid, det->model);
                return true;
            }
            if (det->ble_ja3_hash != 0) {
                snprintf(out, out_len, "BLEMAC:%s|JA3:%08lx",
                         det->bssid, (unsigned long)det->ble_ja3_hash);
                return true;
            }
            snprintf(out, out_len, "BLEMAC:%s", det->bssid);
            return true;
        }
        if (strncmp(det->model, "FP:", 3) == 0) {
            snprintf(out, out_len, "BLE:%s", det->model);
            return true;
        }
        if (det->ble_ja3_hash != 0) {
            snprintf(out, out_len, "BLEJA3:%08lx",
                     (unsigned long)det->ble_ja3_hash);
            return true;
        }
    }

    if (det->source == DETECTION_SRC_WIFI_ASSOC ||
        det->source == DETECTION_SRC_WIFI_SSID ||
        det->source == DETECTION_SRC_WIFI_OUI ||
        det->source == DETECTION_SRC_WIFI_AP_INVENTORY ||
        det->source == DETECTION_SRC_WIFI_DJI_IE ||
        det->source == DETECTION_SRC_WIFI_BEACON) {
        if (det->bssid[0] != '\0') {
            snprintf(out, out_len, "WIFI:%s", det->bssid);
            return true;
        }
    }

    if (det->drone_id[0] != '\0') {
        snprintf(out, out_len, "ID:%s", det->drone_id);
        return true;
    }

    out[0] = '\0';
    return false;
}

bool fof_policy_detection_dedupe_key(const drone_detection_t *det,
                                     int64_t timestamp_ms,
                                     uint32_t bucket_ms,
                                     char *out,
                                     size_t out_len)
{
    if (!out || out_len == 0 || !det || bucket_ms == 0) {
        return false;
    }

    char identity[192];
    if (!fof_policy_detection_identity_key(det, identity, sizeof(identity))) {
        return false;
    }

    int64_t bucket = timestamp_ms > 0
        ? (timestamp_ms / (int64_t)bucket_ms)
        : 0;
    snprintf(out, out_len, "%u:%s:%lld",
             (unsigned)det->source,
             identity,
             (long long)bucket);
    return true;
}

const char *fof_policy_scan_profile_for_slot(uint8_t scanner_id,
                                             bool calibration_active)
{
    if (calibration_active) {
        return "calibration";
    }
    return scanner_id == 0 ? "ble_primary" : "wifi_primary";
}

const char *fof_policy_slot_role_for_slot(uint8_t scanner_id)
{
    return scanner_id == 0 ? "ble_primary" : "wifi_primary";
}

static bool source_is_ble(uint8_t source)
{
    return source == DETECTION_SRC_BLE_RID ||
           source == DETECTION_SRC_BLE_FINGERPRINT;
}

static bool source_is_wifi(uint8_t source)
{
    return source == DETECTION_SRC_WIFI_SSID ||
           source == DETECTION_SRC_WIFI_DJI_IE ||
           source == DETECTION_SRC_WIFI_BEACON ||
           source == DETECTION_SRC_WIFI_OUI ||
           source == DETECTION_SRC_WIFI_ASSOC ||
           source == DETECTION_SRC_WIFI_PROBE_REQUEST ||
           source == DETECTION_SRC_WIFI_AP_INVENTORY;
}

bool fof_policy_scan_profile_allows_source(const char *scan_profile,
                                           uint8_t source)
{
    if (!scan_profile || scan_profile[0] == '\0' ||
        ascii_eq_nocase(scan_profile, "normal") ||
        ascii_eq_nocase(scan_profile, "hybrid_failover")) {
        return true;
    }
    if (ascii_eq_nocase(scan_profile, "ble_primary")) {
        return !source_is_wifi(source);
    }
    if (ascii_eq_nocase(scan_profile, "wifi_primary")) {
        return source_is_wifi(source);
    }
    if (ascii_eq_nocase(scan_profile, "calibration")) {
        return source == DETECTION_SRC_BLE_FINGERPRINT;
    }
    return true;
}

void fof_policy_evil_twin_state_init(fof_policy_evil_twin_state_t *state)
{
    if (state) {
        memset(state, 0, sizeof(*state));
    }
}

const char *fof_policy_wifi_auth_label(uint8_t auth_mode)
{
    switch (auth_mode) {
        case 0: return "open";
        case 1: return "WEP";
        case 2: return "WPA";
        case 3: return "WPA2";
        case 4: return "WPA/WPA2";
        case 5: return "WPA2-ENT";
        case 6: return "WPA3";
        case 7: return "WPA2/3";
        case 8: return "WAPI";
        case 9: return "OWE";
        case 10: return "WPA3-ENT";
        default: return "auth?";
    }
}

uint8_t fof_policy_wifi_beacon_auth_mode(const uint8_t *frame, size_t frame_len)
{
    enum {
        beacon_header_len = 24,
        beacon_fixed_len = 12,
        beacon_capability_offset = beacon_header_len + 10,
        beacon_tagged_offset = beacon_header_len + beacon_fixed_len,
        ie_tag_rsn = 48,
        ie_tag_vendor_specific = 221,
    };

    if (!frame || frame_len < beacon_tagged_offset) {
        return 0xFF;
    }

    uint16_t capabilities =
        (uint16_t)frame[beacon_capability_offset] |
        ((uint16_t)frame[beacon_capability_offset + 1] << 8);
    bool privacy = (capabilities & 0x0010u) != 0;
    bool has_wpa = false;

    size_t offset = beacon_tagged_offset;
    while (offset + 2u <= frame_len) {
        uint8_t tag_id = frame[offset];
        uint8_t tag_len = frame[offset + 1u];
        size_t data_offset = offset + 2u;
        if (data_offset + tag_len > frame_len) {
            break;
        }

        if (tag_id == ie_tag_rsn) {
            return 3; /* WPA2/WPA3 RSN; protected is enough for Evil Twin. */
        }
        if (tag_id == ie_tag_vendor_specific && tag_len >= 4u) {
            const uint8_t *data = &frame[data_offset];
            if (data[0] == 0x00 && data[1] == 0x50 &&
                data[2] == 0xF2 && data[3] == 0x01) {
                has_wpa = true;
            }
        }

        offset = data_offset + tag_len;
    }

    if (has_wpa) {
        return 2;
    }
    return privacy ? 1 : 0;
}

static bool evil_twin_auth_is_open(uint8_t auth_mode)
{
    return auth_mode == 0;
}

static bool evil_twin_auth_is_known(uint8_t auth_mode)
{
    return auth_mode <= 10;
}

static bool evil_twin_auth_is_protected(uint8_t auth_mode)
{
    return evil_twin_auth_is_known(auth_mode) && auth_mode != 0;
}

static bool evil_twin_bssid_same(const uint8_t a[6], const uint8_t b[6])
{
    return a && b && memcmp(a, b, 6) == 0;
}

static bool evil_twin_oui_differs(const uint8_t a[6], const uint8_t b[6])
{
    return a && b && memcmp(a, b, 3) != 0;
}

static int evil_twin_abs_db(int a, int b)
{
    int delta = a - b;
    return delta < 0 ? -delta : delta;
}

static bool evil_twin_normalize_ssid(const char *ssid,
                                     char out[FOF_POLICY_EVIL_TWIN_MAX_SSID_LEN])
{
    if (!out) {
        return false;
    }
    out[0] = '\0';
    if (!ssid) {
        return false;
    }

    bool non_space = false;
    size_t len = 0;
    for (; len < 32; len++) {
        unsigned char ch = (unsigned char)ssid[len];
        if (ch == '\0') {
            break;
        }
        if (ch < 0x20 || ch == 0x7f) {
            out[0] = '\0';
            return false;
        }
        out[len] = (char)ch;
        if (ch != ' ') {
            non_space = true;
        }
    }
    out[len] = '\0';
    return non_space;
}

static fof_policy_evil_twin_ssid_t *evil_twin_find_or_claim_ssid(
    fof_policy_evil_twin_state_t *state,
    const char *ssid,
    int64_t now_ms)
{
    fof_policy_evil_twin_ssid_t *oldest = &state->ssids[0];
    for (int i = 0; i < FOF_POLICY_EVIL_TWIN_SSID_SLOTS; i++) {
        fof_policy_evil_twin_ssid_t *slot = &state->ssids[i];
        if (slot->in_use && strcmp(slot->ssid, ssid) == 0) {
            slot->last_used_ms = now_ms;
            return slot;
        }
        if (!slot->in_use) {
            memset(slot, 0, sizeof(*slot));
            slot->in_use = true;
            strncpy(slot->ssid, ssid, sizeof(slot->ssid) - 1);
            slot->last_used_ms = now_ms;
            return slot;
        }
        if (slot->last_used_ms < oldest->last_used_ms) {
            oldest = slot;
        }
    }

    memset(oldest, 0, sizeof(*oldest));
    oldest->in_use = true;
    strncpy(oldest->ssid, ssid, sizeof(oldest->ssid) - 1);
    oldest->last_used_ms = now_ms;
    return oldest;
}

static fof_policy_evil_twin_ap_t *evil_twin_find_or_claim_ap(
    fof_policy_evil_twin_ssid_t *ssid_slot,
    const uint8_t bssid[6],
    int64_t now_ms,
    bool *is_new)
{
    fof_policy_evil_twin_ap_t *oldest = &ssid_slot->aps[0];
    if (is_new) {
        *is_new = false;
    }
    for (int i = 0; i < FOF_POLICY_EVIL_TWIN_AP_SLOTS; i++) {
        fof_policy_evil_twin_ap_t *slot = &ssid_slot->aps[i];
        if (slot->in_use && evil_twin_bssid_same(slot->bssid, bssid)) {
            return slot;
        }
        if (!slot->in_use) {
            memset(slot, 0, sizeof(*slot));
            slot->in_use = true;
            memcpy(slot->bssid, bssid, 6);
            if (is_new) {
                *is_new = true;
            }
            return slot;
        }
        if (slot->last_seen_ms < oldest->last_seen_ms) {
            oldest = slot;
        }
    }

    memset(oldest, 0, sizeof(*oldest));
    oldest->in_use = true;
    memcpy(oldest->bssid, bssid, 6);
    if (is_new) {
        *is_new = true;
    }
    (void)now_ms;
    return oldest;
}

static void evil_twin_format_alert(fof_policy_evil_twin_alert_t *out,
                                   const char *ssid,
                                   const fof_policy_evil_twin_ap_t *suspect,
                                   const fof_policy_evil_twin_ap_t *reference,
                                   bool mixed_open,
                                   bool strong_clone)
{
    memset(out, 0, sizeof(*out));
    strncpy(out->ssid, ssid, sizeof(out->ssid) - 1);
    memcpy(out->suspect_bssid, suspect->bssid, 6);
    memcpy(out->reference_bssid, reference->bssid, 6);
    out->suspect_rssi = suspect->rssi;
    out->suspect_channel = suspect->channel;
    out->suspect_auth_mode = suspect->auth_mode;
    out->reference_auth_mode = reference->auth_mode;
    out->mixed_open = mixed_open;
    out->strong_clone = strong_clone;
    snprintf(out->detail, sizeof(out->detail), "%s clone vs %s",
             fof_policy_wifi_auth_label(suspect->auth_mode),
             fof_policy_wifi_auth_label(reference->auth_mode));
}

bool fof_policy_evil_twin_observe(fof_policy_evil_twin_state_t *state,
                                  const char *ssid,
                                  const uint8_t bssid[6],
                                  int8_t rssi,
                                  uint8_t channel,
                                  uint8_t auth_mode,
                                  int64_t now_ms,
                                  fof_policy_evil_twin_alert_t *out)
{
    char safe_ssid[FOF_POLICY_EVIL_TWIN_MAX_SSID_LEN];
    if (!state || !evil_twin_normalize_ssid(ssid, safe_ssid) || !bssid ||
        !evil_twin_auth_is_known(auth_mode)) {
        return false;
    }

    fof_policy_evil_twin_ssid_t *ssid_slot =
        evil_twin_find_or_claim_ssid(state, safe_ssid, now_ms);
    if (!ssid_slot) {
        return false;
    }

    bool is_new = false;
    fof_policy_evil_twin_ap_t *current =
        evil_twin_find_or_claim_ap(ssid_slot, bssid, now_ms, &is_new);
    if (!current) {
        return false;
    }

    current->rssi = rssi;
    current->channel = channel;
    current->auth_mode = auth_mode;
    current->last_seen_ms = now_ms;

    const int64_t fresh_ms = 60000;
    const int64_t alert_ms = 30000;
    fof_policy_evil_twin_ap_t *best_suspect = NULL;
    fof_policy_evil_twin_ap_t *best_reference = NULL;
    bool best_mixed_open = false;
    bool best_strong_clone = false;

    for (int i = 0; i < FOF_POLICY_EVIL_TWIN_AP_SLOTS; i++) {
        fof_policy_evil_twin_ap_t *other = &ssid_slot->aps[i];
        if (!other->in_use || other == current ||
            evil_twin_bssid_same(other->bssid, current->bssid)) {
            continue;
        }
        int64_t age = now_ms - other->last_seen_ms;
        if (age < 0 || age > fresh_ms ||
            !evil_twin_auth_is_known(other->auth_mode)) {
            continue;
        }

        bool current_open = evil_twin_auth_is_open(current->auth_mode);
        bool other_open = evil_twin_auth_is_open(other->auth_mode);
        bool mixed_open = current_open != other_open &&
                          ((current_open && evil_twin_auth_is_protected(other->auth_mode)) ||
                           (other_open && evil_twin_auth_is_protected(current->auth_mode)));
        bool oui_diff = evil_twin_oui_differs(current->bssid, other->bssid);
        bool auth_diff = current->auth_mode != other->auth_mode;
        bool close_signal = evil_twin_abs_db(current->rssi, other->rssi) <= 18;
        bool strong_now = current->rssi >= -65 || other->rssi >= -65;
        bool strong_clone = is_new && !mixed_open && oui_diff && auth_diff &&
                            close_signal && strong_now;

        if (!mixed_open && !strong_clone) {
            continue;
        }

        fof_policy_evil_twin_ap_t *suspect = current;
        fof_policy_evil_twin_ap_t *reference = other;
        if (mixed_open && other_open && !current_open) {
            suspect = other;
            reference = current;
        } else if (strong_clone && other->rssi > current->rssi) {
            suspect = other;
            reference = current;
        }

        if (suspect->last_alert_ms > 0 &&
            (now_ms - suspect->last_alert_ms) < alert_ms) {
            continue;
        }
        if (!best_suspect || suspect->rssi > best_suspect->rssi) {
            best_suspect = suspect;
            best_reference = reference;
            best_mixed_open = mixed_open;
            best_strong_clone = strong_clone;
        }
    }

    if (!best_suspect || !best_reference) {
        return false;
    }
    best_suspect->last_alert_ms = now_ms;
    if (out) {
        evil_twin_format_alert(out, safe_ssid, best_suspect, best_reference,
                               best_mixed_open, best_strong_clone);
    }
    return true;
}
