#include "badge_ble_investigation.h"

#include <stdio.h>
#include <string.h>

#include "badge_ble_investigation_state.h"
#include "ble_investigation_protocol.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"
#include "serial_config.h"
#include "uart_protocol.h"
#include "uart_rx.h"

static const char *TAG = "badge_ble_inv";

static badge_ble_investigation_state_t s_state;
static badge_ble_investigation_pending_queue_t s_pending_chunks;
static badge_ble_investigation_start_fence_t s_start_fence;
static uint32_t s_revision = 0;
static SemaphoreHandle_t s_lock = NULL;
static SemaphoreHandle_t s_emit_lock = NULL;
static bool s_initialized = false;
static bool s_pending_queue_error = false;
static portMUX_TYPE s_init_lock = portMUX_INITIALIZER_UNLOCKED;

/* Scanner chunk ingress is single-owner UART slot 0; keep its large encoding
 * scratch out of the 8 KB UART task stack. */
static char s_chunk_json[UART_JSON_MAX_SIZE];
static char s_usb_frame[BADGE_BLE_INVESTIGATION_USB_FRAME_MAX];
static ble_investigation_chunk_t s_pending_emit_chunk;

static void set_error(char *err, size_t err_len, const char *message)
{
    if (!err || err_len == 0) return;
    snprintf(err, err_len, "%s", message ? message : "error");
}

static bool lock_state(void)
{
    if (!s_lock) {
        SemaphoreHandle_t created = xSemaphoreCreateMutex();
        if (!created) return false;
        portENTER_CRITICAL(&s_init_lock);
        if (!s_lock) {
            s_lock = created;
            created = NULL;
        }
        portEXIT_CRITICAL(&s_init_lock);
        if (created) vSemaphoreDelete(created);
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return false;
    if (!s_initialized) {
        badge_ble_investigation_state_init(&s_state);
        badge_ble_investigation_pending_queue_init(&s_pending_chunks);
        badge_ble_investigation_start_fence_init(&s_start_fence);
        s_revision = 0;
        s_pending_queue_error = false;
        s_initialized = true;
    }
    return true;
}

static void unlock_state(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}

static bool lock_emit(void)
{
    if (!s_emit_lock) {
        SemaphoreHandle_t created = xSemaphoreCreateMutex();
        if (!created) return false;
        portENTER_CRITICAL(&s_init_lock);
        if (!s_emit_lock) {
            s_emit_lock = created;
            created = NULL;
        }
        portEXIT_CRITICAL(&s_init_lock);
        if (created) vSemaphoreDelete(created);
    }
    return xSemaphoreTake(s_emit_lock, portMAX_DELAY) == pdTRUE;
}

static void unlock_emit(void)
{
    if (s_emit_lock) xSemaphoreGive(s_emit_lock);
}

static int64_t investigation_now_ms(void)
{
    return esp_timer_get_time() / 1000LL;
}

/* Caller holds s_lock. Expiry mutates state and queues independent copies, but
 * deliberately performs no USB output or logging while the state is locked. */
static bool expire_locked(int64_t now_ms)
{
    badge_ble_investigation_expiry_t expiry;
    if (!badge_ble_investigation_state_expire(&s_state, now_ms, &expiry)) {
        return false;
    }
    if (!badge_ble_investigation_pending_queue_enqueue_expiry(
            &s_pending_chunks, &s_state, &expiry)) {
        s_pending_queue_error = true;
    }
    s_revision++;
    return true;
}

/* Caller holds only s_emit_lock. The JSON/frame scratch is shared by scanner
 * ingress and timeout polling, so both routes serialize through this helper. */
static bool emit_chunk_locked(const ble_investigation_chunk_t *chunk)
{
    return chunk &&
        ble_investigation_chunk_to_json(chunk, s_chunk_json,
                                        sizeof(s_chunk_json)) > 0 &&
        badge_ble_investigation_usb_frame(s_chunk_json, s_usb_frame,
                                          sizeof(s_usb_frame)) > 0 &&
        serial_config_emit_investigation_frame(s_usb_frame);
}

void badge_ble_investigation_init(void)
{
    if (lock_state()) unlock_state();
}

void badge_ble_investigation_poll(void)
{
    if (!lock_emit()) return;

    bool queue_error = false;
    for (uint8_t i = 0; i < BADGE_BLE_INVESTIGATION_PENDING_CHUNKS; ++i) {
        if (!lock_state()) break;
        expire_locked(investigation_now_ms());
        bool have_chunk = badge_ble_investigation_pending_queue_peek(
            &s_pending_chunks, &s_pending_emit_chunk);
        queue_error = queue_error || s_pending_queue_error;
        s_pending_queue_error = false;
        unlock_state();

        if (!have_chunk || !emit_chunk_locked(&s_pending_emit_chunk)) break;

        if (!lock_state()) break;
        bool consumed = badge_ble_investigation_pending_queue_consume(
            &s_pending_chunks);
        unlock_state();
        if (!consumed) break;
    }
    unlock_emit();

    if (queue_error) {
        ESP_LOGE(TAG, "BLE investigation timeout frame queue overflow");
    }
}

static bool copy_json_string(const cJSON *root,
                             const char *key,
                             char *out,
                             size_t out_len,
                             bool required)
{
    if (!root || !key || !out || out_len == 0) return false;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!item || cJSON_IsNull(item)) {
        out[0] = '\0';
        return !required;
    }
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    size_t len = strlen(item->valuestring);
    if (len >= out_len || (required && len == 0)) return false;
    memcpy(out, item->valuestring, len + 1);
    return true;
}

static bool json_bool(const cJSON *root, const char *key, bool *out)
{
    if (!root || !key || !out) return false;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!item) {
        *out = false;
        return true;
    }
    if (!cJSON_IsBool(item)) return false;
    *out = cJSON_IsTrue(item);
    return true;
}

static bool parse_properties(const cJSON *root, uint16_t *out)
{
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "properties");
    if (!cJSON_IsArray(items) || !out) return false;
    uint16_t properties = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        if (!cJSON_IsString(item) || !item->valuestring) return false;
        if (strcmp(item->valuestring, "broadcast") == 0) {
            properties |= BLE_INV_PROP_BROADCAST;
        } else if (strcmp(item->valuestring, "read") == 0) {
            properties |= BLE_INV_PROP_READ;
        } else if (strcmp(item->valuestring, "write_without_response") == 0) {
            properties |= BLE_INV_PROP_WRITE_WITHOUT_RESPONSE;
        } else if (strcmp(item->valuestring, "write") == 0) {
            properties |= BLE_INV_PROP_WRITE;
        } else if (strcmp(item->valuestring, "notify") == 0) {
            properties |= BLE_INV_PROP_NOTIFY;
        } else if (strcmp(item->valuestring, "indicate") == 0) {
            properties |= BLE_INV_PROP_INDICATE;
        } else if (strcmp(item->valuestring,
                          "authenticated_signed_writes") == 0) {
            properties |= BLE_INV_PROP_AUTHENTICATED_SIGNED_WRITES;
        } else if (strcmp(item->valuestring, "extended_properties") == 0) {
            properties |= BLE_INV_PROP_EXTENDED_PROPERTIES;
        } else {
            return false;
        }
    }
    *out = properties;
    return true;
}

static bool parse_index(const cJSON *root, int *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "index");
    return cJSON_IsNumber(item) &&
           badge_ble_investigation_index_from_number(item->valuedouble, out);
}

static bool parse_chunk(const cJSON *root, ble_investigation_chunk_t *chunk)
{
    if (!cJSON_IsObject(root) || !chunk) return false;
    memset(chunk, 0, sizeof(*chunk));
    const cJSON *type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char *type = cJSON_IsString(type_item) ? type_item->valuestring : NULL;
    if (!type || !copy_json_string(root, "request_id", chunk->request_id,
                                   sizeof(chunk->request_id), true)) {
        return false;
    }

    if (strcmp(type, MSG_TYPE_BLE_INV_BEGIN) == 0) {
        char mode[24];
        chunk->kind = BLE_INV_CHUNK_BEGIN;
        return copy_json_string(root, "mode", mode, sizeof(mode), true) &&
               ble_investigation_mode_from_name(mode, &chunk->mode) &&
               copy_json_string(root, "target_mac", chunk->target_mac,
                                sizeof(chunk->target_mac), false);
    }
    if (strcmp(type, MSG_TYPE_BLE_INV_PROGRESS) == 0) {
        char state[24];
        chunk->kind = BLE_INV_CHUNK_PROGRESS;
        return copy_json_string(root, "state", state, sizeof(state), true) &&
               ble_investigation_state_from_name(state, &chunk->state) &&
               chunk->state >= BLE_INV_QUEUED && chunk->state <= BLE_INV_READING;
    }
    if (strcmp(type, MSG_TYPE_BLE_INV_SERVICE) == 0) {
        chunk->kind = BLE_INV_CHUNK_SERVICE;
        return parse_index(root, &chunk->index) &&
               copy_json_string(root, "uuid", chunk->uuid,
                                sizeof(chunk->uuid), true) &&
               badge_ble_investigation_uuid_is_canonical(chunk->uuid);
    }
    if (strcmp(type, MSG_TYPE_BLE_INV_CHAR) == 0) {
        chunk->kind = BLE_INV_CHUNK_CHARACTERISTIC;
        return parse_index(root, &chunk->index) &&
               copy_json_string(root, "service_uuid", chunk->service_uuid,
                                sizeof(chunk->service_uuid), true) &&
               copy_json_string(root, "uuid", chunk->uuid,
                                sizeof(chunk->uuid), true) &&
               badge_ble_investigation_uuid_is_canonical(chunk->service_uuid) &&
               badge_ble_investigation_uuid_is_canonical(chunk->uuid) &&
               parse_properties(root, &chunk->properties);
    }
    if (strcmp(type, MSG_TYPE_BLE_INV_READ) == 0) {
        chunk->kind = BLE_INV_CHUNK_READ;
        return parse_index(root, &chunk->index) &&
               copy_json_string(root, "uuid", chunk->uuid,
                                sizeof(chunk->uuid), true) &&
               copy_json_string(root, "value_hex", chunk->value_hex,
                                sizeof(chunk->value_hex), false) &&
               badge_ble_investigation_uuid_is_canonical(chunk->uuid) &&
               badge_ble_investigation_value_hex_is_valid(chunk->value_hex);
    }
    if (strcmp(type, MSG_TYPE_BLE_INV_END) == 0) {
        char state[24];
        chunk->kind = BLE_INV_CHUNK_END;
        return copy_json_string(root, "state", state, sizeof(state), true) &&
               ble_investigation_state_from_name(state, &chunk->state) &&
               chunk->state >= BLE_INV_COMPLETE &&
               chunk->state <= BLE_INV_CANCELLED &&
               copy_json_string(root, "summary", chunk->summary,
                                sizeof(chunk->summary), false) &&
               copy_json_string(root, "error", chunk->error,
                                sizeof(chunk->error), false) &&
               json_bool(root, "authentication_required",
                         &chunk->authentication_required) &&
               json_bool(root, "truncated", &chunk->truncated);
    }
    return false;
}

bool badge_ble_investigation_start(const char *request_id,
                                   const char *mode,
                                   const char *target_mac,
                                   const char *transport,
                                   char *err,
                                   size_t err_len)
{
    set_error(err, err_len, "");
    ble_investigation_request_t request = {0};
    ble_investigation_request_t normalized;
    if (!request_id || !mode || !transport ||
        strlen(request_id) >= sizeof(request.request_id) ||
        !ble_investigation_mode_from_name(mode, &request.mode)) {
        set_error(err, err_len, "invalid_request");
        return false;
    }
    const char *target = target_mac ? target_mac : "";
    if (strlen(target) >= sizeof(request.target_mac)) {
        set_error(err, err_len, "invalid_target");
        return false;
    }
    memcpy(request.request_id, request_id, strlen(request_id) + 1);
    memcpy(request.target_mac, target, strlen(target) + 1);
    request.timeout_ms = BLE_INV_DEFAULT_TIMEOUT_MS;
    if (!badge_ble_investigation_request_validate(&request, &normalized)) {
        set_error(err, err_len, "invalid_request");
        return false;
    }

    char payload[UART_JSON_MAX_SIZE];
    if (ble_investigation_request_to_json(&normalized, payload,
                                          sizeof(payload)) == 0) {
        set_error(err, err_len, "invalid_request");
        return false;
    }
    bool scanner_available = uart_rx_ble_investigation_ingress_available();
    if (!lock_state()) {
        set_error(err, err_len, "busy");
        return false;
    }
    expire_locked(investigation_now_ms());
    if (!scanner_available) {
        set_error(err, err_len, "scanner_unavailable");
        unlock_state();
        return false;
    }
    if (s_start_fence.pending) {
        set_error(err, err_len, "busy");
        unlock_state();
        return false;
    }
    if (!badge_ble_investigation_pending_queue_can_accept_expiry(
            &s_pending_chunks)) {
        set_error(err, err_len, "busy");
        unlock_state();
        return false;
    }
    int scanner_slot = -1;
    if (!badge_ble_investigation_state_start_at(
            &s_state, &normalized, true, investigation_now_ms(),
            &scanner_slot)) {
        set_error(err, err_len, "busy");
        unlock_state();
        return false;
    }
    s_revision++;
    uint32_t generation = badge_ble_investigation_start_fence_reserve(
        &s_start_fence, s_revision);
    unlock_state();

    bool sent = generation != 0 &&
        scanner_slot == BADGE_BLE_INVESTIGATION_SCANNER_SLOT &&
        uart_rx_send_command_to_scanner_checked(scanner_slot, payload);

    if (!lock_state()) {
        set_error(err, err_len, sent ? "busy" : "scanner_unavailable");
        return false;
    }
    bool rollback = badge_ble_investigation_start_fence_should_rollback(
        &s_start_fence, generation, s_revision, sent);
    if (rollback) {
        badge_ble_investigation_state_init(&s_state);
        s_revision++;
    }
    unlock_state();

    if (!sent) {
        set_error(err, err_len, "scanner_unavailable");
        return false;
    }
    ESP_LOGI(TAG, "BLE investigation %s started via %s", request_id, transport);
    return true;
}

bool badge_ble_investigation_start_local(const char *request_id,
                                         const char *mode,
                                         const char *target_mac,
                                         char *err,
                                         size_t err_len)
{
    return badge_ble_investigation_start(request_id, mode, target_mac,
                                         "badge_button", err, err_len);
}

bool badge_ble_investigation_accept_scanner_json(const cJSON *root)
{
    ble_investigation_chunk_t chunk;
    if (!parse_chunk(root, &chunk) || !lock_state()) return false;
    expire_locked(investigation_now_ms());
    uint8_t before_count = s_state.chunk_count;
    bool accepted = badge_ble_investigation_state_accept(&s_state, &chunk);
    bool stored = accepted && (s_state.chunk_count != before_count ||
                               chunk.kind == BLE_INV_CHUNK_END);
    bool have_emitted_chunk = stored && s_state.chunk_count > 0 &&
        badge_ble_investigation_state_get_chunk(
            &s_state, chunk.request_id, s_state.chunk_count - 1,
            &chunk);
    if (accepted) s_revision++;
    unlock_state();

    if (have_emitted_chunk && lock_emit()) {
        emit_chunk_locked(&chunk);
        unlock_emit();
    }
    return accepted;
}

void badge_ble_investigation_get(ble_investigation_result_t *out)
{
    if (!out) return;
    if (!lock_state()) {
        ble_investigation_result_init(out);
        return;
    }
    expire_locked(investigation_now_ms());
    badge_ble_investigation_state_get(&s_state, out);
    unlock_state();
}

size_t badge_ble_investigation_status_json(char *out, size_t out_len)
{
    if (!out || out_len == 0) return 0;
    out[0] = '\0';
    badge_ble_investigation_status_t status;
    if (!lock_state()) return 0;
    expire_locked(investigation_now_ms());
    badge_ble_investigation_state_status(&s_state, &status);
    unlock_state();
    return badge_ble_investigation_status_to_json(&status, out, out_len);
}

bool badge_ble_investigation_chunk_available(const char *request_id, int seq)
{
    ble_investigation_chunk_t chunk;
    if (!lock_state()) return false;
    expire_locked(investigation_now_ms());
    bool available = badge_ble_investigation_state_get_chunk(
        &s_state, request_id, seq, &chunk);
    unlock_state();
    return available;
}

size_t badge_ble_investigation_chunk_json(const char *request_id,
                                          int seq,
                                          char *out,
                                          size_t out_len)
{
    if (!out || out_len == 0) return 0;
    out[0] = '\0';
    ble_investigation_chunk_t chunk;
    if (!lock_state()) return 0;
    expire_locked(investigation_now_ms());
    bool found = badge_ble_investigation_state_get_chunk(
        &s_state, request_id, seq, &chunk);
    unlock_state();
    return found ? ble_investigation_chunk_to_json(&chunk, out, out_len) : 0;
}
