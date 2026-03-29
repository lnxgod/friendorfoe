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

static const char *TAG = "wifi_ap";

static int  s_station_count = 0;
static char s_ap_ssid[33]   = {0};

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
    /* Hide AP by setting SSID to empty and disabling beacon */
    wifi_config_t ap_config = {0};
    ap_config.ap.ssid_hidden = 1;
    ap_config.ap.ssid_len = 0;
    ap_config.ap.max_connection = 0;
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    ESP_LOGI(TAG, "WiFi AP stopped (STA connected, AP not needed)");
}

void wifi_ap_start(void)
{
    /* Restore AP with original SSID */
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
    ESP_LOGI(TAG, "WiFi AP restarted: SSID='%s' (STA disconnected, AP needed for setup)", s_ap_ssid);
}

int wifi_ap_get_station_count(void)
{
    return s_station_count;
}

const char *wifi_ap_get_ssid(void)
{
    return s_ap_ssid;
}
