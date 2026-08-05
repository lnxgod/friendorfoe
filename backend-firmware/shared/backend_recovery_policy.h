#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_STATUS_STALE_AFTER_MS INT64_C(6000)
#define BACKEND_STATUS_PROBE_INTERVAL_MS INT64_C(1000)
#define BACKEND_STATUS_PROBE_COUNT 3U
#define BACKEND_STATUS_UART_REINIT_AFTER_MS INT64_C(9000)
#define BACKEND_STATUS_UNAVAILABLE_AFTER_MS INT64_C(15000)
#define BACKEND_STATUS_EMIT_INTERVAL_MS INT64_C(2000)
#define BACKEND_COORDINATOR_COMMAND_INTERVAL_MS INT64_C(10000)
#define BACKEND_WORKER_WATCHDOG_BUDGET_MS INT64_C(30000)

typedef enum {
    BACKEND_RECOVERY_NONE = 0,
    BACKEND_RECOVERY_SEND_PROBE,
    BACKEND_RECOVERY_REINIT_LOCAL_UART,
    BACKEND_RECOVERY_MARK_UNAVAILABLE,
    BACKEND_RECOVERY_SEND_RESTART_RADIOS,
} backend_recovery_action_t;

typedef enum {
    BACKEND_STATUS_BOOT_FIRST = 0,
    BACKEND_STATUS_BOOT_UNCHANGED,
    BACKEND_STATUS_BOOT_CHANGED,
    BACKEND_STATUS_BOOT_INVALID,
} backend_status_boot_result_t;

typedef enum {
    BACKEND_REMOTE_RECOVERY_APPLIED = 0,
    BACKEND_REMOTE_RECOVERY_REFRESHED,
    BACKEND_REMOTE_RECOVERY_STALE,
    BACKEND_REMOTE_RECOVERY_INVALID_BOOT,
    BACKEND_REMOTE_RECOVERY_INVALID_GENERATION,
    BACKEND_REMOTE_RECOVERY_INVALID_ARGUMENT,
} backend_remote_recovery_result_t;

typedef struct {
    int64_t monitoring_started_ms;
    int64_t last_valid_status_ms;
    int64_t last_probe_ms;
    int64_t pending_action_issued_ms;
    uint32_t boot_id;
    uint32_t remote_restart_generation;
    uint8_t probes_sent;
    backend_recovery_action_t pending_action;
    bool have_status;
    bool uart_reinitialized;
    bool unavailable;
    bool remote_restart_pending;
} backend_recovery_policy_t;

typedef struct {
    int64_t last_emit_ms;
    bool emitted;
    bool change_pending;
} backend_status_cadence_t;

typedef struct {
    int64_t last_emit_ms;
    bool emitted;
    bool immediate_pending;
} backend_command_cadence_t;

typedef enum {
    BACKEND_WORKER_UART_RX_CONTROL = UINT32_C(1) << 0,
    BACKEND_WORKER_COORDINATOR = UINT32_C(1) << 1,
    BACKEND_WORKER_BLE_RADIO = UINT32_C(1) << 2,
    BACKEND_WORKER_WIFI_RADIO = UINT32_C(1) << 3,
    BACKEND_WORKER_UPLOADER = UINT32_C(1) << 4,
    BACKEND_WORKER_COMMAND_CLIENT = UINT32_C(1) << 5,
    BACKEND_WORKER_OTA = UINT32_C(1) << 6,
} backend_worker_mask_t;

#define BACKEND_WATCHDOG_SCANNER_REQUIRED_MASK \
    (BACKEND_WORKER_UART_RX_CONTROL | BACKEND_WORKER_BLE_RADIO | \
     BACKEND_WORKER_WIFI_RADIO | BACKEND_WORKER_OTA)

#define BACKEND_WATCHDOG_UPLINK_REQUIRED_MASK \
    (BACKEND_WORKER_UART_RX_CONTROL | BACKEND_WORKER_COORDINATOR | \
     BACKEND_WORKER_UPLOADER | BACKEND_WORKER_COMMAND_CLIENT | \
     BACKEND_WORKER_OTA)

void backend_recovery_policy_init(
    backend_recovery_policy_t *policy,
    int64_t now_ms);

backend_status_boot_result_t backend_recovery_policy_note_status(
    backend_recovery_policy_t *policy,
    uint32_t boot_id,
    int64_t now_ms);

backend_remote_recovery_result_t backend_recovery_policy_request_restart(
    backend_recovery_policy_t *policy,
    uint32_t boot_id,
    uint32_t generation);

backend_recovery_action_t backend_recovery_policy_tick(
    backend_recovery_policy_t *policy,
    int64_t now_ms,
    bool ota_active);

bool backend_recovery_policy_complete_action(
    backend_recovery_policy_t *policy,
    backend_recovery_action_t action,
    bool succeeded,
    int64_t now_ms);

void backend_status_cadence_init(backend_status_cadence_t *cadence);
bool backend_status_cadence_due(
    backend_status_cadence_t *cadence,
    int64_t now_ms,
    bool state_changed);
bool backend_status_cadence_mark_sent(
    backend_status_cadence_t *cadence,
    int64_t now_ms);

void backend_command_cadence_init(backend_command_cadence_t *cadence);
bool backend_command_cadence_due(
    backend_command_cadence_t *cadence,
    int64_t now_ms,
    bool boot_or_link_changed);
bool backend_command_cadence_mark_sent(
    backend_command_cadence_t *cadence,
    int64_t now_ms);

bool backend_watchdog_mark_iteration(
    uint32_t *completed_mask,
    uint32_t worker);
bool backend_watchdog_ready(uint32_t required_mask, uint32_t completed_mask);

bool backend_coordinator_epoch_valid(int64_t epoch_ms);

#ifdef __cplusplus
}
#endif
