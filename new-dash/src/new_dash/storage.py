"""SQLite-backed, bounded observation history."""

from __future__ import annotations

import json
import sqlite3
import threading
import time
from base64 import b64decode, urlsafe_b64encode
from collections.abc import Iterator
from dataclasses import dataclass
from math import isfinite
from pathlib import Path
from typing import Any

from new_dash.models import BadgeEntity, DetectionEvent, Observation


@dataclass(frozen=True, slots=True)
class HistoryQuery:
    """Filters and pagination controls for newest-first history."""

    since: float | None = None
    until: float | None = None
    kind: str | None = None
    source: str | None = None
    threat_class: str | None = None
    text: str | None = None
    positioned: bool | None = None
    cursor: str | None = None
    limit: int = 100


@dataclass(frozen=True, slots=True)
class HistoryPage:
    """One newest-first history page and its optional continuation cursor."""

    items: tuple[Observation, ...]
    next_cursor: str | None


_SCHEMA = """
CREATE TABLE IF NOT EXISTS schema_meta (version INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS observations (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    kind TEXT NOT NULL CHECK (kind IN ('event', 'track')),
    received_at REAL NOT NULL,
    observed_at REAL NOT NULL,
    stable_key TEXT NOT NULL,
    source_id INTEGER,
    source TEXT NOT NULL,
    threat_class TEXT NOT NULL DEFAULT '',
    category TEXT NOT NULL DEFAULT '',
    label TEXT NOT NULL DEFAULT '',
    display_id TEXT NOT NULL DEFAULT '',
    manufacturer TEXT NOT NULL DEFAULT '',
    confidence REAL,
    score REAL,
    rssi INTEGER,
    events INTEGER,
    seen_count INTEGER,
    latitude REAL,
    longitude REAL,
    altitude_m REAL,
    operator_latitude REAL,
    operator_longitude REAL,
    operator_id TEXT NOT NULL DEFAULT '',
    extras_json TEXT NOT NULL DEFAULT '{}'
);
CREATE INDEX IF NOT EXISTS observations_received_at_idx
    ON observations (received_at DESC, id DESC);
CREATE INDEX IF NOT EXISTS observations_stable_key_idx
    ON observations (stable_key, kind, id DESC);
CREATE INDEX IF NOT EXISTS observations_source_idx ON observations (source);
CREATE INDEX IF NOT EXISTS observations_threat_class_idx ON observations (threat_class);
"""


class ObservationStore:
    """Owns short-lived SQLite connections for persisted badge observations."""

    def __init__(self, path: Path | str, *, retention_days: int, max_observations: int) -> None:
        if retention_days < 1:
            raise ValueError("retention_days must be at least 1")
        if max_observations < 1:
            raise ValueError("max_observations must be at least 1")
        self.path = Path(path)
        self.retention_days = retention_days
        self.max_observations = max_observations
        self._write_lock = threading.Lock()
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self._write_lock:
            with self._connect() as connection:
                connection.executescript(_SCHEMA)
                connection.execute(
                    "INSERT INTO schema_meta (version) "
                    "SELECT 1 WHERE NOT EXISTS (SELECT 1 FROM schema_meta)"
                )

    def _connect(self) -> sqlite3.Connection:
        connection = sqlite3.connect(self.path)
        connection.execute("PRAGMA journal_mode=WAL")
        connection.execute("PRAGMA busy_timeout=2000")
        connection.row_factory = sqlite3.Row
        return connection

    def add_event(self, event: DetectionEvent, received_at: float) -> int:
        """Persist an emitted event; protocol events are never deduplicated."""
        values = (
            "event", received_at, received_at, event.stable_key, event.source_id, event.source,
            event.badge_class or "", "", event.badge_label or "", event.detection_id or "",
            event.manufacturer or "", event.confidence, event.threat_score,
            int(event.rssi) if event.rssi is not None else None, None, None,
            None, None, None, None, None, "",
            json.dumps(event.to_dict(), separators=(",", ":"), allow_nan=False),
        )
        with self._write_lock:
            with self._connect() as connection:
                return _insert_observation(connection, values)

    def query(self, query: HistoryQuery) -> HistoryPage:
        limit = min(max(query.limit, 1), 500)
        where, parameters = _where_clause(query, include_cursor=True)
        with self._connect() as connection:
            rows = connection.execute(
                f"SELECT * FROM observations{where} "
                "ORDER BY received_at DESC, id DESC LIMIT ?",
                (*parameters, limit + 1),
            ).fetchall()
        items = tuple(_observation_from_row(row) for row in rows[:limit])
        next_cursor = _encode_cursor(items[-1]) if len(rows) > limit else None
        return HistoryPage(items=items, next_cursor=next_cursor)

    def add_track(self, entity: BadgeEntity, received_at: float) -> int | None:
        """Persist a changed, positioned Remote ID track observation."""
        if entity.stale or not entity.is_remote_id or not entity.has_position:
            return None

        age = min(max(entity.last_seen_seconds or 0, 0), 300)
        values = (
            "track", received_at, received_at - age, entity.stable_key, entity.source_id,
            entity.source, entity.threat_class or "", entity.category or "", entity.label or "",
            entity.display_id or "", entity.manufacturer or "", entity.confidence_pct,
            entity.score, entity.rssi, entity.events, entity.seen_count, entity.latitude,
            entity.longitude, entity.altitude_m, entity.operator_latitude, entity.operator_longitude,
            entity.operator_id or "", json.dumps(
                entity.to_dict()["extras"], separators=(",", ":"), allow_nan=False
            ),
        )
        with self._write_lock:
            with self._connect() as connection:
                previous = connection.execute(
                    """SELECT latitude, longitude, altitude_m, events, seen_count
                    FROM observations WHERE stable_key = ? AND kind = 'track'
                    ORDER BY id DESC LIMIT 1""",
                    (entity.stable_key,),
                ).fetchone()
                if previous is not None and (
                    previous["latitude"], previous["longitude"], previous["altitude_m"],
                    previous["events"], previous["seen_count"],
                ) == (
                    entity.latitude, entity.longitude, entity.altitude_m,
                    entity.events, entity.seen_count,
                ):
                    return None
                return _insert_observation(connection, values)

    def iter_export(self, query: HistoryQuery) -> Iterator[Observation]:
        """Yield all matching observations, optionally continuing from a cursor."""
        where, parameters = _where_clause(query, include_cursor=True)
        with self._connect() as connection:
            cursor = connection.execute(
                f"SELECT * FROM observations{where} ORDER BY received_at DESC, id DESC",
                parameters,
            )
            for row in cursor:
                yield _observation_from_row(row)

    def prune(self, now: float | None = None) -> int:
        """Delete expired rows and then the oldest IDs over the row limit."""
        cutoff_now = time.time() if now is None else _finite_number(now, "now")
        cutoff = cutoff_now - self.retention_days * 86_400
        with self._write_lock:
            with self._connect() as connection:
                deleted = connection.execute(
                    "DELETE FROM observations WHERE received_at < ?", (cutoff,)
                ).rowcount
                count = connection.execute("SELECT COUNT(*) FROM observations").fetchone()[0]
                excess = max(count - self.max_observations, 0)
                if excess:
                    deleted += connection.execute(
                        """DELETE FROM observations WHERE id IN (
                        SELECT id FROM observations ORDER BY id ASC LIMIT ?
                        )""",
                        (excess,),
                    ).rowcount
                return deleted

    def clear(self) -> int:
        """Delete all retained observations and return their number."""
        with self._write_lock:
            with self._connect() as connection:
                return connection.execute("DELETE FROM observations").rowcount

    def close(self) -> None:
        """Connections are intentionally short-lived, so there is nothing to close."""


def _observation_from_row(row: sqlite3.Row) -> Observation:
    extras = json.loads(row["extras_json"])
    return Observation(
        row_id=row["id"], kind=row["kind"], received_at=row["received_at"],
        observed_at=row["observed_at"], stable_key=row["stable_key"],
        source_id=row["source_id"], source=row["source"], threat_class=row["threat_class"],
        category=row["category"], label=row["label"], display_id=row["display_id"],
        manufacturer=row["manufacturer"], confidence=row["confidence"], score=row["score"],
        rssi=row["rssi"], events=row["events"], seen_count=row["seen_count"],
        latitude=row["latitude"], longitude=row["longitude"], altitude_m=row["altitude_m"],
        operator_latitude=row["operator_latitude"], operator_longitude=row["operator_longitude"],
        operator_id=row["operator_id"], extras=extras if isinstance(extras, dict) else {},
    )


def _insert_observation(connection: sqlite3.Connection, values: tuple[Any, ...]) -> int:
    cursor = connection.execute(
        """INSERT INTO observations (
        kind, received_at, observed_at, stable_key, source_id, source,
        threat_class, category, label, display_id, manufacturer, confidence, score,
        rssi, events, seen_count, latitude, longitude, altitude_m,
        operator_latitude, operator_longitude, operator_id, extras_json
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)""",
        values,
    )
    return int(cursor.lastrowid)


def _where_clause(query: HistoryQuery, *, include_cursor: bool) -> tuple[str, tuple[Any, ...]]:
    clauses: list[str] = []
    parameters: list[Any] = []
    if query.since is not None:
        clauses.append("received_at >= ?")
        parameters.append(_finite_number(query.since, "since"))
    if query.until is not None:
        clauses.append("received_at <= ?")
        parameters.append(_finite_number(query.until, "until"))
    for column, value in (
        ("kind", query.kind),
        ("source", query.source),
        ("threat_class", query.threat_class),
    ):
        if value is not None:
            if not isinstance(value, str):
                raise ValueError(f"{column} must be a string")
            clauses.append(f"{column} = ?")
            parameters.append(value)
    if query.text is not None:
        if not isinstance(query.text, str):
            raise ValueError("text must be a string")
        text = query.text.casefold().replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_")
        clauses.append(
            "(LOWER(stable_key) LIKE ? ESCAPE '\\' OR LOWER(label) LIKE ? ESCAPE '\\' "
            "OR LOWER(display_id) LIKE ? ESCAPE '\\' OR LOWER(manufacturer) LIKE ? ESCAPE '\\')"
        )
        parameters.extend([f"%{text}%"] * 4)
    if query.positioned is not None:
        if not isinstance(query.positioned, bool):
            raise ValueError("positioned must be a boolean")
        clauses.append(
            "latitude IS NOT NULL AND longitude IS NOT NULL"
            if query.positioned else "latitude IS NULL OR longitude IS NULL"
        )
    if include_cursor and query.cursor is not None:
        received_at, row_id = _decode_cursor(query.cursor)
        clauses.append("(received_at < ? OR (received_at = ? AND id < ?))")
        parameters.extend((received_at, received_at, row_id))
    return (f" WHERE {' AND '.join(clauses)}" if clauses else "", tuple(parameters))


def _encode_cursor(observation: Observation) -> str:
    if observation.row_id is None:
        raise ValueError("cursor observation must have a row ID")
    payload = json.dumps(
        {"received_at": observation.received_at, "id": observation.row_id},
        separators=(",", ":"), allow_nan=False,
    ).encode("utf-8")
    return urlsafe_b64encode(payload).decode("ascii")


def _decode_cursor(cursor: str) -> tuple[float, int]:
    if not isinstance(cursor, str) or len(cursor) % 4 or not cursor:
        raise ValueError("invalid history cursor")
    try:
        decoded = b64decode(cursor.encode("ascii"), altchars=b"-_", validate=True)
        if urlsafe_b64encode(decoded).decode("ascii") != cursor:
            raise ValueError
        payload = json.loads(decoded.decode("utf-8"))
    except (UnicodeEncodeError, ValueError, json.JSONDecodeError) as error:
        raise ValueError("invalid history cursor") from error
    if not isinstance(payload, dict) or set(payload) != {"received_at", "id"}:
        raise ValueError("invalid history cursor")
    return (
        _finite_number(payload["received_at"], "cursor received_at"),
        _positive_int(payload["id"], "cursor id"),
    )


def _finite_number(value: Any, name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not isfinite(value):
        raise ValueError(f"{name} must be finite numeric")
    return float(value)


def _positive_int(value: Any, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 1:
        raise ValueError(f"{name} must be a positive integer")
    return value
