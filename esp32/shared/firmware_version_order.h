#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FOF_VERSION_INVALID = -2,
    FOF_VERSION_OLDER = -1,
    FOF_VERSION_EQUAL = 0,
    FOF_VERSION_NEWER = 1,
    FOF_VERSION_UNORDERED = 2,
} fof_firmware_version_relation_t;

/**
 * Compare Friend or Foe release versions without guessing from named labels.
 *
 * The numeric major.minor.patch core is ordered. An optional leading v/V is
 * ignored. When the numeric core is equal, only the exact same normalized
 * suffix is equal; different named suffixes are deliberately UNORDERED.
 */
fof_firmware_version_relation_t fof_firmware_version_compare(
    const char *candidate,
    const char *current);

bool fof_firmware_version_is_strictly_newer(const char *candidate,
                                            const char *current);

#ifdef __cplusplus
}
#endif
