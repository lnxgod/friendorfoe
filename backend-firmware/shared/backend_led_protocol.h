#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_led_pattern.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_LED_TTL_MIN_MS UINT32_C(2000)
#define BACKEND_LED_TTL_MAX_MS UINT32_C(30000)

typedef struct {
    backend_led_state_t state;
    uint32_t generation;
    uint32_t ttl_ms;
} backend_led_command_t;

typedef enum {
    BACKEND_LED_ACCEPTED_NEW = 0,
    BACKEND_LED_ACCEPTED_REFRESH,
    BACKEND_LED_REJECTED_STALE,
    BACKEND_LED_REJECTED_CONFLICT,
    BACKEND_LED_REJECTED_INVALID,
} backend_led_accept_result_t;

typedef struct {
    backend_led_command_t accepted;
    int64_t accepted_monotonic_ms;
    uint32_t pattern_transition_count;
    bool has_accepted;
} backend_led_mirror_t;

size_t backend_led_command_encode(
    const backend_led_command_t *command, char *output, size_t capacity);

bool backend_led_command_decode(
    const char *json, size_t length, backend_led_command_t *out);

void backend_led_mirror_init(backend_led_mirror_t *mirror);

backend_led_accept_result_t backend_led_mirror_accept(
    backend_led_mirror_t *mirror,
    const backend_led_command_t *incoming,
    int64_t now_ms);

backend_led_state_t backend_led_mirror_effective(
    const backend_led_mirror_t *mirror, int64_t now_ms);

#ifdef __cplusplus
}
#endif
