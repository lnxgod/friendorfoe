from __future__ import annotations

import tempfile
import unittest
import csv
import json
from pathlib import Path

from tools.badge_flasher.models import BatchResult, FlashEvidence, TopologyAssignment
from tools.badge_flasher.records import ManufacturingLedger


class LedgerTests(unittest.TestCase):
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
                runtime={"ok": True},
            ))
            ledger.record_failure(
                version="0.64.76-badge-defcon34",
                bundle_sha256="abc",
                phase="probe",
                error="bad graph",
            )
            json_rows = [json.loads(line) for line in (root / "badge-factory.jsonl").read_text().splitlines()]
            with (root / "badge-factory.csv").open(newline="", encoding="utf-8") as handle:
                csv_rows = list(csv.DictReader(handle))
            self.assertEqual([row["passed"] for row in json_rows], [True, False])
            self.assertEqual([row["passed"] for row in csv_rows], ["True", "False"])
            self.assertEqual(csv_rows[1]["error"], "bad graph")


if __name__ == "__main__":
    unittest.main()
