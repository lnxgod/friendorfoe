#include "backend_http_policy.h"

backend_http_disposition_t backend_http_classify(
    bool transport_complete,
    int status_code,
    bool exact_ack_valid)
{
    if (!transport_complete) {
        return BACKEND_HTTP_RETRY;
    }
    if (status_code >= 200 && status_code <= 299) {
        return exact_ack_valid ? BACKEND_HTTP_ACK : BACKEND_HTTP_RETRY;
    }
    if (status_code == 408 || status_code == 429 ||
        (status_code >= 500 && status_code <= 599)) {
        return BACKEND_HTTP_RETRY;
    }
    if (status_code >= 400 && status_code <= 499) {
        return BACKEND_HTTP_QUARANTINE;
    }
    return BACKEND_HTTP_RETRY;
}

bool backend_http_send_all(
    backend_http_send_fn send_fn,
    void *context,
    const uint8_t *data,
    size_t length)
{
    if (length == 0U) {
        return true;
    }
    if (!send_fn || !data) {
        return false;
    }
    size_t sent = 0U;
    while (sent < length) {
        const ssize_t result = send_fn(
            context, data + sent, length - sent);
        if (result <= 0 || (size_t)result > length - sent) {
            return false;
        }
        sent += (size_t)result;
    }
    return true;
}

uint32_t backend_retry_delay_ms(uint8_t exponent, uint32_t random_value)
{
    uint32_t base = 500U;
    for (uint8_t step = 0U; step < exponent && base < 60000U; ++step) {
        base = base > 30000U ? 60000U : base * 2U;
    }
    const uint32_t jitter_range = base / 4U + 1U;
    return base + random_value % jitter_range;
}
