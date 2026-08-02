from __future__ import annotations

import json
import tempfile
import threading
import time
import unittest
from dataclasses import replace
from pathlib import Path
from queue import Queue

from new_dash.application import ApplicationError, NewDashApplication
from new_dash.controls import ControlValidationError
from new_dash.models import BadgeEntity, BadgeStatus, ControlReply, DetectionEvent, MachineFrame
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


class FailingOnceTrackStore(ObservationStore):
    def __init__(self, *args: object, **kwargs: object) -> None:
        self.track_attempts = 0
        super().__init__(*args, **kwargs)

    def add_track(self, entity: BadgeEntity, received_at: float) -> int | None:
        self.track_attempts += 1
        if self.track_attempts == 1:
            raise RuntimeError("first track write failed")
        return super().add_track(entity, received_at)


class BlockingSecondTrackStore(ObservationStore):
    def __init__(self, *args: object, **kwargs: object) -> None:
        self.track_attempts = 0
        self.second_started = threading.Event()
        self.release_second = threading.Event()
        super().__init__(*args, **kwargs)

    def add_track(self, entity: BadgeEntity, received_at: float) -> int | None:
        self.track_attempts += 1
        if self.track_attempts == 2:
            self.second_started.set()
            self.release_second.wait()
        return super().add_track(entity, received_at)


class SequencedTrackStore(ObservationStore):
    def __init__(
        self, *args: object, fail_first: bool = False, **kwargs: object
    ) -> None:
        self.fail_first = fail_first
        self.track_attempts = 0
        self.first_started = threading.Event()
        self.release_first = threading.Event()
        self.third_started = threading.Event()
        self.release_third = threading.Event()
        super().__init__(*args, **kwargs)

    def add_track(self, entity: BadgeEntity, received_at: float) -> int | None:
        self.track_attempts += 1
        if self.track_attempts == 1:
            self.first_started.set()
            self.release_first.wait()
            if self.fail_first:
                raise RuntimeError("first A failed")
        elif self.track_attempts == 3:
            self.third_started.set()
            self.release_third.wait()
        return super().add_track(entity, received_at)


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


class CloseTrackingStore(ObservationStore):
    def __init__(self, *args: object, **kwargs: object) -> None:
        self.close_called = threading.Event()
        super().__init__(*args, **kwargs)

    def close(self) -> None:
        self.close_called.set()
        super().close()


class PausingRecordQueue(Queue[object]):
    """Pause the first record insertion while allowing a racing barrier."""

    def __init__(self) -> None:
        super().__init__(maxsize=1_024)
        self.record_entered = threading.Event()
        self.release_record = threading.Event()
        self.barrier_inserted = threading.Event()
        self._paused = False

    def put(
        self,
        item: object,
        block: bool = True,
        timeout: float | None = None,
    ) -> None:
        kind = getattr(item, "kind", None)
        if kind in {"event", "track"} and not self._paused:
            self._paused = True
            self.record_entered.set()
            self.release_record.wait()
        elif kind in {"barrier", "clear", "prune"}:
            self.barrier_inserted.set()
        super().put(item, block=block, timeout=timeout)


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

    def test_failed_track_write_allows_identical_status_to_retry(self) -> None:
        self.application.close()
        store = FailingOnceTrackStore(Path(self.temp.name) / "track-retry.sqlite3")
        self.application = NewDashApplication(store)
        status = self._rich_status()

        self.application.handle_frame(MachineFrame("status", status), 10.0)
        self.assertTrue(self.application._persistence_barrier())
        self.assertEqual(store.query(HistoryQuery(kind="track")).items, ())

        self.application.handle_frame(MachineFrame("status", status), 11.0)
        self.assertTrue(self.application._persistence_barrier())

        self.assertEqual(store.track_attempts, 2)
        self.assertEqual(len(store.query(HistoryQuery(kind="track")).items), 1)
        diagnostics = self.application.snapshot(now=11.0)["diagnostics"]
        self.assertTrue(diagnostics["history_available"])
        self.assertIsNone(diagnostics["history_error"])

    def test_track_change_compares_with_latest_pending_fingerprint(self) -> None:
        self.application.close()
        store = BlockingSecondTrackStore(Path(self.temp.name) / "track-order.sqlite3")
        self.application = NewDashApplication(store)
        original = self._rich_status()
        moved_entity = replace(original.entities[0], latitude=37.7750)
        moved = replace(original, entities=(moved_entity,))

        self.application.handle_frame(MachineFrame("status", original), 10.0)
        self.assertTrue(self.application._persistence_barrier())
        self.application.handle_frame(MachineFrame("status", moved), 11.0)
        self.assertTrue(store.second_started.wait(1.0))
        self.application.handle_frame(MachineFrame("status", original), 12.0)
        store.release_second.set()
        self.assertTrue(self.application._persistence_barrier())

        tracks = store.query(HistoryQuery(kind="track")).items
        self.assertEqual(store.track_attempts, 3)
        self.assertEqual(
            [track.latitude for track in tracks],
            [37.7749, 37.7750, 37.7749],
        )

    def test_earlier_success_cannot_clear_later_pending_same_fingerprint(self) -> None:
        self._assert_repeated_pending_action_identity(fail_first=False)

    def test_earlier_failure_cannot_clear_later_pending_same_fingerprint(self) -> None:
        self._assert_repeated_pending_action_identity(fail_first=True)

    def _assert_repeated_pending_action_identity(self, *, fail_first: bool) -> None:
        self.application.close()
        suffix = "failure" if fail_first else "success"
        store = SequencedTrackStore(
            Path(self.temp.name) / f"track-token-{suffix}.sqlite3",
            fail_first=fail_first,
        )
        self.application = NewDashApplication(store)
        original = self._rich_status()
        moved = replace(
            original,
            entities=(replace(original.entities[0], latitude=37.7750),),
        )

        self.application.handle_frame(MachineFrame("status", original), 10.0)
        self.assertTrue(store.first_started.wait(1.0))
        self.application.handle_frame(MachineFrame("status", moved), 11.0)
        self.application.handle_frame(MachineFrame("status", original), 12.0)
        store.release_first.set()
        self.assertTrue(store.third_started.wait(1.0))
        self.application.handle_frame(MachineFrame("status", moved), 13.0)
        store.release_third.set()
        self.assertTrue(self.application._persistence_barrier())

        self.assertEqual(store.track_attempts, 4)
        tracks = store.query(HistoryQuery(kind="track")).items
        expected = (
            [37.7750, 37.7749, 37.7750]
            if fail_first
            else [37.7750, 37.7749, 37.7750, 37.7749]
        )
        self.assertEqual([track.latitude for track in tracks], expected)

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
        self.assertIsNone(snapshot["connection"]["reconnect_attempt"])
        self.assertIsNone(snapshot["diagnostics"]["malformed_lines"])
        self.assertIsNone(snapshot["diagnostics"]["overlong_lines"])
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

    def test_freshness_compares_exact_age_before_rounding_for_display(self) -> None:
        status = NewDashApplicationStateTest._rich_status()
        self.application.handle_connection(ConnectionUpdate("live", "status_valid"))
        self.application.handle_frame(MachineFrame("status", status), 100.0)

        self.assertEqual(
            self.application.snapshot(now=105.04)["freshness"],
            {"state": "fresh", "age_s": 5.0},
        )
        self.assertEqual(
            self.application.snapshot(now=105.95)["freshness"],
            {"state": "fresh", "age_s": 6.0},
        )
        self.assertEqual(
            self.application.snapshot(now=106.0)["freshness"],
            {"state": "stale", "age_s": 6.0},
        )

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

    def test_fresh_normal_status_without_scanner_evidence_is_unknown(self) -> None:
        self.application.handle_connection(ConnectionUpdate("live", "status_valid"))
        status = BadgeStatus.from_payload(
            {
                "version": "normal", "uptime_s": 11, "safe_mode": False,
                "recovery_mode": "normal", "scanners": [],
            }
        )
        self.application.handle_frame(MachineFrame("status", status), 11.0)

        self.assertEqual(
            self.application.snapshot(now=11.0)["status"]["sensing_health"],
            "unknown",
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

        self.assertEqual(self.application.query_history(HistoryQuery()).items, ())
        recovered = self.application.snapshot(now=20.0)["diagnostics"]
        self.assertTrue(recovered["history_available"])
        self.assertIsNone(recovered["history_error"])

    @staticmethod
    def _event(detection_id: str) -> DetectionEvent:
        return DetectionEvent(
            detection_id=detection_id, manufacturer="DJI", badge_label="REMOTE ID",
            badge_class="drone", badge_entity_key=f"rid:{detection_id}", source_id=0,
            source="ble_rid", confidence=0.95, threat_score=80.0, rssi=-48,
        )


class NewDashApplicationPersistenceTest(unittest.TestCase):
    def test_shutdown_discard_removes_only_exact_pending_track_actions(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            store = SequencedTrackStore(Path(temp) / "track-discard.sqlite3")
            application = NewDashApplication(store)
            original = NewDashApplicationStateTest._rich_status()
            moved = replace(
                original,
                entities=(replace(original.entities[0], latitude=37.7750),),
            )
            application.handle_frame(MachineFrame("status", original), 10.0)
            self.assertTrue(store.first_started.wait(1.0))
            application.handle_frame(MachineFrame("status", moved), 11.0)
            application.handle_frame(MachineFrame("status", original), 12.0)
            closer = threading.Thread(target=application.close)
            try:
                closer.start()
                deadline = time.monotonic() + 4.0
                while application._persistence_queue.qsize() and time.monotonic() < deadline:
                    time.sleep(0.01)

                stable_key = original.entities[0].stable_key
                with application._lock:
                    pending = tuple(application._pending_track_actions[stable_key])
                self.assertEqual(len(pending), 1)
                self.assertEqual(
                    pending[0][1],
                    application._track_fingerprint(original.entities[0]),
                )
                self.assertEqual(
                    application.snapshot(now=13.0)["diagnostics"]["persistence_drops"],
                    2,
                )
            finally:
                store.release_first.set()
                store.release_third.set()
                closer.join(1.0)
                application.close()

            self.assertFalse(closer.is_alive())
            with application._lock:
                self.assertEqual(application._pending_track_actions, {})

    def test_clear_linearizes_a_record_accepted_before_its_barrier(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            store = ObservationStore(Path(temp) / "clear-race.sqlite3")
            application = NewDashApplication(store)
            self.assertTrue(application._persistence_barrier())
            paused_queue = PausingRecordQueue()
            application._persistence_queue = paused_queue  # type: ignore[assignment]
            event = NewDashApplicationHealthTest._event("pre-clear")
            handler = threading.Thread(
                target=application.handle_frame,
                args=(MachineFrame("detection", event), 1.0),
            )
            clear_result: list[int] = []
            clearer = threading.Thread(
                target=lambda: clear_result.append(application.clear_history())
            )
            try:
                handler.start()
                self.assertTrue(paused_queue.record_entered.wait(1.0))
                clearer.start()
                paused_queue.barrier_inserted.wait(0.2)
                paused_queue.release_record.set()
                handler.join(1.0)
                clearer.join(1.0)

                self.assertEqual(clear_result, [1])
                self.assertEqual(application.query_history(HistoryQuery()).items, ())
            finally:
                paused_queue.release_record.set()
                handler.join(1.0)
                if clearer.ident is not None:
                    clearer.join(1.0)
                application.close()

    def test_close_linearizes_a_record_accepted_before_the_drain_barrier(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            store = CloseTrackingStore(Path(temp) / "close-race.sqlite3")
            application = NewDashApplication(store)
            self.assertTrue(application._persistence_barrier())
            paused_queue = PausingRecordQueue()
            application._persistence_queue = paused_queue  # type: ignore[assignment]
            event = NewDashApplicationHealthTest._event("pre-close")
            handler = threading.Thread(
                target=application.handle_frame,
                args=(MachineFrame("detection", event), 1.0),
            )
            closer = threading.Thread(target=application.close)

            handler.start()
            self.assertTrue(paused_queue.record_entered.wait(1.0))
            closer.start()
            store.close_called.wait(0.5)
            paused_queue.release_record.set()
            handler.join(1.0)
            closer.join(1.0)

            self.assertFalse(handler.is_alive())
            self.assertFalse(closer.is_alive())
            self.assertEqual(application._persistence_queue.qsize(), 0)
            self.assertEqual(application.snapshot(now=2.0)["diagnostics"]["persistence_drops"], 0)
            self.assertEqual(
                [item.display_id for item in store.query(HistoryQuery()).items],
                ["pre-close"],
            )

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

    def test_full_queue_does_not_count_prune_clear_or_barrier_as_lost_records(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            store = BlockingPruneStore(Path(temp) / "maintenance-full.sqlite3")
            application = NewDashApplication(store, wall_clock=lambda: 0.0)
            self.assertTrue(store.prune_started.wait(1.0))
            event = NewDashApplicationHealthTest._event("queued")
            try:
                for _index in range(1_024):
                    application.handle_frame(MachineFrame("detection", event), 0.0)

                with self.assertRaises(ApplicationError):
                    application.clear_history(timeout=0.01)
                self.assertFalse(application._persistence_barrier(timeout=0.01))
                snapshot = application.snapshot(now=3_600.0)

                self.assertEqual(snapshot["diagnostics"]["persistence_drops"], 0)
                self.assertFalse(snapshot["diagnostics"]["history_available"])
                self.assertIsNotNone(snapshot["diagnostics"]["history_error"])
            finally:
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
