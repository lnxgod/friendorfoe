/**
 * Friend or Foe -- Uplink WiFi Station Implementation
 *
 * WiFi STA with multi-SSID scanning and automatic reconnection.
 * On boot: scans for available networks, connects to the strongest
 * known SSID. On disconnect: tries next known SSID with backoff.
 * Connection state is signaled via an event group for synchronization.
 */

#include "wifi_sta.h"
#include "wifi_ap.h"
#include "nvs_config.h"
#include "config.h"
#include "time_sync.h"

#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi_sta";

/* Event group bits */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_timer_handle_t s_reconnect_timer  = NULL;
static bool               s_is_connected     = false;
static bool               s_standalone       = false;
static bool               s_scanning         = false;  /* true during initial multi-SSID scan */
static int                s_retry_count      = 0;

/* Multi-SSID credential list */
static const wifi_credential_t s_wifi_creds[] = CONFIG_WIFI_CREDENTIALS;
static int  s_current_cred_idx  = 0;    /* Index into s_wifi_creds */
static int  s_scan_attempts     = 0;

/* Exponential backoff parameters */
#define BACKOFF_BASE_MS     1000
#define BACKOFF_MAX_MS      60000

static int backoff_delay_ms(int retry)
{
    int delay = BACKOFF_BASE_MS;
    for (int i = 0; i < retry && delay < BACKOFF_MAX_MS; i++) {
        delay *= 2;
    }
    if (delay > BACKOFF_MAX_MS) {
        delay = BACKOFF_MAX_MS;
    }
    return delay;
}

/* ── Try connecting with a specific credential index ──────────────────── */

static void connect_with_cred(int idx)
{
    if (idx < 0 || idx >= CONFIG_WIFI_CREDENTIAL_COUNT) idx = 0;
    s_current_cred_idx = idx;

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, s_wifi_creds[idx].ssid,
            sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, s_wifi_creds[idx].password,
            sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e       = WPA3_SAE_PWE_BOTH;

    ESP_LOGI(TAG, "Trying SSID '%s' (idx=%d)", s_wifi_creds[idx].ssid, idx);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();
}

/* ── Scan for best known SSID ─────────────────────────────────────────── */

static int find_best_ssid(void)
{
    /* Do a quick scan */
    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time   = { .active = { .min = 100, .max = 300 } },
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true /* block */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        esp_wifi_scan_get_ap_records(&ap_count, NULL);
        ESP_LOGW(TAG, "No APs found in scan");
        return 0;
    }
    if (ap_count > 20) ap_count = 20;

    wifi_ap_record_t *ap_list = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_list) return 0;
    esp_wifi_scan_get_ap_records(&ap_count, ap_list);

    /* Find the strongest known SSID */
    int best_idx  = -1;
    int best_rssi = -999;
    for (int i = 0; i < ap_count; i++) {
        for (int j = 0; j < CONFIG_WIFI_CREDENTIAL_COUNT; j++) {
            if (strcmp((char *)ap_list[i].ssid, s_wifi_creds[j].ssid) == 0) {
                ESP_LOGI(TAG, "  Found '%s' RSSI=%d (cred %d)",
                         ap_list[i].ssid, ap_list[i].rssi, j);
                if (ap_list[i].rssi > best_rssi) {
                    best_rssi = ap_list[i].rssi;
                    best_idx  = j;
                }
            }
        }
    }
    free(ap_list);

    if (best_idx >= 0) {
        ESP_LOGI(TAG, "Best SSID: '%s' (RSSI=%d)", s_wifi_creds[best_idx].ssid, best_rssi);
    } else {
        ESP_LOGW(TAG, "No known SSIDs found in %d APs", ap_count);
    }
    return best_idx >= 0 ? best_idx : 0;
}

/* ── Reconnect timer callback (runs outside event loop) ───────────────── */

static void reconnect_timer_cb(void *arg)
{
    s_retry_count++;
    /* After 3 failures on same SSID, try next one */
    if (s_retry_count > 3) {
        s_current_cred_idx = (s_current_cred_idx + 1) % CONFIG_WIFI_CREDENTIAL_COUNT;
        s_retry_count = 0;
        ESP_LOGI(TAG, "Cycling to next SSID: '%s'", s_wifi_creds[s_current_cred_idx].ssid);
    }
    connect_with_cred(s_current_cred_idx);
}

/* ── Event handlers ────────────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_scanning) {
            ESP_LOGI(TAG, "WiFi STA started (scanning for networks, skip auto-connect)");
            return;
        }
        ESP_LOGI(TAG, "WiFi STA started, connecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_is_connected = false;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        if (s_scanning) {
            return;  /* Don't reconnect during initial multi-SSID scan */
        }

        int delay = backoff_delay_ms(s_retry_count);
        ESP_LOGW(TAG, "Disconnected (retry=%d), reconnecting in %dms...",
                 s_retry_count, delay);

        /* Re-enable AP for setup when STA is disconnected */
        wifi_ap_start();

        /* Schedule reconnect via timer — don't block the event loop */
        esp_timer_start_once(s_reconnect_timer, (uint64_t)delay * 1000);
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));

        s_is_connected = true;
        s_retry_count  = 0;

        /* Max TX power for best range to router */
        esp_wifi_set_max_tx_power(80);  /* 80 = 20dBm (units of 0.25dBm) */

        /* Disable AP — STA is connected, AP not needed */
        wifi_ap_stop();
        if (!time_sync_is_sntp_synced()) {
            time_sync_init();
        }
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ── Public API ────────────────────────────────────────────────────────── */

void wifi_sta_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    /* Create reconnect timer (one-shot, used for non-blocking backoff) */
    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_cb,
        .name     = "wifi_reconn",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));

    /* Initialize TCP/IP stack */
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    /* Initialize WiFi with default config */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL, NULL));

    /* Check if NVS has a manually-configured SSID first */
    char nvs_ssid[33] = {0};
    nvs_config_get_wifi_ssid(nvs_ssid, sizeof(nvs_ssid));

    if (nvs_ssid[0] == '\0' || strcmp(nvs_ssid, "YourSSID") == 0) {
        s_standalone = true;
        ESP_LOGI(TAG, "No WiFi SSID configured -- standalone mode (AP-only)");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_start());
        return;
    }

    s_scanning = true;  /* Suppress auto-connect/reconnect during scan */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Let WiFi radio fully initialize before scanning */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Scan for best known SSID from the multi-SSID list */
    ESP_LOGI(TAG, "Scanning for known WiFi networks (%d configured)...",
             CONFIG_WIFI_CREDENTIAL_COUNT);
    int best = find_best_ssid();
    s_scanning = false;

    if (best >= 0) {
        connect_with_cred(best);
    } else {
        /* No known networks found — try primary SSID as fallback */
        ESP_LOGW(TAG, "No known SSIDs found in scan, trying primary: %s", nvs_ssid);
        connect_with_cred(0);
    }

    ESP_LOGI(TAG, "WiFi STA initialized, trying SSID='%s'",
             s_wifi_creds[best].ssid);
}

bool wifi_sta_is_connected(void)
{
    return !s_standalone && s_is_connected;
}

bool wifi_sta_is_standalone(void)
{
    return s_standalone;
}

void wifi_sta_wait_connected(int timeout_ms)
{
    if (s_standalone) {
        ESP_LOGI(TAG, "Standalone mode -- skipping STA connection wait");
        return;
    }

    TickType_t ticks = (timeout_ms > 0) ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY;

    ESP_LOGI(TAG, "Waiting for WiFi connection (timeout=%dms)...", timeout_ms);

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT,
        pdFALSE,   /* don't clear on exit */
        pdTRUE,    /* wait for all bits */
        ticks);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
    } else {
        ESP_LOGW(TAG, "WiFi connection timeout after %dms", timeout_ms);
    }
}

int8_t wifi_sta_get_rssi(void)
{
    if (!s_is_connected) return 0;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

const char *wifi_sta_get_ssid(void)
{
    static char ssid_buf[33] = {0};
    if (!s_is_connected) return "";
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        strncpy(ssid_buf, (char *)ap_info.ssid, sizeof(ssid_buf) - 1);
        return ssid_buf;
    }
    return "";
}
