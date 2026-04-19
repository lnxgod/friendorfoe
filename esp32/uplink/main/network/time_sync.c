/**
 * Friend or Foe -- Uplink SNTP Time Synchronization Implementation
 *
 * Uses ESP-IDF's SNTP component to synchronize the system clock.
 * Configures two NTP servers for redundancy.
 */

#include "time_sync.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"
#include "esp_log.h"

static const char *TAG = "time_sync";

static volatile bool s_synced = false;

/* ── SNTP sync notification callback ───────────────────────────────────── */

static void time_sync_notification_cb(struct timeval *tv)
{
    s_synced = true;

    time_t now = tv->tv_sec;
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);

    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &timeinfo);
    ESP_LOGI(TAG, "Time synchronized: %s", buf);
}

/* ── Public API ────────────────────────────────────────────────────────── */

void time_sync_init(void)
{
    ESP_LOGI(TAG, "Initializing SNTP...");

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);

    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");

    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);

    esp_sntp_init();

    ESP_LOGI(TAG, "SNTP initialized (servers: pool.ntp.org, time.google.com)");
}

bool time_sync_is_synced(void)
{
    return s_synced;
}

int64_t time_sync_get_epoch_ms(void)
{
    if (!s_synced) {
        return 0;
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000LL + (int64_t)tv.tv_usec / 1000LL;
}

void time_sync_set_from_backend(int64_t epoch_ms)
{
    /* SNTP-fallback path for networks that block outbound NTP. Called by
     * http_upload_task after polling GET /detections/time — the response
     * is authoritative enough for the fleet's internal correlation needs
     * (sub-50ms cluster window is what we're after, not atomic-clock
     * accuracy). Only writes the clock if SNTP hasn't already synced. */
    if (s_synced || epoch_ms <= 1700000000000LL) {
        return;
    }
    struct timeval tv = {
        .tv_sec  = (time_t)(epoch_ms / 1000LL),
        .tv_usec = (suseconds_t)((epoch_ms % 1000LL) * 1000LL),
    };
    settimeofday(&tv, NULL);
    s_synced = true;
    ESP_LOGW(TAG, "Time synced from BACKEND fallback: epoch_ms=%lld", (long long)epoch_ms);
}
