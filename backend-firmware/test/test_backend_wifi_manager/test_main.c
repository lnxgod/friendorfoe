#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "backend_wifi_manager.h"
#include "../support/backend_test_main.h"

static backend_config_record_t config_fixture(
    uint32_t generation, uint8_t network_count)
{
    static const char *const ssids[BACKEND_CONFIG_MAX_NETWORKS] = {
        "FirstSecretSsid", "SecondSecretSsid",
        "ThirdSecretSsid", "FourthSecretSsid",
    };
    static const char *const passwords[BACKEND_CONFIG_MAX_NETWORKS] = {
        "first-password", "second-password",
        "third-password", "fourth-password",
    };
    backend_config_record_t config;
    memset(&config, 0, sizeof(config));
    config.schema_version = BACKEND_CONFIG_SCHEMA_VERSION;
    config.generation = generation;
    config.network_count = network_count;
    for (uint8_t index = 0; index < network_count; ++index) {
        strcpy(config.networks[index].ssid, ssids[index]);
        strcpy(config.networks[index].password, passwords[index]);
    }
    strcpy(config.backend_url, "http://10.0.0.2:8000");
    strcpy(config.device_id, "uplink_CB77A4");
    strcpy(config.display_name, "Lite node");
    strcpy(config.ap_password, "friendorfoe");
    return config;
}

void setUp(void)
{
}

void tearDown(void)
{
}

void test_four_networks_advance_in_order_and_wrap_only_after_all_fail(void)
{
    const backend_config_record_t config = config_fixture(7, 4);
    backend_wifi_policy_t state;
    backend_wifi_policy_init(&state);

    TEST_ASSERT_EQUAL(
        BACKEND_WIFI_CONNECT_NETWORK,
        backend_wifi_policy_update(
            &state, &config, BACKEND_WIFI_EVENT_TICK, 100));
    TEST_ASSERT_EQUAL_UINT8(0, state.network_index);

    TEST_ASSERT_EQUAL(
        BACKEND_WIFI_CONNECT_NETWORK,
        backend_wifi_policy_update(
            &state, &config, BACKEND_WIFI_EVENT_AUTH_FAILED, 200));
    TEST_ASSERT_EQUAL_UINT8(1, state.network_index);

    TEST_ASSERT_EQUAL(
        BACKEND_WIFI_CONNECT_NETWORK,
        backend_wifi_policy_update(
            &state, &config, BACKEND_WIFI_EVENT_NO_AP, 300));
    TEST_ASSERT_EQUAL_UINT8(2, state.network_index);

    TEST_ASSERT_EQUAL(
        BACKEND_WIFI_CONNECT_NETWORK,
        backend_wifi_policy_update(
            &state, &config, BACKEND_WIFI_EVENT_AUTH_FAILED, 400));
    TEST_ASSERT_EQUAL_UINT8(3, state.network_index);

    TEST_ASSERT_EQUAL(
        BACKEND_WIFI_WAIT_RETRY,
        backend_wifi_policy_update(
            &state, &config, BACKEND_WIFI_EVENT_NO_AP, 500));
    TEST_ASSERT_EQUAL_UINT8(0, state.network_index);
    TEST_ASSERT_EQUAL_INT64(1500, state.retry_after_ms);
    TEST_ASSERT_EQUAL_UINT8(1, state.retry_exponent);

    TEST_ASSERT_EQUAL(
        BACKEND_WIFI_NO_CHANGE,
        backend_wifi_policy_update(
            &state, &config, BACKEND_WIFI_EVENT_TICK, 1499));
    TEST_ASSERT_EQUAL(
        BACKEND_WIFI_CONNECT_NETWORK,
        backend_wifi_policy_update(
            &state, &config, BACKEND_WIFI_EVENT_TICK, 1500));
    TEST_ASSERT_EQUAL_UINT8(0, state.network_index);
}

void test_attempt_timeout_advances_at_exact_boundary(void)
{
    const backend_config_record_t config = config_fixture(3, 4);
    backend_wifi_policy_t state;
    backend_wifi_policy_init(&state);
    TEST_ASSERT_EQUAL(
        BACKEND_WIFI_CONNECT_NETWORK,
        backend_wifi_policy_update(
            &state, &config, BACKEND_WIFI_EVENT_TICK, 1000));

    TEST_ASSERT_EQUAL(
        BACKEND_WIFI_NO_CHANGE,
        backend_wifi_policy_update(
            &state, &config, BACKEND_WIFI_EVENT_TICK,
            1000 + BACKEND_WIFI_ATTEMPT_TIMEOUT_MS - 1));
    TEST_ASSERT_EQUAL_UINT8(0, state.network_index);
    TEST_ASSERT_EQUAL(
        BACKEND_WIFI_CONNECT_NETWORK,
        backend_wifi_policy_update(
            &state, &config, BACKEND_WIFI_EVENT_TICK,
            1000 + BACKEND_WIFI_ATTEMPT_TIMEOUT_MS));
    TEST_ASSERT_EQUAL_UINT8(1, state.network_index);
}

void test_one_network_uses_capped_exponential_backoff(void)
{
    const backend_config_record_t config = config_fixture(9, 1);
    backend_wifi_policy_t state;
    backend_wifi_policy_init(&state);
    TEST_ASSERT_EQUAL(
        BACKEND_WIFI_CONNECT_NETWORK,
        backend_wifi_policy_update(
            &state, &config, BACKEND_WIFI_EVENT_TICK, 0));

    int64_t now_ms = 10;
    const uint32_t expected_delays[] = {
        1000U, 2000U, 4000U, 8000U, 16000U,
        32000U, 60000U, 60000U,
    };
    for (size_t index = 0;
         index < sizeof(expected_delays) / sizeof(expected_delays[0]);
         ++index) {
        TEST_ASSERT_EQUAL(
            BACKEND_WIFI_WAIT_RETRY,
            backend_wifi_policy_update(
                &state, &config, BACKEND_WIFI_EVENT_NO_AP, now_ms));
        TEST_ASSERT_EQUAL_INT64(
            now_ms + expected_delays[index], state.retry_after_ms);
        now_ms = state.retry_after_ms;
        TEST_ASSERT_EQUAL(
            BACKEND_WIFI_CONNECT_NETWORK,
            backend_wifi_policy_update(
                &state, &config, BACKEND_WIFI_EVENT_TICK, now_ms));
        ++now_ms;
    }
}

void test_late_failure_events_cannot_bypass_retry_tick_or_advance_backoff(void)
{
    const backend_config_record_t config = config_fixture(10, 1);
    backend_wifi_policy_t state;
    backend_wifi_policy_init(&state);
    TEST_ASSERT_EQUAL(
        BACKEND_WIFI_CONNECT_NETWORK,
        backend_wifi_policy_update(
            &state, &config, BACKEND_WIFI_EVENT_TICK, 0));
    TEST_ASSERT_EQUAL(
        BACKEND_WIFI_WAIT_RETRY,
        backend_wifi_policy_update(
            &state, &config, BACKEND_WIFI_EVENT_AUTH_FAILED, 10));
    TEST_ASSERT_EQUAL_INT64(1010, state.retry_after_ms);
    TEST_ASSERT_EQUAL_UINT8(1, state.retry_exponent);

    TEST_ASSERT_EQUAL(
        BACKEND_WIFI_WAIT_RETRY,
        backend_wifi_policy_update(
            &state, &config, BACKEND_WIFI_EVENT_NO_AP, 2010));
    TEST_ASSERT_EQUAL_INT64(1010, state.retry_after_ms);
    TEST_ASSERT_EQUAL_UINT8(1, state.retry_exponent);
    TEST_ASSERT_EQUAL_UINT8(0, state.network_index);

    TEST_ASSERT_EQUAL(
        BACKEND_WIFI_CONNECT_NETWORK,
        backend_wifi_policy_update(
            &state, &config, BACKEND_WIFI_EVENT_TICK, 2010));
    TEST_ASSERT_EQUAL_UINT8(0, state.network_index);
    TEST_ASSERT_EQUAL_INT64(-1, state.retry_after_ms);
}

void test_connected_and_new_generation_reset_index_and_backoff(void)
{
    backend_config_record_t config = config_fixture(21, 4);
    backend_wifi_policy_t state;
    backend_wifi_policy_init(&state);
    TEST_ASSERT_EQUAL(
        BACKEND_WIFI_CONNECT_NETWORK,
        backend_wifi_policy_update(
            &state, &config, BACKEND_WIFI_EVENT_TICK, 100));
    TEST_ASSERT_EQUAL(
        BACKEND_WIFI_CONNECT_NETWORK,
        backend_wifi_policy_update(
            &state, &config, BACKEND_WIFI_EVENT_AUTH_FAILED, 200));
    TEST_ASSERT_EQUAL(
        BACKEND_WIFI_NO_CHANGE,
        backend_wifi_policy_update(
            &state, &config, BACKEND_WIFI_EVENT_CONNECTED, 300));
    TEST_ASSERT_TRUE(state.connected);
    TEST_ASSERT_EQUAL_UINT8(0, state.retry_exponent);

    config.generation = 22;
    TEST_ASSERT_EQUAL(
        BACKEND_WIFI_CONNECT_NETWORK,
        backend_wifi_policy_update(
            &state, &config, BACKEND_WIFI_EVENT_TICK, 400));
    TEST_ASSERT_FALSE(state.connected);
    TEST_ASSERT_EQUAL_UINT8(0, state.network_index);
    TEST_ASSERT_EQUAL_UINT8(0, state.retry_exponent);
    TEST_ASSERT_EQUAL_UINT32(22, state.config_generation);
}

void test_manager_status_never_exposes_ssids_or_passwords(void)
{
    const backend_config_record_t config = config_fixture(31, 4);
    backend_wifi_policy_t state;
    backend_wifi_policy_init(&state);
    (void)backend_wifi_policy_update(
        &state, &config, BACKEND_WIFI_EVENT_TICK, 1234);

    char status[256];
    const size_t length = backend_wifi_policy_render_status(
        &state, status, sizeof(status));
    TEST_ASSERT_GREATER_THAN_UINT32(0, length);
    TEST_ASSERT_NOT_NULL(strstr(status, "\"network_index\":0"));
    TEST_ASSERT_NULL(strstr(status, "FirstSecretSsid"));
    TEST_ASSERT_NULL(strstr(status, "first-password"));
    for (uint8_t index = 0; index < config.network_count; ++index) {
        TEST_ASSERT_NULL(strstr(status, config.networks[index].ssid));
        TEST_ASSERT_NULL(strstr(status, config.networks[index].password));
    }
}

void test_invalid_or_empty_config_does_not_attempt_connection(void)
{
    backend_config_record_t empty = config_fixture(8, 0);
    backend_wifi_policy_t state;
    backend_wifi_policy_init(&state);
    TEST_ASSERT_EQUAL(
        BACKEND_WIFI_NO_CHANGE,
        backend_wifi_policy_update(
            &state, &empty, BACKEND_WIFI_EVENT_TICK, 1));
    TEST_ASSERT_EQUAL(
        BACKEND_WIFI_NO_CHANGE,
        backend_wifi_policy_update(
            NULL, &empty, BACKEND_WIFI_EVENT_TICK, 1));
    TEST_ASSERT_EQUAL(
        BACKEND_WIFI_NO_CHANGE,
        backend_wifi_policy_update(
            &state, NULL, BACKEND_WIFI_EVENT_TICK, 1));
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_four_networks_advance_in_order_and_wrap_only_after_all_fail);
    BACKEND_RUN_TEST(test_attempt_timeout_advances_at_exact_boundary);
    BACKEND_RUN_TEST(test_one_network_uses_capped_exponential_backoff);
    BACKEND_RUN_TEST(
        test_late_failure_events_cannot_bypass_retry_tick_or_advance_backoff);
    BACKEND_RUN_TEST(
        test_connected_and_new_generation_reset_index_and_backoff);
    BACKEND_RUN_TEST(test_manager_status_never_exposes_ssids_or_passwords);
    BACKEND_RUN_TEST(
        test_invalid_or_empty_config_does_not_attempt_connection);
    return UNITY_END();
}
