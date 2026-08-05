"""Strict mixed-version bundle policy for Backend Badge Lite factories."""

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
EXPECTED_ROLES = frozenset(("probe", "scanner", "uplink"))
MAX_ARCHIVE_BYTES = 32 * 1024 * 1024
MAX_MEMBER_BYTES = 8 * 1024 * 1024
MAX_RELEASE_INDEX_BYTES = 2 * 1024 * 1024
FACTORY_ASSET_RE = re.compile(r"^lite-factory-flasher-v?(.+)\.zip$")

EXPECTED_PARTS = {
    "probe": {
        0x0000: "probe/bootloader.bin",
        0x8000: "probe/partitions.bin",
        0x10000: "probe/firmware.bin",
    },
    "scanner": {
        0x0000: "scanner/bootloader.bin",
        0x8000: "scanner/partitions.bin",
        0xF000: "scanner/ota_data_initial.bin",
        0x20000: "scanner/firmware.bin",
    },
    "uplink": {
        0x0000: "uplink/bootloader.bin",
        0x8000: "uplink/partitions.bin",
        0xF000: "uplink/ota_data_initial.bin",
        0x20000: "uplink/firmware.bin",
    },
}

EXPECTED_FIXED_IDENTITIES = {
    "probe": {
        "project": "fof_badge_factory_probe",
        "target": "factory-probe-s3",
        "version": "1.0.0",
    },
    "scanner": {
        "project": "fof_badge_scanner",
        "target": "scanner-s3-combo-fof_badge",
        "version": "0.67.2-badge-defcon34",
    },
}
EXPECTED_UPLINK_PROJECT = "fof_backend_uplink"
EXPECTED_UPLINK_TARGET = "uplink-s3-backend"
EXPECTED_HARDWARE = "seeed_xiao_esp32s3"
TRUSTED_RELEASE_BUNDLE_SHA256 = {
    "0.2.0-backend": (
        "6d39ff58f5d9030b40efb80cb2e1aa62e901c230e15f4b3f2fee5854b31d9536"
    ),
}
EXPECTED_FIXED_PARTS = {
    "probe": {
        0x0000: (20832, "8b92c8046e5bec77521173a9a530b8ba1cd281df84d88b496d6d922fdb243dea"),
        0x8000: (3072, "520c18ca0bda73aed288d427d260ab051d71219f605707350e6c42401f4836fc"),
        0x10000: (218848, "287d28b5213e3d063b16fade529ef873817f6f7d79c1eff99f20b1a233835c87"),
    },
    "scanner": {
        0x0000: (20928, "fd1eca217b8c8b25407fe2a0c1b3c084734047a4fcd926ce03435b92d6bacad6"),
        0x8000: (3072, "20ce69fdee71cb61bb1d6833677caad5309ac4cd73eff5b734e66539c876d3e9"),
        0xF000: (8192, "7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f"),
        0x20000: (1216800, "2d0e84501baf3bc929eed03a0b9c1f0272ed66baa9b81dd4513d6dc3fa2c032b"),
    },
}


class BundleError(RuntimeError):
    """A Lite factory bundle is incomplete, unsafe, or incompatible."""


@dataclass(frozen=True, slots=True)
class LiteFactoryBundle:
    manifest: dict[str, Any]
    root: Path
    source: str
    bundle_sha256: str

    @property
    def version(self) -> str:
        return str(self.manifest["version"])

    @property
    def scanner_version(self) -> str:
        return str(self.manifest["layouts"]["scanner"]["identity"]["version"])

    def layout(self, role: str) -> dict[str, Any]:
        return dict(self.manifest["layouts"][role])


def is_trusted_release_bundle(bundle: LiteFactoryBundle) -> bool:
    """Bind release selection to a digest reviewed in this flasher version."""

    return (
        TRUSTED_RELEASE_BUNDLE_SHA256.get(bundle.version)
        == bundle.bundle_sha256
    )


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def version_key(value: str) -> tuple[int, int, int, str]:
    match = re.fullmatch(
        r"v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z._-]+))?",
        value,
    )
    if not match:
        raise BundleError(f"invalid ordered firmware version: {value!r}")
    return int(match[1]), int(match[2]), int(match[3]), match[4] or ""


def is_strictly_newer(candidate: str, current: str) -> bool:
    candidate_key = version_key(candidate)
    current_key = version_key(current)
    if candidate_key[:3] != current_key[:3]:
        return candidate_key[:3] > current_key[:3]
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
        raise BundleError("Lite factory archive is oversized")
    root = Path(tempfile.mkdtemp(prefix="fof-lite-factory-"))
    seen: set[str] = set()
    try:
        with zipfile.ZipFile(archive) as bundle:
            total = 0
            for info in bundle.infolist():
                member = PurePosixPath(info.filename)
                mode = info.external_attr >> 16
                if (
                    info.filename in seen
                    or member.is_absolute()
                    or ".." in member.parts
                    or info.is_dir()
                    or info.file_size > MAX_MEMBER_BYTES
                    or mode & 0o170000 == 0o120000
                ):
                    raise BundleError(f"unsafe bundle member: {info.filename!r}")
                seen.add(info.filename)
                total += info.file_size
                if total > MAX_ARCHIVE_BYTES:
                    raise BundleError("Lite factory archive expands beyond limit")
                destination = root.joinpath(*member.parts)
                destination.parent.mkdir(parents=True, exist_ok=True)
                with bundle.open(info) as source, destination.open("wb") as target:
                    shutil.copyfileobj(source, target)
    except Exception:
        shutil.rmtree(root, ignore_errors=True)
        raise
    return root


def _validate_assembly(manifest: dict[str, Any]) -> None:
    assembly = manifest.get("assembly")
    expected = {
        "board_count": 3,
        "layouts": {
            "scanner0": "scanner",
            "scanner1": "scanner",
            "uplink": "uplink",
        },
        "flash_order": ["scanner0", "scanner1", "uplink"],
    }
    if assembly != expected:
        raise BundleError("Lite factory assembly contract mismatch")


def _expected_identity(role: str, version: str) -> dict[str, str]:
    if role == "uplink":
        return {
            "project": EXPECTED_UPLINK_PROJECT,
            "target": EXPECTED_UPLINK_TARGET,
            "version": version,
        }
    return dict(EXPECTED_FIXED_IDENTITIES[role])


def _validate_manifest(
    root: Path,
    source: str,
    bundle_sha256: str,
) -> LiteFactoryBundle:
    try:
        manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise BundleError(f"cannot read Lite factory manifest: {exc}") from exc
    if not isinstance(manifest, dict) or set(manifest) != {
        "schema", "family", "version", "min_flasher", "assembly", "layouts"
    }:
        raise BundleError("Lite factory manifest shape mismatch")
    if manifest["schema"] != BUNDLE_SCHEMA or manifest["family"] != "badge_lite":
        raise BundleError("unsupported Lite factory bundle schema/family")
    version = str(manifest["version"])
    version_key(version)
    if version_key(str(manifest["min_flasher"]))[:3] > version_key(FLASHER_VERSION)[:3]:
        raise BundleError("Lite factory bundle requires a newer flasher")
    layouts = manifest["layouts"]
    if not isinstance(layouts, dict) or set(layouts) != EXPECTED_ROLES:
        raise BundleError("Lite bundle must contain probe, scanner, and uplink")
    _validate_assembly(manifest)

    declared = {"manifest.json"}
    for role, layout in layouts.items():
        if not isinstance(layout, dict) or set(layout) != {
            "chip", "flash_size", "hardware", "identity", "parts"
        }:
            raise BundleError(f"{role} layout shape mismatch")
        if (
            layout["chip"] != "ESP32-S3"
            or layout["flash_size"] != "8MB"
            or layout["hardware"] != EXPECTED_HARDWARE
        ):
            raise BundleError(f"{role} layout has unsupported hardware")
        identity = layout["identity"]
        if identity != _expected_identity(role, version):
            raise BundleError(f"{role} production identity mismatch")
        parts = layout["parts"]
        if not isinstance(parts, list) or not parts:
            raise BundleError(f"{role} layout is empty")
        offsets: set[int] = set()
        for part in parts:
            if not isinstance(part, dict) or set(part) != {
                "offset", "path", "size", "sha256"
            }:
                raise BundleError(f"{role} layout part schema mismatch")
            offset = part["offset"]
            relative = part["path"]
            if type(offset) is not int or offset < 0 or offset in offsets:
                raise BundleError(f"{role} layout offsets are invalid")
            offsets.add(offset)
            pure = PurePosixPath(relative) if isinstance(relative, str) else PurePosixPath("/")
            if pure.is_absolute() or ".." in pure.parts or not pure.parts:
                raise BundleError(f"unsafe declared path in {role}")
            if pure.parts[0] != role:
                raise BundleError(f"{role} file is outside its role directory")
            path = root.joinpath(*pure.parts)
            declared.add(pure.as_posix())
            if (
                path.is_symlink()
                or not path.is_file()
                or type(part["size"]) is not int
                or path.stat().st_size != part["size"]
            ):
                raise BundleError(f"{role} part size mismatch: {relative}")
            if not isinstance(part["sha256"], str) or sha256(path) != part["sha256"]:
                raise BundleError(f"{role} part digest mismatch: {relative}")
        mapping = {part["offset"]: part["path"] for part in parts}
        if mapping != EXPECTED_PARTS[role]:
            raise BundleError(f"{role} partition mapping is not exact")
        ordered = sorted(parts, key=lambda item: item["offset"])
        for current, following in zip(ordered, ordered[1:]):
            if current["offset"] + current["size"] > following["offset"]:
                raise BundleError(f"{role} flash regions overlap")
        if ordered[-1]["offset"] + ordered[-1]["size"] > 8 * 1024 * 1024:
            raise BundleError(f"{role} layout exceeds the 8 MB flash")

        app_parts = [part for part in parts if part["path"].endswith("/firmware.bin")]
        if len(app_parts) != 1:
            raise BundleError(f"{role} needs exactly one application image")
        app_path = root / app_parts[0]["path"]
        embedded_project, embedded_version = _app_identity(app_path)
        if (
            embedded_project != identity["project"]
            or embedded_version != identity["version"]
        ):
            raise BundleError(f"{role} embedded app identity mismatch")
        if role != "probe":
            image = app_path.read_bytes()
            if identity["target"].encode("ascii") not in image:
                raise BundleError(f"{role} embedded target marker missing")
            if EXPECTED_HARDWARE.encode("ascii") not in image:
                raise BundleError(f"{role} embedded hardware marker missing")
            if role == "scanner" and b"scanner-s3-combo-backend" in image:
                raise BundleError("backend scanner image is forbidden in Lite factory")
        if role == "uplink" and b"fof_badge_uplink" in app_path.read_bytes():
            raise BundleError("native badge uplink image is forbidden")
        fixed_parts = EXPECTED_FIXED_PARTS.get(role)
        if fixed_parts is not None:
            observed = {
                part["offset"]: (part["size"], part["sha256"])
                for part in parts
            }
            if observed != fixed_parts:
                raise BundleError(
                    f"{role} bytes do not match the accepted factory release"
                )

    actual = {
        path.relative_to(root).as_posix()
        for path in root.rglob("*")
        if path.is_file()
    }
    if actual != declared:
        raise BundleError("Lite bundle has undeclared or missing files")
    return LiteFactoryBundle(manifest, root, source, bundle_sha256)


def load_bundle(path: Path, *, source: str | None = None) -> LiteFactoryBundle:
    path = path.resolve()
    if path.is_dir():
        raise BundleError(
            "Lite factory bundles must be immutable ZIP archives, not directories"
        )
    root = _safe_extract(path)
    return _validate_manifest(root, source or str(path), sha256(path))


def select_bundle(
    embedded: LiteFactoryBundle,
    releases: Iterable[LiteFactoryBundle],
    *,
    offline: bool = False,
) -> LiteFactoryBundle:
    if offline:
        return embedded
    newer = [item for item in releases if is_strictly_newer(item.version, embedded.version)]
    return max(newer, key=lambda item: version_key(item.version), default=embedded)


def _download_limited(
    url: str,
    target: Path,
    timeout_s: float,
    *,
    opener: Any = urllib.request.urlopen,
) -> None:
    request = urllib.request.Request(
        url,
        headers={"User-Agent": "fof-lite-factory-flasher/1"},
    )
    partial = target.with_name(target.name + ".part")
    total = 0
    try:
        with opener(request, timeout=timeout_s) as response:
            declared = response.headers.get("Content-Length")
            if declared is not None and int(declared) > MAX_ARCHIVE_BYTES:
                raise BundleError("remote Lite bundle exceeds size limit")
            with partial.open("wb") as output:
                while True:
                    chunk = response.read(64 * 1024)
                    if not chunk:
                        break
                    total += len(chunk)
                    if total > MAX_ARCHIVE_BYTES:
                        raise BundleError("remote Lite bundle exceeds size limit")
                    output.write(chunk)
        partial.replace(target)
    finally:
        partial.unlink(missing_ok=True)


def fetch_github_bundles(
    repository: str,
    destination: Path,
    *,
    timeout_s: float = 8,
) -> list[LiteFactoryBundle]:
    """Download complete Lite ZIPs only from final backend releases."""

    request = urllib.request.Request(
        f"https://api.github.com/repos/{repository}/releases?per_page=20",
        headers={
            "Accept": "application/vnd.github+json",
            "User-Agent": "fof-lite-factory-flasher/1",
        },
    )
    with urllib.request.urlopen(request, timeout=timeout_s) as response:
        raw = response.read(MAX_RELEASE_INDEX_BYTES + 1)
    if len(raw) > MAX_RELEASE_INDEX_BYTES:
        raise BundleError("GitHub release index exceeds size limit")
    try:
        releases = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise BundleError("GitHub release index is invalid JSON") from exc
    destination.mkdir(parents=True, exist_ok=True)
    result: list[LiteFactoryBundle] = []
    for release in releases if isinstance(releases, list) else []:
        if release.get("draft") or release.get("prerelease"):
            continue
        tag = str(release.get("tag_name", ""))
        if not tag.endswith("-backend"):
            continue
        for asset in release.get("assets", []):
            name = str(asset.get("name", ""))
            if not FACTORY_ASSET_RE.fullmatch(name) or name != f"lite-factory-flasher-{tag}.zip":
                continue
            target = destination / name
            try:
                _download_limited(asset["browser_download_url"], target, timeout_s)
                bundle = load_bundle(target, source=f"github:{tag}")
                if tag.lstrip("v") == bundle.version.lstrip("v"):
                    result.append(bundle)
            except (
                BundleError,
                OSError,
                ValueError,
                zipfile.BadZipFile,
                KeyError,
                TypeError,
            ):
                continue
    return result
