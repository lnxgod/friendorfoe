"""Append-only, private, fsync-backed Lite manufacturing ledger."""

from __future__ import annotations

import csv
import fcntl
import json
import os
import re
import tempfile
from contextlib import contextmanager
from dataclasses import asdict
from datetime import datetime, timezone
from pathlib import Path

from tools.badge_flasher.models import TopologyAssignment
from tools.badge_flasher.public_output import scrub_user_visible_text
from tools.badge_flasher.topology import normalize_mac

from .models import LiteBatchResult, PassedLiteFactoryRecord


class LedgerError(RuntimeError):
    """The private Lite manufacturing history is unsafe to trust."""


_VERSION_RE = re.compile(r"\d+\.\d+\.\d+(?:-[0-9A-Za-z._-]+)?")
_SHA256_RE = re.compile(r"[0-9a-f]{64}")
_ASSIGNMENT_FIELDS = frozenset((
    "uplink_mac",
    "ble_leaf_mac",
    "wifi_leaf_mac",
))


def _hardware(assignment: TopologyAssignment) -> set[str]:
    return {
        assignment.uplink_mac,
        assignment.ble_leaf_mac,
        assignment.wifi_leaf_mac,
    }


class LiteManufacturingLedger:
    def __init__(
        self,
        directory: Path,
        *,
        session_lock_path: Path | None = None,
    ) -> None:
        self.directory = directory
        self.session_lock_path = session_lock_path or (
            Path(tempfile.gettempdir()) / "fof-lite-factory.lock"
        )

    @property
    def jsonl_path(self) -> Path:
        return self.directory / "lite-factory.jsonl"

    @property
    def csv_path(self) -> Path:
        return self.directory / "lite-factory.csv"

    def _prepare(self) -> None:
        if self.directory.is_symlink():
            raise LedgerError("Lite factory record directory must not be a symlink")
        self.directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        if not self.directory.is_dir():
            raise LedgerError("Lite factory record path is not a directory")
        os.chmod(self.directory, 0o700)

    @contextmanager
    def exclusive_session(self):
        """Hold one factory-wide lock through hardware work and PASS commit."""

        self._prepare()
        lock_path = self.session_lock_path
        flags = os.O_RDWR | os.O_CREAT
        flags |= getattr(os, "O_NOFOLLOW", 0)
        try:
            descriptor = os.open(lock_path, flags, 0o600)
        except OSError as exc:
            raise LedgerError("cannot open the Lite factory session lock") from exc
        try:
            os.fchmod(descriptor, 0o600)
            try:
                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as exc:
                raise LedgerError(
                    "another Lite factory process is already running"
                ) from exc
            yield
        finally:
            try:
                fcntl.flock(descriptor, fcntl.LOCK_UN)
            finally:
                os.close(descriptor)

    @staticmethod
    def _open_append(path: Path, *, newline: str | None = None):
        descriptor = os.open(
            path,
            os.O_WRONLY
            | os.O_CREAT
            | os.O_APPEND
            | getattr(os, "O_NOFOLLOW", 0),
            0o600,
        )
        os.fchmod(descriptor, 0o600)
        return os.fdopen(descriptor, "a", encoding="utf-8", newline=newline)

    def passed_records(self) -> tuple[PassedLiteFactoryRecord, ...]:
        if self.jsonl_path.is_symlink():
            raise LedgerError("Lite factory JSONL record must not be a symlink")
        if not self.jsonl_path.is_file():
            return ()
        try:
            lines = self.jsonl_path.read_text(encoding="utf-8").splitlines()
        except OSError as exc:
            raise LedgerError("cannot read prior Lite factory records") from exc
        records: list[PassedLiteFactoryRecord] = []
        for line_number, line in enumerate(lines, start=1):
            try:
                row = json.loads(line)
            except json.JSONDecodeError as exc:
                raise LedgerError(f"Lite ledger row {line_number} is malformed") from exc
            if not isinstance(row, dict):
                raise LedgerError(f"Lite ledger row {line_number} is not an object")
            if row.get("passed") is not True:
                continue
            records.append(self._parse_pass(row, line_number))
        for index, record in enumerate(records):
            for prior in records[:index]:
                if _hardware(record.assignment) & _hardware(prior.assignment) and (
                    record.assignment != prior.assignment
                ):
                    raise LedgerError("conflicting Lite PASS assignments share hardware")
        return tuple(records)

    @staticmethod
    def _parse_pass(
        row: dict[str, object],
        line_number: int,
    ) -> PassedLiteFactoryRecord:
        version = row.get("version")
        scanner_version = row.get("scanner_version")
        digest = row.get("bundle_sha256")
        assignment = row.get("assignment")
        if (
            row.get("schema") != 1
            or row.get("family") != "badge_lite"
            or not isinstance(version, str)
            or _VERSION_RE.fullmatch(version) is None
            or not isinstance(scanner_version, str)
            or _VERSION_RE.fullmatch(scanner_version) is None
            or not isinstance(digest, str)
            or _SHA256_RE.fullmatch(digest) is None
            or not isinstance(assignment, dict)
            or set(assignment) != _ASSIGNMENT_FIELDS
            or any(not isinstance(assignment[field], str) for field in _ASSIGNMENT_FIELDS)
        ):
            raise LedgerError(f"Lite PASS row {line_number} has invalid canonical fields")
        try:
            normalized = TopologyAssignment(
                normalize_mac(assignment["uplink_mac"]),
                normalize_mac(assignment["ble_leaf_mac"]),
                normalize_mac(assignment["wifi_leaf_mac"]),
            )
        except (KeyError, ValueError) as exc:
            raise LedgerError(f"Lite PASS row {line_number} has invalid assignment") from exc
        if len(_hardware(normalized)) != 3:
            raise LedgerError(f"Lite PASS row {line_number} repeats hardware")
        return PassedLiteFactoryRecord(
            version=version,
            bundle_sha256=digest,
            assignment=normalized,
        )

    def record(self, result: LiteBatchResult) -> bool:
        """Commit JSONL authoritatively; return whether CSV projection succeeded."""

        self._prepare()
        timestamp = datetime.now(timezone.utc).isoformat()
        stored_error = result.error
        if not result.passed and stored_error is not None:
            stored_error = scrub_user_visible_text(stored_error)
        payload = {
            "schema": 1,
            "family": "badge_lite",
            "timestamp": timestamp,
            **asdict(result),
        }
        payload["error"] = stored_error
        with self._open_append(self.jsonl_path) as handle:
            handle.write(json.dumps(payload, sort_keys=True, separators=(",", ":")) + "\n")
            handle.flush()
            os.fsync(handle.fileno())
        try:
            self._append_csv({
                "timestamp": timestamp,
                "unit_id": result.unit_id,
                "version": result.version,
                "scanner_version": result.scanner_version,
                "passed": result.passed,
                "phase": result.phase,
                "uplink_mac": result.assignment.uplink_mac,
                "ble_mac": result.assignment.ble_leaf_mac,
                "wifi_mac": result.assignment.wifi_leaf_mac,
                "bundle_sha256": result.bundle_sha256,
                "receipt": result.receipt or "",
                "error": stored_error or "",
            })
        except (OSError, csv.Error):
            return False
        return True

    def record_failure(
        self,
        *,
        version: str,
        scanner_version: str,
        bundle_sha256: str,
        phase: str,
        error: str,
    ) -> bool:
        self._prepare()
        timestamp = datetime.now(timezone.utc).isoformat()
        scrubbed = scrub_user_visible_text(error)
        payload = {
            "schema": 1,
            "family": "badge_lite",
            "timestamp": timestamp,
            "unit_id": "",
            "version": version,
            "scanner_version": scanner_version,
            "bundle_sha256": bundle_sha256,
            "passed": False,
            "phase": phase,
            "assignment": None,
            "devices": [],
            "runtime": {},
            "receipt": None,
            "error": scrubbed,
        }
        with self._open_append(self.jsonl_path) as handle:
            handle.write(json.dumps(payload, sort_keys=True, separators=(",", ":")) + "\n")
            handle.flush()
            os.fsync(handle.fileno())
        try:
            self._append_csv({
                "timestamp": timestamp,
                "unit_id": "",
                "version": version,
                "scanner_version": scanner_version,
                "passed": False,
                "phase": phase,
                "uplink_mac": "",
                "ble_mac": "",
                "wifi_mac": "",
                "bundle_sha256": bundle_sha256,
                "receipt": "",
                "error": scrubbed,
            })
        except (OSError, csv.Error):
            return False
        return True

    def _append_csv(self, row: dict[str, object]) -> None:
        exists = self.csv_path.exists()
        with self._open_append(self.csv_path, newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(row))
            if not exists:
                writer.writeheader()
            writer.writerow(row)
            handle.flush()
            os.fsync(handle.fileno())
