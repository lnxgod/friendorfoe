#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BACKEND_HTTP_ACK = 0,
    BACKEND_HTTP_RETRY,
    BACKEND_HTTP_QUARANTINE,
} backend_http_disposition_t;

backend_http_disposition_t backend_http_classify(
    bool transport_complete,
    int status_code,
    bool exact_ack_valid);

typedef ssize_t (*backend_http_send_fn)(
    void *context,
    const void *data,
    size_t length);

bool backend_http_send_all(
    backend_http_send_fn send_fn,
    void *context,
    const uint8_t *data,
    size_t length);

uint32_t backend_retry_delay_ms(uint8_t exponent, uint32_t random_value);

#ifdef __cplusplus
}
#endif
