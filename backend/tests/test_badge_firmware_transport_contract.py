import json
import re
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]


def _source(*parts: str) -> str:
    return (REPO_ROOT.joinpath(*parts)).read_text()


def _usb_health_fixture() -> dict:
    return json.loads(
        _source("docs", "badge", "protocol", "badge_usb_health_v1.fixture.json")
    )["usb_health"]


def test_con_crud_rendering_preserves_four_lanes_and_persisted_theme_schema():
    display_contract = _source(
        "esp32", "shared", "badge_display_contract.h"
    )
    threat_policy = _source(
        "esp32", "shared", "badge_threat_policy.h"
    )
    detection_types = _source("esp32", "shared", "detection_types.h")
    theme_header = _source("esp32", "shared", "badge_theme.h")
    theme = _source("esp32", "shared", "badge_theme.c")
    display = _source(
        "esp32", "uplink", "main", "hw", "display_st7735.c"
    )

    lane_enum = display_contract[
        display_contract.index("typedef enum {") :
        display_contract.index("} badge_display_contract_lane_t;")
    ]
    assert re.findall(r"BADGE_LANE_[A-Z0-9_]+", lane_enum) == [
        "BADGE_LANE_GLOBAL_1",
        "BADGE_LANE_GLOBAL_2",
        "BADGE_LANE_BLE",
        "BADGE_LANE_WIFI",
        "BADGE_LANE_INVALID",
    ]
    threat_lane_at = threat_policy.index("BADGE_THREAT_DISPLAY_LANE_NONE")
    threat_lane_enum = threat_policy[
        threat_policy.rfind("typedef enum {", 0, threat_lane_at) :
        threat_policy.index("} badge_threat_display_lane_t;", threat_lane_at)
    ]
    assert re.findall(
        r"BADGE_THREAT_DISPLAY_LANE_[A-Z0-9_]+", threat_lane_enum
    ) == [
        "BADGE_THREAT_DISPLAY_LANE_NONE",
        "BADGE_THREAT_DISPLAY_LANE_BLE",
        "BADGE_THREAT_DISPLAY_LANE_WIFI",
    ]
    assert "BADGE_LANE_GAME" not in display_contract
    assert "BADGE_THREAT_DISPLAY_LANE_GAME" not in threat_policy
    assert "BADGE_THREAT_GAME" not in threat_policy
    assert re.findall(
        r"^#define\s+(DETECTION_SRC_[A-Z0-9_]+)\s+(\d+)",
        detection_types,
        re.MULTILINE,
    ) == [
        ("DETECTION_SRC_BLE_RID", "0"),
        ("DETECTION_SRC_WIFI_SSID", "1"),
        ("DETECTION_SRC_WIFI_DJI_IE", "2"),
        ("DETECTION_SRC_WIFI_BEACON", "3"),
        ("DETECTION_SRC_WIFI_OUI", "4"),
        ("DETECTION_SRC_WIFI_PROBE_REQUEST", "5"),
        ("DETECTION_SRC_BLE_FINGERPRINT", "6"),
        ("DETECTION_SRC_WIFI_ASSOC", "7"),
        ("DETECTION_SRC_WIFI_AP_INVENTORY", "8"),
    ]

    stored_theme = theme_header[
        theme_header.index("typedef struct {", theme_header.index(
            "#define BADGE_THEME_ACCENT_COUNT"
        )) :
        theme_header.index("} badge_theme_t;")
    ]
    assert "badge_con_" not in stored_theme
    derive = theme[
        theme.index("void badge_theme_derive_con_palette(") :
    ]
    assert "nvs_" not in derive
    assert "badge_theme_runtime_set" not in derive
    assert "#if defined(FOF_DC34_GAME_CANARY)" in theme
    assert "badge_con_presentation_hud(" in display
    assert "fb_draw_heart_7x5(" in display
    assert "SHIELD %3u%%" not in display


def test_badge_reset_chord_is_decided_before_all_single_button_dispatch():
    display = _source(
        "esp32", "uplink", "main", "hw", "display_st7735.c"
    )
    chord_policy = _source("esp32", "shared", "badge_power_chord.c")
    task_at = display.index("static void badge_button_task(void *arg)")
    task = display[task_at : display.index(
        "static bool badge_buttons_start(void)", task_at
    )]

    sample = task.index("badge_button_sample_one(")
    both = task.index("bool both_held =", sample)
    allowed = task.index("bool chord_allowed =", both)
    chord = task.index("badge_power_chord_update(", allowed)
    suppress = task.index("badge_power_chord_dispatch_gate_update(", chord)
    dispatch = task.index("badge_button_dispatch_edge(", suppress)
    assert sample < both < allowed < chord < suppress < dispatch
    assert "!buttons[0].boot_ignored && !buttons[1].boot_ignored" in task
    assert "if (both_held)" in task
    gate_update = chord_policy[
        chord_policy.index("bool badge_power_chord_dispatch_gate_update(") :
    ]
    assert "both_held || event_requires_suppression" in gate_update
    assert "gate->suppress_until_full_release ||" in gate_update
    assert "!button_one_pressed &&" in gate_update
    assert "!button_two_pressed" in gate_update

    reset = task[
        task.index("if (power_event == BADGE_POWER_CHORD_RESET)") :
        task.index("badge_button_diag_set_b2_pending()")
    ]
    assert "buttons[0].consume_release = true" in reset
    assert "buttons[1].consume_release = true" in reset
    assert "badge_button_gesture_cancel(&s_b2_gesture)" in reset
    assert "badge_usb_transport_host_active(25)" not in reset
    assert "if (!host_active)" not in reset
    assert "badge_usb_recovery_target(flash_confirmed)" in reset
    assert "badge_usb_recovery_target(false)" in reset
    assert '"button_reboot"' in reset
    assert '"button_usb_rom"' in reset
    assert "esp_restart()" not in reset


def test_con_crud_runtime_is_canary_only_and_ready_before_usb_dispatch():
    main = _source("esp32", "uplink", "main", "main.c")
    cmake = _source("esp32", "uplink", "main", "CMakeLists.txt")
    uplink_ini = _source("esp32", "uplink", "platformio.ini")
    app = main[main.index("void app_main(void)") :]

    assert "if(NOT FOF_DC34_GAME_CANARY)" in cmake
    production_excludes = cmake[
        cmake.index("if(NOT FOF_DC34_GAME_CANARY)") :
        cmake.index("endif()", cmake.index("if(NOT FOF_DC34_GAME_CANARY)"))
    ]
    assert "EXCLUDE_SRCS" in production_excludes
    assert '"core/badge_con_runtime.c"' in production_excludes
    canary = uplink_ini[
        uplink_ini.index("[env:uplink-s3-fof_badge-con-crud-canary]") :
    ]
    assert "-DFOF_DC34_GAME_CANARY=1" in canary
    assert app.index("badge_runtime_init(s_ota_pending_verify)") < (
        app.index("badge_con_runtime_init()")
    ) < app.index("badge_usb_transport_set_dispatch_ready()")


def test_con_crud_vhci_adapter_is_controller_only_static_and_canary_scoped():
    adapter = _source(
        "esp32", "uplink", "main", "game", "badge_con_vhci.c"
    )
    header = _source(
        "esp32", "uplink", "main", "game", "badge_con_vhci.h"
    )
    memory_policy = _source(
        "esp32", "shared", "badge_con_radio_runtime_policy.h"
    )
    display = _source(
        "esp32", "uplink", "main", "hw", "display_st7735.c"
    )
    cmake = _source("esp32", "uplink", "main", "CMakeLists.txt")
    defaults = _source(
        "esp32", "uplink",
        "sdkconfig.esp32s3-fof_badge-con-crud-canary.defaults",
    )

    for token in (
        "nimble_port",
        "esp_nimble",
        "esp_bluedroid",
        "esp_ble_gatt",
        "esp_ble_gap_start_scanning",
        "esp_ble_gap_set_device_name",
        "ble_gap_adv_start",
        "ble_gap_connect",
        "malloc(",
        "calloc(",
        "realloc(",
        "free(",
        "xTaskCreate",
        "wifi_",
    ):
        assert token not in adapter

    for required in (
        "BT_CONTROLLER_INIT_CONFIG_DEFAULT()",
        "controller_config.ble_50_feat_supp = false;",
        "esp_bt_controller_init(",
        "esp_bt_controller_enable(ESP_BT_MODE_BLE)",
        "esp_vhci_host_register_callback(",
        "esp_vhci_host_check_send_available()",
        "esp_vhci_host_send_packet(",
        "heap_caps_get_free_size(",
        "heap_caps_get_largest_free_block(",
        "badge_con_radio_runtime_memory_gate(",
        "psram_available()",
        "psram_total_size()",
        "psram_free_size()",
        'fail_initialization("psram_gate")',
        'fail_initialization("internal_heap_gate")',
        "static uint8_t s_pending_event",
    ):
        assert required in adapter

    for required in (
        "#define BADGE_CON_RADIO_INTERNAL_HEAP_MIN 24576U",
        "#define BADGE_CON_RADIO_INTERNAL_BLOCK_MIN 16384U",
        "#define BADGE_CON_RADIO_PSRAM_TOTAL_MIN 8388608U",
        "#define BADGE_CON_RADIO_PSRAM_FREE_MIN 5242880U",
        "BADGE_CON_RADIO_MEMORY_PSRAM",
        "BADGE_CON_RADIO_MEMORY_INTERNAL",
    ):
        assert required in memory_policy

    assert "s_fb = psram_alloc_strict(LCD_FB_BYTES);" in display
    assert display.index("#if defined(FOF_DC34_GAME_CANARY)") < display.index(
        "s_fb = psram_alloc_strict(LCD_FB_BYTES);"
    ) < display.index("#else", display.index(
        "s_fb = psram_alloc_strict(LCD_FB_BYTES);"
    ))
    assert "#if !defined(FOF_DC34_GAME_CANARY)" in adapter
    assert '#error "badge_con_vhci is private to the explicit game canary"' in adapter
    assert "badge_con_vhci_radio_quiesced" in header
    assert 'SRC_DIRS "." "comms" "hw" "hw/assets" "network" "core"' in cmake
    assert 'list(APPEND FOF_UPLINK_GAME_SRCS "game/badge_con_vhci.c")' in cmake
    assert "if(FOF_DC34_GAME_CANARY)" in cmake
    assert "CONFIG_BT_CONTROLLER_ONLY=y" in defaults
    assert "CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY=y" in defaults
    assert "CONFIG_BT_CTRL_DTM_ENABLE=n" in defaults
    assert "CONFIG_BT_CTRL_HCI_MODE_VHCI=y" in defaults
    assert "CONFIG_BT_BLUEDROID_ENABLED=n" in defaults
    assert "CONFIG_BT_NIMBLE_ENABLED=n" in defaults
    assert "CONFIG_SPIRAM=y" in defaults
    assert "CONFIG_SPIRAM_BOOT_INIT=y" in defaults
    assert "CONFIG_SPIRAM_IGNORE_NOTFOUND=y" in defaults
    assert "CONFIG_SPIRAM_USE_CAPS_ALLOC=y" in defaults
    assert "CONFIG_SPIRAM_USE_MALLOC=y" not in defaults


def test_con_crud_vhci_quiescence_is_serialized_and_epoch_bound():
    adapter = _source(
        "esp32", "uplink", "main", "game", "badge_con_vhci.c"
    )
    header = _source(
        "esp32", "uplink", "main", "game", "badge_con_vhci.h"
    )
    operation_header = _source(
        "esp32", "shared", "firmware_operation_token.h"
    )
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    for token in (
        "badge_con_vhci_apply_radio_policy",
        "badge_con_vhci_request_quiescence",
        "badge_con_vhci_radio_quiesced_for_epoch",
    ):
        assert token in header
    assert "StaticSemaphore_t s_policy_mutex_storage" in adapter
    assert "xSemaphoreCreateMutexStatic" in adapter
    assert "badge_con_vhci_epoch_gate_apply" in adapter
    assert "badge_con_vhci_epoch_gate_matches_inhibit" in adapter
    assert "fw_operation_state_try_begin_quiesced" in operation_header
    assert "static bool policy_try_lock(void)" in adapter
    request_adapter = adapter[
        adapter.index("bool badge_con_vhci_request_quiescence") :
        adapter.index("void badge_con_vhci_poll")
    ]
    quiesced_adapter = adapter[
        adapter.index("bool badge_con_vhci_radio_quiesced_for_epoch") :
    ]
    assert "policy_try_lock()" in request_adapter
    assert "policy_try_lock()" in quiesced_adapter

    begin = store[
        store.index("bool fw_store_operation_try_begin") :
        store.index("bool fw_store_operation_end")
    ]
    latch = begin.index("fw_operation_state_request_radio_inhibit")
    operation_unlock = begin.index(
        "portEXIT_CRITICAL(&s_operation_lock)", latch
    )
    request = begin.index(
        "badge_con_vhci_request_quiescence", operation_unlock
    )
    acknowledged = begin.index(
        "badge_con_vhci_radio_quiesced_for_epoch", request
    )
    operation_relock = begin.index(
        "portENTER_CRITICAL(&s_operation_lock)", acknowledged
    )
    exact_claim = begin.index(
        "fw_operation_state_try_begin_quiesced", operation_relock
    )
    assert (
        latch < operation_unlock < request < acknowledged <
        operation_relock < exact_claim
    )

    preempt = store[
        store.index("fw_update_preempt_result_t fw_store_request_update_preemption") :
        store.index("bool fw_store_game_radio_must_yield")
    ]
    assert "badge_con_vhci_request_quiescence" in preempt
    assert "badge_con_vhci_radio_quiesced_for_epoch" in preempt
    assert "verified.operation_epoch != acknowledged_inhibit_epoch" in preempt
    assert "!verified.radio_inhibited" in preempt
    assert "!verified.preemption_requested" in preempt


def test_con_crud_main_radio_runtime_uses_boot_ids_and_fail_busy_uart_gating():
    main = _source("esp32", "uplink", "main", "main.c")
    helper = main[
        main.index("static void badge_con_radio_runtime_poll") :
        main.index("static bool uart_startup_try_claim")
    ]
    display = main[
        main.index("static void display_task") :
        main.index("static void print_banner")
    ]
    app = main[main.index("void app_main(void)") :]

    for token in (
        "fw_store_campaign_state_sample",
        "badge_con_vhci_apply_radio_policy",
        "scanner.boot_id",
        "badge_con_radio_runtime_observe_boot_id",
        "badge_con_runtime_clear_self_ack",
        "badge_con_radio_runtime_retry_self_due",
        "badge_con_vhci_set_self_ready(false)",
        "badge_con_render_self_command",
        "uart_rx_send_command_to_scanner_checked",
        "fw_store_game_radio_must_yield",
    ):
        assert token in helper
    assert "identity_generation" not in helper
    assert helper.index("fw_store_game_radio_must_yield") < helper.index(
        "uart_rx_send_command_to_scanner_checked"
    )

    radio_poll = display.index("badge_con_radio_runtime_poll")
    power_poll = display.index("badge_power_runtime_poll")
    quiet_return = display.index("badge_power_runtime_is_quiet")
    assert radio_poll < power_poll < quiet_return
    quiet_delay = display[
        quiet_return : display.index("continue;", quiet_return)
    ]
    assert "#if defined(FOF_DC34_GAME_CANARY)" in quiet_delay
    assert "vTaskDelay(pdMS_TO_TICKS(BADGE_DISPLAY_UPDATE_MS))" in quiet_delay
    assert "#else" in quiet_delay
    assert "vTaskDelay(pdMS_TO_TICKS(1000))" in quiet_delay

    normal_startup = app.index("/* ── 14. Start all tasks")
    restore = app.index(
        "fw_store_restore_auto_update_coordinator()", normal_startup
    )
    initial_radio = app.index("badge_con_radio_runtime_poll", restore)
    display_wake = app.index(
        "xTaskNotifyGive(s_display_task_handle)", initial_radio
    )
    assert restore < initial_radio < display_wake


def test_con_crud_seed_command_and_usb_status_are_one_snapshot_transactions():
    serial = _source(
        "esp32", "uplink", "main", "core", "serial_config.c"
    )
    setter_at = serial.index("static bool handle_set_command")
    setter = serial[
        setter_at :
        serial.index("static void handle_control_line", setter_at)
    ]
    assert setter.index('strcmp(key, "game_seed") == 0') < (
        setter.index("nvs_config_set_string(key, value)")
    )
    assert "badge_con_role_parse_exact(value, &seed)" in setter
    assert "badge_con_runtime_set_factory_seed(seed)" in setter
    assert 'send_response("FOF_OK:game_seed\\n")' in setter
    assert "FOF_SAVE" not in setter

    minimal = serial[
        serial.index("static bool emit_minimal_status") :
        serial.index("static void send_startup_recovery_status_response")
    ]
    full = serial[
        serial.index("static void send_badge_status_response") :
        serial.index("static void send_control_error")
    ]
    formatter = serial[
        serial.index("static void print_game_status_json") :
        serial.index("static bool render_emit")
    ]
    for field in (
        r'\"game_seed\":',
        r'\"game_state\":',
        r'\"game_active\":',
        r'\"game_shield\":',
    ):
        assert field in formatter
    assert "print_game_status_json(game_status)" in minimal
    assert "print_game_status_json(&game_status)" in full
    assert full.count("serial_game_status_snapshot(") == 1
    assert "emit_minimal_status(" in full


def test_expected_reboot_token_is_published_after_out_of_lock_game_hook():
    runtime = _source(
        "esp32", "uplink", "main", "core", "badge_runtime.c"
    )
    arm_start = runtime.index("badge_runtime_arm_expected_reboot(")
    arm = runtime[
        arm_start :
        runtime.index("void badge_runtime_set_expected_reboot_hook", arm_start)
    ]
    invalidate = arm.index(
        "&g_fof_badge_rtc_state.expected_reboot_magic"
    )
    unlock_for_hook = arm.index("portEXIT_CRITICAL", invalidate)
    hook = arm.index("hook(generation)", unlock_for_hook)
    assert "portENTER_CRITICAL" not in arm[unlock_for_hook:hook]
    relock_after_hook = arm.index("portENTER_CRITICAL", hook)
    bind = arm.index("badge_update_maintenance_marker_arm_reboot(", hook)
    publish_generation = arm.index(
        "g_fof_badge_rtc_state.expected_reboot_generation = generation",
        bind,
    )
    publish_magic = arm.index(
        "BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC",
        publish_generation,
    )
    failed_gate = arm.index("if (!armed)", publish_magic)
    persist_reason = arm.index("nvs_set_string_value", failed_gate)
    assert (
        invalidate
        < unlock_for_hook
        < hook
        < relock_after_hook
        < bind
        < publish_generation
        < publish_magic
        < failed_gate
        < persist_reason
    )
    assert "__ATOMIC_RELEASE" in arm[invalidate:unlock_for_hook]
    assert "__ATOMIC_RELEASE" in arm[publish_generation:failed_gate]

    init = runtime[
        runtime.index("void badge_runtime_init") :
        runtime.index("void badge_runtime_set_pending_verify")
    ]
    transition_helper_start = runtime.index(
        "static badge_runtime_rtc_boot_result_t "
        "boot_rtc_transition_locked(void)"
    )
    transition_helper = runtime[
        transition_helper_start :
        runtime.index(
            "static bool rtc_layout_valid(void)",
            transition_helper_start,
        )
    ]
    transition = re.search(
        r"s_boot_rtc_transition_result\s*=\s*"
        r"badge_runtime_rtc_transition\(\s*"
        r"\(uint8_t\s*\*\)&g_fof_badge_rtc_state\s*,\s*"
        r"sizeof\(g_fof_badge_rtc_state\)\s*,\s*"
        r"reason\s*==\s*ESP_RST_SW\s*,\s*"
        r"\(uint16_t\)sizeof\(g_fof_badge_rtc_state\)\s*\);",
        transition_helper,
    )
    assert transition is not None
    init_transition = re.search(
        r"badge_runtime_rtc_boot_result_t\s+reboot_decision\s*=\s*"
        r"boot_rtc_transition_locked\(\)\s*;",
        init,
    )
    assert init_transition is not None
    consumed_generation = re.search(
        r"uint32_t\s+consumed_expected_generation\s*=\s*"
        r"reboot_decision\.consumed_generation\s*;",
        init,
    )
    assert consumed_generation is not None
    remember_generation = init.index(
        "s_last_expected_reboot_generation = consumed_expected_generation"
    )
    assert (
        init_transition.start()
        < consumed_generation.start()
        < remember_generation
    )
    assert "badge_runtime_expected_reboot_arm_state_init(" not in init
    assert "expected_reboot_magic =" not in init


def test_badge_usb_transport_is_the_only_lifetime_input_owner():
    header = _source("esp32", "uplink", "main", "core", "badge_usb_transport.h")
    transport = _source("esp32", "uplink", "main", "core", "badge_usb_transport.c")
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")
    app_sources = "\n".join(
        path.read_text()
        for path in (REPO_ROOT / "esp32" / "uplink" / "main").rglob("*.c")
    )

    for api in (
        "badge_usb_transport_start(uint32_t boot_window_ms)",
        "badge_usb_transport_wait_boot_window(TickType_t timeout)",
        "badge_usb_transport_set_dispatch_ready(void)",
        "badge_usb_transport_begin_binary(badge_usb_binary_target_t target",
        "badge_usb_transport_emit(const void *data, size_t len",
        "badge_usb_transport_snapshot(badge_usb_health_t *out)",
        "badge_usb_transport_host_active(uint32_t sample_window_ms)",
    ):
        assert api in header
    assert transport.count("usb_serial_jtag_driver_install(&config)") == 1
    start = transport[transport.index("bool badge_usb_transport_start(") :]
    badge_s3_guard = start.index(
        "#if defined(FOF_BADGE_VARIANT) && "
        "defined(CONFIG_IDF_TARGET_ESP32S3)"
    )
    already_installed = start.index(
        "if (usb_serial_jtag_is_driver_installed())", badge_s3_guard
    )
    reenumerate = start.index(
        "if (!app_reenumerate_usb_serial_jtag())", already_installed
    )
    badge_s3_guard_end = start.index("#endif", reenumerate)
    driver_install = start.index(
        "usb_serial_jtag_driver_install(&config)", badge_s3_guard_end
    )
    assert (
        badge_s3_guard
        < already_installed
        < reenumerate
        < badge_s3_guard_end
        < driver_install
    )
    assert ".rx_buffer_size = 8192" in transport
    assert ".tx_buffer_size = 2048" in transport
    assert "err != ESP_OK && err != ESP_ERR_INVALID_STATE" in transport
    assert "usb_serial_jtag_vfs_use_driver();" in transport
    assert "usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);" in transport
    assert "usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);" in transport
    assert "fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK)" in transport
    assert "usb_serial_jtag_ll_read_rxfifo" not in app_sources
    assert "fgetc(stdin)" not in app_sources
    assert len(re.findall(r"read\s*\(\s*STDIN_FILENO\s*,", app_sources)) == 1
    assert "select(STDIN_FILENO" not in serial
    assert "serial_config_listen(" not in serial
    assert transport.count("static void badge_usb_transport_task(void *arg)") == 1
    assert "ESP_ERROR_CHECK" not in transport
    assert "!s_line_buffer" in transport
    assert "if (task_ok != pdPASS)" in transport

    app = _source("esp32", "uplink", "main", "main.c")
    app_main = app[app.index("void app_main(void)") :]
    assert (
        app_main.index("badge_usb_transport_start(3000)")
        < app_main.index("FOF_PRINT_IDENT(TAG, FOF_FIRMWARE_TARGET)")
    )


def test_con_crud_canary_reclaims_only_unused_badge_detection_queue():
    main = _source("esp32", "uplink", "main", "main.c")
    uart = _source("esp32", "uplink", "main", "comms", "uart_rx.c")
    uart_header = _source(
        "esp32", "uplink", "main", "comms", "uart_rx.h"
    )
    serial = _source(
        "esp32", "uplink", "main", "core", "serial_config.c"
    )

    queue_guard = (
        "#if defined(FOF_BADGE_VARIANT) && "
        "defined(FOF_DC34_GAME_CANARY)"
    )
    assert queue_guard in main
    app = main[main.index("void app_main(void)") :]
    queue_declaration = app.index("QueueHandle_t detection_queue = NULL;")
    queue_guard_start = app.rfind(queue_guard, 0, queue_declaration)
    assert queue_guard_start >= 0
    canary_queue = app[
        queue_guard_start :
        app.index('log_detection_queue_heap("after_queue")', queue_declaration)
    ]
    assert "QueueHandle_t detection_queue = NULL;" in canary_queue
    assert "xQueueCreate(" in canary_queue
    assert canary_queue.index("QueueHandle_t detection_queue = NULL;") < (
        canary_queue.index("#else")
    )
    assert canary_queue.index("#else") < canary_queue.index("xQueueCreate(")
    assert "uart_rx_init(detection_queue)" in main

    route_start = uart.index("FOF_SCANNER_UPLINK_ROUTE_DETECTION")
    badge_start = uart.index("#ifdef FOF_BADGE_VARIANT", route_start)
    badge_route = uart[badge_start:uart.index("#endif", badge_start)]
    assert "badge_ingest_detection(&det, &badge_event)" in badge_route
    assert badge_route.index("badge_ingest_detection") < (
        badge_route.index("cJSON_Delete(root)")
    ) < badge_route.index("return;")
    assert uart.index("return;", uart.index("badge_ingest_detection")) < (
        uart.index("xQueueSend(s_detection_queue")
    )

    assert "uart_rx_detection_queue_capacity(void)" in uart_header
    assert "uart_rx_detection_queue_reclaimed_bytes(void)" in uart_header
    assert '\\"detection_queue_capacity\\":%lu' in serial
    assert '\\"detection_queue_reclaimed_bytes\\":%lu' in serial


def test_badge_usb_boot_dispatch_is_gated_until_dependencies_are_ready():
    main = _source("esp32", "uplink", "main", "main.c")
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")
    transport = _source("esp32", "uplink", "main", "core", "badge_usb_transport.c")
    policy = _source("esp32", "shared", "badge_usb_transport_policy.c")
    app = main[main.index("void app_main(void)") :]

    start = app.index("badge_usb_transport_start(3000)")
    assert start < app.index("rollback_check_at_boot()")
    ready = app.index("badge_usb_transport_set_dispatch_ready()")
    for dependency in (
        "nvs_config_init()",
        "fw_store_init_auto_update_coordinator()",
        "badge_runtime_init(s_ota_pending_verify)",
        "oled_init()",
        "uart_rx_scanner_tx_lease_init()",
    ):
        assert app.index(dependency) < ready
    assert ready < app.index("badge_usb_transport_wait_boot_window(")
    assert "serial_config_listen(3000)" not in app
    assert "serial_config_start_control_task()" not in app

    gate = transport[
        transport.index("static bool normal_line_is_recognized") :
        transport.index("static bool emit_upload_terminal")
    ]
    assert "badge_usb_line_dispatch_run(" in gate
    assert ".emit_booting = emit_booting_rejection" in gate
    assert '"FOF_ERROR:booting\\n"' in gate
    assert "serial_config_line_is_recognized(line, line_byte_len)" in gate
    assert "serial_config_dispatch_line(line, line_byte_len)" in gate

    policy_gate = policy[
        policy.index("bool badge_usb_line_dispatch_run(") :
        policy.index("void badge_usb_upload_policy_init")
    ]
    assert "badge_usb_command_decide(" in policy_gate
    assert "BADGE_USB_COMMAND_BOOTING" in policy_gate
    assert policy_gate.index("badge_usb_command_decide(") < policy_gate.index(
        "hooks->dispatch_normal_line"
    )
    assert "serial_config_dispatch_line" in serial


def test_badge_usb_transport_owns_framing_priority_and_completion():
    transport = _source("esp32", "uplink", "main", "core", "badge_usb_transport.c")
    policy = _source("esp32", "shared", "badge_usb_transport_policy.c")
    policy_header = _source("esp32", "shared", "badge_usb_transport_policy.h")
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    assert "badge_usb_stream_feed" in transport
    assert "badge_usb_stream_poll_timeout" in transport
    assert "result.input_consumed" in transport
    assert "BADGE_USB_BINARY_SCANNER" in transport
    assert "BADGE_USB_BINARY_UPLINK" in transport
    assert "xSemaphoreCreateRecursiveMutex" in transport
    assert "xSemaphoreTakeRecursive" in transport
    assert "usb_serial_jtag_write_bytes" in transport
    assert "BADGE_USB_TX_CHUNK_BYTES 2048U" in policy_header
    assert "chunk > BADGE_USB_TX_CHUNK_BYTES" in policy
    assert "written != (int)chunk" in policy
    assert "usb_serial_jtag_wait_tx_done" in transport
    assert "responses_completed++" in transport
    assert "required_response_failures++" in transport
    assert "policy->poisoned" in policy
    assert "memory_order_release" in policy
    assert '"required_response_failed"' in transport
    assert "dropped_progress_frames++" in transport
    assert "dropped_optional_frames++" in transport
    assert "esp_log_set_vprintf" in transport
    assert "BADGE_USB_FRAME_OPTIONAL" in transport
    assert "portMAX_DELAY" not in transport[transport.index("static int transport_log_vprintf") :]

    machine_printf = re.compile(
        r'(?<![A-Za-z_])(?:printf|fprintf|fwrite)\s*\([^\n]*FOF_'
    )
    assert machine_printf.search(store) is None
    assert machine_printf.search(serial) is None
    assert "badge_usb_transport_emit" in store
    assert "BADGE_USB_FRAME_PROGRESS" in store
    for priority in ("BADGE_USB_FRAME_REQUIRED", "BADGE_USB_FRAME_OPTIONAL"):
        assert priority in serial
    assert "BADGE_USB_FRAME_PROGRESS" in store


def test_badge_status_freezes_identity_usb_health_and_bounded_fallback():
    header = _source("esp32", "uplink", "main", "core", "badge_usb_transport.h")
    transport = _source("esp32", "uplink", "main", "core", "badge_usb_transport.c")
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")
    fixture = _usb_health_fixture()

    assert list(fixture) == [
        "schema", "task_started", "host_connected", "parser_state", "rx_bytes",
        "valid_commands", "responses_completed", "required_response_failures",
        "malformed_lines", "dropped_progress_frames", "dropped_optional_frames",
        "upload_received", "upload_size", "task_heartbeat_age_s", "last_rx_age_s",
        "last_command_age_s", "last_response_age_s", "last_upload_progress_age_s",
    ]
    assert fixture["parser_state"] in {"command", "scanner_upload", "uplink_upload"}
    for key, value in fixture.items():
        if key in {"task_started", "host_connected"}:
            assert type(value) is bool
        elif key not in {"parser_state", "last_upload_progress_age_s"}:
            assert type(value) is int and value >= 0
    assert fixture["last_upload_progress_age_s"] is None

    struct_fields = {
        "parser_state": "parser_target",
        "task_heartbeat_age_s": "task_heartbeat_ms",
        "last_rx_age_s": "last_rx_ms",
        "last_command_age_s": "last_command_ms",
        "last_response_age_s": "last_response_ms",
        "last_upload_progress_age_s": "last_upload_progress_ms",
    }
    for field in fixture:
        if field != "schema":
            assert struct_fields.get(field, field) in header
        assert f'\\"{field}\\"' in serial or f'"{field}"' in serial
    for parser_state in ("command", "scanner_upload", "uplink_upload"):
        assert f'"{parser_state}"' in serial or f'"{parser_state}"' in transport
    for identity in (
        '"target"', '"firmware_name"', '"project"', '"app_project"',
        '"hardware_type"', '"hardware_id"', '"running_partition"',
        '"pending_verify"', '"rollback_state"', '"recovery_mode"',
    ):
        escaped_identity = identity.replace('"', '\\"')
        assert identity in serial or escaped_identity in serial
    assert "esp_efuse_mac_get_default" in serial
    assert '%02X:%02X:%02X:%02X:%02X:%02X' in serial
    assert "MALLOC_CAP_SPIRAM" in serial
    assert "BADGE_USB_STATUS_MAX_BYTES 65535" in serial
    assert "minimal_status" in serial
    assert "badge_usb_transport_emit" in serial
    assert "BADGE_USB_FRAME_REQUIRED" in serial
    assert "portENTER_CRITICAL" in transport
    rx_accounting = "s_health.rx_bytes += (uint64_t)bytes_read"
    assert rx_accounting in transport
    rx_index = transport.index(rx_accounting)
    rx_guard = transport[rx_index - 160:rx_index]
    assert "if (bytes_read > 0)" in rx_guard
    assert "host_active" not in rx_guard


def test_required_response_failure_never_completes_command_or_selects_boot():
    header = _source("esp32", "uplink", "main", "core", "badge_usb_transport.h")
    transport = _source("esp32", "uplink", "main", "core", "badge_usb_transport.c")
    policy = _source("esp32", "shared", "badge_usb_transport_policy.c")
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")
    recovery = _source("esp32", "uplink", "main", "core", "badge_usb_recovery.c")

    required = transport[
        transport.index("static badge_usb_emit_result_t badge_usb_transport_emit_detailed") :
        transport.index("bool badge_usb_transport_emit")
    ]
    failure_helper = transport[
        transport.index("static void health_note_required_failure") :
        transport.index("static bool output_host_connected")
    ]
    clear_helper = transport[
        transport.index("static void health_clear_enqueued_responses") :
        transport.index("static void health_note_required_failure")
    ]
    assert "enqueued_required_responses > 0U" in clear_helper
    assert "responses_completed" in clear_helper
    assert "last_response_ms = now_ms()" in clear_helper
    assert "UINT32_MAX" in clear_helper
    assert "required_response_failures++" in failure_helper
    assert "result == BADGE_USB_EMIT_ENQUEUED" in failure_helper
    assert "enqueued_required_responses++" in failure_helper
    assert "hard_unanswered_required_responses++" in failure_helper
    health_apply = transport[
        transport.index("static void health_apply_emit_result") :
        transport.index("static bool output_host_connected")
    ]
    assert "health_note_required_failure(result)" in health_apply
    for field in (
        "hard_unanswered_required_responses",
        "enqueued_required_responses",
        "oldest_hard_unanswered_response_ms",
        "oldest_enqueued_response_ms",
    ):
        assert field in header
    assert "health_clear_enqueued_responses();" in health_apply
    drain = transport[
        transport.index("bool badge_usb_transport_drain") :
        transport.index("static int transport_log_vprintf")
    ]
    assert "if (drained)" in drain
    assert "health_clear_enqueued_responses();" in drain
    required_take = required.index("xSemaphoreTakeRecursive(")
    required_emit = required.index("badge_usb_output_emit(")
    required_health = required.index(
        "health_apply_emit_result(result, priority, health_mode)", required_emit
    )
    required_give = required.index(
        "xSemaphoreGiveRecursive(", required_health
    )
    assert required_take < required_emit < required_health < required_give
    drain_wait = drain.index("usb_serial_jtag_wait_tx_done(")
    drain_health = drain.index(
        "health_clear_enqueued_responses();", drain_wait
    )
    drain_give = drain.index("xSemaphoreGiveRecursive(", drain_health)
    assert drain_wait < drain_health < drain_give
    assert "responses_completed++" in health_apply
    assert "hooks->drain" in policy
    bootloader = serial[
        serial.index("static void reboot_to_download_mode") :
        serial.index("static void reboot_app")
    ]
    assert "if (!render_emit(BADGE_USB_FRAME_REQUIRED" in bootloader
    assert "return;" in bootloader
    reboot = bootloader.index("badge_usb_recovery_restart(")
    assert bootloader.index("return;") < reboot
    assert "BADGE_USB_RESET_ROM" in bootloader[reboot:]
    assert '"usb_bootloader"' in bootloader[reboot:]
    assert "if (!badge_usb_recovery_restart(" in bootloader
    assert "REG_WRITE(RTC_CNTL_OPTION1_REG" in recovery
    for boot_call in ("esp_ota_mark_app_invalid",):
        call = serial.index(boot_call)
        assert serial.rindex("badge_usb_transport_emit", 0, call) < call
        assert "if (!" in serial[serial.rindex("badge_usb_transport_emit", 0, call) - 80:call]


def test_usb_transport_runtime_uses_host_tested_policy_and_fresh_timeout_clock():
    transport = _source("esp32", "uplink", "main", "core", "badge_usb_transport.c")

    assert '#include "badge_usb_transport_policy.h"' in transport
    assert "badge_usb_output_emit(" in transport
    assert "USB_BINARY_IDLE_TIMEOUT_MS 30000" not in transport
    assert "BADGE_USB_BINARY_IDLE_TIMEOUT_MS" in transport
    task = transport[
        transport.index("static void badge_usb_transport_task(void *arg)") :
        transport.index("bool badge_usb_transport_start(")
    ]
    assert task.index("uint32_t timeout_now_ms = (uint32_t)now_ms();") \
        < task.index("badge_usb_stream_poll_timeout(")


def test_usb_ready_is_not_unsolicited_and_scanner_failures_keep_badge_recovery_ready():
    transport = _source("esp32", "uplink", "main", "core", "badge_usb_transport.c")
    policy = _source("esp32", "shared", "badge_usb_transport_policy.c")
    transport_header = _source(
        "esp32", "uplink", "main", "core", "badge_usb_transport.h"
    )
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")
    ingress = _source(
        "esp32", "uplink", "main", "core", "serial_config_ingress.c"
    )
    control_schema = _source(
        "esp32", "uplink", "main", "core", "badge_usb_control_schema.c"
    )
    serial_header = _source("esp32", "uplink", "main", "core", "serial_config.h")
    main = _source("esp32", "uplink", "main", "main.c")
    set_ready = transport[
        transport.index("void badge_usb_transport_set_dispatch_ready") :
        transport.index("bool badge_usb_transport_begin_binary")
    ]

    assert '"FOF_READY\\n"' in set_ready
    assert "BADGE_USB_FRAME_OPTIONAL" in set_ready
    assert "BADGE_USB_FRAME_REQUIRED" not in set_ready
    assert "pdMS_TO_TICKS(250)" in set_ready

    coordinator_failure = main[
        main.index("if (!fw_store_init_auto_update_coordinator())") :
        main.index('log_detection_queue_heap("after_nvs")')
    ]
    queue_failure = main[
        main.index("if (!detection_queue)") :
        main.index('log_detection_queue_heap("after_queue")')
    ]
    coordinator_badge = coordinator_failure[
        coordinator_failure.index("#ifdef FOF_BADGE_VARIANT") :
        coordinator_failure.index("#else")
    ]
    queue_badge = queue_failure[
        queue_failure.index("#ifdef FOF_BADGE_VARIANT") :
        queue_failure.index("#else")
    ]
    assert "return;" not in coordinator_badge
    assert "return;" not in queue_badge
    assert "badge_startup_safe_reason" in coordinator_failure
    assert "badge_startup_safe_reason" in queue_failure

    runtime_init = main.index("badge_runtime_init(s_ota_pending_verify)")
    display_init = main.index("oled_init();", runtime_init)
    startup_safe = main[runtime_init:display_init]
    assert "badge_runtime_force_safe_mode(true, badge_startup_safe_reason)" \
        in startup_safe

    lease_failure = main.index("if (!uart_rx_scanner_tx_lease_init())")
    ready_call = main.index("badge_usb_transport_set_dispatch_ready()")
    gate = main[lease_failure:ready_call + len("badge_usb_transport_set_dispatch_ready()")]
    assert 'badge_runtime_force_safe_mode(true, "scanner_uart_lease")' in gate
    assert "badge_safe_usb = true" in gate
    assert "badge_startup_recovery_only = true" in gate
    assert "badge_usb_transport_set_recovery_only(" in gate
    assert "bool usb_dispatch_ready = usb_transport_started" in gate
    assert "if (usb_dispatch_ready)" in gate
    dispatch_assignment = gate.index("bool usb_dispatch_ready")
    badge_dispatch_start = gate.rindex(
        "#ifdef FOF_BADGE_VARIANT", 0, dispatch_assignment
    )
    badge_dispatch = gate[
        badge_dispatch_start : gate.index("#else", badge_dispatch_start)
    ]
    assert "required_tasks_started" not in badge_dispatch

    # Scanner-dependent startup remains before readiness, but failure now
    # selects the recovery-only badge surface instead of hiding it.
    assert main.index("xQueueCreate(") < display_init < lease_failure < ready_call
    assert main.index("uart_rx_init(detection_queue)") < ready_call

    recovery_hold = main[
        main.index("if (badge_startup_recovery_only)", ready_call) :
        main.index("esp_event_loop_create_default()")
    ]
    assert "return;" in recovery_hold
    assert "rollback_and_reboot_or_restart" not in recovery_hold
    assert "xTaskCreate(" not in recovery_hold
    assert main.index("oled_badge_buttons_start()") < ready_call
    assert ready_call < main.index("if (badge_startup_recovery_only)", ready_call)

    assert "void badge_usb_transport_set_recovery_only(bool enabled);" \
        in transport_header
    dispatch = transport[
        transport.index("static bool normal_line_is_recognized") :
        transport.index("static bool emit_upload_terminal")
    ]
    assert "serial_config_recovery_command_classify(line, line_byte_len)" in dispatch
    assert ".recovery_line_is_allowed = recovery_line_is_allowed" in dispatch
    assert (
        "serial_config_dispatch_recovery_command(line, line_byte_len)"
        in dispatch
    )
    assert 'FOF_ERROR:{\\"ok\\":false,\\"error\\":\\"startup_recovery_only\\"}' \
        in dispatch
    assert "s_health.valid_commands++" in dispatch
    assert "BADGE_USB_FRAME_REQUIRED" in dispatch

    policy_dispatch = policy[
        policy.index("bool badge_usb_line_dispatch_run(") :
        policy.index("void badge_usb_upload_policy_init")
    ]
    assert "BADGE_USB_COMMAND_RECOVERY_ONLY" in policy_dispatch
    assert "hooks->note_recognized(hooks->context)" in policy_dispatch
    assert "hooks->dispatch_recovery_line(" in policy_dispatch
    assert "hooks->dispatch_normal_line(" in policy_dispatch
    assert policy_dispatch.index("if (recovery_only)") < policy_dispatch.index(
        "decision != BADGE_USB_COMMAND_DISPATCH"
    )

    assert "serial_config_recovery_command_t" in serial_header
    assert "serial_config_dispatch_recovery_command(" in serial_header
    classifier = ingress[
        ingress.index("serial_config_recovery_command_t ") :
    ]
    for command in (
        "CMD_PING", "CMD_STATUS", "CMD_REBOOT", "CMD_BOOTLOADER",
        "CMD_DOWNLOAD", "CMD_FLASH",
    ):
        assert command in ingress
    for accepted in (
        "SERIAL_CONFIG_INGRESS_PING",
        "SERIAL_CONFIG_INGRESS_STATUS",
        "SERIAL_CONFIG_INGRESS_REBOOT",
        "BADGE_USB_CONTROL_SCHEMA_STATUS",
        "BADGE_USB_CONTROL_SCHEMA_REBOOT",
        "schema_is_uplink_ota_begin(",
    ):
        assert accepted in classifier
    assert "FOF_FW_JSON_SCHEMA_USB_UPLINK_OTA_BEGIN" in ingress
    assert "FOF_FW_JSON_SCHEMA_USB_UPLINK_OTA_BEGIN_SESSION" in ingress
    assert '"status"' in control_schema
    assert '"reboot"' in control_schema
    assert '"bootloader"' not in control_schema
    assert '"ota"' not in control_schema
    recovery_dispatch = serial[
        serial.index("bool serial_config_dispatch_recovery_command") :
        serial.index("bool serial_config_dispatch_line")
    ]
    assert "serial_config_recovery_command_classify(line, line_byte_len)" \
        in recovery_dispatch
    assert "serial_config_dispatch_uplink_ota_begin(" in recovery_dispatch
    assert "execute_recovery_command(command)" in recovery_dispatch
    recovery_effects = serial[
        serial.index("static bool execute_recovery_command") :
        serial.index("bool serial_config_dispatch_recovery_command")
    ]
    assert "send_startup_recovery_status_response()" in recovery_effects
    assert "send_badge_status_response()" not in recovery_effects
    assert "reboot_app()" in recovery_effects
    assert "reboot_to_download_mode()" in recovery_effects


def test_usb_valid_command_counter_counts_only_classified_commands():
    transport = _source("esp32", "uplink", "main", "core", "badge_usb_transport.c")
    policy = _source("esp32", "shared", "badge_usb_transport_policy.c")
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")
    ingress = _source(
        "esp32", "uplink", "main", "core", "serial_config_ingress.c"
    )
    header = _source("esp32", "uplink", "main", "core", "serial_config.h")
    dispatch = transport[
        transport.index("static bool normal_line_is_recognized") :
        transport.index("static bool consume_binary_event")
    ]

    assert "serial_config_recovery_command_classify(line, line_byte_len)" \
        in dispatch
    assert "serial_config_line_is_recognized(line, line_byte_len)" in dispatch
    assert "s_health.valid_commands++" in dispatch
    assert ".note_recognized = note_recognized_line" in dispatch

    policy_dispatch = policy[
        policy.index("bool badge_usb_line_dispatch_run(") :
        policy.index("void badge_usb_upload_policy_init")
    ]
    recognition = policy_dispatch.index("recognized =")
    decision = policy_dispatch.index("badge_usb_command_decide(")
    valid = policy_dispatch.index("hooks->note_recognized(")
    rejection = policy_dispatch.index("decision != BADGE_USB_COMMAND_DISPATCH")
    assert recognition < decision < valid < rejection
    accounting = policy_dispatch[decision:rejection]
    assert "if (recognized && dispatch_ready)" in accounting
    assert "const uint8_t *line" in header
    assert "size_t line_byte_len" in header

    normal_dispatch_at = serial.index("bool serial_config_dispatch_line(")
    normal_dispatch = serial[
        normal_dispatch_at :
        serial.index("static void print_json_escaped_string", normal_dispatch_at)
    ]
    authorize = normal_dispatch.index("serial_config_ingress_authorize(")
    effect = normal_dispatch.index("handle_control_line(")
    assert authorize < effect
    assert "handle_control_line(line, line_byte_len, &ingress)" \
        in normal_dispatch
    authorized_effects = serial[
        serial.index("static void handle_control_line(") :
        normal_dispatch_at
    ]
    assert "ingress->control_schema_id" in authorized_effects
    assert "ingress->control_handler_kind" in authorized_effects
    assert 'SPAN_LITERAL("uplink_ota_begin")' in ingress
    assert "serial_config_ingress_is_uplink_ota_begin(" in ingress

    classifier_body = ingress[
        ingress.index("bool serial_config_ingress_authorize(") :
    ]
    assert "badge_usb_control_select_and_validate(" in classifier_body
    assert "fof_fw_json_select_and_validate(" in classifier_body
    assert "serial_config_ingress_parse_set(" in classifier_body
    assert "cJSON_Parse" not in classifier_body


def test_usb_stream_feed_samples_clock_after_each_dispatch():
    transport = _source("esp32", "uplink", "main", "core", "badge_usb_transport.c")
    task = transport[
        transport.index("static void badge_usb_transport_task") :
        transport.index("bool badge_usb_transport_start")
    ]
    feed = task[
        task.index("while (offset < (size_t)bytes_read)") :
        task.index("if (bytes_read <= 0)")
    ]

    fresh_clock = "uint32_t feed_now_ms = (uint32_t)now_ms();"
    assert fresh_clock in feed
    assert feed.index(fresh_clock) < feed.index("badge_usb_stream_feed(")
    assert "(uint32_t)tick_ms, &result" not in feed


def test_usb_terminal_delivery_controls_scanner_activation_and_cleanup_order():
    transport = _source("esp32", "uplink", "main", "core", "badge_usb_transport.c")
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    header = _source("esp32", "uplink", "main", "network", "fw_store.h")

    detailed = transport[
        transport.index(
            "static badge_usb_emit_result_t emit_upload_terminal_detailed"
        ) :
        transport.index("static bool emit_upload_terminal")
    ]
    assert "badge_usb_transport_emit_detailed(" in detailed
    assert "BADGE_USB_FRAME_REQUIRED" in detailed
    terminal = transport[
        transport.index("static bool emit_upload_terminal") :
        transport.index("static bool consume_binary_event")
    ]
    assert "emit_upload_terminal_detailed(target, json)" in terminal
    assert "BADGE_USB_EMIT_COMPLETED" in terminal

    consume_start = transport.index("static bool consume_binary_event")
    consume = transport[
        consume_start : transport.index("static void abort_binary_target", consume_start)
    ]
    finalize = consume.index("fw_store_serial_upload_end(")
    emit = consume.index("emit_upload_terminal(", finalize)
    activate = consume.index("fw_store_serial_upload_complete_terminal(", emit)
    assert finalize < emit < activate
    assert "terminal_delivered" in consume[emit:activate + 120]
    assert "badge_usb_upload_terminal_result" in consume

    assert "fw_store_serial_upload_complete_terminal(bool delivered)" in header
    end = store[
        store.index("bool fw_store_serial_upload_end") :
        store.index("bool fw_store_serial_upload_complete_terminal")
    ]
    for forbidden in (
        "auto_coordinator_reprompt_requested",
        "auto_coordinator_start_worker",
        "memset(&s_serial_upload",
    ):
        assert forbidden not in end

    complete = store[
        store.index("bool fw_store_serial_upload_complete_terminal") :
        store.index("bool fw_store_get_info")
    ]
    failure = complete[complete.index("if (!delivered)") :]
    assert "poison_staged_image" in failure
    assert "invalidate_fw_metadata" in failure
    assert "auto_coordinator_force_fail_closed" in failure
    success = complete[:complete.index("if (!delivered)")]
    assert "auto_coordinator_start_worker" not in success
    assert complete.index("if (!delivered)") < complete.index("auto_coordinator_start_worker")


def test_usb_scanner_generation_is_durably_unarmed_until_terminal_delivery():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    persist = store[
        store.index("static bool persist_validated_metadata") :
        store.index("const esp_partition_t *fw_store_get_target_partition")
    ]
    barrier = persist.index("auto_coordinator_initialize_fail_closed(")
    manifest_valid = persist.index(
        "nvs_config_set_u32(NVS_FW_VALID, FW_MANIFEST_COMMITTED_MAGIC)"
    )
    assert "bool terminal_gate" in persist
    assert barrier < manifest_valid

    end = store[
        store.index("bool fw_store_serial_upload_end") :
        store.index("bool fw_store_serial_upload_complete_terminal")
    ]
    assert "persist_validated_metadata(&info, true)" in end
    assert "auto_coordinator_begin_generation(" not in end

    complete = store[
        store.index("bool fw_store_serial_upload_complete_terminal") :
        store.index("bool fw_store_get_info")
    ]
    arm = complete.index("auto_coordinator_begin_generation(")
    safe_state = complete.index(
        "defer_scanner_activation = badge_runtime_is_safe_mode()"
    )
    safe_guard = complete.index("if (defer_scanner_activation)")
    normal_worker = complete.index(
        "if (!auto_coordinator_start_worker())", safe_guard
    )
    release = complete.index(
        "auto_coordinator_release_excluded_slots()", safe_guard
    )
    reprompt = complete.index(
        "auto_coordinator_reprompt_requested()", release
    )
    assert complete.index("if (!delivered)") < arm
    assert safe_state < arm < safe_guard < release < reprompt < normal_worker
    deferred = complete[safe_guard:release]
    assert "return true;" in deferred
    assert "readiness_probe_attempts" not in deferred
    assert "auto_coordinator_reprompt_requested" not in deferred
    assert "auto_coordinator_start_worker" not in deferred

    restore = store[
        store.index("bool fw_store_restore_auto_update_coordinator") :
        store.index("void fw_store_handle_scanner_check")
    ]
    unarmed = restore.index("if (blob.fail_closed)")
    reprompt = restore.index("auto_coordinator_reprompt_requested()", unarmed)
    assert "return true;" in restore[unarmed:reprompt]
    assert "auto_coordinator_start_worker()" in restore[reprompt:]


def test_terminal_stage_fences_only_targeted_pause_acknowledged_uart_backlog():
    uart_header = _source("esp32", "uplink", "main", "comms", "uart_rx.h")
    uart_rx = _source("esp32", "uplink", "main", "comms", "uart_rx.c")
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    flush_api = "uart_rx_discard_scanner_backlog_guarded"
    signature = (
        f"bool {flush_api}(int scanner_id,\n"
        "    const uart_rx_pause_guard_t *guard)"
    )
    assert signature in uart_header
    assert signature in uart_rx
    flush_helper = uart_rx[
        uart_rx.index(signature) :
        uart_rx.index(
            "void uart_rx_resume_scanner_guarded(",
            uart_rx.index(signature),
        )
    ]
    assert "guard->acquired" in flush_helper
    assert "guard->request_generation == 0U" in flush_helper
    request = flush_helper.index("pause_request_for_scanner(scanner_id)")
    acknowledgement = flush_helper.index(
        "pause_ack_for_scanner(scanner_id)", request
    )
    assert (
        "request_generation != guard->request_generation"
        in flush_helper
    )
    assert "ack_generation != guard->request_generation" in flush_helper
    retained = flush_helper.index(
        "request_generation == guard->request_generation"
    )
    retained_ack = flush_helper.index(
        "ack_generation == guard->request_generation", retained
    )
    flush = flush_helper.index("uart_flush_input(")
    assert flush < retained < retained_ack

    complete = store[
        store.index("bool fw_store_serial_upload_complete_terminal") :
        store.index("bool fw_store_get_info")
    ]
    first_flush = complete.index(f"{flush_api}(")
    drain = complete.index(
        "FW_SERIAL_IDENTITY_FENCE_DRAIN_MS", first_flush
    )
    second_flush = complete.index(f"{flush_api}(", first_flush + 1)
    arm = complete.index("auto_coordinator_begin_generation(", second_flush)
    resume = complete.index("serial_upload_resume_inputs()", arm)
    operation_end = complete.index("fw_store_operation_end(", resume)
    assert first_flush < drain < second_flush < arm < resume < operation_end
    flush_window = complete[
        complete.rfind("for (int scanner_id = 0", 0, first_flush) : arm
    ]
    assert "uint8_t bit = (uint8_t)(1u << scanner_id)" in flush_window
    assert "if (!(target_slot_mask & bit))" in flush_window
    assert "uart_flush_input(" not in flush_window


def test_stage_owned_pause_and_fence_failure_invalidate_and_fail_closed():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    begin = store[
        store.index("bool fw_store_serial_upload_begin") :
        store.index("bool fw_store_serial_upload_write")
    ]
    guarded_pause = begin.index("uart_rx_pause_scanner_guarded(")
    invalidate = begin.index("invalidate_fw_metadata()")
    assert guarded_pause < invalidate
    assert "s_serial_upload.scanner_pause_guards[scanner_id]" in begin
    pause_failure = begin[
        guarded_pause : begin.index("invalidate_fw_metadata()")
    ]
    assert "return false;" in pause_failure
    assert "serial_upload_resume_inputs()" in pause_failure

    complete = store[
        store.index("bool fw_store_serial_upload_complete_terminal") :
        store.index("bool fw_store_get_info")
    ]
    flush_api = "uart_rx_discard_scanner_backlog_guarded"
    assert f"if (!{flush_api}(" in complete
    flush_failure = complete.index(f"if (!{flush_api}(")
    failure = complete[
        flush_failure : complete.index("return false;", flush_failure)
    ]

    poison = failure.index("poison_staged_image(partition)")
    invalidate = failure.index("invalidate_fw_metadata()", poison)
    fail_closed = failure.index(
        "auto_coordinator_force_fail_closed(", invalidate
    )
    resume = failure.index("serial_upload_resume_inputs()", fail_closed)
    assert poison < invalidate < fail_closed < resume
    assert "fw_store_operation_end(" in failure[resume:]


def test_normal_usb_worker_dependency_defers_without_revoking_generation():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    complete = store[
        store.index("bool fw_store_serial_upload_complete_terminal") :
        store.index("bool fw_store_get_info")
    ]
    normal = complete[
        complete.index("if (defer_scanner_activation)") :
    ]

    worker_guard = normal.index("if (!auto_coordinator_start_worker())")
    worker_deferred = normal[
        worker_guard : normal.index("return true;", worker_guard)
    ]
    for destructive_cleanup in (
        "poison_staged_image(partition)",
        "invalidate_fw_metadata()",
        "auto_coordinator_force_fail_closed(",
    ):
        assert destructive_cleanup not in worker_deferred
    assert "coordinator kick" in worker_deferred
    assert "deferred" in worker_deferred

    release = normal.index("auto_coordinator_release_excluded_slots()")
    reprompt = normal.index("auto_coordinator_reprompt_requested()", release)
    assert release < reprompt < worker_guard
    assert "generation %lu" in worker_deferred


def test_usb_manual_relay_is_generation_and_hardware_bound_before_mutation():
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    header = _source("esp32", "uplink", "main", "network", "fw_store.h")
    prepare = _source(
        "esp32", "uplink", "main", "network",
        "fw_relay_prepare_adapter.c",
    )
    prepare_flow = prepare[
        prepare.index("fw_relay_prepare_result_t fw_relay_prepare_for_scanner") :
        prepare.index("bool fw_relay_prepared_release")
    ]

    relay = serial[
        serial.index('} else if (strcmp(cmd, "fw_relay") == 0)') :
        serial.index(
            "} else if (control_handler_kind ==\n"
            "               BADGE_USB_CONTROL_HANDLER_REBOOT)"
        )
    ]
    for required in (
        '"expected_generation"',
        '"expected_hardware_id"',
        "serial_json_uint32_exact",
        "fw_store_relay_staged_to_scanner_bound",
    ):
        assert required in relay
    assert "fw_store_relay_staged_to_scanner_ex(" not in relay
    assert "fw_store_relay_staged_to_scanner_bound" in header

    bound = store[
        store.index("bool fw_store_relay_staged_to_scanner_bound") :
        store.index("#define FW_AUTO_COORDINATOR_MAGIC")
    ]
    assert "expected_generation" in bound
    assert "expected_hardware_id" in bound
    assert "fw_relay_stored_to_scanner(scanner_id, expected_generation," in bound
    for field in (
        r'\"phase\":\"final\"',
        r'\"slot\":\"%s\"',
        r'\"uart\":\"%s\"',
        r'\"generation\":%lu',
        r'\"hardware_id\":\"%s\"',
        r'\"size\":%lu',
        r'\"bytes\":%lu',
        r'\"chunks\":%d',
        r'\"stage\":\"done\"',
        r'\"done\":true',
        r'\"error\":\"\"',
    ):
        assert field in bound

    core = store[
        store.index("static bool fw_relay_stored_to_scanner(") :
        store.index("static esp_err_t fw_relay_handler")
    ]
    assert "fof_firmware_bound_relay_request_matches" in core
    assert core.index("fw_relay_prepare_for_scanner") < core.index(
        "fof_firmware_bound_relay_request_matches"
    )
    assert prepare_flow.index("hooks->token_acquire") < prepare_flow.index(
        "hooks->read_committed"
    )
    assert prepare_flow.index(
        "out->generation != expected_generation"
    ) < (
        prepare_flow.index("hooks->partition_for_snapshot")
    )
    assert "bound MAC %s" not in core
    assert "MAC=%s" not in core


def test_scanner_usb_staging_credit_v1_is_opt_in_durable_and_legacy_safe():
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")
    transport = _source(
        "esp32", "uplink", "main", "core", "badge_usb_transport.c"
    )
    transport_header = _source(
        "esp32", "uplink", "main", "core", "badge_usb_transport.h"
    )
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    begin = serial[
        serial.index("static void handle_fw_upload_begin") :
        serial.index("static void handle_scanner_display_control")
    ]
    assert '"flow_control"' in begin
    assert '"credit-v1"' in begin
    assert "badge_usb_transport_begin_scanner_binary(" in begin
    assert "bool credit_v1" in transport_header

    for exact_fragment in (
        r'\"flow_control\":\"credit-v1\"',
        r'\"phase\":\"%s\"',
        r'\"phase\":\"final\"',
        r'\"received\":%lu',
        r'\"total\":%lu',
        r'\"credit_bytes\":%lu',
    ):
        assert exact_fragment in store
    assert '"ready", 0U, s_serial_upload.next_credit_at' in store
    assert '"credit", s_serial_upload.received, next_credit' in store

    credited = transport[
        transport.index("typedef struct scanner_credit_context") :
        transport.index("static void badge_usb_transport_task(")
    ]
    for hook in (
        ".write_durable = scanner_credit_write_durable",
        ".commit_transport = scanner_credit_commit_transport",
        ".finalize_durable = scanner_credit_finalize_durable",
        ".emit_required = scanner_credit_emit_required",
        ".drain_required = scanner_credit_drain_required",
        ".complete_terminal = scanner_credit_complete_terminal",
    ):
        assert hook in credited
    assert "badge_usb_scanner_credit_process(" in credited
    assert "badge_usb_scanner_credit_result_error(result)" in credited

    task = transport[
        transport.index("static void badge_usb_transport_task(") :
        transport.index("bool badge_usb_transport_start(")
    ]
    assert "badge_usb_upload_credit_enabled(&s_upload_policy)" in task
    assert "consume_scanner_credit_bytes(" in task
    assert "badge_usb_stream_feed(" in task
    assert "badge_usb_stream_poll_timeout(" in task


def test_usb_upload_cleanup_precedes_coalesced_trailing_command_dispatch():
    transport = _source("esp32", "uplink", "main", "core", "badge_usb_transport.c")
    consume = transport[
        transport.index("static bool consume_binary_event") :
        transport.index("static void badge_usb_transport_task")
    ]

    assert "static badge_usb_binary_target_t clear_upload_health" in transport
    complete = consume[consume.index("BADGE_USB_EVENT_BINARY_COMPLETE") :]
    clear = complete.index("clear_upload_health(")
    terminal = complete.index("emit_upload_terminal(")
    assert clear < terminal


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
    assert 'send_fw_check(board_name, caps, "boot")' in main
    assert "FW_CHECK_PERIODIC_INTERVAL_MS" not in main
    assert "FW_CHECK_JITTER_MAX_MS" not in main
    assert "FW_UPDATE_RETRY_INTERVAL_MS" not in main
    assert 'send_fw_check(board_name, caps, "periodic")' not in main
    assert 'send_fw_check(board_name, caps, "pending_update_retry")' not in main
    assert "FW_CHECK_DAILY_INTERVAL_MS" not in main


def test_manual_checks_cannot_interleave_with_relay_and_uplink_has_no_timer():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    header = _source("esp32", "uplink", "main", "network", "fw_store.h")
    main = _source("esp32", "uplink", "main", "main.c")
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")

    assert "fw_store_request_scanner_checks" in header
    helper = store[store.index("uint8_t fw_store_request_scanner_checks") :]
    assert "fw_store_operation_is_active()" in helper
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
    policy = _source("esp32", "shared", "firmware_auto_policy.c")

    helper_start = store.index("static bool auto_reopen_terminal_for_newer_check")
    helper_end = store.index("static void auto_reset_ready_queue_after_revalidation_failure", helper_start)
    helper = store[helper_start:helper_end]
    assert "fof_auto_terminal_reopen_allowed(" in helper
    assert "FW_AUTO_IDENTITY_WAIT_EXHAUSTED" in helper
    assert "FW_AUTO_RELAY_MAX_ATTEMPTS" in helper
    assert "FW_AUTO_SLOT_AWAITING_CHECK" in helper
    assert 'strcmp(check_reason, "periodic")' not in helper
    assert 'strcmp(check_reason, "boot")' in helper
    assert 'strcmp(check_reason, "manual")' in helper
    assert 'strcmp(check_reason, "pending_update_retry")' not in helper
    assert "auto_coordinator_save_locked" in helper
    assert "state == FOF_AUTO_SLOT_REFUSED" in policy
    assert "state == FOF_AUTO_SLOT_FAILED" in policy
    assert "!identity_exhausted" in policy
    assert "attempts_used < max_attempts" in policy

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
    header = _source("esp32", "uplink", "main", "network", "fw_store.h")
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    assert "fw_store_activity_t fw_store_activity_sample(void)" in header
    sample_start = store.index("fw_store_activity_t fw_store_activity_sample(void)")
    sample_end = store.index("bool fw_store_is_relay_active(void)", sample_start)
    sample = store[sample_start:sample_end]
    assert "fw_store_operation_is_active()" in sample
    assert "auto_coordinator_lock()" in sample
    assert "s_auto_relay_worker_running" not in sample
    assert sample.count("return FW_STORE_ACTIVITY_UNKNOWN;") >= 2
    assert "return FW_STORE_ACTIVITY_ACTIVE;" in sample
    assert "return FW_STORE_ACTIVITY_INACTIVE;" in sample

    start = store.index("bool fw_store_is_relay_active(void)")
    end = store.index("\n}", start) + 2
    helper = store[start:end]
    assert "fw_store_activity_sample()" not in helper
    assert "fw_store_operation_is_active()" in helper
    assert "s_auto_coordinator_lifecycle" in helper
    assert "AUTO_COORDINATOR_RUNNING" in helper
    assert "AUTO_COORDINATOR_QUIESCING" in helper
    assert "auto_coordinator_lock()" in helper
    missing_mutex = helper[
        helper.index("if (!s_auto_coordinator_mutex)") :
        helper.index("if (!auto_coordinator_lock())")
    ]
    assert "return false;" in missing_mutex
    lock_failure = helper[
        helper.index("if (!auto_coordinator_lock())") :
        helper.index("bool worker_running")
    ]
    assert "return true;" in lock_failure


def test_auto_relay_worker_has_stack_for_manifest_bound_uart_protocol():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    assert "#define FW_AUTO_RELAY_TASK_STACK_SIZE 12288" in store
    assert "s_auto_relay_task_stack[FW_AUTO_RELAY_TASK_STACK_SIZE]" in store
    creator_start = store.index("bool fw_store_start_auto_update_coordinator")
    creator = store[
        creator_start :
        store.index(
            "static bool auto_coordinator_reprompt_requested(void)\n{",
            creator_start,
        )
    ]
    assert "xTaskCreateStatic(" in creator
    assert "s_auto_relay_task_stack" in creator
    assert "s_auto_relay_task_tcb" in creator
    assert "xTaskCreate(" not in creator
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
    assert "s_rx_pause_ack_generation_ble" in uart_rx
    assert "s_rx_pause_ack_generation_wifi" in uart_rx
    assert "atomic_store_explicit(pause_ack, request_generation" in uart_rx
    assert "ack_generation == request_generation" in uart_rx
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
        uart_rx.index(
            "decision.route == FOF_SCANNER_UPLINK_ROUTE_SCAN_PROFILE_ACK"
        ) :
        uart_rx.index(
            "decision.route == FOF_SCANNER_UPLINK_ROUTE_DISPLAY_CONTROL_ACK"
        )
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
    usb_window = app.index("badge_usb_transport_wait_boot_window(")
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
        uplink_rx.index(
            "decision.firmware_schema_id ==\n"
            "             FOF_FW_JSON_SCHEMA_RECEIPT_FW_READY_STRICT"
        ) :
        uplink_rx.index(
            "decision.route == FOF_SCANNER_UPLINK_ROUTE_CAL_MODE_ACK"
        )
    ]
    common_parser = uplink_rx[
        uplink_rx.index("static bool fw_ready_common_fields_valid") :
        uplink_rx.index("static void json_copy_string")
    ]
    assert common_parser.count("json_get_uint32_exact(") == 2
    assert ready_branch.count("json_get_uint32_exact(") >= 1
    assert 'root, "allow_same_version")' in ready_branch
    assert "cJSON_IsFalse(allow_same_j)" in ready_branch


def test_manual_firmware_check_sends_fresh_identity_before_check_receipt():
    scanner_main = _source("esp32", "scanner", "main", "main.c")
    manual_start = scanner_main.index(
        "FOF_FW_JSON_SCHEMA_SCANNER_FW_CHECK_NOW"
    )
    manual = scanner_main[
        manual_start :
        scanner_main.index(
            "FOF_SCANNER_COMMAND_BLE_INVESTIGATE",
            manual_start,
        )
    ]

    assert "uart_tx_send_scanner_info(" in manual
    identity = manual.index("uart_tx_send_scanner_info(")
    check = manual.index("send_fw_check(", identity)
    assert identity < check


def test_firmware_ready_is_followed_by_fresh_identity_for_relay_proof():
    scanner_main = _source("esp32", "scanner", "main", "main.c")
    offer = scanner_main[
        scanner_main.index("static void handle_fw_offer") :
        scanner_main.index(
            "typedef struct {",
            scanner_main.index("static void handle_fw_offer"),
        )
    ]

    ready = offer.index("uart_tx_send_raw_json(ready);")
    assert "uart_tx_send_scanner_info(" in offer[ready:]
    identity = offer.index("uart_tx_send_scanner_info(", ready)
    assert ready < identity


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


def test_status_route_refreshes_complete_identity_before_status_projection():
    uplink_rx = _source("esp32", "uplink", "main", "comms", "uart_rx.c")

    publisher = uplink_rx[
        uplink_rx.index("static uint32_t publish_scanner_identity_snapshot") :
        uplink_rx.index("static int64_t scanner_status_ssid_age_s")
    ]
    assert "bool require_complete" in publisher
    assert "if (require_complete && !snapshot.complete)" in publisher
    assert "return 0;" in publisher[
        publisher.index("if (require_complete && !snapshot.complete)") :
        publisher.index(
            "/* Publication is the freshness authority.",
            publisher.index("if (require_complete && !snapshot.complete)"),
        )
    ]

    status_handler = uplink_rx[
        uplink_rx.index("static void handle_status") :
        uplink_rx.index("static void process_line")
    ]
    assert re.search(
        r"static void handle_status\(\s*"
        r"const cJSON \*root,\s*int scanner_id,\s*"
        r"uint32_t published_identity_generation\s*\)",
        status_handler,
    )
    for field in (
        "firmware_name",
        "app_project",
        "hardware_type",
        "hardware_id",
    ):
        assert f'"{field}"' in status_handler
        assert f"info->{field}" in status_handler
    assert re.search(
        r"info->identity_generation\s*=\s*"
        r"published_identity_generation\s*;",
        status_handler,
    )

    status_route_start = uplink_rx.index(
        "} else if (decision.route == FOF_SCANNER_UPLINK_ROUTE_STATUS)"
    )
    status_route = uplink_rx[
        status_route_start :
        uplink_rx.index(
            "} else if (decision.route == "
            "FOF_SCANNER_UPLINK_ROUTE_SCANNER_INFO)",
            status_route_start,
        )
    ]
    publish = status_route.index("publish_scanner_identity_snapshot(")
    project = status_route.index("handle_status(", publish)
    assert publish < project
    assert re.search(
        r"publish_scanner_identity_snapshot\(\s*"
        r"scanner_id,\s*root,\s*esp_timer_get_time\(\) / 1000,\s*true\s*\)",
        status_route,
    )
    assert "uint32_t published_identity_generation" in status_route
    assert re.search(
        r"publish_scanner_identity_snapshot\(\s*"
        r"scanner_id,\s*root,\s*[^,]+,\s*true\s*\)",
        status_route,
    )

    scanner_info_start = uplink_rx.index(
        "} else if (decision.route == FOF_SCANNER_UPLINK_ROUTE_SCANNER_INFO)"
    )
    scanner_info_route = uplink_rx[
        scanner_info_start :
        uplink_rx.index(
            "decision.route == FOF_SCANNER_UPLINK_ROUTE_RECOVERY_ACK",
            scanner_info_start,
        )
    ]
    assert re.search(
        r"publish_scanner_identity_snapshot\(\s*"
        r"scanner_id,\s*root,\s*esp_timer_get_time\(\) / 1000,\s*false\s*\)",
        scanner_info_route,
    )
    assert re.search(
        r"handle_status\(\s*root,\s*scanner_id,\s*"
        r"published_identity_generation\s*\)",
        status_route,
    )


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
        uplink_rx.index(
            "} else if (decision.route == "
            "FOF_SCANNER_UPLINK_ROUTE_SCANNER_INFO)"
        ) :
        uplink_rx.index(
            "decision.route == FOF_SCANNER_UPLINK_ROUTE_RECOVERY_ACK"
        )
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
    assert "fw_store_note_relay_progress();" in health

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
    assert "cJSON_ParseWithOpts(json_line, &parse_end, true)" in uplink_rx


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


def test_badge_build_excludes_http_firmware_mutation_handlers_but_generic_keeps_them():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    status = _source("esp32", "uplink", "main", "network", "http_status.c")

    for source, handler in (
        (status, "ota_post_handler"),
        (status, "ota_relay_handler"),
        (store, "fw_upload_handler"),
        (store, "fw_relay_handler"),
        (store, "fw_trigger_handler"),
    ):
        definition = f"static esp_err_t {handler}(httpd_req_t *req)"
        assert f"#ifndef FOF_BADGE_VARIANT\n{definition}" in source
        assert source.count(definition) == 1

    for route, handler in (
        ("/api/ota", "ota_post_handler"),
        ("/api/ota/relay", "ota_relay_handler"),
    ):
        registration = status[
            status.index(f'.uri      = "{route}"') - 80 :
            status.index(f'.handler  = {handler}') + len(handler) + 20
        ]
        assert "#ifndef FOF_BADGE_VARIANT" in registration
        assert "HTTP_POST" in registration

    for route, handler in (
        ("/api/fw/upload", "fw_upload_handler"),
        ("/api/fw/relay", "fw_relay_handler"),
        ("/api/fw/trigger", "fw_trigger_handler"),
    ):
        assert route in store
        assert f".handler = {handler}" in store

    # USB scanner staging and the automatic UART relay coordinator remain
    # compiled for badges; only the HTTP adapters are excluded.
    for shared_api in (
        "bool fw_store_serial_upload_begin(",
        "bool fw_store_relay_staged_to_scanner(",
        "bool fw_store_handle_scanner_ready(",
        "void fw_store_get_auto_update_status(",
    ):
        assert shared_api in store


def test_badge_http_bootloader_is_rejected_before_any_restart_side_effect():
    status = _source("esp32", "uplink", "main", "network", "http_status.c")
    branch_start = status.index('} else if (strcmp(cmd, "bootloader") == 0) {')
    branch = status[branch_start : status.index("} else {", branch_start)]

    assert "#ifdef FOF_BADGE_VARIANT" in branch
    badge = branch[
        branch.index("#ifdef FOF_BADGE_VARIANT") : branch.index("#else")
    ]
    assert '{\\"ok\\":false,\\"error\\":' in badge
    assert '\\"firmware_mutation_requires_usb\\"}' in badge
    for forbidden in (
        "badge_runtime_arm_expected_reboot",
        "vTaskDelay",
        "REG_WRITE",
        "esp_restart",
    ):
        assert forbidden not in badge

    generic = branch[branch.index("#else") : branch.index("#endif")]
    assert '{\\"ok\\":true,\\"message\\":\\"bootloader\\"}' in generic
    assert "RTC_CNTL_FORCE_DOWNLOAD_BOOT" in generic
    assert "esp_restart();" in generic

    reboot = status[
        status.index('} else if (strcmp(cmd, "reboot") == 0) {') : branch_start
    ]
    badge_reboot = reboot[
        reboot.index("#ifdef FOF_BADGE_VARIANT") : reboot.index("#else")
    ]
    generic_reboot = reboot[reboot.index("#else") : reboot.index("#endif")]
    assert "if (!badge_usb_recovery_restart(" in badge_reboot
    assert '"http_reboot"' in badge_reboot
    assert "esp_restart();" not in badge_reboot
    assert "esp_restart();" in generic_reboot


def test_badge_html_omits_bootloader_control_while_generic_retains_it():
    status = _source("esp32", "uplink", "main", "network", "http_status.c")
    html = status[
        status.index("static esp_err_t badge_html_handler") :
        status.index("/* ── Public API", status.index(
            "static esp_err_t badge_html_handler"
        ))
    ]
    button = '<button onclick=\\"ctl(\'bootloader\')\\">Bootloader</button>'
    assert button in html
    button_index = html.index(button)
    guard = html.rfind("#ifndef FOF_BADGE_VARIANT", 0, button_index)
    assert guard >= 0
    assert html.index("#endif", button_index) > button_index
    assert "Firmware: USB/UART only" in html


def test_badge_fw_auto_check_init_is_a_noop_but_generic_starts_task():
    source = _source("esp32", "uplink", "main", "network", "fw_auto_check.c")
    init = source[source.index("void fw_auto_check_init(void)") :]

    assert "#ifdef FOF_BADGE_VARIANT" in init
    badge = init[init.index("#ifdef FOF_BADGE_VARIANT") : init.index("#else")]
    assert "return;" in badge
    assert "xTaskCreatePinnedToCore" not in badge
    generic = init[init.index("#else") : init.index("#endif", init.index("#else"))]
    assert "xTaskCreatePinnedToCore" in generic


def test_badge_excludes_relay_wait_helper_and_private_network_update_worker_graph():
    status = _source("esp32", "uplink", "main", "network", "http_status.c")
    auto = _source("esp32", "uplink", "main", "network", "fw_auto_check.c")

    wait_definition = (
        "static int wait_for_ota_response_since(int64_t start_ms,"
    )
    assert status.count(wait_definition) == 1
    wait_at = status.index(wait_definition)
    wait_guard = status.rfind("#ifndef FOF_BADGE_VARIANT", 0, wait_at)
    wait_end = status.index("#endif", wait_at)
    assert wait_guard >= 0
    assert wait_guard < wait_at < wait_end
    relay_definition = "static esp_err_t ota_relay_handler(httpd_req_t *req)"
    relay_at = status.index(relay_definition)
    relay_guard = status.rfind("#ifndef FOF_BADGE_VARIANT", 0, relay_at)
    relay_end = status.index("#endif", relay_at)
    assert relay_guard >= 0
    assert relay_guard < relay_at < relay_end

    getters = (
        "const char *fw_auto_check_status(void)",
        "int64_t fw_auto_check_last_age_s(void)",
        "const char *fw_auto_check_remote_uplink_version(void)",
        "const char *fw_auto_check_remote_scanner_version(void)",
    )
    worker_guard = auto.index(
        '#ifndef FOF_BADGE_VARIANT\nstatic const char *TAG = "fw_auto";'
    )
    worker_end = auto.index(
        "#endif\n\nvoid fw_auto_check_init(void)", worker_guard
    )
    worker = auto[worker_guard:worker_end]
    for getter in getters:
        assert getter in auto
        assert auto.index(getter) < worker_guard
        assert getter not in worker

    for worker_only in (
        "static int64_t      s_backoff_s = 0;",
        "static TaskHandle_t s_task = NULL;",
        "static scanner_info_t s_auto_check_scanner_snapshots[2] = {0};",
        "static esp_err_t http_collect_event(",
        "static esp_err_t fetch_metadata(",
        "static esp_err_t download_to_partition(",
        "static esp_err_t try_self_update_uplink(",
        "static bool connected_scanner_board(",
        "static esp_err_t try_refresh_scanner_cache(",
        "static void auto_check_task(void *arg)",
    ):
        assert worker_only in worker

    assert "badge_runtime.h" not in auto
    assert "badge_runtime_arm_expected_reboot" not in auto

    init = auto[auto.index("void fw_auto_check_init(void)") :]
    badge = init[init.index("#ifdef FOF_BADGE_VARIANT") : init.index("#else")]
    generic = init[init.index("#else") : init.index("#endif")]
    assert "return;" in badge
    assert "xTaskCreatePinnedToCore" not in badge
    assert "auto_check_task" in generic
    assert "xTaskCreatePinnedToCore" in generic

    for pure_helper in (
        "bool fw_auto_check_decide(",
        "bool fw_auto_check_version_differs(",
    ):
        assert pure_helper in auto
        runtime_guard = auto.index(
            "#ifndef FW_AUTO_CHECK_HOST_TEST",
            auto.index("bool fw_auto_check_version_differs("),
        )
        assert auto.index(pure_helper) < runtime_guard


def test_fof_status_exposes_one_bounded_uplink_ota_snapshot_in_all_render_paths():
    header = _source("esp32", "uplink", "main", "core", "uplink_usb_ota.h")
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")

    assert (
        "const char *uplink_usb_ota_state_name(uplink_usb_ota_state_t state);"
        in header
    )
    helper_start = serial.index("static void print_uplink_ota_status_json(")
    helper = serial[
        helper_start :
        serial.index(
            "#if defined(FOF_BADGE_VARIANT) && "
            "defined(FOF_DC34_GAME_CANARY)",
            helper_start,
        )
    ]
    expected_fields = (
        '\\"state\\"', '\\"partition\\"', '\\"received\\"',
        '\\"total\\"', '\\"target_version\\"', '\\"last_error\\"',
    )
    offsets = [helper.index(field) for field in expected_fields]
    assert offsets == sorted(offsets)
    assert helper.count("print_json_escaped_string(") == 4
    assert "uplink_usb_ota_state_name(status->state)" in helper
    assert "heap_" not in helper
    assert "vTaskDelay" not in helper

    minimal = serial[
        serial.index("static bool emit_minimal_status(") :
        serial.index("static void send_startup_recovery_status_response")
    ]
    assert "const uplink_usb_ota_status_t *uplink_ota" in minimal
    assert "print_uplink_ota_status_json(uplink_ota);" in minimal

    startup = serial[
        serial.index("static void send_startup_recovery_status_response") :
        serial.index("static void send_badge_status_response")
    ]
    assert startup.count("uplink_usb_ota_get_status(") == 1
    assert "&uplink_ota" in startup
    assert "&uplink_ota, status_now_ms" in startup

    normal = serial[
        serial.index("static void send_badge_status_response") :
        serial.index("static void send_control_ok")
    ]
    assert normal.count("uplink_usb_ota_get_status(") == 1
    assert normal.index("uplink_usb_ota_get_status(") < normal.index(
        "heap_caps_malloc("
    )
    assert normal.count("print_uplink_ota_status_json(&uplink_ota);") == 1
    assert normal.count("&uplink_ota, status_now_ms") == 2
    for identity in (
        '\\"version\\"', '\\"target\\"', '\\"firmware_name\\"',
        '\\"hardware_type\\"', '\\"hardware_id\\"',
    ):
        assert identity in serial


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
    manifest_header = _source(
        "esp32", "uplink", "main", "network", "fw_manifest_store.h"
    )
    migration = _source("esp32", "shared", "firmware_coordinator_migration.h")

    assert "NVS_CONFIG_MAX_BLOB_SIZE" in nvs_header
    assert "nvs_config_get_blob" in nvs_header
    assert "nvs_config_set_blob" in nvs_header
    assert "nvs_get_blob" in nvs_source
    assert "nvs_set_blob" in nvs_source
    assert "nvs_commit" in nvs_source

    assert 'FW_MANIFEST_KEY_COORDINATOR "fw_coord"' in manifest_header
    assert (
        "#define NVS_FW_COORDINATOR FW_MANIFEST_KEY_COORDINATOR" in store
    )
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
    save = store[
        store.index("static bool auto_coordinator_save_locked") :
        store.index("static void auto_coordinator_set_fail_closed_locked")
    ]
    assert "nvs_config_set_blob(" in save
    assert "NVS_FW_COORDINATOR" in save
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
    assert init_call < main.index("badge_usb_transport_set_dispatch_ready()")
    assert init_call < main.index("start_uart_rx_with_operation_gate()", init_call)

    lock = store[
        store.index("static bool auto_coordinator_lock") :
        store.index("static void auto_coordinator_unlock")
    ]
    assert "xSemaphoreCreateMutexStatic" not in lock


def test_campaign_snapshot_is_bounded_epoch_consistent_and_fail_busy():
    header = _source("esp32", "uplink", "main", "network", "fw_store.h")
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    for token in (
        "FW_CAMPAIGN_IDLE",
        "FW_CAMPAIGN_OPERATION_ACTIVE",
        "FW_CAMPAIGN_PENDING",
        "FW_CAMPAIGN_ALL_TERMINAL",
        "FW_CAMPAIGN_DEPENDENCY_DEFERRED",
        "FW_CAMPAIGN_UNKNOWN",
        "fw_store_campaign_state_sample",
        "fw_store_request_update_preemption",
        "fw_store_game_radio_must_yield",
    ):
        assert token in header

    sample = store[
        store.index("bool fw_store_campaign_state_sample") :
        store.index("fw_update_preempt_result_t", store.index(
            "bool fw_store_campaign_state_sample"
        ))
    ]
    assert "for (int attempt = 0; attempt < 3; ++attempt)" in sample
    first_operation = sample.index("operation_snapshot()")
    coordinator = sample.index("auto_coordinator_lock_ticks(0)", first_operation)
    second_operation = sample.index("operation_snapshot()", coordinator)
    assert first_operation < coordinator < second_operation
    assert "before.operation_epoch != after.operation_epoch" in sample
    assert "out->state = FW_CAMPAIGN_UNKNOWN" in sample
    assert "out->radio_inhibited = true" in sample
    assert "s_auto_coordinator_persistence_uncertain" in sample


def test_operation_epoch_and_radio_inhibit_are_sticky_through_release():
    header = _source("esp32", "shared", "firmware_operation_token.h")
    source = _source("esp32", "shared", "firmware_operation_token.c")

    assert "uint32_t operation_epoch;" in header
    assert "bool radio_inhibited;" in header
    begin = source[
        source.index("bool fw_operation_state_try_begin") :
        source.index("bool fw_operation_state_attach_uart_lease")
    ]
    assert "state->operation_epoch = next_epoch" in begin
    assert "state->radio_inhibited = true" in begin
    end = source[
        source.index("bool fw_operation_state_end") :
        source.index("bool fw_operation_state_try_reserve_recovery_restart")
    ]
    assert "state->operation_epoch = next_epoch" in end
    assert "radio_inhibited = false" not in end
    clear = source[source.index("bool fw_operation_state_clear_radio_inhibit") :]
    assert "state->active" in clear
    assert "!state->radio_inhibited" in clear
    assert "state->radio_inhibited = false" in clear
    assert "state->operation_epoch = next_epoch" in clear
    assert "state->preemption_requested" in clear


def test_update_preemption_atomically_blocks_every_late_operation_claim():
    header = _source("esp32", "shared", "firmware_operation_token.h")
    source = _source("esp32", "shared", "firmware_operation_token.c")
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    assert "bool preemption_requested;" in header
    assert "fw_operation_state_request_preemption" in header
    begin = source[
        source.index("bool fw_operation_state_try_begin") :
        source.index("bool fw_operation_state_attach_uart_lease")
    ]
    assert "state->preemption_requested" in begin

    request = store[
        store.index("fw_update_preempt_result_t fw_store_request_update_preemption") :
        store.index("bool fw_store_game_radio_must_yield")
    ]
    latch = request.index("fw_operation_state_request_preemption")
    operation_unlock = request.index(
        "portEXIT_CRITICAL(&s_operation_lock)", latch
    )
    coordinator = request.index("auto_coordinator_lock_ticks(0)", operation_unlock)
    assert latch < operation_unlock < coordinator
    restart_collision = request.index(
        "if (operation.recovery_restart_reserved)", operation_unlock
    )
    assert operation_unlock < restart_collision < coordinator
    assert request.index("s_auto_preempt_requested = true", coordinator) > coordinator
    assert "coordinator_suspended" in request
    assert "AUTO_COORDINATOR_QUIESCING" in request
    assert "AUTO_COORDINATOR_SUSPENDED" in request
    radio = request.index("badge_con_vhci_radio_quiesced")
    suspension_gate = request.index("if (!coordinator_suspended)")
    assert suspension_gate < radio


def test_coordinator_worker_is_static_permanent_and_started_after_uart_dependencies():
    header = _source("esp32", "uplink", "main", "network", "fw_store.h")
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    main = _source("esp32", "uplink", "main", "main.c")

    assert "fw_store_start_auto_update_coordinator" in header
    starter_begin = store.index("bool fw_store_start_auto_update_coordinator")
    starter_end = store.index(
        "static bool auto_coordinator_reprompt_requested(void)\n{",
        starter_begin,
    )
    starter = store[starter_begin:starter_end]
    assert "uart_rx_scanner_task_started(0)" in starter
    assert "uart_rx_scanner_task_started(1)" in starter
    assert "xTaskCreateStatic(" in starter
    assert "xTaskCreate(" not in starter

    task_begin = store.index("static void fw_auto_relay_task(void *arg)\n{")
    task = store[
        task_begin :
        store.index(
            "static bool auto_coordinator_start_worker(void)\n{", task_begin
        )
    ]
    assert "ulTaskNotifyTake(pdTRUE, portMAX_DELAY)" in task
    assert "vTaskDelete" not in task
    assert "for (;;)" in task
    kicker = store[
        store.index("static bool auto_coordinator_start_worker") :
        starter_begin
    ]
    assert "xTaskNotifyGive(worker)" in kicker
    assert "xTaskCreate" not in kicker

    normal_startup = main.index("/* ── 14. Start all tasks")
    lease = main.rindex("uart_rx_scanner_tx_lease_init()", 0, normal_startup)
    rx = main.index("start_uart_rx_with_operation_gate()", normal_startup)
    start = main.index("fw_store_start_auto_update_coordinator()", rx)
    restore = main.index("fw_store_restore_auto_update_coordinator()", start)
    display = main.index("xTaskNotifyGive(s_display_task_handle)", restore)
    assert lease < rx < start < restore < display

    coordinator_branch = main[
        main.index("/* The permanent coordinator", normal_startup) :
        main.index("/* ── 15. Start HTTP status server", start)
    ]
    assert "if (!badge_safe_usb)" in coordinator_branch
    assert "safe USB mode" in coordinator_branch
    assert "coordinator creation deferred" in coordinator_branch


def test_campaign_radio_inhibit_publishes_only_after_durable_terminal_save():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    save = store[
        store.index("static bool auto_coordinator_save_locked") :
        store.index("static void auto_coordinator_set_fail_closed_locked")
    ]
    nvs = save.index("nvs_config_set_blob(")
    uncertainty = save.index(
        "s_auto_coordinator_persistence_uncertain = !saved", nvs
    )
    reconcile = save.index(
        "auto_coordinator_reconcile_radio_inhibit_locked()", uncertainty
    )
    assert nvs < uncertainty < reconcile
    assert "fw_operation_state_request_radio_inhibit" in save[uncertainty:]

    begin_start = store.index(
        "static bool auto_coordinator_begin_generation("
        "uint32_t generation,\n"
        "                                              uint8_t target_slot_mask,\n"
        "                                              uint32_t manifest_crc32,\n"
        "                                              const uart_rx_pause_guard_t\n"
        "                                                  scanner_guards[2])\n{"
    )
    begin = store[
        begin_start :
        store.index(
            "static bool auto_coordinator_initialize_fail_closed(\n",
            begin_start,
        )
    ]
    assert begin.index("auto_coordinator_latch_radio_inhibit_locked()") < (
        begin.index("FW_AUTO_SLOT_AWAITING_CHECK")
    )
    reopen = store[
        store.index("static bool auto_reopen_terminal_for_newer_check") :
        store.index(
            "static void auto_reset_ready_queue_after_revalidation_failure"
        )
    ]
    assert reopen.index("auto_coordinator_latch_radio_inhibit_locked()") < (
        reopen.index("FW_AUTO_SLOT_AWAITING_CHECK")
    )


def test_preemption_returns_reserved_attempt_before_any_relay_byte():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    task_begin = store.index("static void fw_auto_relay_task(void *arg)\n{")
    task = store[
        task_begin :
        store.index(
            "static bool auto_coordinator_start_worker(void)\n{", task_begin
        )
    ]
    reservation = task.index("relay_attempts[scanner_id]++")
    preempt = task.index("bool preempt_before_bytes", reservation)
    relay = task.index("fw_relay_stored_to_scanner", preempt)
    assert reservation < preempt < relay
    safe_point = task[preempt:relay]
    assert "relay_attempts[scanner_id]--" in safe_point
    assert "FW_AUTO_SLOT_READY_QUEUED" in safe_point
    assert "pending_mask |= bit" in safe_point
    assert "auto_coordinator_save_locked()" in safe_point
    assert "AUTO_COORDINATOR_SUSPENDED" in safe_point


def test_manifest_invalidation_is_the_durable_idle_journal_for_abort():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    invalidate = store[
        store.index("static bool invalidate_fw_metadata") :
        store.index("static esp_err_t read_manifest_string")
    ]
    size_commit = invalidate.index("nvs_config_set_u32(NVS_FW_SIZE, 0)")
    idle = invalidate.index("auto_coordinator_note_manifest_invalidated()")
    assert size_commit < idle
    note = store[
        store.index("static void auto_coordinator_note_manifest_invalidated(void)\n{") :
        store.index("bool fw_store_campaign_state_sample")
    ]
    assert "s_auto_coordinator_loaded = false" in note
    assert "s_auto_coordinator_persistence_uncertain = false" in note
    assert "auto_coordinator_reconcile_radio_inhibit_locked()" in note


def test_coordinator_restore_distinguishes_no_manifest_from_storage_error():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    restore = store[
        store.index("bool fw_store_restore_auto_update_coordinator") :
        store.index("void fw_store_handle_scanner_check")
    ]

    read = restore.index("fw_store_read_committed(&info)")
    no_manifest = restore.index("FW_STORE_READ_NO_MANIFEST", read)
    read_error = restore.index("FW_STORE_READ_ERROR", no_manifest)
    idle = restore.index("auto_coordinator_note_manifest_invalidated()", no_manifest)
    uncertainty = restore.index(
        "s_auto_coordinator_persistence_uncertain = true", read_error
    )
    assert read < no_manifest < idle < read_error < uncertainty
    identity_copy = restore.index(
        "/* Restore establishes a fresh parser epoch too.", uncertainty
    )
    assert "return false;" in restore[read_error:identity_copy]


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
    manifest_commit = upload_end.index("persist_validated_metadata(&info, true)")
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
    abort = coordinator_failure.index("memset(&s_serial_upload")
    release = coordinator_failure.index(
        "auto_coordinator_release_excluded_slots()"
    )
    assert abort < release

    coordinator = store[store.index("#define FW_AUTO_COORDINATOR_MAGIC") :]
    begin = coordinator[
        coordinator.index("static bool auto_coordinator_begin_generation") :
        coordinator.index("static bool auto_coordinator_initialize_fail_closed")
    ]
    assert "auto_coordinator_set_fail_closed_locked" in begin
    assert "s_auto_coordinator = before" not in begin


def test_auto_relay_reads_only_its_reserved_manifest_generation():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    prepare = _source(
        "esp32", "uplink", "main", "network",
        "fw_relay_prepare_adapter.c",
    )
    prepare_flow = prepare[
        prepare.index("fw_relay_prepare_result_t fw_relay_prepare_for_scanner") :
        prepare.index("bool fw_relay_prepared_release")
    ]
    relay = store[
        store.index("static bool fw_relay_stored_to_scanner") :
        store.index("static esp_err_t fw_relay_handler")
    ]
    assert "uint32_t expected_generation" in relay
    assert "fw_relay_prepare_for_scanner(" in relay
    token = prepare_flow.index("hooks->token_acquire")
    manifest = prepare_flow.index("hooks->read_committed")
    generation_guard = prepare_flow.index(
        "out->generation != expected_generation"
    )
    partition = prepare_flow.index("hooks->partition_for_snapshot")
    assert token < manifest < generation_guard < partition
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

    busy_check = task.index("if (fw_store_operation_is_active())")
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

    first_lock_start = task.index(
        "if (!auto_coordinator_lock())", task.index("int scanner_id")
    )
    first_lock = task[
        first_lock_start : task.index(
            "if (!s_auto_coordinator_loaded)", first_lock_start
        )
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

    badge_start = main.index("uart_startup_gate_result_t uart_startup")
    ready_call = main.index('uart_rx_send_command("{\\\"type\\\":\\\"ready\\\"}")',
                            badge_start)
    restore_call = main.index("fw_store_restore_auto_update_coordinator()",
                              ready_call)
    assert badge_start < ready_call < restore_call
    restore_guard = main[restore_call - 40:restore_call]
    assert "if (!badge_safe_usb &&" in restore_guard


def test_committed_manifest_carries_scope_so_boot_can_rebuild_missing_coordinator():
    header = _source("esp32", "uplink", "main", "network", "fw_store.h")
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    manifest_header = _source(
        "esp32", "uplink", "main", "network", "fw_manifest_store.h"
    )
    assert "target_slot_mask" in header
    assert 'FW_MANIFEST_KEY_SLOT_MASK "fw_slotmask"' in manifest_header
    assert "#define NVS_FW_SLOT_MASK FW_MANIFEST_KEY_SLOT_MASK" in store

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
        upload_end.index("if (!persist_validated_metadata(&info, true))") :
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


def test_newer_scanner_is_terminal_and_opens_the_ordered_wifi_gate():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    coordinator = store[store.index("FW_AUTO_COORDINATOR_MAGIC") :]
    assert "FW_AUTO_SLOT_NEWER_SKIPPED" in coordinator

    terminal = coordinator[
        coordinator.index("static bool auto_coordinator_slot_is_terminal") :
        coordinator.index("static const char *auto_coordinator_state_name")
    ]
    assert "fof_auto_slot_is_terminal" in terminal
    auto_policy = _source("esp32", "shared", "firmware_auto_policy.c")
    shared_terminal = auto_policy[
        auto_policy.index("bool fof_auto_slot_is_terminal") :
        auto_policy.index("bool fof_auto_wifi_gate_open")
    ]
    assert "FOF_AUTO_SLOT_NEWER_SKIPPED" in shared_terminal
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
    ingress_registry = _source(
        "esp32", "shared", "scanner_uplink_ingress_registry.c"
    )
    firmware_registry = _source(
        "esp32", "shared", "firmware_json_schema_registry.c"
    )

    assert 'json_get_string(root, "reason", "")' in uplink_rx
    assert "fw_store_handle_scanner_check(scanner_id, board, ver, reason)" in uplink_rx
    assert "fof_fw_json_select_and_validate(" in ingress_registry
    assert "FOF_FW_JSON_INGRESS_SCANNER_TO_UPLINK_UART" in ingress_registry
    for key in (
        '"fw_name"',
        '"app_project"',
        '"hardware_type"',
        '"sha256"',
        '"generation"',
        '"allow_same_version"',
    ):
        assert key in firmware_registry
    assert "FOF_FW_JSON_SCHEMA_RECEIPT_FW_READY_STRICT" in firmware_registry
    assert "FOF_FW_JSON_SCHEMA_RECEIPT_FW_READY_LEGACY_68" in firmware_registry

    ready_branch = uplink_rx[
        uplink_rx.index(
            "decision.firmware_schema_id ==\n"
            "             FOF_FW_JSON_SCHEMA_RECEIPT_FW_READY_STRICT"
        ) :
        uplink_rx.index(
            "decision.route == FOF_SCANNER_UPLINK_ROUTE_CAL_MODE_ACK"
        )
    ]
    assert (
        "decision.firmware_schema_id ==\n"
        "            FOF_FW_JSON_SCHEMA_RECEIPT_FW_READY_LEGACY_68"
        in ready_branch
    )
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
        uplink_rx.index(
            "decision.firmware_schema_id ==\n"
            "             FOF_FW_JSON_SCHEMA_RECEIPT_FW_READY_STRICT"
        ) :
        uplink_rx.index(
            "decision.route == FOF_SCANNER_UPLINK_ROUTE_CAL_MODE_ACK"
        )
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

    assert "FW_AUTO_SECOND_IDENTITY_WAIT_MS 20000" in coordinator
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


def test_worker_readiness_probe_waits_for_fresh_post_floor_identity():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    task = store[
        store.index("static void fw_auto_relay_task(void *arg)\n{") :
        store.index("static bool auto_coordinator_start_worker(void)\n{")
    ]

    decision = task.index("fof_auto_readiness_probe_decide(")
    freshness = task.rfind("fof_auto_identity_is_fresh(", 0, decision)
    assert freshness >= 0
    assert re.search(
        r"s_auto_identity_generation_floor\[(?:slot|scanner_id)\]",
        task[freshness:decision],
    )

    wait = task.index("FOF_AUTO_PROBE_WAIT", decision)
    exhausted = task.index("FOF_AUTO_PROBE_EXHAUSTED", wait)
    reserve = task.index(
        "s_auto_coordinator.readiness_probe_attempts[slot]++",
        exhausted,
    )
    assert decision < wait < exhausted < reserve
    stale_path = task[wait:exhausted]
    assert re.search(r"\b(?:continue|break)\s*;", stale_path)
    assert "readiness_probe_attempts" not in stale_path
    assert "FW_AUTO_SLOT_FAILED" not in stale_path


def test_identity_acquisition_deadline_is_gate_relative_and_restore_bounded():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    coordinator = store[store.index("#define FW_AUTO_COORDINATOR_MAGIC") :]

    assert "FW_AUTO_IDENTITY_ACQUISITION_TIMEOUT_MS" in coordinator
    assert "s_auto_identity_acquisition_deadline_ms" in coordinator
    assert "FW_AUTO_IDENTITY_WAIT_ACTIVE" in coordinator
    restore = coordinator[
        coordinator.index("bool fw_store_restore_auto_update_coordinator") :
        coordinator.index("void fw_store_handle_scanner_check")
    ]
    floors_start = coordinator.index(
        "static void auto_set_identity_floors_locked"
    )
    floors = coordinator[
        floors_start : coordinator.index("\n}\n", floors_start) + 3
    ]
    assert "s_auto_identity_acquisition_deadline_ms[scanner_id] = 0" in floors
    assert "s_auto_coordinator.reserved[scanner_id]" not in floors
    assert "auto_set_identity_floors_locked(identity_floors)" in restore

    task = coordinator[
        coordinator.index("static void fw_auto_relay_task(void *arg)\n{") :
        coordinator.index("static bool auto_coordinator_start_worker(void)\n{")
    ]
    start = task.index("auto_identity_acquisition_start_locked(")
    gate = task.rfind(
        "auto_coordinator_slot_gate_open_locked(slot)", 0, start
    )
    assert gate < start
    start_path = task[gate:start + 300]
    assert "readiness_probe_attempts" not in start_path


def test_identity_acquisition_start_prompts_once_without_spending_readiness_budget():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    reprompt = store[
        store.index(
            "static bool auto_coordinator_reprompt_requested(void)\n{"
        ) :
        store.index(
            "static void auto_coordinator_release_excluded_slots(void)\n{"
        )
    ]

    reprompt_start = reprompt.index(
        "auto_identity_acquisition_start_locked("
    )
    reprompt_initial_prompt = reprompt[
        reprompt_start :
        reprompt.index("continue;", reprompt_start) + len("continue;")
    ]
    assert "identity_prompt_mask |= bit" in reprompt_initial_prompt
    assert "readiness_probe_attempts" not in reprompt_initial_prompt

    kick = store[
        store.index("static void auto_coordinator_kick_committed_generation") :
        store.index("bool fw_store_persist_metadata(")
    ]
    assert kick.index("auto_coordinator_reprompt_requested()") < (
        kick.index("auto_coordinator_start_worker()")
    )

    complete = store[
        store.index("bool fw_store_serial_upload_complete_terminal") :
        store.index("bool fw_store_get_info")
    ]
    normal = complete[complete.index("if (defer_scanner_activation)") :]
    assert normal.index("auto_coordinator_reprompt_requested()") < (
        normal.index("auto_coordinator_start_worker()")
    )


def test_explicit_identity_reprompt_is_checked_after_durable_active_transition():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    reprompt = store[
        store.index(
            "static bool auto_coordinator_reprompt_requested(void)\n{"
        ) :
        store.index(
            "static void auto_coordinator_release_excluded_slots(void)\n{"
        )
    ]

    start = reprompt.index("auto_identity_acquisition_start_locked(")
    identity_mask = reprompt.index("identity_prompt_mask |= bit", start)
    save = reprompt.index("auto_coordinator_save_locked()", identity_mask)
    unlock = reprompt.index("auto_coordinator_unlock()", save)
    deliver = reprompt.index(
        "auto_deliver_identity_prompts(", unlock
    )
    assert start < identity_mask < save < unlock < deliver
    assert re.search(
        r"bool identity_delivered\s*=\s*"
        r"auto_deliver_identity_prompts\(\s*"
        r"prompt_generation,\s*identity_prompt_mask\s*\);",
        reprompt[deliver - 40 :],
    )
    readiness_send = reprompt.index(
        "fw_store_request_scanner_checks(readiness_prompt_mask)", deliver
    )
    delivery_gate = reprompt[deliver:readiness_send]
    assert "if (!identity_delivered)" in delivery_gate
    assert "return false;" in delivery_gate
    assert "uart_rx_send_command_to_scanner(" not in reprompt


def test_worker_identity_fallback_is_checked_after_durable_active_transition():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    task = store[
        store.index("static void fw_auto_relay_task(void *arg)\n{") :
        store.index("static bool auto_coordinator_start_worker(void)\n{")
    ]

    start = task.index("auto_identity_acquisition_start_locked(")
    save = task.index("auto_coordinator_save_locked()", start)
    identity_mask = task.index("identity_prompt_mask |= bit", save)
    unlock = task.index("auto_coordinator_unlock()", identity_mask)
    deliver = task.index("auto_deliver_identity_prompts(", unlock)
    assert start < save < identity_mask < unlock < deliver
    readiness_wait = task.index("if (readiness_waiting)", deliver)
    delivery_control = task[deliver:readiness_wait]
    assert re.search(
        r"bool identity_delivered\s*=\s*"
        r"identity_prompt_mask\s*==\s*0\s*\|\|\s*"
        r"auto_deliver_identity_prompts\(\s*"
        r"identity_prompt_generation,\s*identity_prompt_mask\s*\);",
        task[deliver - 80 :readiness_wait],
    )
    assert "if (!identity_delivered)" in delivery_control
    failure_control = delivery_control[
        delivery_control.index("if (!identity_delivered)") :
    ]
    assert "if (!auto_coordinator_lock())" in failure_control
    assert "worker_continues = s_auto_relay_worker_running" in failure_control
    assert "auto_coordinator_unlock()" in failure_control
    terminal = failure_control.index("if (!worker_continues)")
    assert "break;" in failure_control[
        terminal : failure_control.index("continue;", terminal)
    ]
    assert "ulTaskNotifyTake" in failure_control[terminal:]
    assert "continue;" in failure_control[terminal:]
    assert "(void)auto_deliver_identity_prompts(" not in delivery_control
    acquisition = task[
        start : task.index("break;", identity_mask) + len("break;")
    ]
    assert "readiness_probe_attempts[slot]++" not in acquisition
    assert "relay_attempts[slot]++" not in acquisition


def test_failed_identity_prompt_rolls_back_only_affected_active_lanes():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    helper = store[
        store.index("static bool auto_deliver_identity_prompts(") :
        store.index("static void fw_auto_relay_task(void *arg)\n{")
    ]

    assert (
        "uint8_t sent_mask = "
        "fw_store_request_scanner_checks(prompt_mask);"
    ) in helper
    assert (
        "uint8_t failed_mask = prompt_mask & (uint8_t)~sent_mask;"
    ) in helper
    send = helper.index("fw_store_request_scanner_checks(prompt_mask)")
    failed = helper.index("uint8_t failed_mask", send)
    lock = helper.index("auto_coordinator_lock()", failed)
    rollback = helper.index(
        "auto_identity_acquisition_clear_locked(scanner_id)", lock
    )
    save = helper.index("auto_coordinator_save_locked()", rollback)
    assert send < failed < lock < rollback < save
    assert "s_auto_coordinator.generation == prompt_generation" in helper
    assert "!auto_coordinator_slot_is_terminal(state)" in helper
    assert "FW_AUTO_IDENTITY_WAIT_ACTIVE" in helper
    assert "failed_mask & bit" in helper
    lane_guard = helper[
        helper.index("if ((failed_mask & bit)") :
        helper.index("changed = true;") + len("changed = true;")
    ]
    for condition in (
        "s_auto_coordinator_loaded",
        "s_auto_coordinator.generation == prompt_generation",
        "!auto_coordinator_slot_is_terminal(state)",
        "FW_AUTO_IDENTITY_WAIT_ACTIVE",
        "auto_identity_acquisition_clear_locked(scanner_id)",
    ):
        assert condition in lane_guard
    assert re.search(
        r"if \(\(failed_mask & bit\) &&\s*"
        r"s_auto_coordinator_loaded &&\s*"
        r"s_auto_coordinator\.generation == prompt_generation &&\s*"
        r"!auto_coordinator_slot_is_terminal\(state\) &&\s*"
        r"s_auto_coordinator\.reserved\[scanner_id\] ==\s*"
        r"FW_AUTO_IDENTITY_WAIT_ACTIVE\) {\s*"
        r"auto_identity_acquisition_clear_locked\(scanner_id\);\s*"
        r"changed = true;",
        lane_guard,
    )
    assert helper.count(
        "auto_identity_acquisition_clear_locked(scanner_id)"
    ) == 1
    assert "sent_mask & bit" not in helper
    full_success = helper[
        helper.index("if (failed_mask == 0)") :
        helper.index("while (!auto_coordinator_lock())")
    ]
    assert "return true;" in full_success
    rollback_result = helper[
        helper.index("bool rolled_back =") :
        helper.index("auto_coordinator_unlock()")
    ]
    assert "bool rolled_back = !changed || auto_coordinator_save_locked()" in (
        rollback_result
    )
    assert re.search(
        r"if \(!rolled_back\) {\s*"
        r"auto_coordinator_fail_closed_after_save_failure_locked\(\s*"
        r'"identity_prompt_rollback"\);\s*}',
        rollback_result,
    )
    fail_closed = helper.index(
        "auto_coordinator_fail_closed_after_save_failure_locked("
    )
    unlock = helper.index("auto_coordinator_unlock()", fail_closed)
    assert helper.index("return false;", unlock) > unlock
    assert "readiness_probe_attempts" not in helper
    assert "relay_attempts" not in helper


def test_identity_acquisition_timeout_fails_without_spending_probe_and_releases_wifi():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    task = store[
        store.index("static void fw_auto_relay_task(void *arg)\n{") :
        store.index("static bool auto_coordinator_start_worker(void)\n{")
    ]

    acquisition = task.index("fof_auto_identity_acquisition_decide(")
    acquisition_expired = task.index(
        "FOF_AUTO_PROBE_EXHAUSTED", acquisition
    )
    reserve = task.index(
        "s_auto_coordinator.readiness_probe_attempts[slot]++",
        acquisition_expired,
    )
    expiry_path = task[acquisition_expired:reserve]
    assert "FW_AUTO_SLOT_FAILED" in expiry_path
    assert "auto_coordinator_save_locked()" in expiry_path
    assert "pending_mask" in expiry_path
    assert "bound_hardware_id" in expiry_path
    assert "auto_identity_acquisition_exhaust_locked(slot)" in expiry_path
    assert "readiness_probe_attempts[slot]++" not in expiry_path
    assert "probe_scanner_id = -2" in expiry_path

    release = task.index("if (probe_scanner_id == -2)", reserve)
    reprompt = task.index("auto_coordinator_reprompt_requested()", release)
    assert acquisition_expired < reserve < release < reprompt


def test_identity_exhaustion_is_durable_and_cannot_be_rearmed_by_checks():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    assert "#define FW_AUTO_IDENTITY_WAIT_EXHAUSTED 2U" in store

    validator = store[
        store.index("static bool auto_coordinator_blob_valid(") :
        store.index("static bool auto_coordinator_save_locked(")
    ]
    assert "FW_AUTO_IDENTITY_WAIT_EXHAUSTED" in validator
    assert "state != FW_AUTO_SLOT_FAILED" in validator

    reopen = store[
        store.index("static bool auto_reopen_terminal_for_newer_check(") :
        store.index(
            "static void auto_reset_ready_queue_after_revalidation_failure("
        )
    ]
    assert "FW_AUTO_IDENTITY_WAIT_EXHAUSTED" in reopen
    assert "fof_auto_terminal_reopen_allowed(" in reopen

    reprompt = store[
        store.index(
            "static bool auto_coordinator_reprompt_requested(void)\n{"
        ) :
        store.index(
            "static void auto_coordinator_release_excluded_slots(void)\n{"
        )
    ]
    exhausted = reprompt.index("FOF_AUTO_PROBE_EXHAUSTED")
    save = reprompt.index("auto_coordinator_save_locked()", exhausted)
    assert (
        "auto_identity_acquisition_exhaust_locked(scanner_id)"
        in reprompt[exhausted:save]
    )


def test_nonserial_staging_kicks_coordinator_after_uart_inputs_resume():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    helper = "auto_coordinator_kick_committed_generation()"

    public_persist = store[
        store.index("bool fw_store_persist_metadata(") :
        store.index("bool fw_store_serial_upload_active(")
    ]
    assert helper in public_persist

    http = store[
        store.index("static esp_err_t fw_upload_handler(") :
        store.index("/* ── Line-based UART read helpers")
    ]
    committed = http.index("fw_store_persist_metadata_with_pause_guards(")
    resume = http.index(
        "resume_guarded_scanner_inputs(upload_pause_guards)", committed
    )
    operation_end = http.index(
        "fw_store_operation_end(operation_token)", resume
    )
    kick = http.index(helper, operation_end)
    assert committed < resume < operation_end < kick


def test_reprompt_waits_for_fresh_post_floor_identity_without_probe_budget():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    reprompt = store[
        store.index(
            "static bool auto_coordinator_reprompt_requested(void)\n{"
        ) :
        store.index(
            "static void auto_coordinator_release_excluded_slots(void)\n{"
        )
    ]

    decision = reprompt.index("fof_auto_readiness_probe_decide(")
    freshness = reprompt.rfind("fof_auto_identity_is_fresh(", 0, decision)
    assert freshness >= 0
    assert re.search(
        r"s_auto_identity_generation_floor\[(?:scanner_id|slot)\]",
        reprompt[freshness:decision],
    )

    wait = reprompt.index("FOF_AUTO_PROBE_WAIT", decision)
    exhausted = reprompt.index("FOF_AUTO_PROBE_EXHAUSTED", wait)
    reserve = reprompt.index(
        "s_auto_coordinator.readiness_probe_attempts[scanner_id]++",
        exhausted,
    )
    assert decision < wait < exhausted < reserve
    stale_path = reprompt[wait:exhausted]
    assert re.search(r"\b(?:continue|break)\s*;", stale_path)
    assert "readiness_probe_attempts" not in stale_path
    assert "FW_AUTO_SLOT_FAILED" not in stale_path


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

    readiness_decision = task.index("fof_auto_readiness_probe_decide(")
    exhausted_start = task.index(
        "if (decision == FOF_AUTO_PROBE_EXHAUSTED)",
        readiness_decision,
    )
    exhausted_probe = task[
        exhausted_start :
        task.index("fw_auto_coordinator_blob_t before", exhausted_start)
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

    assert "auto_coordinator_release_excluded_slots()" in task
    assert "vTaskDelete(NULL)" not in task
    assert "ulTaskNotifyTake(pdTRUE, portMAX_DELAY)" in task
    assert task.index("auto_coordinator_release_excluded_slots()") < task.rindex(
        "AUTO_COORDINATOR_IDLE"
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
    assert "ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250))" in result_phase
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


def test_persistent_usb_start_failure_holds_minimal_recovery_surface():
    main = _source("esp32", "uplink", "main", "main.c")

    failure = main[
        main.index("if (!usb_transport_started)") :
        main.index("log_detection_queue_heap(\"after_display\")")
    ]
    assert "badge_runtime_usb_recovery_once_consumed()" in failure
    assert "if (!badge_usb_recovery_restart(" in failure
    assert "BADGE_USB_RESET_APP" in failure
    assert '"usb_safe_once"' in failure
    consumed = failure.index("badge_runtime_usb_recovery_once_consumed()")
    restart = failure.index("badge_usb_recovery_restart(", consumed)
    recovery_only = failure.index("badge_startup_recovery_only = true", restart)
    exact_reason = failure.index(
        'badge_startup_safe_reason = "usb_transport_init"', restart
    )
    force_safe = failure.index(
        'badge_runtime_force_safe_mode(true, "usb_transport_init")', restart
    )
    assert consumed < restart < recovery_only < exact_reason < force_safe

    ready = main.index("badge_usb_transport_set_dispatch_ready()")
    hold = main[
        main.index("if (badge_startup_recovery_only)", ready) :
        main.index("esp_event_loop_create_default()")
    ]
    assert "return;" in hold


def test_badge_button_and_display_workers_use_static_recovery_safe_storage():
    main = _source("esp32", "uplink", "main", "main.c")
    display = _source(
        "esp32", "uplink", "main", "hw", "display_st7735.c"
    )
    display_header = _source(
        "esp32", "uplink", "main", "hw", "oled_display.h"
    )

    assert "bool oled_badge_buttons_start(void);" in display_header
    button_start = display[
        display.index("static bool badge_buttons_start(void)") :
        display.index("/* ── 5x7 ASCII font")
    ]
    assert "static StaticTask_t s_button_task_tcb" in display
    assert "static StackType_t s_button_task_stack" in display
    assert "__attribute__((aligned(16)))" in display
    assert "_Static_assert(sizeof(StackType_t) == 1" in display
    assert "xTaskCreateStatic(" in button_start
    assert "xTaskCreate(" not in button_start
    assert "return true;" in button_start
    assert "return false;" in button_start
    assert "bool oled_badge_buttons_start(void)" in button_start

    assert "static StaticTask_t s_display_task_tcb" in main
    assert "static StackType_t s_display_task_stack" in main
    assert "__attribute__((aligned(16)))" in main
    assert "_Static_assert(sizeof(StackType_t) == 1" in main
    task_body = main[
        main.index("static void display_task") : main.index("void app_main")
    ]
    assert "ulTaskNotifyTake(pdTRUE, portMAX_DELAY)" in task_body
    app = main[main.index("void app_main(void)") :]
    create = app.index("xTaskCreateStatic(")
    assert create < app.index("badge_usb_transport_set_dispatch_ready()")
    assert create < app.index("esp_event_loop_create_default()")
    assert create < app.index("uart_rx_scanner_tx_lease_init()")
    assert create < app.index("uart_rx_init(detection_queue)")
    assert "xTaskCreate(\n        display_task" not in app
    assert 'badge_startup_safe_reason = "display_task"' in app
    assert 'badge_startup_safe_reason = "button_task"' in app
    assert "badge_runtime_force_safe_mode(true, badge_startup_safe_reason)" \
        in app
    assert "if (!oled_badge_buttons_start())" in app
    assert "xTaskNotifyGive(s_display_task_handle)" in app
    normal_startup = app.index("/* ── 14. Start all tasks")
    required_gate = app.index("if (!required_tasks_started)", normal_startup)
    notify = app.index("xTaskNotifyGive(s_display_task_handle)", required_gate)
    assert required_gate < notify


def test_firmware_restart_reservation_closes_watchdog_operation_race():
    main = _source("esp32", "uplink", "main", "main.c")
    recovery = _source(
        "esp32", "uplink", "main", "core", "badge_usb_recovery.c"
    )
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    store_header = _source(
        "esp32", "uplink", "main", "network", "fw_store.h"
    )

    assert "bool fw_store_try_reserve_recovery_restart(void);" in store_header
    token_header = _source("esp32", "shared", "firmware_operation_token.h")
    assert "bool recovery_restart_reserved;" in token_header
    reserve = store[
        store.index("bool fw_store_try_reserve_recovery_restart(void)") :
        store.index("fw_store_activity_t fw_store_activity_sample(void)")
    ]
    assert "portENTER_CRITICAL(&s_operation_lock)" in reserve
    assert "fw_operation_state_try_reserve_recovery_restart" in reserve
    assert "portEXIT_CRITICAL(&s_operation_lock)" in reserve

    begin = store[
        store.index("bool fw_store_operation_try_begin(") :
        store.index("bool fw_store_operation_end(")
    ]
    assert "fw_operation_state_try_begin" in begin

    watchdog = main[main.index("/* ── 17. Connectivity watchdog") :]
    restart = watchdog[
        watchdog.index("if (usb_action == BADGE_USB_HEALTH_RESTART_SAFE_USB)") :
        watchdog.index("badge_runtime_note_main_stack_free")
    ]
    assert "badge_usb_recovery_prepare_firmware_restart(" in restart
    assert "BADGE_USB_FIRMWARE_RESTART_PREPARE_OWNED" in restart
    assert "badge_usb_recovery_restart_with_owned_lease(" in restart
    assert "fw_store_try_reserve_recovery_restart()" not in restart
    assert "fw_store_activity_sample()" not in restart
    assert recovery.count("fw_store_try_reserve_recovery_restart()") == 1

    low_heap = watchdog[
        watchdog.index("if (!badge_backend_enabled)") :
        watchdog.index("continue;", watchdog.index("if (!badge_backend_enabled)"))
    ]
    assert "badge_automatic_restart_when_firmware_idle(reason)" in low_heap


def test_badge_automatic_restart_helper_waits_for_firmware_ownership():
    main = _source("esp32", "uplink", "main", "main.c")

    helper_start = main.index(
        "static bool badge_automatic_restart_when_firmware_idle"
    )
    helper_end = main.index("static void display_task", helper_start)
    helper = main[helper_start:helper_end]
    badge_guard = main.rfind("#ifdef FOF_BADGE_VARIANT", 0, helper_start)
    assert badge_guard > main.rfind("#endif", 0, helper_start)
    prepare = helper.index(
        "badge_usb_recovery_prepare_firmware_restart("
    )
    owned = helper.index(
        "BADGE_USB_FIRMWARE_RESTART_PREPARE_OWNED", prepare
    )
    busy = helper.index(
        "BADGE_USB_FIRMWARE_RESTART_PREPARE_BUSY", owned
    )
    retry = helper.index("vTaskDelay(pdMS_TO_TICKS(25))", busy)
    execute = helper.index(
        "rollback_and_reboot_with_owned_lease(", retry
    )
    assert prepare < owned < busy < retry < execute
    assert "fw_store_try_reserve_recovery_restart()" not in helper
    assert "vTaskDelay(portMAX_DELAY)" not in helper
    assert "return false;" in helper


def _assert_automatic_restart_has_exact_owned_handoff(main: str) -> None:
    helper_start = main.index(
        "static bool badge_automatic_restart_when_firmware_idle"
    )
    helper = main[
        helper_start:main.index("static void display_task", helper_start)
    ]
    exact_call = (
        "rollback_and_reboot_with_owned_lease(reason, &reboot_lease);"
    )
    assert helper.count(exact_call) == 1
    compact = re.sub(r"\s+", " ", helper).strip()
    assert re.search(
        r"} rollback_and_reboot_with_owned_lease"
        r"\(reason, &reboot_lease\); } #endif$",
        compact,
    )

    executor_start = main.index(
        "static _Noreturn void rollback_and_reboot_with_owned_lease("
    )
    executor = main[
        executor_start :
        main.index(
            "static bool badge_automatic_restart_when_firmware_idle",
            executor_start,
        )
    ]
    executor_compact = re.sub(r"\s+", " ", executor)
    assert executor_compact.count(
        "badge_runtime_expected_reboot_lease_is_owned(reboot_lease)"
    ) == 2


def test_badge_automatic_restart_handoff_contract_rejects_null_or_skippable_executor():
    main = _source("esp32", "uplink", "main", "main.c")
    _assert_automatic_restart_has_exact_owned_handoff(main)

    exact_call = (
        "rollback_and_reboot_with_owned_lease(reason, &reboot_lease);"
    )
    null_handoff = main.replace(
        exact_call,
        "rollback_and_reboot_with_owned_lease(reason, NULL);",
        1,
    )
    with pytest.raises(AssertionError):
        _assert_automatic_restart_has_exact_owned_handoff(null_handoff)

    skippable_handoff = main.replace(
        exact_call,
        "if (false) {\n"
        f"        {exact_call}\n"
        "    }\n"
        "    return false;",
        1,
    )
    with pytest.raises(AssertionError):
        _assert_automatic_restart_has_exact_owned_handoff(
            skippable_handoff
        )


def test_badge_firmware_restart_reservation_has_one_global_arm_first_owner():
    main = _source("esp32", "uplink", "main", "main.c")
    serial = _source(
        "esp32", "uplink", "main", "core", "serial_config.c"
    )
    recovery = _source(
        "esp32", "uplink", "main", "core", "badge_usb_recovery.c"
    )
    reserve_call = "fw_store_try_reserve_recovery_restart()"

    assert reserve_call not in main
    assert reserve_call not in serial
    assert recovery.count(reserve_call) == 1

    prepare_start = recovery.index(
        "badge_usb_recovery_prepare_firmware_restart("
    )
    prepare = recovery[
        prepare_start :
        recovery.index(
            "badge_usb_recovery_restart_with_owned_lease(",
            prepare_start,
        )
    ]
    arm = prepare.index("badge_runtime_arm_expected_reboot(")
    owned = prepare.index(
        "BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_OWNED", arm
    )
    reserve = prepare.index(reserve_call, owned)
    reserve_busy = prepare.index("if (!firmware_reserved)", reserve)
    release = prepare.index(
        "badge_runtime_release_expected_reboot(out_lease)", reserve_busy
    )
    busy_result = prepare.index(
        "BADGE_USB_FIRMWARE_RESTART_PREPARE_BUSY", release
    )
    owned_result = prepare.index(
        "BADGE_USB_FIRMWARE_RESTART_PREPARE_OWNED", busy_result
    )
    assert arm < owned < reserve < reserve_busy < release < busy_result
    assert busy_result < owned_result
    assert "while (" not in prepare


def test_every_prepared_firmware_restart_has_exact_nonreturning_lease_consumer():
    main = _source("esp32", "uplink", "main", "main.c")
    serial = _source(
        "esp32", "uplink", "main", "core", "serial_config.c"
    )
    recovery = _source(
        "esp32", "uplink", "main", "core", "badge_usb_recovery.c"
    )
    recovery_header = _source(
        "esp32", "uplink", "main", "core", "badge_usb_recovery.h"
    )
    prepare_call = "badge_usb_recovery_prepare_firmware_restart("
    owned = "BADGE_USB_FIRMWARE_RESTART_PREPARE_OWNED"

    assert main.count(prepare_call) == 5
    assert serial.count(prepare_call) == 2
    assert recovery.count(prepare_call) == 1
    assert (
        "_Noreturn void badge_usb_recovery_restart_with_owned_lease("
        in recovery_header
    )

    executor_start = recovery.index(
        "_Noreturn void badge_usb_recovery_restart_with_owned_lease("
    )
    executor = recovery[
        executor_start :
        recovery.index("bool badge_usb_recovery_restart(", executor_start)
    ]
    executor_compact = re.sub(r"\s+", " ", executor)
    assert executor_compact.count(
        "badge_runtime_expected_reboot_lease_is_owned(lease)"
    ) == 2
    restart = executor_compact.index("esp_restart();")
    parked = executor_compact.index(
        "park_after_irreversible_restart_failure(", restart
    )
    assert restart < parked

    helper_specs = (
        (
            "static bool badge_update_health_rollback(",
            "static bool badge_update_inactivity_restart(",
            "&rollback_lease",
        ),
        (
            "static bool badge_update_inactivity_restart(",
            "static bool badge_update_terminal_failure_restart(",
            "&reboot_lease",
        ),
        (
            "static bool badge_update_terminal_failure_restart(",
            "#endif",
            "&reboot_lease",
        ),
    )
    for start_marker, end_marker, lease_arg in helper_specs:
        start = main.index(start_marker)
        function = main[start:main.index(end_marker, start)]
        gate = re.search(
            rf"if\s*\(prepare_result\s*!=\s*{owned}\)\s*\{{"
            r".*?return false;\s*\}",
            function,
            re.S,
        )
        assert gate is not None
        committed_tail = function[gate.end():]
        exact_consumer = re.search(
            r"badge_usb_recovery_restart_with_owned_lease\("
            r".*?" + re.escape(lease_arg) + r"\s*\);",
            committed_tail,
            re.S,
        )
        assert exact_consumer is not None
        assert "return " not in committed_tail[:exact_consumer.end()]

    watchdog = main[main.index("/* ── 17. Connectivity watchdog") :]
    usb_restart = watchdog[
        watchdog.index(
            "if (usb_action == BADGE_USB_HEALTH_RESTART_SAFE_USB)"
        ) :
        watchdog.index("badge_runtime_note_main_stack_free")
    ]
    usb_compact = re.sub(r"\s+", " ", usb_restart)
    assert re.search(
        rf"if \(prepare_result == {owned}\) \{{ .*?"
        r"badge_usb_recovery_restart_with_owned_lease\("
        r" BADGE_USB_RESET_APP, \"usb_safe_once\", &reboot_lease\);"
        r" \} else \{",
        usb_compact,
    )

    assert serial.count("s_update_restart_owned = true;") == 2
    dispatch_start = serial.index("bool serial_config_dispatch_line(")
    dispatch = serial[
        dispatch_start :
        serial.index(
            "static void print_json_escaped_string(", dispatch_start
        )
    ]
    dispatch_compact = re.sub(r"\s+", " ", dispatch)
    assert re.search(
        r"if \(s_update_restart_owned\) \{ "
        r"badge_usb_recovery_restart_with_owned_lease\("
        r" BADGE_USB_RESET_APP, restart_reason, "
        r"&s_update_restart_lease\); \} "
        r"if \(!badge_usb_recovery_restart\(",
        dispatch_compact,
    )


def _assert_committed_restart_latch_is_preserved(transport: str) -> None:
    writes = re.findall(
        r"\bs_uplink_committed_restart_pending\s*=\s*(true|false)\s*;",
        transport,
    )
    assert writes == ["true"]
    latched = transport.index(
        "s_uplink_committed_restart_pending = true;"
    )
    retry_start = transport.index(
        "if (s_uplink_committed_restart_pending)", latched
    )
    retry = transport[
        retry_start :
        transport.index("if (s_uplink_abort_pending)", retry_start)
    ]
    assert "(void)restart_app(NULL);" in retry
    assert "vTaskDelay(pdMS_TO_TICKS(25));" in retry
    assert "continue;" in retry
    assert "s_uplink_committed_restart_pending =" not in retry
    assert retry_start < transport.index(
        "read(STDIN_FILENO", retry_start
    )


def test_badge_usb_committed_restart_latch_contract_rejects_early_clear():
    transport = _source(
        "esp32", "uplink", "main", "core", "badge_usb_transport.c"
    )
    _assert_committed_restart_latch_is_preserved(transport)

    early_clear = transport.replace(
        "s_uplink_committed_restart_pending = true;",
        "s_uplink_committed_restart_pending = true;\n"
        "            s_uplink_committed_restart_pending = false;",
        1,
    )
    with pytest.raises(AssertionError):
        _assert_committed_restart_latch_is_preserved(early_clear)


def test_badge_automatic_restart_owns_reboot_before_irreversible_fw_reservation():
    main = _source("esp32", "uplink", "main", "main.c")
    recovery = _source(
        "esp32", "uplink", "main", "core", "badge_usb_recovery.c"
    )
    helper_start = main.index(
        "static bool badge_automatic_restart_when_firmware_idle"
    )
    helper = main[
        helper_start : main.index("static void display_task", helper_start)
    ]

    prepare = helper.index(
        "badge_usb_recovery_prepare_firmware_restart("
    )
    owned = helper.index(
        "BADGE_USB_FIRMWARE_RESTART_PREPARE_OWNED", prepare
    )
    busy = helper.index(
        "BADGE_USB_FIRMWARE_RESTART_PREPARE_BUSY", owned
    )
    retry_delay = helper.index("vTaskDelay(", busy)
    retry_continue = helper.index("continue;", retry_delay)
    failed_return = helper.index("return false;", retry_continue)
    execute = helper.index(
        "rollback_and_reboot_with_owned_lease(", failed_return
    )
    assert (
        prepare
        < owned
        < busy
        < retry_delay
        < retry_continue
        < failed_return
        < execute
    )
    assert "vTaskDelay(portMAX_DELAY)" not in helper

    prepare_start = recovery.index(
        "badge_usb_recovery_prepare_firmware_restart("
    )
    prepare_fn = recovery[
        prepare_start :
        recovery.index(
            "badge_usb_recovery_restart_with_owned_lease(",
            prepare_start,
        )
    ]
    arm = prepare_fn.index("badge_runtime_arm_expected_reboot(")
    arm_owned = prepare_fn.index(
        "BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_OWNED", arm
    )
    reserve = prepare_fn.index(
        "fw_store_try_reserve_recovery_restart()", arm_owned
    )
    assert arm < arm_owned < reserve

    executor_start = main.index(
        "static _Noreturn void rollback_and_reboot_with_owned_lease("
    )
    executor = main[
        executor_start :
        main.index(
            "static bool badge_automatic_restart_when_firmware_idle",
            executor_start,
        )
    ]
    assert "badge_runtime_arm_expected_reboot(" not in executor
    prove = executor.index(
        "badge_runtime_expected_reboot_lease_is_owned("
    )
    rollback = executor.index(
        "esp_ota_mark_app_invalid_rollback_and_reboot()", prove
    )
    restart_prove = executor.index(
        "badge_runtime_expected_reboot_lease_is_owned(", rollback
    )
    restart = executor.index("esp_restart();", restart_prove)
    assert prove < rollback < restart_prove < restart


def test_badge_repeatable_automatic_failure_arms_one_boot_usb_recovery():
    main = _source("esp32", "uplink", "main", "main.c")
    helper_start = main.index(
        "static bool badge_automatic_restart_when_firmware_idle"
    )
    helper = main[
        helper_start:main.index("static void display_task", helper_start)
    ]

    prepare = helper.index(
        "badge_usb_recovery_prepare_firmware_restart("
    )
    owned = helper.index(
        "BADGE_USB_FIRMWARE_RESTART_PREPARE_OWNED", prepare
    )
    execute = helper.index(
        "rollback_and_reboot_with_owned_lease(", owned
    )
    assert prepare < owned < execute

    executor_start = main.index(
        "static _Noreturn void rollback_and_reboot_with_owned_lease("
    )
    executor = main[
        executor_start :
        main.index(
            "static bool badge_automatic_restart_when_firmware_idle",
            executor_start,
        )
    ]
    safe_once = executor.index("badge_runtime_arm_usb_recovery_once()")
    prove = executor.index(
        "badge_runtime_expected_reboot_lease_is_owned(", safe_once
    )
    restart = executor.index("esp_restart();", prove)
    assert safe_once < prove < restart


def test_badge_pending_verify_rollback_arms_safe_usb_only_after_rollback_returns():
    main = _source("esp32", "uplink", "main", "main.c")
    helper_start = main.index(
        "static _Noreturn void rollback_and_reboot_with_owned_lease("
    )
    helper = main[
        helper_start :
        main.index(
            "static bool badge_automatic_restart_when_firmware_idle",
            helper_start,
        )
    ]

    pending = helper.index("if (s_ota_pending_verify)")
    rollback = helper.index(
        "esp_ota_mark_app_invalid_rollback_and_reboot()", pending
    )
    safe_once = helper.index(
        "badge_runtime_arm_usb_recovery_once()", rollback
    )
    assert (
        "badge_runtime_arm_usb_recovery_once()"
        not in helper[pending:rollback]
    )
    assert pending < rollback < safe_once


def test_badge_failed_pending_verify_rollback_arms_safe_token_before_fallback_restart():
    main = _source("esp32", "uplink", "main", "main.c")
    helper_start = main.index(
        "static bool badge_automatic_restart_when_firmware_idle"
    )
    helper = main[
        helper_start:main.index("static void display_task", helper_start)
    ]

    legacy_target = helper.index(
        "BADGE_RUNTIME_EXPECTED_REBOOT_TARGET_LEGACY_V078_ROLLBACK"
    )
    prepare = helper.index(
        "badge_usb_recovery_prepare_firmware_restart(", legacy_target
    )
    owner = helper.index(
        "BADGE_USB_FIRMWARE_RESTART_PREPARE_OWNED", prepare
    )
    execute = helper.index(
        "rollback_and_reboot_with_owned_lease(", owner
    )
    assert legacy_target < prepare < owner < execute

    executor_start = main.index(
        "static _Noreturn void rollback_and_reboot_with_owned_lease("
    )
    executor = main[
        executor_start :
        main.index(
            "static bool badge_automatic_restart_when_firmware_idle",
            executor_start,
        )
    ]
    pending = executor.index("if (s_ota_pending_verify)")
    rollback = executor.index(
        "esp_ota_mark_app_invalid_rollback_and_reboot()", pending
    )
    safe_once = executor.index(
        "badge_runtime_arm_usb_recovery_once()", rollback
    )
    watchdog_log = executor.index(
        'ESP_LOGE(TAG, "WATCHDOG REBOOT: %s", reason)'
    )
    prove = executor.index(
        "badge_runtime_expected_reboot_lease_is_owned(", safe_once
    )
    restart = executor.index("esp_restart()", prove)
    assert (
        pending
        < rollback
        < safe_once
        < watchdog_log
        < prove
        < restart
    )


def test_badge_unhealthy_pending_boot_failed_rollback_enters_one_shot_usb_recovery():
    main = _source("esp32", "uplink", "main", "main.c")
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")
    function_start = main.index("static void rollback_check_at_boot(void)")
    function = main[function_start:main.index(
        "static void rollback_mark_valid", function_start
    )]

    unhealthy = function.index("reset_reason_is_unhealthy_for_rollback")
    rollback = function.index(
        "esp_ota_mark_app_invalid_rollback_and_reboot()", unhealthy
    )
    badge_guard = function.index("#ifdef FOF_BADGE_VARIANT", rollback)
    safe_once = function.index(
        "badge_runtime_arm_usb_recovery_once()", badge_guard
    )
    prove = function.index(
        "badge_runtime_expected_reboot_lease_is_owned(", safe_once
    )
    recovery = function.index("esp_restart();", prove)
    badge_end = function.index("#endif", recovery)
    assert (
        unhealthy
        < rollback
        < badge_guard
        < safe_once
        < prove
        < recovery
        < badge_end
    )

    # Badge builds compile exactly the early-boot, owned automatic executor,
    # and maintenance-health rollback sites. The fourth textual site belongs
    # exclusively to the #ifndef FOF_BADGE_VARIANT implementation.
    rollback_api = "esp_ota_mark_app_invalid_rollback_and_reboot()"
    nonbadge_start = main.index(
        "static void rollback_and_reboot_or_restart(const char *reason)"
    )
    nonbadge = main[
        nonbadge_start:main.index("#else", nonbadge_start)
    ]
    executor_start = main.index(
        "static _Noreturn void rollback_and_reboot_with_owned_lease("
    )
    executor = main[
        executor_start:main.index("#endif", executor_start)
    ]
    health_start = main.index(
        "static bool badge_update_health_rollback("
    )
    health_end = main.index(
        "static bool badge_update_inactivity_restart(",
        health_start,
    )
    health = main[health_start:health_end]
    assert function.count(rollback_api) == 1
    assert nonbadge.count(rollback_api) == 1
    assert executor.count(rollback_api) == 1
    assert health.count(rollback_api) == 1
    assert main.count(rollback_api) == 4
    prepare = health.index(
        "badge_usb_recovery_prepare_firmware_restart("
    )
    legacy_target = health.index(
        "BADGE_RUNTIME_EXPECTED_REBOOT_TARGET_LEGACY_V078_ROLLBACK",
        prepare,
    )
    owned = health.index(
        "BADGE_USB_FIRMWARE_RESTART_PREPARE_OWNED", legacy_target
    )
    receipt = health.index(
        '\\"error\\":\\"maintenance_health_failed\\"', owned
    )
    clear = health.index("badge_runtime_clear_update_maintenance(", receipt)
    prove = health.index(
        "badge_runtime_expected_reboot_lease_is_owned(", clear
    )
    maintenance_rollback = health.index(rollback_api, prove)
    fallback = health.index(
        "badge_runtime_arm_usb_recovery_once()", maintenance_rollback
    )
    restart = health.index(
        "badge_usb_recovery_restart_with_owned_lease(", fallback
    )
    assert (
        prepare
        < legacy_target
        < owned
        < receipt
        < clear
        < prove
        < maintenance_rollback
        < fallback
        < restart
    )
    assert serial.count("esp_ota_mark_app_invalid_rollback_and_reboot()") == 1
    assert '"usb_rollback"' in serial
    assert (
        "BADGE_RUNTIME_EXPECTED_REBOOT_TARGET_LEGACY_V078_ROLLBACK"
        in serial
    )


def test_consumed_automatic_recovery_token_forces_recovery_only_with_reason_precedence():
    main = _source("esp32", "uplink", "main", "main.c")
    app = main[main.index("void app_main(void)") :]
    runtime_init = app.index("badge_runtime_init(s_ota_pending_verify)")
    runtime_gate_end = app.index('log_detection_queue_heap("after_badge_runtime")')
    runtime_gate = app[runtime_init:runtime_gate_end]

    consumed = runtime_gate.index("badge_runtime_usb_recovery_once_consumed()")
    recovery_only = runtime_gate.index("badge_startup_recovery_only = true", consumed)
    precedence = runtime_gate.index("if (!badge_startup_safe_reason)", recovery_only)
    safe_once = runtime_gate.index(
        'badge_startup_safe_reason = "usb_safe_once"', precedence
    )
    force_safe = runtime_gate.index(
        "badge_runtime_force_safe_mode(true, badge_startup_safe_reason)", safe_once
    )
    assert consumed < recovery_only < precedence < safe_once < force_safe


def test_consumed_automatic_recovery_token_is_applied_before_usb_dispatch_and_services():
    main = _source("esp32", "uplink", "main", "main.c")
    app = main[main.index("void app_main(void)") :]
    runtime_init = app.index("badge_runtime_init(s_ota_pending_verify)")
    runtime_gate_end = app.index('log_detection_queue_heap("after_badge_runtime")')
    runtime_gate = app[runtime_init:runtime_gate_end]
    consumed = runtime_init + runtime_gate.index(
        "badge_runtime_usb_recovery_once_consumed()"
    )
    recovery_only = app.index("badge_startup_recovery_only = true", consumed)
    transport_mode = app.index(
        "badge_usb_transport_set_recovery_only(badge_startup_recovery_only)"
    )
    dispatch = app.index("badge_usb_transport_set_dispatch_ready()")
    recovery_return = app.index("if (badge_startup_recovery_only)", dispatch)
    event_loop = app.index("esp_event_loop_create_default()")
    assert consumed < recovery_only < transport_mode < dispatch < recovery_return < event_loop


def test_automatic_safe_escalation_does_not_change_intentional_reset_token_policy():
    main = _source("esp32", "uplink", "main", "main.c")
    recovery = _source(
        "esp32", "uplink", "main", "core", "badge_usb_recovery.c"
    )
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")
    display = _source("esp32", "uplink", "main", "hw", "display_st7735.c")
    helper_start = main.index(
        "static _Noreturn void rollback_and_reboot_with_owned_lease("
    )
    helper = main[
        helper_start :
        main.index(
            "static bool badge_automatic_restart_when_firmware_idle",
            helper_start,
        )
    ]

    assert "badge_runtime_arm_usb_recovery_once()" in helper
    arm = recovery.index("badge_runtime_arm_expected_reboot(")
    explicit_policy = recovery[arm:recovery.index(
        "if (target == BADGE_USB_RESET_ROM)", arm
    )]
    assert explicit_policy.index(
        "BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_OWNED"
    ) < explicit_policy.index("badge_runtime_arm_usb_recovery_once()")
    assert 'strcmp(reason, "usb_safe_once") == 0' in explicit_policy
    assert 'strcmp(reason, "uart_start_token_release") == 0' in explicit_policy
    assert explicit_policy.count("badge_runtime_arm_usb_recovery_once()") == 1
    assert '"usb_reboot"' in serial
    assert '"usb_bootloader"' in serial
    assert "badge_usb_recovery_target(flash_confirmed)" in display
    assert "badge_usb_recovery_target(false)" in display
    assert '"button_reboot"' in display
    assert '"button_usb_rom"' in display


def test_badge_required_worker_failure_defers_automatic_restart():
    main = _source("esp32", "uplink", "main", "main.c")
    app = main[main.index("void app_main(void)") :]
    normal_startup = app.index("/* ── 14. Start all tasks")
    required_start = app.index("if (!required_tasks_started)", normal_startup)
    required_failure = app[
        required_start :
        app.index("xTaskNotifyGive(s_display_task_handle)", required_start)
    ]
    assert "#ifdef FOF_BADGE_VARIANT" in required_failure
    assert "badge_automatic_restart_when_firmware_idle(" in required_failure
    assert "#else" in required_failure
    assert "rollback_and_reboot_or_restart(" in required_failure


def test_badge_coordinator_restore_failure_defers_automatic_restart():
    main = _source("esp32", "uplink", "main", "main.c")
    app = main[main.index("void app_main(void)") :]
    restore_start = app.index(
        "if (!badge_safe_usb && !fw_store_restore_auto_update_coordinator())"
    )
    restore_failure = app[restore_start:app.index(
        "/* ── 17. Connectivity watchdog", restore_start
    )]
    assert "badge_automatic_restart_when_firmware_idle(" in restore_failure
    assert "#else" in restore_failure
    assert "rollback_and_reboot_or_restart(" in restore_failure


def test_both_badge_low_heap_modes_defer_automatic_restart():
    main = _source("esp32", "uplink", "main", "main.c")
    app = main[main.index("void app_main(void)") :]
    watchdog = app[app.index("/* ── 17. Connectivity watchdog") :]
    standalone_start = watchdog.index("if (!badge_backend_enabled)")
    standalone_end = watchdog.index("continue;", standalone_start)
    standalone = watchdog[standalone_start:standalone_end]
    backend_start = watchdog.rindex("if (free_heap < 4000")
    backend = watchdog[backend_start:]
    for low_heap in (standalone, backend):
        assert "#ifdef FOF_BADGE_VARIANT" in low_heap
        assert "badge_automatic_restart_when_firmware_idle(reason)" in low_heap
        assert "#else" in low_heap
        assert "!fw_store_is_relay_active()" in low_heap


def test_firmware_operation_ownership_uses_exact_owner_generation_tokens():
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    header = _source("esp32", "uplink", "main", "network", "fw_store.h")
    prepare = _source(
        "esp32", "uplink", "main", "network",
        "fw_relay_prepare_adapter.c",
    )
    token_header = _source("esp32", "shared", "firmware_operation_token.h")

    for owner in (
        "FW_OPERATION_OWNER_SCANNER_STAGING",
        "FW_OPERATION_OWNER_SCANNER_RELAY",
        "FW_OPERATION_OWNER_UPLINK_OTA",
    ):
        assert owner in token_header
    assert "fw_operation_owner_t owner;" in token_header
    assert "uint32_t generation;" in token_header
    assert "bool valid;" in token_header
    assert "#include \"firmware_operation_token.h\"" in header
    assert "bool fw_store_operation_try_begin(" in header
    assert "bool acquire_uart_lease" in header
    assert "fw_operation_token_t *out_token" in header
    assert "bool fw_store_operation_end(fw_operation_token_t token);" in header

    assert "static fw_operation_state_t s_operation" in store
    assert "static bool s_operation_active" not in store
    assert "static bool s_operation_uart_lease" not in store
    assert "static bool operation_try_begin" not in store
    assert "static void operation_end" not in store

    begin = store[
        store.index("bool fw_store_operation_try_begin(") :
        store.index("bool fw_store_operation_end(")
    ]
    assert "portENTER_CRITICAL(&s_operation_lock)" in begin
    assert "fw_operation_state_try_begin" in begin
    assert "portEXIT_CRITICAL(&s_operation_lock)" in begin
    assert begin.index("portEXIT_CRITICAL(&s_operation_lock)") < begin.index(
        "uart_rx_scanner_tx_lease_acquire"
    )
    assert "fw_operation_state_end" in begin

    end = store[
        store.index("bool fw_store_operation_end(") :
        store.index("bool fw_store_operation_is_active")
    ]
    assert "fw_operation_state_end" in end
    assert end.index("portEXIT_CRITICAL(&s_operation_lock)") < end.index(
        "uart_rx_scanner_tx_lease_release"
    )

    serial_type = store[
        store.index("typedef struct {", store.index("FW_STAGE_BUF_FALLBACK")) :
        store.index("} serial_upload_state_t;")
    ]
    assert "fw_operation_token_t operation_token;" in serial_type
    assert "FW_OPERATION_OWNER_SCANNER_STAGING" in store
    assert "FW_OPERATION_OWNER_SCANNER_RELAY" in prepare
    assert "fw_store_operation_try_begin(" in prepare
    assert "FW_OPERATION_OWNER_UPLINK_OTA" not in store
    assert "operation_try_begin()" not in store
    assert "operation_end()" not in store

    abort = store[
        store.index("void fw_store_serial_upload_abort") :
        store.index("bool fw_store_serial_upload_begin")
    ]
    token_copy = abort.index("fw_operation_token_t operation_token")
    clear = abort.index("memset(&s_serial_upload")
    release = abort.index("fw_store_operation_end(operation_token)")
    assert token_copy < clear < release

    reserve = store[
        store.index("bool fw_store_try_reserve_recovery_restart") :
        store.index("fw_store_activity_t fw_store_activity_sample")
    ]
    assert "fw_operation_state_try_reserve_recovery_restart" in reserve
    assert "portENTER_CRITICAL(&s_operation_lock)" in reserve
    assert "portEXIT_CRITICAL(&s_operation_lock)" in reserve


def test_uplink_usb_ota_is_a_transport_neutral_isolated_adapter():
    header = _source("esp32", "uplink", "main", "core", "uplink_usb_ota.h")
    source = _source("esp32", "uplink", "main", "core", "uplink_usb_ota.c")
    cmake = _source("esp32", "uplink", "main", "CMakeLists.txt")

    for declaration in (
        "bool uplink_usb_ota_begin(",
        "bool uplink_usb_ota_write(",
        "bool uplink_usb_ota_finish(",
        "bool uplink_usb_ota_abort(",
        "uint32_t uplink_usb_ota_remaining(void);",
        "bool uplink_usb_ota_get_status(",
    ):
        assert declaration in header
    assert "uplink_usb_ota_result_t" in header
    assert "bool retryable;" in header
    assert "#define UPLINK_USB_OTA_REMAINING_UNKNOWN UINT32_MAX" in header
    assert "fw_store_operation_try_begin(" in source
    assert "FW_OPERATION_OWNER_UPLINK_OTA" in source
    assert "false, &" in source
    assert "atomic_uint" in source
    assert "SRC_DIRS" in cmake and '"core"' in cmake
    for exact_gate in (
        "UPLINK_USB_OTA_0_OFFSET 0x20000U",
        "UPLINK_USB_OTA_1_OFFSET 0x220000U",
        "UPLINK_USB_OTA_SLOT_SIZE 0x200000U",
        "UPLINK_USB_OTA_DESCRIPTOR_BYTES 144U",
        "UPLINK_USB_OTA_MAX_WRITE_BYTES",
        "fof_firmware_image_parse_identity(",
        "uplink_ota_policy_verify_complete(",
        "uplink_ota_policy_mark_committed(",
        "esp_ota_get_running_partition()",
        "esp_ota_get_next_update_partition(",
        "esp_ota_get_partition_description(",
        "esp_ota_set_boot_partition(",
        "ensure_default_hooks();",
        "uart_rx_pause_scanner_guarded(",
        "uart_rx_resume_scanner_guarded(",
    ):
        assert exact_gate in source

    finish = source[source.index("static bool uplink_usb_ota_finish_locked(") :]
    ota_end = finish.index("s_hooks.ota_end(")
    descriptor_reread = finish.index("s_hooks.get_partition_identity(", ota_end)
    live_running = finish.index("s_hooks.get_running(", descriptor_reread)
    live_next = finish.index("s_hooks.get_next(", live_running)
    set_boot = finish.index("s_hooks.set_boot_partition(", live_next)
    committed_latch = finish.index(
        "uplink_ota_policy_mark_committed(", set_boot
    )
    assert ota_end < descriptor_reread < live_running < live_next < set_boot
    assert set_boot < committed_latch
    assert "s_session.state = UPLINK_USB_OTA_COMMITTED;" in finish[
        set_boot:committed_latch
    ]
    assert 'snprintf(out->partition' not in source
    assert "target_identity_unterminated" in finish
    target_compare = finish[
        finish.index("fof_firmware_image_identity_t target_identity") :
        finish.index("uplink_usb_ota_partition_t live_running")
    ]
    assert target_compare.count("bounded_text(") >= 2
    default_http = source[
        source.index("static bool default_pause_http") :
        source.index("static bool default_pause_scanner")
    ]
    assert "#ifdef FOF_BADGE_VARIANT" in default_http
    badge_http = default_http[
        default_http.index("#ifdef FOF_BADGE_VARIANT") :
        default_http.index("#else")
    ]
    assert "*owned = false;" in badge_http
    assert "return true;" in badge_http
    nonbadge_http = default_http[
        default_http.index("#else") :
        default_http.index("#endif")
    ]
    assert "*owned = false;" in nonbadge_http
    assert "return false;" in nonbadge_http
    for unsafe_http_pause in (
        "http_upload_task_alive",
        "http_upload_is_paused",
        "http_upload_pause",
        "http_upload_resume",
    ):
        assert unsafe_http_pause not in source
    assert "atomic_flag s_mutator" in source
    assert 'result_busy(out, "adapter_busy")' in source
    busy_result = source[
        source.index("static void result_busy") :
        source.index("static bool copy_bounded_text")
    ]
    assert "out->retryable = true;" in busy_result
    assert "out->phase = UPLINK_USB_OTA_PHASE_NONE;" in busy_result
    assert "UPLINK_USB_OTA_PHASE_ABORTED" not in busy_result
    begin = source[
        source.index("static bool uplink_usb_ota_begin_locked(") :
        source.index("static bool uplink_usb_ota_write_locked(")
    ]
    release_pending = begin[
        begin.index("s_session.operation_owned") :
        begin.index("if (!manifest")
    ]
    assert 'result_busy(out, "operation_release_failed")' in release_pending
    assert "result_error(" not in release_pending
    operation_active = begin[
        begin.index("if (!s_hooks.operation_begin(") :
        begin.index("s_session.operation_owned = true;")
    ]
    assert 'result_busy(out, "operation_active")' in operation_active
    assert "result_error(" not in operation_active
    live_state_gate = begin[
        begin.index("s_session.state != UPLINK_USB_OTA_IDLE") :
        begin.index("memset(&s_session")
    ]
    assert 'result_busy(out, "invalid_state")' in live_state_gate
    assert "result_error(" not in live_state_gate
    status_reader = source[source.index("bool uplink_usb_ota_get_status(") :]
    assert "for (unsigned attempt = 0U; attempt < 3U; ++attempt)" in status_reader
    assert "do {" not in status_reader
    remaining = source[
        source.index("uint32_t uplink_usb_ota_remaining(void)") :
        source.index("bool uplink_usb_ota_get_status(")
    ]
    assert "uplink_usb_ota_get_status(&status)" in remaining
    assert "UPLINK_USB_OTA_REMAINING_UNKNOWN" in remaining
    assert "s_session" not in remaining
    cleanup = source[
        source.index("static bool precommit_cleanup") :
        source.index("#ifndef UNIT_TESTING", source.index(
            "static bool precommit_cleanup"
        ))
    ]
    release_attempt = cleanup.index("s_hooks.operation_end(")
    resume_scanner1 = cleanup.index("s_hooks.resume_scanner(s_hooks.context, 1U)")
    resume_scanner0 = cleanup.index("s_hooks.resume_scanner(s_hooks.context, 0U)")
    resume_http = cleanup.index("s_hooks.resume_http(s_hooks.context)")
    assert resume_scanner1 < resume_scanner0 < resume_http < release_attempt
    release_failure = cleanup[
        cleanup.index("if (!s_hooks.operation_end(") :
        cleanup.index("s_session.operation_owned = false")
    ]
    assert 'result_busy(out, "operation_release_failed")' in release_failure
    assert "return false;" in release_failure
    assert "terminal_cleanup_receipt_emitted" in cleanup
    for forbidden in (
        "serial_config",
        "badge_usb_transport",
        "httpd_",
        "fw_store_serial_upload",
        "fw_store_get_target_partition",
        "fw_scanner_s3",
        "nvs_config",
        "psram",
        "FOF_UPLINK_OTA:",
    ):
        assert forbidden not in source


def test_uplink_ota_pause_is_generation_bound_and_startup_gate_is_public():
    uart_header = _source("esp32", "uplink", "main", "comms", "uart_rx.h")
    uart_source = _source("esp32", "uplink", "main", "comms", "uart_rx.c")
    store_header = _source("esp32", "uplink", "main", "network", "fw_store.h")
    store_source = _source("esp32", "uplink", "main", "network", "fw_store.c")

    assert "uint32_t request_generation;" in uart_header
    assert "bool acquired;" in uart_header
    assert "} uart_rx_pause_guard_t;" in uart_header
    assert "bool uart_rx_scanner_task_started(int scanner_id);" in uart_header
    assert "bool uart_rx_scanner_is_paused(int scanner_id);" in uart_header
    assert "bool uart_rx_pause_scanner_guarded(" in uart_header
    assert "void uart_rx_resume_scanner_guarded(" in uart_header
    for required in (
        "s_rx_pause_request_generation_ble",
        "s_rx_pause_ack_generation_ble",
        "s_rx_pause_request_generation_wifi",
        "s_rx_pause_ack_generation_wifi",
        "ack_generation == request_generation",
        "guard->request_generation",
        "guard->acquired",
        "UINT32_MAX",
    ):
        assert required in uart_source
    guarded_resume = uart_source[
        uart_source.index("void uart_rx_resume_scanner_guarded(") :
        uart_source.index("void uart_rx_resume_scanner(")
    ]
    assert "atomic_compare_exchange" in guarded_resume

    assert "bool fw_store_operation_is_active(void);" in store_header
    active = store_source[
        store_source.index("bool fw_store_operation_is_active(void)") :
        store_source.index("bool fw_store_try_reserve_recovery_restart")
    ]
    assert "portENTER_CRITICAL(&s_operation_lock)" in active
    assert "s_operation.active" in active
    assert "portEXIT_CRITICAL(&s_operation_lock)" in active


def test_uart_runtime_startup_is_atomically_owned_and_waits_for_task_entry():
    token_header = _source("esp32", "shared", "firmware_operation_token.h")
    main = _source("esp32", "uplink", "main", "main.c")
    uart = _source("esp32", "uplink", "main", "comms", "uart_rx.c")

    assert "FW_OPERATION_OWNER_RUNTIME_STARTUP" in token_header
    gate = main[
        main.index("static bool uart_startup_try_claim") :
        main.index("void app_main")
    ]
    assert "fw_store_operation_try_begin(" in gate
    assert "FW_OPERATION_OWNER_RUNTIME_STARTUP" in gate
    assert "uart_rx_start()" in gate
    assert "uart_startup_gate_run(" in gate
    assert "UART_STARTUP_RELEASE_ATTEMPTS" in gate
    assert "UART_STARTUP_RELEASE_RETRY_MS" in gate
    assert "fw_store_operation_is_active()" not in gate
    assert "#define UART_STARTUP_RELEASE_ATTEMPTS 3U" in main
    assert "#define UART_STARTUP_RELEASE_RETRY_MS 10U" in main
    assert main.count("uart_rx_start()") == 1
    badge_start = main[main.index("uart_startup_gate_result_t uart_startup") :]
    assert 'badge_runtime_force_safe_mode(true, "uart_rx_start")' in badge_start
    assert "badge_usb_transport_set_recovery_only(true)" in badge_start
    assert "badge_automatic_restart_when_firmware_idle(" in badge_start
    assert '"uart_rx_start"' in badge_start
    release_failed = badge_start[
        badge_start.index("if (uart_startup == UART_STARTUP_GATE_RELEASE_FAILED)") :
        badge_start.index("badge_automatic_restart_when_firmware_idle", badge_start.index(
            "if (uart_startup == UART_STARTUP_GATE_RELEASE_FAILED)"
        ))
    ]
    drain = release_failed.index(
        "badge_usb_transport_drain(pdMS_TO_TICKS(250))"
    )
    restart = release_failed.index("badge_usb_recovery_restart(", drain)
    assert drain < restart
    assert "BADGE_USB_RESET_APP" in release_failed[restart:]
    assert '"uart_start_token_release"' in release_failed[restart:]
    assert "badge_runtime_arm_usb_recovery_once()" not in release_failed
    assert "fw_store_try_reserve_recovery_restart" not in release_failed
    assert "badge_automatic_restart_when_firmware_idle" not in release_failed

    for required in (
        "s_rx_task_entered_ble",
        "s_rx_task_entered_wifi",
        "atomic_store_explicit(task_entered, true",
        "wait_for_rx_task_entries",
        "atomic_store_explicit(&s_rx_task_entered_ble, false",
    ):
        assert required in uart
    task_entry = uart[
        uart.index("static void uart_rx_task") :
        uart.index("bool uart_rx_scanner_tx_lease_init")
    ]
    assert task_entry.index("if (!line_buf || !read_buf)") < task_entry.index(
        "atomic_store_explicit(task_entered, true"
    ) < task_entry.index("while (1)")
    partial_failure = uart[
        uart.index("if (wifi_ok != pdPASS)") :
        uart.index("ESP_LOGI(TAG, \"WiFi scanner RX task created\")")
    ]
    assert "vTaskDelete(ble_task)" in partial_failure
    assert "s_rx_task_entered_ble" in partial_failure
    entry_timeout = uart[
        uart.index("if (!wait_for_rx_task_entries())") :
        uart.index("return true;", uart.index("if (!wait_for_rx_task_entries())"))
    ]
    assert "vTaskDelete(ble_task)" in entry_timeout
    assert "vTaskDelete(wifi_task)" in entry_timeout
    for flag in (
        "s_rx_task_entered_ble",
        "s_rx_task_entered_wifi",
        "s_rx_task_started_ble",
        "s_rx_task_started_wifi",
    ):
        assert flag in entry_timeout


def test_badge_usb_uplink_ota_begin_is_one_exact_normal_and_recovery_handler():
    transport = _source("esp32", "uplink", "main", "core", "badge_usb_transport.c")
    transport_header = _source(
        "esp32", "uplink", "main", "core", "badge_usb_transport.h"
    )
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")
    serial_header = _source("esp32", "uplink", "main", "core", "serial_config.h")
    ingress = _source(
        "esp32", "uplink", "main", "core", "serial_config_ingress.c"
    )

    assert '"uplink_ota_begin"' in serial
    assert '"uplink_upload_begin"' not in serial
    assert "SERIAL_CONFIG_RECOVERY_UPLINK_OTA_BEGIN" in serial_header
    assert "bool serial_config_dispatch_uplink_ota_begin(" in serial_header
    assert "const uint8_t *line, size_t line_byte_len" in serial_header
    assert "badge_usb_transport_handle_uplink_ota_begin(" in transport_header

    classifier = ingress[
        ingress.index("serial_config_recovery_command_t ") :
    ]
    assert "FOF_FW_JSON_SCHEMA_USB_UPLINK_OTA_BEGIN" in ingress
    assert "FOF_FW_JSON_SCHEMA_USB_UPLINK_OTA_BEGIN_SESSION" in ingress
    assert "schema_is_uplink_ota_begin(" in classifier
    assert "SERIAL_CONFIG_RECOVERY_UPLINK_OTA_BEGIN" in classifier

    recovery_branch = serial[
        serial.index("bool serial_config_dispatch_recovery_command(") :
        serial.index("bool serial_config_dispatch_line(")
    ]
    assert (
        "serial_config_dispatch_uplink_ota_begin(\n"
        "            line, line_byte_len)"
        in recovery_branch
    )

    normal_dispatch = serial[
        serial.index("bool serial_config_dispatch_line(") :
        serial.index("static void print_json_escaped_string", serial.index(
            "bool serial_config_dispatch_line("
        ))
    ]
    assert "serial_config_ingress_authorize(" in normal_dispatch
    assert "FOF_FW_JSON_SCHEMA_USB_UPLINK_OTA_BEGIN" in normal_dispatch
    assert "serial_config_dispatch_uplink_ota_begin(" in normal_dispatch

    begin = serial[
        serial.index("bool serial_config_dispatch_uplink_ota_begin(") :
        serial.index("bool serial_config_dispatch_recovery_command(")
    ]
    assert "serial_config_ingress_is_uplink_ota_begin(" in begin
    for exact_field in (
        '"cmd"', '"target"', '"project"', '"hardware_type"', '"version"',
        '"size"', '"crc32"', '"sha256"', '"flow_control"',
        '"recovery_rewrite_same_version"', '"session"',
    ):
        assert exact_field in begin
    assert "member_count != 10 && member_count != 11" in begin
    assert "serial_json_uint32_exact(size_item, &fields.size)" in begin
    assert "serial_json_uint32_exact(crc_item, &fields.crc32)" in begin
    assert "cJSON_IsBool(recovery_item)" in begin
    session_match = begin.index("badge_runtime_update_session_matches(")
    admission = begin.index(
        "badge_update_uplink_ota_begin_admission_decide("
    )
    manifest = begin.index("badge_usb_uplink_ota_manifest_from_fields(")
    transport_begin = begin.index(
        "badge_usb_transport_handle_uplink_ota_begin(&manifest)"
    )
    assert session_match < admission < manifest < transport_begin
    assert "BADGE_UPDATE_OTA_BEGIN_REJECT_SESSION_MISMATCH" in begin
    assert '"update_session_mismatch"' in begin
    assert '"unexpected_update_session"' in begin
    assert "badge_usb_uplink_ota_manifest_from_fields(" in begin
    assert "badge_usb_transport_handle_uplink_ota_begin(&manifest)" in begin


def test_badge_usb_uplink_ota_ready_is_drained_before_binary_parser_is_armed():
    transport = _source("esp32", "uplink", "main", "core", "badge_usb_transport.c")
    handler = transport[
        transport.index("bool badge_usb_transport_handle_uplink_ota_begin(") :
        transport.index("void badge_usb_transport_snapshot(")
    ]
    begin = handler.index("uplink_usb_ota_begin(manifest, &result)")
    flow_begin = handler.index("badge_usb_uplink_ota_flow_begin_result(", begin)
    receipt = handler.index("deliver_uplink_receipt(&result", flow_begin)
    arm = handler.index("badge_usb_stream_begin_binary(", receipt)
    assert begin < flow_begin < receipt < arm
    receipt_helper = transport[
        transport.index(
            "static badge_usb_uplink_receipt_decision_t deliver_uplink_receipt("
        ) :
        transport.index("static badge_usb_binary_target_t clear_upload_health")
    ]
    emit = receipt_helper.index("badge_usb_transport_emit_detailed(")
    enqueued = receipt_helper.index("emitted == BADGE_USB_EMIT_ENQUEUED", emit)
    drain = receipt_helper.index("badge_usb_transport_drain(", enqueued)
    flow_receipt = receipt_helper.index(
        "badge_usb_uplink_ota_flow_receipt_result(", drain
    )
    assert emit < enqueued < drain < flow_receipt
    assert "BADGE_USB_FRAME_REQUIRED" in receipt_helper
    assert '(void)abort_uplink_ota("invalid_ready_result")' in handler
    assert 'deliver_uplink_receipt(&result, "ready_receipt_failed")' in handler
    assert '(void)abort_uplink_ota("usb_parser_busy")' in handler


def test_badge_usb_uplink_ota_binary_loop_is_transactional_and_fail_closed():
    transport = _source("esp32", "uplink", "main", "core", "badge_usb_transport.c")
    task = transport[
        transport.index("static void badge_usb_transport_task(void *arg)") :
        transport.index("bool badge_usb_transport_start(")
    ]
    consume = transport[
        transport.index("static badge_usb_uplink_action_t consume_uplink_bytes(") :
        transport.index("static void badge_usb_transport_task(void *arg)")
    ]
    assert "badge_usb_uplink_ota_flow_plan_read(" in consume
    assert "badge_usb_stream_peek_binary(" in consume
    write = consume.index("uplink_usb_ota_write(")
    flow = consume.index("badge_usb_uplink_ota_flow_write_result(", write)
    commit = consume.index("badge_usb_stream_commit_binary(", flow)
    assert write < flow < commit
    assert "UPLINK_USB_OTA_MAX_WRITE_BYTES" in consume
    assert "pending_bytes" in consume
    assert "BADGE_USB_UPLINK_ACTION_RETRY_PENDING" in consume
    assert "BADGE_USB_UPLINK_ACTION_ABORT_DROP" in consume
    assert "offset = (size_t)bytes_read" in task

    timeout = task[task.index("badge_usb_stream_binary_timed_out(") :]
    assert 'abort_uplink_ota("usb_idle_timeout")' in timeout
    abort_start = transport.index(
        "static badge_usb_uplink_action_t abort_uplink_ota(",
        transport.index("static badge_usb_uplink_action_t complete_uplink_abort("),
    )
    abort_helper = transport[
        abort_start : transport.index("typedef struct {", abort_start)
    ]
    assert abort_helper.index("uplink_usb_ota_abort(") \
        < abort_helper.index("complete_uplink_abort(")
    cleanup = transport[
        transport.index("static badge_usb_uplink_action_t complete_uplink_abort(") :
        abort_start
    ]
    assert cleanup.index("badge_usb_uplink_ota_flow_take_cleanup(") \
        < cleanup.index("clear_uplink_parser_and_health(")
    assert "badge_usb_stream_poll_timeout(" in task  # scanner legacy path


def test_badge_usb_uplink_ota_committed_always_restarts_after_emit_and_drain():
    transport = _source("esp32", "uplink", "main", "core", "badge_usb_transport.c")
    finish = transport[
        transport.index("static badge_usb_uplink_action_t finish_uplink_ota(") :
        transport.index("static badge_usb_uplink_action_t consume_uplink_bytes(")
    ]
    assert "uplink_usb_ota_finish(" in finish
    assert "badge_usb_uplink_ota_flow_finish_result(" in finish
    assert "badge_usb_uplink_ota_run_committed(&hooks)" in finish
    assert ".emit_committed = restart_emit_committed" in finish
    assert ".drain = restart_drain" in finish
    assert ".restart = restart_app" in finish


def test_badge_usb_uplink_begin_release_failure_latches_cleanup_and_drops_read_remainder():
    transport = _source("esp32", "uplink", "main", "core", "badge_usb_transport.c")
    handler = transport[
        transport.index("bool badge_usb_transport_handle_uplink_ota_begin(") :
        transport.index("void badge_usb_transport_snapshot(")
    ]
    failed = handler[handler.index("if (!uplink_usb_ota_begin") :
                     handler.index("badge_usb_uplink_ota_flow_begin_result")]
    emit = failed.index("emit_uplink_ota_result")
    classify = failed.index("badge_usb_uplink_ota_begin_failure_action")
    latch = failed.index("latch_uplink_abort")
    assert emit < classify < latch
    assert "BADGE_USB_UPLINK_ACTION_RETRY_CLEANUP" in failed

    task = transport[
        transport.index("static void badge_usb_transport_task(void *arg)") :
        transport.index("bool badge_usb_transport_start(")
    ]
    line = task[task.index("if (event == BADGE_USB_EVENT_LINE)") :
                task.index("} else if (event == BADGE_USB_EVENT_BINARY_CHUNK")]
    assert "s_uplink_abort_pending" in line
    assert "offset = (size_t)bytes_read" in line
    assert "break" in line


def test_badge_usb_uplink_retries_are_bounded_and_force_one_shot_safe_usb_recovery():
    transport = _source("esp32", "uplink", "main", "core", "badge_usb_transport.c")
    recovery = transport[
        transport.index("static void recover_uplink_usb_once(") :
        transport.index("static badge_usb_uplink_action_t complete_uplink_abort(")
    ]
    assert "if (!badge_usb_recovery_restart(" in recovery
    assert "BADGE_USB_RESET_APP" in recovery
    assert '"usb_safe_once"' in recovery

    task = transport[
        transport.index("static void badge_usb_transport_task(void *arg)") :
        transport.index("bool badge_usb_transport_start(")
    ]
    read = task.index("read(STDIN_FILENO")
    for pending in (
        "if (s_uplink_abort_pending)",
        "if (s_uplink_finish_pending)",
        "if (s_uplink_terminal_pending)",
        "if (pending_length != 0U)",
    ):
        assert task.index(pending) < read
    assert "BADGE_USB_UPLINK_OTA_RETRY_LIMIT" in transport
    assert "BADGE_USB_UPLINK_ACTION_RECOVERY_RESTART" in transport


def test_badge_usb_uplink_terminal_receipt_uses_detailed_cached_outcome_policy():
    transport = _source("esp32", "uplink", "main", "core", "badge_usb_transport.c")
    assert "s_uplink_terminal_frame[BADGE_USB_UPLINK_OTA_FRAME_BYTES]" in transport
    assert "static badge_usb_emit_result_t badge_usb_transport_emit_detailed(" in transport
    terminal_start = transport.index(
        "static badge_usb_uplink_action_t deliver_uplink_terminal("
    )
    terminal = transport[
        terminal_start : transport.index(
            "static void recover_uplink_usb_once(", terminal_start
        )
    ]
    assert "badge_usb_transport_emit_detailed(" in terminal
    assert "badge_usb_uplink_ota_flow_terminal_emit_result(" in terminal
    assert "BADGE_USB_UPLINK_ACTION_RETRY_TERMINAL" in terminal
    assert "BADGE_USB_UPLINK_ACTION_RECOVERY_RESTART" in terminal
    assert "BADGE_USB_EMIT_ENQUEUED" in _source(
        "esp32", "shared", "badge_usb_transport_policy.c"
    )
    public_emit = transport[
        transport.index("bool badge_usb_transport_emit(") :
        transport.index("bool badge_usb_transport_drain(")
    ]
    assert "priority != BADGE_USB_FRAME_REQUIRED" in public_emit


def test_badge_usb_uplink_committed_restart_reports_ownership_rejection():
    transport = _source("esp32", "uplink", "main", "core", "badge_usb_transport.c")
    shared_header = _source("esp32", "shared", "badge_usb_uplink_ota.h")
    restart = transport[
        transport.index("static bool restart_app(void *context)") :
        transport.index("static badge_usb_uplink_action_t finish_uplink_ota(")
    ]
    badge = restart[
        restart.index("#ifdef FOF_BADGE_VARIANT") : restart.index("#else")
    ]
    generic = restart[restart.index("#else") : restart.index("#endif")]
    assert "badge_usb_recovery_restart(" in badge
    assert "return accepted;" in badge
    assert "esp_restart()" not in badge
    call = generic.index("esp_restart()")
    assert "for (;;)" in generic[call:]
    assert "vTaskDelay(" in generic[call:]
    assert "bool (*restart)(void *context);" in shared_header

    finish = transport[
        transport.index("static badge_usb_uplink_action_t finish_uplink_ota(") :
        transport.index("static badge_usb_uplink_action_t consume_uplink_bytes(")
    ]
    rejected = finish.index("if (!badge_usb_uplink_ota_run_committed(&hooks))")
    recovery_action = finish.index(
        "BADGE_USB_UPLINK_ACTION_RECOVERY_RESTART", rejected
    )
    committed_action = finish.index(
        "BADGE_USB_UPLINK_ACTION_COMMITTED_RESTART", recovery_action
    )
    assert rejected < recovery_action < committed_action


def test_badge_usb_uplink_committed_restart_rejection_is_latched_and_retried():
    transport = _source("esp32", "uplink", "main", "core", "badge_usb_transport.c")
    assert "static bool s_uplink_committed_restart_pending" in transport

    finish = transport[
        transport.index("static badge_usb_uplink_action_t finish_uplink_ota(") :
        transport.index("static badge_usb_uplink_action_t consume_uplink_bytes(")
    ]
    rejected = finish.index(
        "if (!badge_usb_uplink_ota_run_committed(&hooks))"
    )
    latch = finish.index(
        "s_uplink_committed_restart_pending = true;", rejected
    )
    recovery = finish.index(
        "BADGE_USB_UPLINK_ACTION_RECOVERY_RESTART", latch
    )
    assert rejected < latch < recovery

    task = transport[
        transport.index("static void badge_usb_transport_task(void *arg)") :
        transport.index("bool badge_usb_transport_start(")
    ]
    retry = task.index("if (s_uplink_committed_restart_pending)")
    restart = task.index("restart_app(NULL)", retry)
    retry_delay = task.index("vTaskDelay(", restart)
    retry_continue = task.index("continue;", retry_delay)
    read = task.index("read(STDIN_FILENO")
    assert retry < restart < retry_delay < retry_continue < read

    consume = task[
        task.index("if (s_stream.target == BADGE_USB_BINARY_UPLINK)") :
        task.index(
            "if (s_stream.target == BADGE_USB_BINARY_SCANNER",
            task.index("if (s_stream.target == BADGE_USB_BINARY_UPLINK)"),
        )
    ]
    assert "BADGE_USB_UPLINK_ACTION_RECOVERY_RESTART" in consume


def test_badge_usb_uplink_ready_and_credit_share_typed_atomic_receipt_helper():
    transport = _source("esp32", "uplink", "main", "core", "badge_usb_transport.c")
    helper = transport[
        transport.index("static badge_usb_uplink_receipt_decision_t deliver_uplink_receipt(") :
        transport.index("static badge_usb_binary_target_t clear_upload_health(")
    ]
    assert helper.count("badge_usb_transport_emit_detailed(") == 1
    assert helper.count("badge_usb_transport_drain(") == 1
    enqueued = helper.index("emitted == BADGE_USB_EMIT_ENQUEUED")
    rescue = helper.index("badge_usb_transport_drain(", enqueued)
    decide = helper.index("badge_usb_uplink_ota_receipt_decide(", rescue)
    assert enqueued < rescue < decide
    assert "badge_usb_uplink_ota_receipt_finalize(" in helper
    assert "decision = BADGE_USB_UPLINK_RECEIPT_ABORT_TERMINAL" not in helper
    assert "health_note_required_rescued" not in transport
    assert "BADGE_USB_UPLINK_RECEIPT_ABORT_TERMINAL" in helper
    assert "BADGE_USB_UPLINK_RECEIPT_CLEANUP_RECOVERY" in helper
    assert "s_uplink_suppress_terminal = true" in helper
    assert "s_uplink_recovery_after_cleanup = true" in helper
    assert helper.count("abort_uplink_ota(") == 2

    begin = transport[
        transport.index("bool badge_usb_transport_handle_uplink_ota_begin(") :
        transport.index("void badge_usb_transport_snapshot(")
    ]
    assert 'deliver_uplink_receipt(&result, "ready_receipt_failed")' in begin
    ready_success = begin[begin.index("badge_usb_uplink_ota_flow_begin_result") :]
    assert "emit_uplink_ota_result(" not in ready_success
    assert "badge_usb_transport_drain(" not in ready_success

    consume = transport[
        transport.index("static badge_usb_uplink_action_t consume_uplink_bytes(") :
        transport.index("static void uplink_retry_backoff(")
    ]
    assert 'deliver_uplink_receipt(&result, "credit_receipt_failed")' in consume
    receipt = consume[consume.index("if (action == BADGE_USB_UPLINK_ACTION_WAIT_RECEIPT)") :]
    assert "emit_uplink_ota_result(" not in receipt
    assert "badge_usb_transport_drain(" not in receipt


def test_badge_usb_uplink_ambiguous_receipt_cleanup_suppresses_terminal_until_recovery():
    transport = _source("esp32", "uplink", "main", "core", "badge_usb_transport.c")
    cleanup_start = transport.index(
        "static badge_usb_uplink_action_t complete_uplink_abort("
    )
    abort_start = transport.index(
        "static badge_usb_uplink_action_t abort_uplink_ota(", cleanup_start
    )
    cleanup = transport[
        cleanup_start : abort_start
    ]
    suppress = cleanup.index("if (s_uplink_suppress_terminal)")
    consume_terminal = cleanup.index(
        "badge_usb_uplink_ota_flow_take_terminal(", suppress
    )
    cached_emit = cleanup.index(
        "badge_usb_uplink_ota_render_result(", consume_terminal
    )
    assert suppress < consume_terminal < cached_emit
    assert "s_uplink_terminal_pending = false" in cleanup[suppress:cached_emit]
    assert "s_uplink_terminal_length = 0U" in cleanup[suppress:cached_emit]

    abort = transport[abort_start : transport.index("typedef struct {", abort_start)]
    assert "BADGE_USB_UPLINK_RETRY_CLEANUP" in abort
    assert "BADGE_USB_UPLINK_ACTION_RECOVERY_RESTART" in abort


def test_usb_rollback_unexpected_return_clears_expected_reboot_and_fails():
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")
    runtime = _source("esp32", "uplink", "main", "core", "badge_runtime.c")
    runtime_header = _source("esp32", "uplink", "main", "core", "badge_runtime.h")

    rollback = serial[
        serial.index('} else if (strcmp(cmd, "rollback") == 0) {') :
        serial.index('} else if (strcmp(cmd, "bootloader") == 0', serial.index(
            '} else if (strcmp(cmd, "rollback") == 0) {'
        ))
    ]
    arm = rollback.index("badge_runtime_arm_expected_reboot(")
    legacy_target = rollback.index(
        "BADGE_RUNTIME_EXPECTED_REBOOT_TARGET_LEGACY_V078_ROLLBACK",
        arm,
    )
    owned = rollback.index(
        "BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_OWNED", legacy_target
    )
    call = rollback.index("esp_ota_mark_app_invalid_rollback_and_reboot()", owned)
    release = rollback.index(
        "badge_runtime_release_expected_reboot(&rollback_lease)", call
    )
    failure = rollback.index('send_control_error("rollback_failed")', release)
    assert arm < legacy_target < owned < call < release < failure

    assert "bool badge_runtime_release_expected_reboot(" in runtime_header
    release_fn_start = runtime.index(
        "bool badge_runtime_release_expected_reboot("
    )
    release_fn = runtime[
        release_fn_start :
        runtime.index(
            "void badge_runtime_arm_usb_recovery_once(void)",
            release_fn_start,
        )
    ]
    clear_lock = release_fn.index(
        "portENTER_CRITICAL(&s_runtime_health_lock)"
    )
    verify_owner = release_fn.index(
        "badge_runtime_expected_reboot_arm_is_owned(", clear_lock
    )
    clear_magic = release_fn.index(
        "&g_fof_badge_rtc_state.expected_reboot_magic", verify_owner
    )
    clear_unlock = release_fn.index(
        "portEXIT_CRITICAL(&s_runtime_health_lock)", clear_magic
    )
    clear_gate = release_fn.index("if (!marker_cleared)", clear_unlock)
    erase_reason = release_fn.index(
        "nvs_erase_key_value(BADGE_RUNTIME_NVS_EXPECTED_REASON);",
        clear_gate,
    )
    release_lock = release_fn.index(
        "portENTER_CRITICAL(&s_runtime_health_lock)", erase_reason
    )
    recheck_owner = release_fn.index(
        "badge_runtime_expected_reboot_arm_is_owned(", release_lock
    )
    release_owner = release_fn.index(
        "badge_runtime_expected_reboot_arm_release(", recheck_owner
    )
    release_unlock = release_fn.index(
        "portEXIT_CRITICAL(&s_runtime_health_lock)", release_owner
    )
    success_gate = release_fn.index("if (!released)", release_unlock)
    clear_reason = release_fn.index(
        "s_last_expected_reboot_reason[0] = '\\0';", success_gate
    )
    assert (
        clear_lock
        < verify_owner
        < clear_magic
        < clear_unlock
        < clear_gate
        < erase_reason
        < release_lock
        < recheck_owner
        < release_owner
        < release_unlock
        < success_gate
        < clear_reason
    )
    assert "__ATOMIC_RELEASE" in release_fn[clear_magic:clear_unlock]


def test_canary_update_maintenance_runtime_is_journaled_and_generation_bound():
    header = _source("esp32", "uplink", "main", "core", "badge_runtime.h")
    runtime = _source("esp32", "uplink", "main", "core", "badge_runtime.c")
    cmake = _source("esp32", "uplink", "main", "CMakeLists.txt")

    for interface in (
        "badge_runtime_prepare_update",
        "badge_runtime_update_maintenance_active",
        "badge_runtime_update_session_matches",
        "badge_runtime_update_keepalive",
        "badge_runtime_update_inactivity_due",
        "badge_runtime_clear_update_maintenance",
        "badge_runtime_update_marker_snapshot",
    ):
        assert interface in header
        assert interface in runtime
    marker_field = runtime.index(
        "badge_update_maintenance_marker_t update_maintenance_marker;"
    )
    rtc_state_start = runtime.rindex("typedef struct {", 0, marker_field)
    rtc_state_end = runtime.index(
        "} badge_runtime_rtc_state_t;", marker_field
    )
    rtc_state = runtime[rtc_state_start:rtc_state_end]
    assert (
        "badge_update_maintenance_marker_t update_maintenance_marker;"
    ) in rtc_state
    assert (
        "_Static_assert(\n"
        "    offsetof(badge_runtime_rtc_state_t, update_maintenance_marker) ==\n"
        "        BADGE_RUNTIME_RTC_CANARY_MARKER_OFFSET,\n"
        '    "RTC update marker ABI moved");'
    ) in runtime
    assert (
        "extern RTC_NOINIT_ATTR badge_runtime_rtc_state_t "
        "g_fof_badge_rtc_state;"
    ) in runtime
    assert runtime.count("RTC_NOINIT_ATTR") == 1
    assert "RTC_NOINIT_ATTR static badge_update_maintenance_marker_t" not in runtime
    assert "badge_update_maintenance_boot_decide(" in runtime
    assert "badge_update_maintenance_marker_activate(" in runtime

    arm_start = runtime.index("badge_runtime_arm_expected_reboot(")
    arm = runtime[
        arm_start :
        runtime.index("void badge_runtime_set_expected_reboot_hook(", arm_start)
    ]
    hook = arm.index("hook(generation)")
    bind = arm.index("badge_update_maintenance_marker_arm_reboot(")
    publish_generation = arm.index(
        "g_fof_badge_rtc_state.expected_reboot_generation = generation",
        bind,
    )
    publish_magic = arm.index(
        "BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC",
        publish_generation,
    )
    assert hook < bind < publish_generation < publish_magic

    noncanary_excludes = cmake[
        cmake.index("if(NOT FOF_DC34_GAME_CANARY)") :
        cmake.index("endif()", cmake.index("if(NOT FOF_DC34_GAME_CANARY)"))
    ]
    assert (
        '"../../shared/badge_update_maintenance_policy.c"' in
        noncanary_excludes
    )
    admission_header = _source(
        "esp32", "shared", "badge_update_admission_policy.h"
    )
    admission_policy = _source(
        "esp32", "uplink", "main", "production",
        "badge_update_admission_policy.c"
    )
    serial = _source(
        "esp32", "uplink", "main", "core", "serial_config.c"
    )
    native_build = _source("esp32", "platformio.ini")
    assert '#include "badge_update_maintenance_policy.h"' in serial
    assert '#include "badge_update_admission_policy.h"' not in serial
    assert "badge_update_ota_begin_admission_t" in admission_header
    assert (
        "badge_update_uplink_ota_begin_admission_decide(" in
        admission_policy
    )
    assert (
        "badge_update_scanner_stage_begin_admission_decide(" in
        admission_policy
    )
    assert "+<shared/badge_update_maintenance_policy.c>" in native_build
    assert "+<shared/badge_update_admission_policy.c>" not in native_build
    register = cmake[cmake.index("idf_component_register(") :]
    assert '"../../shared"' in register
    assert (
        'target_sources(${COMPONENT_LIB} PRIVATE '
        '"production/badge_update_admission_policy.c")'
    ) in register


def test_canary_prepare_update_has_background_reboot_safe_liveness():
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")
    transport = _source(
        "esp32", "uplink", "main", "core", "badge_usb_transport.c"
    )
    store_header = _source("esp32", "uplink", "main", "network", "fw_store.h")
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")

    assert "FW_UPDATE_PREEMPT_REBOOT_SAFE" in store_header
    preempt = store[
        store.index("fw_update_preempt_result_t "
                    "fw_store_request_update_preemption(void)") :
        store.index("bool fw_store_game_radio_must_yield(void)")
    ]
    assert "BADGE_UPDATE_PREPARE_REBOOT_SAFE" in preempt
    assert "FW_UPDATE_PREEMPT_REBOOT_SAFE" in preempt
    assert (
        "if (!badge_con_vhci_request_quiescence" not in
        preempt[:preempt.index("auto_coordinator_lock_ticks(0)")]
    )

    assert "handle_update_mode_command" in serial
    for phase in (
        '\\"phase\\":\\"rebooting\\"',
        '\\"phase\\":\\"waiting_for_owner\\"',
        '\\"phase\\":\\"busy\\"',
    ):
        assert phase in serial
    assert "serial_config_poll_update_preparation" in serial
    assert "serial_config_poll_update_preparation" in transport
    poll = serial[
        serial.index("void serial_config_poll_update_preparation(") :
        serial.index("\n}", serial.index(
            "void serial_config_poll_update_preparation(")) + 2
    ]
    assert "badge_usb_transport_emit(" in poll
    assert "badge_usb_transport_drain(" in poll
    assert "badge_usb_recovery_restart(" in poll


def test_normal_mode_scanner_begin_rejects_before_manifest_or_mutation():
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")
    upload = serial[
        serial.index("static void handle_fw_upload_begin(") :
        serial.index("static void handle_scanner_display_control(")
    ]
    gate = upload.index("badge_runtime_update_maintenance_active()")
    manifest_parse = upload.index("cJSON_GetObjectItemCaseSensitive(")
    mutate = upload.index("fw_store_serial_upload_begin(")
    assert gate < manifest_parse < mutate
    assert "FOF_FW_UPLOAD:" in upload[:manifest_parse]
    assert "update_maintenance_required" in upload[:manifest_parse]


def test_normal_mode_uplink_begin_rejects_before_flow_or_adapter_mutation():
    transport = _source(
        "esp32", "uplink", "main", "core", "badge_usb_transport.c"
    )
    uplink = transport[
        transport.index("bool badge_usb_transport_handle_uplink_ota_begin(") :
        transport.index("void badge_usb_transport_snapshot(")
    ]
    gate = uplink.index("badge_runtime_update_maintenance_active()")
    rejection = uplink.index(
        "badge_usb_uplink_ota_maintenance_required_result("
    )
    flow_reset = uplink.index("badge_usb_uplink_ota_flow_init(")
    adapter = uplink.index("uplink_usb_ota_begin(")
    assert gate < rejection < flow_reset < adapter


def test_canary_uplink_rtc_summary_precommits_before_finish_and_clears_on_noncommit():
    transport = _source(
        "esp32", "uplink", "main", "core", "badge_usb_transport.c"
    )
    runtime_header = _source(
        "esp32", "uplink", "main", "core", "badge_runtime.h"
    )
    runtime = _source(
        "esp32", "uplink", "main", "core", "badge_runtime.c"
    )
    policy = _source(
        "esp32", "shared", "badge_update_maintenance_policy.c"
    )

    finish_start = transport.index(
        "static badge_usb_uplink_action_t finish_uplink_ota(void)"
    )
    finish = transport[
        finish_start :
        transport.index(
            "static badge_usb_uplink_action_t consume_uplink_bytes(",
            finish_start,
        )
    ]
    snapshot = finish.index("uplink_usb_ota_get_status(&update_summary)")
    verifying = finish.index("UPLINK_USB_OTA_VERIFYING", snapshot)
    precommit = finish.index(
        "badge_runtime_update_commit_uplink(", verifying
    )
    ota_finish = finish.index(
        "bool accepted = uplink_usb_ota_finish(", precommit
    )
    noncommit = finish.index("if ((!accepted ||", ota_finish)
    committed_phase = finish.index(
        "result.phase != UPLINK_USB_OTA_PHASE_COMMITTED", noncommit
    )
    prepared = finish.index("update_summary_prepared &&", committed_phase)
    clear = finish.index(
        "badge_runtime_update_clear_uplink_commit()", prepared
    )
    rollback_failure = finish.index(
        '"maintenance_summary_rollback_failed"', clear
    )
    assert (
        snapshot < verifying < precommit < ota_finish < noncommit <
        committed_phase < prepared < clear < rollback_failure
    )
    precommit_call = finish[precommit:ota_finish]
    for summary_field in (
        "update_summary.target_version",
        "update_summary.target_sha256",
        "update_summary.total",
        "update_summary.partition",
    ):
        assert summary_field in precommit_call
    assert finish.count("badge_runtime_update_clear_uplink_commit()") == 1

    assert (
        "bool badge_runtime_update_commit_uplink(" in runtime_header
    )
    assert (
        "bool badge_runtime_update_clear_uplink_commit(void);"
        in runtime_header
    )
    runtime_commit_start = runtime.index(
        "bool badge_runtime_update_commit_uplink("
    )
    runtime_clear_start = runtime.index(
        "bool badge_runtime_update_clear_uplink_commit(void)",
        runtime_commit_start,
    )
    runtime_commit = runtime[runtime_commit_start:runtime_clear_start]
    assert (
        runtime_commit.index("portENTER_CRITICAL(&s_runtime_health_lock)") <
        runtime_commit.index(
            "badge_update_maintenance_marker_commit_uplink("
        ) <
        runtime_commit.index("portEXIT_CRITICAL(&s_runtime_health_lock)")
    )
    runtime_clear = runtime[
        runtime_clear_start :
        runtime.index("#endif", runtime_clear_start)
    ]
    assert (
        runtime_clear.index("portENTER_CRITICAL(&s_runtime_health_lock)") <
        runtime_clear.index(
            "badge_update_maintenance_marker_clear_uplink("
        ) <
        runtime_clear.index("portEXIT_CRITICAL(&s_runtime_health_lock)")
    )

    policy_clear_start = policy.index(
        "bool badge_update_maintenance_marker_clear_uplink("
    )
    policy_clear = policy[
        policy_clear_start :
        policy.index(
            "badge_update_prepare_action_t badge_update_prepare_decide(",
            policy_clear_start,
        )
    ]
    for cleared_field in (
        "marker->uplink_committed = 0U;",
        "marker->uplink_version",
        "marker->uplink_sha256",
        "marker->uplink_partition",
        "marker->uplink_size = 0U;",
        "marker->uplink_received = 0U;",
    ):
        assert cleared_field in policy_clear
    assert policy_clear.index("marker->uplink_received = 0U;") < (
        policy_clear.index("badge_update_maintenance_marker_seal(marker)")
    )


def test_canary_maintenance_boot_never_initializes_game_ble_or_network():
    main = _source("esp32", "uplink", "main", "main.c")
    assert "badge_update_maintenance" in main
    assert "esp_bt_controller_mem_release(ESP_BT_MODE_BLE)" in main
    maintenance_comment = main.index(
        "/* Update maintenance is a distinct, radio-free boot."
    )
    branch_start = main.index(
        "if (badge_update_maintenance)", maintenance_comment
    )
    branch = main[
        branch_start :
        main.index("/* ── 3. Create default event loop", branch_start)
    ]
    assert branch.lstrip().startswith("if (badge_update_maintenance) {")
    for required in (
        "badge_send_scanner_ota_abort_sentinel()",
        "start_uart_rx_with_operation_gate()",
        "fw_store_start_auto_update_coordinator()",
        "fw_store_restore_auto_update_coordinator()",
        "xTaskNotifyGive(s_display_task_handle)",
        "while (1)",
    ):
        assert required in branch
    for forbidden in (
        "badge_con_vhci_",
        "esp_event_loop_create_default(",
        "gps_init(",
        "gps_start(",
        "wifi_sta_",
        "wifi_ap_",
        "badge_con_radio_runtime_poll(",
        "send_badge_scan_profiles(",
        'uart_rx_send_command("{\\"type\\":\\"ready\\"}")',
    ):
        assert forbidden not in branch


def test_canary_maintenance_status_exposes_exact_scanner_stage_identity():
    store_header = _source(
        "esp32", "uplink", "main", "network", "fw_store.h"
    )
    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")

    assert "fw_store_scanner_stage_status_t" in store_header
    assert "fw_store_scanner_stage_status_snapshot(" in store_header
    assert "fw_store_scanner_stage_status_snapshot(" in store
    helper_start = serial.index(
        "static void print_update_maintenance_status_json("
    )
    helper_end = serial.index("static bool emit_minimal_status(", helper_start)
    helper = serial[helper_start:helper_end]
    assert "fw_store_scanner_stage_status_snapshot(" in helper
    assert '\\"update_scanner\\":{\\"phase\\":' in helper
    for member in (
        '\\"session\\":',
        '\\"target\\":',
        '\\"sha256\\":',
        '\\"size\\":',
        '\\"slot_mask\\":',
        '\\"received\\":',
        '\\"generation\\":',
    ):
        assert member in helper
    for phase in ("idle", "receiving", "committed"):
        assert f'"{phase}"' in helper
    assert '"unknown"' in helper
    assert "if (!scanner_stage_valid)" in helper
    assert "memset(&scanner_stage, 0, sizeof(scanner_stage))" in helper


def test_canary_maintenance_status_exposes_compact_generation_bound_campaign():
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")
    helper_start = serial.index(
        "static void print_update_maintenance_status_json("
    )
    helper_end = serial.index("static bool emit_minimal_status(", helper_start)
    helper = serial[helper_start:helper_end]
    minimal = serial[
        helper_end :
        serial.index("static void send_startup_recovery_status_response")
    ]

    assert "fw_auto_update_status_t campaign" in helper
    assert "fw_store_get_auto_update_status(&campaign)" in helper
    assert '\\"update_campaign\\":{\\"generation\\":%lu' in helper
    for member in (
        '\\"target_slot_mask\\":%u',
        '\\"pending_mask\\":%u',
        '\\"worker_running\\":%s',
        '\\"readiness_probes\\":[%u,%u]',
        '\\"scanners\\":[',
        '\\"slot\\":%d',
        '\\"attempts\\":%u',
        '\\"state\\":',
    ):
        assert member in helper
    assert "FW_AUTO_UPDATE_SCANNER_COUNT" in helper
    assert (
        "campaign.state[scanner_id][0] ? campaign.state[scanner_id] : "
        '"idle"'
    ) in helper

    # The compact proof may take the maintenance response past the emergency
    # 2 KiB fallback. It must use PSRAM, not consume more internal heap/stack.
    assert "#define UPDATE_MAINTENANCE_STATUS_MAX_BYTES 4096" in serial
    assert "badge_runtime_update_maintenance_active()" in minimal
    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in minimal
    assert "UPDATE_MAINTENANCE_STATUS_MAX_BYTES" in minimal
    assert "include_update_campaign" in minimal


def test_canary_usb_status_exposes_consumed_expected_reboot_generation():
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")
    minimal = serial[
        serial.index("static bool emit_minimal_status(") :
        serial.index("static void send_startup_recovery_status_response")
    ]
    full = serial[
        serial.index("static void send_badge_status_response") :
        serial.index("static void send_control_ok")
    ]

    for serializer in (minimal, full):
        assert serializer.count(
            "badge_runtime_last_expected_reboot_generation()"
        ) == 1
        assert '\\"last_expected_reboot_generation\\":%lu' in serializer
    assert "FOF_DC34_GAME_CANARY" in minimal
    assert "FOF_DC34_GAME_CANARY" in full


def test_canary_full_and_minimal_status_share_one_live_metric_snapshot():
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")
    snapshot_start = serial.index(
        "static serial_live_metrics_t serial_live_metrics_snapshot("
    )
    snapshot_end = serial.index(
        "static void print_live_metrics_status(",
        snapshot_start,
    )
    snapshot = serial[snapshot_start:snapshot_end]
    printer_end = serial.index(
        "static bool emit_minimal_status(",
        snapshot_end,
    )
    printer = serial[snapshot_end:printer_end]
    minimal_end = serial.index(
        "static void send_startup_recovery_status_response",
        printer_end,
    )
    minimal = serial[printer_end:minimal_end]
    full_start = serial.index("static void send_badge_status_response(")
    full_end = serial.index("static void send_control_ok(", full_start)
    full = serial[full_start:full_end]

    for source in (
        "badge_runtime_main_stack_free()",
        "badge_runtime_display_stack_free()",
        "badge_runtime_usb_stack_free()",
        "badge_runtime_uart_ble_stack_free()",
        "badge_runtime_uart_wifi_stack_free()",
        "heap_caps_get_free_size(MALLOC_CAP_INTERNAL)",
        "heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)",
        "heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)",
        "uart_rx_detection_queue_capacity()",
    ):
        assert source in snapshot
        assert source not in minimal
        assert source not in full

    for field in (
        "stack_main_free",
        "stack_display_free",
        "stack_usb_free",
        "stack_uart_ble_free",
        "stack_uart_wifi_free",
        "heap_internal_free",
        "heap_internal_min_free",
        "heap_internal_largest",
        "detection_queue_capacity",
    ):
        assert f'\\"{field}\\":%lu' in printer

    assert "const serial_live_metrics_t *live_metrics" in minimal
    assert "print_live_metrics_status(live_metrics);" in minimal
    assert "serial_live_metrics_snapshot()" in full
    assert full.index("serial_live_metrics_snapshot()") < full.index(
        "badge_runtime_update_maintenance_active()"
    )
    assert "print_live_metrics_status(&live_metrics);" in full
    assert "heap_caps_malloc" not in snapshot
    assert "heap_caps_malloc" not in printer


def test_canary_update_finish_and_abort_are_bounded_fail_busy_restarts():
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")
    main = _source("esp32", "uplink", "main", "main.c")
    store_header = _source(
        "esp32", "uplink", "main", "network", "fw_store.h"
    )
    handler_start = serial.index("static void handle_update_mode_command(")
    handler_end = serial.index(
        "void serial_config_poll_update_preparation(", handler_start
    )
    handler = serial[handler_start:handler_end]
    lifecycle = serial[
        serial.index("static void render_update_mode_conflict(") :
        handler_end
    ]

    assert "update lifecycle command unavailable" not in handler
    assert "BADGE_USB_CONTROL_SCHEMA_FINISH_UPDATE" in handler
    assert "BADGE_USB_CONTROL_SCHEMA_ABORT_UPDATE" in handler
    assert "badge_runtime_pending_verify()" in handler
    assert "fw_store_campaign_completion_sample()" in handler
    assert "FW_CAMPAIGN_COMPLETION_SUCCESS" in handler
    assert handler.count(
        "badge_usb_recovery_prepare_firmware_restart("
    ) == 2
    assert handler.count(
        "BADGE_USB_FIRMWARE_RESTART_PREPARE_OWNED"
    ) == 2
    assert "fw_store_try_reserve_recovery_restart()" not in handler
    for token in (
        '\\"phase\\":\\"finishing\\"',
        '\\"phase\\":\\"aborting\\"',
        '\\"error\\":\\"success_gates_pending\\"',
        '\\"error\\":\\"firmware_operation_active\\"',
    ):
        assert token in lifecycle
    clear = handler.index("badge_runtime_clear_update_maintenance(")
    restart_latch = handler.index("s_update_restart_reason", clear)
    assert clear < restart_latch

    dispatch_start = serial.index("bool serial_config_dispatch_line(")
    dispatch = serial[
        dispatch_start :
        serial.index(
            "static void print_json_escaped_string(", dispatch_start
        )
    ]
    assert "s_update_restart_reason" in dispatch
    assert "badge_usb_transport_drain(" in dispatch
    assert "if (s_update_restart_owned)" in dispatch
    assert (
        "badge_usb_recovery_restart_with_owned_lease(\n"
        "                BADGE_USB_RESET_APP,\n"
        "                restart_reason,\n"
        "                &s_update_restart_lease);"
    ) in dispatch
    assert "badge_usb_recovery_restart(" in dispatch

    supervisor = main[
        main.index("if (badge_update_maintenance)") :
        main.index("/* ── 3. Create default event loop")
    ]
    assert "maintenance_entry_scanner_generation" in supervisor
    assert "fw_store_scanner_stage_status_snapshot(" in supervisor
    assert "fw_store_campaign_terminal_exit_allowed(" in supervisor
    assert "fw_store_campaign_completion_sample()" in supervisor
    assert "FW_CAMPAIGN_COMPLETION_TERMINAL_FAILURE" in store_header
    terminal_helper = main[
        main.index(
            "static bool "
            "badge_update_terminal_failure_restart("
        ) :
        main.index("#endif", main.index(
            "static bool "
            "badge_update_terminal_failure_restart("
        ))
    ]
    clear = terminal_helper.index(
        "badge_runtime_clear_update_maintenance("
    )
    restart = terminal_helper.index(
        "badge_usb_recovery_restart_with_owned_lease(", clear
    )
    assert "badge_usb_recovery_prepare_firmware_restart(" in terminal_helper
    assert (
        terminal_helper.index(
            "BADGE_USB_FIRMWARE_RESTART_PREPARE_OWNED"
        )
        < clear
        < restart
    )


def test_expected_reboot_arm_lease_stays_owned_until_matching_release():
    header = _source("esp32", "uplink", "main", "core", "badge_runtime.h")
    runtime = _source("esp32", "uplink", "main", "core", "badge_runtime.c")

    assert "BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_OWNED" in header
    assert "BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_BUSY" in header
    assert "badge_runtime_expected_reboot_lease_t *out_lease" in header
    assert "bool badge_runtime_expected_reboot_lease_is_owned(" in header
    assert "bool badge_runtime_release_expected_reboot(" in header
    assert "void badge_runtime_clear_expected_reboot(void);" not in header

    arm_signature = runtime.index("badge_runtime_arm_expected_reboot(")
    arm_start = runtime.rindex(
        "badge_runtime_expected_reboot_arm_result_t", 0, arm_signature
    )
    arm = runtime[
        arm_start :
        runtime.index("void badge_runtime_set_expected_reboot_hook(", arm_start)
    ]
    busy = arm.index("BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_BUSY")
    cache_boot = arm.index("boot_rtc_transition_locked()", busy)
    invalidate = arm.index(
        "&g_fof_badge_rtc_state.expected_reboot_magic", cache_boot
    )
    hook = arm.index("hook(generation)", invalidate)
    publish_owner = arm.index(
        "badge_runtime_expected_reboot_arm_publish(", hook
    )
    publish_generation = arm.index(
        "g_fof_badge_rtc_state.expected_reboot_generation = generation",
        publish_owner,
    )
    publish_magic = arm.index(
        "BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC", publish_generation
    )
    owned_result = arm.index(
        "BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_OWNED",
        publish_magic,
    )
    assert (
        busy
        < cache_boot
        < invalidate
        < hook
        < publish_owner
        < publish_generation
        < publish_magic
        < owned_result
    )
    assert "badge_runtime_expected_reboot_arm_release(" not in arm

    release_start = runtime.index(
        "bool badge_runtime_release_expected_reboot("
    )
    release = runtime[
        release_start :
        runtime.index("void badge_runtime_arm_usb_recovery_once(", release_start)
    ]
    verify_owner = release.index(
        "badge_runtime_expected_reboot_arm_is_owned("
    )
    clear_magic = release.index(
        "g_fof_badge_rtc_state.expected_reboot_magic", verify_owner
    )
    release_owner = release.index(
        "badge_runtime_expected_reboot_arm_release(", clear_magic
    )
    assert verify_owner < clear_magic < release_owner


def test_every_badge_rollback_publishes_v078_compatible_owned_token_first():
    main = _source("esp32", "uplink", "main", "main.c")
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")
    rollback_api = "esp_ota_mark_app_invalid_rollback_and_reboot()"
    target = (
        "BADGE_RUNTIME_EXPECTED_REBOOT_TARGET_LEGACY_V078_ROLLBACK"
    )
    owned = "BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_OWNED"
    prepared_owned = "BADGE_USB_FIRMWARE_RESTART_PREPARE_OWNED"

    early = main[
        main.index("static void rollback_check_at_boot(void)") :
        main.index("static void rollback_mark_valid")
    ]
    assert early.index(target) < early.index(owned) < early.index(rollback_api)

    automatic_start = main.index(
        "static bool badge_automatic_restart_when_firmware_idle"
    )
    automatic = main[
        automatic_start:main.index("static void display_task", automatic_start)
    ]
    prepare = automatic.index(
        "badge_usb_recovery_prepare_firmware_restart("
    )
    execute = automatic.index(
        "rollback_and_reboot_with_owned_lease(", prepare
    )
    assert (
        automatic.index(target)
        < prepare
        < automatic.index(prepared_owned, prepare)
        < execute
    )

    executor_start = main.index(
        "static _Noreturn void rollback_and_reboot_with_owned_lease("
    )
    executor = main[
        executor_start :
        main.index(
            "static bool badge_automatic_restart_when_firmware_idle",
            executor_start,
        )
    ]
    prove = executor.index(
        "badge_runtime_expected_reboot_lease_is_owned("
    )
    assert prove < executor.index(rollback_api, prove)

    health = main[
        main.index("static bool badge_update_health_rollback(") :
        main.index("static bool badge_update_inactivity_restart(")
    ]
    assert (
        health.index(target)
        < health.index(prepared_owned)
        < health.index(rollback_api)
    )

    usb = serial[
        serial.index('} else if (strcmp(cmd, "rollback") == 0) {') :
        serial.index('} else if (strcmp(cmd, "bootloader") == 0')
    ]
    assert usb.index(target) < usb.index(owned) < usb.index(rollback_api)
    returned = usb.index(rollback_api)
    release = usb.index(
        "badge_runtime_release_expected_reboot(", returned
    )
    failure = usb.index('send_control_error("rollback_failed")', release)
    assert returned < release < failure


def test_button_usb_and_ota_restart_executor_requires_owned_lease():
    recovery = _source(
        "esp32", "uplink", "main", "core", "badge_usb_recovery.c"
    )
    buttons = _source(
        "esp32", "uplink", "main", "hw", "display_st7735.c"
    )
    serial = _source("esp32", "uplink", "main", "core", "serial_config.c")
    transport = _source(
        "esp32", "uplink", "main", "core", "badge_usb_transport.c"
    )

    arm = recovery.index("badge_runtime_arm_expected_reboot(")
    owned = recovery.index(
        "BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_OWNED", arm
    )
    prove = recovery.index(
        "badge_runtime_expected_reboot_lease_is_owned(", owned
    )
    restart = recovery.index("esp_restart();", prove)
    assert arm < owned < prove < restart

    restart_helper = buttons[
        buttons.index("static void badge_button_restart_or_resume") :
        buttons.index("static void badge_button_task")
    ]
    button_task = buttons[
        buttons.index("static void badge_button_task") :
        buttons.index("static bool badge_buttons_start")
    ]
    assert restart_helper.count("badge_usb_recovery_restart(") == 1
    assert "atomic_store(&s_usb_recovery_prompt_active, false)" in \
        restart_helper
    assert button_task.count("badge_usb_recovery_restart(") == 0
    assert button_task.count("badge_button_restart_or_resume(") == 2
    assert "badge_usb_recovery_target(" in button_task
    assert "bool confirmation_handled = false;" in button_task
    assert "if (!confirmation_handled)" in button_task
    assert "esp_restart();" not in buttons
    assert '"usb_bootloader"' in serial
    assert "BADGE_USB_RESET_ROM" in serial
    restart_app = transport[
        transport.index("static bool restart_app(") :
        transport.index("static badge_usb_uplink_action_t finish_uplink_ota(")
    ]
    assert "badge_usb_recovery_restart(" in restart_app
    assert "badge_runtime_arm_expected_reboot(" not in restart_app
    badge_restart = restart_app[
        restart_app.index("#ifdef FOF_BADGE_VARIANT") :
        restart_app.index("#else")
    ]
    assert "esp_restart();" not in badge_restart
    assert "return accepted;" in badge_restart
