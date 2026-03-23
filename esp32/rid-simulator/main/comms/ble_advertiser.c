/**
 * Friend or Foe — BLE ODID Advertiser
 *
 * NimBLE broadcaster that transmits OpenDroneID advertisements.
 * Cycles through 4 ODID message types (Basic ID, Location, System, Operator ID),
 * each advertised for ~100ms before switching.
 *
 * BLE advertisement payload (31 bytes max):
 *   [28 bytes] AD Length=27, AD Type=0x16, UUID16=0xFA 0xFF, [25 bytes ODID msg]
 *   [ 3 bytes] AD Length=2,  AD Type=0x01, Flags=0x06
 */

#include "ble_advertiser.h"
#include "core/task_priorities.h"

#include <string.h>
#include <stdatomic.h>
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ble_adv";

#define ODID_MSG_SIZE       25
#define ODID_UUID16_LO      0xFA
#define ODID_UUID16_HI      0xFF
#define NUM_MSG_TYPES       4
#define ADV_ROTATE_MS       100

/* ── Message buffers (double-buffered via copy) ──────────────────────── */

static uint8_t s_messages[NUM_MSG_TYPES][ODID_MSG_SIZE];
static atomic_int s_msg_index = 0;
static atomic_uint_least32_t s_adv_count = 0;
static volatile bool s_synced = false;

static const char *s_msg_names[NUM_MSG_TYPES] = {
    "BasicID", "Location", "System", "OperatorID"
};

/* ── Set advertisement data for current message ──────────────────────── */

static void set_adv_data(int msg_idx)
{
    /*
     * Build raw advertisement data manually:
     *   Flags AD:     len=2, type=0x01, flags=0x06
     *   Service Data: len=27, type=0x16, uuid16_lo, uuid16_hi, [25 bytes]
     */
    uint8_t adv_data[31];
    int pos = 0;

    /* Flags AD structure */
    adv_data[pos++] = 2;       /* AD length */
    adv_data[pos++] = 0x01;   /* AD type: Flags */
    adv_data[pos++] = 0x06;   /* General discoverable + BR/EDR not supported */

    /* Service Data AD structure */
    adv_data[pos++] = 27;     /* AD length: 1(type) + 2(uuid) + 25(odid) - 1 = 27 */
    adv_data[pos++] = 0x16;   /* AD type: Service Data - 16-bit UUID */
    adv_data[pos++] = ODID_UUID16_LO;
    adv_data[pos++] = ODID_UUID16_HI;
    memcpy(&adv_data[pos], s_messages[msg_idx], ODID_MSG_SIZE);
    pos += ODID_MSG_SIZE;

    /* Set the advertising data */
    int rc = ble_gap_adv_set_data(adv_data, pos);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_data failed: %d", rc);
    }
}

/* ── Start advertising ───────────────────────────────────────────────── */

static void start_advertising(void)
{
    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_NON,  /* Non-connectable */
        .disc_mode = BLE_GAP_DISC_MODE_GEN,  /* General discoverable */
        .itvl_min  = 0x00A0,  /* 100ms in 0.625ms units (160) */
        .itvl_max  = 0x00A0,
    };

    int rc = ble_gap_adv_start(
        BLE_OWN_ADDR_PUBLIC,
        NULL,               /* No directed advertising */
        BLE_HS_FOREVER,     /* Advertise indefinitely */
        &adv_params,
        NULL,               /* No event callback needed for non-connectable */
        NULL
    );

    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
    }
}

/* ── Advertising rotation task ───────────────────────────────────────── */

static void adv_rotate_task(void *arg)
{
    ESP_LOGI(TAG, "Advertisement rotation task started");

    /* Wait for NimBLE sync */
    while (!s_synced) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    while (1) {
        int idx = atomic_load(&s_msg_index);

        /* Stop current advertising, update data, restart */
        ble_gap_adv_stop();
        set_adv_data(idx);
        start_advertising();

        uint32_t count = atomic_fetch_add(&s_adv_count, 1) + 1;
        if (count % 40 == 0) {
            ESP_LOGI(TAG, "ADV: %s (total=%lu)", s_msg_names[idx], (unsigned long)count);
        }

        /* Advance to next message type */
        int next = (idx + 1) % NUM_MSG_TYPES;
        atomic_store(&s_msg_index, next);

        vTaskDelay(pdMS_TO_TICKS(ADV_ROTATE_MS));
    }
}

/* ── NimBLE callbacks ────────────────────────────────────────────────── */

static void ble_on_sync(void)
{
    ESP_LOGI(TAG, "NimBLE host synced, starting ODID broadcast");
    s_synced = true;
}

static void ble_host_task(void *arg)
{
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ── Public API ──────────────────────────────────────────────────────── */

void ble_advertiser_init(void)
{
    memset(s_messages, 0, sizeof(s_messages));

    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = NULL;

    ESP_LOGI(TAG, "BLE advertiser initialized");
}

void ble_advertiser_start(void)
{
    /* Start NimBLE host task */
    nimble_port_freertos_init(ble_host_task);

    /* Start advertisement rotation task */
#ifdef CONFIG_FREERTOS_UNICORE
    xTaskCreate(adv_rotate_task, "adv_rot", SIM_TASK_STACK_SIZE,
                NULL, SIM_TASK_PRIORITY, NULL);
#else
    xTaskCreatePinnedToCore(adv_rotate_task, "adv_rot", SIM_TASK_STACK_SIZE,
                            NULL, SIM_TASK_PRIORITY, NULL, SIM_TASK_CORE);
#endif

    ESP_LOGI(TAG, "BLE advertiser started");
}

void ble_advertiser_update_messages(const uint8_t *basic_id_msg,
                                     const uint8_t *location_msg,
                                     const uint8_t *system_msg,
                                     const uint8_t *operator_msg)
{
    if (basic_id_msg)  memcpy(s_messages[0], basic_id_msg,  ODID_MSG_SIZE);
    if (location_msg)  memcpy(s_messages[1], location_msg,  ODID_MSG_SIZE);
    if (system_msg)    memcpy(s_messages[2], system_msg,    ODID_MSG_SIZE);
    if (operator_msg)  memcpy(s_messages[3], operator_msg,  ODID_MSG_SIZE);
}

uint32_t ble_advertiser_get_adv_count(void)
{
    return atomic_load(&s_adv_count);
}
