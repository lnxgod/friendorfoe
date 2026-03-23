#pragma once

/**
 * Friend or Foe — RID Simulator Task Priorities & Stack Sizes
 *
 * FreeRTOS task configuration for the RID simulator board.
 *
 * ESP32-S3 (dual-core): sim on core 1, peripherals on core 1.
 * ESP32-C5/C3 (single-core): all tasks use tskNO_AFFINITY.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Task priorities (higher number = higher priority) ───────────────────── */

#define SIM_TASK_PRIORITY           5
#define DISPLAY_TASK_PRIORITY       2
#define LED_TASK_PRIORITY           1

/* ── Stack sizes (bytes) ─────────────────────────────────────────────────── */

#define SIM_TASK_STACK_SIZE         4096
#define DISPLAY_TASK_STACK_SIZE     4096
#define LED_TASK_STACK_SIZE         2048

/* ── Core affinity ───────────────────────────────────────────────────────── */

#ifdef CONFIG_FREERTOS_UNICORE
#define SIM_TASK_CORE               tskNO_AFFINITY
#define DISPLAY_TASK_CORE           tskNO_AFFINITY
#else
#define CORE_PROCESSING             1

#define SIM_TASK_CORE               CORE_PROCESSING
#define DISPLAY_TASK_CORE           CORE_PROCESSING
#endif

#ifdef __cplusplus
}
#endif
