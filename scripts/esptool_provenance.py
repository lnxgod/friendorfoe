"""Fail-closed provenance boundary for the PlatformIO esptool runtime.

This module deliberately does not import esptool at module import time.  The
only supported production entry point resolves PlatformIO's exact pinned tool
package, freezes its reviewed members into memory, and imports those bytes
through a private meta-path loader.
"""

from __future__ import annotations

import base64
import configparser
from dataclasses import dataclass
import hashlib
import hmac
import importlib
import importlib.abc
import importlib.machinery
import importlib.util
import inspect
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys
import sysconfig
import threading
from types import MappingProxyType, ModuleType
from typing import ClassVar, Mapping


EXPECTED_ESPTOOL_LOCK_SHA256 = (
    "27d0245728aff3b9ce28fba7f9e027cb9481bb0d02510db6362389d758f40291"
)
_PACKAGE_NAME = "tool-esptoolpy"
_PACKAGE_VERSION = "2.41100.0"
_ESPTOOL_VERSION = "4.11.0"
_LOCK_NAME = "esptoolpy-2.41100.0.lock.json"
_HEX_SHA256 = re.compile(r"[0-9a-f]{64}\Z")
_EXECUTION_LOCK = threading.Lock()
_AUDIT_SETUP_LOCK = threading.Lock()
_AUDIT_ROOTS: set[str] = set()
_AUDIT_HOOK_INSTALLED = False
_MAX_EXTERNAL_CONFIG_BYTES = 1024 * 1024

_EXPECTED_LOADER_CONSTANTS = MappingProxyType(
    {
        "DEFAULT_TIMEOUT": 3,
        "CHIP_ERASE_TIMEOUT": 120,
        "MAX_TIMEOUT": 240,
        "SYNC_TIMEOUT": 0.1,
        "MD5_TIMEOUT_PER_MB": 8,
        "ERASE_REGION_TIMEOUT_PER_MB": 30,
        "ERASE_WRITE_TIMEOUT_PER_MB": 40,
        "MEM_END_ROM_TIMEOUT": 0.2,
        "DEFAULT_SERIAL_WRITE_TIMEOUT": 10,
        "DEFAULT_CONNECT_ATTEMPTS": 7,
        "DEFAULT_OPEN_PORT_ATTEMPTS": 1,
    }
)
_EXPECTED_CMDS_CONSTANTS = MappingProxyType(
    {
        "DEFAULT_TIMEOUT": 3,
        "ERASE_WRITE_TIMEOUT_PER_MB": 40,
        "DEFAULT_CONNECT_ATTEMPTS": 7,
    }
)


class EsptoolProvenanceError(RuntimeError):
    """The pinned esptool provenance contract was not satisfied."""


@dataclass(frozen=True, slots=True)
class ReviewedMember:
    kind: str
    mode: int
    path: str
    sha256: str
    size: int


@dataclass(frozen=True, slots=True)
class ReviewedLock:
    package_name: str
    package_version: str
    members: Mapping[str, ReviewedMember]


@dataclass(frozen=True, slots=True)
class FrozenPackage:
    package_root: Path
    members: Mapping[str, bytes]


@dataclass(frozen=True, slots=True)
class FrozenStubRecord:
    text: bytes
    text_start: int
    entry: int
    data: bytes | None
    data_start: int | None
    bss_start: int | None


def _duplicate_rejecting_object(
    pairs: list[tuple[str, object]],
) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise EsptoolProvenanceError(f"duplicate JSON key: {key!r}")
        result[key] = value
    return result


def _reject_json_constant(value: str) -> object:
    raise EsptoolProvenanceError(f"non-finite JSON number: {value}")


def _parse_json_value(raw: bytes, label: str) -> object:
    try:
        text = raw.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise EsptoolProvenanceError(f"{label} is not UTF-8") from exc
    decoder = json.JSONDecoder(
        object_pairs_hook=_duplicate_rejecting_object,
        parse_constant=_reject_json_constant,
    )
    try:
        value, end = decoder.raw_decode(text)
    except (json.JSONDecodeError, EsptoolProvenanceError) as exc:
        if isinstance(exc, EsptoolProvenanceError):
            raise
        raise EsptoolProvenanceError(f"invalid {label} JSON") from exc
    if text[end:].strip():
        raise EsptoolProvenanceError(f"trailing data after {label} JSON")
    return value


def _require_exact_keys(
    value: Mapping[str, object],
    expected: set[str],
    label: str,
) -> None:
    actual = set(value)
    if actual != expected:
        raise EsptoolProvenanceError(
            f"{label} keys differ: expected {sorted(expected)!r}, "
            f"got {sorted(actual)!r}"
        )


def _safe_member_path(value: object) -> str:
    if not isinstance(value, str) or not value:
        raise EsptoolProvenanceError("lock member path must be a string")
    if "\\" in value or value.startswith("/"):
        raise EsptoolProvenanceError(f"unsafe lock member path: {value!r}")
    path = PurePosixPath(value)
    if (
        path.as_posix() != value
        or any(part in ("", ".", "..") for part in path.parts)
        or not path.parts
        or path.parts[0] != "esptool"
    ):
        raise EsptoolProvenanceError(f"unsafe lock member path: {value!r}")
    return value


def parse_reviewed_lock(raw: bytes, expected_sha256: str) -> ReviewedLock:
    """Parse a byte-for-byte canonical reviewed lock."""

    if not isinstance(raw, bytes):
        raise EsptoolProvenanceError("reviewed lock must be immutable bytes")
    if not _HEX_SHA256.fullmatch(expected_sha256):
        raise EsptoolProvenanceError("reviewed lock pin is not lowercase SHA-256")
    actual_digest = hashlib.sha256(raw).hexdigest()
    if not hmac.compare_digest(actual_digest, expected_sha256):
        raise EsptoolProvenanceError("reviewed lock SHA-256 mismatch")

    parsed = _parse_json_value(raw, "reviewed lock")
    if not isinstance(parsed, dict):
        raise EsptoolProvenanceError("reviewed lock must be a JSON object")
    _require_exact_keys(
        parsed,
        {"members", "package_name", "package_version", "schema_version"},
        "reviewed lock",
    )
    if parsed["package_name"] != _PACKAGE_NAME:
        raise EsptoolProvenanceError("unexpected PlatformIO package name")
    if parsed["package_version"] != _PACKAGE_VERSION:
        raise EsptoolProvenanceError("unexpected PlatformIO package version")
    if type(parsed["schema_version"]) is not int or parsed["schema_version"] != 1:
        raise EsptoolProvenanceError("unsupported reviewed lock schema")
    raw_members = parsed["members"]
    if not isinstance(raw_members, list):
        raise EsptoolProvenanceError("reviewed lock members must be a list")

    members: dict[str, ReviewedMember] = {}
    prior_path: str | None = None
    for index, raw_member in enumerate(raw_members):
        if not isinstance(raw_member, dict):
            raise EsptoolProvenanceError(f"lock member {index} is not an object")
        _require_exact_keys(
            raw_member,
            {"kind", "mode", "path", "sha256", "size"},
            f"lock member {index}",
        )
        path = _safe_member_path(raw_member["path"])
        if prior_path is not None and path <= prior_path:
            raise EsptoolProvenanceError(
                "reviewed lock members must have unique sorted paths"
            )
        prior_path = path
        kind = raw_member["kind"]
        if kind not in ("python", "runtime_data"):
            raise EsptoolProvenanceError(f"invalid member kind for {path!r}")
        if kind == "python":
            if not path.endswith(".py"):
                raise EsptoolProvenanceError(
                    f"Python member is not source: {path!r}"
                )
        elif (
            not path.startswith("esptool/targets/stub_flasher/")
            or path.endswith((".py", ".pyc", ".pyo"))
        ):
            raise EsptoolProvenanceError(
                f"runtime member is outside locked stub data: {path!r}"
            )

        mode = raw_member["mode"]
        if (
            type(mode) is not int
            or mode < 0
            or mode > 0o777
            or not (mode & stat.S_IRUSR)
            or mode & (stat.S_IWGRP | stat.S_IWOTH)
        ):
            raise EsptoolProvenanceError(f"unsafe member mode for {path!r}")
        size = raw_member["size"]
        if type(size) is not int or size < 0:
            raise EsptoolProvenanceError(f"invalid member size for {path!r}")
        digest = raw_member["sha256"]
        if not isinstance(digest, str) or not _HEX_SHA256.fullmatch(digest):
            raise EsptoolProvenanceError(f"invalid member SHA-256 for {path!r}")
        members[path] = ReviewedMember(
            kind=kind,
            mode=mode,
            path=path,
            sha256=digest,
            size=size,
        )

    canonical = (
        json.dumps(
            parsed,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=True,
        ).encode("ascii")
        + b"\n"
    )
    if canonical != raw:
        raise EsptoolProvenanceError("reviewed lock is not canonical JSON")
    return ReviewedLock(
        package_name=_PACKAGE_NAME,
        package_version=_PACKAGE_VERSION,
        members=MappingProxyType(members),
    )


def _bounded_address(value: object, field: str) -> int:
    if type(value) is not int or value < 0 or value > 0xFFFF_FFFF:
        raise EsptoolProvenanceError(f"invalid frozen stub {field}")
    return value


def _strict_base64(value: object, field: str) -> bytes:
    if not isinstance(value, str):
        raise EsptoolProvenanceError(f"frozen stub {field} is not base64 text")
    try:
        encoded = value.encode("ascii", errors="strict")
        return bytes(base64.b64decode(encoded, validate=True))
    except (UnicodeEncodeError, ValueError) as exc:
        raise EsptoolProvenanceError(
            f"invalid frozen stub {field} base64"
        ) from exc


def parse_frozen_stub_record(raw: bytes) -> FrozenStubRecord:
    """Strictly parse one already-frozen esptool stub record."""

    if not isinstance(raw, bytes):
        raise EsptoolProvenanceError("frozen stub input must be immutable bytes")
    parsed = _parse_json_value(raw, "frozen stub")
    if not isinstance(parsed, dict):
        raise EsptoolProvenanceError("frozen stub must be a JSON object")
    required = {"entry", "text", "text_start"}
    optional = {"data", "data_start", "bss_start"}
    keys = set(parsed)
    if not required.issubset(keys) or not keys.issubset(required | optional):
        raise EsptoolProvenanceError("frozen stub fields differ from schema")
    has_data = "data" in parsed
    has_data_start = "data_start" in parsed
    if has_data != has_data_start:
        raise EsptoolProvenanceError(
            "frozen stub data and data_start must appear together"
        )

    data = _strict_base64(parsed["data"], "data") if has_data else None
    data_start = (
        _bounded_address(parsed["data_start"], "data_start")
        if has_data_start
        else None
    )
    bss_start = (
        _bounded_address(parsed["bss_start"], "bss_start")
        if "bss_start" in parsed
        else None
    )
    return FrozenStubRecord(
        text=_strict_base64(parsed["text"], "text"),
        text_start=_bounded_address(parsed["text_start"], "text_start"),
        entry=_bounded_address(parsed["entry"], "entry"),
        data=data,
        data_start=data_start,
        bss_start=bss_start,
    )


class FrozenStubFlasher:
    """Stub data adapter with no pathname or filesystem fallback."""

    STUB_SUBDIRS: ClassVar[list[str]] = ["1", "2"]
    _records: ClassVar[
        Mapping[tuple[str, str], FrozenStubRecord]
    ] = MappingProxyType({})

    def __init__(self, target: object) -> None:
        try:
            stub_class = getattr(target, "STUB_CLASS")
            json_name = stub_class.stub_json_name(target)
        except Exception as exc:
            raise EsptoolProvenanceError(
                "target cannot identify a frozen stub"
            ) from exc
        if (
            not isinstance(json_name, str)
            or PurePosixPath(json_name).name != json_name
            or not json_name.endswith(".json")
        ):
            raise EsptoolProvenanceError("target requested an unsafe stub name")
        selected: FrozenStubRecord | None = None
        for subdir in self.STUB_SUBDIRS:
            selected = self._records.get((subdir, json_name))
            if selected is not None:
                break
        if selected is None:
            raise EsptoolProvenanceError(
                f"no reviewed frozen stub for {json_name!r}"
            )
        self.text = bytes(selected.text)
        self.text_start = selected.text_start
        self.entry = selected.entry
        self.data = (
            bytes(selected.data) if selected.data is not None else None
        )
        self.data_start = selected.data_start
        self.bss_start = selected.bss_start

    @classmethod
    def set_preferred_stub_subdir(cls, subdir: str) -> None:
        if subdir not in ("1", "2"):
            raise EsptoolProvenanceError(
                f"unreviewed stub subdirectory: {subdir!r}"
            )
        cls.STUB_SUBDIRS = [subdir, "1" if subdir == "2" else "2"]

    @classmethod
    def _install_records(
        cls,
        records: Mapping[tuple[str, str], FrozenStubRecord],
    ) -> None:
        expected = {("1", "esp32s3.json"), ("2", "esp32s3.json")}
        if set(records) != expected:
            raise EsptoolProvenanceError(
                "both reviewed ESP32-S3 stub versions are required"
            )
        cls._records = MappingProxyType(dict(records))
        cls.STUB_SUBDIRS = ["1", "2"]


@dataclass(frozen=True, slots=True)
class _FileIdentity:
    device: int
    inode: int
    mode: int
    links: int
    size: int
    mtime_ns: int
    ctime_ns: int


def _identity(info: os.stat_result) -> _FileIdentity:
    return _FileIdentity(
        device=info.st_dev,
        inode=info.st_ino,
        mode=info.st_mode,
        links=info.st_nlink,
        size=info.st_size,
        mtime_ns=info.st_mtime_ns,
        ctime_ns=info.st_ctime_ns,
    )


def _required_flag(name: str) -> int:
    value = getattr(os, name, None)
    if not isinstance(value, int):
        raise EsptoolProvenanceError(f"host lacks required {name} support")
    return value


def _directory_flags() -> int:
    return (
        os.O_RDONLY
        | _required_flag("O_DIRECTORY")
        | _required_flag("O_NOFOLLOW")
        | _required_flag("O_CLOEXEC")
    )


def _file_flags() -> int:
    return (
        os.O_RDONLY
        | _required_flag("O_NOFOLLOW")
        | _required_flag("O_CLOEXEC")
    )


def _read_all(fd: int) -> bytes:
    chunks: list[bytes] = []
    while True:
        chunk = os.read(fd, 131072)
        if not chunk:
            return b"".join(chunks)
        chunks.append(chunk)


def _read_regular_at(parent_fd: int, name: str, label: str) -> bytes:
    try:
        before_path = os.stat(
            name, dir_fd=parent_fd, follow_symlinks=False
        )
    except OSError as exc:
        raise EsptoolProvenanceError(f"cannot stat {label}") from exc
    if not stat.S_ISREG(before_path.st_mode) or before_path.st_nlink != 1:
        raise EsptoolProvenanceError(
            f"{label} is not a single-link regular file"
        )
    try:
        fd = os.open(name, _file_flags(), dir_fd=parent_fd)
    except OSError as exc:
        raise EsptoolProvenanceError(f"cannot no-follow open {label}") from exc
    try:
        before_fd = os.fstat(fd)
        if _identity(before_fd) != _identity(before_path):
            raise EsptoolProvenanceError(f"{label} identity changed at open")
        first = _read_all(fd)
        middle_fd = os.fstat(fd)
        os.lseek(fd, 0, os.SEEK_SET)
        second = _read_all(fd)
        after_fd = os.fstat(fd)
        try:
            after_path = os.stat(
                name, dir_fd=parent_fd, follow_symlinks=False
            )
        except OSError as exc:
            raise EsptoolProvenanceError(
                f"cannot re-stat {label}"
            ) from exc
        identities = {
            _identity(before_path),
            _identity(before_fd),
            _identity(middle_fd),
            _identity(after_fd),
            _identity(after_path),
        }
        if len(identities) != 1 or first != second:
            raise EsptoolProvenanceError(
                f"{label} changed during double-read"
            )
        return bytes(first)
    finally:
        os.close(fd)


@dataclass(slots=True)
class _DirectoryCapability:
    relative: str
    name: str
    parent_relative: str | None
    fd: int
    identity: _FileIdentity


class _RetainedPackageTree:
    def __init__(self, package_root: Path) -> None:
        if not package_root.is_absolute() or package_root.name in ("", ".", ".."):
            raise EsptoolProvenanceError(
                "PlatformIO package root must be an absolute child path"
            )
        self.package_root = package_root
        self.parent_fd = -1
        self.directories: dict[str, _DirectoryCapability] = {}
        parent = package_root.parent
        try:
            self.parent_fd = os.open(str(parent), _directory_flags())
            before = os.stat(
                package_root.name,
                dir_fd=self.parent_fd,
                follow_symlinks=False,
            )
            if not stat.S_ISDIR(before.st_mode):
                raise EsptoolProvenanceError(
                    "PlatformIO package root is not a directory"
                )
            root_fd = os.open(
                package_root.name,
                _directory_flags(),
                dir_fd=self.parent_fd,
            )
            opened = os.fstat(root_fd)
            if _identity(before) != _identity(opened):
                os.close(root_fd)
                raise EsptoolProvenanceError(
                    "PlatformIO package root changed at open"
                )
            self.directories[""] = _DirectoryCapability(
                relative="",
                name=package_root.name,
                parent_relative=None,
                fd=root_fd,
                identity=_identity(opened),
            )
        except BaseException:
            self.close()
            raise

    def open_child_directory(self, parent_relative: str, name: str) -> str:
        parent = self.directories[parent_relative]
        relative = f"{parent_relative}/{name}".lstrip("/")
        try:
            before = os.stat(
                name, dir_fd=parent.fd, follow_symlinks=False
            )
            if not stat.S_ISDIR(before.st_mode):
                raise EsptoolProvenanceError(
                    f"package member {relative!r} is not a directory"
                )
            fd = os.open(name, _directory_flags(), dir_fd=parent.fd)
            opened = os.fstat(fd)
            if _identity(before) != _identity(opened):
                os.close(fd)
                raise EsptoolProvenanceError(
                    f"package directory {relative!r} changed at open"
                )
        except EsptoolProvenanceError:
            raise
        except OSError as exc:
            raise EsptoolProvenanceError(
                f"cannot retain package directory {relative!r}"
            ) from exc
        self.directories[relative] = _DirectoryCapability(
            relative=relative,
            name=name,
            parent_relative=parent_relative,
            fd=fd,
            identity=_identity(opened),
        )
        return relative

    def revalidate(self) -> None:
        for relative, capability in self.directories.items():
            if relative == "":
                parent_fd = self.parent_fd
            else:
                assert capability.parent_relative is not None
                parent_fd = self.directories[
                    capability.parent_relative
                ].fd
            try:
                named = os.stat(
                    capability.name,
                    dir_fd=parent_fd,
                    follow_symlinks=False,
                )
                opened = os.fstat(capability.fd)
            except OSError as exc:
                raise EsptoolProvenanceError(
                    f"package directory {relative or '<root>'!r} vanished"
                ) from exc
            if (
                _identity(named) != capability.identity
                or _identity(opened) != capability.identity
            ):
                raise EsptoolProvenanceError(
                    f"package directory {relative or '<root>'!r} changed"
                )

    def close(self) -> None:
        for relative in sorted(
            self.directories, key=lambda value: value.count("/"), reverse=True
        ):
            try:
                os.close(self.directories[relative].fd)
            except OSError:
                pass
        self.directories.clear()
        if self.parent_fd >= 0:
            try:
                os.close(self.parent_fd)
            except OSError:
                pass
            self.parent_fd = -1

    def __enter__(self) -> "_RetainedPackageTree":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


def _is_allowed_bytecode(relative: str, lock: ReviewedLock) -> bool:
    path = PurePosixPath(relative)
    if path.parent.name != "__pycache__" or path.suffix != ".pyc":
        return False
    stem = path.name.split(".", 1)[0]
    if not stem:
        return False
    source = path.parent.parent / f"{stem}.py"
    record = lock.members.get(source.as_posix())
    return record is not None and record.kind == "python"


def _load_package_json(root_fd: int) -> None:
    raw = _read_regular_at(root_fd, "package.json", "PlatformIO package.json")
    parsed = _parse_json_value(raw, "PlatformIO package.json")
    if not isinstance(parsed, dict):
        raise EsptoolProvenanceError("PlatformIO package.json is not an object")
    if parsed.get("name") != _PACKAGE_NAME:
        raise EsptoolProvenanceError("PlatformIO package.json name mismatch")
    if parsed.get("version") != _PACKAGE_VERSION:
        raise EsptoolProvenanceError("PlatformIO package.json version mismatch")


def freeze_verified_package(
    package_root: Path, lock: ReviewedLock
) -> FrozenPackage:
    """Freeze every reviewed esptool source/data member from retained FDs."""

    root = Path(package_root)
    expected_directories: set[str] = {"esptool"}
    for path in lock.members:
        parent = PurePosixPath(path).parent
        while parent.as_posix() not in (".", ""):
            expected_directories.add(parent.as_posix())
            parent = parent.parent

    with _RetainedPackageTree(root) as tree:
        root_fd = tree.directories[""].fd
        _load_package_json(root_fd)
        tree.open_child_directory("", "esptool")
        pending = ["esptool"]
        inventory: set[str] = set()
        while pending:
            relative = pending.pop()
            capability = tree.directories[relative]
            try:
                names = sorted(os.listdir(capability.fd))
            except OSError as exc:
                raise EsptoolProvenanceError(
                    f"cannot inventory package directory {relative!r}"
                ) from exc
            for name in names:
                child = f"{relative}/{name}"
                try:
                    info = os.stat(
                        name,
                        dir_fd=capability.fd,
                        follow_symlinks=False,
                    )
                except OSError as exc:
                    raise EsptoolProvenanceError(
                        f"cannot stat package member {child!r}"
                    ) from exc
                if stat.S_ISLNK(info.st_mode):
                    raise EsptoolProvenanceError(
                        f"symlink package member rejected: {child!r}"
                    )
                if stat.S_ISDIR(info.st_mode):
                    is_pycache = name == "__pycache__"
                    if not is_pycache and child not in expected_directories:
                        raise EsptoolProvenanceError(
                            f"unreviewed namespace directory: {child!r}"
                        )
                    retained = tree.open_child_directory(relative, name)
                    if is_pycache:
                        try:
                            cache_names = sorted(
                                os.listdir(tree.directories[retained].fd)
                            )
                        except OSError as exc:
                            raise EsptoolProvenanceError(
                                f"cannot inventory bytecode directory {child!r}"
                            ) from exc
                        for cache_name in cache_names:
                            cache_relative = f"{retained}/{cache_name}"
                            cache_info = os.stat(
                                cache_name,
                                dir_fd=tree.directories[retained].fd,
                                follow_symlinks=False,
                            )
                            if (
                                not stat.S_ISREG(cache_info.st_mode)
                                or cache_info.st_nlink != 1
                                or not _is_allowed_bytecode(
                                    cache_relative, lock
                                )
                            ):
                                raise EsptoolProvenanceError(
                                    "unreviewed or bytecode-only module: "
                                    f"{cache_relative!r}"
                                )
                    else:
                        pending.append(retained)
                    continue
                if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
                    raise EsptoolProvenanceError(
                        f"special or hard-linked package member: {child!r}"
                    )
                if child not in lock.members:
                    raise EsptoolProvenanceError(
                        f"extra esptool package member: {child!r}"
                    )
                inventory.add(child)

        expected_members = set(lock.members)
        if inventory != expected_members:
            missing = sorted(expected_members - inventory)
            extra = sorted(inventory - expected_members)
            raise EsptoolProvenanceError(
                f"package inventory mismatch; missing={missing!r}, "
                f"extra={extra!r}"
            )

        frozen: dict[str, bytes] = {}
        for path, record in lock.members.items():
            pure = PurePosixPath(path)
            parent = tree.directories[pure.parent.as_posix()]
            data = _read_regular_at(parent.fd, pure.name, path)
            info = os.stat(
                pure.name, dir_fd=parent.fd, follow_symlinks=False
            )
            if stat.S_IMODE(info.st_mode) != record.mode:
                raise EsptoolProvenanceError(f"mode mismatch for {path!r}")
            if len(data) != record.size:
                raise EsptoolProvenanceError(f"size mismatch for {path!r}")
            if not hmac.compare_digest(
                hashlib.sha256(data).hexdigest(), record.sha256
            ):
                raise EsptoolProvenanceError(f"SHA-256 mismatch for {path!r}")
            frozen[path] = bytes(data)
        tree.revalidate()
    return FrozenPackage(
        package_root=root,
        members=MappingProxyType(frozen),
    )


def _read_reviewed_lock() -> ReviewedLock:
    scripts_dir = Path(__file__).absolute().parent
    try:
        directory_fd = os.open(str(scripts_dir), _directory_flags())
    except OSError as exc:
        raise EsptoolProvenanceError(
            "cannot retain scripts directory for reviewed lock"
        ) from exc
    try:
        raw = _read_regular_at(
            directory_fd, _LOCK_NAME, "reviewed esptool lock"
        )
    finally:
        os.close(directory_fd)
    return parse_reviewed_lock(raw, EXPECTED_ESPTOOL_LOCK_SHA256)


def _path_is_within(path: Path, root: Path) -> bool:
    try:
        return os.path.commonpath(
            (os.fspath(path), os.fspath(root))
        ) == os.fspath(root)
    except ValueError:
        return False


def _canonical_platformio_purelib() -> Path:
    raw = sysconfig.get_path("purelib")
    if type(raw) is not str or not raw or not os.path.isabs(raw):
        raise EsptoolProvenanceError(
            "invoking Python has no canonical PlatformIO site-packages"
        )
    purelib = Path(os.path.abspath(raw))
    prefix = Path(os.path.realpath(sys.prefix))
    resolved = Path(os.path.realpath(purelib))
    if resolved != purelib or not _path_is_within(resolved, prefix):
        raise EsptoolProvenanceError(
            "PlatformIO site-packages is outside the invoking environment"
        )
    root = purelib / "platformio"
    try:
        root_info = os.lstat(root)
        init_info = os.lstat(root / "__init__.py")
    except OSError as exc:
        raise EsptoolProvenanceError(
            "canonical PlatformIO Core package is unavailable"
        ) from exc
    if (
        not stat.S_ISDIR(root_info.st_mode)
        or stat.S_ISLNK(root_info.st_mode)
        or not stat.S_ISREG(init_info.st_mode)
        or stat.S_ISLNK(init_info.st_mode)
        or init_info.st_nlink != 1
        or Path(os.path.realpath(root)) != root
    ):
        raise EsptoolProvenanceError(
            "canonical PlatformIO Core package origin is unsafe"
        )
    return purelib


def _platformio_module_origin(
    module: ModuleType,
    root: Path,
    name: str,
) -> Path:
    spec = getattr(module, "__spec__", None)
    origin = getattr(spec, "origin", None)
    module_file = getattr(module, "__file__", None)
    loader = getattr(spec, "loader", None)
    if (
        type(origin) is not str
        or type(module_file) is not str
        or origin != module_file
        or not os.path.isabs(origin)
        or not isinstance(loader, importlib.machinery.SourceFileLoader)
    ):
        raise EsptoolProvenanceError(
            f"PlatformIO module {name!r} has an untrusted origin"
        )
    path = Path(os.path.abspath(origin))
    resolved = Path(os.path.realpath(path))
    if resolved != path or not _path_is_within(path, root):
        raise EsptoolProvenanceError(
            f"PlatformIO module {name!r} escaped canonical Core"
        )
    try:
        info = os.lstat(path)
    except OSError as exc:
        raise EsptoolProvenanceError(
            f"PlatformIO module {name!r} origin is unavailable"
        ) from exc
    if (
        not stat.S_ISREG(info.st_mode)
        or stat.S_ISLNK(info.st_mode)
        or info.st_nlink != 1
    ):
        raise EsptoolProvenanceError(
            f"PlatformIO module {name!r} origin is unsafe"
        )
    return path


def _verify_loaded_platformio_modules(root: Path) -> None:
    found = False
    for name, module in tuple(sys.modules.items()):
        if name != "platformio" and not name.startswith("platformio."):
            continue
        found = True
        if not isinstance(module, ModuleType):
            raise EsptoolProvenanceError(
                f"preloaded PlatformIO module {name!r} is untrusted"
            )
        _platformio_module_origin(module, root, name)
    if not found:
        raise EsptoolProvenanceError(
            "canonical PlatformIO Core modules were not loaded"
        )


def _restricted_runtime_sys_path() -> list[str]:
    roots = tuple(
        {
            Path(os.path.realpath(sys.base_prefix)),
            Path(os.path.realpath(sys.prefix)),
        }
    )
    retained: list[str] = []
    for entry in sys.path:
        if type(entry) is not str or not entry or not os.path.isabs(entry):
            continue
        resolved = Path(os.path.realpath(entry))
        if any(_path_is_within(resolved, root) for root in roots):
            retained.append(entry)
    if not retained:
        raise EsptoolProvenanceError(
            "invoking Python has no trusted import search path"
        )
    return retained


def _load_canonical_platformio_api() -> tuple[object, object]:
    """Load PlatformIO Core only from the invoking environment's purelib."""

    purelib = _canonical_platformio_purelib()
    platformio_root = purelib / "platformio"
    preexisting = {
        name
        for name in sys.modules
        if name == "platformio" or name.startswith("platformio.")
    }
    if preexisting:
        # Origin metadata on an existing ModuleType is caller-mutable.  Do not
        # execute or trust any preloaded PlatformIO API, even when its
        # __file__/loader fields point at canonical source paths.
        raise EsptoolProvenanceError(
            "preloaded PlatformIO Core modules are forbidden"
        )

    original_path = list(sys.path)
    imported_before = set(sys.modules)
    try:
        sys.path[:] = _restricted_runtime_sys_path()
        importlib.invalidate_caches()
        tool_module = importlib.import_module(
            "platformio.package.manager.tool"
        )
        meta_module = importlib.import_module("platformio.package.meta")
        _verify_loaded_platformio_modules(platformio_root)
        tool_manager = getattr(tool_module, "ToolPackageManager", None)
        package_spec = getattr(meta_module, "PackageSpec", None)
        if not callable(tool_manager) or not callable(package_spec):
            raise EsptoolProvenanceError(
                "canonical PlatformIO package API is unavailable"
            )
        return tool_manager, package_spec
    except BaseException:
        for name in tuple(sys.modules):
            if (
                name not in imported_before
                and (
                    name == "platformio"
                    or name.startswith("platformio.")
                )
            ):
                sys.modules.pop(name, None)
        raise
    finally:
        sys.path[:] = original_path
        importlib.invalidate_caches()


def resolve_platformio_esptool_root() -> Path:
    """Resolve only PlatformIO Core's exact pinned tool package."""

    api_loaded = False
    try:
        ToolPackageManager, PackageSpec = _load_canonical_platformio_api()
        api_loaded = True
        manager = ToolPackageManager()
        package = manager.get_package(
            PackageSpec(
                name=_PACKAGE_NAME,
                requirements=_PACKAGE_VERSION,
            )
        )
    except Exception as exc:
        raise EsptoolProvenanceError(
            "cannot resolve exact PlatformIO esptool package"
        ) from exc
    finally:
        if api_loaded:
            for name in tuple(sys.modules):
                if name == "platformio" or name.startswith("platformio."):
                    sys.modules.pop(name, None)
            importlib.invalidate_caches()
    if package is None:
        raise EsptoolProvenanceError(
            "exact PlatformIO esptool package is not installed"
        )
    path = getattr(package, "path", None)
    if not isinstance(path, (str, os.PathLike)):
        raise EsptoolProvenanceError(
            "PlatformIO esptool package has no canonical path"
        )
    root = Path(path)
    if not root.is_absolute() or root.name != _PACKAGE_NAME:
        raise EsptoolProvenanceError(
            "PlatformIO returned an unexpected esptool package root"
        )
    return root


def _locked_empty_esptool_config(
    verbose: bool = False,
) -> tuple[configparser.ConfigParser, None]:
    del verbose
    config = configparser.ConfigParser()
    config["esptool"] = {}
    return config, None


class _FrozenEsptoolFinder(importlib.abc.MetaPathFinder, importlib.abc.Loader):
    def __init__(self, frozen: FrozenPackage) -> None:
        sources: dict[str, bytes] = {}
        packages: set[str] = set()
        origins: dict[str, str] = {}
        for path, data in frozen.members.items():
            if not path.endswith(".py"):
                continue
            pure = PurePosixPath(path)
            if pure.name == "__init__.py":
                module_name = ".".join(pure.parent.parts)
                packages.add(module_name)
            else:
                module_name = ".".join(pure.with_suffix("").parts)
            if module_name in sources:
                raise EsptoolProvenanceError(
                    f"ambiguous frozen module {module_name!r}"
                )
            sources[module_name] = bytes(data)
            origins[module_name] = f"frozen-platformio-esptool:{path}"
        if "esptool" not in sources:
            raise EsptoolProvenanceError("frozen esptool package is incomplete")
        self.sources = MappingProxyType(sources)
        self.packages = frozenset(packages)
        self.origins = MappingProxyType(origins)

    def find_spec(
        self,
        fullname: str,
        path: object = None,
        target: ModuleType | None = None,
    ) -> importlib.machinery.ModuleSpec | None:
        del path, target
        if fullname != "esptool" and not fullname.startswith("esptool."):
            return None
        if fullname not in self.sources:
            raise ModuleNotFoundError(
                f"unreviewed esptool module refused: {fullname}"
            )
        spec = importlib.util.spec_from_loader(
            fullname,
            self,
            origin=self.origins[fullname],
            is_package=fullname in self.packages,
        )
        if spec is None:
            raise EsptoolProvenanceError(
                f"cannot create frozen module spec for {fullname!r}"
            )
        spec.has_location = True
        return spec

    def create_module(
        self, spec: importlib.machinery.ModuleSpec
    ) -> ModuleType | None:
        del spec
        return None

    def exec_module(self, module: ModuleType) -> None:
        name = module.__spec__.name
        try:
            source = self.sources[name]
            origin = self.origins[name]
        except KeyError as exc:
            raise EsptoolProvenanceError(
                f"unreviewed esptool module refused: {name}"
            ) from exc
        code = compile(source, origin, "exec", dont_inherit=True)
        exec(code, module.__dict__)
        if name == "esptool.config":
            # Loader constants must never consult cwd, HOME, or environment
            # after the explicit preflight.  Pin the frozen module to an
            # empty configuration before esptool.loader imports this symbol.
            module.load_config_file = _locked_empty_esptool_config


def _path_under_root(value: object, root: str) -> bool:
    if isinstance(value, int):
        return False
    try:
        path = os.fsdecode(os.fspath(value))
    except (TypeError, ValueError):
        return False
    if not os.path.isabs(path):
        return False
    try:
        return os.path.commonpath((path, root)) == root
    except ValueError:
        return False


def _is_relative_filesystem_path(value: object) -> bool:
    if isinstance(value, int):
        return False
    try:
        path = os.fsdecode(os.fspath(value))
    except (TypeError, ValueError):
        return False
    return bool(path) and not os.path.isabs(path)


def _audit_hook(event: str, args: tuple[object, ...]) -> None:
    if event != "open" or not args:
        return
    # Python's audit event does not include openat(2)'s dir_fd.  While a
    # verified runtime is active, reject relative opens entirely so a caller
    # cannot hide a package-root capability behind a pre-opened directory FD.
    # Absolute aliases remain outside this diagnostic hook's authority; the
    # security boundary is the frozen byte-only importer and stub adapter.
    if _AUDIT_ROOTS and _is_relative_filesystem_path(args[0]):
        raise EsptoolProvenanceError(
            "relative filesystem open refused during frozen esptool runtime"
        )
    for root in tuple(_AUDIT_ROOTS):
        if _path_under_root(args[0], root):
            raise EsptoolProvenanceError(
                "post-freeze PlatformIO package filesystem open refused"
            )


def _ensure_audit_hook() -> None:
    global _AUDIT_HOOK_INSTALLED
    with _AUDIT_SETUP_LOCK:
        if not _AUDIT_HOOK_INSTALLED:
            sys.addaudithook(_audit_hook)
            _AUDIT_HOOK_INSTALLED = True


_EXPECTED_SIGNATURES = MappingProxyType(
    {
        "esptool.main": "(argv=None, esp=None)",
        "esptool.cmds.write_flash": "(esp, args)",
        "esptool.cmds.verify_flash": "(esp, args)",
        "esptool.cmds.erase_flash": "(esp, args)",
        "esptool.cmds.run": "(esp, args)",
        "esptool.cmds.flash_id": "(esp, args)",
        "esptool.cmds.read_mac": "(esp, args)",
        "esptool.loader.ESPLoader.__init__": (
            "(self, port='/dev/ttyUSB0', baud=115200, trace_enabled=False)"
        ),
        "esptool.loader.ESPLoader.connect": (
            "(self, mode='default_reset', attempts=7, detecting=False, "
            "warnings=True)"
        ),
        "esptool.loader.ESPLoader.run_stub": "(self, stub=None)",
        "esptool.loader.ESPLoader.run": "(self, reboot=False)",
        "esptool.loader.StubFlasher.__init__": "(self, target)",
        "esptool.loader.StubFlasher.set_preferred_stub_subdir": "(subdir)",
        "esptool.targets.esp32s3.ESP32S3ROM.__init__": (
            "(self, port='/dev/ttyUSB0', baud=115200, trace_enabled=False)"
        ),
        "esptool.targets.esp32s3.ESP32S3ROM.read_mac": (
            "(self, mac_type='BASE_MAC')"
        ),
        "esptool.targets.esp32s3.ESP32S3StubLoader.__init__": (
            "(self, rom_loader)"
        ),
    }
)


def _resolve_attribute(root: object, dotted: str) -> object:
    current = root
    for part in dotted.split("."):
        current = getattr(current, part)
    return current


def _verify_signatures(esptool: ModuleType) -> None:
    for name, expected in _EXPECTED_SIGNATURES.items():
        relative = name.removeprefix("esptool.")
        try:
            subject = _resolve_attribute(esptool, relative)
            actual = str(inspect.signature(subject))
        except Exception as exc:
            raise EsptoolProvenanceError(
                f"cannot inspect pinned signature {name}"
            ) from exc
        if actual != expected:
            raise EsptoolProvenanceError(
                f"signature drift for {name}: {actual!r} != {expected!r}"
            )


def _require_exact_runtime_value(
    owner: object,
    attribute: str,
    expected: object,
) -> None:
    actual = getattr(owner, attribute, None)
    if type(actual) is not type(expected) or actual != expected:
        raise EsptoolProvenanceError(
            f"pinned esptool runtime drift for {attribute}: "
            f"{actual!r} != {expected!r}"
        )


def _verify_frozen_loader_state(
    esptool: ModuleType,
    *,
    retry_pinned: bool,
) -> None:
    loader = getattr(esptool, "loader", None)
    cmds = getattr(esptool, "cmds", None)
    if not isinstance(loader, ModuleType) or not isinstance(cmds, ModuleType):
        raise EsptoolProvenanceError(
            "pinned esptool loader/cmds modules are unavailable"
        )
    try:
        config_items = dict(loader.cfg)
    except Exception as exc:
        raise EsptoolProvenanceError(
            "pinned esptool loader configuration is unreadable"
        ) from exc
    if config_items:
        raise EsptoolProvenanceError(
            "external esptool loader configuration was accepted"
        )
    for name, expected in _EXPECTED_LOADER_CONSTANTS.items():
        _require_exact_runtime_value(loader, name, expected)
    for name, expected in _EXPECTED_CMDS_CONSTANTS.items():
        _require_exact_runtime_value(cmds, name, expected)
    _require_exact_runtime_value(
        esptool,
        "DEFAULT_CONNECT_ATTEMPTS",
        7,
    )
    _require_exact_runtime_value(
        esptool,
        "DEFAULT_OPEN_PORT_ATTEMPTS",
        1,
    )

    wanted_attempts = (1, 1, 1, 1) if retry_pinned else (3, 2, 2, 2)
    for (owner, attribute), expected in zip(
        _retry_policy_subjects(esptool),
        wanted_attempts,
    ):
        _require_exact_runtime_value(owner, attribute, expected)


def _set_and_verify_retry_policy(esptool: ModuleType) -> None:
    subjects = _retry_policy_subjects(esptool)
    for owner, attribute in subjects:
        if not hasattr(owner, attribute):
            raise EsptoolProvenanceError(
                f"missing retry constant {owner!r}.{attribute}"
            )
        setattr(owner, attribute, 1)
    _verify_retry_policy(esptool)


def _retry_policy_subjects(
    esptool: ModuleType,
) -> tuple[tuple[object, str], ...]:
    esp32s3 = sys.modules.get("esptool.targets.esp32s3")
    if not isinstance(esp32s3, ModuleType):
        raise EsptoolProvenanceError("ESP32-S3 target module was not loaded")
    return (
        (esptool.loader, "WRITE_BLOCK_ATTEMPTS"),
        (esptool.loader.ESPLoader, "WRITE_FLASH_ATTEMPTS"),
        (esp32s3.ESP32S3ROM, "WRITE_FLASH_ATTEMPTS"),
        (esp32s3.ESP32S3StubLoader, "WRITE_FLASH_ATTEMPTS"),
    )


def _verify_retry_policy(esptool: ModuleType) -> None:
    for owner, attribute in _retry_policy_subjects(esptool):
        if getattr(owner, attribute, None) != 1:
            raise EsptoolProvenanceError(
                f"retry policy read-back failed for {attribute}"
            )


class VerifiedEsptoolRuntime:
    """Active exclusive frozen esptool import/runtime capability."""

    __slots__ = (
        "package_root",
        "esptool",
        "_finder",
        "_exists_original",
        "_exists_guard",
        "_closed",
    )

    def __init__(
        self,
        package_root: Path,
        esptool: ModuleType,
        finder: _FrozenEsptoolFinder,
        exists_original: object,
        exists_guard: object,
    ) -> None:
        self.package_root = package_root
        self.esptool = esptool
        self._finder = finder
        self._exists_original = exists_original
        self._exists_guard = exists_guard
        self._closed = False

    def audit_loaded_modules(self) -> None:
        if self._closed:
            raise EsptoolProvenanceError("verified esptool runtime is closed")
        _reject_esptool_environment()
        if self._finder not in sys.meta_path:
            raise EsptoolProvenanceError("frozen esptool finder was removed")
        if os.path.exists is not self._exists_guard:
            raise EsptoolProvenanceError(
                "package-root exists guard was replaced"
            )
        for name, module in tuple(sys.modules.items()):
            if name != "esptool" and not name.startswith("esptool."):
                continue
            if name not in self._finder.origins or not isinstance(
                module, ModuleType
            ):
                raise EsptoolProvenanceError(
                    f"unreviewed loaded esptool module: {name!r}"
                )
            spec = getattr(module, "__spec__", None)
            if (
                spec is None
                or spec.origin != self._finder.origins[name]
                or spec.loader is not self._finder
            ):
                raise EsptoolProvenanceError(
                    f"loaded origin mismatch for {name!r}"
                )
        if sys.modules.get("esptool") is not self.esptool:
            raise EsptoolProvenanceError("root esptool module identity changed")
        if self.esptool.loader.StubFlasher is not FrozenStubFlasher:
            raise EsptoolProvenanceError("loader StubFlasher alias changed")
        if self.esptool.StubFlasher is not FrozenStubFlasher:
            raise EsptoolProvenanceError("root StubFlasher alias changed")
        _verify_frozen_loader_state(
            self.esptool,
            retry_pinned=True,
        )
        _verify_retry_policy(self.esptool)

    def audit_after_command(self, stub_loader: object | None = None) -> None:
        self.audit_loaded_modules()
        if stub_loader is not None:
            attempts = getattr(type(stub_loader), "WRITE_FLASH_ATTEMPTS", None)
            if attempts != 1:
                raise EsptoolProvenanceError(
                    "stub-loader retry read-back is not one"
                )

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        normalized_root = os.path.abspath(os.fspath(self.package_root))
        _AUDIT_ROOTS.discard(normalized_root)
        if os.path.exists is self._exists_guard:
            os.path.exists = self._exists_original  # type: ignore[assignment]
        if self._finder in sys.meta_path:
            sys.meta_path.remove(self._finder)
        for name in tuple(sys.modules):
            if name == "esptool" or name.startswith("esptool."):
                sys.modules.pop(name, None)
        FrozenStubFlasher._records = MappingProxyType({})
        FrozenStubFlasher.STUB_SUBDIRS = ["1", "2"]
        _EXECUTION_LOCK.release()

    def __enter__(self) -> "VerifiedEsptoolRuntime":
        if self._closed:
            raise EsptoolProvenanceError("verified esptool runtime is closed")
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


def _reject_preloaded_esptool() -> None:
    offenders = sorted(
        name
        for name in sys.modules
        if name == "esptool" or name.startswith("esptool.")
    )
    if offenders:
        raise EsptoolProvenanceError(
            f"preloaded esptool modules are forbidden: {offenders!r}"
        )


def _reject_esptool_environment() -> None:
    offenders = sorted(
        key
        for key in os.environ
        if type(key) is str and key.startswith("ESPTOOL_")
    )
    if offenders:
        raise EsptoolProvenanceError(
            "external esptool environment is forbidden: "
            + ", ".join(offenders)
        )


def _read_external_config_candidate(path: Path) -> bytes | None:
    try:
        before = os.lstat(path)
    except FileNotFoundError:
        return None
    except OSError as exc:
        raise EsptoolProvenanceError(
            "external esptool configuration could not be inspected"
        ) from exc
    if (
        not stat.S_ISREG(before.st_mode)
        or stat.S_ISLNK(before.st_mode)
        or before.st_nlink != 1
        or before.st_size > _MAX_EXTERNAL_CONFIG_BYTES
    ):
        raise EsptoolProvenanceError(
            "external esptool configuration candidate is unsafe"
        )
    try:
        fd = os.open(path, _file_flags())
    except OSError as exc:
        raise EsptoolProvenanceError(
            "external esptool configuration could not be opened safely"
        ) from exc
    try:
        opened = os.fstat(fd)
        if _identity(opened) != _identity(before):
            raise EsptoolProvenanceError(
                "external esptool configuration changed at open"
            )
        first = _read_all(fd)
        middle = os.fstat(fd)
        os.lseek(fd, 0, os.SEEK_SET)
        second = _read_all(fd)
        after = os.fstat(fd)
        named = os.lstat(path)
        if (
            len(
                {
                    _identity(before),
                    _identity(opened),
                    _identity(middle),
                    _identity(after),
                    _identity(named),
                }
            )
            != 1
            or first != second
            or len(second) > _MAX_EXTERNAL_CONFIG_BYTES
        ):
            raise EsptoolProvenanceError(
                "external esptool configuration changed during read"
            )
        return bytes(second)
    except OSError as exc:
        raise EsptoolProvenanceError(
            "external esptool configuration became unavailable"
        ) from exc
    finally:
        os.close(fd)


def _external_config_candidates() -> tuple[Path, ...]:
    try:
        current = Path(os.getcwd())
        home = Path(os.path.expanduser("~"))
    except (OSError, ValueError) as exc:
        raise EsptoolProvenanceError(
            "cannot determine external esptool configuration locations"
        ) from exc
    directories = (current, home / ".config" / "esptool", home)
    candidates: list[Path] = []
    seen: set[str] = set()
    for directory in directories:
        for name in ("esptool.cfg", "setup.cfg", "tox.ini"):
            path = Path(os.path.abspath(directory / name))
            rendered = os.fspath(path)
            if rendered not in seen:
                seen.add(rendered)
                candidates.append(path)
    return tuple(candidates)


def _reject_external_esptool_configuration() -> None:
    _reject_esptool_environment()
    for path in _external_config_candidates():
        raw = _read_external_config_candidate(path)
        if raw is None:
            continue
        try:
            text = raw.decode("utf-8", errors="strict")
            parser = configparser.RawConfigParser()
            parser.read_string(text, source="<external-esptool-config>")
        except (UnicodeDecodeError, configparser.Error):
            # Stock esptool ignores an invalid candidate.  The frozen config
            # loader below remains pinned empty, so a later replacement cannot
            # win a validation-to-import race.
            continue
        if parser.has_section("esptool"):
            raise EsptoolProvenanceError(
                "external esptool configuration is forbidden"
            )


def load_verified_platformio_esptool() -> VerifiedEsptoolRuntime:
    """Freeze, import, pin, and return the exclusive reviewed esptool runtime."""

    _reject_preloaded_esptool()
    _reject_external_esptool_configuration()
    if not _EXECUTION_LOCK.acquire(blocking=False):
        raise EsptoolProvenanceError(
            "another verified esptool execution is already active"
        )

    finder: _FrozenEsptoolFinder | None = None
    exists_original: object | None = None
    exists_guard: object | None = None
    normalized_root: str | None = None
    try:
        package_root = resolve_platformio_esptool_root()
        reviewed_lock = _read_reviewed_lock()
        frozen = freeze_verified_package(package_root, reviewed_lock)
        stub_records = {
            ("1", "esp32s3.json"): parse_frozen_stub_record(
                frozen.members[
                    "esptool/targets/stub_flasher/1/esp32s3.json"
                ]
            ),
            ("2", "esp32s3.json"): parse_frozen_stub_record(
                frozen.members[
                    "esptool/targets/stub_flasher/2/esp32s3.json"
                ]
            ),
        }
        FrozenStubFlasher._install_records(stub_records)
        finder = _FrozenEsptoolFinder(frozen)
        sys.meta_path.insert(0, finder)

        _ensure_audit_hook()
        normalized_root = os.path.abspath(os.fspath(package_root))
        _AUDIT_ROOTS.add(normalized_root)
        exists_original = os.path.exists

        def guarded_exists(path: object) -> bool:
            assert normalized_root is not None
            if _is_relative_filesystem_path(path):
                raise EsptoolProvenanceError(
                    "relative exists check refused during frozen esptool "
                    "runtime"
                )
            if _path_under_root(path, normalized_root):
                raise EsptoolProvenanceError(
                    "post-freeze PlatformIO package exists check refused"
                )
            assert callable(exists_original)
            return bool(exists_original(path))

        exists_guard = guarded_exists
        os.path.exists = guarded_exists

        esptool = importlib.import_module("esptool")
        if not isinstance(esptool, ModuleType):
            raise EsptoolProvenanceError("esptool import was not a module")
        if getattr(esptool, "__version__", None) != _ESPTOOL_VERSION:
            raise EsptoolProvenanceError("unexpected esptool module version")
        _verify_frozen_loader_state(
            esptool,
            retry_pinned=False,
        )
        _verify_signatures(esptool)
        esptool.loader.StubFlasher = FrozenStubFlasher
        esptool.StubFlasher = FrozenStubFlasher
        if esptool.loader.StubFlasher is not FrozenStubFlasher:
            raise EsptoolProvenanceError(
                "loader StubFlasher replacement read-back failed"
            )
        if esptool.StubFlasher is not FrozenStubFlasher:
            raise EsptoolProvenanceError(
                "root StubFlasher replacement read-back failed"
            )
        _set_and_verify_retry_policy(esptool)
        runtime = VerifiedEsptoolRuntime(
            package_root=package_root,
            esptool=esptool,
            finder=finder,
            exists_original=exists_original,
            exists_guard=exists_guard,
        )
        runtime.audit_loaded_modules()
        return runtime
    except BaseException as exc:
        if normalized_root is not None:
            _AUDIT_ROOTS.discard(normalized_root)
        if (
            exists_original is not None
            and exists_guard is not None
            and os.path.exists is exists_guard
        ):
            os.path.exists = exists_original  # type: ignore[assignment]
        if finder is not None and finder in sys.meta_path:
            sys.meta_path.remove(finder)
        for name in tuple(sys.modules):
            if name == "esptool" or name.startswith("esptool."):
                sys.modules.pop(name, None)
        FrozenStubFlasher._records = MappingProxyType({})
        FrozenStubFlasher.STUB_SUBDIRS = ["1", "2"]
        _EXECUTION_LOCK.release()
        if isinstance(exc, EsptoolProvenanceError):
            raise
        if not isinstance(exc, Exception):
            raise
        raise EsptoolProvenanceError(
            "verified PlatformIO esptool load failed"
        ) from exc
