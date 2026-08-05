"""Thread-safe live badge state coordinated with bounded local persistence."""

from __future__ import annotations

from collections import deque
from collections.abc import Iterator, Mapping
from dataclasses import dataclass
from queue import Empty, Full, Queue
import threading
import time
from typing import Callable, Protocol, cast

from .controls import (
    BadgeControlCommand,
    build_display_nav,
    build_display_policy,
    build_display_policy_reset,
    build_theme,
    build_theme_reset,
)
from .models import (
    BadgeEntity,
    BadgeStatus,
    ControlReply,
    DetectionEvent,
    LiteConfiguration,
    LiteConfigWriteReply,
    MachineFrame,
    Observation,
)
from .serial_transport import ConnectionUpdate, TransportUnavailable
from .storage import HistoryPage, HistoryQuery, ObservationStore


_PERSISTENCE_CAPACITY = 1_024
_HISTORY_ERROR_LIMIT = 256
_PRUNE_INTERVAL_SECONDS = 3_600.0
_DEFAULT_CLOSE_TIMEOUT_SECONDS = 3.0
_DEFAULT_REMOTE_ID_HOLD_SECONDS = 120.0
_DEFAULT_MAX_REMOTE_ID_ENTITIES = 512
_CANCELLATION_RESERVE_SECONDS = 0.5
_STOP_TIMEOUT_MESSAGE = "New Dash did not stop within the shutdown timeout."


class BadgeTransportLike(Protocol):
    """The safe application-facing portion of the serial transport."""

    def send_control(
        self, command: BadgeControlCommand, timeout: float = 5.0
    ) -> ControlReply: ...

    def get_lite_config(self, timeout: float = 5.0) -> LiteConfiguration: ...

    def set_lite_config(
        self, payload: object, timeout: float = 5.0
    ) -> LiteConfigWriteReply: ...

    def select_port(self, port: str, timeout: float = 3.0) -> None: ...


class ApplicationError(RuntimeError):
    """A stable, bounded application-level operation failure."""

    def __init__(self, code: str, message: str) -> None:
        self.code = code
        self.message = message[:_HISTORY_ERROR_LIMIT]
        super().__init__(f"{code}: {self.message}")


@dataclass(slots=True)
class _PersistenceAction:
    kind: str
    value: object | None = None
    received_at: float | None = None
    completion: threading.Event | None = None
    result: object | None = None
    error: BaseException | None = None
    track_key: str | None = None
    track_fingerprint: tuple[object, ...] | None = None
    track_action_id: int | None = None


class NewDashApplication:
    """Coordinate immutable protocol state, persistence, and safe controls."""

    def __init__(
        self,
        store: ObservationStore,
        *,
        wall_clock: Callable[[], float] = time.time,
        remote_id_hold_seconds: float = _DEFAULT_REMOTE_ID_HOLD_SECONDS,
        max_remote_id_entities: int = _DEFAULT_MAX_REMOTE_ID_ENTITIES,
    ) -> None:
        if remote_id_hold_seconds <= 0:
            raise ValueError("remote_id_hold_seconds must be positive")
        if max_remote_id_entities < 1:
            raise ValueError("max_remote_id_entities must be at least 1")
        self._store = store
        self._wall_clock = wall_clock
        self._remote_id_hold_seconds = float(remote_id_hold_seconds)
        self._max_remote_id_entities = max_remote_id_entities
        self._lock = threading.RLock()
        self._status: BadgeStatus | None = None
        self._status_received_at: float | None = None
        self._connection: ConnectionUpdate | None = None
        self._recent_events: deque[tuple[DetectionEvent, float]] = deque(maxlen=20)
        self._positioned_remote_ids: dict[
            str, tuple[dict[str, object], float]
        ] = {}
        self._persisted_track_fingerprints: dict[str, tuple[object, ...]] = {}
        self._pending_track_actions: dict[
            str, deque[tuple[int, tuple[object, ...]]]
        ] = {}
        self._next_track_action_id = 1
        startup_error = getattr(store, "startup_error", None)
        self._history_available = not isinstance(startup_error, BaseException)
        self._history_error: str | None = (
            self._bounded_error(startup_error)
            if isinstance(startup_error, BaseException)
            else None
        )
        self._persistence_drops = 0
        self._accepting = True
        self._closed = False
        self._close_attempt_lock = threading.Lock()
        self._store_closed = False
        self._last_prune_queued_at: float | None = None
        self._transport: BadgeTransportLike | None = None
        self._persistence_queue: Queue[_PersistenceAction] = Queue(
            maxsize=_PERSISTENCE_CAPACITY
        )
        self._worker_stop = threading.Event()
        self._worker = threading.Thread(
            target=self._run_persistence,
            name="new-dash-persistence",
            daemon=False,
        )
        self._hydrate_positioned_remote_ids(self._now())
        self._worker.start()
        self._prune_if_due(self._now())

    def handle_frame(self, frame: MachineFrame, received_at: float) -> None:
        """Update live state first, then offer persistence without waiting."""

        if frame.kind == "detection" and isinstance(frame.value, DetectionEvent):
            with self._lock:
                if not self._accepting:
                    return
                self._recent_events.append((frame.value, received_at))
                self._enqueue_locked(
                    _PersistenceAction("event", frame.value, received_at)
                )
        elif frame.kind == "status" and isinstance(frame.value, BadgeStatus):
            with self._lock:
                if not self._accepting:
                    return
                self._status = frame.value
                self._status_received_at = received_at
                for entity in frame.value.entities or ():
                    if entity.stale or not entity.is_remote_id or not entity.has_position:
                        continue
                    self._remember_positioned_remote_id_locked(entity, received_at)
                    fingerprint = self._track_fingerprint(entity, received_at)
                    stable_key = entity.stable_key
                    pending = self._pending_track_actions.get(stable_key)
                    latest_fingerprint = (
                        pending[-1][1]
                        if pending
                        else self._persisted_track_fingerprints.get(stable_key)
                    )
                    if latest_fingerprint != fingerprint:
                        action_id = self._next_track_action_id
                        self._next_track_action_id += 1
                        if self._enqueue_locked(
                            _PersistenceAction(
                                "track",
                                entity,
                                received_at,
                                track_key=stable_key,
                                track_fingerprint=fingerprint,
                                track_action_id=action_id,
                            )
                        ):
                            self._pending_track_actions.setdefault(
                                stable_key, deque()
                            ).append((action_id, fingerprint))
        self._prune_if_due(received_at)

    def handle_connection(self, update: ConnectionUpdate) -> None:
        with self._lock:
            if self._accepting:
                self._connection = update

    def attach_transport(self, transport: BadgeTransportLike) -> None:
        with self._lock:
            if not self._accepting:
                return
            self._transport = transport

    def snapshot(self, now: float | None = None) -> dict[str, object]:
        current_time = self._now() if now is None else now
        self._prune_if_due(current_time)
        with self._lock:
            status = self._status
            status_received_at = self._status_received_at
            connection = self._connection
            recent = tuple(self._recent_events)
            positioned_remote_ids = self._positioned_remote_id_snapshot_locked(
                status, current_time
            )
            history_available = self._history_available
            history_error = self._history_error
            persistence_drops = self._persistence_drops
        return {
            "connection": self._connection_dict(connection),
            "freshness": self._freshness_dict(
                connection, status_received_at, current_time
            ),
            "status": self._status_dict(
                status,
                sensing_health=self._sensing_health(
                    status, connection, status_received_at, current_time
                ),
            ),
            "recent_events": [
                {**event.to_dict(), "received_at": received_at}
                for event, received_at in reversed(recent)
            ],
            "positioned_remote_id_entities": positioned_remote_ids,
            "position_retention": {
                "seconds": self._remote_id_hold_seconds,
                "capacity": self._max_remote_id_entities,
            },
            "diagnostics": {
                "malformed_lines": connection.malformed_frames if connection else None,
                "overlong_lines": connection.overlong_lines if connection else None,
                "history_available": history_available,
                "history_error": history_error,
                "persistence_queue_depth": self._persistence_queue.qsize(),
                "persistence_drops": persistence_drops,
            },
        }

    def query_history(self, query: HistoryQuery) -> HistoryPage:
        """Read a bounded history page without involving the serial callback."""

        try:
            result = self._store.query(query)
        except Exception as error:
            self._record_history_error(error)
            raise
        self._record_history_success()
        return result

    def export_history(self, query: HistoryQuery) -> Iterator[object]:
        """Stream history while recording storage degradation on iteration."""

        try:
            yield from self._store.iter_export(query)
        except Exception as error:
            self._record_history_error(error)
            raise
        self._record_history_success()

    def clear_history(self, timeout: float = 3.0) -> int:
        """Delete history after every mutation queued before this call."""

        result = self._ordered_mutation("clear", timeout=timeout)
        return int(result)

    def prune_history(self, now: float | None = None, timeout: float = 3.0) -> int:
        """Run explicit retention as an ordered persistence barrier."""

        result = self._ordered_mutation(
            "prune", received_at=self._now() if now is None else now, timeout=timeout
        )
        return int(result)

    def display_nav(self, action: str, timeout: float = 5.0) -> ControlReply:
        self._require_display_controls()
        return self._send_control(build_display_nav(action), timeout)

    def set_theme(self, payload: object, timeout: float = 5.0) -> ControlReply:
        self._require_display_controls()
        return self._send_control(build_theme(payload), timeout)

    def reset_theme(self, timeout: float = 5.0) -> ControlReply:
        self._require_display_controls()
        return self._send_control(build_theme_reset(), timeout)

    def set_display_policy(
        self, payload: object, timeout: float = 5.0
    ) -> ControlReply:
        self._require_display_controls()
        return self._send_control(build_display_policy(payload), timeout)

    def reset_display_policy(self, timeout: float = 5.0) -> ControlReply:
        self._require_display_controls()
        return self._send_control(build_display_policy_reset(), timeout)

    def get_lite_config(self, timeout: float = 5.0) -> LiteConfiguration:
        with self._lock:
            transport = self._transport if self._accepting else None
        if transport is None:
            raise TransportUnavailable()
        return transport.get_lite_config(timeout=timeout)

    def set_lite_config(
        self, payload: object, timeout: float = 5.0
    ) -> LiteConfigWriteReply:
        with self._lock:
            transport = self._transport if self._accepting else None
        if transport is None:
            raise TransportUnavailable()
        return transport.set_lite_config(payload, timeout=timeout)

    def select_port(self, port: str, timeout: float = 3.0) -> None:
        """Reconnect the local serial bridge to a browser-selected badge port."""

        with self._lock:
            transport = self._transport if self._accepting else None
        if transport is None:
            raise TransportUnavailable()
        transport.select_port(port, timeout=timeout)

    def _require_display_controls(self) -> None:
        with self._lock:
            status = self._status
        if status is None:
            return
        raw = status.raw
        capabilities = raw.get("capabilities")
        if (
            raw.get("product_family") == "badge_lite"
            and raw.get("target") == "uplink-s3-backend"
            and raw.get("project") == "fof_backend_uplink"
            and raw.get("hardware") == "seeed_xiao_esp32s3"
            and raw.get("mode") == "headless"
            and isinstance(capabilities, tuple)
            and "display_none" in capabilities
        ):
            raise TransportUnavailable(
                "Backend Badge Lite is headless and has no display controls."
            )

    def _send_control(
        self, command: BadgeControlCommand, timeout: float
    ) -> ControlReply:
        with self._lock:
            transport = self._transport if self._accepting else None
        if transport is None:
            raise TransportUnavailable()
        return transport.send_control(command, timeout=timeout)

    def _persistence_barrier(self, timeout: float = 3.0) -> bool:
        action = _PersistenceAction("barrier", completion=threading.Event())
        if not self._enqueue(action):
            return False
        if not action.completion.wait(timeout):
            return False
        return action.error is None

    def _ordered_mutation(
        self,
        kind: str,
        *,
        received_at: float | None = None,
        timeout: float,
    ) -> object:
        action = _PersistenceAction(
            kind, received_at=received_at, completion=threading.Event()
        )
        if not self._enqueue(action):
            raise ApplicationError(
                "history_busy", "History persistence is currently unavailable."
            )
        if not action.completion.wait(timeout):
            raise ApplicationError(
                "history_timeout", "The history operation did not finish in time."
            )
        if action.error is not None:
            raise ApplicationError("history_unavailable", self._bounded_error(action.error))
        return action.result

    def close(self, timeout: float = _DEFAULT_CLOSE_TIMEOUT_SECONDS) -> None:
        """Drain accepted work and release storage within one caller deadline."""

        if timeout < 0:
            raise ValueError("timeout must be nonnegative")
        deadline = time.monotonic() + timeout
        with self._lock:
            self._closed = True
            self._accepting = False

        if not self._close_attempt_lock.acquire(timeout=self._remaining(deadline)):
            if self._worker.is_alive():
                raise ApplicationError("stop_timeout", _STOP_TIMEOUT_MESSAGE)
            raise ApplicationError(
                "history_unavailable",
                "History store shutdown is still in progress.",
            )
        try:
            if not self._worker.is_alive() and self._store_closed:
                return

            if not self._worker_stop.is_set():
                remaining = self._remaining(deadline)
                cancellation_reserve = min(
                    _CANCELLATION_RESERVE_SECONDS,
                    remaining / 2.0,
                )
                drain_deadline = deadline - cancellation_reserve
                barrier = _PersistenceAction(
                    "barrier", completion=threading.Event()
                )
                try:
                    self._persistence_queue.put(
                        barrier, timeout=self._remaining(drain_deadline)
                    )
                except Full:
                    pass
                except Exception as error:
                    self._record_history_error(error)
                else:
                    barrier.completion.wait(self._remaining(drain_deadline))

                drain_completed = (
                    barrier.completion.is_set() and barrier.error is None
                )
                if not drain_completed:
                    try:
                        self._discard_queued_actions()
                    except Exception as error:
                        self._record_history_error(error)
                self._worker_stop.set()

                if not drain_completed:
                    try:
                        self._store.cancel_pending()
                    except Exception as error:
                        self._record_history_error(error)
            elif self._worker.is_alive():
                try:
                    self._store.cancel_pending()
                except Exception as error:
                    self._record_history_error(error)

            try:
                self._worker.join(self._remaining(deadline))
            except RuntimeError as error:
                self._record_history_error(error)

            if self._worker.is_alive():
                try:
                    self._store.cancel_pending()
                except Exception as error:
                    self._record_history_error(error)

            store_error: Exception | None = None
            if not self._store_closed:
                try:
                    self._store.close(timeout=self._remaining(deadline))
                except Exception as error:
                    store_error = error
                    self._record_history_error(error)
                else:
                    self._store_closed = True

            if self._worker.is_alive():
                raise ApplicationError("stop_timeout", _STOP_TIMEOUT_MESSAGE)
            if store_error is not None:
                raise ApplicationError(
                    "history_unavailable",
                    self._bounded_error(store_error),
                )
        finally:
            self._close_attempt_lock.release()

    def _discard_queued_actions(self) -> None:
        dropped_records = 0
        while True:
            try:
                action = self._persistence_queue.get_nowait()
            except Empty:
                break
            if action.kind in {"event", "track"}:
                dropped_records += 1
            if action.kind == "track":
                self._finish_track_action(action, succeeded=False)
            action.error = ApplicationError(
                "application_closing", "The application stopped before this action ran."
            )
            if action.completion is not None:
                action.completion.set()
            self._persistence_queue.task_done()
        if dropped_records:
            with self._lock:
                self._persistence_drops += dropped_records

    def _enqueue(self, action: _PersistenceAction) -> bool:
        with self._lock:
            return self._enqueue_locked(action)

    def _enqueue_locked(self, action: _PersistenceAction) -> bool:
        if not self._accepting and action.kind != "barrier":
            self._record_submission_failure(action, "The application is closing.")
            return False
        try:
            self._persistence_queue.put_nowait(action)
        except Full:
            self._record_submission_failure(action, "The persistence queue is full.")
            return False
        return True

    def _record_submission_failure(
        self, action: _PersistenceAction, message: str
    ) -> None:
        if action.kind in {"event", "track"}:
            self._record_drop()
            if action.kind == "track":
                self._finish_track_action(action, succeeded=False)
            return
        self._record_history_error(ApplicationError("history_busy", message))

    def _run_persistence(self) -> None:
        while not self._worker_stop.is_set() or not self._persistence_queue.empty():
            try:
                action = self._persistence_queue.get(timeout=0.05)
            except Empty:
                continue
            try:
                storage_succeeded = False
                if action.kind == "event":
                    self._store.add_event(
                        cast(DetectionEvent, action.value),
                        cast(float, action.received_at),
                    )
                    storage_succeeded = True
                elif action.kind == "track":
                    self._store.add_track(
                        cast(BadgeEntity, action.value),
                        cast(float, action.received_at),
                    )
                    self._finish_track_action(action, succeeded=True)
                    storage_succeeded = True
                elif action.kind == "prune":
                    action.result = self._store.prune(action.received_at)
                    storage_succeeded = True
                elif action.kind == "clear":
                    action.result = self._store.clear()
                    storage_succeeded = True
                if storage_succeeded:
                    self._record_history_success()
            except Exception as error:
                action.error = error
                if action.kind == "track":
                    self._finish_track_action(action, succeeded=False)
                self._record_history_error(error)
                if action.kind in {"event", "track"}:
                    self._record_drop()
            finally:
                if action.completion is not None:
                    action.completion.set()
                self._persistence_queue.task_done()

    def _prune_if_due(self, now: float) -> None:
        with self._lock:
            if not self._accepting:
                return
            last = self._last_prune_queued_at
            if last is not None and now - last < _PRUNE_INTERVAL_SECONDS:
                return
            self._last_prune_queued_at = now
        if not self._enqueue(_PersistenceAction("prune", received_at=now)):
            with self._lock:
                if self._last_prune_queued_at == now:
                    self._last_prune_queued_at = last

    def _record_drop(self) -> None:
        with self._lock:
            self._persistence_drops += 1

    def _finish_track_action(
        self, action: _PersistenceAction, *, succeeded: bool
    ) -> None:
        stable_key = action.track_key
        fingerprint = action.track_fingerprint
        action_id = action.track_action_id
        if stable_key is None or fingerprint is None or action_id is None:
            return
        with self._lock:
            if succeeded and stable_key in self._positioned_remote_ids:
                self._persisted_track_fingerprints[stable_key] = fingerprint
            pending = self._pending_track_actions.get(stable_key)
            if pending is None:
                return
            for index, (pending_id, _pending_fingerprint) in enumerate(pending):
                if pending_id == action_id:
                    del pending[index]
                    break
            if not pending:
                del self._pending_track_actions[stable_key]
                if stable_key not in self._positioned_remote_ids:
                    self._persisted_track_fingerprints.pop(stable_key, None)

    def _record_history_error(self, error: BaseException) -> None:
        message = self._bounded_error(error)
        with self._lock:
            self._history_available = False
            self._history_error = message

    def _record_history_success(self) -> None:
        with self._lock:
            self._history_available = True
            self._history_error = None

    @classmethod
    def _track_fingerprint(
        cls, entity: BadgeEntity, received_at: float
    ) -> tuple[object, ...]:
        return (
            entity.latitude,
            entity.longitude,
            entity.altitude_m,
            entity.events,
            entity.seen_count,
            cls._position_observed_at(entity, received_at),
        )

    @staticmethod
    def _position_observed_at(entity: BadgeEntity, received_at: float) -> float:
        age = min(max(entity.last_seen_seconds or 0.0, 0.0), 300.0)
        return received_at - age

    def _hydrate_positioned_remote_ids(self, now: float) -> None:
        loader = getattr(self._store, "latest_positioned_tracks", None)
        if not self._history_available or not callable(loader):
            return
        try:
            tracks = loader(
                since=now - self._remote_id_hold_seconds,
                limit=self._max_remote_id_entities,
            )
        except Exception as error:
            self._record_history_error(error)
            return
        with self._lock:
            for track in reversed(tuple(tracks)):
                if not isinstance(track, Observation):
                    continue
                if now - track.observed_at >= self._remote_id_hold_seconds:
                    continue
                self._remember_position_payload_locked(
                    track.stable_key,
                    self._position_payload_from_observation(track),
                    track.observed_at,
                )
                self._persisted_track_fingerprints[track.stable_key] = (
                    track.latitude,
                    track.longitude,
                    track.altitude_m,
                    track.events,
                    track.seen_count,
                    track.observed_at,
                )

    def _remember_positioned_remote_id_locked(
        self, entity: BadgeEntity, received_at: float
    ) -> None:
        observed_at = self._position_observed_at(entity, received_at)
        payload = entity.to_dict()
        payload.update({
            "stable_key": entity.stable_key,
            "is_remote_id": True,
            "has_position": True,
        })
        self._remember_position_payload_locked(
            entity.stable_key, payload, observed_at
        )

    def _remember_position_payload_locked(
        self,
        stable_key: str,
        payload: dict[str, object],
        observed_at: float,
    ) -> None:
        previous = self._positioned_remote_ids.get(stable_key)
        if previous is not None and observed_at < previous[1]:
            return
        self._positioned_remote_ids[stable_key] = (dict(payload), observed_at)
        while len(self._positioned_remote_ids) > self._max_remote_id_entities:
            oldest_key = min(
                self._positioned_remote_ids,
                key=lambda key: (self._positioned_remote_ids[key][1], key),
            )
            del self._positioned_remote_ids[oldest_key]
            if oldest_key not in self._pending_track_actions:
                self._persisted_track_fingerprints.pop(oldest_key, None)

    def _positioned_remote_id_snapshot_locked(
        self, status: BadgeStatus | None, now: float
    ) -> list[dict[str, object]]:
        cutoff = now - self._remote_id_hold_seconds
        expired = [
            stable_key
            for stable_key, (_payload, observed_at) in self._positioned_remote_ids.items()
            if observed_at <= cutoff
        ]
        for stable_key in expired:
            del self._positioned_remote_ids[stable_key]
            if stable_key not in self._pending_track_actions:
                self._persisted_track_fingerprints.pop(stable_key, None)

        active_entities = (
            status.entities
            if status is not None and status.entities is not None
            else ()
        )
        active_keys = {
            entity.stable_key
            for entity in active_entities
            if not entity.stale and entity.is_remote_id and entity.has_position
        }
        rendered: list[dict[str, object]] = []
        ordered = sorted(
            self._positioned_remote_ids.items(),
            key=lambda item: (item[1][1], item[0]),
            reverse=True,
        )
        for stable_key, (payload, observed_at) in ordered:
            age = max(now - observed_at, 0.0)
            item = dict(payload)
            item.update({
                "stable_key": stable_key,
                "stale": False,
                "last_seen_s": round(age, 1),
                "host_age_s": round(age, 1),
                "host_observed_at": observed_at,
                "host_retained": stable_key not in active_keys,
                "position_retention_s": self._remote_id_hold_seconds,
                "is_remote_id": True,
                "has_position": True,
            })
            rendered.append(item)
        return rendered

    @staticmethod
    def _position_payload_from_observation(
        observation: Observation,
    ) -> dict[str, object]:
        return {
            "label": observation.label or None,
            "detail": None,
            "evidence": "Host-retained GPS position received over USB.",
            "class": observation.threat_class or None,
            "category": observation.category or None,
            "code": None,
            "display_id": observation.display_id or None,
            "source_id": observation.source_id,
            "source": observation.source,
            "score": observation.score,
            "confidence_pct": observation.confidence,
            "last_seen_s": None,
            "rssi": observation.rssi,
            "best_rssi": observation.rssi,
            "events": observation.events,
            "seen_count": observation.seen_count,
            "stale": False,
            "lat": observation.latitude,
            "lon": observation.longitude,
            "altitude_m": observation.altitude_m,
            "operator_lat": observation.operator_latitude,
            "operator_lon": observation.operator_longitude,
            "operator_id": observation.operator_id or None,
            "ssid": None,
            "bssid": None,
            "manufacturer": observation.manufacturer or None,
            "extras": dict(observation.extras),
            "stable_key": observation.stable_key,
            "is_remote_id": True,
            "has_position": True,
        }

    @staticmethod
    def _status_dict(
        status: BadgeStatus | None, *, sensing_health: str
    ) -> dict[str, object]:
        if status is None:
            return {
                "version": None,
                "uptime_s": None,
                "mode": None,
                "mode_label": None,
                "safe_mode": None,
                "safe_reason": None,
                "recovery_mode": None,
                "threat_score": None,
                "counts": None,
                "scanners": None,
                "entities": None,
                "remote_id_entities": None,
                "reporting": None,
                "memory": None,
                "display_state": None,
                "theme": None,
                "display_policy": None,
                "sensing_health": sensing_health,
            }
        result = status.to_dict()
        result.update({
            "version": status.version,
            "uptime_s": status.uptime_seconds,
            "mode": status.mode,
            "mode_label": status.mode_label,
            "safe_mode": status.safe_mode,
            "safe_reason": result.get("safe_reason"),
            "recovery_mode": status.recovery_mode,
            "threat_score": status.threat_score,
            "counts": result.get("counts") if status.counts is not None else None,
            "reporting": result.get("reporting"),
            "memory": result.get("memory"),
            "display_state": result.get("display_state"),
            "theme": result.get("theme"),
            "display_policy": result.get("display_policy"),
            "sensing_health": sensing_health,
        })
        if status.entities is not None:
            entities = result["entities"]
            for entity, rendered in zip(status.entities, entities):
                rendered["is_remote_id"] = entity.is_remote_id
                rendered["has_position"] = entity.has_position
            result["remote_id_entities"] = [
                rendered
                for entity, rendered in zip(status.entities, entities)
                if entity.is_remote_id
            ]
        return result

    @staticmethod
    def _connection_dict(update: ConnectionUpdate | None) -> dict[str, object]:
        if update is None:
            return {
                "phase": "unavailable",
                "detail": "not_connected",
                "port": None,
                "candidates": [],
                "firmware_version": None,
                "reconnect_attempt": None,
            }
        return {
            "phase": update.state,
            "detail": update.detail,
            "port": update.port,
            "candidates": [list(candidate) for candidate in update.candidates],
            "firmware_version": update.firmware_version,
            "reconnect_attempt": update.reconnect_attempt,
        }

    @staticmethod
    def _freshness_dict(
        connection: ConnectionUpdate | None,
        received_at: float | None,
        now: float,
    ) -> dict[str, object]:
        if received_at is None:
            return {"state": "unavailable", "age_s": None}
        exact_age = max(now - received_at, 0.0)
        state = (
            "fresh"
            if connection is not None
            and connection.state == "live"
            and exact_age < 6.0
            else "stale"
        )
        return {"state": state, "age_s": round(exact_age, 1)}

    @classmethod
    def _sensing_health(
        cls,
        status: BadgeStatus | None,
        connection: ConnectionUpdate | None,
        received_at: float | None,
        now: float,
    ) -> str:
        if status is None:
            return "unknown"
        if status.safe_mode is True or status.recovery_mode == "safe_usb":
            return "safe_usb"
        freshness = cls._freshness_dict(connection, received_at, now)["state"]
        if freshness != "fresh":
            return "degraded"
        if not status.scanners:
            return "unknown"
        has_incomplete_evidence = False
        for scanner in status.scanners:
            if not isinstance(scanner, Mapping):
                has_incomplete_evidence = True
                continue
            connected = scanner.get("connected")
            health = scanner.get("health")
            if connected is False or (
                isinstance(health, str) and health not in {"ok", "healthy"}
            ):
                return "degraded"
            if (
                connected is not True
                or not isinstance(health, str)
                or health not in {"ok", "healthy"}
            ):
                has_incomplete_evidence = True
        if has_incomplete_evidence:
            return "unknown"
        return "healthy"

    @staticmethod
    def _bounded_error(error: BaseException) -> str:
        return f"{type(error).__name__}: {error}"[:_HISTORY_ERROR_LIMIT]

    def _now(self) -> float:
        return self._wall_clock()

    @staticmethod
    def _remaining(deadline: float) -> float:
        return max(0.0, deadline - time.monotonic())
