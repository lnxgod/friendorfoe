#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "detection_types.h"

bool fof_policy_ble_has_exact_uuid128_le(const uint8_t uuids[][16],
                                         uint8_t count,
                                         const char *uuid_token);
bool fof_policy_ble_svc_raw_contains_uuid(const char *svc_raw,
                                          const char *uuid_token);
bool fof_policy_scan_profile_allows_source(const char *scan_profile,
                                           uint8_t source);
