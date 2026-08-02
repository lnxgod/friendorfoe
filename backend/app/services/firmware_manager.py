"""Firmware management — fetch from GitHub releases, cache, serve to dashboard.

Pulls firmware binaries from the latest GitHub release, caches them locally,
and serves them for OTA push to ESP32 nodes.
"""

import hashlib
import logging
import re
import struct
import time
import zlib
from collections import OrderedDict
from dataclasses import dataclass
from pathlib import Path

import httpx

logger = logging.getLogger(__name__)

GITHUB_REPO = "lnxgod/friendorfoe"
# Use /releases (not /releases/latest) to include prereleases
GITHUB_API = f"https://api.github.com/repos/{GITHUB_REPO}/releases"
CACHE_DIR = Path("/tmp/fof-firmware")
CACHE_TTL_S = 1800  # Re-check GitHub every 30 minutes
IMAGE_VERSION_CACHE_SIZE = 32

_ESP_IMAGE_MAGIC = 0xE9
_APP_DESC_OFFSET = 0x20
_APP_DESC_MAGIC = 0xABCD5432
_APP_DESC_MIN_SIZE = 112
_BACKEND_IDENTITY_MAGIC = struct.pack("<I", 0x42464F46)
_BACKEND_IDENTITY_STRUCT = struct.Struct("<IHH40s40s40s32sI")
_BACKEND_VERSION_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+-backend$")

# Repo root relative to backend/app/services/firmware_manager.py
_REPO_ROOT = Path(__file__).resolve().parents[3]

# Current production firmware support is S3-only. Older targets remain in git
# history, but the live catalog must not offer them for OTA rollout.
FIRMWARE_TYPES = {
    "uplink-s3": {
        "description": "Uplink node (ESP32-S3 N16R8)",
        "asset_pattern": "uplink-s3",
        "board": "esp32s3",
        "local_bin": _REPO_ROOT / "esp32/uplink/.pio/build/uplink-s3/firmware.bin",
    },
    "uplink-s3-fof_badge": {
        "description": "FoF Badge uplink (Seeed XIAO ESP32-S3)",
        "asset_pattern": "uplink-s3-fof_badge",
        "board": "esp32s3",
        "local_bin": _REPO_ROOT / "esp32/uplink/.pio/build/uplink-s3-fof_badge/firmware.bin",
    },
    "scanner-s3-combo-seed": {
        "description": "BLE + WiFi scanner (ESP32-S3 Seed/Mini N8R8)",
        "asset_pattern": "scanner-s3-combo-seed",
        "board": "esp32s3",
        "local_bin": _REPO_ROOT / "esp32/scanner/.pio/build/scanner-s3-combo-seed/firmware.bin",
    },
    "scanner-s3-combo-fof_badge": {
        "description": "FoF Badge BLE + WiFi scanner (Seeed XIAO ESP32-S3)",
        "asset_pattern": "scanner-s3-combo-fof_badge",
        "board": "esp32s3",
        "local_bin": _REPO_ROOT / "esp32/scanner/.pio/build/scanner-s3-combo-fof_badge/firmware.bin",
    },
    "scanner-s3-combo": {
        "description": "BLE + WiFi scanner (ESP32-S3)",
        "asset_pattern": "scanner-s3-combo",
        "board": "esp32s3",
        "local_bin": _REPO_ROOT / "esp32/scanner/.pio/build/scanner-s3-combo/firmware.bin",
    },
    "uplink-s3-backend": {
        "description": "Backend sensor uplink (Seeed XIAO ESP32-S3)",
        "asset_pattern": "uplink-s3-backend",
        "board": "esp32s3",
        "project": "fof_backend_uplink",
        "hardware": "seeed_xiao_esp32s3",
        "image_kind": 0,
        "partition_capacity": 0x200000,
        "local_bin": _REPO_ROOT / "backend-firmware/uplink/.pio/build/uplink-s3-backend/firmware.bin",
    },
    "scanner-s3-combo-backend": {
        "description": "Backend sensor BLE + Wi-Fi scanner (Seeed XIAO ESP32-S3)",
        "asset_pattern": "scanner-s3-combo-backend",
        "board": "esp32s3",
        "project": "fof_backend_scanner",
        "hardware": "seeed_xiao_esp32s3",
        "image_kind": 1,
        "partition_capacity": 0x200000,
        "local_bin": _REPO_ROOT / "backend-firmware/scanner/.pio/build/scanner-s3-combo-backend/firmware.bin",
    },
}


def _decode_identity_string(raw: bytes) -> str | None:
    nul = raw.find(b"\0")
    if nul <= 0 or any(raw[nul + 1:]):
        return None
    try:
        value = raw[:nul].decode("ascii")
    except UnicodeDecodeError:
        return None
    if any(ord(char) < 0x21 or ord(char) > 0x7E for char in value):
        return None
    return value


def _parse_backend_identity_record(image: bytes, offset: int) -> dict | None:
    end = offset + _BACKEND_IDENTITY_STRUCT.size
    if end > len(image):
        return None
    record = image[offset:end]
    magic, schema, image_kind, target_raw, project_raw, hardware_raw, version_raw, crc32 = (
        _BACKEND_IDENTITY_STRUCT.unpack(record)
    )
    if magic != 0x42464F46 or schema != 1 or image_kind not in (0, 1):
        return None
    if (zlib.crc32(record[:160]) & 0xFFFFFFFF) != crc32:
        return None
    target = _decode_identity_string(target_raw)
    project = _decode_identity_string(project_raw)
    hardware = _decode_identity_string(hardware_raw)
    version = _decode_identity_string(version_raw)
    if None in (target, project, hardware, version):
        return None
    return {
        "offset": offset,
        "schema": schema,
        "image_kind": image_kind,
        "target": target,
        "project": project,
        "hardware": hardware,
        "version": version,
    }


def _parse_backend_identity(image: bytes) -> dict | None:
    valid: list[dict] = []
    start = 0
    while True:
        offset = image.find(_BACKEND_IDENTITY_MAGIC, start)
        if offset < 0:
            break
        parsed = _parse_backend_identity_record(image, offset)
        if parsed is not None:
            valid.append(parsed)
        start = offset + 1
    return valid[0] if len(valid) == 1 else None


def _validated_backend_image_info(name: str, image: bytes) -> dict | None:
    info = FIRMWARE_TYPES.get(name)
    if info is None or not name.endswith("-backend"):
        return None
    desc = _parse_app_desc_bytes(image)
    identity = _parse_backend_identity(image)
    if desc is None or identity is None:
        return None
    expected = {
        "target": name,
        "project": info["project"],
        "hardware": info["hardware"],
        "image_kind": info["image_kind"],
    }
    if any(identity[key] != value for key, value in expected.items()):
        return None
    if identity["project"] != desc["project"] or identity["version"] != desc["version"]:
        return None
    if _BACKEND_VERSION_RE.fullmatch(identity["version"]) is None:
        return None
    if not (0 < len(image) <= info["partition_capacity"]):
        return None
    return {**identity, "size": len(image)}


def _asset_target(asset_name: str, release_tag: str) -> str | None:
    ordered = sorted(FIRMWARE_TYPES.items(), key=lambda item: len(item[1]["asset_pattern"]), reverse=True)
    for target, info in ordered:
        pattern = info["asset_pattern"]
        if asset_name in {f"{pattern}.bin", f"{pattern}-{release_tag}.bin"}:
            return target
    return None


def _decode_app_desc_field(data: bytes) -> str | None:
    value = data.split(b"\x00", 1)[0]
    try:
        text = value.decode("ascii")
    except UnicodeDecodeError:
        return None
    if any(ord(char) < 0x20 or ord(char) > 0x7E for char in text):
        return None
    return text


def _parse_app_desc_bytes(image: bytes) -> dict | None:
    """Parse esp_app_desc_t from an ESP-IDF app image, failing closed."""
    if len(image) < _APP_DESC_OFFSET + _APP_DESC_MIN_SIZE:
        return None
    if image[0] != _ESP_IMAGE_MAGIC:
        return None

    desc = image[_APP_DESC_OFFSET:_APP_DESC_OFFSET + _APP_DESC_MIN_SIZE]
    if struct.unpack_from("<I", desc)[0] != _APP_DESC_MAGIC:
        return None

    version = _decode_app_desc_field(desc[16:48])
    project = _decode_app_desc_field(desc[48:80])
    build_time = _decode_app_desc_field(desc[80:96])
    build_date = _decode_app_desc_field(desc[96:112])
    if None in (version, project, build_time, build_date):
        return None
    if not version or version != version.strip() or any(char.isspace() for char in version):
        return None

    return {
        "version": version,
        "project": project,
        "time": build_time,
        "date": build_date,
    }


def _parse_app_desc(bin_path: Path) -> dict | None:
    """Parse esp_app_desc_t from a firmware path via the bytes parser."""
    try:
        return _parse_app_desc_bytes(bin_path.read_bytes())
    except OSError:
        return None


@dataclass
class FirmwareAsset:
    name: str
    description: str
    release_tag: str
    size: int
    download_url: str
    cached_path: str | None = None
    cached_at: float = 0


class FirmwareManager:
    def __init__(self):
        self.assets: dict[str, FirmwareAsset] = {}
        self.release_tag: str = ""
        self.last_check: float = 0
        self._custom_firmware: dict[str, bytes] = {}  # name → binary (uploaded overrides)
        self._image_version_cache: OrderedDict[bytes, str | None] = OrderedDict()
        CACHE_DIR.mkdir(parents=True, exist_ok=True)

    def _version_from_image(self, image: bytes) -> str | None:
        identity = hashlib.sha256(image).digest()
        if identity in self._image_version_cache:
            version = self._image_version_cache.pop(identity)
            self._image_version_cache[identity] = version
            return version

        desc = _parse_app_desc_bytes(image)
        version = desc["version"] if desc else None
        self._image_version_cache[identity] = version
        while len(self._image_version_cache) > IMAGE_VERSION_CACHE_SIZE:
            self._image_version_cache.popitem(last=False)
        return version

    async def refresh_from_github(self, force: bool = False):
        """Check GitHub for latest release and update asset catalog."""
        now = time.time()
        if not force and (now - self.last_check) < CACHE_TTL_S and self.assets:
            return

        try:
            async with httpx.AsyncClient(timeout=10.0) as client:
                r = await client.get(GITHUB_API)
                if r.status_code != 200:
                    logger.warning("GitHub API returned %d", r.status_code)
                    return

                releases = r.json()
                if not isinstance(releases, list) or len(releases) == 0:
                    logger.warning("No GitHub releases found")
                    return

                # A successful release-list response counts as a completed
                # refresh even when it contains no supported firmware. Keep
                # the existing catalog without hammering GitHub until TTL.
                self.last_check = now

                # Releases are newest first, but each target may have an
                # independently published newest release.
                new_assets: dict[str, FirmwareAsset] = {}
                selected_tags: list[str] = []
                for fw_name, fw_info in FIRMWARE_TYPES.items():
                    for release in releases:
                        if release.get("draft", False):
                            continue
                        tag = release.get("tag_name", "")
                        asset = next(
                            (
                                item for item in release.get("assets", [])
                                if _asset_target(item.get("name", ""), tag) == fw_name
                            ),
                            None,
                        )
                        if asset is None:
                            continue
                        cached = CACHE_DIR / f"{tag}_{fw_name}.bin"
                        new_assets[fw_name] = FirmwareAsset(
                            name=fw_name,
                            description=fw_info["description"],
                            release_tag=tag,
                            size=asset["size"],
                            download_url=asset["browser_download_url"],
                            cached_path=str(cached) if cached.exists() else None,
                        )
                        selected_tags.append(tag)
                        break

                if not new_assets:
                    logger.warning("No GitHub release with supported firmware found")
                    return
                self.release_tag = selected_tags[0]
                self.assets = new_assets
                logger.info("Firmware catalog: %d types available", len(new_assets))

        except Exception as e:
            logger.warning("Failed to check GitHub releases: %s", e)

    async def get_firmware_binary(self, name: str) -> bytes | None:
        """Get firmware binary by name. Prefers custom upload → local build → GitHub."""
        # Custom upload overrides everything
        if name in self._custom_firmware:
            data = self._custom_firmware[name]
            return data if self.validate_firmware_image(name, data) else None

        # Local .pio build (present when running backend from the repo with fresh builds)
        fw_info = FIRMWARE_TYPES.get(name)
        if fw_info:
            local_bin = fw_info.get("local_bin")
            if local_bin and local_bin.exists():
                try:
                    data = local_bin.read_bytes()
                    logger.info("Serving %s from local build (%s, %d bytes)", name, local_bin, len(data))
                    return data if self.validate_firmware_image(name, data) else None
                except Exception as e:
                    logger.warning("Failed reading local bin %s: %s", local_bin, e)

        await self.refresh_from_github()
        asset = self.assets.get(name)
        if not asset:
            return None

        # Check cache
        cached = CACHE_DIR / f"{asset.release_tag}_{name}.bin"
        if cached.exists():
            try:
                data = cached.read_bytes()
                asset.cached_path = str(cached)
                return data if self.validate_firmware_image(name, data) else None
            except OSError as e:
                logger.warning("Failed reading cached firmware %s: %s", cached, e)
                asset.cached_path = None
                asset.cached_at = 0
        else:
            asset.cached_path = None
            asset.cached_at = 0

        # Download from GitHub
        try:
            async with httpx.AsyncClient(timeout=60.0, follow_redirects=True) as client:
                logger.info("Downloading %s from %s", name, asset.download_url)
                r = await client.get(asset.download_url)
                if r.status_code == 200:
                    data = r.content
                    if not self.validate_firmware_image(name, data):
                        logger.error("Downloaded firmware %s failed validation", name)
                        return None
                    cached.write_bytes(data)
                    asset.cached_path = str(cached)
                    asset.cached_at = time.time()
                    logger.info("Cached %s: %d bytes", name, len(data))
                    return data
                else:
                    logger.error("Download failed: %d", r.status_code)
                    return None
        except Exception as e:
            logger.error("Download failed for %s: %s", name, e)
            return None

    async def get_firmware_version(self, name: str) -> str | None:
        """Return the version for the firmware image that get_firmware_binary serves."""
        if name in self._custom_firmware and not name.endswith("-backend"):
            return "custom"

        image = await self.get_firmware_binary(name)
        return self._version_from_image(image) if image is not None else None

    def validate_firmware_image(self, name: str, image: bytes) -> bool:
        if name.endswith("-backend"):
            return _validated_backend_image_info(name, image) is not None
        return True

    def set_custom_firmware(self, name: str, data: bytes):
        """Upload a custom firmware binary (overrides GitHub for testing)."""
        self._custom_firmware[name] = data
        logger.info("Custom firmware set: %s (%d bytes)", name, len(data))

    def clear_custom_firmware(self, name: str):
        """Remove a custom firmware override."""
        self._custom_firmware.pop(name, None)

    async def get_catalog(self) -> list[dict]:
        """Return available firmware catalog for dashboard.

        Precedence per entry: custom upload → local build → GitHub release.
        """
        await self.refresh_from_github()
        result = []
        for fw_name, fw_info in FIRMWARE_TYPES.items():
            asset = self.assets.get(fw_name)
            is_custom = fw_name in self._custom_firmware
            local_bin: Path | None = fw_info.get("local_bin")
            local_present = bool(local_bin and local_bin.is_file())

            # Figure out which source we'd actually serve + its version
            version = None
            size = None
            available = False
            source = "unavailable"
            if is_custom:
                source = "custom"
                image = await self.get_firmware_binary(fw_name)
                if image is not None:
                    version = (
                        self._version_from_image(image)
                        if fw_name.endswith("-backend")
                        else "custom"
                    )
                    size = len(image)
                    available = True
            elif local_present or asset:
                image = await self.get_firmware_binary(fw_name)
                source = "local" if local_present else "github"
                if image is not None:
                    version = self._version_from_image(image)
                    size = len(image)
                    available = True

            cached = False
            if source == "local":
                cached = available and local_present
            elif source == "github" and available and asset and asset.cached_path:
                cached = Path(asset.cached_path).is_file()

            result.append({
                "name": fw_name,
                "target": fw_name,
                "description": fw_info["description"],
                "board": fw_info["board"],
                "project": fw_info.get("project"),
                "hardware": fw_info.get("hardware"),
                "version": version,
                "size": size,
                "available": available,
                "source": source,
                "cached": cached,
            })
        return result
