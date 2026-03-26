#pragma once

/**
 * Friend or Foe — BLE Scanner Console Output
 *
 * Printf-based JSON output for BLE detections. Reads from the shared
 * detection queue, applies Bayesian fusion, and prints newline-delimited
 * JSON to the serial console.
 */

#include <stdbool.h>
#include <stdint.h>
#include "detection_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the console output FreeRTOS task.
 *
 * Reads detections from the queue, applies Bayesian fusion,
 * serializes to JSON, and prints to stdout.
 *
 * @param detection_queue  FreeRTOS queue carrying drone_detection_t items
 */
void console_output_start(QueueHandle_t detection_queue);

/**
 * Summary of a detection, cached for OLED display.
 */
typedef struct {
    char    drone_id[64];
    char    manufacturer[32];
    float   confidence;
    int8_t  rssi;
    int64_t timestamp_ms;
    double  latitude;
    double  longitude;
    double  altitude_m;
    float   speed_mps;
} scanner_detection_summary_t;

/** Get cumulative BLE detection count. */
int console_output_get_ble_count(void);

/** Get total detection count. */
int console_output_get_total_count(void);

/** Maximum number of cached detections for display scoreboard. */
#define DETECTION_CACHE_SIZE 10

#if CONFIG_FOF_GLASSES_DETECTION
#include "glasses_detector.h"

/** Attach a glasses detection queue for output. */
void console_output_set_glasses_queue(QueueHandle_t glasses_queue);

/** Maximum cached glasses detections for display. */
#define GLASSES_CACHE_SIZE 5

/** Get cached glasses detections. */
int console_output_get_cached_glasses(glasses_detection_t *out, int max_count);

/** Get cumulative glasses detection count. */
int console_output_get_glasses_count(void);
#endif

/**
 * Copy all cached detections into @p out, sorted most-recent first.
 * @param out        Caller-allocated array of at least @p max_count entries
 * @param max_count  Maximum entries to copy
 * @return           Number of entries actually copied (0..max_count)
 */
int console_output_get_cached_detections(scanner_detection_summary_t *out, int max_count);

#ifdef __cplusplus
}
#endif
