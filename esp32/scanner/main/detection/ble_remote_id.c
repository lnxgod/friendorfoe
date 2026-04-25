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
#include "host/ble_gap.h"

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

/* ── Per-device tracking state ─────────────────────────────────────────────── */

typedef struct {
    uint8_t     mac[6];         /* BLE advertiser MAC address */
    bool        in_use;         /* Slot is occupied */
    odid_state_t odid;          /* Accumulated ODID message state */
    int64_t     last_seen_ms;   /* Timestamp of last advertisement */
} ble_device_slot_t;

/* ── Module state ──────────────────────────────────────────────────────────── */

static QueueHandle_t    s_detection_queue = NULL;

#if CONFIG_FOF_GLASSES_DETECTION
static QueueHandle_t    s_glasses_queue = NULL;
#endif
static ble_device_slot_t s_devices[MAX_BLE_DEVICES];
static bool             s_scanning = false;

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
    const uint8_t *odid_msg;
    int odid_len;

    /*
     * Service data format depends on length:
     *   >= 27 bytes: BLE 5 format — skip app_code(1) + counter(1)
     *   == 25 bytes: BLE 4 advertising — raw ODID message
     */
    if (data_len >= 27) {
        odid_msg = data + 2;  /* skip app_code + counter */
        odid_len = data_len - 2;
    } else if (data_len >= ODID_MESSAGE_SIZE) {
        odid_msg = data;
        odid_len = data_len;
    } else {
        ESP_LOGD(TAG, "Service data too short: %d bytes", data_len);
        return;
    }

    /* Find or create device slot */
    ble_device_slot_t *slot = find_or_alloc_device(mac);
    slot->last_seen_ms = now_ms();

    /* Parse the ODID message into the accumulated state (depth=0 for top-level) */
    odid_parse_message(odid_msg, (size_t)odid_len, &slot->odid, 0);

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
        }
    }
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
    case BLE_GAP_EVENT_DISC:
        ESP_LOGW(TAG, "Ignoring unsupported BLE_GAP_EVENT_DISC event");
        break;

    case BLE_GAP_EVENT_EXT_DISC: {
        /* Extended discovery also reports BLE 4.x advertising packets. */
        const struct ble_gap_ext_disc_desc *ext = &event->ext_disc;

        static uint32_t s_ext_adv_rx = 0;
        s_ext_adv_rx++;
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
            } else if (fp.is_tracker) {
                rate_limit_ms = 1000;   /* Trackers: every 1s */
            } else if (fp.device_type == BLE_DEV_FLIPPER_ZERO) {
                rate_limit_ms = 1000;   /* Security tool: every 1s */
            } else if (fp.device_type == BLE_DEV_META_GLASSES ||
                       fp.device_type == BLE_DEV_META_DEVICE) {
                rate_limit_ms = 2000;   /* Privacy interest: every 2s */
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

            if (!recently_sent) {
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
                    det.confidence = 0.50f;  /* Trackers are high interest */
                } else if (fp.device_type == BLE_DEV_DRONE_CONTROLLER ||
                           fp.device_type == BLE_DEV_DRONE_OTHER) {
                    det.confidence = 0.60f;
                } else if (fp.device_type == BLE_DEV_FLIPPER_ZERO) {
                    det.confidence = 0.40f;  /* Security tool: high interest */
                } else if (fp.device_type == BLE_DEV_META_GLASSES) {
                    det.confidence = 0.30f;  /* Privacy interest */
                } else if (fp.device_type == BLE_DEV_META_DEVICE) {
                    det.confidence = 0.10f;
                } else if (fp.device_type != BLE_DEV_UNKNOWN) {
                    det.confidence = 0.05f;  /* Known type, low interest */
                } else {
                    det.confidence = 0.02f;  /* Unknown */
                }

                /* Use fingerprint hash + type as drone_id for backend tracking */
                const char *device_label = is_calibration_beacon
                    ? "Calibration Beacon"
                    : fp.type_name;
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

                xQueueSend(s_detection_queue, &det, 0);
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

                glasses_detection_t gdet;
                if (glasses_check_advertisement(ext->addr.val, adv_name, adv_name_len,
                        mfr_data, mfr_data_len, svc_uuids, svc_uuid_count,
                        appearance, ext->rssi, &gdet)) {
                    xQueueSend(s_glasses_queue, &gdet, pdMS_TO_TICKS(5));
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
     * Passive scanning with 100% duty cycle (window == interval) to catch
     * every advertisement without missing any while sending SCAN_REQ.
     * The extended-discovery API reports both BLE 4.x and BLE 5.x packets.
     */
    struct ble_gap_ext_disc_params uncoded_params = {
        .itvl = 0x0060,        /* 60ms scan interval (96 * 0.625ms) */
        .window = 0x0060,      /* 60ms window = 100% duty cycle */
        .passive = 1,          /* Passive: never miss adverts while sending SCAN_REQ */
    };

    int rc = ble_gap_ext_disc(
        BLE_OWN_ADDR_PUBLIC,
        0,                     /* duration: 0 = forever */
        0,                     /* period: 0 = continuous */
        0,                     /* filter_duplicates: 0 = report every packet */
        0,                     /* filter_policy: 0 = accept all */
        0,                     /* limited: 0 = general discovery */
        &uncoded_params,       /* 1M PHY params */
        NULL,                  /* coded PHY params (NULL = don't scan coded) */
        ble_gap_event_cb,
        NULL
    );

    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_ext_disc() failed: %d; non-extended discovery is unsupported", rc);
        s_scanning = false;
    } else {
        ESP_LOGI(TAG, "BLE scanning started (ext_disc passive, 100%% duty)");
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

    /* Use best available address type */
    uint8_t own_addr_type;
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }

    ESP_LOGI(TAG, "NimBLE host synced, starting ODID scan");
    s_scanning = true;
    ble_remote_id_start_scan_internal();
}

/* ── NimBLE host task ──────────────────────────────────────────────────────── */

static void ble_host_task(void *arg)
{
    ESP_LOGI(TAG, "NimBLE host task started on core %d", xPortGetCoreID());
    nimble_port_run();          /* Runs forever until nimble_port_stop() */
    nimble_port_freertos_deinit();
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
    ble_hs_cfg.reset_cb = NULL;

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
    /*
     * Start the NimBLE host task on Core 0 (radio core).
     * The host task runs the NimBLE event loop; scanning is started
     * from the sync callback once the host is ready.
     */
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE host task created (core=%d, pri=%d)",
             BLE_SCAN_TASK_CORE, BLE_SCAN_TASK_PRIORITY);
}

void ble_remote_id_stop(void)
{
    s_scanning = false;

    int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_disc_cancel() failed: %d", rc);
    }

    nimble_port_stop();
    ESP_LOGI(TAG, "BLE Remote ID scanner stopped");
}
