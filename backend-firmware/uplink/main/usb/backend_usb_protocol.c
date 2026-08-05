#include "backend_usb_protocol.h"

#include <stdio.h>
#include <string.h>

#include "backend_json_reader.h"
#include "backend_json_writer.h"

static bool span_equals(
    const char *span, size_t length, const char *literal)
{
    const size_t literal_length = strlen(literal);
    return length == literal_length &&
           memcmp(span, literal, literal_length) == 0;
}

static bool span_starts_with(
    const char *span, size_t length, const char *prefix)
{
    const size_t prefix_length = strlen(prefix);
    return length >= prefix_length &&
           memcmp(span, prefix, prefix_length) == 0;
}

static void invalidate(backend_usb_command_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->kind = BACKEND_USB_COMMAND_INVALID;
}

static bool safe_text(const char *value, size_t length)
{
    if (value == NULL) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        const unsigned char byte = (unsigned char)value[index];
        if (byte < 0x20U || byte == 0x7FU) {
            return false;
        }
    }
    return true;
}

static bool root_object_exact(
    const char *json,
    size_t length,
    backend_json_token_t *tokens,
    size_t token_capacity,
    size_t expected_members,
    size_t *out_token_count)
{
    size_t token_count = 0;
    if (json == NULL || length == 0 ||
        backend_json_parse(json, length, tokens, token_capacity,
                           &token_count) != BACKEND_JSON_OK ||
        token_count == 0 || tokens[0].kind != BACKEND_JSON_OBJECT ||
        tokens[0].child_count != expected_members * 2U) {
        return false;
    }
    if (out_token_count != NULL) {
        *out_token_count = token_count;
    }
    return true;
}

static bool copy_required_string(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    const char *key,
    char *output,
    size_t capacity)
{
    size_t value_index = 0;
    return backend_json_object_find(
               json, tokens, token_count, 0, key, &value_index) &&
           backend_json_copy_string(
               json, &tokens[value_index], output, capacity) &&
           output[0] != '\0' && safe_text(output, strlen(output));
}

static bool parse_live_start_json(
    const char *json, size_t length, backend_usb_command_t *out)
{
    backend_json_token_t tokens[8];
    size_t token_count = 0;
    char client[16];
    uint64_t protocol = 0;
    size_t protocol_index = 0;
    if (!root_object_exact(
            json, length, tokens,
            sizeof(tokens) / sizeof(tokens[0]), 2, &token_count) ||
        !copy_required_string(
            json, tokens, token_count, "client",
            client, sizeof(client)) ||
        strcmp(client, "new_dash") != 0 ||
        !backend_json_object_find(
            json, tokens, token_count, 0, "protocol", &protocol_index) ||
        !backend_json_get_u64(json, &tokens[protocol_index], &protocol) ||
        protocol != UINT64_C(1)) {
        return false;
    }
    out->kind = BACKEND_USB_COMMAND_LIVE_START;
    out->json = json;
    out->json_length = length;
    return true;
}

static bool parse_live_ack_json(
    const char *json, size_t length, backend_usb_command_t *out)
{
    backend_json_token_t tokens[8];
    size_t token_count = 0;
    size_t sequence_index = 0;
    if (!root_object_exact(
            json, length, tokens,
            sizeof(tokens) / sizeof(tokens[0]), 2, &token_count) ||
        !copy_required_string(
            json, tokens, token_count, "session_id",
            out->session_id, sizeof(out->session_id)) ||
        !backend_json_object_find(
            json, tokens, token_count, 0, "sequence", &sequence_index) ||
        !backend_json_get_u64(
            json, &tokens[sequence_index], &out->sequence)) {
        return false;
    }
    out->kind = BACKEND_USB_COMMAND_LIVE_ACK;
    out->json = json;
    out->json_length = length;
    return true;
}

static bool parse_live_stop_json(
    const char *json, size_t length, backend_usb_command_t *out)
{
    backend_json_token_t tokens[4];
    size_t token_count = 0;
    if (!root_object_exact(
            json, length, tokens,
            sizeof(tokens) / sizeof(tokens[0]), 1, &token_count) ||
        !copy_required_string(
            json, tokens, token_count, "session_id",
            out->session_id, sizeof(out->session_id))) {
        return false;
    }
    out->kind = BACKEND_USB_COMMAND_LIVE_STOP;
    out->json = json;
    out->json_length = length;
    return true;
}

static bool set_key_allowed(const char *key, size_t length)
{
    static const char *const allowed[] = {
        "wifi_ssid",
        "wifi_pass",
        "backend_url",
        "device_id",
        "ap_pass",
    };
    for (size_t index = 0; index < sizeof(allowed) / sizeof(allowed[0]);
         ++index) {
        if (span_equals(key, length, allowed[index])) {
            return true;
        }
    }
    return false;
}

static bool parse_set(
    const char *payload, size_t length, backend_usb_command_t *out)
{
    const char *separator = memchr(payload, '=', length);
    if (separator == NULL || separator == payload) {
        return false;
    }
    const size_t key_length = (size_t)(separator - payload);
    const char *value = separator + 1U;
    const size_t value_length =
        length - key_length - 1U;
    if (key_length >= sizeof(out->key) ||
        value_length >= sizeof(out->value) ||
        !set_key_allowed(payload, key_length) ||
        !safe_text(value, value_length)) {
        return false;
    }
    memcpy(out->key, payload, key_length);
    out->key[key_length] = '\0';
    memcpy(out->value, value, value_length);
    out->value[value_length] = '\0';
    out->kind = BACKEND_USB_COMMAND_SET;
    return true;
}

static bool parse_config_set_json(
    const char *json, size_t length, backend_usb_command_t *out)
{
    backend_json_token_t tokens[BACKEND_JSON_MAX_TOKENS];
    char decoded[BACKEND_USB_COMMAND_MAX + 1U];
    size_t token_count = 0;
    if (backend_json_parse(
            json, length, tokens,
            sizeof(tokens) / sizeof(tokens[0]), &token_count) !=
            BACKEND_JSON_OK ||
        token_count == 0 || tokens[0].kind != BACKEND_JSON_OBJECT) {
        return false;
    }
    for (size_t index = 0; index < token_count; ++index) {
        if (tokens[index].kind == BACKEND_JSON_STRING &&
            (!backend_json_copy_string(
                 json, &tokens[index], decoded, sizeof(decoded)) ||
             !safe_text(decoded, strlen(decoded)))) {
            return false;
        }
    }
    out->kind = BACKEND_USB_COMMAND_CONFIG_SET;
    out->json = json;
    out->json_length = length;
    return true;
}

bool backend_usb_protocol_parse_line(
    const char *line, size_t length, backend_usb_command_t *out)
{
    invalidate(out);
    if (line == NULL || out == NULL || length == 0 ||
        length > BACKEND_USB_COMMAND_MAX || !safe_text(line, length)) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    if (span_equals(line, length, "FOF_PING")) {
        out->kind = BACKEND_USB_COMMAND_PING;
        return true;
    }
    if (span_equals(line, length, "FOF_STATUS")) {
        out->kind = BACKEND_USB_COMMAND_STATUS;
        return true;
    }
    if (span_equals(line, length, "FOF_CONFIG_GET")) {
        out->kind = BACKEND_USB_COMMAND_CONFIG_GET;
        return true;
    }
    if (span_equals(line, length, "FOF_SAVE")) {
        out->kind = BACKEND_USB_COMMAND_SAVE;
        return true;
    }
    if (span_equals(line, length, "FOF_BACKEND_STATUS")) {
        out->kind = BACKEND_USB_COMMAND_BACKEND_STATUS;
        return true;
    }
    if (span_equals(line, length, "FOF_AP_START")) {
        out->kind = BACKEND_USB_COMMAND_AP_START;
        return true;
    }

    static const char live_start[] = "FOF_LIVE_START:";
    static const char live_ack[] = "FOF_LIVE_ACK:";
    static const char live_stop[] = "FOF_LIVE_STOP:";
    static const char config_set[] = "FOF_CONFIG_SET:";
    static const char set[] = "FOF_SET:";
    bool parsed = false;
    if (span_starts_with(line, length, live_start)) {
        parsed = parse_live_start_json(
            line + sizeof(live_start) - 1U,
            length - (sizeof(live_start) - 1U), out);
    } else if (span_starts_with(line, length, live_ack)) {
        parsed = parse_live_ack_json(
            line + sizeof(live_ack) - 1U,
            length - (sizeof(live_ack) - 1U), out);
    } else if (span_starts_with(line, length, live_stop)) {
        parsed = parse_live_stop_json(
            line + sizeof(live_stop) - 1U,
            length - (sizeof(live_stop) - 1U), out);
    } else if (span_starts_with(line, length, config_set)) {
        parsed = parse_config_set_json(
            line + sizeof(config_set) - 1U,
            length - (sizeof(config_set) - 1U), out);
    } else if (span_starts_with(line, length, set)) {
        parsed = parse_set(
            line + sizeof(set) - 1U,
            length - (sizeof(set) - 1U), out);
    } else {
        out->kind = BACKEND_USB_COMMAND_UNKNOWN;
        return true;
    }
    if (!parsed) {
        invalidate(out);
    }
    return parsed;
}

static void clear_output(char *output, size_t capacity)
{
    if (output != NULL && capacity > 0) {
        output[0] = '\0';
    }
}

static size_t encode_literal(
    const char *literal, char *output, size_t capacity)
{
    clear_output(output, capacity);
    if (literal == NULL || output == NULL || capacity == 0) {
        return 0;
    }
    const size_t length = strlen(literal);
    if (length >= capacity) {
        return 0;
    }
    memcpy(output, literal, length + 1U);
    return length;
}

size_t backend_usb_protocol_encode_ready(char *output, size_t capacity)
{
    return encode_literal("FOF_READY\n", output, capacity);
}

size_t backend_usb_protocol_encode_pong(
    const backend_firmware_identity_t *identity,
    char *output,
    size_t capacity)
{
    clear_output(output, capacity);
    if (identity == NULL || identity->version == NULL ||
        !safe_text(identity->version, strlen(identity->version)) ||
        output == NULL || capacity == 0) {
        return 0;
    }
    const int written = snprintf(
        output, capacity, "FOF_PONG:%s\n", identity->version);
    if (written < 0 || (size_t)written >= capacity) {
        clear_output(output, capacity);
        return 0;
    }
    return (size_t)written;
}

static bool session_valid(const char *session_id)
{
    if (session_id == NULL) {
        return false;
    }
    const size_t length = strlen(session_id);
    return length > 0 && length <= 32U && safe_text(session_id, length);
}

size_t backend_usb_protocol_encode_live_ready(
    const char *session_id, char *output, size_t capacity)
{
    clear_output(output, capacity);
    if (!session_valid(session_id) || output == NULL || capacity == 0) {
        return 0;
    }
    backend_json_writer_t writer;
    backend_json_writer_init(&writer, output, capacity);
    if (!backend_json_append(
            &writer, "FOF_LIVE_READY:{\"session_id\":" ) ||
        !backend_json_append_escaped(&writer, session_id) ||
        !backend_json_append_format(
            &writer,
            ",\"heartbeat_ms\":%lld,\"lease_ms\":%lld}\n",
            (long long)BACKEND_USB_HEARTBEAT_MS,
            (long long)BACKEND_USB_LIVE_LEASE_MS)) {
        clear_output(output, capacity);
        return 0;
    }
    return backend_json_writer_finish(&writer);
}

size_t backend_usb_protocol_encode_live_heartbeat(
    const char *session_id,
    uint64_t sequence,
    char *output,
    size_t capacity)
{
    clear_output(output, capacity);
    if (!session_valid(session_id) || output == NULL || capacity == 0) {
        return 0;
    }
    backend_json_writer_t writer;
    backend_json_writer_init(&writer, output, capacity);
    if (!backend_json_append(
            &writer, "FOF_LIVE_HEARTBEAT:{\"session_id\":" ) ||
        !backend_json_append_escaped(&writer, session_id) ||
        !backend_json_append_format(
            &writer, ",\"sequence\":%llu}\n",
            (unsigned long long)sequence)) {
        clear_output(output, capacity);
        return 0;
    }
    return backend_json_writer_finish(&writer);
}

size_t backend_usb_protocol_encode_investigation(
    const char *investigation_json,
    size_t json_length,
    char *output,
    size_t capacity)
{
    static const char prefix[] = "FOF_INV:";
    clear_output(output, capacity);
    backend_json_token_t tokens[BACKEND_JSON_MAX_TOKENS];
    size_t token_count = 0;
    if (investigation_json == NULL || json_length == 0 ||
        output == NULL || capacity == 0 ||
        !safe_text(investigation_json, json_length) ||
        backend_json_parse(
            investigation_json, json_length, tokens,
            sizeof(tokens) / sizeof(tokens[0]), &token_count) !=
            BACKEND_JSON_OK ||
        token_count == 0 || tokens[0].kind != BACKEND_JSON_OBJECT ||
        json_length > SIZE_MAX - sizeof(prefix) - 1U) {
        return 0;
    }
    const size_t frame_length =
        sizeof(prefix) - 1U + json_length + 1U;
    if (frame_length > BACKEND_USB_DET_MAX || frame_length >= capacity) {
        return 0;
    }
    memcpy(output, prefix, sizeof(prefix) - 1U);
    memcpy(output + sizeof(prefix) - 1U,
           investigation_json, json_length);
    output[frame_length - 1U] = '\n';
    output[frame_length] = '\0';
    return frame_length;
}
