#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "backend_identity.h"
#include "backend_version.h"
#include "../support/backend_test_main.h"

void setUp(void)
{
}

void tearDown(void)
{
}

void test_backend_crc32_matches_ieee_known_vector(void)
{
    static const char vector[] = "123456789";

    TEST_ASSERT_EQUAL_HEX32(
        UINT32_C(0xCBF43926),
        backend_identity_crc32(vector, sizeof(vector) - 1));
}

void test_backend_identities_are_exact_and_distinct(void)
{
    const backend_firmware_identity_t *uplink =
        backend_identity_for_image(BACKEND_IMAGE_UPLINK);
    const backend_firmware_identity_t *scanner =
        backend_identity_for_image(BACKEND_IMAGE_SCANNER);

    TEST_ASSERT_NOT_NULL(uplink);
    TEST_ASSERT_NOT_NULL(scanner);
    TEST_ASSERT_NOT_EQUAL(uplink, scanner);
    TEST_ASSERT_EQUAL_STRING("uplink-s3-backend", uplink->target);
    TEST_ASSERT_EQUAL_STRING("fof_backend_uplink", uplink->project);
    TEST_ASSERT_EQUAL_STRING("scanner-s3-combo-backend", scanner->target);
    TEST_ASSERT_EQUAL_STRING("fof_backend_scanner", scanner->project);
    TEST_ASSERT_EQUAL_STRING("seeed_xiao_esp32s3", uplink->hardware);
    TEST_ASSERT_EQUAL_STRING("0.1.0-backend", scanner->version);
    TEST_ASSERT_FALSE(backend_identity_matches(
        scanner, "scanner-s3-combo-fof_badge", "fof_badge_scanner",
        "seeed_xiao_esp32s3"));

    backend_embedded_identity_record_t record = {0};
    TEST_ASSERT_TRUE(backend_identity_record_build(
        BACKEND_IMAGE_SCANNER, &record));
    TEST_ASSERT_EQUAL_HEX32(FOF_BACKEND_IDENTITY_MAGIC, record.magic);
    TEST_ASSERT_EQUAL_UINT16(1, record.schema);
    TEST_ASSERT_EQUAL_STRING("fof_backend_scanner", record.project);
    TEST_ASSERT_TRUE(backend_identity_record_validate(&record));
}

void test_backend_identity_matching_requires_all_exact_non_null_values(void)
{
    const backend_firmware_identity_t *uplink =
        backend_identity_for_image(BACKEND_IMAGE_UPLINK);

    TEST_ASSERT_TRUE(backend_identity_matches(
        uplink, "uplink-s3-backend", "fof_backend_uplink",
        "seeed_xiao_esp32s3"));
    TEST_ASSERT_FALSE(backend_identity_matches(
        uplink, NULL, "fof_backend_uplink", "seeed_xiao_esp32s3"));
    TEST_ASSERT_FALSE(backend_identity_matches(
        uplink, "uplink-s3-backend", NULL, "seeed_xiao_esp32s3"));
    TEST_ASSERT_FALSE(backend_identity_matches(
        uplink, "uplink-s3-backend", "fof_backend_uplink", NULL));
    TEST_ASSERT_FALSE(backend_identity_matches(
        NULL, "uplink-s3-backend", "fof_backend_uplink",
        "seeed_xiao_esp32s3"));
    TEST_ASSERT_FALSE(backend_identity_matches(
        uplink, "uplink-s3-backend-extra", "fof_backend_uplink",
        "seeed_xiao_esp32s3"));
}

void test_backend_record_builds_each_kind_with_zero_filled_tails_and_exact_crc(void)
{
    backend_embedded_identity_record_t uplink;
    backend_embedded_identity_record_t scanner;

    memset(&uplink, 0xA5, sizeof(uplink));
    memset(&scanner, 0xA5, sizeof(scanner));
    TEST_ASSERT_TRUE(backend_identity_record_build(BACKEND_IMAGE_UPLINK, &uplink));
    TEST_ASSERT_TRUE(backend_identity_record_build(BACKEND_IMAGE_SCANNER, &scanner));

    TEST_ASSERT_EQUAL_UINT16(BACKEND_IMAGE_UPLINK, uplink.image_kind);
    TEST_ASSERT_EQUAL_STRING("uplink-s3-backend", uplink.target);
    TEST_ASSERT_EQUAL_STRING("fof_backend_uplink", uplink.project);
    TEST_ASSERT_EQUAL_STRING("seeed_xiao_esp32s3", uplink.hardware);
    TEST_ASSERT_EQUAL_STRING(FOF_VERSION_BACKEND, uplink.version);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0xF08BCDE4), uplink.crc32);
    TEST_ASSERT_EQUAL_UINT8(0, uplink.target[strlen(uplink.target) + 1]);
    TEST_ASSERT_EQUAL_UINT8(0, uplink.project[sizeof(uplink.project) - 1]);
    TEST_ASSERT_EQUAL_UINT8(0, uplink.hardware[sizeof(uplink.hardware) - 1]);
    TEST_ASSERT_EQUAL_UINT8(0, uplink.version[sizeof(uplink.version) - 1]);

    TEST_ASSERT_EQUAL_UINT16(BACKEND_IMAGE_SCANNER, scanner.image_kind);
    TEST_ASSERT_EQUAL_STRING("scanner-s3-combo-backend", scanner.target);
    TEST_ASSERT_EQUAL_STRING("fof_backend_scanner", scanner.project);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x9DD382FF), scanner.crc32);
    TEST_ASSERT_TRUE(backend_identity_record_validate(&uplink));
    TEST_ASSERT_TRUE(backend_identity_record_validate(&scanner));
}

void test_backend_record_validation_rejects_invalid_or_tampered_records(void)
{
    backend_embedded_identity_record_t record;

    TEST_ASSERT_NULL(backend_identity_for_image((backend_image_kind_t)2));
    TEST_ASSERT_FALSE(backend_identity_record_build(
        (backend_image_kind_t)2, &record));
    TEST_ASSERT_FALSE(backend_identity_record_build(BACKEND_IMAGE_UPLINK, NULL));
    TEST_ASSERT_FALSE(backend_identity_record_validate(NULL));

    TEST_ASSERT_TRUE(backend_identity_record_build(BACKEND_IMAGE_UPLINK, &record));
    record.target[0] = 'x';
    TEST_ASSERT_FALSE(backend_identity_record_validate(&record));

    TEST_ASSERT_TRUE(backend_identity_record_build(BACKEND_IMAGE_UPLINK, &record));
    record.crc32 ^= UINT32_C(1);
    TEST_ASSERT_FALSE(backend_identity_record_validate(&record));

    TEST_ASSERT_TRUE(backend_identity_record_build(BACKEND_IMAGE_UPLINK, &record));
    record.image_kind = UINT16_C(2);
    record.crc32 = backend_identity_crc32(&record, offsetof(
        backend_embedded_identity_record_t, crc32));
    TEST_ASSERT_FALSE(backend_identity_record_validate(&record));
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(test_backend_crc32_matches_ieee_known_vector);
    BACKEND_RUN_TEST(test_backend_identities_are_exact_and_distinct);
    BACKEND_RUN_TEST(
        test_backend_identity_matching_requires_all_exact_non_null_values);
    BACKEND_RUN_TEST(
        test_backend_record_builds_each_kind_with_zero_filled_tails_and_exact_crc);
    BACKEND_RUN_TEST(
        test_backend_record_validation_rejects_invalid_or_tampered_records);
    return UNITY_END();
}
