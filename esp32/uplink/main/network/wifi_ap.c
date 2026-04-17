/**
 * Friend or Foe -- Uplink WiFi SoftAP Implementation
 *
 * Configures the AP interface for direct phone connections.
 * Loads SSID/password from NVS with MAC-based fallback.
 */

#include "wifi_ap.h"
#include "nvs_config.h"
#include "config.h"

#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "wifi_ap";

static int  s_station_count = 0;
static char s_ap_ssid[33]   = {0};
static esp_timer_handle_t s_ap_mode_timer = NULL;
static volatile bool s_ap_desired_state = true;  /* true=APSTA, false=STA-only */

/* Deferred mode switch — runs from timer task, safe to call esp_wifi_set_mode */
static void ap_mode_switch_cb(void *arg)
{
    (void)arg;
    if (s_ap_desired_state) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        /* Now apply AP config after mode includes AP */
        char password[65] = {0};
        nvs_config_get_ap_password(password, sizeof(password));
        wifi_config_t ap_config = {0};
        strncpy((char *)ap_config.ap.ssid, s_ap_ssid, sizeof(ap_config.ap.ssid) - 1);
        ap_config.ap.ssid_len = strlen(s_ap_ssid);
        strncpy((char *)ap_config.ap.password, password, sizeof(ap_config.ap.password) - 1);
        ap_config.ap.channel = CONFIG_AP_CHANNEL;
        ap_config.ap.max_connection = CONFIG_AP_MAX_CONNECTIONS;
        ap_config.ap.authmode = strlen(password) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        ap_config.ap.pmf_cfg.required = false;
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        ESP_LOGI(TAG, "WiFi mode → APSTA (AP enabled: SSID='%s')", s_ap_ssid);
    } else {
        esp_wifi_set_mode(WIFI_MODE_STA);
        ESP_LOGI(TAG, "WiFi mode → STA only (AP fully disabled)");
    }
}

/* ── Event handlers ────────────────────────────────────────────────────── */

static void ap_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *ev =
            (wifi_event_ap_staconnected_t *)event_data;
        s_station_count++;
        ESP_LOGI(TAG, "Station connected (AID=%d, total=%d)",
                 ev->aid, s_station_count);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *ev =
            (wifi_event_ap_stadisconnected_t *)event_data;
        if (s_station_count > 0) {
            s_station_count--;
        }
        ESP_LOGI(TAG, "Station disconnected (AID=%d, total=%d)",
                 ev->aid, s_station_count);
    }
}

/* ── Public API ────────────────────────────────────────────────────────── */

void wifi_ap_init(void)
{
    /* Create deferred mode switch timer */
    const esp_timer_create_args_t timer_args = {
        .callback = ap_mode_switch_cb,
        .name = "ap_mode",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_ap_mode_timer));

    /* Load AP credentials from NVS */
    char password[65] = {0};
    nvs_config_get_ap_ssid(s_ap_ssid, sizeof(s_ap_ssid));
    nvs_config_get_ap_password(password, sizeof(password));

    /* Register AP event handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED,
        &ap_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED,
        &ap_event_handler, NULL, NULL));

    /* Configure AP */
    wifi_config_t ap_config = {0};
    strncpy((char *)ap_config.ap.ssid, s_ap_ssid,
            sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(s_ap_ssid);
    strncpy((char *)ap_config.ap.password, password,
            sizeof(ap_config.ap.password) - 1);
    ap_config.ap.channel         = CONFIG_AP_CHANNEL;
    ap_config.ap.max_connection  = CONFIG_AP_MAX_CONNECTIONS;
    ap_config.ap.authmode        = WIFI_AUTH_WPA2_PSK;
    ap_config.ap.pmf_cfg.required = false;

    /* If password is empty, use open auth */
    if (strlen(password) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    ESP_LOGI(TAG, "WiFi AP initialized: SSID='%s', channel=%d, max_conn=%d",
             s_ap_ssid, CONFIG_AP_CHANNEL, CONFIG_AP_MAX_CONNECTIONS);
}

void wifi_ap_stop(void)
{
    /* Fully disable AP by switching to STA-only mode.
     * Deferred via timer — esp_wifi_set_mode is too heavy for event handler context. */
    s_ap_desired_state = false;
    esp_timer_stop(s_ap_mode_timer);
    esp_timer_start_once(s_ap_mode_timer, 200 * 1000);  /* 200ms delay */
    ESP_LOGI(TAG, "WiFi AP stop scheduled (switching to STA-only)");
}

void wifi_ap_start(void)
{
    /* Switch to APSTA mode first via deferred timer, then config will be set
     * by the timer callback after mode switch completes. */
    s_ap_desired_state = true;
    esp_timer_stop(s_ap_mode_timer);
    esp_timer_start_once(s_ap_mode_timer, 100 * 1000);  /* 100ms delay */
    ESP_LOGI(TAG, "WiFi AP starting: SSID='%s' (STA disconnected)", s_ap_ssid);
}

int wifi_ap_get_station_count(void)
{
    return s_station_count;
}

const char *wifi_ap_get_ssid(void)
{
    return s_ap_ssid;
}
