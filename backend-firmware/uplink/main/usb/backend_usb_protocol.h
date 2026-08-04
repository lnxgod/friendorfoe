#ifndef BACKEND_USB_PROTOCOL_H
#define BACKEND_USB_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_identity.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_USB_COMMAND_MAX 2047U
#define BACKEND_USB_STATUS_MAX 16384U
#define BACKEND_USB_DET_MAX 1535U
#define BACKEND_USB_HEARTBEAT_MS INT64_C(5000)
#define BACKEND_USB_LIVE_LEASE_MS INT64_C(15000)
#define BACKEND_USB_REQUIRED_QUEUE_CAPACITY 4U
#define BACKEND_USB_OPTIONAL_QUEUE_CAPACITY 32U

typedef enum {
    BACKEND_USB_COMMAND_PING = 0,
    BACKEND_USB_COMMAND_STATUS,
    BACKEND_USB_COMMAND_LIVE_START,
    BACKEND_USB_COMMAND_LIVE_ACK,
    BACKEND_USB_COMMAND_LIVE_STOP,
    BACKEND_USB_COMMAND_CONFIG_GET,
    BACKEND_USB_COMMAND_CONFIG_SET,
    BACKEND_USB_COMMAND_SET,
    BACKEND_USB_COMMAND_SAVE,
    BACKEND_USB_COMMAND_BACKEND_STATUS,
    BACKEND_USB_COMMAND_AP_START,
    BACKEND_USB_COMMAND_UNKNOWN,
    BACKEND_USB_COMMAND_INVALID,
} backend_usb_command_kind_t;

typedef struct {
    backend_usb_command_kind_t kind;
    char key[32];
    char value[192];
    char session_id[33];
    uint64_t sequence;
    const char *json;
    size_t json_length;
} backend_usb_command_t;

bool backend_usb_protocol_parse_line(
    const char *line,
    size_t length,
    backend_usb_command_t *out);
size_t backend_usb_protocol_encode_ready(char *output, size_t capacity);
size_t backend_usb_protocol_encode_pong(
    const backend_firmware_identity_t *identity,
    char *output,
    size_t capacity);
size_t backend_usb_protocol_encode_live_ready(
    const char *session_id,
    char *output,
    size_t capacity);
size_t backend_usb_protocol_encode_live_heartbeat(
    const char *session_id,
    uint64_t sequence,
    char *output,
    size_t capacity);
size_t backend_usb_protocol_encode_investigation(
    const char *investigation_json,
    size_t json_length,
    char *output,
    size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
