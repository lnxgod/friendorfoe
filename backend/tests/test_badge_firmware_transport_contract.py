from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def _source(*parts: str) -> str:
    return (REPO_ROOT.joinpath(*parts)).read_text()


def test_badge_runtime_reports_usb_reset_reason_by_name():
    runtime = _source("esp32", "uplink", "main", "core", "badge_runtime.c")

    assert 'case ESP_RST_USB:       return "usb";' in runtime


def test_scanner_ota_end_and_abort_are_bound_to_the_active_session():
    main = _source("esp32", "scanner", "main", "main.c")

    assert "uart_ota_session_id()" in main
    assert 'strcmp(request_session, active_session) == 0' in main
    assert r'\"session_mismatch\"' in main


def test_scanners_check_once_after_a_delayed_boot_and_not_periodically():
    main = _source("esp32", "scanner", "main", "main.c")

    assert "FW_CHECK_BOOT_DELAY_MS" in main
    assert "8LL * 1000LL" in main
    assert "boot_fw_check_due_ms" in main
    assert "boot_fw_check_sent" in main
    assert 'send_fw_check(s_board_name, s_caps, "boot")' in main
    assert "FW_CHECK_PERIODIC_INTERVAL_MS" not in main
    assert "FW_CHECK_JITTER_MAX_MS" not in main
    assert "FW_UPDATE_RETRY_INTERVAL_MS" not in main
    assert 'send_fw_check(s_board_name, s_caps, "periodic")' not in main
    assert 'send_fw_check(s_board_name, s_caps, "pending_update_retry")' not in main
    assert "FW_CHECK_DAILY_INTERVAL_MS" not in main


def test_manual_checks_cannot_interleave_with_relay_and_uplink_has_no_timer():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    header = _source("esp32", "uplink", "main", "network", "fw_store.h")
    main = _source("esp32", "uplink", "main", "main.c")
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")

    assert "fw_store_request_scanner_checks" in header
    helper = store[store.index("uint8_t fw_store_request_scanner_checks") :]
    assert "operation_is_active()" in helper
    assert "http_upload_is_paused()" in helper
    assert '"type\\":\\"fw_check_now\\"' in helper
    assert "fw_store_request_scanner_checks" not in main
    assert "UPLINK_SCANNER_FW_CHECK_INTERVAL_MS" not in main
    assert "periodic_fw_info" not in main
    assert 'strcmp(cmd, "fw_check_now")' in serial
    assert 'strcmp(cmd, "fw_check")' in serial
    assert "firmware_operation_active" in serial


def test_terminal_transient_refusal_can_be_rearmed_by_a_newer_check():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    helper_start = store.index("static bool auto_reopen_terminal_for_newer_check")
    helper_end = store.index("static void auto_reset_ready_queue_after_revalidation_failure", helper_start)
    helper = store[helper_start:helper_end]
    assert "FW_AUTO_SLOT_REFUSED" in helper
    assert "FW_AUTO_SLOT_FAILED" in helper
    assert "FW_AUTO_RELAY_MAX_ATTEMPTS" in helper
    assert "FW_AUTO_SLOT_AWAITING_CHECK" in helper
    assert 'strcmp(check_reason, "periodic")' not in helper
    assert 'strcmp(check_reason, "boot")' in helper
    assert 'strcmp(check_reason, "manual")' in helper
    assert 'strcmp(check_reason, "pending_update_retry")' not in helper
    assert "auto_coordinator_save_locked" in helper

    handler = store[
        store.index("void fw_store_handle_scanner_check") :
        store.index("bool fw_store_handle_scanner_ready")
    ]
    assert "auto_reopen_terminal_for_newer_check" in handler
    relation_branch = handler[handler.index("if (relation == FOF_VERSION_NEWER)") :]
    assert relation_branch.index("auto_reopen_terminal_for_newer_check") < relation_branch.index(
        "auto_coordinator_record_scanner_check"
    )


def test_uplink_treats_the_full_coordinator_worker_window_as_relay_busy():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    start = store.index("bool fw_store_is_relay_active(void)")
    end = store.index("\n}", start) + 2
    helper = store[start:end]
    assert "operation_is_active()" in helper
    assert "s_auto_relay_worker_running" in helper
    assert "auto_coordinator_lock()" in helper


def test_auto_relay_worker_has_stack_for_manifest_bound_uart_protocol():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    assert "#define FW_AUTO_RELAY_TASK_STACK_SIZE 12288" in store
    assert 'xTaskCreate(fw_auto_relay_task, "fw_auto_relay",\n'
    assert "FW_AUTO_RELAY_TASK_STACK_SIZE" in store[
        store.index('xTaskCreate(fw_auto_relay_task, "fw_auto_relay",') :
    ]
    assert "uxTaskGetStackHighWaterMark(NULL)" in store


def test_duplicate_exact_fw_ready_is_idempotent_while_queue_owns_it():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    helper_start = store.index("static bool auto_ready_receipt_is_idempotent")
    helper_end = store.index("\n}", helper_start) + 2
    helper = store[helper_start:helper_end]
    assert "FW_AUTO_SLOT_READY_QUEUED" in helper
    assert "FW_AUTO_SLOT_RELAYING" in helper
    assert "manifest_crc32" in helper
    assert "bound_hardware_id" in helper
    assert "s_auto_ready_bindings" in helper
    assert "identity_generation" in helper

    handler_start = store.index("bool fw_store_handle_scanner_ready")
    handler_end = store.index("bool fw_store_handle_legacy_scanner_ready", handler_start)
    handler = store[handler_start:handler_end]
    assert handler.index("auto_ready_receipt_is_idempotent") < handler.index(
        "enqueue_auto_relay"
    )


def test_uplink_ota_end_carries_the_immutable_session_and_manifest():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    assert r'\"type\":\"ota_end\"' in store
    assert r'\"session_id\":\"%s\"' in store
    assert r'\"sha256\":\"%s\"' in store
    assert r'\"generation\":%lu' in store


def test_uart_relay_waits_for_rx_task_pause_barrier_before_direct_reads():
    uart_rx = _source("esp32", "uplink", "main", "comms", "uart_rx.c")
    uart_header = _source("esp32", "uplink", "main", "comms", "uart_rx.h")
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    # Merely setting a shared pause flag races the background RX task: it can
    # consume stop_ack before the relay begins its direct UART read.  Require a
    # task-owned acknowledgement barrier, and fail closed if it is not reached.
    assert "atomic_bool s_rx_pause_acked_ble" in uart_rx
    assert "atomic_bool s_rx_pause_acked_wifi" in uart_rx
    assert "atomic_store_explicit(pause_acked, true" in uart_rx
    assert "atomic_load_explicit(pause_acked" in uart_rx
    assert "bool uart_rx_pause_scanner(int scanner_id)" in uart_header
    relay = store[store.index("static bool fw_relay_stored_to_scanner") :]
    pause = relay.index("uart_rx_pause_scanner(scanner_id)")
    direct_read = relay.index("relay_wait_for_with_resend(", pause)
    assert pause < direct_read
    assert "uart_rx_pause_timeout" in relay[pause:direct_read]


def test_uart_relay_command_probe_is_bound_to_a_fresh_profile_ack():
    uart_rx = _source("esp32", "uplink", "main", "comms", "uart_rx.c")
    uart_header = _source("esp32", "uplink", "main", "comms", "uart_rx.h")
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    ack_branch = uart_rx[
        uart_rx.index('} else if (strcmp(msg_type, "scan_profile_ack") == 0') :
        uart_rx.index('} else if (strcmp(msg_type, "display_control_ack") == 0')
    ]
    health = store[
        store.index("typedef struct {", store.index("fw_command_health_t") - 400) :
        store.index("static bool probe_scanner_command_ingress")
    ]

    assert "uint32_t scan_profile_ack_generation" in uart_header
    assert "info->scan_profile_ack_generation++" in ack_branch
    assert "scan_profile_ack_generation" in health
    assert "after->scan_profile_ack_generation !=" in health
    # A periodic status frame can report a recent command age even when the
    # just-sent probe never arrived.  Age alone is not a receipt.
    assert "return after->cmd_age_s >= 0" not in health


def test_badge_sends_unconditional_slot_roles_before_usb_config_window():
    main = _source("esp32", "uplink", "main", "main.c")
    helper_start = main.index("static void send_badge_boot_slot_roles")
    helper_end = main.index("static void send_badge_scan_profiles", helper_start)
    helper = main[helper_start:helper_end]

    assert "uart_rx_send_command_to_scanner_checked(0" in helper
    assert "uart_rx_send_command_to_scanner_checked(1" in helper
    assert "fof_policy_slot_role_for_slot(0)" in helper
    assert "fof_policy_slot_role_for_slot(1)" in helper
    assert "uart_rx_is_ble_scanner_connected" not in helper
    assert "uart_rx_is_wifi_scanner_connected" not in helper

    app = main[main.index("void app_main(void)") :]
    uart_init = app.index("uart_rx_init(detection_queue)")
    boot_roles = app.index("send_badge_boot_slot_roles()", uart_init)
    usb_window = app.index("serial_config_listen(3000)")
    assert uart_init < boot_roles < usb_window


def test_badge_runtime_never_demotes_fixed_scanner_slots_to_hybrid():
    main = _source("esp32", "uplink", "main", "main.c")
    helper = main[
        main.index("static void send_badge_scan_profiles") :
        main.index("#endif", main.index("static void send_badge_scan_profiles"))
    ]

    # A scanner can reboot independently while its peer remains healthy.  The
    # badge has fixed physical BLE/Wi-Fi slots, so treating the first peer seen
    # as a single-scanner hybrid makes it initialize both radios and can exhaust
    # internal DMA memory before the second identity frame arrives.
    assert '"hybrid_failover"' not in helper
    assert "both_connected" not in helper
    assert "fof_policy_scan_profile_for_slot(0, false)" in helper
    assert "fof_policy_scan_profile_for_slot(1, false)" in helper


def test_every_badge_profile_publisher_uses_fixed_topology_policy():
    header = _source("esp32", "shared", "detection_policy.h")
    policy = _source("esp32", "shared", "detection_policy.c")
    assert "FOF_POLICY_FIXED_SLOT_TOPOLOGY" in header
    assert "fof_policy_scan_profile_for_topology" in header
    assert "fixed_slot_topology || peer_connected" in policy

    for path in (
        ("esp32", "uplink", "main", "comms", "http_upload.c"),
        ("esp32", "uplink", "main", "network", "fw_store.c"),
        ("esp32", "uplink", "main", "network", "http_status.c"),
        ("esp32", "uplink", "main", "core", "serial_config.c"),
        ("esp32", "uplink", "main", "hw", "display_st7735.c"),
    ):
        source_text = _source(*path)
        assert "fof_policy_scan_profile_for_topology" in source_text, path


def test_relay_resend_reader_preserves_partial_lines_and_flushes_old_backlog():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    # Control receipts may straddle the short polling slices used to schedule
    # resends.  The decoder state must survive those slices, including while it
    # drains an oversized scanner_info line left in the UART FIFO.
    resend = store[
        store.index("static int relay_wait_for_with_resend") :
        store.index("static int relay_poll_nack")
    ]
    assert "relay_line_reader_t reader = {0}" in resend
    assert "relay_read_line_stateful(" in resend

    relay = store[store.index("static bool fw_relay_stored_to_scanner") :]
    pause = relay.index("uart_rx_pause_scanner(scanner_id)")
    flush = relay.index("uart_flush_input(uart_num)", pause)
    first_stop = relay.index("relay_wait_for_with_resend(", pause)
    assert pause < flush < first_stop


def test_scanner_raw_control_json_uses_checked_complete_uart_send():
    tx = _source("esp32", "scanner", "main", "comms", "uart_tx.c")
    raw = tx[
        tx.index("void uart_tx_send_raw_json") :
        tx.index("static void uart_send_line", tx.index("void uart_tx_send_raw_json"))
    ]
    assert "uart_send_line_internal(json_str, false)" in raw
    assert "uart_write_bytes(UART_PORT_NUM, json_str, len)" not in raw


def test_display_receives_connectivity_not_radio_health_as_peer_presence():
    main = _source("esp32", "uplink", "main", "main.c")
    display = main[main.index("static void display_task") : main.index("void app_main")]
    call = display[display.index("oled_update(") :]

    assert "detection_count, ble_connected, wifi_connected" in call
    assert "detection_count, ble_ok, wifi_scan_ok" not in call


def test_profile_convergence_requires_the_opposing_radio_to_be_quiesced():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    policy_header = _source("esp32", "shared", "detection_policy.h")
    uart_header = _source("esp32", "uplink", "main", "comms", "uart_rx.h")
    scanner_tx = _source("esp32", "scanner", "main", "comms", "uart_tx.c")

    proof = store[
        store.index("static bool scanner_profile_radio_proved") :
        store.index("static bool scanner_post_update_converged")
    ]
    assert "scanner->ble_quiesced" in proof
    assert "scanner->wifi_quiesced" in proof
    assert "ble_proved && scanner->wifi_quiesced" in proof
    assert "wifi_proved && scanner->ble_quiesced" in proof
    assert "bool ble_quiesced" in policy_header
    assert "bool wifi_quiesced" in policy_header
    assert "bool     ble_quiesced" in uart_header
    assert "bool     wifi_quiesced" in uart_header
    assert r'\"ble_quiesced\":%s' in scanner_tx
    assert r'\"wifi_quiesced\":%s' in scanner_tx


def test_scanner_waits_for_exact_staged_receipt_before_manifest_bound_finalize():
    ota = _source("esp32", "scanner", "main", "comms", "uart_ota.c")
    ota_header = _source("esp32", "scanner", "main", "comms", "uart_ota.h")
    scanner_main = _source("esp32", "scanner", "main", "main.c")
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    # Reaching total_size must only enter a staged/awaiting-finalize state.
    # The scanner must not commit flash until the separately framed ota_end
    # command has been session- and manifest-validated by main.c.
    assert "OTA_AWAITING_FINALIZE" in ota
    assert "uart_ota_is_receiving_binary" in ota_header
    assert "uart_ota_is_receiving_binary()" in scanner_main
    assert "uart_ota_manifest_matches_active" in scanner_main
    assert "uart_ota_finalize()" in scanner_main

    # The uplink waits for an exact receipt before sending ota_end.  This
    # creates a framing barrier, so the final binary chunk and JSON command
    # cannot be coalesced into the scanner's binary-only read path.
    assert '"ota_staged"' in ota
    assert '"ota_staged"' in store
    assert "relay_line_matches_manifest_ack" in store


def test_scanner_discards_stale_duplicate_frame_before_remaining_size_check():
    ota = _source("esp32", "scanner", "main", "comms", "uart_ota.c")

    # A delayed duplicate NACK can make the uplink resend a chunk the scanner
    # already accepted.  Near the short final remainder, reject-by-sequence and
    # consume that whole stale frame before applying the active-sequence bounds
    # check; otherwise a recoverable duplicate aborts the entire session.
    basic_len = ota.index("if (clen == 0 || clen > OTA_CHUNK_MAX_DATA)")
    seq_mismatch = ota.index("if (seq != s_ota.expected_seq)", basic_len)
    active_overrun = ota.index(
        "if (s_ota.received > s_ota.total_size ||",
        basic_len,
    )
    assert basic_len < seq_mismatch < active_overrun


def test_fw_ready_is_an_exact_auto_update_manifest_receipt():
    scanner_main = _source("esp32", "scanner", "main", "main.c")
    uplink_rx = _source("esp32", "uplink", "main", "comms", "uart_rx.c")

    # Automatic readiness is never an authorization for a same-version
    # recovery rewrite, and every integer is parsed without truncation.
    assert r'\"allow_same_version\":false' in scanner_main
    ready_branch = uplink_rx[
        uplink_rx.index("} else if (strcmp(msg_type, MSG_TYPE_FW_READY) == 0") :
    ]
    common_parser = uplink_rx[
        uplink_rx.index("static bool fw_ready_common_fields_valid") :
        uplink_rx.index("static void json_copy_string")
    ]
    assert common_parser.count("json_get_uint32_exact(") == 2
    assert ready_branch.count("json_get_uint32_exact(") >= 1
    assert 'root, "allow_same_version")' in ready_branch
    assert "cJSON_IsFalse(allow_same_j)" in ready_branch


def test_legacy_identity_snapshot_is_current_complete_and_mutex_protected():
    header = _source("esp32", "uplink", "main", "comms", "uart_rx.h")
    uplink_rx = _source("esp32", "uplink", "main", "comms", "uart_rx.c")

    assert "scanner_identity_snapshot_t" in header
    assert "uart_rx_get_scanner_identity_snapshot" in header
    for field in (
        "firmware_name",
        "app_project",
        "hardware_type",
        "hardware_id",
        "identity_generation",
        "received_ms",
        "complete",
    ):
        assert field in header

    assert "StaticSemaphore_t s_scanner_identity_mutex_storage" in uplink_rx
    assert "xSemaphoreCreateMutexStatic" in uplink_rx
    assert "publish_scanner_identity_snapshot" in uplink_rx
    assert "scanner_identity_frame_string" in uplink_rx
    assert "scanner_identity_hardware_id_is_canonical" in uplink_rx
    assert "snapshot.complete" in uplink_rx
    assert "snapshot.identity_generation" in uplink_rx
    assert "snapshot.received_ms" in uplink_rx
    publisher = uplink_rx[
        uplink_rx.index("static uint32_t publish_scanner_identity_snapshot") :
        uplink_rx.index("static int64_t scanner_status_ssid_age_s")
    ]
    assert "portMAX_DELAY" in publisher
    getter = uplink_rx[
        uplink_rx.index("bool uart_rx_get_scanner_identity_snapshot") :
        uplink_rx.index("ota_response_t uart_rx_get_last_ota_response")
    ]
    assert "xSemaphoreTake" in getter
    assert "xSemaphoreGive" in getter
    assert "*out =" in getter


def test_post_update_reboot_proof_uses_a_new_nonzero_scanner_boot_id():
    scanner_tx = _source("esp32", "scanner", "main", "comms", "uart_tx.c")
    uplink_header = _source("esp32", "uplink", "main", "comms", "uart_rx.h")
    uplink_rx = _source("esp32", "uplink", "main", "comms", "uart_rx.c")
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")
    http = _source("esp32", "uplink", "main", "network", "http_status.c")

    # One random, non-zero value is generated once at scanner boot and carried
    # by both detailed and compact scanner_info plus ordinary status frames.
    assert "s_scanner_boot_id" in scanner_tx
    init = scanner_tx[
        scanner_tx.index("void uart_tx_init") :
        scanner_tx.index("void uart_tx_send_detection")
    ]
    assert "esp_random()" in init
    assert "s_scanner_boot_id == 0" in init
    scanner_info = scanner_tx[
        scanner_tx.index("void uart_tx_send_scanner_info") :
        scanner_tx.index("/* \u2500\u2500 UART TX Task")
    ]
    assert scanner_info.count(r'\"boot_id\":%lu') >= 2
    status = scanner_tx[
        scanner_tx.index("void uart_tx_send_status") :
        scanner_tx.index("void uart_tx_send_scanner_info")
    ]
    assert 'cJSON_AddNumberToObject(root, "boot_id"' in status

    # The full identity snapshot and the live scanner view both clear missing
    # or malformed boot IDs rather than retaining stale proof.
    assert uplink_header.count("uint32_t boot_id;") >= 2
    publisher = uplink_rx[
        uplink_rx.index("static uint32_t publish_scanner_identity_snapshot") :
        uplink_rx.index("static int64_t scanner_status_ssid_age_s")
    ]
    assert 'json_get_uint32_exact(root, "boot_id"' in publisher
    assert "snapshot.boot_id" in publisher
    scanner_info_parser = uplink_rx[
        uplink_rx.index('} else if (strcmp(msg_type, "scanner_info") == 0') :
        uplink_rx.index('} else if (strcmp(msg_type, "recovery_ack")')
    ]
    assert 'json_get_uint32_exact(root, "boot_id"' in scanner_info_parser
    assert "info->boot_id = 0" in scanner_info_parser
    status_parser = uplink_rx[
        uplink_rx.index("static void handle_status") :
        uplink_rx.index("static void uart_rx_task")
    ]
    assert 'json_get_uint32_exact(root, "boot_id"' in status_parser
    assert "info->boot_id = 0" in status_parser

    # Relay convergence is authorized by the scanner's boot epoch, not only by
    # an uplink-local frame counter that can be consumed while RX is paused.
    assert "pre_update_boot_id" in store
    convergence = store[
        store.index("static bool scanner_post_update_converged") :
        store.index("static bool wait_for_scanner_post_update_health")
    ]
    assert "fof_firmware_post_reboot_boot_id_proved" in convergence
    assert "const scanner_identity_snapshot_t *identity" in convergence
    assert "identity->complete" in convergence
    assert "identity->identity_generation <= identity_generation_before" in convergence
    assert "scanner->boot_id != identity->boot_id" in convergence
    health = store[
        store.index("static bool wait_for_scanner_post_update_health") :
        store.index("static bool badge_candidate_seen")
    ]
    assert "saw_new_boot" in health
    assert "uart_rx_get_scanner_identity_snapshot" in health
    assert "saw_new_identity" in health
    assert "badge_runtime_note_usb_control_alive();" in health

    # Hardware diagnostics must expose the evidence used by the health gate.
    assert r'\"boot_id\":%lu' in serial
    assert r'\"boot_id\":%lu' in http


def test_manifest_finalize_uses_a_persistent_line_reader_across_short_polls():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    finalize = store[
        store.index('snprintf(stage, sizeof(stage), "end")') :
        store.index("relay_done:;")
    ]

    assert "relay_line_reader_t finalize_reader = {0};" in finalize
    assert "relay_read_line_stateful(" in finalize
    assert "&finalize_reader" in finalize
    assert "relay_read_line(" not in finalize


def test_scanner_rejects_lossy_or_trailing_garbage_firmware_offers():
    scanner_main = _source("esp32", "scanner", "main", "main.c")

    offer = scanner_main[
        scanner_main.index("static void handle_fw_offer") :
        scanner_main.index("static void uart_cmd_listener_task")
    ]
    assert "cJSON_IsBool(update_j)" in offer
    assert "json_uint32_exact(generation_j" in offer
    assert "json_uint32_exact(size_j" in offer
    assert "json_uint32_exact(crc_j" in offer
    assert "malformed_fw_offer" in offer

    # Newline framing is not enough if the parser accepts a valid object with
    # arbitrary bytes after it.  Require the full line to be consumed.
    assert "cJSON_ParseWithOpts(" in scanner_main
    assert "line, &parse_end, true)" in scanner_main

    uplink_rx = _source("esp32", "uplink", "main", "comms", "uart_rx.c")
    assert "memchr(line, '\\0', len)" in uplink_rx
    assert "cJSON_ParseWithOpts(line, &parse_end, true)" in uplink_rx


def test_uplink_coalesces_duplicate_nack_bursts_before_retransmit():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    assert "FW_RELAY_NACK_SETTLE_MS" in store
    assert "relay_coalesce_nack_burst" in store
    poll = store[store.index("static int relay_poll_nack") :]
    assert "relay_coalesce_nack_burst(" in poll

    final_wait = store[
        store.index("static int relay_wait_for_staged_or_nack") :
        store.index("/* ── Firmware offer + relay core")
    ]
    assert "nack_settle_deadline_ms" in final_wait
    assert "exact ota_staged supersedes queued NACKs" in final_wait

    # The scanner reports full receipt before its SHA/descriptor validation.
    # That exact session-bound progress is stronger evidence than a stale NACK
    # queued ahead of it, so keep waiting for ota_staged instead of rewinding.
    assert "relay_line_matches_complete_progress" in store
    assert "final_frame_accepted" in final_wait
    assert "Full-image ota_progress supersedes queued NACKs" in final_wait


def test_badge_build_has_no_http_firmware_mutation_routes_or_network_autofetch():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    status = _source("esp32", "uplink", "main", "network", "http_status.c")
    main = _source("esp32", "uplink", "main", "main.c")

    registration = store[store.index("void fw_store_register") :]
    assert "#ifndef FOF_BADGE_VARIANT" in registration
    assert "Badge firmware transport is USB staging + UART relay only" in registration

    status_registration = status[status.index("void http_status_init") :]
    assert "#ifndef FOF_BADGE_VARIANT" in status_registration
    assert "uri_ota_post" in status_registration
    assert "uri_ota_relay" in status_registration

    auto_check_start = main[main.index("Non-badge network updater") :]
    assert "#ifndef FOF_BADGE_VARIANT" in auto_check_start
    assert "Badge firmware network auto-check disabled" in auto_check_start


def test_usb_status_exposes_staged_manifest_and_serialized_update_queue():
    header = _source("esp32", "uplink", "main", "network", "fw_store.h")
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")

    assert "fw_auto_update_status_t" in header
    assert "fw_store_get_auto_update_status" in header
    assert r'\"firmware_store\"' in serial
    assert r'\"auto_update\"' in serial
    assert r'\"pending_mask\"' in serial
    assert r'\"worker_running\"' in serial
    auto_status = serial[
        serial.index('printf(",\\\"auto_update\\\"') :
        serial.index('printf("]}}")')
    ]
    assert r'\"generation\":%lu' in auto_status
    assert r'\"target_slot_mask\":%u' in auto_status
    assert r'\"readiness_probes\":[%u,%u]' in auto_status


def test_usb_staging_ack_echoes_authoritative_target_identity():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")

    assert r'\"target\":\"%s\"' in store
    assert r'\"hardware_type\":\"%s\"' in store
    assert r'\"app_project\":\"%s\"' in store
    assert 'root, "slot_mask")' in serial
    assert "serial_json_uint32_exact" in serial
    assert "slot_mask == 0 || slot_mask > 0x3" in serial
    assert "(uint8_t)slot_mask" in serial


def test_auto_update_success_state_means_full_post_reboot_convergence():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    success = store[
        store.index("if (result.ok)", store.index("static void fw_auto_relay_task")) :
        store.index("} else if (auto_relay_error_is_retryable", store.index("static void fw_auto_relay_task"))
    ]
    assert "FW_AUTO_SLOT_CONVERGED" in success
    assert "auto_coordinator_save_locked()" in store
    assert 'auto_relay_set_state(scanner_id, "transfer_committed")' not in store


def test_post_reboot_convergence_restores_exact_slot_profile_and_role_radios():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    assert "fof_policy_scan_profile_for_topology" in store
    assert "scanner->scan_profile" in store
    assert "scanner->slot_role" in store
    assert 'strcmp(expected_profile, "ble_primary") == 0' in store
    assert 'strcmp(expected_profile, "wifi_primary") == 0' in store
    assert r'\"type\":\"scan_profile\"' in store
    assert "scanner->wifi_initialized" in store
    assert "scanner->wifi_active" in store
    assert "scanner->wifi_init_rc == 0" in store
    assert "scanner->wifi_full_scan_ok > 0" in store
    assert "scanner->ble_initialized" in store
    assert "return !scanner->ble_scanning && !scanner->wifi_paused;" not in store
    assert "radio_healthy = !scanner.ble_scanning && !scanner.wifi_paused;" not in store


def test_every_badge_profile_command_carries_stable_slot_role():
    for path in (
        ("esp32", "uplink", "main", "main.c"),
        ("esp32", "uplink", "main", "comms", "http_upload.c"),
        ("esp32", "uplink", "main", "network", "fw_store.c"),
    ):
        source = _source(*path)
        command_sites = source.count(r'{\"type\":\"scan_profile\"')
        assert command_sites > 0, path
        assert source.count("JSON_KEY_SLOT_ROLE") >= command_sites, path


def test_lost_ota_done_can_only_recover_via_full_post_reboot_health_proof():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    assert "ota_done lost but full post-reboot convergence proved" in store
    assert "health_proved_without_done" in store
    assert "wait_for_scanner_post_update_health" in store


def test_auto_update_coordinator_is_one_bounded_crc_protected_nvs_blob():
    nvs_header = _source("esp32", "uplink", "main", "core", "nvs_config.h")
    nvs_source = _source("esp32", "uplink", "main", "core", "nvs_config.c")
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    migration = _source("esp32", "shared", "firmware_coordinator_migration.h")

    assert "NVS_CONFIG_MAX_BLOB_SIZE" in nvs_header
    assert "nvs_config_get_blob" in nvs_header
    assert "nvs_config_set_blob" in nvs_header
    assert "nvs_get_blob" in nvs_source
    assert "nvs_set_blob" in nvs_source
    assert "nvs_commit" in nvs_source

    assert 'NVS_FW_COORDINATOR "fw_coord"' in store
    assert "FW_AUTO_COORDINATOR_MAGIC" in store
    assert "FW_AUTO_COORDINATOR_SCHEMA" in store
    coordinator = migration[
        migration.index("typedef struct {", migration.index(
            "} fof_fw_coord_v2_t;")) :
        migration.index("} fof_fw_coord_v3_t;")
    ]
    for field in (
        "magic",
        "schema",
        "crc32",
        "generation",
        "target_slot_mask",
        "pending_mask",
        "relay_attempts",
        "readiness_probe_attempts",
        "slot_state",
    ):
        assert field in coordinator
    assert "nvs_config_set_blob(NVS_FW_COORDINATOR" in store
    assert "offsetof(fw_auto_coordinator_blob_t, crc32)" in store


def test_auto_update_mutex_is_initialized_once_before_any_control_or_scanner_task():
    header = _source("esp32", "uplink", "main", "network", "fw_store.h")
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    main = _source("esp32", "uplink", "main", "main.c")

    assert "fw_store_init_auto_update_coordinator" in header
    initializer = store[
        store.index("bool fw_store_init_auto_update_coordinator") :
        store.index("static bool auto_coordinator_lock")
    ]
    assert "xSemaphoreCreateMutexStatic" in initializer
    assert "if (s_auto_coordinator_mutex)" in initializer

    init_call = main.index("fw_store_init_auto_update_coordinator()")
    assert init_call < main.index("serial_config_start_control_task()")
    assert init_call < main.index("uart_rx_start()")

    lock = store[
        store.index("static bool auto_coordinator_lock") :
        store.index("static void auto_coordinator_unlock")
    ]
    assert "xSemaphoreCreateMutexStatic" not in lock


def test_usb_staging_commits_exact_slot_scope_after_manifest_commit():
    header = _source("esp32", "uplink", "main", "network", "fw_store.h")
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    begin_decl = header[
        header.index("bool fw_store_serial_upload_begin") :
        header.index("bool fw_store_serial_upload_write")
    ]
    assert "uint8_t target_slot_mask" in begin_decl
    assert "target_slot_mask == 0" in store
    assert "target_slot_mask & (uint8_t)~FW_AUTO_TARGET_ALL" in store
    assert "s_serial_upload.target_slot_mask = target_slot_mask" in store
    assert r'\"slot_mask\":%u' in store

    upload_end = store[
        store.index("bool fw_store_serial_upload_end") :
        store.index("bool fw_store_get_info")
    ]
    manifest_commit = upload_end.index("persist_validated_metadata(&info)")
    coordinator_commit = upload_end.index("auto_coordinator_begin_generation(")
    assert manifest_commit < coordinator_commit


def test_split_manifest_coordinator_failure_invalidates_and_fails_closed():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    upload_end = store[
        store.index("bool fw_store_serial_upload_end") :
        store.index("bool fw_store_get_info")
    ]
    failure_start = upload_end.index("if (!auto_coordinator_begin_generation")
    coordinator_failure = upload_end[
        failure_start : upload_end.index("return false;", failure_start)
    ]
    assert "invalidate_fw_metadata()" in coordinator_failure
    assert "auto_coordinator_force_fail_closed" in coordinator_failure
    abort = coordinator_failure.index(
        'fw_store_serial_upload_abort("coordinator_commit_failed")'
    )
    release = coordinator_failure.index(
        "auto_coordinator_release_excluded_slots()"
    )
    assert abort < release

    begin = store[
        store.index("static bool auto_coordinator_begin_generation") :
        store.index("static bool auto_coordinator_initialize_fail_closed")
    ]
    assert "auto_coordinator_set_fail_closed_locked" in begin
    assert "s_auto_coordinator = before" not in begin


def test_auto_relay_reads_only_its_reserved_manifest_generation():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    relay = store[
        store.index("static bool fw_relay_stored_to_scanner") :
        store.index("static esp_err_t fw_relay_handler")
    ]
    assert "uint32_t expected_generation" in relay
    generation_guard = relay.index("info.generation != expected_generation")
    operation = relay.index("operation_try_begin()")
    assert generation_guard < operation
    assert '"generation_changed"' in relay

    task_start = store.index("static void fw_auto_relay_task")
    task = store[
        task_start : store.index("static bool auto_coordinator_start_worker",
                                 task_start)
    ]
    assert "scanner_id, relay_generation," in task
    assert "relay_bound_hardware_id, true" in task


def test_auto_relay_attempt_is_persisted_before_transfer_and_never_cooldown_reset():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    coordinator = store[store.index("FW_AUTO_COORDINATOR_MAGIC") :]
    task = coordinator[
        coordinator.index("static void fw_auto_relay_task") :
        coordinator.index("static bool enqueue_auto_relay")
    ]

    increment = task.index("relay_attempts[scanner_id]++")
    persist = task.index("auto_coordinator_save_locked()", increment)
    relay = task.index("fw_relay_stored_to_scanner", persist)
    assert increment < persist < relay
    assert "FW_AUTO_RELAY_COOLDOWN_MS" not in coordinator
    assert "relay_attempts[scanner_id] = 0" not in coordinator
    assert "FW_AUTO_RELAY_MAX_ATTEMPTS 3" in coordinator


def test_store_contention_does_not_consume_a_durable_relay_attempt():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    task_start = store.index("static void fw_auto_relay_task")
    task = store[
        task_start : store.index("static bool auto_coordinator_start_worker", task_start)
    ]

    busy_check = task.index("if (operation_is_active())")
    attempt_reservation = task.index("relay_attempts[scanner_id]++")
    assert busy_check < attempt_reservation
    assert 'strcmp(result.error, "operation_active") == 0' in task
    assert "relay_attempts[scanner_id]--" in task


def test_worker_mutex_contention_cannot_leave_a_phantom_running_worker():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    task_start = store.index("static void fw_auto_relay_task")
    task = store[
        task_start : store.index("static bool auto_coordinator_start_worker", task_start)
    ]

    first_lock = task[
        task.index("while (true)") : task.index("if (!s_auto_coordinator_loaded)")
    ]
    assert "if (!auto_coordinator_lock())" in first_lock
    assert "continue;" in first_lock
    assert "break;" not in first_lock


def test_auto_update_is_ble_first_with_bounded_durable_readiness_probes():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    coordinator = store[store.index("FW_AUTO_COORDINATOR_MAGIC") :]

    assert "FW_AUTO_READY_MAX_PROBES 3" in coordinator
    assert "FW_AUTO_READY_PROBE_DELAY_MS 10000" in coordinator
    assert "readiness_probe_attempts" in coordinator
    assert "fof_auto_wifi_gate_open" in coordinator
    assert r'{\"type\":\"fw_check_now\"}' in coordinator
    assert "FW_AUTO_SLOT_FAILED" in coordinator
    assert "auto_coordinator_slot_gate_open_locked" in coordinator

    reprompt = coordinator[
        coordinator.index("static bool auto_coordinator_reprompt_requested") :
        coordinator.index("static bool enqueue_auto_relay")
    ]
    assert "auto_coordinator_slot_gate_open_locked(scanner_id)" in reprompt

    handler = coordinator[
        coordinator.index("void fw_store_handle_scanner_check") :
        coordinator.index("bool fw_store_handle_scanner_ready")
    ]
    assert "auto_coordinator_slot_gate_open(scanner_id" in handler


def test_boot_restores_generation_bound_coordinator_and_reprompts_requested_slots():
    header = _source("esp32", "uplink", "main", "network", "fw_store.h")
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    main = _source("esp32", "uplink", "main", "main.c")

    assert "fw_store_restore_auto_update_coordinator" in header
    restore = store[store.index("bool fw_store_restore_auto_update_coordinator") :]
    assert "blob.generation == info.generation" in restore
    assert "auto_coordinator_initialize_fail_closed" in restore
    assert "FW_AUTO_SLOT_RELAYING" in restore
    assert "auto_coordinator_reprompt_requested" in restore

    badge_start = main.index("if (!uart_rx_start())")
    ready_call = main.index('uart_rx_send_command("{\\\"type\\\":\\\"ready\\\"}")',
                            badge_start)
    restore_call = main.index("fw_store_restore_auto_update_coordinator()",
                              ready_call)
    assert badge_start < ready_call < restore_call


def test_committed_manifest_carries_scope_so_boot_can_rebuild_missing_coordinator():
    header = _source("esp32", "uplink", "main", "network", "fw_store.h")
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    assert "target_slot_mask" in header
    assert 'NVS_FW_SLOT_MASK "fw_slotmask"' in store

    manifest_crc = store[
        store.index("static uint32_t fw_manifest_crc") :
        store.index("static bool invalidate_fw_metadata")
    ]
    assert "info->target_slot_mask" in manifest_crc
    persist = store[
        store.index("static bool persist_validated_metadata") :
        store.index("const esp_partition_t *fw_store_get_target_partition")
    ]
    assert "NVS_FW_SLOT_MASK" in persist

    restore = store[
        store.index("bool fw_store_restore_auto_update_coordinator") :
        store.index("void fw_store_handle_scanner_check")
    ]
    missing = restore[
        restore.index("if (read_status == NVS_CONFIG_BLOB_MISSING)") :
        restore.index("if (read_status != NVS_CONFIG_BLOB_PRESENT)")
    ]
    assert "auto_coordinator_begin_generation(" in missing
    assert "info.generation" in missing
    assert "info.target_slot_mask" in missing
    assert "auto_coordinator_reprompt_requested()" in missing


def test_nvs_blob_read_status_never_collapses_present_errors_into_missing():
    header = _source("esp32", "uplink", "main", "core", "nvs_config.h")
    source = _source("esp32", "uplink", "main", "core", "nvs_config.c")

    for name in (
        "NVS_CONFIG_BLOB_READ_ERROR",
        "NVS_CONFIG_BLOB_MISSING",
        "NVS_CONFIG_BLOB_PRESENT",
        "nvs_config_read_blob",
    ):
        assert name in header

    read = source[
        source.index("nvs_config_blob_read_status_t nvs_config_read_blob") :
        source.index("bool nvs_config_get_blob", source.index(
            "nvs_config_blob_read_status_t nvs_config_read_blob"))
    ]
    missing = read[
        read.index("ESP_ERR_NVS_NOT_FOUND") :
        read.index("required == 0")
    ]
    assert "NVS_CONFIG_BLOB_MISSING" in missing
    invalid_size = read[
        read.index("required == 0") :
        read.index("size_t read_size")
    ]
    assert "NVS_CONFIG_BLOB_READ_ERROR" in invalid_size
    read_failure = read[read.index("nvs_get_blob(s_nvs_handle, key, data") :]
    assert "NVS_CONFIG_BLOB_READ_ERROR" in read_failure
    assert "NVS_CONFIG_BLOB_MISSING" not in invalid_size + read_failure


def test_present_invalid_coordinator_aborts_restore_without_fresh_generation():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    restore = store[
        store.index("bool fw_store_restore_auto_update_coordinator") :
        store.index("void fw_store_handle_scanner_check")
    ]
    rejection = restore[
        restore.index("if (read_status != NVS_CONFIG_BLOB_PRESENT)") :
        restore.index("if (blob_size == sizeof(fof_fw_coord_v2_t))")
    ]
    assert "return false" in rejection
    assert "auto_coordinator_begin_generation" not in rejection

    invalid = restore[
        restore.index("if (!valid)") :
        restore.index("if (!auto_coordinator_lock())")
    ]
    assert "return false" in invalid
    assert "auto_coordinator_begin_generation" not in invalid
    assert "auto_coordinator_initialize_fail_closed" not in invalid


def test_schema2_migration_is_saved_as_schema3_before_restore_side_effects():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    restore = store[
        store.index("bool fw_store_restore_auto_update_coordinator") :
        store.index("void fw_store_handle_scanner_check")
    ]

    assert "fof_fw_coord_v2_t" in restore
    assert "fof_fw_coordinator_migrate_v2" in restore
    assert "migrated_from_v2" in restore
    migrated = restore[
        restore.index("if (blob_size == sizeof(fof_fw_coord_v2_t))") :
        restore.index("if (!valid)")
    ]
    assert "fof_fw_coordinator_migrate_v2" in migrated

    load = restore[restore.index("s_auto_coordinator = blob") :]
    migration_save = load.index("if (migrated_from_v2)")
    side_effects = [
        load.index("auto_coordinator_release_excluded_slots()"),
        load.index("auto_coordinator_reprompt_requested()"),
        load.index("auto_coordinator_start_worker()"),
    ]
    assert all(migration_save < side_effect for side_effect in side_effects)
    save_failure = load[
        migration_save : load.index("auto_set_identity_floors_locked")
    ]
    assert "auto_coordinator_save_locked()" in save_failure
    assert "return false" in save_failure
    assert "uart_rx_send_command" not in save_failure
    assert "auto_coordinator_start_worker" not in save_failure


def test_coordinator_is_bound_to_exact_manifest_fingerprint_and_fail_closed_flag():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    migration = _source("esp32", "shared", "firmware_coordinator_migration.h")
    coordinator = store[store.index("FW_AUTO_COORDINATOR_MAGIC") :]
    blob = migration[
        migration.index("typedef struct {", migration.index(
            "} fof_fw_coord_v2_t;")) :
        migration.index("} fof_fw_coord_v3_t;")
    ]
    assert "manifest_crc32" in blob
    assert "fail_closed" in blob
    assert "FW_AUTO_COORDINATOR_SCHEMA 3u" in coordinator

    restore = coordinator[
        coordinator.index("bool fw_store_restore_auto_update_coordinator") :
        coordinator.index("void fw_store_handle_scanner_check")
    ]
    assert "blob.manifest_crc32 == manifest_crc32" in restore
    assert "blob.target_slot_mask == info.target_slot_mask" in restore
    assert "blob.fail_closed" in restore

    persist = store[
        store.index("static bool persist_validated_metadata") :
        store.index("const esp_partition_t *fw_store_get_target_partition")
    ]
    assert "esp_random()" in persist
    assert "info->generation = 1" not in persist


def test_ambiguous_manifest_or_coordinator_commit_poison_staged_image():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    poison = store[
        store.index("static bool poison_staged_image") :
        store.index("static bool persist_validated_metadata")
    ]
    assert "esp_partition_write" in poison
    assert "esp_partition_read" in poison
    assert "ESP_IMAGE_HEADER_MAGIC" in poison

    upload_end = store[
        store.index("bool fw_store_serial_upload_end") :
        store.index("bool fw_store_get_info")
    ]
    manifest_failure = upload_end[
        upload_end.index("if (!persist_validated_metadata(&info))") :
        upload_end.index("if (!auto_coordinator_begin_generation")
    ]
    assert "poison_staged_image" in manifest_failure
    coordinator_start = upload_end.index("if (!auto_coordinator_begin_generation")
    coordinator_failure = upload_end[
        coordinator_start : upload_end.index("return false;", coordinator_start)
    ]
    assert "poison_staged_image" in coordinator_failure

    restore = store[
        store.index("bool fw_store_restore_auto_update_coordinator") :
        store.index("void fw_store_handle_scanner_check")
    ]
    missing = restore[
        restore.index("if (read_status == NVS_CONFIG_BLOB_MISSING)") :
        restore.index("if (read_status != NVS_CONFIG_BLOB_PRESENT)")
    ]
    assert "validate_staged_image" in missing
    assert missing.index("validate_staged_image") < missing.index(
        "auto_coordinator_begin_generation("
    )


def test_boot_rechecks_volatile_slots_but_preserves_durable_recovering():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    restore = store[
        store.index("bool fw_store_restore_auto_update_coordinator") :
        store.index("void fw_store_handle_scanner_check")
    ]
    assert "!auto_coordinator_slot_is_terminal" in restore
    assert "FW_AUTO_SLOT_AWAITING_CHECK" in restore
    assert "pending_mask &= (uint8_t)~bit" in restore
    assert "readiness_probe_attempts[scanner_id] = 0" in restore
    assert "FW_AUTO_SLOT_RECOVERING" in restore


def test_new_generation_releases_every_excluded_scanner_without_prompting_it():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    upload_end = store[
        store.index("bool fw_store_serial_upload_end") :
        store.index("bool fw_store_get_info")
    ]
    release = upload_end.index("auto_coordinator_release_excluded_slots()")
    reprompt = upload_end.index("auto_coordinator_reprompt_requested()")
    assert release < reprompt

    helper = store[
        store.index("static void auto_coordinator_release_excluded_slots") :
        store.index("static bool enqueue_auto_relay")
    ]
    assert r'{\"type\":\"start\"}' in helper
    assert r'{\"type\":\"fw_check_now\"}' not in helper


def test_newer_scanner_is_terminal_for_its_slot_but_not_a_wifi_gate_success():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    coordinator = store[store.index("FW_AUTO_COORDINATOR_MAGIC") :]
    assert "FW_AUTO_SLOT_NEWER_SKIPPED" in coordinator

    terminal = coordinator[
        coordinator.index("static bool auto_coordinator_slot_is_terminal") :
        coordinator.index("static const char *auto_coordinator_state_name")
    ]
    assert "FW_AUTO_SLOT_NEWER_SKIPPED" in terminal
    states = coordinator[
        coordinator.index("static const char *auto_coordinator_state_name") :
        coordinator.index("static bool auto_coordinator_blob_valid")
    ]
    assert 'return "newer_skipped"' in states

    handler = coordinator[
        coordinator.index("void fw_store_handle_scanner_check") :
        coordinator.index("bool fw_store_handle_scanner_ready")
    ]
    newer = handler[handler.index("FOF_VERSION_OLDER") :]
    assert "FW_AUTO_SLOT_NEWER_SKIPPED" in newer
    assert '"newer_skipped"' in newer


def test_boot_restores_interrupted_relay_as_durable_recovering():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    restore = store[
        store.index("bool fw_store_restore_auto_update_coordinator") :
        store.index("void fw_store_handle_scanner_check")
    ]
    interrupted = restore[
        restore.index("FW_AUTO_SLOT_RELAYING") :
        restore.index("} else if", restore.index("FW_AUTO_SLOT_RELAYING"))
    ]

    assert "FW_AUTO_SLOT_RECOVERING" in interrupted
    assert "FW_AUTO_SLOT_READY_QUEUED" not in interrupted
    assert "pending_mask &= (uint8_t)~bit" in interrupted
    assert "relay_attempts[scanner_id]" not in interrupted
    assert "bound_hardware_id[scanner_id]" in interrupted


def test_scanner_negotiation_never_escapes_generation_target_or_terminal_state():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    handlers = store[store.index("void fw_store_handle_scanner_check") :]

    assert "auto_coordinator_slot_requested" in handlers
    assert "target_generation != info.generation" in handlers
    assert "auto_coordinator_record_current_identity" in handlers
    assert "FW_AUTO_SLOT_REFUSED" in handlers
    assert "enqueue_auto_relay(" in handlers

    record_start = store.index("static bool auto_coordinator_record_scanner_check")
    record = store[
        record_start : store.index(
            "bool fw_store_restore_auto_update_coordinator", record_start
        )
    ]
    terminal_guard = record.index("auto_coordinator_slot_is_terminal(current)")
    assignment = record.index("slot_state[scanner_id] = new_state")
    assert terminal_guard < assignment


def test_boot_fw_check_cannot_overwrite_a_reserved_or_active_relay_result():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    record_start = store.index("static bool auto_coordinator_record_scanner_check")
    record = store[
        record_start : store.index(
            "bool fw_store_restore_auto_update_coordinator", record_start
        )
    ]
    guard_start = record.index("if (current == FW_AUTO_SLOT_READY_QUEUED")
    active_guard = record[
        guard_start :
        record.index("fw_auto_coordinator_blob_t before")
    ]
    assert "FW_AUTO_SLOT_RELAYING" in active_guard
    assert "return true" in active_guard
    assert "new_state == FW_AUTO_SLOT_OFFERED" not in active_guard


def test_excluded_scanner_never_receives_offer_prompt_queue_or_relay():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    handler_start = store.index("void fw_store_handle_scanner_check")
    handler = store[handler_start : store.index("bool fw_store_handle_scanner_ready")]
    excluded = handler[
        handler.index("if (!auto_coordinator_slot_requested") :
        handler.index("if (!info.name[0]")
    ]
    assert "send_fw_offer" not in excluded
    assert "return;" in excluded

    coordinator = store[store.index("FW_AUTO_COORDINATOR_MAGIC") :]
    assert "target_slot_mask & bit" in coordinator


def test_durable_queue_is_exact_offered_only_and_saved_before_worker_start():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    enqueue_start = store.index("static bool enqueue_auto_relay")
    enqueue = store[
        enqueue_start : store.index(
            "static bool auto_coordinator_record_scanner_check", enqueue_start
        )
    ]
    assert "fof_auto_queue_state_allows" in enqueue
    assert "manifest_crc32" in enqueue
    assert "bound_hardware_id" in enqueue
    assert "FW_AUTO_SLOT_READY_QUEUED" in enqueue
    assert "pending_mask" in enqueue
    save = enqueue.index("auto_coordinator_save_locked()")
    unlock = enqueue.index("auto_coordinator_unlock()", save)
    worker = enqueue.index("auto_coordinator_start_worker()", unlock)
    assert save < unlock < worker
    failure = enqueue[enqueue.index("if (!ok)") : unlock]
    assert "s_auto_coordinator = before" in failure


def test_fw_check_reason_and_legacy_ready_dialect_are_parsed_fail_closed():
    header = _source("esp32", "uplink", "main", "network", "fw_store.h")
    uplink_rx = _source("esp32", "uplink", "main", "comms", "uart_rx.c")

    assert 'json_get_string(root, "reason", "")' in uplink_rx
    assert "fw_store_handle_scanner_check(scanner_id, board, ver, reason)" in uplink_rx
    assert "strict_receipt_fields_absent" in uplink_rx
    strict_absent = uplink_rx[
        uplink_rx.index("static bool strict_receipt_fields_absent") :
        uplink_rx.index("static bool fw_ready_common_fields_valid")
    ]
    for key in (
        "JSON_KEY_FW_NAME",
        '"app_project"',
        '"hardware_type"',
        '"sha256"',
        '"generation"',
        '"allow_same_version"',
    ):
        assert key in strict_absent

    ready_branch = uplink_rx[
        uplink_rx.index("} else if (strcmp(msg_type, MSG_TYPE_FW_READY) == 0") :
        uplink_rx.index("} else if (strcmp(msg_type, MSG_TYPE_CAL_MODE_ACK) == 0")
    ]
    assert "bool legacy_receipt = strict_receipt_fields_absent(root)" in ready_branch
    assert "fw_ready_common_fields_valid" in ready_branch
    assert "fw_store_handle_legacy_scanner_ready" in ready_branch
    assert ready_branch.index("if (legacy_receipt)") < ready_branch.index(
        "fw_store_handle_legacy_scanner_ready"
    )
    assert ready_branch.index("} else {") < ready_branch.index(
        "fw_store_handle_scanner_ready"
    )
    assert "malformed_fw_ready" in ready_branch
    assert r'{\"type\":\"start\"}' in ready_branch

    assert "bool fw_store_handle_scanner_ready" in header
    assert "bool fw_store_handle_legacy_scanner_ready" in header


def test_ready_ui_state_is_published_only_after_durable_acceptance():
    uplink_rx = _source("esp32", "uplink", "main", "comms", "uart_rx.c")
    ready_branch = uplink_rx[
        uplink_rx.index("} else if (strcmp(msg_type, MSG_TYPE_FW_READY) == 0") :
        uplink_rx.index("} else if (strcmp(msg_type, MSG_TYPE_CAL_MODE_ACK) == 0")
    ]

    assert "bool accepted = false" in ready_branch
    acceptance_gate = ready_branch.index("if (!accepted)")
    ready_state = ready_branch.index('"ready"', acceptance_gate)
    need_firmware = ready_branch.index("info->need_firmware = true", acceptance_gate)
    assert acceptance_gate < ready_state
    assert acceptance_gate < need_firmware


def test_legacy_offer_is_manual_identity_fresh_and_binding_bound():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    coordinator = store[store.index("FW_AUTO_COORDINATOR_MAGIC") :]

    assert "s_auto_identity_generation_floor" in coordinator
    assert "s_auto_offer_bindings" in coordinator
    assert "FW_AUTO_OFFER_BINDING_TTL_MS" in coordinator
    assert "FOF_LEGACY_READY_BOOTSTRAP_VERSION" in coordinator
    assert 'strcmp(check_reason, "manual") != 0' in coordinator
    assert "fof_auto_identity_is_fresh" in coordinator
    assert "fof_auto_offer_binding_matches" in coordinator
    assert "fof_firmware_legacy_ready_authorized" in coordinator

    offer = coordinator[
        coordinator.index("static bool auto_coordinator_record_legacy_offer") :
        coordinator.index("bool fw_store_restore_auto_update_coordinator")
    ]
    save = offer.index("auto_coordinator_save_locked()")
    binding = offer.index("s_auto_offer_bindings[scanner_id]", save)
    assert save < binding

    check = coordinator[
        coordinator.index("void fw_store_handle_scanner_check") :
        coordinator.index("bool fw_store_handle_scanner_ready")
    ]
    snapshot = check.index("uart_rx_get_scanner_identity_snapshot")
    offer_call = check.index("auto_coordinator_record_legacy_offer", snapshot)
    assert snapshot < offer_call


def test_schema3_persists_bound_mac_and_exact_recovering_state():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    migration = _source("esp32", "shared", "firmware_coordinator_migration.h")
    coordinator = store[store.index("FW_AUTO_COORDINATOR_MAGIC") :]
    blob = migration[
        migration.index("typedef struct {", migration.index(
            "} fof_fw_coord_v2_t;")) :
        migration.index("} fof_fw_coord_v3_t;")
    ]
    validation = coordinator[
        coordinator.index("static bool auto_coordinator_blob_valid") :
        coordinator.index("static bool auto_coordinator_save_locked")
    ]

    assert "FW_AUTO_COORDINATOR_SCHEMA 3u" in coordinator
    assert "FW_AUTO_SLOT_RECOVERING" in coordinator
    assert "bound_hardware_id" in blob
    assert "auto_hardware_id_is_canonical" in validation
    assert "FW_AUTO_SLOT_READY_QUEUED" in validation
    assert "FW_AUTO_SLOT_RELAYING" in validation
    assert "FW_AUTO_SLOT_RECOVERING" in validation


def test_every_strict_and_legacy_ready_waits_for_second_identity_before_attempt():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    coordinator = store[store.index("FW_AUTO_COORDINATOR_MAGIC") :]

    assert "FW_AUTO_SECOND_IDENTITY_WAIT_MS 12000" in coordinator
    assert "FW_AUTO_OFFER_BINDING_TTL_MS 60000" in coordinator
    assert "s_auto_ready_bindings" in coordinator
    wait = coordinator[
        coordinator.index("static bool auto_wait_for_ready_second_identity") :
        coordinator.index("static void fw_auto_relay_task")
    ]
    assert "uart_rx_get_scanner_identity_snapshot" in wait
    assert "auto_coordinator_lock" not in wait
    assert "vTaskDelay" in wait
    assert "legacy_mode" in wait
    assert "FOF_LEGACY_READY_BOOTSTRAP_VERSION" in wait
    assert "staged_firmware_is_newer_for_scanner" in wait

    task = coordinator[
        coordinator.index("static void fw_auto_relay_task") :
        coordinator.index("static bool auto_coordinator_start_worker")
    ]
    selection = task[
        task.index("for (int slot = 0") :
        task.index("if (scanner_id < 0 && s_auto_relay_worker_running)")
    ]
    assert "relay_attempts[scanner_id]++" not in selection
    assert "FW_AUTO_SLOT_RELAYING" not in selection

    second_identity = task.index("auto_wait_for_ready_second_identity")
    reserve = task.index("relay_attempts[scanner_id]++", second_identity)
    relaying = task.index("FW_AUTO_SLOT_RELAYING", reserve)
    relay = task.index("fw_relay_stored_to_scanner", relaying)
    assert second_identity < reserve < relaying < relay
    invalid = task[
        task.index("if (!reservation_valid)") :
        task.index("fw_auto_coordinator_blob_t before", second_identity)
    ]
    assert "auto_reset_ready_queue_after_revalidation_failure" in invalid
    revalidation = task[second_identity:reserve]
    assert "s_auto_ready_bindings[scanner_id]" in revalidation
    assert "s_auto_legacy_ready[scanner_id] == legacy_mode" in revalidation
    assert "bound_hardware_id[scanner_id]" in revalidation
    assert "staged_firmware_is_newer_for_scanner" in revalidation


def test_ready_enqueue_binds_identity_for_both_strict_and_legacy_paths():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    enqueue = store[
        store.index("static bool enqueue_auto_relay") :
        store.index("static bool auto_coordinator_record_scanner_check")
    ]
    save = enqueue.index("auto_coordinator_save_locked()")
    ready_binding = enqueue.index("s_auto_ready_bindings[scanner_id]", save)
    unlock = enqueue.index("auto_coordinator_unlock()", ready_binding)
    assert save < ready_binding < unlock
    assert "binding->identity_generation = identity->identity_generation" in enqueue

    handlers = store[store.index("bool fw_store_handle_scanner_ready") :]
    strict = handlers[
        handlers.index("bool fw_store_handle_scanner_ready") :
        handlers.index("bool fw_store_handle_legacy_scanner_ready")
    ]
    legacy = handlers[handlers.index("bool fw_store_handle_legacy_scanner_ready") :]
    assert "enqueue_auto_relay" in strict
    assert "enqueue_auto_relay" in legacy


def test_retry_requeue_advances_ready_identity_only_after_durable_save():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    task_start = store.index("static void fw_auto_relay_task")
    task = store[
        task_start : store.index("static bool auto_coordinator_start_worker",
                                 task_start)
    ]
    result = task[
        task.index('strcmp(result.error, "operation_active") == 0') :
        task.index("worker_continues =", task.index(
            'strcmp(result.error, "operation_active") == 0'))
    ]
    save = result.index("auto_coordinator_save_locked()")
    advance = result.index("auto_advance_ready_binding_locked", save)
    assert save < advance
    assert "if (retry)" in result[save:advance]
    assert "&current_identity" in result[advance:]

    helper = store[
        store.index("static void auto_advance_ready_binding_locked") :
        store.index("static void fw_auto_relay_task")
    ]
    assert "s_auto_ready_bindings[scanner_id]" in helper
    assert "identity->identity_generation" in helper
    assert "identity->hardware_id" in helper
    assert "generation" in helper
    assert "manifest_crc32" in helper


def test_terminal_results_clear_bound_mac_before_save_while_retry_keeps_it():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    task_start = store.index("static void fw_auto_relay_task")
    task = store[
        task_start : store.index("static bool auto_coordinator_start_worker",
                                 task_start)
    ]
    result = task[
        task.index('strcmp(result.error, "operation_active") == 0') :
        task.index("worker_continues =", task.index(
            'strcmp(result.error, "operation_active") == 0'))
    ]
    save = result.index("auto_coordinator_save_locked()")
    terminal_branch = result.index("if (terminal)")
    terminal_clear = result.index(
        "bound_hardware_id[scanner_id][0] = '\\0'", terminal_branch
    )
    assert terminal_branch < terminal_clear < save

    retryable = result[
        result.index("auto_relay_error_is_retryable") :
        result.index("} else {", result.index("auto_relay_error_is_retryable"))
    ]
    assert "bound_hardware_id" not in retryable


def test_attempt_exhaustion_clears_bound_mac_and_volatile_ready_binding():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    task_start = store.index("static void fw_auto_relay_task")
    task = store[
        task_start : store.index("static bool auto_coordinator_start_worker",
                                 task_start)
    ]
    exhausted = task[
        task.index("relay_attempts[slot] >=") :
        task.index("continue;", task.index("relay_attempts[slot] >="))
    ]
    clear = exhausted.index("bound_hardware_id[slot][0] = '\\0'")
    save = exhausted.index("auto_coordinator_save_locked()")
    volatile_clear = exhausted.index("auto_clear_ready_bindings_locked", save)
    assert clear < save < volatile_clear


def test_final_live_identity_change_rolls_back_attempt_without_ota_retry():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    task_start = store.index("static void fw_auto_relay_task")
    task = store[
        task_start : store.index("static bool auto_coordinator_start_worker",
                                 task_start)
    ]
    result = task[
        task.index('strcmp(result.error, "operation_active") == 0') :
        task.index("worker_continues =", task.index(
            'strcmp(result.error, "operation_active") == 0'))
    ]
    changed_start = result.index('"bound_identity_changed"')
    changed = result[changed_start : result.index("} else if", changed_start)]
    assert "relay_attempts[scanner_id]--" in changed
    assert "FW_AUTO_SLOT_AWAITING_CHECK" in changed
    assert "pending_mask &=" in changed
    assert "bound_hardware_id[scanner_id][0] = '\\0'" in changed
    assert "retry = true" not in changed


def test_current_requires_post_floor_complete_exact_identity():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    coordinator = store[store.index("FW_AUTO_COORDINATOR_MAGIC") :]
    handler = coordinator[
        coordinator.index("void fw_store_handle_scanner_check") :
        coordinator.index("bool fw_store_handle_scanner_ready")
    ]
    equal = handler[
        handler.index("if (relation == FOF_VERSION_EQUAL)") :
        handler.index("if (relation == FOF_VERSION_OLDER)")
    ]

    assert "auto_coordinator_record_current_identity" in equal
    current = coordinator[
        coordinator.index("static bool auto_coordinator_record_current_identity") :
        coordinator.index("static bool auto_coordinator_record_legacy_offer")
    ]
    assert "auto_identity_matches_manifest" in current
    assert "fof_auto_identity_is_fresh" in current
    assert "s_auto_identity_generation_floor[scanner_id]" in current
    snapshot = handler.index("uart_rx_get_scanner_identity_snapshot")
    first_coordinator_read = handler.index("auto_coordinator_slot_requested")
    assert snapshot < first_coordinator_read


def test_automatic_relay_uses_persisted_bound_mac_as_preupdate_authority():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    relay = store[
        store.index("static bool fw_relay_stored_to_scanner") :
        store.index("static esp_err_t fw_relay_handler")
    ]
    task_start = store.index("static void fw_auto_relay_task")
    task = store[
        task_start : store.index("static bool auto_coordinator_start_worker",
                                 task_start)
    ]

    assert "const char *expected_hardware_id" in relay
    assert "auto_hardware_id_is_canonical(expected_hardware_id)" in relay
    assert "relay_bound_hardware_id" in task
    relay_call = task[task.index("fw_relay_stored_to_scanner") :]
    assert "relay_bound_hardware_id" in relay_call

    identity = relay[
        relay.index("scanner_identity_snapshot_t pre_update_identity") :
        relay.index("const char *scanner_board")
    ]
    assert "expected_hardware_id" in identity
    assert "pre_update_identity.complete" in identity
    assert "strcmp(pre_update_identity.hardware_id," in identity
    assert "bound_identity_changed" in identity
    assert relay.index("bound_identity_changed") < relay.index("ota_begin")


def test_final_automatic_legacy_guard_requires_exact_bootstrap_source_version():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    relay = store[
        store.index("static bool fw_relay_stored_to_scanner") :
        store.index("static esp_err_t fw_relay_handler")
    ]
    identity = relay[
        relay.index("bool automatic_bound_relay") :
        relay.index("char pre_update_hardware_id")
    ]

    assert "legacy_mode" in identity
    assert "FOF_LEGACY_READY_BOOTSTRAP_VERSION" in identity
    assert "pre_update_identity.version" in identity
    assert relay.index("FOF_LEGACY_READY_BOOTSTRAP_VERSION") < relay.index(
        "ota_begin"
    )


def test_coordinator_blob_is_compile_time_bounded_for_nvs():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    coordinator = store[store.index("FW_AUTO_COORDINATOR_MAGIC") :]
    assert "_Static_assert(sizeof(fw_auto_coordinator_blob_t) <=" in coordinator
    assert "NVS_CONFIG_MAX_BLOB_SIZE" in coordinator


def test_coordinator_persisted_states_are_compile_time_mapped_to_migration_schema():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    normalized = " ".join(store.split())
    for state in (
        "EXCLUDED",
        "AWAITING_CHECK",
        "OFFERED",
        "READY_QUEUED",
        "RELAYING",
        "CONVERGED",
        "CURRENT",
        "REFUSED",
        "FAILED",
        "NEWER_SKIPPED",
        "RECOVERING",
    ):
        assert (
            f"_Static_assert((int)FW_AUTO_SLOT_{state} == "
            f"(int)FOF_FW_COORD_SLOT_{state}" in normalized
        )

    cmake = _source("esp32", "uplink", "main", "CMakeLists.txt")
    assert 'target_sources(${COMPONENT_LIB} PRIVATE "../../shared/firmware_coordinator_migration.c")' in cmake


def test_coordinator_save_refuses_invalid_schema3_before_nvs_write():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    save = store[
        store.index("static bool auto_coordinator_save_locked") :
        store.index("static void auto_coordinator_set_fail_closed_locked")
    ]
    stamp = save.index("s_auto_coordinator.crc32 =")
    validate = save.index("auto_coordinator_blob_valid", stamp)
    write = save.index("nvs_config_set_blob", validate)
    assert stamp < validate < write
    assert "return false" in save[validate:write]
    assert "invalid schema 3" in save[validate:write]


def test_terminal_gate_does_not_open_when_terminal_state_persistence_fails():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    task_start = store.index("static void fw_auto_relay_task")
    task = store[
        task_start : store.index("static bool auto_coordinator_start_worker", task_start)
    ]

    exhausted_attempt = task[
        task.index("relay_attempts[slot] >=") :
        task.index("fw_auto_coordinator_blob_t before")
    ]
    assert "if (!auto_coordinator_save_locked())" in exhausted_attempt
    assert "s_auto_relay_worker_running = false" in exhausted_attempt

    exhausted_probe = task[
        task.index("readiness_probe_attempts[slot] >=") :
        task.index("fw_auto_coordinator_blob_t before", task.index("readiness_probe_attempts[slot] >="))
    ]
    assert "if (!auto_coordinator_save_locked())" in exhausted_probe
    assert "s_auto_relay_worker_running = false" in exhausted_probe


def test_worker_result_save_failure_fails_closed_instead_of_sticking_relaying():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    task_start = store.index("static void fw_auto_relay_task")
    task = store[
        task_start : store.index("static bool auto_coordinator_start_worker", task_start)
    ]
    result_update = task[
        task.index('strcmp(result.error, "operation_active") == 0') :
        task.index("worker_continues =", task.index('strcmp(result.error, "operation_active") == 0'))
    ]
    assert "auto_coordinator_fail_closed_after_save_failure_locked" in result_update
    assert "s_auto_coordinator = before" not in result_update

    helper_start = store.index(
        "static void auto_coordinator_fail_closed_after_save_failure_locked"
    )
    helper = store[
        helper_start : store.index(
            "static bool auto_coordinator_force_fail_closed", helper_start
        )
    ]
    assert "auto_coordinator_set_fail_closed_locked" in helper
    assert "FW_AUTO_COORDINATOR_SAVE_RETRIES" in helper
    assert "s_auto_relay_worker_running = false" in helper

    task_tail = task[task.rindex("}", 0, task.rindex("}")) :]
    assert "auto_coordinator_release_excluded_slots()" in task
    assert task.index("auto_coordinator_release_excluded_slots()") < task.rindex(
        "vTaskDelete(NULL)"
    )


def test_worker_retries_result_lock_instead_of_abandoning_reserved_relay():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    task_start = store.index("static void fw_auto_relay_task")
    task = store[
        task_start : store.index("static bool auto_coordinator_start_worker", task_start)
    ]
    result_phase = task[
        task.index("bool terminal = false;") :
        task.index("if (terminal && !result.ok)")
    ]

    # Once a RELAYING attempt is durably reserved, a transient mutex timeout
    # must not discard the only authoritative transfer/health result. Mirror
    # the loop-top behavior: retain worker ownership and retry after a bounded
    # delay until the result can be reconciled or a newer generation supersedes it.
    assert "while (!auto_coordinator_lock())" in result_phase
    assert "vTaskDelay(pdMS_TO_TICKS(250))" in result_phase
    assert "if (auto_coordinator_lock())" not in result_phase
    assert result_phase.index("while (!auto_coordinator_lock())") < result_phase.index(
        "s_auto_coordinator.generation == relay_generation"
    )


def test_legacy_ack_is_exact_session_bound_without_a_null_manifest_shortcut():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    strict = store[
        store.index("static bool relay_line_matches_manifest_ack") :
        store.index("static bool relay_line_matches_complete_progress")
    ]
    assert "if (!info || !relay_parse_receipt" in strict
    assert "fof_firmware_strict_receipt_matches" in strict
    assert ".session_id = session_id" in strict

    legacy = store[
        store.index("static bool relay_line_matches_legacy_receipt") :
        store.index("static bool relay_line_matches_manifest_ack")
    ]
    assert "fof_firmware_legacy_ack_matches" in legacy
    assert "fof_firmware_legacy_done_matches" in legacy
    assert "strstr" not in legacy

    wait = store[
        store.index("static int relay_wait_for_with_resend") :
        store.index("static int relay_extract_seq")
    ]
    assert "relay_line_matches_legacy_receipt" in wait
    assert "relay_line_matches_manifest_ack" in wait
    assert "expected_manifest != NULL" in wait

    begin = store[
        store.index('snprintf(stage, sizeof(stage), "begin")') :
        store.index('snprintf(stage, sizeof(stage), "chunks")')
    ]
    assert "legacy_mode ? NULL : &info" in begin


def test_legacy_final_progress_and_done_are_exact_session_size_receipts():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    progress = store[
        store.index("static bool relay_line_matches_complete_progress") :
        store.index("static int relay_wait_for(")
    ]
    assert "relay_parse_receipt" in progress
    assert "fof_firmware_legacy_progress_matches" in progress
    assert "&parsed.view, session_id, info->size" in progress

    staged = store[
        store.index("static int relay_wait_for_staged_or_nack") :
        store.index("/* ── Firmware offer + relay core")
    ]
    assert "bool legacy_mode" in staged
    exact_progress = staged.index("relay_line_matches_complete_progress")
    legacy_success = staged.index("if (legacy_mode)", exact_progress)
    staged_match = staged.index("relay_line_matches_manifest_ack", legacy_success)
    assert exact_progress < legacy_success < staged_match

    relay = store[
        store.index("static bool fw_relay_stored_to_scanner") :
        store.index("static esp_err_t fw_relay_handler")
    ]
    final_wait = relay[relay.index("relay_wait_for_staged_or_nack") :]
    assert "legacy_mode" in final_wait[: final_wait.index("15000")]

    done = relay[relay.index("bool saw_exact_done") : relay.index("relay_done:")]
    assert "if (legacy_mode)" in done
    assert "relay_line_matches_legacy_receipt" in done
    assert "MSG_TYPE_OTA_DONE" in done
    assert "info.size" in done
    strict_done = done[done.index("else") :]
    assert "relay_line_matches_manifest_ack" in strict_done


def test_manifest_bound_ota_end_is_retried_until_commit_receipt():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    relay = store[
        store.index("static bool fw_relay_stored_to_scanner") :
        store.index("static esp_err_t fw_relay_handler")
    ]
    final = relay[
        relay.index('snprintf(stage, sizeof(stage), "end")') :
        relay.index("relay_done:")
    ]

    # A single lost ota_end leaves a fully staged scanner waiting until its
    # 30-second finalize watchdog aborts.  Retry the identical immutable
    # session/manifest command while awaiting the exact ota_done receipt.
    assert "FW_RELAY_END_RESEND_MS" in store
    assert "next_end_send_ms" in final
    assert "uart_rx_send_command_to_scanner_checked(" in final
    assert "scanner_id, end_cmd" in final
    assert final.index("next_end_send_ms") < final.index("while (")
    assert final.index("uart_rx_send_command_to_scanner_checked") > final.index(
        "while ("
    )


def test_interrupted_relay_restore_saves_recovery_before_abort_and_cooldown():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    coordinator = store[store.index("FW_AUTO_COORDINATOR_MAGIC") :]

    assert "FW_AUTO_RECOVERY_COOLDOWN_MS 35000" in coordinator
    assert "FW_AUTO_RECOVERY_PROBE_DELAY_MS 20000" in coordinator
    assert "s_auto_recovery_not_before_ms" in coordinator
    assert "s_auto_recovery_next_probe_ms" in coordinator

    restore = coordinator[
        coordinator.index("bool fw_store_restore_auto_update_coordinator") :
        coordinator.index("void fw_store_handle_scanner_check")
    ]
    interrupted = restore[
        restore.index("FW_AUTO_SLOT_RELAYING") :
        restore.index("} else if", restore.index("FW_AUTO_SLOT_RELAYING"))
    ]
    assert "FW_AUTO_SLOT_RECOVERING" in interrupted
    assert "readiness_probe_attempts[scanner_id] = 0" in interrupted
    assert "relay_attempts[scanner_id]" not in interrupted
    assert "bound_hardware_id[scanner_id]" in interrupted

    saved = restore.index("if (!saved)")
    recovery_start = restore.index("auto_begin_recovery_after_restore", saved)
    worker = restore.index("auto_coordinator_start_worker", recovery_start)
    assert saved < recovery_start < worker

    begin = coordinator[
        coordinator.index("static void auto_begin_recovery_after_restore") :
        coordinator.index("static bool auto_coordinator_start_worker")
    ]
    abort = begin.index(r'{\"type\":\"ota_abort\"}')
    sentinel = begin.index("relay_send_wire_abort_sentinel", abort)
    cooldown = begin.index("FW_AUTO_RECOVERY_COOLDOWN_MS")
    assert abort < sentinel
    assert "s_auto_recovery_not_before_ms" in begin
    assert cooldown >= 0


def test_recovery_worker_uses_three_saved_probes_twenty_seconds_apart():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    task = store[
        store.index("static void fw_auto_relay_task") :
        store.index(
            "static bool auto_coordinator_start_worker",
            store.index("static void fw_auto_relay_task"),
        )
    ]

    recovery = task[
        task.index("FOF_AUTO_SLOT_RECOVERING") :
        task.index("for (int slot = 0", task.index("FOF_AUTO_SLOT_RECOVERING") + 1)
    ]
    assert "fof_auto_recovery_probe_decide" in recovery
    assert "FW_AUTO_READY_MAX_PROBES" in recovery
    assert "readiness_probe_attempts[slot]++" in recovery
    save = recovery.index("auto_coordinator_save_locked()")
    reserved = recovery.index("recovery_probe_scanner_id = slot", save)
    assert save < reserved
    assert "auto_coordinator_fail_closed_after_save_failure_locked" in recovery

    send = task[task.index("if (recovery_probe_scanner_id >= 0)") :]
    assert r'{\"type\":\"fw_check_now\"}' in send
    assert "FW_AUTO_RECOVERY_PROBE_DELAY_MS" in send


def test_recovery_resolution_requires_manual_fresh_same_mac_complete_health():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    coordinator = store[store.index("FW_AUTO_COORDINATOR_MAGIC") :]
    helper = coordinator[
        coordinator.index("static auto_recovery_check_result_t") :
        coordinator.index("bool fw_store_restore_auto_update_coordinator")
    ]

    for proof in (
        '.manual_probe = strcmp(check_reason, "manual") == 0',
        ".identity_fresh =",
        ".same_hardware_id =",
        ".target_contract_matches =",
        ".rollback_clear =",
        ".recovery_normal =",
        ".command_healthy =",
        ".profile_healthy =",
        ".radio_healthy =",
        ".version_relation =",
        ".source_version = scanner_version",
    ):
        assert proof in helper
    assert "fof_auto_recovery_decide" in helper
    assert "s_auto_recovery_not_before_ms" in helper
    assert "s_auto_recovery_not_before_ms[scanner_id] > 0" in helper
    assert "FW_AUTO_SLOT_CONVERGED" in helper
    assert "FW_AUTO_SLOT_OFFERED" in helper
    assert "FOF_AUTO_RECOVERY_REFUSED" in helper
    assert "auto_coordinator_fail_closed_after_save_failure_locked" in helper

    handler = coordinator[
        coordinator.index("void fw_store_handle_scanner_check") :
        coordinator.index("bool fw_store_handle_scanner_ready")
    ]
    recovery_check = handler.index("auto_coordinator_handle_recovery_check")
    equal = handler.index("if (relation == FOF_VERSION_EQUAL)")
    current = handler.index("auto_coordinator_record_current_identity", equal)
    assert recovery_check < equal < current

    gate = coordinator[
        coordinator.index("static bool auto_coordinator_slot_gate_open_locked") :
        coordinator.index("static bool auto_coordinator_slot_gate_open(")
    ]
    assert "fof_auto_wifi_gate_open" in gate


def test_manual_http_relay_rejects_legacy_query_before_starting_relay():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    handler = store[
        store.index("static esp_err_t fw_relay_handler") :
        store.index("bool fw_store_relay_staged_to_scanner(")
    ]

    assert "bool legacy_requested" in handler
    rejection = handler.index("if (legacy_requested)")
    assert "HTTPD_400_BAD_REQUEST" in handler[rejection:]
    assert "legacy relay is automatic-only" in handler[rejection:]
    relay_call = handler.index("fw_relay_stored_to_scanner", rejection)
    assert rejection < relay_call
    assert "legacy_mode" not in handler[relay_call : relay_call + 180]


def test_low_level_legacy_relay_requires_shared_automatic_bound_policy():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    relay = store[
        store.index("static bool fw_relay_stored_to_scanner") :
        store.index("static esp_err_t fw_relay_handler")
    ]

    authorization = relay.index("fof_firmware_legacy_relay_authorized")
    assert "fof_legacy_relay_authorization_view_t" in relay[:authorization]
    assert '"legacy_not_authorized"' in relay[authorization:]
    assert authorization < relay.index("ota_begin")

    cmake = _source("esp32", "uplink", "main", "CMakeLists.txt")
    platformio = _source("esp32", "platformio.ini")
    assert "firmware_relay_policy.c" in cmake
    assert "firmware_relay_policy.c" in platformio
