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

#ifdef __cplusplus
}
#endif
