from __future__ import annotations

import json
import unittest
import zlib

from tools.badge_flasher.models import ProbeReport, TopologyAssignment
from tools.badge_flasher.topology import (
    PROBE_PREFIX,
    TopologyError,
    classify_topology,
    parse_probe_report,
)


SESSION = "0123456789abcdef0123456789abcdef"
CENTER = "E0:72:A1:00:00:01"
BLE = "E0:72:A1:00:00:02"
WIFI = "E0:72:A1:00:00:03"


def report_line(*, session: str = SESSION, mac: str = CENTER,
                peers: dict[str, str] | None = None,
                schema: int = 1) -> str:
    payload: dict[str, object] = {
        "schema": schema,
        "session": session,
        "mac": mac,
        "peers": peers or {},
    }
    canonical = json.dumps(
        payload, sort_keys=True, separators=(",", ":")
    ).encode("ascii")
    payload["crc32"] = f"{zlib.crc32(canonical) & 0xFFFFFFFF:08x}"
    return PROBE_PREFIX + json.dumps(
        payload, sort_keys=True, separators=(",", ":")
    )


class ProbeReportParserTests(unittest.TestCase):
    def test_parses_and_normalizes_a_valid_report(self) -> None:
        parsed = parse_probe_report(
            report_line(
                mac="e0-72-a1-00-00-01",
                peers={"a": "e072a1000002", "b": WIFI.lower()},
            ),
            expected_session=SESSION,
        )

        self.assertEqual(
            parsed,
            ProbeReport(
                mac=CENTER,
                session=SESSION,
                peers={"a": BLE, "b": WIFI},
            ),
        )

    def test_rejects_wrong_prefix_schema_or_session(self) -> None:
        with self.assertRaisesRegex(TopologyError, "prefix"):
            parse_probe_report("garbage", expected_session=SESSION)
        with self.assertRaisesRegex(TopologyError, "schema"):
            parse_probe_report(
                report_line(schema=2), expected_session=SESSION
            )
        with self.assertRaisesRegex(TopologyError, "session"):
            parse_probe_report(
                report_line(session="f" * 32), expected_session=SESSION
            )

    def test_rejects_bad_crc_unknown_fields_and_invalid_macs(self) -> None:
        line = report_line(peers={"a": BLE})
        with self.assertRaisesRegex(TopologyError, "CRC"):
            parse_probe_report(line.replace('"crc32":"', '"crc32":"0'), SESSION)

        payload = json.loads(line[len(PROBE_PREFIX):])
        payload["extra"] = True
        with self.assertRaisesRegex(TopologyError, "fields"):
            parse_probe_report(PROBE_PREFIX + json.dumps(payload), SESSION)

        with self.assertRaisesRegex(TopologyError, "MAC"):
            parse_probe_report(report_line(mac="not-a-mac"), SESSION)

    def test_rejects_self_peer_duplicate_peer_and_unknown_link(self) -> None:
        with self.assertRaisesRegex(TopologyError, "self"):
            parse_probe_report(
                report_line(peers={"a": CENTER}), SESSION
            )
        with self.assertRaisesRegex(TopologyError, "distinct"):
            parse_probe_report(
                report_line(peers={"a": BLE, "b": BLE}), SESSION
            )
        with self.assertRaisesRegex(TopologyError, "link"):
            parse_probe_report(
                report_line(peers={"c": BLE}), SESSION
            )


class TopologyClassificationTests(unittest.TestCase):
    def reports(self) -> list[ProbeReport]:
        return [
            ProbeReport(CENTER, SESSION, {"a": BLE, "b": WIFI}),
            ProbeReport(BLE, SESSION, {"a": CENTER}),
            ProbeReport(WIFI, SESSION, {"a": CENTER}),
        ]

    def test_classifies_reciprocal_badge_star(self) -> None:
        self.assertEqual(
            classify_topology(self.reports()),
            TopologyAssignment(
                uplink_mac=CENTER,
                ble_leaf_mac=BLE,
                wifi_leaf_mac=WIFI,
            ),
        )

    def test_order_does_not_change_assignment(self) -> None:
        reports = self.reports()
        self.assertEqual(
            classify_topology([reports[2], reports[0], reports[1]]),
            TopologyAssignment(CENTER, BLE, WIFI),
        )

    def test_rejects_wrong_count_duplicate_mac_and_mixed_session(self) -> None:
        with self.assertRaisesRegex(TopologyError, "exactly three"):
            classify_topology(self.reports()[:2])

        reports = self.reports()
        reports[2] = ProbeReport(BLE, SESSION, {"a": CENTER})
        with self.assertRaisesRegex(TopologyError, "duplicate"):
            classify_topology(reports)

        reports = self.reports()
        reports[2] = ProbeReport(WIFI, "f" * 32, {"a": CENTER})
        with self.assertRaisesRegex(TopologyError, "session"):
            classify_topology(reports)

    def test_rejects_partial_nonreciprocal_or_unknown_peer_graph(self) -> None:
        reports = self.reports()
        reports[1] = ProbeReport(BLE, SESSION, {})
        with self.assertRaisesRegex(TopologyError, "reciprocal"):
            classify_topology(reports)

        reports = self.reports()
        reports[0] = ProbeReport(CENTER, SESSION, {"a": BLE})
        with self.assertRaisesRegex(TopologyError, "two peers"):
            classify_topology(reports)

        reports = self.reports()
        reports[0] = ProbeReport(
            CENTER, SESSION, {"a": BLE, "b": "E0:72:A1:00:00:99"}
        )
        with self.assertRaisesRegex(TopologyError, "unknown peer"):
            classify_topology(reports)

    def test_rejects_multiple_centers_or_leaf_using_link_b(self) -> None:
        reports = [
            ProbeReport(CENTER, SESSION, {"a": BLE, "b": WIFI}),
            ProbeReport(BLE, SESSION, {"a": CENTER, "b": WIFI}),
            ProbeReport(WIFI, SESSION, {"a": CENTER}),
        ]
        with self.assertRaisesRegex(TopologyError, "one two-peer"):
            classify_topology(reports)

        reports = self.reports()
        reports[1] = ProbeReport(BLE, SESSION, {"b": CENTER})
        with self.assertRaisesRegex(TopologyError, "link A"):
            classify_topology(reports)


if __name__ == "__main__":
    unittest.main()
