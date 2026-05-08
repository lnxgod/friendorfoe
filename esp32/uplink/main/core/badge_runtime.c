#include "badge_runtime.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "badge_runtime";

#define BADGE_RUNTIME_NVS_NS             "badge_rt"
#define BADGE_RUNTIME_NVS_CRASH_COUNT    "crash_n"
#define BADGE_RUNTIME_NVS_FORCE_SAFE     "force_safe"
#define BADGE_RUNTIME_NVS_HOLD_MODE      "hold_mode"
#define BADGE_RUNTIME_NVS_HOLD_TTL       "hold_ttl"
#define BADGE_RUNTIME_CRASH_THRESHOLD    3
#define BADGE_RUNTIME_STABLE_AFTER_S     60

static badge_runtime_network_mode_t s_network_mode = BADGE_RUNTIME_NETWORK_OFF;
static int64_t s_network_until_ms = 0;
static bool s_safe_mode = false;
static bool s_pending_verify = false;
static char s_safe_reason[64] = "";
static uint32_t s_crash_count = 0;
static bool s_display_alive = false;
static bool s_usb_control_alive = false;
static bool s_scanner_uart_alive = false;
static bool s_marked_stable = false;
static badge_runtime_apply_network_fn_t s_apply_network = NULL;

static bool reset_reason_is_unhealthy_reset(esp_reset_reason_t reason)
{
    return reason == ESP_RST_PANIC ||
           reason == ESP_RST_INT_WDT ||
           reason == ESP_RST_TASK_WDT ||
           reason == ESP_RST_WDT ||
           reason == ESP_RST_SW;
}

static uint32_t nvs_get_u32_default(const char *key, uint32_t fallback)
{
    nvs_handle_t h;
    if (nvs_open(BADGE_RUNTIME_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return fallback;
    }
    uint32_t value = fallback;
    (void)nvs_get_u32(h, key, &value);
    nvs_close(h);
    return value;
}

static void nvs_set_u32_value(const char *key, uint32_t value)
{
    nvs_handle_t h;
    if (nvs_open(BADGE_RUNTIME_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed for %s", key);
        return;
    }
    nvs_set_u32(h, key, value);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_erase_key_value(const char *key)
{
    nvs_handle_t h;
    if (nvs_open(BADGE_RUNTIME_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}

static void set_safe_reason(const char *reason)
{
    strncpy(s_safe_reason, reason ? reason : "manual", sizeof(s_safe_reason) - 1);
    s_safe_reason[sizeof(s_safe_reason) - 1] = '\0';
}

void badge_runtime_init(bool pending_verify)
{
    s_pending_verify = pending_verify;
    s_network_mode = BADGE_RUNTIME_NETWORK_OFF;
    s_network_until_ms = 0;
    s_display_alive = false;
    s_usb_control_alive = false;
    s_scanner_uart_alive = false;
    s_marked_stable = false;

    s_crash_count = nvs_get_u32_default(BADGE_RUNTIME_NVS_CRASH_COUNT, 0);
    uint32_t forced_safe = nvs_get_u32_default(BADGE_RUNTIME_NVS_FORCE_SAFE, 0);
    uint32_t hold_mode = nvs_get_u32_default(BADGE_RUNTIME_NVS_HOLD_MODE,
                                             BADGE_RUNTIME_NETWORK_OFF);
    uint32_t hold_ttl = nvs_get_u32_default(BADGE_RUNTIME_NVS_HOLD_TTL, 0);
    nvs_erase_key_value(BADGE_RUNTIME_NVS_HOLD_MODE);
    nvs_erase_key_value(BADGE_RUNTIME_NVS_HOLD_TTL);
    if (forced_safe != 0) {
        s_safe_mode = true;
        set_safe_reason("manual");
    } else {
        s_safe_mode = false;
        s_safe_reason[0] = '\0';
    }

    badge_runtime_boot_decision_t decision = badge_runtime_boot_decide(
        reset_reason_is_unhealthy_reset(esp_reset_reason())
            ? BADGE_RUNTIME_RESET_CRASH
            : BADGE_RUNTIME_RESET_CLEAN,
        pending_verify,
        s_crash_count,
        BADGE_RUNTIME_CRASH_THRESHOLD
    );
    s_crash_count = decision.new_crash_count;
    nvs_set_u32_value(BADGE_RUNTIME_NVS_CRASH_COUNT, s_crash_count);
    if (decision.enter_safe_mode) {
        s_safe_mode = true;
        set_safe_reason("crash_loop");
        ESP_LOGE(TAG, "Badge safe mode armed after %lu crashes",
                 (unsigned long)s_crash_count);
    }

    if (!s_safe_mode &&
        (hold_mode == BADGE_RUNTIME_NETWORK_LOCAL_AP ||
         hold_mode == BADGE_RUNTIME_NETWORK_BACKEND)) {
        int ttl_s = badge_runtime_post_ota_hold_ttl_s(
            (badge_runtime_network_mode_t)hold_mode,
            (int)hold_ttl
        );
        if (ttl_s > 0) {
            s_network_mode = (badge_runtime_network_mode_t)hold_mode;
            s_network_until_ms =
                (esp_timer_get_time() / 1000) + ((int64_t)ttl_s * 1000);
            ESP_LOGW(TAG, "Badge consumed one-shot reboot network hold: %s ttl=%ds",
                     badge_runtime_network_mode_name(s_network_mode), ttl_s);
        }
    }
}

void badge_runtime_set_pending_verify(bool pending_verify)
{
    s_pending_verify = pending_verify;
}

void badge_runtime_set_network_apply_callback(badge_runtime_apply_network_fn_t cb)
{
    s_apply_network = cb;
    if (s_apply_network) {
        (void)s_apply_network(s_network_mode);
    }
}

bool badge_runtime_request_network(badge_runtime_network_mode_t mode,
                                   int ttl_s,
                                   const char *reason)
{
    if (!badge_runtime_badge_allows_network_mode(mode)) {
        ESP_LOGW(TAG, "Badge network mode %s disabled in USB-only firmware",
                 badge_runtime_network_mode_name(mode));
        s_network_mode = BADGE_RUNTIME_NETWORK_OFF;
        s_network_until_ms = 0;
        if (s_apply_network) {
            (void)s_apply_network(BADGE_RUNTIME_NETWORK_OFF);
        }
        return false;
    }

    if (s_safe_mode && mode != BADGE_RUNTIME_NETWORK_OFF) {
        ESP_LOGW(TAG, "Ignoring network enable while safe mode is active");
        return false;
    }

    ttl_s = badge_runtime_network_ttl_s(mode, ttl_s);
    s_network_mode = mode;
    s_network_until_ms = (ttl_s > 0)
        ? (esp_timer_get_time() / 1000) + ((int64_t)ttl_s * 1000)
        : 0;

    ESP_LOGW(TAG, "Badge network session -> %s ttl=%ds reason=%s",
             badge_runtime_network_mode_name(mode), ttl_s,
             reason ? reason : "usb");
    if (s_apply_network) {
        return s_apply_network(mode);
    }
    return mode == BADGE_RUNTIME_NETWORK_OFF;
}

bool badge_runtime_arm_reboot_network_hold(badge_runtime_network_mode_t mode,
                                           int ttl_s)
{
    ttl_s = badge_runtime_post_ota_hold_ttl_s(mode, ttl_s);
    if (ttl_s <= 0) {
        nvs_erase_key_value(BADGE_RUNTIME_NVS_HOLD_MODE);
        nvs_erase_key_value(BADGE_RUNTIME_NVS_HOLD_TTL);
        return false;
    }

    nvs_set_u32_value(BADGE_RUNTIME_NVS_HOLD_MODE, (uint32_t)mode);
    nvs_set_u32_value(BADGE_RUNTIME_NVS_HOLD_TTL, (uint32_t)ttl_s);
    ESP_LOGW(TAG, "Badge armed one-shot reboot network hold: %s ttl=%ds",
             badge_runtime_network_mode_name(mode), ttl_s);
    return true;
}

void badge_runtime_poll(void)
{
    if (s_network_mode != BADGE_RUNTIME_NETWORK_OFF &&
        s_network_until_ms > 0 &&
        (esp_timer_get_time() / 1000) >= s_network_until_ms) {
        ESP_LOGW(TAG, "Badge network session expired");
        (void)badge_runtime_request_network(BADGE_RUNTIME_NETWORK_OFF, 0, "ttl_expired");
    }
}

void badge_runtime_force_safe_mode(bool enabled, const char *reason)
{
    s_safe_mode = enabled;
    if (enabled) {
        set_safe_reason(reason ? reason : "manual");
        (void)badge_runtime_request_network(BADGE_RUNTIME_NETWORK_OFF, 0, "safe_mode");
        nvs_set_u32_value(BADGE_RUNTIME_NVS_FORCE_SAFE, 1);
    } else {
        s_safe_reason[0] = '\0';
        nvs_set_u32_value(BADGE_RUNTIME_NVS_FORCE_SAFE, 0);
    }
}

void badge_runtime_note_display_alive(void)
{
    s_display_alive = true;
}

void badge_runtime_note_usb_control_alive(void)
{
    s_usb_control_alive = true;
}

void badge_runtime_note_scanner_uart_alive(bool alive)
{
    if (alive) {
        s_scanner_uart_alive = true;
    }
}

bool badge_runtime_health_can_mark_ota_valid(uint32_t free_heap_bytes,
                                             int64_t uptime_s)
{
    return badge_runtime_can_mark_valid(
        s_safe_mode,
        s_display_alive,
        s_usb_control_alive,
        s_scanner_uart_alive,
        free_heap_bytes,
        uptime_s,
        BADGE_RUNTIME_STABLE_AFTER_S
    );
}

void badge_runtime_mark_stable(void)
{
    if (s_marked_stable) {
        return;
    }
    s_marked_stable = true;
    if (s_crash_count != 0) {
        s_crash_count = 0;
        nvs_set_u32_value(BADGE_RUNTIME_NVS_CRASH_COUNT, 0);
    }
}

badge_runtime_network_mode_t badge_runtime_get_network_mode(void)
{
    return s_network_mode;
}

int badge_runtime_get_network_ttl_s(void)
{
    if (s_network_mode == BADGE_RUNTIME_NETWORK_OFF || s_network_until_ms <= 0) {
        return 0;
    }
    int64_t remaining_ms = s_network_until_ms - (esp_timer_get_time() / 1000);
    if (remaining_ms <= 0) {
        return 0;
    }
    return (int)((remaining_ms + 999) / 1000);
}

bool badge_runtime_is_safe_mode(void)
{
    return s_safe_mode;
}

const char *badge_runtime_safe_reason(void)
{
    return s_safe_reason[0] ? s_safe_reason : "";
}

uint32_t badge_runtime_crash_count(void)
{
    return s_crash_count;
}

bool badge_runtime_pending_verify(void)
{
    return s_pending_verify;
}

bool badge_runtime_display_alive(void)
{
    return s_display_alive;
}

bool badge_runtime_usb_control_alive(void)
{
    return s_usb_control_alive;
}

bool badge_runtime_scanner_uart_alive(void)
{
    return s_scanner_uart_alive;
}
