/* Standalone, receive-only probe scanner. No associations, active scans, BLE,
 * network credentials, backend, or raw-frame storage. USB uses TinyUSB CDC,
 * separate from the full badge's Serial/JTAG identity. */
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "tusb.h"
#include "cJSON.h"
#include "wifi_probe_capture.h"

#define PROTOCOL_VERSION 1
#define FIRMWARE_VERSION "1.0.0"
#define MAX_REPORTS_PER_SECOND 20

typedef struct {
    char mac[18];
    char ssid[33];
    bool wildcard;
    bool binary;
    int rssi;
    int channel;
    int64_t ms;
} probe_t;

/* Advertise the full USB 2.0 bus-power budget for the S3 radio and board.
 * The generic TinyUSB descriptor requests only 100 mA. No remote wakeup. */
static const uint8_t s_usb_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN, 0, 500),
    TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, 64),
};

static QueueHandle_t s_queue;
static atomic_uint s_dropped;
static char s_sensor[32];
static char s_boot[17];
static unsigned s_sequence;
static uint8_t s_channel = 1;

static void on_packet(void *buffer, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *packet = buffer;
    if (packet->rx_ctrl.rx_state != 0 || packet->rx_ctrl.sig_len < 30) return;
    probe_t probe = {0};
    /* ESP-IDF sig_len includes the four-byte FCS. */
    if (!fof_probe_ssid(packet->payload, packet->rx_ctrl.sig_len - 4,
                        probe.ssid, &probe.wildcard, &probe.binary)) return;
    const uint8_t *mac = packet->payload + 10;
    if (!(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5])) return;
    probe.ms = esp_timer_get_time() / 1000;
    /* Only this callback accesses the budget; never block the radio task. */
    static int64_t window_ms;
    static unsigned count;
    if (probe.ms - window_ms >= 1000) { window_ms = probe.ms; count = 0; }
    if (count++ >= MAX_REPORTS_PER_SECOND) { atomic_fetch_add(&s_dropped, 1); return; }
    snprintf(probe.mac, sizeof(probe.mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    probe.rssi = packet->rx_ctrl.rssi;
    probe.channel = packet->rx_ctrl.channel;
    if (xQueueSend(s_queue, &probe, 0) != pdTRUE) atomic_fetch_add(&s_dropped, 1);
}

/* Single writer; enqueue a whole newline-delimited message or drop it. */
static bool send_json(cJSON *json)
{
    char line[512];
    if (!json) return false;
    bool encoded = cJSON_PrintPreallocated(json, line, sizeof(line) - 2, false);
    cJSON_Delete(json);
    if (!encoded || !tud_cdc_connected()) return false;
    size_t size = strlen(line);
    line[size++] = '\n';
    if (tud_cdc_write_available() < size) return false;
    bool written = tud_cdc_write(line, size) == size;
    tud_cdc_write_flush();
    return written;
}

static cJSON *message(const char *type)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return NULL;
    cJSON_AddStringToObject(json, "type", type);
    cJSON_AddNumberToObject(json, "protocol", PROTOCOL_VERSION);
    cJSON_AddStringToObject(json, "boot", s_boot);
    return json;
}

void app_main(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(s_sensor, sizeof(s_sensor), "usb-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_boot, sizeof(s_boot), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    const char *strings[] = { (const char[]){0x09, 0x04}, "Friend or Foe", "FoF WiFi Probe", s_sensor, "Probe CDC" };
    tinyusb_config_t usb = { .string_descriptor = strings, .string_descriptor_count = 5,
                             .configuration_descriptor = s_usb_configuration };
    ESP_ERROR_CHECK(tinyusb_driver_install(&usb));
    tinyusb_config_cdcacm_t cdc = { .usb_dev = TINYUSB_USBDEV_0, .cdc_port = TINYUSB_CDC_ACM_0,
                                  .rx_unread_buf_sz = 64 };
    ESP_ERROR_CHECK(tusb_cdc_acm_init(&cdc));
    s_queue = xQueueCreate(64, sizeof(probe_t));
    configASSERT(s_queue);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t wifi = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    const wifi_country_t country = { .cc = "01", .schan = 1, .nchan = 13, .policy = WIFI_COUNTRY_POLICY_MANUAL };
    ESP_ERROR_CHECK(esp_wifi_set_country(&country));
    wifi_promiscuous_filter_t filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(on_packet));
    ESP_ERROR_CHECK(esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    int64_t heartbeat_ms = -2000, hop_ms = 0;
    bool was_connected = false;
    for (;;) {
        int64_t now = esp_timer_get_time() / 1000;
        bool connected = tud_cdc_connected();
        if (connected && (!was_connected || now - heartbeat_ms >= 2000)) {
            cJSON *hello = message("hello");
            cJSON_AddStringToObject(hello, "firmware", "fof-wifi-probe");
            cJSON_AddStringToObject(hello, "version", FIRMWARE_VERSION);
            cJSON_AddStringToObject(hello, "sensor_id", s_sensor);
            cJSON_AddNumberToObject(hello, "uptime_ms", now);
            cJSON_AddNumberToObject(hello, "channel", s_channel);
            cJSON_AddNumberToObject(hello, "dropped", atomic_load(&s_dropped));
            if (send_json(hello)) heartbeat_ms = now;
        }
        was_connected = connected;
        if (now - hop_ms >= 250) {
            s_channel = s_channel % 13 + 1;
            ESP_ERROR_CHECK(esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE));
            hop_ms = now;
        }
        probe_t probe;
        if (xQueueReceive(s_queue, &probe, pdMS_TO_TICKS(10)) != pdTRUE) continue;
        if (!connected || now - probe.ms > 1000) continue;
        cJSON *json = message("probe");
        cJSON_AddNumberToObject(json, "seq", ++s_sequence);
        cJSON_AddNumberToObject(json, "uptime_ms", probe.ms);
        cJSON_AddStringToObject(json, "mac", probe.mac);
        cJSON_AddStringToObject(json, "ssid", probe.ssid);
        cJSON_AddBoolToObject(json, "wildcard", probe.wildcard);
        cJSON_AddBoolToObject(json, "binary", probe.binary);
        cJSON_AddNumberToObject(json, "rssi", probe.rssi);
        cJSON_AddNumberToObject(json, "channel", probe.channel);
        if (!send_json(json)) atomic_fetch_add(&s_dropped, 1);
    }
}
