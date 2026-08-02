#include "backend_ota_maintenance.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "backend_identity.h"
#include "backend_json_writer.h"
#include "firmware_version_order.h"

#define BACKEND_OTA_USB_MAX_LINE 255U

static int64_t deadline_after(int64_t now_ms, uint32_t delay_ms)
{
    if (now_ms > INT64_MAX - (int64_t)delay_ms) {
        return INT64_MAX;
    }
    return now_ms + (int64_t)delay_ms;
}

void backend_ota_poll_init(backend_ota_poll_state_t *state, int64_t now_ms)
{
    if (state == NULL) {
        return;
    }
    state->next_due_ms = now_ms;
    state->failure_count = 0U;
    state->initialized = true;
}

bool backend_ota_poll_due(
    const backend_ota_poll_state_t *state, int64_t now_ms)
{
    return state != NULL && state->initialized &&
           now_ms >= state->next_due_ms;
}

void backend_ota_poll_note_failure(
    backend_ota_poll_state_t *state, int64_t now_ms)
{
    static const uint32_t backoff_ms[] = {
        5000U, 10000U, 20000U, 40000U,
        80000U, 160000U, 300000U,
    };
    if (state == NULL || !state->initialized) {
        return;
    }
    size_t index = state->failure_count;
    if (index >= sizeof(backoff_ms) / sizeof(backoff_ms[0])) {
        index = sizeof(backoff_ms) / sizeof(backoff_ms[0]) - 1U;
    }
    state->next_due_ms = deadline_after(now_ms, backoff_ms[index]);
    if (state->failure_count < UINT8_MAX) {
        state->failure_count++;
    }
}

void backend_ota_poll_note_success(
    backend_ota_poll_state_t *state, int64_t now_ms)
{
    if (state == NULL || !state->initialized) {
        return;
    }
    state->failure_count = 0U;
    state->next_due_ms = deadline_after(
        now_ms, (uint32_t)BACKEND_OTA_POLL_INTERVAL_MS);
}

backend_ota_auto_decision_t backend_ota_auto_policy(
    bool auto_update_enabled, fof_firmware_version_relation_t relation)
{
    switch (relation) {
    case FOF_VERSION_NEWER:
        return auto_update_enabled
            ? BACKEND_OTA_AUTO_APPLY_NEWER
            : BACKEND_OTA_AUTO_READ_ONLY_UPDATE_AVAILABLE;
    case FOF_VERSION_EQUAL:
    case FOF_VERSION_OLDER:
        return BACKEND_OTA_AUTO_NO_UPDATE;
    case FOF_VERSION_UNORDERED:
    case FOF_VERSION_INVALID:
    default:
        return BACKEND_OTA_AUTO_REJECT_VERSION;
    }
}

static bool bounded_string(const char *value, size_t capacity)
{
    return value != NULL && memchr(value, '\0', capacity) != NULL;
}

static bool lowercase_sha256(const char *value)
{
    if (value == NULL) {
        return false;
    }
    for (size_t index = 0U; index < 64U; ++index) {
        const char character = value[index];
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return value[64] == '\0';
}

static bool uppercase_mac_text(const char *value, uint8_t output[6])
{
    if (value == NULL || strlen(value) != 17U) {
        return false;
    }
    for (size_t index = 0U; index < 6U; ++index) {
        const size_t offset = index * 3U;
        unsigned byte = 0U;
        for (size_t digit = 0U; digit < 2U; ++digit) {
            const char character = value[offset + digit];
            unsigned nibble;
            if (character >= '0' && character <= '9') {
                nibble = (unsigned)(character - '0');
            } else if (character >= 'A' && character <= 'F') {
                nibble = (unsigned)(character - 'A') + 10U;
            } else {
                return false;
            }
            byte = byte * 16U + nibble;
        }
        if (index < 5U && value[offset + 2U] != ':') {
            return false;
        }
        output[index] = (uint8_t)byte;
    }
    return true;
}

static bool parse_nonzero_u32(const char *value, uint32_t *out)
{
    if (value == NULL || out == NULL || value[0] == '\0') {
        return false;
    }
    uint32_t parsed = 0U;
    for (const char *cursor = value; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        const uint32_t digit = (uint32_t)(*cursor - '0');
        if (parsed > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    if (parsed == 0U) {
        return false;
    }
    *out = parsed;
    return true;
}

static bool parse_component(
    const char *value, backend_ota_component_t *component)
{
    if (value == NULL || component == NULL) {
        return false;
    }
    if (strcmp(value, "uplink") == 0) {
        *component = BACKEND_OTA_COMPONENT_UPLINK;
    } else if (strcmp(value, "scanner0") == 0) {
        *component = BACKEND_OTA_COMPONENT_SCANNER0;
    } else if (strcmp(value, "scanner1") == 0) {
        *component = BACKEND_OTA_COMPONENT_SCANNER1;
    } else {
        return false;
    }
    return true;
}

const char *backend_ota_component_catalog_name(
    backend_ota_component_t component)
{
    switch (component) {
    case BACKEND_OTA_COMPONENT_UPLINK:
        return FOF_BACKEND_UPLINK_TARGET;
    case BACKEND_OTA_COMPONENT_SCANNER0:
    case BACKEND_OTA_COMPONENT_SCANNER1:
        return FOF_BACKEND_SCANNER_TARGET;
    default:
        return NULL;
    }
}

int8_t backend_ota_component_slot(backend_ota_component_t component)
{
    switch (component) {
    case BACKEND_OTA_COMPONENT_UPLINK:
        return -1;
    case BACKEND_OTA_COMPONENT_SCANNER0:
        return 0;
    case BACKEND_OTA_COMPONENT_SCANNER1:
        return 1;
    default:
        return -2;
    }
}

static bool copy_line(
    const char *line, size_t length, char output[BACKEND_OTA_USB_MAX_LINE + 1U])
{
    if (line == NULL || length == 0U || length > BACKEND_OTA_USB_MAX_LINE ||
        memchr(line, '\0', length) != NULL) {
        return false;
    }
    if (line[length - 1U] == '\n') {
        --length;
        if (length > 0U && line[length - 1U] == '\r') {
            --length;
        }
    }
    if (length == 0U || line[0] == ' ' || line[length - 1U] == ' ') {
        return false;
    }
    memcpy(output, line, length);
    output[length] = '\0';
    return true;
}

static size_t split_exact(char *line, char *tokens[], size_t capacity)
{
    size_t count = 0U;
    char *cursor = line;
    while (*cursor != '\0') {
        if (count == capacity || *cursor == ' ') {
            return 0U;
        }
        tokens[count++] = cursor;
        char *space = strchr(cursor, ' ');
        if (space == NULL) {
            break;
        }
        *space = '\0';
        cursor = space + 1U;
        if (*cursor == '\0' || *cursor == ' ') {
            return 0U;
        }
    }
    return count;
}

bool backend_ota_maintenance_parse_usb(
    const char *line, size_t length, backend_ota_request_t *out)
{
    char copy[BACKEND_OTA_USB_MAX_LINE + 1U];
    char *tokens[8];
    backend_ota_request_t request;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (out == NULL || !copy_line(line, length, copy)) {
        return false;
    }
    const size_t token_count = split_exact(
        copy, tokens, sizeof(tokens) / sizeof(tokens[0]));
    memset(&request, 0, sizeof(request));

    if (token_count == 4U &&
        strcmp(tokens[0], "FOF_BACKEND_OTA_PROBE") == 0) {
        request.probe = true;
        if (!parse_component(tokens[1], &request.component)) {
            return false;
        }
        const char *catalog = backend_ota_component_catalog_name(
            request.component);
        if (catalog == NULL || strcmp(tokens[2], catalog) != 0 ||
            (strcmp(tokens[3], "*") != 0 &&
             !lowercase_sha256(tokens[3]))) {
            return false;
        }
        memcpy(request.catalog_name, catalog, strlen(catalog) + 1U);
        if (tokens[3][0] != '*') {
            memcpy(request.expected_sha256, tokens[3], 65U);
        }
        request.apply_mode = BACKEND_OTA_NEWER_ONLY;
        *out = request;
        return true;
    }

    if (token_count == 7U &&
        strcmp(tokens[0], "FOF_BACKEND_OTA_APPLY") == 0) {
        if (!parse_component(tokens[1], &request.component) ||
            !lowercase_sha256(tokens[2]) ||
            !uppercase_mac_text(tokens[4], request.expected_mac) ||
            !parse_nonzero_u32(tokens[5], &request.expected_boot_id) ||
            !parse_nonzero_u32(
                tokens[6], &request.expected_topology_generation)) {
            return false;
        }
        if (strcmp(tokens[3], "newer-only") == 0) {
            request.apply_mode = BACKEND_OTA_NEWER_ONLY;
        } else if (strcmp(tokens[3], "same-version-recovery") == 0) {
            request.apply_mode = BACKEND_OTA_SAME_VERSION_RECOVERY;
        } else {
            return false;
        }
        const char *catalog = backend_ota_component_catalog_name(
            request.component);
        if (catalog == NULL) {
            return false;
        }
        memcpy(request.catalog_name, catalog, strlen(catalog) + 1U);
        memcpy(request.expected_sha256, tokens[2], 65U);
        *out = request;
        return true;
    }
    return false;
}

bool backend_ota_maintenance_is_status_usb(
    const char *line, size_t length)
{
    char copy[BACKEND_OTA_USB_MAX_LINE + 1U];
    return copy_line(line, length, copy) &&
           strcmp(copy, "FOF_BACKEND_OTA_STATUS") == 0;
}

bool backend_ota_target_binding_matches(
    const backend_ota_request_t *request,
    const backend_ota_target_binding_t *actual)
{
    if (request == NULL || actual == NULL || request->probe ||
        backend_ota_component_slot(request->component) < -1 ||
        request->expected_boot_id == 0U ||
        request->expected_topology_generation == 0U) {
        return false;
    }
    return actual->component == request->component &&
           actual->component_slot ==
               backend_ota_component_slot(request->component) &&
           memcmp(actual->target_mac, request->expected_mac, 6U) == 0 &&
           actual->target_boot_id == request->expected_boot_id &&
           actual->topology_generation ==
               request->expected_topology_generation;
}

static const char *component_name(backend_ota_component_t component)
{
    switch (component) {
    case BACKEND_OTA_COMPONENT_UPLINK: return "uplink";
    case BACKEND_OTA_COMPONENT_SCANNER0: return "scanner0";
    case BACKEND_OTA_COMPONENT_SCANNER1: return "scanner1";
    default: return NULL;
    }
}

static const char *decision_name(backend_ota_decision_t decision)
{
    switch (decision) {
    case BACKEND_OTA_DECISION_ADMIT: return "admit";
    case BACKEND_OTA_DECISION_NO_UPDATE: return "no_update";
    case BACKEND_OTA_DECISION_REJECT_IDENTITY: return "reject_identity";
    case BACKEND_OTA_DECISION_REJECT_VERSION: return "reject_version";
    case BACKEND_OTA_DECISION_REJECT_DIGEST: return "reject_digest";
    case BACKEND_OTA_DECISION_REJECT_SIZE: return "reject_size";
    case BACKEND_OTA_DECISION_REJECT_CAPACITY: return "reject_capacity";
    case BACKEND_OTA_DECISION_REJECT_BUSY: return "reject_busy";
    case BACKEND_OTA_DECISION_REJECT_TARGET_BINDING:
        return "reject_target_binding";
    case BACKEND_OTA_DECISION_APPLIED: return "applied";
    case BACKEND_OTA_DECISION_FAILED: return "failed";
    default: return NULL;
    }
}

static void format_mac(const uint8_t mac[6], char output[18])
{
    (void)snprintf(output, 18U,
                   "%02X:%02X:%02X:%02X:%02X:%02X",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool evidence_strings_are_bounded(
    const backend_ota_evidence_t *evidence)
{
    return bounded_string(
               evidence->catalog_name, sizeof(evidence->catalog_name)) &&
           bounded_string(
               evidence->manifest.target, sizeof(evidence->manifest.target)) &&
           bounded_string(
               evidence->manifest.project,
               sizeof(evidence->manifest.project)) &&
           bounded_string(
               evidence->manifest.hardware,
               sizeof(evidence->manifest.hardware)) &&
           bounded_string(
               evidence->manifest.version,
               sizeof(evidence->manifest.version)) &&
           bounded_string(
               evidence->manifest.sha256,
               sizeof(evidence->manifest.sha256));
}

static void append_string_field(
    backend_json_writer_t *writer, const char *key, const char *value)
{
    backend_json_append_format(writer, ",\"%s\":", key);
    backend_json_append_escaped(writer, value);
}

static void append_bool_field(
    backend_json_writer_t *writer, const char *key, bool value)
{
    backend_json_append_format(
        writer, ",\"%s\":%s", key, value ? "true" : "false");
}

static void append_u32_field(
    backend_json_writer_t *writer, const char *key, uint32_t value)
{
    backend_json_append_format(
        writer, ",\"%s\":%" PRIu32, key, value);
}

size_t backend_ota_evidence_encode(
    const backend_ota_evidence_t *evidence, char *output, size_t capacity)
{
    if (output != NULL && capacity > 0U) {
        output[0] = '\0';
    }
    const char *component = evidence == NULL
        ? NULL : component_name(evidence->component);
    const char *decision = evidence == NULL
        ? NULL : decision_name(evidence->decision);
    if (evidence == NULL || output == NULL || capacity == 0U ||
        evidence->operation_id == 0U || component == NULL || decision == NULL ||
        backend_ota_component_slot(evidence->component) !=
            evidence->component_slot ||
        !evidence_strings_are_bounded(evidence)) {
        return 0U;
    }

    char uplink_mac[18];
    char expected_mac[18];
    char actual_mac[18];
    format_mac(evidence->uplink_mac, uplink_mac);
    format_mac(evidence->expected_target_mac, expected_mac);
    format_mac(evidence->actual_target_mac, actual_mac);
    if (!evidence->probe &&
        (evidence->apply_mode < BACKEND_OTA_NEWER_ONLY ||
         evidence->apply_mode > BACKEND_OTA_SAME_VERSION_RECOVERY)) {
        return 0U;
    }
    const char *mode = evidence->probe
        ? "probe"
        : (evidence->apply_mode == BACKEND_OTA_SAME_VERSION_RECOVERY
               ? "same-version-recovery" : "newer-only");

    backend_json_writer_t writer;
    backend_json_writer_init(&writer, output, capacity);
    backend_json_append_format(
        &writer,
        "FOF_BACKEND_OTA_EVIDENCE {\"schema\":1,\"operation_id\":%" PRIu32,
        evidence->operation_id);
    append_string_field(&writer, "mode", mode);
    append_string_field(&writer, "component", component);
    backend_json_append_format(
        &writer, ",\"component_slot\":%d", (int)evidence->component_slot);
    append_string_field(&writer, "uplink_mac", uplink_mac);
    append_string_field(&writer, "expected_target_mac", expected_mac);
    append_string_field(&writer, "actual_target_mac", actual_mac);
    append_u32_field(
        &writer, "expected_target_boot_id",
        evidence->expected_target_boot_id);
    append_u32_field(
        &writer, "actual_target_boot_id", evidence->actual_target_boot_id);
    append_u32_field(
        &writer, "expected_topology_generation",
        evidence->expected_topology_generation);
    append_u32_field(
        &writer, "actual_topology_generation",
        evidence->actual_topology_generation);
    append_string_field(&writer, "catalog_name", evidence->catalog_name);
    append_string_field(&writer, "target", evidence->manifest.target);
    append_string_field(&writer, "project", evidence->manifest.project);
    append_string_field(&writer, "hardware", evidence->manifest.hardware);
    append_string_field(&writer, "version", evidence->manifest.version);
    append_string_field(&writer, "sha256", evidence->manifest.sha256);
    append_u32_field(&writer, "crc32", evidence->manifest.crc32);
    append_u32_field(&writer, "size", evidence->manifest.image_size);
    backend_json_append_format(
        &writer, ",\"partition_capacity\":%zu", evidence->partition_capacity);
    append_bool_field(
        &writer, "allow_same_version", evidence->manifest.allow_same_version);
    append_string_field(&writer, "decision", decision);
    append_bool_field(
        &writer, "complete_image_validated",
        evidence->complete_image_validated);
    append_u32_field(
        &writer, "image_writes_before", evidence->image_writes_before);
    append_u32_field(
        &writer, "image_writes_after", evidence->image_writes_after);
    append_u32_field(&writer, "boot_id_before", evidence->boot_id_before);
    append_u32_field(&writer, "boot_id_after", evidence->boot_id_after);
    append_bool_field(&writer, "rollback_clear", evidence->rollback_clear);
    append_bool_field(&writer, "converged", evidence->converged);
    backend_json_append(&writer, "}");
    return backend_json_writer_finish(&writer);
}

size_t backend_ota_accepted_encode(
    const backend_ota_journal_record_t *accepted,
    char *output,
    size_t capacity)
{
    if (output != NULL && capacity > 0U) {
        output[0] = '\0';
    }
    const char *component = accepted == NULL
        ? NULL : component_name(accepted->component);
    if (accepted == NULL || output == NULL || capacity == 0U ||
        component == NULL || accepted->phase != BACKEND_OTA_PHASE_ACCEPTED ||
        backend_ota_journal_validate(accepted) !=
            BACKEND_OTA_JOURNAL_VALID ||
        memcmp(accepted->expected_target_mac,
               accepted->actual_target_mac, 6U) != 0 ||
        accepted->expected_target_boot_id !=
            accepted->actual_target_boot_id ||
        accepted->expected_topology_generation !=
            accepted->actual_topology_generation ||
        accepted->actual_target_boot_id == 0U ||
        accepted->actual_target_boot_id !=
            accepted->expected_target_boot_id) {
        return 0U;
    }

    char uplink_mac[18];
    char expected_mac[18];
    char actual_mac[18];
    format_mac(accepted->uplink_mac, uplink_mac);
    format_mac(accepted->expected_target_mac, expected_mac);
    format_mac(accepted->actual_target_mac, actual_mac);

    backend_json_writer_t writer;
    backend_json_writer_init(&writer, output, capacity);
    backend_json_append_format(
        &writer,
        "FOF_BACKEND_OTA_ACCEPTED {\"schema\":1,\"operation_id\":%" PRIu32,
        accepted->operation_id);
    append_string_field(&writer, "component", component);
    backend_json_append_format(
        &writer, ",\"component_slot\":%d", (int)accepted->component_slot);
    append_string_field(&writer, "sha256", accepted->manifest.sha256);
    append_u32_field(&writer, "crc32", accepted->manifest.crc32);
    append_string_field(&writer, "uplink_mac", uplink_mac);
    append_string_field(&writer, "expected_target_mac", expected_mac);
    append_string_field(&writer, "actual_target_mac", actual_mac);
    append_u32_field(
        &writer, "expected_target_boot_id",
        accepted->expected_target_boot_id);
    append_u32_field(
        &writer, "actual_target_boot_id", accepted->actual_target_boot_id);
    append_u32_field(
        &writer, "expected_topology_generation",
        accepted->expected_topology_generation);
    append_u32_field(
        &writer, "actual_topology_generation",
        accepted->actual_topology_generation);
    append_u32_field(
        &writer, "boot_id_before", accepted->actual_target_boot_id);
    backend_json_append(&writer, "}");
    return backend_json_writer_finish(&writer);
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

static bool maintenance_adapters_valid(
    const backend_ota_maintenance_adapters_t *adapters)
{
    return adapters != NULL && adapters->fetch_metadata != NULL &&
           adapters->download_image != NULL &&
           adapters->running_version != NULL &&
           adapters->partition_capacity != NULL &&
           adapters->image_write_count != NULL &&
           adapters->snapshot_binding != NULL &&
           adapters->acquire_target_claim != NULL &&
           adapters->release_target_claim != NULL &&
           adapters->scanner_dry_run != NULL &&
           adapters->mutate_staged_image != NULL &&
           adapters->request_reboot != NULL &&
           adapters->read_convergence != NULL &&
           adapters->emit_and_flush != NULL;
}

static uint32_t next_operation_id(backend_ota_maintenance_t *state)
{
    if (state->next_operation_id == UINT32_MAX) {
        state->busy = true;
        return 0U;
    }
    state->next_operation_id++;
    return state->next_operation_id;
}

bool backend_ota_maintenance_init(
    backend_ota_maintenance_t *state,
    const backend_ota_maintenance_adapters_t *adapters,
    const backend_ota_journal_storage_t *journal_storage,
    backend_firmware_buffer_t *firmware_buffer,
    const uint8_t uplink_mac[6],
    uint32_t uplink_boot_id)
{
    if (state == NULL || !maintenance_adapters_valid(adapters) ||
        journal_storage == NULL || journal_storage->load == NULL ||
        journal_storage->store == NULL || firmware_buffer == NULL ||
        !hardware_mac_is_valid(uplink_mac) || uplink_boot_id == 0U) {
        if (state != NULL) {
            memset(state, 0, sizeof(*state));
        }
        return false;
    }
    memset(state, 0, sizeof(*state));
    state->adapters = *adapters;
    state->journal_storage = *journal_storage;
    state->firmware_buffer = firmware_buffer;
    memcpy(state->uplink_mac, uplink_mac, 6U);
    state->uplink_boot_id = uplink_boot_id;
    state->initialized = true;

    backend_ota_journal_record_t existing;
    const backend_ota_journal_load_result_t load =
        backend_ota_journal_load(&state->journal_storage, &existing);
    if (load == BACKEND_OTA_JOURNAL_LOAD_PRESENT) {
        state->next_operation_id = existing.operation_id;
        state->busy = existing.phase != BACKEND_OTA_PHASE_COMPLETE &&
                      existing.phase != BACKEND_OTA_PHASE_FAILED;
        if (existing.operation_id == UINT32_MAX) {
            state->busy = true;
        }
    } else if (load == BACKEND_OTA_JOURNAL_LOAD_CORRUPT ||
               load == BACKEND_OTA_JOURNAL_LOAD_IO_ERROR) {
        state->busy = true;
    }
    return true;
}

void backend_ota_maintenance_on_boot(
    backend_ota_maintenance_t *state, uint32_t uplink_boot_id)
{
    if (state == NULL || !state->initialized || uplink_boot_id == 0U) {
        return;
    }
    state->uplink_boot_id = uplink_boot_id;
    backend_ota_journal_record_t record;
    const backend_ota_journal_load_result_t load =
        backend_ota_journal_load(&state->journal_storage, &record);
    state->busy = load != BACKEND_OTA_JOURNAL_LOAD_NOT_FOUND &&
                  !(load == BACKEND_OTA_JOURNAL_LOAD_PRESENT &&
                    (record.phase == BACKEND_OTA_PHASE_COMPLETE ||
                     record.phase == BACKEND_OTA_PHASE_FAILED));
    if (load == BACKEND_OTA_JOURNAL_LOAD_PRESENT &&
        record.operation_id > state->next_operation_id) {
        state->next_operation_id = record.operation_id;
    }
}

typedef struct {
    const uint8_t *bytes;
    size_t length;
} staged_reader_t;

static bool staged_read(
    void *context, size_t offset, uint8_t *output, size_t length)
{
    const staged_reader_t *reader = context;
    if (reader == NULL || output == NULL || length == 0U ||
        offset > reader->length || length > reader->length - offset) {
        return false;
    }
    memcpy(output, reader->bytes + offset, length);
    return true;
}

static backend_ota_image_result_t validate_staged(
    backend_ota_maintenance_t *state,
    const backend_ota_manifest_t *manifest,
    backend_image_kind_t kind,
    const uint8_t *bytes,
    size_t length)
{
    if (state->adapters.validate_staged_image != NULL) {
        return state->adapters.validate_staged_image(
            state->adapters.context, manifest, kind, bytes, length);
    }
    const staged_reader_t reader = {.bytes = bytes, .length = length};
    return backend_ota_image_validate(
        manifest, kind, staged_read, (void *)&reader);
}

static backend_image_kind_t component_image_kind(
    backend_ota_component_t component)
{
    return component == BACKEND_OTA_COMPONENT_UPLINK
        ? BACKEND_IMAGE_UPLINK : BACKEND_IMAGE_SCANNER;
}

static backend_ota_decision_t admission_decision(
    backend_ota_admission_result_t result)
{
    switch (result) {
    case BACKEND_OTA_ADMIT: return BACKEND_OTA_DECISION_ADMIT;
    case BACKEND_OTA_REJECT_IDENTITY:
        return BACKEND_OTA_DECISION_REJECT_IDENTITY;
    case BACKEND_OTA_REJECT_VERSION:
        return BACKEND_OTA_DECISION_REJECT_VERSION;
    case BACKEND_OTA_REJECT_DIGEST:
        return BACKEND_OTA_DECISION_REJECT_DIGEST;
    case BACKEND_OTA_REJECT_SIZE:
    case BACKEND_OTA_REJECT_GENERATION:
        return BACKEND_OTA_DECISION_REJECT_SIZE;
    case BACKEND_OTA_REJECT_CAPACITY:
        return BACKEND_OTA_DECISION_REJECT_CAPACITY;
    case BACKEND_OTA_REJECT_ARGUMENT:
    default:
        return BACKEND_OTA_DECISION_FAILED;
    }
}

static void evidence_begin(
    backend_ota_maintenance_t *state,
    backend_ota_component_t component,
    const char *catalog_name,
    bool probe,
    backend_ota_evidence_t *evidence)
{
    memset(evidence, 0, sizeof(*evidence));
    evidence->operation_id = next_operation_id(state);
    evidence->probe = probe;
    evidence->component = component;
    evidence->apply_mode = BACKEND_OTA_NEWER_ONLY;
    evidence->component_slot = backend_ota_component_slot(component);
    memcpy(evidence->uplink_mac, state->uplink_mac, 6U);
    if (catalog_name != NULL && strlen(catalog_name) <
            sizeof(evidence->catalog_name)) {
        memcpy(evidence->catalog_name, catalog_name,
               strlen(catalog_name) + 1U);
    }
    evidence->image_writes_before =
        state->adapters.image_write_count(state->adapters.context);
    evidence->image_writes_after = evidence->image_writes_before;
}

static bool emit_evidence(
    backend_ota_maintenance_t *state,
    const backend_ota_evidence_t *evidence)
{
    state->last_evidence = *evidence;
    state->has_last_evidence = true;
    const size_t length = backend_ota_evidence_encode(
        evidence, state->evidence_line, sizeof(state->evidence_line));
    return length != 0U && state->adapters.emit_and_flush(
        state->adapters.context, state->evidence_line, length);
}

static void release_buffer(
    backend_ota_maintenance_t *state, uint32_t operation_id)
{
    if (state->buffer_owned) {
        backend_firmware_buffer_release(
            state->firmware_buffer, operation_id);
        state->buffer_owned = false;
    }
}

static bool snapshot_to_evidence(
    backend_ota_maintenance_t *state,
    backend_ota_component_t component,
    backend_ota_target_binding_t *binding,
    backend_ota_evidence_t *evidence,
    bool copy_expected)
{
    if (!state->adapters.snapshot_binding(
            state->adapters.context, component, binding) ||
        binding->component != component ||
        binding->component_slot != backend_ota_component_slot(component) ||
        !hardware_mac_is_valid(binding->target_mac) ||
        binding->target_boot_id == 0U || binding->topology_generation == 0U) {
        return false;
    }
    if (copy_expected) {
        memcpy(evidence->expected_target_mac, binding->target_mac, 6U);
        evidence->expected_target_boot_id = binding->target_boot_id;
        evidence->expected_topology_generation =
            binding->topology_generation;
        evidence->boot_id_before = binding->target_boot_id;
    }
    memcpy(evidence->actual_target_mac, binding->target_mac, 6U);
    evidence->actual_target_boot_id = binding->target_boot_id;
    evidence->actual_topology_generation = binding->topology_generation;
    evidence->boot_id_after = binding->target_boot_id;
    return true;
}

static bool binding_is_same(
    const backend_ota_target_binding_t *left,
    const backend_ota_target_binding_t *right)
{
    return left != NULL && right != NULL &&
           left->component == right->component &&
           left->component_slot == right->component_slot &&
           memcmp(left->target_mac, right->target_mac, 6U) == 0 &&
           left->target_boot_id == right->target_boot_id &&
           left->topology_generation == right->topology_generation;
}

static bool stage_complete_image(
    backend_ota_maintenance_t *state,
    backend_ota_component_t component,
    const char *expected_sha256_or_null,
    bool allow_same_version,
    backend_ota_evidence_t *evidence)
{
    size_t metadata_length = 0U;
    uint32_t catalog_generation = 0U;
    memset(state->metadata, 0, sizeof(state->metadata));
    if (!state->adapters.fetch_metadata(
            state->adapters.context, evidence->catalog_name,
            state->metadata, BACKEND_OTA_MAINTENANCE_METADATA_CAPACITY,
            &metadata_length, &catalog_generation) ||
        metadata_length == 0U ||
        metadata_length > BACKEND_OTA_MAINTENANCE_METADATA_CAPACITY ||
        state->metadata[metadata_length] != '\0' ||
        !backend_ota_manifest_decode_metadata(
            state->metadata, metadata_length, catalog_generation,
            allow_same_version, &evidence->manifest)) {
        evidence->decision = BACKEND_OTA_DECISION_REJECT_IDENTITY;
        return false;
    }

    const char *running = state->adapters.running_version(
        state->adapters.context, component);
    evidence->partition_capacity = state->adapters.partition_capacity(
        state->adapters.context, component);
    if (running == NULL) {
        evidence->decision = BACKEND_OTA_DECISION_FAILED;
        return false;
    }
    const fof_firmware_version_relation_t relation =
        fof_firmware_version_compare(evidence->manifest.version, running);
    if (relation == FOF_VERSION_EQUAL && !allow_same_version) {
        evidence->decision = BACKEND_OTA_DECISION_NO_UPDATE;
        return false;
    }
    const backend_ota_admission_result_t admission =
        backend_ota_manifest_admit(
            &evidence->manifest, component_image_kind(component), running,
            evidence->partition_capacity);
    if (admission != BACKEND_OTA_ADMIT) {
        evidence->decision = admission_decision(admission);
        return false;
    }
    if (expected_sha256_or_null != NULL &&
        strcmp(expected_sha256_or_null, evidence->manifest.sha256) != 0) {
        evidence->decision = BACKEND_OTA_DECISION_REJECT_DIGEST;
        return false;
    }
    if (!backend_firmware_buffer_acquire(
            state->firmware_buffer, evidence->operation_id)) {
        evidence->decision = BACKEND_OTA_DECISION_FAILED;
        return false;
    }
    state->buffer_owned = true;
    uint8_t *bytes = backend_firmware_buffer_data(state->firmware_buffer);
    const size_t capacity = backend_firmware_buffer_capacity(
        state->firmware_buffer);
    size_t downloaded = 0U;
    if (bytes == NULL || capacity < evidence->manifest.image_size ||
        !state->adapters.download_image(
            state->adapters.context, evidence->catalog_name,
            bytes, capacity, evidence->manifest.image_size, &downloaded) ||
        downloaded != evidence->manifest.image_size) {
        evidence->decision = BACKEND_OTA_DECISION_FAILED;
        return false;
    }
    const backend_ota_image_result_t image_result = validate_staged(
        state, &evidence->manifest, component_image_kind(component),
        bytes, downloaded);
    if (image_result != BACKEND_OTA_IMAGE_OK) {
        evidence->decision = image_result ==
                BACKEND_OTA_IMAGE_DIGEST_MISMATCH ||
                image_result == BACKEND_OTA_IMAGE_CRC_MISMATCH
            ? BACKEND_OTA_DECISION_REJECT_DIGEST
            : BACKEND_OTA_DECISION_REJECT_IDENTITY;
        return false;
    }
    evidence->complete_image_validated = true;
    evidence->decision = BACKEND_OTA_DECISION_ADMIT;
    return true;
}

bool backend_ota_maintenance_run_probe(
    backend_ota_maintenance_t *state,
    backend_ota_component_t component,
    const char *catalog_name,
    const char *expected_sha256_or_null,
    backend_ota_evidence_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (state == NULL || !state->initialized || out == NULL ||
        backend_ota_component_slot(component) < -1) {
        return false;
    }
    backend_ota_evidence_t evidence;
    evidence_begin(state, component, catalog_name, true, &evidence);
    if (evidence.operation_id == 0U) {
        return false;
    }
    const char *expected_catalog = backend_ota_component_catalog_name(component);
    if (catalog_name == NULL || expected_catalog == NULL ||
        strcmp(catalog_name, expected_catalog) != 0) {
        evidence.decision = BACKEND_OTA_DECISION_REJECT_IDENTITY;
        emit_evidence(state, &evidence);
        *out = evidence;
        return true;
    }
    if (expected_sha256_or_null != NULL &&
        !lowercase_sha256(expected_sha256_or_null)) {
        evidence.decision = BACKEND_OTA_DECISION_REJECT_DIGEST;
        emit_evidence(state, &evidence);
        *out = evidence;
        return true;
    }
    if (state->busy) {
        evidence.decision = BACKEND_OTA_DECISION_REJECT_BUSY;
        emit_evidence(state, &evidence);
        *out = evidence;
        return true;
    }
    state->busy = true;
    backend_ota_target_binding_t binding;
    if (!snapshot_to_evidence(
            state, component, &binding, &evidence, true) ||
        !stage_complete_image(
            state, component, expected_sha256_or_null, false, &evidence)) {
        evidence.image_writes_after = state->adapters.image_write_count(
            state->adapters.context);
        release_buffer(state, evidence.operation_id);
        state->busy = false;
        emit_evidence(state, &evidence);
        *out = evidence;
        return true;
    }
    if (component != BACKEND_OTA_COMPONENT_UPLINK &&
        !state->adapters.scanner_dry_run(
            state->adapters.context, component, &evidence.manifest,
            backend_firmware_buffer_data(state->firmware_buffer),
            evidence.manifest.image_size)) {
        evidence.decision = BACKEND_OTA_DECISION_FAILED;
    }
    evidence.image_writes_after = state->adapters.image_write_count(
        state->adapters.context);
    if (evidence.image_writes_after != evidence.image_writes_before) {
        evidence.decision = BACKEND_OTA_DECISION_FAILED;
    }
    backend_ota_target_binding_t closing_binding;
    if (!snapshot_to_evidence(
            state, component, &closing_binding, &evidence, false) ||
        !binding_is_same(&binding, &closing_binding)) {
        evidence.decision = BACKEND_OTA_DECISION_REJECT_TARGET_BINDING;
    }
    release_buffer(state, evidence.operation_id);
    state->busy = false;
    emit_evidence(state, &evidence);
    *out = evidence;
    return true;
}

static bool request_is_valid(const backend_ota_request_t *request)
{
    const char *catalog = request == NULL ? NULL :
        backend_ota_component_catalog_name(request->component);
    return request != NULL && !request->probe && catalog != NULL &&
           bounded_string(
               request->catalog_name, sizeof(request->catalog_name)) &&
           strcmp(request->catalog_name, catalog) == 0 &&
           lowercase_sha256(request->expected_sha256) &&
           request->apply_mode >= BACKEND_OTA_NEWER_ONLY &&
           request->apply_mode <= BACKEND_OTA_SAME_VERSION_RECOVERY &&
           hardware_mac_is_valid(request->expected_mac) &&
           request->expected_boot_id != 0U &&
           request->expected_topology_generation != 0U;
}

static void fill_request_binding_evidence(
    const backend_ota_request_t *request,
    backend_ota_evidence_t *evidence)
{
    memcpy(evidence->expected_target_mac, request->expected_mac, 6U);
    evidence->expected_target_boot_id = request->expected_boot_id;
    evidence->expected_topology_generation =
        request->expected_topology_generation;
    evidence->boot_id_before = request->expected_boot_id;
    evidence->boot_id_after = request->expected_boot_id;
}

static backend_ota_journal_record_t accepted_record(
    const backend_ota_maintenance_t *state,
    const backend_ota_request_t *request,
    const backend_ota_evidence_t *evidence)
{
    backend_ota_journal_record_t record;
    memset(&record, 0, sizeof(record));
    record.schema = BACKEND_OTA_JOURNAL_SCHEMA;
    record.operation_id = evidence->operation_id;
    record.component = request->component;
    record.component_slot = backend_ota_component_slot(request->component);
    record.apply_mode = request->apply_mode;
    memcpy(record.catalog_name, evidence->catalog_name,
           sizeof(record.catalog_name));
    record.manifest = evidence->manifest;
    memcpy(record.uplink_mac, state->uplink_mac, 6U);
    record.uplink_boot_id = state->uplink_boot_id;
    memcpy(record.expected_target_mac, evidence->expected_target_mac, 6U);
    memcpy(record.actual_target_mac, evidence->actual_target_mac, 6U);
    record.expected_target_boot_id = evidence->expected_target_boot_id;
    record.actual_target_boot_id = evidence->actual_target_boot_id;
    record.expected_topology_generation =
        evidence->expected_topology_generation;
    record.actual_topology_generation =
        evidence->actual_topology_generation;
    record.phase = BACKEND_OTA_PHASE_ACCEPTED;
    record.image_writes_before = evidence->image_writes_before;
    record.image_writes_after = evidence->image_writes_before;
    return record;
}

static void fail_apply(
    backend_ota_maintenance_t *state,
    backend_ota_evidence_t *evidence,
    bool release_owned_buffer,
    bool durably_terminal)
{
    evidence->image_writes_after = state->adapters.image_write_count(
        state->adapters.context);
    evidence->decision = BACKEND_OTA_DECISION_FAILED;
    if (release_owned_buffer) {
        release_buffer(state, evidence->operation_id);
    }
    state->busy = !durably_terminal;
    emit_evidence(state, evidence);
}

bool backend_ota_maintenance_request_apply(
    backend_ota_maintenance_t *state,
    const backend_ota_request_t *request)
{
    if (state == NULL || !state->initialized || !request_is_valid(request)) {
        return false;
    }
    const backend_ota_request_t immutable_request = *request;
    request = &immutable_request;
    backend_ota_evidence_t evidence;
    evidence_begin(
        state, request->component, request->catalog_name, false, &evidence);
    if (evidence.operation_id == 0U) {
        return false;
    }
    evidence.apply_mode = request->apply_mode;
    fill_request_binding_evidence(request, &evidence);
    if (state->busy) {
        evidence.decision = BACKEND_OTA_DECISION_REJECT_BUSY;
        emit_evidence(state, &evidence);
        return false;
    }
    state->busy = true;
    const bool allow_same =
        request->apply_mode == BACKEND_OTA_SAME_VERSION_RECOVERY;
    if (!stage_complete_image(
            state, request->component, request->expected_sha256,
            allow_same, &evidence)) {
        evidence.image_writes_after = state->adapters.image_write_count(
            state->adapters.context);
        release_buffer(state, evidence.operation_id);
        state->busy = false;
        emit_evidence(state, &evidence);
        return false;
    }
    if (!state->adapters.acquire_target_claim(
            state->adapters.context, request->component)) {
        fail_apply(state, &evidence, true, true);
        return false;
    }
    backend_ota_target_binding_t actual;
    const bool snapshot_ok = snapshot_to_evidence(
        state, request->component, &actual, &evidence, false);
    if (!snapshot_ok ||
        !backend_ota_target_binding_matches(request, &actual)) {
        state->adapters.release_target_claim(
            state->adapters.context, request->component);
        evidence.decision = BACKEND_OTA_DECISION_REJECT_TARGET_BINDING;
        evidence.image_writes_after = state->adapters.image_write_count(
            state->adapters.context);
        release_buffer(state, evidence.operation_id);
        state->busy = false;
        emit_evidence(state, &evidence);
        return false;
    }

    backend_ota_journal_record_t journal = accepted_record(
        state, request, &evidence);
    if (backend_ota_journal_persist_accepted(
            &state->journal_storage, &journal) !=
            BACKEND_OTA_JOURNAL_PERSIST_COMMITTED) {
        state->adapters.release_target_claim(
            state->adapters.context, request->component);
        fail_apply(state, &evidence, true, false);
        return false;
    }
    const size_t accepted_length = backend_ota_accepted_encode(
        &journal, state->evidence_line, sizeof(state->evidence_line));
    if (accepted_length == 0U || !state->adapters.emit_and_flush(
            state->adapters.context, state->evidence_line,
            accepted_length)) {
        journal.phase = BACKEND_OTA_PHASE_FAILED;
        const backend_ota_journal_persist_result_t failed_result =
            backend_ota_journal_persist_transition(
                &state->journal_storage, &journal, state->uplink_boot_id);
        state->adapters.release_target_claim(
            state->adapters.context, request->component);
        fail_apply(
            state, &evidence, true,
            failed_result == BACKEND_OTA_JOURNAL_PERSIST_COMMITTED ||
            failed_result == BACKEND_OTA_JOURNAL_PERSIST_ALREADY_DURABLE);
        return false;
    }

    journal.phase = BACKEND_OTA_PHASE_WRITING;
    if (backend_ota_journal_persist_transition(
            &state->journal_storage, &journal, state->uplink_boot_id) !=
            BACKEND_OTA_JOURNAL_PERSIST_MUTATION_AUTHORIZED) {
        journal.phase = BACKEND_OTA_PHASE_FAILED;
        const backend_ota_journal_persist_result_t failed_result =
            backend_ota_journal_persist_transition(
                &state->journal_storage, &journal, state->uplink_boot_id);
        state->adapters.release_target_claim(
            state->adapters.context, request->component);
        fail_apply(
            state, &evidence, true,
            failed_result == BACKEND_OTA_JOURNAL_PERSIST_COMMITTED ||
            failed_result == BACKEND_OTA_JOURNAL_PERSIST_ALREADY_DURABLE);
        return false;
    }
    const bool mutation_ok = state->adapters.mutate_staged_image(
        state->adapters.context, request->component, &evidence.manifest,
        backend_firmware_buffer_data(state->firmware_buffer),
        evidence.manifest.image_size);
    evidence.image_writes_after = state->adapters.image_write_count(
        state->adapters.context);
    state->adapters.release_target_claim(
        state->adapters.context, request->component);
    if (!mutation_ok ||
        evidence.image_writes_after <= evidence.image_writes_before) {
        journal.phase = BACKEND_OTA_PHASE_FAILED;
        journal.image_writes_after = evidence.image_writes_after;
        const backend_ota_journal_persist_result_t failed_result =
            backend_ota_journal_persist_transition(
                &state->journal_storage, &journal, state->uplink_boot_id);
        fail_apply(
            state, &evidence, true,
            failed_result == BACKEND_OTA_JOURNAL_PERSIST_COMMITTED ||
            failed_result == BACKEND_OTA_JOURNAL_PERSIST_ALREADY_DURABLE);
        return false;
    }

    journal.phase = BACKEND_OTA_PHASE_REBOOT_PENDING;
    journal.image_writes_after = evidence.image_writes_after;
    const backend_ota_journal_persist_result_t reboot_phase_result =
        backend_ota_journal_persist_transition(
            &state->journal_storage, &journal, state->uplink_boot_id);
    const bool reboot_requested =
        reboot_phase_result == BACKEND_OTA_JOURNAL_PERSIST_COMMITTED &&
        state->adapters.request_reboot(
            state->adapters.context, request->component);
    if (!reboot_requested) {
        journal.phase = BACKEND_OTA_PHASE_FAILED;
        const backend_ota_journal_persist_result_t failed_result =
            backend_ota_journal_persist_transition(
                &state->journal_storage, &journal, state->uplink_boot_id);
        fail_apply(
            state, &evidence, true,
            failed_result == BACKEND_OTA_JOURNAL_PERSIST_COMMITTED ||
            failed_result == BACKEND_OTA_JOURNAL_PERSIST_ALREADY_DURABLE);
        return false;
    }
    evidence.decision = BACKEND_OTA_DECISION_ADMIT;
    emit_evidence(state, &evidence);
    return true;
}

bool backend_ota_maintenance_auto_poll(
    backend_ota_maintenance_t *state,
    backend_ota_component_t component,
    bool auto_update_enabled,
    backend_ota_auto_decision_t *out_decision)
{
    if (out_decision != NULL) {
        *out_decision = BACKEND_OTA_AUTO_REJECT_VERSION;
    }
    const char *catalog = backend_ota_component_catalog_name(component);
    if (state == NULL || !state->initialized || out_decision == NULL ||
        catalog == NULL || state->busy) {
        return false;
    }

    size_t metadata_length = 0U;
    uint32_t generation = 0U;
    memset(state->metadata, 0, sizeof(state->metadata));
    if (!state->adapters.fetch_metadata(
            state->adapters.context, catalog, state->metadata,
            BACKEND_OTA_MAINTENANCE_METADATA_CAPACITY,
            &metadata_length, &generation) ||
        metadata_length == 0U ||
        metadata_length > BACKEND_OTA_MAINTENANCE_METADATA_CAPACITY ||
        state->metadata[metadata_length] != '\0') {
        return false;
    }
    backend_ota_manifest_t manifest;
    if (!backend_ota_manifest_decode_metadata(
            state->metadata, metadata_length, generation, false, &manifest)) {
        return false;
    }
    const char *running = state->adapters.running_version(
        state->adapters.context, component);
    if (running == NULL) {
        return false;
    }
    const fof_firmware_version_relation_t relation =
        fof_firmware_version_compare(manifest.version, running);
    if (relation == FOF_VERSION_NEWER &&
        backend_ota_manifest_admit(
            &manifest, component_image_kind(component), running,
            state->adapters.partition_capacity(
                state->adapters.context, component)) != BACKEND_OTA_ADMIT) {
        *out_decision = BACKEND_OTA_AUTO_REJECT_VERSION;
        return false;
    }
    const backend_ota_auto_decision_t policy = backend_ota_auto_policy(
        auto_update_enabled, relation);
    *out_decision = policy;
    if (policy != BACKEND_OTA_AUTO_APPLY_NEWER) {
        return true;
    }
    backend_ota_target_binding_t binding;
    if (!state->adapters.snapshot_binding(
            state->adapters.context, component, &binding) ||
        binding.component != component ||
        binding.component_slot != backend_ota_component_slot(component) ||
        !hardware_mac_is_valid(binding.target_mac) ||
        binding.target_boot_id == 0U || binding.topology_generation == 0U) {
        return false;
    }
    backend_ota_request_t request;
    memset(&request, 0, sizeof(request));
    request.component = component;
    request.apply_mode = BACKEND_OTA_NEWER_ONLY;
    memcpy(request.catalog_name, catalog, strlen(catalog) + 1U);
    memcpy(request.expected_sha256, manifest.sha256,
           sizeof(request.expected_sha256));
    memcpy(request.expected_mac, binding.target_mac, 6U);
    request.expected_boot_id = binding.target_boot_id;
    request.expected_topology_generation = binding.topology_generation;
    return backend_ota_maintenance_request_apply(state, &request);
}

static void journal_to_evidence(
    const backend_ota_journal_record_t *record,
    backend_ota_decision_t decision,
    backend_ota_evidence_t *evidence)
{
    memset(evidence, 0, sizeof(*evidence));
    evidence->operation_id = record->operation_id;
    evidence->component = record->component;
    evidence->component_slot = record->component_slot;
    evidence->apply_mode = record->apply_mode;
    memcpy(evidence->uplink_mac, record->uplink_mac, 6U);
    memcpy(evidence->catalog_name, record->catalog_name,
           sizeof(evidence->catalog_name));
    evidence->manifest = record->manifest;
    evidence->decision = decision;
    evidence->partition_capacity = BACKEND_FIRMWARE_BUFFER_CAPACITY;
    memcpy(evidence->expected_target_mac,
           record->expected_target_mac, 6U);
    memcpy(evidence->actual_target_mac,
           record->actual_target_mac, 6U);
    evidence->expected_target_boot_id = record->expected_target_boot_id;
    evidence->actual_target_boot_id = record->actual_target_boot_id;
    evidence->expected_topology_generation =
        record->expected_topology_generation;
    evidence->actual_topology_generation =
        record->actual_topology_generation;
    evidence->complete_image_validated = true;
    evidence->image_writes_before = record->image_writes_before;
    evidence->image_writes_after = record->image_writes_after;
    evidence->boot_id_before = record->actual_target_boot_id;
    evidence->boot_id_after = record->boot_id_after == 0U
        ? record->actual_target_boot_id : record->boot_id_after;
    evidence->rollback_clear = record->rollback_clear;
    evidence->converged = record->converged;
}

static bool persist_failed(
    backend_ota_maintenance_t *state,
    backend_ota_journal_record_t *record)
{
    record->phase = BACKEND_OTA_PHASE_FAILED;
    const backend_ota_journal_persist_result_t result =
        backend_ota_journal_persist_transition(
            &state->journal_storage, record, state->uplink_boot_id);
    return result == BACKEND_OTA_JOURNAL_PERSIST_COMMITTED ||
           result == BACKEND_OTA_JOURNAL_PERSIST_ALREADY_DURABLE;
}

static void close_failed_only_if_durable(
    backend_ota_maintenance_t *state,
    backend_ota_journal_record_t *record)
{
    if (!persist_failed(state, record)) {
        state->busy = true;
        return;
    }
    backend_ota_evidence_t evidence;
    journal_to_evidence(record, BACKEND_OTA_DECISION_FAILED, &evidence);
    emit_evidence(state, &evidence);
    release_buffer(state, record->operation_id);
    state->busy = false;
}

static bool convergence_binding_valid(
    const backend_ota_journal_record_t *record,
    const backend_ota_convergence_t *convergence)
{
    return convergence->binding.component == record->component &&
           convergence->binding.component_slot == record->component_slot &&
           memcmp(convergence->binding.target_mac,
                  record->expected_target_mac, 6U) == 0 &&
           convergence->binding.topology_generation ==
               record->expected_topology_generation &&
           convergence->binding.target_boot_id != 0U &&
           convergence->binding.target_boot_id !=
               record->actual_target_boot_id;
}

bool backend_ota_maintenance_resume(
    backend_ota_maintenance_t *state, bool convergence_deadline_expired)
{
    if (state == NULL || !state->initialized) {
        return false;
    }
    backend_ota_journal_record_t record;
    if (backend_ota_journal_load(&state->journal_storage, &record) !=
        BACKEND_OTA_JOURNAL_LOAD_PRESENT) {
        state->busy = true;
        return false;
    }
    state->busy = record.phase != BACKEND_OTA_PHASE_COMPLETE &&
                  record.phase != BACKEND_OTA_PHASE_FAILED;
    if (record.phase == BACKEND_OTA_PHASE_COMPLETE ||
        record.phase == BACKEND_OTA_PHASE_FAILED) {
        backend_ota_evidence_t evidence;
        journal_to_evidence(
            &record,
            record.phase == BACKEND_OTA_PHASE_COMPLETE
                ? BACKEND_OTA_DECISION_APPLIED
                : BACKEND_OTA_DECISION_FAILED,
            &evidence);
        emit_evidence(state, &evidence);
        release_buffer(state, record.operation_id);
        state->busy = false;
        return record.phase == BACKEND_OTA_PHASE_COMPLETE;
    }
    if (record.phase == BACKEND_OTA_PHASE_ACCEPTED) {
        close_failed_only_if_durable(state, &record);
        return false;
    }

    backend_ota_convergence_t convergence;
    memset(&convergence, 0, sizeof(convergence));
    if (!state->adapters.read_convergence(
            state->adapters.context, record.component, &record.manifest,
            &convergence) ||
        !convergence_binding_valid(&record, &convergence)) {
        if (!convergence_deadline_expired) {
            return false;
        }
        close_failed_only_if_durable(state, &record);
        return false;
    }

    if (record.phase == BACKEND_OTA_PHASE_WRITING) {
        record.image_writes_after = state->adapters.image_write_count(
            state->adapters.context);
        if (record.image_writes_after <= record.image_writes_before) {
            if (!convergence_deadline_expired) {
                return false;
            }
            close_failed_only_if_durable(state, &record);
            return false;
        }
        record.phase = BACKEND_OTA_PHASE_REBOOT_PENDING;
        if (backend_ota_journal_persist_transition(
                &state->journal_storage, &record, state->uplink_boot_id) !=
            BACKEND_OTA_JOURNAL_PERSIST_COMMITTED) {
            return false;
        }
    }
    if (record.phase == BACKEND_OTA_PHASE_REBOOT_PENDING) {
        record.phase = BACKEND_OTA_PHASE_CONVERGENCE_PENDING;
        record.boot_id_after = convergence.binding.target_boot_id;
        if (backend_ota_journal_persist_transition(
                &state->journal_storage, &record, state->uplink_boot_id) !=
            BACKEND_OTA_JOURNAL_PERSIST_COMMITTED) {
            return false;
        }
    }
    const bool healthy = convergence.identity_exact &&
                         convergence.command_ingress_healthy &&
                         convergence.role_acked &&
                         convergence.profile_correct &&
                         convergence.radio_healthy &&
                         convergence.rollback_clear;
    if (!healthy) {
        if (!convergence_deadline_expired) {
            return false;
        }
        close_failed_only_if_durable(state, &record);
        return false;
    }
    record.phase = BACKEND_OTA_PHASE_COMPLETE;
    record.rollback_clear = true;
    record.converged = true;
    const uint32_t live_write_count = state->adapters.image_write_count(
        state->adapters.context);
    if (live_write_count > record.image_writes_after) {
        record.image_writes_after = live_write_count;
    }
    if (backend_ota_journal_persist_transition(
            &state->journal_storage, &record, state->uplink_boot_id) !=
        BACKEND_OTA_JOURNAL_PERSIST_COMMITTED) {
        return false;
    }
    backend_ota_evidence_t evidence;
    journal_to_evidence(&record, BACKEND_OTA_DECISION_APPLIED, &evidence);
    emit_evidence(state, &evidence);
    release_buffer(state, record.operation_id);
    state->busy = false;
    return true;
}

bool backend_ota_maintenance_emit_status(backend_ota_maintenance_t *state)
{
    if (state == NULL || !state->initialized) {
        return false;
    }
    backend_ota_journal_record_t record;
    if (backend_ota_journal_load(&state->journal_storage, &record) !=
            BACKEND_OTA_JOURNAL_LOAD_PRESENT ||
        (record.phase != BACKEND_OTA_PHASE_COMPLETE &&
         record.phase != BACKEND_OTA_PHASE_FAILED)) {
        return false;
    }
    backend_ota_evidence_t evidence;
    journal_to_evidence(
        &record,
        record.phase == BACKEND_OTA_PHASE_COMPLETE
            ? BACKEND_OTA_DECISION_APPLIED : BACKEND_OTA_DECISION_FAILED,
        &evidence);
    return emit_evidence(state, &evidence);
}

bool backend_ota_maintenance_last_evidence(
    const backend_ota_maintenance_t *state,
    backend_ota_evidence_t *out)
{
    if (state == NULL || out == NULL || !state->has_last_evidence) {
        return false;
    }
    *out = state->last_evidence;
    return true;
}

bool backend_ota_maintenance_available(
    const backend_ota_maintenance_t *state)
{
    return state != NULL && state->initialized && !state->busy &&
           backend_firmware_buffer_capacity(state->firmware_buffer) ==
               BACKEND_FIRMWARE_BUFFER_CAPACITY;
}
