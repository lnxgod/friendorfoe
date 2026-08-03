"""Framing and parsing for newline-delimited badge USB records."""

from __future__ import annotations

import json
from math import isfinite

from .models import BadgeStatus, ControlReply, DetectionEvent, MachineFrame


class MachineFrameError(ValueError):
    """A recognized machine record was malformed."""


def _reject_json_constant(value: str) -> object:
    raise ValueError(f"non-finite JSON value: {value}")


def _parse_finite_json_float(value: str) -> float:
    parsed = float(value)
    if not isfinite(parsed):
        raise ValueError(f"non-finite JSON number: {value}")
    return parsed


class LineFramer:
    """Incrementally split a byte stream into UTF-8 console lines."""

    def __init__(self, max_line_bytes: int = 65536) -> None:
        if max_line_bytes < 1:
            raise ValueError("max_line_bytes must be positive")
        self.max_line_bytes = max_line_bytes
        self.overlong_lines = 0
        self.decode_errors = 0
        self._buffer = bytearray()
        self._dropping = False
        self._previous_was_cr = False

    def feed(self, data: bytes) -> list[str]:
        lines: list[str] = []
        for byte in data:
            if byte == 10 and self._previous_was_cr:
                self._previous_was_cr = False
                continue

            if byte in (10, 13):
                self._finish_line(lines)
                self._previous_was_cr = byte == 13
                continue

            self._previous_was_cr = False
            if self._dropping:
                continue
            if len(self._buffer) == self.max_line_bytes:
                self._buffer.clear()
                self._dropping = True
                self.overlong_lines += 1
                continue
            self._buffer.append(byte)
        return lines

    def _finish_line(self, lines: list[str]) -> None:
        if self._dropping:
            self._dropping = False
            self._buffer.clear()
            return
        if not self._buffer:
            return
        raw = bytes(self._buffer)
        self._buffer.clear()
        try:
            lines.append(raw.decode("utf-8"))
        except UnicodeDecodeError:
            self.decode_errors += 1
            lines.append(raw.decode("utf-8", errors="replace"))


_JSON_PREFIXES = {
    "FOF_DET:": ("detection", DetectionEvent.from_payload),
    "FOF_STATUS:": ("status", BadgeStatus.from_payload),
    "FOF_CTL_OK:": ("control_ok", lambda payload: ControlReply.from_payload(payload, ok=True)),
    "FOF_CTL_ERROR:": ("control_error", lambda payload: ControlReply.from_payload(payload, ok=False)),
}


def parse_machine_line(line: str) -> MachineFrame | None:
    """Parse a recognized machine record and ignore normal firmware logs."""

    if line.startswith("FOF_PONG:"):
        version = line.removeprefix("FOF_PONG:")
        if not version:
            raise MachineFrameError("FOF_PONG requires a version")
        return MachineFrame(kind="pong", value=version)

    for prefix, (kind, parser) in _JSON_PREFIXES.items():
        if not line.startswith(prefix):
            continue
        try:
            payload = json.loads(
                line.removeprefix(prefix),
                parse_constant=_reject_json_constant,
                parse_float=_parse_finite_json_float,
            )
            return MachineFrame(kind=kind, value=parser(payload))
        except (TypeError, ValueError, OverflowError, RecursionError) as error:
            raise MachineFrameError(f"invalid {kind} frame") from error
    return None
