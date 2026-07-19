import re
import unittest
from pathlib import Path


SCANNER_ROOT = Path(__file__).resolve().parents[1]
REPO_ESP32 = SCANNER_ROOT.parent


def source(path: Path) -> str:
    return path.read_text(encoding="utf-8")


class ScannerQuietSourceContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.protocol = source(REPO_ESP32 / "shared" / "uart_protocol.h")
        cls.main = source(SCANNER_ROOT / "main" / "main.c")
        cls.uart_tx = source(SCANNER_ROOT / "main" / "comms" / "uart_tx.c")
        cls.investigator = source(
            SCANNER_ROOT / "main" / "detection" / "ble_investigator.c"
        )
        cls.ble_remote_id = source(
            SCANNER_ROOT / "main" / "detection" / "ble_remote_id.c"
        )
        cls.wifi_scanner = source(
            SCANNER_ROOT / "main" / "detection" / "wifi_scanner.c"
        )
        cls.calibration_mode = source(
            SCANNER_ROOT / "main" / "core" / "calibration_mode.c"
        )

    def test_protocol_names_are_shared_constants(self) -> None:
        self.assertIn(
            '#define MSG_TYPE_SCANNER_QUIET      "scanner_quiet"',
            self.protocol,
        )
        self.assertIn(
            '#define MSG_TYPE_SCANNER_QUIET_ACK  "scanner_quiet_ack"',
            self.protocol,
        )
        self.assertIn('#define JSON_KEY_GENERATION', self.protocol)

    def test_command_requires_bool_and_generation_and_acks_live_state(self) -> None:
        handler = re.search(
            r"static void handle_scanner_quiet_command\(cJSON \*root\)(.*?)\n}\n",
            self.main,
            re.S,
        )
        self.assertIsNotNone(handler, "scanner quiet command handler missing")
        body = handler.group(1)
        self.assertIn("cJSON_IsBool", body)
        self.assertIn("JSON_KEY_GENERATION", body)
        self.assertIn("scanner_quiet_mode_set", body)
        for field in (
            '"generation"',
            '"enabled"',
            '"ble_scanning"',
            '"wifi_paused"',
            '"ble_quiesced"',
            '"wifi_quiesced"',
            '"ble_active"',
            '"wifi_active"',
            '"radios_ready"',
            '"tx_restored"',
            '"tx_enabled"',
            '"uart_commands"',
        ):
            self.assertIn(field, body)
        self.assertIn("MSG_TYPE_SCANNER_QUIET_ACK", body)
        self.assertIn("ble_investigator_runtime_quiesce", body)
        self.assertIn("s_radios_ready", body)
        self.assertNotIn("ble_investigation_active", body)
        self.assertRegex(
            self.main,
            r"strcmp\(type, MSG_TYPE_SCANNER_QUIET\) == 0",
        )

    def test_investigation_cleanup_cannot_restart_ble_while_quiet(self) -> None:
        self.assertIn("ble_investigator_runtime_quiesce", self.investigator)
        resume = re.search(
            r"if \(action == BLE_INV_CLEANUP_RESUME_SCAN\)(.*?)\n        }",
            self.investigator,
            re.S,
        )
        self.assertIsNotNone(resume)
        self.assertIn("scanner_quiet_mode_is_active()", resume.group(1))

        resume_api = re.search(
            r"bool ble_remote_id_resume_after_investigation\(void\)(.*?)\n}",
            self.ble_remote_id,
            re.S,
        )
        self.assertIsNotNone(resume_api)
        self.assertIn("scanner_quiet_mode_is_active()", resume_api.group(1))

        scan_start = re.search(
            r"static bool ble_remote_id_start_scan_internal\(bool investigation_resume\)"
            r"\s*\{(.*?)\n}",
            self.ble_remote_id,
            re.S,
        )
        self.assertIsNotNone(scan_start)
        self.assertIn("scanner_quiet_mode_is_active()", scan_start.group(1))

    def test_all_scanner_restart_paths_honor_authoritative_quiet_state(self) -> None:
        self.assertRegex(
            self.main,
            r"(?s)strcmp\(type, \"ready\"\).*?scanner_quiet_mode_is_active\(\)",
        )
        auto_start = re.search(
            r"if \(!uart_tx_is_enabled\(\).*?BADGE_DEBUG auto-started",
            self.main,
            re.S,
        )
        self.assertIsNotNone(auto_start)
        self.assertIn("!scanner_quiet_mode_is_active()", auto_start.group(0))

        maintenance = re.search(
            r"bool ota_active = uart_ota_is_active_snapshot\(\);(.*?)"
            r"ble_remote_id_meta_reacquire_tick\((.*?)\);",
            self.uart_tx,
            re.S,
        )
        self.assertIsNotNone(maintenance)
        self.assertIn("if (!ota_active)", maintenance.group(1))
        self.assertIn("scanner_scan_profile_apply()", maintenance.group(1))
        self.assertIn("!scanner_quiet_mode_is_active()", maintenance.group(2))
        self.assertIn("!ota_active", maintenance.group(2))

        normal_profile = re.search(
            r"static void apply_normal_profile_radios_locked\(void\)(.*?)\n}",
            self.calibration_mode,
            re.S,
        )
        self.assertIsNotNone(normal_profile)
        self.assertIn("s_quiet_mode", normal_profile.group(1))
        self.assertIn("apply_quiet_radios()", normal_profile.group(1))

        tx_gate = re.search(
            r"void uart_tx_set_enabled\(bool enabled\)(.*?)\n}",
            self.uart_tx,
            re.S,
        )
        self.assertIsNotNone(tx_gate)
        self.assertIn("scanner_quiet_mode_is_active()", tx_gate.group(1))

        badge_start = re.search(
            r"#ifdef FOF_BADGE_VARIANT\s*/\*.*?Badge: let NimBLE sync.*?#else",
            self.main,
            re.S,
        )
        self.assertIsNotNone(badge_start)
        self.assertGreaterEqual(
            badge_start.group(0).count("scanner_quiet_mode_is_active()"),
            2,
        )

        wifi_init = re.search(
            r"void wifi_scanner_init\(QueueHandle_t detection_queue\)(.*?)\n}",
            self.wifi_scanner,
            re.S,
        )
        self.assertIsNotNone(wifi_init)
        self.assertIn(
            "esp_wifi_set_promiscuous(!s_wifi_scan_paused)",
            wifi_init.group(1),
        )
        self.assertRegex(
            wifi_init.group(1),
            r"(?s)s_wifi_initialized = true;.*?if \(s_wifi_scan_paused\)"
            r".*?esp_wifi_set_promiscuous\(false\)",
        )

    def test_quiet_is_reported_and_can_validate_a_uart_healthy_ota_image(self) -> None:
        radio_health = re.search(
            r"static bool scanner_radio_health_ok\(void\)(.*?)\n}",
            self.main,
            re.S,
        )
        self.assertIsNotNone(radio_health)
        self.assertIn("scanner_quiet_mode_is_active()", radio_health.group(1))
        self.assertIn("s_radios_ready", radio_health.group(1))
        self.assertIn("g_cmd_msg_count", radio_health.group(1))
        self.assertIn("s_uart_cmd_last_loop_ms", radio_health.group(1))
        self.assertIn("wifi_scanner_is_quiesced()", radio_health.group(1))
        self.assertIn("ble_remote_id_is_quiesced()", radio_health.group(1))

        self.assertIn('\\"quiet_mode\\":%s', self.uart_tx)
        self.assertIn('\\"quiet_generation\\":%lu', self.uart_tx)
        self.assertIn('\\"tx_enabled\\":%s', self.uart_tx)
        self.assertIn("scanner_quiet_mode_is_active()", self.uart_tx)
        self.assertIn("scanner_quiet_mode_generation()", self.uart_tx)
        self.assertIn("uart_tx_is_enabled()", self.uart_tx)

    def test_quiet_ack_requires_confirmed_radio_idle_barriers(self) -> None:
        calibration = source(
            SCANNER_ROOT / "main" / "core" / "calibration_mode.c"
        )
        quiet_setter = re.search(
            r"bool scanner_quiet_mode_set\(bool enabled, uint32_t generation\)"
            r"(.*?)\n}",
            calibration,
            re.S,
        )
        self.assertIsNotNone(quiet_setter)
        self.assertIn("wifi_scanner_is_quiesced()", quiet_setter.group(1))
        self.assertIn("ble_remote_id_is_quiesced()", quiet_setter.group(1))
        self.assertRegex(
            quiet_setter.group(1),
            r"(?s)apply_quiet_radios\(\).*?radios_quiesced.*?"
            r"uart_tx_flush_detection_queue\(\)",
        )
        self.assertRegex(
            quiet_setter.group(1),
            r"(?s)apply_normal_profile_radios.*?"
            r"uart_tx_flush_detection_queue\(\).*?uart_tx_set_enabled",
        )

        self.assertIn("xSemaphoreTake", calibration)
        self.assertIn("xSemaphoreGive", calibration)
        profile_apply = re.search(
            r"void scanner_scan_profile_apply\(void\)(.*?)\n}",
            calibration,
            re.S,
        )
        self.assertIsNotNone(profile_apply)
        self.assertIn("scanner_transition_lock", profile_apply.group(1))

        self.assertIn("esp_wifi_scan_stop()", self.wifi_scanner)
        self.assertIn("s_active_scan_work", self.wifi_scanner)
        self.assertIn("esp_wifi_get_promiscuous", self.wifi_scanner)
        fast_rescan = re.search(
            r"static void do_fast_rescan\(void\)(.*?)\n}",
            self.wifi_scanner,
            re.S,
        )
        self.assertIsNotNone(fast_rescan)
        self.assertIn("s_wifi_scan_paused", fast_rescan.group(1))

        ble_quiesced = re.search(
            r"bool ble_remote_id_is_quiesced\(void\)(.*?)\n}",
            self.ble_remote_id,
            re.S,
        )
        self.assertIsNotNone(ble_quiesced)
        self.assertIn("s_host_task_active", ble_quiesced.group(1))
        self.assertIn("s_host_task_requested", ble_quiesced.group(1))
        self.assertIn("s_scanning", ble_quiesced.group(1))
        self.assertIn("s_start_inflight", ble_quiesced.group(1))
        self.assertIn("s_resume_inflight", self.wifi_scanner)
        self.assertIn("atomic_bool s_wifi_scan_paused", self.wifi_scanner)
        self.assertIn("atomic_bool s_active_scan_work", self.wifi_scanner)
        self.assertIn("atomic_bool      s_scanning", self.ble_remote_id)

        self.assertIn("wifi_scanner_is_active()", calibration)
        self.assertIn("ble_remote_id_is_active()", calibration)
        self.assertIn("s_wake_pending", calibration)
        self.assertIn("atomic_bool s_tx_enabled", self.uart_tx)
        self.assertIn("atomic_load_explicit(&s_tx_enabled", self.uart_tx)
        self.assertIn("atomic_store_explicit(&s_tx_enabled", self.uart_tx)

    def test_detection_status_and_easter_tx_recheck_after_quiet_convergence(self) -> None:
        gate = re.search(
            r"static bool scanner_data_tx_allowed\(void\)(.*?)\n}",
            self.uart_tx,
            re.S,
        )
        self.assertIsNotNone(gate)
        self.assertIn("s_tx_enabled", gate.group(1))
        self.assertIn("scanner_quiet_mode_is_active()", gate.group(1))

        data_line = re.search(
            r"static bool uart_send_scanner_data_line\(const char \*json_str\)"
            r"(.*?)\n}",
            self.uart_tx,
            re.S,
        )
        self.assertIsNotNone(data_line)
        self.assertIn("uart_send_line_internal(json_str, true)", data_line.group(1))
        line_internal = re.search(
            r"static bool uart_send_line_internal\(.*?\)(.*?)\n}",
            self.uart_tx,
            re.S,
        )
        self.assertIsNotNone(line_internal)
        self.assertIn("scanner_data_tx_allowed()", line_internal.group(1))
        self.assertGreaterEqual(
            self.uart_tx.count("uart_send_scanner_data_line("),
            4,
        )

        tx_task = re.search(
            r"static void uart_tx_task\(void \*arg\)(.*?)\n}",
            self.uart_tx,
            re.S,
        )
        self.assertIsNotNone(tx_task)
        self.assertRegex(
            tx_task.group(1),
            r"(?s)xQueueReceive\([^;]+\).*?scanner_data_tx_allowed\(\)",
        )
        easter = re.search(
            r"uint32_t easter_pending = atomic_exchange_explicit\((.*?)#endif",
            tx_task.group(1),
            re.S,
        )
        self.assertIsNotNone(easter)
        self.assertIn("uart_send_scanner_data_line", easter.group(1))
        self.assertNotIn("uart_tx_send_raw_json", easter.group(1))


if __name__ == "__main__":
    unittest.main()
