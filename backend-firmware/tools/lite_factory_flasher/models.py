"""Immutable Lite factory value objects."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Mapping, Sequence

from tools.badge_flasher.models import FlashEvidence, TopologyAssignment


@dataclass(frozen=True, slots=True)
class PassedLiteFactoryRecord:
    """Authoritative evidence for one previously accepted Lite graph."""

    version: str
    bundle_sha256: str
    assignment: TopologyAssignment


@dataclass(frozen=True, slots=True)
class LiteRuntimeSnapshot:
    """One same-boot, two-sample runtime proof."""

    status: Mapping[str, Any]
    config: Mapping[str, Any]
    boot: Mapping[str, Any]
    health: Mapping[str, Any]


@dataclass(frozen=True, slots=True)
class LiteBatchResult:
    unit_id: str
    version: str
    scanner_version: str
    bundle_sha256: str
    passed: bool
    phase: str
    assignment: TopologyAssignment
    devices: Sequence[FlashEvidence]
    runtime: Mapping[str, Any]
    receipt: str | None
    error: str | None = None
