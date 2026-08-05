#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_ble_investigation.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_COMMAND_DEVICE_ID_CAPACITY 33U
#define BACKEND_COMMAND_PATH_CAPACITY 128U
#define BACKEND_COMMAND_ENVELOPE_MAX_JSON 1024U
#define BACKEND_COMMAND_ACK_MAX_JSON 1024U
#define BACKEND_COMMAND_POLL_INTERVAL_MS INT64_C(5000)

typedef enum {
    BACKEND_COMMAND_KIND_INVESTIGATE = 0,
    BACKEND_COMMAND_KIND_CANCEL,
} backend_command_kind_t;

typedef struct {
    backend_command_kind_t kind;
    char command_id[BLE_INV_REQUEST_ID_LEN];
    ble_investigation_request_t request;
    uint32_t next_sequence;
    bool has_result_state;
    ble_investigation_state_t result_state;
} backend_command_envelope_t;

typedef enum {
    BACKEND_COMMAND_DECODE_OK = 0,
    BACKEND_COMMAND_DECODE_MALFORMED,
    BACKEND_COMMAND_DECODE_SCHEMA_MISMATCH,
    BACKEND_COMMAND_DECODE_TOO_LARGE,
} backend_command_decode_result_t;

typedef enum {
    BACKEND_COMMAND_INTENT_INVALID = 0,
    BACKEND_COMMAND_INTENT_START,
    BACKEND_COMMAND_INTENT_ALREADY_ACTIVE,
    BACKEND_COMMAND_INTENT_CANCEL_FIRST_SEEN,
    BACKEND_COMMAND_INTENT_CANCEL_ACTIVE,
    BACKEND_COMMAND_INTENT_RESUME_FAILED,
    BACKEND_COMMAND_INTENT_RESUME_CANCELLED,
    BACKEND_COMMAND_INTENT_CONFLICT,
} backend_command_intent_t;

typedef struct {
    char command_id[BLE_INV_REQUEST_ID_LEN];
    uint32_t accepted_sequence;
    uint32_t next_sequence;
    ble_investigation_state_t result_state;
    bool terminal;
    bool duplicate;
} backend_command_result_ack_t;

typedef enum {
    BACKEND_COMMAND_HTTP_RETRY = 0,
    BACKEND_COMMAND_HTTP_IDLE,
    BACKEND_COMMAND_HTTP_BODY,
    BACKEND_COMMAND_HTTP_ACK,
    BACKEND_COMMAND_HTTP_QUARANTINE,
} backend_command_http_action_t;

typedef struct {
    bool initialized;
    int64_t next_poll_ms;
    uint32_t retryable_errors;
    uint32_t quarantined_errors;
    int last_status_code;
    backend_command_http_action_t last_action;
    bool result_quarantined;
} backend_command_http_state_t;

/*
 * Bind one active command to this application-static state. The pending body
 * remains byte-identical until a validated ACK is committed after the shared
 * investigation core advances its matching command ID and sequence.
 */
typedef struct {
    bool bound;
    char command_id[BLE_INV_REQUEST_ID_LEN];
    uint32_t next_sequence;
    bool has_result_state;
    ble_investigation_state_t result_state;
    char result_path[BACKEND_COMMAND_PATH_CAPACITY];

    bool pending;
    uint32_t pending_sequence;
    ble_investigation_state_t expected_result_state;
    bool expected_terminal;
    bool replay_eligible;
    uint32_t attempt_count;
    uint16_t post_body_length;
    char post_body[BACKEND_COMMAND_RESULT_MAX_JSON + 1U];
} backend_command_client_state_t;

backend_command_decode_result_t backend_command_envelope_decode(
    const char *json,
    size_t length,
    backend_command_envelope_t *out);

bool backend_command_http_state_init(
    backend_command_http_state_t *state,
    int64_t now_ms);
bool backend_command_poll_due(
    const backend_command_http_state_t *state,
    int64_t now_ms);
bool backend_command_poll_started(
    backend_command_http_state_t *state,
    int64_t now_ms);
backend_command_http_action_t backend_command_poll_http_action(
    bool transport_complete,
    int status_code);
backend_command_http_action_t backend_command_result_http_action(
    bool transport_complete,
    int status_code,
    bool ack_valid);
void backend_command_http_note(
    backend_command_http_state_t *state,
    backend_command_http_action_t action,
    int status_code,
    bool result_request);

/* A NULL or inactive local_state means no local command owns the radio.
 * Active duplicates must match the complete retained original request. */
backend_command_intent_t backend_command_select_intent(
    const backend_command_envelope_t *envelope,
    const backend_ble_investigation_state_t *local_state);

/* Encodes only START and CANCEL_ACTIVE intents; all others return zero. */
size_t backend_command_scanner_line_encode(
    const backend_command_envelope_t *envelope,
    backend_command_intent_t intent,
    char *output,
    size_t capacity);

bool backend_command_build_poll_path(
    const char *device_id,
    char *output,
    size_t capacity);

bool backend_command_build_result_path(
    const char *device_id,
    const char *command_id,
    char *output,
    size_t capacity);

void backend_command_client_init(backend_command_client_state_t *state);

bool backend_command_client_bind(
    backend_command_client_state_t *state,
    const char *device_id,
    const backend_command_envelope_t *envelope);

/* A repeated call is a retry only when sequence and body are byte-identical. */
bool backend_command_result_prepare(
    backend_command_client_state_t *state,
    const backend_command_result_t *pending_result);

bool backend_command_result_ack_validate(
    const backend_command_client_state_t *state,
    const char *json,
    size_t length,
    backend_command_result_ack_t *out);

/* Call only after backend_ble_investigation_mark_acked returns true. */
bool backend_command_result_ack_commit(
    backend_command_client_state_t *state,
    const backend_command_result_ack_t *ack);

#ifdef __cplusplus
}
#endif
