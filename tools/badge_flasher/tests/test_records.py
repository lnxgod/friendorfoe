from __future__ import annotations

import tempfile
import unittest
import csv
import json
from pathlib import Path

from tools.badge_flasher.models import (
    BatchResult,
    FlashEvidence,
    PassedFactoryRecord,
    TopologyAssignment,
)
from tools.badge_flasher.records import LedgerError, ManufacturingLedger


class LedgerTests(unittest.TestCase):
    def test_passed_records_returns_exact_current_assignment(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            rows = (
                {
                    "passed": False,
                    "assignment": None,
                    "error": "ignored failed attempt",
                },
                {
                    "passed": True,
                    "version": "0.67.2-badge-defcon34",
                    "bundle_sha256": "a" * 64,
                    "assignment": {
                        "uplink_mac": "aa:bb:cc:dd:ee:01",
                        "ble_leaf_mac": "aa:bb:cc:dd:ee:02",
                        "wifi_leaf_mac": "aa:bb:cc:dd:ee:03",
                    },
                    "game_seed": "normal",
                },
            )
            (root / "badge-factory.jsonl").write_text(
                "".join(json.dumps(row) + "\n" for row in rows),
                encoding="utf-8",
            )

            self.assertEqual(
                ManufacturingLedger(root).passed_records(),
                (
                    PassedFactoryRecord(
                        version="0.67.2-badge-defcon34",
                        bundle_sha256="a" * 64,
                        assignment=TopologyAssignment(
                            "AA:BB:CC:DD:EE:01",
                            "AA:BB:CC:DD:EE:02",
                            "AA:BB:CC:DD:EE:03",
                        ),
                        game_seed="normal",
                    ),
                ),
            )

    def test_passed_records_rejects_malformed_or_conflicting_passes(
        self,
    ) -> None:
        valid = {
            "passed": True,
            "version": "0.67.2-badge-defcon34",
            "bundle_sha256": "a" * 64,
            "assignment": {
                "uplink_mac": "AA:BB:CC:DD:EE:01",
                "ble_leaf_mac": "AA:BB:CC:DD:EE:02",
                "wifi_leaf_mac": "AA:BB:CC:DD:EE:03",
            },
            "game_seed": "normal",
        }
        invalid_rows = (
            {**valid, "bundle_sha256": "not-a-sha256"},
            {
                **valid,
                "assignment": {
                    "uplink_mac": "AA:BB:CC:DD:EE:01",
                    "ble_leaf_mac": "AA:BB:CC:DD:EE:02",
                },
            },
            {
                **valid,
                "assignment": {
                    "uplink_mac": "AA:BB:CC:DD:EE:01",
                    "ble_leaf_mac": "AA:BB:CC:DD:EE:03",
                    "wifi_leaf_mac": "AA:BB:CC:DD:EE:04",
                },
            },
        )
        for invalid in invalid_rows:
            with self.subTest(invalid=invalid):
                with tempfile.TemporaryDirectory() as temp:
                    root = Path(temp)
                    rows = (
                        (valid, invalid)
                        if invalid is invalid_rows[-1]
                        else (invalid,)
                    )
                    (root / "badge-factory.jsonl").write_text(
                        "".join(json.dumps(row) + "\n" for row in rows),
                        encoding="utf-8",
                    )
                    with self.assertRaises(LedgerError):
                        ManufacturingLedger(root).passed_records()

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            reassigned = {**valid, "game_seed": "immune", "phase": "reassign"}
            (root / "badge-factory.jsonl").write_text(
                json.dumps(valid) + "\n" + json.dumps(reassigned) + "\n",
                encoding="utf-8",
            )
            records = ManufacturingLedger(root).passed_records()
            self.assertEqual(
                [record.game_seed for record in records],
                ["normal", "immune"],
            )

    def test_passed_records_rejects_non_string_assignment_macs(
        self,
    ) -> None:
        valid_assignment = {
            "uplink_mac": "AA:BB:CC:DD:EE:01",
            "ble_leaf_mac": "AA:BB:CC:DD:EE:02",
            "wifi_leaf_mac": "AA:BB:CC:DD:EE:03",
        }
        for field in valid_assignment:
            with self.subTest(field=field):
                with tempfile.TemporaryDirectory() as temp:
                    root = Path(temp)
                    assignment = {
                        **valid_assignment,
                        field: 102030405060,
                    }
                    row = {
                        "passed": True,
                        "version": "0.67.2-badge-defcon34",
                        "bundle_sha256": "a" * 64,
                        "assignment": assignment,
                        "game_seed": "normal",
                    }
                    (root / "badge-factory.jsonl").write_text(
                        json.dumps(row) + "\n",
                        encoding="utf-8",
                    )
                    with self.assertRaises(LedgerError):
                        ManufacturingLedger(root).passed_records()

    def test_passed_macs_recovers_prior_pass_rows(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "badge-factory.csv").write_text(
                "timestamp,badge_id,version,passed,phase,uplink_mac,ble_mac,wifi_mac,bundle_sha256,error\n"
                "t,b,v,True,complete,aa:bb:cc:dd:ee:01,aa:bb:cc:dd:ee:02,aa:bb:cc:dd:ee:03,h,\n"
                "t,b,v,False,failed,aa:bb:cc:dd:ee:11,aa:bb:cc:dd:ee:12,aa:bb:cc:dd:ee:13,h,x\n",
                encoding="utf-8",
            )
            self.assertEqual(
                ManufacturingLedger(root).passed_macs(),
                {"AA:BB:CC:DD:EE:01", "AA:BB:CC:DD:EE:02", "AA:BB:CC:DD:EE:03"},
            )

    def test_pass_and_failure_both_append_jsonl_and_csv(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            ledger = ManufacturingLedger(root)
            assignment = TopologyAssignment(
                "AA:BB:CC:DD:EE:01", "AA:BB:CC:DD:EE:02", "AA:BB:CC:DD:EE:03"
            )
            ledger.record(BatchResult(
                badge_id="DDEE01",
                version="0.64.76-badge-defcon34",
                bundle_sha256="abc",
                passed=True,
                phase="complete",
                assignment=assignment,
                devices=(FlashEvidence(
                    assignment.uplink_mac, "uplink", "/dev/cu.x", "0.64.76-badge-defcon34", True, True
                ),),
                runtime={
                    "game_seed": "infected",
                    "game_state": "infected",
                    "game_active": False,
                    "game_shield": 0,
                },
                game_seed="infected",
                receipt="rcpt_K7M2Q9W4",
            ))
            ledger.record_failure(
                version="0.64.76-badge-defcon34",
                bundle_sha256="abc",
                phase="probe",
                error="bad graph",
                game_seed="immune",
            )
            json_rows = [json.loads(line) for line in (root / "badge-factory.jsonl").read_text().splitlines()]
            with (root / "badge-factory.csv").open(newline="", encoding="utf-8") as handle:
                csv_rows = list(csv.DictReader(handle))
            self.assertEqual([row["passed"] for row in json_rows], [True, False])
            self.assertEqual([row["passed"] for row in csv_rows], ["True", "False"])
            self.assertEqual(csv_rows[1]["error"], "bad graph")
            self.assertEqual(json_rows[0]["game_seed"], "infected")
            self.assertEqual(json_rows[0]["receipt"], "rcpt_K7M2Q9W4")
            self.assertEqual(json_rows[1]["game_seed"], "immune")
            self.assertIsNone(json_rows[1]["receipt"])
            self.assertEqual(
                list(csv_rows[0]),
                [
                    "timestamp",
                    "badge_id",
                    "version",
                    "passed",
                    "phase",
                    "uplink_mac",
                    "ble_mac",
                    "wifi_mac",
                    "bundle_sha256",
                    "error",
                ],
            )

    def test_failure_text_is_scrubbed_but_pass_mac_fields_remain_private(
        self,
    ) -> None:
        macs = (
            "A0:B1:C2:D3:E4:F5",
            "A0:B1:C2:D3:E4:F6",
            "A0:B1:C2:D3:E4:F7",
        )
        raw_error = (
            "colon A0:B1:C2:D3:E4:F5 "
            "hyphen A0-B1-C2-D3-E4-F5 "
            "dotted A0B1.C2D3.E4F5 "
            "compact A0B1C2D3E4F5"
        )
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            ledger = ManufacturingLedger(root)
            assignment = TopologyAssignment(*macs)
            ledger.record(BatchResult(
                badge_id="D3E4F5",
                version="0.64.76-badge-defcon34",
                bundle_sha256="abc",
                passed=True,
                phase="complete",
                assignment=assignment,
                devices=(),
                runtime={
                    "game_seed": "normal",
                    "game_state": "normal",
                    "game_active": False,
                    "game_shield": 0,
                },
                game_seed="normal",
                receipt="rcpt_01234567",
            ))
            ledger.record_failure(
                version="0.64.76-badge-defcon34",
                bundle_sha256="abc",
                phase="factory",
                error=raw_error,
                game_seed="infected",
            )

            json_rows = [
                json.loads(line)
                for line in (
                    root / "badge-factory.jsonl"
                ).read_text(encoding="utf-8").splitlines()
            ]
            with (root / "badge-factory.csv").open(
                newline="",
                encoding="utf-8",
            ) as handle:
                csv_rows = list(csv.DictReader(handle))

        self.assertEqual(
            json_rows[0]["assignment"],
            {
                "uplink_mac": macs[0],
                "ble_leaf_mac": macs[1],
                "wifi_leaf_mac": macs[2],
            },
        )
        self.assertEqual(
            (
                csv_rows[0]["uplink_mac"],
                csv_rows[0]["ble_mac"],
                csv_rows[0]["wifi_mac"],
            ),
            macs,
        )
        expected_error = (
            "colon [hardware-id] "
            "hyphen [hardware-id] "
            "dotted [hardware-id] "
            "compact [hardware-id]"
        )
        self.assertEqual(json_rows[1]["error"], expected_error)
        self.assertEqual(csv_rows[1]["error"], expected_error)

    def test_record_scrubs_failed_batch_error_without_rewriting_mac_fields(
        self,
    ) -> None:
        macs = (
            "A0:B1:C2:D3:E4:F5",
            "A0:B1:C2:D3:E4:F6",
            "A0:B1:C2:D3:E4:F7",
        )
        raw_error = (
            "colon A0:B1:C2:D3:E4:F5 "
            "hyphen A0-B1-C2-D3-E4-F5 "
            "dotted A0B1.C2D3.E4F5 "
            "compact A0B1C2D3E4F5"
        )
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            assignment = TopologyAssignment(*macs)
            ManufacturingLedger(root).record(BatchResult(
                badge_id="D3E4F5",
                version="0.64.76-badge-defcon34",
                bundle_sha256="abc",
                passed=False,
                phase="factory",
                assignment=assignment,
                devices=(),
                runtime={},
                game_seed="immune",
                receipt=None,
                error=raw_error,
            ))
            json_row = json.loads(
                (root / "badge-factory.jsonl").read_text(
                    encoding="utf-8",
                )
            )
            with (root / "badge-factory.csv").open(
                newline="",
                encoding="utf-8",
            ) as handle:
                csv_row = next(csv.DictReader(handle))

        expected_error = (
            "colon [hardware-id] "
            "hyphen [hardware-id] "
            "dotted [hardware-id] "
            "compact [hardware-id]"
        )
        self.assertEqual(json_row["error"], expected_error)
        self.assertEqual(csv_row["error"], expected_error)
        self.assertEqual(
            json_row["assignment"],
            {
                "uplink_mac": macs[0],
                "ble_leaf_mac": macs[1],
                "wifi_leaf_mac": macs[2],
            },
        )
        self.assertEqual(
            (
                csv_row["uplink_mac"],
                csv_row["ble_mac"],
                csv_row["wifi_mac"],
            ),
            macs,
        )


if __name__ == "__main__":
    unittest.main()
