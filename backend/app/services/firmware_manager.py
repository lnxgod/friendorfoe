"""Firmware management — fetch from GitHub releases, cache, serve to dashboard.

Pulls firmware binaries from the latest GitHub release, caches them locally,
and serves them for OTA push to ESP32 nodes.
"""

import logging
import os
import time
from dataclasses import dataclass, field
from pathlib import Path

import httpx

logger = logging.getLogger(__name__)

GITHUB_REPO = "lnxgod/friendorfoe"
GITHUB_API = f"https://api.github.com/repos/{GITHUB_REPO}/releases/latest"
CACHE_DIR = Path("/tmp/fof-firmware")
CACHE_TTL_S = 1800  # Re-check GitHub every 30 minutes

# Map firmware names to their expected asset filename patterns
FIRMWARE_TYPES = {
    "uplink-esp32": {
        "description": "Uplink node (ESP32 OLED)",
        "asset_pattern": "uplink-esp32",
        "board": "esp32dev",
    },
    "uplink-c3": {
        "description": "Uplink node (ESP32-C3)",
        "asset_pattern": "uplink-c3",
        "board": "esp32-c3",
    },
    "scanner-s3-combo": {
        "description": "BLE + WiFi scanner (ESP32-S3)",
        "asset_pattern": "scanner-s3-combo",
        "board": "esp32s3",
    },
    "scanner-esp32": {
        "description": "WiFi-only scanner (ESP32)",
        "asset_pattern": "scanner-esp32",
        "board": "esp32dev",
    },
    "scanner-c5": {
        "description": "2.4+5GHz WiFi scanner (ESP32-C5)",
        "asset_pattern": "scanner-c5",
        "board": "esp32c5",
    },
}


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

                data = r.json()
                tag = data.get("tag_name", "")
                if tag == self.release_tag and self.assets:
                    return  # No change

                self.release_tag = tag
                gh_assets = data.get("assets", [])

                logger.info("GitHub release: %s with %d assets", tag, len(gh_assets))

                new_assets = {}
                for fw_name, fw_info in FIRMWARE_TYPES.items():
                    pattern = fw_info["asset_pattern"]
                    # Find matching asset
                    for a in gh_assets:
                        aname = a["name"].lower()
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
                            break

                self.assets = new_assets
                logger.info("Firmware catalog: %d types available", len(new_assets))

        except Exception as e:
            logger.warning("Failed to check GitHub releases: %s", e)

    async def get_firmware_binary(self, name: str) -> bytes | None:
        """Get firmware binary by name. Downloads from GitHub if not cached."""
        # Custom upload overrides GitHub
        if name in self._custom_firmware:
            return self._custom_firmware[name]

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
        """Return available firmware catalog for dashboard."""
        await self.refresh_from_github()
        result = []
        for fw_name, fw_info in FIRMWARE_TYPES.items():
            asset = self.assets.get(fw_name)
            is_custom = fw_name in self._custom_firmware
            result.append({
                "name": fw_name,
                "description": fw_info["description"],
                "board": fw_info["board"],
                "version": asset.version if asset else None,
                "size": len(self._custom_firmware[fw_name]) if is_custom else (asset.size if asset else None),
                "available": asset is not None or is_custom,
                "source": "custom" if is_custom else ("github" if asset else "unavailable"),
                "cached": asset.cached_path is not None if asset else False,
            })
        return result
