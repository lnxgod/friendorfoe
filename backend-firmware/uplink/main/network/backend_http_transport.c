#include "backend_http_transport.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "backend_http_policy.h"

#ifdef ESP_PLATFORM
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#endif

#define BACKEND_HTTP_MAX_BASE_URL 511U
#define BACKEND_HTTP_MAX_ENDPOINT 383U
#define BACKEND_HTTP_MAX_HOST 255U
#define BACKEND_HTTP_MAX_AUTHORITY 319U
#define BACKEND_HTTP_MAX_TARGET 767U
#define BACKEND_HTTP_MAX_REQUEST_HEADERS 1024U
#define BACKEND_HTTP_READ_BUFFER_SIZE 512U
#define BACKEND_HTTP_MAX_INTERIM_RESPONSES 8U

typedef struct {
    char host[BACKEND_HTTP_MAX_HOST + 1U];
    char authority[BACKEND_HTTP_MAX_AUTHORITY + 1U];
    char target[BACKEND_HTTP_MAX_TARGET + 1U];
    uint16_t port;
} backend_http_url_t;

typedef struct {
    bool has_content_length;
    bool is_chunked;
    size_t content_length;
    int status_code;
} backend_http_response_headers_t;

typedef struct {
    const backend_http_io_t *io;
    int socket_handle;
    int64_t started_ms;
    int64_t last_progress_ms;
    uint32_t total_timeout_ms;
    uint32_t no_progress_timeout_ms;
    uint8_t bytes[BACKEND_HTTP_READ_BUFFER_SIZE];
    size_t position;
    size_t length;
    bool eof;
    backend_http_error_t error;
} backend_http_reader_t;

typedef struct {
    const backend_http_io_t *io;
    int socket_handle;
    int64_t started_ms;
    int64_t last_progress_ms;
    uint32_t timeout_ms;
    uint32_t no_progress_timeout_ms;
    backend_http_error_t error;
} backend_http_sender_t;

typedef struct {
    char *json;
    size_t json_capacity;
    backend_http_body_sink_fn binary_sink;
    void *binary_sink_context;
    size_t binary_expected_length;
    size_t decoded_length;
    bool binary;
    bool discard;
} backend_http_body_target_t;

static backend_http_io_t s_installed_io;
static bool s_has_installed_io;

static backend_http_result_t result_with_error(backend_http_error_t error)
{
    backend_http_result_t result;
    memset(&result, 0, sizeof(result));
    result.error = error;
    return result;
}

bool backend_http_dns_caller_allowed(
    bool tcpip_initialized, bool caller_holds_lwip_core)
{
    return tcpip_initialized && !caller_holds_lwip_core;
}

void backend_http_dns_guard_pool_init(backend_http_dns_guard_pool_t *pool)
{
    if (pool) {
        memset(pool, 0, sizeof(*pool));
    }
}

int backend_http_dns_guard_reserve(
    backend_http_dns_guard_pool_t *pool, uint32_t *out_generation)
{
    if (!pool || !out_generation) {
        return -1;
    }
    for (size_t index = 0U;
         index < BACKEND_HTTP_DNS_GUARD_SLOTS;
         ++index) {
        backend_http_dns_guard_slot_t *slot = &pool->slots[index];
        if (slot->phase != BACKEND_HTTP_DNS_GUARD_FREE) {
            continue;
        }
        pool->next_generation++;
        if (pool->next_generation == 0U) {
            pool->next_generation++;
        }
        slot->generation = pool->next_generation;
        slot->phase = BACKEND_HTTP_DNS_GUARD_RESERVED;
        *out_generation = slot->generation;
        return (int)index;
    }
    return -1;
}

static bool dns_guard_transition(
    backend_http_dns_guard_pool_t *pool,
    size_t slot_index,
    uint32_t generation,
    backend_http_dns_guard_phase_t from,
    backend_http_dns_guard_phase_t to)
{
    if (!pool || slot_index >= BACKEND_HTTP_DNS_GUARD_SLOTS) {
        return false;
    }
    backend_http_dns_guard_slot_t *slot = &pool->slots[slot_index];
    if (slot->generation != generation || slot->phase != from) {
        return false;
    }
    slot->phase = to;
    return true;
}

bool backend_http_dns_guard_mark_pending(
    backend_http_dns_guard_pool_t *pool,
    size_t slot_index,
    uint32_t generation)
{
    return dns_guard_transition(
        pool,
        slot_index,
        generation,
        BACKEND_HTTP_DNS_GUARD_RESERVED,
        BACKEND_HTTP_DNS_GUARD_PENDING);
}

bool backend_http_dns_guard_begin_completion(
    backend_http_dns_guard_pool_t *pool,
    size_t slot_index,
    uint32_t generation)
{
    return dns_guard_transition(
        pool,
        slot_index,
        generation,
        BACKEND_HTTP_DNS_GUARD_PENDING,
        BACKEND_HTTP_DNS_GUARD_COMPLETING);
}

bool backend_http_dns_guard_publish_completion(
    backend_http_dns_guard_pool_t *pool,
    size_t slot_index,
    uint32_t generation)
{
    return dns_guard_transition(
        pool,
        slot_index,
        generation,
        BACKEND_HTTP_DNS_GUARD_COMPLETING,
        BACKEND_HTTP_DNS_GUARD_FREE);
}

bool backend_http_dns_guard_release_unscheduled(
    backend_http_dns_guard_pool_t *pool,
    size_t slot_index,
    uint32_t generation)
{
    if (!pool || slot_index >= BACKEND_HTTP_DNS_GUARD_SLOTS) {
        return false;
    }
    backend_http_dns_guard_slot_t *slot = &pool->slots[slot_index];
    if (slot->generation != generation ||
        (slot->phase != BACKEND_HTTP_DNS_GUARD_RESERVED &&
         slot->phase != BACKEND_HTTP_DNS_GUARD_PENDING)) {
        return false;
    }
    slot->phase = BACKEND_HTTP_DNS_GUARD_FREE;
    return true;
}

static bool bounded_c_string_length(
    const char *value, size_t maximum, size_t *out_length)
{
    if (!value || !out_length) {
        return false;
    }
    for (size_t length = 0U; length <= maximum; ++length) {
        if (value[length] == '\0') {
            *out_length = length;
            return true;
        }
    }
    return false;
}

static bool is_http_token_char(unsigned char character)
{
    if ((character >= '0' && character <= '9') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= 'a' && character <= 'z')) {
        return true;
    }
    switch (character) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
        return true;
    default:
        return false;
    }
}

static bool valid_url_character(unsigned char character)
{
    return character > 0x20U && character < 0x7FU &&
           character != '\\';
}

static bool parse_decimal_port(
    const char *bytes, size_t length, uint16_t *out_port)
{
    if (!bytes || !out_port || length == 0U || length > 5U) {
        return false;
    }
    uint32_t value = 0U;
    for (size_t index = 0U; index < length; ++index) {
        if (bytes[index] < '0' || bytes[index] > '9') {
            return false;
        }
        value = value * 10U + (uint32_t)(bytes[index] - '0');
    }
    if (value == 0U || value > UINT16_MAX) {
        return false;
    }
    *out_port = (uint16_t)value;
    return true;
}

static bool copy_component(
    char *out,
    size_t capacity,
    const char *bytes,
    size_t length)
{
    if (!out || !bytes || capacity == 0U || length >= capacity) {
        return false;
    }
    memcpy(out, bytes, length);
    out[length] = '\0';
    return true;
}

static bool parse_base_url(
    const char *base_url,
    const char *endpoint,
    backend_http_url_t *out)
{
    static const char prefix[] = "http://";
    size_t url_length = 0U;
    size_t endpoint_length = 0U;
    if (!out ||
        !bounded_c_string_length(
            base_url, BACKEND_HTTP_MAX_BASE_URL, &url_length) ||
        !bounded_c_string_length(
            endpoint, BACKEND_HTTP_MAX_ENDPOINT, &endpoint_length) ||
        url_length <= sizeof(prefix) - 1U || endpoint_length == 0U ||
        memcmp(base_url, prefix, sizeof(prefix) - 1U) != 0 ||
        endpoint[0] != '/') {
        return false;
    }
    for (size_t index = 0U; index < endpoint_length; ++index) {
        if (!valid_url_character((unsigned char)endpoint[index]) ||
            endpoint[index] == '#') {
            return false;
        }
    }

    const size_t authority_start = sizeof(prefix) - 1U;
    size_t authority_end = authority_start;
    while (authority_end < url_length && base_url[authority_end] != '/') {
        if (!valid_url_character((unsigned char)base_url[authority_end]) ||
            base_url[authority_end] == '?' ||
            base_url[authority_end] == '#' ||
            base_url[authority_end] == '@') {
            return false;
        }
        authority_end++;
    }
    const size_t authority_length = authority_end - authority_start;
    if (authority_length == 0U ||
        !copy_component(
            out->authority, sizeof(out->authority),
            base_url + authority_start, authority_length)) {
        return false;
    }

    size_t host_start = authority_start;
    size_t host_end = authority_end;
    out->port = 80U;
    if (base_url[host_start] == '[') {
        host_start++;
        host_end = host_start;
        while (host_end < authority_end && base_url[host_end] != ']') {
            host_end++;
        }
        if (host_end == host_start || host_end == authority_end) {
            return false;
        }
        const size_t after_bracket = host_end + 1U;
        if (after_bracket < authority_end) {
            if (base_url[after_bracket] != ':' ||
                !parse_decimal_port(
                    base_url + after_bracket + 1U,
                    authority_end - after_bracket - 1U,
                    &out->port)) {
                return false;
            }
        }
    } else {
        size_t colon = authority_end;
        for (size_t index = authority_start; index < authority_end; ++index) {
            if (base_url[index] == ':') {
                if (colon != authority_end) {
                    return false;
                }
                colon = index;
            }
        }
        if (colon != authority_end) {
            host_end = colon;
            if (!parse_decimal_port(
                    base_url + colon + 1U,
                    authority_end - colon - 1U,
                    &out->port)) {
                return false;
            }
        }
    }
    if (host_end <= host_start ||
        !copy_component(
            out->host, sizeof(out->host),
            base_url + host_start, host_end - host_start)) {
        return false;
    }

    size_t base_end = url_length;
    while (base_end > authority_end && base_url[base_end - 1U] == '/') {
        base_end--;
    }
    if (base_end > authority_end) {
        for (size_t index = authority_end; index < base_end; ++index) {
            if (!valid_url_character((unsigned char)base_url[index]) ||
                base_url[index] == '?' || base_url[index] == '#') {
                return false;
            }
        }
    }
    const size_t base_length = base_end - authority_end;
    if (base_length > BACKEND_HTTP_MAX_TARGET - endpoint_length) {
        return false;
    }
    if (base_length != 0U) {
        memcpy(out->target, base_url + authority_end, base_length);
    }
    memcpy(out->target + base_length, endpoint, endpoint_length);
    out->target[base_length + endpoint_length] = '\0';
    return true;
}

bool backend_http_install_io(const backend_http_io_t *io)
{
    if (!io || !io->resolve || !io->connect || !io->send ||
        !io->receive || !io->close || !io->monotonic_ms) {
        return false;
    }
    s_installed_io = *io;
    s_has_installed_io = true;
    return true;
}

void backend_http_reset_io(void)
{
    memset(&s_installed_io, 0, sizeof(s_installed_io));
    s_has_installed_io = false;
}

#ifdef ESP_PLATFORM
typedef struct {
    StaticSemaphore_t completion_storage;
    SemaphoreHandle_t completion;
    backend_http_resolved_address_t address;
    char host[BACKEND_HTTP_MAX_HOST + 1U];
    uint16_t port;
    size_t index;
    uint32_t request_generation;
    bool completed;
    bool success;
} backend_http_dns_slot_t;

typedef struct {
    portMUX_TYPE lock;
    backend_http_dns_slot_t slots[BACKEND_HTTP_DNS_GUARD_SLOTS];
    backend_http_dns_guard_pool_t guard_pool;
    bool initialized;
} backend_http_dns_state_t;

static backend_http_dns_state_t s_dns_state = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

/*
 * Callback arguments always point into this application-lifetime slot pool.
 * A timed-out slot remains pending until its one lwIP callback completes, so a
 * newer request can use another slot but can never reuse the late result's
 * storage. This relies on the transport's documented single-caller task.
 */

static bool platform_dns_initialize(void)
{
    if (s_dns_state.initialized) {
        return true;
    }
    backend_http_dns_guard_pool_init(&s_dns_state.guard_pool);
    for (size_t index = 0U;
         index < BACKEND_HTTP_DNS_GUARD_SLOTS;
         ++index) {
        s_dns_state.slots[index].index = index;
        s_dns_state.slots[index].completion =
            xSemaphoreCreateBinaryStatic(
                &s_dns_state.slots[index].completion_storage);
        if (!s_dns_state.slots[index].completion) {
            return false;
        }
    }
    s_dns_state.initialized = true;
    return true;
}

static bool platform_address_from_ip(
    const ip_addr_t *ip_address,
    uint16_t port,
    backend_http_resolved_address_t *out)
{
    if (!ip_address || !out) {
        return false;
    }
#if LWIP_IPV6
    if (IP_IS_V6(ip_address)) {
        struct sockaddr_in6 address;
        memset(&address, 0, sizeof(address));
        address.sin6_family = AF_INET6;
        address.sin6_port = htons(port);
        inet6_addr_from_ip6addr(
            &address.sin6_addr, ip_2_ip6(ip_address));
        if (sizeof(address) > sizeof(out->bytes)) {
            return false;
        }
        memcpy(out->bytes, &address, sizeof(address));
        out->length = sizeof(address);
        return true;
    }
#endif
#if LWIP_IPV4
    if (IP_IS_V4(ip_address)) {
        struct sockaddr_in address;
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        inet_addr_from_ip4addr(
            &address.sin_addr, ip_2_ip4(ip_address));
        if (sizeof(address) > sizeof(out->bytes)) {
            return false;
        }
        memcpy(out->bytes, &address, sizeof(address));
        out->length = sizeof(address);
        return true;
    }
#endif
    return false;
}

static void platform_dns_complete(
    backend_http_dns_slot_t *slot, const ip_addr_t *ip_address)
{
    if (!slot) {
        return;
    }
    backend_http_resolved_address_t resolved;
    memset(&resolved, 0, sizeof(resolved));
    const bool success = platform_address_from_ip(
        ip_address, slot->port, &resolved);
    const uint32_t generation = slot->request_generation;
    bool should_signal = false;

    portENTER_CRITICAL(&s_dns_state.lock);
    if (backend_http_dns_guard_begin_completion(
            &s_dns_state.guard_pool, slot->index, generation)) {
        slot->address = resolved;
        slot->success = success;
        slot->completed = true;
        should_signal = true;
    }
    portEXIT_CRITICAL(&s_dns_state.lock);

    if (!should_signal) {
        return;
    }
    (void)xSemaphoreGive(slot->completion);

    /* COMPLETING prevents reuse until the late signal has been published. */
    portENTER_CRITICAL(&s_dns_state.lock);
    (void)backend_http_dns_guard_publish_completion(
        &s_dns_state.guard_pool, slot->index, generation);
    portEXIT_CRITICAL(&s_dns_state.lock);
}

static void platform_dns_found(
    const char *name,
    const ip_addr_t *ip_address,
    void *callback_context)
{
    (void)name;
    platform_dns_complete(
        (backend_http_dns_slot_t *)callback_context, ip_address);
}

static void platform_dns_start(void *callback_context)
{
    backend_http_dns_slot_t *slot = callback_context;
    if (!slot) {
        return;
    }
    ip_addr_t immediate_address;
    const err_t result = dns_gethostbyname_addrtype(
        slot->host,
        &immediate_address,
        platform_dns_found,
        slot,
        LWIP_DNS_ADDRTYPE_DEFAULT);
    if (result == ERR_OK) {
        platform_dns_complete(slot, &immediate_address);
    } else if (result != ERR_INPROGRESS) {
        platform_dns_complete(slot, NULL);
    }
}

static backend_http_dns_slot_t *platform_dns_acquire_slot(
    const char *host, uint16_t port)
{
    size_t host_length = 0U;
    if (!host ||
        !bounded_c_string_length(
            host, BACKEND_HTTP_MAX_HOST, &host_length) ||
        host_length == 0U) {
        return NULL;
    }
    uint32_t generation = 0U;
    portENTER_CRITICAL(&s_dns_state.lock);
    const int slot_index = backend_http_dns_guard_reserve(
        &s_dns_state.guard_pool, &generation);
    portEXIT_CRITICAL(&s_dns_state.lock);
    if (slot_index < 0) {
        return NULL;
    }
    backend_http_dns_slot_t *selected =
        &s_dns_state.slots[(size_t)slot_index];

    while (xSemaphoreTake(selected->completion, 0U) == pdTRUE) {
    }
    memcpy(selected->host, host, host_length);
    selected->host[host_length] = '\0';
    selected->port = port;
    selected->request_generation = generation;
    memset(&selected->address, 0, sizeof(selected->address));

    portENTER_CRITICAL(&s_dns_state.lock);
    if (backend_http_dns_guard_mark_pending(
            &s_dns_state.guard_pool,
            selected->index,
            generation)) {
        selected->completed = false;
        selected->success = false;
    } else {
        selected = NULL;
    }
    portEXIT_CRITICAL(&s_dns_state.lock);
    return selected;
}

static void platform_dns_release_unscheduled(
    backend_http_dns_slot_t *slot)
{
    if (!slot) {
        return;
    }
    portENTER_CRITICAL(&s_dns_state.lock);
    if (backend_http_dns_guard_release_unscheduled(
            &s_dns_state.guard_pool,
            slot->index,
            slot->request_generation)) {
        slot->completed = false;
        slot->success = false;
    }
    portEXIT_CRITICAL(&s_dns_state.lock);
}

static bool platform_dns_wait(
    backend_http_dns_slot_t *slot,
    int64_t deadline_us,
    backend_http_resolved_address_t *out)
{
    if (!slot || !out) {
        return false;
    }
    const int64_t tick_us =
        (int64_t)portTICK_PERIOD_MS * INT64_C(1000);
    const uint32_t expected_generation = slot->request_generation;

    bool signaled = false;
    while (!signaled) {
        const int64_t now_us = esp_timer_get_time();
        if (now_us >= deadline_us) {
            return false;
        }
        const int64_t remaining_us = deadline_us - now_us;
        TickType_t wait_ticks = 0U;
        if (tick_us > 0 && remaining_us >= tick_us) {
            const uint64_t ticks =
                (uint64_t)remaining_us / (uint64_t)tick_us;
            wait_ticks = ticks > (uint64_t)portMAX_DELAY
                ? portMAX_DELAY : (TickType_t)ticks;
        }
        signaled =
            xSemaphoreTake(slot->completion, wait_ticks) == pdTRUE;
        if (!signaled && wait_ticks == 0U) {
            taskYIELD();
        }
    }
    if (esp_timer_get_time() >= deadline_us) {
        return false;
    }
    portENTER_CRITICAL(&s_dns_state.lock);
    const backend_http_dns_guard_slot_t *guard =
        &s_dns_state.guard_pool.slots[slot->index];
    const bool success =
        guard->generation == expected_generation &&
        slot->completed && slot->success;
    if (success) {
        *out = slot->address;
    }
    portEXIT_CRITICAL(&s_dns_state.lock);
    return success;
}

static bool platform_resolve(
    void *context,
    const char *host,
    uint16_t port,
    uint32_t timeout_ms,
    backend_http_resolved_address_t *out)
{
    (void)context;
    if (!host || !out || timeout_ms == 0U) {
        return false;
    }
    const int64_t started_us = esp_timer_get_time();
    const int64_t duration_us = (int64_t)timeout_ms * INT64_C(1000);
    const int64_t deadline_us =
        started_us > INT64_MAX - duration_us
            ? INT64_MAX : started_us + duration_us;
    ip_addr_t numeric_address;
    if (ipaddr_aton(host, &numeric_address)) {
        const bool success = platform_address_from_ip(
            &numeric_address, port, out);
        return esp_timer_get_time() < deadline_us && success;
    }
    const bool tcpip_initialized =
        sys_thread_tcpip(LWIP_CORE_IS_TCPIP_INITIALIZED);
    const bool caller_holds_lwip_core =
        sys_thread_tcpip(LWIP_CORE_LOCK_QUERY_HOLDER);
    if (!backend_http_dns_caller_allowed(
            tcpip_initialized, caller_holds_lwip_core)) {
        return false;
    }
    if (!platform_dns_initialize()) {
        return false;
    }
    backend_http_dns_slot_t *slot =
        platform_dns_acquire_slot(host, port);
    if (!slot) {
        return false;
    }
    if (esp_timer_get_time() >= deadline_us) {
        platform_dns_release_unscheduled(slot);
        return false;
    }
    if (tcpip_try_callback(platform_dns_start, slot) != ERR_OK) {
        platform_dns_release_unscheduled(slot);
        return false;
    }
    return platform_dns_wait(slot, deadline_us, out);
}

static int platform_connect(
    void *context,
    const backend_http_resolved_address_t *address,
    uint32_t timeout_ms)
{
    (void)context;
    if (!address || address->length < sizeof(sa_family_t) ||
        address->length > sizeof(struct sockaddr_storage)) {
        return BACKEND_HTTP_INVALID_SOCKET;
    }
    struct sockaddr_storage socket_address;
    memset(&socket_address, 0, sizeof(socket_address));
    memcpy(&socket_address, address->bytes, address->length);
    const int socket_handle = socket(socket_address.ss_family, SOCK_STREAM, 0);
    if (socket_handle < 0) {
        return BACKEND_HTTP_INVALID_SOCKET;
    }

    const int original_flags = fcntl(socket_handle, F_GETFL, 0);
    if (original_flags < 0 ||
        fcntl(socket_handle, F_SETFL, original_flags | O_NONBLOCK) < 0) {
        close(socket_handle);
        return BACKEND_HTTP_INVALID_SOCKET;
    }
    const int connected = connect(
        socket_handle,
        (const struct sockaddr *)&socket_address,
        (socklen_t)address->length);
    if (connected != 0 && errno != EINPROGRESS) {
        close(socket_handle);
        return BACKEND_HTTP_INVALID_SOCKET;
    }
    if (connected != 0) {
        fd_set writes;
        FD_ZERO(&writes);
        FD_SET(socket_handle, &writes);
        struct timeval timeout = {
            .tv_sec = (time_t)(timeout_ms / 1000U),
            .tv_usec = (suseconds_t)((timeout_ms % 1000U) * 1000U),
        };
        const int selected = select(
            socket_handle + 1, NULL, &writes, NULL, &timeout);
        if (selected == 0) {
            close(socket_handle);
            return BACKEND_HTTP_CONNECT_TIMEOUT;
        }
        int socket_error = 0;
        socklen_t error_length = sizeof(socket_error);
        if (selected < 0 ||
            getsockopt(
                socket_handle, SOL_SOCKET, SO_ERROR,
                &socket_error, &error_length) != 0 ||
            socket_error != 0) {
            close(socket_handle);
            return BACKEND_HTTP_INVALID_SOCKET;
        }
    }
    if (fcntl(socket_handle, F_SETFL, original_flags) < 0) {
        close(socket_handle);
        return BACKEND_HTTP_INVALID_SOCKET;
    }
    return socket_handle;
}

static ssize_t platform_send(
    void *context,
    int socket_handle,
    const void *bytes,
    size_t length,
    uint32_t timeout_ms)
{
    (void)context;
    const struct timeval timeout = {
        .tv_sec = (time_t)(timeout_ms / 1000U),
        .tv_usec = (suseconds_t)((timeout_ms % 1000U) * 1000U),
    };
    if (setsockopt(
            socket_handle, SOL_SOCKET, SO_SNDTIMEO,
            &timeout, sizeof(timeout)) != 0) {
        return -1;
    }
    const ssize_t sent = send(socket_handle, bytes, length, 0);
    if (sent < 0 &&
        (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)) {
        return BACKEND_HTTP_IO_TIMEOUT;
    }
    return sent;
}

static ssize_t platform_receive(
    void *context,
    int socket_handle,
    void *bytes,
    size_t capacity,
    uint32_t timeout_ms)
{
    (void)context;
    struct timeval timeout = {
        .tv_sec = (time_t)(timeout_ms / 1000U),
        .tv_usec = (suseconds_t)((timeout_ms % 1000U) * 1000U),
    };
    if (setsockopt(
            socket_handle, SOL_SOCKET, SO_RCVTIMEO,
            &timeout, sizeof(timeout)) != 0) {
        return -1;
    }
    const ssize_t received = recv(socket_handle, bytes, capacity, 0);
    if (received < 0 &&
        (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)) {
        return BACKEND_HTTP_IO_TIMEOUT;
    }
    return received;
}

static void platform_close(void *context, int socket_handle)
{
    (void)context;
    close(socket_handle);
}

static int64_t platform_monotonic_ms(void *context)
{
    (void)context;
    return esp_timer_get_time() / INT64_C(1000);
}

static backend_http_io_t platform_io(void)
{
    const backend_http_io_t io = {
        .context = NULL,
        .resolve = platform_resolve,
        .connect = platform_connect,
        .send = platform_send,
        .receive = platform_receive,
        .close = platform_close,
        .monotonic_ms = platform_monotonic_ms,
    };
    return io;
}
#endif

static bool select_io(backend_http_io_t *out)
{
    if (!out) {
        return false;
    }
    if (s_has_installed_io) {
        *out = s_installed_io;
        return true;
    }
#ifdef ESP_PLATFORM
    *out = platform_io();
    return true;
#else
    memset(out, 0, sizeof(*out));
    return false;
#endif
}

static bool elapsed_at_least(
    int64_t now_ms, int64_t started_ms, uint32_t duration_ms)
{
    if (now_ms < started_ms) {
        return true;
    }
    const uint64_t elapsed =
        (uint64_t)now_ms - (uint64_t)started_ms;
    return elapsed >= (uint64_t)duration_ms;
}

static uint32_t remaining_ms(
    int64_t now_ms, int64_t started_ms, uint32_t duration_ms)
{
    if (elapsed_at_least(now_ms, started_ms, duration_ms)) {
        return 0U;
    }
    const uint64_t elapsed =
        (uint64_t)now_ms - (uint64_t)started_ms;
    return duration_ms - (uint32_t)elapsed;
}

static bool operation_deadline_expired(
    int64_t now_ms,
    int64_t started_ms,
    int64_t last_progress_ms,
    uint32_t total_timeout_ms,
    uint32_t no_progress_timeout_ms)
{
    return elapsed_at_least(now_ms, started_ms, total_timeout_ms) ||
           (no_progress_timeout_ms != 0U &&
            elapsed_at_least(
                now_ms, last_progress_ms, no_progress_timeout_ms));
}

static uint32_t operation_wait_ms(
    int64_t now_ms,
    int64_t started_ms,
    int64_t last_progress_ms,
    uint32_t total_timeout_ms,
    uint32_t no_progress_timeout_ms)
{
    uint32_t wait_ms = remaining_ms(
        now_ms, started_ms, total_timeout_ms);
    if (no_progress_timeout_ms != 0U) {
        const uint32_t progress_wait = remaining_ms(
            now_ms, last_progress_ms, no_progress_timeout_ms);
        if (progress_wait < wait_ms) {
            wait_ms = progress_wait;
        }
    }
    return wait_ms;
}

static bool reader_deadline_expired(
    backend_http_reader_t *reader, int64_t now_ms)
{
    if (operation_deadline_expired(
            now_ms,
            reader->started_ms,
            reader->last_progress_ms,
            reader->total_timeout_ms,
            reader->no_progress_timeout_ms)) {
        reader->error = BACKEND_HTTP_ERROR_TIMEOUT;
        return true;
    }
    return false;
}

static uint32_t reader_wait_ms(
    const backend_http_reader_t *reader, int64_t now_ms)
{
    return operation_wait_ms(
        now_ms,
        reader->started_ms,
        reader->last_progress_ms,
        reader->total_timeout_ms,
        reader->no_progress_timeout_ms);
}

static bool reader_fill(backend_http_reader_t *reader)
{
    if (reader->error != BACKEND_HTTP_ERROR_NONE || reader->eof) {
        return false;
    }
    const int64_t before_ms =
        reader->io->monotonic_ms(reader->io->context);
    if (reader_deadline_expired(reader, before_ms)) {
        return false;
    }
    const uint32_t wait_ms = reader_wait_ms(reader, before_ms);
    const ssize_t received = reader->io->receive(
        reader->io->context,
        reader->socket_handle,
        reader->bytes,
        sizeof(reader->bytes),
        wait_ms);
    const int64_t after_ms =
        reader->io->monotonic_ms(reader->io->context);
    if (reader_deadline_expired(reader, after_ms)) {
        return false;
    }
    if (received == BACKEND_HTTP_IO_TIMEOUT) {
        reader->error = BACKEND_HTTP_ERROR_TIMEOUT;
        return false;
    }
    if (received < 0 || (size_t)received > sizeof(reader->bytes)) {
        reader->error = BACKEND_HTTP_ERROR_FRAMING;
        return false;
    }
    reader->position = 0U;
    reader->length = (size_t)received;
    if (received == 0) {
        reader->eof = true;
        return false;
    }
    reader->last_progress_ms = after_ms;
    return true;
}

static bool reader_byte(backend_http_reader_t *reader, uint8_t *out)
{
    if (!out) {
        reader->error = BACKEND_HTTP_ERROR_FRAMING;
        return false;
    }
    if (reader->position == reader->length && !reader_fill(reader)) {
        return false;
    }
    *out = reader->bytes[reader->position++];
    return true;
}

static bool reader_copy(
    backend_http_reader_t *reader, uint8_t *out, size_t length)
{
    size_t copied = 0U;
    while (copied < length) {
        if (reader->position == reader->length && !reader_fill(reader)) {
            return false;
        }
        size_t amount = reader->length - reader->position;
        if (amount > length - copied) {
            amount = length - copied;
        }
        memcpy(out + copied, reader->bytes + reader->position, amount);
        reader->position += amount;
        copied += amount;
    }
    return true;
}

static bool reader_require_eof(backend_http_reader_t *reader)
{
    if (reader->position != reader->length) {
        reader->error = BACKEND_HTTP_ERROR_FRAMING;
        return false;
    }
    if (reader_fill(reader)) {
        reader->error = BACKEND_HTTP_ERROR_FRAMING;
        return false;
    }
    return reader->eof && reader->error == BACKEND_HTTP_ERROR_NONE;
}

static bool ascii_equal_case_insensitive(
    const char *left, size_t left_length, const char *right)
{
    const size_t right_length = strlen(right);
    if (left_length != right_length) {
        return false;
    }
    for (size_t index = 0U; index < left_length; ++index) {
        if (tolower((unsigned char)left[index]) !=
            tolower((unsigned char)right[index])) {
            return false;
        }
    }
    return true;
}

static bool parse_status_line(
    const char *line, size_t length, int *out_status_code)
{
    if (!line || !out_status_code || length < 12U ||
        memcmp(line, "HTTP/1.", 7U) != 0 ||
        (line[7] != '0' && line[7] != '1') || line[8] != ' ' ||
        line[9] < '1' || line[9] > '5' ||
        line[10] < '0' || line[10] > '9' ||
        line[11] < '0' || line[11] > '9' ||
        (length > 12U && line[12] != ' ')) {
        return false;
    }
    for (size_t index = 12U; index < length; ++index) {
        const unsigned char character = (unsigned char)line[index];
        if (character < 0x20U || character == 0x7FU) {
            return false;
        }
    }
    *out_status_code =
        (line[9] - '0') * 100 +
        (line[10] - '0') * 10 +
        (line[11] - '0');
    return true;
}

static bool parse_content_length(
    const char *value, size_t length, size_t *out)
{
    if (!value || !out || length == 0U) {
        return false;
    }
    size_t parsed = 0U;
    for (size_t index = 0U; index < length; ++index) {
        if (value[index] < '0' || value[index] > '9') {
            return false;
        }
        const size_t digit = (size_t)(value[index] - '0');
        if (parsed > (SIZE_MAX - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    *out = parsed;
    return true;
}

static bool parse_headers(
    const char *headers,
    size_t length,
    backend_http_response_headers_t *out)
{
    if (!headers || !out || length < 4U ||
        memcmp(headers + length - 4U, "\r\n\r\n", 4U) != 0) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    size_t line_end = 0U;
    while (line_end + 1U < length &&
           !(headers[line_end] == '\r' && headers[line_end + 1U] == '\n')) {
        line_end++;
    }
    if (line_end + 1U >= length ||
        !parse_status_line(headers, line_end, &out->status_code)) {
        return false;
    }

    size_t position = line_end + 2U;
    while (position + 2U <= length) {
        line_end = position;
        while (line_end + 1U < length &&
               !(headers[line_end] == '\r' &&
                 headers[line_end + 1U] == '\n')) {
            line_end++;
        }
        if (line_end + 1U >= length) {
            return false;
        }
        if (line_end == position) {
            position += 2U;
            break;
        }
        size_t colon = position;
        while (colon < line_end && headers[colon] != ':') {
            if (!is_http_token_char((unsigned char)headers[colon])) {
                return false;
            }
            colon++;
        }
        if (colon == position || colon == line_end) {
            return false;
        }
        size_t value_start = colon + 1U;
        while (value_start < line_end &&
               (headers[value_start] == ' ' || headers[value_start] == '\t')) {
            value_start++;
        }
        size_t value_end = line_end;
        while (value_end > value_start &&
               (headers[value_end - 1U] == ' ' ||
                headers[value_end - 1U] == '\t')) {
            value_end--;
        }
        for (size_t index = value_start; index < value_end; ++index) {
            const unsigned char character = (unsigned char)headers[index];
            if ((character < 0x20U && character != '\t') ||
                character == 0x7FU) {
                return false;
            }
        }

        if (ascii_equal_case_insensitive(
                headers + position, colon - position, "Content-Length")) {
            if (out->has_content_length ||
                !parse_content_length(
                    headers + value_start,
                    value_end - value_start,
                    &out->content_length)) {
                return false;
            }
            out->has_content_length = true;
        } else if (ascii_equal_case_insensitive(
                       headers + position,
                       colon - position,
                       "Transfer-Encoding")) {
            if (out->is_chunked ||
                !ascii_equal_case_insensitive(
                    headers + value_start,
                    value_end - value_start,
                    "chunked")) {
                return false;
            }
            out->is_chunked = true;
        }
        position = line_end + 2U;
    }
    if (position != length ||
        (out->has_content_length && out->is_chunked)) {
        return false;
    }
    return true;
}

static bool read_headers(
    backend_http_reader_t *reader,
    backend_http_response_headers_t *out,
    size_t *out_header_length,
    size_t byte_budget)
{
    char headers[BACKEND_HTTP_MAX_RESPONSE_HEADERS + 1U];
    size_t length = 0U;
    bool complete = false;
    while (length < byte_budget) {
        uint8_t byte = 0U;
        if (!reader_byte(reader, &byte)) {
            if (reader->error == BACKEND_HTTP_ERROR_NONE) {
                reader->error = BACKEND_HTTP_ERROR_FRAMING;
            }
            return false;
        }
        headers[length++] = (char)byte;
        if (length >= 4U &&
            memcmp(headers + length - 4U, "\r\n\r\n", 4U) == 0) {
            complete = true;
            break;
        }
    }
    if (!complete) {
        reader->error = BACKEND_HTTP_ERROR_HEADERS_TOO_LARGE;
        return false;
    }
    headers[length] = '\0';
    if (!out_header_length || !parse_headers(headers, length, out)) {
        reader->error = BACKEND_HTTP_ERROR_FRAMING;
        return false;
    }
    *out_header_length = length;
    return true;
}

static bool target_would_exceed(
    const backend_http_body_target_t *target, size_t addition)
{
    if (addition > SIZE_MAX - target->decoded_length) {
        return true;
    }
    const size_t total = target->decoded_length + addition;
    if (target->discard) {
        return total > BACKEND_HTTP_MAX_JSON_BODY;
    }
    if (target->binary) {
        return total > target->binary_expected_length;
    }
    return total > BACKEND_HTTP_MAX_JSON_BODY ||
           total >= target->json_capacity;
}

static bool target_append(
    backend_http_reader_t *reader,
    backend_http_body_target_t *target,
    size_t length)
{
    if (target_would_exceed(target, length)) {
        reader->error = BACKEND_HTTP_ERROR_BODY_TOO_LARGE;
        return false;
    }
    uint8_t scratch[256];
    size_t consumed = 0U;
    while (consumed < length) {
        size_t amount = length - consumed;
        if (amount > sizeof(scratch)) {
            amount = sizeof(scratch);
        }
        if (!reader_copy(reader, scratch, amount)) {
            if (reader->error == BACKEND_HTTP_ERROR_NONE) {
                reader->error = BACKEND_HTTP_ERROR_FRAMING;
            }
            return false;
        }
        if (target->discard) {
            target->decoded_length += amount;
            consumed += amount;
            continue;
        }
        if (target->binary) {
            if (!target->binary_sink(
                    target->binary_sink_context, scratch, amount)) {
                reader->error = BACKEND_HTTP_ERROR_SINK;
                return false;
            }
        } else {
            memcpy(
                target->json + target->decoded_length,
                scratch,
                amount);
        }
        target->decoded_length += amount;
        consumed += amount;
    }
    return true;
}

static bool read_chunk_line(
    backend_http_reader_t *reader, char *line, size_t *out_length)
{
    size_t length = 0U;
    for (;;) {
        uint8_t byte = 0U;
        if (!reader_byte(reader, &byte)) {
            if (reader->error == BACKEND_HTTP_ERROR_NONE) {
                reader->error = BACKEND_HTTP_ERROR_FRAMING;
            }
            return false;
        }
        if (byte == '\r') {
            if (!reader_byte(reader, &byte) || byte != '\n') {
                if (reader->error == BACKEND_HTTP_ERROR_NONE) {
                    reader->error = BACKEND_HTTP_ERROR_FRAMING;
                }
                return false;
            }
            line[length] = '\0';
            *out_length = length;
            return true;
        }
        if (byte == '\n' ||
            (byte < 0x20U && byte != '\t') || byte == 0x7FU ||
            length == BACKEND_HTTP_MAX_CHUNK_LINE) {
            reader->error = BACKEND_HTTP_ERROR_FRAMING;
            return false;
        }
        line[length++] = (char)byte;
    }
    reader->error = BACKEND_HTTP_ERROR_FRAMING;
    return false;
}

static bool parse_chunk_size(
    const char *line, size_t length, size_t *out_size)
{
    if (!line || !out_size || length == 0U) {
        return false;
    }
    size_t position = 0U;
    size_t value = 0U;
    while (position < length) {
        unsigned digit = 0U;
        if (line[position] >= '0' && line[position] <= '9') {
            digit = (unsigned)(line[position] - '0');
        } else if (line[position] >= 'a' && line[position] <= 'f') {
            digit = (unsigned)(line[position] - 'a') + 10U;
        } else if (line[position] >= 'A' && line[position] <= 'F') {
            digit = (unsigned)(line[position] - 'A') + 10U;
        } else {
            break;
        }
        if (position >= sizeof(size_t) * 2U) {
            return false;
        }
        if (value > (SIZE_MAX - digit) / 16U) {
            return false;
        }
        value = value * 16U + digit;
        position++;
    }
    if (position == 0U) {
        return false;
    }

    while (position < length) {
        while (position < length &&
               (line[position] == ' ' || line[position] == '\t')) {
            position++;
        }
        if (position == length) {
            return false;
        }
        if (line[position++] != ';') {
            return false;
        }
        while (position < length &&
               (line[position] == ' ' || line[position] == '\t')) {
            position++;
        }
        const size_t name_start = position;
        while (position < length &&
               is_http_token_char((unsigned char)line[position])) {
            position++;
        }
        if (position == name_start) {
            return false;
        }
        if (position == length || line[position] == ';') {
            continue;
        }
        while (position < length &&
               (line[position] == ' ' || line[position] == '\t')) {
            position++;
        }
        if (position == length) {
            return false;
        }
        if (line[position] == ';') {
            continue;
        }
        if (line[position++] != '=') {
            return false;
        }
        while (position < length &&
               (line[position] == ' ' || line[position] == '\t')) {
            position++;
        }
        if (position == length) {
            return false;
        }
        if (line[position] == '"') {
            position++;
            bool closed = false;
            while (position < length) {
                const unsigned char character =
                    (unsigned char)line[position++];
                if (character == '"') {
                    closed = true;
                    break;
                }
                if (character == '\\') {
                    if (position == length) {
                        return false;
                    }
                    const unsigned char escaped =
                        (unsigned char)line[position++];
                    if (escaped != '\t' &&
                        (escaped < 0x20U || escaped == 0x7FU)) {
                        return false;
                    }
                } else if (character != '\t' &&
                           (character < 0x20U || character == 0x7FU)) {
                    return false;
                }
            }
            if (!closed ||
                (position < length && line[position] != ';' &&
                 line[position] != ' ' && line[position] != '\t')) {
                return false;
            }
        } else {
            const size_t value_start = position;
            while (position < length &&
                   is_http_token_char((unsigned char)line[position])) {
                position++;
            }
            if (position == value_start ||
                (position < length && line[position] != ';' &&
                 line[position] != ' ' && line[position] != '\t')) {
                return false;
            }
        }
    }
    *out_size = value;
    return true;
}

static bool read_trailer_line(
    backend_http_reader_t *reader,
    char *line,
    size_t *out_length,
    size_t initial_header_length,
    size_t *trailer_bytes)
{
    if (!line || !out_length || !trailer_bytes ||
        initial_header_length > BACKEND_HTTP_MAX_RESPONSE_HEADERS) {
        reader->error = BACKEND_HTTP_ERROR_FRAMING;
        return false;
    }
    size_t length = 0U;
    for (;;) {
        if (*trailer_bytes >=
            BACKEND_HTTP_MAX_RESPONSE_HEADERS - initial_header_length) {
            reader->error = BACKEND_HTTP_ERROR_HEADERS_TOO_LARGE;
            return false;
        }
        uint8_t byte = 0U;
        if (!reader_byte(reader, &byte)) {
            if (reader->error == BACKEND_HTTP_ERROR_NONE) {
                reader->error = BACKEND_HTTP_ERROR_FRAMING;
            }
            return false;
        }
        (*trailer_bytes)++;
        if (byte == '\r') {
            if (*trailer_bytes >=
                BACKEND_HTTP_MAX_RESPONSE_HEADERS - initial_header_length) {
                reader->error = BACKEND_HTTP_ERROR_HEADERS_TOO_LARGE;
                return false;
            }
            if (!reader_byte(reader, &byte) || byte != '\n') {
                if (reader->error == BACKEND_HTTP_ERROR_NONE) {
                    reader->error = BACKEND_HTTP_ERROR_FRAMING;
                }
                return false;
            }
            (*trailer_bytes)++;
            line[length] = '\0';
            *out_length = length;
            return true;
        }
        if (byte == '\n' || length >= BACKEND_HTTP_MAX_RESPONSE_HEADERS) {
            reader->error = BACKEND_HTTP_ERROR_FRAMING;
            return false;
        }
        line[length++] = (char)byte;
    }
}

static bool valid_trailer_field(const char *line, size_t length)
{
    if (!line || length == 0U) {
        return false;
    }
    size_t colon = 0U;
    while (colon < length && line[colon] != ':') {
        if (!is_http_token_char((unsigned char)line[colon])) {
            return false;
        }
        colon++;
    }
    if (colon == 0U || colon == length ||
        ascii_equal_case_insensitive(line, colon, "Content-Length") ||
        ascii_equal_case_insensitive(line, colon, "Transfer-Encoding")) {
        return false;
    }
    for (size_t index = colon + 1U; index < length; ++index) {
        const unsigned char character = (unsigned char)line[index];
        if ((character < 0x20U && character != '\t') ||
            character == 0x7FU) {
            return false;
        }
    }
    return true;
}

static bool read_trailers(
    backend_http_reader_t *reader, size_t initial_header_length)
{
    char line[BACKEND_HTTP_MAX_RESPONSE_HEADERS + 1U];
    size_t trailer_bytes = 0U;
    for (;;) {
        size_t line_length = 0U;
        if (!read_trailer_line(
                reader,
                line,
                &line_length,
                initial_header_length,
                &trailer_bytes)) {
            return false;
        }
        if (line_length == 0U) {
            return true;
        }
        if (!valid_trailer_field(line, line_length)) {
            reader->error = BACKEND_HTTP_ERROR_FRAMING;
            return false;
        }
    }
}

static bool read_chunked_body(
    backend_http_reader_t *reader,
    backend_http_body_target_t *target,
    size_t initial_header_length)
{
    for (;;) {
        char line[BACKEND_HTTP_MAX_CHUNK_LINE + 1U];
        size_t line_length = 0U;
        size_t chunk_size = 0U;
        if (!read_chunk_line(reader, line, &line_length) ||
            !parse_chunk_size(line, line_length, &chunk_size)) {
            if (reader->error == BACKEND_HTTP_ERROR_NONE) {
                reader->error = BACKEND_HTTP_ERROR_FRAMING;
            }
            return false;
        }
        if (chunk_size == 0U) {
            if (!read_trailers(reader, initial_header_length)) {
                return false;
            }
            break;
        }
        if (!target_append(reader, target, chunk_size)) {
            return false;
        }
        uint8_t carriage_return = 0U;
        uint8_t line_feed = 0U;
        if (!reader_byte(reader, &carriage_return) ||
            !reader_byte(reader, &line_feed) ||
            carriage_return != '\r' || line_feed != '\n') {
            if (reader->error == BACKEND_HTTP_ERROR_NONE) {
                reader->error = BACKEND_HTTP_ERROR_FRAMING;
            }
            return false;
        }
    }
    if (target->binary &&
        target->decoded_length != target->binary_expected_length) {
        reader->error = BACKEND_HTTP_ERROR_FRAMING;
        return false;
    }
    return reader_require_eof(reader);
}

static bool read_content_length_body(
    backend_http_reader_t *reader,
    backend_http_body_target_t *target,
    size_t content_length)
{
    if (target->binary && content_length != target->binary_expected_length) {
        reader->error = BACKEND_HTTP_ERROR_FRAMING;
        return false;
    }
    if (!target_append(reader, target, content_length)) {
        return false;
    }
    return reader_require_eof(reader);
}

static ssize_t send_with_deadline(
    void *context, const void *data, size_t length)
{
    backend_http_sender_t *sender = context;
    const int64_t before_ms =
        sender->io->monotonic_ms(sender->io->context);
    if (operation_deadline_expired(
            before_ms,
            sender->started_ms,
            sender->last_progress_ms,
            sender->timeout_ms,
            sender->no_progress_timeout_ms)) {
        sender->error = BACKEND_HTTP_ERROR_TIMEOUT;
        return -1;
    }
    const uint32_t wait_ms = operation_wait_ms(
        before_ms,
        sender->started_ms,
        sender->last_progress_ms,
        sender->timeout_ms,
        sender->no_progress_timeout_ms);
    const ssize_t sent = sender->io->send(
        sender->io->context,
        sender->socket_handle,
        data,
        length,
        wait_ms);
    const int64_t after_ms =
        sender->io->monotonic_ms(sender->io->context);
    if (operation_deadline_expired(
            after_ms,
            sender->started_ms,
            sender->last_progress_ms,
            sender->timeout_ms,
            sender->no_progress_timeout_ms) ||
        sent == BACKEND_HTTP_IO_TIMEOUT) {
        sender->error = BACKEND_HTTP_ERROR_TIMEOUT;
        return -1;
    }
    if (sent <= 0 || (size_t)sent > length) {
        sender->error = BACKEND_HTTP_ERROR_SEND;
        return -1;
    }
    sender->last_progress_ms = after_ms;
    return sent;
}

static bool send_bytes(
    backend_http_sender_t *sender, const void *bytes, size_t length)
{
    if (!backend_http_send_all(
            send_with_deadline,
            sender,
            (const uint8_t *)bytes,
            length)) {
        if (sender->error == BACKEND_HTTP_ERROR_NONE) {
            sender->error = BACKEND_HTTP_ERROR_SEND;
        }
        return false;
    }
    return true;
}

static bool format_request_headers(
    char *headers,
    size_t capacity,
    const char *method,
    const backend_http_url_t *url,
    bool has_json_body,
    size_t json_length,
    size_t *out_length)
{
    int written = 0;
    if (has_json_body) {
        written = snprintf(
            headers,
            capacity,
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Accept: application/json\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n\r\n",
            method,
            url->target,
            url->authority,
            json_length);
    } else {
        written = snprintf(
            headers,
            capacity,
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Accept: application/json\r\n"
            "Connection: close\r\n\r\n",
            method,
            url->target,
            url->authority);
    }
    if (written <= 0 || (size_t)written >= capacity) {
        return false;
    }
    *out_length = (size_t)written;
    return true;
}

static backend_http_result_t perform_request(
    const char *base_url,
    const char *endpoint,
    const char *method,
    const char *json,
    size_t json_length,
    backend_http_body_target_t *target,
    uint32_t total_timeout_ms,
    uint32_t no_progress_timeout_ms)
{
    if (target && !target->binary && target->json &&
        target->json_capacity != 0U) {
        target->json[0] = '\0';
    }
    backend_http_url_t url;
    memset(&url, 0, sizeof(url));
    if (!parse_base_url(base_url, endpoint, &url) ||
        !method || !target ||
        (json_length != 0U && !json) ||
        json_length > BACKEND_HTTP_MAX_JSON_BODY ||
        (!target->binary && (!target->json || target->json_capacity == 0U)) ||
        (target->binary && !target->binary_sink)) {
        return result_with_error(BACKEND_HTTP_ERROR_INVALID_URL);
    }
    char request_headers[BACKEND_HTTP_MAX_REQUEST_HEADERS];
    size_t request_header_length = 0U;
    const bool has_json_body = strcmp(method, "POST") == 0;
    if (!format_request_headers(
            request_headers,
            sizeof(request_headers),
            method,
            &url,
            has_json_body,
            json_length,
            &request_header_length)) {
        return result_with_error(BACKEND_HTTP_ERROR_INVALID_URL);
    }

    backend_http_io_t io;
    if (!select_io(&io)) {
        return result_with_error(BACKEND_HTTP_ERROR_DNS);
    }
    const int64_t started_ms = io.monotonic_ms(io.context);
    int64_t last_progress_ms = started_ms;
    backend_http_resolved_address_t address;
    memset(&address, 0, sizeof(address));
    uint32_t wait_ms = operation_wait_ms(
        started_ms,
        started_ms,
        last_progress_ms,
        total_timeout_ms,
        no_progress_timeout_ms);
    const bool resolved = io.resolve(
        io.context, url.host, url.port, wait_ms, &address);
    int64_t now_ms = io.monotonic_ms(io.context);
    if (operation_deadline_expired(
            now_ms,
            started_ms,
            last_progress_ms,
            total_timeout_ms,
            no_progress_timeout_ms)) {
        return result_with_error(BACKEND_HTTP_ERROR_TIMEOUT);
    }
    if (!resolved || address.length == 0U ||
        address.length > sizeof(address.bytes)) {
        return result_with_error(BACKEND_HTTP_ERROR_DNS);
    }
    last_progress_ms = now_ms;

    wait_ms = operation_wait_ms(
        now_ms,
        started_ms,
        last_progress_ms,
        total_timeout_ms,
        no_progress_timeout_ms);
    const int socket_handle = io.connect(io.context, &address, wait_ms);
    now_ms = io.monotonic_ms(io.context);
    if (socket_handle == BACKEND_HTTP_CONNECT_TIMEOUT ||
        operation_deadline_expired(
            now_ms,
            started_ms,
            last_progress_ms,
            total_timeout_ms,
            no_progress_timeout_ms)) {
        if (socket_handle >= 0) {
            io.close(io.context, socket_handle);
        }
        return result_with_error(BACKEND_HTTP_ERROR_TIMEOUT);
    }
    if (socket_handle < 0) {
        return result_with_error(BACKEND_HTTP_ERROR_CONNECT);
    }
    last_progress_ms = now_ms;

    backend_http_result_t result = result_with_error(BACKEND_HTTP_ERROR_NONE);
    backend_http_sender_t sender = {
        .io = &io,
        .socket_handle = socket_handle,
        .started_ms = started_ms,
        .last_progress_ms = last_progress_ms,
        .timeout_ms = total_timeout_ms,
        .no_progress_timeout_ms = no_progress_timeout_ms,
        .error = BACKEND_HTTP_ERROR_NONE,
    };
    if (!send_bytes(&sender, request_headers, request_header_length) ||
        (has_json_body && !send_bytes(&sender, json, json_length))) {
        result.error = sender.error;
        io.close(io.context, socket_handle);
        return result;
    }

    backend_http_reader_t reader;
    memset(&reader, 0, sizeof(reader));
    reader.io = &io;
    reader.socket_handle = socket_handle;
    reader.started_ms = started_ms;
    reader.last_progress_ms = sender.last_progress_ms;
    reader.total_timeout_ms = total_timeout_ms;
    reader.no_progress_timeout_ms = no_progress_timeout_ms;
    reader.error = BACKEND_HTTP_ERROR_NONE;

    backend_http_response_headers_t response_headers;
    backend_http_body_target_t discard_target;
    memset(&discard_target, 0, sizeof(discard_target));
    discard_target.discard = true;
    backend_http_body_target_t *response_target = target;
    size_t response_header_length = 0U;
    size_t total_header_length = 0U;
    size_t interim_count = 0U;
    bool complete = false;
    do {
        complete = read_headers(
            &reader,
            &response_headers,
            &response_header_length,
            BACKEND_HTTP_MAX_RESPONSE_HEADERS - total_header_length);
        if (!complete) {
            break;
        }
        total_header_length += response_header_length;
        if (response_headers.status_code == 101) {
            reader.error = BACKEND_HTTP_ERROR_FRAMING;
            complete = false;
            break;
        }
        if (response_headers.status_code < 100 ||
            response_headers.status_code > 199) {
            break;
        }
        if (response_headers.has_content_length ||
            response_headers.is_chunked) {
            reader.error = BACKEND_HTTP_ERROR_FRAMING;
            complete = false;
            break;
        }
        interim_count++;
        if (interim_count > BACKEND_HTTP_MAX_INTERIM_RESPONSES) {
            reader.error = BACKEND_HTTP_ERROR_FRAMING;
            complete = false;
            break;
        }
    } while (complete);
    if (complete) {
        result.status_code = response_headers.status_code;
        if (target->binary &&
            (result.status_code < 200 || result.status_code > 299)) {
            response_target = &discard_target;
        }
        if (response_headers.status_code == 204) {
            if (response_headers.has_content_length ||
                response_headers.is_chunked) {
                reader.error = BACKEND_HTTP_ERROR_FRAMING;
                complete = false;
            } else {
                complete = reader_require_eof(&reader);
            }
        } else if (response_headers.status_code == 304) {
            if (response_headers.is_chunked) {
                reader.error = BACKEND_HTTP_ERROR_FRAMING;
                complete = false;
            } else {
                complete = reader_require_eof(&reader);
            }
        } else if (response_headers.has_content_length) {
            complete = read_content_length_body(
                &reader, response_target, response_headers.content_length);
        } else if (response_headers.is_chunked) {
            complete = read_chunked_body(
                &reader, response_target, total_header_length);
        } else {
            reader.error = BACKEND_HTTP_ERROR_FRAMING;
            complete = false;
        }
    }
    if (complete) {
        if (!target->binary) {
            target->json[target->decoded_length] = '\0';
        }
        result.transport_complete = true;
        result.body_length = response_target->decoded_length;
        result.error = BACKEND_HTTP_ERROR_NONE;
    } else {
        if (!target->binary) {
            target->json[0] = '\0';
        }
        result.body_length = 0U;
        result.error = reader.error == BACKEND_HTTP_ERROR_NONE
            ? BACKEND_HTTP_ERROR_FRAMING : reader.error;
    }
    io.close(io.context, socket_handle);
    return result;
}

backend_http_result_t backend_http_get_json(
    const char *base_url,
    const char *endpoint,
    char *response_body,
    size_t response_capacity)
{
    backend_http_body_target_t target;
    memset(&target, 0, sizeof(target));
    target.json = response_body;
    target.json_capacity = response_capacity;
    return perform_request(
        base_url,
        endpoint,
        "GET",
        NULL,
        0U,
        &target,
        BACKEND_HTTP_JSON_TIMEOUT_MS,
        0U);
}

backend_http_result_t backend_http_post_json(
    const char *base_url,
    const char *endpoint,
    const char *json,
    size_t json_length,
    char *response_body,
    size_t response_capacity)
{
    backend_http_body_target_t target;
    memset(&target, 0, sizeof(target));
    target.json = response_body;
    target.json_capacity = response_capacity;
    return perform_request(
        base_url,
        endpoint,
        "POST",
        json,
        json_length,
        &target,
        BACKEND_HTTP_JSON_TIMEOUT_MS,
        0U);
}

backend_http_result_t backend_http_get_binary(
    const char *base_url,
    const char *endpoint,
    size_t expected_length,
    backend_http_body_sink_fn sink,
    void *sink_context)
{
    backend_http_body_target_t target;
    memset(&target, 0, sizeof(target));
    target.binary_sink = sink;
    target.binary_sink_context = sink_context;
    target.binary_expected_length = expected_length;
    target.binary = true;
    return perform_request(
        base_url,
        endpoint,
        "GET",
        NULL,
        0U,
        &target,
        BACKEND_HTTP_BINARY_TIMEOUT_MS,
        BACKEND_HTTP_BINARY_NO_PROGRESS_MS);
}
