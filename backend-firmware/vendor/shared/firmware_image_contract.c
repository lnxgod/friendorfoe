#include "firmware_image_contract.h"

#include <string.h>

#define ESP_IMAGE_MAGIC 0xE9u
#define ESP_APP_DESC_OFFSET 0x20u
#define ESP_APP_DESC_MIN_SIZE 112u
#define ESP_APP_DESC_MAGIC 0xABCD5432u
#define ESP_APP_DESC_VERSION_OFFSET (ESP_APP_DESC_OFFSET + 16u)
#define ESP_APP_DESC_PROJECT_OFFSET (ESP_APP_DESC_OFFSET + 48u)
#define ESP_APP_DESC_TEXT_SIZE 32u

static uint32_t read_u32_le(const uint8_t *src)
{
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static bool copy_descriptor_ascii(char out[33], const uint8_t *src)
{
    const uint8_t *terminator = memchr(src, 0, ESP_APP_DESC_TEXT_SIZE);
    if (!terminator || terminator == src) {
        return false;
    }
    size_t length = (size_t)(terminator - src);
    for (size_t i = 0; i < length; ++i) {
        if (src[i] <= 0x20u || src[i] > 0x7Eu) {
            return false;
        }
    }
    memcpy(out, src, length);
    out[length] = '\0';
    return true;
}

bool fof_firmware_image_parse_identity(
    const uint8_t *image_prefix,
    size_t image_prefix_len,
    fof_firmware_image_identity_t *out)
{
    if (!image_prefix || !out ||
        image_prefix_len < ESP_APP_DESC_OFFSET + ESP_APP_DESC_MIN_SIZE ||
        image_prefix[0] != ESP_IMAGE_MAGIC ||
        read_u32_le(image_prefix + ESP_APP_DESC_OFFSET) != ESP_APP_DESC_MAGIC) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    return copy_descriptor_ascii(
               out->version,
               image_prefix + ESP_APP_DESC_VERSION_OFFSET) &&
           copy_descriptor_ascii(
               out->project,
               image_prefix + ESP_APP_DESC_PROJECT_OFFSET);
}

bool fof_firmware_sha256_hex_is_valid(const char *hex)
{
    if (!hex) {
        return false;
    }
    for (size_t i = 0; i < FOF_FIRMWARE_SHA256_HEX_LENGTH; ++i) {
        char ch = hex[i];
        bool valid = (ch >= '0' && ch <= '9') ||
                     (ch >= 'a' && ch <= 'f') ||
                     (ch >= 'A' && ch <= 'F');
        if (!valid) {
            return false;
        }
    }
    return hex[FOF_FIRMWARE_SHA256_HEX_LENGTH] == '\0';
}

void fof_firmware_sha256_to_hex(
    const uint8_t digest[FOF_FIRMWARE_SHA256_SIZE],
    char out[FOF_FIRMWARE_SHA256_HEX_SIZE])
{
    static const char digits[] = "0123456789abcdef";
    if (!digest || !out) {
        return;
    }
    for (size_t i = 0; i < FOF_FIRMWARE_SHA256_SIZE; ++i) {
        out[i * 2] = digits[digest[i] >> 4];
        out[i * 2 + 1] = digits[digest[i] & 0x0Fu];
    }
    out[FOF_FIRMWARE_SHA256_HEX_LENGTH] = '\0';
}
