#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_hardware_profile.h"
#include "backend_flow_policy.h"
#include "backend_recovery_policy.h"
#include "backend_scanner_role.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_SCANNER_UART_PORT FOF_BACKEND_SCANNER_UART_PORT
#define BACKEND_SCANNER_UART_TX_GPIO FOF_BACKEND_SCANNER_UART_TX_PIN
#define BACKEND_SCANNER_UART_RX_GPIO FOF_BACKEND_SCANNER_UART_RX_PIN
#define BACKEND_SCANNER_UART_BAUD FOF_BACKEND_SCANNER_UART_BAUD

#define BACKEND_SCANNER_TIME_SOURCE_CAPACITY 8U
#define BACKEND_SCANNER_RADIO_BLE UINT8_C(0x01)
#define BACKEND_SCANNER_RADIO_WIFI UINT8_C(0x02)

typedef enum {
    BACKEND_SCANNER_TIME_APPLIED = 0,
    BACKEND_SCANNER_TIME_REFRESHED,
    BACKEND_SCANNER_TIME_STALE,
    BACKEND_SCANNER_TIME_CONFLICT,
    BACKEND_SCANNER_TIME_INVALID,
} backend_scanner_time_result_t;

typedef struct {
    uint32_t generation;
    int64_t epoch_ms;
    char source[BACKEND_SCANNER_TIME_SOURCE_CAPACITY];
    bool valid;
} backend_scanner_time_ack_t;

typedef struct {
    backend_scanner_role_state_t role;
    backend_flow_state_t flow;
    backend_status_cadence_t status_cadence;
    uint32_t watchdog_completed_mask;
    uint32_t time_generation;
    int64_t epoch_ms;
    size_t detection_queue_depth;
    size_t control_queue_depth;
    char time_source[BACKEND_SCANNER_TIME_SOURCE_CAPACITY];
    bool time_valid;
    bool ota_active;
    bool ble_healthy;
    bool wifi_healthy;
    bool state_changed;
    bool time_ack_pending;
} backend_scanner_runtime_t;

bool backend_scanner_runtime_init(
    backend_scanner_runtime_t *runtime,
    uint32_t boot_id);

backend_scan_profile_t backend_scanner_runtime_profile(
    const backend_scanner_runtime_t *runtime);

backend_scanner_role_result_t backend_scanner_runtime_apply_role(
    backend_scanner_runtime_t *runtime,
    uint32_t boot_id,
    uint32_t generation,
    backend_scan_profile_t profile);

backend_flow_apply_result_t backend_scanner_runtime_apply_flow(
    backend_scanner_runtime_t *runtime,
    uint32_t generation,
    bool paused);

backend_scanner_time_result_t backend_scanner_runtime_apply_time(
    backend_scanner_runtime_t *runtime,
    uint32_t generation,
    bool valid,
    int64_t epoch_ms,
    const char *source);

bool backend_scanner_runtime_take_time_ack(
    backend_scanner_runtime_t *runtime,
    backend_scanner_time_ack_t *out);

void backend_scanner_runtime_set_radio_health(
    backend_scanner_runtime_t *runtime,
    bool ble_healthy,
    bool wifi_healthy);

void backend_scanner_runtime_set_ota_active(
    backend_scanner_runtime_t *runtime,
    bool ota_active);

bool backend_scanner_runtime_enqueue_detection(
    backend_scanner_runtime_t *runtime);
bool backend_scanner_runtime_complete_detection(
    backend_scanner_runtime_t *runtime);
bool backend_scanner_runtime_enqueue_control(
    backend_scanner_runtime_t *runtime);
bool backend_scanner_runtime_complete_control(
    backend_scanner_runtime_t *runtime);

bool backend_scanner_runtime_status_due(
    backend_scanner_runtime_t *runtime,
    int64_t now_ms);
bool backend_scanner_runtime_status_sent(
    backend_scanner_runtime_t *runtime,
    int64_t now_ms);

uint8_t backend_scanner_runtime_required_restart_mask(
    const backend_scanner_runtime_t *runtime);

bool backend_scanner_runtime_worker_iteration(
    backend_scanner_runtime_t *runtime,
    uint32_t worker);
uint32_t backend_scanner_runtime_required_watchdog_mask(
    backend_scan_profile_t profile);
bool backend_scanner_runtime_rollback_ready(
    const backend_scanner_runtime_t *runtime);

bool backend_scanner_runtime_wdt_register_current(void);

#ifdef __cplusplus
}
#endif
