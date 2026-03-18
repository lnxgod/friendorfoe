#pragma once

/**
 * Friend or Foe -- WiFi OUI (Organizationally Unique Identifier) Database
 *
 * Lookup table mapping MAC address OUI prefixes (first 3 bytes) to known
 * drone hardware manufacturers. Even drones with hidden or generic SSIDs
 * still broadcast their hardware OUI in the BSSID.
 *
 * Ported from Android WifiOuiDatabase.kt.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *manufacturer;
    const char *full_name;
    bool high_false_positive;
} oui_entry_t;

/**
 * Look up manufacturer by BSSID string "AA:BB:CC:DD:EE:FF".
 *
 * @param bssid Null-terminated BSSID string in colon-separated hex format
 * @return Pointer to matching OUI entry, or NULL if not found
 */
const oui_entry_t *wifi_oui_lookup(const char *bssid);

/**
 * Look up manufacturer by raw 3-byte OUI prefix.
 *
 * @param oui Pointer to 3 bytes representing the OUI prefix
 * @return Pointer to matching OUI entry, or NULL if not found
 */
const oui_entry_t *wifi_oui_lookup_raw(const uint8_t oui[3]);

#ifdef __cplusplus
}
#endif
