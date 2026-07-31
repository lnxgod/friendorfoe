#include "scanner_command_producer_policy.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mac_address_policy.h"

static bool producer_output_begin(char *output, size_t output_capacity)
{
    if (!output || output_capacity == 0U) {
        return false;
    }
    output[0] = '\0';
    return true;
}

static bool producer_format(
    char *output,
    size_t output_capacity,
    const char *format,
    ...)
{
    if (!output || output_capacity == 0U || !format) {
        return false;
    }

    va_list args;
    va_start(args, format);
    int written = vsnprintf(output, output_capacity, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= output_capacity) {
        output[0] = '\0';
        return false;
    }
    return true;
}

static bool wifi_lockon_duration_is_valid(int32_t duration_s)
{
    return duration_s == 30 || duration_s == 45 ||
           duration_s == 60 || duration_s == 90;
}

static bool lower_hex_exact(const char *value, size_t expected_length)
{
    if (!value) {
        return false;
    }
    for (size_t i = 0U; i < expected_length; ++i) {
        char ch = value[i];
        if (!((ch >= '0' && ch <= '9') ||
              (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return value[expected_length] == '\0';
}

static bool text_has_exact_length(
    const char *value,
    size_t expected_length)
{
    if (!value) {
        return false;
    }
    for (size_t i = 0U; i < expected_length; ++i) {
        if (value[i] == '\0') {
            return false;
        }
    }
    return value[expected_length] == '\0';
}

static bool calibration_uuid_matches_session(
    const char *calibration_uuid,
    const char *session_id)
{
    return text_has_exact_length(calibration_uuid, 36U) &&
           lower_hex_exact(session_id, 12U) &&
           memcmp(calibration_uuid, "cafe", 4U) == 0 &&
           memcmp(calibration_uuid + 4U, session_id, 4U) == 0 &&
           memcmp(
               calibration_uuid + 8U,
               "-0000-1000-8000-",
               16U) == 0 &&
           memcmp(calibration_uuid + 24U, session_id, 12U) == 0 &&
           calibration_uuid[36] == '\0';
}

static bool display_view_is_valid(const char *view)
{
    return view &&
           (strcmp(view, "privacy") == 0 ||
            strcmp(view, "prv") == 0 ||
            strcmp(view, "glasses") == 0 ||
            strcmp(view, "rf") == 0 ||
            strcmp(view, "activity") == 0 ||
            strcmp(view, "drone") == 0 ||
            strcmp(view, "wifi") == 0);
}

bool fof_scanner_wifi_lockon_command_json(
    int32_t channel,
    int32_t duration_s,
    const char *bssid,
    char *output,
    size_t output_capacity)
{
    if (!producer_output_begin(output, output_capacity)) {
        return false;
    }

    char normalized[FOF_MAC_CANONICAL_BUFFER_SIZE];
    if (channel < 1 || channel > 13 ||
        !wifi_lockon_duration_is_valid(duration_s) ||
        !fof_mac_normalize(bssid, true, normalized)) {
        return false;
    }

    return producer_format(
        output,
        output_capacity,
        "{\"type\":\"lockon\",\"ch\":%" PRId32
        ",\"dur\":%" PRId32 ",\"bssid\":\"%s\"}",
        channel,
        duration_s,
        normalized);
}

bool fof_scanner_ble_lockon_command_json(
    const char *mac,
    int32_t duration_s,
    char *output,
    size_t output_capacity)
{
    if (!producer_output_begin(output, output_capacity)) {
        return false;
    }

    char normalized[FOF_MAC_CANONICAL_BUFFER_SIZE];
    if (duration_s != 45 ||
        !fof_mac_normalize(mac, false, normalized)) {
        return false;
    }

    return producer_format(
        output,
        output_capacity,
        "{\"type\":\"ble_lockon\",\"mac\":\"%s\",\"dur\":45}",
        normalized);
}

bool fof_scanner_calibration_start_command_json(
    const char *session_id,
    const char *calibration_uuid,
    char *output,
    size_t output_capacity)
{
    if (!producer_output_begin(output, output_capacity) ||
        !calibration_uuid_matches_session(calibration_uuid, session_id)) {
        return false;
    }

    return producer_format(
        output,
        output_capacity,
        "{\"type\":\"cal_mode_start\",\"session_id\":\"%s\","
        "\"calibration_uuid\":\"%s\"}",
        session_id,
        calibration_uuid);
}

bool fof_scanner_calibration_stop_command_json(
    const char *session_id,
    char *output,
    size_t output_capacity)
{
    if (!producer_output_begin(output, output_capacity) ||
        (!lower_hex_exact(session_id, 12U) &&
         (!session_id || strcmp(session_id, "stale") != 0))) {
        return false;
    }

    return producer_format(
        output,
        output_capacity,
        "{\"type\":\"cal_mode_stop\",\"session_id\":\"%s\"}",
        session_id);
}

bool fof_scanner_display_full_command_json(
    bool button_enabled,
    const char *view,
    int32_t page,
    bool page_lock,
    bool auto_page,
    char *output,
    size_t output_capacity)
{
    if (!producer_output_begin(output, output_capacity) ||
        !display_view_is_valid(view)) {
        return false;
    }

    return producer_format(
        output,
        output_capacity,
        "{\"type\":\"display_control\",\"button_enabled\":%s,"
        "\"view\":\"%s\",\"page\":%" PRId32 ","
        "\"page_lock\":%s,\"auto_page\":%s}",
        button_enabled ? "true" : "false",
        view,
        page,
        page_lock ? "true" : "false",
        auto_page ? "true" : "false");
}

bool fof_scanner_display_button_command_json(
    bool button_enabled,
    char *output,
    size_t output_capacity)
{
    if (!producer_output_begin(output, output_capacity)) {
        return false;
    }

    return producer_format(
        output,
        output_capacity,
        "{\"type\":\"display_control\",\"button_enabled\":%s}",
        button_enabled ? "true" : "false");
}
