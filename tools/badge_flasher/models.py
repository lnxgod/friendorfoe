"""Immutable value objects shared by factory-flasher policy modules."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Mapping


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
