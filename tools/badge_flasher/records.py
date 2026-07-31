"""Append-only, fsync-backed factory manufacturing ledger."""

from __future__ import annotations

import csv
import json
import os
import re
from dataclasses import asdict
from datetime import datetime, timezone
from pathlib import Path

from .models import BatchResult, PassedFactoryRecord, TopologyAssignment
from .public_output import scrub_user_visible_text
from .topology import normalize_mac


class LedgerError(RuntimeError):
    """The private manufacturing history is unsafe to use as evidence."""


_VERSION_RE = re.compile(
    r"\d+\.\d+\.\d+(?:-[0-9A-Za-z._-]+)?"
)
_SHA256_RE = re.compile(r"[0-9a-f]{64}")
_GAME_SEEDS = frozenset(("normal", "infected", "immune"))
_ASSIGNMENT_FIELDS = frozenset((
    "uplink_mac",
    "ble_leaf_mac",
    "wifi_leaf_mac",
))


class ManufacturingLedger:
    def __init__(self, directory: Path) -> None:
        self.directory = directory

    def passed_macs(self) -> set[str]:
        """Return MACs previously recorded in PASS rows, tolerating no ledger."""
        path = self.directory / "badge-factory.csv"
        if not path.is_file():
            return set()
        result: set[str] = set()
        try:
            with path.open(newline="", encoding="utf-8") as handle:
                for row in csv.DictReader(handle):
                    if str(row.get("passed", "")).lower() not in ("true", "1", "yes"):
                        continue
                    for field in ("uplink_mac", "ble_mac", "wifi_mac"):
                        value = str(row.get(field, "")).strip().upper()
                        if value:
                            result.add(value)
        except (OSError, csv.Error):
            return set()
        return result

    def passed_records(self) -> tuple[PassedFactoryRecord, ...]:
        """Read prior PASS evidence strictly from the authoritative JSONL."""
        path = self.directory / "badge-factory.jsonl"
        if not path.is_file():
            return ()
        records: list[PassedFactoryRecord] = []
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except OSError as exc:
            raise LedgerError("cannot read prior factory PASS records") from exc
        for line_number, line in enumerate(lines, start=1):
            try:
                row = json.loads(line)
            except json.JSONDecodeError as exc:
                raise LedgerError(
                    f"factory ledger row {line_number} is malformed"
                ) from exc
            if not isinstance(row, dict):
                raise LedgerError(
                    f"factory ledger row {line_number} is not an object"
                )
            if row.get("passed") is not True:
                continue
            records.append(self._parse_passed_row(row, line_number))

        for index, record in enumerate(records):
            hardware = _record_hardware(record)
            for other in records[:index]:
                other_hardware = _record_hardware(other)
                if hardware & other_hardware and (
                    record.assignment != other.assignment
                ):
                    raise LedgerError(
                        "conflicting prior PASS assignments share hardware"
                    )
        return tuple(records)

    @staticmethod
    def _parse_passed_row(
        row: dict[str, object],
        line_number: int,
    ) -> PassedFactoryRecord:
        version = row.get("version")
        digest = row.get("bundle_sha256")
        game_seed = row.get("game_seed")
        assignment = row.get("assignment")
        if (
            not isinstance(version, str)
            or _VERSION_RE.fullmatch(version) is None
            or not isinstance(digest, str)
            or _SHA256_RE.fullmatch(digest) is None
            or not isinstance(game_seed, str)
            or game_seed not in _GAME_SEEDS
            or not isinstance(assignment, dict)
            or set(assignment) != _ASSIGNMENT_FIELDS
            or any(
                not isinstance(assignment[field], str)
                for field in _ASSIGNMENT_FIELDS
            )
        ):
            raise LedgerError(
                f"factory PASS row {line_number} has invalid canonical fields"
            )
        try:
            normalized = TopologyAssignment(
                normalize_mac(assignment["uplink_mac"]),
                normalize_mac(assignment["ble_leaf_mac"]),
                normalize_mac(assignment["wifi_leaf_mac"]),
            )
        except (KeyError, ValueError) as exc:
            raise LedgerError(
                f"factory PASS row {line_number} has invalid assignment"
            ) from exc
        if len(_record_hardware_assignment(normalized)) != 3:
            raise LedgerError(
                f"factory PASS row {line_number} repeats hardware"
            )
        return PassedFactoryRecord(
            version=version,
            bundle_sha256=digest,
            assignment=normalized,
            game_seed=game_seed,
        )

    def record(self, result: BatchResult) -> None:
        self.directory.mkdir(parents=True, exist_ok=True)
        timestamp = datetime.now(timezone.utc).isoformat()
        stored_error = result.error
        if not result.passed and stored_error is not None:
            stored_error = scrub_user_visible_text(stored_error)
        payload = {"timestamp": timestamp, **asdict(result)}
        payload["error"] = stored_error
        jsonl = self.directory / "badge-factory.jsonl"
        with jsonl.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(payload, sort_keys=True, separators=(",", ":")) + "\n")
            handle.flush()
            os.fsync(handle.fileno())
        self._append_csv({
            "timestamp": timestamp,
            "badge_id": result.badge_id,
            "version": result.version,
            "passed": result.passed,
            "phase": result.phase,
            "uplink_mac": result.assignment.uplink_mac,
            "ble_mac": result.assignment.ble_leaf_mac,
            "wifi_mac": result.assignment.wifi_leaf_mac,
            "bundle_sha256": result.bundle_sha256,
            "error": stored_error or "",
        })

    def record_failure(
        self,
        *,
        version: str,
        bundle_sha256: str,
        phase: str,
        error: str,
        game_seed: str,
    ) -> None:
        """Persist a failed attempt even when topology was never safe to assign."""
        self.directory.mkdir(parents=True, exist_ok=True)
        timestamp = datetime.now(timezone.utc).isoformat()
        scrubbed_error = scrub_user_visible_text(error)
        payload = {
            "timestamp": timestamp,
            "badge_id": "",
            "version": version,
            "bundle_sha256": bundle_sha256,
            "passed": False,
            "phase": phase,
            "assignment": None,
            "devices": [],
            "runtime": {},
            "game_seed": game_seed,
            "receipt": None,
            "error": scrubbed_error,
        }
        jsonl = self.directory / "badge-factory.jsonl"
        with jsonl.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(payload, sort_keys=True, separators=(",", ":")) + "\n")
            handle.flush()
            os.fsync(handle.fileno())
        self._append_csv({
            "timestamp": timestamp,
            "badge_id": "",
            "version": version,
            "passed": False,
            "phase": phase,
            "uplink_mac": "",
            "ble_mac": "",
            "wifi_mac": "",
            "bundle_sha256": bundle_sha256,
            "error": scrubbed_error,
        })

    def _append_csv(self, row: dict[str, object]) -> None:
        csv_path = self.directory / "badge-factory.csv"
        exists = csv_path.exists()
        with csv_path.open("a", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(row))
            if not exists:
                writer.writeheader()
            writer.writerow(row)
            handle.flush()
            os.fsync(handle.fileno())


def _record_hardware_assignment(
    assignment: TopologyAssignment,
) -> set[str]:
    return {
        assignment.uplink_mac,
        assignment.ble_leaf_mac,
        assignment.wifi_leaf_mac,
    }


def _record_hardware(record: PassedFactoryRecord) -> set[str]:
    return _record_hardware_assignment(record.assignment)
