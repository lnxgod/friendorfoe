/**
 * Friend or Foe -- Uplink SNTP Time Synchronization Implementation
 *
 * Uses ESP-IDF's SNTP component to synchronize the system clock.
 * Configures two NTP servers for redundancy.
 */

#include "time_sync.h"
#include "time_sync_policy.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "time_sync";

static volatile bool s_synced = false;
static volatile bool s_sntp_synced = false;
static volatile bool s_backend_authoritative = false;
static volatile bool s_started = false;
static volatile int64_t s_last_backend_sync_monotonic_ms = 0;

/* ── SNTP sync notification callback ───────────────────────────────────── */

static void time_sync_notification_cb(struct timeval *tv)
{
    s_synced = true;
    s_sntp_synced = true;
    s_backend_authoritative = false;

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
    if (s_sntp_synced) {
        return;
    }
    if (s_started) {
        ESP_LOGI(TAG, "Reinitializing SNTP...");
        esp_sntp_stop();
        s_started = false;
    }

    ESP_LOGI(TAG, "Initializing SNTP...");

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);

    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");

    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);

    esp_sntp_init();
    s_started = true;

    ESP_LOGI(TAG, "SNTP initialized (servers: pool.ntp.org, time.google.com)");
}

bool time_sync_is_synced(void)
{
    return s_synced;
}

bool time_sync_is_sntp_synced(void)
{
    return s_sntp_synced;
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
    if (s_sntp_synced || !fof_time_epoch_is_valid(epoch_ms)) {
        return;
    }
    int64_t current_epoch_ms = time_sync_get_epoch_ms();
    bool have_local_epoch = s_synced && fof_time_epoch_is_valid(current_epoch_ms);
    if (fof_time_should_apply_backend_epoch(
            false,
            have_local_epoch,
            current_epoch_ms,
            epoch_ms,
            FOF_TIME_SYNC_BACKEND_RESTEER_THRESHOLD_MS)) {
        struct timeval tv = {
            .tv_sec  = (time_t)(epoch_ms / 1000LL),
            .tv_usec = (suseconds_t)((epoch_ms % 1000LL) * 1000LL),
        };
        settimeofday(&tv, NULL);
        ESP_LOGW(TAG, "Time synced from BACKEND fallback: epoch_ms=%lld", (long long)epoch_ms);
    }
    s_synced = true;
    s_backend_authoritative = true;
    s_last_backend_sync_monotonic_ms = esp_timer_get_time() / 1000;
}

bool time_sync_has_fresh_authority(int64_t freshness_ms)
{
    if (!s_synced) {
        return false;
    }
    if (s_sntp_synced) {
        return true;
    }
    if (!s_backend_authoritative || s_last_backend_sync_monotonic_ms <= 0) {
        return false;
    }
    int64_t now_ms = esp_timer_get_time() / 1000;
    return (now_ms - s_last_backend_sync_monotonic_ms) <= freshness_ms;
}

const char *time_sync_authority_source(void)
{
    if (s_sntp_synced) {
        return "sntp";
    }
    if (s_backend_authoritative && s_synced) {
        return "backend";
    }
    return "none";
}
