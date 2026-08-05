#include "scanner_rollback.h"

#ifndef SCANNER_ROLLBACK_HOST_TEST
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "uart_tx.h"   /* uart_tx_set_firmware_error / clear */
#endif

#include <stdio.h>
#include <string.h>

#ifndef SCANNER_ROLLBACK_HOST_TEST
static const char *TAG = "rollback";

/* NVS namespace + key for the crash counter. Persists across reboots. */
#define NVS_NS          "fof_rb"
#define NVS_KEY_CRASH_N "crash_n"
#define NVS_KEY_FORCE_SAFE "force_safe"
#define NVS_KEY_SAFE_REASON "safe_reason"
#define NVS_KEY_CLEAR_ONCE "clear_once"
#define SAFE_CLEAR_ONCE_MAGIC 0x53434631u
#define SAFE_CLEAR_ONCE_CHECK 0xACBCA9CEu
#endif

/* Marks-valid threshold. Mirrors uplink intent: long enough that a
 * crashing image has had time to crash, short enough that we don't sit
 * in PENDING_VERIFY forever. Uplink uses "first WiFi association"; we
 * use uptime because UART command RX is asynchronous from the uplink
 * side and may take longer than 60 s on a fresh fleet boot. */
#define STABLE_BOOT_S       60
#define CRASH_LOOP_THRESHOLD 3

#ifndef SCANNER_ROLLBACK_HOST_TEST
static volatile bool     s_pending_verify = false;
static volatile uint32_t s_crash_count    = 0;
static volatile bool     s_safe_mode_requested = false;
static char              s_safe_reason[64] = {0};
RTC_NOINIT_ATTR static uint32_t s_safe_clear_once_magic;
RTC_NOINIT_ATTR static uint32_t s_safe_clear_once_check;
RTC_NOINIT_ATTR static uint32_t s_boot_stage;
static volatile uint32_t s_last_boot_stage = SCANNER_BOOT_STAGE_UNKNOWN;
static volatile uint32_t s_last_reset_reason = 0;
#endif

/* Mirrors esp_reset_reason_t values we care about. Kept as ints in the
 * pure policy API so host tests don't need esp_system.h. */
#define RST_PANIC     4   /* ESP_RST_PANIC */
#define RST_INT_WDT   5   /* ESP_RST_INT_WDT */
#define RST_TASK_WDT  6   /* ESP_RST_TASK_WDT */
#define RST_WDT       7   /* ESP_RST_WDT */

static bool reset_reason_is_crash(int r)
{
    /* ESP_RST_BROWNOUT is excluded on purpose: brownouts indicate a power
     * problem, not a firmware bug. Re-flashing won't help; rolling back
     * might cause spurious version churn during a noisy power event. */
    return r == RST_PANIC ||
           r == RST_INT_WDT ||
           r == RST_TASK_WDT ||
           r == RST_WDT;
}

rollback_decision_t scanner_rollback_decide(int reset_reason,
                                            bool pending_verify,
                                            uint32_t prior_crash_count,
                                            uint32_t crash_loop_threshold)
{
    rollback_decision_t d = {
        .action = ROLLBACK_ACTION_NONE,
        .new_crash_count = prior_crash_count,
        .reset_was_crash = false,
    };

    if (!reset_reason_is_crash(reset_reason)) {
        /* Clean boot. Crash counter stays — only mark_valid() clears it,
         * because we haven't yet proven this boot is stable. */
        return d;
    }

    d.reset_was_crash = true;
    d.new_crash_count = prior_crash_count + 1;

    if (pending_verify) {
        /* Bad OTA: previous boot crashed while still PENDING_VERIFY.
         * Revert to the last-known-good slot immediately — no point
         * letting the bad image keep panicking. */
        d.action = ROLLBACK_ACTION_FORCE_ROLLBACK;
        return d;
    }

    if (d.new_crash_count >= crash_loop_threshold) {
        /* Validated image but it keeps crashing. Surface so the next
         * fw_check carries crash_loop reason; uplink will offer cached
         * firmware via the existing fw_offer / fw_ready flow. */
        d.action = ROLLBACK_ACTION_MARK_CRASH_LOOP;
        d.enter_safe_mode = true;
    }
    return d;
}

#ifndef SCANNER_ROLLBACK_HOST_TEST

static const char *boot_stage_label(uint32_t stage)
{
    switch ((scanner_boot_stage_t)stage) {
        case SCANNER_BOOT_STAGE_ROLLBACK: return "rollback";
        case SCANNER_BOOT_STAGE_NETIF: return "netif";
        case SCANNER_BOOT_STAGE_EVENT_LOOP: return "event";
        case SCANNER_BOOT_STAGE_UART: return "uart";
        case SCANNER_BOOT_STAGE_WIFI_INIT: return "wifi_init";
        case SCANNER_BOOT_STAGE_WIFI_READY: return "wifi_ready";
        case SCANNER_BOOT_STAGE_BLE_INIT: return "ble_init";
        case SCANNER_BOOT_STAGE_BLE_READY: return "ble_ready";
        case SCANNER_BOOT_STAGE_NORMAL: return "normal";
        case SCANNER_BOOT_STAGE_UNKNOWN:
        default:
            return "unknown";
    }
}

void scanner_rollback_note_boot_stage(scanner_boot_stage_t stage)
{
    if (stage < SCANNER_BOOT_STAGE_UNKNOWN || stage > SCANNER_BOOT_STAGE_NORMAL) {
        stage = SCANNER_BOOT_STAGE_UNKNOWN;
    }
    s_boot_stage = (uint32_t)stage;
}

static uint32_t crash_count_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    uint32_t n = 0;
    nvs_get_u32(h, NVS_KEY_CRASH_N, &n);
    nvs_close(h);
    return n;
}

static void crash_count_store(uint32_t n)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed; crash counter not persisted");
        return;
    }
    nvs_set_u32(h, NVS_KEY_CRASH_N, n);
    nvs_commit(h);
    nvs_close(h);
}

static uint32_t nvs_get_u32_default(const char *key, uint32_t fallback)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return fallback;
    uint32_t value = fallback;
    nvs_get_u32(h, key, &value);
    nvs_close(h);
    return value;
}

static void nvs_set_u32_value(const char *key, uint32_t value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed; %s not persisted", key);
        return;
    }
    nvs_set_u32(h, key, value);
    nvs_commit(h);
    nvs_close(h);
}

static void recovery_nvs_erase_all(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed; recovery namespace not erased");
        return;
    }
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}

static void mark_pending_verify_valid(const char *why)
{
    if (!s_pending_verify) {
        return;
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        s_pending_verify = false;
        ESP_LOGW(TAG, "OTA: image marked VALID (%s)",
                 why && why[0] ? why : "scanner recovery");
    } else {
        ESP_LOGW(TAG, "OTA: mark_valid failed during %s: %s",
                 why && why[0] ? why : "scanner recovery",
                 esp_err_to_name(err));
    }
}

static void safe_reason_load(char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = out_len;
    if (nvs_get_str(h, NVS_KEY_SAFE_REASON, out, &len) != ESP_OK) {
        out[0] = '\0';
    }
    nvs_close(h);
}

static void safe_reason_store(const char *reason)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed; safe reason not persisted");
        return;
    }
    nvs_set_str(h, NVS_KEY_SAFE_REASON,
                (reason && reason[0]) ? reason : "operator");
    nvs_commit(h);
    nvs_close(h);
}

void scanner_rollback_init(void)
{
    s_last_boot_stage =
        (s_boot_stage <= SCANNER_BOOT_STAGE_NORMAL)
            ? s_boot_stage
            : SCANNER_BOOT_STAGE_UNKNOWN;
    scanner_rollback_note_boot_stage(SCANNER_BOOT_STAGE_ROLLBACK);

    bool safe_clear_once =
        (s_safe_clear_once_magic == SAFE_CLEAR_ONCE_MAGIC &&
         s_safe_clear_once_check == SAFE_CLEAR_ONCE_CHECK) ||
        nvs_get_u32_default(NVS_KEY_CLEAR_ONCE, 0) == 1;
    if (safe_clear_once) {
        s_safe_clear_once_magic = 0;
        s_safe_clear_once_check = 0;
        recovery_nvs_erase_all();
        ESP_LOGW(TAG, "Operator safe-mode clear armed: bypassing stale scanner "
                      "recovery once");
    }

    uint32_t forced_safe = safe_clear_once
        ? 0
        : nvs_get_u32_default(NVS_KEY_FORCE_SAFE, 0);
    if (forced_safe != 0) {
        s_safe_mode_requested = true;
        safe_reason_load(s_safe_reason, sizeof(s_safe_reason));
        if (!s_safe_reason[0]) {
            strncpy(s_safe_reason, "forced_safe", sizeof(s_safe_reason) - 1);
        }
        ESP_LOGW(TAG, "UART-only recovery forced by NVS: %s", s_safe_reason);
    }

    /* 1. PENDING_VERIFY check. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
        if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
            if (state == ESP_OTA_IMG_PENDING_VERIFY) {
                s_pending_verify = true;
                ESP_LOGW(TAG, "OTA: running PENDING_VERIFY partition '%s' — "
                              "will mark valid after %d s of stable uptime",
                         running->label, STABLE_BOOT_S);
            } else {
                ESP_LOGI(TAG, "OTA: running partition '%s' state=%d (validated)",
                         running->label, state);
            }
        }
    }

    /* 2. Apply the pure policy: increment counter, decide action. */
    esp_reset_reason_t reason = esp_reset_reason();
    s_last_reset_reason = (uint32_t)reason;
    uint32_t prior = safe_clear_once ? 0 : crash_count_load();
    rollback_decision_t d = scanner_rollback_decide(
        (int)reason, s_pending_verify, prior, CRASH_LOOP_THRESHOLD);

    if (d.reset_was_crash) {
        crash_count_store(d.new_crash_count);
        ESP_LOGE(TAG, "Boot after crash: reset_reason=%d crash_count=%lu",
                 (int)reason, (unsigned long)d.new_crash_count);
    } else {
        ESP_LOGI(TAG, "Boot reason=%d crash_count=%lu (carried)",
                 (int)reason, (unsigned long)d.new_crash_count);
    }
    s_crash_count = d.new_crash_count;

    if (d.action == ROLLBACK_ACTION_FORCE_ROLLBACK) {
        ESP_LOGE(TAG, "OTA ROLLBACK: crash on PENDING_VERIFY image — "
                      "reverting to previous slot");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_ota_mark_app_invalid_rollback_and_reboot();
        /* Falls through to normal boot only if rollback fails — extremely
         * unlikely once we've confirmed PENDING_VERIFY. */
    } else if (d.action == ROLLBACK_ACTION_MARK_CRASH_LOOP) {
        char buf[48];
        snprintf(buf, sizeof(buf), "crash_loop:%lu@%s/r%lu",
                 (unsigned long)d.new_crash_count,
                 boot_stage_label(s_last_boot_stage),
                 (unsigned long)s_last_reset_reason);
        uart_tx_set_firmware_error(buf);
        if (d.enter_safe_mode) {
            s_safe_mode_requested = true;
            strncpy(s_safe_reason, buf, sizeof(s_safe_reason) - 1);
            s_safe_reason[sizeof(s_safe_reason) - 1] = '\0';
        }
        ESP_LOGE(TAG, "CRASH LOOP (%lu boots) on validated image — fw_state "
                      "carries '%s'; uplink will re-offer cached firmware",
                 (unsigned long)d.new_crash_count, buf);
    }

    if (s_safe_mode_requested) {
        mark_pending_verify_valid("safe_uart command path alive");
    }
}

bool scanner_rollback_is_pending_verify(void)
{
    return s_pending_verify;
}

uint32_t scanner_rollback_crash_count(void)
{
    return s_crash_count;
}

bool scanner_rollback_safe_mode_requested(void)
{
    return s_safe_mode_requested;
}

const char *scanner_rollback_recovery_mode(void)
{
    if (s_safe_mode_requested) {
        return "safe_uart";
    }
    return s_pending_verify ? "ota_pending" : "normal";
}

const char *scanner_rollback_safe_reason(void)
{
    return s_safe_reason[0] ? s_safe_reason : "";
}

void scanner_rollback_force_safe_mode(bool enabled, const char *reason)
{
    nvs_set_u32_value(NVS_KEY_FORCE_SAFE, enabled ? 1 : 0);
    if (enabled) {
        s_safe_clear_once_magic = 0;
        s_safe_clear_once_check = 0;
        safe_reason_store(reason && reason[0] ? reason : "operator");
        s_safe_mode_requested = true;
        strncpy(s_safe_reason,
                reason && reason[0] ? reason : "operator",
                sizeof(s_safe_reason) - 1);
        s_safe_reason[sizeof(s_safe_reason) - 1] = '\0';
    } else {
        s_safe_clear_once_magic = SAFE_CLEAR_ONCE_MAGIC;
        s_safe_clear_once_check = SAFE_CLEAR_ONCE_CHECK;
        recovery_nvs_erase_all();
        nvs_set_u32_value(NVS_KEY_CLEAR_ONCE, 1);
        mark_pending_verify_valid("scanner safe-mode clear");
        s_crash_count = 0;
        s_safe_mode_requested = false;
        s_safe_reason[0] = '\0';
        uart_tx_clear_firmware_error();
    }
}

void scanner_rollback_clear_crash_state(void)
{
    s_safe_clear_once_magic = SAFE_CLEAR_ONCE_MAGIC;
    s_safe_clear_once_check = SAFE_CLEAR_ONCE_CHECK;
    recovery_nvs_erase_all();
    nvs_set_u32_value(NVS_KEY_CLEAR_ONCE, 1);
    mark_pending_verify_valid("scanner crash-state clear");
    s_crash_count = 0;
    s_safe_mode_requested = false;
    s_safe_reason[0] = '\0';
    uart_tx_clear_firmware_error();
}

void scanner_rollback_mark_valid(void)
{
    if (s_safe_mode_requested) {
        ESP_LOGW(TAG, "mark_valid skipped while UART-only recovery is active");
        return;
    }
    if (s_pending_verify) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            s_pending_verify = false;
            ESP_LOGW(TAG, "OTA: image marked VALID (rollback cancelled)");
        } else {
            ESP_LOGE(TAG, "OTA: mark_valid failed: %s — will retry next call",
                     esp_err_to_name(err));
            return;
        }
    }
    if (s_crash_count != 0) {
        crash_count_store(0);
        s_crash_count = 0;
        uart_tx_clear_firmware_error();
        ESP_LOGW(TAG, "crash counter reset (boot proven stable)");
    }
}

void scanner_rollback_reboot_or_restart(const char *reason)
{
    if (s_pending_verify) {
        ESP_LOGE(TAG, "OTA ROLLBACK: %s while PENDING_VERIFY — reverting to previous slot",
                 reason ? reason : "(no reason)");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_ota_mark_app_invalid_rollback_and_reboot();
        /* If rollback fails (e.g. no valid fallback partition), fall through
         * to a normal restart so we don't sit in a halted state. */
    }
    ESP_LOGE(TAG, "WATCHDOG REBOOT: %s", reason ? reason : "(no reason)");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

void scanner_rollback_mark_valid_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(STABLE_BOOT_S * 1000));
    scanner_rollback_mark_valid();
    vTaskDelete(NULL);
}

#endif /* SCANNER_ROLLBACK_HOST_TEST */
