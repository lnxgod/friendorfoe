#include "unity.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "scanner_uart_line_framer.h"

static void assert_ready_frame(scanner_uart_line_event_t event,
                               const uint8_t *expected,
                               size_t expected_len)
{
    TEST_ASSERT_EQUAL_INT(SCANNER_UART_LINE_EVENT_FRAME_READY, event.kind);
    TEST_ASSERT_EQUAL_INT(SCANNER_UART_LINE_REJECT_NONE, event.reject_reason);
    TEST_ASSERT_NOT_NULL(event.bytes);
    TEST_ASSERT_EQUAL_UINT(expected_len, event.byte_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, event.bytes, expected_len);
}

void test_scanner_uart_line_framer_accepts_lf_and_crlf_frames(void)
{
    static const uint8_t payload[] = "{\"type\":\"ready\"}";
    static const uint8_t lf_wire[] = "{\"type\":\"ready\"}\n";
    static const uint8_t crlf_wire[] = "{\"type\":\"ready\"}\r\n";
    uint8_t storage[SCANNER_UART_LINE_BUFFER_SIZE];
    scanner_uart_line_framer_t framer;
    size_t consumed = 99U;

    memset(storage, 0xa5, sizeof(storage));
    TEST_ASSERT_TRUE(scanner_uart_line_framer_init(
        &framer, storage, sizeof(storage)));
    scanner_uart_line_event_t event = scanner_uart_line_framer_consume(
        &framer, lf_wire, sizeof(lf_wire) - 1U, &consumed);
    TEST_ASSERT_EQUAL_UINT(sizeof(lf_wire) - 1U, consumed);
    assert_ready_frame(event, payload, sizeof(payload) - 1U);
    TEST_ASSERT_EQUAL_HEX8(
        0xa5U, storage[sizeof(payload) - 1U]);

    scanner_uart_line_framer_reset(&framer);
    memset(storage, 0xa5, sizeof(storage));
    consumed = 99U;
    event = scanner_uart_line_framer_consume(
        &framer, crlf_wire, sizeof(crlf_wire) - 1U, &consumed);
    TEST_ASSERT_EQUAL_UINT(sizeof(crlf_wire) - 1U, consumed);
    assert_ready_frame(event, payload, sizeof(payload) - 1U);
    TEST_ASSERT_EQUAL_HEX8(
        0xa5U, storage[sizeof(payload) - 1U]);
}

void test_scanner_uart_line_framer_handles_every_lf_and_crlf_split(void)
{
    static const uint8_t payload[] = "{\"type\":\"scanner_quiet\","
                                     "\"enabled\":true,\"generation\":7}";
    static const uint8_t lf_wire[] = "{\"type\":\"scanner_quiet\","
                                    "\"enabled\":true,\"generation\":7}\n";
    static const uint8_t crlf_wire[] = "{\"type\":\"scanner_quiet\","
                                      "\"enabled\":true,\"generation\":7}\r\n";
    const uint8_t *wires[] = {lf_wire, crlf_wire};
    const size_t wire_lens[] = {
        sizeof(lf_wire) - 1U,
        sizeof(crlf_wire) - 1U,
    };

    for (size_t wire_index = 0U; wire_index < 2U; ++wire_index) {
        for (size_t split = 0U; split < wire_lens[wire_index]; ++split) {
            uint8_t storage[SCANNER_UART_LINE_BUFFER_SIZE];
            scanner_uart_line_framer_t framer;
            size_t consumed = 99U;
            TEST_ASSERT_TRUE(scanner_uart_line_framer_init(
                &framer, storage, sizeof(storage)));

            scanner_uart_line_event_t event =
                scanner_uart_line_framer_consume(
                    &framer, wires[wire_index], split, &consumed);
            TEST_ASSERT_EQUAL_UINT(split, consumed);
            TEST_ASSERT_EQUAL_INT(
                SCANNER_UART_LINE_EVENT_NONE, event.kind);

            event = scanner_uart_line_framer_consume(
                &framer,
                wires[wire_index] + split,
                wire_lens[wire_index] - split,
                &consumed);
            TEST_ASSERT_EQUAL_UINT(
                wire_lens[wire_index] - split, consumed);
            assert_ready_frame(event, payload, sizeof(payload) - 1U);
        }
    }
}

void test_scanner_uart_line_framer_skips_empty_lines_and_preserves_remainder(void)
{
    static const uint8_t wire[] =
        "\n\r\n{\"type\":\"ready\"}\n{\"type\":\"stop\"}\r\n";
    static const uint8_t ready[] = "{\"type\":\"ready\"}";
    static const uint8_t stop[] = "{\"type\":\"stop\"}";
    uint8_t storage[SCANNER_UART_LINE_BUFFER_SIZE];
    scanner_uart_line_framer_t framer;
    size_t first_consumed = 0U;

    TEST_ASSERT_TRUE(scanner_uart_line_framer_init(
        &framer, storage, sizeof(storage)));
    scanner_uart_line_event_t event = scanner_uart_line_framer_consume(
        &framer, wire, sizeof(wire) - 1U, &first_consumed);
    assert_ready_frame(event, ready, sizeof(ready) - 1U);
    TEST_ASSERT_GREATER_THAN_UINT(4U, first_consumed);
    TEST_ASSERT_LESS_THAN_UINT(sizeof(wire) - 1U, first_consumed);

    size_t second_consumed = 0U;
    event = scanner_uart_line_framer_consume(
        &framer,
        wire + first_consumed,
        sizeof(wire) - 1U - first_consumed,
        &second_consumed);
    TEST_ASSERT_EQUAL_UINT(
        sizeof(wire) - 1U - first_consumed, second_consumed);
    assert_ready_frame(event, stop, sizeof(stop) - 1U);
}

void test_scanner_uart_line_framer_accepts_exact_4095_byte_payload(void)
{
    uint8_t wire[SCANNER_UART_LINE_BUFFER_SIZE];
    uint8_t storage[SCANNER_UART_LINE_BUFFER_SIZE];
    scanner_uart_line_framer_t framer;
    memset(wire, 'A', SCANNER_UART_LINE_MAX_PAYLOAD);
    wire[SCANNER_UART_LINE_MAX_PAYLOAD] = '\n';

    TEST_ASSERT_TRUE(scanner_uart_line_framer_init(
        &framer, storage, sizeof(storage)));
    size_t consumed = 0U;
    scanner_uart_line_event_t event = scanner_uart_line_framer_consume(
        &framer, wire, sizeof(wire), &consumed);
    TEST_ASSERT_EQUAL_UINT(sizeof(wire), consumed);
    assert_ready_frame(event, wire, SCANNER_UART_LINE_MAX_PAYLOAD);
}

void test_scanner_uart_line_framer_rejects_overflow_without_suffix_resurrection(
    void)
{
    static const uint8_t suffix_and_next[] =
        "{\"type\":\"ready\"}\n{\"type\":\"stop\"}\n";
    static const uint8_t stop[] = "{\"type\":\"stop\"}";
    uint8_t first_chunk[SCANNER_UART_LINE_BUFFER_SIZE];
    uint8_t storage[SCANNER_UART_LINE_BUFFER_SIZE];
    scanner_uart_line_framer_t framer;
    memset(first_chunk, 'A', sizeof(first_chunk));

    TEST_ASSERT_TRUE(scanner_uart_line_framer_init(
        &framer, storage, sizeof(storage)));
    size_t consumed = 0U;
    scanner_uart_line_event_t event = scanner_uart_line_framer_consume(
        &framer, first_chunk, sizeof(first_chunk), &consumed);
    TEST_ASSERT_EQUAL_UINT(sizeof(first_chunk), consumed);
    TEST_ASSERT_EQUAL_INT(
        SCANNER_UART_LINE_EVENT_FRAME_REJECTED, event.kind);
    TEST_ASSERT_EQUAL_INT(
        SCANNER_UART_LINE_REJECT_TOO_LONG, event.reject_reason);
    TEST_ASSERT_NULL(event.bytes);
    TEST_ASSERT_EQUAL_UINT(0U, event.byte_len);

    event = scanner_uart_line_framer_consume(
        &framer, suffix_and_next, sizeof(suffix_and_next) - 1U, &consumed);
    assert_ready_frame(event, stop, sizeof(stop) - 1U);
    TEST_ASSERT_EQUAL_UINT(sizeof(suffix_and_next) - 1U, consumed);
}

void test_scanner_uart_line_framer_rejects_bare_cr_without_suffix_resurrection(
    void)
{
    static const uint8_t wire[] =
        "{\"type\":\"re\rady\"}\n{\"type\":\"stop\"}\n";
    static const uint8_t stop[] = "{\"type\":\"stop\"}";
    uint8_t storage[SCANNER_UART_LINE_BUFFER_SIZE];
    scanner_uart_line_framer_t framer;
    size_t first_consumed = 0U;

    TEST_ASSERT_TRUE(scanner_uart_line_framer_init(
        &framer, storage, sizeof(storage)));
    scanner_uart_line_event_t event = scanner_uart_line_framer_consume(
        &framer, wire, sizeof(wire) - 1U, &first_consumed);
    TEST_ASSERT_EQUAL_INT(
        SCANNER_UART_LINE_EVENT_FRAME_REJECTED, event.kind);
    TEST_ASSERT_EQUAL_INT(
        SCANNER_UART_LINE_REJECT_BARE_CR, event.reject_reason);
    TEST_ASSERT_GREATER_THAN_UINT(1U, first_consumed);
    TEST_ASSERT_LESS_THAN_UINT(sizeof(wire) - 1U, first_consumed);

    size_t second_consumed = 0U;
    event = scanner_uart_line_framer_consume(
        &framer,
        wire + first_consumed,
        sizeof(wire) - 1U - first_consumed,
        &second_consumed);
    TEST_ASSERT_EQUAL_UINT(
        sizeof(wire) - 1U - first_consumed, second_consumed);
    assert_ready_frame(event, stop, sizeof(stop) - 1U);
}

void test_scanner_uart_line_framer_stale_partial_discards_until_next_lf(void)
{
    static const uint8_t partial[] = "{\"type\":\"re";
    static const uint8_t suffix_and_next[] =
        "ady\"}\n{\"type\":\"stop\"}\n";
    static const uint8_t stop[] = "{\"type\":\"stop\"}";
    uint8_t storage[SCANNER_UART_LINE_BUFFER_SIZE];
    scanner_uart_line_framer_t framer;
    size_t consumed = 0U;

    TEST_ASSERT_TRUE(scanner_uart_line_framer_init(
        &framer, storage, sizeof(storage)));
    scanner_uart_line_event_t event = scanner_uart_line_framer_consume(
        &framer, partial, sizeof(partial) - 1U, &consumed);
    TEST_ASSERT_EQUAL_INT(SCANNER_UART_LINE_EVENT_NONE, event.kind);
    TEST_ASSERT_TRUE(scanner_uart_line_framer_has_partial(&framer));

    event = scanner_uart_line_framer_expire_partial(&framer);
    TEST_ASSERT_EQUAL_INT(
        SCANNER_UART_LINE_EVENT_FRAME_REJECTED, event.kind);
    TEST_ASSERT_EQUAL_INT(
        SCANNER_UART_LINE_REJECT_STALE_PARTIAL, event.reject_reason);
    TEST_ASSERT_FALSE(scanner_uart_line_framer_has_partial(&framer));

    event = scanner_uart_line_framer_consume(
        &framer,
        suffix_and_next,
        sizeof(suffix_and_next) - 1U,
        &consumed);
    TEST_ASSERT_EQUAL_UINT(sizeof(suffix_and_next) - 1U, consumed);
    assert_ready_frame(event, stop, sizeof(stop) - 1U);
}

void test_scanner_uart_line_framer_pending_cr_timeout_discards_split_suffix(void)
{
    static const uint8_t partial[] = "{\"type\":\"ready\"}\r";
    static const uint8_t delimiter[] = "\n";
    static const uint8_t next[] = "{\"type\":\"stop\"}\r\n";
    static const uint8_t stop[] = "{\"type\":\"stop\"}";
    uint8_t storage[SCANNER_UART_LINE_BUFFER_SIZE];
    scanner_uart_line_framer_t framer;
    size_t consumed = 0U;

    TEST_ASSERT_TRUE(scanner_uart_line_framer_init(
        &framer, storage, sizeof(storage)));
    scanner_uart_line_event_t event = scanner_uart_line_framer_consume(
        &framer, partial, sizeof(partial) - 1U, &consumed);
    TEST_ASSERT_EQUAL_INT(SCANNER_UART_LINE_EVENT_NONE, event.kind);
    TEST_ASSERT_TRUE(scanner_uart_line_framer_has_partial(&framer));

    event = scanner_uart_line_framer_expire_partial(&framer);
    TEST_ASSERT_EQUAL_INT(
        SCANNER_UART_LINE_EVENT_FRAME_REJECTED, event.kind);
    TEST_ASSERT_EQUAL_INT(
        SCANNER_UART_LINE_REJECT_STALE_PARTIAL, event.reject_reason);

    event = scanner_uart_line_framer_consume(
        &framer, delimiter, sizeof(delimiter) - 1U, &consumed);
    TEST_ASSERT_EQUAL_UINT(sizeof(delimiter) - 1U, consumed);
    TEST_ASSERT_EQUAL_INT(SCANNER_UART_LINE_EVENT_NONE, event.kind);

    event = scanner_uart_line_framer_consume(
        &framer, next, sizeof(next) - 1U, &consumed);
    TEST_ASSERT_EQUAL_UINT(sizeof(next) - 1U, consumed);
    assert_ready_frame(event, stop, sizeof(stop) - 1U);
}

void test_scanner_uart_line_framer_preserves_embedded_nul_for_span_validation(
    void)
{
    static const uint8_t wire[] = {
        '{', '"', 't', 'y', 'p', 'e', '"', ':', '"', 'r', 'e', '\0',
        'a', 'd', 'y', '"', '}', '\n',
    };
    uint8_t storage[SCANNER_UART_LINE_BUFFER_SIZE];
    scanner_uart_line_framer_t framer;
    size_t consumed = 0U;

    TEST_ASSERT_TRUE(scanner_uart_line_framer_init(
        &framer, storage, sizeof(storage)));
    scanner_uart_line_event_t event = scanner_uart_line_framer_consume(
        &framer, wire, sizeof(wire), &consumed);
    TEST_ASSERT_EQUAL_UINT(sizeof(wire), consumed);
    assert_ready_frame(event, wire, sizeof(wire) - 1U);
}

void test_scanner_uart_line_framer_rejects_invalid_api_arguments(void)
{
    uint8_t small_storage[SCANNER_UART_LINE_BUFFER_SIZE - 1U];
    uint8_t storage[SCANNER_UART_LINE_BUFFER_SIZE];
    scanner_uart_line_framer_t framer;
    size_t consumed = 99U;

    TEST_ASSERT_FALSE(scanner_uart_line_framer_init(
        NULL, storage, sizeof(storage)));
    TEST_ASSERT_FALSE(scanner_uart_line_framer_init(
        &framer, NULL, sizeof(storage)));
    TEST_ASSERT_FALSE(scanner_uart_line_framer_init(
        &framer, small_storage, sizeof(small_storage)));
    TEST_ASSERT_TRUE(scanner_uart_line_framer_init(
        &framer, storage, sizeof(storage)));

    scanner_uart_line_event_t event = scanner_uart_line_framer_consume(
        &framer, NULL, 1U, &consumed);
    TEST_ASSERT_EQUAL_INT(
        SCANNER_UART_LINE_EVENT_INVALID_ARGUMENT, event.kind);
    TEST_ASSERT_EQUAL_UINT(0U, consumed);

    event = scanner_uart_line_framer_expire_partial(NULL);
    TEST_ASSERT_EQUAL_INT(
        SCANNER_UART_LINE_EVENT_INVALID_ARGUMENT, event.kind);
}
