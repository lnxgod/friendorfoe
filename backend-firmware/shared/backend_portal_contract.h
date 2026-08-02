#ifndef BACKEND_PORTAL_CONTRACT_H
#define BACKEND_PORTAL_CONTRACT_H

#include <stdbool.h>
#include <stddef.h>

#include "backend_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_PORTAL_CONFIG_BODY_MAX 4096U

typedef enum {
    BACKEND_PORTAL_GET = 0,
    BACKEND_PORTAL_POST,
} backend_portal_method_t;

typedef enum {
    BACKEND_PORTAL_ROOT = 0,
    BACKEND_PORTAL_STATUS,
    BACKEND_PORTAL_CONFIG_GET,
    BACKEND_PORTAL_CONFIG_POST,
    BACKEND_PORTAL_BACKEND_TEST,
} backend_portal_route_id_t;

typedef struct {
    backend_portal_method_t method;
    const char *path;
    backend_portal_route_id_t id;
} backend_portal_route_t;

typedef enum {
    BACKEND_PORTAL_UPDATE_OK = 0,
    BACKEND_PORTAL_UPDATE_INVALID_ARGUMENT,
    BACKEND_PORTAL_UPDATE_INVALID_JSON,
    BACKEND_PORTAL_UPDATE_UNKNOWN_FIELD,
    BACKEND_PORTAL_UPDATE_CONFIRMATION_REQUIRED,
    BACKEND_PORTAL_UPDATE_INVALID_CONFIG,
    BACKEND_PORTAL_UPDATE_COMMIT_FAILED,
    BACKEND_PORTAL_UPDATE_RECONNECT_FAILED,
} backend_portal_update_result_t;

const backend_portal_route_t *backend_portal_routes(size_t *out_count);
bool backend_portal_route_lookup(
    backend_portal_method_t method,
    const char *path,
    backend_portal_route_id_t *out);
size_t backend_portal_render_redacted_config(
    const backend_config_record_t *config,
    char *output,
    size_t capacity);
backend_portal_update_result_t backend_portal_parse_config_update(
    const backend_config_record_t *current,
    const char *json,
    size_t length,
    backend_config_record_t *out_candidate);

#ifdef __cplusplus
}
#endif

#endif
