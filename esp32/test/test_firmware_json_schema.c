#include "unity.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "firmware_json_schema.h"

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))

#define EXACT_FRAME(string_value, nullable_value, bool_value, int_value,      \
                    uint_value, object_value)                                 \
    "{\"s\":" string_value ",\"nullable\":" nullable_value                  \
    ",\"flag\":" bool_value ",\"signed32\":" int_value                      \
    ",\"unsigned32\":" uint_value ",\"nested\":" object_value "}"

typedef struct {
    const char *label;
    const uint8_t *bytes;
    size_t byte_len;
    fof_json_schema_result_t expected;
} schema_case_t;

#define SCHEMA_CASE(label_value, wire_value, expected_value)                  \
    {                                                                         \
        (label_value), (const uint8_t *)(wire_value),                         \
        sizeof(wire_value) - 1U, (expected_value)                             \
    }

static const fof_json_member_spec_t EXACT_SCHEMA[] = {
    {"s", FOF_JSON_STRING, FOF_JSON_STRING_POLICY_PRINTABLE_UTF8},
    {"nullable", FOF_JSON_NULLABLE_STRING,
     FOF_JSON_STRING_POLICY_PRINTABLE_UTF8},
    {"flag", FOF_JSON_BOOL, FOF_JSON_STRING_POLICY_NONE},
    {"signed32", FOF_JSON_INT32, FOF_JSON_STRING_POLICY_NONE},
    {"unsigned32", FOF_JSON_UINT32, FOF_JSON_STRING_POLICY_NONE},
    {"nested", FOF_JSON_OBJECT, FOF_JSON_STRING_POLICY_NONE},
};

static fof_json_schema_result_t validate_exact(const uint8_t *bytes,
                                               size_t byte_len)
{
    return fof_json_validate_exact_object(
        bytes, byte_len, EXACT_SCHEMA, ARRAY_SIZE(EXACT_SCHEMA));
}

static void assert_schema_cases(const schema_case_t *cases, size_t count)
{
    for (size_t i = 0U; i < count; ++i) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            cases[i].expected,
            validate_exact(cases[i].bytes, cases[i].byte_len),
            cases[i].label);
    }
}

void test_firmware_json_schema_accepts_exact_object_and_all_wire_types(void)
{
    static const uint8_t canonical[] = EXACT_FRAME(
        "\"plain\"", "null", "true", "-2147483648", "4294967295",
        "{\"inner\":\"value\",\"array\":[null,false,-1.25e+2]}");
    static const uint8_t reordered[] =
        " \t\r\n{\"nested\":{},\"unsigned32\":0,\"signed32\":2147483647,"
        "\"flag\":false,\"nullable\":\"text\",\"s\":"
        "\"a\\\"b\\\\c\\/d\\u20ac\\ud83d\\ude00\"} \r\n";
    static const uint8_t signed_negative_zero[] = EXACT_FRAME(
        "\"ok\"", "\"optional\"", "false", "-0", "0", "{}");
    static const uint8_t empty_object[] = " \t{}\r\n";

    TEST_ASSERT_EQUAL_INT(
        FOF_JSON_SCHEMA_OK,
        validate_exact(canonical, sizeof(canonical) - 1U));
    TEST_ASSERT_EQUAL_INT(
        FOF_JSON_SCHEMA_OK,
        validate_exact(reordered, sizeof(reordered) - 1U));
    TEST_ASSERT_EQUAL_INT(
        FOF_JSON_SCHEMA_OK,
        validate_exact(signed_negative_zero,
                       sizeof(signed_negative_zero) - 1U));
    TEST_ASSERT_EQUAL_INT(
        FOF_JSON_SCHEMA_OK,
        fof_json_validate_exact_object(
            empty_object, sizeof(empty_object) - 1U, NULL, 0U));
}

void test_firmware_json_schema_uses_explicit_length_for_embedded_nul(void)
{
    static const uint8_t complete_then_nul[] =
        EXACT_FRAME("\"ok\"", "null", "true", "0", "0", "{}")
        "\0{\"s\":\"authorization-looking suffix\"}";
    static const uint8_t nul_inside_string[] =
        "{\"s\":\"ok\0evil\",\"nullable\":null,\"flag\":true,"
        "\"signed32\":0,\"unsigned32\":0,\"nested\":{}}";

    TEST_ASSERT_EQUAL_INT(
        FOF_JSON_SCHEMA_EMBEDDED_NUL,
        validate_exact(complete_then_nul, sizeof(complete_then_nul) - 1U));
    TEST_ASSERT_EQUAL_INT(
        FOF_JSON_SCHEMA_EMBEDDED_NUL,
        validate_exact(nul_inside_string, sizeof(nul_inside_string) - 1U));
}

void test_firmware_json_schema_rejects_malformed_roots_and_trailing_data(void)
{
    static const schema_case_t cases[] = {
        SCHEMA_CASE("empty", "", FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("array root", "[]", FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("null root", "null", FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("truncated root", "{\"s\":\"ok\"",
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE(
            "missing colon",
            "{\"s\" \"ok\",\"nullable\":null,\"flag\":true,"
            "\"signed32\":0,\"unsigned32\":0,\"nested\":{}}",
            FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE(
            "missing comma",
            "{\"s\":\"ok\" \"nullable\":null,\"flag\":true,"
            "\"signed32\":0,\"unsigned32\":0,\"nested\":{}}",
            FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE(
            "trailing comma",
            "{\"s\":\"ok\",\"nullable\":null,\"flag\":true,"
            "\"signed32\":0,\"unsigned32\":0,\"nested\":{},}",
            FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE(
            "trailing bytes",
            EXACT_FRAME("\"ok\"", "null", "true", "0", "0", "{}")
            "junk",
            FOF_JSON_SCHEMA_TRAILING_DATA),
        SCHEMA_CASE(
            "trailing second object",
            EXACT_FRAME("\"ok\"", "null", "true", "0", "0", "{}")
            "{}",
            FOF_JSON_SCHEMA_TRAILING_DATA),
        SCHEMA_CASE(
            "trailing second scalar",
            EXACT_FRAME("\"ok\"", "null", "true", "0", "0", "{}")
            " true",
            FOF_JSON_SCHEMA_TRAILING_DATA),
        SCHEMA_CASE(
            "trailing data outranks missing members",
            "{}junk",
            FOF_JSON_SCHEMA_TRAILING_DATA),
    };

    assert_schema_cases(cases, ARRAY_SIZE(cases));
}

void test_firmware_json_schema_rejects_escaped_duplicate_unknown_and_missing_members(
    void)
{
    static const schema_case_t cases[] = {
        SCHEMA_CASE(
            "escaped allowed name",
            "{\"\\u0073\":\"ok\",\"nullable\":null,\"flag\":true,"
            "\"signed32\":0,\"unsigned32\":0,\"nested\":{}}",
            FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE(
            "duplicate near front",
            "{\"s\":\"one\",\"s\":\"two\",\"nullable\":null,"
            "\"flag\":true,\"signed32\":0,\"unsigned32\":0,"
            "\"nested\":{}}",
            FOF_JSON_SCHEMA_DUPLICATE),
        SCHEMA_CASE(
            "duplicate at end",
            "{\"nullable\":null,\"flag\":true,\"signed32\":0,"
            "\"unsigned32\":0,\"nested\":{},\"s\":\"one\","
            "\"s\":\"two\"}",
            FOF_JSON_SCHEMA_DUPLICATE),
        SCHEMA_CASE(
            "unknown",
            "{\"s\":\"ok\",\"nullable\":null,\"flag\":true,"
            "\"signed32\":0,\"unsigned32\":0,\"nested\":{},"
            "\"unknown\":false}",
            FOF_JSON_SCHEMA_UNKNOWN),
        SCHEMA_CASE(
            "missing string",
            "{\"nullable\":null,\"flag\":true,\"signed32\":0,"
            "\"unsigned32\":0,\"nested\":{}}",
            FOF_JSON_SCHEMA_MISSING),
        SCHEMA_CASE(
            "missing nullable",
            "{\"s\":\"ok\",\"flag\":true,\"signed32\":0,"
            "\"unsigned32\":0,\"nested\":{}}",
            FOF_JSON_SCHEMA_MISSING),
        SCHEMA_CASE(
            "missing bool",
            "{\"s\":\"ok\",\"nullable\":null,\"signed32\":0,"
            "\"unsigned32\":0,\"nested\":{}}",
            FOF_JSON_SCHEMA_MISSING),
        SCHEMA_CASE(
            "missing int32",
            "{\"s\":\"ok\",\"nullable\":null,\"flag\":true,"
            "\"unsigned32\":0,\"nested\":{}}",
            FOF_JSON_SCHEMA_MISSING),
        SCHEMA_CASE(
            "missing uint32",
            "{\"s\":\"ok\",\"nullable\":null,\"flag\":true,"
            "\"signed32\":0,\"nested\":{}}",
            FOF_JSON_SCHEMA_MISSING),
        SCHEMA_CASE(
            "missing object",
            "{\"s\":\"ok\",\"nullable\":null,\"flag\":true,"
            "\"signed32\":0,\"unsigned32\":0}",
            FOF_JSON_SCHEMA_MISSING),
    };

    assert_schema_cases(cases, ARRAY_SIZE(cases));
}

void test_firmware_json_schema_enforces_string_bool_and_object_wire_types(void)
{
    static const schema_case_t cases[] = {
        SCHEMA_CASE("string as number",
                    EXACT_FRAME("7", "null", "true", "0", "0", "{}"),
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("string as null",
                    EXACT_FRAME("null", "null", "true", "0", "0", "{}"),
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("nullable as bool",
                    EXACT_FRAME("\"ok\"", "false", "true", "0", "0", "{}"),
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("numeric true",
                    EXACT_FRAME("\"ok\"", "null", "1", "0", "0", "{}"),
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("numeric false",
                    EXACT_FRAME("\"ok\"", "null", "0", "0", "0", "{}"),
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("object as array",
                    EXACT_FRAME("\"ok\"", "null", "true", "0", "0", "[]"),
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("object as null",
                    EXACT_FRAME("\"ok\"", "null", "true", "0", "0", "null"),
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("object as string",
                    EXACT_FRAME("\"ok\"", "null", "true", "0", "0", "\"{}\""),
                    FOF_JSON_SCHEMA_WRONG_TYPE),
    };

    assert_schema_cases(cases, ARRAY_SIZE(cases));
}

void test_firmware_json_schema_enforces_integer_grammar_sign_and_bounds(void)
{
    static const schema_case_t cases[] = {
        SCHEMA_CASE("int32 positive overflow",
                    EXACT_FRAME("\"ok\"", "null", "true", "2147483648",
                                "0", "{}"),
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("int32 negative overflow",
                    EXACT_FRAME("\"ok\"", "null", "true", "-2147483649",
                                "0", "{}"),
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("uint32 overflow",
                    EXACT_FRAME("\"ok\"", "null", "true", "0",
                                "4294967296", "{}"),
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("uint32 negative",
                    EXACT_FRAME("\"ok\"", "null", "true", "0", "-1", "{}"),
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("uint32 negative zero",
                    EXACT_FRAME("\"ok\"", "null", "true", "0", "-0", "{}"),
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("int32 fraction",
                    EXACT_FRAME("\"ok\"", "null", "true", "1.0", "0", "{}"),
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("int32 exponent",
                    EXACT_FRAME("\"ok\"", "null", "true", "1e0", "0", "{}"),
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("uint32 fraction",
                    EXACT_FRAME("\"ok\"", "null", "true", "0", "1.0", "{}"),
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("uint32 exponent",
                    EXACT_FRAME("\"ok\"", "null", "true", "0", "1e0", "{}"),
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("leading zero",
                    EXACT_FRAME("\"ok\"", "null", "true", "01", "0", "{}"),
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("bare minus",
                    EXACT_FRAME("\"ok\"", "null", "true", "-", "0", "{}"),
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("missing fraction digit",
                    EXACT_FRAME("\"ok\"", "null", "true", "1.", "0", "{}"),
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("missing exponent digit",
                    EXACT_FRAME("\"ok\"", "null", "true", "1e", "0", "{}"),
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("leading plus",
                    EXACT_FRAME("\"ok\"", "null", "true", "+1", "0", "{}"),
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("nan",
                    EXACT_FRAME("\"ok\"", "null", "true", "NaN", "0", "{}"),
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("infinity",
                    EXACT_FRAME("\"ok\"", "null", "true", "Infinity", "0",
                                "{}"),
                    FOF_JSON_SCHEMA_MALFORMED),
    };

    assert_schema_cases(cases, ARRAY_SIZE(cases));
}

void test_firmware_json_schema_handles_nested_values_without_top_level_confusion(
    void)
{
    static const uint8_t nested_valid[] = EXACT_FRAME(
        "\"outer\"", "null", "true", "0", "0",
        "{\"s\":\"inner\",\"unknown\":{\"flag\":1},"
        "\"array\":[{\"nullable\":false},[1,2,3]],"
        "\"quoted\":\"braces } ] , : {\"}");
    static const uint8_t nested_malformed[] = EXACT_FRAME(
        "\"outer\"", "null", "true", "0", "0",
        "{\"array\":[1,],\"s\":\"inner\"}");

    TEST_ASSERT_EQUAL_INT(
        FOF_JSON_SCHEMA_OK,
        validate_exact(nested_valid, sizeof(nested_valid) - 1U));
    TEST_ASSERT_EQUAL_INT(
        FOF_JSON_SCHEMA_MALFORMED,
        validate_exact(nested_malformed, sizeof(nested_malformed) - 1U));
}

void test_firmware_json_schema_rejects_raw_escaped_controls_and_bad_unicode(void)
{
    static const schema_case_t cases[] = {
        SCHEMA_CASE("raw c0",
                    EXACT_FRAME("\"bad\x1f\"", "null", "true", "0", "0",
                                "{}"),
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("raw del",
                    EXACT_FRAME("\"bad\x7f\"", "null", "true", "0", "0",
                                "{}"),
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("escaped backspace",
                    EXACT_FRAME("\"bad\\b\"", "null", "true", "0", "0",
                                "{}"),
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("escaped newline",
                    EXACT_FRAME("\"bad\\n\"", "null", "true", "0", "0",
                                "{}"),
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("escaped nul",
                    EXACT_FRAME("\"bad\\u0000\"", "null", "true", "0", "0",
                                "{}"),
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("escaped unit separator",
                    EXACT_FRAME("\"bad\\u001f\"", "null", "true", "0", "0",
                                "{}"),
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("escaped del",
                    EXACT_FRAME("\"bad\\u007f\"", "null", "true", "0", "0",
                                "{}"),
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("invalid escape",
                    EXACT_FRAME("\"bad\\q\"", "null", "true", "0", "0",
                                "{}"),
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("short unicode",
                    EXACT_FRAME("\"bad\\u12\"", "null", "true", "0", "0",
                                "{}"),
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("lone high surrogate",
                    EXACT_FRAME("\"bad\\ud800\"", "null", "true", "0", "0",
                                "{}"),
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("lone low surrogate",
                    EXACT_FRAME("\"bad\\udc00\"", "null", "true", "0", "0",
                                "{}"),
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("invalid surrogate pair",
                    EXACT_FRAME("\"bad\\ud800\\u0041\"", "null", "true", "0",
                                "0", "{}"),
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE(
            "nested escaped nul",
            EXACT_FRAME("\"ok\"", "null", "true", "0", "0",
                        "{\"reason\":\"bad\\u0000\"}"),
            FOF_JSON_SCHEMA_MALFORMED),
    };

    assert_schema_cases(cases, ARRAY_SIZE(cases));
}

void test_firmware_json_schema_validates_raw_utf8_scalar_sequences(void)
{
    static const schema_case_t cases[] = {
        SCHEMA_CASE(
            "valid two three and four byte scalars",
            EXACT_FRAME(
                "\"cent \xC2\xA2 euro \xE2\x82\xAC face "
                "\xF0\x9F\x98\x80\"",
                "null", "true", "0", "0", "{}"),
            FOF_JSON_SCHEMA_OK),
        SCHEMA_CASE(
            "lone continuation",
            EXACT_FRAME("\"bad \x80\"", "null", "true", "0", "0", "{}"),
            FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE(
            "overlong two byte scalar",
            EXACT_FRAME(
                "\"bad \xC0\xAF\"", "null", "true", "0", "0", "{}"),
            FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE(
            "overlong three byte scalar",
            EXACT_FRAME(
                "\"bad \xE0\x80\xAF\"", "null", "true", "0", "0", "{}"),
            FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE(
            "truncated three byte scalar",
            EXACT_FRAME(
                "\"bad \xE2\x82\"", "null", "true", "0", "0", "{}"),
            FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE(
            "utf8 encoded surrogate",
            EXACT_FRAME(
                "\"bad \xED\xA0\x80\"", "null", "true", "0", "0", "{}"),
            FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE(
            "scalar above unicode maximum",
            EXACT_FRAME(
                "\"bad \xF4\x90\x80\x80\"", "null", "true", "0", "0",
                "{}"),
            FOF_JSON_SCHEMA_MALFORMED),
    };

    assert_schema_cases(cases, ARRAY_SIZE(cases));
}

static fof_json_schema_result_t validate_single_member(
    const uint8_t *bytes,
    size_t byte_len,
    fof_json_wire_type_t type,
    fof_json_string_policy_t string_policy)
{
    const fof_json_member_spec_t schema[] = {
        {"value", type, string_policy},
    };
    return fof_json_validate_exact_object(
        bytes, byte_len, schema, ARRAY_SIZE(schema));
}

void test_firmware_json_schema_enforces_ascii_token_no_escape_policy(void)
{
    static const schema_case_t cases[] = {
        SCHEMA_CASE("empty remains value policy",
                    "{\"value\":\"\"}", FOF_JSON_SCHEMA_OK),
        SCHEMA_CASE("printable token punctuation",
                    "{\"value\":\"aA0-._:/,=+@[]{}\"}",
                    FOF_JSON_SCHEMA_OK),
        SCHEMA_CASE("escaped printable ascii",
                    "{\"value\":\"firm\\u0077are\"}",
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("escaped quote",
                    "{\"value\":\"bad\\\"token\"}",
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("escaped slash",
                    "{\"value\":\"bad\\/token\"}",
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("raw non ascii",
                    "{\"value\":\"bad\xE2\x82\xAC\"}",
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("leading space",
                    "{\"value\":\" token\"}",
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("embedded space",
                    "{\"value\":\"bad token\"}",
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("trailing space",
                    "{\"value\":\"token \"}",
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("raw c0",
                    "{\"value\":\"bad\x1f\"}",
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("raw del",
                    "{\"value\":\"bad\x7f\"}",
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("escaped nul",
                    "{\"value\":\"bad\\u0000\"}",
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("escaped unit separator",
                    "{\"value\":\"bad\\u001f\"}",
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("escaped del",
                    "{\"value\":\"bad\\u007f\"}",
                    FOF_JSON_SCHEMA_MALFORMED),
    };

    for (size_t i = 0U; i < ARRAY_SIZE(cases); ++i) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            cases[i].expected,
            validate_single_member(
                cases[i].bytes, cases[i].byte_len,
                FOF_JSON_STRING,
                FOF_JSON_STRING_POLICY_ASCII_TOKEN_NO_ESCAPE),
            cases[i].label);
    }
}

void test_firmware_json_schema_allows_printable_utf8_diagnostics_only(void)
{
    static const schema_case_t cases[] = {
        SCHEMA_CASE("empty diagnostic",
                    "{\"value\":\"\"}", FOF_JSON_SCHEMA_OK),
        SCHEMA_CASE("raw utf8 diagnostic",
                    "{\"value\":\"cent \xC2\xA2 euro \xE2\x82\xAC "
                    "face \xF0\x9F\x98\x80\"}",
                    FOF_JSON_SCHEMA_OK),
        SCHEMA_CASE("escaped printable diagnostic",
                    "{\"value\":\"quote \\\" slash \\/ backslash \\\\ "
                    "euro \\u20ac face \\ud83d\\ude00\"}",
                    FOF_JSON_SCHEMA_OK),
        SCHEMA_CASE("raw c0 diagnostic",
                    "{\"value\":\"bad\x1f\"}",
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("raw del diagnostic",
                    "{\"value\":\"bad\x7f\"}",
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("escaped nul diagnostic",
                    "{\"value\":\"bad\\u0000\"}",
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("escaped unit separator diagnostic",
                    "{\"value\":\"bad\\u001f\"}",
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("escaped del diagnostic",
                    "{\"value\":\"bad\\u007f\"}",
                    FOF_JSON_SCHEMA_MALFORMED),
    };

    for (size_t i = 0U; i < ARRAY_SIZE(cases); ++i) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            cases[i].expected,
            validate_single_member(
                cases[i].bytes, cases[i].byte_len,
                FOF_JSON_STRING,
                FOF_JSON_STRING_POLICY_PRINTABLE_UTF8),
            cases[i].label);
    }
}

void test_firmware_json_schema_applies_nullable_policy_and_rejects_bad_specs(
    void)
{
    static const uint8_t null_value[] = "{\"value\":null}";
    static const uint8_t plain_value[] = "{\"value\":\"token\"}";
    static const uint8_t escaped_value[] =
        "{\"value\":\"to\\u006ben\"}";
    static const uint8_t raw_utf8_value[] =
        "{\"value\":\"\xE2\x82\xAC\"}";
    static const uint8_t numeric_value[] = "{\"value\":1}";
    static const fof_json_member_spec_t string_without_policy[] = {
        {"value", FOF_JSON_STRING, FOF_JSON_STRING_POLICY_NONE},
    };
    static const fof_json_member_spec_t nullable_without_policy[] = {
        {"value", FOF_JSON_NULLABLE_STRING, FOF_JSON_STRING_POLICY_NONE},
    };
    static const fof_json_member_spec_t bool_with_token_policy[] = {
        {"value", FOF_JSON_BOOL,
         FOF_JSON_STRING_POLICY_ASCII_TOKEN_NO_ESCAPE},
    };
    static const fof_json_member_spec_t uint_with_diagnostic_policy[] = {
        {"value", FOF_JSON_UINT32, FOF_JSON_STRING_POLICY_PRINTABLE_UTF8},
    };
    static const fof_json_member_spec_t string_with_unknown_policy[] = {
        {"value", FOF_JSON_STRING, (fof_json_string_policy_t)99},
    };
    static const fof_json_member_spec_t numeric_with_none[] = {
        {"value", FOF_JSON_UINT32, FOF_JSON_STRING_POLICY_NONE},
    };

    TEST_ASSERT_EQUAL_INT(
        FOF_JSON_SCHEMA_OK,
        validate_single_member(
            null_value, sizeof(null_value) - 1U,
            FOF_JSON_NULLABLE_STRING,
            FOF_JSON_STRING_POLICY_ASCII_TOKEN_NO_ESCAPE));
    TEST_ASSERT_EQUAL_INT(
        FOF_JSON_SCHEMA_OK,
        validate_single_member(
            plain_value, sizeof(plain_value) - 1U,
            FOF_JSON_NULLABLE_STRING,
            FOF_JSON_STRING_POLICY_ASCII_TOKEN_NO_ESCAPE));
    TEST_ASSERT_EQUAL_INT(
        FOF_JSON_SCHEMA_WRONG_TYPE,
        validate_single_member(
            escaped_value, sizeof(escaped_value) - 1U,
            FOF_JSON_NULLABLE_STRING,
            FOF_JSON_STRING_POLICY_ASCII_TOKEN_NO_ESCAPE));
    TEST_ASSERT_EQUAL_INT(
        FOF_JSON_SCHEMA_WRONG_TYPE,
        validate_single_member(
            raw_utf8_value, sizeof(raw_utf8_value) - 1U,
            FOF_JSON_NULLABLE_STRING,
            FOF_JSON_STRING_POLICY_ASCII_TOKEN_NO_ESCAPE));

    const struct {
        const char *label;
        const fof_json_member_spec_t *schema;
        const uint8_t *wire;
        size_t wire_len;
    } invalid_specs[] = {
        {
            "string without policy",
            string_without_policy,
            plain_value,
            sizeof(plain_value) - 1U,
        },
        {
            "nullable without policy",
            nullable_without_policy,
            null_value,
            sizeof(null_value) - 1U,
        },
        {
            "bool with token policy",
            bool_with_token_policy,
            plain_value,
            sizeof(plain_value) - 1U,
        },
        {
            "uint with diagnostic policy",
            uint_with_diagnostic_policy,
            numeric_value,
            sizeof(numeric_value) - 1U,
        },
        {
            "string with unknown policy",
            string_with_unknown_policy,
            plain_value,
            sizeof(plain_value) - 1U,
        },
    };
    for (size_t i = 0U; i < ARRAY_SIZE(invalid_specs); ++i) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            FOF_JSON_SCHEMA_MALFORMED,
            fof_json_validate_exact_object(
                invalid_specs[i].wire,
                invalid_specs[i].wire_len,
                invalid_specs[i].schema,
                1U),
            invalid_specs[i].label);
    }
    TEST_ASSERT_EQUAL_INT(
        FOF_JSON_SCHEMA_OK,
        fof_json_validate_exact_object(
            numeric_value, sizeof(numeric_value) - 1U,
            numeric_with_none, ARRAY_SIZE(numeric_with_none)));
}

static size_t build_nested_object_frame(uint8_t *out, size_t out_capacity,
                                        size_t nested_object_count)
{
    static const char prefix[] =
        "{\"s\":\"ok\",\"nullable\":null,\"flag\":true,\"signed32\":0,"
        "\"unsigned32\":0,\"nested\":";
    static const char open_object[] = "{\"x\":";
    size_t used = 0U;

    if (!out || out_capacity == 0U ||
        sizeof(prefix) - 1U >= out_capacity) {
        return 0U;
    }
    memcpy(out, prefix, sizeof(prefix) - 1U);
    used = sizeof(prefix) - 1U;
    for (size_t i = 0U; i < nested_object_count; ++i) {
        if (used + sizeof(open_object) - 1U >= out_capacity) {
            return 0U;
        }
        memcpy(out + used, open_object, sizeof(open_object) - 1U);
        used += sizeof(open_object) - 1U;
    }
    if (used + 4U + nested_object_count + 1U >= out_capacity) {
        return 0U;
    }
    memcpy(out + used, "null", 4U);
    used += 4U;
    for (size_t i = 0U; i < nested_object_count; ++i) {
        out[used++] = '}';
    }
    out[used++] = '}';
    out[used] = '\0';
    return used;
}

static size_t build_many_member_frame(
    uint8_t *out, size_t out_capacity,
    fof_json_member_spec_t *members, char names[][8],
    size_t member_count)
{
    size_t used = 0U;
    if (!out || out_capacity < 3U || !members || !names) {
        return 0U;
    }
    out[used++] = '{';
    for (size_t i = 0U; i < member_count; ++i) {
        int name_len = snprintf(names[i], sizeof(names[i]), "m%02u",
                                (unsigned)i);
        if (name_len <= 0 || (size_t)name_len >= sizeof(names[i])) {
            return 0U;
        }
        members[i].name = names[i];
        members[i].type = FOF_JSON_STRING;
        members[i].string_policy = FOF_JSON_STRING_POLICY_PRINTABLE_UTF8;
        int written = snprintf(
            (char *)out + used, out_capacity - used,
            "%s\"%s\":\"x\"", i == 0U ? "" : ",", names[i]);
        if (written <= 0 || (size_t)written >= out_capacity - used) {
            return 0U;
        }
        used += (size_t)written;
    }
    if (used + 2U > out_capacity) {
        return 0U;
    }
    out[used++] = '}';
    out[used] = '\0';
    return used;
}

void test_firmware_json_schema_enforces_argument_member_and_depth_bounds(void)
{
    static const uint8_t empty_object[] = "{}";
    static const fof_json_member_spec_t null_name[] = {
        {NULL, FOF_JSON_STRING, FOF_JSON_STRING_POLICY_PRINTABLE_UTF8},
    };
    static const fof_json_member_spec_t empty_name[] = {
        {"", FOF_JSON_STRING, FOF_JSON_STRING_POLICY_PRINTABLE_UTF8},
    };
    static const fof_json_member_spec_t invalid_type[] = {
        {"s", (fof_json_wire_type_t)99, FOF_JSON_STRING_POLICY_NONE},
    };
    static const fof_json_member_spec_t duplicate_specs[] = {
        {"s", FOF_JSON_STRING, FOF_JSON_STRING_POLICY_PRINTABLE_UTF8},
        {"s", FOF_JSON_STRING, FOF_JSON_STRING_POLICY_PRINTABLE_UTF8},
    };
    uint8_t nested_at_limit[512];
    uint8_t nested_over_limit[512];
    size_t at_limit_len = build_nested_object_frame(
        nested_at_limit, sizeof(nested_at_limit), 31U);
    size_t over_limit_len = build_nested_object_frame(
        nested_over_limit, sizeof(nested_over_limit), 32U);
    fof_json_member_spec_t members[65];
    char names[65][8];
    uint8_t many_member_frame[1024];
    size_t sixty_four_len = build_many_member_frame(
        many_member_frame, sizeof(many_member_frame), members, names, 64U);

    TEST_ASSERT_GREATER_THAN_UINT(0U, at_limit_len);
    TEST_ASSERT_GREATER_THAN_UINT(0U, over_limit_len);
    TEST_ASSERT_GREATER_THAN_UINT(0U, sixty_four_len);

    TEST_ASSERT_EQUAL_INT(
        FOF_JSON_SCHEMA_MALFORMED,
        fof_json_validate_exact_object(NULL, 0U, NULL, 0U));
    TEST_ASSERT_EQUAL_INT(
        FOF_JSON_SCHEMA_MALFORMED,
        fof_json_validate_exact_object(
            empty_object, sizeof(empty_object) - 1U, NULL, 1U));
    TEST_ASSERT_EQUAL_INT(
        FOF_JSON_SCHEMA_MALFORMED,
        fof_json_validate_exact_object(
            empty_object, sizeof(empty_object) - 1U,
            null_name, ARRAY_SIZE(null_name)));
    TEST_ASSERT_EQUAL_INT(
        FOF_JSON_SCHEMA_MALFORMED,
        fof_json_validate_exact_object(
            empty_object, sizeof(empty_object) - 1U,
            empty_name, ARRAY_SIZE(empty_name)));
    TEST_ASSERT_EQUAL_INT(
        FOF_JSON_SCHEMA_MALFORMED,
        fof_json_validate_exact_object(
            empty_object, sizeof(empty_object) - 1U,
            invalid_type, ARRAY_SIZE(invalid_type)));
    TEST_ASSERT_EQUAL_INT(
        FOF_JSON_SCHEMA_MALFORMED,
        fof_json_validate_exact_object(
            empty_object, sizeof(empty_object) - 1U,
            duplicate_specs, ARRAY_SIZE(duplicate_specs)));

    TEST_ASSERT_EQUAL_INT(
        FOF_JSON_SCHEMA_OK,
        validate_exact(nested_at_limit, at_limit_len));
    TEST_ASSERT_EQUAL_INT(
        FOF_JSON_SCHEMA_MALFORMED,
        validate_exact(nested_over_limit, over_limit_len));
    TEST_ASSERT_EQUAL_INT(
        FOF_JSON_SCHEMA_OK,
        fof_json_validate_exact_object(
            many_member_frame, sixty_four_len, members, 64U));

    size_t sixty_five_len = build_many_member_frame(
        many_member_frame, sizeof(many_member_frame), members, names, 65U);
    TEST_ASSERT_GREATER_THAN_UINT(0U, sixty_five_len);
    TEST_ASSERT_EQUAL_INT(
        FOF_JSON_SCHEMA_MALFORMED,
        fof_json_validate_exact_object(
            many_member_frame, sixty_five_len, members, 65U));
}

static void assert_selector_success(const char *label,
                                    const uint8_t *bytes,
                                    size_t byte_len,
                                    const char *expected)
{
    char value[32] = "stale";
    size_t value_len = 99U;

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        FOF_JSON_SCHEMA_OK,
        fof_json_extract_unique_ascii_token_member(
            bytes, byte_len, "cmd", value, sizeof(value), &value_len),
        label);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(strlen(expected), value_len, label);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, value, label);
}

void test_firmware_json_selector_extracts_unique_top_level_token_in_any_position(
    void)
{
    static const uint8_t first[] =
        "{\"cmd\":\"fw_relay\",\"other\":true,"
        "\"nested\":{\"cmd\":\"decoy\"}}";
    static const uint8_t middle[] =
        "{\"before\":[],\"cmd\":\"fw_relay\",\"after\":{}}";
    static const uint8_t last[] =
        "{\"nested\":{\"cmd\":\"decoy\"},\"other\":null,"
        "\"cmd\":\"fw_relay\"}";

    assert_selector_success(
        "selector first", first, sizeof(first) - 1U, "fw_relay");
    assert_selector_success(
        "selector middle", middle, sizeof(middle) - 1U, "fw_relay");
    assert_selector_success(
        "selector last", last, sizeof(last) - 1U, "fw_relay");
}

static void assert_selector_failure_clears_output(
    const char *label,
    const uint8_t *bytes,
    size_t byte_len,
    char *value,
    size_t value_capacity,
    size_t *value_len)
{
    fof_json_schema_result_t result =
        fof_json_extract_unique_ascii_token_member(
            bytes, byte_len, "cmd",
            value, value_capacity, value_len);

    TEST_ASSERT_NOT_EQUAL_MESSAGE(FOF_JSON_SCHEMA_OK, result, label);
    if (value && value_capacity > 0U) {
        TEST_ASSERT_EQUAL_CHAR_MESSAGE('\0', value[0], label);
    }
    if (value_len) {
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, *value_len, label);
    }
}

void test_firmware_json_selector_rejects_duplicate_escaped_and_invalid_values(
    void)
{
    static const uint8_t duplicate_valid_then_invalid[] =
        "{\"cmd\":\"fw_relay\",\"cmd\":false}";
    static const uint8_t duplicate_invalid_then_valid[] =
        "{\"cmd\":false,\"cmd\":\"fw_relay\"}";
    static const uint8_t escaped_name[] =
        "{\"c\\u006dd\":\"fw_relay\"}";
    static const uint8_t escaped_value[] =
        "{\"cmd\":\"fw\\u005frelay\"}";
    static const uint8_t missing[] =
        "{\"nested\":{\"cmd\":\"fw_relay\"}}";
    static const uint8_t empty[] = "{\"cmd\":\"\"}";
    static const uint8_t non_ascii[] =
        "{\"cmd\":\"fw_\xE2\x82\xAC\"}";
    static const uint8_t whitespace[] =
        "{\"cmd\":\"two words\"}";
    static const uint8_t boolean_value[] = "{\"cmd\":true}";
    static const uint8_t null_value[] = "{\"cmd\":null}";
    static const uint8_t number_value[] = "{\"cmd\":1}";
    static const uint8_t object_value[] = "{\"cmd\":{}}";
    static const uint8_t array_value[] = "{\"cmd\":[]}";
    static const struct {
        const char *label;
        const uint8_t *bytes;
        size_t byte_len;
    } cases[] = {
        {"duplicate valid then invalid",
         duplicate_valid_then_invalid,
         sizeof(duplicate_valid_then_invalid) - 1U},
        {"duplicate invalid then valid",
         duplicate_invalid_then_valid,
         sizeof(duplicate_invalid_then_valid) - 1U},
        {"escaped selector name", escaped_name, sizeof(escaped_name) - 1U},
        {"escaped selector value", escaped_value, sizeof(escaped_value) - 1U},
        {"nested selector only", missing, sizeof(missing) - 1U},
        {"empty selector", empty, sizeof(empty) - 1U},
        {"non-ascii selector", non_ascii, sizeof(non_ascii) - 1U},
        {"selector token whitespace", whitespace, sizeof(whitespace) - 1U},
        {"boolean selector", boolean_value, sizeof(boolean_value) - 1U},
        {"null selector", null_value, sizeof(null_value) - 1U},
        {"number selector", number_value, sizeof(number_value) - 1U},
        {"object selector", object_value, sizeof(object_value) - 1U},
        {"array selector", array_value, sizeof(array_value) - 1U},
    };

    for (size_t i = 0U; i < ARRAY_SIZE(cases); ++i) {
        char value[32] = "stale";
        size_t value_len = 99U;
        assert_selector_failure_clears_output(
            cases[i].label, cases[i].bytes, cases[i].byte_len,
            value, sizeof(value), &value_len);
    }
}

void test_firmware_json_selector_rejects_length_syntax_and_capacity_failures(
    void)
{
    static const uint8_t embedded_nul[] =
        "{\"cmd\":\"fw_relay\"}\0{\"cmd\":\"other\"}";
    static const uint8_t trailing_object[] =
        "{\"cmd\":\"fw_relay\"}{}";
    static const uint8_t trailing_data[] =
        "{\"cmd\":\"fw_relay\"}junk";
    static const uint8_t malformed_root[] =
        "{\"cmd\":\"fw_relay\"";
    static const uint8_t valid[] = "{\"cmd\":\"fw_relay\"}";
    static const struct {
        const char *label;
        const uint8_t *bytes;
        size_t byte_len;
    } cases[] = {
        {"embedded nul", embedded_nul, sizeof(embedded_nul) - 1U},
        {"trailing object", trailing_object, sizeof(trailing_object) - 1U},
        {"trailing data", trailing_data, sizeof(trailing_data) - 1U},
        {"malformed root", malformed_root, sizeof(malformed_root) - 1U},
    };

    for (size_t i = 0U; i < ARRAY_SIZE(cases); ++i) {
        char value[32] = "stale";
        size_t value_len = 99U;
        assert_selector_failure_clears_output(
            cases[i].label, cases[i].bytes, cases[i].byte_len,
            value, sizeof(value), &value_len);
    }

    char short_value[8] = "stale";
    size_t short_len = 99U;
    assert_selector_failure_clears_output(
        "capacity must include terminator",
        valid, sizeof(valid) - 1U,
        short_value, sizeof(short_value), &short_len);

    size_t invalid_len = 99U;
    assert_selector_failure_clears_output(
        "null output",
        valid, sizeof(valid) - 1U,
        NULL, 0U, &invalid_len);

    char invalid_value[32] = "stale";
    assert_selector_failure_clears_output(
        "null length output",
        valid, sizeof(valid) - 1U,
        invalid_value, sizeof(invalid_value), NULL);
}

void test_firmware_json_schema_capture_returns_raw_values_in_schema_order(void)
{
    static const fof_json_member_spec_t members[] = {
        {"cmd", FOF_JSON_STRING,
         FOF_JSON_STRING_POLICY_ASCII_TOKEN_NO_ESCAPE},
        {"policy", FOF_JSON_OBJECT, FOF_JSON_STRING_POLICY_NONE},
        {"ttl_s", FOF_JSON_INT32, FOF_JSON_STRING_POLICY_NONE},
    };
    static const uint8_t json[] =
        "{\"ttl_s\":-7,\"policy\":{\"enabled\":true},"
        "\"cmd\":\"network\"}";
    fof_json_value_span_t values[ARRAY_SIZE(members)];

    TEST_ASSERT_EQUAL(
        FOF_JSON_SCHEMA_OK,
        fof_json_validate_exact_object_capture(
            json, sizeof(json) - 1U, members, ARRAY_SIZE(members),
            values, ARRAY_SIZE(values)));
    TEST_ASSERT_EQUAL_UINT(sizeof("\"network\"") - 1U, values[0].byte_len);
    TEST_ASSERT_EQUAL_MEMORY(
        "\"network\"", values[0].bytes, values[0].byte_len);
    TEST_ASSERT_EQUAL_UINT(
        sizeof("{\"enabled\":true}") - 1U, values[1].byte_len);
    TEST_ASSERT_EQUAL_MEMORY(
        "{\"enabled\":true}", values[1].bytes, values[1].byte_len);
    TEST_ASSERT_EQUAL_UINT(sizeof("-7") - 1U, values[2].byte_len);
    TEST_ASSERT_EQUAL_MEMORY("-7", values[2].bytes, values[2].byte_len);
}

void test_firmware_json_schema_capture_clears_all_outputs_on_failure(void)
{
    static const fof_json_member_spec_t members[] = {
        {"cmd", FOF_JSON_STRING,
         FOF_JSON_STRING_POLICY_ASCII_TOKEN_NO_ESCAPE},
        {"enabled", FOF_JSON_BOOL, FOF_JSON_STRING_POLICY_NONE},
    };
    static const uint8_t invalid[] =
        "{\"cmd\":\"safe_mode\",\"enabled\":true,\"extra\":0}";
    fof_json_value_span_t values[ARRAY_SIZE(members)] = {
        {(const uint8_t *)"stale", 5U},
        {(const uint8_t *)"stale", 5U},
    };

    TEST_ASSERT_EQUAL(
        FOF_JSON_SCHEMA_UNKNOWN,
        fof_json_validate_exact_object_capture(
            invalid, sizeof(invalid) - 1U,
            members, ARRAY_SIZE(members),
            values, ARRAY_SIZE(values)));
    for (size_t i = 0U; i < ARRAY_SIZE(values); ++i) {
        TEST_ASSERT_NULL(values[i].bytes);
        TEST_ASSERT_EQUAL_UINT(0U, values[i].byte_len);
    }

    values[0].bytes = (const uint8_t *)"stale";
    values[0].byte_len = 5U;
    TEST_ASSERT_EQUAL(
        FOF_JSON_SCHEMA_MALFORMED,
        fof_json_validate_exact_object_capture(
            (const uint8_t *)"{\"cmd\":\"safe_mode\",\"enabled\":true}",
            sizeof("{\"cmd\":\"safe_mode\",\"enabled\":true}") - 1U,
            members, ARRAY_SIZE(members), values, 1U));
    TEST_ASSERT_EQUAL_PTR((const uint8_t *)"stale", values[0].bytes);
    TEST_ASSERT_EQUAL_UINT(5U, values[0].byte_len);
}

void test_firmware_json_schema_capture_rejects_unbounded_capacity_without_write(
    void)
{
    static const fof_json_member_spec_t members[] = {
        {"cmd", FOF_JSON_STRING,
         FOF_JSON_STRING_POLICY_ASCII_TOKEN_NO_ESCAPE},
    };
    static const uint8_t json[] = "{\"cmd\":\"status\"}";
    fof_json_value_span_t tiny = {
        (const uint8_t *)"canary", 6U};

    TEST_ASSERT_EQUAL(
        FOF_JSON_SCHEMA_MALFORMED,
        fof_json_validate_exact_object_capture(
            json, sizeof(json) - 1U,
            members, ARRAY_SIZE(members), &tiny, SIZE_MAX));
    TEST_ASSERT_EQUAL_PTR((const uint8_t *)"canary", tiny.bytes);
    TEST_ASSERT_EQUAL_UINT(6U, tiny.byte_len);

    TEST_ASSERT_EQUAL(
        FOF_JSON_SCHEMA_MALFORMED,
        fof_json_validate_exact_object_capture(
            json, sizeof(json) - 1U,
            members, ARRAY_SIZE(members), &tiny, 65U));
    TEST_ASSERT_EQUAL_PTR((const uint8_t *)"canary", tiny.bytes);
    TEST_ASSERT_EQUAL_UINT(6U, tiny.byte_len);
}

void test_firmware_json_value_span_parsers_enforce_scalar_lexical_boundaries(
    void)
{
    bool bool_value = false;
    int32_t int32_value = 0;
    uint32_t uint32_value = 0;
    int64_t int64_value = 0;

    const fof_json_value_span_t true_span = {
        (const uint8_t *)"true", sizeof("true") - 1U};
    const fof_json_value_span_t false_span = {
        (const uint8_t *)"false", sizeof("false") - 1U};
    TEST_ASSERT_TRUE(
        fof_json_value_span_parse_bool(&true_span, &bool_value));
    TEST_ASSERT_TRUE(bool_value);
    TEST_ASSERT_TRUE(
        fof_json_value_span_parse_bool(&false_span, &bool_value));
    TEST_ASSERT_FALSE(bool_value);

    const fof_json_value_span_t int32_min = {
        (const uint8_t *)"-2147483648", sizeof("-2147483648") - 1U};
    const fof_json_value_span_t int32_max = {
        (const uint8_t *)"2147483647", sizeof("2147483647") - 1U};
    TEST_ASSERT_TRUE(
        fof_json_value_span_parse_int32(&int32_min, &int32_value));
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, int32_value);
    TEST_ASSERT_TRUE(
        fof_json_value_span_parse_int32(&int32_max, &int32_value));
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, int32_value);

    const fof_json_value_span_t uint32_max = {
        (const uint8_t *)"4294967295", sizeof("4294967295") - 1U};
    TEST_ASSERT_TRUE(
        fof_json_value_span_parse_uint32(&uint32_max, &uint32_value));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, uint32_value);

    const fof_json_value_span_t int64_min = {
        (const uint8_t *)"-9223372036854775808",
        sizeof("-9223372036854775808") - 1U};
    const fof_json_value_span_t int64_max = {
        (const uint8_t *)"9223372036854775807",
        sizeof("9223372036854775807") - 1U};
    TEST_ASSERT_TRUE(
        fof_json_value_span_parse_int64(&int64_min, &int64_value));
    TEST_ASSERT_EQUAL_INT64(INT64_MIN, int64_value);
    TEST_ASSERT_TRUE(
        fof_json_value_span_parse_int64(&int64_max, &int64_value));
    TEST_ASSERT_EQUAL_INT64(INT64_MAX, int64_value);
    const fof_json_value_span_t negative_zero = {
        (const uint8_t *)"-0", sizeof("-0") - 1U};
    TEST_ASSERT_TRUE(
        fof_json_value_span_parse_int64(&negative_zero, &int64_value));
    TEST_ASSERT_EQUAL_INT64(0, int64_value);

    static const char *const rejected[] = {
        "", " 1", "1 ", "+1", "01", "1.0", "1e0",
        "9223372036854775808", "-9223372036854775809",
    };
    for (size_t i = 0U; i < ARRAY_SIZE(rejected); ++i) {
        const fof_json_value_span_t span = {
            (const uint8_t *)rejected[i], strlen(rejected[i])};
        int64_value = 99;
        TEST_ASSERT_FALSE_MESSAGE(
            fof_json_value_span_parse_int64(&span, &int64_value),
            rejected[i]);
        TEST_ASSERT_EQUAL_INT64(0, int64_value);
    }
}

void test_firmware_json_value_span_parsers_return_unquoted_exact_tokens(void)
{
    const fof_json_value_span_t token = {
        (const uint8_t *)"\"AA:BB:CC:DD:EE:FF\"",
        sizeof("\"AA:BB:CC:DD:EE:FF\"") - 1U};
    fof_json_value_span_t parsed = {
        (const uint8_t *)"stale", 5U};
    bool is_null = true;

    TEST_ASSERT_TRUE(
        fof_json_value_span_parse_ascii_token(&token, &parsed));
    TEST_ASSERT_EQUAL_UINT(
        sizeof("AA:BB:CC:DD:EE:FF") - 1U, parsed.byte_len);
    TEST_ASSERT_EQUAL_MEMORY(
        "AA:BB:CC:DD:EE:FF", parsed.bytes, parsed.byte_len);
    TEST_ASSERT_TRUE(fof_json_value_span_parse_nullable_ascii_token(
        &token, &is_null, &parsed));
    TEST_ASSERT_FALSE(is_null);

    const fof_json_value_span_t null_span = {
        (const uint8_t *)"null", sizeof("null") - 1U};
    TEST_ASSERT_TRUE(fof_json_value_span_parse_nullable_ascii_token(
        &null_span, &is_null, &parsed));
    TEST_ASSERT_TRUE(is_null);
    TEST_ASSERT_NULL(parsed.bytes);
    TEST_ASSERT_EQUAL_UINT(0U, parsed.byte_len);

    static const char *const rejected[] = {
        "\"\"", "\"has space\"", "\"escaped\\u0020token\"",
        "\"line\\nfeed\"", "\"\xc3\xa9\"", "null", "true",
    };
    for (size_t i = 0U; i < ARRAY_SIZE(rejected); ++i) {
        const fof_json_value_span_t span = {
            (const uint8_t *)rejected[i], strlen(rejected[i])};
        parsed.bytes = (const uint8_t *)"stale";
        parsed.byte_len = 5U;
        TEST_ASSERT_FALSE_MESSAGE(
            fof_json_value_span_parse_ascii_token(&span, &parsed),
            rejected[i]);
        TEST_ASSERT_NULL(parsed.bytes);
        TEST_ASSERT_EQUAL_UINT(0U, parsed.byte_len);
    }
}

void test_firmware_json_schema_int64_wire_type_is_exact_and_bounded(void)
{
    static const fof_json_member_spec_t members[] = {
        {"ms", FOF_JSON_INT64, FOF_JSON_STRING_POLICY_NONE},
    };
    static const char *const accepted[] = {
        "{\"ms\":-9223372036854775808}",
        "{\"ms\":9223372036854775807}",
    };
    static const char *const rejected[] = {
        "{\"ms\":-9223372036854775809}",
        "{\"ms\":9223372036854775808}",
        "{\"ms\":1.0}",
        "{\"ms\":1e0}",
    };

    for (size_t i = 0U; i < ARRAY_SIZE(accepted); ++i) {
        TEST_ASSERT_EQUAL(
            FOF_JSON_SCHEMA_OK,
            fof_json_validate_exact_object(
                (const uint8_t *)accepted[i], strlen(accepted[i]),
                members, ARRAY_SIZE(members)));
    }
    for (size_t i = 0U; i < ARRAY_SIZE(rejected); ++i) {
        TEST_ASSERT_EQUAL(
            FOF_JSON_SCHEMA_WRONG_TYPE,
            fof_json_validate_exact_object(
                (const uint8_t *)rejected[i], strlen(rejected[i]),
                members, ARRAY_SIZE(members)));
    }
}

void test_firmware_json_schema_array_wire_type_is_exact_nested_and_bounded(
    void)
{
    static const fof_json_member_spec_t array_schema[] = {
        {"items", FOF_JSON_ARRAY, FOF_JSON_STRING_POLICY_NONE},
    };
    static const schema_case_t cases[] = {
        SCHEMA_CASE("empty array", "{\"items\":[]}",
                    FOF_JSON_SCHEMA_OK),
        SCHEMA_CASE(
            "nested array",
            "{\"items\":[null,true,-1.25e+2,\"text\","
            "{\"nested\":[1,2,3]},[[],{}]]}",
            FOF_JSON_SCHEMA_OK),
        SCHEMA_CASE("array as object", "{\"items\":{}}",
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("array as string", "{\"items\":\"[]\"}",
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("array as null", "{\"items\":null}",
                    FOF_JSON_SCHEMA_WRONG_TYPE),
        SCHEMA_CASE("truncated array", "{\"items\":[1,2}",
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("array trailing comma", "{\"items\":[1,]}",
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("array trailing scalar", "{\"items\":[] true}",
                    FOF_JSON_SCHEMA_MALFORMED),
        SCHEMA_CASE("root trailing data", "{\"items\":[]}junk",
                    FOF_JSON_SCHEMA_TRAILING_DATA),
    };

    for (size_t i = 0U; i < ARRAY_SIZE(cases); ++i) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            cases[i].expected,
            fof_json_validate_exact_object(
                cases[i].bytes,
                cases[i].byte_len,
                array_schema,
                ARRAY_SIZE(array_schema)),
            cases[i].label);
    }

    fof_json_value_span_t captured = {0};
    static const uint8_t capture[] =
        "{\"items\":[{\"inside\":true},[null,false]]}";
    TEST_ASSERT_EQUAL(
        FOF_JSON_SCHEMA_OK,
        fof_json_validate_exact_object_capture(
            capture, sizeof(capture) - 1U,
            array_schema, ARRAY_SIZE(array_schema),
            &captured, 1U));
    TEST_ASSERT_EQUAL_UINT(
        sizeof("[{\"inside\":true},[null,false]]") - 1U,
        captured.byte_len);
    TEST_ASSERT_EQUAL_MEMORY(
        "[{\"inside\":true},[null,false]]",
        captured.bytes,
        captured.byte_len);
}
