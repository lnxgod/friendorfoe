#ifndef BACKEND_HTTP_TRANSPORT_H
#define BACKEND_HTTP_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "backend_upload_batch.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_HTTP_MAX_RESPONSE_HEADERS 2048U
#define BACKEND_HTTP_MAX_JSON_BODY BACKEND_UPLOAD_MAX_JSON
#define BACKEND_HTTP_MAX_CHUNK_LINE 256U
#define BACKEND_HTTP_JSON_TIMEOUT_MS 5000U
#define BACKEND_HTTP_BINARY_TIMEOUT_MS 60000U
#define BACKEND_HTTP_BINARY_NO_PROGRESS_MS 5000U
#define BACKEND_HTTP_INVALID_SOCKET (-1)
#define BACKEND_HTTP_CONNECT_TIMEOUT (-2)
#define BACKEND_HTTP_IO_TIMEOUT ((ssize_t)-2)
#define BACKEND_HTTP_RESOLVED_ADDRESS_CAPACITY 128U
#define BACKEND_HTTP_DNS_GUARD_SLOTS 4U

typedef enum {
    BACKEND_HTTP_DNS_GUARD_FREE = 0,
    BACKEND_HTTP_DNS_GUARD_RESERVED,
    BACKEND_HTTP_DNS_GUARD_PENDING,
    BACKEND_HTTP_DNS_GUARD_COMPLETING,
} backend_http_dns_guard_phase_t;

typedef struct {
    uint32_t generation;
    backend_http_dns_guard_phase_t phase;
} backend_http_dns_guard_slot_t;

typedef struct {
    backend_http_dns_guard_slot_t slots[BACKEND_HTTP_DNS_GUARD_SLOTS];
    uint32_t next_generation;
} backend_http_dns_guard_pool_t;

typedef bool (*backend_http_body_sink_fn)(
    void *context, const uint8_t *bytes, size_t length);

typedef enum {
    BACKEND_HTTP_ERROR_NONE = 0,
    BACKEND_HTTP_ERROR_INVALID_URL,
    BACKEND_HTTP_ERROR_DNS,
    BACKEND_HTTP_ERROR_CONNECT,
    BACKEND_HTTP_ERROR_TIMEOUT,
    BACKEND_HTTP_ERROR_SEND,
    BACKEND_HTTP_ERROR_HEADERS_TOO_LARGE,
    BACKEND_HTTP_ERROR_BODY_TOO_LARGE,
    BACKEND_HTTP_ERROR_FRAMING,
    BACKEND_HTTP_ERROR_SINK,
} backend_http_error_t;

typedef struct {
    bool transport_complete;
    int status_code;
    size_t body_length;
    backend_http_error_t error;
} backend_http_result_t;

typedef struct {
    uint8_t bytes[BACKEND_HTTP_RESOLVED_ADDRESS_CAPACITY];
    size_t length;
} backend_http_resolved_address_t;

typedef bool (*backend_http_resolve_fn)(
    void *context,
    const char *host,
    uint16_t port,
    uint32_t timeout_ms,
    backend_http_resolved_address_t *out);
typedef int (*backend_http_connect_fn)(
    void *context,
    const backend_http_resolved_address_t *address,
    uint32_t timeout_ms);
typedef ssize_t (*backend_http_socket_send_fn)(
    void *context,
    int socket_handle,
    const void *bytes,
    size_t length,
    uint32_t timeout_ms);
typedef ssize_t (*backend_http_socket_receive_fn)(
    void *context,
    int socket_handle,
    void *bytes,
    size_t capacity,
    uint32_t timeout_ms);
typedef void (*backend_http_socket_close_fn)(
    void *context, int socket_handle);
typedef int64_t (*backend_http_monotonic_ms_fn)(void *context);

typedef struct {
    void *context;
    backend_http_resolve_fn resolve;
    backend_http_connect_fn connect;
    backend_http_socket_send_fn send;
    backend_http_socket_receive_fn receive;
    backend_http_socket_close_fn close;
    backend_http_monotonic_ms_fn monotonic_ms;
} backend_http_io_t;

/* Internal DNS lifetime policy. Callers provide synchronization. */
bool backend_http_dns_caller_allowed(
    bool tcpip_initialized, bool caller_holds_lwip_core);
void backend_http_dns_guard_pool_init(backend_http_dns_guard_pool_t *pool);
int backend_http_dns_guard_reserve(
    backend_http_dns_guard_pool_t *pool, uint32_t *out_generation);
bool backend_http_dns_guard_mark_pending(
    backend_http_dns_guard_pool_t *pool,
    size_t slot_index,
    uint32_t generation);
bool backend_http_dns_guard_begin_completion(
    backend_http_dns_guard_pool_t *pool,
    size_t slot_index,
    uint32_t generation);
bool backend_http_dns_guard_publish_completion(
    backend_http_dns_guard_pool_t *pool,
    size_t slot_index,
    uint32_t generation);
bool backend_http_dns_guard_release_unscheduled(
    backend_http_dns_guard_pool_t *pool,
    size_t slot_index,
    uint32_t generation);

/* The uplink owns one transport task. Tests replace its socket boundary. */
bool backend_http_install_io(const backend_http_io_t *io);
void backend_http_reset_io(void);

backend_http_result_t backend_http_get_json(
    const char *base_url,
    const char *endpoint,
    char *response_body,
    size_t response_capacity);
backend_http_result_t backend_http_post_json(
    const char *base_url,
    const char *endpoint,
    const char *json,
    size_t json_length,
    char *response_body,
    size_t response_capacity);
backend_http_result_t backend_http_get_binary(
    const char *base_url,
    const char *endpoint,
    size_t expected_length,
    backend_http_body_sink_fn sink,
    void *sink_context);

#ifdef __cplusplus
}
#endif

#endif
