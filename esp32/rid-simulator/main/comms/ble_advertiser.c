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
#include "os/os_mbuf.h"
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

/* ── BLE 5.0 Extended Advertising ─────────────────────────────────── */

#if CONFIG_BT_NIMBLE_EXT_ADV

static uint8_t s_ext_adv_instance = 0;  /* Extended advertising instance handle */

/**
 * Build extended advertising data with flags + service data (32 bytes).
 * BLE 5.0 Extended Advertising supports up to 251 bytes, so no 31-byte limit.
 */
static void set_ext_adv_data(int msg_idx)
{
    uint8_t buf[32];
    int pos = 0;

    /* Flags AD structure (3 bytes) */
    buf[pos++] = 2;       /* AD length */
    buf[pos++] = 0x01;   /* AD type: Flags */
    buf[pos++] = 0x06;   /* General discoverable + BR/EDR not supported */

    /* Service Data AD structure (29 bytes) */
    buf[pos++] = 28;     /* AD length = type(1) + uuid(2) + odid(25) */
    buf[pos++] = 0x16;   /* AD type: Service Data - 16-bit UUID */
    buf[pos++] = ODID_UUID16_LO;
    buf[pos++] = ODID_UUID16_HI;
    memcpy(&buf[pos], s_messages[msg_idx], ODID_MSG_SIZE);
    pos += ODID_MSG_SIZE;

    struct os_mbuf *data = os_msys_get_pkthdr(pos, 0);
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate mbuf for ext adv data");
        return;
    }
    os_mbuf_append(data, buf, pos);

    int rc = ble_gap_ext_adv_set_data(s_ext_adv_instance, data);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_ext_adv_set_data failed: %d", rc);
    }
}

static void start_ext_advertising(void)
{
    struct ble_gap_ext_adv_params params = {0};
    params.connectable = 0;         /* Non-connectable */
    params.scannable = 0;           /* Non-scannable (no scan response) */
    params.directed = 0;
    params.anonymous = 0;
    params.legacy_pdu = 1;          /* Use legacy PDU for BLE 4.x scanner compatibility */
    params.include_tx_power = 0;
    params.scan_req_notif = 0;
    params.itvl_min = 0x00A0;      /* 100ms */
    params.itvl_max = 0x00A0;
    params.channel_map = 0;         /* All channels */
    params.own_addr_type = BLE_OWN_ADDR_PUBLIC;
    params.primary_phy = BLE_HCI_LE_PHY_1M;
    params.secondary_phy = BLE_HCI_LE_PHY_1M;
    params.sid = 0;

    int rc = ble_gap_ext_adv_configure(s_ext_adv_instance, &params, NULL, NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_ext_adv_configure failed: %d", rc);
        return;
    }

    rc = ble_gap_ext_adv_start(s_ext_adv_instance, 0, 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_ext_adv_start failed: %d", rc);
    }
}

/* ── Advertising rotation task ───────────────────────────────────────── */

static void adv_rotate_task(void *arg)
{
    ESP_LOGI(TAG, "Extended advertisement rotation task started (BLE 5.0)");

    /* Wait for NimBLE sync */
    while (!s_synced) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* Configure and start extended advertising */
    start_ext_advertising();
    set_ext_adv_data(0);
    int current_idx = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(ADV_ROTATE_MS));

        /* Advance to next message type */
        current_idx = (current_idx + 1) % NUM_MSG_TYPES;
        atomic_store(&s_msg_index, current_idx);

        /* Stop → update data → restart */
        ble_gap_ext_adv_stop(s_ext_adv_instance);
        vTaskDelay(pdMS_TO_TICKS(5));
        set_ext_adv_data(current_idx);
        ble_gap_ext_adv_start(s_ext_adv_instance, 0, 0);

        uint32_t count = atomic_fetch_add(&s_adv_count, 1) + 1;
        if (count % 40 == 0) {
            ESP_LOGI(TAG, "ADV: %s (total=%lu)", s_msg_names[current_idx], (unsigned long)count);
        }
    }
}

#else /* Legacy BLE 4.x fallback — limited to 31 bytes (no flags) */

static void set_adv_data(int msg_idx)
{
    /* Build raw AD: service data only, no flags (saves 3 bytes to fit 31-byte limit) */
    uint8_t adv[29];
    adv[0] = 28;      /* AD length = type(1) + uuid(2) + odid(25) = 28 */
    adv[1] = 0x16;    /* AD type: Service Data - 16-bit UUID */
    adv[2] = ODID_UUID16_LO;
    adv[3] = ODID_UUID16_HI;
    memcpy(&adv[4], s_messages[msg_idx], ODID_MSG_SIZE);

    int rc = ble_gap_adv_set_data(adv, sizeof(adv));
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_data failed: %d", rc);
    }
}

static void start_advertising(void)
{
    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_NON,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min  = 0x00A0,
        .itvl_max  = 0x00A0,
    };
    int rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
    }
}

static void adv_rotate_task(void *arg)
{
    ESP_LOGI(TAG, "Legacy advertisement rotation task started (BLE 4.x)");
    while (!s_synced) vTaskDelay(pdMS_TO_TICKS(10));

    set_adv_data(0);
    start_advertising();
    int current_idx = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(ADV_ROTATE_MS));
        current_idx = (current_idx + 1) % NUM_MSG_TYPES;
        atomic_store(&s_msg_index, current_idx);
        ble_gap_adv_stop();
        vTaskDelay(pdMS_TO_TICKS(10));
        set_adv_data(current_idx);
        start_advertising();

        uint32_t count = atomic_fetch_add(&s_adv_count, 1) + 1;
        if (count % 40 == 0) {
            ESP_LOGI(TAG, "ADV: %s (total=%lu)", s_msg_names[current_idx], (unsigned long)count);
        }
    }
}

#endif /* CONFIG_BT_NIMBLE_EXT_ADV */

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
