#include "backend_scanner_relay.h"

#include <limits.h>
#include <string.h>

#include "backend_identity.h"
#include "backend_scanner_topology.h"

#define BACKEND_SCANNER_RELAY_STATUS_POLL_MS INT64_C(5000)

static bool fixed_string_equals_literal(
    const char *value, size_t capacity, const char *expected)
{
    if (value == NULL || expected == NULL) {
        return false;
    }
    const char *end = memchr(value, '\0', capacity);
    size_t expected_length = strlen(expected);
    return end != NULL && (size_t)(end - value) == expected_length &&
           memcmp(value, expected, expected_length) == 0;
}

static bool bounded_nonempty(const char *value, size_t capacity)
{
    if (value == NULL) {
        return false;
    }
    const char *end = memchr(value, '\0', capacity);
    return end != NULL && end != value;
}

static bool sha256_is_hex(const char value[65])
{
    if (value == NULL || value[64] != '\0') {
        return false;
    }
    for (size_t index = 0U; index < 64U; ++index) {
        char byte = value[index];
        bool digit = byte >= '0' && byte <= '9';
        bool lower = byte >= 'a' && byte <= 'f';
        bool upper = byte >= 'A' && byte <= 'F';
        if (!digit && !lower && !upper) {
            return false;
        }
    }
    return true;
}

static bool mac_is_specific(const uint8_t mac[6])
{
    if (mac == NULL) {
        return false;
    }
    bool all_zero = true;
    bool all_ff = true;
    for (size_t index = 0U; index < 6U; ++index) {
        all_zero = all_zero && mac[index] == 0U;
        all_ff = all_ff && mac[index] == UINT8_MAX;
    }
    return !all_zero && !all_ff;
}

static bool profile_is_valid(backend_scan_profile_t profile)
{
    return profile >= BACKEND_SCAN_PROFILE_QUIESCENT &&
           profile <= BACKEND_SCAN_PROFILE_HYBRID_FAILOVER;
}

static int64_t deadline_after(int64_t now_ms, int64_t interval_ms)
{
    if (now_ms > INT64_MAX - interval_ms) {
        return INT64_MAX;
    }
    return now_ms + interval_ms;
}

static void write_be16(uint8_t output[2], uint16_t value)
{
    output[0] = (uint8_t)(value >> 8);
    output[1] = (uint8_t)value;
}

static void write_be32(uint8_t output[4], uint32_t value)
{
    output[0] = (uint8_t)(value >> 24);
    output[1] = (uint8_t)(value >> 16);
    output[2] = (uint8_t)(value >> 8);
    output[3] = (uint8_t)value;
}

static void format_mac(const uint8_t mac[6], char output[18])
{
    static const char hex[] = "0123456789ABCDEF";
    for (size_t index = 0U; index < 6U; ++index) {
        output[index * 3U] = hex[mac[index] >> 4];
        output[index * 3U + 1U] = hex[mac[index] & 0x0FU];
        if (index < 5U) {
            output[index * 3U + 2U] = ':';
        }
    }
    output[17] = '\0';
}

static int hex_nibble(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static bool parse_mac(const char value[18], uint8_t output[6])
{
    if (value == NULL || output == NULL || value[17] != '\0') {
        return false;
    }
    for (size_t index = 0U; index < 6U; ++index) {
        size_t offset = index * 3U;
        int high = hex_nibble(value[offset]);
        int low = hex_nibble(value[offset + 1U]);
        if (high < 0 || low < 0 ||
            (index < 5U && value[offset + 2U] != ':')) {
            return false;
        }
        output[index] = (uint8_t)((unsigned)high << 4) | (uint8_t)low;
    }
    return true;
}

static bool relay_binding_is_live(const backend_scanner_relay_t *relay)
{
    return relay != NULL && relay->store != NULL &&
           backend_firmware_store_matches(
               relay->store,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
               relay->has_operation_id, &relay->operation_id,
#endif
               &relay->manifest) &&
           backend_firmware_store_relay_claim_matches(
               relay->store,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
               relay->has_operation_id, &relay->operation_id,
#endif
               relay->generation, relay->session_id);
}

static uint32_t relay_wire_generation(const backend_scanner_relay_t *relay)
{
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    return relay->session_generation;
#else
    return relay->generation;
#endif
}

static void action_init(
    const backend_scanner_relay_t *relay,
    backend_scanner_relay_action_kind_t kind,
    backend_scanner_relay_action_t *action)
{
    memset(action, 0, sizeof(*action));
    action->kind = kind;
    action->slot = relay->slot;
    action->session_id = relay->session_id;
    action->generation = relay_wire_generation(relay);
    action->topology_generation = relay->expected_topology_generation;
    action->dry_run = relay->dry_run;
}

static void queue_new_action(
    backend_scanner_relay_t *relay,
    const backend_scanner_relay_action_t *action)
{
    relay->pending_action = *action;
    relay->action_pending = true;
    relay->awaiting_receipt = false;
    relay->retry_pending = false;
    relay->retry_count = 0U;
}

static void queue_quiet(backend_scanner_relay_t *relay)
{
    backend_scanner_relay_action_t action;
    action_init(relay, BACKEND_SCANNER_RELAY_ACTION_SEND_QUIET, &action);
    queue_new_action(relay, &action);
}

static void queue_begin(backend_scanner_relay_t *relay)
{
    backend_scanner_relay_action_t action;
    action_init(relay, BACKEND_SCANNER_RELAY_ACTION_SEND_BEGIN, &action);
    action.control.type = BACKEND_SCANNER_CONTROL_OTA_BEGIN;
    backend_scanner_ota_begin_control_t *begin =
        &action.control.payload.ota_begin;
    begin->session_id = relay->session_id;
    begin->generation = relay_wire_generation(relay);
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    begin->manifest_generation = relay->manifest.generation;
#endif
    begin->component_slot = (uint8_t)relay->slot;
    format_mac(relay->expected_mac, begin->expected_mac);
    begin->expected_boot_id = relay->old_boot_id;
    begin->expected_topology_generation = relay->expected_topology_generation;
    strcpy(begin->target, relay->manifest.target);
    strcpy(begin->project, relay->manifest.project);
    strcpy(begin->hardware, relay->manifest.hardware);
    strcpy(begin->version, relay->manifest.version);
    begin->image_size = relay->manifest.image_size;
    begin->crc32 = relay->manifest.crc32;
    strcpy(begin->sha256, relay->manifest.sha256);
    begin->allow_same_version = relay->manifest.allow_same_version;
    begin->dry_run = relay->dry_run;
    queue_new_action(relay, &action);
}

static bool queue_chunk(backend_scanner_relay_t *relay)
{
    if (relay->acknowledged_bytes >= relay->manifest.image_size ||
        relay->next_sequence > UINT16_MAX) {
        return false;
    }
    backend_scanner_relay_action_t action;
    action_init(relay, BACKEND_SCANNER_RELAY_ACTION_SEND_CHUNK, &action);
    size_t remaining = (size_t)relay->manifest.image_size -
                       relay->acknowledged_bytes;
    size_t length = remaining < OTA_CHUNK_MAX_DATA ?
                    remaining : OTA_CHUNK_MAX_DATA;
    action.sequence = relay->next_sequence;
    action.next_sequence = relay->next_sequence + 1U;
    action.image_offset = relay->acknowledged_bytes;
    action.image_length = length;
    action.frame[0] = OTA_CHUNK_MAGIC;
    write_be16(action.frame + 1U, (uint16_t)action.sequence);
    write_be16(action.frame + 3U, (uint16_t)length);
    if (!backend_firmware_store_read(
            relay->store,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
            relay->has_operation_id, &relay->operation_id,
#endif
            relay->generation, action.image_offset,
            action.frame + OTA_CHUNK_HEADER_SIZE, length)) {
        return false;
    }
    uint32_t crc32 = backend_identity_crc32(
        action.frame + OTA_CHUNK_HEADER_SIZE, length);
    write_be32(action.frame + OTA_CHUNK_HEADER_SIZE + length, crc32);
    action.frame_length = OTA_CHUNK_HEADER_SIZE + length + OTA_CHUNK_CRC_SIZE;
    queue_new_action(relay, &action);
    return true;
}

static void queue_end(backend_scanner_relay_t *relay)
{
    backend_scanner_relay_action_t action;
    action_init(relay, BACKEND_SCANNER_RELAY_ACTION_SEND_END, &action);
    action.sequence = relay->next_sequence;
    action.next_sequence = relay->next_sequence;
    action.image_offset = relay->acknowledged_bytes;
    action.control.type = BACKEND_SCANNER_CONTROL_OTA_END;
    action.control.payload.ota_finish.session_id = relay->session_id;
    action.control.payload.ota_finish.generation = relay_wire_generation(relay);
    strcpy(action.control.payload.ota_finish.reason, "image_validated");
    queue_new_action(relay, &action);
}

static void queue_status_request(backend_scanner_relay_t *relay)
{
    backend_scanner_relay_action_t action;
    action_init(relay, BACKEND_SCANNER_RELAY_ACTION_REQUEST_STATUS, &action);
    action.control.type = BACKEND_SCANNER_CONTROL_HEALTH_REQUEST;
    action.control.payload.health_request.sequence = relay->next_sequence;
    queue_new_action(relay, &action);
}

static void release_and_discard_store(backend_scanner_relay_t *relay)
{
    if (relay == NULL || relay->store == NULL) {
        return;
    }
    backend_firmware_store_release_relay(
        relay->store,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        relay->has_operation_id, &relay->operation_id,
#endif
        relay->generation, relay->session_id);
    (void)backend_firmware_store_discard(
        relay->store,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        relay->has_operation_id, &relay->operation_id,
#endif
        relay->generation);
}

static backend_scanner_relay_event_result_t finish_terminal(
    backend_scanner_relay_t *relay, bool success)
{
    release_and_discard_store(relay);
    relay->state = success ? BACKEND_SCANNER_RELAY_COMPLETE
                           : BACKEND_SCANNER_RELAY_FAILED;
    relay->awaiting_receipt = false;
    relay->action_pending = false;
    relay->retry_pending = false;
    memset(&relay->pending_action, 0, sizeof(relay->pending_action));
    return success ? BACKEND_SCANNER_RELAY_EVENT_COMPLETE
                   : BACKEND_SCANNER_RELAY_EVENT_FAILED;
}

static backend_scanner_relay_event_result_t queue_restore(
    backend_scanner_relay_t *relay, bool success)
{
    backend_scanner_relay_action_t action;
    action_init(relay, BACKEND_SCANNER_RELAY_ACTION_SEND_RESTORE, &action);
    action.control.type = BACKEND_SCANNER_CONTROL_ROLE;
    action.control.payload.role.boot_id = relay->old_boot_id;
    action.control.payload.role.generation = relay->expected_role_generation;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    action.control.payload.role.topology_generation =
        relay->expected_topology_generation;
#endif
    action.control.payload.role.profile = relay->expected_profile;
    relay->cleanup_success = success;
    relay->state = BACKEND_SCANNER_RELAY_RESTORE_REQUESTED;
    queue_new_action(relay, &action);
    return BACKEND_SCANNER_RELAY_EVENT_ACCEPTED;
}

static backend_scanner_relay_event_result_t start_failure_cleanup(
    backend_scanner_relay_t *relay,
    backend_scanner_relay_event_result_t immediate_result)
{
    if (relay->state == BACKEND_SCANNER_RELAY_ABORT_REQUESTED ||
        relay->state == BACKEND_SCANNER_RELAY_RESTORE_REQUESTED ||
        relay->state == BACKEND_SCANNER_RELAY_RESTORE_WAIT) {
        return BACKEND_SCANNER_RELAY_EVENT_WAITING;
    }
    relay->cleanup_success = false;
    if (relay->remote_begin_sent) {
        backend_scanner_relay_action_t action;
        action_init(relay, BACKEND_SCANNER_RELAY_ACTION_SEND_ABORT, &action);
        action.control.type = BACKEND_SCANNER_CONTROL_OTA_ABORT;
        action.control.payload.ota_finish.session_id = relay->session_id;
        action.control.payload.ota_finish.generation =
            relay_wire_generation(relay);
        strcpy(action.control.payload.ota_finish.reason, "relay_cleanup");
        relay->state = BACKEND_SCANNER_RELAY_ABORT_REQUESTED;
        queue_new_action(relay, &action);
        return BACKEND_SCANNER_RELAY_EVENT_ACCEPTED;
    }
    if (relay->quiet_sent) {
        return queue_restore(relay, false);
    }
    (void)finish_terminal(relay, false);
    return immediate_result;
}

static backend_scanner_relay_event_result_t schedule_retry(
    backend_scanner_relay_t *relay)
{
    if (relay->retry_pending) {
        return BACKEND_SCANNER_RELAY_EVENT_RETRY_SCHEDULED;
    }
    if (relay->retry_count >= BACKEND_SCANNER_RELAY_MAX_RETRIES) {
        if (relay->state == BACKEND_SCANNER_RELAY_ABORT_REQUESTED) {
            return queue_restore(relay, false);
        }
        return start_failure_cleanup(
            relay, BACKEND_SCANNER_RELAY_EVENT_FAILED);
    }
    ++relay->retry_count;
    relay->pending_action = relay->last_action;
    relay->action_pending = true;
    relay->awaiting_receipt = false;
    relay->retry_pending = true;
    return BACKEND_SCANNER_RELAY_EVENT_RETRY_SCHEDULED;
}

static bool response_is_expected(const backend_scanner_relay_t *relay)
{
    return relay->awaiting_receipt ||
           (relay->action_pending && relay->retry_pending);
}

static void clear_expected_response(backend_scanner_relay_t *relay)
{
    relay->awaiting_receipt = false;
    if (relay->retry_pending) {
        relay->action_pending = false;
        relay->retry_pending = false;
        memset(&relay->pending_action, 0, sizeof(relay->pending_action));
    }
}

static bool receipt_is_completed_progress(
    const backend_scanner_relay_t *relay,
    const backend_scanner_relay_receipt_t *receipt)
{
    if (relay->state == BACKEND_SCANNER_RELAY_BEGIN_SENT &&
        receipt->kind == BACKEND_SCANNER_RELAY_RECEIPT_QUIET_ACK) {
        return true;
    }
    if (relay->state == BACKEND_SCANNER_RELAY_STREAMING &&
        (receipt->kind == BACKEND_SCANNER_RELAY_RECEIPT_ACK ||
         receipt->kind == BACKEND_SCANNER_RELAY_RECEIPT_NACK) &&
        receipt->next_sequence <= relay->next_sequence &&
        receipt->received <= relay->acknowledged_bytes) {
        return true;
    }
    if ((relay->state == BACKEND_SCANNER_RELAY_IMAGE_STAGED ||
         relay->state == BACKEND_SCANNER_RELAY_END_SENT) &&
        receipt->kind == BACKEND_SCANNER_RELAY_RECEIPT_STAGED &&
        receipt->next_sequence == relay->next_sequence &&
        receipt->received == relay->acknowledged_bytes) {
        return true;
    }
    return (relay->state == BACKEND_SCANNER_RELAY_REBOOT_WAIT ||
            relay->state == BACKEND_SCANNER_RELAY_CONVERGENCE_WAIT) &&
           receipt->kind == BACKEND_SCANNER_RELAY_RECEIPT_DONE &&
           receipt->next_sequence == relay->next_sequence &&
           receipt->received == relay->acknowledged_bytes;
}

void backend_scanner_relay_init(backend_scanner_relay_t *relay)
{
    if (relay == NULL) {
        return;
    }
    memset(relay, 0, sizeof(*relay));
    relay->state = BACKEND_SCANNER_RELAY_IDLE;
}

bool backend_scanner_relay_can_begin(
    backend_scanner_slot_t slot,
    const backend_ota_manifest_t *manifest,
    const uint8_t expected_mac[6],
    uint32_t generation)
{
    if ((slot != BACKEND_SCANNER_SLOT_BLE &&
         slot != BACKEND_SCANNER_SLOT_WIFI) ||
        manifest == NULL || !mac_is_specific(expected_mac) ||
        generation == 0U || generation != manifest->generation ||
        manifest->image_size == 0U ||
        manifest->image_size > FOF_BACKEND_SCANNER_CACHE_CAPACITY) {
        return false;
    }
    return fixed_string_equals_literal(
               manifest->target, sizeof(manifest->target),
               FOF_BACKEND_SCANNER_TARGET) &&
           fixed_string_equals_literal(
               manifest->project, sizeof(manifest->project),
               FOF_BACKEND_SCANNER_PROJECT) &&
           fixed_string_equals_literal(
               manifest->hardware, sizeof(manifest->hardware),
               FOF_BACKEND_HARDWARE) &&
           bounded_nonempty(manifest->version, sizeof(manifest->version)) &&
           sha256_is_hex(manifest->sha256);
}

bool backend_scanner_relay_begin(
    backend_scanner_relay_t *relay,
    backend_firmware_store_t *store,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    bool has_operation_id,
    const backend_ota_operation_id_t *operation_id,
    uint32_t session_generation,
#endif
    backend_scanner_slot_t slot,
    const backend_ota_manifest_t *manifest,
    const uint8_t expected_mac[6],
    uint32_t session_id,
    uint32_t generation,
    uint32_t old_boot_id,
    uint32_t expected_topology_generation,
    backend_scan_profile_t expected_profile,
    uint32_t expected_role_generation,
    bool dry_run)
{
    if (relay == NULL || manifest == NULL || expected_mac == NULL ||
        relay->state != BACKEND_SCANNER_RELAY_IDLE ||
        store == NULL || session_id == 0U || old_boot_id == 0U ||
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        !has_operation_id || operation_id == NULL || session_generation == 0U ||
#endif
        expected_topology_generation == 0U ||
        expected_role_generation == 0U || !profile_is_valid(expected_profile)) {
        return false;
    }
    const backend_ota_manifest_t admitted = *manifest;
    uint8_t admitted_mac[6];
    memcpy(admitted_mac, expected_mac, sizeof(admitted_mac));
    if (!backend_scanner_relay_can_begin(
            slot, &admitted, admitted_mac, generation) ||
        !backend_firmware_store_matches(
            store,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
            has_operation_id, operation_id,
#endif
            &admitted) ||
        !backend_firmware_store_claim_relay(
            store,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
            has_operation_id, operation_id,
#endif
            generation, session_id)) {
        return false;
    }

    relay->store = store;
    relay->manifest = admitted;
    relay->slot = slot;
    relay->expected_profile = expected_profile;
    memcpy(relay->expected_mac, admitted_mac, sizeof(relay->expected_mac));
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    relay->operation_id = *operation_id;
    relay->has_operation_id = true;
    relay->session_generation = session_generation;
#endif
    relay->session_id = session_id;
    relay->generation = generation;
    relay->old_boot_id = old_boot_id;
    relay->expected_topology_generation = expected_topology_generation;
    relay->expected_role_generation = expected_role_generation;
    relay->dry_run = dry_run;
    relay->state = BACKEND_SCANNER_RELAY_QUIET_REQUESTED;
    queue_quiet(relay);
    return true;
}

bool backend_scanner_relay_take_action(
    backend_scanner_relay_t *relay,
    int64_t now_ms,
    backend_scanner_relay_action_t *out)
{
    if (relay == NULL || out == NULL || now_ms < 0 ||
        !relay->action_pending) {
        return false;
    }
    const bool cleanup_action =
        relay->state == BACKEND_SCANNER_RELAY_ABORT_REQUESTED ||
        relay->state == BACKEND_SCANNER_RELAY_RESTORE_REQUESTED ||
        relay->state == BACKEND_SCANNER_RELAY_RESTORE_WAIT;
    if (!cleanup_action && !relay_binding_is_live(relay)) {
        (void)start_failure_cleanup(
            relay, BACKEND_SCANNER_RELAY_EVENT_FAILED);
        return false;
    }

    *out = relay->pending_action;
    relay->action_pending = false;
    relay->retry_pending = false;
    memset(&relay->pending_action, 0, sizeof(relay->pending_action));
    if (out->kind == BACKEND_SCANNER_RELAY_ACTION_REQUEST_STATUS) {
        relay->awaiting_receipt = false;
        relay->next_status_poll_ms = deadline_after(
            now_ms, BACKEND_SCANNER_RELAY_STATUS_POLL_MS);
        return true;
    }
    if (out->kind == BACKEND_SCANNER_RELAY_ACTION_SEND_RESTORE) {
        relay->state = BACKEND_SCANNER_RELAY_RESTORE_WAIT;
        relay->awaiting_receipt = false;
        relay->convergence_deadline_ms = deadline_after(
            now_ms, BACKEND_SCANNER_RELAY_CONVERGENCE_TIMEOUT_MS);
        relay->next_status_poll_ms = now_ms;
        relay->last_action = *out;
        return true;
    }
    if (out->kind == BACKEND_SCANNER_RELAY_ACTION_NONE) {
        return false;
    }
    if (out->kind == BACKEND_SCANNER_RELAY_ACTION_SEND_END) {
        relay->state = BACKEND_SCANNER_RELAY_END_SENT;
    }
    relay->quiet_sent = relay->quiet_sent ||
        out->kind == BACKEND_SCANNER_RELAY_ACTION_SEND_QUIET;
    relay->remote_begin_sent = relay->remote_begin_sent ||
        out->kind == BACKEND_SCANNER_RELAY_ACTION_SEND_BEGIN;
    relay->last_action = *out;
    relay->awaiting_receipt = true;
    relay->response_deadline_ms = deadline_after(
        now_ms, BACKEND_SCANNER_RELAY_RESPONSE_TIMEOUT_MS);
    return true;
}

backend_scanner_relay_event_result_t backend_scanner_relay_receive(
    backend_scanner_relay_t *relay,
    const backend_scanner_relay_receipt_t *receipt,
    int64_t now_ms)
{
    if (relay == NULL || receipt == NULL || now_ms < 0 ||
        receipt->kind < BACKEND_SCANNER_RELAY_RECEIPT_QUIET_ACK ||
        receipt->kind > BACKEND_SCANNER_RELAY_RECEIPT_ERROR) {
        return BACKEND_SCANNER_RELAY_EVENT_INVALID_ARGUMENT;
    }
    if (relay->state == BACKEND_SCANNER_RELAY_IDLE ||
        relay->state == BACKEND_SCANNER_RELAY_COMPLETE ||
        relay->state == BACKEND_SCANNER_RELAY_FAILED) {
        return BACKEND_SCANNER_RELAY_EVENT_INVALID_TRANSITION;
    }
    if (relay->state != BACKEND_SCANNER_RELAY_ABORT_REQUESTED &&
        !relay_binding_is_live(relay)) {
        return start_failure_cleanup(
            relay, BACKEND_SCANNER_RELAY_EVENT_FAILED);
    }
    if (receipt->session_id != relay->session_id ||
        receipt->generation != relay_wire_generation(relay)) {
        return BACKEND_SCANNER_RELAY_EVENT_IGNORED_STALE;
    }
    if (receipt->dry_run != relay->dry_run) {
        return start_failure_cleanup(
            relay, BACKEND_SCANNER_RELAY_EVENT_INVALID_TRANSITION);
    }
    if (relay->state == BACKEND_SCANNER_RELAY_ABORT_REQUESTED) {
        if (response_is_expected(relay) &&
            relay->last_action.kind ==
                BACKEND_SCANNER_RELAY_ACTION_SEND_ABORT &&
            receipt->kind == BACKEND_SCANNER_RELAY_RECEIPT_ERROR) {
            clear_expected_response(relay);
            return queue_restore(relay, false);
        }
        return BACKEND_SCANNER_RELAY_EVENT_IGNORED_STALE;
    }
    if (receipt->kind == BACKEND_SCANNER_RELAY_RECEIPT_ERROR) {
        return start_failure_cleanup(
            relay, BACKEND_SCANNER_RELAY_EVENT_FAILED);
    }

    if (relay->state == BACKEND_SCANNER_RELAY_QUIET_REQUESTED &&
        response_is_expected(relay) &&
        relay->last_action.kind == BACKEND_SCANNER_RELAY_ACTION_SEND_QUIET &&
        receipt->kind == BACKEND_SCANNER_RELAY_RECEIPT_QUIET_ACK) {
        clear_expected_response(relay);
        relay->state = BACKEND_SCANNER_RELAY_BEGIN_SENT;
        queue_begin(relay);
        return BACKEND_SCANNER_RELAY_EVENT_ACCEPTED;
    }

    if (relay->state == BACKEND_SCANNER_RELAY_BEGIN_SENT &&
        response_is_expected(relay) &&
        relay->last_action.kind == BACKEND_SCANNER_RELAY_ACTION_SEND_BEGIN &&
        receipt->kind == BACKEND_SCANNER_RELAY_RECEIPT_ACK) {
        if (receipt->sequence != 0U || receipt->next_sequence != 0U ||
            receipt->received != 0U) {
            return BACKEND_SCANNER_RELAY_EVENT_IGNORED_STALE;
        }
        clear_expected_response(relay);
        relay->state = BACKEND_SCANNER_RELAY_STREAMING;
        if (!queue_chunk(relay)) {
            return start_failure_cleanup(
                relay, BACKEND_SCANNER_RELAY_EVENT_FAILED);
        }
        return BACKEND_SCANNER_RELAY_EVENT_ACCEPTED;
    }

    if (relay->state == BACKEND_SCANNER_RELAY_STREAMING &&
        response_is_expected(relay) &&
        relay->last_action.kind == BACKEND_SCANNER_RELAY_ACTION_SEND_CHUNK) {
        const backend_scanner_relay_action_t *chunk = &relay->last_action;
        if (receipt->kind == BACKEND_SCANNER_RELAY_RECEIPT_NACK) {
            if (receipt->sequence != chunk->sequence ||
                receipt->next_sequence != relay->next_sequence ||
                receipt->received != relay->acknowledged_bytes) {
                return BACKEND_SCANNER_RELAY_EVENT_IGNORED_STALE;
            }
            return schedule_retry(relay);
        }
        if (receipt->kind == BACKEND_SCANNER_RELAY_RECEIPT_ACK) {
            size_t expected_received = chunk->image_offset +
                                       chunk->image_length;
            if (receipt->sequence != chunk->sequence ||
                receipt->next_sequence != chunk->next_sequence ||
                receipt->received != expected_received) {
                return BACKEND_SCANNER_RELAY_EVENT_IGNORED_STALE;
            }
            clear_expected_response(relay);
            relay->acknowledged_bytes = expected_received;
            relay->next_sequence = chunk->next_sequence;
            if (relay->acknowledged_bytes < relay->manifest.image_size &&
                !queue_chunk(relay)) {
                return start_failure_cleanup(
                    relay, BACKEND_SCANNER_RELAY_EVENT_FAILED);
            }
            return BACKEND_SCANNER_RELAY_EVENT_ACCEPTED;
        }
    }

    if (relay->state == BACKEND_SCANNER_RELAY_STREAMING &&
        !relay->awaiting_receipt && !relay->action_pending &&
        relay->acknowledged_bytes == relay->manifest.image_size &&
        receipt->kind == BACKEND_SCANNER_RELAY_RECEIPT_STAGED) {
        if (relay->next_sequence == 0U ||
            receipt->sequence != relay->next_sequence - 1U ||
            receipt->next_sequence != relay->next_sequence ||
            receipt->received != relay->manifest.image_size) {
            return BACKEND_SCANNER_RELAY_EVENT_IGNORED_STALE;
        }
        relay->state = BACKEND_SCANNER_RELAY_IMAGE_STAGED;
        queue_end(relay);
        return BACKEND_SCANNER_RELAY_EVENT_ACCEPTED;
    }

    if (relay->state == BACKEND_SCANNER_RELAY_END_SENT &&
        response_is_expected(relay) &&
        relay->last_action.kind == BACKEND_SCANNER_RELAY_ACTION_SEND_END &&
        receipt->kind == BACKEND_SCANNER_RELAY_RECEIPT_DONE) {
        if (relay->next_sequence == 0U ||
            receipt->sequence != relay->next_sequence - 1U ||
            receipt->next_sequence != relay->next_sequence ||
            receipt->received != relay->manifest.image_size) {
            return BACKEND_SCANNER_RELAY_EVENT_IGNORED_STALE;
        }
        clear_expected_response(relay);
        if (relay->dry_run) {
            return queue_restore(relay, true);
        }
        relay->state = BACKEND_SCANNER_RELAY_REBOOT_WAIT;
        relay->convergence_deadline_ms = deadline_after(
            now_ms, BACKEND_SCANNER_RELAY_CONVERGENCE_TIMEOUT_MS);
        relay->next_status_poll_ms = now_ms;
        queue_status_request(relay);
        return BACKEND_SCANNER_RELAY_EVENT_ACCEPTED;
    }

    if (receipt_is_completed_progress(relay, receipt)) {
        return BACKEND_SCANNER_RELAY_EVENT_IGNORED_STALE;
    }

    return start_failure_cleanup(
        relay, BACKEND_SCANNER_RELAY_EVENT_INVALID_TRANSITION);
}

backend_scanner_relay_event_result_t backend_scanner_relay_tick(
    backend_scanner_relay_t *relay,
    int64_t now_ms)
{
    if (relay == NULL || now_ms < 0) {
        return BACKEND_SCANNER_RELAY_EVENT_INVALID_ARGUMENT;
    }
    if (relay->state == BACKEND_SCANNER_RELAY_COMPLETE) {
        return BACKEND_SCANNER_RELAY_EVENT_COMPLETE;
    }
    if (relay->state == BACKEND_SCANNER_RELAY_FAILED) {
        return BACKEND_SCANNER_RELAY_EVENT_FAILED;
    }
    if (relay->state == BACKEND_SCANNER_RELAY_IDLE) {
        return BACKEND_SCANNER_RELAY_EVENT_INVALID_TRANSITION;
    }
    const bool cleanup_state =
        relay->state == BACKEND_SCANNER_RELAY_ABORT_REQUESTED ||
        relay->state == BACKEND_SCANNER_RELAY_RESTORE_REQUESTED ||
        relay->state == BACKEND_SCANNER_RELAY_RESTORE_WAIT;
    if (!cleanup_state && !relay_binding_is_live(relay)) {
        return start_failure_cleanup(
            relay, BACKEND_SCANNER_RELAY_EVENT_FAILED);
    }
    if (relay->awaiting_receipt && now_ms >= relay->response_deadline_ms) {
        return schedule_retry(relay);
    }
    if ((relay->state == BACKEND_SCANNER_RELAY_REBOOT_WAIT ||
         relay->state == BACKEND_SCANNER_RELAY_CONVERGENCE_WAIT) &&
        now_ms >= relay->convergence_deadline_ms) {
        return start_failure_cleanup(
            relay, BACKEND_SCANNER_RELAY_EVENT_FAILED);
    }
    if (relay->state == BACKEND_SCANNER_RELAY_RESTORE_WAIT &&
        now_ms >= relay->convergence_deadline_ms) {
        return finish_terminal(relay, false);
    }
    if ((relay->state == BACKEND_SCANNER_RELAY_REBOOT_WAIT ||
         relay->state == BACKEND_SCANNER_RELAY_CONVERGENCE_WAIT ||
         relay->state == BACKEND_SCANNER_RELAY_RESTORE_WAIT) &&
        !relay->action_pending && now_ms >= relay->next_status_poll_ms) {
        queue_status_request(relay);
        return BACKEND_SCANNER_RELAY_EVENT_ACCEPTED;
    }
    return BACKEND_SCANNER_RELAY_EVENT_WAITING;
}

backend_scanner_relay_event_result_t backend_scanner_relay_on_status(
    backend_scanner_relay_t *relay,
    const backend_scanner_status_t *status,
    uint32_t live_topology_generation,
    int64_t now_ms)
{
    if (relay == NULL || status == NULL || now_ms < 0) {
        return BACKEND_SCANNER_RELAY_EVENT_INVALID_ARGUMENT;
    }
    const bool restoring =
        relay->state == BACKEND_SCANNER_RELAY_RESTORE_WAIT;
    if (relay->state != BACKEND_SCANNER_RELAY_REBOOT_WAIT &&
        relay->state != BACKEND_SCANNER_RELAY_CONVERGENCE_WAIT &&
        !restoring) {
        return BACKEND_SCANNER_RELAY_EVENT_INVALID_TRANSITION;
    }
    if ((!restoring && !relay_binding_is_live(relay)) ||
        live_topology_generation != relay->expected_topology_generation ||
        status->schema != BACKEND_SCANNER_STATUS_SCHEMA) {
        return restoring
            ? finish_terminal(relay, false)
            : start_failure_cleanup(
                relay, BACKEND_SCANNER_RELAY_EVENT_FAILED);
    }
    uint8_t observed_mac[6];
    if (!parse_mac(status->mac, observed_mac) ||
        memcmp(observed_mac, relay->expected_mac,
               sizeof(relay->expected_mac)) != 0) {
        return restoring
            ? finish_terminal(relay, false)
            : start_failure_cleanup(
                relay, BACKEND_SCANNER_RELAY_EVENT_FAILED);
    }
    if (now_ms >= relay->convergence_deadline_ms) {
        return restoring
            ? finish_terminal(relay, false)
            : start_failure_cleanup(
                relay, BACKEND_SCANNER_RELAY_EVENT_FAILED);
    }

    if (restoring) {
        const bool binding_restored = status->boot_id != 0U &&
            (!relay->cleanup_success || status->boot_id == relay->old_boot_id) &&
            fixed_string_equals_literal(
                status->target, sizeof(status->target),
                FOF_BACKEND_SCANNER_TARGET) &&
            fixed_string_equals_literal(
                status->project, sizeof(status->project),
                FOF_BACKEND_SCANNER_PROJECT) &&
            fixed_string_equals_literal(
                status->hardware, sizeof(status->hardware),
                FOF_BACKEND_HARDWARE);
        const bool detection_restored = status->command_ingress &&
            status->role_acked &&
            status->role_generation == relay->expected_role_generation &&
            status->profile == relay->expected_profile &&
            backend_scanner_required_radio_healthy(
                status->profile, status->ble_healthy, status->wifi_healthy);
        if (binding_restored && detection_restored) {
            return finish_terminal(relay, relay->cleanup_success);
        }
        return BACKEND_SCANNER_RELAY_EVENT_WAITING;
    }

    if (relay->state == BACKEND_SCANNER_RELAY_REBOOT_WAIT) {
        if (status->boot_id == relay->old_boot_id) {
            return BACKEND_SCANNER_RELAY_EVENT_WAITING;
        }
        if (status->boot_id == 0U ||
            !fixed_string_equals_literal(
                status->target, sizeof(status->target),
                FOF_BACKEND_SCANNER_TARGET) ||
            !fixed_string_equals_literal(
                status->project, sizeof(status->project),
                FOF_BACKEND_SCANNER_PROJECT) ||
            !fixed_string_equals_literal(
                status->hardware, sizeof(status->hardware),
                FOF_BACKEND_HARDWARE) ||
            !fixed_string_equals_literal(
                status->version, sizeof(status->version),
                relay->manifest.version)) {
            return start_failure_cleanup(
                relay, BACKEND_SCANNER_RELAY_EVENT_FAILED);
        }
        relay->new_boot_id = status->boot_id;
        relay->state = BACKEND_SCANNER_RELAY_CONVERGENCE_WAIT;
    } else if (status->boot_id != relay->new_boot_id ||
               !fixed_string_equals_literal(
                   status->target, sizeof(status->target),
                   FOF_BACKEND_SCANNER_TARGET) ||
               !fixed_string_equals_literal(
                   status->project, sizeof(status->project),
                   FOF_BACKEND_SCANNER_PROJECT) ||
               !fixed_string_equals_literal(
                   status->hardware, sizeof(status->hardware),
                   FOF_BACKEND_HARDWARE) ||
               !fixed_string_equals_literal(
                   status->version, sizeof(status->version),
                   relay->manifest.version)) {
        return start_failure_cleanup(
            relay, BACKEND_SCANNER_RELAY_EVENT_FAILED);
    }

    bool healthy = status->command_ingress && status->role_acked &&
        status->role_generation == relay->expected_role_generation &&
        status->profile == relay->expected_profile &&
        backend_scanner_required_radio_healthy(
            status->profile, status->ble_healthy, status->wifi_healthy) &&
        fixed_string_equals_literal(
            status->rollback_state, sizeof(status->rollback_state), "valid");
    if (healthy) {
        return finish_terminal(relay, true);
    }
    return BACKEND_SCANNER_RELAY_EVENT_WAITING;
}

backend_scanner_relay_event_result_t backend_scanner_relay_abort(
    backend_scanner_relay_t *relay,
    int64_t now_ms)
{
    if (relay == NULL || now_ms < 0) {
        return BACKEND_SCANNER_RELAY_EVENT_INVALID_ARGUMENT;
    }
    if (relay->state == BACKEND_SCANNER_RELAY_IDLE ||
        relay->state == BACKEND_SCANNER_RELAY_COMPLETE ||
        relay->state == BACKEND_SCANNER_RELAY_FAILED) {
        return BACKEND_SCANNER_RELAY_EVENT_INVALID_TRANSITION;
    }
    return start_failure_cleanup(
        relay, BACKEND_SCANNER_RELAY_EVENT_FAILED);
}

bool backend_scanner_relay_reset(backend_scanner_relay_t *relay)
{
    if (relay == NULL ||
        (relay->state != BACKEND_SCANNER_RELAY_COMPLETE &&
         relay->state != BACKEND_SCANNER_RELAY_FAILED)) {
        return false;
    }
    if (relay->store != NULL) {
        backend_firmware_store_release_relay(
            relay->store,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
            relay->has_operation_id, &relay->operation_id,
#endif
            relay->generation, relay->session_id);
    }
    backend_scanner_relay_init(relay);
    return true;
}
