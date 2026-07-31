#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FOF_MAC_CANONICAL_TEXT_SIZE 17U
#define FOF_MAC_CANONICAL_BUFFER_SIZE (FOF_MAC_CANONICAL_TEXT_SIZE + 1U)

/**
 * Normalize a complete MAC string into uppercase colon form.
 *
 * Accepted non-empty inputs are 12 compact hex digits, six colon- or
 * hyphen-separated octets, or Cisco-style xxxx.xxxx.xxxx. Whitespace,
 * mixed separators, partial forms, and trailing data are rejected.
 */
bool fof_mac_normalize(
    const char *input,
    bool allow_empty,
    char output[FOF_MAC_CANONICAL_BUFFER_SIZE]);

/** True only for uppercase colon form (or explicitly allowed empty input). */
bool fof_mac_is_canonical_upper(const char *input, bool allow_empty);

#ifdef __cplusplus
}
#endif
