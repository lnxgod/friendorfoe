#pragma once

/**
 * Friend or Foe — BLE Scanner Task Priorities & Stack Sizes
 *
 * FreeRTOS task configuration for the dedicated BLE Scanner board.
 *
 * ESP32-S3 (dual-core): BLE on core 0, processing on core 1.
 * ESP32-C5/C3 (single-core): all tasks use tskNO_AFFINITY.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Task priorities (higher number = higher priority) ───────────────────── */

#define BLE_SCAN_TASK_PRIORITY      5
#define CONSOLE_TX_TASK_PRIORITY    3
#define DISPLAY_TASK_PRIORITY       2
#define LED_TASK_PRIORITY           1

/* ── Stack sizes (bytes) ─────────────────────────────────────────────────── */

#define BLE_SCAN_TASK_STACK_SIZE    4096
#define CONSOLE_TX_TASK_STACK_SIZE  4096
#define DISPLAY_TASK_STACK_SIZE     8192
#define LED_TASK_STACK_SIZE         2048

/* ── Core affinity ───────────────────────────────────────────────────────── */

#ifdef CONFIG_FREERTOS_UNICORE
/* Single-core (ESP32-C5/C3): no pinning, scheduler handles it */
#define BLE_SCAN_TASK_CORE          tskNO_AFFINITY
#define CONSOLE_TX_TASK_CORE        tskNO_AFFINITY
#define DISPLAY_TASK_CORE           tskNO_AFFINITY
#else
/* Dual-core (ESP32-S3): BLE on core 0, processing on core 1 */
#define CORE_RADIO                  0
#define CORE_PROCESSING             1

#define BLE_SCAN_TASK_CORE          CORE_RADIO
#define CONSOLE_TX_TASK_CORE        CORE_PROCESSING
#define DISPLAY_TASK_CORE           CORE_PROCESSING
#endif

#ifdef __cplusplus
}
#endif
