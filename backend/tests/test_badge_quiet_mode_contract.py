import re
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


def test_badge_button_chord_performs_expected_software_reset_at_10000ms():
    display = _source("esp32", "uplink", "main", "hw", "display_st7735.c")

    assert "BADGE_RESET_CHORD_HOLD_MS 10000" in display
    assert "badge_power_chord_update" in display
    assert "BADGE_POWER_CHORD_RESET" in display
    button_task = display[
        display.index("static void badge_button_task") :
        display.index("static bool badge_buttons_start")
    ]
    assert "badge_usb_transport_host_active(25)" not in button_task
    assert '"USB FLASH?"' in button_task
    assert '"OK=YES"' in button_task
    assert '"MENU=RESET"' in button_task
    assert "#define BADGE_USB_CONFIRM_MS" in display
    assert "BADGE_USB_CONFIRM_MS       5000" in display
    assert "s_usb_recovery_prompt_active" in button_task
    assert "oled_set_power(true)" in button_task
    assert "oled_set_power(false)" not in button_task
    restart_helper = display[
        display.index("static void badge_button_restart_or_resume") :
        display.index("static void badge_button_task")
    ]
    rom_indicator = restart_helper[
        restart_helper.index("if (target == BADGE_USB_RESET_ROM)") :
        restart_helper.index("if (!badge_usb_recovery_restart(")
    ]
    flash_screen = (
        'oled_show_boot_status(\n'
        '            "USB FLASH MODE", "READY FOR HOST", "DO NOT UNPLUG");'
    )
    assert flash_screen in rom_indicator
    assert restart_helper.index('"USB FLASH MODE"') < \
        restart_helper.index("badge_usb_recovery_restart(")
    assert restart_helper.count("badge_usb_recovery_restart(") == 1
    assert "atomic_store(&s_usb_recovery_prompt_active, false)" in \
        restart_helper
    assert button_task.count("badge_usb_recovery_restart(") == 0
    assert button_task.count("badge_button_restart_or_resume(") == 2
    assert "badge_usb_recovery_target(" in button_task
    assert "bool confirmation_handled = false;" in button_task
    assert "if (!confirmation_handled)" in button_task
    assert "if (!host_active)" not in button_task
    assert button_task.index("oled_show_boot_status(\"USB FLASH?\"") < \
        button_task.index("badge_usb_recovery_target(flash_confirmed)")
    oled_update = display[
        display.index("void oled_update(") :
        display.index("void oled_show_detection")
    ]
    assert "atomic_load(&s_usb_recovery_prompt_active)" in oled_update
    assert "BADGE_USB_RESET_ROM" in button_task
    assert '"button_usb_rom"' in button_task
    assert '"button_reboot"' in button_task
    assert "esp_restart();" not in button_task
    assert 'badge_power_runtime_toggle("button_chord")' not in display
    assert 'badge_power_runtime_request(' not in button_task
    assert "buttons[0].consume_release = true" in display
    assert "buttons[1].consume_release = true" in display
    dispatch_edge = display[
        display.index("static void badge_button_dispatch_edge") :
        display.index("static void badge_button_task")
    ]
    stable_press = dispatch_edge[
        dispatch_edge.index("if (edge == BADGE_BUTTON_EDGE_PRESSED)") :
    ]
    assert stable_press.index("badge_easter_egg_claim_press_in_batch") < \
        stable_press.index("badge_power_runtime_is_quiet")


def test_badge_usb_recovery_uses_public_sof_and_one_restart_helper():
    transport = _source(
        "esp32", "uplink", "main", "core", "badge_usb_transport.c"
    )
    recovery = _source(
        "esp32", "uplink", "main", "core", "badge_usb_recovery.c"
    )
    recovery_header = _source(
        "esp32", "uplink", "main", "core", "badge_usb_recovery.h"
    )
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")

    host_sample = transport[
        transport.index("bool badge_usb_transport_host_active") :
    ]
    assert "usb_serial_jtag_is_connected()" in host_sample
    assert "DTR" not in host_sample
    assert "RTS" not in host_sample
    assert "REG_READ" not in host_sample
    assert "RTC_CNTL" not in host_sample
    drain = transport[
        transport.index("bool badge_usb_transport_drain") :
        transport.index("static int transport_log_vprintf")
    ]
    assert "TickType_t started = xTaskGetTickCount()" in drain
    assert "TickType_t elapsed = xTaskGetTickCount() - started" in drain
    assert "timeout - elapsed" in drain
    assert "bool badge_usb_recovery_restart(" in recovery_header
    arm = recovery.index("badge_runtime_arm_expected_reboot(")
    owned = recovery.index(
        "BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_OWNED", arm
    )
    safe_once = recovery.index(
        "badge_runtime_arm_usb_recovery_once()", owned
    )
    assert arm < owned < safe_once
    assert 'strcmp(reason, "usb_safe_once") == 0' in recovery
    assert "badge_runtime_arm_usb_recovery_once()" in recovery
    assert "badge_usb_transport_drain(pdMS_TO_TICKS(250))" in recovery
    assert '"ROM recovery output did not drain; continuing to ROM"' in recovery
    assert 'reason = "usb_rom_drain_failed"' not in recovery
    assert "RTC_CNTL_FORCE_DOWNLOAD_BOOT" in recovery
    assert "oled_show_boot_status" not in recovery
    assert "esp_restart();" in recovery
    bootloader = serial[
        serial.index("static void reboot_to_download_mode") :
        serial.index("static void reboot_app")
    ]
    assert "if (!badge_usb_recovery_restart(" in bootloader
    assert "BADGE_USB_RESET_ROM" in bootloader
    assert '"usb_bootloader"' in bootloader
    assert "RTC_CNTL_FORCE_DOWNLOAD_BOOT" not in serial


def test_badge_safe_usb_is_one_boot_and_health_executor_is_policy_driven():
    runtime = _source(
        "esp32", "uplink", "main", "core", "badge_runtime.c"
    )
    runtime_header = _source(
        "esp32", "uplink", "main", "core", "badge_runtime.h"
    )
    main = _source("esp32", "uplink", "main", "main.c")
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    store_header = _source("esp32", "uplink", "main", "network", "fw_store.h")

    recovery_field = runtime.index("uint32_t usb_recovery_once_magic;")
    rtc_state_start = runtime.rindex("typedef struct {", 0, recovery_field)
    rtc_state_end = runtime.index(
        "} badge_runtime_rtc_state_t;", recovery_field
    )
    rtc_state = runtime[rtc_state_start:rtc_state_end]
    assert "uint32_t usb_recovery_once_magic;" in rtc_state
    assert (
        "_Static_assert(\n"
        "    offsetof(badge_runtime_rtc_state_t, usb_recovery_once_magic) ==\n"
        "        BADGE_RUNTIME_RTC_RECOVERY_OFFSET,\n"
        '    "RTC recovery token ABI moved");'
    ) in runtime
    assert (
        "extern RTC_NOINIT_ATTR badge_runtime_rtc_state_t "
        "g_fof_badge_rtc_state;"
    ) in runtime
    assert runtime.count("RTC_NOINIT_ATTR") == 1
    assert "s_usb_recovery_once_magic" not in runtime
    assert "badge_runtime_recovery_token_decide" in runtime
    assert "BADGE_RUNTIME_NVS_FORCE_SAFE" not in runtime
    assert '"force_safe"' not in runtime
    assert "nvs_get_string_value(BADGE_RUNTIME_NVS_EXPECTED_REASON" in runtime
    init = runtime[
        runtime.index("void badge_runtime_init(bool pending_verify)") :
        runtime.index("void badge_runtime_set_pending_verify(")
    ]
    assert "nvs_erase_key_value(BADGE_RUNTIME_NVS_EXPECTED_REASON)" not in init
    token_decision = re.search(
        r"badge_runtime_recovery_token_action_t\s+token_action\s*=\s*"
        r"badge_runtime_recovery_token_decide\(\s*"
        r"g_fof_badge_rtc_state\.usb_recovery_once_magic\s*==\s*"
        r"BADGE_RUNTIME_USB_RECOVERY_ONCE_MAGIC\s*,\s*"
        r"s_last_reset_class\s*\);",
        init,
    )
    assert token_decision is not None
    clear_recovery = init.index(
        "g_fof_badge_rtc_state.usb_recovery_once_magic = 0U;",
        token_decision.end(),
    )
    consumed = re.search(
        r"s_usb_recovery_once_consumed\s*=\s*"
        r"token_action\s*==\s*"
        r"BADGE_RUNTIME_RECOVERY_TOKEN_CONSUME_SAFE_USB\s*;",
        init[clear_recovery:],
    )
    assert consumed is not None
    assert token_decision.start() < clear_recovery
    assert "badge_runtime_arm_usb_recovery_once(void)" in runtime_header
    assert "badge_runtime_usb_recovery_once_consumed(void)" in runtime_header

    display_start = main.index("oled_init();")
    buttons_start = main.index("oled_badge_buttons_start()")
    assert display_start < buttons_start
    assert buttons_start < main.index("esp_event_loop_create_default()")
    assert buttons_start < main.index("uart_rx_init(detection_queue)")

    watchdog = main[main.index("/* ── 17. Connectivity watchdog") :]
    usb_watchdog = watchdog[
        watchdog.index("badge_runtime_poll();") :
        watchdog.index("badge_runtime_note_main_stack_free")
    ]
    assert "badge_usb_transport_snapshot(&usb_health)" in usb_watchdog
    assert "badge_usb_health_decide(&usb_inputs)" in usb_watchdog
    assert "fw_store_activity_sample()" in usb_watchdog
    assert "fw_store_is_relay_active()" not in usb_watchdog
    assert "fw_store_last_relay_progress_ms()" in usb_watchdog
    assert "FW_STORE_ACTIVITY_UNKNOWN" in usb_watchdog
    snapshot = usb_watchdog.index("badge_usb_transport_snapshot(&usb_health)")
    activity = usb_watchdog.index("fw_store_activity_sample()")
    progress = usb_watchdog.index("fw_store_last_relay_progress_ms()")
    fresh_now = usb_watchdog.index(
        "int64_t usb_now_ms = esp_timer_get_time() / 1000"
    )
    decision = usb_watchdog.index("badge_usb_health_decide(&usb_inputs)")
    assert snapshot < activity < fresh_now
    assert snapshot < progress < fresh_now
    assert fresh_now < decision
    assert ".now_ms = usb_now_ms" in usb_watchdog
    assert ".now_ms = now_ms" not in usb_watchdog
    unknown_guard = usb_watchdog.index(
        "if (firmware_activity != FW_STORE_ACTIVITY_UNKNOWN)"
    )
    assert unknown_guard < decision
    assert "badge_usb_health_action_t usb_action = BADGE_USB_HEALTH_WAITING" \
        in usb_watchdog
    assert "BADGE_USB_HEALTH_RESTART_SAFE_USB" in watchdog
    restart = watchdog[
        watchdog.index("if (usb_action == BADGE_USB_HEALTH_RESTART_SAFE_USB)") :
        watchdog.index("badge_runtime_note_main_stack_free")
    ]
    assert "badge_usb_recovery_prepare_firmware_restart(" in restart
    assert "BADGE_USB_FIRMWARE_RESTART_PREPARE_OWNED" in restart
    assert "fw_store_try_reserve_recovery_restart()" not in restart
    assert "fw_store_activity_sample()" not in restart
    safe_restart_pattern = (
        r"badge_usb_recovery_restart_with_owned_lease\(\s*"
        r"BADGE_USB_RESET_APP\s*,\s*"
        r'"usb_safe_once"\s*,\s*'
        r"&reboot_lease\s*\)"
    )
    safe_restart = re.search(safe_restart_pattern, restart)
    assert safe_restart is not None
    assert (
        restart.index("BADGE_USB_FIRMWARE_RESTART_PREPARE_OWNED")
        < safe_restart.start()
    )
    assert len(re.findall(safe_restart_pattern, watchdog)) == 1
    assert "badge_runtime_usb_control_recovery_due" not in watchdog
    assert "badge_runtime_force_safe_mode" not in restart
    assert "fw_store_last_relay_progress_ms(void)" in store_header
    assert "fw_store_activity_t fw_store_activity_sample(void)" in store_header
    assert "bool fw_store_try_reserve_recovery_restart(void)" in store_header
    for state in (
        "FW_STORE_ACTIVITY_INACTIVE",
        "FW_STORE_ACTIVITY_ACTIVE",
        "FW_STORE_ACTIVITY_UNKNOWN",
    ):
        assert state in store_header
    assert "s_last_relay_progress_ms" in store
    assert "atomic_int_fast64_t s_last_relay_progress_ms" not in store
    relay_progress = store[
        store.index("static void relay_emit_progress") :
        store.index("#else", store.index("static void relay_emit_progress"))
    ]
    relay_progress_getter = store[
        store.index("int64_t fw_store_last_relay_progress_ms(void)") :
        store.index("static void auto_capture_identity_floors")
    ]
    progress_note = store[
        store.index("static void fw_store_note_relay_progress(void)") :
        store.index("bool fw_store_operation_try_begin")
    ]
    assert "portENTER_CRITICAL(&s_operation_lock)" in progress_note
    assert "portEXIT_CRITICAL(&s_operation_lock)" in progress_note
    assert "fw_store_note_relay_progress();" in relay_progress
    assert "portENTER_CRITICAL(&s_operation_lock)" in relay_progress_getter
    assert "portEXIT_CRITICAL(&s_operation_lock)" in relay_progress_getter
    health_wait = store[
        store.index("static bool wait_for_scanner_post_update_health") :
        store.index("static bool badge_candidate_seen")
    ]
    assert "+ 180000" in health_wait
    assert "fw_store_note_relay_progress();" in health_wait
    assert health_wait.index("fw_store_note_relay_progress();") < \
        health_wait.index("vTaskDelay(pdMS_TO_TICKS(500))")
    assert "badge_runtime_note_usb_control_alive();" not in health_wait


def test_badge_rollback_requires_completed_ping_or_status_and_reports_reason():
    runtime = _source(
        "esp32", "uplink", "main", "core", "badge_runtime.c"
    )
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")
    main = _source("esp32", "uplink", "main", "main.c")
    uart_rx = _source("esp32", "uplink", "main", "comms", "uart_rx.c")

    assert "badge_runtime_note_usb_response_completed" in serial
    assert "CMD_PING" in serial and "CMD_STATUS" in serial
    assert "badge_runtime_normal_stability_satisfied" in runtime
    assert "badge_runtime_uart_heartbeat_fresh" in runtime
    assert "badge_runtime_rollback_health_satisfied" in runtime
    assert "s_usb_response_completed" in runtime
    assert "badge_runtime_note_scanner_uart_worker_alive((uint8_t)scanner_id)" in uart_rx
    assert "badge_runtime_note_scanner_uart_alive" not in main
    watchdog = main[main.index("/* ── 17. Connectivity watchdog") :]
    stable_gate = watchdog[
        watchdog.index("if (badge_runtime_health_can_mark_stable(") :
        watchdog.index("if (badge_runtime_health_can_mark_ota_valid(")
    ]
    ota_gate = watchdog[
        watchdog.index("if (badge_runtime_health_can_mark_ota_valid(") :
        watchdog.index('ESP_LOGI(TAG, "WATCHDOG:')
    ]
    assert "badge_runtime_mark_stable();" in stable_gate
    assert "rollback_mark_valid();" not in stable_gate
    assert "rollback_mark_valid();" in ota_gate
    assert "badge_runtime_mark_stable();" not in ota_gate
    assert "uart_rx_is_scanner_connected()" not in watchdog

    assert "static portMUX_TYPE s_runtime_health_lock" in runtime
    display_note = runtime[
        runtime.index("void badge_runtime_note_display_alive") :
        runtime.index("void badge_runtime_note_usb_control_alive")
    ]
    response_note = runtime[
        runtime.index("void badge_runtime_note_usb_response_completed") :
        runtime.index("void badge_runtime_note_scanner_uart_worker_alive")
    ]
    uart_note = runtime[
        runtime.index("void badge_runtime_note_scanner_uart_worker_alive") :
        runtime.index("void badge_runtime_note_display_stack_free")
    ]
    for writer, field in (
        (display_note, "s_display_alive = true"),
        (response_note, "s_usb_response_completed = true"),
        (uart_note, "s_scanner_uart_last_ms[scanner_id] ="),
    ):
        assert "portENTER_CRITICAL(&s_runtime_health_lock)" in writer
        assert "portEXIT_CRITICAL(&s_runtime_health_lock)" in writer
        assert writer.index("portENTER_CRITICAL") < writer.index(field) < \
            writer.index("portEXIT_CRITICAL")

    snapshot = runtime[
        runtime.index("static void runtime_health_snapshot") :
        runtime.index("bool badge_runtime_health_can_mark_stable")
    ]
    assert "portENTER_CRITICAL(&s_runtime_health_lock)" in snapshot
    assert "out->display_alive = s_display_alive" in snapshot
    assert "out->usb_response_completed = s_usb_response_completed" in snapshot
    assert "out->scanner_uart_last_ms[i] = s_scanner_uart_last_ms[i]" in snapshot
    assert "portEXIT_CRITICAL(&s_runtime_health_lock)" in snapshot

    stable = runtime[
        runtime.index("bool badge_runtime_health_can_mark_stable") :
        runtime.index("bool badge_runtime_health_can_mark_ota_valid")
    ]
    ota = runtime[
        runtime.index("bool badge_runtime_health_can_mark_ota_valid") :
        runtime.index("void badge_runtime_mark_stable")
    ]
    status_reader = runtime[
        runtime.index("bool badge_runtime_scanner_uart_alive") :
        runtime.index("uint32_t badge_runtime_display_stack_free")
    ]
    for reader in (stable, ota, status_reader):
        assert "runtime_health_snapshot(&health)" in reader
        assert "s_scanner_uart_last_ms" not in reader
    assert "health.display_alive" in stable
    assert "health.display_alive" in ota
    assert "health.usb_response_completed" in ota
    assert "last_expected_reboot_reason" in serial
    assert "badge_runtime_last_expected_reboot_reason()" in serial
    display_notes = [
        index
        for index in range(len(main))
        if main.startswith("badge_runtime_note_display_alive();", index)
    ]
    # Maintenance adds a radio-free display fast path alongside the existing
    # quiet and normal dashboard paths. Every path proves the panel is
    # actually powered before publishing liveness.
    assert len(display_notes) == 3
    maintenance_display = main[
        main.index("if (s_badge_update_maintenance_boot)") :
        main.index("/* Gather current state */")
    ]
    assert "badge_runtime_note_display_alive();" in maintenance_display
    for index in display_notes:
        guard = main[max(0, index - 100):index]
        assert "if (oled_is_powered()) {" in guard


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
    assert "uart_rx_get_scanner_info_snapshot" in runtime
    assert "s_power_scanner_snapshots[scanner_id]" in runtime
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
        main.index("badge_usb_transport_set_dispatch_ready()")
    operation_begin = store[store.index("bool fw_store_operation_try_begin") :]
    operation_end = store[store.index("bool fw_store_operation_end") :]
    assert "uart_rx_scanner_tx_lease_acquire" in operation_begin.split("\n}", 1)[0]
    assert "uart_rx_scanner_tx_lease_release" in operation_end.split("\n}", 1)[0]


def test_uplink_accepts_quiet_ack_and_repairs_control_ack_allowlist():
    rx = _source("esp32", "uplink", "main", "comms", "uart_rx.c")
    registry = _source(
        "esp32", "shared", "scanner_uplink_ingress_registry.c"
    )
    firmware_registry = _source(
        "esp32", "shared", "firmware_json_schema_registry.c"
    )
    for message_type in ('"display_policy_ack"', "MSG_TYPE_SCANNER_QUIET_ACK"):
        assert message_type in registry
    assert '"stop_ack"' in firmware_registry
    assert "FOF_FW_JSON_SCHEMA_RECEIPT_STOP_ACK_SHARED" in firmware_registry
    assert "FOF_SCANNER_UPLINK_ROUTE_SCANNER_QUIET_ACK" in registry
    assert "decision.route == FOF_SCANNER_UPLINK_ROUTE_SCANNER_QUIET_ACK" in rx
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
    registry = _source(
        "esp32", "uplink", "main", "core", "badge_usb_control_schema.c"
    )

    assert '"power_mode", "power_mode", POWER_MODE' in registry
    assert "BADGE_USB_CONTROL_HANDLER_POWER_MODE" in registry
    assert "BADGE_USB_CONTROL_HANDLER_POWER_MODE" in serial
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
