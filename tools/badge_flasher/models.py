"""Immutable value objects shared by factory-flasher policy modules."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Mapping


@dataclass(frozen=True, slots=True)
class ProbeReport:
    """One factory probe's view of its two physical UART links."""

    mac: str
    session: str
    peers: Mapping[str, str]


@dataclass(frozen=True, slots=True)
class TopologyAssignment:
    """MAC assignment derived from the badge's reciprocal UART star."""

    uplink_mac: str
    ble_leaf_mac: str
    wifi_leaf_mac: str


@dataclass(frozen=True, slots=True)
class UsbDevice:
    """One ESP32 target, tracked by immutable eFuse MAC across resets."""

    mac: str
    port: str
    chip: str
    revision: str
    flash_size: str
    psram_size: str
    location_id: str | None = None


@dataclass(frozen=True, slots=True)
class FlashEvidence:
    mac: str
    role: str
    port: str
    version: str
    write_verified: bool
    readback_verified: bool


@dataclass(frozen=True, slots=True)
class BatchResult:
    badge_id: str
    version: str
    bundle_sha256: str
    passed: bool
    phase: str
    assignment: TopologyAssignment
    devices: tuple[FlashEvidence, ...]
    runtime: Mapping[str, Any]
    error: str | None = None
