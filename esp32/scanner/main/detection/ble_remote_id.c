/**
 * Friend or Foe -- BLE Remote ID Scanner (ASTM F3411 / OpenDroneID)
 *
 * Uses NimBLE to scan for BLE advertisements carrying OpenDroneID data
 * on service UUID 0xFFFA. Maintains per-device partial state to accumulate
 * multiple ODID message types (Basic ID, Location, System, Operator ID)
 * into a complete drone detection.
 *
 * Service data format (BLE 4 Legacy):
 *   - 25 bytes: raw ODID message
 *
 * Service data format (BLE 5 Long Range):
 *   - 27+ bytes: app_code(1) + counter(1) + ODID message(25)
 */

#include "ble_remote_id.h"
#include "ble_fingerprint.h"
#include "open_drone_id_parser.h"
#include "constants.h"
#include "detection_types.h"
#include "core/task_priorities.h"

#if CONFIG_FOF_GLASSES_DETECTION
#include "glasses_detector.h"
#endif

#include "esp_log.h"
#include "esp_timer.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"

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
     *   == 25 bytes: BLE 4 legacy — raw ODID message
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

        /* If no drone_id was set from the ODID Basic ID, use MAC */
        if (det.drone_id[0] == '\0') {
            snprintf(det.drone_id, sizeof(det.drone_id),
                     "ble_%02x%02x%02x%02x%02x%02x",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }

        int64_t ts = now_ms();
        det.first_seen_ms = ts;
        det.last_updated_ms = ts;

        ESP_LOGI(TAG, "BLE RID: id=%s lat=%.6f lon=%.6f alt=%.0fm RSSI=%d",
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
    case BLE_GAP_EVENT_DISC: {
        const struct ble_gap_disc_desc *desc = &event->disc;

        /* Debug: count all BLE advertisements to verify scanner is receiving */
        static uint32_t s_total_adv_rx = 0;
        s_total_adv_rx++;
        if (s_total_adv_rx % 500 == 1) {
            ESP_LOGI(TAG, "BLE adv received (total=%lu, this: addr=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d len=%d)",
                     (unsigned long)s_total_adv_rx,
                     desc->addr.val[5], desc->addr.val[4], desc->addr.val[3],
                     desc->addr.val[2], desc->addr.val[1], desc->addr.val[0],
                     desc->rssi, desc->length_data);
        }

        /*
         * Search the advertisement data for service data with UUID 0xFFFA.
         *
         * BLE AD type 0x16 = Service Data - 16-bit UUID
         * Format: UUID_lo(1) + UUID_hi(1) + service_data(N)
         */
        /*
         * Try NimBLE structured parser first (handles standard advertisements
         * with flags). If it succeeds, check for ODID service data.
         */
        struct ble_hs_adv_fields fields;
        int rc = ble_hs_adv_parse_fields(&fields, desc->data, desc->length_data);
        if (rc == 0) {
            /*
             * NimBLE parses svc_data_uuid16 for us. Check if it matches ODID UUID.
             * The svc_data_uuid16 field contains the raw bytes after the AD length+type,
             * starting with the 2-byte UUID in little-endian.
             */
            if (fields.svc_data_uuid16 != NULL && fields.svc_data_uuid16_len >= 2) {
                uint16_t uuid16 = (uint16_t)fields.svc_data_uuid16[0] |
                                  ((uint16_t)fields.svc_data_uuid16[1] << 8);

                if (uuid16 == ODID_SERVICE_UUID_16 && fields.svc_data_uuid16_len > 2) {
                    const uint8_t *svc_data = fields.svc_data_uuid16 + 2;
                    int svc_data_len = fields.svc_data_uuid16_len - 2;

                    process_odid_service_data(
                        desc->addr.val,
                        svc_data,
                        svc_data_len,
                        desc->rssi
                    );
                }
            }
        }

        /*
         * ALWAYS walk raw AD structures as fallback — handles non-standard
         * advertisements without flags (e.g., OpenDroneID-only payloads where
         * flags are omitted to fit within the 31-byte BLE 4.x limit).
         */
        if (desc->data != NULL && desc->length_data > 0) {
            /* Walk raw AD structures looking for type 0x16 with UUID 0xFFFA */
            int pos = 0;
            while (pos + 1 < desc->length_data) {
                uint8_t ad_len = desc->data[pos];
                if (ad_len == 0 || pos + 1 + ad_len > desc->length_data) {
                    break;
                }
                uint8_t ad_type = desc->data[pos + 1];

                /* AD type 0x16 = Service Data - 16-bit UUID */
                if (ad_type == 0x16 && ad_len >= 3) {
                    uint16_t uuid16 = (uint16_t)desc->data[pos + 2] |
                                      ((uint16_t)desc->data[pos + 3] << 8);
                    if (uuid16 == ODID_SERVICE_UUID_16) {
                        const uint8_t *svc_data = &desc->data[pos + 4];
                        int svc_data_len = ad_len - 3; /* minus type(1) + uuid(2) */

                        process_odid_service_data(
                            desc->addr.val,
                            svc_data,
                            svc_data_len,
                            desc->rssi
                        );
                    }
                }

                pos += 1 + ad_len;
            }
        }

#if CONFIG_FOF_GLASSES_DETECTION
        /* ── Smart glasses / privacy device check ──────────────────── */
        if (s_glasses_queue != NULL && glasses_detection_is_enabled()) {
            /* Extract name, manufacturer data, service UUIDs from parsed fields */
            const char *adv_name = NULL;
            int adv_name_len = 0;
            const uint8_t *mfr_data = NULL;
            int mfr_data_len = 0;
            uint16_t svc_uuids[8];
            int svc_uuid_count = 0;
            uint16_t appearance = 0;

            if (rc == 0) { /* NimBLE parsed successfully */
                if (fields.name != NULL && fields.name_len > 0) {
                    adv_name = (const char *)fields.name;
                    adv_name_len = fields.name_len;
                }
                if (fields.mfg_data != NULL && fields.mfg_data_len >= 2) {
                    mfr_data = fields.mfg_data;
                    mfr_data_len = fields.mfg_data_len;
                }
                if (fields.svc_data_uuid16 != NULL && fields.svc_data_uuid16_len >= 2) {
                    uint16_t u = (uint16_t)fields.svc_data_uuid16[0] |
                                 ((uint16_t)fields.svc_data_uuid16[1] << 8);
                    if (u != ODID_SERVICE_UUID_16 && svc_uuid_count < 8) {
                        svc_uuids[svc_uuid_count++] = u;
                    }
                }
                if (fields.uuids16 != NULL) {
                    for (int i = 0; i < (int)fields.num_uuids16 && svc_uuid_count < 8; i++) {
                        svc_uuids[svc_uuid_count++] = ble_uuid_u16(&fields.uuids16[i].u);
                    }
                }
                if (fields.appearance_is_present) {
                    appearance = fields.appearance;
                }
            }

            glasses_detection_t gdet;
            if (glasses_check_advertisement(
                    desc->addr.val, adv_name, adv_name_len,
                    mfr_data, mfr_data_len,
                    svc_uuids, svc_uuid_count,
                    appearance, desc->rssi, &gdet)) {
                xQueueSend(s_glasses_queue, &gdet, pdMS_TO_TICKS(5));
            }
        }
#endif  /* CONFIG_FOF_GLASSES_DETECTION */

        break;
    }

    case BLE_GAP_EVENT_EXT_DISC: {
        /* Extended discovery event (BLE 5) — same processing as legacy */
        const struct ble_gap_ext_disc_desc *ext = &event->ext_disc;

        static uint32_t s_ext_adv_rx = 0;
        s_ext_adv_rx++;
        if (s_ext_adv_rx % 500 == 1) {
            ESP_LOGI(TAG, "BLE ext_adv (total=%lu rssi=%d len=%d legacy=%d)",
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

            /* Rate limit: drones/trackers fast, others moderate */
            static uint32_t last_hashes[50];
            static int64_t  last_times[50];
            static int      hash_idx = 0;
            int rate_limit_ms;
            if (fp.device_type == BLE_DEV_DRONE_CONTROLLER) {
                rate_limit_ms = 1000;   /* Drones: every 1s */
            } else if (fp.is_tracker) {
                rate_limit_ms = 1500;   /* Trackers: every 1.5s */
            } else {
                rate_limit_ms = 5000;   /* Others: every 5s (was 10s) */
            }

            bool recently_sent = false;
            for (int i = 0; i < 50; i++) {
                if (last_hashes[i] == fp.hash &&
                    (now_ms - last_times[i]) < rate_limit_ms) {
                    recently_sent = true;
                    break;
                }
            }

            if (!recently_sent) {
                last_hashes[hash_idx] = fp.hash;
                last_times[hash_idx] = now_ms;
                hash_idx = (hash_idx + 1) % 50;

                drone_detection_t det = {0};
                det.source = DETECTION_SRC_BLE_RID;
                det.rssi = ext->rssi;
                det.last_updated_ms = now_ms;
                det.first_seen_ms = now_ms;

                /* Confidence based on device classification */
                if (fp.is_tracker) {
                    det.confidence = 0.50f;  /* Trackers are high interest */
                } else if (fp.device_type == BLE_DEV_DRONE_CONTROLLER) {
                    det.confidence = 0.60f;
                } else if (fp.device_type != BLE_DEV_UNKNOWN) {
                    det.confidence = 0.05f;  /* Known type, low interest */
                } else {
                    det.confidence = 0.02f;  /* Unknown */
                }

                /* Use fingerprint hash + type as drone_id for backend tracking */
                snprintf(det.drone_id, sizeof(det.drone_id),
                         "BLE:%08lX:%s",
                         (unsigned long)fp.hash, fp.type_name);

                snprintf(det.bssid, sizeof(det.bssid),
                         "%02X:%02X:%02X:%02X:%02X:%02X",
                         ext->addr.val[5], ext->addr.val[4], ext->addr.val[3],
                         ext->addr.val[2], ext->addr.val[1], ext->addr.val[0]);

                /* Store device type in manufacturer field */
                snprintf(det.manufacturer, sizeof(det.manufacturer),
                         "%s", fp.type_name);

                /* Store fingerprint hash in model field for backend correlation */
                snprintf(det.model, sizeof(det.model),
                         "FP:%08lX", (unsigned long)fp.hash);

                /* BLE-specific fields for backend device fingerprinting */
                det.ble_company_id = fp.company_id;
                det.ble_apple_type = fp.apple_type;
                det.ble_ad_type_count = fp.ad_type_count;
                det.ble_payload_len = fp.payload_len;
                det.ble_addr_type = ext->addr.type;

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
     * Handles both legacy (4.x) and extended (5.x) advertising.
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
        ESP_LOGE(TAG, "ble_gap_ext_disc() failed: %d, falling back to legacy", rc);
        /* Fallback to legacy discovery */
        struct ble_gap_disc_params legacy_params = {
            .passive = 1,
            .itvl = 0x0060,
            .window = 0x0060,
            .filter_duplicates = 0,
            .limited = 0,
            .filter_policy = 0,
        };
        rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER,
                          &legacy_params, ble_gap_event_cb, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "ble_gap_disc() also failed: %d", rc);
        } else {
            ESP_LOGI(TAG, "BLE scanning started (legacy passive, continuous)");
        }
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
