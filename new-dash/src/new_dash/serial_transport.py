"""macOS badge discovery and one verified serial-session boundary."""

from __future__ import annotations

from collections.abc import Callable, Iterable
from dataclasses import dataclass
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
    """Own one safely opened serial handle through application verification."""

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
        self._worker: threading.Thread | None = None
        self._worker_stop: threading.Event | None = None
        self._stopping = False
        self._remembered_identity: PortIdentity | None = None

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
        """Expose the control contract before live reply routing is added."""

        del command, timeout
        raise TransportError(
            "control_unavailable",
            "Control reply routing is not active for this verified session.",
        )

    def _run_one_session(self, stop_event: threading.Event) -> None:
        serial_port: Any | None = None
        identity: PortIdentity | None = None
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
                return

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
            with self._write_lock:
                serial_port.write(b"\nFOF_PING\n")

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
                return

            live_update = ConnectionUpdate(
                state="live",
                detail="verified",
                port=identity.device,
                firmware_version=firmware_version,
            )
            if self._handshake_ended(handshake_deadline, stop_event):
                if not stop_event.is_set():
                    self._emit(
                        ConnectionUpdate(
                            state="error",
                            detail="wrong_device",
                            port=identity.device,
                        )
                    )
                return
            self._emit(live_update)
            self._remembered_identity = identity
            stop_event.wait()
        except OSError:
            if not stop_event.is_set():
                self._emit(
                    ConnectionUpdate(
                        state="error",
                        detail="read_error" if serial_port is not None else "open_error",
                        port=identity.device if identity is not None else None,
                    )
                )
        except Exception:
            if not stop_event.is_set():
                self._emit(
                    ConnectionUpdate(
                        state="error",
                        detail="serial_error",
                        port=identity.device if identity is not None else None,
                    )
                )
        finally:
            if serial_port is not None:
                serial_port.close()

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
