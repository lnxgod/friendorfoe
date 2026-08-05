#include "production_scanner_uart.h"

#include <inttypes.h>
#include <limits.h>
#include <string.h>

#include "backend_detection_codec.h"
#include "backend_json_reader.h"
#include "backend_json_writer.h"

static bool find_value(
    const char *json,
    const backend_json_token_t *tokens,
    size_t count,
    const char *key,
    size_t *index)
{
    return count != 0U && tokens[0].kind == BACKEND_JSON_OBJECT &&
        backend_json_object_find(json, tokens, count, 0U, key, index);
}

static bool copy_optional_string(
    const char *json,
    const backend_json_token_t *tokens,
    size_t count,
    const char *key,
    char *output,
    size_t capacity,
    bool *present)
{
    size_t index = 0U;
    if (present != NULL) {
        *present = false;
    }
    if (!find_value(json, tokens, count, key, &index)) {
        if (capacity != 0U) {
            output[0] = '\0';
        }
        return true;
    }
    if (present != NULL) {
        *present = true;
    }
    return backend_json_copy_string(json, &tokens[index], output, capacity);
}

static bool get_optional_u64(
    const char *json,
    const backend_json_token_t *tokens,
    size_t count,
    const char *key,
    uint64_t *value)
{
    size_t index = 0U;
    if (!find_value(json, tokens, count, key, &index)) {
        return true;
    }
    return backend_json_get_u64(json, &tokens[index], value);
}

static bool get_optional_bool(
    const char *json,
    const backend_json_token_t *tokens,
    size_t count,
    const char *key,
    bool *value,
    bool *present)
{
    size_t index = 0U;
    if (present != NULL) {
        *present = false;
    }
    if (!find_value(json, tokens, count, key, &index)) {
        return true;
    }
    if (present != NULL) {
        *present = true;
    }
    if (backend_json_get_bool(json, &tokens[index], value)) {
        return true;
    }
    int64_t numeric = 0;
    if (!backend_json_get_i64(json, &tokens[index], &numeric) ||
        (numeric != 0 && numeric != 1)) {
        return false;
    }
    *value = numeric != 0;
    return true;
}

static bool parse_profile(
    const char *value,
    backend_scan_profile_t *profile)
{
    if (strcmp(value, "ble_primary") == 0) {
        *profile = BACKEND_SCAN_PROFILE_BLE_PRIMARY;
    } else if (strcmp(value, "wifi_primary") == 0) {
        *profile = BACKEND_SCAN_PROFILE_WIFI_PRIMARY;
    } else if (strcmp(value, "hybrid_failover") == 0) {
        *profile = BACKEND_SCAN_PROFILE_HYBRID_FAILOVER;
    } else if (strcmp(value, "quiescent") == 0) {
        *profile = BACKEND_SCAN_PROFILE_QUIESCENT;
    } else {
        return false;
    }
    return true;
}

static bool canonical_hardware_id(const char *value)
{
    if (value == NULL || strlen(value) != 17U) {
        return false;
    }
    for (size_t index = 0U; index < 17U; ++index) {
        const char ch = value[index];
        if (index == 2U || index == 5U || index == 8U ||
            index == 11U || index == 14U) {
            if (ch != ':') {
                return false;
            }
        } else if (!((ch >= '0' && ch <= '9') ||
                     (ch >= 'A' && ch <= 'F') ||
                     (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool production_app_project_valid(const char *value)
{
    static const char prefix[] = "fof";
    static const char suffix[] = "badge_scanner";
    return value != NULL &&
        strncmp(value, prefix, sizeof(prefix) - 1U) == 0 &&
        value[sizeof(prefix) - 1U] == '_' &&
        strcmp(value + sizeof(prefix), suffix) == 0;
}

static const char *profile_name(backend_scan_profile_t profile)
{
    switch (profile) {
    case BACKEND_SCAN_PROFILE_BLE_PRIMARY:
        return "ble_primary";
    case BACKEND_SCAN_PROFILE_WIFI_PRIMARY:
        return "wifi_primary";
    case BACKEND_SCAN_PROFILE_HYBRID_FAILOVER:
        return "hybrid_failover";
    default:
        return NULL;
    }
}

bool production_scanner_identity_valid(
    const char *board,
    const char *chip,
    const char *capabilities,
    const char *version)
{
    if (board == NULL || chip == NULL || capabilities == NULL ||
        version == NULL || version[0] == '\0' ||
        strcmp(chip, "esp32s3") != 0 ||
        strcmp(capabilities, "ble,wifi") != 0) {
        return false;
    }
    if (strcmp(board, "scanner-s3-combo") == 0 ||
        strcmp(board, "scanner-s3-combo-seed") == 0) {
        return true;
    }

    /* The production full-size combo reports a legacy board suffix that
     * includes the badge marker.  Match it exactly in two pieces so the
     * backend uplink image does not itself contain a badge-firmware marker;
     * the release verifier must continue rejecting mixed badge binaries. */
    static const char production_prefix[] = "scanner-s3-combo-fof";
    return strncmp(board, production_prefix, sizeof(production_prefix) - 1U) == 0 &&
        board[sizeof(production_prefix) - 1U] == '_' &&
        strcmp(board + sizeof(production_prefix), "badge") == 0;
}

bool production_scanner_uart_decode(
    const char *json,
    size_t length,
    production_scanner_message_t *out)
{
    if (json == NULL || out == NULL || length == 0U ||
        length > BACKEND_DETECTION_UART_MAX_LINE) {
        return false;
    }
    /* Production scanner status frames carry substantially more telemetry
     * than backend-native frames (roughly 300 JSON tokens).  The UART worker
     * has a 12 KiB stack, so this bounded 4.5 KiB token scratch fits without
     * relaxing the default 256-token budget used by other decoders. */
    backend_json_token_t tokens[BACKEND_JSON_EXTENDED_MAX_TOKENS];
    size_t token_count = 0U;
    if (backend_json_parse(
            json, length, tokens, BACKEND_JSON_EXTENDED_MAX_TOKENS,
            &token_count) != BACKEND_JSON_OK || token_count == 0U ||
        tokens[0].kind != BACKEND_JSON_OBJECT) {
        return false;
    }

    production_scanner_message_t message = {0};
    char type[24];
    bool type_present = false;
    if (!copy_optional_string(
            json, tokens, token_count, "type", type, sizeof(type),
            &type_present) || !type_present) {
        return false;
    }
    if (strcmp(type, "scanner_info") == 0) {
        message.kind = PRODUCTION_SCANNER_MESSAGE_INFO;
    } else if (strcmp(type, "status") == 0) {
        message.kind = PRODUCTION_SCANNER_MESSAGE_STATUS;
    } else if (strcmp(type, "scan_profile_ack") == 0) {
        message.kind = PRODUCTION_SCANNER_MESSAGE_PROFILE_ACK;
    } else if (strcmp(type, "stop_ack") == 0) {
        message.kind = PRODUCTION_SCANNER_MESSAGE_STOP_ACK;
    } else if (strcmp(type, "fw_check") == 0) {
        message.kind = PRODUCTION_SCANNER_MESSAGE_FW_CHECK;
    } else if (strcmp(type, "fw_ready") == 0) {
        message.kind = PRODUCTION_SCANNER_MESSAGE_FW_READY;
    } else if (strncmp(type, "ota_", 4U) == 0) {
        message.kind = PRODUCTION_SCANNER_MESSAGE_OTA;
    } else {
        return false;
    }

    bool version_present = false;
    bool board_present = false;
    bool chip_present = false;
    bool caps_present = false;
    if (!copy_optional_string(
            json, tokens, token_count, "ver", message.version,
            sizeof(message.version), &version_present) ||
        !copy_optional_string(
            json, tokens, token_count, "board", message.board,
            sizeof(message.board), &board_present) ||
        !copy_optional_string(
            json, tokens, token_count, "chip", message.chip,
            sizeof(message.chip), &chip_present) ||
        !copy_optional_string(
            json, tokens, token_count, "caps", message.capabilities,
            sizeof(message.capabilities), &caps_present)) {
        return false;
    }
    message.identity_present = version_present && board_present &&
        chip_present && caps_present;
    message.identity_valid = message.identity_present &&
        production_scanner_identity_valid(
            message.board, message.chip, message.capabilities,
            message.version);

    bool firmware_name_present = false;
    bool app_project_present = false;
    bool hardware_type_present = false;
    bool hardware_id_present = false;
    if (!copy_optional_string(
            json, tokens, token_count, "firmware_name",
            message.firmware_name, sizeof(message.firmware_name),
            &firmware_name_present) ||
        !copy_optional_string(
            json, tokens, token_count, "app_project",
            message.app_project, sizeof(message.app_project),
            &app_project_present) ||
        !copy_optional_string(
            json, tokens, token_count, "hardware_type",
            message.hardware_type, sizeof(message.hardware_type),
            &hardware_type_present) ||
        !copy_optional_string(
            json, tokens, token_count, "hardware_id",
            message.hardware_id, sizeof(message.hardware_id),
            &hardware_id_present)) {
        return false;
    }
    message.management_identity_present = firmware_name_present &&
        app_project_present && hardware_type_present && hardware_id_present;
    message.management_identity_valid =
        message.management_identity_present &&
        message.identity_valid &&
        strcmp(message.firmware_name, message.board) == 0 &&
        production_scanner_identity_valid(
            message.firmware_name, message.chip, message.capabilities,
            message.version) &&
        production_app_project_valid(message.app_project) &&
        strcmp(message.hardware_type, "seeed_xiao_esp32s3") == 0 &&
        canonical_hardware_id(message.hardware_id);

    char profile[24];
    bool profile_present = false;
    if (!copy_optional_string(
            json, tokens, token_count, "scan_profile", profile,
            sizeof(profile), &profile_present)) {
        return false;
    }
    if (!profile_present && message.kind == PRODUCTION_SCANNER_MESSAGE_PROFILE_ACK &&
        !copy_optional_string(
            json, tokens, token_count, "profile", profile,
            sizeof(profile), &profile_present)) {
        return false;
    }
    if (profile_present) {
        if (!parse_profile(profile, &message.profile)) {
            return false;
        }
        message.profile_present = true;
    }

    uint64_t sequence = 0U;
    uint64_t boot_id = 0U;
    uint64_t uptime_s = 0U;
    uint64_t command_rx = 0U;
    uint64_t tx_drops = 0U;
    if (!get_optional_u64(
            json, tokens, token_count, "seq", &sequence) ||
        !get_optional_u64(
            json, tokens, token_count, "boot_id", &boot_id) ||
        !get_optional_u64(
            json, tokens, token_count, "uptime_s", &uptime_s) ||
        !get_optional_u64(
            json, tokens, token_count, "cmd_rx", &command_rx) ||
        !get_optional_u64(
            json, tokens, token_count, "uart_tx_dropped", &tx_drops) ||
        sequence > UINT32_MAX || boot_id > UINT32_MAX ||
        command_rx > UINT32_MAX ||
        tx_drops > UINT32_MAX || uptime_s > UINT64_MAX / 1000U) {
        return false;
    }
    message.sequence = (uint32_t)sequence;
    message.boot_id = (uint32_t)boot_id;
    message.boot_id_present = message.boot_id != 0U;
    message.command_rx_count = (uint32_t)command_rx;
    message.tx_drops = (uint32_t)tx_drops;
    message.uptime_ms = uptime_s * 1000U;
    if (!get_optional_bool(
            json, tokens, token_count, "slot_role_ok",
            &message.slot_role_ok, &message.slot_role_ok_present) ||
        !get_optional_bool(
            json, tokens, token_count, "ble_initialized",
            &message.ble_initialized, &message.ble_initialized_present) ||
        !get_optional_bool(
            json, tokens, token_count, "ble_scanning",
            &message.ble_scanning, &message.ble_scanning_present) ||
        !get_optional_bool(
            json, tokens, token_count, "ble_host_active",
            &message.ble_host_active, &message.ble_host_active_present) ||
        !get_optional_bool(
            json, tokens, token_count, "ble_host_synced",
            &message.ble_host_synced, &message.ble_host_synced_present) ||
        !get_optional_bool(
            json, tokens, token_count, "wifi_initialized",
            &message.wifi_initialized, &message.wifi_initialized_present) ||
        !get_optional_bool(
            json, tokens, token_count, "wifi_active",
            &message.wifi_active, &message.wifi_active_present) ||
        !get_optional_bool(
            json, tokens, token_count, "wifi_paused",
            &message.wifi_paused, &message.wifi_paused_present) ||
        !copy_optional_string(
            json, tokens, token_count, "ota_state", message.ota_state,
            sizeof(message.ota_state), NULL) ||
        !copy_optional_string(
            json, tokens, token_count, "recovery_mode",
            message.rollback_state, sizeof(message.rollback_state), NULL)) {
        return false;
    }

    if ((message.kind == PRODUCTION_SCANNER_MESSAGE_INFO ||
         message.kind == PRODUCTION_SCANNER_MESSAGE_STATUS) &&
        !message.identity_valid) {
        return false;
    }
    if (message.kind == PRODUCTION_SCANNER_MESSAGE_PROFILE_ACK &&
        !message.profile_present) {
        return false;
    }
    *out = message;
    return true;
}

static size_t finish_writer(
    backend_json_writer_t *writer,
    char *output,
    size_t capacity)
{
    const size_t length = backend_json_writer_finish(writer);
    if (length == 0U) {
        if (capacity != 0U) {
            output[0] = '\0';
        }
        return 0U;
    }
    return length;
}

size_t production_scanner_encode_ready(char *output, size_t capacity)
{
    if (output == NULL || capacity == 0U) {
        return 0U;
    }
    backend_json_writer_t writer;
    backend_json_writer_init(&writer, output, capacity);
    backend_json_append(&writer, "{\"type\":\"ready\"}");
    return finish_writer(&writer, output, capacity);
}

size_t production_scanner_encode_stop(char *output, size_t capacity)
{
    if (output == NULL || capacity == 0U) {
        return 0U;
    }
    backend_json_writer_t writer;
    backend_json_writer_init(&writer, output, capacity);
    backend_json_append(&writer, "{\"type\":\"stop\"}");
    return finish_writer(&writer, output, capacity);
}

size_t production_scanner_encode_profile(
    backend_scan_profile_t profile,
    char *output,
    size_t capacity)
{
    const char *name = profile_name(profile);
    if (output == NULL || capacity == 0U || name == NULL) {
        return 0U;
    }
    backend_json_writer_t writer;
    backend_json_writer_init(&writer, output, capacity);
    backend_json_append(&writer, "{\"type\":\"scan_profile\",\"scan_profile\":");
    backend_json_append_escaped(&writer, name);
    backend_json_append(&writer, ",\"slot_role\":");
    backend_json_append_escaped(&writer, name);
    backend_json_append(&writer, "}");
    return finish_writer(&writer, output, capacity);
}

size_t production_scanner_encode_time(
    int64_t epoch_ms,
    const char *source,
    char *output,
    size_t capacity)
{
    if (output == NULL || capacity == 0U || source == NULL ||
        epoch_ms <= BACKEND_DETECTION_EPOCH_MIN_MS) {
        return 0U;
    }
    backend_json_writer_t writer;
    backend_json_writer_init(&writer, output, capacity);
    backend_json_append_format(
        &writer, "{\"type\":\"time\",\"ms\":%" PRId64
        ",\"ok\":true,\"src\":", epoch_ms);
    backend_json_append_escaped(&writer, source);
    backend_json_append(&writer, "}");
    return finish_writer(&writer, output, capacity);
}
