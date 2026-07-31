#!/usr/bin/env python3
"""Badge-only one-command flasher for a FoF badge trio.

Targets one physical badge assembly:
  - uplink: XIAO ESP32-S3 running uplink-s3-fof_badge
  - ble scanner: XIAO ESP32-S3 running scanner-s3-combo-fof_badge
  - wifi scanner: XIAO ESP32-S3 running scanner-s3-combo-fof_badge
"""

from __future__ import annotations

import argparse
import binascii
import copy
import fcntl
import glob
import hashlib
import json
import math
import os
import re
import secrets
import shutil
import stat
import subprocess
import struct
import sys
import tempfile
import time
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from types import MappingProxyType
from collections.abc import Mapping
from typing import Any, Callable, Literal, NoReturn
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen


REPO_ROOT = Path(__file__).resolve().parent.parent
ESP32_DIR = REPO_ROOT / "esp32"
SCANNER_DIR = ESP32_DIR / "scanner"
UPLINK_DIR = ESP32_DIR / "uplink"
ESP32_SCRIPTS_DIR = ESP32_DIR / "scripts"
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))
if str(ESP32_SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(ESP32_SCRIPTS_DIR))

from firmware_version import parse_firmware_identity
from tools.badge_flasher.public_output import (
    CapturedUserVisibleOutput,
    capture_user_visible_output,
    print_user_visible,
    scrub_user_visible_text,
)
from scripts.bound_rom import (
    BoundRomError,
    BoundRomMutationUncertainError,
    BoundRomSession,
    BoundRomUnavailableError,
    RomIdentityEvidence,
    RomOperationEvidence,
)
from scripts.usb_descriptor_binding import (
    TrustedUplinkBinding,
    UsbDescriptorBindingError,
    UsbDescriptorRecord,
    bind_selected_uplink,
    open_bound_application_serial,
    take_usb_descriptor_census,
)
from secure_artifact_tree import FrozenArtifactSet, SecureArtifactError
from verify_badge_scanner_build import (
    prepare_verified_badge_scanner_snapshot,
    verify_badge_scanner_build,
    verify_badge_scanner_sdkconfig,
)
from verify_badge_uplink_build import (
    UPLINK_CANARY_ENV,
    UPLINK_CANARY_RTC_NOINIT_BYTES,
    UPLINK_PRODUCTION_ENV,
    UPLINK_PRODUCTION_RTC_NOINIT_BYTES,
    prepare_verified_badge_uplink_snapshot,
    validate_esp32_s3_image_bytes,
    verify_frozen_badge_uplink_flash_authority,
    verify_badge_uplink_build,
)

DEFAULT_BACKEND = os.environ.get("FOF_BACKEND", "http://localhost:8000")
SCANNER_RELAY_TIMEOUT_MIN_S = 240
SCANNER_RELAY_TIMEOUT_MAX_S = 900
SCANNER_RELAY_TIMEOUT_PER_KB_S = 0.30
SERIAL_RX_BUFFER_MAX = 128 * 1024
BADGE_DETECTION_JSON_MAX_BYTES = 1535
UPLINK_OTA_CREDIT_BYTES = 4096
UPLINK_OTA_WRITE_BYTES = 1024
SCANNER_STAGE_CREDIT_BYTES = 4096
SCANNER_STAGE_WRITE_BYTES = 1024
SCANNER_STAGE_TIMEOUT_S = 480.0
SCANNER_RELAY_CHUNK_BYTES = 1024
UPDATE_PREPARE_TIMEOUT_S = 30.0
UPDATE_PREPARE_RECEIPT_SLICE_S = 3.0
UPDATE_PREPARE_RETRY_S = 0.25
UPDATE_MAINTENANCE_READY_TIMEOUT_S = 30.0
UPDATE_READINESS_MAX_PROBES = 3
UPDATE_ZERO_ATTEMPT_REPROMPT_GRACE_S = 5.0
UPDATE_HOST_CAMPAIGNS_PER_LANE = 3
UPDATE_KEEPALIVE_MAX_S = 15.0
UPDATE_TRANSFER_TIMEOUT_S = 1200.0
UPDATE_MAX_TRANSFER_RESTARTS = 4
UPDATE_BINARY_IDLE_TIMEOUT_S = 5.0
UPDATE_BINARY_IDLE_GUARD_S = 0.25
PASSIVE_RETRY_SLICE_S = 1.0
APPLICATION_DISCOVERY_PROBE_SLICE_S = 5.0
POST_UPLINK_TRANSITION_POLL_S = 5.0
POST_UPLINK_APPLICATION_TIMEOUT_S = 180.0
ROM_CHORD_DISCOVERY_TIMEOUT_S = 120.0
SELECTED_ROM_ABSENCE_TIMEOUT_S = 3.0
ESPRESSIF_USB_SERIAL_JTAG_VID = 0x303A
ESPRESSIF_USB_SERIAL_JTAG_PID = 0x1001
UPLINK_OTA_RECEIPT_KEYS = {
    "ok", "phase", "partition", "received", "total", "credit_bytes",
    "retryable", "reboot_required", "error",
}
SCANNER_RELAY_RECEIPT_KEYS = frozenset({
    "ok", "phase", "slot", "uart", "generation", "hardware_id", "size",
    "bytes", "chunks", "stage", "done", "error",
})
SCANNER_RELAY_PROGRESS_KEYS = frozenset({
    "uart", "stage", "bytes", "size", "percent", "chunks", "nacks",
    "retries", "elapsed_s", "error",
})
ROM_ENTRY_PROMPT = (
    "HOLD OK + MENU FOR 10 SECONDS, RELEASE, THEN PRESS OK ONCE; "
    "THE SCREEN AND USB PORT MAY LOOK UNCHANGED IN ROM - "
    "DO NOT PRESS AGAIN OR REBOOT"
)
LEGACY_USB_BOOTSTRAP_SOURCE_VERSIONS = frozenset({
    "0.64.76-badge-defcon34",
})
LEGACY_USB_BOOTSTRAP_COMMAND = "FOF_BOOTLOADER"
LEGACY_USB_BOOTSTRAP_ACK_PREFIX = "FOF_BOOTLOADER:"
UPDATE_MAINTENANCE_MIN_VERSION = "0.64.79-badge-defcon34"
UPDATE_MAINTENANCE_BOOTSTRAP_SOURCE_VERSION = "0.64.78-badge-defcon34"
UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION = "0.64.79-badge-defcon34"
LEGACY_ROM_ENUMERATION_SETTLE_S = 1.0
ROM_APP_HANDOFF_OPERATION = (
    "write_mem", "0x6000812c", "0x0", "0x1",
)

PLATFORMS: dict[str, dict[str, Any]] = {
    "badge-trio-xiao-s3": {
        "hardware": "FoF Badge trio on Seeed XIAO ESP32-S3",
        "uplink_env": "uplink-s3-fof_badge",
        "uplink_name": "uplink-s3-fof_badge",
        "uplink_project": "fof_badge_uplink",
        "uplink_bin": UPLINK_DIR / ".pio/build/uplink-s3-fof_badge/firmware.bin",
        "scanner_env": "scanner-s3-combo-fof_badge",
        "scanner_name": "scanner-s3-combo-fof_badge",
        "scanner_project": "fof_badge_scanner",
        "scanner_bin": SCANNER_DIR / ".pio/build/scanner-s3-combo-fof_badge/firmware.bin",
        "hardware_type": "seeed_xiao_esp32s3",
        "slots": ("ble", "wifi"),
        "version_macro": "FOF_VERSION_BADGE",
    },
    "badge-trio-xiao-s3-con-crud-canary": {
        "hardware": "FoF Badge trio CON CRUD canary on Seeed XIAO ESP32-S3",
        "uplink_env": "uplink-s3-fof_badge-con-crud-canary",
        "uplink_name": "uplink-s3-fof_badge",
        "uplink_project": "fof_badge_uplink",
        "uplink_bin": UPLINK_DIR / (
            ".pio/build/uplink-s3-fof_badge-con-crud-canary/firmware.bin"
        ),
        "scanner_env": "scanner-s3-combo-fof_badge-con-crud-canary",
        "scanner_name": "scanner-s3-combo-fof_badge",
        "scanner_project": "fof_badge_scanner",
        "scanner_bin": SCANNER_DIR / (
            ".pio/build/scanner-s3-combo-fof_badge-con-crud-canary/"
            "firmware.bin"
        ),
        "hardware_type": "seeed_xiao_esp32s3",
        "slots": ("ble", "wifi"),
        "version_macro": "FOF_VERSION_BADGE_CANARY",
    },
}


class FlashError(RuntimeError):
    pass


class _CliArgumentError(FlashError):
    """Command-line validation failed before any badge action."""


class _PrivateArgumentParser(argparse.ArgumentParser):
    """Raise parser diagnostics so the public stderr boundary can scrub them."""

    def error(self, message: str) -> NoReturn:
        raise _CliArgumentError(message) from None


class RomProbeUnavailable(FlashError):
    """One guarded no-reset ROM sync received no serial bytes."""


class RomFlashUncertainError(FlashError):
    """A ROM mutation may have happened, but its terminal state is unknown."""


class SerialReadTimeout(FlashError):
    """A bounded passive read expired without a complete requested frame."""

    def __init__(self, message: str, *, saw_activity: bool,
                 partial_frame: bool) -> None:
        super().__init__(message)
        self.saw_activity = saw_activity
        self.partial_frame = partial_frame


class SerialTransportError(FlashError):
    """USB application transport produced malformed or failed I/O."""

    def __init__(self, message: str, *, terminal_unavailable: bool = False) -> None:
        super().__init__(message)
        self.terminal_unavailable = terminal_unavailable


class UpdateMaintenanceUnsupportedError(FlashError):
    """The exact legacy application rejected prepare_update as unknown."""


class _DeferredLegacyUpdateMaintenanceMarker(
    UpdateMaintenanceUnsupportedError
):
    """A legacy leading-LF marker remained unresolved through one slice."""


class _DuplicateJsonMemberError(ValueError):
    pass


@dataclass(frozen=True, slots=True)
class FrozenUsbFirmwareArtifacts:
    """Selected verified build trees frozen before any badge USB access."""

    uplink: FrozenArtifactSet | None
    scanner: FrozenArtifactSet | None

    def __post_init__(self) -> None:
        for role, artifact_set in (
            ("uplink", self.uplink),
            ("scanner", self.scanner),
        ):
            if artifact_set is not None and \
                    type(artifact_set) is not FrozenArtifactSet:
                raise FlashError(
                    f"frozen USB {role} artifacts have an invalid type"
                )


class HostJsonSchemaId(Enum):
    SCANNER_STAGE_READY_CREDIT = "scanner_stage_ready_credit"
    SCANNER_STAGE_FINAL = "scanner_stage_final"
    SCANNER_STAGE_FAILURE = "scanner_stage_failure"
    UPLINK_OTA = "uplink_ota"
    RELAY_TERMINAL = "relay_terminal"
    RELAY_PROGRESS = "relay_progress"
    UPDATE_MODE_REBOOTING = "update_mode_rebooting"
    UPDATE_MODE_ACTIVE = "update_mode_active"
    UPDATE_MODE_FINISHING = "update_mode_finishing"
    UPDATE_MODE_ABORTING = "update_mode_aborting"
    UPDATE_MODE_WAITING = "update_mode_waiting"
    UPDATE_MODE_BUSY = "update_mode_busy"
    CONTROL_ERROR = "control_error"
    BADGE_DETECTION = "badge_detection"


class _HostJsonWireType(Enum):
    BOOL = "bool"
    UINT32 = "uint32"
    NONNEGATIVE_INT64 = "nonnegative_int64"
    SIGNED_INT32 = "signed_int32"
    FINITE_NUMBER = "finite_number"
    ASCII_TOKEN = "ascii_token"
    PRINTABLE_STRING = "printable_string"


_HOST_JSON_SCHEMA_REGISTRY = MappingProxyType({
    HostJsonSchemaId.SCANNER_STAGE_READY_CREDIT: MappingProxyType({
        "ok": _HostJsonWireType.BOOL,
        "partition": _HostJsonWireType.ASCII_TOKEN,
        "size": _HostJsonWireType.UINT32,
        "crc32": _HostJsonWireType.UINT32,
        "sha256": _HostJsonWireType.ASCII_TOKEN,
        "target": _HostJsonWireType.ASCII_TOKEN,
        "name": _HostJsonWireType.ASCII_TOKEN,
        "app_project": _HostJsonWireType.ASCII_TOKEN,
        "project": _HostJsonWireType.ASCII_TOKEN,
        "hardware_type": _HostJsonWireType.ASCII_TOKEN,
        "hardware": _HostJsonWireType.ASCII_TOKEN,
        "version": _HostJsonWireType.ASCII_TOKEN,
        "slot_mask": _HostJsonWireType.UINT32,
        "flow_control": _HostJsonWireType.ASCII_TOKEN,
        "phase": _HostJsonWireType.ASCII_TOKEN,
        "received": _HostJsonWireType.UINT32,
        "total": _HostJsonWireType.UINT32,
        "credit_bytes": _HostJsonWireType.UINT32,
    }),
    HostJsonSchemaId.SCANNER_STAGE_FINAL: MappingProxyType({
        "ok": _HostJsonWireType.BOOL,
        "partition": _HostJsonWireType.ASCII_TOKEN,
        "size": _HostJsonWireType.UINT32,
        "crc32": _HostJsonWireType.UINT32,
        "sha256": _HostJsonWireType.ASCII_TOKEN,
        "target": _HostJsonWireType.ASCII_TOKEN,
        "name": _HostJsonWireType.ASCII_TOKEN,
        "app_project": _HostJsonWireType.ASCII_TOKEN,
        "project": _HostJsonWireType.ASCII_TOKEN,
        "hardware_type": _HostJsonWireType.ASCII_TOKEN,
        "hardware": _HostJsonWireType.ASCII_TOKEN,
        "version": _HostJsonWireType.ASCII_TOKEN,
        "slot_mask": _HostJsonWireType.UINT32,
        "flow_control": _HostJsonWireType.ASCII_TOKEN,
        "phase": _HostJsonWireType.ASCII_TOKEN,
        "received": _HostJsonWireType.UINT32,
        "total": _HostJsonWireType.UINT32,
        "credit_bytes": _HostJsonWireType.UINT32,
        "generation": _HostJsonWireType.UINT32,
    }),
    HostJsonSchemaId.SCANNER_STAGE_FAILURE: MappingProxyType({
        "ok": _HostJsonWireType.BOOL,
        "error": _HostJsonWireType.PRINTABLE_STRING,
    }),
    HostJsonSchemaId.UPLINK_OTA: MappingProxyType({
        "ok": _HostJsonWireType.BOOL,
        "phase": _HostJsonWireType.ASCII_TOKEN,
        "partition": _HostJsonWireType.ASCII_TOKEN,
        "received": _HostJsonWireType.UINT32,
        "total": _HostJsonWireType.UINT32,
        "credit_bytes": _HostJsonWireType.UINT32,
        "retryable": _HostJsonWireType.BOOL,
        "reboot_required": _HostJsonWireType.BOOL,
        "error": _HostJsonWireType.PRINTABLE_STRING,
    }),
    HostJsonSchemaId.RELAY_TERMINAL: MappingProxyType({
        "ok": _HostJsonWireType.BOOL,
        "phase": _HostJsonWireType.ASCII_TOKEN,
        "slot": _HostJsonWireType.ASCII_TOKEN,
        "uart": _HostJsonWireType.ASCII_TOKEN,
        "generation": _HostJsonWireType.UINT32,
        "hardware_id": _HostJsonWireType.ASCII_TOKEN,
        "size": _HostJsonWireType.UINT32,
        "bytes": _HostJsonWireType.UINT32,
        "chunks": _HostJsonWireType.UINT32,
        "stage": _HostJsonWireType.ASCII_TOKEN,
        "done": _HostJsonWireType.BOOL,
        "error": _HostJsonWireType.PRINTABLE_STRING,
    }),
    HostJsonSchemaId.RELAY_PROGRESS: MappingProxyType({
        "uart": _HostJsonWireType.ASCII_TOKEN,
        "stage": _HostJsonWireType.ASCII_TOKEN,
        "bytes": _HostJsonWireType.UINT32,
        "size": _HostJsonWireType.UINT32,
        "percent": _HostJsonWireType.UINT32,
        "chunks": _HostJsonWireType.UINT32,
        "nacks": _HostJsonWireType.UINT32,
        "retries": _HostJsonWireType.UINT32,
        "elapsed_s": _HostJsonWireType.NONNEGATIVE_INT64,
        "error": _HostJsonWireType.PRINTABLE_STRING,
    }),
    HostJsonSchemaId.UPDATE_MODE_REBOOTING: MappingProxyType({
        "ok": _HostJsonWireType.BOOL,
        "phase": _HostJsonWireType.ASCII_TOKEN,
        "session": _HostJsonWireType.ASCII_TOKEN,
        "retryable": _HostJsonWireType.BOOL,
        "reboot_required": _HostJsonWireType.BOOL,
    }),
    HostJsonSchemaId.UPDATE_MODE_ACTIVE: MappingProxyType({
        "ok": _HostJsonWireType.BOOL,
        "phase": _HostJsonWireType.ASCII_TOKEN,
        "session": _HostJsonWireType.ASCII_TOKEN,
        "retryable": _HostJsonWireType.BOOL,
        "reboot_required": _HostJsonWireType.BOOL,
    }),
    HostJsonSchemaId.UPDATE_MODE_FINISHING: MappingProxyType({
        "ok": _HostJsonWireType.BOOL,
        "phase": _HostJsonWireType.ASCII_TOKEN,
        "session": _HostJsonWireType.ASCII_TOKEN,
        "retryable": _HostJsonWireType.BOOL,
        "reboot_required": _HostJsonWireType.BOOL,
    }),
    HostJsonSchemaId.UPDATE_MODE_ABORTING: MappingProxyType({
        "ok": _HostJsonWireType.BOOL,
        "phase": _HostJsonWireType.ASCII_TOKEN,
        "session": _HostJsonWireType.ASCII_TOKEN,
        "retryable": _HostJsonWireType.BOOL,
        "reboot_required": _HostJsonWireType.BOOL,
    }),
    HostJsonSchemaId.UPDATE_MODE_WAITING: MappingProxyType({
        "ok": _HostJsonWireType.BOOL,
        "phase": _HostJsonWireType.ASCII_TOKEN,
        "session": _HostJsonWireType.ASCII_TOKEN,
        "retryable": _HostJsonWireType.BOOL,
        "reboot_required": _HostJsonWireType.BOOL,
        "error": _HostJsonWireType.ASCII_TOKEN,
    }),
    HostJsonSchemaId.UPDATE_MODE_BUSY: MappingProxyType({
        "ok": _HostJsonWireType.BOOL,
        "phase": _HostJsonWireType.ASCII_TOKEN,
        "session": _HostJsonWireType.ASCII_TOKEN,
        "retryable": _HostJsonWireType.BOOL,
        "reboot_required": _HostJsonWireType.BOOL,
        "error": _HostJsonWireType.ASCII_TOKEN,
    }),
    HostJsonSchemaId.CONTROL_ERROR: MappingProxyType({
        "error": _HostJsonWireType.PRINTABLE_STRING,
    }),
    HostJsonSchemaId.BADGE_DETECTION: MappingProxyType({
        "id": _HostJsonWireType.PRINTABLE_STRING,
        "manufacturer": _HostJsonWireType.PRINTABLE_STRING,
        "badge_label": _HostJsonWireType.PRINTABLE_STRING,
        "badge_class": _HostJsonWireType.PRINTABLE_STRING,
        "badge_entity_key": _HostJsonWireType.PRINTABLE_STRING,
        "source": _HostJsonWireType.UINT32,
        "confidence": _HostJsonWireType.FINITE_NUMBER,
        "threat_score": _HostJsonWireType.FINITE_NUMBER,
        "rssi": _HostJsonWireType.SIGNED_INT32,
    }),
})

_HOST_FIRMWARE_RECEIPT_SCHEMAS = MappingProxyType({
    "FOF_FW_UPLOAD:": frozenset({
        HostJsonSchemaId.SCANNER_STAGE_READY_CREDIT,
        HostJsonSchemaId.SCANNER_STAGE_FINAL,
        HostJsonSchemaId.SCANNER_STAGE_FAILURE,
    }),
    "FOF_UPLINK_OTA:": frozenset({
        HostJsonSchemaId.UPLINK_OTA,
    }),
    "FOF_FW_RELAY:": frozenset({
        HostJsonSchemaId.RELAY_TERMINAL,
    }),
    "FOF_FW_RELAY_PROGRESS:": frozenset({
        HostJsonSchemaId.RELAY_PROGRESS,
    }),
    "FOF_UPDATE_MODE:": frozenset({
        HostJsonSchemaId.UPDATE_MODE_REBOOTING,
        HostJsonSchemaId.UPDATE_MODE_ACTIVE,
        HostJsonSchemaId.UPDATE_MODE_FINISHING,
        HostJsonSchemaId.UPDATE_MODE_ABORTING,
        HostJsonSchemaId.UPDATE_MODE_WAITING,
        HostJsonSchemaId.UPDATE_MODE_BUSY,
    }),
    "FOF_CTL_ERROR:": frozenset({
        HostJsonSchemaId.CONTROL_ERROR,
    }),
})

_UPDATE_SESSION_RE = re.compile(r"^[0-9A-F]{16}$")
_UPDATE_MODE_SUCCESS_TUPLES = MappingProxyType({
    "rebooting": (True, True, True),
    "active": (True, False, False),
    "finishing": (True, False, True),
    "aborting": (True, False, True),
})
_UPDATE_MODE_RETRYABLE_BUSY_ERRORS = frozenset({
    "campaign_state_busy",
    "success_gates_pending",
    "firmware_operation_active",
})
_UPDATE_MODE_SCHEMA_PHASES = MappingProxyType({
    HostJsonSchemaId.UPDATE_MODE_REBOOTING: "rebooting",
    HostJsonSchemaId.UPDATE_MODE_ACTIVE: "active",
    HostJsonSchemaId.UPDATE_MODE_FINISHING: "finishing",
    HostJsonSchemaId.UPDATE_MODE_ABORTING: "aborting",
    HostJsonSchemaId.UPDATE_MODE_WAITING: "waiting_for_owner",
    HostJsonSchemaId.UPDATE_MODE_BUSY: "busy",
})


def _validated_update_session(session: Any) -> str:
    if type(session) is not str or not _UPDATE_SESSION_RE.fullmatch(session) or \
            session == "0000000000000000":
        raise FlashError(
            "update session must be 16 uppercase nonzero hexadecimal digits"
        )
    return session


def _new_update_session() -> str:
    session = secrets.token_hex(8).upper()
    return _validated_update_session(session)


class ScannerCampaignFailure(FlashError):
    """Validated terminal scanner failure with immutable retry evidence."""

    __slots__ = (
        "_session",
        "_requested_slots",
        "_failed_slots",
        "_successful_slots",
        "_stage_receipt_json",
        "_campaign_json",
    )

    def __init__(
        self,
        *,
        session: str,
        requested_slots: list[str],
        stage_receipt: Mapping[str, Any],
        campaign: Mapping[str, Any],
    ) -> None:
        bound_session = _validated_update_session(session)
        if type(requested_slots) is not list or not requested_slots or \
                len(requested_slots) != len(set(requested_slots)) or \
                any(slot not in ("ble", "wifi") for slot in requested_slots):
            raise FlashError(
                "scanner campaign failure requested slots are invalid"
            )
        slots = tuple(requested_slots)
        expected_mask = scanner_slot_mask(list(slots))
        if type(stage_receipt) is not dict:
            raise FlashError(
                "scanner campaign failure stage receipt is malformed"
            )
        generation = stage_receipt.get("generation")
        if type(generation) is not int or not 1 <= generation <= 0xFFFFFFFF:
            raise FlashError(
                "scanner campaign failure stage generation is invalid"
            )
        if stage_receipt.get("slot_mask") != expected_mask:
            raise FlashError(
                "scanner campaign failure stage slot mask mismatch"
            )
        if type(stage_receipt.get("target")) is not str or \
                not stage_receipt["target"] or \
                type(stage_receipt.get("sha256")) is not str or \
                not re.fullmatch(
                    r"[0-9a-f]{64}", stage_receipt["sha256"]
                ) or \
                type(stage_receipt.get("size")) is not int or \
                not 1 <= stage_receipt["size"] <= 0xFFFFFFFF:
            raise FlashError(
                "scanner campaign failure stage identity is invalid"
            )
        validated_campaign = _validate_update_campaign_status(
            dict(campaign),
            expected_generation=generation,
            expected_slot_mask=expected_mask,
        )
        by_slot = {
            entry["slot"]: entry
            for entry in validated_campaign["scanners"]
        }
        slot_ids = {"ble": 0, "wifi": 1}
        failed = frozenset(
            slot for slot in slots
            if by_slot[slot_ids[slot]]["state"] == "failed"
        )
        successful = frozenset(
            slot for slot in slots
            if by_slot[slot_ids[slot]]["state"] in {
                "converged", "current",
            }
        )
        if not failed:
            raise FlashError(
                "scanner campaign failure has no failed requested lane"
            )
        try:
            stage_json = json.dumps(
                stage_receipt,
                allow_nan=False,
                separators=(",", ":"),
                sort_keys=True,
            ).encode("utf-8")
            campaign_json = json.dumps(
                validated_campaign,
                allow_nan=False,
                separators=(",", ":"),
                sort_keys=True,
            ).encode("utf-8")
        except (TypeError, ValueError) as exc:
            raise FlashError(
                "scanner campaign failure evidence is not JSON-safe"
            ) from exc

        self._session = bound_session
        self._requested_slots = frozenset(slots)
        self._failed_slots = failed
        self._successful_slots = successful
        self._stage_receipt_json = stage_json
        self._campaign_json = campaign_json
        super().__init__(
            "maintenance scanner campaign failed: " +
            ", ".join(sorted(failed))
        )

    @property
    def session(self) -> str:
        return self._session

    @property
    def requested_slots(self) -> frozenset[str]:
        return self._requested_slots

    @property
    def failed_slots(self) -> frozenset[str]:
        return self._failed_slots

    @property
    def successful_slots(self) -> frozenset[str]:
        return self._successful_slots

    @property
    def stage_receipt(self) -> dict[str, Any]:
        return json.loads(self._stage_receipt_json)

    @property
    def campaign(self) -> dict[str, Any]:
        return json.loads(self._campaign_json)


@dataclass(frozen=True, slots=True)
class _ScannerCampaignRetryDecision:
    slot: str
    reason: str
    scanner_hardware_id: str
    successful_slots: frozenset[str]


class ScannerCampaignRetriesExhausted(FlashError):
    """Bounded host campaign failure with immutable operator evidence."""

    __slots__ = ("_attempt_history_json",)

    def __init__(
        self,
        attempt_history: tuple[dict[str, Any], ...],
    ) -> None:
        if type(attempt_history) is not tuple or not attempt_history:
            raise FlashError(
                "scanner retry exhaustion history is malformed"
            )
        try:
            self._attempt_history_json = tuple(
                json.dumps(
                    attempt,
                    allow_nan=False,
                    separators=(",", ":"),
                    sort_keys=True,
                ).encode("utf-8")
                for attempt in attempt_history
            )
        except (TypeError, ValueError) as exc:
            raise FlashError(
                "scanner retry exhaustion history is not JSON-safe"
            ) from exc
        super().__init__(
            "scanner update stopped after three host campaigns for one lane"
        )

    @property
    def attempt_history(self) -> tuple[dict[str, Any], ...]:
        return tuple(
            json.loads(snapshot)
            for snapshot in self._attempt_history_json
        )


@dataclass(frozen=True, slots=True)
class _ScannerRetrySequence:
    session: str
    latest_slots: tuple[str, ...]
    preflight_older_slots: frozenset[str]
    recovery_slots: frozenset[str]
    expected_hardware_ids: tuple[tuple[str, str], ...]
    _pre_stage_status_json: bytes = field(repr=False)
    _maintenance_status_json: bytes = field(repr=False)
    _stage_receipt_json: bytes = field(repr=False)
    _attempt_history_json: tuple[bytes, ...] = field(repr=False)

    @property
    def scanner_result(self) -> tuple[
        dict[str, Any],
        dict[str, Any],
        dict[str, Any],
        frozenset[str],
        frozenset[str],
        dict[str, str],
    ]:
        return (
            json.loads(self._pre_stage_status_json),
            json.loads(self._maintenance_status_json),
            json.loads(self._stage_receipt_json),
            self.preflight_older_slots,
            self.recovery_slots,
            dict(self.expected_hardware_ids),
        )

    @property
    def attempt_history(self) -> tuple[dict[str, Any], ...]:
        return tuple(
            json.loads(snapshot)
            for snapshot in self._attempt_history_json
        )


@dataclass(slots=True)
class _UpdateRetryResetBudget:
    used: bool = False

    def __post_init__(self) -> None:
        if type(self.used) is not bool:
            raise FlashError(
                "update retry USB reset budget is malformed"
            )


@dataclass(frozen=True, slots=True)
class _UpdateMaintenanceRecoveryResult:
    descriptor: UsbDescriptorRecord | None
    action: str
    usb_reset_used: bool
    _status_json: bytes = field(repr=False)

    @property
    def status(self) -> dict[str, Any]:
        return json.loads(self._status_json)


def _issue_update_maintenance_recovery_result(
    *,
    badge: Any,
    status: dict[str, Any],
    action: str,
    usb_reset_used: bool,
    require_descriptor: bool,
) -> _UpdateMaintenanceRecoveryResult:
    if action not in ("already_normal", "session_abort", "usb_reset"):
        raise FlashError(
            "update maintenance recovery action is invalid"
        )
    if type(usb_reset_used) is not bool or \
            usb_reset_used is not (action == "usb_reset"):
        raise FlashError(
            "update maintenance USB reset evidence is inconsistent"
        )
    descriptor = getattr(badge, "_descriptor", None)
    if descriptor is not None and type(descriptor) is not UsbDescriptorRecord:
        raise FlashError(
            "recovered uplink descriptor is malformed"
        )
    if require_descriptor and descriptor is None:
        raise FlashError(
            "retry recovery did not retain its uplink descriptor"
        )
    try:
        status_json = json.dumps(
            status,
            allow_nan=False,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
    except (TypeError, ValueError) as exc:
        raise FlashError(
            "recovered normal status is not JSON-safe"
        ) from exc
    return _UpdateMaintenanceRecoveryResult(
        descriptor=descriptor,
        action=action,
        usb_reset_used=usb_reset_used,
        _status_json=status_json,
    )


def _classify_scanner_campaign_retry(
    failure: ScannerCampaignFailure,
    *,
    status: dict[str, Any],
    platform: dict[str, Any],
    target_version: str,
    expected_hardware_ids: dict[str, str],
) -> _ScannerCampaignRetryDecision:
    """Accept only one identity-bound, explicitly recognized transient."""
    if type(failure) is not ScannerCampaignFailure:
        raise FlashError(
            "scanner campaign retry requires typed failure evidence"
        )
    validate_uplink_application_status(status)
    if len(failure.failed_slots) != 1:
        raise FlashError(
            "scanner campaign retry requires exactly one failed lane"
        )
    requested = set(failure.requested_slots)
    if type(expected_hardware_ids) is not dict or \
            set(expected_hardware_ids) != requested:
        raise FlashError(
            "scanner campaign retry identity set is incomplete"
        )
    expected_ids = {
        slot: normalized_hardware_id(expected_hardware_ids[slot])
        for slot in requested
    }
    captured = capture_scanner_hardware_ids(
        status,
        platform,
        sorted(requested),
        require_connected=True,
    )
    for slot in requested:
        if captured.get(slot) != expected_ids[slot]:
            raise FlashError(
                f"{slot} scanner hardware id mismatch: "
                f"got {captured.get(slot)}, wanted {expected_ids[slot]}"
            )

    failed_slot = next(iter(failure.failed_slots))
    successful = failure.successful_slots
    if successful != frozenset(requested - {failed_slot}):
        raise FlashError(
            "scanner campaign peer lanes did not converge"
        )
    if successful:
        verify_scanners(
            status,
            platform,
            sorted(successful),
            target_version,
            expected_hardware_ids={
                slot: expected_ids[slot] for slot in successful
            },
        )

    failed_info = scanner_status_by_uart(status).get(failed_slot)
    if not isinstance(failed_info, dict):
        raise FlashError(
            f"{failed_slot} scanner retry status is missing"
        )
    current_version = failed_info.get("ver") or \
        failed_info.get("version")
    relation = firmware_version_relation(
        target_version, str(current_version or "")
    )
    if relation not in ("newer", "equal"):
        raise FlashError(
            f"{failed_slot} scanner retry version is unsafe: "
            f"current={current_version!r}, target={target_version!r}"
        )
    verify_scanners(
        status,
        platform,
        [failed_slot],
        str(current_version),
        expected_hardware_ids={
            failed_slot: expected_ids[failed_slot]
        },
    )

    campaign = failure.campaign
    scanner_id = 0 if failed_slot == "ble" else 1
    entry = {
        item["slot"]: item for item in campaign["scanners"]
    }[scanner_id]
    attempts = entry["attempts"]
    probes = campaign["readiness_probes"][scanner_id]

    error_values: list[str] = []
    for field in ("last_relay_error", "last_fw_error"):
        value = failed_info.get(field)
        if value in (None, ""):
            continue
        if type(value) is not str:
            raise FlashError(
                f"{failed_slot} scanner {field} is malformed"
            )
        error_values.append(value)
    unique_errors = set(error_values)
    recognized_errors = {
        "ota_ack_timeout",
        "offer_manifest_mismatch",
    }
    if unique_errors - recognized_errors or len(unique_errors) > 1:
        raise FlashError(
            f"{failed_slot} scanner failure is not a recognized transient"
        )

    reason: str | None = None
    if attempts == 0 and probes == UPDATE_READINESS_MAX_PROBES:
        reason = "readiness_exhausted"
    elif unique_errors == {"ota_ack_timeout"}:
        reason = "ota_ack_timeout"
    elif unique_errors == {"offer_manifest_mismatch"}:
        reason = "offer_manifest_mismatch"
    else:
        fw_state = failed_info.get("fw_state")
        fw_backoff_s = failed_info.get("fw_backoff_s")
        if (
            fw_state == "deferred" and
            type(fw_backoff_s) is int and
            fw_backoff_s > 0
        ):
            reason = "deferred_backoff"
    if reason is None:
        raise FlashError(
            f"{failed_slot} scanner failure is not a recognized transient"
        )
    return _ScannerCampaignRetryDecision(
        slot=failed_slot,
        reason=reason,
        scanner_hardware_id=expected_ids[failed_slot],
        successful_slots=frozenset(successful),
    )


def _validate_update_mode_receipt(
    receipt: Mapping[str, Any],
    *,
    session: str,
) -> dict[str, Any]:
    """Validate phase semantics after exact wire-schema decoding."""
    bound_session = _validated_update_session(session)
    if type(receipt) is not dict:
        raise FlashError("update mode receipt must be an exact object")
    if receipt.get("session") != bound_session:
        raise FlashError("update mode receipt session mismatch")
    phase = receipt.get("phase")
    if phase in _UPDATE_MODE_SUCCESS_TUPLES:
        if set(receipt) != {
            "ok", "phase", "session", "retryable", "reboot_required"
        }:
            raise FlashError("update mode success receipt schema mismatch")
        expected = _UPDATE_MODE_SUCCESS_TUPLES[phase]
        actual = (
            receipt.get("ok"),
            receipt.get("retryable"),
            receipt.get("reboot_required"),
        )
        if actual != expected:
            raise FlashError("update mode success receipt tuple mismatch")
        return dict(receipt)
    if phase == "waiting_for_owner":
        if set(receipt) != {
            "ok", "phase", "session", "retryable", "reboot_required",
            "error",
        } or (
            receipt.get("ok"),
            receipt.get("retryable"),
            receipt.get("reboot_required"),
            receipt.get("error"),
        ) != (False, True, False, "firmware_operation_active"):
            raise FlashError("update mode waiting receipt tuple mismatch")
        return dict(receipt)
    if phase == "busy":
        if set(receipt) != {
            "ok", "phase", "session", "retryable", "reboot_required",
            "error",
        } or receipt.get("ok") is not False or \
                receipt.get("reboot_required") is not False:
            raise FlashError("update mode busy receipt tuple mismatch")
        retryable = receipt.get("retryable")
        error = receipt.get("error")
        if retryable is True:
            if error not in _UPDATE_MODE_RETRYABLE_BUSY_ERRORS:
                raise FlashError("update mode busy error mismatch")
        elif retryable is False:
            if error != "session_conflict":
                raise FlashError("update mode conflict receipt mismatch")
        else:
            raise FlashError("update mode busy retryable flag mismatch")
        return dict(receipt)
    raise FlashError("update mode receipt phase is unrecognized")


def _host_json_raw_member_lexemes(
    payload: str,
) -> tuple[dict[str, bool], dict[str, str]]:
    """Validate raw lexemes and record top-level string/number spelling."""
    tokens: list[tuple[str, str, bool]] = []
    index = 0
    while index < len(payload):
        char = payload[index]
        codepoint = ord(char)
        if char in " \t\r\n":
            index += 1
            continue
        if codepoint < 0x20 or codepoint == 0x7F:
            raise ValueError("raw JSON control")
        if char in "{}[]:,":
            tokens.append((char, "", False))
            index += 1
            continue
        if char != '"':
            start = index
            while index < len(payload):
                char = payload[index]
                codepoint = ord(char)
                if char in " \t\r\n{}[]:,\"":
                    break
                if codepoint < 0x20 or codepoint == 0x7F:
                    raise ValueError("raw JSON control")
                index += 1
            if index == start:
                raise ValueError("malformed JSON token")
            tokens.append(("primitive", payload[start:index], False))
            continue

        index += 1
        start = index
        escaped = False
        while index < len(payload):
            char = payload[index]
            codepoint = ord(char)
            if codepoint < 0x20 or codepoint == 0x7F:
                raise ValueError("raw JSON string control")
            if char == '"':
                tokens.append(
                    ("string", payload[start:index], escaped)
                )
                index += 1
                break
            if char != "\\":
                index += 1
                continue

            escaped = True
            index += 1
            if index >= len(payload):
                raise ValueError("incomplete JSON escape")
            escape = payload[index]
            if escape in "bfnrt":
                raise ValueError("escaped JSON control")
            if escape in '"\\/':
                index += 1
                continue
            if escape != "u" or index + 4 >= len(payload):
                raise ValueError("invalid JSON escape")
            digits = payload[index + 1:index + 5]
            if any(digit not in "0123456789abcdefABCDEF" for digit in digits):
                raise ValueError("invalid JSON unicode escape")
            escaped_codepoint = int(digits, 16)
            if escaped_codepoint < 0x20 or escaped_codepoint == 0x7F:
                raise ValueError("escaped JSON control")
            index += 5
        else:
            raise ValueError("unterminated JSON string")

    value_escapes: dict[str, bool] = {}
    primitive_lexemes: dict[str, str] = {}
    containers: list[str] = []
    for token_index, token in enumerate(tokens):
        kind, raw, escaped = token
        if kind in ("{", "["):
            containers.append(kind)
            continue
        if kind in ("}", "]"):
            if not containers:
                raise ValueError("unbalanced JSON container")
            expected = "{" if kind == "}" else "["
            if containers[-1] != expected:
                raise ValueError("mismatched JSON container")
            containers.pop()
            continue
        if kind != "string" or token_index + 1 >= len(tokens) or \
                tokens[token_index + 1][0] != ":":
            continue
        if not containers or containers[-1] != "{":
            continue
        if escaped:
            raise ValueError("escaped JSON member name")
        if len(containers) != 1 or token_index + 2 >= len(tokens):
            continue
        value_token = tokens[token_index + 2]
        if value_token[0] == "string":
            value_escapes[raw] = value_token[2]
        elif value_token[0] == "primitive":
            primitive_lexemes[raw] = value_token[1]
    return value_escapes, primitive_lexemes


def _host_json_object_without_duplicates(
    pairs: list[tuple[str, Any]],
) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for name, value in pairs:
        if name in result:
            raise _DuplicateJsonMemberError()
        result[name] = value
    return result


def _reject_host_json_constant(_constant: str) -> Any:
    raise ValueError("nonstandard JSON numeric constant")


def _host_json_value_matches(
    value: Any,
    wire_type: _HostJsonWireType,
    *,
    escaped: bool,
    primitive_lexeme: str | None,
) -> bool:
    if wire_type is _HostJsonWireType.BOOL:
        return type(value) is bool
    if wire_type is _HostJsonWireType.UINT32:
        return (
            type(value) is int and
            0 <= value <= 0xFFFFFFFF and
            primitive_lexeme is not None and
            bool(primitive_lexeme) and
            all("0" <= char <= "9" for char in primitive_lexeme)
        )
    if wire_type is _HostJsonWireType.NONNEGATIVE_INT64:
        return (
            type(value) is int and
            0 <= value <= 0x7FFFFFFFFFFFFFFF and
            primitive_lexeme is not None and
            bool(primitive_lexeme) and
            all("0" <= char <= "9" for char in primitive_lexeme)
        )
    if wire_type is _HostJsonWireType.SIGNED_INT32:
        if type(value) is not int or not -(1 << 31) <= value < (1 << 31) or \
                primitive_lexeme is None:
            return False
        digits = (
            primitive_lexeme[1:]
            if primitive_lexeme.startswith("-")
            else primitive_lexeme
        )
        return bool(digits) and all("0" <= char <= "9" for char in digits)
    if wire_type is _HostJsonWireType.FINITE_NUMBER:
        return (
            type(value) in (int, float) and
            math.isfinite(value) and
            primitive_lexeme is not None
        )
    if wire_type is _HostJsonWireType.ASCII_TOKEN:
        return (
            type(value) is str and
            not escaped and
            bool(value) and
            all(0x21 <= ord(char) <= 0x7E for char in value)
        )
    if wire_type is _HostJsonWireType.PRINTABLE_STRING:
        if type(value) is not str or not all(
            char.isprintable() for char in value
        ):
            return False
        try:
            value.encode("utf-8", "strict")
        except UnicodeError:
            return False
        return True
    return False


def _strict_json_object_loads(
    payload: str,
    *,
    label: str,
    allowed_schema_ids: tuple[HostJsonSchemaId, ...],
) -> dict[str, Any]:
    """Decode one bounded host receipt against one exact permitted schema."""
    try:
        if type(label) is not str or not label:
            raise ValueError("invalid JSON label")
        if type(allowed_schema_ids) is not tuple or not allowed_schema_ids:
            raise ValueError("invalid JSON schema selection")
        if len(set(allowed_schema_ids)) != len(allowed_schema_ids):
            raise ValueError("duplicate JSON schema selection")
        if any(
            type(schema_id) is not HostJsonSchemaId or
            schema_id not in _HOST_JSON_SCHEMA_REGISTRY
            for schema_id in allowed_schema_ids
        ):
            raise ValueError("unknown JSON schema selection")
        if type(payload) is not str or len(payload) > SERIAL_RX_BUFFER_MAX:
            raise ValueError("invalid JSON payload")
        encoded_payload = payload.encode("utf-8", "strict")
        if len(encoded_payload) > SERIAL_RX_BUFFER_MAX:
            raise ValueError("oversized JSON payload")

        value_escapes, primitive_lexemes = (
            _host_json_raw_member_lexemes(payload)
        )
        parsed = json.loads(
            payload,
            object_pairs_hook=_host_json_object_without_duplicates,
            parse_constant=_reject_host_json_constant,
        )
        if type(parsed) is not dict:
            raise ValueError("non-object JSON payload")

        matching_schema_ids = [
            schema_id
            for schema_id in allowed_schema_ids
            if set(parsed) == set(_HOST_JSON_SCHEMA_REGISTRY[schema_id]) and
            (
                schema_id not in _UPDATE_MODE_SCHEMA_PHASES or
                parsed.get("phase") == _UPDATE_MODE_SCHEMA_PHASES[schema_id]
            )
        ]
        if len(matching_schema_ids) != 1:
            raise ValueError("JSON schema mismatch")
        schema = _HOST_JSON_SCHEMA_REGISTRY[matching_schema_ids[0]]
        for member_name, wire_type in schema.items():
            if not _host_json_value_matches(
                parsed[member_name],
                wire_type,
                escaped=value_escapes.get(member_name, True),
                primitive_lexeme=primitive_lexemes.get(member_name),
            ):
                raise ValueError("JSON wire type mismatch")
        return parsed
    except Exception:
        pass
    raise SerialTransportError(
        "host JSON receipt failed strict validation"
    )


def _strict_serial_utf8_decode(payload: bytes) -> str:
    """Decode one bounded serial span without retaining malformed bytes."""
    try:
        if type(payload) is not bytes:
            raise UnicodeError()
        return payload.decode("utf-8", "strict")
    except UnicodeError:
        pass
    raise SerialTransportError(
        "serial frame failed strict UTF-8 validation"
    )


_BADGE_DETECTION_STRING_BYTE_LIMITS = MappingProxyType({
    "id": 63,
    "manufacturer": 31,
    "badge_label": 23,
    "badge_class": 15,
    "badge_entity_key": 95,
})


def _validate_optional_badge_detection_frame(
    raw_line: bytes,
) -> dict[str, Any]:
    """Strictly consume one producer-shaped asynchronous FOF_DET frame."""
    prefix = b"FOF_DET:"
    if type(raw_line) is not bytes or not raw_line.startswith(prefix):
        raise SerialTransportError("badge detection frame prefix mismatch")
    payload_bytes = raw_line[len(prefix):]
    if not payload_bytes or \
            len(payload_bytes) > BADGE_DETECTION_JSON_MAX_BYTES:
        raise SerialTransportError("badge detection payload size mismatch")
    detection = _strict_json_object_loads(
        _strict_serial_utf8_decode(payload_bytes),
        label="FOF_DET optional telemetry",
        allowed_schema_ids=(HostJsonSchemaId.BADGE_DETECTION,),
    )
    for field_name, byte_limit in \
            _BADGE_DETECTION_STRING_BYTE_LIMITS.items():
        if len(detection[field_name].encode("utf-8", "strict")) > byte_limit:
            raise SerialTransportError(
                f"badge detection {field_name} exceeds producer bound"
            )
    source = detection["source"]
    confidence = detection["confidence"]
    threat_score = detection["threat_score"]
    rssi = detection["rssi"]
    if source > 8:
        raise SerialTransportError("badge detection source is out of range")
    if not 0 <= confidence <= 1:
        raise SerialTransportError(
            "badge detection confidence is out of range"
        )
    if not 0 <= threat_score <= 100:
        raise SerialTransportError(
            "badge detection threat score is out of range"
        )
    if not -128 <= rssi <= 127:
        raise SerialTransportError("badge detection RSSI is out of range")
    return detection


@dataclass(frozen=True)
class PartitionEntry:
    label: str
    type: int
    subtype: int
    offset: int
    size: int
    flags: int


@dataclass(frozen=True)
class RomFlashRegion:
    offset: int
    path: Path
    data: bytes = field(repr=False)
    size: int
    sha256: str


@dataclass(frozen=True)
class UplinkRomLayout:
    build_dir: Path
    version: str
    regions: tuple[RomFlashRegion, ...]
    partitions: tuple[PartitionEntry, ...]


@dataclass(frozen=True)
class RomDeviceIdentity:
    base_mac: str
    port: str
    chip: str
    revision: str
    flash_size: str
    psram_size: str


@dataclass(frozen=True)
class EsptoolWriteReceipt:
    offset: int
    size: int
    compressed_size: int


@dataclass(frozen=True)
class EsptoolVerifyReceipt:
    offset: int
    size: int
    path: Path


@dataclass(frozen=True, slots=True)
class RomFlashStageEvidence:
    """ROM-stage evidence only; application health still requires USB proof."""

    descriptor: UsbDescriptorRecord
    base_mac: str
    layout_version: str
    aggregate_sha256: str
    probe: RomIdentityEvidence
    write: RomOperationEvidence
    verify: RomOperationEvidence
    run: RomOperationEvidence
    application_health_verified: bool = field(default=False, init=False)
    rollback_cleared: bool = field(default=False, init=False)


@dataclass(frozen=True)
class _PostUplinkExpectation:
    """Immutable receipt-derived claim that later USB proof must satisfy."""

    expected_hardware_id: str
    expected_version: str
    expected_partition: str
    expected_sha256: str
    expected_size: int
    pre_version: str | None
    pre_partition: str | None
    mutation_expected: bool
    source: str
    update_session: str


@dataclass(frozen=True, init=False)
class PostUplinkApplicationEvidence:
    """Verifier-issued application USB proof after an uplink transition."""

    hardware_id: str
    version: str
    running_partition: str
    responses_completed: int
    application_health_verified: bool
    rollback_cleared: bool

    def __new__(cls, *_args: Any, **_kwargs: Any) -> PostUplinkApplicationEvidence:
        raise TypeError(
            "PostUplinkApplicationEvidence is verifier-issued only"
        )


@dataclass(frozen=True, init=False, eq=False, slots=True)
class UsbScannerFlowResult:
    """Production-issued proof bundle for one completed scanner USB flow."""

    def __new__(cls, *_args: Any, **_kwargs: Any) -> UsbScannerFlowResult:
        raise TypeError("UsbScannerFlowResult is production-issued only")

    @property
    def pre_stage_status(self) -> dict[str, Any]:
        """Return a defensive copy of the status captured before staging."""
        return _usb_scanner_flow_result_value(self, "pre_stage_status")

    @property
    def final_restored_status(self) -> dict[str, Any]:
        """Return a defensive copy of the validated restored-theme status."""
        return _usb_scanner_flow_result_value(
            self, "final_restored_status"
        )

    @property
    def stage_receipt(self) -> dict[str, Any]:
        """Return a defensive copy of the validated scanner-stage receipt."""
        return _usb_scanner_flow_result_value(self, "stage_receipt")

    @property
    def stage_receipts(self) -> tuple[dict[str, Any], ...]:
        """Return defensive copies of all ordered stage receipts."""
        return _usb_scanner_flow_result_value(self, "stage_receipts")

    @property
    def attempt_history(self) -> tuple[dict[str, Any], ...]:
        """Return defensive copies of bounded campaign attempt evidence."""
        return _usb_scanner_flow_result_value(self, "attempt_history")

    @property
    def preflight_older_slots(self) -> frozenset[str]:
        return _usb_scanner_flow_result_value(
            self, "preflight_older_slots"
        )

    @property
    def recovery_slots(self) -> frozenset[str]:
        return _usb_scanner_flow_result_value(self, "recovery_slots")

    @property
    def stage_count(self) -> int:
        return _usb_scanner_flow_result_value(self, "stage_count")

    @property
    def theme_restored(self) -> bool:
        return _usb_scanner_flow_result_value(self, "theme_restored")

    @property
    def fresh_usb_proven(self) -> bool:
        return _usb_scanner_flow_result_value(self, "fresh_usb_proven")


BADGE_THEME_KEYS = frozenset({
    "version", "palette", "background", "brightness", "accents",
})
BADGE_THEME_ACCENT_ORDER = (
    "drone", "meta", "tracker", "flock", "wifi_attack", "clear",
)
BADGE_THEME_PALETTES = frozenset({"field", "night", "neon", "mono"})
BADGE_THEME_BACKGROUNDS = frozenset({"dark", "dim", "scanline"})
BADGE_THEME_ACK_KEYS = frozenset({
    "message", "theme_hash", "persisted", "reboot_required",
})


@dataclass(frozen=True)
class _BadgeThemeSnapshot:
    """Exact, immutable copy of the reversible USB-control test state."""

    version: int
    palette: str
    background: str
    brightness: int
    accents: tuple[int, ...]
    theme_hash: int


@dataclass(frozen=True)
class _PrivatePathBinding:
    device: int
    inode: int
    mode: int
    links: int
    uid: int
    gid: int
    size: int
    mtime_ns: int
    ctime_ns: int


@dataclass(frozen=True)
class _PrivateRomFile:
    name: str
    path: Path
    binding: _PrivatePathBinding
    sha256: str


@dataclass(frozen=True)
class _PrivateRomSnapshot:
    root: Path
    parent_fd: int
    root_fd: int
    parent_binding: _PrivatePathBinding
    root_binding: _PrivatePathBinding
    files: tuple[_PrivateRomFile, ...]


UPLINK_ROM_FLASH_SIZE = 8 * 1024 * 1024
UPLINK_ROM_TARGET = "uplink-s3-fof_badge"
UPLINK_ROM_PROJECT = "fof_badge_uplink"
UPLINK_ROM_HARDWARE = "seeed_xiao_esp32s3"
UPLINK_ROM_PARTITIONS = (
    PartitionEntry("nvs", 1, 2, 0x9000, 0x6000, 0),
    PartitionEntry("otadata", 1, 0, 0xF000, 0x2000, 0),
    PartitionEntry("phy_init", 1, 1, 0x11000, 0x1000, 0),
    PartitionEntry("ota_0", 0, 0x10, 0x20000, 0x200000, 0),
    PartitionEntry("ota_1", 0, 0x11, 0x220000, 0x200000, 0),
    PartitionEntry("fw_scanner_s3", 1, 0x40, 0x420000, 0x200000, 0),
    PartitionEntry("storage", 1, 0x82, 0x620000, 0x100000, 0),
    PartitionEntry("reserved", 1, 0x81, 0x720000, 0xE0000, 0),
)
UPLINK_ROM_BUILD_SNAPSHOT_LIMITS = (
    ("bootloader.bin", 0x8000),
    ("partitions.bin", 0x1000),
    ("ota_data_initial.bin", 0x2000),
    ("firmware.bin", 0x200000),
    ("flash_args", 0x10000),
    ("flash_app_args", 0x10000),
    ("flash_project_args", 0x10000),
    ("flasher_args.json", 0x10000),
    ("bootloader/bootloader.bin", 0x8000),
    ("partition_table/partition-table.bin", 0x1000),
    ("fof_badge_uplink.bin", 0x200000),
)
UPLINK_ROM_PARTITION_SOURCE_LIMIT = 0x10000
UPLINK_ROM_REGION_OFFSETS = (0x0, 0x8000, 0xF000, 0x20000)
UPLINK_ROM_PRIVATE_NAMES = (
    "00-bootloader.bin",
    "01-partitions.bin",
    "02-ota-data-initial.bin",
    "03-firmware.bin",
)
ESPTOOL_TIMEOUT_MAX_S = 600.0
ESPTOOL_RETRY_MARKER = "Lost connection, retrying..."
ESPTOOL_VERSION_LINE = "esptool.py v4.11.0"
ESPTOOL_PROBE_UNAVAILABLE_EXIT = 73
ESPTOOL_PROBE_UNAVAILABLE_MARKER = "__FOF_ROM_PROBE_NO_SERIAL__"
ESPTOOL_APPLICATION_PROTOCOL_NOISE = (
    "Invalid head of packet (0x46): Possible serial noise or corruption.",
    "Invalid head of packet (0x49): Possible serial noise or corruption.",
    "Invalid head of packet (0x65): Possible serial noise or corruption.",
)
ROM_CENSUS_ABSENCE_PROOFS = 3
ESPTOOL_NO_RESET_WARNING = (
    'WARNING: Pre-connection option "no_reset" was selected. Connection may '
    "fail if the chip is not in bootloader or flasher stub mode."
)
ESPTOOL_GUARD = (
    "import esptool,sys;"
    "getattr(esptool,'__version__',None)=='4.11.0' or "
    "sys.exit('unsupported esptool version');"
    "import esptool.loader;"
    "from esptool.loader import ESPLoader;"
    "from esptool.targets.esp32s3 import "
    "ESP32S3ROM,ESP32S3StubLoader;"
    "esptool.loader.WRITE_BLOCK_ATTEMPTS=1;"
    "ESPLoader.WRITE_FLASH_ATTEMPTS="
    "ESP32S3ROM.WRITE_FLASH_ATTEMPTS="
    "ESP32S3StubLoader.WRITE_FLASH_ATTEMPTS=1;"
    "esptool._main()"
)
ESPTOOL_PROBE_GUARD = (
    "import esptool,sys;"
    "getattr(esptool,'__version__',None)=='4.11.0' or "
    "sys.exit('unsupported esptool version');"
    "from esptool.config import load_config_file;"
    "load_config_file()[1] is None or "
    "sys.exit('esptool config is forbidden');"
    "from esptool.loader import ESPLoader;"
    "from esptool.util import FatalError;"
    "exec(\"def _fof_probe_connect_attempt(self,reset_strategy,"
    "mode='default_reset'):\\n"
    " if mode not in ('no_reset','usb_reset'):\\n"
    "  raise FatalError('FOF ROM probe requires no_reset or usb_reset')\\n"
    " if mode=='usb_reset':\\n"
    "  reset_strategy()\\n"
    " self.flush_input()\\n"
    " self._port.flushOutput()\\n"
    " try:\\n"
    "  self.sync()\\n"
    "  return None\\n"
    " except FatalError as error:\\n"
    "  message=str(error)\\n"
    "  if message=='No serial data received.' or message in "
    f"{ESPTOOL_APPLICATION_PROTOCOL_NOISE!r}:\\n"
    f"   print('\\\\n{ESPTOOL_PROBE_UNAVAILABLE_MARKER}')\\n"
    f"   raise SystemExit({ESPTOOL_PROBE_UNAVAILABLE_EXIT})\\n"
    "  return error\");"
    "ESPLoader._connect_attempt=_fof_probe_connect_attempt;"
    "esptool._main()"
)
_ANSI_ESCAPE_RE = re.compile(
    r"\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])"
)
_ESPTOOL_MAC_RE = re.compile(
    r"^MAC: ((?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2})$"
)
_ESPTOOL_CHIP_RE = re.compile(
    r"^Chip is (ESP32-S3)(?: \([^()]+\))? "
    r"\(revision (v[0-9]+(?:\.[0-9]+)*)\)$"
)
_ESPTOOL_WROTE_RE = re.compile(
    r"^Wrote ([0-9]{1,10}) bytes \(([0-9]{1,10}) compressed\) at "
    r"(0x[0-9A-Fa-f]{1,8}) in [0-9]{1,10}(?:\.[0-9]{1,10})? seconds"
    r"(?: \([^\r\n]*\))?\.\.\.$"
)
_ESPTOOL_VERIFY_RE = re.compile(
    r"^Verifying (0x[0-9A-Fa-f]{1,8}) \(([0-9]{1,10})\) bytes @ "
    r"(0x[0-9A-Fa-f]{1,8}) in flash against (.+)\.\.\.$"
)
def require_usb_firmware_transport(transport: str) -> None:
    if transport != "usb":
        raise FlashError(
            "badge firmware transport is USB to the uplink plus UART to the "
            "scanners; HTTP/AP/LAN firmware mutation is disabled"
        )


def log(msg: object) -> None:
    print_user_visible(msg, flush=True)


def scanner_relay_timeout_s(size: int) -> int:
    """Conservative wall-clock timeout for one scanner UART relay."""
    estimate = int((max(size, 0) / 1024.0) * SCANNER_RELAY_TIMEOUT_PER_KB_S)
    return max(SCANNER_RELAY_TIMEOUT_MIN_S,
               min(SCANNER_RELAY_TIMEOUT_MAX_S, estimate))


def format_relay_progress(progress: dict[str, Any]) -> str:
    slot = progress.get("uart") or progress.get("slot") or "scanner"
    stage = progress.get("stage") or "relay"
    pct = progress.get("percent")
    if pct is None:
        size = progress.get("size") or progress.get("total") or 0
        got = progress.get("bytes") or progress.get("received") or 0
        pct = int((int(got) * 100) / int(size)) if size else 0
    details = [
        f"[relay] {slot} {stage} {pct}%",
        f"{progress.get('bytes', progress.get('received', 0))}/{progress.get('size', progress.get('total', 0))}",
        f"chunks={progress.get('chunks', 0)}",
        f"nacks={progress.get('nacks', 0)}",
        f"retries={progress.get('retries', 0)}",
        f"elapsed={progress.get('elapsed_s', 0)}s",
    ]
    error = progress.get("error")
    if error:
        details.append(f"error={error}")
    return " ".join(str(part) for part in details)


def find_pio() -> str:
    candidates = [
        os.environ.get("PIO"),
        shutil.which("pio"),
        str(ESP32_DIR / ".venv312/bin/pio"),
        str(Path.home() / ".platformio/penv/bin/pio"),
    ]
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            return candidate
    raise FlashError("PlatformIO not found; set PIO or install PlatformIO")


def find_platformio_python() -> str:
    candidates = [
        os.environ.get("PIO_PYTHON"),
        str(Path.home() / ".platformio/penv/bin/python"),
        sys.executable,
    ]
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            return candidate
    return sys.executable


def _esptool_lines(transcript: str) -> list[str]:
    if not isinstance(transcript, str):
        raise FlashError("esptool transcript must be text")
    normalized = _ANSI_ESCAPE_RE.sub("", transcript)
    normalized = normalized.replace("\r\n", "\n").replace("\r", "\n")
    return [line.strip() for line in normalized.split("\n") if line.strip()]


def _reject_esptool_failure_markers(transcript: str) -> list[str]:
    lines = _esptool_lines(transcript)
    if any(ESPTOOL_RETRY_MARKER in line for line in lines):
        raise FlashError(
            "esptool attempted an internal reconnect; ROM operation state is "
            "uncertain"
        )
    if any(re.search(r"\bFAILED\b", line, re.IGNORECASE) for line in lines):
        raise FlashError("esptool transcript contains a failed operation")
    return lines


def _normalized_rom_mac(value: Any, *, label: str) -> str:
    rendered = str(value or "").strip().lower()
    if not _HARDWARE_ID_RE.fullmatch(rendered):
        raise FlashError(f"invalid {label} MAC: {value!r}")
    return rendered


def _validated_rom_port(value: Any) -> str:
    if type(value) is not str or value != value.strip() or not value or \
            "\x00" in value or "\n" in value or "\r" in value or \
            value.startswith("-") or not os.path.isabs(value) or \
            os.path.normpath(value) != value or not value.startswith("/dev/"):
        raise FlashError(f"invalid ROM serial port: {value!r}")
    return value


def _parse_esptool_int(token: str, base: int, *, label: str) -> int:
    try:
        return int(token, base)
    except (ValueError, OverflowError) as exc:
        raise FlashError(f"invalid numeric {label} in esptool receipt") from exc


def _validate_esptool_version_lines(lines: list[str]) -> None:
    candidates = [line for line in lines if line.startswith("esptool.py v")]
    if candidates != [ESPTOOL_VERSION_LINE]:
        raise FlashError(
            "esptool transcript must contain exactly one supported version "
            f"line ({ESPTOOL_VERSION_LINE})"
        )


def _operation_mac_from_lines(lines: list[str]) -> str:
    candidates = [line for line in lines if line.startswith("MAC")]
    if len(candidates) != 1:
        raise FlashError(
            "esptool transcript must contain exactly one base MAC line"
        )
    match = _ESPTOOL_MAC_RE.fullmatch(candidates[0])
    if not match:
        raise FlashError(f"malformed esptool MAC line: {candidates[0]!r}")
    return _normalized_rom_mac(match.group(1), label="ROM base")


def verify_esptool_operation_mac(transcript: str,
                                  expected_mac: str) -> str:
    lines = _reject_esptool_failure_markers(transcript)
    _validate_esptool_version_lines(lines)
    got = _operation_mac_from_lines(lines)
    wanted = _normalized_rom_mac(expected_mac, label="expected ROM base")
    if got != wanted:
        raise FlashError(
            f"esptool device changed: got base MAC {got}, wanted {wanted}"
        )
    return got


def parse_esptool_rom_identity(transcript: str,
                               port: str) -> RomDeviceIdentity:
    lines = _reject_esptool_failure_markers(transcript)
    _validate_esptool_version_lines(lines)
    port = _validated_rom_port(port)

    serial_lines = [line for line in lines if line.startswith("Serial port")]
    if serial_lines != [f"Serial port {port}"]:
        raise FlashError(
            "esptool probe must contain exactly one Serial port line for "
            f"the selected port {port!r}"
        )

    chip_lines = [line for line in lines if line.startswith("Chip is")]
    if len(chip_lines) != 1:
        raise FlashError(
            "esptool transcript must contain exactly one chip identity line"
        )
    chip_match = _ESPTOOL_CHIP_RE.fullmatch(chip_lines[0])
    if not chip_match:
        raise FlashError(
            f"unsupported or malformed ESP32-S3 identity: {chip_lines[0]!r}"
        )

    psram_lines = [line for line in lines if "PSRAM" in line]
    if len(psram_lines) != 1:
        raise FlashError(
            "esptool transcript must contain exactly one PSRAM identity line"
        )
    features_prefix = "Features: "
    psram_features = (
        [part.strip() for part in psram_lines[0][len(features_prefix):].split(",")]
        if psram_lines[0].startswith(features_prefix) else []
    )
    psram_tokens = [part for part in psram_features if "PSRAM" in part]
    if len(psram_tokens) != 1 or not re.fullmatch(
        r"Embedded PSRAM 8MB(?: \([^()]+\))?", psram_tokens[0]
    ):
        raise FlashError(
            f"ROM target must report Embedded PSRAM 8MB: {psram_lines[0]!r}"
        )

    flash_lines = [
        line for line in lines if line.startswith("Detected flash size:")
    ]
    if len(flash_lines) != 1:
        raise FlashError(
            "esptool transcript must contain exactly one flash-size line"
        )
    if flash_lines[0] != "Detected flash size: 8MB":
        raise FlashError(
            f"ROM target must report 8MB flash: {flash_lines[0]!r}"
        )

    return RomDeviceIdentity(
        base_mac=_operation_mac_from_lines(lines),
        port=port,
        chip=chip_match.group(1),
        revision=chip_match.group(2),
        flash_size="8MB",
        psram_size="8MB",
    )


def parse_esptool_run_result(transcript: str, expected_mac: str) -> str:
    """Bind the sticky-download clear and watchdog handoff to one device."""
    operation_mac = verify_esptool_operation_mac(transcript, expected_mac)
    lines = _esptool_lines(transcript)
    clear_receipt = "Wrote 00000000, mask 00000001 to 6000812c"
    reset_receipt = "Hard resetting with a watchdog..."
    if lines.count(clear_receipt) != 1:
        raise FlashError(
            "ROM application handoff must prove one exact force-download clear"
        )
    if lines.count(reset_receipt) != 1 or lines[-1] != reset_receipt:
        raise FlashError(
            "ROM application handoff must end with one watchdog reset"
        )
    if lines.index(clear_receipt) >= lines.index(reset_receipt):
        raise FlashError(
            "ROM application handoff reset preceded its force-download clear"
        )
    return operation_mac


def _validate_rom_regions(
    regions: tuple[RomFlashRegion, ...] | list[RomFlashRegion],
) -> tuple[RomFlashRegion, ...]:
    frozen = tuple(regions)
    if len(frozen) != len(UPLINK_ROM_REGION_OFFSETS):
        raise FlashError(
            "ROM flash requires exactly four verified snapshot regions"
        )
    if any(type(region) is not RomFlashRegion for region in frozen):
        raise FlashError("ROM flash region evidence has an invalid type")
    if any(type(region.offset) is not int for region in frozen):
        raise FlashError("ROM flash region offsets must be exact integers")
    offsets = tuple(region.offset for region in frozen)
    if offsets != UPLINK_ROM_REGION_OFFSETS:
        rendered = ", ".join(f"{offset:#x}" for offset in offsets)
        raise FlashError(
            "ROM snapshot offsets must be exactly "
            "0x0, 0x8000, 0xf000, 0x20000 in order; got " + rendered
        )
    for region in frozen:
        if type(region.data) is not bytes:
            raise FlashError(
                f"ROM snapshot at {region.offset:#x} is not immutable bytes"
            )
        if type(region.size) is not int or region.size <= 0:
            raise FlashError(
                f"ROM snapshot at {region.offset:#x} has invalid size"
            )
        if len(region.data) != region.size:
            raise FlashError(
                f"ROM snapshot at {region.offset:#x} size metadata changed"
            )
        digest = hashlib.sha256(region.data).hexdigest()
        if type(region.sha256) is not str or not re.fullmatch(
            r"[0-9a-f]{64}", region.sha256
        ) or \
                digest != region.sha256:
            raise FlashError(
                f"ROM snapshot at {region.offset:#x} digest metadata changed"
            )
    return frozen


def _normalize_snapshot_paths(
    regions: tuple[RomFlashRegion, ...] | list[RomFlashRegion],
    snapshot_paths: tuple[Path, ...] | list[Path],
) -> tuple[tuple[RomFlashRegion, ...], tuple[Path, ...]]:
    frozen = _validate_rom_regions(regions)
    supplied = tuple(snapshot_paths)
    if len(supplied) != len(frozen):
        raise FlashError(
            "one private snapshot display path is required per ROM region"
        )
    normalized: list[Path] = []
    for path in supplied:
        try:
            rendered = os.fspath(path)
        except TypeError as exc:
            raise FlashError(f"invalid ROM snapshot path: {path!r}") from exc
        if not rendered or "\x00" in rendered:
            raise FlashError(f"invalid ROM snapshot path: {path!r}")
        if not os.path.isabs(rendered) or os.path.normpath(rendered) != rendered:
            raise FlashError(
                "ROM snapshot display path must already be normalized and "
                f"absolute: {rendered!r}"
            )
        normalized.append(Path(rendered))
    if len(set(normalized)) != len(normalized):
        raise FlashError("ROM snapshot display paths must be unique")
    return frozen, tuple(normalized)


def _esptool_common_argv(port: str, baud: int, *,
                         no_stub: bool,
                         guard: str = ESPTOOL_GUARD,
                         before: str = "no_reset",
                         after: str = "no_reset") -> list[str]:
    port = _validated_rom_port(port)
    argv = [
        find_platformio_python(), "-I", "-c", guard,
        "--chip", "esp32s3", "--port", port, "--baud", str(baud),
        "--before", before, "--after", after,
    ]
    if no_stub:
        argv.append("--no-stub")
    argv.extend(("--connect-attempts", "1"))
    return argv


def build_esptool_probe_argv(
    port: str, *, native_usb_reset: bool = False,
) -> list[str]:
    if type(native_usb_reset) is not bool:
        raise FlashError("native USB reset selection must be an exact boolean")
    return _esptool_common_argv(
        port, 115200, no_stub=True, guard=ESPTOOL_PROBE_GUARD,
        before="usb_reset" if native_usb_reset else "no_reset",
    ) + ["flash_id"]


def _esptool_region_pairs(
    regions: tuple[RomFlashRegion, ...] | list[RomFlashRegion],
    snapshot_paths: tuple[Path, ...] | list[Path],
) -> list[str]:
    frozen, paths = _normalize_snapshot_paths(regions, snapshot_paths)
    pairs: list[str] = []
    for region, path in zip(frozen, paths):
        pairs.extend((f"0x{region.offset:x}", str(path)))
    return pairs


def build_esptool_write_argv(
    port: str,
    regions: tuple[RomFlashRegion, ...] | list[RomFlashRegion],
    snapshot_paths: tuple[Path, ...] | list[Path],
) -> list[str]:
    return _esptool_common_argv(port, 460800, no_stub=False) + [
        "write_flash", "--compress", "--verify",
        "--flash_mode", "dio", "--flash_freq", "80m",
        "--flash_size", "8MB",
        *_esptool_region_pairs(regions, snapshot_paths),
    ]


def build_esptool_verify_argv(
    port: str,
    regions: tuple[RomFlashRegion, ...] | list[RomFlashRegion],
    snapshot_paths: tuple[Path, ...] | list[Path],
) -> list[str]:
    return _esptool_common_argv(port, 460800, no_stub=False) + [
        "verify_flash", "--flash_mode", "dio", "--flash_freq", "80m",
        "--flash_size", "8MB",
        *_esptool_region_pairs(regions, snapshot_paths),
    ]


def build_esptool_run_argv(port: str) -> list[str]:
    return _esptool_common_argv(
        port, 115200, no_stub=True, after="watchdog_reset"
    ) + list(ROM_APP_HANDOFF_OPERATION)


def _validate_guarded_esptool_argv(argv: tuple[str, ...]) -> None:
    expected_prefix = (find_platformio_python(), "-I", "-c")
    if type(argv) is not tuple or argv[:3] != expected_prefix or \
            len(argv) < 4 or argv[3] not in (
                ESPTOOL_GUARD, ESPTOOL_PROBE_GUARD,
            ) or \
            not all(type(value) is str and "\x00" not in value
                    for value in argv):
        raise FlashError(
            "refusing non-guarded esptool command; -m esptool and alternate "
            "entrypoints are disabled"
        )
    guard = argv[3]
    tail = argv[4:]
    fixed = (
        "--chip", "esp32s3", "--port",
    )
    if tail[:3] != fixed or len(tail) < 13:
        raise FlashError("guarded esptool command has an invalid common prefix")
    port = tail[3]
    _validated_rom_port(port)
    if tail[4] != "--baud" or tail[6] != "--before" or \
            tail[8] != "--after":
        raise FlashError(
            "guarded esptool command must use explicit reset controls"
        )
    baud = tail[5]
    before = tail[7]
    after = tail[9]
    cursor = 10
    no_stub = tail[cursor:cursor + 1] == ("--no-stub",)
    if no_stub:
        cursor += 1
    if tail[cursor:cursor + 2] != ("--connect-attempts", "1"):
        raise FlashError(
            "guarded esptool command requires exactly one connection attempt"
        )
    cursor += 2
    operation = tail[cursor:]
    if operation == ("flash_id",):
        if baud != "115200" or not no_stub:
            raise FlashError(
                "ROM probe command requires 115200 baud and --no-stub"
            )
        if guard != ESPTOOL_PROBE_GUARD or \
                before not in ("no_reset", "usb_reset") or \
                after != "no_reset":
            raise FlashError(
                "the single-sync probe guard is required only for flash_id"
            )
        return
    if operation == ROM_APP_HANDOFF_OPERATION:
        if baud != "115200" or not no_stub or \
                guard != ESPTOOL_GUARD or before != "no_reset" or \
                after != "watchdog_reset":
            raise FlashError(
                "ROM application handoff requires one masked force-download "
                "clear followed by watchdog reset"
            )
        return
    if not operation:
        raise FlashError("guarded esptool command is missing its operation")
    operation_name = operation[0]
    if operation_name == "write_flash":
        expected_options = (
            "write_flash", "--compress", "--verify",
            "--flash_mode", "dio", "--flash_freq", "80m",
            "--flash_size", "8MB",
        )
    elif operation_name == "verify_flash":
        expected_options = (
            "verify_flash", "--flash_mode", "dio", "--flash_freq", "80m",
            "--flash_size", "8MB",
        )
    else:
        raise FlashError(
            f"unsupported guarded esptool operation: {operation_name!r}"
        )
    if guard != ESPTOOL_GUARD:
        raise FlashError(
            "write/verify operations require the mutation retry guard"
        )
    if baud != "460800" or no_stub or before != "no_reset" or \
            after != "no_reset" or \
            operation[:len(expected_options)] != expected_options:
        raise FlashError(
            f"guarded {operation_name} command has unsafe or missing options"
        )
    pairs = operation[len(expected_options):]
    if len(pairs) != 8 or tuple(pairs[::2]) != tuple(
        f"0x{offset:x}" for offset in UPLINK_ROM_REGION_OFFSETS
    ):
        raise FlashError(
            "guarded ROM command requires exactly four canonical flash offsets"
        )
    for rendered in pairs[1::2]:
        if not os.path.isabs(rendered) or os.path.normpath(rendered) != rendered:
            raise FlashError(
                f"guarded ROM command requires a normalized absolute snapshot "
                f"path: {rendered!r}"
            )
    if len(set(pairs[1::2])) != 4:
        raise FlashError(
            "guarded ROM command requires four unique snapshot paths"
        )


def run_guarded_esptool(argv: list[str], *, timeout_s: float,
                         runner: Any = subprocess.run) -> str:
    if not isinstance(timeout_s, (int, float)) or isinstance(timeout_s, bool) \
            or not (0 < float(timeout_s) <= ESPTOOL_TIMEOUT_MAX_S):
        raise FlashError(
            f"esptool timeout must be in (0, {ESPTOOL_TIMEOUT_MAX_S:g}] seconds"
        )
    if type(argv) is not list:
        raise FlashError("guarded esptool argv must be an exact built-in list")
    frozen_argv = tuple(argv)
    _validate_guarded_esptool_argv(frozen_argv)
    is_probe = (
        frozen_argv[3] == ESPTOOL_PROBE_GUARD
        and frozen_argv[-1] == "flash_id"
    )
    selected_port = frozen_argv[frozen_argv.index("--port") + 1]
    env = {
        key: value for key, value in os.environ.items()
        if not key.startswith("PYTHON") and not key.startswith("ESPTOOL_")
    }
    env["ESPTOOL_OPEN_PORT_ATTEMPTS"] = "1"
    try:
        proc = runner(
            list(frozen_argv), cwd=str(REPO_ROOT), env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=timeout_s, check=False, shell=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise FlashError(
            f"guarded esptool timed out after {timeout_s:g} seconds; ROM "
            "operation state is uncertain"
        ) from exc
    except OSError as exc:
        raise FlashError(f"could not execute guarded esptool: {exc}") from exc
    output = proc.stdout or ""
    if not isinstance(output, str):
        output = bytes(output).decode("utf-8", "replace")
    lines = _esptool_lines(output)
    if type(proc.returncode) is int and \
            proc.returncode == ESPTOOL_PROBE_UNAVAILABLE_EXIT and is_probe:
        expected = [
            ESPTOOL_VERSION_LINE,
            f"Serial port {selected_port}",
            ESPTOOL_NO_RESET_WARNING,
            "Connecting...",
            ESPTOOL_PROBE_UNAVAILABLE_MARKER,
        ]
        if lines == expected:
            raise RomProbeUnavailable(
                f"ROM probe received no serial data from {selected_port}"
            )
    if ESPTOOL_PROBE_UNAVAILABLE_MARKER in output:
        raise FlashError(
            "guarded esptool probe-unavailable attestation is malformed"
        )
    _reject_esptool_failure_markers(output)
    if proc.returncode != 0:
        raise FlashError(
            f"guarded esptool failed with exit {proc.returncode}: {output.strip()}"
        )
    _validate_esptool_version_lines(_esptool_lines(output))
    return output


def parse_esptool_write_receipts(
    transcript: str,
    regions: tuple[RomFlashRegion, ...] | list[RomFlashRegion],
    expected_mac: str,
) -> tuple[EsptoolWriteReceipt, ...]:
    frozen = _validate_rom_regions(regions)
    verify_esptool_operation_mac(transcript, expected_mac)
    lines = _reject_esptool_failure_markers(transcript)
    indices = [
        index for index, line in enumerate(lines) if line.startswith("Wrote")
    ]
    hash_lines = [line for line in lines if line == "Hash of data verified."]
    if len(indices) != len(frozen) or len(hash_lines) != len(frozen):
        raise FlashError(
            "write transcript must contain exactly four Wrote/hash receipts"
        )
    receipts: list[EsptoolWriteReceipt] = []
    for index, region in zip(indices, frozen):
        match = _ESPTOOL_WROTE_RE.fullmatch(lines[index])
        if not match:
            raise FlashError(f"malformed esptool Wrote receipt: {lines[index]!r}")
        if index + 1 >= len(lines) or \
                lines[index + 1] != "Hash of data verified.":
            raise FlashError(
                "each Wrote receipt must be immediately paired with its hash proof"
            )
        size = _parse_esptool_int(match.group(1), 10, label="write size")
        compressed_size = _parse_esptool_int(
            match.group(2), 10, label="compressed size"
        )
        offset = _parse_esptool_int(
            match.group(3), 16, label="write offset"
        )
        padded = (region.size + 3) & ~3
        if size != padded or offset != region.offset:
            raise FlashError(
                "write receipt region mismatch: got "
                f"{size} bytes at {offset:#x}, wanted {padded} at "
                f"{region.offset:#x}"
            )
        if compressed_size <= 0 or compressed_size > UPLINK_ROM_FLASH_SIZE:
            raise FlashError(
                f"invalid compressed byte count in write receipt: "
                f"{compressed_size} for {size} bytes"
            )
        receipts.append(EsptoolWriteReceipt(
            offset=offset, size=size, compressed_size=compressed_size,
        ))
    return tuple(receipts)


def parse_esptool_verify_receipts(
    transcript: str,
    regions: tuple[RomFlashRegion, ...] | list[RomFlashRegion],
    snapshot_paths: tuple[Path, ...] | list[Path],
    expected_mac: str,
) -> tuple[EsptoolVerifyReceipt, ...]:
    frozen, paths = _normalize_snapshot_paths(regions, snapshot_paths)
    verify_esptool_operation_mac(transcript, expected_mac)
    lines = _reject_esptool_failure_markers(transcript)
    indices = [
        index for index, line in enumerate(lines)
        if line.startswith("Verifying")
    ]
    digest_lines = [
        line for line in lines if line == "-- verify OK (digest matched)"
    ]
    if len(indices) != len(frozen) or len(digest_lines) != len(frozen):
        raise FlashError(
            "verify transcript must contain exactly four region/digest receipts"
        )
    receipts: list[EsptoolVerifyReceipt] = []
    for index, region, path in zip(indices, frozen, paths):
        match = _ESPTOOL_VERIFY_RE.fullmatch(lines[index])
        if not match:
            raise FlashError(
                f"malformed esptool Verifying receipt: {lines[index]!r}"
            )
        if index + 1 >= len(lines) or \
                lines[index + 1] != "-- verify OK (digest matched)":
            raise FlashError(
                "each Verifying receipt must be immediately paired with its "
                "digest proof"
            )
        hex_size = _parse_esptool_int(
            match.group(1), 16, label="verify hexadecimal size"
        )
        decimal_size = _parse_esptool_int(
            match.group(2), 10, label="verify decimal size"
        )
        offset = _parse_esptool_int(
            match.group(3), 16, label="verify offset"
        )
        rendered_path = match.group(4)
        padded = (region.size + 3) & ~3
        if hex_size != decimal_size or decimal_size != padded or \
                offset != region.offset or rendered_path != str(path):
            raise FlashError(
                "verify receipt does not exactly match the private ROM snapshot "
                f"at {region.offset:#x}"
            )
        receipts.append(EsptoolVerifyReceipt(
            offset=offset, size=decimal_size, path=path,
        ))
    return tuple(receipts)


def _revalidate_selected_rom_device(
    device: RomDeviceIdentity,
) -> RomDeviceIdentity:
    if type(device) is not RomDeviceIdentity:
        raise FlashError("selected ROM device identity has an invalid type")
    if any(type(value) is not str for value in (
        device.base_mac, device.port, device.chip, device.revision,
        device.flash_size, device.psram_size,
    )):
        raise FlashError("selected ROM device fields must be exact strings")
    normalized_mac = _normalized_rom_mac(
        device.base_mac, label="selected ROM base"
    )
    if device.base_mac != normalized_mac:
        raise FlashError("selected ROM base MAC is not normalized")
    port = _validated_rom_port(device.port)
    if device.chip != "ESP32-S3":
        raise FlashError(f"selected ROM chip is not ESP32-S3: {device.chip!r}")
    if device.flash_size != "8MB":
        raise FlashError(
            f"selected ROM flash size is not 8MB: {device.flash_size!r}"
        )
    if device.psram_size != "8MB":
        raise FlashError(
            f"selected ROM PSRAM size is not 8MB: {device.psram_size!r}"
        )
    if type(device.revision) is not str or not re.fullmatch(
        r"v[0-9]{1,2}(?:\.[0-9]{1,2}){0,2}", device.revision
    ):
        raise FlashError(
            f"selected ROM revision is malformed: {device.revision!r}"
        )
    if port != device.port:
        raise FlashError("selected ROM serial port is not normalized")
    return device


def _revalidate_retained_uplink_rom_layout(
    layout: UplinkRomLayout,
    platform: dict[str, Any],
) -> tuple[RomFlashRegion, ...]:
    """Re-prove B1 semantics using retained bytes, never source paths."""
    if type(layout) is not UplinkRomLayout:
        raise FlashError("ROM layout must be exact UplinkRomLayout evidence")
    if type(layout.regions) is not tuple or type(layout.partitions) is not tuple:
        raise FlashError("ROM layout evidence collections must be immutable tuples")
    if type(layout.version) is not str:
        raise FlashError("ROM layout version must be an exact string")
    for entry in layout.partitions:
        if type(entry) is not PartitionEntry or type(entry.label) is not str or \
                any(type(value) is not int for value in (
                    entry.type, entry.subtype, entry.offset,
                    entry.size, entry.flags,
                )):
            raise FlashError("ROM partition evidence has invalid field types")
    try:
        current_version = repo_version(platform)
    except Exception as exc:
        raise FlashError(
            f"canonical repo version cannot be read: {exc}"
        ) from exc
    if type(current_version) is not str or not \
            _ORDERED_VERSION_RE.fullmatch(current_version):
        raise FlashError(
            f"canonical repo version is invalid: {current_version!r}"
        )
    if layout.version != current_version:
        raise FlashError(
            "ROM layout version is not the current repo version: "
            f"got {layout.version!r}, wanted {current_version!r}"
        )
    if not isinstance(layout.build_dir, Path):
        raise FlashError("ROM layout build directory is not a Path")
    rendered_build = os.fspath(layout.build_dir)
    if not os.path.isabs(rendered_build) or \
            os.path.normpath(rendered_build) != rendered_build:
        raise FlashError("ROM layout build directory is not normalized absolute")

    regions = _validate_rom_regions(layout.regions)
    expected_names = (
        "bootloader.bin", "partitions.bin",
        "ota_data_initial.bin", "firmware.bin",
    )
    for region, name in zip(regions, expected_names):
        if not isinstance(region.path, Path) or \
                region.path != layout.build_dir / name:
            raise FlashError(
                f"ROM retained region path metadata drift at {region.offset:#x}"
            )

    bootloader, partition_image, ota_data, firmware = (
        region.data for region in regions
    )
    if len(bootloader) > 0x8000:
        raise FlashError("retained bootloader exceeds 0x8000")
    if 0x8000 + len(partition_image) > 0x9000:
        raise FlashError("retained partition image exceeds 0x9000")
    if len(ota_data) != 0x2000 or ota_data != b"\xFF" * 0x2000:
        raise FlashError("retained OTA data must be exactly 0x2000 all 0xFF")
    if len(firmware) > 0x200000:
        raise FlashError("retained firmware exceeds ota_0 capacity")

    parsed_partitions = _decode_partition_table_bytes(
        partition_image, regions[1].path
    )
    _validate_uplink_partitions(parsed_partitions)
    if parsed_partitions != UPLINK_ROM_PARTITIONS or \
            layout.partitions != UPLINK_ROM_PARTITIONS or \
            layout.partitions != parsed_partitions:
        raise FlashError("retained ROM partition evidence drift")
    ota_0 = next(
        entry for entry in parsed_partitions if entry.label == "ota_0"
    )
    if ota_0.offset != 0x20000 or len(firmware) > ota_0.size:
        raise FlashError("retained firmware does not fit canonical ota_0")

    _validate_esp32_s3_image(
        bootloader, regions[0].path, "retained bootloader", flash_offset=0
    )
    _validate_esp32_s3_image(
        firmware, regions[3].path, "retained firmware",
        flash_offset=0x20000,
    )
    _validate_firmware_bytes(
        firmware, regions[3].path,
        target=UPLINK_ROM_TARGET,
        project=UPLINK_ROM_PROJECT,
        hardware=UPLINK_ROM_HARDWARE,
        version=current_version,
    )
    return regions


def _private_path_binding(info: os.stat_result) -> _PrivatePathBinding:
    return _PrivatePathBinding(
        device=info.st_dev,
        inode=info.st_ino,
        mode=info.st_mode,
        links=info.st_nlink,
        uid=info.st_uid,
        gid=info.st_gid,
        size=info.st_size,
        mtime_ns=info.st_mtime_ns,
        ctime_ns=info.st_ctime_ns,
    )


def _same_directory_identity(
    info: os.stat_result, binding: _PrivatePathBinding,
) -> bool:
    return stat.S_ISDIR(info.st_mode) and \
        info.st_dev == binding.device and info.st_ino == binding.inode and \
        info.st_uid == binding.uid and info.st_gid == binding.gid and \
        stat.S_IMODE(info.st_mode) == stat.S_IMODE(binding.mode)


def _same_cleanup_directory_object(
    info: os.stat_result, binding: _PrivatePathBinding,
) -> bool:
    """Bind cleanup to one owned directory object, ignoring mutable metadata."""
    return stat.S_ISDIR(info.st_mode) and \
        info.st_dev == binding.device and info.st_ino == binding.inode and \
        info.st_uid == binding.uid and info.st_gid == binding.gid


def _same_private_file_object(
    info: os.stat_result, binding: _PrivatePathBinding,
) -> bool:
    return stat.S_ISREG(info.st_mode) and \
        info.st_dev == binding.device and info.st_ino == binding.inode and \
        info.st_uid == binding.uid and info.st_gid == binding.gid


def _add_secondary_failure_note(
    primary: BaseException, secondary: BaseException, *, scope: str,
) -> None:
    add_note = getattr(primary, "add_note", None)
    if callable(add_note):
        add_note(f"{scope} also failed: {secondary}")


def _close_bound_private_file_descriptor(
    fd: int, binding: _PrivatePathBinding | None, *, label: str,
) -> FlashError | None:
    """Fail closed on close errors; retry only while fd is still our inode."""
    if binding is None:
        return FlashError(
            f"{label} close refused: descriptor binding is unavailable"
        )
    try:
        before_close = os.fstat(fd)
    except BaseException as probe_error:
        failure = FlashError(
            f"{label} close refused: descriptor cannot be revalidated"
        )
        failure.__cause__ = probe_error
        return failure
    if not _same_private_file_object(before_close, binding):
        return FlashError(
            f"{label} close refused: descriptor binding changed"
        )
    try:
        os.close(fd)
        return None
    except BaseException as first:
        failure = FlashError(
            f"{label} close failed: {str(first) or type(first).__name__}"
        )
        failure.__cause__ = first

    try:
        current = os.fstat(fd)
    except OSError:
        return failure
    except BaseException as probe_error:
        _add_secondary_failure_note(
            failure, probe_error, scope=f"{label} close recovery probe"
        )
        return failure
    if not _same_private_file_object(current, binding):
        _add_secondary_failure_note(
            failure,
            FlashError("descriptor number was reused; left untouched"),
            scope=f"{label} close recovery",
        )
        return failure
    try:
        os.close(fd)
    except BaseException as retry_error:
        _add_secondary_failure_note(
            failure, retry_error, scope=f"{label} close retry"
        )
    return failure


def _private_open_flags(*, directory: bool = False) -> int:
    nofollow = getattr(os, "O_NOFOLLOW", None)
    if nofollow is None:
        raise FlashError("private ROM snapshot requires O_NOFOLLOW")
    flags = nofollow | os.O_NONBLOCK | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOCTTY", 0)
    if directory:
        directory_flag = getattr(os, "O_DIRECTORY", None)
        if directory_flag is None:
            raise FlashError("private ROM snapshot requires O_DIRECTORY")
        flags |= os.O_RDONLY | directory_flag
    return flags


def _require_private_directory_descriptor(
    fd: int, binding: _PrivatePathBinding, *, label: str,
) -> os.stat_result:
    try:
        info = os.fstat(fd)
    except OSError as exc:
        raise FlashError(f"{label} descriptor is unavailable: {exc}") from exc
    if not _same_directory_identity(info, binding):
        raise FlashError(f"{label} descriptor binding changed")
    return info


def _create_private_rom_file(
    root_fd: int, root: Path, name: str, region: RomFlashRegion,
    *, expected_root_binding: _PrivatePathBinding,
) -> _PrivateRomFile:
    _require_private_directory_descriptor(
        root_fd, expected_root_binding, label="private ROM snapshot root"
    )
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | \
        _private_open_flags()
    try:
        fd = os.open(name, flags, 0o400, dir_fd=root_fd)
    except OSError as exc:
        raise FlashError(
            f"cannot create private ROM snapshot {name}: {exc}"
        ) from exc
    close_binding: _PrivatePathBinding | None = None
    binding: _PrivatePathBinding | None = None
    primary: BaseException | None = None
    try:
        _require_private_directory_descriptor(
            root_fd, expected_root_binding,
            label="private ROM snapshot root",
        )
        opened = os.fstat(fd)
        path_opened = os.stat(
            name, dir_fd=root_fd, follow_symlinks=False
        )
        if not stat.S_ISREG(opened.st_mode) or opened.st_nlink != 1 or \
                opened.st_uid != os.geteuid() or \
                opened.st_gid != os.getegid() or \
                _private_path_binding(path_opened) != \
                _private_path_binding(opened):
            raise FlashError(
                f"private ROM snapshot descriptor is unsafe: {name}"
            )
        close_binding = _private_path_binding(opened)
        view = memoryview(region.data)
        written = 0
        while written < len(view):
            try:
                count = os.write(fd, view[written:])
            except InterruptedError:
                continue
            if count <= 0:
                raise FlashError(
                    f"short write creating private ROM snapshot {name}"
                )
            written += count
        os.fsync(fd)
        os.fchmod(fd, 0o400)
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or \
                info.st_mode & 0o777 != 0o400 or \
                info.st_uid != os.geteuid() or info.st_gid != os.getegid() or \
                info.st_size != region.size:
            raise FlashError(
                f"private ROM snapshot metadata is unsafe: {name}"
            )
        binding = _private_path_binding(info)
    except OSError as exc:
        primary = FlashError(
            f"cannot complete private ROM snapshot {name}: {exc}"
        )
        primary.__cause__ = exc
    except BaseException as exc:
        primary = exc

    close_error = _close_bound_private_file_descriptor(
        fd, close_binding, label=f"private ROM snapshot {name}"
    )
    if primary is not None:
        if close_error is not None:
            _add_secondary_failure_note(
                primary, close_error,
                scope=f"private ROM snapshot {name}",
            )
        raise primary
    if close_error is not None:
        raise close_error
    if binding is None:
        raise FlashError(
            f"private ROM snapshot {name} has no final binding"
        )
    _require_private_directory_descriptor(
        root_fd, expected_root_binding, label="private ROM snapshot root"
    )
    try:
        path_info = os.stat(name, dir_fd=root_fd, follow_symlinks=False)
    except OSError as exc:
        raise FlashError(
            f"private ROM snapshot vanished after creation: {name}: {exc}"
        ) from exc
    if _private_path_binding(path_info) != binding:
        raise FlashError(
            f"private ROM snapshot changed after creation: {name}"
        )
    return _PrivateRomFile(
        name=name,
        path=root / name,
        binding=binding,
        sha256=region.sha256,
    )


def _add_cleanup_failure_note(
    primary: BaseException, cleanup: BaseException, *, scope: str,
) -> None:
    add_note = getattr(primary, "add_note", None)
    if callable(add_note):
        add_note(f"{scope} cleanup also failed: {cleanup}")


def _cleanup_private_rom_snapshot(snapshot: _PrivateRomSnapshot) -> None:
    errors: list[str] = []
    recorded_errors: set[str] = set()

    def record(message: str) -> None:
        if message not in recorded_errors:
            recorded_errors.add(message)
            errors.append(message)

    def checked_directory_fd(
        fd: int, binding: _PrivatePathBinding, label: str,
    ) -> os.stat_result | None:
        try:
            info = os.fstat(fd)
        except OSError as exc:
            record(f"inspect {label} descriptor: {exc}")
            return None
        if not _same_cleanup_directory_object(info, binding):
            record(f"{label} descriptor binding changed")
            return None
        return info

    parent_valid = checked_directory_fd(
        snapshot.parent_fd, snapshot.parent_binding,
        "private snapshot parent",
    ) is not None
    root_valid = checked_directory_fd(
        snapshot.root_fd, snapshot.root_binding,
        "private snapshot root",
    ) is not None

    try:
        parent_path = os.lstat(snapshot.root.parent)
        if not _same_cleanup_directory_object(
            parent_path, snapshot.parent_binding
        ):
            record("private snapshot parent pathname binding changed")
    except OSError as exc:
        record(f"inspect private snapshot parent pathname: {exc}")

    if root_valid and checked_directory_fd(
        snapshot.root_fd, snapshot.root_binding,
        "private snapshot root",
    ) is not None:
        try:
            os.fchmod(snapshot.root_fd, 0o700)
        except OSError as exc:
            record(f"restore private snapshot root mode: {exc}")
        try:
            names = os.listdir(snapshot.root_fd)
        except OSError as exc:
            names = []
            record(f"list bound snapshot root: {exc}")
        for name in names:
            if not isinstance(name, str) or \
                    name in ("", os.curdir, os.pardir) or \
                    "/" in name or "\x00" in name:
                record(f"unsafe entry in bound snapshot root: {name!r}")
                continue
            if checked_directory_fd(
                snapshot.root_fd, snapshot.root_binding,
                "private snapshot root",
            ) is None:
                root_valid = False
                break
            try:
                info = os.stat(
                    name, dir_fd=snapshot.root_fd, follow_symlinks=False
                )
            except OSError as exc:
                record(f"inspect bound snapshot entry {name!r}: {exc}")
                continue
            if stat.S_ISDIR(info.st_mode) and not stat.S_ISLNK(info.st_mode):
                record(
                    "refusing recursive cleanup of directory in bound "
                    f"snapshot: {name!r}"
                )
                continue
            if checked_directory_fd(
                snapshot.root_fd, snapshot.root_binding,
                "private snapshot root",
            ) is None:
                root_valid = False
                break
            try:
                os.unlink(name, dir_fd=snapshot.root_fd)
            except OSError as exc:
                record(f"unlink bound snapshot entry {name!r}: {exc}")

    if root_valid:
        root_info = checked_directory_fd(
            snapshot.root_fd, snapshot.root_binding,
            "private snapshot root",
        )
        if root_info is None:
            root_valid = False
        else:
            try:
                remaining = os.listdir(snapshot.root_fd)
                if remaining:
                    record(
                        "bound snapshot root is not empty after anchored "
                        "cleanup: "
                        + ", ".join(repr(name) for name in remaining)
                    )
            except OSError as exc:
                record(f"verify bound snapshot root cleanup: {exc}")

    bound_names: list[str] = []
    expected_name_replaced = False
    if root_valid and parent_valid and checked_directory_fd(
        snapshot.parent_fd, snapshot.parent_binding,
        "private snapshot parent",
    ) is not None:
        try:
            parent_names = os.listdir(snapshot.parent_fd)
        except OSError as exc:
            parent_names = []
            record(
                f"scan private ROM snapshot parent during cleanup: {exc}"
            )
        for candidate in parent_names:
            if checked_directory_fd(
                snapshot.parent_fd, snapshot.parent_binding,
                "private snapshot parent",
            ) is None:
                parent_valid = False
                break
            try:
                info = os.stat(
                    candidate, dir_fd=snapshot.parent_fd,
                    follow_symlinks=False,
                )
            except OSError:
                continue
            if info.st_dev == snapshot.root_binding.device and \
                    info.st_ino == snapshot.root_binding.inode:
                bound_names.append(candidate)
            elif candidate == snapshot.root.name:
                expected_name_replaced = True
    if expected_name_replaced:
        record(
            "private ROM snapshot pathname was replaced; unrelated "
            "replacement was left untouched"
        )
    if len(bound_names) > 1:
        record("bound private ROM snapshot root has duplicate names")
    elif len(bound_names) == 1 and root_valid and parent_valid:
        candidate = bound_names[0]
        parent_before = checked_directory_fd(
            snapshot.parent_fd, snapshot.parent_binding,
            "private snapshot parent",
        )
        root_before = checked_directory_fd(
            snapshot.root_fd, snapshot.root_binding,
            "private snapshot root",
        )
        if parent_before is None:
            parent_valid = False
        if root_before is None:
            root_valid = False
        if parent_valid and root_valid:
            try:
                before = os.stat(
                    candidate, dir_fd=snapshot.parent_fd,
                    follow_symlinks=False,
                )
                if before.st_dev != snapshot.root_binding.device or \
                        before.st_ino != snapshot.root_binding.inode:
                    record(
                        "bound snapshot directory changed before removal: "
                        f"{candidate!r}"
                    )
                else:
                    os.rmdir(candidate, dir_fd=snapshot.parent_fd)
                    after_removal = os.fstat(snapshot.root_fd)
                    if not _same_cleanup_directory_object(
                        after_removal, snapshot.root_binding
                    ):
                        record(
                            "bound snapshot root descriptor changed during "
                            "directory removal"
                        )
                    try:
                        os.stat(
                            candidate, dir_fd=snapshot.parent_fd,
                            follow_symlinks=False,
                        )
                    except FileNotFoundError:
                        pass
                    except OSError as exc:
                        record(
                            "cannot prove removed snapshot pathname absent: "
                            f"{exc}"
                        )
                    else:
                        record(
                            "removed snapshot pathname still exists after "
                            "directory removal"
                        )

                    remaining_bound_names: list[str] = []
                    try:
                        for remaining_name in os.listdir(snapshot.parent_fd):
                            try:
                                remaining_info = os.stat(
                                    remaining_name,
                                    dir_fd=snapshot.parent_fd,
                                    follow_symlinks=False,
                                )
                            except OSError:
                                continue
                            if remaining_info.st_dev == \
                                    snapshot.root_binding.device and \
                                    remaining_info.st_ino == \
                                    snapshot.root_binding.inode:
                                remaining_bound_names.append(remaining_name)
                    except OSError as exc:
                        record(
                            "cannot rescan snapshot parent after removal: "
                            f"{exc}"
                        )
                    if remaining_bound_names:
                        record(
                            "bound snapshot root remains named after directory "
                            "removal: "
                            + ", ".join(
                                repr(name) for name in remaining_bound_names
                            )
                        )

                    if after_removal.st_nlink != 0:
                        getpath = getattr(fcntl, "F_GETPATH", None)
                        if sys.platform != "darwin" or getpath is None:
                            record(
                                "bound snapshot root remains linked after "
                                "directory removal"
                            )
                        else:
                            try:
                                raw_path = fcntl.fcntl(
                                    snapshot.root_fd, getpath, b"\x00" * 1024
                                )
                                linked_path = os.fsdecode(
                                    raw_path.split(b"\x00", 1)[0]
                                )
                                if not linked_path or not \
                                        os.path.isabs(linked_path):
                                    raise OSError(
                                        "F_GETPATH returned an invalid path"
                                    )
                                os.lstat(linked_path)
                            except FileNotFoundError:
                                pass
                            except OSError as exc:
                                record(
                                    "cannot prove Darwin snapshot inode "
                                    f"unlinked: {exc}"
                                )
                            else:
                                record(
                                    "bound snapshot root remains linked after "
                                    "directory removal"
                                )
            except OSError as exc:
                record(
                    f"remove bound snapshot directory {candidate!r}: {exc}"
                )
    elif not bound_names and root_valid:
        root_info = checked_directory_fd(
            snapshot.root_fd, snapshot.root_binding,
            "private snapshot root",
        )
        if root_info is not None and root_info.st_nlink > 0:
            record(
                "bound snapshot root remains linked outside its private parent"
            )

    if root_valid and checked_directory_fd(
        snapshot.root_fd, snapshot.root_binding,
        "private snapshot root",
    ) is not None:
        try:
            os.close(snapshot.root_fd)
        except OSError as exc:
            record(f"close root descriptor: {exc}")
    if parent_valid and checked_directory_fd(
        snapshot.parent_fd, snapshot.parent_binding,
        "private snapshot parent",
    ) is not None:
        try:
            os.close(snapshot.parent_fd)
        except OSError as exc:
            record(f"close snapshot parent descriptor: {exc}")
    if errors:
        raise FlashError(
            "private ROM snapshot cleanup failed: " + "; ".join(errors)
        )


def _materialize_private_rom_snapshot(
    layout: UplinkRomLayout, regions: tuple[RomFlashRegion, ...],
) -> _PrivateRomSnapshot:
    root = Path(os.path.realpath(tempfile.mkdtemp(
        prefix="fof-uplink-rom-flash-"
    )))
    parent_fd: int | None = None
    root_fd: int | None = None
    parent_binding: _PrivatePathBinding | None = None
    original_root_binding: _PrivatePathBinding | None = None
    final_root_binding: _PrivatePathBinding | None = None
    files: list[_PrivateRomFile] = []

    def require_parent_binding() -> os.stat_result:
        if parent_fd is None or parent_binding is None:
            raise FlashError("private ROM snapshot parent is not bound")
        parent_path = os.lstat(root.parent)
        if not _same_directory_identity(parent_path, parent_binding):
            raise FlashError(
                "private ROM snapshot parent pathname binding changed"
            )
        return _require_private_directory_descriptor(
            parent_fd, parent_binding,
            label="private ROM snapshot parent",
        )

    def require_original_root_path() -> os.stat_result:
        if parent_fd is None or original_root_binding is None:
            raise FlashError("private ROM snapshot root is not bound")
        require_parent_binding()
        try:
            info = os.stat(
                root.name, dir_fd=parent_fd, follow_symlinks=False
            )
        except OSError as exc:
            raise FlashError(
                f"private ROM snapshot root pathname is unavailable: {exc}"
            ) from exc
        if not _same_directory_identity(info, original_root_binding):
            raise FlashError(
                "private ROM snapshot root pathname binding changed"
            )
        return info

    try:
        rendered_root = os.fspath(root)
        rendered_source = os.fspath(layout.build_dir)
        common_root = os.path.commonpath((rendered_root, rendered_source))
        if not os.path.isabs(rendered_root) or \
                os.path.normpath(rendered_root) != rendered_root or \
                common_root in (rendered_root, rendered_source):
            raise FlashError(
                "private ROM snapshot root aliases the canonical source tree"
            )
        os.chmod(root, 0o700)
        before_root = os.lstat(root)
        if not stat.S_ISDIR(before_root.st_mode) or \
                before_root.st_mode & 0o777 != 0o700 or \
                before_root.st_uid != os.geteuid() or \
                before_root.st_gid != os.getegid():
            raise FlashError("private ROM snapshot root is not owned 0700")
        original_root_binding = _private_path_binding(before_root)
        before_parent = os.lstat(root.parent)
        if not stat.S_ISDIR(before_parent.st_mode):
            raise FlashError("private ROM snapshot parent is not a directory")
        parent_binding = _private_path_binding(before_parent)
        parent_fd = os.open(
            root.parent, _private_open_flags(directory=True)
        )
        require_parent_binding()

        before_root = require_original_root_path()
        if _private_path_binding(before_root) != original_root_binding:
            raise FlashError(
                "private ROM snapshot root metadata changed before open"
            )
        require_parent_binding()
        root_fd = os.open(
            root.name, _private_open_flags(directory=True), dir_fd=parent_fd
        )
        require_parent_binding()
        require_original_root_path()
        opened_root = _require_private_directory_descriptor(
            root_fd, original_root_binding,
            label="private ROM snapshot root",
        )
        if _private_path_binding(before_root) != \
                _private_path_binding(opened_root) or \
                _private_path_binding(opened_root) != original_root_binding:
            raise FlashError("private ROM snapshot root changed during open")

        for name, region in zip(UPLINK_ROM_PRIVATE_NAMES, regions):
            require_parent_binding()
            require_original_root_path()
            _require_private_directory_descriptor(
                root_fd, original_root_binding,
                label="private ROM snapshot root",
            )
            files.append(
                _create_private_rom_file(
                    root_fd, root, name, region,
                    expected_root_binding=original_root_binding,
                )
            )
            require_parent_binding()
            require_original_root_path()
            _require_private_directory_descriptor(
                root_fd, original_root_binding,
                label="private ROM snapshot root",
            )
        if len({item.path for item in files}) != len(files):
            raise FlashError("private ROM snapshot paths are not unique")
        source_paths = {region.path for region in regions}
        if any(item.path in source_paths for item in files):
            raise FlashError("private ROM snapshot path aliases source path")

        require_parent_binding()
        after_root = require_original_root_path()
        opened_after = _require_private_directory_descriptor(
            root_fd, original_root_binding,
            label="private ROM snapshot root",
        )
        final_root_binding = _private_path_binding(after_root)
        if final_root_binding != _private_path_binding(opened_after) or \
                not stat.S_ISDIR(after_root.st_mode) or \
                after_root.st_mode & 0o777 != 0o700 or \
                after_root.st_uid != os.geteuid() or \
                after_root.st_gid != os.getegid():
            raise FlashError("private ROM snapshot root metadata drift")
        return _PrivateRomSnapshot(
            root=root,
            parent_fd=parent_fd,
            root_fd=root_fd,
            parent_binding=parent_binding,
            root_binding=final_root_binding,
            files=tuple(files),
        )
    except BaseException as primary:
        cleanup_errors: list[str] = []
        if root_fd is not None and parent_fd is not None and \
                parent_binding is not None and \
                original_root_binding is not None:
            try:
                _cleanup_private_rom_snapshot(_PrivateRomSnapshot(
                    root=root,
                    parent_fd=parent_fd,
                    root_fd=root_fd,
                    parent_binding=parent_binding,
                    root_binding=original_root_binding,
                    files=tuple(files),
                ))
            except BaseException as exc:
                cleanup_errors.append(str(exc))
        else:
            parent_valid = False
            if parent_fd is not None and parent_binding is not None:
                try:
                    parent_valid = _same_cleanup_directory_object(
                        os.fstat(parent_fd), parent_binding
                    )
                except OSError as exc:
                    cleanup_errors.append(
                        f"inspect partial parent descriptor: {exc}"
                    )
                if not parent_valid:
                    cleanup_errors.append(
                        "partial parent descriptor binding changed; "
                        "descriptor left untouched"
                    )

            if root_fd is None and parent_valid and \
                    parent_fd is not None and \
                    original_root_binding is not None:
                try:
                    candidate_fd = os.open(
                        root.name, _private_open_flags(directory=True),
                        dir_fd=parent_fd,
                    )
                except OSError as exc:
                    cleanup_errors.append(
                        f"reopen partial materialization root: {exc}"
                    )
                else:
                    try:
                        candidate_valid = _same_cleanup_directory_object(
                            os.fstat(candidate_fd), original_root_binding
                        )
                    except OSError as exc:
                        candidate_valid = False
                        cleanup_errors.append(
                            f"inspect reopened partial root: {exc}"
                        )
                    if candidate_valid:
                        root_fd = candidate_fd
                    else:
                        cleanup_errors.append(
                            "reopened partial root binding changed; "
                            "descriptor left untouched"
                        )

            if root_fd is not None and parent_fd is not None and \
                    original_root_binding is not None and \
                    parent_binding is not None:
                try:
                    _cleanup_private_rom_snapshot(_PrivateRomSnapshot(
                        root=root,
                        parent_fd=parent_fd,
                        root_fd=root_fd,
                        parent_binding=parent_binding,
                        root_binding=original_root_binding,
                        files=tuple(files),
                    ))
                except BaseException as exc:
                    cleanup_errors.append(str(exc))
            else:
                if root_fd is not None and \
                        original_root_binding is not None:
                    try:
                        root_valid = _same_cleanup_directory_object(
                            os.fstat(root_fd), original_root_binding
                        )
                    except OSError as exc:
                        root_valid = False
                        cleanup_errors.append(
                            f"inspect partial root descriptor: {exc}"
                        )
                    if root_valid:
                        try:
                            os.close(root_fd)
                        except OSError as exc:
                            cleanup_errors.append(
                                f"close partial root descriptor: {exc}"
                            )
                    else:
                        cleanup_errors.append(
                            "partial root descriptor binding changed; "
                            "descriptor left untouched"
                        )
                if parent_fd is not None and parent_binding is not None and \
                        parent_valid:
                    try:
                        still_parent = _same_cleanup_directory_object(
                            os.fstat(parent_fd), parent_binding
                        )
                    except OSError as exc:
                        still_parent = False
                        cleanup_errors.append(
                            f"inspect partial parent before close: {exc}"
                        )
                    if still_parent:
                        try:
                            os.close(parent_fd)
                        except OSError as exc:
                            cleanup_errors.append(
                                f"close partial parent descriptor: {exc}"
                            )
                    else:
                        cleanup_errors.append(
                            "partial parent descriptor changed before close; "
                            "descriptor left untouched"
                        )
                if parent_fd is None:
                    cleanup_errors.append(
                        "materialization root has no anchored parent "
                        "descriptor; pathname left untouched"
                    )
        if cleanup_errors:
            cleanup = FlashError(
                "private ROM materialization cleanup failed: " +
                "; ".join(cleanup_errors)
            )
            _add_cleanup_failure_note(
                primary, cleanup, scope="private ROM materialization"
            )
            raise
        raise


def _read_private_rom_file(
    snapshot: _PrivateRomSnapshot, item: _PrivateRomFile,
) -> bytes:
    try:
        before = os.stat(
            item.name, dir_fd=snapshot.root_fd, follow_symlinks=False
        )
    except OSError as exc:
        raise FlashError(
            f"private ROM snapshot path is unavailable: {item.name}: {exc}"
        ) from exc
    if _private_path_binding(before) != item.binding or \
            not stat.S_ISREG(before.st_mode) or before.st_nlink != 1 or \
            before.st_mode & 0o777 != 0o400:
        raise FlashError(
            f"private ROM snapshot metadata changed: {item.name}"
        )
    try:
        fd = os.open(
            item.name, os.O_RDONLY | _private_open_flags(),
            dir_fd=snapshot.root_fd,
        )
    except OSError as exc:
        raise FlashError(
            f"private ROM snapshot cannot be opened safely: {item.name}: {exc}"
        ) from exc
    after_fd: os.stat_result | None = None
    chunks: list[bytes] = []
    total = 0
    primary: BaseException | None = None
    try:
        opened = os.fstat(fd)
        if _private_path_binding(opened) != item.binding:
            raise FlashError(
                f"private ROM snapshot changed during open: {item.name}"
            )
        while True:
            try:
                chunk = os.read(fd, min(64 * 1024, item.binding.size + 1 - total))
            except InterruptedError:
                continue
            if not chunk:
                break
            chunks.append(chunk)
            total += len(chunk)
            if total > item.binding.size:
                raise FlashError(
                    f"private ROM snapshot grew during read: {item.name}"
                )
        after_fd = os.fstat(fd)
    except OSError as exc:
        primary = FlashError(
            f"private ROM snapshot read failed: {item.name}: {exc}"
        )
        primary.__cause__ = exc
    except BaseException as exc:
        primary = exc

    close_error = _close_bound_private_file_descriptor(
        fd, item.binding, label=f"private ROM snapshot reader {item.name}"
    )
    if primary is not None:
        if close_error is not None:
            _add_secondary_failure_note(
                primary, close_error,
                scope=f"private ROM snapshot reader {item.name}",
            )
        raise primary
    if close_error is not None:
        raise close_error
    if after_fd is None:
        raise FlashError(
            f"private ROM snapshot reader {item.name} has no final binding"
        )
    try:
        after_path = os.stat(
            item.name, dir_fd=snapshot.root_fd, follow_symlinks=False
        )
    except OSError as exc:
        raise FlashError(
            f"private ROM snapshot changed during read: {item.name}: {exc}"
        ) from exc
    if _private_path_binding(after_fd) != item.binding or \
            _private_path_binding(after_path) != item.binding or \
            total != item.binding.size:
        raise FlashError(
            f"private ROM snapshot changed during read: {item.name}"
        )
    return b"".join(chunks)


def _verify_private_rom_snapshot(snapshot: _PrivateRomSnapshot) -> None:
    try:
        parent_path = os.lstat(snapshot.root.parent)
        parent_fd = os.fstat(snapshot.parent_fd)
        root_path = os.stat(
            snapshot.root.name, dir_fd=snapshot.parent_fd,
            follow_symlinks=False,
        )
        root_fd = os.fstat(snapshot.root_fd)
    except OSError as exc:
        raise FlashError(
            f"private ROM snapshot root changed: {exc}"
        ) from exc
    if not _same_directory_identity(parent_path, snapshot.parent_binding) or \
            not _same_directory_identity(parent_fd, snapshot.parent_binding) or \
            _private_path_binding(root_path) != snapshot.root_binding or \
            _private_path_binding(root_fd) != snapshot.root_binding or \
            not stat.S_ISDIR(root_path.st_mode) or \
            root_path.st_mode & 0o777 != 0o700 or \
            root_path.st_uid != os.geteuid() or \
            root_path.st_gid != os.getegid():
        raise FlashError("private ROM snapshot root binding changed")
    for item in snapshot.files:
        data = _read_private_rom_file(snapshot, item)
        if hashlib.sha256(data).hexdigest() != item.sha256:
            raise FlashError(
                f"private ROM snapshot digest changed: {item.name}"
            )
    try:
        parent_path = os.lstat(snapshot.root.parent)
        parent_fd = os.fstat(snapshot.parent_fd)
        root_path = os.stat(
            snapshot.root.name, dir_fd=snapshot.parent_fd,
            follow_symlinks=False,
        )
        root_fd = os.fstat(snapshot.root_fd)
    except OSError as exc:
        raise FlashError(
            f"private ROM snapshot root changed during verification: {exc}"
        ) from exc
    if not _same_directory_identity(parent_path, snapshot.parent_binding) or \
            not _same_directory_identity(parent_fd, snapshot.parent_binding) or \
            _private_path_binding(root_path) != snapshot.root_binding or \
            _private_path_binding(root_fd) != snapshot.root_binding:
        raise FlashError("private ROM snapshot root changed during verification")


def _call_rom_stage(
    stage: str, argv: list[str], timeout_s: float,
    snapshot: _PrivateRomSnapshot, esptool_runner: Any,
    *, mutation_uncertain: bool, before_runner: Any = None,
) -> str:
    _verify_private_rom_snapshot(snapshot)
    output: Any = None
    primary: BaseException | None = None
    try:
        if before_runner is not None:
            before_runner()
        output = esptool_runner(argv, timeout_s=timeout_s)
    except BaseException as exc:
        primary = exc
    try:
        _verify_private_rom_snapshot(snapshot)
    except BaseException as integrity_error:
        if primary is None:
            primary = integrity_error
        else:
            add_note = getattr(primary, "add_note", None)
            if callable(add_note):
                add_note(
                    f"{stage} post-child integrity also failed: "
                    f"{integrity_error}"
                )
    if primary is not None:
        if isinstance(primary, RomFlashUncertainError):
            raise primary
        if mutation_uncertain:
            raise RomFlashUncertainError(
                f"{stage} outcome is uncertain: "
                f"{str(primary) or type(primary).__name__}"
            ) from primary
        if isinstance(primary, FlashError):
            raise primary
        if isinstance(primary, Exception):
            raise FlashError(f"{stage} failed: {primary}") from primary
        raise primary
    if type(output) is not str:
        malformed = FlashError(
            f"{stage} runner returned a non-text transcript"
        )
        if mutation_uncertain:
            raise RomFlashUncertainError(
                f"{stage} outcome is uncertain: {malformed}"
            ) from malformed
        raise malformed
    return output


def flash_complete_uplink_layout(
    descriptor: UsbDescriptorRecord,
    artifacts: FrozenArtifactSet,
    version: str,
) -> RomFlashStageEvidence:
    """Mutate one descriptor-bound uplink over one retained ROM handle."""
    _validate_bound_rom_inputs(descriptor, artifacts, version)
    mutation_may_have_started = False
    try:
        with BoundRomSession.open(
            descriptor,
            descriptor.serial_number,
        ) as session:
            probe = session.probe()
            mutation_may_have_started = True
            write = session.write_layout(artifacts)
            verify = session.verify_layout(artifacts)
            run = session.run_application()
            if session.transcript != (write, verify, run):
                raise RomFlashUncertainError(
                    "bound ROM operation transcript changed after mutation"
                )
    except BoundRomUnavailableError as exc:
        if mutation_may_have_started:
            raise RomFlashUncertainError(
                "bound ROM mutation outcome is uncertain"
            ) from exc
        raise
    except BoundRomMutationUncertainError as exc:
        raise RomFlashUncertainError(
            "bound ROM mutation outcome is uncertain"
        ) from exc
    except (BoundRomError, UsbDescriptorBindingError) as exc:
        if mutation_may_have_started:
            raise RomFlashUncertainError(
                "bound ROM mutation outcome is uncertain"
            ) from exc
        raise FlashError(f"bound ROM flash failed: {exc}") from exc
    except BaseException as exc:
        if mutation_may_have_started:
            raise RomFlashUncertainError(
                "bound ROM mutation outcome is uncertain"
            ) from exc
        raise

    evidence = RomFlashStageEvidence(
        descriptor=descriptor,
        base_mac=descriptor.serial_number,
        layout_version=version,
        aggregate_sha256=artifacts.aggregate_sha256,
        probe=probe,
        write=write,
        verify=verify,
        run=run,
    )
    try:
        _attest_rom_flash_stage(
            evidence,
            layout_version=version,
            artifacts=artifacts,
        )
    except BaseException as exc:
        raise RomFlashUncertainError(
            "bound ROM mutation evidence could not be attested"
        ) from exc
    return evidence


def _validate_bound_rom_inputs(
    descriptor: UsbDescriptorRecord,
    artifacts: FrozenArtifactSet,
    version: str,
) -> None:
    if type(descriptor) is not UsbDescriptorRecord:
        raise FlashError(
            "ROM mutation requires an exact USB descriptor record"
        )
    if descriptor.device != _validated_rom_port(descriptor.device) or \
            descriptor.vid != ESPRESSIF_USB_SERIAL_JTAG_VID or \
            descriptor.pid != ESPRESSIF_USB_SERIAL_JTAG_PID or \
            descriptor.serial_number != normalized_hardware_id(
                descriptor.serial_number
            ) or type(descriptor.location) is not str or \
            not descriptor.location or \
            descriptor.location != descriptor.location.strip() or any(
                ord(character) < 0x20 or ord(character) == 0x7F
                for character in descriptor.location
            ) or any(
                type(value) is not int or value < 0
                for value in (
                    descriptor.stat_device,
                    descriptor.stat_inode,
                    descriptor.stat_rdev,
                )
            ):
        raise FlashError("ROM USB descriptor evidence is malformed")
    if type(artifacts) is not FrozenArtifactSet:
        raise FlashError(
            "ROM mutation requires an exact frozen artifact set"
        )
    try:
        artifacts.__post_init__()
    except SecureArtifactError as exc:
        raise FlashError("ROM frozen artifact set identity changed") from exc
    if type(version) is not str or not _ORDERED_VERSION_RE.fullmatch(version):
        raise FlashError("ROM layout version is invalid")
    firmware = _frozen_firmware_bytes(artifacts, role="uplink")
    identity = parse_firmware_identity(firmware)
    if identity is None or identity.version != version:
        raise FlashError(
            "ROM frozen application version does not match the requested "
            "layout version"
        )


def _expected_rom_member_hashes(
    artifacts: FrozenArtifactSet,
) -> tuple[tuple[int, str, str], ...]:
    by_name = {
        member.logical_name: member.sha256
        for member in artifacts.members
    }
    expected_layout = (
        (0x00000, "artifact.bootloader"),
        (0x08000, "artifact.partitions"),
        (0x0F000, "artifact.ota_data_initial"),
        (0x20000, "artifact.firmware"),
    )
    try:
        return tuple(
            (offset, logical_name, by_name[logical_name])
            for offset, logical_name in expected_layout
        )
    except KeyError as exc:
        raise FlashError(
            "ROM frozen artifact set is missing a required region"
        ) from exc


def _attest_rom_flash_stage(
    stage: RomFlashStageEvidence,
    *,
    layout_version: str,
    artifacts: FrozenArtifactSet,
) -> None:
    """Strictly bind ROM evidence to one descriptor and frozen byte set."""
    if type(stage) is not RomFlashStageEvidence:
        raise FlashError("ROM flash stage evidence has an invalid shape")
    _validate_bound_rom_inputs(stage.descriptor, artifacts, layout_version)
    if type(stage.base_mac) is not str or \
            stage.base_mac != stage.descriptor.serial_number or \
            stage.layout_version != layout_version or \
            type(stage.aggregate_sha256) is not str or \
            stage.aggregate_sha256 != artifacts.aggregate_sha256:
        raise FlashError("ROM flash stage evidence has an invalid identity")
    probe = stage.probe
    if type(probe) is not RomIdentityEvidence or \
            probe.descriptor_serial != stage.descriptor.serial_number or \
            probe.base_mac != stage.base_mac or \
            probe.chip_name != "ESP32-S3" or \
            type(probe.revision) is not str or \
            re.fullmatch(
                r"v[0-9]{1,2}(?:\.[0-9]{1,2}){1,2}",
                probe.revision,
            ) is None or \
            probe.flash_size != "8MB" or probe.psram_size != "8MB":
        raise FlashError("ROM probe evidence is malformed or mismatched")
    member_hashes = _expected_rom_member_hashes(artifacts)
    expected_operations = (
        ("write", stage.write, member_hashes),
        ("verify", stage.verify, member_hashes),
        ("run", stage.run, ()),
    )
    for operation, evidence, hashes in expected_operations:
        if type(evidence) is not RomOperationEvidence or \
                evidence.operation != operation or \
                evidence.base_mac != stage.base_mac or \
                evidence.aggregate_sha256 != stage.aggregate_sha256 or \
                evidence.member_sha256 != hashes:
            raise FlashError(
                f"ROM {operation} evidence is malformed or mismatched"
            )
    if stage.application_health_verified is not False or \
            stage.rollback_cleared is not False:
        raise FlashError("ROM stage evidence contains false app-health claims")


def repo_version(platform: dict[str, Any]) -> str:
    version_h = ESP32_DIR / "shared/version.h"
    text = version_h.read_text(encoding="utf-8")
    macro = platform.get("version_macro")
    if macro not in {"FOF_VERSION_BADGE", "FOF_VERSION_BADGE_CANARY"}:
        raise FlashError("platform version macro is missing or unsupported")
    match = re.search(
        rf'^\s*#define\s+{re.escape(macro)}\s+"([^"]+)"\s*$',
        text,
        re.MULTILINE,
    )
    if not match:
        raise FlashError(f"missing exact {macro} version macro")
    return match.group(1)


def norm_version(value: str | None) -> str:
    value = (value or "").strip()
    return value[1:] if value[:1].lower() == "v" else value


def versions_match(got: str | None, wanted: str | None) -> bool:
    got_norm = norm_version(got)
    wanted_norm = norm_version(wanted)
    return bool(got_norm) and got_norm == wanted_norm


_ORDERED_VERSION_RE = re.compile(
    r"^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z][0-9A-Za-z._-]*))?$"
)
_HARDWARE_ID_RE = re.compile(r"^(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$")


def firmware_version_relation(candidate: str | None,
                              current: str | None) -> str:
    """Order numeric cores and fail closed on ambiguous named variants."""
    candidate_norm = norm_version(candidate)
    current_norm = norm_version(current)
    candidate_match = _ORDERED_VERSION_RE.fullmatch(candidate_norm)
    current_match = _ORDERED_VERSION_RE.fullmatch(current_norm)
    if not candidate_match or not current_match:
        return "invalid"
    candidate_core = tuple(int(candidate_match.group(i)) for i in range(1, 4))
    current_core = tuple(int(current_match.group(i)) for i in range(1, 4))
    if candidate_core < current_core:
        return "older"
    if candidate_core > current_core:
        return "newer"
    if candidate_norm == current_norm:
        return "equal"
    return "unordered"


def scanner_status_by_uart(status: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {
        str(item.get("uart")): item
        for item in status.get("scanners", [])
        if isinstance(item, dict) and item.get("uart")
    }


def normalized_hardware_id(value: Any) -> str:
    rendered = str(value or "").strip().lower()
    if not _HARDWARE_ID_RE.fullmatch(rendered):
        raise FlashError(f"invalid scanner hardware id: {value!r}")
    return rendered


def verify_scanner_identity_fields(info: dict[str, Any],
                                   platform: dict[str, Any],
                                   slot: str) -> None:
    expected = {
        "firmware_name": platform["scanner_name"],
        "app_project": platform["scanner_project"],
        "hardware_type": platform["hardware_type"],
    }
    labels = {
        "firmware_name": "target",
        "app_project": "project",
        "hardware_type": "hardware type",
    }
    for key, wanted in expected.items():
        got = info.get(key)
        if got != wanted:
            raise FlashError(
                f"{slot} scanner {labels[key]} mismatch: got {got}, wanted {wanted}"
            )
    board = info.get("board")
    if board not in (None, "", platform["scanner_name"]):
        raise FlashError(
            f"{slot} scanner board mismatch: got {board}, "
            f"wanted {platform['scanner_name']}"
        )


def capture_scanner_hardware_ids(status: dict[str, Any],
                                 platform: dict[str, Any],
                                 slots: list[str], *,
                                 require_connected: bool = True) -> dict[str, str]:
    by_uart = scanner_status_by_uart(status)
    captured: dict[str, str] = {}
    for slot in slots:
        info = by_uart.get(slot)
        if not info or not info.get("connected"):
            if require_connected:
                raise FlashError(f"{slot} scanner is not connected")
            continue
        verify_scanner_identity_fields(info, platform, slot)
        captured[slot] = normalized_hardware_id(info.get("hardware_id"))
    if len(set(captured.values())) != len(captured):
        raise FlashError("scanner hardware ids are not unique across requested slots")
    return captured


def scanner_update_newer_slots(status: dict[str, Any], slots: list[str],
                               target_version: str, *,
                               require_connected: bool = True) -> set[str]:
    """Classify safe per-slot skips without weakening manual-flash guards."""
    by_uart = scanner_status_by_uart(status)
    newer: set[str] = set()
    for slot in slots:
        info = by_uart.get(slot)
        if not info or not info.get("connected"):
            if require_connected:
                raise FlashError(
                    f"cannot prove downgrade safety: {slot} scanner is not connected"
                )
            continue
        current = info.get("ver") or info.get("version")
        relation = firmware_version_relation(target_version, str(current or ""))
        if relation == "older":
            newer.add(slot)
        elif relation == "unordered":
            raise FlashError(
                f"unordered firmware variants refused for {slot}: current "
                f"{current}, candidate {target_version}"
            )
        elif relation == "invalid":
            raise FlashError(
                f"cannot prove downgrade safety for {slot}: current {current!r}, "
                f"candidate {target_version!r}"
            )
    return newer


def scanner_strictly_older_slots(
    status: dict[str, Any],
    slots: list[str],
    target_version: str,
    *,
    require_connected: bool = True,
) -> set[str]:
    """Return only slots proven strictly older before coordinator staging."""
    by_uart = scanner_status_by_uart(status)
    older: set[str] = set()
    for slot in slots:
        info = by_uart.get(slot)
        if not info or not info.get("connected"):
            if require_connected:
                raise FlashError(
                    f"cannot prove automatic convergence setup: "
                    f"{slot} scanner is not connected"
                )
            continue
        current = info.get("ver") or info.get("version")
        relation = firmware_version_relation(
            target_version, str(current or "")
        )
        if relation == "newer":
            older.add(slot)
        elif relation == "unordered":
            raise FlashError(
                f"unordered firmware variants refused for {slot}: current "
                f"{current}, candidate {target_version}"
            )
        elif relation == "invalid":
            raise FlashError(
                f"cannot prove automatic convergence setup for {slot}: "
                f"current {current!r}, candidate {target_version!r}"
            )
    return older


def reject_scanner_downgrades(status: dict[str, Any], slots: list[str],
                              target_version: str) -> None:
    newer = scanner_update_newer_slots(
        status, slots, target_version, require_connected=True
    )
    if newer:
        slot = sorted(newer)[0]
        info = scanner_status_by_uart(status).get(slot, {})
        current = info.get("ver") or info.get("version")
        raise FlashError(
            f"downgrade refused for {slot}: current {current}, "
            f"candidate {target_version}"
        )


def _validate_firmware_bytes(image: bytes, path: Path, *, target: str,
                             project: str, hardware: str,
                             version: str) -> None:
    """Validate identity markers on the exact immutable image supplied."""
    if not isinstance(image, bytes):
        raise FlashError(f"firmware artifact snapshot is not immutable: {path}")
    identity = parse_firmware_identity(image)
    if identity is None:
        raise FlashError(f"invalid firmware project/version descriptor: {path}")
    if identity.project != project:
        raise FlashError(
            f"firmware project mismatch for {target}: "
            f"embedded {identity.project}, expected {project}"
        )
    if identity.version != version:
        raise FlashError(
            f"firmware version mismatch for {target}: "
            f"embedded {identity.version}, expected {version}"
        )
    if target.encode("ascii") not in image:
        raise FlashError(f"firmware target marker missing for {target}: {path}")
    if hardware.encode("ascii") not in image:
        raise FlashError(
            f"firmware hardware marker missing for {target}: "
            f"expected {hardware} in {path}"
        )


def read_validated_firmware_artifact(
    path: Path, *, target: str, project: str, hardware: str, version: str,
) -> bytes:
    """Read once, validate that snapshot, and return the same bytes to stream."""
    try:
        image = path.read_bytes()
    except OSError as exc:
        raise FlashError(f"cannot read firmware artifact {path}: {exc}") from exc
    _validate_firmware_bytes(
        image, path, target=target, project=project, hardware=hardware,
        version=version,
    )
    return image


def _frozen_firmware_bytes(
    artifacts: FrozenArtifactSet,
    *,
    role: str,
) -> bytes:
    """Revalidate one immutable member identity without consulting a path."""
    if type(artifacts) is not FrozenArtifactSet:
        raise FlashError(f"{role} USB firmware artifacts are not frozen")
    try:
        artifacts.__post_init__()
    except SecureArtifactError as exc:
        raise FlashError(
            f"{role} frozen firmware set identity changed"
        ) from exc
    members = tuple(
        member for member in artifacts.members
        if member.logical_name == "artifact.firmware"
    )
    if len(members) != 1:
        raise FlashError(
            f"{role} frozen firmware member is unavailable"
        )
    member = members[0]
    data = artifacts.member_bytes("artifact.firmware")
    if (
        type(data) is not bytes
        or data is not member.content
        or member.size != len(data)
        or hashlib.sha256(data).hexdigest() != member.sha256
    ):
        raise FlashError(
            f"{role} frozen firmware member identity changed"
        )
    return data


def _validated_frozen_firmware_bytes(
    artifacts: FrozenArtifactSet,
    *,
    role: str,
    target: str,
    project: str,
    hardware: str,
    version: str,
) -> bytes:
    """Validate one immutable firmware image without consulting a path."""
    data = _frozen_firmware_bytes(artifacts, role=role)
    _validate_firmware_bytes(
        data,
        Path(f"<frozen:{role}:artifact.firmware>"),
        target=target,
        project=project,
        hardware=hardware,
        version=version,
    )
    return data


def validate_firmware_artifact(path: Path, *, target: str, project: str,
                               hardware: str, version: str) -> None:
    """Reject stale or cross-target images before any flash path can run."""
    read_validated_firmware_artifact(
        path, target=target, project=project, hardware=hardware,
        version=version,
    )


_PARTITION_ENTRY_STRUCT = struct.Struct("<HBBII16sI")
_PARTITION_ENTRY_MAGIC = 0x50AA
_PARTITION_MD5_MAGIC = 0xEBEB


def _decode_partition_table_bytes(payload: bytes,
                                  path: Path) -> tuple[PartitionEntry, ...]:
    if not payload or len(payload) % _PARTITION_ENTRY_STRUCT.size:
        raise FlashError(
            f"partition table has malformed trailing data: {path}"
        )

    entries: list[PartitionEntry] = []
    entry_bytes = bytearray()
    labels: set[str] = set()
    all_ff = b"\xFF" * _PARTITION_ENTRY_STRUCT.size
    for position in range(0, len(payload), _PARTITION_ENTRY_STRUCT.size):
        raw = payload[position:position + _PARTITION_ENTRY_STRUCT.size]
        magic = struct.unpack_from("<H", raw)[0]
        if magic == _PARTITION_ENTRY_MAGIC:
            (_magic, entry_type, subtype, offset, size, raw_label,
             flags) = _PARTITION_ENTRY_STRUCT.unpack(raw)
            nul_at = raw_label.find(b"\x00")
            if nul_at == 0:
                raise FlashError(
                    f"partition label is empty at entry {len(entries)}"
                )
            if nul_at >= 0:
                label_bytes = raw_label[:nul_at]
                if any(raw_label[nul_at:]):
                    raise FlashError(
                        f"partition label is not NUL-clean at entry "
                        f"{len(entries)}"
                    )
            else:
                label_bytes = raw_label
            try:
                label = label_bytes.decode("ascii")
            except UnicodeDecodeError as exc:
                raise FlashError(
                    f"partition label is not ASCII at entry {len(entries)}"
                ) from exc
            if not label or any(ord(ch) < 0x20 or ord(ch) > 0x7E
                                for ch in label):
                raise FlashError(
                    f"partition label is not ASCII at entry {len(entries)}"
                )
            if label in labels:
                raise FlashError(f"duplicate partition label: {label}")
            labels.add(label)
            entries.append(PartitionEntry(
                label, entry_type, subtype, offset, size, flags
            ))
            entry_bytes.extend(raw)
            continue

        if magic == _PARTITION_MD5_MAGIC:
            if raw[2:16] != b"\xFF" * 14:
                raise FlashError(
                    f"partition MD5 trailer is malformed at {position:#x}"
                )
            wanted_digest = hashlib.md5(bytes(entry_bytes)).digest()
            if raw[16:32] != wanted_digest:
                raise FlashError(
                    f"partition MD5 digest mismatch at {position:#x}"
                )
            padding = payload[position + _PARTITION_ENTRY_STRUCT.size:]
            if any(byte != 0xFF for byte in padding):
                raise FlashError("partition table padding is not all FF")
            return tuple(entries)

        if raw == all_ff:
            raise FlashError("partition MD5 trailer is missing before padding")
        raise FlashError(
            f"invalid partition entry magic or MD5 trailer at {position:#x}"
        )

    raise FlashError("partition MD5 trailer is missing")


def decode_partition_table(path: Path) -> tuple[PartitionEntry, ...]:
    """Decode one exact ESP-IDF partition table with its MD5 trailer."""
    path = Path(path)
    try:
        payload = path.read_bytes()
    except OSError as exc:
        raise FlashError(f"cannot read partition table {path}: {exc}") from exc
    return _decode_partition_table_bytes(payload, path)


def _validate_uplink_partitions(
    partitions: tuple[PartitionEntry, ...],
) -> None:
    for entry in partitions:
        if entry.size == 0:
            raise FlashError(f"partition {entry.label} has zero size")
        end = entry.offset + entry.size
        if end > 0xFFFFFFFF:
            raise FlashError(
                f"partition {entry.label} arithmetic overflow: "
                f"{entry.offset:#x}+{entry.size:#x}"
            )
        if end > UPLINK_ROM_FLASH_SIZE:
            raise FlashError(
                f"partition {entry.label} exceeds 8 MiB flash: end={end:#x}"
            )
        if entry.type == 0 and entry.offset == 0x10000:
            raise FlashError(
                f"forbidden application partition at 0x10000: {entry.label}"
            )

    ordered = sorted(partitions, key=lambda entry: entry.offset)
    for previous, current in zip(ordered, ordered[1:]):
        previous_end = previous.offset + previous.size
        if current.offset < previous_end:
            raise FlashError(
                f"partition overlap: {previous.label} ends {previous_end:#x}, "
                f"{current.label} starts {current.offset:#x}"
            )

    if len(partitions) != len(UPLINK_ROM_PARTITIONS):
        raise FlashError(
            "partition table missing or extra entries: "
            f"got {len(partitions)}, wanted {len(UPLINK_ROM_PARTITIONS)}"
        )
    if partitions != UPLINK_ROM_PARTITIONS:
        for index, (got, wanted) in enumerate(zip(
            partitions, UPLINK_ROM_PARTITIONS
        )):
            if got != wanted:
                raise FlashError(
                    f"partition table semantic/order drift at entry {index}: "
                    f"got {got}, wanted {wanted}"
                )
        raise FlashError("partition table semantic/order drift")


def _snapshot_stat_tuple(info: Any) -> tuple[int, ...]:
    return (
        info.st_dev,
        info.st_ino,
        info.st_mode,
        info.st_nlink,
        info.st_uid,
        info.st_gid,
        info.st_size,
        info.st_mtime_ns,
        info.st_ctime_ns,
    )


def _open_snapshot_parent_fds(
    path: Path, root: Path, artifact: str, nofollow: int,
) -> tuple[str, int, list[int], tuple[int, ...],
           list[tuple[int, str, tuple[int, ...]]]]:
    try:
        relative = path.relative_to(root)
    except ValueError as exc:
        raise FlashError(
            f"{artifact} snapshot path escapes canonical root: {path}"
        ) from exc
    if any(component in (os.curdir, os.pardir) for component in relative.parts):
        raise FlashError(
            f"{artifact} snapshot path escapes canonical root: {path}"
        )
    if not relative.parts:
        raise FlashError(f"{artifact} snapshot path has no file name: {path}")
    directory_flag = getattr(os, "O_DIRECTORY", None)
    if directory_flag is None:
        raise FlashError(f"{artifact} snapshot requires O_DIRECTORY")
    directory_flags = os.O_RDONLY | directory_flag | nofollow | os.O_NONBLOCK
    directory_flags |= (
        getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOCTTY", 0)
    )
    directory_fds: list[int] = []
    bindings: list[tuple[int, str, tuple[int, ...]]] = []
    try:
        try:
            before_root = os.lstat(root)
        except OSError as exc:
            raise FlashError(
                f"{artifact} snapshot parent cannot be inspected: "
                f"{root}: {exc}"
            ) from exc
        if not stat.S_ISDIR(before_root.st_mode):
            raise FlashError(
                f"{artifact} snapshot parent is not a real directory: "
                f"{root}"
            )
        root_fd = os.open(root, directory_flags)
        directory_fds.append(root_fd)
        opened_root = os.fstat(root_fd)
        root_binding = _snapshot_stat_tuple(before_root)
        if not stat.S_ISDIR(opened_root.st_mode) or \
                root_binding != _snapshot_stat_tuple(opened_root):
            raise FlashError(
                f"{artifact} snapshot root changed during open: {root}"
            )

        current_fd = root_fd
        for component in relative.parts[:-1]:
            before = os.stat(
                component, dir_fd=current_fd, follow_symlinks=False
            )
            if not stat.S_ISDIR(before.st_mode):
                raise FlashError(
                    f"{artifact} snapshot parent is not a real directory: "
                    f"{component}"
                )
            child_fd = os.open(
                component, directory_flags, dir_fd=current_fd
            )
            directory_fds.append(child_fd)
            opened = os.fstat(child_fd)
            binding = _snapshot_stat_tuple(before)
            if not stat.S_ISDIR(opened.st_mode) or \
                    binding != _snapshot_stat_tuple(opened):
                raise FlashError(
                    f"{artifact} snapshot parent changed during open: "
                    f"{component}"
                )
            bindings.append((current_fd, component, binding))
            current_fd = child_fd
        return (
            relative.parts[-1], current_fd, directory_fds,
            root_binding, bindings,
        )
    except OSError as exc:
        for fd in reversed(directory_fds):
            try:
                os.close(fd)
            except OSError:
                pass
        raise FlashError(
            f"{artifact} snapshot parent cannot be opened safely: {exc}"
        ) from exc
    except Exception:
        for fd in reversed(directory_fds):
            try:
                os.close(fd)
            except OSError:
                pass
        raise


def _postcheck_snapshot_parent_fds(
    root: Path, root_binding: tuple[int, ...],
    bindings: list[tuple[int, str, tuple[int, ...]]], artifact: str,
) -> None:
    try:
        after_root = os.lstat(root)
        if _snapshot_stat_tuple(after_root) != root_binding:
            raise FlashError(
                f"{artifact} snapshot root changed during read: {root}"
            )
        for parent_fd, component, wanted in bindings:
            after = os.stat(
                component, dir_fd=parent_fd, follow_symlinks=False
            )
            if _snapshot_stat_tuple(after) != wanted:
                raise FlashError(
                    f"{artifact} snapshot parent changed during read: "
                    f"{component}"
                )
    except OSError as exc:
        raise FlashError(
            f"{artifact} snapshot parent changed during read: {exc}"
        ) from exc


def _read_regular_file_snapshot(
    path: Path, *, root: Path, max_size: int, artifact: str,
) -> bytes:
    """Read one bounded regular file once, bound to stable path/fd metadata."""
    path = Path(path)
    root = Path(root)
    if type(max_size) is not int or max_size < 0:
        raise FlashError(f"{artifact} snapshot size bound is invalid")
    nofollow = getattr(os, "O_NOFOLLOW", None)
    if nofollow is None:
        raise FlashError(f"{artifact} snapshot requires O_NOFOLLOW")
    (
        file_name, parent_fd, directory_fds, root_binding,
        directory_bindings,
    ) = _open_snapshot_parent_fds(path, root, artifact, nofollow)

    flags = os.O_RDONLY | nofollow | os.O_NONBLOCK
    flags |= getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOCTTY", 0)
    fd: int | None = None
    try:
        before_path = os.stat(
            file_name, dir_fd=parent_fd, follow_symlinks=False
        )
        if stat.S_ISLNK(before_path.st_mode):
            raise FlashError(
                f"{artifact} snapshot source is a symlink: {path}"
            )
        if not stat.S_ISREG(before_path.st_mode):
            raise FlashError(
                f"{artifact} snapshot source is not a regular file: {path}"
            )
        if before_path.st_nlink != 1:
            raise FlashError(
                f"{artifact} snapshot source link count must be 1: {path}"
            )

        fd = os.open(file_name, flags, dir_fd=parent_fd)
        opened = os.fstat(fd)
        if not stat.S_ISREG(opened.st_mode):
            raise FlashError(
                f"{artifact} snapshot descriptor is not a regular file: {path}"
            )
        if opened.st_nlink != 1:
            raise FlashError(
                f"{artifact} snapshot descriptor link count must be 1: {path}"
            )
        if _snapshot_stat_tuple(before_path) != _snapshot_stat_tuple(opened):
            raise FlashError(
                f"{artifact} snapshot source changed during open: {path}"
            )
        if opened.st_size > max_size:
            raise FlashError(
                f"{artifact} snapshot size exceeds {max_size:#x} bytes: "
                f"got {opened.st_size:#x}"
            )

        chunks: list[bytes] = []
        total = 0
        while True:
            request_size = min(64 * 1024, max_size + 1 - total)
            if request_size <= 0:
                raise FlashError(
                    f"{artifact} snapshot exceeds {max_size:#x} bytes"
                )
            try:
                chunk = os.read(fd, request_size)
            except InterruptedError:
                continue
            if not chunk:
                break
            chunks.append(chunk)
            total += len(chunk)
            if total > max_size:
                raise FlashError(
                    f"{artifact} snapshot exceeds {max_size:#x} bytes"
                )

        after_fd = os.fstat(fd)
        after_path = os.stat(
            file_name, dir_fd=parent_fd, follow_symlinks=False
        )
        if after_fd.st_nlink != 1 or after_path.st_nlink != 1:
            raise FlashError(
                f"{artifact} snapshot source link count changed during read: "
                f"{path}"
            )
        stable = _snapshot_stat_tuple(opened)
        if stable != _snapshot_stat_tuple(after_fd) or \
                stable != _snapshot_stat_tuple(after_path):
            raise FlashError(
                f"{artifact} snapshot source changed during read: {path}"
            )
        if total != opened.st_size:
            raise FlashError(
                f"{artifact} snapshot size changed during read: {path}"
            )
        _postcheck_snapshot_parent_fds(
            root, root_binding, directory_bindings, artifact
        )
        return b"".join(chunks)
    except OSError as exc:
        raise FlashError(
            f"{artifact} snapshot read failed: {path}: {exc}"
        ) from exc
    finally:
        if fd is not None:
            try:
                os.close(fd)
            except OSError:
                pass
        for directory_fd in reversed(directory_fds):
            try:
                os.close(directory_fd)
            except OSError:
                pass


def _ensure_private_snapshot_directory(root: Path, parent: Path) -> None:
    current = root
    for component in parent.relative_to(root).parts:
        current = current / component
        current.mkdir(mode=0o700, exist_ok=True)
        info = os.lstat(current)
        if not stat.S_ISDIR(info.st_mode):
            raise FlashError(
                f"private verifier path is not a directory: {current}"
            )
        os.chmod(current, 0o700)
        if os.lstat(current).st_mode & 0o777 != 0o700:
            raise FlashError(
                f"private verifier directory mode is not 0700: {current}"
            )


def _write_private_snapshot(path: Path, data: bytes) -> None:
    nofollow = getattr(os, "O_NOFOLLOW", None)
    if nofollow is None:
        raise FlashError("private verifier materialization requires O_NOFOLLOW")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | nofollow
    flags |= getattr(os, "O_CLOEXEC", 0)
    try:
        fd = os.open(path, flags, 0o400)
    except OSError as exc:
        raise FlashError(
            f"cannot create private verifier snapshot {path}: {exc}"
        ) from exc
    try:
        view = memoryview(data)
        written = 0
        while written < len(view):
            try:
                count = os.write(fd, view[written:])
            except InterruptedError:
                continue
            if count <= 0:
                raise FlashError(
                    f"short write for private verifier snapshot {path}"
                )
            written += count
        os.fchmod(fd, 0o400)
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or \
                info.st_mode & 0o777 != 0o400 or info.st_size != len(data):
            raise FlashError(
                f"private verifier snapshot is inconsistent: {path}"
            )
    except OSError as exc:
        raise FlashError(
            f"cannot write private verifier snapshot {path}: {exc}"
        ) from exc
    finally:
        try:
            os.close(fd)
        except OSError:
            pass


def _verify_uplink_snapshots(
    build_snapshots: dict[str, bytes], partition_source: bytes,
) -> None:
    with tempfile.TemporaryDirectory(
        prefix="fof-uplink-rom-verify-"
    ) as temporary_dir:
        private_build = Path(temporary_dir)
        os.chmod(private_build, 0o700)
        if os.lstat(private_build).st_mode & 0o777 != 0o700:
            raise FlashError("private verifier root mode is not 0700")
        for relative, data in build_snapshots.items():
            destination = private_build / relative
            _ensure_private_snapshot_directory(
                private_build, destination.parent
            )
            _write_private_snapshot(destination, data)
        private_source = (
            private_build / "partitions_s3_fof_badge_8mb.csv"
        )
        _write_private_snapshot(private_source, partition_source)
        try:
            verifier_errors = verify_badge_uplink_build(
                private_build, private_source
            )
        except Exception as exc:
            raise FlashError(
                f"current uplink build verifier failed: {exc}"
            ) from exc
        if verifier_errors:
            raise FlashError(
                "current uplink build verification failed: " +
                "; ".join(str(error) for error in verifier_errors)
            )


def _validate_esp32_s3_image(
    image: bytes, path: Path, artifact: str, *, flash_offset: int = 0,
) -> None:
    """Validate one complete digest-appended ESP32-S3 image snapshot."""
    _entrypoint, _segments, errors = validate_esp32_s3_image_bytes(
        image,
        artifact,
        flash_offset=flash_offset,
        max_size=max(len(image), 1),
    )
    if errors:
        raise FlashError(f"{errors[0]}: {path}")


def validate_current_uplink_rom_layout(
    platform: dict[str, Any], version: str,
) -> UplinkRomLayout:
    """Prove the canonical current uplink build is one safe ROM flash set."""
    expected_env_by_macro = {
        "FOF_VERSION_BADGE": "uplink-s3-fof_badge",
        "FOF_VERSION_BADGE_CANARY":
            "uplink-s3-fof_badge-con-crud-canary",
    }
    expected_env = expected_env_by_macro.get(platform.get("version_macro"))
    if expected_env is None:
        raise FlashError("canonical uplink platform version track is invalid")
    canonical_platform = {
        "uplink_name": UPLINK_ROM_TARGET,
        "uplink_project": UPLINK_ROM_PROJECT,
        "hardware_type": UPLINK_ROM_HARDWARE,
        "uplink_env": expected_env,
    }
    for key, wanted in canonical_platform.items():
        if platform.get(key) != wanted:
            raise FlashError(
                f"canonical uplink platform drift for {key}: "
                f"got {platform.get(key)!r}, wanted {wanted!r}"
            )
    try:
        canonical_version = repo_version(platform)
    except Exception as exc:
        raise FlashError(
            f"canonical repo version cannot be read: {exc}"
        ) from exc
    if type(canonical_version) is not str or not \
            _ORDERED_VERSION_RE.fullmatch(canonical_version):
        raise FlashError(
            f"canonical repo version is invalid: {canonical_version!r}"
        )
    if version != canonical_version:
        raise FlashError(
            f"caller version does not match canonical repo version: "
            f"got {version!r}, wanted {canonical_version!r}"
        )

    build_dir = (
        UPLINK_DIR / ".pio" / "build" / expected_env
    )
    partition_source = UPLINK_DIR / "partitions_s3_fof_badge_8mb.csv"
    region_specs = (
        (0x00000, "bootloader.bin"),
        (0x08000, "partitions.bin"),
        (0x0F000, "ota_data_initial.bin"),
        (0x20000, "firmware.bin"),
    )
    artifact_labels = {
        "bootloader.bin": "bootloader",
        "partitions.bin": "partitions image ending at 0x9000",
        "ota_data_initial.bin": "OTA data at 0xf000",
        "firmware.bin": "firmware for ota_0 at 0x20000",
    }
    snapshots: dict[str, bytes] = {}
    for name, max_size in UPLINK_ROM_BUILD_SNAPSHOT_LIMITS:
        snapshots[name] = _read_regular_file_snapshot(
            build_dir / name,
            root=UPLINK_DIR,
            max_size=max_size,
            artifact=artifact_labels.get(
                name, f"current uplink verifier input {name}"
            ),
        )
    partition_source_snapshot = _read_regular_file_snapshot(
        partition_source,
        root=UPLINK_DIR,
        max_size=UPLINK_ROM_PARTITION_SOURCE_LIMIT,
        artifact="exact partition CSV",
    )
    _verify_uplink_snapshots(snapshots, partition_source_snapshot)

    bootloader = snapshots["bootloader.bin"]
    partition_image = snapshots["partitions.bin"]
    ota_data = snapshots["ota_data_initial.bin"]
    firmware = snapshots["firmware.bin"]
    if len(bootloader) > 0x8000:
        raise FlashError(
            f"bootloader end exceeds 0x8000: {len(bootloader):#x}"
        )
    if 0x8000 + len(partition_image) > 0x9000:
        raise FlashError(
            "partitions image at 0x8000 exceeds 0x9000: "
            f"size={len(partition_image):#x}"
        )
    if len(ota_data) != 0x2000:
        raise FlashError(
            "OTA data at 0xf000 must be exactly 0x2000 bytes: "
            f"got {len(ota_data):#x}"
        )
    if ota_data != b"\xFF" * 0x2000:
        raise FlashError(
            "OTA data at 0xf000 must be erased (all 0xFF)"
        )
    if len(firmware) > 0x200000:
        raise FlashError(
            f"firmware does not fit ota_0 at 0x20000: "
            f"size={len(firmware):#x}, capacity=0x200000"
        )

    partitions = _decode_partition_table_bytes(
        partition_image, build_dir / "partitions.bin"
    )
    _validate_uplink_partitions(partitions)
    ota_0 = next(
        entry for entry in partitions if entry.label == "ota_0"
    )
    if ota_0.offset != 0x20000 or len(firmware) > ota_0.size:
        raise FlashError(
            f"firmware does not fit ota_0 at 0x20000: "
            f"size={len(firmware):#x}, capacity={ota_0.size:#x}"
        )
    _validate_esp32_s3_image(
        bootloader, build_dir / "bootloader.bin", "bootloader",
        flash_offset=0,
    )
    _validate_esp32_s3_image(
        firmware, build_dir / "firmware.bin", "firmware",
        flash_offset=0x20000,
    )
    _validate_firmware_bytes(
        firmware,
        build_dir / "firmware.bin",
        target=UPLINK_ROM_TARGET,
        project=UPLINK_ROM_PROJECT,
        hardware=UPLINK_ROM_HARDWARE,
        version=canonical_version,
    )

    regions = tuple(
        RomFlashRegion(
            offset=offset,
            path=build_dir / name,
            data=snapshots[name],
            size=len(snapshots[name]),
            sha256=hashlib.sha256(snapshots[name]).hexdigest(),
        )
        for offset, name in region_specs
    )
    return UplinkRomLayout(
        build_dir=build_dir,
        version=canonical_version,
        regions=regions,
        partitions=partitions,
    )


def run(cmd: list[str], cwd: Path, dry_run: bool) -> None:
    log("$ " + " ".join(cmd))
    if dry_run:
        return
    proc = subprocess.run(cmd, cwd=str(cwd), text=True)
    if proc.returncode != 0:
        raise FlashError(f"command failed with exit {proc.returncode}: {' '.join(cmd)}")


def build_firmware(platform: dict[str, Any], dry_run: bool) -> None:
    pio = find_pio()
    run([pio, "run", "-e", platform["scanner_env"]], SCANNER_DIR, dry_run)
    run([pio, "run", "-e", platform["uplink_env"]], UPLINK_DIR, dry_run)


def build_scanner_firmware(platform: dict[str, Any], dry_run: bool) -> None:
    pio = find_pio()
    run([pio, "run", "-e", platform["scanner_env"]], SCANNER_DIR, dry_run)


def selected_targets(only: str) -> tuple[bool, list[str]]:
    if only == "all":
        return True, ["ble", "wifi"]
    if only == "uplink":
        return True, []
    if only == "scanners":
        return False, ["ble", "wifi"]
    if only in ("ble", "wifi"):
        return False, [only]
    raise FlashError(f"unsupported --only value: {only}")


def scanner_slot_mask(slots: list[str]) -> int:
    mask = 0
    for slot in slots:
        if slot == "ble":
            mask |= 0x1
        elif slot == "wifi":
            mask |= 0x2
        else:
            raise FlashError(f"unsupported scanner slot: {slot}")
    if mask == 0:
        raise FlashError("scanner firmware staging requires at least one slot")
    return mask


def scanner_stage_receipt_fields(platform: dict[str, Any], version: str,
                                 data: bytes, slot_mask: int) -> dict[str, Any]:
    return {
        "target": platform["scanner_name"],
        "name": platform["scanner_name"],
        "app_project": platform["scanner_project"],
        "project": platform["scanner_project"],
        "hardware_type": platform["hardware_type"],
        "hardware": platform["hardware_type"],
        "version": version,
        "size": len(data),
        "crc32": binascii.crc32(data) & 0xFFFFFFFF,
        "sha256": hashlib.sha256(data).hexdigest(),
        "slot_mask": slot_mask,
    }


def validate_scanner_stage_receipt(receipt: dict[str, Any],
                                   expected: dict[str, Any], *,
                                   phase: str,
                                   require_generation: bool) -> None:
    if receipt.get("ok") is not True:
        raise FlashError(f"USB scanner firmware stage {phase} failed")
    for key, wanted in expected.items():
        got = receipt.get(key)
        if type(got) is not type(wanted) or got != wanted:
            raise FlashError(
                f"USB scanner firmware stage {phase} {key} mismatch"
            )
    if require_generation:
        generation = receipt.get("generation")
        if (
            type(generation) is not int or
            not 1 <= generation <= 0xFFFFFFFF
        ):
            raise FlashError(
                "USB scanner firmware stage final generation is invalid"
            )


def validate_scanner_relay_receipt(
    receipt: dict[str, Any],
    *,
    slot: str,
    expected_generation: int,
    expected_hardware_id: str,
    firmware_size: int,
) -> dict[str, Any]:
    """Require the exact terminal proof for one bound scanner relay."""
    if slot not in ("ble", "wifi"):
        raise FlashError("USB scanner relay slot is invalid")
    if (
        type(expected_generation) is not int or
        not 1 <= expected_generation <= 0xFFFFFFFF
    ):
        raise FlashError("USB scanner relay expected generation is invalid")
    if (
        type(firmware_size) is not int or
        firmware_size <= 0
    ):
        raise FlashError("USB scanner relay firmware size is invalid")
    if type(expected_hardware_id) is not str:
        raise FlashError("USB scanner relay expected hardware_id is invalid")
    expected_id = expected_hardware_id.strip().lower()
    if not _HARDWARE_ID_RE.fullmatch(expected_id):
        raise FlashError("USB scanner relay expected hardware_id is invalid")
    if type(receipt) is not dict:
        raise FlashError("USB scanner relay terminal receipt is not an object")
    received_keys = frozenset(receipt)
    if received_keys != SCANNER_RELAY_RECEIPT_KEYS:
        raise FlashError(
            "USB scanner relay terminal receipt schema mismatch"
        )

    received_id = receipt["hardware_id"]
    if (
        type(received_id) is not str or
        received_id != received_id.strip().lower() or
        not _HARDWARE_ID_RE.fullmatch(received_id)
    ):
        raise FlashError(
            "USB scanner relay terminal hardware_id is malformed"
        )
    if received_id != expected_id:
        raise FlashError(
            "USB scanner relay terminal hardware_id continuity mismatch"
        )

    expected_fields: dict[str, Any] = {
        "ok": True,
        "phase": "final",
        "slot": slot,
        "uart": slot,
        "generation": expected_generation,
        "size": firmware_size,
        "bytes": firmware_size,
        "chunks": (
            firmware_size + SCANNER_RELAY_CHUNK_BYTES - 1
        ) // SCANNER_RELAY_CHUNK_BYTES,
        "stage": "done",
        "done": True,
        "error": "",
    }
    for key, wanted in expected_fields.items():
        got = receipt[key]
        if type(got) is not type(wanted) or got != wanted:
            raise FlashError(
                f"USB scanner relay terminal {key} mismatch"
            )
    return dict(receipt)


def validate_scanner_relay_progress(
    progress: dict[str, Any],
    *,
    slot: str,
    firmware_size: int,
) -> dict[str, Any]:
    """Authorize one exact diagnostic progress receipt for a bound relay."""
    if slot not in ("ble", "wifi"):
        raise FlashError("USB scanner relay progress slot is invalid")
    if (
        type(firmware_size) is not int or
        not 1 <= firmware_size <= 0xFFFFFFFF
    ):
        raise FlashError("USB scanner relay progress firmware size is invalid")
    if type(progress) is not dict or \
            frozenset(progress) != SCANNER_RELAY_PROGRESS_KEYS:
        raise FlashError("USB scanner relay progress schema mismatch")
    if progress.get("uart") != slot:
        raise FlashError("USB scanner relay progress UART mismatch")
    if progress.get("size") != firmware_size:
        raise FlashError("USB scanner relay progress size mismatch")
    for field in ("bytes", "size", "percent", "chunks", "nacks", "retries"):
        value = progress.get(field)
        if type(value) is not int or not 0 <= value <= 0xFFFFFFFF:
            raise FlashError(
                f"USB scanner relay progress {field} is invalid"
            )
    elapsed_s = progress.get("elapsed_s")
    if type(elapsed_s) is not int or \
            not 0 <= elapsed_s <= 0x7FFFFFFFFFFFFFFF:
        raise FlashError(
            "USB scanner relay progress elapsed_s is invalid"
        )
    if type(progress.get("stage")) is not str or \
            type(progress.get("error")) is not str:
        raise FlashError("USB scanner relay progress string type mismatch")
    received = progress["bytes"]
    percent = progress["percent"]
    if received > firmware_size:
        raise FlashError("USB scanner relay progress bytes exceed size")
    expected_percent = (received * 100) // firmware_size
    if percent > 100 or percent != expected_percent:
        raise FlashError("USB scanner relay progress percent is inconsistent")
    return dict(progress)


def validate_scanner_stage_credit_receipt(
    receipt: dict[str, Any],
    expected: dict[str, Any],
    *,
    phase: str,
    received: int,
    credit_bytes: int,
    require_generation: bool,
) -> None:
    validate_scanner_stage_receipt(
        receipt,
        expected,
        phase=phase,
        require_generation=require_generation,
    )
    if receipt.get("flow_control") != "credit-v1":
        raise FlashError(
            f"USB scanner firmware stage {phase} omitted credit-v1 proof"
        )
    protocol = {
        "flow_control": "credit-v1",
        "phase": phase,
        "received": received,
        "total": expected["size"],
        "credit_bytes": credit_bytes,
    }
    for key, wanted in protocol.items():
        got = receipt.get(key)
        if type(got) is not type(wanted) or got != wanted:
            raise FlashError(
                f"USB scanner firmware stage {phase} {key} mismatch"
            )


def require_artifacts(platform: dict[str, Any], need_uplink: bool,
                      slots: list[str]) -> None:
    missing: list[Path] = []
    if need_uplink and not platform["uplink_bin"].exists():
        missing.append(platform["uplink_bin"])
    if slots and not platform["scanner_bin"].exists():
        missing.append(platform["scanner_bin"])
    if missing:
        rendered = "\n".join(f"  {p}" for p in missing)
        raise FlashError(f"missing firmware artifact(s):\n{rendered}")
    if slots and platform.get("scanner_env") in {
        "scanner-s3-combo-fof_badge",
        "scanner-s3-combo-fof_badge-con-crud-canary",
    }:
        scanner_layout_errors = verify_badge_scanner_build(
            platform["scanner_bin"].parent,
            SCANNER_DIR / "partitions_s3_scanner_8mb.csv",
        )
        scanner_layout_errors.extend(verify_badge_scanner_sdkconfig(
            SCANNER_DIR /
            f"sdkconfig.{platform['scanner_env']}"
        ))
        if scanner_layout_errors:
            raise FlashError(
                "badge scanner build layout verification failed:\n" +
                "\n".join(
                    f"  {error}" for error in scanner_layout_errors
                )
            )
    version = repo_version(platform)
    if need_uplink:
        validate_firmware_artifact(
            platform["uplink_bin"],
            target=platform["uplink_name"],
            project=platform["uplink_project"],
            hardware=platform["hardware_type"],
            version=version,
        )
    if slots:
        validate_firmware_artifact(
            platform["scanner_bin"],
            target=platform["scanner_name"],
            project=platform["scanner_project"],
            hardware=platform["hardware_type"],
            version=version,
        )


def _prepare_frozen_usb_firmware_artifacts(
    platform: dict[str, Any],
    need_uplink: bool,
    slots: list[str],
) -> FrozenUsbFirmwareArtifacts:
    """Freeze selected verified build trees before any USB descriptor access."""
    if type(need_uplink) is not bool or type(slots) is not list or any(
        type(slot) is not str for slot in slots
    ):
        raise FlashError("USB artifact selection is malformed")

    uplink: FrozenArtifactSet | None = None
    scanner: FrozenArtifactSet | None = None
    try:
        with tempfile.TemporaryDirectory(
            prefix="fof-usb-artifact-snapshot-",
            dir=os.path.realpath(tempfile.gettempdir()),
        ) as temporary:
            private_parent = Path(temporary)
            os.chmod(private_parent, 0o700)

            if need_uplink:
                snapshot = prepare_verified_badge_uplink_snapshot(
                    Path(platform["uplink_bin"]).parent,
                    UPLINK_DIR / "partitions_s3_fof_badge_8mb.csv",
                    UPLINK_DIR / f"sdkconfig.{platform['uplink_env']}",
                    private_parent=private_parent,
                    materialize_missing_aliases=False,
                )
                try:
                    uplink = snapshot.freeze_for_mutation()
                finally:
                    snapshot.close()

            if slots:
                snapshot = prepare_verified_badge_scanner_snapshot(
                    Path(platform["scanner_bin"]).parent,
                    SCANNER_DIR / "partitions_s3_scanner_8mb.csv",
                    SCANNER_DIR / f"sdkconfig.{platform['scanner_env']}",
                    private_parent=private_parent,
                    materialize_missing_aliases=False,
                )
                try:
                    scanner = snapshot.freeze_for_mutation()
                finally:
                    snapshot.close()
    except (OSError, SecureArtifactError, KeyError, TypeError) as exc:
        raise FlashError(
            "verified USB firmware artifact freeze failed"
        ) from exc

    return FrozenUsbFirmwareArtifacts(
        uplink=uplink,
        scanner=scanner,
    )


def _attest_frozen_uplink_flash_authority(
    platform: dict[str, Any],
    frozen: FrozenArtifactSet,
    version: str,
) -> None:
    """Bind the exact immutable ELF evidence to the bytes about to flash."""
    if (
        type(platform) is not dict or
        type(frozen) is not FrozenArtifactSet or
        type(version) is not str or
        not version
    ):
        raise FlashError("frozen uplink attestation input is malformed")
    expected_sizes = {
        UPLINK_PRODUCTION_ENV: UPLINK_PRODUCTION_RTC_NOINIT_BYTES,
        UPLINK_CANARY_ENV: UPLINK_CANARY_RTC_NOINIT_BYTES,
    }
    environment = platform.get("uplink_env")
    if type(environment) is not str or environment not in expected_sizes:
        raise FlashError(
            "frozen uplink attestation has an unknown build environment"
        )
    required_platform_fields = {
        "uplink_name",
        "uplink_project",
        "hardware_type",
    }
    if (
        not required_platform_fields <= platform.keys() or
        any(
            type(platform[key]) is not str or not platform[key]
            for key in required_platform_fields
        )
    ):
        raise FlashError(
            "frozen uplink attestation platform identity is malformed"
        )
    errors = verify_frozen_badge_uplink_flash_authority(
        frozen,
        environment=environment,
        target=platform["uplink_name"],
        project=platform["uplink_project"],
        hardware=platform["hardware_type"],
        version=version,
        expected_rtc_size=expected_sizes[environment],
    )
    if errors:
        raise FlashError(
            "frozen uplink ELF/bin attestation failed:\n" +
            "\n".join(f"  {error}" for error in errors)
        )


def scanner_firmware_size(platform: dict[str, Any]) -> int:
    return platform["scanner_bin"].stat().st_size if platform["scanner_bin"].exists() else 0


def list_usb_ports() -> list[str]:
    ports: list[str] = []
    for pattern in (
        "/dev/cu.usbmodem*",
        "/dev/cu.usbserial*",
        "/dev/cu.wchusbserial*",
        "/dev/cu.SLAB*",
    ):
        ports.extend(glob.glob(pattern))
    return sorted(dict.fromkeys(ports))


def _take_badge_usb_descriptor_census() -> tuple[UsbDescriptorRecord, ...]:
    try:
        return take_usb_descriptor_census()
    except UsbDescriptorBindingError as exc:
        raise FlashError(str(exc)) from exc


def usb_descriptor_record_for_port(port: str) -> UsbDescriptorRecord:
    """Select one path only after validating the complete USB census."""
    selected_port = _validated_rom_port(port)
    matches = [
        record for record in _take_badge_usb_descriptor_census()
        if record.device == selected_port
    ]
    if len(matches) != 1:
        raise FlashError(
            f"expected exactly one supported USB descriptor for "
            f"{selected_port!r}; found {len(matches)}"
        )
    return matches[0]


def usb_port_hardware_id(port: str) -> str:
    """Read the immutable ESP32-S3 USB serial descriptor for one exact port."""
    return usb_descriptor_record_for_port(port).serial_number


def select_trusted_uplink_descriptor(
    *,
    selected_port: str | None,
    operator_acknowledged: bool,
    trusted_binding: TrustedUplinkBinding | None = None,
) -> tuple[UsbDescriptorRecord, TrustedUplinkBinding]:
    """Complete one census and bind the uplink role before any serial open."""
    census = _take_badge_usb_descriptor_census()
    try:
        descriptor, binding = bind_selected_uplink(
            census,
            selected_port=selected_port,
            trusted_binding=trusted_binding,
            operator_acknowledged=operator_acknowledged,
        )
        return descriptor, _strengthen_trusted_uplink_binding(
            descriptor, binding
        )
    except UsbDescriptorBindingError as exc:
        raise FlashError(str(exc)) from exc


def _strengthen_trusted_uplink_binding(
    descriptor: UsbDescriptorRecord,
    binding: TrustedUplinkBinding,
) -> TrustedUplinkBinding:
    """Bind every later enumeration to the exact selected USB location."""
    if type(descriptor) is not UsbDescriptorRecord or \
            type(binding) is not TrustedUplinkBinding:
        raise FlashError("selected uplink binding is malformed")
    if descriptor.serial_number != binding.serial_number:
        raise FlashError(
            "selected uplink descriptor differs from its trusted serial"
        )
    if binding.location is not None and \
            descriptor.location != binding.location:
        raise FlashError(
            "selected uplink descriptor differs from its trusted location"
        )
    if descriptor.location is not None and (
        type(descriptor.location) is not str or
        not descriptor.location or
        descriptor.location != descriptor.location.strip() or
        any(
            ord(character) < 0x20 or ord(character) == 0x7F
            for character in descriptor.location
        )
    ):
        raise FlashError("selected uplink descriptor location is malformed")
    return TrustedUplinkBinding(
        serial_number=descriptor.serial_number,
        location=descriptor.location,
        source=binding.source,
    )


def probe_rom_device(
    port: str,
    timeout_s: float,
    *,
    esptool_runner: Any = run_guarded_esptool,
) -> RomDeviceIdentity | None:
    """Perform one guarded no-reset ROM sync on one explicit USB port."""
    selected_port = _validated_rom_port(port)
    if not isinstance(timeout_s, (int, float)) or isinstance(timeout_s, bool) \
            or not (0 < float(timeout_s) <= ESPTOOL_TIMEOUT_MAX_S):
        raise FlashError(
            f"ROM probe timeout must be in "
            f"(0, {ESPTOOL_TIMEOUT_MAX_S:g}] seconds"
        )
    try:
        transcript = esptool_runner(
            build_esptool_probe_argv(selected_port), timeout_s=timeout_s
        )
    except RomProbeUnavailable:
        return None
    return parse_esptool_rom_identity(transcript, selected_port)


def reset_uplink_usb_to_rom(
    port: str,
    timeout_s: float,
    *,
    esptool_runner: Any = run_guarded_esptool,
) -> RomDeviceIdentity:
    """Use native USB reset once, then require an exact uplink ROM identity."""
    selected_port = _validated_rom_port(port)
    if not isinstance(timeout_s, (int, float)) or isinstance(timeout_s, bool) \
            or not (0 < float(timeout_s) <= ESPTOOL_TIMEOUT_MAX_S):
        raise FlashError(
            f"native USB reset timeout must be in "
            f"(0, {ESPTOOL_TIMEOUT_MAX_S:g}] seconds"
        )
    transcript = esptool_runner(
        build_esptool_probe_argv(
            selected_port, native_usb_reset=True,
        ),
        timeout_s=timeout_s,
    )
    return parse_esptool_rom_identity(transcript, selected_port)


def _reset_bound_uplink_without_write(
    badge: BadgeSerial,
    *,
    deadline: float,
) -> tuple[UsbDescriptorRecord, dict[str, Any]]:
    """Reset one bound uplink through ROM without reading or writing flash."""
    if type(deadline) not in (int, float) or isinstance(deadline, bool):
        raise FlashError(
            "non-writing uplink reset deadline is invalid"
        )
    expected = normalized_hardware_id(
        getattr(badge, "expected_hardware_id", None)
    )
    descriptor = getattr(badge, "_descriptor", None)
    trusted_location = getattr(badge, "_trusted_location", None)
    if type(descriptor) is not UsbDescriptorRecord or \
            descriptor.serial_number != expected or \
            descriptor.location != trusted_location:
        raise FlashError(
            "non-writing uplink reset lost its trusted descriptor"
        )

    def remaining() -> float:
        value = float(deadline) - time.monotonic()
        if value <= 0:
            raise FlashError(
                "non-writing uplink reset exceeded its deadline"
            )
        return value

    badge._close_serial()
    census = _take_badge_usb_descriptor_census()
    matching_serial = tuple(
        record for record in census
        if record.serial_number == expected
    )
    if any(
        record.location != trusted_location
        for record in matching_serial
    ):
        raise FlashError(
            "non-writing uplink reset found its identity at another location"
        )
    matching = tuple(
        record for record in matching_serial
        if record.location == trusted_location
    )
    if len(matching) != 1:
        raise FlashError(
            "non-writing uplink reset requires one exact descriptor"
        )
    selected = matching[0]
    identity = reset_uplink_usb_to_rom(
        selected.device,
        min(30.0, remaining()),
    )
    if normalized_hardware_id(identity.base_mac) != expected or \
            identity.port != selected.device:
        raise FlashError(
            "non-writing uplink reset ROM identity mismatch"
        )
    transcript = run_guarded_esptool(
        build_esptool_run_argv(identity.port),
        timeout_s=min(30.0, remaining()),
    )
    if normalized_hardware_id(
        parse_esptool_run_result(transcript, expected)
    ) != expected:
        raise FlashError(
            "non-writing uplink reset application handoff mismatch"
        )
    status = badge.reconnect_same_uplink_normal(
        deadline=float(deadline)
    )
    rebound = getattr(badge, "_descriptor", None)
    if type(rebound) is not UsbDescriptorRecord or \
            rebound.serial_number != expected or \
            rebound.location != trusted_location:
        raise FlashError(
            "non-writing uplink reset rebound descriptor mismatch"
        )
    validate_uplink_application_status(status)
    return rebound, status


def require_selected_port_not_rom(
    required_application_port: str,
    expected_hardware_id: str,
    timeout_s: float,
) -> None:
    """Prove the selected fixed-function USB identity is not already in ROM."""
    required_port = _validated_rom_port(required_application_port)
    expected = _normalized_rom_mac(
        expected_hardware_id, label="expected USB descriptor"
    )
    if not isinstance(timeout_s, (int, float)) or isinstance(timeout_s, bool) \
            or not (0 < float(timeout_s) <= ESPTOOL_TIMEOUT_MAX_S):
        raise FlashError(
            "selected-port ROM absence timeout must be in "
            f"(0, {ESPTOOL_TIMEOUT_MAX_S:g}] seconds"
        )
    if usb_port_hardware_id(required_port) != expected:
        raise FlashError(
            "selected USB descriptor changed before the ROM absence proof"
        )

    deadline = time.monotonic() + float(timeout_s)
    for proof_index in range(ROM_CENSUS_ABSENCE_PROOFS):
        remaining = deadline - time.monotonic()
        remaining_proofs = ROM_CENSUS_ABSENCE_PROOFS - proof_index
        probe_timeout = remaining / remaining_proofs
        if probe_timeout <= 0:
            raise FlashError(
                "selected-port ROM absence proof exceeded its global timeout"
            )
        identity = probe_rom_device(required_port, probe_timeout)
        if identity is not None:
            selected = _revalidate_selected_rom_device(identity)
            if selected.base_mac != expected:
                raise FlashError(
                    "selected USB device identity changed during the ROM "
                    "absence proof"
                )
            raise FlashError(
                "pre-existing ROM device found on the selected USB port; "
                "physical-chord recovery refused"
            )

    if usb_port_hardware_id(required_port) != expected:
        raise FlashError(
            "selected USB descriptor changed during the ROM absence proof"
        )
    if time.monotonic() > deadline:
        raise FlashError(
            "selected-port ROM absence proof exceeded its global timeout"
        )


def require_no_rom_devices(
    required_application_port: str,
    timeout_s: float,
) -> tuple[str, ...]:
    """Take one stable global census and reject any pre-existing ROM."""
    required_port = _validated_rom_port(required_application_port)
    if not isinstance(timeout_s, (int, float)) or isinstance(timeout_s, bool) \
            or not (0 < float(timeout_s) <= ESPTOOL_TIMEOUT_MAX_S):
        raise FlashError(
            "ROM census timeout must be in "
            f"(0, {ESPTOOL_TIMEOUT_MAX_S:g}] seconds"
        )
    before = tuple(sorted(set(list_usb_ports())))
    if required_port not in before:
        raise FlashError(
            f"legacy application port disappeared before ROM census: "
            f"{required_port}"
        )
    deadline = time.monotonic() + float(timeout_s)
    found: list[RomDeviceIdentity] = []
    for index, port in enumerate(before):
        for proof_index in range(ROM_CENSUS_ABSENCE_PROOFS):
            remaining = deadline - time.monotonic()
            remaining_probes = (
                (len(before) - index - 1) *
                ROM_CENSUS_ABSENCE_PROOFS +
                (ROM_CENSUS_ABSENCE_PROOFS - proof_index)
            )
            probe_timeout = (
                remaining / remaining_probes if remaining_probes else 0.0
            )
            if probe_timeout <= 0:
                raise FlashError("ROM census exceeded its global timeout")
            identity = probe_rom_device(port, probe_timeout)
            if identity is not None:
                found.append(_revalidate_selected_rom_device(identity))
                break
    after = tuple(sorted(set(list_usb_ports())))
    if after != before:
        raise FlashError(
            "USB serial ports changed during the pre-bootstrap ROM census"
        )
    if time.monotonic() > deadline:
        raise FlashError("ROM census exceeded its global timeout")
    if found:
        rendered = ", ".join(
            f"{identity.port}={identity.base_mac}"
            for identity in found
        )
        raise FlashError(
            "pre-existing ROM device found; legacy bootstrap command refused: "
            + rendered
        )
    return before


def wait_for_rom_device(
    expected_hardware_id: str | None,
    timeout_s: float,
) -> RomDeviceIdentity:
    """Discover ROM mode by base MAC within one global monotonic deadline."""
    if expected_hardware_id is None:
        expected = None
    else:
        if type(expected_hardware_id) is not str:
            raise FlashError("expected ROM base MAC must be an exact string")
        expected = _normalized_rom_mac(
            expected_hardware_id, label="expected ROM base"
        )
    if not isinstance(timeout_s, (int, float)) or isinstance(timeout_s, bool) \
            or not (0 < float(timeout_s) <= ESPTOOL_TIMEOUT_MAX_S):
        raise FlashError(
            f"ROM discovery timeout must be in "
            f"(0, {ESPTOOL_TIMEOUT_MAX_S:g}] seconds"
        )

    deadline = time.monotonic() + float(timeout_s)
    observations: dict[str, str] = {}
    while True:
        ports = sorted(set(list_usb_ports()))
        identities: list[RomDeviceIdentity] = []
        round_complete = True
        for index, port in enumerate(ports):
            remaining = max(0.0, deadline - time.monotonic())
            ports_left = len(ports) - index
            probe_timeout = remaining / ports_left
            if probe_timeout <= 0:
                round_complete = False
                continue
            identity = probe_rom_device(port, probe_timeout)
            if time.monotonic() > deadline:
                round_complete = False
            if identity is None:
                observations[port] = "silent"
                continue
            identity = _revalidate_selected_rom_device(identity)
            if identity.port != port:
                raise FlashError(
                    f"ROM probe path changed: queried {port!r}, identity has "
                    f"{identity.port!r}"
                )
            identities.append(identity)
            observations[port] = identity.base_mac

        round_authorized = round_complete and time.monotonic() <= deadline
        if round_authorized and expected is None:
            if len(identities) > 1:
                rendered = ", ".join(
                    f"{identity.port}={identity.base_mac}"
                    for identity in identities
                )
                raise FlashError(
                    "multiple ROM devices found without a known base MAC: "
                    + rendered
                )
            if len(identities) == 1:
                return identities[0]
        elif round_authorized:
            matches = [
                identity for identity in identities
                if identity.base_mac == expected
            ]
            if len(matches) > 1:
                rendered = ", ".join(
                    identity.port for identity in matches
                )
                raise FlashError(
                    f"duplicate ROM base MAC {expected} found on: {rendered}"
                )
            if len(matches) == 1:
                return matches[0]

        remaining = deadline - time.monotonic()
        if remaining <= 0:
            detail = ", ".join(
                f"{port}={observation}"
                for port, observation in sorted(observations.items())
            ) or "no ROM serial ports"
            target = expected or "an unambiguous ROM device"
            raise FlashError(
                f"timed out discovering ROM base MAC {target}; observed "
                f"{detail}"
            )
        time.sleep(min(0.25, remaining))


def detect_usb_port() -> str:
    ports = list_usb_ports()
    if not ports:
        raise FlashError("no badge USB serial port found; pass --port")
    if len(ports) > 1:
        raise FlashError("multiple USB serial ports found; pass --port:\n" +
                         "\n".join(f"  {p}" for p in ports))
    return ports[0]


def import_pyserial() -> Any:
    try:
        import serial  # type: ignore
        import serial.tools.list_ports  # type: ignore
        return serial
    except Exception:
        for site in glob.glob(str(Path.home() / ".platformio/penv/lib/python*/site-packages")):
            if site not in sys.path:
                sys.path.insert(0, site)
        try:
            import serial  # type: ignore
            import serial.tools.list_ports  # type: ignore
            return serial
        except Exception as exc:
            raise FlashError(
                "pyserial is required for USB badge flashing; install it for "
                "python3 or run with /Users/billh/.platformio/penv/bin/python"
            ) from exc


def flash_scanner_usb(platform: dict[str, Any], port: str, dry_run: bool,
                      slot: str | None = None) -> None:
    if not dry_run:
        require_artifacts(platform, False, [slot or "scanner"])
    pio = find_pio()
    label = f" ({slot})" if slot else ""
    log(f"[scanner-usb] flashing scanner firmware{label} on {port}")
    run([pio, "run", "-e", platform["scanner_env"], "-t", "upload",
         "--upload-port", port], SCANNER_DIR, dry_run)


def http_json(url: str, method: str = "GET", data: bytes | None = None,
              timeout: int = 30) -> dict[str, Any]:
    req = Request(url, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/octet-stream")
    with urlopen(req, timeout=timeout) as resp:
        body = resp.read().decode("utf-8", "replace")
    parsed = json.loads(body)
    if not isinstance(parsed, dict):
        raise FlashError(f"unexpected JSON response from {url}: {parsed!r}")
    return parsed


def post_json(url: str, payload: dict[str, Any], timeout: int = 30) -> dict[str, Any]:
    data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    req = Request(url, data=data, method="POST")
    req.add_header("Content-Type", "application/json")
    with urlopen(req, timeout=timeout) as resp:
        body = resp.read().decode("utf-8", "replace")
    parsed = json.loads(body)
    if not isinstance(parsed, dict):
        raise FlashError(f"unexpected JSON response from {url}: {parsed!r}")
    return parsed


def wait_http_status(base_url: str, timeout_s: int = 90) -> dict[str, Any]:
    deadline = time.time() + timeout_s
    last_error: Exception | None = None
    while time.time() < deadline:
        try:
            return http_json(f"{base_url}/api/badge/status", timeout=10)
        except (HTTPError, URLError, TimeoutError, json.JSONDecodeError) as exc:
            last_error = exc
            time.sleep(2)
    raise FlashError(f"badge status did not become reachable at {base_url}: {last_error}")


def upload_scanner_network(platform: dict[str, Any], base_url: str,
                           version: str, dry_run: bool) -> None:
    require_usb_firmware_transport("network")
    query = urlencode({"name": platform["scanner_name"], "version": version})
    url = f"{base_url}/api/fw/upload?{query}"
    log(f"[stage] POST {url} ({platform['scanner_bin'].stat().st_size if platform['scanner_bin'].exists() else '?'} bytes)")
    if dry_run:
        return
    data = platform["scanner_bin"].read_bytes()
    body = http_json(url, method="POST", data=data, timeout=600)
    if not body.get("ok"):
        raise FlashError(f"scanner firmware upload failed: {body}")


def relay_scanner_network(base_url: str, slot: str, dry_run: bool,
                          force_probe: bool,
                          allow_same_version: bool,
                          firmware_size: int = 0) -> None:
    require_usb_firmware_transport("network")
    query = urlencode({
        "uart": slot,
        "force": "1" if force_probe else "0",
        "allow_same_version": "1" if allow_same_version else "0",
    })
    url = f"{base_url}/api/fw/relay?{query}"
    log(f"[relay] POST {url}")
    if dry_run:
        return
    body = http_json(url, method="POST", data=b"",
                     timeout=scanner_relay_timeout_s(firmware_size))
    if not body.get("ok"):
        raise FlashError(f"{slot} scanner relay failed: {body}")


def flash_uplink_network(platform: dict[str, Any], base_url: str,
                         dry_run: bool) -> None:
    require_usb_firmware_transport("network")
    url = f"{base_url}/api/ota"
    log(f"[uplink] POST {url} ({platform['uplink_bin'].stat().st_size if platform['uplink_bin'].exists() else '?'} bytes)")
    if dry_run:
        return
    data = platform["uplink_bin"].read_bytes()
    body = http_json(url, method="POST", data=data, timeout=600)
    if not body.get("ok"):
        raise FlashError(f"uplink OTA failed: {body}")


def resolve_node(backend: str, node: str) -> str:
    if re.match(r"^\d+\.\d+\.\d+\.\d+$", node):
        return node
    for path in ("/detections/nodes/status", "/nodes"):
        try:
            data = http_json(f"{backend}{path}", timeout=10)
        except Exception:
            continue
        nodes = data.get("nodes", data.get("items", data if isinstance(data, list) else []))
        for item in nodes:
            if not isinstance(item, dict):
                continue
            if item.get("device_id") == node or item.get("name") == node:
                ip = item.get("ip") or item.get("last_ip") or item.get("static_ip")
                if ip:
                    return ip
    fallback = REPO_ROOT / "scripts/fof_flash.local.json"
    if fallback.exists():
        data = json.loads(fallback.read_text(encoding="utf-8"))
        ip = data.get("device_ip", {}).get(node)
        if ip:
            return ip
    raise FlashError(f"could not resolve badge node {node!r}")


def _exact_uint(value: Any, label: str) -> int:
    if type(value) is not int or value < 0:
        raise FlashError(f"{label} must be an exact non-negative integer")
    return value


def _validate_uplink_status_common(status: dict[str, Any]) -> str:
    """Validate identity, partition, rollback, and USB-health invariants."""
    if not isinstance(status, dict):
        raise FlashError("FOF_STATUS payload is not an object")
    aliases = {
        "target": "uplink-s3-fof_badge",
        "firmware_name": "uplink-s3-fof_badge",
        "project": "fof_badge_uplink",
        "app_project": "fof_badge_uplink",
        "hardware_type": "seeed_xiao_esp32s3",
    }
    for field, expected in aliases.items():
        if status.get(field) != expected:
            raise FlashError(
                f"uplink {field} mismatch: got {status.get(field)!r}, "
                f"wanted {expected!r}"
            )
    version = status.get("version")
    if not isinstance(version, str) or not version or \
            firmware_version_relation(version, version) != "equal":
        raise FlashError(f"uplink version is invalid: {version!r}")
    hardware_id = normalized_hardware_id(status.get("hardware_id"))
    partition = status.get("running_partition")
    if partition not in ("ota_0", "ota_1"):
        raise FlashError(f"uplink running partition is invalid: {partition!r}")
    pending = status.get("pending_verify")
    if type(pending) is not bool:
        raise FlashError("uplink pending_verify must be an exact boolean")
    rollback = status.get("rollback_state")
    expected_rollback = "pending_verify" if pending else "clear"
    if rollback != expected_rollback:
        raise FlashError(
            f"uplink rollback_state is inconsistent: got {rollback!r}, "
            f"wanted {expected_rollback!r}"
        )
    health = status.get("usb_health")
    if not isinstance(health, dict):
        raise FlashError("uplink FOF_STATUS is missing usb_health")
    _exact_uint(health.get("responses_completed"),
                "usb_health.responses_completed")
    return hardware_id


def validate_uplink_application_status(status: dict[str, Any]) -> str:
    """Validate the immutable normal-application identity and rollback proof."""
    hardware_id = _validate_uplink_status_common(status)
    pending = status["pending_verify"]
    recovery = status.get("recovery_mode")
    allowed_recovery = {"normal", "startup_dependency"} if pending else {"normal"}
    if recovery not in allowed_recovery:
        raise FlashError(
            f"uplink recovery_mode is invalid for current rollback state: "
            f"{recovery!r}"
        )
    if status.get("update_session") not in (None, ""):
        raise FlashError(
            "normal uplink status retained an update session"
        )
    return hardware_id


_UPDATE_UPLINK_STATUS_KEYS = frozenset({
    "phase", "session", "version", "sha256", "size", "partition",
    "received",
})
_UPDATE_SCANNER_STATUS_KEYS = frozenset({
    "phase", "session", "target", "sha256", "size", "slot_mask",
    "received", "generation",
})
_UPDATE_CAMPAIGN_STATUS_KEYS = frozenset({
    "generation", "target_slot_mask", "pending_mask", "worker_running",
    "readiness_probes", "scanners",
})
_UPDATE_CAMPAIGN_SCANNER_KEYS = frozenset({
    "slot", "state", "attempts",
})
_UPDATE_CAMPAIGN_STATES = frozenset({
    "excluded", "awaiting_check", "offered", "ready_queued", "relaying",
    "converged", "current", "newer_skipped", "refused", "failed",
    "recovering",
})


def _validate_update_uplink_status(
    status: Any,
    *,
    session: str,
) -> dict[str, Any]:
    """Validate the exact durable/live OTA summary in maintenance status."""
    bound_session = _validated_update_session(session)
    if type(status) is not dict or set(status) != _UPDATE_UPLINK_STATUS_KEYS:
        raise FlashError("update_uplink status schema mismatch")
    phase = status.get("phase")
    if phase not in ("idle", "receiving", "committed"):
        raise FlashError("update_uplink phase is invalid")
    if status.get("session") != bound_session:
        raise FlashError("update_uplink session mismatch")
    size = status.get("size")
    received = status.get("received")
    if type(size) is not int or type(received) is not int or \
            size < 0 or size > 0xFFFFFFFF or received < 0 or \
            received > size:
        raise FlashError("update_uplink byte count is invalid")
    for field in ("version", "sha256", "partition"):
        if type(status.get(field)) is not str:
            raise FlashError(f"update_uplink {field} is invalid")
    if phase == "idle":
        if (
            status["version"] != "" or status["sha256"] != "" or
            status["partition"] != "" or size != 0 or received != 0
        ):
            raise FlashError("update_uplink idle summary is not empty")
    else:
        if firmware_version_relation(
            status["version"], status["version"]
        ) != "equal":
            raise FlashError("update_uplink version is invalid")
        if not re.fullmatch(r"[0-9a-f]{64}", status["sha256"]):
            raise FlashError("update_uplink sha256 is invalid")
        if status["partition"] not in ("ota_0", "ota_1") or size <= 0:
            raise FlashError("update_uplink manifest is invalid")
        if phase == "committed" and received != size:
            raise FlashError("update_uplink commit is incomplete")
    return dict(status)


def _validate_update_scanner_status(
    status: Any,
    *,
    session: str,
) -> dict[str, Any]:
    """Validate the exact live/durable scanner stage summary."""
    bound_session = _validated_update_session(session)
    if type(status) is not dict or set(status) != _UPDATE_SCANNER_STATUS_KEYS:
        raise FlashError("update_scanner status schema mismatch")
    phase = status.get("phase")
    if phase not in ("idle", "receiving", "committed"):
        raise FlashError("update_scanner phase is invalid")
    if status.get("session") != bound_session:
        raise FlashError("update_scanner session mismatch")
    for field in ("size", "slot_mask", "received", "generation"):
        value = status.get(field)
        if type(value) is not int or not 0 <= value <= 0xFFFFFFFF:
            raise FlashError(f"update_scanner {field} is invalid")
    if type(status.get("target")) is not str or \
            type(status.get("sha256")) is not str:
        raise FlashError("update_scanner manifest string is invalid")

    size = status["size"]
    slot_mask = status["slot_mask"]
    received = status["received"]
    generation = status["generation"]
    if phase == "idle":
        if (
            status["target"] != "" or status["sha256"] != "" or
            size != 0 or slot_mask != 0 or received != 0 or generation != 0
        ):
            raise FlashError("update_scanner idle summary is not empty")
        return dict(status)

    if not status["target"] or any(
        not 0x21 <= ord(char) <= 0x7E for char in status["target"]
    ):
        raise FlashError("update_scanner target is invalid")
    if not re.fullmatch(r"[0-9a-f]{64}", status["sha256"]):
        raise FlashError("update_scanner sha256 is invalid")
    if size <= 0 or slot_mask not in (1, 2, 3) or received > size:
        raise FlashError("update_scanner manifest is invalid")
    if phase == "receiving":
        if received >= size or generation != 0:
            raise FlashError("update_scanner receiving summary is ambiguous")
    elif received != size or not 1 <= generation <= 0xFFFFFFFF:
        raise FlashError("update_scanner committed summary is incomplete")
    return dict(status)


def _validate_update_campaign_status(
    status: Any,
    *,
    expected_generation: int,
    expected_slot_mask: int,
) -> dict[str, Any]:
    """Validate the compact maintenance-only coordinator snapshot."""
    if type(status) is not dict or \
            set(status) != _UPDATE_CAMPAIGN_STATUS_KEYS:
        raise FlashError("update_campaign status schema mismatch")
    generation = status.get("generation")
    target_mask = status.get("target_slot_mask")
    pending_mask = status.get("pending_mask")
    if type(expected_generation) is not int or not (
        1 <= expected_generation <= 0xFFFFFFFF
    ):
        raise FlashError("expected update campaign generation is invalid")
    if type(expected_slot_mask) is not int or \
            expected_slot_mask not in (1, 2, 3):
        raise FlashError("expected update campaign slot mask is invalid")
    if type(generation) is not int or generation != expected_generation:
        raise FlashError("update campaign generation mismatch")
    if type(target_mask) is not int or target_mask != expected_slot_mask:
        raise FlashError("update campaign target slot mask mismatch")
    if type(pending_mask) is not int or pending_mask < 0 or \
            pending_mask > 3 or pending_mask & ~target_mask:
        raise FlashError("update campaign pending mask is invalid")
    if type(status.get("worker_running")) is not bool:
        raise FlashError(
            "update campaign worker_running must be an exact boolean"
        )

    probes = status.get("readiness_probes")
    if type(probes) is not list or len(probes) != 2 or any(
        type(value) is not int or
        not 0 <= value <= UPDATE_READINESS_MAX_PROBES
        for value in probes
    ):
        raise FlashError("update campaign readiness probes are invalid")

    scanners = status.get("scanners")
    if type(scanners) is not list or len(scanners) != 2:
        raise FlashError("update campaign scanner summaries are invalid")
    normalized: dict[int, dict[str, Any]] = {}
    for scanner in scanners:
        if type(scanner) is not dict or \
                set(scanner) != _UPDATE_CAMPAIGN_SCANNER_KEYS:
            raise FlashError(
                "update campaign scanner summary schema mismatch"
            )
        slot = scanner.get("slot")
        state = scanner.get("state")
        attempts = scanner.get("attempts")
        if type(slot) is not int or slot not in (0, 1) or \
                slot in normalized:
            raise FlashError("update campaign scanner slot is invalid")
        if type(state) is not str or state not in \
                _UPDATE_CAMPAIGN_STATES:
            raise FlashError("update campaign scanner state is invalid")
        if type(attempts) is not int or not 0 <= attempts <= 3:
            raise FlashError("update campaign scanner attempts are invalid")
        requested = bool(target_mask & (1 << slot))
        if not requested and state != "excluded":
            raise FlashError(
                "update campaign excluded scanner state mismatch"
            )
        normalized[slot] = dict(scanner)
    if set(normalized) != {0, 1}:
        raise FlashError("update campaign must report both scanner slots")
    return {
        "generation": generation,
        "target_slot_mask": target_mask,
        "pending_mask": pending_mask,
        "worker_running": status["worker_running"],
        "readiness_probes": list(probes),
        "scanners": [normalized[0], normalized[1]],
    }


def _validate_update_preparing_status(
    status: dict[str, Any],
    *,
    session: str,
    expected_hardware_id: str,
) -> dict[str, Any]:
    """Prove the exact same-session PREPARING marker before cancellation."""
    bound_session = _validated_update_session(session)
    hardware_id = _validate_uplink_status_common(status)
    if hardware_id != normalized_hardware_id(expected_hardware_id):
        raise FlashError("preparing uplink hardware_id mismatch")
    if status.get("recovery_mode") != "update_preparing":
        raise FlashError("uplink update preparation is not active")
    if status.get("update_session") != bound_session:
        raise FlashError("uplink update preparing session mismatch")
    if status.get("pending_verify") is not False or \
            status.get("rollback_state") != "clear":
        raise FlashError(
            "uplink update preparing rollback state is not clear"
        )
    return dict(status)


def _validate_update_maintenance_status(
    status: dict[str, Any],
    *,
    session: str,
    expected_hardware_id: str,
) -> dict[str, Any]:
    """Prove maintenance mode and radio shutdown before update bytes."""
    bound_session = _validated_update_session(session)
    hardware_id = _validate_uplink_status_common(status)
    if hardware_id != normalized_hardware_id(expected_hardware_id):
        raise FlashError("maintenance uplink hardware_id mismatch")
    if status.get("recovery_mode") != "update_maintenance":
        raise FlashError("uplink update maintenance is not active")
    if status.get("update_session") != bound_session:
        raise FlashError("uplink update maintenance session mismatch")
    if status.get("ble_initialized") is not False:
        raise FlashError(
            "uplink Bluetooth must be uninitialized in update maintenance"
        )
    _validate_update_uplink_status(
        status.get("update_uplink"), session=bound_session
    )
    _validate_update_scanner_status(
        status.get("update_scanner"), session=bound_session
    )
    return dict(status)


def _update_maintenance_workers_ready(status: Mapping[str, Any]) -> bool:
    """Prove startup restore released both scanner UART lanes."""
    return all(
        _exact_uint(status.get(field), f"maintenance {field}") > 0
        for field in (
            "stack_main_free",
            "stack_uart_ble_free",
            "stack_uart_wifi_free",
        )
    )


_PERSISTED_GAME_STATUS_FIELDS = (
    "game_seed", "game_state", "game_active", "game_shield",
)


def _capture_persisted_game_state(
    status: Mapping[str, Any],
) -> tuple[Any, ...] | None:
    present = tuple(
        field in status for field in _PERSISTED_GAME_STATUS_FIELDS
    )
    if not any(present):
        return None
    if not all(present):
        raise FlashError("persisted game status is incomplete")
    values = tuple(
        status[field] for field in _PERSISTED_GAME_STATUS_FIELDS
    )
    if (
        type(values[0]) is not str or not values[0] or
        type(values[1]) is not str or not values[1] or
        type(values[2]) is not bool or
        type(values[3]) is not int or not 0 <= values[3] <= 100
    ):
        raise FlashError("persisted game status is malformed")
    return values


def _verify_persisted_game_state(
    status: Mapping[str, Any],
    expected: tuple[Any, ...] | None,
) -> None:
    if expected is None:
        return
    if _capture_persisted_game_state(status) != expected:
        raise FlashError("persisted game state changed across update mode")


def validate_legacy_uplink_bootstrap_status(status: dict[str, Any]) -> str:
    """Accept only the known pre-hardening badge application schema.

    This proof authorizes one reboot-to-ROM command, never a flash write.
    The ROM probe remains the immutable hardware identity authority.
    """
    if not isinstance(status, dict):
        raise FlashError("legacy FOF_STATUS payload is not an object")
    version = status.get("version")
    if type(version) is not str or \
            version not in LEGACY_USB_BOOTSTRAP_SOURCE_VERSIONS:
        raise FlashError(
            f"legacy uplink version is not allowlisted: {version!r}"
        )
    identity = {
        "firmware_name": "uplink-s3-fof_badge",
        "app_project": "fof_badge_uplink",
        "hardware_type": "seeed_xiao_esp32s3",
    }
    for field_name, wanted in identity.items():
        if status.get(field_name) != wanted:
            raise FlashError(
                f"legacy uplink {field_name} mismatch: "
                f"got {status.get(field_name)!r}, wanted {wanted!r}"
            )
    health = {
        "pending_verify": False,
        "recovery_mode": "normal",
        "safe_mode": False,
        "usb_control_alive": True,
    }
    for field_name, wanted in health.items():
        got = status.get(field_name)
        if type(wanted) is bool:
            valid = type(got) is bool and got is wanted
        else:
            valid = got == wanted
        if not valid:
            raise FlashError(
                f"legacy uplink {field_name} is unsafe: "
                f"got {got!r}, wanted {wanted!r}"
            )
    if type(status.get("scanner_uart_alive")) is not bool:
        raise FlashError(
            "legacy uplink scanner_uart_alive must be an exact boolean"
        )
    current_schema_fields = (
        "target", "project", "hardware_id", "running_partition",
        "rollback_state", "usb_health",
    )
    present = [
        field_name for field_name in current_schema_fields
        if field_name in status
    ]
    if present:
        raise FlashError(
            "legacy bootstrap refused a current-schema status containing: "
            + ", ".join(present)
        )
    return version


def _uses_update_maintenance(target_version: str) -> bool:
    relation = firmware_version_relation(
        target_version, UPDATE_MAINTENANCE_MIN_VERSION
    )
    return relation in ("equal", "newer")


def _allows_direct_update_maintenance_bootstrap(
    status: dict[str, Any],
    *,
    target_version: str,
) -> bool:
    """Authorize only the Bluetooth-free .78 -> .79 migration."""
    try:
        validate_uplink_application_status(status)
    except FlashError:
        return False
    return (
        type(target_version) is str and
        target_version == UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION and
        status.get("version") ==
        UPDATE_MAINTENANCE_BOOTSTRAP_SOURCE_VERSION and
        status.get("target") == "uplink-s3-fof_badge" and
        status.get("firmware_name") == "uplink-s3-fof_badge" and
        status.get("project") == "fof_badge_uplink" and
        status.get("app_project") == "fof_badge_uplink" and
        status.get("hardware_type") == "seeed_xiao_esp32s3" and
        status.get("pending_verify") is False and
        status.get("rollback_state") == "clear" and
        status.get("recovery_mode") == "normal"
    )


def _validate_direct_bootstrap_source_continuity(
    initial_status: dict[str, Any],
    fresh_status: dict[str, Any],
    *,
    target_version: str,
) -> dict[str, Any]:
    """Bind the one .78 -> .79 exception to one unchanged healthy boot."""
    if not _allows_direct_update_maintenance_bootstrap(
        initial_status, target_version=target_version
    ):
        raise FlashError("direct bootstrap source is not the exact healthy .78")
    if not _allows_direct_update_maintenance_bootstrap(
        fresh_status, target_version=target_version
    ):
        raise FlashError(
            "direct bootstrap fresh proof is not the exact healthy .78"
        )

    stable_fields = (
        "version", "target", "firmware_name", "project", "app_project",
        "hardware_type", "running_partition", "pending_verify",
        "rollback_state", "recovery_mode",
    )
    for field_name in stable_fields:
        if fresh_status.get(field_name) != initial_status.get(field_name):
            raise FlashError(
                f"direct bootstrap source {field_name} changed during "
                "prepare rejection"
            )
    initial_id = validate_uplink_application_status(initial_status)
    fresh_id = validate_uplink_application_status(fresh_status)
    if fresh_id != initial_id:
        raise FlashError(
            "direct bootstrap hardware_id changed during prepare rejection"
        )

    initial_responses = _exact_uint(
        initial_status["usb_health"].get("responses_completed"),
        "direct bootstrap initial usb_health.responses_completed",
    )
    fresh_responses = _exact_uint(
        fresh_status["usb_health"].get("responses_completed"),
        "direct bootstrap fresh usb_health.responses_completed",
    )
    if fresh_responses <= initial_responses:
        raise FlashError(
            "direct bootstrap fresh response counter did not advance"
        )

    forbidden_update_fields = (
        "update_session", "update_uplink", "update_scanner",
        "update_campaign",
    )
    if any(
        field_name in initial_status or field_name in fresh_status
        for field_name in forbidden_update_fields
    ):
        raise FlashError(
            "direct bootstrap source entered update maintenance"
        )

    if "uplink_ota" in initial_status or "uplink_ota" in fresh_status:
        idle_ota = {
            "state": "idle",
            "partition": "",
            "received": 0,
            "total": 0,
            "target_version": "",
            "last_error": "",
        }
        for label, status in (
            ("initial", initial_status),
            ("fresh", fresh_status),
        ):
            ota = status.get("uplink_ota")
            if type(ota) is not dict or set(ota) != set(idle_ota):
                raise FlashError(
                    f"direct bootstrap {label} OTA schema is invalid"
                )
            for field_name, wanted in idle_ota.items():
                got = ota.get(field_name)
                if type(got) is not type(wanted) or got != wanted:
                    raise FlashError(
                        f"direct bootstrap {label} OTA {field_name} "
                        "is not idle"
                    )

    if "uptime_s" in initial_status or "uptime_s" in fresh_status:
        initial_uptime = _exact_uint(
            initial_status.get("uptime_s"),
            "direct bootstrap initial uptime_s",
        )
        fresh_uptime = _exact_uint(
            fresh_status.get("uptime_s"),
            "direct bootstrap fresh uptime_s",
        )
        if fresh_uptime < initial_uptime:
            raise FlashError(
                "direct bootstrap source rebooted during prepare rejection"
            )
    return dict(fresh_status)


def _prove_direct_bootstrap_source_after_rejection(
    badge: Any,
    *,
    initial_status: dict[str, Any],
    target_version: str,
    deadline: float,
) -> dict[str, Any]:
    """Freshly prove the same exact .78 app after its legacy rejection."""
    if not _allows_direct_update_maintenance_bootstrap(
        initial_status, target_version=target_version
    ):
        raise FlashError("direct bootstrap source is not authorized")
    if type(deadline) not in (int, float) or isinstance(deadline, bool):
        raise FlashError("direct bootstrap reproof deadline is invalid")
    remaining = float(deadline) - time.monotonic()
    if remaining <= 0:
        raise FlashError("direct bootstrap reproof deadline expired")
    fresh_status = badge._prove_open_application(min(5.0, remaining))
    if time.monotonic() > float(deadline):
        raise FlashError("direct bootstrap reproof exceeded its deadline")
    return _validate_direct_bootstrap_source_continuity(
        initial_status,
        fresh_status,
        target_version=target_version,
    )


def _make_post_uplink_application_verifier() -> tuple[Any, Any]:
    field_names = (
        "hardware_id", "version", "running_partition",
        "responses_completed", "application_health_verified",
        "rollback_cleared",
    )
    exact_fields = set(field_names)
    issued: dict[
        int,
        tuple[PostUplinkApplicationEvidence, tuple[Any, ...]],
    ] = {}

    def revalidate(
        evidence: PostUplinkApplicationEvidence,
    ) -> PostUplinkApplicationEvidence:
        record = issued.get(id(evidence))
        if type(evidence) is not PostUplinkApplicationEvidence or \
                set(vars(evidence)) != exact_fields or record is None or \
                record[0] is not evidence:
            raise FlashError(
                "post-uplink application evidence lacks verifier attestation"
            )
        if type(evidence.hardware_id) is not str:
            raise FlashError("post-uplink evidence hardware_id is malformed")
        normalized = _normalized_rom_mac(
            evidence.hardware_id, label="post-uplink evidence hardware"
        )
        if evidence.hardware_id != normalized:
            raise FlashError(
                "post-uplink evidence hardware_id is not normalized"
            )
        if type(evidence.version) is not str or not evidence.version or \
                firmware_version_relation(
                    evidence.version, evidence.version
                ) != "equal":
            raise FlashError("post-uplink evidence version is malformed")
        if type(evidence.running_partition) is not str or \
                evidence.running_partition not in ("ota_0", "ota_1"):
            raise FlashError("post-uplink evidence partition is malformed")
        _exact_uint(
            evidence.responses_completed,
            "post-uplink evidence responses_completed",
        )
        if evidence.application_health_verified is not True or \
                evidence.rollback_cleared is not True:
            raise FlashError("post-uplink evidence proof flags are invalid")
        current = tuple(getattr(evidence, name) for name in field_names)
        if current != record[1]:
            raise FlashError(
                "post-uplink application evidence changed after issuance"
            )
        return evidence

    def verify_post_uplink_application(
        status: dict[str, Any],
        *,
        expected_hardware_id: str,
        expected_version: str,
        expected_partition: str,
    ) -> PostUplinkApplicationEvidence:
        """Convert strict post-transition status into attested USB proof."""
        if isinstance(status, RomFlashStageEvidence):
            raise FlashError(
                "ROM flash-stage evidence cannot prove application USB health"
            )
        if type(expected_hardware_id) is not str:
            raise FlashError(
                "expected uplink hardware_id must be an exact string"
            )
        expected_id = _normalized_rom_mac(
            expected_hardware_id, label="expected uplink hardware"
        )
        if type(expected_version) is not str or not expected_version:
            raise FlashError(
                "expected uplink version must be a non-empty string"
            )
        if type(expected_partition) is not str or expected_partition not in (
            "ota_0", "ota_1",
        ):
            raise FlashError(
                f"expected uplink partition is invalid: "
                f"{expected_partition!r}"
            )

        hardware_id = validate_uplink_application_status(status)
        if hardware_id != expected_id:
            raise FlashError(
                f"post-uplink hardware_id mismatch: got {hardware_id}, "
                f"wanted {expected_id}"
            )
        version = status.get("version")
        if type(version) is not str or version != expected_version:
            raise FlashError(
                f"post-uplink version mismatch: got {version!r}, "
                f"wanted exact {expected_version!r}"
            )
        partition = status.get("running_partition")
        if partition != expected_partition:
            raise FlashError(
                f"post-uplink partition mismatch: got {partition!r}, "
                f"wanted {expected_partition!r}"
            )
        if status.get("pending_verify") is not False:
            raise FlashError(
                "post-uplink application is still pending verification"
            )
        if status.get("rollback_state") != "clear":
            raise FlashError("post-uplink rollback state is not clear")
        if status.get("recovery_mode") != "normal":
            raise FlashError("post-uplink recovery mode is not normal")
        responses = _exact_uint(
            status["usb_health"].get("responses_completed"),
            "post-uplink usb_health.responses_completed",
        )
        values = (
            hardware_id, version, partition, responses, True, True,
        )
        evidence = object.__new__(PostUplinkApplicationEvidence)
        for name, value in zip(field_names, values):
            object.__setattr__(evidence, name, value)
        issued[id(evidence)] = (evidence, values)
        try:
            return revalidate(evidence)
        except BaseException:
            issued.pop(id(evidence), None)
            raise

    return verify_post_uplink_application, revalidate


(
    verify_post_uplink_application,
    _revalidate_post_uplink_application_evidence,
) = _make_post_uplink_application_verifier()
del _make_post_uplink_application_verifier


def _alternate_uplink_partition(partition: str) -> str:
    if type(partition) is not str or partition not in ("ota_0", "ota_1"):
        raise FlashError(
            f"uplink running partition is invalid: {partition!r}"
        )
    return "ota_1" if partition == "ota_0" else "ota_0"


def _revalidate_post_uplink_expectation(
    expectation: _PostUplinkExpectation,
) -> _PostUplinkExpectation:
    fields = {
        "expected_hardware_id", "expected_version", "expected_partition",
        "expected_sha256", "expected_size",
        "pre_version", "pre_partition", "mutation_expected", "source",
        "update_session",
    }
    if type(expectation) is not _PostUplinkExpectation or \
            set(vars(expectation)) != fields:
        raise FlashError("post-uplink expectation has an invalid shape")
    if type(expectation.expected_hardware_id) is not str:
        raise FlashError("post-uplink expected hardware_id must be a string")
    expected_id = _normalized_rom_mac(
        expectation.expected_hardware_id,
        label="post-uplink expected hardware",
    )
    if expectation.expected_hardware_id != expected_id:
        raise FlashError("post-uplink expected hardware_id is not normalized")
    if type(expectation.expected_version) is not str or not \
            _ORDERED_VERSION_RE.fullmatch(expectation.expected_version):
        raise FlashError("post-uplink expected version is invalid")
    if type(expectation.expected_partition) is not str or \
            expectation.expected_partition not in ("ota_0", "ota_1"):
        raise FlashError("post-uplink expected partition is invalid")
    _validated_update_session(expectation.update_session)
    if type(expectation.mutation_expected) is not bool:
        raise FlashError("post-uplink mutation flag must be an exact boolean")
    if type(expectation.source) is not str or expectation.source not in {
        "rom", "current", "committed", "terminal_unavailable",
    }:
        raise FlashError("post-uplink expectation source is invalid")

    pre_missing = expectation.pre_version is None and \
        expectation.pre_partition is None
    pre_complete = type(expectation.pre_version) is str and \
        type(expectation.pre_partition) is str
    if not pre_missing and not pre_complete:
        raise FlashError("post-uplink pre-state is incomplete")
    if pre_complete:
        if not _ORDERED_VERSION_RE.fullmatch(expectation.pre_version):
            raise FlashError("post-uplink pre-state version is invalid")
        if expectation.pre_partition not in ("ota_0", "ota_1"):
            raise FlashError("post-uplink pre-state partition is invalid")

    if expectation.source == "rom":
        if expectation.mutation_expected is not True or not pre_missing or \
                expectation.expected_partition != "ota_0":
            raise FlashError("ROM post-uplink expectation is inconsistent")
    elif expectation.source == "current":
        if expectation.mutation_expected is not False or not pre_complete or \
                expectation.pre_version != expectation.expected_version or \
                expectation.pre_partition != expectation.expected_partition or \
                expectation.expected_sha256 != "" or \
                expectation.expected_size != 0:
            raise FlashError("current post-uplink expectation is inconsistent")
    else:
        if expectation.mutation_expected is not True or not pre_complete or \
                expectation.expected_partition != \
                _alternate_uplink_partition(expectation.pre_partition):
            raise FlashError("mutating post-uplink expectation is inconsistent")
        relation = firmware_version_relation(
            expectation.expected_version, expectation.pre_version
        )
        if relation not in ("newer", "equal"):
            raise FlashError(
                "mutating post-uplink version transition is not safe"
            )
    if expectation.mutation_expected:
        if type(expectation.expected_sha256) is not str or not re.fullmatch(
            r"[0-9a-f]{64}", expectation.expected_sha256
        ):
            raise FlashError("post-uplink expected sha256 is invalid")
        if type(expectation.expected_size) is not int or not (
            1 <= expectation.expected_size <= 0xFFFFFFFF
        ):
            raise FlashError("post-uplink expected size is invalid")
    return expectation


def _classify_uplink_update_receipt(
    receipt: dict[str, Any], *, pre_status: dict[str, Any],
    target_version: str, expected_sha256: str, expected_size: int,
    update_session: str,
) -> _PostUplinkExpectation:
    """Turn one exact application OTA result into a later-proof contract."""
    if type(receipt) is not dict:
        raise FlashError("uplink update receipt is not an exact object")
    if type(target_version) is not str or not \
            _ORDERED_VERSION_RE.fullmatch(target_version):
        raise FlashError("uplink update target version is invalid")
    hardware_id = validate_uplink_application_status(pre_status)
    pre_version = pre_status.get("version")
    pre_partition = pre_status.get("running_partition")
    if type(pre_version) is not str or type(pre_partition) is not str:
        raise FlashError("uplink pre-update state is malformed")
    relation = firmware_version_relation(target_version, pre_version)
    if relation not in ("newer", "equal"):
        raise FlashError(
            f"uplink update transition is unsafe: {pre_version!r} -> "
            f"{target_version!r}"
        )
    bound_session = _validated_update_session(update_session)

    skipped_keys = {
        "ok", "skipped", "phase", "hardware_id", "version", "partition",
    }
    terminal_keys = {
        "ok", "uncertain", "phase", "expected_partition", "hardware_id",
        "version", "received", "total", "error",
    }
    if set(receipt) == skipped_keys:
        receipt_hardware_id = receipt.get("hardware_id")
        if type(receipt_hardware_id) is not str:
            raise FlashError(
                "uplink current receipt hardware_id must be an exact string"
            )
        receipt_id = receipt_hardware_id.strip().lower()
        if not _HARDWARE_ID_RE.fullmatch(receipt_id):
            raise FlashError(
                "uplink current receipt hardware_id is invalid"
            )
        if receipt_hardware_id != receipt_id:
            raise FlashError(
                "uplink current receipt hardware_id is not normalized"
            )
        expected = {
            "ok": True,
            "skipped": True,
            "phase": "current",
            "hardware_id": hardware_id,
            "version": target_version,
            "partition": pre_partition,
        }
        for key, wanted in expected.items():
            got = receipt_id if key == "hardware_id" else receipt.get(key)
            if type(got) is not type(wanted) or got != wanted:
                raise FlashError(
                    f"uplink current receipt {key} mismatch"
                )
        if relation != "equal":
            raise FlashError("uplink current receipt cannot skip an older app")
        expectation = _PostUplinkExpectation(
            expected_hardware_id=hardware_id,
            expected_version=target_version,
            expected_partition=pre_partition,
            expected_sha256="",
            expected_size=0,
            pre_version=pre_version,
            pre_partition=pre_partition,
            mutation_expected=False,
            source="current",
            update_session=bound_session,
        )
        return _revalidate_post_uplink_expectation(expectation)

    alternate = _alternate_uplink_partition(pre_partition)
    if set(receipt) == UPLINK_OTA_RECEIPT_KEYS:
        total = _exact_uint(receipt.get("total"), "uplink committed total")
        if type(expected_size) is not int or total != expected_size or \
                total <= 0:
            raise FlashError("uplink committed total does not match artifact")
        if type(expected_sha256) is not str or not re.fullmatch(
            r"[0-9a-f]{64}", expected_sha256
        ):
            raise FlashError("uplink committed artifact sha256 is invalid")
        validate_uplink_ota_receipt(
            receipt,
            phase="committed",
            partition=alternate,
            received=total,
            total=total,
            credit_bytes=0,
            reboot_required=True,
        )
        expectation = _PostUplinkExpectation(
            expected_hardware_id=hardware_id,
            expected_version=target_version,
            expected_partition=alternate,
            expected_sha256=expected_sha256,
            expected_size=expected_size,
            pre_version=pre_version,
            pre_partition=pre_partition,
            mutation_expected=True,
            source="committed",
            update_session=bound_session,
        )
        return _revalidate_post_uplink_expectation(expectation)

    if set(receipt) == terminal_keys:
        expected = {
            "ok": False,
            "uncertain": True,
            "phase": "terminal_unavailable",
            "expected_partition": alternate,
            "hardware_id": hardware_id,
            "version": target_version,
        }
        receipt_hardware_id = receipt.get("hardware_id")
        if type(receipt_hardware_id) is not str:
            raise FlashError(
                "uplink terminal receipt hardware_id must be an exact string"
            )
        receipt_id = receipt_hardware_id.strip().lower()
        if not _HARDWARE_ID_RE.fullmatch(receipt_id):
            raise FlashError(
                "uplink terminal receipt hardware_id is invalid"
            )
        if receipt_hardware_id != receipt_id:
            raise FlashError(
                "uplink terminal receipt hardware_id is not normalized"
            )
        for key, wanted in expected.items():
            got = receipt_id if key == "hardware_id" else receipt.get(key)
            if type(got) is not type(wanted) or got != wanted:
                raise FlashError(
                    f"uplink terminal receipt {key} mismatch"
                )
        received = _exact_uint(
            receipt.get("received"), "uplink terminal received"
        )
        total = _exact_uint(receipt.get("total"), "uplink terminal total")
        if total <= 0 or received != total:
            raise FlashError(
                "uplink terminal receipt must prove every byte was received"
            )
        if total != expected_size or type(expected_sha256) is not str or \
                not re.fullmatch(r"[0-9a-f]{64}", expected_sha256):
            raise FlashError(
                "uplink terminal receipt does not match frozen artifact"
            )
        error = receipt.get("error")
        if type(error) is not str or not error:
            raise FlashError("uplink terminal receipt error is missing")
        expectation = _PostUplinkExpectation(
            expected_hardware_id=hardware_id,
            expected_version=target_version,
            expected_partition=alternate,
            expected_sha256=expected_sha256,
            expected_size=expected_size,
            pre_version=pre_version,
            pre_partition=pre_partition,
            mutation_expected=True,
            source="terminal_unavailable",
            update_session=bound_session,
        )
        return _revalidate_post_uplink_expectation(expectation)

    raise FlashError("uplink update receipt schema mismatch")


def _expectation_from_rom_flash(
    stage: RomFlashStageEvidence,
    *,
    layout_version: str,
    artifacts: FrozenArtifactSet,
    update_session: str,
) -> _PostUplinkExpectation:
    """Validate ROM-stage facts without upgrading them to app-health proof."""
    if type(stage) is not RomFlashStageEvidence:
        raise FlashError("ROM flash stage evidence has an invalid shape")
    _attest_rom_flash_stage(
        stage,
        layout_version=layout_version,
        artifacts=artifacts,
    )
    firmware = _frozen_firmware_bytes(artifacts, role="uplink")
    expectation = _PostUplinkExpectation(
        expected_hardware_id=stage.base_mac,
        expected_version=layout_version,
        expected_partition="ota_0",
        expected_sha256=hashlib.sha256(firmware).hexdigest(),
        expected_size=len(firmware),
        pre_version=None,
        pre_partition=None,
        mutation_expected=True,
        source="rom",
        update_session=_validated_update_session(update_session),
    )
    return _revalidate_post_uplink_expectation(expectation)


def wait_for_post_uplink_application(
    expectation: _PostUplinkExpectation,
    timeout_s: float = POST_UPLINK_APPLICATION_TIMEOUT_S,
) -> tuple[UsbDescriptorRecord, PostUplinkApplicationEvidence]:
    """Follow one immutable MAC until the exact safe post-state is proven."""
    expected = _revalidate_post_uplink_expectation(expectation)
    if not isinstance(timeout_s, (int, float)) or isinstance(timeout_s, bool) \
            or not (0 < float(timeout_s) <= ESPTOOL_TIMEOUT_MAX_S):
        raise FlashError(
            "post-uplink application timeout must be in "
            f"(0, {ESPTOOL_TIMEOUT_MAX_S:g}] seconds"
        )
    deadline = time.monotonic() + float(timeout_s)

    def pace_transition() -> None:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise FlashError("post-uplink application deadline timed out")
        time.sleep(min(POST_UPLINK_TRANSITION_POLL_S, remaining))

    bound_descriptor: UsbDescriptorRecord | None = None
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise FlashError("post-uplink application deadline timed out")
        if bound_descriptor is None:
            descriptor, status = wait_for_application_port(
                expected.expected_hardware_id, remaining
            )
        else:
            descriptor = bound_descriptor
            try:
                status = probe_application(
                    descriptor,
                    min(APPLICATION_DISCOVERY_PROBE_SLICE_S, remaining),
                )
            except SerialReadTimeout:
                bound_descriptor = None
                pace_transition()
                continue
            except SerialTransportError as exc:
                if not exc.terminal_unavailable:
                    raise
                bound_descriptor = None
                pace_transition()
                continue
            if status is None:
                bound_descriptor = None
                pace_transition()
                continue
        if time.monotonic() >= deadline:
            raise FlashError("post-uplink application deadline timed out")
        if type(descriptor) is not UsbDescriptorRecord:
            raise FlashError("post-uplink application descriptor is malformed")
        _validated_rom_port(descriptor.device)
        hardware_id = validate_uplink_application_status(status)
        if hardware_id != expected.expected_hardware_id:
            raise FlashError(
                f"post-uplink hardware_id mismatch: got {hardware_id}, "
                f"wanted {expected.expected_hardware_id}"
            )
        bound_descriptor = descriptor
        version = status.get("version")
        partition = status.get("running_partition")
        pending = status.get("pending_verify")

        if version == expected.expected_version and \
                partition == expected.expected_partition:
            if pending is True:
                # validate_uplink_application_status() already proved the only
                # safe pending rollback/recovery combinations.
                pace_transition()
                continue
            evidence = verify_post_uplink_application(
                status,
                expected_hardware_id=expected.expected_hardware_id,
                expected_version=expected.expected_version,
                expected_partition=expected.expected_partition,
            )
            return descriptor, _revalidate_post_uplink_application_evidence(
                evidence
            )

        exact_safe_pre_state = (
            expected.mutation_expected and expected.pre_version is not None and
            version == expected.pre_version and
            partition == expected.pre_partition and pending is False and
            status.get("rollback_state") == "clear" and
            status.get("recovery_mode") == "normal"
        )
        if exact_safe_pre_state:
            pace_transition()
            continue
        raise FlashError(
            "unexpected post-uplink application state: "
            f"version={version!r}, partition={partition!r}, "
            f"pending_verify={pending!r}"
        )


def _require_post_uplink_evidence(
    evidence: PostUplinkApplicationEvidence,
    expectation: _PostUplinkExpectation,
) -> PostUplinkApplicationEvidence:
    expected = _revalidate_post_uplink_expectation(expectation)
    proven = _revalidate_post_uplink_application_evidence(evidence)
    if (
        proven.hardware_id != expected.expected_hardware_id or
        proven.version != expected.expected_version or
        proven.running_partition != expected.expected_partition
    ):
        raise FlashError(
            "post-uplink application evidence does not satisfy its receipt"
        )
    return proven


def _badge_theme_values(
    theme: dict[str, Any],
) -> tuple[int, str, str, int, tuple[int, ...]]:
    if type(theme) is not dict or set(theme) != BADGE_THEME_KEYS:
        raise FlashError("badge theme schema mismatch")
    version = theme.get("version")
    brightness = theme.get("brightness")
    palette = theme.get("palette")
    background = theme.get("background")
    accents = theme.get("accents")
    if type(version) is not int or version != 1:
        raise FlashError("badge theme version must be exact integer 1")
    if type(brightness) is not int or not 25 <= brightness <= 100:
        raise FlashError(
            "badge theme brightness must be an exact integer from 25 to 100"
        )
    if type(palette) is not str or \
            palette.lower() not in BADGE_THEME_PALETTES:
        raise FlashError(f"badge theme palette is invalid: {palette!r}")
    if type(background) is not str or \
            background.lower() not in BADGE_THEME_BACKGROUNDS:
        raise FlashError(
            f"badge theme background is invalid: {background!r}"
        )
    if type(accents) is not dict or \
            set(accents) != set(BADGE_THEME_ACCENT_ORDER):
        raise FlashError("badge theme accent schema mismatch")
    colors: list[int] = []
    for key in BADGE_THEME_ACCENT_ORDER:
        color = accents.get(key)
        if type(color) is not int or not 0 <= color <= 0xFFFF:
            raise FlashError(
                f"badge theme accent {key} must be an exact 16-bit integer"
            )
        colors.append(color)
    return version, palette, background, brightness, tuple(colors)


def _badge_theme_hash(theme: dict[str, Any]) -> int:
    """Mirror esp32/shared/badge_theme.c's byte-exact FNV-1a hash."""
    version, palette, background, brightness, colors = \
        _badge_theme_values(theme)
    value = 2166136261

    def add(byte: int) -> None:
        nonlocal value
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF

    add(version)
    add(brightness)
    for byte in palette.encode("ascii"):
        add(byte)
    add(0)
    for byte in background.encode("ascii"):
        add(byte)
    add(0)
    for color in colors:
        add((color >> 8) & 0xFF)
        add(color & 0xFF)
    return value


def _revalidate_badge_theme_snapshot(
    snapshot: _BadgeThemeSnapshot,
) -> _BadgeThemeSnapshot:
    fields = {
        "version", "palette", "background", "brightness", "accents",
        "theme_hash",
    }
    if type(snapshot) is not _BadgeThemeSnapshot or \
            set(vars(snapshot)) != fields or \
            type(snapshot.accents) is not tuple or \
            len(snapshot.accents) != len(BADGE_THEME_ACCENT_ORDER):
        raise FlashError("badge theme snapshot has an invalid shape")
    theme = {
        "version": snapshot.version,
        "palette": snapshot.palette,
        "background": snapshot.background,
        "brightness": snapshot.brightness,
        "accents": dict(zip(BADGE_THEME_ACCENT_ORDER, snapshot.accents)),
    }
    calculated = _badge_theme_hash(theme)
    if type(snapshot.theme_hash) is not int or \
            not 0 <= snapshot.theme_hash <= 0xFFFFFFFF or \
            snapshot.theme_hash != calculated:
        raise FlashError("badge theme snapshot hash mismatch")
    return snapshot


def _snapshot_badge_theme(status: dict[str, Any]) -> _BadgeThemeSnapshot:
    if type(status) is not dict:
        raise FlashError("FOF_STATUS theme proof must be an exact object")
    theme = status.get("theme")
    version, palette, background, brightness, accents = \
        _badge_theme_values(theme)
    reported_hash = status.get("theme_hash")
    if type(reported_hash) is not int or \
            not 0 <= reported_hash <= 0xFFFFFFFF:
        raise FlashError("FOF_STATUS theme_hash must be an exact 32-bit integer")
    calculated_hash = _badge_theme_hash(theme)
    if reported_hash != calculated_hash:
        raise FlashError(
            "FOF_STATUS badge theme hash does not match its exact theme"
        )
    return _revalidate_badge_theme_snapshot(_BadgeThemeSnapshot(
        version=version,
        palette=palette,
        background=background,
        brightness=brightness,
        accents=accents,
        theme_hash=reported_hash,
    ))


def _badge_theme_payload(snapshot: _BadgeThemeSnapshot) -> dict[str, Any]:
    proven = _revalidate_badge_theme_snapshot(snapshot)
    return {
        "version": proven.version,
        "palette": proven.palette,
        "background": proven.background,
        "brightness": proven.brightness,
        "accents": dict(zip(BADGE_THEME_ACCENT_ORDER, proven.accents)),
    }


def _temporary_badge_theme(
    original: _BadgeThemeSnapshot,
) -> _BadgeThemeSnapshot:
    proven = _revalidate_badge_theme_snapshot(original)
    theme = _badge_theme_payload(proven)
    theme["brightness"] = 99 if proven.brightness == 100 \
        else proven.brightness + 1
    return _BadgeThemeSnapshot(
        version=theme["version"],
        palette=theme["palette"],
        background=theme["background"],
        brightness=theme["brightness"],
        accents=tuple(
            theme["accents"][key] for key in BADGE_THEME_ACCENT_ORDER
        ),
        theme_hash=_badge_theme_hash(theme),
    )


def _validate_badge_theme_ack(
    ack: dict[str, Any], *, expected_hash: int,
) -> dict[str, Any]:
    if type(expected_hash) is not int or not 0 <= expected_hash <= 0xFFFFFFFF:
        raise FlashError("expected badge theme hash is invalid")
    if type(ack) is not dict or set(ack) != BADGE_THEME_ACK_KEYS:
        raise FlashError("badge theme acknowledgement schema mismatch")
    expected = {
        "message": "badge theme updated",
        "theme_hash": expected_hash,
        "persisted": False,
        "reboot_required": False,
    }
    for key, wanted in expected.items():
        got = ack.get(key)
        if type(got) is not type(wanted) or got != wanted:
            raise FlashError(
                f"badge theme acknowledgement {key} mismatch: "
                f"got {got!r}, wanted {wanted!r}"
            )
    return ack


def _fresh_post_uplink_application_evidence(
    status: dict[str, Any], *, expectation: _PostUplinkExpectation,
    previous: PostUplinkApplicationEvidence, label: str,
) -> PostUplinkApplicationEvidence:
    expected = _revalidate_post_uplink_expectation(expectation)
    before = _require_post_uplink_evidence(previous, expected)
    current = verify_post_uplink_application(
        status,
        expected_hardware_id=expected.expected_hardware_id,
        expected_version=expected.expected_version,
        expected_partition=expected.expected_partition,
    )
    current = _require_post_uplink_evidence(current, expected)
    if current.responses_completed <= before.responses_completed:
        raise FlashError(
            f"{label} did not prove a strictly newer USB response counter: "
            f"before={before.responses_completed}, "
            f"after={current.responses_completed}"
        )
    return current


def _prove_reversible_usb_theme_control(
    badge: BadgeSerial, *, initial_status: dict[str, Any],
    expectation: _PostUplinkExpectation,
    initial_evidence: PostUplinkApplicationEvidence,
    restored_status_validator: Callable[[dict[str, Any]], None] | None = None,
) -> PostUplinkApplicationEvidence:
    """Prove Android-compatible USB control, then restore exact prior state."""
    if restored_status_validator is not None and not callable(
        restored_status_validator
    ):
        raise FlashError("restored badge status validator must be callable")
    expected = _revalidate_post_uplink_expectation(expectation)
    supplied_evidence = _require_post_uplink_evidence(
        initial_evidence, expected
    )
    status_evidence = verify_post_uplink_application(
        initial_status,
        expected_hardware_id=expected.expected_hardware_id,
        expected_version=expected.expected_version,
        expected_partition=expected.expected_partition,
    )
    status_evidence = _require_post_uplink_evidence(status_evidence, expected)
    if status_evidence != supplied_evidence:
        raise FlashError(
            "initial badge theme status does not match its USB proof evidence"
        )

    original = _snapshot_badge_theme(initial_status)
    temporary = _temporary_badge_theme(original)
    original_payload = _badge_theme_payload(original)
    temporary_payload = _badge_theme_payload(temporary)
    primary: BaseException | None = None
    restoration_errors: list[BaseException] = []
    temporary_evidence: PostUplinkApplicationEvidence | None = None
    final_evidence: PostUplinkApplicationEvidence | None = None
    restored_status: dict[str, Any] | None = None
    restored_state_proven = False
    control_started = False

    try:
        control_started = True
        ack = badge.ctl({
            "cmd": "badge_theme",
            "theme": temporary_payload,
            "persist": False,
        })
        _validate_badge_theme_ack(ack, expected_hash=temporary.theme_hash)
        temporary_status = badge.status()
        temporary_evidence = _fresh_post_uplink_application_evidence(
            temporary_status,
            expectation=expected,
            previous=supplied_evidence,
            label="temporary badge theme readback",
        )
        if _snapshot_badge_theme(temporary_status) != temporary:
            raise FlashError("temporary badge theme readback mismatch")
    except BaseException as exc:
        primary = exc
    finally:
        if control_started:
            try:
                restore_ack = badge.ctl({
                    "cmd": "badge_theme",
                    "theme": original_payload,
                    "persist": False,
                })
                _validate_badge_theme_ack(
                    restore_ack, expected_hash=original.theme_hash
                )
            except BaseException as exc:
                restoration_errors.append(exc)

            try:
                restored_status = badge.status()
                final_evidence = _fresh_post_uplink_application_evidence(
                    restored_status,
                    expectation=expected,
                    previous=temporary_evidence or supplied_evidence,
                    label="restored badge theme readback",
                )
                if _snapshot_badge_theme(restored_status) != original:
                    raise FlashError("restored badge theme readback mismatch")
                restored_state_proven = True
            except BaseException as exc:
                restoration_errors.append(exc)

    post_control_error: BaseException | None = None
    if restored_state_proven and restored_status_validator is not None:
        try:
            restored_status_validator(restored_status)
        except BaseException as exc:
            post_control_error = exc

    if primary is not None:
        for error in restoration_errors:
            _add_secondary_failure_note(
                primary, error, scope="badge theme restoration"
            )
        if post_control_error is not None:
            _add_secondary_failure_note(
                primary, post_control_error,
                scope="post-control scanner proof",
            )
        raise primary
    if restoration_errors:
        failure = FlashError(
            "badge theme restoration failed: "
            f"{restoration_errors[0]}"
        )
        failure.__cause__ = restoration_errors[0]
        for error in restoration_errors[1:]:
            _add_secondary_failure_note(
                failure, error, scope="badge theme restoration"
            )
        if post_control_error is not None:
            _add_secondary_failure_note(
                failure, post_control_error,
                scope="post-control scanner proof",
            )
        raise failure
    if post_control_error is not None:
        raise post_control_error
    if final_evidence is None:
        raise FlashError("badge theme restoration produced no final USB proof")
    return final_evidence


def validate_uplink_ota_receipt(
    receipt: dict[str, Any], *, phase: str, partition: str,
    received: int, total: int, credit_bytes: int,
    reboot_required: bool,
) -> dict[str, Any]:
    if not isinstance(receipt, dict) or set(receipt) != UPLINK_OTA_RECEIPT_KEYS:
        raise FlashError("uplink OTA receipt schema mismatch")
    if type(receipt.get("ok")) is not bool or \
            type(receipt.get("retryable")) is not bool or \
            type(receipt.get("reboot_required")) is not bool:
        raise FlashError("uplink OTA receipt boolean type mismatch")
    for key in ("received", "total", "credit_bytes"):
        _exact_uint(receipt.get(key), f"uplink OTA receipt {key}")
    if not isinstance(receipt.get("phase"), str) or \
            not isinstance(receipt.get("partition"), str) or \
            not isinstance(receipt.get("error"), str):
        raise FlashError("uplink OTA receipt string type mismatch")
    if receipt["phase"] == "aborted":
        raise FlashError("uplink OTA aborted definitively")
    expected = {
        "ok": True,
        "phase": phase,
        "partition": partition,
        "received": received,
        "total": total,
        "credit_bytes": credit_bytes,
        "retryable": False,
        "reboot_required": reboot_required,
        "error": "",
    }
    for key, wanted in expected.items():
        if type(receipt.get(key)) is not type(wanted) or receipt.get(key) != wanted:
            raise FlashError(f"uplink OTA {phase} {key} mismatch")
    return receipt


def validate_uplink_ota_aborted_receipt(
    receipt: dict[str, Any],
    *,
    partition: str,
    received: int,
    total: int,
) -> dict[str, Any]:
    """Require the sole receipt that authorizes a byte-zero OTA restart."""
    if type(receipt) is not dict or set(receipt) != UPLINK_OTA_RECEIPT_KEYS:
        raise FlashError("uplink OTA aborted receipt schema mismatch")
    expected = {
        "ok": True,
        "phase": "aborted",
        "partition": partition,
        "received": received,
        "total": total,
        "credit_bytes": 0,
        "retryable": True,
        "reboot_required": False,
        "error": "",
    }
    for key, wanted in expected.items():
        got = receipt.get(key)
        if type(got) is not type(wanted) or got != wanted:
            raise FlashError(f"uplink OTA aborted {key} mismatch")
    return dict(receipt)


def _retry_passive_read(
    action: Any, *, deadline: float, label: str,
    logical_clock: list[float],
) -> Any:
    """Retry dropped application replies without resetting the USB device."""
    saw_activity = False
    partial_frame = False
    attempts = 0
    last_timeout: SerialReadTimeout | None = None
    while True:
        now = max(time.monotonic(), logical_clock[0])
        remaining = deadline - now
        if remaining <= 0:
            timeout = SerialReadTimeout(
                f"timed out waiting for {label}",
                saw_activity=saw_activity,
                partial_frame=partial_frame,
            )
            if last_timeout is not None:
                raise timeout from last_timeout
            raise timeout
        if attempts == 0:
            attempt_timeout = min(PASSIVE_RETRY_SLICE_S, remaining / 2.0)
        else:
            attempt_timeout = min(PASSIVE_RETRY_SLICE_S, remaining)
        if attempt_timeout <= 0:
            timeout = SerialReadTimeout(
                f"timed out waiting for {label}",
                saw_activity=saw_activity,
                partial_frame=partial_frame,
            )
            if last_timeout is not None:
                raise timeout from last_timeout
            raise timeout
        attempts += 1
        try:
            result = action(attempt_timeout)
            if max(time.monotonic(), logical_clock[0]) > deadline:
                raise SerialReadTimeout(
                    f"timed out waiting for {label}",
                    saw_activity=True,
                    partial_frame=False,
                )
            return result
        except SerialReadTimeout as exc:
            last_timeout = exc
            saw_activity = saw_activity or exc.saw_activity
            partial_frame = partial_frame or exc.partial_frame
            logical_clock[0] = max(
                time.monotonic(), now + attempt_timeout
            )


def _prove_badge_application(badge: Any,
                             timeout_s: float) -> dict[str, Any]:
    deadline = time.monotonic() + max(timeout_s, 0)
    activity_proven = False

    def read_once(action: Any, label: str) -> Any:
        nonlocal activity_proven
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise SerialReadTimeout(
                f"timed out waiting for {label}",
                saw_activity=activity_proven,
                partial_frame=False,
            )
        try:
            result = action(remaining)
        except SerialReadTimeout as exc:
            raise SerialReadTimeout(
                f"timed out waiting for {label}",
                saw_activity=activity_proven or exc.saw_activity,
                partial_frame=exc.partial_frame,
            ) from exc
        if time.monotonic() > deadline:
            raise SerialReadTimeout(
                f"timed out waiting for {label}",
                saw_activity=True,
                partial_frame=False,
            )
        activity_proven = True
        return result

    baseline = read_once(
        lambda attempt_s: badge.status(timeout_s=attempt_s),
        "baseline FOF_STATUS",
    )
    baseline_id = validate_uplink_application_status(baseline)
    baseline_count = _exact_uint(
        baseline["usb_health"].get("responses_completed"),
        "baseline usb_health.responses_completed",
    )
    def ping(attempt_s: float) -> str:
        badge.write_line("FOF_PING")
        return badge.read_prefixed_text("FOF_PONG:", attempt_s)

    pong = read_once(ping, "FOF_PONG")
    final = read_once(
        lambda attempt_s: badge.status(timeout_s=attempt_s),
        "final FOF_STATUS",
    )
    final_id = validate_uplink_application_status(final)
    if final_id != baseline_id:
        raise FlashError(
            f"uplink hardware_id changed during application proof: "
            f"{baseline_id} -> {final_id}"
        )
    if pong != final.get("version"):
        raise FlashError(
            f"FOF_PONG version mismatch: got {pong!r}, "
            f"status has {final.get('version')!r}"
        )
    final_count = _exact_uint(
        final["usb_health"].get("responses_completed"),
        "final usb_health.responses_completed",
    )
    if final_count < baseline_count + 2:
        raise FlashError(
            "FOF_STATUS response counter did not prove fresh PONG/status "
            f"frames: baseline={baseline_count}, final={final_count}"
        )
    return final


class BadgeSerial:
    def __init__(self, descriptor: UsbDescriptorRecord, dry_run: bool,
                 expected_hardware_id: str | None = None) -> None:
        if type(descriptor) is not UsbDescriptorRecord:
            raise FlashError(
                "BadgeSerial requires an immutable USB descriptor record"
            )
        self._descriptor = descriptor
        self.dry_run = dry_run
        self.ser: Any = None
        self.expected_hardware_id = (
            normalized_hardware_id(expected_hardware_id)
            if expected_hardware_id is not None
            else descriptor.serial_number
        )
        if self.expected_hardware_id != descriptor.serial_number:
            raise FlashError(
                "BadgeSerial descriptor does not match the trusted uplink "
                "hardware_id"
            )
        self._trusted_location = descriptor.location
        self._rx_buffer = bytearray()
        self._rx_bytes_seen = 0
        self._update_session: str | None = None
        self._binary_transport_loss_at: float | None = None
        self._reconciled_scanner_generation: int | None = None

    @property
    def port(self) -> str:
        return self._descriptor.device

    def __enter__(self) -> "BadgeSerial":
        if self.dry_run:
            return self
        self._open_serial()
        return self

    def _open_serial(self) -> None:
        if self.ser is not None:
            raise SerialTransportError(
                "descriptor-bound serial transport cannot reopen"
            )
        try:
            ser = open_bound_application_serial(
                self._descriptor,
                expected_uplink_serial=self.expected_hardware_id,
                baudrate=115200,
                timeout=0.15,
                write_timeout=3,
            )
        except UsbDescriptorBindingError as exc:
            raise SerialTransportError(
                f"cannot open descriptor-bound serial transport "
                f"{self.port}: {exc}",
                terminal_unavailable=True,
            ) from exc
        self.ser = ser
        self._rx_buffer.clear()
        self._rx_bytes_seen = 0

    def _close_serial(self) -> None:
        handle = self.ser
        self.ser = None
        if handle is None:
            return
        try:
            handle.close()
        except BaseException as exc:
            raise SerialTransportError(
                f"serial transport close failed on {self.port}: {exc}",
                terminal_unavailable=True,
            ) from exc

    def _read_update_mode_or_control_error(
        self,
        timeout_s: float,
        *,
        allowed_schema_ids: tuple[HostJsonSchemaId, ...],
    ) -> dict[str, Any]:
        if self.dry_run:
            return {"ok": True, "dry_run": True}
        deadline = time.monotonic() + max(timeout_s, 0)
        update_prefix = b"FOF_UPDATE_MODE:"
        error_prefix = b"FOF_CTL_ERROR:"
        detection_prefix = b"FOF_DET:"
        legacy_unknown = b"FOF_ERROR:unknown command"
        saw_frame = False
        saw_reserved_frame = False
        saw_legacy_unknown = False

        def raise_terminal_timeout(
            cause: SerialReadTimeout | None = None,
        ) -> NoReturn:
            partial_bytes = bytes(self._rx_buffer)
            partial_frame = bool(partial_bytes) or (
                cause.partial_frame if cause is not None else False
            )
            partial_may_be_reserved = bool(partial_bytes) and (
                b"FOF".startswith(partial_bytes) or
                partial_bytes.startswith(b"FOF")
            )
            inconsistent_partial = (
                cause is not None and
                cause.partial_frame and
                not partial_bytes
            )
            if saw_legacy_unknown and not saw_reserved_frame and \
                    not partial_may_be_reserved and \
                    not inconsistent_partial:
                error = _DeferredLegacyUpdateMaintenanceMarker(
                    "prepare_update is unavailable on the proven source"
                )
                if cause is not None:
                    raise error from cause
                raise error
            timeout = SerialReadTimeout(
                f"timed out waiting for FOF_UPDATE_MODE: on {self.port}",
                saw_activity=saw_frame or (
                    cause.saw_activity if cause is not None else False
                ),
                partial_frame=partial_frame,
            )
            if cause is not None:
                raise timeout from cause
            raise timeout

        while time.monotonic() < deadline:
            try:
                raw_line = self._read_line(
                    max(0.0, deadline - time.monotonic())
                )
            except SerialReadTimeout as exc:
                raise_terminal_timeout(exc)
            saw_frame = True
            if raw_line.startswith(update_prefix):
                return _strict_json_object_loads(
                    _strict_serial_utf8_decode(
                        raw_line[len(update_prefix):]
                    ),
                    label="FOF_UPDATE_MODE host receipt",
                    allowed_schema_ids=allowed_schema_ids,
                )
            if raw_line.startswith(error_prefix):
                error = _strict_json_object_loads(
                    _strict_serial_utf8_decode(
                        raw_line[len(error_prefix):]
                    ),
                    label="FOF_CTL_ERROR host receipt",
                    allowed_schema_ids=(HostJsonSchemaId.CONTROL_ERROR,),
                )
                if error.get("error") == "unknown command":
                    raise UpdateMaintenanceUnsupportedError(
                        "prepare_update is unavailable on the proven source"
                    )
                raise FlashError("update maintenance request was rejected")
            if raw_line == legacy_unknown:
                saw_legacy_unknown = True
                continue
            if raw_line.startswith(detection_prefix):
                _validate_optional_badge_detection_frame(raw_line)
                continue
            if not raw_line.startswith(b"FOF"):
                continue
            saw_reserved_frame = True
            try:
                line = _strict_serial_utf8_decode(raw_line)
            except SerialTransportError:
                continue
            self._log_device_line(line)
        raise_terminal_timeout()

    def prepare_update_maintenance(
        self,
        session: str,
        *,
        deadline: float,
        source_supports_update_maintenance: bool,
    ) -> dict[str, Any]:
        """Request one bounded, idempotent update-maintenance transition."""
        bound_session = _validated_update_session(session)
        if type(deadline) not in (int, float) or isinstance(deadline, bool):
            raise FlashError("update preparation deadline is invalid")
        if type(source_supports_update_maintenance) is not bool:
            raise FlashError(
                "update maintenance source-support proof is invalid"
            )
        started = time.monotonic()
        prepare_deadline = min(float(deadline), started + UPDATE_PREPARE_TIMEOUT_S)
        if prepare_deadline <= started:
            raise FlashError("update preparation deadline expired")
        command = {
            "cmd": "prepare_update",
            "session": bound_session,
        }
        previous_update_session = self._update_session
        allowed = (
            HostJsonSchemaId.UPDATE_MODE_REBOOTING,
            HostJsonSchemaId.UPDATE_MODE_ACTIVE,
            HostJsonSchemaId.UPDATE_MODE_FINISHING,
            HostJsonSchemaId.UPDATE_MODE_ABORTING,
            HostJsonSchemaId.UPDATE_MODE_WAITING,
            HostJsonSchemaId.UPDATE_MODE_BUSY,
        )
        while True:
            remaining = prepare_deadline - time.monotonic()
            if remaining <= 0:
                raise FlashError("update preparation deadline expired")
            try:
                if source_supports_update_maintenance:
                    self._update_session = bound_session
                self.write_line(
                    "FOF_CTL:" + json.dumps(command, separators=(",", ":"))
                )
                receipt = self._read_update_mode_or_control_error(
                    min(remaining, UPDATE_PREPARE_RECEIPT_SLICE_S)
                    if source_supports_update_maintenance
                    else remaining,
                    allowed_schema_ids=allowed,
                )
            except _DeferredLegacyUpdateMaintenanceMarker:
                if not source_supports_update_maintenance:
                    self._update_session = previous_update_session
                    raise
                self._reconnect_same_uplink_mode(
                    deadline=prepare_deadline,
                    maintenance_session=bound_session,
                )
                self._update_session = bound_session
                continue
            except UpdateMaintenanceUnsupportedError:
                if not source_supports_update_maintenance:
                    self._update_session = previous_update_session
                raise
            except SerialReadTimeout:
                if not source_supports_update_maintenance:
                    raise
                self._reconnect_same_uplink_mode(
                    deadline=prepare_deadline,
                    maintenance_session=bound_session,
                )
                self._update_session = bound_session
                continue
            except SerialTransportError as exc:
                if not source_supports_update_maintenance or \
                        not exc.terminal_unavailable:
                    raise
                self._reconnect_same_uplink_mode(
                    deadline=prepare_deadline,
                    maintenance_session=bound_session,
                )
                self._update_session = bound_session
                continue
            validated = _validate_update_mode_receipt(
                receipt, session=bound_session
            )
            phase = validated["phase"]
            if phase in ("rebooting", "active"):
                self._update_session = bound_session
                return validated
            if phase == "busy" and validated["retryable"] is False:
                self._update_session = previous_update_session
                raise FlashError("update preparation session conflict")
            if phase not in ("waiting_for_owner", "busy"):
                raise FlashError(
                    "update preparation returned an invalid lifecycle phase"
                )
            remaining = prepare_deadline - time.monotonic()
            if remaining <= 0:
                raise FlashError("update preparation deadline expired")
            time.sleep(min(UPDATE_PREPARE_RETRY_S, remaining))

    def _request_update_lifecycle_reboot(
        self,
        command: Literal["finish_update", "abort_update"],
        *,
        deadline: float,
    ) -> dict[str, Any]:
        session = _validated_update_session(self._update_session)
        if type(deadline) not in (int, float) or isinstance(deadline, bool):
            raise FlashError("update lifecycle deadline is invalid")
        lifecycle_deadline = float(deadline)
        if command == "finish_update":
            phase = "finishing"
            schema_id = HostJsonSchemaId.UPDATE_MODE_FINISHING
        elif command == "abort_update":
            phase = "aborting"
            schema_id = HostJsonSchemaId.UPDATE_MODE_ABORTING
        else:
            raise FlashError("update lifecycle command is invalid")
        while True:
            remaining = lifecycle_deadline - time.monotonic()
            if remaining <= 0:
                raise FlashError("update lifecycle deadline expired")
            self.write_line(
                "FOF_CTL:" + json.dumps(
                    {"cmd": command, "session": session},
                    separators=(",", ":"),
                )
            )
            receipt = self._read_update_mode_or_control_error(
                remaining,
                allowed_schema_ids=(
                    schema_id,
                    HostJsonSchemaId.UPDATE_MODE_BUSY,
                ),
            )
            validated = _validate_update_mode_receipt(
                receipt, session=session
            )
            if validated["phase"] == phase:
                return validated
            if validated["phase"] != "busy":
                raise FlashError("update lifecycle phase mismatch")
            if validated["retryable"] is False:
                raise FlashError("update lifecycle session conflict")
            remaining = lifecycle_deadline - time.monotonic()
            if remaining <= 0:
                raise FlashError("update lifecycle deadline expired")
            time.sleep(min(UPDATE_PREPARE_RETRY_S, remaining))

    def finish_update_maintenance(
        self,
        *,
        deadline: float,
    ) -> dict[str, Any]:
        return self._request_update_lifecycle_reboot(
            "finish_update", deadline=deadline
        )

    def abort_update_maintenance(
        self,
        *,
        deadline: float,
    ) -> dict[str, Any]:
        return self._request_update_lifecycle_reboot(
            "abort_update", deadline=deadline
        )

    def _reconnect_same_uplink_mode(
        self,
        *,
        deadline: float,
        maintenance_session: str | None,
        allow_recoverable_update: bool = False,
    ) -> dict[str, Any]:
        """Rebind only the same immutable uplink and prove the requested mode."""
        if self.dry_run:
            return {"ok": True, "dry_run": True}
        if type(deadline) not in (int, float) or isinstance(deadline, bool):
            raise FlashError("same-uplink reconnect deadline is invalid")
        bound_session = (
            _validated_update_session(maintenance_session)
            if maintenance_session is not None
            else None
        )
        if self.expected_hardware_id is None:
            raise FlashError(
                "same-uplink reconnect requires a bound hardware_id"
            )

        def remaining() -> float:
            left = float(deadline) - time.monotonic()
            if left <= 0:
                raise SerialReadTimeout(
                    "same-uplink reconnect exceeded its absolute deadline",
                    saw_activity=False,
                    partial_frame=False,
                )
            return left

        def retry_pause() -> None:
            time.sleep(min(UPDATE_PREPARE_RETRY_S, remaining()))

        self._close_serial()
        if self._binary_transport_loss_at is not None:
            parser_safe_at = (
                self._binary_transport_loss_at +
                UPDATE_BINARY_IDLE_TIMEOUT_S +
                UPDATE_BINARY_IDLE_GUARD_S
            )
            while time.monotonic() < parser_safe_at:
                time.sleep(min(
                    UPDATE_PREPARE_RETRY_S,
                    parser_safe_at - time.monotonic(),
                    remaining(),
                ))
            self._binary_transport_loss_at = None
        while True:
            remaining()
            records = _take_badge_usb_descriptor_census()
            matching_serial = tuple(
                record for record in records
                if record.serial_number == self.expected_hardware_id
            )
            moved = tuple(
                record for record in matching_serial
                if record.location != self._trusted_location
            )
            if moved:
                raise FlashError(
                    "rebound uplink moved outside the trusted USB location "
                    "policy"
                )
            matching = tuple(
                record for record in matching_serial
                if record.location == self._trusted_location
            )
            if len(matching) > 1:
                raise FlashError(
                    "same-uplink reconnect found duplicate immutable identity"
                )
            if not matching:
                retry_pause()
                continue

            self._descriptor = matching[0]
            try:
                self._open_serial()
            except SerialTransportError:
                self._close_serial()
                retry_pause()
                continue
            try:
                self._wait_ping_once(
                    min(PASSIVE_RETRY_SLICE_S, remaining())
                )
                status = self.status(timeout_s=min(5.0, remaining()))
            except SerialReadTimeout:
                self._close_serial()
                retry_pause()
                continue
            except SerialTransportError as exc:
                self._close_serial()
                if not exc.terminal_unavailable:
                    raise
                retry_pause()
                continue

            try:
                hardware_id = _validate_uplink_status_common(status)
                if hardware_id != self.expected_hardware_id:
                    raise FlashError(
                        "reconnected uplink hardware_id mismatch"
                    )
                if bound_session is not None:
                    recovery_mode = status.get("recovery_mode")
                    if recovery_mode == "update_maintenance":
                        return _validate_update_maintenance_status(
                            status,
                            session=bound_session,
                            expected_hardware_id=self.expected_hardware_id,
                        )
                    if allow_recoverable_update and \
                            recovery_mode == "update_preparing":
                        return _validate_update_preparing_status(
                            status,
                            session=bound_session,
                            expected_hardware_id=self.expected_hardware_id,
                        )
                    if allow_recoverable_update and \
                            recovery_mode == "normal":
                        normal = validate_uplink_application_status(status)
                        if normal != self.expected_hardware_id:
                            raise FlashError(
                                "normal-mode uplink hardware_id mismatch"
                            )
                        self._update_session = None
                        return dict(status)
                    if recovery_mode != "update_maintenance":
                        self._close_serial()
                        retry_pause()
                        continue
                if status.get("recovery_mode") == "update_maintenance":
                    self._close_serial()
                    retry_pause()
                    continue
                normal = validate_uplink_application_status(status)
                if normal != self.expected_hardware_id:
                    raise FlashError(
                        "normal-mode uplink hardware_id mismatch"
                    )
                self._update_session = None
                return dict(status)
            except Exception:
                if self.ser is not None:
                    self._close_serial()
                raise

    def reconnect_same_uplink(
        self,
        *,
        deadline: float,
    ) -> dict[str, Any]:
        """Rebind only the same immutable uplink and prove maintenance."""
        return self._reconnect_same_uplink_mode(
            deadline=deadline,
            maintenance_session=_validated_update_session(
                self._update_session
            ),
        )

    def reconnect_same_uplink_recoverable_update(
        self,
        *,
        deadline: float,
    ) -> dict[str, Any]:
        """Prove normal, PREPARING, or ACTIVE for this exact update owner."""
        return self._reconnect_same_uplink_mode(
            deadline=deadline,
            maintenance_session=_validated_update_session(
                self._update_session
            ),
            allow_recoverable_update=True,
        )

    def reconnect_same_uplink_normal(
        self,
        *,
        deadline: float,
    ) -> dict[str, Any]:
        """Rebind the same uplink after lifecycle reboot and prove normal."""
        return self._reconnect_same_uplink_mode(
            deadline=deadline,
            maintenance_session=None,
        )

    def reconcile_uplink_ota(
        self,
        expected: Mapping[str, Any],
    ) -> Literal["committed", "restart_from_zero"]:
        """Resolve an interrupted uplink transfer without offset resume."""
        expected_keys = {
            "session", "version", "sha256", "size", "partition"
        }
        if not isinstance(expected, Mapping) or set(expected) != expected_keys:
            raise FlashError("uplink OTA reconciliation expectation malformed")
        session = _validated_update_session(expected.get("session"))
        if session != _validated_update_session(self._update_session):
            raise FlashError("uplink OTA reconciliation session mismatch")
        version = expected.get("version")
        sha256 = expected.get("sha256")
        size = expected.get("size")
        partition = expected.get("partition")
        if type(version) is not str or firmware_version_relation(
            version, version
        ) != "equal":
            raise FlashError("uplink OTA reconciliation version is invalid")
        if type(sha256) is not str or not re.fullmatch(
            r"[0-9a-f]{64}", sha256
        ):
            raise FlashError("uplink OTA reconciliation sha256 is invalid")
        if type(size) is not int or not 1 <= size <= 0xFFFFFFFF:
            raise FlashError("uplink OTA reconciliation size is invalid")
        if partition not in ("ota_0", "ota_1"):
            raise FlashError("uplink OTA reconciliation partition is invalid")

        status = self.status(timeout_s=5)
        maintenance = _validate_update_maintenance_status(
            status,
            session=session,
            expected_hardware_id=self.expected_hardware_id,
        )
        summary = _validate_update_uplink_status(
            maintenance["update_uplink"], session=session
        )
        phase = summary["phase"]
        if phase == "idle":
            return "restart_from_zero"
        manifest_matches = (
            summary["version"] == version and
            summary["sha256"] == sha256 and
            summary["size"] == size and
            summary["partition"] == partition
        )
        if not manifest_matches:
            raise FlashError("uplink OTA reconciliation manifest mismatch")
        if phase == "committed":
            if summary["received"] != size:
                raise FlashError("uplink OTA reconciliation commit ambiguous")
            return "committed"
        if phase != "receiving":
            raise FlashError("uplink OTA reconciliation phase is ambiguous")

        command = {
            "cmd": "uplink_ota_abort",
            "session": session,
        }
        self.write_line(
            "FOF_CTL:" + json.dumps(command, separators=(",", ":"))
        )
        receipt = self.read_prefixed_json(
            "FOF_UPLINK_OTA:",
            30,
            allowed_schema_ids=(HostJsonSchemaId.UPLINK_OTA,),
        )
        validate_uplink_ota_aborted_receipt(
            receipt,
            partition=partition,
            received=summary["received"],
            total=size,
        )
        return "restart_from_zero"

    def reconcile_scanner_stage(
        self,
        expected: Mapping[str, Any],
    ) -> Literal["committed", "restart_from_zero"]:
        """Resolve an interrupted scanner stage without offset resume."""
        expected_keys = {
            "session", "target", "sha256", "size", "slot_mask"
        }
        if not isinstance(expected, Mapping) or set(expected) != expected_keys:
            raise FlashError(
                "scanner stage reconciliation expectation malformed"
            )
        session = _validated_update_session(expected.get("session"))
        if session != _validated_update_session(self._update_session):
            raise FlashError("scanner stage reconciliation session mismatch")
        target = expected.get("target")
        sha256 = expected.get("sha256")
        size = expected.get("size")
        slot_mask = expected.get("slot_mask")
        if type(target) is not str or not target or any(
            not 0x21 <= ord(char) <= 0x7E for char in target
        ):
            raise FlashError(
                "scanner stage reconciliation target is invalid"
            )
        if type(sha256) is not str or not re.fullmatch(
            r"[0-9a-f]{64}", sha256
        ):
            raise FlashError(
                "scanner stage reconciliation sha256 is invalid"
            )
        if type(size) is not int or not 1 <= size <= 0xFFFFFFFF:
            raise FlashError("scanner stage reconciliation size is invalid")
        if type(slot_mask) is not int or slot_mask not in (1, 2, 3):
            raise FlashError(
                "scanner stage reconciliation slot mask is invalid"
            )

        status = self.status(timeout_s=5)
        maintenance = _validate_update_maintenance_status(
            status,
            session=session,
            expected_hardware_id=self.expected_hardware_id,
        )
        summary = _validate_update_scanner_status(
            maintenance["update_scanner"], session=session
        )
        if summary["phase"] == "idle":
            self._reconciled_scanner_generation = None
            return "restart_from_zero"
        manifest_matches = (
            summary["target"] == target and
            summary["sha256"] == sha256 and
            summary["size"] == size and
            summary["slot_mask"] == slot_mask
        )
        if summary["phase"] == "receiving":
            if not manifest_matches:
                raise FlashError(
                    "active scanner parser manifest mismatch"
                )
            self._reconciled_scanner_generation = None
            return "restart_from_zero"
        if summary["phase"] == "committed":
            if manifest_matches:
                self._reconciled_scanner_generation = summary["generation"]
                return "committed"
            self._reconciled_scanner_generation = None
            return "restart_from_zero"
        raise FlashError("scanner stage reconciliation phase is ambiguous")

    def reconnect(self, timeout_s: int = 15) -> None:
        if self.dry_run:
            return
        if self.expected_hardware_id is None:
            raise FlashError(
                "application reconnect requires a bound uplink hardware_id"
            )
        deadline = time.monotonic() + max(timeout_s, 0)

        def remaining_timeout(*, saw_activity: bool) -> float:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise SerialReadTimeout(
                    "application reconnect exceeded its overall deadline",
                    saw_activity=saw_activity,
                    partial_frame=False,
                )
            return remaining

        self._close_serial()
        descriptor, _status = wait_for_application_port(
            self.expected_hardware_id,
            timeout_s=remaining_timeout(saw_activity=False),
        )
        remaining_timeout(saw_activity=True)
        if descriptor.location != self._trusted_location:
            raise FlashError(
                "rebound uplink moved outside the trusted USB location policy"
            )
        self._descriptor = descriptor
        try:
            self._open_serial()
            reopened_status = self.status(
                timeout_s=remaining_timeout(saw_activity=True)
            )
            if time.monotonic() > deadline:
                raise SerialReadTimeout(
                    "application reconnect exceeded its overall deadline",
                    saw_activity=True,
                    partial_frame=False,
                )
            validate_uplink_application_status(reopened_status)
        except Exception:
            self._close_serial()
            raise

    def __exit__(
        self,
        _exc_type: object,
        primary: BaseException | None,
        _traceback: object,
    ) -> bool | None:
        try:
            self._close_serial()
        except BaseException as close_error:
            if primary is None:
                raise
            _add_secondary_failure_note(
                primary,
                close_error,
                scope="serial transport context cleanup",
            )
            return False
        return None

    def write_line(self, line: str, *,
                   log_override: str | None = None) -> None:
        log(f"[usb] > {line if log_override is None else log_override}")
        if self.dry_run:
            return
        payload = ("\n" + line + "\n").encode("utf-8")
        try:
            wrote = self.ser.write(payload)
        except Exception as exc:
            raise SerialTransportError(
                f"serial command write failed on {self.port}: {exc}",
                terminal_unavailable=True,
            ) from exc
        if type(wrote) is not int or wrote != len(payload):
            raise SerialTransportError(
                f"serial command short write on {self.port}: "
                f"got {wrote!r}, wanted {len(payload)}",
                terminal_unavailable=True,
            )
        try:
            self.ser.flush()
        except Exception as exc:
            raise SerialTransportError(
                f"serial command flush failed on {self.port}: {exc}",
                terminal_unavailable=True,
            ) from exc

    def _read_line(self, timeout_s: float) -> bytes:
        if self.dry_run:
            raise SerialReadTimeout(
                "dry-run has no serial input", saw_activity=False,
                partial_frame=False,
            )
        deadline = time.monotonic() + max(timeout_s, 0)
        seen_before = self._rx_bytes_seen
        while True:
            if time.monotonic() > deadline:
                raise SerialReadTimeout(
                    f"timed out waiting for a serial frame on {self.port}",
                    saw_activity=self._rx_bytes_seen > seen_before,
                    partial_frame=bool(self._rx_buffer),
                )
            newline = self._rx_buffer.find(b"\n")
            if len(self._rx_buffer) > SERIAL_RX_BUFFER_MAX or \
                    newline > SERIAL_RX_BUFFER_MAX:
                self._rx_buffer.clear()
                raise SerialTransportError(
                    f"serial receive buffer exceeded {SERIAL_RX_BUFFER_MAX} bytes"
                )
            if newline >= 0:
                raw = bytes(self._rx_buffer[:newline])
                del self._rx_buffer[:newline + 1]
                return raw[:-1] if raw.endswith(b"\r") else raw
            if time.monotonic() >= deadline:
                raise SerialReadTimeout(
                    f"timed out waiting for a serial frame on {self.port}",
                    saw_activity=self._rx_bytes_seen > seen_before,
                    partial_frame=bool(self._rx_buffer),
                )
            try:
                chunk = self.ser.read(1024)
            except Exception as exc:
                raise SerialTransportError(
                    f"serial read failed on {self.port}: {exc}",
                    terminal_unavailable=True,
                ) from exc
            if chunk:
                self._rx_buffer.extend(chunk)
                self._rx_bytes_seen += len(chunk)
                continue
            remaining = deadline - time.monotonic()
            if remaining > 0:
                time.sleep(min(0.03, remaining))

    @staticmethod
    def _log_authorized_progress(progress: dict[str, Any]) -> None:
        log(format_relay_progress(progress))

    @staticmethod
    def _log_device_line(line: str) -> bool:
        diagnostic_markers = (
            "Auto scanner relay[",
            "Relay FAILED @",
            "Relay complete:",
            "Firmware update offered:",
            "firmware ready durably accepted:",
        )
        for marker in diagnostic_markers:
            marker_at = line.find(marker)
            if marker_at >= 0:
                log(f"[device] {line[marker_at:][:320]}")
                break
        return False

    @staticmethod
    def _validate_receipt_schema_selection(
        prefix: str,
        allowed_schema_ids: tuple[HostJsonSchemaId, ...] | None,
    ) -> None:
        permitted = _HOST_FIRMWARE_RECEIPT_SCHEMAS.get(prefix)
        if permitted is None:
            if allowed_schema_ids is not None:
                raise SerialTransportError(
                    "host JSON schema selection does not match receipt prefix"
                )
            return
        if (
            type(allowed_schema_ids) is not tuple or
            not allowed_schema_ids or
            len(set(allowed_schema_ids)) != len(allowed_schema_ids) or
            any(
                type(schema_id) is not HostJsonSchemaId or
                schema_id not in permitted
                for schema_id in allowed_schema_ids
            )
        ):
            raise SerialTransportError(
                "firmware receipt requires an exact host JSON schema selection"
            )

    def read_prefixed_json(
        self,
        prefix: str,
        timeout_s: float,
        progress_prefix: str | None = None,
        *,
        allowed_schema_ids: tuple[HostJsonSchemaId, ...] | None = None,
        progress_allowed_schema_ids:
            tuple[HostJsonSchemaId, ...] | None = None,
        progress_validator:
            Callable[[dict[str, Any]], Any] | None = None,
    ) -> dict[str, Any]:
        self._validate_receipt_schema_selection(
            prefix, allowed_schema_ids
        )
        if progress_prefix is None:
            if progress_allowed_schema_ids is not None or \
                    progress_validator is not None:
                raise SerialTransportError(
                    "progress schema configuration requires a progress prefix"
                )
        else:
            self._validate_receipt_schema_selection(
                progress_prefix, progress_allowed_schema_ids
            )
            if progress_validator is None or not callable(progress_validator):
                raise SerialTransportError(
                    "progress receipt requires a bound-state validator"
                )
        if self.dry_run:
            return {"ok": True, "dry_run": True}
        deadline = time.monotonic() + timeout_s
        saw_frame = False
        prefix_bytes = prefix.encode("ascii", "strict")
        progress_prefix_bytes = (
            progress_prefix.encode("ascii", "strict")
            if progress_prefix is not None else None
        )
        while time.monotonic() < deadline:
            try:
                raw_line = self._read_line(
                    max(0.0, deadline - time.monotonic())
                )
            except SerialReadTimeout as exc:
                raise SerialReadTimeout(
                    f"timed out waiting for {prefix} on {self.port}",
                    saw_activity=saw_frame or exc.saw_activity,
                    partial_frame=exc.partial_frame,
                ) from exc
            saw_frame = True

            if progress_prefix_bytes is not None and \
                    raw_line.startswith(progress_prefix_bytes):
                progress_payload = raw_line[len(progress_prefix_bytes):]
                try:
                    progress_text = _strict_serial_utf8_decode(
                        progress_payload
                    )
                    progress = _strict_json_object_loads(
                        progress_text,
                        label="host relay progress receipt",
                        allowed_schema_ids=progress_allowed_schema_ids,
                    )
                    progress_validator(progress)
                except FlashError:
                    continue
                self._log_authorized_progress(progress)
                deadline = time.monotonic() + timeout_s
                continue

            if raw_line.startswith(prefix_bytes):
                payload_bytes = raw_line[len(prefix_bytes):]
                payload = _strict_serial_utf8_decode(payload_bytes)
                if allowed_schema_ids is not None:
                    return _strict_json_object_loads(
                        payload,
                        label=f"{prefix} host receipt",
                        allowed_schema_ids=allowed_schema_ids,
                    )
                try:
                    parsed = json.loads(payload)
                except json.JSONDecodeError as exc:
                    raise SerialTransportError(
                        f"malformed {prefix} frame on {self.port}: {exc}"
                    ) from exc
                if not isinstance(parsed, dict):
                    raise SerialTransportError(
                        f"non-object {prefix} frame on {self.port}"
                    )
                return parsed

            try:
                line = _strict_serial_utf8_decode(raw_line)
            except SerialTransportError:
                continue
            self._log_device_line(line)
        raise SerialReadTimeout(
            f"timed out waiting for {prefix} on {self.port}",
            saw_activity=saw_frame, partial_frame=bool(self._rx_buffer),
        )

    def read_prefixed_text(self, prefix: str, timeout_s: int) -> str:
        deadline = time.monotonic() + timeout_s
        saw_frame = False
        while time.monotonic() < deadline:
            try:
                raw_line = self._read_line(
                    max(0.0, deadline - time.monotonic())
                )
            except SerialReadTimeout as exc:
                raise SerialReadTimeout(
                    f"timed out waiting for {prefix} on {self.port}",
                    saw_activity=saw_frame or exc.saw_activity,
                    partial_frame=exc.partial_frame,
                ) from exc
            saw_frame = True
            prefix_bytes = prefix.encode("ascii", "strict")
            if raw_line.startswith(prefix_bytes):
                payload = _strict_serial_utf8_decode(
                    raw_line[len(prefix_bytes):]
                )
                if not payload or payload.strip() != payload or any(
                    ch.isspace() for ch in payload
                ):
                    raise SerialTransportError(
                        f"malformed {prefix} text frame on {self.port}"
                    )
                return payload
            try:
                line = _strict_serial_utf8_decode(raw_line)
            except SerialTransportError:
                continue
            self._log_device_line(line)
        raise SerialReadTimeout(
            f"timed out waiting for {prefix} on {self.port}",
            saw_activity=saw_frame, partial_frame=bool(self._rx_buffer),
        )

    def _wait_ping_once(self, timeout_s: float) -> None:
        if self.dry_run:
            log("[usb] wait for FOF_PONG")
            return
        self.write_line("FOF_PING")
        pong = self.read_prefixed_text("FOF_PONG:", timeout_s)
        if firmware_version_relation(pong, pong) != "equal":
            raise SerialTransportError(f"invalid FOF_PONG version: {pong!r}")
        log("[usb] badge uplink responded")

    def wait_ping(self, timeout_s: float = 45) -> None:
        started = time.monotonic()
        _retry_passive_read(
            self._wait_ping_once,
            deadline=started + max(timeout_s, 0),
            label="FOF_PONG",
            logical_clock=[started],
        )

    def status(self, timeout_s: int = 5) -> dict[str, Any]:
        deadline = time.monotonic() + max(timeout_s, 0)
        self.write_line("FOF_STATUS")
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise SerialReadTimeout(
                "timed out waiting for FOF_STATUS",
                saw_activity=False,
                partial_frame=False,
            )
        status = self.read_prefixed_json("FOF_STATUS:", remaining)
        if time.monotonic() > deadline:
            raise SerialReadTimeout(
                "timed out waiting for FOF_STATUS",
                saw_activity=True,
                partial_frame=False,
            )
        hardware_id = status.get("hardware_id")
        if hardware_id not in (None, ""):
            normalized = normalized_hardware_id(hardware_id)
            if self.expected_hardware_id is None:
                self.expected_hardware_id = normalized
            elif normalized != self.expected_hardware_id:
                raise FlashError(
                    f"uplink hardware_id changed: got {normalized}, "
                    f"wanted {self.expected_hardware_id}"
                )
        return status

    def _prove_open_application(self, timeout_s: int) -> dict[str, Any]:
        return _prove_badge_application(self, timeout_s)

    def ctl(self, payload: dict[str, Any], prefix: str = "FOF_CTL_OK:",
            timeout_s: int = 30) -> dict[str, Any]:
        self.write_line("FOF_CTL:" + json.dumps(payload, separators=(",", ":")))
        return self.read_prefixed_json(prefix, timeout_s)

    def recover_scanner_lane(
        self,
        slot: str,
        *,
        platform: dict[str, Any],
        expected_hardware_id: str,
        expected_version: str,
        deadline: float,
    ) -> dict[str, Any]:
        """Reboot exactly one scanner lane and re-prove its bound identity."""
        if slot not in ("ble", "wifi"):
            raise FlashError(f"invalid scanner recovery lane: {slot!r}")
        expected_id = normalized_hardware_id(expected_hardware_id)
        if firmware_version_relation(
            expected_version, expected_version
        ) != "equal":
            raise FlashError(
                f"invalid scanner recovery version: {expected_version!r}"
            )
        if (
            isinstance(deadline, bool) or
            not isinstance(deadline, (int, float)) or
            deadline <= time.monotonic()
        ):
            raise FlashError("scanner recovery deadline is invalid or expired")

        remaining = deadline - time.monotonic()
        receipt = self.ctl(
            {
                "cmd": "scanner_recovery",
                "uart": slot,
                "enabled": False,
            },
            timeout_s=min(5.0, remaining),
        )
        expected_keys = {
            "message",
            "ble_sent",
            "wifi_sent",
            "enabled",
            "reboot_required",
        }
        selected_key = f"{slot}_sent"
        peer_key = "wifi_sent" if slot == "ble" else "ble_sent"
        if (
            type(receipt) is not dict or
            set(receipt) != expected_keys or
            receipt.get("message") != "scanner safe mode command sent" or
            receipt.get(selected_key) is not True or
            receipt.get(peer_key) is not False or
            receipt.get("enabled") is not False or
            receipt.get("reboot_required") is not True
        ):
            raise FlashError(
                "scanner recovery receipt did not prove an exact-lane reboot"
            )

        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise FlashError(
                "scanner recovery deadline expired before identity proof"
            )
        wait_for_scanners_usb(
            self,
            platform,
            [slot],
            expected_version,
            timeout_s=max(1, int(remaining)),
            expected_hardware_ids={slot: expected_id},
            require_auto_update=False,
        )
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise FlashError(
                "scanner recovery deadline expired before final status proof"
            )
        status = self.status(timeout_s=min(5.0, remaining))
        verify_scanners(
            status,
            platform,
            [slot],
            expected_version,
            expected_hardware_ids={slot: expected_id},
        )
        return copy.deepcopy(status)

    def _wait_for_update_maintenance_workers(
        self, session: str,
    ) -> dict[str, Any]:
        """Wait until startup restore has released both scanner UART lanes."""
        ready_deadline = (
            time.monotonic() + UPDATE_MAINTENANCE_READY_TIMEOUT_S
        )
        while True:
            remaining = ready_deadline - time.monotonic()
            if remaining <= 0:
                raise FlashError(
                    "update maintenance workers did not become ready"
                )
            status = self.status(timeout_s=min(5.0, remaining))
            status = _validate_update_maintenance_status(
                status,
                session=session,
                expected_hardware_id=self.expected_hardware_id,
            )
            if _update_maintenance_workers_ready(status):
                return status
            remaining = ready_deadline - time.monotonic()
            if remaining <= 0:
                raise FlashError(
                    "update maintenance workers did not become ready"
                )
            time.sleep(min(UPDATE_PREPARE_RETRY_S, remaining))

    def upload_uplink_firmware(
        self, platform: dict[str, Any],
        artifacts: FrozenArtifactSet,
        version: str,
        recovery_rewrite_same_version: bool = False,
        *,
        expected_pre_status: dict[str, Any] | None = None,
        maintenance_status_validator: Callable[
            [dict[str, Any], str], None
        ] | None = None,
    ) -> dict[str, Any]:
        """Stream one application image through the proven credit-v1 channel."""
        data = _validated_frozen_firmware_bytes(
            artifacts,
            role="uplink",
            target=platform["uplink_name"],
            project=platform["uplink_project"],
            hardware=platform["hardware_type"],
            version=version,
        )
        session = (
            _validated_update_session(self._update_session)
            if self._update_session is not None
            else None
        )
        if expected_pre_status is not None and session is not None:
            raise FlashError(
                "direct bootstrap pre-status cannot enter maintenance upload"
            )
        if session is None:
            status = self._prove_open_application(5)
            hardware_id = validate_uplink_application_status(status)
            if expected_pre_status is not None:
                status = _validate_direct_bootstrap_source_continuity(
                    expected_pre_status,
                    status,
                    target_version=version,
                )
        else:
            status = self._wait_for_update_maintenance_workers(session)
            hardware_id = self.expected_hardware_id
        running_partition = str(status["running_partition"])

        if status["pending_verify"]:
            for _attempt in range(20):
                time.sleep(0.25)
                status = self.status(timeout_s=5)
                if session is None:
                    current_id = validate_uplink_application_status(status)
                else:
                    status = _validate_update_maintenance_status(
                        status,
                        session=session,
                        expected_hardware_id=hardware_id,
                    )
                    current_id = hardware_id
                if current_id != hardware_id:
                    raise FlashError(
                        "uplink hardware_id changed while pending_verify cleared"
                    )
                if status["running_partition"] != running_partition:
                    raise FlashError(
                        "uplink running partition changed while pending_verify cleared"
                    )
                if not status["pending_verify"]:
                    break
            if status["pending_verify"]:
                raise FlashError(
                    "uplink pending_verify did not clear before OTA begin"
                )

        current_version = str(status.get("version") or "")
        relation = firmware_version_relation(version, current_version)
        if relation == "older":
            raise FlashError(
                f"uplink downgrade refused: current {current_version}, "
                f"candidate {version}"
            )
        if relation == "unordered":
            raise FlashError(
                f"unordered uplink firmware variants refused: current "
                f"{current_version}, candidate {version}"
            )
        if relation == "invalid":
            raise FlashError(
                f"cannot prove uplink version safety: current "
                f"{current_version!r}, candidate {version!r}"
            )
        if relation == "equal" and not recovery_rewrite_same_version:
            return {
                "ok": True,
                "skipped": True,
                "phase": "current",
                "hardware_id": hardware_id,
                "version": version,
                "partition": running_partition,
            }

        expected_partition = "ota_1" if running_partition == "ota_0" else "ota_0"
        crc32 = binascii.crc32(data) & 0xFFFFFFFF
        sha256 = hashlib.sha256(data).hexdigest()
        manifest = {
            "cmd": "uplink_ota_begin",
            "target": platform["uplink_name"],
            "project": platform["uplink_project"],
            "hardware_type": platform["hardware_type"],
            "version": version,
            "size": len(data),
            "crc32": crc32,
            "sha256": sha256,
            "flow_control": "credit-v1",
            "recovery_rewrite_same_version": recovery_rewrite_same_version,
        }
        if session is not None:
            manifest["session"] = session
        transfer_deadline = time.monotonic() + UPDATE_TRANSFER_TIMEOUT_S

        def timeout_slice(limit: float) -> float:
            remaining = transfer_deadline - time.monotonic()
            if remaining <= 0:
                raise SerialReadTimeout(
                    "uplink OTA exceeded its absolute transfer deadline",
                    saw_activity=False,
                    partial_frame=False,
                )
            return min(limit, remaining)

        def attempt_from_byte_zero() -> dict[str, Any]:
            self.write_line(
                "FOF_CTL:" + json.dumps(manifest, separators=(",", ":"))
            )
            ready = self.read_prefixed_json(
                "FOF_UPLINK_OTA:",
                timeout_slice(30.0),
                allowed_schema_ids=(HostJsonSchemaId.UPLINK_OTA,),
            )
            first_credit = min(UPLINK_OTA_CREDIT_BYTES, len(data))
            validate_uplink_ota_receipt(
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
            while sent < len(data):
                window_end = sent + credit
                if credit <= 0 or window_end > len(data):
                    raise FlashError("uplink OTA credit window is invalid")
                while sent < window_end:
                    timeout_slice(UPDATE_KEEPALIVE_MAX_S)
                    chunk_end = min(
                        sent + UPLINK_OTA_WRITE_BYTES, window_end
                    )
                    chunk = data[sent:chunk_end]
                    try:
                        wrote = self.ser.write(chunk)
                    except Exception as exc:
                        raise SerialTransportError(
                            f"uplink OTA serial write failed at {sent}: {exc}",
                            terminal_unavailable=True,
                        ) from exc
                    if type(wrote) is not int or wrote != len(chunk):
                        raise SerialTransportError(
                            f"uplink OTA short serial write at {sent}: "
                            f"got {wrote!r}, wanted {len(chunk)}",
                            terminal_unavailable=True,
                        )
                    sent = chunk_end

                try:
                    receipt = self.read_prefixed_json(
                        "FOF_UPLINK_OTA:",
                        timeout_slice(120.0),
                        allowed_schema_ids=(HostJsonSchemaId.UPLINK_OTA,),
                    )
                except (SerialReadTimeout, SerialTransportError) as exc:
                    if session is not None or sent != len(data) or (
                        isinstance(exc, SerialTransportError) and
                        not exc.terminal_unavailable
                    ):
                        raise
                    return {
                        "ok": False,
                        "uncertain": True,
                        "phase": "terminal_unavailable",
                        "expected_partition": expected_partition,
                        "hardware_id": hardware_id,
                        "version": version,
                        "received": sent,
                        "total": len(data),
                        "error": str(exc),
                    }

                if sent == len(data):
                    return validate_uplink_ota_receipt(
                        receipt,
                        phase="committed",
                        partition=expected_partition,
                        received=len(data),
                        total=len(data),
                        credit_bytes=0,
                        reboot_required=True,
                    )

                remaining = len(data) - sent
                next_credit = min(UPLINK_OTA_CREDIT_BYTES, remaining)
                validate_uplink_ota_receipt(
                    receipt,
                    phase="credit",
                    partition=expected_partition,
                    received=sent,
                    total=len(data),
                    credit_bytes=next_credit,
                    reboot_required=False,
                )
                credit = next_credit
            raise FlashError(
                "uplink OTA ended without a committed receipt"
            )

        restarts = 0
        while True:
            try:
                return attempt_from_byte_zero()
            except (SerialReadTimeout, SerialTransportError):
                if session is None:
                    raise
                self._binary_transport_loss_at = time.monotonic()
                if restarts >= UPDATE_MAX_TRANSFER_RESTARTS:
                    raise FlashError(
                        "uplink OTA exhausted byte-zero restart budget"
                    ) from None
                reconnect_status = self.reconnect_same_uplink(
                    deadline=transfer_deadline
                )
                if maintenance_status_validator is not None:
                    maintenance_status_validator(
                        copy.deepcopy(reconnect_status),
                        session,
                    )
                disposition = self.reconcile_uplink_ota({
                    "session": session,
                    "version": version,
                    "sha256": sha256,
                    "size": len(data),
                    "partition": expected_partition,
                })
                if disposition == "committed":
                    return {
                        "ok": True,
                        "phase": "committed",
                        "partition": expected_partition,
                        "received": len(data),
                        "total": len(data),
                        "credit_bytes": 0,
                        "retryable": False,
                        "reboot_required": True,
                        "error": "",
                    }
                restarts += 1

    def stage_scanner_firmware(
        self,
        platform: dict[str, Any],
        artifacts: FrozenArtifactSet,
        version: str,
        slots: list[str],
        *,
        maintenance_status_validator: Callable[
            [dict[str, Any], str], None
        ] | None = None,
    ) -> dict[str, Any]:
        data = _validated_frozen_firmware_bytes(
            artifacts,
            role="scanner",
            target=platform["scanner_name"],
            project=platform["scanner_project"],
            hardware=platform["hardware_type"],
            version=version,
        )
        crc = binascii.crc32(data) & 0xFFFFFFFF
        sha256 = hashlib.sha256(data).hexdigest()
        slot_mask = scanner_slot_mask(slots)
        expected = scanner_stage_receipt_fields(
            platform, version, data, slot_mask
        )
        session = (
            _validated_update_session(self._update_session)
            if self._update_session is not None
            else None
        )
        if session is not None:
            self._wait_for_update_maintenance_workers(session)
        deadline = time.monotonic() + SCANNER_STAGE_TIMEOUT_S

        def remaining_timeout() -> float:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise SerialReadTimeout(
                    "scanner firmware staging exceeded its overall deadline",
                    saw_activity=False,
                    partial_frame=False,
                )
            return remaining

        def read_stage_receipt(
            allowed_schema_ids: tuple[HostJsonSchemaId, ...],
        ) -> dict[str, Any]:
            return self.read_prefixed_json(
                "FOF_FW_UPLOAD:",
                min(remaining_timeout(), UPDATE_KEEPALIVE_MAX_S),
                allowed_schema_ids=allowed_schema_ids,
            )

        def write_stage_bytes(offset: int, chunk: bytes) -> None:
            remaining_timeout()
            try:
                wrote = self.ser.write(chunk)
            except Exception as exc:
                raise SerialTransportError(
                    f"scanner stage write failed at {offset}: {exc}",
                    terminal_unavailable=True,
                ) from exc
            if type(wrote) is not int or wrote != len(chunk):
                raise SerialTransportError(
                    f"scanner stage short write at {offset}: "
                    f"got {wrote!r}, wanted {len(chunk)}",
                    terminal_unavailable=True,
                )

        manifest = {
            "cmd": "fw_upload_begin",
            "name": platform["scanner_name"],
            "target": platform["scanner_name"],
            "project": platform["scanner_project"],
            "hardware_type": platform["hardware_type"],
            "version": version,
            "size": len(data),
            "crc32": crc,
            "sha256": sha256,
            "slot_mask": slot_mask,
            "flow_control": "credit-v1",
        }
        if session is not None:
            manifest["session"] = session

        def attempt_from_byte_zero() -> dict[str, Any]:
            next_progress_percent = 10
            self.write_line(
                "FOF_CTL:" + json.dumps(manifest, separators=(",", ":"))
            )
            begin = read_stage_receipt((
                HostJsonSchemaId.SCANNER_STAGE_READY_CREDIT,
                HostJsonSchemaId.SCANNER_STAGE_FAILURE,
            ))
            if self.dry_run:
                log(
                    "[usb] staged scanner firmware verify skipped for dry-run"
                )
                return {"ok": True, "dry_run": True, **expected}
            first_credit = min(SCANNER_STAGE_CREDIT_BYTES, len(data))
            validate_scanner_stage_credit_receipt(
                begin,
                expected,
                phase="ready",
                received=0,
                credit_bytes=first_credit,
                require_generation=False,
            )
            log(
                f"[usb] staging scanner firmware ({len(data)} bytes "
                f"crc={crc:08x} sha256={sha256})"
            )
            sent = 0
            credit = first_credit
            while sent < len(data):
                window_end = sent + credit
                if credit <= 0 or window_end > len(data):
                    raise FlashError(
                        "USB scanner firmware stage credit window is invalid"
                    )
                while sent < window_end:
                    chunk_end = min(
                        sent + SCANNER_STAGE_WRITE_BYTES, window_end
                    )
                    write_stage_bytes(sent, data[sent:chunk_end])
                    sent = chunk_end

                if sent == len(data):
                    receipt = read_stage_receipt((
                        HostJsonSchemaId.SCANNER_STAGE_FINAL,
                        HostJsonSchemaId.SCANNER_STAGE_FAILURE,
                    ))
                    validate_scanner_stage_credit_receipt(
                        receipt,
                        expected,
                        phase="final",
                        received=len(data),
                        credit_bytes=0,
                        require_generation=True,
                    )
                    log(
                        "[usb] staged scanner firmware verified "
                        f"({receipt.get('name')} {receipt.get('version')})"
                    )
                    return dict(receipt)

                receipt = read_stage_receipt((
                    HostJsonSchemaId.SCANNER_STAGE_READY_CREDIT,
                    HostJsonSchemaId.SCANNER_STAGE_FAILURE,
                ))
                next_credit = min(
                    SCANNER_STAGE_CREDIT_BYTES, len(data) - sent
                )
                validate_scanner_stage_credit_receipt(
                    receipt,
                    expected,
                    phase="credit",
                    received=sent,
                    credit_bytes=next_credit,
                    require_generation=False,
                )
                progress_percent = sent * 100 // len(data)
                if progress_percent >= next_progress_percent:
                    log(
                        "[usb] scanner firmware stage "
                        f"{progress_percent}% ({sent}/{len(data)} bytes)"
                    )
                    while next_progress_percent <= progress_percent:
                        next_progress_percent += 10
                credit = next_credit
            raise FlashError(
                "USB scanner firmware stage ended without a final receipt"
            )

        restarts = 0
        while True:
            try:
                return attempt_from_byte_zero()
            except (SerialReadTimeout, SerialTransportError):
                if session is None:
                    raise
                self._binary_transport_loss_at = time.monotonic()
                if restarts >= UPDATE_MAX_TRANSFER_RESTARTS:
                    raise FlashError(
                        "scanner stage exhausted byte-zero restart budget"
                    ) from None
                reconnect_status = self.reconnect_same_uplink(
                    deadline=deadline
                )
                if maintenance_status_validator is not None:
                    maintenance_status_validator(
                        copy.deepcopy(reconnect_status),
                        session,
                    )
                disposition = self.reconcile_scanner_stage({
                    "session": session,
                    "target": platform["scanner_name"],
                    "sha256": sha256,
                    "size": len(data),
                    "slot_mask": slot_mask,
                })
                if disposition == "committed":
                    generation = self._reconciled_scanner_generation
                    if type(generation) is not int or not (
                        1 <= generation <= 0xFFFFFFFF
                    ):
                        raise FlashError(
                            "scanner commit reconciliation generation missing"
                        )
                    return {
                        "ok": True,
                        **expected,
                        "flow_control": "credit-v1",
                        "phase": "final",
                        "received": len(data),
                        "total": len(data),
                        "credit_bytes": 0,
                        "generation": generation,
                    }
                restarts += 1

    def relay_scanner(
        self,
        slot: str,
        force_probe: bool,
        allow_same_version: bool,
        firmware_size: int,
        *,
        expected_generation: int,
        expected_hardware_id: str,
    ) -> dict[str, Any]:
        if slot not in ("ble", "wifi"):
            raise FlashError("USB scanner relay slot is invalid")
        if (
            type(expected_generation) is not int or
            not 1 <= expected_generation <= 0xFFFFFFFF
        ):
            raise FlashError("USB scanner relay expected generation is invalid")
        if type(firmware_size) is not int or firmware_size <= 0:
            raise FlashError("USB scanner relay firmware size is invalid")
        if type(expected_hardware_id) is not str:
            raise FlashError(
                "USB scanner relay expected hardware_id is invalid"
            )
        bound_hardware_id = expected_hardware_id.strip().lower()
        if not _HARDWARE_ID_RE.fullmatch(bound_hardware_id):
            raise FlashError(
                "USB scanner relay expected hardware_id is invalid"
            )
        payload = {
            "cmd": "fw_relay",
            "uart": slot,
            "allow_same_version": allow_same_version,
            "expected_generation": expected_generation,
            "expected_hardware_id": bound_hardware_id,
        }
        if force_probe:
            payload["force"] = True
            payload["skip_command_probe"] = True
        redacted_payload = dict(payload)
        redacted_payload["expected_hardware_id"] = "[redacted]"
        self.write_line(
            "FOF_CTL:" + json.dumps(payload, separators=(",", ":")),
            log_override=(
                "FOF_CTL:" +
                json.dumps(redacted_payload, separators=(",", ":"))
            ),
        )
        body = self.read_prefixed_json(
            "FOF_FW_RELAY:",
            scanner_relay_timeout_s(firmware_size),
            progress_prefix="FOF_FW_RELAY_PROGRESS:",
            allowed_schema_ids=(
                HostJsonSchemaId.RELAY_TERMINAL,
            ),
            progress_allowed_schema_ids=(
                HostJsonSchemaId.RELAY_PROGRESS,
            ),
            progress_validator=lambda progress: validate_scanner_relay_progress(
                progress,
                slot=slot,
                firmware_size=firmware_size,
            ),
        )
        if self.dry_run:
            return body
        return validate_scanner_relay_receipt(
            body,
            slot=slot,
            expected_generation=expected_generation,
            expected_hardware_id=bound_hardware_id,
            firmware_size=firmware_size,
        )


def wait_for_legacy_uplink_status(
    badge: BadgeSerial, timeout_s: float,
) -> str:
    """Retry only read-only status requests until one legacy proof passes."""
    if not isinstance(timeout_s, (int, float)) or isinstance(timeout_s, bool) \
            or not (0 < float(timeout_s) <= ESPTOOL_TIMEOUT_MAX_S):
        raise FlashError(
            "legacy bootstrap timeout must be in "
            f"(0, {ESPTOOL_TIMEOUT_MAX_S:g}] seconds"
        )
    deadline = time.monotonic() + float(timeout_s)
    last_timeout: SerialReadTimeout | None = None
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            detail = f": {last_timeout}" if last_timeout is not None else ""
            raise FlashError(
                "timed out proving the legacy uplink application" + detail
            )
        try:
            status = badge.status(
                timeout_s=min(PASSIVE_RETRY_SLICE_S, remaining)
            )
        except SerialReadTimeout as exc:
            last_timeout = exc
            continue
        return validate_legacy_uplink_bootstrap_status(status)


def request_legacy_uplink_rom(
    badge: BadgeSerial, timeout_s: float,
) -> str:
    """Prove a legacy app, send one ROM command, and require its exact ACK."""
    source_version = wait_for_legacy_uplink_status(badge, timeout_s)
    badge.write_line(LEGACY_USB_BOOTSTRAP_COMMAND)
    ack = badge.read_prefixed_text(
        LEGACY_USB_BOOTSTRAP_ACK_PREFIX, timeout_s
    )
    if ack != "OK":
        raise FlashError(
            f"legacy bootloader ack mismatch: got {ack!r}, wanted 'OK'"
        )
    return source_version


def _fresh_descriptor_for_trusted_uplink(
    trusted_binding: TrustedUplinkBinding,
) -> UsbDescriptorRecord | None:
    """Resolve only the retained serial/location from one complete census."""
    if type(trusted_binding) is not TrustedUplinkBinding:
        raise FlashError("trusted uplink binding is malformed")
    expected_serial = normalized_hardware_id(
        trusted_binding.serial_number
    )
    if expected_serial != trusted_binding.serial_number or \
            trusted_binding.source not in (
                "retained-session",
                "factory-ledger",
                "operator-selection",
            ) or (
                trusted_binding.location is not None and (
                    type(trusted_binding.location) is not str or
                    not trusted_binding.location or
                    trusted_binding.location !=
                    trusted_binding.location.strip() or
                    any(
                        ord(character) < 0x20 or ord(character) == 0x7F
                        for character in trusted_binding.location
                    )
                )
            ):
        raise FlashError("trusted uplink binding is not canonical")
    records = _take_badge_usb_descriptor_census()
    serial_matches = [
        record for record in records
        if record.serial_number == expected_serial
    ]
    if len(serial_matches) > 1:
        raise FlashError("trusted uplink serial is duplicated")
    if not serial_matches:
        return None
    selected = serial_matches[0]
    if selected.location != trusted_binding.location:
        raise FlashError(
            "trusted uplink re-enumerated outside its bound USB location"
        )
    return selected


def _wait_for_bound_rom_flash(
    trusted_binding: TrustedUplinkBinding,
    artifacts: FrozenArtifactSet,
    version: str,
    *,
    timeout_s: float,
) -> RomFlashStageEvidence:
    """Wait for one trusted descriptor, then mutate on its retained handle."""
    if not isinstance(timeout_s, (int, float)) or isinstance(timeout_s, bool) \
            or not (0 < float(timeout_s) <= ESPTOOL_TIMEOUT_MAX_S):
        raise FlashError(
            "bound ROM discovery timeout must be in "
            f"(0, {ESPTOOL_TIMEOUT_MAX_S:g}] seconds"
        )
    deadline = time.monotonic() + float(timeout_s)
    saw_descriptor = False
    while True:
        descriptor = _fresh_descriptor_for_trusted_uplink(trusted_binding)
        if descriptor is not None:
            saw_descriptor = True
            try:
                return flash_complete_uplink_layout(
                    descriptor,
                    artifacts,
                    version,
                )
            except BoundRomUnavailableError:
                # This exact reset-neutral/no-data result is the sole
                # pre-mutation retry condition.  Integrity, provenance,
                # descriptor, identity, and partial-ROM replies remain hard.
                pass
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            observed = (
                "bound descriptor never entered ROM"
                if saw_descriptor else
                "bound descriptor did not re-enumerate"
            )
            raise FlashError(
                f"timed out waiting for trusted uplink ROM: {observed}"
            )
        time.sleep(min(0.25, remaining))


def _flash_silent_uplink_with_chord_fallback(
    descriptor: UsbDescriptorRecord,
    trusted_binding: TrustedUplinkBinding,
    artifacts: FrozenArtifactSet,
    version: str,
) -> RomFlashStageEvidence:
    """Accept already-ROM, otherwise wait for the physical chord transition."""
    if descriptor.serial_number != trusted_binding.serial_number or \
            descriptor.location != trusted_binding.location:
        raise FlashError(
            "silent uplink descriptor differs from its trusted binding"
        )
    rom_binding = TrustedUplinkBinding(
        serial_number=descriptor.serial_number,
        location=descriptor.location,
        source="retained-session",
    )
    try:
        return flash_complete_uplink_layout(
            descriptor,
            artifacts,
            version,
        )
    except BoundRomUnavailableError:
        log(ROM_ENTRY_PROMPT)
    return _wait_for_bound_rom_flash(
        rom_binding,
        artifacts,
        version,
        timeout_s=ROM_CHORD_DISCOVERY_TIMEOUT_S,
    )


def legacy_usb_bootstrap_to_rom(
    descriptor: UsbDescriptorRecord,
    artifacts: FrozenArtifactSet,
    version: str,
    timeout_s: float,
) -> RomFlashStageEvidence:
    """Reboot only the bound legacy uplink and flash its rebound descriptor."""
    if type(descriptor) is not UsbDescriptorRecord:
        raise FlashError(
            "legacy USB bootstrap requires a bound uplink descriptor"
        )
    _validate_bound_rom_inputs(descriptor, artifacts, version)
    trusted_binding = TrustedUplinkBinding(
        serial_number=descriptor.serial_number,
        location=descriptor.location,
        source="retained-session",
    )
    with BadgeSerial(descriptor, False) as badge:
        request_legacy_uplink_rom(badge, timeout_s=timeout_s)
    time.sleep(LEGACY_ROM_ENUMERATION_SETTLE_S)
    return _wait_for_bound_rom_flash(
        trusted_binding,
        artifacts,
        version,
        timeout_s=timeout_s,
    )


def probe_application(
    descriptor: UsbDescriptorRecord,
    timeout_s: float,
) -> dict[str, Any] | None:
    """Passively prove one running badge application without reset controls."""
    if type(descriptor) is not UsbDescriptorRecord:
        raise FlashError(
            "application probe requires an immutable USB descriptor record"
        )
    try:
        with BadgeSerial(descriptor, False) as badge:
            return _prove_badge_application(badge, timeout_s)
    except SerialReadTimeout as exc:
        if not exc.saw_activity and not exc.partial_frame:
            return None
        raise


def wait_for_application_port(expected_hardware_id: str,
                              timeout_s: float = 15) -> \
        tuple[UsbDescriptorRecord, dict[str, Any]]:
    """Rebind by immutable MAC under one monotonic deadline.

    A zero timeout preserves the existing one-round, zero-wait snapshot mode.
    """
    expected = normalized_hardware_id(expected_hardware_id)
    if not isinstance(timeout_s, (int, float)) or isinstance(timeout_s, bool) \
            or not (0 <= float(timeout_s) <= ESPTOOL_TIMEOUT_MAX_S):
        raise FlashError(
            "application discovery timeout must be in "
            f"[0, {ESPTOOL_TIMEOUT_MAX_S:g}] seconds"
        )
    duration = float(timeout_s)
    deadline = time.monotonic() + duration
    observations: dict[str, str] = {}
    first_round = True

    def timeout_error() -> FlashError:
        detail = ", ".join(
            f"{port}={observation}"
            for port, observation in sorted(observations.items())
        ) or "no responsive uplink applications"
        return FlashError(
            "timed out rebinding the bound uplink application; "
            f"observed {detail}"
        )

    while True:
        if not first_round and time.monotonic() >= deadline:
            raise timeout_error()
        matches: list[tuple[UsbDescriptorRecord, dict[str, Any]]] = []
        descriptors = _take_badge_usb_descriptor_census()
        round_expired = duration > 0 and time.monotonic() > deadline
        for index, descriptor in enumerate(descriptors):
            port = descriptor.device
            if round_expired:
                break
            if duration > 0 and time.monotonic() > deadline:
                round_expired = True
                break
            descriptor_hardware_id = descriptor.serial_number
            if descriptor_hardware_id != expected:
                observations[port] = "descriptor_mismatch"
                log(
                    f"[usb] discovery skipped {port}: "
                    "descriptor identity mismatch"
                )
                continue
            if duration > 0 and time.monotonic() > deadline:
                round_expired = True
                break
            remaining = max(0.0, deadline - time.monotonic())
            ports_left = len(descriptors) - index
            fair_share = remaining / ports_left if ports_left else 0.0
            # A USB reboot can preserve the macOS device pathname while an
            # already-open descriptor remains attached to the disappearing
            # interface. Bound each proof attempt so discovery closes that
            # stale handle and reopens the new application within the single
            # global deadline.
            probe_timeout = min(
                APPLICATION_DISCOVERY_PROBE_SLICE_S, fair_share
            )
            try:
                status = probe_application(descriptor, probe_timeout)
                if duration > 0 and time.monotonic() > deadline:
                    round_expired = True
                    break
                if status is None:
                    observations[port] = "silent"
                    continue
                hardware_id = validate_uplink_application_status(status)
            except FlashError as exc:
                if duration > 0 and time.monotonic() > deadline:
                    round_expired = True
                    break
                observations[port] = str(exc)
                log(f"[usb] discovery skipped {port}: {exc}")
                continue
            if hardware_id == expected:
                matches.append((descriptor, status))
                if len(matches) > 1:
                    rendered = ", ".join(
                        matched.device for matched, _status in matches
                    )
                    raise FlashError(
                        "duplicate bound application identity on: "
                        f"{rendered}"
                    )
            else:
                observations[port] = "application_identity_mismatch"
                log(
                    f"[usb] discovery skipped {port}: application identity "
                    "did not match its bound USB descriptor"
                )
        if matches and not round_expired and (
            duration == 0 or time.monotonic() <= deadline
        ):
            return matches[0]
        remaining = deadline - time.monotonic()
        if duration == 0 or round_expired or remaining <= 0:
            raise timeout_error()
        time.sleep(min(0.25, remaining))
        first_round = False


def verify_uplink_status(status: dict[str, Any], version: str) -> None:
    got = status.get("version")
    if norm_version(str(got or "")) != norm_version(version):
        raise FlashError(f"uplink version mismatch: got {got}, wanted {version}")


def verify_uplink_identity_fields(status: dict[str, Any],
                                  platform: dict[str, Any]) -> None:
    expected = {
        "firmware_name": platform["uplink_name"],
        "app_project": platform["uplink_project"],
        "hardware_type": platform["hardware_type"],
    }
    labels = {
        "firmware_name": "target",
        "app_project": "project",
        "hardware_type": "hardware type",
    }
    for key, wanted in expected.items():
        got = status.get(key)
        if got != wanted:
            raise FlashError(
                f"uplink {labels[key]} mismatch: got {got}, wanted {wanted}"
            )


def uplink_flash_needed(status: dict[str, Any], platform: dict[str, Any],
                        target_version: str,
                        recovery_rewrite_same_version: bool) -> bool:
    verify_uplink_identity_fields(status, platform)
    current = str(status.get("version") or "")
    relation = firmware_version_relation(target_version, current)
    if relation == "older":
        raise FlashError(
            f"uplink downgrade refused: current {current}, "
            f"candidate {target_version}"
        )
    if relation == "unordered":
        raise FlashError(
            f"unordered uplink firmware variants refused: current {current}, "
            f"candidate {target_version}"
        )
    if relation == "invalid":
        raise FlashError(
            f"cannot prove uplink downgrade safety: current {current!r}, "
            f"candidate {target_version!r}"
        )
    if relation == "equal" and not recovery_rewrite_same_version:
        log(
            f"[usb] uplink already has {target_version}; same-version UART "
            "rewrite is disabled"
        )
        return False
    return True


def verify_scanners(status: dict[str, Any], platform: dict[str, Any],
                    slots: list[str], version: str, *,
                    expected_hardware_ids: dict[str, str] | None = None,
                    allowed_newer_slots: set[str] | None = None,
                    require_radio_health: bool = True,
                    allow_uplink_update_maintenance: bool = False) -> None:
    allowed_uplink_modes = {None, "", "normal"}
    if allow_uplink_update_maintenance:
        allowed_uplink_modes.add("update_maintenance")
    if status.get("recovery_mode") not in allowed_uplink_modes:
        raise FlashError(
            f"uplink recovery mode is not normal: {status.get('recovery_mode')}"
        )
    if status.get("safe_mode") is True:
        raise FlashError("uplink safe mode is active")
    for key in ("usb_control_alive", "scanner_uart_alive"):
        if key in status and status.get(key) is not True:
            raise FlashError(f"uplink health check failed: {key}=false")

    by_uart = scanner_status_by_uart(status)
    allowed_newer = set(allowed_newer_slots or ())
    seen_hardware_ids: dict[str, str] = {}
    for slot in slots:
        info = by_uart.get(slot)
        if not info or not info.get("connected"):
            raise FlashError(f"{slot} scanner is not connected")
        verify_scanner_identity_fields(info, platform, slot)
        got = info.get("ver") or info.get("version")
        if slot in allowed_newer:
            relation = firmware_version_relation(version, str(got or ""))
            if relation != "older":
                raise FlashError(
                    f"{slot} scanner newer-skip proof is invalid: got {got}, "
                    f"candidate {version}; scanner must remain newer"
                )
        elif norm_version(str(got or "")) != norm_version(version):
            raise FlashError(f"{slot} scanner version mismatch: got {got}, wanted {version}")
        hardware_id = normalized_hardware_id(info.get("hardware_id"))
        prior_slot = seen_hardware_ids.get(hardware_id)
        if prior_slot is not None:
            raise FlashError(
                f"requested scanner hardware ids are not unique: "
                f"{prior_slot} and {slot} both report {hardware_id}"
            )
        seen_hardware_ids[hardware_id] = slot
        if expected_hardware_ids is not None and slot in expected_hardware_ids:
            wanted_id = normalized_hardware_id(expected_hardware_ids.get(slot))
            if hardware_id != wanted_id:
                raise FlashError(
                    f"{slot} scanner hardware id mismatch: got {hardware_id}, "
                    f"wanted {wanted_id}"
                )
        if info.get("rollback_pending") is not False:
            raise FlashError(
                f"{slot} scanner rollback is not proven clear: "
                f"rollback_pending={info.get('rollback_pending')!r}"
            )
        recovery_mode = info.get("recovery_mode")
        if recovery_mode not in (None, "", "normal"):
            raise FlashError(
                f"{slot} scanner recovery mode is not normal: {recovery_mode}"
            )
        health = info.get("health")
        if require_radio_health and health not in (None, "", "ok"):
            raise FlashError(f"{slot} scanner health is not normal: {health}")
        ota_state = info.get("ota_state")
        if ota_state not in (None, "", "idle"):
            raise FlashError(
                f"{slot} scanner OTA state is not idle: {ota_state}"
            )

        slot_profile = "ble_primary" if slot == "ble" else "wifi_primary"
        slot_role = info.get("slot_role")
        reported_expected = info.get("expected_scan_profile")
        scan_profile = info.get("scan_profile")
        if (slot_role != slot_profile or
                reported_expected != slot_profile or
                scan_profile != slot_profile):
            raise FlashError(
                f"{slot} scanner role mismatch: slot_role={slot_role!r}, "
                f"expected_scan_profile={reported_expected!r}, "
                f"scan_profile={scan_profile!r}, "
                f"wanted all role fields={slot_profile!r}"
            )
        if info.get("role_acked") is not True:
            raise FlashError(
                f"{slot} scanner role convergence missing: "
                f"role_acked={info.get('role_acked')!r}, "
                f"scan_profile={scan_profile!r}"
            )

        ble_ok = (
            info.get("ble_initialized") is True and
            info.get("ble_scanning") is True and
            info.get("ble_host_active") is True and
            info.get("ble_host_synced") is True
        )
        if "full_scan_ok" in info:
            full_scan_ok = info.get("full_scan_ok")
            full_scan_alias_ok = (
                "wifi_full_scan_ok" not in info or
                info.get("wifi_full_scan_ok") == full_scan_ok
            )
        else:
            full_scan_ok = info.get("wifi_full_scan_ok")
            full_scan_alias_ok = "wifi_full_scan_ok" in info
        wifi_init_rc = info.get("wifi_init_rc")
        wifi_ok = (
            info.get("wifi_initialized") is True and
            isinstance(wifi_init_rc, int) and
            not isinstance(wifi_init_rc, bool) and
            wifi_init_rc == 0 and
            info.get("wifi_active") is True and
            isinstance(full_scan_ok, int) and
            not isinstance(full_scan_ok, bool) and
            full_scan_ok > 0 and
            full_scan_alias_ok
        )
        if scan_profile == "ble_primary":
            radio_ok = ble_ok and info.get("wifi_paused") is True
        elif scan_profile == "wifi_primary":
            radio_ok = (
                wifi_ok and
                info.get("ble_scanning") is False and
                info.get("ble_host_active") is False and
                info.get("wifi_paused") is False
            )
        else:
            radio_ok = False
        if require_radio_health and not radio_ok:
            raise FlashError(
                f"{slot} scanner physical radio health is not proven for "
                f"profile {scan_profile!r}: "
                f"ble_initialized={info.get('ble_initialized')!r}, "
                f"ble_scanning={info.get('ble_scanning')!r}, "
                f"ble_host_active={info.get('ble_host_active')!r}, "
                f"ble_host_synced={info.get('ble_host_synced')!r}, "
                f"wifi_paused={info.get('wifi_paused')!r}, "
                f"wifi_initialized={info.get('wifi_initialized')!r}, "
                f"wifi_init_rc={wifi_init_rc!r}, "
                f"wifi_active={info.get('wifi_active')!r}, "
                f"full_scan_ok={full_scan_ok!r}"
            )


def verify_auto_update_convergence(
    status: dict[str, Any],
    slots: list[str],
    *,
    expected_stage_receipt: dict[str, Any] | None = None,
    required_converged_slots: set[str] | None = None,
) -> None:
    required_converged = set(required_converged_slots or ())
    requested_names = set(slots)
    if not required_converged.issubset(requested_names):
        raise FlashError(
            "preflight-proven older slots must be a subset of requested slots"
        )
    store = status.get("firmware_store")
    if not isinstance(store, dict) or store.get("stored") is not True:
        raise FlashError("automatic scanner update has no staged firmware manifest")
    generation = store.get("generation")
    if not isinstance(generation, int) or isinstance(generation, bool) or generation <= 0:
        raise FlashError(f"invalid staged firmware generation: {generation!r}")

    expected_mask = scanner_slot_mask(slots)
    if expected_stage_receipt is not None:
        receipt_aliases = {
            "name": "target",
            "project": "app_project",
            "hardware": "hardware_type",
        }
        if expected_stage_receipt.get("ok") is not True:
            raise FlashError("automatic scanner update stage receipt is not successful")
        for alias, canonical in receipt_aliases.items():
            if expected_stage_receipt.get(alias) != expected_stage_receipt.get(canonical):
                raise FlashError(
                    f"automatic scanner update stage receipt {alias} identity mismatch"
                )
        store_fields = {
            "target": "target",
            "app_project": "app_project",
            "hardware_type": "hardware_type",
            "version": "version",
            "size": "size",
            "crc32": "crc32",
            "sha256": "sha256",
            "generation": "generation",
        }
        for receipt_key, store_key in store_fields.items():
            wanted = expected_stage_receipt.get(receipt_key)
            got = store.get(store_key)
            if type(got) is not type(wanted) or got != wanted:
                raise FlashError(
                    "automatic scanner update stage receipt "
                    f"{receipt_key} mismatch"
                )
        receipt_mask = expected_stage_receipt.get("slot_mask")
        if (
            not isinstance(receipt_mask, int) or isinstance(receipt_mask, bool) or
            receipt_mask != expected_mask
        ):
            raise FlashError(
                "automatic scanner update stage receipt slot_mask mismatch"
            )

    auto_update = store.get("auto_update")
    if not isinstance(auto_update, dict):
        raise FlashError("automatic scanner update status is missing")
    auto_generation = auto_update.get("generation")
    if auto_generation != generation:
        raise FlashError(
            "automatic scanner update generation mismatch: "
            f"coordinator={auto_generation!r}, manifest={generation}"
        )

    target_mask = auto_update.get("target_slot_mask")
    if target_mask != expected_mask:
        raise FlashError(
            "automatic scanner update slot mask mismatch: "
            f"got={target_mask!r}, wanted={expected_mask}"
        )
    pending_mask = auto_update.get("pending_mask")
    if pending_mask != 0:
        raise FlashError(
            f"automatic scanner update still has pending mask {pending_mask!r}"
        )
    if auto_update.get("worker_running") is not False:
        raise FlashError(
            "automatic scanner update worker has not reached a stopped state"
        )

    probes = auto_update.get("readiness_probes")
    if (
        not isinstance(probes, list) or len(probes) != 2 or
        any(
            not isinstance(value, int) or isinstance(value, bool) or
            value < 0 or value > 3
            for value in probes
        )
    ):
        raise FlashError(
            f"automatic scanner update readiness probe proof is invalid: {probes!r}"
        )

    scanner_entries = auto_update.get("scanners")
    if not isinstance(scanner_entries, list):
        raise FlashError("automatic scanner update per-slot status is missing")
    by_slot = {
        entry.get("slot"): entry
        for entry in scanner_entries
        if isinstance(entry, dict) and entry.get("slot") in (0, 1)
    }
    if set(by_slot) != {0, 1}:
        raise FlashError("automatic scanner update must report both scanner slots")

    for scanner_id in (0, 1):
        entry = by_slot[scanner_id]
        slot_name = "ble" if scanner_id == 0 else "wifi"
        state = entry.get("state")
        attempts = entry.get("attempts")
        if (
            not isinstance(attempts, int) or isinstance(attempts, bool) or
            attempts < 0 or attempts > 3
        ):
            raise FlashError(
                f"automatic scanner update slot {scanner_id} attempt proof is invalid: "
                f"{attempts!r}"
            )
        requested = (expected_mask & (1 << scanner_id)) != 0
        if slot_name in required_converged and (
            state != "converged" or attempts < 1
        ):
            raise FlashError(
                f"preflight-proven older {slot_name} scanner did not perform "
                f"automatic convergence: state={state!r}, attempts={attempts!r}"
            )
        if requested and state not in {
            "converged", "current", "newer_skipped"
        }:
            raise FlashError(
                f"automatic scanner update slot {scanner_id} is not in a "
                f"successful terminal state: {state!r}"
            )
        if not requested and state != "excluded":
            raise FlashError(
                f"automatic scanner update slot {scanner_id} was not excluded: "
                f"{state!r}"
            )


def coordinator_newer_skipped_slots(status: dict[str, Any],
                                     slots: list[str]) -> set[str]:
    store = status.get("firmware_store")
    auto_update = store.get("auto_update") if isinstance(store, dict) else None
    entries = auto_update.get("scanners") if isinstance(auto_update, dict) else None
    if not isinstance(entries, list):
        return set()
    requested = set(slots)
    slot_names = {0: "ble", 1: "wifi"}
    return {
        slot_names[entry["slot"]]
        for entry in entries
        if isinstance(entry, dict) and entry.get("slot") in slot_names and
        entry.get("state") == "newer_skipped" and
        slot_names[entry["slot"]] in requested
    }


def current_scanner_slots(status: dict[str, Any], platform: dict[str, Any],
                          slots: list[str], version: str) -> set[str]:
    by_uart = {
        item.get("uart"): item
        for item in status.get("scanners", [])
        if isinstance(item, dict)
    }
    current: set[str] = set()
    for slot in slots:
        info = by_uart.get(slot)
        if not info or not info.get("connected"):
            continue
        board = info.get("board")
        got = info.get("ver") or info.get("version")
        if board == platform["scanner_name"] and versions_match(got, version):
            current.add(slot)
    return current


def scanner_slot_identity_ready(info: dict[str, Any] | None) -> bool:
    if not info or not info.get("connected"):
        return False
    return all(
        info.get(key) not in (None, "")
        for key in (
            "firmware_name",
            "app_project",
            "hardware_type",
            "hardware_id",
        )
    ) and (info.get("ver") or info.get("version")) not in (None, "")


def scanner_status_ready(status: dict[str, Any], slots: list[str]) -> bool:
    by_uart = {
        item.get("uart"): item
        for item in status.get("scanners", [])
        if isinstance(item, dict)
    }
    return all(scanner_slot_identity_ready(by_uart.get(slot)) for slot in slots)


def scanner_slot_is_blank_boot_placeholder(
    info: dict[str, Any] | None,
    platform: dict[str, Any],
    slot: str,
) -> bool:
    """Recognize only the connected, safe, identity-empty boot shape."""
    if not isinstance(info, dict) or info.get("connected") is not True:
        return False
    if any(
        info.get(key) not in (None, "")
        for key in (
            "firmware_name",
            "app_project",
            "hardware_type",
            "hardware_id",
            "ver",
            "version",
        )
    ):
        return False
    if info.get("board") not in (None, "", platform["scanner_name"]):
        return False
    if info.get("rollback_pending") is not False or \
            info.get("recovery_mode") not in (None, "", "normal") or \
            info.get("health") not in (None, "", "ok") or \
            info.get("ota_state") not in (None, "", "idle") or \
            info.get("safe_mode") is True:
        return False
    expected_role = "ble_primary" if slot == "ble" else "wifi_primary"
    for field_name in (
        "slot_role", "expected_scan_profile", "scan_profile",
    ):
        if info.get(field_name) not in (None, "", expected_role):
            return False
    if "role_acked" in info and type(info.get("role_acked")) is not bool:
        return False
    return True


def scanner_slot_has_relay_path(info: dict[str, Any] | None) -> bool:
    if not info:
        return False
    if info.get("connected"):
        # During uplink boot a scanner UART can be marked connected before its
        # immutable target, version, and MAC status fields have arrived. Do not
        # let that transient placeholder end the condition-based preflight.
        return scanner_slot_identity_ready(info)
    if info.get("uart_raw_seen") or int(info.get("uart_raw_bytes") or 0) > 0:
        return True
    for key in ("board", "ver", "version", "cmd_rx", "fw_check_count",
                "ota_state", "recovery_mode"):
        if info.get(key) not in (None, "", False):
            return True
    return False


def scanner_status_has_relay_path(status: dict[str, Any],
                                  slots: list[str]) -> bool:
    by_uart = {
        item.get("uart"): item
        for item in status.get("scanners", [])
        if isinstance(item, dict)
    }
    return all(scanner_slot_has_relay_path(by_uart.get(slot)) for slot in slots)


def scanner_debug_summary(status: dict[str, Any], slots: list[str]) -> str:
    by_uart = {
        item.get("uart"): item
        for item in status.get("scanners", [])
        if isinstance(item, dict)
    }
    parts: list[str] = []
    for slot in slots:
        info = by_uart.get(slot)
        if not info:
            parts.append(f"{slot}:missing")
            continue

        fields: list[str] = ["up" if info.get("connected") else "down"]
        board = info.get("board")
        ver = info.get("ver") or info.get("version")
        if board or ver:
            fields.append(f"{board or '?'}@{ver or '?'}")
        role = info.get("slot_role")
        if role:
            fields.append(f"role={role}")
        if "role_acked" in info:
            fields.append(f"role_ack={1 if info.get('role_acked') else 0}")
        profile = info.get("scan_profile")
        if profile:
            fields.append(f"profile={profile}")
        if info.get("uart_raw_seen") and not info.get("connected"):
            raw = info.get("uart_raw_bytes", 0)
            ovf = info.get("uart_line_overflow", 0)
            json_err = info.get("uart_json_err", 0)
            fields.append(f"raw={raw} ovf={ovf} json_err={json_err}")

        for key, label in (
            ("ota_state", "ota"),
            ("recovery_mode", "recovery"),
            ("safe_reason", "safe"),
            ("cmd_age_ms", "cmd_age_ms"),
            ("last_relay_error", "relay_error"),
            ("radio_restart_count", "radio_restarts"),
            ("crc", "crc"),
        ):
            value = info.get(key)
            if value not in (None, "", False):
                fields.append(f"{label}={value}")
        if info.get("rollback_pending"):
            fields.append("rollback_pending=1")
        if info.get("crash_count"):
            fields.append(f"crashes={info.get('crash_count')}")

        parts.append(f"{slot}:(" + " ".join(fields) + ")")
    return " ".join(parts) if parts else "no scanner slots requested"


def choose_relay_slots(status: dict[str, Any], platform: dict[str, Any],
                       slots: list[str], version: str,
                       recovery_rewrite_same_version: bool,
                       label: str) -> list[str]:
    current = current_scanner_slots(status, platform, slots, version)
    relay_slots: list[str] = []
    for slot in slots:
        if slot not in current:
            log(
                f"[{label}] {slot} scanner is not current; automatic uplink "
                "convergence owns the upgrade"
            )
            continue
        if not recovery_rewrite_same_version:
            log(
                f"[{label}] {slot} scanner already current; same-version "
                "rewrite is disabled"
            )
            continue
        relay_slots.append(slot)
        log(
            f"[{label}] {slot} scanner is current; explicit recovery "
            "same-version rewrite requested"
        )
    return relay_slots


def wait_for_scanner_status_usb(
    serial_link: BadgeSerial,
    slots: list[str],
    timeout_s: float = 45,
    *,
    maintenance_session: str | None = None,
    monotonic_deadline: float | None = None,
) -> dict[str, Any]:
    if maintenance_session is not None:
        _validated_update_session(maintenance_session)
    started = time.monotonic()
    if monotonic_deadline is None:
        deadline = started + max(float(timeout_s), 0.0)
    else:
        if isinstance(monotonic_deadline, bool) or not isinstance(
            monotonic_deadline, (int, float)
        ) or not math.isfinite(float(monotonic_deadline)):
            raise FlashError(
                "scanner status monotonic deadline is invalid"
            )
        deadline = float(monotonic_deadline)
    last: dict[str, Any] = {}
    last_error: Exception | None = None
    next_log = 0.0
    while True:
        remaining_s = deadline - time.monotonic()
        if remaining_s <= 0:
            break
        try:
            last = serial_link.status(
                timeout_s=min(5.0, remaining_s)
            )
            last_error = None
        except Exception as exc:
            last_error = exc
            last = {}
            reconnect_now = time.monotonic()
            remaining_s = deadline - reconnect_now
            if remaining_s <= 0:
                break
            try:
                reconnect_budget = min(15.0, remaining_s)
                if maintenance_session is None:
                    serial_link.reconnect(timeout_s=reconnect_budget)
                else:
                    serial_link.reconnect_same_uplink(
                        deadline=reconnect_now + reconnect_budget
                    )
            except Exception as reconnect_exc:
                last_error = FlashError(
                    f"{exc}; USB reconnect failed: {reconnect_exc}"
                )
        now = time.monotonic()
        if now >= deadline:
            break
        if now >= next_log:
            if last:
                log(f"[usb] scanner status: {scanner_debug_summary(last, slots)}")
            elif last_error:
                log(f"[usb] scanner status unavailable: {last_error}")
            next_log = now + 6
        if scanner_status_ready(last, slots):
            return last
        if last and scanner_status_has_relay_path(
            last, slots
        ) and now - started > 6:
            return last
        remaining_s = deadline - now
        if remaining_s > 0:
            time.sleep(min(2.0, remaining_s))
    if last:
        log(f"[usb] scanner status timeout: {scanner_debug_summary(last, slots)}")
    elif last_error:
        log(f"[usb] scanner status timeout: {last_error}")
    return last


def wait_for_scanner_status_network(base_url: str, slots: list[str],
                                    timeout_s: int = 45) -> dict[str, Any]:
    deadline = time.time() + timeout_s
    started = time.time()
    last: dict[str, Any] = {}
    next_log = 0.0
    while time.time() < deadline:
        last = http_json(f"{base_url}/api/badge/status", timeout=10)
        now = time.time()
        if now >= next_log:
            log(f"[network] scanner status: {scanner_debug_summary(last, slots)}")
            next_log = now + 6
        if scanner_status_ready(last, slots):
            return last
        if last and scanner_status_has_relay_path(last, slots) and time.time() - started > 6:
            return last
        time.sleep(2)
    if last:
        log(f"[network] scanner status timeout: {scanner_debug_summary(last, slots)}")
    return last


def wait_for_scanners_usb(serial_link: BadgeSerial, platform: dict[str, Any],
                          slots: list[str], version: str,
                          timeout_s: int = 120, *,
                          expected_hardware_ids: dict[str, str] | None = None,
                          expected_stage_receipt: dict[str, Any] | None = None,
                          allowed_newer_slots: set[str] | None = None,
                          required_converged_slots: set[str] | None = None,
                          require_auto_update: bool = True,
                          require_radio_health: bool = True,
                          maintenance_session: str | None = None) -> None:
    if maintenance_session is not None:
        _validated_update_session(maintenance_session)
    if require_auto_update and expected_stage_receipt is None:
        raise FlashError(
            "automatic scanner verification requires the exact stage receipt"
        )
    deadline = time.monotonic() + max(float(timeout_s), 0.0)
    last_error: Exception | None = None
    next_log = 0.0
    status: dict[str, Any] = {}
    while True:
        remaining_s = deadline - time.monotonic()
        if remaining_s <= 0:
            break
        try:
            status = serial_link.status(timeout_s=min(5.0, remaining_s))
        except Exception as exc:
            last_error = exc
            now = time.monotonic()
            if now >= next_log:
                log(f"[usb] scanner verify transport unavailable: {exc}")
                next_log = now + 6
            remaining_s = deadline - now
            if remaining_s <= 0:
                break
            try:
                if maintenance_session is None:
                    serial_link.reconnect(
                        timeout_s=min(15.0, remaining_s)
                    )
                else:
                    serial_link.reconnect_same_uplink(
                        deadline=time.monotonic() + min(15.0, remaining_s)
                    )
            except Exception as reconnect_exc:
                last_error = FlashError(
                    f"{exc}; USB reconnect failed: {reconnect_exc}"
                )
            remaining_s = deadline - time.monotonic()
            if remaining_s > 0:
                time.sleep(min(3.0, remaining_s))
            continue
        if time.monotonic() >= deadline:
            last_error = FlashError(
                "scanner status response arrived after the deadline"
            )
            break
        try:
            if require_auto_update:
                # Automatic newer-skip authority comes only from the exact
                # receipt-bound coordinator checked below, never from a stale
                # preflight observation.
                proven_newer = coordinator_newer_skipped_slots(status, slots)
            else:
                proven_newer = set(allowed_newer_slots or ())
            verify_scanners(
                status,
                platform,
                slots,
                version,
                expected_hardware_ids=expected_hardware_ids,
                allowed_newer_slots=proven_newer,
                require_radio_health=require_radio_health,
                allow_uplink_update_maintenance=(
                    maintenance_session is not None
                ),
            )
            if require_auto_update:
                convergence_kwargs: dict[str, Any] = {
                    "expected_stage_receipt": expected_stage_receipt,
                }
                if required_converged_slots is not None:
                    convergence_kwargs["required_converged_slots"] = (
                        required_converged_slots
                    )
                verify_auto_update_convergence(
                    status, slots, **convergence_kwargs
                )
            if time.monotonic() >= deadline:
                last_error = FlashError(
                    "scanner convergence completed after the deadline"
                )
                break
            return
        except Exception as exc:
            last_error = exc
            now = time.monotonic()
            if now >= next_log:
                try:
                    log(f"[usb] scanner verify waiting: {scanner_debug_summary(status, slots)} ({exc})")
                except Exception:
                    log(f"[usb] scanner verify waiting: {exc}")
                next_log = now + 6
            remaining_s = deadline - now
            if remaining_s > 0:
                time.sleep(min(3.0, remaining_s))
    raise FlashError(f"scanner verification failed: {last_error}")


def wait_for_scanners_network(base_url: str, platform: dict[str, Any],
                              slots: list[str], version: str,
                              timeout_s: int = 120) -> None:
    deadline = time.time() + timeout_s
    last_error: Exception | None = None
    next_log = 0.0
    while time.time() < deadline:
        try:
            status = http_json(f"{base_url}/api/badge/status", timeout=10)
            verify_scanners(status, platform, slots, version)
            return
        except Exception as exc:
            last_error = exc
            now = time.time()
            if now >= next_log:
                try:
                    log(f"[network] scanner verify waiting: {scanner_debug_summary(status, slots)} ({exc})")
                except Exception:
                    log(f"[network] scanner verify waiting: {exc}")
                next_log = now + 6
            time.sleep(3)
    raise FlashError(f"scanner verification failed: {last_error}")


def _wait_for_maintenance_uplink_target(
    badge: BadgeSerial,
    *,
    session: str,
    expected_version: str,
    expected_partition: str,
    deadline: float,
) -> dict[str, Any]:
    """Prove the new app and rollback clearance without leaving maintenance."""
    last: dict[str, Any] | None = None
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise FlashError(
                "maintenance uplink target proof exceeded its deadline"
            )
        try:
            status = badge.status(timeout_s=min(5.0, remaining))
        except SerialReadTimeout:
            status = badge.reconnect_same_uplink(deadline=deadline)
        except SerialTransportError as exc:
            truncated_status = str(exc).startswith(
                "malformed FOF_STATUS: frame "
            )
            if not exc.terminal_unavailable and not truncated_status:
                raise
            status = badge.reconnect_same_uplink(deadline=deadline)
        last = _validate_update_maintenance_status(
            status,
            session=session,
            expected_hardware_id=badge.expected_hardware_id,
        )
        if last.get("version") != expected_version:
            raise FlashError("maintenance uplink version mismatch")
        if last.get("running_partition") != expected_partition:
            raise FlashError("maintenance uplink partition mismatch")
        if (
            last.get("pending_verify") is False and
            last.get("rollback_state") == "clear"
        ):
            return last
        time.sleep(min(UPDATE_PREPARE_RETRY_S, remaining))


def _wait_for_maintenance_scanner_campaign(
    badge: BadgeSerial,
    *,
    session: str,
    stage_receipt: Mapping[str, Any],
    slots: list[str],
    required_converged_slots: set[str],
    deadline: float,
) -> dict[str, Any]:
    """Wait for the compact radio-free coordinator proof before finish."""
    expected_mask = scanner_slot_mask(slots)
    if not isinstance(stage_receipt, Mapping):
        raise FlashError("maintenance scanner stage receipt is malformed")
    generation = stage_receipt.get("generation")
    if type(generation) is not int or not 1 <= generation <= 0xFFFFFFFF:
        raise FlashError(
            "maintenance scanner stage generation is invalid"
        )
    expected = {
        "target": stage_receipt.get("target"),
        "sha256": stage_receipt.get("sha256"),
        "size": stage_receipt.get("size"),
        "slot_mask": expected_mask,
        "generation": generation,
    }
    if (
        type(expected["target"]) is not str or
        not expected["target"] or
        type(expected["sha256"]) is not str or
        not re.fullmatch(r"[0-9a-f]{64}", expected["sha256"]) or
        type(expected["size"]) is not int or
        not 1 <= expected["size"] <= 0xFFFFFFFF or
        stage_receipt.get("slot_mask") != expected_mask
    ):
        raise FlashError(
            "maintenance scanner stage identity is invalid"
        )
    required = set(required_converged_slots)
    if not required.issubset(set(slots)):
        raise FlashError(
            "maintenance required convergence slots are invalid"
        )
    zero_attempt_reprompt_deadlines: dict[str, float] = {}

    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise FlashError(
                "maintenance scanner campaign exceeded its deadline"
            )
        try:
            status = badge.status(timeout_s=min(5.0, remaining))
        except (SerialReadTimeout, SerialTransportError):
            badge.reconnect_same_uplink(deadline=deadline)
            continue
        maintenance = _validate_update_maintenance_status(
            status,
            session=session,
            expected_hardware_id=badge.expected_hardware_id,
        )
        stage = _validate_update_scanner_status(
            maintenance["update_scanner"], session=session
        )
        if stage["phase"] != "committed":
            raise FlashError(
                "maintenance scanner stage lost its durable commit"
            )
        for field, wanted in expected.items():
            if type(stage.get(field)) is not type(wanted) or \
                    stage.get(field) != wanted:
                raise FlashError(
                    f"maintenance scanner stage {field} mismatch"
                )
        if stage["received"] != stage["size"]:
            raise FlashError(
                "maintenance scanner stage commit is incomplete"
            )

        campaign = _validate_update_campaign_status(
            maintenance.get("update_campaign"),
            expected_generation=generation,
            expected_slot_mask=expected_mask,
        )
        by_slot = {
            entry["slot"]: entry for entry in campaign["scanners"]
        }
        for scanner_id, slot_name in ((0, "ble"), (1, "wifi")):
            if slot_name not in slots:
                continue
            state = by_slot[scanner_id]["state"]
            if state in ("refused", "newer_skipped"):
                raise FlashError(
                    f"maintenance {slot_name} scanner campaign "
                    f"failed terminally: {state}"
                )

        failed_entries = [
            (scanner_id, slot_name, by_slot[scanner_id])
            for scanner_id, slot_name in ((0, "ble"), (1, "wifi"))
            if slot_name in slots and
            by_slot[scanner_id]["state"] == "failed"
        ]
        if failed_entries and campaign["worker_running"]:
            time.sleep(min(UPDATE_PREPARE_RETRY_S, remaining))
            continue

        waiting_for_safe_reprompt = False
        for scanner_id, slot_name, entry in failed_entries:
            if (
                entry["attempts"] == 0 and
                campaign["readiness_probes"][scanner_id] ==
                    UPDATE_READINESS_MAX_PROBES
            ):
                reprompt_deadline = (
                    zero_attempt_reprompt_deadlines.get(slot_name)
                )
                if reprompt_deadline is None:
                    receipt = badge.ctl(
                        {
                            "cmd": "fw_check_now",
                            "uart": slot_name,
                        },
                        timeout_s=min(5.0, remaining),
                    )
                    sent_key = (
                        "ble_sent" if slot_name == "ble"
                        else "wifi_sent"
                    )
                    if (
                        receipt.get("message") !=
                            "firmware check requested" or
                        receipt.get("uart") != slot_name or
                        receipt.get(sent_key) is not True or
                        receipt.get("deferred") is not False or
                        receipt.get("error") != ""
                    ):
                        raise FlashError(
                            f"maintenance {slot_name} scanner "
                            "zero-attempt reprompt was not accepted"
                        )
                    reprompt_deadline = min(
                        deadline,
                        time.monotonic() +
                            UPDATE_ZERO_ATTEMPT_REPROMPT_GRACE_S,
                    )
                    zero_attempt_reprompt_deadlines[slot_name] = (
                        reprompt_deadline
                    )
                    log(
                        f"[usb] {slot_name} scanner exhausted readiness "
                        "before relay; issued one targeted reprompt"
                    )
                if time.monotonic() < reprompt_deadline:
                    waiting_for_safe_reprompt = True
                    break
        if waiting_for_safe_reprompt:
            time.sleep(min(UPDATE_PREPARE_RETRY_S, remaining))
            continue
        if failed_entries:
            raise ScannerCampaignFailure(
                session=session,
                requested_slots=list(slots),
                stage_receipt=dict(stage_receipt),
                campaign=campaign,
            )

        terminal = (
            campaign["pending_mask"] == 0 and
            campaign["worker_running"] is False and
            all(
                by_slot[0 if slot == "ble" else 1]["state"] in {
                    "converged", "current",
                }
                for slot in slots
            )
        )
        if terminal:
            for slot in required:
                scanner_id = 0 if slot == "ble" else 1
                entry = by_slot[scanner_id]
                if entry["state"] != "converged" or \
                        entry["attempts"] < 1:
                    raise FlashError(
                        f"maintenance preflight-older {slot} scanner did "
                        "not prove convergence"
                    )
            return maintenance
        time.sleep(min(UPDATE_PREPARE_RETRY_S, remaining))


def _run_scanner_update_in_maintenance(
    badge: BadgeSerial,
    *,
    platform: dict[str, Any],
    artifacts: FrozenArtifactSet,
    version: str,
    slots: list[str],
    scanner_image_size: int,
    session: str,
    recovery_rewrite_same_version: bool,
    skip_command_probe: bool,
    preflight_status: dict[str, Any],
    deadline: float,
    maintenance_status_validator: Callable[
        [dict[str, Any], str], None
    ] | None = None,
) -> tuple[
    dict[str, Any],
    dict[str, Any],
    dict[str, Any],
    frozenset[str],
    frozenset[str],
    dict[str, str],
]:
    """Stage and converge both scanner lanes under one maintenance session."""
    if type(preflight_status) is not dict:
        raise FlashError("normal scanner preflight status is malformed")
    preflight_hardware_id = validate_uplink_application_status(
        preflight_status
    )
    if preflight_hardware_id != normalized_hardware_id(
        badge.expected_hardware_id
    ):
        raise FlashError("scanner preflight uplink identity mismatch")
    status = copy.deepcopy(preflight_status)
    expected_hardware_ids = capture_scanner_hardware_ids(
        status, platform, slots, require_connected=True
    )
    newer_slots = scanner_update_newer_slots(
        status, slots, version, require_connected=True
    )
    preflight_older_slots = scanner_strictly_older_slots(
        status, slots, version, require_connected=True
    )
    if newer_slots:
        raise FlashError(
            "maintenance scanner campaign refuses a candidate older than: "
            + ", ".join(sorted(newer_slots))
        )
    recovery_slots = choose_relay_slots(
        status,
        platform,
        slots,
        version,
        recovery_rewrite_same_version,
        "usb",
    )
    pre_stage_status = copy.deepcopy(status)
    stage_receipt = badge.stage_scanner_firmware(
        platform,
        artifacts,
        version,
        slots,
        maintenance_status_validator=maintenance_status_validator,
    )
    stage_snapshot = copy.deepcopy(stage_receipt)
    if recovery_slots:
        log(
            "[usb] scanner image staged; waiting for automatic convergence "
            "before exact same-version recovery relay"
        )
    else:
        log(
            "[usb] scanner image staged; waiting for automatic strict-newer "
            "uplink convergence"
        )
    final_status = _wait_for_maintenance_scanner_campaign(
        badge,
        session=session,
        stage_receipt=stage_snapshot,
        slots=slots,
        required_converged_slots=preflight_older_slots,
        deadline=deadline,
    )
    if maintenance_status_validator is not None:
        maintenance_status_validator(
            copy.deepcopy(final_status),
            session,
        )
    for slot in recovery_slots:
        badge.relay_scanner(
            slot,
            skip_command_probe,
            True,
            scanner_image_size,
            expected_generation=stage_snapshot["generation"],
            expected_hardware_id=expected_hardware_ids[slot],
        )
    return (
        pre_stage_status,
        final_status,
        stage_snapshot,
        frozenset(preflight_older_slots),
        frozenset(recovery_slots),
        expected_hardware_ids,
    )


def _restore_failed_update_maintenance(
    badge: BadgeSerial,
    *,
    session: str,
    persisted_game_state: tuple[Any, ...] | None,
    primary: BaseException,
    reset_budget: _UpdateRetryResetBudget | None,
    _require_descriptor: bool = True,
) -> _UpdateMaintenanceRecoveryResult:
    """Restore one owned failed session and return strict normal evidence."""
    bound_session = _validated_update_session(session)
    recovered: dict[str, Any] | None = None
    action = "already_normal"
    usb_reset_used = False
    first_deadline = time.monotonic() + 15.0
    try:
        recovered = badge.reconnect_same_uplink_normal(
            deadline=first_deadline
        )
    except BaseException as automatic_error:
        try:
            owned_status = badge.reconnect_same_uplink_recoverable_update(
                deadline=time.monotonic() + 5.0
            )
            recovery_mode = owned_status.get("recovery_mode")
            if recovery_mode == "normal":
                validate_uplink_application_status(owned_status)
                recovered = owned_status
            elif recovery_mode in (
                "update_preparing",
                "update_maintenance",
            ):
                if _validated_update_session(
                    owned_status.get("update_session")
                ) != bound_session:
                    raise FlashError(
                        "recoverable update session does not match its owner"
                    )
                abort_uncertainty: BaseException | None = None
                try:
                    badge.abort_update_maintenance(
                        deadline=time.monotonic() + 5.0
                    )
                except BaseException as error:
                    abort_uncertainty = error
                try:
                    recovered = badge.reconnect_same_uplink_normal(
                        deadline=time.monotonic() + 15.0
                    )
                    action = "session_abort"
                except BaseException as proof_error:
                    _add_secondary_failure_note(
                        primary,
                        automatic_error,
                        scope="automatic update-mode failure recovery",
                    )
                    if abort_uncertainty is not None:
                        _add_secondary_failure_note(
                            primary,
                            abort_uncertainty,
                            scope="abort receipt reconciliation",
                        )
                    _add_secondary_failure_note(
                        primary,
                        proof_error,
                        scope="post-abort normal-mode proof",
                    )
                    if reset_budget is None:
                        raise primary
                    if reset_budget.used:
                        raise FlashError(
                            "non-writing uplink USB reset budget already "
                            "consumed"
                        )
                    reset_budget.used = True
                    try:
                        _descriptor, recovered = \
                            _reset_bound_uplink_without_write(
                                badge,
                                deadline=time.monotonic() + 30.0,
                            )
                    except BaseException as reset_error:
                        _add_secondary_failure_note(
                            primary,
                            reset_error,
                            scope="non-writing uplink USB reset fallback",
                        )
                        raise primary
                    action = "usb_reset"
                    usb_reset_used = True
                if abort_uncertainty is not None:
                    _add_secondary_failure_note(
                        primary,
                        abort_uncertainty,
                        scope="abort receipt reconciliation",
                    )
            else:
                raise FlashError(
                    "recoverable update returned an unsafe lifecycle mode"
                )
        except BaseException as abort_error:
            if abort_error is primary:
                raise
            if isinstance(abort_error, FlashError) and \
                    "reset budget already consumed" in str(abort_error):
                raise
            _add_secondary_failure_note(
                primary,
                automatic_error,
                scope="automatic update-mode failure recovery",
            )
            _add_secondary_failure_note(
                primary,
                abort_error,
                scope="bounded update-mode abort fallback",
            )
            raise primary
    if recovered is None:
        raise primary
    try:
        validate_uplink_application_status(recovered)
        if recovered.get("recovery_mode") != "normal" or \
                recovered.get("update_session") not in (None, ""):
            raise FlashError(
                "failed update did not clear its maintenance session"
            )
    except BaseException as recovery_error:
        _add_secondary_failure_note(
            primary, recovery_error, scope="normal-mode failure proof"
        )
        raise primary
    try:
        _verify_persisted_game_state(recovered, persisted_game_state)
    except BaseException as game_error:
        _add_secondary_failure_note(
            primary, game_error, scope="failed update game-state restoration"
        )
        raise primary
    return _issue_update_maintenance_recovery_result(
        badge=badge,
        status=recovered,
        action=action,
        usb_reset_used=usb_reset_used,
        require_descriptor=_require_descriptor,
    )


def _recover_failed_update_maintenance(
    badge: BadgeSerial,
    *,
    session: str,
    persisted_game_state: tuple[Any, ...] | None,
    primary: BaseException,
) -> NoReturn:
    """Restore normal operation, then preserve the original terminal error."""
    _restore_failed_update_maintenance(
        badge,
        session=session,
        persisted_game_state=persisted_game_state,
        primary=primary,
        reset_budget=None,
        _require_descriptor=False,
    )
    raise primary


def _finalize_update_maintenance(
    badge: BadgeSerial,
    *,
    session: str,
    expectation: _PostUplinkExpectation,
    persisted_game_state: tuple[Any, ...] | None,
    deadline: float,
) -> tuple[dict[str, Any], PostUplinkApplicationEvidence]:
    badge.finish_update_maintenance(deadline=deadline)
    status = badge.reconnect_same_uplink_normal(deadline=deadline)
    if status.get("update_session") not in (None, ""):
        raise FlashError("finished update left a stale update session")
    _verify_persisted_game_state(status, persisted_game_state)
    evidence = verify_post_uplink_application(
        status,
        expected_hardware_id=expectation.expected_hardware_id,
        expected_version=expectation.expected_version,
        expected_partition=expectation.expected_partition,
    )
    return status, evidence


def _post_uplink_expectation_for_session(
    expectation: _PostUplinkExpectation,
    session: str,
) -> _PostUplinkExpectation:
    current = _revalidate_post_uplink_expectation(expectation)
    rebound = _PostUplinkExpectation(
        expected_hardware_id=current.expected_hardware_id,
        expected_version=current.expected_version,
        expected_partition=current.expected_partition,
        expected_sha256=current.expected_sha256,
        expected_size=current.expected_size,
        pre_version=current.pre_version,
        pre_partition=current.pre_partition,
        mutation_expected=current.mutation_expected,
        source=current.source,
        update_session=_validated_update_session(session),
    )
    return _revalidate_post_uplink_expectation(rebound)


def _scanner_attempt_snapshot(
    *,
    ordinal: int,
    session: str,
    requested_slots: list[str],
    pre_stage_status: dict[str, Any],
    stage_receipt: dict[str, Any],
    campaign: dict[str, Any],
    outcome: str,
    classification: str | None,
    recovery_action: str | None,
    platform: dict[str, Any],
    artifacts: FrozenArtifactSet,
    version: str,
) -> dict[str, Any]:
    """Validate and copy one immutable scanner campaign audit record."""
    if type(ordinal) is not int or ordinal < 1:
        raise FlashError("scanner campaign ordinal is invalid")
    bound_session = _validated_update_session(session)
    if type(requested_slots) is not list or not requested_slots or \
            len(requested_slots) != len(set(requested_slots)) or \
            any(slot not in ("ble", "wifi") for slot in requested_slots):
        raise FlashError("scanner campaign requested lanes are invalid")
    if type(pre_stage_status) is not dict:
        raise FlashError("scanner campaign pre-stage status is malformed")
    if type(stage_receipt) is not dict:
        raise FlashError("scanner campaign stage receipt is malformed")
    data = _validated_frozen_firmware_bytes(
        artifacts,
        role="scanner",
        target=platform["scanner_name"],
        project=platform["scanner_project"],
        hardware=platform["hardware_type"],
        version=version,
    )
    expected = scanner_stage_receipt_fields(
        platform,
        version,
        data,
        scanner_slot_mask(requested_slots),
    )
    for field_name, wanted in expected.items():
        got = stage_receipt.get(field_name)
        if type(got) is not type(wanted) or got != wanted:
            raise FlashError(
                f"scanner campaign stage {field_name} drifted"
            )
    generation = stage_receipt.get("generation")
    if type(generation) is not int or not 1 <= generation <= 0xFFFFFFFF:
        raise FlashError("scanner campaign stage generation is invalid")
    validated_campaign = _validate_update_campaign_status(
        campaign,
        expected_generation=generation,
        expected_slot_mask=expected["slot_mask"],
    )
    by_slot = {
        entry["slot"]: entry
        for entry in validated_campaign["scanners"]
    }
    requested_ids = {
        0 if slot == "ble" else 1 for slot in requested_slots
    }
    if outcome == "failed":
        if not any(
            by_slot[scanner_id]["state"] == "failed"
            for scanner_id in requested_ids
        ):
            raise FlashError(
                "failed scanner campaign has no failed requested lane"
            )
    elif outcome == "converged":
        if (
            validated_campaign["pending_mask"] != 0 or
            validated_campaign["worker_running"] is not False or
            any(
                by_slot[scanner_id]["state"] not in {
                    "converged", "current",
                }
                for scanner_id in requested_ids
            )
        ):
            raise FlashError(
                "successful scanner campaign lacks terminal convergence"
            )
    else:
        raise FlashError("scanner campaign outcome is invalid")
    allowed_classifications = {
        None,
        "readiness_exhausted",
        "ota_ack_timeout",
        "offer_manifest_mismatch",
        "deferred_backoff",
    }
    if classification not in allowed_classifications:
        raise FlashError("scanner campaign classification is invalid")
    if recovery_action not in {
        None, "already_normal", "session_abort", "usb_reset",
    }:
        raise FlashError("scanner campaign recovery action is invalid")
    snapshot = {
        "ordinal": ordinal,
        "session": bound_session,
        "requested_slots": list(requested_slots),
        "pre_stage_status": copy.deepcopy(pre_stage_status),
        "stage_receipt": copy.deepcopy(stage_receipt),
        "campaign": copy.deepcopy(validated_campaign),
        "outcome": outcome,
        "classification": classification,
        "recovery_action": recovery_action,
        "verified_target": version,
    }
    try:
        return json.loads(json.dumps(
            snapshot,
            allow_nan=False,
            separators=(",", ":"),
            sort_keys=True,
        ))
    except (TypeError, ValueError) as exc:
        raise FlashError(
            "scanner campaign attempt evidence is not JSON-safe"
        ) from exc


def _issue_scanner_retry_sequence(
    *,
    session: str,
    latest_slots: list[str],
    scanner_result: tuple[
        dict[str, Any],
        dict[str, Any],
        dict[str, Any],
        frozenset[str],
        frozenset[str],
        dict[str, str],
    ],
    original_hardware_ids: dict[str, str],
    attempt_history: list[dict[str, Any]],
) -> _ScannerRetrySequence:
    bound_session = _validated_update_session(session)
    if type(latest_slots) is not list or not latest_slots or \
            len(latest_slots) != len(set(latest_slots)) or \
            any(slot not in ("ble", "wifi") for slot in latest_slots):
        raise FlashError("scanner retry latest lanes are invalid")
    if type(scanner_result) is not tuple or len(scanner_result) != 6:
        raise FlashError("scanner retry result is malformed")
    (
        pre_stage_status,
        maintenance_status,
        stage_receipt,
        preflight_older_slots,
        recovery_slots,
        _latest_hardware_ids,
    ) = scanner_result
    if any(
        type(value) is not dict
        for value in (
            pre_stage_status,
            maintenance_status,
            stage_receipt,
            _latest_hardware_ids,
        )
    ):
        raise FlashError("scanner retry result mappings are malformed")
    if type(preflight_older_slots) is not frozenset or \
            type(recovery_slots) is not frozenset or \
            not preflight_older_slots.issubset(set(latest_slots)) or \
            not recovery_slots.issubset(set(latest_slots)):
        raise FlashError("scanner retry result lane sets are malformed")
    if set(_latest_hardware_ids) != set(latest_slots):
        raise FlashError("scanner retry latest identity proof is incomplete")
    normalized_latest = {
        slot: normalized_hardware_id(_latest_hardware_ids[slot])
        for slot in latest_slots
    }
    if type(original_hardware_ids) is not dict or \
            not set(latest_slots).issubset(original_hardware_ids):
        raise FlashError("scanner retry original identity proof is incomplete")
    normalized_original = {
        slot: normalized_hardware_id(value)
        for slot, value in original_hardware_ids.items()
    }
    if any(
        normalized_latest[slot] != normalized_original[slot]
        for slot in latest_slots
    ):
        raise FlashError("scanner retry latest identity proof drifted")
    if type(attempt_history) is not list or not attempt_history:
        raise FlashError("scanner retry attempt history is missing")
    try:
        encoded = tuple(
            json.dumps(
                item,
                allow_nan=False,
                separators=(",", ":"),
                sort_keys=True,
            ).encode("utf-8")
            for item in attempt_history
        )
        return _ScannerRetrySequence(
            session=bound_session,
            latest_slots=tuple(latest_slots),
            preflight_older_slots=preflight_older_slots,
            recovery_slots=recovery_slots,
            expected_hardware_ids=tuple(sorted(
                normalized_original.items()
            )),
            _pre_stage_status_json=json.dumps(
                pre_stage_status,
                allow_nan=False,
                separators=(",", ":"),
                sort_keys=True,
            ).encode("utf-8"),
            _maintenance_status_json=json.dumps(
                maintenance_status,
                allow_nan=False,
                separators=(",", ":"),
                sort_keys=True,
            ).encode("utf-8"),
            _stage_receipt_json=json.dumps(
                stage_receipt,
                allow_nan=False,
                separators=(",", ":"),
                sort_keys=True,
            ).encode("utf-8"),
            _attempt_history_json=encoded,
        )
    except (TypeError, ValueError) as exc:
        raise FlashError("scanner retry result is not JSON-safe") from exc


def _scanner_retry_recovery_placeholder_slots(
    status: dict[str, Any],
    *,
    platform: dict[str, Any],
    original_slots: list[str],
    expected_hardware_ids: dict[str, str],
    target_version: str,
    completed_slots: set[str],
) -> frozenset[str]:
    """Reject hard recovery evidence; return only authorized blank lanes."""
    by_uart = scanner_status_by_uart(status)
    placeholders: set[str] = set()
    for slot in original_slots:
        info = by_uart.get(slot)
        if scanner_slot_identity_ready(info):
            reported_version = str(
                info.get("ver") or info.get("version")
            )
            verify_scanners(
                status,
                platform,
                [slot],
                reported_version,
                expected_hardware_ids={
                    slot: expected_hardware_ids[slot]
                },
            )
            relation = firmware_version_relation(
                target_version, reported_version
            )
            if relation not in ("newer", "equal"):
                raise FlashError(
                    f"{slot} scanner recovery version is unsafe: "
                    f"current={reported_version!r}, "
                    f"target={target_version!r}"
                )
            continue
        if scanner_slot_is_blank_boot_placeholder(
            info, platform, slot
        ):
            placeholders.add(slot)
            continue
        raise FlashError(
            f"{slot} scanner recovery status is neither fully bound "
            "nor a safe blank boot placeholder"
        )

    ready_completed = completed_slots - placeholders
    if ready_completed:
        verify_scanners(
            status,
            platform,
            sorted(ready_completed),
            target_version,
            expected_hardware_ids={
                slot: expected_hardware_ids[slot]
                for slot in ready_completed
            },
        )
    return frozenset(placeholders)


def _retry_failed_scanner_campaigns(
    badge: BadgeSerial,
    *,
    failure: ScannerCampaignFailure,
    platform: dict[str, Any],
    artifacts: FrozenArtifactSet,
    version: str,
    original_slots: list[str],
    original_hardware_ids: dict[str, str],
    first_preflight_status: dict[str, Any],
    scanner_image_size: int,
    persisted_game_state: tuple[Any, ...] | None,
    expected_uplink_partition: str,
    deadline: float,
    recovery_rewrite_same_version: bool,
    skip_command_probe: bool,
    maintenance_status_validator: Callable[
        [dict[str, Any], str], None
    ] | None = None,
) -> _ScannerRetrySequence:
    """Recover and restage only one unresolved scanner, at most twice."""
    if type(failure) is not ScannerCampaignFailure:
        raise FlashError("scanner retry coordinator requires typed evidence")
    if type(original_slots) is not list or not original_slots or \
            len(original_slots) != len(set(original_slots)) or \
            any(slot not in ("ble", "wifi") for slot in original_slots):
        raise FlashError("scanner retry original lanes are invalid")
    original_set = set(original_slots)
    if set(failure.requested_slots) != original_set:
        raise FlashError(
            "first scanner retry failure did not cover original lanes"
        )
    if type(original_hardware_ids) is not dict or \
            set(original_hardware_ids) != original_set:
        raise FlashError(
            "scanner retry original identity set is incomplete"
        )
    expected_ids = {
        slot: normalized_hardware_id(original_hardware_ids[slot])
        for slot in original_slots
    }
    if type(first_preflight_status) is not dict:
        raise FlashError(
            "scanner retry first preflight status is malformed"
        )
    if type(scanner_image_size) is not int or scanner_image_size <= 0:
        raise FlashError("scanner retry image size is invalid")
    if (
        isinstance(deadline, bool) or
        not isinstance(deadline, (int, float)) or
        deadline <= time.monotonic()
    ):
        raise FlashError("scanner retry deadline is invalid or expired")
    if type(recovery_rewrite_same_version) is not bool or \
            type(skip_command_probe) is not bool:
        raise FlashError("scanner retry control flags are malformed")
    if maintenance_status_validator is not None and not callable(
        maintenance_status_validator
    ):
        raise FlashError(
            "scanner retry maintenance validator must be callable"
        )

    reset_budget = _UpdateRetryResetBudget()
    attempts_per_lane = {
        slot: 1 for slot in original_slots
    }
    completed_slots: set[str] = set()
    history: list[dict[str, Any]] = []
    current_failure = failure
    current_preflight = copy.deepcopy(first_preflight_status)

    while True:
        recovery = _restore_failed_update_maintenance(
            badge,
            session=current_failure.session,
            persisted_game_state=persisted_game_state,
            primary=current_failure,
            reset_budget=reset_budget,
        )
        recovery_status = recovery.status
        verify_post_uplink_application(
            recovery_status,
            expected_hardware_id=badge.expected_hardware_id,
            expected_version=version,
            expected_partition=expected_uplink_partition,
        )
        _verify_persisted_game_state(
            recovery_status, persisted_game_state
        )
        proven_completed = completed_slots | set(
            current_failure.successful_slots
        )
        placeholders = _scanner_retry_recovery_placeholder_slots(
            recovery_status,
            platform=platform,
            original_slots=original_slots,
            expected_hardware_ids=expected_ids,
            target_version=version,
            completed_slots=proven_completed,
        )
        if placeholders:
            if deadline - time.monotonic() <= 0:
                raise FlashError(
                    "scanner identities were not ready before retry deadline"
                ) from current_failure
            normal_status = wait_for_scanner_status_usb(
                badge,
                original_slots,
                monotonic_deadline=deadline,
            )
            if time.monotonic() >= deadline or not scanner_status_ready(
                normal_status, original_slots
            ):
                raise FlashError(
                    "scanner identities were not ready before retry deadline"
                ) from current_failure
        else:
            normal_status = recovery_status
        verify_post_uplink_application(
            normal_status,
            expected_hardware_id=badge.expected_hardware_id,
            expected_version=version,
            expected_partition=expected_uplink_partition,
        )
        _verify_persisted_game_state(
            normal_status, persisted_game_state
        )
        captured = capture_scanner_hardware_ids(
            normal_status,
            platform,
            original_slots,
            require_connected=True,
        )
        for slot in original_slots:
            if captured.get(slot) != expected_ids[slot]:
                raise FlashError(
                    f"{slot} scanner hardware id mismatch: "
                    f"got {captured.get(slot)}, wanted {expected_ids[slot]}"
                )
        by_uart = scanner_status_by_uart(normal_status)
        for slot in original_slots:
            info = by_uart.get(slot)
            reported_version = (
                info.get("ver") or info.get("version")
                if isinstance(info, dict)
                else None
            )
            verify_scanners(
                normal_status,
                platform,
                [slot],
                str(reported_version or ""),
                expected_hardware_ids={slot: expected_ids[slot]},
            )
        if proven_completed:
            verify_scanners(
                normal_status,
                platform,
                sorted(proven_completed),
                version,
                expected_hardware_ids={
                    slot: expected_ids[slot]
                    for slot in proven_completed
                },
            )

        failure_slots = set(current_failure.requested_slots)
        decision = _classify_scanner_campaign_retry(
            current_failure,
            status=normal_status,
            platform=platform,
            target_version=version,
            expected_hardware_ids={
                slot: expected_ids[slot] for slot in failure_slots
            },
        )
        completed_slots.update(decision.successful_slots)
        history.append(_scanner_attempt_snapshot(
            ordinal=len(history) + 1,
            session=current_failure.session,
            requested_slots=[
                slot for slot in original_slots
                if slot in failure_slots
            ],
            pre_stage_status=current_preflight,
            stage_receipt=current_failure.stage_receipt,
            campaign=current_failure.campaign,
            outcome="failed",
            classification=decision.reason,
            recovery_action=recovery.action,
            platform=platform,
            artifacts=artifacts,
            version=version,
        ))

        failed_slot = decision.slot
        if attempts_per_lane[failed_slot] >= \
                UPDATE_HOST_CAMPAIGNS_PER_LANE:
            raise ScannerCampaignRetriesExhausted(tuple(history))

        failed_info = scanner_status_by_uart(normal_status).get(failed_slot)
        if not isinstance(failed_info, dict):
            raise FlashError(
                f"{failed_slot} scanner disappeared before exact recovery"
            )
        current_version = str(
            failed_info.get("ver") or failed_info.get("version") or ""
        )
        recovered_status = badge.recover_scanner_lane(
            failed_slot,
            platform=platform,
            expected_hardware_id=decision.scanner_hardware_id,
            expected_version=current_version,
            deadline=deadline,
        )
        verify_post_uplink_application(
            recovered_status,
            expected_hardware_id=badge.expected_hardware_id,
            expected_version=version,
            expected_partition=expected_uplink_partition,
        )
        _verify_persisted_game_state(
            recovered_status, persisted_game_state
        )
        recovered_ids = capture_scanner_hardware_ids(
            recovered_status,
            platform,
            original_slots,
            require_connected=True,
        )
        if recovered_ids != expected_ids:
            raise FlashError(
                "scanner identities changed during exact-lane recovery"
            )
        if completed_slots:
            verify_scanners(
                recovered_status,
                platform,
                sorted(completed_slots),
                version,
                expected_hardware_ids={
                    slot: expected_ids[slot]
                    for slot in completed_slots
                },
            )

        retry_session = _new_update_session()
        try:
            preparation = badge.prepare_update_maintenance(
                retry_session,
                deadline=deadline,
                source_supports_update_maintenance=True,
            )
            if preparation["phase"] == "rebooting":
                maintenance_status = badge.reconnect_same_uplink(
                    deadline=deadline
                )
            else:
                maintenance_status = badge.status(
                    timeout_s=min(5.0, deadline - time.monotonic())
                )
            maintenance_status = _validate_update_maintenance_status(
                maintenance_status,
                session=retry_session,
                expected_hardware_id=badge.expected_hardware_id,
            )
            if maintenance_status_validator is not None:
                maintenance_status_validator(
                    copy.deepcopy(maintenance_status),
                    retry_session,
                )
            _wait_for_maintenance_uplink_target(
                badge,
                session=retry_session,
                expected_version=version,
                expected_partition=expected_uplink_partition,
                deadline=deadline,
            )
            attempts_per_lane[failed_slot] += 1
            scanner_result = _run_scanner_update_in_maintenance(
                badge,
                platform=platform,
                artifacts=artifacts,
                version=version,
                slots=[failed_slot],
                scanner_image_size=scanner_image_size,
                session=retry_session,
                recovery_rewrite_same_version=(
                    recovery_rewrite_same_version
                ),
                skip_command_probe=skip_command_probe,
                preflight_status=recovered_status,
                deadline=deadline,
                maintenance_status_validator=(
                    maintenance_status_validator
                ),
            )
        except ScannerCampaignFailure as next_failure:
            if next_failure.session != retry_session or \
                    next_failure.requested_slots != \
                    frozenset({failed_slot}):
                primary = FlashError(
                    "scanner retry failure escaped its exact lane/session"
                )
                _recover_failed_update_maintenance(
                    badge,
                    session=retry_session,
                    persisted_game_state=persisted_game_state,
                    primary=primary,
                )
            current_failure = next_failure
            current_preflight = copy.deepcopy(recovered_status)
            continue
        except BaseException as primary:
            if getattr(badge, "_update_session", None) == retry_session:
                _recover_failed_update_maintenance(
                    badge,
                    session=retry_session,
                    persisted_game_state=persisted_game_state,
                    primary=primary,
                )
            raise

        (
            retry_pre_stage,
            retry_maintenance_final,
            retry_stage_receipt,
            _retry_preflight_older,
            _retry_recovery_slots,
            retry_hardware_ids,
        ) = scanner_result
        success_campaign = retry_maintenance_final.get(
            "update_campaign"
        )
        success_record = _scanner_attempt_snapshot(
            ordinal=len(history) + 1,
            session=retry_session,
            requested_slots=[failed_slot],
            pre_stage_status=retry_pre_stage,
            stage_receipt=retry_stage_receipt,
            campaign=success_campaign,
            outcome="converged",
            classification=decision.reason,
            recovery_action=recovery.action,
            platform=platform,
            artifacts=artifacts,
            version=version,
        )
        if retry_hardware_ids != {
            failed_slot: expected_ids[failed_slot]
        }:
            primary = FlashError(
                "scanner retry success identity proof drifted"
            )
            _recover_failed_update_maintenance(
                badge,
                session=retry_session,
                persisted_game_state=persisted_game_state,
                primary=primary,
            )
        history.append(success_record)
        completed_slots.add(failed_slot)
        if completed_slots != original_set:
            primary = FlashError(
                "scanner retry did not preserve every original lane"
            )
            _recover_failed_update_maintenance(
                badge,
                session=retry_session,
                persisted_game_state=persisted_game_state,
                primary=primary,
            )
        return _issue_scanner_retry_sequence(
            session=retry_session,
            latest_slots=[failed_slot],
            scanner_result=scanner_result,
            original_hardware_ids=expected_ids,
            attempt_history=history,
        )


def _verify_scanner_attempt_history(
    attempt_history: list[dict[str, Any]],
    stage_receipts: list[dict[str, Any]],
    *,
    platform: dict[str, Any],
    artifacts: FrozenArtifactSet,
    version: str,
    original_slots: list[str],
    original_hardware_ids: dict[str, str],
) -> None:
    """Revalidate bounded history and prior-lane convergence authority."""
    if type(attempt_history) is not list or \
            not 1 <= len(attempt_history) <= \
            UPDATE_HOST_CAMPAIGNS_PER_LANE:
        raise FlashError("scanner attempt history count is invalid")
    if type(stage_receipts) is not list or \
            len(stage_receipts) != len(attempt_history):
        raise FlashError("scanner stage history count is invalid")
    original_set = set(original_slots)
    if type(original_hardware_ids) is not dict or \
            set(original_hardware_ids) != original_set:
        raise FlashError("scanner history identity set is incomplete")
    expected_ids = {
        slot: normalized_hardware_id(original_hardware_ids[slot])
        for slot in original_slots
    }
    seen_sessions: set[str] = set()
    completed: set[str] = set()
    campaigns_per_lane = {slot: 0 for slot in original_slots}

    for index, attempt in enumerate(attempt_history, start=1):
        if type(attempt) is not dict:
            raise FlashError("scanner attempt history entry is malformed")
        requested = attempt.get("requested_slots")
        if type(requested) is not list or not requested or \
                any(slot not in original_set for slot in requested):
            raise FlashError("scanner attempt history lanes are invalid")
        if completed.intersection(requested):
            raise FlashError(
                "scanner attempt history rewrote a proven successful lane"
            )
        validated = _scanner_attempt_snapshot(
            ordinal=index,
            session=attempt.get("session"),
            requested_slots=list(requested),
            pre_stage_status=attempt.get("pre_stage_status"),
            stage_receipt=attempt.get("stage_receipt"),
            campaign=attempt.get("campaign"),
            outcome=attempt.get("outcome"),
            classification=attempt.get("classification"),
            recovery_action=attempt.get("recovery_action"),
            platform=platform,
            artifacts=artifacts,
            version=version,
        )
        if validated != attempt:
            raise FlashError(
                "scanner attempt history is not canonical"
            )
        if stage_receipts[index - 1] != \
                attempt["stage_receipt"]:
            raise FlashError(
                "scanner stage receipt order does not match attempts"
            )
        bound_session = attempt["session"]
        if bound_session in seen_sessions:
            raise FlashError(
                "scanner attempt history reused a maintenance session"
            )
        seen_sessions.add(bound_session)
        for slot in requested:
            campaigns_per_lane[slot] += 1
            if campaigns_per_lane[slot] > \
                    UPDATE_HOST_CAMPAIGNS_PER_LANE:
                raise FlashError(
                    f"{slot} scanner exceeded its host campaign budget"
                )
        preflight_ids = capture_scanner_hardware_ids(
            attempt["pre_stage_status"],
            platform,
            requested,
            require_connected=True,
        )
        for slot in requested:
            if preflight_ids.get(slot) != expected_ids[slot]:
                raise FlashError(
                    f"{slot} scanner attempt identity changed"
                )
        by_slot = {
            entry["slot"]: entry
            for entry in attempt["campaign"]["scanners"]
        }
        successful_this_attempt: set[str] = set()
        for slot in requested:
            scanner_id = 0 if slot == "ble" else 1
            state = by_slot[scanner_id]["state"]
            if state in {"converged", "current"}:
                successful_this_attempt.add(slot)
            elif state != "failed":
                raise FlashError(
                    f"{slot} scanner attempt ended ambiguously: {state}"
                )
        if attempt["outcome"] == "failed":
            if attempt["classification"] is None or \
                    attempt["recovery_action"] is None:
                raise FlashError(
                    "failed scanner attempt lacks recovery authority"
                )
        elif index != len(attempt_history):
            raise FlashError(
                "scanner attempt history continued after convergence"
            )
        completed.update(successful_this_attempt)

    if attempt_history[-1]["outcome"] != "converged":
        raise FlashError(
            "scanner attempt history lacks terminal convergence"
        )
    if completed != original_set:
        raise FlashError(
            "scanner attempt history does not prove every original lane"
        )


def _usb_update_maintenance_flow(
    args: argparse.Namespace,
    platform: dict[str, Any],
    need_uplink: bool,
    slots: list[str],
    version: str,
    *,
    initial_descriptor: UsbDescriptorRecord,
    trusted_uplink_binding: TrustedUplinkBinding,
    application_status: dict[str, Any] | None,
    frozen_artifacts: FrozenUsbFirmwareArtifacts,
    scanner_image_size: int,
    legacy_bootstrap: bool,
    _issue_scanner_flow_result: Callable[..., UsbScannerFlowResult],
    maintenance_status_validator: Callable[
        [dict[str, Any], str], None
    ] | None = None,
    post_direct_bootstrap_status_validator: Callable[
        [dict[str, Any]], None
    ] | None = None,
    post_rom_bootstrap_status_validator: Callable[
        [dict[str, Any]], None
    ] | None = None,
) -> UsbScannerFlowResult | None:
    """Run the .79+ single-session update flow."""
    uplink_artifacts = frozen_artifacts.uplink
    scanner_artifacts = frozen_artifacts.scanner
    deadline = time.monotonic() + UPDATE_TRANSFER_TIMEOUT_S
    session = _new_update_session()
    uplink_data = (
        _frozen_firmware_bytes(uplink_artifacts, role="uplink")
        if uplink_artifacts is not None
        else None
    )
    persisted_game_state = (
        _capture_persisted_game_state(application_status)
        if application_status is not None
        else None
    )
    if maintenance_status_validator is not None and not callable(
        maintenance_status_validator
    ):
        raise FlashError(
            "maintenance status validator must be callable"
        )
    if post_direct_bootstrap_status_validator is not None and not callable(
        post_direct_bootstrap_status_validator
    ):
        raise FlashError(
            "post-direct-bootstrap status validator must be callable"
        )
    if post_rom_bootstrap_status_validator is not None and not callable(
        post_rom_bootstrap_status_validator
    ):
        raise FlashError(
            "post-ROM-bootstrap status validator must be callable"
        )

    expectation: _PostUplinkExpectation
    rebound_descriptor = initial_descriptor
    post_evidence: PostUplinkApplicationEvidence | None = None

    if legacy_bootstrap or application_status is None:
        if not need_uplink or uplink_artifacts is None:
            raise FlashError(
                "maintenance ROM/bootstrap path requires uplink artifacts"
            )
        if legacy_bootstrap:
            stage = legacy_usb_bootstrap_to_rom(
                initial_descriptor, uplink_artifacts, version, timeout_s=30
            )
        else:
            stage = _flash_silent_uplink_with_chord_fallback(
                initial_descriptor,
                trusted_uplink_binding,
                uplink_artifacts,
                version,
            )
        expectation = _expectation_from_rom_flash(
            stage,
            layout_version=version,
            artifacts=uplink_artifacts,
            update_session=session,
        )
        rebound_descriptor, post_evidence = wait_for_post_uplink_application(
            expectation, timeout_s=POST_UPLINK_APPLICATION_TIMEOUT_S
        )
        if rebound_descriptor.serial_number != \
                trusted_uplink_binding.serial_number or \
                rebound_descriptor.location != trusted_uplink_binding.location:
            raise FlashError(
                "post-bootstrap uplink left its trusted USB binding"
            )
        if not slots:
            with BadgeSerial(
                rebound_descriptor,
                False,
                expected_hardware_id=expectation.expected_hardware_id,
            ) as badge:
                final_status = badge.status()
                evidence = _fresh_post_uplink_application_evidence(
                    final_status,
                    expectation=expectation,
                    previous=_require_post_uplink_evidence(
                        post_evidence, expectation
                    ),
                    label="maintenance-bootstrap final application status",
                )
                _prove_reversible_usb_theme_control(
                    badge,
                    initial_status=final_status,
                    expectation=expectation,
                    initial_evidence=evidence,
                )
            return None
        application_status = probe_application(rebound_descriptor, 5)
        if application_status is None:
            raise FlashError(
                "post-bootstrap application disappeared before maintenance"
            )
        if post_rom_bootstrap_status_validator is not None:
            post_rom_bootstrap_status_validator(
                copy.deepcopy(application_status)
            )
        persisted_game_state = _capture_persisted_game_state(
            application_status
        )
    else:
        hardware_id = validate_uplink_application_status(application_status)
        if not need_uplink and application_status.get("version") != version:
            raise FlashError(
                "scanner-only maintenance requires the target uplink version"
            )
        expectation = _PostUplinkExpectation(
            expected_hardware_id=hardware_id,
            expected_version=version,
            expected_partition=str(application_status["running_partition"]),
            expected_sha256="",
            expected_size=0,
            pre_version=str(application_status["version"]),
            pre_partition=str(application_status["running_partition"]),
            mutation_expected=False,
            source="current",
            update_session=session,
        )

    hardware_id = validate_uplink_application_status(application_status)
    with BadgeSerial(
        rebound_descriptor,
        False,
        expected_hardware_id=hardware_id,
    ) as badge:
        scanner_preflight_status: dict[str, Any] | None = None
        original_scanner_hardware_ids: dict[str, str] = {}
        flow_preflight_older_slots = frozenset[str]()
        flow_recovery_slots = frozenset[str]()
        if slots:
            scanner_preflight_status = wait_for_scanner_status_usb(
                badge, slots
            )
            preflight_hardware_id = validate_uplink_application_status(
                scanner_preflight_status
            )
            if preflight_hardware_id != hardware_id:
                raise FlashError(
                    "scanner preflight changed the uplink hardware identity"
                )
            original_scanner_hardware_ids = capture_scanner_hardware_ids(
                scanner_preflight_status,
                platform,
                slots,
                require_connected=True,
            )
            flow_preflight_older_slots = frozenset(
                scanner_strictly_older_slots(
                    scanner_preflight_status,
                    slots,
                    version,
                    require_connected=True,
                )
            )
            if getattr(
                args, "recovery_rewrite_same_version", False
            ):
                flow_recovery_slots = frozenset(
                    current_scanner_slots(
                        scanner_preflight_status,
                        platform,
                        slots,
                        version,
                    )
                )

        def prepare_bound_update(
            bound_session: str,
            source_status: dict[str, Any],
        ) -> dict[str, Any]:
            try:
                return badge.prepare_update_maintenance(
                    bound_session,
                    deadline=deadline,
                    source_supports_update_maintenance=(
                        _uses_update_maintenance(
                            str(source_status.get("version"))
                        )
                    ),
                )
            except BaseException as primary:
                if getattr(badge, "_update_session", None) == bound_session:
                    _recover_failed_update_maintenance(
                        badge,
                        session=bound_session,
                        persisted_game_state=persisted_game_state,
                        primary=primary,
                    )
                raise

        try:
            preparation = prepare_bound_update(
                session,
                application_status,
            )
        except UpdateMaintenanceUnsupportedError:
            if _uses_update_maintenance(
                str(application_status.get("version"))
            ):
                raise
            if not need_uplink or uplink_artifacts is None:
                raise
            application_status = \
                _prove_direct_bootstrap_source_after_rejection(
                    badge,
                    initial_status=application_status,
                    target_version=version,
                    deadline=deadline,
                )
            refreshed_hardware_id = validate_uplink_application_status(
                application_status
            )
            if refreshed_hardware_id != hardware_id:
                raise FlashError(
                    "direct bootstrap reproof changed uplink identity"
                )
            persisted_game_state = _capture_persisted_game_state(
                application_status
            )
            receipt = badge.upload_uplink_firmware(
                platform,
                uplink_artifacts,
                version,
                getattr(args, "recovery_rewrite_same_version", False),
                expected_pre_status=application_status,
            )
            post_bootstrap_session = (
                _new_update_session() if slots else session
            )
            if uplink_data is None:
                raise FlashError(
                    "direct bootstrap frozen artifact is unavailable"
                )
            expectation = _classify_uplink_update_receipt(
                receipt,
                pre_status=application_status,
                target_version=version,
                expected_sha256=hashlib.sha256(uplink_data).hexdigest(),
                expected_size=len(uplink_data),
                update_session=post_bootstrap_session,
            )
            badge._close_serial()
            rebound_descriptor, post_evidence = \
                wait_for_post_uplink_application(
                    expectation,
                    timeout_s=POST_UPLINK_APPLICATION_TIMEOUT_S,
                )
            if rebound_descriptor.serial_number != hardware_id or \
                    rebound_descriptor.location != \
                    trusted_uplink_binding.location:
                raise FlashError(
                    "direct bootstrap rebound outside trusted USB binding"
                )
            badge._descriptor = rebound_descriptor
            badge._open_serial()
            application_status = badge._prove_open_application(5)
            if post_direct_bootstrap_status_validator is not None:
                post_direct_bootstrap_status_validator(
                    copy.deepcopy(application_status)
                )
            persisted_game_state = _capture_persisted_game_state(
                application_status
            )
            if not slots:
                evidence = _fresh_post_uplink_application_evidence(
                    application_status,
                    expectation=expectation,
                    previous=_require_post_uplink_evidence(
                        post_evidence, expectation
                    ),
                    label="direct-bootstrap final application status",
                )
                _prove_reversible_usb_theme_control(
                    badge,
                    initial_status=application_status,
                    expectation=expectation,
                    initial_evidence=evidence,
                )
                return None
            scanner_preflight_status = wait_for_scanner_status_usb(
                badge, slots
            )
            refreshed_hardware_id = validate_uplink_application_status(
                scanner_preflight_status
            )
            if refreshed_hardware_id != hardware_id:
                raise FlashError(
                    "direct bootstrap scanner preflight changed uplink "
                    "hardware identity"
                )
            original_scanner_hardware_ids = capture_scanner_hardware_ids(
                scanner_preflight_status,
                platform,
                slots,
                require_connected=True,
            )
            flow_preflight_older_slots = frozenset(
                scanner_strictly_older_slots(
                    scanner_preflight_status,
                    slots,
                    version,
                    require_connected=True,
                )
            )
            if getattr(
                args, "recovery_rewrite_same_version", False
            ):
                flow_recovery_slots = frozenset(
                    current_scanner_slots(
                        scanner_preflight_status,
                        platform,
                        slots,
                        version,
                    )
                )
            session = post_bootstrap_session
            preparation = prepare_bound_update(
                session,
                application_status,
            )

        scanner_result: tuple[
            dict[str, Any],
            dict[str, Any],
            dict[str, Any],
            frozenset[str],
            frozenset[str],
            dict[str, str],
        ] | None = None
        latest_scanner_slots = list(slots)
        scanner_attempt_history: list[dict[str, Any]] = []
        scanner_stage_receipts: list[dict[str, Any]] = []
        try:
            if preparation["phase"] == "rebooting":
                maintenance_status = badge.reconnect_same_uplink(
                    deadline=deadline
                )
            else:
                maintenance_status = badge.status(timeout_s=5)
            maintenance_status = _validate_update_maintenance_status(
                maintenance_status,
                session=session,
                expected_hardware_id=hardware_id,
            )
            if maintenance_status_validator is not None:
                maintenance_status_validator(
                    copy.deepcopy(maintenance_status),
                    session,
                )

            if need_uplink and expectation.source != "committed":
                if uplink_artifacts is None:
                    raise FlashError(
                        "maintenance flow lacks uplink artifacts"
                    )
                receipt = badge.upload_uplink_firmware(
                    platform,
                    uplink_artifacts,
                    version,
                    getattr(
                        args, "recovery_rewrite_same_version", False
                    ),
                    maintenance_status_validator=(
                        maintenance_status_validator
                    ),
                )
                if uplink_data is None:
                    raise FlashError(
                        "maintenance frozen uplink artifact is unavailable"
                    )
                expectation = _classify_uplink_update_receipt(
                    receipt,
                    pre_status=application_status,
                    target_version=version,
                    expected_sha256=hashlib.sha256(
                        uplink_data
                    ).hexdigest(),
                    expected_size=len(uplink_data),
                    update_session=session,
                )
                if receipt.get("phase") == "committed":
                    maintenance_status = badge.reconnect_same_uplink(
                        deadline=deadline
                    )
                    maintenance_status = \
                        _validate_update_maintenance_status(
                            maintenance_status,
                            session=session,
                            expected_hardware_id=hardware_id,
                        )
                    if maintenance_status_validator is not None:
                        maintenance_status_validator(
                            copy.deepcopy(maintenance_status),
                            session,
                        )
            elif not need_uplink:
                expectation = _PostUplinkExpectation(
                    expected_hardware_id=hardware_id,
                    expected_version=version,
                    expected_partition=str(
                        maintenance_status["running_partition"]
                    ),
                    expected_sha256="",
                    expected_size=0,
                    pre_version=str(application_status["version"]),
                    pre_partition=str(
                        application_status["running_partition"]
                    ),
                    mutation_expected=False,
                    source="current",
                    update_session=session,
                )
            _revalidate_post_uplink_expectation(expectation)
            maintenance_status = _wait_for_maintenance_uplink_target(
                badge,
                session=session,
                expected_version=expectation.expected_version,
                expected_partition=expectation.expected_partition,
                deadline=deadline,
            )
            if maintenance_status_validator is not None:
                maintenance_status_validator(
                    copy.deepcopy(maintenance_status),
                    session,
                )

            if slots:
                if scanner_artifacts is None:
                    raise FlashError(
                        "maintenance flow lacks scanner artifacts"
                    )
                if scanner_preflight_status is None:
                    raise FlashError(
                        "maintenance flow lacks normal scanner preflight"
                    )
                try:
                    scanner_result = _run_scanner_update_in_maintenance(
                        badge,
                        platform=platform,
                        artifacts=scanner_artifacts,
                        version=version,
                        slots=slots,
                        scanner_image_size=scanner_image_size,
                        session=session,
                        recovery_rewrite_same_version=getattr(
                            args, "recovery_rewrite_same_version", False
                        ),
                        skip_command_probe=getattr(
                            args, "skip_command_probe", False
                        ),
                        preflight_status=scanner_preflight_status,
                        deadline=deadline,
                        maintenance_status_validator=(
                            maintenance_status_validator
                        ),
                    )
                except ScannerCampaignFailure as campaign_failure:
                    retry_sequence = _retry_failed_scanner_campaigns(
                        badge,
                        failure=campaign_failure,
                        platform=platform,
                        artifacts=scanner_artifacts,
                        version=version,
                        original_slots=slots,
                        original_hardware_ids=(
                            original_scanner_hardware_ids
                        ),
                        first_preflight_status=(
                            scanner_preflight_status
                        ),
                        scanner_image_size=scanner_image_size,
                        persisted_game_state=persisted_game_state,
                        expected_uplink_partition=(
                            expectation.expected_partition
                        ),
                        deadline=deadline,
                        recovery_rewrite_same_version=getattr(
                            args, "recovery_rewrite_same_version", False
                        ),
                        skip_command_probe=getattr(
                            args, "skip_command_probe", False
                        ),
                        maintenance_status_validator=(
                            maintenance_status_validator
                        ),
                    )
                    session = retry_sequence.session
                    expectation = _post_uplink_expectation_for_session(
                        expectation, session
                    )
                    latest_scanner_slots = list(
                        retry_sequence.latest_slots
                    )
                    scanner_result = retry_sequence.scanner_result
                    scanner_attempt_history = list(
                        retry_sequence.attempt_history
                    )
                    scanner_stage_receipts = [
                        copy.deepcopy(attempt["stage_receipt"])
                        for attempt in scanner_attempt_history
                    ]
                else:
                    (
                        successful_pre_stage,
                        successful_maintenance,
                        successful_stage_receipt,
                        _successful_older,
                        _successful_recovery,
                        _successful_ids,
                    ) = scanner_result
                    scanner_attempt_history = [
                        _scanner_attempt_snapshot(
                            ordinal=1,
                            session=session,
                            requested_slots=list(slots),
                            pre_stage_status=successful_pre_stage,
                            stage_receipt=successful_stage_receipt,
                            campaign=successful_maintenance.get(
                                "update_campaign"
                            ),
                            outcome="converged",
                            classification=None,
                            recovery_action=None,
                            platform=platform,
                            artifacts=scanner_artifacts,
                            version=version,
                        )
                    ]
                    scanner_stage_receipts = [
                        copy.deepcopy(successful_stage_receipt)
                    ]
            final_status, final_evidence = _finalize_update_maintenance(
                badge,
                session=session,
                expectation=expectation,
                persisted_game_state=persisted_game_state,
                deadline=deadline,
            )
        except BaseException as primary:
            if getattr(badge, "_update_session", None) == session:
                _recover_failed_update_maintenance(
                    badge,
                    session=session,
                    persisted_game_state=persisted_game_state,
                    primary=primary,
                )
            raise

        if not slots:
            final_evidence = _prove_reversible_usb_theme_control(
                badge,
                initial_status=final_status,
                expectation=expectation,
                initial_evidence=final_evidence,
            )
            log(
                f"[verify] uplink {final_evidence.version} on "
                f"{final_evidence.running_partition}; "
                "reversible USB control ok"
            )
            return None

        if scanner_result is None:
            raise FlashError("maintenance scanner flow produced no result")
        (
            _latest_pre_stage_status,
            _maintenance_final_status,
            stage_receipt,
            preflight_older_slots,
            recovery_slots,
            final_hardware_ids,
        ) = scanner_result
        if scanner_artifacts is None or scanner_preflight_status is None:
            raise FlashError(
                "maintenance scanner proof lost its frozen authority"
            )
        _verify_scanner_attempt_history(
            scanner_attempt_history,
            scanner_stage_receipts,
            platform=platform,
            artifacts=scanner_artifacts,
            version=version,
            original_slots=slots,
            original_hardware_ids=final_hardware_ids,
        )
        latest_expected_hardware_ids = {
            slot: final_hardware_ids[slot]
            for slot in latest_scanner_slots
        }
        remaining_scanner_s = deadline - time.monotonic()
        if remaining_scanner_s <= 0:
            raise FlashError(
                "post-maintenance scanner proof exceeded its deadline"
            )
        wait_for_scanners_usb(
            badge,
            platform,
            latest_scanner_slots,
            version,
            timeout_s=max(1, int(remaining_scanner_s)),
            expected_hardware_ids=latest_expected_hardware_ids,
            expected_stage_receipt=stage_receipt,
            required_converged_slots=set(preflight_older_slots),
        )
        final_status = badge.status(timeout_s=5)
        final_evidence = _fresh_post_uplink_application_evidence(
            final_status,
            expectation=expectation,
            previous=final_evidence,
            label="post-maintenance scanner application status",
        )
        proven_newer_slots = coordinator_newer_skipped_slots(
            final_status, slots
        )
        verify_scanners(
            final_status,
            platform,
            slots,
            version,
            expected_hardware_ids=final_hardware_ids,
            allowed_newer_slots=proven_newer_slots,
        )
        verify_auto_update_convergence(
            final_status,
            latest_scanner_slots,
            expected_stage_receipt=stage_receipt,
            required_converged_slots=set(preflight_older_slots),
        )
        restored_status_snapshot: dict[str, Any] | None = None

        def reprove_after_theme(restored: dict[str, Any]) -> None:
            nonlocal restored_status_snapshot
            _verify_persisted_game_state(restored, persisted_game_state)
            restored_newer = coordinator_newer_skipped_slots(
                restored, slots
            )
            verify_scanners(
                restored,
                platform,
                slots,
                version,
                expected_hardware_ids=final_hardware_ids,
                allowed_newer_slots=restored_newer,
            )
            verify_auto_update_convergence(
                restored,
                latest_scanner_slots,
                expected_stage_receipt=stage_receipt,
                required_converged_slots=set(preflight_older_slots),
            )
            restored_status_snapshot = copy.deepcopy(restored)

        _prove_reversible_usb_theme_control(
            badge,
            initial_status=final_status,
            expectation=expectation,
            initial_evidence=final_evidence,
            restored_status_validator=reprove_after_theme,
        )
        if restored_status_snapshot is None:
            raise FlashError(
                "maintenance scanner flow did not prove restored theme"
            )
        return _issue_scanner_flow_result(
            pre_stage_status=scanner_preflight_status,
            final_restored_status=restored_status_snapshot,
            stage_receipt=stage_receipt,
            stage_receipts=tuple(scanner_stage_receipts),
            attempt_history=tuple(scanner_attempt_history),
            preflight_older_slots=flow_preflight_older_slots,
            recovery_slots=flow_recovery_slots,
        )


def _usb_flow_impl(
    args: argparse.Namespace,
    platform: dict[str, Any],
    need_uplink: bool,
    slots: list[str],
    version: str,
    *,
    _issue_scanner_flow_result: Callable[..., UsbScannerFlowResult],
    pre_mutation_validator: Callable[
        [dict[str, Any] | None, FrozenUsbFirmwareArtifacts],
        None,
    ] | None = None,
    maintenance_status_validator: Callable[
        [dict[str, Any], str], None
    ] | None = None,
    post_direct_bootstrap_status_validator: Callable[
        [dict[str, Any]], None
    ] | None = None,
    post_rom_bootstrap_status_validator: Callable[
        [dict[str, Any]], None
    ] | None = None,
    frozen_artifacts: FrozenUsbFirmwareArtifacts | None = None,
) -> UsbScannerFlowResult | None:
    log(f"[platform] {args.platform}: {platform['hardware']}")
    legacy_bootstrap = getattr(args, "legacy_usb_bootstrap", False)
    if type(legacy_bootstrap) is not bool:
        raise FlashError("legacy USB bootstrap flag must be an exact boolean")
    require_rom_recovery = getattr(args, "require_rom_recovery", False)
    if type(require_rom_recovery) is not bool:
        raise FlashError("required ROM recovery flag must be an exact boolean")
    if pre_mutation_validator is not None and not callable(
        pre_mutation_validator
    ):
        raise FlashError("USB pre-mutation validator must be callable")
    if maintenance_status_validator is not None and not callable(
        maintenance_status_validator
    ):
        raise FlashError("maintenance status validator must be callable")
    if post_direct_bootstrap_status_validator is not None and not callable(
        post_direct_bootstrap_status_validator
    ):
        raise FlashError(
            "post-direct-bootstrap status validator must be callable"
        )
    if post_rom_bootstrap_status_validator is not None and not callable(
        post_rom_bootstrap_status_validator
    ):
        raise FlashError(
            "post-ROM-bootstrap status validator must be callable"
        )
    if frozen_artifacts is not None:
        if type(frozen_artifacts) is not FrozenUsbFirmwareArtifacts:
            raise FlashError("supplied frozen USB artifacts are malformed")
        frozen_artifacts.__post_init__()
        if need_uplink and frozen_artifacts.uplink is None:
            raise FlashError("supplied frozen uplink artifacts are missing")
        if slots and frozen_artifacts.scanner is None:
            raise FlashError("supplied frozen scanner artifacts are missing")
    if require_rom_recovery and (legacy_bootstrap or not need_uplink):
        raise FlashError(
            "required ROM recovery needs the current hardened uplink target"
        )
    if require_rom_recovery:
        trusted = getattr(args, "trusted_uplink_binding", None)
        if (
            type(trusted) is not TrustedUplinkBinding
            or trusted.source != "retained-session"
            or getattr(args, "bind_selected_uplink", False) is not False
        ):
            raise FlashError(
                "required ROM recovery needs a retained-session binding"
            )
        if (
            not isinstance(getattr(args, "port", None), str)
            or not args.port
            or tuple(slots) != tuple(platform.get("slots", ()))
            or getattr(
                args, "recovery_rewrite_same_version", False
            ) is not True
            or getattr(args, "skip_command_probe", False) is not False
        ):
            raise FlashError(
                "required ROM recovery needs the exact full-badge "
                "recovery contract"
            )
    if legacy_bootstrap:
        if not getattr(args, "port", None):
            raise FlashError(
                "legacy USB bootstrap requires one explicit --port"
            )
        if not need_uplink:
            raise FlashError(
                "legacy USB bootstrap requires the uplink target"
            )
        if tuple(slots) != tuple(platform.get("slots", ())):
            raise FlashError(
                "legacy USB bootstrap requires both scanner targets"
            )
        if getattr(args, "recovery_rewrite_same_version", False) or \
                getattr(args, "skip_command_probe", False):
            raise FlashError(
                "legacy USB bootstrap refuses scanner recovery overrides"
            )
    if args.dry_run:
        planned = []
        if need_uplink:
            planned.append("guarded uplink application OTA or ROM recovery")
        if slots:
            planned.append("scanner stage and UART convergence: " + ", ".join(slots))
        log("[dry-run] plan: " + ("; ".join(planned) or "verification only"))
        log("[dry-run] no ports, artifacts, or device APIs were accessed")
        return

    if frozen_artifacts is None:
        frozen_artifacts = _prepare_frozen_usb_firmware_artifacts(
            platform,
            need_uplink,
            slots,
        )
    uplink_artifacts = frozen_artifacts.uplink
    if need_uplink and type(uplink_artifacts) is not FrozenArtifactSet:
        raise FlashError("frozen uplink artifact set is unavailable")
    if need_uplink:
        _attest_frozen_uplink_flash_authority(
            platform,
            uplink_artifacts,
            version,
        )
    scanner_image_size = (
        len(_frozen_firmware_bytes(
            frozen_artifacts.scanner,
            role="scanner",
        ))
        if slots
        else 0
    )
    initial_descriptor, selected_uplink_binding = \
        select_trusted_uplink_descriptor(
            selected_port=args.port,
            operator_acknowledged=getattr(
                args, "bind_selected_uplink", False
            ),
            trusted_binding=getattr(
                args, "trusted_uplink_binding", None
            ),
    )
    trusted_uplink_binding = _strengthen_trusted_uplink_binding(
        initial_descriptor,
        selected_uplink_binding,
    )
    initial_port = initial_descriptor.device
    log(f"[usb] using {initial_port}")
    application_status: dict[str, Any] | None = None
    if not legacy_bootstrap:
        # Prove the application before any ROM probe. Activity that does not
        # complete the exact proof remains a hard failure: it could be a
        # scanner or a damaged/stale application and must never authorize a
        # flash mutation.
        application_status = probe_application(initial_descriptor, 5)
        if require_rom_recovery and application_status is not None:
            raise FlashError(
                "ROM recovery requires the selected uplink to be in ROM; "
                "a running application was proven before mutation"
            )
    if pre_mutation_validator is not None:
        pre_mutation_validator(application_status, frozen_artifacts)
    if _uses_update_maintenance(version):
        return _usb_update_maintenance_flow(
            args,
            platform,
            need_uplink,
            slots,
            version,
            initial_descriptor=initial_descriptor,
            trusted_uplink_binding=trusted_uplink_binding,
            application_status=application_status,
            frozen_artifacts=frozen_artifacts,
            scanner_image_size=scanner_image_size,
            legacy_bootstrap=legacy_bootstrap,
            _issue_scanner_flow_result=_issue_scanner_flow_result,
            maintenance_status_validator=maintenance_status_validator,
            post_direct_bootstrap_status_validator=(
                post_direct_bootstrap_status_validator
            ),
            post_rom_bootstrap_status_validator=(
                post_rom_bootstrap_status_validator
            ),
        )
    expectation: _PostUplinkExpectation

    if legacy_bootstrap:
        if uplink_artifacts is None:
            raise FlashError("legacy bootstrap has no frozen uplink artifacts")
        rom_stage = legacy_usb_bootstrap_to_rom(
            initial_descriptor,
            uplink_artifacts,
            version,
            timeout_s=30,
        )
        expectation = _expectation_from_rom_flash(
            rom_stage,
            layout_version=version,
            artifacts=uplink_artifacts,
            update_session=_new_update_session(),
        )
    else:
        if application_status is None:
            if not need_uplink:
                raise FlashError(
                    "scanner-only flashing refused: the selected uplink is "
                    "silent and its application role cannot be proven"
                )
            if uplink_artifacts is None:
                raise FlashError("ROM recovery has no frozen uplink artifacts")
            rom_stage = _flash_silent_uplink_with_chord_fallback(
                initial_descriptor,
                trusted_uplink_binding,
                uplink_artifacts,
                version,
            )
            expectation = _expectation_from_rom_flash(
                rom_stage,
                layout_version=version,
                artifacts=uplink_artifacts,
                update_session=_new_update_session(),
            )
        else:
            hardware_id = validate_uplink_application_status(
                application_status
            )
            if need_uplink:
                with BadgeSerial(
                    initial_descriptor, False,
                    expected_hardware_id=hardware_id,
                ) as badge:
                    receipt = badge.upload_uplink_firmware(
                        platform,
                        uplink_artifacts,
                        version,
                        getattr(
                            args, "recovery_rewrite_same_version", False
                        ),
                    )
            else:
                receipt = {
                    "ok": True,
                    "skipped": True,
                    "phase": "current",
                    "hardware_id": hardware_id,
                    "version": version,
                    "partition": application_status.get(
                        "running_partition"
                    ),
                }
            expectation = _classify_uplink_update_receipt(
                receipt,
                pre_status=application_status,
                target_version=version,
                expected_sha256=(
                    hashlib.sha256(_frozen_firmware_bytes(
                        uplink_artifacts, role="uplink"
                    )).hexdigest()
                    if uplink_artifacts is not None
                    else ""
                ),
                expected_size=(
                    len(_frozen_firmware_bytes(
                        uplink_artifacts, role="uplink"
                    ))
                    if uplink_artifacts is not None
                    else 0
                ),
                update_session=_new_update_session(),
            )

    rebound_descriptor, post_evidence = wait_for_post_uplink_application(
        expectation, timeout_s=POST_UPLINK_APPLICATION_TIMEOUT_S
    )
    _validated_rom_port(rebound_descriptor.device)
    if rebound_descriptor.serial_number != \
            trusted_uplink_binding.serial_number:
        raise FlashError(
            "post-update application descriptor no longer matches the "
            "trusted uplink binding"
        )
    if rebound_descriptor.location != trusted_uplink_binding.location:
        raise FlashError(
            "post-update application moved outside the trusted uplink "
            "location policy"
        )
    post_evidence = _require_post_uplink_evidence(
        post_evidence, expectation
    )
    if not slots:
        with BadgeSerial(
            rebound_descriptor, False,
            expected_hardware_id=expectation.expected_hardware_id,
        ) as badge:
            final_status = badge.status()
            final_evidence = _fresh_post_uplink_application_evidence(
                final_status,
                expectation=expectation,
                previous=post_evidence,
                label="uplink-only final application status",
            )
            final_evidence = _prove_reversible_usb_theme_control(
                badge,
                initial_status=final_status,
                expectation=expectation,
                initial_evidence=final_evidence,
            )
        log(
            f"[verify] uplink {final_evidence.version} on "
            f"{final_evidence.running_partition}; reversible USB control ok"
        )
        return

    with BadgeSerial(
        rebound_descriptor, False,
        expected_hardware_id=expectation.expected_hardware_id,
    ) as badge:
        status = wait_for_scanner_status_usb(badge, slots)
        latest_evidence = verify_post_uplink_application(
            status,
            expected_hardware_id=expectation.expected_hardware_id,
            expected_version=expectation.expected_version,
            expected_partition=expectation.expected_partition,
        )
        latest_evidence = _require_post_uplink_evidence(
            latest_evidence, expectation
        )
        if latest_evidence.responses_completed <= \
                post_evidence.responses_completed:
            raise FlashError(
                "scanner preflight did not prove a fresh uplink USB response: "
                f"post={post_evidence.responses_completed}, "
                f"preflight={latest_evidence.responses_completed}"
            )
        expected_hardware_ids = capture_scanner_hardware_ids(
            status, platform, slots, require_connected=False
        )
        missing_preflight_ids = sorted(set(slots) - set(expected_hardware_ids))
        if missing_preflight_ids:
            log(
                "[usb] preflight MAC continuity unavailable for: " +
                ", ".join(missing_preflight_ids) +
                "; relay-time firmware will enforce immutable same-MAC "
                "continuity and final verification will enforce unique IDs"
            )
        newer_slots = scanner_update_newer_slots(
            status, slots, version, require_connected=False
        )
        preflight_older_slots = scanner_strictly_older_slots(
            status, list(expected_hardware_ids), version
        )
        if newer_slots:
            log(
                "[usb] scanner(s) already newer than the staged image will be "
                "durably skipped: " + ", ".join(sorted(newer_slots))
            )
        recovery_slots = choose_relay_slots(
            status,
            platform,
            slots,
            version,
            getattr(args, "recovery_rewrite_same_version", False),
            "usb",
        )
        pre_stage_status_snapshot = copy.deepcopy(status)
        preflight_older_slots_snapshot = frozenset(
            preflight_older_slots
        )
        recovery_slots_snapshot = frozenset(recovery_slots)

        stage_receipt = badge.stage_scanner_firmware(
            platform,
            frozen_artifacts.scanner,
            version,
            slots,
        )
        stage_receipt_snapshot = copy.deepcopy(stage_receipt)
        if recovery_slots:
            log(
                "[usb] explicit recovery mode selected; manually relaying only "
                "the exact same-version scanner slot(s)"
            )
        else:
            log(
                "[usb] scanner image staged; waiting for automatic strict-newer "
                "uplink convergence"
            )
        for slot in recovery_slots:
            # Same-version recovery must prove the immutable identity, UART
            # path, rollback/OTA state, and exact physical-slot role before
            # relay.  It intentionally does not require the old image's radio
            # to be healthy: repairing a failed radio is the recovery use case.
            # The post-reboot verification below restores the full radio gate.
            wait_for_scanners_usb(
                badge,
                platform,
                [slot],
                version,
                expected_hardware_ids=expected_hardware_ids,
                require_auto_update=False,
                require_radio_health=False,
            )
            badge.relay_scanner(
                slot,
                getattr(args, "skip_command_probe", False),
                True,
                scanner_image_size,
                expected_generation=stage_receipt_snapshot["generation"],
                expected_hardware_id=expected_hardware_ids[slot],
            )

        timeout_s = (
            scanner_relay_timeout_s(scanner_image_size) * len(slots)
            + 180
        )
        wait_for_scanners_usb(
            badge,
            platform,
            slots,
            version,
            timeout_s=timeout_s,
            expected_hardware_ids=expected_hardware_ids,
            expected_stage_receipt=stage_receipt,
            required_converged_slots=preflight_older_slots,
        )
        final_status = badge.status()
        final_evidence = _fresh_post_uplink_application_evidence(
            final_status,
            expectation=expectation,
            previous=latest_evidence,
            label="post-convergence uplink application status",
        )
        proven_newer_slots = coordinator_newer_skipped_slots(
            final_status, slots
        )
        verify_scanners(
            final_status,
            platform,
            slots,
            version,
            expected_hardware_ids=expected_hardware_ids,
            allowed_newer_slots=proven_newer_slots,
        )
        final_hardware_ids = capture_scanner_hardware_ids(
            final_status,
            platform,
            slots,
            require_connected=True,
        )
        verify_auto_update_convergence(
            final_status,
            slots,
            expected_stage_receipt=stage_receipt,
            required_converged_slots=preflight_older_slots,
        )

        restored_status_snapshot: dict[str, Any] | None = None

        def reprove_scanners_after_usb_control(
            restored_status: dict[str, Any],
        ) -> None:
            nonlocal restored_status_snapshot
            restored_newer_slots = coordinator_newer_skipped_slots(
                restored_status, slots
            )
            verify_scanners(
                restored_status,
                platform,
                slots,
                version,
                expected_hardware_ids=final_hardware_ids,
                allowed_newer_slots=restored_newer_slots,
            )
            verify_auto_update_convergence(
                restored_status,
                slots,
                expected_stage_receipt=stage_receipt,
                required_converged_slots=preflight_older_slots,
            )
            restored_status_snapshot = copy.deepcopy(restored_status)

        restored_evidence = _prove_reversible_usb_theme_control(
            badge,
            initial_status=final_status,
            expectation=expectation,
            initial_evidence=final_evidence,
            restored_status_validator=reprove_scanners_after_usb_control,
        )
        restored_evidence = _require_post_uplink_evidence(
            restored_evidence, expectation
        )
        if restored_evidence.responses_completed <= \
                final_evidence.responses_completed:
            raise FlashError(
                "scanner flow did not prove a fresh restored-theme USB "
                "response"
            )
        if restored_status_snapshot is None:
            raise FlashError(
                "scanner flow did not validate a restored-theme scanner "
                "status"
            )

        return _issue_scanner_flow_result(
            pre_stage_status=pre_stage_status_snapshot,
            final_restored_status=restored_status_snapshot,
            stage_receipt=stage_receipt_snapshot,
            preflight_older_slots=preflight_older_slots_snapshot,
            recovery_slots=recovery_slots_snapshot,
        )


def _make_attested_usb_flow(implementation: Callable[..., Any]) -> \
        tuple[Any, Any, Any]:
    issued: dict[
        int,
        tuple[
            UsbScannerFlowResult,
            bytes,
            bytes,
            bytes,
            frozenset[str],
            frozenset[str],
            int,
            bool,
            bool,
            tuple[bytes, ...],
            tuple[bytes, ...],
        ],
    ] = {}

    def encode_snapshot(value: dict[str, Any], label: str) -> bytes:
        if type(value) is not dict:
            raise FlashError(f"{label} must be an exact mapping")
        try:
            return json.dumps(
                copy.deepcopy(value),
                allow_nan=False,
                separators=(",", ":"),
                sort_keys=True,
            ).encode("utf-8")
        except (TypeError, ValueError) as exc:
            raise FlashError(f"{label} is not a JSON status snapshot") from exc

    def attested_record(
        result: UsbScannerFlowResult,
    ) -> tuple[
        UsbScannerFlowResult,
        bytes,
        bytes,
        bytes,
        frozenset[str],
        frozenset[str],
        int,
        bool,
        bool,
        tuple[bytes, ...],
        tuple[bytes, ...],
    ]:
        record = issued.get(id(result))
        if type(result) is not UsbScannerFlowResult or record is None or \
                record[0] is not result:
            raise FlashError(
                "USB scanner flow result lacks production issuance "
                "attestation"
            )
        if any(type(record[index]) is not bytes for index in (1, 2, 3)):
            raise FlashError(
                "USB scanner flow result snapshot attestation is malformed"
            )
        for proven_slots in record[4:6]:
            if type(proven_slots) is not frozenset or not \
                    proven_slots.issubset({"ble", "wifi"}):
                raise FlashError(
                    "USB scanner flow result slot attestation is malformed"
                )
        if type(record[6]) is not int or not (
            1 <= record[6] <= UPDATE_HOST_CAMPAIGNS_PER_LANE
        ) or \
                record[7] is not True or record[8] is not True:
            raise FlashError(
                "USB scanner flow result success attestation is malformed"
            )
        if type(record[9]) is not tuple or \
                len(record[9]) != record[6] or \
                any(type(value) is not bytes for value in record[9]):
            raise FlashError(
                "USB scanner stage history attestation is malformed"
            )
        if json.loads(record[9][-1]) != json.loads(record[3]):
            raise FlashError(
                "USB scanner latest stage receipt attestation drifted"
            )
        if type(record[10]) is not tuple or \
                len(record[10]) not in (0, record[6]) or \
                any(type(value) is not bytes for value in record[10]):
            raise FlashError(
                "USB scanner attempt history attestation is malformed"
            )
        if record[10]:
            attempts = tuple(json.loads(value) for value in record[10])
            if any(
                type(attempt) is not dict or
                attempt.get("ordinal") != ordinal or
                attempt.get("stage_receipt") !=
                    json.loads(record[9][ordinal - 1])
                for ordinal, attempt in enumerate(attempts, start=1)
            ) or attempts[-1].get("outcome") != "converged":
                raise FlashError(
                    "USB scanner attempt history proof is malformed"
                )
        return record

    def revalidate(
        result: UsbScannerFlowResult,
    ) -> UsbScannerFlowResult:
        attested_record(result)
        return result

    def result_value(
        result: UsbScannerFlowResult,
        field_name: str,
    ) -> Any:
        record = attested_record(result)
        if field_name == "pre_stage_status":
            return json.loads(record[1])
        if field_name == "final_restored_status":
            return json.loads(record[2])
        if field_name == "stage_receipt":
            return json.loads(record[3])
        if field_name == "stage_receipts":
            return tuple(json.loads(value) for value in record[9])
        if field_name == "attempt_history":
            return tuple(json.loads(value) for value in record[10])
        if field_name == "preflight_older_slots":
            return record[4]
        if field_name == "recovery_slots":
            return record[5]
        if field_name == "stage_count":
            return record[6]
        if field_name == "theme_restored":
            return record[7]
        if field_name == "fresh_usb_proven":
            return record[8]
        raise FlashError(
            f"unknown USB scanner flow result field: {field_name}"
        )

    def issue(
        *,
        pre_stage_status: dict[str, Any],
        final_restored_status: dict[str, Any],
        stage_receipt: dict[str, Any],
        stage_receipts: tuple[dict[str, Any], ...] | None = None,
        attempt_history: tuple[dict[str, Any], ...] | None = None,
        preflight_older_slots: frozenset[str],
        recovery_slots: frozenset[str],
    ) -> UsbScannerFlowResult:
        if type(preflight_older_slots) is not frozenset or \
                type(recovery_slots) is not frozenset:
            raise FlashError(
                "USB scanner flow result slot snapshots must be immutable"
            )
        if stage_receipts is None:
            stage_receipts = (stage_receipt,)
        if type(stage_receipts) is not tuple or \
                not 1 <= len(stage_receipts) <= \
                UPDATE_HOST_CAMPAIGNS_PER_LANE or \
                any(type(value) is not dict for value in stage_receipts):
            raise FlashError(
                "USB scanner stage history must be a bounded tuple"
            )
        if stage_receipts[-1] != stage_receipt:
            raise FlashError(
                "USB scanner latest stage receipt is not history tail"
            )
        if attempt_history is None:
            attempt_history = ()
        if type(attempt_history) is not tuple or \
                len(attempt_history) not in (0, len(stage_receipts)) or \
                any(type(value) is not dict for value in attempt_history):
            raise FlashError(
                "USB scanner attempt history must match its stages"
            )
        result = object.__new__(UsbScannerFlowResult)
        record = (
            result,
            encode_snapshot(
                pre_stage_status, "USB scanner pre-stage status"
            ),
            encode_snapshot(
                final_restored_status,
                "USB scanner restored-theme status",
            ),
            encode_snapshot(
                stage_receipt, "USB scanner stage receipt"
            ),
            preflight_older_slots,
            recovery_slots,
            len(stage_receipts),
            True,
            True,
            tuple(
                encode_snapshot(
                    value,
                    f"USB scanner stage receipt {index}",
                )
                for index, value in enumerate(
                    stage_receipts, start=1
                )
            ),
            tuple(
                encode_snapshot(
                    value,
                    f"USB scanner attempt history {index}",
                )
                for index, value in enumerate(
                    attempt_history, start=1
                )
            ),
        )
        issued[id(result)] = record
        try:
            return revalidate(result)
        except BaseException:
            issued.pop(id(result), None)
            raise

    def usb_flow(
        args: argparse.Namespace,
        platform: dict[str, Any],
        need_uplink: bool,
        slots: list[str],
        version: str,
        *,
        pre_mutation_validator: Callable[
            [dict[str, Any] | None, FrozenUsbFirmwareArtifacts],
            None,
        ] | None = None,
        maintenance_status_validator: Callable[
            [dict[str, Any], str], None
        ] | None = None,
        post_direct_bootstrap_status_validator: Callable[
            [dict[str, Any]], None
        ] | None = None,
        post_rom_bootstrap_status_validator: Callable[
            [dict[str, Any]], None
        ] | None = None,
        frozen_artifacts: FrozenUsbFirmwareArtifacts | None = None,
    ) -> UsbScannerFlowResult | None:
        return implementation(
            args,
            platform,
            need_uplink,
            slots,
            version,
            _issue_scanner_flow_result=issue,
            pre_mutation_validator=pre_mutation_validator,
            maintenance_status_validator=maintenance_status_validator,
            post_direct_bootstrap_status_validator=(
                post_direct_bootstrap_status_validator
            ),
            post_rom_bootstrap_status_validator=(
                post_rom_bootstrap_status_validator
            ),
            frozen_artifacts=frozen_artifacts,
        )

    return usb_flow, revalidate, result_value


(
    usb_flow,
    _revalidate_usb_scanner_flow_result,
    _usb_scanner_flow_result_value,
) = _make_attested_usb_flow(_usb_flow_impl)
del _make_attested_usb_flow
del _usb_flow_impl


def enable_network_from_usb(
    descriptor: UsbDescriptorRecord,
    mode: str,
    ttl_s: int,
    dry_run: bool,
) -> None:
    with BadgeSerial(descriptor, dry_run) as badge:
        badge.wait_ping()
        badge.ctl({"cmd": "network", "mode": mode, "ttl_s": ttl_s})


def network_base_url(args: argparse.Namespace) -> str:
    if args.transport == "ap":
        return "http://192.168.4.1"
    if args.host:
        host = args.host
    elif args.node:
        host = resolve_node(args.backend, args.node)
    else:
        raise FlashError("LAN transport requires --host or --node")
    if host.startswith("http://") or host.startswith("https://"):
        return host.rstrip("/")
    return f"http://{host}"


def network_flow(args: argparse.Namespace, platform: dict[str, Any],
                 need_uplink: bool, slots: list[str], version: str) -> None:
    require_usb_firmware_transport(args.transport)
    base_url = network_base_url(args)
    log(f"[platform] {args.platform}: {platform['hardware']}")
    log(f"[network] using {base_url}")

    if args.port:
        mode = "local_ap" if args.transport == "ap" else "backend"
        descriptor, _binding = select_trusted_uplink_descriptor(
            selected_port=args.port,
            operator_acknowledged=getattr(
                args, "bind_selected_uplink", False
            ),
        )
        enable_network_from_usb(
            descriptor, mode, args.network_ttl_s, args.dry_run
        )

    initial_status: dict[str, Any] = {}
    if not args.dry_run:
        initial_status = wait_http_status(base_url, timeout_s=90)

    flash_uplink = need_uplink
    if need_uplink and not args.dry_run:
        flash_uplink = uplink_flash_needed(
            initial_status,
            platform,
            version,
            getattr(args, "recovery_rewrite_same_version", False),
        )

    if slots:
        relay_slots = list(slots)
        same_version_recovery_slots: set[str] = set()
        if not args.dry_run:
            status = wait_for_scanner_status_network(base_url, slots)
            if not scanner_status_ready(status, slots):
                reject_scanner_downgrades(status, slots, version)
                raise FlashError(
                    "cannot prove downgrade safety: every requested scanner "
                    "must be connected and report its current version"
                )
            else:
                reject_scanner_downgrades(status, slots, version)
                current = current_scanner_slots(
                    status, platform, slots, version
                )
                relay_slots = [slot for slot in slots if slot not in current]
                same_version_recovery_slots = set(choose_relay_slots(
                    status,
                    platform,
                    slots,
                    version,
                    getattr(args, "recovery_rewrite_same_version", False),
                    "network",
                ))
                relay_slots.extend(same_version_recovery_slots)
        if relay_slots:
            upload_scanner_network(platform, base_url, version, args.dry_run)
        for slot in relay_slots:
            relay_scanner_network(base_url, slot, args.dry_run,
                                  args.skip_command_probe,
                                  slot in same_version_recovery_slots,
                                  scanner_firmware_size(platform))
        if args.dry_run:
            log("[verify] scanner versions: " + ", ".join(slots))
        else:
            wait_for_scanners_network(base_url, platform, slots, version)

    if flash_uplink:
        flash_uplink_network(platform, base_url, args.dry_run)
        if args.dry_run:
            log("[verify] uplink version after reboot")
        else:
            status = wait_http_status(base_url, timeout_s=180)
            verify_uplink_identity_fields(status, platform)
            verify_uplink_status(status, version)


def manual_scanner_flow(args: argparse.Namespace, platform: dict[str, Any],
                        version: str) -> None:
    slot = args.manual_scanner
    port = args.port or detect_usb_port()
    log(f"[platform] {args.platform}: {platform['hardware']}")
    log(f"[manual] direct USB scanner flash for {slot} scanner on {port}")
    if args.dry_run:
        flash_scanner_usb(platform, port, True, slot)
        log(f"[verify] {slot} scanner identity/version via uplink status")
        return
    if not args.verify_port:
        raise FlashError(
            "cannot prove downgrade safety for a direct scanner flash without "
            "--verify-port pointing to the connected badge uplink"
        )

    log(
        f"[manual] preflighting {slot} through uplink {args.verify_port} "
        "before direct flash"
    )
    descriptor, _binding = select_trusted_uplink_descriptor(
        selected_port=args.verify_port,
        operator_acknowledged=getattr(
            args, "bind_selected_uplink", False
        ),
    )
    with BadgeSerial(descriptor, False) as badge:
        badge.wait_ping()
        status = wait_for_scanner_status_usb(badge, [slot])
        expected_hardware_ids = capture_scanner_hardware_ids(
            status, platform, [slot]
        )
        reject_scanner_downgrades(status, [slot], version)
        current = current_scanner_slots(status, platform, [slot], version)
        if slot in current and not getattr(
            args, "recovery_rewrite_same_version", False
        ):
            log(
                f"[manual] {slot} scanner already has {version}; direct "
                "same-version rewrite is disabled"
            )
            return

        flash_scanner_usb(platform, port, False, slot)
        wait_for_scanners_usb(
            badge,
            platform,
            [slot],
            version,
            timeout_s=240,
            expected_hardware_ids=expected_hardware_ids,
            require_auto_update=False,
        )


def parse_args() -> argparse.Namespace:
    parser = _PrivateArgumentParser(
        prog="fof_badge_flash.py",
        description=__doc__,
    )
    parser.add_argument("--transport", choices=("usb", "ap", "lan"), default="usb")
    parser.add_argument("--platform", choices=sorted(PLATFORMS),
                        default="badge-trio-xiao-s3")
    parser.add_argument("--port", help="USB serial port for USB flashing or enabling AP/LAN")
    parser.add_argument(
        "--bind-selected-uplink",
        action="store_true",
        help=(
            "Acknowledge that --port is the uplink role in a complete "
            "three-cable USB descriptor census"
        ),
    )
    parser.add_argument("--host", help="LAN badge host/IP for --transport lan")
    parser.add_argument("--backend", default=DEFAULT_BACKEND)
    parser.add_argument("--node", help="Backend node/device id for --transport lan")
    parser.add_argument("--only", choices=("uplink", "scanners", "ble", "wifi", "all"),
                        default="all")
    parser.add_argument("--manual-scanner", choices=("ble", "wifi"),
                        help=(
                            "Disabled compatibility option: scanner USB is "
                            "diagnostics-only; stage firmware through the "
                            "uplink USB/UART path"
                        ))
    parser.add_argument("--verify-port",
                        help=(
                            "Disabled compatibility option paired with "
                            "--manual-scanner"
                        ))
    parser.add_argument(
        "--legacy-usb-bootstrap",
        action="store_true",
        help=(
            "Explicit factory recovery: after exact legacy badge identity and "
            "health proof, request ROM over USB and flash the complete guarded "
            "uplink layout, then update both scanners over UART; requires "
            "--transport usb, --only all, and an explicit --port"
        ),
    )
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--force", action="store_true",
                        help="Accepted for compatibility; does not bypass version safety")
    parser.add_argument("--allow-same-version", action="store_true",
                        help=argparse.SUPPRESS)
    parser.add_argument(
        "--recovery-rewrite-same-version",
        action="store_true",
        help=(
            "Recovery-only: manually relay an exact same-version scanner image; "
            "never permits a downgrade"
        ),
    )
    parser.add_argument("--skip-current", action="store_true",
                        help="Deprecated compatibility option; same-version scanners are skipped by default")
    parser.add_argument("--skip-command-probe", action="store_true",
                        help="Recovery-only: skip scanner command-ingress probe during relay")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--network-ttl-s", type=int, default=900)
    return parser.parse_args()


def main() -> int:
    try:
        args = parse_args()
        if getattr(args, "manual_scanner", None):
            raise FlashError(
                "--manual-scanner direct USB flashing is disabled; scanner "
                "USB is diagnostics-only and scanner firmware must be staged "
                "through the uplink USB/UART path"
            )
        platform = PLATFORMS[args.platform]
        version = repo_version(platform)
        need_uplink, slots = selected_targets(args.only)
        require_usb_firmware_transport(args.transport)
        if args.allow_same_version:
            raise FlashError(
                "--allow-same-version is disabled; use the explicit "
                "--recovery-rewrite-same-version flag for an exact "
                "same-version recovery only"
            )
        if args.legacy_usb_bootstrap and args.transport != "usb":
            raise FlashError(
                "--legacy-usb-bootstrap requires --transport usb"
            )
        if args.legacy_usb_bootstrap and args.only != "all":
            raise FlashError(
                "--legacy-usb-bootstrap requires --only all"
            )
        if args.legacy_usb_bootstrap and (
            args.recovery_rewrite_same_version or args.skip_command_probe
        ):
            raise FlashError(
                "--legacy-usb-bootstrap refuses scanner recovery overrides"
            )
        if not args.skip_build:
            build_firmware(platform, args.dry_run)
        if not args.dry_run or args.skip_build:
            require_artifacts(platform, need_uplink, slots)

        if args.transport == "usb":
            usb_flow(args, platform, need_uplink, slots, version)
        else:
            network_flow(args, platform, need_uplink, slots, version)
    except _CliArgumentError as exc:
        print_user_visible(
            f"ERROR: {exc}",
            file=sys.stderr,
        )
        return 2
    except FlashError as exc:
        print_user_visible(
            f"ERROR: {exc}",
            file=sys.stderr,
        )
        return 1
    except KeyboardInterrupt:
        print_user_visible(
            "Interrupted",
            file=sys.stderr,
        )
        return 130
    log("[done] badge flash flow complete")
    return 0


if __name__ == "__main__":
    sys.exit(main())
