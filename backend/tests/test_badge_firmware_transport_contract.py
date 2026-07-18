from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def _source(*parts: str) -> str:
    return (REPO_ROOT.joinpath(*parts)).read_text()


def test_scanner_ota_end_and_abort_are_bound_to_the_active_session():
    main = _source("esp32", "scanner", "main", "main.c")

    assert "uart_ota_session_id()" in main
    assert 'strcmp(request_session, active_session) == 0' in main
    assert r'\"session_mismatch\"' in main


def test_uplink_ota_end_carries_the_immutable_session_and_manifest():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    assert r'\"type\":\"ota_end\"' in store
    assert r'\"session_id\":\"%s\"' in store
    assert r'\"sha256\":\"%s\"' in store
    assert r'\"generation\":%lu' in store


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
    assert ready_branch.count("json_get_uint32_exact(") >= 3
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
    getter = uplink_rx[
        uplink_rx.index("bool uart_rx_get_scanner_identity_snapshot") :
        uplink_rx.index("ota_response_t uart_rx_get_last_ota_response")
    ]
    assert "xSemaphoreTake" in getter
    assert "xSemaphoreGive" in getter
    assert "*out =" in getter


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

    auto_check_start = main[main.index("Spawn the periodic firmware auto-check") :]
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

    assert "fof_policy_scan_profile_for_slot" in store
    assert "scanner->scan_profile" in store
    assert 'strcmp(expected_profile, "ble_primary") == 0' in store
    assert 'strcmp(expected_profile, "wifi_primary") == 0' in store
    assert r'\"type\":\"scan_profile\"' in store


def test_lost_ota_done_can_only_recover_via_full_post_reboot_health_proof():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    assert "ota_done lost but full post-reboot convergence proved" in store
    assert "health_proved_without_done" in store
    assert "wait_for_scanner_post_update_health" in store


def test_auto_update_coordinator_is_one_bounded_crc_protected_nvs_blob():
    nvs_header = _source("esp32", "uplink", "main", "core", "nvs_config.h")
    nvs_source = _source("esp32", "uplink", "main", "core", "nvs_config.c")
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    assert "NVS_CONFIG_MAX_BLOB_SIZE" in nvs_header
    assert "nvs_config_get_blob" in nvs_header
    assert "nvs_config_set_blob" in nvs_header
    assert "nvs_get_blob" in nvs_source
    assert "nvs_set_blob" in nvs_source
    assert "nvs_commit" in nvs_source

    assert 'NVS_FW_COORDINATOR "fw_coord"' in store
    assert "FW_AUTO_COORDINATOR_MAGIC" in store
    assert "FW_AUTO_COORDINATOR_SCHEMA" in store
    coordinator = store[
        store.index("typedef struct {", store.index("FW_AUTO_COORDINATOR_SCHEMA")) :
        store.index("} fw_auto_coordinator_blob_t;")
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
    assert "scanner_id, relay_generation, true" in task


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
    assert "auto_coordinator_slot_is_terminal" in coordinator
    assert "scanner_id != 1" in coordinator
    assert "!auto_coordinator_slot_is_terminal" in coordinator
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
        coordinator.index("void fw_store_handle_scanner_ready")
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
    invalid = restore[restore.index("if (!valid)") :]
    assert "auto_coordinator_begin_generation(" in invalid
    assert "info.generation" in invalid
    assert "info.target_slot_mask" in invalid
    assert "auto_coordinator_reprompt_requested()" in invalid


def test_coordinator_is_bound_to_exact_manifest_fingerprint_and_fail_closed_flag():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    coordinator = store[store.index("FW_AUTO_COORDINATOR_MAGIC") :]
    blob = coordinator[
        coordinator.index("typedef struct {") :
        coordinator.index("} fw_auto_coordinator_blob_t;")
    ]
    assert "manifest_crc32" in blob
    assert "fail_closed" in blob
    assert "FW_AUTO_COORDINATOR_SCHEMA 2u" in coordinator

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
    invalid = restore[restore.index("if (!valid)") :]
    assert "validate_staged_image" in invalid
    assert invalid.index("validate_staged_image") < invalid.index(
        "auto_coordinator_begin_generation("
    )


def test_boot_rechecks_every_nonterminal_slot_instead_of_trusting_ready_state():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    restore = store[
        store.index("bool fw_store_restore_auto_update_coordinator") :
        store.index("void fw_store_handle_scanner_check")
    ]
    assert "!auto_coordinator_slot_is_terminal" in restore
    assert "FW_AUTO_SLOT_AWAITING_CHECK" in restore
    assert "pending_mask &= (uint8_t)~bit" in restore
    assert "readiness_probe_attempts[scanner_id] = 0" in restore


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


def test_newer_scanner_is_a_successful_terminal_skip_that_opens_the_next_slot():
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
        coordinator.index("void fw_store_handle_scanner_ready")
    ]
    newer = handler[handler.index("FOF_VERSION_OLDER") :]
    assert "FW_AUTO_SLOT_NEWER_SKIPPED" in newer
    assert '"newer_skipped"' in newer


def test_boot_probes_version_before_retrying_an_interrupted_relay():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    restore = store[
        store.index("bool fw_store_restore_auto_update_coordinator") :
        store.index("void fw_store_handle_scanner_check")
    ]
    interrupted = restore[
        restore.index("FW_AUTO_SLOT_RELAYING") :
        restore.index("} else if", restore.index("FW_AUTO_SLOT_RELAYING"))
    ]

    assert "FW_AUTO_SLOT_AWAITING_CHECK" in interrupted
    assert "FW_AUTO_SLOT_READY_QUEUED" not in interrupted
    assert "pending_mask &= (uint8_t)~bit" in interrupted
    assert "readiness_probe_attempts[scanner_id] = 0" in interrupted
    assert "relay_attempts[scanner_id]" not in interrupted


def test_scanner_negotiation_never_escapes_generation_target_or_terminal_state():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    handlers = store[store.index("void fw_store_handle_scanner_check") :]

    assert "auto_coordinator_slot_requested" in handlers
    assert "target_generation != info.generation" in handlers
    assert "FW_AUTO_SLOT_CURRENT" in handlers
    assert "FW_AUTO_SLOT_REFUSED" in handlers
    assert "enqueue_auto_relay(scanner_id, target_generation)" in handlers

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
    handler = store[handler_start : store.index("void fw_store_handle_scanner_ready")]
    excluded = handler[
        handler.index("if (!auto_coordinator_slot_requested") :
        handler.index("if (!info.name[0]")
    ]
    assert "send_fw_offer" not in excluded
    assert "return;" in excluded

    coordinator = store[store.index("FW_AUTO_COORDINATOR_MAGIC") :]
    assert "target_slot_mask & bit" in coordinator


def test_durable_queue_can_restart_worker_after_task_creation_or_nvs_failure():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    enqueue_start = store.index("static bool enqueue_auto_relay")
    enqueue = store[
        enqueue_start : store.index(
            "static bool auto_coordinator_record_scanner_check", enqueue_start
        )
    ]
    duplicate = enqueue[
        enqueue.index("FW_AUTO_SLOT_RELAYING") :
        enqueue.index("fw_auto_coordinator_blob_t before")
    ]
    assert "auto_coordinator_start_worker()" in duplicate


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
