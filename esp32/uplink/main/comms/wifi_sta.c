/**
 * Friend or Foe -- Uplink WiFi Station Implementation
 *
 * WiFi STA with automatic reconnection and exponential backoff.
 * Connection state is signaled via an event group for synchronization.
 */

#include "wifi_sta.h"
#include "wifi_ap.h"
#include "nvs_config.h"
#include "config.h"

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
static int                s_retry_count      = 0;

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

/* ── Reconnect timer callback (runs outside event loop) ───────────────── */

static void reconnect_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "Reconnect timer fired, attempting connection...");
    s_retry_count++;
    esp_wifi_connect();
}

/* ── Event handlers ────────────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA started, connecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_is_connected = false;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

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

        /* Disable AP — STA is connected, AP not needed */
        wifi_ap_stop();
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

    /* Load WiFi credentials */
    char ssid[33]     = {0};
    char password[65] = {0};
    nvs_config_get_wifi_ssid(ssid, sizeof(ssid));
    nvs_config_get_wifi_password(password, sizeof(password));

    /* Check if SSID is unconfigured */
    if (ssid[0] == '\0' || strcmp(ssid, "YourSSID") == 0) {
        s_standalone = true;
        ESP_LOGI(TAG, "No WiFi SSID configured -- standalone mode (AP-only)");

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_start());
        return;
    }

    /* Configure and start */
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password,
            sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e       = WPA3_SAE_PWE_BOTH;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA initialized, SSID='%s'", ssid);
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
