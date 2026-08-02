#include "backend_ota_journal.h"

#include <string.h>

#define BACKEND_OTA_JOURNAL_CRC_OFFSET 343U
#define BACKEND_OTA_JOURNAL_PHASE_OFFSET 325U
#define BACKEND_OTA_JOURNAL_WRITES_BEFORE_OFFSET 329U

typedef struct {
    uint8_t *bytes;
    size_t capacity;
    size_t position;
    bool valid;
} journal_writer_t;

typedef struct {
    const uint8_t *bytes;
    size_t length;
    size_t position;
    bool valid;
} journal_reader_t;

static void writer_bytes(
    journal_writer_t *writer, const void *bytes, size_t length)
{
    if (writer == NULL || !writer->valid ||
        (bytes == NULL && length != 0U) ||
        writer->position > writer->capacity ||
        length > writer->capacity - writer->position) {
        if (writer != NULL) {
            writer->valid = false;
        }
        return;
    }
    if (length != 0U) {
        memcpy(writer->bytes + writer->position, bytes, length);
    }
    writer->position += length;
}

static void writer_u8(journal_writer_t *writer, uint8_t value)
{
    writer_bytes(writer, &value, sizeof(value));
}

static void writer_u32_le(journal_writer_t *writer, uint32_t value)
{
    const uint8_t bytes[4] = {
        (uint8_t)value,
        (uint8_t)(value >> 8U),
        (uint8_t)(value >> 16U),
        (uint8_t)(value >> 24U),
    };
    writer_bytes(writer, bytes, sizeof(bytes));
}

static void reader_bytes(
    journal_reader_t *reader, void *out, size_t length)
{
    if (reader == NULL || !reader->valid ||
        (out == NULL && length != 0U) ||
        reader->position > reader->length ||
        length > reader->length - reader->position) {
        if (reader != NULL) {
            reader->valid = false;
        }
        return;
    }
    if (length != 0U) {
        memcpy(out, reader->bytes + reader->position, length);
    }
    reader->position += length;
}

static uint8_t reader_u8(journal_reader_t *reader)
{
    uint8_t value = 0U;
    reader_bytes(reader, &value, sizeof(value));
    return value;
}

static uint32_t reader_u32_le(journal_reader_t *reader)
{
    uint8_t bytes[4] = {0U};
    reader_bytes(reader, bytes, sizeof(bytes));
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

static uint32_t journal_crc32(const uint8_t *bytes, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0U; index < length; ++index) {
        crc ^= bytes[index];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask =
                (uint32_t)-(int32_t)(crc & UINT32_C(1));
            crc = (crc >> 1U) ^ (UINT32_C(0xEDB88320) & mask);
        }
    }
    return ~crc;
}

static bool fixed_string_is_canonical(
    const char *value, size_t capacity, bool allow_empty)
{
    if (value == NULL || capacity == 0U) {
        return false;
    }
    const char *terminator = memchr(value, '\0', capacity);
    if (terminator == NULL || (!allow_empty && terminator == value)) {
        return false;
    }
    const size_t length = (size_t)(terminator - value);
    for (size_t index = 0U; index < length; ++index) {
        const uint8_t character = (uint8_t)value[index];
        if (character <= UINT8_C(0x20) || character > UINT8_C(0x7E)) {
            return false;
        }
    }
    for (size_t index = length + 1U; index < capacity; ++index) {
        if (value[index] != '\0') {
            return false;
        }
    }
    return true;
}

static bool fixed_string_matches(
    const char *value, size_t capacity, const char *expected)
{
    const size_t expected_length = expected == NULL ? 0U : strlen(expected);
    return expected != NULL && expected_length < capacity &&
           fixed_string_is_canonical(value, capacity, false) &&
           memcmp(value, expected, expected_length + 1U) == 0;
}

static bool sha256_is_lowercase(const char value[65])
{
    if (value == NULL) {
        return false;
    }
    for (size_t index = 0U; index < 64U; ++index) {
        if (!((value[index] >= '0' && value[index] <= '9') ||
              (value[index] >= 'a' && value[index] <= 'f'))) {
            return false;
        }
    }
    return value[64] == '\0';
}

static bool component_identity_is_exact(
    const backend_ota_journal_record_t *record)
{
    const bool uplink = record->component == BACKEND_OTA_COMPONENT_UPLINK;
    const char *target = uplink
        ? FOF_BACKEND_UPLINK_TARGET : FOF_BACKEND_SCANNER_TARGET;
    const char *project = uplink
        ? FOF_BACKEND_UPLINK_PROJECT : FOF_BACKEND_SCANNER_PROJECT;
    return fixed_string_matches(
               record->catalog_name, sizeof(record->catalog_name), target) &&
           fixed_string_matches(
               record->manifest.target, sizeof(record->manifest.target),
               target) &&
           fixed_string_matches(
               record->manifest.project, sizeof(record->manifest.project),
               project) &&
           fixed_string_matches(
               record->manifest.hardware, sizeof(record->manifest.hardware),
               FOF_BACKEND_HARDWARE);
}

static bool hardware_mac_is_valid(const uint8_t mac[6])
{
    if (mac == NULL || (mac[0] & UINT8_C(1)) != 0U) {
        return false;
    }
    uint8_t combined = 0U;
    for (size_t index = 0U; index < 6U; ++index) {
        combined = (uint8_t)(combined | mac[index]);
    }
    return combined != 0U;
}

static bool binding_is_exact(const backend_ota_journal_record_t *record)
{
    if (!hardware_mac_is_valid(record->uplink_mac) ||
        !hardware_mac_is_valid(record->expected_target_mac) ||
        !hardware_mac_is_valid(record->actual_target_mac) ||
        memcmp(record->expected_target_mac, record->actual_target_mac,
               sizeof(record->expected_target_mac)) != 0 ||
        record->expected_target_boot_id != record->actual_target_boot_id ||
        record->expected_topology_generation !=
            record->actual_topology_generation) {
        return false;
    }
    if (record->component == BACKEND_OTA_COMPONENT_UPLINK &&
        (memcmp(record->uplink_mac, record->expected_target_mac,
                sizeof(record->uplink_mac)) != 0 ||
         record->uplink_boot_id != record->expected_target_boot_id)) {
        return false;
    }
    if (record->component != BACKEND_OTA_COMPONENT_UPLINK &&
        memcmp(record->uplink_mac, record->expected_target_mac,
               sizeof(record->uplink_mac)) == 0) {
        return false;
    }
    return true;
}

static bool phase_evidence_is_valid(
    const backend_ota_journal_record_t *record)
{
    if (record->image_writes_after < record->image_writes_before ||
        (record->converged && !record->rollback_clear)) {
        return false;
    }
    switch (record->phase) {
    case BACKEND_OTA_PHASE_ACCEPTED:
    case BACKEND_OTA_PHASE_WRITING:
        return record->image_writes_after == record->image_writes_before &&
               record->boot_id_after == 0U && !record->rollback_clear &&
               !record->converged;
    case BACKEND_OTA_PHASE_REBOOT_PENDING:
        return record->image_writes_after > record->image_writes_before &&
               record->boot_id_after == 0U && !record->rollback_clear &&
               !record->converged;
    case BACKEND_OTA_PHASE_CONVERGENCE_PENDING:
        return record->image_writes_after > record->image_writes_before &&
               record->boot_id_after != 0U &&
               record->boot_id_after != record->actual_target_boot_id &&
               !record->rollback_clear && !record->converged;
    case BACKEND_OTA_PHASE_COMPLETE:
        return record->image_writes_after > record->image_writes_before &&
               record->boot_id_after != 0U &&
               record->boot_id_after != record->actual_target_boot_id &&
               record->rollback_clear && record->converged;
    case BACKEND_OTA_PHASE_FAILED:
        return !record->converged;
    default:
        return false;
    }
}

backend_ota_journal_validation_t backend_ota_journal_validate(
    const backend_ota_journal_record_t *record)
{
    if (record == NULL) {
        return BACKEND_OTA_JOURNAL_INVALID_ARGUMENT;
    }
    if (record->schema != BACKEND_OTA_JOURNAL_SCHEMA ||
        record->operation_id == 0U ||
        record->component < BACKEND_OTA_COMPONENT_UPLINK ||
        record->component > BACKEND_OTA_COMPONENT_SCANNER1 ||
        record->apply_mode < BACKEND_OTA_NEWER_ONLY ||
        record->apply_mode > BACKEND_OTA_SAME_VERSION_RECOVERY ||
        record->phase < BACKEND_OTA_PHASE_ACCEPTED ||
        record->phase > BACKEND_OTA_PHASE_FAILED ||
        record->manifest.image_size == 0U ||
        record->manifest.generation == 0U ||
        record->uplink_boot_id == 0U ||
        record->expected_target_boot_id == 0U ||
        record->actual_target_boot_id == 0U ||
        record->expected_topology_generation == 0U ||
        record->actual_topology_generation == 0U) {
        return BACKEND_OTA_JOURNAL_INVALID_FIELD;
    }
    if ((record->component == BACKEND_OTA_COMPONENT_UPLINK &&
         record->component_slot != -1) ||
        (record->component == BACKEND_OTA_COMPONENT_SCANNER0 &&
         record->component_slot != 0) ||
        (record->component == BACKEND_OTA_COMPONENT_SCANNER1 &&
         record->component_slot != 1) ||
        !component_identity_is_exact(record) ||
        !fixed_string_is_canonical(
            record->manifest.version, sizeof(record->manifest.version), false) ||
        !sha256_is_lowercase(record->manifest.sha256) ||
        ((record->apply_mode == BACKEND_OTA_SAME_VERSION_RECOVERY) !=
         record->manifest.allow_same_version) ||
        !binding_is_exact(record) || !phase_evidence_is_valid(record)) {
        return BACKEND_OTA_JOURNAL_INVALID_FIELD;
    }
    return BACKEND_OTA_JOURNAL_VALID;
}

bool backend_ota_journal_encode(
    const backend_ota_journal_record_t *record,
    backend_ota_journal_blob_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (out == NULL ||
        backend_ota_journal_validate(record) != BACKEND_OTA_JOURNAL_VALID) {
        return false;
    }

    journal_writer_t writer = {
        .bytes = out->bytes,
        .capacity = sizeof(out->bytes),
        .valid = true,
    };
    writer_u32_le(&writer, record->schema);
    writer_u32_le(&writer, record->operation_id);
    writer_u32_le(&writer, (uint32_t)record->component);
    writer_u8(&writer, (uint8_t)record->component_slot);
    writer_u32_le(&writer, (uint32_t)record->apply_mode);
    writer_bytes(&writer, record->catalog_name, sizeof(record->catalog_name));
    writer_bytes(&writer, record->manifest.target,
                 sizeof(record->manifest.target));
    writer_bytes(&writer, record->manifest.project,
                 sizeof(record->manifest.project));
    writer_bytes(&writer, record->manifest.hardware,
                 sizeof(record->manifest.hardware));
    writer_bytes(&writer, record->manifest.version,
                 sizeof(record->manifest.version));
    writer_u32_le(&writer, record->manifest.image_size);
    writer_u32_le(&writer, record->manifest.crc32);
    writer_bytes(&writer, record->manifest.sha256,
                 sizeof(record->manifest.sha256));
    writer_u32_le(&writer, record->manifest.generation);
    writer_u8(&writer, record->manifest.allow_same_version ? 1U : 0U);
    writer_bytes(&writer, record->uplink_mac, sizeof(record->uplink_mac));
    writer_u32_le(&writer, record->uplink_boot_id);
    writer_bytes(&writer, record->expected_target_mac,
                 sizeof(record->expected_target_mac));
    writer_bytes(&writer, record->actual_target_mac,
                 sizeof(record->actual_target_mac));
    writer_u32_le(&writer, record->expected_target_boot_id);
    writer_u32_le(&writer, record->actual_target_boot_id);
    writer_u32_le(&writer, record->expected_topology_generation);
    writer_u32_le(&writer, record->actual_topology_generation);
    writer_u32_le(&writer, (uint32_t)record->phase);
    writer_u32_le(&writer, record->image_writes_before);
    writer_u32_le(&writer, record->image_writes_after);
    writer_u32_le(&writer, record->boot_id_after);
    writer_u8(&writer, record->rollback_clear ? 1U : 0U);
    writer_u8(&writer, record->converged ? 1U : 0U);
    if (!writer.valid || writer.position != BACKEND_OTA_JOURNAL_CRC_OFFSET) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    writer_u32_le(&writer, journal_crc32(out->bytes, writer.position));
    if (!writer.valid || writer.position != BACKEND_OTA_JOURNAL_CANONICAL_SIZE) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    out->length = writer.position;
    return true;
}

backend_ota_journal_validation_t backend_ota_journal_decode(
    const uint8_t *bytes,
    size_t length,
    backend_ota_journal_record_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (bytes == NULL || out == NULL) {
        return BACKEND_OTA_JOURNAL_INVALID_ARGUMENT;
    }
    if (length != BACKEND_OTA_JOURNAL_CANONICAL_SIZE) {
        return BACKEND_OTA_JOURNAL_INVALID_LENGTH;
    }

    journal_reader_t crc_reader = {
        .bytes = bytes,
        .length = length,
        .position = BACKEND_OTA_JOURNAL_CRC_OFFSET,
        .valid = true,
    };
    const uint32_t stored_crc = reader_u32_le(&crc_reader);
    if (!crc_reader.valid || crc_reader.position != length ||
        stored_crc != journal_crc32(bytes, BACKEND_OTA_JOURNAL_CRC_OFFSET)) {
        return BACKEND_OTA_JOURNAL_INVALID_CRC;
    }

    journal_reader_t reader = {
        .bytes = bytes,
        .length = BACKEND_OTA_JOURNAL_CRC_OFFSET,
        .valid = true,
    };
    backend_ota_journal_record_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    decoded.schema = reader_u32_le(&reader);
    decoded.operation_id = reader_u32_le(&reader);
    const uint32_t component = reader_u32_le(&reader);
    const uint8_t slot = reader_u8(&reader);
    const uint32_t apply_mode = reader_u32_le(&reader);
    reader_bytes(&reader, decoded.catalog_name, sizeof(decoded.catalog_name));
    reader_bytes(&reader, decoded.manifest.target,
                 sizeof(decoded.manifest.target));
    reader_bytes(&reader, decoded.manifest.project,
                 sizeof(decoded.manifest.project));
    reader_bytes(&reader, decoded.manifest.hardware,
                 sizeof(decoded.manifest.hardware));
    reader_bytes(&reader, decoded.manifest.version,
                 sizeof(decoded.manifest.version));
    decoded.manifest.image_size = reader_u32_le(&reader);
    decoded.manifest.crc32 = reader_u32_le(&reader);
    reader_bytes(&reader, decoded.manifest.sha256,
                 sizeof(decoded.manifest.sha256));
    decoded.manifest.generation = reader_u32_le(&reader);
    const uint8_t allow_same_version = reader_u8(&reader);
    reader_bytes(&reader, decoded.uplink_mac, sizeof(decoded.uplink_mac));
    decoded.uplink_boot_id = reader_u32_le(&reader);
    reader_bytes(&reader, decoded.expected_target_mac,
                 sizeof(decoded.expected_target_mac));
    reader_bytes(&reader, decoded.actual_target_mac,
                 sizeof(decoded.actual_target_mac));
    decoded.expected_target_boot_id = reader_u32_le(&reader);
    decoded.actual_target_boot_id = reader_u32_le(&reader);
    decoded.expected_topology_generation = reader_u32_le(&reader);
    decoded.actual_topology_generation = reader_u32_le(&reader);
    const uint32_t phase = reader_u32_le(&reader);
    decoded.image_writes_before = reader_u32_le(&reader);
    decoded.image_writes_after = reader_u32_le(&reader);
    decoded.boot_id_after = reader_u32_le(&reader);
    const uint8_t rollback_clear = reader_u8(&reader);
    const uint8_t converged = reader_u8(&reader);

    if (!reader.valid || reader.position != reader.length ||
        component > (uint32_t)BACKEND_OTA_COMPONENT_SCANNER1 ||
        (slot != UINT8_C(0xFF) && slot > UINT8_C(1)) ||
        apply_mode > (uint32_t)BACKEND_OTA_SAME_VERSION_RECOVERY ||
        phase > (uint32_t)BACKEND_OTA_PHASE_FAILED ||
        allow_same_version > 1U || rollback_clear > 1U || converged > 1U) {
        return BACKEND_OTA_JOURNAL_INVALID_FIELD;
    }
    decoded.component = (backend_ota_component_t)component;
    decoded.component_slot = slot == UINT8_C(0xFF) ? -1 : (int8_t)slot;
    decoded.apply_mode = (backend_ota_apply_mode_t)apply_mode;
    decoded.manifest.allow_same_version = allow_same_version != 0U;
    decoded.phase = (backend_ota_phase_t)phase;
    decoded.rollback_clear = rollback_clear != 0U;
    decoded.converged = converged != 0U;
    decoded.record_crc32 = stored_crc;

    const backend_ota_journal_validation_t validation =
        backend_ota_journal_validate(&decoded);
    if (validation != BACKEND_OTA_JOURNAL_VALID) {
        return validation;
    }
    *out = decoded;
    return BACKEND_OTA_JOURNAL_VALID;
}

backend_ota_journal_load_result_t backend_ota_journal_load(
    const backend_ota_journal_storage_t *storage,
    backend_ota_journal_record_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (storage == NULL || storage->load == NULL || out == NULL) {
        return BACKEND_OTA_JOURNAL_LOAD_IO_ERROR;
    }
    uint8_t bytes[BACKEND_OTA_JOURNAL_CANONICAL_SIZE + 1U];
    size_t length = 0U;
    const backend_ota_journal_io_result_t result = storage->load(
        storage->context, bytes, sizeof(bytes), &length);
    if (result == BACKEND_OTA_JOURNAL_IO_NOT_FOUND) {
        return BACKEND_OTA_JOURNAL_LOAD_NOT_FOUND;
    }
    if (result != BACKEND_OTA_JOURNAL_IO_OK || length > sizeof(bytes)) {
        return BACKEND_OTA_JOURNAL_LOAD_IO_ERROR;
    }
    backend_ota_journal_record_t decoded;
    const backend_ota_journal_validation_t validation =
        backend_ota_journal_decode(bytes, length, &decoded);
    if (validation != BACKEND_OTA_JOURNAL_VALID) {
        return BACKEND_OTA_JOURNAL_LOAD_CORRUPT;
    }
    *out = decoded;
    return BACKEND_OTA_JOURNAL_LOAD_PRESENT;
}

backend_ota_journal_persist_result_t backend_ota_journal_persist_accepted(
    const backend_ota_journal_storage_t *storage,
    const backend_ota_journal_record_t *accepted)
{
    if (storage == NULL || storage->load == NULL || storage->store == NULL) {
        return BACKEND_OTA_JOURNAL_PERSIST_IO_ERROR;
    }
    if (accepted == NULL || accepted->phase != BACKEND_OTA_PHASE_ACCEPTED ||
        backend_ota_journal_validate(accepted) != BACKEND_OTA_JOURNAL_VALID) {
        return BACKEND_OTA_JOURNAL_PERSIST_INVALID;
    }

    backend_ota_journal_record_t current;
    const backend_ota_journal_load_result_t load_result =
        backend_ota_journal_load(storage, &current);
    if (load_result == BACKEND_OTA_JOURNAL_LOAD_IO_ERROR) {
        return BACKEND_OTA_JOURNAL_PERSIST_IO_ERROR;
    }
    if (load_result == BACKEND_OTA_JOURNAL_LOAD_CORRUPT) {
        return BACKEND_OTA_JOURNAL_PERSIST_CONFLICT;
    }

    backend_ota_journal_blob_t accepted_blob;
    if (!backend_ota_journal_encode(accepted, &accepted_blob)) {
        return BACKEND_OTA_JOURNAL_PERSIST_INVALID;
    }
    if (load_result == BACKEND_OTA_JOURNAL_LOAD_PRESENT) {
        backend_ota_journal_blob_t current_blob;
        if (!backend_ota_journal_encode(&current, &current_blob)) {
            return BACKEND_OTA_JOURNAL_PERSIST_CONFLICT;
        }
        if (current.phase == BACKEND_OTA_PHASE_ACCEPTED &&
            current_blob.length == accepted_blob.length &&
            memcmp(current_blob.bytes, accepted_blob.bytes,
                   accepted_blob.length) == 0) {
            return BACKEND_OTA_JOURNAL_PERSIST_ALREADY_DURABLE;
        }
        const bool terminal = current.phase == BACKEND_OTA_PHASE_COMPLETE ||
                              current.phase == BACKEND_OTA_PHASE_FAILED;
        if (!terminal || current.operation_id == accepted->operation_id) {
            return BACKEND_OTA_JOURNAL_PERSIST_CONFLICT;
        }
    }

    return storage->store(
               storage->context, accepted_blob.bytes, accepted_blob.length)
        ? BACKEND_OTA_JOURNAL_PERSIST_COMMITTED
        : BACKEND_OTA_JOURNAL_PERSIST_IO_ERROR;
}

backend_ota_journal_persist_result_t backend_ota_journal_persist_transition(
    const backend_ota_journal_storage_t *storage,
    const backend_ota_journal_record_t *next,
    uint32_t live_uplink_boot_id)
{
    if (storage == NULL || storage->load == NULL || storage->store == NULL) {
        return BACKEND_OTA_JOURNAL_PERSIST_IO_ERROR;
    }
    if (next == NULL || live_uplink_boot_id == 0U ||
        backend_ota_journal_validate(next) != BACKEND_OTA_JOURNAL_VALID) {
        return BACKEND_OTA_JOURNAL_PERSIST_INVALID;
    }

    backend_ota_journal_record_t current;
    const backend_ota_journal_load_result_t load_result =
        backend_ota_journal_load(storage, &current);
    if (load_result == BACKEND_OTA_JOURNAL_LOAD_IO_ERROR) {
        return BACKEND_OTA_JOURNAL_PERSIST_IO_ERROR;
    }
    if (load_result != BACKEND_OTA_JOURNAL_LOAD_PRESENT) {
        return BACKEND_OTA_JOURNAL_PERSIST_CONFLICT;
    }

    backend_ota_journal_blob_t current_blob;
    backend_ota_journal_blob_t next_blob;
    if (!backend_ota_journal_encode(&current, &current_blob) ||
        !backend_ota_journal_encode(next, &next_blob)) {
        return BACKEND_OTA_JOURNAL_PERSIST_INVALID;
    }
    if (memcmp(current_blob.bytes, next_blob.bytes,
               BACKEND_OTA_JOURNAL_CANONICAL_SIZE) == 0) {
        return BACKEND_OTA_JOURNAL_PERSIST_ALREADY_DURABLE;
    }

    if (memcmp(current_blob.bytes, next_blob.bytes,
               BACKEND_OTA_JOURNAL_PHASE_OFFSET) != 0 ||
        memcmp(current_blob.bytes + BACKEND_OTA_JOURNAL_WRITES_BEFORE_OFFSET,
               next_blob.bytes + BACKEND_OTA_JOURNAL_WRITES_BEFORE_OFFSET,
               sizeof(uint32_t)) != 0 ||
        next->image_writes_after < current.image_writes_after ||
        (current.boot_id_after != 0U &&
         next->boot_id_after != current.boot_id_after) ||
        (current.rollback_clear && !next->rollback_clear) ||
        (current.converged && !next->converged)) {
        return BACKEND_OTA_JOURNAL_PERSIST_CONFLICT;
    }

    bool allowed = false;
    switch (current.phase) {
    case BACKEND_OTA_PHASE_ACCEPTED:
        allowed = next->phase == BACKEND_OTA_PHASE_WRITING ||
                  next->phase == BACKEND_OTA_PHASE_FAILED;
        break;
    case BACKEND_OTA_PHASE_WRITING:
        allowed = next->phase == BACKEND_OTA_PHASE_REBOOT_PENDING ||
                  next->phase == BACKEND_OTA_PHASE_FAILED;
        break;
    case BACKEND_OTA_PHASE_REBOOT_PENDING:
        allowed = next->phase == BACKEND_OTA_PHASE_CONVERGENCE_PENDING ||
                  next->phase == BACKEND_OTA_PHASE_FAILED;
        break;
    case BACKEND_OTA_PHASE_CONVERGENCE_PENDING:
        allowed = next->phase == BACKEND_OTA_PHASE_COMPLETE ||
                  next->phase == BACKEND_OTA_PHASE_FAILED;
        break;
    case BACKEND_OTA_PHASE_COMPLETE:
    case BACKEND_OTA_PHASE_FAILED:
    default:
        allowed = false;
        break;
    }
    if (!allowed ||
        (current.phase == BACKEND_OTA_PHASE_ACCEPTED &&
         next->phase == BACKEND_OTA_PHASE_WRITING &&
         live_uplink_boot_id != current.uplink_boot_id) ||
        (current.phase == BACKEND_OTA_PHASE_ACCEPTED &&
         next->phase == BACKEND_OTA_PHASE_FAILED &&
         (next->image_writes_after != current.image_writes_after ||
          next->boot_id_after != current.boot_id_after ||
          next->rollback_clear != current.rollback_clear ||
          next->converged != current.converged))) {
        return BACKEND_OTA_JOURNAL_PERSIST_CONFLICT;
    }
    if (!storage->store(
            storage->context, next_blob.bytes, next_blob.length)) {
        return BACKEND_OTA_JOURNAL_PERSIST_IO_ERROR;
    }
    return current.phase == BACKEND_OTA_PHASE_ACCEPTED &&
                   next->phase == BACKEND_OTA_PHASE_WRITING
        ? BACKEND_OTA_JOURNAL_PERSIST_MUTATION_AUTHORIZED
        : BACKEND_OTA_JOURNAL_PERSIST_COMMITTED;
}

backend_ota_journal_recovery_action_t backend_ota_journal_recovery_action(
    const backend_ota_journal_record_t *record)
{
    if (backend_ota_journal_validate(record) != BACKEND_OTA_JOURNAL_VALID) {
        return BACKEND_OTA_JOURNAL_RECOVERY_INVALID;
    }
    switch (record->phase) {
    case BACKEND_OTA_PHASE_ACCEPTED:
        return BACKEND_OTA_JOURNAL_RECOVERY_FAIL_BEFORE_MUTATION;
    case BACKEND_OTA_PHASE_WRITING:
        return BACKEND_OTA_JOURNAL_RECOVERY_POST_WRITE_CHECKS;
    case BACKEND_OTA_PHASE_REBOOT_PENDING:
        return BACKEND_OTA_JOURNAL_RECOVERY_REBOOT_CHECKS;
    case BACKEND_OTA_PHASE_CONVERGENCE_PENDING:
        return BACKEND_OTA_JOURNAL_RECOVERY_CONVERGENCE_CHECKS;
    case BACKEND_OTA_PHASE_COMPLETE:
        return BACKEND_OTA_JOURNAL_RECOVERY_COMPLETE;
    case BACKEND_OTA_PHASE_FAILED:
        return BACKEND_OTA_JOURNAL_RECOVERY_FAILED;
    default:
        return BACKEND_OTA_JOURNAL_RECOVERY_INVALID;
    }
}
