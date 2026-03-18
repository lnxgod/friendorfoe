#pragma once

/**
 * Friend or Foe — Scanner Task Priorities & Stack Sizes
 *
 * FreeRTOS task configuration for the Scanner (ESP32-S3).
 * The ESP32-S3 is dual-core: radio-intensive tasks are pinned to core 0
 * (which shares with the WiFi/BT driver ISRs), processing tasks to core 1.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Task priorities (higher number = higher priority) ───────────────────── */

#define WIFI_SCAN_TASK_PRIORITY     5
#define BLE_SCAN_TASK_PRIORITY      4
#define FUSION_TASK_PRIORITY        3
#define UART_TX_TASK_PRIORITY       2

/* ── Stack sizes (bytes) ─────────────────────────────────────────────────── */

#define WIFI_SCAN_TASK_STACK_SIZE   8192
#define BLE_SCAN_TASK_STACK_SIZE    4096
#define FUSION_TASK_STACK_SIZE      4096
#define UART_TX_TASK_STACK_SIZE     4096

/* ── Core affinity ───────────────────────────────────────────────────────── */
/* ESP32-S3 dual-core: PRO_CPU = 0, APP_CPU = 1                             */
/* Radio tasks share core 0 with WiFi/BT driver ISRs for lowest latency.    */
/* Processing tasks run on core 1 to avoid contending with radio work.      */

#define CORE_RADIO                  0   /* WiFi scan, BLE scan */
#define CORE_PROCESSING             1   /* Fusion, UART TX */

#define WIFI_SCAN_TASK_CORE         CORE_RADIO
#define BLE_SCAN_TASK_CORE          CORE_RADIO
#define FUSION_TASK_CORE            CORE_PROCESSING
#define UART_TX_TASK_CORE           CORE_PROCESSING

#ifdef __cplusplus
}
#endif
