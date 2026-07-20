from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
UPLINK_MAIN = REPO_ROOT / "esp32" / "uplink" / "main"


def _source(*parts: str) -> str:
    return (REPO_ROOT.joinpath(*parts)).read_text()


def test_scanner_status_is_published_and_read_as_complete_mutex_snapshots():
    header = _source("esp32", "uplink", "main", "comms", "uart_rx.h")
    source = _source("esp32", "uplink", "main", "comms", "uart_rx.c")

    assert (
        "bool uart_rx_get_scanner_info_snapshot(int scanner_id, "
        "scanner_info_t *out);"
    ) in header
    assert "const scanner_info_t *uart_rx_get_ble_scanner_info" not in header
    assert "const scanner_info_t *uart_rx_get_wifi_scanner_info" not in header
    assert "s_scanner_info_mutex" in source
    assert "xSemaphoreCreateMutexStatic" in source
    assert "scanner_info_update_begin" in source
    assert "scanner_info_update_commit" in source
    assert "*out = *scanner_info_for_slot_unlocked(scanner_id)" in source


def test_no_uplink_consumer_keeps_a_pointer_to_mutable_scanner_status():
    legacy_getters = (
        "uart_rx_get_ble_scanner_info",
        "uart_rx_get_wifi_scanner_info",
    )
    for path in UPLINK_MAIN.rglob("*"):
        if path.suffix not in {".c", ".h"}:
            continue
        source = path.read_text()
        for getter in legacy_getters:
            assert getter not in source, f"{path} still uses unsafe {getter}"

    store = _source("esp32", "uplink", "main", "network", "fw_store.c")
    assert store.count("uart_rx_get_scanner_info_snapshot(") >= 4
