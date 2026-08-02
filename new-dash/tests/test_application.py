from __future__ import annotations

import json
import tempfile
import threading
import time
import unittest
from dataclasses import replace
from pathlib import Path

from new_dash.application import NewDashApplication
from new_dash.controls import ControlValidationError
from new_dash.models import BadgeStatus, ControlReply, DetectionEvent, MachineFrame
from new_dash.serial_transport import ConnectionUpdate, TransportUnavailable
from new_dash.storage import HistoryQuery, ObservationStore
from tests.test_controls import THEME, complete_policy


class FakeTransport:
    def __init__(self) -> None:
        self.commands: list[object] = []

    def send_control(self, command: object, timeout: float = 5.0) -> ControlReply:
        self.commands.append(command)
        return ControlReply.from_payload(
            {"message": getattr(command, "expected_message")}, ok=True
        )


class FailingEventStore(ObservationStore):
    def add_event(self, event: DetectionEvent, received_at: float) -> int:
        raise RuntimeError("database failed " + "x" * 400)


class BlockingPruneStore(ObservationStore):
    def __init__(self, *args: object, **kwargs: object) -> None:
        self.prune_started = threading.Event()
        self.release_prune = threading.Event()
        super().__init__(*args, **kwargs)

    def prune(self, now: float | None = None) -> int:
        self.prune_started.set()
        self.release_prune.wait(3.0)
        return super().prune(now)


class CountingPruneStore(ObservationStore):
    def __init__(self, *args: object, **kwargs: object) -> None:
        self.prune_calls: list[float | None] = []
        super().__init__(*args, **kwargs)

    def prune(self, now: float | None = None) -> int:
        self.prune_calls.append(now)
        return super().prune(now)


class SlowClosingStore(ObservationStore):
    def __init__(self, *args: object, **kwargs: object) -> None:
        self.closed = False
        super().__init__(*args, **kwargs)

    def add_event(self, event: DetectionEvent, received_at: float) -> int:
        time.sleep(0.02)
        return super().add_event(event, received_at)

    def close(self) -> None:
        self.closed = True
        super().close()


class NewDashApplicationStateTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.store = ObservationStore(Path(self.temp.name) / "history.sqlite3")
        self.application = NewDashApplication(self.store)

    def tearDown(self) -> None:
        self.application.close()
        self.temp.cleanup()

    def test_detection_updates_recent_state_before_persisting_append_only_event(self) -> None:
        event = DetectionEvent(
            detection_id="RID-7", manufacturer="DJI", badge_label="REMOTE ID",
            badge_class="drone", badge_entity_key="rid:7", source_id=0,
            source="ble_rid", confidence=0.95, threat_score=80.0, rssi=-48,
        )

        self.application.handle_frame(MachineFrame("detection", event), 1_700_000_000.0)

        recent = self.application.snapshot(now=1_700_000_000.0)["recent_events"]
        self.assertEqual(len(recent), 1)
        self.assertEqual(recent[0]["detection_id"], "RID-7")
        self.assertEqual(recent[0]["received_at"], 1_700_000_000.0)
        self.assertTrue(self.application._persistence_barrier())
        persisted = self.store.query(HistoryQuery()).items
        self.assertEqual(len(persisted), 1)
        self.assertEqual(persisted[0].kind, "event")
        self.assertEqual(persisted[0].stable_key, "rid:7")

    def test_status_atomically_replaces_current_state_and_persists_changed_tracks(self) -> None:
        first = BadgeStatus.from_payload(
            {"version": "old", "uptime_s": 1, "legacy_only": "gone"}
        )
        rich = self._rich_status()
        self.application.handle_frame(MachineFrame("status", first), 100.0)

        self.application.handle_frame(MachineFrame("status", rich), 110.0)

        snapshot = self.application.snapshot(now=110.0)
        status = snapshot["status"]
        self.assertEqual(status["version"], "0.64.66-badge-signature-parity")
        self.assertNotIn("legacy_only", status)
        self.assertEqual(status["mode"], "usb_only")
        self.assertEqual(status["scanners"][0]["health"], "ok")
        self.assertEqual(status["remote_id_entities"][0]["source"], "ble_rid")
        self.assertTrue(status["remote_id_entities"][0]["is_remote_id"])
        self.assertTrue(status["remote_id_entities"][0]["has_position"])
        self.assertTrue(self.application._persistence_barrier())
        self.assertEqual(len(self.store.query(HistoryQuery(kind="track")).items), 1)

        self.application.handle_frame(MachineFrame("status", rich), 112.0)
        self.assertTrue(self.application._persistence_barrier())
        self.assertEqual(len(self.store.query(HistoryQuery(kind="track")).items), 1)

        moved = replace(rich.entities[0], latitude=37.7750)
        self.application.handle_frame(
            MachineFrame("status", replace(rich, entities=(moved,))), 114.0
        )
        counted = replace(moved, events=8)
        self.application.handle_frame(
            MachineFrame("status", replace(rich, entities=(counted,))), 116.0
        )
        self.assertTrue(self.application._persistence_barrier())
        tracks = self.store.query(HistoryQuery(kind="track")).items
        self.assertEqual(len(tracks), 3)
        self.assertEqual((tracks[0].latitude, tracks[0].events), (37.7750, 8))

    @staticmethod
    def _rich_status() -> BadgeStatus:
        fixture = Path(__file__).parent / "fixtures" / "badge_status_remote_id.json"
        return BadgeStatus.from_payload(json.loads(fixture.read_text()))


class NewDashApplicationHealthTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.store = ObservationStore(Path(self.temp.name) / "history.sqlite3")
        self.application = NewDashApplication(self.store)

    def tearDown(self) -> None:
        self.application.close()
        self.temp.cleanup()

    def test_no_status_is_unavailable_without_fabricated_counts(self) -> None:
        snapshot = self.application.snapshot(now=100.0)

        self.assertEqual(snapshot["freshness"], {"state": "unavailable", "age_s": None})
        self.assertIsNone(snapshot["status"]["version"])
        self.assertIsNone(snapshot["status"]["counts"])
        self.assertEqual(snapshot["status"]["sensing_health"], "unknown")

    def test_age_rounding_staleness_and_reconnect_retain_prior_status(self) -> None:
        status = NewDashApplicationStateTest._rich_status()
        self.application.handle_connection(
            ConnectionUpdate("live", "status_valid", port="USB-data")
        )
        self.application.handle_frame(MachineFrame("status", status), 100.04)

        fresh = self.application.snapshot(now=101.29)
        self.assertEqual(fresh["freshness"], {"state": "fresh", "age_s": 1.2})
        self.assertEqual(fresh["status"]["sensing_health"], "healthy")
        stale = self.application.snapshot(now=106.04)
        self.assertEqual(stale["freshness"], {"state": "stale", "age_s": 6.0})

        self.application.handle_connection(
            ConnectionUpdate("reconnecting", "USB string stays data", reconnect_attempt=3)
        )
        reconnecting = self.application.snapshot(now=102.0)
        self.assertEqual(reconnecting["freshness"]["state"], "stale")
        self.assertEqual(reconnecting["status"]["version"], status.version)
        self.assertEqual(reconnecting["connection"]["detail"], "USB string stays data")
        self.assertEqual(reconnecting["connection"]["reconnect_attempt"], 3)

    def test_safe_usb_and_disconnected_scanner_have_distinct_sensing_health(self) -> None:
        self.application.handle_connection(ConnectionUpdate("live", "status_valid"))
        safe = BadgeStatus.from_payload(
            {
                "version": "safe", "uptime_s": 10, "safe_mode": True,
                "recovery_mode": "safe_usb", "scanners": [],
            }
        )
        self.application.handle_frame(MachineFrame("status", safe), 10.0)
        self.assertEqual(
            self.application.snapshot(now=10.0)["status"]["sensing_health"],
            "safe_usb",
        )

        disconnected = BadgeStatus.from_payload(
            {
                "version": "normal", "uptime_s": 11, "safe_mode": False,
                "recovery_mode": "normal",
                "scanners": [{"uart": "ble", "connected": False, "health": "offline"}],
            }
        )
        self.application.handle_frame(MachineFrame("status", disconnected), 11.0)
        self.assertEqual(
            self.application.snapshot(now=11.0)["status"]["sensing_health"],
            "degraded",
        )

    def test_latest_connection_protocol_counters_are_diagnostics(self) -> None:
        update = ConnectionUpdate(
            "reconnecting", "read_error", port="/dev/cu.usb-data",
            candidates=(("/dev/cu.usb-a", "/dev/tty.usb-a"),),
            firmware_version="v9", malformed_frames=7, overlong_lines=4,
            reconnect_attempt=2,
        )
        self.application.handle_connection(update)

        snapshot = self.application.snapshot(now=5.0)
        self.assertEqual(snapshot["connection"], {
            "phase": "reconnecting", "detail": "read_error",
            "port": "/dev/cu.usb-data",
            "candidates": [["/dev/cu.usb-a", "/dev/tty.usb-a"]],
            "firmware_version": "v9", "reconnect_attempt": 2,
        })
        self.assertEqual(snapshot["diagnostics"]["malformed_lines"], 7)
        self.assertEqual(snapshot["diagnostics"]["overlong_lines"], 4)

    def test_storage_failure_keeps_live_state_and_reports_bounded_unavailable_history(self) -> None:
        self.application.close()
        failing = FailingEventStore(Path(self.temp.name) / "failing.sqlite3")
        self.application = NewDashApplication(failing)
        event = self._event("not-saved")

        self.application.handle_frame(MachineFrame("detection", event), 20.0)
        self.assertTrue(self.application._persistence_barrier())

        snapshot = self.application.snapshot(now=20.0)
        self.assertEqual(snapshot["recent_events"][0]["detection_id"], "not-saved")
        self.assertFalse(snapshot["diagnostics"]["history_available"])
        self.assertLessEqual(len(snapshot["diagnostics"]["history_error"]), 256)
        self.assertEqual(snapshot["diagnostics"]["persistence_drops"], 1)
        self.assertEqual(failing.query(HistoryQuery()).items, ())

    @staticmethod
    def _event(detection_id: str) -> DetectionEvent:
        return DetectionEvent(
            detection_id=detection_id, manufacturer="DJI", badge_label="REMOTE ID",
            badge_class="drone", badge_entity_key=f"rid:{detection_id}", source_id=0,
            source="ble_rid", confidence=0.95, threat_score=80.0, rssi=-48,
        )


class NewDashApplicationPersistenceTest(unittest.TestCase):
    def test_full_queue_never_blocks_live_callback_and_counts_unsaved_records(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            store = BlockingPruneStore(Path(temp) / "blocked.sqlite3")
            application = NewDashApplication(store)
            self.assertTrue(store.prune_started.wait(1.0))
            event = NewDashApplicationHealthTest._event("overflow")

            started = time.monotonic()
            for index in range(1_025):
                application.handle_frame(MachineFrame("detection", event), float(index))
            elapsed = time.monotonic() - started
            snapshot = application.snapshot(now=1_025.0)

            self.assertLess(elapsed, 0.5)
            self.assertEqual(len(snapshot["recent_events"]), 20)
            self.assertEqual(snapshot["diagnostics"]["persistence_queue_depth"], 1_024)
            self.assertEqual(snapshot["diagnostics"]["persistence_drops"], 1)
            store.release_prune.set()
            application.close()

    def test_clear_is_ordered_after_earlier_rows_and_before_later_rows(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            store = ObservationStore(Path(temp) / "clear.sqlite3")
            application = NewDashApplication(store)
            application.handle_frame(
                MachineFrame("detection", NewDashApplicationHealthTest._event("before")), 1.0
            )

            self.assertEqual(application.clear_history(), 1)
            self.assertEqual(application.query_history(HistoryQuery()).items, ())

            application.handle_frame(
                MachineFrame("detection", NewDashApplicationHealthTest._event("after")), 2.0
            )
            self.assertTrue(application._persistence_barrier())
            self.assertEqual(
                [item.display_id for item in application.query_history(HistoryQuery()).items],
                ["after"],
            )
            application.close()

    def test_prune_is_queued_at_startup_and_at_most_hourly(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            store = CountingPruneStore(Path(temp) / "prune.sqlite3")
            application = NewDashApplication(store, wall_clock=lambda: 1_000.0)
            self.assertTrue(application._persistence_barrier())
            self.assertEqual(store.prune_calls, [1_000.0])

            application.snapshot(now=4_599.9)
            self.assertTrue(application._persistence_barrier())
            self.assertEqual(store.prune_calls, [1_000.0])
            application.snapshot(now=4_600.0)
            self.assertTrue(application._persistence_barrier())
            self.assertEqual(store.prune_calls, [1_000.0, 4_600.0])
            application.close()

    def test_close_uses_one_bounded_drain_then_counts_and_discards_remainder(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            store = SlowClosingStore(Path(temp) / "slow.sqlite3")
            application = NewDashApplication(store)
            event = NewDashApplicationHealthTest._event("queued")
            for index in range(400):
                application.handle_frame(MachineFrame("detection", event), float(index))

            started = time.monotonic()
            application.close()
            elapsed = time.monotonic() - started

            diagnostics = application.snapshot(now=500.0)["diagnostics"]
            self.assertLess(elapsed, 3.5)
            self.assertGreater(diagnostics["persistence_drops"], 0)
            self.assertFalse(application._worker.is_alive())
            self.assertTrue(store.closed)
            recent_before = application.snapshot(now=500.0)["recent_events"]
            application.handle_frame(MachineFrame("detection", event), 501.0)
            self.assertEqual(application.snapshot(now=501.0)["recent_events"], recent_before)


class NewDashApplicationControlTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.store = ObservationStore(Path(self.temp.name) / "controls.sqlite3")
        self.application = NewDashApplication(self.store)
        self.transport = FakeTransport()
        self.application.attach_transport(self.transport)

    def tearDown(self) -> None:
        self.application.close()
        self.temp.cleanup()

    def test_each_control_uses_only_its_sealed_builder_and_transport(self) -> None:
        replies = (
            self.application.display_nav("detail"),
            self.application.set_theme(THEME),
            self.application.reset_theme(),
            self.application.set_display_policy(complete_policy()),
            self.application.reset_display_policy(),
        )

        self.assertTrue(all(reply.ok for reply in replies))
        self.assertEqual([command.payload["cmd"] for command in self.transport.commands], [
            "display_nav",
            "badge_theme",
            "badge_theme_reset",
            "badge_display_policy",
            "badge_display_policy_reset",
        ])

    def test_invalid_control_is_rejected_before_transport(self) -> None:
        with self.assertRaises(ControlValidationError):
            self.application.display_nav("reboot")
        with self.assertRaises(ControlValidationError):
            self.application.set_theme({"cmd": "reboot"})
        with self.assertRaises(ControlValidationError):
            self.application.set_display_policy({})

        self.assertEqual(self.transport.commands, [])

    def test_unattached_transport_returns_bounded_stable_error(self) -> None:
        unattached = NewDashApplication(
            ObservationStore(Path(self.temp.name) / "unattached.sqlite3")
        )
        try:
            with self.assertRaises(TransportUnavailable) as raised:
                unattached.reset_theme()
            self.assertEqual(raised.exception.code, "transport_unavailable")
            self.assertLessEqual(len(raised.exception.message), 256)
        finally:
            unattached.close()

    def test_control_reply_does_not_optimistically_change_status(self) -> None:
        status = NewDashApplicationStateTest._rich_status()
        self.application.handle_frame(MachineFrame("status", status), 10.0)
        before = self.application.snapshot(now=10.0)["status"]["theme"]

        self.application.set_theme(THEME)

        after = self.application.snapshot(now=10.0)["status"]["theme"]
        self.assertEqual(after, before)


if __name__ == "__main__":
    unittest.main()
