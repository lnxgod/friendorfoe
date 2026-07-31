#!/usr/bin/env python3
"""Privacy-safe physical acceptance evidence for hardened DEF CON badges.

This deliberately keeps destructive acceptance actions out of the normal
factory flasher.  The only automated fault injection here is the specified
65,536-byte interrupted uplink application upload.
"""

from __future__ import annotations

import argparse
import binascii
import copy
import ctypes
import fcntl
import hashlib
import json
import os
import pwd
import re
import secrets
import stat
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

if __package__:
    from . import fof_badge_flash as flash
else:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import fof_badge_flash as flash


class AcceptanceError(RuntimeError):
    """A physical release fact is absent, malformed, or contradictory."""


REQUIRED_GATES = (
    "android-control-reconnect",
    "three-update-cycles",
    "interrupted-upload",
    "chord-rom-recovery",
    "no-host-reboot",
    "power-state-audit",
)
_MANUAL_PASS_PREFIXES = {
    "android-control-reconnect": (),
    "no-host-reboot": (
        ("android-control-reconnect", "PASS"),
        ("three-update-cycles", "CHECKPOINT"),
        ("three-update-cycles", "CHECKPOINT"),
        ("three-update-cycles", "CHECKPOINT"),
        ("three-update-cycles", "PASS"),
        ("interrupted-upload", "PASS"),
        ("chord-rom-recovery", "PASS"),
    ),
    "power-state-audit": (
        ("android-control-reconnect", "PASS"),
        ("three-update-cycles", "CHECKPOINT"),
        ("three-update-cycles", "CHECKPOINT"),
        ("three-update-cycles", "CHECKPOINT"),
        ("three-update-cycles", "PASS"),
        ("interrupted-upload", "PASS"),
        ("chord-rom-recovery", "PASS"),
        ("no-host-reboot", "PASS"),
    ),
}

INTERRUPTED_UPLOAD_BYTES = 65_536
INTERRUPTED_UPLOAD_IDLE_WAIT_S = 7
INTERRUPTED_UPLOAD_RECEIPT_TIMEOUT_S = 120
EVIDENCE_SCHEMA = 1
MAX_EVIDENCE_BYTES = 16 * 1024 * 1024
MAX_EVIDENCE_RECORD_BYTES = 32 * 1024
MUTATING_GATE_RESERVATION_SCHEMA = 1
RETAINED_OPERATION_MARKER_SCHEMA = 2
OPERATION_REGISTRY_SCHEMA = 3
OPERATION_REGISTRY_NAME = (
    ".fof-badge-acceptance-operation-registry-v3.jsonl"
)
MAX_OPERATION_REGISTRY_BYTES = 16 * 1024 * 1024
MAX_SESSION_FILE_BYTES = 32 * 1024
DEFAULT_EVIDENCE = (
    Path(__file__).resolve().parents[1]
    / "artifacts/badge-usb-hardening/acceptance.jsonl"
)
CANARY_PLATFORM_KEY = "badge-trio-xiao-s3-con-crud-canary"
_LEGACY_V078_GENERATION_CAPABILITY = "legacy-v078-absent"
_V078_PRE_MUTATION_RX_DELTA = 34
_V078_PRE_MUTATION_COMMAND_DELTA = 3
_V078_DETECTION_QUEUE_CAPACITY = 48
_STATUS_COMMAND_RX_DELTA = 12

_SESSION_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
_TIMESTAMP_UTC_RE = re.compile(
    r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$"
)
_MAC_RE = re.compile(
    r"(?i)(?<![0-9a-f])"
    r"(?:(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}|[0-9a-f]{12})"
    r"(?![0-9a-f])"
)
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
_FORBIDDEN_FACT_KEY_PARTS = (
    "ssid",
    "bssid",
    "nearby",
    "remote_id",
    "ambient",
    "detection_payload",
    "detections",
    "advertisement",
    "observed_device",
)
_FAIL_ERROR_CODES = frozenset({
    "operator_gate_failed",
    "device_disconnected",
    "identity_mismatch",
    "version_mismatch",
    "transport_failure",
    "validation_failure",
    "hardware_gate_failed",
})
_FAIL_PHASE_CODES = frozenset({
    "android_control",
    "update_cycle_1",
    "update_cycle_2",
    "update_cycle_3",
    "interrupted_upload",
    "chord_recovery",
    "no_host_reboot",
    "power_audit",
    "bootstrap",
})
_SAFE_EXPECTED_REBOOT_REASONS = frozenset({
    "",
    "button_reboot",
    "button_usb_rom",
    "http_ota",
    "http_reboot",
    "http_wifi_config",
    "update_abort",
    "update_finish",
    "usb_bootloader",
    "usb_reboot",
    "usb_rollback",
    "usb_safe_once",
    "usb_uplink_ota",
})
_EXPECTED_REBOOT_MAGIC = 0xF0F0B007
_UINT32_MAX = 0xFFFFFFFF
_INTERNAL_HEAP_MAX_BYTES = 512 * 1024
_LIVE_METRIC_FIELDS = (
    "stack_main_free",
    "stack_display_free",
    "stack_usb_free",
    "stack_uart_ble_free",
    "stack_uart_wifi_free",
    "heap_internal_free",
    "heap_internal_min_free",
    "heap_internal_largest",
    "detection_queue_capacity",
)
_STACK_METRIC_FIELDS = _LIVE_METRIC_FIELDS[:5]
_PRODUCTION_STACK_METRIC_MAXIMUMS = {
    "stack_main_free": 12288,
    "stack_display_free": 12288,
    "stack_usb_free": 16384,
    "stack_uart_ble_free": 8192,
    "stack_uart_wifi_free": 8192,
}
_CANARY_STACK_METRIC_MAXIMUMS = {
    **_PRODUCTION_STACK_METRIC_MAXIMUMS,
    "stack_usb_free": 20480,
    "stack_uart_ble_free": 9216,
    "stack_uart_wifi_free": 9216,
}
_V078_BASELINE_KEYS = frozenset({
    "source_version",
    "session_id",
    "hardware_id",
    "recovery_mode",
    "update_session",
    "reboot_generation_capability",
    "source_partition",
    "pre_uptime_s",
    "uptime_s",
    "pre_reboot_generation",
    "pre_rx_bytes",
    "pre_valid_commands",
    "pre_responses_completed",
    "reboot_generation",
    "rx_bytes",
    "valid_commands",
    "responses_completed",
    *_LIVE_METRIC_FIELDS,
})
_LIVE_SAMPLE_KEYS = frozenset({
    "version",
    "session_id",
    "hardware_id",
    "recovery_mode",
    "update_session",
    "reboot_generation_capability",
    "running_partition",
    "uptime_s",
    "reboot_generation",
    "rx_bytes",
    "valid_commands",
    "responses_completed",
    *_LIVE_METRIC_FIELDS,
})
_LIVE_METRIC_ISSUER = object()
_USB_COUNTER_FIELDS = (
    "rx_bytes",
    "valid_commands",
    "responses_completed",
    "required_response_failures",
    "malformed_lines",
    "dropped_progress_frames",
    "dropped_optional_frames",
    "upload_received",
    "upload_size",
)
_USB_AGE_FIELDS = (
    "task_heartbeat_age_s",
    "last_rx_age_s",
    "last_command_age_s",
    "last_response_age_s",
    "last_upload_progress_age_s",
)
_SNAPSHOT_KEYS = frozenset({
    "version",
    "reboot_generation_capability",
    "reboot_generation",
    "uplink_hardware_id",
    "ble_hardware_id",
    "wifi_hardware_id",
    "running_partition",
    "uptime_s",
    "rollback_clear",
    "recovery_mode",
    "usb_parser_state",
    "usb_rx_bytes",
    "usb_valid_commands",
    "usb_responses_completed",
    "usb_required_response_failures",
    "usb_task_heartbeat_age_s",
    "usb_last_response_age_s",
    "ble_role",
    "wifi_role",
    "radio_health",
    "last_expected_reboot_reason",
})
_SNAPSHOT_ISSUER = object()
_CHORD_ROM_BOOT_SNAPSHOT_ISSUER = object()
_CANDIDATE_ARTIFACT_KEYS = frozenset({
    "schema",
    "platform_key",
    "version",
    "uplink",
    "scanner",
})
_CANDIDATE_ARTIFACT_ROLE_KEYS = frozenset({
    "content_set_sha256",
    "firmware_size",
    "firmware_crc32",
    "firmware_sha256",
})
_CANDIDATE_ARTIFACT_SCHEMA = 3
_CANDIDATE_CONTENT_SET_DOMAIN = (
    b"friend-or-foe/badge-acceptance/candidate-content-set/v1\x00"
)
_CANDIDATE_ARTIFACT_ISSUER = object()
_CHORD_RECOVERY_FACT_KEYS = frozenset({
    "snapshot",
    "rom_boot_snapshot",
    "candidate_artifacts",
    "usb_data_host_attached",
    "hold_ms",
    "rom_enumerated",
    "base_mac_continuity",
    "full_layout_verified",
    "application_returned",
    "scanner_staged_once",
    "both_uart_updates",
    "last_expected_reboot_reason",
})
_CHORD_RECOVERY_FACT_ISSUER = object()
_CYCLE_PRE_SNAPSHOT_KEYS = frozenset({
    "candidate_version",
    "uplink_version",
    "reboot_generation_capability",
    "reboot_generation",
    "uplink_hardware_id",
    "ble_hardware_id",
    "wifi_hardware_id",
    "ble_version",
    "wifi_version",
    "running_partition",
    "uptime_s",
    "rollback_clear",
    "recovery_mode",
    "usb_parser_state",
    "usb_rx_bytes",
    "usb_valid_commands",
    "usb_responses_completed",
    "usb_required_response_failures",
    "usb_task_heartbeat_age_s",
    "usb_last_response_age_s",
    "ble_role",
    "wifi_role",
    "radio_health",
})
_CYCLE_SNAPSHOT_ISSUER = object()
_CYCLE_CHECKPOINT_KEYS = frozenset({
    "cycle",
    "candidate_version",
    "candidate_artifacts",
    "updater_baseline",
    "pre_snapshot",
    "snapshot",
    "recovery_rewrite_same_version",
    "scanner_uploads",
    "manual_relay_commands",
    "stage_generation",
    "slot_mask",
    "scanner_flow_control",
    "scanner_stage_phase",
    "scanner_stage_received",
    "scanner_stage_total",
    "scanner_stage_crc32",
    "scanner_stage_sha256",
    "scanner_stage_credit_bytes",
    "pending_mask_after",
    "preflight_older_slot_mask",
    "recovery_slot_mask",
    "ble_state",
    "ble_attempts",
    "wifi_state",
    "wifi_attempts",
    "fresh_ping_status",
    "theme_restored",
})
_CYCLE_CHECKPOINT_ISSUER = object()
_THREE_CYCLE_AGGREGATE_KEYS = frozenset({
    "snapshot",
    "candidate_version",
    "candidate_artifacts",
    "cycles_completed",
    "strictly_older_setup",
    "automatic_convergence",
    "checkpoint_generations",
    "first_cycle_manual_relay_commands",
    "recovery_manual_relay_commands",
})
_THREE_CYCLE_AGGREGATE_ISSUER = object()
_EVIDENCE_RECORD_KEYS = frozenset({
    "schema",
    "timestamp_utc",
    "session_id",
    "gate",
    "status",
    "uplink_hardware_id",
    "ble_hardware_id",
    "wifi_hardware_id",
    "facts",
})


@dataclass(frozen=True)
class _DurableGateReservation:
    path: Path
    fd: int
    parent_fd: int
    parent_path: Path
    parent_device: int
    parent_inode: int
    encoded: bytes
    evidence_path: Path
    evidence_fd: int
    evidence_device: int
    evidence_inode: int
    session_id: str
    gate: str
    phase: str
    cycle: int | None


@dataclass(frozen=True)
class _PrivateSessionInput:
    path: Path
    fd: int
    device: int
    inode: int
    encoded: bytes
    parent_bindings: tuple[_StateDirectoryBinding, ...]

    @property
    def parent_fd(self) -> int:
        return self.parent_bindings[-1].fd


@dataclass(frozen=True)
class _StateDirectoryBinding:
    name: str | None
    fd: int
    device: int
    inode: int


@dataclass(frozen=True)
class _RetainedStateRoot:
    bindings: tuple[_StateDirectoryBinding, ...]

    @property
    def fd(self) -> int:
        return self.bindings[-1].fd


@dataclass(frozen=True)
class _RetainedOperationMarker:
    name: str
    fd: int
    device: int | None
    inode: int | None
    encoded: bytes
    state_root: _RetainedStateRoot


@dataclass(frozen=True)
class _OperationRegistry:
    name: str
    fd: int
    device: int | None
    inode: int | None
    parent_bindings: tuple[_StateDirectoryBinding, ...]

    @property
    def parent_fd(self) -> int:
        return self.parent_bindings[-1].fd


@dataclass(frozen=True)
class _AnchoredCandidate:
    version: str
    artifacts: VerifiedCandidateArtifacts
    updater_baseline: VerifiedV078UpdaterBaseline | None = None


@dataclass(frozen=True)
class _MutatingGateContext:
    version: str
    artifacts: VerifiedCandidateArtifacts
    frozen: flash.FrozenUsbFirmwareArtifacts
    updater_baseline: VerifiedV078UpdaterBaseline | None = None


class VerifiedBadgeSnapshot(dict[str, object]):
    """Process-issued immutable proof; caller-authored JSON is not sufficient."""

    def __init__(self, issuer: object, values: dict[str, object]) -> None:
        if issuer is not _SNAPSHOT_ISSUER or set(values) != _SNAPSHOT_KEYS:
            raise TypeError("VerifiedBadgeSnapshot is verifier-issued only")
        dict.__init__(self, values)

    @staticmethod
    def _immutable(*_args: object, **_kwargs: object) -> None:
        raise TypeError("verified badge snapshots are immutable")

    __setitem__ = _immutable
    __delitem__ = _immutable
    clear = _immutable
    pop = _immutable
    popitem = _immutable
    setdefault = _immutable
    update = _immutable
    __ior__ = _immutable


class VerifiedChordRomBootSnapshot(dict[str, object]):
    """Verifier-issued post-ROM proof before maintenance overwrites reason."""

    def __init__(self, issuer: object, values: dict[str, object]) -> None:
        if issuer is not _CHORD_ROM_BOOT_SNAPSHOT_ISSUER or \
                set(values) != _SNAPSHOT_KEYS:
            raise TypeError("chord ROM boot snapshots are verifier-issued")
        dict.__init__(self, values)

    @staticmethod
    def _immutable(*_args: object, **_kwargs: object) -> None:
        raise TypeError("chord ROM boot snapshots are immutable")

    __setitem__ = _immutable
    __delitem__ = _immutable
    clear = _immutable
    pop = _immutable
    popitem = _immutable
    setdefault = _immutable
    update = _immutable
    __ior__ = _immutable


class VerifiedV078UpdaterBaseline(dict[str, object]):
    """Exact live .78 updater comparison bound to one acceptance session."""

    def __init__(self, issuer: object, values: dict[str, object]) -> None:
        if issuer is not _LIVE_METRIC_ISSUER or \
                set(values) != _V078_BASELINE_KEYS:
            raise TypeError("v0.78 updater baseline is verifier-issued only")
        dict.__init__(self, values)

    @staticmethod
    def _immutable(*_args: object, **_kwargs: object) -> None:
        raise TypeError("v0.78 updater baselines are immutable")

    __setitem__ = _immutable
    __delitem__ = _immutable
    clear = _immutable
    pop = _immutable
    popitem = _immutable
    setdefault = _immutable
    update = _immutable
    __ior__ = _immutable


class VerifiedLiveMetricSample(dict[str, object]):
    """Exact live normal/maintenance resource sample."""

    def __init__(self, issuer: object, values: dict[str, object]) -> None:
        if issuer is not _LIVE_METRIC_ISSUER or \
                set(values) != _LIVE_SAMPLE_KEYS:
            raise TypeError("live metric samples are verifier-issued only")
        dict.__init__(self, values)

    @staticmethod
    def _immutable(*_args: object, **_kwargs: object) -> None:
        raise TypeError("live metric samples are immutable")

    __setitem__ = _immutable
    __delitem__ = _immutable
    clear = _immutable
    pop = _immutable
    popitem = _immutable
    setdefault = _immutable
    update = _immutable
    __ior__ = _immutable


class _VerifiedCandidateArtifactRole(dict[str, object]):
    """Immutable exact identity for one frozen candidate artifact tree."""

    def __init__(self, issuer: object, values: dict[str, object]) -> None:
        if issuer is not _CANDIDATE_ARTIFACT_ISSUER or \
                set(values) != _CANDIDATE_ARTIFACT_ROLE_KEYS:
            raise TypeError("candidate artifact role identity is verifier-only")
        dict.__init__(self, values)

    @staticmethod
    def _immutable(*_args: object, **_kwargs: object) -> None:
        raise TypeError("candidate artifact identities are immutable")

    __setitem__ = _immutable
    __delitem__ = _immutable
    clear = _immutable
    pop = _immutable
    popitem = _immutable
    setdefault = _immutable
    update = _immutable
    __ior__ = _immutable


class VerifiedCandidateArtifacts(dict[str, object]):
    """Verifier-issued exact identity for both frozen firmware trees."""

    def __init__(self, issuer: object, values: dict[str, object]) -> None:
        if issuer is not _CANDIDATE_ARTIFACT_ISSUER or \
                set(values) != _CANDIDATE_ARTIFACT_KEYS or \
                type(values.get("uplink")) is not \
                _VerifiedCandidateArtifactRole or \
                type(values.get("scanner")) is not \
                _VerifiedCandidateArtifactRole:
            raise TypeError("candidate artifact identity is verifier-only")
        dict.__init__(self, values)

    @staticmethod
    def _immutable(*_args: object, **_kwargs: object) -> None:
        raise TypeError("candidate artifact identities are immutable")

    __setitem__ = _immutable
    __delitem__ = _immutable
    clear = _immutable
    pop = _immutable
    popitem = _immutable
    setdefault = _immutable
    update = _immutable
    __ior__ = _immutable


class VerifiedChordRecoveryFacts(dict[str, object]):
    """Machine-issued Gate 4 proof; caller-authored booleans are insufficient."""

    def __init__(self, issuer: object, values: dict[str, object]) -> None:
        if issuer is not _CHORD_RECOVERY_FACT_ISSUER or \
                set(values) != _CHORD_RECOVERY_FACT_KEYS:
            raise TypeError("chord recovery facts are machine-issued only")
        dict.__init__(self, values)

    @staticmethod
    def _immutable(*_args: object, **_kwargs: object) -> None:
        raise TypeError("verified chord recovery facts are immutable")

    __setitem__ = _immutable
    __delitem__ = _immutable
    clear = _immutable
    pop = _immutable
    popitem = _immutable
    setdefault = _immutable
    update = _immutable
    __ior__ = _immutable


class VerifiedCyclePreSnapshot(dict[str, object]):
    """Verifier-issued pre-stage identity/version/radio proof."""

    def __init__(self, issuer: object, values: dict[str, object]) -> None:
        if issuer is not _CYCLE_SNAPSHOT_ISSUER or \
                set(values) != _CYCLE_PRE_SNAPSHOT_KEYS:
            raise TypeError("VerifiedCyclePreSnapshot is verifier-issued only")
        dict.__init__(self, values)

    @staticmethod
    def _immutable(*_args: object, **_kwargs: object) -> None:
        raise TypeError("verified cycle pre-snapshots are immutable")

    __setitem__ = _immutable
    __delitem__ = _immutable
    clear = _immutable
    pop = _immutable
    popitem = _immutable
    setdefault = _immutable
    update = _immutable
    __ior__ = _immutable


class VerifiedCycleCheckpoint(dict[str, object]):
    """Production-flow-derived proof for one update cycle."""

    def __init__(self, issuer: object, values: dict[str, object]) -> None:
        if issuer is not _CYCLE_CHECKPOINT_ISSUER or \
                set(values) != _CYCLE_CHECKPOINT_KEYS:
            raise TypeError("VerifiedCycleCheckpoint is verifier-issued only")
        dict.__init__(self, values)

    @staticmethod
    def _immutable(*_args: object, **_kwargs: object) -> None:
        raise TypeError("verified cycle checkpoints are immutable")

    __setitem__ = _immutable
    __delitem__ = _immutable
    clear = _immutable
    pop = _immutable
    popitem = _immutable
    setdefault = _immutable
    update = _immutable
    __ior__ = _immutable


class VerifiedThreeCycleAggregate(dict[str, object]):
    """Aggregate PASS issued only from three locked checkpoints."""

    def __init__(self, issuer: object, values: dict[str, object]) -> None:
        if issuer is not _THREE_CYCLE_AGGREGATE_ISSUER or \
                set(values) != _THREE_CYCLE_AGGREGATE_KEYS:
            raise TypeError(
                "VerifiedThreeCycleAggregate is verifier-issued only"
            )
        dict.__init__(self, values)

    @staticmethod
    def _immutable(*_args: object, **_kwargs: object) -> None:
        raise TypeError("verified cycle aggregates are immutable")

    __setitem__ = _immutable
    __delitem__ = _immutable
    clear = _immutable
    pop = _immutable
    popitem = _immutable
    setdefault = _immutable
    update = _immutable
    __ior__ = _immutable


def _normalize_hardware_id(value: Any, label: str) -> str:
    try:
        return flash.normalized_hardware_id(value)
    except Exception as exc:
        raise ValueError(f"{label} is not a valid hardware ID") from exc


@dataclass(frozen=True)
class BadgeAcceptanceSession:
    session_id: str
    uplink_hardware_id: str
    ble_hardware_id: str
    wifi_hardware_id: str

    def __post_init__(self) -> None:
        if not isinstance(self.session_id, str) or not \
                _SESSION_ID_RE.fullmatch(self.session_id):
            raise ValueError(
                "session_id must be 1-64 safe ASCII identifier characters"
            )
        normalized = (
            _normalize_hardware_id(
                self.uplink_hardware_id, "uplink_hardware_id"
            ),
            _normalize_hardware_id(self.ble_hardware_id, "ble_hardware_id"),
            _normalize_hardware_id(
                self.wifi_hardware_id, "wifi_hardware_id"
            ),
        )
        if len(set(normalized)) != 3:
            raise ValueError("acceptance session hardware IDs must be unique")
        object.__setattr__(self, "uplink_hardware_id", normalized[0])
        object.__setattr__(self, "ble_hardware_id", normalized[1])
        object.__setattr__(self, "wifi_hardware_id", normalized[2])


def _candidate_content_set_sha256(
    artifacts: flash.FrozenArtifactSet,
    *,
    platform_key: str,
    version: str,
    role: str,
) -> str:
    """Hash validated member triples without private receipt metadata."""
    if type(artifacts) is not flash.FrozenArtifactSet:
        raise AcceptanceError(
            "candidate content identity requires a frozen artifact set"
        )
    try:
        artifacts.__post_init__()
    except Exception as exc:
        raise AcceptanceError(
            "candidate content identity received malformed frozen artifacts"
        ) from exc
    if platform_key not in flash.PLATFORMS:
        raise AcceptanceError(
            "candidate content identity platform is unknown"
        )
    if role not in ("uplink", "scanner"):
        raise AcceptanceError(
            "candidate content identity role is malformed"
        )
    bound_values: list[bytes] = []
    for label, value in (
        ("platform", platform_key),
        ("version", version),
        ("role", role),
    ):
        if type(value) is not str or not value or "\x00" in value:
            raise AcceptanceError(
                f"candidate content identity {label} is malformed"
            )
        encoded = value.encode("utf-8")
        if len(encoded) > 255:
            raise AcceptanceError(
                f"candidate content identity {label} is too long"
            )
        bound_values.append(encoded)
    members = tuple(sorted(
        artifacts.members,
        key=lambda member: member.logical_name,
    ))
    digest = hashlib.sha256()
    digest.update(_CANDIDATE_CONTENT_SET_DOMAIN)
    for encoded in bound_values:
        digest.update(len(encoded).to_bytes(4, "big"))
        digest.update(encoded)
    digest.update(len(members).to_bytes(4, "big"))
    for member in members:
        logical_name = member.logical_name.encode("utf-8")
        digest.update(len(logical_name).to_bytes(4, "big"))
        digest.update(logical_name)
        digest.update(member.size.to_bytes(8, "big"))
        digest.update(bytes.fromhex(member.sha256))
    return digest.hexdigest()


def verify_candidate_artifacts(
    frozen: flash.FrozenUsbFirmwareArtifacts,
    version: str,
) -> VerifiedCandidateArtifacts:
    """Derive an exact, privacy-safe identity from both frozen build trees."""
    if type(frozen) is not flash.FrozenUsbFirmwareArtifacts:
        raise AcceptanceError("candidate firmware trees are not frozen")
    frozen.__post_init__()
    if version != flash.UPDATE_MAINTENANCE_MIN_VERSION:
        raise AcceptanceError(
            "acceptance artifacts require the exact .79 canary version"
        )
    platform = flash.PLATFORMS[CANARY_PLATFORM_KEY]
    roles: dict[str, _VerifiedCandidateArtifactRole] = {}
    for role, artifacts, target, project in (
        (
            "uplink",
            frozen.uplink,
            platform["uplink_name"],
            platform["uplink_project"],
        ),
        (
            "scanner",
            frozen.scanner,
            platform["scanner_name"],
            platform["scanner_project"],
        ),
    ):
        if type(artifacts) is not flash.FrozenArtifactSet:
            raise AcceptanceError(
                f"candidate {role} firmware tree is unavailable"
            )
        try:
            artifacts.__post_init__()
            image = flash._validated_frozen_firmware_bytes(
                artifacts,
                role=role,
                target=target,
                project=project,
                hardware=platform["hardware_type"],
                version=version,
            )
            content_set_sha256 = _candidate_content_set_sha256(
                artifacts,
                platform_key=CANARY_PLATFORM_KEY,
                version=version,
                role=role,
            )
        except Exception as exc:
            raise AcceptanceError(
                f"candidate {role} firmware identity validation failed"
            ) from exc
        roles[role] = _VerifiedCandidateArtifactRole(
            _CANDIDATE_ARTIFACT_ISSUER,
            {
                "content_set_sha256": content_set_sha256,
                "firmware_size": len(image),
                "firmware_crc32": binascii.crc32(image) & 0xFFFFFFFF,
                "firmware_sha256": hashlib.sha256(image).hexdigest(),
            },
        )
    return VerifiedCandidateArtifacts(
        _CANDIDATE_ARTIFACT_ISSUER,
        {
            "schema": _CANDIDATE_ARTIFACT_SCHEMA,
            "platform_key": CANARY_PLATFORM_KEY,
            "version": version,
            "uplink": roles["uplink"],
            "scanner": roles["scanner"],
        },
    )


def _validate_candidate_artifacts(
    value: object,
    expected_version: str,
) -> VerifiedCandidateArtifacts:
    if type(value) is not VerifiedCandidateArtifacts or \
            set(value) != _CANDIDATE_ARTIFACT_KEYS:
        raise AcceptanceError(
            "candidate artifacts must be verifier-issued with exact schema"
        )
    if type(value.get("schema")) is not int or \
            value.get("schema") != _CANDIDATE_ARTIFACT_SCHEMA:
        raise AcceptanceError(
            "candidate artifact schema must be exact "
            f"{_CANDIDATE_ARTIFACT_SCHEMA}"
        )
    if value.get("platform_key") != CANARY_PLATFORM_KEY:
        raise AcceptanceError(
            "candidate artifact platform is not the fixed canary track"
        )
    if expected_version != flash.UPDATE_MAINTENANCE_MIN_VERSION:
        raise AcceptanceError(
            "candidate artifact validation requires exact .79"
        )
    if value.get("version") != expected_version:
        raise AcceptanceError(
            "candidate artifact version does not match the candidate"
        )
    for role in ("uplink", "scanner"):
        identity = value.get(role)
        if type(identity) is not _VerifiedCandidateArtifactRole or \
                set(identity) != _CANDIDATE_ARTIFACT_ROLE_KEYS:
            raise AcceptanceError(
                f"candidate {role} artifact identity schema is invalid"
            )
        for key in ("content_set_sha256", "firmware_sha256"):
            digest = identity.get(key)
            if not isinstance(digest, str) or \
                    _SHA256_RE.fullmatch(digest) is None:
                raise AcceptanceError(
                    f"candidate {role} artifact {key} is malformed"
                )
        size = identity.get("firmware_size")
        crc32 = identity.get("firmware_crc32")
        if type(size) is not int or size <= 0 or \
                type(crc32) is not int or not 0 <= crc32 <= 0xFFFFFFFF:
            raise AcceptanceError(
                f"candidate {role} artifact size or CRC32 is malformed"
            )
    return value


def _exact_nonnegative_int(value: Any, label: str) -> int:
    if type(value) is not int or value < 0:
        raise AcceptanceError(f"{label} must be an exact non-negative integer")
    return value


def _exact_uint32(value: Any, label: str, *, nonzero: bool = False) -> int:
    if type(value) is not int or not 0 <= value <= _UINT32_MAX or (
        nonzero and value == 0
    ):
        qualifier = "nonzero " if nonzero else ""
        raise AcceptanceError(
            f"{label} must be an exact {qualifier}uint32"
        )
    return value


def _expected_reboot_successor(prior_generation: int) -> int:
    prior = _exact_uint32(
        prior_generation,
        "prior reboot generation",
    )
    successor = (prior + 1) & _UINT32_MAX
    if successor == 0:
        successor = 1
    if successor == _EXPECTED_REBOOT_MAGIC:
        successor += 1
    return successor


def _bound_snapshot_reboot_generation(
    status: dict[str, Any],
    version: str,
    *,
    label: str,
) -> tuple[str, int | None]:
    if version == flash.UPDATE_MAINTENANCE_BOOTSTRAP_SOURCE_VERSION:
        if "last_expected_reboot_generation" in status:
            raise AcceptanceError(
                f"{label} fabricated a reboot generation for exact .78"
            )
        return _LEGACY_V078_GENERATION_CAPABILITY, None
    generation = _exact_uint32(
        status.get("last_expected_reboot_generation"),
        f"{label} reboot generation",
        nonzero=True,
    )
    return "reported", generation


def _stack_metric_maximums(expected_version: str) -> dict[str, int]:
    if expected_version == flash.UPDATE_MAINTENANCE_BOOTSTRAP_SOURCE_VERSION:
        return _PRODUCTION_STACK_METRIC_MAXIMUMS
    if expected_version == flash.UPDATE_MAINTENANCE_MIN_VERSION:
        return _CANARY_STACK_METRIC_MAXIMUMS
    raise AcceptanceError(
        "live metric version is outside the exact promotion lineage"
    )


def _live_metrics(
    status: dict[str, Any],
    label: str,
    *,
    expected_version: str,
) -> dict[str, int]:
    if type(status) is not dict:
        raise AcceptanceError(f"{label} must be an exact status object")
    stack_maximums = _stack_metric_maximums(expected_version)
    metrics: dict[str, int] = {}
    for field in _LIVE_METRIC_FIELDS:
        value = status.get(field)
        maximum = (
            stack_maximums[field]
            if field in stack_maximums
            else _INTERNAL_HEAP_MAX_BYTES
            if field.startswith("heap_internal_")
            else 4096
        )
        if type(value) is not int or not 0 <= value <= maximum:
            raise AcceptanceError(
                f"{label} {field} is missing, malformed, or impossible"
            )
        if field != "detection_queue_capacity" and value == 0:
            raise AcceptanceError(f"{label} {field} is not live")
        metrics[field] = value
    if metrics["heap_internal_largest"] > metrics["heap_internal_free"]:
        raise AcceptanceError(
            f"{label} largest internal block exceeds free internal heap"
        )
    if metrics["heap_internal_min_free"] > metrics["heap_internal_free"]:
        raise AcceptanceError(
            f"{label} minimum-ever internal heap exceeds current free heap"
        )
    return metrics


def _legacy_v078_live_metrics(
    status: dict[str, Any],
    label: str,
) -> dict[str, int]:
    queue_capacity = status.get("detection_queue_capacity")
    if "detection_queue_capacity" in status and (
        type(queue_capacity) is not int
        or queue_capacity != _V078_DETECTION_QUEUE_CAPACITY
    ):
        raise AcceptanceError(
            f"{label} detection queue telemetry is not exact deployed 48"
        )
    derived = dict(status)
    derived["detection_queue_capacity"] = \
        _V078_DETECTION_QUEUE_CAPACITY
    return _live_metrics(
        derived,
        label,
        expected_version=flash.UPDATE_MAINTENANCE_BOOTSTRAP_SOURCE_VERSION,
    )


def _bound_live_status(
    status: dict[str, Any],
    session: BadgeAcceptanceSession,
    *,
    expected_version: str,
    expected_mode: str,
    expected_update_session: str,
    label: str,
) -> tuple[str, dict[str, Any], int, int, int, int, str, int]:
    if type(session) is not BadgeAcceptanceSession:
        raise AcceptanceError(f"{label} acceptance session is malformed")
    try:
        hardware_id = flash._validate_uplink_status_common(status)
    except Exception as exc:
        raise AcceptanceError(f"{label} uplink proof failed: {exc}") from exc
    if hardware_id != session.uplink_hardware_id:
        raise AcceptanceError(f"{label} hardware identity mismatch")
    if status.get("version") != expected_version:
        raise AcceptanceError(f"{label} version mismatch")
    if status.get("recovery_mode") != expected_mode:
        raise AcceptanceError(f"{label} recovery mode mismatch")
    if status.get("pending_verify") is not False or \
            status.get("rollback_state") != "clear" or \
            status.get("safe_mode") is True:
        raise AcceptanceError(
            f"{label} rollback or safe-mode state is not clear"
        )
    update_session = status.get("update_session")
    if expected_update_session:
        if update_session != expected_update_session:
            raise AcceptanceError(f"{label} update session mismatch")
    elif update_session not in (None, ""):
        raise AcceptanceError(f"{label} unexpectedly carries an update session")
    health = _validate_usb_health(status)
    rx_bytes = _exact_uint32(
        health.get("rx_bytes"),
        f"{label} RX bytes",
        nonzero=True,
    )
    valid_commands = _exact_uint32(
        health.get("valid_commands"),
        f"{label} valid commands",
        nonzero=True,
    )
    responses = _exact_uint32(
        health.get("responses_completed"),
        f"{label} USB responses",
        nonzero=True,
    )
    generation = _exact_uint32(
        status.get("last_expected_reboot_generation"),
        f"{label} reboot generation",
        nonzero=True,
    )
    partition = status.get("running_partition")
    if partition not in ("ota_0", "ota_1"):
        raise AcceptanceError(f"{label} running partition is malformed")
    uptime = _exact_nonnegative_int(
        status.get("uptime_s"),
        f"{label} uptime",
    )
    return (
        hardware_id,
        health,
        rx_bytes,
        valid_commands,
        responses,
        generation,
        str(partition),
        uptime,
    )


def _bound_legacy_v078_status(
    status: dict[str, Any],
    session: BadgeAcceptanceSession,
    *,
    label: str,
) -> tuple[str, dict[str, Any], str, int, int, int, int]:
    source_version = flash.UPDATE_MAINTENANCE_BOOTSTRAP_SOURCE_VERSION
    if "last_expected_reboot_generation" in status:
        raise AcceptanceError(
            f"{label} must omit unsupported reboot generation"
        )
    if not flash._allows_direct_update_maintenance_bootstrap(
        status,
        target_version=flash.UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION,
    ):
        raise AcceptanceError(f"{label} legacy source proof failed")
    hardware_id = flash.validate_uplink_application_status(status)
    if hardware_id != session.uplink_hardware_id:
        raise AcceptanceError(f"{label} hardware identity mismatch")
    queue_capacity = status.get("detection_queue_capacity")
    if "detection_queue_capacity" in status and (
        type(queue_capacity) is not int
        or queue_capacity != _V078_DETECTION_QUEUE_CAPACITY
    ):
        raise AcceptanceError(
            f"{label} detection queue telemetry is not exact deployed 48"
        )
    health = _validate_usb_health(status)
    partition = status.get("running_partition")
    if partition not in ("ota_0", "ota_1"):
        raise AcceptanceError(f"{label} source partition is malformed")
    uptime = _exact_nonnegative_int(
        status.get("uptime_s"),
        f"{label} uptime",
    )
    valid_commands = _exact_uint32(
        health.get("valid_commands"),
        f"{label} valid commands",
        nonzero=True,
    )
    responses = _exact_uint32(
        health.get("responses_completed"),
        f"{label} USB responses",
        nonzero=True,
    )
    rx_bytes = _exact_uint32(
        health.get("rx_bytes"),
        f"{label} RX bytes",
        nonzero=True,
    )
    return (
        hardware_id,
        health,
        str(partition),
        uptime,
        rx_bytes,
        valid_commands,
        responses,
    )


def capture_v078_updater_baseline(
    status: dict[str, Any],
    session: BadgeAcceptanceSession,
    *,
    pre_status: dict[str, Any],
    challenge_version: str,
) -> VerifiedV078UpdaterBaseline:
    """Issue a fresh two-response .78 normal-mode comparison."""
    source_version = flash.UPDATE_MAINTENANCE_BOOTSTRAP_SOURCE_VERSION
    if challenge_version != source_version:
        raise AcceptanceError("v0.78 updater live challenge version mismatch")
    (
        pre_hardware_id,
        _pre_health,
        pre_partition,
        pre_uptime,
        pre_rx_bytes,
        pre_valid_commands,
        pre_responses,
    ) = _bound_legacy_v078_status(
        pre_status,
        session,
        label="v0.78 updater pre-acquisition status",
    )
    (
        hardware_id,
        _health,
        partition,
        uptime,
        rx_bytes,
        valid_commands,
        responses,
    ) = _bound_legacy_v078_status(
        status,
        session,
        label="v0.78 updater baseline",
    )
    try:
        flash._validate_direct_bootstrap_source_continuity(
            pre_status,
            status,
            target_version=flash.UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION,
        )
    except Exception as exc:
        raise AcceptanceError(
            f"v0.78 updater baseline continuity failed: {exc}"
        ) from exc
    if hardware_id != pre_hardware_id or partition != pre_partition:
        raise AcceptanceError(
            "v0.78 updater baseline acquisition changed hardware or partition"
        )
    if uptime < pre_uptime:
        raise AcceptanceError(
            "v0.78 updater baseline acquisition uptime moved backward"
        )
    if valid_commands != pre_valid_commands + 2 or \
            responses != pre_responses + 2 or \
            rx_bytes != pre_rx_bytes + 22:
        raise AcceptanceError(
            "v0.78 updater baseline challenge counters did not advance "
            "exactly across PING/status"
        )
    metrics = _legacy_v078_live_metrics(
        status,
        "v0.78 updater baseline",
    )
    return VerifiedV078UpdaterBaseline(_LIVE_METRIC_ISSUER, {
        "source_version": source_version,
        "session_id": session.session_id,
        "hardware_id": hardware_id,
        "recovery_mode": "normal",
        "update_session": "",
        "reboot_generation_capability":
            _LEGACY_V078_GENERATION_CAPABILITY,
        "source_partition": partition,
        "pre_uptime_s": pre_uptime,
        "uptime_s": uptime,
        "pre_reboot_generation": None,
        "pre_rx_bytes": pre_rx_bytes,
        "pre_valid_commands": pre_valid_commands,
        "pre_responses_completed": pre_responses,
        "reboot_generation": None,
        "rx_bytes": rx_bytes,
        "valid_commands": valid_commands,
        "responses_completed": responses,
        **metrics,
    })


def _capture_reserved_v078_updater_baseline(
    port: str,
    session: BadgeAcceptanceSession,
    reservation: _DurableGateReservation,
) -> VerifiedV078UpdaterBaseline:
    """Capture two fresh .78 responses while one mutation reservation lives."""
    _validate_live_mutating_gate_reservation(reservation)
    descriptor = _require_session_uplink_descriptor(
        _trusted_session_uplink_descriptor(port, session),
        session,
        stage="v0.78 baseline",
    )
    with flash.BadgeSerial(
        descriptor,
        False,
        expected_hardware_id=session.uplink_hardware_id,
    ) as badge:
        pre_status = badge.status(timeout_s=5)
        badge.write_line("FOF_PING")
        challenge_version = badge.read_prefixed_text("FOF_PONG:", 5)
        status = badge.status(timeout_s=5)
    _validate_live_mutating_gate_reservation(reservation)
    return capture_v078_updater_baseline(
        status,
        session,
        pre_status=pre_status,
        challenge_version=challenge_version,
    )


def _validate_v078_baseline_schema(
    baseline: object,
) -> VerifiedV078UpdaterBaseline:
    if type(baseline) is not VerifiedV078UpdaterBaseline or \
            set(baseline) != _V078_BASELINE_KEYS:
        raise AcceptanceError(
            "promotion requires a verifier-issued exact v0.78 baseline"
        )
    if baseline.get("source_version") != \
            flash.UPDATE_MAINTENANCE_BOOTSTRAP_SOURCE_VERSION or \
            baseline.get("recovery_mode") != "normal" or \
            baseline.get("update_session") != "" or \
            baseline.get("reboot_generation_capability") != \
            _LEGACY_V078_GENERATION_CAPABILITY or \
            baseline.get("pre_reboot_generation") is not None or \
            baseline.get("reboot_generation") is not None:
        raise AcceptanceError(
            "v0.78 updater baseline capability/version/mode is malformed"
        )
    if baseline.get("source_partition") not in ("ota_0", "ota_1"):
        raise AcceptanceError("v0.78 baseline source partition is malformed")
    session_id = baseline.get("session_id")
    if not isinstance(session_id, str) or \
            _SESSION_ID_RE.fullmatch(session_id) is None:
        raise AcceptanceError("v0.78 baseline session is malformed")
    _normalize_hardware_id(
        baseline.get("hardware_id"),
        "v0.78 baseline hardware_id",
    )
    pre_uptime = _exact_nonnegative_int(
        baseline.get("pre_uptime_s"),
        "v0.78 baseline pre-acquisition uptime",
    )
    uptime = _exact_nonnegative_int(
        baseline.get("uptime_s"),
        "v0.78 baseline acquisition uptime",
    )
    pre_rx_bytes = _exact_uint32(
        baseline.get("pre_rx_bytes"),
        "v0.78 baseline pre-acquisition RX bytes",
        nonzero=True,
    )
    rx_bytes = _exact_uint32(
        baseline.get("rx_bytes"),
        "v0.78 baseline acquisition RX bytes",
        nonzero=True,
    )
    pre_valid_commands = _exact_uint32(
        baseline.get("pre_valid_commands"),
        "v0.78 baseline pre-acquisition valid commands",
        nonzero=True,
    )
    valid_commands = _exact_uint32(
        baseline.get("valid_commands"),
        "v0.78 baseline acquisition valid commands",
        nonzero=True,
    )
    pre_responses = _exact_uint32(
        baseline.get("pre_responses_completed"),
        "v0.78 baseline pre-acquisition responses",
        nonzero=True,
    )
    responses = _exact_uint32(
        baseline.get("responses_completed"),
        "v0.78 baseline acquisition responses",
        nonzero=True,
    )
    if uptime < pre_uptime or \
            rx_bytes != pre_rx_bytes + 22 or \
            valid_commands != pre_valid_commands + 2 or \
            responses != pre_responses + 2:
        raise AcceptanceError(
            "v0.78 updater baseline lacks its exact live challenge"
        )
    _live_metrics(
        dict(baseline),
        "v0.78 updater baseline evidence",
        expected_version=flash.UPDATE_MAINTENANCE_BOOTSTRAP_SOURCE_VERSION,
    )
    return baseline


def _validate_v078_baseline_binding(
    baseline: object,
    session: BadgeAcceptanceSession,
) -> VerifiedV078UpdaterBaseline:
    baseline = _validate_v078_baseline_schema(baseline)
    if baseline.get("session_id") != session.session_id or \
            baseline.get("hardware_id") != session.uplink_hardware_id:
        raise AcceptanceError(
            "v0.78 updater baseline does not match this session/device"
        )
    return baseline


def _validate_live_sample_binding(
    previous: object,
    session: BadgeAcceptanceSession,
    expected_version: str,
) -> VerifiedLiveMetricSample:
    if type(previous) is not VerifiedLiveMetricSample or \
            set(previous) != _LIVE_SAMPLE_KEYS or \
            previous.get("session_id") != session.session_id or \
            previous.get("hardware_id") != session.uplink_hardware_id or \
            previous.get("version") != expected_version:
        raise AcceptanceError(
            "live gate lacks a bound prior live sample"
        )
    capability = previous.get("reboot_generation_capability")
    generation = previous.get("reboot_generation")
    if expected_version == \
            flash.UPDATE_MAINTENANCE_BOOTSTRAP_SOURCE_VERSION:
        if capability != _LEGACY_V078_GENERATION_CAPABILITY or \
                generation is not None:
            raise AcceptanceError(
                "legacy live sample fabricated a reboot generation"
            )
    elif expected_version == flash.UPDATE_MAINTENANCE_MIN_VERSION:
        if capability != "reported":
            raise AcceptanceError(
                "canary live sample lacks reported-generation capability"
            )
        _exact_uint32(
            generation,
            "canary live reboot generation",
            nonzero=True,
        )
    else:
        raise AcceptanceError("live sample version is outside canary lineage")
    if previous.get("running_partition") not in ("ota_0", "ota_1"):
        raise AcceptanceError("live sample running partition is malformed")
    _exact_nonnegative_int(previous.get("uptime_s"), "prior live uptime")
    for field, label in (
        ("rx_bytes", "prior live RX bytes"),
        ("valid_commands", "prior live valid commands"),
        ("responses_completed", "prior live USB responses"),
    ):
        _exact_uint32(previous.get(field), label, nonzero=True)
    _live_metrics(
        dict(previous),
        "prior live sample",
        expected_version=expected_version,
    )
    return previous


def _require_fresh_live_sample(
    previous: VerifiedLiveMetricSample,
    *,
    generation: int,
    partition: str,
    uptime: int,
    rx_bytes: int,
    valid_commands: int,
    responses: int,
    label: str,
) -> None:
    prior_generation = _exact_uint32(
        previous.get("reboot_generation"),
        "prior live reboot generation",
        nonzero=True,
    )
    prior_responses = _exact_uint32(
        previous.get("responses_completed"),
        "prior live USB responses",
        nonzero=True,
    )
    if generation == prior_generation:
        prior_uptime = _exact_nonnegative_int(
            previous.get("uptime_s"),
            "prior live uptime",
        )
        prior_rx_bytes = _exact_uint32(
            previous.get("rx_bytes"),
            "prior live RX bytes",
            nonzero=True,
        )
        prior_valid_commands = _exact_uint32(
            previous.get("valid_commands"),
            "prior live valid commands",
            nonzero=True,
        )
        if previous.get("running_partition") != partition or \
                uptime < prior_uptime or \
                rx_bytes <= prior_rx_bytes or \
                valid_commands <= prior_valid_commands or \
                responses <= prior_responses:
            raise AcceptanceError(
                f"{label} broke same-boot partition, uptime, or counter "
                "continuity"
            )
        return
    if generation != _expected_reboot_successor(prior_generation):
        raise AcceptanceError(
            f"{label} reboot generation is not the exact successor"
        )


def _require_canary_normal_memory(
    metrics: dict[str, int],
    baseline: VerifiedV078UpdaterBaseline,
    *,
    label: str,
) -> None:
    for field, minimum in {
        "heap_internal_free": 24576,
        "heap_internal_largest": 16384,
        "heap_internal_min_free": 12288,
    }.items():
        if metrics[field] < minimum:
            raise AcceptanceError(
                f"{label} {field} is below {minimum}"
            )
    if metrics["detection_queue_capacity"] != 0:
        raise AcceptanceError(
            f"{label} detection queue was not reclaimed"
        )
    for field in _STACK_METRIC_FIELDS:
        if metrics[field] < baseline[field]:
            raise AcceptanceError(
                f"{label} {field} regressed below the .78 floor"
            )


def _require_canary_maintenance_memory(
    metrics: dict[str, int],
    baseline: VerifiedV078UpdaterBaseline,
    *,
    label: str,
) -> None:
    if metrics["heap_internal_min_free"] < 12288:
        raise AcceptanceError(
            f"{label} heap_internal_min_free is below 12288"
        )
    for field in ("heap_internal_free", "heap_internal_largest"):
        if metrics[field] < baseline[field]:
            raise AcceptanceError(
                f"{label} {field} is below the exact .78 baseline"
            )
    if metrics["detection_queue_capacity"] != 0:
        raise AcceptanceError(
            f"{label} detection queue was not reclaimed"
        )
    for field in _STACK_METRIC_FIELDS:
        if metrics[field] < baseline[field]:
            raise AcceptanceError(
                f"{label} {field} regressed below the .78 floor"
            )


def verify_canary_normal_live_metrics(
    status: dict[str, Any],
    session: BadgeAcceptanceSession,
    expected_version: str,
    *,
    baseline: VerifiedV078UpdaterBaseline,
    previous: VerifiedLiveMetricSample | None = None,
) -> VerifiedLiveMetricSample:
    """Apply the pre-mutation normal-mode promotion gate."""
    if expected_version != flash.UPDATE_MAINTENANCE_MIN_VERSION:
        raise AcceptanceError("normal live gate requires the exact .79 canary")
    bound_baseline = _validate_v078_baseline_binding(baseline, session)
    (
        hardware_id,
        _health,
        rx_bytes,
        valid_commands,
        responses,
        generation,
        partition,
        uptime,
    ) = _bound_live_status(
        status,
        session,
        expected_version=expected_version,
        expected_mode="normal",
        expected_update_session="",
        label="canary normal live sample",
    )
    metrics = _live_metrics(
        status,
        "canary normal live sample",
        expected_version=expected_version,
    )
    if previous is not None:
        prior = _validate_live_sample_binding(
            previous, session, expected_version
        )
        if prior["recovery_mode"] == "update_maintenance" and (
            generation
            != _expected_reboot_successor(
                int(prior["reboot_generation"])
            )
            or partition != prior["running_partition"]
        ):
            raise AcceptanceError(
                "normal return after maintenance must use the same "
                "partition and exact successor generation"
            )
        _require_fresh_live_sample(
            prior,
            generation=generation,
            partition=partition,
            uptime=uptime,
            rx_bytes=rx_bytes,
            valid_commands=valid_commands,
            responses=responses,
            label="canary normal live sample",
        )
    _require_canary_normal_memory(
        metrics,
        bound_baseline,
        label="canary normal live sample",
    )
    return VerifiedLiveMetricSample(_LIVE_METRIC_ISSUER, {
        "version": expected_version,
        "session_id": session.session_id,
        "hardware_id": hardware_id,
        "recovery_mode": "normal",
        "update_session": "",
        "reboot_generation_capability": "reported",
        "running_partition": partition,
        "uptime_s": uptime,
        "reboot_generation": generation,
        "rx_bytes": rx_bytes,
        "valid_commands": valid_commands,
        "responses_completed": responses,
        **metrics,
    })


def verify_v078_updater_live_successor(
    status: dict[str, Any],
    session: BadgeAcceptanceSession,
    *,
    baseline: VerifiedV078UpdaterBaseline,
) -> VerifiedLiveMetricSample:
    """Bind the final read-only .78 sample immediately before cycle 1."""
    bound_baseline = _validate_v078_baseline_binding(baseline, session)
    source_version = flash.UPDATE_MAINTENANCE_BOOTSTRAP_SOURCE_VERSION
    (
        hardware_id,
        _health,
        partition,
        uptime,
        rx_bytes,
        valid_commands,
        responses,
    ) = _bound_legacy_v078_status(
        status,
        session,
        label="v0.78 updater pre-mutation sample",
    )
    if partition != bound_baseline["source_partition"] or \
            uptime < bound_baseline["uptime_s"]:
        raise AcceptanceError(
            "v0.78 updater pre-mutation sample is stale or changed source"
        )
    if rx_bytes != (
        bound_baseline["rx_bytes"] + _V078_PRE_MUTATION_RX_DELTA
    ) or valid_commands != (
        bound_baseline["valid_commands"]
        + _V078_PRE_MUTATION_COMMAND_DELTA
    ) or responses != (
        bound_baseline["responses_completed"]
        + _V078_PRE_MUTATION_COMMAND_DELTA
    ):
        raise AcceptanceError(
            "v0.78 updater pre-mutation probe counters did not advance "
            "by exact RX +34, commands +3, responses +3"
        )
    metrics = _legacy_v078_live_metrics(
        status,
        "v0.78 updater pre-mutation sample",
    )
    return VerifiedLiveMetricSample(_LIVE_METRIC_ISSUER, {
        "version": source_version,
        "session_id": session.session_id,
        "hardware_id": hardware_id,
        "recovery_mode": "normal",
        "update_session": "",
        "reboot_generation_capability":
            _LEGACY_V078_GENERATION_CAPABILITY,
        "running_partition": partition,
        "uptime_s": uptime,
        "reboot_generation": None,
        "rx_bytes": rx_bytes,
        "valid_commands": valid_commands,
        "responses_completed": responses,
        **metrics,
    })


def verify_canary_post_direct_bootstrap_live_metrics(
    status: dict[str, Any],
    session: BadgeAcceptanceSession,
    expected_version: str,
    *,
    baseline: VerifiedV078UpdaterBaseline,
    previous: VerifiedLiveMetricSample,
) -> VerifiedLiveMetricSample:
    """Bind the first exact .79 normal boot after the direct .78 OTA."""
    bound_baseline = _validate_v078_baseline_binding(baseline, session)
    prior = _validate_live_sample_binding(
        previous,
        session,
        flash.UPDATE_MAINTENANCE_BOOTSTRAP_SOURCE_VERSION,
    )
    if prior["running_partition"] != bound_baseline["source_partition"]:
        raise AcceptanceError(
            "post-bootstrap prior sample changed the .78 source partition"
        )
    live = verify_canary_normal_live_metrics(
        status,
        session,
        expected_version,
        baseline=bound_baseline,
    )
    alternate = {
        "ota_0": "ota_1",
        "ota_1": "ota_0",
    }[str(bound_baseline["source_partition"])]
    if live["running_partition"] != alternate:
        raise AcceptanceError(
            "post-bootstrap .79 did not boot the alternate OTA partition"
        )
    if status.get("last_expected_reboot_reason") != "usb_uplink_ota":
        raise AcceptanceError(
            "post-bootstrap .79 lacks usb_uplink_ota reboot provenance"
        )
    if live["reboot_generation"] != 1:
        raise AcceptanceError(
            "post-bootstrap .79 reboot generation must be exact 1"
        )
    return live


def verify_chord_first_maintenance_live_metrics(
    status: dict[str, Any],
    session: BadgeAcceptanceSession,
    expected_version: str,
    update_session: str,
    *,
    baseline: VerifiedV078UpdaterBaseline,
    previous: VerifiedChordRomBootSnapshot,
) -> VerifiedLiveMetricSample:
    """Bridge the exceptional generation-0 ROM boot to maintenance gen 1."""
    if expected_version != flash.UPDATE_MAINTENANCE_MIN_VERSION:
        raise AcceptanceError(
            "chord maintenance bridge requires the exact .79 canary"
        )
    bound_baseline = _validate_v078_baseline_binding(baseline, session)
    _validate_chord_rom_boot_snapshot(
        previous,
        session,
        expected_version,
    )
    try:
        flash._validate_update_maintenance_status(
            status,
            session=update_session,
            expected_hardware_id=session.uplink_hardware_id,
        )
    except Exception as exc:
        raise AcceptanceError(
            f"chord first maintenance status proof failed: {exc}"
        ) from exc
    (
        hardware_id,
        _health,
        rx_bytes,
        valid_commands,
        responses,
        generation,
        partition,
        uptime,
    ) = _bound_live_status(
        status,
        session,
        expected_version=expected_version,
        expected_mode="update_maintenance",
        expected_update_session=update_session,
        label="chord first maintenance live sample",
    )
    if generation != 1 or \
            partition != previous["running_partition"]:
        raise AcceptanceError(
            "chord first maintenance boot must be generation 1 on the "
            "post-ROM partition"
        )
    metrics = _live_metrics(
        status,
        "chord first maintenance live sample",
        expected_version=expected_version,
    )
    _require_canary_maintenance_memory(
        metrics,
        bound_baseline,
        label="chord first maintenance live sample",
    )
    return VerifiedLiveMetricSample(_LIVE_METRIC_ISSUER, {
        "version": expected_version,
        "session_id": session.session_id,
        "hardware_id": hardware_id,
        "recovery_mode": "update_maintenance",
        "update_session": update_session,
        "reboot_generation_capability": "reported",
        "running_partition": partition,
        "uptime_s": uptime,
        "reboot_generation": generation,
        "rx_bytes": rx_bytes,
        "valid_commands": valid_commands,
        "responses_completed": responses,
        **metrics,
    })


def verify_canary_maintenance_live_metrics(
    status: dict[str, Any],
    session: BadgeAcceptanceSession,
    expected_version: str,
    update_session: str,
    *,
    baseline: VerifiedV078UpdaterBaseline,
    previous: VerifiedLiveMetricSample,
) -> VerifiedLiveMetricSample:
    """Gate each maintenance boot before the next firmware bytes."""
    if expected_version != flash.UPDATE_MAINTENANCE_MIN_VERSION:
        raise AcceptanceError(
            "maintenance live gate requires the exact .79 canary"
        )
    bound_baseline = _validate_v078_baseline_binding(baseline, session)
    prior_version = previous.get("version")
    if prior_version != expected_version:
        raise AcceptanceError(
            "maintenance live gate requires an exact .79 prior sample"
        )
    prior = _validate_live_sample_binding(
        previous, session, str(prior_version)
    )
    if prior["recovery_mode"] == "update_maintenance" and \
            prior["update_session"] != update_session:
        raise AcceptanceError(
            "maintenance live gate changed the active update session"
        )
    try:
        flash._validate_update_maintenance_status(
            status,
            session=update_session,
            expected_hardware_id=session.uplink_hardware_id,
        )
    except Exception as exc:
        raise AcceptanceError(
            f"maintenance status proof failed: {exc}"
        ) from exc
    live_version = status.get("version")
    if live_version != expected_version:
        raise AcceptanceError(
            "maintenance live sample must be the exact .79 canary"
        )
    (
        hardware_id,
        _health,
        rx_bytes,
        valid_commands,
        responses,
        generation,
        partition,
        uptime,
    ) = _bound_live_status(
        status,
        session,
        expected_version=str(live_version),
        expected_mode="update_maintenance",
        expected_update_session=update_session,
        label="canary maintenance live sample",
    )
    if (
        prior["recovery_mode"] == "normal"
        and (
            generation
            != _expected_reboot_successor(int(prior["reboot_generation"]))
            or partition != prior["running_partition"]
        )
    ):
        raise AcceptanceError(
            "first maintenance boot must use the same partition and exact "
            "successor generation"
        )
    _require_fresh_live_sample(
        prior,
        generation=generation,
        partition=partition,
        uptime=uptime,
        rx_bytes=rx_bytes,
        valid_commands=valid_commands,
        responses=responses,
        label="canary maintenance live sample",
    )
    metrics = _live_metrics(
        status,
        "canary maintenance live sample",
        expected_version=expected_version,
    )
    _require_canary_maintenance_memory(
        metrics,
        bound_baseline,
        label="canary maintenance live sample",
    )
    return VerifiedLiveMetricSample(_LIVE_METRIC_ISSUER, {
        "version": str(live_version),
        "session_id": session.session_id,
        "hardware_id": hardware_id,
        "recovery_mode": "update_maintenance",
        "update_session": update_session,
        "reboot_generation_capability": "reported",
        "running_partition": partition,
        "uptime_s": uptime,
        "reboot_generation": generation,
        "rx_bytes": rx_bytes,
        "valid_commands": valid_commands,
        "responses_completed": responses,
        **metrics,
    })


def _validate_usb_health(status: dict[str, Any]) -> dict[str, Any]:
    health = status.get("usb_health")
    if not isinstance(health, dict):
        raise AcceptanceError("FOF_STATUS is missing usb_health")
    required = {
        "schema", "task_started", "host_connected", "parser_state",
        *_USB_COUNTER_FIELDS, *_USB_AGE_FIELDS,
    }
    missing = sorted(required - set(health))
    if missing:
        raise AcceptanceError(
            "usb_health is missing required fields: " + ", ".join(missing)
        )
    if health.get("schema") != 1 or type(health.get("schema")) is not int:
        raise AcceptanceError("usb_health schema must be exact integer 1")
    for key in ("task_started", "host_connected"):
        if type(health.get(key)) is not bool or health.get(key) is not True:
            raise AcceptanceError(f"usb_health {key} is not proven true")
    if health.get("parser_state") != "command":
        raise AcceptanceError(
            "USB parser did not return to command state: "
            f"{health.get('parser_state')!r}"
        )
    for key in _USB_COUNTER_FIELDS:
        _exact_nonnegative_int(health.get(key), f"usb_health.{key}")
    if health["responses_completed"] <= 0:
        raise AcceptanceError("USB has no completed command response")
    for key in _USB_AGE_FIELDS:
        value = health.get(key)
        if value is not None:
            _exact_nonnegative_int(value, f"usb_health.{key}")
    return health


def verify_badge_snapshot(
    status: dict[str, object],
    session: BadgeAcceptanceSession,
    expected_version: str,
) -> dict[str, object]:
    """Return a bounded proof after validating all three immutable boards."""
    if type(session) is not BadgeAcceptanceSession:
        raise AcceptanceError("acceptance session is malformed")
    if not isinstance(status, dict):
        raise AcceptanceError("FOF_STATUS snapshot must be an object")
    if not isinstance(expected_version, str) or \
            flash.firmware_version_relation(
                expected_version, expected_version
            ) != "equal":
        raise AcceptanceError("expected badge version is invalid")

    try:
        uplink_id = flash.validate_uplink_application_status(status)
    except Exception as exc:
        raise AcceptanceError(f"uplink application proof failed: {exc}") from exc
    if uplink_id != session.uplink_hardware_id:
        raise AcceptanceError(
            f"uplink board changed: got {uplink_id}, "
            f"wanted {session.uplink_hardware_id}"
        )
    if status.get("version") != expected_version:
        raise AcceptanceError(
            f"uplink version mismatch: got {status.get('version')!r}, "
            f"wanted {expected_version!r}"
        )
    if status.get("pending_verify") is not False or \
            status.get("rollback_state") != "clear":
        raise AcceptanceError("uplink rollback state is not clear")
    if status.get("safe_mode") is True:
        raise AcceptanceError("uplink safe mode is active")

    health = _validate_usb_health(status)
    uptime = _exact_nonnegative_int(
        status.get("uptime_s"),
        "badge snapshot uptime",
    )
    expected_scanners = {
        "ble": session.ble_hardware_id,
        "wifi": session.wifi_hardware_id,
    }
    try:
        flash.verify_scanners(
            status,
            flash.PLATFORMS[CANARY_PLATFORM_KEY],
            ["ble", "wifi"],
            expected_version,
            expected_hardware_ids=expected_scanners,
            require_radio_health=True,
        )
    except Exception as exc:
        raise AcceptanceError(f"scanner proof failed: {exc}") from exc
    by_uart = flash.scanner_status_by_uart(status)
    if set(by_uart) != {"ble", "wifi"}:
        raise AcceptanceError("snapshot must contain exactly both scanner slots")

    reboot_reason = status.get("last_expected_reboot_reason", "")
    if not isinstance(reboot_reason, str) or \
            reboot_reason not in _SAFE_EXPECTED_REBOOT_REASONS:
        raise AcceptanceError(
            "last_expected_reboot_reason is not an allowlisted firmware value"
        )
    reboot_capability, reboot_generation = \
        _bound_snapshot_reboot_generation(
            status,
            expected_version,
            label="badge snapshot",
        )
    return VerifiedBadgeSnapshot(_SNAPSHOT_ISSUER, {
        "version": expected_version,
        "reboot_generation_capability": reboot_capability,
        "reboot_generation": reboot_generation,
        "uplink_hardware_id": session.uplink_hardware_id,
        "ble_hardware_id": session.ble_hardware_id,
        "wifi_hardware_id": session.wifi_hardware_id,
        "running_partition": status.get("running_partition"),
        "uptime_s": uptime,
        "rollback_clear": True,
        "recovery_mode": status.get("recovery_mode"),
        "usb_parser_state": health["parser_state"],
        "usb_rx_bytes": health["rx_bytes"],
        "usb_valid_commands": health["valid_commands"],
        "usb_responses_completed": health["responses_completed"],
        "usb_required_response_failures": health[
            "required_response_failures"
        ],
        "usb_task_heartbeat_age_s": health["task_heartbeat_age_s"],
        "usb_last_response_age_s": health["last_response_age_s"],
        "ble_role": by_uart["ble"].get("slot_role"),
        "wifi_role": by_uart["wifi"].get("slot_role"),
        "radio_health": True,
        "last_expected_reboot_reason": reboot_reason,
    })


def verify_chord_rom_boot_snapshot(
    status: dict[str, object],
    session: BadgeAcceptanceSession,
    expected_version: str,
    *,
    baseline: VerifiedV078UpdaterBaseline,
) -> VerifiedChordRomBootSnapshot:
    """Bind the post-ROM application before maintenance changes provenance."""
    bound_baseline = _validate_v078_baseline_binding(baseline, session)
    generation = _exact_uint32(
        status.get("last_expected_reboot_generation"),
        "chord ROM boot reboot generation",
    )
    if generation != 0:
        raise AcceptanceError(
            "chord ROM boot generation must be exact watchdog-handoff 0"
        )
    if status.get("last_expected_reboot_reason") != "button_usb_rom":
        raise AcceptanceError(
            "chord ROM boot lacks button_usb_rom provenance"
        )
    normalized = dict(status)
    if generation == 0:
        normalized["last_expected_reboot_generation"] = 1
    snapshot = verify_badge_snapshot(
        normalized,
        session,
        expected_version,
    )
    metrics = _live_metrics(
        status,
        "chord ROM boot live sample",
        expected_version=expected_version,
    )
    _require_canary_normal_memory(
        metrics,
        bound_baseline,
        label="chord ROM boot live sample",
    )
    values = dict(snapshot)
    values["reboot_generation"] = generation
    return VerifiedChordRomBootSnapshot(
        _CHORD_ROM_BOOT_SNAPSHOT_ISSUER,
        values,
    )


def verify_cycle_pre_snapshot(
    status: dict[str, object],
    session: BadgeAcceptanceSession,
    candidate_version: str,
    cycle: int,
) -> VerifiedCyclePreSnapshot:
    """Prove exact boards and pre-stage version relation for one cycle."""
    if type(session) is not BadgeAcceptanceSession:
        raise AcceptanceError("acceptance session is malformed")
    if type(cycle) is not int or cycle not in (1, 2, 3):
        raise AcceptanceError("update cycle must be exact integer 1, 2, or 3")
    if not isinstance(status, dict):
        raise AcceptanceError("cycle pre-stage status must be an object")
    if not isinstance(candidate_version, str) or \
            flash.firmware_version_relation(
                candidate_version, candidate_version
            ) != "equal":
        raise AcceptanceError("candidate badge version is invalid")
    try:
        uplink_id = flash.validate_uplink_application_status(status)
    except Exception as exc:
        raise AcceptanceError(
            f"cycle pre-stage uplink proof failed: {exc}"
        ) from exc
    if uplink_id != session.uplink_hardware_id:
        raise AcceptanceError(
            f"cycle pre-stage uplink board changed: got {uplink_id}, "
            f"wanted {session.uplink_hardware_id}"
        )
    uplink_version = status.get("version")
    exact_v078_bootstrap = (
        candidate_version == flash.UPDATE_MAINTENANCE_MIN_VERSION
        and cycle == 1
    )
    wanted_uplink_version = (
        flash.UPDATE_MAINTENANCE_BOOTSTRAP_SOURCE_VERSION
        if exact_v078_bootstrap
        else candidate_version
    )
    if uplink_version != wanted_uplink_version:
        if exact_v078_bootstrap and uplink_version == candidate_version:
            raise AcceptanceError(
                "canary acceptance must return or rollback the uplink to "
                "the exact .78 updater before cycle 1"
            )
        raise AcceptanceError(
            "cycle pre-stage uplink is not on its exact required source "
            "version"
        )
    if status.get("pending_verify") is not False or \
            status.get("rollback_state") != "clear":
        raise AcceptanceError(
            "cycle pre-stage uplink rollback state is not clear"
        )
    if status.get("recovery_mode") != "normal" or \
            status.get("safe_mode") is True:
        raise AcceptanceError(
            "cycle pre-stage uplink recovery state is not normal"
        )
    health = _validate_usb_health(status)
    uptime = _exact_nonnegative_int(
        status.get("uptime_s"),
        f"cycle {cycle} source pre-state uptime",
    )

    platform = flash.PLATFORMS[CANARY_PLATFORM_KEY]
    by_uart = flash.scanner_status_by_uart(status)
    if set(by_uart) != {"ble", "wifi"}:
        raise AcceptanceError(
            "cycle pre-stage status must contain exactly both scanner slots"
        )
    expected_ids = {
        "ble": session.ble_hardware_id,
        "wifi": session.wifi_hardware_id,
    }
    versions: dict[str, str] = {}
    for slot in ("ble", "wifi"):
        info = by_uart[slot]
        current = info.get("ver") or info.get("version")
        if not isinstance(current, str):
            raise AcceptanceError(
                f"{slot} cycle pre-stage version is malformed"
            )
        relation = flash.firmware_version_relation(
            candidate_version, current
        )
        if cycle == 1 and relation != "newer":
            raise AcceptanceError(
                f"cycle 1 {slot} scanner is not strictly older than "
                "the candidate"
            )
        if cycle > 1 and (
            relation != "equal" or current != candidate_version
        ):
            raise AcceptanceError(
                f"cycle {cycle} {slot} scanner is not exactly current"
            )
        try:
            flash.verify_scanners(
                status,
                platform,
                [slot],
                current,
                expected_hardware_ids={slot: expected_ids[slot]},
                require_radio_health=True,
            )
        except Exception as exc:
            raise AcceptanceError(
                f"cycle pre-stage {slot} scanner proof failed: {exc}"
            ) from exc
        versions[slot] = current

    reboot_capability, reboot_generation = \
        _bound_snapshot_reboot_generation(
            status,
            str(uplink_version),
            label=f"cycle {cycle} source pre-state",
        )
    return VerifiedCyclePreSnapshot(_CYCLE_SNAPSHOT_ISSUER, {
        "candidate_version": candidate_version,
        "uplink_version": uplink_version,
        "reboot_generation_capability": reboot_capability,
        "reboot_generation": reboot_generation,
        "uplink_hardware_id": session.uplink_hardware_id,
        "ble_hardware_id": session.ble_hardware_id,
        "wifi_hardware_id": session.wifi_hardware_id,
        "ble_version": versions["ble"],
        "wifi_version": versions["wifi"],
        "running_partition": status.get("running_partition"),
        "uptime_s": uptime,
        "rollback_clear": True,
        "recovery_mode": status.get("recovery_mode"),
        "usb_parser_state": health["parser_state"],
        "usb_rx_bytes": health["rx_bytes"],
        "usb_valid_commands": health["valid_commands"],
        "usb_responses_completed": health["responses_completed"],
        "usb_required_response_failures": health[
            "required_response_failures"
        ],
        "usb_task_heartbeat_age_s": health["task_heartbeat_age_s"],
        "usb_last_response_age_s": health["last_response_age_s"],
        "ble_role": by_uart["ble"].get("slot_role"),
        "wifi_role": by_uart["wifi"].get("slot_role"),
        "radio_health": True,
    })


def _validate_attested_scanner_pre_stage_status(
    status: dict[str, Any],
    session: BadgeAcceptanceSession,
    candidate_version: str,
    cycle: int,
) -> tuple[str, int]:
    """Validate the post-bootstrap normal status captured before staging."""
    if type(status) is not dict:
        raise AcceptanceError(
            "attested scanner pre-stage status must be an object"
        )
    try:
        hardware_id = flash.validate_uplink_application_status(status)
    except Exception as exc:
        raise AcceptanceError(
            f"attested scanner pre-stage uplink proof failed: {exc}"
        ) from exc
    if hardware_id != session.uplink_hardware_id or \
            status.get("version") != candidate_version or \
            status.get("pending_verify") is not False or \
            status.get("rollback_state") != "clear" or \
            status.get("recovery_mode") != "normal" or \
            status.get("safe_mode") is True:
        raise AcceptanceError(
            "attested scanner pre-stage is not the exact healthy canary"
        )
    _validate_usb_health(status)
    capability, generation = _bound_snapshot_reboot_generation(
        status,
        candidate_version,
        label="attested scanner pre-stage",
    )
    if capability != "reported" or generation is None:
        raise AcceptanceError(
            "attested scanner pre-stage lacks reported reboot generation"
        )
    partition = status.get("running_partition")
    if partition not in ("ota_0", "ota_1"):
        raise AcceptanceError(
            "attested scanner pre-stage partition is malformed"
        )
    by_uart = flash.scanner_status_by_uart(status)
    if set(by_uart) != {"ble", "wifi"}:
        raise AcceptanceError(
            "attested scanner pre-stage lacks both exact scanner slots"
        )
    expected_ids = {
        "ble": session.ble_hardware_id,
        "wifi": session.wifi_hardware_id,
    }
    platform = flash.PLATFORMS[CANARY_PLATFORM_KEY]
    for slot in ("ble", "wifi"):
        current = by_uart[slot].get("ver") or \
            by_uart[slot].get("version")
        if not isinstance(current, str):
            raise AcceptanceError(
                f"attested {slot} pre-stage version is malformed"
            )
        relation = flash.firmware_version_relation(
            candidate_version,
            current,
        )
        if cycle == 1 and relation != "newer":
            raise AcceptanceError(
                f"attested cycle 1 {slot} pre-stage is not older"
            )
        if cycle > 1 and (
            relation != "equal" or current != candidate_version
        ):
            raise AcceptanceError(
                f"attested cycle {cycle} {slot} pre-stage is not current"
            )
        try:
            flash.verify_scanners(
                status,
                platform,
                [slot],
                current,
                expected_hardware_ids={slot: expected_ids[slot]},
                require_radio_health=True,
            )
        except Exception as exc:
            raise AcceptanceError(
                f"attested {slot} scanner pre-stage proof failed: {exc}"
            ) from exc
    return str(partition), generation


def _cycle_slot_mask(slots: frozenset[str]) -> int:
    if not slots.issubset({"ble", "wifi"}):
        raise AcceptanceError("cycle proof contains an unknown scanner slot")
    return (1 if "ble" in slots else 0) | (2 if "wifi" in slots else 0)


def _validate_bounded_scanner_attempt_history(
    result: object,
    session: BadgeAcceptanceSession,
    candidate_version: str,
    candidate_artifacts: VerifiedCandidateArtifacts,
) -> tuple[
    tuple[dict[str, Any], ...],
    tuple[dict[str, Any], ...],
    tuple[str, ...],
    frozenset[str],
    dict[str, tuple[str, int]] | None,
]:
    stage_count = getattr(result, "stage_count", None)
    if type(stage_count) is not int or not (
        1 <= stage_count <= flash.UPDATE_HOST_CAMPAIGNS_PER_LANE
    ):
        raise AcceptanceError(
            "update cycle scanner stage count exceeds its bounded budget"
        )
    stage_receipts = getattr(result, "stage_receipts", None)
    if type(stage_receipts) is not tuple or \
            len(stage_receipts) != stage_count or \
            any(type(receipt) is not dict for receipt in stage_receipts):
        raise AcceptanceError(
            "update cycle ordered scanner stage receipts are malformed"
        )
    latest_receipt = getattr(result, "stage_receipt", None)
    if type(latest_receipt) is not dict or \
            stage_receipts[-1] != latest_receipt:
        raise AcceptanceError(
            "update cycle latest scanner receipt is not the history tail"
        )

    platform = flash.PLATFORMS[CANARY_PLATFORM_KEY]
    scanner_artifact = candidate_artifacts["scanner"]
    exact_identity = {
        "ok": True,
        "target": platform["scanner_name"],
        "name": platform["scanner_name"],
        "app_project": platform["scanner_project"],
        "project": platform["scanner_project"],
        "hardware_type": platform["hardware_type"],
        "hardware": platform["hardware_type"],
        "version": candidate_version,
    }
    for receipt in stage_receipts:
        for key, wanted in exact_identity.items():
            got = receipt.get(key)
            if type(got) is not type(wanted) or got != wanted:
                raise AcceptanceError(
                    f"scanner stage history receipt {key} mismatch"
                )
        for receipt_key, identity_key in (
            ("size", "firmware_size"),
            ("crc32", "firmware_crc32"),
            ("sha256", "firmware_sha256"),
        ):
            got = receipt.get(receipt_key)
            wanted = scanner_artifact[identity_key]
            if type(got) is not type(wanted) or got != wanted:
                raise AcceptanceError(
                    f"scanner stage history receipt {receipt_key} does not "
                    "match the anchored candidate artifact"
                )
        generation = receipt.get("generation")
        size = receipt.get("size")
        slot_mask = receipt.get("slot_mask")
        if type(generation) is not int or generation <= 0 or \
                type(size) is not int or size <= 0 or \
                type(slot_mask) is not int or slot_mask not in (1, 2, 3):
            raise AcceptanceError(
                "scanner stage history manifest is malformed"
            )
        for key, wanted in {
            "flow_control": "credit-v1",
            "phase": "final",
            "received": size,
            "total": size,
            "credit_bytes": 0,
        }.items():
            got = receipt.get(key)
            if type(got) is not type(wanted) or got != wanted:
                raise AcceptanceError(
                    "scanner stage history credit-v1 final receipt "
                    f"{key} mismatch"
                )

    attempts = getattr(result, "attempt_history", None)
    if type(attempts) is not tuple:
        raise AcceptanceError(
            "update cycle scanner attempt history is malformed"
        )
    if not attempts:
        if stage_count != 1:
            raise AcceptanceError(
                "multi-stage update cycle lacks immutable attempt history"
            )
        return (
            tuple(copy.deepcopy(receipt) for receipt in stage_receipts),
            (),
            ("ble", "wifi"),
            frozenset(),
            None,
        )
    if len(attempts) != stage_count:
        raise AcceptanceError(
            "scanner attempt history count does not match stage count"
        )

    expected_keys = {
        "ordinal", "session", "requested_slots", "pre_stage_status",
        "stage_receipt", "campaign", "outcome", "classification",
        "recovery_action", "verified_target",
    }
    expected_ids = {
        "ble": session.ble_hardware_id,
        "wifi": session.wifi_hardware_id,
    }
    completed: set[str] = set()
    seen_sessions: set[str] = set()
    campaigns_per_lane = {"ble": 0, "wifi": 0}
    final_states: dict[str, tuple[str, int]] = {}
    latest_slots: tuple[str, ...] = ()
    latest_required: frozenset[str] = frozenset()
    allowed_classifications = {
        None,
        "readiness_exhausted",
        "ota_ack_timeout",
        "offer_manifest_mismatch",
        "deferred_backoff",
    }
    allowed_recovery_actions = {
        None, "already_normal", "session_abort", "usb_reset",
    }

    for ordinal, attempt in enumerate(attempts, start=1):
        if type(attempt) is not dict or set(attempt) != expected_keys or \
                attempt.get("ordinal") != ordinal:
            raise AcceptanceError(
                "scanner attempt history entry schema is malformed"
            )
        try:
            bound_session = flash._validated_update_session(
                attempt.get("session")
            )
        except Exception as exc:
            raise AcceptanceError(
                "scanner attempt maintenance session is malformed"
            ) from exc
        if bound_session in seen_sessions:
            raise AcceptanceError(
                "scanner attempt history reused a maintenance session"
            )
        seen_sessions.add(bound_session)
        requested = attempt.get("requested_slots")
        if type(requested) is not list or not requested or \
                len(requested) != len(set(requested)) or \
                any(slot not in ("ble", "wifi") for slot in requested):
            raise AcceptanceError(
                "scanner attempt requested lanes are malformed"
            )
        if completed.intersection(requested):
            raise AcceptanceError(
                "scanner retry rewrote a previously successful lane"
            )
        for slot in requested:
            campaigns_per_lane[slot] += 1
            if campaigns_per_lane[slot] > \
                    flash.UPDATE_HOST_CAMPAIGNS_PER_LANE:
                raise AcceptanceError(
                    f"{slot} scanner exceeded its campaign retry budget"
                )
        receipt = attempt.get("stage_receipt")
        if receipt != stage_receipts[ordinal - 1] or \
                receipt.get("slot_mask") != _cycle_slot_mask(
                    frozenset(requested)
                ):
            raise AcceptanceError(
                "scanner attempt stage receipt/lane binding mismatch"
            )
        if attempt.get("verified_target") != candidate_version or \
                attempt.get("classification") not in \
                allowed_classifications or \
                attempt.get("recovery_action") not in \
                allowed_recovery_actions:
            raise AcceptanceError(
                "scanner attempt retry authority is malformed"
            )
        pre_stage = attempt.get("pre_stage_status")
        if type(pre_stage) is not dict:
            raise AcceptanceError(
                "scanner attempt pre-stage status is malformed"
            )
        try:
            captured = flash.capture_scanner_hardware_ids(
                pre_stage,
                platform,
                requested,
                require_connected=True,
            )
        except Exception as exc:
            raise AcceptanceError(
                f"scanner attempt identity proof failed: {exc}"
            ) from exc
        if any(captured.get(slot) != expected_ids[slot]
               for slot in requested):
            raise AcceptanceError(
                "scanner attempt hardware identity changed"
            )
        campaign = attempt.get("campaign")
        try:
            validated_campaign = flash._validate_update_campaign_status(
                campaign,
                expected_generation=receipt["generation"],
                expected_slot_mask=receipt["slot_mask"],
            )
        except Exception as exc:
            raise AcceptanceError(
                f"scanner attempt campaign proof failed: {exc}"
            ) from exc
        by_slot = {
            entry["slot"]: entry
            for entry in validated_campaign["scanners"]
        }
        successful: set[str] = set()
        failed: set[str] = set()
        required: set[str] = set()
        pre_by_uart = flash.scanner_status_by_uart(pre_stage)
        for slot in requested:
            current = pre_by_uart[slot].get("ver") or \
                pre_by_uart[slot].get("version")
            relation = flash.firmware_version_relation(
                candidate_version, str(current or "")
            )
            if relation == "newer":
                required.add(slot)
            elif relation != "equal":
                raise AcceptanceError(
                    f"{slot} scanner retry pre-stage version is unsafe"
                )
            entry = by_slot[0 if slot == "ble" else 1]
            state = entry["state"]
            if state in ("converged", "current"):
                if slot in required and state != "converged":
                    raise AcceptanceError(
                        f"{slot} scanner older pre-stage did not converge"
                    )
                successful.add(slot)
                final_states[slot] = (state, entry["attempts"])
            elif state == "failed":
                failed.add(slot)
            else:
                raise AcceptanceError(
                    f"{slot} scanner attempt ended ambiguously: {state}"
                )
        outcome = attempt.get("outcome")
        if outcome == "failed":
            if not failed or successful != set(requested) - failed or \
                    attempt.get("classification") is None or \
                    attempt.get("recovery_action") is None:
                raise AcceptanceError(
                    "failed scanner attempt lacks exact recovery authority"
                )
        elif outcome == "converged":
            if failed or successful != set(requested) or \
                    ordinal != len(attempts):
                raise AcceptanceError(
                    "scanner convergence history is not terminal"
                )
        else:
            raise AcceptanceError(
                "scanner attempt outcome is malformed"
            )
        completed.update(successful)
        latest_slots = tuple(requested)
        latest_required = frozenset(required)

    if attempts[-1].get("outcome") != "converged" or \
            completed != {"ble", "wifi"}:
        raise AcceptanceError(
            "scanner attempt history does not prove both original lanes"
        )
    return (
        tuple(copy.deepcopy(receipt) for receipt in stage_receipts),
        tuple(copy.deepcopy(attempt) for attempt in attempts),
        latest_slots,
        latest_required,
        final_states,
    )


def verify_update_cycle_result(
    result: object,
    session: BadgeAcceptanceSession,
    candidate_version: str,
    cycle: int,
    *,
    candidate_artifacts: VerifiedCandidateArtifacts,
    source_pre_snapshot: VerifiedCyclePreSnapshot,
    final_maintenance_sample: VerifiedLiveMetricSample,
    updater_baseline: VerifiedV078UpdaterBaseline | None = None,
) -> VerifiedCycleCheckpoint:
    """Convert one successful production USB flow into bounded evidence."""
    try:
        attested_result = flash._revalidate_usb_scanner_flow_result(result)
    except Exception as exc:
        raise AcceptanceError(
            "update cycle did not return production-issued scanner proof"
        ) from exc
    if attested_result is not result:
        raise AcceptanceError(
            "production scanner proof revalidation changed identity"
        )
    if type(cycle) is not int or cycle not in (1, 2, 3):
        raise AcceptanceError("update cycle must be exact integer 1, 2, or 3")
    candidate_artifacts = _validate_candidate_artifacts(
        candidate_artifacts,
        candidate_version,
    )
    (
        stage_receipts,
        attempt_history,
        latest_slots,
        latest_required_slots,
        historical_states,
    ) = _validate_bounded_scanner_attempt_history(
        result,
        session,
        candidate_version,
        candidate_artifacts,
    )
    if candidate_version == flash.UPDATE_MAINTENANCE_MIN_VERSION:
        updater_baseline = _validate_v078_baseline_binding(
            updater_baseline,
            session,
        )
    elif updater_baseline is not None:
        raise AcceptanceError(
            "non-canary cycle cannot carry a v0.78 updater baseline"
        )
    if result.theme_restored is not True or \
            result.fresh_usb_proven is not True:
        raise AcceptanceError(
            "update cycle lacks restored-theme fresh USB proof"
        )
    if type(result.preflight_older_slots) is not frozenset or \
            type(result.recovery_slots) is not frozenset:
        raise AcceptanceError("update cycle slot proofs are malformed")

    wanted_older = frozenset({"ble", "wifi"} if cycle == 1 else ())
    wanted_recovery = frozenset(() if cycle == 1 else {"ble", "wifi"})
    if result.preflight_older_slots != wanted_older:
        raise AcceptanceError(
            f"cycle {cycle} preflight older-slot proof is incomplete"
        )
    if result.recovery_slots != wanted_recovery:
        raise AcceptanceError(
            f"cycle {cycle} recovery relay-slot proof is incomplete"
        )

    _validate_cycle_pre_snapshot(
        session,
        source_pre_snapshot,
        candidate_version,
        cycle,
    )
    pre_status = result.pre_stage_status
    restored_status = result.final_restored_status
    stage_receipt = result.stage_receipt
    attested_partition, attested_generation = \
        _validate_attested_scanner_pre_stage_status(
            pre_status,
            session,
            candidate_version,
            cycle,
        )
    if cycle == 1:
        if attested_generation != 1 or attested_partition == \
                source_pre_snapshot["running_partition"]:
            raise AcceptanceError(
                "cycle 1 attested pre-stage is not the exact alternate "
                "generation-1 canary boot"
            )
    elif attested_partition != source_pre_snapshot["running_partition"] or \
            attested_generation != source_pre_snapshot["reboot_generation"]:
        raise AcceptanceError(
            f"cycle {cycle} attested pre-stage changed the reserved "
            "source boot"
        )
    pre_snapshot = source_pre_snapshot
    post_snapshot = verify_badge_snapshot(
        restored_status, session, candidate_version
    )
    if post_snapshot["last_expected_reboot_reason"] != "update_finish":
        raise AcceptanceError(
            "cycle final normal snapshot lacks update_finish provenance"
        )
    final_maintenance = _validate_live_sample_binding(
        final_maintenance_sample,
        session,
        candidate_version,
    )
    if final_maintenance["recovery_mode"] != "update_maintenance" or \
            post_snapshot["running_partition"] != \
            final_maintenance["running_partition"] or \
            post_snapshot["reboot_generation"] != \
            _expected_reboot_successor(
                int(final_maintenance["reboot_generation"])
            ):
        raise AcceptanceError(
            "cycle final normal status is not the same-partition exact "
            "successor of its last maintenance boot"
        )
    if not isinstance(stage_receipt, dict):
        raise AcceptanceError("scanner stage receipt is malformed")
    platform = flash.PLATFORMS[CANARY_PLATFORM_KEY]
    exact_stage_identity = {
        "ok": True,
        "target": platform["scanner_name"],
        "name": platform["scanner_name"],
        "app_project": platform["scanner_project"],
        "project": platform["scanner_project"],
        "hardware_type": platform["hardware_type"],
        "hardware": platform["hardware_type"],
        "version": candidate_version,
        "slot_mask": _cycle_slot_mask(frozenset(latest_slots)),
    }
    for key, wanted in exact_stage_identity.items():
        got = stage_receipt.get(key)
        if type(got) is not type(wanted) or got != wanted:
            raise AcceptanceError(
                f"scanner stage receipt {key} mismatch"
            )
    scanner_artifact = candidate_artifacts["scanner"]
    for receipt_key, identity_key in (
        ("size", "firmware_size"),
        ("crc32", "firmware_crc32"),
        ("sha256", "firmware_sha256"),
    ):
        got = stage_receipt.get(receipt_key)
        wanted = scanner_artifact[identity_key]
        if type(got) is not type(wanted) or got != wanted:
            raise AcceptanceError(
                f"scanner stage receipt {receipt_key} does not match the "
                "anchored candidate artifact"
            )
    generation = stage_receipt.get("generation")
    if type(generation) is not int or generation <= 0:
        raise AcceptanceError("scanner stage generation is malformed")
    stage_size = stage_receipt.get("size")
    if type(stage_size) is not int or stage_size <= 0:
        raise AcceptanceError(
            "scanner credit-v1 final receipt size is malformed"
        )
    exact_credit_final = {
        "flow_control": "credit-v1",
        "phase": "final",
        "received": stage_size,
        "total": stage_size,
        "credit_bytes": 0,
    }
    for key, wanted in exact_credit_final.items():
        got = stage_receipt.get(key)
        if type(got) is not type(wanted) or got != wanted:
            raise AcceptanceError(
                f"scanner credit-v1 final receipt {key} mismatch"
            )
    try:
        flash.verify_auto_update_convergence(
            restored_status,
            list(latest_slots),
            expected_stage_receipt=stage_receipt,
            required_converged_slots=set(
                latest_required_slots
                if attempt_history
                else frozenset(wanted_older).intersection(latest_slots)
            ),
        )
    except Exception as exc:
        raise AcceptanceError(
            f"scanner coordinator convergence proof failed: {exc}"
        ) from exc

    store = restored_status.get("firmware_store")
    auto = store.get("auto_update") if isinstance(store, dict) else None
    entries = auto.get("scanners") if isinstance(auto, dict) else None
    if not isinstance(entries, list):
        raise AcceptanceError("scanner coordinator slot proof is missing")
    by_slot = {
        entry.get("slot"): entry
        for entry in entries
        if isinstance(entry, dict) and entry.get("slot") in (0, 1)
    }
    if set(by_slot) != {0, 1}:
        raise AcceptanceError("scanner coordinator slot proof is incomplete")

    states: dict[str, object] = {}
    for slot_id, slot_name in ((0, "ble"), (1, "wifi")):
        state = by_slot[slot_id].get("state")
        attempts = by_slot[slot_id].get("attempts")
        if not isinstance(state, str) or type(attempts) is not int:
            raise AcceptanceError(
                f"{slot_name} coordinator result is malformed"
            )
        states[f"{slot_name}_state"] = state
        states[f"{slot_name}_attempts"] = attempts
    if historical_states is not None:
        for slot_name in ("ble", "wifi"):
            state, attempts = historical_states[slot_name]
            states[f"{slot_name}_state"] = state
            states[f"{slot_name}_attempts"] = attempts

    pending_after = auto.get("pending_mask")
    if type(pending_after) is not int or pending_after != 0:
        raise AcceptanceError("scanner coordinator pending mask is not clear")
    older_mask = _cycle_slot_mask(result.preflight_older_slots)
    recovery_mask = _cycle_slot_mask(result.recovery_slots)
    return VerifiedCycleCheckpoint(_CYCLE_CHECKPOINT_ISSUER, {
        "cycle": cycle,
        "candidate_version": candidate_version,
        "candidate_artifacts": candidate_artifacts,
        "updater_baseline": updater_baseline,
        "pre_snapshot": pre_snapshot,
        "snapshot": post_snapshot,
        "recovery_rewrite_same_version": cycle > 1,
        "scanner_uploads": result.stage_count,
        "manual_relay_commands": len(result.recovery_slots),
        "stage_generation": generation,
        "slot_mask": stage_receipt["slot_mask"],
        "scanner_flow_control": stage_receipt["flow_control"],
        "scanner_stage_phase": stage_receipt["phase"],
        "scanner_stage_received": stage_receipt["received"],
        "scanner_stage_total": stage_receipt["total"],
        "scanner_stage_crc32": stage_receipt["crc32"],
        "scanner_stage_sha256": stage_receipt["sha256"],
        "scanner_stage_credit_bytes": stage_receipt["credit_bytes"],
        "pending_mask_after": pending_after,
        "preflight_older_slot_mask": older_mask,
        "recovery_slot_mask": recovery_mask,
        "ble_state": states["ble_state"],
        "ble_attempts": states["ble_attempts"],
        "wifi_state": states["wifi_state"],
        "wifi_attempts": states["wifi_attempts"],
        "fresh_ping_status": True,
        "theme_restored": True,
    })


def run_update_cycle_checkpoint(
    port: str,
    session: BadgeAcceptanceSession,
    cycle: int,
    *,
    expected_version: str | None = None,
    frozen_artifacts: flash.FrozenUsbFirmwareArtifacts | None = None,
    candidate_artifacts: VerifiedCandidateArtifacts | None = None,
    expected_pre_snapshot: VerifiedCyclePreSnapshot | None = None,
    updater_baseline: VerifiedV078UpdaterBaseline | None = None,
) -> VerifiedCycleCheckpoint:
    """Run one fixed production flasher cycle and return machine proof."""
    if not isinstance(port, str) or not port:
        raise AcceptanceError("update cycle requires a live badge port")
    if type(session) is not BadgeAcceptanceSession:
        raise AcceptanceError("update cycle session is malformed")
    if type(cycle) is not int or cycle not in (1, 2, 3):
        raise AcceptanceError("update cycle must be exact integer 1, 2, or 3")
    if expected_version is None:
        version = flash.repo_version(
            flash.PLATFORMS[CANARY_PLATFORM_KEY]
        )
    else:
        if not isinstance(expected_version, str) or \
                flash.firmware_version_relation(
                    expected_version, expected_version
                ) != "equal":
            raise AcceptanceError(
                "anchored update candidate version is malformed"
            )
        version = expected_version
    bound_updater_baseline: VerifiedV078UpdaterBaseline | None = None
    if version == flash.UPDATE_MAINTENANCE_MIN_VERSION:
        bound_updater_baseline = _validate_v078_baseline_binding(
            updater_baseline,
            session,
        )
    platform = flash.PLATFORMS[CANARY_PLATFORM_KEY]
    slots = ["ble", "wifi"]
    if frozen_artifacts is None:
        flash.require_artifacts(platform, True, slots)
        frozen_artifacts = flash._prepare_frozen_usb_firmware_artifacts(
            platform,
            True,
            slots,
        )
    derived_artifacts = verify_candidate_artifacts(
        frozen_artifacts,
        version,
    )
    if candidate_artifacts is None:
        candidate_artifacts = derived_artifacts
    else:
        _validate_candidate_artifacts(candidate_artifacts, version)
        if dict(candidate_artifacts) != dict(derived_artifacts):
            raise AcceptanceError(
                "frozen update artifacts changed from the anchored Gate 1 "
                "candidate"
            )
    if expected_pre_snapshot is not None:
        _validate_cycle_pre_snapshot(
            session,
            expected_pre_snapshot,
            version,
            cycle,
        )
    descriptor = _trusted_session_uplink_descriptor(port, session)
    flow_args = argparse.Namespace(
        platform=CANARY_PLATFORM_KEY,
        port=descriptor.device,
        dry_run=False,
        bind_selected_uplink=False,
        trusted_uplink_binding=flash.TrustedUplinkBinding(
            serial_number=descriptor.serial_number,
            location=descriptor.location,
            source="retained-session",
        ),
        recovery_rewrite_same_version=cycle > 1,
        skip_command_probe=False,
    )
    live_sample: VerifiedLiveMetricSample | None = None
    source_pre_snapshot: VerifiedCyclePreSnapshot | None = None

    def pre_mutation_validator(
        application_status: dict[str, Any] | None,
        supplied_frozen: flash.FrozenUsbFirmwareArtifacts,
    ) -> None:
        nonlocal live_sample, source_pre_snapshot
        if supplied_frozen is not frozen_artifacts:
            raise AcceptanceError(
                "USB flow changed the prevalidated frozen artifact object"
            )
        if application_status is None:
            raise AcceptanceError(
                "update cycle lost the anchored uplink application before "
                "mutation"
            )
        observed = verify_cycle_pre_snapshot(
            application_status,
            session,
            version,
            cycle,
        )
        source_pre_snapshot = observed
        if expected_pre_snapshot is not None:
            stable_keys = (
                "candidate_version",
                "uplink_hardware_id",
                "ble_hardware_id",
                "wifi_hardware_id",
                "ble_version",
                "wifi_version",
                "running_partition",
                "rollback_clear",
                "recovery_mode",
                "ble_role",
                "wifi_role",
                "radio_health",
            )
            if any(
                observed[key] != expected_pre_snapshot[key]
                for key in stable_keys
            ):
                raise AcceptanceError(
                    "live cycle pre-state changed after its read-only "
                    "acceptance preflight"
                )
            if cycle > 1:
                _require_exact_cycle_probe_transition(
                    expected_pre_snapshot,
                    observed,
                    probe_count=1,
                    label=f"cycle {cycle} in-flow source probe",
                )
        if bound_updater_baseline is not None:
            if cycle == 1:
                live_sample = verify_v078_updater_live_successor(
                    application_status,
                    session,
                    baseline=bound_updater_baseline,
                )
            else:
                live_sample = verify_canary_normal_live_metrics(
                    application_status,
                    session,
                    version,
                    baseline=bound_updater_baseline,
                )

    def maintenance_status_validator(
        status: dict[str, Any],
        update_session: str,
    ) -> None:
        nonlocal live_sample
        if bound_updater_baseline is None or live_sample is None:
            raise AcceptanceError(
                "maintenance bytes lack a bound normal live sample"
            )
        live_sample = verify_canary_maintenance_live_metrics(
            status,
            session,
            version,
            update_session,
            baseline=bound_updater_baseline,
            previous=live_sample,
        )

    def post_direct_bootstrap_status_validator(
        status: dict[str, Any],
    ) -> None:
        nonlocal live_sample
        if cycle != 1 or bound_updater_baseline is None or \
                live_sample is None:
            raise AcceptanceError(
                "post-bootstrap .79 lacks its exact .78 live lineage"
            )
        live_sample = verify_canary_post_direct_bootstrap_live_metrics(
            status,
            session,
            version,
            baseline=bound_updater_baseline,
            previous=live_sample,
        )

    result = flash.usb_flow(
        flow_args,
        platform,
        True,
        slots,
        version,
        pre_mutation_validator=pre_mutation_validator,
        maintenance_status_validator=(
            maintenance_status_validator
            if bound_updater_baseline is not None
            else None
        ),
        post_direct_bootstrap_status_validator=(
            post_direct_bootstrap_status_validator
            if cycle == 1 and bound_updater_baseline is not None
            else None
        ),
        frozen_artifacts=frozen_artifacts,
    )
    if source_pre_snapshot is None:
        raise AcceptanceError(
            "update cycle lacks its verifier-issued source pre-snapshot"
        )
    if live_sample is None or \
            live_sample.get("recovery_mode") != "update_maintenance":
        raise AcceptanceError(
            "update cycle lacks its final bound maintenance live sample"
        )
    return verify_update_cycle_result(
        result,
        session,
        version,
        cycle,
        candidate_artifacts=candidate_artifacts,
        source_pre_snapshot=source_pre_snapshot,
        final_maintenance_sample=live_sample,
        updater_baseline=bound_updater_baseline,
    )


def verify_chord_rom_recovery_result(
    result: object,
    session: BadgeAcceptanceSession,
    candidate_version: str,
    *,
    candidate_artifacts: VerifiedCandidateArtifacts,
    rom_boot_snapshot: VerifiedChordRomBootSnapshot,
    final_maintenance_sample: VerifiedLiveMetricSample,
    updater_baseline: VerifiedV078UpdaterBaseline,
) -> VerifiedChordRecoveryFacts:
    """Bind one production ROM flow without weakening normal cycle lineage."""
    try:
        attested_result = flash._revalidate_usb_scanner_flow_result(result)
    except Exception as exc:
        raise AcceptanceError(
            "chord recovery did not return production-issued scanner proof"
        ) from exc
    if attested_result is not result:
        raise AcceptanceError(
            "chord scanner proof revalidation changed identity"
        )
    candidate_artifacts = _validate_candidate_artifacts(
        candidate_artifacts,
        candidate_version,
    )
    (
        _stage_receipts,
        chord_attempt_history,
        chord_latest_slots,
        chord_latest_required,
        _historical_states,
    ) = _validate_bounded_scanner_attempt_history(
        result,
        session,
        candidate_version,
        candidate_artifacts,
    )
    updater_baseline = _validate_v078_baseline_binding(
        updater_baseline,
        session,
    )
    rom_boot = _validate_chord_rom_boot_snapshot(
        rom_boot_snapshot,
        session,
        candidate_version,
    )
    if result.theme_restored is not True or \
            result.fresh_usb_proven is not True:
        raise AcceptanceError(
            "chord recovery lacks bounded restored-theme fresh USB proof"
        )
    if result.preflight_older_slots != frozenset() or \
            result.recovery_slots != frozenset({"ble", "wifi"}):
        raise AcceptanceError(
            "chord recovery lacks exact same-version two-UART relay proof"
        )

    pre_stage = verify_chord_rom_boot_snapshot(
        result.pre_stage_status,
        session,
        candidate_version,
        baseline=updater_baseline,
    )
    for key in (
        "version",
        "reboot_generation_capability",
        "reboot_generation",
        "uplink_hardware_id",
        "ble_hardware_id",
        "wifi_hardware_id",
        "running_partition",
        "rollback_clear",
        "recovery_mode",
        "ble_role",
        "wifi_role",
        "radio_health",
        "last_expected_reboot_reason",
    ):
        if pre_stage[key] != rom_boot[key]:
            raise AcceptanceError(
                f"chord scanner pre-stage changed ROM boot {key}"
            )
    if pre_stage["uptime_s"] < rom_boot["uptime_s"]:
        raise AcceptanceError("chord scanner pre-stage uptime moved backward")
    for key in (
        "usb_rx_bytes",
        "usb_valid_commands",
        "usb_responses_completed",
    ):
        if pre_stage[key] < rom_boot[key]:
            raise AcceptanceError(
                f"chord scanner pre-stage {key} moved backward"
            )

    final_snapshot = verify_badge_snapshot(
        result.final_restored_status,
        session,
        candidate_version,
    )
    if final_snapshot["last_expected_reboot_reason"] != "update_finish":
        raise AcceptanceError(
            "chord final application lacks update_finish provenance"
        )
    final_maintenance = _validate_live_sample_binding(
        final_maintenance_sample,
        session,
        candidate_version,
    )
    if final_maintenance["recovery_mode"] != "update_maintenance" or \
            final_snapshot["running_partition"] != \
            final_maintenance["running_partition"] or \
            final_snapshot["reboot_generation"] != \
            _expected_reboot_successor(
                int(final_maintenance["reboot_generation"])
            ):
        raise AcceptanceError(
            "chord final application is not the same-partition exact "
            "successor of its last maintenance boot"
        )

    stage_receipt = result.stage_receipt
    if not isinstance(stage_receipt, dict):
        raise AcceptanceError("chord scanner stage receipt is malformed")
    platform = flash.PLATFORMS[CANARY_PLATFORM_KEY]
    for key, wanted in {
        "ok": True,
        "target": platform["scanner_name"],
        "name": platform["scanner_name"],
        "app_project": platform["scanner_project"],
        "project": platform["scanner_project"],
        "hardware_type": platform["hardware_type"],
        "hardware": platform["hardware_type"],
        "version": candidate_version,
        "slot_mask": _cycle_slot_mask(
            frozenset(chord_latest_slots)
        ),
    }.items():
        got = stage_receipt.get(key)
        if type(got) is not type(wanted) or got != wanted:
            raise AcceptanceError(
                f"chord scanner stage receipt {key} mismatch"
            )
    scanner_artifact = candidate_artifacts["scanner"]
    for receipt_key, identity_key in (
        ("size", "firmware_size"),
        ("crc32", "firmware_crc32"),
        ("sha256", "firmware_sha256"),
    ):
        got = stage_receipt.get(receipt_key)
        wanted = scanner_artifact[identity_key]
        if type(got) is not type(wanted) or got != wanted:
            raise AcceptanceError(
                f"chord scanner stage receipt {receipt_key} changed "
                "candidate identity"
            )
    generation = stage_receipt.get("generation")
    stage_size = stage_receipt.get("size")
    if type(generation) is not int or generation <= 0 or \
            type(stage_size) is not int or stage_size <= 0:
        raise AcceptanceError(
            "chord scanner stage generation or size is malformed"
        )
    for key, wanted in {
        "flow_control": "credit-v1",
        "phase": "final",
        "received": stage_size,
        "total": stage_size,
        "credit_bytes": 0,
    }.items():
        got = stage_receipt.get(key)
        if type(got) is not type(wanted) or got != wanted:
            raise AcceptanceError(
                f"chord scanner final receipt {key} mismatch"
            )
    try:
        flash.verify_auto_update_convergence(
            result.final_restored_status,
            list(chord_latest_slots),
            expected_stage_receipt=stage_receipt,
            required_converged_slots=set(
                chord_latest_required
                if chord_attempt_history
                else ()
            ),
        )
    except Exception as exc:
        raise AcceptanceError(
            f"chord scanner coordinator proof failed: {exc}"
        ) from exc
    store = result.final_restored_status.get("firmware_store")
    auto = store.get("auto_update") if isinstance(store, dict) else None
    if not isinstance(auto, dict) or \
            auto.get("pending_mask") != 0 or \
            type(auto.get("pending_mask")) is not int:
        raise AcceptanceError(
            "chord scanner coordinator pending mask is not clear"
        )

    return VerifiedChordRecoveryFacts(
        _CHORD_RECOVERY_FACT_ISSUER,
        {
            "snapshot": final_snapshot,
            "rom_boot_snapshot": rom_boot,
            "candidate_artifacts": candidate_artifacts,
            "usb_data_host_attached": True,
            "hold_ms": 10_000,
            "rom_enumerated": True,
            "base_mac_continuity": True,
            "full_layout_verified": True,
            "application_returned": True,
            "scanner_staged_once": True,
            "both_uart_updates": True,
            "last_expected_reboot_reason": "button_usb_rom",
        },
    )


def run_chord_rom_recovery_gate(
    port: str,
    session: BadgeAcceptanceSession,
    *,
    expected_version: str | None = None,
    frozen_artifacts: flash.FrozenUsbFirmwareArtifacts | None = None,
    candidate_artifacts: VerifiedCandidateArtifacts | None = None,
    updater_baseline: VerifiedV078UpdaterBaseline | None = None,
) -> dict[str, object]:
    """Run one retained-session ROM recovery and derive Gate 4 proof."""
    if not isinstance(port, str) or not port:
        raise AcceptanceError(
            "chord ROM recovery requires an explicit current ROM port"
        )
    if type(session) is not BadgeAcceptanceSession:
        raise AcceptanceError("chord ROM recovery session is malformed")

    version = (
        flash.repo_version(
            flash.PLATFORMS[CANARY_PLATFORM_KEY]
        )
        if expected_version is None
        else expected_version
    )
    if version != flash.UPDATE_MAINTENANCE_MIN_VERSION:
        raise AcceptanceError(
            "chord ROM recovery requires the exact .79 canary"
        )
    bound_updater_baseline = _validate_v078_baseline_binding(
        updater_baseline,
        session,
    )
    platform = flash.PLATFORMS[CANARY_PLATFORM_KEY]
    slots = ["ble", "wifi"]
    if frozen_artifacts is None:
        flash.require_artifacts(platform, True, slots)
        frozen_artifacts = flash._prepare_frozen_usb_firmware_artifacts(
            platform,
            True,
            slots,
        )
    derived_artifacts = verify_candidate_artifacts(
        frozen_artifacts,
        version,
    )
    if candidate_artifacts is None:
        candidate_artifacts = derived_artifacts
    else:
        _validate_candidate_artifacts(candidate_artifacts, version)
        if dict(candidate_artifacts) != dict(derived_artifacts):
            raise AcceptanceError(
                "chord recovery artifacts changed from the anchored "
                "candidate"
            )
    flow_args = argparse.Namespace(
        platform=CANARY_PLATFORM_KEY,
        port=port,
        dry_run=False,
        bind_selected_uplink=False,
        trusted_uplink_binding=flash.TrustedUplinkBinding(
            serial_number=session.uplink_hardware_id,
            # The private session supplies cross-chord board authority. The
            # USB flow freezes artifacts first, resolves this explicit ROM
            # path by that serial, then strengthens the binding to the
            # descriptor's exact physical location before opening it.
            location=None,
            source="retained-session",
        ),
        recovery_rewrite_same_version=True,
        skip_command_probe=False,
        require_rom_recovery=True,
    )
    rom_boot_snapshot: VerifiedChordRomBootSnapshot | None = None
    live_sample: VerifiedLiveMetricSample | None = None

    def validate_post_rom_boot(status: dict[str, Any]) -> None:
        nonlocal rom_boot_snapshot
        if rom_boot_snapshot is not None:
            raise AcceptanceError(
                "chord ROM flow emitted duplicate post-ROM proof"
            )
        rom_boot_snapshot = verify_chord_rom_boot_snapshot(
            status,
            session,
            version,
            baseline=bound_updater_baseline,
        )

    def validate_maintenance(
        status: dict[str, Any],
        update_session: str,
    ) -> None:
        nonlocal live_sample
        if rom_boot_snapshot is None:
            raise AcceptanceError(
                "chord maintenance began before post-ROM proof"
            )
        if live_sample is None:
            live_sample = verify_chord_first_maintenance_live_metrics(
                status,
                session,
                version,
                update_session,
                baseline=bound_updater_baseline,
                previous=rom_boot_snapshot,
            )
        else:
            live_sample = verify_canary_maintenance_live_metrics(
                status,
                session,
                version,
                update_session,
                baseline=bound_updater_baseline,
                previous=live_sample,
            )

    result = flash.usb_flow(
        flow_args,
        platform,
        True,
        slots,
        version,
        maintenance_status_validator=validate_maintenance,
        post_rom_bootstrap_status_validator=validate_post_rom_boot,
        frozen_artifacts=frozen_artifacts,
    )
    if rom_boot_snapshot is None or live_sample is None:
        raise AcceptanceError(
            "chord ROM flow omitted ROM or maintenance live proof"
        )
    facts = verify_chord_rom_recovery_result(
        result,
        session,
        version,
        candidate_artifacts=candidate_artifacts,
        rom_boot_snapshot=rom_boot_snapshot,
        final_maintenance_sample=live_sample,
        updater_baseline=bound_updater_baseline,
    )
    _validate_pass_facts(
        session, "chord-rom-recovery", facts
    )
    return facts


def _require_bool(facts: dict[str, Any], key: str, value: bool) -> None:
    if type(facts.get(key)) is not bool or facts.get(key) is not value:
        raise AcceptanceError(
            f"PASS gate requires {key}={str(value).lower()}"
        )


def _require_exact(facts: dict[str, Any], key: str, value: Any) -> None:
    got = facts.get(key)
    if type(got) is not type(value) or got != value:
        raise AcceptanceError(f"PASS gate requires {key}={value!r}")


def _validate_verified_snapshot(
    session: BadgeAcceptanceSession,
    snapshot: object,
    *,
    expected_version: str | None = None,
) -> str:
    if type(snapshot) is not VerifiedBadgeSnapshot:
        raise AcceptanceError(
            "PASS gate requires a verifier-issued live badge snapshot"
        )
    if set(snapshot) != _SNAPSHOT_KEYS:
        raise AcceptanceError("verified snapshot schema mismatch")
    exact_snapshot = {
        "uplink_hardware_id": session.uplink_hardware_id,
        "ble_hardware_id": session.ble_hardware_id,
        "wifi_hardware_id": session.wifi_hardware_id,
        "rollback_clear": True,
        "usb_parser_state": "command",
        "radio_health": True,
        "recovery_mode": "normal",
        "ble_role": "ble_primary",
        "wifi_role": "wifi_primary",
        "usb_required_response_failures": 0,
    }
    for key, wanted in exact_snapshot.items():
        got = snapshot.get(key)
        if type(got) is not type(wanted) or got != wanted:
            raise AcceptanceError(
                f"PASS snapshot requires {key}={wanted!r}"
            )
    version = snapshot.get("version")
    if not isinstance(version, str) or \
            flash.firmware_version_relation(version, version) != "equal":
        raise AcceptanceError("PASS snapshot version is malformed")
    if expected_version is not None and version != expected_version:
        raise AcceptanceError(
            f"PASS snapshot version mismatch: got {version!r}, "
            f"wanted {expected_version!r}"
        )
    if snapshot.get("reboot_generation_capability") != "reported":
        raise AcceptanceError(
            "PASS snapshot lacks reported reboot-generation capability"
        )
    _exact_uint32(
        snapshot.get("reboot_generation"),
        "PASS snapshot reboot generation",
        nonzero=True,
    )
    if snapshot.get("running_partition") not in ("ota_0", "ota_1"):
        raise AcceptanceError("PASS snapshot partition is malformed")
    _exact_nonnegative_int(
        snapshot.get("uptime_s"),
        "PASS snapshot uptime",
    )
    for key, label in (
        ("usb_rx_bytes", "PASS snapshot RX bytes"),
        ("usb_valid_commands", "PASS snapshot valid commands"),
        ("usb_responses_completed", "PASS snapshot USB responses"),
    ):
        _exact_uint32(snapshot.get(key), label, nonzero=True)
    for key in (
        "usb_task_heartbeat_age_s",
        "usb_last_response_age_s",
    ):
        value = snapshot.get(key)
        if value is not None and (type(value) is not int or value < 0):
            raise AcceptanceError(f"PASS snapshot {key} is malformed")
    reboot_reason = snapshot.get("last_expected_reboot_reason")
    if not isinstance(reboot_reason, str) or \
            reboot_reason not in _SAFE_EXPECTED_REBOOT_REASONS:
        raise AcceptanceError(
            "PASS snapshot reboot reason is not allowlisted"
        )
    return version


def _validate_chord_rom_boot_snapshot(
    snapshot: object,
    session: BadgeAcceptanceSession,
    expected_version: str,
) -> VerifiedChordRomBootSnapshot:
    if type(snapshot) is not VerifiedChordRomBootSnapshot or \
            set(snapshot) != _SNAPSHOT_KEYS:
        raise AcceptanceError(
            "chord ROM boot snapshot must be verifier-issued"
        )
    generation = _exact_uint32(
        snapshot.get("reboot_generation"),
        "chord ROM boot reboot generation",
    )
    if generation != 0 or \
            snapshot.get("last_expected_reboot_reason") != \
            "button_usb_rom":
        raise AcceptanceError(
            "chord ROM boot snapshot lacks exact generation-0 "
            "button_usb_rom provenance"
        )
    normalized = dict(snapshot)
    normalized["reboot_generation"] = 1
    _validate_verified_snapshot(
        session,
        VerifiedBadgeSnapshot(_SNAPSHOT_ISSUER, normalized),
        expected_version=expected_version,
    )
    return snapshot


def _validate_cycle_pre_snapshot(
    session: BadgeAcceptanceSession,
    pre: object,
    candidate_version: str,
    cycle: int,
) -> None:
    if type(pre) is not VerifiedCyclePreSnapshot or \
            set(pre) != _CYCLE_PRE_SNAPSHOT_KEYS:
        raise AcceptanceError(
            "cycle checkpoint pre-state must be verifier-issued"
        )
    exact_values = {
        "candidate_version": candidate_version,
        "uplink_version": (
            flash.UPDATE_MAINTENANCE_BOOTSTRAP_SOURCE_VERSION
            if candidate_version == flash.UPDATE_MAINTENANCE_MIN_VERSION
            and cycle == 1
            else candidate_version
        ),
        "uplink_hardware_id": session.uplink_hardware_id,
        "ble_hardware_id": session.ble_hardware_id,
        "wifi_hardware_id": session.wifi_hardware_id,
        "rollback_clear": True,
        "recovery_mode": "normal",
        "usb_parser_state": "command",
        "usb_required_response_failures": 0,
        "ble_role": "ble_primary",
        "wifi_role": "wifi_primary",
        "radio_health": True,
    }
    for key, wanted in exact_values.items():
        got = pre.get(key)
        if type(got) is not type(wanted) or got != wanted:
            raise AcceptanceError(
                f"cycle {cycle} pre-state requires {key}={wanted!r}"
            )
    if cycle == 1 and \
            candidate_version == flash.UPDATE_MAINTENANCE_MIN_VERSION:
        if pre.get("reboot_generation_capability") != \
                _LEGACY_V078_GENERATION_CAPABILITY or \
                pre.get("reboot_generation") is not None:
            raise AcceptanceError(
                "cycle 1 exact .78 pre-state must report absent generation"
            )
    else:
        if pre.get("reboot_generation_capability") != "reported":
            raise AcceptanceError(
                f"cycle {cycle} pre-state lacks reported generation"
            )
        _exact_uint32(
            pre.get("reboot_generation"),
            f"cycle {cycle} pre-state reboot generation",
            nonzero=True,
        )
    if pre.get("running_partition") not in ("ota_0", "ota_1"):
        raise AcceptanceError(
            f"cycle {cycle} pre-state partition is malformed"
        )
    _exact_nonnegative_int(
        pre.get("uptime_s"),
        f"cycle {cycle} pre-state uptime",
    )
    for key, label in (
        ("usb_rx_bytes", "RX bytes"),
        ("usb_valid_commands", "valid commands"),
        ("usb_responses_completed", "USB responses"),
    ):
        _exact_uint32(
            pre.get(key),
            f"cycle {cycle} pre-state {label}",
            nonzero=True,
        )
    for key in (
        "usb_task_heartbeat_age_s",
        "usb_last_response_age_s",
    ):
        value = pre.get(key)
        if value is not None and (type(value) is not int or value < 0):
            raise AcceptanceError(
                f"cycle {cycle} pre-state {key} is malformed"
            )
    for slot in ("ble", "wifi"):
        scanner_version = pre.get(f"{slot}_version")
        if not isinstance(scanner_version, str):
            raise AcceptanceError(
                f"cycle {cycle} {slot} pre-state version is malformed"
            )
        relation = flash.firmware_version_relation(
            candidate_version, scanner_version
        )
        if cycle == 1 and relation != "newer":
            raise AcceptanceError(
                f"cycle 1 {slot} pre-state is not strictly older"
            )
        if cycle > 1 and (
            relation != "equal" or scanner_version != candidate_version
        ):
            raise AcceptanceError(
                f"cycle {cycle} {slot} pre-state is not current"
            )


def _require_exact_cycle_probe_transition(
    previous: VerifiedBadgeSnapshot | VerifiedCyclePreSnapshot,
    current: VerifiedCyclePreSnapshot,
    *,
    probe_count: int,
    label: str,
) -> None:
    if type(probe_count) is not int or probe_count <= 0:
        raise AcceptanceError(f"{label} probe count is malformed")
    if previous.get("reboot_generation_capability") != "reported" or \
            current.get("reboot_generation_capability") != "reported" or \
            current.get("reboot_generation") != \
            previous.get("reboot_generation") or \
            current.get("running_partition") != \
            previous.get("running_partition"):
        raise AcceptanceError(
            f"{label} changed reboot generation or partition"
        )
    prior_uptime = _exact_nonnegative_int(
        previous.get("uptime_s"),
        f"{label} prior uptime",
    )
    current_uptime = _exact_nonnegative_int(
        current.get("uptime_s"),
        f"{label} current uptime",
    )
    if current_uptime < prior_uptime:
        raise AcceptanceError(f"{label} uptime moved backward")
    expected_deltas = {
        "usb_rx_bytes": _V078_PRE_MUTATION_RX_DELTA * probe_count,
        "usb_valid_commands":
            _V078_PRE_MUTATION_COMMAND_DELTA * probe_count,
        "usb_responses_completed":
            _V078_PRE_MUTATION_COMMAND_DELTA * probe_count,
    }
    for field, delta in expected_deltas.items():
        prior_value = _exact_uint32(
            previous.get(field),
            f"{label} prior {field}",
            nonzero=True,
        )
        current_value = _exact_uint32(
            current.get(field),
            f"{label} current {field}",
            nonzero=True,
        )
        if current_value != prior_value + delta:
            raise AcceptanceError(
                f"{label} {field} did not advance by exact {delta}"
            )


def _validate_adjacent_cycle_boot_lineage(
    previous: VerifiedCycleCheckpoint,
    current: VerifiedCycleCheckpoint,
) -> None:
    if current["cycle"] != previous["cycle"] + 1:
        raise AcceptanceError(
            "adjacent cycle boot lineage is out of order"
        )
    _require_exact_cycle_probe_transition(
        previous["snapshot"],
        current["pre_snapshot"],
        probe_count=2,
        label=f"cycle {current['cycle']} checkpoint source lineage",
    )


def _validate_cycle_checkpoint(
    session: BadgeAcceptanceSession,
    checkpoint: object,
) -> None:
    if type(checkpoint) is not VerifiedCycleCheckpoint or \
            set(checkpoint) != _CYCLE_CHECKPOINT_KEYS:
        raise AcceptanceError(
            "cycle checkpoint must be verifier-issued with exact schema"
        )
    cycle = checkpoint.get("cycle")
    if type(cycle) is not int or cycle not in (1, 2, 3):
        raise AcceptanceError("cycle checkpoint number is invalid")
    version = checkpoint.get("candidate_version")
    if not isinstance(version, str) or \
            flash.firmware_version_relation(version, version) != "equal":
        raise AcceptanceError("cycle checkpoint version is invalid")
    _validate_candidate_artifacts(
        checkpoint.get("candidate_artifacts"),
        version,
    )
    updater_baseline = checkpoint.get("updater_baseline")
    if version == flash.UPDATE_MAINTENANCE_MIN_VERSION:
        updater_baseline = _validate_v078_baseline_binding(
            updater_baseline,
            session,
        )
    elif updater_baseline is not None:
        raise AcceptanceError(
            "non-canary checkpoint carries an updater baseline"
        )
    pre = checkpoint.get("pre_snapshot")
    _validate_cycle_pre_snapshot(session, pre, version, cycle)
    if cycle == 1:
        if updater_baseline["source_partition"] != \
                pre.get("running_partition"):
            raise AcceptanceError(
                "cycle 1 updater baseline source partition changed"
            )
        if pre["uptime_s"] < updater_baseline["uptime_s"] or \
                pre["usb_rx_bytes"] != (
                    updater_baseline["rx_bytes"]
                    + _V078_PRE_MUTATION_RX_DELTA
                ) or pre["usb_valid_commands"] != (
                    updater_baseline["valid_commands"]
                    + _V078_PRE_MUTATION_COMMAND_DELTA
                ) or pre["usb_responses_completed"] != (
                    updater_baseline["responses_completed"]
                    + _V078_PRE_MUTATION_COMMAND_DELTA
                ):
            raise AcceptanceError(
                "cycle 1 persisted pre-state is not the exact .78 "
                "pre-mutation successor"
            )
    post = checkpoint.get("snapshot")
    _validate_verified_snapshot(
        session, post, expected_version=version
    )
    if post["last_expected_reboot_reason"] != "update_finish":
        raise AcceptanceError(
            f"cycle {cycle} final snapshot lacks update_finish provenance"
        )
    common_exact = {
        "scanner_uploads": 1,
        "slot_mask": 3,
        "scanner_flow_control": "credit-v1",
        "scanner_stage_phase": "final",
        "scanner_stage_credit_bytes": 0,
        "pending_mask_after": 0,
        "fresh_ping_status": True,
        "theme_restored": True,
    }
    for key, wanted in common_exact.items():
        _require_exact(checkpoint, key, wanted)
    generation = checkpoint.get("stage_generation")
    if type(generation) is not int or generation <= 0:
        raise AcceptanceError(
            f"cycle {cycle} stage generation is invalid"
        )
    received = checkpoint.get("scanner_stage_received")
    total = checkpoint.get("scanner_stage_total")
    if type(received) is not int or received <= 0 or \
            type(total) is not int or total != received:
        raise AcceptanceError(
            f"cycle {cycle} scanner credit-v1 final byte proof is invalid"
        )
    scanner_artifact = checkpoint["candidate_artifacts"]["scanner"]
    for checkpoint_key, artifact_key in (
        ("scanner_stage_total", "firmware_size"),
        ("scanner_stage_crc32", "firmware_crc32"),
        ("scanner_stage_sha256", "firmware_sha256"),
    ):
        got = checkpoint.get(checkpoint_key)
        wanted = scanner_artifact[artifact_key]
        if type(got) is not type(wanted) or got != wanted:
            raise AcceptanceError(
                f"cycle {cycle} scanner stage artifact proof is invalid"
            )

    recovery = cycle > 1
    _require_exact(
        checkpoint, "recovery_rewrite_same_version", recovery
    )
    _require_exact(
        checkpoint, "manual_relay_commands", 2 if recovery else 0
    )
    _require_exact(
        checkpoint, "preflight_older_slot_mask", 0 if recovery else 3
    )
    _require_exact(
        checkpoint, "recovery_slot_mask", 3 if recovery else 0
    )
    for slot in ("ble", "wifi"):
        scanner_version = pre.get(f"{slot}_version")
        relation = flash.firmware_version_relation(
            version, scanner_version
            if isinstance(scanner_version, str) else None
        )
        state = checkpoint.get(f"{slot}_state")
        attempts = checkpoint.get(f"{slot}_attempts")
        if not recovery:
            if relation != "newer":
                raise AcceptanceError(
                    f"cycle 1 {slot} pre-state is not strictly older"
                )
            if state != "converged" or \
                    type(attempts) is not int or attempts < 1:
                raise AcceptanceError(
                    f"cycle 1 {slot} did not automatically converge"
                )
        elif scanner_version != version or relation != "equal":
            raise AcceptanceError(
                f"cycle {cycle} {slot} pre-state is not current"
            )
        elif state not in ("current", "converged") or \
                type(attempts) is not int or attempts < 0 or attempts > 3:
            raise AcceptanceError(
                f"cycle {cycle} {slot} recovery result is invalid"
            )


def _validate_three_cycle_aggregate(
    session: BadgeAcceptanceSession,
    facts: object,
) -> None:
    if type(facts) is not VerifiedThreeCycleAggregate or \
            set(facts) != _THREE_CYCLE_AGGREGATE_KEYS:
        raise AcceptanceError(
            "three-update-cycles PASS requires a locked aggregate proof"
        )
    version = facts.get("candidate_version")
    if not isinstance(version, str):
        raise AcceptanceError("cycle aggregate version is malformed")
    _validate_candidate_artifacts(
        facts.get("candidate_artifacts"),
        version,
    )
    _validate_verified_snapshot(
        session, facts.get("snapshot"), expected_version=version
    )
    _require_exact(facts, "cycles_completed", 3)
    _require_bool(facts, "strictly_older_setup", True)
    _require_bool(facts, "automatic_convergence", True)
    _require_exact(facts, "first_cycle_manual_relay_commands", 0)
    _require_exact(facts, "recovery_manual_relay_commands", 4)
    generations = facts.get("checkpoint_generations")
    if not isinstance(generations, list) or len(generations) != 3 or \
            any(type(value) is not int or value <= 0
                for value in generations) or \
            generations != sorted(set(generations)):
        raise AcceptanceError(
            "cycle aggregate generations must strictly advance"
        )


def _validate_pass_facts(
    session: BadgeAcceptanceSession,
    gate: str,
    facts: dict[str, Any],
) -> None:
    if gate == "chord-rom-recovery" and \
            type(facts) is not VerifiedChordRecoveryFacts:
        raise AcceptanceError(
            "chord-rom-recovery PASS requires machine-issued facts"
        )
    snapshot = facts.get("snapshot")
    if gate == "android-control-reconnect":
        candidate_version = (
            snapshot.get("candidate_version")
            if isinstance(snapshot, dict) else None
        )
        if not isinstance(candidate_version, str):
            raise AcceptanceError(
                "Gate 1 pre-update snapshot candidate is malformed"
            )
        _validate_cycle_pre_snapshot(
            session,
            snapshot,
            candidate_version,
            1,
        )
        snapshot_version = candidate_version
    else:
        snapshot_version = _validate_verified_snapshot(session, snapshot)

    allowed_keys: set[str]

    if gate == "android-control-reconnect":
        allowed_keys = {
            "snapshot", "candidate_artifacts", "status_received",
            "badge_detection_received",
            "theme_changed", "theme_restored",
            "reconnected_after_cable_removal", "firmware_actions_absent",
        }
        _validate_candidate_artifacts(
            facts.get("candidate_artifacts"),
            snapshot_version,
        )
        for key in (
            "status_received",
            "badge_detection_received",
            "theme_changed",
            "theme_restored",
            "reconnected_after_cable_removal",
            "firmware_actions_absent",
        ):
            _require_bool(facts, key, True)
    elif gate == "three-update-cycles":
        allowed_keys = set(_THREE_CYCLE_AGGREGATE_KEYS)
        _validate_three_cycle_aggregate(session, facts)
    elif gate == "interrupted-upload":
        allowed_keys = {
            "snapshot", "candidate_artifacts", "baseline_snapshot",
            "recovered_snapshot",
            "abort_after", "idle_wait_s", "prior_partition_bootable",
            "scanner_cache_unchanged", "parser_returned_to_command",
            "retry_succeeded", "scanner_cache_before",
            "scanner_cache_after_abort", "scanner_cache_after_retry",
        }
        _validate_candidate_artifacts(
            facts.get("candidate_artifacts"),
            snapshot_version,
        )
        _require_exact(facts, "abort_after", INTERRUPTED_UPLOAD_BYTES)
        for key in (
            "prior_partition_bootable",
            "scanner_cache_unchanged",
            "parser_returned_to_command",
            "retry_succeeded",
        ):
            _require_bool(facts, key, True)
        cache_before = facts.get("scanner_cache_before")
        cache_after_abort = facts.get("scanner_cache_after_abort")
        cache_after_retry = facts.get("scanner_cache_after_retry")
        for key, fingerprint in (
            ("scanner_cache_before", cache_before),
            ("scanner_cache_after_abort", cache_after_abort),
            ("scanner_cache_after_retry", cache_after_retry),
        ):
            if not isinstance(fingerprint, dict) or set(fingerprint) != {
                "stored", "generation", "sha256",
            }:
                raise AcceptanceError(f"{key} proof is malformed")
            _require_bool(fingerprint, "stored", True)
            generation = fingerprint.get("generation")
            sha256 = fingerprint.get("sha256")
            if type(generation) is not int or generation <= 0 or \
                    not isinstance(sha256, str) or \
                    not _SHA256_RE.fullmatch(sha256):
                raise AcceptanceError(f"{key} proof is malformed")
        if cache_after_abort != cache_before or \
                cache_after_retry != cache_before:
            raise AcceptanceError("scanner cache fingerprints changed")
        for key in ("baseline_snapshot", "recovered_snapshot"):
            _validate_verified_snapshot(
                session,
                facts.get(key),
                expected_version=snapshot_version,
            )
        for key, reason in (
            ("baseline_snapshot", "update_finish"),
            ("recovered_snapshot", "update_abort"),
            ("snapshot", "update_finish"),
        ):
            _require_exact(
                facts[key],
                "last_expected_reboot_reason",
                reason,
            )
        baseline_partition = facts["baseline_snapshot"][
            "running_partition"
        ]
        recovered_partition = facts["recovered_snapshot"][
            "running_partition"
        ]
        final_partition = snapshot["running_partition"]
        if recovered_partition != baseline_partition:
            raise AcceptanceError(
                "interrupted upload did not preserve the prior partition"
            )
        expected_final_partition = (
            "ota_1" if recovered_partition == "ota_0" else "ota_0"
        )
        if final_partition != expected_final_partition:
            raise AcceptanceError(
                "interrupted upload retry did not boot the alternate "
                "OTA partition"
            )
        idle_wait = facts.get("idle_wait_s")
        if type(idle_wait) is not int or \
                idle_wait < INTERRUPTED_UPLOAD_IDLE_WAIT_S:
            raise AcceptanceError("idle_wait_s is too short")
    elif gate == "chord-rom-recovery":
        allowed_keys = set(_CHORD_RECOVERY_FACT_KEYS)
        _validate_candidate_artifacts(
            facts.get("candidate_artifacts"),
            snapshot_version,
        )
        _require_bool(facts, "usb_data_host_attached", True)
        _require_exact(facts, "hold_ms", 10_000)
        for key in (
            "rom_enumerated",
            "base_mac_continuity",
            "full_layout_verified",
            "application_returned",
            "scanner_staged_once",
            "both_uart_updates",
        ):
            _require_bool(facts, key, True)
        _require_exact(
            facts, "last_expected_reboot_reason", "button_usb_rom"
        )
        _validate_chord_rom_boot_snapshot(
            facts.get("rom_boot_snapshot"),
            session,
            snapshot_version,
        )
        _require_exact(
            snapshot, "last_expected_reboot_reason", "update_finish"
        )
    elif gate == "no-host-reboot":
        allowed_keys = {
            "snapshot", "usb_data_host_attached", "hold_ms",
            "normal_reboot", "persistent_rom_wait",
            "power_only_charger_repeated",
            "last_expected_reboot_reason",
        }
        _require_bool(facts, "usb_data_host_attached", False)
        _require_exact(facts, "hold_ms", 10_000)
        _require_bool(facts, "normal_reboot", True)
        _require_bool(facts, "persistent_rom_wait", False)
        _require_bool(facts, "power_only_charger_repeated", True)
        _require_exact(
            facts, "last_expected_reboot_reason", "button_reboot"
        )
        _require_exact(
            snapshot, "last_expected_reboot_reason", "button_reboot"
        )
    elif gate == "power-state-audit":
        allowed_keys = {
            "snapshot", "battery_continuously_connected",
            "power_off_entered", "physical_chord_changed_quiet_mode",
            "persistent_safe_mode_or_reboot_loop",
        }
        _require_bool(facts, "battery_continuously_connected", True)
        _require_bool(facts, "power_off_entered", False)
        _require_bool(facts, "physical_chord_changed_quiet_mode", False)
        _require_bool(
            facts, "persistent_safe_mode_or_reboot_loop", False
        )
    else:
        raise AcceptanceError(f"unknown physical gate: {gate!r}")
    unexpected = sorted(set(facts) - allowed_keys)
    if unexpected:
        raise AcceptanceError(
            "unexpected evidence fields: " + ", ".join(unexpected)
        )


def _require_interrupted_baseline_matches_cycle_three(
    facts: dict[str, Any],
    cycle_three_snapshot: VerifiedBadgeSnapshot,
) -> None:
    baseline = facts.get("baseline_snapshot")
    if not isinstance(baseline, dict):
        raise AcceptanceError(
            "interrupted-upload baseline snapshot is malformed"
        )
    for key in (
        "version",
        "reboot_generation_capability",
        "reboot_generation",
        "uplink_hardware_id",
        "ble_hardware_id",
        "wifi_hardware_id",
        "running_partition",
        "rollback_clear",
        "recovery_mode",
        "usb_parser_state",
        "usb_required_response_failures",
        "ble_role",
        "wifi_role",
        "radio_health",
        "last_expected_reboot_reason",
    ):
        if baseline.get(key) != cycle_three_snapshot.get(key):
            raise AcceptanceError(
                "interrupted-upload baseline changed cycle 3 "
                f"{key} lineage"
            )
    prior_uptime = _exact_nonnegative_int(
        cycle_three_snapshot.get("uptime_s"),
        "cycle 3 final uptime",
    )
    baseline_uptime = _exact_nonnegative_int(
        baseline.get("uptime_s"),
        "interrupted-upload baseline uptime",
    )
    if baseline_uptime < prior_uptime:
        raise AcceptanceError(
            "interrupted-upload baseline uptime moved backward"
        )
    for key, delta in (
        ("usb_rx_bytes", _STATUS_COMMAND_RX_DELTA),
        ("usb_valid_commands", 1),
        ("usb_responses_completed", 1),
    ):
        previous = _exact_uint32(
            cycle_three_snapshot.get(key),
            f"cycle 3 final {key}",
            nonzero=True,
        )
        current = _exact_uint32(
            baseline.get(key),
            f"interrupted-upload baseline {key}",
            nonzero=True,
        )
        if current != previous + delta:
            raise AcceptanceError(
                "interrupted-upload baseline "
                f"{key} did not advance by exact {delta}"
            )


def _validate_private_facts(
    value: Any,
    allowed_hardware_ids: set[str],
    *,
    key_path: str = "facts",
    depth: int = 0,
) -> None:
    if depth > 8:
        raise AcceptanceError("evidence facts are nested too deeply")
    if isinstance(value, dict):
        for key, nested in value.items():
            if not isinstance(key, str) or not key or len(key) > 96:
                raise AcceptanceError(f"{key_path} has an invalid key")
            lowered = key.lower()
            if any(part in lowered for part in _FORBIDDEN_FACT_KEY_PARTS):
                raise AcceptanceError(
                    f"privacy-sensitive evidence key is forbidden: {key}"
                )
            _validate_private_facts(
                nested,
                allowed_hardware_ids,
                key_path=f"{key_path}.{key}",
                depth=depth + 1,
            )
        return
    if isinstance(value, (list, tuple)):
        if len(value) > 128:
            raise AcceptanceError(f"{key_path} has too many values")
        for index, nested in enumerate(value):
            _validate_private_facts(
                nested,
                allowed_hardware_ids,
                key_path=f"{key_path}[{index}]",
                depth=depth + 1,
            )
        return
    if isinstance(value, str):
        if len(value) > 512:
            raise AcceptanceError(f"{key_path} string is too long")
        for matched in _MAC_RE.findall(value):
            canonical = re.sub(r"[:-]", "", matched).lower()
            if canonical not in allowed_hardware_ids:
                raise AcceptanceError(
                    f"{key_path} contains an ambient hardware address"
                )
        return
    if value is None or type(value) in (bool, int, float):
        return
    raise AcceptanceError(f"{key_path} is not JSON-safe")


def _json_object_without_duplicate_keys(
    pairs: list[tuple[str, Any]],
) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, nested in pairs:
        if key in value:
            raise AcceptanceError(
                f"JSON object contains duplicate JSON key {key!r}"
            )
        value[key] = nested
    return value


def _parse_evidence_records(text: str) -> list[dict[str, Any]]:
    if text and not text.endswith("\n"):
        raise AcceptanceError(
            "existing evidence must end with a terminal newline"
        )
    records: list[dict[str, Any]] = []
    lines = text.splitlines()
    for line_number, line in enumerate(lines, start=1):
        if not line:
            raise AcceptanceError(
                f"existing evidence line {line_number} is empty"
            )
        if len(line.encode("utf-8")) + 1 > MAX_EVIDENCE_RECORD_BYTES:
            raise AcceptanceError(
                f"existing evidence line {line_number} exceeds 32 KiB"
            )
        try:
            record = json.loads(
                line,
                object_pairs_hook=_json_object_without_duplicate_keys,
            )
        except json.JSONDecodeError as exc:
            raise AcceptanceError(
                f"existing evidence line {line_number} is invalid JSON"
            ) from exc
        if type(record) is not dict or set(record) != _EVIDENCE_RECORD_KEYS:
            raise AcceptanceError(
                f"existing evidence line {line_number} has invalid "
                "record schema"
            )
        if type(record.get("schema")) is not int or \
                record["schema"] != EVIDENCE_SCHEMA:
            raise AcceptanceError(
                f"existing evidence line {line_number} has invalid schema"
            )
        timestamp = record.get("timestamp_utc")
        if not isinstance(timestamp, str) or \
                not _TIMESTAMP_UTC_RE.fullmatch(timestamp):
            raise AcceptanceError(
                f"existing evidence line {line_number} has invalid timestamp"
            )
        try:
            datetime.strptime(timestamp, "%Y-%m-%dT%H:%M:%SZ")
        except ValueError as exc:
            raise AcceptanceError(
                f"existing evidence line {line_number} has invalid timestamp"
            ) from exc
        gate = record.get("gate")
        status = record.get("status")
        if gate not in REQUIRED_GATES:
            raise AcceptanceError(
                f"existing evidence line {line_number} has invalid gate"
            )
        if status not in ("PASS", "FAIL", "CHECKPOINT"):
            raise AcceptanceError(
                f"existing evidence line {line_number} has invalid status"
            )
        if status == "CHECKPOINT" and gate != "three-update-cycles":
            raise AcceptanceError(
                f"existing evidence line {line_number} has invalid "
                "checkpoint gate"
            )
        if type(record.get("facts")) is not dict:
            raise AcceptanceError(
                f"existing evidence line {line_number} has invalid facts"
            )
        records.append(record)
    return records


def _session_bindings_from_records(
    records: list[dict[str, Any]],
) -> dict[str, BadgeAcceptanceSession]:
    bindings: dict[str, BadgeAcceptanceSession] = {}
    for line_number, record in enumerate(records, start=1):
        try:
            bound = BadgeAcceptanceSession(
                session_id=record["session_id"],
                uplink_hardware_id=record["uplink_hardware_id"],
                ble_hardware_id=record["ble_hardware_id"],
                wifi_hardware_id=record["wifi_hardware_id"],
            )
        except (KeyError, TypeError, ValueError) as exc:
            raise AcceptanceError(
                f"existing evidence line {line_number} has invalid binding"
            ) from exc
        canonical_binding = {
            "uplink_hardware_id": bound.uplink_hardware_id,
            "ble_hardware_id": bound.ble_hardware_id,
            "wifi_hardware_id": bound.wifi_hardware_id,
        }
        if any(
            record.get(key) != wanted
            for key, wanted in canonical_binding.items()
        ):
            raise AcceptanceError(
                f"existing evidence line {line_number} has a "
                "non-canonical hardware binding"
            )
        prior = bindings.get(bound.session_id)
        if prior is not None and prior != bound:
            raise AcceptanceError(
                f"existing session {bound.session_id!r} has conflicting "
                "hardware bindings"
            )
        try:
            _validate_private_facts(
                record["facts"], _allowed_hardware_ids(bound)
            )
        except AcceptanceError as exc:
            raise AcceptanceError(
                f"existing evidence line {line_number} is not privacy-safe: "
                f"{exc}"
            ) from exc
        bindings[bound.session_id] = bound
    return bindings


def _existing_session_bindings(
    evidence_path: Path,
) -> dict[str, BadgeAcceptanceSession]:
    path = Path(evidence_path)
    if not path.exists():
        return {}
    fd = _open_existing_evidence_readonly(path)
    try:
        fcntl.flock(fd, fcntl.LOCK_SH)
        return _session_bindings_from_records(_read_locked_evidence(fd))
    finally:
        try:
            fcntl.flock(fd, fcntl.LOCK_UN)
        except OSError:
            pass
        os.close(fd)


def _read_locked_evidence(fd: int) -> list[dict[str, Any]]:
    size = os.fstat(fd).st_size
    if size > MAX_EVIDENCE_BYTES:
        raise AcceptanceError("existing evidence exceeds the 16 MiB limit")
    os.lseek(fd, 0, os.SEEK_SET)
    remaining = size
    chunks: list[bytes] = []
    while remaining > 0:
        chunk = os.read(fd, min(remaining, 65_536))
        if not chunk:
            raise AcceptanceError("existing evidence ended during locked read")
        chunks.append(chunk)
        remaining -= len(chunk)
    try:
        text = b"".join(chunks).decode("utf-8")
    except UnicodeDecodeError as exc:
        raise AcceptanceError("existing evidence is not UTF-8") from exc
    return _parse_evidence_records(text)


def load_anchored_session(
    evidence_path: Path,
    session_id: str | None = None,
) -> BadgeAcceptanceSession:
    """Load an immutable board binding already present in evidence."""
    bindings = _existing_session_bindings(evidence_path)
    if not bindings:
        raise AcceptanceError(
            "no anchored acceptance session exists in the evidence file"
        )
    if session_id is not None:
        anchored = bindings.get(session_id)
        if anchored is None:
            raise AcceptanceError(
                f"session {session_id!r} is not anchored in evidence"
            )
        return anchored
    if len(bindings) != 1:
        raise AcceptanceError(
            "multiple anchored sessions exist; select one with --session-id"
        )
    return next(iter(bindings.values()))


def _allowed_hardware_ids(
    session: BadgeAcceptanceSession,
) -> set[str]:
    return {
        re.sub(r"[:-]", "", value).lower()
        for value in (
            session.uplink_hardware_id,
            session.ble_hardware_id,
            session.wifi_hardware_id,
        )
    }


def _make_record(
    session: BadgeAcceptanceSession,
    gate: str,
    status: str,
    facts: dict[str, object],
) -> dict[str, object]:
    return {
        "schema": EVIDENCE_SCHEMA,
        "timestamp_utc": datetime.now(timezone.utc).isoformat(
            timespec="seconds"
        ).replace("+00:00", "Z"),
        "session_id": session.session_id,
        "gate": gate,
        "status": status,
        "uplink_hardware_id": session.uplink_hardware_id,
        "ble_hardware_id": session.ble_hardware_id,
        "wifi_hardware_id": session.wifi_hardware_id,
        "facts": facts,
    }


def _encode_record(record: dict[str, object]) -> bytes:
    encoded = (
        json.dumps(record, separators=(",", ":"), sort_keys=True) + "\n"
    ).encode("utf-8")
    if len(encoded) > MAX_EVIDENCE_RECORD_BYTES:
        raise AcceptanceError("one evidence record exceeds 32 KiB")
    return encoded


def _write_all(fd: int, encoded: bytes) -> None:
    view = memoryview(encoded)
    while view:
        written = os.write(fd, view)
        if written <= 0:
            raise AcceptanceError("evidence append made no progress")
        view = view[written:]


def _validate_open_evidence_file(fd: int, path: Path) -> None:
    opened = os.fstat(fd)
    try:
        named = os.stat(path, follow_symlinks=False)
    except OSError as exc:
        raise AcceptanceError(
            f"cannot revalidate acceptance evidence path: {exc}"
        ) from exc
    if not stat.S_ISREG(opened.st_mode) or \
            not os.path.samestat(opened, named):
        raise AcceptanceError(
            "acceptance evidence must be one stable regular file"
        )
    if opened.st_nlink != 1:
        raise AcceptanceError(
            "acceptance evidence must not have hard links"
        )
    if opened.st_uid != os.geteuid() or \
            stat.S_IMODE(opened.st_mode) & 0o077:
        raise AcceptanceError(
            "acceptance evidence must be a private owner-only file"
        )


def _open_evidence_for_append(path: Path) -> int:
    path.parent.mkdir(parents=True, exist_ok=True)
    flags = os.O_RDWR | os.O_APPEND | os.O_CREAT | os.O_NONBLOCK
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    try:
        fd = os.open(path, flags, 0o600)
    except OSError as exc:
        raise AcceptanceError(
            f"cannot open acceptance evidence for append: {exc}"
        ) from exc
    try:
        _validate_open_evidence_file(fd, path)
    except BaseException:
        os.close(fd)
        raise
    return fd


def _mutating_gate_reservation_path(evidence_path: Path) -> Path:
    path = Path(os.path.abspath(os.fspath(evidence_path)))
    return path.with_name(
        f".{path.name}.mutating-gate-reservation"
    )


def _require_no_mutating_gate_reservation(evidence_path: Path) -> None:
    reservation_path = _mutating_gate_reservation_path(evidence_path)
    try:
        os.stat(reservation_path, follow_symlinks=False)
    except FileNotFoundError:
        return
    except OSError as exc:
        raise AcceptanceError(
            f"cannot inspect mutating-gate reservation: {exc}"
        ) from exc
    raise AcceptanceError(
        "a durable mutating-gate reservation already exists; "
        "the acceptance session cannot continue"
    )


def _open_reservation_parent(path: Path) -> int:
    flags = os.O_RDONLY
    if hasattr(os, "O_DIRECTORY"):
        flags |= os.O_DIRECTORY
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    try:
        return os.open(path.parent, flags)
    except OSError as exc:
        raise AcceptanceError(
            f"cannot open mutating-gate reservation directory: {exc}"
        ) from exc


def _validate_open_directory_binding(
    fd: int,
    path: Path,
    expected_device: int,
    expected_inode: int,
) -> None:
    opened = os.fstat(fd)
    try:
        named = os.stat(path, follow_symlinks=False)
    except OSError as exc:
        raise AcceptanceError(
            f"cannot revalidate mutating-gate reservation directory: {exc}"
        ) from exc
    if not stat.S_ISDIR(opened.st_mode) or \
            not os.path.samestat(opened, named) or \
            opened.st_dev != expected_device or \
            opened.st_ino != expected_inode:
        raise AcceptanceError(
            "mutating-gate reservation directory binding changed"
        )


def _directory_open_flags() -> int:
    flags = os.O_RDONLY
    if hasattr(os, "O_DIRECTORY"):
        flags |= os.O_DIRECTORY
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    return flags


def _validate_private_directory_stat(
    value: os.stat_result,
    label: str,
    *,
    require_user_owner: bool,
) -> None:
    if not stat.S_ISDIR(value.st_mode):
        raise AcceptanceError(f"{label} must remain a directory")
    allowed_owners = {os.geteuid()} if require_user_owner else {
        0, os.geteuid()
    }
    if value.st_uid not in allowed_owners or \
            stat.S_IMODE(value.st_mode) & 0o022:
        raise AcceptanceError(
            f"{label} must be owner-controlled and not group/world writable"
        )


def _validate_directory_binding_chain(
    bindings: tuple[_StateDirectoryBinding, ...],
    label: str,
    *,
    final_user_owner: bool,
) -> None:
    if not bindings or bindings[0].name is not None:
        raise AcceptanceError(f"{label} binding chain is malformed")
    for index, binding in enumerate(bindings):
        if type(binding) is not _StateDirectoryBinding:
            raise AcceptanceError(f"{label} binding chain is malformed")
        opened = os.fstat(binding.fd)
        _validate_private_directory_stat(
            opened,
            label,
            require_user_owner=final_user_owner
            and index == len(bindings) - 1,
        )
        if opened.st_dev != binding.device or \
                opened.st_ino != binding.inode:
            raise AcceptanceError(f"{label} directory identity changed")
        if index == 0:
            continue
        parent = bindings[index - 1]
        try:
            named = os.stat(
                binding.name,
                dir_fd=parent.fd,
                follow_symlinks=False,
            )
        except OSError as exc:
            raise AcceptanceError(
                f"cannot revalidate {label} directory component: {exc}"
            ) from exc
        if not os.path.samestat(opened, named):
            raise AcceptanceError(f"{label} directory binding changed")


def _open_secure_absolute_directory_chain(
    path: Path,
    label: str,
) -> tuple[_StateDirectoryBinding, ...]:
    absolute = Path(path)
    if not absolute.is_absolute() or \
            Path(os.path.realpath(os.fspath(absolute))) != absolute:
        raise AcceptanceError(
            f"{label} trusted anchor must be one canonical absolute path"
        )
    fds: list[int] = []
    bindings: list[_StateDirectoryBinding] = []
    try:
        root_fd = os.open(absolute.anchor, _directory_open_flags())
        fds.append(root_fd)
        root_stat = os.fstat(root_fd)
        bindings.append(_StateDirectoryBinding(
            name=None,
            fd=root_fd,
            device=root_stat.st_dev,
            inode=root_stat.st_ino,
        ))
        for component in absolute.parts[1:]:
            try:
                child_fd = os.open(
                    component,
                    _directory_open_flags(),
                    dir_fd=fds[-1],
                )
            except OSError as exc:
                raise AcceptanceError(
                    f"cannot open {label} component: {exc}"
                ) from exc
            fds.append(child_fd)
            child_stat = os.fstat(child_fd)
            bindings.append(_StateDirectoryBinding(
                name=component,
                fd=child_fd,
                device=child_stat.st_dev,
                inode=child_stat.st_ino,
            ))
        result = tuple(bindings)
        _validate_directory_binding_chain(
            result, label, final_user_owner=True
        )
        return result
    except BaseException:
        for fd in reversed(fds):
            os.close(fd)
        raise


def _close_directory_bindings(
    bindings: tuple[_StateDirectoryBinding, ...],
) -> None:
    for binding in reversed(bindings):
        os.close(binding.fd)


def _require_no_user_symlink_components(
    path: Path,
    label: str,
) -> None:
    absolute = Path(os.path.abspath(os.fspath(path)))
    current = Path(absolute.anchor)
    for component in absolute.parts[1:]:
        current /= component
        try:
            component_stat = os.lstat(current)
        except OSError as exc:
            raise AcceptanceError(
                f"cannot inspect {label} path component: {exc}"
            ) from exc
        if stat.S_ISLNK(component_stat.st_mode) and \
                component_stat.st_uid != 0:
            raise AcceptanceError(
                f"{label} must not traverse a user-controlled symlink"
            )


def _validate_private_session_input(
    session_input: _PrivateSessionInput,
) -> None:
    if type(session_input) is not _PrivateSessionInput or \
            not session_input.path.is_absolute():
        raise AcceptanceError("private mutating session input is malformed")
    _validate_directory_binding_chain(
        session_input.parent_bindings,
        "mutating session input",
        final_user_owner=True,
    )
    opened = os.fstat(session_input.fd)
    try:
        named = os.stat(
            session_input.path.name,
            dir_fd=session_input.parent_fd,
            follow_symlinks=False,
        )
    except OSError as exc:
        raise AcceptanceError(
            f"cannot revalidate mutating session file: {exc}"
        ) from exc
    if not stat.S_ISREG(opened.st_mode) or \
            not os.path.samestat(opened, named) or \
            opened.st_dev != session_input.device or \
            opened.st_ino != session_input.inode or \
            opened.st_nlink != 1:
        raise AcceptanceError(
            "mutating session file must remain one stable regular file"
        )
    if opened.st_uid != os.geteuid() or \
            stat.S_IMODE(opened.st_mode) & 0o077:
        raise AcceptanceError(
            "mutating session file must remain private and owner-only"
        )
    if opened.st_size != len(session_input.encoded) or \
            opened.st_size > MAX_SESSION_FILE_BYTES:
        raise AcceptanceError("mutating session file content changed")
    try:
        os.lseek(session_input.fd, 0, os.SEEK_SET)
        remaining = opened.st_size
        chunks: list[bytes] = []
        while remaining:
            chunk = os.read(session_input.fd, remaining)
            if not chunk:
                raise AcceptanceError(
                    "mutating session file ended during validation"
                )
            chunks.append(chunk)
            remaining -= len(chunk)
    except OSError as exc:
        raise AcceptanceError(
            f"cannot read mutating session file: {exc}"
        ) from exc
    if b"".join(chunks) != session_input.encoded:
        raise AcceptanceError("mutating session file content changed")


def _open_private_session_input(
    session_path: Path,
) -> _PrivateSessionInput:
    supplied = Path(os.path.abspath(os.fspath(session_path)))
    _require_no_user_symlink_components(
        supplied.parent, "mutating session input"
    )
    canonical_parent = Path(os.path.realpath(os.fspath(supplied.parent)))
    bindings = _open_secure_absolute_directory_chain(
        canonical_parent, "mutating session input"
    )
    session_fd = -1
    keep_open = False
    flags = os.O_RDONLY | os.O_NONBLOCK
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    try:
        try:
            session_fd = os.open(
                supplied.name,
                flags,
                dir_fd=bindings[-1].fd,
            )
        except OSError as exc:
            raise AcceptanceError(
                f"cannot open private mutating session file: {exc}"
            ) from exc
        session_stat = os.fstat(session_fd)
        if session_stat.st_size > MAX_SESSION_FILE_BYTES:
            raise AcceptanceError(
                "mutating session file exceeds 32 KiB"
            )
        os.lseek(session_fd, 0, os.SEEK_SET)
        remaining = session_stat.st_size
        chunks: list[bytes] = []
        while remaining:
            chunk = os.read(session_fd, remaining)
            if not chunk:
                raise AcceptanceError(
                    "mutating session file ended during read"
                )
            chunks.append(chunk)
            remaining -= len(chunk)
        session_input = _PrivateSessionInput(
            path=canonical_parent / supplied.name,
            fd=session_fd,
            device=session_stat.st_dev,
            inode=session_stat.st_ino,
            encoded=b"".join(chunks),
            parent_bindings=bindings,
        )
        _validate_private_session_input(session_input)
        keep_open = True
        return session_input
    finally:
        if not keep_open:
            if session_fd >= 0:
                os.close(session_fd)
            _close_directory_bindings(bindings)


def _close_private_session_input(
    session_input: _PrivateSessionInput | None,
) -> None:
    if session_input is None:
        return
    os.close(session_input.fd)
    _close_directory_bindings(session_input.parent_bindings)


def _trusted_effective_user_home() -> Path:
    try:
        home = Path(
            pwd.getpwuid(os.geteuid()).pw_dir
        ).resolve(strict=True)
    except (KeyError, OSError, RuntimeError) as exc:
        raise AcceptanceError(
            f"cannot resolve trusted effective-user home: {exc}"
        ) from exc
    if not home.is_absolute():
        raise AcceptanceError(
            "trusted effective-user home must be an absolute path"
        )
    return home


def _operation_registry_anchor_spec() -> tuple[Path, bool]:
    return _trusted_effective_user_home(), True


def _retained_state_root_spec() -> tuple[Path, tuple[str, ...]]:
    home = _trusted_effective_user_home()
    if sys.platform == "darwin":
        components = (
            "Library",
            "Application Support",
            "FoF Badge Flasher",
            "acceptance-state",
        )
    else:
        components = (
            ".local",
            "state",
            "fof-badge-flasher",
            "acceptance",
        )
    return home, components


def _operation_registry_append_flag() -> int:
    flag = getattr(stat, "UF_APPEND", None)
    if sys.platform != "darwin" or type(flag) is not int or flag <= 0:
        raise AcceptanceError(
            "mutating acceptance requires macOS append-only file flags"
        )
    return flag


def _set_operation_registry_file_flags(fd: int, flags: int) -> None:
    if type(fd) is not int or fd < 0 or type(flags) is not int or flags < 0:
        raise AcceptanceError("operation registry flag request is malformed")
    try:
        fchflags = ctypes.CDLL(None, use_errno=True).fchflags
    except (AttributeError, OSError) as exc:
        raise AcceptanceError(
            "macOS fchflags is unavailable for the operation registry"
        ) from exc
    fchflags.argtypes = [ctypes.c_int, ctypes.c_uint]
    fchflags.restype = ctypes.c_int
    ctypes.set_errno(0)
    if fchflags(fd, flags) != 0:
        error_number = ctypes.get_errno()
        raise OSError(
            error_number,
            os.strerror(error_number),
        )


def _durably_sync_operation_registry_file(fd: int) -> None:
    os.fsync(fd)
    full_fsync = getattr(fcntl, "F_FULLFSYNC", None)
    if sys.platform != "darwin" or type(full_fsync) is not int:
        raise AcceptanceError(
            "macOS F_FULLFSYNC is unavailable for the operation registry"
        )
    fcntl.fcntl(fd, full_fsync)


def _operation_registry_header() -> bytes:
    return (
        json.dumps(
            {
                "kind": "REGISTRY",
                "schema": OPERATION_REGISTRY_SCHEMA,
            },
            separators=(",", ":"),
            sort_keys=True,
        ) + "\n"
    ).encode("utf-8")


def _validate_operation_registry_binding(
    registry: _OperationRegistry,
    *,
    require_append_only: bool,
) -> None:
    if type(registry) is not _OperationRegistry:
        raise AcceptanceError("operation registry capability is malformed")
    _validate_directory_binding_chain(
        registry.parent_bindings,
        "operation registry anchor",
        final_user_owner=True,
    )
    opened = os.fstat(registry.fd)
    try:
        named = os.stat(
            registry.name,
            dir_fd=registry.parent_fd,
            follow_symlinks=False,
        )
    except OSError as exc:
        raise AcceptanceError(
            f"cannot revalidate operation registry: {exc}"
        ) from exc
    if not stat.S_ISREG(opened.st_mode) or \
            not os.path.samestat(opened, named) or \
            registry.device is not None and \
            opened.st_dev != registry.device or \
            registry.inode is not None and \
            opened.st_ino != registry.inode or \
            opened.st_nlink != 1:
        raise AcceptanceError(
            "operation registry must remain one stable regular file"
        )
    if opened.st_uid != os.geteuid() or \
            stat.S_IMODE(opened.st_mode) & 0o077:
        raise AcceptanceError(
            "operation registry must remain private and owner-only"
        )
    if require_append_only and \
            not opened.st_flags & _operation_registry_append_flag():
        raise AcceptanceError(
            "operation registry lost its kernel append-only flag"
        )


def _read_operation_registry_bytes(
    registry: _OperationRegistry,
) -> bytes:
    _validate_operation_registry_binding(
        registry, require_append_only=True
    )
    size = os.fstat(registry.fd).st_size
    if size > MAX_OPERATION_REGISTRY_BYTES:
        raise AcceptanceError(
            "operation registry exceeds the 16 MiB limit"
        )
    os.lseek(registry.fd, 0, os.SEEK_SET)
    remaining = size
    chunks: list[bytes] = []
    while remaining:
        chunk = os.read(registry.fd, min(remaining, 65_536))
        if not chunk:
            raise AcceptanceError(
                "operation registry ended during locked read"
            )
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def _operation_registry_json_object(
    pairs: list[tuple[str, Any]],
) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, nested in pairs:
        if key in value:
            raise AcceptanceError(
                f"operation registry contains duplicate key {key!r}"
            )
        value[key] = nested
    return value


def _parse_operation_registry(
    encoded: bytes,
) -> tuple[
    list[dict[str, object]],
    dict[str, tuple[str, str]],
    set[tuple[str, str]],
    bool,
    set[tuple[str, str]],
]:
    try:
        text = encoded.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise AcceptanceError(
            "operation registry is not UTF-8"
        ) from exc
    if not text.endswith("\n"):
        raise AcceptanceError(
            "operation registry lacks a terminal newline"
        )
    lines = text.splitlines()
    if not lines:
        raise AcceptanceError("operation registry is empty")
    parsed: list[dict[str, object]] = []
    for line_number, line in enumerate(lines, start=1):
        if not line or len(line.encode("utf-8")) > 1024:
            raise AcceptanceError(
                f"operation registry line {line_number} is malformed"
            )
        try:
            value = json.loads(
                line,
                object_pairs_hook=_operation_registry_json_object,
            )
        except json.JSONDecodeError as exc:
            raise AcceptanceError(
                f"operation registry line {line_number} is invalid JSON"
            ) from exc
        if type(value) is not dict:
            raise AcceptanceError(
                f"operation registry line {line_number} is not an object"
            )
        parsed.append(value)

    if parsed[0] != {
        "kind": "REGISTRY",
        "schema": OPERATION_REGISTRY_SCHEMA,
    }:
        raise AcceptanceError("operation registry header is invalid")

    states: dict[str, tuple[str, str]] = {}
    reservation_armed: set[tuple[str, str]] = set()
    protocol_fence_seen = False
    post_fence_prepared: set[tuple[str, str]] = set()
    for line_number, record in enumerate(parsed[1:], start=2):
        transition_keys = {
            "attempt_id",
            "operation_sha256",
            "schema",
            "state",
        }
        event_keys = {
            "attempt_id",
            "event",
            "operation_sha256",
            "schema",
        }
        fence_keys = {
            "event",
            "schema",
        }
        if set(record) not in (
            transition_keys,
            event_keys,
            fence_keys,
        ) or \
                record.get("schema") != OPERATION_REGISTRY_SCHEMA or \
                type(record.get("schema")) is not int:
            raise AcceptanceError(
                f"operation registry line {line_number} has invalid schema"
            )
        if set(record) == fence_keys:
            if record != {
                "event": "ARMING_PROTOCOL_FENCE",
                "schema": OPERATION_REGISTRY_SCHEMA,
            } or protocol_fence_seen or any(
                state_value[0] == "PREPARED"
                for state_value in states.values()
            ):
                raise AcceptanceError(
                    "operation registry has an invalid protocol fence"
                )
            protocol_fence_seen = True
            continue

        operation_sha256 = record.get("operation_sha256")
        attempt_id = record.get("attempt_id")
        if not isinstance(operation_sha256, str) or \
                not _SHA256_RE.fullmatch(operation_sha256) or \
                not isinstance(attempt_id, str) or \
                not re.fullmatch(r"[0-9a-f]{32}", attempt_id):
            raise AcceptanceError(
                f"operation registry line {line_number} is malformed"
            )
        if set(record) == event_keys:
            if record.get("event") != "RESERVATION_ARMED" or \
                    states.get(operation_sha256) != (
                        "PREPARED", attempt_id
                    ) or \
                    (operation_sha256, attempt_id) in reservation_armed:
                raise AcceptanceError(
                    "operation registry has an invalid reservation event"
                )
            reservation_armed.add((operation_sha256, attempt_id))
            continue

        state_value = record.get("state")
        if state_value not in ("PREPARED", "CANCELLED", "STARTED"):
            raise AcceptanceError(
                f"operation registry line {line_number} is malformed"
            )
        previous = states.get(operation_sha256)
        if state_value == "PREPARED":
            if previous is not None and previous[0] != "CANCELLED":
                raise AcceptanceError(
                    "operation registry has an invalid PREPARED transition"
                )
            if protocol_fence_seen and \
                    previous is not None and \
                    previous[0] == "CANCELLED" and \
                    (operation_sha256, previous[1]) not in \
                    post_fence_prepared:
                raise AcceptanceError(
                    "operation registry restarts a legacy ambiguous "
                    "operation after the protocol fence"
                )
            if protocol_fence_seen:
                post_fence_prepared.add(
                    (operation_sha256, attempt_id)
                )
        elif previous != ("PREPARED", attempt_id):
            raise AcceptanceError(
                f"operation registry has an invalid {state_value} "
                "transition"
            )
        states[operation_sha256] = (state_value, attempt_id)
    return (
        parsed,
        states,
        reservation_armed,
        protocol_fence_seen,
        post_fence_prepared,
    )


def _operation_registry_snapshot(
    registry: _OperationRegistry,
) -> tuple[
    list[dict[str, object]],
    dict[str, tuple[str, str]],
    set[tuple[str, str]],
    bool,
    set[tuple[str, str]],
]:
    return _parse_operation_registry(
        _read_operation_registry_bytes(registry)
    )


def _write_one_operation_registry_record(
    fd: int,
    encoded: bytes,
) -> None:
    written = os.write(fd, encoded)
    if written != len(encoded):
        raise AcceptanceError(
            "operation registry append was not one complete write"
        )


def _sync_operation_registry_transition(
    fd: int,
    state_value: str,
) -> None:
    del state_value
    _durably_sync_operation_registry_file(fd)


def _write_operation_registry_transition(
    fd: int,
    encoded: bytes,
    state_value: str,
) -> None:
    del state_value
    _write_one_operation_registry_record(fd, encoded)


def _ensure_operation_registry_protocol_fence(
    registry: _OperationRegistry,
) -> None:
    (
        _records,
        states,
        _reservation_armed,
        protocol_fence_seen,
        _post_fence_prepared,
    ) = _operation_registry_snapshot(registry)
    if protocol_fence_seen:
        return
    if any(
        state_value[0] == "PREPARED"
        for state_value in states.values()
    ):
        raise AcceptanceError(
            "legacy operation registry has an ambiguous PREPARED record; "
            "manual state repair is required"
        )

    record = {
        "event": "ARMING_PROTOCOL_FENCE",
        "schema": OPERATION_REGISTRY_SCHEMA,
    }
    encoded = (
        json.dumps(record, separators=(",", ":"), sort_keys=True) + "\n"
    ).encode("utf-8")
    _validate_operation_registry_binding(
        registry, require_append_only=True
    )
    os.lseek(registry.fd, 0, os.SEEK_END)
    _write_operation_registry_transition(
        registry.fd, encoded, "ARMING_PROTOCOL_FENCE"
    )
    _sync_operation_registry_transition(
        registry.fd, "ARMING_PROTOCOL_FENCE"
    )
    (
        records,
        _states,
        _reservation_armed,
        protocol_fence_seen,
        _post_fence_prepared,
    ) = _operation_registry_snapshot(registry)
    if records[-1] != record or not protocol_fence_seen:
        raise AcceptanceError(
            "operation registry protocol fence was not durably observed"
        )


def _append_operation_registry_transition(
    registry: _OperationRegistry,
    operation_sha256: str,
    attempt_id: str,
    state_value: str,
) -> None:
    record = {
        "attempt_id": attempt_id,
        "operation_sha256": operation_sha256,
        "schema": OPERATION_REGISTRY_SCHEMA,
        "state": state_value,
    }
    encoded = (
        json.dumps(record, separators=(",", ":"), sort_keys=True) + "\n"
    ).encode("utf-8")
    _validate_operation_registry_binding(
        registry, require_append_only=True
    )
    os.lseek(registry.fd, 0, os.SEEK_END)
    _write_operation_registry_transition(
        registry.fd, encoded, state_value
    )
    _sync_operation_registry_transition(
        registry.fd, state_value
    )
    (
        records,
        states,
        _reservation_armed,
        _protocol_fence_seen,
        _post_fence_prepared,
    ) = (
        _operation_registry_snapshot(registry)
    )
    if records[-1] != record or \
            states.get(operation_sha256) != (state_value, attempt_id):
        raise AcceptanceError(
            "operation registry transition was not durably observed"
        )


def _arm_operation_registry_reservation(
    registry: _OperationRegistry,
    operation_sha256: str,
    attempt_id: str,
) -> None:
    (
        _records,
        states,
        reservation_armed,
        protocol_fence_seen,
        post_fence_prepared,
    ) = (
        _operation_registry_snapshot(registry)
    )
    claim = (operation_sha256, attempt_id)
    if states.get(operation_sha256) != ("PREPARED", attempt_id) or \
            claim in reservation_armed or \
            not protocol_fence_seen or \
            claim not in post_fence_prepared:
        raise AcceptanceError(
            "operation registry claim cannot arm a reservation"
        )
    record = {
        "attempt_id": attempt_id,
        "event": "RESERVATION_ARMED",
        "operation_sha256": operation_sha256,
        "schema": OPERATION_REGISTRY_SCHEMA,
    }
    encoded = (
        json.dumps(record, separators=(",", ":"), sort_keys=True) + "\n"
    ).encode("utf-8")
    _validate_operation_registry_binding(
        registry, require_append_only=True
    )
    os.lseek(registry.fd, 0, os.SEEK_END)
    _write_operation_registry_transition(
        registry.fd, encoded, "RESERVATION_ARMED"
    )
    _sync_operation_registry_transition(
        registry.fd, "RESERVATION_ARMED"
    )
    (
        records,
        states,
        reservation_armed,
        protocol_fence_seen,
        post_fence_prepared,
    ) = (
        _operation_registry_snapshot(registry)
    )
    if records[-1] != record or \
            states.get(operation_sha256) != ("PREPARED", attempt_id) or \
            claim not in reservation_armed or \
            not protocol_fence_seen or \
            claim not in post_fence_prepared:
        raise AcceptanceError(
            "operation registry reservation event was not durably observed"
        )


def _prepare_operation_registry_claim(
    registry: _OperationRegistry,
    operation_sha256: str,
    state_root: _RetainedStateRoot,
) -> str:
    (
        _records,
        states,
        reservation_armed,
        protocol_fence_seen,
        post_fence_prepared,
    ) = (
        _operation_registry_snapshot(registry)
    )
    if not protocol_fence_seen:
        raise AcceptanceError(
            "operation registry lacks the arming-protocol fence; "
            "manual state repair is required"
        )
    previous = states.get(operation_sha256)
    if previous is not None and previous[0] == "STARTED":
        raise AcceptanceError(
            "this mutating gate operation has a durable STARTED record; "
            "hardware will not run again"
        )
    if previous is not None and previous[0] == "CANCELLED" and \
            (operation_sha256, previous[1]) not in \
            post_fence_prepared:
        raise AcceptanceError(
            "this mutating gate operation has a legacy ambiguous "
            "CANCELLED record; manual state repair is required"
        )
    if previous is not None and previous[0] == "PREPARED":
        previous_claim = (operation_sha256, previous[1])
        if previous_claim not in post_fence_prepared:
            raise AcceptanceError(
                "this mutating gate operation has a legacy ambiguous "
                "PREPARED record; manual state repair is required"
            )
        if previous_claim in reservation_armed:
            raise AcceptanceError(
                "this mutating gate operation has an unresolved durable "
                "pre-start reservation; manual state repair is required"
            )
        _cleanup_stale_prepared_operation_marker(
            state_root, operation_sha256
        )
        _append_operation_registry_transition(
            registry,
            operation_sha256,
            previous[1],
            "CANCELLED",
        )
    attempt_id = secrets.token_hex(16)
    _append_operation_registry_transition(
        registry,
        operation_sha256,
        attempt_id,
        "PREPARED",
    )
    return attempt_id


def _cancel_operation_registry_claim(
    registry: _OperationRegistry,
    operation_sha256: str,
    attempt_id: str,
) -> None:
    (
        _records,
        states,
        _reservation_armed,
        protocol_fence_seen,
        post_fence_prepared,
    ) = (
        _operation_registry_snapshot(registry)
    )
    current = states.get(operation_sha256)
    if current == ("CANCELLED", attempt_id):
        return
    if current != ("PREPARED", attempt_id):
        raise AcceptanceError(
            "operation registry claim cannot be cancelled"
        )
    if not protocol_fence_seen or \
            (operation_sha256, attempt_id) not in post_fence_prepared:
        raise AcceptanceError(
            "legacy operation registry claims cannot be cancelled"
        )
    _append_operation_registry_transition(
        registry,
        operation_sha256,
        attempt_id,
        "CANCELLED",
    )


def _start_operation_registry_claim(
    registry: _OperationRegistry,
    operation_sha256: str,
    attempt_id: str,
) -> None:
    (
        _records,
        states,
        reservation_armed,
        protocol_fence_seen,
        post_fence_prepared,
    ) = (
        _operation_registry_snapshot(registry)
    )
    claim = (operation_sha256, attempt_id)
    if states.get(operation_sha256) != ("PREPARED", attempt_id) or \
            claim not in reservation_armed or \
            not protocol_fence_seen or \
            claim not in post_fence_prepared:
        raise AcceptanceError(
            "operation registry claim is not armed for STARTED"
        )
    _append_operation_registry_transition(
        registry,
        operation_sha256,
        attempt_id,
        "STARTED",
    )


def _operation_registry_claim_started(
    registry: _OperationRegistry,
    operation_sha256: str,
    attempt_id: str,
) -> bool:
    (
        _records,
        states,
        _reservation_armed,
        _protocol_fence_seen,
        _post_fence_prepared,
    ) = (
        _operation_registry_snapshot(registry)
    )
    return states.get(operation_sha256) == ("STARTED", attempt_id)


def _cleanup_created_operation_registry(
    registry: _OperationRegistry,
) -> None:
    try:
        current_flags = os.fstat(registry.fd).st_flags
        _set_operation_registry_file_flags(
            registry.fd,
            current_flags & ~_operation_registry_append_flag(),
        )
    except OSError:
        pass
    _validate_operation_registry_binding(
        registry, require_append_only=False
    )
    os.unlink(registry.name, dir_fd=registry.parent_fd)
    if os.fstat(registry.fd).st_nlink != 0 or \
            _canonical_blocker_exists(
                registry.parent_fd, registry.name
            ):
        raise AcceptanceError(
            "new operation registry identity changed during cleanup"
        )
    os.fsync(registry.parent_fd)


def _validate_system_pinned_registry_anchor(
    bindings: tuple[_StateDirectoryBinding, ...],
) -> None:
    if len(bindings) < 2:
        raise AcceptanceError(
            "operation registry anchor has no protected parent"
        )
    parent = os.fstat(bindings[-2].fd)
    anchor = os.fstat(bindings[-1].fd)
    if parent.st_uid != 0 or stat.S_IMODE(parent.st_mode) & 0o022:
        raise AcceptanceError(
            "operation registry home parent must be root-owned and not "
            "group/world writable"
        )
    if anchor.st_uid != os.geteuid():
        raise AcceptanceError(
            "operation registry home must be owned by the effective user"
        )


def _open_operation_registry() -> _OperationRegistry:
    anchor_spec = _operation_registry_anchor_spec()
    if type(anchor_spec) is not tuple or len(anchor_spec) != 2 or \
            not isinstance(anchor_spec[0], Path) or \
            type(anchor_spec[1]) is not bool:
        raise AcceptanceError(
            "operation registry anchor specification is malformed"
        )
    anchor_path, require_system_pinned_parent = anchor_spec
    bindings = _open_secure_absolute_directory_chain(
        anchor_path,
        "operation registry anchor",
    )
    parent_fd = bindings[-1].fd
    registry_fd = -1
    registry: _OperationRegistry | None = None
    created = False
    keep_open = False
    flags = os.O_RDWR | os.O_APPEND
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    try:
        if require_system_pinned_parent:
            _validate_system_pinned_registry_anchor(bindings)
        fcntl.flock(parent_fd, fcntl.LOCK_EX)
        try:
            registry_fd = os.open(
                OPERATION_REGISTRY_NAME,
                flags,
                dir_fd=parent_fd,
            )
        except FileNotFoundError:
            registry_fd = os.open(
                OPERATION_REGISTRY_NAME,
                flags | os.O_CREAT | os.O_EXCL,
                0o600,
                dir_fd=parent_fd,
            )
            created = True

        registry = _OperationRegistry(
            name=OPERATION_REGISTRY_NAME,
            fd=registry_fd,
            device=None,
            inode=None,
            parent_bindings=bindings,
        )
        opened = os.fstat(registry_fd)
        registry = _OperationRegistry(
            name=OPERATION_REGISTRY_NAME,
            fd=registry_fd,
            device=opened.st_dev,
            inode=opened.st_ino,
            parent_bindings=bindings,
        )
        _validate_operation_registry_binding(
            registry, require_append_only=False
        )
        if not opened.st_flags & _operation_registry_append_flag():
            existing = b""
            if opened.st_size:
                os.lseek(registry_fd, 0, os.SEEK_SET)
                existing = os.read(
                    registry_fd, MAX_OPERATION_REGISTRY_BYTES + 1
                )
            header = _operation_registry_header()
            if existing not in (b"", header):
                raise AcceptanceError(
                    "unsealed operation registry is not recoverable"
                )
            if existing == b"":
                os.lseek(registry_fd, 0, os.SEEK_END)
                _write_one_operation_registry_record(
                    registry_fd, header
                )
                _durably_sync_operation_registry_file(registry_fd)
            _set_operation_registry_file_flags(
                registry_fd,
                opened.st_flags | _operation_registry_append_flag(),
            )
            _durably_sync_operation_registry_file(registry_fd)
            os.fsync(parent_fd)
        _validate_operation_registry_binding(
            registry, require_append_only=True
        )
        fcntl.flock(registry_fd, fcntl.LOCK_EX)
        _ensure_operation_registry_protocol_fence(registry)
        keep_open = True
        return registry
    except BaseException as exc:
        if created and registry is not None:
            try:
                _cleanup_created_operation_registry(registry)
            except BaseException as cleanup_exc:
                raise AcceptanceError(
                    "operation registry initialization failed and cleanup "
                    "failed; manual state repair is required"
                ) from cleanup_exc
        raise
    finally:
        if not keep_open:
            if registry_fd >= 0:
                os.close(registry_fd)
            try:
                fcntl.flock(parent_fd, fcntl.LOCK_UN)
            except OSError:
                pass
            _close_directory_bindings(bindings)


def _close_operation_registry(
    registry: _OperationRegistry | None,
) -> None:
    if registry is None:
        return
    try:
        fcntl.flock(registry.fd, fcntl.LOCK_UN)
    except OSError:
        pass
    os.close(registry.fd)
    try:
        fcntl.flock(registry.parent_fd, fcntl.LOCK_UN)
    except OSError:
        pass
    _close_directory_bindings(registry.parent_bindings)


def _validate_state_component_name(value: str) -> None:
    if not isinstance(value, str) or value in ("", ".", "..") or \
            os.sep in value or (os.altsep is not None and os.altsep in value):
        raise AcceptanceError("retained state root component is malformed")


def _open_retained_state_root() -> _RetainedStateRoot:
    anchor_path, components = _retained_state_root_spec()
    if not isinstance(components, tuple) or not components:
        raise AcceptanceError("retained state root specification is malformed")
    bindings = list(_open_secure_absolute_directory_chain(
        Path(anchor_path), "retained state root"
    ))
    keep_open = False
    try:
        for component in components:
            _validate_state_component_name(component)
            parent_fd = bindings[-1].fd
            created = False
            try:
                os.mkdir(component, 0o700, dir_fd=parent_fd)
                created = True
            except FileExistsError:
                pass
            except OSError as exc:
                raise AcceptanceError(
                    f"cannot create retained state root component: {exc}"
                ) from exc
            try:
                child_fd = os.open(
                    component,
                    _directory_open_flags(),
                    dir_fd=parent_fd,
                )
            except OSError as exc:
                raise AcceptanceError(
                    f"cannot open retained state root component: {exc}"
                ) from exc
            child_stat = os.fstat(child_fd)
            binding = _StateDirectoryBinding(
                name=component,
                fd=child_fd,
                device=child_stat.st_dev,
                inode=child_stat.st_ino,
            )
            bindings.append(binding)
            _validate_directory_binding_chain(
                tuple(bindings),
                "retained state root",
                final_user_owner=True,
            )
            if created:
                os.fsync(child_fd)
                os.fsync(parent_fd)
        state_root = _RetainedStateRoot(tuple(bindings))
        _validate_retained_state_root(state_root)
        keep_open = True
        return state_root
    finally:
        if not keep_open:
            _close_directory_bindings(tuple(bindings))


def _validate_retained_state_root(
    state_root: _RetainedStateRoot,
) -> None:
    if type(state_root) is not _RetainedStateRoot:
        raise AcceptanceError("retained state root capability is malformed")
    _validate_directory_binding_chain(
        state_root.bindings,
        "retained state root",
        final_user_owner=True,
    )


def _close_retained_state_root(
    state_root: _RetainedStateRoot | None,
) -> None:
    if state_root is not None:
        _close_directory_bindings(state_root.bindings)


def _operation_identity_sha256(
    session: BadgeAcceptanceSession,
    firmware_version: str,
    candidate_artifacts: VerifiedCandidateArtifacts,
    gate: str,
    phase: str,
    cycle: int | None,
) -> str:
    platform = flash.PLATFORMS[CANARY_PLATFORM_KEY]
    operation = {
        "artifacts": {
            "platform": CANARY_PLATFORM_KEY,
            "scanner_target": platform["scanner_name"],
            "uplink_target": platform["uplink_name"],
            "candidate": candidate_artifacts,
        },
        "cycle": cycle,
        "firmware_version": firmware_version,
        "gate": gate,
        "phase": phase,
        "schema": RETAINED_OPERATION_MARKER_SCHEMA,
        "session": {
            "ble_hardware_id": session.ble_hardware_id,
            "session_id": session.session_id,
            "uplink_hardware_id": session.uplink_hardware_id,
            "wifi_hardware_id": session.wifi_hardware_id,
        },
    }
    canonical = json.dumps(
        operation, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")
    return hashlib.sha256(
        b"friend-or-foe/badge-acceptance/"
        b"retained-operation/v2\x00" + canonical
    ).hexdigest()


def _validate_retained_operation_marker_identity(
    marker: _RetainedOperationMarker,
    *,
    require_canonical_state_root: bool,
    require_content: bool = True,
) -> None:
    if type(marker) is not _RetainedOperationMarker:
        raise AcceptanceError("retained operation marker is malformed")
    if require_canonical_state_root:
        _validate_retained_state_root(marker.state_root)
    else:
        state_stat = os.fstat(marker.state_root.fd)
        _validate_private_directory_stat(
            state_stat,
            "retained state root",
            require_user_owner=True,
        )
    opened = os.fstat(marker.fd)
    try:
        named = os.stat(
            marker.name,
            dir_fd=marker.state_root.fd,
            follow_symlinks=False,
        )
    except OSError as exc:
        raise AcceptanceError(
            f"cannot revalidate retained operation marker: {exc}"
        ) from exc
    if not stat.S_ISREG(opened.st_mode) or \
            not os.path.samestat(opened, named) or \
            marker.device is not None and \
            opened.st_dev != marker.device or \
            marker.inode is not None and \
            opened.st_ino != marker.inode or \
            opened.st_nlink != 1:
        raise AcceptanceError(
            "retained operation marker must remain one stable regular file"
        )
    if opened.st_uid != os.geteuid() or \
            stat.S_IMODE(opened.st_mode) & 0o077:
        raise AcceptanceError(
            "retained operation marker must remain private and owner-only"
        )
    if not require_content:
        return
    os.lseek(marker.fd, 0, os.SEEK_SET)
    remaining = len(marker.encoded) + 1
    chunks: list[bytes] = []
    while remaining:
        chunk = os.read(marker.fd, remaining)
        if not chunk:
            break
        chunks.append(chunk)
        remaining -= len(chunk)
    if b"".join(chunks) != marker.encoded or \
            opened.st_size != len(marker.encoded):
        raise AcceptanceError("retained operation marker content changed")


def _write_retained_marker_bytes(fd: int, encoded: bytes) -> None:
    _write_all(fd, encoded)


def _fstat_new_retained_operation_marker(fd: int) -> os.stat_result:
    return os.fstat(fd)


def _fsync_retained_marker_file(fd: int) -> None:
    os.fsync(fd)


def _fsync_retained_state_root(fd: int) -> None:
    os.fsync(fd)


def _post_create_validate_retained_operation_marker(
    marker: _RetainedOperationMarker,
) -> None:
    _validate_retained_operation_marker_identity(
        marker, require_canonical_state_root=True
    )


def _cleanup_pre_action_retained_operation_marker(
    marker: _RetainedOperationMarker,
) -> None:
    _validate_retained_operation_marker_identity(
        marker,
        require_canonical_state_root=False,
        require_content=False,
    )
    os.unlink(marker.name, dir_fd=marker.state_root.fd)
    opened = os.fstat(marker.fd)
    if opened.st_nlink != 0 or _canonical_blocker_exists(
        marker.state_root.fd, marker.name
    ):
        raise AcceptanceError(
            "pre-action retained operation marker identity changed"
        )
    os.fsync(marker.state_root.fd)


def _retained_operation_marker_material(
    operation_sha256: str,
) -> tuple[str, bytes]:
    if not _SHA256_RE.fullmatch(operation_sha256):
        raise AcceptanceError("retained operation digest is malformed")
    name = f"op-{operation_sha256}.retained"
    encoded = (
        json.dumps(
            {
                "operation_sha256": operation_sha256,
                "schema": RETAINED_OPERATION_MARKER_SCHEMA,
            },
            separators=(",", ":"),
            sort_keys=True,
        ) + "\n"
    ).encode("utf-8")
    return name, encoded


def _cleanup_stale_prepared_operation_marker(
    state_root: _RetainedStateRoot,
    operation_sha256: str,
) -> None:
    _validate_retained_state_root(state_root)
    name, encoded = _retained_operation_marker_material(
        operation_sha256
    )
    flags = os.O_RDWR
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    try:
        marker_fd = os.open(name, flags, dir_fd=state_root.fd)
    except FileNotFoundError:
        return
    except OSError as exc:
        raise AcceptanceError(
            f"cannot open stale PREPARED operation marker: {exc}"
        ) from exc
    try:
        marker_stat = os.fstat(marker_fd)
        marker = _RetainedOperationMarker(
            name=name,
            fd=marker_fd,
            device=marker_stat.st_dev,
            inode=marker_stat.st_ino,
            encoded=encoded,
            state_root=state_root,
        )
        _validate_retained_operation_marker_identity(
            marker, require_canonical_state_root=True
        )
        _cleanup_pre_action_retained_operation_marker(marker)
    finally:
        os.close(marker_fd)


def _create_retained_operation_marker(
    state_root: _RetainedStateRoot,
    operation_sha256: str,
) -> _RetainedOperationMarker:
    _validate_retained_state_root(state_root)
    name, encoded = _retained_operation_marker_material(
        operation_sha256
    )
    flags = os.O_RDWR | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    marker_fd = -1
    marker: _RetainedOperationMarker | None = None
    created = False
    keep_open = False
    try:
        try:
            marker_fd = os.open(
                name,
                flags,
                0o600,
                dir_fd=state_root.fd,
            )
            created = True
        except FileExistsError as exc:
            raise AcceptanceError(
                "this mutating gate operation already has a retained "
                "run-once marker; hardware will not run again"
            ) from exc
        except OSError as exc:
            raise AcceptanceError(
                f"cannot create retained operation marker: {exc}"
            ) from exc
        marker = _RetainedOperationMarker(
            name=name,
            fd=marker_fd,
            device=None,
            inode=None,
            encoded=encoded,
            state_root=state_root,
        )
        marker_stat = _fstat_new_retained_operation_marker(marker_fd)
        marker = _RetainedOperationMarker(
            name=name,
            fd=marker_fd,
            device=marker_stat.st_dev,
            inode=marker_stat.st_ino,
            encoded=encoded,
            state_root=state_root,
        )
        _write_retained_marker_bytes(marker_fd, encoded)
        _fsync_retained_marker_file(marker_fd)
        _post_create_validate_retained_operation_marker(marker)
        _fsync_retained_state_root(state_root.fd)
        _post_create_validate_retained_operation_marker(marker)
        keep_open = True
        return marker
    except BaseException as exc:
        if created and marker is not None:
            try:
                _cleanup_pre_action_retained_operation_marker(marker)
            except BaseException as cleanup_exc:
                raise AcceptanceError(
                    "retained operation marker creation failed before "
                    "hardware and cleanup failed; manual state repair is "
                    "required"
                ) from cleanup_exc
        raise
    finally:
        if marker_fd >= 0 and not keep_open:
            os.close(marker_fd)


def _validate_retained_state_root_before_action(
    state_root: _RetainedStateRoot,
    marker: _RetainedOperationMarker,
) -> None:
    _validate_retained_state_root(state_root)
    _validate_retained_operation_marker_identity(
        marker, require_canonical_state_root=True
    )


def _close_retained_operation_marker(
    marker: _RetainedOperationMarker | None,
) -> None:
    if marker is not None:
        os.close(marker.fd)


def _validate_open_reservation_parent(
    reservation: _DurableGateReservation,
) -> None:
    if not reservation.parent_path.is_absolute():
        raise AcceptanceError(
            "mutating-gate reservation directory binding is malformed"
        )
    _validate_open_directory_binding(
        reservation.parent_fd,
        reservation.parent_path,
        reservation.parent_device,
        reservation.parent_inode,
    )


def _create_mutating_gate_reservation(
    evidence_path: Path,
    evidence_fd: int,
    session: BadgeAcceptanceSession,
    gate: str,
    phase: str,
    *,
    cycle: int | None,
) -> _DurableGateReservation:
    path = Path(evidence_path)
    _require_no_mutating_gate_reservation(path)
    _validate_open_evidence_file(evidence_fd, path)
    evidence_stat = os.fstat(evidence_fd)
    bound_evidence_path = Path(os.path.abspath(os.fspath(path)))
    reservation_path = _mutating_gate_reservation_path(path)
    encoded = (
        json.dumps(
            {
                "cycle": cycle,
                "evidence_device": evidence_stat.st_dev,
                "evidence_inode": evidence_stat.st_ino,
                "evidence_path": str(bound_evidence_path),
                "gate": gate,
                "phase": phase,
                "schema": MUTATING_GATE_RESERVATION_SCHEMA,
                "session_id": session.session_id,
            },
            separators=(",", ":"),
            sort_keys=True,
        ) + "\n"
    ).encode("utf-8")
    parent_fd = _open_reservation_parent(reservation_path)
    parent_stat = os.fstat(parent_fd)
    parent_path = reservation_path.parent
    _validate_open_directory_binding(
        parent_fd,
        parent_path,
        parent_stat.st_dev,
        parent_stat.st_ino,
    )
    marker_fd = -1
    keep_open = False
    flags = os.O_RDWR | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    try:
        marker_fd = os.open(
            reservation_path.name,
            flags,
            0o600,
            dir_fd=parent_fd,
        )
        _write_all(marker_fd, encoded)
        os.fsync(marker_fd)
        reservation = _DurableGateReservation(
            path=reservation_path,
            fd=marker_fd,
            parent_fd=parent_fd,
            parent_path=parent_path,
            parent_device=parent_stat.st_dev,
            parent_inode=parent_stat.st_ino,
            encoded=encoded,
            evidence_path=bound_evidence_path,
            evidence_fd=evidence_fd,
            evidence_device=evidence_stat.st_dev,
            evidence_inode=evidence_stat.st_ino,
            session_id=session.session_id,
            gate=gate,
            phase=phase,
            cycle=cycle,
        )
        _validate_open_mutating_gate_reservation(reservation)
        os.fsync(parent_fd)
        _validate_live_mutating_gate_reservation(reservation)
        keep_open = True
        return reservation
    except FileExistsError as exc:
        raise AcceptanceError(
            "a durable mutating-gate reservation already exists; "
            "the acceptance session cannot continue"
        ) from exc
    except OSError as exc:
        raise AcceptanceError(
            f"cannot create durable mutating-gate reservation: {exc}"
        ) from exc
    finally:
        if not keep_open:
            if marker_fd >= 0:
                os.close(marker_fd)
            os.close(parent_fd)


def _validate_open_mutating_gate_reservation(
    reservation: _DurableGateReservation,
) -> None:
    _validate_open_reservation_parent(reservation)
    opened = os.fstat(reservation.fd)
    try:
        named = os.stat(
            reservation.path.name,
            dir_fd=reservation.parent_fd,
            follow_symlinks=False,
        )
    except OSError as exc:
        raise AcceptanceError(
            f"cannot revalidate mutating-gate reservation: {exc}"
        ) from exc
    if not stat.S_ISREG(opened.st_mode) or \
            not os.path.samestat(opened, named):
        raise AcceptanceError(
            "mutating-gate reservation must remain one stable regular file"
        )
    if opened.st_nlink != 1:
        raise AcceptanceError(
            "mutating-gate reservation must not have hard links"
        )
    if opened.st_uid != os.geteuid() or \
            stat.S_IMODE(opened.st_mode) & 0o077:
        raise AcceptanceError(
            "mutating-gate reservation must remain private"
        )
    try:
        os.lseek(reservation.fd, 0, os.SEEK_SET)
        chunks: list[bytes] = []
        remaining = len(reservation.encoded) + 1
        while remaining:
            chunk = os.read(reservation.fd, remaining)
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        observed = b"".join(chunks)
    except OSError as exc:
        raise AcceptanceError(
            f"cannot read mutating-gate reservation: {exc}"
        ) from exc
    if observed != reservation.encoded or \
            opened.st_size != len(reservation.encoded):
        raise AcceptanceError(
            "mutating-gate reservation content changed"
        )


def _validate_live_mutating_gate_reservation(
    reservation: _DurableGateReservation,
) -> None:
    if type(reservation) is not _DurableGateReservation or \
            type(reservation.evidence_fd) is not int or \
            reservation.evidence_fd < 0 or \
            not reservation.evidence_path.is_absolute():
        raise AcceptanceError(
            "mutating-gate reservation capability is malformed"
        )
    _validate_open_reservation_parent(reservation)
    _validate_open_evidence_file(
        reservation.evidence_fd, reservation.evidence_path
    )
    evidence_stat = os.fstat(reservation.evidence_fd)
    if evidence_stat.st_dev != reservation.evidence_device or \
            evidence_stat.st_ino != reservation.evidence_inode:
        raise AcceptanceError(
            "mutating-gate reservation evidence binding changed"
        )
    _validate_open_mutating_gate_reservation(reservation)


def _canonical_blocker_exists(parent_fd: int, name: str) -> bool:
    try:
        os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
    except FileNotFoundError:
        return False
    except OSError as exc:
        raise AcceptanceError(
            f"cannot inspect canonical mutating-gate blocker: {exc}"
        ) from exc
    return True


def _ensure_canonical_mutating_gate_blocker(
    reservation: _DurableGateReservation,
) -> None:
    """Persist a blocker beside the evidence's current canonical pathname."""
    parent_path = reservation.path.parent
    parent_fd = _open_reservation_parent(reservation.path)
    marker_fd = -1
    try:
        parent_stat = os.fstat(parent_fd)
        _validate_open_directory_binding(
            parent_fd,
            parent_path,
            parent_stat.st_dev,
            parent_stat.st_ino,
        )
        flags = os.O_RDWR | os.O_CREAT | os.O_EXCL
        if hasattr(os, "O_NOFOLLOW"):
            flags |= os.O_NOFOLLOW
        if hasattr(os, "O_CLOEXEC"):
            flags |= os.O_CLOEXEC
        for _attempt in range(3):
            try:
                marker_fd = os.open(
                    reservation.path.name,
                    flags,
                    0o600,
                    dir_fd=parent_fd,
                )
            except FileExistsError:
                if not _canonical_blocker_exists(
                    parent_fd, reservation.path.name
                ):
                    continue
                os.fsync(parent_fd)
                _validate_open_directory_binding(
                    parent_fd,
                    parent_path,
                    parent_stat.st_dev,
                    parent_stat.st_ino,
                )
                if not _canonical_blocker_exists(
                    parent_fd, reservation.path.name
                ):
                    continue
                return
            except OSError as exc:
                raise AcceptanceError(
                    "cannot create canonical mutating-gate blocker: "
                    f"{exc}"
                ) from exc

            _write_all(marker_fd, reservation.encoded)
            os.fsync(marker_fd)
            replacement = _DurableGateReservation(
                path=reservation.path,
                fd=marker_fd,
                parent_fd=parent_fd,
                parent_path=parent_path,
                parent_device=parent_stat.st_dev,
                parent_inode=parent_stat.st_ino,
                encoded=reservation.encoded,
                evidence_path=reservation.evidence_path,
                evidence_fd=reservation.evidence_fd,
                evidence_device=reservation.evidence_device,
                evidence_inode=reservation.evidence_inode,
                session_id=reservation.session_id,
                gate=reservation.gate,
                phase=reservation.phase,
                cycle=reservation.cycle,
            )
            _validate_open_mutating_gate_reservation(replacement)
            os.fsync(parent_fd)
            _validate_open_mutating_gate_reservation(replacement)
            return
        raise AcceptanceError(
            "canonical mutating-gate blocker did not remain present"
        )
    finally:
        if marker_fd >= 0:
            os.close(marker_fd)
        os.close(parent_fd)


def _require_reserved_gate_capability(
    reservation: _DurableGateReservation,
    evidence_path: Path,
    session: BadgeAcceptanceSession,
    gate: str,
    phase: str,
    cycle: int | None,
) -> None:
    _validate_live_mutating_gate_reservation(reservation)
    bound_path = Path(os.path.abspath(os.fspath(evidence_path)))
    if reservation.evidence_path != bound_path or \
            reservation.session_id != session.session_id or \
            reservation.gate != gate or \
            reservation.phase != phase or \
            reservation.cycle != cycle:
        raise AcceptanceError(
            "mutating-gate reservation capability does not match the "
            "reserved operation"
        )


def _remove_mutating_gate_reservation(
    reservation: _DurableGateReservation,
) -> None:
    _validate_live_mutating_gate_reservation(reservation)
    unlinked = False
    try:
        os.unlink(
            reservation.path.name,
            dir_fd=reservation.parent_fd,
        )
        unlinked = True
        opened_marker = os.fstat(reservation.fd)
        if opened_marker.st_nlink != 0 or \
                _canonical_blocker_exists(
                    reservation.parent_fd, reservation.path.name
                ):
            raise AcceptanceError(
                "mutating-gate reservation identity changed during unlink"
            )
        _validate_open_evidence_file(
            reservation.evidence_fd, reservation.evidence_path
        )
        os.fsync(reservation.parent_fd)
        _validate_open_evidence_file(
            reservation.evidence_fd, reservation.evidence_path
        )
    except BaseException as exc:
        if unlinked:
            try:
                _recreate_mutating_gate_reservation(reservation)
            except BaseException as recreate_exc:
                raise AcceptanceError(
                    "evidence binding changed after reservation unlink and "
                    "its durable blocker could not be restored"
                ) from recreate_exc
        if isinstance(exc, OSError):
            raise AcceptanceError(
                f"cannot remove durable mutating-gate reservation: {exc}"
            ) from exc
        raise


def _recreate_mutating_gate_reservation(
    reservation: _DurableGateReservation,
) -> None:
    _ensure_canonical_mutating_gate_blocker(reservation)


def _close_mutating_gate_reservation(
    reservation: _DurableGateReservation | None,
) -> None:
    if reservation is None:
        return
    os.close(reservation.fd)
    os.close(reservation.parent_fd)


def _encode_gate_record(
    session: BadgeAcceptanceSession,
    gate: str,
    status: str,
    facts: dict[str, object],
) -> bytes:
    if type(session) is not BadgeAcceptanceSession:
        raise AcceptanceError("acceptance session is malformed")
    if gate not in REQUIRED_GATES:
        raise AcceptanceError(f"unknown physical gate: {gate!r}")
    if status not in ("PASS", "FAIL"):
        raise AcceptanceError("gate status must be PASS or FAIL")
    if not isinstance(facts, dict):
        raise AcceptanceError("gate facts must be an object")
    if status == "PASS":
        _validate_pass_facts(session, gate, facts)
    else:
        if set(facts) - {"error", "phase"} or \
                facts.get("error") not in _FAIL_ERROR_CODES:
            raise AcceptanceError(
                "FAIL evidence requires a recognized privacy-safe error code"
            )
        if "phase" in facts and facts.get("phase") not in _FAIL_PHASE_CODES:
            raise AcceptanceError(
                "FAIL evidence phase must be a recognized privacy-safe code"
            )
    allowed_ids = _allowed_hardware_ids(session)
    _validate_private_facts(facts, allowed_ids)
    return _encode_record(_make_record(
        session, gate, status, facts
    ))


def _append_gate_record_locked(
    fd: int,
    evidence_path: Path,
    session: BadgeAcceptanceSession,
    encoded: bytes,
    *,
    manual_pass_gate: str | None = None,
) -> None:
    _validate_open_evidence_file(fd, evidence_path)
    records = _read_locked_evidence(fd)
    prior = _session_bindings_from_records(records).get(
        session.session_id
    )
    if prior is not None and prior != session:
        raise AcceptanceError(
            f"session {session.session_id!r} hardware binding changed"
        )
    if manual_pass_gate is not None:
        expected_prefix = _MANUAL_PASS_PREFIXES[manual_pass_gate]
        observed_prefix = tuple(
            (record["gate"], record["status"])
            for record in records
            if record["session_id"] == session.session_id
        )
        if observed_prefix != expected_prefix:
            raise AcceptanceError(
                f"{manual_pass_gate} PASS is not the exact next allowed "
                "manual gate record"
            )
    _write_all(fd, encoded)
    os.fsync(fd)
    _validate_open_evidence_file(fd, evidence_path)


def record_gate(
    evidence_path: Path,
    session: BadgeAcceptanceSession,
    gate: str,
    status: str,
    facts: dict[str, object],
) -> None:
    """Append one bounded record without collecting ambient RF observations."""
    encoded = _encode_gate_record(session, gate, status, facts)
    manual_pass_gate = gate if status == "PASS" else None
    if manual_pass_gate is not None and \
            manual_pass_gate not in _MANUAL_PASS_PREFIXES:
        raise AcceptanceError(
            f"{gate} PASS requires its reserved machine record path"
        )
    path = Path(evidence_path)
    _require_no_mutating_gate_reservation(path)
    fd = _open_evidence_for_append(path)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        _require_no_mutating_gate_reservation(path)
        _append_gate_record_locked(
            fd,
            path,
            session,
            encoded,
            manual_pass_gate=manual_pass_gate,
        )
    finally:
        try:
            fcntl.flock(fd, fcntl.LOCK_UN)
        except OSError:
            pass
        os.close(fd)


def _checkpoint_from_serialized(
    facts: object,
) -> VerifiedCycleCheckpoint:
    if not isinstance(facts, dict) or set(facts) != _CYCLE_CHECKPOINT_KEYS:
        raise AcceptanceError("stored cycle checkpoint schema is invalid")
    pre_value = facts.get("pre_snapshot")
    post_value = facts.get("snapshot")
    if not isinstance(pre_value, dict) or \
            set(pre_value) != _CYCLE_PRE_SNAPSHOT_KEYS or \
            not isinstance(post_value, dict) or \
            set(post_value) != _SNAPSHOT_KEYS:
        raise AcceptanceError("stored cycle checkpoint snapshots are invalid")
    values = dict(facts)
    values["candidate_artifacts"] = \
        _candidate_artifacts_from_serialized(
            facts.get("candidate_artifacts"),
            "stored cycle candidate artifacts",
        )
    baseline_value = facts.get("updater_baseline")
    if baseline_value is None:
        values["updater_baseline"] = None
    elif type(baseline_value) is dict and \
            set(baseline_value) == _V078_BASELINE_KEYS:
        values["updater_baseline"] = _validate_v078_baseline_schema(
            VerifiedV078UpdaterBaseline(
                _LIVE_METRIC_ISSUER,
                dict(baseline_value),
            )
        )
    else:
        raise AcceptanceError(
            "stored cycle updater baseline schema is invalid"
        )
    values["pre_snapshot"] = VerifiedCyclePreSnapshot(
        _CYCLE_SNAPSHOT_ISSUER, dict(pre_value)
    )
    values["snapshot"] = _snapshot_from_serialized(
        post_value, "stored cycle post-snapshot"
    )
    return VerifiedCycleCheckpoint(_CYCLE_CHECKPOINT_ISSUER, values)


def _cycle_pre_snapshot_from_serialized(
    value: object,
    label: str,
) -> VerifiedCyclePreSnapshot:
    if type(value) is not dict or set(value) != _CYCLE_PRE_SNAPSHOT_KEYS:
        raise AcceptanceError(f"{label} schema is invalid")
    return VerifiedCyclePreSnapshot(_CYCLE_SNAPSHOT_ISSUER, dict(value))


def _candidate_artifacts_from_serialized(
    value: object,
    label: str,
) -> VerifiedCandidateArtifacts:
    if type(value) is not dict or set(value) != _CANDIDATE_ARTIFACT_KEYS:
        raise AcceptanceError(f"{label} schema is invalid")
    roles: dict[str, _VerifiedCandidateArtifactRole] = {}
    for role in ("uplink", "scanner"):
        role_value = value.get(role)
        if type(role_value) is not dict or \
                set(role_value) != _CANDIDATE_ARTIFACT_ROLE_KEYS:
            raise AcceptanceError(f"{label} {role} schema is invalid")
        roles[role] = _VerifiedCandidateArtifactRole(
            _CANDIDATE_ARTIFACT_ISSUER,
            dict(role_value),
        )
    result = VerifiedCandidateArtifacts(
        _CANDIDATE_ARTIFACT_ISSUER,
        {
            "schema": value.get("schema"),
            "platform_key": value.get("platform_key"),
            "version": value.get("version"),
            "uplink": roles["uplink"],
            "scanner": roles["scanner"],
        },
    )
    version = result.get("version")
    if not isinstance(version, str):
        raise AcceptanceError(f"{label} version is invalid")
    return _validate_candidate_artifacts(result, version)


def _snapshot_from_serialized(
    value: object,
    label: str,
) -> VerifiedBadgeSnapshot:
    if type(value) is not dict or set(value) != _SNAPSHOT_KEYS:
        raise AcceptanceError(f"{label} schema is invalid")
    return VerifiedBadgeSnapshot(_SNAPSHOT_ISSUER, dict(value))


def _chord_rom_snapshot_from_serialized(
    value: object,
    label: str,
) -> VerifiedChordRomBootSnapshot:
    if type(value) is not dict or set(value) != _SNAPSHOT_KEYS:
        raise AcceptanceError(f"{label} schema is invalid")
    return VerifiedChordRomBootSnapshot(
        _CHORD_ROM_BOOT_SNAPSHOT_ISSUER,
        dict(value),
    )


def _aggregate_from_serialized(
    facts: object,
) -> VerifiedThreeCycleAggregate:
    if type(facts) is not dict or \
            set(facts) != _THREE_CYCLE_AGGREGATE_KEYS:
        raise AcceptanceError("stored cycle aggregate schema is invalid")
    values = dict(facts)
    values["candidate_artifacts"] = \
        _candidate_artifacts_from_serialized(
            facts.get("candidate_artifacts"),
            "stored cycle aggregate candidate artifacts",
        )
    values["snapshot"] = _snapshot_from_serialized(
        facts.get("snapshot"), "stored cycle aggregate snapshot"
    )
    return VerifiedThreeCycleAggregate(
        _THREE_CYCLE_AGGREGATE_ISSUER, values
    )


def _pass_facts_from_serialized(
    gate: str,
    facts: object,
) -> dict[str, Any]:
    if type(facts) is not dict:
        raise AcceptanceError(f"{gate} stored PASS facts are invalid")
    if gate == "three-update-cycles":
        return _aggregate_from_serialized(facts)
    values = dict(facts)
    if gate in (
        "android-control-reconnect",
        "interrupted-upload",
        "chord-rom-recovery",
    ):
        values["candidate_artifacts"] = \
            _candidate_artifacts_from_serialized(
                facts.get("candidate_artifacts"),
                f"{gate} candidate artifacts",
            )
    if gate == "android-control-reconnect":
        values["snapshot"] = _cycle_pre_snapshot_from_serialized(
            facts.get("snapshot"), f"{gate} pre-update snapshot"
        )
    else:
        values["snapshot"] = _snapshot_from_serialized(
            facts.get("snapshot"), f"{gate} snapshot"
        )
    if gate == "interrupted-upload":
        for key in ("baseline_snapshot", "recovered_snapshot"):
            values[key] = _snapshot_from_serialized(
                facts.get(key), f"interrupted-upload {key}"
            )
    if gate == "chord-rom-recovery":
        values["rom_boot_snapshot"] = \
            _chord_rom_snapshot_from_serialized(
                facts.get("rom_boot_snapshot"),
                "chord-rom-recovery ROM boot snapshot",
            )
        values = VerifiedChordRecoveryFacts(
            _CHORD_RECOVERY_FACT_ISSUER,
            values,
        )
    return values


def _validate_mutating_gate_prefix(
    records: list[dict[str, Any]],
    session: BadgeAcceptanceSession,
    gate: str,
    *,
    cycle: int | None = None,
) -> _AnchoredCandidate:
    """Prove the selected session is at the exact next mutating gate."""
    if type(session) is not BadgeAcceptanceSession:
        raise AcceptanceError("mutating gate session is malformed")
    if gate == "three-update-cycles":
        if type(cycle) is not int or cycle not in (1, 2, 3):
            raise AcceptanceError(
                "update cycle must be exact integer 1, 2, or 3"
            )
        expected_sequence = [
            ("android-control-reconnect", "PASS"),
            *[
                ("three-update-cycles", "CHECKPOINT")
                for _prior_cycle in range(cycle - 1)
            ],
        ]
    elif gate == "interrupted-upload":
        if cycle is not None:
            raise AcceptanceError(
                "interrupted upload reservation does not accept a cycle"
            )
        expected_sequence = [
            ("android-control-reconnect", "PASS"),
            ("three-update-cycles", "CHECKPOINT"),
            ("three-update-cycles", "CHECKPOINT"),
            ("three-update-cycles", "CHECKPOINT"),
            ("three-update-cycles", "PASS"),
        ]
    elif gate == "chord-rom-recovery":
        if cycle is not None:
            raise AcceptanceError(
                "chord ROM recovery reservation does not accept a cycle"
            )
        expected_sequence = [
            ("android-control-reconnect", "PASS"),
            ("three-update-cycles", "CHECKPOINT"),
            ("three-update-cycles", "CHECKPOINT"),
            ("three-update-cycles", "CHECKPOINT"),
            ("three-update-cycles", "PASS"),
            ("interrupted-upload", "PASS"),
        ]
    else:
        raise AcceptanceError(
            f"gate {gate!r} is not a mutating acceptance gate"
        )

    bindings = _session_bindings_from_records(records)
    anchored = bindings.get(session.session_id)
    if anchored is None:
        raise AcceptanceError(
            "mutating gate requires a previously anchored session"
        )
    if anchored != session:
        raise AcceptanceError(
            f"session {session.session_id!r} hardware binding changed"
        )
    selected = [
        record for record in records
        if record["session_id"] == session.session_id
    ]
    if any(record["status"] == "FAIL" for record in selected):
        raise AcceptanceError(
            "selected acceptance session has a recorded failure"
        )
    observed_sequence = [
        (record["gate"], record["status"]) for record in selected
    ]
    if observed_sequence != expected_sequence:
        if gate == "three-update-cycles":
            observed_checkpoints = sum(
                record["gate"] == "three-update-cycles"
                and record["status"] == "CHECKPOINT"
                for record in selected
            )
            raise AcceptanceError(
                "update checkpoint must record cycle "
                f"{observed_checkpoints + 1}; an extra, duplicate, or "
                "out-of-order record exists"
            )
        raise AcceptanceError(
            "mutating gate is not the exact next allowed session record; "
            "an extra, duplicate, or out-of-order record exists"
        )

    gate_one = _pass_facts_from_serialized(
        "android-control-reconnect", selected[0]["facts"]
    )
    _validate_pass_facts(
        session, "android-control-reconnect", gate_one
    )
    gate_one_snapshot = gate_one["snapshot"]
    gate_one_version = gate_one_snapshot.get("candidate_version")
    if not isinstance(gate_one_version, str):
        raise AcceptanceError(
            "anchored Gate 1 candidate version is malformed"
        )
    _validate_cycle_pre_snapshot(
        session,
        gate_one_snapshot,
        gate_one_version,
        1,
    )
    gate_one_artifacts = _validate_candidate_artifacts(
        gate_one.get("candidate_artifacts"),
        gate_one_version,
    )

    checkpoint_records = [
        record for record in selected
        if record["gate"] == "three-update-cycles"
        and record["status"] == "CHECKPOINT"
    ]
    checkpoints: list[VerifiedCycleCheckpoint] = []
    generations: list[int] = []
    candidate_version: str | None = None
    updater_baseline: VerifiedV078UpdaterBaseline | None = None
    for expected_cycle, record in enumerate(
        checkpoint_records, start=1
    ):
        checkpoint = _checkpoint_from_serialized(record["facts"])
        _validate_cycle_checkpoint(session, checkpoint)
        if checkpoint["cycle"] != expected_cycle:
            raise AcceptanceError(
                "stored update checkpoint order is invalid: "
                f"expected cycle {expected_cycle}, "
                f"got {checkpoint['cycle']!r}"
            )
        version = checkpoint["candidate_version"]
        if dict(checkpoint["candidate_artifacts"]) != \
                dict(gate_one_artifacts):
            raise AcceptanceError(
                "stored update checkpoint candidate artifacts changed "
                "from the anchored Gate 1 candidate"
            )
        if candidate_version is None:
            candidate_version = str(version)
        elif version != candidate_version:
            raise AcceptanceError(
                "stored update checkpoint candidate version changed"
            )
        checkpoint_baseline = checkpoint["updater_baseline"]
        if checkpoint_baseline is not None:
            checkpoint_baseline = _validate_v078_baseline_binding(
                checkpoint_baseline,
                session,
            )
        if updater_baseline is None:
            updater_baseline = checkpoint_baseline
        elif checkpoint_baseline is None or \
                dict(checkpoint_baseline) != dict(updater_baseline):
            raise AcceptanceError(
                "stored update checkpoint changed its live .78 lineage"
            )
        generation = checkpoint["stage_generation"]
        if generations and generation <= generations[-1]:
            raise AcceptanceError(
                "stored update checkpoint generations do not "
                "strictly advance"
            )
        if checkpoints:
            _validate_adjacent_cycle_boot_lineage(
                checkpoints[-1],
                checkpoint,
            )
        generations.append(generation)
        checkpoints.append(checkpoint)

    if checkpoints:
        cycle_one_pre = checkpoints[0]["pre_snapshot"]
        for slot in ("ble", "wifi"):
            if cycle_one_pre[f"{slot}_version"] != \
                    gate_one_snapshot[f"{slot}_version"]:
                raise AcceptanceError(
                    f"cycle 1 {slot} scanner version changed from the "
                    "anchored Gate 1 pre-update state"
                )

    if candidate_version is not None and \
            candidate_version != gate_one_version:
        raise AcceptanceError(
            "update checkpoint version changed from the anchored Gate 1 "
            "snapshot"
        )
    if gate_one_version == flash.UPDATE_MAINTENANCE_MIN_VERSION and \
            checkpoints and updater_baseline is None:
        raise AcceptanceError(
            "canary update checkpoints lack live .78 acquisition lineage"
        )

    if gate in ("interrupted-upload", "chord-rom-recovery"):
        aggregate_record = (
            selected[-1]
            if gate == "interrupted-upload"
            else selected[-2]
        )
        aggregate = _aggregate_from_serialized(
            aggregate_record["facts"]
        )
        _validate_pass_facts(
            session, "three-update-cycles", aggregate
        )
        if candidate_version is None or \
                aggregate["candidate_version"] != candidate_version:
            raise AcceptanceError(
                "three-update-cycles aggregate candidate changed"
            )
        if aggregate["checkpoint_generations"] != generations:
            raise AcceptanceError(
                "three-update-cycles aggregate generations do not match "
                "its checkpoints"
            )
        if dict(aggregate["candidate_artifacts"]) != \
                dict(gate_one_artifacts):
            raise AcceptanceError(
                "three-update-cycles aggregate candidate artifacts changed "
                "from Gate 1"
            )
        if dict(aggregate["snapshot"]) != \
                dict(checkpoints[-1]["snapshot"]):
            raise AcceptanceError(
                "three-update-cycles aggregate snapshot does not match "
                "cycle 3"
            )
        if aggregate["first_cycle_manual_relay_commands"] != \
                checkpoints[0]["manual_relay_commands"] or \
                aggregate["recovery_manual_relay_commands"] != sum(
                    checkpoint["manual_relay_commands"]
                    for checkpoint in checkpoints[1:]
                ):
            raise AcceptanceError(
                "three-update-cycles aggregate relay totals do not match "
                "its checkpoints"
            )
    if gate == "chord-rom-recovery":
        interrupted = _pass_facts_from_serialized(
            "interrupted-upload", selected[-1]["facts"]
        )
        _validate_pass_facts(
            session, "interrupted-upload", interrupted
        )
        _require_interrupted_baseline_matches_cycle_three(
            interrupted,
            checkpoints[-1]["snapshot"],
        )
        if dict(interrupted["candidate_artifacts"]) != \
                dict(gate_one_artifacts):
            raise AcceptanceError(
                "interrupted-upload candidate artifacts changed from Gate 1"
            )
        for key in (
            "snapshot", "baseline_snapshot", "recovered_snapshot",
        ):
            _validate_verified_snapshot(
                session,
                interrupted[key],
                expected_version=gate_one_version,
            )
    return _AnchoredCandidate(
        version=gate_one_version,
        artifacts=gate_one_artifacts,
        updater_baseline=updater_baseline,
    )


def _record_update_cycle_checkpoint_impl(
    evidence_path: Path,
    session: BadgeAcceptanceSession,
    checkpoint: VerifiedCycleCheckpoint,
    *,
    reservation: _DurableGateReservation | None = None,
) -> bool:
    """Append one ordered checkpoint and aggregate PASS after cycle three."""
    if type(session) is not BadgeAcceptanceSession:
        raise AcceptanceError("acceptance session is malformed")
    _validate_cycle_checkpoint(session, checkpoint)
    allowed_ids = _allowed_hardware_ids(session)
    _validate_private_facts(checkpoint, allowed_ids)

    path = Path(evidence_path)
    owns_fd = reservation is None
    if owns_fd:
        _require_no_mutating_gate_reservation(path)
        fd = _open_evidence_for_append(path)
    else:
        _require_reserved_gate_capability(
            reservation,
            path,
            session,
            "three-update-cycles",
            f"update_cycle_{checkpoint['cycle']}",
            checkpoint["cycle"],
        )
        fd = reservation.evidence_fd
    try:
        if owns_fd:
            fcntl.flock(fd, fcntl.LOCK_EX)
            _require_no_mutating_gate_reservation(path)
        records = _read_locked_evidence(fd)
        anchored = _validate_mutating_gate_prefix(
            records,
            session,
            "three-update-cycles",
            cycle=checkpoint["cycle"],
        )
        if checkpoint["candidate_version"] != anchored.version:
            raise AcceptanceError(
                "update checkpoint candidate changed from the anchored "
                "Gate 1 snapshot"
            )
        if dict(checkpoint["candidate_artifacts"]) != \
                dict(anchored.artifacts):
            raise AcceptanceError(
                "update checkpoint candidate artifacts changed from the "
                "anchored Gate 1 candidate"
            )
        if checkpoint["candidate_version"] == \
                flash.UPDATE_MAINTENANCE_MIN_VERSION:
            checkpoint_baseline = _validate_v078_baseline_binding(
                checkpoint["updater_baseline"],
                session,
            )
            if checkpoint["cycle"] == 1:
                if anchored.updater_baseline is not None:
                    raise AcceptanceError(
                        "cycle 1 unexpectedly inherited an older baseline"
                    )
            elif anchored.updater_baseline is None or \
                    dict(checkpoint_baseline) != \
                    dict(anchored.updater_baseline):
                raise AcceptanceError(
                    "update checkpoint changed its linked .78 baseline"
                )
        if checkpoint["cycle"] == 1:
            gate_one_record = next(
                record for record in records
                if record.get("session_id") == session.session_id
                and record.get("gate") == "android-control-reconnect"
                and record.get("status") == "PASS"
            )
            gate_one = _pass_facts_from_serialized(
                "android-control-reconnect",
                gate_one_record.get("facts"),
            )
            gate_one_pre = gate_one["snapshot"]
            cycle_one_pre = checkpoint["pre_snapshot"]
            for slot in ("ble", "wifi"):
                if cycle_one_pre[f"{slot}_version"] != \
                        gate_one_pre[f"{slot}_version"]:
                    raise AcceptanceError(
                        f"cycle 1 {slot} scanner version changed from the "
                        "anchored Gate 1 pre-update state"
                    )
        bindings = _session_bindings_from_records(records)
        prior = bindings.get(session.session_id)
        if prior is None:
            raise AcceptanceError(
                "update cycles require a previously anchored session"
            )
        if prior != session:
            raise AcceptanceError(
                f"session {session.session_id!r} hardware binding changed"
            )

        stored_checkpoints: list[VerifiedCycleCheckpoint] = []
        for record in records:
            if record.get("session_id") != session.session_id or \
                    record.get("gate") != "three-update-cycles":
                continue
            if record.get("status") == "PASS":
                raise AcceptanceError(
                    "three-update-cycles already has an aggregate PASS"
                )
            if record.get("status") == "FAIL":
                raise AcceptanceError(
                    "three-update-cycles has a recorded failure"
                )
            if record.get("status") != "CHECKPOINT":
                raise AcceptanceError(
                    "three-update-cycles contains an invalid record status"
                )
            stored = _checkpoint_from_serialized(record.get("facts"))
            _validate_cycle_checkpoint(session, stored)
            expected_stored_cycle = len(stored_checkpoints) + 1
            if stored["cycle"] != expected_stored_cycle:
                raise AcceptanceError(
                    "stored update checkpoint order is invalid: "
                    f"expected cycle {expected_stored_cycle}, "
                    f"got {stored['cycle']!r}"
                )
            if stored_checkpoints and \
                    stored["stage_generation"] <= \
                    stored_checkpoints[-1]["stage_generation"]:
                raise AcceptanceError(
                    "stored update checkpoint generations do not "
                    "strictly advance"
                )
            if stored_checkpoints and \
                    stored["candidate_version"] != \
                    stored_checkpoints[0]["candidate_version"]:
                raise AcceptanceError(
                    "stored update checkpoint candidate version changed"
                )
            if dict(stored["candidate_artifacts"]) != \
                    dict(anchored.artifacts):
                raise AcceptanceError(
                    "stored update checkpoint candidate artifacts changed"
                )
            if stored_checkpoints:
                _validate_adjacent_cycle_boot_lineage(
                    stored_checkpoints[-1],
                    stored,
                )
            stored_checkpoints.append(stored)

        next_cycle = len(stored_checkpoints) + 1
        if next_cycle > 3 or checkpoint["cycle"] != next_cycle:
            raise AcceptanceError(
                f"update checkpoint must record cycle {next_cycle}"
            )
        generations = [
            int(stored["stage_generation"])
            for stored in stored_checkpoints
        ]
        generation = int(checkpoint["stage_generation"])
        if generations and generation <= generations[-1]:
            raise AcceptanceError(
                "update checkpoint generation must strictly advance"
            )
        versions = {
            stored["candidate_version"] for stored in stored_checkpoints
        }
        if versions and versions != {checkpoint["candidate_version"]}:
            raise AcceptanceError(
                "update checkpoint candidate version changed"
            )
        if stored_checkpoints:
            _validate_adjacent_cycle_boot_lineage(
                stored_checkpoints[-1],
                checkpoint,
            )

        checkpoint_record = _make_record(
            session,
            "three-update-cycles",
            "CHECKPOINT",
            checkpoint,
        )
        encoded = _encode_record(checkpoint_record)
        aggregate_appended = checkpoint["cycle"] == 3
        if aggregate_appended:
            all_generations = generations + [generation]
            aggregate = VerifiedThreeCycleAggregate(
                _THREE_CYCLE_AGGREGATE_ISSUER,
                {
                    "snapshot": checkpoint["snapshot"],
                    "candidate_version": checkpoint["candidate_version"],
                    "candidate_artifacts": checkpoint[
                        "candidate_artifacts"
                    ],
                    "cycles_completed": 3,
                    "strictly_older_setup": True,
                    "automatic_convergence": True,
                    "checkpoint_generations": all_generations,
                    "first_cycle_manual_relay_commands": 0,
                    "recovery_manual_relay_commands": 4,
                },
            )
            _validate_pass_facts(
                session, "three-update-cycles", aggregate
            )
            _validate_private_facts(aggregate, allowed_ids)
            encoded += _encode_record(_make_record(
                session,
                "three-update-cycles",
                "PASS",
                aggregate,
            ))
        if reservation is None:
            _validate_open_evidence_file(fd, path)
        else:
            _validate_live_mutating_gate_reservation(reservation)
        _write_all(fd, encoded)
        os.fsync(fd)
        if reservation is None:
            _validate_open_evidence_file(fd, path)
        else:
            _validate_live_mutating_gate_reservation(reservation)
        return aggregate_appended
    finally:
        if owns_fd:
            try:
                fcntl.flock(fd, fcntl.LOCK_UN)
            except OSError:
                pass
            os.close(fd)


def record_update_cycle_checkpoint(
    evidence_path: Path,
    session: BadgeAcceptanceSession,
    checkpoint: VerifiedCycleCheckpoint,
) -> bool:
    """Lock and append one ordered checkpoint through the public path."""
    return _record_update_cycle_checkpoint_impl(
        evidence_path, session, checkpoint
    )


def _record_reserved_update_cycle_checkpoint(
    evidence_path: Path,
    session: BadgeAcceptanceSession,
    checkpoint: VerifiedCycleCheckpoint,
    reservation: _DurableGateReservation,
) -> bool:
    """Append one checkpoint only with its live mutating-gate capability."""
    return _record_update_cycle_checkpoint_impl(
        evidence_path,
        session,
        checkpoint,
        reservation=reservation,
    )


def _run_reserved_mutating_gate(
    evidence_path: Path,
    session: BadgeAcceptanceSession,
    session_input: _PrivateSessionInput,
    gate: str,
    phase: str,
    action: Callable[[_MutatingGateContext], object],
    recorder: Callable[[_DurableGateReservation, object], None],
    *,
    cycle: int | None = None,
    updater_baseline_port: str | None = None,
    pre_action_validator: Callable[
        [_MutatingGateContext, list[dict[str, Any]]],
        None,
    ] | None = None,
) -> object:
    """Hold one evidence reservation through preflight, action, and record."""
    path = Path(evidence_path)
    fd = _open_evidence_for_append(path)
    reservation: _DurableGateReservation | None = None
    operation_registry: _OperationRegistry | None = None
    state_root: _RetainedStateRoot | None = None
    retained_marker: _RetainedOperationMarker | None = None
    operation_sha256: str | None = None
    attempt_id: str | None = None
    action_started = False
    reservation_removed = False
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        records = _read_locked_evidence(fd)
        anchored = _validate_mutating_gate_prefix(
            records, session, gate, cycle=cycle
        )
        _validate_private_session_input(session_input)
        current_version = flash.repo_version(
            flash.PLATFORMS[CANARY_PLATFORM_KEY]
        )
        if current_version != anchored.version:
            raise AcceptanceError(
                "repository candidate changed from the anchored Gate 1 "
                "version before mutation"
            )
        platform = flash.PLATFORMS[CANARY_PLATFORM_KEY]
        slots = ["ble", "wifi"]
        flash.require_artifacts(platform, True, slots)
        frozen = flash._prepare_frozen_usb_firmware_artifacts(
            platform,
            True,
            slots,
        )
        current_artifacts = verify_candidate_artifacts(
            frozen,
            anchored.version,
        )
        if dict(current_artifacts) != dict(anchored.artifacts):
            raise AcceptanceError(
                "repository candidate bytes changed from the anchored "
                "Gate 1 artifacts before mutation"
            )
        context = _MutatingGateContext(
            version=anchored.version,
            artifacts=anchored.artifacts,
            frozen=frozen,
            updater_baseline=anchored.updater_baseline,
        )
        if pre_action_validator is not None:
            if not callable(pre_action_validator):
                raise AcceptanceError(
                    "mutating gate pre-action validator is malformed"
                )
            pre_action_validator(context, records)
        operation_sha256 = _operation_identity_sha256(
            session,
            context.version,
            context.artifacts,
            gate,
            phase,
            cycle,
        )
        operation_registry = _open_operation_registry()
        state_root = _open_retained_state_root()
        attempt_id = _prepare_operation_registry_claim(
            operation_registry,
            operation_sha256,
            state_root,
        )
        retained_marker = _create_retained_operation_marker(
            state_root,
            operation_sha256,
        )
        _arm_operation_registry_reservation(
            operation_registry,
            operation_sha256,
            attempt_id,
        )
        reservation = _create_mutating_gate_reservation(
            path,
            fd,
            session,
            gate,
            phase,
            cycle=cycle,
        )
        _validate_private_session_input(session_input)
        _validate_retained_state_root_before_action(
            state_root, retained_marker
        )
        _validate_live_mutating_gate_reservation(reservation)
        if context.version == flash.UPDATE_MAINTENANCE_MIN_VERSION:
            if gate == "three-update-cycles" and cycle == 1:
                if updater_baseline_port is None:
                    raise AcceptanceError(
                        "cycle 1 lacks a live .78 updater port"
                    )
                context = _MutatingGateContext(
                    version=context.version,
                    artifacts=context.artifacts,
                    frozen=context.frozen,
                    updater_baseline=(
                        _capture_reserved_v078_updater_baseline(
                            updater_baseline_port,
                            session,
                            reservation,
                        )
                    ),
                )
            elif context.updater_baseline is None:
                raise AcceptanceError(
                    "canary mutation lacks linked live .78 baseline evidence"
                )
        _start_operation_registry_claim(
            operation_registry,
            operation_sha256,
            attempt_id,
        )
        action_started = True
        try:
            result = action(context)
            if not _operation_registry_claim_started(
                operation_registry,
                operation_sha256,
                attempt_id,
            ):
                raise AcceptanceError(
                    "operation registry lost its durable STARTED record"
                )
            _validate_private_session_input(session_input)
            _validate_retained_operation_marker_identity(
                retained_marker,
                require_canonical_state_root=True,
            )
            recorder(reservation, result)
            if not _operation_registry_claim_started(
                operation_registry,
                operation_sha256,
                attempt_id,
            ):
                raise AcceptanceError(
                    "operation registry lost its durable STARTED record"
                )
            _validate_retained_operation_marker_identity(
                retained_marker,
                require_canonical_state_root=True,
            )
        except Exception as exc:
            failure = _encode_gate_record(
                session,
                gate,
                "FAIL",
                {
                    "error": "hardware_gate_failed",
                    "phase": phase,
                },
            )
            try:
                _append_gate_record_locked(
                    fd, path, session, failure
                )
            except Exception as append_exc:
                raise AcceptanceError(
                    "mutating acceptance gate failed after start and its "
                    "permanent FAIL evidence could not be recorded"
                ) from append_exc
            _remove_mutating_gate_reservation(reservation)
            reservation_removed = True
            raise AcceptanceError(
                "mutating acceptance gate failed after start; permanent "
                "FAIL evidence was recorded"
            ) from exc
        _remove_mutating_gate_reservation(reservation)
        reservation_removed = True
        return result
    except BaseException:
        if action_started and reservation is not None and \
                not reservation_removed:
            try:
                _ensure_canonical_mutating_gate_blocker(reservation)
            except BaseException as blocker_exc:
                raise AcceptanceError(
                    "mutating acceptance gate cannot return without a "
                    "durable canonical blocker"
                ) from blocker_exc
        raise
    finally:
        cleanup_error: BaseException | None = None
        durable_started = action_started
        if not durable_started and operation_registry is not None and \
                operation_sha256 is not None and attempt_id is not None:
            try:
                durable_started = _operation_registry_claim_started(
                    operation_registry,
                    operation_sha256,
                    attempt_id,
                )
            except BaseException as registry_state_exc:
                durable_started = True
                cleanup_error = registry_state_exc
        if not durable_started:
            if reservation is not None:
                try:
                    _remove_mutating_gate_reservation(reservation)
                except BaseException as reservation_cleanup_exc:
                    cleanup_error = reservation_cleanup_exc
            else:
                try:
                    _require_no_mutating_gate_reservation(path)
                except BaseException as reservation_state_exc:
                    cleanup_error = reservation_state_exc
            if cleanup_error is None and retained_marker is not None:
                try:
                    _cleanup_pre_action_retained_operation_marker(
                        retained_marker
                    )
                except BaseException as cleanup_exc:
                    cleanup_error = cleanup_exc
            if cleanup_error is None and \
                    operation_registry is not None and \
                    operation_sha256 is not None and \
                    attempt_id is not None:
                try:
                    _cancel_operation_registry_claim(
                        operation_registry,
                        operation_sha256,
                        attempt_id,
                    )
                except BaseException as cancel_exc:
                    cleanup_error = cancel_exc
        _close_retained_operation_marker(retained_marker)
        _close_retained_state_root(state_root)
        _close_mutating_gate_reservation(reservation)
        _close_operation_registry(operation_registry)
        try:
            fcntl.flock(fd, fcntl.LOCK_UN)
        except OSError:
            pass
        os.close(fd)
        if cleanup_error is not None:
            raise AcceptanceError(
                "mutating gate authority cleanup or state validation "
                "failed; manual state repair is required"
            ) from cleanup_error


def run_reserved_update_cycle_checkpoint(
    evidence_path: Path,
    port: str,
    session: BadgeAcceptanceSession,
    session_input: _PrivateSessionInput,
    cycle: int,
) -> VerifiedCycleCheckpoint:
    """Run one exact next cycle while holding its evidence reservation."""
    if type(cycle) is not int or cycle not in (1, 2, 3):
        raise AcceptanceError(
            "update cycle must be exact integer 1, 2, or 3"
        )
    pre_snapshot: VerifiedCyclePreSnapshot | None = None

    def pre_action(
        context: _MutatingGateContext,
        records: list[dict[str, Any]],
    ) -> None:
        nonlocal pre_snapshot
        descriptor = _trusted_session_uplink_descriptor(port, session)
        status = flash.probe_application(descriptor, 5)
        if status is None:
            raise AcceptanceError(
                "update cycle preflight could not prove the anchored uplink "
                "application"
            )
        observed = verify_cycle_pre_snapshot(
            status,
            session,
            context.version,
            cycle,
        )
        if cycle == 1:
            gate_one_record = next(
                record for record in records
                if record.get("session_id") == session.session_id
                and record.get("gate") == "android-control-reconnect"
                and record.get("status") == "PASS"
            )
            gate_one = _pass_facts_from_serialized(
                "android-control-reconnect",
                gate_one_record.get("facts"),
            )
            for slot in ("ble", "wifi"):
                if observed[f"{slot}_version"] != \
                        gate_one["snapshot"][f"{slot}_version"]:
                    raise AcceptanceError(
                        f"cycle 1 {slot} scanner version changed from the "
                        "anchored Gate 1 pre-update state before mutation"
                    )
        else:
            prior_records = [
                record
                for record in records
                if record.get("session_id") == session.session_id
                and record.get("gate") == "three-update-cycles"
                and record.get("status") == "CHECKPOINT"
            ]
            if len(prior_records) != cycle - 1:
                raise AcceptanceError(
                    f"cycle {cycle} lacks its exact prior checkpoint"
                )
            prior_checkpoint = _checkpoint_from_serialized(
                prior_records[-1].get("facts")
            )
            _validate_cycle_checkpoint(session, prior_checkpoint)
            _require_exact_cycle_probe_transition(
                prior_checkpoint["snapshot"],
                observed,
                probe_count=1,
                label=f"cycle {cycle} pre-action probe",
            )
        pre_snapshot = observed

    def record(
        reservation: _DurableGateReservation,
        value: object,
    ) -> None:
        _record_reserved_update_cycle_checkpoint(
            evidence_path,
            session,
            value,
            reservation,
        )

    result = _run_reserved_mutating_gate(
        evidence_path,
        session,
        session_input,
        "three-update-cycles",
        f"update_cycle_{cycle}",
        lambda context: run_update_cycle_checkpoint(
            port,
            session,
            cycle,
            expected_version=context.version,
            frozen_artifacts=context.frozen,
            candidate_artifacts=context.artifacts,
            expected_pre_snapshot=pre_snapshot,
            updater_baseline=context.updater_baseline,
        ),
        record,
        cycle=cycle,
        updater_baseline_port=port,
        pre_action_validator=pre_action,
    )
    if type(result) is not VerifiedCycleCheckpoint:
        raise AcceptanceError(
            "reserved update cycle returned malformed checkpoint proof"
        )
    return result


def run_reserved_interrupted_upload_gate(
    evidence_path: Path,
    port: str,
    session: BadgeAcceptanceSession,
    session_input: _PrivateSessionInput,
    *,
    abort_after: int,
) -> tuple[BadgeAcceptanceSession, dict[str, object]]:
    """Run Gate 3 while holding its exact-prefix evidence reservation."""

    def record(
        reservation: _DurableGateReservation,
        value: object,
    ) -> None:
        if not isinstance(value, tuple) or len(value) != 2:
            raise AcceptanceError(
                "interrupted-upload runner returned malformed proof"
            )
        observed_session, facts = value
        if observed_session != session or not isinstance(facts, dict):
            raise AcceptanceError(
                "interrupted-upload runner changed its reserved session"
            )
        records = _read_locked_evidence(reservation.evidence_fd)
        cycle_three_records = [
            record for record in records
            if record.get("session_id") == session.session_id
            and record.get("gate") == "three-update-cycles"
            and record.get("status") == "CHECKPOINT"
            and isinstance(record.get("facts"), dict)
            and record["facts"].get("cycle") == 3
        ]
        if len(cycle_three_records) != 1:
            raise AcceptanceError(
                "interrupted-upload record lacks exact cycle 3 lineage"
            )
        cycle_three = _checkpoint_from_serialized(
            cycle_three_records[0]["facts"]
        )
        _validate_cycle_checkpoint(session, cycle_three)
        _require_interrupted_baseline_matches_cycle_three(
            facts,
            cycle_three["snapshot"],
        )
        encoded = _encode_gate_record(
            session, "interrupted-upload", "PASS", facts
        )
        _require_reserved_gate_capability(
            reservation,
            Path(evidence_path),
            session,
            "interrupted-upload",
            "interrupted_upload",
            None,
        )
        _append_gate_record_locked(
            reservation.evidence_fd,
            reservation.evidence_path,
            session,
            encoded,
        )
        _require_reserved_gate_capability(
            reservation,
            Path(evidence_path),
            session,
            "interrupted-upload",
            "interrupted_upload",
            None,
        )

    result = _run_reserved_mutating_gate(
        evidence_path,
        session,
        session_input,
        "interrupted-upload",
        "interrupted_upload",
        lambda context: run_interrupted_upload_gate(
            port,
            expected_session=session,
            abort_after=abort_after,
            expected_version=context.version,
            frozen_artifacts=context.frozen,
            candidate_artifacts=context.artifacts,
            updater_baseline=context.updater_baseline,
        ),
        record,
        updater_baseline_port=port,
    )
    if not isinstance(result, tuple) or len(result) != 2 or \
            result[0] != session or not isinstance(result[1], dict):
        raise AcceptanceError(
            "reserved interrupted-upload proof is malformed"
        )
    return result


def run_reserved_chord_rom_recovery_gate(
    evidence_path: Path,
    port: str,
    session: BadgeAcceptanceSession,
    session_input: _PrivateSessionInput,
) -> dict[str, object]:
    """Run and atomically record machine-issued Gate 4 proof."""

    def record(
        reservation: _DurableGateReservation,
        value: object,
    ) -> None:
        if type(value) is not VerifiedChordRecoveryFacts:
            raise AcceptanceError(
                "chord ROM recovery runner returned malformed proof"
            )
        encoded = _encode_gate_record(
            session, "chord-rom-recovery", "PASS", value
        )
        _require_reserved_gate_capability(
            reservation,
            Path(evidence_path),
            session,
            "chord-rom-recovery",
            "chord_recovery",
            None,
        )
        _append_gate_record_locked(
            reservation.evidence_fd,
            reservation.evidence_path,
            session,
            encoded,
        )
        _require_reserved_gate_capability(
            reservation,
            Path(evidence_path),
            session,
            "chord-rom-recovery",
            "chord_recovery",
            None,
        )

    result = _run_reserved_mutating_gate(
        evidence_path,
        session,
        session_input,
        "chord-rom-recovery",
        "chord_recovery",
        lambda context: run_chord_rom_recovery_gate(
            port,
            session,
            expected_version=context.version,
            frozen_artifacts=context.frozen,
            candidate_artifacts=context.artifacts,
            updater_baseline=context.updater_baseline,
        ),
        record,
    )
    if type(result) is not VerifiedChordRecoveryFacts:
        raise AcceptanceError(
            "reserved chord ROM recovery proof is malformed"
        )
    return result


def _open_existing_evidence_readonly(path: Path) -> int:
    flags = os.O_RDONLY | os.O_NONBLOCK
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    try:
        fd = os.open(path, flags)
    except OSError as exc:
        raise AcceptanceError(
            f"cannot open existing evidence read-only: {exc}"
        ) from exc
    try:
        _validate_open_evidence_file(fd, path)
    except BaseException:
        os.close(fd)
        raise
    return fd


def _validate_expected_version(value: object, expected_version: str,
                               label: str) -> None:
    if value != expected_version:
        raise AcceptanceError(
            f"{label} does not match expected version "
            f"{expected_version!r}"
        )


def _verify_acceptance_records(
    records: list[dict[str, Any]],
    session: BadgeAcceptanceSession,
    expected_version: str,
) -> dict[str, object]:
    bindings = _session_bindings_from_records(records)
    anchored = bindings.get(session.session_id)
    if anchored is None:
        raise AcceptanceError(
            f"session {session.session_id!r} is not present in evidence"
        )
    if anchored != session:
        raise AcceptanceError(
            f"session {session.session_id!r} hardware binding changed"
        )
    selected = [
        record for record in records
        if record["session_id"] == session.session_id
    ]
    if any(record["status"] == "FAIL" for record in selected):
        raise AcceptanceError(
            "selected acceptance session has a recorded failure"
        )

    pass_records: dict[str, list[dict[str, Any]]] = {
        gate: [
            record for record in selected
            if record["gate"] == gate and record["status"] == "PASS"
        ]
        for gate in REQUIRED_GATES
    }
    if len(pass_records["three-update-cycles"]) != 1:
        raise AcceptanceError(
            "three-update-cycles requires exactly one aggregate PASS"
        )
    for gate in REQUIRED_GATES:
        if gate == "three-update-cycles":
            continue
        count = len(pass_records[gate])
        if count != 1:
            raise AcceptanceError(
                f"{gate} requires exactly one PASS; found {count}"
            )

    checkpoint_records = [
        record for record in selected
        if record["gate"] == "three-update-cycles"
        and record["status"] == "CHECKPOINT"
    ]
    if len(checkpoint_records) != 3:
        raise AcceptanceError(
            "three-update-cycles requires exactly three checkpoints"
        )

    expected_sequence = [
        ("android-control-reconnect", "PASS"),
        ("three-update-cycles", "CHECKPOINT"),
        ("three-update-cycles", "CHECKPOINT"),
        ("three-update-cycles", "CHECKPOINT"),
        ("three-update-cycles", "PASS"),
        ("interrupted-upload", "PASS"),
        ("chord-rom-recovery", "PASS"),
        ("no-host-reboot", "PASS"),
        ("power-state-audit", "PASS"),
    ]
    observed_sequence = [
        (record["gate"], record["status"]) for record in selected
    ]
    if observed_sequence != expected_sequence:
        raise AcceptanceError(
            "selected session has an extra, duplicate, or out-of-order "
            "acceptance gate record"
        )

    checkpoints: list[VerifiedCycleCheckpoint] = []
    generations: list[int] = []
    for expected_cycle, record in enumerate(checkpoint_records, start=1):
        checkpoint = _checkpoint_from_serialized(record["facts"])
        _validate_cycle_checkpoint(session, checkpoint)
        if checkpoint["cycle"] != expected_cycle:
            raise AcceptanceError(
                "three-update-cycles checkpoint order is invalid: "
                f"expected cycle {expected_cycle}, "
                f"got {checkpoint['cycle']!r}"
            )
        _validate_expected_version(
            checkpoint["candidate_version"],
            expected_version,
            f"cycle {expected_cycle} candidate",
        )
        pre = checkpoint["pre_snapshot"]
        post = checkpoint["snapshot"]
        _validate_expected_version(
            pre["candidate_version"],
            expected_version,
            f"cycle {expected_cycle} pre-state",
        )
        _validate_verified_snapshot(
            session, post, expected_version=expected_version
        )
        generation = checkpoint["stage_generation"]
        if generations and generation <= generations[-1]:
            raise AcceptanceError(
                "three-update-cycles checkpoint generations do not "
                "strictly advance"
            )
        if checkpoints:
            _validate_adjacent_cycle_boot_lineage(
                checkpoints[-1],
                checkpoint,
            )
        generations.append(generation)
        checkpoints.append(checkpoint)

    gate_one = _pass_facts_from_serialized(
        "android-control-reconnect",
        pass_records["android-control-reconnect"][0]["facts"],
    )
    _validate_pass_facts(
        session,
        "android-control-reconnect",
        gate_one,
    )
    gate_one_pre = gate_one["snapshot"]
    gate_one_artifacts = gate_one["candidate_artifacts"]
    _validate_expected_version(
        gate_one_pre["candidate_version"],
        expected_version,
        "Gate 1 candidate",
    )
    cycle_one_pre = checkpoints[0]["pre_snapshot"]
    for checkpoint in checkpoints:
        if dict(checkpoint["candidate_artifacts"]) != \
                dict(gate_one_artifacts):
            raise AcceptanceError(
                "update checkpoint candidate artifacts changed from Gate 1"
            )
    for slot in ("ble", "wifi"):
        if cycle_one_pre[f"{slot}_version"] != \
                gate_one_pre[f"{slot}_version"]:
            raise AcceptanceError(
                f"cycle 1 {slot} scanner version changed from the "
                "anchored Gate 1 pre-update state"
            )

    aggregate = _aggregate_from_serialized(
        pass_records["three-update-cycles"][0]["facts"]
    )
    _validate_pass_facts(session, "three-update-cycles", aggregate)
    _validate_expected_version(
        aggregate["candidate_version"],
        expected_version,
        "three-update-cycles aggregate candidate",
    )
    _validate_verified_snapshot(
        session, aggregate["snapshot"], expected_version=expected_version
    )
    if aggregate["checkpoint_generations"] != generations:
        raise AcceptanceError(
            "three-update-cycles aggregate generations do not match "
            "its checkpoints"
        )
    if dict(aggregate["candidate_artifacts"]) != \
            dict(gate_one_artifacts):
        raise AcceptanceError(
            "three-update-cycles candidate artifacts changed from Gate 1"
        )
    if dict(aggregate["snapshot"]) != dict(checkpoints[-1]["snapshot"]):
        raise AcceptanceError(
            "three-update-cycles aggregate snapshot does not match cycle 3"
        )
    if aggregate["first_cycle_manual_relay_commands"] != \
            checkpoints[0]["manual_relay_commands"] or \
            aggregate["recovery_manual_relay_commands"] != sum(
                checkpoint["manual_relay_commands"]
                for checkpoint in checkpoints[1:]
            ):
        raise AcceptanceError(
            "three-update-cycles aggregate relay totals do not match "
            "its checkpoints"
        )

    for gate in REQUIRED_GATES:
        if gate == "three-update-cycles":
            continue
        facts = _pass_facts_from_serialized(
            gate, pass_records[gate][0]["facts"]
        )
        _validate_pass_facts(session, gate, facts)
        if gate in ("interrupted-upload", "chord-rom-recovery") and \
                dict(facts["candidate_artifacts"]) != \
                dict(gate_one_artifacts):
            raise AcceptanceError(
                f"{gate} candidate artifacts changed from Gate 1"
            )
        if gate == "android-control-reconnect":
            _validate_cycle_pre_snapshot(
                session,
                facts["snapshot"],
                expected_version,
                1,
            )
        else:
            _validate_verified_snapshot(
                session,
                facts["snapshot"],
                expected_version=expected_version,
            )
        if gate == "interrupted-upload":
            _require_interrupted_baseline_matches_cycle_three(
                facts,
                checkpoints[-1]["snapshot"],
            )
            for key in ("baseline_snapshot", "recovered_snapshot"):
                _validate_verified_snapshot(
                    session,
                    facts[key],
                    expected_version=expected_version,
                )

    return {
        "session_id": session.session_id,
        "version": expected_version,
        "passed_gates": list(REQUIRED_GATES),
        "update_cycles": 3,
        "checkpoint_generations": generations,
        "records_verified": len(selected),
    }


def verify_acceptance_evidence(
    evidence_path: Path,
    session: BadgeAcceptanceSession | None,
    expected_version: str,
    *,
    session_id: str | None = None,
) -> dict[str, object]:
    """Fail closed unless the exact final nine-record session is complete."""
    if session is not None and type(session) is not BadgeAcceptanceSession:
        raise AcceptanceError("acceptance session is malformed")
    if session_id is not None and (
        not isinstance(session_id, str)
        or not _SESSION_ID_RE.fullmatch(session_id)
    ):
        raise AcceptanceError("acceptance session ID is malformed")
    if session is not None and session_id is not None and \
            session.session_id != session_id:
        raise AcceptanceError(
            "selected session ID does not match the supplied session"
        )
    if not isinstance(expected_version, str) or \
            flash.firmware_version_relation(
                expected_version, expected_version
            ) != "equal":
        raise AcceptanceError("expected final version is invalid")
    path = Path(evidence_path)
    fd = _open_existing_evidence_readonly(path)
    try:
        fcntl.flock(fd, fcntl.LOCK_SH)
        _validate_open_evidence_file(fd, path)
        _require_no_mutating_gate_reservation(path)
        records = _read_locked_evidence(fd)
        bindings = _session_bindings_from_records(records)
        if session is None:
            if session_id is not None:
                session = bindings.get(session_id)
                if session is None:
                    raise AcceptanceError(
                        f"session {session_id!r} is not present in evidence"
                    )
            elif len(bindings) == 1:
                session = next(iter(bindings.values()))
            else:
                raise AcceptanceError(
                    "completion audit requires --session-id when evidence "
                    "contains zero or multiple sessions"
                )
        result = _verify_acceptance_records(
            records, session, expected_version
        )
        _validate_open_evidence_file(fd, path)
        _require_no_mutating_gate_reservation(path)
        _validate_open_evidence_file(fd, path)
        return result
    finally:
        try:
            fcntl.flock(fd, fcntl.LOCK_UN)
        except OSError:
            pass
        os.close(fd)


def _scanner_store_fingerprint(status: dict[str, Any]) -> dict[str, Any]:
    store = status.get("firmware_store")
    if not isinstance(store, dict):
        raise AcceptanceError("FOF_STATUS is missing firmware_store")
    stored = store.get("stored")
    if stored is not True:
        raise AcceptanceError(
            "interrupted-upload gate requires a stored scanner manifest"
        )
    generation = store.get("generation", 0)
    _exact_nonnegative_int(generation, "firmware_store.generation")
    if generation <= 0:
        raise AcceptanceError("firmware_store.generation must be positive")
    sha256 = store.get("sha256", "")
    if not isinstance(sha256, str) or not _SHA256_RE.fullmatch(sha256):
        raise AcceptanceError("firmware_store.sha256 is malformed")
    return {
        "stored": True,
        "generation": generation,
        "sha256": sha256,
    }


def interrupt_uplink_upload(
    badge: flash.BadgeSerial,
    platform: dict[str, Any],
    artifacts: flash.FrozenArtifactSet,
    version: str,
    *,
    running_partition: str,
    abort_after: int = INTERRUPTED_UPLOAD_BYTES,
    update_session: str | None = None,
) -> int:
    """Write and receipt exactly the fault boundary, then leave via close."""
    if platform is not flash.PLATFORMS[CANARY_PLATFORM_KEY]:
        raise AcceptanceError(
            "interrupted upload requires the fixed canary platform identity"
        )
    if version != flash.UPDATE_MAINTENANCE_MIN_VERSION:
        raise AcceptanceError(
            "interrupted upload requires the exact .79 canary version"
        )
    if type(abort_after) is not int or \
            abort_after != INTERRUPTED_UPLOAD_BYTES:
        raise AcceptanceError(
            f"interrupted upload must stop at exactly "
            f"{INTERRUPTED_UPLOAD_BYTES} bytes"
        )
    if running_partition not in ("ota_0", "ota_1"):
        raise AcceptanceError("running OTA partition is invalid")
    if flash._uses_update_maintenance(version):
        try:
            bound_update_session = flash._validated_update_session(
                update_session
            )
        except flash.FlashError as exc:
            raise AcceptanceError(
                "maintenance interrupted upload requires an exact session"
            ) from exc
    else:
        if update_session is not None:
            raise AcceptanceError(
                "legacy interrupted upload cannot carry a maintenance session"
            )
        bound_update_session = None
    expected_partition = (
        "ota_1" if running_partition == "ota_0" else "ota_0"
    )
    data = flash._validated_frozen_firmware_bytes(
        artifacts,
        role="uplink",
        target=platform["uplink_name"],
        project=platform["uplink_project"],
        hardware=platform["hardware_type"],
        version=version,
    )
    if len(data) <= abort_after:
        raise AcceptanceError(
            "uplink artifact is too small for deterministic interruption"
        )
    manifest = {
        "cmd": "uplink_ota_begin",
        "target": platform["uplink_name"],
        "project": platform["uplink_project"],
        "hardware_type": platform["hardware_type"],
        "version": version,
        "size": len(data),
        "crc32": binascii.crc32(data) & 0xFFFFFFFF,
        "sha256": hashlib.sha256(data).hexdigest(),
        "flow_control": "credit-v1",
        "recovery_rewrite_same_version": True,
    }
    if bound_update_session is not None:
        manifest["session"] = bound_update_session
    deadline = (
        time.monotonic() + INTERRUPTED_UPLOAD_RECEIPT_TIMEOUT_S
    )
    badge.write_line(
        "FOF_CTL:" + json.dumps(manifest, separators=(",", ":"))
    )

    def read_receipt_before_deadline() -> dict[str, Any]:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise AcceptanceError(
                "interrupted upload receipt deadline expired"
            )
        receipt = badge.read_prefixed_json(
            "FOF_UPLINK_OTA:",
            remaining,
            allowed_schema_ids=(
                flash.HostJsonSchemaId.UPLINK_OTA,
            ),
        )
        if time.monotonic() > deadline:
            raise AcceptanceError(
                "interrupted upload receipt exceeded its deadline"
            )
        return receipt

    first_credit = min(flash.UPLINK_OTA_CREDIT_BYTES, len(data))
    ready = read_receipt_before_deadline()
    flash.validate_uplink_ota_receipt(
        ready,
        phase="ready",
        partition=expected_partition,
        received=0,
        total=len(data),
        credit_bytes=first_credit,
        reboot_required=False,
    )

    sent = 0
    credit = first_credit
    while sent < abort_after:
        window_end = sent + credit
        if credit <= 0 or window_end > abort_after:
            raise AcceptanceError(
                "device credit does not align with interruption boundary"
            )
        while sent < window_end:
            chunk_end = min(
                sent + flash.UPLINK_OTA_WRITE_BYTES, window_end
            )
            chunk = data[sent:chunk_end]
            wrote = badge.ser.write(chunk)
            if type(wrote) is not int or wrote != len(chunk):
                raise AcceptanceError(
                    f"uplink interruption short write at {sent}: "
                    f"got {wrote!r}, wanted {len(chunk)}"
                )
            sent = chunk_end
        next_credit = min(
            flash.UPLINK_OTA_CREDIT_BYTES, len(data) - sent
        )
        receipt = read_receipt_before_deadline()
        flash.validate_uplink_ota_receipt(
            receipt,
            phase="credit",
            partition=expected_partition,
            received=sent,
            total=len(data),
            credit_bytes=next_credit,
            reboot_required=False,
        )
        credit = next_credit
    return sent


def _new_session(status: dict[str, Any], session_id: str) -> \
        BadgeAcceptanceSession:
    try:
        uplink = flash.validate_uplink_application_status(status)
        scanner_ids = flash.capture_scanner_hardware_ids(
            status,
            flash.PLATFORMS[CANARY_PLATFORM_KEY],
            ["ble", "wifi"],
            require_connected=True,
        )
        return BadgeAcceptanceSession(
            session_id=session_id,
            uplink_hardware_id=uplink,
            ble_hardware_id=scanner_ids["ble"],
            wifi_hardware_id=scanner_ids["wifi"],
        )
    except Exception as exc:
        if isinstance(exc, (AcceptanceError, ValueError)):
            raise
        raise AcceptanceError(
            f"cannot establish immutable three-board session: {exc}"
        ) from exc


def _trusted_session_uplink_descriptor(
    port: str,
    session: BadgeAcceptanceSession,
) -> flash.UsbDescriptorRecord:
    """Resolve a live path only through the anchored uplink identity."""
    if not isinstance(port, str) or not port:
        raise AcceptanceError("live badge port is required")
    if type(session) is not BadgeAcceptanceSession:
        raise AcceptanceError("badge acceptance session is malformed")
    try:
        descriptor, binding = flash.select_trusted_uplink_descriptor(
            selected_port=port,
            operator_acknowledged=False,
            trusted_binding=flash.TrustedUplinkBinding(
                serial_number=session.uplink_hardware_id,
                location=None,
                source="retained-session",
            ),
        )
    except flash.FlashError as exc:
        raise AcceptanceError(
            "live badge does not match the anchored uplink identity"
        ) from exc
    if descriptor.serial_number != session.uplink_hardware_id:
        raise AcceptanceError(
            "live badge descriptor does not match the anchored uplink"
        )
    if binding.serial_number != descriptor.serial_number or \
            binding.location != descriptor.location:
        raise AcceptanceError(
            "live badge binding dropped its exact USB location"
        )
    return descriptor


_UNBOUND_USB_LOCATION = object()


def _require_session_uplink_descriptor(
    descriptor: object,
    session: BadgeAcceptanceSession,
    *,
    stage: str,
    expected_location: object = _UNBOUND_USB_LOCATION,
) -> flash.UsbDescriptorRecord:
    """Fail closed on anything except one exact anchored descriptor value."""
    if type(descriptor) is not flash.UsbDescriptorRecord:
        raise AcceptanceError(
            f"{stage} uplink descriptor has an invalid type"
        )
    if (
        type(descriptor.device) is not str
        or not descriptor.device.startswith("/dev/")
        or descriptor.vid != flash.ESPRESSIF_USB_SERIAL_JTAG_VID
        or descriptor.pid != flash.ESPRESSIF_USB_SERIAL_JTAG_PID
        or descriptor.serial_number != session.uplink_hardware_id
        or not (
            descriptor.location is None
            or (
                type(descriptor.location) is str
                and bool(descriptor.location)
                and descriptor.location == descriptor.location.strip()
                and not any(
                    ord(character) < 0x20 or ord(character) == 0x7F
                    for character in descriptor.location
                )
            )
        )
        or any(
            type(value) is not int or value < 0
            for value in (
                descriptor.stat_device,
                descriptor.stat_inode,
                descriptor.stat_rdev,
            )
        )
    ):
        raise AcceptanceError(
            f"{stage} uplink descriptor does not match the anchored session"
        )
    if expected_location is not _UNBOUND_USB_LOCATION and \
            descriptor.location != expected_location:
        raise AcceptanceError(
            f"{stage} uplink descriptor moved outside the initial USB location"
        )
    return descriptor


def _prepare_interrupted_upload_maintenance(
    badge: flash.BadgeSerial,
    update_session: str,
    *,
    deadline: float,
    source_version: str,
) -> dict[str, Any]:
    """Prepare only from the exact source proven to support maintenance."""
    if source_version != flash.UPDATE_MAINTENANCE_MIN_VERSION:
        raise AcceptanceError(
            "interrupted update maintenance requires the exact .79 canary "
            "source"
        )
    return badge.prepare_update_maintenance(
        update_session,
        deadline=deadline,
        source_supports_update_maintenance=True,
    )


def _run_interrupted_upload_gate_in_update_maintenance(
    *,
    initial_descriptor: flash.UsbDescriptorRecord,
    expected_session: BadgeAcceptanceSession,
    platform: dict[str, Any],
    artifacts: flash.FrozenArtifactSet,
    version: str,
    candidate_artifacts: VerifiedCandidateArtifacts,
    updater_baseline: VerifiedV078UpdaterBaseline,
    abort_after: int,
    wait_seconds: int,
    sleep: Callable[[float], None],
) -> tuple[BadgeAcceptanceSession, dict[str, object]]:
    """Exercise an interruption and retry inside one exact update session."""
    data = flash._frozen_firmware_bytes(artifacts, role="uplink")
    sha256 = hashlib.sha256(data).hexdigest()
    update_session = flash._new_update_session()
    deadline = time.monotonic() + flash.UPDATE_TRANSFER_TIMEOUT_S

    with flash.BadgeSerial(
        initial_descriptor,
        False,
        expected_hardware_id=expected_session.uplink_hardware_id,
    ) as badge:
        baseline = badge.status(timeout_s=5)
        observed_session = _new_session(
            baseline, expected_session.session_id
        )
        if observed_session != expected_session:
            raise AcceptanceError(
                "attached board binding does not match the anchored session"
            )
        session = expected_session
        baseline_snapshot = verify_badge_snapshot(
            baseline, session, version
        )
        live_sample = verify_canary_normal_live_metrics(
            baseline,
            session,
            version,
            baseline=updater_baseline,
        )
        baseline_store = _scanner_store_fingerprint(baseline)
        baseline_partition = str(baseline["running_partition"])
        persisted_game_state = flash._capture_persisted_game_state(
            baseline
        )
        expected_partition = (
            "ota_1" if baseline_partition == "ota_0" else "ota_0"
        )
        reconcile_expectation = {
            "session": update_session,
            "version": version,
            "sha256": sha256,
            "size": len(data),
            "partition": expected_partition,
        }

        def enter_maintenance() -> dict[str, Any]:
            nonlocal live_sample
            preparation = _prepare_interrupted_upload_maintenance(
                badge,
                update_session,
                deadline=deadline,
                source_version=str(baseline["version"]),
            )
            if preparation["phase"] == "rebooting":
                status = badge.reconnect_same_uplink(deadline=deadline)
            else:
                status = badge.status(timeout_s=5)
            maintenance = flash._validate_update_maintenance_status(
                status,
                session=update_session,
                expected_hardware_id=session.uplink_hardware_id,
            )
            live_sample = verify_canary_maintenance_live_metrics(
                maintenance,
                session,
                version,
                update_session,
                baseline=updater_baseline,
                previous=live_sample,
            )
            return maintenance

        def validate_transfer_reconnect(
            status: dict[str, Any],
            reconnect_session: str,
        ) -> None:
            nonlocal live_sample
            live_sample = verify_canary_maintenance_live_metrics(
                status,
                session,
                version,
                reconnect_session,
                baseline=updater_baseline,
                previous=live_sample,
            )

        try:
            enter_maintenance()
            written = interrupt_uplink_upload(
                badge,
                platform,
                artifacts,
                version,
                running_partition=baseline_partition,
                abort_after=abort_after,
                update_session=update_session,
            )
            if written != INTERRUPTED_UPLOAD_BYTES:
                raise AcceptanceError(
                    "interrupted upload wrote the wrong byte count"
                )

            badge._binary_transport_loss_at = time.monotonic()
            badge._close_serial()
            sleep(wait_seconds)
            interrupted_status = badge.reconnect_same_uplink(
                deadline=deadline
            )
            interrupted_status = flash._validate_update_maintenance_status(
                interrupted_status,
                session=update_session,
                expected_hardware_id=session.uplink_hardware_id,
            )
            live_sample = verify_canary_maintenance_live_metrics(
                interrupted_status,
                session,
                version,
                update_session,
                baseline=updater_baseline,
                previous=live_sample,
            )
            if interrupted_status.get("running_partition") != \
                    baseline_partition:
                raise AcceptanceError(
                    "interrupted upload changed the running partition"
                )
            disposition = badge.reconcile_uplink_ota(
                reconcile_expectation
            )
            if disposition != "restart_from_zero":
                raise AcceptanceError(
                    "partial upload was not reconciled to byte zero"
                )

            badge.abort_update_maintenance(deadline=deadline)
            recovered = badge.reconnect_same_uplink_normal(
                deadline=deadline
            )
            flash._verify_persisted_game_state(
                recovered, persisted_game_state
            )
            recovered_snapshot = verify_badge_snapshot(
                recovered, session, version
            )
            live_sample = verify_canary_normal_live_metrics(
                recovered,
                session,
                version,
                baseline=updater_baseline,
                previous=live_sample,
            )
            if recovered.get("running_partition") != baseline_partition:
                raise AcceptanceError(
                    "interrupted upload changed the running partition"
                )
            recovered_store = _scanner_store_fingerprint(recovered)
            if recovered_store != baseline_store:
                raise AcceptanceError(
                    "interrupted upload changed the scanner firmware cache"
                )

            enter_maintenance()
            receipt = badge.upload_uplink_firmware(
                platform,
                artifacts,
                version,
                recovery_rewrite_same_version=True,
                maintenance_status_validator=validate_transfer_reconnect,
            )
            expectation = flash._classify_uplink_update_receipt(
                receipt,
                pre_status=recovered,
                target_version=version,
                expected_sha256=sha256,
                expected_size=len(data),
                update_session=update_session,
            )
            if expectation.mutation_expected is not True or \
                    expectation.expected_partition != expected_partition:
                raise AcceptanceError(
                    "retry did not authorize the alternate OTA partition"
                )
            committed_status = badge.reconnect_same_uplink(
                deadline=deadline
            )
            committed_status = flash._validate_update_maintenance_status(
                committed_status,
                session=update_session,
                expected_hardware_id=session.uplink_hardware_id,
            )
            live_sample = verify_canary_maintenance_live_metrics(
                committed_status,
                session,
                version,
                update_session,
                baseline=updater_baseline,
                previous=live_sample,
            )
            target_status = flash._wait_for_maintenance_uplink_target(
                badge,
                session=update_session,
                expected_version=expectation.expected_version,
                expected_partition=expectation.expected_partition,
                deadline=deadline,
            )
            live_sample = verify_canary_maintenance_live_metrics(
                target_status,
                session,
                version,
                update_session,
                baseline=updater_baseline,
                previous=live_sample,
            )
            final_status, _evidence = \
                flash._finalize_update_maintenance(
                    badge,
                    session=update_session,
                    expectation=expectation,
                    persisted_game_state=persisted_game_state,
                    deadline=deadline,
                )
            live_sample = verify_canary_normal_live_metrics(
                final_status,
                session,
                version,
                baseline=updater_baseline,
                previous=live_sample,
            )
        except BaseException as primary:
            if getattr(badge, "_update_session", None) == update_session:
                flash._recover_failed_update_maintenance(
                    badge,
                    session=update_session,
                    persisted_game_state=persisted_game_state,
                    primary=primary,
                )
            raise

    final_snapshot = verify_badge_snapshot(
        final_status, session, version
    )
    final_store = _scanner_store_fingerprint(final_status)
    if final_store != baseline_store:
        raise AcceptanceError(
            "successful uplink retry changed the scanner firmware cache"
        )
    facts: dict[str, object] = {
        "snapshot": final_snapshot,
        "candidate_artifacts": candidate_artifacts,
        "baseline_snapshot": baseline_snapshot,
        "recovered_snapshot": recovered_snapshot,
        "abort_after": INTERRUPTED_UPLOAD_BYTES,
        "idle_wait_s": wait_seconds,
        "prior_partition_bootable": True,
        "scanner_cache_unchanged": True,
        "parser_returned_to_command": True,
        "retry_succeeded": True,
        "scanner_cache_before": baseline_store,
        "scanner_cache_after_abort": recovered_store,
        "scanner_cache_after_retry": final_store,
    }
    return session, facts


def run_interrupted_upload_gate(
    port: str,
    *,
    expected_session: BadgeAcceptanceSession,
    abort_after: int = INTERRUPTED_UPLOAD_BYTES,
    wait_seconds: int = INTERRUPTED_UPLOAD_IDLE_WAIT_S,
    sleep: Callable[[float], None] = time.sleep,
    expected_version: str | None = None,
    frozen_artifacts: flash.FrozenUsbFirmwareArtifacts | None = None,
    candidate_artifacts: VerifiedCandidateArtifacts | None = None,
    updater_baseline: VerifiedV078UpdaterBaseline | None = None,
) -> tuple[BadgeAcceptanceSession, dict[str, object]]:
    """Exercise idle-abort safety, reconnect, and one successful retry."""
    if type(expected_session) is not BadgeAcceptanceSession:
        raise AcceptanceError(
            "interrupted upload requires an anchored three-board session"
        )
    if wait_seconds < INTERRUPTED_UPLOAD_IDLE_WAIT_S:
        raise AcceptanceError(
            "interrupted upload must wait at least seven seconds"
        )
    version = (
        flash.repo_version(
            flash.PLATFORMS[CANARY_PLATFORM_KEY]
        )
        if expected_version is None
        else expected_version
    )
    platform = flash.PLATFORMS[CANARY_PLATFORM_KEY]
    bound_updater_baseline: VerifiedV078UpdaterBaseline | None = None
    if flash._uses_update_maintenance(version):
        bound_updater_baseline = _validate_v078_baseline_binding(
            updater_baseline,
            expected_session,
        )
    if frozen_artifacts is None:
        frozen_artifacts = flash._prepare_frozen_usb_firmware_artifacts(
            platform,
            True,
            ["ble", "wifi"],
        )
    derived_artifacts = verify_candidate_artifacts(
        frozen_artifacts,
        version,
    )
    if candidate_artifacts is None:
        candidate_artifacts = derived_artifacts
    else:
        _validate_candidate_artifacts(candidate_artifacts, version)
        if dict(candidate_artifacts) != dict(derived_artifacts):
            raise AcceptanceError(
                "interrupted upload artifacts changed from the anchored "
                "candidate"
            )
    artifacts = frozen_artifacts.uplink
    if type(artifacts) is not flash.FrozenArtifactSet:
        raise AcceptanceError(
            "interrupted upload has no frozen uplink artifact set"
        )
    flash._validated_frozen_firmware_bytes(
        artifacts,
        role="uplink",
        target=platform["uplink_name"],
        project=platform["uplink_project"],
        hardware=platform["hardware_type"],
        version=version,
    )
    initial_descriptor = _require_session_uplink_descriptor(
        _trusted_session_uplink_descriptor(port, expected_session),
        expected_session,
        stage="initial",
    )
    if flash._uses_update_maintenance(version):
        if bound_updater_baseline is None:
            raise AcceptanceError(
                "maintenance interrupted upload lacks its .78 baseline"
            )
        return _run_interrupted_upload_gate_in_update_maintenance(
            initial_descriptor=initial_descriptor,
            expected_session=expected_session,
            platform=platform,
            artifacts=artifacts,
            version=version,
            candidate_artifacts=candidate_artifacts,
            updater_baseline=bound_updater_baseline,
            abort_after=abort_after,
            wait_seconds=wait_seconds,
            sleep=sleep,
        )

    with flash.BadgeSerial(
        initial_descriptor,
        False,
        expected_hardware_id=expected_session.uplink_hardware_id,
    ) as badge:
        baseline = badge.status(timeout_s=5)
        observed_session = _new_session(
            baseline, expected_session.session_id
        )
        if observed_session != expected_session:
            raise AcceptanceError(
                "attached board binding does not match the anchored session"
            )
        session = expected_session
        baseline_snapshot = verify_badge_snapshot(
            baseline, session, version
        )
        baseline_store = _scanner_store_fingerprint(baseline)
        baseline_partition = str(baseline["running_partition"])
        written = interrupt_uplink_upload(
            badge,
            platform,
            artifacts,
            version,
            running_partition=baseline_partition,
            abort_after=abort_after,
        )
    if written != INTERRUPTED_UPLOAD_BYTES:
        raise AcceptanceError("interrupted upload wrote the wrong byte count")

    sleep(wait_seconds)
    rebound_descriptor, recovered = flash.wait_for_application_port(
        session.uplink_hardware_id, timeout_s=30
    )
    rebound_descriptor = _require_session_uplink_descriptor(
        rebound_descriptor,
        session,
        stage="rebound",
        expected_location=initial_descriptor.location,
    )
    recovered_snapshot = verify_badge_snapshot(
        recovered, session, version
    )
    if recovered.get("running_partition") != baseline_partition:
        raise AcceptanceError(
            "interrupted upload changed the running partition"
        )
    recovered_store = _scanner_store_fingerprint(recovered)
    if recovered_store != baseline_store:
        raise AcceptanceError(
            "interrupted upload changed the scanner firmware cache"
        )

    with flash.BadgeSerial(
        rebound_descriptor,
        False,
        expected_hardware_id=session.uplink_hardware_id,
    ) as badge:
        receipt = badge.upload_uplink_firmware(
            platform,
            artifacts,
            version,
            recovery_rewrite_same_version=True,
        )
    try:
        expected_uplink_bytes = flash._frozen_firmware_bytes(
            artifacts, role="uplink"
        )
        expectation = flash._classify_uplink_update_receipt(
            receipt,
            pre_status=recovered,
            target_version=version,
            expected_sha256=hashlib.sha256(
                expected_uplink_bytes
            ).hexdigest(),
            expected_size=len(expected_uplink_bytes),
            update_session=flash._new_update_session(),
        )
        final_descriptor, _evidence = \
            flash.wait_for_post_uplink_application(
            expectation,
            timeout_s=flash.POST_UPLINK_APPLICATION_TIMEOUT_S,
        )
    except Exception as exc:
        raise AcceptanceError(
            f"retry after interrupted upload failed: {exc}"
        ) from exc
    final_descriptor = _require_session_uplink_descriptor(
        final_descriptor,
        session,
        stage="final",
        expected_location=initial_descriptor.location,
    )
    with flash.BadgeSerial(
        final_descriptor,
        False,
        expected_hardware_id=session.uplink_hardware_id,
    ) as badge:
        final_status = badge.status(timeout_s=5)
    final_snapshot = verify_badge_snapshot(
        final_status, session, version
    )
    final_store = _scanner_store_fingerprint(final_status)
    if final_store != baseline_store:
        raise AcceptanceError(
            "successful uplink retry changed the scanner firmware cache"
        )
    facts: dict[str, object] = {
        "snapshot": final_snapshot,
        "candidate_artifacts": candidate_artifacts,
        "baseline_snapshot": baseline_snapshot,
        "recovered_snapshot": recovered_snapshot,
        "abort_after": INTERRUPTED_UPLOAD_BYTES,
        "idle_wait_s": wait_seconds,
        "prior_partition_bootable": True,
        "scanner_cache_unchanged": True,
        "parser_returned_to_command": True,
        "retry_succeeded": True,
        "scanner_cache_before": baseline_store,
        "scanner_cache_after_abort": recovered_store,
        "scanner_cache_after_retry": final_store,
    }
    return session, facts


def _load_json_object(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=_json_object_without_duplicate_keys,
        )
    except (OSError, json.JSONDecodeError) as exc:
        raise AcceptanceError(f"cannot read {label}: {exc}") from exc
    if not isinstance(value, dict):
        raise AcceptanceError(f"{label} must contain one JSON object")
    return value


def _session_from_json(value: dict[str, Any]) -> BadgeAcceptanceSession:
    expected = {
        "session_id",
        "uplink_hardware_id",
        "ble_hardware_id",
        "wifi_hardware_id",
    }
    if set(value) != expected:
        raise AcceptanceError(
            "session JSON must contain exactly: " +
            ", ".join(sorted(expected))
        )
    try:
        return BadgeAcceptanceSession(**value)
    except (TypeError, ValueError) as exc:
        raise AcceptanceError(f"session JSON is invalid: {exc}") from exc


def _session_from_private_input(
    session_input: _PrivateSessionInput,
) -> BadgeAcceptanceSession:
    _validate_private_session_input(session_input)
    try:
        value = json.loads(
            session_input.encoded.decode("utf-8"),
            object_pairs_hook=_json_object_without_duplicate_keys,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise AcceptanceError(
            f"cannot read private mutating session file: {exc}"
        ) from exc
    if not isinstance(value, dict):
        raise AcceptanceError(
            "mutating session file must contain one JSON object"
        )
    return _session_from_json(value)


def _open_mutating_cli_context(
    args: argparse.Namespace,
) -> tuple[BadgeAcceptanceSession, _PrivateSessionInput]:
    if args.session_file is None:
        raise AcceptanceError(
            "mutating gates require an explicit private --session-file"
        )
    session_input = _open_private_session_input(args.session_file)
    keep_open = False
    try:
        session = _session_from_private_input(session_input)
        if args.session_id is not None and \
                args.session_id != session.session_id:
            raise AcceptanceError(
                "--session-id does not match --session-file"
            )
        anchored = load_anchored_session(
            args.evidence, session.session_id
        )
        if anchored != session:
            raise AcceptanceError(
                "session file hardware binding changed from evidence"
            )
        _validate_private_session_input(session_input)
        keep_open = True
        return session, session_input
    finally:
        if not keep_open:
            _close_private_session_input(session_input)


def _anchored_cli_session(args: argparse.Namespace) -> \
        BadgeAcceptanceSession:
    if args.session_file is None:
        return load_anchored_session(args.evidence, args.session_id)
    session = _session_from_json(
        _load_json_object(args.session_file, "session file")
    )
    if args.session_id is not None and args.session_id != session.session_id:
        raise AcceptanceError(
            "--session-id does not match --session-file"
        )
    anchored = load_anchored_session(args.evidence, session.session_id)
    if anchored != session:
        raise AcceptanceError(
            "session file hardware binding changed from evidence"
        )
    return session


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--gate", choices=REQUIRED_GATES)
    mode.add_argument(
        "--verify-complete",
        action="store_true",
        help="read-only audit of the exact six-gate final evidence",
    )
    parser.add_argument(
        "--run-chord-rom-recovery",
        action="store_true",
        help=(
            "machine-run Gate 4 from an explicitly selected current uplink "
            "ROM port using the private anchored session; the application "
            "and ROM may retain the same device path"
        ),
    )
    parser.add_argument("--port")
    parser.add_argument(
        "--abort-after", type=int, default=INTERRUPTED_UPLOAD_BYTES
    )
    parser.add_argument(
        "--cycle", type=int, choices=(1, 2, 3),
        help="machine-run checkpoint number for three-update-cycles",
    )
    parser.add_argument("--evidence", type=Path, default=DEFAULT_EVIDENCE)
    parser.add_argument("--session-id")
    parser.add_argument(
        "--session-file", type=Path,
        help=(
            "JSON session; mutating gates require a private regular file "
            "in an owner-controlled directory"
        ),
    )
    parser.add_argument(
        "--facts-file", type=Path,
        help="privacy-safe JSON facts for manually observed gates",
    )
    parser.add_argument(
        "--expected-version", default=None,
        help="exact application version required by a manual PASS",
    )
    parser.add_argument(
        "--status", choices=("PASS", "FAIL"), default="PASS"
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        if args.verify_complete:
            if args.port is not None or args.facts_file is not None or \
                    args.cycle is not None or args.status != "PASS" or \
                    args.run_chord_rom_recovery:
                raise AcceptanceError(
                    "--verify-complete is read-only and does not accept "
                    "--port, --facts-file, --cycle, a machine-run chord, "
                    "or non-PASS status"
                )
            session: BadgeAcceptanceSession | None = None
            if args.session_file is not None:
                session = _session_from_json(
                    _load_json_object(args.session_file, "session file")
                )
                if args.session_id is not None and \
                        args.session_id != session.session_id:
                    raise AcceptanceError(
                        "--session-id does not match --session-file"
                    )
            expected_version = (
                args.expected_version
                if args.expected_version is not None
                else flash.repo_version(
                    flash.PLATFORMS[CANARY_PLATFORM_KEY]
                )
            )
            result = verify_acceptance_evidence(
                args.evidence,
                session,
                expected_version,
                session_id=args.session_id,
            )
            print(json.dumps({
                "ok": True,
                "evidence": str(args.evidence),
                **result,
            }, separators=(",", ":"), sort_keys=True))
            return 0
        if args.cycle is not None and args.gate != "three-update-cycles":
            raise AcceptanceError(
                "--cycle is valid only for three-update-cycles"
            )
        if args.run_chord_rom_recovery and (
            args.gate != "chord-rom-recovery"
            or args.status != "PASS"
        ):
            raise AcceptanceError(
                "--run-chord-rom-recovery requires "
                "--gate chord-rom-recovery with PASS status"
            )
        if args.gate == "chord-rom-recovery" and \
                args.status == "PASS" and \
                not args.run_chord_rom_recovery:
            raise AcceptanceError(
                "chord-rom-recovery PASS requires "
                "--run-chord-rom-recovery"
            )
        if args.run_chord_rom_recovery:
            if not args.port:
                raise AcceptanceError(
                    "machine-run chord ROM recovery requires --port"
                )
            if args.facts_file is not None:
                raise AcceptanceError(
                    "machine-run chord ROM recovery derives its own facts"
                )
            if args.expected_version is not None:
                raise AcceptanceError(
                    "machine-run chord ROM recovery uses the exact repo "
                    "artifact version and does not accept "
                    "--expected-version"
                )
            session, session_input = _open_mutating_cli_context(args)
            try:
                run_reserved_chord_rom_recovery_gate(
                    args.evidence,
                    args.port,
                    session,
                    session_input,
                )
            finally:
                _close_private_session_input(session_input)
        elif args.gate == "interrupted-upload":
            if args.status != "PASS":
                raise AcceptanceError(
                    "interrupted-upload does not accept --status FAIL"
                )
            if not args.port:
                raise AcceptanceError(
                    "--port is required for interrupted-upload"
                )
            session, session_input = _open_mutating_cli_context(args)
            try:
                session, _facts = run_reserved_interrupted_upload_gate(
                    args.evidence,
                    args.port,
                    session,
                    session_input,
                    abort_after=args.abort_after,
                )
            finally:
                _close_private_session_input(session_input)
        elif args.gate == "three-update-cycles" and \
                args.status == "PASS":
            if args.cycle is None or not args.port:
                raise AcceptanceError(
                    "three-update-cycles PASS requires --cycle and --port"
                )
            if args.facts_file is not None:
                raise AcceptanceError(
                    "three-update-cycles PASS does not accept caller facts"
                )
            session, session_input = _open_mutating_cli_context(args)
            try:
                run_reserved_update_cycle_checkpoint(
                    args.evidence,
                    args.port,
                    session,
                    session_input,
                    args.cycle,
                )
            finally:
                _close_private_session_input(session_input)
        else:
            if args.session_file is None or args.facts_file is None:
                raise AcceptanceError(
                    "manual gates require --session-file and --facts-file"
                )
            session = _session_from_json(
                _load_json_object(args.session_file, "session file")
            )
            facts = _load_json_object(args.facts_file, "facts file")
            if args.status == "PASS":
                if not args.port:
                    raise AcceptanceError(
                        "manual PASS requires a live badge --port"
                    )
                if "snapshot" in facts:
                    raise AcceptanceError(
                        "manual PASS facts must not supply a snapshot"
                    )
                expected_version = (
                    args.expected_version
                    if args.expected_version is not None
                    else flash.repo_version(
                        flash.PLATFORMS[CANARY_PLATFORM_KEY]
                    )
                )
                if args.gate == "android-control-reconnect":
                    if "candidate_artifacts" in facts:
                        raise AcceptanceError(
                            "Gate 1 facts must not supply candidate artifacts"
                        )
                    repo_version = flash.repo_version(
                        flash.PLATFORMS[CANARY_PLATFORM_KEY]
                    )
                    if expected_version != repo_version:
                        raise AcceptanceError(
                            "Gate 1 expected version does not match the "
                            "repository candidate"
                        )
                    platform = flash.PLATFORMS[CANARY_PLATFORM_KEY]
                    slots = ["ble", "wifi"]
                    flash.require_artifacts(platform, True, slots)
                    frozen = \
                        flash._prepare_frozen_usb_firmware_artifacts(
                            platform,
                            True,
                            slots,
                        )
                    facts["candidate_artifacts"] = \
                        verify_candidate_artifacts(
                            frozen,
                            expected_version,
                        )
                descriptor = _trusted_session_uplink_descriptor(
                    args.port, session
                )
                with flash.BadgeSerial(
                    descriptor,
                    False,
                    expected_hardware_id=session.uplink_hardware_id,
                ) as badge:
                    badge_status = badge.status(timeout_s=5)
                if args.gate == "android-control-reconnect":
                    facts["snapshot"] = verify_cycle_pre_snapshot(
                        badge_status,
                        session,
                        expected_version,
                        1,
                    )
                else:
                    facts["snapshot"] = verify_badge_snapshot(
                        badge_status,
                        session,
                        expected_version,
                    )
            record_gate(
                args.evidence, session, args.gate, args.status, facts
            )
            if args.status == "FAIL":
                print(json.dumps({
                    "ok": False,
                    "gate": args.gate,
                    "evidence": str(args.evidence),
                    "session_id": session.session_id,
                }, separators=(",", ":"), sort_keys=True))
                return 1
        print(json.dumps({
            "ok": True,
            "gate": args.gate,
            "evidence": str(args.evidence),
            "session_id": session.session_id,
        }, separators=(",", ":"), sort_keys=True))
        return 0
    except (AcceptanceError, flash.FlashError, OSError) as exc:
        flash.print_user_visible(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
