"""Shared user-visible redaction and process-wide OS-FD capture."""

from __future__ import annotations

import os
import re
import sys
import tempfile
import threading
from collections.abc import Callable
from contextvars import ContextVar
from dataclasses import dataclass
from typing import Generic, TextIO, TypeVar


_USER_VISIBLE_MAC_RE = re.compile(
    r"""
    (?<!\w)
    (?:
        (?P<colon>(?:[0-9a-f]{2}:){5}[0-9a-f]{2})
      |
        (?P<hyphen>(?:[0-9a-f]{2}-){5}[0-9a-f]{2})
      |
        (?P<dotted>(?:[0-9a-f]{4}\.){2}[0-9a-f]{4})
      |
        (?P<compact>[0-9a-f]{12})
    )
    (?!\w)
    """,
    re.IGNORECASE | re.VERBOSE,
)
_FACTORY_PRIVATE_HEX_RE = re.compile(
    r"(?<![0-9A-Za-z_])[0-9A-Fa-f]{6,64}(?![0-9A-Za-z_])"
)
_PUBLIC_OUTPUT_LOCK = threading.RLock()
_LIVE_STDOUT_FD: ContextVar[int | None] = ContextVar(
    "factory_live_stdout_fd",
    default=None,
)
_CAPTURE_TEMP_PREFIX = "fof-private-output-"
_EXCEPTION_MESSAGE_UNAVAILABLE = "[exception message unavailable]"

T = TypeVar("T")


@dataclass(frozen=True)
class CapturedUserVisibleOutput(Generic[T]):
    """A private operation result plus scrubbed public transcript metadata."""

    result: T | None
    stdout: str
    stderr: str
    error_type: str | None
    error: str | None

    @property
    def succeeded(self) -> bool:
        return self.error_type is None


def scrub_user_visible_text(value: object) -> str:
    """Render and replace canonical, hyphen, dotted, and compact MAC forms."""
    rendered = str(value)

    def replace_mac(match: re.Match[str]) -> str:
        start, end = match.span()

        def is_word_char(char: str) -> bool:
            return char == "_" or char.isalnum()

        if start > 0 and is_word_char(rendered[start - 1]):
            return match.group(0)
        if end < len(rendered) and is_word_char(rendered[end]):
            return match.group(0)

        def has_left_hex_extension(separator: str) -> bool:
            if start == 0 or rendered[start - 1] != separator:
                return False
            group_end = start - 1
            group_start = group_end
            while group_start > 0 and is_word_char(
                rendered[group_start - 1]
            ):
                group_start -= 1
            group = rendered[group_start:group_end]
            return (
                bool(group) and
                all(char in "0123456789abcdefABCDEF" for char in group)
            ) or (
                match.lastgroup == "colon" and separator == ":" and
                not group and group_end > 0 and
                rendered[group_end - 1] == separator
            )

        def has_right_hex_extension(separator: str) -> bool:
            if end >= len(rendered) or rendered[end] != separator:
                return False
            group_start = end + 1
            group_end = group_start
            while group_end < len(rendered) and is_word_char(
                rendered[group_end]
            ):
                group_end += 1
            group = rendered[group_start:group_end]
            return (
                bool(group) and
                all(char in "0123456789abcdefABCDEF" for char in group)
            ) or (
                match.lastgroup == "colon" and separator == ":" and
                not group and group_start < len(rendered) and
                rendered[group_start] == separator
            )

        separator = {
            "colon": ":",
            "hyphen": "-",
            "dotted": ".",
        }.get(match.lastgroup or "")
        separators = (separator,) if separator else (":", "-", ".")
        if any(
            has_left_hex_extension(candidate) or
            has_right_hex_extension(candidate)
            for candidate in separators
        ):
            return match.group(0)

        return "[hardware-id]"

    return _USER_VISIBLE_MAC_RE.sub(replace_mac, rendered)


def scrub_factory_transcript(value: object) -> str:
    """Replace all public factory hardware/bundle IDs with a fixed alias."""
    scrubbed = scrub_user_visible_text(value).replace(
        "[hardware-id]", "BADGE"
    )
    return _FACTORY_PRIVATE_HEX_RE.sub("BADGE", scrubbed)


def print_user_visible(
    value: object = "",
    *,
    file: TextIO | None = None,
    end: str = "\n",
    flush: bool = False,
) -> None:
    """Scrub once at the final print boundary under the capture lock."""
    with _PUBLIC_OUTPUT_LOCK:
        print(
            scrub_user_visible_text(value),
            file=file,
            end=end,
            flush=flush,
        )


def print_live_user_visible(
    value: object = "",
    *,
    end: str = "\n",
    flush: bool = False,
) -> None:
    """Print a scrubbed status line outside the active private capture."""
    rendered = scrub_user_visible_text(value)
    with _PUBLIC_OUTPUT_LOCK:
        live_fd = _LIVE_STDOUT_FD.get()
        if live_fd is None:
            print(rendered, end=end, flush=flush)
            return
        pending = memoryview((rendered + end).encode("utf-8", "replace"))
        try:
            while pending:
                pending = pending[os.write(live_fd, pending):]
        except OSError:
            # Status output must never turn a valid hardware operation into
            # a failure. Fall back to the private transcript for later print.
            print(rendered, end=end, flush=flush)


def _flush_standard_streams() -> None:
    sys.stdout.flush()
    sys.stderr.flush()


def _read_descriptor(descriptor: int) -> str:
    os.lseek(descriptor, 0, os.SEEK_SET)
    chunks: list[bytes] = []
    while True:
        chunk = os.read(descriptor, 64 * 1024)
        if not chunk:
            break
        chunks.append(chunk)
    return b"".join(chunks).decode("utf-8", "replace")


def _inert_string(value: str) -> str:
    """Copy even a str subclass into an exact built-in str without callbacks."""
    encoded = str.encode(value, "utf-8", "surrogatepass")
    return bytes.decode(encoded, "utf-8", "surrogatepass")


def capture_user_visible_output(
    operation: Callable[[], T],
    *,
    scrubber: Callable[[object], str] | None = None,
) -> CapturedUserVisibleOutput[T]:
    """Run one operation with exclusive OS-level stdout/stderr capture."""
    with _PUBLIC_OUTPUT_LOCK:
        stdout_fd = -1
        stderr_fd = -1
        saved_stdout_fd = -1
        saved_stderr_fd = -1
        stdout_path = ""
        stderr_path = ""
        stdout_redirected = False
        stderr_redirected = False
        result: T | None = None
        raw_error_type: str | None = None
        raw_error: str | None = None
        transcript_stdout = ""
        transcript_stderr = ""
        try:
            stdout_fd, stdout_path = tempfile.mkstemp(
                prefix=_CAPTURE_TEMP_PREFIX + "stdout-",
            )
            stderr_fd, stderr_path = tempfile.mkstemp(
                prefix=_CAPTURE_TEMP_PREFIX + "stderr-",
            )
            os.fchmod(stdout_fd, 0o600)
            os.fchmod(stderr_fd, 0o600)
            _flush_standard_streams()
            saved_stdout_fd = os.dup(1)
            saved_stderr_fd = os.dup(2)
            os.dup2(stdout_fd, 1)
            stdout_redirected = True
            os.dup2(stderr_fd, 2)
            stderr_redirected = True
            live_token = _LIVE_STDOUT_FD.set(saved_stdout_fd)
            try:
                result = operation()
            except BaseException as exc:
                raw_error_type = _inert_string(type(exc).__name__)
                try:
                    rendered_error = _inert_string(str(exc))
                except BaseException:
                    raw_error = _EXCEPTION_MESSAGE_UNAVAILABLE
                else:
                    raw_error = rendered_error or raw_error_type
            finally:
                _LIVE_STDOUT_FD.reset(live_token)
                try:
                    _flush_standard_streams()
                finally:
                    if stdout_redirected:
                        os.dup2(saved_stdout_fd, 1)
                        stdout_redirected = False
                    if stderr_redirected:
                        os.dup2(saved_stderr_fd, 2)
                        stderr_redirected = False
            transcript_stdout = _read_descriptor(stdout_fd)
            transcript_stderr = _read_descriptor(stderr_fd)
        finally:
            if stdout_redirected and saved_stdout_fd >= 0:
                os.dup2(saved_stdout_fd, 1)
            if stderr_redirected and saved_stderr_fd >= 0:
                os.dup2(saved_stderr_fd, 2)
            for descriptor in (
                saved_stdout_fd,
                saved_stderr_fd,
                stdout_fd,
                stderr_fd,
            ):
                if descriptor >= 0:
                    try:
                        os.close(descriptor)
                    except OSError:
                        pass
            for path in (stdout_path, stderr_path):
                if path:
                    try:
                        os.unlink(path)
                    except FileNotFoundError:
                        pass

        render = scrubber or scrub_user_visible_text
        return CapturedUserVisibleOutput(
            result=result,
            stdout=render(transcript_stdout),
            stderr=render(transcript_stderr),
            error_type=(
                render(raw_error_type)
                if raw_error_type is not None
                else None
            ),
            error=(
                render(raw_error)
                if raw_error is not None
                else None
            ),
        )
