#pragma once

/**
 * Friend or Foe -- Uplink WiFi SoftAP
 *
 * Configures the AP side of APSTA mode so a phone can connect directly
 * to the uplink board in the field.  Always-on, concurrent with STA.
 *
 * Default SSID: FoF-XXYYZZ (last 3 bytes of MAC), configurable via NVS.
 * Default password: "friendorfoe", configurable via NVS.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the SoftAP interface.
 * Must be called after wifi_sta_init() (which sets APSTA mode and
 * creates the AP netif).
 */
void wifi_ap_init(void);

/**
 * Get the number of stations currently connected to the AP.
 */
int wifi_ap_get_station_count(void);

/**
 * Get the AP SSID that was configured at init time.
 */
const char *wifi_ap_get_ssid(void);

#ifdef __cplusplus
}
#endif
