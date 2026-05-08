#pragma once

/**
 * Friend or Foe — Scanner OTA Rollback + Crash-Loop Guard
 *
 * Mirrors the uplink rollback machinery in uplink/main/main.c. Two layers
 * of protection for a bad scanner OTA:
 *
 *   1. ESP-IDF rollback: a fresh OTA boots in PENDING_VERIFY. If a watchdog
 *      fires before we prove ourselves, the previous slot is restored on
 *      next boot via esp_ota_mark_app_invalid_rollback_and_reboot().
 *
 *   2. Crash-loop counter (NVS): if we keep panicking on a *validated* image
 *      (nothing to roll back to), the count is reflected in fw_state /
 *      last_fw_error so the next fw_check we send to the uplink carries
 *      "reason: crash_loop:N". The uplink's existing fw_check / fw_offer /
 *      fw_ready flow then re-delivers the cached firmware.
 *
 * Call order in app_main():
 *
 *   nvs_flash_init();
 *   scanner_rollback_init();   ← reads PENDING_VERIFY + crash counter
 *   ... rest of init ...
 *   xTaskCreate(scanner_rollback_mark_valid_task, ...);  ← marks valid after
 *                                                          STABLE_BOOT_S
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Pure decision function (host-testable) ─────────────────────────────── */

typedef enum {
    /* No action — boot normally. */
    ROLLBACK_ACTION_NONE = 0,
    /* Crashing on a validated image (no rollback target). Surface in
     * fw_state so the next fw_check carries crash_loop reason and the
     * uplink can re-deliver firmware. */
    ROLLBACK_ACTION_MARK_CRASH_LOOP,
    /* Crashed while running PENDING_VERIFY — bad OTA, revert to previous
     * slot via esp_ota_mark_app_invalid_rollback_and_reboot(). */
    ROLLBACK_ACTION_FORCE_ROLLBACK,
} rollback_action_t;

typedef struct {
    rollback_action_t action;
    uint32_t          new_crash_count;
    bool              reset_was_crash;
    bool              enter_safe_mode;
} rollback_decision_t;

/**
 * Pure policy: what to do at boot, given the reset reason and prior state.
 *
 * @param reset_reason   esp_reset_reason() return as int (so tests don't
 *                       have to depend on esp_system.h enums).
 * @param pending_verify True if running PENDING_VERIFY OTA partition.
 * @param prior_crash_count  Crash counter from NVS at boot.
 * @param crash_loop_threshold  Boots required to call it a crash loop.
 */
rollback_decision_t scanner_rollback_decide(int reset_reason,
                                            bool pending_verify,
                                            uint32_t prior_crash_count,
                                            uint32_t crash_loop_threshold);

/* ── Runtime API (target-only — uses esp_ota / nvs / freertos) ──────────── */

/**
 * Read PENDING_VERIFY state of running partition, increment crash counter
 * if the previous boot ended in panic / watchdog. May reboot via rollback
 * if it determines the running image is bad.
 *
 * Idempotent — safe to call once early in app_main(), after nvs_flash_init().
 */
void scanner_rollback_init(void);

/** True if the running app partition is PENDING_VERIFY (fresh OTA, unproven). */
bool scanner_rollback_is_pending_verify(void);

/** Number of consecutive crash boots (panic, WDT) since the last mark_valid. */
uint32_t scanner_rollback_crash_count(void);

/** True when the scanner should stay in UART-only recovery mode. */
bool scanner_rollback_safe_mode_requested(void);

/** Human-readable recovery mode label for status/debug surfaces. */
const char *scanner_rollback_recovery_mode(void);

/** Reason associated with UART-only recovery mode, if active. */
const char *scanner_rollback_safe_reason(void);

/** Persistently request or clear UART-only safe mode on next boot. */
void scanner_rollback_force_safe_mode(bool enabled, const char *reason);

/** Clear crash-loop state and any forced safe-mode request. */
void scanner_rollback_clear_crash_state(void);

/**
 * Mark the running app VALID and clear the crash counter. Call once after
 * the boot is proven stable (e.g. STABLE_BOOT_S of uptime + at least one
 * UART command RX, or first successful fw_check exchange).
 *
 * No-op if the partition is already valid. Safe to call multiple times.
 */
void scanner_rollback_mark_valid(void);

/**
 * Reboot path. If we are PENDING_VERIFY, calls
 * esp_ota_mark_app_invalid_rollback_and_reboot() — boots the previous slot.
 * Otherwise calls esp_restart().
 *
 * Use this from watchdog tasks instead of raw esp_restart() so a freshly-
 * OTA'd scanner that goes silent can self-heal.
 */
void scanner_rollback_reboot_or_restart(const char *reason) __attribute__((noreturn));

/**
 * Long-running task that marks the boot valid after a stable-uptime
 * threshold. Pin to any core, very low priority. Self-deletes after
 * marking valid.
 */
void scanner_rollback_mark_valid_task(void *arg);

#ifdef __cplusplus
}
#endif
