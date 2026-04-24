"""Firmware management — fetch from GitHub releases, cache, serve to dashboard.

Pulls firmware binaries from the latest GitHub release, caches them locally,
and serves them for OTA push to ESP32 nodes.
"""

import logging
import os
import struct
import time
from dataclasses import dataclass, field
from pathlib import Path

import httpx

logger = logging.getLogger(__name__)

GITHUB_REPO = "lnxgod/friendorfoe"
# Use /releases (not /releases/latest) to include prereleases
GITHUB_API = f"https://api.github.com/repos/{GITHUB_REPO}/releases"
CACHE_DIR = Path("/tmp/fof-firmware")
CACHE_TTL_S = 1800  # Re-check GitHub every 30 minutes

# Repo root relative to backend/app/services/firmware_manager.py
_REPO_ROOT = Path(__file__).resolve().parents[3]

# Map firmware names to their expected asset filename patterns + local build dir
FIRMWARE_TYPES = {
    "uplink-esp32": {
        "description": "Uplink node (ESP32 OLED)",
        "asset_pattern": "uplink-esp32",
        "board": "esp32dev",
        "local_bin": _REPO_ROOT / "esp32/uplink/.pio/build/uplink-esp32/firmware.bin",
    },
    "uplink-c3": {
        "description": "Uplink node (ESP32-C3)",
        "asset_pattern": "uplink-c3",
        "board": "esp32-c3",
        "local_bin": _REPO_ROOT / "esp32/uplink/.pio/build/uplink/firmware.bin",
    },
    "uplink-s3": {
        "description": "Uplink node (ESP32-S3 N16R8)",
        "asset_pattern": "uplink-s3",
        "board": "esp32s3",
        "local_bin": _REPO_ROOT / "esp32/uplink/.pio/build/uplink-s3/firmware.bin",
    },
    "scanner-s3-combo-seed": {
        "description": "BLE + WiFi scanner (ESP32-S3 Seed/Mini N8R8)",
        "asset_pattern": "scanner-s3-combo-seed",
        "board": "esp32s3",
        "local_bin": _REPO_ROOT / "esp32/scanner/.pio/build/scanner-s3-combo-seed/firmware.bin",
    },
    "scanner-s3-combo": {
        "description": "BLE + WiFi scanner (ESP32-S3)",
        "asset_pattern": "scanner-s3-combo",
        "board": "esp32s3",
        "local_bin": _REPO_ROOT / "esp32/scanner/.pio/build/scanner-s3-combo/firmware.bin",
    },
    "scanner-s3-legacy": {
        "description": "BLE + WiFi scanner (S3 for legacy OLED uplink)",
        "asset_pattern": "scanner-s3-legacy",
        "board": "esp32s3",
        "local_bin": _REPO_ROOT / "esp32/scanner/.pio/build/scanner-s3-legacy/firmware.bin",
    },
    "scanner-s3-wifi": {
        "description": "WiFi-only scanner (ESP32-S3)",
        "asset_pattern": "scanner-s3-wifi",
        "board": "esp32s3",
        "local_bin": _REPO_ROOT / "esp32/scanner/.pio/build/scanner-s3-wifi/firmware.bin",
    },
    "scanner-esp32": {
        "description": "WiFi-only scanner (ESP32)",
        "asset_pattern": "scanner-esp32",
        "board": "esp32dev",
        "local_bin": _REPO_ROOT / "esp32/scanner/.pio/build/scanner-esp32/firmware.bin",
    },
    "scanner-c5": {
        "description": "2.4+5GHz WiFi scanner (ESP32-C5)",
        "asset_pattern": "scanner-c5",
        "board": "esp32c5",
        "local_bin": _REPO_ROOT / "esp32/scanner/.pio/build/scanner-c5/firmware.bin",
    },
}


def _parse_app_desc(bin_path: Path) -> dict | None:
    """Read esp_app_desc_t at offset 0x20 of an ESP-IDF app image. Returns
    {version, project, date, time} or None on failure."""
    try:
        with bin_path.open("rb") as f:
            f.seek(0x20)
            d = f.read(256)
        if len(d) < 112 or struct.unpack("<I", d[0:4])[0] != 0xABCD5432:
            return None
        return {
            "version": d[16:48].rstrip(b"\x00").decode(errors="replace").strip(),
            "project": d[48:80].rstrip(b"\x00").decode(errors="replace").strip(),
            "time": d[80:96].rstrip(b"\x00").decode(errors="replace").strip(),
            "date": d[96:112].rstrip(b"\x00").decode(errors="replace").strip(),
        }
    except Exception:
        return None


@dataclass
class FirmwareAsset:
    name: str
    description: str
    version: str
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
        CACHE_DIR.mkdir(parents=True, exist_ok=True)

    async def refresh_from_github(self, force: bool = False):
        """Check GitHub for latest release and update asset catalog."""
        now = time.time()
        if not force and (now - self.last_check) < CACHE_TTL_S and self.assets:
            return

        self.last_check = now
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

                # Use the first release (most recent, includes prereleases)
                data = releases[0]
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
                                version=tag,
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
        cached = CACHE_DIR / f"{asset.version}_{name}.bin"
        if cached.exists():
            return cached.read_bytes()

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
            local_present = bool(local_bin and local_bin.exists())

            # Figure out which source we'd actually serve + its version
            version = None
            size = None
            source = "unavailable"
            if is_custom:
                version = "custom"
                size = len(self._custom_firmware[fw_name])
                source = "custom"
            elif local_present:
                desc = _parse_app_desc(local_bin)
                version = desc["version"] if desc else "local"
                try:
                    size = local_bin.stat().st_size
                except OSError:
                    size = None
                source = "local"
            elif asset:
                version = asset.version
                size = asset.size
                source = "github"

            result.append({
                "name": fw_name,
                "description": fw_info["description"],
                "board": fw_info["board"],
                "version": version,
                "size": size,
                "available": is_custom or local_present or asset is not None,
                "source": source,
                "cached": (asset.cached_path is not None) if asset else local_present,
            })
        return result
