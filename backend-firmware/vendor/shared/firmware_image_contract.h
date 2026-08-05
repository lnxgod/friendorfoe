#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FOF_FIRMWARE_SHA256_SIZE 32
#define FOF_FIRMWARE_SHA256_HEX_LENGTH 64
#define FOF_FIRMWARE_SHA256_HEX_SIZE (FOF_FIRMWARE_SHA256_HEX_LENGTH + 1)

typedef struct {
    char version[33];
    char project[33];
} fof_firmware_image_identity_t;

/** Parse the fixed ESP-IDF application descriptor from a firmware prefix. */
bool fof_firmware_image_parse_identity(
    const uint8_t *image_prefix,
    size_t image_prefix_len,
    fof_firmware_image_identity_t *out);

/** True only for exactly 64 hexadecimal SHA-256 characters. */
bool fof_firmware_sha256_hex_is_valid(const char *hex);

/** Encode a 32-byte SHA-256 digest as lowercase hexadecimal. */
void fof_firmware_sha256_to_hex(
    const uint8_t digest[FOF_FIRMWARE_SHA256_SIZE],
    char out[FOF_FIRMWARE_SHA256_HEX_SIZE]);

#ifdef __cplusplus
}
#endif
