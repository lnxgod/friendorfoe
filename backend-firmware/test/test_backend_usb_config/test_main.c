#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "backend_portal_contract.h"
#include "backend_usb_config.h"
#include "../support/backend_test_main.h"

typedef struct {
    bool commit_result;
    bool reconnect_result;
    unsigned commit_calls;
    unsigned reconnect_calls;
    int64_t reconnect_now_ms;
    backend_config_record_t committed;
    backend_config_record_t reconnected;
} config_callbacks_t;

static backend_config_record_t config_fixture(void)
{
    backend_config_record_t config;
    memset(&config, 0, sizeof(config));
    config.schema_version = BACKEND_CONFIG_SCHEMA_VERSION;
    config.generation = 41;
    config.network_count = BACKEND_CONFIG_MAX_NETWORKS;
    strcpy(config.networks[0].ssid, "Primary");
    strcpy(config.networks[0].password, "primary-secret");
    strcpy(config.networks[1].ssid, "Fallback-One");
    strcpy(config.networks[1].password, "fallback-one-secret");
    strcpy(config.networks[2].ssid, "Fallback-Two");
    strcpy(config.networks[2].password, "fallback-two-secret");
    strcpy(config.networks[3].ssid, "Fallback-Three");
    strcpy(config.networks[3].password, "fallback-three-secret");
    strcpy(config.backend_url, "http://10.0.0.2:8000");
    strcpy(config.device_id, "uplink_CB77A4");
    strcpy(config.display_name, "Lite Front Yard");
    strcpy(config.ap_password, "portal-secret");
    config.has_location = true;
    config.latitude = 37.7749;
    config.longitude = -122.4194;
    config.altitude_m = 16.5f;
    return config;
}

static bool fake_commit(
    void *context, const backend_config_record_t *candidate)
{
    config_callbacks_t *callbacks = context;
    ++callbacks->commit_calls;
    callbacks->committed = *candidate;
    return callbacks->commit_result;
}

static bool fake_reconnect(
    void *context,
    const backend_config_record_t *committed,
    int64_t now_ms)
{
    config_callbacks_t *callbacks = context;
    ++callbacks->reconnect_calls;
    callbacks->reconnected = *committed;
    callbacks->reconnect_now_ms = now_ms;
    return callbacks->reconnect_result;
}

void setUp(void)
{
}

void tearDown(void)
{
}

void test_slot_zero_ssid_and_password_stage_without_deleting_fallback_slots(void)
{
    backend_config_record_t active = config_fixture();
    const backend_config_record_t original = active;
    backend_usb_config_t state;
    backend_usb_config_init(&state, &active);

    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_OK,
        backend_usb_config_stage(&state, "wifi_ssid", "New Primary"));
    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_OK,
        backend_usb_config_stage(&state, "wifi_pass", "new-primary-secret"));
    TEST_ASSERT_TRUE(state.dirty);
    TEST_ASSERT_EQUAL_UINT8(BACKEND_CONFIG_MAX_NETWORKS,
                            state.staged.network_count);
    TEST_ASSERT_EQUAL_STRING("New Primary", state.staged.networks[0].ssid);
    TEST_ASSERT_EQUAL_STRING(
        "new-primary-secret", state.staged.networks[0].password);
    for (uint8_t index = 1; index < BACKEND_CONFIG_MAX_NETWORKS; ++index) {
        TEST_ASSERT_EQUAL_MEMORY(
            &original.networks[index], &state.staged.networks[index],
            sizeof(original.networks[index]));
    }
    TEST_ASSERT_EQUAL_MEMORY(&original, &active, sizeof(active));
}

void test_stage_supports_backend_device_and_ap_fields_without_commit_side_effect(void)
{
    backend_config_record_t active = config_fixture();
    const backend_config_record_t original = active;
    backend_usb_config_t state;
    backend_usb_config_init(&state, &active);

    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_OK,
        backend_usb_config_stage(
            &state, "backend_url", "http://192.168.4.20:9000/base"));
    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_OK,
        backend_usb_config_stage(&state, "device_id", "lite_porch"));
    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_OK,
        backend_usb_config_stage(&state, "ap_pass", "new-ap-secret"));
    TEST_ASSERT_EQUAL_STRING(
        "http://192.168.4.20:9000/base", state.staged.backend_url);
    TEST_ASSERT_EQUAL_STRING("lite_porch", state.staged.device_id);
    TEST_ASSERT_EQUAL_STRING("new-ap-secret", state.staged.ap_password);
    TEST_ASSERT_EQUAL_UINT32(original.generation, state.staged.generation);
    TEST_ASSERT_EQUAL_MEMORY(&original, &active, sizeof(active));
}

void test_unknown_and_invalid_stage_values_leave_transaction_unchanged(void)
{
    const backend_config_record_t active = config_fixture();
    backend_usb_config_t state;
    backend_usb_config_init(&state, &active);
    const backend_usb_config_t original = state;

    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_UNKNOWN_FIELD,
        backend_usb_config_stage(&state, "display_name", "porch"));
    TEST_ASSERT_EQUAL_MEMORY(&original, &state, sizeof(state));
    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_INVALID_CONFIG,
        backend_usb_config_stage(&state, "wifi_ssid", ""));
    TEST_ASSERT_EQUAL_MEMORY(&original, &state, sizeof(state));
    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_INVALID_CONFIG,
        backend_usb_config_stage(&state, "backend_url", "https://example.com"));
    TEST_ASSERT_EQUAL_MEMORY(&original, &state, sizeof(state));
    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_INVALID_CONFIG,
        backend_usb_config_stage(&state, "ap_pass", "short"));
    TEST_ASSERT_EQUAL_MEMORY(&original, &state, sizeof(state));
    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_INVALID_CONFIG,
        backend_usb_config_stage(&state, "device_id", "bad\ndevice"));
    TEST_ASSERT_EQUAL_MEMORY(&original, &state, sizeof(state));
}

void test_save_validates_commits_once_then_reconnects_with_one_generation_increment(void)
{
    const backend_config_record_t active = config_fixture();
    backend_usb_config_t state;
    backend_usb_config_init(&state, &active);
    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_OK,
        backend_usb_config_stage(&state, "wifi_ssid", "New Primary"));
    config_callbacks_t callbacks = {
        .commit_result = true,
        .reconnect_result = true,
    };
    uint32_t generation = 0;

    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_OK,
        backend_usb_config_save(
            &state, fake_commit, fake_reconnect,
            &callbacks, 12345, &generation));
    TEST_ASSERT_EQUAL_UINT32(1, callbacks.commit_calls);
    TEST_ASSERT_EQUAL_UINT32(1, callbacks.reconnect_calls);
    TEST_ASSERT_EQUAL_UINT32(active.generation + 1U, generation);
    TEST_ASSERT_EQUAL_UINT32(generation, callbacks.committed.generation);
    TEST_ASSERT_EQUAL_UINT32(generation, callbacks.reconnected.generation);
    TEST_ASSERT_EQUAL_INT64(12345, callbacks.reconnect_now_ms);
    TEST_ASSERT_EQUAL_STRING("New Primary", callbacks.committed.networks[0].ssid);
    TEST_ASSERT_EQUAL_MEMORY(
        &callbacks.committed, &state.staged, sizeof(state.staged));
    TEST_ASSERT_FALSE(state.dirty);
}

void test_invalid_staged_record_fails_before_commit_or_reconnect(void)
{
    const backend_config_record_t active = config_fixture();
    backend_usb_config_t state;
    backend_usb_config_init(&state, &active);
    state.staged.backend_url[0] = '\0';
    state.dirty = true;
    const backend_usb_config_t before = state;
    config_callbacks_t callbacks = {
        .commit_result = true,
        .reconnect_result = true,
    };
    uint32_t generation = 99;

    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_COMMIT_FAILED,
        backend_usb_config_save(
            &state, fake_commit, fake_reconnect,
            &callbacks, 10, &generation));
    TEST_ASSERT_EQUAL_UINT32(0, callbacks.commit_calls);
    TEST_ASSERT_EQUAL_UINT32(0, callbacks.reconnect_calls);
    TEST_ASSERT_EQUAL_UINT32(0, generation);
    TEST_ASSERT_EQUAL_MEMORY(&before, &state, sizeof(state));
}

void test_commit_failure_rolls_back_transaction_state_and_skips_reconnect(void)
{
    const backend_config_record_t active = config_fixture();
    backend_usb_config_t state;
    backend_usb_config_init(&state, &active);
    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_OK,
        backend_usb_config_stage(&state, "device_id", "lite_porch"));
    const backend_usb_config_t before = state;
    config_callbacks_t callbacks = {
        .commit_result = false,
        .reconnect_result = true,
    };
    uint32_t generation = 99;

    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_COMMIT_FAILED,
        backend_usb_config_save(
            &state, fake_commit, fake_reconnect,
            &callbacks, 10, &generation));
    TEST_ASSERT_EQUAL_UINT32(1, callbacks.commit_calls);
    TEST_ASSERT_EQUAL_UINT32(0, callbacks.reconnect_calls);
    TEST_ASSERT_EQUAL_UINT32(active.generation + 1U,
                             callbacks.committed.generation);
    TEST_ASSERT_EQUAL_UINT32(0, generation);
    TEST_ASSERT_EQUAL_MEMORY(&before, &state, sizeof(state));
}

void test_reconnect_failure_reports_saved_and_keeps_committed_generation(void)
{
    const backend_config_record_t active = config_fixture();
    backend_usb_config_t state;
    backend_usb_config_init(&state, &active);
    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_OK,
        backend_usb_config_stage(&state, "ap_pass", "replacement-secret"));
    config_callbacks_t callbacks = {
        .commit_result = true,
        .reconnect_result = false,
    };
    uint32_t generation = 0;

    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_RECONNECT_FAILED,
        backend_usb_config_save(
            &state, fake_commit, fake_reconnect,
            &callbacks, 55, &generation));
    TEST_ASSERT_EQUAL_UINT32(1, callbacks.commit_calls);
    TEST_ASSERT_EQUAL_UINT32(1, callbacks.reconnect_calls);
    TEST_ASSERT_EQUAL_UINT32(active.generation + 1U, generation);
    TEST_ASSERT_EQUAL_UINT32(generation, state.staged.generation);
    TEST_ASSERT_EQUAL_STRING("replacement-secret", state.staged.ap_password);
    TEST_ASSERT_FALSE(state.dirty);
}

void test_redacted_json_exposes_presence_but_never_staged_passwords(void)
{
    const backend_config_record_t active = config_fixture();
    backend_usb_config_t state;
    backend_usb_config_init(&state, &active);
    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_OK,
        backend_usb_config_stage(&state, "wifi_pass", "new-primary-secret"));
    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_OK,
        backend_usb_config_stage(&state, "ap_pass", "new-portal-secret"));

    char json[2048];
    const size_t length = backend_portal_render_redacted_config(
        &state.staged, json, sizeof(json));
    TEST_ASSERT_GREATER_THAN_UINT32(0, length);
    TEST_ASSERT_NULL(strstr(json, "new-primary-secret"));
    TEST_ASSERT_NULL(strstr(json, "new-portal-secret"));
    TEST_ASSERT_NULL(strstr(json, "fallback-one-secret"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"password_set\":true"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"ap_password_set\":true"));
}

void test_empty_configuration_can_stage_slot_zero_without_touching_other_storage(void)
{
    backend_config_record_t current = config_fixture();
    current.network_count = 0;
    memset(current.networks, 0, sizeof(current.networks));
    backend_usb_config_t state;
    backend_usb_config_init(&state, &current);

    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_OK,
        backend_usb_config_stage(&state, "wifi_pass", "open-later"));
    TEST_ASSERT_EQUAL_UINT8(0, state.staged.network_count);
    TEST_ASSERT_EQUAL_STRING("open-later", state.staged.networks[0].password);
    TEST_ASSERT_EQUAL(
        BACKEND_PORTAL_UPDATE_OK,
        backend_usb_config_stage(&state, "wifi_ssid", "First Network"));
    TEST_ASSERT_EQUAL_UINT8(1, state.staged.network_count);
    TEST_ASSERT_EQUAL_STRING("First Network", state.staged.networks[0].ssid);
    TEST_ASSERT_EQUAL_STRING("open-later", state.staged.networks[0].password);
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(test_slot_zero_ssid_and_password_stage_without_deleting_fallback_slots);
    BACKEND_RUN_TEST(test_stage_supports_backend_device_and_ap_fields_without_commit_side_effect);
    BACKEND_RUN_TEST(test_unknown_and_invalid_stage_values_leave_transaction_unchanged);
    BACKEND_RUN_TEST(test_save_validates_commits_once_then_reconnects_with_one_generation_increment);
    BACKEND_RUN_TEST(test_invalid_staged_record_fails_before_commit_or_reconnect);
    BACKEND_RUN_TEST(test_commit_failure_rolls_back_transaction_state_and_skips_reconnect);
    BACKEND_RUN_TEST(test_reconnect_failure_reports_saved_and_keeps_committed_generation);
    BACKEND_RUN_TEST(test_redacted_json_exposes_presence_but_never_staged_passwords);
    BACKEND_RUN_TEST(test_empty_configuration_can_stage_slot_zero_without_touching_other_storage);
    return UNITY_END();
}
