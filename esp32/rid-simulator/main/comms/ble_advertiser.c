/**
 * Friend or Foe — BLE ODID Advertiser (parallel-instance edition)
 *
 * Runs up to BLE_ADV_MAX_DRONES concurrent NimBLE extended advertising
 * instances, one per simulated drone. Each instance independently cycles
 * through its own 4 ODID messages (Basic ID → Location → System → Operator)
 * at ADV_ROTATE_MS per message. Both drones are on-air simultaneously —
 * no multiplexed alternation that would leave one drone appearing "frozen"
 * from the scanner's perspective.
 *
 * The rotation is done in a single task to keep the stack simple; each
 * iteration steps every active instance one slot forward so message types
 * stay phase-aligned across drones (drone 0 Location fires at the same time
 * as drone 1 Location). If you want de-phased instances, stagger each
 * instance's initial `cur_slot`.
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

static const char *s_msg_names[NUM_MSG_TYPES] = {
    "BasicID", "Location", "System", "OperatorID"
};

/* Per-drone message buffers; each drone has its own 4 ODID messages. */
static uint8_t      s_drone_msgs[BLE_ADV_MAX_DRONES][NUM_MSG_TYPES][ODID_MSG_SIZE];
static atomic_int   s_cur_slot[BLE_ADV_MAX_DRONES];
static atomic_uint_least32_t s_adv_count = 0;
static volatile bool s_synced = false;
static int           s_num_drones = 0;

#if CONFIG_BT_NIMBLE_EXT_ADV

/* Each drone has its own advertising instance ID == drone_idx. */

static void set_ext_adv_data(int drone_idx, int msg_idx)
{
    uint8_t buf[32];
    int pos = 0;

    /* Flags AD structure */
    buf[pos++] = 2;
    buf[pos++] = 0x01;
    buf[pos++] = 0x06;

    /* Service Data AD structure: type(1) + uuid16(2) + ODID msg(25) = 28 bytes */
    buf[pos++] = 28;
    buf[pos++] = 0x16;
    buf[pos++] = ODID_UUID16_LO;
    buf[pos++] = ODID_UUID16_HI;
    memcpy(&buf[pos], s_drone_msgs[drone_idx][msg_idx], ODID_MSG_SIZE);
    pos += ODID_MSG_SIZE;

    struct os_mbuf *data = os_msys_get_pkthdr(pos, 0);
    if (data == NULL) {
        ESP_LOGE(TAG, "drone[%d] mbuf alloc failed", drone_idx);
        return;
    }
    os_mbuf_append(data, buf, pos);

    int rc = ble_gap_ext_adv_set_data((uint8_t)drone_idx, data);
    if (rc != 0) {
        ESP_LOGE(TAG, "drone[%d] ext_adv_set_data rc=%d", drone_idx, rc);
    }
}

static void configure_instance(int drone_idx)
{
    /* BLE 5 extended-PDU mode: legacy_pdu=0. Forced because two concurrent
     * advertising instances can't share SID=0 (which legacy PDU requires);
     * extended PDU lets each instance own a distinct SID. Modern scanners
     * and Android devices support this; ASTM F3411 Section 6 explicitly
     * allows BLE 5 Long Range and BLE 5 Extended for Remote ID. */
    struct ble_gap_ext_adv_params params = {0};
    params.connectable = 0;
    params.scannable = 0;
    params.directed = 0;
    params.anonymous = 0;
    params.legacy_pdu = 0;
    params.include_tx_power = 0;
    params.scan_req_notif = 0;
    params.itvl_min = 0x00A0;          /* 100 ms */
    params.itvl_max = 0x00A0;
    params.channel_map = 0;
    params.own_addr_type = BLE_OWN_ADDR_PUBLIC;
    params.primary_phy = BLE_HCI_LE_PHY_1M;
    params.secondary_phy = BLE_HCI_LE_PHY_1M;
    params.sid = (uint8_t)drone_idx;   /* distinct SID per instance */

    int rc = ble_gap_ext_adv_configure((uint8_t)drone_idx, &params, NULL, NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "drone[%d] ext_adv_configure rc=%d", drone_idx, rc);
    }
}

static void start_instance(int drone_idx)
{
    int rc = ble_gap_ext_adv_start((uint8_t)drone_idx, 0, 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "drone[%d] ext_adv_start rc=%d", drone_idx, rc);
    }
}

static void adv_rotate_task(void *arg)
{
    ESP_LOGI(TAG, "Parallel ext-adv rotation task started — %d drone(s)", s_num_drones);

    while (!s_synced) vTaskDelay(pdMS_TO_TICKS(10));

    /* Configure every instance once, push initial data, then start each. */
    for (int d = 0; d < s_num_drones; d++) {
        configure_instance(d);
        set_ext_adv_data(d, 0);
        start_instance(d);
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(ADV_ROTATE_MS));

        /* Step each instance to its next message slot. NimBLE accepts live
         * data updates on an active extended-advertising instance, so we
         * skip the stop/start churn — it was racing and rc=3 EINVAL'ing. */
        for (int d = 0; d < s_num_drones; d++) {
            int next = (atomic_load(&s_cur_slot[d]) + 1) % NUM_MSG_TYPES;
            atomic_store(&s_cur_slot[d], next);
            set_ext_adv_data(d, next);

            uint32_t count = atomic_fetch_add(&s_adv_count, 1) + 1;
            if (count % 80 == 0) {
                ESP_LOGI(TAG, "ADV drone[%d] %s total=%lu",
                         d, s_msg_names[next], (unsigned long)count);
            }
        }
    }
}

#else  /* Legacy BLE 4.x — only one drone at a time (fallback). */

static int s_legacy_active = 0;

static void set_adv_data(int drone_idx, int msg_idx)
{
    uint8_t adv[29];
    adv[0] = 28;
    adv[1] = 0x16;
    adv[2] = ODID_UUID16_LO;
    adv[3] = ODID_UUID16_HI;
    memcpy(&adv[4], s_drone_msgs[drone_idx][msg_idx], ODID_MSG_SIZE);
    ble_gap_adv_set_data(adv, sizeof(adv));
}

static void start_advertising(void)
{
    struct ble_gap_adv_params p = {
        .conn_mode = BLE_GAP_CONN_MODE_NON,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min  = 0x00A0,
        .itvl_max  = 0x00A0,
    };
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &p, NULL, NULL);
}

static void adv_rotate_task(void *arg)
{
    ESP_LOGW(TAG, "Legacy BLE 4.x — only one drone visible at a time");
    while (!s_synced) vTaskDelay(pdMS_TO_TICKS(10));
    set_adv_data(0, 0);
    start_advertising();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(ADV_ROTATE_MS));
        int next = (atomic_load(&s_cur_slot[s_legacy_active]) + 1) % NUM_MSG_TYPES;
        atomic_store(&s_cur_slot[s_legacy_active], next);

        /* Rotate active drone when we complete a full message cycle so each
         * drone still appears, just sequentially. */
        if (next == 0 && s_num_drones > 1) {
            s_legacy_active = (s_legacy_active + 1) % s_num_drones;
        }

        ble_gap_adv_stop();
        vTaskDelay(pdMS_TO_TICKS(10));
        set_adv_data(s_legacy_active, next);
        start_advertising();
        atomic_fetch_add(&s_adv_count, 1);
    }
}

#endif  /* CONFIG_BT_NIMBLE_EXT_ADV */

/* ── NimBLE callbacks ────────────────────────────────────────────────── */

static void ble_on_sync(void)
{
    ESP_LOGI(TAG, "NimBLE host synced");
    s_synced = true;
}

static void ble_host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ── Public API ──────────────────────────────────────────────────────── */

void ble_advertiser_init(int num_drones)
{
    if (num_drones < 1) num_drones = 1;
    if (num_drones > BLE_ADV_MAX_DRONES) num_drones = BLE_ADV_MAX_DRONES;
    s_num_drones = num_drones;

    memset(s_drone_msgs, 0, sizeof(s_drone_msgs));
    for (int i = 0; i < BLE_ADV_MAX_DRONES; i++) atomic_store(&s_cur_slot[i], 0);

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = NULL;

    ESP_LOGI(TAG, "advertiser init: %d drones, ext_adv=%s",
             s_num_drones,
#if CONFIG_BT_NIMBLE_EXT_ADV
             "yes"
#else
             "no (legacy BLE 4.x)"
#endif
             );
}

void ble_advertiser_start(void)
{
    nimble_port_freertos_init(ble_host_task);
#ifdef CONFIG_FREERTOS_UNICORE
    xTaskCreate(adv_rotate_task, "adv_rot", SIM_TASK_STACK_SIZE,
                NULL, SIM_TASK_PRIORITY, NULL);
#else
    xTaskCreatePinnedToCore(adv_rotate_task, "adv_rot", SIM_TASK_STACK_SIZE,
                            NULL, SIM_TASK_PRIORITY, NULL, SIM_TASK_CORE);
#endif
    ESP_LOGI(TAG, "advertiser started");
}

void ble_advertiser_update_drone(int drone_idx,
                                  const uint8_t *basic_id_msg,
                                  const uint8_t *location_msg,
                                  const uint8_t *system_msg,
                                  const uint8_t *operator_msg)
{
    if (drone_idx < 0 || drone_idx >= s_num_drones) return;
    if (basic_id_msg) memcpy(s_drone_msgs[drone_idx][0], basic_id_msg, ODID_MSG_SIZE);
    if (location_msg) memcpy(s_drone_msgs[drone_idx][1], location_msg, ODID_MSG_SIZE);
    if (system_msg)   memcpy(s_drone_msgs[drone_idx][2], system_msg,   ODID_MSG_SIZE);
    if (operator_msg) memcpy(s_drone_msgs[drone_idx][3], operator_msg, ODID_MSG_SIZE);
}

uint32_t ble_advertiser_get_adv_count(void)
{
    return atomic_load(&s_adv_count);
}
