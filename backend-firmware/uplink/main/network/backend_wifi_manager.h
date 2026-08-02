#ifndef BACKEND_WIFI_MANAGER_H
#define BACKEND_WIFI_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_WIFI_ATTEMPT_TIMEOUT_MS INT64_C(15000)
#define BACKEND_WIFI_RETRY_BASE_MS INT64_C(1000)
#define BACKEND_WIFI_RETRY_CAP_MS INT64_C(60000)

typedef enum {
    BACKEND_WIFI_EVENT_TICK = 0,
    BACKEND_WIFI_EVENT_CONNECTED,
    BACKEND_WIFI_EVENT_AUTH_FAILED,
    BACKEND_WIFI_EVENT_NO_AP,
    BACKEND_WIFI_EVENT_DISCONNECTED,
} backend_wifi_event_t;

typedef enum {
    BACKEND_WIFI_NO_CHANGE = 0,
    BACKEND_WIFI_CONNECT_NETWORK,
    BACKEND_WIFI_WAIT_RETRY,
} backend_wifi_action_t;

typedef struct {
    uint32_t config_generation;
    uint8_t network_count;
    uint8_t network_index;
    uint8_t retry_exponent;
    int64_t attempt_started_ms;
    int64_t retry_after_ms;
    bool connected;
} backend_wifi_policy_t;

void backend_wifi_policy_init(backend_wifi_policy_t *state);
backend_wifi_action_t backend_wifi_policy_update(
    backend_wifi_policy_t *state,
    const backend_config_record_t *config,
    backend_wifi_event_t event,
    int64_t now_ms);
size_t backend_wifi_policy_render_status(
    const backend_wifi_policy_t *state,
    char *output,
    size_t capacity);

typedef struct {
    backend_wifi_policy_t policy;
    backend_config_record_t config;
    bool initialized;
} backend_wifi_manager_t;

bool backend_wifi_manager_init(
    backend_wifi_manager_t *manager,
    const backend_config_record_t *config,
    int64_t now_ms);
bool backend_wifi_manager_handle_event(
    backend_wifi_manager_t *manager,
    backend_wifi_event_t event,
    int64_t now_ms);
bool backend_wifi_manager_apply_committed_config(
    backend_wifi_manager_t *manager,
    const backend_config_record_t *committed,
    int64_t now_ms);
const backend_wifi_network_t *backend_wifi_manager_active_network(
    const backend_wifi_manager_t *manager);

#ifdef __cplusplus
}
#endif

#endif
