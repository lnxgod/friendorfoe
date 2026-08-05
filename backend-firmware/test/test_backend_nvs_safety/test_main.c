#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "backend_config.h"
#include "backend_nvs_config.h"
#include "../support/backend_test_main.h"

typedef struct {
    backend_nvs_io_result_t init_result;
    bool committed_present;
    uint8_t committed[BACKEND_CONFIG_BLOB_MAX];
    size_t committed_length;
    bool staged_present;
    uint8_t staged[BACKEND_CONFIG_BLOB_MAX];
    size_t staged_length;
    bool fail_write;
    bool fail_commit;
    bool legacy_wifi_ssid_present;
    bool legacy_wifi_password_present;
    bool legacy_wifi_pass_present;
    bool legacy_backend_url_present;
    bool legacy_device_id_present;
    bool legacy_ap_pass_present;
    char legacy_wifi_ssid[33];
    char legacy_wifi_password[65];
    char legacy_wifi_pass[65];
    char legacy_backend_url[192];
    char legacy_device_id[33];
    char legacy_ap_pass[65];
    uint8_t sta_mac[6];
    unsigned init_calls;
    unsigned read_blob_calls;
    unsigned write_blob_calls;
    unsigned commit_calls;
    unsigned read_string_calls;
    unsigned read_sta_mac_calls;
    unsigned erase_calls;
    unsigned rollback_clear_calls;
    bool wrong_namespace_or_key;
} fake_nvs_t;

static fake_nvs_t s_fake;

static backend_nvs_io_result_t fake_init(void *context)
{
    fake_nvs_t *fake = context;
    fake->init_calls++;
    return fake->init_result;
}

static backend_nvs_io_result_t fake_read_blob(
    void *context, const char *namespace_name, const char *key,
    uint8_t *out, size_t capacity, size_t *out_length)
{
    fake_nvs_t *fake = context;
    fake->read_blob_calls++;
    if (strcmp(namespace_name, "fof_config") != 0 ||
        strcmp(key, "backend_config") != 0) {
        fake->wrong_namespace_or_key = true;
        return BACKEND_NVS_IO_ERROR;
    }
    if (!fake->committed_present) {
        return BACKEND_NVS_IO_NOT_FOUND;
    }
    if (fake->committed_length > capacity) {
        return BACKEND_NVS_IO_ERROR;
    }
    memcpy(out, fake->committed, fake->committed_length);
    *out_length = fake->committed_length;
    return BACKEND_NVS_IO_OK;
}

static backend_nvs_io_result_t fake_write_blob(
    void *context, const char *namespace_name, const char *key,
    const uint8_t *bytes, size_t length)
{
    fake_nvs_t *fake = context;
    fake->write_blob_calls++;
    if (strcmp(namespace_name, "fof_config") != 0 ||
        strcmp(key, "backend_config") != 0) {
        fake->wrong_namespace_or_key = true;
        return BACKEND_NVS_IO_ERROR;
    }
    if (fake->fail_write || length > sizeof(fake->staged)) {
        return BACKEND_NVS_IO_ERROR;
    }
    memcpy(fake->staged, bytes, length);
    fake->staged_length = length;
    fake->staged_present = true;
    return BACKEND_NVS_IO_OK;
}

static backend_nvs_io_result_t fake_commit(void *context)
{
    fake_nvs_t *fake = context;
    fake->commit_calls++;
    if (fake->fail_commit || !fake->staged_present) {
        return BACKEND_NVS_IO_ERROR;
    }
    memcpy(fake->committed, fake->staged, fake->staged_length);
    fake->committed_length = fake->staged_length;
    fake->committed_present = true;
    fake->staged_present = false;
    return BACKEND_NVS_IO_OK;
}

static backend_nvs_io_result_t fake_read_string(
    void *context, const char *namespace_name, const char *key,
    char *out, size_t capacity)
{
    fake_nvs_t *fake = context;
    fake->read_string_calls++;
    if (strcmp(namespace_name, "fof_config") != 0) {
        fake->wrong_namespace_or_key = true;
        return BACKEND_NVS_IO_ERROR;
    }

    const char *value = NULL;
    bool present = false;
    if (strcmp(key, "wifi_ssid") == 0) {
        value = fake->legacy_wifi_ssid;
        present = fake->legacy_wifi_ssid_present;
    } else if (strcmp(key, "wifi_password") == 0) {
        value = fake->legacy_wifi_password;
        present = fake->legacy_wifi_password_present;
    } else if (strcmp(key, "wifi_pass") == 0) {
        value = fake->legacy_wifi_pass;
        present = fake->legacy_wifi_pass_present;
    } else if (strcmp(key, "backend_url") == 0) {
        value = fake->legacy_backend_url;
        present = fake->legacy_backend_url_present;
    } else if (strcmp(key, "device_id") == 0) {
        value = fake->legacy_device_id;
        present = fake->legacy_device_id_present;
    } else if (strcmp(key, "ap_pass") == 0) {
        value = fake->legacy_ap_pass;
        present = fake->legacy_ap_pass_present;
    } else {
        fake->wrong_namespace_or_key = true;
        return BACKEND_NVS_IO_ERROR;
    }

    if (!present) {
        return BACKEND_NVS_IO_NOT_FOUND;
    }
    const size_t length = strlen(value);
    if (length + 1U > capacity) {
        return BACKEND_NVS_IO_ERROR;
    }
    memcpy(out, value, length + 1U);
    return BACKEND_NVS_IO_OK;
}

static bool fake_read_sta_mac(void *context, uint8_t out[6])
{
    fake_nvs_t *fake = context;
    fake->read_sta_mac_calls++;
    memcpy(out, fake->sta_mac, 6);
    return true;
}

static void fake_erase_storage(void *context)
{
    fake_nvs_t *fake = context;
    fake->erase_calls++;
}

static void fake_clear_rollback(void *context)
{
    fake_nvs_t *fake = context;
    fake->rollback_clear_calls++;
}

static backend_config_record_t valid_config(uint32_t generation)
{
    backend_config_record_t record;
    memset(&record, 0, sizeof(record));
    record.schema_version = BACKEND_CONFIG_SCHEMA_VERSION;
    record.generation = generation;
    record.network_count = 1;
    strcpy(record.networks[0].ssid, "FieldNet");
    strcpy(record.networks[0].password, "secret-value");
    strcpy(record.backend_url, "http://10.0.0.2:8000");
    strcpy(record.device_id, "uplink_CB77A4");
    strcpy(record.display_name, "Lite");
    strcpy(record.ap_password, "friendorfoe");
    return record;
}

static void install_fake(void)
{
    backend_nvs_config_hooks_t hooks = {
        .context = &s_fake,
        .init = fake_init,
        .read_blob = fake_read_blob,
        .write_blob = fake_write_blob,
        .commit = fake_commit,
        .read_string = fake_read_string,
        .read_sta_mac = fake_read_sta_mac,
        .erase_storage = fake_erase_storage,
        .clear_rollback = fake_clear_rollback,
    };
    backend_nvs_config_set_test_hooks(&hooks);
}

static void seed_legacy(bool include_device_id)
{
    s_fake.legacy_wifi_ssid_present = true;
    s_fake.legacy_wifi_pass_present = true;
    s_fake.legacy_backend_url_present = true;
    s_fake.legacy_ap_pass_present = true;
    strcpy(s_fake.legacy_wifi_ssid, "FieldNet");
    strcpy(s_fake.legacy_wifi_pass, "secret-value");
    strcpy(s_fake.legacy_backend_url, "http://10.0.0.2:8000");
    strcpy(s_fake.legacy_ap_pass, "friendorfoe");
    s_fake.legacy_device_id_present = include_device_id;
    if (include_device_id) {
        strcpy(s_fake.legacy_device_id, "uplink_112233");
    }
}

void setUp(void)
{
    backend_nvs_config_reset_test_hooks();
    memset(&s_fake, 0, sizeof(s_fake));
    s_fake.init_result = BACKEND_NVS_IO_OK;
    const uint8_t mac[6] = {0x02, 0x00, 0x00, 0xCB, 0x77, 0xA4};
    memcpy(s_fake.sta_mac, mac, sizeof(mac));
    install_fake();
}

void tearDown(void)
{
    backend_nvs_config_reset_test_hooks();
}

void test_commit_and_load_use_one_validated_canonical_blob(void)
{
    backend_config_record_t record = valid_config(7);
    TEST_ASSERT_TRUE(backend_config_commit(&record));
    TEST_ASSERT_EQUAL_UINT32(1, s_fake.write_blob_calls);
    TEST_ASSERT_EQUAL_UINT32(1, s_fake.commit_calls);
    TEST_ASSERT_TRUE(s_fake.committed_present);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(BACKEND_CONFIG_BLOB_MAX,
                                     s_fake.committed_length);

    backend_config_record_t loaded = {0};
    TEST_ASSERT_TRUE(backend_config_load(&loaded));
    TEST_ASSERT_EQUAL_UINT32(7, loaded.generation);
    TEST_ASSERT_EQUAL_STRING("uplink_CB77A4", loaded.device_id);
    TEST_ASSERT_EQUAL_STRING("secret-value", loaded.networks[0].password);
    TEST_ASSERT_FALSE(s_fake.wrong_namespace_or_key);
    TEST_ASSERT_EQUAL_UINT32(1, s_fake.init_calls);
}

void test_invalid_record_never_reaches_write_or_commit(void)
{
    backend_config_record_t record = valid_config(0);
    TEST_ASSERT_FALSE(backend_config_commit(&record));
    TEST_ASSERT_EQUAL_UINT32(0, s_fake.write_blob_calls);
    TEST_ASSERT_EQUAL_UINT32(0, s_fake.commit_calls);
    TEST_ASSERT_EQUAL_UINT32(0, s_fake.init_calls);
}

void test_failed_write_or_commit_leaves_prior_record_readable(void)
{
    backend_config_record_t prior = valid_config(1);
    TEST_ASSERT_TRUE(backend_config_commit(&prior));

    backend_config_record_t replacement = valid_config(2);
    s_fake.fail_write = true;
    TEST_ASSERT_FALSE(backend_config_commit(&replacement));
    s_fake.fail_write = false;
    backend_config_record_t loaded = {0};
    TEST_ASSERT_TRUE(backend_config_load(&loaded));
    TEST_ASSERT_EQUAL_UINT32(1, loaded.generation);

    s_fake.fail_commit = true;
    TEST_ASSERT_FALSE(backend_config_commit(&replacement));
    TEST_ASSERT_TRUE(s_fake.staged_present);

    backend_nvs_config_reset_test_hooks();
    s_fake.staged_present = false;
    s_fake.staged_length = 0;
    s_fake.fail_commit = false;
    install_fake();
    memset(&loaded, 0, sizeof(loaded));
    TEST_ASSERT_TRUE(backend_config_load(&loaded));
    TEST_ASSERT_EQUAL_UINT32(1, loaded.generation);
}

void test_missing_blob_imports_legacy_once_and_disables_auto_update(void)
{
    seed_legacy(true);
    backend_config_record_t migrated = {0};
    TEST_ASSERT_TRUE(backend_config_load_or_migrate(&migrated));
    TEST_ASSERT_EQUAL_UINT32(1, migrated.generation);
    TEST_ASSERT_EQUAL_STRING("uplink_112233", migrated.device_id);
    TEST_ASSERT_EQUAL_STRING("secret-value", migrated.networks[0].password);
    TEST_ASSERT_FALSE(migrated.auto_update_enabled);
    TEST_ASSERT_EQUAL_UINT32(6, s_fake.read_string_calls);
    TEST_ASSERT_EQUAL_UINT32(0, s_fake.read_sta_mac_calls);
    TEST_ASSERT_EQUAL_UINT32(1, s_fake.write_blob_calls);
    TEST_ASSERT_EQUAL_UINT32(1, s_fake.commit_calls);

    const unsigned legacy_reads = s_fake.read_string_calls;
    memset(&s_fake.legacy_wifi_ssid, 0, sizeof(s_fake.legacy_wifi_ssid));
    s_fake.legacy_wifi_ssid_present = false;
    backend_config_record_t loaded = {0};
    TEST_ASSERT_TRUE(backend_config_load_or_migrate(&loaded));
    TEST_ASSERT_EQUAL_STRING("uplink_112233", loaded.device_id);
    TEST_ASSERT_EQUAL_UINT32(1, s_fake.write_blob_calls);
    TEST_ASSERT_EQUAL_UINT32(legacy_reads, s_fake.read_string_calls);
}

void test_legacy_password_alias_conflict_fails_without_writing(void)
{
    seed_legacy(true);
    s_fake.legacy_wifi_password_present = true;
    strcpy(s_fake.legacy_wifi_password, "different-secret");
    backend_config_record_t output;
    backend_config_record_t sentinel;
    memset(&output, 0x3C, sizeof(output));
    memcpy(&sentinel, &output, sizeof(sentinel));

    TEST_ASSERT_FALSE(backend_config_load_or_migrate(&output));
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &output, sizeof(output));
    TEST_ASSERT_EQUAL_UINT32(0, s_fake.write_blob_calls);
    TEST_ASSERT_EQUAL_UINT32(0, s_fake.commit_calls);
}

void test_present_empty_password_alias_conflicts_with_present_stale_alias(void)
{
    seed_legacy(true);
    s_fake.legacy_wifi_password_present = true;
    s_fake.legacy_wifi_password[0] = '\0';
    backend_config_record_t output;
    backend_config_record_t sentinel;
    memset(&output, 0x91, sizeof(output));
    memcpy(&sentinel, &output, sizeof(sentinel));

    TEST_ASSERT_FALSE(backend_config_load_or_migrate(&output));
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &output, sizeof(output));
    TEST_ASSERT_EQUAL_UINT32(0, s_fake.write_blob_calls);
    TEST_ASSERT_EQUAL_UINT32(0, s_fake.commit_calls);
}

void test_migration_generates_only_missing_device_id_from_sta_mac(void)
{
    seed_legacy(false);
    backend_config_record_t generated = {0};
    TEST_ASSERT_TRUE(backend_config_load_or_migrate(&generated));
    TEST_ASSERT_EQUAL_STRING("uplink_CB77A4", generated.device_id);
    TEST_ASSERT_EQUAL_UINT32(1, s_fake.read_sta_mac_calls);

    setUp();
    seed_legacy(true);
    backend_config_record_t preserved = {0};
    TEST_ASSERT_TRUE(backend_config_load_or_migrate(&preserved));
    TEST_ASSERT_EQUAL_STRING("uplink_112233", preserved.device_id);
    TEST_ASSERT_EQUAL_UINT32(0, s_fake.read_sta_mac_calls);
}

static void assert_fatal_init_is_non_destructive(backend_nvs_io_result_t error)
{
    setUp();
    s_fake.init_result = error;
    backend_config_record_t output;
    backend_config_record_t sentinel;
    memset(&output, 0x6D, sizeof(output));
    memcpy(&sentinel, &output, sizeof(sentinel));

    TEST_ASSERT_FALSE(backend_config_load_or_migrate(&output));
    TEST_ASSERT_EQUAL(BACKEND_NVS_STORAGE_FATAL,
                      backend_nvs_config_storage_health());
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &output, sizeof(output));
    TEST_ASSERT_EQUAL_UINT32(1, s_fake.init_calls);
    TEST_ASSERT_EQUAL_UINT32(0, s_fake.read_blob_calls);
    TEST_ASSERT_EQUAL_UINT32(0, s_fake.read_string_calls);
    TEST_ASSERT_EQUAL_UINT32(0, s_fake.erase_calls);
    TEST_ASSERT_EQUAL_UINT32(0, s_fake.write_blob_calls);
    TEST_ASSERT_EQUAL_UINT32(0, s_fake.commit_calls);
    TEST_ASSERT_EQUAL_UINT32(0, s_fake.rollback_clear_calls);
}

void test_unrecoverable_init_errors_preserve_identity_and_rollback_state(void)
{
    assert_fatal_init_is_non_destructive(BACKEND_NVS_IO_NO_FREE_PAGES);
    assert_fatal_init_is_non_destructive(BACKEND_NVS_IO_NEW_VERSION);
}

void test_invalid_stored_blob_does_not_modify_output_or_trigger_migration(void)
{
    s_fake.committed_present = true;
    s_fake.committed_length = 16;
    memset(s_fake.committed, 0xA5, s_fake.committed_length);
    seed_legacy(true);
    backend_config_record_t output;
    backend_config_record_t sentinel;
    memset(&output, 0x27, sizeof(output));
    memcpy(&sentinel, &output, sizeof(sentinel));

    TEST_ASSERT_FALSE(backend_config_load_or_migrate(&output));
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &output, sizeof(output));
    TEST_ASSERT_EQUAL_UINT32(0, s_fake.write_blob_calls);
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(test_commit_and_load_use_one_validated_canonical_blob);
    BACKEND_RUN_TEST(test_invalid_record_never_reaches_write_or_commit);
    BACKEND_RUN_TEST(test_failed_write_or_commit_leaves_prior_record_readable);
    BACKEND_RUN_TEST(test_missing_blob_imports_legacy_once_and_disables_auto_update);
    BACKEND_RUN_TEST(test_legacy_password_alias_conflict_fails_without_writing);
    BACKEND_RUN_TEST(test_present_empty_password_alias_conflicts_with_present_stale_alias);
    BACKEND_RUN_TEST(test_migration_generates_only_missing_device_id_from_sta_mac);
    BACKEND_RUN_TEST(test_unrecoverable_init_errors_preserve_identity_and_rollback_state);
    BACKEND_RUN_TEST(test_invalid_stored_blob_does_not_modify_output_or_trigger_migration);
    return UNITY_END();
}
