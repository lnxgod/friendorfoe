#ifndef BACKEND_USB_TRANSPORT_CORE_H
#define BACKEND_USB_TRANSPORT_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

#include "backend_usb_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char session_id[33];
    uint64_t last_sent_sequence;
    uint64_t last_ack_sequence;
    uint64_t pending_heartbeat_sequence;
    int64_t next_heartbeat_ms;
    int64_t last_sent_ms;
    int64_t lease_expires_ms;
    bool started;
    bool confirmed;
    bool heartbeat_pending;
} backend_usb_live_state_t;

bool backend_usb_live_start(
    backend_usb_live_state_t *state,
    const char *session_id,
    int64_t now_ms);
bool backend_usb_live_prepare_heartbeat(
    backend_usb_live_state_t *state,
    int64_t now_ms,
    uint64_t *out_heartbeat_sequence);
bool backend_usb_live_note_heartbeat_sent(
    backend_usb_live_state_t *state,
    uint64_t sequence,
    int64_t now_ms);
void backend_usb_live_note_heartbeat_failed(
    backend_usb_live_state_t *state,
    uint64_t sequence);
bool backend_usb_live_acknowledge(
    backend_usb_live_state_t *state,
    const char *session_id,
    uint64_t sequence,
    int64_t now_ms);
void backend_usb_live_stop(backend_usb_live_state_t *state);
bool backend_usb_live_confirmed(
    backend_usb_live_state_t *state,
    int64_t now_ms);

typedef enum {
    BACKEND_USB_FRAME_REQUIRED = 0,
    BACKEND_USB_FRAME_OPTIONAL,
} backend_usb_frame_priority_t;

typedef enum {
    BACKEND_USB_FRAME_GENERIC = 0,
    BACKEND_USB_FRAME_LIVE_HEARTBEAT,
    BACKEND_USB_FRAME_LIVE_READY,
} backend_usb_frame_kind_t;

typedef struct {
    backend_usb_frame_priority_t priority;
    backend_usb_frame_kind_t kind;
    uint64_t correlation_sequence;
    uint64_t live_generation;
    size_t length;
    char bytes[BACKEND_USB_STATUS_MAX];
} backend_usb_frame_t;

typedef struct {
    backend_usb_frame_kind_t kind;
    uint64_t correlation_sequence;
    uint64_t live_generation;
    size_t length;
    char bytes[BACKEND_USB_STATUS_MAX];
} backend_usb_required_frame_t;

typedef struct {
    backend_usb_frame_kind_t kind;
    uint64_t correlation_sequence;
    uint64_t live_generation;
    size_t length;
    char bytes[BACKEND_USB_DET_MAX];
} backend_usb_optional_frame_t;

typedef struct {
    backend_usb_required_frame_t *required;
    backend_usb_optional_frame_t *optional;
    size_t required_capacity;
    size_t optional_capacity;
    size_t required_head;
    size_t required_count;
    size_t optional_head;
    size_t optional_count;
    uint64_t optional_drops;
    uint64_t required_failures;
    atomic_uint_fast64_t optional_contention_drops;
    atomic_uint_fast64_t required_contention_failures;
    bool output_poisoned;
    backend_usb_live_state_t live;
} backend_usb_transport_core_t;

bool backend_usb_transport_init(
    backend_usb_transport_core_t *transport,
    backend_usb_required_frame_t *required_storage,
    size_t required_capacity,
    backend_usb_optional_frame_t *optional_storage,
    size_t optional_capacity);

bool backend_usb_transport_enqueue(
    backend_usb_transport_core_t *transport,
    backend_usb_frame_priority_t priority,
    backend_usb_frame_kind_t kind,
    uint64_t correlation_sequence,
    const char *frame,
    size_t length);
bool backend_usb_transport_enqueue_live(
    backend_usb_transport_core_t *transport,
    backend_usb_frame_priority_t priority,
    backend_usb_frame_kind_t kind,
    uint64_t correlation_sequence,
    uint64_t live_generation,
    const char *frame,
    size_t length);
bool backend_usb_transport_pop(
    backend_usb_transport_core_t *transport,
    backend_usb_frame_t *out);
bool backend_usb_transport_pop_current(
    backend_usb_transport_core_t *transport,
    uint64_t live_generation,
    backend_usb_frame_t *out);
void backend_usb_transport_note_tx_failed(
    backend_usb_transport_core_t *transport,
    const backend_usb_frame_t *frame,
    uint64_t live_generation,
    bool wrote_any_bytes);
void backend_usb_transport_note_output_recovered(
    backend_usb_transport_core_t *transport);
void backend_usb_transport_note_lock_failure(
    backend_usb_transport_core_t *transport,
    backend_usb_frame_priority_t priority);
uint64_t backend_usb_transport_required_contention_failures(
    const backend_usb_transport_core_t *transport);
uint64_t backend_usb_transport_optional_contention_drops(
    const backend_usb_transport_core_t *transport);

#ifdef __cplusplus
}
#endif

#endif
