#include "badge_ble_investigation.h"

#include <stdio.h>
#include <string.h>

#include "badge_ble_investigation_state.h"
#include "ble_investigation_protocol.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"
#include "uart_protocol.h"
#include "uart_rx.h"

static const char *TAG = "badge_ble_inv";

static badge_ble_investigation_state_t s_state;
static SemaphoreHandle_t s_lock = NULL;
static bool s_initialized = false;
static portMUX_TYPE s_init_lock = portMUX_INITIALIZER_UNLOCKED;

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
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    if (!s_initialized) {
        badge_ble_investigation_state_init(&s_state);
        s_initialized = true;
    }
    return true;
}

static void unlock_state(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}

void badge_ble_investigation_init(void)
{
    if (lock_state()) unlock_state();
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
    const cJSON *index = cJSON_GetObjectItemCaseSensitive(root, "index");
    if (strcmp(type, MSG_TYPE_BLE_INV_SERVICE) == 0) {
        chunk->kind = BLE_INV_CHUNK_SERVICE;
        if (!cJSON_IsNumber(index)) return false;
        chunk->index = index->valueint;
        return copy_json_string(root, "uuid", chunk->uuid,
                                sizeof(chunk->uuid), true);
    }
    if (strcmp(type, MSG_TYPE_BLE_INV_CHAR) == 0) {
        chunk->kind = BLE_INV_CHUNK_CHARACTERISTIC;
        if (!cJSON_IsNumber(index)) return false;
        chunk->index = index->valueint;
        return copy_json_string(root, "service_uuid", chunk->service_uuid,
                                sizeof(chunk->service_uuid), true) &&
               copy_json_string(root, "uuid", chunk->uuid,
                                sizeof(chunk->uuid), true) &&
               parse_properties(root, &chunk->properties);
    }
    if (strcmp(type, MSG_TYPE_BLE_INV_READ) == 0) {
        chunk->kind = BLE_INV_CHUNK_READ;
        if (!cJSON_IsNumber(index)) return false;
        chunk->index = index->valueint;
        return copy_json_string(root, "uuid", chunk->uuid,
                                sizeof(chunk->uuid), true) &&
               copy_json_string(root, "value_hex", chunk->value_hex,
                                sizeof(chunk->value_hex), false);
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
    if (!request_id || !mode || !transport ||
        strlen(request_id) == 0 || strlen(request_id) >= sizeof(request.request_id) ||
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

    char payload[UART_JSON_MAX_SIZE];
    if (ble_investigation_request_to_json(&request, payload,
                                          sizeof(payload)) == 0) {
        set_error(err, err_len, "invalid_request");
        return false;
    }
    if (!lock_state()) {
        set_error(err, err_len, "busy");
        return false;
    }

    badge_ble_investigation_state_t next = s_state;
    int scanner_slot = -1;
    bool available = uart_rx_ble_investigation_ingress_available();
    bool prepared = badge_ble_investigation_state_start(
        &next, &request, available, &scanner_slot);
    if (!prepared) {
        set_error(err, err_len, available ? "busy" : "scanner_unavailable");
        unlock_state();
        return false;
    }
    if (scanner_slot != BADGE_BLE_INVESTIGATION_SCANNER_SLOT ||
        !uart_rx_send_command_to_scanner_checked(scanner_slot, payload)) {
        set_error(err, err_len, "scanner_unavailable");
        unlock_state();
        return false;
    }
    s_state = next;
    ESP_LOGI(TAG, "BLE investigation %s started via %s", request_id, transport);
    unlock_state();
    return true;
}

bool badge_ble_investigation_start_local(const char *request_id,
                                         const char *mode,
                                         const char *target_mac,
                                         char *err,
                                         size_t err_len)
{
    return badge_ble_investigation_start(request_id, mode, target_mac,
                                         "local_button", err, err_len);
}

bool badge_ble_investigation_accept_scanner_json(const cJSON *root)
{
    ble_investigation_chunk_t chunk;
    if (!parse_chunk(root, &chunk) || !lock_state()) return false;

    bool accepted = badge_ble_investigation_state_accept(&s_state, &chunk);
    if (accepted) {
        char json[UART_JSON_MAX_SIZE];
        if (ble_investigation_chunk_to_json(&chunk, json, sizeof(json)) > 0) {
            printf("FOF_INV:%s\n", json);
            fflush(stdout);
        }
    }
    unlock_state();
    return accepted;
}

void badge_ble_investigation_get(ble_investigation_result_t *out)
{
    if (!out) return;
    if (!lock_state()) {
        ble_investigation_result_init(out);
        return;
    }
    badge_ble_investigation_state_get(&s_state, out);
    unlock_state();
}

size_t badge_ble_investigation_status_json(char *out, size_t out_len)
{
    if (!out || out_len == 0) return 0;
    out[0] = '\0';
    if (!lock_state()) return 0;
    size_t len = badge_ble_investigation_state_status_json(
        &s_state, out, out_len);
    unlock_state();
    return len;
}

bool badge_ble_investigation_select_chunk(const char *request_id, int seq)
{
    if (!lock_state()) return false;
    bool selected = badge_ble_investigation_state_select_chunk(
        &s_state, request_id, seq);
    unlock_state();
    return selected;
}

size_t badge_ble_investigation_selected_chunk_json(char *out, size_t out_len)
{
    if (!out || out_len == 0) return 0;
    out[0] = '\0';
    if (!lock_state()) return 0;
    ble_investigation_chunk_t chunk;
    size_t len = badge_ble_investigation_state_get_selected_chunk(
        &s_state, &chunk)
        ? ble_investigation_chunk_to_json(&chunk, out, out_len)
        : 0;
    unlock_state();
    return len;
}
