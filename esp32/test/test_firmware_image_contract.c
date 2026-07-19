#include "unity.h"
#include "firmware_image_contract.h"

#include <string.h>

static void put_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static void make_badge_scanner_prefix(uint8_t image[160])
{
    memset(image, 0, 160);
    image[0] = 0xE9;
    put_u32_le(image + 0x20, 0xABCD5432u);
    memcpy(image + 0x30, "0.64.69-badge-live-follow", 27);
    memcpy(image + 0x50, "fof_badge_scanner", 18);
}

void test_firmware_image_contract_parses_exact_app_descriptor(void)
{
    uint8_t image[160];
    make_badge_scanner_prefix(image);
    fof_firmware_image_identity_t identity = {0};

    TEST_ASSERT_TRUE(fof_firmware_image_parse_identity(
        image, sizeof(image), &identity));
    TEST_ASSERT_EQUAL_STRING("0.64.69-badge-live-follow", identity.version);
    TEST_ASSERT_EQUAL_STRING("fof_badge_scanner", identity.project);
}

void test_firmware_image_contract_rejects_invalid_or_unterminated_descriptor(void)
{
    uint8_t image[160];
    fof_firmware_image_identity_t identity = {0};
    make_badge_scanner_prefix(image);

    image[0] = 0;
    TEST_ASSERT_FALSE(fof_firmware_image_parse_identity(
        image, sizeof(image), &identity));

    make_badge_scanner_prefix(image);
    memset(image + 0x30, 'X', 32);
    TEST_ASSERT_FALSE(fof_firmware_image_parse_identity(
        image, sizeof(image), &identity));
}

void test_firmware_image_contract_validates_and_encodes_sha256(void)
{
    TEST_ASSERT_TRUE(fof_firmware_sha256_hex_is_valid(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
    TEST_ASSERT_TRUE(fof_firmware_sha256_hex_is_valid(
        "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"));
    TEST_ASSERT_FALSE(fof_firmware_sha256_hex_is_valid("abcd"));
    TEST_ASSERT_FALSE(fof_firmware_sha256_hex_is_valid(
        "g123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));

    uint8_t digest[FOF_FIRMWARE_SHA256_SIZE];
    for (size_t i = 0; i < sizeof(digest); ++i) {
        digest[i] = (uint8_t)i;
    }
    char hex[FOF_FIRMWARE_SHA256_HEX_SIZE];
    fof_firmware_sha256_to_hex(digest, hex);
    TEST_ASSERT_EQUAL_STRING(
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
        hex);
}
