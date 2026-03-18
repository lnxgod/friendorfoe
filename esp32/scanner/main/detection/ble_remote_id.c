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
#include "open_drone_id_parser.h"
#include "constants.h"
#include "detection_types.h"
#include "core/task_priorities.h"

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

        /*
         * Search the advertisement data for service data with UUID 0xFFFA.
         *
         * BLE AD type 0x16 = Service Data - 16-bit UUID
         * Format: UUID_lo(1) + UUID_hi(1) + service_data(N)
         */
        struct ble_hs_adv_fields fields;
        int rc = ble_hs_adv_parse_fields(&fields, desc->data, desc->length_data);
        if (rc != 0) {
            break;
        }

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

        /*
         * Also check scan response data (some devices split ODID across
         * advertisement + scan response).
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
        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "BLE scan complete, restarting...");
        if (s_scanning) {
            /* Restart scanning */
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
    struct ble_gap_disc_params scan_params = {
        .passive = 0,           /* Active scanning to get scan responses */
        .itvl = 0x0010,         /* Scan interval (10ms in 0.625ms units = 10ms) */
        .window = 0x0010,       /* Scan window = interval = continuous scanning */
        .filter_duplicates = 0, /* Don't filter: we want every advertisement */
        .limited = 0,
        .filter_policy = 0,  /* Accept all (no whitelist filtering) */
    };

    int rc = ble_gap_disc(
        BLE_OWN_ADDR_PUBLIC,
        BLE_HS_FOREVER,         /* Scan indefinitely */
        &scan_params,
        ble_gap_event_cb,
        NULL
    );

    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc() failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "BLE scanning started (active, continuous)");
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
