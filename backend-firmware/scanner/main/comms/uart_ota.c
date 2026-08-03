#include "uart_ota.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const uint8_t *bytes;
    size_t size;
} staged_reader_t;

static bool fixed_string_terminated(const char *value, size_t capacity)
{
    return value != NULL && memchr(value, '\0', capacity) != NULL;
}

static bool canonical_mac(const char *value, size_t capacity)
{
    if (!fixed_string_terminated(value, capacity) || strlen(value) != 17U) {
        return false;
    }
    for (size_t index = 0U; index < 17U; ++index) {
        if ((index + 1U) % 3U == 0U) {
            if (value[index] != ':') {
                return false;
            }
        } else if (!((value[index] >= '0' && value[index] <= '9') ||
                     (value[index] >= 'A' && value[index] <= 'F'))) {
            return false;
        }
    }
    return true;
}

static bool config_is_valid(const uart_ota_config_t *config)
{
    return config != NULL && config->running_version != NULL &&
           config->running_version[0] != '\0' &&
           strlen(config->running_version) < 32U &&
           config->inactive_slot_capacity ==
               FOF_BACKEND_SCANNER_OTA_CAPACITY &&
           config->ops.read_binding != NULL &&
           config->ops.psram_acquire != NULL &&
           config->ops.psram_release != NULL &&
           config->ops.emit_receipt != NULL &&
           config->ops.inactive_slot_begin != NULL &&
           config->ops.inactive_slot_write != NULL &&
           config->ops.inactive_slot_finish != NULL &&
           config->ops.inactive_slot_abort != NULL &&
           config->ops.inactive_slot_activate_pending_verify != NULL &&
           config->ops.request_reboot != NULL;
}

static void manifest_from_begin(
    const backend_scanner_ota_begin_control_t *begin,
    backend_ota_manifest_t *manifest)
{
    memset(manifest, 0, sizeof(*manifest));
    memcpy(manifest->target, begin->target, sizeof(manifest->target));
    memcpy(manifest->project, begin->project, sizeof(manifest->project));
    memcpy(manifest->hardware, begin->hardware, sizeof(manifest->hardware));
    memcpy(manifest->version, begin->version, sizeof(manifest->version));
    manifest->image_size = begin->image_size;
    manifest->crc32 = begin->crc32;
    memcpy(manifest->sha256, begin->sha256, sizeof(manifest->sha256));
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    manifest->generation = begin->manifest_generation;
#else
    manifest->generation = begin->generation;
#endif
    manifest->allow_same_version = begin->allow_same_version;
}

static bool binding_is_valid(const uart_ota_local_binding_t *binding)
{
    return binding != NULL && binding->component_slot < 2U &&
           canonical_mac(binding->mac, sizeof(binding->mac)) &&
           binding->boot_id != 0U && binding->topology_generation != 0U;
}

static bool binding_matches_begin(
    const uart_ota_local_binding_t *binding,
    const backend_scanner_ota_begin_control_t *begin)
{
    return binding_is_valid(binding) && begin != NULL &&
           fixed_string_terminated(
               begin->expected_mac, sizeof(begin->expected_mac)) &&
           binding->component_slot == begin->component_slot &&
           strcmp(binding->mac, begin->expected_mac) == 0 &&
           binding->boot_id == begin->expected_boot_id &&
           binding->topology_generation ==
               begin->expected_topology_generation;
}

static bool bindings_equal(
    const uart_ota_local_binding_t *left,
    const uart_ota_local_binding_t *right)
{
    return binding_is_valid(left) && binding_is_valid(right) &&
           left->component_slot == right->component_slot &&
           strcmp(left->mac, right->mac) == 0 &&
           left->boot_id == right->boot_id &&
           left->topology_generation == right->topology_generation;
}

static bool manifests_equal(
    const backend_ota_manifest_t *left,
    const backend_ota_manifest_t *right)
{
    return left != NULL && right != NULL &&
           memcmp(left->target, right->target, sizeof(left->target)) == 0 &&
           memcmp(left->project, right->project, sizeof(left->project)) == 0 &&
           memcmp(left->hardware, right->hardware,
                  sizeof(left->hardware)) == 0 &&
           memcmp(left->version, right->version, sizeof(left->version)) == 0 &&
           left->image_size == right->image_size &&
           left->crc32 == right->crc32 &&
           memcmp(left->sha256, right->sha256, sizeof(left->sha256)) == 0 &&
           left->generation == right->generation &&
           left->allow_same_version == right->allow_same_version;
}

static bool reason_is_safe(const char *reason)
{
    if (!fixed_string_terminated(reason, UART_OTA_RECEIPT_REASON_CAPACITY)) {
        return false;
    }
    for (size_t index = 0U; reason[index] != '\0'; ++index) {
        const char value = reason[index];
        if (!((value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == '_')) {
            return false;
        }
    }
    return true;
}

static bool emit_receipt(
    uart_ota_t *ota,
    uart_ota_receipt_kind_t type,
    uint16_t sequence,
    uint16_t next_sequence,
    const char *reason,
    backend_ota_image_result_t image_result)
{
    uart_ota_receipt_t receipt;
    memset(&receipt, 0, sizeof(receipt));
    receipt.type = type;
    receipt.session_id = ota->session_id;
    receipt.generation = ota->generation;
    receipt.sequence = sequence;
    receipt.next_sequence = next_sequence;
    receipt.received = ota->staged_size > UINT32_MAX
        ? UINT32_MAX : (uint32_t)ota->staged_size;
    receipt.dry_run = ota->dry_run;
    receipt.image_result = image_result;
    if (reason != NULL) {
        const size_t length = strlen(reason);
        if (length >= sizeof(receipt.reason)) {
            return false;
        }
        memcpy(receipt.reason, reason, length + 1U);
    }
    return ota->ops.emit_receipt(ota->ops.context, &receipt);
}

static void release_staging(uart_ota_t *ota)
{
    if (ota->staging != NULL) {
        ota->ops.psram_release(
            ota->ops.context, ota->staging, ota->manifest.image_size);
        ota->staging = NULL;
    }
}

static uart_ota_result_t fail_session(
    uart_ota_t *ota,
    uart_ota_result_t result,
    const char *reason,
    backend_ota_image_result_t image_result)
{
    ota->state = UART_OTA_STATE_FAILED;
    const uint16_t sequence = ota->expected_sequence == 0U
        ? 0U : (uint16_t)(ota->expected_sequence - 1U);
    const bool emitted = emit_receipt(
        ota, UART_OTA_RECEIPT_ERROR, sequence,
        ota->expected_sequence, reason, image_result);
    release_staging(ota);
    return emitted ? result : UART_OTA_RESULT_RECEIPT_ERROR;
}

static bool staged_read(
    void *context,
    size_t offset,
    uint8_t *output,
    size_t length)
{
    const staged_reader_t *reader = context;
    if (reader == NULL || output == NULL || length == 0U ||
        offset > reader->size || length > reader->size - offset) {
        return false;
    }
    memcpy(output, reader->bytes + offset, length);
    return true;
}

static backend_ota_image_result_t validate_staged_image(
    const uart_ota_t *ota)
{
    if (ota == NULL || ota->staging == NULL ||
        ota->staged_size != ota->manifest.image_size) {
        return BACKEND_OTA_IMAGE_READ_ERROR;
    }
    const staged_reader_t reader = {
        .bytes = ota->staging,
        .size = ota->staged_size,
    };
    return backend_ota_image_validate(
        &ota->manifest, BACKEND_IMAGE_SCANNER,
        staged_read, (void *)&reader);
}

static void reset_frame(uart_ota_t *ota)
{
    ota->frame_header_position = 0U;
    ota->frame_position = 0U;
    ota->frame_crc_position = 0U;
    ota->frame_length = 0U;
    ota->discard_remaining = 0U;
    ota->frame_duplicate = false;
    ota->frame_duplicate_matches = false;
    ota->frame_phase = UART_OTA_FRAME_HEADER;
}

static bool begin_matches_active(
    const uart_ota_t *ota,
    const backend_scanner_ota_begin_control_t *begin,
    const backend_ota_manifest_t *manifest)
{
    return ota != NULL && begin != NULL && manifest != NULL &&
           begin->session_id == ota->session_id &&
           begin->generation == ota->generation &&
           begin->dry_run == ota->dry_run &&
           manifests_equal(manifest, &ota->manifest) &&
           binding_matches_begin(&ota->bound_binding, begin);
}

static uart_ota_result_t replay_active_begin(
    uart_ota_t *ota,
    const backend_scanner_ota_begin_control_t *begin,
    const backend_ota_manifest_t *manifest)
{
    if ((ota->state != UART_OTA_STATE_STAGING &&
         ota->state != UART_OTA_STATE_IMAGE_STAGED) ||
        !begin_matches_active(ota, begin, manifest)) {
        return UART_OTA_RESULT_INVALID_STATE;
    }
    const uint16_t sequence = ota->expected_sequence == 0U
        ? 0U : (uint16_t)(ota->expected_sequence - 1U);
    const uart_ota_receipt_kind_t type =
        ota->state == UART_OTA_STATE_IMAGE_STAGED
            ? UART_OTA_RECEIPT_STAGED
            : UART_OTA_RECEIPT_ACK;
    return emit_receipt(
        ota, type, sequence, ota->expected_sequence,
        NULL, BACKEND_OTA_IMAGE_OK)
        ? UART_OTA_RESULT_OK
        : UART_OTA_RESULT_RECEIPT_ERROR;
}

static bool finish_control_is_bounded(
    const backend_scanner_ota_finish_control_t *finish)
{
    return finish != NULL && finish->session_id != 0U &&
           finish->generation != 0U &&
           fixed_string_terminated(finish->reason, sizeof(finish->reason));
}

static uart_ota_result_t check_finish(
    uart_ota_t *ota,
    const backend_scanner_ota_finish_control_t *finish,
    uart_ota_state_t required_state)
{
    if (ota == NULL || !ota->initialized ||
        !finish_control_is_bounded(finish)) {
        return UART_OTA_RESULT_INVALID_ARGUMENT;
    }
    if (ota->state != required_state) {
        return UART_OTA_RESULT_INVALID_STATE;
    }
    if (finish->session_id != ota->session_id) {
        return UART_OTA_RESULT_WRONG_SESSION;
    }
    if (finish->generation != ota->generation) {
        return UART_OTA_RESULT_STALE_GENERATION;
    }
    return UART_OTA_RESULT_OK;
}

bool uart_ota_init(uart_ota_t *ota, const uart_ota_config_t *config)
{
    if (ota == NULL || !config_is_valid(config)) {
        return false;
    }
    memset(ota, 0, sizeof(*ota));
    ota->state = UART_OTA_STATE_IDLE;
    ota->ops = config->ops;
    ota->inactive_slot_capacity = config->inactive_slot_capacity;
    memcpy(ota->running_version, config->running_version,
           strlen(config->running_version) + 1U);
    ota->initialized = true;
    return true;
}

uart_ota_result_t uart_ota_begin(
    uart_ota_t *ota,
    const backend_scanner_ota_begin_control_t *begin)
{
    if (ota == NULL || begin == NULL || !ota->initialized ||
        begin->session_id == 0U || begin->generation == 0U
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        || begin->manifest_generation == 0U
#endif
        ) {
        return UART_OTA_RESULT_INVALID_ARGUMENT;
    }
    backend_ota_manifest_t manifest;
    manifest_from_begin(begin, &manifest);
    if (ota->state != UART_OTA_STATE_IDLE) {
        return replay_active_begin(ota, begin, &manifest);
    }
    if (begin->generation <= ota->highest_generation) {
        return UART_OTA_RESULT_STALE_GENERATION;
    }
    if (backend_ota_manifest_admit(
            &manifest, BACKEND_IMAGE_SCANNER, ota->running_version,
            ota->inactive_slot_capacity) != BACKEND_OTA_ADMIT) {
        return UART_OTA_RESULT_MANIFEST_REJECTED;
    }

    uart_ota_local_binding_t binding;
    memset(&binding, 0, sizeof(binding));
    if (!ota->ops.read_binding(ota->ops.context, &binding) ||
        !binding_matches_begin(&binding, begin)) {
        return UART_OTA_RESULT_BINDING_MISMATCH;
    }
    uint8_t *staging = ota->ops.psram_acquire(
        ota->ops.context, manifest.image_size);
    if (staging == NULL) {
        return UART_OTA_RESULT_NO_PSRAM;
    }

    ota->manifest = manifest;
    ota->bound_binding = binding;
    ota->staging = staging;
    ota->staged_size = 0U;
    ota->session_id = begin->session_id;
    ota->generation = begin->generation;
    ota->highest_generation = begin->generation;
    ota->expected_sequence = 0U;
    ota->have_last_accepted = false;
    ota->dry_run = begin->dry_run;
    ota->state = UART_OTA_STATE_STAGING;
    reset_frame(ota);
    if (!emit_receipt(
            ota, UART_OTA_RECEIPT_ACK, 0U, 0U, NULL,
            BACKEND_OTA_IMAGE_OK)) {
        ota->state = UART_OTA_STATE_FAILED;
        release_staging(ota);
        return UART_OTA_RESULT_RECEIPT_ERROR;
    }
    return UART_OTA_RESULT_OK;
}

uint32_t uart_ota_chunk_crc32(const uint8_t *bytes, size_t size)
{
    if (bytes == NULL && size != 0U) {
        return 0U;
    }
    uint32_t crc = UINT32_C(0xFFFFFFFF);
    for (size_t index = 0U; index < size; ++index) {
        crc ^= bytes[index];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask =
                (uint32_t)-(int32_t)(crc & UINT32_C(1));
            crc = (crc >> 1) ^ (UINT32_C(0xEDB88320) & mask);
        }
    }
    return ~crc;
}

static uint16_t read_u16_be(const uint8_t bytes[2])
{
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static uint32_t read_u32_be(const uint8_t bytes[4])
{
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static uart_ota_result_t accept_complete_frame(uart_ota_t *ota)
{
    const uint32_t expected_crc = read_u32_be(ota->frame_crc);
    const uint32_t actual_crc = uart_ota_chunk_crc32(
        ota->frame_data, ota->frame_length);
    if (ota->frame_duplicate) {
        const uint16_t sequence = ota->frame_sequence;
        const bool exact = ota->frame_duplicate_matches &&
            actual_crc == expected_crc &&
            expected_crc == ota->last_accepted_crc &&
            ota->frame_length == ota->last_accepted_length;
        reset_frame(ota);
        if (!emit_receipt(
                ota,
                exact ? UART_OTA_RECEIPT_ACK : UART_OTA_RECEIPT_NACK,
                sequence, ota->expected_sequence,
                exact ? NULL : "duplicate_mismatch",
                BACKEND_OTA_IMAGE_OK)) {
            return fail_session(
                ota, UART_OTA_RESULT_RECEIPT_ERROR,
                "receipt", BACKEND_OTA_IMAGE_OK);
        }
        return UART_OTA_RESULT_OK;
    }
    if (actual_crc != expected_crc) {
        const uint16_t sequence = ota->frame_sequence;
        reset_frame(ota);
        if (!emit_receipt(
                ota, UART_OTA_RECEIPT_NACK, sequence,
                ota->expected_sequence, "chunk_crc",
                BACKEND_OTA_IMAGE_OK)) {
            return fail_session(
                ota, UART_OTA_RESULT_RECEIPT_ERROR,
                "receipt", BACKEND_OTA_IMAGE_OK);
        }
        return UART_OTA_RESULT_OK;
    }

    memcpy(ota->staging + ota->staged_size,
           ota->frame_data, ota->frame_length);
    ota->staged_size += ota->frame_length;
    const uint16_t sequence = ota->frame_sequence;
    if (ota->expected_sequence == UINT16_MAX) {
        return fail_session(
            ota, UART_OTA_RESULT_WIRE_ERROR,
            "sequence_overflow", BACKEND_OTA_IMAGE_OK);
    }
    ota->last_accepted_sequence = sequence;
    ota->last_accepted_length = ota->frame_length;
    ota->last_accepted_crc = expected_crc;
    ota->have_last_accepted = true;
    ++ota->expected_sequence;
    reset_frame(ota);
    if (!emit_receipt(
            ota, UART_OTA_RECEIPT_ACK, sequence,
            ota->expected_sequence, NULL, BACKEND_OTA_IMAGE_OK)) {
        return fail_session(
            ota, UART_OTA_RESULT_RECEIPT_ERROR,
            "receipt", BACKEND_OTA_IMAGE_OK);
    }
    if (ota->staged_size != ota->manifest.image_size) {
        return UART_OTA_RESULT_OK;
    }

    const backend_ota_image_result_t image_result =
        validate_staged_image(ota);
    if (image_result != BACKEND_OTA_IMAGE_OK) {
        return fail_session(
            ota, UART_OTA_RESULT_IMAGE_REJECTED,
            "image_rejected", image_result);
    }
    ota->state = UART_OTA_STATE_IMAGE_STAGED;
    if (!emit_receipt(
            ota, UART_OTA_RECEIPT_STAGED, sequence,
            ota->expected_sequence, NULL, image_result)) {
        return fail_session(
            ota, UART_OTA_RESULT_RECEIPT_ERROR,
            "receipt", image_result);
    }
    return UART_OTA_RESULT_OK;
}

uart_ota_result_t uart_ota_consume(
    uart_ota_t *ota,
    const uint8_t *bytes,
    size_t size,
    size_t *consumed)
{
    if (consumed != NULL) {
        *consumed = 0U;
    }
    if (ota == NULL || bytes == NULL || size == 0U || consumed == NULL ||
        !ota->initialized) {
        return UART_OTA_RESULT_INVALID_ARGUMENT;
    }
    if (ota->state != UART_OTA_STATE_STAGING) {
        return UART_OTA_RESULT_INVALID_STATE;
    }

    for (size_t index = 0U; index < size; ++index) {
        const uint8_t byte = bytes[index];
        *consumed = index + 1U;
        switch (ota->frame_phase) {
        case UART_OTA_FRAME_HEADER:
            if (ota->frame_header_position == 0U &&
                byte != OTA_CHUNK_MAGIC) {
                break;
            }
            ota->frame_header[ota->frame_header_position++] = byte;
            if (ota->frame_header_position == OTA_CHUNK_HEADER_SIZE) {
                ota->frame_sequence = read_u16_be(ota->frame_header + 1U);
                ota->frame_length = read_u16_be(ota->frame_header + 3U);
                ota->frame_header_position = 0U;
                if (ota->frame_length == 0U ||
                    ota->frame_length > OTA_CHUNK_MAX_DATA) {
                    return fail_session(
                        ota, UART_OTA_RESULT_WIRE_ERROR,
                        "chunk_length", BACKEND_OTA_IMAGE_OK);
                }
                if (ota->frame_sequence != ota->expected_sequence &&
                    ota->have_last_accepted &&
                    ota->frame_sequence == ota->last_accepted_sequence &&
                    ota->frame_length == ota->last_accepted_length) {
                    ota->frame_duplicate = true;
                    ota->frame_duplicate_matches = true;
                    ota->frame_position = 0U;
                    ota->frame_crc_position = 0U;
                    ota->frame_phase = UART_OTA_FRAME_DATA;
                    break;
                }
                if (ota->frame_sequence != ota->expected_sequence) {
                    const uint16_t rejected = ota->frame_sequence;
                    ota->discard_remaining =
                        (uint32_t)ota->frame_length + OTA_CHUNK_CRC_SIZE;
                    ota->frame_phase = UART_OTA_FRAME_DISCARD;
                    if (!emit_receipt(
                            ota, UART_OTA_RECEIPT_NACK, rejected,
                            ota->expected_sequence, "sequence",
                            BACKEND_OTA_IMAGE_OK)) {
                        return fail_session(
                            ota, UART_OTA_RESULT_RECEIPT_ERROR,
                            "receipt", BACKEND_OTA_IMAGE_OK);
                    }
                    break;
                }
                if (ota->staged_size > ota->manifest.image_size ||
                    ota->frame_length >
                        ota->manifest.image_size - ota->staged_size) {
                    return fail_session(
                        ota, UART_OTA_RESULT_WIRE_ERROR,
                        "image_overrun", BACKEND_OTA_IMAGE_OK);
                }
                ota->frame_position = 0U;
                ota->frame_crc_position = 0U;
                ota->frame_phase = UART_OTA_FRAME_DATA;
            }
            break;

        case UART_OTA_FRAME_DATA:
            if (ota->frame_duplicate) {
                if (ota->staged_size < ota->last_accepted_length ||
                    byte != ota->staging[
                        ota->staged_size - ota->last_accepted_length +
                        ota->frame_position]) {
                    ota->frame_duplicate_matches = false;
                }
            }
            ota->frame_data[ota->frame_position++] = byte;
            if (ota->frame_position == ota->frame_length) {
                ota->frame_phase = UART_OTA_FRAME_CRC;
            }
            break;

        case UART_OTA_FRAME_CRC:
            ota->frame_crc[ota->frame_crc_position++] = byte;
            if (ota->frame_crc_position == OTA_CHUNK_CRC_SIZE) {
                const uart_ota_result_t result = accept_complete_frame(ota);
                if (result != UART_OTA_RESULT_OK ||
                    ota->state != UART_OTA_STATE_STAGING) {
                    return result;
                }
            }
            break;

        case UART_OTA_FRAME_DISCARD:
            if (ota->discard_remaining > 0U) {
                --ota->discard_remaining;
            }
            if (ota->discard_remaining == 0U) {
                reset_frame(ota);
            }
            break;

        default:
            return fail_session(
                ota, UART_OTA_RESULT_WIRE_ERROR,
                "wire_state", BACKEND_OTA_IMAGE_OK);
        }
    }
    return UART_OTA_RESULT_OK;
}

static uart_ota_result_t recheck_binding_and_image(uart_ota_t *ota)
{
    uart_ota_local_binding_t binding;
    memset(&binding, 0, sizeof(binding));
    if (!ota->ops.read_binding(ota->ops.context, &binding) ||
        !bindings_equal(&binding, &ota->bound_binding)) {
        return fail_session(
            ota, UART_OTA_RESULT_BINDING_MISMATCH,
            "binding_changed", BACKEND_OTA_IMAGE_OK);
    }
    const backend_ota_image_result_t image_result =
        validate_staged_image(ota);
    if (image_result != BACKEND_OTA_IMAGE_OK) {
        return fail_session(
            ota, UART_OTA_RESULT_IMAGE_REJECTED,
            "image_rejected", image_result);
    }
    return UART_OTA_RESULT_OK;
}

static uart_ota_result_t replay_completed_end(
    uart_ota_t *ota,
    const backend_scanner_ota_finish_control_t *end)
{
    if (end->session_id != ota->session_id) {
        return UART_OTA_RESULT_WRONG_SESSION;
    }
    if (end->generation != ota->generation) {
        return UART_OTA_RESULT_STALE_GENERATION;
    }

    const uint16_t sequence = ota->expected_sequence == 0U
        ? 0U : (uint16_t)(ota->expected_sequence - 1U);
    if (!emit_receipt(
            ota, UART_OTA_RECEIPT_DONE, sequence,
            ota->expected_sequence, NULL, BACKEND_OTA_IMAGE_OK)) {
        return UART_OTA_RESULT_RECEIPT_ERROR;
    }
    if (ota->state == UART_OTA_STATE_DRY_RUN_COMPLETE) {
        return UART_OTA_RESULT_OK;
    }
    if (!ota->ops.request_reboot(ota->ops.context)) {
        if (!emit_receipt(
                ota, UART_OTA_RECEIPT_ERROR, sequence,
                ota->expected_sequence, "reboot_failed",
                BACKEND_OTA_IMAGE_OK)) {
            return UART_OTA_RESULT_RECEIPT_ERROR;
        }
        return UART_OTA_RESULT_FLASH_ERROR;
    }
    return UART_OTA_RESULT_OK;
}

uart_ota_result_t uart_ota_end(
    uart_ota_t *ota,
    const backend_scanner_ota_finish_control_t *end)
{
    if (ota == NULL || !ota->initialized ||
        !finish_control_is_bounded(end)) {
        return UART_OTA_RESULT_INVALID_ARGUMENT;
    }
    if (ota->state == UART_OTA_STATE_DRY_RUN_COMPLETE ||
        ota->state == UART_OTA_STATE_PENDING_VERIFY) {
        return replay_completed_end(ota, end);
    }

    uart_ota_result_t checked = check_finish(
        ota, end, UART_OTA_STATE_IMAGE_STAGED);
    if (checked != UART_OTA_RESULT_OK) {
        return checked;
    }
    checked = recheck_binding_and_image(ota);
    if (checked != UART_OTA_RESULT_OK) {
        return checked;
    }

    const uint16_t sequence = (uint16_t)(ota->expected_sequence - 1U);
    if (ota->dry_run) {
        ota->state = UART_OTA_STATE_DRY_RUN_COMPLETE;
        const bool emitted = emit_receipt(
            ota, UART_OTA_RECEIPT_DONE, sequence,
            ota->expected_sequence, NULL, BACKEND_OTA_IMAGE_OK);
        release_staging(ota);
        if (!emitted) {
            ota->state = UART_OTA_STATE_FAILED;
            return UART_OTA_RESULT_RECEIPT_ERROR;
        }
        return UART_OTA_RESULT_OK;
    }

    ota->state = UART_OTA_STATE_WRITING;
    if (!ota->ops.inactive_slot_begin(
            ota->ops.context, ota->manifest.image_size)) {
        return fail_session(
            ota, UART_OTA_RESULT_FLASH_ERROR,
            "flash_begin", BACKEND_OTA_IMAGE_OK);
    }
    size_t written = 0U;
    while (written < ota->staged_size) {
        size_t amount = ota->staged_size - written;
        if (amount > UART_OTA_FLASH_WRITE_BYTES) {
            amount = UART_OTA_FLASH_WRITE_BYTES;
        }
        if (!ota->ops.inactive_slot_write(
                ota->ops.context, written,
                ota->staging + written, amount)) {
            ota->ops.inactive_slot_abort(ota->ops.context);
            return fail_session(
                ota, UART_OTA_RESULT_FLASH_ERROR,
                "flash_write", BACKEND_OTA_IMAGE_OK);
        }
        written += amount;
    }
    if (!ota->ops.inactive_slot_finish(ota->ops.context)) {
        ota->ops.inactive_slot_abort(ota->ops.context);
        return fail_session(
            ota, UART_OTA_RESULT_FLASH_ERROR,
            "flash_finish", BACKEND_OTA_IMAGE_OK);
    }
    if (!ota->ops.inactive_slot_activate_pending_verify(
            ota->ops.context)) {
        ota->ops.inactive_slot_abort(ota->ops.context);
        return fail_session(
            ota, UART_OTA_RESULT_FLASH_ERROR,
            "pending_verify", BACKEND_OTA_IMAGE_OK);
    }

    ota->state = UART_OTA_STATE_PENDING_VERIFY;
    const bool emitted = emit_receipt(
        ota, UART_OTA_RECEIPT_DONE, sequence,
        ota->expected_sequence, NULL, BACKEND_OTA_IMAGE_OK);
    release_staging(ota);
    if (!emitted) {
        return UART_OTA_RESULT_RECEIPT_ERROR;
    }
    if (!ota->ops.request_reboot(ota->ops.context)) {
        if (!emit_receipt(
                ota, UART_OTA_RECEIPT_ERROR, sequence,
                ota->expected_sequence, "reboot_failed",
                BACKEND_OTA_IMAGE_OK)) {
            return UART_OTA_RESULT_RECEIPT_ERROR;
        }
        return UART_OTA_RESULT_FLASH_ERROR;
    }
    return UART_OTA_RESULT_OK;
}

uart_ota_result_t uart_ota_abort(
    uart_ota_t *ota,
    const backend_scanner_ota_finish_control_t *abort_control)
{
    if (ota == NULL || !ota->initialized ||
        !finish_control_is_bounded(abort_control)) {
        return UART_OTA_RESULT_INVALID_ARGUMENT;
    }
    if (ota->state != UART_OTA_STATE_STAGING &&
        ota->state != UART_OTA_STATE_IMAGE_STAGED) {
        return UART_OTA_RESULT_INVALID_STATE;
    }
    if (abort_control->session_id != ota->session_id) {
        return UART_OTA_RESULT_WRONG_SESSION;
    }
    if (abort_control->generation != ota->generation) {
        return UART_OTA_RESULT_STALE_GENERATION;
    }
    return fail_session(
        ota, UART_OTA_RESULT_OK,
        "aborted", BACKEND_OTA_IMAGE_OK);
}

void uart_ota_reset(uart_ota_t *ota)
{
    if (ota == NULL || !ota->initialized) {
        return;
    }
    release_staging(ota);
    const uart_ota_ops_t ops = ota->ops;
    const size_t capacity = ota->inactive_slot_capacity;
    const uint32_t highest_generation = ota->highest_generation;
    char running_version[sizeof(ota->running_version)];
    memcpy(running_version, ota->running_version, sizeof(running_version));
    memset(ota, 0, sizeof(*ota));
    ota->state = UART_OTA_STATE_IDLE;
    ota->ops = ops;
    ota->inactive_slot_capacity = capacity;
    ota->highest_generation = highest_generation;
    memcpy(ota->running_version, running_version, sizeof(running_version));
    ota->initialized = true;
}

bool uart_ota_is_receiving_binary(const uart_ota_t *ota)
{
    return ota != NULL && ota->initialized &&
           ota->state == UART_OTA_STATE_STAGING;
}

uint32_t uart_ota_received(const uart_ota_t *ota)
{
    return ota == NULL || ota->staged_size > UINT32_MAX
        ? 0U : (uint32_t)ota->staged_size;
}

uint16_t uart_ota_expected_sequence(const uart_ota_t *ota)
{
    return ota == NULL ? 0U : ota->expected_sequence;
}

static const char *receipt_type_name(uart_ota_receipt_kind_t type)
{
    switch (type) {
    case UART_OTA_RECEIPT_ACK:
        return MSG_TYPE_OTA_ACK;
    case UART_OTA_RECEIPT_NACK:
        return MSG_TYPE_OTA_NACK;
    case UART_OTA_RECEIPT_STAGED:
        return MSG_TYPE_OTA_STAGED;
    case UART_OTA_RECEIPT_DONE:
        return MSG_TYPE_OTA_DONE;
    case UART_OTA_RECEIPT_ERROR:
        return MSG_TYPE_OTA_ERROR;
    default:
        return NULL;
    }
}

size_t uart_ota_receipt_to_json(
    const uart_ota_receipt_t *receipt,
    char *output,
    size_t capacity)
{
    if (output != NULL && capacity > 0U) {
        output[0] = '\0';
    }
    const char *type = receipt == NULL
        ? NULL : receipt_type_name(receipt->type);
    if (receipt == NULL || output == NULL || capacity == 0U ||
        capacity > UART_JSON_MAX_SIZE || type == NULL ||
        receipt->session_id == 0U || receipt->generation == 0U ||
        !reason_is_safe(receipt->reason) ||
        ((receipt->type == UART_OTA_RECEIPT_NACK ||
          receipt->type == UART_OTA_RECEIPT_ERROR) &&
         receipt->reason[0] == '\0') ||
        ((receipt->type != UART_OTA_RECEIPT_NACK &&
          receipt->type != UART_OTA_RECEIPT_ERROR) &&
         receipt->reason[0] != '\0')) {
        return 0U;
    }

    int written;
    if (receipt->reason[0] == '\0') {
        written = snprintf(
            output, capacity,
            "{\"type\":\"%s\",\"session_id\":%" PRIu32
            ",\"generation\":%" PRIu32
            ",\"sequence\":%u,\"next_sequence\":%u,"
            "\"received\":%" PRIu32 ",\"dry_run\":%s,"
            "\"reason\":null}",
            type, receipt->session_id, receipt->generation,
            (unsigned)receipt->sequence,
            (unsigned)receipt->next_sequence,
            receipt->received,
            receipt->dry_run ? "true" : "false");
    } else {
        written = snprintf(
            output, capacity,
            "{\"type\":\"%s\",\"session_id\":%" PRIu32
            ",\"generation\":%" PRIu32
            ",\"sequence\":%u,\"next_sequence\":%u,"
            "\"received\":%" PRIu32 ",\"dry_run\":%s,"
            "\"reason\":\"%s\"}",
            type, receipt->session_id, receipt->generation,
            (unsigned)receipt->sequence,
            (unsigned)receipt->next_sequence,
            receipt->received,
            receipt->dry_run ? "true" : "false",
            receipt->reason);
    }
    if (written < 0 || (size_t)written >= capacity) {
        output[0] = '\0';
        return 0U;
    }
    return (size_t)written;
}
