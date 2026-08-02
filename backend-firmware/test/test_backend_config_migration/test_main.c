#include <string.h>

#include <unity.h>

#include "backend_config.h"
#include "../support/backend_test_main.h"

static backend_legacy_config_t valid_legacy_fixture(void)
{
    backend_legacy_config_t legacy;
    memset(&legacy, 0, sizeof(legacy));
    strcpy(legacy.wifi_ssid, "FieldNet");
    strcpy(legacy.backend_url, "http://10.0.0.2:8000");
    strcpy(legacy.device_id, "uplink_CB77A4");
    strcpy(legacy.ap_pass, "friendorfoe");
    return legacy;
}

void test_legacy_migration_accepts_wifi_pass_alias_and_never_overwrites_id(void)
{
    backend_legacy_config_t legacy = valid_legacy_fixture();
    strcpy(legacy.wifi_pass, "secret-value");

    backend_config_record_t migrated = {0};
    TEST_ASSERT_TRUE(backend_config_migrate_legacy(&legacy, 9, &migrated));
    TEST_ASSERT_EQUAL_UINT8(1, migrated.network_count);
    TEST_ASSERT_EQUAL_STRING("FieldNet", migrated.networks[0].ssid);
    TEST_ASSERT_EQUAL_STRING("secret-value", migrated.networks[0].password);
    TEST_ASSERT_EQUAL_STRING("uplink_CB77A4", migrated.device_id);
    TEST_ASSERT_EQUAL_UINT32(9, migrated.generation);
    TEST_ASSERT_FALSE(migrated.auto_update_enabled);
}

void test_legacy_migration_accepts_password_aliases_only_when_unambiguous(void)
{
    backend_legacy_config_t legacy = valid_legacy_fixture();
    strcpy(legacy.wifi_password, "canonical-secret");

    backend_config_record_t migrated = {0};
    TEST_ASSERT_TRUE(backend_config_migrate_legacy(&legacy, 3, &migrated));
    TEST_ASSERT_EQUAL_STRING("canonical-secret", migrated.networks[0].password);

    strcpy(legacy.wifi_pass, "canonical-secret");
    memset(&migrated, 0, sizeof(migrated));
    TEST_ASSERT_TRUE(backend_config_migrate_legacy(&legacy, 4, &migrated));
    TEST_ASSERT_EQUAL_STRING("canonical-secret", migrated.networks[0].password);

    strcpy(legacy.wifi_pass, "different-secret");
    backend_config_record_t sentinel;
    memset(&migrated, 0xA5, sizeof(migrated));
    memcpy(&sentinel, &migrated, sizeof(sentinel));
    TEST_ASSERT_FALSE(backend_config_migrate_legacy(&legacy, 5, &migrated));
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &migrated, sizeof(migrated));
}

void test_legacy_migration_defaults_optional_fields_and_rejects_invalid_input(void)
{
    backend_legacy_config_t legacy = valid_legacy_fixture();
    strcpy(legacy.wifi_pass, "secret-value");
    backend_config_record_t migrated = {0};

    TEST_ASSERT_TRUE(backend_config_migrate_legacy(&legacy, 1, &migrated));
    TEST_ASSERT_EQUAL_UINT16(BACKEND_CONFIG_SCHEMA_VERSION,
                             migrated.schema_version);
    TEST_ASSERT_FALSE(migrated.auto_update_enabled);
    TEST_ASSERT_FALSE(migrated.has_location);
    TEST_ASSERT_EQUAL_STRING("", migrated.display_name);
    TEST_ASSERT_EQUAL(BACKEND_CONFIG_VALID,
                      backend_config_validate(&migrated));

    TEST_ASSERT_FALSE(backend_config_migrate_legacy(&legacy, 0, &migrated));
    strcpy(legacy.backend_url, "https://not-allowed.local");
    TEST_ASSERT_FALSE(backend_config_migrate_legacy(&legacy, 2, &migrated));
}

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(test_legacy_migration_accepts_wifi_pass_alias_and_never_overwrites_id);
    BACKEND_RUN_TEST(test_legacy_migration_accepts_password_aliases_only_when_unambiguous);
    BACKEND_RUN_TEST(test_legacy_migration_defaults_optional_fields_and_rejects_invalid_input);
    return UNITY_END();
}
