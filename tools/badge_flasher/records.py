"""Append-only, fsync-backed factory manufacturing ledger."""

from __future__ import annotations

import csv
import json
import os
from dataclasses import asdict
from datetime import datetime, timezone
from pathlib import Path

from .models import BatchResult


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

    def record(self, result: BatchResult) -> None:
        self.directory.mkdir(parents=True, exist_ok=True)
        timestamp = datetime.now(timezone.utc).isoformat()
        payload = {"timestamp": timestamp, **asdict(result)}
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
            "error": result.error or "",
        })

    def record_failure(
        self, *, version: str, bundle_sha256: str, phase: str, error: str
    ) -> None:
        """Persist a failed attempt even when topology was never safe to assign."""
        self.directory.mkdir(parents=True, exist_ok=True)
        timestamp = datetime.now(timezone.utc).isoformat()
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
            "error": error,
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
            "error": error,
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
