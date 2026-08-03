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
from .models import BadgeEntity, BadgeStatus, ControlReply, DetectionEvent, MachineFrame
from .serial_transport import ConnectionUpdate, TransportUnavailable
from .storage import HistoryPage, HistoryQuery, ObservationStore


_PERSISTENCE_CAPACITY = 1_024
_HISTORY_ERROR_LIMIT = 256
_PRUNE_INTERVAL_SECONDS = 3_600.0
_DEFAULT_CLOSE_TIMEOUT_SECONDS = 3.0
_CANCELLATION_RESERVE_SECONDS = 0.5
_STOP_TIMEOUT_MESSAGE = "New Dash did not stop within the shutdown timeout."


class BadgeTransportLike(Protocol):
    """The safe application-facing portion of the serial transport."""

    def send_control(
        self, command: BadgeControlCommand, timeout: float = 5.0
    ) -> ControlReply: ...


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
    ) -> None:
        self._store = store
        self._wall_clock = wall_clock
        self._lock = threading.RLock()
        self._status: BadgeStatus | None = None
        self._status_received_at: float | None = None
        self._connection: ConnectionUpdate | None = None
        self._recent_events: deque[tuple[DetectionEvent, float]] = deque(maxlen=20)
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
                    fingerprint = self._track_fingerprint(entity)
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
        return self._send_control(build_display_nav(action), timeout)

    def set_theme(self, payload: object, timeout: float = 5.0) -> ControlReply:
        return self._send_control(build_theme(payload), timeout)

    def reset_theme(self, timeout: float = 5.0) -> ControlReply:
        return self._send_control(build_theme_reset(), timeout)

    def set_display_policy(
        self, payload: object, timeout: float = 5.0
    ) -> ControlReply:
        return self._send_control(build_display_policy(payload), timeout)

    def reset_display_policy(self, timeout: float = 5.0) -> ControlReply:
        return self._send_control(build_display_policy_reset(), timeout)

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
            if succeeded:
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

    def _record_history_error(self, error: BaseException) -> None:
        message = self._bounded_error(error)
        with self._lock:
            self._history_available = False
            self._history_error = message

    def _record_history_success(self) -> None:
        with self._lock:
            self._history_available = True
            self._history_error = None

    @staticmethod
    def _track_fingerprint(entity: BadgeEntity) -> tuple[object, ...]:
        return (
            entity.latitude,
            entity.longitude,
            entity.altitude_m,
            entity.events,
            entity.seen_count,
        )

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
