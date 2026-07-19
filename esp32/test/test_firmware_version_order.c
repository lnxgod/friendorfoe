#include "unity.h"
#include "firmware_version_order.h"

void test_firmware_version_orders_numeric_release_core(void)
{
    TEST_ASSERT_EQUAL(FOF_VERSION_NEWER,
        fof_firmware_version_compare("0.64.69-badge-live-follow",
                                     "0.64.68-badge-live-follow"));
    TEST_ASSERT_EQUAL(FOF_VERSION_NEWER,
        fof_firmware_version_compare("0.65.0-badge-live-follow",
                                     "0.64.99-badge-live-follow"));
    TEST_ASSERT_EQUAL(FOF_VERSION_NEWER,
        fof_firmware_version_compare("1.0.0-badge-live-follow",
                                     "0.99.99-badge-live-follow"));
    TEST_ASSERT_EQUAL(FOF_VERSION_OLDER,
        fof_firmware_version_compare("0.64.9-badge-live-follow",
                                     "0.64.10-badge-live-follow"));
}

void test_firmware_version_accepts_optional_v_prefix(void)
{
    TEST_ASSERT_EQUAL(FOF_VERSION_EQUAL,
        fof_firmware_version_compare("v0.64.68-badge-live-follow",
                                     "0.64.68-badge-live-follow"));
    TEST_ASSERT_TRUE(fof_firmware_version_is_strictly_newer(
        "V0.64.69-badge-live-follow", "v0.64.68-badge-live-follow"));
}

void test_firmware_version_same_release_is_not_newer(void)
{
    TEST_ASSERT_EQUAL(FOF_VERSION_EQUAL,
        fof_firmware_version_compare("0.64.68-badge-live-follow",
                                     "0.64.68-badge-live-follow"));
    TEST_ASSERT_FALSE(fof_firmware_version_is_strictly_newer(
        "0.64.68-badge-live-follow", "0.64.68-badge-live-follow"));
}

void test_firmware_version_named_suffix_change_is_unordered(void)
{
    /* Project suffixes are release-track labels, not monotonic counters.
     * Never guess that a lexical rename is newer and accidentally downgrade. */
    TEST_ASSERT_EQUAL(FOF_VERSION_UNORDERED,
        fof_firmware_version_compare("0.64.68-badge-purple",
                                     "0.64.68-badge-live-follow"));
    TEST_ASSERT_FALSE(fof_firmware_version_is_strictly_newer(
        "0.64.68-badge-purple", "0.64.68-badge-live-follow"));
}

void test_firmware_version_rejects_malformed_or_unknown_values(void)
{
    const char *invalid[] = {
        NULL,
        "",
        "unknown",
        "0.64",
        "0.64.68.1",
        "0.64.x",
        " 0.64.68",
        "0.64.68 ",
        "4294967296.0.0",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        TEST_ASSERT_EQUAL(FOF_VERSION_INVALID,
            fof_firmware_version_compare(invalid[i],
                                         "0.64.68-badge-live-follow"));
        TEST_ASSERT_FALSE(fof_firmware_version_is_strictly_newer(
            invalid[i], "0.64.68-badge-live-follow"));
    }
    TEST_ASSERT_EQUAL(FOF_VERSION_INVALID,
        fof_firmware_version_compare("0.64.69-badge-live-follow", NULL));
}
