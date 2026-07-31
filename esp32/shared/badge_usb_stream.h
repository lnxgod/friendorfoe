#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BADGE_USB_BINARY_NONE = 0,
    BADGE_USB_BINARY_SCANNER,
    BADGE_USB_BINARY_UPLINK,
} badge_usb_binary_target_t;

typedef enum {
    BADGE_USB_EVENT_NONE = 0,
    BADGE_USB_EVENT_LINE,
    BADGE_USB_EVENT_BINARY_CHUNK,
    BADGE_USB_EVENT_BINARY_COMPLETE,
    BADGE_USB_EVENT_ERROR,
} badge_usb_stream_event_t;

typedef struct {
    badge_usb_binary_target_t target;
    char *line;
    size_t line_capacity;
    uint32_t exact_size;
    uint32_t received;
    uint32_t last_activity_ms;
    size_t line_length;
    /* A line CR is valid only when the next received byte is LF. */
    bool pending_cr;
    bool discarding_oversize_line;
} badge_usb_stream_t;

typedef struct {
    badge_usb_stream_event_t event;
    badge_usb_binary_target_t target;
    const uint8_t *bytes;
    size_t bytes_len;
    const char *line;
    /* Authoritative completed bytes, excluding LF or CRLF terminators. */
    size_t line_byte_len;
    size_t input_consumed;
    const char *error;
} badge_usb_stream_result_t;

void badge_usb_stream_init(badge_usb_stream_t *state,
                           char *line, size_t line_capacity);
bool badge_usb_stream_begin_binary(badge_usb_stream_t *state,
                                   badge_usb_binary_target_t target,
                                   uint32_t exact_size, uint32_t now_ms);
/*
 * Uplink OTA uses a two-phase read contract: inspect bytes without advancing
 * stream accounting, then commit only after durable storage accepts them.
 * The legacy badge_usb_stream_feed() behavior remains unchanged for scanner
 * uploads.
 */
badge_usb_stream_event_t badge_usb_stream_peek_binary(
    const badge_usb_stream_t *state, const uint8_t *src, size_t src_len,
    size_t max_bytes, badge_usb_stream_result_t *result);
bool badge_usb_stream_commit_binary(
    badge_usb_stream_t *state, const badge_usb_stream_result_t *result,
    uint32_t now_ms);
void badge_usb_stream_clear_binary(badge_usb_stream_t *state);
bool badge_usb_stream_binary_timed_out(
    const badge_usb_stream_t *state, uint32_t now_ms,
    uint32_t idle_timeout_ms);
badge_usb_stream_event_t badge_usb_stream_feed(
    badge_usb_stream_t *state, const uint8_t *src, size_t src_len,
    uint32_t now_ms, badge_usb_stream_result_t *result);
badge_usb_stream_event_t badge_usb_stream_poll_timeout(
    badge_usb_stream_t *state, uint32_t now_ms, uint32_t idle_timeout_ms);
badge_usb_stream_event_t badge_usb_stream_abort(
    badge_usb_stream_t *state, const char *reason,
    badge_usb_stream_result_t *result);

#ifdef __cplusplus
}
#endif
