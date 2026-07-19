"""Firmware management — fetch from GitHub releases, cache, serve to dashboard.

Pulls firmware binaries from the latest GitHub release, caches them locally,
and serves them for OTA push to ESP32 nodes.
"""

import hashlib
import logging
import struct
import time
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
}


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

                # GitHub returns releases newest first. APK-only releases must
                # not replace the firmware catalog with an empty one.
                data = next(
                    (
                        release
                        for release in releases
                        if not release.get("draft", False)
                        and any(
                            a["name"].lower().endswith(".bin")
                            and any(
                                fw_info["asset_pattern"] in a["name"].lower()
                                for fw_info in FIRMWARE_TYPES.values()
                            )
                            for a in release.get("assets", [])
                        )
                    ),
                    None,
                )
                if data is None:
                    logger.warning("No GitHub release with supported firmware found")
                    return

                tag = data.get("tag_name", "")
                if tag == self.release_tag and self.assets:
                    return  # No change

                self.release_tag = tag
                gh_assets = data.get("assets", [])

                logger.info("GitHub release: %s with %d assets", tag, len(gh_assets))

                new_assets = {}
                used_assets = set()
                fw_items = sorted(
                    FIRMWARE_TYPES.items(),
                    key=lambda item: len(item[1]["asset_pattern"]),
                    reverse=True,
                )
                for fw_name, fw_info in fw_items:
                    pattern = fw_info["asset_pattern"]
                    # Find matching asset
                    for a in gh_assets:
                        aname = a["name"].lower()
                        if a["name"] in used_assets:
                            continue
                        if pattern in aname and aname.endswith(".bin"):
                            cached = CACHE_DIR / f"{tag}_{fw_name}.bin"
                            new_assets[fw_name] = FirmwareAsset(
                                name=fw_name,
                                description=fw_info["description"],
                                release_tag=tag,
                                size=a["size"],
                                download_url=a["browser_download_url"],
                                cached_path=str(cached) if cached.exists() else None,
                            )
                            used_assets.add(a["name"])
                            break

                self.assets = new_assets
                logger.info("Firmware catalog: %d types available", len(new_assets))

        except Exception as e:
            logger.warning("Failed to check GitHub releases: %s", e)

    async def get_firmware_binary(self, name: str) -> bytes | None:
        """Get firmware binary by name. Prefers custom upload → local build → GitHub."""
        # Custom upload overrides everything
        if name in self._custom_firmware:
            return self._custom_firmware[name]

        # Local .pio build (present when running backend from the repo with fresh builds)
        fw_info = FIRMWARE_TYPES.get(name)
        if fw_info:
            local_bin = fw_info.get("local_bin")
            if local_bin and local_bin.exists():
                try:
                    data = local_bin.read_bytes()
                    logger.info("Serving %s from local build (%s, %d bytes)", name, local_bin, len(data))
                    return data
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
                return data
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
        if name in self._custom_firmware:
            return "custom"

        image = await self.get_firmware_binary(name)
        return self._version_from_image(image) if image is not None else None

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
                version = "custom"
                size = len(self._custom_firmware[fw_name])
                available = True
                source = "custom"
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
                "description": fw_info["description"],
                "board": fw_info["board"],
                "version": version,
                "size": size,
                "available": available,
                "source": source,
                "cached": cached,
            })
        return result
