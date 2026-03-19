#pragma once

/**
 * Friend or Foe -- Embedded HTTP Status Page
 *
 * Serves a mobile-responsive status page at http://192.168.4.1 when a
 * phone connects to the uplink's WiFi AP.  Also provides a JSON API
 * at /api/status for future app integration.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the HTTP status server on port 80.
 * Must be called after all other subsystems are initialized.
 */
void http_status_init(void);

#ifdef __cplusplus
}
#endif
