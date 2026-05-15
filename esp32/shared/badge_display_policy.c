#include "badge_display_policy.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *key;
    const char *label;
    bool enabled;
    badge_display_lane_t lane;
    badge_display_min_proximity_t min_proximity;
    uint8_t priority;
} badge_display_policy_default_t;

static const badge_display_policy_default_t DEFAULTS[BADGE_DISPLAY_POLICY_CLASS_COUNT] = {
    [BADGE_DISPLAY_CLASS_DRONE]         = {"drone", "Drone", true, BADGE_DISPLAY_LANE_BOTH, BADGE_DISPLAY_PROX_PRESENT, 100},
    [BADGE_DISPLAY_CLASS_META]          = {"meta", "Meta Glasses", true, BADGE_DISPLAY_LANE_BOTH, BADGE_DISPLAY_PROX_PRESENT, 95},
    [BADGE_DISPLAY_CLASS_TRACKER]       = {"tracker", "Trackers", true, BADGE_DISPLAY_LANE_LOWER, BADGE_DISPLAY_PROX_NEAR, 70},
    [BADGE_DISPLAY_CLASS_WIFI_ATTACK]   = {"wifi_attack", "WiFi Attacks", true, BADGE_DISPLAY_LANE_BOTH, BADGE_DISPLAY_PROX_PRESENT, 90},
    [BADGE_DISPLAY_CLASS_SKIMMER]       = {"skimmer", "Skimmers", true, BADGE_DISPLAY_LANE_BOTH, BADGE_DISPLAY_PROX_NEAR, 88},
    [BADGE_DISPLAY_CLASS_CAMERA]        = {"camera", "Cameras", true, BADGE_DISPLAY_LANE_LOWER, BADGE_DISPLAY_PROX_NEAR, 65},
    [BADGE_DISPLAY_CLASS_FLOCK]         = {"flock", "Flock/ALPR", true, BADGE_DISPLAY_LANE_BOTH, BADGE_DISPLAY_PROX_PRESENT, 85},
    [BADGE_DISPLAY_CLASS_LOCK]          = {"lock", "Locks", true, BADGE_DISPLAY_LANE_LOWER, BADGE_DISPLAY_PROX_NEAR, 55},
    [BADGE_DISPLAY_CLASS_HID]           = {"hid", "BLE HID", true, BADGE_DISPLAY_LANE_LOWER, BADGE_DISPLAY_PROX_CLOSE, 45},
    [BADGE_DISPLAY_CLASS_BEACON]        = {"beacon", "Venue Beacons", true, BADGE_DISPLAY_LANE_LOWER, BADGE_DISPLAY_PROX_NEAR, 30},
    [BADGE_DISPLAY_CLASS_EVENT_BADGE]   = {"event_badge", "Event Badges", true, BADGE_DISPLAY_LANE_LOWER, BADGE_DISPLAY_PROX_NEAR, 35},
    [BADGE_DISPLAY_CLASS_AURACAST]      = {"auracast", "Auracast", true, BADGE_DISPLAY_LANE_LOWER, BADGE_DISPLAY_PROX_NEAR, 20},
    [BADGE_DISPLAY_CLASS_SCANNER_STATUS]= {"scanner_status", "Scanner Status", true, BADGE_DISPLAY_LANE_LOWER, BADGE_DISPLAY_PROX_PRESENT, 10},
};

static char lower_char(char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
}

static bool eq_nocase(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        if (lower_char(*a++) != lower_char(*b++)) return false;
    }
    return *a == '\0' && *b == '\0';
}

static bool contains_nocase(const char *text, const char *needle)
{
    if (!text || !needle || needle[0] == '\0') return false;
    for (const char *p = text; *p; p++) {
        const char *a = p;
        const char *b = needle;
        while (*a && *b && lower_char(*a) == lower_char(*b)) {
            a++;
            b++;
        }
        if (*b == '\0') return true;
    }
    return false;
}

static void set_err(char *err, size_t err_len, const char *msg)
{
    if (!err || err_len == 0) return;
    snprintf(err, err_len, "%s", msg ? msg : "invalid policy");
}

void badge_display_policy_defaults(badge_display_policy_t *policy)
{
    if (!policy) return;
    memset(policy, 0, sizeof(*policy));
    policy->version = BADGE_DISPLAY_POLICY_VERSION;
    for (int i = 0; i < BADGE_DISPLAY_POLICY_CLASS_COUNT; i++) {
        policy->classes[i].enabled = DEFAULTS[i].enabled;
        policy->classes[i].lane = DEFAULTS[i].lane;
        policy->classes[i].min_proximity = DEFAULTS[i].min_proximity;
        policy->classes[i].priority = DEFAULTS[i].priority;
    }
}

const char *badge_display_policy_class_key(badge_display_policy_class_t cls)
{
    if ((int)cls < 0 || cls >= BADGE_DISPLAY_POLICY_CLASS_COUNT) return "unknown";
    return DEFAULTS[cls].key;
}

const char *badge_display_policy_class_label(badge_display_policy_class_t cls)
{
    if ((int)cls < 0 || cls >= BADGE_DISPLAY_POLICY_CLASS_COUNT) return "Unknown";
    return DEFAULTS[cls].label;
}

const char *badge_display_lane_name(badge_display_lane_t lane)
{
    switch (lane) {
        case BADGE_DISPLAY_LANE_OFF:   return "off";
        case BADGE_DISPLAY_LANE_LOWER: return "lower";
        case BADGE_DISPLAY_LANE_TOP:   return "top";
        case BADGE_DISPLAY_LANE_BOTH:  return "both";
        default:                       return "both";
    }
}

const char *badge_display_min_proximity_name(badge_display_min_proximity_t prox)
{
    switch (prox) {
        case BADGE_DISPLAY_PROX_PRESENT: return "present";
        case BADGE_DISPLAY_PROX_NEAR:    return "near";
        case BADGE_DISPLAY_PROX_CLOSE:   return "close";
        default:                         return "present";
    }
}

bool badge_display_policy_class_from_key(const char *key,
                                         badge_display_policy_class_t *out)
{
    if (!key) return false;
    for (int i = 0; i < BADGE_DISPLAY_POLICY_CLASS_COUNT; i++) {
        if (eq_nocase(key, DEFAULTS[i].key)) {
            if (out) *out = (badge_display_policy_class_t)i;
            return true;
        }
    }
    return false;
}

static bool lane_from_name(const char *name, badge_display_lane_t *out)
{
    if (eq_nocase(name, "off"))   { if (out) *out = BADGE_DISPLAY_LANE_OFF; return true; }
    if (eq_nocase(name, "lower")) { if (out) *out = BADGE_DISPLAY_LANE_LOWER; return true; }
    if (eq_nocase(name, "top"))   { if (out) *out = BADGE_DISPLAY_LANE_TOP; return true; }
    if (eq_nocase(name, "both"))  { if (out) *out = BADGE_DISPLAY_LANE_BOTH; return true; }
    return false;
}

static bool prox_from_name(const char *name, badge_display_min_proximity_t *out)
{
    if (eq_nocase(name, "present")) { if (out) *out = BADGE_DISPLAY_PROX_PRESENT; return true; }
    if (eq_nocase(name, "near"))    { if (out) *out = BADGE_DISPLAY_PROX_NEAR; return true; }
    if (eq_nocase(name, "close"))   { if (out) *out = BADGE_DISPLAY_PROX_CLOSE; return true; }
    return false;
}

static void hash_byte(uint32_t *h, uint8_t byte)
{
    *h ^= byte;
    *h *= 16777619u;
}

uint32_t badge_display_policy_hash(const badge_display_policy_t *policy)
{
    badge_display_policy_t fallback;
    if (!policy) {
        badge_display_policy_defaults(&fallback);
        policy = &fallback;
    }
    uint32_t h = 2166136261u;
    hash_byte(&h, policy->version);
    for (int i = 0; i < BADGE_DISPLAY_POLICY_CLASS_COUNT; i++) {
        hash_byte(&h, policy->classes[i].enabled ? 1 : 0);
        hash_byte(&h, (uint8_t)policy->classes[i].lane);
        hash_byte(&h, (uint8_t)policy->classes[i].min_proximity);
        hash_byte(&h, policy->classes[i].priority);
    }
    return h;
}

static const char *find_string_value(const char *obj, const char *field,
                                     char *out, size_t out_len)
{
    char pattern[40];
    snprintf(pattern, sizeof(pattern), "\"%s\"", field);
    const char *p = strstr(obj, pattern);
    if (!p) return NULL;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return NULL;
    p++;
    size_t used = 0;
    while (*p && *p != '"') {
        if (used + 1 < out_len) out[used++] = *p;
        p++;
    }
    if (*p != '"') return NULL;
    if (out_len > 0) out[used] = '\0';
    return p + 1;
}

static const char *find_bool_value(const char *obj, const char *field,
                                   bool *out)
{
    char pattern[40];
    snprintf(pattern, sizeof(pattern), "\"%s\"", field);
    const char *p = strstr(obj, pattern);
    if (!p) return NULL;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (strncmp(p, "true", 4) == 0) {
        if (out) *out = true;
        return p + 4;
    }
    if (strncmp(p, "false", 5) == 0) {
        if (out) *out = false;
        return p + 5;
    }
    return NULL;
}

static const char *find_int_value(const char *obj, const char *field,
                                  int *out)
{
    char pattern[40];
    snprintf(pattern, sizeof(pattern), "\"%s\"", field);
    const char *p = strstr(obj, pattern);
    if (!p) return NULL;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    int sign = 1;
    if (*p == '-') {
        sign = -1;
        p++;
    }
    if (*p < '0' || *p > '9') return NULL;
    int value = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        p++;
    }
    if (out) *out = value * sign;
    return p;
}

static const char *object_for_key(const char *json, const char *key,
                                  const char **end_out)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '{') return NULL;
    int depth = 0;
    const char *start = p;
    for (; *p; p++) {
        if (*p == '{') depth++;
        if (*p == '}') {
            depth--;
            if (depth == 0) {
                if (end_out) *end_out = p + 1;
                return start;
            }
        }
    }
    return NULL;
}

bool badge_display_policy_parse_json(const char *json,
                                     badge_display_policy_t *out,
                                     char *err,
                                     size_t err_len)
{
    if (!json || !out) {
        set_err(err, err_len, "missing policy");
        return false;
    }
    badge_display_policy_defaults(out);
    int version = BADGE_DISPLAY_POLICY_VERSION;
    if (find_int_value(json, "version", &version) && version != BADGE_DISPLAY_POLICY_VERSION) {
        set_err(err, err_len, "unsupported version");
        return false;
    }
    out->version = (uint8_t)version;

    const char *classes = strstr(json, "\"classes\"");
    if (!classes) {
        set_err(err, err_len, "missing classes");
        return false;
    }

    for (int i = 0; i < BADGE_DISPLAY_POLICY_CLASS_COUNT; i++) {
        const char *end = NULL;
        const char *obj = object_for_key(classes, DEFAULTS[i].key, &end);
        if (!obj) continue;
        char chunk[256];
        size_t chunk_len = end && end > obj ? (size_t)(end - obj) : strlen(obj);
        if (chunk_len >= sizeof(chunk)) {
            chunk_len = sizeof(chunk) - 1;
        }
        memcpy(chunk, obj, chunk_len);
        chunk[chunk_len] = '\0';
        obj = chunk;

        bool enabled = out->classes[i].enabled;
        if (strstr(obj, "\"enabled\"") && !find_bool_value(obj, "enabled", &enabled)) {
            set_err(err, err_len, "invalid enabled");
            return false;
        }
        out->classes[i].enabled = enabled;

        char value[16] = {0};
        if (strstr(obj, "\"lane\"")) {
            if (!find_string_value(obj, "lane", value, sizeof(value)) ||
                !lane_from_name(value, &out->classes[i].lane)) {
                set_err(err, err_len, "invalid lane");
                return false;
            }
        }
        if (strstr(obj, "\"min_proximity\"")) {
            if (!find_string_value(obj, "min_proximity", value, sizeof(value)) ||
                !prox_from_name(value, &out->classes[i].min_proximity)) {
                set_err(err, err_len, "invalid proximity");
                return false;
            }
        }
        int priority = out->classes[i].priority;
        if (strstr(obj, "\"priority\"")) {
            if (!find_int_value(obj, "priority", &priority) ||
                priority < 0 || priority > 100) {
                set_err(err, err_len, "invalid priority");
                return false;
            }
            out->classes[i].priority = (uint8_t)priority;
        }
    }
    return true;
}

size_t badge_display_policy_to_json(const badge_display_policy_t *policy,
                                    char *out,
                                    size_t out_len)
{
    badge_display_policy_t fallback;
    if (!policy) {
        badge_display_policy_defaults(&fallback);
        policy = &fallback;
    }
    if (!out || out_len == 0) return 0;
    size_t used = 0;
    int n = snprintf(out, out_len, "{\"version\":%u,\"classes\":{",
                     (unsigned)policy->version);
    if (n < 0) return 0;
    used = (size_t)n < out_len ? (size_t)n : out_len - 1;
    for (int i = 0; i < BADGE_DISPLAY_POLICY_CLASS_COUNT && used < out_len; i++) {
        n = snprintf(out + used, out_len - used,
                     "%s\"%s\":{\"enabled\":%s,\"lane\":\"%s\","
                     "\"min_proximity\":\"%s\",\"priority\":%u}",
                     i == 0 ? "" : ",",
                     DEFAULTS[i].key,
                     policy->classes[i].enabled ? "true" : "false",
                     badge_display_lane_name(policy->classes[i].lane),
                     badge_display_min_proximity_name(policy->classes[i].min_proximity),
                     (unsigned)policy->classes[i].priority);
        if (n < 0) break;
        used += (size_t)n;
        if (used >= out_len) {
            out[out_len - 1] = '\0';
            return out_len - 1;
        }
    }
    if (used + 2 < out_len) {
        out[used++] = '}';
        out[used++] = '}';
        out[used] = '\0';
    } else {
        out[out_len - 1] = '\0';
    }
    return used;
}

size_t badge_display_policy_to_command_json(const badge_display_policy_t *policy,
                                            uint32_t hash,
                                            char *out,
                                            size_t out_len)
{
    char policy_json[BADGE_DISPLAY_POLICY_JSON_MAX];
    badge_display_policy_to_json(policy, policy_json, sizeof(policy_json));
    if (!out || out_len == 0) return 0;
    int n = snprintf(out, out_len,
                     "{\"type\":\"display_policy\",\"version\":%u,"
                     "\"hash\":%lu,\"policy\":%s}",
                     BADGE_DISPLAY_POLICY_VERSION,
                     (unsigned long)hash,
                     policy_json);
    if (n < 0) return 0;
    if ((size_t)n >= out_len) {
        out[out_len - 1] = '\0';
        return out_len - 1;
    }
    return (size_t)n;
}

badge_display_min_proximity_t badge_display_proximity_for_rssi(int8_t rssi)
{
    if (rssi >= -60) return BADGE_DISPLAY_PROX_CLOSE;
    if (rssi >= -76) return BADGE_DISPLAY_PROX_NEAR;
    return BADGE_DISPLAY_PROX_PRESENT;
}

badge_display_policy_class_t badge_display_policy_class_for_detection(
    const drone_detection_t *det)
{
    if (!det) return BADGE_DISPLAY_CLASS_SCANNER_STATUS;
    const char *reason = det->class_reason;
    const char *mfr = det->manufacturer;
    const char *model = det->model;
    const char *ssid = det->ssid[0] ? det->ssid : det->drone_id;

    if (det->source == DETECTION_SRC_BLE_RID ||
        det->source == DETECTION_SRC_WIFI_DJI_IE ||
        det->source == DETECTION_SRC_WIFI_BEACON ||
        det->source == DETECTION_SRC_WIFI_SSID ||
        contains_nocase(reason, "remote id") ||
        contains_nocase(reason, "drone") ||
        contains_nocase(mfr, "dji") ||
        contains_nocase(ssid, "drone") ||
        contains_nocase(ssid, "uav") ||
        contains_nocase(ssid, "fpv")) {
        return BADGE_DISPLAY_CLASS_DRONE;
    }
    if (contains_nocase(reason, "deauth") ||
        contains_nocase(reason, "disassoc") ||
        contains_nocase(reason, "beacon spam") ||
        contains_nocase(reason, "evil") ||
        contains_nocase(reason, "pwnagotchi") ||
        contains_nocase(reason, "pineapple") ||
        contains_nocase(reason, "marauder")) {
        return BADGE_DISPLAY_CLASS_WIFI_ATTACK;
    }
    if (contains_nocase(reason, "flock") || contains_nocase(model, "flock") ||
        contains_nocase(mfr, "flock")) {
        return BADGE_DISPLAY_CLASS_FLOCK;
    }
    if (contains_nocase(reason, "meta") || contains_nocase(reason, "ray-ban") ||
        contains_nocase(reason, "rayban") || contains_nocase(reason, "oakley") ||
        contains_nocase(reason, "luxottica") || contains_nocase(mfr, "meta") ||
        contains_nocase(model, "meta")) {
        return BADGE_DISPLAY_CLASS_META;
    }
    if (contains_nocase(reason, "airtag") || contains_nocase(reason, "find my") ||
        contains_nocase(reason, "findmy") || contains_nocase(reason, "tracker") ||
        contains_nocase(reason, "tile") || contains_nocase(reason, "smarttag") ||
        contains_nocase(model, "tracker")) {
        return BADGE_DISPLAY_CLASS_TRACKER;
    }
    if (contains_nocase(reason, "skimmer") || contains_nocase(reason, "hc-05") ||
        contains_nocase(reason, "hc-06") || contains_nocase(reason, "hm-10") ||
        contains_nocase(reason, "bt05") || contains_nocase(reason, "jdy")) {
        return BADGE_DISPLAY_CLASS_SKIMMER;
    }
    if (contains_nocase(reason, "lock") || contains_nocase(reason, "dormakaba") ||
        contains_nocase(reason, "assa") || contains_nocase(reason, "salto") ||
        contains_nocase(reason, "onity") || contains_nocase(reason, "schlage") ||
        contains_nocase(reason, "yale")) {
        return BADGE_DISPLAY_CLASS_LOCK;
    }
    if (contains_nocase(reason, "ble hid") || contains_nocase(reason, "keyboard") ||
        contains_nocase(reason, "mouse") || contains_nocase(reason, "input device")) {
        return BADGE_DISPLAY_CLASS_HID;
    }
    if (contains_nocase(reason, "event badge") || contains_nocase(reason, "conference badge") ||
        contains_nocase(reason, "wristband") || contains_nocase(reason, "bizzabo") ||
        contains_nocase(reason, "cvent")) {
        return BADGE_DISPLAY_CLASS_EVENT_BADGE;
    }
    if (contains_nocase(reason, "auracast") || contains_nocase(reason, "le audio") ||
        contains_nocase(reason, "broadcast audio")) {
        return BADGE_DISPLAY_CLASS_AURACAST;
    }
    if (contains_nocase(reason, "beacon") || contains_nocase(reason, "ibeacon") ||
        contains_nocase(reason, "eddystone") || contains_nocase(reason, "estimote") ||
        contains_nocase(reason, "kontakt") || contains_nocase(reason, "gimbal")) {
        return BADGE_DISPLAY_CLASS_BEACON;
    }
    if (contains_nocase(reason, "camera") || contains_nocase(reason, "cam") ||
        contains_nocase(reason, "gopro") || contains_nocase(reason, "axon") ||
        contains_nocase(reason, "samsara") || contains_nocase(reason, "verkada")) {
        return BADGE_DISPLAY_CLASS_CAMERA;
    }
    return BADGE_DISPLAY_CLASS_SCANNER_STATUS;
}

bool badge_display_policy_is_safety_floor(badge_display_policy_class_t cls,
                                          badge_display_min_proximity_t prox,
                                          int score)
{
    switch (cls) {
        case BADGE_DISPLAY_CLASS_DRONE:
        case BADGE_DISPLAY_CLASS_WIFI_ATTACK:
        case BADGE_DISPLAY_CLASS_SKIMMER:
        case BADGE_DISPLAY_CLASS_FLOCK:
            return true;
        case BADGE_DISPLAY_CLASS_META:
        case BADGE_DISPLAY_CLASS_TRACKER:
        case BADGE_DISPLAY_CLASS_CAMERA:
        case BADGE_DISPLAY_CLASS_LOCK:
        case BADGE_DISPLAY_CLASS_HID:
            return prox >= BADGE_DISPLAY_PROX_CLOSE || score >= 80;
        default:
            return false;
    }
}

bool badge_display_policy_allows_class(const badge_display_policy_t *policy,
                                       badge_display_policy_class_t cls,
                                       badge_display_min_proximity_t prox,
                                       int score,
                                       bool *safety_floor)
{
    badge_display_policy_t fallback;
    if (!policy) {
        badge_display_policy_defaults(&fallback);
        policy = &fallback;
    }
    if ((int)cls < 0 || cls >= BADGE_DISPLAY_POLICY_CLASS_COUNT) {
        cls = BADGE_DISPLAY_CLASS_SCANNER_STATUS;
    }
    bool safety = badge_display_policy_is_safety_floor(cls, prox, score);
    if (safety_floor) *safety_floor = safety;
    const badge_display_class_policy_t *cfg = &policy->classes[cls];
    if (safety) return true;
    if (!cfg->enabled || cfg->lane == BADGE_DISPLAY_LANE_OFF) return false;
    return prox >= cfg->min_proximity;
}

bool badge_display_policy_allows_detection(const badge_display_policy_t *policy,
                                           const drone_detection_t *det,
                                           bool *safety_floor,
                                           badge_display_policy_class_t *cls_out)
{
    badge_display_policy_class_t cls = badge_display_policy_class_for_detection(det);
    if (cls_out) *cls_out = cls;
    badge_display_min_proximity_t prox = det
        ? badge_display_proximity_for_rssi(det->rssi)
        : BADGE_DISPLAY_PROX_PRESENT;
    int score = det ? (int)(det->confidence * 100.0f) : 0;
    return badge_display_policy_allows_class(policy, cls, prox, score,
                                             safety_floor);
}
