import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
HTTP_STATUS = REPO_ROOT / "esp32" / "uplink" / "main" / "network" / "http_status.c"


def _source() -> str:
    return HTTP_STATUS.read_text()


def test_con_crud_http_status_is_read_only_bounded_and_identifier_free():
    source = _source()
    handler = source[
        source.index("static esp_err_t badge_status_json_handler") :
        source.index("static esp_err_t badge_control_post_handler")
    ]

    assert "#if defined(FOF_DC34_GAME_CANARY)" in handler
    assert handler.count("badge_con_runtime_snapshot(") == 1
    for field in (
        r'\"game_seed\":',
        r'\"game_state\":',
        r'\"game_active\":',
        r'\"game_shield\":',
    ):
        assert field in handler
    game_at = handler.index(r'\"game_seed\":')
    game_slice = handler[game_at - 600 : game_at + 600]
    assert "badge_con_runtime_set_factory_seed" not in handler
    assert "peer" not in game_slice
    assert "session" not in game_slice
    assert "hardware_id" not in game_slice


def test_badge_scanner_status_never_streams_truncated_printf_output():
    source = _source()
    assert "static esp_err_t scanner_status_sendf" in source
    scanner = source[
        source.index("static void badge_status_chunk_scanner") :
        source.index("static const char *time_source_name")
    ]
    helper = source[
        source.index("static esp_err_t scanner_status_sendf") :
        source.index("static void badge_status_chunk_scanner")
    ]

    size_match = re.search(r"SCANNER_STATUS_BUF_LEN\s*=\s*(\d+)", scanner)
    assert size_match, "scanner status scratch buffer must have an explicit bound"
    assert int(size_match.group(1)) >= 4096

    # The helper must inspect vsnprintf's required length and grow the
    # PSRAM-backed output before sending. HTTPD_RESP_USE_STRLEN on a truncated
    # scratch buffer is precisely the release regression this guards against.
    assert "vsnprintf" in helper
    assert "written < 0" in helper
    assert "(size_t)written >= scratch_len" in helper
    assert "psram_alloc(required)" in helper
    assert "retry_written" in helper
    assert "httpd_resp_send_chunk(req, payload, payload_len)" in helper

    assert "snprintf(buf, SCANNER_STATUS_BUF_LEN" not in scanner
    assert scanner.count("scanner_status_sendf(") >= 5


def test_badge_scanner_status_exposes_normal_radio_quiescence():
    source = _source()
    scanner = source[
        source.index("static void badge_status_chunk_scanner") :
        source.index("static const char *time_source_name")
    ]

    assert r'\"ble_quiesced\":%s' in scanner
    assert "info->ble_quiesced ? \"true\" : \"false\"" in scanner
    assert r'\"wifi_quiesced\":%s' in scanner
    assert "info->wifi_quiesced ? \"true\" : \"false\"" in scanner


def test_badge_html_offers_usb_uart_firmware_guidance_not_dead_http_ota():
    source = _source()
    handler = source[
        source.index("static esp_err_t badge_html_handler") :
        source.index("static const httpd_uri_t uri_badge_html")
    ]

    assert "#ifdef FOF_BADGE_VARIANT" in handler
    badge_branch = handler[
        handler.index("#ifdef FOF_BADGE_VARIANT") :
        handler.index("#else", handler.index("#ifdef FOF_BADGE_VARIANT"))
    ]
    non_badge_branch = handler[
        handler.index("#else", handler.index("#ifdef FOF_BADGE_VARIANT")) :
        handler.index("#endif", handler.index("#ifdef FOF_BADGE_VARIANT"))
    ]

    assert "USB/UART only" in badge_branch
    assert "scanner firmware over UART" in badge_branch
    assert 'type=\\\"file\\\"' not in badge_branch
    assert "/api/ota" not in badge_branch
    assert "OTA Update" not in badge_branch

    # Production/non-badge nodes retain their existing HTTP OTA control.
    assert 'type=\\\"file\\\"' in non_badge_branch
    assert "OTA Update" in non_badge_branch

    ota_function = handler.index("async function ota()")
    ota_guard = handler.rfind("#ifndef FOF_BADGE_VARIANT", 0, ota_function)
    ota_script = handler[ota_guard : handler.index("#endif", ota_function)]
    assert "async function ota()" in ota_script
    assert "/api/ota" in ota_script


def test_badge_http_control_is_explicitly_read_only_and_usb_authoritative():
    source = _source()
    handler = source[
        source.index("static esp_err_t badge_control_post_handler") :
        source.index("static esp_err_t badge_html_handler")
    ]

    badge_guard = handler.index("#ifdef FOF_BADGE_VARIANT")
    non_badge_branch = handler.index("#else", badge_guard)
    body_allocation = handler.index("psram_calloc")
    assert badge_guard < non_badge_branch < body_allocation
    badge_branch = handler[badge_guard:non_badge_branch]
    assert 'httpd_resp_set_status(req, "403 Forbidden")' in badge_branch
    assert "badge_control_requires_usb" in badge_branch
    assert "httpd_req_recv" not in badge_branch
    assert "cJSON_Parse" not in badge_branch
    assert "esp_restart" not in badge_branch

    html = source[
        source.index("static esp_err_t badge_html_handler") :
        source.index("static const httpd_uri_t uri_badge_html")
    ]
    assert "Badge controls are USB-only" in html
    for mutation_script in (
        "async function ctl(cmd)",
        "async function setMode()",
        "async function setDebug()",
    ):
        function_at = html.index(mutation_script)
        guard_at = html.rfind("#ifndef FOF_BADGE_VARIANT", 0, function_at)
        end_at = html.index("#endif", function_at)
        assert guard_at >= 0
        assert guard_at < function_at < end_at


def test_badge_build_compiles_out_every_other_http_mutation_surface():
    source = _source()

    setup_mutators = source[
        source.index("static esp_err_t setup_html_handler") :
        source.index("/* ── OTA Firmware Update Handler")
    ]
    setup_guard = source.rfind(
        "#ifndef FOF_BADGE_VARIANT",
        0,
        source.index("static esp_err_t setup_html_handler"),
    )
    setup_end = source.index(
        "#endif",
        source.index("static esp_err_t connect_post_handler"),
    )
    assert setup_guard >= 0
    assert setup_guard < source.index("static esp_err_t setup_html_handler")
    assert source.index("static esp_err_t connect_post_handler") < setup_end
    assert "fetch('/api/connect'" in setup_mutators

    registration = source[source.index("void http_status_init") :]
    non_badge_registration = registration.index("#ifndef FOF_BADGE_VARIANT")
    for route in (
        "uri_setup_html",
        "uri_scan_json",
        "uri_connect_post",
        "uri_cal_mode_start",
        "uri_cal_mode_stop",
    ):
        route_at = registration.index(
            f"httpd_register_uri_handler(server, &{route})"
        )
        guard_at = registration.rfind(
            "#ifndef FOF_BADGE_VARIANT", 0, route_at
        )
        end_at = registration.index("#endif", route_at)
        assert guard_at >= non_badge_registration
        assert guard_at < route_at < end_at

    badge_html = source[
        source.index("static esp_err_t badge_html_handler") :
        source.index("static const httpd_uri_t uri_badge_html")
    ]
    assert 'href=\\"/setup\\"' not in badge_html
    assert "Configure Wi-Fi/backend over USB" in badge_html

    status_html = source[
        source.index("static esp_err_t status_html_handler") :
        source.index("static esp_err_t status_json_handler")
    ]
    setup_link_at = status_html.index('href=\\"/setup\\"')
    guard_at = status_html.rfind(
        "#ifndef FOF_BADGE_VARIANT", 0, setup_link_at
    )
    end_at = status_html.index("#endif", setup_link_at)
    assert guard_at >= 0
    assert guard_at < setup_link_at < end_at
