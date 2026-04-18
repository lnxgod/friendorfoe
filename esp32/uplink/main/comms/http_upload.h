#pragma once

/**
 * Friend or Foe -- Uplink HTTP Upload Module
 *
 * Consumes drone detections from a FreeRTOS queue, batches them, and
 * POSTs them to the FastAPI backend as JSON.  Supports offline buffering
 * in a ring buffer and drains the backlog on reconnect.
 */

#include "detection_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the HTTP upload module.
 *
 * @param detection_queue  FreeRTOS queue carrying drone_detection_t items
 *                         from the UART RX task.
 */
void http_upload_init(QueueHandle_t detection_queue);

/**
 * Start the HTTP upload FreeRTOS task.
 * Batches detections and POSTs them periodically.
 */
void http_upload_start(void);

/**
 * Get the count of successfully uploaded batches since boot.
 */
int http_upload_get_success_count(void);

/**
 * Get the count of failed upload attempts since boot.
 */
int http_upload_get_fail_count(void);

/**
 * Get the timestamp of the last successful upload (milliseconds since boot).
 * Returns 0 if no successful upload has occurred yet.
 */
int64_t http_upload_get_last_success_ms(void);

/**
 * Pause the HTTP upload task.
 * Releases the HTTP client and stops consuming the detection queue.
 * Used during firmware relay to free heap and prevent WiFi contention.
 */
void http_upload_pause(void);

/**
 * Resume the HTTP upload task after a pause.
 * HTTP client will be recreated on next upload attempt.
 */
void http_upload_resume(void);

/** True if the upload task is currently paused. */
bool http_upload_is_paused(void);

/**
 * Current depth of the offline ring buffer (batches queued while WiFi was
 * down or uploads were failing). Capped at CONFIG_MAX_OFFLINE_BATCHES.
 * Exposed via /api/status for Phase-2 PSRAM queue observability.
 */
int http_upload_get_offline_count(void);

/** Capacity of the offline ring buffer (= CONFIG_MAX_OFFLINE_BATCHES). */
int http_upload_get_offline_capacity(void);

#ifdef __cplusplus
}
#endif
