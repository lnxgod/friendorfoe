#include "unity.h"

#include <stdint.h>

#include "badge_usb_stream.h"

static badge_usb_stream_t stream_with_line(char *line, size_t line_capacity)
{
    badge_usb_stream_t state;
    badge_usb_stream_init(&state, line, line_capacity);
    return state;
}

void test_badge_usb_stream_reports_completed_line_byte_length(void)
{
    static const uint8_t payload[] = "FOF_SET:wifi_pass=a=b==";
    char line[64];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));
    badge_usb_stream_result_t result;

    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_NONE,
        badge_usb_stream_feed(&state, payload, 8U, 1U, &result));
    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_NONE,
        badge_usb_stream_feed(
            &state, payload + 8U, sizeof(payload) - 1U - 8U,
            2U, &result));
    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_LINE,
        badge_usb_stream_feed(
            &state, (const uint8_t *)"\n", 1U, 3U, &result));
    TEST_ASSERT_EQUAL_UINT(sizeof(payload) - 1U, result.line_byte_len);
    TEST_ASSERT_EQUAL_MEMORY(
        payload, result.line, sizeof(payload) - 1U);
    TEST_ASSERT_EQUAL_UINT(0U, state.line_length);
}

void test_badge_usb_stream_line_length_preserves_embedded_nul(void)
{
    static const uint8_t payload[] = {'A', 0U, 'B', '\n'};
    static const uint8_t expected[] = {'A', 0U, 'B'};
    char line[8];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));
    badge_usb_stream_result_t result;

    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_NONE,
        badge_usb_stream_feed(&state, payload, 2U, 1U, &result));
    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_LINE,
        badge_usb_stream_feed(
            &state, payload + 2U, sizeof(payload) - 2U,
            2U, &result));
    TEST_ASSERT_EQUAL_UINT(sizeof(expected), result.line_byte_len);
    TEST_ASSERT_EQUAL_MEMORY(expected, result.line, sizeof(expected));
    TEST_ASSERT_EQUAL_UINT(0U, state.line_length);
}

void test_badge_usb_stream_crlf_splits_and_coalesced_lines_keep_spans(void)
{
    static const uint8_t wire[] =
        "FOF_SET:wifi_pass=a=b==\r\n";
    const size_t wire_len = sizeof(wire) - 1U;
    const size_t payload_len = wire_len - 2U;
    char line[64];
    badge_usb_stream_t state;
    badge_usb_stream_result_t result;

    state = stream_with_line(line, sizeof(line));
    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_LINE,
        badge_usb_stream_feed(
            &state, wire, wire_len, 1U, &result));
    TEST_ASSERT_EQUAL_UINT(payload_len, result.line_byte_len);
    TEST_ASSERT_EQUAL_MEMORY(wire, result.line, payload_len);
    TEST_ASSERT_EQUAL_UINT(wire_len, result.input_consumed);

    for (size_t split = 1U; split < wire_len; split++) {
        badge_usb_stream_init(&state, line, sizeof(line));
        TEST_ASSERT_EQUAL(
            BADGE_USB_EVENT_NONE,
            badge_usb_stream_feed(
                &state, wire, split, 2U, &result));
        TEST_ASSERT_EQUAL_UINT(split, result.input_consumed);
        TEST_ASSERT_EQUAL(
            BADGE_USB_EVENT_LINE,
            badge_usb_stream_feed(
                &state, wire + split, wire_len - split,
                3U, &result));
        TEST_ASSERT_EQUAL_UINT(payload_len, result.line_byte_len);
        TEST_ASSERT_EQUAL_MEMORY(wire, result.line, payload_len);
        TEST_ASSERT_EQUAL_UINT(wire_len - split, result.input_consumed);
    }

    static const uint8_t coalesced[] = "A\nBC\r\n";
    badge_usb_stream_init(&state, line, sizeof(line));
    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_LINE,
        badge_usb_stream_feed(
            &state, coalesced, sizeof(coalesced) - 1U,
            4U, &result));
    TEST_ASSERT_EQUAL_UINT(1U, result.line_byte_len);
    TEST_ASSERT_EQUAL_MEMORY("A", result.line, 1U);
    TEST_ASSERT_EQUAL_UINT(2U, result.input_consumed);

    const size_t first_consumed = result.input_consumed;
    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_LINE,
        badge_usb_stream_feed(
            &state, coalesced + first_consumed,
            sizeof(coalesced) - 1U - first_consumed,
            5U, &result));
    TEST_ASSERT_EQUAL_UINT(2U, result.line_byte_len);
    TEST_ASSERT_EQUAL_MEMORY("BC", result.line, 2U);
    TEST_ASSERT_EQUAL_UINT(4U, result.input_consumed);
}

void test_badge_usb_stream_rejects_cr_inside_frame_and_recovers(void)
{
    static const uint8_t after_bare_cr[] = "D\nOK\n";
    static const uint8_t in_frame_cr[] =
        "FOF_SET:wifi_pass=a\rb\nOK\n";
    char line[64];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));
    badge_usb_stream_result_t result;

    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_NONE,
        badge_usb_stream_feed(
            &state, (const uint8_t *)"ABC\r", 4U,
            1U, &result));
    TEST_ASSERT_EQUAL_UINT(4U, result.input_consumed);

    size_t offset = 0U;
    size_t lines = 0U;
    while (offset < sizeof(after_bare_cr) - 1U) {
        badge_usb_stream_event_t event = badge_usb_stream_feed(
            &state, after_bare_cr + offset,
            sizeof(after_bare_cr) - 1U - offset,
            2U, &result);
        TEST_ASSERT_GREATER_THAN_UINT(0U, result.input_consumed);
        if (event == BADGE_USB_EVENT_LINE) {
            TEST_ASSERT_EQUAL_UINT(2U, result.line_byte_len);
            TEST_ASSERT_EQUAL_MEMORY("OK", result.line, 2U);
            lines++;
        }
        offset += result.input_consumed;
    }
    TEST_ASSERT_EQUAL_UINT(1U, lines);

    badge_usb_stream_init(&state, line, sizeof(line));
    offset = 0U;
    lines = 0U;
    while (offset < sizeof(in_frame_cr) - 1U) {
        badge_usb_stream_event_t event = badge_usb_stream_feed(
            &state, in_frame_cr + offset,
            sizeof(in_frame_cr) - 1U - offset,
            3U, &result);
        TEST_ASSERT_GREATER_THAN_UINT(0U, result.input_consumed);
        if (event == BADGE_USB_EVENT_LINE) {
            TEST_ASSERT_EQUAL_UINT(2U, result.line_byte_len);
            TEST_ASSERT_EQUAL_MEMORY("OK", result.line, 2U);
            lines++;
        }
        offset += result.input_consumed;
    }
    TEST_ASSERT_EQUAL_UINT(1U, lines);
}

void test_badge_usb_stream_oversize_crlf_recovers_without_reviving(void)
{
    char line[4];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));
    badge_usb_stream_result_t result;
    static const uint8_t recovery[] = "\nOK\n";

    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_ERROR,
        badge_usb_stream_feed(
            &state, (const uint8_t *)"ABCD", 4U,
            1U, &result));
    TEST_ASSERT_TRUE(state.discarding_oversize_line);
    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_NONE,
        badge_usb_stream_feed(
            &state, (const uint8_t *)"\r", 1U,
            2U, &result));
    TEST_ASSERT_TRUE(state.discarding_oversize_line);
    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_NONE,
        badge_usb_stream_feed(
            &state, recovery, sizeof(recovery) - 1U,
            3U, &result));
    TEST_ASSERT_EQUAL_UINT(1U, result.input_consumed);
    TEST_ASSERT_FALSE(state.discarding_oversize_line);
    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_LINE,
        badge_usb_stream_feed(
            &state, recovery + result.input_consumed,
            sizeof(recovery) - 1U - result.input_consumed,
            4U, &result));
    TEST_ASSERT_EQUAL_UINT(2U, result.line_byte_len);
    TEST_ASSERT_EQUAL_MEMORY("OK", result.line, 2U);
}

void test_badge_usb_stream_binary_crlf_is_opaque_after_line_cr(void)
{
    static const uint8_t input[] = {
        0x0dU, 0x0aU, 'O', 'K', '\n',
    };
    char line[8];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));
    badge_usb_stream_result_t result;

    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_NONE,
        badge_usb_stream_feed(
            &state, (const uint8_t *)"\r", 1U,
            1U, &result));
    TEST_ASSERT_TRUE(badge_usb_stream_begin_binary(
        &state, BADGE_USB_BINARY_SCANNER, 2U, 2U));
    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_BINARY_COMPLETE,
        badge_usb_stream_feed(
            &state, input, sizeof(input), 3U, &result));
    TEST_ASSERT_EQUAL_UINT(2U, result.bytes_len);
    TEST_ASSERT_EQUAL_MEMORY(input, result.bytes, 2U);
    TEST_ASSERT_EQUAL_UINT(2U, result.input_consumed);

    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_LINE,
        badge_usb_stream_feed(
            &state, input + result.input_consumed,
            sizeof(input) - result.input_consumed,
            4U, &result));
    TEST_ASSERT_EQUAL_UINT(2U, result.line_byte_len);
    TEST_ASSERT_EQUAL_MEMORY("OK", result.line, 2U);
}

void test_badge_usb_stream_pending_cr_clears_on_line_and_binary_transitions(
    void)
{
    static const uint8_t binary[] = {0x41U, 0x42U};
    char line[8];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));
    badge_usb_stream_result_t result;

    TEST_ASSERT_FALSE(state.pending_cr);
    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_NONE,
        badge_usb_stream_feed(
            &state, (const uint8_t *)"OK\r", 3U,
            1U, &result));
    TEST_ASSERT_TRUE(state.pending_cr);
    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_LINE,
        badge_usb_stream_feed(
            &state, (const uint8_t *)"\n", 1U,
            2U, &result));
    TEST_ASSERT_FALSE(state.pending_cr);

    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_NONE,
        badge_usb_stream_feed(
            &state, (const uint8_t *)"\r", 1U,
            3U, &result));
    TEST_ASSERT_TRUE(state.pending_cr);
    TEST_ASSERT_FALSE(badge_usb_stream_begin_binary(
        &state, BADGE_USB_BINARY_NONE, 2U, 4U));
    TEST_ASSERT_TRUE(state.pending_cr);
    TEST_ASSERT_TRUE(badge_usb_stream_begin_binary(
        &state, BADGE_USB_BINARY_SCANNER, 2U, 5U));
    TEST_ASSERT_FALSE(state.pending_cr);

    state.pending_cr = true;
    badge_usb_stream_clear_binary(&state);
    TEST_ASSERT_FALSE(state.pending_cr);

    TEST_ASSERT_TRUE(badge_usb_stream_begin_binary(
        &state, BADGE_USB_BINARY_SCANNER, 2U, 6U));
    state.pending_cr = true;
    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_BINARY_COMPLETE,
        badge_usb_stream_feed(
            &state, binary, sizeof(binary), 7U, &result));
    TEST_ASSERT_FALSE(state.pending_cr);
}

void test_badge_usb_stream_pending_cr_clears_on_errors_abort_and_timeout(
    void)
{
    static const uint8_t byte[] = {0x41U};
    char line[8];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));
    badge_usb_stream_result_t result;

    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_NONE,
        badge_usb_stream_feed(
            &state, (const uint8_t *)"\r", 1U,
            1U, &result));
    TEST_ASSERT_TRUE(state.pending_cr);
    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_ERROR,
        badge_usb_stream_feed(&state, NULL, 1U, 2U, &result));
    TEST_ASSERT_FALSE(state.pending_cr);

    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_NONE,
        badge_usb_stream_feed(
            &state, (const uint8_t *)"\r", 1U,
            3U, &result));
    TEST_ASSERT_TRUE(state.pending_cr);
    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_ERROR,
        badge_usb_stream_feed(
            &state, byte, sizeof(byte), 4U, NULL));
    TEST_ASSERT_FALSE(state.pending_cr);

    TEST_ASSERT_TRUE(badge_usb_stream_begin_binary(
        &state, BADGE_USB_BINARY_UPLINK, 2U, 100U));
    state.pending_cr = true;
    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_ERROR,
        badge_usb_stream_poll_timeout(&state, 5100U, 5000U));
    TEST_ASSERT_FALSE(state.pending_cr);

    TEST_ASSERT_TRUE(badge_usb_stream_begin_binary(
        &state, BADGE_USB_BINARY_SCANNER, 2U, 6U));
    state.pending_cr = true;
    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_ERROR,
        badge_usb_stream_abort(&state, "cancelled", &result));
    TEST_ASSERT_FALSE(state.pending_cr);

    TEST_ASSERT_TRUE(badge_usb_stream_begin_binary(
        &state, BADGE_USB_BINARY_UPLINK, 2U, 7U));
    state.pending_cr = true;
    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_ERROR,
        badge_usb_stream_feed(&state, NULL, 1U, 8U, &result));
    TEST_ASSERT_FALSE(state.pending_cr);

    char short_line[4];
    badge_usb_stream_init(&state, short_line, sizeof(short_line));
    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_ERROR,
        badge_usb_stream_feed(
            &state, (const uint8_t *)"ABCD", 4U,
            9U, &result));
    TEST_ASSERT_FALSE(state.pending_cr);
    TEST_ASSERT_EQUAL(
        BADGE_USB_EVENT_NONE,
        badge_usb_stream_feed(
            &state, (const uint8_t *)"\r\n", 2U,
            10U, &result));
    TEST_ASSERT_FALSE(state.pending_cr);
}

void test_badge_usb_stream_parses_fragmented_lf_and_crlf_commands(void)
{
    char line[16];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));
    badge_usb_stream_result_t result;

    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_NONE,
                      badge_usb_stream_feed(&state, (const uint8_t *)"PING\r", 5U,
                                            10U, &result));
    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_LINE,
                      badge_usb_stream_feed(&state, (const uint8_t *)"\n", 1U,
                                            11U, &result));
    TEST_ASSERT_EQUAL_STRING("PING", result.line);
    TEST_ASSERT_EQUAL_UINT(1U, result.input_consumed);
}

void test_badge_usb_stream_keeps_multiple_commands_separate(void)
{
    char line[16];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));
    badge_usb_stream_result_t result;
    const uint8_t input[] = "ONE\nTWO\n";

    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_LINE,
                      badge_usb_stream_feed(&state, input, sizeof(input) - 1U,
                                            10U, &result));
    TEST_ASSERT_EQUAL_STRING("ONE", result.line);
    TEST_ASSERT_EQUAL_UINT(4U, result.input_consumed);
    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_LINE,
                      badge_usb_stream_feed(&state, input + result.input_consumed,
                                            sizeof(input) - 1U - result.input_consumed,
                                            11U, &result));
    TEST_ASSERT_EQUAL_STRING("TWO", result.line);
    TEST_ASSERT_EQUAL_UINT(4U, result.input_consumed);
}

void test_badge_usb_stream_reports_exact_binary_boundary(void)
{
    char line[16];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));
    badge_usb_stream_result_t result;
    const uint8_t payload[] = {1U, 2U, 3U, 4U};

    TEST_ASSERT_TRUE(badge_usb_stream_begin_binary(&state, BADGE_USB_BINARY_SCANNER,
                                                    4U, 10U));
    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_BINARY_COMPLETE,
                      badge_usb_stream_feed(&state, payload, sizeof(payload), 11U,
                                            &result));
    TEST_ASSERT_EQUAL_UINT(4U, result.input_consumed);
    TEST_ASSERT_EQUAL_UINT(4U, result.bytes_len);
    TEST_ASSERT_EQUAL(BADGE_USB_BINARY_SCANNER, result.target);
    TEST_ASSERT_EQUAL(BADGE_USB_BINARY_NONE, state.target);
}

void test_badge_usb_stream_leaves_command_after_binary_for_next_feed(void)
{
    char line[16];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));
    badge_usb_stream_result_t result;
    const uint8_t input[] = {0xa0U, 0xa1U, 'O', 'K', '\n'};

    TEST_ASSERT_TRUE(badge_usb_stream_begin_binary(&state, BADGE_USB_BINARY_UPLINK,
                                                    2U, 10U));
    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_BINARY_COMPLETE,
                      badge_usb_stream_feed(&state, input, sizeof(input), 11U,
                                            &result));
    TEST_ASSERT_EQUAL_UINT(2U, result.input_consumed);
    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_LINE,
                      badge_usb_stream_feed(&state, input + result.input_consumed,
                                            sizeof(input) - result.input_consumed,
                                            11U, &result));
    TEST_ASSERT_EQUAL_STRING("OK", result.line);
}

void test_badge_usb_stream_scanner_completion_keeps_scanner_owner(void)
{
    char line[8];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));
    badge_usb_stream_result_t result;
    const uint8_t byte[] = {0U};

    TEST_ASSERT_TRUE(badge_usb_stream_begin_binary(&state, BADGE_USB_BINARY_SCANNER,
                                                    1U, 1U));
    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_BINARY_COMPLETE,
                      badge_usb_stream_feed(&state, byte, sizeof(byte), 2U, &result));
    TEST_ASSERT_EQUAL(BADGE_USB_BINARY_SCANNER, result.target);
    TEST_ASSERT_NOT_EQUAL(BADGE_USB_BINARY_UPLINK, result.target);
}

void test_badge_usb_stream_uplink_completion_keeps_uplink_owner(void)
{
    char line[8];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));
    badge_usb_stream_result_t result;
    const uint8_t byte[] = {0U};

    TEST_ASSERT_TRUE(badge_usb_stream_begin_binary(&state, BADGE_USB_BINARY_UPLINK,
                                                    1U, 1U));
    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_BINARY_COMPLETE,
                      badge_usb_stream_feed(&state, byte, sizeof(byte), 2U, &result));
    TEST_ASSERT_EQUAL(BADGE_USB_BINARY_UPLINK, result.target);
    TEST_ASSERT_NOT_EQUAL(BADGE_USB_BINARY_SCANNER, result.target);
}

void test_badge_usb_stream_uplink_peek_commit_retains_target_until_finish(void)
{
    char line[8];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));
    badge_usb_stream_result_t result;
    const uint8_t input[] = {0xa0U, 0xa1U, 'O', 'K', '\n'};

    TEST_ASSERT_TRUE(badge_usb_stream_begin_binary(
        &state, BADGE_USB_BINARY_UPLINK, 2U, 10U));
    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_BINARY_COMPLETE,
                      badge_usb_stream_peek_binary(
                          &state, input, sizeof(input), 2U, &result));
    TEST_ASSERT_EQUAL_UINT(2U, result.input_consumed);
    TEST_ASSERT_EQUAL_UINT32(0U, state.received);
    TEST_ASSERT_EQUAL(BADGE_USB_BINARY_UPLINK, state.target);

    TEST_ASSERT_TRUE(badge_usb_stream_commit_binary(&state, &result, 11U));
    TEST_ASSERT_EQUAL_UINT32(2U, state.received);
    TEST_ASSERT_EQUAL(BADGE_USB_BINARY_UPLINK, state.target);
    badge_usb_stream_clear_binary(&state);
    TEST_ASSERT_EQUAL(BADGE_USB_BINARY_NONE, state.target);
}

void test_badge_usb_stream_uplink_timeout_decision_is_nonmutating(void)
{
    char line[8];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));

    TEST_ASSERT_TRUE(badge_usb_stream_begin_binary(
        &state, BADGE_USB_BINARY_UPLINK, 4U, 100U));
    TEST_ASSERT_TRUE(badge_usb_stream_binary_timed_out(
        &state, 5100U, 5000U));
    TEST_ASSERT_EQUAL(BADGE_USB_BINARY_UPLINK, state.target);
    TEST_ASSERT_EQUAL_UINT32(4U, state.exact_size);
    TEST_ASSERT_EQUAL_UINT32(0U, state.received);
}

void test_badge_usb_stream_discards_overflowed_line_then_recovers(void)
{
    char line[4];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));
    badge_usb_stream_result_t result;

    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_ERROR,
                      badge_usb_stream_feed(&state, (const uint8_t *)"ABCD", 4U,
                                            1U, &result));
    TEST_ASSERT_TRUE(state.discarding_oversize_line);
    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_NONE,
                      badge_usb_stream_feed(&state, (const uint8_t *)"\n", 1U,
                                            2U, &result));
    TEST_ASSERT_FALSE(state.discarding_oversize_line);
    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_LINE,
                      badge_usb_stream_feed(&state, (const uint8_t *)"OK\n", 3U,
                                            3U, &result));
    TEST_ASSERT_EQUAL_STRING("OK", result.line);
}

void test_badge_usb_stream_times_out_binary_after_five_seconds(void)
{
    char line[8];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));

    TEST_ASSERT_TRUE(badge_usb_stream_begin_binary(&state, BADGE_USB_BINARY_SCANNER,
                                                    2U, 100U));
    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_ERROR,
                      badge_usb_stream_poll_timeout(&state, 5100U, 5000U));
    TEST_ASSERT_EQUAL(BADGE_USB_BINARY_NONE, state.target);
}

void test_badge_usb_stream_stale_pre_begin_tick_does_not_abort_new_upload(void)
{
    char line[8];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));

    /* The staging backend can block before begin_binary records its newer
     * timestamp. A loop tick captured before that delay must not wrap into an
     * immediate timeout. */
    TEST_ASSERT_TRUE(badge_usb_stream_begin_binary(&state, BADGE_USB_BINARY_SCANNER,
                                                    2U, 1200U));
    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_NONE,
                      badge_usb_stream_poll_timeout(&state, 1000U, 5000U));
    TEST_ASSERT_EQUAL(BADGE_USB_BINARY_SCANNER, state.target);
    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_NONE,
                      badge_usb_stream_poll_timeout(&state, 6199U, 5000U));
    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_ERROR,
                      badge_usb_stream_poll_timeout(&state, 6200U, 5000U));
}

void test_badge_usb_stream_abort_clears_binary_ownership(void)
{
    char line[8];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));
    badge_usb_stream_result_t result;

    TEST_ASSERT_TRUE(badge_usb_stream_begin_binary(&state, BADGE_USB_BINARY_UPLINK,
                                                    2U, 1U));
    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_ERROR,
                      badge_usb_stream_abort(&state, "cancelled", &result));
    TEST_ASSERT_EQUAL_STRING("cancelled", result.error);
    TEST_ASSERT_EQUAL(BADGE_USB_BINARY_UPLINK, result.target);
    TEST_ASSERT_EQUAL(BADGE_USB_BINARY_NONE, state.target);
}

void test_badge_usb_stream_timeout_handles_uint32_wrap(void)
{
    char line[8];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));

    TEST_ASSERT_TRUE(badge_usb_stream_begin_binary(&state, BADGE_USB_BINARY_SCANNER,
                                                    2U, UINT32_MAX - 500U));
    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_ERROR,
                      badge_usb_stream_poll_timeout(&state, 4499U, 5000U));
}

void test_badge_usb_stream_rejects_zero_size_binary(void)
{
    char line[8];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));

    TEST_ASSERT_FALSE(badge_usb_stream_begin_binary(&state, BADGE_USB_BINARY_SCANNER,
                                                     0U, 1U));
    TEST_ASSERT_EQUAL(BADGE_USB_BINARY_NONE, state.target);
}

void test_badge_usb_stream_invalid_source_aborts_active_binary_with_target(void)
{
    char line[8];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));
    badge_usb_stream_result_t result;

    TEST_ASSERT_TRUE(badge_usb_stream_begin_binary(&state, BADGE_USB_BINARY_UPLINK,
                                                    3U, 1U));
    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_ERROR,
                      badge_usb_stream_feed(&state, NULL, 1U, 2U, &result));
    TEST_ASSERT_EQUAL(BADGE_USB_BINARY_UPLINK, result.target);
    TEST_ASSERT_EQUAL(BADGE_USB_BINARY_NONE, state.target);
    TEST_ASSERT_EQUAL_UINT32(0U, state.exact_size);
    TEST_ASSERT_EQUAL_UINT32(0U, state.received);
}

void test_badge_usb_stream_null_result_aborts_active_binary(void)
{
    char line[8];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));
    const uint8_t byte[] = {0U};

    TEST_ASSERT_TRUE(badge_usb_stream_begin_binary(&state, BADGE_USB_BINARY_SCANNER,
                                                    3U, 1U));
    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_ERROR,
                      badge_usb_stream_feed(&state, byte, sizeof(byte), 2U, NULL));
    TEST_ASSERT_EQUAL(BADGE_USB_BINARY_NONE, state.target);
    TEST_ASSERT_EQUAL_UINT32(0U, state.exact_size);
    TEST_ASSERT_EQUAL_UINT32(0U, state.received);
}

void test_badge_usb_stream_accepts_exact_capacity_lf_and_fragmented_crlf(void)
{
    char line[4];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));
    badge_usb_stream_result_t result;

    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_LINE,
                      badge_usb_stream_feed(&state, (const uint8_t *)"ABC\n", 4U,
                                            1U, &result));
    TEST_ASSERT_EQUAL_STRING("ABC", result.line);

    badge_usb_stream_init(&state, line, sizeof(line));
    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_NONE,
                      badge_usb_stream_feed(&state, (const uint8_t *)"ABC\r", 4U,
                                            2U, &result));
    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_LINE,
                      badge_usb_stream_feed(&state, (const uint8_t *)"\n", 1U,
                                            3U, &result));
    TEST_ASSERT_EQUAL_STRING("ABC", result.line);
}

void test_badge_usb_stream_fragmented_cr_reports_consumed_before_lf(void)
{
    char line[4];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));
    badge_usb_stream_result_t result;

    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_NONE,
                      badge_usb_stream_feed(&state, (const uint8_t *)"ABC\r", 4U,
                                            1U, &result));
    TEST_ASSERT_EQUAL_UINT(4U, result.input_consumed);
    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_LINE,
                      badge_usb_stream_feed(&state, (const uint8_t *)"\n", 1U,
                                            2U, &result));
    TEST_ASSERT_EQUAL_UINT(1U, result.input_consumed);
    TEST_ASSERT_EQUAL_STRING("ABC", result.line);
}

void test_badge_usb_stream_null_result_abort_releases_binary_for_line_command(void)
{
    char line[8];
    badge_usb_stream_t state = stream_with_line(line, sizeof(line));
    badge_usb_stream_result_t result;

    TEST_ASSERT_TRUE(badge_usb_stream_begin_binary(&state, BADGE_USB_BINARY_SCANNER,
                                                    3U, 1U));
    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_ERROR,
                      badge_usb_stream_abort(&state, "cancelled", NULL));
    TEST_ASSERT_EQUAL(BADGE_USB_BINARY_NONE, state.target);
    TEST_ASSERT_EQUAL(BADGE_USB_EVENT_LINE,
                      badge_usb_stream_feed(&state, (const uint8_t *)"OK\n", 3U,
                                            2U, &result));
    TEST_ASSERT_EQUAL_STRING("OK", result.line);
}
