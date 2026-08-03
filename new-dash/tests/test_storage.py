import json
import sqlite3
import tempfile
import threading
import time
import unittest
from base64 import urlsafe_b64encode
from dataclasses import replace
from pathlib import Path

from new_dash.models import BadgeEntity, DetectionEvent
from new_dash.storage import (
    HistoryQuery,
    ObservationStore,
    ObservationStoreSchemaError,
)


class TrackingObservationStore(ObservationStore):
    """Records real SQLite connections so lifecycle behavior can be exercised."""

    def __init__(self, *args: object, **kwargs: object) -> None:
        self.opened_connections: list[sqlite3.Connection] = []
        super().__init__(*args, **kwargs)

    def _connect(self) -> sqlite3.Connection:
        connection = super()._connect()
        self.opened_connections.append(connection)
        return connection


class SignalingObservationStore(ObservationStore):
    """Signals connection attempts made after schema initialization."""

    def __init__(self, *args: object, **kwargs: object) -> None:
        self.connection_attempted = threading.Event()
        self._signal_connections = False
        super().__init__(*args, **kwargs)
        self._signal_connections = True

    def _connect(self) -> sqlite3.Connection:
        if self._signal_connections:
            self.connection_attempted.set()
        return super()._connect()


class ObservationStoreTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / "new-dash.sqlite3"

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_event_survives_store_restart(self) -> None:
        store = ObservationStore(self.path, retention_days=30, max_observations=50_000)
        event = DetectionEvent(
            detection_id="RID-7", manufacturer="DJI", badge_label="REMOTE ID",
            badge_class="drone", badge_entity_key="rid:7", source_id=0,
            source="ble_rid", confidence=0.95, threat_score=80.0, rssi=-48,
        )
        row_id = store.add_event(event, received_at=1_700_000_000.0)
        store.close()

        reopened = ObservationStore(self.path, retention_days=30, max_observations=50_000)
        page = reopened.query(HistoryQuery(limit=10))
        self.assertEqual(page.items[0].row_id, row_id)
        self.assertEqual(page.items[0].stable_key, "rid:7")
        reopened.close()

    def test_tracks_deduplicate_only_identical_positioned_remote_id_snapshots(self) -> None:
        entity = self._positioned_entity()
        store = ObservationStore(self.path, retention_days=30, max_observations=50_000)

        first = store.add_track(entity, received_at=1_700_000_010.0)
        duplicate = store.add_track(entity, received_at=1_700_000_012.0)
        moved = store.add_track(replace(entity, latitude=37.7750), received_at=1_700_000_014.0)

        self.assertIsNotNone(first)
        self.assertIsNone(duplicate)
        self.assertIsNotNone(moved)
        page = store.query(HistoryQuery(limit=10))
        self.assertEqual([item.latitude for item in page.items], [37.7750, 37.7749])
        self.assertEqual(page.items[0].observed_at, 1_700_000_013.0)

    def test_track_counter_change_persists_without_coordinate_change(self) -> None:
        entity = self._positioned_entity()
        store = ObservationStore(self.path, retention_days=30, max_observations=50_000)

        store.add_track(entity, received_at=1_700_000_010.0)
        row_id = store.add_track(replace(entity, events=8), received_at=1_700_000_012.0)

        self.assertIsNotNone(row_id)
        self.assertEqual(store.query(HistoryQuery(limit=1)).items[0].events, 8)

    def test_out_of_range_device_integers_normalize_before_event_and_track_storage(self) -> None:
        too_large = 10 ** 400
        event = DetectionEvent.from_payload({
            "id": "huge-source",
            "source": too_large,
            "rssi": -48,
        })
        entity = BadgeEntity.from_payload({
            "display_id": "RID-HUGE",
            "source": "ble_rid",
            "source_id": too_large,
            "rssi": too_large,
            "events": too_large,
            "seen_count": -too_large,
            "lat": 37.7,
            "lon": -122.4,
        })
        store = ObservationStore(self.path)

        event_id = store.add_event(event, received_at=10.0)
        track_id = store.add_track(entity, received_at=11.0)
        rows = store.query(HistoryQuery(limit=10)).items

        self.assertIsNotNone(event_id)
        self.assertIsNotNone(track_id)
        self.assertIsNone(event.source_id)
        self.assertEqual(entity.source, "ble_rid")
        self.assertIsNone(entity.source_id)
        self.assertIsNone(entity.rssi)
        self.assertIsNone(entity.events)
        self.assertIsNone(entity.seen_count)
        self.assertTrue(all(row.source_id is None for row in rows))
        self.assertIsNone(rows[0].rssi)
        self.assertIsNone(rows[0].events)
        self.assertIsNone(rows[0].seen_count)
        store.close()

    def test_out_of_range_detection_rssi_normalizes_and_event_persists(self) -> None:
        event = DetectionEvent.from_payload({
            "id": "huge-rssi",
            "source": 0,
            "rssi": 1e20,
        })
        store = ObservationStore(self.path)

        row_id = store.add_event(event, received_at=10.0)
        row = store.query(HistoryQuery(limit=1)).items[0]

        self.assertIsNotNone(row_id)
        self.assertIsNone(event.rssi)
        self.assertIsNone(row.rssi)
        store.close()

    def test_rejects_stale_non_remote_id_and_unpositioned_tracks(self) -> None:
        entity = self._positioned_entity()
        store = ObservationStore(self.path, retention_days=30, max_observations=50_000)

        self.assertIsNone(store.add_track(replace(entity, stale=True), received_at=1_700_000_010.0))
        self.assertIsNone(store.add_track(replace(entity, source="wifi_dji_ie"), received_at=1_700_000_010.0))
        self.assertIsNone(store.add_track(replace(entity, latitude=None), received_at=1_700_000_010.0))
        self.assertEqual(store.query(HistoryQuery()).items, ())

    def test_query_filters_and_clamps_newest_first_limit(self) -> None:
        store = ObservationStore(self.path, retention_days=30, max_observations=50_000)
        oldest = store.add_event(self._event("old", source="ble_rid", threat_class="drone"), 10.0)
        store.add_event(self._event("middle", source="wifi_dji_ie", threat_class="privacy"), 20.0)
        newest = store.add_event(self._event("new", source="ble_rid", threat_class="drone"), 30.0)

        self.assertEqual(store.query(HistoryQuery(limit=0)).items[0].row_id, newest)
        self.assertEqual(len(store.query(HistoryQuery(limit=999)).items), 3)
        self.assertEqual(
            [
                item.row_id
                for item in store.query(
                    HistoryQuery(kind="event", source="ble_rid", threat_class="drone")
                ).items
            ],
            [newest, oldest],
        )

    def test_cursor_continues_equal_timestamp_rows_deterministically(self) -> None:
        store = ObservationStore(self.path, retention_days=30, max_observations=50_000)
        first = store.add_event(self._event("one"), 10.0)
        second = store.add_event(self._event("two"), 10.0)
        third = store.add_event(self._event("three"), 10.0)

        page_one = store.query(HistoryQuery(limit=2))
        page_two = store.query(HistoryQuery(limit=2, cursor=page_one.next_cursor))

        self.assertEqual([item.row_id for item in page_one.items], [third, second])
        self.assertEqual([item.row_id for item in page_two.items], [first])
        self.assertIsNone(page_two.next_cursor)

    def test_query_rejects_malformed_cursor(self) -> None:
        store = ObservationStore(self.path, retention_days=30, max_observations=50_000)
        for cursor in ("not-base64", "eyJyZWNlaXZlZF9hdCI6MX0", "eyJyZWNlaXZlZF9hdCI6MSwiaWQiOiIxIn0="):
            with self.subTest(cursor=cursor):
                with self.assertRaises(ValueError):
                    store.query(HistoryQuery(cursor=cursor))

    def test_identity_search_and_positioned_filter_match_only_requested_rows(self) -> None:
        store = ObservationStore(self.path, retention_days=30, max_observations=50_000)
        event_id = store.add_event(self._event("ignored", manufacturer="Acme Flight"), 10.0)
        track_id = store.add_track(self._positioned_entity(), 20.0)

        self.assertEqual(
            [item.row_id for item in store.query(HistoryQuery(text="ACME")).items], [event_id]
        )
        self.assertEqual(
            [
                item.row_id
                for item in store.query(HistoryQuery(text="abc123", positioned=True)).items
            ],
            [track_id],
        )
        self.assertEqual(store.query(HistoryQuery(positioned=True, source="wifi_rid")).items, ())

    def test_export_uses_filters_without_page_limit_and_rows_have_stable_json_fields(self) -> None:
        store = ObservationStore(self.path, retention_days=30, max_observations=50_000)
        store.add_event(self._event("one", source="ble_rid"), 10.0)
        store.add_event(self._event("two", source="ble_rid"), 20.0)
        store.add_event(self._event("three", source="wifi_rid"), 30.0)

        exported = list(store.iter_export(HistoryQuery(source="ble_rid", limit=1)))

        self.assertEqual([item.display_id for item in exported], ["two", "one"])
        self.assertEqual(list(exported[0].to_dict()), [
            "row_id", "kind", "received_at", "observed_at", "stable_key", "source_id",
            "source", "threat_class", "category", "label", "display_id", "manufacturer",
            "confidence", "score", "rssi", "events", "seen_count", "latitude", "longitude",
            "altitude_m", "operator_latitude", "operator_longitude", "operator_id", "extras",
        ])
        self.assertIsInstance(json.loads(json.dumps(exported[0].to_dict())), dict)

    def test_rehydrated_observation_extras_remain_immutable(self) -> None:
        store = ObservationStore(self.path, retention_days=30, max_observations=50_000)
        store.add_event(self._event("one"), 10.0)
        observation = store.query(HistoryQuery()).items[0]

        with self.assertRaises(TypeError):
            observation.extras["tamper"] = True

    def test_clear_returns_deleted_row_count(self) -> None:
        store = ObservationStore(self.path, retention_days=30, max_observations=50_000)
        store.add_event(self._event("one"), 10.0)
        store.add_event(self._event("two"), 20.0)

        self.assertEqual(store.clear(), 2)
        self.assertEqual(store.query(HistoryQuery()).items, ())

    def test_prune_keeps_exact_retention_boundary_and_removes_older_row(self) -> None:
        now = 1_700_000_000.0
        store = ObservationStore(self.path, retention_days=30, max_observations=50_000)
        store.add_event(self._event("boundary"), now - 30 * 86_400)
        store.add_event(self._event("expired"), now - 30 * 86_400 - 0.1)

        self.assertEqual(store.prune(now), 1)
        self.assertEqual([item.display_id for item in store.query(HistoryQuery()).items], ["boundary"])

    def test_prune_keeps_newest_fifty_thousand_rows(self) -> None:
        now = 1_700_000_000.0
        store = ObservationStore(self.path, retention_days=30, max_observations=50_000)
        with store._connection() as connection:
            connection.executemany(
                """INSERT INTO observations (
                kind, received_at, observed_at, stable_key, source, extras_json
                ) VALUES ('event', ?, ?, ?, 'ble_rid', '{}')""",
                [(now - 1, now - 1, f"event:{index}") for index in range(50_003)],
            )

        self.assertEqual(store.prune(now), 3)
        with store._connection() as connection:
            count = connection.execute("SELECT COUNT(*) FROM observations").fetchone()[0]
            oldest = connection.execute("SELECT MIN(id) FROM observations").fetchone()[0]
        self.assertEqual(count, 50_000)
        self.assertEqual(oldest, 4)

    def test_constructor_rejects_non_positive_bounds(self) -> None:
        with self.assertRaises(ValueError):
            ObservationStore(self.path, retention_days=0, max_observations=1)
        with self.assertRaises(ValueError):
            ObservationStore(self.path, retention_days=1, max_observations=0)

    def test_constructor_uses_documented_retention_defaults(self) -> None:
        store = ObservationStore(self.path)

        self.assertEqual(store.retention_days, 30)
        self.assertEqual(store.max_observations, 50_000)

    def test_constructor_creates_exactly_one_supported_schema_version(self) -> None:
        store = ObservationStore(self.path)
        store.close()

        with sqlite3.connect(self.path) as connection:
            versions = connection.execute(
                "SELECT version, typeof(version) FROM schema_meta"
            ).fetchall()
            tables = {
                row[0]
                for row in connection.execute(
                    "SELECT name FROM sqlite_schema WHERE type = 'table'"
                )
            }

        self.assertEqual(versions, [(1, "integer")])
        self.assertIn("observations", tables)

    def test_constructor_rejects_future_schema_without_mutating_database(self) -> None:
        self._create_schema_metadata(2)

        self._assert_constructor_rejects_schema_without_mutating()

    def test_constructor_rejects_duplicate_schema_versions_without_mutating_database(self) -> None:
        self._create_schema_metadata(1, 1)

        self._assert_constructor_rejects_schema_without_mutating()

    def test_constructor_rejects_missing_schema_version_without_mutating_database(self) -> None:
        self._create_schema_metadata()

        self._assert_constructor_rejects_schema_without_mutating()

    def test_constructor_rejects_malformed_schema_version_without_mutating_database(self) -> None:
        self._create_schema_metadata("not-an-integer")

        self._assert_constructor_rejects_schema_without_mutating()

    def test_public_operations_close_their_real_sqlite_connections(self) -> None:
        store = TrackingObservationStore(self.path)
        store.add_event(self._event("one"), 10.0)
        store.query(HistoryQuery())
        store.add_track(self._positioned_entity(), 20.0)
        list(store.iter_export(HistoryQuery()))
        store.prune(now=1_700_000_000.0)
        store.clear()
        store.close()

        self.assertGreaterEqual(len(store.opened_connections), 7)
        for connection in store.opened_connections:
            with self.subTest(connection=connection):
                with self.assertRaises(sqlite3.ProgrammingError):
                    connection.execute("SELECT 1")

    def test_close_tracks_connection_attempt_blocked_in_wal_negotiation(self) -> None:
        store = SignalingObservationStore(self.path)
        setup = sqlite3.connect(self.path)
        self.assertEqual(setup.execute("PRAGMA journal_mode=DELETE").fetchone()[0], "delete")
        setup.close()
        blocker = sqlite3.connect(
            self.path,
            isolation_level=None,
            check_same_thread=False,
        )
        blocker.execute("BEGIN EXCLUSIVE")
        errors: list[BaseException] = []

        def prune() -> None:
            try:
                store.prune()
            except BaseException as error:
                errors.append(error)

        worker = threading.Thread(target=prune)
        worker.start()
        try:
            self.assertTrue(store.connection_attempted.wait(1.0))
            time.sleep(0.02)
            self.assertTrue(worker.is_alive())

            started = time.monotonic()
            close_error: BaseException | None = None
            try:
                store.close(timeout=0.2)
            except BaseException as error:
                close_error = error
            elapsed = time.monotonic() - started

            self.assertIsNone(close_error)
            self.assertLess(elapsed, 0.35)
            self.assertFalse(worker.is_alive())
            self.assertEqual(len(errors), 1)
        finally:
            blocker.execute("ROLLBACK")
            blocker.close()
            worker.join(1.0)

    def test_close_interrupts_registered_sqlite_busy_wait(self) -> None:
        store = ObservationStore(self.path)
        blocker = sqlite3.connect(
            self.path,
            isolation_level=None,
            check_same_thread=False,
        )
        blocker.execute("BEGIN IMMEDIATE")
        errors: list[BaseException] = []

        def prune() -> None:
            try:
                store.prune()
            except BaseException as error:
                errors.append(error)

        worker = threading.Thread(target=prune)
        worker.start()
        try:
            deadline = time.monotonic() + 1.0
            while time.monotonic() < deadline:
                with store._connection_condition:
                    if store._active_connections:
                        break
                time.sleep(0.005)
            with store._connection_condition:
                self.assertTrue(store._active_connections)

            started = time.monotonic()
            close_error: BaseException | None = None
            try:
                store.close(timeout=0.2)
            except BaseException as error:
                close_error = error
            elapsed = time.monotonic() - started

            self.assertIsNone(close_error)
            self.assertLess(elapsed, 0.35)
            self.assertFalse(worker.is_alive())
            self.assertEqual(len(errors), 1)
        finally:
            blocker.execute("ROLLBACK")
            blocker.close()
            worker.join(1.0)

    def test_partially_consumed_export_keeps_no_sqlite_connection_open(self) -> None:
        store = TrackingObservationStore(self.path)
        store.add_event(self._event("one"), 10.0)
        store.add_event(self._event("two"), 20.0)
        connection_count = len(store.opened_connections)

        export = store.iter_export(HistoryQuery())
        self.assertEqual(next(export).display_id, "two")

        export_connections = store.opened_connections[connection_count:]
        self.assertTrue(export_connections)
        for connection in export_connections:
            with self.subTest(connection=connection):
                with self.assertRaises(sqlite3.ProgrammingError):
                    connection.execute("SELECT 1")

    def test_query_rejects_cursor_with_huge_received_at_integer(self) -> None:
        store = ObservationStore(self.path)
        cursor = urlsafe_b64encode(json.dumps({"received_at": 10 ** 1_000, "id": 1}).encode()).decode()

        with self.assertRaises(ValueError):
            store.query(HistoryQuery(cursor=cursor))

    def test_cursor_row_id_must_fit_sqlite_signed_64_bit_range(self) -> None:
        store = ObservationStore(self.path)
        too_large = urlsafe_b64encode(
            json.dumps(
                {"received_at": 1.0, "id": 9_223_372_036_854_775_808},
                separators=(",", ":"),
            ).encode("utf-8")
        ).decode("ascii")
        maximum = urlsafe_b64encode(
            json.dumps(
                {"received_at": 1.0, "id": 9_223_372_036_854_775_807},
                separators=(",", ":"),
            ).encode("utf-8")
        ).decode("ascii")

        with self.assertRaises(ValueError):
            store.query(HistoryQuery(cursor=too_large))

        self.assertEqual(store.query(HistoryQuery(cursor=maximum)).items, ())

    def _positioned_entity(self) -> BadgeEntity:
        fixture = Path(__file__).parent / "fixtures" / "badge_status_remote_id.json"
        return BadgeEntity.from_payload(json.loads(fixture.read_text())["entities"][0])

    def _create_schema_metadata(self, *versions: object) -> None:
        with sqlite3.connect(self.path) as connection:
            connection.execute(
                "CREATE TABLE schema_meta (version INTEGER NOT NULL)"
            )
            connection.executemany(
                "INSERT INTO schema_meta (version) VALUES (?)",
                ((version,) for version in versions),
            )

    def _assert_constructor_rejects_schema_without_mutating(self) -> None:
        before = self._database_snapshot()

        with self.assertRaises(ObservationStoreSchemaError):
            ObservationStore(self.path)

        self.assertEqual(self._database_snapshot(), before)

    def _database_snapshot(
        self,
    ) -> tuple[str, list[tuple[object, ...]], list[tuple[object, ...]]]:
        with sqlite3.connect(self.path) as connection:
            journal_mode = connection.execute("PRAGMA journal_mode").fetchone()[0]
            objects = connection.execute(
                """SELECT type, name, tbl_name, sql
                FROM sqlite_schema
                WHERE name NOT LIKE 'sqlite_%'
                ORDER BY type, name"""
            ).fetchall()
            versions = connection.execute(
                "SELECT version, typeof(version) FROM schema_meta ORDER BY rowid"
            ).fetchall()
        return journal_mode, objects, versions

    @staticmethod
    def _event(
        detection_id: str,
        *,
        source: str = "ble_rid",
        threat_class: str = "drone",
        manufacturer: str = "DJI",
    ) -> DetectionEvent:
        return DetectionEvent(
            detection_id=detection_id, manufacturer=manufacturer, badge_label=f"Label {detection_id}",
            badge_class=threat_class, badge_entity_key=f"{source}:{detection_id}", source_id=0,
            source=source, confidence=0.95, threat_score=80.0, rssi=-48,
        )
