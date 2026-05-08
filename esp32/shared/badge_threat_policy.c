#include "badge_threat_policy.h"

#include "detection_policy.h"

#include <stdio.h>
#include <string.h>

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

static bool text_mentions_meta(const char *text)
{
    return contains_nocase(text, "meta") ||
           contains_nocase(text, "ray-ban") ||
           contains_nocase(text, "rayban") ||
           contains_nocase(text, "rb meta") ||
           contains_nocase(text, "rb-") ||
           contains_nocase(text, "wayfarer") ||
           contains_nocase(text, "oakley") ||
           contains_nocase(text, "luxottica") ||
           contains_nocase(text, "quest");
}

static bool text_mentions_tracker(const char *text)
{
    return contains_nocase(text, "airtag") ||
           contains_nocase(text, "findmy") ||
           contains_nocase(text, "find my") ||
           contains_nocase(text, "tile") ||
           contains_nocase(text, "tracker") ||
           contains_nocase(text, "smarttag") ||
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

static bool text_mentions_ambient_demo_ssid(const char *text)
{
    return contains_nocase(text, "teamcharitycase") ||
           contains_nocase(text, "friendorfoe") ||
           contains_nocase(text, "fof-") ||
           contains_nocase(text, "fof_");
}

static bool text_mentions_glasses(const char *text)
{
    return text_mentions_meta(text) ||
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

static bool detection_is_close_airtag(const drone_detection_t *det)
{
    if (!det || det->source != DETECTION_SRC_BLE_FINGERPRINT || det->rssi < -50) {
        return false;
    }
    return contains_nocase(det->manufacturer, "airtag") ||
           contains_nocase(det->model, "airtag") ||
           contains_nocase(det->ble_name, "airtag") ||
           contains_nocase(det->drone_id, "airtag");
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
        case BADGE_THREAT_CATEGORY_WIFI:      return 30;
        case BADGE_THREAT_CATEGORY_TAG_CLOSE: return 20;
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

static bool ble_company_is_meta(uint16_t company_id)
{
    return company_id == 0x01AB ||  /* Meta Platforms */
           company_id == 0x058E ||  /* Meta Platforms Technologies */
           company_id == 0x0D53;    /* Luxottica / Ray-Ban + Oakley frames */
}

static bool ble_services_mention_meta(const drone_detection_t *det)
{
    if (!det) {
        return false;
    }
    for (uint8_t i = 0; i < det->ble_svc_uuid_count && i < 4; i++) {
        uint16_t uuid = det->ble_service_uuids[i];
        if (uuid == 0xFD5F || uuid == 0xFEB7 || uuid == 0xFEB8) {
            return true;
        }
    }
    return contains_nocase(det->ble_svc_uuids_raw, "fd5f") ||
           contains_nocase(det->ble_svc_uuids_raw, "feb7") ||
           contains_nocase(det->ble_svc_uuids_raw, "feb8");
}

static bool detection_has_meta_evidence(const drone_detection_t *det)
{
    if (!det) {
        return false;
    }
    return text_mentions_meta(det->manufacturer) ||
           text_mentions_meta(det->model) ||
           text_mentions_meta(det->ble_name) ||
           text_mentions_meta(det->class_reason) ||
           ble_company_is_meta(det->ble_company_id) ||
           ble_services_mention_meta(det) ||
           contains_nocase(det->class_reason, "0x01AB") ||
           contains_nocase(det->class_reason, "0x058E") ||
           contains_nocase(det->class_reason, "0x0D53") ||
           contains_nocase(det->class_reason, "0xFD5F");
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
    }

    copy_label(out, "Tracker");
}

static void copy_drone_label_and_detail(badge_threat_event_t *event,
                                        const drone_detection_t *det)
{
    char detail[BADGE_THREAT_DETAIL_LEN] = {0};
    const char *id_tail = last_id_token(det->drone_id);

    if (det->source == DETECTION_SRC_BLE_RID ||
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

    if (det->model[0] != '\0' && !contains_nocase(det->model, "fp:")) {
        snprintf(detail, sizeof(detail), "model %.24s", det->model);
    } else if (det->self_id_text[0] != '\0') {
        snprintf(detail, sizeof(detail), "self %.24s", det->self_id_text);
    } else if (det->operator_id[0] != '\0') {
        snprintf(detail, sizeof(detail), "op %.24s", det->operator_id);
    } else if (det->ssid[0] != '\0') {
        const char *kind = fof_policy_notable_ssid_label(det->ssid);
        snprintf(detail, sizeof(detail), "%.10s %.19s", kind, det->ssid);
    } else if (det->manufacturer[0] != '\0' &&
               det->source == DETECTION_SRC_WIFI_OUI) {
        snprintf(detail, sizeof(detail), "vendor OUI");
    } else if (id_tail && id_tail[0] != '\0') {
        snprintf(detail, sizeof(detail), "id %.26s", id_tail);
    } else {
        snprintf(detail, sizeof(detail), "RID evidence");
    }
    copy_detail(event->detail, detail);
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
    } else if (det && contains_nocase(det->class_reason, "name:meta_glasses")) {
        snprintf(detail, sizeof(detail), "Ray-Ban/Oakley hint");
    } else if (det && contains_nocase(det->class_reason, "flock_ble_name")) {
        snprintf(detail, sizeof(detail), "Flock BLE name");
    } else if (det && contains_nocase(det->class_reason, "default_uart_ble_name")) {
        snprintf(detail, sizeof(detail), "default BLE module");
    } else if (det && contains_nocase(det->class_reason, "explicit_camera_ble_name")) {
        snprintf(detail, sizeof(detail), "camera BLE name");
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

static void make_event_key(const drone_detection_t *det,
                           badge_threat_class_t cls,
                           char *out,
                           size_t out_len)
{
    if (!out || out_len == 0) {
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
            (void)name;
            snprintf(out, out_len, "PRIV:META:GLASS");
            return;
        }
        if (cls == BADGE_THREAT_TRACKER) {
            snprintf(out, out_len, "PRIV:TAG:%s", name[0] ? name : "tracker");
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

    if ((det->source == DETECTION_SRC_WIFI_PROBE_REQUEST ||
         det->source == DETECTION_SRC_WIFI_ASSOC ||
         det->source == DETECTION_SRC_WIFI_SSID) &&
        (text_mentions_ambient_demo_ssid(det->ssid) ||
         text_mentions_ambient_demo_ssid(det->probed_ssids) ||
         text_mentions_ambient_demo_ssid(det->drone_id))) {
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
    const bool mfr_security = text_mentions_security_device(det->manufacturer) ||
                              text_mentions_security_device(det->model) ||
                              text_mentions_security_device(det->ble_name) ||
                              text_mentions_security_device(det->class_reason);

    if (source_is_drone_candidate(det->source) || mfr_drone) {
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
        event->cls = BADGE_THREAT_META;
        event->category = BADGE_THREAT_CATEGORY_GLASS;
        if (contains_nocase(det->manufacturer, "glasses") ||
            contains_nocase(det->model, "glasses") ||
            contains_nocase(det->ble_name, "glasses") ||
            contains_nocase(det->class_reason, "0x0D53") ||
            contains_nocase(det->class_reason, "0xFD5F") ||
            contains_nocase(det->class_reason, "0xFEB7") ||
            contains_nocase(det->class_reason, "0xFEB8") ||
            det->ble_company_id == 0x0D53 ||
            ble_services_mention_meta(det) ||
            contains_nocase(det->manufacturer, "ray") ||
            contains_nocase(det->ble_name, "ray") ||
            contains_nocase(det->manufacturer, "oakley")) {
            copy_label(event->label, "Meta Glasses");
        } else {
            copy_label(event->label, "Meta Device");
        }
        copy_ble_detail(event->detail, det);
        event->base_score = 72.0f;
        event->evidence_quality = 8;
    } else if (det->source == DETECTION_SRC_BLE_FINGERPRINT && mfr_glasses) {
        event->cls = BADGE_THREAT_META;
        event->category = BADGE_THREAT_CATEGORY_GLASS;
        copy_label(event->label, "Smart Glasses");
        copy_ble_detail(event->detail, det);
        if (event->detail[0] == '\0') {
            copy_detail(event->detail, "glasses evidence");
        }
        event->base_score = 68.0f;
        event->evidence_quality = 7;
    } else if (det->source == DETECTION_SRC_BLE_FINGERPRINT &&
               (mfr_skimmer || mfr_hidden_camera || mfr_security)) {
        event->cls = BADGE_THREAT_OTHER;
        event->category = BADGE_THREAT_CATEGORY_SKIM;
        if (mfr_skimmer) {
            copy_label(event->label, "Skimmer");
        } else if (mfr_hidden_camera || contains_nocase(det->manufacturer, "camera")) {
            copy_label(event->label, "Hidden Cam");
        } else {
            copy_label(event->label, "Security Tool");
        }
        copy_ble_detail(event->detail, det);
        if (event->detail[0] == '\0') {
            copy_detail(event->detail, det->manufacturer[0] ? det->manufacturer : "privacy evidence");
        }
        event->base_score = mfr_skimmer ? 66.0f : 58.0f;
        event->evidence_quality = mfr_skimmer ? 7 : 6;
    } else if (det->source == DETECTION_SRC_BLE_FINGERPRINT && mfr_tracker) {
        if (!detection_is_close_airtag(det)) {
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
    finalize_event_rank(event);
    event->lcd_visible = true;
    make_event_key(det, event->cls, event->key, sizeof(event->key));
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
        entity->first_seen_ms = now_ms;
        entity->strongest_rssi = det->rssi;
        strncpy(entity->key, event.key, sizeof(entity->key) - 1);
    }

    if (!is_new && event.display_rank >= entity->display_rank) {
        entity->cls = event.cls;
        entity->category = event.category;
        copy_label(entity->label, event.label);
        copy_detail(entity->detail, event.detail);
    } else if (entity->detail[0] == '\0' && event.detail[0] != '\0') {
        copy_detail(entity->detail, event.detail);
    }
    entity->last_seen_ms = now_ms;
    entity->event_count++;
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
        if (age_ms > (int64_t)ttl_for_class(entity->cls)) {
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
        badge_threat_proximity_t prox = proximity_for_rssi(entity->strongest_rssi);
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
            .evidence_quality = entity->evidence_quality,
            .display_rank = entity->display_rank,
            .rssi = entity->strongest_rssi,
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
        case BADGE_THREAT_CATEGORY_WIFI:      return "WIFI";
        case BADGE_THREAT_CATEGORY_TAG_CLOSE: return "TAG";
        case BADGE_THREAT_CATEGORY_PRIVACY:   return "PRIV";
        default:                              return "WATCH";
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
