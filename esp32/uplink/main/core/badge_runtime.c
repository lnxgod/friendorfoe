#include "badge_runtime.h"

#if defined(FOF_DC34_GAME_CANARY)
#include "badge_con_game.h"
#include "badge_update_maintenance_policy.h"
#endif

#include "esp_log.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"

#include <stddef.h>
#include <string.h>

static const char *TAG = "badge_runtime";

#define BADGE_RUNTIME_NVS_NS             "badge_rt"
#define BADGE_RUNTIME_NVS_CRASH_COUNT    "crash_n"
#define BADGE_RUNTIME_NVS_HOLD_MODE      "hold_mode"
#define BADGE_RUNTIME_NVS_HOLD_TTL       "hold_ttl"
#define BADGE_RUNTIME_NVS_EXPECTED_REASON "exp_reason"
#define BADGE_RUNTIME_CRASH_THRESHOLD    3
#define BADGE_RUNTIME_STABLE_AFTER_S     60
#define BADGE_RUNTIME_USB_STALE_AFTER_S  90
#define BADGE_RUNTIME_USB_BOOT_GRACE_S   120
#define BADGE_RUNTIME_UART_HEARTBEAT_STALE_MS 15000
#define BADGE_RUNTIME_UPDATE_ORPHAN_GRACE_MS 1500U
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
static bool s_usb_response_completed = false;
static int64_t s_usb_control_last_ms = 0;
static int64_t s_scanner_uart_last_ms[2] = {0, 0};
static portMUX_TYPE s_runtime_health_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_marked_stable = false;
static bool s_usb_recovery_once_consumed = false;
static char s_last_expected_reboot_reason[64] = "";
static uint32_t s_display_stack_free_words = 0;
static uint32_t s_main_stack_free_words = 0;
static uint32_t s_usb_stack_free_words = 0;
static uint32_t s_uart_ble_stack_free_words = 0;
static uint32_t s_uart_wifi_stack_free_words = 0;
static badge_runtime_apply_network_fn_t s_apply_network = NULL;
static badge_runtime_expected_reboot_hook_t s_expected_reboot_hook = NULL;
static uint32_t s_last_expected_reboot_generation = 0U;
static badge_runtime_expected_reboot_arm_state_t s_expected_reboot_arm_state;
static bool s_boot_rtc_transition_cached;
static badge_runtime_rtc_boot_result_t s_boot_rtc_transition_result;

typedef struct {
    uint32_t usb_recovery_once_magic;
    uint32_t expected_reboot_generation;
    uint32_t expected_reboot_magic;
    uint32_t layout_magic;
    uint16_t layout_version;
    uint16_t layout_size;
#if defined(FOF_DC34_GAME_CANARY)
    badge_update_maintenance_marker_t update_maintenance_marker;
    uint8_t game_rtc_record[BADGE_CON_RTC_RECORD_BYTES];
#endif
} badge_runtime_rtc_state_t;

_Static_assert(
    offsetof(badge_runtime_rtc_state_t, usb_recovery_once_magic) ==
        BADGE_RUNTIME_RTC_RECOVERY_OFFSET,
    "RTC recovery token ABI moved");
_Static_assert(
    offsetof(badge_runtime_rtc_state_t, expected_reboot_generation) ==
        BADGE_RUNTIME_RTC_GENERATION_OFFSET,
    "RTC reboot generation ABI moved");
_Static_assert(
    offsetof(badge_runtime_rtc_state_t, expected_reboot_magic) ==
        BADGE_RUNTIME_RTC_EXPECTED_MAGIC_OFFSET,
    "RTC reboot magic ABI moved");
_Static_assert(
    offsetof(badge_runtime_rtc_state_t, layout_magic) ==
        BADGE_RUNTIME_RTC_EXTENSION_OFFSET,
    "RTC extension ABI moved");
_Static_assert(
    offsetof(badge_runtime_rtc_state_t, layout_version) ==
        BADGE_RUNTIME_RTC_LAYOUT_VERSION_OFFSET,
    "RTC layout version ABI moved");
_Static_assert(
    offsetof(badge_runtime_rtc_state_t, layout_size) ==
        BADGE_RUNTIME_RTC_LAYOUT_SIZE_OFFSET,
    "RTC layout size ABI moved");
_Static_assert(_Alignof(badge_runtime_rtc_state_t) == 4U,
               "RTC state ABI alignment changed");
_Static_assert(sizeof(badge_runtime_rtc_state_t) <= UINT16_MAX,
               "RTC state size does not fit its ABI header");
#if defined(FOF_DC34_GAME_CANARY)
_Static_assert(sizeof(badge_update_maintenance_marker_t) == 0xA0U,
               "RTC update marker size changed");
_Static_assert(BADGE_CON_RTC_RECORD_BYTES == 0x14U,
               "RTC game record size changed");
_Static_assert(
    offsetof(badge_runtime_rtc_state_t, update_maintenance_marker) ==
        BADGE_RUNTIME_RTC_CANARY_MARKER_OFFSET,
    "RTC update marker ABI moved");
_Static_assert(
    offsetof(badge_runtime_rtc_state_t, game_rtc_record) ==
        BADGE_RUNTIME_RTC_CANARY_GAME_OFFSET,
    "RTC game record ABI moved");
_Static_assert(sizeof(badge_runtime_rtc_state_t) ==
                   BADGE_RUNTIME_RTC_CANARY_SIZE,
               "RTC canary state ABI size changed");
#else
_Static_assert(sizeof(badge_runtime_rtc_state_t) ==
                   BADGE_RUNTIME_RTC_PRODUCTION_SIZE,
               "RTC production state ABI size changed");
#endif

/*
 * The storage and offset aliases are emitted together so the final ELF has
 * exactly one GLOBAL OBJECT spanning .rtc_noinit, while each stable ABI alias
 * remains a zero-sized GLOBAL NOTYPE symbol in the same section.
 * The sole retained-memory attribute stays on the C declaration as a
 * source-level guard; the assembly definition is NOBITS and has no initializer.
 */
extern RTC_NOINIT_ATTR badge_runtime_rtc_state_t g_fof_badge_rtc_state;
#if defined(FOF_DC34_GAME_CANARY)
#define BADGE_RUNTIME_RTC_ASM_REMAINDER "188"
#else
#define BADGE_RUNTIME_RTC_ASM_REMAINDER "8"
#endif
__asm__(
    ".pushsection .rtc_noinit,\"aw\",@nobits\n"
    ".balign 4\n"
    ".global fof_badge_rtc_usb_recovery_once_magic\n"
    ".type fof_badge_rtc_usb_recovery_once_magic, @notype\n"
    ".size fof_badge_rtc_usb_recovery_once_magic, 0\n"
    ".global g_fof_badge_rtc_state\n"
    ".type g_fof_badge_rtc_state, @object\n"
    "g_fof_badge_rtc_state:\n"
    "fof_badge_rtc_usb_recovery_once_magic:\n"
    ".space 4\n"
    ".global fof_badge_rtc_expected_reboot_generation\n"
    ".type fof_badge_rtc_expected_reboot_generation, @notype\n"
    ".size fof_badge_rtc_expected_reboot_generation, 0\n"
    "fof_badge_rtc_expected_reboot_generation:\n"
    ".space 4\n"
    ".global fof_badge_rtc_expected_reboot_magic\n"
    ".type fof_badge_rtc_expected_reboot_magic, @notype\n"
    ".size fof_badge_rtc_expected_reboot_magic, 0\n"
    "fof_badge_rtc_expected_reboot_magic:\n"
    ".space 4\n"
    ".space " BADGE_RUNTIME_RTC_ASM_REMAINDER "\n"
    ".size g_fof_badge_rtc_state, .-g_fof_badge_rtc_state\n"
    ".popsection\n"
);
#undef BADGE_RUNTIME_RTC_ASM_REMAINDER

#if defined(FOF_DC34_GAME_CANARY)
static bool s_update_maintenance_initialized;
static bool s_update_maintenance_active;
static bool s_update_prepare_seen;
static uint32_t s_update_last_prepare_request_ms;
static uint32_t s_update_last_activity_ms;
#endif

/*
 * Must be called with s_runtime_health_lock held. The USB transport starts
 * before badge_runtime_init(), so the first pre-init reboot owner performs
 * and caches the current boot's one-shot RTC transition before publishing its
 * next-boot token. Runtime init then reuses this immutable result instead of
 * consuming the new token out from under that owner.
 */
static badge_runtime_rtc_boot_result_t boot_rtc_transition_locked(void)
{
    if (!s_boot_rtc_transition_cached) {
        esp_reset_reason_t reason = esp_reset_reason();
        s_boot_rtc_transition_result = badge_runtime_rtc_transition(
            (uint8_t *)&g_fof_badge_rtc_state,
            sizeof(g_fof_badge_rtc_state),
            reason == ESP_RST_SW,
            (uint16_t)sizeof(g_fof_badge_rtc_state));
        s_boot_rtc_transition_cached = true;
    }
    return s_boot_rtc_transition_result;
}

static bool rtc_layout_valid(void)
{
    return g_fof_badge_rtc_state.layout_magic ==
               BADGE_RUNTIME_RTC_LAYOUT_MAGIC &&
           g_fof_badge_rtc_state.layout_version ==
               BADGE_RUNTIME_RTC_LAYOUT_VERSION &&
           g_fof_badge_rtc_state.layout_size ==
               sizeof(g_fof_badge_rtc_state);
}

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
        case ESP_RST_USB:       return "usb";
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

static void nvs_set_string_value(const char *key, const char *value)
{
    nvs_handle_t h;
    if (nvs_open(BADGE_RUNTIME_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed for %s", key);
        return;
    }
    if (nvs_set_str(h, key, value ? value : "planned") == ESP_OK) {
        (void)nvs_commit(h);
    }
    nvs_close(h);
}

static void nvs_get_string_value(const char *key, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(BADGE_RUNTIME_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t required = out_size;
    if (nvs_get_str(h, key, out, &required) != ESP_OK) {
        out[0] = '\0';
    }
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
    portENTER_CRITICAL(&s_runtime_health_lock);
    badge_runtime_rtc_boot_result_t reboot_decision =
        boot_rtc_transition_locked();
    portEXIT_CRITICAL(&s_runtime_health_lock);
    bool expected_software_reset =
        reboot_decision.expected_software_reset;
    uint32_t consumed_expected_generation =
        reboot_decision.consumed_generation;
    s_last_reset_reason = (uint32_t)reason;
    s_last_reset_expected = expected_software_reset;
    s_last_expected_reboot_generation = consumed_expected_generation;
    s_last_reset_class = reset_class_for_reason(reason, expected_software_reset);
    badge_runtime_recovery_token_action_t token_action =
        badge_runtime_recovery_token_decide(
            g_fof_badge_rtc_state.usb_recovery_once_magic ==
                BADGE_RUNTIME_USB_RECOVERY_ONCE_MAGIC,
            s_last_reset_class);
    g_fof_badge_rtc_state.usb_recovery_once_magic = 0U;
    s_usb_recovery_once_consumed =
        token_action == BADGE_RUNTIME_RECOVERY_TOKEN_CONSUME_SAFE_USB;
    s_pending_verify = pending_verify;
    s_network_mode = BADGE_RUNTIME_NETWORK_OFF;
    s_network_until_ms = 0;
    s_usb_control_alive = false;
    s_usb_control_last_ms = 0;
    portENTER_CRITICAL(&s_runtime_health_lock);
    s_display_alive = false;
    s_usb_response_completed = false;
    s_scanner_uart_last_ms[0] = 0;
    s_scanner_uart_last_ms[1] = 0;
    portEXIT_CRITICAL(&s_runtime_health_lock);
    s_marked_stable = false;
    /*
     * Do not reinitialize s_expected_reboot_arm_state here. The USB
     * transport starts before badge_runtime_init() and may already own a
     * same-session arm; resetting it here would break the arm-to-restart
     * lease. Static BSS initialization supplies the one boot-time IDLE state.
     */

    s_last_expected_reboot_reason[0] = '\0';
    nvs_get_string_value(BADGE_RUNTIME_NVS_EXPECTED_REASON,
                         s_last_expected_reboot_reason,
                         sizeof(s_last_expected_reboot_reason));

    s_crash_count = nvs_get_u32_default(BADGE_RUNTIME_NVS_CRASH_COUNT, 0);
    uint32_t hold_mode = nvs_get_u32_default(BADGE_RUNTIME_NVS_HOLD_MODE,
                                             BADGE_RUNTIME_NETWORK_OFF);
    uint32_t hold_ttl = nvs_get_u32_default(BADGE_RUNTIME_NVS_HOLD_TTL, 0);
    nvs_erase_key_value(BADGE_RUNTIME_NVS_HOLD_MODE);
    nvs_erase_key_value(BADGE_RUNTIME_NVS_HOLD_TTL);
    if (s_usb_recovery_once_consumed) {
        s_safe_mode = true;
        set_safe_reason("usb_safe_once");
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
#if defined(FOF_DC34_GAME_CANARY)
    s_update_maintenance_initialized = true;
    s_update_maintenance_active = false;
    s_update_prepare_seen = false;
    s_update_last_prepare_request_ms = 0U;
    s_update_last_activity_ms = 0U;
    badge_update_maintenance_boot_action_t update_boot =
        badge_update_maintenance_boot_decide(
            &g_fof_badge_rtc_state.update_maintenance_marker,
            s_last_reset_expected,
            s_last_expected_reboot_generation,
            s_safe_mode);
    if (update_boot == BADGE_UPDATE_BOOT_ENTER &&
        badge_update_maintenance_marker_activate(
            &g_fof_badge_rtc_state.update_maintenance_marker)) {
        s_update_maintenance_active = true;
        s_update_last_activity_ms =
            (uint32_t)(esp_timer_get_time() / 1000);
        ESP_LOGW(TAG,
                 "Update-maintenance boot admitted session=%s boot=%u",
                 g_fof_badge_rtc_state.update_maintenance_marker.session,
                 (unsigned)g_fof_badge_rtc_state
                     .update_maintenance_marker.boot_count);
    } else {
        memset(
            &g_fof_badge_rtc_state.update_maintenance_marker,
            0,
            sizeof(g_fof_badge_rtc_state.update_maintenance_marker));
    }
#endif
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
    } else {
        s_safe_reason[0] = '\0';
        if (s_crash_count != 0) {
            s_crash_count = 0;
            nvs_set_u32_value(BADGE_RUNTIME_NVS_CRASH_COUNT, 0);
        }
    }
}

badge_runtime_expected_reboot_arm_result_t
badge_runtime_arm_expected_reboot(
    const char *reason,
    badge_runtime_expected_reboot_target_t target,
    badge_runtime_expected_reboot_lease_t *out_lease)
{
    badge_runtime_expected_reboot_hook_t hook = NULL;
    uint32_t generation = 0U;
    badge_runtime_expected_reboot_lease_t lease = {0};

    if (out_lease) {
        memset(out_lease, 0, sizeof(*out_lease));
    }
    if (!out_lease ||
        (target != BADGE_RUNTIME_EXPECTED_REBOOT_TARGET_CURRENT &&
         target !=
             BADGE_RUNTIME_EXPECTED_REBOOT_TARGET_LEGACY_V078_ROLLBACK)) {
        ESP_LOGE(TAG, "Expected reboot arm rejected: invalid arguments");
        return BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_FAILED;
    }

    portENTER_CRITICAL(&s_runtime_health_lock);
    /*
     * Reject a duplicate before touching retained memory. The first owner
     * keeps its authenticated token from preparation through the non-returning
     * restart handoff.
     */
    if (s_expected_reboot_arm_state.phase !=
        BADGE_RUNTIME_EXPECTED_REBOOT_ARM_IDLE) {
        portEXIT_CRITICAL(&s_runtime_health_lock);
        ESP_LOGW(TAG, "Expected reboot arm rejected: owner already active");
        return BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_BUSY;
    }
    (void)boot_rtc_transition_locked();
    if (!rtc_layout_valid()) {
        /*
         * Recovery can arm a second software reset before badge_runtime_init()
         * runs (for example if OTA rollback unexpectedly returns). Normalize
         * retained bytes here while preserving the recovery word at +0.
         */
        (void)badge_runtime_rtc_transition(
            (uint8_t *)&g_fof_badge_rtc_state,
            sizeof(g_fof_badge_rtc_state),
            false,
            (uint16_t)sizeof(g_fof_badge_rtc_state));
    }
    if (!rtc_layout_valid()) {
        portEXIT_CRITICAL(&s_runtime_health_lock);
        ESP_LOGE(TAG,
                 "Expected reboot arm rejected: invalid RTC layout");
        return BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_FAILED;
    }
    generation = badge_runtime_expected_reboot_generation_for_target(
        target,
        g_fof_badge_rtc_state.expected_reboot_generation);
    if (!badge_runtime_expected_reboot_arm_reserve(
            &s_expected_reboot_arm_state, generation, &lease)) {
        portEXIT_CRITICAL(&s_runtime_health_lock);
        ESP_LOGE(TAG,
                 "Expected reboot arm rejected: ownership exhausted");
        return BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_FAILED;
    }
    /*
     * Invalidate any earlier token before preparing extension records.
     * Generation and magic are not published until every dependent RTC
     * record has been written successfully.
     */
    __atomic_store_n(
        &g_fof_badge_rtc_state.expected_reboot_magic,
        0U,
        __ATOMIC_RELEASE);
    hook = s_expected_reboot_hook;
    portEXIT_CRITICAL(&s_runtime_health_lock);

    /*
     * Lock-order invariant: badge_runtime never holds s_runtime_health_lock
     * while calling the game hook. The hook may hold its game lock and call
     * badge_runtime_game_rtc_write(), which acquires the runtime lock. There
     * is no runtime-lock -> game-lock edge.
     */
    bool hook_ok = !hook || hook(generation);
    bool armed = false;

    portENTER_CRITICAL(&s_runtime_health_lock);
    bool dependencies_valid =
        badge_runtime_expected_reboot_arm_is_preparing(
            &s_expected_reboot_arm_state, &lease) &&
        rtc_layout_valid() &&
        hook_ok;
#if defined(FOF_DC34_GAME_CANARY)
    bool needs_update_prepare =
        reason && strcmp(reason, "update_maintenance") == 0;
    bool needs_update_ota =
        reason && strcmp(reason, "usb_uplink_ota") == 0;
    if (needs_update_prepare || needs_update_ota) {
        dependencies_valid = dependencies_valid && hook != NULL;
    }
    bool can_bind_update_prepare =
        needs_update_prepare &&
        badge_update_maintenance_marker_valid(
            &g_fof_badge_rtc_state.update_maintenance_marker) &&
        g_fof_badge_rtc_state.update_maintenance_marker.phase ==
            BADGE_UPDATE_PHASE_PREPARING;
    bool can_bind_update_ota =
        needs_update_ota &&
        s_update_maintenance_active &&
        badge_update_maintenance_marker_valid(
            &g_fof_badge_rtc_state.update_maintenance_marker) &&
        g_fof_badge_rtc_state.update_maintenance_marker.phase ==
            BADGE_UPDATE_PHASE_ACTIVE;
    if (dependencies_valid &&
        (needs_update_prepare || needs_update_ota)) {
        dependencies_valid =
            (can_bind_update_prepare || can_bind_update_ota) &&
            badge_update_maintenance_marker_arm_reboot(
                &g_fof_badge_rtc_state.update_maintenance_marker,
                generation);
    }
#endif
    if (dependencies_valid) {
        armed = badge_runtime_expected_reboot_arm_publish(
            &s_expected_reboot_arm_state, &lease);
        if (armed) {
            g_fof_badge_rtc_state.expected_reboot_generation = generation;
            /* Publish the authentication word last. */
            __atomic_store_n(
                &g_fof_badge_rtc_state.expected_reboot_magic,
                BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC,
                __ATOMIC_RELEASE);
        }
    }
    if (!armed) {
        (void)badge_runtime_expected_reboot_arm_cancel(
            &s_expected_reboot_arm_state, &lease);
    }
    portEXIT_CRITICAL(&s_runtime_health_lock);

    if (!armed) {
        ESP_LOGE(TAG,
                 "Expected reboot arm failed before token publication");
        return BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_FAILED;
    }

    nvs_set_string_value(BADGE_RUNTIME_NVS_EXPECTED_REASON,
                         reason ? reason : "planned");
    ESP_LOGW(TAG, "Badge expected software reboot armed: %s",
             reason ? reason : "planned");
    *out_lease = lease;
    return BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_OWNED;
}

void badge_runtime_set_expected_reboot_hook(
    badge_runtime_expected_reboot_hook_t hook)
{
    portENTER_CRITICAL(&s_runtime_health_lock);
    s_expected_reboot_hook = hook;
    portEXIT_CRITICAL(&s_runtime_health_lock);
}

bool badge_runtime_expected_reboot_lease_is_owned(
    const badge_runtime_expected_reboot_lease_t *lease)
{
    bool owned;
    portENTER_CRITICAL(&s_runtime_health_lock);
    owned = badge_runtime_expected_reboot_arm_is_owned(
        &s_expected_reboot_arm_state, lease);
    portEXIT_CRITICAL(&s_runtime_health_lock);
    return owned;
}

bool badge_runtime_release_expected_reboot(
    const badge_runtime_expected_reboot_lease_t *lease)
{
    /*
     * Invalidate the retained reboot marker before touching the durable
     * diagnostic reason. Ownership remains latched across the NVS erase so a
     * concurrent arm cannot publish a new token/reason pair that this release
     * then partially clears.
     */
    bool marker_cleared = false;
    portENTER_CRITICAL(&s_runtime_health_lock);
    if (badge_runtime_expected_reboot_arm_is_owned(
            &s_expected_reboot_arm_state, lease)) {
        __atomic_store_n(
            &g_fof_badge_rtc_state.expected_reboot_magic,
            0U,
            __ATOMIC_RELEASE);
        if (g_fof_badge_rtc_state.expected_reboot_generation ==
            BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC) {
            g_fof_badge_rtc_state.expected_reboot_generation = 0U;
        }
        marker_cleared = true;
    }
    portEXIT_CRITICAL(&s_runtime_health_lock);
    if (!marker_cleared) {
        return false;
    }

    nvs_erase_key_value(BADGE_RUNTIME_NVS_EXPECTED_REASON);

    bool released = false;
    portENTER_CRITICAL(&s_runtime_health_lock);
    if (badge_runtime_expected_reboot_arm_is_owned(
            &s_expected_reboot_arm_state, lease)) {
        released = badge_runtime_expected_reboot_arm_release(
            &s_expected_reboot_arm_state, lease);
    }
    portEXIT_CRITICAL(&s_runtime_health_lock);
    if (!released) {
        return false;
    }

    s_last_expected_reboot_reason[0] = '\0';
    ESP_LOGE(TAG, "Badge expected reboot marker cleared after restart failure");
    return true;
}

void badge_runtime_arm_usb_recovery_once(void)
{
    g_fof_badge_rtc_state.usb_recovery_once_magic =
        BADGE_RUNTIME_USB_RECOVERY_ONCE_MAGIC;
}

bool badge_runtime_reset_reason_was_expected_software(uint32_t reset_reason)
{
    /*
     * Rollback admission calls this before badge_runtime_init(). Classification
     * is intentionally read-only; init performs the one-shot transition.
     */
    bool expected;
    portENTER_CRITICAL(&s_runtime_health_lock);
    if (s_boot_rtc_transition_cached) {
        expected =
            reset_reason == (uint32_t)ESP_RST_SW &&
            s_boot_rtc_transition_result.expected_software_reset;
    } else {
        expected = badge_runtime_rtc_classify(
            (const uint8_t *)&g_fof_badge_rtc_state,
            sizeof(g_fof_badge_rtc_state),
            reset_reason == (uint32_t)ESP_RST_SW,
            (uint16_t)sizeof(g_fof_badge_rtc_state))
            .expected_software_reset;
    }
    portEXIT_CRITICAL(&s_runtime_health_lock);
    return expected;
}

bool badge_runtime_usb_recovery_once_consumed(void)
{
    return s_usb_recovery_once_consumed;
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
    portENTER_CRITICAL(&s_runtime_health_lock);
    s_display_alive = true;
    portEXIT_CRITICAL(&s_runtime_health_lock);
}

void badge_runtime_note_usb_control_alive(void)
{
    s_usb_control_alive = true;
    s_usb_control_last_ms = esp_timer_get_time() / 1000;
}

void badge_runtime_note_usb_response_completed(void)
{
    portENTER_CRITICAL(&s_runtime_health_lock);
    s_usb_response_completed = true;
    portEXIT_CRITICAL(&s_runtime_health_lock);
}

void badge_runtime_note_scanner_uart_worker_alive(uint8_t scanner_id)
{
    if (scanner_id < 2) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        portENTER_CRITICAL(&s_runtime_health_lock);
        s_scanner_uart_last_ms[scanner_id] = now_ms;
        portEXIT_CRITICAL(&s_runtime_health_lock);
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

typedef struct {
    bool display_alive;
    bool usb_response_completed;
    int64_t scanner_uart_last_ms[2];
} badge_runtime_health_snapshot_t;

static void runtime_health_snapshot(badge_runtime_health_snapshot_t *out)
{
    if (!out) {
        return;
    }
    portENTER_CRITICAL(&s_runtime_health_lock);
    out->display_alive = s_display_alive;
    out->usb_response_completed = s_usb_response_completed;
    for (size_t i = 0; i < 2; ++i) {
        out->scanner_uart_last_ms[i] = s_scanner_uart_last_ms[i];
    }
    portEXIT_CRITICAL(&s_runtime_health_lock);
}

bool badge_runtime_health_can_mark_stable(uint32_t free_heap_bytes,
                                          int64_t uptime_s)
{
    badge_runtime_health_snapshot_t health = {0};
    runtime_health_snapshot(&health);
    int64_t now_ms = esp_timer_get_time() / 1000;
    return badge_runtime_normal_stability_satisfied(
        s_safe_mode,
        health.display_alive,
        badge_runtime_uart_heartbeat_fresh(
            health.scanner_uart_last_ms[0], now_ms,
            BADGE_RUNTIME_UART_HEARTBEAT_STALE_MS),
        badge_runtime_uart_heartbeat_fresh(
            health.scanner_uart_last_ms[1], now_ms,
            BADGE_RUNTIME_UART_HEARTBEAT_STALE_MS),
        free_heap_bytes,
        uptime_s,
        BADGE_RUNTIME_STABLE_AFTER_S
    );
}

bool badge_runtime_health_can_mark_ota_valid(uint32_t free_heap_bytes,
                                             int64_t uptime_s)
{
    badge_runtime_health_snapshot_t health = {0};
    runtime_health_snapshot(&health);
    int64_t now_ms = esp_timer_get_time() / 1000;
    bool scanner_uart_heartbeat =
        badge_runtime_uart_heartbeat_fresh(
            health.scanner_uart_last_ms[0], now_ms,
            BADGE_RUNTIME_UART_HEARTBEAT_STALE_MS) &&
        badge_runtime_uart_heartbeat_fresh(
            health.scanner_uart_last_ms[1], now_ms,
            BADGE_RUNTIME_UART_HEARTBEAT_STALE_MS);
    return badge_runtime_rollback_health_satisfied(
        s_safe_mode,
        health.display_alive,
        health.usb_response_completed,
        scanner_uart_heartbeat,
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

const char *badge_runtime_last_expected_reboot_reason(void)
{
    return s_last_expected_reboot_reason;
}

uint32_t badge_runtime_last_expected_reboot_generation(void)
{
    return s_last_expected_reboot_generation;
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
#if defined(FOF_DC34_GAME_CANARY)
    if (s_update_maintenance_active) {
        return "update_maintenance";
    }
    if (badge_runtime_update_preparing()) {
        return "update_preparing";
    }
#endif
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
    badge_runtime_health_snapshot_t health = {0};
    runtime_health_snapshot(&health);
    return health.display_alive;
}

bool badge_runtime_usb_control_alive(void)
{
    return s_usb_control_alive;
}

bool badge_runtime_scanner_uart_alive(void)
{
    badge_runtime_health_snapshot_t health = {0};
    runtime_health_snapshot(&health);
    int64_t now_ms = esp_timer_get_time() / 1000;
    for (size_t i = 0; i < 2; ++i) {
        if (!badge_runtime_uart_heartbeat_fresh(
                health.scanner_uart_last_ms[i], now_ms,
                BADGE_RUNTIME_UART_HEARTBEAT_STALE_MS)) {
            return false;
        }
    }
    return true;
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

#if defined(FOF_DC34_GAME_CANARY)
bool badge_runtime_game_rtc_read(void *out, size_t record_size)
{
    if (!out || record_size != BADGE_CON_RTC_RECORD_BYTES) {
        return false;
    }
    portENTER_CRITICAL(&s_runtime_health_lock);
    bool valid = rtc_layout_valid();
    if (valid) {
        memcpy(
            out,
            g_fof_badge_rtc_state.game_rtc_record,
            BADGE_CON_RTC_RECORD_BYTES);
    }
    portEXIT_CRITICAL(&s_runtime_health_lock);
    return valid;
}

bool badge_runtime_game_rtc_write(
    const void *record, size_t record_size)
{
    if (!record || record_size != BADGE_CON_RTC_RECORD_BYTES) {
        return false;
    }
    portENTER_CRITICAL(&s_runtime_health_lock);
    bool valid = rtc_layout_valid();
    if (valid) {
        memcpy(
            g_fof_badge_rtc_state.game_rtc_record,
            record,
            BADGE_CON_RTC_RECORD_BYTES);
    }
    portEXIT_CRITICAL(&s_runtime_health_lock);
    return valid;
}

void badge_runtime_game_rtc_clear(void)
{
    portENTER_CRITICAL(&s_runtime_health_lock);
    if (rtc_layout_valid()) {
        memset(
            g_fof_badge_rtc_state.game_rtc_record,
            0,
            BADGE_CON_RTC_RECORD_BYTES);
    }
    portEXIT_CRITICAL(&s_runtime_health_lock);
}

bool badge_runtime_prepare_update(const char session[17])
{
    if (!session || !s_update_maintenance_initialized ||
        s_safe_mode ||
        !badge_update_session_valid(
            session, strnlen(session, BADGE_UPDATE_SESSION_CAPACITY))) {
        return false;
    }
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool accepted = false;
    portENTER_CRITICAL(&s_runtime_health_lock);
    if (badge_update_maintenance_marker_valid(
            &g_fof_badge_rtc_state.update_maintenance_marker)) {
        accepted = badge_update_maintenance_session_matches(
            &g_fof_badge_rtc_state.update_maintenance_marker, session,
            BADGE_UPDATE_SESSION_LENGTH);
    } else {
        memset(
            &g_fof_badge_rtc_state.update_maintenance_marker,
            0,
            sizeof(g_fof_badge_rtc_state.update_maintenance_marker));
        accepted = badge_update_maintenance_marker_prepare(
            &g_fof_badge_rtc_state.update_maintenance_marker, session,
            BADGE_UPDATE_SESSION_LENGTH);
    }
    if (accepted) {
        s_update_prepare_seen = true;
        s_update_last_prepare_request_ms = now_ms;
    }
    portEXIT_CRITICAL(&s_runtime_health_lock);
    return accepted;
}

bool badge_runtime_update_maintenance_active(void)
{
    portENTER_CRITICAL(&s_runtime_health_lock);
    bool active = s_update_maintenance_active &&
        badge_update_maintenance_marker_valid(
            &g_fof_badge_rtc_state.update_maintenance_marker) &&
        g_fof_badge_rtc_state.update_maintenance_marker.phase ==
            BADGE_UPDATE_PHASE_ACTIVE;
    portEXIT_CRITICAL(&s_runtime_health_lock);
    return active;
}

bool badge_runtime_update_preparing(void)
{
    portENTER_CRITICAL(&s_runtime_health_lock);
    bool preparing =
        badge_update_maintenance_marker_valid(
            &g_fof_badge_rtc_state.update_maintenance_marker) &&
        g_fof_badge_rtc_state.update_maintenance_marker.phase ==
            BADGE_UPDATE_PHASE_PREPARING;
    portEXIT_CRITICAL(&s_runtime_health_lock);
    return preparing;
}

bool badge_runtime_update_session_matches(const char session[17])
{
    if (!session) {
        return false;
    }
    portENTER_CRITICAL(&s_runtime_health_lock);
    bool matches = badge_update_maintenance_session_matches(
        &g_fof_badge_rtc_state.update_maintenance_marker, session,
        strnlen(session, BADGE_UPDATE_SESSION_CAPACITY));
    portEXIT_CRITICAL(&s_runtime_health_lock);
    return matches;
}

bool badge_runtime_update_session_copy(char session_out[17])
{
    if (!session_out) {
        return false;
    }
    session_out[0] = '\0';
    portENTER_CRITICAL(&s_runtime_health_lock);
    bool valid = badge_update_maintenance_marker_valid(
        &g_fof_badge_rtc_state.update_maintenance_marker);
    if (valid) {
        memcpy(
            session_out,
            g_fof_badge_rtc_state.update_maintenance_marker.session,
            BADGE_UPDATE_SESSION_CAPACITY);
    }
    portEXIT_CRITICAL(&s_runtime_health_lock);
    return valid;
}

void badge_runtime_update_keepalive(uint32_t now_ms)
{
    portENTER_CRITICAL(&s_runtime_health_lock);
    if (s_update_maintenance_active &&
        badge_update_maintenance_marker_valid(
            &g_fof_badge_rtc_state.update_maintenance_marker) &&
        g_fof_badge_rtc_state.update_maintenance_marker.phase ==
            BADGE_UPDATE_PHASE_ACTIVE) {
        s_update_last_activity_ms = now_ms;
    }
    portEXIT_CRITICAL(&s_runtime_health_lock);
}

bool badge_runtime_update_inactivity_due(uint32_t now_ms)
{
    portENTER_CRITICAL(&s_runtime_health_lock);
    bool due = s_update_maintenance_active &&
        badge_update_maintenance_marker_valid(
            &g_fof_badge_rtc_state.update_maintenance_marker) &&
        badge_update_maintenance_inactivity_due(
            now_ms, s_update_last_activity_ms);
    portEXIT_CRITICAL(&s_runtime_health_lock);
    return due;
}

bool badge_runtime_update_prepare_orphan_due(uint32_t now_ms)
{
    portENTER_CRITICAL(&s_runtime_health_lock);
    bool due = s_update_prepare_seen &&
        badge_update_maintenance_marker_valid(
            &g_fof_badge_rtc_state.update_maintenance_marker) &&
        g_fof_badge_rtc_state.update_maintenance_marker.phase ==
            BADGE_UPDATE_PHASE_PREPARING &&
        (uint32_t)(now_ms - s_update_last_prepare_request_ms) >=
            BADGE_RUNTIME_UPDATE_ORPHAN_GRACE_MS;
    portEXIT_CRITICAL(&s_runtime_health_lock);
    return due;
}

bool badge_runtime_update_health_can_mark_ota_valid(
    uint32_t free_internal_heap,
    uint32_t largest_internal_block,
    uint32_t uptime_ms)
{
    badge_runtime_health_snapshot_t health = {0};
    runtime_health_snapshot(&health);
    int64_t now_ms = esp_timer_get_time() / 1000;
    return s_update_maintenance_active &&
        badge_update_maintenance_health_satisfied(
            uptime_ms,
            health.display_alive,
            health.usb_response_completed,
            badge_runtime_uart_heartbeat_fresh(
                health.scanner_uart_last_ms[0], now_ms,
                BADGE_RUNTIME_UART_HEARTBEAT_STALE_MS),
            badge_runtime_uart_heartbeat_fresh(
                health.scanner_uart_last_ms[1], now_ms,
                BADGE_RUNTIME_UART_HEARTBEAT_STALE_MS),
            s_safe_mode,
            free_internal_heap,
            largest_internal_block);
}

bool badge_runtime_clear_update_maintenance(const char *reason)
{
    portENTER_CRITICAL(&s_runtime_health_lock);
    bool had_marker = badge_update_maintenance_marker_valid(
        &g_fof_badge_rtc_state.update_maintenance_marker);
    memset(
        &g_fof_badge_rtc_state.update_maintenance_marker,
        0,
        sizeof(g_fof_badge_rtc_state.update_maintenance_marker));
    s_update_maintenance_active = false;
    s_update_prepare_seen = false;
    s_update_last_prepare_request_ms = 0U;
    s_update_last_activity_ms = 0U;
    portEXIT_CRITICAL(&s_runtime_health_lock);
    ESP_LOGW(TAG, "Update-maintenance cleared: %s",
             reason ? reason : "unspecified");
    return had_marker;
}

bool badge_runtime_update_marker_snapshot(
    badge_update_maintenance_marker_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&s_runtime_health_lock);
    bool valid = badge_update_maintenance_marker_valid(
        &g_fof_badge_rtc_state.update_maintenance_marker);
    if (valid) {
        *out = g_fof_badge_rtc_state.update_maintenance_marker;
    }
    portEXIT_CRITICAL(&s_runtime_health_lock);
    return valid;
}

bool badge_runtime_abort_update_session(
    const char session[17], const char *reason)
{
    if (!session) {
        return false;
    }
    portENTER_CRITICAL(&s_runtime_health_lock);
    bool aborted = badge_update_maintenance_marker_abort(
        &g_fof_badge_rtc_state.update_maintenance_marker,
        session,
        strnlen(session, BADGE_UPDATE_SESSION_CAPACITY));
    if (aborted) {
        s_update_maintenance_active = false;
        s_update_prepare_seen = false;
        s_update_last_prepare_request_ms = 0U;
        s_update_last_activity_ms = 0U;
    }
    portEXIT_CRITICAL(&s_runtime_health_lock);
    if (aborted) {
        ESP_LOGW(TAG, "Update-maintenance exact session aborted: %s",
                 reason ? reason : "unspecified");
    }
    return aborted;
}

bool badge_runtime_update_commit_uplink(
    const char *version,
    const char *sha256,
    uint32_t size,
    const char *partition)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&s_runtime_health_lock);
    bool committed = s_update_maintenance_active &&
        badge_update_maintenance_marker_commit_uplink(
            &g_fof_badge_rtc_state.update_maintenance_marker,
            version, sha256, size, partition);
    if (committed) {
        s_update_last_activity_ms = now_ms;
    }
    portEXIT_CRITICAL(&s_runtime_health_lock);
    return committed;
}

bool badge_runtime_update_clear_uplink_commit(void)
{
    portENTER_CRITICAL(&s_runtime_health_lock);
    bool cleared = s_update_maintenance_active &&
        badge_update_maintenance_marker_clear_uplink(
            &g_fof_badge_rtc_state.update_maintenance_marker);
    portEXIT_CRITICAL(&s_runtime_health_lock);
    return cleared;
}
#endif
