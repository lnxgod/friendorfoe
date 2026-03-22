#pragma once

/**
 * Friend or Foe — Scanner Task Priorities & Stack Sizes
 *
 * FreeRTOS task configuration for the Scanner board.
 *
 * ESP32-S3 (dual-core): radio tasks pinned to core 0 (WiFi/BT driver ISRs),
 * processing tasks to core 1.
 *
 * ESP32-C5 (single-core RISC-V): all tasks use tskNO_AFFINITY; the
 * preemptive scheduler handles prioritisation on the single core.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Task priorities (higher number = higher priority) ───────────────────── */

#define WIFI_SCAN_TASK_PRIORITY     5
#define BLE_SCAN_TASK_PRIORITY      4
#define FUSION_TASK_PRIORITY        3
#define UART_TX_TASK_PRIORITY       2
#define LED_TASK_PRIORITY           1

/* ── Stack sizes (bytes) ─────────────────────────────────────────────────── */

#define WIFI_SCAN_TASK_STACK_SIZE   8192
#define BLE_SCAN_TASK_STACK_SIZE    4096
#define FUSION_TASK_STACK_SIZE      4096
#define UART_TX_TASK_STACK_SIZE     4096
#define LED_TASK_STACK_SIZE         2048

/* ── Core affinity ───────────────────────────────────────────────────────── */

#ifdef CONFIG_FREERTOS_UNICORE
/* Single-core (ESP32-C5): no pinning, scheduler handles it */
#define WIFI_SCAN_TASK_CORE         tskNO_AFFINITY
#define BLE_SCAN_TASK_CORE          tskNO_AFFINITY
#define FUSION_TASK_CORE            tskNO_AFFINITY
#define UART_TX_TASK_CORE           tskNO_AFFINITY
#else
/* Dual-core (ESP32-S3): PRO_CPU = 0, APP_CPU = 1                           */
/* Radio tasks share core 0 with WiFi/BT driver ISRs for lowest latency.    */
/* Processing tasks run on core 1 to avoid contending with radio work.       */
#define CORE_RADIO                  0
#define CORE_PROCESSING             1

#define WIFI_SCAN_TASK_CORE         CORE_RADIO
#define BLE_SCAN_TASK_CORE          CORE_RADIO
#define FUSION_TASK_CORE            CORE_PROCESSING
#define UART_TX_TASK_CORE           CORE_PROCESSING
#endif

#ifdef __cplusplus
}
#endif
