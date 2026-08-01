#include "badge_usb_uplink_ota.h"

#include <stdio.h>
#include <string.h>

static bool bounded_nonempty(const char *value, size_t capacity)
{
    return value && value[0] != '\0' && strnlen(value, capacity) < capacity;
}

static badge_usb_uplink_action_t abort_flow(
    badge_usb_uplink_ota_flow_t *flow)
{
    if (flow && !flow->aborted && !flow->committed) {
        flow->aborted = true;
        flow->finish_available = false;
        flow->cleanup_available = true;
        flow->terminal_available = true;
        flow->waiting_for_receipt = false;
        flow->pending_credit = 0U;
    }
    return BADGE_USB_UPLINK_ACTION_ABORT_DROP;
}

bool badge_usb_uplink_ota_manifest_from_fields(
    const badge_usb_uplink_manifest_fields_t *fields,
    uplink_ota_manifest_t *manifest, const char **error)
{
    if (error) {
        *error = NULL;
    }
    if (!fields || !manifest) {
        if (error) *error = "invalid_manifest";
        return false;
    }
    if (!fields->target || strcmp(fields->target, UPLINK_OTA_TARGET) != 0) {
        if (error) *error = "invalid_target";
        return false;
    }
    if (!fields->project || strcmp(fields->project, UPLINK_OTA_PROJECT) != 0) {
        if (error) *error = "invalid_project";
        return false;
    }
    if (!fields->hardware || strcmp(fields->hardware, UPLINK_OTA_HARDWARE) != 0) {
        if (error) *error = "invalid_hardware";
        return false;
    }
    if (!fields->flow_control || strcmp(fields->flow_control, "credit-v1") != 0) {
        if (error) *error = "invalid_flow_control";
        return false;
    }
    if (!bounded_nonempty(fields->version, sizeof(manifest->version))) {
        if (error) *error = "invalid_version";
        return false;
    }
    if (!fof_firmware_sha256_hex_is_valid(fields->sha256)) {
        if (error) *error = "invalid_sha256";
        return false;
    }
    if (fields->size == 0U) {
        if (error) *error = "invalid_size";
        return false;
    }
    if (fields->crc32 == 0U) {
        if (error) *error = "invalid_crc";
        return false;
    }

    memset(manifest, 0, sizeof(*manifest));
    strcpy(manifest->target, fields->target);
    strcpy(manifest->project, fields->project);
    strcpy(manifest->hardware, fields->hardware);
    strcpy(manifest->version, fields->version);
    strcpy(manifest->sha256, fields->sha256);
    manifest->size = fields->size;
    manifest->crc32 = fields->crc32;
    manifest->recovery_rewrite_same_version =
        fields->recovery_rewrite_same_version;
    return true;
}

void badge_usb_uplink_ota_flow_init(badge_usb_uplink_ota_flow_t *flow)
{
    if (flow) {
        memset(flow, 0, sizeof(*flow));
    }
}

badge_usb_uplink_action_t badge_usb_uplink_ota_flow_begin_result(
    badge_usb_uplink_ota_flow_t *flow,
    const uplink_usb_ota_result_t *result)
{
    if (!flow || !result || !result->ok || result->retryable ||
        !result->emit_required || result->reboot_required ||
        result->phase != UPLINK_USB_OTA_PHASE_READY ||
        result->received != 0U || result->total == 0U) {
        return abort_flow(flow);
    }
    uint32_t expected = result->total < UPLINK_OTA_CREDIT_BYTES
        ? result->total : UPLINK_OTA_CREDIT_BYTES;
    if (result->credit_bytes != expected) {
        return abort_flow(flow);
    }
    flow->transport_received = 0U;
    flow->durable_received = 0U;
    flow->total = result->total;
    flow->credit_remaining = 0U;
    flow->pending_credit = result->credit_bytes;
    flow->waiting_for_receipt = true;
    return BADGE_USB_UPLINK_ACTION_WAIT_RECEIPT;
}

badge_usb_uplink_action_t badge_usb_uplink_ota_flow_receipt_result(
    badge_usb_uplink_ota_flow_t *flow, bool receipt_ok)
{
    if (!flow || !flow->waiting_for_receipt || !receipt_ok ||
        flow->pending_credit == 0U) {
        return abort_flow(flow);
    }
    flow->credit_remaining = flow->pending_credit;
    flow->pending_credit = 0U;
    flow->waiting_for_receipt = false;
    return BADGE_USB_UPLINK_ACTION_CONTINUE;
}

badge_usb_uplink_action_t badge_usb_uplink_ota_flow_plan_read(
    badge_usb_uplink_ota_flow_t *flow, size_t available, size_t *allowed)
{
    if (allowed) {
        *allowed = 0U;
    }
    if (!flow || !allowed) {
        return abort_flow(flow);
    }
    if (flow->aborted || flow->committed || flow->total == 0U ||
        flow->transport_received > flow->total) {
        return abort_flow(flow);
    }
    if (flow->pending_retry_bytes != 0U) {
        return BADGE_USB_UPLINK_ACTION_RETRY_PENDING;
    }
    if (flow->waiting_for_receipt) {
        return BADGE_USB_UPLINK_ACTION_WAIT_RECEIPT;
    }
    uint32_t remaining = flow->total - flow->transport_received;
    if (available > (size_t)flow->credit_remaining ||
        available > (size_t)remaining) {
        return abort_flow(flow);
    }
    if (available == 0U) {
        return BADGE_USB_UPLINK_ACTION_CONTINUE;
    }
    *allowed = available < UPLINK_USB_OTA_MAX_WRITE_BYTES
        ? available : UPLINK_USB_OTA_MAX_WRITE_BYTES;
    return BADGE_USB_UPLINK_ACTION_CONTINUE;
}

badge_usb_uplink_action_t badge_usb_uplink_ota_flow_write_result(
    badge_usb_uplink_ota_flow_t *flow, size_t attempted,
    bool adapter_accepted,
    const uplink_usb_ota_result_t *result)
{
    if (!flow || !result || flow->aborted || flow->committed ||
        flow->total == 0U || flow->transport_received > flow->total ||
        attempted == 0U ||
        attempted > UPLINK_USB_OTA_MAX_WRITE_BYTES ||
        attempted > flow->credit_remaining ||
        attempted > flow->total - flow->transport_received ||
        (flow->pending_retry_bytes != 0U &&
         flow->pending_retry_bytes != attempted)) {
        return abort_flow(flow);
    }
    if (!adapter_accepted || !result->ok) {
        if (result->retryable && result->phase == UPLINK_USB_OTA_PHASE_NONE &&
            strcmp(result->error, "adapter_busy") == 0) {
            flow->pending_retry_bytes = (uint32_t)attempted;
            return badge_usb_uplink_ota_flow_note_retry(
                flow, BADGE_USB_UPLINK_RETRY_WRITE);
        }
        return abort_flow(flow);
    }
    uint32_t next_transport =
        flow->transport_received + (uint32_t)attempted;
    if (result->retryable || result->reboot_required ||
        result->total != flow->total ||
        result->received < flow->durable_received ||
        result->received > next_transport) {
        return abort_flow(flow);
    }

    flow->pending_retry_bytes = 0U;
    badge_usb_uplink_ota_flow_clear_retry(flow, BADGE_USB_UPLINK_RETRY_WRITE);
    flow->transport_received = next_transport;
    flow->durable_received = result->received;
    flow->credit_remaining -= (uint32_t)attempted;
    if (flow->transport_received == flow->total) {
        if (result->phase != UPLINK_USB_OTA_PHASE_PROGRESS ||
            result->emit_required || result->credit_bytes != 0U ||
            flow->durable_received != flow->transport_received) {
            return abort_flow(flow);
        }
        flow->finish_available = true;
        return BADGE_USB_UPLINK_ACTION_FINISH;
    }
    if (flow->credit_remaining == 0U) {
        uint32_t remaining = flow->total - flow->transport_received;
        uint32_t expected = remaining < UPLINK_OTA_CREDIT_BYTES
            ? remaining : UPLINK_OTA_CREDIT_BYTES;
        if (result->phase != UPLINK_USB_OTA_PHASE_CREDIT ||
            !result->emit_required || result->credit_bytes != expected ||
            flow->durable_received != flow->transport_received) {
            return abort_flow(flow);
        }
        flow->pending_credit = result->credit_bytes;
        flow->waiting_for_receipt = true;
        return BADGE_USB_UPLINK_ACTION_WAIT_RECEIPT;
    }
    if (result->phase != UPLINK_USB_OTA_PHASE_PROGRESS ||
        result->emit_required || result->credit_bytes != 0U) {
        return abort_flow(flow);
    }
    return BADGE_USB_UPLINK_ACTION_CONTINUE;
}

badge_usb_uplink_action_t badge_usb_uplink_ota_flow_finish_result(
    badge_usb_uplink_ota_flow_t *flow, bool adapter_accepted,
    const uplink_usb_ota_result_t *result)
{
    if (!flow || !result || !adapter_accepted || !result->ok ||
        result->retryable || !result->emit_required ||
        !result->reboot_required ||
        result->phase != UPLINK_USB_OTA_PHASE_COMMITTED ||
        flow->transport_received != flow->total ||
        flow->durable_received != flow->total ||
        result->received != flow->total || result->total != flow->total ||
        result->credit_bytes != 0U) {
        return abort_flow(flow);
    }
    flow->cleanup_available = true;
    flow->terminal_available = true;
    flow->committed = true;
    return BADGE_USB_UPLINK_ACTION_COMMITTED_RESTART;
}

badge_usb_uplink_action_t badge_usb_uplink_ota_flow_abort(
    badge_usb_uplink_ota_flow_t *flow)
{
    return abort_flow(flow);
}

badge_usb_uplink_action_t badge_usb_uplink_ota_begin_failure_action(
    const uplink_usb_ota_result_t *result)
{
    if (!result || !result->retryable ||
        result->phase != UPLINK_USB_OTA_PHASE_NONE) {
        return BADGE_USB_UPLINK_ACTION_ABORT_DROP;
    }
    if (strcmp(result->error, "operation_release_failed") == 0) {
        return BADGE_USB_UPLINK_ACTION_RETRY_CLEANUP;
    }
    if (strcmp(result->error, "operation_active") == 0 ||
        strcmp(result->error, "adapter_busy") == 0) {
        return BADGE_USB_UPLINK_ACTION_CONTINUE;
    }
    return BADGE_USB_UPLINK_ACTION_ABORT_DROP;
}

static uint8_t *retry_counter(badge_usb_uplink_ota_flow_t *flow,
                              badge_usb_uplink_retry_kind_t kind)
{
    if (!flow) return NULL;
    switch (kind) {
        case BADGE_USB_UPLINK_RETRY_WRITE: return &flow->write_retry_count;
        case BADGE_USB_UPLINK_RETRY_FINISH: return &flow->finish_retry_count;
        case BADGE_USB_UPLINK_RETRY_CLEANUP: return &flow->cleanup_retry_count;
        default: return NULL;
    }
}

badge_usb_uplink_action_t badge_usb_uplink_ota_flow_note_retry(
    badge_usb_uplink_ota_flow_t *flow, badge_usb_uplink_retry_kind_t kind)
{
    uint8_t *count = retry_counter(flow, kind);
    if (!count) return BADGE_USB_UPLINK_ACTION_RECOVERY_RESTART;
    if (*count < UINT8_MAX) (*count)++;
    return *count >= BADGE_USB_UPLINK_OTA_RETRY_LIMIT
        ? BADGE_USB_UPLINK_ACTION_RECOVERY_RESTART
        : BADGE_USB_UPLINK_ACTION_RETRY_PENDING;
}

void badge_usb_uplink_ota_flow_clear_retry(
    badge_usb_uplink_ota_flow_t *flow, badge_usb_uplink_retry_kind_t kind)
{
    uint8_t *count = retry_counter(flow, kind);
    if (count) *count = 0U;
}

badge_usb_uplink_action_t badge_usb_uplink_ota_flow_terminal_emit_result(
    badge_usb_uplink_ota_flow_t *flow, badge_usb_emit_result_t result)
{
    if (!flow || !flow->terminal_available) {
        return BADGE_USB_UPLINK_ACTION_ABORT_DROP;
    }
    if (result == BADGE_USB_EMIT_COMPLETED ||
        result == BADGE_USB_EMIT_ENQUEUED) {
        flow->terminal_available = false;
        flow->terminal_retry_count = 0U;
        return BADGE_USB_UPLINK_ACTION_CONTINUE;
    }
    if (result == BADGE_USB_EMIT_FAILED) {
        if (flow->terminal_retry_count < UINT8_MAX) {
            flow->terminal_retry_count++;
        }
        if (flow->terminal_retry_count < BADGE_USB_UPLINK_OTA_RETRY_LIMIT) {
            return BADGE_USB_UPLINK_ACTION_RETRY_TERMINAL;
        }
    }
    flow->terminal_available = false;
    return BADGE_USB_UPLINK_ACTION_RECOVERY_RESTART;
}

badge_usb_uplink_receipt_decision_t badge_usb_uplink_ota_receipt_decide(
    badge_usb_emit_result_t emitted, bool rescued_drain)
{
    if (emitted == BADGE_USB_EMIT_COMPLETED ||
        (emitted == BADGE_USB_EMIT_ENQUEUED && rescued_drain)) {
        return BADGE_USB_UPLINK_RECEIPT_ACKNOWLEDGED;
    }
    if (emitted == BADGE_USB_EMIT_ENQUEUED ||
        emitted == BADGE_USB_EMIT_POISONED) {
        return BADGE_USB_UPLINK_RECEIPT_CLEANUP_RECOVERY;
    }
    return BADGE_USB_UPLINK_RECEIPT_ABORT_TERMINAL;
}

badge_usb_uplink_receipt_decision_t badge_usb_uplink_ota_receipt_finalize(
    badge_usb_uplink_receipt_decision_t decision, bool flow_accepted)
{
    if (decision == BADGE_USB_UPLINK_RECEIPT_ACKNOWLEDGED &&
        !flow_accepted) {
        return BADGE_USB_UPLINK_RECEIPT_CLEANUP_RECOVERY;
    }
    return decision;
}

static bool take(bool *available)
{
    if (!available || !*available) {
        return false;
    }
    *available = false;
    return true;
}

bool badge_usb_uplink_ota_flow_take_finish(badge_usb_uplink_ota_flow_t *flow)
{
    return flow && take(&flow->finish_available);
}

bool badge_usb_uplink_ota_flow_take_cleanup(badge_usb_uplink_ota_flow_t *flow)
{
    return flow && take(&flow->cleanup_available);
}

bool badge_usb_uplink_ota_flow_take_terminal(badge_usb_uplink_ota_flow_t *flow)
{
    return flow && take(&flow->terminal_available);
}

static const char *phase_name(uplink_usb_ota_phase_t phase)
{
    switch (phase) {
        case UPLINK_USB_OTA_PHASE_READY: return "ready";
        case UPLINK_USB_OTA_PHASE_PROGRESS: return "progress";
        case UPLINK_USB_OTA_PHASE_CREDIT: return "credit";
        case UPLINK_USB_OTA_PHASE_COMMITTED: return "committed";
        case UPLINK_USB_OTA_PHASE_ABORTED: return "aborted";
#if defined(FOF_DC34_GAME_CANARY) || defined(UNIT_TESTING)
        case UPLINK_USB_OTA_PHASE_ERROR: return "error";
#endif
        default: return "none";
    }
}

#if defined(FOF_DC34_GAME_CANARY) || defined(UNIT_TESTING)
void badge_usb_uplink_ota_maintenance_required_result(
    uplink_usb_ota_result_t *result)
{
    if (!result) {
        return;
    }
    memset(result, 0, sizeof(*result));
    result->retryable = true;
    result->emit_required = true;
    result->reboot_required = true;
    result->phase = UPLINK_USB_OTA_PHASE_ERROR;
    snprintf(result->error, sizeof(result->error),
             "%s", "update_maintenance_required");
    snprintf(result->partition, sizeof(result->partition), "%s", "none");
}
#endif

size_t badge_usb_uplink_ota_render_result(
    const uplink_usb_ota_result_t *result, char *frame,
    size_t frame_capacity)
{
    if (!result || !frame || frame_capacity == 0U) {
        return 0U;
    }
    const char *error = result->error[0] ? result->error : "";
    const char *partition = result->partition[0] ? result->partition : "";
    int written = snprintf(
        frame, frame_capacity,
        "FOF_UPLINK_OTA:{\"ok\":%s,\"phase\":\"%s\","
        "\"partition\":\"%.*s\",\"received\":%lu,\"total\":%lu,"
        "\"credit_bytes\":%lu,\"retryable\":%s,\"reboot_required\":%s,"
        "\"error\":\"%.*s\"}\n",
        result->ok ? "true" : "false", phase_name(result->phase),
        (int)(UPLINK_USB_OTA_PARTITION_LABEL_BYTES - 1U), partition,
        (unsigned long)result->received, (unsigned long)result->total,
        (unsigned long)result->credit_bytes,
        result->retryable ? "true" : "false",
        result->reboot_required ? "true" : "false",
        (int)(UPLINK_USB_OTA_ERROR_BYTES - 1U), error);
    if (written <= 0 || (size_t)written >= frame_capacity) {
        frame[0] = '\0';
        return 0U;
    }
    return (size_t)written;
}

bool badge_usb_uplink_ota_run_committed(
    const badge_usb_uplink_ota_commit_hooks_t *hooks)
{
    if (!hooks) {
        return false;
    }
    if (hooks->emit_committed) {
        (void)hooks->emit_committed(hooks->context);
    }
    if (hooks->drain) {
        (void)hooks->drain(hooks->context);
    }
    if (hooks->restart) {
        return hooks->restart(hooks->context);
    }
    return false;
}
