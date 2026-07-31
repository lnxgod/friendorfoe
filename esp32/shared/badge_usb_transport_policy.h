#pragma once

#include "badge_usb_stream.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BADGE_USB_TX_CHUNK_BYTES 2048U
#define BADGE_USB_BINARY_IDLE_TIMEOUT_MS 5000U
#define BADGE_USB_SCANNER_CREDIT_BYTES 4096U
#define BADGE_USB_APP_REENUMERATE_DETACH_MS 500U

typedef struct {
    void *context;
    void (*enable_bus_clock)(void *context);
    void (*set_pad_enabled)(void *context, bool enabled);
    void (*delay_ms)(void *context, uint32_t delay_ms);
    void (*select_internal_phy)(void *context);
} badge_usb_app_reenumerate_hooks_t;

bool badge_usb_app_reenumerate(
    const badge_usb_app_reenumerate_hooks_t *hooks);

typedef enum {
    BADGE_USB_FRAME_REQUIRED = 0,
    BADGE_USB_FRAME_PROGRESS,
    BADGE_USB_FRAME_OPTIONAL,
} badge_usb_frame_priority_t;

typedef enum {
    BADGE_USB_EMIT_COMPLETED = 0,
    BADGE_USB_EMIT_ENQUEUED,
    BADGE_USB_EMIT_DROPPED,
    BADGE_USB_EMIT_FAILED,
    BADGE_USB_EMIT_POISONED,
} badge_usb_emit_result_t;

typedef enum {
    BADGE_USB_EMIT_HEALTH_TRACKED = 0,
    BADGE_USB_EMIT_HEALTH_NEUTRAL,
} badge_usb_emit_health_mode_t;

typedef enum {
    BADGE_USB_EMIT_HEALTH_EFFECT_NONE = 0,
    BADGE_USB_EMIT_HEALTH_EFFECT_COMPLETED,
    BADGE_USB_EMIT_HEALTH_EFFECT_REQUIRED_ENQUEUED,
    BADGE_USB_EMIT_HEALTH_EFFECT_REQUIRED_HARD_FAILURE,
    BADGE_USB_EMIT_HEALTH_EFFECT_PROGRESS_DROP,
    BADGE_USB_EMIT_HEALTH_EFFECT_OPTIONAL_DROP,
} badge_usb_emit_health_effect_t;

badge_usb_emit_health_effect_t badge_usb_emit_health_effect_decide(
    badge_usb_emit_result_t result,
    badge_usb_frame_priority_t priority,
    badge_usb_emit_health_mode_t health_mode);

typedef struct badge_usb_output_hooks {
    void *context;
    bool (*host_connected)(void *context);
    bool (*lock)(void *context, uint32_t timeout_ticks);
    void (*unlock)(void *context);
    uintptr_t (*current_owner)(void *context);
    uint32_t (*now_ticks)(void *context);
    int (*write)(void *context, const uint8_t *data, size_t len,
                 uint32_t timeout_ticks);
    bool (*drain)(void *context, uint32_t timeout_ticks);
} badge_usb_output_hooks_t;

typedef struct {
    atomic_bool poisoned;
    atomic_uintptr_t emission_owner;
} badge_usb_output_policy_t;

badge_usb_emit_result_t badge_usb_output_emit(
    badge_usb_output_policy_t *policy,
    const badge_usb_output_hooks_t *hooks,
    const void *data, size_t len,
    badge_usb_frame_priority_t priority,
    uint32_t timeout_ticks);

typedef enum {
    BADGE_USB_COMMAND_UNKNOWN = 0,
    BADGE_USB_COMMAND_BOOTING,
    BADGE_USB_COMMAND_RECOVERY_ONLY,
    BADGE_USB_COMMAND_DISPATCH,
} badge_usb_command_decision_t;

badge_usb_command_decision_t badge_usb_command_decide(bool recognized,
                                                       bool dispatch_ready,
                                                       bool recovery_only,
                                                       bool recovery_allowed);

typedef struct {
    void *context;
    bool (*normal_line_is_recognized)(
        void *context, const uint8_t *line, size_t line_byte_len);
    bool (*recovery_line_is_allowed)(
        void *context, const uint8_t *line, size_t line_byte_len);
    void (*note_recognized)(void *context);
    bool (*emit_booting)(void *context);
    bool (*emit_recovery_only)(void *context);
    bool (*emit_unknown)(void *context);
    bool (*dispatch_normal_line)(
        void *context, const uint8_t *line, size_t line_byte_len);
    bool (*dispatch_recovery_line)(
        void *context, const uint8_t *line, size_t line_byte_len);
} badge_usb_line_dispatch_hooks_t;

bool badge_usb_line_dispatch_run(
    const uint8_t *line,
    size_t line_byte_len,
    bool dispatch_ready,
    bool recovery_only,
    const badge_usb_line_dispatch_hooks_t *hooks);

typedef struct {
    badge_usb_binary_target_t target;
    uint32_t received;
    uint32_t size;
    uint32_t credit_remaining;
    uint32_t pending_credit;
    bool credit_v1;
    bool waiting_for_credit_receipt;
    bool durable_finalized;
} badge_usb_upload_policy_t;

typedef enum {
    BADGE_USB_SCANNER_CREDIT_CONTINUE = 0,
    BADGE_USB_SCANNER_CREDIT_COMPLETE,
    BADGE_USB_SCANNER_CREDIT_INVALID,
    BADGE_USB_SCANNER_CREDIT_WRITE_FAILED,
    BADGE_USB_SCANNER_CREDIT_ACCOUNTING_FAILED,
    BADGE_USB_SCANNER_CREDIT_COMMIT_FAILED,
    BADGE_USB_SCANNER_CREDIT_RECEIPT_MISSING,
    BADGE_USB_SCANNER_CREDIT_RECEIPT_FAILED,
    BADGE_USB_SCANNER_CREDIT_UNEXPECTED_RECEIPT,
    BADGE_USB_SCANNER_CREDIT_FINAL_MISMATCH,
    BADGE_USB_SCANNER_CREDIT_FINALIZE_FAILED,
    BADGE_USB_SCANNER_CREDIT_TERMINAL_FAILED,
    BADGE_USB_SCANNER_CREDIT_ACTIVATION_FAILED,
} badge_usb_scanner_credit_result_t;

/**
 * Scanner credit-v1 side effects. emit_required returns COMPLETED only when
 * the frame has drained; ENQUEUED asks the orchestrator to call
 * drain_required before it grants credit or permits activation.
 */
typedef struct {
    void *context;
    bool (*write_durable)(void *context,
                          const uint8_t *data, size_t len,
                          char *out_receipt, size_t out_receipt_len);
    bool (*commit_transport)(void *context);
    bool (*finalize_durable)(void *context,
                             char *out_receipt, size_t out_receipt_len);
    badge_usb_emit_result_t (*emit_required)(void *context,
                                             const char *receipt);
    bool (*drain_required)(void *context);
    bool (*complete_terminal)(void *context, bool delivered);
} badge_usb_scanner_credit_hooks_t;

void badge_usb_upload_policy_init(badge_usb_upload_policy_t *state);
bool badge_usb_upload_begin(badge_usb_upload_policy_t *state,
                            badge_usb_binary_target_t target,
                            uint32_t exact_size);
bool badge_usb_upload_begin_credit_v1(badge_usb_upload_policy_t *state,
                                      badge_usb_binary_target_t target,
                                      uint32_t exact_size);
bool badge_usb_upload_credit_enabled(
    const badge_usb_upload_policy_t *state);
bool badge_usb_upload_plan_credit_bytes(
    const badge_usb_upload_policy_t *state,
    size_t available, size_t *allowed);
bool badge_usb_upload_note_bytes(badge_usb_upload_policy_t *state,
                                 size_t bytes);
bool badge_usb_upload_credit_pending(
    const badge_usb_upload_policy_t *state);
uint32_t badge_usb_upload_pending_credit(
    const badge_usb_upload_policy_t *state);
bool badge_usb_upload_credit_result(badge_usb_upload_policy_t *state,
                                    bool delivered);
badge_usb_scanner_credit_result_t badge_usb_scanner_credit_process(
    badge_usb_upload_policy_t *state,
    const badge_usb_scanner_credit_hooks_t *hooks,
    const uint8_t *bytes, size_t length, bool final_chunk,
    char *receipt, size_t receipt_capacity);
const char *badge_usb_scanner_credit_result_error(
    badge_usb_scanner_credit_result_t result);
void badge_usb_upload_note_durable_finalize(badge_usb_upload_policy_t *state);
bool badge_usb_upload_terminal_result(badge_usb_upload_policy_t *state,
                                      bool delivered);
badge_usb_binary_target_t badge_usb_upload_abort(
    badge_usb_upload_policy_t *state);

#ifdef __cplusplus
}
#endif
