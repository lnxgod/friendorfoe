#pragma once

/**
 * Friend or Foe -- WiFi SSID Pattern Matching
 *
 * Known drone SSID prefix patterns matched against beacon frames.
 * Ported from Android WifiDroneScanner.kt DRONE_SSID_PATTERNS table.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *prefix;
    const char *manufacturer;
} drone_ssid_pattern_t;

/**
 * Match an SSID against the known drone pattern table.
 * Comparison is case-insensitive prefix match.
 *
 * @param ssid Null-terminated SSID string
 * @return Pointer to matching pattern entry, or NULL if no match
 */
const drone_ssid_pattern_t *wifi_ssid_match(const char *ssid);

/**
 * Get the full pattern table for iteration.
 *
 * @param count Output: number of entries in the table
 * @return Pointer to the first entry in the static pattern array
 */
const drone_ssid_pattern_t *wifi_ssid_get_patterns(int *count);

/**
 * Soft-match an SSID against generic drone-like patterns.
 * Catches cheap/generic drone hotspots (e.g. "WIFI_9", "FPV_123", "4KCAM").
 * These are low-confidence (0.15) — too ambiguous for the hard match table.
 *
 * @param ssid Null-terminated SSID string
 * @return true if the SSID looks like a cheap drone hotspot
 */
bool wifi_ssid_match_soft(const char *ssid);

#ifdef __cplusplus
}
#endif
