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

bool fof_policy_probe_should_ignore_broadcast(const char *ssid)
{
    return !ssid || ssid[0] == '\0';
}

float fof_policy_probe_confidence(bool hard_match)
{
    return hard_match ? 0.50f : 0.05f;
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
    if (source == DETECTION_SRC_BLE_FINGERPRINT &&
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
