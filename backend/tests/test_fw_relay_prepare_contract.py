import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
STORE = ROOT / "esp32/uplink/main/network/fw_store.c"
ADAPTER = ROOT / "esp32/uplink/main/network/fw_relay_prepare_adapter.c"
MANIFEST = ROOT / "esp32/uplink/main/network/fw_manifest_store.c"


def _function_body(source: str, signature: str, next_marker: str) -> str:
    start = source.index(signature)
    return source[start : source.index(next_marker, start)]


def test_relay_enters_the_single_preparation_adapter_before_scanner_io():
    source = STORE.read_text(encoding="utf-8")
    body = _function_body(
        source,
        "static bool fw_relay_stored_to_scanner(",
        "/* ── POST /api/fw/relay",
    )

    assert body.count("fw_relay_prepare_for_scanner(") == 1
    prepare_call = body.index("fw_relay_prepare_for_scanner(")
    prepared_gate_end = body.index(
        "\n    fw_store_info_t info = prepared.manifest;"
    )
    gate = body[prepare_call:prepared_gate_end]
    assert "if (prepare_result != FW_RELAY_PREPARED)" in gate
    assert "return false;" in gate
    for forbidden in (
        "fw_store_get_info(",
        "nvs_config_get_",
        "find_fw_partition(",
        "invalidate_fw_metadata(",
        "clear_fw_metadata(",
    ):
        assert forbidden not in body
    for scanner_io in (
        "uart_rx_get_scanner_identity_snapshot(",
        "capture_command_health(",
        "probe_scanner_command_ingress(",
        "badge_try_heal_command_tx_pin(",
        "uart_rx_send_command_to_scanner(",
        "uart_rx_send_command_to_scanner_checked(",
        "uart_rx_pause_scanner(",
        "uart_rx_resume_scanner(",
        "uart_write_bytes(",
        "uart_wait_tx_done(",
        "uart_read_bytes(",
        "relay_read_line_stateful(",
        "relay_wait_for_staged_or_nack(",
        "relay_poll_nack(",
        "relay_send_wire_abort_sentinel(",
    ):
        for match in re.finditer(re.escape(scanner_io), body):
            assert prepared_gate_end < match.start(), scanner_io
    assert len(re.findall(r"\breturn\s+", body[prepared_gate_end:])) == 1
    assert body.count("fw_relay_prepared_release(") == 0
    assert body.count("release_relay_prepared_retry(") == 2
    assert body.count("fw_store_operation_end(") == 0


def test_adapter_and_manifest_store_have_no_scanner_send_surface():
    adapter = ADAPTER.read_text(encoding="utf-8")
    manifest = MANIFEST.read_text(encoding="utf-8")
    combined = adapter + manifest

    assert "uart_write_bytes" not in combined
    assert "uart_rx_send_command" not in combined
    sequence = _function_body(
        adapter,
        "fw_relay_prepare_result_t fw_relay_prepare_for_scanner(",
        "bool fw_relay_prepared_release(",
    )
    ordered = (
        "token_acquire",
        "uart_lease_acquire",
        "read_committed",
        "out->generation != expected_generation",
        "partition_for_snapshot",
        "validate_image",
        "clear_if_current",
    )
    positions = [sequence.index(item) for item in ordered]
    assert positions == sorted(positions)

    clear = _function_body(
        manifest,
        "fw_manifest_clear_result_t fw_store_clear_if_current(",
        "#ifdef UNIT_TESTING",
    )
    assert clear.count("open(") == 1
    assert "commit(" not in clear
    assert clear.count("close(") == 1
    assert re.search(
        r"set_u32\([^;]+FW_MANIFEST_KEY_VALID[^;]+0U\)", clear, re.S
    )
    assert clear.count("set_u32(") == 1
    assert "FW_MANIFEST_KEY_SIZE" not in clear


def test_production_reader_and_release_keep_typed_ownership_semantics():
    store = STORE.read_text(encoding="utf-8")
    adapter = ADAPTER.read_text(encoding="utf-8")

    production_reader = _function_body(
        adapter,
        "static fw_store_read_result_t production_read_committed(",
        "static const esp_partition_t *production_partition_for_snapshot(",
    )
    assert "return fw_store_read_committed(out);" in production_reader
    assert "FW_STORE_READ_NO_MANIFEST" not in production_reader

    typed_reader = _function_body(
        store,
        "fw_store_read_result_t fw_store_read_committed(",
        "bool fw_store_get_info(",
    )
    assert "FW_STORE_READ_NO_MANIFEST" in typed_reader
    assert "FW_STORE_READ_ERROR" in typed_reader
    assert "FW_STORE_READ_COMMITTED" in typed_reader

    token_release = _function_body(
        adapter,
        "static bool production_token_release(",
        "static bool production_uart_lease_acquire(",
    )
    assert "return fw_store_operation_end(token);" in token_release
    prepared_release = _function_body(
        adapter,
        "bool fw_relay_prepared_release(",
        "#ifdef UNIT_TESTING",
    )
    assert re.search(
        r"if \(!hooks->token_release\(prepared->operation_token\)\)"
        r"\s*\{\s*return false;",
        prepared_release,
        re.S,
    )
