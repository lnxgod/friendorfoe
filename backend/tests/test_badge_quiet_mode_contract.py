from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def _source(*parts: str) -> str:
    return (REPO_ROOT.joinpath(*parts)).read_text()


def test_badge_panel_sleep_uses_st7735_sleep_commands_and_skips_redraw():
    display = _source("esp32", "uplink", "main", "hw", "display_st7735.c")
    header = _source("esp32", "uplink", "main", "hw", "oled_display.h")

    assert "ST_CMD_DISPOFF" in display
    assert "ST_CMD_SLPIN" in display
    assert "void oled_set_power(bool on)" in display
    assert "bool oled_is_powered(void)" in display
    assert "if (!s_initialized || !oled_is_powered()) return;" in display
    assert display.count("if (!oled_is_powered()) {") >= 3
    assert "void oled_set_power(bool on);" in header


def test_badge_button_chord_toggles_volatile_power_runtime_at_9000ms():
    display = _source("esp32", "uplink", "main", "hw", "display_st7735.c")

    assert "BADGE_POWER_CHORD_HOLD_MS 9000" in display
    assert "badge_power_chord_update" in display
    assert 'badge_power_runtime_toggle("button_chord")' in display
    assert "buttons[0].consume_release = true" in display
    assert "buttons[1].consume_release = true" in display
    poll_one = display[
        display.index("static void badge_button_poll_one") :
        display.index("static void badge_button_task")
    ]
    stable_press = poll_one[poll_one.index("if (button->stable_pressed)") :]
    assert stable_press.index("badge_easter_egg_runtime_dismiss") < \
        stable_press.index("badge_power_runtime_is_quiet")


def test_badge_power_runtime_keeps_uart_up_and_reasserts_after_relay():
    runtime = _source(
        "esp32", "uplink", "main", "core", "badge_power_runtime.c"
    )

    assert "MSG_TYPE_SCANNER_QUIET" in runtime
    assert '\\"generation\\"' in runtime
    assert "fw_store_is_relay_active" in runtime
    assert "uart_rx_send_command_to_scanner_checked" in runtime
    assert "esp_deep_sleep" not in runtime
    assert "esp_light_sleep" not in runtime
    assert "badge_power_state_still_matches" in runtime
    assert "uart_rx_scanner_tx_lease_acquire" in runtime
    assert "uart_rx_scanner_tx_lease_release" in runtime
    assert "s_transition_mutex" in runtime
    assert "BADGE_POWER_PENDING_RETRY_MS 500" in runtime
    assert "scanner.connected && !scanner.acked" in runtime
    request = runtime[
        runtime.index("bool badge_power_runtime_request") :
        runtime.index("bool badge_power_runtime_toggle")
    ]
    poll = runtime[runtime.index("void badge_power_runtime_poll") :]
    assert "xSemaphoreTake" in request
    assert "xSemaphoreTake" in poll


def test_quiet_reassert_uses_known_identity_after_rx_freshness_expires():
    runtime = _source(
        "esp32", "uplink", "main", "core", "badge_power_runtime.c"
    )

    assert "scanner_identity_known" in runtime
    assert "scanner_target_known" in runtime
    assert "uart_rx_get_ble_scanner_info" in runtime
    assert "uart_rx_get_wifi_scanner_info" in runtime
    relay_finished = runtime[
        runtime.index("static void note_relay_finished") :
        runtime.index("void badge_power_runtime_poll")
    ]
    assert "badge_power_state_note_disconnected" in relay_finished
    poll = runtime[runtime.index("void badge_power_runtime_poll") :]
    assert "scanner_target_known" in poll
    assert "badge_power_state_note_disconnected" in poll
    assert "badge_power_state_note_identity" in poll


def test_firmware_operations_hold_a_recursive_scanner_uart_lease():
    rx = _source("esp32", "uplink", "main", "comms", "uart_rx.c")
    rx_header = _source("esp32", "uplink", "main", "comms", "uart_rx.h")
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    main = _source("esp32", "uplink", "main", "main.c")

    assert "xSemaphoreCreateRecursiveMutex" in rx
    assert "xSemaphoreTakeRecursive" in rx
    assert "xSemaphoreGiveRecursive" in rx
    assert "uart_rx_scanner_tx_lease_acquire" in rx
    assert "uart_rx_scanner_tx_lease_release" in rx
    assert "uart_rx_scanner_tx_lease_acquire" in rx_header
    assert "uart_rx_scanner_tx_lease_release" in rx_header
    assert "uart_rx_scanner_tx_lease_init" in rx_header
    assert main.index("uart_rx_scanner_tx_lease_init()") < \
        main.index("serial_config_listen(3000)")
    operation_begin = store[store.index("static bool operation_try_begin") :]
    operation_end = store[store.index("static void operation_end") :]
    assert "uart_rx_scanner_tx_lease_acquire" in operation_begin.split("\n}", 1)[0]
    assert "uart_rx_scanner_tx_lease_release" in operation_end.split("\n}", 1)[0]


def test_uplink_accepts_quiet_ack_and_repairs_control_ack_allowlist():
    rx = _source("esp32", "uplink", "main", "comms", "uart_rx.c")

    allowlist = rx[
        rx.index("static bool msg_type_is_scanner_originated") :
        rx.index("void uart_rx_set_node_calibration_mode")
    ]
    for message_type in (
        '"stop_ack"',
        '"display_policy_ack"',
        "MSG_TYPE_SCANNER_QUIET_ACK",
    ):
        assert message_type in allowlist
    assert "badge_power_runtime_note_scanner_ack" in rx
    assert "badge_power_runtime_note_scanner_identity" in rx
    assert '"tx_enabled"' in rx
    for fact in (
        '"ble_quiesced"',
        '"wifi_quiesced"',
        '"ble_active"',
        '"wifi_active"',
        '"radios_ready"',
        '"tx_restored"',
    ):
        assert fact in rx
    assert "transition_ok" in rx
    assert "quiet_ack_fields_valid" in rx


def test_uplink_backpressure_never_sends_legacy_start_while_quiet():
    rx = _source("esp32", "uplink", "main", "comms", "uart_rx.c")

    resume = rx[rx.index("static void maybe_resume_scanner") :]
    assert resume.count("badge_power_runtime_is_quiet()") >= 2


def test_badge_quiet_mode_suppresses_ready_profiles_and_display_work():
    main = _source("esp32", "uplink", "main", "main.c")

    assert "badge_power_runtime_is_quiet()" in main
    assert "badge_power_runtime_start()" in main
    assert "if (badge_power_runtime_is_quiet())" in main
    assert "!badge_power_runtime_is_quiet()" in main


def test_usb_status_and_control_expose_badge_power_convergence():
    serial = _source(
        "esp32", "uplink", "main", "core", "serial_config.c"
    )

    assert 'strcmp(cmd, "power_mode") == 0' in serial
    assert "badge_power_runtime_request" in serial
    assert '\\\"power_mode\\\"' in serial
    assert '\\\"power_generation\\\"' in serial
    assert '\\\"power_converged\\\"' in serial
    assert '\\\"tx_enabled\\\"' in serial
    assert '\\\"transition_ok\\\"' in serial
    assert '\\\"ble_quiesced\\\"' in serial
    assert '\\\"wifi_quiesced\\\"' in serial
    assert '\\\"radios_ready\\\"' in serial
    assert '\\\"tx_restored\\\"' in serial
    assert "scanner_power_converged" in serial
    assert "oled_is_powered() == !power_state.quiet" in serial
