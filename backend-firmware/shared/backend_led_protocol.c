#include "backend_led_protocol.h"

#include <inttypes.h>
#include <string.h>

#include "backend_json_reader.h"
#include "backend_json_writer.h"

#define BACKEND_LED_JSON_TOKEN_CAPACITY 16U
#define BACKEND_LED_JSON_FIELD_COUNT 4U

static const char *wire_state_name(backend_led_state_t state)
{
    switch (state) {
    case BACKEND_LED_HEALTHY:
        return "healthy";
    case BACKEND_LED_NETWORK_DEGRADED:
        return "network_degraded";
    case BACKEND_LED_DRONE:
        return "drone";
    case BACKEND_LED_META:
        return "meta";
    case BACKEND_LED_DRONE_META:
        return "drone_meta";
    case BACKEND_LED_FATAL:
        return "fatal";
    case BACKEND_LED_UART_LOST:
    default:
        return NULL;
    }
}

static bool parse_wire_state(const char *name, backend_led_state_t *state)
{
    if (name == NULL || state == NULL) {
        return false;
    }
    if (strcmp(name, "healthy") == 0) {
        *state = BACKEND_LED_HEALTHY;
    } else if (strcmp(name, "network_degraded") == 0) {
        *state = BACKEND_LED_NETWORK_DEGRADED;
    } else if (strcmp(name, "drone") == 0) {
        *state = BACKEND_LED_DRONE;
    } else if (strcmp(name, "meta") == 0) {
        *state = BACKEND_LED_META;
    } else if (strcmp(name, "drone_meta") == 0) {
        *state = BACKEND_LED_DRONE_META;
    } else if (strcmp(name, "fatal") == 0) {
        *state = BACKEND_LED_FATAL;
    } else {
        return false;
    }
    return true;
}

static bool command_valid(const backend_led_command_t *command)
{
    return command != NULL && wire_state_name(command->state) != NULL &&
           command->generation != 0U &&
           command->ttl_ms >= BACKEND_LED_TTL_MIN_MS &&
           command->ttl_ms <= BACKEND_LED_TTL_MAX_MS;
}

size_t backend_led_command_encode(
    const backend_led_command_t *command, char *output, size_t capacity)
{
    if (output != NULL && capacity > 0U) {
        output[0] = '\0';
    }
    if (!command_valid(command) || output == NULL || capacity == 0U) {
        return 0U;
    }

    backend_json_writer_t writer;
    backend_json_writer_init(&writer, output, capacity);
    backend_json_append(&writer, "{\"type\":\"led_state\",\"state\":");
    backend_json_append_escaped(&writer, wire_state_name(command->state));
    backend_json_append_format(
        &writer,
        ",\"generation\":%" PRIu32 ",\"ttl_ms\":%" PRIu32 "}",
        command->generation,
        command->ttl_ms);
    return backend_json_writer_finish(&writer);
}

static bool find_field(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    const char *name,
    size_t *index)
{
    return backend_json_object_find(
        json, tokens, token_count, 0U, name, index);
}

static bool read_u32_field(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    const char *name,
    uint32_t *value)
{
    size_t index = 0;
    uint64_t parsed = 0;
    if (value == NULL ||
        !find_field(json, tokens, token_count, name, &index) ||
        !backend_json_get_u64(json, &tokens[index], &parsed) ||
        parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool read_string_field(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    const char *name,
    char *value,
    size_t capacity)
{
    size_t index = 0;
    return find_field(json, tokens, token_count, name, &index) &&
           backend_json_copy_string(json, &tokens[index], value, capacity);
}

bool backend_led_command_decode(
    const char *json, size_t length, backend_led_command_t *out)
{
    if (json == NULL || out == NULL || length == 0U) {
        return false;
    }

    backend_json_token_t tokens[BACKEND_LED_JSON_TOKEN_CAPACITY];
    size_t token_count = 0;
    if (backend_json_parse(
            json, length, tokens, BACKEND_LED_JSON_TOKEN_CAPACITY,
            &token_count) != BACKEND_JSON_OK ||
        token_count == 0U || tokens[0].kind != BACKEND_JSON_OBJECT ||
        tokens[0].child_count != BACKEND_LED_JSON_FIELD_COUNT * 2U) {
        return false;
    }

    char type[16];
    char state_name[24];
    backend_led_command_t decoded = {0};
    if (!read_string_field(
            json, tokens, token_count, "type", type, sizeof(type)) ||
        strcmp(type, "led_state") != 0 ||
        !read_string_field(
            json, tokens, token_count, "state", state_name,
            sizeof(state_name)) ||
        !parse_wire_state(state_name, &decoded.state) ||
        !read_u32_field(
            json, tokens, token_count, "generation", &decoded.generation) ||
        !read_u32_field(
            json, tokens, token_count, "ttl_ms", &decoded.ttl_ms) ||
        !command_valid(&decoded)) {
        return false;
    }

    *out = decoded;
    return true;
}

void backend_led_mirror_init(backend_led_mirror_t *mirror)
{
    if (mirror != NULL) {
        *mirror = (backend_led_mirror_t){0};
    }
}

backend_led_accept_result_t backend_led_mirror_accept(
    backend_led_mirror_t *mirror,
    const backend_led_command_t *incoming,
    int64_t now_ms)
{
    if (mirror == NULL || !command_valid(incoming) || now_ms < 0) {
        return BACKEND_LED_REJECTED_INVALID;
    }

    if (!mirror->has_accepted) {
        mirror->accepted = *incoming;
        mirror->accepted_monotonic_ms = now_ms;
        mirror->pattern_transition_count = 1U;
        mirror->has_accepted = true;
        return BACKEND_LED_ACCEPTED_NEW;
    }

    if (incoming->generation < mirror->accepted.generation) {
        return BACKEND_LED_REJECTED_STALE;
    }
    if (incoming->generation == mirror->accepted.generation) {
        if (incoming->state != mirror->accepted.state ||
            incoming->ttl_ms != mirror->accepted.ttl_ms) {
            return BACKEND_LED_REJECTED_CONFLICT;
        }
        if (now_ms < mirror->accepted_monotonic_ms) {
            return BACKEND_LED_REJECTED_INVALID;
        }
        mirror->accepted_monotonic_ms = now_ms;
        return BACKEND_LED_ACCEPTED_REFRESH;
    }
    if (now_ms < mirror->accepted_monotonic_ms) {
        return BACKEND_LED_REJECTED_INVALID;
    }

    const bool state_changed = incoming->state != mirror->accepted.state;
    mirror->accepted = *incoming;
    mirror->accepted_monotonic_ms = now_ms;
    if (state_changed) {
        ++mirror->pattern_transition_count;
    }
    return BACKEND_LED_ACCEPTED_NEW;
}

backend_led_state_t backend_led_mirror_effective(
    const backend_led_mirror_t *mirror, int64_t now_ms)
{
    if (mirror == NULL || !mirror->has_accepted ||
        now_ms < mirror->accepted_monotonic_ms) {
        return BACKEND_LED_UART_LOST;
    }
    const uint64_t age_ms =
        (uint64_t)(now_ms - mirror->accepted_monotonic_ms);
    return age_ms < mirror->accepted.ttl_ms
        ? mirror->accepted.state
        : BACKEND_LED_UART_LOST;
}
