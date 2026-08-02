#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "backend_json_reader.h"
#include "backend_json_writer.h"

static void assert_parse_result(const char *json, size_t length,
                                backend_json_result_t expected)
{
    backend_json_token_t tokens[BACKEND_JSON_MAX_TOKENS];
    size_t count = 99U;
    TEST_ASSERT_EQUAL(expected, backend_json_parse(
        json, length, tokens, BACKEND_JSON_MAX_TOKENS, &count));
    if (expected != BACKEND_JSON_OK) {
        TEST_ASSERT_EQUAL_size_t(0U, count);
    }
}

void test_backend_json_writer_escapes_and_fails_closed(void)
{
    char output[160];
    backend_json_writer_t writer;
    backend_json_writer_init(&writer, output, sizeof(output));

    TEST_ASSERT_TRUE(backend_json_append(&writer, "{\"value\":"));
    TEST_ASSERT_TRUE(backend_json_append_escaped(
        &writer,
        "quote\" slash\\ solidus/ \b\f\n\r\t " "\x01" " \xe2\x82\xac"));
    TEST_ASSERT_TRUE(backend_json_append_format(&writer, ",\"count\":%d}", 7));
    TEST_ASSERT_EQUAL_STRING(
        "{\"value\":\"quote\\\" slash\\\\ solidus/ "
        "\\b\\f\\n\\r\\t \\u0001 \xe2\x82\xac\",\"count\":7}",
        output);
    TEST_ASSERT_EQUAL_size_t(strlen(output), backend_json_writer_finish(&writer));

    char too_small[12];
    memset(too_small, 'X', sizeof(too_small));
    backend_json_writer_init(&writer, too_small, sizeof(too_small));
    TEST_ASSERT_TRUE(backend_json_append(&writer, "{\"v\":"));
    TEST_ASSERT_FALSE(backend_json_append_escaped(&writer, "0123456789"));
    TEST_ASSERT_EQUAL_CHAR('\0', too_small[0]);
    TEST_ASSERT_FALSE(backend_json_append(&writer, "ignored"));
    TEST_ASSERT_EQUAL_size_t(0U, backend_json_writer_finish(&writer));
    TEST_ASSERT_EQUAL_CHAR('\0', too_small[0]);

    const char invalid_utf8[] = {'b', (char)0xc0, (char)0xaf, '\0'};
    backend_json_writer_init(&writer, output, sizeof(output));
    TEST_ASSERT_FALSE(backend_json_append_escaped(&writer, invalid_utf8));
    TEST_ASSERT_EQUAL_CHAR('\0', output[0]);
    TEST_ASSERT_EQUAL_size_t(0U, backend_json_writer_finish(&writer));
}

void test_backend_json_reader_typed_accessors(void)
{
    static const char document[] =
        "{\"\\u0074ext\":\"A\\n\\u00a2\\u20ac\\ud83d\\ude00\","
        "\"truth\":true,\"falsehood\":false,"
        "\"i\":-9223372036854775808,"
        "\"u\":18446744073709551615,\"d\":-1.25e2,\"n\":null}TRAIL";
    const size_t json_length = sizeof(document) - sizeof("TRAIL");
    backend_json_token_t tokens[32];
    size_t count = 0U;
    TEST_ASSERT_EQUAL(BACKEND_JSON_OK, backend_json_parse(
        document, json_length, tokens, 32U, &count));
    TEST_ASSERT_GREATER_THAN(0U, count);
    TEST_ASSERT_EQUAL(BACKEND_JSON_OBJECT, tokens[0].kind);

    size_t index = 0U;
    char decoded[32];
    TEST_ASSERT_TRUE(backend_json_object_find(
        document, tokens, count, 0U, "text", &index));
    TEST_ASSERT_EQUAL(BACKEND_JSON_STRING, tokens[index].kind);
    TEST_ASSERT_TRUE(backend_json_copy_string(
        document, &tokens[index], decoded, sizeof(decoded)));
    TEST_ASSERT_EQUAL_STRING(
        "A\n\xc2\xa2\xe2\x82\xac\xf0\x9f\x98\x80", decoded);

    char too_small[4] = {'X', 'X', 'X', 'X'};
    TEST_ASSERT_FALSE(backend_json_copy_string(
        document, &tokens[index], too_small, sizeof(too_small)));
    TEST_ASSERT_EQUAL_CHAR('\0', too_small[0]);

    bool boolean = false;
    TEST_ASSERT_TRUE(backend_json_object_find(
        document, tokens, count, 0U, "truth", &index));
    TEST_ASSERT_TRUE(backend_json_get_bool(document, &tokens[index], &boolean));
    TEST_ASSERT_TRUE(boolean);
    TEST_ASSERT_FALSE(backend_json_get_i64(document, &tokens[index], NULL));

    TEST_ASSERT_TRUE(backend_json_object_find(
        document, tokens, count, 0U, "falsehood", &index));
    TEST_ASSERT_TRUE(backend_json_get_bool(document, &tokens[index], &boolean));
    TEST_ASSERT_FALSE(boolean);

    int64_t signed_value = 0;
    TEST_ASSERT_TRUE(backend_json_object_find(
        document, tokens, count, 0U, "i", &index));
    TEST_ASSERT_TRUE(backend_json_get_i64(
        document, &tokens[index], &signed_value));
    TEST_ASSERT_EQUAL_INT64(INT64_MIN, signed_value);
    uint64_t unsigned_value = 0U;
    TEST_ASSERT_FALSE(backend_json_get_u64(
        document, &tokens[index], &unsigned_value));

    TEST_ASSERT_TRUE(backend_json_object_find(
        document, tokens, count, 0U, "u", &index));
    TEST_ASSERT_TRUE(backend_json_get_u64(
        document, &tokens[index], &unsigned_value));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, unsigned_value);
    TEST_ASSERT_FALSE(backend_json_get_i64(
        document, &tokens[index], &signed_value));

    double double_value = 0.0;
    TEST_ASSERT_TRUE(backend_json_object_find(
        document, tokens, count, 0U, "d", &index));
    TEST_ASSERT_TRUE(backend_json_get_double(
        document, &tokens[index], &double_value));
    TEST_ASSERT_DOUBLE_WITHIN(0.000001, -125.0, double_value);
    TEST_ASSERT_FALSE(backend_json_get_u64(
        document, &tokens[index], &unsigned_value));

    TEST_ASSERT_FALSE(backend_json_object_find(
        document, tokens, count, 0U, "missing", &index));
    TEST_ASSERT_FALSE(backend_json_object_find(
        document, tokens, count, count, "text", &index));
}

void test_backend_json_reader_rejects_malformed_and_limits(void)
{
    const char invalid_utf8[] = {'"', (char)0xc0, (char)0xaf, '"'};
    const char embedded_nul[] = {'{', '"', 'a', '"', ':', '1', '\0', '}'};
    assert_parse_result(invalid_utf8, sizeof(invalid_utf8), BACKEND_JSON_MALFORMED);
    assert_parse_result("\"\\x20\"", 6U, BACKEND_JSON_MALFORMED);
    assert_parse_result("\"\\u12g4\"", 8U, BACKEND_JSON_MALFORMED);
    assert_parse_result("\"\\ud800\"", 8U, BACKEND_JSON_MALFORMED);
    assert_parse_result("\"\\ud800\\u0041\"", 14U, BACKEND_JSON_MALFORMED);
    assert_parse_result("\"\\u0000\"", 8U, BACKEND_JSON_MALFORMED);
    assert_parse_result(embedded_nul, sizeof(embedded_nul), BACKEND_JSON_MALFORMED);
    assert_parse_result("{\"a\":1}x", 8U, BACKEND_JSON_MALFORMED);

    assert_parse_result("{\"a\":1,\"a\":2}", 13U,
                        BACKEND_JSON_DUPLICATE_KEY);
    assert_parse_result("{\"a\":1,\"\\u0061\":2}", 18U,
                        BACKEND_JSON_DUPLICATE_KEY);
    assert_parse_result("{\"x\":{\"a\":1},\"y\":{\"a\":2}}", 25U,
                        BACKEND_JSON_OK);

    assert_parse_result("[[[[]]]]", 8U, BACKEND_JSON_OK);
    assert_parse_result("[[[[[]]]]]", 10U, BACKEND_JSON_TOO_DEEP);

    char token_document[600];
    size_t position = 0U;
    token_document[position++] = '[';
    for (size_t i = 0U; i < 255U; ++i) {
        if (i != 0U) {
            token_document[position++] = ',';
        }
        token_document[position++] = '0';
    }
    token_document[position++] = ']';
    assert_parse_result(token_document, position, BACKEND_JSON_OK);
    token_document[position - 1U] = ',';
    token_document[position++] = '0';
    token_document[position++] = ']';
    assert_parse_result(token_document, position, BACKEND_JSON_TOO_MANY_TOKENS);

    backend_json_token_t tiny_tokens[2];
    size_t tiny_count = 3U;
    TEST_ASSERT_EQUAL(BACKEND_JSON_TOO_MANY_TOKENS, backend_json_parse(
        "[0,1]", 5U, tiny_tokens, 2U, &tiny_count));
    TEST_ASSERT_EQUAL_size_t(0U, tiny_count);

    assert_parse_result("18446744073709551616", 20U, BACKEND_JSON_RANGE);
    assert_parse_result("-9223372036854775809", 20U, BACKEND_JSON_RANGE);
    assert_parse_result("1e309", 5U, BACKEND_JSON_RANGE);
    assert_parse_result("NaN", 3U, BACKEND_JSON_MALFORMED);
    assert_parse_result("Infinity", 8U, BACKEND_JSON_MALFORMED);
    assert_parse_result("-Infinity", 9U, BACKEND_JSON_MALFORMED);
    assert_parse_result("01", 2U, BACKEND_JSON_MALFORMED);
    assert_parse_result("1.", 2U, BACKEND_JSON_MALFORMED);
    assert_parse_result("1e", 2U, BACKEND_JSON_MALFORMED);
}
