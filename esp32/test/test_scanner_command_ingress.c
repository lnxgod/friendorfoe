#include "unity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "scanner_command_ingress.h"

typedef struct {
    bool binary_mode;
    bool ota_begin_succeeds;
    size_t authorized_count;
    fof_scanner_command_decision_t decisions[4];
    bool authorized_was_terminated;
    uint8_t binary[64];
    size_t binary_len;
} ingress_fake_t;

static bool fake_binary_active(void *context)
{
    return ((ingress_fake_t *)context)->binary_mode;
}

static bool fake_binary_write(void *context,
                              const uint8_t *bytes,
                              size_t byte_len)
{
    ingress_fake_t *fake = context;
    TEST_ASSERT_LESS_OR_EQUAL_UINT(
        sizeof(fake->binary) - fake->binary_len, byte_len);
    memcpy(fake->binary + fake->binary_len, bytes, byte_len);
    fake->binary_len += byte_len;
    return true;
}

static void fake_authorized(void *context,
                            const fof_scanner_command_decision_t *decision,
                            const uint8_t *bytes,
                            size_t byte_len)
{
    ingress_fake_t *fake = context;
    TEST_ASSERT_LESS_THAN_UINT(4U, fake->authorized_count);
    fake->decisions[fake->authorized_count++] = *decision;
    fake->authorized_was_terminated = bytes[byte_len] == '\0';
    if (decision->route == FOF_SCANNER_COMMAND_ROUTE_FIRMWARE &&
        decision->firmware_schema_id ==
            FOF_FW_JSON_SCHEMA_SCANNER_OTA_BEGIN &&
        fake->ota_begin_succeeds) {
        fake->binary_mode = true;
    }
}

static void init_ingress(scanner_command_ingress_t *ingress,
                         ingress_fake_t *fake,
                         uint8_t storage[SCANNER_UART_LINE_BUFFER_SIZE])
{
    scanner_command_ingress_callbacks_t callbacks = {
        .binary_active = fake_binary_active,
        .binary_write = fake_binary_write,
        .authorized_frame = fake_authorized,
    };
    memset(fake, 0, sizeof(*fake));
    fake->ota_begin_succeeds = true;
    TEST_ASSERT_TRUE(scanner_command_ingress_init(
        ingress,
        storage,
        SCANNER_UART_LINE_BUFFER_SIZE,
        FOF_SCANNER_DEPLOYMENT_BADGE,
        &callbacks,
        fake));
}

void test_scanner_command_ingress_rejects_before_any_authorized_effect(void)
{
    static const char malformed[] =
        "{\"type\":\"scanner_quiet\",\"enabled\":true}\n";
    static const char legacy_mutations[] =
        "{\"type\":\"bootloader\"}\n{\"type\":\"ota\"}\n";
    static const char malformed_firmware[] =
        "{\"type\":\"ota_begin\"}\n";
    uint8_t storage[SCANNER_UART_LINE_BUFFER_SIZE];
    scanner_command_ingress_t ingress;
    ingress_fake_t fake;
    init_ingress(&ingress, &fake, storage);

    scanner_command_ingress_result_t result =
        scanner_command_ingress_consume(
            &ingress,
            (const uint8_t *)malformed,
            sizeof(malformed) - 1U);
    TEST_ASSERT_EQUAL_UINT(0U, result.authorized_frames);
    TEST_ASSERT_EQUAL_UINT(1U, result.rejected_frames);
    TEST_ASSERT_EQUAL_UINT(0U, fake.authorized_count);

    result = scanner_command_ingress_consume(
        &ingress,
        (const uint8_t *)legacy_mutations,
        sizeof(legacy_mutations) - 1U);
    TEST_ASSERT_EQUAL_UINT(0U, result.authorized_frames);
    TEST_ASSERT_EQUAL_UINT(2U, result.rejected_frames);
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_MUTATION_REFUSED,
        result.last_registry_result);
    TEST_ASSERT_EQUAL_UINT(0U, fake.authorized_count);

    result = scanner_command_ingress_consume(
        &ingress,
        (const uint8_t *)malformed_firmware,
        sizeof(malformed_firmware) - 1U);
    TEST_ASSERT_EQUAL_UINT(0U, result.authorized_frames);
    TEST_ASSERT_EQUAL_UINT(1U, result.rejected_frames);
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_REGISTRY_FIRMWARE_SCHEMA_REJECTED,
        result.last_registry_result);
    TEST_ASSERT_EQUAL_UINT(0U, fake.authorized_count);
}

void test_scanner_command_ingress_discards_bare_cr_and_overflow_suffixes(void)
{
    static const char bare_cr_wire[] =
        "{\"type\":\"stop\"}\rgarbage{\"type\":\"stop\"}\n"
        "{\"type\":\"ready\"}\n";
    uint8_t storage[SCANNER_UART_LINE_BUFFER_SIZE];
    scanner_command_ingress_t ingress;
    ingress_fake_t fake;
    init_ingress(&ingress, &fake, storage);

    scanner_command_ingress_result_t result =
        scanner_command_ingress_consume(
            &ingress,
            (const uint8_t *)bare_cr_wire,
            sizeof(bare_cr_wire) - 1U);
    TEST_ASSERT_EQUAL_UINT(1U, result.rejected_frames);
    TEST_ASSERT_EQUAL_UINT(1U, result.authorized_frames);
    TEST_ASSERT_EQUAL_UINT(1U, fake.authorized_count);
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_READY, fake.decisions[0].command.id);

    init_ingress(&ingress, &fake, storage);
    uint8_t overflow[SCANNER_UART_LINE_BUFFER_SIZE + 64U];
    memset(overflow, 'A', sizeof(overflow));
    static const char suffix[] =
        "{\"type\":\"stop\"}\n{\"type\":\"ready\"}\n";
    size_t suffix_offset = sizeof(overflow) - sizeof(suffix) + 1U;
    memcpy(overflow + suffix_offset, suffix, sizeof(suffix) - 1U);
    result = scanner_command_ingress_consume(
        &ingress, overflow, sizeof(overflow));
    TEST_ASSERT_EQUAL_UINT(1U, result.rejected_frames);
    TEST_ASSERT_EQUAL_UINT(1U, result.authorized_frames);
    TEST_ASSERT_EQUAL_UINT(1U, fake.authorized_count);
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_READY, fake.decisions[0].command.id);
}

void test_scanner_command_ingress_stale_partial_discards_through_next_lf(void)
{
    static const char partial[] = "{\"type\":\"sto";
    static const char suffix_then_ready[] =
        "p\"}\n{\"type\":\"ready\"}\n";
    uint8_t storage[SCANNER_UART_LINE_BUFFER_SIZE];
    scanner_command_ingress_t ingress;
    ingress_fake_t fake;
    init_ingress(&ingress, &fake, storage);

    scanner_command_ingress_result_t result =
        scanner_command_ingress_consume(
            &ingress,
            (const uint8_t *)partial,
            sizeof(partial) - 1U);
    TEST_ASSERT_TRUE(scanner_command_ingress_has_partial(&ingress));
    TEST_ASSERT_EQUAL_UINT(0U, result.rejected_frames);

    result = scanner_command_ingress_expire_partial(&ingress);
    TEST_ASSERT_EQUAL_UINT(1U, result.rejected_frames);
    TEST_ASSERT_EQUAL_INT(
        SCANNER_UART_LINE_REJECT_STALE_PARTIAL,
        result.last_line_reject);

    result = scanner_command_ingress_consume(
        &ingress,
        (const uint8_t *)suffix_then_ready,
        sizeof(suffix_then_ready) - 1U);
    TEST_ASSERT_EQUAL_UINT(1U, result.authorized_frames);
    TEST_ASSERT_EQUAL_UINT(1U, fake.authorized_count);
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_READY, fake.decisions[0].command.id);
}

void test_scanner_command_ingress_hands_same_read_ota_remainder_to_binary(void)
{
    static const char ota_begin[] =
        "{\"type\":\"ota_begin\",\"session_id\":\"s\","
        "\"size\":2,\"crc\":3,\"sha256\":\"aa\",\"target_ver\":\"1\","
        "\"fw_name\":\"scanner\",\"app_project\":\"fof_scanner\","
        "\"hardware_type\":\"esp32s3\",\"generation\":1,"
        "\"allow_same_version\":false}\n";
    static const uint8_t binary[] = {0x00, 0x0a, 0x0d, 0xff, 0x41};
    uint8_t wire[sizeof(ota_begin) - 1U + sizeof(binary)];
    memcpy(wire, ota_begin, sizeof(ota_begin) - 1U);
    memcpy(wire + sizeof(ota_begin) - 1U, binary, sizeof(binary));

    uint8_t storage[SCANNER_UART_LINE_BUFFER_SIZE];
    scanner_command_ingress_t ingress;
    ingress_fake_t fake;
    init_ingress(&ingress, &fake, storage);

    scanner_command_ingress_result_t result =
        scanner_command_ingress_consume(&ingress, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_UINT(1U, result.authorized_frames);
    TEST_ASSERT_EQUAL_UINT(0U, result.rejected_frames);
    TEST_ASSERT_EQUAL_UINT(sizeof(binary), result.binary_bytes);
    TEST_ASSERT_EQUAL_UINT(1U, fake.authorized_count);
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_ROUTE_FIRMWARE,
        fake.decisions[0].route);
    TEST_ASSERT_EQUAL_INT(
        FOF_FW_JSON_SCHEMA_SCANNER_OTA_BEGIN,
        fake.decisions[0].firmware_schema_id);
    TEST_ASSERT_TRUE(fake.authorized_was_terminated);
    TEST_ASSERT_EQUAL_UINT(sizeof(binary), fake.binary_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(binary, fake.binary, sizeof(binary));
}

void test_scanner_command_ingress_failed_ota_begin_discards_same_read_remainder(
    void)
{
    static const char ota_begin[] =
        "{\"type\":\"ota_begin\",\"session_id\":\"s\","
        "\"size\":2,\"crc\":3,\"sha256\":\"aa\",\"target_ver\":\"1\","
        "\"fw_name\":\"scanner\",\"app_project\":\"fof_scanner\","
        "\"hardware_type\":\"esp32s3\",\"generation\":1,"
        "\"allow_same_version\":false}\n";
    static const char command_shaped_remainder[] =
        "{\"type\":\"reboot\"}\n";
    uint8_t wire[
        sizeof(ota_begin) - 1U + sizeof(command_shaped_remainder) - 1U];
    memcpy(wire, ota_begin, sizeof(ota_begin) - 1U);
    memcpy(wire + sizeof(ota_begin) - 1U,
           command_shaped_remainder,
           sizeof(command_shaped_remainder) - 1U);

    uint8_t storage[SCANNER_UART_LINE_BUFFER_SIZE];
    scanner_command_ingress_t ingress;
    ingress_fake_t fake;
    init_ingress(&ingress, &fake, storage);
    fake.ota_begin_succeeds = false;

    scanner_command_ingress_result_t result =
        scanner_command_ingress_consume(&ingress, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_UINT(1U, result.authorized_frames);
    TEST_ASSERT_EQUAL_UINT(1U, fake.authorized_count);
    TEST_ASSERT_FALSE(fake.binary_mode);
    TEST_ASSERT_EQUAL_UINT(0U, result.binary_bytes);
    TEST_ASSERT_EQUAL_UINT(0U, fake.binary_len);
    TEST_ASSERT_EQUAL_UINT(
        sizeof(command_shaped_remainder) - 1U,
        result.discarded_after_failed_ota_begin);
}

void test_scanner_command_ingress_routes_all_six_firmware_schema_ids(void)
{
    static const struct {
        const char *wire;
        fof_fw_json_schema_id_t expected;
    } fixtures[] = {
        {
            "{\"type\":\"fw_offer\",\"update\":true,"
            "\"target_ver\":\"1\",\"fw_name\":\"scanner\","
            "\"app_project\":\"fof_scanner\","
            "\"hardware_type\":\"esp32s3\",\"sha256\":\"aa\","
            "\"generation\":1,\"size\":2,\"crc\":3,\"reason\":\"new\"}\n",
            FOF_FW_JSON_SCHEMA_SCANNER_FW_OFFER,
        },
        {
            "{\"type\":\"fw_check_now\"}\n",
            FOF_FW_JSON_SCHEMA_SCANNER_FW_CHECK_NOW,
        },
        {
            "{\"type\":\"ota_begin\",\"session_id\":\"s\","
            "\"size\":2,\"crc\":3,\"sha256\":\"aa\","
            "\"target_ver\":\"1\",\"fw_name\":\"scanner\","
            "\"app_project\":\"fof_scanner\","
            "\"hardware_type\":\"esp32s3\",\"generation\":1,"
            "\"allow_same_version\":false}\n",
            FOF_FW_JSON_SCHEMA_SCANNER_OTA_BEGIN,
        },
        {
            "{\"type\":\"ota_end\",\"session_id\":\"s\","
            "\"size\":2,\"crc\":3,\"sha256\":\"aa\","
            "\"target_ver\":\"1\",\"fw_name\":\"scanner\","
            "\"app_project\":\"fof_scanner\","
            "\"hardware_type\":\"esp32s3\",\"generation\":1,"
            "\"allow_same_version\":false}\n",
            FOF_FW_JSON_SCHEMA_SCANNER_OTA_END,
        },
        {
            "{\"type\":\"ota_abort\",\"session_id\":\"s\"}\n",
            FOF_FW_JSON_SCHEMA_SCANNER_OTA_ABORT_ACTIVE,
        },
        {
            "{\"type\":\"ota_abort\"}\n",
            FOF_FW_JSON_SCHEMA_SCANNER_OTA_ABORT_UNBOUND,
        },
    };

    for (size_t i = 0U; i < sizeof(fixtures) / sizeof(fixtures[0]); ++i) {
        uint8_t storage[SCANNER_UART_LINE_BUFFER_SIZE];
        scanner_command_ingress_t ingress;
        ingress_fake_t fake;
        init_ingress(&ingress, &fake, storage);
        scanner_command_ingress_result_t result =
            scanner_command_ingress_consume(
                &ingress,
                (const uint8_t *)fixtures[i].wire,
                strlen(fixtures[i].wire));
        TEST_ASSERT_EQUAL_UINT(1U, result.authorized_frames);
        TEST_ASSERT_EQUAL_UINT(0U, result.rejected_frames);
        TEST_ASSERT_EQUAL_UINT(1U, fake.authorized_count);
        TEST_ASSERT_EQUAL_INT(
            FOF_SCANNER_COMMAND_ROUTE_FIRMWARE,
            fake.decisions[0].route);
        TEST_ASSERT_EQUAL_INT(
            fixtures[i].expected,
            fake.decisions[0].firmware_schema_id);
    }
}

void test_scanner_command_ingress_rejects_invalid_setup_atomically(void)
{
    uint8_t storage[SCANNER_UART_LINE_BUFFER_SIZE];
    scanner_command_ingress_t ingress;
    scanner_command_ingress_callbacks_t callbacks = {0};

    TEST_ASSERT_FALSE(scanner_command_ingress_init(
        NULL, storage, sizeof(storage),
        FOF_SCANNER_DEPLOYMENT_BADGE, &callbacks, NULL));
    TEST_ASSERT_FALSE(scanner_command_ingress_init(
        &ingress, storage, sizeof(storage) - 1U,
        FOF_SCANNER_DEPLOYMENT_BADGE, &callbacks, NULL));
    TEST_ASSERT_FALSE(scanner_command_ingress_init(
        &ingress, storage, sizeof(storage),
        FOF_SCANNER_DEPLOYMENT_BADGE, &callbacks, NULL));
}

void test_scanner_command_ingress_authorizes_exact_crud_self_once(void)
{
    static const char wire[] =
        "{\"type\":\"crud_self\",\"v\":1,\"round\":34,"
        "\"peer\":\"A1B2C3\",\"session\":\"07\"}\n";
    uint8_t storage[SCANNER_UART_LINE_BUFFER_SIZE];
    scanner_command_ingress_t ingress;
    ingress_fake_t fake;
    init_ingress(&ingress, &fake, storage);

    scanner_command_ingress_result_t result =
        scanner_command_ingress_consume(
            &ingress, (const uint8_t *)wire, sizeof(wire) - 1U);

    TEST_ASSERT_EQUAL_UINT(1U, result.authorized_frames);
    TEST_ASSERT_EQUAL_UINT(0U, result.rejected_frames);
    TEST_ASSERT_EQUAL_UINT(1U, fake.authorized_count);
    TEST_ASSERT_EQUAL_INT(
        FOF_SCANNER_COMMAND_CRUD_SELF,
        fake.decisions[0].command.id);
    TEST_ASSERT_EQUAL_HEX32(
        0xA1B2C3U,
        fake.decisions[0].command.data.crud_self.peer);
    TEST_ASSERT_EQUAL_HEX8(
        0x07U,
        fake.decisions[0].command.data.crud_self.session);
}

void test_scanner_command_ingress_rejects_bad_crud_self_before_effect(void)
{
    static const char wire[] =
        "{\"type\":\"crud_self\",\"v\":1,\"round\":34,"
        "\"peer\":\"A1B2C3\",\"session\":\"00\"}\n";
    uint8_t storage[SCANNER_UART_LINE_BUFFER_SIZE];
    scanner_command_ingress_t ingress;
    ingress_fake_t fake;
    init_ingress(&ingress, &fake, storage);

    scanner_command_ingress_result_t result =
        scanner_command_ingress_consume(
            &ingress, (const uint8_t *)wire, sizeof(wire) - 1U);

    TEST_ASSERT_EQUAL_UINT(0U, result.authorized_frames);
    TEST_ASSERT_EQUAL_UINT(1U, result.rejected_frames);
    TEST_ASSERT_EQUAL_UINT(0U, fake.authorized_count);
}
