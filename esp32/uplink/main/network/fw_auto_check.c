#include "fw_auto_check.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef FW_AUTO_CHECK_HOST_TEST
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "cJSON.h"

#include "fw_store.h"
#include "wifi_sta.h"
#include "uart_rx.h"
#include "nvs_config.h"
#include "version.h"
#endif

/* ── Tunables ────────────────────────────────────────────────────────────── */

#define CHECK_INTERVAL_S        (30 * 60)   /* Routine cadence: 30 min */
#define FIRST_CHECK_DELAY_S     (2 * 60)    /* Wait for boot to settle */
#define BACKOFF_INITIAL_S       60
#define BACKOFF_MAX_S           (60 * 60)
#define HEAP_FLOOR_KB           50
#define HTTP_TIMEOUT_MS         10000
#define DOWNLOAD_TIMEOUT_MS     180000

/* ── Pure decision helpers ───────────────────────────────────────────────── */

bool fw_auto_check_decide(int free_heap_kb,
                          bool wifi_connected,
                          bool relay_active,
                          int64_t last_check_age_s,
                          int64_t check_interval_s)
{
    if (!wifi_connected) return false;
    if (relay_active) return false;        /* Manual flash takes priority */
    if (free_heap_kb < HEAP_FLOOR_KB) return false;
    /* last_check_age_s == 0 means "never checked" — always allow first check. */
    if (last_check_age_s != 0 && last_check_age_s < check_interval_s) return false;
    return true;
}

bool fw_auto_check_version_differs(const char *local, const char *remote)
{
    if (!remote || !remote[0]) return false;
    if (strcmp(remote, "unknown") == 0) return false;
    if (!local || !local[0]) return true;  /* No local sentinel → take remote */
    return strcmp(local, remote) != 0;
}

#ifndef FW_AUTO_CHECK_HOST_TEST

/* ── Runtime ─────────────────────────────────────────────────────────────── */

static const char *TAG = "fw_auto";

static const char  *s_status = "idle";
static int64_t      s_last_check_ms = 0;
static char         s_remote_uplink_ver[40] = {0};
static char         s_remote_scanner_ver[40] = {0};
static int64_t      s_backoff_s = 0;
static TaskHandle_t s_task = NULL;

const char *fw_auto_check_status(void)
{
    return s_status ? s_status : "idle";
}

int64_t fw_auto_check_last_age_s(void)
{
    if (s_last_check_ms == 0) return 0;
    int64_t now = esp_timer_get_time() / 1000;
    return (now - s_last_check_ms) / 1000;
}

const char *fw_auto_check_remote_uplink_version(void) { return s_remote_uplink_ver; }
const char *fw_auto_check_remote_scanner_version(void) { return s_remote_scanner_ver; }

/* HTTP body collection for esp_http_client. */
typedef struct {
    char    *buf;
    size_t   cap;
    size_t   len;
} http_collect_t;

static esp_err_t http_collect_event(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    http_collect_t *c = (http_collect_t *)evt->user_data;
    if (!c || !c->buf) return ESP_OK;
    int copy = evt->data_len;
    if (c->len + copy >= c->cap) copy = (int)(c->cap - c->len - 1);
    if (copy > 0) {
        memcpy(c->buf + c->len, evt->data, copy);
        c->len += copy;
    }
    return ESP_OK;
}

/**
 * GET <backend>/nodes/firmware/latest/<name>. Fills version + size.
 * Returns ESP_OK on 200, ESP_FAIL otherwise.
 */
static esp_err_t fetch_metadata(const char *backend_base, const char *name,
                                char *version_out, size_t version_cap,
                                int *size_out)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/nodes/firmware/latest/%s", backend_base, name);
    char body[512] = {0};
    http_collect_t collect = { .buf = body, .cap = sizeof(body), .len = 0 };
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_collect_event,
        .user_data = &collect,
        .timeout_ms = HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "GET %s → err=%s status=%d", url, esp_err_to_name(err), status);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGW(TAG, "Bad JSON from %s: %.80s", url, body);
        return ESP_FAIL;
    }
    const cJSON *jver  = cJSON_GetObjectItem(root, "version");
    const cJSON *jsize = cJSON_GetObjectItem(root, "size");
    if (cJSON_IsString(jver) && jver->valuestring) {
        strncpy(version_out, jver->valuestring, version_cap - 1);
        version_out[version_cap - 1] = '\0';
    } else {
        version_out[0] = '\0';
    }
    if (size_out) *size_out = cJSON_IsNumber(jsize) ? jsize->valueint : 0;
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * Stream-download <backend>/nodes/firmware/download/<name> directly into
 * the given OTA partition via esp_ota_*. On success returns ESP_OK and
 * `received_out` is filled with byte count.
 *
 * The caller is responsible for esp_ota_end / abort + boot decisions.
 */
static esp_err_t download_to_partition(const char *backend_base, const char *name,
                                       const esp_partition_t *partition,
                                       esp_ota_handle_t *ota_handle_out,
                                       int *received_out, uint32_t *crc_out)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/nodes/firmware/download/%s", backend_base, name);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = DOWNLOAD_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open %s: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    int total = (int)esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200 || total <= 0) {
        ESP_LOGE(TAG, "GET %s status=%d total=%d", url, status, total);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_INTERNAL);
    if (!buf) {
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int received = 0;
    uint32_t crc = 0;
    while (received < total) {
        int len = esp_http_client_read(client, (char *)buf, 4096);
        if (len <= 0) break;
        err = esp_ota_write(ota_handle, buf, len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write at %d: %s", received, esp_err_to_name(err));
            free(buf);
            esp_ota_abort(ota_handle);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return err;
        }
        crc = esp_rom_crc32_le(crc, buf, len);
        received += len;
        if ((received % (256 * 1024)) < 4096) {
            ESP_LOGI(TAG, "auto-check %s: %d/%d (%.0f%%)",
                     name, received, total, (float)received / total * 100);
        }
    }
    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (received != total) {
        ESP_LOGE(TAG, "auto-check %s: short read %d/%d", name, received, total);
        esp_ota_abort(ota_handle);
        return ESP_FAIL;
    }
    if (ota_handle_out) *ota_handle_out = ota_handle;
    if (received_out)   *received_out   = received;
    if (crc_out)        *crc_out        = crc;
    return ESP_OK;
}

/* Try to update the uplink itself. Returns ESP_OK if a reboot is imminent. */
static esp_err_t try_self_update_uplink(const char *backend_base)
{
    char remote_ver[40] = {0};
    int  remote_size = 0;
    esp_err_t err = fetch_metadata(backend_base, "uplink-s3",
                                   remote_ver, sizeof(remote_ver), &remote_size);
    if (err != ESP_OK) return err;
    strncpy(s_remote_uplink_ver, remote_ver, sizeof(s_remote_uplink_ver) - 1);
    s_remote_uplink_ver[sizeof(s_remote_uplink_ver) - 1] = '\0';

    if (!fw_auto_check_version_differs(FOF_VERSION, remote_ver)) {
        ESP_LOGI(TAG, "uplink-s3: local=%s remote=%s — up to date",
                 FOF_VERSION, remote_ver);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "uplink-s3: local=%s remote=%s — self-OTA starting (%d bytes)",
             FOF_VERSION, remote_ver, remote_size);
    s_status = "updating";

    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next) return ESP_FAIL;

    esp_ota_handle_t handle = 0;
    int received = 0;
    uint32_t crc = 0;
    err = download_to_partition(backend_base, "uplink-s3", next, &handle, &received, &crc);
    if (err != ESP_OK) {
        s_status = "error:download";
        return err;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        s_status = "error:ota_end";
        return err;
    }
    err = esp_ota_set_boot_partition(next);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        s_status = "error:set_boot";
        return err;
    }

    ESP_LOGW(TAG, "uplink-s3 OTA complete (%d bytes, crc=%lu) — restarting",
             received, (unsigned long)crc);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;  /* unreachable */
}

/**
 * Pick the right scanner firmware name based on what's connected.
 * Returns NULL if no scanner identified yet.
 */
static const char *connected_scanner_board(void)
{
    const scanner_info_t *ble = uart_rx_get_ble_scanner_info();
    if (ble && ble->received && ble->board[0]) return ble->board;
    const scanner_info_t *wifi = uart_rx_get_wifi_scanner_info();
    if (wifi && wifi->received && wifi->board[0]) return wifi->board;
    return NULL;
}

/* Refresh the fw_store cache for the connected scanner variant. */
static esp_err_t try_refresh_scanner_cache(const char *backend_base)
{
    const char *board = connected_scanner_board();
    if (!board) {
        ESP_LOGI(TAG, "scanner board unknown — skipping cache refresh");
        return ESP_OK;
    }
    /* Sanity: only refresh known scanner variants, never random strings. */
    if (strcmp(board, "scanner-s3-combo") != 0 &&
        strcmp(board, "scanner-s3-combo-seed") != 0) {
        ESP_LOGW(TAG, "ignoring unknown scanner board '%s'", board);
        return ESP_OK;
    }

    char remote_ver[40] = {0};
    int  remote_size = 0;
    esp_err_t err = fetch_metadata(backend_base, board,
                                   remote_ver, sizeof(remote_ver), &remote_size);
    if (err != ESP_OK) return err;
    strncpy(s_remote_scanner_ver, remote_ver, sizeof(s_remote_scanner_ver) - 1);
    s_remote_scanner_ver[sizeof(s_remote_scanner_ver) - 1] = '\0';

    fw_store_info_t info = {0};
    fw_store_get_info(&info);

    bool need_refresh = !info.stored ||
                        strcmp(info.name, board) != 0 ||
                        fw_auto_check_version_differs(info.version, remote_ver);
    if (!need_refresh) {
        ESP_LOGI(TAG, "%s: cached=%s remote=%s — fw_store up to date",
                 board, info.version, remote_ver);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "%s: cached=%s remote=%s — refreshing fw_store (%d bytes)",
             board, info.stored ? info.version : "(empty)", remote_ver, remote_size);

    /* fw_store owns the partition selection — get the same target the
     * /api/fw/upload handler would use, so the cached image lives where
     * existing fw_check / fw_offer / fw_ready code expects to find it. */
    const esp_partition_t *p = fw_store_get_target_partition();
    if (!p) {
        ESP_LOGE(TAG, "fw_store_get_target_partition returned NULL");
        return ESP_FAIL;
    }

    esp_ota_handle_t handle = 0;
    int received = 0;
    uint32_t crc = 0;
    err = download_to_partition(backend_base, board, p,
                                &handle, &received, &crc);
    if (err != ESP_OK) return err;

    /* Critical: do NOT esp_ota_end/set_boot — that would make the scanner
     * firmware bootable as the uplink's next image, bricking the uplink.
     * fw_upload_handler likewise calls esp_ota_abort here. */
    esp_ota_abort(handle);

    fw_store_persist_metadata(board, remote_ver, p,
                              (uint32_t)received, crc);
    ESP_LOGW(TAG, "fw_store refreshed: %s v%s (%d bytes, crc=%lu) on '%s'",
             board, remote_ver, received, (unsigned long)crc, p->label);
    return ESP_OK;
}

static void auto_check_task(void *arg)
{
    (void)arg;
    /* Settle period — give the uplink time to mark itself valid before we
     * potentially OTA over it. */
    vTaskDelay(pdMS_TO_TICKS(FIRST_CHECK_DELAY_S * 1000));

    while (1) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        int64_t age_s = (s_last_check_ms == 0)
                            ? 0
                            : (now_ms - s_last_check_ms) / 1000;
        int interval_s = (int)CHECK_INTERVAL_S;
        if (s_backoff_s > 0 && s_backoff_s > interval_s) {
            interval_s = (int)s_backoff_s;
        }

        bool wifi_ok = wifi_sta_is_connected();
        bool relay_active = fw_store_is_relay_active();
        int free_kb = (int)(esp_get_free_heap_size() / 1024);

        if (!fw_auto_check_decide(free_kb, wifi_ok, relay_active,
                                  age_s, interval_s)) {
            vTaskDelay(pdMS_TO_TICKS(60 * 1000));
            continue;
        }

        char backend[128] = {0};
        nvs_config_get_backend_url(backend, sizeof(backend));
        if (!backend[0]) {
            vTaskDelay(pdMS_TO_TICKS(60 * 1000));
            continue;
        }

        s_status = "checking";
        s_last_check_ms = esp_timer_get_time() / 1000;

        esp_err_t err = try_self_update_uplink(backend);
        /* If we got here, no self-update happened — try the scanner cache. */
        if (err == ESP_OK) {
            err = try_refresh_scanner_cache(backend);
        }

        if (err == ESP_OK) {
            s_status = "idle";
            s_backoff_s = 0;
        } else {
            s_backoff_s = (s_backoff_s == 0)
                              ? BACKOFF_INITIAL_S
                              : (s_backoff_s * 2);
            if (s_backoff_s > BACKOFF_MAX_S) s_backoff_s = BACKOFF_MAX_S;
            ESP_LOGW(TAG, "auto-check failed; backing off %llds",
                     (long long)s_backoff_s);
        }

        vTaskDelay(pdMS_TO_TICKS(60 * 1000));
    }
}

void fw_auto_check_init(void)
{
    if (s_task != NULL) return;
    BaseType_t ok = xTaskCreatePinnedToCore(
        auto_check_task, "fw_auto", 6144, NULL,
        tskIDLE_PRIORITY + 2, &s_task, tskNO_AFFINITY);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "fw_auto_check task create failed");
        s_task = NULL;
    } else {
        ESP_LOGW(TAG, "fw_auto_check task started; first check in %ds, then every %ds",
                 FIRST_CHECK_DELAY_S, CHECK_INTERVAL_S);
    }
}

#endif /* FW_AUTO_CHECK_HOST_TEST */
