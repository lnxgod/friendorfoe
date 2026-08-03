"""macOS badge discovery and one verified live serial-session owner."""

from __future__ import annotations

from collections import deque
from collections.abc import Callable, Iterable
from dataclasses import dataclass, field
import json
import threading
import time
from typing import Any

from .controls import BadgeControlCommand, build_lite_config_set
from .models import (
    BadgeStatus,
    ControlReply,
    LiteConfiguration,
    LiteConfigWriteReply,
    LiteLiveHeartbeat,
    LiteLiveReady,
    MachineFrame,
)
from .protocol import LineFramer, MachineFrameError, parse_machine_line


ESPRESSIF_VID = 0x303A
USB_SERIAL_JTAG_PID = 0x1001


@dataclass(frozen=True, slots=True)
class PortIdentity:
    """PySerial port metadata copied into an immutable application value."""

    device: str
    vid: int | None
    pid: int | None
    manufacturer: str | None
    product: str | None
    serial_number: str | None
    location: str | None


class DiscoveryError(RuntimeError):
    """A stable discovery failure with structured physical candidates."""

    def __init__(
        self,
        code: str,
        message: str,
        candidates: tuple[tuple[str, ...], ...] = (),
    ) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message
        self.candidates = candidates


class TransportError(RuntimeError):
    """A stable serial-transport operation failure."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


class TransportUnavailable(TransportError):
    """A mutation cannot run without a freshly verified live session."""

    def __init__(self, message: str = "The badge transport is unavailable.") -> None:
        super().__init__("transport_unavailable", message)


class ControlTimeout(TransportError):
    """The badge did not acknowledge the outstanding control in time."""

    def __init__(self, message: str = "The badge control reply timed out.") -> None:
        super().__init__("control_timeout", message)


class UnsupportedCapability(TransportError):
    """The verified device family does not support the requested operation."""

    def __init__(
        self,
        message: str = "The connected badge does not support this operation.",
    ) -> None:
        super().__init__("unsupported_capability", message)


@dataclass(frozen=True, slots=True)
class ConnectionUpdate:
    """An immutable connection-state observation."""

    state: str
    detail: str
    port: str | None = None
    candidates: tuple[tuple[str, ...], ...] = ()
    firmware_version: str | None = None
    last_valid_status_at: float | None = None
    malformed_frames: int = 0
    overlong_lines: int = 0
    reconnect_attempt: int = 0


@dataclass(slots=True)
class _ControlRequest:
    command: BadgeControlCommand
    generation: int
    timeout: float
    deadline: float | None = None
    completion: threading.Event = field(default_factory=threading.Event)
    reply: ControlReply | None = None
    error: BaseException | None = None


@dataclass(slots=True)
class _ProtocolRequest:
    wire: bytes
    expected_kinds: frozenset[str]
    generation: int
    timeout: float
    deadline: float | None = None
    completion: threading.Event = field(default_factory=threading.Event)
    reply: object | None = None
    error: BaseException | None = None


_PendingRequest = _ControlRequest | _ProtocolRequest


_LITE_IDENTITY = {
    "product_family": "badge_lite",
    "target": "uplink-s3-backend",
    "project": "fof_backend_uplink",
    "hardware": "seeed_xiao_esp32s3",
    "mode": "headless",
}
_LITE_REQUIRED_CAPABILITIES = frozenset({"display_none", "usb_live", "usb_live_ack"})
_LIVE_START = b'FOF_LIVE_START:{"client":"new_dash","protocol":1}\n'
_LIVE_RETRY_SECONDS = 15.0


@dataclass(frozen=True, slots=True)
class _SessionOutcome:
    verified: bool
    detail: str
    identity: PortIdentity | None = None
    firmware_version: str | None = None
    candidates: tuple[tuple[str, ...], ...] = ()


def discover_badge_ports(
    enumerate_ports: Callable[[], Iterable[Any]] | None = None,
) -> tuple[PortIdentity, ...]:
    """Copy PySerial list-port records at the adapter boundary."""

    adapter = enumerate_ports or _default_enumerate_ports
    return tuple(
        PortIdentity(
            device=port.device,
            vid=getattr(port, "vid", None),
            pid=getattr(port, "pid", None),
            manufacturer=getattr(port, "manufacturer", None),
            product=getattr(port, "product", None),
            serial_number=getattr(port, "serial_number", None),
            location=getattr(port, "location", None),
        )
        for port in adapter()
    )


def choose_candidate(
    ports: Iterable[PortIdentity],
    *,
    explicit_path: str | None = None,
) -> PortIdentity:
    """Select one physical candidate without guessing between badges."""

    available = tuple(ports)
    if explicit_path is not None:
        for port in available:
            if port.device == explicit_path:
                return port
        raise DiscoveryError(
            "explicit_port_missing",
            f"The requested serial port is not present: {explicit_path}",
        )

    exact = tuple(
        port
        for port in available
        if port.vid == ESPRESSIF_VID and port.pid == USB_SERIAL_JTAG_PID
    )
    groups = _physical_groups(exact)
    candidate_paths = _candidate_paths(exact)
    if not groups:
        raise DiscoveryError(
            "no_badge",
            "No Espressif USB Serial/JTAG badge was found.",
        )
    if len(groups) > 1:
        raise DiscoveryError(
            "multiple_badges",
            "Multiple badge devices were found; select an explicit port.",
            candidate_paths,
        )
    return min(groups[0], key=lambda port: _path_preference(port.device))


def _physical_groups(
    ports: tuple[PortIdentity, ...],
) -> tuple[tuple[PortIdentity, ...], ...]:
    groups: dict[tuple[str, str], list[PortIdentity]] = {}
    for port in ports:
        key = _physical_key(port)
        groups.setdefault(key, []).append(port)
    return tuple(tuple(group) for group in groups.values())


def _candidate_paths(
    ports: tuple[PortIdentity, ...],
) -> tuple[tuple[str, ...], ...]:
    return tuple(
        tuple(sorted((port.device for port in group), key=_path_preference))
        for group in _physical_groups(ports)
    )


def _physical_key(port: PortIdentity) -> tuple[str, str]:
    if port.serial_number:
        return ("serial", port.serial_number)
    if port.location:
        return ("location", port.location)
    return ("path", _macos_alias_suffix(port.device))


def _macos_alias_suffix(device: str) -> str:
    for prefix in ("/dev/cu.", "/dev/tty."):
        if device.startswith(prefix):
            return device.removeprefix(prefix)
    return device


def _path_preference(device: str) -> tuple[int, str]:
    return (0 if device.startswith("/dev/cu.") else 1, device)


class BadgeSerialTransport:
    """Own discovery, verification, polling, controls, and reconnect lifecycle."""

    def __init__(
        self,
        *,
        explicit_port: str | None = None,
        enumerate_ports: Callable[[], Iterable[Any]] | None = None,
        serial_factory: Callable[[], Any] | None = None,
        wall_clock: Callable[[], float] = time.time,
        monotonic_clock: Callable[[], float] = time.monotonic,
        on_frame: Callable[[MachineFrame, float], None] | None = None,
        on_connection: Callable[[ConnectionUpdate], None] | None = None,
    ) -> None:
        self._explicit_port = explicit_port
        self._enumerate_ports = enumerate_ports or _default_enumerate_ports
        self._serial_factory = serial_factory or _default_serial_factory
        self._wall_clock = wall_clock
        self._monotonic_clock = monotonic_clock
        self._shutdown_clock = time.monotonic
        self._on_frame = on_frame or (lambda _frame, _received_at: None)
        self._on_connection = on_connection or (lambda _update: None)
        self._write_lock = threading.Lock()
        self._lifecycle_lock = threading.Lock()
        self._control_lock = threading.Lock()
        self._side_effect_condition = threading.Condition()
        self._stop_requests: set[threading.Event] = set()
        self._active_side_effects: dict[threading.Event, int] = {}
        self._worker: threading.Thread | None = None
        self._worker_stop: threading.Event | None = None
        self._stopping = False
        self._remembered_identity: PortIdentity | None = None
        self._malformed_frames = 0
        self._overlong_lines = 0
        self._last_valid_status_at: float | None = None
        self._last_valid_status_monotonic: float | None = None
        self._generation = 0
        self._live_generation: int | None = None
        self._verified_family: str | None = None
        self._verified_capabilities: frozenset[str] = frozenset()
        self._control_queue: deque[_PendingRequest] = deque()
        self._outstanding_control: _PendingRequest | None = None

    def start(self) -> None:
        """Start one non-daemon discovery and verified-session worker."""

        with self._lifecycle_lock:
            if self._stopping:
                raise TransportError(
                    "lifecycle_busy", "The serial worker is currently stopping."
                )
            if self._worker is not None and self._worker.is_alive():
                raise TransportError(
                    "already_started", "The serial worker is already running."
                )
            stop_event = threading.Event()
            worker = threading.Thread(
                target=self._run_one_session,
                args=(stop_event,),
                name="new-dash-serial",
                daemon=False,
            )
            self._worker_stop = stop_event
            self._worker = worker
            try:
                worker.start()
            except Exception:
                self._worker = None
                self._worker_stop = None
                raise

    def stop(self, timeout: float = 3.0) -> None:
        """Request worker shutdown and wait for the owned handle to close."""

        if timeout < 0:
            raise ValueError("timeout must be nonnegative")
        deadline = self._shutdown_clock() + timeout
        with self._lifecycle_lock:
            if self._stopping:
                raise TransportError(
                    "lifecycle_busy", "The serial worker is currently stopping."
                )
            worker = self._worker
            stop_event = self._worker_stop
            if worker is None or stop_event is None:
                return
            self._stopping = True
            is_current_worker = worker is threading.current_thread()
        self._commit_stop(
            stop_event,
            deadline=deadline,
            wait_for_active=not is_current_worker,
        )
        if is_current_worker:
            with self._lifecycle_lock:
                self._stopping = False
            return
        worker.join(max(0.0, deadline - self._shutdown_clock()))
        if worker.is_alive():
            with self._lifecycle_lock:
                self._stopping = False
            raise TransportError(
                "stop_timeout", "The serial worker did not stop in time."
            )
        with self._side_effect_condition:
            self._stop_requests.discard(stop_event)
        with self._lifecycle_lock:
            if self._worker is worker:
                self._worker = None
            if self._worker_stop is stop_event:
                self._worker_stop = None
            self._stopping = False

    def _commit_stop(
        self,
        stop_event: threading.Event,
        *,
        deadline: float,
        wait_for_active: bool,
    ) -> None:
        with self._side_effect_condition:
            self._stop_requests.add(stop_event)
            stop_event.set()
            if wait_for_active:
                while self._active_side_effects.get(stop_event, 0):
                    remaining = deadline - self._shutdown_clock()
                    if remaining <= 0:
                        return
                    self._side_effect_condition.wait(remaining)

    def _begin_side_effect(self, stop_event: threading.Event) -> bool:
        with self._side_effect_condition:
            if stop_event.is_set() or stop_event in self._stop_requests:
                return False
            self._active_side_effects[stop_event] = (
                self._active_side_effects.get(stop_event, 0) + 1
            )
            return True

    def _end_side_effect(self, stop_event: threading.Event) -> None:
        with self._side_effect_condition:
            remaining = self._active_side_effects[stop_event] - 1
            if remaining:
                self._active_side_effects[stop_event] = remaining
            else:
                del self._active_side_effects[stop_event]
                self._side_effect_condition.notify_all()

    def send_control(
        self,
        command: BadgeControlCommand,
        timeout: float = 5.0,
    ) -> ControlReply:
        """Queue one allowlisted mutation for the verified serial worker."""

        if timeout < 0:
            raise ValueError("timeout must be nonnegative")
        with self._control_lock:
            generation = self._live_generation
            if generation is None:
                raise TransportUnavailable()
            if self._verified_family is None:
                raise TransportUnavailable(
                    "A fresh badge status is required before controls are enabled."
                )
            if self._verified_family == "badge_lite":
                raise UnsupportedCapability(
                    "Backend Badge Lite is headless and has no display controls."
                )
            if self._verified_family != "regular_badge":
                raise UnsupportedCapability()
            request = _ControlRequest(
                command=command,
                generation=generation,
                timeout=timeout,
            )
            self._control_queue.append(request)
        request.completion.wait()
        if request.error is not None:
            raise request.error
        if request.reply is None:
            raise TransportUnavailable("The badge control ended without a reply.")
        return request.reply

    def get_lite_config(self, timeout: float = 5.0) -> LiteConfiguration:
        """Read one freshly redacted canonical Lite configuration."""

        reply = self._send_protocol_request(
            b"FOF_CONFIG_GET\n",
            frozenset({"config"}),
            timeout,
            required_capability="usb_config",
        )
        if not isinstance(reply, LiteConfiguration):
            raise TransportUnavailable("The Lite configuration reply was unavailable.")
        return reply

    def set_lite_config(
        self, payload: object, timeout: float = 5.0
    ) -> LiteConfigWriteReply:
        """Commit one validated atomic Lite configuration update."""

        wire = build_lite_config_set(payload)
        reply = self._send_protocol_request(
            wire,
            frozenset({"config_ok", "config_error"}),
            timeout,
            required_capability="usb_config",
        )
        if not isinstance(reply, LiteConfigWriteReply):
            raise TransportUnavailable("The Lite configuration result was unavailable.")
        return reply

    def _send_protocol_request(
        self,
        wire: bytes,
        expected_kinds: frozenset[str],
        timeout: float,
        *,
        required_capability: str,
    ) -> object:
        if timeout < 0:
            raise ValueError("timeout must be nonnegative")
        with self._control_lock:
            generation = self._live_generation
            if generation is None:
                raise TransportUnavailable()
            if self._verified_family != "badge_lite":
                raise UnsupportedCapability(
                    "Lite USB configuration requires a freshly verified Backend Badge Lite."
                )
            if required_capability not in self._verified_capabilities:
                raise UnsupportedCapability()
            request = _ProtocolRequest(
                wire=wire,
                expected_kinds=expected_kinds,
                generation=generation,
                timeout=timeout,
            )
            self._control_queue.append(request)
        request.completion.wait()
        if request.error is not None:
            raise request.error
        if request.reply is None:
            raise TransportUnavailable("The Lite protocol operation ended without a reply.")
        return request.reply

    def _run_one_session(self, stop_event: threading.Event) -> None:
        reconnect_attempt = 0
        while not stop_event.is_set():
            outcome = self._run_session(stop_event)
            if stop_event.is_set():
                return
            if outcome.verified:
                reconnect_attempt = 0
            reconnect_attempt += 1
            self._emit(
                ConnectionUpdate(
                    state="reconnecting",
                    detail=outcome.detail,
                    port=(
                        outcome.identity.device
                        if outcome.identity is not None
                        else None
                    ),
                    candidates=outcome.candidates,
                    firmware_version=outcome.firmware_version,
                    last_valid_status_at=self._last_valid_status_at,
                    malformed_frames=self._malformed_frames,
                    overlong_lines=self._overlong_lines,
                    reconnect_attempt=reconnect_attempt,
                )
            )
            delay = min(2.0 ** min(reconnect_attempt - 1, 4), 10.0)
            if self._wait_before_retry(stop_event, delay):
                return

    def _wait_before_retry(
        self,
        stop_event: threading.Event,
        delay: float,
    ) -> bool:
        return stop_event.wait(delay)

    def _run_session(self, stop_event: threading.Event) -> _SessionOutcome:
        serial_port: Any | None = None
        identity: PortIdentity | None = None
        generation: int | None = None
        firmware_version: str | None = None
        verified = False
        detail = "serial_error"
        try:
            try:
                ports = self._read_port_identities()
                identity = self._choose_for_start(ports)
            except DiscoveryError as error:
                state = (
                    "waiting"
                    if error.code in {"no_badge", "explicit_port_missing"}
                    else "error"
                )
                self._emit(
                    ConnectionUpdate(
                        state=state,
                        detail=error.code,
                        candidates=error.candidates,
                    )
                )
                return _SessionOutcome(
                    verified=False,
                    detail=error.code,
                    candidates=error.candidates,
                )

            self._emit(
                ConnectionUpdate(
                    state="connecting",
                    detail="verifying",
                    port=identity.device,
                )
            )
            serial_port = self._open_safely(identity)
            serial_port.reset_input_buffer()
            framer = LineFramer()
            if not self._write_if_running(
                serial_port,
                b"\nFOF_PING\n",
                stop_event,
            ):
                return _SessionOutcome(False, "stopped", identity)

            handshake_deadline = self._monotonic_clock() + 3.0
            firmware_version = self._await_pong(
                serial_port,
                framer,
                stop_event,
                handshake_deadline,
            )
            if firmware_version is None:
                if not stop_event.is_set():
                    self._emit(
                        ConnectionUpdate(
                            state="error",
                            detail="wrong_device",
                            port=identity.device,
                        )
                    )
                return _SessionOutcome(False, "wrong_device", identity)

            if self._handshake_ended(handshake_deadline, stop_event):
                if not stop_event.is_set():
                    self._emit(
                        ConnectionUpdate(
                            state="error",
                            detail="wrong_device",
                            port=identity.device,
                        )
                    )
                return _SessionOutcome(False, "wrong_device", identity)
            self._remembered_identity = identity
            generation = self._activate_generation()
            if self._handshake_ended(handshake_deadline, stop_event):
                if not stop_event.is_set():
                    self._emit(
                        ConnectionUpdate(
                            state="error",
                            detail="wrong_device",
                            port=identity.device,
                        )
                    )
                return _SessionOutcome(False, "wrong_device", identity)
            verified = True
            publication_now = self._monotonic_clock()
            if publication_now >= handshake_deadline or stop_event.is_set():
                return _SessionOutcome(False, "wrong_device", identity)
            initial_state = (
                "stale"
                if self._last_valid_status_monotonic is not None
                and publication_now - self._last_valid_status_monotonic >= 6.0
                else "live"
            )
            if not self._emit_session_update(
                initial_state,
                identity,
                firmware_version,
                detail=("status_stale" if initial_state == "stale" else None),
                stop_event=stop_event,
            ):
                return _SessionOutcome(True, "stopped", identity, firmware_version)
            detail = self._run_live_session(
                serial_port,
                framer,
                stop_event,
                identity,
                firmware_version,
                generation,
                initial_state,
            )
            return _SessionOutcome(True, detail, identity, firmware_version)
        except OSError:
            detail = "read_error" if serial_port is not None else "open_error"
            if not stop_event.is_set():
                self._emit(
                    ConnectionUpdate(
                        state="error",
                        detail=detail,
                        port=identity.device if identity is not None else None,
                    )
                )
            return _SessionOutcome(verified, detail, identity, firmware_version)
        except Exception:
            if not stop_event.is_set():
                self._emit(
                    ConnectionUpdate(
                        state="error",
                        detail="serial_error",
                        port=identity.device if identity is not None else None,
                    )
                )
            return _SessionOutcome(
                verified,
                "serial_error",
                identity,
                firmware_version,
            )
        finally:
            if generation is not None:
                self._fail_generation(generation, TransportUnavailable())
            if serial_port is not None:
                self._safe_close(serial_port)

    def _run_live_session(
        self,
        serial_port: Any,
        framer: LineFramer,
        stop_event: threading.Event,
        identity: PortIdentity,
        firmware_version: str,
        generation: int,
        initial_state: str,
    ) -> str:
        session_started = self._monotonic_clock()
        next_poll = session_started
        next_presence_check = session_started + 1.0
        last_valid_frame = session_started
        last_valid_status = (
            self._last_valid_status_monotonic
            if self._last_valid_status_monotonic is not None
            else session_started
        )
        state = initial_state
        live_started_at: float | None = None
        live_exchange_at: float | None = None
        live_session_id: str | None = None
        live_last_sequence: int | None = None
        try:
            while not stop_event.is_set():
                now = self._monotonic_clock()
                if self._control_timed_out(generation, now):
                    return "control_timeout"
                if now - last_valid_frame >= 12.0:
                    return "status_silent"
                if state == "live" and now - last_valid_status >= 6.0:
                    state = "stale"
                    self._emit_session_update(
                        state,
                        identity,
                        firmware_version,
                        detail="status_stale",
                        stop_event=stop_event,
                    )
                if live_started_at is not None:
                    lease_reference = live_exchange_at or live_started_at
                    if now - lease_reference >= _LIVE_RETRY_SECONDS:
                        if not self._write_if_running(serial_port, _LIVE_START, stop_event):
                            return "stopped"
                        live_started_at = now
                        live_exchange_at = None
                        live_session_id = None
                        live_last_sequence = None
                if now >= next_presence_check:
                    if not self._current_path_present(identity.device):
                        return "path_missing"
                    next_presence_check = now + 1.0
                if now >= next_poll:
                    if not self._write_if_running(
                        serial_port,
                        b"FOF_STATUS\n",
                        stop_event,
                    ):
                        return "stopped"
                    next_poll += 2.0
                    while next_poll <= now:
                        next_poll += 2.0

                request = self._take_next_control(generation)
                if request is not None:
                    wire = (
                        request.command.to_wire()
                        if isinstance(request, _ControlRequest)
                        else request.wire
                    )
                    if not self._write_if_running(serial_port, wire, stop_event):
                        return "stopped"
                    self._mark_control_transmitted(request, generation)

                prior_overlong = framer.overlong_lines
                chunk = serial_port.read(4096)
                if stop_event.is_set():
                    return "stopped"
                if not chunk:
                    continue
                lines = framer.feed(chunk)
                if framer.overlong_lines != prior_overlong:
                    self._overlong_lines += framer.overlong_lines - prior_overlong
                    self._emit_session_update(
                        state,
                        identity,
                        firmware_version,
                        stop_event=stop_event,
                    )
                for line in lines:
                    try:
                        frame = parse_machine_line(line)
                    except MachineFrameError:
                        self._malformed_frames += 1
                        self._emit_session_update(
                            state,
                            identity,
                            firmware_version,
                            stop_event=stop_event,
                        )
                        continue
                    if frame is None:
                        continue
                    received_at = self._wall_clock()
                    frame_monotonic = self._monotonic_clock()
                    last_valid_frame = frame_monotonic
                    classified_family: str | None = None
                    if frame.kind == "status":
                        status = frame.value
                        if not isinstance(status, BadgeStatus):
                            return "wrong_device"
                        classification = self._classify_status(
                            status, firmware_version
                        )
                        if classification is None:
                            return "wrong_device"
                        classified_family, capabilities = classification
                        if not self._authorize_status(
                            generation, classified_family, capabilities
                        ):
                            return "wrong_device"
                        self._last_valid_status_at = received_at
                        self._last_valid_status_monotonic = frame_monotonic
                        last_valid_status = frame_monotonic
                        if state == "stale":
                            state = "live"
                            self._emit_session_update(
                                state,
                                identity,
                                firmware_version,
                                detail="status_recovered",
                                stop_event=stop_event,
                            )
                    if frame.kind in {"detection", "status"}:
                        if not self._emit_frame_if_running(
                            frame,
                            received_at,
                            stop_event,
                        ):
                            return "stopped"
                        if classified_family == "badge_lite" and live_started_at is None:
                            if not self._write_if_running(serial_port, _LIVE_START, stop_event):
                                return "stopped"
                            live_started_at = frame_monotonic
                            live_exchange_at = None
                            live_session_id = None
                            live_last_sequence = None
                    elif frame.kind == "live_ready":
                        ready = frame.value
                        if (
                            self._verified_family != "badge_lite"
                            or not isinstance(ready, LiteLiveReady)
                            or live_started_at is None
                        ):
                            continue
                        live_session_id = ready.session_id
                        live_last_sequence = None
                        live_exchange_at = frame_monotonic
                    elif frame.kind == "live_heartbeat":
                        heartbeat = frame.value
                        if (
                            self._verified_family != "badge_lite"
                            or not isinstance(heartbeat, LiteLiveHeartbeat)
                            or heartbeat.session_id != live_session_id
                            or live_last_sequence is not None
                            and heartbeat.sequence <= live_last_sequence
                        ):
                            continue
                        ack = self._live_session_wire(
                            "FOF_LIVE_ACK", heartbeat.session_id, heartbeat.sequence
                        )
                        if not self._write_if_running(serial_port, ack, stop_event):
                            return "stopped"
                        live_last_sequence = heartbeat.sequence
                        live_exchange_at = frame_monotonic
                    elif frame.kind in {
                        "control_ok", "control_error", "config", "config_ok", "config_error"
                    }:
                        if self._control_timed_out(generation, frame_monotonic):
                            return "control_timeout"
                        if not self._route_control_reply(frame, generation):
                            self._malformed_frames += 1
                            self._emit_session_update(
                                state,
                                identity,
                                firmware_version,
                                stop_event=stop_event,
                            )
            return "stopped"
        finally:
            if live_session_id is not None:
                try:
                    self._write(
                        serial_port,
                        self._live_session_wire("FOF_LIVE_STOP", live_session_id),
                    )
                except Exception:
                    pass

    def _current_path_present(self, device: str) -> bool:
        return any(port.device == device for port in self._read_port_identities())

    @staticmethod
    def _classify_status(
        status: BadgeStatus,
        firmware_version: str,
    ) -> tuple[str, frozenset[str]] | None:
        raw = status.raw
        raw_capabilities = raw.get("capabilities")
        if raw_capabilities is None:
            capabilities = frozenset()
        elif (
            isinstance(raw_capabilities, tuple)
            and all(isinstance(value, str) and value for value in raw_capabilities)
        ):
            capabilities = frozenset(raw_capabilities)
            if len(capabilities) != len(raw_capabilities):
                return None
        else:
            return None

        lite_hint = any(
            raw.get(key) == _LITE_IDENTITY[key]
            for key in ("product_family", "target", "project", "mode")
        ) or "display_none" in capabilities
        if lite_hint:
            if any(raw.get(key) != value for key, value in _LITE_IDENTITY.items()):
                return None
            if status.version != firmware_version:
                return None
            if not _LITE_REQUIRED_CAPABILITIES <= capabilities:
                return None
            return "badge_lite", capabilities

        identity_keys = ("product_family", "target", "project", "hardware")
        if any(key in raw for key in identity_keys):
            native_badge = (
                raw.get("product_family") in {None, "badge"}
                and raw.get("target") == "uplink-s3-fof_badge"
                and raw.get("project") == "fof_badge_uplink"
                and raw.get("hardware") in {None, "seeed_xiao_esp32s3"}
            )
            if not native_badge:
                return None
        return "regular_badge", capabilities

    def _authorize_status(
        self,
        generation: int,
        family: str,
        capabilities: frozenset[str],
    ) -> bool:
        with self._control_lock:
            if self._live_generation != generation:
                return False
            if self._verified_family is not None and self._verified_family != family:
                return False
            self._verified_family = family
            self._verified_capabilities = capabilities
            return True

    @staticmethod
    def _live_session_wire(
        prefix: str,
        session_id: str,
        sequence: int | None = None,
    ) -> bytes:
        payload: dict[str, object] = {"session_id": session_id}
        if sequence is not None:
            payload["sequence"] = sequence
        encoded = json.dumps(
            payload,
            separators=(",", ":"),
            ensure_ascii=True,
            allow_nan=False,
        ).encode("ascii")
        return prefix.encode("ascii") + b":" + encoded + b"\n"

    def _activate_generation(self) -> int:
        with self._control_lock:
            self._generation += 1
            self._live_generation = self._generation
            self._verified_family = None
            self._verified_capabilities = frozenset()
            return self._generation

    def _take_next_control(self, generation: int) -> _PendingRequest | None:
        with self._control_lock:
            if self._live_generation != generation:
                return None
            if self._outstanding_control is not None:
                return None
            while self._control_queue:
                request = self._control_queue.popleft()
                if request.generation == generation:
                    self._outstanding_control = request
                    return request
                request.error = TransportUnavailable()
                request.completion.set()
            return None

    def _mark_control_transmitted(
        self,
        request: _PendingRequest,
        generation: int,
    ) -> None:
        transmitted_at = self._monotonic_clock()
        with self._control_lock:
            if (
                self._live_generation == generation
                and self._outstanding_control is request
            ):
                request.deadline = transmitted_at + request.timeout

    def _route_control_reply(self, frame: MachineFrame, generation: int) -> bool:
        with self._control_lock:
            request = self._outstanding_control
            if request is None or request.generation != generation:
                return frame.kind not in {"control_ok", "config_ok", "config_error", "config"}
            if isinstance(request, _ControlRequest):
                if frame.kind not in {"control_ok", "control_error"}:
                    return False
                reply = frame.value
                if not isinstance(reply, ControlReply):
                    return False
                if frame.kind == "control_ok" and reply.message != request.command.expected_message:
                    return False
            else:
                if frame.kind not in request.expected_kinds:
                    return False
                reply = frame.value
            request.reply = reply
            self._outstanding_control = None
            request.completion.set()
            return True

    def _control_timed_out(self, generation: int, now: float) -> bool:
        with self._control_lock:
            request = self._outstanding_control
            if (
                request is None
                or request.generation != generation
                or request.deadline is None
                or now < request.deadline
            ):
                return False
            self._live_generation = None
            self._outstanding_control = None
            request.error = ControlTimeout()
            request.completion.set()
            self._fail_queued_locked(generation, TransportUnavailable())
            return True

    def _fail_generation(self, generation: int, error: BaseException) -> None:
        with self._control_lock:
            if self._live_generation == generation:
                self._live_generation = None
                self._verified_family = None
                self._verified_capabilities = frozenset()
            request = self._outstanding_control
            if request is not None and request.generation == generation:
                self._outstanding_control = None
                request.error = error
                request.completion.set()
            self._fail_queued_locked(generation, error)

    def _fail_queued_locked(self, generation: int, error: BaseException) -> None:
        retained: deque[_PendingRequest] = deque()
        while self._control_queue:
            request = self._control_queue.popleft()
            if request.generation == generation:
                request.error = error
                request.completion.set()
            else:
                retained.append(request)
        self._control_queue = retained

    def _write(self, serial_port: Any, data: bytes) -> None:
        with self._write_lock:
            serial_port.write(data)

    def _write_if_running(
        self,
        serial_port: Any,
        data: bytes,
        stop_event: threading.Event,
    ) -> bool:
        if not self._begin_side_effect(stop_event):
            return False
        try:
            self._write(serial_port, data)
            return True
        finally:
            self._end_side_effect(stop_event)

    def _emit_frame_if_running(
        self,
        frame: MachineFrame,
        received_at: float,
        stop_event: threading.Event,
    ) -> bool:
        if not self._begin_side_effect(stop_event):
            return False
        try:
            self._on_frame(frame, received_at)
            return True
        finally:
            self._end_side_effect(stop_event)

    def _emit_session_update(
        self,
        state: str,
        identity: PortIdentity,
        firmware_version: str,
        *,
        detail: str | None = None,
        reconnect_attempt: int = 0,
        stop_event: threading.Event | None = None,
    ) -> bool:
        if stop_event is not None and not self._begin_side_effect(stop_event):
            return False
        try:
            self._emit(
                ConnectionUpdate(
                    state=state,
                    detail=detail or ("verified" if state == "live" else state),
                    port=identity.device,
                    firmware_version=firmware_version,
                    last_valid_status_at=self._last_valid_status_at,
                    malformed_frames=self._malformed_frames,
                    overlong_lines=self._overlong_lines,
                    reconnect_attempt=reconnect_attempt,
                )
            )
            return True
        finally:
            if stop_event is not None:
                self._end_side_effect(stop_event)

    def _read_port_identities(self) -> tuple[PortIdentity, ...]:
        raw_ports = tuple(self._enumerate_ports())
        if all(isinstance(port, PortIdentity) for port in raw_ports):
            return raw_ports
        return discover_badge_ports(lambda: raw_ports)

    def _choose_for_start(self, ports: tuple[PortIdentity, ...]) -> PortIdentity:
        remembered = self._remembered_identity
        if remembered is not None:
            exact = tuple(
                port
                for port in ports
                if port.vid == ESPRESSIF_VID and port.pid == USB_SERIAL_JTAG_PID
            )
            if remembered.serial_number is not None:
                matches = tuple(
                    port
                    for port in exact
                    if port.serial_number == remembered.serial_number
                )
            elif remembered.location is not None:
                matches = tuple(
                    port for port in exact if port.location == remembered.location
                )
            else:
                path_candidates = ports if self._explicit_port is not None else exact
                for port in path_candidates:
                    if port.device == remembered.device:
                        return port
                candidate_paths = _candidate_paths(exact)
                if self._explicit_port is not None:
                    raise DiscoveryError(
                        "explicit_port_missing",
                        f"The requested serial port is not present: {remembered.device}",
                        candidate_paths,
                    )
                if len(candidate_paths) > 1:
                    raise DiscoveryError(
                        "multiple_badges",
                        "Multiple badge devices were found; the previously verified "
                        "path is absent.",
                        candidate_paths,
                    )
                raise DiscoveryError(
                    "no_badge",
                    "The previously verified badge path is not currently present.",
                    candidate_paths,
                )
            if not matches:
                raise DiscoveryError(
                    "no_badge",
                    "The previously verified badge is not currently present.",
                    _candidate_paths(exact),
                )
            return choose_candidate(matches)
        return choose_candidate(ports, explicit_path=self._explicit_port)

    def _open_safely(self, identity: PortIdentity) -> Any:
        serial_port = self._serial_factory()
        try:
            serial_port.port = identity.device
            serial_port.baudrate = 115200
            serial_port.timeout = 0.1
            serial_port.write_timeout = 3.0
            try:
                serial_port.exclusive = True
            except (AttributeError, NotImplementedError):
                pass
            serial_port.open()
            return serial_port
        except Exception:
            self._safe_close(serial_port)
            raise

    def _safe_close(self, serial_port: Any) -> None:
        try:
            serial_port.close()
        except Exception:
            pass

    def _await_pong(
        self,
        serial_port: Any,
        framer: LineFramer,
        stop_event: threading.Event,
        deadline: float,
    ) -> str | None:
        while not self._handshake_ended(deadline, stop_event):
            chunk = serial_port.read(4096)
            if self._handshake_ended(deadline, stop_event):
                return None
            if not chunk:
                continue
            for line in framer.feed(chunk):
                try:
                    frame = parse_machine_line(line)
                except MachineFrameError:
                    continue
                if frame is not None and frame.kind == "pong":
                    if self._handshake_ended(deadline, stop_event):
                        return None
                    return str(frame.value)
        return None

    def _handshake_ended(
        self,
        deadline: float,
        stop_event: threading.Event,
    ) -> bool:
        return stop_event.is_set() or self._monotonic_clock() >= deadline

    def _emit(self, update: ConnectionUpdate) -> None:
        self._on_connection(update)


def _default_enumerate_ports() -> Iterable[Any]:
    from serial.tools.list_ports import comports

    return comports()


def _default_serial_factory() -> Any:
    import serial

    return serial.Serial()
