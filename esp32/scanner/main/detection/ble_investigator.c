#include "ble_investigator.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define APPLE_COMPANY_ID       0x004C
#define MICROSOFT_COMPANY_ID   0x0006
#define FAST_PAIR_SERVICE_UUID 0xFE2C
#define APPLE_AIRPODS_TYPE     0x07
#define APPLE_NEARBY_ACTION    0x0F
#define APPLE_NEARBY_INFO      0x10
#define PASSIVE_CONNECTABLE    0x01

static size_t bounded_length(const char *text, size_t bound)
{
    if (!text) return 0;
    size_t len = 0;
    while (len < bound && text[len] != '\0') ++len;
    return len;
}

static bool copy_text(char *out, size_t out_len,
                      const char *text, size_t text_bound)
{
    if (!out || out_len == 0 || !text) return false;
    size_t len = bounded_length(text, text_bound);
    bool complete = len < text_bound && len < out_len;
    if (len >= out_len) len = out_len - 1;
    memcpy(out, text, len);
    out[len] = '\0';
    return complete;
}

static bool request_id_is_valid(const char request_id[BLE_INV_REQUEST_ID_LEN])
{
    size_t len = bounded_length(request_id, BLE_INV_REQUEST_ID_LEN);
    return len > 0 && len < BLE_INV_REQUEST_ID_LEN;
}

static bool target_is_present(const char target_mac[18])
{
    return bounded_length(target_mac, 18) > 0;
}

static int hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

bool ble_investigator_parse_target_mac(const char *text, uint8_t out[6])
{
    if (!text || !out || strlen(text) != 17) return false;
    for (int index = 0; index < 6; ++index) {
        int offset = index * 3;
        int high = hex_nibble(text[offset]);
        int low = hex_nibble(text[offset + 1]);
        if (high < 0 || low < 0 ||
            (index < 5 && text[offset + 2] != ':')) {
            return false;
        }
        out[5 - index] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static void finish(ble_investigator_t *state,
                   ble_investigation_state_t terminal_state,
                   const char *summary,
                   const char *error)
{
    if (!state || terminal_state < BLE_INV_COMPLETE ||
        terminal_state > BLE_INV_CANCELLED) {
        return;
    }
    state->state = terminal_state;
    state->result.state = terminal_state;
    snprintf(state->result.summary, sizeof(state->result.summary), "%s",
             summary ? summary : "");
    snprintf(state->result.error, sizeof(state->result.error), "%s",
             error ? error : "");
    state->busy = false;
    state->resume_scan_required = true;
    state->result_pending = true;
}

static void fail_with_status(ble_investigator_t *state,
                             const char *error,
                             int status)
{
    char summary[BLE_INV_SUMMARY_LEN];
    snprintf(summary, sizeof(summary), "%s (status=%d)", error, status);
    finish(state, BLE_INV_FAILED, summary, error);
}

static void accept_service(ble_investigator_t *state,
                           const ble_investigator_event_t *event)
{
    ble_investigation_result_t *result = &state->result;
    if (result->service_count >= BLE_INV_MAX_SERVICES) {
        result->truncated = true;
        return;
    }
    if (!copy_text(result->services[result->service_count], BLE_INV_UUID_LEN,
                   event->uuid, sizeof(event->uuid))) {
        result->truncated = true;
    }
    ++result->service_count;
}

static void accept_characteristic(ble_investigator_t *state,
                                  const ble_investigator_event_t *event)
{
    ble_investigation_result_t *result = &state->result;
    if (result->characteristic_count >= BLE_INV_MAX_CHARS) {
        result->truncated = true;
        return;
    }
    ble_investigation_characteristic_t *characteristic =
        &result->characteristics[result->characteristic_count];
    if (!copy_text(characteristic->service_uuid,
                   sizeof(characteristic->service_uuid),
                   event->service_uuid, sizeof(event->service_uuid)) ||
        !copy_text(characteristic->uuid, sizeof(characteristic->uuid),
                   event->uuid, sizeof(event->uuid))) {
        result->truncated = true;
    }
    characteristic->properties = event->properties;
    ++result->characteristic_count;
}

static void accept_read(ble_investigator_t *state,
                        const ble_investigator_event_t *event)
{
    ble_investigation_result_t *result = &state->result;
    if (result->read_count >= BLE_INV_MAX_READS) {
        result->truncated = true;
        return;
    }
    ble_investigation_read_t *read = &result->reads[result->read_count];
    if (!copy_text(read->uuid, sizeof(read->uuid),
                   event->uuid, sizeof(event->uuid))) {
        result->truncated = true;
    }

    size_t value_len = event->value_len;
    size_t max_value_len = (BLE_INV_READ_HEX_LEN - 1) / 2;
    if (value_len > max_value_len) {
        value_len = max_value_len;
        result->truncated = true;
    }
    if (!event->value && value_len > 0) {
        value_len = 0;
        result->truncated = true;
    }
    for (size_t i = 0; i < value_len; ++i) {
        snprintf(&read->value_hex[i * 2], 3, "%02X", event->value[i]);
    }
    read->value_hex[value_len * 2] = '\0';
    ++result->read_count;
    state->state = BLE_INV_READING;
    result->state = BLE_INV_READING;
}

void ble_investigator_init(ble_investigator_t *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->state = BLE_INV_IDLE;
    state->result.state = BLE_INV_IDLE;
    state->result.mode = BLE_INV_MODE_GATT;
}

bool ble_investigator_start(ble_investigator_t *state,
                            const ble_investigation_request_t *request,
                            int64_t now_ms)
{
    if (!state || !request || state->busy || state->result_pending ||
        !request_id_is_valid(request->request_id) ||
        (request->mode != BLE_INV_MODE_GATT &&
         request->mode != BLE_INV_MODE_PASSIVE_CAPTURE) ||
        (request->mode == BLE_INV_MODE_GATT && !target_is_present(request->target_mac)) ||
        (request->mode == BLE_INV_MODE_PASSIVE_CAPTURE && target_is_present(request->target_mac))) {
        return false;
    }

    ble_investigator_init(state);
    state->request = *request;
    uint32_t timeout_ms = request->timeout_ms == 0
        ? BLE_INV_DEFAULT_TIMEOUT_MS
        : request->timeout_ms;
    if (timeout_ms > BLE_INV_DEFAULT_TIMEOUT_MS) {
        timeout_ms = BLE_INV_DEFAULT_TIMEOUT_MS;
    }
    state->request.timeout_ms = timeout_ms;
    state->deadline_ms = now_ms + timeout_ms;
    state->busy = true;
    state->state = request->mode == BLE_INV_MODE_GATT
        ? BLE_INV_CONNECTING
        : BLE_INV_SCANNING;

    state->result.mode = request->mode;
    state->result.state = state->state;
    copy_text(state->result.request_id, sizeof(state->result.request_id),
              request->request_id, sizeof(request->request_id));
    copy_text(state->result.target_mac, sizeof(state->result.target_mac),
              request->target_mac, sizeof(request->target_mac));
    return true;
}

void ble_investigator_handle_event(ble_investigator_t *state,
                                   const ble_investigator_event_t *event,
                                   int64_t now_ms)
{
    if (!state || !event || !state->busy) return;
    if (now_ms >= state->deadline_ms) {
        ble_investigator_tick(state, now_ms);
        return;
    }
    if (state->request.mode != BLE_INV_MODE_GATT) return;

    switch (event->kind) {
    case BLE_INVESTIGATOR_EVENT_CONNECTED:
        state->connected = true;
        state->result.connectable = true;
        state->state = BLE_INV_DISCOVERING;
        state->result.state = BLE_INV_DISCOVERING;
        break;
    case BLE_INVESTIGATOR_EVENT_CONNECT_FAILED:
        fail_with_status(state,
                         event->uuid[0] ? event->uuid : "connect_failed",
                         event->status);
        break;
    case BLE_INVESTIGATOR_EVENT_SERVICE:
        accept_service(state, event);
        break;
    case BLE_INVESTIGATOR_EVENT_CHARACTERISTIC:
        accept_characteristic(state, event);
        break;
    case BLE_INVESTIGATOR_EVENT_READ:
        accept_read(state, event);
        break;
    case BLE_INVESTIGATOR_EVENT_AUTH_REQUIRED:
        state->result.authentication_required = true;
        finish(state, BLE_INV_FAILED,
               "Readable attribute requires authentication or encryption",
               "authentication_required");
        break;
    case BLE_INVESTIGATOR_EVENT_DISCOVERY_COMPLETE: {
        char summary[BLE_INV_SUMMARY_LEN];
        snprintf(summary, sizeof(summary),
                 "%u services, %u characteristics, %u reads",
                 state->result.service_count,
                 state->result.characteristic_count,
                 state->result.read_count);
        finish(state, BLE_INV_COMPLETE, summary, NULL);
        break;
    }
    case BLE_INVESTIGATOR_EVENT_DISCONNECTED:
        state->connected = false;
        fail_with_status(state, "disconnected", event->status);
        break;
    default:
        break;
    }
}

void ble_investigator_tick(ble_investigator_t *state, int64_t now_ms)
{
    if (!state || !state->busy || now_ms < state->deadline_ms) return;
    if (state->request.mode == BLE_INV_MODE_PASSIVE_CAPTURE) {
        char summary[BLE_INV_SUMMARY_LEN];
        snprintf(summary, sizeof(summary),
                 "Prompt families: Apple=%u Fast=%u Swift=%u",
                 state->passive_apple_count,
                 state->passive_fast_pair_count,
                 state->passive_swift_pair_count);
        finish(state, BLE_INV_COMPLETE, summary, NULL);
    } else {
        finish(state, BLE_INV_FAILED, "BLE investigation timed out", "timeout");
    }
}

void ble_investigator_cancel(ble_investigator_t *state, int64_t now_ms)
{
    (void)now_ms;
    if (!state || !state->busy) return;
    finish(state, BLE_INV_CANCELLED, "BLE investigation cancelled", "cancelled");
}

bool ble_investigator_take_result(ble_investigator_t *state,
                                  ble_investigation_result_t *out)
{
    if (!state || !out || !state->result_pending) return false;
    *out = state->result;
    ble_investigator_init(state);
    return true;
}

static void increment_saturating(uint16_t *value)
{
    if (*value < UINT16_MAX) ++*value;
}

void ble_investigator_note_advertisement(ble_investigator_t *state,
                                         const uint8_t mac[6],
                                         const ble_fingerprint_t *fingerprint,
                                         int8_t rssi,
                                         uint8_t properties,
                                         int64_t now_ms)
{
    (void)mac;
    (void)rssi;
    if (!state || !fingerprint || !state->busy ||
        state->request.mode != BLE_INV_MODE_PASSIVE_CAPTURE) {
        return;
    }
    if (now_ms >= state->deadline_ms) {
        ble_investigator_tick(state, now_ms);
        return;
    }

    state->result.connectable = state->result.connectable ||
                                (properties & PASSIVE_CONNECTABLE) != 0;
    if (fingerprint->company_id == APPLE_COMPANY_ID &&
        (fingerprint->apple_type == APPLE_AIRPODS_TYPE ||
         fingerprint->apple_type == APPLE_NEARBY_ACTION ||
         fingerprint->apple_type == APPLE_NEARBY_INFO)) {
        increment_saturating(&state->passive_apple_count);
        return;
    }
    if (fingerprint->company_id == MICROSOFT_COMPANY_ID) {
        increment_saturating(&state->passive_swift_pair_count);
        return;
    }
    uint8_t count = fingerprint->svc_uuid_count > 4
        ? 4
        : fingerprint->svc_uuid_count;
    for (uint8_t i = 0; i < count; ++i) {
        if (fingerprint->service_uuids[i] == FAST_PAIR_SERVICE_UUID) {
            increment_saturating(&state->passive_fast_pair_count);
            return;
        }
    }
}

#ifndef UNIT_TESTING

#include <ctype.h>

#include "ble_investigation_protocol.h"
#include "ble_remote_id.h"
#include "comms/uart_tx.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/ble_uuid.h"
#include "nimble/ble.h"
#include "os/os_mbuf.h"
#include "uart_protocol.h"

#define BLE_INV_INVALID_CONN_HANDLE UINT16_MAX

typedef struct {
    uint16_t start_handle;
    uint16_t end_handle;
} runtime_service_t;

typedef struct {
    uint16_t value_handle;
    char uuid[BLE_INV_UUID_LEN];
} runtime_read_t;

typedef struct {
    ble_investigator_t core;
    bool active;
    bool cleanup_started;
    bool connecting;
    uint16_t conn_handle;
    runtime_service_t services[BLE_INV_MAX_SERVICES];
    uint8_t service_count;
    uint8_t service_index;
    runtime_read_t reads[BLE_INV_MAX_READS];
    uint8_t read_count;
    uint8_t read_index;
} ble_investigator_runtime_t;

static const char *RUNTIME_TAG = "ble_investigator";
static ble_investigator_runtime_t s_runtime;
static ble_investigation_result_t s_last_result;
static portMUX_TYPE s_runtime_lock = portMUX_INITIALIZER_UNLOCKED;

static int runtime_gap_event(struct ble_gap_event *event, void *arg);
static int runtime_service_cb(uint16_t conn_handle,
                              const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *service,
                              void *arg);
static int runtime_characteristic_cb(uint16_t conn_handle,
                                     const struct ble_gatt_error *error,
                                     const struct ble_gatt_chr *characteristic,
                                     void *arg);
static int runtime_read_cb(uint16_t conn_handle,
                           const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attribute,
                           void *arg);

static bool request_ids_match(const char *request_id)
{
    if (!request_id) return false;
    size_t incoming_len = bounded_length(request_id, BLE_INV_REQUEST_ID_LEN);
    size_t active_len = bounded_length(
        s_runtime.core.request.request_id, BLE_INV_REQUEST_ID_LEN);
    return incoming_len > 0 && incoming_len < BLE_INV_REQUEST_ID_LEN &&
           incoming_len == active_len &&
           memcmp(request_id, s_runtime.core.request.request_id, incoming_len) == 0;
}

static bool runtime_is_active(void)
{
    bool active;
    portENTER_CRITICAL(&s_runtime_lock);
    active = s_runtime.active;
    portEXIT_CRITICAL(&s_runtime_lock);
    return active;
}

static bool runtime_accepts_events(void)
{
    bool accepts;
    portENTER_CRITICAL(&s_runtime_lock);
    accepts = s_runtime.active && !s_runtime.cleanup_started;
    portEXIT_CRITICAL(&s_runtime_lock);
    return accepts;
}

static void emit_chunk(const ble_investigation_chunk_t *chunk)
{
    char json[UART_JSON_MAX_SIZE];
    size_t len = ble_investigation_chunk_to_json(chunk, json, sizeof(json));
    if (len == 0 || len >= sizeof(json)) {
        ESP_LOGE(RUNTIME_TAG, "Dropped invalid BLE investigation chunk kind=%d",
                 chunk ? (int)chunk->kind : -1);
        return;
    }
    uart_tx_send_raw_json(json);
}

static void emit_begin(void)
{
    ble_investigation_chunk_t chunk = {
        .kind = BLE_INV_CHUNK_BEGIN,
        .mode = s_runtime.core.request.mode,
    };
    snprintf(chunk.request_id, sizeof(chunk.request_id), "%s",
             s_runtime.core.request.request_id);
    snprintf(chunk.target_mac, sizeof(chunk.target_mac), "%s",
             s_runtime.core.request.target_mac);
    emit_chunk(&chunk);
}

static void emit_progress(ble_investigation_state_t state)
{
    ble_investigation_chunk_t chunk = {
        .kind = BLE_INV_CHUNK_PROGRESS,
        .state = state,
    };
    snprintf(chunk.request_id, sizeof(chunk.request_id), "%s",
             s_runtime.core.request.request_id);
    emit_chunk(&chunk);
}

static void emit_service(uint8_t index, const char *uuid)
{
    ble_investigation_chunk_t chunk = {
        .kind = BLE_INV_CHUNK_SERVICE,
        .index = index,
    };
    snprintf(chunk.request_id, sizeof(chunk.request_id), "%s",
             s_runtime.core.request.request_id);
    snprintf(chunk.uuid, sizeof(chunk.uuid), "%s", uuid);
    emit_chunk(&chunk);
}

static void emit_characteristic(uint8_t index,
                                const char *service_uuid,
                                const char *uuid,
                                uint16_t properties)
{
    ble_investigation_chunk_t chunk = {
        .kind = BLE_INV_CHUNK_CHARACTERISTIC,
        .index = index,
        .properties = properties,
    };
    snprintf(chunk.request_id, sizeof(chunk.request_id), "%s",
             s_runtime.core.request.request_id);
    snprintf(chunk.service_uuid, sizeof(chunk.service_uuid), "%s", service_uuid);
    snprintf(chunk.uuid, sizeof(chunk.uuid), "%s", uuid);
    emit_chunk(&chunk);
}

static void emit_read(uint8_t index, const ble_investigation_read_t *read)
{
    ble_investigation_chunk_t chunk = {
        .kind = BLE_INV_CHUNK_READ,
        .index = index,
    };
    snprintf(chunk.request_id, sizeof(chunk.request_id), "%s",
             s_runtime.core.request.request_id);
    snprintf(chunk.uuid, sizeof(chunk.uuid), "%s", read->uuid);
    snprintf(chunk.value_hex, sizeof(chunk.value_hex), "%s", read->value_hex);
    emit_chunk(&chunk);
}

static void emit_end(void)
{
    const ble_investigation_result_t *result = &s_runtime.core.result;
    ble_investigation_chunk_t chunk = {
        .kind = BLE_INV_CHUNK_END,
        .state = result->state,
        .authentication_required = result->authentication_required,
        .truncated = result->truncated,
    };
    snprintf(chunk.request_id, sizeof(chunk.request_id), "%s", result->request_id);
    snprintf(chunk.summary, sizeof(chunk.summary), "%s", result->summary);
    snprintf(chunk.error, sizeof(chunk.error), "%s", result->error);
    emit_chunk(&chunk);
}

static void runtime_cleanup(void)
{
    bool connected;
    bool connecting;
    uint16_t conn_handle;
    bool resume_scan;

    portENTER_CRITICAL(&s_runtime_lock);
    if (!s_runtime.active || s_runtime.cleanup_started ||
        !s_runtime.core.result_pending) {
        portEXIT_CRITICAL(&s_runtime_lock);
        return;
    }
    s_runtime.cleanup_started = true;
    connected = s_runtime.conn_handle != BLE_INV_INVALID_CONN_HANDLE;
    connecting = s_runtime.connecting;
    conn_handle = s_runtime.conn_handle;
    resume_scan = s_runtime.core.resume_scan_required;
    portEXIT_CRITICAL(&s_runtime_lock);

    emit_end();
    if (connecting && !connected) {
        int rc = ble_gap_conn_cancel();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(RUNTIME_TAG, "ble_gap_conn_cancel failed: %d", rc);
        }
    }
    if (connected && conn_handle != BLE_INV_INVALID_CONN_HANDLE) {
        int rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0 && rc != BLE_HS_ENOTCONN) {
            ESP_LOGW(RUNTIME_TAG, "ble_gap_terminate failed: %d", rc);
        }
    }
    if (resume_scan) {
        ble_remote_id_resume_after_investigation();
    }

    ble_investigator_take_result(&s_runtime.core, &s_last_result);
    portENTER_CRITICAL(&s_runtime_lock);
    s_runtime.connecting = false;
    s_runtime.conn_handle = BLE_INV_INVALID_CONN_HANDLE;
    s_runtime.service_count = 0;
    s_runtime.service_index = 0;
    s_runtime.read_count = 0;
    s_runtime.read_index = 0;
    s_runtime.active = false;
    s_runtime.cleanup_started = false;
    portEXIT_CRITICAL(&s_runtime_lock);
}

static void runtime_fail(int status, const char *error_name)
{
    ble_investigator_event_t event = {
        .kind = BLE_INVESTIGATOR_EVENT_CONNECT_FAILED,
        .status = status,
    };
    if (error_name) {
        snprintf(event.uuid, sizeof(event.uuid), "%s", error_name);
    }
    portENTER_CRITICAL(&s_runtime_lock);
    if (s_runtime.active && s_runtime.core.busy) {
        ble_investigator_handle_event(
            &s_runtime.core, &event, esp_timer_get_time() / 1000);
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    runtime_cleanup();
}

static bool status_requires_authentication(uint16_t status)
{
    const uint16_t att_errors[] = {
        BLE_ATT_ERR_INSUFFICIENT_AUTHEN,
        BLE_ATT_ERR_INSUFFICIENT_AUTHOR,
        BLE_ATT_ERR_INSUFFICIENT_KEY_SZ,
        BLE_ATT_ERR_INSUFFICIENT_ENC,
    };
    for (size_t i = 0; i < sizeof(att_errors) / sizeof(att_errors[0]); ++i) {
        if (status == att_errors[i] || status == BLE_HS_ATT_ERR(att_errors[i])) {
            return true;
        }
    }
    return false;
}

static void runtime_auth_required(uint16_t status)
{
    ble_investigator_event_t event = {
        .kind = BLE_INVESTIGATOR_EVENT_AUTH_REQUIRED,
        .status = status,
    };
    portENTER_CRITICAL(&s_runtime_lock);
    if (s_runtime.active && s_runtime.core.busy) {
        ble_investigator_handle_event(
            &s_runtime.core, &event, esp_timer_get_time() / 1000);
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    runtime_cleanup();
}

static void uuid_to_text(const ble_uuid_t *uuid, char out[BLE_INV_UUID_LEN])
{
    if (!uuid || !out) return;
    switch (uuid->type) {
    case BLE_UUID_TYPE_16:
        snprintf(out, BLE_INV_UUID_LEN, "%04X", BLE_UUID16(uuid)->value);
        return;
    case BLE_UUID_TYPE_32:
        snprintf(out, BLE_INV_UUID_LEN, "%08lX",
                 (unsigned long)BLE_UUID32(uuid)->value);
        return;
    default:
        ble_uuid_to_str(uuid, out);
        for (size_t i = 0; out[i] != '\0'; ++i) {
            out[i] = (char)toupper((unsigned char)out[i]);
        }
        return;
    }
}

static bool read_is_allowed(const char *service_uuid, const char *uuid)
{
    return strcmp(service_uuid, "1800") == 0 ||
           strcmp(service_uuid, "180A") == 0 ||
           strcmp(uuid, "FFE1") == 0 ||
           strcmp(uuid, "FFF1") == 0;
}

static void runtime_complete(void)
{
    ble_investigator_event_t event = {
        .kind = BLE_INVESTIGATOR_EVENT_DISCOVERY_COMPLETE,
    };
    portENTER_CRITICAL(&s_runtime_lock);
    if (s_runtime.active && s_runtime.core.busy) {
        ble_investigator_handle_event(
            &s_runtime.core, &event, esp_timer_get_time() / 1000);
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    runtime_cleanup();
}

static void start_next_read(void)
{
    if (!runtime_accepts_events()) return;
    if (s_runtime.read_index >= s_runtime.read_count) {
        runtime_complete();
        return;
    }
    if (s_runtime.read_index == 0) emit_progress(BLE_INV_READING);
    runtime_read_t *read = &s_runtime.reads[s_runtime.read_index];
    int rc = ble_gattc_read(s_runtime.conn_handle, read->value_handle,
                            runtime_read_cb, NULL);
    if (rc != 0) runtime_fail(rc, "read_start_failed");
}

static void start_next_characteristic_discovery(void)
{
    if (!runtime_accepts_events()) return;
    if (s_runtime.service_index >= s_runtime.service_count) {
        start_next_read();
        return;
    }
    runtime_service_t *service = &s_runtime.services[s_runtime.service_index];
    int rc = ble_gattc_disc_all_chrs(
        s_runtime.conn_handle,
        service->start_handle,
        service->end_handle,
        runtime_characteristic_cb,
        NULL);
    if (rc != 0) runtime_fail(rc, "characteristic_discovery_failed");
}

static int runtime_service_cb(uint16_t conn_handle,
                              const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *service,
                              void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (!runtime_accepts_events()) return 0;
    uint16_t status = error ? error->status : BLE_HS_EUNKNOWN;
    if (status_requires_authentication(status)) {
        runtime_auth_required(status);
        return 0;
    }
    if (status == BLE_HS_EDONE) {
        s_runtime.service_index = 0;
        start_next_characteristic_discovery();
        return 0;
    }
    if (status != 0 || !service) {
        runtime_fail(status, "service_discovery_failed");
        return 0;
    }

    ble_investigator_event_t event = {
        .kind = BLE_INVESTIGATOR_EVENT_SERVICE,
    };
    uuid_to_text(&service->uuid.u, event.uuid);
    uint8_t before;
    bool accepted = false;
    portENTER_CRITICAL(&s_runtime_lock);
    before = s_runtime.core.result.service_count;
    ble_investigator_handle_event(
        &s_runtime.core, &event, esp_timer_get_time() / 1000);
    if (s_runtime.core.result.service_count > before) {
        s_runtime.services[s_runtime.service_count].start_handle = service->start_handle;
        s_runtime.services[s_runtime.service_count].end_handle = service->end_handle;
        ++s_runtime.service_count;
        accepted = true;
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    if (accepted) emit_service(before, event.uuid);
    runtime_cleanup();
    return 0;
}

static int runtime_characteristic_cb(uint16_t conn_handle,
                                     const struct ble_gatt_error *error,
                                     const struct ble_gatt_chr *characteristic,
                                     void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (!runtime_accepts_events()) return 0;
    uint16_t status = error ? error->status : BLE_HS_EUNKNOWN;
    if (status_requires_authentication(status)) {
        runtime_auth_required(status);
        return 0;
    }
    if (status == BLE_HS_EDONE) {
        ++s_runtime.service_index;
        start_next_characteristic_discovery();
        return 0;
    }
    if (status != 0 || !characteristic ||
        s_runtime.service_index >= s_runtime.service_count) {
        runtime_fail(status, "characteristic_discovery_failed");
        return 0;
    }

    ble_investigator_event_t event = {
        .kind = BLE_INVESTIGATOR_EVENT_CHARACTERISTIC,
        .properties = characteristic->properties,
    };
    const char *service_uuid =
        s_runtime.core.result.services[s_runtime.service_index];
    snprintf(event.service_uuid, sizeof(event.service_uuid), "%s", service_uuid);
    uuid_to_text(&characteristic->uuid.u, event.uuid);

    uint8_t before;
    bool accepted = false;
    portENTER_CRITICAL(&s_runtime_lock);
    before = s_runtime.core.result.characteristic_count;
    ble_investigator_handle_event(
        &s_runtime.core, &event, esp_timer_get_time() / 1000);
    if (s_runtime.core.result.characteristic_count > before &&
        (characteristic->properties & BLE_INV_PROP_READ) != 0 &&
        read_is_allowed(event.service_uuid, event.uuid)) {
        if (s_runtime.read_count < BLE_INV_MAX_READS) {
            runtime_read_t *read = &s_runtime.reads[s_runtime.read_count++];
            read->value_handle = characteristic->val_handle;
            snprintf(read->uuid, sizeof(read->uuid), "%s", event.uuid);
        } else {
            s_runtime.core.result.truncated = true;
        }
    }
    accepted = s_runtime.core.result.characteristic_count > before;
    portEXIT_CRITICAL(&s_runtime_lock);
    if (accepted) {
        emit_characteristic(before, event.service_uuid, event.uuid,
                            event.properties);
    }
    runtime_cleanup();
    return 0;
}

static int runtime_read_cb(uint16_t conn_handle,
                           const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attribute,
                           void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (!runtime_accepts_events()) return 0;
    uint16_t status = error ? error->status : BLE_HS_EUNKNOWN;
    if (status_requires_authentication(status)) {
        runtime_auth_required(status);
        return 0;
    }
    if (status != 0 || !attribute || !attribute->om ||
        s_runtime.read_index >= s_runtime.read_count) {
        runtime_fail(status, "read_failed");
        return 0;
    }

    uint8_t value[(BLE_INV_READ_HEX_LEN - 1) / 2];
    uint16_t packet_len = OS_MBUF_PKTLEN(attribute->om);
    uint16_t value_len = packet_len > sizeof(value) ? sizeof(value) : packet_len;
    if (os_mbuf_copydata(attribute->om, 0, value_len, value) != 0) {
        runtime_fail(BLE_HS_EINVAL, "read_copy_failed");
        return 0;
    }
    ble_investigator_event_t event = {
        .kind = BLE_INVESTIGATOR_EVENT_READ,
        .value = value,
        .value_len = value_len,
    };
    snprintf(event.uuid, sizeof(event.uuid), "%s",
             s_runtime.reads[s_runtime.read_index].uuid);
    uint8_t before;
    bool accepted;
    portENTER_CRITICAL(&s_runtime_lock);
    before = s_runtime.core.result.read_count;
    if (packet_len > value_len) s_runtime.core.result.truncated = true;
    ble_investigator_handle_event(
        &s_runtime.core, &event, esp_timer_get_time() / 1000);
    accepted = s_runtime.core.result.read_count > before;
    portEXIT_CRITICAL(&s_runtime_lock);
    if (accepted) {
        emit_read(before, &s_runtime.core.result.reads[before]);
    }
    ++s_runtime.read_index;
    runtime_cleanup();
    start_next_read();
    return 0;
}

static int runtime_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            portENTER_CRITICAL(&s_runtime_lock);
            bool accepts_failure = s_runtime.active &&
                                   !s_runtime.cleanup_started &&
                                   s_runtime.core.busy;
            if (accepts_failure) s_runtime.connecting = false;
            portEXIT_CRITICAL(&s_runtime_lock);
            if (!accepts_failure) {
                ble_remote_id_resume_after_investigation();
                return 0;
            }
            runtime_fail(event->connect.status, "connect_failed");
            return 0;
        }
        ble_investigator_event_t connected = {
            .kind = BLE_INVESTIGATOR_EVENT_CONNECTED,
        };
        struct ble_gap_conn_desc description;
        bool has_description =
            ble_gap_conn_find(event->connect.conn_handle, &description) == 0;
        portENTER_CRITICAL(&s_runtime_lock);
        if (!s_runtime.active || s_runtime.cleanup_started ||
            !s_runtime.core.busy) {
            portEXIT_CRITICAL(&s_runtime_lock);
            (void)ble_gap_terminate(
                event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            ble_remote_id_resume_after_investigation();
            return 0;
        }
        s_runtime.connecting = false;
        s_runtime.conn_handle = event->connect.conn_handle;
        ble_investigator_handle_event(
            &s_runtime.core, &connected, esp_timer_get_time() / 1000);
        if (has_description) {
            s_runtime.core.result.bonded = description.sec_state.bonded;
            s_runtime.core.result.encrypted = description.sec_state.encrypted;
        }
        portEXIT_CRITICAL(&s_runtime_lock);
        runtime_cleanup();
        if (!runtime_accepts_events()) return 0;
        emit_progress(BLE_INV_DISCOVERING);
        int discovery_rc = ble_gattc_disc_all_svcs(
            s_runtime.conn_handle, runtime_service_cb, NULL);
        if (discovery_rc != 0) {
            runtime_fail(discovery_rc, "service_discovery_failed");
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        if (!runtime_accepts_events()) {
            ble_remote_id_resume_after_investigation();
            return 0;
        }
        ble_investigator_event_t disconnected = {
            .kind = BLE_INVESTIGATOR_EVENT_DISCONNECTED,
            .status = event->disconnect.reason,
        };
        portENTER_CRITICAL(&s_runtime_lock);
        if (s_runtime.active && s_runtime.core.busy) {
            ble_investigator_handle_event(
                &s_runtime.core, &disconnected, esp_timer_get_time() / 1000);
        }
        portEXIT_CRITICAL(&s_runtime_lock);
        runtime_cleanup();
        return 0;
    }

    default:
        return 0;
    }
}

bool ble_investigator_runtime_start(
    const ble_investigation_request_t *request,
    int64_t now_ms)
{
    if (!request) return false;
    uint8_t target_mac[6] = {0};
    if (request->mode == BLE_INV_MODE_GATT &&
        !ble_investigator_parse_target_mac(request->target_mac, target_mac)) {
        return false;
    }

    portENTER_CRITICAL(&s_runtime_lock);
    if (s_runtime.active ||
        !ble_investigator_start(&s_runtime.core, request, now_ms)) {
        portEXIT_CRITICAL(&s_runtime_lock);
        return false;
    }
    s_runtime.active = true;
    s_runtime.cleanup_started = false;
    s_runtime.connecting = false;
    s_runtime.conn_handle = BLE_INV_INVALID_CONN_HANDLE;
    s_runtime.service_count = 0;
    s_runtime.service_index = 0;
    s_runtime.read_count = 0;
    s_runtime.read_index = 0;
    portEXIT_CRITICAL(&s_runtime_lock);

    emit_begin();
    emit_progress(s_runtime.core.state);
    if (request->mode == BLE_INV_MODE_PASSIVE_CAPTURE) {
        return true;
    }

    uint8_t peer_addr_type = BLE_ADDR_PUBLIC;
    (void)ble_remote_id_lookup_peer_addr_type(target_mac, &peer_addr_type);
    if (!ble_remote_id_pause_for_investigation()) {
        runtime_fail(BLE_HS_EBUSY, "scan_pause_failed");
        return true;
    }

    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        runtime_fail(rc, "local_address_unavailable");
        return true;
    }
    ble_addr_t peer_addr = {
        .type = peer_addr_type,
    };
    memcpy(peer_addr.val, target_mac, sizeof(peer_addr.val));
    portENTER_CRITICAL(&s_runtime_lock);
    s_runtime.connecting = true;
    portEXIT_CRITICAL(&s_runtime_lock);
    rc = ble_gap_connect(own_addr_type, &peer_addr,
                         (int32_t)s_runtime.core.request.timeout_ms,
                         NULL, runtime_gap_event, NULL);
    if (rc != 0) {
        portENTER_CRITICAL(&s_runtime_lock);
        s_runtime.connecting = false;
        portEXIT_CRITICAL(&s_runtime_lock);
        runtime_fail(rc, "connect_start_failed");
    }
    return true;
}

void ble_investigator_runtime_tick(int64_t now_ms)
{
    portENTER_CRITICAL(&s_runtime_lock);
    if (s_runtime.active && s_runtime.core.busy) {
        ble_investigator_tick(&s_runtime.core, now_ms);
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    runtime_cleanup();
}

bool ble_investigator_runtime_cancel(const char *request_id, int64_t now_ms)
{
    bool cancelled = false;
    portENTER_CRITICAL(&s_runtime_lock);
    if (s_runtime.active && request_ids_match(request_id)) {
        ble_investigator_cancel(&s_runtime.core, now_ms);
        cancelled = true;
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    if (cancelled) runtime_cleanup();
    return cancelled;
}

bool ble_investigator_runtime_is_busy(void)
{
    return runtime_is_active();
}

bool ble_investigator_runtime_is_gatt_active(void)
{
    bool active;
    portENTER_CRITICAL(&s_runtime_lock);
    active = s_runtime.active &&
             s_runtime.core.request.mode == BLE_INV_MODE_GATT;
    portEXIT_CRITICAL(&s_runtime_lock);
    return active;
}

void ble_investigator_runtime_note_advertisement(
    const uint8_t mac[6],
    const ble_fingerprint_t *fingerprint,
    int8_t rssi,
    uint8_t properties,
    int64_t now_ms)
{
    portENTER_CRITICAL(&s_runtime_lock);
    if (s_runtime.active) {
        ble_investigator_note_advertisement(
            &s_runtime.core, mac, fingerprint, rssi, properties, now_ms);
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    runtime_cleanup();
}

#endif
