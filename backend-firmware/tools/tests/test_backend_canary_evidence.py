from __future__ import annotations

import json
import os
from pathlib import Path
import stat

import pytest

from tools.backend_canary_evidence import (
    CanaryEvidenceRecorder,
    EvidenceError,
    _create_ready_file,
    _serial_log_receipt,
    contains_secret_key,
    find_matching_detection,
    monitor_soak,
    normalize_history_timestamp_ms,
    validate_command_history,
    verify_soak,
    wait_for_command,
    wait_for_detection,
)


DEVICE_ID = "uplink_CB77A4"
COMMAND_ID = "0123456789abcdef0123456789abcdef"
CONTINUITY = {
    "schema": 1,
    "device_id": DEVICE_ID,
    "calibration_status": "trusted",
    "session_id": "0123456789ab",
    "applied_at": 1_785_500_000.25,
    "listener_model_present": True,
    "listener_model_schema": "rssi-ref-path-loss-v1",
    "listener_model_sha256": "a" * 64,
}


def scanner_fixture(slot: int, profile: str) -> dict:
    return {
        "slot": slot,
        "uart": "ble" if slot == 0 else "wifi",
        "firmware_target": "scanner-s3-combo-backend",
        "app_project": "fof_backend_scanner",
        "hardware_type": "seeed_xiao_esp32s3",
        "firmware_version": "0.1.0-backend",
        "mac": f"AA:BB:CC:DD:EE:0{slot + 1}",
        "boot_id": 100 + slot,
        "profile": profile,
        # This display-only field must never substitute for profile.
        "role": profile,
        "status_sequence": 20 + slot,
        "role_generation": 4,
        "role_acked": True,
        "command_ingress": True,
        "radio_healthy": True,
        "ble_healthy": slot == 0,
        "wifi_healthy": slot == 1,
        "ota_state": "idle",
        "rollback_state": "valid",
    }


def node_fixture(device_id: str = DEVICE_ID) -> dict:
    return {
        "device_id": device_id,
        "last_seen": 1_785_600_000,
        "online": True,
        "firmware_target": "uplink-s3-backend",
        "app_project": "fof_backend_uplink",
        "hardware_type": "seeed_xiao_esp32s3",
        "firmware_version": "0.1.0-backend",
        "hardware_mac": "A4:CF:12:CB:77:A4",
        "scanners": [
            scanner_fixture(0, "ble_primary"),
            scanner_fixture(1, "wifi_primary"),
        ],
        "led_state": "healthy",
        "upload_queue": {
            "depth_batches": 0,
            "capacity_batches": 512,
            "overflow_dropped_batches": 0,
            "quarantined_batches": 0,
        },
        "upload": {
            "ok": 12,
            "failed": 0,
            "retry_count": 0,
            "last_success_age_s": 1,
        },
        "health": {
            "clock_valid": True,
            "epoch_ms": 1_785_600_000_000,
            "ap_active": False,
            "config_generation": 2,
            "command_success_count": 1,
            "command_failure_count": 0,
            "uptime_ms": 90_000,
        },
        "total_batches": 55,
        "password": "must-not-be-written",
    }


def nodes_status_fixture(device_id: str = DEVICE_ID) -> dict:
    return {"count": 1, "nodes": [node_fixture(device_id)]}


def history_row(
    *,
    kind: str,
    timestamp: int = 1_785_600_000,
    timestamp_ms: int | None = None,
) -> dict:
    if kind == "drone":
        row = {
            "device_id": DEVICE_ID,
            "source": "wifi_beacon_rid",
            "drone_id": "RID-CANON-001",
            "manufacturer": "Lab Remote ID",
            "timestamp": timestamp,
        }
    else:
        row = {
            "device_id": DEVICE_ID,
            "source": "ble_fingerprint",
            "drone_id": "META-AA:BB:CC:DD:EE:02",
            "bssid": "AA:BB:CC:DD:EE:02",
            "manufacturer": "Meta Glasses",
            "ble_svc_uuids": "180f,FD5F",
            "timestamp": timestamp,
            "value_hex": "01020304",
        }
    if timestamp_ms is not None:
        row.pop("timestamp")
        row["observed_at_ms"] = timestamp_ms
    return row


def history_fixture(*rows: dict) -> dict:
    return {"count": len(rows), "total": len(rows), "detections": list(rows)}


class FakeBackend:
    def __init__(
        self,
        *,
        nodes_status: dict | None = None,
        history: dict | None = None,
        continuity: dict | None = None,
    ):
        self.nodes_status = nodes_status or nodes_status_fixture()
        self.history = history or history_fixture(
            history_row(kind="drone"), history_row(kind="meta")
        )
        self.continuity = continuity or dict(CONTINUITY)
        self.calls: list[tuple[str, int]] = []

    def __call__(self, url: str, *, timeout: int):
        self.calls.append((url, timeout))
        if url.endswith("/detections/nodes/status"):
            return self.nodes_status
        if "/detections/drones/history?hours=1&limit=2000" in url:
            return self.history
        if url.endswith(f"/detections/calibrate/continuity/{DEVICE_ID}"):
            return self.continuity
        raise AssertionError(f"unexpected URL: {url}")


def write_state(output: Path, *, continuity: dict | None = None) -> Path:
    canary_dir = output.parent.parent
    canary_dir.mkdir(parents=True, mode=0o700)
    canary_dir.chmod(0o700)
    state = canary_dir / "canary-state.json"
    state.write_text(json.dumps({
        "schema": 1,
        "captured_device_id": DEVICE_ID,
        "installed_capture": {"continuity": continuity or CONTINUITY},
        "boards": {
            "uplink": {"final_health": {
                "target": "uplink-s3-backend",
                "mac": "A4:CF:12:CB:77:A4",
                "boot_id": 9001,
                "device_id": DEVICE_ID,
                "config_state": "loaded",
                "config_generation": 2,
                "nvs_loaded": True,
                "nvs_erased": False,
                "auto_update_enabled": False,
                "uart0_started": True,
                "uart1_started": True,
                "coordinator_started": True,
                "network_state": "sta",
                "rollback_clear": True,
            }},
            "scanner0": {"final_health": {
                "target": "scanner-s3-combo-backend",
                "mac": "AA:BB:CC:DD:EE:01",
                "boot_id": 100,
                "role": "ble_primary",
                "command_ingress_boot_id": 100,
                "radio_healthy": True,
                "rollback_clear": True,
                "nvs_erased": False,
            }},
            "scanner1": {"final_health": {
                "target": "scanner-s3-combo-backend",
                "mac": "AA:BB:CC:DD:EE:02",
                "boot_id": 101,
                "role": "wifi_primary",
                "command_ingress_boot_id": 101,
                "radio_healthy": True,
                "rollback_clear": True,
                "nvs_erased": False,
            }},
        },
    }))
    state.chmod(0o600)
    return state


def make_recorder(tmp_path: Path, fetcher: FakeBackend) -> CanaryEvidenceRecorder:
    output = tmp_path / ".canary" / "evidence" / "canary.jsonl"
    write_state(output)
    return CanaryEvidenceRecorder(
        backend_base="http://127.0.0.1:8000",
        device_id=DEVICE_ID,
        output=output,
        fetch_json=fetcher,
        now=lambda: 1_785_600_000.0,
    )


def test_evidence_snapshot_is_canonical_redacted_and_bound_to_device(tmp_path):
    fetcher = FakeBackend()
    recorder = make_recorder(tmp_path, fetcher)

    returned = recorder.snapshot("drone")

    path = recorder.output
    raw = path.read_text()
    record = json.loads(raw)
    assert raw == json.dumps(
        record, sort_keys=True, separators=(",", ":"), allow_nan=False,
    ) + "\n"
    assert returned == record
    assert record["device_id"] == DEVICE_ID
    assert record["phase"] == "drone"
    assert record["backend"]["scanner_profiles"] == [
        "ble_primary", "wifi_primary",
    ]
    assert record["backend"]["uplink_boot_id"] == 9001
    assert record["backend"]["uplink_final_health"]["rollback_clear"] is True
    assert "unexpected_resets" not in record["backend"]
    assert "schema_errors" not in record["backend"]
    assert not contains_secret_key(record)
    assert stat.S_IMODE(path.parent.stat().st_mode) == 0o700
    assert stat.S_IMODE(path.stat().st_mode) == 0o600
    assert {timeout for _url, timeout in fetcher.calls} == {10}


def test_snapshot_rejects_wrong_device_id(tmp_path):
    recorder = make_recorder(
        tmp_path, FakeBackend(nodes_status=nodes_status_fixture("uplink_OTHER")),
    )
    with pytest.raises(EvidenceError, match="device"):
        recorder.snapshot("baseline")


@pytest.mark.parametrize(
    ("phase", "healthy", "led"),
    [
        ("scanner0-disconnected", (False, True), "uart_lost"),
        ("both-scanners-disconnected", (False, False), "fatal"),
    ],
)
def test_snapshot_accepts_only_exact_bounded_degraded_topology(
    tmp_path, phase, healthy, led,
):
    status = nodes_status_fixture()
    node = status["nodes"][0]
    node["led_state"] = led
    for scanner, expected in zip(node["scanners"], healthy):
        scanner["radio_healthy"] = expected
    record = make_recorder(
        tmp_path, FakeBackend(nodes_status=status),
    ).snapshot(phase)
    assert [
        item["radio_healthy"] for item in record["backend"]["scanners"]
    ] == list(healthy)
    assert record["backend"]["led_state"] == led


def test_snapshot_rejects_arbitrary_degraded_baseline_and_nonzero_queue(tmp_path):
    status = nodes_status_fixture()
    status["nodes"][0]["scanners"][0]["radio_healthy"] = False
    status["nodes"][0]["led_state"] = "uart_lost"
    with pytest.raises(EvidenceError):
        make_recorder(
            tmp_path / "degraded", FakeBackend(nodes_status=status),
        ).snapshot("baseline")

    queued = nodes_status_fixture()
    queued["nodes"][0]["upload_queue"]["depth_batches"] = 1
    with pytest.raises(EvidenceError, match="queue"):
        make_recorder(
            tmp_path / "queued", FakeBackend(nodes_status=queued),
        ).snapshot("baseline")

    with pytest.raises(EvidenceError, match="phase"):
        make_recorder(tmp_path / "unknown", FakeBackend()).snapshot(
            "network-recovery-copy"
        )


def test_snapshot_refuses_output_outside_ignored_canary_directory(tmp_path):
    output = tmp_path / "evidence" / "canary.jsonl"
    state = write_state(tmp_path / ".canary" / "evidence" / "unused.jsonl")
    recorder = CanaryEvidenceRecorder(
        backend_base="http://127.0.0.1:8000",
        device_id=DEVICE_ID,
        output=output,
        state_path=state,
        fetch_json=FakeBackend(),
        now=lambda: 1_785_600_000.0,
    )
    with pytest.raises(EvidenceError, match=".canary"):
        recorder.snapshot("baseline")


def test_snapshot_rejects_duplicate_scanner_profiles(tmp_path):
    status = nodes_status_fixture()
    status["nodes"][0]["scanners"][1]["profile"] = "ble_primary"
    recorder = make_recorder(tmp_path, FakeBackend(nodes_status=status))
    with pytest.raises(EvidenceError, match="profile"):
        recorder.snapshot("baseline")


@pytest.mark.parametrize(
    ("scope", "field", "value"),
    [
        ("uplink", "firmware_target", "uplink-s3-fof_badge"),
        ("uplink", "app_project", "fof_badge_uplink"),
        ("uplink", "hardware_type", "esp32-s3-devkitc-1"),
        ("uplink", "firmware_version", "0.67.2-badge-defcon34"),
        ("scanner", "firmware_target", "scanner-s3-combo-fof_badge"),
        ("scanner", "app_project", "fof_badge_scanner"),
        ("scanner", "hardware_type", "esp32-s3-devkitc-1"),
        ("scanner", "firmware_version", "0.67.2-badge-defcon34"),
    ],
)
def test_snapshot_rejects_non_backend_target_project_hardware_or_version(
    tmp_path, scope, field, value,
):
    status = nodes_status_fixture()
    target = status["nodes"][0]
    if scope == "scanner":
        target = target["scanners"][0]
    target[field] = value
    recorder = make_recorder(tmp_path, FakeBackend(nodes_status=status))
    with pytest.raises(EvidenceError, match="identity"):
        recorder.snapshot("baseline")


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("profile", None),
        ("role_generation", 0),
        ("role_acked", False),
        ("radio_healthy", False),
    ],
)
def test_display_role_never_substitutes_for_exact_scanner_health(
    tmp_path, field, value,
):
    status = nodes_status_fixture()
    scanner = status["nodes"][0]["scanners"][0]
    scanner[field] = value
    scanner["role"] = "ble_primary"
    recorder = make_recorder(tmp_path, FakeBackend(nodes_status=status))
    with pytest.raises(EvidenceError, match=field):
        recorder.snapshot("baseline")


@pytest.mark.parametrize(
    ("mutation", "message"),
    [
        ({"calibration_status": "untrusted"}, "continuity"),
        ({"session_id": "changed"}, "continuity"),
        ({"applied_at": None}, "continuity"),
        ({"listener_model_present": False}, "continuity"),
        ({"listener_model_schema": "changed"}, "continuity"),
        ({"listener_model_sha256": "b" * 64}, "continuity"),
    ],
)
def test_snapshot_rejects_changed_calibration_continuity(
    tmp_path, mutation, message,
):
    observed = {**CONTINUITY, **mutation}
    recorder = make_recorder(
        tmp_path, FakeBackend(continuity=observed),
    )
    with pytest.raises(EvidenceError, match=message):
        recorder.snapshot("baseline")


def test_snapshot_rejects_missing_calibration_continuity_field(tmp_path):
    observed = dict(CONTINUITY)
    observed.pop("session_id")
    recorder = make_recorder(tmp_path, FakeBackend(continuity=observed))
    with pytest.raises(EvidenceError, match="continuity"):
        recorder.snapshot("baseline")


def command_history_fixture(
    *,
    states=("ble_inv_begin", "ble_inv_progress", "ble_inv_end"),
    terminal_state="cancelled",
) -> dict:
    events = []
    for sequence, event_type in enumerate(states):
        event = {
            "sequence": sequence,
            "type": event_type,
            "request_id": COMMAND_ID,
        }
        if event_type == "ble_inv_begin":
            event.update(mode="passive_capture", target_mac=None)
        elif event_type == "ble_inv_progress":
            event["state"] = "scanning"
        elif event_type == "ble_inv_read":
            event.update(uuid="fd5f", value_hex="010203")
        elif event_type == "ble_inv_end":
            event.update(
                state=terminal_state, summary="finished", error=None,
                authentication_required=False, truncated=False,
            )
        events.append(event)
    return {
        "command_id": COMMAND_ID,
        "device_id": DEVICE_ID,
        "command_type": "ble_investigate",
        "state": "terminal",
        "next_sequence": len(events),
        "result_state": terminal_state,
        "terminal": True,
        "events": events,
    }


def test_command_evidence_requires_ordered_begin_and_exact_terminal():
    history = command_history_fixture()
    record = validate_command_history(
        history, device_id=DEVICE_ID, command_id=COMMAND_ID,
        terminal_state="cancelled",
    )
    assert record["terminal"] is True
    assert [event["sequence"] for event in record["events"]] == [0, 1, 2]


@pytest.mark.parametrize(
    ("mutate", "message"),
    [
        (lambda h: h["events"].pop(0), "begin"),
        (lambda h: h["events"][1].update(sequence=2), "sequence"),
        (lambda h: h["events"][1].update(sequence=0), "sequence"),
        (lambda h: h.update(result_state="failed"), "terminal"),
        (lambda h: h.update(device_id="uplink_OTHER"), "device"),
        (lambda h: h.update(command_id="f" * 32), "command"),
        (lambda h: h.update(terminal=False, state="delivered"), "terminal"),
    ],
)
def test_command_history_rejects_wrong_identity_order_or_terminal(mutate, message):
    history = command_history_fixture()
    mutate(history)
    with pytest.raises(EvidenceError, match=message):
        validate_command_history(
            history, device_id=DEVICE_ID, command_id=COMMAND_ID,
            terminal_state="cancelled",
        )


def test_command_history_redacts_raw_characteristic_values():
    history = command_history_fixture(
        states=("ble_inv_begin", "ble_inv_read", "ble_inv_end"),
        terminal_state="complete",
    )
    record = validate_command_history(
        history, device_id=DEVICE_ID, command_id=COMMAND_ID,
        terminal_state="complete",
    )
    assert "value_hex" not in record["events"][1]


def test_command_history_redacts_real_apple_auth_payload_but_keeps_boolean():
    history = command_history_fixture(
        states=("ble_inv_begin", "ble_inv_read", "ble_inv_end"),
        terminal_state="complete",
    )
    history["events"][1].update(
        ble_apple_auth="01a5ff",
        ble_auth_payload="01a5ff",
        authentication_required=True,
    )
    record = validate_command_history(
        history, device_id=DEVICE_ID, command_id=COMMAND_ID,
        terminal_state="complete",
    )
    event = record["events"][1]
    assert "ble_apple_auth" not in event
    assert "ble_auth_payload" not in event
    assert event["authentication_required"] is True


def test_command_wait_rejects_nonterminal_timeout(tmp_path):
    history = command_history_fixture(states=("ble_inv_begin",))
    history.update(
        state="delivered", next_sequence=1, result_state="scanning",
        terminal=False,
    )

    def fetch(_url: str, *, timeout: int):
        assert timeout == 10
        return history

    ticks = iter((0.0, 0.0, 2.0))
    with pytest.raises(EvidenceError, match="timed out"):
        wait_for_command(
            backend_base="http://127.0.0.1:8000",
            device_id=DEVICE_ID,
            command_id=COMMAND_ID,
            terminal_state="complete",
            timeout_s=1,
            output=tmp_path / "evidence" / "canary.jsonl",
            fetch_json=fetch,
            monotonic=lambda: next(ticks),
            sleep=lambda _seconds: None,
        )


@pytest.mark.parametrize(
    ("row", "want"),
    [
        ({"timestamp": 1_785_600_000}, 1_785_600_000_000),
        ({"observed_at_ms": 1_785_600_000_000}, 1_785_600_000_000),
    ],
)
def test_history_timestamp_units_are_normalized_exactly(row, want):
    assert normalize_history_timestamp_ms(row) == want


@pytest.mark.parametrize(
    "row",
    [
        {"timestamp": -1},
        {"timestamp": 1_699_999_999},
        {"timestamp": 1_785_600_000_000},
        {"observed_at_ms": 1_785_600_000},
        {"timestamp": 1_785_600_000, "observed_at_ms": 1_785_600_000_000},
        {"observed_at_ms": 1_785_600_000_000, "received_at_ms": 1_785_600_000_001},
        {"timestamp": 9_223_372_036_854_776},
    ],
)
def test_history_timestamp_rejects_negative_pre_epoch_mixed_or_overflow(row):
    with pytest.raises(EvidenceError, match="timestamp"):
        normalize_history_timestamp_ms(row)


@pytest.mark.parametrize(
    ("after_ms", "accepted_second", "rejected_second"),
    [
        (1_785_600_000_999, 1_785_600_001, 1_785_600_000),
        (1_785_600_001_000, 1_785_600_001, 1_785_600_000),
    ],
)
def test_seconds_cutoff_matches_database_precision(
    after_ms, accepted_second, rejected_second,
):
    accepted = history_row(kind="drone", timestamp=accepted_second)
    rejected = history_row(kind="drone", timestamp=rejected_second)
    kwargs = dict(
        device_id=DEVICE_ID, kind="drone", source="wifi_beacon_rid",
        identity_field="drone_id", identity_value="RID-CANON-001",
        after_ms=after_ms,
    )
    assert find_matching_detection(history_fixture(accepted), **kwargs) is not None
    assert find_matching_detection(history_fixture(rejected), **kwargs) is None


def test_detection_match_requires_exact_source_identity_and_meta_evidence():
    drone = find_matching_detection(
        history_fixture(history_row(kind="drone")),
        device_id=DEVICE_ID, kind="drone", source="wifi_beacon_rid",
        identity_field="drone_id", identity_value="RID-CANON-001",
        after_ms=1_785_600_000_000,
    )
    meta = find_matching_detection(
        history_fixture(history_row(kind="meta")),
        device_id=DEVICE_ID, kind="meta", source="ble_fingerprint",
        identity_field="bssid", identity_value="AA:BB:CC:DD:EE:02",
        manufacturer="Meta Glasses", service_uuid_token="fd5f",
        after_ms=1_785_600_000_000,
    )
    assert drone["source"] == "wifi_beacon_rid"
    assert meta["manufacturer"] == "Meta Glasses"
    assert "value_hex" not in meta


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("device_id", "uplink_OTHER"),
        ("source", "ble_rid"),
        ("bssid", "AA:BB:CC:DD:EE:03"),
        ("manufacturer", "Meta"),
        ("ble_svc_uuids", "180f"),
    ],
)
def test_unrelated_meta_detection_after_cutoff_is_not_accepted(field, value):
    row = history_row(kind="meta")
    row[field] = value
    match = find_matching_detection(
        history_fixture(row),
        device_id=DEVICE_ID, kind="meta", source="ble_fingerprint",
        identity_field="bssid", identity_value="AA:BB:CC:DD:EE:02",
        manufacturer="Meta Glasses", service_uuid_token="fd5f",
        after_ms=1_785_600_000_000,
    )
    assert match is None


def test_wait_detection_creates_ready_file_only_after_successful_initial_poll(
    tmp_path,
):
    ready = tmp_path / ".canary" / "evidence" / "drone.ready"
    output = tmp_path / ".canary" / "evidence" / "canary.jsonl"
    calls = []

    def fetch(url: str, *, timeout: int):
        calls.append((ready.exists(), timeout, url))
        if len(calls) == 1:
            return history_fixture()
        return history_fixture(history_row(kind="drone", timestamp=1_785_600_006))

    ticks = iter((0.0, 0.0, 0.1, 0.1))
    record = wait_for_detection(
        backend_base="http://127.0.0.1:8000", device_id=DEVICE_ID,
        kind="drone", source="wifi_beacon_rid", identity_field="drone_id",
        identity_value="RID-CANON-001", after_ms=1_785_600_000_000,
        ready_file=ready, timeout_s=120, output=output, fetch_json=fetch,
        now=lambda: 1_785_600_001.0, monotonic=lambda: next(ticks),
        sleep=lambda _seconds: None,
    )
    assert calls[0][0] is False
    assert calls[1][0] is True
    assert record["detection"]["drone_id"] == "RID-CANON-001"
    ready_record = json.loads(ready.read_text())
    assert record["after_ms"] == ready_record["ready_cutoff_ms"]
    assert record["after_ms"] > 1_785_600_001_000
    assert stat.S_IMODE(ready.stat().st_mode) == 0o600


def test_ready_file_is_absent_until_atomic_ready_publication(tmp_path):
    ready = tmp_path / ".canary/evidence/source.ready"
    visible: list[bool] = []

    def now() -> float:
        visible.append(ready.exists() and ready.stat().st_size > 0)
        return 1_785_600_001.0

    cutoff = _create_ready_file(
        ready,
        {"schema": 1, "kind": "drone"},
        requested_after_ms=1_785_600_000_000,
        now=now,
    )
    assert visible[:2] == [False, False]
    assert visible[-1] is True
    assert json.loads(ready.read_text()) == {
        "kind": "drone",
        "ready_cutoff_ms": cutoff,
        "requested_after_ms": 1_785_600_000_000,
        "schema": 1,
        "state": "ready",
    }


def test_wait_detection_rejects_source_already_present_before_ready(tmp_path):
    ready = tmp_path / ".canary/evidence/drone.ready"
    with pytest.raises(EvidenceError, match="before ready"):
        wait_for_detection(
            backend_base="http://127.0.0.1:8000", device_id=DEVICE_ID,
            kind="drone", source="wifi_beacon_rid", identity_field="drone_id",
            identity_value="RID-CANON-001", after_ms=1_785_600_000_000,
            ready_file=ready, timeout_s=120,
            output=tmp_path / ".canary/evidence/canary.jsonl",
            fetch_json=lambda _url, timeout: history_fixture(
                history_row(kind="drone", timestamp=1_785_600_001)
            ),
            now=lambda: 1_785_600_001.0,
            monotonic=lambda: 0.0,
            sleep=lambda _seconds: None,
        )
    assert not ready.exists()


def test_serial_log_receipt_requires_exact_private_current_three_roles(tmp_path):
    root = tmp_path / "serial-logs"
    root.mkdir(mode=0o700)
    observed_at_ms = 1_785_600_000_000
    for role in ("scanner0", "scanner1", "uplink"):
        path = root / f"{role}.log"
        path.write_bytes(f"{role} healthy\n".encode())
        path.chmod(0o600)
        os.utime(path, ns=(observed_at_ms * 1_000_000,) * 2)
    receipts = _serial_log_receipt(root, observed_at_ms=observed_at_ms)
    assert [item["role"] for item in receipts] == [
        "scanner0", "scanner1", "uplink",
    ]
    (root / "scanner1.log").unlink()
    with pytest.raises(EvidenceError, match="exactly"):
        _serial_log_receipt(root, observed_at_ms=observed_at_ms)


@pytest.mark.parametrize(
    "forbidden",
    (
        b"secret=wifi-value\n",
        b"token=backend-value\n",
        b"cookie=session-value\n",
        b"Set-Cookie: session=value\n",
        b"raw_auth_payload=001122\n",
    ),
)
def test_serial_log_receipt_rejects_full_secret_and_raw_auth_vocabulary(
    tmp_path,
    forbidden,
):
    root = tmp_path / "serial-logs"
    root.mkdir(mode=0o700)
    observed_at_ms = 1_785_600_000_000
    for role in ("scanner0", "scanner1", "uplink"):
        path = root / f"{role}.log"
        path.write_bytes(b"healthy\n")
        path.chmod(0o600)
        os.utime(path, ns=(observed_at_ms * 1_000_000,) * 2)
    (root / "scanner0.log").write_bytes(forbidden)
    (root / "scanner0.log").chmod(0o600)
    os.utime(
        root / "scanner0.log",
        ns=(observed_at_ms * 1_000_000,) * 2,
    )

    with pytest.raises(EvidenceError, match="serial log gate"):
        _serial_log_receipt(root, observed_at_ms=observed_at_ms)


def test_monitor_records_exact_three_board_serial_receipts(tmp_path):
    output = tmp_path / ".canary/evidence/monitor.jsonl"
    write_state(output)
    logs = tmp_path / ".canary/serial-logs"
    logs.mkdir(mode=0o700)
    observed_at_ms = 1_785_600_000_000
    for role in ("scanner0", "scanner1", "uplink"):
        path = logs / f"{role}.log"
        path.write_text(f"{role} running\n")
        path.chmod(0o600)
        os.utime(path, ns=(observed_at_ms * 1_000_000,) * 2)
    ticks = iter((0.0, 1.0))
    monitor_soak(
        backend_base="http://127.0.0.1:8000",
        device_id=DEVICE_ID,
        duration_s=1,
        interval_s=1,
        serial_log_dir=logs,
        output=output,
        fetch_json=FakeBackend(),
        now=lambda: observed_at_ms / 1000,
        monotonic=lambda: next(ticks),
        sleep=lambda _seconds: None,
    )
    row = json.loads(output.read_text())
    assert [item["role"] for item in row["serial_logs"]] == [
        "scanner0", "scanner1", "uplink",
    ]


def soak_backend(*, queue_depth: int = 0, fifo_sequence: int = 1) -> dict:
    return {
        "identity": {
            "firmware_target": "uplink-s3-backend",
            "app_project": "fof_backend_uplink",
            "hardware_type": "seeed_xiao_esp32s3",
            "firmware_version": "0.1.0-backend",
            "hardware_mac": "A4:CF:12:CB:77:A4",
        },
        "uplink_boot_id": 9001,
        "scanner_profiles": ["ble_primary", "wifi_primary"],
        "scanners": [
            {
                **scanner_fixture(0, "ble_primary"),
                "final_health": True,
            },
            {
                **scanner_fixture(1, "wifi_primary"),
                "final_health": True,
            },
        ],
        "upload_queue": {
            "depth_batches": queue_depth,
            "capacity_batches": 512,
            "overflow_dropped_batches": 0,
            "quarantined_batches": 0,
        },
        "fifo_sequence": fifo_sequence,
        "continuity": dict(CONTINUITY),
        "health": {"uptime_ms": 90_000},
    }


def write_soak_fixture(
    tmp_path: Path,
    *,
    duration_s: int = 86_400,
    heartbeat_gap_s: int = 90,
    final_queue_depth: int = 0,
) -> Path:
    path = tmp_path / "canary.jsonl"
    first_ms = 1_785_600_000_000
    rows = [{
        "schema": 1, "record_type": "snapshot",
        "phase": "network-recovery", "device_id": DEVICE_ID,
        "observed_at_ms": first_ms,
        "backend": soak_backend(queue_depth=0, fifo_sequence=1),
    }]
    offsets = list(range(0, duration_s + 1, heartbeat_gap_s))
    if offsets[-1] != duration_s:
        offsets.append(duration_s)
    for index, offset_s in enumerate(offsets):
        backend = soak_backend(
            queue_depth=(final_queue_depth if offset_s == duration_s else 0),
            fifo_sequence=index + 2,
        )
        backend["health"]["uptime_ms"] += offset_s * 1000
        backend["max_heartbeat_gap_s"] = heartbeat_gap_s
        rows.append({
            "schema": 1, "record_type": "monitor", "phase": "soak",
            "device_id": DEVICE_ID,
            "observed_at_ms": first_ms + offset_s * 1000,
            "heartbeat_at_ms": first_ms + offset_s * 1000,
            "serial_logs": [
                {
                    "role": role,
                    "name": f"{role}.log",
                    "size": 100 + index,
                    "sha256": f"{slot + 1}" * 64,
                    "mtime_ms": first_ms + offset_s * 1000,
                }
                for slot, role in enumerate(("scanner0", "scanner1", "uplink"))
            ],
            "backend": backend,
        })
    path.write_text("".join(
        json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n"
        for row in rows
    ))
    path.chmod(0o600)
    return path


def test_soak_rejects_gap_or_unfinished_queue(tmp_path):
    for kwargs in (
        {"heartbeat_gap_s": 91},
        {"final_queue_depth": 2},
    ):
        path = write_soak_fixture(tmp_path, **kwargs)
        with pytest.raises(EvidenceError):
            verify_soak(
                path, expected_duration_s=86_400, max_heartbeat_gap_s=90,
            )


def test_soak_computes_heartbeat_gap_instead_of_trusting_reported_summary(tmp_path):
    path = write_soak_fixture(tmp_path, duration_s=91, heartbeat_gap_s=91)
    rows = [json.loads(line) for line in path.read_text().splitlines()]
    rows[-1]["backend"]["max_heartbeat_gap_s"] = 0
    path.write_text("".join(json.dumps(row) + "\n" for row in rows))
    with pytest.raises(EvidenceError, match="heartbeat gap"):
        verify_soak(path, expected_duration_s=91, max_heartbeat_gap_s=90)


def test_soak_rejects_86399_seconds_and_accepts_86400(tmp_path):
    short = write_soak_fixture(tmp_path, duration_s=86_399)
    with pytest.raises(EvidenceError, match="duration"):
        verify_soak(short, expected_duration_s=86_400, max_heartbeat_gap_s=90)
    passing = write_soak_fixture(tmp_path, duration_s=86_400)
    result = verify_soak(
        passing, expected_duration_s=86_400, max_heartbeat_gap_s=90,
    )
    assert result.duration_s == 86_400
    assert result.final_queue_depth == 0


def test_soak_rejects_missing_scanner_final_health(tmp_path):
    path = write_soak_fixture(tmp_path)
    rows = [json.loads(line) for line in path.read_text().splitlines()]
    rows[-1]["backend"]["scanners"][1].pop("final_health")
    path.write_text("".join(json.dumps(row) + "\n" for row in rows))
    with pytest.raises(EvidenceError, match="final_health"):
        verify_soak(path, expected_duration_s=86_400, max_heartbeat_gap_s=90)


def test_soak_rejects_missing_three_board_serial_receipts(tmp_path):
    path = write_soak_fixture(tmp_path)
    rows = [json.loads(line) for line in path.read_text().splitlines()]
    rows[-1]["serial_logs"] = rows[-1]["serial_logs"][:2]
    path.write_text("".join(json.dumps(row) + "\n" for row in rows))
    with pytest.raises(EvidenceError, match="serial"):
        verify_soak(path, expected_duration_s=86_400, max_heartbeat_gap_s=90)


def test_soak_rejects_out_of_order_fifo_or_counter_increment(tmp_path):
    for mutation in ("fifo", "drop", "quarantine"):
        path = write_soak_fixture(tmp_path)
        rows = [json.loads(line) for line in path.read_text().splitlines()]
        if mutation == "fifo":
            rows[-1]["backend"]["fifo_sequence"] = 1
        elif mutation == "drop":
            rows[-1]["backend"]["upload_queue"]["overflow_dropped_batches"] = 1
        else:
            rows[-1]["backend"]["upload_queue"]["quarantined_batches"] = 1
        path.write_text("".join(json.dumps(row) + "\n" for row in rows))
        with pytest.raises(EvidenceError):
            verify_soak(
                path, expected_duration_s=86_400, max_heartbeat_gap_s=90,
            )


def test_soak_rejects_scanner_role_generation_drift(tmp_path):
    path = write_soak_fixture(tmp_path)
    rows = [json.loads(line) for line in path.read_text().splitlines()]
    rows[-1]["backend"]["scanners"][0]["role_generation"] = 5
    path.write_text("".join(json.dumps(row) + "\n" for row in rows))
    with pytest.raises(EvidenceError, match="scanner binding"):
        verify_soak(path, expected_duration_s=86_400, max_heartbeat_gap_s=90)


def test_soak_rejects_duplicate_physical_mac_binding(tmp_path):
    path = write_soak_fixture(tmp_path)
    rows = [json.loads(line) for line in path.read_text().splitlines()]
    for row in rows:
        row["backend"]["scanners"][1]["mac"] = "AA:BB:CC:DD:EE:01"
    path.write_text("".join(json.dumps(row) + "\n" for row in rows))
    with pytest.raises(EvidenceError, match="MAC"):
        verify_soak(path, expected_duration_s=86_400, max_heartbeat_gap_s=90)


def test_soak_rejects_changed_calibration_continuity_or_secret(tmp_path):
    for mutation in ("continuity", "secret"):
        path = write_soak_fixture(tmp_path)
        rows = [json.loads(line) for line in path.read_text().splitlines()]
        if mutation == "continuity":
            rows[-1]["backend"]["continuity"]["session_id"] = "changed"
        else:
            rows[-1]["backend"]["api_token"] = "must-not-appear"
        path.write_text("".join(json.dumps(row) + "\n" for row in rows))
        with pytest.raises(EvidenceError):
            verify_soak(
                path, expected_duration_s=86_400, max_heartbeat_gap_s=90,
            )
