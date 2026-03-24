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

#include "esp_wifi.h"
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
#define CHANNEL_DWELL_MS            200   /* Dual-band: faster sweep needed */
#else
#define CHANNEL_DWELL_MS            100
#endif

/* ── Channel tables ────────────────────────────────────────────────────────── */

static const uint16_t s_channels_24ghz[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13
};
#define NUM_CHANNELS_24GHZ  (sizeof(s_channels_24ghz) / sizeof(s_channels_24ghz[0]))

#ifdef CONFIG_SCANNER_5GHZ_ENABLED
static const uint16_t s_channels_5ghz[] = {
    36,  40,  44,  48,                                     /* UNII-1 */
    52,  56,  60,  64,                                     /* UNII-2 */
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, /* UNII-2 Ext */
    149, 153, 157, 161, 165                                /* UNII-3 */
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

/* ── Diagnostic counters ──────────────────────────────────────────────────── */
static uint32_t s_total_frames = 0;
static uint32_t s_mgmt_frames = 0;
static uint32_t s_beacon_frames = 0;
static uint32_t s_fc_histogram[16] = {0};  /* subtype distribution */

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

    /* Extract BSSID from bytes 16-21 */
    const uint8_t *bssid = &frame[16];

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

    /* Priority 3: SSID pattern match */
    if (ssid[0] != '\0') {
        const drone_ssid_pattern_t *pattern = wifi_ssid_match(ssid);
        if (pattern) {
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

    /* Priority 5: Soft SSID match (cheap drone-like SSIDs, low confidence) */
    if (ssid[0] != '\0' && wifi_ssid_match_soft(ssid)) {
        bool moving = rssi_track_update(bssid, rssi);
        float conf = moving ? 0.30f : 0.15f;

        init_detection(&det, bssid, rssi, ssid);
        det.source = DETECTION_SRC_WIFI_SSID;
        det.confidence = conf;

        strncpy(det.manufacturer, "Drone Likely", sizeof(det.manufacturer) - 1);
        strncpy(det.drone_id, ssid, sizeof(det.drone_id) - 1);

        ESP_LOGI(TAG, "Soft SSID: \"%s\" RSSI=%d ~%.0fm conf=%.2f%s",
                 ssid, rssi, det.estimated_distance_m, conf,
                 moving ? " [MOVING]" : "");

        if (s_detection_queue) {
            xQueueSend(s_detection_queue, &det, pdMS_TO_TICKS(10));
        }
        return;
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

    /* Process beacons, probe responses, AND probe requests.
     * Probe requests (0x40) catch drone controllers searching for their drone's SSID.
     * Even when a drone isn't broadcasting an AP, its controller probes for it. */
    if (frame_ctrl != WIFI_FC_SUBTYPE_BEACON &&
        frame_ctrl != 0x50 /* probe response */ &&
        frame_ctrl != 0x40 /* probe request */) {
        return;
    }
    s_beacon_frames++;

    process_beacon_frame(frame, frame_len, rssi);
}

/* ── Channel advance helpers ────────────────────────────────────────────────── */

static uint16_t next_channel_24ghz(void)
{
    uint16_t ch = s_channels_24ghz[s_idx_24ghz];
    s_idx_24ghz = (s_idx_24ghz + 1) % NUM_CHANNELS_24GHZ;
    return ch;
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

/* ── WiFi scan task (channel hopping) ──────────────────────────────────────── */

static void wifi_scan_task(void *arg)
{
    ESP_LOGI(TAG, "WiFi scan task started on core %d", xPortGetCoreID());
#ifdef CONFIG_SCANNER_5GHZ_ENABLED
    ESP_LOGI(TAG, "Dual-band: %d channels (2.4 GHz: %d, 5 GHz: %d)",
             (int)(NUM_CHANNELS_24GHZ + NUM_CHANNELS_5GHZ),
             (int)NUM_CHANNELS_24GHZ, (int)NUM_CHANNELS_5GHZ);
#else
    ESP_LOGI(TAG, "2.4 GHz only: %d channels", (int)NUM_CHANNELS_24GHZ);
#endif

    TickType_t last_heartbeat = xTaskGetTickCount();

    while (1) {
        /* Sweep all channels */
        int num_channels = NUM_CHANNELS_24GHZ;
#ifdef CONFIG_SCANNER_5GHZ_ENABLED
        num_channels += NUM_CHANNELS_5GHZ;
#endif
        for (int i = 0; i < num_channels; i++) {
            s_current_channel = advance_channel();

            esp_err_t err = esp_wifi_set_channel(s_current_channel, WIFI_SECOND_CHAN_NONE);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to set channel %d: %s",
                         s_current_channel, esp_err_to_name(err));
            }

            vTaskDelay(pdMS_TO_TICKS(CHANNEL_DWELL_MS));
        }

        /* Heartbeat every ~10 seconds */
        if ((xTaskGetTickCount() - last_heartbeat) >= pdMS_TO_TICKS(10000)) {
            last_heartbeat = xTaskGetTickCount();
            ESP_LOGI(TAG, "ch=%d tot=%lu bcn=%lu",
                     s_current_channel,
                     (unsigned long)s_total_frames,
                     (unsigned long)s_beacon_frames);
        }
    }
}

/* ── Public API ────────────────────────────────────────────────────────────── */

void wifi_scanner_init(QueueHandle_t detection_queue)
{
    s_detection_queue = detection_queue;

    /* Initialize WiFi in NULL mode (no STA/AP, just promiscuous) */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
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
