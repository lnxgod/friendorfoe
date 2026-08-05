#include "ble_investigation_protocol.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "backend_uart_protocol.h"

#define JSON_ESCAPED_SIZE(size) ((size) * 2 - 1)
#define BLE_INV_KNOWN_PROPERTY_MASK ((uint16_t)( \
    BLE_INV_PROP_BROADCAST | BLE_INV_PROP_READ | \
    BLE_INV_PROP_WRITE_WITHOUT_RESPONSE | BLE_INV_PROP_WRITE | \
    BLE_INV_PROP_NOTIFY | BLE_INV_PROP_INDICATE | \
    BLE_INV_PROP_AUTHENTICATED_SIGNED_WRITES | \
    BLE_INV_PROP_EXTENDED_PROPERTIES))

typedef struct {
    uint16_t mask;
    const char *name;
} property_name_t;

static const property_name_t PROPERTY_NAMES[] = {
    {BLE_INV_PROP_BROADCAST, "broadcast"},
    {BLE_INV_PROP_READ, "read"},
    {BLE_INV_PROP_WRITE_WITHOUT_RESPONSE, "write_without_response"},
    {BLE_INV_PROP_WRITE, "write"},
    {BLE_INV_PROP_NOTIFY, "notify"},
    {BLE_INV_PROP_INDICATE, "indicate"},
    {BLE_INV_PROP_AUTHENTICATED_SIGNED_WRITES, "authenticated_signed_writes"},
    {BLE_INV_PROP_EXTENDED_PROPERTIES, "extended_properties"},
};

static size_t bounded_length(const char *text, size_t bound)
{
    if (!text) return 0;
    size_t len = 0;
    while (len < bound && text[len] != '\0') ++len;
    return len;
}

static bool bounded_equal(const char *left,
                          size_t left_bound,
                          const char *right,
                          size_t right_bound)
{
    size_t left_len = bounded_length(left, left_bound);
    size_t right_len = bounded_length(right, right_bound);
    return left_len < left_bound && right_len < right_bound &&
           left_len == right_len && memcmp(left, right, left_len) == 0;
}

static bool copy_bounded(char *out,
                         size_t out_len,
                         const char *text,
                         size_t text_bound)
{
    if (!out || out_len == 0) return false;
    size_t len = bounded_length(text, text_bound);
    bool complete = len < text_bound && len < out_len;
    if (len >= out_len) len = out_len - 1;
    if (len > 0 && text) memcpy(out, text, len);
    out[len] = '\0';
    return complete;
}

static bool json_escape(char *out,
                        size_t out_len,
                        const char *text,
                        size_t text_bound)
{
    if (!out || out_len == 0) return false;
    size_t used = 0;
    size_t len = bounded_length(text, text_bound);
    bool terminated = len < text_bound;

    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)text[i];
        const char *escape = NULL;
        switch (ch) {
        case '"': escape = "\\\""; break;
        case '\\': escape = "\\\\"; break;
        case '\b': escape = "\\b"; break;
        case '\f': escape = "\\f"; break;
        case '\n': escape = "\\n"; break;
        case '\r': escape = "\\r"; break;
        case '\t': escape = "\\t"; break;
        default: break;
        }

        if (escape) {
            if (used + 2 >= out_len) {
                out[0] = '\0';
                return false;
            }
            out[used++] = escape[0];
            out[used++] = escape[1];
        } else {
            if (used + 1 >= out_len) {
                out[0] = '\0';
                return false;
            }
            out[used++] = ch >= 0x20 && ch <= 0x7E ? (char)ch : '?';
        }
    }
    out[used] = '\0';
    return terminated;
}

static size_t write_json(char *out, size_t out_len, const char *format, ...)
{
    if (!out || out_len == 0 || !format) return 0;
    size_t bounded_out_len = out_len < UART_JSON_MAX_SIZE
        ? out_len
        : UART_JSON_MAX_SIZE;

    va_list args;
    va_start(args, format);
    int written = vsnprintf(out, bounded_out_len, format, args);
    va_end(args);

    if (written < 0 || (size_t)written >= bounded_out_len ||
        (size_t)written >= UART_JSON_MAX_SIZE) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)written;
}

static bool append_text(char *out,
                        size_t out_len,
                        size_t *used,
                        const char *text)
{
    if (!out || !used || !text || *used >= out_len) return false;
    int written = snprintf(out + *used, out_len - *used, "%s", text);
    if (written < 0 || (size_t)written >= out_len - *used) return false;
    *used += (size_t)written;
    return true;
}

static bool properties_to_json(uint16_t properties,
                               char *out,
                               size_t out_len)
{
    if (!out || out_len < 3) return false;
    if ((properties & (uint16_t)~BLE_INV_KNOWN_PROPERTY_MASK) != 0) return false;
    size_t used = 0;
    out[used++] = '[';
    out[used] = '\0';
    bool first = true;

    for (size_t i = 0; i < sizeof(PROPERTY_NAMES) / sizeof(PROPERTY_NAMES[0]); ++i) {
        if ((properties & PROPERTY_NAMES[i].mask) == 0) continue;
        if (!first && !append_text(out, out_len, &used, ",")) return false;
        if (!append_text(out, out_len, &used, "\"")) return false;
        if (!append_text(out, out_len, &used, PROPERTY_NAMES[i].name)) return false;
        if (!append_text(out, out_len, &used, "\"")) return false;
        first = false;
    }

    return append_text(out, out_len, &used, "]");
}

const char *ble_investigation_mode_name(ble_investigation_mode_t mode)
{
    switch (mode) {
    case BLE_INV_MODE_GATT: return "gatt";
    case BLE_INV_MODE_PASSIVE_CAPTURE: return "passive_capture";
    default: return NULL;
    }
}

const char *ble_investigation_state_name(ble_investigation_state_t state)
{
    switch (state) {
    case BLE_INV_IDLE: return "idle";
    case BLE_INV_QUEUED: return "queued";
    case BLE_INV_SCANNING: return "scanning";
    case BLE_INV_CONNECTING: return "connecting";
    case BLE_INV_DISCOVERING: return "discovering";
    case BLE_INV_READING: return "reading";
    case BLE_INV_COMPLETE: return "complete";
    case BLE_INV_FAILED: return "failed";
    case BLE_INV_CANCELLED: return "cancelled";
    default: return NULL;
    }
}

bool ble_investigation_mode_from_name(const char *name,
                                      ble_investigation_mode_t *out)
{
    if (!name || !out) return false;
    if (strcmp(name, "gatt") == 0) {
        *out = BLE_INV_MODE_GATT;
        return true;
    }
    if (strcmp(name, "passive_capture") == 0) {
        *out = BLE_INV_MODE_PASSIVE_CAPTURE;
        return true;
    }
    return false;
}

bool ble_investigation_state_from_name(const char *name,
                                       ble_investigation_state_t *out)
{
    if (!name || !out) return false;
    for (int state = BLE_INV_IDLE; state <= BLE_INV_CANCELLED; ++state) {
        const char *candidate = ble_investigation_state_name(
            (ble_investigation_state_t)state);
        if (candidate && strcmp(name, candidate) == 0) {
            *out = (ble_investigation_state_t)state;
            return true;
        }
    }
    return false;
}

void ble_investigation_result_init(ble_investigation_result_t *result)
{
    if (!result) return;
    memset(result, 0, sizeof(*result));
    result->mode = BLE_INV_MODE_GATT;
    result->state = BLE_INV_IDLE;
}

static bool result_has_request(const ble_investigation_result_t *result)
{
    return result && bounded_length(result->request_id, BLE_INV_REQUEST_ID_LEN) > 0;
}

static bool chunk_matches_result(const ble_investigation_result_t *result,
                                 const ble_investigation_chunk_t *chunk)
{
    return result_has_request(result) && chunk &&
           bounded_equal(result->request_id, BLE_INV_REQUEST_ID_LEN,
                         chunk->request_id, BLE_INV_REQUEST_ID_LEN);
}

bool ble_investigation_result_accept(ble_investigation_result_t *result,
                                     const ble_investigation_chunk_t *chunk)
{
    if (!result || !chunk) return false;

    if (chunk->kind == BLE_INV_CHUNK_BEGIN) {
        if (result_has_request(result)) return false;
        ble_investigation_result_init(result);
        if (!copy_bounded(result->request_id, sizeof(result->request_id),
                          chunk->request_id, sizeof(chunk->request_id)) ||
            result->request_id[0] == '\0' ||
            !ble_investigation_mode_name(chunk->mode)) {
            ble_investigation_result_init(result);
            return false;
        }
        result->mode = chunk->mode;
        result->state = BLE_INV_QUEUED;
        if (!copy_bounded(result->target_mac, sizeof(result->target_mac),
                          chunk->target_mac, sizeof(chunk->target_mac))) {
            result->truncated = true;
        }
        return true;
    }

    if (!chunk_matches_result(result, chunk)) return false;
    if (result->state >= BLE_INV_COMPLETE && result->state <= BLE_INV_CANCELLED) {
        return false;
    }

    switch (chunk->kind) {
    case BLE_INV_CHUNK_PROGRESS:
        if (!ble_investigation_state_name(chunk->state) ||
            chunk->state < BLE_INV_QUEUED || chunk->state > BLE_INV_READING ||
            chunk->state < result->state) {
            return false;
        }
        result->state = chunk->state;
        return true;

    case BLE_INV_CHUNK_SERVICE:
        if (chunk->index < 0 || chunk->index != result->service_count) {
            return false;
        }
        if (result->service_count < BLE_INV_MAX_SERVICES) {
            if (!copy_bounded(result->services[result->service_count], BLE_INV_UUID_LEN,
                              chunk->uuid, sizeof(chunk->uuid))) {
                result->truncated = true;
            }
            ++result->service_count;
        } else {
            result->truncated = true;
            return false;
        }
        return true;

    case BLE_INV_CHUNK_CHARACTERISTIC:
        if (chunk->index < 0 || chunk->index != result->characteristic_count) {
            return false;
        }
        if (result->characteristic_count < BLE_INV_MAX_CHARS) {
            ble_investigation_characteristic_t *characteristic =
                &result->characteristics[result->characteristic_count];
            bool service_ok = copy_bounded(
                characteristic->service_uuid, sizeof(characteristic->service_uuid),
                chunk->service_uuid, sizeof(chunk->service_uuid));
            bool uuid_ok = copy_bounded(characteristic->uuid, sizeof(characteristic->uuid),
                                        chunk->uuid, sizeof(chunk->uuid));
            characteristic->properties = chunk->properties;
            if (!service_ok || !uuid_ok) result->truncated = true;
            ++result->characteristic_count;
        } else {
            result->truncated = true;
            return false;
        }
        return true;

    case BLE_INV_CHUNK_READ:
        if (chunk->index < 0 || chunk->index != result->read_count) {
            return false;
        }
        if (result->read_count < BLE_INV_MAX_READS) {
            ble_investigation_read_t *read = &result->reads[result->read_count];
            bool uuid_ok = copy_bounded(read->uuid, sizeof(read->uuid),
                                        chunk->uuid, sizeof(chunk->uuid));
            bool value_ok = copy_bounded(read->value_hex, sizeof(read->value_hex),
                                         chunk->value_hex, sizeof(chunk->value_hex));
            if (!uuid_ok || !value_ok) result->truncated = true;
            ++result->read_count;
        } else {
            result->truncated = true;
            return false;
        }
        return true;

    case BLE_INV_CHUNK_END:
        if (chunk->state != BLE_INV_COMPLETE &&
            chunk->state != BLE_INV_FAILED &&
            chunk->state != BLE_INV_CANCELLED) {
            return false;
        }
        result->state = chunk->state;
        result->authentication_required = chunk->authentication_required;
        result->truncated = result->truncated || chunk->truncated;
        if (!copy_bounded(result->summary, sizeof(result->summary),
                          chunk->summary, sizeof(chunk->summary)) ||
            !copy_bounded(result->error, sizeof(result->error),
                          chunk->error, sizeof(chunk->error))) {
            result->truncated = true;
        }
        return true;

    case BLE_INV_CHUNK_BEGIN:
    default:
        return false;
    }
}

size_t ble_investigation_request_to_json(
    const ble_investigation_request_t *request,
    char *out,
    size_t out_len)
{
    if (!request || !out) return 0;
    const char *mode = ble_investigation_mode_name(request->mode);
    if (!mode) return 0;
    uint32_t timeout_ms = request->timeout_ms == 0
        ? BLE_INV_DEFAULT_TIMEOUT_MS
        : request->timeout_ms;

    char request_id[JSON_ESCAPED_SIZE(BLE_INV_REQUEST_ID_LEN)];
    char target_mac[JSON_ESCAPED_SIZE(18)];
    if (!json_escape(request_id, sizeof(request_id), request->request_id,
                     sizeof(request->request_id)) || request_id[0] == '\0' ||
        !json_escape(target_mac, sizeof(target_mac), request->target_mac,
                     sizeof(request->target_mac))) {
        return 0;
    }

    if (target_mac[0] == '\0') {
        return write_json(out, out_len,
            "{\"type\":\"%s\",\"request_id\":\"%s\",\"mode\":\"%s\","
            "\"target\":null,\"timeout_ms\":%lu}",
            MSG_TYPE_BLE_INVESTIGATE, request_id, mode,
            (unsigned long)timeout_ms);
    }
    return write_json(out, out_len,
        "{\"type\":\"%s\",\"request_id\":\"%s\",\"mode\":\"%s\","
        "\"target\":\"%s\",\"timeout_ms\":%lu}",
        MSG_TYPE_BLE_INVESTIGATE, request_id, mode, target_mac,
        (unsigned long)timeout_ms);
}

size_t ble_investigation_chunk_to_json(const ble_investigation_chunk_t *chunk,
                                       char *out,
                                       size_t out_len)
{
    if (!chunk || !out) return 0;

    char request_id[JSON_ESCAPED_SIZE(BLE_INV_REQUEST_ID_LEN)];
    char target_mac[JSON_ESCAPED_SIZE(18)];
    char service_uuid[JSON_ESCAPED_SIZE(BLE_INV_UUID_LEN)];
    char uuid[JSON_ESCAPED_SIZE(BLE_INV_UUID_LEN)];
    char value_hex[JSON_ESCAPED_SIZE(BLE_INV_READ_HEX_LEN)];
    char summary[JSON_ESCAPED_SIZE(BLE_INV_SUMMARY_LEN)];
    char error[JSON_ESCAPED_SIZE(BLE_INV_ERROR_LEN)];
    char properties[192];

    if (!json_escape(request_id, sizeof(request_id), chunk->request_id,
                     sizeof(chunk->request_id)) || request_id[0] == '\0') {
        return 0;
    }

    switch (chunk->kind) {
    case BLE_INV_CHUNK_BEGIN: {
        const char *mode = ble_investigation_mode_name(chunk->mode);
        if (!mode || !json_escape(target_mac, sizeof(target_mac), chunk->target_mac,
                                  sizeof(chunk->target_mac))) {
            return 0;
        }
        if (target_mac[0] == '\0') {
            return write_json(out, out_len,
                "{\"type\":\"%s\",\"request_id\":\"%s\",\"mode\":\"%s\","
                "\"target_mac\":null}",
                MSG_TYPE_BLE_INV_BEGIN, request_id, mode);
        }
        return write_json(out, out_len,
            "{\"type\":\"%s\",\"request_id\":\"%s\",\"mode\":\"%s\","
            "\"target_mac\":\"%s\"}",
            MSG_TYPE_BLE_INV_BEGIN, request_id, mode, target_mac);
    }

    case BLE_INV_CHUNK_PROGRESS: {
        const char *state = ble_investigation_state_name(chunk->state);
        if (!state || chunk->state < BLE_INV_QUEUED ||
            chunk->state > BLE_INV_READING) {
            return 0;
        }
        return write_json(out, out_len,
            "{\"type\":\"%s\",\"request_id\":\"%s\",\"state\":\"%s\"}",
            MSG_TYPE_BLE_INV_PROGRESS, request_id, state);
    }

    case BLE_INV_CHUNK_SERVICE:
        if (chunk->index < 0 ||
            !json_escape(uuid, sizeof(uuid), chunk->uuid, sizeof(chunk->uuid))) {
            return 0;
        }
        return write_json(out, out_len,
            "{\"type\":\"%s\",\"request_id\":\"%s\",\"index\":%d,"
            "\"uuid\":\"%s\"}",
            MSG_TYPE_BLE_INV_SERVICE, request_id, chunk->index, uuid);

    case BLE_INV_CHUNK_CHARACTERISTIC:
        if (chunk->index < 0 ||
            !json_escape(service_uuid, sizeof(service_uuid), chunk->service_uuid,
                         sizeof(chunk->service_uuid)) ||
            !json_escape(uuid, sizeof(uuid), chunk->uuid, sizeof(chunk->uuid)) ||
            !properties_to_json(chunk->properties, properties, sizeof(properties))) {
            return 0;
        }
        return write_json(out, out_len,
            "{\"type\":\"%s\",\"request_id\":\"%s\",\"index\":%d,"
            "\"service_uuid\":\"%s\",\"uuid\":\"%s\",\"properties\":%s}",
            MSG_TYPE_BLE_INV_CHAR, request_id, chunk->index,
            service_uuid, uuid, properties);

    case BLE_INV_CHUNK_READ:
        if (chunk->index < 0 ||
            !json_escape(uuid, sizeof(uuid), chunk->uuid, sizeof(chunk->uuid)) ||
            !json_escape(value_hex, sizeof(value_hex), chunk->value_hex,
                         sizeof(chunk->value_hex))) {
            return 0;
        }
        return write_json(out, out_len,
            "{\"type\":\"%s\",\"request_id\":\"%s\",\"index\":%d,"
            "\"uuid\":\"%s\",\"value_hex\":\"%s\"}",
            MSG_TYPE_BLE_INV_READ, request_id, chunk->index, uuid, value_hex);

    case BLE_INV_CHUNK_END: {
        const char *state = ble_investigation_state_name(chunk->state);
        if (!state || chunk->state < BLE_INV_COMPLETE ||
            chunk->state > BLE_INV_CANCELLED ||
            !json_escape(summary, sizeof(summary), chunk->summary,
                         sizeof(chunk->summary)) ||
            !json_escape(error, sizeof(error), chunk->error, sizeof(chunk->error))) {
            return 0;
        }
        const char *auth = chunk->authentication_required ? "true" : "false";
        const char *truncated = chunk->truncated ? "true" : "false";
        if (error[0] == '\0') {
            return write_json(out, out_len,
                "{\"type\":\"%s\",\"request_id\":\"%s\",\"state\":\"%s\","
                "\"summary\":\"%s\",\"error\":null,"
                "\"authentication_required\":%s,\"truncated\":%s}",
                MSG_TYPE_BLE_INV_END, request_id, state, summary, auth, truncated);
        }
        return write_json(out, out_len,
            "{\"type\":\"%s\",\"request_id\":\"%s\",\"state\":\"%s\","
            "\"summary\":\"%s\",\"error\":\"%s\","
            "\"authentication_required\":%s,\"truncated\":%s}",
            MSG_TYPE_BLE_INV_END, request_id, state, summary, error, auth, truncated);
    }

    default:
        return 0;
    }
}
