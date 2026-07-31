#include "unity.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "mac_address_policy.h"

typedef struct {
    const char *input;
    const char *expected;
} mac_normalize_case_t;

void test_mac_address_policy_normalizes_supported_wire_forms(void)
{
    static const mac_normalize_case_t cases[] = {
        {"aa:bb:cc:dd:ee:ff", "AA:BB:CC:DD:EE:FF"},
        {"AA-Bb-cC-dD-eE-fF", "AA:BB:CC:DD:EE:FF"},
        {"aAbBcCdDeEfF", "AA:BB:CC:DD:EE:FF"},
        {"aAbB.cCdD.eEfF", "AA:BB:CC:DD:EE:FF"},
    };

    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char normalized[FOF_MAC_CANONICAL_BUFFER_SIZE] = "unchanged";
        TEST_ASSERT_TRUE_MESSAGE(
            fof_mac_normalize(cases[i].input, false, normalized),
            cases[i].input);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(
            cases[i].expected, normalized, cases[i].input);
    }
}

void test_mac_address_policy_empty_is_only_valid_when_explicitly_allowed(void)
{
    char normalized[FOF_MAC_CANONICAL_BUFFER_SIZE] = "unchanged";
    TEST_ASSERT_TRUE(fof_mac_normalize("", true, normalized));
    TEST_ASSERT_EQUAL_STRING("", normalized);
    TEST_ASSERT_TRUE(fof_mac_is_canonical_upper("", true));

    memcpy(normalized, "unchanged", sizeof("unchanged"));
    TEST_ASSERT_FALSE(fof_mac_normalize("", false, normalized));
    TEST_ASSERT_EQUAL_STRING("", normalized);
    TEST_ASSERT_FALSE(fof_mac_is_canonical_upper("", false));
}

void test_mac_address_policy_rejects_malformed_or_ambiguous_forms(void)
{
    static const char *const invalid[] = {
        "AA:BB:CC:DD:EE",
        "AA:BB:CC:DD:EE:FF:00",
        "AA:BB-CC:DD:EE:FF",
        "AA-BB.CCDD.EEFF",
        "AABB.CCDD:EEFF",
        "AABB.CCDD.EEF",
        "AABB.CCDD.EEFF0",
        "GG:BB:CC:DD:EE:FF",
        "AA::BB:CC:DD:EE:FF",
        " AA:BB:CC:DD:EE:FF",
        "AA:BB:CC:DD:EE:FF ",
        "AA:BB:CC:DD:\tEE:FF",
        "AA BB CC DD EE FF",
    };

    for (size_t i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        char normalized[FOF_MAC_CANONICAL_BUFFER_SIZE] = "unchanged";
        TEST_ASSERT_FALSE_MESSAGE(
            fof_mac_normalize(invalid[i], true, normalized),
            invalid[i]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("", normalized, invalid[i]);
    }
}

void test_mac_address_policy_canonical_check_requires_upper_colon_form(void)
{
    TEST_ASSERT_TRUE(
        fof_mac_is_canonical_upper("AA:BB:CC:DD:EE:FF", false));
    TEST_ASSERT_FALSE(
        fof_mac_is_canonical_upper("aa:bb:cc:dd:ee:ff", false));
    TEST_ASSERT_FALSE(
        fof_mac_is_canonical_upper("AA-BB-CC-DD-EE-FF", false));
    TEST_ASSERT_FALSE(
        fof_mac_is_canonical_upper("AABBCCDDEEFF", false));
    TEST_ASSERT_FALSE(
        fof_mac_is_canonical_upper("AABB.CCDD.EEFF", false));
}

void test_mac_address_policy_supports_in_place_normalization(void)
{
    char colon[FOF_MAC_CANONICAL_BUFFER_SIZE] = "aa:bb:cc:dd:ee:ff";
    char compact[FOF_MAC_CANONICAL_BUFFER_SIZE] = "aabbccddeeff";

    TEST_ASSERT_TRUE(fof_mac_normalize(colon, false, colon));
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", colon);

    TEST_ASSERT_TRUE(fof_mac_normalize(compact, false, compact));
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", compact);
}

void test_mac_address_policy_clears_output_on_invalid_arguments(void)
{
    char normalized[FOF_MAC_CANONICAL_BUFFER_SIZE] = "unchanged";

    TEST_ASSERT_FALSE(fof_mac_normalize(NULL, false, normalized));
    TEST_ASSERT_EQUAL_STRING("", normalized);

    TEST_ASSERT_FALSE(fof_mac_normalize(
        "AA:BB:CC:DD:EE:FF", false, NULL));
    TEST_ASSERT_FALSE(fof_mac_is_canonical_upper(NULL, false));
}
