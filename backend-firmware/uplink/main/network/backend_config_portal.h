#ifndef BACKEND_CONFIG_PORTAL_H
#define BACKEND_CONFIG_PORTAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_config.h"
#include "backend_dashboard_page.h"
#include "backend_portal_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_CONFIG_PORTAL_BACKEND_TEST_TIMEOUT_MS UINT32_C(5000)
#define BACKEND_CONFIG_PORTAL_IPV4 "192.168.4.1"
#define BACKEND_CONFIG_PORTAL_DEFAULT_PASSWORD "friendorfoe"
#define BACKEND_CONFIG_PORTAL_CHANNEL 1U
#define BACKEND_CONFIG_PORTAL_MAX_CLIENTS 4U
#define BACKEND_CONFIG_PORTAL_ROUTE_PATH_CAPACITY 96U

typedef bool (*backend_config_portal_commit_fn)(
    void *context, const backend_config_record_t *candidate);
typedef bool (*backend_config_portal_reconnect_fn)(
    void *context,
    const backend_config_record_t *committed,
    int64_t now_ms);
typedef bool (*backend_config_portal_begin_transaction_fn)(void *context);
typedef void (*backend_config_portal_end_transaction_fn)(void *context);
typedef bool (*backend_config_portal_get_fn)(
    void *context,
    const char *base_url,
    const char *path,
    uint32_t timeout_ms,
    int *status_code);
typedef bool (*backend_config_portal_dashboard_status_fn)(
    void *context,
    char *output,
    size_t capacity,
    size_t *out_length);
typedef bool (*backend_config_portal_event_snapshot_fn)(
    void *context,
    uint64_t after,
    size_t limit,
    backend_dashboard_event_t *events,
    size_t event_capacity,
    backend_event_ring_snapshot_t *snapshot);

typedef struct {
    void *context;
    backend_config_portal_commit_fn commit_config;
    backend_config_portal_reconnect_fn reconnect_wifi;
    backend_config_portal_begin_transaction_fn begin_config_transaction;
    backend_config_portal_end_transaction_fn end_config_transaction;
    backend_config_portal_get_fn backend_get;
    backend_config_portal_dashboard_status_fn dashboard_status;
    backend_config_portal_event_snapshot_fn event_snapshot;
} backend_config_portal_ops_t;

typedef struct {
    char ssid[33];
    char password[65];
    char ipv4[16];
    uint8_t channel;
    uint8_t max_clients;
} backend_config_portal_ap_config_t;

#ifdef UNIT_TESTING
typedef struct {
    void *context;
    bool (*set_ap_config)(
        void *context,
        const backend_config_portal_ap_config_t *config);
    bool (*start_ap)(void *context);
    bool (*start_http)(void *context);
    bool (*register_route)(
        void *context, const backend_portal_route_t *route);
    bool (*unregister_route)(
        void *context, const backend_portal_route_t *route);
    bool (*rollback)(void *context);
} backend_config_portal_test_platform_hooks_t;

void backend_config_portal_set_test_platform_hooks(
    const backend_config_portal_test_platform_hooks_t *hooks);
#endif

typedef struct {
    bool transport_complete;
    int status_code;
    bool healthy;
} backend_portal_backend_test_result_t;

typedef struct {
    backend_config_record_t config;
    backend_config_portal_ops_t ops;
    bool initialized;
    bool running;
    bool dashboard_routes_enabled;
    const char *dashboard_failure_reason;
    bool usb_start_requested;
    void *server;
    void *ap_netif;
} backend_config_portal_t;

bool backend_config_portal_init(
    backend_config_portal_t *portal,
    const backend_config_record_t *current,
    const backend_config_portal_ops_t *ops);
backend_portal_update_result_t backend_config_portal_apply_update(
    backend_config_portal_t *portal,
    const char *json,
    size_t length,
    int64_t now_ms);
bool backend_config_portal_snapshot_config(
    const backend_config_portal_t *portal,
    backend_config_record_t *out);
bool backend_config_portal_test_backend(
    backend_config_portal_t *portal,
    backend_portal_backend_test_result_t *out);
bool backend_config_portal_handle_usb_line(
    backend_config_portal_t *portal,
    const char *line,
    size_t length);
bool backend_config_portal_take_usb_start_request(
    backend_config_portal_t *portal);
bool backend_config_portal_build_ap_config(
    const backend_config_portal_t *portal,
    const uint8_t sta_mac[6],
    backend_config_portal_ap_config_t *out);
bool backend_config_portal_local_ipv4_allowed(const uint8_t address[4]);
bool backend_config_portal_route_from_uri(
    backend_portal_method_t method,
    const char *uri,
    backend_portal_route_id_t *out);
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
bool backend_config_portal_dashboard_status(
    backend_config_portal_t *portal,
    char *output,
    size_t capacity,
    size_t *out_length);
bool backend_config_portal_copy_dashboard_events(
    backend_config_portal_t *portal,
    backend_dashboard_query_t query,
    backend_dashboard_event_t events[BACKEND_DASHBOARD_MAX_LIMIT],
    backend_event_ring_snapshot_t *snapshot);
#endif
const char *backend_config_portal_update_response(
    backend_portal_update_result_t result, int *out_status_code);
bool backend_config_portal_start(
    backend_config_portal_t *portal,
    const uint8_t sta_mac[6]);
bool backend_config_portal_stop(backend_config_portal_t *portal);
bool backend_config_portal_is_running(
    const backend_config_portal_t *portal);
const char *backend_config_portal_html(void);

#ifdef __cplusplus
}
#endif

#endif
