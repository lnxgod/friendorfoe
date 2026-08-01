#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FOF_SCANNER_PRODUCER_JSON_CAPACITY 192U

bool fof_scanner_wifi_lockon_command_json(
    int32_t channel,
    int32_t duration_s,
    const char *bssid,
    char *output,
    size_t output_capacity);

bool fof_scanner_ble_lockon_command_json(
    const char *mac,
    int32_t duration_s,
    char *output,
    size_t output_capacity);

bool fof_scanner_calibration_start_command_json(
    const char *session_id,
    const char *calibration_uuid,
    char *output,
    size_t output_capacity);

bool fof_scanner_calibration_stop_command_json(
    const char *session_id,
    char *output,
    size_t output_capacity);

bool fof_scanner_display_full_command_json(
    bool button_enabled,
    const char *view,
    int32_t page,
    bool page_lock,
    bool auto_page,
    char *output,
    size_t output_capacity);

bool fof_scanner_display_button_command_json(
    bool button_enabled,
    char *output,
    size_t output_capacity);

#ifdef __cplusplus
}
#endif
