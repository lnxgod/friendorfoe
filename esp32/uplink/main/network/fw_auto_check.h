#pragma once

/**
 * Friend or Foe — Automatic firmware check / self-update
 *
 * Periodic task that polls the backend for newer firmware:
 *
 *   1. uplink-s3 → if newer, downloads to inactive OTA partition,
 *      esp_ota_set_boot_partition + esp_restart. Existing rollback machinery
 *      in main.c reverts on first-boot WiFi failure.
 *
 *   2. scanner-s3-combo[-seed] (matched to whatever scanner is connected) →
 *      if newer than the fw_store cache, downloads + writes via the existing
 *      esp_ota_begin/write/abort + NVS metadata pattern. Next periodic
 *      fw_check from the scanner picks it up via the existing fw_offer flow.
 *
 * Skip conditions: WiFi disconnected, relay already in progress, low heap,
 * upload pause active. Exponential backoff on consecutive failures.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Pure decision helpers (host-testable) ───────────────────────────────── */

/**
 * Should the auto-check task make a check attempt this tick?
 *
 * @param free_heap_kb       Internal heap free, in KB. <50 KB = skip.
 * @param wifi_connected     STA connected.
 * @param relay_active       Manual fw_store relay running. We never
 *                           contend with a manual flash.
 * @param last_check_age_s   Seconds since last attempt; 0 means "never".
 * @param check_interval_s   Configured interval between checks.
 */
bool fw_auto_check_decide(int free_heap_kb,
                          bool wifi_connected,
                          bool relay_active,
                          int64_t last_check_age_s,
                          int64_t check_interval_s);

/**
 * Returns true if the remote version meaningfully differs from local.
 *
 * Plain string compare — we don't try to parse "0.63.0-svc155" vs
 * "0.63.18-rf-intel" as ordered numbers because the project uses two
 * concurrent naming schemes. If they differ, we trust the backend's
 * choice. Empty / "unknown" remote → false (never blindly downgrade).
 */
bool fw_auto_check_version_differs(const char *local, const char *remote);

/* ── Runtime API ─────────────────────────────────────────────────────────── */

/** Spawn the auto-check task. Idempotent. */
void fw_auto_check_init(void);

/** Status string for /api/status / heartbeat: "idle"|"checking"|"updating"|
 *  "error:<reason>". Static, do not free. */
const char *fw_auto_check_status(void);

/** Seconds since the last completed check (0 if never). */
int64_t fw_auto_check_last_age_s(void);

/** Most recent remote uplink version seen (or empty). */
const char *fw_auto_check_remote_uplink_version(void);

/** Most recent remote scanner version seen (or empty). */
const char *fw_auto_check_remote_scanner_version(void);

#ifdef __cplusplus
}
#endif
