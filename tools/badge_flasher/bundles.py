"""Validated offline/GitHub firmware bundle policy for the badge factory."""

from __future__ import annotations

import hashlib
import json
import re
import shutil
import struct
import tempfile
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Iterable


BUNDLE_SCHEMA = 1
FLASHER_VERSION = "1.0.0"
EXPECTED_TARGETS = {"probe", "uplink", "scanner"}
MAX_ARCHIVE_BYTES = 32 * 1024 * 1024
MAX_MEMBER_BYTES = 8 * 1024 * 1024
MAX_RELEASE_INDEX_BYTES = 2 * 1024 * 1024
FACTORY_ASSET_RE = re.compile(r"^badge-factory-flasher-v?(.+)\.zip$")

EXPECTED_PARTS = {
    "probe": {
        0x0000: "probe/bootloader.bin",
        0x8000: "probe/partitions.bin",
        0x10000: "probe/firmware.bin",
    },
    "uplink": {
        0x0000: "uplink/bootloader.bin",
        0x8000: "uplink/partitions.bin",
        0xF000: "uplink/ota_data_initial.bin",
        0x20000: "uplink/firmware.bin",
    },
    "scanner": {
        0x0000: "scanner/bootloader.bin",
        0x8000: "scanner/partitions.bin",
        0xF000: "scanner/ota_data_initial.bin",
        0x20000: "scanner/firmware.bin",
    },
}


class BundleError(RuntimeError):
    """A firmware bundle is incomplete, unsafe, or incompatible."""


@dataclass(frozen=True, slots=True)
class FactoryBundle:
    manifest: dict[str, Any]
    root: Path
    source: str
    bundle_sha256: str

    @property
    def version(self) -> str:
        return str(self.manifest["version"])

    def layout(self, role: str) -> dict[str, Any]:
        return dict(self.manifest["layouts"][role])


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _version_key(value: str) -> tuple[int, int, int, str]:
    match = re.fullmatch(r"v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z._-]+))?", value)
    if not match:
        raise BundleError(f"invalid ordered firmware version: {value!r}")
    return (int(match[1]), int(match[2]), int(match[3]), match[4] or "")


def is_strictly_newer(candidate: str, current: str) -> bool:
    ckey, current_key = _version_key(candidate), _version_key(current)
    if ckey[:3] != current_key[:3]:
        return ckey[:3] > current_key[:3]
    return False


def _app_identity(path: Path) -> tuple[str, str]:
    image = path.read_bytes()
    if len(image) < 0x20 + 112 or image[0] != 0xE9:
        raise BundleError(f"invalid ESP-IDF app image: {path.name}")
    descriptor = image[0x20:0x20 + 112]
    if struct.unpack_from("<I", descriptor)[0] != 0xABCD5432:
        raise BundleError(f"missing ESP-IDF app descriptor: {path.name}")
    try:
        version = descriptor[16:48].split(b"\0", 1)[0].decode("ascii")
        project = descriptor[48:80].split(b"\0", 1)[0].decode("ascii")
    except UnicodeDecodeError as exc:
        raise BundleError(f"non-ASCII ESP-IDF app identity: {path.name}") from exc
    if not version or not project:
        raise BundleError(f"empty ESP-IDF app identity: {path.name}")
    return project, version


def _safe_extract(archive: Path) -> Path:
    if archive.stat().st_size > MAX_ARCHIVE_BYTES:
        raise BundleError("factory bundle archive is oversized")
    root = Path(tempfile.mkdtemp(prefix="fof-badge-factory-"))
    seen: set[str] = set()
    try:
        with zipfile.ZipFile(archive) as zf:
            total = 0
            for info in zf.infolist():
                member = PurePosixPath(info.filename)
                if (
                    info.filename in seen
                    or member.is_absolute()
                    or ".." in member.parts
                    or info.is_dir()
                    or info.file_size > MAX_MEMBER_BYTES
                    or (info.external_attr >> 16) & 0o170000 == 0o120000
                ):
                    raise BundleError(f"unsafe bundle member: {info.filename!r}")
                seen.add(info.filename)
                total += info.file_size
                if total > MAX_ARCHIVE_BYTES:
                    raise BundleError("factory bundle expands beyond size limit")
                target = root.joinpath(*member.parts)
                target.parent.mkdir(parents=True, exist_ok=True)
                with zf.open(info) as source, target.open("wb") as destination:
                    shutil.copyfileobj(source, destination)
    except Exception:
        shutil.rmtree(root, ignore_errors=True)
        raise
    return root


def _validate_manifest(root: Path, source: str, bundle_sha256: str) -> FactoryBundle:
    try:
        manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise BundleError(f"cannot read factory manifest: {exc}") from exc
    if not isinstance(manifest, dict) or manifest.get("schema") != BUNDLE_SCHEMA:
        raise BundleError("unsupported factory bundle schema")
    if set(manifest.get("layouts", {})) != EXPECTED_TARGETS:
        raise BundleError("factory bundle must contain probe, uplink, and scanner layouts")
    version = str(manifest.get("version", ""))
    _version_key(version)
    if _version_key(str(manifest.get("min_flasher", "")))[:3] > _version_key(FLASHER_VERSION)[:3]:
        raise BundleError("factory bundle requires a newer flasher")

    declared = {"manifest.json"}
    for role, layout in manifest["layouts"].items():
        if layout.get("chip") != "ESP32-S3" or layout.get("flash_size") != "8MB":
            raise BundleError(f"{role} layout has unsupported hardware")
        parts = layout.get("parts")
        if not isinstance(parts, list) or not parts:
            raise BundleError(f"{role} layout is empty")
        offsets: set[int] = set()
        for part in parts:
            if not isinstance(part, dict) or set(part) != {"offset", "path", "size", "sha256"}:
                raise BundleError(f"{role} layout part schema mismatch")
            offset = part["offset"]
            rel = part["path"]
            if type(offset) is not int or offset < 0 or offset in offsets:
                raise BundleError(f"{role} layout offsets are invalid or duplicated")
            offsets.add(offset)
            pure = PurePosixPath(rel) if isinstance(rel, str) else PurePosixPath("/")
            if pure.is_absolute() or ".." in pure.parts:
                raise BundleError(f"unsafe declared path in {role}")
            path = root.joinpath(*pure.parts)
            if not pure.parts or pure.parts[0] != role:
                raise BundleError(f"{role} layout file is outside its role directory")
            declared.add(pure.as_posix())
            if path.is_symlink() or not path.is_file() or path.stat().st_size != part["size"]:
                raise BundleError(f"{role} part size mismatch: {rel}")
            if _sha256(path) != part["sha256"]:
                raise BundleError(f"{role} part digest mismatch: {rel}")
        actual_mapping = {part["offset"]: part["path"] for part in parts}
        if actual_mapping != EXPECTED_PARTS[role]:
            raise BundleError(f"{role} layout does not match the exact safe partition mapping")
        ordered = sorted(parts, key=lambda item: item["offset"])
        for current, following in zip(ordered, ordered[1:]):
            if current["offset"] + current["size"] > following["offset"]:
                raise BundleError(f"{role} layout regions overlap")
        if ordered[-1]["offset"] + ordered[-1]["size"] > 8 * 1024 * 1024:
            raise BundleError(f"{role} layout exceeds the 8MB flash boundary")

        identity = layout.get("identity")
        if role != "probe":
            expected_project = "fof_badge_uplink" if role == "uplink" else "fof_badge_scanner"
            expected_target = "uplink-s3-fof_badge" if role == "uplink" else "scanner-s3-combo-fof_badge"
            if identity != {"project": expected_project, "target": expected_target, "version": version}:
                raise BundleError(f"{role} production identity mismatch")
        elif identity != {
            "project": "fof_badge_factory_probe",
            "target": "factory-probe-s3",
            "version": "1.0.0",
        }:
            raise BundleError("probe identity mismatch")

        app_parts = [part for part in parts if str(part["path"]).endswith("/firmware.bin")]
        if len(app_parts) != 1:
            raise BundleError(f"{role} layout must declare exactly one application image")
        app_path = root / app_parts[0]["path"]
        embedded_project, embedded_version = _app_identity(app_path)
        if embedded_project != identity["project"] or embedded_version != identity["version"]:
            raise BundleError(f"{role} embedded app identity mismatch")
        if role != "probe":
            image = app_path.read_bytes()
            if identity["target"].encode("ascii") not in image:
                raise BundleError(f"{role} embedded target marker missing")
            if b"seeed_xiao_esp32s3" not in image:
                raise BundleError(f"{role} embedded hardware marker missing")

    actual = {
        path.relative_to(root).as_posix()
        for path in root.rglob("*")
        if path.is_file()
    }
    if actual != declared:
        raise BundleError("bundle contains undeclared or missing files")
    return FactoryBundle(manifest, root, source, bundle_sha256)


def load_bundle(path: Path, *, source: str | None = None) -> FactoryBundle:
    path = path.resolve()
    if path.is_dir():
        digest = hashlib.sha256((path / "manifest.json").read_bytes()).hexdigest()
        return _validate_manifest(path, source or "directory", digest)
    root = _safe_extract(path)
    return _validate_manifest(root, source or str(path), _sha256(path))


def select_bundle(
    embedded: FactoryBundle, releases: Iterable[FactoryBundle], *, offline: bool = False
) -> FactoryBundle:
    if offline:
        return embedded
    valid_newer = [item for item in releases if is_strictly_newer(item.version, embedded.version)]
    return max(valid_newer, key=lambda item: _version_key(item.version), default=embedded)


def _download_limited(
    url: str, target: Path, timeout_s: float,
    *, opener: Any = urllib.request.urlopen,
) -> None:
    request = urllib.request.Request(url, headers={"User-Agent": "fof-badge-flasher/1"})
    partial = target.with_name(target.name + ".part")
    total = 0
    try:
        with opener(request, timeout=timeout_s) as response:
            declared = response.headers.get("Content-Length")
            if declared is not None and int(declared) > MAX_ARCHIVE_BYTES:
                raise BundleError("remote factory bundle exceeds download size limit")
            with partial.open("wb") as output:
                while True:
                    chunk = response.read(64 * 1024)
                    if not chunk:
                        break
                    total += len(chunk)
                    if total > MAX_ARCHIVE_BYTES:
                        raise BundleError("remote factory bundle exceeds download size limit")
                    output.write(chunk)
        partial.replace(target)
    finally:
        partial.unlink(missing_ok=True)


def fetch_github_bundles(
    repository: str, destination: Path, *, timeout_s: float = 8
) -> list[FactoryBundle]:
    """Download only complete factory ZIP assets from public final releases."""
    request = urllib.request.Request(
        f"https://api.github.com/repos/{repository}/releases?per_page=20",
        headers={"Accept": "application/vnd.github+json", "User-Agent": "fof-badge-flasher/1"},
    )
    with urllib.request.urlopen(request, timeout=timeout_s) as response:
        raw_index = response.read(MAX_RELEASE_INDEX_BYTES + 1)
    if len(raw_index) > MAX_RELEASE_INDEX_BYTES:
        raise BundleError("GitHub release index exceeds size limit")
    try:
        releases = json.loads(raw_index)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise BundleError("GitHub release index is invalid JSON") from exc
    destination.mkdir(parents=True, exist_ok=True)
    bundles: list[FactoryBundle] = []
    for release in releases if isinstance(releases, list) else []:
        if release.get("draft") or release.get("prerelease"):
            continue
        for asset in release.get("assets", []):
            name = str(asset.get("name", ""))
            tag = str(release.get("tag_name", ""))
            if not FACTORY_ASSET_RE.fullmatch(name) or name != f"badge-factory-flasher-{tag}.zip":
                continue
            target = destination / name
            try:
                _download_limited(asset["browser_download_url"], target, timeout_s)
                bundle = load_bundle(target, source=f"github:{tag}")
                if tag.lstrip("v") != bundle.version.lstrip("v"):
                    continue
                bundles.append(bundle)
            except (BundleError, OSError, ValueError, zipfile.BadZipFile, KeyError, TypeError):
                continue
    return bundles
