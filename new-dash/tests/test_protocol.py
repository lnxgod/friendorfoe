import json
import pathlib
import unittest

from new_dash.models import BadgeStatus, ControlReply, DetectionEvent, SOURCE_NAMES
from new_dash.protocol import LineFramer, MachineFrameError, parse_machine_line


class SourceMappingTest(unittest.TestCase):
    def test_remote_id_sources_are_not_conflated_with_dji(self) -> None:
        self.assertEqual(SOURCE_NAMES[0], "ble_rid")
        self.assertEqual(SOURCE_NAMES[2], "wifi_dji_ie")
        self.assertEqual(SOURCE_NAMES[3], "wifi_rid")


class FramingTest(unittest.TestCase):
    def test_chunked_crlf_and_logs_yield_only_complete_lines(self) -> None:
        framer = LineFramer(max_line_bytes=128)
        self.assertEqual(framer.feed(b"I (12) boot\r\nFOF_PO"), ["I (12) boot"])
        self.assertEqual(
            framer.feed(b"NG:v1\n\rFOF_DET:{}\r"),
            ["FOF_PONG:v1", "FOF_DET:{}"],
        )

    def test_overlong_line_is_dropped_until_delimiter(self) -> None:
        framer = LineFramer(max_line_bytes=5)
        self.assertEqual(framer.feed(b"123456789\nOK\n"), ["OK"])
        self.assertEqual(framer.overlong_lines, 1)

    def test_65536_byte_line_is_accepted_and_next_byte_is_dropped(self) -> None:
        framer = LineFramer()
        accepted = b"a" * 65536 + b"\r\n"
        self.assertEqual(framer.feed(accepted), ["a" * 65536])
        self.assertEqual(framer.feed(b"b" * 65537 + b"\r\nOK\n"), ["OK"])
        self.assertEqual(framer.overlong_lines, 1)

    def test_invalid_utf8_is_replaced_and_counted_once_per_line(self) -> None:
        framer = LineFramer()
        self.assertEqual(framer.feed(b"good\xff\n"), ["good\ufffd"])
        self.assertEqual(framer.decode_errors, 1)


class DetectionParsingTest(unittest.TestCase):
    def test_parses_detection_with_explicit_remote_id_source(self) -> None:
        frame = parse_machine_line(
            'FOF_DET:{"id":"RID-7","manufacturer":"DJI",'
            '"badge_label":"REMOTE ID","badge_class":"drone",'
            '"badge_entity_key":"rid:RID-7","source":0,'
            '"confidence":0.95,"threat_score":77.5,"rssi":-57}'
        )
        self.assertIsInstance(frame.value, DetectionEvent)
        self.assertEqual(frame.value.source, "ble_rid")
        self.assertEqual(frame.value.stable_key, "rid:RID-7")

    def test_future_integer_source_is_retained_without_dji_conflation(self) -> None:
        frame = parse_machine_line('FOF_DET:{"id":"future","source":42}')
        self.assertEqual(frame.value.source_id, 42)
        self.assertEqual(frame.value.source, "unknown_42")
        self.assertEqual(frame.value.stable_key, "unknown_42:future")

    def test_recognized_bad_json_raises_but_log_line_is_ignored(self) -> None:
        self.assertIsNone(parse_machine_line("I (55) scanner: alive"))
        with self.assertRaises(MachineFrameError):
            parse_machine_line("FOF_DET:{bad")


class StatusParsingTest(unittest.TestCase):
    def test_status_accepts_factory_startup_status_without_uptime(self) -> None:
        fixture = pathlib.Path(__file__).parent / "fixtures" / "badge_status_factory_0_67_2_startup.json"
        frame = parse_machine_line(f"FOF_STATUS:{fixture.read_text()}")
        self.assertEqual(frame.value.version, "0.67.2-badge-defcon34")
        self.assertIsNone(frame.value.uptime_seconds)
        self.assertEqual(frame.value.recovery_mode, "startup_dependency")
        self.assertIsNone(frame.value.entities)
        self.assertIsNone(frame.value.scanners)
        self.assertEqual(frame.value.to_dict()["usb_health"]["schema"], 1)

    def test_status_normalizes_positioned_remote_id_entity(self) -> None:
        fixture = pathlib.Path(__file__).parent / "fixtures" / "badge_status_remote_id.json"
        frame = parse_machine_line(f"FOF_STATUS:{fixture.read_text()}")
        self.assertIsInstance(frame.value, BadgeStatus)
        entity = frame.value.entities[0]
        self.assertTrue(entity.is_remote_id)
        self.assertTrue(entity.has_position)
        self.assertEqual(entity.operator_latitude, 37.7754)
        self.assertEqual(entity.operator_longitude, -122.4188)
        self.assertIsNone(entity.ssid)

    def test_invalid_coordinate_does_not_discard_entity(self) -> None:
        fixture = pathlib.Path(__file__).parent / "fixtures" / "badge_status_remote_id.json"
        payload = json.loads(fixture.read_text())
        payload["entities"][0]["lat"] = 91
        frame = parse_machine_line(f"FOF_STATUS:{json.dumps(payload)}")
        entity = frame.value.entities[0]
        self.assertFalse(entity.has_position)
        self.assertIsNone(entity.latitude)
        self.assertIsNone(entity.longitude)
        self.assertEqual(entity.label, "RID-ABC123")

    def test_unpaired_operator_coordinates_are_normalized_as_unavailable(self) -> None:
        frame = parse_machine_line(
            'FOF_STATUS:{"version":"v1","uptime_s":1,"entities":['
            '{"source":"ble_rid","operator_lat":37.7}]}'
        )

        entity = frame.value.entities[0]

        self.assertIsNone(entity.operator_latitude)
        self.assertIsNone(entity.operator_longitude)

    def test_status_rejects_invalid_roots_and_nonfinite_json(self) -> None:
        invalid_payloads = (
            {},
            {"version": "", "uptime_s": 0},
            {"version": "v1", "uptime_s": True},
            {"version": "v1", "uptime_s": None},
            {"version": "v1", "uptime_s": float("inf")},
            {"version": "v1", "uptime_s": 0, "entities": {}},
            {"version": "v1", "uptime_s": 0, "scanners": {}},
        )
        for payload in invalid_payloads:
            with self.subTest(payload=payload):
                with self.assertRaises(MachineFrameError):
                    parse_machine_line(f"FOF_STATUS:{json.dumps(payload)}")
        for token in ("NaN", "Infinity", "-Infinity"):
            with self.subTest(token=token):
                with self.assertRaises(MachineFrameError):
                    parse_machine_line(f'FOF_STATUS:{{"version":"v1","uptime_s":{token}}}')

    def test_status_normalizes_numeric_overflow_and_excessive_nesting(self) -> None:
        huge_integer = "9" * 400
        deep_value = "[" * 500 + "0" + "]" * 500
        invalid_lines = (
            f'FOF_STATUS:{{"version":"v1","uptime_s":{huge_integer},"entities":[]}}',
            'FOF_STATUS:{"version":"v1","uptime_s":1e309,"entities":[]}',
            f'FOF_STATUS:{{"version":"v1","uptime_s":0,"future":{deep_value}}}',
        )

        for line in invalid_lines:
            with self.subTest(case=line[:80]):
                with self.assertRaises(MachineFrameError):
                    parse_machine_line(line)

    def test_status_accepts_additive_objects(self) -> None:
        frame = parse_machine_line(
            'FOF_STATUS:{"version":"v1","uptime_s":1,"extra":{"future":true},'
            '"entities":[{"label":"added","nested":{"value":1}}],'
            '"scanners":[{"connected":true,"next":"field"}]}'
        )
        self.assertEqual(frame.value.version, "v1")
        self.assertEqual(frame.value.entities[0].label, "added")

    def test_status_preserves_missing_arrays_separately_from_explicit_empty_arrays(self) -> None:
        missing = parse_machine_line('FOF_STATUS:{"version":"v1","uptime_s":1}').value
        explicit = parse_machine_line(
            'FOF_STATUS:{"version":"v1","uptime_s":1,"entities":[],"scanners":[]}'
        ).value

        self.assertIsNone(missing.entities)
        self.assertIsNone(missing.scanners)
        self.assertNotIn("entities", missing.to_dict())
        self.assertNotIn("scanners", missing.to_dict())
        self.assertEqual(explicit.entities, ())
        self.assertEqual(explicit.scanners, ())
        self.assertEqual(explicit.to_dict()["entities"], [])
        self.assertEqual(explicit.to_dict()["scanners"], [])


class ControlReplyParsingTest(unittest.TestCase):
    def test_parses_control_success_and_error(self) -> None:
        success = parse_machine_line('FOF_CTL_OK:{"message":"display updated"}')
        failure = parse_machine_line('FOF_CTL_ERROR:{"error":"rejected"}')
        self.assertIsInstance(success.value, ControlReply)
        self.assertTrue(success.value.ok)
        self.assertFalse(failure.value.ok)
        self.assertEqual(failure.value.error, "rejected")
