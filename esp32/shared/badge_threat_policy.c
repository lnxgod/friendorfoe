#include "badge_threat_policy.h"

#include "detection_policy.h"

#include <stdio.h>
#include <string.h>

#define BADGE_META_LIVE_WINDOW_MS 90000
#define BADGE_DRONE_SSID_LIVE_WINDOW_MS 15000
#define BADGE_META_WEAK_KEY "PRIV:META:WEAK"

static char ascii_lower(char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
}

static bool contains_nocase(const char *haystack, const char *needle)
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

static uint32_t parse_count_token(const char *text)
{
    if (!text) {
        return 0;
    }
    const char *p = strstr(text, "count:");
    if (!p) {
        return 0;
    }
    p += 6;
    uint32_t value = 0;
    bool saw_digit = false;
    while (*p >= '0' && *p <= '9') {
        saw_digit = true;
        value = (value * 10U) + (uint32_t)(*p - '0');
        if (value > 999U) {
            return 999U;
        }
        p++;
    }
    return saw_digit ? value : 0;
}

static bool source_is_confirmed_drone(uint8_t source)
{
    return source == DETECTION_SRC_BLE_RID ||
           source == DETECTION_SRC_WIFI_DJI_IE ||
           source == DETECTION_SRC_WIFI_BEACON;
}

static bool source_is_drone_candidate(uint8_t source)
{
    return source_is_confirmed_drone(source) ||
           source == DETECTION_SRC_WIFI_SSID ||
           source == DETECTION_SRC_WIFI_OUI;
}

static bool text_mentions_drone(const char *text)
{
    return contains_nocase(text, "drone") ||
           contains_nocase(text, "dji") ||
           contains_nocase(text, "remote id") ||
           contains_nocase(text, "remoteid");
}

static bool text_mentions_meta_glasses(const char *text)
{
    return contains_nocase(text, "meta glasses") ||
           contains_nocase(text, "ray-ban") ||
           contains_nocase(text, "rayban") ||
           contains_nocase(text, "rb meta") ||
           contains_nocase(text, "wayfarer") ||
           contains_nocase(text, "oakley") ||
           contains_nocase(text, "luxottica") ||
           contains_nocase(text, "name:meta_glasses") ||
           contains_nocase(text, "0x0D53") ||
           contains_nocase(text, "0xFD5F");
}

static bool text_mentions_tracker(const char *text)
{
    return contains_nocase(text, "airtag") ||
           contains_nocase(text, "findmy") ||
           contains_nocase(text, "find my") ||
           contains_nocase(text, "tile") ||
           contains_nocase(text, "tracker") ||
           contains_nocase(text, "smarttag") ||
           contains_nocase(text, "google tag") ||
           contains_nocase(text, "google tracker") ||
           contains_nocase(text, "chipolo") ||
           contains_nocase(text, "pebblebee");
}

static bool text_mentions_security_device(const char *text)
{
    return contains_nocase(text, "flipper") ||
           contains_nocase(text, "pwnagotchi") ||
           contains_nocase(text, "camera") ||
           contains_nocase(text, "skimmer") ||
           contains_nocase(text, "flock") ||
           contains_nocase(text, "surveillance");
}

static bool text_mentions_flock(const char *text)
{
    return contains_nocase(text, "flock");
}

static bool text_mentions_skimmer(const char *text)
{
    return contains_nocase(text, "skimmer") ||
           contains_nocase(text, "hc-05") ||
           contains_nocase(text, "hc-06") ||
           contains_nocase(text, "bt05") ||
           contains_nocase(text, "hm-10") ||
           contains_nocase(text, "jdy-");
}

static bool text_mentions_hidden_camera(const char *text)
{
    return contains_nocase(text, "hidden camera") ||
           contains_nocase(text, "spy cam") ||
           contains_nocase(text, "spycam") ||
           contains_nocase(text, "hidvcam") ||
           contains_nocase(text, "hdwificam") ||
           contains_nocase(text, "lookcam");
}

static bool text_mentions_camera_privacy(const char *text)
{
    return text_mentions_hidden_camera(text) ||
           contains_nocase(text, "camera") ||
           contains_nocase(text, "body cam") ||
           contains_nocase(text, "bodycam") ||
           contains_nocase(text, "dash cam") ||
           contains_nocase(text, "dashcam") ||
           contains_nocase(text, "fleet cam") ||
           contains_nocase(text, "conference cam") ||
           contains_nocase(text, "security cam") ||
           contains_nocase(text, "action cam") ||
           contains_nocase(text, "gopro") ||
           contains_nocase(text, "axon") ||
           contains_nocase(text, "samsara") ||
           contains_nocase(text, "verkada") ||
           contains_nocase(text, "hikvision") ||
           contains_nocase(text, "dahua");
}

static bool text_mentions_venue_beacon(const char *text)
{
    return contains_nocase(text, "venue beacon") ||
           contains_nocase(text, "ibeacon") ||
           contains_nocase(text, "eddystone") ||
           contains_nocase(text, "estimote") ||
           contains_nocase(text, "kontakt") ||
           contains_nocase(text, "gimbal") ||
           contains_nocase(text, "retailnext") ||
           contains_nocase(text, "vergesense") ||
           contains_nocase(text, "beaconstac") ||
           contains_nocase(text, "radiusnetworks") ||
           contains_nocase(text, "venue") ||
           strcmp(text ? text : "", "Beacon") == 0;
}

static bool text_mentions_event_badge(const char *text)
{
    return contains_nocase(text, "event badge") ||
           contains_nocase(text, "smart badge") ||
           contains_nocase(text, "attendee badge") ||
           contains_nocase(text, "conference badge") ||
           contains_nocase(text, "expo badge") ||
           contains_nocase(text, "event tag") ||
           contains_nocase(text, "wristband") ||
           contains_nocase(text, "bizzabo") ||
           contains_nocase(text, "cvent") ||
           contains_nocase(text, "klik");
}

static bool text_mentions_mobile_key_lock(const char *text)
{
    return contains_nocase(text, "mobile key") ||
           contains_nocase(text, "mobile access") ||
           contains_nocase(text, "mobile key lock") ||
           contains_nocase(text, "smart lock") ||
           contains_nocase(text, "dormakaba") ||
           contains_nocase(text, "saflok") ||
           contains_nocase(text, "vingcard") ||
           contains_nocase(text, "assa") ||
           contains_nocase(text, "abloy") ||
           contains_nocase(text, "salto") ||
           contains_nocase(text, "onity") ||
           contains_nocase(text, "kaba") ||
           contains_nocase(text, "august") ||
           contains_nocase(text, "schlage") ||
           contains_nocase(text, "yale") ||
           contains_nocase(text, "level lock");
}

static bool text_mentions_ble_hid(const char *text)
{
    return contains_nocase(text, "ble hid") ||
           contains_nocase(text, "keyboard") ||
           contains_nocase(text, "mouse") ||
           contains_nocase(text, "input device") ||
           contains_nocase(text, "presenter") ||
           strcmp(text ? text : "", "BLE HID") == 0;
}

static bool text_mentions_auracast(const char *text)
{
    return contains_nocase(text, "auracast") ||
           contains_nocase(text, "le audio") ||
           contains_nocase(text, "broadcast audio");
}

static bool text_mentions_ambient_demo_ssid(const char *text)
{
    return contains_nocase(text, "teamcharitycase") ||
           contains_nocase(text, "friendorfoe") ||
           contains_nocase(text, "fof-") ||
           contains_nocase(text, "fof_");
}

static bool detection_is_ambient_demo_ssid(const drone_detection_t *det)
{
    if (!det || (det->source != DETECTION_SRC_WIFI_PROBE_REQUEST &&
                 det->source != DETECTION_SRC_WIFI_ASSOC &&
                 det->source != DETECTION_SRC_WIFI_SSID)) {
        return false;
    }
    if (!text_mentions_ambient_demo_ssid(det->ssid) &&
        !text_mentions_ambient_demo_ssid(det->probed_ssids) &&
        !text_mentions_ambient_demo_ssid(det->drone_id)) {
        return false;
    }

    return !text_mentions_drone(det->ssid) &&
           !text_mentions_drone(det->probed_ssids) &&
           !text_mentions_drone(det->drone_id) &&
           !text_mentions_drone(det->manufacturer) &&
           !text_mentions_drone(det->class_reason);
}

static bool text_mentions_glasses(const char *text)
{
    return text_mentions_meta_glasses(text) ||
           contains_nocase(text, "glasses") ||
           contains_nocase(text, "eyewear") ||
           contains_nocase(text, "spectacles");
}

static bool detection_mentions_any(const drone_detection_t *det,
                                   bool (*predicate)(const char *))
{
    if (!det || !predicate) {
        return false;
    }
    return predicate(det->manufacturer) ||
           predicate(det->model) ||
           predicate(det->ble_name) ||
           predicate(det->class_reason) ||
           predicate(det->ssid);
}

static bool detection_is_close_tracker(const drone_detection_t *det)
{
    if (!det || det->source != DETECTION_SRC_BLE_FINGERPRINT || det->rssi < -55) {
        return false;
    }
    return text_mentions_tracker(det->manufacturer) ||
           text_mentions_tracker(det->model) ||
           text_mentions_tracker(det->ble_name) ||
           text_mentions_tracker(det->drone_id) ||
           text_mentions_tracker(det->class_reason);
}

static bool detection_has_notable_ssid(const drone_detection_t *det)
{
    if (!det) {
        return false;
    }
    return fof_policy_ssid_is_notable(det->ssid) ||
           fof_policy_ssid_is_notable(det->probed_ssids) ||
           (det->source == DETECTION_SRC_WIFI_SSID && det->ssid[0] != '\0');
}

static bool drone_detection_has_lift(const drone_detection_t *det)
{
    if (!det) {
        return false;
    }
    if (source_is_confirmed_drone(det->source)) {
        return true;
    }
    if (det->source == DETECTION_SRC_WIFI_SSID && det->ssid[0] != '\0') {
        return true;
    }
    if (det->source == DETECTION_SRC_WIFI_OUI && det->manufacturer[0] != '\0') {
        return true;
    }
    if (det->model[0] != '\0' && !contains_nocase(det->model, "fp:")) {
        return true;
    }
    if (det->self_id_text[0] != '\0' || det->operator_id[0] != '\0') {
        return true;
    }
    if (det->ssid[0] != '\0') {
        return true;
    }
    if (contains_nocase(det->manufacturer, "dji") ||
        contains_nocase(det->manufacturer, "autel") ||
        contains_nocase(det->manufacturer, "parrot") ||
        contains_nocase(det->manufacturer, "skydio")) {
        return true;
    }
    return false;
}

static int category_priority(badge_threat_category_t category)
{
    switch (category) {
        case BADGE_THREAT_CATEGORY_DRONE:     return 80;
        case BADGE_THREAT_CATEGORY_SSID:      return 70;
        case BADGE_THREAT_CATEGORY_FLOCK:     return 60;
        case BADGE_THREAT_CATEGORY_GLASS:     return 50;
        case BADGE_THREAT_CATEGORY_SKIM:      return 40;
        case BADGE_THREAT_CATEGORY_CAMERA:    return 38;
        case BADGE_THREAT_CATEGORY_LOCK:      return 36;
        case BADGE_THREAT_CATEGORY_HID:       return 32;
        case BADGE_THREAT_CATEGORY_WIFI:      return 30;
        case BADGE_THREAT_CATEGORY_EVENT_BADGE: return 24;
        case BADGE_THREAT_CATEGORY_TAG_CLOSE: return 20;
        case BADGE_THREAT_CATEGORY_BEACON:    return 16;
        case BADGE_THREAT_CATEGORY_AUDIO:     return 14;
        case BADGE_THREAT_CATEGORY_PRIVACY:   return 15;
        default:                              return 0;
    }
}

static void finalize_event_rank(badge_threat_event_t *event)
{
    if (!event) {
        return;
    }
    if (event->evidence_quality == 0) {
        event->evidence_quality = 1;
    }
    event->display_rank = category_priority(event->category) * 1000 +
                          (int)event->evidence_quality * 10 +
                          (int)(event->base_score + 0.5f);
}

static bool wifi_anomaly_is_lcd_worthy(const drone_detection_t *det)
{
    if (!det) {
        return false;
    }
    if (text_mentions_drone(det->manufacturer) ||
        text_mentions_drone(det->model) ||
        contains_nocase(det->manufacturer, "deauth") ||
        contains_nocase(det->class_reason, "deauth") ||
        contains_nocase(det->manufacturer, "disassoc") ||
        contains_nocase(det->class_reason, "disassoc") ||
        contains_nocase(det->manufacturer, "evil") ||
        contains_nocase(det->class_reason, "evil") ||
        contains_nocase(det->manufacturer, "twin") ||
        contains_nocase(det->class_reason, "twin") ||
        contains_nocase(det->manufacturer, "beacon spam") ||
        contains_nocase(det->class_reason, "beacon spam") ||
        contains_nocase(det->manufacturer, "anomaly") ||
        contains_nocase(det->class_reason, "anomaly") ||
        contains_nocase(det->manufacturer, "suspicious") ||
        contains_nocase(det->class_reason, "suspicious")) {
        return true;
    }
    if (det->source == DETECTION_SRC_WIFI_PROBE_REQUEST) {
        return det->confidence >= 0.62f ||
               (det->rssi < 0 && det->rssi >= -50);
    }
    if (det->source == DETECTION_SRC_WIFI_ASSOC) {
        return det->confidence >= 0.55f;
    }
    return false;
}

static bool ble_company_is_meta_glasses(uint16_t company_id)
{
    return company_id == 0x0D53;    /* Luxottica / Ray-Ban + Oakley frames */
}

static bool ble_services_mention_meta_glasses(const drone_detection_t *det)
{
    if (!det) {
        return false;
    }
    for (uint8_t i = 0; i < det->ble_svc_uuid_count && i < 4; i++) {
        uint16_t uuid = det->ble_service_uuids[i];
        if (uuid == 0xFD5F) {
            return true;
        }
    }
    return contains_nocase(det->ble_svc_uuids_raw, "fd5f");
}

static bool detection_has_meta_evidence(const drone_detection_t *det)
{
    if (!det) {
        return false;
    }
    return text_mentions_meta_glasses(det->manufacturer) ||
           text_mentions_meta_glasses(det->model) ||
           text_mentions_meta_glasses(det->ble_name) ||
           text_mentions_meta_glasses(det->class_reason) ||
           ble_company_is_meta_glasses(det->ble_company_id) ||
           ble_services_mention_meta_glasses(det);
}

static bool class_reason_is_raw_evidence(const char *reason)
{
    return contains_nocase(reason, "0x") ||
           contains_nocase(reason, "cid") ||
           contains_nocase(reason, "uuid") ||
           contains_nocase(reason, "mac");
}

static void copy_label(char *out, const char *label)
{
    if (!out) {
        return;
    }
    strncpy(out, label ? label : "Signal", BADGE_THREAT_LABEL_LEN - 1);
    out[BADGE_THREAT_LABEL_LEN - 1] = '\0';
}

static void copy_detail(char *out, const char *detail)
{
    if (!out) {
        return;
    }
    strncpy(out, detail ? detail : "", BADGE_THREAT_DETAIL_LEN - 1);
    out[BADGE_THREAT_DETAIL_LEN - 1] = '\0';
}

static void copy_evidence(char *out, const char *evidence)
{
    if (!out) {
        return;
    }
    strncpy(out, evidence ? evidence : "", BADGE_THREAT_EVIDENCE_LEN - 1);
    out[BADGE_THREAT_EVIDENCE_LEN - 1] = '\0';
}

static void copy_trimmed_reason(char *out, size_t out_len, const char *reason)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!reason) {
        return;
    }
    while (*reason == ' ' || *reason == '\t' || *reason == ':') {
        reason++;
    }
    snprintf(out, out_len, "%.*s", (int)(out_len - 1), reason);
}

static void copy_first_csv_token(char *out, size_t out_len, const char *text)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!text) {
        return;
    }
    size_t n = 0;
    while (text[n] && text[n] != ',' && n < out_len - 1) {
        out[n] = text[n];
        n++;
    }
    out[n] = '\0';
}

static void copy_compact_ssid_family(char *out, size_t out_len, const char *ssid)
{
    if (!out || out_len == 0) {
        return;
    }
    copy_first_csv_token(out, out_len, ssid);
    size_t len = strlen(out);
    while (len > 0 && out[len - 1] >= '0' && out[len - 1] <= '9') {
        len--;
    }
    if (len > 1 && (out[len - 1] == '-' || out[len - 1] == '_' ||
                    out[len - 1] == ' ')) {
        out[len - 1] = '\0';
    }
}

static const char *last_id_token(const char *id)
{
    const char *best = id;
    if (!id) return "";
    for (const char *p = id; *p; p++) {
        if (*p == ':' || *p == '-' || *p == '_') {
            best = p + 1;
        }
    }
    return best ? best : "";
}

static void copy_tracker_label(char *out, const drone_detection_t *det)
{
    const char *texts[] = {
        det ? det->manufacturer : NULL,
        det ? det->model : NULL,
        det ? det->ble_name : NULL,
        det ? det->drone_id : NULL,
        det ? det->class_reason : NULL,
    };

    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
        const char *text = texts[i];
        if (contains_nocase(text, "airtag")) {
            copy_label(out, "AirTag");
            return;
        }
        if (contains_nocase(text, "smarttag")) {
            copy_label(out, "SmartTag");
            return;
        }
        if (contains_nocase(text, "tile")) {
            copy_label(out, "Tile");
            return;
        }
        if (contains_nocase(text, "chipolo")) {
            copy_label(out, "Chipolo");
            return;
        }
        if (contains_nocase(text, "pebblebee")) {
            copy_label(out, "Pebblebee");
            return;
        }
        if (contains_nocase(text, "findmy") ||
            contains_nocase(text, "find my")) {
            copy_label(out, "Find My");
            return;
        }
        if (contains_nocase(text, "google tracker")) {
            copy_label(out, "Google Tag");
            return;
        }
        if (contains_nocase(text, "google tag")) {
            copy_label(out, "Google Tag");
            return;
        }
    }

    copy_label(out, "Tracker");
}

static void copy_drone_label_and_detail(badge_threat_event_t *event,
                                        const drone_detection_t *det)
{
    char detail[BADGE_THREAT_DETAIL_LEN] = {0};
    const char *id_tail = last_id_token(det->drone_id);

    if (det->source == DETECTION_SRC_WIFI_SSID) {
        copy_label(event->label, "Drone SSID");
    } else if (det->source == DETECTION_SRC_BLE_RID ||
        det->source == DETECTION_SRC_WIFI_BEACON) {
        copy_label(event->label, "Remote ID");
    } else if (det->source == DETECTION_SRC_WIFI_DJI_IE ||
               contains_nocase(det->manufacturer, "dji") ||
               contains_nocase(det->model, "dji")) {
        copy_label(event->label, "DJI Drone");
    } else if (det->self_id_text[0] != '\0') {
        copy_label(event->label, det->self_id_text);
    } else if (det->ssid[0] != '\0') {
        copy_label(event->label, det->ssid);
    } else if (det->model[0] != '\0' && !contains_nocase(det->model, "fp:")) {
        copy_label(event->label, det->model);
    } else if (det->source == DETECTION_SRC_WIFI_OUI) {
        copy_label(event->label, det->manufacturer[0] ? det->manufacturer : "Drone OUI");
    } else if (det->source == DETECTION_SRC_WIFI_SSID) {
        copy_label(event->label, "Drone SSID");
    } else if (det->manufacturer[0] != '\0') {
        copy_label(event->label, det->manufacturer);
    } else {
        copy_label(event->label, "Drone");
    }

    if (det->source == DETECTION_SRC_WIFI_SSID && det->ssid[0] != '\0') {
        snprintf(detail, sizeof(detail), "ssid %.26s", det->ssid);
    } else if (det->model[0] != '\0' && !contains_nocase(det->model, "fp:")) {
        snprintf(detail, sizeof(detail), "model %.24s", det->model);
    } else if (det->self_id_text[0] != '\0') {
        snprintf(detail, sizeof(detail), "self %.24s", det->self_id_text);
    } else if (det->operator_id[0] != '\0') {
        snprintf(detail, sizeof(detail), "op %.24s", det->operator_id);
    } else if (det->ssid[0] != '\0') {
        const char *kind = fof_policy_notable_ssid_label(det->ssid);
        if (strcmp(kind, det->ssid) == 0) {
            snprintf(detail, sizeof(detail), "%.30s", det->ssid);
        } else {
            snprintf(detail, sizeof(detail), "%.10s %.19s", kind, det->ssid);
        }
    } else if (det->manufacturer[0] != '\0' &&
               det->source == DETECTION_SRC_WIFI_OUI) {
        snprintf(detail, sizeof(detail), "vendor OUI");
    } else if (id_tail && id_tail[0] != '\0') {
        snprintf(detail, sizeof(detail), "RID decoded");
    } else {
        snprintf(detail, sizeof(detail), "RID evidence");
    }
    copy_detail(event->detail, detail);
}

static void format_detection_evidence(char *out,
                                      size_t out_len,
                                      const drone_detection_t *det)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!det) {
        return;
    }

    const char *reason = det->class_reason;
    switch (det->source) {
        case DETECTION_SRC_BLE_RID:
            snprintf(out, out_len, "BLE Remote ID");
            return;
        case DETECTION_SRC_WIFI_DJI_IE:
            snprintf(out, out_len, "DJI vendor IE");
            return;
        case DETECTION_SRC_WIFI_BEACON:
            snprintf(out, out_len, "WiFi Remote ID");
            return;
        case DETECTION_SRC_WIFI_SSID:
            if (det->ssid[0] != '\0') {
                snprintf(out, out_len, "ssid %.38s", det->ssid);
            } else {
                snprintf(out, out_len, "WiFi SSID pattern");
            }
            return;
        case DETECTION_SRC_WIFI_OUI:
            if (contains_nocase(reason, "flock")) {
                if (contains_nocase(reason, "registered")) {
                    snprintf(out, out_len, "Flock registered OUI");
                } else if (contains_nocase(reason, "field")) {
                    snprintf(out, out_len, "Flock field OUI");
                } else {
                    snprintf(out, out_len, "Flock WiFi OUI");
                }
            } else if (det->bssid[0] != '\0') {
                snprintf(out, out_len, "WiFi OUI %.17s", det->bssid);
            } else {
                snprintf(out, out_len, "WiFi OUI");
            }
            return;
        case DETECTION_SRC_WIFI_PROBE_REQUEST:
        case DETECTION_SRC_WIFI_ASSOC:
            if (contains_nocase(reason, "flock")) {
                if (contains_nocase(reason, "wildcard") ||
                    contains_nocase(reason, "probe")) {
                    snprintf(out, out_len, "Flock probe match");
                } else if (contains_nocase(reason, "data")) {
                    snprintf(out, out_len, "Flock data frame");
                } else {
                    snprintf(out, out_len, "Flock WiFi evidence");
                }
            } else if (contains_nocase(reason, "deauth")) {
                snprintf(out, out_len, "WiFi deauth frames");
            } else if (contains_nocase(reason, "disassoc")) {
                snprintf(out, out_len, "WiFi disassoc frames");
            } else if (contains_nocase(reason, "beacon spam")) {
                snprintf(out, out_len, "WiFi beacon spam");
            } else if (contains_nocase(reason, "evil") ||
                       contains_nocase(reason, "twin")) {
                snprintf(out, out_len, "WiFi evil twin hint");
            } else if (det->ssid[0] != '\0') {
                snprintf(out, out_len, "ssid %.38s", det->ssid);
            } else if (det->probed_ssids[0] != '\0') {
                snprintf(out, out_len, "probe %.37s", det->probed_ssids);
            } else {
                snprintf(out, out_len, "WiFi frame evidence");
            }
            return;
        case DETECTION_SRC_BLE_FINGERPRINT:
            if (contains_nocase(reason, "flock_ble_name")) {
                snprintf(out, out_len, "BLE name Flock");
            } else if (contains_nocase(reason, "0x0D53")) {
                snprintf(out, out_len, "CID 0x0D53 Luxottica");
            } else if (contains_nocase(reason, "0xFD5F")) {
                snprintf(out, out_len, "svc 0xFD5F Meta");
            } else if (contains_nocase(reason, "0xFEB7") ||
                       contains_nocase(reason, "0xFEB8")) {
                snprintf(out, out_len, "svc 0xFEB7/8 Meta");
            } else if (contains_nocase(reason, "0x1812") ||
                       contains_nocase(reason, "ble_hid")) {
                snprintf(out, out_len, "svc 0x1812 HID");
            } else if (contains_nocase(reason, "0xFEAA") ||
                       contains_nocase(reason, "venue_beacon")) {
                snprintf(out, out_len, "svc 0xFEAA beacon");
            } else if (contains_nocase(reason, "ble_audio") ||
                       contains_nocase(reason, "auracast")) {
                snprintf(out, out_len, "LE Audio broadcast");
            } else if (contains_nocase(reason, "default_uart_ble_name")) {
                snprintf(out, out_len, "BLE UART module name");
            } else if (contains_nocase(reason, "explicit_camera_ble_name")) {
                snprintf(out, out_len, "BLE camera name");
            } else if (contains_nocase(reason, "mobile_key_lock")) {
                snprintf(out, out_len, "BLE lock/mobile key");
            } else if (contains_nocase(reason, "event_badge")) {
                snprintf(out, out_len, "BLE event badge");
            } else if (contains_nocase(reason, "weak_meta")) {
                snprintf(out, out_len, "weak Meta presence");
            } else if (det->ble_company_id != 0) {
                snprintf(out, out_len, "CID 0x%04X",
                         (unsigned)det->ble_company_id);
            } else if (det->ble_svc_uuid_count > 0) {
                snprintf(out, out_len, "svc 0x%04X",
                         (unsigned)det->ble_service_uuids[0]);
            } else if (det->ble_name[0] != '\0') {
                snprintf(out, out_len, "BLE name %.36s", det->ble_name);
            } else if (reason && reason[0] != '\0') {
                copy_trimmed_reason(out, out_len, reason);
            } else {
                snprintf(out, out_len, "BLE fingerprint");
            }
            return;
        default:
            if (reason && reason[0] != '\0') {
                copy_trimmed_reason(out, out_len, reason);
            }
            return;
    }
}

static void copy_ble_detail(char *out, const drone_detection_t *det)
{
    char detail[BADGE_THREAT_DETAIL_LEN] = {0};

    if (det && det->ble_name[0] != '\0') {
        snprintf(detail, sizeof(detail), "%.30s", det->ble_name);
    } else if (det && contains_nocase(det->class_reason, "0x0D53")) {
        snprintf(detail, sizeof(detail), "Meta frame signal");
    } else if (det && contains_nocase(det->class_reason, "0xFD5F")) {
        snprintf(detail, sizeof(detail), "Meta glasses svc");
    } else if (det && (contains_nocase(det->class_reason, "0xFEB7") ||
                       contains_nocase(det->class_reason, "0xFEB8"))) {
        snprintf(detail, sizeof(detail), "Meta service");
    } else if (det && contains_nocase(det->class_reason, "status:meta")) {
        snprintf(detail, sizeof(detail), "Meta glasses signal");
    } else if (det && contains_nocase(det->class_reason, "name:meta_glasses")) {
        snprintf(detail, sizeof(detail), "Ray-Ban/Oakley hint");
    } else if (det && contains_nocase(det->class_reason, "weak_meta")) {
        snprintf(detail, sizeof(detail), "Meta presence");
    } else if (det && contains_nocase(det->class_reason, "flock_ble_name")) {
        snprintf(detail, sizeof(detail), "Flock BLE name");
    } else if (det && contains_nocase(det->class_reason, "default_uart_ble_name")) {
        snprintf(detail, sizeof(detail), "default BLE module");
    } else if (det && contains_nocase(det->class_reason, "explicit_camera_ble_name")) {
        snprintf(detail, sizeof(detail), "camera BLE name");
    } else if (det && contains_nocase(det->class_reason, "mobile_key_lock")) {
        snprintf(detail, sizeof(detail), "mobile key signal");
    } else if (det && contains_nocase(det->class_reason, "event_badge")) {
        snprintf(detail, sizeof(detail), "badge signal");
    } else if (det && contains_nocase(det->class_reason, "venue_beacon")) {
        snprintf(detail, sizeof(detail), "location beacon");
    } else if (det && contains_nocase(det->class_reason, "ble_hid")) {
        snprintf(detail, sizeof(detail), "input device");
    } else if (det && contains_nocase(det->class_reason, "0x1812")) {
        snprintf(detail, sizeof(detail), "HID input service");
    } else if (det && contains_nocase(det->class_reason, "0xFEAA")) {
        snprintf(detail, sizeof(detail), "Eddystone beacon");
    } else if (det && contains_nocase(det->class_reason, "auracast")) {
        snprintf(detail, sizeof(detail), "broadcast audio");
    } else if (det && contains_nocase(det->class_reason, "ble_audio")) {
        snprintf(detail, sizeof(detail), "broadcast audio");
    } else if (det && contains_nocase(det->class_reason, "strong BLE near")) {
        snprintf(detail, sizeof(detail), "strong BLE near");
    } else if (det && contains_nocase(det->class_reason, "structured BLE")) {
        snprintf(detail, sizeof(detail), "%.30s", det->class_reason);
    } else if (det && (contains_nocase(det->class_reason, "nearby ble") ||
                       contains_nocase(det->class_reason, "privacy signal"))) {
        snprintf(detail, sizeof(detail), "strong BLE near");
    } else if (det && det->class_reason[0] != '\0') {
        snprintf(detail, sizeof(detail), "%s",
                 class_reason_is_raw_evidence(det->class_reason)
                    ? "BLE evidence"
                    : det->class_reason);
    } else if (det && det->drone_id[0] != '\0') {
        snprintf(detail, sizeof(detail), "BLE signal");
    }
    copy_detail(out, detail);
}

static uint32_t ttl_for_class(badge_threat_class_t cls)
{
    switch (cls) {
        case BADGE_THREAT_DRONE:        return 300000;
        case BADGE_THREAT_META:         return 600000;
        case BADGE_THREAT_TRACKER:      return 900000;
        case BADGE_THREAT_WIFI_ANOMALY: return 180000;
        case BADGE_THREAT_BLE:          return 30000;
        case BADGE_THREAT_OTHER:        return 900000;
        default:                        return 0;
    }
}

static bool entity_is_drone_ssid(const badge_threat_entity_t *entity)
{
    return entity &&
           entity->cls == BADGE_THREAT_DRONE &&
           entity->category == BADGE_THREAT_CATEGORY_SSID;
}

static bool entity_is_meta_glasses(const badge_threat_entity_t *entity)
{
    return entity &&
           entity->cls == BADGE_THREAT_META &&
           entity->category == BADGE_THREAT_CATEGORY_GLASS;
}

static uint32_t ttl_for_entity(const badge_threat_entity_t *entity)
{
    if (entity_is_drone_ssid(entity)) {
        return 45000;
    }
    return entity ? ttl_for_class(entity->cls) : 0;
}

static uint32_t hold_for_class(badge_threat_class_t cls)
{
    switch (cls) {
        case BADGE_THREAT_META:
        case BADGE_THREAT_TRACKER:
        case BADGE_THREAT_OTHER:
            return 300000;
        case BADGE_THREAT_DRONE:
            return 90000;
        case BADGE_THREAT_WIFI_ANOMALY:
            return 60000;
        case BADGE_THREAT_BLE:
            return 12000;
        default:
            return 6000;
    }
}

static bool class_is_privacy(badge_threat_class_t cls)
{
    return cls == BADGE_THREAT_META ||
           cls == BADGE_THREAT_OTHER;
}

static bool coord_pair_is_valid(double lat, double lon)
{
    return (lat != 0.0 || lon != 0.0) &&
           lat >= -90.0 && lat <= 90.0 &&
           lon >= -180.0 && lon <= 180.0;
}

static int snapshot_class_priority(badge_threat_class_t cls)
{
    switch (cls) {
        case BADGE_THREAT_DRONE:        return 600;
        case BADGE_THREAT_META:         return 500;
        case BADGE_THREAT_OTHER:        return 400;
        case BADGE_THREAT_WIFI_ANOMALY: return 300;
        case BADGE_THREAT_TRACKER:      return 200;
        case BADGE_THREAT_BLE:          return 100;
        default:                        return 0;
    }
}

static bool entity_is_display_stale(const badge_threat_entity_t *entity,
                                    int64_t age_ms)
{
    if (!entity) {
        return false;
    }
    if (entity_is_drone_ssid(entity)) {
        return age_ms > BADGE_DRONE_SSID_LIVE_WINDOW_MS;
    }
    if (entity_is_meta_glasses(entity)) {
        return age_ms > BADGE_META_LIVE_WINDOW_MS;
    }
    if (class_is_privacy(entity->cls)) {
        return age_ms > 300000;
    }
    if (entity->cls == BADGE_THREAT_DRONE) {
        return age_ms > 90000;
    }
    if (entity->cls == BADGE_THREAT_WIFI_ANOMALY) {
        return age_ms > 60000;
    }
    return false;
}

static badge_threat_proximity_t proximity_for_rssi(int8_t rssi)
{
    if (rssi >= 0) {
        return BADGE_THREAT_PROX_UNKNOWN;
    }
    if (rssi >= -45) {
        return BADGE_THREAT_PROX_CLOSE;
    }
    if (rssi >= -60) {
        return BADGE_THREAT_PROX_NEARBY;
    }
    if (rssi >= -75) {
        return BADGE_THREAT_PROX_PRESENT;
    }
    return BADGE_THREAT_PROX_UNKNOWN;
}

static float proximity_floor_for_class(badge_threat_class_t cls,
                                       badge_threat_proximity_t prox)
{
    switch (prox) {
        case BADGE_THREAT_PROX_CLOSE:
            if (cls == BADGE_THREAT_BLE) return 26.0f;
            if (cls == BADGE_THREAT_TRACKER) return 38.0f;
            return class_is_privacy(cls) ? 92.0f : 84.0f;
        case BADGE_THREAT_PROX_NEARBY:
            if (cls == BADGE_THREAT_BLE) return 18.0f;
            if (cls == BADGE_THREAT_TRACKER) return 0.0f;
            return class_is_privacy(cls) ? 76.0f : 66.0f;
        case BADGE_THREAT_PROX_PRESENT:
            if (cls == BADGE_THREAT_BLE) return 10.0f;
            if (cls == BADGE_THREAT_TRACKER) return 0.0f;
            return class_is_privacy(cls) ? 54.0f : 44.0f;
        default:
            return 0.0f;
    }
}

static bool detection_has_ble_fingerprint_identity(const drone_detection_t *det);
static bool detection_copy_ble_fingerprint_identity(const drone_detection_t *det,
                                                    char *out,
                                                    size_t out_len);
static bool detection_is_status_meta_without_identity(const drone_detection_t *det);
static bool detection_is_weak_meta_presence(const drone_detection_t *det);
static bool detection_is_detector_weak_meta(const drone_detection_t *det);
static bool entity_is_weak_meta_presence(const badge_threat_entity_t *entity);

static void make_event_key(const drone_detection_t *det,
                           badge_threat_class_t cls,
                           badge_threat_category_t category,
                           char *out,
                           size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }

    if (cls == BADGE_THREAT_DRONE && det &&
        det->source == DETECTION_SRC_WIFI_SSID && det->ssid[0] != '\0') {
        char family[BADGE_THREAT_LABEL_LEN] = {0};
        copy_compact_ssid_family(family, sizeof(family), det->ssid);
        snprintf(out, out_len, "DRONE_SSID:%s", family[0] ? family : det->ssid);
        return;
    }

    if (cls == BADGE_THREAT_DRONE && det && det->drone_id[0] != '\0') {
        snprintf(out, out_len, "DRONE:%s", det->drone_id);
        return;
    }

    if (cls == BADGE_THREAT_WIFI_ANOMALY && det &&
        (det->source == DETECTION_SRC_WIFI_PROBE_REQUEST ||
         det->source == DETECTION_SRC_WIFI_ASSOC) &&
        detection_has_notable_ssid(det)) {
        char family[BADGE_THREAT_LABEL_LEN] = {0};
        copy_compact_ssid_family(family, sizeof(family),
                                 det->ssid[0] ? det->ssid : det->probed_ssids);
        snprintf(out, out_len, "SSID:%s", family[0] ? family : "notable");
        return;
    }

    if ((cls == BADGE_THREAT_META || cls == BADGE_THREAT_TRACKER ||
         cls == BADGE_THREAT_OTHER) && det) {
        const char *name = det->ble_name[0] ? det->ble_name :
                           det->model[0] ? det->model :
                           det->manufacturer[0] ? det->manufacturer :
                           det->class_reason[0] ? det->class_reason : "";
        if (cls == BADGE_THREAT_META) {
            char identity[40] = {0};
            if (detection_copy_ble_fingerprint_identity(det,
                                                        identity,
                                                        sizeof(identity))) {
                snprintf(out, out_len, "PRIV:META:GLASS:%s", identity);
                return;
            }
            (void)name;
            snprintf(out, out_len, "%s", BADGE_META_WEAK_KEY);
            return;
        }
        if (cls == BADGE_THREAT_TRACKER) {
            snprintf(out, out_len, "PRIV:TAG:%s", name[0] ? name : "tracker");
            return;
        }
        if (category == BADGE_THREAT_CATEGORY_BEACON) {
            snprintf(out, out_len, "PRIV:BEACON:AREA");
            return;
        }
        if (category == BADGE_THREAT_CATEGORY_EVENT_BADGE) {
            snprintf(out, out_len, "PRIV:EVENT:AREA");
            return;
        }
        if (category == BADGE_THREAT_CATEGORY_AUDIO) {
            snprintf(out, out_len, "PRIV:AUDIO:AREA");
            return;
        }
        if (category == BADGE_THREAT_CATEGORY_CAMERA ||
            category == BADGE_THREAT_CATEGORY_SKIM ||
            category == BADGE_THREAT_CATEGORY_LOCK ||
            category == BADGE_THREAT_CATEGORY_HID ||
            category == BADGE_THREAT_CATEGORY_FLOCK) {
            char identity[40] = {0};
            if (detection_copy_ble_fingerprint_identity(det,
                                                        identity,
                                                        sizeof(identity))) {
                snprintf(out, out_len, "PRIV:%d:%s",
                         (int)category,
                         identity);
                return;
            }
            snprintf(out, out_len, "PRIV:%d:%s",
                     (int)category,
                     name[0] ? name : "signal");
            return;
        }
        if (det->rssi < 0 && det->rssi >= -60) {
            snprintf(out, out_len, "PRIV:SIGNAL:%s", name[0] ? name : "near");
            return;
        }
    }

    if (cls == BADGE_THREAT_BLE && det &&
        det->source == DETECTION_SRC_BLE_FINGERPRINT) {
        snprintf(out, out_len, "BLE:NEARBY");
        return;
    }

    if (!fof_policy_detection_identity_key(det, out, out_len)) {
        snprintf(out, out_len, "SRC:%u:%s",
                 det ? (unsigned)det->source : 0U,
                 (det && det->drone_id[0]) ? det->drone_id : "unknown");
    }
}

static float clamp_score(float score)
{
    if (score < 0.0f) return 0.0f;
    if (score > 100.0f) return 100.0f;
    return score;
}

void badge_threat_state_init(badge_threat_state_t *state)
{
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
}

bool badge_threat_classify_detection(const drone_detection_t *det,
                                     badge_threat_event_t *event)
{
    if (!det || !event) {
        return false;
    }
    memset(event, 0, sizeof(*event));
    event->cls = BADGE_THREAT_IGNORE;
    event->source = det->source;
    event->confidence = det->confidence;

    if (detection_is_ambient_demo_ssid(det)) {
        return false;
    }

    const bool mfr_drone = text_mentions_drone(det->manufacturer) ||
                           text_mentions_drone(det->model) ||
                           text_mentions_drone(det->class_reason);
    const bool mfr_meta = detection_has_meta_evidence(det);
    const bool mfr_tracker = text_mentions_tracker(det->manufacturer) ||
                             text_mentions_tracker(det->model) ||
                             text_mentions_tracker(det->ble_name);
    const bool mfr_flock = detection_mentions_any(det, text_mentions_flock);
    const bool mfr_glasses = detection_mentions_any(det, text_mentions_glasses);
    const bool mfr_skimmer = detection_mentions_any(det, text_mentions_skimmer);
    const bool mfr_hidden_camera = detection_mentions_any(det, text_mentions_hidden_camera);
    const bool mfr_camera = detection_mentions_any(det, text_mentions_camera_privacy);
    const bool mfr_beacon = detection_mentions_any(det, text_mentions_venue_beacon);
    const bool mfr_event_badge = detection_mentions_any(det, text_mentions_event_badge);
    const bool mfr_lock = detection_mentions_any(det, text_mentions_mobile_key_lock);
    const bool mfr_hid = detection_mentions_any(det, text_mentions_ble_hid);
    const bool mfr_auracast = detection_mentions_any(det, text_mentions_auracast);
    const bool mfr_security = text_mentions_security_device(det->manufacturer) ||
                              text_mentions_security_device(det->model) ||
                              text_mentions_security_device(det->ble_name) ||
                              text_mentions_security_device(det->class_reason);

    if ((det->source == DETECTION_SRC_WIFI_OUI ||
         det->source == DETECTION_SRC_WIFI_SSID ||
         det->source == DETECTION_SRC_WIFI_ASSOC ||
         det->source == DETECTION_SRC_WIFI_PROBE_REQUEST) &&
        mfr_flock) {
        event->cls = BADGE_THREAT_WIFI_ANOMALY;
        event->category = BADGE_THREAT_CATEGORY_FLOCK;
        copy_label(event->label, "FLOCK Camera");
        if (det->ssid[0] != '\0') {
            char detail[BADGE_THREAT_DETAIL_LEN] = {0};
            snprintf(detail, sizeof(detail), "ssid %.26s", det->ssid);
            copy_detail(event->detail, detail);
        } else if (det->bssid[0] != '\0') {
            char detail[BADGE_THREAT_DETAIL_LEN] = {0};
            snprintf(detail, sizeof(detail), "bssid %.24s", det->bssid);
            copy_detail(event->detail, detail);
        } else if (det->class_reason[0] != '\0') {
            copy_detail(event->detail, "Flock WiFi evidence");
        } else {
            copy_detail(event->detail, "Flock WiFi signal");
        }
        event->base_score = 78.0f;
        event->evidence_quality = 8;
    } else if (source_is_drone_candidate(det->source) || mfr_drone) {
        if (!drone_detection_has_lift(det)) {
            return false;
        }
        event->cls = BADGE_THREAT_DRONE;
        event->category = (det->source == DETECTION_SRC_WIFI_SSID ||
                           det->source == DETECTION_SRC_WIFI_OUI)
            ? BADGE_THREAT_CATEGORY_SSID
            : BADGE_THREAT_CATEGORY_DRONE;
        copy_drone_label_and_detail(event, det);
        if (strcmp(event->label, "Drone") == 0 && event->detail[0] == '\0') {
            return false;
        }
        event->base_score = source_is_confirmed_drone(det->source) ? 82.0f :
                            det->source == DETECTION_SRC_WIFI_SSID ? 68.0f :
                            det->source == DETECTION_SRC_WIFI_OUI ? 58.0f : 62.0f;
        if (det->confidence >= 0.80f) event->base_score += 5.0f;
        event->evidence_quality = source_is_confirmed_drone(det->source) ? 9 :
                                  det->source == DETECTION_SRC_WIFI_SSID ? 7 :
                                  det->source == DETECTION_SRC_WIFI_OUI ? 5 : 4;
        if (coord_pair_is_valid(det->latitude, det->longitude)) {
            event->latitude = det->latitude;
            event->longitude = det->longitude;
            event->altitude_m = det->altitude_m;
            event->has_location = true;
        }
        if (coord_pair_is_valid(det->operator_lat, det->operator_lon)) {
            event->operator_lat = det->operator_lat;
            event->operator_lon = det->operator_lon;
            event->has_operator_location = true;
        }
        if (det->operator_id[0] != '\0') {
            strncpy(event->operator_id, det->operator_id,
                    sizeof(event->operator_id) - 1);
            event->operator_id[sizeof(event->operator_id) - 1] = '\0';
        }
    } else if ((det->source == DETECTION_SRC_WIFI_PROBE_REQUEST ||
                det->source == DETECTION_SRC_WIFI_ASSOC) &&
               detection_has_notable_ssid(det)) {
        const char *ssid_text = det->ssid[0] ? det->ssid : det->probed_ssids;
        const char *ssid_kind = fof_policy_notable_ssid_label(ssid_text);
        char ssid_family[BADGE_THREAT_LABEL_LEN] = {0};
        copy_compact_ssid_family(ssid_family, sizeof(ssid_family), ssid_text);
        event->cls = BADGE_THREAT_WIFI_ANOMALY;
        event->category = BADGE_THREAT_CATEGORY_SSID;
        if (contains_nocase(ssid_kind, "flock")) {
            event->category = BADGE_THREAT_CATEGORY_FLOCK;
            copy_label(event->label, "FLOCK Camera");
            char detail[BADGE_THREAT_DETAIL_LEN] = {0};
            snprintf(detail, sizeof(detail), "ssid %.26s", ssid_text);
            copy_detail(event->detail, detail);
        } else {
            copy_label(event->label, ssid_family[0] ? ssid_family : "Notable SSID");
            copy_detail(event->detail, ssid_kind);
        }
        event->base_score = 64.0f;
        event->evidence_quality = 6;
    } else if (det->source == DETECTION_SRC_BLE_FINGERPRINT && mfr_flock) {
        event->cls = BADGE_THREAT_OTHER;
        event->category = BADGE_THREAT_CATEGORY_FLOCK;
        copy_label(event->label, "FLOCK Camera");
        copy_ble_detail(event->detail, det);
        if (event->detail[0] == '\0') {
            copy_detail(event->detail, "Flock evidence");
        }
        event->base_score = 74.0f;
        event->evidence_quality = 8;
    } else if (det->source == DETECTION_SRC_BLE_FINGERPRINT && mfr_meta) {
        if (detection_is_status_meta_without_identity(det)) {
            return false;
        }
        bool weak_meta = detection_is_weak_meta_presence(det);
        bool detector_weak_meta = detection_is_detector_weak_meta(det);
        if (detector_weak_meta) {
            return false;
        }
        event->cls = BADGE_THREAT_META;
        event->category = BADGE_THREAT_CATEGORY_GLASS;
        if (text_mentions_meta_glasses(det->manufacturer) ||
            text_mentions_meta_glasses(det->model) ||
            text_mentions_meta_glasses(det->ble_name) ||
            text_mentions_meta_glasses(det->class_reason) ||
            det->ble_company_id == 0x0D53 ||
            ble_services_mention_meta_glasses(det)) {
            copy_label(event->label, "Meta Glasses");
        } else {
            copy_label(event->label, "Meta Device");
        }
        copy_ble_detail(event->detail, det);
        (void)weak_meta;
        event->base_score = 82.0f;
        event->evidence_quality = 8;
    } else if (det->source == DETECTION_SRC_BLE_FINGERPRINT && mfr_glasses) {
        if (detection_is_status_meta_without_identity(det)) {
            return false;
        }
        bool weak_meta = detection_is_weak_meta_presence(det);
        bool detector_weak_meta = detection_is_detector_weak_meta(det);
        if (detector_weak_meta) {
            return false;
        }
        event->cls = BADGE_THREAT_META;
        event->category = BADGE_THREAT_CATEGORY_GLASS;
        copy_label(event->label, "Smart Glasses");
        copy_ble_detail(event->detail, det);
        if (event->detail[0] == '\0') {
            copy_detail(event->detail, "glasses evidence");
        }
        event->base_score = 68.0f;
        event->evidence_quality = 7;
        (void)weak_meta;
    } else if (det->source == DETECTION_SRC_BLE_FINGERPRINT &&
               (mfr_skimmer || mfr_camera || mfr_hidden_camera ||
                mfr_event_badge || mfr_beacon || mfr_lock ||
                mfr_hid || mfr_auracast || mfr_security)) {
        if ((mfr_skimmer || mfr_beacon || mfr_event_badge ||
             mfr_hid || mfr_auracast) &&
            det->rssi < -72 &&
            det->confidence < 0.70f) {
            return false;
        }
        event->cls = BADGE_THREAT_OTHER;
        if (mfr_skimmer) {
            event->category = BADGE_THREAT_CATEGORY_SKIM;
            copy_label(event->label, "Skimmer");
            event->base_score = 66.0f;
            event->evidence_quality = 7;
        } else if (mfr_camera || mfr_hidden_camera) {
            event->category = BADGE_THREAT_CATEGORY_CAMERA;
            copy_label(event->label, "Camera Near");
            event->base_score = 58.0f;
            event->evidence_quality = 6;
        } else if (mfr_lock) {
            event->category = BADGE_THREAT_CATEGORY_LOCK;
            copy_label(event->label, "Lock Near");
            event->base_score = 50.0f;
            event->evidence_quality = 6;
        } else if (mfr_hid) {
            event->category = BADGE_THREAT_CATEGORY_HID;
            copy_label(event->label, "HID Near");
            event->base_score = 34.0f;
            event->evidence_quality = 4;
        } else if (mfr_event_badge) {
            event->category = BADGE_THREAT_CATEGORY_EVENT_BADGE;
            copy_label(event->label, "Event Badge");
            event->base_score = 36.0f;
            event->evidence_quality = 4;
        } else if (mfr_beacon) {
            event->category = BADGE_THREAT_CATEGORY_BEACON;
            copy_label(event->label, "Venue Beacon");
            event->base_score = 28.0f;
            event->evidence_quality = 3;
        } else if (mfr_auracast) {
            event->category = BADGE_THREAT_CATEGORY_AUDIO;
            copy_label(event->label, "Auracast");
            event->base_score = 22.0f;
            event->evidence_quality = 3;
        } else {
            event->category = BADGE_THREAT_CATEGORY_PRIVACY;
            copy_label(event->label, "Security Tool");
            event->base_score = 52.0f;
            event->evidence_quality = 5;
        }
        copy_ble_detail(event->detail, det);
        if (event->detail[0] == '\0') {
            copy_detail(event->detail, det->manufacturer[0] ? det->manufacturer : "privacy evidence");
        }
    } else if (det->source == DETECTION_SRC_BLE_FINGERPRINT && mfr_tracker) {
        if (!detection_is_close_tracker(det)) {
            return false;
        }
        event->cls = BADGE_THREAT_TRACKER;
        event->category = BADGE_THREAT_CATEGORY_TAG_CLOSE;
        copy_tracker_label(event->label, det);
        copy_ble_detail(event->detail, det);
        if (event->detail[0] == '\0') {
            copy_detail(event->detail, "close tag");
        }
        event->base_score = 36.0f;
        event->evidence_quality = 4;
    } else if (det->source == DETECTION_SRC_WIFI_PROBE_REQUEST ||
               det->source == DETECTION_SRC_WIFI_ASSOC) {
        if (!wifi_anomaly_is_lcd_worthy(det)) {
            return false;
        }
        event->cls = BADGE_THREAT_WIFI_ANOMALY;
        event->category = BADGE_THREAT_CATEGORY_WIFI;
        if (contains_nocase(det->manufacturer, "deauth") ||
            contains_nocase(det->class_reason, "deauth")) {
            copy_label(event->label, "Deauth");
            event->base_score = det->confidence >= 0.70f ? 70.0f : 55.0f;
            event->evidence_quality = 6;
        } else if (contains_nocase(det->manufacturer, "disassoc") ||
                   contains_nocase(det->class_reason, "disassoc")) {
            copy_label(event->label, "Disassoc");
            event->base_score = det->confidence >= 0.70f ? 62.0f : 46.0f;
            event->evidence_quality = 5;
        } else if (contains_nocase(det->manufacturer, "evil") ||
                   contains_nocase(det->class_reason, "evil") ||
                   contains_nocase(det->manufacturer, "twin") ||
                   contains_nocase(det->class_reason, "twin")) {
            copy_label(event->label, "Evil Twin");
            event->base_score = det->confidence >= 0.70f ? 75.0f : 60.0f;
            event->evidence_quality = 6;
        } else if (contains_nocase(det->manufacturer, "beacon spam") ||
                   contains_nocase(det->class_reason, "beacon spam")) {
            copy_label(event->label, "Beacon Spam");
            event->base_score = det->confidence >= 0.70f ? 60.0f : 45.0f;
            event->evidence_quality = 5;
        } else {
            copy_label(event->label, "Wi-Fi Anomaly");
            event->base_score = det->confidence >= 0.70f ? 55.0f : 40.0f;
            event->evidence_quality = 4;
        }
        copy_ble_detail(event->detail, det);
    } else if (det->source == DETECTION_SRC_WIFI_AP_INVENTORY) {
        return false;
    } else {
        return false;
    }

    if (det->rssi < 0 && det->rssi >= -50) {
        event->base_score += 10.0f;
    }
    event->base_score = clamp_score(event->base_score);
    event->ttl_ms = ttl_for_class(event->cls);
    format_detection_evidence(event->evidence, sizeof(event->evidence), det);
    finalize_event_rank(event);
    event->lcd_visible = true;
    make_event_key(det, event->cls, event->category,
                   event->key, sizeof(event->key));
    return event->key[0] != '\0' && badge_threat_label_is_lcd_safe(event->label);
}

static badge_threat_entity_t *find_entity(badge_threat_state_t *state,
                                          const char *key)
{
    if (!state || !key) {
        return NULL;
    }
    for (int i = 0; i < BADGE_THREAT_MAX_ENTITIES; i++) {
        if (state->entities[i].active &&
            strcmp(state->entities[i].key, key) == 0) {
            return &state->entities[i];
        }
    }
    return NULL;
}

static badge_threat_entity_t *claim_entity_slot(badge_threat_state_t *state,
                                                int64_t now_ms)
{
    if (!state) {
        return NULL;
    }
    badge_threat_entity_t *oldest = &state->entities[0];
    for (int i = 0; i < BADGE_THREAT_MAX_ENTITIES; i++) {
        badge_threat_entity_t *entity = &state->entities[i];
        if (!entity->active) {
            return entity;
        }
        if (entity->last_seen_ms < oldest->last_seen_ms) {
            oldest = entity;
        }
    }
    (void)now_ms;
    memset(oldest, 0, sizeof(*oldest));
    return oldest;
}

bool badge_threat_state_ingest(badge_threat_state_t *state,
                               const drone_detection_t *det,
                               int64_t now_ms,
                               badge_threat_event_t *event_out)
{
    if (!state || !det) {
        return false;
    }

    badge_threat_event_t event;
    if (!badge_threat_classify_detection(det, &event)) {
        return false;
    }
    if (event_out) {
        *event_out = event;
    }

    badge_threat_entity_t *entity = find_entity(state, event.key);
    const bool is_new = entity == NULL || !entity->active;
    if (!entity) {
        entity = claim_entity_slot(state, now_ms);
    }
    if (!entity) {
        return false;
    }

    if (is_new) {
        memset(entity, 0, sizeof(*entity));
        entity->active = true;
        entity->cls = event.cls;
        entity->category = event.category;
        copy_label(entity->label, event.label);
        copy_detail(entity->detail, event.detail);
        copy_evidence(entity->evidence, event.evidence);
        entity->last_source = event.source;
        entity->first_seen_ms = now_ms;
        entity->last_rssi = det->rssi;
        entity->strongest_rssi = det->rssi;
        strncpy(entity->key, event.key, sizeof(entity->key) - 1);
    }

    if (!is_new && event.display_rank >= entity->display_rank) {
        entity->cls = event.cls;
        entity->category = event.category;
        copy_label(entity->label, event.label);
        copy_detail(entity->detail, event.detail);
        copy_evidence(entity->evidence, event.evidence);
        entity->last_source = event.source;
    } else if (entity->detail[0] == '\0' && event.detail[0] != '\0') {
        copy_detail(entity->detail, event.detail);
    }
    if (entity->evidence[0] == '\0' && event.evidence[0] != '\0') {
        copy_evidence(entity->evidence, event.evidence);
    }
    entity->last_source = event.source;
    entity->last_seen_ms = now_ms;
    entity->event_count++;
    if (det->rssi < 0) {
        entity->last_rssi = det->rssi;
    }
    if (det->confidence > entity->peak_confidence) {
        entity->peak_confidence = det->confidence;
    }
    if (det->rssi < 0 &&
        (entity->strongest_rssi == 0 || det->rssi > entity->strongest_rssi)) {
        entity->strongest_rssi = det->rssi;
    }
    if (event.base_score > entity->base_score) {
        entity->base_score = event.base_score;
    }
    if (event.base_score > entity->current_score) {
        entity->current_score = event.base_score;
    }
    if (event.evidence_quality > entity->evidence_quality) {
        entity->evidence_quality = event.evidence_quality;
    }
    if (event.display_rank > entity->display_rank) {
        entity->display_rank = event.display_rank;
    }
    if (event.has_location) {
        entity->latitude = event.latitude;
        entity->longitude = event.longitude;
        entity->altitude_m = event.altitude_m;
        entity->has_location = true;
    }
    if (event.has_operator_location) {
        entity->operator_lat = event.operator_lat;
        entity->operator_lon = event.operator_lon;
        entity->has_operator_location = true;
    }
    if (event.operator_id[0] != '\0') {
        strncpy(entity->operator_id, event.operator_id,
                sizeof(entity->operator_id) - 1);
        entity->operator_id[sizeof(entity->operator_id) - 1] = '\0';
    }
    return true;
}

static float entity_score_now(const badge_threat_entity_t *entity,
                              int64_t now_ms)
{
    if (!entity || !entity->active) {
        return 0.0f;
    }
    int64_t age_ms = now_ms - entity->last_seen_ms;
    if (age_ms < 0) age_ms = 0;

    float score = entity->current_score > 0.0f
        ? entity->current_score
        : entity->base_score;

    uint32_t hold_ms = hold_for_class(entity->cls);
    if (age_ms > (int64_t)hold_ms) {
        float decay_per_ms = 0.008f;
        if (class_is_privacy(entity->cls)) {
            decay_per_ms = 0.00012f;
        } else if (entity->cls == BADGE_THREAT_DRONE) {
            decay_per_ms = 0.00035f;
        } else if (entity->cls == BADGE_THREAT_WIFI_ANOMALY) {
            decay_per_ms = 0.00055f;
        }
        score -= (float)(age_ms - (int64_t)hold_ms) * decay_per_ms;
    }

    int64_t sustained_ms = now_ms - entity->first_seen_ms;
    if (sustained_ms > 60000) {
        float normalization = (float)(sustained_ms - 60000) / 3000.0f;
        if (normalization > 20.0f) normalization = 20.0f;
        score -= normalization;
    }

    return clamp_score(score);
}

static void append_ticker(char *out, size_t out_len, const char *label)
{
    if (!out || !label || label[0] == '\0') {
        return;
    }
    size_t used = strlen(out);
    if (used >= out_len - 1) {
        return;
    }
    if (used > 0) {
        snprintf(out + used, out_len - used, "  |  ");
        used = strlen(out);
    }
    snprintf(out + used, out_len - used, "%s", label);
}

static uint32_t badge_hash_text32(const char *text)
{
    uint32_t hash = 2166136261U;
    if (!text) {
        text = "";
    }
    while (*text) {
        hash ^= (uint8_t)(*text++);
        hash *= 16777619U;
    }
    return hash;
}

static bool ascii_is_hex(char ch)
{
    return (ch >= '0' && ch <= '9') ||
           (ch >= 'a' && ch <= 'f') ||
           (ch >= 'A' && ch <= 'F');
}

static bool copy_prefixed_ble_hash(const char *text,
                                   const char *prefix,
                                   char *out,
                                   size_t out_len)
{
    if (!text || !prefix || !out || out_len == 0) {
        return false;
    }
    size_t prefix_len = strlen(prefix);
    if (strncmp(text, prefix, prefix_len) != 0) {
        return false;
    }
    const char *hash = text + prefix_len;
    for (int i = 0; i < 8; i++) {
        if (!ascii_is_hex(hash[i])) {
            return false;
        }
    }
    if (hash[8] != ':' && hash[8] != '\0') {
        return false;
    }
    snprintf(out, out_len, "%s%.8s", prefix, hash);
    return out[0] != '\0';
}

static bool detection_copy_ble_fingerprint_identity(const drone_detection_t *det,
                                                    char *out,
                                                    size_t out_len)
{
    if (!det || !out || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    if (copy_prefixed_ble_hash(det->model, "FP:", out, out_len)) {
        return true;
    }
    if (copy_prefixed_ble_hash(det->drone_id, "FP:", out, out_len)) {
        return true;
    }
    if (det->bssid[0] != '\0') {
        snprintf(out, out_len, "MAC:%s", det->bssid);
        return out[0] != '\0';
    }
    return false;
}

static bool detection_has_ble_fingerprint_identity(const drone_detection_t *det)
{
    char key[40] = {0};
    return detection_copy_ble_fingerprint_identity(det, key, sizeof(key));
}

static bool detection_is_status_meta_without_identity(const drone_detection_t *det)
{
    if (!det) {
        return false;
    }
    bool status_meta = strncmp(det->drone_id, "status:ble:meta", 15) == 0 ||
                       contains_nocase(det->class_reason, "status:meta");
    return status_meta && !detection_has_ble_fingerprint_identity(det);
}

static bool detection_is_weak_meta_presence(const drone_detection_t *det)
{
    if (!det) {
        return false;
    }
    if (detection_has_ble_fingerprint_identity(det)) {
        return false;
    }
    return contains_nocase(det->class_reason, "weak_meta") ||
           contains_nocase(det->class_reason, "glasses_detector") ||
           strncmp(det->drone_id, "meta:weak", 9) == 0 ||
           strncmp(det->drone_id, "status:ble:meta", 15) == 0 ||
           detection_has_meta_evidence(det);
}

static bool detection_is_detector_weak_meta(const drone_detection_t *det)
{
    if (!det || detection_has_ble_fingerprint_identity(det)) {
        return false;
    }
    return contains_nocase(det->class_reason, "weak_meta") ||
           contains_nocase(det->class_reason, "glasses_detector") ||
           strncmp(det->drone_id, "meta:weak", 9) == 0;
}

static bool entity_is_weak_meta_presence(const badge_threat_entity_t *entity)
{
    return entity &&
           entity->cls == BADGE_THREAT_META &&
           entity->category == BADGE_THREAT_CATEGORY_GLASS &&
           strcmp(entity->key, BADGE_META_WEAK_KEY) == 0;
}

static void copy_snapshot_display_id(char *out, size_t out_len,
                                     const badge_threat_entity_t *entity)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!entity || !entity->active) {
        return;
    }
    if (entity->cls == BADGE_THREAT_DRONE &&
        entity->category == BADGE_THREAT_CATEGORY_DRONE) {
        uint32_t hash = badge_hash_text32(entity->key[0] ? entity->key : entity->label);
        snprintf(out, out_len, "%04lX", (unsigned long)(hash & 0xFFFFU));
    } else if (entity->cls == BADGE_THREAT_META &&
               entity->category == BADGE_THREAT_CATEGORY_GLASS) {
        if (entity_is_weak_meta_presence(entity)) {
            return;
        }
        uint32_t hash = badge_hash_text32(entity->key[0] ? entity->key : entity->label);
        snprintf(out, out_len, "%04lX", (unsigned long)(hash & 0xFFFFU));
    }
}

static int snapshot_entity_rank(const badge_threat_snapshot_entity_t *item)
{
    if (!item || !item->active) {
        return 0;
    }
    badge_threat_proximity_t prox = item->stale
        ? BADGE_THREAT_PROX_UNKNOWN
        : item->proximity_level;
    int rank = item->display_rank > 0
        ? item->display_rank
        : (category_priority(item->category) * 1000 +
           snapshot_class_priority(item->cls));
    rank += item->score;
    rank += (int)prox * 20;
    if (item->stale) {
        rank -= 160;
    }
    return rank;
}

static int snapshot_entity_insert_rank(const badge_threat_snapshot_entity_t *items,
                                       int count,
                                       const badge_threat_snapshot_entity_t *item)
{
    int pos = count;
    int rank = snapshot_entity_rank(item);
    for (int i = 0; i < count; i++) {
        if (rank > snapshot_entity_rank(&items[i])) {
            pos = i;
            break;
        }
    }
    return pos;
}

void badge_threat_state_snapshot(badge_threat_state_t *state,
                                 int64_t now_ms,
                                 badge_threat_snapshot_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->dominant_class = BADGE_THREAT_IGNORE;
    out->dominant_proximity = BADGE_THREAT_PROX_UNKNOWN;
    if (!state) {
        out->color_rgb565 = badge_threat_score_to_rgb565(0.0f);
        copy_label(out->top_label, "Clear");
        return;
    }

    float aggregate = 0.0f;
    bool strong_nearby = false;

    for (int i = 0; i < BADGE_THREAT_MAX_ENTITIES; i++) {
        badge_threat_entity_t *entity = &state->entities[i];
        if (!entity->active) {
            continue;
        }
        int64_t age_ms = now_ms - entity->last_seen_ms;
        if (age_ms < 0) age_ms = 0;
        if (age_ms > (int64_t)ttl_for_entity(entity)) {
            memset(entity, 0, sizeof(*entity));
            continue;
        }

        float score = entity_score_now(entity, now_ms);
        entity->current_score = score;
        if (score < 5.0f) {
            memset(entity, 0, sizeof(*entity));
            continue;
        }

        bool stale = entity_is_display_stale(entity, age_ms);
        int8_t display_rssi = entity->strongest_rssi;
        if (entity->cls == BADGE_THREAT_META && entity->last_rssi < 0) {
            display_rssi = entity->last_rssi;
        }
        badge_threat_proximity_t prox = proximity_for_rssi(display_rssi);
        float proximity_floor = stale ? 0.0f :
            proximity_floor_for_class(entity->cls, prox);
        float display_score = score;
        if (display_score < proximity_floor) {
            display_score = proximity_floor;
        }
        if (stale) {
            if (class_is_privacy(entity->cls)) {
                display_score *= 0.45f;
            } else if (entity->cls == BADGE_THREAT_DRONE) {
                display_score *= 0.55f;
            } else if (entity->cls == BADGE_THREAT_WIFI_ANOMALY) {
                display_score *= 0.50f;
            } else {
                display_score *= 0.35f;
            }
        }
        if (!stale && entity->cls >= 0 && entity->cls < BADGE_THREAT_CLASS_COUNT) {
            out->active_counts[entity->cls]++;
            if (display_score > out->class_scores[entity->cls]) {
                out->class_scores[entity->cls] = display_score;
            }
        }
        if (!stale && prox >= BADGE_THREAT_PROX_NEARBY) {
            strong_nearby = true;
        }
        if (display_score > aggregate) {
            aggregate = display_score;
            out->dominant_class = entity->cls;
            out->dominant_proximity = prox;
        } else if (!stale &&
                   class_is_privacy(entity->cls) &&
                   prox > out->dominant_proximity) {
            out->dominant_class = entity->cls;
            out->dominant_proximity = prox;
        }

        badge_threat_snapshot_entity_t item = {
            .active = true,
            .stale = stale,
            .cls = entity->cls,
            .category = entity->category,
            .age_s = (int)(age_ms / 1000),
            .last_seen_s = (int)(age_ms / 1000),
            .score = (int)(display_score + 0.5f),
            .confidence_pct = (int)(entity->peak_confidence * 100.0f + 0.5f),
            .source = entity->last_source,
            .evidence_quality = entity->evidence_quality,
            .display_rank = entity->display_rank,
            .rssi = entity->last_rssi < 0 ? entity->last_rssi : entity->strongest_rssi,
            .best_rssi = entity->strongest_rssi,
            .event_count = entity->event_count,
            .seen_count = entity->event_count,
            .group_count = entity->event_count,
            .proximity_level = prox,
            .latitude = entity->latitude,
            .longitude = entity->longitude,
            .altitude_m = entity->altitude_m,
            .operator_lat = entity->operator_lat,
            .operator_lon = entity->operator_lon,
            .has_location = entity->has_location,
            .has_operator_location = entity->has_operator_location,
        };
        copy_label(item.label, entity->label);
        copy_detail(item.detail, entity->detail);
        copy_evidence(item.evidence, entity->evidence);
        copy_snapshot_display_id(item.display_id, sizeof(item.display_id), entity);
        strncpy(item.operator_id, entity->operator_id,
                sizeof(item.operator_id) - 1);
        item.operator_id[sizeof(item.operator_id) - 1] = '\0';

        int max_items = BADGE_THREAT_SNAPSHOT_ENTITIES;
        int count = out->entity_count;
        int pos = snapshot_entity_insert_rank(out->entities, count, &item);
        if (pos < max_items) {
            int end = count < max_items - 1 ? count : max_items - 1;
            for (int j = end; j > pos; j--) {
                out->entities[j] = out->entities[j - 1];
            }
            out->entities[pos] = item;
            if (out->entity_count < max_items) {
                out->entity_count++;
            }
        }
    }

    if (out->active_counts[BADGE_THREAT_META] > 0) {
        out->active_counts[BADGE_THREAT_META] =
            badge_threat_snapshot_meta_glasses_count(out);
    }

    if (out->active_counts[BADGE_THREAT_DRONE] > 0 &&
        out->active_counts[BADGE_THREAT_META] > 0) {
        aggregate += 10.0f;
    }
    if (out->active_counts[BADGE_THREAT_DRONE] > 1) {
        float bonus = (float)(out->active_counts[BADGE_THREAT_DRONE] - 1U) * 10.0f;
        aggregate += bonus > 25.0f ? 25.0f : bonus;
    }
    if (strong_nearby) {
        aggregate += 5.0f;
    }
    aggregate = clamp_score(aggregate);

    if (state->last_snapshot_ms <= 0 || now_ms < state->last_snapshot_ms) {
        state->display_score = aggregate;
    } else if (aggregate >= state->display_score) {
        state->display_score = aggregate;
    } else {
        float elapsed_s = (float)(now_ms - state->last_snapshot_ms) / 1000.0f;
        float step = elapsed_s * 8.0f;
        state->display_score = (state->display_score - aggregate) <= step
            ? aggregate
            : state->display_score - step;
    }
    state->display_score = clamp_score(state->display_score);
    state->last_snapshot_ms = now_ms;

    out->threat_score = state->display_score;
    out->color_rgb565 = badge_threat_score_to_rgb565(out->threat_score);

    if (out->entity_count > 0) {
        copy_label(out->top_label,
                   badge_threat_category_name(out->entities[0].category));
        for (int i = 0; i < out->entity_count; i++) {
            append_ticker(out->ticker, sizeof(out->ticker), out->entities[i].label);
        }
    } else {
        copy_label(out->top_label, "Watching");
        snprintf(out->ticker, sizeof(out->ticker), "Watching");
    }
}

const char *badge_threat_class_code(badge_threat_class_t cls)
{
    switch (cls) {
        case BADGE_THREAT_DRONE:        return "DRN";
        case BADGE_THREAT_META:         return "META";
        case BADGE_THREAT_TRACKER:      return "TAG";
        case BADGE_THREAT_WIFI_ANOMALY: return "WIFI";
        case BADGE_THREAT_BLE:          return "BLE";
        case BADGE_THREAT_OTHER:        return "OTR";
        default:                        return "IGN";
    }
}

const char *badge_threat_class_name(badge_threat_class_t cls)
{
    switch (cls) {
        case BADGE_THREAT_DRONE:        return "drone";
        case BADGE_THREAT_META:         return "meta";
        case BADGE_THREAT_TRACKER:      return "tracker";
        case BADGE_THREAT_WIFI_ANOMALY: return "wifi_anomaly";
        case BADGE_THREAT_BLE:          return "ble";
        case BADGE_THREAT_OTHER:        return "other";
        default:                        return "ignored";
    }
}

const char *badge_threat_category_code(badge_threat_category_t category)
{
    switch (category) {
        case BADGE_THREAT_CATEGORY_DRONE:     return "DRN";
        case BADGE_THREAT_CATEGORY_SSID:      return "SSID";
        case BADGE_THREAT_CATEGORY_FLOCK:     return "FLK";
        case BADGE_THREAT_CATEGORY_GLASS:     return "GLS";
        case BADGE_THREAT_CATEGORY_SKIM:      return "SKIM";
        case BADGE_THREAT_CATEGORY_CAMERA:    return "CAM";
        case BADGE_THREAT_CATEGORY_BEACON:    return "BCN";
        case BADGE_THREAT_CATEGORY_EVENT_BADGE: return "EVT";
        case BADGE_THREAT_CATEGORY_LOCK:      return "LOCK";
        case BADGE_THREAT_CATEGORY_HID:       return "HID";
        case BADGE_THREAT_CATEGORY_AUDIO:     return "AUD";
        case BADGE_THREAT_CATEGORY_WIFI:      return "WIFI";
        case BADGE_THREAT_CATEGORY_TAG_CLOSE: return "TAG";
        case BADGE_THREAT_CATEGORY_PRIVACY:   return "PRV";
        default:                              return "FOF";
    }
}

const char *badge_threat_category_name(badge_threat_category_t category)
{
    switch (category) {
        case BADGE_THREAT_CATEGORY_DRONE:     return "DRONE";
        case BADGE_THREAT_CATEGORY_SSID:      return "SSID";
        case BADGE_THREAT_CATEGORY_FLOCK:     return "FLOCK";
        case BADGE_THREAT_CATEGORY_GLASS:     return "GLASS";
        case BADGE_THREAT_CATEGORY_SKIM:      return "SKIM";
        case BADGE_THREAT_CATEGORY_CAMERA:    return "CAMERA";
        case BADGE_THREAT_CATEGORY_BEACON:    return "BEACON";
        case BADGE_THREAT_CATEGORY_EVENT_BADGE: return "EVENT";
        case BADGE_THREAT_CATEGORY_LOCK:      return "LOCK";
        case BADGE_THREAT_CATEGORY_HID:       return "HID";
        case BADGE_THREAT_CATEGORY_AUDIO:     return "AUDIO";
        case BADGE_THREAT_CATEGORY_WIFI:      return "WIFI";
        case BADGE_THREAT_CATEGORY_TAG_CLOSE: return "TAG";
        case BADGE_THREAT_CATEGORY_PRIVACY:   return "PRIV";
        default:                              return "WATCH";
    }
}

const char *badge_threat_source_code(uint8_t source)
{
    switch (source) {
        case DETECTION_SRC_BLE_RID:             return "ble_rid";
        case DETECTION_SRC_WIFI_SSID:           return "wifi_ssid";
        case DETECTION_SRC_WIFI_DJI_IE:         return "wifi_dji_ie";
        case DETECTION_SRC_WIFI_BEACON:         return "wifi_rid";
        case DETECTION_SRC_WIFI_OUI:            return "wifi_oui";
        case DETECTION_SRC_WIFI_PROBE_REQUEST:  return "wifi_probe";
        case DETECTION_SRC_BLE_FINGERPRINT:     return "ble_fingerprint";
        case DETECTION_SRC_WIFI_ASSOC:          return "wifi_assoc";
        case DETECTION_SRC_WIFI_AP_INVENTORY:   return "wifi_inventory";
        default:                                return "unknown";
    }
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r & 0xF8) << 8) |
                      ((uint16_t)(g & 0xFC) << 3) |
                      ((uint16_t)b >> 3));
}

static uint8_t lerp_u8(uint8_t a, uint8_t b, float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return (uint8_t)((float)a + ((float)b - (float)a) * t + 0.5f);
}

uint16_t badge_threat_score_to_rgb565(float score)
{
    static const uint8_t stops[][3] = {
        {  0, 220,  70 },
        {120, 255,  20 },
        {255, 235,   0 },
        {255, 125,   0 },
        {255,  25,  25 },
    };

    score = clamp_score(score);
    int seg = (int)(score / 20.0f);
    if (seg >= 4) {
        seg = 3;
        score = 80.0f;
    }
    float t = (score - (float)(seg * 20)) / 20.0f;
    return rgb565(
        lerp_u8(stops[seg][0], stops[seg + 1][0], t),
        lerp_u8(stops[seg][1], stops[seg + 1][1], t),
        lerp_u8(stops[seg][2], stops[seg + 1][2], t)
    );
}

bool badge_threat_label_is_lcd_safe(const char *label)
{
    if (!label || label[0] == '\0') {
        return false;
    }
    int hex_run = 0;
    int colon_count = 0;
    for (const char *p = label; *p; p++) {
        char ch = *p;
        bool hex = (ch >= '0' && ch <= '9') ||
                   (ch >= 'a' && ch <= 'f') ||
                   (ch >= 'A' && ch <= 'F');
        if (hex) {
            hex_run++;
        } else {
            hex_run = 0;
        }
        if (ch == ':') {
            colon_count++;
        }
        if (hex_run >= 6 || colon_count >= 2) {
            return false;
        }
    }
    return true;
}

uint32_t badge_threat_snapshot_count_active(const badge_threat_snapshot_t *snapshot,
                                            badge_threat_class_t cls,
                                            badge_threat_category_t category,
                                            bool include_stale)
{
    if (!snapshot) {
        return 0;
    }
    uint32_t count = 0;
    for (int i = 0; i < snapshot->entity_count; i++) {
        const badge_threat_snapshot_entity_t *item = &snapshot->entities[i];
        if (!item->active) {
            continue;
        }
        if (!include_stale && item->stale) {
            continue;
        }
        if (cls != BADGE_THREAT_IGNORE && item->cls != cls) {
            continue;
        }
        if (category != BADGE_THREAT_CATEGORY_NONE &&
            item->category != category) {
            continue;
        }
        count++;
    }
    return count;
}

uint32_t badge_threat_snapshot_drone_evidence_count(
    const badge_threat_snapshot_t *snapshot)
{
    if (!snapshot) {
        return 0;
    }
    uint32_t count = 0;
    char seen_keys[BADGE_THREAT_SNAPSHOT_ENTITIES][BADGE_THREAT_VIEW_KEY_LEN] = {{0}};
    size_t seen_count = 0;
    for (int i = 0; i < snapshot->entity_count; i++) {
        const badge_threat_snapshot_entity_t *item = &snapshot->entities[i];
        if (!item->active || item->stale || item->cls != BADGE_THREAT_DRONE) {
            continue;
        }
        if (!badge_threat_snapshot_entity_is_remote_id_drone(item) &&
            item->category != BADGE_THREAT_CATEGORY_SSID) {
            continue;
        }
        char key[BADGE_THREAT_VIEW_KEY_LEN] = {0};
        if (!badge_threat_snapshot_entity_view_key(item, key, sizeof(key)) ||
            badge_threat_snapshot_entity_view_key_seen(
                item,
                (const char (*)[BADGE_THREAT_VIEW_KEY_LEN])seen_keys,
                seen_count)) {
            continue;
        }
        if (seen_count < BADGE_THREAT_SNAPSHOT_ENTITIES) {
            snprintf(seen_keys[seen_count], sizeof(seen_keys[seen_count]),
                     "%s", key);
            seen_count++;
        }
        count++;
    }
    return count;
}

static uint32_t badge_threat_snapshot_remote_id_drone_count(
    const badge_threat_snapshot_t *snapshot)
{
    if (!snapshot) {
        return 0;
    }
    uint32_t count = 0;
    for (int i = 0; i < snapshot->entity_count; i++) {
        const badge_threat_snapshot_entity_t *item = &snapshot->entities[i];
        if (item->active && !item->stale &&
            badge_threat_snapshot_entity_is_remote_id_drone(item)) {
            count++;
        }
    }
    return count;
}

static uint32_t badge_threat_snapshot_drone_ssid_count(
    const badge_threat_snapshot_t *snapshot)
{
    if (!snapshot) {
        return 0;
    }
    uint32_t count = 0;
    char seen_keys[BADGE_THREAT_SNAPSHOT_ENTITIES][BADGE_THREAT_VIEW_KEY_LEN] = {{0}};
    size_t seen_count = 0;
    for (int i = 0; i < snapshot->entity_count; i++) {
        const badge_threat_snapshot_entity_t *item = &snapshot->entities[i];
        if (!item->active || item->stale ||
            item->cls != BADGE_THREAT_DRONE ||
            item->category != BADGE_THREAT_CATEGORY_SSID) {
            continue;
        }
        char key[BADGE_THREAT_VIEW_KEY_LEN] = {0};
        if (!badge_threat_snapshot_entity_view_key(item, key, sizeof(key)) ||
            badge_threat_snapshot_entity_view_key_seen(
                item,
                (const char (*)[BADGE_THREAT_VIEW_KEY_LEN])seen_keys,
                seen_count)) {
            continue;
        }
        if (seen_count < BADGE_THREAT_SNAPSHOT_ENTITIES) {
            snprintf(seen_keys[seen_count], sizeof(seen_keys[seen_count]),
                     "%s", key);
            seen_count++;
        }
        count++;
    }
    return count;
}

static bool badge_threat_snapshot_entity_is_live_drone_evidence(
    const badge_threat_snapshot_entity_t *item)
{
    return item && item->active && !item->stale &&
           item->cls == BADGE_THREAT_DRONE &&
           (badge_threat_snapshot_entity_is_remote_id_drone(item) ||
            item->category == BADGE_THREAT_CATEGORY_SSID);
}

const badge_threat_snapshot_entity_t *badge_threat_snapshot_strongest_drone_evidence(
    const badge_threat_snapshot_t *snapshot)
{
    if (!snapshot) {
        return NULL;
    }
    const badge_threat_snapshot_entity_t *best = NULL;
    uint8_t best_percent = 0;
    for (int i = 0; i < snapshot->entity_count; i++) {
        const badge_threat_snapshot_entity_t *item = &snapshot->entities[i];
        if (!badge_threat_snapshot_entity_is_live_drone_evidence(item)) {
            continue;
        }
        uint8_t percent = badge_threat_snapshot_entity_proximity_percent(item);
        if (!best ||
            percent > best_percent ||
            (percent == best_percent &&
             item->best_rssi < 0 &&
             (best->best_rssi >= 0 || item->best_rssi > best->best_rssi))) {
            best = item;
            best_percent = percent;
        }
    }
    return best;
}

uint8_t badge_threat_snapshot_drone_aggregate_heat_percent(
    const badge_threat_snapshot_t *snapshot)
{
    uint32_t count = badge_threat_snapshot_drone_evidence_count(snapshot);
    if (count == 0) {
        return 0;
    }
    const badge_threat_snapshot_entity_t *strongest =
        badge_threat_snapshot_strongest_drone_evidence(snapshot);
    uint8_t base = strongest
        ? badge_threat_snapshot_entity_proximity_percent(strongest)
        : 0;
    if (base == 0) {
        base = 18;
    }
    return badge_threat_heat_percent(base, count);
}

uint16_t badge_threat_snapshot_drone_aggregate_heat_color_rgb565(
    const badge_threat_snapshot_t *snapshot)
{
    return badge_threat_proximity_percent_to_rgb565(
        badge_threat_snapshot_drone_aggregate_heat_percent(snapshot));
}

uint32_t badge_threat_snapshot_entity_ordinal(const badge_threat_snapshot_t *snapshot,
                                              const badge_threat_snapshot_entity_t *item,
                                              badge_threat_class_t cls,
                                              badge_threat_category_t category,
                                              bool include_stale)
{
    if (!snapshot || !item) {
        return 0;
    }
    uint32_t ordinal = 0;
    for (int i = 0; i < snapshot->entity_count; i++) {
        const badge_threat_snapshot_entity_t *candidate = &snapshot->entities[i];
        if (!candidate->active) {
            continue;
        }
        if (!include_stale && candidate->stale) {
            continue;
        }
        if (cls != BADGE_THREAT_IGNORE && candidate->cls != cls) {
            continue;
        }
        if (category != BADGE_THREAT_CATEGORY_NONE &&
            candidate->category != category) {
            continue;
        }
        ordinal++;
        if (candidate == item) {
            return ordinal;
        }
    }
    return 0;
}

bool badge_threat_snapshot_entity_is_remote_id_drone(
    const badge_threat_snapshot_entity_t *item)
{
    return item &&
           item->cls == BADGE_THREAT_DRONE &&
           item->category == BADGE_THREAT_CATEGORY_DRONE &&
           strcmp(item->label, "Remote ID") == 0;
}

bool badge_threat_snapshot_entity_is_meta_glasses(
    const badge_threat_snapshot_entity_t *item)
{
    return item &&
           item->cls == BADGE_THREAT_META &&
           item->category == BADGE_THREAT_CATEGORY_GLASS;
}

uint32_t badge_threat_snapshot_meta_glasses_count(
    const badge_threat_snapshot_t *snapshot)
{
    if (!snapshot) {
        return 0;
    }
    uint32_t strong_count = 0;
    bool weak_present = false;
    for (int i = 0; i < snapshot->entity_count; i++) {
        const badge_threat_snapshot_entity_t *item = &snapshot->entities[i];
        if (!item->active || item->stale ||
            !badge_threat_snapshot_entity_is_meta_glasses(item)) {
            continue;
        }
        if (item->display_id[0] != '\0') {
            strong_count++;
        } else {
            weak_present = true;
        }
    }
    return strong_count > 0 ? strong_count : (weak_present ? 1U : 0U);
}

const badge_threat_snapshot_entity_t *badge_threat_snapshot_best_meta_glasses(
    const badge_threat_snapshot_t *snapshot)
{
    return badge_threat_snapshot_meta_glasses_at(snapshot, 0, NULL, NULL);
}

const badge_threat_snapshot_entity_t *badge_threat_snapshot_meta_glasses_at(
    const badge_threat_snapshot_t *snapshot,
    uint32_t phase,
    int *pos_out,
    int *total_out)
{
    if (pos_out) *pos_out = 0;
    if (total_out) *total_out = 0;
    if (!snapshot) {
        return NULL;
    }

    int strong_count = 0;
    for (int i = 0; i < snapshot->entity_count; i++) {
        const badge_threat_snapshot_entity_t *item = &snapshot->entities[i];
        if (!item->stale &&
            badge_threat_snapshot_entity_is_meta_glasses(item) &&
            item->display_id[0] != '\0') {
            strong_count++;
        }
    }
    if (strong_count > 0) {
        int target = (int)(phase % (uint32_t)strong_count);
        int seen = 0;
        for (int i = 0; i < snapshot->entity_count; i++) {
            const badge_threat_snapshot_entity_t *item = &snapshot->entities[i];
            if (item->stale ||
                !badge_threat_snapshot_entity_is_meta_glasses(item) ||
                item->display_id[0] == '\0') {
                continue;
            }
            if (seen == target) {
                if (pos_out) *pos_out = target + 1;
                if (total_out) *total_out = strong_count;
                return item;
            }
            seen++;
        }
    }

    for (int i = 0; i < snapshot->entity_count; i++) {
        const badge_threat_snapshot_entity_t *item = &snapshot->entities[i];
        if (!item->stale &&
            badge_threat_snapshot_entity_is_meta_glasses(item)) {
            if (pos_out) *pos_out = 1;
            if (total_out) *total_out = 1;
            return item;
        }
    }
    return NULL;
}

void badge_threat_format_meta_glasses_title(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    snprintf(out, out_len, "META GLASSES");
}

static uint8_t badge_threat_rssi_to_percent(int8_t rssi)
{
    if (rssi <= -90) return 0;
    if (rssi >= -40) return 100;
    return (uint8_t)(((rssi + 90) * 100 + 25) / 50);
}

static uint8_t badge_threat_lerp_percent(int rssi,
                                          int low_rssi,
                                          uint8_t low_percent,
                                          int high_rssi,
                                          uint8_t high_percent)
{
    int span = high_rssi - low_rssi;
    if (span <= 0) {
        return low_percent;
    }
    int delta = ((int)high_percent - (int)low_percent) *
                (rssi - low_rssi);
    int rounded = delta >= 0 ? delta + span / 2 : delta - span / 2;
    int percent = (int)low_percent + rounded / span;
    if (percent < 0) return 0;
    if (percent > 100) return 100;
    return (uint8_t)percent;
}

static uint8_t badge_threat_meta_rssi_to_percent(int8_t rssi)
{
    if (rssi <= -88) return 0;
    if (rssi >= -60) return 100;
    if (rssi <= -80) {
        return badge_threat_lerp_percent(rssi, -88, 0, -80, 25);
    }
    if (rssi <= -72) {
        return badge_threat_lerp_percent(rssi, -80, 25, -72, 55);
    }
    if (rssi <= -64) {
        return badge_threat_lerp_percent(rssi, -72, 55, -64, 85);
    }
    return badge_threat_lerp_percent(rssi, -64, 85, -60, 100);
}

uint8_t badge_threat_snapshot_entity_proximity_percent(
    const badge_threat_snapshot_entity_t *item)
{
    if (!item || !item->active || item->stale) {
        return 0;
    }
    if (item->best_rssi < 0) {
        return badge_threat_rssi_to_percent(item->best_rssi);
    }
    switch (item->proximity_level) {
        case BADGE_THREAT_PROX_CLOSE:  return 100;
        case BADGE_THREAT_PROX_NEARBY: return 70;
        case BADGE_THREAT_PROX_PRESENT:return 35;
        default:                       return 0;
    }
}

uint8_t badge_threat_snapshot_entity_signal_percent(
    const badge_threat_snapshot_entity_t *item)
{
    if (!item || !item->active || item->stale) {
        return 0;
    }

    uint8_t percent = 0;
    int8_t rssi = item->rssi < 0 ? item->rssi : item->best_rssi;
    if (rssi < 0) {
        percent = badge_threat_snapshot_entity_is_meta_glasses(item)
            ? badge_threat_meta_rssi_to_percent(rssi)
            : badge_threat_rssi_to_percent(rssi);
    } else {
        percent = badge_threat_snapshot_entity_proximity_percent(item);
    }

    int age_s = item->last_seen_s >= 0 ? item->last_seen_s : item->age_s;
    if (age_s <= 4) {
        return percent;
    }

    uint32_t decay = (uint32_t)(age_s - 4) * 2U;
    return decay >= percent ? 0 : (uint8_t)(percent - decay);
}

uint8_t badge_threat_snapshot_entity_heat_percent(
    const badge_threat_snapshot_entity_t *item,
    uint32_t live_count)
{
    if (!item || !item->active || item->stale) {
        return 0;
    }
    if (badge_threat_snapshot_entity_is_meta_glasses(item)) {
        return badge_threat_snapshot_entity_signal_percent(item);
    }
    return badge_threat_heat_percent(
        badge_threat_snapshot_entity_proximity_percent(item),
        live_count);
}

uint8_t badge_threat_heat_percent(uint8_t base_percent, uint32_t live_count)
{
    if (base_percent > 100) {
        base_percent = 100;
    }
    if (live_count == 0) {
        return 0;
    }
    uint32_t boost = live_count > 1 ? (live_count - 1U) * 12U : 0U;
    if (boost > 30U) {
        boost = 30U;
    }
    uint32_t heat = (uint32_t)base_percent + boost;
    return (uint8_t)(heat > 100U ? 100U : heat);
}

uint16_t badge_threat_proximity_percent_to_rgb565(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    return badge_threat_score_to_rgb565((float)percent);
}

uint16_t badge_threat_heat_percent_to_rgb565(uint8_t percent)
{
    static const uint8_t stops[][3] = {
        {  70, 220, 255 },
        {  30, 255, 185 },
        { 255, 240,  30 },
        { 255, 120,   0 },
        { 255,   0,   0 },
    };

    if (percent > 100) {
        percent = 100;
    }
    int seg = (int)(percent / 25U);
    if (seg >= 4) {
        seg = 3;
        percent = 100;
    }
    float t = ((float)percent - (float)(seg * 25)) / 25.0f;
    return rgb565(
        lerp_u8(stops[seg][0], stops[seg + 1][0], t),
        lerp_u8(stops[seg][1], stops[seg + 1][1], t),
        lerp_u8(stops[seg][2], stops[seg + 1][2], t)
    );
}

uint16_t badge_threat_snapshot_entity_heat_color_rgb565(
    const badge_threat_snapshot_entity_t *item,
    uint32_t live_count)
{
    uint8_t heat = badge_threat_snapshot_entity_heat_percent(item, live_count);
    if (badge_threat_snapshot_entity_is_meta_glasses(item)) {
        return badge_threat_heat_percent_to_rgb565(heat);
    }
    return badge_threat_proximity_percent_to_rgb565(heat);
}

static const badge_threat_snapshot_entity_t *badge_threat_snapshot_best_remote_id_drone(
    const badge_threat_snapshot_t *snapshot)
{
    if (!snapshot) {
        return NULL;
    }
    const badge_threat_snapshot_entity_t *best = NULL;
    uint8_t best_percent = 0;
    for (int i = 0; i < snapshot->entity_count; i++) {
        const badge_threat_snapshot_entity_t *item = &snapshot->entities[i];
        if (!item->active || item->stale ||
            !badge_threat_snapshot_entity_is_remote_id_drone(item)) {
            continue;
        }
        uint8_t percent = badge_threat_snapshot_entity_proximity_percent(item);
        if (!best ||
            percent > best_percent ||
            (percent == best_percent &&
             item->best_rssi < 0 &&
             (best->best_rssi >= 0 || item->best_rssi > best->best_rssi))) {
            best = item;
            best_percent = percent;
        }
    }
    return best;
}

static void badge_format_compact_coords(char *out, size_t out_len,
                                        double lat, double lon)
{
    if (!out || out_len == 0) {
        return;
    }
    snprintf(out, out_len, "%.3f,%.3f", lat, lon);
}

static const char *badge_tracker_known_label(const char *label)
{
    if (contains_nocase(label, "airtag")) return "AirTag";
    if (contains_nocase(label, "find my") ||
        contains_nocase(label, "findmy")) return "Find My";
    if (contains_nocase(label, "smarttag")) return "SmartTag";
    if (contains_nocase(label, "google")) return "Google Tag";
    if (contains_nocase(label, "chipolo")) return "Chipolo";
    if (contains_nocase(label, "pebblebee")) return "Pebblebee";
    if (contains_nocase(label, "tile")) return "Tile";
    return NULL;
}

static bool badge_tracker_detail_is_friendly(const char *detail)
{
    return detail && detail[0] != '\0' &&
           badge_threat_label_is_lcd_safe(detail) &&
           !contains_nocase(detail, "status:") &&
           !contains_nocase(detail, "structured") &&
           !contains_nocase(detail, "evidence") &&
           !contains_nocase(detail, "mfr") &&
           !contains_nocase(detail, "0x");
}

static bool badge_format_tracker_top_detail(
    const badge_threat_snapshot_entity_t *item,
    char *out,
    size_t out_len)
{
    if (!item || !out || out_len == 0 || item->cls != BADGE_THREAT_TRACKER) {
        return false;
    }
    const char *known = badge_tracker_known_label(item->label);
    const char *name = known ? known : NULL;
    if (!name && badge_tracker_detail_is_friendly(item->detail)) {
        name = item->detail;
    }
    if (!name) {
        name = "Tracker";
    }
    int8_t rssi = item->rssi < 0 ? item->rssi : item->best_rssi;
    int age_s = item->last_seen_s >= 0 ? item->last_seen_s : item->age_s;
    if (age_s < 0) {
        age_s = 0;
    }
    if (rssi < 0) {
        snprintf(out, out_len, "%.15s %ddB %ds", name, rssi, age_s);
    } else {
        snprintf(out, out_len, "%.18s %ds", name, age_s);
    }
    return true;
}

bool badge_threat_format_top_detail(
    const badge_threat_snapshot_t *snapshot,
    const badge_threat_snapshot_entity_t *item,
    char *out,
    size_t out_len)
{
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    if (!item) {
        return false;
    }

    if (item->cls == BADGE_THREAT_DRONE &&
        (badge_threat_snapshot_entity_is_remote_id_drone(item) ||
         item->category == BADGE_THREAT_CATEGORY_SSID)) {
        uint32_t rid_count = badge_threat_snapshot_remote_id_drone_count(snapshot);
        uint32_t ssid_count = badge_threat_snapshot_drone_ssid_count(snapshot);
        uint32_t count = badge_threat_snapshot_drone_evidence_count(snapshot);
        if (count == 0) {
            count = 1;
        }
        const badge_threat_snapshot_entity_t *rid_item =
            badge_threat_snapshot_best_remote_id_drone(snapshot);
        if (!rid_item && badge_threat_snapshot_entity_is_remote_id_drone(item)) {
            rid_item = item;
        }
        if (rid_item) {
            bool has_gps = rid_item->has_location;
            bool has_op = rid_item->has_operator_location ||
                          rid_item->operator_id[0] != '\0';
            if (rid_item->display_id[0] != '\0') {
                if (has_gps) {
                    char coords[24];
                    badge_format_compact_coords(coords, sizeof(coords),
                                                rid_item->latitude,
                                                rid_item->longitude);
                    snprintf(out, out_len, "RID #%s %s",
                             rid_item->display_id,
                             coords);
                } else if (has_op) {
                    snprintf(out, out_len, "RID #%s OP",
                             rid_item->display_id);
                } else if (rid_item->best_rssi < 0) {
                    snprintf(out, out_len, "RID #%s %ddB",
                             rid_item->display_id,
                             rid_item->best_rssi);
                } else {
                    snprintf(out, out_len, "RID #%s", rid_item->display_id);
                }
                return true;
            }
            if (has_gps) {
                char coords[24];
                badge_format_compact_coords(coords, sizeof(coords),
                                            rid_item->latitude,
                                            rid_item->longitude);
                snprintf(out, out_len, "GPS %s", coords);
                return true;
            }
            if (has_op && rid_item->operator_id[0] != '\0') {
                snprintf(out, out_len, "OP %.14s", rid_item->operator_id);
                return true;
            }
        }
        if (rid_count > 0 && ssid_count > 0) {
            snprintf(out, out_len, "RID x%lu SSID x%lu",
                     (unsigned long)rid_count,
                     (unsigned long)ssid_count);
        } else if (rid_count > 0) {
            const badge_threat_snapshot_entity_t *strongest =
                badge_threat_snapshot_strongest_drone_evidence(snapshot);
            int8_t rssi = strongest && strongest->best_rssi < 0
                ? strongest->best_rssi
                : item->best_rssi;
            if (rssi < 0) {
                snprintf(out, out_len, "%lu drone%s near %ddB",
                         (unsigned long)count,
                         count == 1 ? "" : "s",
                         rssi);
            } else {
                snprintf(out, out_len, "%lu drone%s near",
                         (unsigned long)count,
                         count == 1 ? "" : "s");
            }
        } else if (ssid_count > 0 && item->best_rssi < 0) {
            snprintf(out, out_len, "%lu drone%s near %ddB",
                     (unsigned long)count,
                     count == 1 ? "" : "s",
                     item->best_rssi);
        } else if (ssid_count > 0) {
            snprintf(out, out_len, "%lu drone SSID%s live",
                     (unsigned long)count,
                     count == 1 ? "" : "s");
        } else {
            snprintf(out, out_len, "%lu drone%s near",
                     (unsigned long)count,
                     count == 1 ? "" : "s");
        }
        return true;
    }

    if (badge_format_tracker_top_detail(item, out, out_len)) {
        return true;
    }

    if (item->cls == BADGE_THREAT_WIFI_ANOMALY ||
        item->category == BADGE_THREAT_CATEGORY_WIFI) {
        const char *kind = "WIFI ALERT";
        if (contains_nocase(item->label, "deauth") ||
            contains_nocase(item->detail, "deauth")) {
            kind = "DEAUTH";
        } else if (contains_nocase(item->label, "disassoc") ||
                   contains_nocase(item->detail, "disassoc")) {
            kind = "DISASSOC";
        } else if (contains_nocase(item->label, "beacon") ||
                   contains_nocase(item->detail, "beacon")) {
            kind = "BEACON SPAM";
        }
        int8_t rssi = item->rssi < 0 ? item->rssi : item->best_rssi;
        uint32_t count = item->seen_count > 1 ? item->seen_count
                         : parse_count_token(item->detail);
        int age_s = item->last_seen_s >= 0 ? item->last_seen_s : item->age_s;
        if (age_s < 0) {
            age_s = 0;
        }
        if (rssi < 0) {
            snprintf(out, out_len, "%s %ddB", kind, rssi);
        } else if (count > 0) {
            snprintf(out, out_len, "%s x%lu", kind,
                     (unsigned long)count);
            if (age_s > 0 && out_len > 0) {
                size_t used = strlen(out);
                if (used + 5 < out_len) {
                    snprintf(out + used, out_len - used, " %ds", age_s);
                }
            }
        } else {
            snprintf(out, out_len, "%s active", kind);
        }
        return true;
    }

    if (badge_threat_snapshot_entity_is_meta_glasses(item)) {
        uint32_t count = badge_threat_snapshot_meta_glasses_count(snapshot);
        if (count == 0) {
            count = 1;
        }
        int8_t rssi = item->rssi < 0 ? item->rssi : item->best_rssi;
        if (item->display_id[0] != '\0') {
            if (rssi < 0) {
                snprintf(out, out_len, "META #%s %ddB",
                         item->display_id,
                         rssi);
            } else {
                snprintf(out, out_len, "META #%s near", item->display_id);
            }
        } else if (rssi < 0) {
            snprintf(out, out_len, "%lu glass%s %ddB",
                     (unsigned long)count,
                     count == 1 ? "" : "es",
                     rssi);
        } else {
            snprintf(out, out_len, "%lu glass%s near",
                     (unsigned long)count,
                     count == 1 ? "" : "es");
        }
        return true;
    }

    return false;
}

bool badge_threat_top_detail_uses_large_text(const char *detail,
                                             size_t visible_chars)
{
    if (!detail || visible_chars == 0) {
        return false;
    }
    return strlen(detail) <= visible_chars;
}

static void badge_threat_snapshot_entity_view_title(
    const badge_threat_snapshot_entity_t *item,
    char *out,
    size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!item) {
        return;
    }
    switch (item->category) {
        case BADGE_THREAT_CATEGORY_GLASS:
            badge_threat_format_meta_glasses_title(out, out_len);
            return;
        case BADGE_THREAT_CATEGORY_FLOCK:
            snprintf(out, out_len, "FLOCK CAM");
            return;
        case BADGE_THREAT_CATEGORY_SKIM:
            snprintf(out, out_len, "SKIMMER");
            return;
        case BADGE_THREAT_CATEGORY_CAMERA:
            snprintf(out, out_len, "CAMERA NEAR");
            return;
        case BADGE_THREAT_CATEGORY_BEACON:
            snprintf(out, out_len, "BEACON AREA");
            return;
        case BADGE_THREAT_CATEGORY_EVENT_BADGE:
            snprintf(out, out_len, "EVENT BADGE");
            return;
        case BADGE_THREAT_CATEGORY_LOCK:
            snprintf(out, out_len, "LOCK NEAR");
            return;
        case BADGE_THREAT_CATEGORY_HID:
            snprintf(out, out_len, "HID NEAR");
            return;
        case BADGE_THREAT_CATEGORY_AUDIO:
            snprintf(out, out_len, "AURACAST");
            return;
        case BADGE_THREAT_CATEGORY_TAG_CLOSE:
            snprintf(out, out_len, "%s",
                     badge_tracker_known_label(item->label)
                         ? badge_tracker_known_label(item->label)
                         : "TRACKER");
            return;
        case BADGE_THREAT_CATEGORY_SSID:
            snprintf(out, out_len, "%s", item->cls == BADGE_THREAT_DRONE
                     ? "DRONE SSID" : "WIFI SSID");
            return;
        case BADGE_THREAT_CATEGORY_DRONE:
            snprintf(out, out_len, "%s",
                     badge_threat_snapshot_entity_is_remote_id_drone(item)
                         ? "DRONE NEAR"
                         : "DRONE");
            return;
        case BADGE_THREAT_CATEGORY_WIFI:
            snprintf(out, out_len, "WIFI ALERT");
            return;
        case BADGE_THREAT_CATEGORY_PRIVACY:
            snprintf(out, out_len, "PRIVACY");
            return;
        default:
            snprintf(out, out_len, "%s", item->label);
            return;
    }
}

bool badge_threat_snapshot_entity_view_key(
    const badge_threat_snapshot_entity_t *item,
    char *out,
    size_t out_len)
{
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    if (!item || !item->active) {
        return false;
    }
    if (badge_threat_snapshot_entity_is_remote_id_drone(item)) {
        if (item->display_id[0] != '\0') {
            snprintf(out, out_len, "RID:%s", item->display_id);
        } else {
            snprintf(out, out_len, "RID:%s:%s", item->label, item->detail);
        }
        return out[0] != '\0';
    }
    if (badge_threat_snapshot_entity_is_meta_glasses(item) &&
        item->display_id[0] != '\0') {
        snprintf(out, out_len, "META:%s", item->display_id);
        return out[0] != '\0';
    }

    char title[BADGE_THREAT_LABEL_LEN] = {0};
    badge_threat_snapshot_entity_view_title(item, title, sizeof(title));
    snprintf(out, out_len, "ENT:%d:%d:%s:%s",
             (int)item->cls,
             (int)item->category,
             title,
             item->detail);
    return out[0] != '\0';
}

bool badge_threat_snapshot_entity_view_key_seen(
    const badge_threat_snapshot_entity_t *item,
    const char viewed_keys[][BADGE_THREAT_VIEW_KEY_LEN],
    size_t viewed_count)
{
    if (!item || !viewed_keys || viewed_count == 0) {
        return false;
    }
    char key[BADGE_THREAT_VIEW_KEY_LEN] = {0};
    if (!badge_threat_snapshot_entity_view_key(item, key, sizeof(key))) {
        return false;
    }
    for (size_t i = 0; i < viewed_count; i++) {
        if (strcmp(viewed_keys[i], key) == 0) {
            return true;
        }
    }
    return false;
}

badge_threat_display_lane_t badge_threat_snapshot_entity_display_lane(
    const badge_threat_snapshot_entity_t *item)
{
    if (!item || !item->active) {
        return BADGE_THREAT_DISPLAY_LANE_NONE;
    }
    if (badge_threat_snapshot_entity_is_remote_id_drone(item)) {
        return BADGE_THREAT_DISPLAY_LANE_BLE;
    }
    if (item->cls == BADGE_THREAT_WIFI_ANOMALY ||
        item->category == BADGE_THREAT_CATEGORY_WIFI ||
        item->category == BADGE_THREAT_CATEGORY_SSID ||
        item->cls == BADGE_THREAT_DRONE) {
        return BADGE_THREAT_DISPLAY_LANE_WIFI;
    }
    if (item->cls == BADGE_THREAT_META ||
        item->cls == BADGE_THREAT_TRACKER ||
        item->cls == BADGE_THREAT_BLE ||
        item->cls == BADGE_THREAT_OTHER ||
        item->category == BADGE_THREAT_CATEGORY_GLASS ||
        item->category == BADGE_THREAT_CATEGORY_TAG_CLOSE ||
        item->category == BADGE_THREAT_CATEGORY_SKIM ||
        item->category == BADGE_THREAT_CATEGORY_CAMERA ||
        item->category == BADGE_THREAT_CATEGORY_BEACON ||
        item->category == BADGE_THREAT_CATEGORY_EVENT_BADGE ||
        item->category == BADGE_THREAT_CATEGORY_LOCK ||
        item->category == BADGE_THREAT_CATEGORY_HID ||
        item->category == BADGE_THREAT_CATEGORY_AUDIO ||
        item->category == BADGE_THREAT_CATEGORY_FLOCK ||
        item->category == BADGE_THREAT_CATEGORY_PRIVACY) {
        return BADGE_THREAT_DISPLAY_LANE_BLE;
    }
    return BADGE_THREAT_DISPLAY_LANE_NONE;
}

bool badge_threat_snapshot_should_show_lower_drone_evidence(
    const badge_threat_snapshot_t *snapshot,
    const badge_threat_snapshot_entity_t *item)
{
    if (!snapshot || !item || !item->active || item->stale ||
        item->cls != BADGE_THREAT_DRONE) {
        return false;
    }
    if (!badge_threat_snapshot_entity_is_remote_id_drone(item) &&
        item->category != BADGE_THREAT_CATEGORY_SSID) {
        return false;
    }
    if (badge_threat_snapshot_drone_evidence_count(snapshot) > 1) {
        return true;
    }
    if (badge_threat_snapshot_entity_is_remote_id_drone(item)) {
        return item->display_id[0] != '\0' ||
               item->has_location ||
               item->has_operator_location ||
               item->operator_id[0] != '\0';
    }
    return item->detail[0] != '\0';
}

bool badge_threat_snapshot_should_show_lower_meta_evidence(
    const badge_threat_snapshot_t *snapshot,
    const badge_threat_snapshot_entity_t *item,
    bool top_meta_active)
{
    if (!snapshot || !item || !item->active || item->stale ||
        !badge_threat_snapshot_entity_is_meta_glasses(item)) {
        return false;
    }
    (void)snapshot;
    return !top_meta_active;
}

size_t badge_threat_marquee_offset_rate(size_t text_len,
                                        size_t visible_chars,
                                        uint32_t frame,
                                        uint32_t step_chars,
                                        uint32_t step_frames)
{
    if (visible_chars == 0 || text_len <= visible_chars) {
        return 0;
    }
    if (step_chars == 0) {
        step_chars = 1;
    }
    if (step_frames == 0) {
        step_frames = 1;
    }
    size_t cycle = text_len + 3U;
    uint64_t scaled = ((uint64_t)frame * (uint64_t)step_chars) /
                      (uint64_t)step_frames;
    return (size_t)(scaled % (uint32_t)cycle);
}

size_t badge_threat_marquee_offset(size_t text_len,
                                   size_t visible_chars,
                                   uint32_t frame,
                                   uint32_t step_frames)
{
    return badge_threat_marquee_offset_rate(text_len, visible_chars, frame,
                                            1, step_frames);
}

void badge_threat_format_drone_near_title(const badge_threat_snapshot_t *snapshot,
                                          char *out,
                                          size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    (void)snapshot;
    snprintf(out, out_len, "DRONE NEAR");
}

void badge_threat_format_drone_entity_title(const badge_threat_snapshot_t *snapshot,
                                            const badge_threat_snapshot_entity_t *item,
                                            char *out,
                                            size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    uint32_t ordinal = badge_threat_snapshot_entity_ordinal(
        snapshot,
        item,
        BADGE_THREAT_DRONE,
        BADGE_THREAT_CATEGORY_DRONE,
        true
    );
    if (ordinal > 0) {
        snprintf(out, out_len, "DRONE #%lu", (unsigned long)ordinal);
    } else {
        snprintf(out, out_len, "DRONE");
    }
}

bool badge_threat_status_ssid_is_fresh(const char *ssid, int64_t age_s)
{
    return ssid && ssid[0] != '\0' &&
           age_s >= 0 &&
           age_s <= BADGE_SCANNER_STATUS_SSID_FRESH_S;
}
