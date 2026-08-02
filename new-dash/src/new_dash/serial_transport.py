"""macOS badge discovery and one verified live serial-session owner."""

from __future__ import annotations

from collections import deque
from collections.abc import Callable, Iterable
from dataclasses import dataclass, field
import threading
import time
from typing import Any

from .controls import BadgeControlCommand
from .models import ControlReply, MachineFrame
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
    deadline: float
    completion: threading.Event = field(default_factory=threading.Event)
    reply: ControlReply | None = None
    error: BaseException | None = None


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
        self._on_frame = on_frame or (lambda _frame, _received_at: None)
        self._on_connection = on_connection or (lambda _update: None)
        self._write_lock = threading.Lock()
        self._lifecycle_lock = threading.Lock()
        self._control_lock = threading.Lock()
        self._worker: threading.Thread | None = None
        self._worker_stop: threading.Event | None = None
        self._stopping = False
        self._remembered_identity: PortIdentity | None = None
        self._malformed_frames = 0
        self._overlong_lines = 0
        self._last_valid_status_at: float | None = None
        self._generation = 0
        self._live_generation: int | None = None
        self._control_queue: deque[_ControlRequest] = deque()
        self._outstanding_control: _ControlRequest | None = None

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
            stop_event.set()
            if worker is threading.current_thread():
                self._stopping = False
                return
        worker.join(timeout)
        if worker.is_alive():
            with self._lifecycle_lock:
                self._stopping = False
            raise TransportError(
                "stop_timeout", "The serial worker did not stop in time."
            )
        with self._lifecycle_lock:
            if self._worker is worker:
                self._worker = None
            if self._worker_stop is stop_event:
                self._worker_stop = None
            self._stopping = False

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
            request = _ControlRequest(
                command=command,
                generation=generation,
                deadline=self._monotonic_clock() + timeout,
            )
            self._control_queue.append(request)
        request.completion.wait()
        if request.error is not None:
            raise request.error
        if request.reply is None:
            raise TransportUnavailable("The badge control ended without a reply.")
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
            delay = min(2.0 ** (reconnect_attempt - 1), 10.0)
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
            serial_port.setDTR(False)
            serial_port.setRTS(False)
            serial_port.reset_input_buffer()
            framer = LineFramer()
            self._write(serial_port, b"\nFOF_PING\n")

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
            self._emit_session_update("live", identity, firmware_version)
            detail = self._run_live_session(
                serial_port,
                framer,
                stop_event,
                identity,
                firmware_version,
                generation,
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
                serial_port.close()

    def _run_live_session(
        self,
        serial_port: Any,
        framer: LineFramer,
        stop_event: threading.Event,
        identity: PortIdentity,
        firmware_version: str,
        generation: int,
    ) -> str:
        session_started = self._monotonic_clock()
        next_poll = session_started
        next_presence_check = session_started + 1.0
        last_valid_frame = session_started
        last_valid_status = session_started
        state = "live"
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
                )
            if now >= next_presence_check:
                if not self._current_path_present(identity.device):
                    return "path_missing"
                next_presence_check = now + 1.0
            if now >= next_poll:
                self._write(serial_port, b"FOF_STATUS\n")
                next_poll += 2.0
                while next_poll <= now:
                    next_poll += 2.0

            request = self._take_next_control(generation)
            if request is not None:
                if stop_event.is_set():
                    return "stopped"
                self._write(serial_port, request.command.to_wire())

            prior_overlong = framer.overlong_lines
            chunk = serial_port.read(4096)
            if stop_event.is_set():
                return "stopped"
            if not chunk:
                continue
            lines = framer.feed(chunk)
            if framer.overlong_lines != prior_overlong:
                self._overlong_lines += framer.overlong_lines - prior_overlong
                self._emit_session_update(state, identity, firmware_version)
            for line in lines:
                try:
                    frame = parse_machine_line(line)
                except MachineFrameError:
                    self._malformed_frames += 1
                    self._emit_session_update(state, identity, firmware_version)
                    continue
                if frame is None:
                    continue
                received_at = self._wall_clock()
                frame_monotonic = self._monotonic_clock()
                last_valid_frame = frame_monotonic
                if frame.kind == "status":
                    self._last_valid_status_at = received_at
                    last_valid_status = frame_monotonic
                    if state == "stale":
                        state = "live"
                        self._emit_session_update(
                            state,
                            identity,
                            firmware_version,
                            detail="status_recovered",
                        )
                if frame.kind in {"detection", "status"}:
                    self._on_frame(frame, received_at)
                elif frame.kind in {"control_ok", "control_error"}:
                    if not self._route_control_reply(frame, generation):
                        self._malformed_frames += 1
                        self._emit_session_update(state, identity, firmware_version)
        return "stopped"

    def _current_path_present(self, device: str) -> bool:
        return any(port.device == device for port in self._read_port_identities())

    def _activate_generation(self) -> int:
        with self._control_lock:
            self._generation += 1
            self._live_generation = self._generation
            return self._generation

    def _take_next_control(self, generation: int) -> _ControlRequest | None:
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

    def _route_control_reply(self, frame: MachineFrame, generation: int) -> bool:
        with self._control_lock:
            request = self._outstanding_control
            if request is None or request.generation != generation:
                return frame.kind != "control_ok"
            reply = frame.value
            if not isinstance(reply, ControlReply):
                return False
            if frame.kind == "control_ok" and reply.message != request.command.expected_message:
                return False
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
            request = self._outstanding_control
            if request is not None and request.generation == generation:
                self._outstanding_control = None
                request.error = error
                request.completion.set()
            self._fail_queued_locked(generation, error)

    def _fail_queued_locked(self, generation: int, error: BaseException) -> None:
        retained: deque[_ControlRequest] = deque()
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

    def _emit_session_update(
        self,
        state: str,
        identity: PortIdentity,
        firmware_version: str,
        *,
        detail: str | None = None,
        reconnect_attempt: int = 0,
    ) -> None:
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
            serial_port.dtr = False
            serial_port.rts = False
            serial_port.open()
            return serial_port
        except Exception:
            serial_port.close()
            raise

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
