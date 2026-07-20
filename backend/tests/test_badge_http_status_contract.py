import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
HTTP_STATUS = REPO_ROOT / "esp32" / "uplink" / "main" / "network" / "http_status.c"


def _source() -> str:
    return HTTP_STATUS.read_text()


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

    ota_script = handler[
        handler.index("#ifndef FOF_BADGE_VARIANT") :
        handler.index("#endif", handler.index("#ifndef FOF_BADGE_VARIANT"))
    ]
    assert "async function ota()" in ota_script
    assert "/api/ota" in ota_script
