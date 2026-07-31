#!/usr/bin/env python3
"""Descriptor-rooted, owner-private artifact snapshots and immutable freezes."""

from __future__ import annotations

import ctypes
import errno
import hashlib
import io
import json
import os
import re
import secrets
import stat
import subprocess
import sys
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


DEFAULT_ARTIFACT_MEMBER_BYTES = 8 * 1024 * 1024
MAX_ARTIFACT_MEMBER_BYTES = 16 * 1024 * 1024
MAX_ARTIFACT_SET_BYTES = 32 * 1024 * 1024

_COPY_CHUNK_BYTES = 64 * 1024
_LOGICAL_NAME_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}")
_HEX_SHA256_RE = re.compile(r"[0-9a-f]{64}")
_AGGREGATE_DOMAIN = b"FOF-FROZEN-ARTIFACT-SET-v1\x00"
_SNAPSHOT_RECEIPT_SCHEMA = 1
_SNAPSHOT_TOKEN = object()
_GENERATED_MEMBER_TOKEN = object()
_FROZEN_AUTHORITY_TOKEN = object()
# macOS rejects open("/dev/fd/N", "wb") because that reopen requests
# O_TRUNC.  The child maps only the exact output argument back to a duplicate
# of the already-retained descriptor, so the generator keeps its normal
# pathname API without consulting the replaceable workspace directory entry.
_PRIVATE_GENERATOR_RUNNER = r"""
import builtins
import io
import os
import sys

output_fd = int(sys.argv[1], 10)
output_path = sys.argv[2]
generator_fd = int(sys.argv[3], 10)
generator_path = sys.argv[4]
generator_argv = sys.argv[5:]
real_io_open = io.open


def retained_output_open(
    file,
    mode="r",
    buffering=-1,
    encoding=None,
    errors=None,
    newline=None,
    closefd=True,
    opener=None,
):
    rendered = os.fspath(file) if not isinstance(file, int) else file
    if rendered == output_path:
        if mode not in ("w", "wb") or not closefd or opener is not None:
            raise PermissionError("invalid retained output open mode")
        duplicate = os.dup(output_fd)
        try:
            os.ftruncate(duplicate, 0)
            os.lseek(duplicate, 0, os.SEEK_SET)
            return real_io_open(
                duplicate,
                mode,
                buffering,
                encoding,
                errors,
                newline,
                closefd=True,
            )
        except BaseException:
            try:
                os.close(duplicate)
            except OSError:
                pass
            raise
    return real_io_open(
        file,
        mode,
        buffering,
        encoding,
        errors,
        newline,
        closefd,
        opener,
    )


builtins.open = retained_output_open
io.open = retained_output_open
sys.argv = [generator_path, *generator_argv]
os.lseek(generator_fd, 0, os.SEEK_SET)
generator_chunks = []
while True:
    generator_chunk = os.read(generator_fd, 64 * 1024)
    if not generator_chunk:
        break
    generator_chunks.append(generator_chunk)
generator_code = compile(
    b"".join(generator_chunks),
    generator_path,
    "exec",
)
generator_globals = {
    "__name__": "__main__",
    "__file__": generator_path,
    "__package__": None,
    "__cached__": None,
    "__spec__": None,
    "__builtins__": builtins,
}
exec(generator_code, generator_globals, generator_globals)
"""


class SecureArtifactError(RuntimeError):
    """Artifact input or retained snapshot state failed closed."""


def _test_hook(_stage: str, _logical_name: str | None = None) -> None:
    """Private no-op seam used only by race-schedule tests."""


def _require_exact_str(value: object, label: str) -> str:
    if type(value) is not str or not value or "\x00" in value:
        raise SecureArtifactError(f"{label} must be one nonempty string")
    return value


def _validate_logical_name(value: object) -> str:
    logical_name = _require_exact_str(value, "artifact logical name")
    if _LOGICAL_NAME_RE.fullmatch(logical_name) is None:
        raise SecureArtifactError("artifact logical name is malformed")
    return logical_name


def _relative_components(value: object) -> tuple[str, ...]:
    relative = _require_exact_str(value, "artifact relative path")
    if (
        relative.startswith("/")
        or relative.endswith("/")
        or "\\" in relative
    ):
        raise SecureArtifactError("artifact relative path is unsafe")
    components = tuple(relative.split("/"))
    if any(component in ("", ".", "..") for component in components):
        raise SecureArtifactError("artifact relative path is unsafe")
    return components


def _lexical_absolute(path: os.PathLike[str] | str, label: str) -> Path:
    try:
        rendered = os.fspath(path)
    except TypeError as exc:
        raise SecureArtifactError(f"{label} path is malformed") from exc
    if type(rendered) is not str or not rendered or "\x00" in rendered:
        raise SecureArtifactError(f"{label} path is malformed")
    absolute = os.path.abspath(rendered)
    if not os.path.isabs(absolute) or os.path.normpath(absolute) != absolute:
        raise SecureArtifactError(f"{label} path is not lexical absolute")
    return Path(absolute)


def _directory_open_flags() -> int:
    nofollow = getattr(os, "O_NOFOLLOW", None)
    directory = getattr(os, "O_DIRECTORY", None)
    if nofollow is None or directory is None:
        raise SecureArtifactError(
            "descriptor-rooted traversal requires O_NOFOLLOW and O_DIRECTORY"
        )
    return (
        os.O_RDONLY
        | nofollow
        | directory
        | getattr(os, "O_CLOEXEC", 0)
        | getattr(os, "O_NOCTTY", 0)
    )


def _file_open_flags(*, write: bool = False, create: bool = False) -> int:
    nofollow = getattr(os, "O_NOFOLLOW", None)
    if nofollow is None:
        raise SecureArtifactError("secure artifact files require O_NOFOLLOW")
    flags = os.O_WRONLY if write else os.O_RDONLY
    flags |= nofollow | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOCTTY", 0)
    if not write:
        flags |= getattr(os, "O_NONBLOCK", 0)
    if create:
        flags |= os.O_CREAT | os.O_EXCL
    return flags


def _duplicate_cloexec(fd: int) -> int:
    duplicate = os.dup(fd)
    try:
        os.set_inheritable(duplicate, False)
    except BaseException:
        os.close(duplicate)
        raise
    return duplicate


def _stat_tuple(info: os.stat_result) -> tuple[int, ...]:
    return (
        info.st_dev,
        info.st_ino,
        info.st_mode,
        info.st_nlink,
        info.st_size,
        info.st_ctime_ns,
        info.st_mtime_ns,
    )


def _same_object(
    left: os.stat_result,
    right: os.stat_result,
) -> bool:
    return left.st_dev == right.st_dev and left.st_ino == right.st_ino


@dataclass(frozen=True, slots=True)
class _DirectoryBinding:
    name: str | None
    fd: int
    device: int
    inode: int
    mode: int
    uid: int
    gid: int


def _directory_binding(
    name: str | None,
    fd: int,
    info: os.stat_result,
) -> _DirectoryBinding:
    if not stat.S_ISDIR(info.st_mode):
        raise SecureArtifactError("descriptor path component is not a directory")
    return _DirectoryBinding(
        name=name,
        fd=fd,
        device=info.st_dev,
        inode=info.st_ino,
        mode=info.st_mode,
        uid=info.st_uid,
        gid=info.st_gid,
    )


def _directory_matches(
    info: os.stat_result,
    binding: _DirectoryBinding,
) -> bool:
    return (
        stat.S_ISDIR(info.st_mode)
        and info.st_dev == binding.device
        and info.st_ino == binding.inode
        and info.st_mode == binding.mode
        and info.st_uid == binding.uid
        and info.st_gid == binding.gid
    )


def _close_directory_chain(
    bindings: Iterable[_DirectoryBinding],
) -> None:
    for binding in reversed(tuple(bindings)):
        try:
            os.close(binding.fd)
        except OSError:
            pass


def _validate_directory_chain(
    bindings: tuple[_DirectoryBinding, ...],
) -> None:
    if not bindings:
        raise SecureArtifactError("descriptor directory chain is empty")
    for index, binding in enumerate(bindings):
        try:
            opened = os.fstat(binding.fd)
        except OSError as exc:
            raise SecureArtifactError(
                "descriptor directory capability is unavailable"
            ) from exc
        if not _directory_matches(opened, binding):
            raise SecureArtifactError(
                "descriptor directory capability identity changed"
            )
        if index == 0:
            continue
        parent = bindings[index - 1]
        assert binding.name is not None
        try:
            named = os.stat(
                binding.name,
                dir_fd=parent.fd,
                follow_symlinks=False,
            )
        except OSError as exc:
            raise SecureArtifactError(
                "descriptor directory pathname binding changed"
            ) from exc
        if not _directory_matches(named, binding):
            raise SecureArtifactError(
                "descriptor directory pathname binding changed"
            )


def _open_directory_chain(
    path: Path,
) -> tuple[_DirectoryBinding, ...]:
    flags = _directory_open_flags()
    bindings: list[_DirectoryBinding] = []
    try:
        root_fd = os.open(path.anchor, flags)
        bindings.append(
            _directory_binding(None, root_fd, os.fstat(root_fd))
        )
        for component in path.parts[1:]:
            parent_fd = bindings[-1].fd
            try:
                before = os.stat(
                    component,
                    dir_fd=parent_fd,
                    follow_symlinks=False,
                )
            except OSError as exc:
                raise SecureArtifactError(
                    "descriptor directory component is unavailable"
                ) from exc
            if stat.S_ISLNK(before.st_mode) or not stat.S_ISDIR(
                before.st_mode
            ):
                raise SecureArtifactError(
                    "descriptor directory component is not a real directory"
                )
            try:
                child_fd = os.open(component, flags, dir_fd=parent_fd)
            except OSError as exc:
                raise SecureArtifactError(
                    "descriptor directory component cannot be opened safely"
                ) from exc
            opened = os.fstat(child_fd)
            if not _same_object(before, opened) or not stat.S_ISDIR(
                opened.st_mode
            ):
                os.close(child_fd)
                raise SecureArtifactError(
                    "descriptor directory component changed during open"
                )
            bindings.append(
                _directory_binding(component, child_fd, opened)
            )
        result = tuple(bindings)
        _validate_directory_chain(result)
        return result
    except BaseException:
        _close_directory_chain(bindings)
        raise


def _retrace_final_directory(
    path: Path,
    *,
    device: int,
    inode: int,
) -> None:
    bindings = _open_directory_chain(path)
    try:
        final = os.fstat(bindings[-1].fd)
        if final.st_dev != device or final.st_ino != inode:
            raise SecureArtifactError(
                "descriptor root pathname identity changed"
            )
    finally:
        _close_directory_chain(bindings)


@dataclass(frozen=True, slots=True)
class SnapshotFileSpec:
    """One exact source pathname and its explicit eligibility policy."""

    logical_name: str
    relative: str
    allowed_modes: tuple[int, ...] = (0o600, 0o640, 0o644)
    max_size: int = DEFAULT_ARTIFACT_MEMBER_BYTES

    def __post_init__(self) -> None:
        _validate_logical_name(self.logical_name)
        _relative_components(self.relative)
        if (
            type(self.allowed_modes) is not tuple
            or not self.allowed_modes
            or any(
                type(mode) is not int
                or isinstance(mode, bool)
                or not 0 <= mode <= 0o777
                for mode in self.allowed_modes
            )
            or len(set(self.allowed_modes)) != len(self.allowed_modes)
        ):
            raise SecureArtifactError(
                "artifact allowed modes must be unique permission bits"
            )
        if (
            type(self.max_size) is not int
            or isinstance(self.max_size, bool)
            or not 0 <= self.max_size <= MAX_ARTIFACT_MEMBER_BYTES
        ):
            raise SecureArtifactError("artifact size bound is invalid")


@dataclass(frozen=True, slots=True)
class RegularFileIdentity:
    relative: str
    device: int
    inode: int
    mode: int
    nlink: int
    size: int
    ctime_ns: int
    mtime_ns: int
    sha256: str


@dataclass(frozen=True, slots=True)
class VerifiedSnapshotFile:
    logical_name: str
    private_relative: str
    source: RegularFileIdentity
    private: RegularFileIdentity


@dataclass(frozen=True, slots=True)
class FrozenArtifactMember:
    logical_name: str
    size: int
    sha256: str
    content: bytes

    def __post_init__(self) -> None:
        _validate_logical_name(self.logical_name)
        if (
            type(self.content) is not bytes
            or type(self.size) is not int
            or isinstance(self.size, bool)
            or self.size != len(self.content)
            or self.size > MAX_ARTIFACT_MEMBER_BYTES
            or type(self.sha256) is not str
            or _HEX_SHA256_RE.fullmatch(self.sha256) is None
            or hashlib.sha256(self.content).hexdigest() != self.sha256
        ):
            raise SecureArtifactError("frozen artifact member is inconsistent")


@dataclass(frozen=True, slots=True, init=False)
class GeneratedArtifactMember:
    """Verifier-issued immutable bytes produced by a private tool execution."""

    logical_name: str
    size: int
    sha256: str
    content: bytes

    def __init__(
        self,
        *,
        token: object,
        logical_name: str,
        content: bytes,
    ) -> None:
        if token is not _GENERATED_MEMBER_TOKEN:
            raise TypeError("generated artifact members are verifier-issued")
        validated_name = _validate_logical_name(logical_name)
        if type(content) is not bytes:
            raise SecureArtifactError(
                "generated artifact content must be exact bytes"
            )
        if len(content) > MAX_ARTIFACT_MEMBER_BYTES:
            raise SecureArtifactError(
                "generated artifact exceeds member size bound"
            )
        object.__setattr__(self, "logical_name", validated_name)
        object.__setattr__(self, "content", content)
        object.__setattr__(self, "size", len(content))
        object.__setattr__(
            self,
            "sha256",
            hashlib.sha256(content).hexdigest(),
        )


def _aggregate_sha256(
    receipt_sha256: str,
    members: tuple[FrozenArtifactMember, ...],
) -> str:
    digest = hashlib.sha256()
    digest.update(_AGGREGATE_DOMAIN)
    digest.update(bytes.fromhex(receipt_sha256))
    digest.update(len(members).to_bytes(4, "big"))
    for member in members:
        logical = member.logical_name.encode("utf-8")
        digest.update(len(logical).to_bytes(4, "big"))
        digest.update(logical)
        digest.update(member.size.to_bytes(8, "big"))
        digest.update(bytes.fromhex(member.sha256))
        digest.update(member.content)
    return digest.hexdigest()


@dataclass(frozen=True, slots=True, init=False)
class DescriptorRootedFreezeAuthority:
    """Verifier-issued proof that frozen bytes came from one retained receipt."""

    receipt_bytes: bytes
    source_bindings: tuple[tuple[str, str, int, str], ...]

    def __init__(
        self,
        *,
        token: object,
        receipt_bytes: bytes,
        files: tuple[VerifiedSnapshotFile, ...],
    ) -> None:
        if token is not _FROZEN_AUTHORITY_TOKEN:
            raise TypeError(
                "descriptor-rooted freeze authority is verifier-issued"
            )
        if (
            type(receipt_bytes) is not bytes or
            not receipt_bytes or
            type(files) is not tuple or
            not files or
            any(type(item) is not VerifiedSnapshotFile for item in files)
        ):
            raise SecureArtifactError(
                "descriptor-rooted freeze authority is malformed"
            )
        if receipt_bytes != _canonical_receipt(files):
            raise SecureArtifactError(
                "descriptor-rooted freeze authority receipt is not canonical"
            )
        bindings = tuple(
            (
                item.logical_name,
                item.source.relative,
                item.source.size,
                item.source.sha256,
            )
            for item in files
        )
        object.__setattr__(self, "receipt_bytes", receipt_bytes)
        object.__setattr__(self, "source_bindings", bindings)


@dataclass(frozen=True, slots=True)
class FrozenArtifactSet:
    receipt_sha256: str
    members: tuple[FrozenArtifactMember, ...]
    aggregate_sha256: str
    authority: DescriptorRootedFreezeAuthority | None = None

    def __post_init__(self) -> None:
        if (
            type(self.receipt_sha256) is not str
            or _HEX_SHA256_RE.fullmatch(self.receipt_sha256) is None
            or type(self.members) is not tuple
            or not self.members
            or any(type(member) is not FrozenArtifactMember for member in self.members)
        ):
            raise SecureArtifactError("frozen artifact set is malformed")
        names = tuple(member.logical_name for member in self.members)
        if names != tuple(sorted(names)) or len(set(names)) != len(names):
            raise SecureArtifactError(
                "frozen artifact members must be uniquely ordered"
            )
        if sum(member.size for member in self.members) > MAX_ARTIFACT_SET_BYTES:
            raise SecureArtifactError("frozen artifact set exceeds size bound")
        if (
            type(self.aggregate_sha256) is not str
            or _HEX_SHA256_RE.fullmatch(self.aggregate_sha256) is None
            or _aggregate_sha256(self.receipt_sha256, self.members)
            != self.aggregate_sha256
        ):
            raise SecureArtifactError(
                "frozen artifact aggregate is inconsistent"
            )
        if self.authority is not None:
            if type(self.authority) is not DescriptorRootedFreezeAuthority:
                raise SecureArtifactError(
                    "frozen artifact authority type is invalid"
                )
            if (
                hashlib.sha256(
                    self.authority.receipt_bytes
                ).hexdigest() != self.receipt_sha256
            ):
                raise SecureArtifactError(
                    "frozen artifact authority receipt is inconsistent"
                )
            bindings = self.authority.source_bindings
            if (
                tuple(binding[0] for binding in bindings) != names or
                tuple(
                    (binding[2], binding[3]) for binding in bindings
                ) != tuple(
                    (member.size, member.sha256)
                    for member in self.members
                )
            ):
                raise SecureArtifactError(
                    "frozen artifact authority members are inconsistent"
                )

    def member_bytes(self, logical_name: str) -> bytes:
        wanted = _validate_logical_name(logical_name)
        for member in self.members:
            if member.logical_name == wanted:
                return member.content
        raise SecureArtifactError("frozen artifact member is unavailable")

    def open_readonly(self, logical_name: str) -> "FrozenBytesView":
        wanted = _validate_logical_name(logical_name)
        return FrozenBytesView(wanted, self.member_bytes(wanted))


class FrozenBytesView:
    """A fresh read-only cursor over one immutable artifact byte string."""

    __slots__ = ("_closed", "_content", "_position", "_name")

    def __init__(self, logical_name: str, content: bytes) -> None:
        self._name = f"<frozen:{_validate_logical_name(logical_name)}>"
        if type(content) is not bytes:
            raise SecureArtifactError("frozen view content must be exact bytes")
        self._content = content
        self._position = 0
        self._closed = False

    @property
    def name(self) -> str:
        return self._name

    @property
    def closed(self) -> bool:
        return self._closed

    def _require_open(self) -> None:
        if self._closed:
            raise ValueError("I/O operation on closed frozen view")

    def read(self, size: int = -1) -> bytes:
        self._require_open()
        if type(size) is not int or isinstance(size, bool):
            raise TypeError("read size must be an integer")
        available = max(0, len(self._content) - self._position)
        if size < 0:
            count = available
        else:
            count = min(size, available)
        end = self._position + count
        result = self._content[self._position:end]
        self._position = end
        return result

    def readinto(self, buffer: object) -> int:
        self._require_open()
        try:
            view = memoryview(buffer)
            if view.readonly:
                raise TypeError("readinto buffer must be writable")
            byte_view = view.cast("B")
        except (TypeError, ValueError) as exc:
            raise TypeError("readinto requires a writable byte buffer") from exc
        available = max(0, len(self._content) - self._position)
        count = min(len(byte_view), available)
        byte_view[:count] = self._content[
            self._position:self._position + count
        ]
        self._position += count
        return count

    def seek(self, offset: int, whence: int = os.SEEK_SET) -> int:
        self._require_open()
        if type(offset) is not int or isinstance(offset, bool):
            raise TypeError("seek offset must be an integer")
        if whence == os.SEEK_SET:
            position = offset
        elif whence == os.SEEK_CUR:
            position = self._position + offset
        elif whence == os.SEEK_END:
            position = len(self._content) + offset
        else:
            raise ValueError("invalid seek whence")
        if position < 0:
            raise ValueError("negative seek position")
        self._position = position
        return position

    def tell(self) -> int:
        self._require_open()
        return self._position

    def write(self, _data: object) -> int:
        raise io.UnsupportedOperation("frozen view is read-only")

    def truncate(self, _size: int | None = None) -> int:
        raise io.UnsupportedOperation("frozen view is read-only")

    def fileno(self) -> int:
        raise io.UnsupportedOperation("frozen view has no file descriptor")

    def close(self) -> None:
        self._closed = True
        self._content = b""
        self._position = 0

    def __enter__(self) -> "FrozenBytesView":
        self._require_open()
        return self

    def __exit__(
        self,
        _exc_type: object,
        _exc: object,
        _traceback: object,
    ) -> None:
        self.close()


def _regular_identity(
    relative: str,
    info: os.stat_result,
    sha256: str,
) -> RegularFileIdentity:
    return RegularFileIdentity(
        relative=relative,
        device=info.st_dev,
        inode=info.st_ino,
        mode=info.st_mode,
        nlink=info.st_nlink,
        size=info.st_size,
        ctime_ns=info.st_ctime_ns,
        mtime_ns=info.st_mtime_ns,
        sha256=sha256,
    )


def _identity_tuple(identity: RegularFileIdentity) -> tuple[int, ...]:
    return (
        identity.device,
        identity.inode,
        identity.mode,
        identity.nlink,
        identity.size,
        identity.ctime_ns,
        identity.mtime_ns,
    )


def _validate_source_stat(
    info: os.stat_result,
    spec: SnapshotFileSpec,
) -> None:
    if not stat.S_ISREG(info.st_mode):
        raise SecureArtifactError("artifact source is not a regular file")
    if info.st_nlink != 1:
        raise SecureArtifactError("artifact source link count must be one")
    if stat.S_IMODE(info.st_mode) not in spec.allowed_modes:
        raise SecureArtifactError("artifact source mode is not allowed")
    if info.st_size < 0 or info.st_size > spec.max_size:
        raise SecureArtifactError("artifact source exceeds its size bound")
    if info.st_size > MAX_ARTIFACT_MEMBER_BYTES:
        raise SecureArtifactError("artifact source exceeds global member bound")


def _validate_private_file_stat(
    info: os.stat_result,
    *,
    size: int,
) -> None:
    if (
        not stat.S_ISREG(info.st_mode)
        or info.st_nlink != 1
        or stat.S_IMODE(info.st_mode) != 0o600
        or info.st_uid != os.geteuid()
        or info.st_size != size
    ):
        raise SecureArtifactError(
            "private artifact must remain one owner-private regular file"
        )


def _open_relative_source(
    root_fd: int,
    spec: SnapshotFileSpec,
) -> tuple[int, os.stat_result]:
    components = _relative_components(spec.relative)
    directory_fds: list[int] = []
    current_fd = root_fd
    try:
        for component in components[:-1]:
            try:
                before = os.stat(
                    component,
                    dir_fd=current_fd,
                    follow_symlinks=False,
                )
            except OSError as exc:
                raise SecureArtifactError(
                    "artifact source directory is unavailable"
                ) from exc
            if stat.S_ISLNK(before.st_mode) or not stat.S_ISDIR(
                before.st_mode
            ):
                raise SecureArtifactError(
                    "artifact source traverses a non-directory"
                )
            try:
                child_fd = os.open(
                    component,
                    _directory_open_flags(),
                    dir_fd=current_fd,
                )
            except OSError as exc:
                raise SecureArtifactError(
                    "artifact source directory cannot be opened safely"
                ) from exc
            opened = os.fstat(child_fd)
            if not _same_object(before, opened) or not stat.S_ISDIR(
                opened.st_mode
            ):
                os.close(child_fd)
                raise SecureArtifactError(
                    "artifact source directory changed during open"
                )
            directory_fds.append(child_fd)
            current_fd = child_fd

        leaf = components[-1]
        try:
            before_leaf = os.stat(
                leaf,
                dir_fd=current_fd,
                follow_symlinks=False,
            )
        except OSError as exc:
            raise SecureArtifactError(
                "artifact source is unavailable"
            ) from exc
        if stat.S_ISLNK(before_leaf.st_mode):
            raise SecureArtifactError("artifact source is a symlink")
        _validate_source_stat(before_leaf, spec)
        try:
            file_fd = os.open(
                leaf,
                _file_open_flags(),
                dir_fd=current_fd,
            )
        except OSError as exc:
            raise SecureArtifactError(
                "artifact source cannot be opened safely"
            ) from exc
        try:
            opened_leaf = os.fstat(file_fd)
            _validate_source_stat(opened_leaf, spec)
            if _stat_tuple(before_leaf) != _stat_tuple(opened_leaf):
                raise SecureArtifactError(
                    "artifact source changed during open"
                )
            return file_fd, opened_leaf
        except BaseException:
            os.close(file_fd)
            raise
    finally:
        for directory_fd in reversed(directory_fds):
            os.close(directory_fd)


def _read_exact_bytes(fd: int, expected_size: int) -> bytes:
    try:
        os.lseek(fd, 0, os.SEEK_SET)
        remaining = expected_size
        chunks: list[bytes] = []
        while remaining:
            try:
                chunk = os.read(fd, min(_COPY_CHUNK_BYTES, remaining))
            except InterruptedError:
                continue
            if not chunk:
                raise SecureArtifactError(
                    "retained artifact ended before its receipt size"
                )
            chunks.append(chunk)
            remaining -= len(chunk)
        while True:
            try:
                extra = os.read(fd, 1)
                break
            except InterruptedError:
                continue
        if extra:
            raise SecureArtifactError(
                "retained artifact has bytes beyond its receipt size"
            )
        return b"".join(chunks)
    except OSError as exc:
        raise SecureArtifactError(
            "retained artifact cannot be read exactly"
        ) from exc


def _write_all(fd: int, data: bytes) -> None:
    view = memoryview(data)
    written = 0
    while written < len(view):
        try:
            count = os.write(fd, view[written:])
        except InterruptedError:
            continue
        except OSError as exc:
            raise SecureArtifactError(
                "private artifact write failed"
            ) from exc
        if count <= 0:
            raise SecureArtifactError("private artifact write was short")
        written += count


def _copy_source_to_private(
    source_fd: int,
    destination_fd: int,
    source_a: os.stat_result,
    spec: SnapshotFileSpec,
) -> str:
    try:
        os.lseek(source_fd, 0, os.SEEK_SET)
        remaining = source_a.st_size
        first_hash = hashlib.sha256()
        while remaining:
            try:
                chunk = os.read(
                    source_fd,
                    min(_COPY_CHUNK_BYTES, remaining),
                )
            except InterruptedError:
                continue
            if not chunk:
                raise SecureArtifactError(
                    "artifact source ended during first read"
                )
            first_hash.update(chunk)
            _write_all(destination_fd, chunk)
            remaining -= len(chunk)
        try:
            extra = os.read(source_fd, 1)
        except InterruptedError:
            extra = os.read(source_fd, 1)
        if extra:
            raise SecureArtifactError(
                "artifact source grew during first read"
            )
        os.fsync(destination_fd)
        _test_hook("source_after_copy", spec.logical_name)
        source_b = os.fstat(source_fd)
        _test_hook("source_before_second_read", spec.logical_name)
        second = _read_exact_bytes(source_fd, source_a.st_size)
        source_c = os.fstat(source_fd)
    except OSError as exc:
        raise SecureArtifactError("artifact source copy failed") from exc
    if not (
        _stat_tuple(source_a)
        == _stat_tuple(source_b)
        == _stat_tuple(source_c)
    ):
        raise SecureArtifactError("artifact source changed during two-pass copy")
    first_digest = first_hash.hexdigest()
    second_digest = hashlib.sha256(second).hexdigest()
    if first_digest != second_digest:
        raise SecureArtifactError("artifact source reads do not match")
    return first_digest


def _verify_reopened_source(
    root_fd: int,
    spec: SnapshotFileSpec,
    wanted: os.stat_result,
) -> None:
    reopened_fd = -1
    try:
        reopened_fd, reopened = _open_relative_source(root_fd, spec)
        if _stat_tuple(reopened) != _stat_tuple(wanted):
            raise SecureArtifactError(
                "artifact source pathname changed after copy"
            )
    finally:
        if reopened_fd >= 0:
            os.close(reopened_fd)


def _identity_json(identity: RegularFileIdentity) -> dict[str, object]:
    return {
        "ctime_ns": identity.ctime_ns,
        "device": identity.device,
        "inode": identity.inode,
        "mode": identity.mode,
        "mtime_ns": identity.mtime_ns,
        "nlink": identity.nlink,
        "relative": identity.relative,
        "sha256": identity.sha256,
        "size": identity.size,
    }


def _canonical_receipt(files: tuple[VerifiedSnapshotFile, ...]) -> bytes:
    payload = {
        "files": [
            {
                "logical_name": item.logical_name,
                "private": _identity_json(item.private),
                "private_relative": item.private_relative,
                "source": _identity_json(item.source),
            }
            for item in files
        ],
        "schema": _SNAPSHOT_RECEIPT_SCHEMA,
    }
    return (
        json.dumps(
            payload,
            ensure_ascii=True,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
        + b"\n"
    )


def _rename_noreplace(
    directory_fd: int,
    source_name: str,
    destination_name: str,
) -> None:
    libc = ctypes.CDLL(None, use_errno=True)
    source = os.fsencode(source_name)
    destination = os.fsencode(destination_name)
    if sys.platform == "darwin" and hasattr(libc, "renameatx_np"):
        rename = libc.renameatx_np
        rename.argtypes = [
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_uint,
        ]
        rename.restype = ctypes.c_int
        result = rename(
            directory_fd,
            source,
            directory_fd,
            destination,
            0x00000004,
        )
    elif sys.platform.startswith("linux") and hasattr(libc, "renameat2"):
        rename = libc.renameat2
        rename.argtypes = [
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_uint,
        ]
        rename.restype = ctypes.c_int
        result = rename(
            directory_fd,
            source,
            directory_fd,
            destination,
            1,
        )
    else:
        raise SecureArtifactError(
            "atomic no-replace rename is unavailable"
        )
    if result != 0:
        error_number = ctypes.get_errno()
        raise SecureArtifactError(
            "atomic no-replace rename failed"
        ) from OSError(error_number, os.strerror(error_number))


@dataclass(slots=True)
class _RetainedMember:
    verified: VerifiedSnapshotFile
    spec: SnapshotFileSpec | None
    fd: int


def _validate_retained_named_file(
    root_fd: int,
    member: _RetainedMember,
) -> os.stat_result:
    try:
        opened = os.fstat(member.fd)
        named = os.stat(
            member.verified.private_relative,
            dir_fd=root_fd,
            follow_symlinks=False,
        )
    except OSError as exc:
        raise SecureArtifactError(
            "retained private artifact binding is unavailable"
        ) from exc
    wanted = _identity_tuple(member.verified.private)
    if (
        _stat_tuple(opened) != wanted
        or _stat_tuple(named) != wanted
        or not _same_object(opened, named)
    ):
        raise SecureArtifactError(
            "retained private artifact binding changed"
        )
    _validate_private_file_stat(opened, size=member.verified.private.size)
    return opened


def _remove_private_root(
    *,
    parent_fd: int,
    root_fd: int,
    root_binding: _DirectoryBinding,
    reserved_names: set[str],
    known_identities: set[tuple[int, int]],
) -> list[str]:
    errors: list[str] = []
    # A reserved pathname is not proof of ownership.  Cleanup authorization
    # comes only from the exact device/inode identities captured at creation.
    _ = reserved_names
    root_valid = False
    try:
        opened_root = os.fstat(root_fd)
        root_valid = _directory_matches(opened_root, root_binding)
    except OSError as exc:
        errors.append(f"inspect private root: {exc}")

    if root_valid:
        try:
            names = tuple(os.listdir(root_fd))
        except OSError as exc:
            errors.append(f"list private root: {exc}")
            names = ()
        for name in names:
            try:
                info = os.stat(
                    name,
                    dir_fd=root_fd,
                    follow_symlinks=False,
                )
            except OSError as exc:
                if exc.errno != errno.ENOENT:
                    errors.append(f"inspect private entry: {exc}")
                continue
            known_object = (info.st_dev, info.st_ino) in known_identities
            if not known_object:
                continue
            if stat.S_ISDIR(info.st_mode):
                errors.append("owned private entry became a directory")
                continue
            try:
                os.unlink(name, dir_fd=root_fd)
            except OSError as exc:
                if exc.errno != errno.ENOENT:
                    errors.append(f"unlink private entry: {exc}")
        try:
            os.fsync(root_fd)
        except OSError as exc:
            errors.append(f"fsync private root: {exc}")

    try:
        os.close(root_fd)
    except OSError as exc:
        errors.append(f"close private root: {exc}")

    try:
        parent_names = tuple(os.listdir(parent_fd))
    except OSError as exc:
        errors.append(f"list private parent: {exc}")
        parent_names = ()
    for name in parent_names:
        try:
            candidate = os.stat(
                name,
                dir_fd=parent_fd,
                follow_symlinks=False,
            )
        except OSError:
            continue
        if (
            stat.S_ISDIR(candidate.st_mode)
            and candidate.st_dev == root_binding.device
            and candidate.st_ino == root_binding.inode
        ):
            try:
                os.rmdir(name, dir_fd=parent_fd)
            except OSError as exc:
                errors.append(f"remove private root: {exc}")
            break
    try:
        os.fsync(parent_fd)
    except OSError as exc:
        errors.append(f"fsync private parent: {exc}")
    return errors


def _pinned_interpreter() -> tuple[Path, int, os.stat_result]:
    rendered = os.path.realpath(sys.executable)
    interpreter = _lexical_absolute(
        rendered,
        "partition generator interpreter",
    )
    parent_bindings = _open_directory_chain(interpreter.parent)
    interpreter_fd = -1
    try:
        parent_fd = parent_bindings[-1].fd
        try:
            before = os.stat(
                interpreter.name,
                dir_fd=parent_fd,
                follow_symlinks=False,
            )
        except OSError as exc:
            raise SecureArtifactError(
                "partition generator interpreter is unavailable"
            ) from exc
        if (
            stat.S_ISLNK(before.st_mode)
            or not stat.S_ISREG(before.st_mode)
            or stat.S_IMODE(before.st_mode) & 0o111 == 0
            or stat.S_IMODE(before.st_mode) & 0o022 != 0
        ):
            raise SecureArtifactError(
                "partition generator interpreter is not pinned executable"
            )
        try:
            interpreter_fd = os.open(
                interpreter.name,
                _file_open_flags(),
                dir_fd=parent_fd,
            )
        except OSError as exc:
            raise SecureArtifactError(
                "partition generator interpreter cannot be opened safely"
            ) from exc
        opened = os.fstat(interpreter_fd)
        if _stat_tuple(opened) != _stat_tuple(before):
            raise SecureArtifactError(
                "partition generator interpreter changed during open"
            )
        return interpreter, interpreter_fd, opened
    except BaseException:
        if interpreter_fd >= 0:
            os.close(interpreter_fd)
        raise
    finally:
        _close_directory_chain(parent_bindings)


def run_private_partition_generator(
    frozen_inputs: FrozenArtifactSet,
    *,
    csv_logical_name: str,
    generator_logical_name: str,
    expected_logical_name: str,
    output_logical_name: str,
    private_parent: os.PathLike[str] | str,
) -> GeneratedArtifactMember:
    """Run a snapshotted generator and compare its private output exactly."""
    if type(frozen_inputs) is not FrozenArtifactSet:
        raise SecureArtifactError(
            "partition generation requires one frozen artifact set"
        )
    csv_name = _validate_logical_name(csv_logical_name)
    generator_name = _validate_logical_name(generator_logical_name)
    expected_name = _validate_logical_name(expected_logical_name)
    output_name = _validate_logical_name(output_logical_name)
    csv_bytes = frozen_inputs.member_bytes(csv_name)
    generator_bytes = frozen_inputs.member_bytes(generator_name)
    expected_bytes = frozen_inputs.member_bytes(expected_name)

    private_parent_path = _lexical_absolute(
        private_parent,
        "partition generator private parent",
    )
    parent_bindings = _open_directory_chain(private_parent_path)
    parent_fd = -1
    root_fd = -1
    root_binding: _DirectoryBinding | None = None
    root_name = ""
    generator_fd = -1
    csv_fd = -1
    output_fd = -1
    interpreter_fd = -1
    known_identities: set[tuple[int, int]] = set()
    primary_error: BaseException | None = None
    result: GeneratedArtifactMember | None = None
    try:
        parent_info = os.fstat(parent_bindings[-1].fd)
        if (
            not stat.S_ISDIR(parent_info.st_mode)
            or parent_info.st_uid != os.geteuid()
            or stat.S_IMODE(parent_info.st_mode) != 0o700
        ):
            raise SecureArtifactError(
                "partition generator private parent must be owner 0700"
            )
        parent_fd = _duplicate_cloexec(parent_bindings[-1].fd)
        _close_directory_chain(parent_bindings)
        parent_bindings = ()
        for _attempt in range(128):
            candidate = "fof-partition-generator-" + secrets.token_hex(16)
            try:
                os.mkdir(candidate, 0o700, dir_fd=parent_fd)
                root_name = candidate
                break
            except FileExistsError:
                continue
            except OSError as exc:
                raise SecureArtifactError(
                    "partition generator workspace cannot be created"
                ) from exc
        else:
            raise SecureArtifactError(
                "partition generator workspace name allocation failed"
            )
        created_root = os.stat(
            root_name,
            dir_fd=parent_fd,
            follow_symlinks=False,
        )
        root_fd = os.open(
            root_name,
            _directory_open_flags(),
            dir_fd=parent_fd,
        )
        opened_root = os.fstat(root_fd)
        if (
            not stat.S_ISDIR(opened_root.st_mode)
            or not _same_object(created_root, opened_root)
        ):
            raise SecureArtifactError(
                "partition generator workspace changed during open"
            )
        os.fchmod(root_fd, 0o700)
        root_info = os.fstat(root_fd)
        if (
            stat.S_IMODE(root_info.st_mode) != 0o700
            or root_info.st_uid != os.geteuid()
        ):
            raise SecureArtifactError(
                "partition generator workspace is not owner 0700"
            )
        root_binding = _directory_binding(root_name, root_fd, root_info)
        os.fsync(parent_fd)

        def write_input(name: str, content: bytes, mode: int) -> None:
            writer = -1
            try:
                writer = os.open(
                    name,
                    _file_open_flags(write=True, create=True),
                    mode,
                    dir_fd=root_fd,
                )
                created = os.fstat(writer)
                known_identities.add((created.st_dev, created.st_ino))
                os.fchmod(writer, mode)
                _write_all(writer, content)
                os.fsync(writer)
                written = os.fstat(writer)
                if (
                    not stat.S_ISREG(written.st_mode)
                    or written.st_nlink != 1
                    or stat.S_IMODE(written.st_mode) != mode
                    or written.st_uid != os.geteuid()
                    or written.st_size != len(content)
                ):
                    raise SecureArtifactError(
                        "partition generator input was not written privately"
                    )
            finally:
                if writer >= 0:
                    os.close(writer)

        write_input("partition-source.csv", csv_bytes, 0o600)
        write_input("partition-generator.py", generator_bytes, 0o700)
        nofollow = getattr(os, "O_NOFOLLOW", None)
        if nofollow is None:
            raise SecureArtifactError(
                "partition generator output requires O_NOFOLLOW"
            )
        output_fd = os.open(
            "generated-output.bin",
            os.O_RDWR
            | os.O_CREAT
            | os.O_EXCL
            | nofollow
            | getattr(os, "O_CLOEXEC", 0),
            0o600,
            dir_fd=root_fd,
        )
        output_created = os.fstat(output_fd)
        known_identities.add(
            (output_created.st_dev, output_created.st_ino)
        )
        os.fchmod(output_fd, 0o600)
        os.fsync(output_fd)

        generator_fd = os.open(
            "partition-generator.py",
            _file_open_flags(),
            dir_fd=root_fd,
        )
        csv_fd = os.open(
            "partition-source.csv",
            _file_open_flags(),
            dir_fd=root_fd,
        )
        generator_info = os.fstat(generator_fd)
        csv_info = os.fstat(csv_fd)
        if (
            _read_exact_bytes(generator_fd, len(generator_bytes))
            != generator_bytes
            or _read_exact_bytes(csv_fd, len(csv_bytes)) != csv_bytes
        ):
            raise SecureArtifactError(
                "private partition generator inputs changed"
            )
        interpreter_path, interpreter_fd, interpreter_info = (
            _pinned_interpreter()
        )
        os.lseek(generator_fd, 0, os.SEEK_SET)
        os.lseek(csv_fd, 0, os.SEEK_SET)
        _test_hook("partition_generator_before_exec", generator_name)
        completed = subprocess.run(
            [
                os.fspath(interpreter_path),
                "-c",
                _PRIVATE_GENERATOR_RUNNER,
                str(output_fd),
                f"/dev/fd/{output_fd}",
                str(generator_fd),
                f"/dev/fd/{generator_fd}",
                "--quiet",
                "--flash-size",
                "8MB",
                "--offset",
                "0x8000",
                f"/dev/fd/{csv_fd}",
                f"/dev/fd/{output_fd}",
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            cwd=os.fspath(private_parent_path / root_name),
            env={
                "LANG": "C",
                "LC_ALL": "C",
                "PYTHONHASHSEED": "0",
                "PYTHONDONTWRITEBYTECODE": "1",
                "PYTHONNOUSERSITE": "1",
            },
            close_fds=True,
            pass_fds=(generator_fd, csv_fd, output_fd),
            timeout=30,
            check=False,
        )
        _test_hook("partition_generator_after_exec", generator_name)
        if completed.returncode != 0:
            raise SecureArtifactError(
                "private partition generator execution failed"
            )
        interpreter_after = os.fstat(interpreter_fd)
        interpreter_named_after = os.stat(
            interpreter_path,
            follow_symlinks=False,
        )
        if (
            _stat_tuple(interpreter_after)
            != _stat_tuple(interpreter_info)
            or _stat_tuple(interpreter_named_after)
            != _stat_tuple(interpreter_info)
        ):
            raise SecureArtifactError(
                "partition generator interpreter identity changed"
            )
        generator_after = os.fstat(generator_fd)
        csv_after = os.fstat(csv_fd)
        if (
            _stat_tuple(generator_after) != _stat_tuple(generator_info)
            or _stat_tuple(csv_after) != _stat_tuple(csv_info)
            or _read_exact_bytes(generator_fd, len(generator_bytes))
            != generator_bytes
            or _read_exact_bytes(csv_fd, len(csv_bytes)) != csv_bytes
        ):
            raise SecureArtifactError(
                "private partition generator inputs drifted during execution"
            )
        output_named = os.stat(
            "generated-output.bin",
            dir_fd=root_fd,
            follow_symlinks=False,
        )
        output_opened = os.fstat(output_fd)
        if (
            not stat.S_ISREG(output_named.st_mode)
            or output_named.st_nlink != 1
            or stat.S_IMODE(output_named.st_mode) != 0o600
            or output_named.st_uid != os.geteuid()
            or not _same_object(output_named, output_created)
            or not _same_object(output_opened, output_created)
            or output_named.st_size > MAX_ARTIFACT_MEMBER_BYTES
        ):
            raise SecureArtifactError(
                "private partition generator output identity changed"
            )
        first = _read_exact_bytes(output_fd, output_named.st_size)
        output_middle = os.fstat(output_fd)
        second = _read_exact_bytes(output_fd, output_named.st_size)
        output_after = os.fstat(output_fd)
        if (
            _stat_tuple(output_named) != _stat_tuple(output_middle)
            or _stat_tuple(output_named) != _stat_tuple(output_after)
            or first != second
            or second != expected_bytes
        ):
            raise SecureArtifactError(
                "partition source does not reproduce partitions.bin"
            )
        os.fsync(root_fd)
        result = GeneratedArtifactMember(
            token=_GENERATED_MEMBER_TOKEN,
            logical_name=output_name,
            content=second,
        )
    except BaseException as exc:
        primary_error = exc

    cleanup_errors: list[str] = []
    try:
        _test_hook("partition_generator_before_cleanup", output_name)
    except BaseException as hook_exc:
        if primary_error is None:
            primary_error = hook_exc
        else:
            cleanup_errors.append(
                f"partition generator cleanup hook: {hook_exc}"
            )
    for label, fd in (
        ("output", output_fd),
        ("csv", csv_fd),
        ("generator", generator_fd),
        ("interpreter", interpreter_fd),
    ):
        if fd >= 0:
            try:
                os.close(fd)
            except OSError as exc:
                cleanup_errors.append(
                    f"close partition generator {label}: {exc}"
                )
    output_fd = csv_fd = generator_fd = interpreter_fd = -1
    if (
        root_fd >= 0
        and parent_fd >= 0
        and root_binding is not None
    ):
        cleanup_errors.extend(_remove_private_root(
            parent_fd=parent_fd,
            root_fd=root_fd,
            root_binding=root_binding,
            reserved_names={
                "partition-source.csv",
                "partition-generator.py",
                "generated-output.bin",
            },
            known_identities=known_identities,
        ))
        root_fd = -1
    elif root_fd >= 0:
        try:
            os.close(root_fd)
        except OSError as exc:
            cleanup_errors.append(
                f"close partition generator workspace: {exc}"
            )
        root_fd = -1
    if parent_fd >= 0:
        try:
            os.close(parent_fd)
        except OSError as exc:
            cleanup_errors.append(
                f"close partition generator parent: {exc}"
            )
        parent_fd = -1
    if parent_bindings:
        _close_directory_chain(parent_bindings)
    if cleanup_errors:
        cleanup_failure = SecureArtifactError(
            "partition generator cleanup failed: "
            + "; ".join(cleanup_errors)
        )
        if primary_error is not None:
            raise cleanup_failure from primary_error
        raise cleanup_failure
    if primary_error is not None:
        if isinstance(primary_error, SecureArtifactError):
            raise primary_error
        if isinstance(primary_error, subprocess.TimeoutExpired):
            raise SecureArtifactError(
                "private partition generator timed out"
            ) from primary_error
        if isinstance(primary_error, Exception):
            raise SecureArtifactError(
                "private partition generator failed"
            ) from primary_error
        raise primary_error
    assert result is not None
    return result


class VerifiedBadgeArtifactSnapshot:
    """Retained validation capability with a one-way immutable freeze."""

    __slots__ = (
        "private_root",
        "files",
        "receipt_sha256",
        "_lock",
        "_state",
        "_source_root_path",
        "_source_root_fd",
        "_source_root_device",
        "_source_root_inode",
        "_private_parent_fd",
        "_private_root_fd",
        "_private_root_binding",
        "_private_root_name",
        "_receipt_fd",
        "_receipt_identity",
        "_receipt_bytes",
        "_retained",
        "_reserved_names",
    )

    def __init__(
        self,
        *,
        token: object,
        private_root: Path,
        files: tuple[VerifiedSnapshotFile, ...],
        receipt_sha256: str,
        source_root_path: Path,
        source_root_fd: int,
        source_root_device: int,
        source_root_inode: int,
        private_parent_fd: int,
        private_root_fd: int,
        private_root_binding: _DirectoryBinding,
        private_root_name: str,
        receipt_fd: int,
        receipt_identity: RegularFileIdentity,
        receipt_bytes: bytes,
        retained: list[_RetainedMember],
        reserved_names: set[str],
    ) -> None:
        if token is not _SNAPSHOT_TOKEN:
            raise TypeError(
                "VerifiedBadgeArtifactSnapshot is verifier-issued only"
            )
        self.private_root = private_root
        self.files = files
        self.receipt_sha256 = receipt_sha256
        self._lock = threading.RLock()
        self._state = "OPEN"
        self._source_root_path = source_root_path
        self._source_root_fd = source_root_fd
        self._source_root_device = source_root_device
        self._source_root_inode = source_root_inode
        self._private_parent_fd = private_parent_fd
        self._private_root_fd = private_root_fd
        self._private_root_binding = private_root_binding
        self._private_root_name = private_root_name
        self._receipt_fd = receipt_fd
        self._receipt_identity = receipt_identity
        self._receipt_bytes = receipt_bytes
        self._retained = retained
        self._reserved_names = reserved_names

    @property
    def state(self) -> str:
        return self._state

    def _require_open(self) -> None:
        if self._state != "OPEN":
            raise SecureArtifactError(
                "artifact snapshot is not in the OPEN state"
            )

    def _validate_source_root(self) -> None:
        try:
            opened = os.fstat(self._source_root_fd)
        except OSError as exc:
            raise SecureArtifactError(
                "retained source root is unavailable"
            ) from exc
        if (
            not stat.S_ISDIR(opened.st_mode)
            or opened.st_dev != self._source_root_device
            or opened.st_ino != self._source_root_inode
        ):
            raise SecureArtifactError("retained source root identity changed")
        _retrace_final_directory(
            self._source_root_path,
            device=self._source_root_device,
            inode=self._source_root_inode,
        )

    def _validate_private_root(self) -> None:
        try:
            opened = os.fstat(self._private_root_fd)
            named = os.stat(
                self._private_root_name,
                dir_fd=self._private_parent_fd,
                follow_symlinks=False,
            )
        except OSError as exc:
            raise SecureArtifactError(
                "private snapshot root binding is unavailable"
            ) from exc
        binding = self._private_root_binding
        if (
            not _directory_matches(opened, binding)
            or not _directory_matches(named, binding)
            or stat.S_IMODE(opened.st_mode) != 0o700
            or opened.st_uid != os.geteuid()
        ):
            raise SecureArtifactError(
                "private snapshot root binding changed"
            )
        expected_names = {
            member.verified.private_relative for member in self._retained
        }
        expected_names.add("receipt.json")
        try:
            actual_names = set(os.listdir(self._private_root_fd))
        except OSError as exc:
            raise SecureArtifactError(
                "private snapshot inventory cannot be read"
            ) from exc
        if actual_names != expected_names:
            raise SecureArtifactError(
                "private snapshot inventory changed"
            )

    def revalidate_sources(self) -> None:
        with self._lock:
            self._require_open()
            self._validate_source_root()
            for member in self._retained:
                if member.spec is None:
                    continue
                source_fd = -1
                try:
                    source_fd, opened = _open_relative_source(
                        self._source_root_fd,
                        member.spec,
                    )
                    wanted = _identity_tuple(member.verified.source)
                    if _stat_tuple(opened) != wanted:
                        raise SecureArtifactError(
                            "artifact source identity changed"
                        )
                    first = _read_exact_bytes(
                        source_fd,
                        member.verified.source.size,
                    )
                    middle = os.fstat(source_fd)
                    second = _read_exact_bytes(
                        source_fd,
                        member.verified.source.size,
                    )
                    final = os.fstat(source_fd)
                    if (
                        _stat_tuple(opened)
                        != _stat_tuple(middle)
                        or _stat_tuple(opened)
                        != _stat_tuple(final)
                        or first != second
                        or hashlib.sha256(first).hexdigest()
                        != member.verified.source.sha256
                    ):
                        raise SecureArtifactError(
                            "artifact source content continuity failed"
                        )
                    _verify_reopened_source(
                        self._source_root_fd,
                        member.spec,
                        opened,
                    )
                except OSError as exc:
                    raise SecureArtifactError(
                        "artifact source revalidation failed"
                    ) from exc
                finally:
                    if source_fd >= 0:
                        os.close(source_fd)
            self._validate_source_root()

    def revalidate_retained_files(self) -> None:
        with self._lock:
            self._require_open()
            self._validate_private_root()
            try:
                receipt_opened = os.fstat(self._receipt_fd)
                receipt_named = os.stat(
                    "receipt.json",
                    dir_fd=self._private_root_fd,
                    follow_symlinks=False,
                )
            except OSError as exc:
                raise SecureArtifactError(
                    "private receipt binding is unavailable"
                ) from exc
            wanted_receipt = _identity_tuple(self._receipt_identity)
            if (
                _stat_tuple(receipt_opened) != wanted_receipt
                or _stat_tuple(receipt_named) != wanted_receipt
            ):
                raise SecureArtifactError("private receipt binding changed")
            receipt = _read_exact_bytes(
                self._receipt_fd,
                self._receipt_identity.size,
            )
            receipt_after = os.fstat(self._receipt_fd)
            if (
                _stat_tuple(receipt_after) != wanted_receipt
                or receipt != self._receipt_bytes
                or hashlib.sha256(receipt).hexdigest()
                != self.receipt_sha256
            ):
                raise SecureArtifactError("private receipt content changed")

            for member in self._retained:
                before = _validate_retained_named_file(
                    self._private_root_fd,
                    member,
                )
                content = _read_exact_bytes(
                    member.fd,
                    member.verified.private.size,
                )
                after = os.fstat(member.fd)
                if (
                    _stat_tuple(before) != _stat_tuple(after)
                    or hashlib.sha256(content).hexdigest()
                    != member.verified.private.sha256
                ):
                    raise SecureArtifactError(
                        "retained private artifact content changed"
                    )
            self._validate_private_root()
            try:
                receipt_final = os.fstat(self._receipt_fd)
                receipt_named_final = os.stat(
                    "receipt.json",
                    dir_fd=self._private_root_fd,
                    follow_symlinks=False,
                )
            except OSError as exc:
                raise SecureArtifactError(
                    "private receipt final binding is unavailable"
                ) from exc
            if (
                _stat_tuple(receipt_final) != wanted_receipt
                or _stat_tuple(receipt_named_final) != wanted_receipt
            ):
                raise SecureArtifactError(
                    "private receipt final binding changed"
                )
            for member in self._retained:
                _validate_retained_named_file(
                    self._private_root_fd,
                    member,
                )

    def _close_mutation_input_fds(self) -> None:
        errors: list[OSError] = []
        for member in self._retained:
            if member.fd >= 0:
                try:
                    os.close(member.fd)
                except OSError as exc:
                    errors.append(exc)
                member.fd = -1
        if self._receipt_fd >= 0:
            try:
                os.close(self._receipt_fd)
            except OSError as exc:
                errors.append(exc)
            self._receipt_fd = -1
        if self._source_root_fd >= 0:
            try:
                os.close(self._source_root_fd)
            except OSError as exc:
                errors.append(exc)
            self._source_root_fd = -1
        if errors:
            raise SecureArtifactError(
                "artifact mutation input descriptors did not close"
            ) from errors[0]

    def freeze_for_mutation(self) -> FrozenArtifactSet:
        with self._lock:
            self._require_open()
            try:
                self.revalidate_sources()
                self.revalidate_retained_files()
                total_size = 0
                frozen_members: list[FrozenArtifactMember] = []
                for member in sorted(
                    self._retained,
                    key=lambda item: item.verified.logical_name,
                ):
                    logical_name = member.verified.logical_name
                    _test_hook("freeze_before_first_read", logical_name)
                    source_a = _validate_retained_named_file(
                        self._private_root_fd,
                        member,
                    )
                    first = _read_exact_bytes(
                        member.fd,
                        member.verified.private.size,
                    )
                    source_b = os.fstat(member.fd)
                    _validate_retained_named_file(
                        self._private_root_fd,
                        member,
                    )
                    _test_hook("freeze_between_reads", logical_name)
                    second = _read_exact_bytes(
                        member.fd,
                        member.verified.private.size,
                    )
                    source_c = os.fstat(member.fd)
                    _validate_retained_named_file(
                        self._private_root_fd,
                        member,
                    )
                    wanted = _identity_tuple(member.verified.private)
                    first_digest = hashlib.sha256(first).hexdigest()
                    second_digest = hashlib.sha256(second).hexdigest()
                    if (
                        _stat_tuple(source_a) != wanted
                        or _stat_tuple(source_b) != wanted
                        or _stat_tuple(source_c) != wanted
                        or first != second
                        or first_digest
                        != member.verified.private.sha256
                        or second_digest
                        != member.verified.private.sha256
                    ):
                        raise SecureArtifactError(
                            "private artifact changed during final freeze"
                        )
                    total_size += len(second)
                    if total_size > MAX_ARTIFACT_SET_BYTES:
                        raise SecureArtifactError(
                            "frozen artifact set exceeds size bound"
                        )
                    frozen_members.append(
                        FrozenArtifactMember(
                            logical_name=logical_name,
                            size=len(second),
                            sha256=second_digest,
                            content=second,
                        )
                    )
                    first = b""
                    _test_hook("freeze_after_member", logical_name)

                self.revalidate_sources()
                self.revalidate_retained_files()
                members = tuple(frozen_members)
                frozen = FrozenArtifactSet(
                    receipt_sha256=self.receipt_sha256,
                    members=members,
                    aggregate_sha256=_aggregate_sha256(
                        self.receipt_sha256,
                        members,
                    ),
                    authority=DescriptorRootedFreezeAuthority(
                        token=_FROZEN_AUTHORITY_TOKEN,
                        receipt_bytes=self._receipt_bytes,
                        files=self.files,
                    ),
                )
                self._close_mutation_input_fds()
                self._state = "FROZEN"
                return frozen
            except BaseException as exc:
                cleanup_error: BaseException | None = None
                try:
                    self.close()
                except BaseException as close_exc:
                    cleanup_error = close_exc
                if cleanup_error is not None:
                    add_note = getattr(exc, "add_note", None)
                    if callable(add_note):
                        add_note(
                            "artifact snapshot cleanup also failed: "
                            f"{cleanup_error}"
                        )
                if isinstance(exc, SecureArtifactError):
                    raise
                if isinstance(exc, Exception):
                    raise SecureArtifactError(
                        "artifact final freeze failed"
                    ) from exc
                raise

    def close(self) -> None:
        with self._lock:
            if self._state == "CLOSED":
                return
            errors: list[str] = []
            for member in self._retained:
                if member.fd >= 0:
                    try:
                        os.close(member.fd)
                    except OSError as exc:
                        errors.append(f"close private artifact: {exc}")
                    member.fd = -1
            if self._receipt_fd >= 0:
                try:
                    os.close(self._receipt_fd)
                except OSError as exc:
                    errors.append(f"close private receipt: {exc}")
                self._receipt_fd = -1
            if self._source_root_fd >= 0:
                try:
                    os.close(self._source_root_fd)
                except OSError as exc:
                    errors.append(f"close source root: {exc}")
                self._source_root_fd = -1

            if self._private_root_fd >= 0 and self._private_parent_fd >= 0:
                known_identities = {
                    (
                        member.verified.private.device,
                        member.verified.private.inode,
                    )
                    for member in self._retained
                }
                known_identities.add(
                    (
                        self._receipt_identity.device,
                        self._receipt_identity.inode,
                    )
                )
                errors.extend(_remove_private_root(
                    parent_fd=self._private_parent_fd,
                    root_fd=self._private_root_fd,
                    root_binding=self._private_root_binding,
                    reserved_names=set(self._reserved_names),
                    known_identities=known_identities,
                ))
                self._private_root_fd = -1
            if self._private_parent_fd >= 0:
                try:
                    os.close(self._private_parent_fd)
                except OSError as exc:
                    errors.append(f"close private parent: {exc}")
                self._private_parent_fd = -1
            self._state = "CLOSED"
            if errors:
                raise SecureArtifactError(
                    "private artifact snapshot cleanup failed: "
                    + "; ".join(errors)
                )

    def __enter__(self) -> "VerifiedBadgeArtifactSnapshot":
        self._require_open()
        return self

    def __exit__(
        self,
        _exc_type: object,
        _exc: object,
        _traceback: object,
    ) -> None:
        self.close()


class SecureArtifactTree:
    """Pinned descriptor-rooted view of one source artifact directory."""

    __slots__ = ("root", "_bindings", "_closed")

    def __init__(
        self,
        root: Path,
        bindings: tuple[_DirectoryBinding, ...],
    ) -> None:
        self.root = root
        self._bindings = bindings
        self._closed = False

    @classmethod
    def open(
        cls,
        root: os.PathLike[str] | str,
    ) -> "SecureArtifactTree":
        absolute = _lexical_absolute(root, "artifact root")
        bindings = _open_directory_chain(absolute)
        return cls(absolute, bindings)

    def _require_open(self) -> None:
        if self._closed:
            raise SecureArtifactError("secure artifact tree is closed")
        _validate_directory_chain(self._bindings)

    @property
    def _root_fd(self) -> int:
        return self._bindings[-1].fd

    def materialize_alias(
        self,
        canonical: SnapshotFileSpec,
        alias_relative: str,
    ) -> None:
        """Exclusively publish one content-identical, distinct-inode alias."""
        self._require_open()
        if type(canonical) is not SnapshotFileSpec:
            raise SecureArtifactError("canonical alias specification is malformed")
        alias_components = _relative_components(alias_relative)
        if canonical.relative == alias_relative:
            raise SecureArtifactError(
                "canonical artifact and alias path must be distinct"
            )

        source_fd = -1
        alias_fd = -1
        temporary_fd = -1
        opened_directories: list[int] = []
        created_directories: list[
            tuple[int, str, int, int]
        ] = []
        temporary_name = ""
        temporary_identity: tuple[int, int] | None = None
        alias_name = alias_components[-1]
        alias_was_absent = False
        try:
            source_fd, source_a = _open_relative_source(
                self._root_fd,
                canonical,
            )
            current_fd = self._root_fd
            for component in alias_components[:-1]:
                try:
                    before = os.stat(
                        component,
                        dir_fd=current_fd,
                        follow_symlinks=False,
                    )
                except FileNotFoundError:
                    try:
                        os.mkdir(component, 0o700, dir_fd=current_fd)
                    except OSError as exc:
                        raise SecureArtifactError(
                            "alias destination directory cannot be created"
                        ) from exc
                    before = os.stat(
                        component,
                        dir_fd=current_fd,
                        follow_symlinks=False,
                    )
                    created_directories.append(
                        (
                            current_fd,
                            component,
                            before.st_dev,
                            before.st_ino,
                        )
                    )
                    os.fsync(current_fd)
                except OSError as exc:
                    raise SecureArtifactError(
                        "alias destination directory is unavailable"
                    ) from exc
                if stat.S_ISLNK(before.st_mode) or not stat.S_ISDIR(
                    before.st_mode
                ):
                    raise SecureArtifactError(
                        "alias destination traverses a non-directory"
                    )
                try:
                    child_fd = os.open(
                        component,
                        _directory_open_flags(),
                        dir_fd=current_fd,
                    )
                except OSError as exc:
                    raise SecureArtifactError(
                        "alias destination directory cannot be opened safely"
                    ) from exc
                opened = os.fstat(child_fd)
                if not _same_object(before, opened) or not stat.S_ISDIR(
                    opened.st_mode
                ):
                    os.close(child_fd)
                    raise SecureArtifactError(
                        "alias destination directory changed during open"
                    )
                opened_directories.append(child_fd)
                current_fd = child_fd

            try:
                alias_before = os.stat(
                    alias_name,
                    dir_fd=current_fd,
                    follow_symlinks=False,
                )
            except FileNotFoundError:
                alias_before = None
                alias_was_absent = True
            except OSError as exc:
                raise SecureArtifactError(
                    "alias destination cannot be inspected"
                ) from exc

            if alias_before is not None:
                if (
                    stat.S_ISLNK(alias_before.st_mode)
                    or not stat.S_ISREG(alias_before.st_mode)
                    or alias_before.st_nlink != 1
                    or stat.S_IMODE(alias_before.st_mode)
                    not in canonical.allowed_modes
                ):
                    raise SecureArtifactError(
                        "existing alias is not one eligible regular file"
                    )
                if _same_object(alias_before, source_a):
                    raise SecureArtifactError(
                        "existing alias shares the canonical inode"
                    )
                try:
                    alias_fd = os.open(
                        alias_name,
                        _file_open_flags(),
                        dir_fd=current_fd,
                    )
                except OSError as exc:
                    raise SecureArtifactError(
                        "existing alias cannot be opened safely"
                    ) from exc
                alias_opened = os.fstat(alias_fd)
                if _stat_tuple(alias_opened) != _stat_tuple(alias_before):
                    raise SecureArtifactError(
                        "existing alias changed during open"
                    )
                alias_first = _read_exact_bytes(
                    alias_fd,
                    alias_opened.st_size,
                )
                alias_middle = os.fstat(alias_fd)
                alias_second = _read_exact_bytes(
                    alias_fd,
                    alias_opened.st_size,
                )
                alias_after = os.fstat(alias_fd)
                source_first = _read_exact_bytes(
                    source_fd,
                    source_a.st_size,
                )
                source_middle = os.fstat(source_fd)
                source_second = _read_exact_bytes(
                    source_fd,
                    source_a.st_size,
                )
                source_after = os.fstat(source_fd)
                alias_named_after = os.stat(
                    alias_name,
                    dir_fd=current_fd,
                    follow_symlinks=False,
                )
                if (
                    _stat_tuple(alias_before)
                    != _stat_tuple(alias_middle)
                    or _stat_tuple(alias_before)
                    != _stat_tuple(alias_after)
                    or _stat_tuple(alias_before)
                    != _stat_tuple(alias_named_after)
                    or _stat_tuple(source_a)
                    != _stat_tuple(source_middle)
                    or _stat_tuple(source_a)
                    != _stat_tuple(source_after)
                    or alias_first != alias_second
                    or source_first != source_second
                    or alias_second != source_second
                ):
                    raise SecureArtifactError(
                        "existing alias content or identity differs"
                    )
                _verify_reopened_source(
                    self._root_fd,
                    canonical,
                    source_a,
                )
                return

            for _attempt in range(128):
                candidate = "alias-" + secrets.token_hex(16) + ".tmp"
                try:
                    temporary_fd = os.open(
                        candidate,
                        _file_open_flags(write=True, create=True),
                        0o600,
                        dir_fd=current_fd,
                    )
                    temporary_name = candidate
                    break
                except FileExistsError:
                    continue
                except OSError as exc:
                    raise SecureArtifactError(
                        "alias temporary cannot be created exclusively"
                    ) from exc
            else:
                raise SecureArtifactError(
                    "alias temporary name allocation failed"
                )
            created_temporary = os.fstat(temporary_fd)
            temporary_identity = (
                created_temporary.st_dev,
                created_temporary.st_ino,
            )
            os.fchmod(temporary_fd, 0o600)
            source_digest = _copy_source_to_private(
                source_fd,
                temporary_fd,
                source_a,
                canonical,
            )
            temporary_info = os.fstat(temporary_fd)
            _validate_private_file_stat(
                temporary_info,
                size=source_a.st_size,
            )
            os.close(temporary_fd)
            temporary_fd = os.open(
                temporary_name,
                _file_open_flags(),
                dir_fd=current_fd,
            )
            reopened_temporary = os.fstat(temporary_fd)
            if (
                _stat_tuple(reopened_temporary)
                != _stat_tuple(temporary_info)
                or
                hashlib.sha256(
                    _read_exact_bytes(temporary_fd, temporary_info.st_size)
                ).hexdigest()
                != source_digest
            ):
                raise SecureArtifactError(
                    "alias temporary digest differs from canonical source"
                )
            _verify_reopened_source(
                self._root_fd,
                canonical,
                source_a,
            )
            os.close(temporary_fd)
            temporary_fd = -1
            _rename_noreplace(
                current_fd,
                temporary_name,
                alias_name,
            )
            os.fsync(current_fd)

            published = os.stat(
                alias_name,
                dir_fd=current_fd,
                follow_symlinks=False,
            )
            if (
                not stat.S_ISREG(published.st_mode)
                or published.st_nlink != 1
                or stat.S_IMODE(published.st_mode) != 0o600
                or (published.st_dev, published.st_ino)
                != temporary_identity
                or _same_object(published, source_a)
            ):
                raise SecureArtifactError(
                    "published alias identity is invalid"
                )
            alias_fd = os.open(
                alias_name,
                _file_open_flags(),
                dir_fd=current_fd,
            )
            published_opened = os.fstat(alias_fd)
            published_bytes = _read_exact_bytes(
                alias_fd,
                published.st_size,
            )
            published_after = os.fstat(alias_fd)
            if (
                _stat_tuple(published)
                != _stat_tuple(published_opened)
                or _stat_tuple(published)
                != _stat_tuple(published_after)
                or hashlib.sha256(published_bytes).hexdigest()
                != source_digest
            ):
                raise SecureArtifactError(
                    "published alias content verification failed"
                )
            _verify_reopened_source(
                self._root_fd,
                canonical,
                source_a,
            )
        except BaseException as exc:
            cleanup_errors: list[str] = []
            if temporary_fd >= 0:
                try:
                    os.close(temporary_fd)
                except OSError as close_exc:
                    cleanup_errors.append(
                        f"close alias temporary: {close_exc}"
                    )
                temporary_fd = -1
            destination_fd = (
                opened_directories[-1]
                if opened_directories
                else self._root_fd
            )
            if temporary_identity is not None and alias_was_absent:
                try:
                    published_during_failure = os.stat(
                        alias_name,
                        dir_fd=destination_fd,
                        follow_symlinks=False,
                    )
                except FileNotFoundError:
                    pass
                except OSError as inspect_exc:
                    cleanup_errors.append(
                        f"inspect published alias: {inspect_exc}"
                    )
                else:
                    if (
                        published_during_failure.st_dev,
                        published_during_failure.st_ino,
                    ) == temporary_identity:
                        try:
                            os.unlink(
                                alias_name,
                                dir_fd=destination_fd,
                            )
                            os.fsync(destination_fd)
                        except OSError as unlink_exc:
                            cleanup_errors.append(
                                f"unlink published alias: {unlink_exc}"
                            )
                    else:
                        cleanup_errors.append(
                            "published alias pathname was replaced"
                        )
            if temporary_name:
                try:
                    named = os.stat(
                        temporary_name,
                        dir_fd=destination_fd,
                        follow_symlinks=False,
                    )
                except FileNotFoundError:
                    pass
                except OSError as inspect_exc:
                    cleanup_errors.append(
                        f"inspect alias temporary: {inspect_exc}"
                    )
                else:
                    if (
                        temporary_identity is not None
                        and (named.st_dev, named.st_ino)
                        == temporary_identity
                    ):
                        try:
                            os.unlink(
                                temporary_name,
                                dir_fd=destination_fd,
                            )
                            os.fsync(destination_fd)
                        except OSError as unlink_exc:
                            cleanup_errors.append(
                                f"unlink alias temporary: {unlink_exc}"
                            )
                    else:
                        cleanup_errors.append(
                            "alias temporary pathname was replaced"
                        )
            for parent_fd, name, device, inode in reversed(
                created_directories
            ):
                try:
                    named = os.stat(
                        name,
                        dir_fd=parent_fd,
                        follow_symlinks=False,
                    )
                except FileNotFoundError:
                    continue
                except OSError as inspect_exc:
                    cleanup_errors.append(
                        f"inspect alias directory: {inspect_exc}"
                    )
                    continue
                if (
                    not stat.S_ISDIR(named.st_mode)
                    or named.st_dev != device
                    or named.st_ino != inode
                ):
                    cleanup_errors.append(
                        "alias destination directory was replaced"
                    )
                    continue
                try:
                    os.rmdir(name, dir_fd=parent_fd)
                    os.fsync(parent_fd)
                except OSError as remove_exc:
                    cleanup_errors.append(
                        f"remove alias directory: {remove_exc}"
                    )
            if cleanup_errors:
                raise SecureArtifactError(
                    "alias materialization cleanup failed: "
                    + "; ".join(cleanup_errors)
                ) from exc
            if isinstance(exc, SecureArtifactError):
                raise
            if isinstance(exc, Exception):
                raise SecureArtifactError(
                    "alias materialization failed"
                ) from exc
            raise
        finally:
            if temporary_fd >= 0:
                os.close(temporary_fd)
            if alias_fd >= 0:
                os.close(alias_fd)
            if source_fd >= 0:
                os.close(source_fd)
            for directory_fd in reversed(opened_directories):
                os.close(directory_fd)

    def prepare_snapshot(
        self,
        specs: Iterable[SnapshotFileSpec],
        *,
        generated_members: Iterable[GeneratedArtifactMember] = (),
        private_parent: os.PathLike[str] | str,
    ) -> VerifiedBadgeArtifactSnapshot:
        self._require_open()
        if isinstance(specs, (str, bytes)):
            raise SecureArtifactError(
                "artifact specification collection is malformed"
            )
        try:
            normalized_specs = tuple(specs)
        except TypeError as exc:
            raise SecureArtifactError(
                "artifact specification collection is malformed"
            ) from exc
        if (
            not normalized_specs
            or any(type(spec) is not SnapshotFileSpec for spec in normalized_specs)
        ):
            raise SecureArtifactError(
                "artifact specification collection is malformed"
            )
        ordered_specs = tuple(
            sorted(normalized_specs, key=lambda item: item.logical_name)
        )
        if isinstance(generated_members, (str, bytes)):
            raise SecureArtifactError(
                "generated artifact collection is malformed"
            )
        try:
            normalized_generated = tuple(generated_members)
        except TypeError as exc:
            raise SecureArtifactError(
                "generated artifact collection is malformed"
            ) from exc
        if any(
            type(member) is not GeneratedArtifactMember
            for member in normalized_generated
        ):
            raise SecureArtifactError(
                "generated artifact collection is malformed"
            )
        ordered_generated = tuple(
            sorted(
                normalized_generated,
                key=lambda item: item.logical_name,
            )
        )
        logical_names = tuple(spec.logical_name for spec in ordered_specs)
        generated_names = tuple(
            member.logical_name for member in ordered_generated
        )
        relative_names = tuple(spec.relative for spec in ordered_specs)
        if (
            len(set((*logical_names, *generated_names)))
            != len(logical_names) + len(generated_names)
            or len(set(relative_names)) != len(relative_names)
        ):
            raise SecureArtifactError(
                "artifact specifications must be unique"
            )

        private_parent_path = _lexical_absolute(
            private_parent,
            "private artifact parent",
        )
        parent_bindings = _open_directory_chain(private_parent_path)
        parent_fd = -1
        private_root_fd = -1
        source_root_fd = -1
        receipt_fd = -1
        root_binding: _DirectoryBinding | None = None
        root_name = ""
        created_root_key: tuple[int, int] | None = None
        retained: list[_RetainedMember] = []
        reserved_names: set[str] = set()
        known_identities: set[tuple[int, int]] = set()
        snapshot: VerifiedBadgeArtifactSnapshot | None = None
        try:
            parent_final = os.fstat(parent_bindings[-1].fd)
            if (
                not stat.S_ISDIR(parent_final.st_mode)
                or parent_final.st_uid != os.geteuid()
                or stat.S_IMODE(parent_final.st_mode) != 0o700
            ):
                raise SecureArtifactError(
                    "private artifact parent must be owner 0700"
                )
            parent_fd = _duplicate_cloexec(parent_bindings[-1].fd)
            _close_directory_chain(parent_bindings)
            parent_bindings = ()

            for _attempt in range(128):
                candidate = (
                    "fof-artifacts-" + secrets.token_hex(16)
                )
                try:
                    os.mkdir(candidate, 0o700, dir_fd=parent_fd)
                    root_name = candidate
                    break
                except FileExistsError:
                    continue
                except OSError as exc:
                    raise SecureArtifactError(
                        "private artifact root cannot be created"
                    ) from exc
            else:
                raise SecureArtifactError(
                    "private artifact root name allocation failed"
                )
            try:
                created_root = os.stat(
                    root_name,
                    dir_fd=parent_fd,
                    follow_symlinks=False,
                )
            except OSError as exc:
                try:
                    os.rmdir(root_name, dir_fd=parent_fd)
                except OSError:
                    pass
                raise SecureArtifactError(
                    "private artifact root cannot be inspected"
                ) from exc
            if (
                not stat.S_ISDIR(created_root.st_mode)
                or created_root.st_uid != os.geteuid()
            ):
                raise SecureArtifactError(
                    "private artifact root creation was rebound"
                )
            created_root_key = (
                created_root.st_dev,
                created_root.st_ino,
            )
            private_root_fd = os.open(
                root_name,
                _directory_open_flags(),
                dir_fd=parent_fd,
            )
            opened_root = os.fstat(private_root_fd)
            if (
                not stat.S_ISDIR(opened_root.st_mode)
                or (
                    opened_root.st_dev,
                    opened_root.st_ino,
                )
                != created_root_key
            ):
                raise SecureArtifactError(
                    "private artifact root changed during open"
                )
            root_binding = _directory_binding(
                root_name,
                private_root_fd,
                opened_root,
            )
            try:
                os.fchmod(private_root_fd, 0o700)
            except BaseException:
                try:
                    changed_root = os.fstat(private_root_fd)
                    if (
                        stat.S_ISDIR(changed_root.st_mode)
                        and (
                            changed_root.st_dev,
                            changed_root.st_ino,
                        )
                        == created_root_key
                    ):
                        root_binding = _directory_binding(
                            root_name,
                            private_root_fd,
                            changed_root,
                        )
                except OSError:
                    pass
                raise
            root_info = os.fstat(private_root_fd)
            if (
                not stat.S_ISDIR(root_info.st_mode)
                or stat.S_IMODE(root_info.st_mode) != 0o700
                or root_info.st_uid != os.geteuid()
            ):
                raise SecureArtifactError(
                    "private artifact root is not owner 0700"
                )
            root_binding = _directory_binding(
                root_name,
                private_root_fd,
                root_info,
            )
            os.fsync(parent_fd)

            verified_files: list[VerifiedSnapshotFile] = []
            source_inodes: set[tuple[int, int]] = set()
            total_size = 0
            for index, spec in enumerate(ordered_specs):
                source_fd = -1
                destination_fd = -1
                try:
                    source_fd, source_a = _open_relative_source(
                        self._root_fd,
                        spec,
                    )
                    source_key = (source_a.st_dev, source_a.st_ino)
                    if source_key in source_inodes:
                        raise SecureArtifactError(
                            "distinct artifact paths share one source inode"
                        )
                    source_inodes.add(source_key)
                    total_size += source_a.st_size
                    if total_size > MAX_ARTIFACT_SET_BYTES:
                        raise SecureArtifactError(
                            "artifact snapshot exceeds aggregate size bound"
                        )
                    private_name = (
                        f"{index:03d}-"
                        f"{secrets.token_hex(16)}"
                        ".artifact"
                    )
                    reserved_names.add(private_name)
                    destination_fd = os.open(
                        private_name,
                        _file_open_flags(write=True, create=True),
                        0o600,
                        dir_fd=private_root_fd,
                    )
                    created_private = os.fstat(destination_fd)
                    known_identities.add(
                        (created_private.st_dev, created_private.st_ino)
                    )
                    os.fchmod(destination_fd, 0o600)
                    source_digest = _copy_source_to_private(
                        source_fd,
                        destination_fd,
                        source_a,
                        spec,
                    )
                    private_info = os.fstat(destination_fd)
                    _validate_private_file_stat(
                        private_info,
                        size=source_a.st_size,
                    )
                    _verify_reopened_source(
                        self._root_fd,
                        spec,
                        source_a,
                    )
                    source_identity = _regular_identity(
                        spec.relative,
                        source_a,
                        source_digest,
                    )
                    private_identity = _regular_identity(
                        private_name,
                        private_info,
                        source_digest,
                    )
                    verified = VerifiedSnapshotFile(
                        logical_name=spec.logical_name,
                        private_relative=private_name,
                        source=source_identity,
                        private=private_identity,
                    )
                    verified_files.append(verified)
                    known_identities.add(
                        (private_info.st_dev, private_info.st_ino)
                    )
                finally:
                    if destination_fd >= 0:
                        os.close(destination_fd)
                    if source_fd >= 0:
                        os.close(source_fd)

                retained_fd = os.open(
                    private_name,
                    _file_open_flags(),
                    dir_fd=private_root_fd,
                )
                retained_member = _RetainedMember(
                    verified=verified_files[-1],
                    spec=spec,
                    fd=retained_fd,
                )
                try:
                    opened_private = _validate_retained_named_file(
                        private_root_fd,
                        retained_member,
                    )
                    retained_bytes = _read_exact_bytes(
                        retained_fd,
                        opened_private.st_size,
                    )
                    retained_after = os.fstat(retained_fd)
                    if (
                        _stat_tuple(opened_private)
                        != _stat_tuple(retained_after)
                        or hashlib.sha256(retained_bytes).hexdigest()
                        != retained_member.verified.private.sha256
                    ):
                        raise SecureArtifactError(
                            "private artifact retained verification failed"
                        )
                except BaseException:
                    os.close(retained_fd)
                    raise
                retained.append(retained_member)
                os.fsync(private_root_fd)

            for generated_index, generated in enumerate(
                ordered_generated,
                start=len(ordered_specs),
            ):
                total_size += generated.size
                if total_size > MAX_ARTIFACT_SET_BYTES:
                    raise SecureArtifactError(
                        "artifact snapshot exceeds aggregate size bound"
                    )
                private_name = (
                    f"{generated_index:03d}-"
                    f"{secrets.token_hex(16)}"
                    ".artifact"
                )
                reserved_names.add(private_name)
                destination_fd = -1
                try:
                    destination_fd = os.open(
                        private_name,
                        _file_open_flags(write=True, create=True),
                        0o600,
                        dir_fd=private_root_fd,
                    )
                    created_private = os.fstat(destination_fd)
                    known_identities.add(
                        (created_private.st_dev, created_private.st_ino)
                    )
                    os.fchmod(destination_fd, 0o600)
                    _write_all(destination_fd, generated.content)
                    os.fsync(destination_fd)
                    private_info = os.fstat(destination_fd)
                    _validate_private_file_stat(
                        private_info,
                        size=generated.size,
                    )
                    private_identity = _regular_identity(
                        private_name,
                        private_info,
                        generated.sha256,
                    )
                    generated_source_identity = _regular_identity(
                        f"generated/{generated.logical_name}",
                        private_info,
                        generated.sha256,
                    )
                    verified = VerifiedSnapshotFile(
                        logical_name=generated.logical_name,
                        private_relative=private_name,
                        source=generated_source_identity,
                        private=private_identity,
                    )
                    verified_files.append(verified)
                    known_identities.add(
                        (private_info.st_dev, private_info.st_ino)
                    )
                finally:
                    if destination_fd >= 0:
                        os.close(destination_fd)

                retained_fd = os.open(
                    private_name,
                    _file_open_flags(),
                    dir_fd=private_root_fd,
                )
                retained_member = _RetainedMember(
                    verified=verified,
                    spec=None,
                    fd=retained_fd,
                )
                try:
                    opened_private = _validate_retained_named_file(
                        private_root_fd,
                        retained_member,
                    )
                    retained_bytes = _read_exact_bytes(
                        retained_fd,
                        opened_private.st_size,
                    )
                    retained_after = os.fstat(retained_fd)
                    if (
                        _stat_tuple(opened_private)
                        != _stat_tuple(retained_after)
                        or retained_bytes != generated.content
                        or hashlib.sha256(retained_bytes).hexdigest()
                        != generated.sha256
                    ):
                        raise SecureArtifactError(
                            "generated private artifact verification failed"
                        )
                except BaseException:
                    os.close(retained_fd)
                    raise
                retained.append(retained_member)
                os.fsync(private_root_fd)

            files = tuple(
                sorted(
                    verified_files,
                    key=lambda item: item.logical_name,
                )
            )
            receipt_bytes = _canonical_receipt(files)
            receipt_digest = hashlib.sha256(receipt_bytes).hexdigest()
            receipt_temp = "receipt-" + secrets.token_hex(16) + ".tmp"
            reserved_names.update((receipt_temp, "receipt.json"))
            receipt_writer = os.open(
                receipt_temp,
                _file_open_flags(write=True, create=True),
                0o600,
                dir_fd=private_root_fd,
            )
            try:
                created_receipt = os.fstat(receipt_writer)
                known_identities.add(
                    (created_receipt.st_dev, created_receipt.st_ino)
                )
                os.fchmod(receipt_writer, 0o600)
                _write_all(receipt_writer, receipt_bytes)
                os.fsync(receipt_writer)
                receipt_temp_info = os.fstat(receipt_writer)
                _validate_private_file_stat(
                    receipt_temp_info,
                    size=len(receipt_bytes),
                )
                known_identities.add(
                    (
                        receipt_temp_info.st_dev,
                        receipt_temp_info.st_ino,
                    )
                )
            finally:
                os.close(receipt_writer)
            _rename_noreplace(
                private_root_fd,
                receipt_temp,
                "receipt.json",
            )
            reserved_names.discard(receipt_temp)
            os.fsync(private_root_fd)
            receipt_fd = os.open(
                "receipt.json",
                _file_open_flags(),
                dir_fd=private_root_fd,
            )
            receipt_info = os.fstat(receipt_fd)
            _validate_private_file_stat(
                receipt_info,
                size=len(receipt_bytes),
            )
            if not _same_object(receipt_temp_info, receipt_info):
                raise SecureArtifactError(
                    "published receipt is not the fsynced receipt inode"
                )
            if (
                _read_exact_bytes(receipt_fd, len(receipt_bytes))
                != receipt_bytes
            ):
                raise SecureArtifactError(
                    "private receipt readback changed"
                )
            receipt_after = os.fstat(receipt_fd)
            if _stat_tuple(receipt_info) != _stat_tuple(receipt_after):
                raise SecureArtifactError(
                    "private receipt changed during readback"
                )
            receipt_identity = _regular_identity(
                "receipt.json",
                receipt_info,
                receipt_digest,
            )
            known_identities.add(
                (receipt_info.st_dev, receipt_info.st_ino)
            )

            source_root_info = os.fstat(self._root_fd)
            source_root_fd = _duplicate_cloexec(self._root_fd)
            snapshot = VerifiedBadgeArtifactSnapshot(
                token=_SNAPSHOT_TOKEN,
                private_root=private_parent_path / root_name,
                files=files,
                receipt_sha256=receipt_digest,
                source_root_path=self.root,
                source_root_fd=source_root_fd,
                source_root_device=source_root_info.st_dev,
                source_root_inode=source_root_info.st_ino,
                private_parent_fd=parent_fd,
                private_root_fd=private_root_fd,
                private_root_binding=root_binding,
                private_root_name=root_name,
                receipt_fd=receipt_fd,
                receipt_identity=receipt_identity,
                receipt_bytes=receipt_bytes,
                retained=retained,
                reserved_names=reserved_names,
            )
            source_root_fd = -1
            parent_fd = -1
            private_root_fd = -1
            receipt_fd = -1
            retained = []
            _test_hook("snapshot_before_return", None)
            snapshot.revalidate_sources()
            snapshot.revalidate_retained_files()
            return snapshot
        except BaseException as exc:
            cleanup_errors: list[str] = []
            if snapshot is not None:
                try:
                    snapshot.close()
                except BaseException as cleanup_exc:
                    cleanup_errors.append(str(cleanup_exc))
            else:
                for member in retained:
                    if member.fd >= 0:
                        try:
                            os.close(member.fd)
                        except OSError as close_exc:
                            cleanup_errors.append(str(close_exc))
                if receipt_fd >= 0:
                    try:
                        os.close(receipt_fd)
                    except OSError as close_exc:
                        cleanup_errors.append(str(close_exc))
                if source_root_fd >= 0:
                    try:
                        os.close(source_root_fd)
                    except OSError as close_exc:
                        cleanup_errors.append(str(close_exc))
                if (
                    private_root_fd >= 0
                    and parent_fd >= 0
                    and root_binding is not None
                ):
                    cleanup_errors.extend(_remove_private_root(
                        parent_fd=parent_fd,
                        root_fd=private_root_fd,
                        root_binding=root_binding,
                        reserved_names=reserved_names,
                        known_identities=known_identities,
                    ))
                    private_root_fd = -1
                elif private_root_fd >= 0:
                    try:
                        os.close(private_root_fd)
                    except OSError as close_exc:
                        cleanup_errors.append(str(close_exc))
                    private_root_fd = -1
                if (
                    parent_fd >= 0
                    and root_name
                    and created_root_key is not None
                ):
                    try:
                        remaining_root = os.stat(
                            root_name,
                            dir_fd=parent_fd,
                            follow_symlinks=False,
                        )
                    except FileNotFoundError:
                        pass
                    except OSError as inspect_exc:
                        cleanup_errors.append(str(inspect_exc))
                    else:
                        if (
                            stat.S_ISDIR(remaining_root.st_mode)
                            and (
                                remaining_root.st_dev,
                                remaining_root.st_ino,
                            )
                            == created_root_key
                        ):
                            try:
                                os.rmdir(root_name, dir_fd=parent_fd)
                                os.fsync(parent_fd)
                            except OSError as remove_exc:
                                cleanup_errors.append(str(remove_exc))
                if parent_fd >= 0:
                    try:
                        os.close(parent_fd)
                    except OSError as close_exc:
                        cleanup_errors.append(str(close_exc))
            if cleanup_errors:
                add_note = getattr(exc, "add_note", None)
                if callable(add_note):
                    add_note(
                        "private snapshot cleanup also failed: "
                        + "; ".join(cleanup_errors)
                    )
            if isinstance(exc, SecureArtifactError):
                raise
            if isinstance(exc, Exception):
                raise SecureArtifactError(
                    "artifact snapshot preparation failed"
                ) from exc
            raise
        finally:
            if parent_bindings:
                _close_directory_chain(parent_bindings)

    def close(self) -> None:
        if self._closed:
            return
        _close_directory_chain(self._bindings)
        self._bindings = ()
        self._closed = True

    def __enter__(self) -> "SecureArtifactTree":
        self._require_open()
        return self

    def __exit__(
        self,
        _exc_type: object,
        _exc: object,
        _traceback: object,
    ) -> None:
        self.close()


__all__ = (
    "DEFAULT_ARTIFACT_MEMBER_BYTES",
    "DescriptorRootedFreezeAuthority",
    "FrozenArtifactMember",
    "FrozenArtifactSet",
    "FrozenBytesView",
    "GeneratedArtifactMember",
    "MAX_ARTIFACT_MEMBER_BYTES",
    "MAX_ARTIFACT_SET_BYTES",
    "RegularFileIdentity",
    "SecureArtifactError",
    "SecureArtifactTree",
    "SnapshotFileSpec",
    "VerifiedBadgeArtifactSnapshot",
    "VerifiedSnapshotFile",
    "run_private_partition_generator",
)
