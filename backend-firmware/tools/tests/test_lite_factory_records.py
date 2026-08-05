from __future__ import annotations

import json
import stat
from pathlib import Path

import pytest

from tools.lite_factory_flasher.models import LiteBatchResult
from tools.lite_factory_flasher.records import LedgerError, LiteManufacturingLedger
from tools.badge_flasher.models import TopologyAssignment


ASSIGNMENT = TopologyAssignment(
    "AA:BB:CC:DD:EE:01",
    "AA:BB:CC:DD:EE:02",
    "AA:BB:CC:DD:EE:03",
)


def _result() -> LiteBatchResult:
    return LiteBatchResult(
        unit_id="DDEE01",
        version="0.2.0-backend",
        scanner_version="0.67.2-badge-defcon34",
        bundle_sha256="a" * 64,
        passed=True,
        phase="complete",
        assignment=ASSIGNMENT,
        devices=(),
        runtime={"health": {"rollback_clear": True}},
        receipt="lite_12345678",
    )


def _pass_row(assignment: TopologyAssignment = ASSIGNMENT) -> dict[str, object]:
    return {
        "schema": 1,
        "family": "badge_lite",
        "passed": True,
        "version": "0.2.0-backend",
        "scanner_version": "0.67.2-badge-defcon34",
        "bundle_sha256": "a" * 64,
        "assignment": {
            "uplink_mac": assignment.uplink_mac,
            "ble_leaf_mac": assignment.ble_leaf_mac,
            "wifi_leaf_mac": assignment.wifi_leaf_mac,
        },
    }


def test_pass_ledger_is_private_append_only_and_recoverable(tmp_path: Path) -> None:
    root = tmp_path / "factory-records"
    ledger = LiteManufacturingLedger(root)
    result = _result()

    assert ledger.record(result) is True

    assert stat.S_IMODE(root.stat().st_mode) == 0o700
    assert stat.S_IMODE(ledger.jsonl_path.stat().st_mode) == 0o600
    assert stat.S_IMODE(ledger.csv_path.stat().st_mode) == 0o600
    assert ledger.passed_records()[0].assignment == ASSIGNMENT
    stored = json.loads(ledger.jsonl_path.read_text(encoding="utf-8"))
    assert stored["family"] == "badge_lite"
    assert stored["receipt"] == "lite_12345678"
    assert "game_seed" not in stored


def test_jsonl_pass_remains_authoritative_when_csv_projection_fails(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    ledger = LiteManufacturingLedger(tmp_path / "factory-records")

    def fail_projection(_row: dict[str, object]) -> None:
        raise OSError("projection unavailable")

    monkeypatch.setattr(ledger, "_append_csv", fail_projection)

    assert ledger.record(_result()) is False
    assert ledger.passed_records()[0].assignment == ASSIGNMENT
    assert not ledger.csv_path.exists()


def test_factory_session_lock_rejects_a_second_process_boundary(
    tmp_path: Path,
) -> None:
    lock_path = tmp_path / "factory.lock"
    first = LiteManufacturingLedger(
        tmp_path / "factory-records-a",
        session_lock_path=lock_path,
    )
    second = LiteManufacturingLedger(
        tmp_path / "factory-records-b",
        session_lock_path=lock_path,
    )

    with first.exclusive_session():
        with pytest.raises(LedgerError, match="already running"):
            with second.exclusive_session():
                raise AssertionError("second factory lock unexpectedly acquired")

    with second.exclusive_session():
        pass


def test_ledger_rejects_conflicting_pass_graphs(tmp_path: Path) -> None:
    root = tmp_path / "factory-records"
    root.mkdir()
    conflicting = TopologyAssignment(
        ASSIGNMENT.uplink_mac,
        "AA:BB:CC:DD:EE:04",
        "AA:BB:CC:DD:EE:05",
    )
    rows = (_pass_row(), _pass_row(conflicting))
    (root / "lite-factory.jsonl").write_text(
        "".join(json.dumps(row) + "\n" for row in rows),
        encoding="utf-8",
    )

    with pytest.raises(LedgerError, match="conflicting Lite PASS assignments"):
        LiteManufacturingLedger(root).passed_records()


@pytest.mark.parametrize(
    "mutation",
    (
        {"bundle_sha256": "not-a-digest"},
        {"scanner_version": None},
        {"family": "badge"},
        {
            "assignment": {
                "uplink_mac": ASSIGNMENT.uplink_mac,
                "ble_leaf_mac": ASSIGNMENT.ble_leaf_mac,
                "wifi_leaf_mac": ASSIGNMENT.ble_leaf_mac,
            }
        },
    ),
)
def test_ledger_rejects_malformed_pass_evidence(
    tmp_path: Path,
    mutation: dict[str, object],
) -> None:
    root = tmp_path / "factory-records"
    root.mkdir()
    row = {**_pass_row(), **mutation}
    (root / "lite-factory.jsonl").write_text(
        json.dumps(row) + "\n",
        encoding="utf-8",
    )

    with pytest.raises(LedgerError):
        LiteManufacturingLedger(root).passed_records()
