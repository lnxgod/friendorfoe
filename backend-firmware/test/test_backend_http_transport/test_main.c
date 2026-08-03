#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "backend_http_transport.h"
#include "../support/backend_test_main.h"

#define SCRIPTED_SEND_STEPS 32U

typedef struct {
    const uint8_t *response;
    size_t response_length;
    size_t response_position;
    size_t split_at;
    bool split_pending;
    size_t receive_limit;
    size_t send_limit;
    size_t send_amounts[SCRIPTED_SEND_STEPS];
    int64_t send_times_ms[SCRIPTED_SEND_STEPS];
    uint32_t send_timeouts_ms[SCRIPTED_SEND_STEPS];
    size_t send_schedule_count;
    char sent[8192];
    size_t sent_length;
    char resolved_host[256];
    uint16_t resolved_port;
    uint32_t resolve_timeout_ms;
    uint32_t connect_timeout_ms;
    size_t resolve_calls;
    size_t connect_calls;
    size_t close_calls;
    size_t send_calls;
    size_t receive_calls;
    bool resolve_ok;
    bool connect_ok;
    int64_t now_ms;
    bool resolve_clock_enabled;
    int64_t resolve_complete_ms;
    bool connect_clock_enabled;
    int64_t connect_complete_ms;
    size_t jump_at_position;
    int64_t jump_to_ms;
    bool jump_pending;
    size_t paced_body_start;
    size_t paced_body_length;
    int64_t paced_final_ms;
} scripted_socket_t;

typedef struct {
    uint8_t bytes[64];
    size_t length;
    size_t calls;
    size_t fail_on_call;
} sink_fixture_t;

void setUp(void)
{
    backend_http_reset_io();
}

void tearDown(void)
{
    backend_http_reset_io();
}

static bool scripted_resolve(
    void *context,
    const char *host,
    uint16_t port,
    uint32_t timeout_ms,
    backend_http_resolved_address_t *out)
{
    scripted_socket_t *script = context;
    script->resolve_calls++;
    snprintf(script->resolved_host, sizeof(script->resolved_host), "%s", host);
    script->resolved_port = port;
    script->resolve_timeout_ms = timeout_ms;
    if (script->resolve_clock_enabled) {
        script->now_ms = script->resolve_complete_ms;
    }
    if (!script->resolve_ok) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->bytes[0] = UINT8_C(0xA5);
    out->length = 1U;
    return true;
}

static int scripted_connect(
    void *context,
    const backend_http_resolved_address_t *address,
    uint32_t timeout_ms)
{
    scripted_socket_t *script = context;
    script->connect_calls++;
    script->connect_timeout_ms = timeout_ms;
    if (script->connect_clock_enabled) {
        script->now_ms = script->connect_complete_ms;
    }
    if (!script->connect_ok || !address || address->length != 1U ||
        address->bytes[0] != UINT8_C(0xA5)) {
        return BACKEND_HTTP_INVALID_SOCKET;
    }
    return 17;
}

static ssize_t scripted_send(
    void *context,
    int socket_handle,
    const void *bytes,
    size_t length,
    uint32_t timeout_ms)
{
    scripted_socket_t *script = context;
    if (socket_handle != 17 || !bytes || length == 0U) {
        return -1;
    }
    const size_t call = script->send_calls;
    if (call < SCRIPTED_SEND_STEPS) {
        script->send_timeouts_ms[call] = timeout_ms;
    }
    size_t amount = length;
    if (call < script->send_schedule_count) {
        script->now_ms = script->send_times_ms[call];
        if (script->send_amounts[call] != 0U &&
            amount > script->send_amounts[call]) {
            amount = script->send_amounts[call];
        }
    } else if (script->send_limit != 0U &&
               amount > script->send_limit) {
        amount = script->send_limit;
    }
    if (amount > sizeof(script->sent) - script->sent_length) {
        return -1;
    }
    memcpy(script->sent + script->sent_length, bytes, amount);
    script->sent_length += amount;
    script->send_calls = call + 1U;
    return (ssize_t)amount;
}

static void apply_scripted_clock(scripted_socket_t *script)
{
    if (script->jump_pending &&
        script->response_position >= script->jump_at_position) {
        script->now_ms = script->jump_to_ms;
        script->jump_pending = false;
    }

    if (script->paced_body_length != 0U &&
        script->response_position >= script->paced_body_start &&
        script->response_position <
            script->paced_body_start + script->paced_body_length) {
        const size_t body_index =
            script->response_position - script->paced_body_start;
        if (body_index + 1U == script->paced_body_length) {
            script->now_ms = script->paced_final_ms;
        } else {
            script->now_ms = (int64_t)(body_index + 1U) * INT64_C(4999);
        }
    }
}

static ssize_t scripted_receive(
    void *context,
    int socket_handle,
    void *bytes,
    size_t capacity,
    uint32_t timeout_ms)
{
    (void)timeout_ms;
    scripted_socket_t *script = context;
    if (socket_handle != 17 || !bytes || capacity == 0U) {
        return -1;
    }
    script->receive_calls++;
    apply_scripted_clock(script);
    if (script->response_position == script->response_length) {
        return 0;
    }

    size_t amount = script->response_length - script->response_position;
    if (amount > capacity) {
        amount = capacity;
    }
    if (script->receive_limit != 0U && amount > script->receive_limit) {
        amount = script->receive_limit;
    }
    if (script->split_pending &&
        script->response_position < script->split_at &&
        amount > script->split_at - script->response_position) {
        amount = script->split_at - script->response_position;
    }
    if (script->split_pending &&
        script->response_position + amount == script->split_at) {
        script->split_pending = false;
    }
    memcpy(bytes, script->response + script->response_position, amount);
    script->response_position += amount;
    return (ssize_t)amount;
}

static void scripted_close(void *context, int socket_handle)
{
    scripted_socket_t *script = context;
    TEST_ASSERT_EQUAL_INT(17, socket_handle);
    script->close_calls++;
}

static int64_t scripted_now_ms(void *context)
{
    return ((scripted_socket_t *)context)->now_ms;
}

static void install_script(scripted_socket_t *script)
{
    script->resolve_ok = true;
    script->connect_ok = true;
    backend_http_io_t io = {
        .context = script,
        .resolve = scripted_resolve,
        .connect = scripted_connect,
        .send = scripted_send,
        .receive = scripted_receive,
        .close = scripted_close,
        .monotonic_ms = scripted_now_ms,
    };
    TEST_ASSERT_TRUE(backend_http_install_io(&io));
}

static scripted_socket_t response_script(const char *response)
{
    scripted_socket_t script;
    memset(&script, 0, sizeof(script));
    script.response = (const uint8_t *)response;
    script.response_length = strlen(response);
    script.split_at = SIZE_MAX;
    return script;
}

static scripted_socket_t response_script_bytes(
    const uint8_t *response, size_t response_length)
{
    scripted_socket_t script;
    memset(&script, 0, sizeof(script));
    script.response = response;
    script.response_length = response_length;
    script.split_at = SIZE_MAX;
    return script;
}

static bool collecting_sink(
    void *context, const uint8_t *bytes, size_t length)
{
    sink_fixture_t *sink = context;
    sink->calls++;
    if (sink->fail_on_call != 0U && sink->calls == sink->fail_on_call) {
        return false;
    }
    if (!bytes || length > sizeof(sink->bytes) - sink->length) {
        return false;
    }
    memcpy(sink->bytes + sink->length, bytes, length);
    sink->length += length;
    return true;
}

static void assert_json_success(
    scripted_socket_t *script,
    const char *base_url,
    const char *endpoint,
    const char *expected_body)
{
    char body[BACKEND_HTTP_MAX_JSON_BODY + 1U];
    memset(body, 0xA5, sizeof(body));
    install_script(script);
    backend_http_result_t result = backend_http_get_json(
        base_url, endpoint, body, sizeof(body));
    TEST_ASSERT_TRUE(result.transport_complete);
    TEST_ASSERT_EQUAL_INT(200, result.status_code);
    TEST_ASSERT_EQUAL(BACKEND_HTTP_ERROR_NONE, result.error);
    TEST_ASSERT_EQUAL_UINT(strlen(expected_body), result.body_length);
    TEST_ASSERT_EQUAL_STRING(expected_body, body);
    TEST_ASSERT_EQUAL_UINT(1U, script->close_calls);
}

static void assert_bodyless_json_success(
    const char *response, int expected_status)
{
    scripted_socket_t script = response_script(response);
    char body[1] = {'X'};
    install_script(&script);
    const backend_http_result_t result = backend_http_get_json(
        "http://host", "/health", body, sizeof(body));
    TEST_ASSERT_TRUE(result.transport_complete);
    TEST_ASSERT_EQUAL_INT(expected_status, result.status_code);
    TEST_ASSERT_EQUAL(BACKEND_HTTP_ERROR_NONE, result.error);
    TEST_ASSERT_EQUAL_UINT(0U, result.body_length);
    TEST_ASSERT_EQUAL_CHAR('\0', body[0]);
    TEST_ASSERT_EQUAL_UINT(1U, script.close_calls);
}

static void assert_json_error(
    const char *response,
    size_t response_length,
    size_t capacity,
    backend_http_error_t expected_error);

void test_configured_url_controls_dns_host_header_and_joined_path(void)
{
    static const char response[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}";
    scripted_socket_t script = response_script(response);
    script.send_limit = 3U;
    char body[16];
    install_script(&script);

    backend_http_result_t result = backend_http_post_json(
        "http://host:8080/base", "/detections/drones",
        "{}", 2U, body, sizeof(body));

    static const char expected_request[] =
        "POST /base/detections/drones HTTP/1.1\r\n"
        "Host: host:8080\r\n"
        "Accept: application/json\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n"
        "Connection: close\r\n\r\n{}";
    TEST_ASSERT_TRUE(result.transport_complete);
    TEST_ASSERT_EQUAL_STRING("host", script.resolved_host);
    TEST_ASSERT_EQUAL_UINT16(8080U, script.resolved_port);
    TEST_ASSERT_EQUAL_UINT(strlen(expected_request), script.sent_length);
    TEST_ASSERT_EQUAL_MEMORY(
        expected_request, script.sent, script.sent_length);
    TEST_ASSERT_GREATER_THAN_UINT(1U, script.send_calls);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(
        BACKEND_HTTP_JSON_TIMEOUT_MS, script.resolve_timeout_ms);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(
        BACKEND_HTTP_JSON_TIMEOUT_MS, script.connect_timeout_ms);
}

void test_default_port_and_empty_base_generate_exact_get_request(void)
{
    static const char response[] =
        "HTTP/1.1 204 No Content\r\n\r\n";
    scripted_socket_t script = response_script(response);
    char body[1];
    install_script(&script);

    backend_http_result_t result = backend_http_get_json(
        "http://backend.local", "/health", body, sizeof(body));

    static const char expected_request[] =
        "GET /health HTTP/1.1\r\n"
        "Host: backend.local\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n\r\n";
    TEST_ASSERT_TRUE(result.transport_complete);
    TEST_ASSERT_EQUAL_INT(204, result.status_code);
    TEST_ASSERT_EQUAL_UINT16(80U, script.resolved_port);
    TEST_ASSERT_EQUAL_MEMORY(
        expected_request, script.sent, strlen(expected_request));
}

void test_204_rejects_framing_headers_and_body_bytes(void)
{
    static const char content_length[] =
        "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n";
    static const char transfer_encoding[] =
        "HTTP/1.1 204 No Content\r\nTransfer-Encoding: chunked\r\n\r\n"
        "0\r\n\r\n";
    static const char body_bytes[] =
        "HTTP/1.1 204 No Content\r\n\r\nx";
    assert_json_error(
        content_length, sizeof(content_length) - 1U, 1U,
        BACKEND_HTTP_ERROR_FRAMING);
    assert_json_error(
        transfer_encoding, sizeof(transfer_encoding) - 1U, 1U,
        BACKEND_HTTP_ERROR_FRAMING);
    assert_json_error(
        body_bytes, sizeof(body_bytes) - 1U, 1U,
        BACKEND_HTTP_ERROR_FRAMING);
}

void test_304_accepts_optional_content_length_metadata_without_body(void)
{
    static const char no_metadata[] =
        "HTTP/1.1 304 Not Modified\r\nETag: \"abc\"\r\n\r\n";
    static const char length_metadata[] =
        "HTTP/1.1 304 Not Modified\r\nContent-Length: 12345\r\n\r\n";
    assert_bodyless_json_success(no_metadata, 304);
    assert_bodyless_json_success(length_metadata, 304);
}

void test_304_rejects_transfer_encoding_and_body_bytes(void)
{
    static const char transfer_encoding[] =
        "HTTP/1.1 304 Not Modified\r\nTransfer-Encoding: chunked\r\n\r\n"
        "0\r\n\r\n";
    static const char body_bytes[] =
        "HTTP/1.1 304 Not Modified\r\nContent-Length: 3\r\n\r\nabc";
    assert_json_error(
        transfer_encoding, sizeof(transfer_encoding) - 1U, 1U,
        BACKEND_HTTP_ERROR_FRAMING);
    assert_json_error(
        body_bytes, sizeof(body_bytes) - 1U, 1U,
        BACKEND_HTTP_ERROR_FRAMING);
}

void test_informational_responses_are_skipped_before_final_response(void)
{
    static const char response[] =
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 103 Early Hints\r\nLink: </firmware.bin>\r\n\r\n"
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}";
    for (size_t split = 1U; split < sizeof(response) - 1U; ++split) {
        scripted_socket_t script = response_script(response);
        script.split_at = split;
        script.split_pending = true;
        assert_json_success(&script, "http://host", "/health", "{}");
    }
}

void test_switching_protocols_is_not_skipped_as_an_interim_response(void)
{
    static const char response[] =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n"
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    assert_json_error(
        response, sizeof(response) - 1U, 1U,
        BACKEND_HTTP_ERROR_FRAMING);
}

void test_more_than_eight_informational_responses_are_rejected(void)
{
    static const char response[] =
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    assert_json_error(
        response, sizeof(response) - 1U, 1U,
        BACKEND_HTTP_ERROR_FRAMING);
}

void test_interim_and_final_headers_share_the_2048_byte_budget(void)
{
    char response[2200];
    size_t position = 0U;
    static const char interim_prefix[] =
        "HTTP/1.1 103 Early Hints\r\nX-Fill: ";
    memcpy(response + position, interim_prefix, sizeof(interim_prefix) - 1U);
    position += sizeof(interim_prefix) - 1U;
    memset(response + position, 'a', 1000U);
    position += 1000U;
    memcpy(response + position, "\r\n\r\n", 4U);
    position += 4U;

    static const char final_prefix[] =
        "HTTP/1.1 200 OK\r\nX-Fill: ";
    memcpy(response + position, final_prefix, sizeof(final_prefix) - 1U);
    position += sizeof(final_prefix) - 1U;
    memset(response + position, 'b', 1000U);
    position += 1000U;
    static const char final_suffix[] =
        "\r\nContent-Length: 0\r\n\r\n";
    memcpy(response + position, final_suffix, sizeof(final_suffix) - 1U);
    position += sizeof(final_suffix) - 1U;

    TEST_ASSERT_LESS_THAN_UINT(
        BACKEND_HTTP_MAX_RESPONSE_HEADERS,
        (sizeof(interim_prefix) - 1U) + 1000U + 4U);
    TEST_ASSERT_LESS_THAN_UINT(
        BACKEND_HTTP_MAX_RESPONSE_HEADERS,
        (sizeof(final_prefix) - 1U) + 1000U +
            (sizeof(final_suffix) - 1U));
    TEST_ASSERT_GREATER_THAN_UINT(
        BACKEND_HTTP_MAX_RESPONSE_HEADERS, position);
    assert_json_error(
        response, position, 1U,
        BACKEND_HTTP_ERROR_HEADERS_TOO_LARGE);
}

void test_status_headers_and_content_length_survive_every_split_boundary(void)
{
    static const char response[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 11\r\n\r\n"
        "{\"ok\":true}";
    for (size_t split = 1U; split < sizeof(response) - 1U; ++split) {
        scripted_socket_t script = response_script(response);
        script.split_at = split;
        script.split_pending = true;
        assert_json_success(
            &script, "http://host:8080/base", "/health", "{\"ok\":true}");
    }
}

void test_chunk_size_body_and_terminator_survive_every_split_boundary(void)
{
    static const char response[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
    for (size_t split = 1U; split < sizeof(response) - 1U; ++split) {
        scripted_socket_t script = response_script(response);
        script.split_at = split;
        script.split_pending = true;
        assert_json_success(
            &script, "http://host", "/health", "Wikipedia");
    }
}

void test_valid_chunk_extensions_and_trailers_survive_every_json_split_boundary(void)
{
    static const char response[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "4;foo=bar;quoted=\"a\\\"b\";flag\r\nWiki\r\n"
        "5;second=token\r\npedia\r\n"
        "0;done=yes\r\n"
        "X-Checksum: abc123\r\n"
        "X-Note:\tvalidated value\r\n\r\n";
    for (size_t split = 1U; split < sizeof(response) - 1U; ++split) {
        scripted_socket_t script = response_script(response);
        script.split_at = split;
        script.split_pending = true;
        assert_json_success(
            &script, "http://host", "/health", "Wikipedia");
    }
}

void test_valid_chunk_extensions_and_trailers_survive_every_binary_split_boundary(void)
{
    static const char response[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "6;kind=firmware;verified=\"yes\"\r\nabcdef\r\n"
        "0;complete\r\nDigest: sha-256=abc123\r\n\r\n";
    for (size_t split = 1U; split < sizeof(response) - 1U; ++split) {
        scripted_socket_t script = response_script(response);
        script.split_at = split;
        script.split_pending = true;
        sink_fixture_t sink = {0};
        install_script(&script);
        backend_http_result_t result = backend_http_get_binary(
            "http://host", "/firmware.bin", 6U,
            collecting_sink, &sink);
        TEST_ASSERT_TRUE(result.transport_complete);
        TEST_ASSERT_EQUAL(BACKEND_HTTP_ERROR_NONE, result.error);
        TEST_ASSERT_EQUAL_UINT(6U, result.body_length);
        TEST_ASSERT_EQUAL_UINT8_ARRAY("abcdef", sink.bytes, 6U);
    }
}

void test_chunk_extension_and_combined_trailer_header_limits_accept_exact_boundaries(void)
{
    static const char initial_headers[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    char extension_response[
        sizeof(initial_headers) + BACKEND_HTTP_MAX_CHUNK_LINE + 16U];
    size_t position = sizeof(initial_headers) - 1U;
    memcpy(extension_response, initial_headers, position);
    static const char extension_prefix[] = "1;x=";
    memcpy(
        extension_response + position,
        extension_prefix,
        sizeof(extension_prefix) - 1U);
    position += sizeof(extension_prefix) - 1U;
    const size_t extension_fill =
        BACKEND_HTTP_MAX_CHUNK_LINE - (sizeof(extension_prefix) - 1U);
    memset(extension_response + position, 'a', extension_fill);
    position += extension_fill;
    static const char extension_suffix[] = "\r\nx\r\n0\r\n\r\n";
    memcpy(
        extension_response + position,
        extension_suffix,
        sizeof(extension_suffix) - 1U);
    position += sizeof(extension_suffix) - 1U;
    extension_response[position] = '\0';
    scripted_socket_t extension_script =
        response_script(extension_response);
    assert_json_success(
        &extension_script, "http://host", "/health", "x");

    char trailer_response[BACKEND_HTTP_MAX_RESPONSE_HEADERS + 32U];
    position = sizeof(initial_headers) - 1U;
    memcpy(trailer_response, initial_headers, position);
    memcpy(trailer_response + position, "0\r\nX-Fill: ", 11U);
    position += 11U;
    const size_t trailer_budget =
        BACKEND_HTTP_MAX_RESPONSE_HEADERS -
        (sizeof(initial_headers) - 1U);
    const size_t trailer_fill = trailer_budget - 8U - 4U;
    memset(trailer_response + position, 'b', trailer_fill);
    position += trailer_fill;
    memcpy(trailer_response + position, "\r\n\r\n", 4U);
    position += 4U;
    trailer_response[position] = '\0';
    scripted_socket_t trailer_script = response_script(trailer_response);
    assert_json_success(
        &trailer_script, "http://host", "/health", "");
}

void test_chunk_quoted_extensions_accept_tab_obs_text_and_quoted_pairs(void)
{
    static const char response[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "1;quoted=\"tab\tobs\x80\\\"slash\\\\\"\r\nx\r\n0\r\n\r\n";
    scripted_socket_t script = response_script(response);
    assert_json_success(&script, "http://host", "/health", "x");
}

void test_chunk_extensions_accept_rfc_bws_around_semicolon_and_equals(void)
{
    static const char response[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "3 \t; \tname \t= \tvalue \t;\tquoted\t=\t\"yes\"; flag\r\n"
        "abc\r\n0 \t; done \t= token\r\n\r\n";
    for (size_t split = 1U; split < sizeof(response) - 1U; ++split) {
        scripted_socket_t script = response_script(response);
        script.split_at = split;
        script.split_pending = true;
        assert_json_success(
            &script, "http://host", "/health", "abc");
    }
}

static void assert_json_error(
    const char *response,
    size_t response_length,
    size_t capacity,
    backend_http_error_t expected_error)
{
    scripted_socket_t script = response_script("");
    script.response = (const uint8_t *)response;
    script.response_length = response_length;
    char body[BACKEND_HTTP_MAX_JSON_BODY + 1U];
    memset(body, 'X', sizeof(body));
    install_script(&script);
    backend_http_result_t result = backend_http_get_json(
        "http://host", "/health", body, capacity);
    TEST_ASSERT_FALSE(result.transport_complete);
    TEST_ASSERT_EQUAL(expected_error, result.error);
    TEST_ASSERT_EQUAL_UINT(0U, result.body_length);
    TEST_ASSERT_EQUAL_CHAR('\0', body[0]);
    TEST_ASSERT_EQUAL_UINT(1U, script.close_calls);
}

void test_conflicting_and_duplicate_framing_headers_are_rejected(void)
{
    static const char conflict[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n"
        "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n";
    static const char duplicate_length[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n"
        "Content-Length: 0\r\n\r\n";
    static const char duplicate_transfer[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
        "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n";
    assert_json_error(
        conflict, sizeof(conflict) - 1U, 32U,
        BACKEND_HTTP_ERROR_FRAMING);
    assert_json_error(
        duplicate_length, sizeof(duplicate_length) - 1U, 32U,
        BACKEND_HTTP_ERROR_FRAMING);
    assert_json_error(
        duplicate_transfer, sizeof(duplicate_transfer) - 1U, 32U,
        BACKEND_HTTP_ERROR_FRAMING);
}

void test_unsupported_or_missing_body_framing_is_rejected(void)
{
    static const char unsupported[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n";
    static const char missing[] =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n";
    assert_json_error(
        unsupported, sizeof(unsupported) - 1U, 32U,
        BACKEND_HTTP_ERROR_FRAMING);
    assert_json_error(
        missing, sizeof(missing) - 1U, 32U,
        BACKEND_HTTP_ERROR_FRAMING);
}

void test_header_limit_rejects_only_responses_over_2048_bytes(void)
{
    char exact[BACKEND_HTTP_MAX_RESPONSE_HEADERS + 1U];
    const char exact_prefix[] = "HTTP/1.1 200 OK\r\nX-Fill: ";
    const char exact_suffix[] = "\r\nContent-Length: 0\r\n\r\n";
    size_t exact_position = sizeof(exact_prefix) - 1U;
    memcpy(exact, exact_prefix, exact_position);
    const size_t fill_length =
        BACKEND_HTTP_MAX_RESPONSE_HEADERS - exact_position -
        (sizeof(exact_suffix) - 1U);
    memset(exact + exact_position, 'a', fill_length);
    exact_position += fill_length;
    memcpy(exact + exact_position, exact_suffix, sizeof(exact_suffix) - 1U);
    exact_position += sizeof(exact_suffix) - 1U;
    exact[exact_position] = '\0';
    TEST_ASSERT_EQUAL_UINT(
        BACKEND_HTTP_MAX_RESPONSE_HEADERS, exact_position);
    scripted_socket_t exact_script = response_script(exact);
    assert_json_success(
        &exact_script, "http://host", "/health", "");

    char response[2200];
    const char prefix[] = "HTTP/1.1 200 OK\r\nX-Fill: ";
    memcpy(response, prefix, sizeof(prefix) - 1U);
    size_t position = sizeof(prefix) - 1U;
    while (position < 2049U) {
        response[position++] = 'a';
    }
    const char suffix[] = "\r\nContent-Length: 0\r\n\r\n";
    memcpy(response + position, suffix, sizeof(suffix) - 1U);
    position += sizeof(suffix) - 1U;
    assert_json_error(
        response, position, 1U, BACKEND_HTTP_ERROR_HEADERS_TOO_LARGE);
}

void test_json_body_limit_capacity_truncation_and_trailing_bytes_are_rejected(void)
{
    char oversized[128];
    const int oversized_length = snprintf(
        oversized, sizeof(oversized),
        "HTTP/1.1 200 OK\r\nContent-Length: %u\r\n\r\n",
        (unsigned)(BACKEND_HTTP_MAX_JSON_BODY + 1U));
    TEST_ASSERT_GREATER_THAN_INT(0, oversized_length);
    TEST_ASSERT_LESS_THAN_UINT(sizeof(oversized), (size_t)oversized_length);
    static const char too_small[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc";
    static const char truncated[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nabc";
    static const char trailing[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabcd";
    assert_json_error(
        oversized, (size_t)oversized_length,
        BACKEND_HTTP_MAX_JSON_BODY + 1U,
        BACKEND_HTTP_ERROR_BODY_TOO_LARGE);
    assert_json_error(
        too_small, sizeof(too_small) - 1U, 3U,
        BACKEND_HTTP_ERROR_BODY_TOO_LARGE);
    assert_json_error(
        truncated, sizeof(truncated) - 1U, 32U,
        BACKEND_HTTP_ERROR_FRAMING);
    assert_json_error(
        trailing, sizeof(trailing) - 1U, 32U,
        BACKEND_HTTP_ERROR_FRAMING);
}

void test_invalid_chunk_syntax_truncation_and_trailing_bytes_are_rejected(void)
{
    static const char invalid_hex[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "z\r\nabc\r\n0\r\n\r\n";
    static const char missing_chunk_crlf[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "3\r\nabc0\r\n\r\n";
    static const char truncated[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "3\r\nab";
    static const char trailing[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "3\r\nabc\r\n0\r\n\r\nx";
    assert_json_error(
        invalid_hex, sizeof(invalid_hex) - 1U, 32U,
        BACKEND_HTTP_ERROR_FRAMING);
    assert_json_error(
        missing_chunk_crlf, sizeof(missing_chunk_crlf) - 1U, 32U,
        BACKEND_HTTP_ERROR_FRAMING);
    assert_json_error(
        truncated, sizeof(truncated) - 1U, 32U,
        BACKEND_HTTP_ERROR_FRAMING);
    assert_json_error(
        trailing, sizeof(trailing) - 1U, 32U,
        BACKEND_HTTP_ERROR_FRAMING);
}

void test_malformed_or_oversized_chunk_extensions_are_rejected(void)
{
    static const char missing_name[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "3;=value\r\nabc\r\n0\r\n\r\n";
    static const char missing_value[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "3;name=\r\nabc\r\n0\r\n\r\n";
    static const char unterminated_quote[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "3;name=\"value\r\nabc\r\n0\r\n\r\n";
    assert_json_error(
        missing_name, sizeof(missing_name) - 1U, 32U,
        BACKEND_HTTP_ERROR_FRAMING);
    assert_json_error(
        missing_value, sizeof(missing_value) - 1U, 32U,
        BACKEND_HTTP_ERROR_FRAMING);
    assert_json_error(
        unterminated_quote, sizeof(unterminated_quote) - 1U, 32U,
        BACKEND_HTTP_ERROR_FRAMING);
    char oversized[BACKEND_HTTP_MAX_CHUNK_LINE + 128U];
    static const char prefix[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1;x=";
    size_t position = sizeof(prefix) - 1U;
    memcpy(oversized, prefix, position);
    memset(
        oversized + position,
        'a',
        BACKEND_HTTP_MAX_CHUNK_LINE + 1U);
    position += BACKEND_HTTP_MAX_CHUNK_LINE + 1U;
    static const char suffix[] = "\r\nx\r\n0\r\n\r\n";
    memcpy(oversized + position, suffix, sizeof(suffix) - 1U);
    position += sizeof(suffix) - 1U;
    assert_json_error(
        oversized, position, 32U, BACKEND_HTTP_ERROR_FRAMING);
}

void test_malformed_forbidden_or_oversized_trailers_are_rejected(void)
{
    static const char malformed[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "0\r\nNot-A-Field\r\n\r\n";
    static const char folded[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "0\r\n X-Folded: no\r\n\r\n";
    static const char content_length[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "0\r\nContent-Length: 0\r\n\r\n";
    static const char transfer_encoding[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "0\r\nTransfer-Encoding: chunked\r\n\r\n";
    assert_json_error(
        malformed, sizeof(malformed) - 1U, 32U,
        BACKEND_HTTP_ERROR_FRAMING);
    assert_json_error(
        folded, sizeof(folded) - 1U, 32U,
        BACKEND_HTTP_ERROR_FRAMING);
    assert_json_error(
        content_length, sizeof(content_length) - 1U, 32U,
        BACKEND_HTTP_ERROR_FRAMING);
    assert_json_error(
        transfer_encoding, sizeof(transfer_encoding) - 1U, 32U,
        BACKEND_HTTP_ERROR_FRAMING);

    char oversized[BACKEND_HTTP_MAX_RESPONSE_HEADERS + 128U];
    static const char prefix[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "0\r\nX-Fill: ";
    size_t position = sizeof(prefix) - 1U;
    memcpy(oversized, prefix, position);
    while (position < BACKEND_HTTP_MAX_RESPONSE_HEADERS + 1U) {
        oversized[position++] = 'a';
    }
    static const char suffix[] = "\r\n\r\n";
    memcpy(oversized + position, suffix, sizeof(suffix) - 1U);
    position += sizeof(suffix) - 1U;
    assert_json_error(
        oversized, position, 32U,
        BACKEND_HTTP_ERROR_HEADERS_TOO_LARGE);
}

void test_decoded_chunked_json_over_4096_is_rejected_before_copy(void)
{
    char response[128];
    const int response_length = snprintf(
        response, sizeof(response),
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "%zx\r\n",
        (size_t)BACKEND_HTTP_MAX_JSON_BODY + 1U);
    TEST_ASSERT_GREATER_THAN_INT(0, response_length);
    TEST_ASSERT_LESS_THAN_UINT(sizeof(response), (size_t)response_length);
    assert_json_error(
        response, (size_t)response_length,
        BACKEND_HTTP_MAX_JSON_BODY + 1U,
        BACKEND_HTTP_ERROR_BODY_TOO_LARGE);
}

void test_json_total_deadline_accepts_4999_and_rejects_5000_ms(void)
{
    static const char response[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nx";
    const size_t body_start = sizeof(response) - 2U;

    scripted_socket_t before = response_script(response);
    before.split_at = body_start;
    before.split_pending = true;
    before.jump_at_position = body_start;
    before.jump_to_ms = INT64_C(4999);
    before.jump_pending = true;
    assert_json_success(&before, "http://host", "/health", "x");

    scripted_socket_t at = response_script(response);
    at.split_at = body_start;
    at.split_pending = true;
    at.jump_at_position = body_start;
    at.jump_to_ms = INT64_C(5000);
    at.jump_pending = true;
    char body[8];
    install_script(&at);
    backend_http_result_t result = backend_http_get_json(
        "http://host", "/health", body, sizeof(body));
    TEST_ASSERT_FALSE(result.transport_complete);
    TEST_ASSERT_EQUAL(BACKEND_HTTP_ERROR_TIMEOUT, result.error);
    TEST_ASSERT_EQUAL_CHAR('\0', body[0]);
}

void test_binary_request_and_framing_preserve_arbitrary_bytes_exactly(void)
{
    static const uint8_t expected[] = {
        UINT8_C(0x00), UINT8_C(0x80), UINT8_C(0xFF),
    };
    static const uint8_t fixed[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\n"
        "\x00\x80\xff";
    scripted_socket_t fixed_script = response_script_bytes(
        fixed, sizeof(fixed) - 1U);
    fixed_script.receive_limit = 1U;
    sink_fixture_t fixed_sink = {0};
    install_script(&fixed_script);
    backend_http_result_t result = backend_http_get_binary(
        "http://host/base", "/firmware.bin", sizeof(expected),
        collecting_sink, &fixed_sink);
    TEST_ASSERT_TRUE(result.transport_complete);
    TEST_ASSERT_EQUAL_UINT(sizeof(expected), result.body_length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        expected, fixed_sink.bytes, sizeof(expected));

    static const char expected_request[] =
        "GET /base/firmware.bin HTTP/1.1\r\n"
        "Host: host\r\n"
        "Accept: application/octet-stream\r\n"
        "Connection: close\r\n\r\n";
    TEST_ASSERT_EQUAL_UINT(
        sizeof(expected_request) - 1U, fixed_script.sent_length);
    TEST_ASSERT_EQUAL_MEMORY(
        expected_request,
        fixed_script.sent,
        sizeof(expected_request) - 1U);

    static const uint8_t chunked[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "1\r\n\x00\r\n2\r\n\x80\xff\r\n0\r\n\r\n";
    scripted_socket_t chunked_script = response_script_bytes(
        chunked, sizeof(chunked) - 1U);
    chunked_script.receive_limit = 1U;
    sink_fixture_t chunked_sink = {0};
    install_script(&chunked_script);
    result = backend_http_get_binary(
        "http://host/base", "/firmware.bin", sizeof(expected),
        collecting_sink, &chunked_sink);
    TEST_ASSERT_TRUE(result.transport_complete);
    TEST_ASSERT_EQUAL_UINT(sizeof(expected), result.body_length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        expected, chunked_sink.bytes, sizeof(expected));
}

void test_binary_fixed_404_discards_body_without_firmware_sink(void)
{
    static const char response[] =
        "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nnot found";
    scripted_socket_t script = response_script(response);
    sink_fixture_t sink = {0};
    install_script(&script);

    const backend_http_result_t result = backend_http_get_binary(
        "http://host", "/firmware.bin", 4096U,
        collecting_sink, &sink);

    TEST_ASSERT_TRUE(result.transport_complete);
    TEST_ASSERT_EQUAL_INT(404, result.status_code);
    TEST_ASSERT_EQUAL(BACKEND_HTTP_ERROR_NONE, result.error);
    TEST_ASSERT_EQUAL_UINT(9U, result.body_length);
    TEST_ASSERT_EQUAL_UINT(0U, sink.calls);
    TEST_ASSERT_EQUAL_UINT(0U, sink.length);
}

void test_binary_chunked_404_discards_body_without_firmware_sink(void)
{
    static const char response[] =
        "HTTP/1.1 404 Not Found\r\nTransfer-Encoding: chunked\r\n\r\n"
        "4\r\nnot \r\n5\r\nfound\r\n0\r\nX-Error: missing\r\n\r\n";
    scripted_socket_t script = response_script(response);
    script.receive_limit = 1U;
    sink_fixture_t sink = {0};
    install_script(&script);

    const backend_http_result_t result = backend_http_get_binary(
        "http://host", "/firmware.bin", 4096U,
        collecting_sink, &sink);

    TEST_ASSERT_TRUE(result.transport_complete);
    TEST_ASSERT_EQUAL_INT(404, result.status_code);
    TEST_ASSERT_EQUAL(BACKEND_HTTP_ERROR_NONE, result.error);
    TEST_ASSERT_EQUAL_UINT(9U, result.body_length);
    TEST_ASSERT_EQUAL_UINT(0U, sink.calls);
    TEST_ASSERT_EQUAL_UINT(0U, sink.length);
}

void test_binary_error_discard_rejects_body_over_4096_without_sink(void)
{
    char response[BACKEND_HTTP_MAX_JSON_BODY + 128U];
    const int header_length = snprintf(
        response, sizeof(response),
        "HTTP/1.1 404 Not Found\r\nContent-Length: %u\r\n\r\n",
        (unsigned)(BACKEND_HTTP_MAX_JSON_BODY + 1U));
    TEST_ASSERT_GREATER_THAN_INT(0, header_length);
    TEST_ASSERT_LESS_THAN_UINT(sizeof(response), (size_t)header_length);
    size_t response_length = (size_t)header_length;
    memset(response + response_length, 'x', BACKEND_HTTP_MAX_JSON_BODY + 1U);
    response_length += BACKEND_HTTP_MAX_JSON_BODY + 1U;

    scripted_socket_t script = response_script("");
    script.response = (const uint8_t *)response;
    script.response_length = response_length;
    sink_fixture_t sink = {0};
    install_script(&script);
    const backend_http_result_t result = backend_http_get_binary(
        "http://host", "/firmware.bin", 8192U,
        collecting_sink, &sink);

    TEST_ASSERT_FALSE(result.transport_complete);
    TEST_ASSERT_EQUAL_INT(404, result.status_code);
    TEST_ASSERT_EQUAL(BACKEND_HTTP_ERROR_BODY_TOO_LARGE, result.error);
    TEST_ASSERT_EQUAL_UINT(0U, result.body_length);
    TEST_ASSERT_EQUAL_UINT(0U, sink.calls);
    TEST_ASSERT_EQUAL_UINT(0U, sink.length);
}

void test_binary_length_mismatch_and_sink_failure_are_rejected(void)
{
    static const char short_fixed[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nabcde";
    scripted_socket_t short_script = response_script(short_fixed);
    sink_fixture_t sink = {0};
    install_script(&short_script);
    backend_http_result_t result = backend_http_get_binary(
        "http://host", "/firmware.bin", 6U, collecting_sink, &sink);
    TEST_ASSERT_FALSE(result.transport_complete);
    TEST_ASSERT_EQUAL(BACKEND_HTTP_ERROR_FRAMING, result.error);
    TEST_ASSERT_EQUAL_UINT(0U, sink.length);

    static const char long_chunk[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "7\r\nabcdefg\r\n0\r\n\r\n";
    scripted_socket_t long_script = response_script(long_chunk);
    memset(&sink, 0, sizeof(sink));
    install_script(&long_script);
    result = backend_http_get_binary(
        "http://host", "/firmware.bin", 6U, collecting_sink, &sink);
    TEST_ASSERT_FALSE(result.transport_complete);
    TEST_ASSERT_EQUAL(BACKEND_HTTP_ERROR_BODY_TOO_LARGE, result.error);
    TEST_ASSERT_EQUAL_UINT(0U, sink.length);

    static const char sink_response[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc";
    scripted_socket_t sink_script = response_script(sink_response);
    memset(&sink, 0, sizeof(sink));
    sink.fail_on_call = 1U;
    install_script(&sink_script);
    result = backend_http_get_binary(
        "http://host", "/firmware.bin", 3U, collecting_sink, &sink);
    TEST_ASSERT_FALSE(result.transport_complete);
    TEST_ASSERT_EQUAL(BACKEND_HTTP_ERROR_SINK, result.error);
}

static backend_http_result_t run_paced_binary(int64_t final_ms)
{
    static const char response[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\n"
        "abcdefghijklm";
    const size_t body_start = sizeof(response) - 14U;
    scripted_socket_t script = response_script(response);
    script.receive_limit = 1U;
    script.paced_body_start = body_start;
    script.paced_body_length = 13U;
    script.paced_final_ms = final_ms;
    sink_fixture_t sink = {0};
    install_script(&script);
    return backend_http_get_binary(
        "http://host", "/firmware.bin", 13U,
        collecting_sink, &sink);
}

void test_binary_total_deadline_accepts_59999_and_rejects_60000_ms(void)
{
    backend_http_result_t result = run_paced_binary(INT64_C(59999));
    TEST_ASSERT_TRUE(result.transport_complete);
    TEST_ASSERT_EQUAL(BACKEND_HTTP_ERROR_NONE, result.error);

    result = run_paced_binary(INT64_C(60000));
    TEST_ASSERT_FALSE(result.transport_complete);
    TEST_ASSERT_EQUAL(BACKEND_HTTP_ERROR_TIMEOUT, result.error);
}

static backend_http_result_t run_stalled_binary(int64_t body_arrival_ms)
{
    static const char response[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nx";
    const size_t body_start = sizeof(response) - 2U;
    scripted_socket_t script = response_script(response);
    script.split_at = body_start;
    script.split_pending = true;
    script.jump_at_position = body_start;
    script.jump_to_ms = body_arrival_ms;
    script.jump_pending = true;
    sink_fixture_t sink = {0};
    install_script(&script);
    return backend_http_get_binary(
        "http://host", "/firmware.bin", 1U,
        collecting_sink, &sink);
}

void test_binary_no_progress_accepts_4999_and_rejects_5000_ms(void)
{
    backend_http_result_t result = run_stalled_binary(INT64_C(4999));
    TEST_ASSERT_TRUE(result.transport_complete);
    TEST_ASSERT_EQUAL(BACKEND_HTTP_ERROR_NONE, result.error);

    result = run_stalled_binary(INT64_C(5000));
    TEST_ASSERT_FALSE(result.transport_complete);
    TEST_ASSERT_EQUAL(BACKEND_HTTP_ERROR_TIMEOUT, result.error);
}

static backend_http_result_t run_empty_binary(scripted_socket_t *script)
{
    static const char response[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    script->response = (const uint8_t *)response;
    script->response_length = sizeof(response) - 1U;
    script->split_at = SIZE_MAX;
    sink_fixture_t sink = {0};
    install_script(script);
    return backend_http_get_binary(
        "http://host", "/firmware.bin", 0U,
        collecting_sink, &sink);
}

void test_binary_no_progress_resets_after_resolve_connect_and_each_partial_send(void)
{
    scripted_socket_t script;
    memset(&script, 0, sizeof(script));
    script.resolve_clock_enabled = true;
    script.resolve_complete_ms = INT64_C(4999);
    script.connect_clock_enabled = true;
    script.connect_complete_ms = INT64_C(9998);
    script.send_schedule_count = 2U;
    script.send_amounts[0] = 1U;
    script.send_times_ms[0] = INT64_C(14997);
    script.send_times_ms[1] = INT64_C(19996);

    backend_http_result_t result = run_empty_binary(&script);

    TEST_ASSERT_TRUE(result.transport_complete);
    TEST_ASSERT_EQUAL(BACKEND_HTTP_ERROR_NONE, result.error);
    TEST_ASSERT_EQUAL_UINT32(
        BACKEND_HTTP_BINARY_NO_PROGRESS_MS, script.resolve_timeout_ms);
    TEST_ASSERT_EQUAL_UINT32(
        BACKEND_HTTP_BINARY_NO_PROGRESS_MS, script.connect_timeout_ms);
    TEST_ASSERT_EQUAL_UINT(2U, script.send_calls);
    TEST_ASSERT_EQUAL_UINT32(
        BACKEND_HTTP_BINARY_NO_PROGRESS_MS, script.send_timeouts_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(
        BACKEND_HTTP_BINARY_NO_PROGRESS_MS, script.send_timeouts_ms[1]);
}

void test_binary_pre_response_no_progress_rejects_each_exact_5000_ms_boundary(void)
{
    scripted_socket_t resolve_at;
    memset(&resolve_at, 0, sizeof(resolve_at));
    resolve_at.resolve_clock_enabled = true;
    resolve_at.resolve_complete_ms = INT64_C(5000);
    backend_http_result_t result = run_empty_binary(&resolve_at);
    TEST_ASSERT_FALSE(result.transport_complete);
    TEST_ASSERT_EQUAL(BACKEND_HTTP_ERROR_TIMEOUT, result.error);
    TEST_ASSERT_EQUAL_UINT(0U, resolve_at.connect_calls);

    scripted_socket_t connect_at;
    memset(&connect_at, 0, sizeof(connect_at));
    connect_at.resolve_clock_enabled = true;
    connect_at.resolve_complete_ms = INT64_C(4999);
    connect_at.connect_clock_enabled = true;
    connect_at.connect_complete_ms = INT64_C(9999);
    result = run_empty_binary(&connect_at);
    TEST_ASSERT_FALSE(result.transport_complete);
    TEST_ASSERT_EQUAL(BACKEND_HTTP_ERROR_TIMEOUT, result.error);
    TEST_ASSERT_EQUAL_UINT(0U, connect_at.send_calls);

    scripted_socket_t send_at;
    memset(&send_at, 0, sizeof(send_at));
    send_at.send_schedule_count = 2U;
    send_at.send_amounts[0] = 1U;
    send_at.send_times_ms[0] = INT64_C(4999);
    send_at.send_times_ms[1] = INT64_C(9999);
    result = run_empty_binary(&send_at);
    TEST_ASSERT_FALSE(result.transport_complete);
    TEST_ASSERT_EQUAL(BACKEND_HTTP_ERROR_TIMEOUT, result.error);
    TEST_ASSERT_EQUAL_UINT(1U, send_at.close_calls);
}

static backend_http_result_t run_paced_binary_send(int64_t final_ms)
{
    scripted_socket_t script;
    memset(&script, 0, sizeof(script));
    script.send_schedule_count = 13U;
    for (size_t index = 0U; index < 12U; ++index) {
        script.send_amounts[index] = 1U;
        script.send_times_ms[index] =
            (int64_t)(index + 1U) * INT64_C(4999);
    }
    script.send_times_ms[12] = final_ms;
    return run_empty_binary(&script);
}

void test_binary_partial_send_total_deadline_accepts_59999_and_rejects_60000_ms(void)
{
    backend_http_result_t result =
        run_paced_binary_send(INT64_C(59999));
    TEST_ASSERT_TRUE(result.transport_complete);
    TEST_ASSERT_EQUAL(BACKEND_HTTP_ERROR_NONE, result.error);

    result = run_paced_binary_send(INT64_C(60000));
    TEST_ASSERT_FALSE(result.transport_complete);
    TEST_ASSERT_EQUAL(BACKEND_HTTP_ERROR_TIMEOUT, result.error);
}

void test_invalid_url_dns_connect_and_send_failures_are_distinct(void)
{
    char body[8];
    backend_http_result_t result = backend_http_get_json(
        "https://host", "/health", body, sizeof(body));
    TEST_ASSERT_FALSE(result.transport_complete);
    TEST_ASSERT_EQUAL(BACKEND_HTTP_ERROR_INVALID_URL, result.error);

    static const char response[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    scripted_socket_t dns = response_script(response);
    install_script(&dns);
    dns.resolve_ok = false;
    result = backend_http_get_json(
        "http://host", "/health", body, sizeof(body));
    TEST_ASSERT_EQUAL(BACKEND_HTTP_ERROR_DNS, result.error);
    TEST_ASSERT_EQUAL_UINT(0U, dns.connect_calls);

    scripted_socket_t connect = response_script(response);
    install_script(&connect);
    connect.connect_ok = false;
    result = backend_http_get_json(
        "http://host", "/health", body, sizeof(body));
    TEST_ASSERT_EQUAL(BACKEND_HTTP_ERROR_CONNECT, result.error);
    TEST_ASSERT_EQUAL_UINT(0U, connect.close_calls);

    scripted_socket_t send = response_script(response);
    install_script(&send);
    send.send_limit = SIZE_MAX;
    send.sent_length = sizeof(send.sent);
    result = backend_http_get_json(
        "http://host", "/health", body, sizeof(body));
    TEST_ASSERT_EQUAL(BACKEND_HTTP_ERROR_SEND, result.error);
    TEST_ASSERT_EQUAL_UINT(1U, send.close_calls);
}

void test_dns_guard_pool_exhausts_then_reuses_without_late_callback_corruption(void)
{
    backend_http_dns_guard_pool_t pool;
    backend_http_dns_guard_pool_init(&pool);
    int slots[BACKEND_HTTP_DNS_GUARD_SLOTS];
    uint32_t generations[BACKEND_HTTP_DNS_GUARD_SLOTS];

    for (size_t index = 0U;
         index < BACKEND_HTTP_DNS_GUARD_SLOTS;
         ++index) {
        slots[index] = backend_http_dns_guard_reserve(
            &pool, &generations[index]);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(0, slots[index]);
        TEST_ASSERT_TRUE(backend_http_dns_guard_mark_pending(
            &pool, (size_t)slots[index], generations[index]));
    }
    uint32_t unavailable_generation = 0U;
    TEST_ASSERT_EQUAL_INT(-1, backend_http_dns_guard_reserve(
        &pool, &unavailable_generation));

    TEST_ASSERT_TRUE(backend_http_dns_guard_begin_completion(
        &pool, (size_t)slots[0], generations[0]));
    TEST_ASSERT_EQUAL_INT(-1, backend_http_dns_guard_reserve(
        &pool, &unavailable_generation));
    TEST_ASSERT_TRUE(backend_http_dns_guard_publish_completion(
        &pool, (size_t)slots[0], generations[0]));

    uint32_t replacement_generation = 0U;
    const int replacement_slot = backend_http_dns_guard_reserve(
        &pool, &replacement_generation);
    TEST_ASSERT_EQUAL_INT(slots[0], replacement_slot);
    TEST_ASSERT_NOT_EQUAL(generations[0], replacement_generation);
    TEST_ASSERT_TRUE(backend_http_dns_guard_mark_pending(
        &pool, (size_t)replacement_slot, replacement_generation));
    TEST_ASSERT_FALSE(backend_http_dns_guard_begin_completion(
        &pool, (size_t)replacement_slot, generations[0]));
    TEST_ASSERT_FALSE(backend_http_dns_guard_publish_completion(
        &pool, (size_t)replacement_slot, generations[0]));

    TEST_ASSERT_TRUE(backend_http_dns_guard_release_unscheduled(
        &pool, (size_t)replacement_slot, replacement_generation));
    for (size_t index = 1U;
         index < BACKEND_HTTP_DNS_GUARD_SLOTS;
         ++index) {
        TEST_ASSERT_TRUE(backend_http_dns_guard_begin_completion(
            &pool, (size_t)slots[index], generations[index]));
        TEST_ASSERT_TRUE(backend_http_dns_guard_publish_completion(
            &pool, (size_t)slots[index], generations[index]));
    }
    for (size_t index = 0U;
         index < BACKEND_HTTP_DNS_GUARD_SLOTS;
         ++index) {
        TEST_ASSERT_GREATER_OR_EQUAL_INT(
            0, backend_http_dns_guard_reserve(
                &pool, &replacement_generation));
    }
}

void test_platform_request_guard_requires_initialized_non_lwip_task(void)
{
    TEST_ASSERT_FALSE(backend_http_platform_caller_allowed(false, false));
    TEST_ASSERT_FALSE(backend_http_platform_caller_allowed(false, true));
    TEST_ASSERT_FALSE(backend_http_platform_caller_allowed(true, true));
    TEST_ASSERT_TRUE(backend_http_platform_caller_allowed(true, false));
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_configured_url_controls_dns_host_header_and_joined_path);
    BACKEND_RUN_TEST(
        test_default_port_and_empty_base_generate_exact_get_request);
    BACKEND_RUN_TEST(
        test_204_rejects_framing_headers_and_body_bytes);
    BACKEND_RUN_TEST(
        test_304_accepts_optional_content_length_metadata_without_body);
    BACKEND_RUN_TEST(
        test_304_rejects_transfer_encoding_and_body_bytes);
    BACKEND_RUN_TEST(
        test_informational_responses_are_skipped_before_final_response);
    BACKEND_RUN_TEST(
        test_switching_protocols_is_not_skipped_as_an_interim_response);
    BACKEND_RUN_TEST(
        test_more_than_eight_informational_responses_are_rejected);
    BACKEND_RUN_TEST(
        test_interim_and_final_headers_share_the_2048_byte_budget);
    BACKEND_RUN_TEST(
        test_status_headers_and_content_length_survive_every_split_boundary);
    BACKEND_RUN_TEST(
        test_chunk_size_body_and_terminator_survive_every_split_boundary);
    BACKEND_RUN_TEST(
        test_valid_chunk_extensions_and_trailers_survive_every_json_split_boundary);
    BACKEND_RUN_TEST(
        test_valid_chunk_extensions_and_trailers_survive_every_binary_split_boundary);
    BACKEND_RUN_TEST(
        test_chunk_extension_and_combined_trailer_header_limits_accept_exact_boundaries);
    BACKEND_RUN_TEST(
        test_chunk_quoted_extensions_accept_tab_obs_text_and_quoted_pairs);
    BACKEND_RUN_TEST(
        test_chunk_extensions_accept_rfc_bws_around_semicolon_and_equals);
    BACKEND_RUN_TEST(
        test_conflicting_and_duplicate_framing_headers_are_rejected);
    BACKEND_RUN_TEST(
        test_unsupported_or_missing_body_framing_is_rejected);
    BACKEND_RUN_TEST(
        test_header_limit_rejects_only_responses_over_2048_bytes);
    BACKEND_RUN_TEST(
        test_json_body_limit_capacity_truncation_and_trailing_bytes_are_rejected);
    BACKEND_RUN_TEST(
        test_invalid_chunk_syntax_truncation_and_trailing_bytes_are_rejected);
    BACKEND_RUN_TEST(
        test_malformed_or_oversized_chunk_extensions_are_rejected);
    BACKEND_RUN_TEST(
        test_malformed_forbidden_or_oversized_trailers_are_rejected);
    BACKEND_RUN_TEST(
        test_decoded_chunked_json_over_4096_is_rejected_before_copy);
    BACKEND_RUN_TEST(
        test_json_total_deadline_accepts_4999_and_rejects_5000_ms);
    BACKEND_RUN_TEST(
        test_binary_request_and_framing_preserve_arbitrary_bytes_exactly);
    BACKEND_RUN_TEST(
        test_binary_fixed_404_discards_body_without_firmware_sink);
    BACKEND_RUN_TEST(
        test_binary_chunked_404_discards_body_without_firmware_sink);
    BACKEND_RUN_TEST(
        test_binary_error_discard_rejects_body_over_4096_without_sink);
    BACKEND_RUN_TEST(
        test_binary_length_mismatch_and_sink_failure_are_rejected);
    BACKEND_RUN_TEST(
        test_binary_total_deadline_accepts_59999_and_rejects_60000_ms);
    BACKEND_RUN_TEST(
        test_binary_no_progress_accepts_4999_and_rejects_5000_ms);
    BACKEND_RUN_TEST(
        test_binary_no_progress_resets_after_resolve_connect_and_each_partial_send);
    BACKEND_RUN_TEST(
        test_binary_pre_response_no_progress_rejects_each_exact_5000_ms_boundary);
    BACKEND_RUN_TEST(
        test_binary_partial_send_total_deadline_accepts_59999_and_rejects_60000_ms);
    BACKEND_RUN_TEST(
        test_invalid_url_dns_connect_and_send_failures_are_distinct);
    BACKEND_RUN_TEST(
        test_dns_guard_pool_exhausts_then_reuses_without_late_callback_corruption);
    BACKEND_RUN_TEST(
        test_platform_request_guard_requires_initialized_non_lwip_task);
    return UNITY_END();
}
