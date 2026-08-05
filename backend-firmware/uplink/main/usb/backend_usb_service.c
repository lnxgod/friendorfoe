#include "backend_usb_service.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "backend_usb_protocol.h"
#include "psram_alloc.h"

#define BACKEND_USB_DRIVER_WRITE_MAX 4096U
#define BACKEND_USB_IO_WAIT_MS 100U
#define BACKEND_USB_REQUIRED_LOCK_MS 20U
#define BACKEND_USB_TX_TASK_STACK 6144U
#define BACKEND_USB_RX_TASK_STACK 6144U

static backend_usb_service_t *s_log_service;

static int64_t monotonic_ms(void)
{
    return esp_timer_get_time() / INT64_C(1000);
}

static void increment_saturating(uint64_t *value)
{
    if (value != NULL && *value != UINT64_MAX) {
        ++*value;
    }
}

static void add_saturating(uint64_t *value, size_t amount)
{
    if (value == NULL) {
        return;
    }
    if ((uint64_t)amount > UINT64_MAX - *value) {
        *value = UINT64_MAX;
    } else {
        *value += (uint64_t)amount;
    }
}

static bool complete_single_line(const char *frame, size_t length)
{
    if (frame == NULL || length == 0U || frame[length - 1U] != '\n') {
        return false;
    }
    for (size_t index = 0U; index + 1U < length; ++index) {
        if (frame[index] == '\n' || frame[index] == '\r' ||
            frame[index] == '\0') {
            return false;
        }
    }
    return true;
}

static void note_tx_failure_locked(
    backend_usb_service_t *service,
    const backend_usb_frame_t *frame,
    bool wrote_any_bytes)
{
    increment_saturating(&service->tx_failures);
    backend_usb_transport_note_tx_failed(
        &service->transport,
        frame,
        service->live_generation,
        wrote_any_bytes);
}

typedef struct {
    size_t bytes_written;
    bool wrote_any_bytes;
    bool complete;
} backend_usb_physical_tx_result_t;

static bool write_recovery_newline_physical(void)
{
    static const char newline = '\n';
    const int written = usb_serial_jtag_write_bytes(
        &newline, 1U, pdMS_TO_TICKS(BACKEND_USB_IO_WAIT_MS));
    return written == 1;
}

static backend_usb_physical_tx_result_t write_frame_physical(
    const backend_usb_frame_t *frame)
{
    backend_usb_physical_tx_result_t result = {0};
    if (frame == NULL) {
        return result;
    }
    size_t offset = 0U;
    while (offset < frame->length) {
        const size_t remaining = frame->length - offset;
        const size_t chunk = remaining > BACKEND_USB_DRIVER_WRITE_MAX
            ? BACKEND_USB_DRIVER_WRITE_MAX : remaining;
        const int written = usb_serial_jtag_write_bytes(
            frame->bytes + offset,
            chunk,
            pdMS_TO_TICKS(BACKEND_USB_IO_WAIT_MS));
        if (written <= 0 || (size_t)written > chunk) {
            result.wrote_any_bytes = offset != 0U || written > 0;
            return result;
        }
        offset += (size_t)written;
        result.bytes_written += (size_t)written;
        result.wrote_any_bytes = true;
    }
    result.complete = true;
    return result;
}

static void finish_tx_locked(
    backend_usb_service_t *service,
    const backend_usb_frame_t *frame,
    backend_usb_physical_tx_result_t result)
{
    add_saturating(&service->bytes_transmitted, result.bytes_written);
    if (!result.complete) {
        note_tx_failure_locked(service, frame, result.wrote_any_bytes);
        return;
    }
    if (frame->kind == BACKEND_USB_FRAME_LIVE_HEARTBEAT &&
        frame->live_generation == service->live_generation) {
        (void)backend_usb_live_note_heartbeat_sent(
            &service->transport.live,
            frame->correlation_sequence,
            monotonic_ms());
    }
}

static void remove_queued_live_controls_locked(
    backend_usb_service_t *service)
{
    backend_usb_transport_core_t *transport = &service->transport;
    size_t position = 0U;
    while (position < transport->required_count) {
        const size_t index =
            (transport->required_head + position) %
            transport->required_capacity;
        if (transport->required[index].kind !=
                BACKEND_USB_FRAME_LIVE_HEARTBEAT &&
            transport->required[index].kind !=
                BACKEND_USB_FRAME_LIVE_READY) {
            ++position;
            continue;
        }
        for (size_t move = position;
             move + 1U < transport->required_count; ++move) {
            const size_t destination =
                (transport->required_head + move) %
                transport->required_capacity;
            const size_t source =
                (transport->required_head + move + 1U) %
                transport->required_capacity;
            transport->required[destination] =
                transport->required[source];
        }
        --transport->required_count;
    }
}

static void queue_due_heartbeat(backend_usb_service_t *service)
{
    char frame[192];
    uint64_t sequence = 0U;
    uint64_t live_generation = 0U;
    if (xSemaphoreTake(service->lock, 0) != pdTRUE) {
        return;
    }
    const bool due = !service->live_ready_pending &&
        backend_usb_live_prepare_heartbeat(
            &service->transport.live, monotonic_ms(), &sequence);
    char session_id[33];
    (void)snprintf(
        session_id, sizeof(session_id), "%s",
        service->transport.live.session_id);
    live_generation = service->live_generation;
    (void)xSemaphoreGive(service->lock);
    if (!due) {
        return;
    }

    const size_t length = backend_usb_protocol_encode_live_heartbeat(
        session_id, sequence, frame, sizeof(frame));
    if (length == 0U || !backend_usb_service_emit_heartbeat(
            service, sequence, live_generation, frame, length)) {
        if (xSemaphoreTake(service->lock, portMAX_DELAY) == pdTRUE) {
            if (live_generation == service->live_generation) {
                backend_usb_live_note_heartbeat_failed(
                    &service->transport.live, sequence);
            }
            (void)xSemaphoreGive(service->lock);
        }
    }
}

static void usb_tx_task(void *argument)
{
    backend_usb_service_t *service = argument;
    for (;;) {
        queue_due_heartbeat(service);
        if (xSemaphoreTake(service->tx_gate, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (xSemaphoreTake(service->lock, portMAX_DELAY) != pdTRUE) {
            (void)xSemaphoreGive(service->tx_gate);
            continue;
        }
        if (service->transport.output_poisoned) {
            (void)xSemaphoreGive(service->lock);
            const bool recovered = write_recovery_newline_physical();
            (void)xSemaphoreTake(service->lock, portMAX_DELAY);
            if (recovered) {
                add_saturating(&service->bytes_transmitted, 1U);
                backend_usb_transport_note_output_recovered(
                    &service->transport);
            }
            (void)xSemaphoreGive(service->lock);
            (void)xSemaphoreGive(service->tx_gate);
            if (!recovered) {
                vTaskDelay(pdMS_TO_TICKS(20U));
            }
            continue;
        }
        const bool have_frame = backend_usb_transport_pop_current(
            &service->transport,
            service->live_generation,
            &service->tx_frame);
        (void)xSemaphoreGive(service->lock);
        if (!have_frame) {
            (void)xSemaphoreGive(service->tx_gate);
            vTaskDelay(pdMS_TO_TICKS(10U));
            continue;
        }
        const backend_usb_physical_tx_result_t result =
            write_frame_physical(&service->tx_frame);
        (void)xSemaphoreTake(service->lock, portMAX_DELAY);
        finish_tx_locked(service, &service->tx_frame, result);
        (void)xSemaphoreGive(service->lock);
        (void)xSemaphoreGive(service->tx_gate);
    }
}

static void usb_rx_task(void *argument)
{
    backend_usb_service_t *service = argument;
    uint8_t input[512];
    for (;;) {
        const int received = usb_serial_jtag_read_bytes(
            input, sizeof(input), pdMS_TO_TICKS(BACKEND_USB_IO_WAIT_MS));
        if (received <= 0) {
            continue;
        }
        if (xSemaphoreTake(service->lock, 0) == pdTRUE) {
            add_saturating(&service->bytes_received, (size_t)received);
            (void)xSemaphoreGive(service->lock);
        }
        size_t offset = 0U;
        while (offset < (size_t)received) {
            size_t consumed = 0U;
            const scanner_uart_line_event_t event =
                scanner_uart_line_framer_consume(
                    &service->rx_framer,
                    input + offset,
                    (size_t)received - offset,
                    &consumed);
            if (consumed == 0U) {
                break;
            }
            offset += consumed;
            if (event.kind == SCANNER_UART_LINE_EVENT_FRAME_READY &&
                service->config.on_line != NULL) {
                service->config.on_line(
                    service->config.context,
                    (const char *)event.bytes,
                    event.byte_len,
                    monotonic_ms());
            }
        }
    }
}

static int usb_log_vprintf(const char *format, va_list arguments)
{
    backend_usb_service_t *service = s_log_service;
    if (service == NULL || format == NULL) {
        return 0;
    }
    char frame[384];
    const int formatted = vsnprintf(frame, sizeof(frame), format, arguments);
    if (formatted <= 0) {
        return formatted;
    }
    size_t length = (size_t)formatted;
    if (length >= sizeof(frame)) {
        length = sizeof(frame) - 1U;
    }
    while (length > 0U &&
           (frame[length - 1U] == '\n' || frame[length - 1U] == '\r')) {
        --length;
    }
    if (length == 0U || length + 1U >= sizeof(frame)) {
        return formatted;
    }
    for (size_t index = 0U; index < length; ++index) {
        if (frame[index] == '\n' || frame[index] == '\r' ||
            frame[index] == '\0') {
            frame[index] = ' ';
        }
    }
    frame[length++] = '\n';
    (void)backend_usb_service_emit(
        service, BACKEND_USB_FRAME_OPTIONAL, frame, length);
    return formatted;
}

bool backend_usb_service_start(
    backend_usb_service_t *service,
    const backend_usb_service_config_t *config)
{
    if (service == NULL || config == NULL || config->on_line == NULL) {
        return false;
    }
    memset(service, 0, sizeof(*service));
    service->config = *config;
    service->required_storage = psram_alloc_strict(
        BACKEND_USB_REQUIRED_QUEUE_CAPACITY *
        sizeof(backend_usb_required_frame_t));
    service->optional_storage = psram_alloc_strict(
        BACKEND_USB_OPTIONAL_QUEUE_CAPACITY *
        sizeof(backend_usb_optional_frame_t));
    if (service->required_storage == NULL ||
        service->optional_storage == NULL) {
        psram_free(service->required_storage);
        psram_free(service->optional_storage);
        service->required_storage = NULL;
        service->optional_storage = NULL;
        return false;
    }
    memset(
        service->required_storage, 0,
        BACKEND_USB_REQUIRED_QUEUE_CAPACITY *
        sizeof(backend_usb_required_frame_t));
    memset(
        service->optional_storage, 0,
        BACKEND_USB_OPTIONAL_QUEUE_CAPACITY *
        sizeof(backend_usb_optional_frame_t));

    service->lock = xSemaphoreCreateMutex();
    service->tx_gate = xSemaphoreCreateMutex();
    if (service->lock == NULL || service->tx_gate == NULL ||
        !scanner_uart_line_framer_init(
            &service->rx_framer,
            service->rx_line_storage,
            sizeof(service->rx_line_storage)) ||
        !backend_usb_transport_init(
            &service->transport,
            service->required_storage,
            BACKEND_USB_REQUIRED_QUEUE_CAPACITY,
            service->optional_storage,
            BACKEND_USB_OPTIONAL_QUEUE_CAPACITY)) {
        if (service->lock != NULL) {
            vSemaphoreDelete(service->lock);
        }
        if (service->tx_gate != NULL) {
            vSemaphoreDelete(service->tx_gate);
        }
        psram_free(service->required_storage);
        psram_free(service->optional_storage);
        memset(service, 0, sizeof(*service));
        return false;
    }

    usb_serial_jtag_driver_config_t driver = {
        .rx_buffer_size = 8192,
        .tx_buffer_size = 8192,
    };
    if (usb_serial_jtag_driver_install(&driver) != ESP_OK) {
        vSemaphoreDelete(service->lock);
        vSemaphoreDelete(service->tx_gate);
        psram_free(service->required_storage);
        psram_free(service->optional_storage);
        memset(service, 0, sizeof(*service));
        return false;
    }
    usb_serial_jtag_vfs_use_driver();
    service->started = true;
    BaseType_t created = xTaskCreate(
        usb_tx_task, "backend_usb_tx", BACKEND_USB_TX_TASK_STACK,
        service, 6U, &service->tx_task);
    if (created == pdPASS) {
        created = xTaskCreate(
            usb_rx_task, "backend_usb_rx", BACKEND_USB_RX_TASK_STACK,
            service, 6U, &service->rx_task);
    }
    if (created != pdPASS) {
        if (service->tx_task != NULL) {
            vTaskDelete(service->tx_task);
        }
        (void)usb_serial_jtag_driver_uninstall();
        vSemaphoreDelete(service->lock);
        vSemaphoreDelete(service->tx_gate);
        psram_free(service->required_storage);
        psram_free(service->optional_storage);
        memset(service, 0, sizeof(*service));
        return false;
    }
    s_log_service = service;
    (void)esp_log_set_vprintf(usb_log_vprintf);
    return true;
}

bool backend_usb_service_emit(
    backend_usb_service_t *service,
    backend_usb_frame_priority_t priority,
    const char *frame,
    size_t length)
{
    if (service == NULL || !service->started ||
        !complete_single_line(frame, length)) {
        return false;
    }
    const TickType_t wait = priority == BACKEND_USB_FRAME_REQUIRED
        ? pdMS_TO_TICKS(BACKEND_USB_REQUIRED_LOCK_MS) : 0;
    if (xSemaphoreTake(service->lock, wait) != pdTRUE) {
        backend_usb_transport_note_lock_failure(
            &service->transport, priority);
        return false;
    }
    const bool enqueued = backend_usb_transport_enqueue(
        &service->transport,
        priority,
        BACKEND_USB_FRAME_GENERIC,
        0U,
        frame,
        length);
    (void)xSemaphoreGive(service->lock);
    return enqueued;
}

bool backend_usb_service_emit_heartbeat(
    backend_usb_service_t *service,
    uint64_t sequence,
    uint64_t live_generation,
    const char *frame,
    size_t length)
{
    if (service == NULL || !service->started || sequence == 0U ||
        live_generation == 0U || !complete_single_line(frame, length)) {
        return false;
    }
    if (xSemaphoreTake(
            service->lock,
            pdMS_TO_TICKS(BACKEND_USB_REQUIRED_LOCK_MS)) != pdTRUE) {
        backend_usb_transport_note_lock_failure(
            &service->transport, BACKEND_USB_FRAME_REQUIRED);
        return false;
    }
    const bool enqueued = live_generation == service->live_generation &&
        service->transport.live.started &&
        backend_usb_transport_enqueue_live(
            &service->transport,
            BACKEND_USB_FRAME_REQUIRED,
            BACKEND_USB_FRAME_LIVE_HEARTBEAT,
            sequence,
            live_generation,
            frame,
            length);
    (void)xSemaphoreGive(service->lock);
    return enqueued;
}

bool backend_usb_service_emit_live_ready(
    backend_usb_service_t *service,
    const char *session_id,
    const char *frame,
    size_t length)
{
    if (service == NULL || !service->started || session_id == NULL ||
        !complete_single_line(frame, length)) {
        return false;
    }
    if (xSemaphoreTake(
            service->lock,
            pdMS_TO_TICKS(BACKEND_USB_REQUIRED_LOCK_MS)) != pdTRUE) {
        backend_usb_transport_note_lock_failure(
            &service->transport, BACKEND_USB_FRAME_REQUIRED);
        return false;
    }
    const bool current = service->transport.live.started &&
        service->live_ready_pending &&
        strcmp(service->transport.live.session_id, session_id) == 0;
    const bool enqueued = current && backend_usb_transport_enqueue_live(
        &service->transport,
        BACKEND_USB_FRAME_REQUIRED,
        BACKEND_USB_FRAME_LIVE_READY,
        0U,
        service->live_generation,
        frame,
        length);
    if (enqueued) {
        service->live_ready_pending = false;
    }
    (void)xSemaphoreGive(service->lock);
    return enqueued;
}

static void next_session_id(
    backend_usb_service_t *service, char output[33])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t random_bytes[16];
    do {
        esp_fill_random(random_bytes, sizeof(random_bytes));
        for (size_t index = 0U; index < sizeof(random_bytes); ++index) {
            output[index * 2U] = hex[random_bytes[index] >> 4U];
            output[index * 2U + 1U] = hex[random_bytes[index] & 0x0FU];
        }
        output[32] = '\0';
    } while (strcmp(output, service->last_session_id) == 0);
}

bool backend_usb_service_live_start(
    backend_usb_service_t *service,
    int64_t now_ms,
    char out_session_id[33])
{
    if (out_session_id != NULL) {
        out_session_id[0] = '\0';
    }
    if (service == NULL || !service->started || out_session_id == NULL) {
        return false;
    }
    if (xSemaphoreTake(service->tx_gate, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    if (xSemaphoreTake(service->lock, portMAX_DELAY) != pdTRUE) {
        (void)xSemaphoreGive(service->tx_gate);
        return false;
    }
    char session_id[33];
    next_session_id(service, session_id);
    remove_queued_live_controls_locked(service);
    if (service->live_generation != UINT64_MAX) {
        ++service->live_generation;
    } else {
        service->live_generation = 1U;
    }
    const bool started = backend_usb_live_start(
        &service->transport.live, session_id, now_ms);
    if (started) {
        service->live_ready_pending = true;
        memcpy(service->last_session_id, session_id, sizeof(session_id));
        memcpy(out_session_id, session_id, sizeof(session_id));
    }
    (void)xSemaphoreGive(service->lock);
    (void)xSemaphoreGive(service->tx_gate);
    return started;
}

bool backend_usb_service_live_acknowledge(
    backend_usb_service_t *service,
    const char *session_id,
    uint64_t sequence,
    int64_t now_ms)
{
    if (service == NULL || !service->started) {
        return false;
    }
    if (xSemaphoreTake(service->tx_gate, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    if (xSemaphoreTake(service->lock, portMAX_DELAY) != pdTRUE) {
        (void)xSemaphoreGive(service->tx_gate);
        return false;
    }
    const int64_t fresh_now_ms = monotonic_ms();
    if (fresh_now_ms > now_ms) {
        now_ms = fresh_now_ms;
    }
    const bool acknowledged = backend_usb_live_acknowledge(
        &service->transport.live, session_id, sequence, now_ms);
    (void)xSemaphoreGive(service->lock);
    (void)xSemaphoreGive(service->tx_gate);
    return acknowledged;
}

bool backend_usb_service_live_stop(
    backend_usb_service_t *service,
    const char *session_id)
{
    if (service == NULL || !service->started || session_id == NULL) {
        return false;
    }
    if (xSemaphoreTake(service->tx_gate, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    if (xSemaphoreTake(service->lock, portMAX_DELAY) != pdTRUE) {
        (void)xSemaphoreGive(service->tx_gate);
        return false;
    }
    const bool matches = service->transport.live.started &&
        strcmp(service->transport.live.session_id, session_id) == 0;
    if (matches) {
        remove_queued_live_controls_locked(service);
        backend_usb_live_stop(&service->transport.live);
        service->live_ready_pending = false;
        if (service->live_generation != UINT64_MAX) {
            ++service->live_generation;
        } else {
            service->live_generation = 1U;
        }
    }
    (void)xSemaphoreGive(service->lock);
    (void)xSemaphoreGive(service->tx_gate);
    return matches;
}

bool backend_usb_service_live_confirmed(
    backend_usb_service_t *service,
    int64_t now_ms)
{
    if (service == NULL || !service->started ||
        xSemaphoreTake(service->lock, 0) != pdTRUE) {
        return false;
    }
    const bool confirmed = backend_usb_live_confirmed(
        &service->transport.live, now_ms);
    (void)xSemaphoreGive(service->lock);
    return confirmed;
}

bool backend_usb_service_snapshot(
    backend_usb_service_t *service,
    int64_t now_ms,
    backend_usb_service_snapshot_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (service == NULL || !service->started ||
        xSemaphoreTake(
            service->lock,
            pdMS_TO_TICKS(BACKEND_USB_REQUIRED_LOCK_MS)) != pdTRUE) {
        return false;
    }
    out->available = true;
    out->output_poisoned = service->transport.output_poisoned;
    out->required_queue_depth = service->transport.required_count;
    out->optional_queue_depth = service->transport.optional_count;
    out->optional_drops = service->transport.optional_drops;
    out->required_failures = service->transport.required_failures;
    const uint64_t contention_optional =
        backend_usb_transport_optional_contention_drops(
            &service->transport);
    const uint64_t contention_required =
        backend_usb_transport_required_contention_failures(
            &service->transport);
    if (UINT64_MAX - out->optional_drops < contention_optional) {
        out->optional_drops = UINT64_MAX;
    } else {
        out->optional_drops += contention_optional;
    }
    if (UINT64_MAX - out->required_failures < contention_required) {
        out->required_failures = UINT64_MAX;
    } else {
        out->required_failures += contention_required;
    }
    if (UINT64_MAX - out->required_failures < service->tx_failures) {
        out->required_failures = UINT64_MAX;
    } else {
        out->required_failures += service->tx_failures;
    }
    out->bytes_transmitted = service->bytes_transmitted;
    out->bytes_received = service->bytes_received;
    out->live_started = service->transport.live.started;
    out->last_ack_sequence = service->transport.live.last_ack_sequence;
    (void)snprintf(
        out->live_session_id, sizeof(out->live_session_id), "%s",
        service->transport.live.session_id);
    out->live_confirmed = backend_usb_live_confirmed(
        &service->transport.live, now_ms);
    if (out->live_confirmed &&
        service->transport.live.lease_expires_ms > now_ms) {
        out->live_lease_remaining_ms =
            service->transport.live.lease_expires_ms - now_ms;
    }
    (void)xSemaphoreGive(service->lock);
    out->host_connected = usb_serial_jtag_is_connected();
    return true;
}
