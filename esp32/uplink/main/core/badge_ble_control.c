#include "badge_ble_control.h"

#ifdef FOF_BADGE_VARIANT

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "oled_display.h"
#include "badge_display_policy_runtime.h"
#include "badge_theme_runtime.h"
#include "version.h"
#include "cJSON.h"

#if defined(CONFIG_BT_ENABLED) && CONFIG_BT_ENABLED && \
    defined(CONFIG_BT_NIMBLE_ENABLED) && CONFIG_BT_NIMBLE_ENABLED
#include "esp_bt.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#endif

static const char *TAG = "badge_ble";

static bool s_enabled = false;
static bool s_started = false;
static bool s_pairing_open = false;
static bool s_connected = false;
static bool s_bonded = false;
static bool s_encrypted = false;
static int64_t s_pairing_started_ms = 0;
static char s_last_error[64] = "";
static uint32_t s_rx_count = 0;
static uint32_t s_tx_count = 0;

#define BADGE_BLE_PAIRING_WINDOW_MS (10000)

static int64_t badge_ble_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void badge_ble_set_error(const char *error)
{
    snprintf(s_last_error, sizeof(s_last_error), "%s", error ? error : "");
}

bool badge_ble_control_pairing_active(void)
{
    if (!s_pairing_open || s_pairing_started_ms <= 0) {
        return false;
    }
    if ((badge_ble_now_ms() - s_pairing_started_ms) > BADGE_BLE_PAIRING_WINDOW_MS) {
        s_pairing_open = false;
        return false;
    }
    return true;
}

size_t badge_ble_control_status_json(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return 0;
    }
    bool pairing = badge_ble_control_pairing_active();
    int64_t age_s = pairing && s_pairing_started_ms > 0
        ? (badge_ble_now_ms() - s_pairing_started_ms) / 1000
        : -1;
    int n = snprintf(out, out_len,
                     "{\"enabled\":%s,\"bonded\":%s,\"pairing_age_s\":%lld,"
                     "\"pairing_window_s\":10,\"connected\":%s,"
                     "\"encrypted\":%s,\"last_error\":\"%s\",\"rx\":%lu,\"tx\":%lu}",
                     s_enabled ? "true" : "false",
                     s_bonded ? "true" : "false",
                     (long long)age_s,
                     s_connected ? "true" : "false",
                     s_encrypted ? "true" : "false",
                     s_last_error,
                     (unsigned long)s_rx_count,
                     (unsigned long)s_tx_count);
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    if ((size_t)n >= out_len) {
        out[out_len - 1] = '\0';
        return out_len - 1;
    }
    return (size_t)n;
}

#if defined(CONFIG_BT_ENABLED) && CONFIG_BT_ENABLED && \
    defined(CONFIG_BT_NIMBLE_ENABLED) && CONFIG_BT_NIMBLE_ENABLED

#define BADGE_BLE_STATUS_UUID 0xFF01
#define BADGE_BLE_CONTROL_UUID 0xFF02

static uint8_t s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

static int badge_ble_gap_event(struct ble_gap_event *event, void *arg);
static void badge_ble_advertise(void);

static bool badge_ble_connection_authorized(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    int rc = ble_gap_conn_find(conn_handle, &desc);
    if (rc != 0) {
        return false;
    }
    s_encrypted = desc.sec_state.encrypted;
    s_bonded = desc.sec_state.bonded || s_bonded;
    return desc.sec_state.encrypted && (desc.sec_state.bonded || s_bonded);
}

static int badge_ble_status_access(uint16_t conn_handle,
                                   uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt,
                                   void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    char ble_json[224];
    char theme_json[BADGE_THEME_JSON_MAX];
    badge_ble_control_status_json(ble_json, sizeof(ble_json));
    badge_theme_runtime_json(theme_json, sizeof(theme_json));
    char json[960];
    snprintf(json, sizeof(json),
             "{\"version\":\"%s\",\"mode\":\"ble\","
             "\"mode_label\":\"BLE Tether\",\"theme_hash\":%lu,"
             "\"theme\":%s,\"ble_control\":%s}",
             FOF_VERSION,
             (unsigned long)badge_theme_runtime_hash(),
             theme_json[0] ? theme_json : "{\"version\":1}",
             ble_json[0] ? ble_json : "{\"enabled\":true}");
    int rc = os_mbuf_append(ctxt->om, json, strlen(json));
    if (rc == 0) {
        s_tx_count++;
    }
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static void badge_ble_control_reply(uint16_t conn_handle, const char *json)
{
    (void)conn_handle;
    ESP_LOGI(TAG, "BLE control reply: %s", json ? json : "{}");
}

static int badge_ble_control_access(uint16_t conn_handle,
                                    uint16_t attr_handle,
                                    struct ble_gatt_access_ctxt *ctxt,
                                    void *arg)
{
    (void)attr_handle;
    (void)arg;
    if (!badge_ble_connection_authorized(conn_handle)) {
        badge_ble_set_error("pair phone first");
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    char buf[1024];
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len >= sizeof(buf)) {
        badge_ble_set_error("control too large");
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, &len);
    if (rc != 0) {
        badge_ble_set_error("control read failed");
        return BLE_ATT_ERR_UNLIKELY;
    }
    buf[len] = '\0';
    s_rx_count++;

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        badge_ble_set_error("invalid json");
        badge_ble_control_reply(conn_handle, "{\"ok\":false,\"error\":\"invalid json\"}");
        return 0;
    }
    const cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    const char *cmd = cJSON_IsString(cmd_item) ? cmd_item->valuestring : "";
    bool ok = false;
    const char *reply = "unsupported ble command";
    if (strcmp(cmd, "display_nav") == 0) {
        const cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
        ok = cJSON_IsString(action) &&
            oled_badge_handle_nav_command(action->valuestring);
        badge_ble_set_error(ok ? "" : "invalid display nav");
        reply = ok ? "display nav updated" : "invalid display nav";
    } else if (strcmp(cmd, "badge_display_policy") == 0) {
        const cJSON *policy_item = cJSON_GetObjectItemCaseSensitive(root, "policy");
        char *policy_json = policy_item ? cJSON_PrintUnformatted(policy_item) : NULL;
        badge_display_policy_t policy;
        char err[64] = {0};
        bool parsed = policy_json &&
            badge_display_policy_parse_json(policy_json, &policy, err, sizeof(err));
        if (policy_json) {
            cJSON_free(policy_json);
        }
        bool persist = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "persist"));
        ok = parsed && badge_display_policy_runtime_set(&policy, persist);
        badge_ble_set_error(ok ? "" : (err[0] ? err : "invalid display policy"));
        reply = ok ? "display policy updated" : s_last_error;
    } else if (strcmp(cmd, "badge_display_policy_reset") == 0) {
        bool persist = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "persist"));
        badge_display_policy_runtime_reset(persist);
        ok = true;
        badge_ble_set_error("");
        reply = "display policy reset";
    } else if (strcmp(cmd, "badge_theme") == 0) {
        const cJSON *theme_item = cJSON_GetObjectItemCaseSensitive(root, "theme");
        char *theme_json = theme_item ? cJSON_PrintUnformatted(theme_item) : NULL;
        badge_theme_t theme;
        char err[64] = {0};
        bool parsed = theme_json &&
            badge_theme_parse_json(theme_json, &theme, err, sizeof(err));
        if (theme_json) {
            cJSON_free(theme_json);
        }
        bool persist = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "persist"));
        ok = parsed && badge_theme_runtime_set(&theme, persist);
        badge_ble_set_error(ok ? "" : (err[0] ? err : "invalid badge theme"));
        reply = ok ? "badge theme updated" : s_last_error;
    } else if (strcmp(cmd, "badge_theme_reset") == 0) {
        bool persist = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "persist"));
        badge_theme_runtime_reset(persist);
        ok = true;
        badge_ble_set_error("");
        reply = "badge theme reset";
    } else {
        badge_ble_set_error("unsupported ble command");
    }
    cJSON_Delete(root);
    char resp[160];
    snprintf(resp, sizeof(resp),
             ok ? "{\"ok\":true,\"message\":\"%s\"}"
                : "{\"ok\":false,\"error\":\"%s\"}",
             reply ? reply : (ok ? "ok" : "error"));
    badge_ble_control_reply(conn_handle, resp);
    return 0;
}

static const struct ble_gatt_svc_def s_badge_ble_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0xF0F0),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(BADGE_BLE_STATUS_UUID),
                .access_cb = badge_ble_status_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = BLE_UUID16_DECLARE(BADGE_BLE_CONTROL_UUID),
                .access_cb = badge_ble_control_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                         BLE_GATT_CHR_F_WRITE_ENC,
            },
            { 0 },
        },
    },
    { 0 },
};

static void badge_ble_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        badge_ble_set_error("addr infer failed");
        ESP_LOGE(TAG, "addr infer failed: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "BLE control host synced");
}

static void badge_ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void badge_ble_control_init(void)
{
    if (s_started) {
        return;
    }
    s_enabled = true;
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        badge_ble_set_error("nimble init failed");
        ESP_LOGE(TAG, "NimBLE init failed: %s", esp_err_to_name(err));
        return;
    }
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("FoF Badge");
    ble_hs_cfg.sync_cb = badge_ble_on_sync;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;

    int rc = ble_gatts_count_cfg(s_badge_ble_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_badge_ble_svcs);
    }
    if (rc != 0) {
        badge_ble_set_error("gatt init failed");
        ESP_LOGE(TAG, "GATT init failed: %d", rc);
        return;
    }
    nimble_port_freertos_init(badge_ble_host_task);
    s_started = true;
    badge_ble_set_error("");
    ESP_LOGI(TAG, "Badge BLE control ready; advertising starts from BTN2 pairing");
}

bool badge_ble_control_open_pairing_window(void)
{
    if (!s_started) {
        badge_ble_control_init();
    }
    if (!s_started) {
        return false;
    }
    s_pairing_open = true;
    s_pairing_started_ms = badge_ble_now_ms();
    badge_ble_advertise();
    return true;
}

static void badge_ble_advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    const char *name = "FoF Badge";
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    fields.uuids16 = (ble_uuid16_t[]) { BLE_UUID16_INIT(0xF0F0) };
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        badge_ble_set_error("adv fields failed");
        return;
    }

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    int32_t duration = badge_ble_control_pairing_active()
        ? BADGE_BLE_PAIRING_WINDOW_MS
        : BLE_HS_FOREVER;
    rc = ble_gap_adv_start(s_own_addr_type, NULL,
                           duration,
                           &params, badge_ble_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        badge_ble_set_error("adv start failed");
        ESP_LOGE(TAG, "adv start failed: %d", rc);
    } else {
        badge_ble_set_error("");
        ESP_LOGI(TAG, "BLE advertising %s",
                 duration == BLE_HS_FOREVER ? "for bonded reconnect" : "for pairing window");
    }
}

static int badge_ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_connected = true;
                s_conn_handle = event->connect.conn_handle;
                s_encrypted = false;
                s_pairing_open = false;
                badge_ble_set_error("");
            } else {
                s_connected = false;
                s_encrypted = false;
                s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                if (badge_ble_control_pairing_active()) {
                    badge_ble_advertise();
                }
            }
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            s_connected = false;
            s_encrypted = false;
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            if (badge_ble_control_pairing_active() || s_bonded) {
                badge_ble_advertise();
            }
            return 0;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            s_pairing_open = false;
            return 0;
        case BLE_GAP_EVENT_ENC_CHANGE:
            if (event->enc_change.status == 0) {
                (void)badge_ble_connection_authorized(event->enc_change.conn_handle);
                badge_ble_set_error("");
            } else {
                s_encrypted = false;
                badge_ble_set_error("encryption failed");
            }
            return 0;
        default:
            return 0;
    }
}

#else

void badge_ble_control_init(void)
{
    s_enabled = false;
    badge_ble_set_error("ble disabled");
}

bool badge_ble_control_open_pairing_window(void)
{
    s_enabled = false;
    badge_ble_set_error("ble disabled");
    return false;
}

#endif

#endif /* FOF_BADGE_VARIANT */
