from __future__ import annotations

import unittest

from tools.badge_flasher.models import TopologyAssignment
from tools.badge_flasher.verify import VerificationError, runtime_evidence, verify_status


def valid_status():
    return {"version":"0.64.76-badge-defcon34","firmware_name":"uplink-s3-fof_badge","app_project":"fof_badge_uplink","hardware_type":"seeed_xiao_esp32s3","safe_mode":False,"recovery_mode":"normal","usb_control_alive":True,"scanner_uart_alive":True,"display_alive":True,"power_converged":True,"scanner_power_converged":True,"psram_total":8388608,"scanners":[
        {"uart":"ble","connected":True,"firmware_name":"scanner-s3-combo-fof_badge","app_project":"fof_badge_scanner","hardware_type":"seeed_xiao_esp32s3","ver":"0.64.76-badge-defcon34","hardware_id":"E0:72:A1:F9:49:84","role_acked":True,"scan_profile":"ble_primary","ble_scanning":True,"recovery_mode":"normal","rollback_pending":False,"health":"ok"},
        {"uart":"wifi","connected":True,"firmware_name":"scanner-s3-combo-fof_badge","app_project":"fof_badge_scanner","hardware_type":"seeed_xiao_esp32s3","ver":"0.64.76-badge-defcon34","hardware_id":"E0:72:A1:F8:4C:58","role_acked":True,"scan_profile":"wifi_primary","wifi_active":True,"recovery_mode":"normal","rollback_pending":False,"health":"ok"},
    ]}


class VerifyTests(unittest.TestCase):
    def test_runtime_host_path_never_calls_blocking_serial_flush(self):
        source = __import__("inspect").getsource(
            __import__("tools.badge_flasher.verify", fromlist=["*"])
        )
        self.assertNotIn("handle.flush()", source)

    def test_accepts_exact_runtime_graph(self):
        assignment=TopologyAssignment("E0:72:A1:F9:47:FC","E0:72:A1:F9:49:84","E0:72:A1:F8:4C:58")
        self.assertEqual(verify_status(valid_status(), assignment, "0.64.76-badge-defcon34")["safe_mode"], False)
    def test_rejects_wrong_scanner_identity(self):
        status=valid_status(); status["scanners"][0]["hardware_id"]="E0:72:A1:00:00:01"
        with self.assertRaisesRegex(VerificationError,"MAC mismatch"):
            verify_status(status,TopologyAssignment("E0:72:A1:F9:47:FC","E0:72:A1:F9:49:84","E0:72:A1:F8:4C:58"),"0.64.76-badge-defcon34")

    def test_manufacturing_evidence_excludes_nearby_detections(self):
        status = valid_status()
        status["entities"] = [{"bssid": "AA:BB:CC:DD:EE:FF"}]
        evidence = runtime_evidence(status)
        self.assertNotIn("entities", evidence)
        self.assertNotIn("bssid", str(evidence))

    def test_missing_fail_closed_fields_never_pass(self):
        assignment = TopologyAssignment("E0:72:A1:F9:47:FC","E0:72:A1:F9:49:84","E0:72:A1:F8:4C:58")
        for field in ("safe_mode", "recovery_mode", "usb_control_alive", "scanner_uart_alive"):
            status = valid_status()
            status.pop(field)
            with self.subTest(field=field), self.assertRaises(VerificationError):
                verify_status(status, assignment, "0.64.76-badge-defcon34")
        for field in ("rollback_pending", "recovery_mode"):
            status = valid_status()
            status["scanners"][0].pop(field)
            with self.subTest(scanner_field=field), self.assertRaises(VerificationError):
                verify_status(status, assignment, "0.64.76-badge-defcon34")


if __name__ == "__main__": unittest.main()
