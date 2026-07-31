#pragma once

#include "badge_usb_stream.h"
#include "badge_usb_transport_policy.h"
#include "uplink_ota_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool task_started;
    bool host_connected;
    badge_usb_binary_target_t parser_target;
    uint64_t rx_bytes;
    uint32_t valid_commands;
    uint32_t responses_completed;
    uint32_t required_response_failures;
    uint32_t hard_unanswered_required_responses;
    uint32_t enqueued_required_responses;
    uint32_t malformed_lines;
    uint32_t dropped_progress_frames;
    uint32_t dropped_optional_frames;
    uint32_t upload_received;
    uint32_t upload_size;
    int64_t task_heartbeat_ms;
    int64_t last_rx_ms;
    int64_t last_command_ms;
    int64_t last_response_ms;
    int64_t oldest_hard_unanswered_response_ms;
    int64_t oldest_enqueued_response_ms;
    int64_t last_upload_progress_ms;
} badge_usb_health_t;

bool badge_usb_transport_start(uint32_t boot_window_ms);
bool badge_usb_transport_wait_boot_window(TickType_t timeout);
void badge_usb_transport_set_recovery_only(bool enabled);
void badge_usb_transport_set_dispatch_ready(void);
bool badge_usb_transport_begin_binary(badge_usb_binary_target_t target,
                                      uint32_t exact_size);
bool badge_usb_transport_begin_scanner_binary(uint32_t exact_size,
                                              bool credit_v1);
bool badge_usb_transport_handle_uplink_ota_begin(
    const uplink_ota_manifest_t *manifest);
bool badge_usb_transport_reject_uplink_ota_begin(const char *error);
bool badge_usb_transport_emit(const void *data, size_t len,
                              badge_usb_frame_priority_t priority,
                              TickType_t timeout);
bool badge_usb_transport_drain(TickType_t timeout);
void badge_usb_transport_snapshot(badge_usb_health_t *out);
bool badge_usb_transport_host_active(uint32_t sample_window_ms);

#ifdef __cplusplus
}
#endif
