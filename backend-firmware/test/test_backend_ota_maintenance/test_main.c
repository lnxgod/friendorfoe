#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "backend_identity.h"
#include "backend_ota_identity.h"
#include "backend_ota_maintenance.h"
#include "backend_self_ota.h"
#include "backend_test_main.h"

typedef struct {
    unsigned begin_calls;
    unsigned write_calls;
    unsigned end_calls;
    unsigned select_calls;
    unsigned mark_valid_calls;
    size_t next_offset;
    size_t expected_size;
    bool fail_begin;
    bool fail_write;
    bool fail_end;
    bool fail_select;
} self_fixture_t;

typedef struct {
    uint8_t journal[BACKEND_OTA_JOURNAL_CANONICAL_SIZE];
    size_t journal_length;
    bool has_journal;
    uint8_t arena[BACKEND_FIRMWARE_BUFFER_CAPACITY];
    char metadata[1024];
    const char *running_version;
    backend_ota_target_binding_t binding;
    backend_ota_convergence_t convergence;
    uint32_t image_write_count;
    unsigned metadata_calls;
    unsigned download_calls;
    unsigned snapshot_calls;
    unsigned scanner_dry_run_calls;
    unsigned mutate_calls;
    unsigned reboot_calls;
    unsigned accepted_emits;
    unsigned evidence_emits;
    char events[64];
    size_t event_count;
    uint8_t drift_binding;
    bool fail_download;
    bool fail_validate;
    bool fail_emit;
    bool fail_journal_store;
    uint8_t change_binding_during_dry_run;
    backend_ota_request_t *request_to_mutate;
} maintenance_fixture_t;

static void fixture_event(maintenance_fixture_t *fixture, char event)
{
    if (fixture->event_count < sizeof(fixture->events)) {
        fixture->events[fixture->event_count++] = event;
    }
}

static void *maintenance_alloc(size_t size, void *context)
{
    maintenance_fixture_t *fixture = context;
    return size == sizeof(fixture->arena) ? fixture->arena : NULL;
}

static backend_ota_journal_io_result_t maintenance_journal_load(
    void *context, uint8_t *out, size_t capacity, size_t *out_length)
{
    maintenance_fixture_t *fixture = context;
    if (!fixture->has_journal) {
        return BACKEND_OTA_JOURNAL_IO_NOT_FOUND;
    }
    if (out == NULL || out_length == NULL ||
        capacity < fixture->journal_length) {
        return BACKEND_OTA_JOURNAL_IO_ERROR;
    }
    memcpy(out, fixture->journal, fixture->journal_length);
    *out_length = fixture->journal_length;
    return BACKEND_OTA_JOURNAL_IO_OK;
}

static bool maintenance_journal_store(
    void *context, const uint8_t *bytes, size_t length)
{
    maintenance_fixture_t *fixture = context;
    backend_ota_journal_record_t record;
    if (fixture->fail_journal_store || bytes == NULL ||
        length > sizeof(fixture->journal) ||
        backend_ota_journal_decode(bytes, length, &record) !=
            BACKEND_OTA_JOURNAL_VALID) {
        return false;
    }
    memcpy(fixture->journal, bytes, length);
    fixture->journal_length = length;
    fixture->has_journal = true;
    static const char phase_events[] = {'A', 'W', 'R', 'C', 'X', 'F'};
    fixture_event(fixture, phase_events[record.phase]);
    return true;
}

static bool maintenance_fetch_metadata(
    void *context,
    const char *catalog_name,
    char *json,
    size_t capacity,
    size_t *out_length,
    uint32_t *out_generation)
{
    maintenance_fixture_t *fixture = context;
    fixture->metadata_calls++;
    if (catalog_name == NULL || json == NULL || out_length == NULL ||
        out_generation == NULL) {
        return false;
    }
    const size_t length = strlen(fixture->metadata);
    if (length >= capacity) {
        return false;
    }
    memcpy(json, fixture->metadata, length + 1U);
    *out_length = length;
    *out_generation = 9U;
    return true;
}

static bool maintenance_download(
    void *context,
    const char *catalog_name,
    uint8_t *destination,
    size_t capacity,
    size_t expected_size,
    size_t *out_size)
{
    maintenance_fixture_t *fixture = context;
    fixture->download_calls++;
    if (fixture->fail_download || catalog_name == NULL ||
        destination == NULL || out_size == NULL || expected_size != 8U ||
        capacity < expected_size) {
        return false;
    }
    if (fixture->request_to_mutate != NULL) {
        fixture->request_to_mutate->component =
            BACKEND_OTA_COMPONENT_UPLINK;
        fixture->request_to_mutate->expected_boot_id++;
        fixture->request_to_mutate->expected_sha256[0] = 'f';
    }
    for (size_t index = 0U; index < expected_size; ++index) {
        destination[index] = (uint8_t)(index + 1U);
    }
    *out_size = expected_size;
    return true;
}

static const char *maintenance_running_version(
    void *context, backend_ota_component_t component)
{
    maintenance_fixture_t *fixture = context;
    (void)component;
    return fixture->running_version;
}

static size_t maintenance_partition_capacity(
    void *context, backend_ota_component_t component)
{
    (void)context;
    (void)component;
    return BACKEND_FIRMWARE_BUFFER_CAPACITY;
}

static uint32_t maintenance_write_count(void *context)
{
    maintenance_fixture_t *fixture = context;
    return fixture->image_write_count;
}

static bool maintenance_snapshot(
    void *context,
    backend_ota_component_t component,
    backend_ota_target_binding_t *out)
{
    maintenance_fixture_t *fixture = context;
    fixture->snapshot_calls++;
    *out = fixture->binding;
    out->component = component;
    out->component_slot = backend_ota_component_slot(component);
    if (fixture->drift_binding == 1U) {
        out->target_mac[5] ^= 1U;
    } else if (fixture->drift_binding == 2U) {
        out->target_boot_id++;
    } else if (fixture->drift_binding == 3U) {
        out->topology_generation++;
    } else if (fixture->drift_binding == 4U) {
        out->component_slot = out->component_slot == 0 ? 1 : 0;
    }
    return true;
}

static bool maintenance_acquire_claim(
    void *context, backend_ota_component_t component)
{
    maintenance_fixture_t *fixture = context;
    (void)component;
    fixture_event(fixture, 'L');
    return true;
}

static void maintenance_release_claim(
    void *context, backend_ota_component_t component)
{
    maintenance_fixture_t *fixture = context;
    (void)component;
    fixture_event(fixture, 'U');
}

static backend_ota_image_result_t maintenance_validate(
    void *context,
    const backend_ota_manifest_t *manifest,
    backend_image_kind_t expected_kind,
    const uint8_t *bytes,
    size_t length)
{
    maintenance_fixture_t *fixture = context;
    if (fixture->fail_validate || manifest == NULL || bytes == NULL ||
        length != manifest->image_size ||
        (expected_kind == BACKEND_IMAGE_SCANNER &&
         strcmp(manifest->target, FOF_BACKEND_SCANNER_TARGET) != 0) ||
        (expected_kind == BACKEND_IMAGE_UPLINK &&
         strcmp(manifest->target, FOF_BACKEND_UPLINK_TARGET) != 0)) {
        return BACKEND_OTA_IMAGE_FORMAT_ERROR;
    }
    return BACKEND_OTA_IMAGE_OK;
}

static bool maintenance_scanner_dry_run(
    void *context,
    backend_ota_component_t component,
    const backend_ota_manifest_t *manifest,
    const uint8_t *bytes,
    size_t length)
{
    maintenance_fixture_t *fixture = context;
    fixture->scanner_dry_run_calls++;
    if (fixture->change_binding_during_dry_run == 1U) {
        fixture->binding.target_boot_id++;
    } else if (fixture->change_binding_during_dry_run == 2U) {
        fixture->drift_binding = 4U;
    }
    return component != BACKEND_OTA_COMPONENT_UPLINK && manifest != NULL &&
           bytes != NULL && length == manifest->image_size;
}

static bool maintenance_mutate(
    void *context,
    backend_ota_component_t component,
    const backend_ota_manifest_t *manifest,
    const uint8_t *bytes,
    size_t length)
{
    maintenance_fixture_t *fixture = context;
    (void)component;
    fixture->mutate_calls++;
    fixture->image_write_count++;
    fixture_event(fixture, 'D');
    return manifest != NULL && bytes != NULL && length == manifest->image_size;
}

static bool maintenance_reboot(
    void *context, backend_ota_component_t component)
{
    maintenance_fixture_t *fixture = context;
    (void)component;
    fixture->reboot_calls++;
    fixture_event(fixture, 'Q');
    return true;
}

static bool maintenance_read_convergence(
    void *context,
    backend_ota_component_t component,
    const backend_ota_manifest_t *manifest,
    backend_ota_convergence_t *out)
{
    maintenance_fixture_t *fixture = context;
    (void)component;
    (void)manifest;
    *out = fixture->convergence;
    return true;
}

static bool maintenance_emit(
    void *context, const char *line, size_t length)
{
    maintenance_fixture_t *fixture = context;
    if (fixture->fail_emit || line == NULL || length == 0U) {
        return false;
    }
    if (strncmp(line, "FOF_BACKEND_OTA_ACCEPTED ",
                strlen("FOF_BACKEND_OTA_ACCEPTED ")) == 0) {
        fixture->accepted_emits++;
        fixture_event(fixture, 'B');
    } else if (strncmp(line, "FOF_BACKEND_OTA_EVIDENCE ",
                       strlen("FOF_BACKEND_OTA_EVIDENCE ")) == 0) {
        fixture->evidence_emits++;
        fixture_event(fixture, 'E');
    } else {
        return false;
    }
    return true;
}

static void maintenance_metadata(
    maintenance_fixture_t *fixture,
    backend_ota_component_t component,
    const char *version)
{
    const bool uplink = component == BACKEND_OTA_COMPONENT_UPLINK;
    const char *target = uplink
        ? FOF_BACKEND_UPLINK_TARGET : FOF_BACKEND_SCANNER_TARGET;
    const char *project = uplink
        ? FOF_BACKEND_UPLINK_PROJECT : FOF_BACKEND_SCANNER_PROJECT;
    const int length = snprintf(
        fixture->metadata, sizeof(fixture->metadata),
        "{\"name\":\"%s\",\"target\":\"%s\",\"description\":\"backend\","
        "\"board\":\"esp32s3\",\"project\":\"%s\","
        "\"hardware\":\"seeed_xiao_esp32s3\",\"version\":\"%s\","
        "\"size\":8,\"sha256\":\"0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef\",\"crc32\":305419896,"
        "\"download_url\":\"/nodes/firmware/download/%s\"}",
        target, target, project, version, target);
    TEST_ASSERT_GREATER_THAN(0, length);
    TEST_ASSERT_LESS_THAN((int)sizeof(fixture->metadata), length);
}

static backend_ota_maintenance_adapters_t maintenance_adapters(
    maintenance_fixture_t *fixture)
{
    const backend_ota_maintenance_adapters_t adapters = {
        .context = fixture,
        .fetch_metadata = maintenance_fetch_metadata,
        .download_image = maintenance_download,
        .running_version = maintenance_running_version,
        .partition_capacity = maintenance_partition_capacity,
        .image_write_count = maintenance_write_count,
        .snapshot_binding = maintenance_snapshot,
        .acquire_target_claim = maintenance_acquire_claim,
        .release_target_claim = maintenance_release_claim,
        .validate_staged_image = maintenance_validate,
        .scanner_dry_run = maintenance_scanner_dry_run,
        .mutate_staged_image = maintenance_mutate,
        .request_reboot = maintenance_reboot,
        .read_convergence = maintenance_read_convergence,
        .emit_and_flush = maintenance_emit,
    };
    return adapters;
}

static void maintenance_fixture_init(
    maintenance_fixture_t *fixture,
    backend_ota_maintenance_t *state,
    backend_firmware_buffer_t *buffer,
    backend_ota_component_t component,
    const char *candidate_version,
    const char *running_version)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->running_version = running_version;
    maintenance_metadata(fixture, component, candidate_version);
    fixture->binding.component = component;
    fixture->binding.component_slot = backend_ota_component_slot(component);
    const uint8_t target_mac[6] = {
        0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU,
        component == BACKEND_OTA_COMPONENT_UPLINK ? 0xFFU :
            (uint8_t)(component == BACKEND_OTA_COMPONENT_SCANNER0 ? 1U : 2U),
    };
    memcpy(fixture->binding.target_mac, target_mac, 6U);
    fixture->binding.target_boot_id = 77U;
    fixture->binding.topology_generation = 4U;
    fixture->convergence.binding = fixture->binding;
    fixture->convergence.binding.target_boot_id = 78U;
    fixture->convergence.identity_exact = true;
    fixture->convergence.command_ingress_healthy = true;
    fixture->convergence.role_acked = true;
    fixture->convergence.profile_correct = true;
    fixture->convergence.radio_healthy = true;
    fixture->convergence.rollback_clear = true;

    memset(buffer, 0, sizeof(*buffer));
    TEST_ASSERT_TRUE(backend_firmware_buffer_init_once(
        buffer, maintenance_alloc, fixture));
    const backend_ota_maintenance_adapters_t adapters =
        maintenance_adapters(fixture);
    const backend_ota_journal_storage_t journal = {
        .context = fixture,
        .load = maintenance_journal_load,
        .store = maintenance_journal_store,
    };
    const uint8_t uplink_mac[6] = {
        0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU,
    };
    TEST_ASSERT_TRUE(backend_ota_maintenance_init(
        state, &adapters, &journal, buffer, uplink_mac, 100U));
}

static backend_ota_request_t maintenance_apply_request(
    const maintenance_fixture_t *fixture,
    backend_ota_component_t component,
    backend_ota_apply_mode_t mode)
{
    backend_ota_request_t request = {0};
    request.component = component;
    request.apply_mode = mode;
    strcpy(request.catalog_name, backend_ota_component_catalog_name(component));
    strcpy(request.expected_sha256,
           "0123456789abcdef0123456789abcdef"
           "0123456789abcdef0123456789abcdef");
    memcpy(request.expected_mac, fixture->binding.target_mac, 6U);
    request.expected_boot_id = fixture->binding.target_boot_id;
    request.expected_topology_generation =
        fixture->binding.topology_generation;
    return request;
}

static bool fixture_begin(void *context, size_t image_size)
{
    self_fixture_t *fixture = context;
    fixture->begin_calls++;
    fixture->expected_size = image_size;
    fixture->next_offset = 0U;
    return !fixture->fail_begin;
}

static bool fixture_write(
    void *context, size_t offset, const uint8_t *bytes, size_t length)
{
    self_fixture_t *fixture = context;
    fixture->write_calls++;
    if (fixture->fail_write || bytes == NULL || length == 0U ||
        offset != fixture->next_offset ||
        length > fixture->expected_size - fixture->next_offset) {
        return false;
    }
    fixture->next_offset += length;
    return true;
}

static bool fixture_end(void *context)
{
    self_fixture_t *fixture = context;
    fixture->end_calls++;
    return !fixture->fail_end;
}

static bool fixture_select(void *context)
{
    self_fixture_t *fixture = context;
    fixture->select_calls++;
    return !fixture->fail_select;
}

static bool fixture_mark_valid(void *context)
{
    self_fixture_t *fixture = context;
    fixture->mark_valid_calls++;
    return true;
}

static backend_ota_manifest_t valid_uplink_manifest(void)
{
    backend_ota_manifest_t manifest = {0};
    strcpy(manifest.target, FOF_BACKEND_UPLINK_TARGET);
    strcpy(manifest.project, FOF_BACKEND_UPLINK_PROJECT);
    strcpy(manifest.hardware, FOF_BACKEND_HARDWARE);
    strcpy(manifest.version, "0.1.1-backend");
    manifest.image_size = 8U;
    manifest.crc32 = UINT32_C(0x12345678);
    strcpy(manifest.sha256,
           "0123456789abcdef0123456789abcdef"
           "0123456789abcdef0123456789abcdef");
    manifest.generation = 7U;
    return manifest;
}

static backend_self_ota_adapters_t fixture_adapters(self_fixture_t *fixture)
{
    const backend_self_ota_adapters_t adapters = {
        .context = fixture,
        .begin = fixture_begin,
        .write = fixture_write,
        .end = fixture_end,
        .select_boot_partition = fixture_select,
        .mark_running_valid = fixture_mark_valid,
    };
    return adapters;
}

void setUp(void) {}
void tearDown(void) {}

void test_self_ota_rejects_non_uplink_identity_before_mutation(void)
{
    self_fixture_t fixture = {0};
    backend_self_ota_t state;
    backend_self_ota_adapters_t adapters = fixture_adapters(&fixture);
    backend_self_ota_init(&state, &adapters);

    backend_ota_manifest_t manifest = valid_uplink_manifest();
    strcpy(manifest.target, FOF_BACKEND_SCANNER_TARGET);
    TEST_ASSERT_EQUAL(BACKEND_SELF_OTA_REJECTED,
                      backend_self_ota_begin(&state, &manifest));
    TEST_ASSERT_EQUAL_UINT(0U, fixture.begin_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, backend_self_ota_image_write_count(&state));
}

void test_self_ota_writes_exact_image_then_readies_separately_gated_reboot(void)
{
    self_fixture_t fixture = {0};
    backend_self_ota_t state;
    backend_self_ota_adapters_t adapters = fixture_adapters(&fixture);
    backend_self_ota_init(&state, &adapters);
    const backend_ota_manifest_t manifest = valid_uplink_manifest();
    const uint8_t first[] = {1U, 2U, 3U};
    const uint8_t second[] = {4U, 5U, 6U, 7U, 8U};

    TEST_ASSERT_EQUAL(BACKEND_SELF_OTA_READY,
                      backend_self_ota_begin(&state, &manifest));
    TEST_ASSERT_TRUE(backend_self_ota_write(&state, 0U, first, sizeof(first)));
    TEST_ASSERT_FALSE(backend_self_ota_write(&state, 0U, first, sizeof(first)));
    TEST_ASSERT_TRUE(backend_self_ota_write(
        &state, sizeof(first), second, sizeof(second)));
    TEST_ASSERT_EQUAL(BACKEND_SELF_OTA_READY_TO_REBOOT,
                      backend_self_ota_finish(&state));
    TEST_ASSERT_EQUAL_UINT(1U, fixture.begin_calls);
    TEST_ASSERT_EQUAL_UINT(2U, fixture.write_calls);
    TEST_ASSERT_EQUAL_UINT(1U, fixture.end_calls);
    TEST_ASSERT_EQUAL_UINT(1U, fixture.select_calls);
    TEST_ASSERT_EQUAL_UINT32(5U, backend_self_ota_image_write_count(&state));
}

void test_self_ota_failure_never_selects_boot_partition(void)
{
    self_fixture_t fixture = {.fail_write = true};
    backend_self_ota_t state;
    backend_self_ota_adapters_t adapters = fixture_adapters(&fixture);
    backend_self_ota_init(&state, &adapters);
    const backend_ota_manifest_t manifest = valid_uplink_manifest();
    const uint8_t bytes[] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};

    TEST_ASSERT_EQUAL(BACKEND_SELF_OTA_READY,
                      backend_self_ota_begin(&state, &manifest));
    TEST_ASSERT_FALSE(backend_self_ota_write(
        &state, 0U, bytes, sizeof(bytes)));
    TEST_ASSERT_EQUAL(BACKEND_SELF_OTA_FAILED,
                      backend_self_ota_finish(&state));
    TEST_ASSERT_EQUAL_UINT(0U, fixture.end_calls);
    TEST_ASSERT_EQUAL_UINT(0U, fixture.select_calls);
}

void test_self_ota_rollback_clear_requires_local_runtime_not_backend(void)
{
    self_fixture_t fixture = {0};
    backend_self_ota_t state;
    backend_self_ota_adapters_t adapters = fixture_adapters(&fixture);
    backend_self_ota_init(&state, &adapters);
    backend_self_ota_on_boot(&state, 41U, true);

    backend_self_ota_health_t health = {
        .config_loaded = true,
        .led_worker_running = true,
        .uart_worker_running = true,
        .coordinator_worker_running = true,
        .ap_healthy = false,
        .sta_healthy = true,
        .backend_reachable = false,
    };
    TEST_ASSERT_TRUE(backend_self_ota_mark_valid_if_healthy(&state, &health));
    TEST_ASSERT_EQUAL_UINT(1U, fixture.mark_valid_calls);
    TEST_ASSERT_TRUE(backend_self_ota_rollback_clear(&state));
    const backend_ota_manifest_t next = valid_uplink_manifest();
    TEST_ASSERT_EQUAL(
        BACKEND_SELF_OTA_READY, backend_self_ota_begin(&state, &next));

    backend_self_ota_on_boot(&state, 42U, true);
    health.sta_healthy = false;
    TEST_ASSERT_FALSE(backend_self_ota_mark_valid_if_healthy(&state, &health));
    TEST_ASSERT_EQUAL_UINT(1U, fixture.mark_valid_calls);
    TEST_ASSERT_FALSE(backend_self_ota_rollback_clear(&state));
}

void test_usb_probe_and_apply_parse_only_exact_backend_commands(void)
{
    static const char sha[] =
        "0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef";
    backend_ota_request_t request;
    const char probe[] =
        "FOF_BACKEND_OTA_PROBE scanner0 scanner-s3-combo-backend *";
    TEST_ASSERT_TRUE(backend_ota_maintenance_parse_usb(
        probe, sizeof(probe) - 1U, &request));
    TEST_ASSERT_TRUE(request.probe);
    TEST_ASSERT_EQUAL(BACKEND_OTA_COMPONENT_SCANNER0, request.component);
    TEST_ASSERT_EQUAL_STRING("scanner-s3-combo-backend", request.catalog_name);
    TEST_ASSERT_EQUAL_STRING("", request.expected_sha256);

    char apply[256];
    const int apply_length = snprintf(
        apply, sizeof(apply),
        "FOF_BACKEND_OTA_APPLY scanner1 %s same-version-recovery "
        "AA:BB:CC:DD:EE:01 77 4",
        sha);
    TEST_ASSERT_GREATER_THAN(0, apply_length);
    TEST_ASSERT_TRUE(backend_ota_maintenance_parse_usb(
        apply, (size_t)apply_length, &request));
    TEST_ASSERT_FALSE(request.probe);
    TEST_ASSERT_EQUAL(BACKEND_OTA_COMPONENT_SCANNER1, request.component);
    TEST_ASSERT_EQUAL_STRING("scanner-s3-combo-backend", request.catalog_name);
    TEST_ASSERT_EQUAL_STRING(sha, request.expected_sha256);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_SAME_VERSION_RECOVERY, request.apply_mode);
    const uint8_t expected_mac[6] = {
        0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0x01U,
    };
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_mac, request.expected_mac, 6U);
    TEST_ASSERT_EQUAL_UINT32(77U, request.expected_boot_id);
    TEST_ASSERT_EQUAL_UINT32(4U, request.expected_topology_generation);
}

void test_usb_maintenance_parser_rejects_wildcards_ambiguity_and_bad_binding(void)
{
    static const char *invalid[] = {
        "FOF_BACKEND_OTA_STATUS extra",
        "FOF_BACKEND_OTA_PROBE scanner0 scanner-s3-combo-fof_badge *",
        "FOF_BACKEND_OTA_PROBE uplink scanner-s3-combo-backend *",
        "FOF_BACKEND_OTA_APPLY uplink * newer-only AA:BB:CC:DD:EE:FF 1 1",
        "FOF_BACKEND_OTA_APPLY uplink 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef newer-only aa:bb:cc:dd:ee:ff 1 1",
        "FOF_BACKEND_OTA_APPLY uplink 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef newer-only AA:BB:CC:DD:EE:FF 0 1",
        "FOF_BACKEND_OTA_APPLY uplink 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef newer-only AA:BB:CC:DD:EE:FF 1 0",
        "FOF_BACKEND_OTA_APPLY  uplink 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef newer-only AA:BB:CC:DD:EE:FF 1 1",
        " FOF_BACKEND_OTA_STATUS",
    };
    backend_ota_request_t request;
    for (size_t index = 0U; index < sizeof(invalid) / sizeof(invalid[0]);
         ++index) {
        TEST_ASSERT_FALSE_MESSAGE(
            backend_ota_maintenance_parse_usb(
                invalid[index], strlen(invalid[index]), &request),
            invalid[index]);
    }
    TEST_ASSERT_TRUE(backend_ota_maintenance_is_status_usb(
        "FOF_BACKEND_OTA_STATUS", strlen("FOF_BACKEND_OTA_STATUS")));
    TEST_ASSERT_FALSE(backend_ota_maintenance_is_status_usb(
        "FOF_BACKEND_OTA_STATUS\n\n",
        strlen("FOF_BACKEND_OTA_STATUS\n\n")));
}

void test_target_binding_match_requires_component_slot_mac_boot_and_topology(void)
{
    backend_ota_request_t request = {0};
    request.component = BACKEND_OTA_COMPONENT_SCANNER1;
    request.expected_boot_id = 77U;
    request.expected_topology_generation = 4U;
    const uint8_t mac[6] = {0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0x01U};
    memcpy(request.expected_mac, mac, sizeof(mac));
    backend_ota_target_binding_t actual = {
        .component = BACKEND_OTA_COMPONENT_SCANNER1,
        .component_slot = 1,
        .target_boot_id = 77U,
        .topology_generation = 4U,
    };
    memcpy(actual.target_mac, mac, sizeof(mac));
    TEST_ASSERT_TRUE(backend_ota_target_binding_matches(&request, &actual));
    actual.component_slot = 0;
    TEST_ASSERT_FALSE(backend_ota_target_binding_matches(&request, &actual));
    actual.component_slot = 1;
    actual.target_boot_id++;
    TEST_ASSERT_FALSE(backend_ota_target_binding_matches(&request, &actual));
    actual.target_boot_id = 77U;
    actual.topology_generation++;
    TEST_ASSERT_FALSE(backend_ota_target_binding_matches(&request, &actual));
    actual.topology_generation = 4U;
    actual.target_mac[5] ^= 1U;
    TEST_ASSERT_FALSE(backend_ota_target_binding_matches(&request, &actual));
}

void test_evidence_encoder_has_exact_prefix_key_order_and_canonical_values(void)
{
    backend_ota_evidence_t evidence;
    memset(&evidence, 0, sizeof(evidence));
    evidence.operation_id = 7U;
    evidence.probe = false;
    evidence.component = BACKEND_OTA_COMPONENT_SCANNER0;
    evidence.apply_mode = BACKEND_OTA_SAME_VERSION_RECOVERY;
    evidence.component_slot = 0;
    const uint8_t uplink_mac[6] = {
        0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU,
    };
    const uint8_t target_mac[6] = {
        0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0x01U,
    };
    memcpy(evidence.uplink_mac, uplink_mac, 6U);
    memcpy(evidence.expected_target_mac, target_mac, 6U);
    memcpy(evidence.actual_target_mac, target_mac, 6U);
    evidence.expected_target_boot_id = UINT32_C(0x12345678);
    evidence.actual_target_boot_id = UINT32_C(0x12345678);
    evidence.expected_topology_generation = 4U;
    evidence.actual_topology_generation = 4U;
    strcpy(evidence.catalog_name, FOF_BACKEND_SCANNER_TARGET);
    evidence.manifest = valid_uplink_manifest();
    strcpy(evidence.manifest.target, FOF_BACKEND_SCANNER_TARGET);
    strcpy(evidence.manifest.project, FOF_BACKEND_SCANNER_PROJECT);
    evidence.manifest.allow_same_version = true;
    evidence.decision = BACKEND_OTA_DECISION_APPLIED;
    evidence.partition_capacity = UINT32_C(0x200000);
    evidence.complete_image_validated = true;
    evidence.image_writes_before = 11U;
    evidence.image_writes_after = 15U;
    evidence.boot_id_before = UINT32_C(0x12345678);
    evidence.boot_id_after = UINT32_C(0x87654321);
    evidence.rollback_clear = true;
    evidence.converged = true;

    char output[2048];
    size_t length = backend_ota_evidence_encode(
        &evidence, output, sizeof(output));
    TEST_ASSERT_GREATER_THAN(0U, length);
    TEST_ASSERT_EQUAL_STRING(
        "FOF_BACKEND_OTA_EVIDENCE {\"schema\":1,\"operation_id\":7,"
        "\"mode\":\"same-version-recovery\",\"component\":\"scanner0\","
        "\"component_slot\":0,\"uplink_mac\":\"AA:BB:CC:DD:EE:FF\","
        "\"expected_target_mac\":\"AA:BB:CC:DD:EE:01\","
        "\"actual_target_mac\":\"AA:BB:CC:DD:EE:01\","
        "\"expected_target_boot_id\":305419896,"
        "\"actual_target_boot_id\":305419896,"
        "\"expected_topology_generation\":4,"
        "\"actual_topology_generation\":4,"
        "\"catalog_name\":\"scanner-s3-combo-backend\","
        "\"target\":\"scanner-s3-combo-backend\","
        "\"project\":\"fof_backend_scanner\","
        "\"hardware\":\"seeed_xiao_esp32s3\","
        "\"version\":\"0.1.1-backend\","
        "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
        "\"crc32\":305419896,\"size\":8,\"partition_capacity\":2097152,"
        "\"allow_same_version\":true,\"decision\":\"applied\","
        "\"complete_image_validated\":true,\"image_writes_before\":11,"
        "\"image_writes_after\":15,\"boot_id_before\":305419896,"
        "\"boot_id_after\":2271560481,\"rollback_clear\":true,"
        "\"converged\":true}",
        output);
}

void test_probe_validates_complete_scanner_image_without_mutation(void)
{
    static maintenance_fixture_t fixture;
    backend_ota_maintenance_t state;
    backend_firmware_buffer_t buffer;
    maintenance_fixture_init(
        &fixture, &state, &buffer, BACKEND_OTA_COMPONENT_SCANNER0,
        "0.1.1-backend", "0.1.0-backend");
    backend_ota_evidence_t evidence;

    TEST_ASSERT_TRUE(backend_ota_maintenance_run_probe(
        &state, BACKEND_OTA_COMPONENT_SCANNER0,
        FOF_BACKEND_SCANNER_TARGET, NULL, &evidence));
    TEST_ASSERT_EQUAL(BACKEND_OTA_DECISION_ADMIT, evidence.decision);
    TEST_ASSERT_TRUE(evidence.complete_image_validated);
    TEST_ASSERT_EQUAL_UINT32(
        evidence.image_writes_before, evidence.image_writes_after);
    TEST_ASSERT_EQUAL_UINT32(
        evidence.boot_id_before, evidence.boot_id_after);
    TEST_ASSERT_EQUAL_UINT(1U, fixture.metadata_calls);
    TEST_ASSERT_EQUAL_UINT(1U, fixture.download_calls);
    TEST_ASSERT_EQUAL_UINT(1U, fixture.scanner_dry_run_calls);
    TEST_ASSERT_EQUAL_UINT(0U, fixture.mutate_calls);
    TEST_ASSERT_FALSE(fixture.has_journal);
    TEST_ASSERT_EQUAL_UINT(0U, fixture.accepted_emits);
    TEST_ASSERT_EQUAL_UINT(1U, fixture.evidence_emits);
    TEST_ASSERT_FALSE(buffer.acquired);
}

void test_cross_family_probe_rejects_before_metadata_download_or_write(void)
{
    static maintenance_fixture_t fixture;
    backend_ota_maintenance_t state;
    backend_firmware_buffer_t buffer;
    maintenance_fixture_init(
        &fixture, &state, &buffer, BACKEND_OTA_COMPONENT_SCANNER0,
        "0.1.1-backend", "0.1.0-backend");
    backend_ota_evidence_t evidence;

    TEST_ASSERT_TRUE(backend_ota_maintenance_run_probe(
        &state, BACKEND_OTA_COMPONENT_SCANNER0,
        "scanner-s3-combo-fof_badge", NULL, &evidence));
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_DECISION_REJECT_IDENTITY, evidence.decision);
    TEST_ASSERT_EQUAL_UINT(0U, fixture.metadata_calls);
    TEST_ASSERT_EQUAL_UINT(0U, fixture.download_calls);
    TEST_ASSERT_EQUAL_UINT(0U, fixture.mutate_calls);
    TEST_ASSERT_FALSE(fixture.has_journal);
}

void test_probe_rejects_scanner_reboot_or_slot_change_during_dry_run(void)
{
    const backend_ota_component_t components[] = {
        BACKEND_OTA_COMPONENT_SCANNER0,
        BACKEND_OTA_COMPONENT_SCANNER1,
    };
    static maintenance_fixture_t fixture;
    for (size_t index = 0U;
         index < sizeof(components) / sizeof(components[0]); ++index) {
        backend_ota_maintenance_t state;
        backend_firmware_buffer_t buffer;
        maintenance_fixture_init(
            &fixture, &state, &buffer, components[index],
            "0.1.1-backend", "0.1.0-backend");
        fixture.change_binding_during_dry_run = (uint8_t)(index + 1U);
        backend_ota_evidence_t evidence;
        TEST_ASSERT_TRUE(backend_ota_maintenance_run_probe(
            &state, components[index], FOF_BACKEND_SCANNER_TARGET,
            NULL, &evidence));
        TEST_ASSERT_EQUAL(
            BACKEND_OTA_DECISION_REJECT_TARGET_BINDING,
            evidence.decision);
        TEST_ASSERT_EQUAL_UINT32(77U, evidence.expected_target_boot_id);
        TEST_ASSERT_EQUAL_UINT32(
            index == 0U ? 78U : 77U,
            evidence.actual_target_boot_id);
        TEST_ASSERT_EQUAL_UINT(0U, fixture.mutate_calls);
        TEST_ASSERT_FALSE(fixture.has_journal);
    }
}

void test_same_version_apply_requires_explicit_recovery_mode(void)
{
    static maintenance_fixture_t fixture;
    backend_ota_maintenance_t state;
    backend_firmware_buffer_t buffer;
    maintenance_fixture_init(
        &fixture, &state, &buffer, BACKEND_OTA_COMPONENT_SCANNER0,
        "0.1.0-backend", "0.1.0-backend");
    backend_ota_request_t request = maintenance_apply_request(
        &fixture, BACKEND_OTA_COMPONENT_SCANNER0, BACKEND_OTA_NEWER_ONLY);

    TEST_ASSERT_FALSE(backend_ota_maintenance_request_apply(&state, &request));
    backend_ota_evidence_t evidence;
    TEST_ASSERT_TRUE(backend_ota_maintenance_last_evidence(&state, &evidence));
    TEST_ASSERT_EQUAL(BACKEND_OTA_DECISION_NO_UPDATE, evidence.decision);
    TEST_ASSERT_EQUAL_UINT(0U, fixture.download_calls);
    TEST_ASSERT_EQUAL_UINT(0U, fixture.mutate_calls);

    request.apply_mode = BACKEND_OTA_SAME_VERSION_RECOVERY;
    TEST_ASSERT_TRUE(backend_ota_maintenance_request_apply(&state, &request));
    TEST_ASSERT_EQUAL_UINT(1U, fixture.download_calls);
    TEST_ASSERT_EQUAL_UINT(1U, fixture.mutate_calls);
    TEST_ASSERT_EQUAL_UINT(1U, fixture.reboot_calls);
    TEST_ASSERT_EQUAL_UINT(1U, fixture.accepted_emits);
    TEST_ASSERT_TRUE(fixture.has_journal);
}

void test_apply_binding_drift_rejects_before_journal_acceptance_or_mutation(void)
{
    const backend_ota_component_t components[] = {
        BACKEND_OTA_COMPONENT_UPLINK,
        BACKEND_OTA_COMPONENT_SCANNER0,
        BACKEND_OTA_COMPONENT_SCANNER1,
    };
    static maintenance_fixture_t fixture;
    for (size_t index = 0U;
         index < sizeof(components) / sizeof(components[0]); ++index) {
        for (uint8_t drift = 1U; drift <= 4U; ++drift) {
            backend_ota_maintenance_t state;
            backend_firmware_buffer_t buffer;
            maintenance_fixture_init(
                &fixture, &state, &buffer, components[index],
                "0.1.1-backend", "0.1.0-backend");
            fixture.drift_binding = drift;
            backend_ota_request_t request = maintenance_apply_request(
                &fixture, components[index], BACKEND_OTA_NEWER_ONLY);

            TEST_ASSERT_FALSE(
                backend_ota_maintenance_request_apply(&state, &request));
            backend_ota_evidence_t evidence;
            TEST_ASSERT_TRUE(
                backend_ota_maintenance_last_evidence(&state, &evidence));
            TEST_ASSERT_EQUAL(
                BACKEND_OTA_DECISION_REJECT_TARGET_BINDING,
                evidence.decision);
            TEST_ASSERT_EQUAL_UINT(1U, fixture.download_calls);
            TEST_ASSERT_EQUAL_UINT(0U, fixture.mutate_calls);
            TEST_ASSERT_EQUAL_UINT(0U, fixture.accepted_emits);
            TEST_ASSERT_FALSE(fixture.has_journal);
            TEST_ASSERT_EQUAL_UINT32(0U, evidence.image_writes_after);
            TEST_ASSERT_FALSE(buffer.acquired);
        }
    }
}

void test_matching_apply_persists_and_flushes_before_first_mutation(void)
{
    static maintenance_fixture_t fixture;
    backend_ota_maintenance_t state;
    backend_firmware_buffer_t buffer;
    maintenance_fixture_init(
        &fixture, &state, &buffer, BACKEND_OTA_COMPONENT_SCANNER1,
        "0.1.1-backend", "0.1.0-backend");
    backend_ota_request_t request = maintenance_apply_request(
        &fixture, BACKEND_OTA_COMPONENT_SCANNER1, BACKEND_OTA_NEWER_ONLY);

    TEST_ASSERT_TRUE(backend_ota_maintenance_request_apply(&state, &request));
    char sequence[65];
    memcpy(sequence, fixture.events, fixture.event_count);
    sequence[fixture.event_count] = '\0';
    TEST_ASSERT_EQUAL_STRING("LABWDURQE", sequence);
    backend_ota_journal_record_t record;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_VALID,
        backend_ota_journal_decode(
            fixture.journal, fixture.journal_length, &record));
    TEST_ASSERT_EQUAL(BACKEND_OTA_PHASE_REBOOT_PENDING, record.phase);
    TEST_ASSERT_EQUAL_UINT32(1U, record.image_writes_after);
    TEST_ASSERT_TRUE(buffer.acquired);
}

void test_reboot_resume_checks_convergence_without_repeating_write(void)
{
    static maintenance_fixture_t fixture;
    backend_ota_maintenance_t state;
    backend_firmware_buffer_t buffer;
    maintenance_fixture_init(
        &fixture, &state, &buffer, BACKEND_OTA_COMPONENT_SCANNER1,
        "0.1.1-backend", "0.1.0-backend");
    backend_ota_request_t request = maintenance_apply_request(
        &fixture, BACKEND_OTA_COMPONENT_SCANNER1, BACKEND_OTA_NEWER_ONLY);
    TEST_ASSERT_TRUE(backend_ota_maintenance_request_apply(&state, &request));
    const unsigned writes_before_resume = fixture.mutate_calls;
    fixture.event_count = 0U;

    TEST_ASSERT_TRUE(backend_ota_maintenance_resume(&state, false));
    TEST_ASSERT_EQUAL_UINT(writes_before_resume, fixture.mutate_calls);
    backend_ota_journal_record_t record;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_VALID,
        backend_ota_journal_decode(
            fixture.journal, fixture.journal_length, &record));
    TEST_ASSERT_EQUAL(BACKEND_OTA_PHASE_COMPLETE, record.phase);
    TEST_ASSERT_TRUE(record.rollback_clear);
    TEST_ASSERT_TRUE(record.converged);
    TEST_ASSERT_EQUAL_UINT32(78U, record.boot_id_after);
    TEST_ASSERT_FALSE(buffer.acquired);
    backend_ota_evidence_t evidence;
    TEST_ASSERT_TRUE(backend_ota_maintenance_last_evidence(&state, &evidence));
    TEST_ASSERT_EQUAL(BACKEND_OTA_DECISION_APPLIED, evidence.decision);
    TEST_ASSERT_TRUE(evidence.rollback_clear);
    TEST_ASSERT_TRUE(evidence.converged);
}

void test_accepted_encoder_is_exact_and_rejects_mismatched_binding(void)
{
    backend_ota_journal_record_t record;
    memset(&record, 0, sizeof(record));
    record.schema = BACKEND_OTA_JOURNAL_SCHEMA;
    record.operation_id = 7U;
    record.component = BACKEND_OTA_COMPONENT_SCANNER0;
    record.component_slot = 0;
    record.apply_mode = BACKEND_OTA_NEWER_ONLY;
    strcpy(record.catalog_name, FOF_BACKEND_SCANNER_TARGET);
    record.manifest = valid_uplink_manifest();
    strcpy(record.manifest.target, FOF_BACKEND_SCANNER_TARGET);
    strcpy(record.manifest.project, FOF_BACKEND_SCANNER_PROJECT);
    record.phase = BACKEND_OTA_PHASE_ACCEPTED;
    const uint8_t uplink_mac[6] = {
        0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU,
    };
    const uint8_t target_mac[6] = {
        0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0x01U,
    };
    memcpy(record.uplink_mac, uplink_mac, 6U);
    record.uplink_boot_id = UINT32_C(0x12345678);
    memcpy(record.expected_target_mac, target_mac, 6U);
    memcpy(record.actual_target_mac, target_mac, 6U);
    record.expected_target_boot_id = UINT32_C(0x12345678);
    record.actual_target_boot_id = UINT32_C(0x12345678);
    record.expected_topology_generation = 4U;
    record.actual_topology_generation = 4U;

    char output[1024];
    TEST_ASSERT_GREATER_THAN(0U, backend_ota_accepted_encode(
        &record, output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING(
        "FOF_BACKEND_OTA_ACCEPTED {\"schema\":1,\"operation_id\":7,"
        "\"component\":\"scanner0\",\"component_slot\":0,"
        "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
        "\"crc32\":305419896,\"uplink_mac\":\"AA:BB:CC:DD:EE:FF\","
        "\"expected_target_mac\":\"AA:BB:CC:DD:EE:01\","
        "\"actual_target_mac\":\"AA:BB:CC:DD:EE:01\","
        "\"expected_target_boot_id\":305419896,"
        "\"actual_target_boot_id\":305419896,"
        "\"expected_topology_generation\":4,"
        "\"actual_topology_generation\":4,"
        "\"boot_id_before\":305419896}",
        output);
    record.actual_target_boot_id++;
    TEST_ASSERT_EQUAL_UINT(0U, backend_ota_accepted_encode(
        &record, output, sizeof(output)));
}

void test_catalog_poll_is_immediate_then_30_minutes_with_exact_backoff(void)
{
    backend_ota_poll_state_t poll;
    backend_ota_poll_init(&poll, 100U);
    TEST_ASSERT_TRUE(backend_ota_poll_due(&poll, 100U));
    static const uint32_t delays[] = {
        5000U, 10000U, 20000U, 40000U,
        80000U, 160000U, 300000U, 300000U,
    };
    int64_t now = 100;
    for (size_t index = 0U; index < sizeof(delays) / sizeof(delays[0]);
         ++index) {
        backend_ota_poll_note_failure(&poll, now);
        TEST_ASSERT_EQUAL_INT64(now + delays[index], poll.next_due_ms);
        TEST_ASSERT_FALSE(backend_ota_poll_due(&poll, poll.next_due_ms - 1));
        TEST_ASSERT_TRUE(backend_ota_poll_due(&poll, poll.next_due_ms));
        now = poll.next_due_ms;
    }
    backend_ota_poll_note_success(&poll, now);
    TEST_ASSERT_EQUAL_INT64(now + INT64_C(1800000), poll.next_due_ms);
    TEST_ASSERT_EQUAL_UINT8(0U, poll.failure_count);
}

void test_auto_update_disabled_is_read_only_and_never_allows_recovery(void)
{
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_AUTO_READ_ONLY_UPDATE_AVAILABLE,
        backend_ota_auto_policy(false, FOF_VERSION_NEWER));
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_AUTO_APPLY_NEWER,
        backend_ota_auto_policy(true, FOF_VERSION_NEWER));
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_AUTO_NO_UPDATE,
        backend_ota_auto_policy(true, FOF_VERSION_EQUAL));
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_AUTO_REJECT_VERSION,
        backend_ota_auto_policy(true, FOF_VERSION_UNORDERED));
}

void test_automatic_catalog_poll_downloads_only_after_explicit_enable(void)
{
    static maintenance_fixture_t fixture;
    backend_ota_maintenance_t state;
    backend_firmware_buffer_t buffer;
    maintenance_fixture_init(
        &fixture, &state, &buffer, BACKEND_OTA_COMPONENT_SCANNER0,
        "0.1.1-backend", "0.1.0-backend");
    backend_ota_auto_decision_t decision = BACKEND_OTA_AUTO_REJECT_VERSION;

    TEST_ASSERT_TRUE(backend_ota_maintenance_auto_poll(
        &state, BACKEND_OTA_COMPONENT_SCANNER0, false, &decision));
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_AUTO_READ_ONLY_UPDATE_AVAILABLE, decision);
    TEST_ASSERT_EQUAL_UINT(1U, fixture.metadata_calls);
    TEST_ASSERT_EQUAL_UINT(0U, fixture.download_calls);
    TEST_ASSERT_EQUAL_UINT(0U, fixture.mutate_calls);
    TEST_ASSERT_FALSE(buffer.acquired);

    TEST_ASSERT_TRUE(backend_ota_maintenance_auto_poll(
        &state, BACKEND_OTA_COMPONENT_SCANNER0, true, &decision));
    TEST_ASSERT_EQUAL(BACKEND_OTA_AUTO_APPLY_NEWER, decision);
    TEST_ASSERT_EQUAL_UINT(1U, fixture.download_calls);
    TEST_ASSERT_EQUAL_UINT(1U, fixture.mutate_calls);
    TEST_ASSERT_EQUAL_UINT(1U, fixture.reboot_calls);
}

void test_missing_psram_arena_exposes_unavailable_and_performs_no_download(void)
{
    static maintenance_fixture_t fixture;
    backend_ota_maintenance_t state;
    backend_firmware_buffer_t buffer;
    memset(&fixture, 0, sizeof(fixture));
    fixture.running_version = "0.1.0-backend";
    maintenance_metadata(
        &fixture, BACKEND_OTA_COMPONENT_SCANNER0, "0.1.1-backend");
    fixture.binding.component = BACKEND_OTA_COMPONENT_SCANNER0;
    fixture.binding.component_slot = 0;
    const uint8_t target_mac[6] = {
        0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0x01U,
    };
    memcpy(fixture.binding.target_mac, target_mac, 6U);
    fixture.binding.target_boot_id = 77U;
    fixture.binding.topology_generation = 4U;
    memset(&buffer, 0, sizeof(buffer));
    const backend_ota_maintenance_adapters_t adapters =
        maintenance_adapters(&fixture);
    const backend_ota_journal_storage_t journal = {
        .context = &fixture,
        .load = maintenance_journal_load,
        .store = maintenance_journal_store,
    };
    const uint8_t uplink_mac[6] = {
        0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU,
    };
    TEST_ASSERT_TRUE(backend_ota_maintenance_init(
        &state, &adapters, &journal, &buffer, uplink_mac, 100U));
    TEST_ASSERT_FALSE(backend_ota_maintenance_available(&state));

    backend_ota_evidence_t evidence;
    TEST_ASSERT_TRUE(backend_ota_maintenance_run_probe(
        &state, BACKEND_OTA_COMPONENT_SCANNER0,
        FOF_BACKEND_SCANNER_TARGET, NULL, &evidence));
    TEST_ASSERT_EQUAL(BACKEND_OTA_DECISION_FAILED, evidence.decision);
    TEST_ASSERT_EQUAL_UINT(0U, fixture.download_calls);
    TEST_ASSERT_EQUAL_UINT(0U, fixture.mutate_calls);
}

void test_digest_change_and_parallel_operation_fail_closed(void)
{
    static maintenance_fixture_t fixture;
    backend_ota_maintenance_t state;
    backend_firmware_buffer_t buffer;
    maintenance_fixture_init(
        &fixture, &state, &buffer, BACKEND_OTA_COMPONENT_SCANNER0,
        "0.1.1-backend", "0.1.0-backend");
    backend_ota_request_t request = maintenance_apply_request(
        &fixture, BACKEND_OTA_COMPONENT_SCANNER0, BACKEND_OTA_NEWER_ONLY);
    request.expected_sha256[0] = 'f';
    TEST_ASSERT_FALSE(backend_ota_maintenance_request_apply(&state, &request));
    backend_ota_evidence_t evidence;
    TEST_ASSERT_TRUE(backend_ota_maintenance_last_evidence(&state, &evidence));
    TEST_ASSERT_EQUAL(BACKEND_OTA_DECISION_REJECT_DIGEST, evidence.decision);
    TEST_ASSERT_EQUAL_UINT(0U, fixture.download_calls);
    TEST_ASSERT_FALSE(fixture.has_journal);

    request.expected_sha256[0] = '0';
    TEST_ASSERT_TRUE(backend_ota_maintenance_request_apply(&state, &request));
    const unsigned mutations = fixture.mutate_calls;
    backend_ota_request_t recovery = request;
    recovery.apply_mode = BACKEND_OTA_SAME_VERSION_RECOVERY;
    TEST_ASSERT_FALSE(
        backend_ota_maintenance_request_apply(&state, &recovery));
    TEST_ASSERT_TRUE(backend_ota_maintenance_last_evidence(&state, &evidence));
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_SAME_VERSION_RECOVERY, evidence.apply_mode);
    TEST_ASSERT_EQUAL(BACKEND_OTA_DECISION_REJECT_BUSY, evidence.decision);
    char busy_line[2048];
    TEST_ASSERT_GREATER_THAN(0U, backend_ota_evidence_encode(
        &evidence, busy_line, sizeof(busy_line)));
    TEST_ASSERT_NOT_NULL(strstr(
        busy_line, "\"mode\":\"same-version-recovery\""));
    backend_ota_evidence_t probe;
    TEST_ASSERT_TRUE(backend_ota_maintenance_run_probe(
        &state, BACKEND_OTA_COMPONENT_SCANNER0,
        FOF_BACKEND_SCANNER_TARGET, NULL, &probe));
    TEST_ASSERT_EQUAL(BACKEND_OTA_DECISION_REJECT_BUSY, probe.decision);
    TEST_ASSERT_EQUAL_UINT(mutations, fixture.mutate_calls);
}

void test_apply_uses_immutable_request_copy_across_download_callbacks(void)
{
    static maintenance_fixture_t fixture;
    backend_ota_maintenance_t state;
    backend_firmware_buffer_t buffer;
    maintenance_fixture_init(
        &fixture, &state, &buffer, BACKEND_OTA_COMPONENT_SCANNER0,
        "0.1.1-backend", "0.1.0-backend");
    backend_ota_request_t request = maintenance_apply_request(
        &fixture, BACKEND_OTA_COMPONENT_SCANNER0, BACKEND_OTA_NEWER_ONLY);
    fixture.request_to_mutate = &request;

    TEST_ASSERT_TRUE(backend_ota_maintenance_request_apply(&state, &request));
    TEST_ASSERT_EQUAL(BACKEND_OTA_COMPONENT_UPLINK, request.component);
    TEST_ASSERT_EQUAL_UINT32(78U, request.expected_boot_id);
    TEST_ASSERT_EQUAL_UINT(1U, fixture.mutate_calls);
    backend_ota_journal_record_t record;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_VALID,
        backend_ota_journal_decode(
            fixture.journal, fixture.journal_length, &record));
    TEST_ASSERT_EQUAL(BACKEND_OTA_COMPONENT_SCANNER0, record.component);
    TEST_ASSERT_EQUAL_UINT32(77U, record.expected_target_boot_id);
    TEST_ASSERT_EQUAL_UINT8('0', record.manifest.sha256[0]);
}

void test_power_cut_after_writing_never_replays_image_mutation(void)
{
    static maintenance_fixture_t fixture;
    backend_ota_maintenance_t state;
    backend_firmware_buffer_t buffer;
    maintenance_fixture_init(
        &fixture, &state, &buffer, BACKEND_OTA_COMPONENT_SCANNER1,
        "0.1.1-backend", "0.1.0-backend");
    backend_ota_request_t request = maintenance_apply_request(
        &fixture, BACKEND_OTA_COMPONENT_SCANNER1, BACKEND_OTA_NEWER_ONLY);
    TEST_ASSERT_TRUE(backend_ota_maintenance_request_apply(&state, &request));
    TEST_ASSERT_EQUAL_UINT(1U, fixture.mutate_calls);

    backend_ota_journal_record_t record;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_VALID,
        backend_ota_journal_decode(
            fixture.journal, fixture.journal_length, &record));
    record.phase = BACKEND_OTA_PHASE_WRITING;
    record.image_writes_after = record.image_writes_before;
    record.boot_id_after = 0U;
    backend_ota_journal_blob_t writing;
    TEST_ASSERT_TRUE(backend_ota_journal_encode(&record, &writing));
    memcpy(fixture.journal, writing.bytes, writing.length);
    fixture.journal_length = writing.length;

    backend_ota_maintenance_t restarted;
    backend_firmware_buffer_t restarted_buffer;
    memset(&restarted_buffer, 0, sizeof(restarted_buffer));
    TEST_ASSERT_TRUE(backend_firmware_buffer_init_once(
        &restarted_buffer, maintenance_alloc, &fixture));
    const backend_ota_maintenance_adapters_t adapters =
        maintenance_adapters(&fixture);
    const backend_ota_journal_storage_t journal = {
        .context = &fixture,
        .load = maintenance_journal_load,
        .store = maintenance_journal_store,
    };
    const uint8_t uplink_mac[6] = {
        0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU,
    };
    TEST_ASSERT_TRUE(backend_ota_maintenance_init(
        &restarted, &adapters, &journal, &restarted_buffer,
        uplink_mac, 100U));
    TEST_ASSERT_TRUE(backend_ota_maintenance_resume(&restarted, false));
    TEST_ASSERT_EQUAL_UINT(1U, fixture.mutate_calls);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_VALID,
        backend_ota_journal_decode(
            fixture.journal, fixture.journal_length, &record));
    TEST_ASSERT_EQUAL(BACKEND_OTA_PHASE_COMPLETE, record.phase);
}

void test_resume_never_advertises_failed_until_failed_record_is_durable(void)
{
    static maintenance_fixture_t fixture;
    backend_ota_maintenance_t state;
    backend_firmware_buffer_t buffer;
    maintenance_fixture_init(
        &fixture, &state, &buffer, BACKEND_OTA_COMPONENT_SCANNER1,
        "0.1.1-backend", "0.1.0-backend");
    backend_ota_request_t request = maintenance_apply_request(
        &fixture, BACKEND_OTA_COMPONENT_SCANNER1, BACKEND_OTA_NEWER_ONLY);
    TEST_ASSERT_TRUE(backend_ota_maintenance_request_apply(&state, &request));

    backend_ota_journal_record_t record;
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_VALID,
        backend_ota_journal_decode(
            fixture.journal, fixture.journal_length, &record));
    record.phase = BACKEND_OTA_PHASE_ACCEPTED;
    record.image_writes_after = record.image_writes_before;
    record.boot_id_after = 0U;
    backend_ota_journal_blob_t accepted;
    TEST_ASSERT_TRUE(backend_ota_journal_encode(&record, &accepted));
    memcpy(fixture.journal, accepted.bytes, accepted.length);
    fixture.journal_length = accepted.length;
    fixture.evidence_emits = 0U;
    fixture.fail_journal_store = true;

    backend_ota_maintenance_t restarted;
    backend_firmware_buffer_t restarted_buffer;
    memset(&restarted_buffer, 0, sizeof(restarted_buffer));
    TEST_ASSERT_TRUE(backend_firmware_buffer_init_once(
        &restarted_buffer, maintenance_alloc, &fixture));
    const backend_ota_maintenance_adapters_t adapters =
        maintenance_adapters(&fixture);
    const backend_ota_journal_storage_t journal = {
        .context = &fixture,
        .load = maintenance_journal_load,
        .store = maintenance_journal_store,
    };
    const uint8_t uplink_mac[6] = {
        0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU,
    };
    TEST_ASSERT_TRUE(backend_ota_maintenance_init(
        &restarted, &adapters, &journal, &restarted_buffer,
        uplink_mac, 100U));
    TEST_ASSERT_FALSE(backend_ota_maintenance_resume(&restarted, true));
    TEST_ASSERT_TRUE(restarted.busy);
    TEST_ASSERT_EQUAL_UINT(0U, fixture.evidence_emits);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_VALID,
        backend_ota_journal_decode(
            fixture.journal, fixture.journal_length, &record));
    TEST_ASSERT_EQUAL(BACKEND_OTA_PHASE_ACCEPTED, record.phase);

    fixture.fail_journal_store = false;
    TEST_ASSERT_FALSE(backend_ota_maintenance_resume(&restarted, true));
    TEST_ASSERT_FALSE(restarted.busy);
    TEST_ASSERT_EQUAL_UINT(1U, fixture.evidence_emits);
    TEST_ASSERT_EQUAL(
        BACKEND_OTA_JOURNAL_VALID,
        backend_ota_journal_decode(
            fixture.journal, fixture.journal_length, &record));
    TEST_ASSERT_EQUAL(BACKEND_OTA_PHASE_FAILED, record.phase);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_self_ota_rejects_non_uplink_identity_before_mutation);
    RUN_TEST(
        test_self_ota_writes_exact_image_then_readies_separately_gated_reboot);
    RUN_TEST(test_self_ota_failure_never_selects_boot_partition);
    RUN_TEST(test_self_ota_rollback_clear_requires_local_runtime_not_backend);
    RUN_TEST(test_usb_probe_and_apply_parse_only_exact_backend_commands);
    RUN_TEST(
        test_usb_maintenance_parser_rejects_wildcards_ambiguity_and_bad_binding);
    RUN_TEST(
        test_target_binding_match_requires_component_slot_mac_boot_and_topology);
    RUN_TEST(
        test_evidence_encoder_has_exact_prefix_key_order_and_canonical_values);
    RUN_TEST(test_probe_validates_complete_scanner_image_without_mutation);
    RUN_TEST(
        test_cross_family_probe_rejects_before_metadata_download_or_write);
    RUN_TEST(
        test_probe_rejects_scanner_reboot_or_slot_change_during_dry_run);
    RUN_TEST(test_same_version_apply_requires_explicit_recovery_mode);
    RUN_TEST(
        test_apply_binding_drift_rejects_before_journal_acceptance_or_mutation);
    RUN_TEST(
        test_matching_apply_persists_and_flushes_before_first_mutation);
    RUN_TEST(
        test_reboot_resume_checks_convergence_without_repeating_write);
    RUN_TEST(
        test_accepted_encoder_is_exact_and_rejects_mismatched_binding);
    RUN_TEST(
        test_catalog_poll_is_immediate_then_30_minutes_with_exact_backoff);
    RUN_TEST(
        test_auto_update_disabled_is_read_only_and_never_allows_recovery);
    RUN_TEST(
        test_automatic_catalog_poll_downloads_only_after_explicit_enable);
    RUN_TEST(
        test_missing_psram_arena_exposes_unavailable_and_performs_no_download);
    RUN_TEST(test_digest_change_and_parallel_operation_fail_closed);
    RUN_TEST(
        test_apply_uses_immutable_request_copy_across_download_callbacks);
    RUN_TEST(test_power_cut_after_writing_never_replays_image_mutation);
    RUN_TEST(
        test_resume_never_advertises_failed_until_failed_record_is_durable);
    return UNITY_END();
}
