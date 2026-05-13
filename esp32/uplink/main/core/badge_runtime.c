#include "badge_runtime.h"

#include "esp_log.h"
#include "esp_attr.h"
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
#define BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC 0xF0F0B007U
#define BADGE_RUNTIME_USB_STALE_AFTER_S  90
#define BADGE_RUNTIME_USB_BOOT_GRACE_S   120

static badge_runtime_network_mode_t s_network_mode = BADGE_RUNTIME_NETWORK_OFF;
static int64_t s_network_until_ms = 0;
static bool s_safe_mode = false;
static bool s_pending_verify = false;
static char s_safe_reason[64] = "";
static uint32_t s_crash_count = 0;
static uint32_t s_last_reset_reason = 0;
static badge_runtime_reset_class_t s_last_reset_class = BADGE_RUNTIME_RESET_CLEAN;
static bool s_last_reset_expected = false;
static bool s_display_alive = false;
static bool s_usb_control_alive = false;
static int64_t s_usb_control_last_ms = 0;
static bool s_scanner_uart_alive = false;
static bool s_marked_stable = false;
static uint32_t s_display_stack_free_words = 0;
static uint32_t s_main_stack_free_words = 0;
static uint32_t s_usb_stack_free_words = 0;
static uint32_t s_uart_ble_stack_free_words = 0;
static uint32_t s_uart_wifi_stack_free_words = 0;
static badge_runtime_apply_network_fn_t s_apply_network = NULL;

RTC_NOINIT_ATTR static uint32_t s_expected_reboot_magic;

static bool reset_reason_is_unhealthy_reset(esp_reset_reason_t reason,
                                            bool expected_software_reset)
{
    if (reason == ESP_RST_SW) {
        return !expected_software_reset;
    }
    return reason == ESP_RST_PANIC ||
           reason == ESP_RST_INT_WDT ||
           reason == ESP_RST_TASK_WDT ||
           reason == ESP_RST_WDT;
}

static badge_runtime_reset_class_t reset_class_for_reason(
    esp_reset_reason_t reason,
    bool expected_software_reset)
{
    if (reason == ESP_RST_SW && expected_software_reset) {
        return BADGE_RUNTIME_RESET_EXPECTED_SW;
    }
    return reset_reason_is_unhealthy_reset(reason, expected_software_reset)
        ? BADGE_RUNTIME_RESET_CRASH
        : BADGE_RUNTIME_RESET_CLEAN;
}

static const char *reset_reason_name(uint32_t reason)
{
    switch ((esp_reset_reason_t)reason) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_EXT:       return "external";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
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
    esp_reset_reason_t reason = esp_reset_reason();
    bool expected_software_reset =
        badge_runtime_reset_reason_was_expected_software((uint32_t)reason);
    s_expected_reboot_magic = 0;
    s_last_reset_reason = (uint32_t)reason;
    s_last_reset_expected = expected_software_reset;
    s_last_reset_class = reset_class_for_reason(reason, expected_software_reset);
    s_pending_verify = pending_verify;
    s_network_mode = BADGE_RUNTIME_NETWORK_OFF;
    s_network_until_ms = 0;
    s_display_alive = false;
    s_usb_control_alive = false;
    s_usb_control_last_ms = 0;
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
        s_last_reset_class,
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
    ESP_LOGW(TAG, "Badge reset reason=%s expected=%d class=%d crashes=%lu pending=%d",
             reset_reason_name(s_last_reset_reason),
             s_last_reset_expected ? 1 : 0,
             (int)s_last_reset_class,
             (unsigned long)s_crash_count,
             pending_verify ? 1 : 0);

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
        if (s_crash_count != 0) {
            s_crash_count = 0;
            nvs_set_u32_value(BADGE_RUNTIME_NVS_CRASH_COUNT, 0);
        }
    }
}

void badge_runtime_arm_expected_reboot(const char *reason)
{
    s_expected_reboot_magic = BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC;
    ESP_LOGW(TAG, "Badge expected software reboot armed: %s",
             reason ? reason : "planned");
}

bool badge_runtime_reset_reason_was_expected_software(uint32_t reset_reason)
{
    return reset_reason == (uint32_t)ESP_RST_SW &&
           s_expected_reboot_magic == BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC;
}

bool badge_runtime_usb_control_recovery_due(int64_t uptime_s)
{
    return badge_runtime_usb_recovery_due(
        s_safe_mode,
        s_usb_control_alive,
        badge_runtime_usb_control_age_s(),
        uptime_s,
        BADGE_RUNTIME_USB_STALE_AFTER_S,
        BADGE_RUNTIME_USB_BOOT_GRACE_S
    );
}

void badge_runtime_note_display_alive(void)
{
    s_display_alive = true;
}

void badge_runtime_note_usb_control_alive(void)
{
    s_usb_control_alive = true;
    s_usb_control_last_ms = esp_timer_get_time() / 1000;
}

void badge_runtime_note_scanner_uart_alive(bool alive)
{
    if (alive) {
        s_scanner_uart_alive = true;
    }
}

void badge_runtime_note_display_stack_free(uint32_t words)
{
    s_display_stack_free_words = words;
}

void badge_runtime_note_main_stack_free(uint32_t words)
{
    s_main_stack_free_words = words;
}

void badge_runtime_note_usb_stack_free(uint32_t words)
{
    s_usb_stack_free_words = words;
}

void badge_runtime_note_uart_stack_free(uint8_t scanner_id, uint32_t words)
{
    if (scanner_id == 0) {
        s_uart_ble_stack_free_words = words;
    } else if (scanner_id == 1) {
        s_uart_wifi_stack_free_words = words;
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

uint32_t badge_runtime_last_reset_reason(void)
{
    return s_last_reset_reason;
}

const char *badge_runtime_last_reset_reason_name(void)
{
    return reset_reason_name(s_last_reset_reason);
}

bool badge_runtime_last_reset_expected(void)
{
    return s_last_reset_expected;
}

int64_t badge_runtime_usb_control_age_s(void)
{
    if (!s_usb_control_alive || s_usb_control_last_ms <= 0) {
        return -1;
    }
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms < s_usb_control_last_ms) {
        return 0;
    }
    return (now_ms - s_usb_control_last_ms) / 1000;
}

const char *badge_runtime_recovery_mode(void)
{
    if (s_safe_mode) {
        return "safe_usb";
    }
    if (!s_usb_control_alive) {
        return "usb_wait";
    }
    if (badge_runtime_usb_control_age_s() >= BADGE_RUNTIME_USB_STALE_AFTER_S) {
        return "usb_stale";
    }
    return "normal";
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

uint32_t badge_runtime_display_stack_free(void)
{
    return s_display_stack_free_words;
}

uint32_t badge_runtime_main_stack_free(void)
{
    return s_main_stack_free_words;
}

uint32_t badge_runtime_usb_stack_free(void)
{
    return s_usb_stack_free_words;
}

uint32_t badge_runtime_uart_ble_stack_free(void)
{
    return s_uart_ble_stack_free_words;
}

uint32_t badge_runtime_uart_wifi_stack_free(void)
{
    return s_uart_wifi_stack_free_words;
}
