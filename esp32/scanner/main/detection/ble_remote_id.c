/**
 * Friend or Foe -- BLE Remote ID Scanner (ASTM F3411 / OpenDroneID)
 *
 * Uses NimBLE to scan for BLE advertisements carrying OpenDroneID data
 * on service UUID 0xFFFA. Maintains per-device partial state to accumulate
 * multiple ODID message types (Basic ID, Location, System, Operator ID)
 * into a complete drone detection.
 *
 * Service data format (BLE 4 advertising):
 *   - 25 bytes: raw ODID message
 *
 * Service data format (BLE 5 Long Range):
 *   - 27+ bytes: app_code(1) + counter(1) + ODID message(25)
 */

#include "ble_remote_id.h"
#include "ble_fingerprint.h"
#include "ble_ja3.h"
#include "open_drone_id_parser.h"
#include "constants.h"
#include "detection_types.h"
#include "detection_policy.h"
#include "core/task_priorities.h"

#include <stdbool.h>
#include <stdint.h>

#include "calibration_mode.h"

#if CONFIG_FOF_GLASSES_DETECTION
#include "glasses_detector.h"
#endif

#include "esp_log.h"
#include "esp_timer.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/ble_gap.h"
#include "host/util/util.h"

#if !MYNEWT_VAL(BLE_EXT_ADV)
#error "Supported FoF scanner firmware requires NimBLE extended advertising discovery."
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>
#include <stdio.h>

/* ── Constants ─────────────────────────────────────────────────────────────── */

static const char *TAG = "ble_rid";

/** OpenDroneID BLE service UUID (16-bit: 0xFFFA) */
#define ODID_SERVICE_UUID_16        0xFFFA

/** Maximum concurrent BLE devices tracked */
#define MAX_BLE_DEVICES             50

/** ODID message size per ASTM F3411 */
#define ODID_MESSAGE_SIZE           25

/* ESP-IDF's FreeRTOS port uses byte-sized StackType_t on Xtensa. Badge
 * privacy classification runs inside the NimBLE host callback, and 4 KB
 * overflows as soon as Meta/Ray-Ban service UUIDs are decoded in busy air.
 */
#define BLE_HOST_TASK_STACK_BYTES   12288
#define BLE_HOST_TASK_PRIORITY      (configMAX_PRIORITIES - 4)

#if defined(FOF_BADGE_VARIANT)
/* Badge privacy mode wants scan responses because many glasses only put
 * useful names in the response packet. */
#define BLE_SCAN_PASSIVE_MODE       0
#else
#define BLE_SCAN_PASSIVE_MODE       1
#endif

/* ── Per-device tracking state ─────────────────────────────────────────────── */

typedef struct {
    uint8_t     mac[6];         /* BLE advertiser MAC address */
    bool        in_use;         /* Slot is occupied */
    odid_state_t odid;          /* Accumulated ODID message state */
    int64_t     last_seen_ms;   /* Timestamp of last advertisement */
} ble_device_slot_t;

/* ── Module state ──────────────────────────────────────────────────────────── */

static QueueHandle_t    s_detection_queue = NULL;
static uint32_t         s_odid_service_seen = 0;
static uint32_t         s_odid_emit = 0;
static uint32_t         s_privacy_seen = 0;
static uint32_t         s_ble_service_trace_seen = 0;
static uint32_t         s_ble_adv_seen = 0;
static uint32_t         s_ble_fp_emit = 0;
static uint32_t         s_ble_meta_seen = 0;
static uint32_t         s_ble_tracker_seen = 0;
static uint32_t         s_ble_privacy_candidate_seen = 0;
static uint32_t         s_ble_near_unknown_seen = 0;
static uint32_t         s_ble_drop_rate = 0;

typedef struct {
    uint32_t seen;
    int8_t   best_rssi;
    char     label[24];
    char     name[32];
    char     reason[32];
    uint16_t company_id;
    uint16_t svc0;
    uint8_t  svc_count;
    uint8_t  payload_len;
} badge_ble_diag_t;

static badge_ble_diag_t s_ble_dbg_near = {0};
static badge_ble_diag_t s_ble_dbg_priv = {0};

static const char *badge_ble_privacy_reason(const ble_fingerprint_t *fp,
                                            int8_t rssi);

static void diag_copy_text(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }
    size_t out = 0;
    for (size_t i = 0; src[i] && out + 1 < dst_len; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (ch < 0x20 || ch > 0x7e) {
            continue;
        }
        if (ch == '"' || ch == '\\') {
            ch = ' ';
        }
        dst[out++] = (char)ch;
    }
    dst[out] = '\0';
}

static void badge_ble_diag_update(badge_ble_diag_t *diag,
                                  const ble_fingerprint_t *fp,
                                  int8_t rssi,
                                  const char *fallback_label)
{
    if (!diag || !fp) {
        return;
    }

    diag->seen++;
    bool better = diag->seen == 1 ||
                  rssi > diag->best_rssi ||
                  (diag->name[0] == '\0' && fp->local_name[0] != '\0');
    if (!better) {
        return;
    }

    diag->best_rssi = rssi;
    const char *label = (fp->type_name && strcmp(fp->type_name, "Unknown") != 0)
        ? fp->type_name
        : fallback_label;
    diag_copy_text(diag->label, sizeof(diag->label), label);
    diag_copy_text(diag->name, sizeof(diag->name), fp->local_name);
    diag_copy_text(diag->reason, sizeof(diag->reason),
                   fp->class_reason[0] ? fp->class_reason :
                   badge_ble_privacy_reason(fp, rssi));
    diag->company_id = fp->company_id;
    diag->svc0 = fp->svc_uuid_count > 0 ? fp->service_uuids[0] : 0;
    diag->svc_count = fp->svc_uuid_count;
    diag->payload_len = fp->payload_len;
}

static void badge_ble_diag_reset(void)
{
    memset(&s_ble_dbg_near, 0, sizeof(s_ble_dbg_near));
    memset(&s_ble_dbg_priv, 0, sizeof(s_ble_dbg_priv));
}

#if CONFIG_FOF_GLASSES_DETECTION
static QueueHandle_t    s_glasses_queue = NULL;

static int collect_uuid16s_from_adv(const uint8_t *data, int length,
                                    uint16_t *out, int out_max)
{
    int count = 0;
    int pos = 0;

    if (!data || !out || out_max <= 0 || length <= 0) {
        return 0;
    }

    while (pos + 1 < length) {
        uint8_t ad_len = data[pos];
        if (ad_len == 0 || pos + 1 + ad_len > length) {
            break;
        }

        uint8_t ad_type = data[pos + 1];
        const uint8_t *ad_data = &data[pos + 2];
        int ad_data_len = ad_len - 1;

        if (ad_type == 0x02 || ad_type == 0x03) {
            for (int i = 0; i + 1 < ad_data_len && count < out_max; i += 2) {
                out[count++] = (uint16_t)ad_data[i] |
                               ((uint16_t)ad_data[i + 1] << 8);
            }
        } else if (ad_type == 0x16 && ad_data_len >= 2 && count < out_max) {
            out[count++] = (uint16_t)ad_data[0] |
                           ((uint16_t)ad_data[1] << 8);
        }

        pos += 1 + ad_len;
    }

    return count;
}
#endif
static ble_device_slot_t s_devices[MAX_BLE_DEVICES];
static bool             s_scanning = false;
static bool             s_host_task_active = false;
static bool             s_host_task_requested = false;
static bool             s_host_synced = false;
static bool             s_initialized = false;
static TaskHandle_t     s_host_task_handle = NULL;
static StaticTask_t     s_host_task_tcb;
static StackType_t      s_host_task_stack[BLE_HOST_TASK_STACK_BYTES];
static uint8_t          s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static int64_t          s_host_start_ms = 0;
static uint32_t         s_ble_host_restart_count = 0;
static uint32_t         s_ble_scan_start_count = 0;
static uint32_t         s_ble_scan_start_ok = 0;
static int              s_ble_scan_last_rc = 0;
static int              s_ble_sync_last_rc = 0;

/* ── BLE Focus Mode (lock-on to specific MAC for Remote ID tracking) ───────── */

static struct {
    bool     active;
    uint8_t  target_mac[6];
    int64_t  start_ms;
    int64_t  duration_ms;           /* 45000 default */
    uint32_t target_adv_count;      /* how many ads from target during focus */
} s_ble_focus = {0};

void ble_rid_lockon(const uint8_t mac[6], int duration_s)
{
    memcpy(s_ble_focus.target_mac, mac, 6);
    s_ble_focus.start_ms = esp_timer_get_time() / 1000;
    s_ble_focus.duration_ms = (int64_t)duration_s * 1000;
    s_ble_focus.target_adv_count = 0;
    s_ble_focus.active = true;
    ESP_LOGW("ble_rid", "BLE FOCUS: tracking %02X:%02X:%02X:%02X:%02X:%02X for %ds",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], duration_s);
}

void ble_rid_lockon_cancel(void)
{
    if (s_ble_focus.active) {
        ESP_LOGI("ble_rid", "BLE FOCUS cancelled after %lu ads from target",
                 (unsigned long)s_ble_focus.target_adv_count);
    }
    s_ble_focus.active = false;
}

static bool ble_rid_is_focused(void)
{
    if (!s_ble_focus.active) return false;
    int64_t elapsed = (esp_timer_get_time() / 1000) - s_ble_focus.start_ms;
    if (elapsed >= s_ble_focus.duration_ms) {
        ESP_LOGI("ble_rid", "BLE FOCUS expired after %lu ads",
                 (unsigned long)s_ble_focus.target_adv_count);
        s_ble_focus.active = false;
        return false;
    }
    return true;
}

static bool ble_rid_is_target(const uint8_t *mac)
{
    return s_ble_focus.active && memcmp(mac, s_ble_focus.target_mac, 6) == 0;
}

/* ── Helper: get current time in milliseconds ──────────────────────────────── */

static int64_t now_ms(void)
{
    return (int64_t)(esp_timer_get_time() / 1000LL);
}

static bool badge_ble_has_structured_hint(const ble_fingerprint_t *fp)
{
    if (!fp) {
        return false;
    }
    return fp->local_name[0] != '\0' ||
           fp->svc_uuid_count > 0 ||
           fp->svc_uuid_128_count > 0 ||
           fp->company_id != 0 ||
           fp->ad_type_count >= 3 ||
           fp->payload_len >= 12;
}

static bool badge_ble_is_privacy_candidate(const ble_fingerprint_t *fp,
                                           int8_t rssi)
{
    if (!fp || fp->device_type != BLE_DEV_UNKNOWN) {
        return false;
    }
    if (rssi >= -50) {
        return true;
    }
    return rssi >= -65 && badge_ble_has_structured_hint(fp);
}

static const char *badge_ble_privacy_reason(const ble_fingerprint_t *fp,
                                            int8_t rssi)
{
    if (rssi >= -50) {
        return "strong BLE near";
    }
    if (fp && fp->local_name[0] != '\0') {
        return "structured BLE name";
    }
    if (fp && (fp->svc_uuid_count > 0 || fp->svc_uuid_128_count > 0)) {
        return "structured BLE service";
    }
    if (fp && fp->company_id != 0) {
        return "structured BLE mfr";
    }
    return "structured BLE near";
}

static bool badge_ble_unknown_diag_should_emit(uint32_t fp_hash,
                                               int8_t rssi,
                                               int64_t now_ms)
{
    static int64_t  s_last_emit_ms = 0;
    static uint32_t s_last_hash = 0;
    static int8_t   s_best_rssi = -127;

    if (rssi < -48) {
        return false;
    }
    if (s_last_emit_ms == 0 ||
        (now_ms - s_last_emit_ms) >= 15000 ||
        (fp_hash == s_last_hash && rssi > (s_best_rssi + 8))) {
        s_last_emit_ms = now_ms;
        s_last_hash = fp_hash;
        s_best_rssi = rssi;
        return true;
    }
    if (rssi > s_best_rssi) {
        s_best_rssi = rssi;
    }
    return false;
}

static bool badge_ble_should_emit_detection(const ble_fingerprint_t *fp,
                                            bool is_calibration_beacon,
                                            bool is_focus_target,
                                            bool is_meta_device,
                                            int8_t rssi)
{
#if defined(FOF_BADGE_VARIANT)
    if (is_calibration_beacon || is_focus_target) {
        return true;
    }
    if (!fp) {
        return false;
    }
    switch (fp->device_type) {
        case BLE_DEV_DRONE_CONTROLLER:
        case BLE_DEV_DRONE_OTHER:
        case BLE_DEV_META_GLASSES:
        case BLE_DEV_META_DEVICE:
        case BLE_DEV_FLOCK_SAFETY:
        case BLE_DEV_CARD_SKIMMER:
        case BLE_DEV_HIDDEN_CAMERA:
        case BLE_DEV_FLIPPER_ZERO:
            return true;
        default:
            break;
    }
    if (is_meta_device) {
        return true;
    }
    return fp->is_tracker && rssi >= -50;
#else
    (void)fp;
    (void)is_calibration_beacon;
    (void)is_focus_target;
    (void)is_meta_device;
    (void)rssi;
    return true;
#endif
}

#if CONFIG_FOF_GLASSES_DETECTION
static uint32_t badge_glasses_hash(const uint8_t mac[6])
{
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < 6; i++) {
        h ^= mac ? mac[i] : 0;
        h *= 0x01000193u;
    }
    return h;
}

static void badge_emit_glasses_detection(const glasses_detection_t *gdet)
{
#if defined(FOF_BADGE_VARIANT)
    static struct {
        uint32_t hash;
        int64_t last_ms;
    } s_recent_glasses[12];
    static int s_recent_idx = 0;

    if (!gdet || !s_detection_queue) {
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    uint32_t hash = badge_glasses_hash(gdet->mac);
    for (int i = 0; i < (int)(sizeof(s_recent_glasses) / sizeof(s_recent_glasses[0])); i++) {
        if (s_recent_glasses[i].hash == hash &&
            (now_ms - s_recent_glasses[i].last_ms) < 1000) {
            return;
        }
    }
    s_recent_glasses[s_recent_idx].hash = hash;
    s_recent_glasses[s_recent_idx].last_ms = now_ms;
    s_recent_idx = (s_recent_idx + 1) %
        (int)(sizeof(s_recent_glasses) / sizeof(s_recent_glasses[0]));

    drone_detection_t det = {0};
    det.source = DETECTION_SRC_BLE_FINGERPRINT;
    det.rssi = gdet->rssi;
    det.confidence = gdet->confidence;
    det.first_seen_ms = now_ms;
    det.last_updated_ms = now_ms;

    const bool is_meta = strcmp(gdet->manufacturer, "Meta") == 0;
    const char *label = is_meta ? "Meta Glasses" :
        (gdet->device_type[0] ? gdet->device_type : "Smart Glasses");
    snprintf(det.drone_id, sizeof(det.drone_id), "BLE:%08lX:%s",
             (unsigned long)hash, label);
    snprintf(det.manufacturer, sizeof(det.manufacturer), "%s", label);
    snprintf(det.model, sizeof(det.model), "%s",
             gdet->device_type[0] ? gdet->device_type : "Smart Glasses");
    strncpy(det.ble_name, gdet->device_name, sizeof(det.ble_name) - 1);
    strncpy(det.class_reason, gdet->match_reason, sizeof(det.class_reason) - 1);
    (void)xQueueSend(s_detection_queue, &det, 0);
#else
    (void)gdet;
#endif
}
#endif

/* ── Device slot management ────────────────────────────────────────────────── */

/**
 * Find or allocate a slot for a BLE device by MAC address.
 * If no existing slot matches, the oldest (LRU) slot is recycled.
 */
static ble_device_slot_t *find_or_alloc_device(const uint8_t mac[6])
{
    int oldest_idx = 0;
    int64_t oldest_time = INT64_MAX;
    int free_idx = -1;

    for (int i = 0; i < MAX_BLE_DEVICES; i++) {
        if (s_devices[i].in_use) {
            if (memcmp(s_devices[i].mac, mac, 6) == 0) {
                return &s_devices[i];
            }
            if (s_devices[i].last_seen_ms < oldest_time) {
                oldest_time = s_devices[i].last_seen_ms;
                oldest_idx = i;
            }
        } else if (free_idx < 0) {
            free_idx = i;
        }
    }

    /* Use a free slot if available, otherwise recycle the oldest */
    int slot_idx = (free_idx >= 0) ? free_idx : oldest_idx;
    ble_device_slot_t *slot = &s_devices[slot_idx];

    memset(slot, 0, sizeof(*slot));
    memcpy(slot->mac, mac, 6);
    slot->in_use = true;
    slot->last_seen_ms = now_ms();

    /* Initialize the ODID accumulator with the device MAC as address */
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    odid_state_init(&slot->odid, mac_str, slot->last_seen_ms);

    return slot;
}

/* ── ODID service data extraction and parsing ──────────────────────────────── */

static bool odid_message_type_is_valid(const uint8_t *data, int len)
{
    if (!data || len < ODID_MESSAGE_SIZE) {
        return false;
    }
    uint8_t message_type = (uint8_t)((data[0] & 0xF0) >> 4);
    return message_type == ODID_MSG_TYPE_BASIC_ID ||
           message_type == ODID_MSG_TYPE_LOCATION ||
           message_type == ODID_MSG_TYPE_AUTH ||
           message_type == ODID_MSG_TYPE_SELF_ID ||
           message_type == ODID_MSG_TYPE_SYSTEM ||
           message_type == ODID_MSG_TYPE_OPERATOR_ID ||
           message_type == ODID_MSG_TYPE_MESSAGE_PACK;
}

static const uint8_t *select_odid_payload(const uint8_t *data, int data_len,
                                          int *odid_len, int *skip_len)
{
    if (!data || !odid_len || !skip_len) {
        return NULL;
    }
    *odid_len = 0;
    *skip_len = 0;

    if (data_len < ODID_MESSAGE_SIZE) {
        return NULL;
    }

    /* Simulators are not perfectly consistent:
     * 25 bytes = raw ODID message
     * 26 bytes = app/counter byte + raw ODID message
     * 27+ bytes = app_code + counter + one or more ODID messages
     */
    if (data_len == ODID_MESSAGE_SIZE) {
        *odid_len = data_len;
        return data;
    }
    if (data_len == ODID_MESSAGE_SIZE + 1 &&
        odid_message_type_is_valid(data + 1, data_len - 1)) {
        *skip_len = 1;
        *odid_len = data_len - 1;
        return data + 1;
    }
    if (data_len >= ODID_MESSAGE_SIZE + 2 &&
        odid_message_type_is_valid(data + 2, data_len - 2)) {
        *skip_len = 2;
        *odid_len = data_len - 2;
        return data + 2;
    }
    if (data_len > ODID_MESSAGE_SIZE &&
        odid_message_type_is_valid(data + 1, data_len - 1)) {
        *skip_len = 1;
        *odid_len = data_len - 1;
        return data + 1;
    }
    if (odid_message_type_is_valid(data, data_len)) {
        *odid_len = data_len;
        return data;
    }
    return NULL;
}

/**
 * Process ODID service data from a BLE advertisement.
 *
 * @param mac      Advertiser MAC address (6 bytes)
 * @param data     Service data payload
 * @param data_len Length of service data
 * @param rssi     Received signal strength
 */
static void process_odid_service_data(const uint8_t mac[6],
                                      const uint8_t *data, int data_len,
                                      int8_t rssi)
{
    int odid_len = 0;
    int skip_len = 0;
    const uint8_t *odid_msg = select_odid_payload(data, data_len,
                                                  &odid_len, &skip_len);
    if (!odid_msg) {
        ESP_LOGD(TAG, "Service data too short: %d bytes", data_len);
        return;
    }
    s_odid_service_seen++;
    if (s_odid_service_seen <= 5 || (s_odid_service_seen % 25) == 0) {
        ESP_LOGI(TAG, "BLE RID service seen count=%lu len=%d skip=%d odid_len=%d type=%u RSSI=%d",
                 (unsigned long)s_odid_service_seen,
                 data_len, skip_len, odid_len,
                 (unsigned)((odid_msg[0] & 0xF0) >> 4),
                 rssi);
    }

    /* Find or create device slot */
    ble_device_slot_t *slot = find_or_alloc_device(mac);
    slot->last_seen_ms = now_ms();

    /* Parse the ODID message into the accumulated state (depth=0 for top-level) */
    odid_parse_message(odid_msg, (size_t)odid_len, &slot->odid, 0);
    if (skip_len > 0) {
        ESP_LOGD(TAG, "BLE RID service payload len=%d skip=%d odid_len=%d",
                 data_len, skip_len, odid_len);
    }

    /* Convert accumulated state to a detection */
    drone_detection_t det;
    if (odid_state_to_detection(&slot->odid, "rid_",
                                DETECTION_SRC_BLE_RID, &det)) {
        det.rssi = rssi;
        snprintf(det.bssid, sizeof(det.bssid),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        /* If no drone_id was set from the ODID Basic ID, use MAC */
        if (det.drone_id[0] == '\0') {
            snprintf(det.drone_id, sizeof(det.drone_id),
                     "ble_%02x%02x%02x%02x%02x%02x",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }

        int64_t ts = now_ms();
        det.first_seen_ms = ts;
        det.last_updated_ms = ts;

        ESP_LOGD(TAG, "BLE RID: id=%s lat=%.6f lon=%.6f alt=%.0fm RSSI=%d",
                 det.drone_id, det.latitude, det.longitude,
                 det.altitude_m, rssi);

        if (s_detection_queue) {
            xQueueSend(s_detection_queue, &det, pdMS_TO_TICKS(10));
            s_odid_emit++;
        }
    }
}

static void trace_ble_service_data(uint16_t uuid16, int data_len, int rssi)
{
#if defined(FOF_BADGE_VARIANT)
    if (s_ble_service_trace_seen < 12 || uuid16 == ODID_SERVICE_UUID_16) {
        s_ble_service_trace_seen++;
        ESP_LOGI(TAG, "BLE service-data uuid=0x%04X len=%d RSSI=%d",
                 uuid16, data_len, rssi);
    }
#else
    (void)uuid16;
    (void)data_len;
    (void)rssi;
#endif
}

/* ── Forward declarations ──────────────────────────────────────────────────── */

static void ble_remote_id_start_scan_internal(void);

/* ── NimBLE GAP event callback ─────────────────────────────────────────────── */

/**
 * Called by NimBLE for each BLE GAP event during scanning.
 * We look for advertisements containing service data for UUID 0xFFFA.
 */
static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        /* Some NimBLE targets surface BLE 4.x advertisements through the
         * legacy discovery event. Keep this path behaviorally aligned with
         * EXT_DISC so calibration beacons are not silently dropped. */
        const struct ble_gap_disc_desc *disc = &event->disc;

#if !defined(FOF_BADGE_VARIANT)
        if (!scanner_calibration_mode_is_active()) {
            break;
        }
#endif

        static uint32_t s_legacy_adv_rx = 0;
        s_legacy_adv_rx++;
        s_ble_adv_seen++;
        if (s_legacy_adv_rx % 500 == 1) {
            ESP_LOGD(TAG, "BLE legacy_adv (total=%lu rssi=%d len=%d)",
                     (unsigned long)s_legacy_adv_rx, disc->rssi,
                     disc->length_data);
        }

        ble_fingerprint_t fp;
        ble_fingerprint_compute(disc->data, disc->length_data,
                                disc->addr.type,
                                (uint8_t)(disc->event_type & 0xFF), &fp);
        bool is_calibration_beacon = fof_policy_ble_has_calibration_uuid_le(
            fp.service_uuids_128,
            fp.svc_uuid_128_count
        );
        bool is_meta_device = fp.device_type == BLE_DEV_META_GLASSES ||
                              fp.device_type == BLE_DEV_META_DEVICE;
#if defined(FOF_BADGE_VARIANT)
        bool badge_nearby_ble_diag = badge_ble_is_privacy_candidate(&fp, disc->rssi);
#else
        bool badge_nearby_ble_diag = false;
#endif
        bool privacy_candidate = is_meta_device || fp.is_tracker || badge_nearby_ble_diag;
        bool known_privacy_candidate = is_meta_device || fp.is_tracker;
        if (is_meta_device) {
            s_ble_meta_seen++;
        }
        if (fp.is_tracker) {
            s_ble_tracker_seen++;
        }
        if (known_privacy_candidate) {
            s_ble_privacy_candidate_seen++;
            badge_ble_diag_update(&s_ble_dbg_priv, &fp, disc->rssi,
                                  is_meta_device ? "Meta" : "Privacy");
        }
        if (badge_nearby_ble_diag) {
            s_ble_near_unknown_seen++;
            if (!known_privacy_candidate) {
                badge_ble_diag_update(&s_ble_dbg_near, &fp, disc->rssi,
                                      "near BLE");
            }
        }
        if (scanner_calibration_mode_is_active() &&
            !scanner_calibration_mode_allows_ble_uuid128(
                fp.service_uuids_128,
                fp.svc_uuid_128_count
            )) {
            break;
        }

        if (disc->data != NULL && disc->length_data > 0) {
            int pos = 0;
            while (pos + 1 < disc->length_data) {
                uint8_t ad_len = disc->data[pos];
                if (ad_len == 0 || pos + 1 + ad_len > disc->length_data) break;
                uint8_t ad_type = disc->data[pos + 1];

                if (ad_type == 0x16 && ad_len >= 3) {
                    uint16_t uuid16 = (uint16_t)disc->data[pos + 2] |
                                      ((uint16_t)disc->data[pos + 3] << 8);
                    trace_ble_service_data(uuid16, ad_len - 3, disc->rssi);
                    if (uuid16 == ODID_SERVICE_UUID_16) {
                        process_odid_service_data(
                            disc->addr.val,
                            &disc->data[pos + 4],
                            ad_len - 3,
                            disc->rssi
                        );
                    }
                }
                pos += 1 + ad_len;
            }
        }

        if (s_detection_queue != NULL) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            bool is_focus_target = ble_rid_is_focused() && ble_rid_is_target(disc->addr.val);
            if (is_focus_target) {
                s_ble_focus.target_adv_count++;
            }
            bool badge_nearby_ble_diag_emit = badge_nearby_ble_diag &&
                badge_ble_unknown_diag_should_emit(fp.hash, disc->rssi, now_ms);

            static uint8_t  last_macs[50][6];
            static int64_t  last_times[50];
            static int      mac_idx = 0;
            int rate_limit_ms;
            if (is_calibration_beacon) {
                rate_limit_ms = 500;
            } else if (is_focus_target) {
                rate_limit_ms = 200;
            } else if (fp.device_type == BLE_DEV_DRONE_CONTROLLER) {
                rate_limit_ms = 500;
            } else if (fp.is_tracker || is_meta_device) {
                rate_limit_ms = 1000;
            } else if (badge_nearby_ble_diag_emit) {
                rate_limit_ms = 15000;
            } else if (fp.device_type == BLE_DEV_FLIPPER_ZERO) {
                rate_limit_ms = 1000;
            } else if (ble_rid_is_focused()) {
                rate_limit_ms = 5000;
            } else if (fp.device_type == BLE_DEV_DRONE_OTHER) {
                rate_limit_ms = 500;
            } else if (fp.device_type == BLE_DEV_AUDIO_DEVICE ||
                       fp.device_type == BLE_DEV_SMART_HOME ||
                       fp.device_type == BLE_DEV_GAMING ||
                       fp.device_type == BLE_DEV_MEDICAL ||
                       fp.device_type == BLE_DEV_ESCOOTER ||
                       fp.device_type == BLE_DEV_VEHICLE ||
                       fp.device_type == BLE_DEV_CAMERA) {
                rate_limit_ms = 10000;
            } else if (fp.device_type != BLE_DEV_UNKNOWN) {
                rate_limit_ms = 5000;
            } else if (badge_nearby_ble_diag) {
                rate_limit_ms = 30000;
            } else {
                rate_limit_ms = 10000;
            }

            bool recently_sent = false;
            for (int i = 0; i < 50; i++) {
                if (memcmp(last_macs[i], disc->addr.val, 6) == 0 &&
                    (now_ms - last_times[i]) < rate_limit_ms) {
                    recently_sent = true;
                    break;
                }
            }

            bool should_emit = badge_ble_should_emit_detection(
                &fp, is_calibration_beacon, is_focus_target, is_meta_device,
                disc->rssi
            );
            if (!recently_sent && should_emit) {
                memcpy(last_macs[mac_idx], disc->addr.val, 6);
                last_times[mac_idx] = now_ms;
                mac_idx = (mac_idx + 1) % 50;

                drone_detection_t det = {0};
                det.source = DETECTION_SRC_BLE_FINGERPRINT;
                det.rssi = disc->rssi;
                det.last_updated_ms = now_ms;
                det.first_seen_ms = now_ms;

                if (is_calibration_beacon) {
                    det.confidence = 0.85f;
                } else if (fp.is_tracker) {
                    det.confidence = 0.65f;
                } else if (fp.device_type == BLE_DEV_DRONE_CONTROLLER ||
                           fp.device_type == BLE_DEV_DRONE_OTHER) {
                    det.confidence = 0.60f;
                } else if (fp.device_type == BLE_DEV_FLIPPER_ZERO) {
                    det.confidence = 0.40f;
                } else if (fp.device_type == BLE_DEV_META_GLASSES) {
                    det.confidence = 0.85f;
                } else if (fp.device_type == BLE_DEV_META_DEVICE) {
                    det.confidence = 0.55f;
                } else if (fp.device_type != BLE_DEV_UNKNOWN) {
                    det.confidence = 0.05f;
                } else if (badge_nearby_ble_diag_emit) {
                    det.confidence = 0.18f;
                } else {
                    det.confidence = 0.02f;
                }

                const char *device_label = is_calibration_beacon
                    ? "Calibration Beacon"
                    : (badge_nearby_ble_diag_emit ? "BLE Nearby" : fp.type_name);
                snprintf(det.drone_id, sizeof(det.drone_id),
                         "BLE:%08lX:%s",
                         (unsigned long)fp.hash, device_label);

                snprintf(det.bssid, sizeof(det.bssid),
                         "%02X:%02X:%02X:%02X:%02X:%02X",
                         disc->addr.val[5], disc->addr.val[4], disc->addr.val[3],
                         disc->addr.val[2], disc->addr.val[1], disc->addr.val[0]);
                snprintf(det.manufacturer, sizeof(det.manufacturer),
                         "%s", device_label);
                snprintf(det.model, sizeof(det.model),
                         "FP:%08lX", (unsigned long)fp.hash);

                det.ble_company_id = fp.company_id;
                det.ble_apple_type = fp.apple_type;
                det.ble_ad_type_count = fp.ad_type_count;
                det.ble_payload_len = fp.payload_len;
                det.ble_addr_type = disc->addr.type;
                strncpy(det.ble_name, fp.local_name, sizeof(det.ble_name) - 1);
                if (badge_nearby_ble_diag_emit && fp.class_reason[0] == '\0') {
                    strncpy(det.class_reason,
                            badge_ble_privacy_reason(&fp, disc->rssi),
                            sizeof(det.class_reason) - 1);
                } else {
                    strncpy(det.class_reason, fp.class_reason, sizeof(det.class_reason) - 1);
                }
                memcpy(det.ble_apple_auth, fp.apple_auth, 3);
                det.ble_apple_activity = fp.apple_activity;
                det.ble_apple_flags = fp.apple_flags;
                memcpy(det.ble_raw_mfr, fp.raw_mfr, fp.raw_mfr_len);
                det.ble_raw_mfr_len = fp.raw_mfr_len;
                det.ble_svc_uuid_count = fp.svc_uuid_count;
                for (int u = 0; u < fp.svc_uuid_count && u < 4; u++) {
                    det.ble_service_uuids[u] = fp.service_uuids[u];
                }
                det.ble_svc_uuid_128_count = fp.svc_uuid_128_count;
                for (int u = 0; u < fp.svc_uuid_128_count && u < 2; u++) {
                    memcpy(det.ble_service_uuids_128[u],
                           fp.service_uuids_128[u], 16);
                }

                {
                    static struct { uint8_t mac[6]; int64_t last_us; } ival_cache[64];
                    static int ival_idx = 0;
                    int64_t now_us = esp_timer_get_time();
                    int found = -1;
                    for (int k = 0; k < 64; k++) {
                        if (memcmp(ival_cache[k].mac, disc->addr.val, 6) == 0) {
                            found = k;
                            break;
                        }
                    }
                    if (found >= 0) {
                        det.ble_adv_interval_us = now_us - ival_cache[found].last_us;
                        ival_cache[found].last_us = now_us;
                    } else {
                        det.ble_adv_interval_us = 0;
                        memcpy(ival_cache[ival_idx].mac, disc->addr.val, 6);
                        ival_cache[ival_idx].last_us = now_us;
                        ival_idx = (ival_idx + 1) % 64;
                    }
                }

                ble_ja3_hash_t ja3;
                if (ble_ja3_from_gap_event(event, &ja3)) {
                    det.ble_ja3_hash = ja3.value;
                }

                if (xQueueSend(s_detection_queue, &det, 0) == pdTRUE) {
                    s_ble_fp_emit++;
                }
            } else if (privacy_candidate || (!should_emit && fp.device_type != BLE_DEV_UNKNOWN)) {
                s_ble_drop_rate++;
            }
        }

#if CONFIG_FOF_GLASSES_DETECTION
        if (s_glasses_queue != NULL && glasses_detection_is_enabled()) {
            struct ble_hs_adv_fields fields;
            int parse_rc = ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data);
            if (parse_rc == 0) {
                const char *adv_name = NULL;
                int adv_name_len = 0;
                const uint8_t *mfr_data = NULL;
                int mfr_data_len = 0;
                uint16_t svc_uuids[8];
                int svc_uuid_count = 0;
                uint16_t appearance = 0;

                if (fields.name) { adv_name = (const char *)fields.name; adv_name_len = fields.name_len; }
                if (fields.mfg_data && fields.mfg_data_len >= 2) { mfr_data = fields.mfg_data; mfr_data_len = fields.mfg_data_len; }
                if (fields.appearance_is_present) appearance = fields.appearance;
                svc_uuid_count = collect_uuid16s_from_adv(disc->data, disc->length_data,
                                                          svc_uuids,
                                                          sizeof(svc_uuids) / sizeof(svc_uuids[0]));

                glasses_detection_t gdet;
                if (glasses_check_advertisement(disc->addr.val, adv_name, adv_name_len,
                        mfr_data, mfr_data_len, svc_uuids, svc_uuid_count,
                        appearance, disc->rssi, &gdet)) {
                    s_privacy_seen++;
                    xQueueSend(s_glasses_queue, &gdet, pdMS_TO_TICKS(5));
                    badge_emit_glasses_detection(&gdet);
                }
            }
        }
#endif
        break;
    }

    case BLE_GAP_EVENT_EXT_DISC: {
        /* Extended discovery also reports BLE 4.x advertising packets. */
        const struct ble_gap_ext_disc_desc *ext = &event->ext_disc;

        static uint32_t s_ext_adv_rx = 0;
        s_ext_adv_rx++;
        s_ble_adv_seen++;
        if (s_ext_adv_rx % 500 == 1) {
            ESP_LOGD(TAG, "BLE ext_adv (total=%lu rssi=%d len=%d ble4=%d)",
                     (unsigned long)s_ext_adv_rx, ext->rssi,
                     ext->length_data,
                     (ext->props & BLE_HCI_ADV_LEGACY_MASK) ? 1 : 0);
        }

        /* Process ODID service data from ext_disc payload */
        if (ext->data != NULL && ext->length_data > 0) {
            int pos = 0;
            while (pos + 1 < ext->length_data) {
                uint8_t ad_len = ext->data[pos];
                if (ad_len == 0 || pos + 1 + ad_len > ext->length_data) break;
                uint8_t ad_type = ext->data[pos + 1];

                if (ad_type == 0x16 && ad_len >= 3) {
                    uint16_t uuid16 = (uint16_t)ext->data[pos + 2] |
                                      ((uint16_t)ext->data[pos + 3] << 8);
                    trace_ble_service_data(uuid16, ad_len - 3, ext->rssi);
                    if (uuid16 == ODID_SERVICE_UUID_16) {
                        process_odid_service_data(
                            ext->addr.val,
                            &ext->data[pos + 4],
                            ad_len - 3,
                            ext->rssi
                        );
                    }
                }
                pos += 1 + ad_len;
            }
        }

        /* Send BLE devices to detection queue, deduplicated by fingerprint hash */
        if (s_detection_queue != NULL) {
            int64_t now_ms = esp_timer_get_time() / 1000;

            /* Compute fingerprint first to dedup by hash, not MAC */
            ble_fingerprint_t fp;
            ble_fingerprint_compute(ext->data, ext->length_data,
                                    ext->addr.type,
                                    (uint8_t)(ext->props & 0xFF), &fp);
            bool is_calibration_beacon = fof_policy_ble_has_calibration_uuid_le(
                fp.service_uuids_128,
                fp.svc_uuid_128_count
            );
            bool is_meta_device = fp.device_type == BLE_DEV_META_GLASSES ||
                                  fp.device_type == BLE_DEV_META_DEVICE;
#if defined(FOF_BADGE_VARIANT)
            bool badge_nearby_ble_diag = badge_ble_is_privacy_candidate(&fp, ext->rssi);
#else
            bool badge_nearby_ble_diag = false;
#endif
            bool privacy_candidate = is_meta_device || fp.is_tracker || badge_nearby_ble_diag;
            bool known_privacy_candidate = is_meta_device || fp.is_tracker;
            if (is_meta_device) {
                s_ble_meta_seen++;
            }
            if (fp.is_tracker) {
                s_ble_tracker_seen++;
            }
            if (known_privacy_candidate) {
                s_ble_privacy_candidate_seen++;
                badge_ble_diag_update(&s_ble_dbg_priv, &fp, ext->rssi,
                                      is_meta_device ? "Meta" : "Privacy");
            }
            if (badge_nearby_ble_diag) {
                s_ble_near_unknown_seen++;
                if (!known_privacy_candidate) {
                    badge_ble_diag_update(&s_ble_dbg_near, &fp, ext->rssi,
                                          "near BLE");
                }
            }
            if (scanner_calibration_mode_is_active() &&
                !scanner_calibration_mode_allows_ble_uuid128(
                    fp.service_uuids_128,
                    fp.svc_uuid_128_count
                )) {
                break;
            }

            /* BLE Focus mode: if this MAC is the tracking target, bypass rate limit */
            bool is_focus_target = ble_rid_is_focused() && ble_rid_is_target(ext->addr.val);
            if (is_focus_target) {
                s_ble_focus.target_adv_count++;
            }
            bool badge_nearby_ble_diag_emit = badge_nearby_ble_diag &&
                badge_ble_unknown_diag_should_emit(fp.hash, ext->rssi, now_ms);

            /* Rate limit: drones/trackers fast, others moderate
             * Focus target: 200ms (5 reports/sec for maximum resolution) */
            static uint8_t  last_macs[50][6];
            static int64_t  last_times[50];
            static int      mac_idx = 0;
            int rate_limit_ms;
            if (is_calibration_beacon) {
                rate_limit_ms = 500;    /* Phone calibration beacon: high cadence */
            } else if (is_focus_target) {
                rate_limit_ms = 200;    /* Focus target: max resolution */
            } else if (fp.device_type == BLE_DEV_DRONE_CONTROLLER) {
                rate_limit_ms = 500;    /* Drones: every 0.5s */
            } else if (fp.is_tracker || is_meta_device) {
                rate_limit_ms = 1000;   /* Trackers: every 1s */
            } else if (badge_nearby_ble_diag_emit) {
                rate_limit_ms = 15000;  /* One compact unknown-BLE diagnostic */
            } else if (fp.device_type == BLE_DEV_FLIPPER_ZERO) {
                rate_limit_ms = 1000;   /* Security tool: every 1s */
            } else if (ble_rid_is_focused()) {
                rate_limit_ms = 5000;   /* Non-target during focus: slow down */
            } else if (fp.device_type == BLE_DEV_DRONE_OTHER) {
                rate_limit_ms = 500;    /* Non-DJI drones: fast like DJI */
            } else if (fp.device_type == BLE_DEV_AUDIO_DEVICE ||
                       fp.device_type == BLE_DEV_SMART_HOME ||
                       fp.device_type == BLE_DEV_GAMING ||
                       fp.device_type == BLE_DEV_MEDICAL ||
                       fp.device_type == BLE_DEV_ESCOOTER ||
                       fp.device_type == BLE_DEV_VEHICLE ||
                       fp.device_type == BLE_DEV_CAMERA) {
                rate_limit_ms = 10000;  /* Low interest: every 10s */
            } else if (fp.device_type != BLE_DEV_UNKNOWN) {
                rate_limit_ms = 5000;   /* Known devices: every 5s */
            } else if (badge_nearby_ble_diag) {
                rate_limit_ms = 30000;  /* Keep unknown BLE off the main display */
            } else {
                rate_limit_ms = 10000;  /* Unknown: every 10s (was 3s — floods UART) */
            }

            bool recently_sent = false;
            for (int i = 0; i < 50; i++) {
                if (memcmp(last_macs[i], ext->addr.val, 6) == 0 &&
                    (now_ms - last_times[i]) < rate_limit_ms) {
                    recently_sent = true;
                    break;
                }
            }

            bool should_emit = badge_ble_should_emit_detection(
                &fp, is_calibration_beacon, is_focus_target, is_meta_device,
                ext->rssi
            );
            if (!recently_sent && should_emit) {
                memcpy(last_macs[mac_idx], ext->addr.val, 6);
                last_times[mac_idx] = now_ms;
                mac_idx = (mac_idx + 1) % 50;

                drone_detection_t det = {0};
                det.source = DETECTION_SRC_BLE_FINGERPRINT;
                det.rssi = ext->rssi;
                det.last_updated_ms = now_ms;
                det.first_seen_ms = now_ms;

                /* Confidence based on device classification */
                if (is_calibration_beacon) {
                    det.confidence = 0.85f;
                } else if (fp.is_tracker) {
                    det.confidence = 0.65f;  /* Trackers are high interest */
                } else if (fp.device_type == BLE_DEV_DRONE_CONTROLLER ||
                           fp.device_type == BLE_DEV_DRONE_OTHER) {
                    det.confidence = 0.60f;
                } else if (fp.device_type == BLE_DEV_FLIPPER_ZERO) {
                    det.confidence = 0.40f;  /* Security tool: high interest */
                } else if (fp.device_type == BLE_DEV_META_GLASSES) {
                    det.confidence = 0.85f;  /* Badge privacy demo priority */
                } else if (fp.device_type == BLE_DEV_META_DEVICE) {
                    det.confidence = 0.55f;
                } else if (fp.device_type != BLE_DEV_UNKNOWN) {
                    det.confidence = 0.05f;  /* Known type, low interest */
                } else if (badge_nearby_ble_diag_emit) {
                    det.confidence = 0.18f;
                } else {
                    det.confidence = 0.02f;  /* Unknown */
                }

                /* Use fingerprint hash + type as drone_id for backend tracking */
                const char *device_label = is_calibration_beacon
                    ? "Calibration Beacon"
                    : (badge_nearby_ble_diag_emit ? "BLE Nearby" : fp.type_name);
                snprintf(det.drone_id, sizeof(det.drone_id),
                         "BLE:%08lX:%s",
                         (unsigned long)fp.hash, device_label);

                snprintf(det.bssid, sizeof(det.bssid),
                         "%02X:%02X:%02X:%02X:%02X:%02X",
                         ext->addr.val[5], ext->addr.val[4], ext->addr.val[3],
                         ext->addr.val[2], ext->addr.val[1], ext->addr.val[0]);

                /* Store device type in manufacturer field */
                snprintf(det.manufacturer, sizeof(det.manufacturer),
                         "%s", device_label);

                /* Store fingerprint hash in model field for backend correlation */
                snprintf(det.model, sizeof(det.model),
                         "FP:%08lX", (unsigned long)fp.hash);

                /* BLE-specific fields for backend device fingerprinting */
                det.ble_company_id = fp.company_id;
                det.ble_apple_type = fp.apple_type;
                det.ble_ad_type_count = fp.ad_type_count;
                det.ble_payload_len = fp.payload_len;
                det.ble_addr_type = ext->addr.type;
                strncpy(det.ble_name, fp.local_name, sizeof(det.ble_name) - 1);
                if (badge_nearby_ble_diag_emit && fp.class_reason[0] == '\0') {
                    strncpy(det.class_reason,
                            badge_ble_privacy_reason(&fp, ext->rssi),
                            sizeof(det.class_reason) - 1);
                } else {
                    strncpy(det.class_reason, fp.class_reason, sizeof(det.class_reason) - 1);
                }

                /* Apple Continuity deep fields */
                memcpy(det.ble_apple_auth, fp.apple_auth, 3);
                det.ble_apple_activity = fp.apple_activity;
                det.ble_apple_flags = fp.apple_flags;
                memcpy(det.ble_raw_mfr, fp.raw_mfr, fp.raw_mfr_len);
                det.ble_raw_mfr_len = fp.raw_mfr_len;

                /* Service UUIDs from fingerprint */
                det.ble_svc_uuid_count = fp.svc_uuid_count;
                for (int u = 0; u < fp.svc_uuid_count && u < 4; u++) {
                    det.ble_service_uuids[u] = fp.service_uuids[u];
                }
                /* v0.63: 128-bit service UUIDs */
                det.ble_svc_uuid_128_count = fp.svc_uuid_128_count;
                for (int u = 0; u < fp.svc_uuid_128_count && u < 2; u++) {
                    memcpy(det.ble_service_uuids_128[u],
                           fp.service_uuids_128[u], 16);
                }

                /* Advertisement interval tracking (per-MAC timing) */
                {
                    static struct { uint8_t mac[6]; int64_t last_us; } ival_cache[64];
                    static int ival_idx = 0;
                    int64_t now_us = esp_timer_get_time();
                    int found = -1;
                    for (int k = 0; k < 64; k++) {
                        if (memcmp(ival_cache[k].mac, ext->addr.val, 6) == 0) {
                            found = k;
                            break;
                        }
                    }
                    if (found >= 0) {
                        det.ble_adv_interval_us = now_us - ival_cache[found].last_us;
                        ival_cache[found].last_us = now_us;
                    } else {
                        det.ble_adv_interval_us = 0;
                        memcpy(ival_cache[ival_idx].mac, ext->addr.val, 6);
                        ival_cache[ival_idx].last_us = now_us;
                        ival_idx = (ival_idx + 1) % 64;
                    }
                }

                /* BLE-JA3 structural profile hash (same for all devices of same model) */
                ble_ja3_hash_t ja3;
                if (ble_ja3_from_gap_event(event, &ja3)) {
                    det.ble_ja3_hash = ja3.value;
                }

                if (xQueueSend(s_detection_queue, &det, 0) == pdTRUE) {
                    s_ble_fp_emit++;
                }
            } else if (privacy_candidate || (!should_emit && fp.device_type != BLE_DEV_UNKNOWN)) {
                s_ble_drop_rate++;
            }
        }

#if CONFIG_FOF_GLASSES_DETECTION
        if (s_glasses_queue != NULL && glasses_detection_is_enabled()) {
            struct ble_hs_adv_fields fields;
            int parse_rc = ble_hs_adv_parse_fields(&fields, ext->data, ext->length_data);
            if (parse_rc == 0) {
                const char *adv_name = NULL;
                int adv_name_len = 0;
                const uint8_t *mfr_data = NULL;
                int mfr_data_len = 0;
                uint16_t svc_uuids[8];
                int svc_uuid_count = 0;
                uint16_t appearance = 0;

                if (fields.name) { adv_name = (const char *)fields.name; adv_name_len = fields.name_len; }
                if (fields.mfg_data && fields.mfg_data_len >= 2) { mfr_data = fields.mfg_data; mfr_data_len = fields.mfg_data_len; }
                if (fields.appearance_is_present) appearance = fields.appearance;
                svc_uuid_count = collect_uuid16s_from_adv(ext->data, ext->length_data,
                                                          svc_uuids,
                                                          sizeof(svc_uuids) / sizeof(svc_uuids[0]));

                glasses_detection_t gdet;
                if (glasses_check_advertisement(ext->addr.val, adv_name, adv_name_len,
                        mfr_data, mfr_data_len, svc_uuids, svc_uuid_count,
                        appearance, ext->rssi, &gdet)) {
                    s_privacy_seen++;
                    xQueueSend(s_glasses_queue, &gdet, pdMS_TO_TICKS(5));
                    badge_emit_glasses_detection(&gdet);
                }
            }
        }
#endif

        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "BLE scan complete, restarting...");
        if (s_scanning) {
            ble_remote_id_start_scan_internal();
        }
        break;

    default:
        break;
    }

    return 0;
}

/* ── Internal: start BLE scanning ──────────────────────────────────────────── */

static void ble_remote_id_start_scan_internal(void)
{
    /*
     * Use ble_gap_ext_disc for BLE 5 extended discovery on ESP32-S3.
     * 100% duty cycle (window == interval) to catch every advertisement.
     * Badge privacy builds use active scanning so glasses scan-response names
     * make it into the classifier.
     * The extended-discovery API reports both BLE 4.x and BLE 5.x packets.
     */
    struct ble_gap_ext_disc_params uncoded_params = {
        .itvl = 0x0060,        /* 60ms scan interval (96 * 0.625ms) */
        .window = 0x0060,      /* 60ms window = 100% duty cycle */
        .passive = BLE_SCAN_PASSIVE_MODE,
    };
    struct ble_gap_ext_disc_params coded_params = {
        .itvl = 0x0060,
        .window = 0x0060,
        .passive = BLE_SCAN_PASSIVE_MODE,
    };

    s_ble_scan_start_count++;
    int rc = ble_gap_ext_disc(
        s_own_addr_type,
        0,                     /* duration: 0 = forever */
        0,                     /* period: 0 = continuous */
        0,                     /* filter_duplicates: 0 = report every packet */
        0,                     /* filter_policy: 0 = accept all */
        0,                     /* limited: 0 = general discovery */
        &uncoded_params,       /* 1M PHY params */
        &coded_params,         /* coded PHY params for BT5 Long Range RID */
        ble_gap_event_cb,
        NULL
    );
    s_ble_scan_last_rc = rc;

    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_ext_disc() failed: %d; non-extended discovery is unsupported", rc);
        s_scanning = false;
    } else {
        s_scanning = true;
        s_ble_scan_start_ok++;
        ESP_LOGI(TAG, "BLE scanning started (ext_disc %s, 1M+coded PHY, 100%% duty)",
                 BLE_SCAN_PASSIVE_MODE ? "passive" : "active");
    }
}

/* ── NimBLE host sync callback ─────────────────────────────────────────────── */

/**
 * Called when the NimBLE host has synced with the controller.
 * This is the safe point to start BLE operations.
 */
static void ble_on_sync(void)
{
    int rc;

    rc = ble_hs_util_ensure_addr(0);
    if (rc == BLE_HS_ENOADDR) {
        ble_addr_t addr;
        rc = ble_hs_id_gen_rnd(0, &addr);
        if (rc == 0) {
            rc = ble_hs_id_set_rnd(addr.val);
        }
    }
    if (rc != 0) {
        s_ble_sync_last_rc = rc;
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed: %d", rc);
        return;
    }

    /* Use best available address type */
    uint8_t own_addr_type;
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        s_ble_sync_last_rc = rc;
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }
    s_ble_sync_last_rc = 0;
    s_own_addr_type = own_addr_type;
    s_host_synced = true;

    ESP_LOGI(TAG, "NimBLE host synced, starting ODID scan");
    ble_remote_id_start_scan_internal();
}

static void ble_on_reset(int reason)
{
    s_ble_sync_last_rc = reason;
    s_host_synced = false;
    s_scanning = false;
    ESP_LOGW(TAG, "NimBLE host reset: %d", reason);
}

/* ── NimBLE host task ──────────────────────────────────────────────────────── */

static void ble_host_task(void *arg)
{
    s_host_task_requested = false;
    s_host_task_active = true;
    ESP_LOGI(TAG, "NimBLE host task started on core %d", xPortGetCoreID());
    nimble_port_run();          /* Runs forever until nimble_port_stop() */
    s_host_task_active = false;
    s_host_task_requested = false;
    s_host_synced = false;
    s_scanning = false;
    s_host_start_ms = 0;
    s_host_task_handle = NULL;
    ESP_LOGI(TAG, "NimBLE host task stopped");
    vTaskDelete(NULL);
}

static bool ble_wait_host_stopped(int timeout_ms)
{
    int waited_ms = 0;
    while (s_host_task_active && waited_ms < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(25));
        waited_ms += 25;
    }
    return !s_host_task_active;
}

/* ── Public API ────────────────────────────────────────────────────────────── */

void ble_remote_id_init(QueueHandle_t detection_queue)
{
    s_detection_queue = detection_queue;
    memset(s_devices, 0, sizeof(s_devices));

    /* Initialize NimBLE */
    ESP_ERROR_CHECK(nimble_port_init());

    /* Configure host callbacks */
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;
    s_initialized = true;

    ESP_LOGI(TAG, "BLE Remote ID scanner initialized");
}

#if CONFIG_FOF_GLASSES_DETECTION
void ble_remote_id_set_glasses_queue(QueueHandle_t queue)
{
    s_glasses_queue = queue;
    ESP_LOGI(TAG, "Glasses detection queue attached");
}
#endif

void ble_remote_id_start(void)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "BLE start requested before NimBLE init; deferring");
        return;
    }
    if (s_host_task_requested && !s_host_task_active) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        int64_t age_ms = s_host_start_ms > 0 ? now_ms - s_host_start_ms : 0;
        if (age_ms < 5000) {
            ESP_LOGI(TAG, "BLE host task creation pending (%lldms)",
                     (long long)age_ms);
            return;
        }
        ESP_LOGW(TAG, "BLE host task did not start after %lldms; retrying",
                 (long long)age_ms);
        s_ble_host_restart_count++;
        s_host_task_requested = false;
        s_host_start_ms = 0;
    }
    if (s_host_task_active) {
        if (!s_scanning && s_host_synced) {
            ESP_LOGW(TAG, "BLE host active but scan stopped; restarting scan");
            ble_remote_id_start_scan_internal();
            return;
        } else if (!s_scanning && !s_host_synced) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            int64_t age_ms = s_host_start_ms > 0 ? now_ms - s_host_start_ms : 0;
            if (age_ms >= 5000) {
                ESP_LOGW(TAG, "BLE host active but unsynced for %lldms; restarting host",
                         (long long)age_ms);
                s_ble_host_restart_count++;
                (void)nimble_port_stop();
                if (!ble_wait_host_stopped(1200)) {
                    ESP_LOGW(TAG, "BLE host restart deferred; host stop still pending");
                    return;
                }
            } else {
                ESP_LOGI(TAG, "BLE host task active, waiting for sync (%lldms)",
                         (long long)age_ms);
                return;
            }
        } else {
            ESP_LOGI(TAG, "BLE host task already active (scanning=%d)", s_scanning ? 1 : 0);
            return;
        }
    }
    /*
     * Start the NimBLE host task on Core 0 (radio core).
     * The host task runs the NimBLE event loop; scanning is started
     * from the sync callback once the host is ready.
     */
    s_host_task_requested = true;
    s_host_synced = false;
    s_scanning = false;
    s_host_start_ms = esp_timer_get_time() / 1000;
    s_host_task_handle = xTaskCreateStaticPinnedToCore(
        ble_host_task,
        "nimble_host",
        BLE_HOST_TASK_STACK_BYTES,
        NULL,
        BLE_HOST_TASK_PRIORITY,
        s_host_task_stack,
        &s_host_task_tcb,
        BLE_SCAN_TASK_CORE
    );
    if (s_host_task_handle == NULL) {
        s_host_task_requested = false;
        s_host_start_ms = 0;
        s_ble_sync_last_rc = -1000;
        ESP_LOGE(TAG, "Failed to create NimBLE host task (stack_bytes=%u pri=%u core=%u)",
                 (unsigned)BLE_HOST_TASK_STACK_BYTES,
                 (unsigned)BLE_HOST_TASK_PRIORITY,
                 (unsigned)BLE_SCAN_TASK_CORE);
        return;
    }

    ESP_LOGI(TAG, "BLE host task created (core=%d, pri=%d)",
             BLE_SCAN_TASK_CORE, BLE_HOST_TASK_PRIORITY);
}

void ble_remote_id_stop(void)
{
    if (!s_host_task_active && !s_host_task_requested && !s_scanning) {
        return;
    }
    s_host_task_requested = false;
    s_scanning = false;

    int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_disc_cancel() failed: %d", rc);
    }

    nimble_port_stop();
    if (!ble_wait_host_stopped(600)) {
        ESP_LOGW(TAG, "BLE host stop still pending after 600ms");
    }
    ESP_LOGI(TAG, "BLE Remote ID scanner stopped");
}

bool ble_remote_id_is_scanning(void)
{
    return s_scanning;
}

void ble_remote_id_get_stats(ble_remote_id_stats_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->ble_scanning = s_scanning;
    out->ble_host_active = s_host_task_active;
    out->ble_host_synced = s_host_synced;
    out->ble_adv_seen = s_ble_adv_seen;
    out->ble_fp_emit = s_ble_fp_emit;
    out->ble_meta_seen = s_ble_meta_seen;
    out->ble_tracker_seen = s_ble_tracker_seen;
    out->ble_privacy_candidate_seen = s_ble_privacy_candidate_seen;
    out->ble_near_unknown_seen = s_ble_near_unknown_seen;
    out->ble_drop_rate = s_ble_drop_rate;
    out->ble_dbg_near_seen = s_ble_dbg_near.seen;
    out->ble_dbg_near_rssi = s_ble_dbg_near.best_rssi;
    strncpy(out->ble_dbg_near_label, s_ble_dbg_near.label,
            sizeof(out->ble_dbg_near_label) - 1);
    strncpy(out->ble_dbg_near_name, s_ble_dbg_near.name,
            sizeof(out->ble_dbg_near_name) - 1);
    strncpy(out->ble_dbg_near_reason, s_ble_dbg_near.reason,
            sizeof(out->ble_dbg_near_reason) - 1);
    out->ble_dbg_near_cid = s_ble_dbg_near.company_id;
    out->ble_dbg_near_svc0 = s_ble_dbg_near.svc0;
    out->ble_dbg_near_svc_count = s_ble_dbg_near.svc_count;
    out->ble_dbg_near_payload_len = s_ble_dbg_near.payload_len;
    out->ble_dbg_priv_seen = s_ble_dbg_priv.seen;
    out->ble_dbg_priv_rssi = s_ble_dbg_priv.best_rssi;
    strncpy(out->ble_dbg_priv_label, s_ble_dbg_priv.label,
            sizeof(out->ble_dbg_priv_label) - 1);
    strncpy(out->ble_dbg_priv_name, s_ble_dbg_priv.name,
            sizeof(out->ble_dbg_priv_name) - 1);
    strncpy(out->ble_dbg_priv_reason, s_ble_dbg_priv.reason,
            sizeof(out->ble_dbg_priv_reason) - 1);
    out->ble_dbg_priv_cid = s_ble_dbg_priv.company_id;
    out->ble_dbg_priv_svc0 = s_ble_dbg_priv.svc0;
    out->ble_dbg_priv_svc_count = s_ble_dbg_priv.svc_count;
    out->ble_dbg_priv_payload_len = s_ble_dbg_priv.payload_len;
    out->ble_host_restart_count = s_ble_host_restart_count;
    out->ble_scan_start_count = s_ble_scan_start_count;
    out->ble_scan_start_ok = s_ble_scan_start_ok;
    out->ble_scan_last_rc = s_ble_scan_last_rc;
    out->ble_sync_last_rc = s_ble_sync_last_rc;
}

void ble_remote_id_reset_profile_counters(void)
{
    s_ble_adv_seen = 0;
    s_ble_fp_emit = 0;
    s_ble_meta_seen = 0;
    s_ble_tracker_seen = 0;
    s_ble_privacy_candidate_seen = 0;
    s_ble_near_unknown_seen = 0;
    s_ble_drop_rate = 0;
    badge_ble_diag_reset();
    s_privacy_seen = 0;
    s_odid_service_seen = 0;
    s_odid_emit = 0;
    s_ble_service_trace_seen = 0;
}

uint32_t ble_remote_id_service_seen_count(void)
{
    return s_odid_service_seen;
}

uint32_t ble_remote_id_emit_count(void)
{
    return s_odid_emit;
}

uint32_t ble_remote_id_privacy_seen_count(void)
{
    return s_privacy_seen;
}
