/**
 * Friend or Foe -- Promiscuous WiFi Scanner
 *
 * Captures raw 802.11 management frames (beacons) in promiscuous mode,
 * then applies a detection pipeline:
 *
 *   1. DJI vendor-specific IE  -> conf 0.85, full GPS position
 *   2. ASTM F3411 Beacon RID   -> conf 0.90, ODID position
 *   3. SSID pattern match      -> conf 0.30, no position
 *   4. OUI prefix match        -> conf 0.40, no position
 *
 * ESP32-S3: 2.4 GHz channels 1-13 (~1.3s full sweep).
 * ESP32-C5: Interleaved 2.4 + 5 GHz channels (~3.8s full sweep).
 * Dwell time: ~100ms per channel.
 */

#include "wifi_scanner.h"
#include "wifi_ssid_patterns.h"
#include "wifi_oui_database.h"
#include "dji_drone_id_parser.h"
#include "wifi_beacon_rid_parser.h"
#include "french_dri_parser.h"
#include "open_drone_id_parser.h"
#include "constants.h"
#include "detection_types.h"
#include "core/task_priorities.h"

#include <stdlib.h>
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ── Constants ─────────────────────────────────────────────────────────────── */

static const char *TAG = "wifi_scan";

#ifdef CONFIG_SCANNER_5GHZ_ENABLED
#define CHANNEL_DWELL_MS            300   /* C5: slightly longer for dual-band (was 2000 — way too slow) */
#else
#define CHANNEL_DWELL_MS            200   /* ESP32: fast hop for 2.4 GHz only */
#endif

/* ── Channel tables ────────────────────────────────────────────────────────── */

static const uint16_t s_channels_24ghz[] = {
    1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13   /* popular channels first */
};
#define NUM_CHANNELS_24GHZ  (sizeof(s_channels_24ghz) / sizeof(s_channels_24ghz[0]))

#ifdef CONFIG_SCANNER_5GHZ_ENABLED
static const uint16_t s_channels_5ghz[] = {
    36,  40,  44,  48,                             /* UNII-1 (most common) */
    149, 153, 157, 161, 165,                       /* UNII-3 (common) */
    52,  56,  60,  64,                             /* UNII-2 */
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144  /* UNII-2 Ext */
};
#define NUM_CHANNELS_5GHZ   (sizeof(s_channels_5ghz) / sizeof(s_channels_5ghz[0]))
#endif

/* 802.11 frame control: Management frame type = 0x00, Beacon subtype = 0x80 */
#define WIFI_FC_TYPE_MGMT           0x00
#define WIFI_FC_SUBTYPE_BEACON      0x80

/* Beacon frame fixed fields: Timestamp(8) + Interval(2) + Capability(2) = 12 bytes */
#define BEACON_FIXED_FIELDS_LEN     12

/* Minimum beacon frame: FC(2) + Duration(2) + DA(6) + SA(6) + BSSID(6) + SeqCtrl(2) + Fixed(12) = 36 */
#define BEACON_HEADER_LEN           24
#define BEACON_TAGGED_PARAMS_OFFSET (BEACON_HEADER_LEN + BEACON_FIXED_FIELDS_LEN)

/* 802.11 information element tag IDs */
#define IE_TAG_SSID                 0
#define IE_TAG_VENDOR_SPECIFIC      221

/* ── Module state ──────────────────────────────────────────────────────────── */

static QueueHandle_t s_detection_queue = NULL;
static uint16_t      s_current_channel = 1;
static size_t        s_idx_24ghz = 0;
#ifdef CONFIG_SCANNER_5GHZ_ENABLED
static size_t        s_idx_5ghz = 0;
static bool          s_next_is_5ghz = false; /* interleave toggle */
#endif

/* ── Forward declarations ─────────────────────────────────────────────────── */
static void add_channel_heat(uint16_t ch, uint8_t points);
static void decay_channel_heat(void);

/* ── Lock-on mode ─────────────────────────────────────────────────────────── */

typedef struct {
    bool     active;
    uint8_t  channel;               /* Channel to lock onto */
    char     target_bssid[18];      /* Target BSSID (empty = capture all on channel) */
    int64_t  start_ms;
    int64_t  duration_ms;           /* 30000, 60000, or 90000 */
    uint32_t frames_captured;
} lockon_state_t;

static lockon_state_t s_lockon = {0};

/* ── Diagnostic counters ──────────────────────────────────────────────────── */
static uint32_t s_total_frames = 0;
static uint32_t s_mgmt_frames = 0;
static uint32_t s_beacon_frames = 0;
static uint32_t s_fc_histogram[16] = {0};  /* subtype distribution */

/* ── Attack / anomaly counters (delta-reported, reset each status) ────────── */

static uint16_t s_deauth_count  = 0;      /* deauth frames since last status */
static uint16_t s_disassoc_count = 0;     /* disassoc frames since last status */
static uint16_t s_auth_count    = 0;      /* auth frames since last status */
static bool     s_deauth_flood  = false;  /* flood detected in current window */

/* Per-source deauth tracker — detect flood from single source */
#define DEAUTH_SRC_SLOTS    16
#define DEAUTH_FLOOD_THRESH  5            /* >5 deauth from one MAC in 10s = flood */
#define DEAUTH_WINDOW_MS     10000

typedef struct {
    uint8_t  mac[6];
    uint16_t count;
    int64_t  window_start_ms;
} deauth_src_t;

static deauth_src_t s_deauth_sources[DEAUTH_SRC_SLOTS] = {0};

/* ── Beacon spam tracker — detect Marauder/Flipper beacon floods ──────────── */

#define BEACON_SPAM_SLOTS    8
#define BEACON_SPAM_SSID_THRESH  10       /* >10 unique SSIDs from 1 BSSID in 30s */
#define BEACON_SPAM_WINDOW_MS    30000

typedef struct {
    uint8_t  bssid[6];
    uint16_t unique_ssid_count;
    uint32_t ssid_hashes[16];             /* FNV-1a of recent SSIDs */
    int64_t  window_start_ms;
} beacon_spam_tracker_t;

static beacon_spam_tracker_t s_beacon_spam[BEACON_SPAM_SLOTS] = {0};
static bool s_beacon_spam_active = false;

/* ── RSSI movement tracker (for soft-match confidence boost) ─────────────── */

#define RSSI_TRACK_SLOTS    16
#define RSSI_HISTORY_LEN     4       /* samples per BSSID */
#define RSSI_MOVE_THRESHOLD  6       /* dBm delta to count as movement */

typedef struct {
    uint8_t  bssid[6];
    bool     in_use;
    int8_t   rssi[RSSI_HISTORY_LEN];
    uint8_t  count;        /* total samples seen */
    uint8_t  idx;          /* ring buffer write index */
} rssi_track_t;

static rssi_track_t s_rssi_track[RSSI_TRACK_SLOTS];

/**
 * Record an RSSI sample for a BSSID and return true if the RSSI history
 * shows enough variance to indicate movement (likely a drone, not a router).
 */
/* Forward declaration — beacon_rate_limit_allow is defined further down in
 * the file but needs to be callable from process_beacon_frame above. */
static bool beacon_rate_limit_allow(const uint8_t *bssid, int8_t rssi, int64_t ts_ms);

static bool rssi_track_update(const uint8_t *bssid, int8_t rssi)
{
    /* Find existing or allocate */
    int free_idx = -1;
    rssi_track_t *slot = NULL;
    for (int i = 0; i < RSSI_TRACK_SLOTS; i++) {
        if (s_rssi_track[i].in_use &&
            memcmp(s_rssi_track[i].bssid, bssid, 6) == 0) {
            slot = &s_rssi_track[i];
            break;
        }
        if (!s_rssi_track[i].in_use && free_idx < 0) {
            free_idx = i;
        }
    }
    if (!slot) {
        if (free_idx < 0) free_idx = 0;  /* overwrite first slot */
        slot = &s_rssi_track[free_idx];
        memset(slot, 0, sizeof(*slot));
        memcpy(slot->bssid, bssid, 6);
        slot->in_use = true;
    }

    /* Record sample */
    slot->rssi[slot->idx] = rssi;
    slot->idx = (slot->idx + 1) % RSSI_HISTORY_LEN;
    if (slot->count < 255) slot->count++;

    /* Need at least 3 samples to judge movement */
    if (slot->count < 3) return false;

    /* Check max delta across history */
    int8_t lo = 0, hi = -127;
    int n = slot->count < RSSI_HISTORY_LEN ? slot->count : RSSI_HISTORY_LEN;
    lo = hi = slot->rssi[0];
    for (int i = 1; i < n; i++) {
        if (slot->rssi[i] < lo) lo = slot->rssi[i];
        if (slot->rssi[i] > hi) hi = slot->rssi[i];
    }
    return (hi - lo) >= RSSI_MOVE_THRESHOLD;
}

/* ── Helper: distance estimation from RSSI ─────────────────────────────────── */

/**
 * Estimate distance from RSSI using the log-distance path loss model.
 *
 *   d = 10^((RSSI_ref - RSSI) / (10 * n))
 *
 * @param rssi  Measured signal strength in dBm
 * @return Estimated distance in meters, clamped to [0.5, 5000.0]
 */
static double estimate_distance(int8_t rssi)
{
    double exponent = (double)(RSSI_REF - rssi) / (10.0 * PATH_LOSS_EXPONENT);
    double dist = pow(10.0, exponent);
    if (dist < 0.5)    dist = 0.5;
    if (dist > 5000.0)  dist = 5000.0;
    return dist;
}

/* ── Helper: format BSSID bytes to string ──────────────────────────────────── */

static void format_bssid(const uint8_t *mac, char *out, size_t out_len)
{
    snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ── Helper: get current time in milliseconds ──────────────────────────────── */

static int64_t now_ms(void)
{
    return (int64_t)(esp_timer_get_time() / 1000LL);
}

/* ── Helper: initialize a detection struct with common WiFi fields ─────────── */

static void init_detection(drone_detection_t *det, const uint8_t *bssid,
                           int8_t rssi, const char *ssid)
{
    memset(det, 0, sizeof(*det));
    det->rssi = rssi;
    det->estimated_distance_m = estimate_distance(rssi);

    format_bssid(bssid, det->bssid, sizeof(det->bssid));

    if (ssid) {
        strncpy(det->ssid, ssid, sizeof(det->ssid) - 1);
        det->ssid[sizeof(det->ssid) - 1] = '\0';
    }

    int64_t ts = now_ms();
    det->first_seen_ms = ts;
    det->last_updated_ms = ts;
}

/* ── Beacon frame parser ───────────────────────────────────────────────────── */

/**
 * Extract SSID and vendor IEs from an 802.11 beacon frame.
 *
 * Frame layout:
 *   [0-1]   Frame Control
 *   [2-3]   Duration
 *   [4-9]   Destination Address (broadcast)
 *   [10-15] Source Address
 *   [16-21] BSSID
 *   [22-23] Sequence Control
 *   [24-35] Fixed Parameters (Timestamp + Beacon Interval + Capability Info)
 *   [36+]   Tagged Parameters (IEs)
 */
static void process_beacon_frame(const uint8_t *frame, int frame_len,
                                 int8_t rssi)
{
    if (frame_len < BEACON_TAGGED_PARAMS_OFFSET) {
        return;
    }

    /* Extract source addr (frame[10..15]) and BSSID (frame[16..21]).
     * Pwnagotchi's beacon forwarding uses a hardcoded sender MAC of
     * DE:AD:BE:EF:DE:AD (Marauder-confirmed signature). When seen, emit a
     * high-confidence hostile-scanner detection so the backend can alert
     * on a pwnagotchi nearby — regardless of what SSID it's pretending
     * to broadcast. */
    const uint8_t *src  = &frame[10];
    const uint8_t *bssid = &frame[16];
    static const uint8_t PWNAGOTCHI_MAC[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD};
    if (memcmp(src, PWNAGOTCHI_MAC, 6) == 0 ||
        memcmp(bssid, PWNAGOTCHI_MAC, 6) == 0) {
        drone_detection_t det;
        init_detection(&det, bssid, rssi, "pwnagotchi");
        det.source = DETECTION_SRC_WIFI_OUI;
        det.confidence = 0.95f;
        strncpy(det.manufacturer, "Pwnagotchi", sizeof(det.manufacturer) - 1);
        strncpy(det.drone_id, "pwnagotchi", sizeof(det.drone_id) - 1);
        ESP_LOGW(TAG, "Pwnagotchi beacon seen RSSI=%d", rssi);
        if (s_detection_queue) {
            xQueueSend(s_detection_queue, &det, pdMS_TO_TICKS(10));
        }
        return;  /* no further classification needed */
    }

    /* ── Parse tagged parameters ──────────────────────────────────────────── */
    char ssid[33] = { 0 };
    int offset = BEACON_TAGGED_PARAMS_OFFSET;

    while (offset + 2 <= frame_len) {
        uint8_t tag_id  = frame[offset];
        uint8_t tag_len = frame[offset + 1];
        int tag_data_offset = offset + 2;

        if (tag_data_offset + tag_len > frame_len) {
            break; /* truncated IE */
        }

        if (tag_id == IE_TAG_SSID && tag_len > 0 && tag_len <= 32) {
            memcpy(ssid, &frame[tag_data_offset], tag_len);
            ssid[tag_len] = '\0';
        }

        offset = tag_data_offset + tag_len;
    }

    /* ── Beacon spam detection ────────────────────────────────────────────── */
    if (ssid[0] != '\0') {
        int64_t now_ms = esp_timer_get_time() / 1000;

        /* FNV-1a hash of SSID for compact tracking */
        uint32_t ssid_hash = 0x811c9dc5;
        for (const char *p = ssid; *p; p++) {
            ssid_hash ^= (uint8_t)*p;
            ssid_hash *= 0x01000193;
        }

        /* Find or allocate tracker for this BSSID */
        int bslot = -1;
        for (int i = 0; i < BEACON_SPAM_SLOTS; i++) {
            if (memcmp(s_beacon_spam[i].bssid, bssid, 6) == 0) {
                bslot = i;
                break;
            }
        }
        if (bslot < 0) {
            /* Find empty or oldest slot */
            int oldest = 0;
            for (int i = 0; i < BEACON_SPAM_SLOTS; i++) {
                if (s_beacon_spam[i].unique_ssid_count == 0) { oldest = i; break; }
                if (s_beacon_spam[i].window_start_ms <
                    s_beacon_spam[oldest].window_start_ms) {
                    oldest = i;
                }
            }
            bslot = oldest;
            memset(&s_beacon_spam[bslot], 0, sizeof(s_beacon_spam[bslot]));
            memcpy(s_beacon_spam[bslot].bssid, bssid, 6);
            s_beacon_spam[bslot].window_start_ms = now_ms;
        }

        beacon_spam_tracker_t *bst = &s_beacon_spam[bslot];

        /* Reset window if expired */
        if ((now_ms - bst->window_start_ms) > BEACON_SPAM_WINDOW_MS) {
            bst->unique_ssid_count = 0;
            bst->window_start_ms = now_ms;
        }

        /* Check if this SSID hash is new for this BSSID */
        bool ssid_seen = false;
        for (int i = 0; i < bst->unique_ssid_count && i < 16; i++) {
            if (bst->ssid_hashes[i] == ssid_hash) { ssid_seen = true; break; }
        }
        if (!ssid_seen && bst->unique_ssid_count < 16) {
            bst->ssid_hashes[bst->unique_ssid_count] = ssid_hash;
            bst->unique_ssid_count++;
        }
        if (bst->unique_ssid_count >= BEACON_SPAM_SSID_THRESH) {
            s_beacon_spam_active = true;
        }
    }

    /* ── Detection pipeline ───────────────────────────────────────────────── */
    drone_detection_t det;

    /* Priority 1: DJI vendor-specific IE (DroneID) */
    {
        dji_drone_id_data_t dji_data;
        if (dji_parse_beacon_frame(frame, (size_t)frame_len, &dji_data)) {
            init_detection(&det, bssid, rssi, ssid);
            det.source = DETECTION_SRC_WIFI_DJI_IE;
            det.confidence = 0.85f;
            det.latitude = dji_data.latitude;
            det.longitude = dji_data.longitude;
            det.altitude_m = dji_data.altitude_m;
            det.heading_deg = dji_data.heading_deg;
            det.speed_mps = dji_data.speed_mps;
            if (dji_data.has_home) {
                det.operator_lat = dji_data.home_lat;
                det.operator_lon = dji_data.home_lon;
            }
            strncpy(det.manufacturer, "DJI", sizeof(det.manufacturer) - 1);

            if (dji_data.serial_prefix[0] != '\0') {
                strncpy(det.drone_id, dji_data.serial_prefix,
                        sizeof(det.drone_id) - 1);
            } else if (ssid[0] != '\0') {
                snprintf(det.drone_id, sizeof(det.drone_id), "wifi_dji_%s", ssid);
            } else {
                snprintf(det.drone_id, sizeof(det.drone_id),
                         "wifi_dji_%02x%02x%02x%02x%02x%02x",
                         bssid[0], bssid[1], bssid[2],
                         bssid[3], bssid[4], bssid[5]);
            }

            ESP_LOGI(TAG, "DJI DroneID: BSSID=%s lat=%.6f lon=%.6f alt=%.0fm",
                     det.bssid, det.latitude, det.longitude, det.altitude_m);
            add_channel_heat(s_current_channel, 8);  /* DJI IE = high heat */

            if (s_detection_queue) {
                xQueueSend(s_detection_queue, &det, pdMS_TO_TICKS(10));
            }
            return;
        }
    }

    /* Priority 2: ASTM F3411 WiFi Beacon Remote ID */
    {
        char bssid_str[18];
        format_bssid(bssid, bssid_str, sizeof(bssid_str));

        odid_state_t rid_state;
        odid_state_init(&rid_state, bssid_str, now_ms());
        rid_state.rssi = rssi;

        if (wifi_beacon_rid_parse_frame(frame, (size_t)frame_len, &rid_state)) {
            drone_detection_t rid_det;
            if (odid_state_to_detection(&rid_state, "wfb_",
                                        DETECTION_SRC_WIFI_BEACON, &rid_det)) {
                /* Fill in WiFi-specific fields the ODID parser doesn't set */
                rid_det.rssi = rssi;
                rid_det.estimated_distance_m = estimate_distance(rssi);
                strncpy(rid_det.bssid, bssid_str, sizeof(rid_det.bssid) - 1);
                if (ssid[0] != '\0') {
                    strncpy(rid_det.ssid, ssid, sizeof(rid_det.ssid) - 1);
                }
                if (rid_det.confidence == 0.0f) {
                    rid_det.confidence = 0.90f;
                }

                int64_t ts = now_ms();
                rid_det.first_seen_ms = ts;
                rid_det.last_updated_ms = ts;

                ESP_LOGI(TAG, "WiFi Beacon RID: BSSID=%s id=%s",
                         rid_det.bssid, rid_det.drone_id);

                if (s_detection_queue) {
                    xQueueSend(s_detection_queue, &rid_det, pdMS_TO_TICKS(10));
                }
                return;
            }
        }
    }

    /* Priority 2.5: French "Signalement Electronique" DRI */
    {
        char bssid_str2[18];
        format_bssid(bssid, bssid_str2, sizeof(bssid_str2));

        odid_state_t fr_state;
        odid_state_init(&fr_state, bssid_str2, now_ms());
        fr_state.rssi = rssi;

        if (french_dri_parse_frame(frame, (size_t)frame_len, &fr_state)) {
            drone_detection_t fr_det;
            if (odid_state_to_detection(&fr_state, "fr_",
                                        DETECTION_SRC_WIFI_BEACON, &fr_det)) {
                fr_det.rssi = rssi;
                fr_det.estimated_distance_m = estimate_distance(rssi);
                strncpy(fr_det.bssid, bssid_str2, sizeof(fr_det.bssid) - 1);
                if (ssid[0] != '\0') {
                    strncpy(fr_det.ssid, ssid, sizeof(fr_det.ssid) - 1);
                }
                if (fr_det.confidence == 0.0f) {
                    fr_det.confidence = 0.90f;
                }

                int64_t ts = now_ms();
                fr_det.first_seen_ms = ts;
                fr_det.last_updated_ms = ts;

                ESP_LOGI(TAG, "French DRI: BSSID=%s id=%s",
                         fr_det.bssid, fr_det.drone_id);

                if (s_detection_queue) {
                    xQueueSend(s_detection_queue, &fr_det, pdMS_TO_TICKS(10));
                }
                return;
            }
        }
    }

    /* Log every SSID starting with F, and all unique SSIDs */
    static char seen_ssids[50][33];
    static int seen_count = 0;
    if (ssid[0] != '\0') {
        /* Always log F-starting SSIDs */
        if (ssid[0] == 'F' || ssid[0] == 'f') {
            ESP_LOGI(TAG, "F-SSID: \"%s\" RSSI=%d ch=%d", ssid, rssi, s_current_channel);
        }
        /* Log new unique SSIDs */
        bool already = false;
        for (int si = 0; si < seen_count; si++) {
            if (strcmp(seen_ssids[si], ssid) == 0) { already = true; break; }
        }
        if (!already && seen_count < 50) {
            strncpy(seen_ssids[seen_count], ssid, 32);
            seen_ssids[seen_count][32] = '\0';
            seen_count++;
            ESP_LOGI(TAG, "NEW[%d]: \"%s\" RSSI=%d ch=%d", seen_count, ssid, rssi, s_current_channel);
        }
    }

    int64_t beacon_ts = now_ms();

    /* Priority 3: SSID pattern match */
    if (ssid[0] != '\0') {
        const drone_ssid_pattern_t *pattern = wifi_ssid_match(ssid);
        if (pattern) {
            if (!beacon_rate_limit_allow(bssid, rssi, beacon_ts)) {
                return;  /* same BSSID reported recently with similar RSSI */
            }
            init_detection(&det, bssid, rssi, ssid);
            det.source = DETECTION_SRC_WIFI_SSID;
            det.confidence = 0.30f;

            strncpy(det.manufacturer, pattern->manufacturer,
                    sizeof(det.manufacturer) - 1);
            strncpy(det.drone_id, ssid, sizeof(det.drone_id) - 1);

            ESP_LOGI(TAG, "SSID match: \"%s\" (%s) RSSI=%d ~%.0fm",
                     ssid, pattern->manufacturer, rssi,
                     det.estimated_distance_m);

            if (s_detection_queue) {
                xQueueSend(s_detection_queue, &det, pdMS_TO_TICKS(10));
            }
            return;
        }
    }

    /* Priority 4: OUI prefix match (catches hidden/generic SSIDs) */
    {
        const oui_entry_t *oui = wifi_oui_lookup_raw(bssid);
        if (oui && !oui->high_false_positive) {
            if (!beacon_rate_limit_allow(bssid, rssi, beacon_ts)) {
                return;
            }
            init_detection(&det, bssid, rssi, ssid);
            det.source = DETECTION_SRC_WIFI_OUI;
            det.confidence = 0.40f;

            strncpy(det.manufacturer, oui->manufacturer,
                    sizeof(det.manufacturer) - 1);

            /* Use BSSID as drone_id since SSID may be hidden */
            format_bssid(bssid, det.drone_id, sizeof(det.drone_id));

            ESP_LOGI(TAG, "OUI match: BSSID=%s (%s) RSSI=%d ~%.0fm",
                     det.bssid, oui->manufacturer, rssi,
                     det.estimated_distance_m);

            if (s_detection_queue) {
                xQueueSend(s_detection_queue, &det, pdMS_TO_TICKS(10));
            }
            return;
        }
    }

    /* Priority 5: Soft SSID match — require a second corroborating signal
     * before emitting. A match on "WIFI_xxx" / "FPV_xxx" / "CAMERA_xxx"
     * alone is too weak; household routers and IoT gear match these
     * patterns. Only emit if the RSSI has moved recently (suggesting a
     * moving transmitter) — stationary devices with drone-like names are
     * dropped. Confidence floor raised to 0.25 minimum. */
    if (ssid[0] != '\0' && wifi_ssid_match_soft(ssid)) {
        bool moving = rssi_track_update(bssid, rssi);
        if (!moving) {
            /* Soft match without corroboration — drop silently. */
            return;
        }

        init_detection(&det, bssid, rssi, ssid);
        det.source = DETECTION_SRC_WIFI_SSID;
        det.confidence = 0.30f;

        strncpy(det.manufacturer, "Drone Likely", sizeof(det.manufacturer) - 1);
        strncpy(det.drone_id, ssid, sizeof(det.drone_id) - 1);

        ESP_LOGI(TAG, "Soft SSID (moving): \"%s\" RSSI=%d ~%.0fm conf=%.2f",
                 ssid, rssi, det.estimated_distance_m, det.confidence);

        if (s_detection_queue) {
            xQueueSend(s_detection_queue, &det, pdMS_TO_TICKS(10));
        }
        return;
    }
}

/* ── Beacon/scan-result rate-limit cache (per-BSSID) ───────────────────── */

/* S3-combo has plenty of internal SRAM — growing this from 128 to 1024 slots
 * reduces LRU wrap in dense RF environments (16 KB BSS, all internal: stays
 * out of PSRAM per policy rule 7 because this cache is iterated per packet).
 * Drone-protocol sources (BLE RID, DJI IE, Beacon RID) are exempt from this
 * rate limit anyway — see feedback_rid_top_priority.md. */
#if defined(SCANNER_S3_COMBO) || defined(UPLINK_ESP32S3)
#define BEACON_CACHE_SLOTS        1024
#else
#define BEACON_CACHE_SLOTS        128
#endif
#define BEACON_RATE_LIMIT_MS      30000   /* re-report each BSSID at most every 30s */
#define BEACON_RSSI_DELTA_DB      5       /* unless RSSI shifted by >= 5 dB */

typedef struct {
    uint8_t  bssid[6];
    int8_t   last_rssi;
    int64_t  last_seen_ms;
} beacon_cache_entry_t;

static beacon_cache_entry_t s_beacon_cache[BEACON_CACHE_SLOTS];
static int s_beacon_cache_idx = 0;  /* circular write index */

/**
 * Rate-limit repeated beacon/scan-result reports for the same BSSID.
 * Returns true when the caller may emit a detection; false if the BSSID
 * was seen <30 s ago with a similar RSSI and should be suppressed.
 * The caller is expected to have already classified the frame as a hard
 * match (SSID/OUI) — we intentionally do NOT apply this to drone-protocol
 * sources (DJI IE, Beacon RID) because those are precious.
 */
static bool beacon_rate_limit_allow(const uint8_t *bssid, int8_t rssi, int64_t ts_ms)
{
    for (int i = 0; i < BEACON_CACHE_SLOTS; i++) {
        if (memcmp(s_beacon_cache[i].bssid, bssid, 6) == 0) {
            int64_t age = ts_ms - s_beacon_cache[i].last_seen_ms;
            int delta = (int)rssi - (int)s_beacon_cache[i].last_rssi;
            if (delta < 0) delta = -delta;
            if (age < BEACON_RATE_LIMIT_MS && delta < BEACON_RSSI_DELTA_DB) {
                return false;
            }
            s_beacon_cache[i].last_seen_ms = ts_ms;
            s_beacon_cache[i].last_rssi = rssi;
            return true;
        }
    }
    beacon_cache_entry_t *slot = &s_beacon_cache[s_beacon_cache_idx];
    memcpy(slot->bssid, bssid, 6);
    slot->last_rssi = rssi;
    slot->last_seen_ms = ts_ms;
    s_beacon_cache_idx = (s_beacon_cache_idx + 1) % BEACON_CACHE_SLOTS;
    return true;
}

/* ── Probe request rate-limit cache ───────────────────────────────────────── */

/* Bumped from 16 to 128 on S3 — crowded networks have far more than 16
 * concurrent probers, causing the LRU to overwrite before entries expire. */
#if defined(SCANNER_S3_COMBO) || defined(UPLINK_ESP32S3)
#define PROBE_CACHE_SLOTS       128
#else
#define PROBE_CACHE_SLOTS       16
#endif
#define PROBE_RATE_LIMIT_MS     5000    /* 1 detection per MAC+SSID pair per 5s */

typedef struct {
    uint8_t  mac[6];
    char     ssid[33];
    int64_t  last_seen_ms;
} probe_cache_entry_t;

static probe_cache_entry_t s_probe_cache[PROBE_CACHE_SLOTS];
static int s_probe_cache_idx = 0;  /* circular write index */

/**
 * Check if a probe from this MAC+SSID was recently seen (within 5s).
 * If not, record it and return true (allow). If yes, return false (suppress).
 */
static bool probe_rate_limit_allow(const uint8_t *mac, const char *ssid, int64_t ts_ms)
{
    /* Search for existing entry */
    for (int i = 0; i < PROBE_CACHE_SLOTS; i++) {
        if (memcmp(s_probe_cache[i].mac, mac, 6) == 0 &&
            strcmp(s_probe_cache[i].ssid, ssid) == 0) {
            if ((ts_ms - s_probe_cache[i].last_seen_ms) < PROBE_RATE_LIMIT_MS) {
                return false;  /* rate-limited */
            }
            /* Expired — update timestamp and allow */
            s_probe_cache[i].last_seen_ms = ts_ms;
            return true;
        }
    }

    /* Not found — insert at circular index */
    probe_cache_entry_t *slot = &s_probe_cache[s_probe_cache_idx];
    memcpy(slot->mac, mac, 6);
    strncpy(slot->ssid, ssid, sizeof(slot->ssid) - 1);
    slot->ssid[sizeof(slot->ssid) - 1] = '\0';
    slot->last_seen_ms = ts_ms;
    s_probe_cache_idx = (s_probe_cache_idx + 1) % PROBE_CACHE_SLOTS;
    return true;
}

/* ── Probe request frame parser ──────────────────────────────────────────── */

/**
 * Process a WiFi probe request frame.
 *
 * Probe request layout (NO 12-byte fixed params unlike beacons):
 *   [0-1]   Frame Control (0x40 = probe request)
 *   [2-3]   Duration
 *   [4-9]   Destination Address (broadcast)
 *   [10-15] Source Address (transmitter = probing device)
 *   [16-21] BSSID (broadcast or target)
 *   [22-23] Sequence Control
 *   [24+]   Tagged Parameters (IEs) — SSID is typically the first IE
 */
static void process_probe_request(const uint8_t *frame, int frame_len,
                                  int8_t rssi, uint16_t channel)
{
    /* Minimum: 24-byte header + at least 2-byte IE header */
    if (frame_len < 26) {
        return;
    }

    /* Source MAC (transmitter address) at bytes 10-15 */
    const uint8_t *src_mac = &frame[10];

    /* Parse ALL Information Elements — extract SSID, capabilities, and fingerprint */
    char ssid[33] = {0};
    char probed_ssids[128] = {0};
    int probed_pos = 0;
    uint32_t ie_hash = 0x811c9dc5;  /* FNV1a offset basis */
    uint8_t wifi_gen = 0;           /* 0=legacy, 4=n, 5=ac, 6=ax */
    int offset = 24;

    while (offset + 2 <= frame_len) {
        uint8_t tag_id  = frame[offset];
        uint8_t tag_len = frame[offset + 1];
        int tag_data_offset = offset + 2;

        if (tag_data_offset + tag_len > frame_len) break;

        /* Build IE fingerprint hash (FNV1a of ordered IE type+length sequence) */
        ie_hash ^= (uint32_t)tag_id;
        ie_hash *= 0x01000193;
        ie_hash ^= (uint32_t)tag_len;
        ie_hash *= 0x01000193;

        /* Identity-stable IE payload bytes fed into the hash (per PETS-2017).
         * These fields survive random-MAC rotation: capability bits are tied
         * to the chipset/driver, not the virtual MAC. */
        bool hash_payload = false;
        int payload_bytes = 0;
        switch (tag_id) {
            case 1:   hash_payload = true; payload_bytes = (tag_len > 8) ? 8 : tag_len; break;  /* Supported Rates */
            case 45:  hash_payload = true; payload_bytes = (tag_len > 26) ? 26 : tag_len; break; /* HT Cap */
            case 127: hash_payload = true; payload_bytes = (tag_len > 8) ? 8 : tag_len; break;  /* Ext Cap */
            case 191: hash_payload = true; payload_bytes = (tag_len > 12) ? 12 : tag_len; break; /* VHT Cap */
            default: break;
        }
        if (hash_payload) {
            for (int i = 0; i < payload_bytes; i++) {
                ie_hash ^= (uint32_t)frame[tag_data_offset + i];
                ie_hash *= 0x01000193;
            }
        }

        switch (tag_id) {
            case 0:  /* SSID */
                if (tag_len > 0 && tag_len <= 32) {
                    memcpy(ssid, &frame[tag_data_offset], tag_len);
                    ssid[tag_len] = '\0';
                    /* Accumulate probed SSIDs list */
                    if (probed_pos > 0 && probed_pos < (int)sizeof(probed_ssids) - 2) {
                        probed_ssids[probed_pos++] = ',';
                    }
                    int copy_len = tag_len;
                    if (probed_pos + copy_len >= (int)sizeof(probed_ssids) - 1) {
                        copy_len = sizeof(probed_ssids) - 1 - probed_pos;
                    }
                    if (copy_len > 0) {
                        memcpy(probed_ssids + probed_pos, ssid, copy_len);
                        probed_pos += copy_len;
                    }
                }
                break;
            case 45:  /* HT Capabilities (802.11n) */
                if (wifi_gen < 4) wifi_gen = 4;
                break;
            case 191: /* VHT Capabilities (802.11ac) */
                if (wifi_gen < 5) wifi_gen = 5;
                break;
            case 255: /* Extension — check for HE (802.11ax) */
                if (tag_len >= 1 && frame[tag_data_offset] == 35) {
                    wifi_gen = 6;  /* HE Capabilities */
                }
                break;
            case 221: /* Vendor Specific — hash OUI + first 4 body bytes */
                if (tag_len >= 3) {
                    ie_hash ^= ((uint32_t)frame[tag_data_offset] << 16) |
                               ((uint32_t)frame[tag_data_offset+1] << 8) |
                                (uint32_t)frame[tag_data_offset+2];
                    ie_hash *= 0x01000193;
                    int body = tag_len - 3;
                    if (body > 4) body = 4;
                    for (int i = 0; i < body; i++) {
                        ie_hash ^= (uint32_t)frame[tag_data_offset + 3 + i];
                        ie_hash *= 0x01000193;
                    }
                }
                break;
        }

        offset = tag_data_offset + tag_len;
    }
    probed_ssids[probed_pos] = '\0';

    /* Drop broadcast probes — they flood UART/queue/heap for zero value.
     * Every phone sends these constantly. Only targeted probes (with SSID) matter. */
    bool is_broadcast = (ssid[0] == '\0');
    if (is_broadcast) {
        return;
    }

    /* Rate-limit: 1 per MAC+SSID pair per 5 seconds */
    int64_t ts = now_ms();
    if (!probe_rate_limit_allow(src_mac, ssid, ts)) {
        return;
    }

    /* Check SSID against drone patterns */
    const drone_ssid_pattern_t *pattern = NULL;
    bool soft = false;
    if (!is_broadcast) {
        pattern = wifi_ssid_match(ssid);
        soft = (!pattern && wifi_ssid_match_soft(ssid));
    }

    float conf;
    const char *mfr;
    if (pattern) {
        conf = 0.50f;
        mfr = pattern->manufacturer;
    } else {
        /* Probe requests are what a CLIENT is searching for, not what a
         * drone is broadcasting. Even if the SSID looks drone-like, a
         * phone asking for "WIFI_FPV" isn't itself a drone. Treat all
         * non-hard-match probes as generic wifi_device (handled by the
         * backend classifier) — no "Drone Likely" soft tag. */
        (void)soft;
        conf = 0.05f;
        mfr = "Unknown";
    }

    /* Build detection with full probe fingerprint */
    drone_detection_t det;
    init_detection(&det, src_mac, rssi, is_broadcast ? "(broadcast)" : ssid);
    det.source = DETECTION_SRC_WIFI_PROBE_REQUEST;
    det.confidence = conf;
    det.freq_mhz = (channel <= 13) ? (2407 + channel * 5) : (5000 + channel * 5);
    det.probe_ie_hash = ie_hash;
    det.wifi_generation = wifi_gen;
    strncpy(det.probed_ssids, probed_ssids, sizeof(det.probed_ssids) - 1);

    strncpy(det.manufacturer, mfr, sizeof(det.manufacturer) - 1);

    /* drone_id format: "probe_XX:XX:XX:XX:XX:XX" */
    snprintf(det.drone_id, sizeof(det.drone_id),
             "probe_%02X:%02X:%02X:%02X:%02X:%02X",
             src_mac[0], src_mac[1], src_mac[2],
             src_mac[3], src_mac[4], src_mac[5]);

    if (pattern) {
        add_channel_heat(channel, 4);  /* Drone SSID probe = medium heat */
    } else if (soft) {
        add_channel_heat(channel, 2);  /* Soft match probe = low heat */
    }

    ESP_LOGI(TAG, "Probe req: MAC=%s SSID=\"%s\" RSSI=%d conf=%.2f",
             det.bssid, ssid, rssi, conf);

    if (s_detection_queue) {
        xQueueSend(s_detection_queue, &det, pdMS_TO_TICKS(10));
    }
}

/* ── Promiscuous mode callback ─────────────────────────────────────────────── */

/**
 * Called by the WiFi driver for every received frame in promiscuous mode.
 * We filter for management frames (type 0x00) with beacon subtype (0x80).
 */
static void wifi_promiscuous_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    s_total_frames++;

    if (type != WIFI_PKT_MGMT) {
        return;
    }
    s_mgmt_frames++;

    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *frame = pkt->payload;
    int frame_len = pkt->rx_ctrl.sig_len;
    int8_t rssi = pkt->rx_ctrl.rssi;

    /* Minimum frame: Frame Control (2 bytes) */
    if (frame_len < 2) {
        return;
    }

    /*
     * Frame Control field (little-endian):
     *   Bits [1:0]  = Protocol version (0)
     *   Bits [3:2]  = Type: 0 = Management
     *   Bits [7:4]  = Subtype: 8 = Beacon (0x80 when combined with type)
     *
     * frame[0] for beacon = 0x80 (subtype=1000, type=00, proto=00)
     */
    uint8_t frame_ctrl = frame[0];
    /* Track management subtype distribution (bits 7:4) */
    uint8_t subtype = (frame_ctrl >> 4) & 0x0F;
    if (subtype < 16) {
        s_fc_histogram[subtype]++;
    }

    /* ── Deauth / disassoc / auth frame counting ─────────────────────────── */
    if (frame_ctrl == 0xC0 /* deauth */ || frame_ctrl == 0xA0 /* disassoc */) {
        if (frame_ctrl == 0xC0) {
            s_deauth_count++;
        } else {
            s_disassoc_count++;
        }
        /* Track per-source flood: src MAC at offset 10 in mgmt frame header */
        if (frame_len >= 16) {
            const uint8_t *src_mac = &frame[10];
            int64_t now_ms = esp_timer_get_time() / 1000;
            int slot = -1;
            for (int i = 0; i < DEAUTH_SRC_SLOTS; i++) {
                if (memcmp(s_deauth_sources[i].mac, src_mac, 6) == 0) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) {
                /* Find empty or oldest slot */
                int oldest = 0;
                for (int i = 0; i < DEAUTH_SRC_SLOTS; i++) {
                    if (s_deauth_sources[i].count == 0) { oldest = i; break; }
                    if (s_deauth_sources[i].window_start_ms <
                        s_deauth_sources[oldest].window_start_ms) {
                        oldest = i;
                    }
                }
                slot = oldest;
                memcpy(s_deauth_sources[slot].mac, src_mac, 6);
                s_deauth_sources[slot].count = 0;
                s_deauth_sources[slot].window_start_ms = now_ms;
            }
            /* Reset window if expired */
            if ((now_ms - s_deauth_sources[slot].window_start_ms) > DEAUTH_WINDOW_MS) {
                s_deauth_sources[slot].count = 0;
                s_deauth_sources[slot].window_start_ms = now_ms;
            }
            s_deauth_sources[slot].count++;
            if (s_deauth_sources[slot].count > DEAUTH_FLOOD_THRESH) {
                s_deauth_flood = true;
            }
        }
        return;  /* Don't process deauth/disassoc as beacon */
    }
    if (frame_ctrl == 0xB0 /* authentication */) {
        s_auth_count++;
        return;
    }

    /* Process beacons, probe responses, AND probe requests.
     * Probe requests (0x40) catch drone controllers searching for their drone's SSID.
     * Even when a drone isn't broadcasting an AP, its controller probes for it. */
    if (frame_ctrl != WIFI_FC_SUBTYPE_BEACON &&
        frame_ctrl != 0x50 /* probe response */ &&
        frame_ctrl != 0x40 /* probe request */) {
        return;
    }
    s_beacon_frames++;

    if (frame_ctrl == 0x40) {
        /* Probe requests have NO 12-byte fixed params -- use dedicated parser */
        process_probe_request(frame, frame_len, rssi, s_current_channel);
    } else {
        /* Beacons (0x80) and probe responses (0x50) share the same frame layout.
         * Probe responses are replies to a client's search — not an actively
         * broadcasting AP. Pass the subtype so we can differentiate. */
        process_beacon_frame(frame, frame_len, rssi);
    }
}

/* ── Affine channel hopping (sensor diversity) ─────────────────────────────── */

/*
 * Deterministic channel schedule: channel_index = (hop * STRIDE + phase) % 13
 * With stride=5 (coprime to 13), each sensor visits all 13 channels before repeat.
 * Different sensors use different phase offsets so they're always on different channels.
 */
#define AFFINE_STRIDE       5
static uint8_t  s_device_phase = 0;   /* Set from device MAC at init */
static uint32_t s_hop_counter = 0;

/* Per-channel heat tracking for adaptive dwell (packed nibbles — 7 bytes) */
static uint8_t  s_channel_heat[13] = {0};

static void add_channel_heat(uint16_t ch, uint8_t points)
{
    if (ch >= 1 && ch <= 13) {
        uint8_t cur = s_channel_heat[ch - 1];
        uint8_t nv = cur + points;
        s_channel_heat[ch - 1] = (nv > 15) ? 15 : nv;  /* Cap at 15 (4-bit) */
    }
}

static void decay_channel_heat(void)
{
    for (int i = 0; i < 13; i++) {
        if (s_channel_heat[i] > 0) s_channel_heat[i]--;
    }
}

static uint16_t get_adaptive_dwell_ms(uint16_t ch)
{
    if (ch < 1 || ch > 13) return CHANNEL_DWELL_MS;
    uint8_t heat = s_channel_heat[ch - 1];
    /* Base 90ms + 12ms per heat point, max 200ms */
    uint16_t dwell = 90 + (heat * 12);
    return (dwell > 200) ? 200 : dwell;
}

/* ── Channel advance helpers ────────────────────────────────────────────────── */

static uint16_t next_channel_24ghz(void)
{
    /* Affine hop: deterministic, coprime stride ensures full coverage */
    uint16_t idx = (s_hop_counter * AFFINE_STRIDE + s_device_phase) % NUM_CHANNELS_24GHZ;
    s_hop_counter++;
    return s_channels_24ghz[idx];
}

#ifdef CONFIG_SCANNER_5GHZ_ENABLED
static uint16_t next_channel_5ghz(void)
{
    uint16_t ch = s_channels_5ghz[s_idx_5ghz];
    s_idx_5ghz = (s_idx_5ghz + 1) % NUM_CHANNELS_5GHZ;
    return ch;
}
#endif

/**
 * Pick the next channel to scan.
 *
 * Single-band (S3): sequential 2.4 GHz sweep.
 * Dual-band (C5):   interleaved — alternates between 2.4 and 5 GHz so
 *                    neither band is starved.
 */
static uint16_t advance_channel(void)
{
#ifdef CONFIG_SCANNER_5GHZ_ENABLED
    uint16_t ch;
    if (s_next_is_5ghz) {
        ch = next_channel_5ghz();
    } else {
        ch = next_channel_24ghz();
    }
    s_next_is_5ghz = !s_next_is_5ghz;
    return ch;
#else
    return next_channel_24ghz();
#endif
}

/* ── WiFi scan task (active scan + promiscuous hybrid) ──────────────────────── */

/*
 * The ESP32-C5's promiscuous mode has poor frame capture after channel switches.
 * Hybrid approach: run periodic active scans (esp_wifi_scan_start) to reliably
 * discover all APs, then feed matching SSIDs into the detection pipeline.
 * Promiscuous mode still runs in parallel for DJI IEs and beacon RID.
 */

/* ── Scan configuration ─────────────────────────────────────────────────────── */

#define FULL_SCAN_INTERVAL_MS    30000  /* Full discovery every 30s (was 1.5s — too aggressive) */
#define FAST_RESCAN_INTERVAL_MS  100    /* Hot channel rescan ~10 Hz (was 150ms) */
#define MAX_AP_RECORDS           96     /* Capture more APs per scan (was 64) */
#define MAX_HOT_CHANNELS         5      /* Track more hot channels (was 3) */
#define HOT_CHANNEL_TTL_MS       6000   /* Keep hot channels longer (was 4s) */

/* Hot channel tracking — channels where interesting targets were last seen */
typedef struct {
    uint16_t channel;
    int64_t  last_seen_ms;
    uint16_t hits;
} hot_channel_t;

static hot_channel_t s_hot_channels[MAX_HOT_CHANNELS];
static int      s_hot_channel_count = 0;

static void prune_hot_channels(int64_t ts_ms)
{
    int write_idx = 0;
    for (int i = 0; i < s_hot_channel_count; i++) {
        if ((ts_ms - s_hot_channels[i].last_seen_ms) <= HOT_CHANNEL_TTL_MS) {
            if (write_idx != i) {
                s_hot_channels[write_idx] = s_hot_channels[i];
            }
            write_idx++;
        }
    }
    s_hot_channel_count = write_idx;
}

static void update_hot_channel(uint16_t ch)
{
    int64_t ts_ms = now_ms();

    /* Check if already tracked */
    for (int i = 0; i < s_hot_channel_count; i++) {
        if (s_hot_channels[i].channel == ch) {
            hot_channel_t slot = s_hot_channels[i];
            slot.last_seen_ms = ts_ms;
            if (slot.hits < UINT16_MAX) {
                slot.hits++;
            }

            /* Move most-recent channels to the end for rescans. */
            for (int j = i; j < s_hot_channel_count - 1; j++) {
                s_hot_channels[j] = s_hot_channels[j + 1];
            }
            s_hot_channels[s_hot_channel_count - 1] = slot;
            return;
        }
    }

    prune_hot_channels(ts_ms);

    hot_channel_t slot = {
        .channel = ch,
        .last_seen_ms = ts_ms,
        .hits = 1,
    };

    /* Add or replace oldest */
    if (s_hot_channel_count < MAX_HOT_CHANNELS) {
        s_hot_channels[s_hot_channel_count++] = slot;
    } else {
        /* Shift and add at end */
        for (int i = 0; i < MAX_HOT_CHANNELS - 1; i++) {
            s_hot_channels[i] = s_hot_channels[i + 1];
        }
        s_hot_channels[MAX_HOT_CHANNELS - 1] = slot;
    }
}

/* ── Process scan results ──────────────────────────────────────────────────── */

static void process_scan_results(void)
{
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) return;
    if (ap_count > MAX_AP_RECORDS) ap_count = MAX_AP_RECORDS;

    wifi_ap_record_t *ap_list = malloc(ap_count * sizeof(wifi_ap_record_t));
    if (!ap_list) return;

    esp_wifi_scan_get_ap_records(&ap_count, ap_list);
    s_beacon_frames += ap_count;
    s_total_frames += ap_count;

    int64_t scan_ts = now_ms();

    for (int i = 0; i < ap_count; i++) {
        const char *ssid = (const char *)ap_list[i].ssid;
        int8_t rssi = ap_list[i].rssi;
        uint8_t *bssid = ap_list[i].bssid;
        uint16_t ch = ap_list[i].primary;

        if (ssid[0] == '\0') continue;

        /* Classify: drone pattern match, soft match, or unmatched */
        const drone_ssid_pattern_t *pattern = wifi_ssid_match(ssid);
        bool soft = (!pattern && wifi_ssid_match_soft(ssid));

        /* Rate-limit repeated BSSIDs (hard or soft match). Unmatched SSIDs
         * fall through as low-confidence wifi_oui and are also subject to
         * the rate limit so background APs don't flood the stream. */
        if (!beacon_rate_limit_allow(bssid, rssi, scan_ts)) {
            continue;
        }

        /* Build detection for every AP — backend handles filtering */
        drone_detection_t det;
        init_detection(&det, bssid, rssi, ssid);
        det.freq_mhz = (ch <= 13) ? (2407 + ch * 5) : (5000 + ch * 5);

        if (pattern) {
            det.source = DETECTION_SRC_WIFI_SSID;
            det.confidence = 0.30f;
            strncpy(det.manufacturer, pattern->manufacturer,
                    sizeof(det.manufacturer) - 1);
            strncpy(det.drone_id, ssid, sizeof(det.drone_id) - 1);
            update_hot_channel(ch);
            add_channel_heat(ch, 4);  /* SSID match = medium heat */
        } else if (soft && rssi_track_update(bssid, rssi)) {
            /* Soft match only admitted when RSSI has moved — static APs
             * with drone-like names are dropped. */
            det.source = DETECTION_SRC_WIFI_SSID;
            det.confidence = 0.25f;
            strncpy(det.manufacturer, "Drone Likely", sizeof(det.manufacturer) - 1);
            strncpy(det.drone_id, ssid, sizeof(det.drone_id) - 1);
            update_hot_channel(ch);
            add_channel_heat(ch, 2);  /* Soft match = low heat */
        } else if (soft) {
            /* Soft match without movement — skip entirely. */
            continue;
        } else {
            det.source = DETECTION_SRC_WIFI_OUI;
            det.confidence = 0.05f;
            strncpy(det.drone_id, ssid, sizeof(det.drone_id) - 1);
        }

        if (s_detection_queue) {
            xQueueSend(s_detection_queue, &det, pdMS_TO_TICKS(5));
        }
    }

    free(ap_list);
}

/* ── Full discovery scan (all channels) ────────────────────────────────────── */

static void do_full_scan(void)
{
    wifi_scan_config_t cfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,           /* all channels */
        .show_hidden = true,
        .scan_type   = WIFI_SCAN_TYPE_PASSIVE,  /* Passive = listen only, no probe TX */
        .scan_time.passive = 100,               /* 100ms per channel passive dwell */
    };

    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Full scan failed: %s", esp_err_to_name(err));
        return;
    }
    process_scan_results();
}

/* ── Fast targeted rescan (hot channels only) ──────────────────────────────── */

static void do_fast_rescan(void)
{
    prune_hot_channels(now_ms());
    if (s_hot_channel_count == 0) return;

    for (int i = 0; i < s_hot_channel_count; i++) {
        wifi_scan_config_t cfg = {
            .ssid        = NULL,
            .bssid       = NULL,
            .channel     = s_hot_channels[i].channel,  /* single hot channel */
            .show_hidden = true,
            .scan_type   = WIFI_SCAN_TYPE_PASSIVE,  /* Passive = listen only */
            .scan_time.passive = 50,                /* 50ms per hot channel */
        };

        esp_err_t err = esp_wifi_scan_start(&cfg, true);
        if (err != ESP_OK) continue;
        process_scan_results();
    }
}

/* ── WiFi scan task ────────────────────────────────────────────────────────── */

static void wifi_scan_task(void *arg)
{
    ESP_LOGI(TAG, "WiFi scan task started on core %d", xPortGetCoreID());
    ESP_LOGI(TAG, "Adaptive scan: full every %dms, fast rescan every %dms on hot channels",
             FULL_SCAN_INTERVAL_MS, FAST_RESCAN_INTERVAL_MS);

    TickType_t last_full_scan = 0;
    TickType_t last_heartbeat = xTaskGetTickCount();

    while (1) {
        TickType_t now = xTaskGetTickCount();

        /* Lock-on mode: stay on target channel, no hopping */
        if (wifi_scanner_is_locked_on()) {
            /* Just dwell — promiscuous callback handles frame capture */
            s_lockon.frames_captured += s_beacon_frames;  /* Approximate */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Full discovery scan periodically */
        if ((now - last_full_scan) >= pdMS_TO_TICKS(FULL_SCAN_INTERVAL_MS)) {
            do_full_scan();
            last_full_scan = now;
            decay_channel_heat();  /* Decay heat once per full scan cycle */
        } else if (s_hot_channel_count > 0) {
            /* Fast rescan hot channels between full scans */
            do_fast_rescan();
        }

        /* Heartbeat */
        if ((now - last_heartbeat) >= pdMS_TO_TICKS(10000)) {
            last_heartbeat = now;
            ESP_LOGI(TAG, "scan: tot=%lu bcn=%lu hot_ch=%d",
                     (unsigned long)s_total_frames,
                     (unsigned long)s_beacon_frames,
                     s_hot_channel_count);
        }

        vTaskDelay(pdMS_TO_TICKS(FAST_RESCAN_INTERVAL_MS));
    }
}

/* ── Public API ────────────────────────────────────────────────────────────── */

void wifi_scanner_init(QueueHandle_t detection_queue)
{
    s_detection_queue = detection_queue;

    /* Set device phase from MAC address for affine channel hopping diversity.
     * Each sensor gets a different phase so they scan different channels. */
    {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        s_device_phase = (mac[4] ^ mac[5]) % NUM_CHANNELS_24GHZ;
        ESP_LOGI(TAG, "Affine hop phase=%d (from MAC ...%02X:%02X)",
                 s_device_phase, mac[4], mac[5]);
    }

    /* Initialize WiFi in STA mode (needed for active scan + promiscuous) */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Capture management frames (beacons, probe responses contain drone IEs) */
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_promiscuous_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    /* Start on channel 1 */
    s_current_channel = 1;
    s_idx_24ghz = 0;
#ifdef CONFIG_SCANNER_5GHZ_ENABLED
    s_idx_5ghz = 0;
    s_next_is_5ghz = false;
#endif
    ESP_ERROR_CHECK(esp_wifi_set_channel(s_current_channel, WIFI_SECOND_CHAN_NONE));

    ESP_LOGI(TAG, "WiFi promiscuous scanner initialized");
}

void wifi_scanner_start(void)
{
    xTaskCreatePinnedToCore(
        wifi_scan_task,
        "wifi_scan",
        WIFI_SCAN_TASK_STACK_SIZE,
        NULL,
        WIFI_SCAN_TASK_PRIORITY,
        NULL,
        WIFI_SCAN_TASK_CORE
    );

    ESP_LOGI(TAG, "WiFi scan task created (core=%d, pri=%d, stack=%d)",
             WIFI_SCAN_TASK_CORE, WIFI_SCAN_TASK_PRIORITY,
             WIFI_SCAN_TASK_STACK_SIZE);
}

void wifi_scanner_set_channel(uint16_t channel)
{
    /* Accept any valid 2.4 GHz or 5 GHz channel */
    bool valid = (channel >= 1 && channel <= 13);
#ifdef CONFIG_SCANNER_5GHZ_ENABLED
    valid = valid || (channel >= 36 && channel <= 165);
#endif
    if (valid) {
        s_current_channel = channel;
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    }
}

uint16_t wifi_scanner_get_channel(void)
{
    return s_current_channel;
}

bool wifi_scanner_is_5ghz_enabled(void)
{
#ifdef CONFIG_SCANNER_5GHZ_ENABLED
    return true;
#else
    return false;
#endif
}

/* ── Lock-on implementation ──────────────────────────────────────────────── */

bool wifi_scanner_lockon(uint8_t channel, const char *bssid, int duration_s)
{
    if (channel < 1 || channel > 13) return false;
    if (duration_s != 30 && duration_s != 60 && duration_s != 90) duration_s = 60;

    s_lockon.active = true;
    s_lockon.channel = channel;
    s_lockon.start_ms = now_ms();
    s_lockon.duration_ms = (int64_t)duration_s * 1000;
    s_lockon.frames_captured = 0;

    if (bssid && bssid[0]) {
        strncpy(s_lockon.target_bssid, bssid, sizeof(s_lockon.target_bssid) - 1);
    } else {
        s_lockon.target_bssid[0] = '\0';
    }

    /* Immediately switch to target channel */
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    s_current_channel = channel;

    ESP_LOGW(TAG, "LOCK-ON: ch=%d bssid=%s duration=%ds",
             channel, s_lockon.target_bssid[0] ? s_lockon.target_bssid : "*",
             duration_s);
    return true;
}

void wifi_scanner_lockon_cancel(void)
{
    if (s_lockon.active) {
        ESP_LOGI(TAG, "LOCK-ON cancelled after %lu frames",
                 (unsigned long)s_lockon.frames_captured);
        s_lockon.active = false;
    }
}

bool wifi_scanner_is_locked_on(void)
{
    if (!s_lockon.active) return false;

    /* Auto-expire */
    if ((now_ms() - s_lockon.start_ms) >= s_lockon.duration_ms) {
        ESP_LOGI(TAG, "LOCK-ON expired after %lu frames in %llds",
                 (unsigned long)s_lockon.frames_captured,
                 (long long)(s_lockon.duration_ms / 1000));
        s_lockon.active = false;
        return false;
    }
    return true;
}

/* ── Attack / anomaly counter API ──────────────────────────────────────────── */

void wifi_scanner_get_attack_counters(uint16_t *deauth, uint16_t *disassoc,
                                       uint16_t *auth, bool *flood,
                                       bool *bcn_spam)
{
    if (deauth)    *deauth    = s_deauth_count;
    if (disassoc)  *disassoc  = s_disassoc_count;
    if (auth)      *auth      = s_auth_count;
    if (flood)     *flood     = s_deauth_flood;
    if (bcn_spam)  *bcn_spam  = s_beacon_spam_active;
}

void wifi_scanner_reset_attack_counters(void)
{
    s_deauth_count  = 0;
    s_disassoc_count = 0;
    s_auth_count    = 0;
    s_deauth_flood  = false;
    s_beacon_spam_active = false;
    for (int i = 0; i < DEAUTH_SRC_SLOTS; i++) {
        s_deauth_sources[i].count = 0;
    }
}

void wifi_scanner_get_fc_histogram(uint32_t out[16])
{
    memcpy(out, s_fc_histogram, sizeof(s_fc_histogram));
}

void wifi_scanner_reset_fc_histogram(void)
{
    memset(s_fc_histogram, 0, sizeof(s_fc_histogram));
}

void wifi_scanner_pause(void)
{
    esp_wifi_set_promiscuous(false);
    ESP_LOGW(TAG, "WiFi scanning PAUSED (OTA in progress)");
}

void wifi_scanner_resume(void)
{
    esp_wifi_set_promiscuous(true);
    ESP_LOGW(TAG, "WiFi scanning RESUMED");
}
