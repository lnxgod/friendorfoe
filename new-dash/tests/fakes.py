from __future__ import annotations

from collections import deque
from threading import Event, Lock
from typing import Iterable


class StepClock:
    """A monotonic clock that advances without sleeping."""

    def __init__(self, *, start: float = 0.0, step: float = 0.5) -> None:
        self.value = start
        self.step = step
        self._lock = Lock()

    def __call__(self) -> float:
        with self._lock:
            value = self.value
            self.value += self.step
            return value


class FakeSerial:
    """A deterministic unopened PySerial-compatible serial object."""

    def __init__(
        self,
        incoming: Iterable[bytes] = (),
        *,
        stale_input: bytes = b"",
        read_exception: Exception | None = None,
        open_exception: Exception | None = None,
        exclusive_supported: bool = True,
    ) -> None:
        self.actions: list[tuple[object, ...]] = []
        self.writes: list[bytes] = []
        self._incoming = deque(incoming)
        self._incoming_lock = Lock()
        self._stale_input = stale_input
        self.read_exception = read_exception
        self.open_exception = open_exception
        self.exclusive_supported = exclusive_supported
        self.is_open = False
        self.opened = Event()
        self.closed = Event()
        self.written = Event()
        self._port: str | None = None
        self._baudrate: int | None = None
        self._timeout: float | None = None
        self._write_timeout: float | None = None
        self._exclusive: bool | None = None
        self._dtr: bool | None = None
        self._rts: bool | None = None

    @property
    def port(self) -> str | None:
        return self._port

    @port.setter
    def port(self, value: str) -> None:
        self._port = value
        self.actions.append(("set", "port", value))

    @property
    def baudrate(self) -> int | None:
        return self._baudrate

    @baudrate.setter
    def baudrate(self, value: int) -> None:
        self._baudrate = value
        self.actions.append(("set", "baudrate", value))

    @property
    def timeout(self) -> float | None:
        return self._timeout

    @timeout.setter
    def timeout(self, value: float) -> None:
        self._timeout = value
        self.actions.append(("set", "timeout", value))

    @property
    def write_timeout(self) -> float | None:
        return self._write_timeout

    @write_timeout.setter
    def write_timeout(self, value: float) -> None:
        self._write_timeout = value
        self.actions.append(("set", "write_timeout", value))

    @property
    def exclusive(self) -> bool | None:
        return self._exclusive

    @exclusive.setter
    def exclusive(self, value: bool) -> None:
        self.actions.append(("set", "exclusive", value))
        if not self.exclusive_supported:
            raise AttributeError("exclusive is unsupported")
        self._exclusive = value

    @property
    def dtr(self) -> bool | None:
        return self._dtr

    @dtr.setter
    def dtr(self, value: bool) -> None:
        self._dtr = value
        self.actions.append(("set", "dtr", value))

    @property
    def rts(self) -> bool | None:
        return self._rts

    @rts.setter
    def rts(self, value: bool) -> None:
        self._rts = value
        self.actions.append(("set", "rts", value))

    def open(self) -> None:
        if self.is_open:
            raise RuntimeError("already open")
        if self.dtr is not False or self.rts is not False:
            raise AssertionError("DTR and RTS must be false before open")
        self.actions.append(("open",))
        if self.open_exception is not None:
            raise self.open_exception
        self.is_open = True
        self.opened.set()

    def setDTR(self, value: bool) -> None:
        self._dtr = value
        self.actions.append(("setDTR", value))

    def setRTS(self, value: bool) -> None:
        self._rts = value
        self.actions.append(("setRTS", value))

    def reset_input_buffer(self) -> None:
        self._stale_input = b""
        self.actions.append(("reset_input_buffer",))

    def write(self, data: bytes) -> int:
        if not self.is_open:
            raise RuntimeError("port is closed")
        copied = bytes(data)
        self.writes.append(copied)
        self.actions.append(("write", copied))
        self.written.set()
        return len(copied)

    def read(self, size: int = 1) -> bytes:
        if not self.is_open:
            raise RuntimeError("port is closed")
        if self._stale_input:
            data = self._stale_input[:size]
            self._stale_input = self._stale_input[size:]
            return data
        with self._incoming_lock:
            if self._incoming:
                return self._incoming.popleft()
        if self.read_exception is not None:
            raise self.read_exception
        return b""

    def feed(self, *chunks: bytes) -> None:
        with self._incoming_lock:
            self._incoming.extend(chunks)

    def close(self) -> None:
        self.actions.append(("close",))
        self.is_open = False
        self.closed.set()
