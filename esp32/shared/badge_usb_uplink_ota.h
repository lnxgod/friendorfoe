#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "uplink_usb_ota.h"
#include "badge_usb_transport_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BADGE_USB_UPLINK_OTA_FRAME_BYTES 384U
#define BADGE_USB_UPLINK_OTA_RETRY_LIMIT 4U

typedef enum {
    BADGE_USB_UPLINK_ACTION_CONTINUE = 0,
    BADGE_USB_UPLINK_ACTION_RETRY_PENDING,
    BADGE_USB_UPLINK_ACTION_WAIT_RECEIPT,
    BADGE_USB_UPLINK_ACTION_FINISH,
    BADGE_USB_UPLINK_ACTION_ABORT_DROP,
    BADGE_USB_UPLINK_ACTION_COMMITTED_RESTART,
    BADGE_USB_UPLINK_ACTION_RETRY_CLEANUP,
    BADGE_USB_UPLINK_ACTION_RETRY_TERMINAL,
    BADGE_USB_UPLINK_ACTION_RECOVERY_RESTART,
} badge_usb_uplink_action_t;

typedef enum {
    BADGE_USB_UPLINK_RETRY_WRITE = 0,
    BADGE_USB_UPLINK_RETRY_FINISH,
    BADGE_USB_UPLINK_RETRY_CLEANUP,
} badge_usb_uplink_retry_kind_t;

typedef enum {
    BADGE_USB_UPLINK_RECEIPT_ACKNOWLEDGED = 0,
    BADGE_USB_UPLINK_RECEIPT_ABORT_TERMINAL,
    BADGE_USB_UPLINK_RECEIPT_CLEANUP_RECOVERY,
} badge_usb_uplink_receipt_decision_t;

typedef struct {
    const char *target;
    const char *project;
    const char *hardware;
    const char *version;
    const char *sha256;
    const char *flow_control;
    uint32_t size;
    uint32_t crc32;
    bool recovery_rewrite_same_version;
} badge_usb_uplink_manifest_fields_t;

typedef struct {
    uint32_t transport_received;
    uint32_t durable_received;
    uint32_t total;
    uint32_t credit_remaining;
    uint32_t pending_retry_bytes;
    uint32_t pending_credit;
    uint8_t write_retry_count;
    uint8_t finish_retry_count;
    uint8_t cleanup_retry_count;
    uint8_t terminal_retry_count;
    bool waiting_for_receipt;
    bool finish_available;
    bool cleanup_available;
    bool terminal_available;
    bool aborted;
    bool committed;
} badge_usb_uplink_ota_flow_t;

typedef struct {
    void *context;
    bool (*emit_committed)(void *context);
    bool (*drain)(void *context);
    bool (*restart)(void *context);
} badge_usb_uplink_ota_commit_hooks_t;

bool badge_usb_uplink_ota_manifest_from_fields(
    const badge_usb_uplink_manifest_fields_t *fields,
    uplink_ota_manifest_t *manifest, const char **error);

void badge_usb_uplink_ota_flow_init(badge_usb_uplink_ota_flow_t *flow);
badge_usb_uplink_action_t badge_usb_uplink_ota_flow_begin_result(
    badge_usb_uplink_ota_flow_t *flow,
    const uplink_usb_ota_result_t *result);
badge_usb_uplink_action_t badge_usb_uplink_ota_flow_receipt_result(
    badge_usb_uplink_ota_flow_t *flow, bool receipt_ok);
badge_usb_uplink_action_t badge_usb_uplink_ota_flow_plan_read(
    badge_usb_uplink_ota_flow_t *flow, size_t available, size_t *allowed);
badge_usb_uplink_action_t badge_usb_uplink_ota_flow_write_result(
    badge_usb_uplink_ota_flow_t *flow, size_t attempted,
    bool adapter_accepted,
    const uplink_usb_ota_result_t *result);
badge_usb_uplink_action_t badge_usb_uplink_ota_flow_finish_result(
    badge_usb_uplink_ota_flow_t *flow, bool adapter_accepted,
    const uplink_usb_ota_result_t *result);
badge_usb_uplink_action_t badge_usb_uplink_ota_flow_abort(
    badge_usb_uplink_ota_flow_t *flow);
badge_usb_uplink_action_t badge_usb_uplink_ota_begin_failure_action(
    const uplink_usb_ota_result_t *result);
badge_usb_uplink_action_t badge_usb_uplink_ota_flow_note_retry(
    badge_usb_uplink_ota_flow_t *flow, badge_usb_uplink_retry_kind_t kind);
void badge_usb_uplink_ota_flow_clear_retry(
    badge_usb_uplink_ota_flow_t *flow, badge_usb_uplink_retry_kind_t kind);
badge_usb_uplink_action_t badge_usb_uplink_ota_flow_terminal_emit_result(
    badge_usb_uplink_ota_flow_t *flow, badge_usb_emit_result_t result);
badge_usb_uplink_receipt_decision_t badge_usb_uplink_ota_receipt_decide(
    badge_usb_emit_result_t emitted, bool rescued_drain);
badge_usb_uplink_receipt_decision_t badge_usb_uplink_ota_receipt_finalize(
    badge_usb_uplink_receipt_decision_t decision, bool flow_accepted);
bool badge_usb_uplink_ota_flow_take_finish(badge_usb_uplink_ota_flow_t *flow);
bool badge_usb_uplink_ota_flow_take_cleanup(badge_usb_uplink_ota_flow_t *flow);
bool badge_usb_uplink_ota_flow_take_terminal(badge_usb_uplink_ota_flow_t *flow);

size_t badge_usb_uplink_ota_render_result(
    const uplink_usb_ota_result_t *result, char *frame,
    size_t frame_capacity);
#if defined(FOF_DC34_GAME_CANARY) || defined(UNIT_TESTING)
void badge_usb_uplink_ota_maintenance_required_result(
    uplink_usb_ota_result_t *result);
#endif
bool badge_usb_uplink_ota_run_committed(
    const badge_usb_uplink_ota_commit_hooks_t *hooks);

#ifdef __cplusplus
}
#endif
