#ifndef BACKEND_USB_SERVICE_H
#define BACKEND_USB_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "backend_usb_transport_core.h"
#include "scanner_uart_line_framer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*backend_usb_service_line_fn)(
    void *context,
    const char *line,
    size_t length,
    int64_t now_ms);

typedef struct {
    void *context;
    backend_usb_service_line_fn on_line;
} backend_usb_service_config_t;

typedef struct {
    bool available;
    bool host_connected;
    bool output_poisoned;
    size_t required_queue_depth;
    size_t optional_queue_depth;
    uint64_t optional_drops;
    uint64_t required_failures;
    uint64_t bytes_transmitted;
    uint64_t bytes_received;
    char live_session_id[33];
    uint64_t last_ack_sequence;
    bool live_started;
    bool live_confirmed;
    int64_t live_lease_remaining_ms;
} backend_usb_service_snapshot_t;

typedef struct backend_usb_service {
    SemaphoreHandle_t lock;
    TaskHandle_t rx_task;
    TaskHandle_t tx_task;
    backend_usb_service_config_t config;
    backend_usb_transport_core_t transport;
    backend_usb_required_frame_t *required_storage;
    backend_usb_optional_frame_t *optional_storage;
    backend_usb_frame_t tx_frame;
    scanner_uart_line_framer_t rx_framer;
    uint8_t rx_line_storage[SCANNER_UART_LINE_BUFFER_SIZE];
    char last_session_id[33];
    uint64_t live_generation;
    uint64_t tx_live_generation;
    uint64_t bytes_transmitted;
    uint64_t bytes_received;
    uint64_t tx_failures;
    bool started;
    bool output_poisoned;
    bool live_ready_pending;
} backend_usb_service_t;

bool backend_usb_service_start(
    backend_usb_service_t *service,
    const backend_usb_service_config_t *config);
bool backend_usb_service_emit(
    backend_usb_service_t *service,
    backend_usb_frame_priority_t priority,
    const char *frame,
    size_t length);
bool backend_usb_service_emit_heartbeat(
    backend_usb_service_t *service,
    uint64_t sequence,
    const char *frame,
    size_t length);
bool backend_usb_service_live_confirmed(
    backend_usb_service_t *service,
    int64_t now_ms);
bool backend_usb_service_live_start(
    backend_usb_service_t *service,
    int64_t now_ms,
    char out_session_id[33]);
bool backend_usb_service_live_acknowledge(
    backend_usb_service_t *service,
    const char *session_id,
    uint64_t sequence,
    int64_t now_ms);
bool backend_usb_service_live_stop(
    backend_usb_service_t *service,
    const char *session_id);
bool backend_usb_service_snapshot(
    backend_usb_service_t *service,
    int64_t now_ms,
    backend_usb_service_snapshot_t *out);

#ifdef __cplusplus
}
#endif

#endif
