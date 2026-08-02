#include "backend_scanner_runtime.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_task_wdt.h"
#endif

static bool time_source_valid(bool valid, const char *source)
{
    if (!source) {
        return false;
    }
    if (valid) {
        return strcmp(source, "sntp") == 0 ||
               strcmp(source, "backend") == 0;
    }
    return strcmp(source, "none") == 0;
}

bool backend_scanner_runtime_init(
    backend_scanner_runtime_t *runtime,
    uint32_t boot_id)
{
    if (!runtime || boot_id == 0U) {
        return false;
    }
    memset(runtime, 0, sizeof(*runtime));
    backend_scanner_role_init(&runtime->role, boot_id);
    backend_flow_state_init(&runtime->flow);
    backend_status_cadence_init(&runtime->status_cadence);
    memcpy(runtime->time_source, "none", sizeof("none"));
    runtime->state_changed = true;
    return true;
}

backend_scan_profile_t backend_scanner_runtime_profile(
    const backend_scanner_runtime_t *runtime)
{
    if (!runtime) {
        return BACKEND_SCAN_PROFILE_QUIESCENT;
    }
    return backend_scanner_role_effective(&runtime->role);
}

backend_scanner_role_result_t backend_scanner_runtime_apply_role(
    backend_scanner_runtime_t *runtime,
    uint32_t boot_id,
    uint32_t generation,
    backend_scan_profile_t profile)
{
    if (!runtime) {
        return BACKEND_ROLE_INVALID_BOOT;
    }
    const backend_scanner_role_result_t result = backend_scanner_role_apply(
        &runtime->role, boot_id, generation, profile);
    if (result == BACKEND_ROLE_APPLIED) {
        runtime->state_changed = true;
    }
    return result;
}

backend_flow_apply_result_t backend_scanner_runtime_apply_flow(
    backend_scanner_runtime_t *runtime,
    uint32_t generation,
    bool paused)
{
    if (!runtime) {
        return BACKEND_FLOW_INVALID_ARGUMENT;
    }
    const backend_flow_apply_result_t result = backend_flow_state_apply(
        &runtime->flow, generation, paused);
    if (result == BACKEND_FLOW_APPLIED) {
        runtime->state_changed = true;
    }
    return result;
}

backend_scanner_time_result_t backend_scanner_runtime_apply_time(
    backend_scanner_runtime_t *runtime,
    uint32_t generation,
    bool valid,
    int64_t epoch_ms,
    const char *source)
{
    if (!runtime) {
        return BACKEND_SCANNER_TIME_INVALID;
    }
    if (generation < runtime->time_generation) {
        return BACKEND_SCANNER_TIME_STALE;
    }
    if (generation == 0U || !time_source_valid(valid, source) ||
        (valid && !backend_coordinator_epoch_valid(epoch_ms)) ||
        (!valid && epoch_ms != 0)) {
        return BACKEND_SCANNER_TIME_INVALID;
    }
    if (strlen(source) >= sizeof(runtime->time_source)) {
        return BACKEND_SCANNER_TIME_INVALID;
    }
    if (generation == runtime->time_generation) {
        if (valid != runtime->time_valid || epoch_ms != runtime->epoch_ms ||
            strcmp(source, runtime->time_source) != 0) {
            return BACKEND_SCANNER_TIME_CONFLICT;
        }
        runtime->time_ack_pending = true;
        return BACKEND_SCANNER_TIME_REFRESHED;
    }

    runtime->time_generation = generation;
    runtime->time_valid = valid;
    runtime->epoch_ms = epoch_ms;
    memset(runtime->time_source, 0, sizeof(runtime->time_source));
    memcpy(runtime->time_source, source, strlen(source) + 1U);
    runtime->time_ack_pending = true;
    runtime->state_changed = true;
    return BACKEND_SCANNER_TIME_APPLIED;
}

bool backend_scanner_runtime_take_time_ack(
    backend_scanner_runtime_t *runtime,
    backend_scanner_time_ack_t *out)
{
    if (!runtime || !out || !runtime->time_ack_pending) {
        return false;
    }
    out->generation = runtime->time_generation;
    out->epoch_ms = runtime->epoch_ms;
    out->valid = runtime->time_valid;
    memcpy(out->source, runtime->time_source, sizeof(out->source));
    runtime->time_ack_pending = false;
    return true;
}

void backend_scanner_runtime_set_radio_health(
    backend_scanner_runtime_t *runtime,
    bool ble_healthy,
    bool wifi_healthy)
{
    if (!runtime) {
        return;
    }
    if (runtime->ble_healthy != ble_healthy ||
        runtime->wifi_healthy != wifi_healthy) {
        runtime->ble_healthy = ble_healthy;
        runtime->wifi_healthy = wifi_healthy;
        runtime->state_changed = true;
    }
}

void backend_scanner_runtime_set_ota_active(
    backend_scanner_runtime_t *runtime,
    bool ota_active)
{
    if (!runtime) {
        return;
    }
    if (runtime->ota_active != ota_active) {
        runtime->ota_active = ota_active;
        runtime->state_changed = true;
    }
}

bool backend_scanner_runtime_enqueue_detection(
    backend_scanner_runtime_t *runtime)
{
    if (!runtime || runtime->ota_active ||
        !backend_flow_detection_enqueue_allowed(
            runtime->flow.paused, runtime->detection_queue_depth)) {
        return false;
    }
    runtime->detection_queue_depth++;
    return true;
}

bool backend_scanner_runtime_complete_detection(
    backend_scanner_runtime_t *runtime)
{
    if (!runtime || runtime->detection_queue_depth == 0U) {
        return false;
    }
    runtime->detection_queue_depth--;
    return true;
}

bool backend_scanner_runtime_enqueue_control(
    backend_scanner_runtime_t *runtime)
{
    if (!runtime || !backend_flow_control_enqueue_allowed(
            runtime->control_queue_depth)) {
        return false;
    }
    runtime->control_queue_depth++;
    return true;
}

bool backend_scanner_runtime_complete_control(
    backend_scanner_runtime_t *runtime)
{
    if (!runtime || runtime->control_queue_depth == 0U) {
        return false;
    }
    runtime->control_queue_depth--;
    return true;
}

bool backend_scanner_runtime_status_due(
    backend_scanner_runtime_t *runtime,
    int64_t now_ms)
{
    if (!runtime) {
        return false;
    }
    return backend_status_cadence_due(
        &runtime->status_cadence, now_ms, runtime->state_changed);
}

bool backend_scanner_runtime_status_sent(
    backend_scanner_runtime_t *runtime,
    int64_t now_ms)
{
    if (!runtime || !backend_status_cadence_due(
            &runtime->status_cadence, now_ms, runtime->state_changed) ||
        !backend_status_cadence_mark_sent(
            &runtime->status_cadence, now_ms)) {
        return false;
    }
    runtime->state_changed = false;
    return true;
}

uint8_t backend_scanner_runtime_required_restart_mask(
    const backend_scanner_runtime_t *runtime)
{
    switch (backend_scanner_runtime_profile(runtime)) {
    case BACKEND_SCAN_PROFILE_BLE_PRIMARY:
        return BACKEND_SCANNER_RADIO_BLE;
    case BACKEND_SCAN_PROFILE_WIFI_PRIMARY:
        return BACKEND_SCANNER_RADIO_WIFI;
    case BACKEND_SCAN_PROFILE_HYBRID_FAILOVER:
        return BACKEND_SCANNER_RADIO_BLE | BACKEND_SCANNER_RADIO_WIFI;
    case BACKEND_SCAN_PROFILE_QUIESCENT:
    default:
        return 0U;
    }
}

bool backend_scanner_runtime_wdt_register_current(void)
{
#ifdef ESP_PLATFORM
    return esp_task_wdt_add(NULL) == ESP_OK;
#else
    return true;
#endif
}

bool backend_scanner_runtime_worker_iteration(
    backend_scanner_runtime_t *runtime,
    uint32_t worker)
{
    if (!runtime) {
        return false;
    }
    uint32_t completed = runtime->watchdog_completed_mask;
    if (!backend_watchdog_mark_iteration(&completed, worker)) {
        return false;
    }
#ifdef ESP_PLATFORM
    if (esp_task_wdt_reset() != ESP_OK) {
        return false;
    }
#endif
    runtime->watchdog_completed_mask = completed;
    return true;
}

bool backend_scanner_runtime_rollback_ready(
    const backend_scanner_runtime_t *runtime)
{
    return runtime && backend_watchdog_ready(
        BACKEND_WATCHDOG_SCANNER_REQUIRED_MASK,
        runtime->watchdog_completed_mask);
}
