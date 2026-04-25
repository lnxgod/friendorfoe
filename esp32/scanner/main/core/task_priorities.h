#pragma once

/**
 * Friend or Foe — WiFi Scanner Task Priorities & Stack Sizes
 *
 * FreeRTOS task configuration for the ESP32-S3 combo/seed scanner.
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
#define DISPLAY_TASK_PRIORITY      1

/* ── Stack sizes (bytes) ─────────────────────────────────────────────────── */

#define WIFI_SCAN_TASK_STACK_SIZE   8192
#define BLE_SCAN_TASK_STACK_SIZE    6144
#define FUSION_TASK_STACK_SIZE      4096
#define UART_TX_TASK_STACK_SIZE     8192
#define LED_TASK_STACK_SIZE         4096
#define DISPLAY_TASK_STACK_SIZE    4096

/* ── Core affinity ───────────────────────────────────────────────────────── */

/* ESP32-S3 dual-core: PRO_CPU = 0, APP_CPU = 1                             */
/* Radio tasks share core 0 with WiFi driver ISRs for lowest latency.       */
/* Processing tasks run on core 1 to avoid contending with radio work.       */
#define CORE_RADIO                  0
#define CORE_PROCESSING             1

#define WIFI_SCAN_TASK_CORE         CORE_RADIO
#define BLE_SCAN_TASK_CORE          CORE_RADIO
#define FUSION_TASK_CORE            CORE_PROCESSING
#define UART_TX_TASK_CORE           CORE_PROCESSING
#define DISPLAY_TASK_CORE           CORE_PROCESSING

#ifdef __cplusplus
}
#endif
