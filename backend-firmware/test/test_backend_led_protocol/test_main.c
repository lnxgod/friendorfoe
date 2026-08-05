#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "backend_led_protocol.h"
#include "../support/backend_test_main.h"

void setUp(void)
{
}

void tearDown(void)
{
}

void test_led_command_encodes_exact_wire_json_and_round_trips_states(void)
{
    static const struct {
        backend_led_state_t state;
        const char *json;
    } cases[] = {
        {BACKEND_LED_HEALTHY,
         "{\"type\":\"led_state\",\"state\":\"healthy\","
         "\"generation\":42,\"ttl_ms\":6000}"},
        {BACKEND_LED_NETWORK_DEGRADED,
         "{\"type\":\"led_state\",\"state\":\"network_degraded\","
         "\"generation\":42,\"ttl_ms\":6000}"},
        {BACKEND_LED_DRONE,
         "{\"type\":\"led_state\",\"state\":\"drone\","
         "\"generation\":42,\"ttl_ms\":6000}"},
        {BACKEND_LED_META,
         "{\"type\":\"led_state\",\"state\":\"meta\","
         "\"generation\":42,\"ttl_ms\":6000}"},
        {BACKEND_LED_DRONE_META,
         "{\"type\":\"led_state\",\"state\":\"drone_meta\","
         "\"generation\":42,\"ttl_ms\":6000}"},
        {BACKEND_LED_FATAL,
         "{\"type\":\"led_state\",\"state\":\"fatal\","
         "\"generation\":42,\"ttl_ms\":6000}"},
    };

    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const backend_led_command_t input = {
            .state = cases[index].state,
            .generation = 42,
            .ttl_ms = 6000,
        };
        char output[128] = {0};
        const size_t length =
            backend_led_command_encode(&input, output, sizeof(output));
        TEST_ASSERT_EQUAL_UINT(strlen(cases[index].json), length);
        TEST_ASSERT_EQUAL_STRING(cases[index].json, output);

        backend_led_command_t decoded = {0};
        TEST_ASSERT_TRUE(backend_led_command_decode(output, length, &decoded));
        TEST_ASSERT_EQUAL(input.state, decoded.state);
        TEST_ASSERT_EQUAL_UINT32(input.generation, decoded.generation);
        TEST_ASSERT_EQUAL_UINT32(input.ttl_ms, decoded.ttl_ms);
    }
}

void test_led_command_decode_requires_exact_valid_schema(void)
{
    static const char *invalid[] = {
        "{\"type\":\"wrong\",\"state\":\"drone\","
        "\"generation\":42,\"ttl_ms\":6000}",
        "{\"type\":\"led_state\",\"state\":\"unknown\","
        "\"generation\":42,\"ttl_ms\":6000}",
        "{\"type\":\"led_state\",\"state\":\"uart_lost\","
        "\"generation\":42,\"ttl_ms\":6000}",
        "{\"type\":\"led_state\",\"state\":\"drone\","
        "\"generation\":0,\"ttl_ms\":6000}",
        "{\"type\":\"led_state\",\"state\":\"drone\","
        "\"generation\":42,\"ttl_ms\":1999}",
        "{\"type\":\"led_state\",\"state\":\"drone\","
        "\"generation\":42,\"ttl_ms\":30001}",
        "{\"type\":\"led_state\",\"state\":\"drone\","
        "\"generation\":42}",
        "{\"type\":\"led_state\",\"state\":\"drone\","
        "\"generation\":42,\"ttl_ms\":6000,\"extra\":true}",
        "{\"type\":\"led_state\",\"state\":\"drone\","
        "\"generation\":42,\"generation\":43,\"ttl_ms\":6000}",
        "{\"type\":\"led_state\",\"state\":\"drone\","
        "\"generation\":42,\"ttl_ms\":6000} trailing",
    };
    backend_led_command_t output = {
        .state = BACKEND_LED_FATAL,
        .generation = 91,
        .ttl_ms = 9000,
    };

    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        TEST_ASSERT_FALSE(backend_led_command_decode(
            invalid[index], strlen(invalid[index]), &output));
        TEST_ASSERT_EQUAL(BACKEND_LED_FATAL, output.state);
        TEST_ASSERT_EQUAL_UINT32(91, output.generation);
        TEST_ASSERT_EQUAL_UINT32(9000, output.ttl_ms);
    }

    TEST_ASSERT_FALSE(backend_led_command_decode(NULL, 1, &output));
    TEST_ASSERT_FALSE(backend_led_command_decode("{}", 0, &output));
    TEST_ASSERT_FALSE(backend_led_command_decode("{}", 2, NULL));
}

void test_led_command_accepts_inclusive_ttl_bounds_and_any_field_order(void)
{
    static const char minimum[] =
        "{\"ttl_ms\":2000,\"generation\":1,\"state\":\"healthy\","
        "\"type\":\"led_state\"}";
    static const char maximum[] =
        "{\"state\":\"fatal\",\"ttl_ms\":30000,"
        "\"type\":\"led_state\",\"generation\":4294967295}";
    backend_led_command_t command = {0};

    TEST_ASSERT_TRUE(backend_led_command_decode(
        minimum, sizeof(minimum) - 1, &command));
    TEST_ASSERT_EQUAL(BACKEND_LED_HEALTHY, command.state);
    TEST_ASSERT_EQUAL_UINT32(1, command.generation);
    TEST_ASSERT_EQUAL_UINT32(2000, command.ttl_ms);

    TEST_ASSERT_TRUE(backend_led_command_decode(
        maximum, sizeof(maximum) - 1, &command));
    TEST_ASSERT_EQUAL(BACKEND_LED_FATAL, command.state);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, command.generation);
    TEST_ASSERT_EQUAL_UINT32(30000, command.ttl_ms);
}

void test_led_command_encoder_fails_closed_for_invalid_or_short_output(void)
{
    const backend_led_command_t valid = {
        .state = BACKEND_LED_DRONE,
        .generation = 42,
        .ttl_ms = 6000,
    };
    backend_led_command_t invalid = valid;
    char output[128] = "sentinel";

    TEST_ASSERT_EQUAL_UINT(0,
        backend_led_command_encode(&valid, output, 8));
    TEST_ASSERT_EQUAL_CHAR('\0', output[0]);
    TEST_ASSERT_EQUAL_UINT(0,
        backend_led_command_encode(NULL, output, sizeof(output)));
    TEST_ASSERT_EQUAL_CHAR('\0', output[0]);
    TEST_ASSERT_EQUAL_UINT(0,
        backend_led_command_encode(&valid, NULL, sizeof(output)));

    invalid.state = BACKEND_LED_UART_LOST;
    TEST_ASSERT_EQUAL_UINT(0,
        backend_led_command_encode(&invalid, output, sizeof(output)));
    invalid = valid;
    invalid.generation = 0;
    TEST_ASSERT_EQUAL_UINT(0,
        backend_led_command_encode(&invalid, output, sizeof(output)));
    invalid = valid;
    invalid.ttl_ms = 30001;
    TEST_ASSERT_EQUAL_UINT(0,
        backend_led_command_encode(&invalid, output, sizeof(output)));
}

void test_led_mirror_refreshes_equal_generation_without_restarting_pattern(void)
{
    backend_led_mirror_t mirror;
    backend_led_mirror_init(&mirror);
    backend_led_command_t command = {
        .state = BACKEND_LED_DRONE,
        .generation = 42,
        .ttl_ms = 6000,
    };
    TEST_ASSERT_EQUAL(BACKEND_LED_ACCEPTED_NEW,
        backend_led_mirror_accept(&mirror, &command, 1000));
    uint32_t transitions = mirror.pattern_transition_count;
    TEST_ASSERT_EQUAL(BACKEND_LED_ACCEPTED_REFRESH,
        backend_led_mirror_accept(&mirror, &command, 3000));
    TEST_ASSERT_EQUAL(BACKEND_LED_ACCEPTED_REFRESH,
        backend_led_mirror_accept(&mirror, &command, 5000));
    TEST_ASSERT_EQUAL(BACKEND_LED_ACCEPTED_REFRESH,
        backend_led_mirror_accept(&mirror, &command, 7000));
    TEST_ASSERT_EQUAL_UINT32(transitions, mirror.pattern_transition_count);
    TEST_ASSERT_EQUAL(BACKEND_LED_DRONE,
                      backend_led_mirror_effective(&mirror, 12999));
    TEST_ASSERT_EQUAL(BACKEND_LED_UART_LOST,
                      backend_led_mirror_effective(&mirror, 13000));

    backend_led_command_t conflict = command;
    conflict.state = BACKEND_LED_META;
    TEST_ASSERT_EQUAL(BACKEND_LED_REJECTED_CONFLICT,
        backend_led_mirror_accept(&mirror, &conflict, 8000));
    conflict = command;
    conflict.generation = 41;
    TEST_ASSERT_EQUAL(BACKEND_LED_REJECTED_STALE,
        backend_led_mirror_accept(&mirror, &conflict, 8000));
}

void test_led_mirror_only_restarts_when_larger_generation_changes_state(void)
{
    backend_led_mirror_t mirror;
    backend_led_mirror_init(&mirror);
    backend_led_command_t command = {
        .state = BACKEND_LED_META,
        .generation = 1,
        .ttl_ms = 6000,
    };

    TEST_ASSERT_EQUAL(BACKEND_LED_UART_LOST,
                      backend_led_mirror_effective(&mirror, 0));
    TEST_ASSERT_EQUAL(BACKEND_LED_ACCEPTED_NEW,
        backend_led_mirror_accept(&mirror, &command, 0));
    TEST_ASSERT_EQUAL_UINT32(1, mirror.pattern_transition_count);

    command.generation = 2;
    command.ttl_ms = 8000;
    TEST_ASSERT_EQUAL(BACKEND_LED_ACCEPTED_NEW,
        backend_led_mirror_accept(&mirror, &command, 100));
    TEST_ASSERT_EQUAL_UINT32(1, mirror.pattern_transition_count);

    command.generation = 3;
    command.state = BACKEND_LED_DRONE_META;
    TEST_ASSERT_EQUAL(BACKEND_LED_ACCEPTED_NEW,
        backend_led_mirror_accept(&mirror, &command, 200));
    TEST_ASSERT_EQUAL_UINT32(2, mirror.pattern_transition_count);
    TEST_ASSERT_EQUAL(BACKEND_LED_DRONE_META,
                      backend_led_mirror_effective(&mirror, 8199));
    TEST_ASSERT_EQUAL(BACKEND_LED_UART_LOST,
                      backend_led_mirror_effective(&mirror, 8200));
}

void test_led_mirror_rejects_invalid_input_without_mutating_acceptance(void)
{
    backend_led_mirror_t mirror;
    backend_led_mirror_init(&mirror);
    backend_led_command_t command = {
        .state = BACKEND_LED_HEALTHY,
        .generation = 7,
        .ttl_ms = 6000,
    };

    TEST_ASSERT_EQUAL(BACKEND_LED_REJECTED_INVALID,
        backend_led_mirror_accept(NULL, &command, 0));
    TEST_ASSERT_EQUAL(BACKEND_LED_REJECTED_INVALID,
        backend_led_mirror_accept(&mirror, NULL, 0));
    TEST_ASSERT_EQUAL(BACKEND_LED_REJECTED_INVALID,
        backend_led_mirror_accept(&mirror, &command, -1));
    TEST_ASSERT_FALSE(mirror.has_accepted);

    TEST_ASSERT_EQUAL(BACKEND_LED_ACCEPTED_NEW,
        backend_led_mirror_accept(&mirror, &command, 1000));
    backend_led_command_t conflict = command;
    conflict.ttl_ms = 7000;
    TEST_ASSERT_EQUAL(BACKEND_LED_REJECTED_CONFLICT,
        backend_led_mirror_accept(&mirror, &conflict, 2000));
    TEST_ASSERT_EQUAL_UINT32(6000, mirror.accepted.ttl_ms);
    TEST_ASSERT_EQUAL_INT64(1000, mirror.accepted_monotonic_ms);

    TEST_ASSERT_EQUAL(BACKEND_LED_UART_LOST,
                      backend_led_mirror_effective(NULL, 1000));
    TEST_ASSERT_EQUAL(BACKEND_LED_UART_LOST,
                      backend_led_mirror_effective(&mirror, 999));
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_led_command_encodes_exact_wire_json_and_round_trips_states);
    BACKEND_RUN_TEST(test_led_command_decode_requires_exact_valid_schema);
    BACKEND_RUN_TEST(
        test_led_command_accepts_inclusive_ttl_bounds_and_any_field_order);
    BACKEND_RUN_TEST(
        test_led_command_encoder_fails_closed_for_invalid_or_short_output);
    BACKEND_RUN_TEST(
        test_led_mirror_refreshes_equal_generation_without_restarting_pattern);
    BACKEND_RUN_TEST(
        test_led_mirror_only_restarts_when_larger_generation_changes_state);
    BACKEND_RUN_TEST(
        test_led_mirror_rejects_invalid_input_without_mutating_acceptance);
    return UNITY_END();
}
