import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "esp32/uplink/main/comms/uart_rx.c"


def _source() -> str:
    return SOURCE.read_text(encoding="utf-8")


def _between(source: str, start: str, end: str) -> str:
    start_at = source.index(start)
    return source[start_at : source.index(end, start_at)]


def test_scanner_frames_are_authorized_before_json_or_liveness_effects():
    source = _source()
    handler = _between(
        source,
        "static void process_line",
        "/* ── UART RX task",
    )

    crud_at = handler.index('memcmp(line, "FOF_CRUD:"')
    easter_at = handler.index("badge_easter_egg_source_from_uart_frame")
    authorize_at = handler.index(
        "fof_scanner_uplink_ingress_select_and_validate"
    )
    parse_at = handler.index("cJSON_ParseWithOpts")
    activity_at = handler.index("note_scanner_activity")
    assert crud_at < easter_at < authorize_at < parse_at < activity_at

    assert "fof_scanner_uplink_decision_t" in handler
    for route in (
        "FOF_SCANNER_UPLINK_ROUTE_BLE_INVESTIGATION",
        "FOF_SCANNER_UPLINK_ROUTE_DETECTION",
        "FOF_SCANNER_UPLINK_ROUTE_STATUS",
        "FOF_SCANNER_UPLINK_ROUTE_SCANNER_INFO",
        "FOF_SCANNER_UPLINK_ROUTE_FIRMWARE",
    ):
        assert route in handler
    assert 'strncmp(msg_type, "ble_inv_", 8)' not in handler
    assert 'strncmp(msg_type, "ota_", 4)' not in handler


def test_game_observer_preempts_existing_ble_detectors_without_callback_work():
    source = (
        ROOT / "esp32/scanner/main/detection/ble_remote_id.c"
    ).read_text(encoding="utf-8")
    callback = _between(
        source,
        "static int ble_gap_event_cb",
        "static void ble_host_task",
    )

    legacy = _between(
        callback,
        "case BLE_GAP_EVENT_DISC:",
        "badge_ble_note_any_packet(disc->rssi",
    )
    extended = _between(
        callback,
        "case BLE_GAP_EVENT_EXT_DISC:",
        "badge_ble_note_any_packet(ext->rssi",
    )
    for prefix in (legacy, extended):
        assert "badge_con_observer_consume" in prefix
        assert "BADGE_CON_FRAME_NOT_GAME" in prefix
        assert "return 0" in prefix
        for forbidden in (
            "cJSON",
            "malloc",
            "calloc",
            "realloc",
            "xQueueCreate",
            "xSemaphoreCreate",
            "uart_write_bytes",
            "ESP_LOG",
        ):
            assert forbidden not in prefix


def test_badge_low_effort_ble_uses_one_minus_50_scanner_and_lcd_gate():
    scanner = (
        ROOT / "esp32/scanner/main/detection/ble_remote_id.c"
    ).read_text(encoding="utf-8")
    uart = (
        ROOT / "esp32/uplink/main/comms/uart_rx.c"
    ).read_text(encoding="utf-8")
    display = (
        ROOT / "esp32/uplink/main/hw/display_st7735.c"
    ).read_text(encoding="utf-8")

    assert '#include "badge_ble_rssi_policy.h"' in scanner
    assert '#include "badge_ble_rssi_policy.h"' in uart
    assert '#include "badge_ble_rssi_policy.h"' in display

    unknown_candidate = _between(
        scanner,
        "static bool badge_ble_is_privacy_candidate",
        "static const char *badge_ble_privacy_reason",
    )
    assert "badge_ble_low_effort_rssi_allowed(rssi)" in unknown_candidate
    assert "rssi >= -58" not in unknown_candidate
    assert "rssi >= -72" not in unknown_candidate

    unknown_emit = _between(
        scanner,
        "static bool badge_ble_unknown_diag_should_emit",
        "static bool badge_ble_should_emit_detection",
    )
    assert "!badge_ble_low_effort_rssi_allowed(rssi)" in unknown_emit
    assert "rssi < -72" not in unknown_emit

    emit_policy = _between(
        scanner,
        "static bool badge_ble_should_emit_detection",
        "#if CONFIG_FOF_GLASSES_DETECTION",
    )
    assert re.search(
        r"badge_ble_low_effort_detection_allowed\(\s*"
        r"fp->device_type == BLE_DEV_UNKNOWN,\s*rssi\)",
        emit_policy,
    )
    assert "is_calibration_beacon || is_focus_target" not in emit_policy

    odid = _between(
        scanner,
        "static void process_odid_service_data",
        "static void trace_ble_service_data",
    )
    assert "badge_ble_low_effort_rssi_allowed" not in odid
    assert "enqueue_odid_detection_priority(&det)" in odid

    uart_detection = _between(
        uart,
        "if (decision.route == FOF_SCANNER_UPLINK_ROUTE_DETECTION)",
        "} else if (decision.route == FOF_SCANNER_UPLINK_ROUTE_STATUS)",
    )
    assert '"BLE Nearby"' in uart_detection
    assert '"Unknown"' in uart_detection
    assert "badge_ble_low_effort_detection_allowed(" in uart_detection
    assert uart_detection.index("badge_ble_low_effort_detection_allowed(") < (
        uart_detection.index("badge_ingest_detection(")
    )

    assert "badge_ingest_ble_near_status_event" not in uart
    status_evidence = _between(
        uart,
        "static void badge_ingest_scanner_status_evidence",
        "static void push_recent",
    )
    assert "badge_ingest_ble_near_status_event" not in status_evidence

    live = _between(
        display,
        "static const scanner_info_t *scanner_status_best_ble_live_info",
        "static void format_ble_signal_status",
    )
    assert "badge_ble_low_effort_rssi_allowed(" in live
    assert "info->ble_any_last_rssi" in live
    assert "info->ble_any_best_rssi > best->ble_any_best_rssi" not in live

    formatter = _between(
        display,
        "static void format_ble_signal_status",
        "static void draw_scanner_health_line",
    )
    assert 'snprintf(label_out, label_len, "BLE SIGNAL")' in formatter
    assert "info->ble_any_last_rssi" in formatter
    assert "info->ble_dbg_near_rssi" not in formatter

    assert "ble_live->ble_any_best_rssi >= -60" not in display
    assert len(
        re.findall(
            r"badge_ble_low_effort_rssi_allowed\(\s*"
            r"ble_live->ble_any_last_rssi\)",
            display,
        )
    ) == 2


def test_scanner_game_wiring_is_canary_only_and_keeps_uart_priority_gates():
    scanner_main = (
        ROOT / "esp32/scanner/main/main.c"
    ).read_text(encoding="utf-8")
    uart_tx = (
        ROOT / "esp32/scanner/main/comms/uart_tx.c"
    ).read_text(encoding="utf-8")
    cmake = (
        ROOT / "esp32/scanner/main/CMakeLists.txt"
    ).read_text(encoding="utf-8")

    assert "FOF_SCANNER_COMMAND_CRUD_SELF" in scanner_main
    assert "badge_con_observer_set_self" in scanner_main
    assert "badge_con_render_self_ack" in scanner_main
    assert "uart_tx_set_firmware_quiet_window" in scanner_main

    game_drain = _between(
        uart_tx,
        "static void uart_tx_maybe_send_badge_con_packet",
        "static void uart_tx_task",
    )
    for gate in (
        "scanner_data_tx_allowed()",
        "uart_ota_is_active_snapshot()",
        "uart_tx_firmware_quiet_window_active()",
        "uxQueueMessagesWaiting(detection_queue)",
        "badge_con_observer_take_pending",
        "badge_con_render_uart_line",
    ):
        assert gate in game_drain

    canary_sources = _between(
        cmake,
        'if("$ENV{PIOENV}" STREQUAL '
        '"scanner-s3-combo-fof_badge-con-crud-canary")',
        "endif()",
    )
    assert "detection/badge_con_observer.c" in canary_sources
    assert "../../shared/badge_con_protocol.c" in canary_sources
    assert "../../shared/badge_con_encounter.c" in canary_sources


def test_scanner_uart_task_uses_strict_framer_and_stale_discard():
    source = _source()
    task = _between(
        source,
        "static void uart_rx_task",
        "/* Static params",
    )

    assert "scanner_uart_line_framer_init" in task
    assert "scanner_uart_line_framer_consume" in task
    assert "scanner_uart_line_framer_expire_partial" in task
    assert "SCANNER_UART_LINE_EVENT_FRAME_READY" in task
    assert "SCANNER_UART_LINE_EVENT_FRAME_REJECTED" in task
    assert "UART_MSG_DELIMITER" not in task
    assert "line_pos" not in task


def test_every_json_sink_authorizes_before_uart_write():
    source = _source()
    broadcast = _between(
        source,
        "void uart_rx_send_command(",
        "bool uart_rx_send_command_to_scanner_checked",
    )
    checked = _between(
        source,
        "bool uart_rx_send_command_to_scanner_checked",
        "void uart_rx_send_command_to_scanner(",
    )

    for handler in (broadcast, checked):
        authorize_at = handler.index("fof_scanner_command_select_and_validate")
        write_at = handler.index("send_json_line_to_scanner_locked")
        assert authorize_at < write_at

    assert "decision.command.id" in broadcast
    assert "strstr(json_cmd" not in broadcast
    # One definition plus the two authorized broadcast writes and the
    # authorized single-scanner write. No internal helper may bypass them.
    assert source.count("send_json_line_to_scanner_locked(") == 4
