#!/usr/bin/env python3
"""Tests for the battery-connected badge USB hardening acceptance harness."""

from __future__ import annotations

import copy
import dataclasses
import json
import contextlib
import hashlib
import io
import inspect
import os
import pwd
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import verify_badge_usb_hardening as acceptance
import secure_artifact_tree as artifact_tree


LEGACY_VERSION = "0.64.76-badge-defcon34"
V078_VERSION = "0.64.78-badge-defcon34"
CANARY_VERSION = "0.64.79-badge-defcon34"
VERSION = CANARY_VERSION
CANARY_PLATFORM_KEY = "badge-trio-xiao-s3-con-crud-canary"
UPLINK_ID = "02:00:00:00:00:01"
BLE_ID = "02:00:00:00:00:02"
WIFI_ID = "02:00:00:00:00:03"


def _usb_record(
    device: str,
    serial_number: str = UPLINK_ID,
) -> acceptance.flash.UsbDescriptorRecord:
    path_suffix = hashlib.sha256(device.encode("utf-8")).hexdigest()[:8]
    location_suffix = hashlib.sha256(
        serial_number.lower().encode("ascii")
    ).hexdigest()[:8]
    return acceptance.flash.UsbDescriptorRecord(
        device=device,
        vid=acceptance.flash.ESPRESSIF_USB_SERIAL_JTAG_VID,
        pid=acceptance.flash.ESPRESSIF_USB_SERIAL_JTAG_PID,
        serial_number=serial_number,
        location=f"acceptance-{location_suffix}",
        stat_device=1,
        stat_inode=int(path_suffix, 16),
        stat_rdev=int(path_suffix, 16),
    )


def _uplink_firmware_image(
    size: int = 131_072,
    *,
    version: str = VERSION,
) -> bytes:
    platform = acceptance.flash.PLATFORMS[CANARY_PLATFORM_KEY]
    image = bytearray(size)
    image[0] = 0xE9
    struct.pack_into("<I", image, 0x20, 0xABCD5432)
    image[0x30:0x50] = version.encode("ascii").ljust(32, b"\x00")
    image[0x50:0x70] = platform["uplink_project"].encode(
        "ascii"
    ).ljust(32, b"\x00")
    cursor = 0x100
    for marker in (
        platform["uplink_name"],
        platform["hardware_type"],
    ):
        encoded = b"\x00" + marker.encode("ascii") + b"\x00"
        image[cursor:cursor + len(encoded)] = encoded
        cursor += len(encoded)
    return bytes(image)


def _scanner_firmware_image(
    size: int = 131_072,
    *,
    version: str = VERSION,
) -> bytes:
    platform = acceptance.flash.PLATFORMS[CANARY_PLATFORM_KEY]
    image = bytearray(size)
    image[0] = 0xE9
    struct.pack_into("<I", image, 0x20, 0xABCD5432)
    image[0x30:0x50] = version.encode("ascii").ljust(32, b"\x00")
    image[0x50:0x70] = platform["scanner_project"].encode(
        "ascii"
    ).ljust(32, b"\x00")
    cursor = 0x100
    for marker in (
        platform["scanner_name"],
        platform["hardware_type"],
    ):
        encoded = b"\x00" + marker.encode("ascii") + b"\x00"
        image[cursor:cursor + len(encoded)] = encoded
        cursor += len(encoded)
    return bytes(image)


def _frozen_artifacts(
    image: bytes,
) -> artifact_tree.FrozenArtifactSet:
    content = image
    receipt_sha256 = "0" * 64
    member = artifact_tree.FrozenArtifactMember(
        logical_name="artifact.firmware",
        size=len(content),
        sha256=hashlib.sha256(content).hexdigest(),
        content=content,
    )
    members = (member,)
    return artifact_tree.FrozenArtifactSet(
        receipt_sha256=receipt_sha256,
        members=members,
        aggregate_sha256=artifact_tree._aggregate_sha256(
            receipt_sha256,
            members,
        ),
    )


def _frozen_uplink_artifacts(
    image: bytes | None = None,
) -> artifact_tree.FrozenArtifactSet:
    return _frozen_artifacts(
        _uplink_firmware_image() if image is None else image
    )


def _frozen_candidate_artifacts() -> \
        acceptance.flash.FrozenUsbFirmwareArtifacts:
    return acceptance.flash.FrozenUsbFirmwareArtifacts(
        uplink=_frozen_artifacts(_uplink_firmware_image()),
        scanner=_frozen_artifacts(_scanner_firmware_image()),
    )


def _candidate_artifacts() -> acceptance.VerifiedCandidateArtifacts:
    return acceptance.verify_candidate_artifacts(
        _frozen_candidate_artifacts(),
        VERSION,
    )


class _StrictInterruptedBadge:
    def __init__(
        self,
        *,
        status: dict | None = None,
        artifacts: artifact_tree.FrozenArtifactSet | None = None,
        retry: bool = False,
    ) -> None:
        self._status = status
        self._artifacts = artifacts
        self._retry = retry
        self.upload_calls: list[tuple] = []

    def __enter__(self):
        return self

    def __exit__(self, _exc_type, _exc, _traceback) -> None:
        return None

    def status(self, timeout_s: int = 5) -> dict:
        if timeout_s != 5 or self._status is None:
            raise AssertionError("unexpected strict badge status call")
        return copy.deepcopy(self._status)

    def upload_uplink_firmware(
        self,
        platform: dict,
        artifacts: artifact_tree.FrozenArtifactSet,
        version: str,
        recovery_rewrite_same_version: bool = False,
    ) -> dict:
        if not self._retry:
            raise AssertionError("upload invoked on a non-retry badge")
        if platform is not acceptance.flash.PLATFORMS[
            CANARY_PLATFORM_KEY
        ]:
            raise AssertionError("retry platform identity changed")
        if artifacts is not self._artifacts:
            raise AssertionError("retry frozen artifact identity changed")
        if version != VERSION or recovery_rewrite_same_version is not True:
            raise AssertionError("retry version/recovery semantics changed")
        self.upload_calls.append((
            platform,
            artifacts,
            version,
            recovery_rewrite_same_version,
        ))
        return {"phase": "committed"}


class _StrictInterruptedBadgeFactory:
    def __init__(
        self,
        descriptors: tuple[acceptance.flash.UsbDescriptorRecord, ...],
        badges: tuple[_StrictInterruptedBadge, ...],
    ) -> None:
        self._descriptors = descriptors
        self._badges = badges
        self.calls: list[tuple] = []

    def __call__(
        self,
        descriptor: acceptance.flash.UsbDescriptorRecord,
        exclusive: bool,
        *,
        expected_hardware_id: str,
    ) -> _StrictInterruptedBadge:
        index = len(self.calls)
        if type(descriptor) is not acceptance.flash.UsbDescriptorRecord:
            raise AssertionError("BadgeSerial received a non-descriptor")
        if index >= len(self._descriptors) or \
                descriptor is not self._descriptors[index]:
            raise AssertionError("BadgeSerial descriptor order changed")
        if exclusive is not False or expected_hardware_id != UPLINK_ID:
            raise AssertionError("BadgeSerial binding arguments changed")
        self.calls.append((
            descriptor,
            exclusive,
            expected_hardware_id,
        ))
        return self._badges[index]


def _scanner(
    slot: str,
    hardware_id: str,
    *,
    version: str = VERSION,
) -> dict:
    ble_primary = slot == "ble"
    profile = "ble_primary" if ble_primary else "wifi_primary"
    return {
        "uart": slot,
        "connected": True,
        "board": "scanner-s3-combo-fof_badge",
        "firmware_name": "scanner-s3-combo-fof_badge",
        "app_project": "fof_badge_scanner",
        "hardware_type": "seeed_xiao_esp32s3",
        "hardware_id": hardware_id,
        "ver": version,
        "rollback_pending": False,
        "recovery_mode": "normal",
        "health": "ok",
        "ota_state": "idle",
        "slot_role": profile,
        "expected_scan_profile": profile,
        "scan_profile": profile,
        "role_acked": True,
        "ble_initialized": ble_primary,
        "ble_scanning": ble_primary,
        "ble_host_active": ble_primary,
        "ble_host_synced": ble_primary,
        "wifi_paused": ble_primary,
        "wifi_initialized": not ble_primary,
        "wifi_init_rc": 0,
        "wifi_active": not ble_primary,
        "full_scan_ok": 0 if ble_primary else 4,
    }


def _usb_health(*, parser_state: str = "command",
                responses_completed: int = 18) -> dict:
    return {
        "schema": 1,
        "task_started": True,
        "host_connected": True,
        "parser_state": parser_state,
        "rx_bytes": 1024,
        "valid_commands": 18,
        "responses_completed": responses_completed,
        "required_response_failures": 0,
        "malformed_lines": 0,
        "dropped_progress_frames": 0,
        "dropped_optional_frames": 0,
        "upload_received": 0,
        "upload_size": 0,
        "task_heartbeat_age_s": 0,
        "last_rx_age_s": 0,
        "last_command_age_s": 0,
        "last_response_age_s": 0,
        "last_upload_progress_age_s": None,
    }


def _status(*, uplink_id: str = UPLINK_ID,
            ble_id: str = BLE_ID, wifi_id: str = WIFI_ID,
            parser_state: str = "command",
            reboot_reason: str = "",
            version: str = VERSION,
            partition: str = "ota_0",
            reboot_generation: int = 9,
            responses_completed: int = 18) -> dict:
    return {
        "version": version,
        "target": "uplink-s3-fof_badge",
        "firmware_name": "uplink-s3-fof_badge",
        "project": "fof_badge_uplink",
        "app_project": "fof_badge_uplink",
        "hardware_type": "seeed_xiao_esp32s3",
        "hardware_id": uplink_id,
        "running_partition": partition,
        "uptime_s": 100,
        "pending_verify": False,
        "rollback_state": "clear",
        "recovery_mode": "normal",
        "safe_mode": False,
        "usb_control_alive": True,
        "scanner_uart_alive": True,
        "last_expected_reboot_reason": reboot_reason,
        "last_expected_reboot_generation": reboot_generation,
        "usb_health": _usb_health(
            parser_state=parser_state,
            responses_completed=responses_completed,
        ),
        "stack_main_free": 4096,
        "stack_display_free": 3072,
        "stack_usb_free": 2048,
        "stack_uart_ble_free": 4096,
        "stack_uart_wifi_free": 4096,
        "heap_internal_free": 32768,
        "heap_internal_min_free": 16384,
        "heap_internal_largest": 20000,
        "detection_queue_capacity": 0,
        "scanners": [
            _scanner("ble", ble_id, version=version),
            _scanner("wifi", wifi_id, version=version),
        ],
        "firmware_store": {
            "stored": True,
            "generation": 7,
            "sha256": "a" * 64,
        },
    }


def _maintenance_status(
    *,
    version: str,
    update_session: str,
    partition: str = "ota_0",
    uplink_phase: str = "idle",
    uplink_received: int = 0,
    uplink_size: int = 0,
    uplink_sha256: str = "",
    reboot_generation: int = 10,
    responses_completed: int = 1,
) -> dict:
    status = _status(
        version=version,
        partition=partition,
        reboot_generation=reboot_generation,
        responses_completed=responses_completed,
    )
    status["uptime_s"] = 5
    status["usb_health"]["valid_commands"] = responses_completed
    status.update({
        "recovery_mode": "update_maintenance",
        "update_session": update_session,
        "ble_initialized": False,
        "update_uplink": {
            "phase": uplink_phase,
            "session": update_session,
            "version": version if uplink_phase != "idle" else "",
            "sha256": uplink_sha256 if uplink_phase != "idle" else "",
            "size": uplink_size if uplink_phase != "idle" else 0,
            "partition": (
                "ota_1" if uplink_phase != "idle" else ""
            ),
            "received": (
                uplink_received if uplink_phase != "idle" else 0
            ),
        },
        "update_scanner": {
            "phase": "idle",
            "session": update_session,
            "target": "",
            "sha256": "",
            "size": 0,
            "slot_mask": 0,
            "received": 0,
            "generation": 0,
        },
    })
    return status


def _session() -> acceptance.BadgeAcceptanceSession:
    return acceptance.BadgeAcceptanceSession(
        session_id="defcon34-canary-001",
        uplink_hardware_id=UPLINK_ID,
        ble_hardware_id=BLE_ID,
        wifi_hardware_id=WIFI_ID,
    )


def _scanner_stage_receipt(
    generation: int,
    *,
    slot_mask: int = 3,
) -> dict:
    image = _scanner_firmware_image()
    size = len(image)
    return {
        "ok": True,
        "target": "scanner-s3-combo-fof_badge",
        "name": "scanner-s3-combo-fof_badge",
        "app_project": "fof_badge_scanner",
        "project": "fof_badge_scanner",
        "hardware_type": "seeed_xiao_esp32s3",
        "hardware": "seeed_xiao_esp32s3",
        "version": VERSION,
        "size": size,
        "crc32": acceptance.binascii.crc32(image) & 0xFFFFFFFF,
        "sha256": hashlib.sha256(image).hexdigest(),
        "slot_mask": slot_mask,
        "generation": generation,
        "flow_control": "credit-v1",
        "phase": "final",
        "received": size,
        "total": size,
        "credit_bytes": 0,
    }


class _UsbCycleResultFixture:
    def __init__(
        self,
        *,
        pre_stage_status: dict,
        final_restored_status: dict,
        stage_receipt: dict,
        preflight_older_slots: frozenset[str],
        recovery_slots: frozenset[str],
    ) -> None:
        self.pre_stage_status = copy.deepcopy(pre_stage_status)
        self.final_restored_status = copy.deepcopy(final_restored_status)
        self.stage_receipt = copy.deepcopy(stage_receipt)
        self.stage_receipts = (self.stage_receipt,)
        self.attempt_history = ()
        self.preflight_older_slots = preflight_older_slots
        self.recovery_slots = recovery_slots
        self.stage_count = 1
        self.theme_restored = True
        self.fresh_usb_proven = True


def _usb_cycle_result(
    cycle: int,
    *,
    generation: int | None = None,
) -> _UsbCycleResultFixture:
    generation = generation if generation is not None else 20 + cycle
    before = _status(
        version=CANARY_VERSION,
        partition=("ota_1", "ota_1", "ota_0")[cycle - 1],
        reboot_generation=(1, 3, 5)[cycle - 1],
        responses_completed=(30, 48, 58)[cycle - 1],
    )
    before["uptime_s"] = (5, 22, 22)[cycle - 1]
    before["usb_health"]["rx_bytes"] = (1500, 2100, 3100)[cycle - 1]
    before["usb_health"]["valid_commands"] = (30, 48, 58)[cycle - 1]
    if cycle == 1:
        for scanner in before["scanners"]:
            scanner["ver"] = "0.64.75-badge-defcon34"
    restored = _status(
        reboot_reason="update_finish",
        partition=("ota_1", "ota_0", "ota_1")[cycle - 1],
        reboot_generation=(3, 5, 7)[cycle - 1],
        responses_completed=(41, 51, 61)[cycle - 1],
    )
    restored["uptime_s"] = 20
    restored["usb_health"] = _usb_health(
        responses_completed=(41, 51, 61)[cycle - 1]
    )
    restored["usb_health"]["rx_bytes"] = (2000, 3000, 4000)[cycle - 1]
    restored["usb_health"]["valid_commands"] = (40, 50, 60)[cycle - 1]
    receipt = _scanner_stage_receipt(generation)
    converged = cycle == 1
    restored["firmware_store"] = {
        "stored": True,
        "target": receipt["target"],
        "app_project": receipt["app_project"],
        "hardware_type": receipt["hardware_type"],
        "version": receipt["version"],
        "size": receipt["size"],
        "crc32": receipt["crc32"],
        "sha256": receipt["sha256"],
        "generation": generation,
        "auto_update": {
            "generation": generation,
            "target_slot_mask": 3,
            "pending_mask": 0,
            "worker_running": False,
            "readiness_probes": [1, 1],
            "scanners": [
                {
                    "slot": 0,
                    "state": "converged" if converged else "current",
                    "attempts": 1 if converged else 0,
                },
                {
                    "slot": 1,
                    "state": "converged" if converged else "current",
                    "attempts": 1 if converged else 0,
                },
            ],
        },
    }
    return _UsbCycleResultFixture(
        pre_stage_status=before,
        final_restored_status=restored,
        stage_receipt=receipt,
        preflight_older_slots=frozenset(
            {"ble", "wifi"} if cycle == 1 else set()
        ),
        recovery_slots=frozenset(
            set() if cycle == 1 else {"ble", "wifi"}
        ),
    )


def _usb_cycle_retry_result() -> _UsbCycleResultFixture:
    result = _usb_cycle_result(1, generation=21)
    first_receipt = copy.deepcopy(result.stage_receipt)
    retry_receipt = _scanner_stage_receipt(22, slot_mask=2)
    retry_pre_stage = copy.deepcopy(result.pre_stage_status)
    retry_pre_stage["scanners"][0]["ver"] = VERSION
    first_campaign = {
        "generation": 21,
        "target_slot_mask": 3,
        "pending_mask": 0,
        "worker_running": False,
        "readiness_probes": [1, 1],
        "scanners": [
            {"slot": 0, "state": "converged", "attempts": 1},
            {"slot": 1, "state": "failed", "attempts": 3},
        ],
    }
    retry_campaign = {
        "generation": 22,
        "target_slot_mask": 2,
        "pending_mask": 0,
        "worker_running": False,
        "readiness_probes": [1, 1],
        "scanners": [
            {"slot": 0, "state": "excluded", "attempts": 0},
            {"slot": 1, "state": "converged", "attempts": 1},
        ],
    }
    result.stage_receipt = copy.deepcopy(retry_receipt)
    result.stage_receipts = (
        copy.deepcopy(first_receipt),
        copy.deepcopy(retry_receipt),
    )
    result.attempt_history = (
        {
            "ordinal": 1,
            "session": "1111111111111111",
            "requested_slots": ["ble", "wifi"],
            "pre_stage_status": copy.deepcopy(result.pre_stage_status),
            "stage_receipt": copy.deepcopy(first_receipt),
            "campaign": first_campaign,
            "outcome": "failed",
            "classification": "ota_ack_timeout",
            "recovery_action": "session_abort",
            "verified_target": VERSION,
        },
        {
            "ordinal": 2,
            "session": "2222222222222222",
            "requested_slots": ["wifi"],
            "pre_stage_status": retry_pre_stage,
            "stage_receipt": copy.deepcopy(retry_receipt),
            "campaign": retry_campaign,
            "outcome": "converged",
            "classification": "ota_ack_timeout",
            "recovery_action": "session_abort",
            "verified_target": VERSION,
        },
    )
    result.stage_count = 2
    store = result.final_restored_status["firmware_store"]
    for key in (
        "target",
        "app_project",
        "hardware_type",
        "version",
        "size",
        "crc32",
        "sha256",
        "generation",
    ):
        store[key] = retry_receipt[key]
    store["auto_update"] = copy.deepcopy(retry_campaign)
    return result


def _cycle_source_status(cycle: int) -> dict:
    if cycle == 1:
        status = LivePromotionMetricsTest._baseline_status(
            responses_completed=26
        )
        status["uptime_s"] = 102
        status["usb_health"]["rx_bytes"] = 1080
        for scanner in status["scanners"]:
            scanner["ver"] = "0.64.75-badge-defcon34"
        return status
    status = _status(
        version=CANARY_VERSION,
        partition=("ota_1", "ota_0")[cycle - 2],
        reboot_generation=(3, 5)[cycle - 2],
        responses_completed=(47, 57)[cycle - 2],
    )
    status["uptime_s"] = 21
    status["usb_health"]["rx_bytes"] = (2068, 3068)[cycle - 2]
    status["usb_health"]["valid_commands"] = (46, 56)[cycle - 2]
    return status


def _issued_maintenance_sample(
    *,
    generation: int,
    partition: str,
    session: acceptance.BadgeAcceptanceSession | None = None,
) -> acceptance.VerifiedLiveMetricSample:
    bound_session = _session() if session is None else session
    status = _maintenance_status(
        version=CANARY_VERSION,
        update_session="0123456789ABCDEF",
        partition=partition,
        reboot_generation=generation,
        responses_completed=10,
    )
    return acceptance.VerifiedLiveMetricSample(
        acceptance._LIVE_METRIC_ISSUER,
        {
            "version": CANARY_VERSION,
            "session_id": bound_session.session_id,
            "hardware_id": bound_session.uplink_hardware_id,
            "recovery_mode": "update_maintenance",
            "update_session": "0123456789ABCDEF",
            "reboot_generation_capability": "reported",
            "running_partition": partition,
            "uptime_s": status["uptime_s"],
            "reboot_generation": generation,
            "rx_bytes": status["usb_health"]["rx_bytes"],
            "valid_commands": status["usb_health"]["valid_commands"],
            "responses_completed": status["usb_health"][
                "responses_completed"
            ],
            **{
                field: status[field]
                for field in acceptance._LIVE_METRIC_FIELDS
            },
        },
    )


def _verify_cycle_fixture(
    cycle: int,
    *,
    generation: int | None = None,
    result: _UsbCycleResultFixture | None = None,
    source_status: dict | None = None,
    session: acceptance.BadgeAcceptanceSession | None = None,
) -> acceptance.VerifiedCycleCheckpoint:
    bound_session = _session() if session is None else session
    fixture = result or _usb_cycle_result(cycle, generation=generation)
    source_pre_snapshot = acceptance.verify_cycle_pre_snapshot(
        _cycle_source_status(cycle)
        if source_status is None
        else source_status,
        bound_session,
        VERSION,
        cycle,
    )
    with mock.patch.object(
        acceptance.flash,
        "_revalidate_usb_scanner_flow_result",
        return_value=fixture,
    ):
        return acceptance.verify_update_cycle_result(
            fixture,
            bound_session,
            VERSION,
            cycle,
            candidate_artifacts=_candidate_artifacts(),
            source_pre_snapshot=source_pre_snapshot,
            final_maintenance_sample=_issued_maintenance_sample(
                generation=(2, 4, 6)[cycle - 1],
                partition=("ota_1", "ota_0", "ota_1")[cycle - 1],
                session=bound_session,
            ),
            updater_baseline=LivePromotionMetricsTest._issued_baseline(
                session=bound_session
            ),
        )


def _pass_facts(gate: str) -> dict:
    snapshot_status = _status(
        reboot_reason=(
            "update_finish" if gate == "chord-rom-recovery" else
            "button_reboot" if gate == "no-host-reboot" else ""
        ),
        version=(
            V078_VERSION
            if gate == "android-control-reconnect"
            else CANARY_VERSION
        ),
    )
    if gate == "android-control-reconnect":
        snapshot_status.pop("last_expected_reboot_generation")
        for scanner in snapshot_status["scanners"]:
            scanner["ver"] = "0.64.75-badge-defcon34"
        snapshot = acceptance.verify_cycle_pre_snapshot(
            snapshot_status,
            _session(),
            VERSION,
            1,
        )
    else:
        snapshot = acceptance.verify_badge_snapshot(
            snapshot_status,
            _session(),
            VERSION,
        )
    shared = {"snapshot": snapshot}
    per_gate = {
        "android-control-reconnect": {
            "candidate_artifacts": _candidate_artifacts(),
            "status_received": True,
            "badge_detection_received": True,
            "theme_changed": True,
            "theme_restored": True,
            "reconnected_after_cable_removal": True,
            "firmware_actions_absent": True,
        },
        "three-update-cycles": {
            "candidate_artifacts": _candidate_artifacts(),
            "cycles_completed": 3,
            "strictly_older_setup": True,
            "automatic_convergence": True,
            "cycles": [
                {
                    "cycle": 1,
                    "recovery_rewrite_same_version": False,
                    "scanner_uploads": 1,
                    "manual_relay_commands": 0,
                    "stage_generation": 21,
                    "slot_mask": 3,
                    "pending_mask_after": 0,
                    "ble_state": "converged",
                    "ble_attempts": 1,
                    "wifi_state": "converged",
                    "wifi_attempts": 1,
                    "ble_manual_relay_verified": False,
                    "wifi_manual_relay_verified": False,
                    "fresh_ping_status": True,
                    "theme_restored": True,
                },
                {
                    "cycle": 2,
                    "recovery_rewrite_same_version": True,
                    "scanner_uploads": 1,
                    "manual_relay_commands": 2,
                    "stage_generation": 22,
                    "slot_mask": 3,
                    "pending_mask_after": 0,
                    "ble_state": "converged",
                    "ble_attempts": 1,
                    "wifi_state": "converged",
                    "wifi_attempts": 1,
                    "ble_manual_relay_verified": True,
                    "wifi_manual_relay_verified": True,
                    "fresh_ping_status": True,
                    "theme_restored": True,
                },
                {
                    "cycle": 3,
                    "recovery_rewrite_same_version": True,
                    "scanner_uploads": 1,
                    "manual_relay_commands": 2,
                    "stage_generation": 23,
                    "slot_mask": 3,
                    "pending_mask_after": 0,
                    "ble_state": "converged",
                    "ble_attempts": 1,
                    "wifi_state": "converged",
                    "wifi_attempts": 1,
                    "ble_manual_relay_verified": True,
                    "wifi_manual_relay_verified": True,
                    "fresh_ping_status": True,
                    "theme_restored": True,
                },
            ],
        },
        "interrupted-upload": {
            "candidate_artifacts": _candidate_artifacts(),
            "baseline_snapshot": shared["snapshot"],
            "recovered_snapshot": shared["snapshot"],
            "abort_after": 65536,
            "idle_wait_s": 7,
            "prior_partition_bootable": True,
            "scanner_cache_unchanged": True,
            "parser_returned_to_command": True,
            "retry_succeeded": True,
            "scanner_cache_before": {
                "stored": True,
                "generation": 7,
                "sha256": "a" * 64,
            },
            "scanner_cache_after_abort": {
                "stored": True,
                "generation": 7,
                "sha256": "a" * 64,
            },
            "scanner_cache_after_retry": {
                "stored": True,
                "generation": 7,
                "sha256": "a" * 64,
            },
        },
        "chord-rom-recovery": {
            "candidate_artifacts": _candidate_artifacts(),
            "usb_data_host_attached": True,
            "hold_ms": 10000,
            "rom_enumerated": True,
            "base_mac_continuity": True,
            "full_layout_verified": True,
            "application_returned": True,
            "scanner_staged_once": True,
            "both_uart_updates": True,
            "last_expected_reboot_reason": "button_usb_rom",
        },
        "no-host-reboot": {
            "usb_data_host_attached": False,
            "hold_ms": 10000,
            "normal_reboot": True,
            "persistent_rom_wait": False,
            "power_only_charger_repeated": True,
            "last_expected_reboot_reason": "button_reboot",
        },
        "power-state-audit": {
            "battery_continuously_connected": True,
            "power_off_entered": False,
            "physical_chord_changed_quiet_mode": False,
            "persistent_safe_mode_or_reboot_loop": False,
        },
    }
    facts = {**shared, **per_gate[gate]}
    if gate == "interrupted-upload":
        baseline_status = _usb_cycle_result(3).final_restored_status
        baseline_status["uptime_s"] = 21
        baseline_status["usb_health"]["rx_bytes"] = 4012
        baseline_status["usb_health"]["valid_commands"] = 61
        baseline_status["usb_health"]["responses_completed"] = 62
        recovered_status = copy.deepcopy(baseline_status)
        recovered_status.update({
            "last_expected_reboot_reason": "update_abort",
            "last_expected_reboot_generation": 9,
            "uptime_s": 5,
        })
        recovered_status["usb_health"]["rx_bytes"] = 120
        recovered_status["usb_health"]["valid_commands"] = 2
        recovered_status["usb_health"]["responses_completed"] = 2
        final_status = _status(
            reboot_reason="update_finish",
            partition="ota_0",
            reboot_generation=12,
            responses_completed=2,
        )
        final_status["uptime_s"] = 5
        final_status["usb_health"]["rx_bytes"] = 120
        final_status["usb_health"]["valid_commands"] = 2
        facts["baseline_snapshot"] = acceptance.verify_badge_snapshot(
            baseline_status, _session(), VERSION
        )
        facts["recovered_snapshot"] = acceptance.verify_badge_snapshot(
            recovered_status, _session(), VERSION
        )
        facts["snapshot"] = acceptance.verify_badge_snapshot(
            final_status, _session(), VERSION
        )
    if gate == "chord-rom-recovery":
        rom_status = _status(
            reboot_reason="button_usb_rom",
            reboot_generation=0,
        )
        facts["rom_boot_snapshot"] = \
            acceptance.verify_chord_rom_boot_snapshot(
                rom_status,
                _session(),
                VERSION,
                baseline=LivePromotionMetricsTest._issued_baseline(),
            )
        return acceptance.VerifiedChordRecoveryFacts(
            acceptance._CHORD_RECOVERY_FACT_ISSUER,
            facts,
        )
    return facts


def _record_complete_evidence(path: Path) -> None:
    acceptance.record_gate(
        path,
        _session(),
        "android-control-reconnect",
        "PASS",
        _pass_facts("android-control-reconnect"),
    )
    for cycle in (1, 2, 3):
        checkpoint = _verify_cycle_fixture(cycle)
        acceptance.record_update_cycle_checkpoint(
            path, _session(), checkpoint
        )
    for gate in acceptance.REQUIRED_GATES[2:4]:
        _append_pass_fixture(path, gate)
    for gate in acceptance.REQUIRED_GATES[4:]:
        acceptance.record_gate(
            path, _session(), gate, "PASS", _pass_facts(gate)
        )


def _read_records(path: Path) -> list[dict]:
    return [
        json.loads(line)
        for line in path.read_text(encoding="utf-8").splitlines()
    ]


def _write_records(path: Path, records: list[dict]) -> None:
    path.write_text(
        "".join(
            json.dumps(record, separators=(",", ":"), sort_keys=True) + "\n"
            for record in records
        ),
        encoding="utf-8",
    )


def _write_session_file(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    path.write_text(json.dumps({
        "session_id": _session().session_id,
        "uplink_hardware_id": UPLINK_ID,
        "ble_hardware_id": BLE_ID,
        "wifi_hardware_id": WIFI_ID,
    }), encoding="utf-8")
    os.chmod(path, 0o600)


def _write_private_session_anchor(root: Path) -> Path:
    anchor = root / "session-anchor"
    anchor.mkdir(mode=0o700)
    path = anchor / "session.json"
    _write_session_file(path)
    return path


def _append_pass_fixture(
    path: Path,
    gate: str,
    *,
    session: acceptance.BadgeAcceptanceSession | None = None,
    facts: dict | None = None,
) -> None:
    """Append a validated PASS without exercising the public manual API."""
    session = _session() if session is None else session
    facts = _pass_facts(gate) if facts is None else facts
    encoded = acceptance._encode_gate_record(
        session, gate, "PASS", facts
    )
    fd = acceptance._open_evidence_for_append(path)
    try:
        acceptance.fcntl.flock(fd, acceptance.fcntl.LOCK_EX)
        acceptance._append_gate_record_locked(
            fd, path, session, encoded
        )
    finally:
        try:
            acceptance.fcntl.flock(fd, acceptance.fcntl.LOCK_UN)
        except OSError:
            pass
        os.close(fd)


def _record_gate_one(path: Path) -> None:
    acceptance.record_gate(
        path,
        _session(),
        "android-control-reconnect",
        "PASS",
        _pass_facts("android-control-reconnect"),
    )


def _record_gate_two(path: Path) -> None:
    _record_gate_one(path)
    for cycle in (1, 2, 3):
        acceptance.record_update_cycle_checkpoint(
            path, _session(), _verify_cycle_fixture(cycle)
        )


def _record_gate_three(path: Path) -> None:
    _record_gate_two(path)
    _append_pass_fixture(path, "interrupted-upload")


def _record_gate_four(path: Path) -> None:
    _record_gate_three(path)
    _append_pass_fixture(path, "chord-rom-recovery")


def _uplink_receipt(
    *,
    total: int,
    phase: str,
    received: int,
    credit_bytes: int,
    reboot_required: bool = False,
) -> dict:
    return {
        "ok": True,
        "phase": phase,
        "partition": "ota_1",
        "received": received,
        "total": total,
        "credit_bytes": credit_bytes,
        "retryable": False,
        "reboot_required": reboot_required,
        "error": "",
    }


class BadgeAcceptanceSessionTest(unittest.TestCase):
    def test_package_import_reuses_the_package_flasher_module(self) -> None:
        completed = subprocess.run(
            [
                sys.executable,
                "-c",
                (
                    "import scripts.fof_badge_flash as flash\n"
                    "import scripts.verify_badge_usb_hardening as acceptance\n"
                    "assert acceptance.flash is flash\n"
                    "assert acceptance.flash.UsbDescriptorRecord is "
                    "flash.UsbDescriptorRecord\n"
                ),
            ],
            cwd=Path(__file__).resolve().parent.parent,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_session_normalizes_exact_unique_hardware_ids(self) -> None:
        session = acceptance.BadgeAcceptanceSession(
            session_id="canary-A",
            uplink_hardware_id=UPLINK_ID,
            ble_hardware_id=BLE_ID.lower(),
            wifi_hardware_id=WIFI_ID,
        )
        self.assertEqual(session.uplink_hardware_id, UPLINK_ID.lower())
        self.assertEqual(session.ble_hardware_id, BLE_ID.lower())
        self.assertEqual(session.wifi_hardware_id, WIFI_ID.lower())

    def test_session_rejects_malformed_or_duplicate_hardware_ids(self) -> None:
        for replacement in ("not-a-mac", UPLINK_ID):
            with self.subTest(replacement=replacement):
                with self.assertRaises(ValueError):
                    acceptance.BadgeAcceptanceSession(
                        session_id="canary-A",
                        uplink_hardware_id=UPLINK_ID,
                        ble_hardware_id=replacement,
                        wifi_hardware_id=WIFI_ID,
                    )


class SnapshotVerificationTest(unittest.TestCase):
    def test_snapshot_accepts_firmware_update_finish_and_abort_reasons(
        self,
    ) -> None:
        for reason in ("update_finish", "update_abort"):
            with self.subTest(reason=reason):
                snapshot = acceptance.verify_badge_snapshot(
                    _status(reboot_reason=reason),
                    _session(),
                    VERSION,
                )
                self.assertEqual(
                    snapshot["last_expected_reboot_reason"],
                    reason,
                )

    def test_snapshot_proves_exact_three_board_identity_and_health(self) -> None:
        result = acceptance.verify_badge_snapshot(
            _status(), _session(), VERSION
        )
        self.assertEqual(result["version"], VERSION)
        self.assertEqual(result["uplink_hardware_id"], UPLINK_ID.lower())
        self.assertEqual(result["ble_hardware_id"], BLE_ID.lower())
        self.assertEqual(result["wifi_hardware_id"], WIFI_ID.lower())
        self.assertEqual(result["running_partition"], "ota_0")
        self.assertEqual(result["usb_parser_state"], "command")
        self.assertEqual(result["usb_responses_completed"], 18)
        self.assertTrue(result["rollback_clear"])
        self.assertEqual(result["ble_role"], "ble_primary")
        self.assertEqual(result["wifi_role"], "wifi_primary")
        self.assertTrue(result["radio_health"])

    def test_snapshot_rejects_board_swap_and_non_command_parser(self) -> None:
        for status in (
            _status(ble_id=WIFI_ID, wifi_id=BLE_ID),
            _status(parser_state="uplink_upload"),
        ):
            with self.subTest(status=status["usb_health"]["parser_state"]):
                with self.assertRaises(acceptance.AcceptanceError):
                    acceptance.verify_badge_snapshot(
                        status, _session(), VERSION
                    )

    def test_snapshot_rejects_missing_health_field_and_boolean_counter(self) -> None:
        missing = _status()
        del missing["usb_health"]["last_response_age_s"]
        boolean_counter = _status()
        boolean_counter["usb_health"]["responses_completed"] = True
        for status in (missing, boolean_counter):
            with self.assertRaises(acceptance.AcceptanceError):
                acceptance.verify_badge_snapshot(
                    status, _session(), VERSION
                )


class CanaryStackBudgetCompileTest(unittest.TestCase):
    @staticmethod
    def _compiled_stack_budgets(*defines: str) -> tuple[int, int]:
        repo_root = Path(__file__).resolve().parent.parent
        include_dir = repo_root / "esp32" / "uplink" / "main" / "core"
        source = (
            '#include <stdio.h>\n'
            '#include "badge_task_stack_budget.h"\n'
            'int main(void) {\n'
            '    printf("%u %u\\n",\n'
            '           (unsigned)BADGE_USB_TASK_STACK_BYTES,\n'
            '           (unsigned)BADGE_UART_RX_TASK_STACK_BYTES);\n'
            '    return 0;\n'
            '}\n'
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            source_path = temp_path / "stack_budget_probe.c"
            binary_path = temp_path / "stack_budget_probe"
            source_path.write_text(source, encoding="utf-8")
            compile_result = subprocess.run(
                [
                    os.environ.get("CC", "cc"),
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{include_dir}",
                    *(f"-D{define}" for define in defines),
                    str(source_path),
                    "-o",
                    str(binary_path),
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            if compile_result.returncode != 0:
                raise AssertionError(compile_result.stderr)
            run_result = subprocess.run(
                [str(binary_path)],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            if run_result.returncode != 0:
                raise AssertionError(run_result.stderr)
            usb_stack, uart_stack = run_result.stdout.strip().split()
            return int(usb_stack), int(uart_stack)

    def test_production_stack_budgets_remain_unchanged(self) -> None:
        self.assertEqual(
            self._compiled_stack_budgets(),
            (16384, 8192),
        )
        self.assertEqual(
            self._compiled_stack_budgets("FOF_BADGE_VARIANT=1"),
            (16384, 8192),
        )
        self.assertEqual(
            self._compiled_stack_budgets("FOF_DC34_GAME_CANARY=1"),
            (16384, 8192),
        )

    def test_canary_only_stack_budgets_gain_headroom(self) -> None:
        self.assertEqual(
            self._compiled_stack_budgets(
                "FOF_BADGE_VARIANT=1",
                "FOF_DC34_GAME_CANARY=1",
            ),
            (20480, 9216),
        )


class LivePromotionMetricsTest(unittest.TestCase):
    @staticmethod
    def _baseline_status(*, responses_completed: int = 21) -> dict:
        status = _status(
            version=V078_VERSION,
            reboot_generation=40,
            responses_completed=responses_completed,
        )
        status.pop("last_expected_reboot_generation")
        status.pop("detection_queue_capacity")
        status["uptime_s"] = 100
        status["usb_health"]["valid_commands"] = responses_completed
        status["usb_health"]["rx_bytes"] = (
            1024 + (responses_completed - 21) * 11
        )
        status.update({
            "stack_main_free": 2048,
            "stack_display_free": 2048,
            "stack_usb_free": 1024,
            "stack_uart_ble_free": 2048,
            "stack_uart_wifi_free": 2048,
            "heap_internal_free": 30000,
            "heap_internal_min_free": 14000,
            "heap_internal_largest": 18000,
        })
        return status

    @staticmethod
    def _normal_status() -> dict:
        status = _status(
            version=CANARY_VERSION,
            reboot_generation=50,
            responses_completed=31,
        )
        status["uptime_s"] = 12
        status["usb_health"]["valid_commands"] = 31
        status.update({
            "heap_internal_free": 24576,
            "heap_internal_min_free": 12288,
            "heap_internal_largest": 16384,
            "detection_queue_capacity": 0,
        })
        return status

    @classmethod
    def _issued_baseline(
        cls,
        *,
        session: acceptance.BadgeAcceptanceSession | None = None,
    ):
        bound_session = _session() if session is None else session
        pre = cls._baseline_status()
        acquired = cls._baseline_status(responses_completed=23)
        acquired["uptime_s"] = 101
        return acceptance.capture_v078_updater_baseline(
            acquired,
            bound_session,
            pre_status=pre,
            challenge_version=V078_VERSION,
        )

    def test_production_v078_absent_generation_challenge_is_explicit(
        self,
    ) -> None:
        pre = self._baseline_status()
        acquired = self._baseline_status(responses_completed=23)
        acquired["uptime_s"] = 101

        baseline = acceptance.capture_v078_updater_baseline(
            acquired,
            _session(),
            pre_status=pre,
            challenge_version=V078_VERSION,
        )

        self.assertEqual(
            baseline["reboot_generation_capability"],
            "legacy-v078-absent",
        )
        self.assertIsNone(baseline["pre_reboot_generation"])
        self.assertIsNone(baseline["reboot_generation"])
        self.assertEqual(baseline["source_partition"], "ota_0")
        self.assertEqual(baseline["pre_uptime_s"], 100)
        self.assertEqual(baseline["uptime_s"], 101)
        self.assertEqual(baseline["pre_valid_commands"], 21)
        self.assertEqual(baseline["valid_commands"], 23)
        self.assertEqual(baseline["pre_rx_bytes"], 1024)
        self.assertEqual(baseline["rx_bytes"], 1046)

        wrong_rx = copy.deepcopy(acquired)
        wrong_rx["usb_health"]["rx_bytes"] = 1045
        with self.assertRaises(acceptance.AcceptanceError):
            acceptance.capture_v078_updater_baseline(
                wrong_rx,
                _session(),
                pre_status=pre,
                challenge_version=V078_VERSION,
            )

    def test_v078_queue_capacity_is_absent_on_wire_and_derived_as_48(
        self,
    ) -> None:
        pre = self._baseline_status()
        acquired = self._baseline_status(responses_completed=23)
        acquired["uptime_s"] = 101
        accepted = acceptance.capture_v078_updater_baseline(
            acquired,
            _session(),
            pre_status=pre,
            challenge_version=V078_VERSION,
        )
        self.assertEqual(accepted["detection_queue_capacity"], 48)

        present_pre = copy.deepcopy(pre)
        present = copy.deepcopy(acquired)
        present_pre["detection_queue_capacity"] = 48
        present["detection_queue_capacity"] = 48
        present_accepted = acceptance.capture_v078_updater_baseline(
            present,
            _session(),
            pre_status=present_pre,
            challenge_version=V078_VERSION,
        )
        self.assertEqual(
            present_accepted["detection_queue_capacity"],
            48,
        )

        for value in (47, 49, True):
            changed_pre = copy.deepcopy(pre)
            changed = copy.deepcopy(acquired)
            changed_pre["detection_queue_capacity"] = value
            changed["detection_queue_capacity"] = value
            with self.subTest(value=value), self.assertRaises(
                acceptance.AcceptanceError
            ):
                acceptance.capture_v078_updater_baseline(
                    changed,
                    _session(),
                    pre_status=changed_pre,
                    challenge_version=V078_VERSION,
                )

    def test_canary_normal_live_requires_clear_rollback_and_not_safe(
        self,
    ) -> None:
        baseline = self._issued_baseline()
        for field, value in (
            ("pending_verify", True),
            ("rollback_state", "pending_verify"),
            ("safe_mode", True),
        ):
            status = self._normal_status()
            status[field] = value
            with self.subTest(field=field), self.assertRaises(
                acceptance.AcceptanceError
            ):
                acceptance.verify_canary_normal_live_metrics(
                    status,
                    _session(),
                    CANARY_VERSION,
                    baseline=baseline,
                )

    def test_v078_challenge_and_third_read_reject_continuity_drift(
        self,
    ) -> None:
        pre = self._baseline_status()
        acquired = self._baseline_status(responses_completed=23)
        acquired["uptime_s"] = 101
        baseline = acceptance.capture_v078_updater_baseline(
            acquired,
            _session(),
            pre_status=pre,
            challenge_version=V078_VERSION,
        )
        live = self._baseline_status(responses_completed=26)
        live["uptime_s"] = 102
        live["usb_health"]["rx_bytes"] = 1080
        sample = acceptance.verify_v078_updater_live_successor(
            live,
            _session(),
            baseline=baseline,
        )
        self.assertIsNone(sample["reboot_generation"])
        self.assertEqual(
            sample["reboot_generation_capability"],
            "legacy-v078-absent",
        )

        for label, mutate in (
            (
                "generation",
                lambda value: value.update({
                    "last_expected_reboot_generation": 1
                }),
            ),
            (
                "partition",
                lambda value: value.update({"running_partition": "ota_1"}),
            ),
            (
                "uptime",
                lambda value: value.update({"uptime_s": 100}),
            ),
            (
                "responses",
                lambda value: value["usb_health"].update({
                    "responses_completed": 23
                }),
            ),
            (
                "commands",
                lambda value: value["usb_health"].update({
                    "valid_commands": 23
                }),
            ),
        ):
            changed = copy.deepcopy(live)
            mutate(changed)
            with self.subTest(label=label), self.assertRaises(
                acceptance.AcceptanceError
            ):
                acceptance.verify_v078_updater_live_successor(
                    changed,
                    _session(),
                    baseline=baseline,
                )

    def test_v078_third_read_requires_exact_probe_delta(self) -> None:
        baseline = self._issued_baseline()
        live = self._baseline_status(responses_completed=26)
        live["uptime_s"] = 102
        live["usb_health"]["rx_bytes"] = 1080

        accepted = acceptance.verify_v078_updater_live_successor(
            live,
            _session(),
            baseline=baseline,
        )
        self.assertEqual(accepted["rx_bytes"], 1080)
        self.assertEqual(accepted["valid_commands"], 26)
        self.assertEqual(accepted["responses_completed"], 26)

        for field, values in (
            ("rx_bytes", (1079, 1081)),
            ("valid_commands", (25, 27)),
            ("responses_completed", (25, 27)),
        ):
            for value in values:
                changed = copy.deepcopy(live)
                changed["usb_health"][field] = value
                with self.subTest(field=field, value=value), \
                        self.assertRaises(acceptance.AcceptanceError):
                    acceptance.verify_v078_updater_live_successor(
                        changed,
                        _session(),
                        baseline=baseline,
                    )

    def test_post_direct_bootstrap_requires_exact_generation_one_lineage(
        self,
    ) -> None:
        pre = self._baseline_status()
        acquired = self._baseline_status(responses_completed=23)
        acquired["uptime_s"] = 101
        baseline = acceptance.capture_v078_updater_baseline(
            acquired,
            _session(),
            pre_status=pre,
            challenge_version=V078_VERSION,
        )
        source = self._baseline_status(responses_completed=26)
        source["uptime_s"] = 102
        source["usb_health"]["rx_bytes"] = 1080
        prior = acceptance.verify_v078_updater_live_successor(
            source,
            _session(),
            baseline=baseline,
        )
        post = self._normal_status()
        post.update({
            "running_partition": "ota_1",
            "last_expected_reboot_reason": "usb_uplink_ota",
            "last_expected_reboot_generation": 1,
        })

        live = acceptance.verify_canary_post_direct_bootstrap_live_metrics(
            post,
            _session(),
            CANARY_VERSION,
            baseline=baseline,
            previous=prior,
        )
        self.assertEqual(live["reboot_generation"], 1)
        self.assertEqual(
            live["reboot_generation_capability"],
            "reported",
        )

        maintenance = _maintenance_status(
            version=CANARY_VERSION,
            update_session="0123456789ABCDEF",
            partition="ota_1",
            reboot_generation=2,
            responses_completed=1,
        )
        maintenance["usb_health"]["valid_commands"] = 1
        accepted = acceptance.verify_canary_maintenance_live_metrics(
            maintenance,
            _session(),
            CANARY_VERSION,
            "0123456789ABCDEF",
            baseline=baseline,
            previous=live,
        )
        self.assertEqual(accepted["reboot_generation"], 2)

        for field, value in (
            ("version", V078_VERSION),
            ("running_partition", "ota_0"),
            ("last_expected_reboot_reason", ""),
            ("last_expected_reboot_generation", 0),
            ("last_expected_reboot_generation", 2),
            ("last_expected_reboot_generation", True),
        ):
            changed = copy.deepcopy(post)
            changed[field] = value
            with self.subTest(field=field, value=value), self.assertRaises(
                acceptance.AcceptanceError
            ):
                acceptance.verify_canary_post_direct_bootstrap_live_metrics(
                    changed,
                    _session(),
                    CANARY_VERSION,
                    baseline=baseline,
                    previous=prior,
                )
        missing = copy.deepcopy(post)
        del missing["last_expected_reboot_generation"]
        with self.assertRaises(acceptance.AcceptanceError):
            acceptance.verify_canary_post_direct_bootstrap_live_metrics(
                missing,
                _session(),
                CANARY_VERSION,
                baseline=baseline,
                previous=prior,
            )

    def test_json_roundtrip_live_sample_rejects_zero_canary_generation(
        self,
    ) -> None:
        serialized = json.loads(json.dumps(
            _issued_maintenance_sample(
                generation=2,
                partition="ota_1",
            )
        ))
        serialized["reboot_generation"] = 0
        restored = acceptance.VerifiedLiveMetricSample(
            acceptance._LIVE_METRIC_ISSUER,
            serialized,
        )

        with self.assertRaises(acceptance.AcceptanceError):
            acceptance._validate_live_sample_binding(
                restored,
                _session(),
                CANARY_VERSION,
            )

    def test_chord_rom_boot_and_first_maintenance_have_strict_lineage(
        self,
    ) -> None:
        baseline = self._issued_baseline()
        rom_status = self._normal_status()
        rom_status.update({
            "running_partition": "ota_1",
            "last_expected_reboot_reason": "button_usb_rom",
            "last_expected_reboot_generation": 0,
        })
        rom_boot = acceptance.verify_chord_rom_boot_snapshot(
            rom_status,
            _session(),
            CANARY_VERSION,
            baseline=baseline,
        )
        self.assertEqual(rom_boot["reboot_generation"], 0)

        first_maintenance = _maintenance_status(
            version=CANARY_VERSION,
            update_session="0123456789ABCDEF",
            partition="ota_1",
            reboot_generation=1,
            responses_completed=1,
        )
        accepted = acceptance.verify_chord_first_maintenance_live_metrics(
            first_maintenance,
            _session(),
            CANARY_VERSION,
            "0123456789ABCDEF",
            baseline=baseline,
            previous=rom_boot,
        )
        self.assertEqual(accepted["reboot_generation"], 1)
        self.assertEqual(accepted["running_partition"], "ota_1")

        for field, value in (
            ("last_expected_reboot_generation", 1),
            ("last_expected_reboot_reason", "update_finish"),
            ("heap_internal_free", 24575),
            ("detection_queue_capacity", 1),
        ):
            changed = copy.deepcopy(rom_status)
            changed[field] = value
            with self.subTest(rom_field=field), self.assertRaises(
                acceptance.AcceptanceError
            ):
                acceptance.verify_chord_rom_boot_snapshot(
                    changed,
                    _session(),
                    CANARY_VERSION,
                    baseline=baseline,
                )

        for field, value in (
            ("last_expected_reboot_generation", 2),
            ("running_partition", "ota_0"),
            ("heap_internal_free", 29999),
        ):
            changed = copy.deepcopy(first_maintenance)
            changed[field] = value
            with self.subTest(maintenance_field=field), self.assertRaises(
                acceptance.AcceptanceError
            ):
                acceptance.verify_chord_first_maintenance_live_metrics(
                    changed,
                    _session(),
                    CANARY_VERSION,
                    "0123456789ABCDEF",
                    baseline=baseline,
                    previous=rom_boot,
                )

    def test_maintenance_live_sample_rejects_prior_update_session_drift(
        self,
    ) -> None:
        baseline = self._issued_baseline()
        previous = _issued_maintenance_sample(
            generation=50,
            partition="ota_0",
        )
        changed_session = _maintenance_status(
            version=CANARY_VERSION,
            update_session="FEDCBA9876543210",
            partition="ota_0",
            reboot_generation=51,
            responses_completed=1,
        )

        with self.assertRaises(acceptance.AcceptanceError):
            acceptance.verify_canary_maintenance_live_metrics(
                changed_session,
                _session(),
                CANARY_VERSION,
                "FEDCBA9876543210",
                baseline=baseline,
                previous=previous,
            )

    def test_normal_after_maintenance_requires_same_partition_next_boot(
        self,
    ) -> None:
        baseline = self._issued_baseline()
        previous = _issued_maintenance_sample(
            generation=50,
            partition="ota_0",
        )
        same_boot = self._normal_status()
        same_boot["usb_health"]["rx_bytes"] = 1025
        wrong_partition = self._normal_status()
        wrong_partition.update({
            "running_partition": "ota_1",
            "last_expected_reboot_generation": 51,
        })

        for label, status in (
            ("same boot", same_boot),
            ("wrong partition", wrong_partition),
        ):
            with self.subTest(label=label), self.assertRaises(
                acceptance.AcceptanceError
            ):
                acceptance.verify_canary_normal_live_metrics(
                    status,
                    _session(),
                    CANARY_VERSION,
                    baseline=baseline,
                    previous=previous,
                )

    def test_first_maintenance_after_post_bootstrap_requires_gen_two(
        self,
    ) -> None:
        baseline = self._issued_baseline()
        source = self._baseline_status(responses_completed=26)
        source["uptime_s"] = 102
        source["usb_health"]["rx_bytes"] = 1080
        prior = acceptance.verify_v078_updater_live_successor(
            source,
            _session(),
            baseline=baseline,
        )
        post = self._normal_status()
        post.update({
            "running_partition": "ota_1",
            "last_expected_reboot_reason": "usb_uplink_ota",
            "last_expected_reboot_generation": 1,
        })
        live = acceptance.verify_canary_post_direct_bootstrap_live_metrics(
            post,
            _session(),
            CANARY_VERSION,
            baseline=baseline,
            previous=prior,
        )

        for generation, responses in ((1, 32), (3, 1)):
            maintenance = _maintenance_status(
                version=CANARY_VERSION,
                update_session="0123456789ABCDEF",
                partition="ota_1",
                reboot_generation=generation,
                responses_completed=responses,
            )
            with self.subTest(generation=generation), self.assertRaises(
                acceptance.AcceptanceError
            ):
                acceptance.verify_canary_maintenance_live_metrics(
                    maintenance,
                    _session(),
                    CANARY_VERSION,
                    "0123456789ABCDEF",
                    baseline=baseline,
                    previous=live,
                )

    def test_maintenance_same_boot_requires_full_counter_continuity(
        self,
    ) -> None:
        baseline = self._issued_baseline()
        source = self._baseline_status(responses_completed=26)
        source["uptime_s"] = 102
        source["usb_health"]["rx_bytes"] = 1080
        prior_v078 = acceptance.verify_v078_updater_live_successor(
            source,
            _session(),
            baseline=baseline,
        )
        post = self._normal_status()
        post.update({
            "running_partition": "ota_1",
            "last_expected_reboot_reason": "usb_uplink_ota",
            "last_expected_reboot_generation": 1,
        })
        normal = acceptance.verify_canary_post_direct_bootstrap_live_metrics(
            post,
            _session(),
            CANARY_VERSION,
            baseline=baseline,
            previous=prior_v078,
        )

        wrong_partition = _maintenance_status(
            version=CANARY_VERSION,
            update_session="0123456789ABCDEF",
            partition="ota_0",
            reboot_generation=2,
            responses_completed=1,
        )
        with self.assertRaises(acceptance.AcceptanceError):
            acceptance.verify_canary_maintenance_live_metrics(
                wrong_partition,
                _session(),
                CANARY_VERSION,
                "0123456789ABCDEF",
                baseline=baseline,
                previous=normal,
            )

        maintenance = _maintenance_status(
            version=CANARY_VERSION,
            update_session="0123456789ABCDEF",
            partition="ota_1",
            reboot_generation=2,
            responses_completed=1,
        )
        maintenance_live = (
            acceptance.verify_canary_maintenance_live_metrics(
                maintenance,
                _session(),
                CANARY_VERSION,
                "0123456789ABCDEF",
                baseline=baseline,
                previous=normal,
            )
        )
        fresh = _maintenance_status(
            version=CANARY_VERSION,
            update_session="0123456789ABCDEF",
            partition="ota_1",
            reboot_generation=2,
            responses_completed=2,
        )
        fresh["uptime_s"] = 6
        fresh["usb_health"]["rx_bytes"] = 1025
        accepted = acceptance.verify_canary_maintenance_live_metrics(
            fresh,
            _session(),
            CANARY_VERSION,
            "0123456789ABCDEF",
            baseline=baseline,
            previous=maintenance_live,
        )
        self.assertEqual(accepted["responses_completed"], 2)

        for label, mutate in (
            (
                "partition",
                lambda value: value.update({"running_partition": "ota_0"}),
            ),
            (
                "uptime",
                lambda value: value.update({"uptime_s": 4}),
            ),
            (
                "rx",
                lambda value: value["usb_health"].update({
                    "rx_bytes": 1024
                }),
            ),
            (
                "commands",
                lambda value: value["usb_health"].update({
                    "valid_commands": 1
                }),
            ),
            (
                "responses",
                lambda value: value["usb_health"].update({
                    "responses_completed": 1
                }),
            ),
        ):
            changed = copy.deepcopy(fresh)
            mutate(changed)
            with self.subTest(label=label), self.assertRaises(
                acceptance.AcceptanceError
            ):
                acceptance.verify_canary_maintenance_live_metrics(
                    changed,
                    _session(),
                    CANARY_VERSION,
                    "0123456789ABCDEF",
                    baseline=baseline,
                    previous=maintenance_live,
                )

    def _proofs(self):
        baseline = self._issued_baseline()
        normal = acceptance.verify_canary_normal_live_metrics(
            self._normal_status(),
            _session(),
            CANARY_VERSION,
            baseline=baseline,
        )
        return baseline, normal

    def test_exact_normal_boundaries_and_v078_stack_floors_pass(self) -> None:
        baseline, normal = self._proofs()

        self.assertEqual(normal["heap_internal_free"], 24576)
        self.assertEqual(normal["heap_internal_largest"], 16384)
        self.assertEqual(normal["heap_internal_min_free"], 12288)
        self.assertEqual(normal["detection_queue_capacity"], 0)
        self.assertEqual(
            baseline["source_version"],
            V078_VERSION,
        )

    def test_normal_gate_rejects_numeric_and_schema_drift(self) -> None:
        baseline = self._issued_baseline()
        mutations = (
            ("heap_internal_free", 24575),
            ("heap_internal_largest", 16383),
            ("heap_internal_min_free", 12287),
            ("detection_queue_capacity", 1),
            ("heap_internal_free", True),
            ("heap_internal_largest", -1),
            ("heap_internal_min_free", 0x1_0000_0000),
            ("stack_usb_free", 1023),
        )
        for field, value in mutations:
            status = self._normal_status()
            status[field] = value
            with self.subTest(field=field, value=value), self.assertRaises(
                acceptance.AcceptanceError
            ):
                acceptance.verify_canary_normal_live_metrics(
                    status,
                    _session(),
                    CANARY_VERSION,
                    baseline=baseline,
                )

    def test_maintenance_requires_minimum_ever_heap_floor(self) -> None:
        baseline, normal = self._proofs()
        status = _maintenance_status(
            version=CANARY_VERSION,
            update_session="0123456789ABCDEF",
            partition="ota_0",
            reboot_generation=51,
            responses_completed=1,
        )
        status["heap_internal_min_free"] = 12287
        with self.assertRaises(acceptance.AcceptanceError):
            acceptance.verify_canary_maintenance_live_metrics(
                status,
                _session(),
                CANARY_VERSION,
                "0123456789ABCDEF",
                baseline=baseline,
                previous=normal,
            )

        for missing in (
            "heap_internal_free",
            "heap_internal_largest",
            "heap_internal_min_free",
            "stack_uart_wifi_free",
            "detection_queue_capacity",
        ):
            status = self._normal_status()
            del status[missing]
            with self.subTest(missing=missing), self.assertRaises(
                acceptance.AcceptanceError
            ):
                acceptance.verify_canary_normal_live_metrics(
                    status,
                    _session(),
                    CANARY_VERSION,
                    baseline=baseline,
                )

    def test_metrics_reject_impossible_heap_relationships(self) -> None:
        baseline = self._issued_baseline()
        for field, value in (
            ("heap_internal_largest", 24577),
            ("heap_internal_min_free", 24577),
            ("heap_internal_free", 600000),
            ("stack_usb_free", 20481),
        ):
            status = self._normal_status()
            status[field] = value
            with self.subTest(field=field), self.assertRaises(
                acceptance.AcceptanceError
            ):
                acceptance.verify_canary_normal_live_metrics(
                    status,
                    _session(),
                    CANARY_VERSION,
                    baseline=baseline,
                )

    def test_canary_stack_maxima_are_truthful_in_normal_and_maintenance(
        self,
    ) -> None:
        baseline = self._issued_baseline()
        normal_status = self._normal_status()
        normal_status.update({
            "stack_usb_free": 20480,
            "stack_uart_ble_free": 9216,
            "stack_uart_wifi_free": 9216,
        })
        normal = acceptance.verify_canary_normal_live_metrics(
            normal_status,
            _session(),
            CANARY_VERSION,
            baseline=baseline,
        )
        self.assertEqual(normal["stack_usb_free"], 20480)
        self.assertEqual(normal["stack_uart_ble_free"], 9216)
        self.assertEqual(normal["stack_uart_wifi_free"], 9216)

        maintenance = _maintenance_status(
            version=CANARY_VERSION,
            update_session="0123456789ABCDEF",
            reboot_generation=51,
            responses_completed=1,
        )
        maintenance.update({
            "stack_usb_free": 20480,
            "stack_uart_ble_free": 9216,
            "stack_uart_wifi_free": 9216,
        })
        accepted = acceptance.verify_canary_maintenance_live_metrics(
            maintenance,
            _session(),
            CANARY_VERSION,
            "0123456789ABCDEF",
            baseline=baseline,
            previous=normal,
        )
        self.assertEqual(accepted["stack_usb_free"], 20480)
        self.assertEqual(accepted["stack_uart_ble_free"], 9216)
        self.assertEqual(accepted["stack_uart_wifi_free"], 9216)

    def test_stack_metric_ranges_reject_impossible_values_by_version(
        self,
    ) -> None:
        baseline = self._issued_baseline()
        for field, value in (
            ("stack_usb_free", 20481),
            ("stack_uart_ble_free", 9217),
            ("stack_uart_wifi_free", 9217),
        ):
            status = self._normal_status()
            status[field] = value
            with self.subTest(
                lineage="canary",
                field=field,
            ), self.assertRaises(acceptance.AcceptanceError):
                acceptance.verify_canary_normal_live_metrics(
                    status,
                    _session(),
                    CANARY_VERSION,
                    baseline=baseline,
                )

        for field, value in (
            ("stack_usb_free", 16385),
            ("stack_uart_ble_free", 8193),
            ("stack_uart_wifi_free", 8193),
        ):
            pre = self._baseline_status()
            acquired = self._baseline_status(responses_completed=23)
            acquired["uptime_s"] = 101
            pre[field] = value
            acquired[field] = value
            with self.subTest(
                lineage="v078",
                field=field,
            ), self.assertRaises(acceptance.AcceptanceError):
                acceptance.capture_v078_updater_baseline(
                    acquired,
                    _session(),
                    pre_status=pre,
                    challenge_version=V078_VERSION,
                )

    def test_baseline_capture_rejects_wrong_identity_version_and_mode(
        self,
    ) -> None:
        for mutate in (
            lambda value: value.update({"version": CANARY_VERSION}),
            lambda value: value.update({"hardware_id": BLE_ID}),
            lambda value: value.update({
                "recovery_mode": "update_maintenance",
                "update_session": "0123456789ABCDEF",
            }),
            lambda value: value["usb_health"].update(
                {"responses_completed": False}
            ),
        ):
            status = self._baseline_status()
            mutate(status)
            with self.assertRaises(acceptance.AcceptanceError):
                acceptance.capture_v078_updater_baseline(
                    status,
                    _session(),
                    pre_status=self._baseline_status(),
                    challenge_version=V078_VERSION,
                )

    def test_baseline_capture_requires_two_fresh_same_boot_live_statuses(
        self,
    ) -> None:
        pre = self._baseline_status()
        acquired = self._baseline_status(responses_completed=23)
        acquired["uptime_s"] = 101

        baseline = acceptance.capture_v078_updater_baseline(
            acquired,
            _session(),
            pre_status=pre,
            challenge_version=V078_VERSION,
        )
        self.assertEqual(baseline["pre_responses_completed"], 21)
        self.assertEqual(baseline["responses_completed"], 23)
        self.assertIsNone(baseline["pre_reboot_generation"])
        self.assertIsNone(baseline["reboot_generation"])

        for mutate in (
            lambda status: status["usb_health"].update(
                {"responses_completed": 22}
            ),
            lambda status: status["usb_health"].update(
                {"valid_commands": 22}
            ),
            lambda status: status["usb_health"].update(
                {"rx_bytes": 1045}
            ),
            lambda status: status.update(
                {"uptime_s": 99}
            ),
            lambda status: status.update(
                {"last_expected_reboot_generation": 1}
            ),
        ):
            stale = self._baseline_status(responses_completed=23)
            stale["uptime_s"] = 101
            mutate(stale)
            with self.subTest(stale=stale), self.assertRaises(
                acceptance.AcceptanceError
            ):
                acceptance.capture_v078_updater_baseline(
                    stale,
                    _session(),
                    pre_status=pre,
                    challenge_version=V078_VERSION,
                )

    def test_raw_or_replayed_baseline_cannot_authorize_another_session(
        self,
    ) -> None:
        baseline = self._issued_baseline()
        other = acceptance.BadgeAcceptanceSession(
            session_id="other-session",
            uplink_hardware_id=UPLINK_ID,
            ble_hardware_id=BLE_ID,
            wifi_hardware_id=WIFI_ID,
        )
        with self.assertRaises(acceptance.AcceptanceError):
            acceptance.verify_canary_normal_live_metrics(
                self._normal_status(),
                _session(),
                CANARY_VERSION,
                baseline=dict(baseline),
            )
        with self.assertRaises(acceptance.AcceptanceError):
            acceptance.verify_canary_normal_live_metrics(
                self._normal_status(),
                other,
                CANARY_VERSION,
                baseline=baseline,
            )

    def test_cycle_one_canary_prestate_requires_exact_live_v078_uplink(
        self,
    ) -> None:
        source = self._baseline_status()
        for scanner in source["scanners"]:
            scanner["ver"] = V078_VERSION
        proof = acceptance.verify_cycle_pre_snapshot(
            source,
            _session(),
            CANARY_VERSION,
            1,
        )
        self.assertEqual(proof["uplink_version"], V078_VERSION)

        already_canary = copy.deepcopy(source)
        already_canary["version"] = CANARY_VERSION
        with self.assertRaisesRegex(
            acceptance.AcceptanceError,
            "return.*exact.*\\.78|rollback.*\\.78",
        ):
            acceptance.verify_cycle_pre_snapshot(
                already_canary,
                _session(),
                CANARY_VERSION,
                1,
            )

    def test_reserved_live_baseline_capture_is_fresh_and_precedes_action(
        self,
    ) -> None:
        events: list[str] = []
        statuses = [
            self._baseline_status(),
            self._baseline_status(responses_completed=23),
        ]
        statuses[1]["uptime_s"] = 101

        class LiveBadge:
            def __init__(inner_self, *_args, **_kwargs) -> None:
                pass

            def __enter__(inner_self):
                return inner_self

            def __exit__(inner_self, *_args) -> None:
                return None

            def status(inner_self, *, timeout_s: int) -> dict:
                self.assertEqual(timeout_s, 5)
                value = copy.deepcopy(statuses.pop(0))
                events.append(
                    f"status-{value['usb_health']['responses_completed']}"
                )
                return value

            def write_line(inner_self, line: str) -> None:
                self.assertEqual(line, "FOF_PING")
                events.append("ping")

            def read_prefixed_text(
                inner_self,
                prefix: str,
                timeout_s: int,
            ) -> str:
                self.assertEqual(prefix, "FOF_PONG:")
                self.assertEqual(timeout_s, 5)
                events.append("pong")
                return V078_VERSION

        def validate_reservation(_reservation) -> None:
            events.append("reservation")

        with mock.patch.object(
            acceptance,
            "_validate_live_mutating_gate_reservation",
            side_effect=validate_reservation,
        ), mock.patch.object(
            acceptance,
            "_trusted_session_uplink_descriptor",
            return_value=_usb_record("/dev/v078-live"),
        ), mock.patch.object(
            acceptance.flash,
            "BadgeSerial",
            LiveBadge,
        ):
            baseline = acceptance._capture_reserved_v078_updater_baseline(
                "/dev/v078-live",
                _session(),
                mock.sentinel.reservation,
            )
            events.append("canary-mutation")

        self.assertEqual(
            events,
            [
                "reservation",
                "status-21",
                "ping",
                "pong",
                "status-23",
                "reservation",
                "canary-mutation",
            ],
        )
        self.assertEqual(baseline["source_version"], V078_VERSION)

    def test_cycle_one_checkpoint_retains_live_v078_to_v079_lineage(
        self,
    ) -> None:
        pre = self._baseline_status()
        acquired = self._baseline_status(responses_completed=23)
        acquired["uptime_s"] = 101
        baseline = acceptance.capture_v078_updater_baseline(
            acquired,
            _session(),
            pre_status=pre,
            challenge_version=V078_VERSION,
        )
        frozen = acceptance.flash.FrozenUsbFirmwareArtifacts(
            uplink=_frozen_artifacts(
                _uplink_firmware_image(version=CANARY_VERSION)
            ),
            scanner=_frozen_artifacts(
                _scanner_firmware_image(version=CANARY_VERSION)
            ),
        )
        artifacts = acceptance.verify_candidate_artifacts(
            frozen, CANARY_VERSION
        )
        source = self._baseline_status(responses_completed=26)
        source["uptime_s"] = 102
        source["usb_health"]["rx_bytes"] = 1080
        for scanner in source["scanners"]:
            scanner["ver"] = V078_VERSION
        source_pre_snapshot = acceptance.verify_cycle_pre_snapshot(
            source,
            _session(),
            CANARY_VERSION,
            1,
        )
        before = self._normal_status()
        before["running_partition"] = "ota_1"
        before["last_expected_reboot_generation"] = 1
        for scanner in before["scanners"]:
            scanner["ver"] = V078_VERSION
        restored = self._normal_status()
        restored["running_partition"] = "ota_1"
        restored["last_expected_reboot_generation"] = 3
        restored["last_expected_reboot_reason"] = "update_finish"
        restored["usb_health"]["rx_bytes"] = 1100
        restored["usb_health"]["valid_commands"] = 40
        restored["usb_health"]["responses_completed"] = 40
        receipt = _scanner_stage_receipt(21)
        receipt["version"] = CANARY_VERSION
        scanner_data = acceptance.flash._frozen_firmware_bytes(
            frozen.scanner,
            role="scanner",
        )
        receipt.update({
            "size": len(scanner_data),
            "received": len(scanner_data),
            "total": len(scanner_data),
            "crc32": acceptance.binascii.crc32(scanner_data) & 0xFFFFFFFF,
            "sha256": hashlib.sha256(scanner_data).hexdigest(),
        })
        store = copy.deepcopy(
            _usb_cycle_result(1).final_restored_status["firmware_store"]
        )
        for key in (
            "target",
            "app_project",
            "hardware_type",
            "version",
            "size",
            "crc32",
            "sha256",
            "generation",
        ):
            store[key] = receipt[key]
        restored["firmware_store"] = store
        result = _UsbCycleResultFixture(
            pre_stage_status=before,
            final_restored_status=restored,
            stage_receipt=receipt,
            preflight_older_slots=frozenset({"ble", "wifi"}),
            recovery_slots=frozenset(),
        )

        with mock.patch.object(
            acceptance.flash,
            "_revalidate_usb_scanner_flow_result",
            return_value=result,
        ):
            checkpoint = acceptance.verify_update_cycle_result(
                result,
                _session(),
                CANARY_VERSION,
                1,
                candidate_artifacts=artifacts,
                source_pre_snapshot=source_pre_snapshot,
                final_maintenance_sample=_issued_maintenance_sample(
                    generation=2,
                    partition="ota_1",
                ),
                updater_baseline=baseline,
            )

        self.assertEqual(checkpoint["updater_baseline"], baseline)
        self.assertEqual(
            checkpoint["pre_snapshot"]["uplink_version"],
            V078_VERSION,
        )
        self.assertEqual(checkpoint["snapshot"]["version"], CANARY_VERSION)
        restored_checkpoint = acceptance._checkpoint_from_serialized(
            json.loads(json.dumps(checkpoint))
        )
        self.assertIs(
            type(restored_checkpoint["updater_baseline"]),
            acceptance.VerifiedV078UpdaterBaseline,
        )
        self.assertEqual(
            restored_checkpoint["updater_baseline"],
            baseline,
        )
        for field, value in (
            ("reboot_generation", 0),
            ("pre_reboot_generation", 0),
            ("reboot_generation_capability", "reported"),
            ("source_partition", "ota_1"),
            ("rx_bytes", baseline["rx_bytes"] - 1),
        ):
            tampered = json.loads(json.dumps(checkpoint))
            tampered["updater_baseline"][field] = value
            with self.subTest(field=field), self.assertRaises(
                acceptance.AcceptanceError
            ):
                restored_tampered = acceptance._checkpoint_from_serialized(
                    tampered
                )
                acceptance._validate_cycle_checkpoint(
                    _session(),
                    restored_tampered,
                )

    def test_maintenance_gate_binds_baseline_and_exact_successor(self) -> None:
        baseline, normal = self._proofs()
        maintenance = _maintenance_status(
            version=CANARY_VERSION,
            update_session="0123456789ABCDEF",
            reboot_generation=51,
            responses_completed=1,
        )
        maintenance.update({
            "heap_internal_free": 30000,
            "heap_internal_largest": 18000,
            "heap_internal_min_free": 12288,
        })

        result = acceptance.verify_canary_maintenance_live_metrics(
            maintenance,
            _session(),
            CANARY_VERSION,
            "0123456789ABCDEF",
            baseline=baseline,
            previous=normal,
        )
        self.assertEqual(result["reboot_generation"], 51)

        for field, value in (
            ("heap_internal_free", 29999),
            ("heap_internal_largest", 17999),
            ("last_expected_reboot_generation", 50),
            ("last_expected_reboot_generation", 52),
            ("last_expected_reboot_generation", False),
            ("update_session", "FEDCBA9876543210"),
            ("recovery_mode", "normal"),
            ("version", V078_VERSION),
            ("hardware_id", BLE_ID),
        ):
            changed = copy.deepcopy(maintenance)
            changed[field] = value
            with self.subTest(field=field, value=value), self.assertRaises(
                acceptance.AcceptanceError
            ):
                acceptance.verify_canary_maintenance_live_metrics(
                    changed,
                    _session(),
                    CANARY_VERSION,
                    "0123456789ABCDEF",
                    baseline=baseline,
                    previous=normal,
                )

    def test_reboot_successor_wraps_and_skips_reserved_magic(self) -> None:
        self.assertEqual(
            acceptance._expected_reboot_successor(0xFFFFFFFF),
            1,
        )
        self.assertEqual(
            acceptance._expected_reboot_successor(0xF0F0B006),
            0xF0F0B008,
        )

    def test_live_samples_require_same_boot_progress_or_exact_next_boot(
        self,
    ) -> None:
        baseline, normal = self._proofs()
        same_boot = self._normal_status()
        same_boot["uptime_s"] = 13
        same_boot["usb_health"]["rx_bytes"] = 1025
        same_boot["usb_health"]["valid_commands"] = 32
        same_boot["usb_health"]["responses_completed"] = 32
        advanced = acceptance.verify_canary_normal_live_metrics(
            same_boot,
            _session(),
            CANARY_VERSION,
            baseline=baseline,
            previous=normal,
        )
        self.assertEqual(advanced["responses_completed"], 32)

        for generation, responses in ((50, 31), (49, 99), (52, 1)):
            stale = self._normal_status()
            stale["last_expected_reboot_generation"] = generation
            stale["usb_health"]["responses_completed"] = responses
            with self.subTest(
                generation=generation,
                responses=responses,
            ), self.assertRaises(acceptance.AcceptanceError):
                acceptance.verify_canary_normal_live_metrics(
                    stale,
                    _session(),
                    CANARY_VERSION,
                    baseline=baseline,
                    previous=normal,
                )


class CandidateArtifactIdentityTest(unittest.TestCase):
    @staticmethod
    def _with_receipt(
        artifacts: artifact_tree.FrozenArtifactSet,
        receipt_sha256: str,
    ) -> artifact_tree.FrozenArtifactSet:
        return artifact_tree.FrozenArtifactSet(
            receipt_sha256=receipt_sha256,
            members=artifacts.members,
            aggregate_sha256=artifact_tree._aggregate_sha256(
                receipt_sha256,
                artifacts.members,
            ),
        )

    @staticmethod
    def _with_extra_uplink_member(
        frozen: acceptance.flash.FrozenUsbFirmwareArtifacts,
        *,
        logical_name: str,
        content: bytes,
    ) -> acceptance.flash.FrozenUsbFirmwareArtifacts:
        member = artifact_tree.FrozenArtifactMember(
            logical_name=logical_name,
            size=len(content),
            sha256=hashlib.sha256(content).hexdigest(),
            content=content,
        )
        members = tuple(sorted(
            (*frozen.uplink.members, member),
            key=lambda value: value.logical_name,
        ))
        receipt_sha256 = frozen.uplink.receipt_sha256
        uplink = artifact_tree.FrozenArtifactSet(
            receipt_sha256=receipt_sha256,
            members=members,
            aggregate_sha256=artifact_tree._aggregate_sha256(
                receipt_sha256,
                members,
            ),
        )
        return acceptance.flash.FrozenUsbFirmwareArtifacts(
            uplink=uplink,
            scanner=frozen.scanner,
        )

    def test_candidate_identity_is_stable_across_private_freeze_receipts(
        self,
    ) -> None:
        first = _frozen_candidate_artifacts()
        second = acceptance.flash.FrozenUsbFirmwareArtifacts(
            uplink=self._with_receipt(first.uplink, "1" * 64),
            scanner=self._with_receipt(first.scanner, "2" * 64),
        )
        self.assertNotEqual(
            first.uplink.aggregate_sha256,
            second.uplink.aggregate_sha256,
        )
        self.assertNotEqual(
            first.scanner.aggregate_sha256,
            second.scanner.aggregate_sha256,
        )

        first_identity = acceptance.verify_candidate_artifacts(
            first,
            CANARY_VERSION,
        )
        second_identity = acceptance.verify_candidate_artifacts(
            second,
            CANARY_VERSION,
        )

        self.assertEqual(first_identity, second_identity)
        self.assertEqual(first_identity["schema"], 3)
        for role in ("uplink", "scanner"):
            self.assertIn(
                "content_set_sha256",
                first_identity[role],
            )
            self.assertNotIn("aggregate_sha256", first_identity[role])
        first_operation = acceptance._operation_identity_sha256(
            _session(),
            CANARY_VERSION,
            first_identity,
            "three-update-cycles",
            "update_cycle_1",
            1,
        )
        second_operation = acceptance._operation_identity_sha256(
            _session(),
            CANARY_VERSION,
            second_identity,
            "three-update-cycles",
            "update_cycle_1",
            1,
        )
        self.assertEqual(first_operation, second_operation)

        serialized = json.loads(json.dumps(first_identity))
        restored = acceptance._candidate_artifacts_from_serialized(
            serialized,
            "stable candidate identity",
        )
        self.assertEqual(restored, first_identity)

        tampered = copy.deepcopy(serialized)
        original_digest = tampered["scanner"]["content_set_sha256"]
        tampered["scanner"]["content_set_sha256"] = (
            "f" * 64 if original_digest != "f" * 64 else "e" * 64
        )
        restored_tampered = \
            acceptance._candidate_artifacts_from_serialized(
                tampered,
                "tampered stable candidate identity",
            )
        self.assertNotEqual(restored_tampered, first_identity)
        self.assertNotEqual(
            acceptance._operation_identity_sha256(
                _session(),
                CANARY_VERSION,
                restored_tampered,
                "three-update-cycles",
                "update_cycle_1",
                1,
            ),
            first_operation,
        )

        schema_two = copy.deepcopy(serialized)
        schema_two["schema"] = 2
        with self.assertRaises(acceptance.AcceptanceError):
            acceptance._candidate_artifacts_from_serialized(
                schema_two,
                "schema-2 candidate identity",
            )
        receipt_sensitive = copy.deepcopy(serialized)
        for role in ("uplink", "scanner"):
            receipt_sensitive[role]["aggregate_sha256"] = \
                receipt_sensitive[role].pop("content_set_sha256")
        with self.assertRaises(acceptance.AcceptanceError):
            acceptance._candidate_artifacts_from_serialized(
                receipt_sensitive,
                "receipt-sensitive candidate identity",
            )

    def test_candidate_content_identity_changes_for_every_member_drift(
        self,
    ) -> None:
        frozen = _frozen_candidate_artifacts()
        variants = (
            self._with_extra_uplink_member(
                frozen,
                logical_name="manifest.alpha",
                content=b"stable-manifest",
            ),
            self._with_extra_uplink_member(
                frozen,
                logical_name="manifest.alpha",
                content=b"drift!-manifest",
            ),
            self._with_extra_uplink_member(
                frozen,
                logical_name="manifest.beta",
                content=b"stable-manifest",
            ),
            self._with_extra_uplink_member(
                frozen,
                logical_name="manifest.alpha",
                content=b"stable-manifest-longer",
            ),
        )
        identities = [
            acceptance.verify_candidate_artifacts(
                variant,
                CANARY_VERSION,
            )["uplink"]["content_set_sha256"]
            for variant in variants
        ]
        self.assertEqual(len(set(identities)), len(variants))

        invalid_hash = _frozen_candidate_artifacts()
        object.__setattr__(
            invalid_hash.uplink.members[0],
            "sha256",
            "f" * 64,
        )
        with self.assertRaises(acceptance.AcceptanceError):
            acceptance.verify_candidate_artifacts(
                invalid_hash,
                CANARY_VERSION,
            )

    def test_content_set_digest_binds_platform_version_role_and_triples(
        self,
    ) -> None:
        artifacts = _frozen_candidate_artifacts().uplink

        def expected(
            platform_key: str,
            version: str,
            role: str,
        ) -> str:
            digest = hashlib.sha256()
            digest.update(
                b"friend-or-foe/badge-acceptance/"
                b"candidate-content-set/v1\x00"
            )
            for value in (platform_key, version, role):
                encoded = value.encode("utf-8")
                digest.update(len(encoded).to_bytes(4, "big"))
                digest.update(encoded)
            members = tuple(sorted(
                artifacts.members,
                key=lambda member: member.logical_name,
            ))
            digest.update(len(members).to_bytes(4, "big"))
            for member in members:
                logical_name = member.logical_name.encode("utf-8")
                digest.update(len(logical_name).to_bytes(4, "big"))
                digest.update(logical_name)
                digest.update(member.size.to_bytes(8, "big"))
                digest.update(bytes.fromhex(member.sha256))
            return digest.hexdigest()

        inputs = (
            (CANARY_PLATFORM_KEY, CANARY_VERSION, "uplink"),
            ("badge-trio-xiao-s3", CANARY_VERSION, "uplink"),
            (CANARY_PLATFORM_KEY, LEGACY_VERSION, "uplink"),
            (CANARY_PLATFORM_KEY, CANARY_VERSION, "scanner"),
        )
        observed = [
            acceptance._candidate_content_set_sha256(
                artifacts,
                platform_key=platform_key,
                version=version,
                role=role,
            )
            for platform_key, version, role in inputs
        ]
        self.assertEqual(observed[0], expected(*inputs[0]))
        self.assertEqual(len(set(observed)), len(inputs))

    def test_canary_identity_binds_platform_and_rejects_production_poison(
        self,
    ) -> None:
        frozen = acceptance.flash.FrozenUsbFirmwareArtifacts(
            uplink=_frozen_artifacts(
                _uplink_firmware_image(version=CANARY_VERSION)
            ),
            scanner=_frozen_artifacts(
                _scanner_firmware_image(version=CANARY_VERSION)
            ),
        )
        identity = acceptance.verify_candidate_artifacts(
            frozen,
            CANARY_VERSION,
        )
        self.assertEqual(identity["platform_key"], CANARY_PLATFORM_KEY)

        poisoned = json.loads(json.dumps(identity))
        poisoned["platform_key"] = "badge-trio-xiao-s3"
        with self.assertRaises(acceptance.AcceptanceError):
            acceptance._candidate_artifacts_from_serialized(
                poisoned,
                "poisoned production candidate",
            )

        production = acceptance.flash.FrozenUsbFirmwareArtifacts(
            uplink=_frozen_artifacts(
                _uplink_firmware_image(version=LEGACY_VERSION)
            ),
            scanner=_frozen_artifacts(
                _scanner_firmware_image(version=LEGACY_VERSION)
            ),
        )
        with self.assertRaises(acceptance.AcceptanceError):
            acceptance.verify_candidate_artifacts(
                production,
                LEGACY_VERSION,
            )

    def test_operation_digest_uses_fixed_canary_platform(self) -> None:
        frozen = acceptance.flash.FrozenUsbFirmwareArtifacts(
            uplink=_frozen_artifacts(
                _uplink_firmware_image(version=CANARY_VERSION)
            ),
            scanner=_frozen_artifacts(
                _scanner_firmware_image(version=CANARY_VERSION)
            ),
        )
        artifacts = acceptance.verify_candidate_artifacts(
            frozen,
            CANARY_VERSION,
        )
        actual = acceptance._operation_identity_sha256(
            _session(),
            CANARY_VERSION,
            artifacts,
            "three-update-cycles",
            "update_cycle_1",
            1,
        )

        def digest(platform_key: str) -> str:
            platform = acceptance.flash.PLATFORMS[platform_key]
            operation = {
                "artifacts": {
                    "platform": platform_key,
                    "scanner_target": platform["scanner_name"],
                    "uplink_target": platform["uplink_name"],
                    "candidate": artifacts,
                },
                "cycle": 1,
                "firmware_version": CANARY_VERSION,
                "gate": "three-update-cycles",
                "phase": "update_cycle_1",
                "schema": 2,
                "session": {
                    "ble_hardware_id": BLE_ID,
                    "session_id": _session().session_id,
                    "uplink_hardware_id": UPLINK_ID,
                    "wifi_hardware_id": WIFI_ID,
                },
            }
            return hashlib.sha256(
                b"friend-or-foe/badge-acceptance/"
                b"retained-operation/v2\x00"
                + json.dumps(
                    operation,
                    separators=(",", ":"),
                    sort_keys=True,
                ).encode("utf-8")
            ).hexdigest()

        self.assertEqual(actual, digest(CANARY_PLATFORM_KEY))
        self.assertNotEqual(actual, digest("badge-trio-xiao-s3"))

    def test_identity_is_issued_from_exact_validated_frozen_bytes(self) -> None:
        frozen = _frozen_candidate_artifacts()
        identity = acceptance.verify_candidate_artifacts(
            frozen,
            VERSION,
        )

        self.assertIs(
            type(identity),
            acceptance.VerifiedCandidateArtifacts,
        )
        self.assertEqual(identity["schema"], 3)
        self.assertEqual(identity["platform_key"], CANARY_PLATFORM_KEY)
        self.assertEqual(identity["version"], VERSION)
        self.assertEqual(
            identity["uplink"]["content_set_sha256"],
            acceptance._candidate_content_set_sha256(
                frozen.uplink,
                platform_key=CANARY_PLATFORM_KEY,
                version=CANARY_VERSION,
                role="uplink",
            ),
        )
        self.assertEqual(
            identity["scanner"]["content_set_sha256"],
            acceptance._candidate_content_set_sha256(
                frozen.scanner,
                platform_key=CANARY_PLATFORM_KEY,
                version=CANARY_VERSION,
                role="scanner",
            ),
        )
        for role, image in (
            ("uplink", _uplink_firmware_image()),
            ("scanner", _scanner_firmware_image()),
        ):
            self.assertEqual(identity[role]["firmware_size"], len(image))
            self.assertEqual(
                identity[role]["firmware_crc32"],
                acceptance.binascii.crc32(image) & 0xFFFFFFFF,
            )
            self.assertEqual(
                identity[role]["firmware_sha256"],
                hashlib.sha256(image).hexdigest(),
            )
        with self.assertRaises(TypeError):
            identity["version"] = "changed"

    def test_gate_one_rejects_missing_or_caller_authored_identity(self) -> None:
        for replacement in (None, dict(_candidate_artifacts())):
            with self.subTest(replacement=replacement):
                facts = _pass_facts("android-control-reconnect")
                if replacement is None:
                    facts.pop("candidate_artifacts")
                else:
                    facts["candidate_artifacts"] = replacement
                with tempfile.TemporaryDirectory() as td, \
                        self.assertRaises(acceptance.AcceptanceError):
                    acceptance.record_gate(
                        Path(td) / "acceptance.jsonl",
                        _session(),
                        "android-control-reconnect",
                        "PASS",
                        facts,
                    )


class CyclePreSnapshotTest(unittest.TestCase):
    def test_cycle_one_requires_both_exact_boards_strictly_older(self) -> None:
        status = _status(version=V078_VERSION)
        status.pop("last_expected_reboot_generation")
        for scanner in status["scanners"]:
            scanner["ver"] = "0.64.75-badge-defcon34"
        proof = acceptance.verify_cycle_pre_snapshot(
            status, _session(), VERSION, 1
        )
        self.assertEqual(proof["ble_version"], "0.64.75-badge-defcon34")
        self.assertEqual(proof["wifi_version"], "0.64.75-badge-defcon34")
        self.assertEqual(proof["usb_parser_state"], "command")

        status["scanners"][0]["ver"] = VERSION
        with self.assertRaisesRegex(
            acceptance.AcceptanceError, "strictly older"
        ):
            acceptance.verify_cycle_pre_snapshot(
                status, _session(), VERSION, 1
            )

    def test_recovery_cycle_requires_both_scanners_exactly_current(
        self,
    ) -> None:
        proof = acceptance.verify_cycle_pre_snapshot(
            _status(), _session(), VERSION, 2
        )
        self.assertEqual(proof["ble_version"], VERSION)
        older = _status()
        older["scanners"][1]["ver"] = "0.64.75-badge-defcon34"
        with self.assertRaisesRegex(
            acceptance.AcceptanceError, "exactly current"
        ):
            acceptance.verify_cycle_pre_snapshot(
                older, _session(), VERSION, 2
            )

    def test_cycle_pre_snapshot_rejects_slot_swap(self) -> None:
        swapped = _status(ble_id=WIFI_ID, wifi_id=BLE_ID)
        with self.assertRaises(acceptance.AcceptanceError):
            acceptance.verify_cycle_pre_snapshot(
                swapped, _session(), VERSION, 2
            )


class UpdateCycleCheckpointTest(unittest.TestCase):
    def test_json_roundtrip_cycle_final_requires_update_finish_reason(
        self,
    ) -> None:
        serialized = json.loads(json.dumps(_verify_cycle_fixture(2)))
        serialized["snapshot"][
            "last_expected_reboot_reason"
        ] = "usb_uplink_ota"
        restored = acceptance._checkpoint_from_serialized(serialized)

        with self.assertRaises(acceptance.AcceptanceError):
            acceptance._validate_cycle_checkpoint(_session(), restored)

    def test_json_roundtrip_cycle_one_rechecks_exact_v078_successor(
        self,
    ) -> None:
        checkpoint = _verify_cycle_fixture(1)
        mutations = (
            ("uptime_s", 100),
            ("usb_rx_bytes", 1079),
            ("usb_rx_bytes", 1081),
            ("usb_valid_commands", 25),
            ("usb_valid_commands", 27),
            ("usb_responses_completed", 25),
            ("usb_responses_completed", 27),
        )
        for field, value in mutations:
            serialized = json.loads(json.dumps(checkpoint))
            serialized["pre_snapshot"][field] = value
            restored = acceptance._checkpoint_from_serialized(serialized)
            with self.subTest(field=field, value=value), self.assertRaises(
                acceptance.AcceptanceError
            ):
                acceptance._validate_cycle_checkpoint(
                    _session(),
                    restored,
                )

    def test_json_roundtrip_pre_snapshot_rejects_zero_canary_generation(
        self,
    ) -> None:
        serialized = json.loads(json.dumps(_verify_cycle_fixture(2)))
        serialized["pre_snapshot"]["reboot_generation"] = 0
        restored = acceptance._checkpoint_from_serialized(serialized)

        with self.assertRaises(acceptance.AcceptanceError):
            acceptance._validate_cycle_checkpoint(_session(), restored)

    def test_json_roundtrip_post_snapshot_rejects_zero_canary_generation(
        self,
    ) -> None:
        serialized = json.loads(json.dumps(_verify_cycle_fixture(2)))
        serialized["snapshot"]["reboot_generation"] = 0
        restored = acceptance._checkpoint_from_serialized(serialized)

        with self.assertRaises(acceptance.AcceptanceError):
            acceptance._validate_cycle_checkpoint(_session(), restored)

    def test_cross_reboot_counter_reset_keeps_exact_cycle_lineage(self) -> None:
        source = _cycle_source_status(2)
        source["usb_health"]["rx_bytes"] = 50000
        source["usb_health"]["valid_commands"] = 500
        source["usb_health"]["responses_completed"] = 500
        source_snapshot = acceptance.verify_cycle_pre_snapshot(
            source,
            _session(),
            CANARY_VERSION,
            2,
        )
        fixture = _usb_cycle_result(2)
        fixture.final_restored_status["usb_health"]["rx_bytes"] = 120
        fixture.final_restored_status["usb_health"]["valid_commands"] = 2
        fixture.final_restored_status[
            "usb_health"
        ]["responses_completed"] = 2

        with mock.patch.object(
            acceptance.flash,
            "_revalidate_usb_scanner_flow_result",
            return_value=fixture,
        ):
            checkpoint = acceptance.verify_update_cycle_result(
                fixture,
                _session(),
                CANARY_VERSION,
                2,
                candidate_artifacts=_candidate_artifacts(),
                updater_baseline=LivePromotionMetricsTest._issued_baseline(),
                source_pre_snapshot=source_snapshot,
                final_maintenance_sample=_issued_maintenance_sample(
                    generation=4,
                    partition="ota_0",
                ),
            )

        restored = acceptance._checkpoint_from_serialized(
            json.loads(json.dumps(checkpoint))
        )
        acceptance._validate_cycle_checkpoint(_session(), restored)
        self.assertEqual(restored["pre_snapshot"]["reboot_generation"], 3)
        self.assertEqual(restored["snapshot"]["reboot_generation"], 5)
        self.assertEqual(
            restored["snapshot"]["usb_responses_completed"],
            2,
        )

    def test_cycle_one_checkpoint_keeps_v078_source_and_v079_pre_stage(
        self,
    ) -> None:
        fixture = _usb_cycle_result(1)
        source = _status(version=V078_VERSION)
        source.pop("last_expected_reboot_generation")
        source["uptime_s"] = 100
        for scanner in source["scanners"]:
            scanner["ver"] = "0.64.75-badge-defcon34"
        source_snapshot = acceptance.verify_cycle_pre_snapshot(
            source,
            _session(),
            CANARY_VERSION,
            1,
        )
        attested_pre_stage = _status(
            version=CANARY_VERSION,
            partition="ota_1",
            reboot_generation=1,
        )
        attested_pre_stage["uptime_s"] = 5
        for scanner in attested_pre_stage["scanners"]:
            scanner["ver"] = "0.64.75-badge-defcon34"
        fixture.pre_stage_status = attested_pre_stage
        fixture.final_restored_status[
            "last_expected_reboot_generation"
        ] = 3
        fixture.final_restored_status["running_partition"] = "ota_1"
        fixture.final_restored_status["uptime_s"] = 20

        with mock.patch.object(
            acceptance.flash,
            "_revalidate_usb_scanner_flow_result",
            return_value=fixture,
        ):
            checkpoint = acceptance.verify_update_cycle_result(
                fixture,
                _session(),
                CANARY_VERSION,
                1,
                candidate_artifacts=_candidate_artifacts(),
                updater_baseline=LivePromotionMetricsTest._issued_baseline(),
                source_pre_snapshot=source_snapshot,
                final_maintenance_sample=_issued_maintenance_sample(
                    generation=2,
                    partition="ota_1",
                ),
            )

        self.assertEqual(
            checkpoint["pre_snapshot"]["reboot_generation_capability"],
            "legacy-v078-absent",
        )
        self.assertIsNone(
            checkpoint["pre_snapshot"]["reboot_generation"]
        )
        self.assertEqual(
            checkpoint["snapshot"]["reboot_generation_capability"],
            "reported",
        )
        self.assertEqual(checkpoint["snapshot"]["reboot_generation"], 3)
        self.assertEqual(
            checkpoint["pre_snapshot"]["uplink_version"],
            V078_VERSION,
        )
        self.assertEqual(checkpoint["pre_snapshot"]["uptime_s"], 100)
        self.assertEqual(checkpoint["pre_snapshot"]["usb_rx_bytes"], 1024)
        self.assertEqual(
            checkpoint["pre_snapshot"]["usb_valid_commands"],
            18,
        )
        self.assertEqual(checkpoint["snapshot"]["uptime_s"], 20)
        self.assertEqual(checkpoint["snapshot"]["usb_rx_bytes"], 2000)
        self.assertEqual(
            checkpoint["snapshot"]["usb_valid_commands"],
            40,
        )

    def test_next_checkpoint_requires_exact_two_probe_boot_lineage(
        self,
    ) -> None:
        first = _verify_cycle_fixture(1)
        second = _verify_cycle_fixture(2)
        mutations = (
            ("reboot_generation", 4),
            ("running_partition", "ota_0"),
            ("uptime_s", 19),
            ("usb_rx_bytes", 2067),
            ("usb_rx_bytes", 2069),
            ("usb_valid_commands", 45),
            ("usb_valid_commands", 47),
            ("usb_responses_completed", 46),
            ("usb_responses_completed", 48),
        )
        for field, value in mutations:
            pre_values = dict(second["pre_snapshot"])
            pre_values[field] = value
            bad_pre = acceptance.VerifiedCyclePreSnapshot(
                acceptance._CYCLE_SNAPSHOT_ISSUER,
                pre_values,
            )
            checkpoint_values = dict(second)
            checkpoint_values["pre_snapshot"] = bad_pre
            bad_second = acceptance.VerifiedCycleCheckpoint(
                acceptance._CYCLE_CHECKPOINT_ISSUER,
                checkpoint_values,
            )
            with tempfile.TemporaryDirectory() as td:
                path = Path(td) / "acceptance.jsonl"
                _record_gate_one(path)
                acceptance.record_update_cycle_checkpoint(
                    path,
                    _session(),
                    first,
                )
                with self.subTest(field=field, value=value), \
                        self.assertRaises(acceptance.AcceptanceError):
                    acceptance.record_update_cycle_checkpoint(
                        path,
                        _session(),
                        bad_second,
                    )
                self.assertEqual(len(_read_records(path)), 2)

    def test_cycle_two_in_flow_probe_requires_exact_single_probe_delta(
        self,
    ) -> None:
        frozen = _frozen_candidate_artifacts()
        expected_status = _status(
            partition="ota_1",
            reboot_generation=3,
            responses_completed=44,
        )
        expected_status["uptime_s"] = 21
        expected_status["usb_health"]["rx_bytes"] = 2034
        expected_status["usb_health"]["valid_commands"] = 43
        expected_pre = acceptance.verify_cycle_pre_snapshot(
            expected_status,
            _session(),
            CANARY_VERSION,
            2,
        )
        exact_live = _cycle_source_status(2)
        mutations = (
            ("last_expected_reboot_generation", 4),
            ("running_partition", "ota_0"),
            ("uptime_s", 20),
            ("rx_bytes", 2067),
            ("rx_bytes", 2069),
            ("valid_commands", 45),
            ("valid_commands", 47),
            ("responses_completed", 46),
            ("responses_completed", 48),
        )
        for field, value in mutations:
            changed = copy.deepcopy(exact_live)
            if field in (
                "last_expected_reboot_generation",
                "running_partition",
                "uptime_s",
            ):
                changed[field] = value
            else:
                changed["usb_health"][field] = value

            def usb_flow(*_args, **kwargs):
                kwargs["pre_mutation_validator"](changed, frozen)
                raise AssertionError(
                    "invalid live source probe reached mutation"
                )

            with mock.patch.object(
                acceptance,
                "_trusted_session_uplink_descriptor",
                return_value=_usb_record("/dev/live-badge"),
            ), mock.patch.object(
                acceptance.flash,
                "usb_flow",
                side_effect=usb_flow,
            ), self.subTest(field=field, value=value), self.assertRaises(
                acceptance.AcceptanceError
            ):
                acceptance.run_update_cycle_checkpoint(
                    "/dev/live-badge",
                    _session(),
                    2,
                    expected_version=CANARY_VERSION,
                    frozen_artifacts=frozen,
                    candidate_artifacts=_candidate_artifacts(),
                    expected_pre_snapshot=expected_pre,
                    updater_baseline=LivePromotionMetricsTest._issued_baseline(),
                )

    def test_cycle_result_binds_final_normal_to_last_maintenance_boot(
        self,
    ) -> None:
        source_pre = acceptance.verify_cycle_pre_snapshot(
            _cycle_source_status(2),
            _session(),
            CANARY_VERSION,
            2,
        )
        maintenance = _issued_maintenance_sample(
            generation=4,
            partition="ota_0",
        )
        for generation, partition in (
            (4, "ota_0"),
            (6, "ota_0"),
            (5, "ota_1"),
        ):
            fixture = _usb_cycle_result(2)
            fixture.final_restored_status[
                "last_expected_reboot_generation"
            ] = generation
            fixture.final_restored_status[
                "running_partition"
            ] = partition
            with mock.patch.object(
                acceptance.flash,
                "_revalidate_usb_scanner_flow_result",
                return_value=fixture,
            ), self.subTest(
                generation=generation,
                partition=partition,
            ), self.assertRaises(acceptance.AcceptanceError):
                acceptance.verify_update_cycle_result(
                    fixture,
                    _session(),
                    CANARY_VERSION,
                    2,
                    candidate_artifacts=_candidate_artifacts(),
                    source_pre_snapshot=source_pre,
                    final_maintenance_sample=maintenance,
                    updater_baseline=(
                        LivePromotionMetricsTest._issued_baseline()
                    ),
                )

    def test_cycle_one_requires_post_bootstrap_proof_before_next_bytes(
        self,
    ) -> None:
        frozen = acceptance.flash.FrozenUsbFirmwareArtifacts(
            uplink=_frozen_artifacts(
                _uplink_firmware_image(version=CANARY_VERSION)
            ),
            scanner=_frozen_artifacts(
                _scanner_firmware_image(version=CANARY_VERSION)
            ),
        )
        artifacts = acceptance.verify_candidate_artifacts(
            frozen,
            CANARY_VERSION,
        )
        pre = LivePromotionMetricsTest._baseline_status()
        acquired = LivePromotionMetricsTest._baseline_status(
            responses_completed=23
        )
        acquired["uptime_s"] = 101
        baseline = acceptance.capture_v078_updater_baseline(
            acquired,
            _session(),
            pre_status=pre,
            challenge_version=V078_VERSION,
        )
        source = LivePromotionMetricsTest._baseline_status(
            responses_completed=26
        )
        source["uptime_s"] = 102
        source["usb_health"]["rx_bytes"] = 1080
        post = LivePromotionMetricsTest._normal_status()
        post.update({
            "running_partition": "ota_1",
            "last_expected_reboot_reason": "usb_uplink_ota",
            "last_expected_reboot_generation": 1,
        })
        maintenance = _maintenance_status(
            version=CANARY_VERSION,
            update_session="0123456789ABCDEF",
            partition="ota_1",
            reboot_generation=2,
            responses_completed=1,
        )
        maintenance["usb_health"]["valid_commands"] = 1
        events: list[str] = []

        def complete_flow(*_args, **kwargs):
            kwargs["pre_mutation_validator"](source, frozen)
            events.append("v078-prior")
            kwargs["post_direct_bootstrap_status_validator"](post)
            events.append("v079-post-bootstrap")
            kwargs["maintenance_status_validator"](
                maintenance,
                "0123456789ABCDEF",
            )
            events.append("maintenance-gen2")
            events.append("scanner-or-next-stage-bytes")
            return object()

        with mock.patch.object(
            acceptance,
            "_trusted_session_uplink_descriptor",
            return_value=_usb_record("/dev/live-badge"),
        ), mock.patch.object(
            acceptance.flash,
            "usb_flow",
            side_effect=complete_flow,
        ), mock.patch.object(
            acceptance,
            "verify_update_cycle_result",
            return_value=object(),
        ):
            acceptance.run_update_cycle_checkpoint(
                "/dev/live-badge",
                _session(),
                1,
                expected_version=CANARY_VERSION,
                frozen_artifacts=frozen,
                candidate_artifacts=artifacts,
                updater_baseline=baseline,
            )
        self.assertEqual(events, [
            "v078-prior",
            "v079-post-bootstrap",
            "maintenance-gen2",
            "scanner-or-next-stage-bytes",
        ])

        events.clear()

        def omitted_callback(*_args, **kwargs):
            kwargs["pre_mutation_validator"](source, frozen)
            kwargs["maintenance_status_validator"](
                maintenance,
                "0123456789ABCDEF",
            )
            events.append("scanner-or-next-stage-bytes")
            return object()

        with mock.patch.object(
            acceptance,
            "_trusted_session_uplink_descriptor",
            return_value=_usb_record("/dev/live-badge"),
        ), mock.patch.object(
            acceptance.flash,
            "usb_flow",
            side_effect=omitted_callback,
        ), mock.patch.object(
            acceptance,
            "verify_update_cycle_result",
            return_value=object(),
        ), self.assertRaises(acceptance.AcceptanceError):
            acceptance.run_update_cycle_checkpoint(
                "/dev/live-badge",
                _session(),
                1,
                expected_version=CANARY_VERSION,
                frozen_artifacts=frozen,
                candidate_artifacts=artifacts,
                updater_baseline=baseline,
            )
        self.assertEqual(events, [])

    def _run_cycle_one_live_callbacks(
        self,
        live_versions: tuple[str, ...],
        events: list[str] | None = None,
    ) -> list[str]:
        frozen = acceptance.flash.FrozenUsbFirmwareArtifacts(
            uplink=_frozen_artifacts(
                _uplink_firmware_image(version=CANARY_VERSION)
            ),
            scanner=_frozen_artifacts(
                _scanner_firmware_image(version=CANARY_VERSION)
            ),
        )
        artifacts = acceptance.verify_candidate_artifacts(
            frozen, CANARY_VERSION
        )
        updater_baseline = LivePromotionMetricsTest._issued_baseline()
        prior = LivePromotionMetricsTest._baseline_status(
            responses_completed=26
        )
        prior["uptime_s"] = 102
        prior["usb_health"]["rx_bytes"] = 1080
        post = LivePromotionMetricsTest._normal_status()
        post.update({
            "running_partition": "ota_1",
            "last_expected_reboot_reason": "usb_uplink_ota",
            "last_expected_reboot_generation": 1,
        })
        maintenance_statuses = [
            _maintenance_status(
                version=version,
                update_session="0123456789ABCDEF",
                partition="ota_1",
                reboot_generation=2,
                responses_completed=index,
            )
            for index, version in enumerate(live_versions, start=1)
        ]
        for index, status in enumerate(maintenance_statuses):
            status["uptime_s"] = 5 + index
            status["usb_health"]["rx_bytes"] = 1024 + index
            status["usb_health"]["valid_commands"] = index + 1
        if events is None:
            events = []

        def usb_flow(*_args, **kwargs):
            kwargs["pre_mutation_validator"](prior, frozen)
            kwargs["post_direct_bootstrap_status_validator"](post)
            for status in maintenance_statuses:
                kwargs["maintenance_status_validator"](
                    status,
                    "0123456789ABCDEF",
                )
            events.append("uplink-or-scanner-bytes")
            return object()

        with mock.patch.object(
            acceptance,
            "_trusted_session_uplink_descriptor",
            return_value=_usb_record("/dev/live-badge"),
        ), mock.patch.object(
            acceptance.flash,
            "usb_flow",
            side_effect=usb_flow,
        ), mock.patch.object(
            acceptance,
            "verify_update_cycle_result",
            return_value=object(),
        ):
            acceptance.run_update_cycle_checkpoint(
                "/dev/live-badge",
                _session(),
                1,
                expected_version=CANARY_VERSION,
                frozen_artifacts=frozen,
                candidate_artifacts=artifacts,
                updater_baseline=updater_baseline,
            )
        return events

    def test_cycle_one_allows_v078_prior_only_when_live_is_v079(
        self,
    ) -> None:
        self.assertEqual(
            self._run_cycle_one_live_callbacks((CANARY_VERSION,)),
            ["uplink-or-scanner-bytes"],
        )

    def test_cycle_one_rejects_v078_live_before_firmware_bytes(self) -> None:
        events: list[str] = []
        with self.assertRaisesRegex(
            acceptance.AcceptanceError,
            "exact \\.79",
        ):
            self._run_cycle_one_live_callbacks(
                (V078_VERSION,),
                events,
            )
        self.assertEqual(events, [])

    def test_cycle_one_rejects_v079_prior_to_v078_live_before_bytes(
        self,
    ) -> None:
        events: list[str] = []
        with self.assertRaisesRegex(
            acceptance.AcceptanceError,
            "exact \\.79",
        ):
            self._run_cycle_one_live_callbacks(
                (CANARY_VERSION, V078_VERSION),
                events,
            )
        self.assertEqual(events, [])

    def test_gate_one_anchors_the_reachable_cycle_one_pre_update_state(
        self,
    ) -> None:
        cycle_one = _usb_cycle_result(1)
        gate_one_facts = _pass_facts("android-control-reconnect")
        gate_one_facts["snapshot"] = acceptance.verify_cycle_pre_snapshot(
            _cycle_source_status(1),
            _session(),
            VERSION,
            1,
        )
        checkpoint = _verify_cycle_fixture(1, result=cycle_one)

        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            try:
                acceptance.record_gate(
                    path,
                    _session(),
                    "android-control-reconnect",
                    "PASS",
                    gate_one_facts,
                )
            except acceptance.AcceptanceError as exc:
                self.fail(
                    "reachable Gate 1 pre-update state was rejected: "
                    f"{exc}"
                )
            aggregate_appended = (
                acceptance.record_update_cycle_checkpoint(
                    path,
                    _session(),
                    checkpoint,
                )
            )

        self.assertFalse(aggregate_appended)

    def test_cycle_one_rejects_scanner_versions_changed_after_gate_one(
        self,
    ) -> None:
        gate_one_facts = _pass_facts("android-control-reconnect")
        changed_cycle = _usb_cycle_result(1)
        changed_source = _cycle_source_status(1)
        for scanner in changed_cycle.pre_stage_status["scanners"]:
            scanner["ver"] = "0.64.74-badge-defcon34"
        for scanner in changed_source["scanners"]:
            scanner["ver"] = "0.64.74-badge-defcon34"
        checkpoint = _verify_cycle_fixture(
            1,
            result=changed_cycle,
            source_status=changed_source,
        )

        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            acceptance.record_gate(
                path,
                _session(),
                "android-control-reconnect",
                "PASS",
                gate_one_facts,
            )
            with self.assertRaisesRegex(
                acceptance.AcceptanceError,
                "Gate 1",
            ):
                acceptance.record_update_cycle_checkpoint(
                    path,
                    _session(),
                    checkpoint,
                )

    def test_next_cycle_prefix_rejects_gate_one_version_discontinuity(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            acceptance.record_gate(
                path,
                _session(),
                "android-control-reconnect",
                "PASS",
                _pass_facts("android-control-reconnect"),
            )
            acceptance.record_update_cycle_checkpoint(
                path,
                _session(),
                _verify_cycle_fixture(1),
            )
            records = _read_records(path)
            cycle_one = next(
                record for record in records
                if record["gate"] == "three-update-cycles"
                and record["status"] == "CHECKPOINT"
            )
            cycle_one["facts"]["pre_snapshot"][
                "wifi_version"
            ] = "0.64.74-badge-defcon34"

            with self.assertRaisesRegex(
                acceptance.AcceptanceError,
                "Gate 1",
            ):
                acceptance._validate_mutating_gate_prefix(
                    records,
                    _session(),
                    "three-update-cycles",
                    cycle=2,
                )

    def test_session_descriptor_uses_anchored_identity_before_open(
        self,
    ) -> None:
        descriptor = _usb_record("/dev/live-badge")
        requested_binding = acceptance.flash.TrustedUplinkBinding(
            serial_number=UPLINK_ID,
            location=None,
            source="retained-session",
        )
        strengthened_binding = dataclasses.replace(
            requested_binding,
            location=descriptor.location,
        )
        with mock.patch.object(
            acceptance.flash,
            "select_trusted_uplink_descriptor",
            return_value=(descriptor, strengthened_binding),
        ) as select:
            observed = acceptance._trusted_session_uplink_descriptor(
                "/dev/live-badge", _session()
            )

        self.assertEqual(observed, descriptor)
        select.assert_called_once_with(
            selected_port="/dev/live-badge",
            operator_acknowledged=False,
            trusted_binding=requested_binding,
        )

    def test_session_descriptor_rejects_a_dropped_initial_location(self) -> None:
        descriptor = _usb_record("/dev/live-badge")
        dropped = acceptance.flash.TrustedUplinkBinding(
            serial_number=UPLINK_ID,
            location=None,
            source="retained-session",
        )
        with mock.patch.object(
            acceptance.flash,
            "select_trusted_uplink_descriptor",
            return_value=(descriptor, dropped),
        ), self.assertRaisesRegex(
            acceptance.AcceptanceError,
            "location",
        ):
            acceptance._trusted_session_uplink_descriptor(
                "/dev/live-badge", _session()
            )

    def test_rebound_and_final_descriptors_keep_exact_initial_location(
        self,
    ) -> None:
        initial = _usb_record("/dev/initial")
        rebound = dataclasses.replace(
            initial,
            device="/dev/rebound",
            stat_inode=initial.stat_inode + 1,
        )
        self.assertEqual(
            acceptance._require_session_uplink_descriptor(
                rebound,
                _session(),
                stage="rebound",
                expected_location=initial.location,
            ),
            rebound,
        )
        for moved in (
            dataclasses.replace(rebound, location="different-location"),
            dataclasses.replace(rebound, location=None),
        ):
            with self.subTest(location=moved.location), self.assertRaisesRegex(
                acceptance.AcceptanceError,
                "location",
            ):
                acceptance._require_session_uplink_descriptor(
                    moved,
                    _session(),
                    stage="final",
                    expected_location=initial.location,
                )

        no_location = dataclasses.replace(initial, location=None)
        no_location_rebound = dataclasses.replace(
            rebound,
            location=None,
        )
        self.assertEqual(
            acceptance._require_session_uplink_descriptor(
                no_location_rebound,
                _session(),
                stage="rebound",
                expected_location=no_location.location,
            ),
            no_location_rebound,
        )

    def test_production_cycle_result_issues_exact_machine_checkpoint(
        self,
    ) -> None:
        checkpoint = _verify_cycle_fixture(1)
        self.assertEqual(checkpoint["cycle"], 1)
        self.assertEqual(checkpoint["stage_generation"], 21)
        self.assertEqual(checkpoint["slot_mask"], 3)
        self.assertEqual(checkpoint["scanner_flow_control"], "credit-v1")
        self.assertEqual(checkpoint["scanner_stage_phase"], "final")
        self.assertEqual(
            checkpoint["scanner_stage_received"],
            len(_scanner_firmware_image()),
        )
        self.assertEqual(
            checkpoint["scanner_stage_total"],
            len(_scanner_firmware_image()),
        )
        self.assertEqual(checkpoint["scanner_stage_credit_bytes"], 0)
        self.assertEqual(set(checkpoint), {
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
        self.assertEqual(checkpoint["pending_mask_after"], 0)
        self.assertEqual(checkpoint["preflight_older_slot_mask"], 3)
        self.assertEqual(checkpoint["recovery_slot_mask"], 0)
        self.assertEqual(checkpoint["manual_relay_commands"], 0)
        self.assertEqual(checkpoint["ble_state"], "converged")
        self.assertGreaterEqual(checkpoint["ble_attempts"], 1)
        self.assertIs(checkpoint["theme_restored"], True)
        self.assertIs(checkpoint["fresh_ping_status"], True)

    def test_cycle_result_accepts_bound_exact_lane_retry_history(
        self,
    ) -> None:
        result = _usb_cycle_retry_result()
        checkpoint = _verify_cycle_fixture(1, result=result)
        self.assertEqual(checkpoint["scanner_uploads"], 2)
        self.assertEqual(checkpoint["slot_mask"], 2)
        self.assertEqual(checkpoint["ble_state"], "converged")
        self.assertEqual(checkpoint["wifi_state"], "converged")

    def test_cycle_result_rejects_retry_that_rewrites_successful_peer(
        self,
    ) -> None:
        result = _usb_cycle_retry_result()
        result.attempt_history[1]["requested_slots"] = ["ble", "wifi"]
        result.attempt_history[1]["stage_receipt"]["slot_mask"] = 3
        result.stage_receipts[1]["slot_mask"] = 3
        result.stage_receipt["slot_mask"] = 3
        with self.assertRaisesRegex(
            acceptance.AcceptanceError, "successful lane"
        ):
            _verify_cycle_fixture(1, result=result)

    def test_cycle_result_requires_exact_final_credit_v1_receipt(
        self,
    ) -> None:
        mutations = {
            "flow_control": (None, "legacy-paced"),
            "phase": (None, "ready"),
            "received": (None, 1_197_972, True),
            "total": (None, 1_197_972, True),
            "credit_bytes": (None, 4096, False),
        }
        for field, invalid_values in mutations.items():
            for invalid in invalid_values:
                with self.subTest(field=field, invalid=invalid):
                    result = _usb_cycle_result(1)
                    if invalid is None:
                        del result.stage_receipt[field]
                    else:
                        result.stage_receipt[field] = invalid
                    with self.assertRaisesRegex(
                        acceptance.AcceptanceError,
                        "credit-v1 final",
                    ):
                        _verify_cycle_fixture(1, result=result)

    def test_cycle_result_rejects_forged_counts_flags_and_slot_sets(
        self,
    ) -> None:
        mutations = (
            ("stage_count", 2),
            ("theme_restored", False),
            ("fresh_usb_proven", False),
            ("preflight_older_slots", frozenset({"ble"})),
            ("recovery_slots", frozenset({"ble"})),
        )
        for field, value in mutations:
            with self.subTest(field=field):
                result = _usb_cycle_result(1 if field != "recovery_slots" else 2)
                setattr(result, field, value)
                with self.assertRaises(acceptance.AcceptanceError):
                    _verify_cycle_fixture(
                        1 if field != "recovery_slots" else 2,
                        result=result,
                    )

    def test_cycle_result_rejects_stage_receipt_artifact_mismatch(self) -> None:
        for field, value in (
            ("size", 1),
            ("crc32", 1),
            ("sha256", "f" * 64),
        ):
            with self.subTest(field=field):
                result = _usb_cycle_result(1)
                result.stage_receipt[field] = value
                with self.assertRaisesRegex(
                    acceptance.AcceptanceError,
                    "artifact",
                ):
                    _verify_cycle_fixture(1, result=result)

    def test_cycle_result_rejects_a_non_attested_fixture(self) -> None:
        with self.assertRaisesRegex(
            acceptance.AcceptanceError, "production-issued"
        ):
            acceptance.verify_update_cycle_result(
                _usb_cycle_result(1),
                _session(),
                VERSION,
                1,
                candidate_artifacts=_candidate_artifacts(),
                source_pre_snapshot=acceptance.verify_cycle_pre_snapshot(
                    _cycle_source_status(1),
                    _session(),
                    CANARY_VERSION,
                    1,
                ),
                final_maintenance_sample=_issued_maintenance_sample(
                    generation=2,
                    partition="ota_1",
                ),
            )

    def test_checkpoint_sequence_is_locked_and_cycle_three_appends_pass(
        self,
    ) -> None:
        checkpoints = [
            _verify_cycle_fixture(cycle)
            for cycle in (1, 2, 3)
        ]
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            acceptance.record_gate(
                path,
                _session(),
                "android-control-reconnect",
                "PASS",
                _pass_facts("android-control-reconnect"),
            )
            self.assertFalse(
                acceptance.record_update_cycle_checkpoint(
                    path, _session(), checkpoints[0]
                )
            )
            self.assertFalse(
                acceptance.record_update_cycle_checkpoint(
                    path, _session(), checkpoints[1]
                )
            )
            self.assertTrue(
                acceptance.record_update_cycle_checkpoint(
                    path, _session(), checkpoints[2]
                )
            )
            records = [
                json.loads(line)
                for line in path.read_text(encoding="utf-8").splitlines()
            ]
        cycle_records = [
            record for record in records
            if record["gate"] == "three-update-cycles"
        ]
        self.assertEqual(
            [record["status"] for record in cycle_records],
            ["CHECKPOINT", "CHECKPOINT", "CHECKPOINT", "PASS"],
        )
        self.assertEqual(
            cycle_records[-1]["facts"]["checkpoint_generations"],
            [21, 22, 23],
        )

    def test_checkpoint_rejects_out_of_order_or_nonadvancing_generation(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            acceptance.record_gate(
                path,
                _session(),
                "android-control-reconnect",
                "PASS",
                _pass_facts("android-control-reconnect"),
            )
            cycle_two = _verify_cycle_fixture(2)
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "cycle 1"
            ):
                acceptance.record_update_cycle_checkpoint(
                    path, _session(), cycle_two
                )

            cycle_one = _verify_cycle_fixture(1)
            acceptance.record_update_cycle_checkpoint(
                path, _session(), cycle_one
            )
            stale_two = _verify_cycle_fixture(2, generation=21)
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "generation"
            ):
                acceptance.record_update_cycle_checkpoint(
                    path, _session(), stale_two
                )

    def test_cycle_runner_uses_fixed_production_arguments(self) -> None:
        result = _usb_cycle_result(2)
        result.final_restored_status[
            "last_expected_reboot_generation"
        ] = 6
        descriptor = _usb_record("/dev/live-badge")
        frozen = _frozen_candidate_artifacts()
        first_maintenance = _maintenance_status(
            version=CANARY_VERSION,
            update_session="0123456789ABCDEF",
            partition="ota_1",
            reboot_generation=4,
            responses_completed=1,
        )
        committed_maintenance = _maintenance_status(
            version=CANARY_VERSION,
            update_session="0123456789ABCDEF",
            partition="ota_0",
            reboot_generation=5,
            responses_completed=1,
        )

        def run_flow(*_args, **kwargs):
            kwargs["pre_mutation_validator"](
                _cycle_source_status(2),
                frozen,
            )
            for status in (first_maintenance, committed_maintenance):
                kwargs["maintenance_status_validator"](
                    status,
                    "0123456789ABCDEF",
                )
            return result

        with mock.patch.object(
            acceptance.flash, "repo_version", return_value=VERSION
        ), mock.patch.object(
            acceptance.flash, "require_artifacts"
        ) as require_artifacts, mock.patch.object(
            acceptance.flash,
            "usb_flow",
            side_effect=run_flow,
        ) as usb_flow, mock.patch.object(
            acceptance.flash,
            "_revalidate_usb_scanner_flow_result",
            return_value=result,
        ), mock.patch.object(
            acceptance,
            "_trusted_session_uplink_descriptor",
            return_value=descriptor,
        ):
            checkpoint = acceptance.run_update_cycle_checkpoint(
                "/dev/live-badge",
                _session(),
                2,
                expected_version=VERSION,
                frozen_artifacts=frozen,
                candidate_artifacts=_candidate_artifacts(),
                updater_baseline=LivePromotionMetricsTest._issued_baseline(),
            )
        require_artifacts.assert_not_called()
        flow_args = usb_flow.call_args.args
        self.assertTrue(flow_args[0].recovery_rewrite_same_version)
        self.assertFalse(flow_args[0].dry_run)
        self.assertEqual(flow_args[0].port, "/dev/live-badge")
        self.assertFalse(flow_args[0].bind_selected_uplink)
        self.assertEqual(
            flow_args[0].trusted_uplink_binding,
            acceptance.flash.TrustedUplinkBinding(
                serial_number=UPLINK_ID,
                location=descriptor.location,
                source="retained-session",
            ),
        )
        self.assertIs(flow_args[2], True)
        self.assertEqual(flow_args[3], ["ble", "wifi"])
        self.assertEqual(flow_args[4], VERSION)
        self.assertEqual(checkpoint["cycle"], 2)

    def test_canary_cycle_live_gates_run_before_each_firmware_lane(
        self,
    ) -> None:
        frozen = acceptance.flash.FrozenUsbFirmwareArtifacts(
            uplink=_frozen_artifacts(
                _uplink_firmware_image(version=CANARY_VERSION)
            ),
            scanner=_frozen_artifacts(
                _scanner_firmware_image(version=CANARY_VERSION)
            ),
        )
        artifacts = acceptance.verify_candidate_artifacts(
            frozen, CANARY_VERSION
        )
        updater_baseline = LivePromotionMetricsTest._issued_baseline()
        normal = LivePromotionMetricsTest._normal_status()
        maintenance = _maintenance_status(
            version=CANARY_VERSION,
            update_session="0123456789ABCDEF",
            reboot_generation=51,
            responses_completed=1,
        )
        events: list[str] = []

        def usb_flow(*_args, **kwargs):
            kwargs["pre_mutation_validator"](normal, frozen)
            events.append("normal-gate")
            kwargs["maintenance_status_validator"](
                maintenance,
                "0123456789ABCDEF",
            )
            events.append("maintenance-gate")
            events.append("uplink-or-scanner-bytes")
            return object()

        with mock.patch.object(
            acceptance,
            "_trusted_session_uplink_descriptor",
            return_value=_usb_record("/dev/live-badge"),
        ), mock.patch.object(
            acceptance.flash,
            "usb_flow",
            side_effect=usb_flow,
        ), mock.patch.object(
            acceptance,
            "verify_update_cycle_result",
            return_value=object(),
        ):
            acceptance.run_update_cycle_checkpoint(
                "/dev/live-badge",
                _session(),
                2,
                expected_version=CANARY_VERSION,
                frozen_artifacts=frozen,
                candidate_artifacts=artifacts,
                updater_baseline=updater_baseline,
            )

        self.assertEqual(events, [
            "normal-gate",
            "maintenance-gate",
            "uplink-or-scanner-bytes",
        ])

    def test_invalid_cycle_is_rejected_before_artifact_or_device_access(
        self,
    ) -> None:
        for cycle in (0, 4, True):
            with self.subTest(cycle=cycle), mock.patch.object(
                acceptance.flash, "repo_version"
            ) as repo_version, mock.patch.object(
                acceptance.flash, "require_artifacts"
            ) as require_artifacts, mock.patch.object(
                acceptance.flash, "usb_flow"
            ) as usb_flow, self.assertRaisesRegex(
                acceptance.AcceptanceError, "cycle"
            ):
                acceptance.run_update_cycle_checkpoint(
                    "/dev/live-badge", _session(), cycle
                )
            repo_version.assert_not_called()
            require_artifacts.assert_not_called()
            usb_flow.assert_not_called()

    def test_cycle_runner_rejects_changed_frozen_bytes_before_usb(
        self,
    ) -> None:
        scanner_image = bytearray(_scanner_firmware_image())
        scanner_image[-1] ^= 0x01
        changed = acceptance.flash.FrozenUsbFirmwareArtifacts(
            uplink=_frozen_artifacts(_uplink_firmware_image()),
            scanner=_frozen_artifacts(bytes(scanner_image)),
        )
        with mock.patch.object(
            acceptance.flash, "require_artifacts"
        ) as require_artifacts, mock.patch.object(
            acceptance,
            "_trusted_session_uplink_descriptor",
        ) as trusted_descriptor, mock.patch.object(
            acceptance.flash, "usb_flow"
        ) as usb_flow, self.assertRaisesRegex(
            acceptance.AcceptanceError,
            "anchored",
        ):
            acceptance.run_update_cycle_checkpoint(
                "/dev/live-badge",
                _session(),
                1,
                expected_version=VERSION,
                frozen_artifacts=changed,
                candidate_artifacts=_candidate_artifacts(),
                updater_baseline=LivePromotionMetricsTest._issued_baseline(),
            )

        require_artifacts.assert_not_called()
        trusted_descriptor.assert_not_called()
        usb_flow.assert_not_called()

    def test_cycle_in_flow_preflight_rejects_state_change_before_mutation(
        self,
    ) -> None:
        frozen = _frozen_candidate_artifacts()
        before = _status(version=V078_VERSION)
        before.pop("last_expected_reboot_generation")
        for scanner in before["scanners"]:
            scanner["ver"] = "0.64.75-badge-defcon34"
        expected_pre = acceptance.verify_cycle_pre_snapshot(
            before,
            _session(),
            VERSION,
            1,
        )
        changed = copy.deepcopy(before)
        for scanner in changed["scanners"]:
            scanner["ver"] = "0.64.74-badge-defcon34"
        descriptor = _usb_record("/dev/live-badge")

        def usb_flow(*_args, **kwargs):
            kwargs["pre_mutation_validator"](changed, frozen)
            raise AssertionError("rejected preflight unexpectedly returned")

        with mock.patch.object(
            acceptance,
            "_trusted_session_uplink_descriptor",
            return_value=descriptor,
        ), mock.patch.object(
            acceptance.flash,
            "usb_flow",
            side_effect=usb_flow,
        ) as run_flow, self.assertRaisesRegex(
            acceptance.AcceptanceError,
            "changed",
        ):
            acceptance.run_update_cycle_checkpoint(
                "/dev/live-badge",
                _session(),
                1,
                expected_version=VERSION,
                frozen_artifacts=frozen,
                candidate_artifacts=_candidate_artifacts(),
                expected_pre_snapshot=expected_pre,
                updater_baseline=LivePromotionMetricsTest._issued_baseline(),
            )

        run_flow.assert_called_once()


class EvidenceRecordingTest(unittest.TestCase):
    def test_interrupted_gate_requires_exact_firmware_reboot_reasons(
        self,
    ) -> None:
        facts = _pass_facts("interrupted-upload")
        mutations = (
            ("baseline_snapshot", "usb_uplink_ota"),
            ("recovered_snapshot", "update_finish"),
            ("snapshot", "update_abort"),
        )
        for key, reason in mutations:
            serialized = json.loads(json.dumps(facts))
            serialized[key]["last_expected_reboot_reason"] = reason
            restored = acceptance._pass_facts_from_serialized(
                "interrupted-upload",
                serialized,
            )
            with self.subTest(key=key, reason=reason), self.assertRaises(
                acceptance.AcceptanceError
            ):
                acceptance._validate_pass_facts(
                    _session(),
                    "interrupted-upload",
                    restored,
                )

    def test_chord_pass_rejects_caller_authored_facts(self) -> None:
        with tempfile.TemporaryDirectory() as td, self.assertRaisesRegex(
            acceptance.AcceptanceError,
            "machine-issued",
        ):
            acceptance.record_gate(
                Path(td) / "acceptance.jsonl",
                _session(),
                "chord-rom-recovery",
                "PASS",
                dict(_pass_facts("chord-rom-recovery")),
            )

    def test_all_six_pass_gates_append_privacy_safe_records(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            acceptance.record_gate(
                path,
                _session(),
                "android-control-reconnect",
                "PASS",
                _pass_facts("android-control-reconnect"),
            )
            for cycle in (1, 2, 3):
                checkpoint = _verify_cycle_fixture(cycle)
                acceptance.record_update_cycle_checkpoint(
                    path, _session(), checkpoint
                )
            for gate in acceptance.REQUIRED_GATES[2:4]:
                _append_pass_fixture(path, gate)
            for gate in acceptance.REQUIRED_GATES[4:]:
                acceptance.record_gate(
                    path, _session(), gate, "PASS", _pass_facts(gate)
                )
            records = [
                json.loads(line)
                for line in path.read_text(encoding="utf-8").splitlines()
            ]
        pass_records = [
            record for record in records if record["status"] == "PASS"
        ]
        self.assertEqual(
            [record["gate"] for record in pass_records],
            list(acceptance.REQUIRED_GATES),
        )
        self.assertEqual(
            sum(record["status"] == "CHECKPOINT" for record in records), 3
        )
        self.assertTrue(all(record["schema"] == 1 for record in records))
        self.assertTrue(all(record["timestamp_utc"].endswith("Z")
                            for record in records))

    def test_record_gate_rejects_out_of_order_manual_pass_without_append(
        self,
    ) -> None:
        scenarios = (
            (
                "duplicate-gate-one",
                _record_gate_one,
                "android-control-reconnect",
            ),
            (
                "gate-five-before-gate-four",
                _record_gate_three,
                "no-host-reboot",
            ),
            (
                "gate-six-before-gate-five",
                _record_gate_four,
                "power-state-audit",
            ),
        )
        for name, arrange, gate in scenarios:
            with self.subTest(name=name), tempfile.TemporaryDirectory() as td:
                path = Path(td) / "acceptance.jsonl"
                arrange(path)
                before = path.read_bytes()

                with self.assertRaisesRegex(
                    acceptance.AcceptanceError,
                    "exact next",
                ):
                    acceptance.record_gate(
                        path,
                        _session(),
                        gate,
                        "PASS",
                        _pass_facts(gate),
                    )

                self.assertEqual(path.read_bytes(), before)

    def test_record_gate_accepts_exact_gate_one_five_and_six_prefixes(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            _record_gate_four(path)
            acceptance.record_gate(
                path,
                _session(),
                "no-host-reboot",
                "PASS",
                _pass_facts("no-host-reboot"),
            )
            acceptance.record_gate(
                path,
                _session(),
                "power-state-audit",
                "PASS",
                _pass_facts("power-state-audit"),
            )
            observed = [
                (record["gate"], record["status"])
                for record in _read_records(path)
            ]

        self.assertEqual(
            observed,
            [
                ("android-control-reconnect", "PASS"),
                ("three-update-cycles", "CHECKPOINT"),
                ("three-update-cycles", "CHECKPOINT"),
                ("three-update-cycles", "CHECKPOINT"),
                ("three-update-cycles", "PASS"),
                ("interrupted-upload", "PASS"),
                ("chord-rom-recovery", "PASS"),
                ("no-host-reboot", "PASS"),
                ("power-state-audit", "PASS"),
            ],
        )

    def test_record_gate_rejects_machine_pass_paths_before_file_creation(
        self,
    ) -> None:
        for gate in ("interrupted-upload", "chord-rom-recovery"):
            with self.subTest(gate=gate), tempfile.TemporaryDirectory() as td:
                path = Path(td) / "acceptance.jsonl"

                with self.assertRaisesRegex(
                    acceptance.AcceptanceError,
                    "reserved machine",
                ):
                    acceptance.record_gate(
                        path,
                        _session(),
                        gate,
                        "PASS",
                        _pass_facts(gate),
                    )

                self.assertFalse(path.exists())

    def test_stale_reservation_prevents_creating_missing_evidence_file(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            path = root / "acceptance.jsonl"
            reservation_path = root / (
                f".{path.name}.mutating-gate-reservation"
            )
            reservation_path.write_text("{}\n", encoding="utf-8")
            os.chmod(reservation_path, 0o600)

            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "reservation"
            ):
                acceptance.record_gate(
                    path,
                    _session(),
                    "power-state-audit",
                    "FAIL",
                    {"error": "operator_gate_failed"},
                )

            self.assertFalse(path.exists())

    def test_record_gate_rejects_existing_json_without_terminal_newline(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            acceptance.record_gate(
                path,
                _session(),
                "android-control-reconnect",
                "PASS",
                _pass_facts("android-control-reconnect"),
            )
            path.write_text(
                path.read_text(encoding="utf-8").rstrip("\n"),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "newline"
            ):
                acceptance.record_gate(
                    path,
                    _session(),
                    "power-state-audit",
                    "PASS",
                    _pass_facts("power-state-audit"),
                )

    def test_recording_rejects_fifo_and_nonprivate_evidence_file(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            fifo = root / "evidence.fifo"
            os.mkfifo(fifo)
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "regular file"
            ):
                acceptance.record_gate(
                    fifo,
                    _session(),
                    "power-state-audit",
                    "FAIL",
                    {"error": "operator_gate_failed"},
                )

            public = root / "public.jsonl"
            public.touch(mode=0o644)
            os.chmod(public, 0o644)
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "private"
            ):
                acceptance.record_gate(
                    public,
                    _session(),
                    "power-state-audit",
                    "FAIL",
                    {"error": "operator_gate_failed"},
                )

    def test_fail_evidence_accepts_codes_only_and_rejects_ssid_text(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            acceptance.record_gate(
                path,
                _session(),
                "power-state-audit",
                "FAIL",
                {"error": "operator_gate_failed", "phase": "power_audit"},
            )
            with self.assertRaises(acceptance.AcceptanceError):
                acceptance.record_gate(
                    path,
                    _session(),
                    "power-state-audit",
                    "FAIL",
                    {"error": "SSID GameChangersAI-67 observed"},
                )

    def test_same_session_id_rejects_changed_board_binding(self) -> None:
        swapped = acceptance.BadgeAcceptanceSession(
            session_id=_session().session_id,
            uplink_hardware_id=UPLINK_ID,
            ble_hardware_id=BLE_ID,
            wifi_hardware_id="02:00:00:00:00:04",
        )
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            acceptance.record_gate(
                path,
                _session(),
                "android-control-reconnect",
                "PASS",
                _pass_facts("android-control-reconnect"),
            )
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "binding"
            ):
                acceptance.record_gate(
                    path, swapped, "power-state-audit", "FAIL",
                    {"error": "identity_mismatch"},
                )

    def test_pass_rejects_caller_forged_minimal_snapshot(self) -> None:
        facts = _pass_facts("power-state-audit")
        facts["snapshot"] = {
            key: facts["snapshot"][key]
            for key in (
                "uplink_hardware_id",
                "ble_hardware_id",
                "wifi_hardware_id",
                "rollback_clear",
                "usb_parser_state",
                "radio_health",
            )
        }
        with tempfile.TemporaryDirectory() as td:
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "verifier-issued"
            ):
                acceptance.record_gate(
                    Path(td) / "acceptance.jsonl",
                    _session(),
                    "power-state-audit",
                    "PASS",
                    facts,
                )

    def test_pass_rejects_missing_gate_fact_or_unobserved_reboot_reason(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            facts = dict(_pass_facts("chord-rom-recovery"))
            del facts["full_layout_verified"]
            with self.assertRaises(acceptance.AcceptanceError):
                acceptance.record_gate(
                    path, _session(), "chord-rom-recovery", "PASS", facts
                )

            facts = _pass_facts("no-host-reboot")
            facts["snapshot"] = dict(facts["snapshot"])
            facts["snapshot"]["last_expected_reboot_reason"] = ""
            with self.assertRaises(acceptance.AcceptanceError):
                acceptance.record_gate(
                    path, _session(), "no-host-reboot", "PASS", facts
                )

    def test_chord_serialized_evidence_keeps_rom_and_final_provenance_apart(
        self,
    ) -> None:
        serialized = json.loads(json.dumps(
            _pass_facts("chord-rom-recovery")
        ))
        restored = acceptance._pass_facts_from_serialized(
            "chord-rom-recovery",
            serialized,
        )
        acceptance._validate_pass_facts(
            _session(),
            "chord-rom-recovery",
            restored,
        )

        for path, value in (
            (("snapshot", "last_expected_reboot_reason"), "button_usb_rom"),
            (
                ("rom_boot_snapshot", "last_expected_reboot_reason"),
                "update_finish",
            ),
            (("rom_boot_snapshot", "reboot_generation"), 1),
        ):
            changed = copy.deepcopy(serialized)
            changed[path[0]][path[1]] = value
            restored_changed = acceptance._pass_facts_from_serialized(
                "chord-rom-recovery",
                changed,
            )
            with self.subTest(path=path), self.assertRaises(
                acceptance.AcceptanceError
            ):
                acceptance._validate_pass_facts(
                    _session(),
                    "chord-rom-recovery",
                    restored_changed,
                )

    def test_gate_two_rejects_caller_authored_summary_as_pass(self) -> None:
        facts = _pass_facts("three-update-cycles")
        with tempfile.TemporaryDirectory() as td:
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "locked aggregate"
            ):
                acceptance.record_gate(
                    Path(td) / "acceptance.jsonl",
                    _session(),
                    "three-update-cycles",
                    "PASS",
                    facts,
                )

    def test_no_host_gate_requires_power_only_charger_repeat(self) -> None:
        facts = _pass_facts("no-host-reboot")
        del facts["power_only_charger_repeated"]
        with tempfile.TemporaryDirectory() as td:
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "power_only_charger_repeated"
            ):
                acceptance.record_gate(
                    Path(td) / "acceptance.jsonl",
                    _session(),
                    "no-host-reboot",
                    "PASS",
                    facts,
                )

    def test_evidence_rejects_ambient_detections_ssids_and_nearby_macs(self) -> None:
        forbidden = (
            {"detected_ssid": "GameChangersAI-67"},
            {"detection_payload": {"kind": "drone"}},
            {"payload": {"kind": "drone"}},
            {"nearby_device": "AA:BB:CC:DD:EE:FF"},
            {"notes": "saw AA:BB:CC:DD:EE:FF nearby"},
            {"notes": "saw AA-BB-CC-DD-EE-FF nearby"},
            {"notes": "saw AABBCCDDEEFF nearby"},
        )
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            for extra in forbidden:
                with self.subTest(extra=extra):
                    facts = {
                        **_pass_facts("power-state-audit"),
                        **extra,
                    }
                    with self.assertRaises(acceptance.AcceptanceError):
                        acceptance.record_gate(
                            path,
                            _session(),
                            "power-state-audit",
                            "PASS",
                            facts,
                        )

    def test_scanner_cache_proof_requires_a_stored_manifest(self) -> None:
        with self.assertRaisesRegex(
            acceptance.AcceptanceError, "stored scanner"
        ):
            acceptance._scanner_store_fingerprint({
                "firmware_store": {"stored": False}
            })

    def test_interrupted_gate_requires_all_orchestration_proofs(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            for missing in (
                "baseline_snapshot",
                "recovered_snapshot",
                "idle_wait_s",
            ):
                with self.subTest(missing=missing):
                    facts = _pass_facts("interrupted-upload")
                    facts.pop(missing)
                    with self.assertRaises(acceptance.AcceptanceError):
                        acceptance.record_gate(
                            path,
                            _session(),
                            "interrupted-upload",
                            "PASS",
                            facts,
                        )

    def test_anchored_session_loads_only_existing_exact_binding(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "anchored"
            ):
                acceptance.load_anchored_session(path)

            acceptance.record_gate(
                path,
                _session(),
                "android-control-reconnect",
                "PASS",
                _pass_facts("android-control-reconnect"),
            )
            self.assertEqual(
                acceptance.load_anchored_session(path), _session()
            )
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "not anchored"
            ):
                acceptance.load_anchored_session(path, "another-session")


class CompletionAuditTest(unittest.TestCase):
    def test_interrupted_baseline_must_equal_cycle_three_final_snapshot(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            _record_complete_evidence(path)
            records = _read_records(path)
            interrupted = next(
                record for record in records
                if record["gate"] == "interrupted-upload"
            )
            interrupted["facts"]["baseline_snapshot"][
                "usb_rx_bytes"
            ] += 1
            _write_records(path, records)

            with self.assertRaises(acceptance.AcceptanceError):
                acceptance.verify_acceptance_evidence(
                    path,
                    _session(),
                    VERSION,
                )

    def test_complete_evidence_revalidates_all_six_gates(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            _record_complete_evidence(path)
            result = acceptance.verify_acceptance_evidence(
                path, _session(), VERSION
            )

        self.assertEqual(result["session_id"], _session().session_id)
        self.assertEqual(result["version"], VERSION)
        self.assertEqual(result["passed_gates"], list(
            acceptance.REQUIRED_GATES
        ))
        self.assertEqual(result["update_cycles"], 3)
        self.assertEqual(result["checkpoint_generations"], [21, 22, 23])

    def test_missing_gate_or_gate_two_aggregate_blocks_completion(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            _record_complete_evidence(path)
            records = _read_records(path)

            for missing_gate in acceptance.REQUIRED_GATES:
                with self.subTest(missing_gate=missing_gate):
                    without_gate_pass = [
                        record for record in records
                        if not (
                            record["gate"] == missing_gate
                            and record["status"] == "PASS"
                        )
                    ]
                    _write_records(path, without_gate_pass)
                    with self.assertRaises(acceptance.AcceptanceError):
                        acceptance.verify_acceptance_evidence(
                            path, _session(), VERSION
                        )

            without_aggregate = [
                record for record in records
                if not (
                    record["gate"] == "three-update-cycles"
                    and record["status"] == "PASS"
                )
            ]
            _write_records(path, without_aggregate)
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "aggregate PASS"
            ):
                acceptance.verify_acceptance_evidence(
                    path, _session(), VERSION
                )

    def test_out_of_order_gates_or_duplicate_checkpoint_are_rejected(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            _record_complete_evidence(path)
            original = _read_records(path)

            reordered = copy.deepcopy(original)
            gate_three = next(
                index for index, record in enumerate(reordered)
                if record["gate"] == "interrupted-upload"
            )
            gate_four = next(
                index for index, record in enumerate(reordered)
                if record["gate"] == "chord-rom-recovery"
            )
            reordered[gate_three], reordered[gate_four] = (
                reordered[gate_four], reordered[gate_three]
            )
            _write_records(path, reordered)
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "out-of-order"
            ):
                acceptance.verify_acceptance_evidence(
                    path, _session(), VERSION
                )

            duplicate_checkpoint = copy.deepcopy(original)
            checkpoint = next(
                record for record in duplicate_checkpoint
                if record["status"] == "CHECKPOINT"
            )
            duplicate_checkpoint.insert(2, copy.deepcopy(checkpoint))
            _write_records(path, duplicate_checkpoint)
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "exactly three checkpoints"
            ):
                acceptance.verify_acceptance_evidence(
                    path, _session(), VERSION
                )

    def test_any_failure_or_duplicate_pass_blocks_completion(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            _record_complete_evidence(path)
            acceptance.record_gate(
                path,
                _session(),
                "power-state-audit",
                "FAIL",
                {
                    "error": "operator_gate_failed",
                    "phase": "power_audit",
                },
            )
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "recorded failure"
            ):
                acceptance.verify_acceptance_evidence(
                    path, _session(), VERSION
                )

            records = _read_records(path)
            records = [
                record for record in records
                if record["status"] != "FAIL"
            ]
            duplicate = next(
                record for record in records
                if record["gate"] == "power-state-audit"
            )
            records.append(copy.deepcopy(duplicate))
            _write_records(path, records)
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "exactly one PASS"
            ):
                acceptance.verify_acceptance_evidence(
                    path, _session(), VERSION
                )

    def test_tampered_checkpoint_sequence_or_version_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            _record_complete_evidence(path)
            original = _read_records(path)

            reordered = copy.deepcopy(original)
            indexes = [
                index for index, record in enumerate(reordered)
                if record["gate"] == "three-update-cycles"
                and record["status"] == "CHECKPOINT"
            ]
            reordered[indexes[0]], reordered[indexes[1]] = (
                reordered[indexes[1]], reordered[indexes[0]]
            )
            _write_records(path, reordered)
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "checkpoint order"
            ):
                acceptance.verify_acceptance_evidence(
                    path, _session(), VERSION
                )

            wrong_version = copy.deepcopy(original)
            for record in wrong_version:
                if record["gate"] == "three-update-cycles":
                    record["facts"]["candidate_version"] = (
                        "0.64.77-badge-defcon34"
                    )
                    if record["status"] == "CHECKPOINT":
                        record["facts"]["pre_snapshot"][
                            "candidate_version"
                        ] = "0.64.77-badge-defcon34"
                        record["facts"]["snapshot"]["version"] = (
                            "0.64.77-badge-defcon34"
                        )
                    else:
                        record["facts"]["snapshot"]["version"] = (
                            "0.64.77-badge-defcon34"
                        )
            _write_records(path, wrong_version)
            with self.assertRaisesRegex(
                acceptance.AcceptanceError,
                "expected version|candidate artifact",
            ):
                acceptance.verify_acceptance_evidence(
                    path, _session(), VERSION
                )

    def test_interrupted_snapshots_and_cycle_aggregate_are_linked(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            _record_complete_evidence(path)
            original = _read_records(path)

            stale_baseline = copy.deepcopy(original)
            interrupted = next(
                record for record in stale_baseline
                if record["gate"] == "interrupted-upload"
            )
            interrupted["facts"]["baseline_snapshot"]["version"] = (
                "0.64.75-badge-defcon34"
            )
            _write_records(path, stale_baseline)
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "version mismatch"
            ):
                acceptance.verify_acceptance_evidence(
                    path, _session(), VERSION
                )

            changed_partition = copy.deepcopy(original)
            interrupted = next(
                record for record in changed_partition
                if record["gate"] == "interrupted-upload"
            )
            interrupted["facts"]["recovered_snapshot"][
                "running_partition"
            ] = "ota_0"
            _write_records(path, changed_partition)
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "prior partition"
            ):
                acceptance.verify_acceptance_evidence(
                    path, _session(), VERSION
                )

            wrong_generations = copy.deepcopy(original)
            aggregate = next(
                record for record in wrong_generations
                if record["gate"] == "three-update-cycles"
                and record["status"] == "PASS"
            )
            aggregate["facts"]["checkpoint_generations"] = [21, 22, 24]
            _write_records(path, wrong_generations)
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "do not match"
            ):
                acceptance.verify_acceptance_evidence(
                    path, _session(), VERSION
                )

            wrong_snapshot = copy.deepcopy(original)
            aggregate = next(
                record for record in wrong_snapshot
                if record["gate"] == "three-update-cycles"
                and record["status"] == "PASS"
            )
            aggregate["facts"]["snapshot"][
                "usb_responses_completed"
            ] += 1
            _write_records(path, wrong_snapshot)
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "does not match cycle 3"
            ):
                acceptance.verify_acceptance_evidence(
                    path, _session(), VERSION
                )

    def test_unknown_record_field_is_rejected_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            _record_complete_evidence(path)
            records = _read_records(path)
            records[0]["operator_notes"] = "looks good"
            _write_records(path, records)
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "record schema"
            ):
                acceptance.verify_acceptance_evidence(
                    path, _session(), VERSION
                )

    def test_serialized_cycle_pre_health_is_fully_revalidated(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            _record_complete_evidence(path)
            original = _read_records(path)
            checkpoint_index = next(
                index for index, record in enumerate(original)
                if record["status"] == "CHECKPOINT"
            )
            mutations = (
                ("running_partition", "factory"),
                ("usb_task_heartbeat_age_s", "not-an-int"),
                ("usb_last_response_age_s", -99),
                ("usb_responses_completed", 0),
                ("usb_required_response_failures", False),
            )
            for key, value in mutations:
                with self.subTest(key=key):
                    records = copy.deepcopy(original)
                    records[checkpoint_index]["facts"]["pre_snapshot"][
                        key
                    ] = value
                    _write_records(path, records)
                    with self.assertRaises(acceptance.AcceptanceError):
                        acceptance.verify_acceptance_evidence(
                            path, _session(), VERSION
                        )

    def test_record_schema_types_timestamp_and_duplicate_keys_fail_closed(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            _record_complete_evidence(path)
            original = _read_records(path)

            mutations = (
                ("boolean schema", "schema", True, "invalid schema"),
                (
                    "invalid timestamp",
                    "timestamp_utc",
                    "2026-99-99T99:99:99Z",
                    "invalid timestamp",
                ),
                ("unknown status", "status", "OK", "invalid status"),
                ("non-object facts", "facts", [], "invalid facts"),
            )
            for name, key, value, message in mutations:
                with self.subTest(name=name):
                    records = copy.deepcopy(original)
                    records[0][key] = value
                    _write_records(path, records)
                    with self.assertRaisesRegex(
                        acceptance.AcceptanceError, message
                    ):
                        acceptance.verify_acceptance_evidence(
                            path, _session(), VERSION
                        )

            _write_records(path, original)
            lines = path.read_text(encoding="utf-8").splitlines()
            lines[0] = lines[0][:-1] + ',"schema":1}'
            path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "duplicate JSON key"
            ):
                acceptance.verify_acceptance_evidence(
                    path, _session(), VERSION
                )

    def test_audit_does_not_create_missing_file_and_rejects_symlink(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            missing = root / "missing.jsonl"
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "open existing evidence"
            ):
                acceptance.verify_acceptance_evidence(
                    missing, _session(), VERSION
                )
            self.assertFalse(missing.exists())

            target = root / "target.jsonl"
            link = root / "link.jsonl"
            _record_complete_evidence(target)
            link.symlink_to(target)
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "open existing evidence"
            ):
                acceptance.verify_acceptance_evidence(
                    link, _session(), VERSION
                )

            fifo = root / "evidence.fifo"
            os.mkfifo(fifo)
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "regular file"
            ):
                acceptance.verify_acceptance_evidence(
                    fifo, _session(), VERSION
                )

    def test_audit_rejects_path_swap_immediately_after_shared_lock(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "acceptance.jsonl"
            detached_path = root / "detached.jsonl"
            _record_complete_evidence(evidence_path)
            real_flock = acceptance.fcntl.flock
            swapped = False

            def swap_after_lock(fd: int, operation: int) -> None:
                nonlocal swapped
                real_flock(fd, operation)
                if operation == acceptance.fcntl.LOCK_SH and not swapped:
                    evidence_path.rename(detached_path)
                    evidence_path.write_bytes(b"")
                    os.chmod(evidence_path, 0o600)
                    swapped = True

            with mock.patch.object(
                acceptance.fcntl,
                "flock",
                side_effect=swap_after_lock,
            ), self.assertRaisesRegex(
                acceptance.AcceptanceError, "stable"
            ):
                acceptance.verify_acceptance_evidence(
                    evidence_path, _session(), VERSION
                )

            self.assertTrue(swapped)
            self.assertEqual(evidence_path.read_bytes(), b"")

    def test_audit_revalidates_path_immediately_before_success(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "acceptance.jsonl"
            detached_path = root / "detached.jsonl"
            _record_complete_evidence(evidence_path)
            real_verify = acceptance._verify_acceptance_records

            def swap_after_verification(
                records: list[dict],
                session: acceptance.BadgeAcceptanceSession,
                expected_version: str,
            ) -> dict[str, object]:
                result = real_verify(records, session, expected_version)
                evidence_path.rename(detached_path)
                evidence_path.write_bytes(b"")
                os.chmod(evidence_path, 0o600)
                return result

            with mock.patch.object(
                acceptance,
                "_verify_acceptance_records",
                side_effect=swap_after_verification,
            ), self.assertRaisesRegex(
                acceptance.AcceptanceError, "stable"
            ):
                acceptance.verify_acceptance_evidence(
                    evidence_path, _session(), VERSION
                )

            self.assertEqual(evidence_path.read_bytes(), b"")

    def test_audit_rechecks_reservation_immediately_before_success(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "acceptance.jsonl"
            _record_complete_evidence(evidence_path)
            reservation_path = root / (
                f".{evidence_path.name}.mutating-gate-reservation"
            )
            real_verify = acceptance._verify_acceptance_records

            def reserve_after_verification(
                records: list[dict],
                session: acceptance.BadgeAcceptanceSession,
                expected_version: str,
            ) -> dict[str, object]:
                result = real_verify(records, session, expected_version)
                reservation_path.write_text("{}\n", encoding="utf-8")
                os.chmod(reservation_path, 0o600)
                return result

            with mock.patch.object(
                acceptance,
                "_verify_acceptance_records",
                side_effect=reserve_after_verification,
            ), self.assertRaisesRegex(
                acceptance.AcceptanceError, "reservation"
            ):
                acceptance.verify_acceptance_evidence(
                    evidence_path, _session(), VERSION
                )

            self.assertTrue(reservation_path.is_file())

    def test_audit_revalidates_path_after_final_reservation_check(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "acceptance.jsonl"
            detached_path = root / "detached.jsonl"
            _record_complete_evidence(evidence_path)
            real_check = acceptance._require_no_mutating_gate_reservation
            checks = 0

            def swap_after_final_reservation_check(path: Path) -> None:
                nonlocal checks
                real_check(path)
                checks += 1
                if checks == 2:
                    evidence_path.rename(detached_path)
                    evidence_path.write_bytes(b"")
                    os.chmod(evidence_path, 0o600)

            with mock.patch.object(
                acceptance,
                "_require_no_mutating_gate_reservation",
                side_effect=swap_after_final_reservation_check,
            ), self.assertRaisesRegex(
                acceptance.AcceptanceError, "stable"
            ):
                acceptance.verify_acceptance_evidence(
                    evidence_path, _session(), VERSION
                )

            self.assertEqual(checks, 2)
            self.assertEqual(evidence_path.read_bytes(), b"")

    def test_unallowlisted_reboot_reason_cannot_hide_private_text(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            _record_complete_evidence(path)
            records = _read_records(path)
            chord_record = next(
                record for record in records
                if record["gate"] == "chord-rom-recovery"
            )
            chord_record["facts"]["snapshot"][
                "last_expected_reboot_reason"
            ] = "GameChangersAI-67"
            _write_records(path, records)
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "reboot reason"
            ):
                acceptance.verify_acceptance_evidence(
                    path, _session(), VERSION
                )

    def test_audit_links_gate_one_scanner_versions_to_cycle_one(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            _record_complete_evidence(path)
            records = _read_records(path)
            cycle_one = next(
                record for record in records
                if record["gate"] == "three-update-cycles"
                and record["status"] == "CHECKPOINT"
                and record["facts"]["cycle"] == 1
            )
            cycle_one["facts"]["pre_snapshot"][
                "ble_version"
            ] = "0.64.74-badge-defcon34"
            _write_records(path, records)

            with self.assertRaisesRegex(
                acceptance.AcceptanceError,
                "Gate 1",
            ):
                acceptance.verify_acceptance_evidence(
                    path, _session(), VERSION
                )

    def test_audit_rejects_mixed_candidate_artifacts_across_mutating_gates(
        self,
    ) -> None:
        for target_gate, target_status in (
            ("three-update-cycles", "CHECKPOINT"),
            ("interrupted-upload", "PASS"),
            ("chord-rom-recovery", "PASS"),
        ):
            with self.subTest(gate=target_gate), \
                    tempfile.TemporaryDirectory() as td:
                path = Path(td) / "acceptance.jsonl"
                _record_complete_evidence(path)
                records = _read_records(path)
                target = next(
                    record for record in records
                    if record["gate"] == target_gate
                    and record["status"] == target_status
                )
                target["facts"]["candidate_artifacts"]["scanner"][
                    "content_set_sha256"
                ] = "f" * 64
                _write_records(path, records)

                with self.assertRaisesRegex(
                    acceptance.AcceptanceError,
                    "candidate artifacts",
                ):
                    acceptance.verify_acceptance_evidence(
                        path,
                        _session(),
                        VERSION,
                    )


class RetainedStateRootTest(unittest.TestCase):
    def test_macos_default_uses_trusted_home_application_support_namespace(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            untrusted_home = Path(td).resolve()
            trusted_home = Path(
                pwd.getpwuid(os.geteuid()).pw_dir
            ).resolve(strict=True)
            with mock.patch.object(
                acceptance.sys, "platform", "darwin"
            ), mock.patch.dict(
                os.environ, {"HOME": str(untrusted_home)}
            ):
                anchor, components = acceptance._retained_state_root_spec()
                registry_anchor, require_pinned_parent = (
                    acceptance._operation_registry_anchor_spec()
                )

        self.assertEqual(anchor, trusted_home)
        self.assertEqual(registry_anchor, trusted_home)
        self.assertIs(require_pinned_parent, True)
        self.assertNotEqual(anchor, untrusted_home)
        self.assertEqual(components, (
            "Library",
            "Application Support",
            "FoF Badge Flasher",
            "acceptance-state",
        ))


class AcceptanceCliTest(unittest.TestCase):
    def setUp(self) -> None:
        self._retained_state_td = tempfile.TemporaryDirectory()
        self._retained_state_anchor = Path(
            self._retained_state_td.name
        ).resolve()
        os.chmod(self._retained_state_anchor, 0o700)
        self._retained_state_components = ("app-parent", "fixed-state")
        self._retained_state_patch = mock.patch.object(
            acceptance,
            "_retained_state_root_spec",
            return_value=(
                self._retained_state_anchor,
                self._retained_state_components,
            ),
            create=True,
        )
        self._retained_state_patch.start()
        self._operation_registry_patch = mock.patch.object(
            acceptance,
            "_operation_registry_anchor_spec",
            return_value=(self._retained_state_anchor, False),
            create=True,
        )
        self._operation_registry_patch.start()
        self._repo_version_patch = mock.patch.object(
            acceptance.flash,
            "repo_version",
            return_value=VERSION,
        )
        self._repo_version_patch.start()
        self._require_artifacts_patch = mock.patch.object(
            acceptance.flash,
            "require_artifacts",
        )
        self._require_artifacts_patch.start()
        self._freeze_artifacts_patch = mock.patch.object(
            acceptance.flash,
            "_prepare_frozen_usb_firmware_artifacts",
            side_effect=lambda *_args, **_kwargs:
                _frozen_candidate_artifacts(),
        )
        self._freeze_artifacts_patch.start()
        self._trusted_descriptor_patch = mock.patch.object(
            acceptance,
            "_trusted_session_uplink_descriptor",
            side_effect=lambda port, _session_value: _usb_record(port),
        )
        self._trusted_descriptor_patch.start()
        default_cycle_status = _cycle_source_status(1)
        self._probe_application_patch = mock.patch.object(
            acceptance.flash,
            "probe_application",
            side_effect=lambda *_args, **_kwargs:
                copy.deepcopy(default_cycle_status),
        )
        self._probe_application_patch.start()
        self._capture_updater_baseline_patch = mock.patch.object(
            acceptance,
            "_capture_reserved_v078_updater_baseline",
            side_effect=lambda *_args, **_kwargs:
                LivePromotionMetricsTest._issued_baseline(),
        )
        self._capture_updater_baseline_patch.start()

    def tearDown(self) -> None:
        for registry_path in self._retained_state_anchor.rglob(
            acceptance.OPERATION_REGISTRY_NAME
        ):
            registry_fd = os.open(registry_path, os.O_RDONLY)
            try:
                registry_flags = os.fstat(registry_fd).st_flags
                acceptance._set_operation_registry_file_flags(
                    registry_fd,
                    registry_flags & ~stat.UF_APPEND,
                )
            finally:
                os.close(registry_fd)
        self._operation_registry_patch.stop()
        self._retained_state_patch.stop()
        self._capture_updater_baseline_patch.stop()
        self._probe_application_patch.stop()
        self._trusted_descriptor_patch.stop()
        self._freeze_artifacts_patch.stop()
        self._require_artifacts_patch.stop()
        self._repo_version_patch.stop()
        self._retained_state_td.cleanup()

    def _retained_state_root(self) -> Path:
        return self._retained_state_anchor.joinpath(
            *self._retained_state_components
        )

    def _retained_markers(self) -> list[Path]:
        root = self._retained_state_root()
        return sorted(root.glob("op-*.retained")) if root.exists() else []

    def _operation_registry_path(self) -> Path:
        return (
            self._retained_state_anchor
            / acceptance.OPERATION_REGISTRY_NAME
        )

    def _operation_registry_records(self) -> list[dict]:
        return [
            json.loads(line)
            for line in self._operation_registry_path().read_text(
                encoding="utf-8"
            ).splitlines()
        ]

    def _cycle_one_operation_sha256(self) -> str:
        return acceptance._operation_identity_sha256(
            _session(),
            VERSION,
            _candidate_artifacts(),
            "three-update-cycles",
            "update_cycle_1",
            1,
        )

    def _write_legacy_operation_registry(
        self,
        records: list[dict],
    ) -> None:
        registry_path = self._operation_registry_path()
        encoded = "".join(
            json.dumps(
                record,
                separators=(",", ":"),
                sort_keys=True,
            ) + "\n"
            for record in [
                {
                    "kind": "REGISTRY",
                    "schema": acceptance.OPERATION_REGISTRY_SCHEMA,
                },
                *records,
            ]
        ).encode("utf-8")
        registry_fd = os.open(
            registry_path,
            os.O_RDWR | os.O_CREAT | os.O_EXCL,
            0o600,
        )
        try:
            acceptance._write_one_operation_registry_record(
                registry_fd, encoded
            )
            acceptance._durably_sync_operation_registry_file(
                registry_fd
            )
            flags = os.fstat(registry_fd).st_flags
            acceptance._set_operation_registry_file_flags(
                registry_fd,
                flags | stat.UF_APPEND,
            )
            acceptance._durably_sync_operation_registry_file(
                registry_fd
            )
        finally:
            os.close(registry_fd)
        parent_fd = os.open(
            self._retained_state_anchor,
            os.O_RDONLY | os.O_DIRECTORY,
        )
        try:
            os.fsync(parent_fd)
        finally:
            os.close(parent_fd)

    def _registry_fences(self) -> list[dict]:
        return [
            record
            for record in self._operation_registry_records()[1:]
            if record.get("event") == "ARMING_PROTOCOL_FENCE"
        ]

    def test_cycle_cli_records_machine_checkpoints_then_aggregate(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            acceptance.record_gate(
                evidence_path,
                _session(),
                "android-control-reconnect",
                "PASS",
                _pass_facts("android-control-reconnect"),
            )
            checkpoints = [
                _verify_cycle_fixture(cycle)
                for cycle in (1, 2, 3)
            ]
            with mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                side_effect=checkpoints,
            ) as run_cycle, mock.patch.object(
                acceptance.flash,
                "probe_application",
                side_effect=[
                    _cycle_source_status(1),
                    {
                        **_cycle_source_status(2),
                        "uptime_s": 21,
                        "usb_health": {
                            **_cycle_source_status(2)["usb_health"],
                            "rx_bytes": 2034,
                            "valid_commands": 43,
                            "responses_completed": 44,
                        },
                    },
                    {
                        **_cycle_source_status(3),
                        "uptime_s": 21,
                        "usb_health": {
                            **_cycle_source_status(3)["usb_health"],
                            "rx_bytes": 3034,
                            "valid_commands": 53,
                            "responses_completed": 54,
                        },
                    },
                ],
            ):
                results = [
                    acceptance.main([
                        "--gate", "three-update-cycles",
                        "--cycle", str(cycle),
                        "--port", "/dev/live-badge",
                        "--session-file", str(session_path),
                        "--evidence", str(evidence_path),
                    ])
                    for cycle in (1, 2, 3)
                ]
            records = [
                json.loads(line)
                for line in evidence_path.read_text(
                    encoding="utf-8"
                ).splitlines()
            ]
            retained_markers = self._retained_markers()
        self.assertEqual(results, [0, 0, 0])
        self.assertEqual(run_cycle.call_count, 3)
        self.assertEqual(len(retained_markers), 3)
        self.assertEqual(
            [
                record["status"] for record in records
                if record["gate"] == "three-update-cycles"
            ],
            ["CHECKPOINT", "CHECKPOINT", "CHECKPOINT", "PASS"],
        )

    def test_cycle_candidate_version_drift_stops_before_artifacts_or_registry(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            acceptance.record_gate(
                evidence_path,
                _session(),
                "android-control-reconnect",
                "PASS",
                _pass_facts("android-control-reconnect"),
            )
            argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/live-badge",
                "--session-file", str(session_path),
                "--evidence", str(evidence_path),
            ]
            with mock.patch.object(
                acceptance.flash,
                "repo_version",
                return_value="0.64.77-badge-defcon34",
            ), mock.patch.object(
                acceptance.flash, "require_artifacts"
            ) as require_artifacts, mock.patch.object(
                acceptance.flash, "_prepare_frozen_usb_firmware_artifacts"
            ) as freeze_artifacts, mock.patch.object(
                acceptance, "_open_operation_registry"
            ) as open_registry, mock.patch.object(
                acceptance, "run_update_cycle_checkpoint"
            ) as run_cycle, contextlib.redirect_stderr(io.StringIO()):
                result = acceptance.main(argv)

        self.assertEqual(result, 2)
        require_artifacts.assert_not_called()
        freeze_artifacts.assert_not_called()
        open_registry.assert_not_called()
        run_cycle.assert_not_called()

    def test_cycle_changed_candidate_bytes_stop_before_registry_or_hardware(
        self,
    ) -> None:
        scanner_image = bytearray(_scanner_firmware_image())
        scanner_image[-1] ^= 0x01
        changed = acceptance.flash.FrozenUsbFirmwareArtifacts(
            uplink=_frozen_artifacts(_uplink_firmware_image()),
            scanner=_frozen_artifacts(bytes(scanner_image)),
        )
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            acceptance.record_gate(
                evidence_path,
                _session(),
                "android-control-reconnect",
                "PASS",
                _pass_facts("android-control-reconnect"),
            )
            argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/live-badge",
                "--session-file", str(session_path),
                "--evidence", str(evidence_path),
            ]
            with mock.patch.object(
                acceptance.flash, "repo_version", return_value=VERSION
            ), mock.patch.object(
                acceptance.flash, "require_artifacts"
            ), mock.patch.object(
                acceptance.flash,
                "_prepare_frozen_usb_firmware_artifacts",
                return_value=changed,
            ), mock.patch.object(
                acceptance, "_open_operation_registry"
            ) as open_registry, mock.patch.object(
                acceptance, "run_update_cycle_checkpoint"
            ) as run_cycle, contextlib.redirect_stderr(io.StringIO()):
                result = acceptance.main(argv)

        self.assertEqual(result, 2)
        open_registry.assert_not_called()
        run_cycle.assert_not_called()

    def test_cycle_changed_live_scanner_state_stops_before_registry(
        self,
    ) -> None:
        changed_status = _cycle_source_status(1)
        for scanner in changed_status["scanners"]:
            scanner["ver"] = "0.64.74-badge-defcon34"
        descriptor = _usb_record("/dev/live-badge")
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            acceptance.record_gate(
                evidence_path,
                _session(),
                "android-control-reconnect",
                "PASS",
                _pass_facts("android-control-reconnect"),
            )
            argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/live-badge",
                "--session-file", str(session_path),
                "--evidence", str(evidence_path),
            ]
            with mock.patch.object(
                acceptance.flash, "repo_version", return_value=VERSION
            ), mock.patch.object(
                acceptance.flash, "require_artifacts"
            ), mock.patch.object(
                acceptance.flash,
                "_prepare_frozen_usb_firmware_artifacts",
                return_value=_frozen_candidate_artifacts(),
            ), mock.patch.object(
                acceptance,
                "_trusted_session_uplink_descriptor",
                return_value=descriptor,
            ), mock.patch.object(
                acceptance.flash,
                "probe_application",
                return_value=changed_status,
            ), mock.patch.object(
                acceptance, "_open_operation_registry"
            ) as open_registry, mock.patch.object(
                acceptance, "run_update_cycle_checkpoint"
            ) as run_cycle, contextlib.redirect_stderr(io.StringIO()):
                result = acceptance.main(argv)

        self.assertEqual(result, 2)
        open_registry.assert_not_called()
        run_cycle.assert_not_called()

    def test_machine_chord_runner_uses_retained_rom_binding_and_machine_facts(
        self,
    ) -> None:
        frozen = _frozen_candidate_artifacts()
        flow_result = _usb_cycle_result(2)
        rom_status = _status(
            reboot_reason="button_usb_rom",
            partition="ota_1",
            reboot_generation=0,
            responses_completed=30,
        )
        rom_status["uptime_s"] = 5
        rom_status["usb_health"]["rx_bytes"] = 1500
        rom_status["usb_health"]["valid_commands"] = 30
        flow_result.pre_stage_status = copy.deepcopy(rom_status)
        flow_result.final_restored_status.update({
            "last_expected_reboot_reason": "update_finish",
            "last_expected_reboot_generation": 3,
            "running_partition": "ota_0",
        })
        first_maintenance = _maintenance_status(
            version=CANARY_VERSION,
            update_session="0123456789ABCDEF",
            partition="ota_1",
            reboot_generation=1,
            responses_completed=1,
        )
        final_maintenance = _maintenance_status(
            version=CANARY_VERSION,
            update_session="0123456789ABCDEF",
            partition="ota_0",
            reboot_generation=2,
            responses_completed=1,
        )
        observed_args: list[object] = []
        callback_events: list[str] = []

        def usb_flow(
            args: object,
            platform: dict,
            need_uplink: bool,
            slots: list[str],
            version: str,
            *,
            maintenance_status_validator=None,
            post_rom_bootstrap_status_validator=None,
            frozen_artifacts=None,
        ) -> object:
            observed_args.append(args)
            self.assertIs(
                platform,
                acceptance.flash.PLATFORMS[CANARY_PLATFORM_KEY],
            )
            self.assertIs(need_uplink, True)
            self.assertEqual(slots, ["ble", "wifi"])
            self.assertEqual(version, VERSION)
            self.assertIs(frozen_artifacts, frozen)
            self.assertEqual(args.port, "/dev/current-rom")
            self.assertIs(args.bind_selected_uplink, False)
            self.assertIs(args.recovery_rewrite_same_version, True)
            self.assertIs(args.skip_command_probe, False)
            self.assertIs(args.require_rom_recovery, True)
            self.assertEqual(
                args.trusted_uplink_binding,
                acceptance.flash.TrustedUplinkBinding(
                    serial_number=UPLINK_ID,
                    location=None,
                    source="retained-session",
                ),
            )
            self.assertTrue(callable(post_rom_bootstrap_status_validator))
            self.assertTrue(callable(maintenance_status_validator))
            post_rom_bootstrap_status_validator(
                copy.deepcopy(rom_status)
            )
            callback_events.append("rom")
            maintenance_status_validator(
                copy.deepcopy(first_maintenance),
                "0123456789ABCDEF",
            )
            callback_events.append("maintenance-first")
            maintenance_status_validator(
                copy.deepcopy(final_maintenance),
                "0123456789ABCDEF",
            )
            callback_events.append("maintenance-final")
            return flow_result

        with mock.patch.object(
            acceptance.flash, "repo_version", return_value=VERSION
        ), mock.patch.object(
            acceptance.flash, "require_artifacts"
        ), mock.patch.object(
            acceptance.flash, "usb_flow", side_effect=usb_flow
        ), mock.patch.object(
            acceptance.flash,
            "_revalidate_usb_scanner_flow_result",
            return_value=flow_result,
        ), mock.patch.object(
            acceptance,
            "_trusted_session_uplink_descriptor",
            side_effect=AssertionError(
                "descriptor access must remain inside the frozen USB flow"
            ),
        ):
            runner = getattr(
                acceptance,
                "run_chord_rom_recovery_gate",
                lambda *_args, **_kwargs: None,
            )
            facts = runner(
                "/dev/current-rom",
                _session(),
                expected_version=VERSION,
                frozen_artifacts=frozen,
                candidate_artifacts=_candidate_artifacts(),
                updater_baseline=(
                    LivePromotionMetricsTest._issued_baseline()
                ),
            )

        self.assertEqual(len(observed_args), 1)
        self.assertEqual(
            callback_events,
            ["rom", "maintenance-first", "maintenance-final"],
        )
        self.assertEqual(
            facts["rom_boot_snapshot"]["reboot_generation"],
            0,
        )
        self.assertEqual(
            facts["rom_boot_snapshot"]["last_expected_reboot_reason"],
            "button_usb_rom",
        )
        self.assertEqual(
            facts["snapshot"]["last_expected_reboot_reason"],
            "update_finish",
        )
        self.assertEqual(
            facts["last_expected_reboot_reason"],
            "button_usb_rom",
        )
        self.assertTrue(facts["scanner_staged_once"])
        self.assertTrue(facts["both_uart_updates"])

    def test_machine_chord_cli_records_one_reserved_pass(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            _record_gate_three(evidence_path)
            facts = _pass_facts("chord-rom-recovery")
            argv = [
                "--gate", "chord-rom-recovery",
                "--run-chord-rom-recovery",
                "--port", "/dev/current-rom",
                "--session-file", str(session_path),
                "--evidence", str(evidence_path),
            ]
            with mock.patch.object(
                acceptance,
                "run_chord_rom_recovery_gate",
                return_value=facts,
                create=True,
            ) as runner:
                try:
                    result: object = acceptance.main(argv)
                except SystemExit as exc:
                    result = ("system-exit", exc.code)
            records = _read_records(evidence_path)
            retained_markers = self._retained_markers()

        self.assertEqual(result, 0)
        runner.assert_called_once()
        call_args, call_kwargs = runner.call_args
        self.assertEqual(call_args, ("/dev/current-rom", _session()))
        self.assertEqual(call_kwargs["expected_version"], VERSION)
        self.assertEqual(
            call_kwargs["candidate_artifacts"],
            _candidate_artifacts(),
        )
        self.assertIs(
            type(call_kwargs["frozen_artifacts"]),
            acceptance.flash.FrozenUsbFirmwareArtifacts,
        )
        self.assertEqual(len(retained_markers), 1)
        self.assertEqual(
            [
                (record["gate"], record["status"])
                for record in records[-2:]
            ],
            [
                ("interrupted-upload", "PASS"),
                ("chord-rom-recovery", "PASS"),
            ],
        )
        self.assertEqual(
            records[-1]["facts"]["last_expected_reboot_reason"],
            "button_usb_rom",
        )

    def test_manual_chord_pass_is_rejected_before_evidence_or_hardware(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            facts_path = root / "facts.json"
            _record_gate_three(evidence_path)
            facts = dict(_pass_facts("chord-rom-recovery"))
            facts.pop("snapshot")
            facts_path.write_text(
                json.dumps(facts),
                encoding="utf-8",
            )
            evidence_before = evidence_path.read_bytes()
            badge = mock.MagicMock()
            badge.__enter__.return_value = badge
            badge.__exit__.return_value = None
            badge.status.return_value = _status(
                reboot_reason="button_usb_rom"
            )
            errors = io.StringIO()
            argv = [
                "--gate", "chord-rom-recovery",
                "--port", "/dev/must-not-open",
                "--session-file", str(session_path),
                "--facts-file", str(facts_path),
                "--expected-version", VERSION,
                "--evidence", str(evidence_path),
            ]
            with contextlib.redirect_stderr(
                errors
            ), mock.patch.object(
                acceptance,
                "_trusted_session_uplink_descriptor",
                return_value=_usb_record("/dev/must-not-open"),
            ) as resolve_descriptor, mock.patch.object(
                acceptance.flash,
                "BadgeSerial",
                return_value=badge,
            ) as badge_serial:
                result = acceptance.main(argv)

            evidence_after = evidence_path.read_bytes()

        self.assertEqual(result, 2)
        self.assertIn("--run-chord-rom-recovery", errors.getvalue())
        self.assertEqual(evidence_after, evidence_before)
        resolve_descriptor.assert_not_called()
        badge_serial.assert_not_called()
        self.assertEqual(self._retained_markers(), [])

    def test_cli_caught_errors_scrub_every_mac_rendering(self) -> None:
        raw_hardware_ids = (
            "AA:BB:CC:DD:EE:01",
            "aa-bb-cc-dd-ee-02",
            "AAbb.cCdD.ee03",
            "001122334404",
        )
        message = "failure " + " ".join(raw_hardware_ids)
        argv = [
            "--gate", "power-state-audit",
            "--status", "FAIL",
            "--session-file", "/must-not-read/session.json",
            "--facts-file", "/must-not-read/facts.json",
        ]
        for error in (
            acceptance.AcceptanceError(message),
            acceptance.flash.FlashError(message),
            OSError(message),
        ):
            with self.subTest(error=type(error).__name__):
                stdout = io.StringIO()
                stderr = io.StringIO()
                with contextlib.redirect_stdout(
                    stdout
                ), contextlib.redirect_stderr(
                    stderr
                ), mock.patch.object(
                    acceptance,
                    "_load_json_object",
                    side_effect=error,
                ):
                    result = acceptance.main(argv)

                self.assertEqual(result, 2)
                self.assertEqual(stdout.getvalue(), "")
                for raw in raw_hardware_ids:
                    self.assertNotIn(raw, stderr.getvalue())
                self.assertEqual(
                    stderr.getvalue().count("[hardware-id]"),
                    len(raw_hardware_ids),
                )

    def test_cli_has_no_file_path_that_can_promote_an_updater_baseline(
        self,
    ) -> None:
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(
            SystemExit
        ) as raised:
            acceptance.parse_args([
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/must-not-open",
                "--session-file", "/must-not-read/session.json",
                "--updater-baseline-file", "/must-not-read/baseline.json",
            ])
        self.assertEqual(raised.exception.code, 2)

    def test_canary_reservation_captures_v078_before_cycle_one_action(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            frozen = acceptance.flash.FrozenUsbFirmwareArtifacts(
                uplink=_frozen_artifacts(
                    _uplink_firmware_image(version=CANARY_VERSION)
                ),
                scanner=_frozen_artifacts(
                    _scanner_firmware_image(version=CANARY_VERSION)
                ),
            )
            artifacts = acceptance.verify_candidate_artifacts(
                frozen, CANARY_VERSION
            )
            source = LivePromotionMetricsTest._baseline_status(
                responses_completed=23
            )
            for scanner in source["scanners"]:
                scanner["ver"] = V078_VERSION
            gate_one = dict(_pass_facts("android-control-reconnect"))
            gate_one["snapshot"] = acceptance.verify_cycle_pre_snapshot(
                source,
                _session(),
                CANARY_VERSION,
                1,
            )
            gate_one["candidate_artifacts"] = artifacts
            acceptance.record_gate(
                evidence_path,
                _session(),
                "android-control-reconnect",
                "PASS",
                gate_one,
            )
            baseline = LivePromotionMetricsTest._issued_baseline()
            session_input = acceptance._open_private_session_input(
                session_path
            )
            events: list[str] = []

            def capture(*_args) -> object:
                events.append("capture")
                return baseline

            def action(context) -> object:
                events.append("action")
                self.assertIs(context.updater_baseline, baseline)
                return mock.sentinel.result

            def record(_reservation, value) -> None:
                events.append("record")
                self.assertIs(value, mock.sentinel.result)

            try:
                with mock.patch.object(
                    acceptance.flash,
                    "repo_version",
                    return_value=CANARY_VERSION,
                ), mock.patch.object(
                    acceptance.flash,
                    "_prepare_frozen_usb_firmware_artifacts",
                    return_value=frozen,
                ), mock.patch.object(
                    acceptance,
                    "_capture_reserved_v078_updater_baseline",
                    side_effect=capture,
                ):
                    result = acceptance._run_reserved_mutating_gate(
                        evidence_path,
                        _session(),
                        session_input,
                        "three-update-cycles",
                        "update_cycle_1",
                        action,
                        record,
                        cycle=1,
                        updater_baseline_port="/dev/v078-live",
                        pre_action_validator=(
                            lambda _context, _records:
                            events.append("preflight")
                        ),
                    )
            finally:
                acceptance._close_private_session_input(session_input)

        self.assertIs(result, mock.sentinel.result)
        self.assertEqual(
            events,
            ["preflight", "capture", "action", "record"],
        )

    def test_machine_chord_cli_rejects_missing_gate_three_before_runner(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            _record_gate_two(evidence_path)
            argv = [
                "--gate", "chord-rom-recovery",
                "--run-chord-rom-recovery",
                "--port", "/dev/must-not-open",
                "--session-file", str(session_path),
                "--evidence", str(evidence_path),
            ]
            with contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "run_chord_rom_recovery_gate",
                create=True,
            ) as runner, mock.patch.object(
                acceptance.flash, "repo_version"
            ) as repo_version, mock.patch.object(
                acceptance.flash, "require_artifacts"
            ) as require_artifacts, mock.patch.object(
                acceptance.flash, "usb_flow"
            ) as usb_flow:
                try:
                    result: object = acceptance.main(argv)
                except SystemExit as exc:
                    result = ("system-exit", exc.code)

        self.assertEqual(result, 2)
        runner.assert_not_called()
        repo_version.assert_not_called()
        require_artifacts.assert_not_called()
        usb_flow.assert_not_called()

    def test_mutating_gates_reject_duplicate_or_out_of_order_prefixes_before_run(
        self,
    ) -> None:
        scenarios = (
            ("three-update-cycles", "duplicate", 1),
            ("three-update-cycles", "out-of-order", 1),
            ("interrupted-upload", "duplicate", None),
            ("interrupted-upload", "out-of-order", None),
        )
        for gate, corruption, cycle in scenarios:
            with self.subTest(gate=gate, corruption=corruption), \
                    tempfile.TemporaryDirectory() as td:
                root = Path(td)
                evidence_path = root / "evidence.jsonl"
                session_path = _write_private_session_anchor(root)
                if gate == "three-update-cycles":
                    _record_gate_one(evidence_path)
                else:
                    _record_gate_two(evidence_path)
                if corruption == "duplicate":
                    _append_pass_fixture(
                        evidence_path, "android-control-reconnect"
                    )
                else:
                    _append_pass_fixture(
                        evidence_path, "power-state-audit"
                    )
                argv = [
                    "--gate", gate,
                    "--port", "/dev/must-not-open",
                    "--session-file", str(session_path),
                    "--evidence", str(evidence_path),
                ]
                if cycle is not None:
                    argv.extend(("--cycle", str(cycle)))
                stderr = io.StringIO()
                with contextlib.redirect_stderr(stderr), mock.patch.object(
                    acceptance, "run_update_cycle_checkpoint"
                ) as run_cycle, mock.patch.object(
                    acceptance, "run_interrupted_upload_gate"
                ) as run_interrupted, mock.patch.object(
                    acceptance.flash, "repo_version"
                ) as repo_version, mock.patch.object(
                    acceptance.flash, "require_artifacts"
                ) as require_artifacts, mock.patch.object(
                    acceptance.flash, "usb_flow"
                ) as usb_flow, mock.patch.object(
                    acceptance.flash, "BadgeSerial"
                ) as badge_serial:
                    run_cycle.return_value = _verify_cycle_fixture(1)
                    run_interrupted.return_value = (
                        _session(), _pass_facts("interrupted-upload")
                    )
                    result = acceptance.main(argv)

                self.assertEqual(result, 2)
                run_cycle.assert_not_called()
                run_interrupted.assert_not_called()
                repo_version.assert_not_called()
                require_artifacts.assert_not_called()
                usb_flow.assert_not_called()
                badge_serial.assert_not_called()

    def test_update_cycle_must_be_the_exact_next_reserved_cycle(
        self,
    ) -> None:
        for requested_cycle in (1, 3):
            with self.subTest(cycle=requested_cycle), \
                    tempfile.TemporaryDirectory() as td:
                root = Path(td)
                evidence_path = root / "evidence.jsonl"
                session_path = _write_private_session_anchor(root)
                _record_gate_one(evidence_path)
                acceptance.record_update_cycle_checkpoint(
                    evidence_path, _session(), _verify_cycle_fixture(1)
                )
                with contextlib.redirect_stderr(
                    io.StringIO()
                ), mock.patch.object(
                    acceptance, "run_update_cycle_checkpoint"
                ) as run_cycle, mock.patch.object(
                    acceptance.flash, "repo_version"
                ) as repo_version, mock.patch.object(
                    acceptance.flash, "require_artifacts"
                ) as require_artifacts, mock.patch.object(
                    acceptance.flash, "usb_flow"
                ) as usb_flow:
                    result = acceptance.main([
                        "--gate", "three-update-cycles",
                        "--cycle", str(requested_cycle),
                        "--port", "/dev/must-not-open",
                        "--session-file", str(session_path),
                        "--evidence", str(evidence_path),
                    ])

                self.assertEqual(result, 2)
                run_cycle.assert_not_called()
                repo_version.assert_not_called()
                require_artifacts.assert_not_called()
                usb_flow.assert_not_called()

    def test_mutating_gates_reject_prior_failure_before_run(self) -> None:
        scenarios = (
            (
                "three-update-cycles",
                1,
                "three-update-cycles",
                "update_cycle_1",
            ),
            (
                "interrupted-upload",
                None,
                "interrupted-upload",
                "interrupted_upload",
            ),
        )
        for gate, cycle, failed_gate, failed_phase in scenarios:
            with self.subTest(gate=gate), tempfile.TemporaryDirectory() as td:
                root = Path(td)
                evidence_path = root / "evidence.jsonl"
                session_path = _write_private_session_anchor(root)
                if gate == "three-update-cycles":
                    _record_gate_one(evidence_path)
                else:
                    _record_gate_two(evidence_path)
                acceptance.record_gate(
                    evidence_path,
                    _session(),
                    failed_gate,
                    "FAIL",
                    {
                        "error": "hardware_gate_failed",
                        "phase": failed_phase,
                    },
                )
                argv = [
                    "--gate", gate,
                    "--port", "/dev/must-not-open",
                    "--session-file", str(session_path),
                    "--evidence", str(evidence_path),
                ]
                if cycle is not None:
                    argv.extend(("--cycle", str(cycle)))
                with contextlib.redirect_stderr(
                    io.StringIO()
                ), mock.patch.object(
                    acceptance, "run_update_cycle_checkpoint"
                ) as run_cycle, mock.patch.object(
                    acceptance, "run_interrupted_upload_gate"
                ) as run_interrupted, mock.patch.object(
                    acceptance.flash, "repo_version"
                ) as repo_version, mock.patch.object(
                    acceptance.flash, "require_artifacts"
                ) as require_artifacts, mock.patch.object(
                    acceptance.flash, "usb_flow"
                ) as usb_flow, mock.patch.object(
                    acceptance.flash, "BadgeSerial"
                ) as badge_serial:
                    run_cycle.return_value = _verify_cycle_fixture(1)
                    run_interrupted.return_value = (
                        _session(), _pass_facts("interrupted-upload")
                    )
                    result = acceptance.main(argv)

                self.assertEqual(result, 2)
                run_cycle.assert_not_called()
                run_interrupted.assert_not_called()
                repo_version.assert_not_called()
                require_artifacts.assert_not_called()
                usb_flow.assert_not_called()
                badge_serial.assert_not_called()

    def test_started_mutating_gate_failure_is_permanent_fixed_code_evidence(
        self,
    ) -> None:
        scenarios = (
            ("three-update-cycles", 1, "update_cycle_1"),
            ("interrupted-upload", None, "interrupted_upload"),
        )
        for gate, cycle, phase in scenarios:
            with self.subTest(gate=gate), tempfile.TemporaryDirectory() as td:
                root = Path(td)
                evidence_path = root / "evidence.jsonl"
                reservation_path = root / (
                    f".{evidence_path.name}.mutating-gate-reservation"
                )
                session_path = _write_private_session_anchor(root)
                if gate == "three-update-cycles":
                    _record_gate_one(evidence_path)
                else:
                    _record_gate_two(evidence_path)
                argv = [
                    "--gate", gate,
                    "--port", "/dev/failing-badge",
                    "--session-file", str(session_path),
                    "--evidence", str(evidence_path),
                ]
                if cycle is not None:
                    argv.extend(("--cycle", str(cycle)))
                runner_name = (
                    "run_update_cycle_checkpoint"
                    if gate == "three-update-cycles"
                    else "run_interrupted_upload_gate"
                )
                with mock.patch.object(
                    acceptance,
                    runner_name,
                    side_effect=RuntimeError(
                        "private SSID and device details must not persist"
                    ),
                ) as runner:
                    def invoke() -> int | None:
                        try:
                            return acceptance.main(argv)
                        except RuntimeError:
                            return None

                    with contextlib.redirect_stderr(io.StringIO()):
                        first_result = invoke()
                        retry_result = (
                            invoke() if first_result == 2 else None
                        )

                records = _read_records(evidence_path)
                failure_records = [
                    record for record in records
                    if record["gate"] == gate
                    and record["status"] == "FAIL"
                ]
                self.assertEqual(first_result, 2)
                self.assertEqual(retry_result, 2)
                self.assertEqual(runner.call_count, 1)
                self.assertEqual(len(failure_records), 1)
                self.assertEqual(
                    failure_records[0]["facts"],
                    {
                        "error": "hardware_gate_failed",
                        "phase": phase,
                    },
                )
                self.assertNotIn(
                    "private SSID",
                    evidence_path.read_text(encoding="utf-8"),
                )
                self.assertFalse(reservation_path.exists())

    def test_abnormal_cycle_exit_leaves_durable_reservation_and_blocks_retry(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            _record_gate_one(evidence_path)
            evidence_stat = evidence_path.stat()
            reservation_path = root / (
                f".{evidence_path.name}.mutating-gate-reservation"
            )
            argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/abnormal-exit",
                "--session-file", str(session_path),
                "--evidence", str(evidence_path),
            ]

            with mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                side_effect=KeyboardInterrupt(),
            ) as first_runner, self.assertRaises(KeyboardInterrupt):
                acceptance.main(argv)

            self.assertEqual(first_runner.call_count, 1)
            self.assertTrue(reservation_path.is_file())
            self.assertEqual(
                reservation_path.stat().st_mode & 0o077,
                0,
            )
            self.assertEqual(
                json.loads(reservation_path.read_text(encoding="utf-8")),
                {
                    "cycle": 1,
                    "evidence_device": evidence_stat.st_dev,
                    "evidence_inode": evidence_stat.st_ino,
                    "evidence_path": str(evidence_path.absolute()),
                    "gate": "three-update-cycles",
                    "phase": "update_cycle_1",
                    "schema": 1,
                    "session_id": _session().session_id,
                },
            )

            with contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1),
            ) as retry_runner:
                retry_result = acceptance.main(argv)

            self.assertEqual(retry_result, 2)
            retry_runner.assert_not_called()
            self.assertTrue(reservation_path.is_file())

    def test_evidence_replacement_during_cycle_fails_and_keeps_reservation(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            detached_path = root / "detached-evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            _record_gate_one(evidence_path)
            original = evidence_path.read_bytes()
            reservation_path = root / (
                f".{evidence_path.name}.mutating-gate-reservation"
            )
            argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/replaced-evidence",
                "--session-file", str(session_path),
                "--evidence", str(evidence_path),
            ]

            def replace_evidence(
                _port: str,
                _session_value: acceptance.BadgeAcceptanceSession,
                _cycle: int,
                **_kwargs: object,
            ) -> acceptance.VerifiedCycleCheckpoint:
                evidence_path.rename(detached_path)
                evidence_path.write_bytes(original)
                os.chmod(evidence_path, 0o600)
                return _verify_cycle_fixture(1)

            with contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                side_effect=replace_evidence,
            ) as runner:
                result = acceptance.main(argv)

            self.assertEqual(result, 2)
            self.assertEqual(runner.call_count, 1)
            self.assertEqual(evidence_path.read_bytes(), original)
            self.assertEqual(detached_path.read_bytes(), original)
            self.assertTrue(reservation_path.is_file())

            with contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1),
            ) as retry_runner:
                retry_result = acceptance.main(argv)

            self.assertEqual(retry_result, 2)
            retry_runner.assert_not_called()
            self.assertEqual(evidence_path.read_bytes(), original)

    def test_parent_directory_replacement_during_cycle_blocks_retry(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            active_root = root / "active"
            detached_root = root / "detached"
            active_root.mkdir()
            evidence_path = active_root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            _record_gate_one(evidence_path)
            original = evidence_path.read_bytes()
            reservation_name = (
                f".{evidence_path.name}.mutating-gate-reservation"
            )
            argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/replaced-parent",
                "--session-file", str(session_path),
                "--evidence", str(evidence_path),
            ]

            def replace_parent(
                _port: str,
                _session_value: acceptance.BadgeAcceptanceSession,
                _cycle: int,
                **_kwargs: object,
            ) -> acceptance.VerifiedCycleCheckpoint:
                active_root.rename(detached_root)
                active_root.mkdir()
                replacement_evidence = active_root / evidence_path.name
                replacement_evidence.write_bytes(original)
                os.chmod(replacement_evidence, 0o600)
                return _verify_cycle_fixture(1)

            with contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                side_effect=replace_parent,
            ) as runner:
                result = acceptance.main(argv)

            canonical_reservation = active_root / reservation_name
            detached_reservation = detached_root / reservation_name
            self.assertEqual(result, 2)
            self.assertEqual(runner.call_count, 1)
            self.assertTrue(canonical_reservation.is_file())
            self.assertEqual(
                canonical_reservation.stat().st_mode & 0o077,
                0,
            )
            self.assertTrue(detached_reservation.is_file())
            self.assertEqual(
                (active_root / evidence_path.name).read_bytes(),
                original,
            )

            with contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1),
            ) as retry_runner:
                retry_result = acceptance.main(argv)

            self.assertEqual(retry_result, 2)
            retry_runner.assert_not_called()
            self.assertTrue(canonical_reservation.is_file())

    def test_retained_anchor_blocks_parent_swap_after_cleanup_final_check(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            active_root = root / "active"
            detached_root = root / "detached"
            active_root.mkdir()
            evidence_path = active_root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            _record_gate_one(evidence_path)
            gate_one = evidence_path.read_bytes()
            argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/cleanup-parent-race",
                "--session-file", str(session_path),
                "--evidence", str(evidence_path),
            ]
            real_validate = acceptance._validate_open_evidence_file
            cleanup_validations = 0
            swapped = False

            def swap_after_cleanup_final_check(
                fd: int,
                path: Path,
            ) -> None:
                nonlocal cleanup_validations, swapped
                real_validate(fd, path)
                stack = {
                    frame.function for frame in inspect.stack()[1:9]
                }
                if "_remove_mutating_gate_reservation" in stack and \
                        "_validate_live_mutating_gate_reservation" \
                        not in stack:
                    cleanup_validations += 1
                    if cleanup_validations == 2 and not swapped:
                        active_root.rename(detached_root)
                        active_root.mkdir()
                        evidence_path.write_bytes(gate_one)
                        os.chmod(evidence_path, 0o600)
                        swapped = True

            with contextlib.redirect_stdout(
                io.StringIO()
            ), contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "_validate_open_evidence_file",
                side_effect=swap_after_cleanup_final_check,
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1),
            ) as runner:
                first_result = acceptance.main(argv)
                retained_after_first = self._retained_markers()
                retry_result = acceptance.main(argv)

            self.assertTrue(swapped)
            self.assertEqual(first_result, 0)
            self.assertEqual(retry_result, 2)
            self.assertEqual(runner.call_count, 1)
            self.assertEqual(len(retained_after_first), 1)

    def test_retained_anchor_blocks_parent_swap_after_recovery_final_check(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            active_root = root / "active"
            first_detached = root / "first-detached"
            second_detached = root / "second-detached"
            active_root.mkdir()
            evidence_path = active_root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            _record_gate_one(evidence_path)
            gate_one = evidence_path.read_bytes()
            argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/recovery-parent-race",
                "--session-file", str(session_path),
                "--evidence", str(evidence_path),
            ]
            action_calls = 0
            recovery_validations = 0
            recovery_swapped = False

            def replace_parent_during_action(
                _port: str,
                _session_value: acceptance.BadgeAcceptanceSession,
                _cycle: int,
                **_kwargs: object,
            ) -> acceptance.VerifiedCycleCheckpoint:
                nonlocal action_calls
                action_calls += 1
                if action_calls == 1:
                    active_root.rename(first_detached)
                    active_root.mkdir()
                    evidence_path.write_bytes(gate_one)
                    os.chmod(evidence_path, 0o600)
                return _verify_cycle_fixture(1)

            real_validate = (
                acceptance._validate_open_mutating_gate_reservation
            )

            def swap_after_recovery_final_check(
                reservation: acceptance._DurableGateReservation,
            ) -> None:
                nonlocal recovery_validations, recovery_swapped
                real_validate(reservation)
                stack = {
                    frame.function for frame in inspect.stack()[1:9]
                }
                if "_ensure_canonical_mutating_gate_blocker" in stack:
                    recovery_validations += 1
                    if recovery_validations == 2 and \
                            not recovery_swapped:
                        active_root.rename(second_detached)
                        active_root.mkdir()
                        evidence_path.write_bytes(gate_one)
                        os.chmod(evidence_path, 0o600)
                        recovery_swapped = True

            with contextlib.redirect_stdout(
                io.StringIO()
            ), contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "_validate_open_mutating_gate_reservation",
                side_effect=swap_after_recovery_final_check,
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                side_effect=replace_parent_during_action,
            ) as runner:
                first_result = acceptance.main(argv)
                retained_after_first = self._retained_markers()
                retry_result = acceptance.main(argv)

            self.assertTrue(recovery_swapped)
            self.assertEqual(first_result, 2)
            self.assertEqual(retry_result, 2)
            self.assertEqual(runner.call_count, 1)
            self.assertEqual(len(retained_after_first), 1)

    def test_mutating_gate_requires_explicit_private_session_file(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            _record_gate_one(evidence_path)
            with contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance, "run_update_cycle_checkpoint"
            ) as runner, mock.patch.object(
                acceptance.flash, "repo_version"
            ) as repo_version, mock.patch.object(
                acceptance.flash, "require_artifacts"
            ) as require_artifacts, mock.patch.object(
                acceptance.flash, "usb_flow"
            ) as usb_flow, mock.patch.object(
                acceptance.flash, "BadgeSerial"
            ) as badge_serial:
                runner.return_value = _verify_cycle_fixture(1)
                result = acceptance.main([
                    "--gate", "three-update-cycles",
                    "--cycle", "1",
                    "--port", "/dev/must-not-open",
                    "--evidence", str(evidence_path),
                ])

            self.assertEqual(result, 2)
            runner.assert_not_called()
            repo_version.assert_not_called()
            require_artifacts.assert_not_called()
            usb_flow.assert_not_called()
            badge_serial.assert_not_called()

    def test_mutating_session_duplicate_member_is_rejected_before_activity(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            session_path.write_text(
                (
                    '{"session_id":"shadow-session",'
                    '"session_id":"defcon34-canary-001",'
                    f'"uplink_hardware_id":"{UPLINK_ID}",'
                    f'"ble_hardware_id":"{BLE_ID}",'
                    f'"wifi_hardware_id":"{WIFI_ID}"}}'
                ),
                encoding="utf-8",
            )
            _record_gate_one(evidence_path)
            evidence_before = evidence_path.read_bytes()
            stderr = io.StringIO()

            with contextlib.redirect_stderr(
                stderr
            ), mock.patch.object(
                acceptance,
                "load_anchored_session",
                wraps=acceptance.load_anchored_session,
            ) as load_anchored, mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1),
            ) as runner:
                result = acceptance.main([
                    "--gate", "three-update-cycles",
                    "--cycle", "1",
                    "--port", "/dev/must-not-open",
                    "--session-file", str(session_path),
                    "--evidence", str(evidence_path),
                ])

            self.assertEqual(result, 2)
            self.assertIn("duplicate JSON key", stderr.getvalue())
            self.assertEqual(evidence_path.read_bytes(), evidence_before)
            load_anchored.assert_not_called()
            runner.assert_not_called()
            self.assertFalse(self._operation_registry_path().exists())
            self.assertEqual(self._retained_markers(), [])

    def test_manual_session_duplicate_member_is_rejected_before_activity(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = root / "session.json"
            session_path.write_text(
                (
                    '{"session_id":"shadow-session",'
                    '"session_id":"defcon34-canary-001",'
                    f'"uplink_hardware_id":"{UPLINK_ID}",'
                    f'"ble_hardware_id":"{BLE_ID}",'
                    f'"wifi_hardware_id":"{WIFI_ID}"}}'
                ),
                encoding="utf-8",
            )
            facts = dict(_pass_facts("android-control-reconnect"))
            facts.pop("snapshot")
            facts.pop("candidate_artifacts")
            facts_path = root / "facts.json"
            facts_path.write_text(json.dumps(facts), encoding="utf-8")
            badge_status = _status()
            for scanner in badge_status["scanners"]:
                scanner["ver"] = "0.64.75-badge-defcon34"
            badge = mock.MagicMock()
            badge.__enter__.return_value = badge
            badge.__exit__.return_value = None
            badge.status.return_value = badge_status
            stderr = io.StringIO()

            with contextlib.redirect_stderr(
                stderr
            ), mock.patch.object(
                acceptance,
                "_trusted_session_uplink_descriptor",
                return_value=_usb_record("/dev/must-not-open"),
            ) as resolve_descriptor, mock.patch.object(
                acceptance.flash,
                "BadgeSerial",
                return_value=badge,
            ) as badge_serial:
                result = acceptance.main([
                    "--gate", "android-control-reconnect",
                    "--port", "/dev/must-not-open",
                    "--session-file", str(session_path),
                    "--facts-file", str(facts_path),
                    "--expected-version", VERSION,
                    "--evidence", str(evidence_path),
                ])

            self.assertEqual(result, 2)
            self.assertIn("duplicate JSON key", stderr.getvalue())
            self.assertFalse(evidence_path.exists())
            resolve_descriptor.assert_not_called()
            badge_serial.assert_not_called()
            self.assertFalse(self._operation_registry_path().exists())
            self.assertEqual(self._retained_markers(), [])

    def test_manual_facts_duplicate_member_is_rejected_before_activity(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = root / "session.json"
            _write_session_file(session_path)
            facts = dict(_pass_facts("android-control-reconnect"))
            facts.pop("snapshot")
            facts.pop("candidate_artifacts")
            facts_path = root / "facts.json"
            facts_path.write_text(
                '{"status_received":false,' + json.dumps(facts)[1:],
                encoding="utf-8",
            )
            badge_status = _status()
            for scanner in badge_status["scanners"]:
                scanner["ver"] = "0.64.75-badge-defcon34"
            badge = mock.MagicMock()
            badge.__enter__.return_value = badge
            badge.__exit__.return_value = None
            badge.status.return_value = badge_status
            stderr = io.StringIO()

            with contextlib.redirect_stderr(
                stderr
            ), mock.patch.object(
                acceptance,
                "_trusted_session_uplink_descriptor",
                return_value=_usb_record("/dev/must-not-open"),
            ) as resolve_descriptor, mock.patch.object(
                acceptance.flash,
                "BadgeSerial",
                return_value=badge,
            ) as badge_serial:
                result = acceptance.main([
                    "--gate", "android-control-reconnect",
                    "--port", "/dev/must-not-open",
                    "--session-file", str(session_path),
                    "--facts-file", str(facts_path),
                    "--expected-version", VERSION,
                    "--evidence", str(evidence_path),
                ])

            self.assertEqual(result, 2)
            self.assertIn("duplicate JSON key", stderr.getvalue())
            self.assertFalse(evidence_path.exists())
            resolve_descriptor.assert_not_called()
            badge_serial.assert_not_called()
            self.assertFalse(self._operation_registry_path().exists())
            self.assertEqual(self._retained_markers(), [])

    def test_mutating_gate_rejects_unsafe_session_anchor_before_run(
        self,
    ) -> None:
        scenarios = (
            "public",
            "symlink",
            "hardlink",
            "writable-parent",
            "intermediate-symlink",
        )
        for scenario in scenarios:
            with self.subTest(scenario=scenario), \
                    tempfile.TemporaryDirectory() as td:
                root = Path(td)
                evidence_path = root / "evidence.jsonl"
                _record_gate_one(evidence_path)
                session_path = _write_private_session_anchor(root)
                if scenario == "public":
                    os.chmod(session_path, 0o644)
                elif scenario == "symlink":
                    link = session_path.with_name("session-link.json")
                    link.symlink_to(session_path.name)
                    session_path = link
                elif scenario == "hardlink":
                    link = session_path.with_name("session-hardlink.json")
                    os.link(session_path, link)
                    session_path = link
                elif scenario == "writable-parent":
                    os.chmod(session_path.parent, 0o770)
                else:
                    real_root = root / "real-anchor-root"
                    nested = real_root / "nested"
                    nested.mkdir(parents=True, mode=0o700)
                    alias = root / "anchor-alias"
                    alias.symlink_to(real_root, target_is_directory=True)
                    session_path = alias / "nested" / "session.json"
                    _write_session_file(session_path)

                with contextlib.redirect_stderr(
                    io.StringIO()
                ), mock.patch.object(
                    acceptance, "run_update_cycle_checkpoint"
                ) as runner, mock.patch.object(
                    acceptance.flash, "repo_version"
                ) as repo_version, mock.patch.object(
                    acceptance.flash, "require_artifacts"
                ) as require_artifacts, mock.patch.object(
                    acceptance.flash, "usb_flow"
                ) as usb_flow:
                    runner.return_value = _verify_cycle_fixture(1)
                    result = acceptance.main([
                        "--gate", "three-update-cycles",
                        "--cycle", "1",
                        "--port", "/dev/must-not-open",
                        "--session-file", str(session_path),
                        "--evidence", str(evidence_path),
                    ])

                self.assertEqual(result, 2)
                runner.assert_not_called()
                repo_version.assert_not_called()
                require_artifacts.assert_not_called()
                usb_flow.assert_not_called()

    def test_session_file_replacement_before_action_blocks_hardware(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            detached_session = session_path.with_name(
                "detached-session.json"
            )
            _record_gate_one(evidence_path)
            original = session_path.read_bytes()
            real_validate = acceptance._validate_mutating_gate_prefix
            replaced = False

            def replace_after_prefix(
                records: list[dict],
                session: acceptance.BadgeAcceptanceSession,
                gate: str,
                *,
                cycle: int | None,
            ) -> str:
                nonlocal replaced
                result = real_validate(
                    records, session, gate, cycle=cycle
                )
                if not replaced:
                    session_path.rename(detached_session)
                    session_path.write_bytes(original)
                    os.chmod(session_path, 0o600)
                    replaced = True
                return result

            with contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "_validate_mutating_gate_prefix",
                side_effect=replace_after_prefix,
            ), mock.patch.object(
                acceptance, "run_update_cycle_checkpoint"
            ) as runner:
                runner.return_value = _verify_cycle_fixture(1)
                result = acceptance.main([
                    "--gate", "three-update-cycles",
                    "--cycle", "1",
                    "--port", "/dev/must-not-open",
                    "--session-file", str(session_path),
                    "--evidence", str(evidence_path),
                ])

            self.assertTrue(replaced)
            self.assertEqual(result, 2)
            runner.assert_not_called()

    def test_fresh_registry_fences_before_first_prepared(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            _record_gate_one(evidence_path)
            with contextlib.redirect_stdout(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1),
            ) as runner:
                result = acceptance.main([
                    "--gate", "three-update-cycles",
                    "--cycle", "1",
                    "--port", "/dev/fresh-registry-fence",
                    "--session-file", str(session_path),
                    "--evidence", str(evidence_path),
                ])

            self.assertEqual(result, 0)
            self.assertEqual(runner.call_count, 1)
            records = self._operation_registry_records()
            self.assertEqual(
                records[1],
                {
                    "event": "ARMING_PROTOCOL_FENCE",
                    "schema": acceptance.OPERATION_REGISTRY_SCHEMA,
                },
            )
            self.assertEqual(
                [
                    record.get("state", record.get("event"))
                    for record in records[1:]
                ],
                [
                    "ARMING_PROTOCOL_FENCE",
                    "PREPARED",
                    "RESERVATION_ARMED",
                    "STARTED",
                ],
            )

    def test_legacy_started_is_preserved_before_migration_fence(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            _record_gate_one(evidence_path)
            operation_sha256 = self._cycle_one_operation_sha256()
            attempt_id = "1" * 32
            legacy_records = [
                {
                    "attempt_id": attempt_id,
                    "operation_sha256": operation_sha256,
                    "schema": acceptance.OPERATION_REGISTRY_SCHEMA,
                    "state": state_value,
                }
                for state_value in ("PREPARED", "STARTED")
            ]
            self._write_legacy_operation_registry(legacy_records)

            stderr = io.StringIO()
            with contextlib.redirect_stderr(
                stderr
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1),
            ) as runner:
                result = acceptance.main([
                    "--gate", "three-update-cycles",
                    "--cycle", "1",
                    "--port", "/dev/legacy-started",
                    "--session-file", str(session_path),
                    "--evidence", str(evidence_path),
                ])

            self.assertEqual(result, 2)
            runner.assert_not_called()
            records = self._operation_registry_records()
            self.assertEqual(records[1:3], legacy_records)
            self.assertEqual(
                self._registry_fences(),
                [{
                    "event": "ARMING_PROTOCOL_FENCE",
                    "schema": acceptance.OPERATION_REGISTRY_SCHEMA,
                }],
            )
            self.assertIn("started", stderr.getvalue().lower())

    def test_legacy_cancelled_with_possible_sidecar_blocks_exact_operation(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            first_root = root / "first"
            copied_root = root / "copied"
            first_root.mkdir(mode=0o700)
            copied_root.mkdir(mode=0o700)
            evidence_path = first_root / "evidence.jsonl"
            session_path = _write_private_session_anchor(first_root)
            _record_gate_one(evidence_path)
            copied_evidence = copied_root / "evidence-copy.jsonl"
            copied_evidence.write_bytes(evidence_path.read_bytes())
            os.chmod(copied_evidence, 0o600)
            copied_session = _write_private_session_anchor(copied_root)
            operation_sha256 = self._cycle_one_operation_sha256()
            attempt_id = "2" * 32
            legacy_records = [
                {
                    "attempt_id": attempt_id,
                    "operation_sha256": operation_sha256,
                    "schema": acceptance.OPERATION_REGISTRY_SCHEMA,
                    "state": state_value,
                }
                for state_value in ("PREPARED", "CANCELLED")
            ]
            self._write_legacy_operation_registry(legacy_records)

            state_root = acceptance._open_retained_state_root()
            retained_marker = None
            try:
                retained_marker = (
                    acceptance._create_retained_operation_marker(
                        state_root, operation_sha256
                    )
                )
            finally:
                acceptance._close_retained_operation_marker(
                    retained_marker
                )
                acceptance._close_retained_state_root(state_root)

            evidence_fd = acceptance._open_evidence_for_append(
                evidence_path
            )
            reservation = None
            try:
                reservation = (
                    acceptance._create_mutating_gate_reservation(
                        evidence_path,
                        evidence_fd,
                        _session(),
                        "three-update-cycles",
                        "update_cycle_1",
                        cycle=1,
                    )
                )
            finally:
                acceptance._close_mutating_gate_reservation(
                    reservation
                )
                os.close(evidence_fd)

            reservation_path = (
                first_root
                / ".evidence.jsonl.mutating-gate-reservation"
            )
            stderr = io.StringIO()
            copied_argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/legacy-cancelled-copy",
                "--session-file", str(copied_session),
                "--evidence", str(copied_evidence),
            ]
            original_argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/legacy-cancelled-original",
                "--session-file", str(session_path),
                "--evidence", str(evidence_path),
            ]
            with contextlib.redirect_stderr(
                stderr
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1),
            ) as runner:
                copied_result = acceptance.main(copied_argv)
                original_result = acceptance.main(original_argv)

            self.assertEqual([copied_result, original_result], [2, 2])
            runner.assert_not_called()
            records = self._operation_registry_records()
            self.assertEqual(records[1:3], legacy_records)
            self.assertEqual(records[3], {
                "event": "ARMING_PROTOCOL_FENCE",
                "schema": acceptance.OPERATION_REGISTRY_SCHEMA,
            })
            self.assertEqual(records[4:], [])
            self.assertTrue(reservation_path.is_file())
            self.assertEqual(len(self._retained_markers()), 1)
            self.assertIn("legacy", stderr.getvalue().lower())
            self.assertIn(
                "manual state repair", stderr.getvalue().lower()
            )

    def test_legacy_prepared_with_sidecar_blocks_original_and_copy(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            first_root = root / "first"
            copied_root = root / "copied"
            first_root.mkdir(mode=0o700)
            copied_root.mkdir(mode=0o700)
            evidence_path = first_root / "evidence.jsonl"
            session_path = _write_private_session_anchor(first_root)
            _record_gate_one(evidence_path)
            copied_evidence = copied_root / "evidence-copy.jsonl"
            copied_evidence.write_bytes(evidence_path.read_bytes())
            os.chmod(copied_evidence, 0o600)
            copied_session = _write_private_session_anchor(copied_root)
            operation_sha256 = self._cycle_one_operation_sha256()
            attempt_id = "3" * 32
            self._write_legacy_operation_registry([{
                "attempt_id": attempt_id,
                "operation_sha256": operation_sha256,
                "schema": acceptance.OPERATION_REGISTRY_SCHEMA,
                "state": "PREPARED",
            }])

            state_root = acceptance._open_retained_state_root()
            retained_marker = None
            try:
                retained_marker = (
                    acceptance._create_retained_operation_marker(
                        state_root, operation_sha256
                    )
                )
            finally:
                acceptance._close_retained_operation_marker(
                    retained_marker
                )
                acceptance._close_retained_state_root(state_root)

            evidence_fd = acceptance._open_evidence_for_append(
                evidence_path
            )
            reservation = None
            try:
                reservation = (
                    acceptance._create_mutating_gate_reservation(
                        evidence_path,
                        evidence_fd,
                        _session(),
                        "three-update-cycles",
                        "update_cycle_1",
                        cycle=1,
                    )
                )
            finally:
                acceptance._close_mutating_gate_reservation(
                    reservation
                )
                os.close(evidence_fd)

            registry_before = self._operation_registry_path().read_bytes()
            reservation_path = (
                first_root
                / ".evidence.jsonl.mutating-gate-reservation"
            )
            stderr = io.StringIO()
            copied_argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/legacy-copy",
                "--session-file", str(copied_session),
                "--evidence", str(copied_evidence),
            ]
            original_argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/legacy-original",
                "--session-file", str(session_path),
                "--evidence", str(evidence_path),
            ]
            with contextlib.redirect_stderr(
                stderr
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1),
            ) as runner:
                copied_result = acceptance.main(copied_argv)
                original_result = acceptance.main(original_argv)

            self.assertEqual([copied_result, original_result], [2, 2])
            runner.assert_not_called()
            self.assertEqual(
                self._operation_registry_path().read_bytes(),
                registry_before,
            )
            self.assertEqual(self._registry_fences(), [])
            self.assertTrue(reservation_path.is_file())
            self.assertEqual(len(self._retained_markers()), 1)
            self.assertIn("legacy", stderr.getvalue().lower())
            self.assertIn(
                "manual state repair", stderr.getvalue().lower()
            )

    def test_success_retains_one_private_opaque_fixed_root_marker(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            _record_gate_one(evidence_path)

            with contextlib.redirect_stdout(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1),
            ):
                result = acceptance.main([
                    "--gate", "three-update-cycles",
                    "--cycle", "1",
                    "--port", "/dev/live-badge",
                    "--session-file", str(session_path),
                    "--evidence", str(evidence_path),
                ])

            markers = self._retained_markers()
            self.assertEqual(result, 0)
            self.assertEqual(len(markers), 1)
            marker = markers[0]
            marker_stat = marker.stat()
            self.assertTrue(marker.is_file())
            self.assertEqual(marker_stat.st_nlink, 1)
            self.assertEqual(marker_stat.st_uid, os.geteuid())
            self.assertEqual(marker_stat.st_mode & 0o077, 0)
            self.assertTrue(marker.name.startswith("op-"))
            self.assertTrue(marker.name.endswith(".retained"))
            digest = marker.name.removeprefix("op-").removesuffix(
                ".retained"
            )
            self.assertRegex(digest, r"^[0-9a-f]{64}$")
            operation = {
                "artifacts": {
                    "candidate": _candidate_artifacts(),
                    "platform": CANARY_PLATFORM_KEY,
                    "scanner_target": "scanner-s3-combo-fof_badge",
                    "uplink_target": "uplink-s3-fof_badge",
                },
                "cycle": 1,
                "firmware_version": VERSION,
                "gate": "three-update-cycles",
                "phase": "update_cycle_1",
                "schema": 2,
                "session": {
                    "ble_hardware_id": BLE_ID,
                    "session_id": _session().session_id,
                    "uplink_hardware_id": UPLINK_ID,
                    "wifi_hardware_id": WIFI_ID,
                },
            }
            expected_digest = hashlib.sha256(
                b"friend-or-foe/badge-acceptance/"
                b"retained-operation/v2\x00"
                + json.dumps(
                    operation, separators=(",", ":"), sort_keys=True
                ).encode("utf-8")
            ).hexdigest()
            self.assertEqual(digest, expected_digest)
            self.assertEqual(
                json.loads(marker.read_text(encoding="utf-8")),
                {
                    "operation_sha256": digest,
                    "schema": 2,
                },
            )
            private_values = (
                _session().session_id,
                UPLINK_ID,
                BLE_ID,
                WIFI_ID,
                evidence_path.name,
                session_path.name,
                "three-update-cycles",
                "update_cycle_1",
            )
            for value in private_values:
                self.assertNotIn(value.lower(), marker.name.lower())
                self.assertNotIn(
                    value.replace(":", "").lower(),
                    marker.name.lower(),
                )
            registry_path = self._operation_registry_path()
            registry_stat = registry_path.stat()
            self.assertTrue(
                registry_stat.st_flags & stat.UF_APPEND
            )
            registry_records = self._operation_registry_records()
            self.assertEqual(
                registry_records[0],
                {"kind": "REGISTRY", "schema": 3},
            )
            self.assertEqual(
                [
                    record["state"]
                    for record in registry_records[1:]
                    if "state" in record
                ],
                ["PREPARED", "STARTED"],
            )
            self.assertEqual(
                [
                    record["event"]
                    for record in registry_records[1:]
                    if "event" in record
                ],
                [
                    "ARMING_PROTOCOL_FENCE",
                    "RESERVATION_ARMED",
                ],
            )
            self.assertEqual(
                {
                    record["operation_sha256"]
                    for record in registry_records[1:]
                    if "operation_sha256" in record
                },
                {digest},
            )
            self.assertEqual(
                len({
                    record["attempt_id"]
                    for record in registry_records[1:]
                    if "attempt_id" in record
                }),
                1,
            )
            registry_text = registry_path.read_text(encoding="utf-8")
            for value in private_values:
                self.assertNotIn(value.lower(), registry_text.lower())
                self.assertNotIn(
                    value.replace(":", "").lower(),
                    registry_text.lower(),
                )
            with self.assertRaises(PermissionError):
                registry_path.rename(
                    registry_path.with_name("detached-registry")
                )

    def test_session_and_evidence_copies_share_one_operation_namespace(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            first_evidence = root / "first" / "evidence.jsonl"
            second_evidence = root / "second" / "copied-evidence.jsonl"
            first_session = root / "first" / "session.json"
            second_session = root / "second" / "copied-session.json"
            _write_session_file(first_session)
            _write_session_file(second_session)
            _record_gate_one(first_evidence)
            gate_one = first_evidence.read_bytes()
            second_evidence.write_bytes(gate_one)
            os.chmod(second_evidence, 0o600)
            base_argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/copied-session",
            ]

            with contextlib.redirect_stdout(
                io.StringIO()
            ), contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1),
            ) as runner:
                first_result = acceptance.main([
                    *base_argv,
                    "--session-file", str(first_session),
                    "--evidence", str(first_evidence),
                ])
                retry_result = acceptance.main([
                    *base_argv,
                    "--session-file", str(second_session),
                    "--evidence", str(second_evidence),
                ])

            self.assertEqual(first_result, 0)
            self.assertEqual(retry_result, 2)
            self.assertEqual(runner.call_count, 1)
            self.assertEqual(len(self._retained_markers()), 1)

    def test_case_variant_samefiles_share_one_operation_namespace(
        self,
    ) -> None:
        if sys.platform != "darwin":
            self.skipTest("case-variant samefile gate is macOS-specific")
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "Evidence.jsonl"
            alternate_evidence = root / "eVIDENCE.JSONL"
            session_path = root / "Session.json"
            alternate_session = root / "sESSION.JSON"
            _write_session_file(session_path)
            _record_gate_one(evidence_path)
            if not alternate_evidence.exists() or \
                    not alternate_session.exists() or \
                    not os.path.samefile(evidence_path, alternate_evidence) or \
                    not os.path.samefile(session_path, alternate_session):
                self.skipTest("test volume is case-sensitive")
            gate_one = evidence_path.read_bytes()

            with contextlib.redirect_stdout(
                io.StringIO()
            ), contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1),
            ) as runner:
                first_result = acceptance.main([
                    "--gate", "three-update-cycles",
                    "--cycle", "1",
                    "--port", "/dev/case-alias",
                    "--session-file", str(session_path),
                    "--evidence", str(evidence_path),
                ])
                evidence_path.write_bytes(gate_one)
                os.chmod(evidence_path, 0o600)
                retry_result = acceptance.main([
                    "--gate", "three-update-cycles",
                    "--cycle", "1",
                    "--port", "/dev/case-alias",
                    "--session-file", str(alternate_session),
                    "--evidence", str(alternate_evidence),
                ])

            self.assertEqual(first_result, 0)
            self.assertEqual(retry_result, 2)
            self.assertEqual(runner.call_count, 1)
            self.assertEqual(len(self._retained_markers()), 1)

    def test_nested_session_survives_evidence_parent_namespace_replacement(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            active = root / "active"
            detached = root / "detached"
            active.mkdir()
            evidence_path = active / "evidence.jsonl"
            session_path = active / "session" / "session.json"
            _write_session_file(session_path)
            session_bytes = session_path.read_bytes()
            _record_gate_one(evidence_path)
            gate_one = evidence_path.read_bytes()
            action_calls = 0

            def replace_parent(
                _port: str,
                _session_value: acceptance.BadgeAcceptanceSession,
                _cycle: int,
                **_kwargs: object,
            ) -> acceptance.VerifiedCycleCheckpoint:
                nonlocal action_calls
                action_calls += 1
                active.rename(detached)
                active.mkdir()
                evidence_path.write_bytes(gate_one)
                os.chmod(evidence_path, 0o600)
                session_path.parent.mkdir()
                session_path.write_bytes(session_bytes)
                os.chmod(session_path, 0o600)
                return _verify_cycle_fixture(1)

            argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/nested-session",
                "--session-file", str(session_path),
                "--evidence", str(evidence_path),
            ]
            with contextlib.redirect_stdout(
                io.StringIO()
            ), contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                side_effect=replace_parent,
            ) as runner:
                first_result = acceptance.main(argv)
                retry_result = acceptance.main(argv)

            self.assertEqual(first_result, 2)
            self.assertEqual(retry_result, 2)
            self.assertEqual(action_calls, 1)
            self.assertEqual(runner.call_count, 1)
            self.assertEqual(len(self._retained_markers()), 1)

    def test_mac_shaped_session_id_never_appears_in_marker_name(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = root / "session.json"
            mac_shaped_id = "020000000001"
            session = acceptance.BadgeAcceptanceSession(
                session_id=mac_shaped_id,
                uplink_hardware_id=UPLINK_ID,
                ble_hardware_id=BLE_ID,
                wifi_hardware_id=WIFI_ID,
            )
            session_path.write_text(json.dumps({
                "session_id": session.session_id,
                "uplink_hardware_id": session.uplink_hardware_id,
                "ble_hardware_id": session.ble_hardware_id,
                "wifi_hardware_id": session.wifi_hardware_id,
            }), encoding="utf-8")
            os.chmod(session_path, 0o600)
            acceptance.record_gate(
                evidence_path,
                session,
                "android-control-reconnect",
                "PASS",
                _pass_facts("android-control-reconnect"),
            )

            with contextlib.redirect_stdout(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1, session=session),
            ):
                result = acceptance.main([
                    "--gate", "three-update-cycles",
                    "--cycle", "1",
                    "--port", "/dev/mac-shaped-session",
                    "--session-file", str(session_path),
                    "--evidence", str(evidence_path),
                ])

            markers = self._retained_markers()
            self.assertEqual(result, 0)
            self.assertEqual(len(markers), 1)
            self.assertNotIn(mac_shaped_id, markers[0].name)
            self.assertRegex(
                markers[0].name, r"^op-[0-9a-f]{64}\.retained$"
            )

    def test_fixed_state_root_rejects_unsafe_components_before_run(
        self,
    ) -> None:
        for scenario in (
            "intermediate-symlink",
            "world-writable",
            "regular-file",
        ):
            with self.subTest(scenario=scenario), \
                    tempfile.TemporaryDirectory() as td:
                anchor = Path(td).resolve()
                os.chmod(anchor, 0o700)
                if scenario == "intermediate-symlink":
                    real = anchor / "real-component"
                    real.mkdir(mode=0o700)
                    alias = anchor / "state-alias"
                    alias.symlink_to(real, target_is_directory=True)
                    components = ("state-alias", "fixed-state")
                else:
                    unsafe = anchor / f"{scenario}-component"
                    if scenario == "world-writable":
                        unsafe.mkdir(mode=0o770)
                        os.chmod(unsafe, 0o770)
                    else:
                        unsafe.write_bytes(b"not a directory")
                        os.chmod(unsafe, 0o600)
                    components = (unsafe.name, "fixed-state")

                work = anchor / "work"
                work.mkdir(mode=0o700)
                evidence_path = work / "evidence.jsonl"
                session_path = work / "session" / "session.json"
                _write_session_file(session_path)
                _record_gate_one(evidence_path)
                with contextlib.redirect_stderr(
                    io.StringIO()
                ), mock.patch.object(
                    acceptance,
                    "_retained_state_root_spec",
                    return_value=(anchor, components),
                    create=True,
                ), mock.patch.object(
                    acceptance, "run_update_cycle_checkpoint"
                ) as runner:
                    runner.return_value = _verify_cycle_fixture(1)
                    result = acceptance.main([
                        "--gate", "three-update-cycles",
                        "--cycle", "1",
                        "--port", "/dev/must-not-open",
                        "--session-file", str(session_path),
                        "--evidence", str(evidence_path),
                    ])

                self.assertEqual(result, 2)
                runner.assert_not_called()

    def test_state_root_or_ancestor_replacement_before_action_is_rejected(
        self,
    ) -> None:
        for scenario in ("state-root", "ancestor"):
            with self.subTest(scenario=scenario), \
                    tempfile.TemporaryDirectory() as td:
                anchor = Path(td).resolve()
                os.chmod(anchor, 0o700)
                components = ("app-parent", "fixed-state")
                state_root = anchor.joinpath(*components)
                work = anchor / "work"
                work.mkdir(mode=0o700)
                evidence_path = work / "evidence.jsonl"
                session_path = work / "session" / "session.json"
                _write_session_file(session_path)
                _record_gate_one(evidence_path)
                detached = anchor / f"detached-{scenario}"
                replaced = False
                real_validate = getattr(
                    acceptance,
                    "_validate_retained_state_root_before_action",
                    None,
                )

                def replace_then_validate(
                    state: object,
                    marker: object,
                ) -> None:
                    nonlocal replaced
                    if not replaced:
                        if scenario == "state-root":
                            state_root.rename(detached)
                            state_root.mkdir(mode=0o700)
                        else:
                            app_parent = state_root.parent
                            app_parent.rename(detached)
                            app_parent.mkdir(mode=0o700)
                            state_root.mkdir(mode=0o700)
                        replaced = True
                    if real_validate is not None:
                        real_validate(state, marker)

                with contextlib.redirect_stderr(
                    io.StringIO()
                ), mock.patch.object(
                    acceptance,
                    "_retained_state_root_spec",
                    return_value=(anchor, components),
                    create=True,
                ), mock.patch.object(
                    acceptance,
                    "_validate_retained_state_root_before_action",
                    side_effect=replace_then_validate,
                    create=True,
                ), mock.patch.object(
                    acceptance, "run_update_cycle_checkpoint"
                ) as runner:
                    runner.return_value = _verify_cycle_fixture(1)
                    result = acceptance.main([
                        "--gate", "three-update-cycles",
                        "--cycle", "1",
                        "--port", "/dev/must-not-open",
                        "--session-file", str(session_path),
                        "--evidence", str(evidence_path),
                    ])

                self.assertTrue(replaced)
                self.assertEqual(result, 2)
                runner.assert_not_called()
                self.assertEqual(
                    list(anchor.rglob("op-*.retained")), []
                )

    def test_post_validation_ancestor_swap_cannot_replay_from_copies(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            first = root / "first"
            second = root / "second"
            first_session = first / "session.json"
            second_session = second / "session-copy.json"
            first_evidence = first / "evidence.jsonl"
            second_evidence = second / "evidence-copy.jsonl"
            _write_session_file(first_session)
            _write_session_file(second_session)
            _record_gate_one(first_evidence)
            second_evidence.write_bytes(first_evidence.read_bytes())
            os.chmod(second_evidence, 0o600)

            state_parent = self._retained_state_root().parent
            detached_parent = state_parent.with_name(
                "detached-after-final-validation"
            )
            real_validate = (
                acceptance._validate_retained_state_root_before_action
            )
            swapped = False

            def validate_then_swap(
                state: object,
                marker: object,
            ) -> None:
                nonlocal swapped
                real_validate(state, marker)
                if not swapped:
                    state_parent.rename(detached_parent)
                    state_parent.mkdir(mode=0o700)
                    swapped = True

            first_argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/post-validation-swap",
                "--session-file", str(first_session),
                "--evidence", str(first_evidence),
            ]
            second_argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/post-validation-swap",
                "--session-file", str(second_session),
                "--evidence", str(second_evidence),
            ]
            with contextlib.redirect_stdout(
                io.StringIO()
            ), contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "_validate_retained_state_root_before_action",
                side_effect=validate_then_swap,
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1),
            ) as runner:
                first_result = acceptance.main(first_argv)
                second_result = acceptance.main(second_argv)

            self.assertTrue(swapped)
            self.assertEqual(first_result, 2)
            self.assertEqual(second_result, 2)
            self.assertEqual(runner.call_count, 1)

    def test_marker_creation_failures_clean_up_before_hardware(
        self,
    ) -> None:
        hooks = (
            "_write_retained_marker_bytes",
            "_fsync_retained_marker_file",
            "_fsync_retained_state_root",
            "_post_create_validate_retained_operation_marker",
        )
        for hook in hooks:
            with self.subTest(hook=hook), \
                    tempfile.TemporaryDirectory() as td:
                anchor = Path(td).resolve()
                os.chmod(anchor, 0o700)
                components = ("app-parent", "fixed-state")
                state_root = anchor.joinpath(*components)
                work = anchor / "work"
                work.mkdir(mode=0o700)
                evidence_path = work / "evidence.jsonl"
                session_path = work / "session" / "session.json"
                _write_session_file(session_path)
                _record_gate_one(evidence_path)

                with contextlib.redirect_stderr(
                    io.StringIO()
                ), mock.patch.object(
                    acceptance,
                    "_retained_state_root_spec",
                    return_value=(anchor, components),
                    create=True,
                ), mock.patch.object(
                    acceptance,
                    hook,
                    side_effect=OSError(f"injected {hook} failure"),
                    create=True,
                ), mock.patch.object(
                    acceptance, "run_update_cycle_checkpoint"
                ) as runner:
                    runner.return_value = _verify_cycle_fixture(1)
                    result = acceptance.main([
                        "--gate", "three-update-cycles",
                        "--cycle", "1",
                        "--port", "/dev/must-not-open",
                        "--session-file", str(session_path),
                        "--evidence", str(evidence_path),
                    ])

                self.assertEqual(result, 2)
                runner.assert_not_called()
                self.assertEqual(
                    list(state_root.glob("op-*.retained")), []
                )

    def test_post_o_excl_fstat_failure_cleans_and_can_retry(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            _record_gate_one(evidence_path)
            argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/post-o-excl-fstat",
                "--session-file", str(session_path),
                "--evidence", str(evidence_path),
            ]

            with contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "_fstat_new_retained_operation_marker",
                side_effect=OSError("injected marker fstat failure"),
                create=True,
            ), mock.patch.object(
                acceptance, "run_update_cycle_checkpoint"
            ) as first_runner:
                first_runner.return_value = _verify_cycle_fixture(1)
                first_result = acceptance.main(argv)

            self.assertEqual(first_result, 2)
            first_runner.assert_not_called()
            self.assertEqual(self._retained_markers(), [])
            first_transitions = self._operation_registry_records()[1:]
            self.assertEqual(
                [
                    record["state"]
                    for record in first_transitions
                    if "state" in record
                ],
                ["PREPARED", "CANCELLED"],
            )

            with contextlib.redirect_stdout(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1),
            ) as retry_runner:
                retry_result = acceptance.main(argv)

            self.assertEqual(retry_result, 0)
            self.assertEqual(retry_runner.call_count, 1)
            self.assertEqual(len(self._retained_markers()), 1)
            all_transitions = [
                record
                for record in self._operation_registry_records()[1:]
                if "state" in record
            ]
            self.assertEqual(
                [
                    record["state"]
                    for record in all_transitions
                ],
                [
                    "PREPARED",
                    "CANCELLED",
                    "PREPARED",
                    "STARTED",
                ],
            )
            self.assertNotEqual(
                all_transitions[0]["attempt_id"],
                all_transitions[2]["attempt_id"],
            )

    def test_protocol_fence_parser_accepts_one_safe_boundary_only(
        self,
    ) -> None:
        operation_sha256 = "d" * 64
        post_fence_operation_sha256 = "e" * 64
        legacy_attempt = "4" * 32
        post_fence_attempt = "5" * 32
        header = {
            "kind": "REGISTRY",
            "schema": acceptance.OPERATION_REGISTRY_SCHEMA,
        }
        legacy_prepared = {
            "attempt_id": legacy_attempt,
            "operation_sha256": operation_sha256,
            "schema": acceptance.OPERATION_REGISTRY_SCHEMA,
            "state": "PREPARED",
        }
        legacy_cancelled = {
            **legacy_prepared,
            "state": "CANCELLED",
        }
        fence = {
            "event": "ARMING_PROTOCOL_FENCE",
            "schema": acceptance.OPERATION_REGISTRY_SCHEMA,
        }
        post_fence_prepared = {
            "attempt_id": post_fence_attempt,
            "operation_sha256": post_fence_operation_sha256,
            "schema": acceptance.OPERATION_REGISTRY_SCHEMA,
            "state": "PREPARED",
        }

        def encode(records: list[dict]) -> bytes:
            return "".join(
                json.dumps(
                    record,
                    separators=(",", ":"),
                    sort_keys=True,
                ) + "\n"
                for record in records
            ).encode("utf-8")

        try:
            acceptance._parse_operation_registry(encode([
                header,
                legacy_prepared,
                legacy_cancelled,
                fence,
                post_fence_prepared,
            ]))
            accepted_safe_boundary = True
        except acceptance.AcceptanceError:
            accepted_safe_boundary = False
        self.assertTrue(accepted_safe_boundary)

        invalid_histories = (
            [
                header,
                legacy_prepared,
                legacy_cancelled,
                fence,
                fence,
            ],
            [
                header,
                legacy_prepared,
                fence,
            ],
            [
                header,
                {
                    **fence,
                    "unexpected": True,
                },
            ],
            [
                header,
                legacy_prepared,
                legacy_cancelled,
                fence,
                {
                    **post_fence_prepared,
                    "operation_sha256": operation_sha256,
                },
            ],
        )
        for history in invalid_histories:
            with self.subTest(history=history), self.assertRaises(
                acceptance.AcceptanceError
            ):
                acceptance._parse_operation_registry(encode(history))

    def test_torn_protocol_fence_blocks_without_dispatch(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            _record_gate_one(evidence_path)
            operation_sha256 = self._cycle_one_operation_sha256()
            attempt_id = "6" * 32
            self._write_legacy_operation_registry([
                {
                    "attempt_id": attempt_id,
                    "operation_sha256": operation_sha256,
                    "schema": acceptance.OPERATION_REGISTRY_SCHEMA,
                    "state": state_value,
                }
                for state_value in ("PREPARED", "CANCELLED")
            ])
            argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/torn-protocol-fence",
                "--session-file", str(session_path),
                "--evidence", str(evidence_path),
            ]
            real_write = (
                acceptance._write_operation_registry_transition
            )
            torn_once = False

            def tear_protocol_fence(
                fd: int,
                encoded: bytes,
                state_value: str,
            ) -> None:
                nonlocal torn_once
                if state_value == "ARMING_PROTOCOL_FENCE" and \
                        not torn_once:
                    torn_once = True
                    partial = encoded[:len(encoded) // 2]
                    self.assertEqual(os.write(fd, partial), len(partial))
                    raise acceptance.AcceptanceError(
                        "injected torn protocol fence"
                    )
                real_write(fd, encoded, state_value)

            with contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "_write_operation_registry_transition",
                side_effect=tear_protocol_fence,
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1),
            ) as runner:
                first_result = acceptance.main(argv)
                retry_result = acceptance.main(argv)

            self.assertEqual([first_result, retry_result], [2, 2])
            runner.assert_not_called()
            registry_bytes = self._operation_registry_path().read_bytes()
            self.assertFalse(registry_bytes.endswith(b"\n"))
            self.assertNotIn(b'"state":"STARTED"', registry_bytes)
            self.assertEqual(self._retained_markers(), [])

    def test_complete_protocol_fence_sync_failure_retries_safely(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            _record_gate_one(evidence_path)
            current_operation_sha256 = (
                self._cycle_one_operation_sha256()
            )
            operation_sha256 = "a" * 64
            self.assertNotEqual(
                operation_sha256, current_operation_sha256
            )
            attempt_id = "7" * 32
            legacy_records = [
                {
                    "attempt_id": attempt_id,
                    "operation_sha256": operation_sha256,
                    "schema": acceptance.OPERATION_REGISTRY_SCHEMA,
                    "state": state_value,
                }
                for state_value in ("PREPARED", "CANCELLED")
            ]
            self._write_legacy_operation_registry(legacy_records)
            argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/protocol-fence-sync-failure",
                "--session-file", str(session_path),
                "--evidence", str(evidence_path),
            ]
            real_sync = acceptance._sync_operation_registry_transition
            failed_once = False

            def fail_first_fence_sync(
                fd: int,
                state_value: str,
            ) -> None:
                nonlocal failed_once
                if state_value == "ARMING_PROTOCOL_FENCE" and \
                        not failed_once:
                    failed_once = True
                    raise OSError(
                        "injected protocol fence F_FULLFSYNC failure"
                    )
                real_sync(fd, state_value)

            with contextlib.redirect_stdout(
                io.StringIO()
            ), contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "_sync_operation_registry_transition",
                side_effect=fail_first_fence_sync,
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1),
            ) as runner:
                first_result = acceptance.main(argv)
                retry_result = acceptance.main(argv)

            self.assertEqual([first_result, retry_result], [2, 0])
            self.assertEqual(runner.call_count, 1)
            records = self._operation_registry_records()
            self.assertEqual(records[1:3], legacy_records)
            self.assertEqual(len(self._registry_fences()), 1)
            self.assertEqual(
                [
                    record["state"]
                    for record in records[3:]
                    if "state" in record
                ],
                ["PREPARED", "STARTED"],
            )

    def test_complete_prepared_sync_failure_recovers_without_dispatch(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            _record_gate_one(evidence_path)
            argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/prepared-sync-failure",
                "--session-file", str(session_path),
                "--evidence", str(evidence_path),
            ]
            real_sync = acceptance._sync_operation_registry_transition
            failed_once = False

            def fail_first_prepared_sync(
                fd: int,
                state_value: str,
            ) -> None:
                nonlocal failed_once
                if state_value == "PREPARED" and not failed_once:
                    failed_once = True
                    raise OSError(
                        "injected complete PREPARED sync failure"
                    )
                real_sync(fd, state_value)

            with contextlib.redirect_stdout(
                io.StringIO()
            ), contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "_sync_operation_registry_transition",
                side_effect=fail_first_prepared_sync,
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1),
            ) as runner:
                first_result = acceptance.main(argv)
                transitions_after_failure = (
                    self._operation_registry_records()[1:]
                )
                retry_result = acceptance.main(argv)

            self.assertEqual(first_result, 2)
            self.assertEqual(retry_result, 0)
            self.assertEqual(runner.call_count, 1)
            self.assertEqual(
                [
                    record["state"]
                    for record in transitions_after_failure
                    if "state" in record
                ],
                ["PREPARED"],
            )
            self.assertEqual(
                [
                    record["state"]
                    for record in self._operation_registry_records()[1:]
                    if "state" in record
                ],
                [
                    "PREPARED",
                    "CANCELLED",
                    "PREPARED",
                    "STARTED",
                ],
            )

    def test_torn_prepared_append_is_explicit_fail_closed_not_started(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            _record_gate_one(evidence_path)
            argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/torn-prepared",
                "--session-file", str(session_path),
                "--evidence", str(evidence_path),
            ]
            real_write = (
                acceptance._write_operation_registry_transition
            )
            torn_once = False

            def tear_first_prepared(
                fd: int,
                encoded: bytes,
                state_value: str,
            ) -> None:
                nonlocal torn_once
                if state_value == "PREPARED" and not torn_once:
                    torn_once = True
                    partial = encoded[:len(encoded) // 2]
                    self.assertEqual(os.write(fd, partial), len(partial))
                    raise acceptance.AcceptanceError(
                        "injected torn PREPARED append"
                    )
                real_write(fd, encoded, state_value)

            with contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "_write_operation_registry_transition",
                side_effect=tear_first_prepared,
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1),
            ) as runner:
                first_result = acceptance.main(argv)
                retry_result = acceptance.main(argv)

            self.assertEqual(first_result, 2)
            self.assertEqual(retry_result, 2)
            runner.assert_not_called()
            self.assertEqual(self._retained_markers(), [])
            registry_bytes = self._operation_registry_path().read_bytes()
            self.assertNotIn(b'"state":"STARTED"', registry_bytes)
            self.assertFalse(registry_bytes.endswith(b"\n"))

    def test_post_fence_stale_prepared_is_cancelled_and_retryable(
        self,
    ) -> None:
        operation_sha256 = "c" * 64
        state_root = acceptance._open_retained_state_root()
        registry = acceptance._open_operation_registry()
        abandoned_marker = None
        try:
            abandoned_attempt = (
                acceptance._prepare_operation_registry_claim(
                    registry, operation_sha256, state_root
                )
            )
            abandoned_marker = (
                acceptance._create_retained_operation_marker(
                    state_root, operation_sha256
                )
            )
        finally:
            acceptance._close_retained_operation_marker(
                abandoned_marker
            )
            acceptance._close_operation_registry(registry)
            acceptance._close_retained_state_root(state_root)
        self.assertEqual(len(self._retained_markers()), 1)

        state_root = acceptance._open_retained_state_root()
        registry = acceptance._open_operation_registry()
        try:
            retry_attempt = acceptance._prepare_operation_registry_claim(
                registry, operation_sha256, state_root
            )
            self.assertEqual(self._retained_markers(), [])
            acceptance._cancel_operation_registry_claim(
                registry, operation_sha256, retry_attempt
            )
        finally:
            acceptance._close_operation_registry(registry)
            acceptance._close_retained_state_root(state_root)

        self.assertNotEqual(abandoned_attempt, retry_attempt)
        transitions = [
            record
            for record in self._operation_registry_records()[1:]
            if "state" in record
        ]
        self.assertEqual(
            [
                record["state"]
                for record in transitions
            ],
            [
                "PREPARED",
                "CANCELLED",
                "PREPARED",
                "CANCELLED",
            ],
        )
        self.assertEqual(
            transitions[0]["attempt_id"],
            transitions[1]["attempt_id"],
        )
        self.assertEqual(
            transitions[2]["attempt_id"],
            transitions[3]["attempt_id"],
        )
        self.assertEqual(len(self._registry_fences()), 1)
        records = self._operation_registry_records()
        self.assertLess(
            records.index(self._registry_fences()[0]),
            records.index(transitions[0]),
        )

    def test_started_full_sync_failure_never_dispatches_or_retries(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            _record_gate_one(evidence_path)
            argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/started-full-sync-failure",
                "--session-file", str(session_path),
                "--evidence", str(evidence_path),
            ]
            real_sync = acceptance._sync_operation_registry_transition

            def fail_started_sync(fd: int, state_value: str) -> None:
                if state_value == "STARTED":
                    raise OSError("injected STARTED F_FULLFSYNC failure")
                real_sync(fd, state_value)

            with contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "_sync_operation_registry_transition",
                side_effect=fail_started_sync,
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1),
            ) as runner:
                first_result = acceptance.main(argv)
                retry_result = acceptance.main(argv)

            self.assertEqual(first_result, 2)
            self.assertEqual(retry_result, 2)
            runner.assert_not_called()
            self.assertEqual(len(self._retained_markers()), 1)
            transitions = self._operation_registry_records()[1:]
            self.assertEqual(
                [
                    record["state"]
                    for record in transitions
                    if "state" in record
                ],
                ["PREPARED", "STARTED"],
            )

    def test_pre_start_reservation_remove_failure_stays_prepared(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            first_root = root / "first"
            copied_root = root / "copied"
            first_root.mkdir(mode=0o700)
            copied_root.mkdir(mode=0o700)
            evidence_path = first_root / "evidence.jsonl"
            session_path = _write_private_session_anchor(first_root)
            _record_gate_one(evidence_path)
            copied_evidence = copied_root / "evidence-copy.jsonl"
            copied_evidence.write_bytes(evidence_path.read_bytes())
            os.chmod(copied_evidence, 0o600)
            copied_session = _write_private_session_anchor(copied_root)
            first_argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/reservation-remove-failure",
                "--session-file", str(session_path),
                "--evidence", str(evidence_path),
            ]
            copied_argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/reservation-copy-retry",
                "--session-file", str(copied_session),
                "--evidence", str(copied_evidence),
            ]
            reservation_path = (
                first_root
                / ".evidence.jsonl.mutating-gate-reservation"
            )
            stderr = io.StringIO()

            with contextlib.redirect_stderr(
                stderr
            ), mock.patch.object(
                acceptance,
                "_validate_retained_state_root_before_action",
                side_effect=acceptance.AcceptanceError(
                    "injected failure after durable reservation"
                ),
            ), mock.patch.object(
                acceptance,
                "_remove_mutating_gate_reservation",
                side_effect=acceptance.AcceptanceError(
                    "injected reservation removal failure"
                ),
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1),
            ) as runner:
                first_result = acceptance.main(first_argv)

            with contextlib.redirect_stderr(
                stderr
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1),
            ) as retry_runner:
                retry_result = acceptance.main(copied_argv)

            self.assertEqual(first_result, 2)
            self.assertEqual(retry_result, 2)
            runner.assert_not_called()
            retry_runner.assert_not_called()
            self.assertTrue(reservation_path.is_file())
            self.assertEqual(len(self._retained_markers()), 1)
            transitions = self._operation_registry_records()[1:]
            self.assertEqual(
                [
                    record["state"]
                    for record in transitions
                    if "state" in record
                ],
                ["PREPARED"],
            )
            self.assertIn(
                "manual state repair", stderr.getvalue().lower()
            )

    @unittest.skipUnless(
        hasattr(os, "fork"),
        "post-reservation process-loss regression requires fork",
    )
    def test_process_loss_after_durable_reservation_stays_prepared(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            _record_gate_one(evidence_path)
            argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/post-reservation-process-loss",
                "--session-file", str(session_path),
                "--evidence", str(evidence_path),
            ]
            reservation_path = (
                root
                / ".evidence.jsonl.mutating-gate-reservation"
            )

            child = os.fork()
            if child == 0:
                with mock.patch.object(
                    acceptance,
                    "_validate_retained_state_root_before_action",
                    side_effect=lambda *_args: os._exit(73),
                ), mock.patch.object(
                    acceptance,
                    "run_update_cycle_checkpoint",
                    side_effect=lambda *_args: os._exit(74),
                ):
                    acceptance.main(argv)
                os._exit(75)

            waited_child, wait_status = os.waitpid(child, 0)
            self.assertEqual(waited_child, child)
            self.assertEqual(os.waitstatus_to_exitcode(wait_status), 73)
            self.assertTrue(reservation_path.is_file())
            self.assertEqual(len(self._retained_markers()), 1)

            stderr = io.StringIO()
            with contextlib.redirect_stderr(
                stderr
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1),
            ) as runner:
                retry_result = acceptance.main(argv)

            self.assertEqual(retry_result, 2)
            runner.assert_not_called()
            self.assertTrue(reservation_path.is_file())
            self.assertEqual(len(self._retained_markers()), 1)
            transitions = self._operation_registry_records()[1:]
            self.assertEqual(
                [
                    record["state"]
                    for record in transitions
                    if "state" in record
                ],
                ["PREPARED"],
            )
            self.assertIn(
                "manual state repair", stderr.getvalue().lower()
            )

    def test_pre_action_cleanup_failure_is_loud_and_fail_closed(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            anchor = Path(td).resolve()
            os.chmod(anchor, 0o700)
            components = ("app-parent", "fixed-state")
            state_root = anchor.joinpath(*components)
            work = anchor / "work"
            work.mkdir(mode=0o700)
            evidence_path = work / "evidence.jsonl"
            session_path = work / "session" / "session.json"
            _write_session_file(session_path)
            _record_gate_one(evidence_path)
            real_unlink = os.unlink

            def fail_marker_unlink(
                path: str,
                *args: object,
                **kwargs: object,
            ) -> None:
                if str(path).startswith("op-"):
                    raise OSError("injected retained marker unlink failure")
                real_unlink(path, *args, **kwargs)

            stderr = io.StringIO()
            with contextlib.redirect_stderr(
                stderr
            ), mock.patch.object(
                acceptance,
                "_retained_state_root_spec",
                return_value=(anchor, components),
                create=True,
            ), mock.patch.object(
                acceptance,
                "_fsync_retained_marker_file",
                side_effect=OSError("injected marker fsync failure"),
                create=True,
            ), mock.patch.object(
                acceptance.os,
                "unlink",
                side_effect=fail_marker_unlink,
            ), mock.patch.object(
                acceptance, "run_update_cycle_checkpoint"
            ) as runner:
                runner.return_value = _verify_cycle_fixture(1)
                result = acceptance.main([
                    "--gate", "three-update-cycles",
                    "--cycle", "1",
                    "--port", "/dev/must-not-open",
                    "--session-file", str(session_path),
                    "--evidence", str(evidence_path),
                ])

            self.assertEqual(result, 2)
            runner.assert_not_called()
            self.assertIn("cleanup", stderr.getvalue().lower())
            self.assertEqual(
                len(list(state_root.glob("op-*.retained"))), 1
            )

    def test_base_exception_after_action_retains_opaque_o_excl_blocker(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            _record_gate_one(evidence_path)
            argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/base-exception",
                "--session-file", str(session_path),
                "--evidence", str(evidence_path),
            ]
            with mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                side_effect=[
                    KeyboardInterrupt(),
                    _verify_cycle_fixture(1),
                ],
            ) as runner, self.assertRaises(KeyboardInterrupt):
                acceptance.main(argv)

            markers_after_interrupt = self._retained_markers()
            with contextlib.redirect_stderr(io.StringIO()):
                retry_result = acceptance.main(argv)

            self.assertEqual(len(markers_after_interrupt), 1)
            self.assertEqual(retry_result, 2)
            self.assertEqual(runner.call_count, 1)

    def test_replaced_reservation_is_never_unlinked_and_records_fail(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            _record_gate_one(evidence_path)
            reservation_path = root / (
                f".{evidence_path.name}.mutating-gate-reservation"
            )
            orphaned_reservation = root / "original-reservation"
            replacement = b"replacement reservation must survive\n"
            argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/replaced-reservation",
                "--session-file", str(session_path),
                "--evidence", str(evidence_path),
            ]

            def replace_reservation(
                _port: str,
                _session_value: acceptance.BadgeAcceptanceSession,
                _cycle: int,
                **_kwargs: object,
            ) -> acceptance.VerifiedCycleCheckpoint:
                reservation_path.rename(orphaned_reservation)
                reservation_path.write_bytes(replacement)
                os.chmod(reservation_path, 0o600)
                return _verify_cycle_fixture(1)

            with contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                side_effect=replace_reservation,
            ) as runner:
                result = acceptance.main(argv)

            self.assertEqual(result, 2)
            self.assertEqual(runner.call_count, 1)
            self.assertEqual(reservation_path.read_bytes(), replacement)
            self.assertTrue(orphaned_reservation.is_file())
            self.assertEqual(
                [
                    (record["gate"], record["status"])
                    for record in _read_records(evidence_path)
                ],
                [
                    ("android-control-reconnect", "PASS"),
                    ("three-update-cycles", "FAIL"),
                ],
            )
            self.assertEqual(
                _read_records(evidence_path)[-1]["facts"],
                {
                    "error": "hardware_gate_failed",
                    "phase": "update_cycle_1",
                },
            )

            with contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(2),
            ) as retry_runner:
                retry_result = acceptance.main([
                    *argv[:3],
                    "2",
                    *argv[4:],
                ])

            self.assertEqual(retry_result, 2)
            retry_runner.assert_not_called()
            self.assertEqual(reservation_path.read_bytes(), replacement)

    def test_pass_swap_at_reservation_unlink_recreates_durable_blocker(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            detached_path = root / "detached-evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            _record_gate_one(evidence_path)
            original = evidence_path.read_bytes()
            reservation_path = root / (
                f".{evidence_path.name}.mutating-gate-reservation"
            )
            argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/pass-unlink-race",
                "--session-file", str(session_path),
                "--evidence", str(evidence_path),
            ]
            real_unlink = os.unlink
            swapped = False

            def swap_evidence_then_unlink(
                path: str,
                *args: object,
                **kwargs: object,
            ) -> None:
                nonlocal swapped
                if Path(path).name == reservation_path.name and not swapped:
                    evidence_path.rename(detached_path)
                    evidence_path.write_bytes(original)
                    os.chmod(evidence_path, 0o600)
                    swapped = True
                real_unlink(path, *args, **kwargs)

            with contextlib.redirect_stdout(
                io.StringIO()
            ), contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance.os,
                "unlink",
                side_effect=swap_evidence_then_unlink,
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1),
            ) as runner:
                result = acceptance.main(argv)

            self.assertEqual(result, 2)
            self.assertTrue(swapped)
            self.assertEqual(runner.call_count, 1)
            self.assertEqual(evidence_path.read_bytes(), original)
            self.assertEqual(
                [
                    (record["gate"], record["status"])
                    for record in _read_records(detached_path)
                ],
                [
                    ("android-control-reconnect", "PASS"),
                    ("three-update-cycles", "CHECKPOINT"),
                ],
            )
            self.assertTrue(reservation_path.is_file())
            self.assertEqual(reservation_path.stat().st_mode & 0o077, 0)

            with contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1),
            ) as retry_runner:
                retry_result = acceptance.main(argv)

            self.assertEqual(retry_result, 2)
            retry_runner.assert_not_called()

    def test_fail_swap_at_reservation_unlink_recreates_durable_blocker(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            detached_path = root / "detached-evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            _record_gate_one(evidence_path)
            original = evidence_path.read_bytes()
            reservation_path = root / (
                f".{evidence_path.name}.mutating-gate-reservation"
            )
            argv = [
                "--gate", "three-update-cycles",
                "--cycle", "1",
                "--port", "/dev/fail-unlink-race",
                "--session-file", str(session_path),
                "--evidence", str(evidence_path),
            ]
            real_unlink = os.unlink
            swapped = False

            def swap_evidence_then_unlink(
                path: str,
                *args: object,
                **kwargs: object,
            ) -> None:
                nonlocal swapped
                if Path(path).name == reservation_path.name and not swapped:
                    evidence_path.rename(detached_path)
                    evidence_path.write_bytes(original)
                    os.chmod(evidence_path, 0o600)
                    swapped = True
                real_unlink(path, *args, **kwargs)

            with contextlib.redirect_stdout(
                io.StringIO()
            ), contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance.os,
                "unlink",
                side_effect=swap_evidence_then_unlink,
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                side_effect=RuntimeError("hardware action failed"),
            ) as runner:
                result = acceptance.main(argv)

            self.assertEqual(result, 2)
            self.assertTrue(swapped)
            self.assertEqual(runner.call_count, 1)
            self.assertEqual(evidence_path.read_bytes(), original)
            failure = _read_records(detached_path)[-1]
            self.assertEqual(
                (failure["gate"], failure["status"], failure["facts"]),
                (
                    "three-update-cycles",
                    "FAIL",
                    {
                        "error": "hardware_gate_failed",
                        "phase": "update_cycle_1",
                    },
                ),
            )
            self.assertTrue(reservation_path.is_file())
            self.assertEqual(reservation_path.stat().st_mode & 0o077, 0)

            with contextlib.redirect_stderr(
                io.StringIO()
            ), mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                return_value=_verify_cycle_fixture(1),
            ) as retry_runner:
                retry_result = acceptance.main(argv)

            self.assertEqual(retry_result, 2)
            retry_runner.assert_not_called()

    def test_marker_swap_at_unlink_recreates_canonical_blocker(
        self,
    ) -> None:
        scenarios = (
            ("pass", _verify_cycle_fixture(1)),
            ("fail", RuntimeError("hardware action failed")),
        )
        for outcome, runner_result in scenarios:
            with self.subTest(outcome=outcome), \
                    tempfile.TemporaryDirectory() as td:
                root = Path(td)
                evidence_path = root / "evidence.jsonl"
                session_path = _write_private_session_anchor(root)
                _record_gate_one(evidence_path)
                reservation_path = root / (
                    f".{evidence_path.name}.mutating-gate-reservation"
                )
                orphaned_reservation = root / "orphaned-reservation"
                replacement = b"replacement marker\n"
                argv = [
                    "--gate", "three-update-cycles",
                    "--cycle", "1",
                    "--port", "/dev/marker-unlink-race",
                    "--session-file", str(session_path),
                    "--evidence", str(evidence_path),
                ]
                real_unlink = os.unlink
                swapped = False

                def swap_marker_then_unlink(
                    path: str,
                    *args: object,
                    **kwargs: object,
                ) -> None:
                    nonlocal swapped
                    if Path(path).name == reservation_path.name and \
                            not swapped:
                        reservation_path.rename(orphaned_reservation)
                        reservation_path.write_bytes(replacement)
                        os.chmod(reservation_path, 0o600)
                        swapped = True
                    real_unlink(path, *args, **kwargs)

                runner_kwargs = (
                    {"return_value": runner_result}
                    if outcome == "pass"
                    else {"side_effect": runner_result}
                )
                registry_anchor = (
                    self._retained_state_anchor
                    / f"operation-registry-{outcome}"
                )
                registry_anchor.mkdir(mode=0o700)
                with contextlib.redirect_stdout(
                    io.StringIO()
                ), contextlib.redirect_stderr(
                    io.StringIO()
                ), mock.patch.object(
                    acceptance,
                    "_retained_state_root_spec",
                    return_value=(
                        self._retained_state_anchor,
                        ("app-parent", f"fixed-state-{outcome}"),
                    ),
                    create=True,
                ), mock.patch.object(
                    acceptance,
                    "_operation_registry_anchor_spec",
                    return_value=(registry_anchor, False),
                    create=True,
                ), mock.patch.object(
                    acceptance.os,
                    "unlink",
                    side_effect=swap_marker_then_unlink,
                ), mock.patch.object(
                    acceptance,
                    "run_update_cycle_checkpoint",
                    **runner_kwargs,
                ) as runner:
                    result = acceptance.main(argv)

                self.assertEqual(result, 2)
                self.assertTrue(swapped)
                self.assertEqual(runner.call_count, 1)
                self.assertTrue(orphaned_reservation.is_file())
                self.assertTrue(reservation_path.is_file())
                self.assertNotEqual(
                    reservation_path.read_bytes(), replacement
                )
                self.assertEqual(
                    reservation_path.stat().st_mode & 0o077,
                    0,
                )

                with contextlib.redirect_stderr(
                    io.StringIO()
                ), mock.patch.object(
                    acceptance,
                    "run_update_cycle_checkpoint",
                    return_value=_verify_cycle_fixture(1),
                ) as retry_runner:
                    retry_result = acceptance.main(argv)

                self.assertEqual(retry_result, 2)
                retry_runner.assert_not_called()

    def test_concurrent_cycle_reservation_allows_only_one_runner(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            first_root = root / "first"
            second_root = root / "second"
            first_root.mkdir(mode=0o700)
            second_root.mkdir(mode=0o700)
            first_evidence = first_root / "evidence.jsonl"
            first_session = _write_private_session_anchor(first_root)
            _record_gate_one(first_evidence)
            second_evidence = second_root / "evidence-copy.jsonl"
            second_evidence.write_bytes(first_evidence.read_bytes())
            os.chmod(second_evidence, 0o600)
            second_session = _write_private_session_anchor(second_root)
            checkpoint = _verify_cycle_fixture(1)
            argvs = {
                "first": [
                    "--gate", "three-update-cycles",
                    "--cycle", "1",
                    "--port", "/dev/concurrent-badge-first",
                    "--session-file", str(first_session),
                    "--evidence", str(first_evidence),
                ],
                "second": [
                    "--gate", "three-update-cycles",
                    "--cycle", "1",
                    "--port", "/dev/concurrent-badge-second",
                    "--session-file", str(second_session),
                    "--evidence", str(second_evidence),
                ],
            }
            first_started = threading.Event()
            release_first = threading.Event()
            second_mutated = threading.Event()
            results: dict[str, int] = {}
            run_count = 0
            run_count_lock = threading.Lock()

            def run_cycle(
                _port: str,
                _session_value: acceptance.BadgeAcceptanceSession,
                _cycle: int,
                **_kwargs: object,
            ) -> acceptance.VerifiedCycleCheckpoint:
                nonlocal run_count
                with run_count_lock:
                    run_count += 1
                    call_number = run_count
                if call_number == 1:
                    first_started.set()
                    if not release_first.wait(3):
                        raise RuntimeError("test did not release first runner")
                else:
                    second_mutated.set()
                return checkpoint

            def invoke(name: str) -> None:
                results[name] = acceptance.main(argvs[name])

            with mock.patch.object(
                acceptance,
                "run_update_cycle_checkpoint",
                side_effect=run_cycle,
            ) as runner:
                first = threading.Thread(
                    target=invoke, args=("first",), daemon=True
                )
                second = threading.Thread(
                    target=invoke, args=("second",), daemon=True
                )
                first.start()
                self.assertTrue(first_started.wait(3))
                second.start()
                mutated_while_reserved = second_mutated.wait(1)
                release_first.set()
                first.join(3)
                second.join(3)

            self.assertFalse(first.is_alive())
            self.assertFalse(second.is_alive())
            self.assertFalse(mutated_while_reserved)
            self.assertEqual(runner.call_count, 1)
            self.assertEqual(sorted(results.values()), [0, 2])
            self.assertEqual(len(self._registry_fences()), 1)

    def test_completion_cli_is_read_only_and_requires_exact_version(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "acceptance.jsonl"
            _record_complete_evidence(path)
            before = path.read_bytes()
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout), mock.patch.object(
                acceptance,
                "_anchored_cli_session",
                side_effect=AssertionError(
                    "completion audit must use its one locked evidence read"
                ),
            ):
                result = acceptance.main([
                    "--verify-complete",
                    "--evidence", str(path),
                    "--session-id", _session().session_id,
                    "--expected-version", VERSION,
                ])
            after = path.read_bytes()

        self.assertEqual(result, 0)
        self.assertEqual(after, before)
        output = json.loads(stdout.getvalue())
        self.assertTrue(output["ok"])
        self.assertEqual(output["version"], VERSION)
        self.assertEqual(output["update_cycles"], 3)

    def test_stale_reservation_blocks_manual_write_and_completion_audit(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "acceptance.jsonl"
            _record_complete_evidence(evidence_path)
            original = evidence_path.read_bytes()
            reservation_path = root / (
                f".{evidence_path.name}.mutating-gate-reservation"
            )
            reservation_path.write_text("{}\n", encoding="utf-8")
            os.chmod(reservation_path, 0o600)

            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "reservation"
            ):
                acceptance.record_gate(
                    evidence_path,
                    _session(),
                    "power-state-audit",
                    "FAIL",
                    {
                        "error": "operator_gate_failed",
                        "phase": "power_audit",
                    },
                )
            self.assertEqual(evidence_path.read_bytes(), original)

            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "reservation"
            ):
                acceptance.verify_acceptance_evidence(
                    evidence_path, _session(), VERSION
                )
            self.assertEqual(evidence_path.read_bytes(), original)

    def test_stale_reservation_blocks_direct_cycle_checkpoint_write(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "acceptance.jsonl"
            _record_gate_one(evidence_path)
            original = evidence_path.read_bytes()
            reservation_path = root / (
                f".{evidence_path.name}.mutating-gate-reservation"
            )
            reservation_path.write_text("{}\n", encoding="utf-8")
            os.chmod(reservation_path, 0o600)

            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "reservation"
            ):
                acceptance.record_update_cycle_checkpoint(
                    evidence_path,
                    _session(),
                    _verify_cycle_fixture(1),
                )

            self.assertEqual(evidence_path.read_bytes(), original)

    def test_public_checkpoint_writer_rejects_raw_fd_marker_bypass(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "acceptance.jsonl"
            _record_gate_one(evidence_path)
            original = evidence_path.read_bytes()
            reservation_path = root / (
                f".{evidence_path.name}.mutating-gate-reservation"
            )
            reservation_path.write_text("{}\n", encoding="utf-8")
            os.chmod(reservation_path, 0o600)
            fd = os.open(evidence_path, os.O_RDWR | os.O_APPEND)
            try:
                with self.assertRaises(TypeError):
                    acceptance.record_update_cycle_checkpoint(
                        evidence_path,
                        _session(),
                        _verify_cycle_fixture(1),
                        _locked_fd=fd,
                    )
            finally:
                os.close(fd)

            self.assertEqual(evidence_path.read_bytes(), original)
            self.assertTrue(reservation_path.is_file())

    def test_manual_pass_builds_snapshot_from_live_bound_badge_status(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            session_path = root / "session.json"
            facts_path = root / "facts.json"
            evidence_path = root / "evidence.jsonl"
            session_path.write_text(json.dumps({
                "session_id": _session().session_id,
                "uplink_hardware_id": UPLINK_ID,
                "ble_hardware_id": BLE_ID,
                "wifi_hardware_id": WIFI_ID,
            }), encoding="utf-8")
            facts = _pass_facts("power-state-audit")
            facts.pop("snapshot")
            facts_path.write_text(json.dumps(facts), encoding="utf-8")
            _record_gate_four(evidence_path)
            acceptance.record_gate(
                evidence_path,
                _session(),
                "no-host-reboot",
                "PASS",
                _pass_facts("no-host-reboot"),
            )
            badge = mock.MagicMock()
            badge.__enter__.return_value = badge
            badge.__exit__.return_value = None
            badge.status.return_value = _status()
            descriptor = _usb_record("/dev/live-badge")
            with mock.patch.object(
                acceptance.flash, "BadgeSerial", return_value=badge
            ) as badge_serial, mock.patch.object(
                acceptance,
                "_trusted_session_uplink_descriptor",
                return_value=descriptor,
            ) as trusted_descriptor:
                result = acceptance.main([
                    "--gate", "power-state-audit",
                    "--port", "/dev/live-badge",
                    "--session-file", str(session_path),
                    "--facts-file", str(facts_path),
                    "--expected-version", VERSION,
                    "--evidence", str(evidence_path),
                ])

            record = _read_records(evidence_path)[-1]
        self.assertEqual(result, 0)
        trusted_descriptor.assert_called_once_with(
            "/dev/live-badge", _session()
        )
        badge_serial.assert_called_once_with(
            descriptor,
            False,
            expected_hardware_id=_session().uplink_hardware_id,
        )
        self.assertEqual(record["facts"]["snapshot"]["version"], VERSION)
        self.assertEqual(
            record["facts"]["snapshot"]["usb_parser_state"], "command"
        )

    def test_manual_gate_one_records_the_reachable_pre_update_snapshot(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            session_path = root / "session.json"
            facts_path = root / "facts.json"
            evidence_path = root / "evidence.jsonl"
            session_path.write_text(json.dumps({
                "session_id": _session().session_id,
                "uplink_hardware_id": UPLINK_ID,
                "ble_hardware_id": BLE_ID,
                "wifi_hardware_id": WIFI_ID,
            }), encoding="utf-8")
            facts = _pass_facts("android-control-reconnect")
            facts.pop("snapshot")
            facts.pop("candidate_artifacts")
            facts_path.write_text(json.dumps(facts), encoding="utf-8")
            pre_update_status = _cycle_source_status(1)
            badge = mock.MagicMock()
            badge.__enter__.return_value = badge
            badge.__exit__.return_value = None
            badge.status.return_value = pre_update_status
            descriptor = _usb_record("/dev/live-badge")
            output = io.StringIO()
            errors = io.StringIO()
            with mock.patch.object(
                acceptance.flash, "BadgeSerial", return_value=badge
            ), mock.patch.object(
                acceptance,
                "_trusted_session_uplink_descriptor",
                return_value=descriptor,
            ), contextlib.redirect_stdout(output), \
                    contextlib.redirect_stderr(errors):
                result = acceptance.main([
                    "--gate", "android-control-reconnect",
                    "--port", "/dev/live-badge",
                    "--session-file", str(session_path),
                    "--facts-file", str(facts_path),
                    "--expected-version", VERSION,
                    "--evidence", str(evidence_path),
                ])

            record = (
                json.loads(evidence_path.read_text(encoding="utf-8").strip())
                if evidence_path.exists() else None
            )

        self.assertEqual(result, 0, errors.getvalue())
        self.assertIsNotNone(record)
        snapshot = record["facts"]["snapshot"]
        self.assertEqual(snapshot["candidate_version"], VERSION)
        self.assertEqual(
            snapshot["ble_version"], "0.64.75-badge-defcon34"
        )
        self.assertEqual(
            snapshot["wifi_version"], "0.64.75-badge-defcon34"
        )
        canary_platform = acceptance.flash.PLATFORMS[
            CANARY_PLATFORM_KEY
        ]
        self.assertTrue(acceptance.flash.repo_version.call_args_list)
        self.assertTrue(acceptance.flash.require_artifacts.call_args_list)
        self.assertTrue(
            acceptance.flash._prepare_frozen_usb_firmware_artifacts.
            call_args_list
        )
        for platform_call in (
            *acceptance.flash.repo_version.call_args_list,
            *acceptance.flash.require_artifacts.call_args_list,
            *acceptance.flash._prepare_frozen_usb_firmware_artifacts.
            call_args_list,
        ):
            self.assertIs(platform_call.args[0], canary_platform)

    def test_manual_fail_records_failure_and_returns_nonzero(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            session_path = root / "session.json"
            facts_path = root / "facts.json"
            evidence_path = root / "evidence.jsonl"
            session_path.write_text(json.dumps({
                "session_id": _session().session_id,
                "uplink_hardware_id": UPLINK_ID,
                "ble_hardware_id": BLE_ID,
                "wifi_hardware_id": WIFI_ID,
            }), encoding="utf-8")
            facts_path.write_text(
                json.dumps({"error": "operator_gate_failed"}),
                encoding="utf-8",
            )
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                result = acceptance.main([
                    "--gate", "power-state-audit",
                    "--status", "FAIL",
                    "--session-file", str(session_path),
                    "--facts-file", str(facts_path),
                    "--evidence", str(evidence_path),
                ])
            record = json.loads(
                evidence_path.read_text(encoding="utf-8").strip()
            )
        self.assertEqual(result, 1)
        self.assertEqual(record["status"], "FAIL")
        self.assertFalse(json.loads(output.getvalue())["ok"])

    def test_interrupted_session_conflict_is_rejected_before_gate_runs(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            evidence_path = root / "evidence.jsonl"
            session_path = _write_private_session_anchor(root)
            acceptance.record_gate(
                evidence_path,
                _session(),
                "android-control-reconnect",
                "PASS",
                _pass_facts("android-control-reconnect"),
            )
            session_path.write_text(json.dumps({
                "session_id": _session().session_id,
                "uplink_hardware_id": UPLINK_ID,
                "ble_hardware_id": BLE_ID,
                "wifi_hardware_id": "02:00:00:00:00:04",
            }), encoding="utf-8")
            os.chmod(session_path, 0o600)
            with mock.patch.object(
                acceptance, "run_interrupted_upload_gate"
            ) as run_gate:
                result = acceptance.main([
                    "--gate", "interrupted-upload",
                    "--port", "/dev/fake",
                    "--session-file", str(session_path),
                    "--evidence", str(evidence_path),
                ])
        self.assertEqual(result, 2)
        run_gate.assert_not_called()


class InterruptedUploadTest(unittest.TestCase):
    def test_partial_upload_rejects_non_canary_identity_before_badge_io(
        self,
    ) -> None:
        cases = (
            (
                acceptance.flash.PLATFORMS["badge-trio-xiao-s3"],
                CANARY_VERSION,
                _frozen_uplink_artifacts(),
                "0123456789ABCDEF",
            ),
            (
                acceptance.flash.PLATFORMS[CANARY_PLATFORM_KEY],
                LEGACY_VERSION,
                _frozen_uplink_artifacts(
                    _uplink_firmware_image(version=LEGACY_VERSION)
                ),
                None,
            ),
        )
        for platform, version, artifacts, update_session in cases:
            badge = mock.Mock()
            badge.write_line.side_effect = AssertionError(
                "badge I/O must remain unreachable"
            )
            with self.subTest(
                platform=platform["uplink_env"],
                version=version,
            ), self.assertRaises(acceptance.AcceptanceError):
                acceptance.interrupt_uplink_upload(
                    badge,
                    platform,
                    artifacts,
                    version,
                    running_partition="ota_0",
                    update_session=update_session,
                )
            badge.write_line.assert_not_called()
            badge.read_prefixed_json.assert_not_called()
            self.assertFalse(hasattr(badge, "ser") and badge.ser.write.called)

    def setUp(self) -> None:
        patcher = mock.patch.object(
            acceptance,
            "_trusted_session_uplink_descriptor",
            side_effect=lambda port, session: _usb_record(
                port, session.uplink_hardware_id
            ),
        )
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_prepare_helper_uses_real_signature_and_exact_source_proof(
        self,
    ) -> None:
        badge = mock.create_autospec(
            acceptance.flash.BadgeSerial,
            instance=True,
        )
        badge.prepare_update_maintenance.return_value = {
            "ok": True,
            "phase": "rebooting",
            "session": "0123456789ABCDEF",
            "retryable": True,
            "reboot_required": True,
        }

        receipt = acceptance._prepare_interrupted_upload_maintenance(
            badge,
            "0123456789ABCDEF",
            deadline=123.0,
            source_version=CANARY_VERSION,
        )

        self.assertEqual(receipt["phase"], "rebooting")
        badge.prepare_update_maintenance.assert_called_once_with(
            "0123456789ABCDEF",
            deadline=123.0,
            source_supports_update_maintenance=True,
        )
        for unsupported in (
            V078_VERSION,
            "0.64.790-badge-defcon34",
            "0.64.79-badge-defcon34-near-miss",
            "",
        ):
            with self.subTest(unsupported=unsupported), self.assertRaises(
                acceptance.AcceptanceError
            ):
                acceptance._prepare_interrupted_upload_maintenance(
                    badge,
                    "0123456789ABCDEF",
                    deadline=123.0,
                    source_version=unsupported,
                )
        self.assertEqual(
            badge.prepare_update_maintenance.call_count,
            1,
        )

    def test_gate_freezes_before_usb_and_reuses_exact_bound_inputs(
        self,
    ) -> None:
        artifacts = _frozen_uplink_artifacts()
        initial_descriptor = _usb_record("/dev/initial")
        rebound_descriptor = dataclasses.replace(
            initial_descriptor,
            device="/dev/rebound",
            stat_inode=initial_descriptor.stat_inode + 1,
        )
        final_descriptor = dataclasses.replace(
            initial_descriptor,
            device="/dev/final",
            stat_inode=initial_descriptor.stat_inode + 2,
        )
        recovered_status = _status()
        final_status = _status()
        final_status["running_partition"] = "ota_1"
        events: list[str] = []
        usb_opened = False

        class StrictBadge:
            def __init__(
                inner_self,
                *,
                status: dict | None = None,
                retry: bool = False,
            ) -> None:
                inner_self._status = status
                inner_self._retry = retry
                inner_self.upload_call: tuple | None = None

            def __enter__(inner_self):
                return inner_self

            def __exit__(
                inner_self,
                _exc_type,
                _exc,
                _traceback,
            ) -> None:
                return None

            def status(inner_self, timeout_s: int = 5) -> dict:
                self.assertEqual(timeout_s, 5)
                self.assertIsNotNone(inner_self._status)
                return copy.deepcopy(inner_self._status)

            def upload_uplink_firmware(
                inner_self,
                platform: dict,
                frozen: artifact_tree.FrozenArtifactSet,
                version: str,
                recovery_rewrite_same_version: bool = False,
            ) -> dict:
                self.assertTrue(inner_self._retry)
                self.assertIs(
                    platform,
                    acceptance.flash.PLATFORMS[
                        CANARY_PLATFORM_KEY
                    ],
                )
                self.assertIs(frozen, artifacts)
                self.assertEqual(version, VERSION)
                self.assertIs(recovery_rewrite_same_version, True)
                inner_self.upload_call = (
                    platform,
                    frozen,
                    version,
                    recovery_rewrite_same_version,
                )
                return {"phase": "committed"}

        baseline_badge = StrictBadge(status=_status())
        retry_badge = StrictBadge(retry=True)
        final_badge = StrictBadge(status=final_status)
        badges = iter((baseline_badge, retry_badge, final_badge))
        badge_open_calls: list[tuple] = []

        def badge_serial(
            descriptor: acceptance.flash.UsbDescriptorRecord,
            exclusive: bool,
            *,
            expected_hardware_id: str,
        ) -> StrictBadge:
            nonlocal usb_opened
            self.assertIs(
                descriptor,
                (
                    initial_descriptor,
                    rebound_descriptor,
                    final_descriptor,
                )[len(badge_open_calls)],
            )
            self.assertIs(exclusive, False)
            self.assertEqual(expected_hardware_id, UPLINK_ID)
            events.append(f"open:{descriptor.device}")
            usb_opened = True
            badge_open_calls.append(
                (descriptor, exclusive, expected_hardware_id)
            )
            return next(badges)

        def prepare_snapshot(*_args, **_kwargs):
            self.assertFalse(usb_opened)
            events.extend(("prepare", "freeze", "close"))
            return acceptance.flash.FrozenUsbFirmwareArtifacts(
                uplink=artifacts,
                scanner=_frozen_artifacts(_scanner_firmware_image()),
            )

        def trusted_descriptor(
            port: str,
            session: acceptance.BadgeAcceptanceSession,
        ) -> acceptance.flash.UsbDescriptorRecord:
            self.assertEqual(port, "/dev/initial")
            self.assertEqual(session, _session())
            self.assertEqual(events[:3], ["prepare", "freeze", "close"])
            events.append("descriptor")
            return initial_descriptor

        def partial_upload(
            badge: StrictBadge,
            platform: dict,
            frozen: artifact_tree.FrozenArtifactSet,
            version: str,
            *,
            running_partition: str,
            abort_after: int,
        ) -> int:
            self.assertIs(badge, baseline_badge)
            self.assertIs(
                platform,
                acceptance.flash.PLATFORMS[
                    CANARY_PLATFORM_KEY
                ],
            )
            self.assertIs(frozen, artifacts)
            self.assertEqual(version, VERSION)
            self.assertEqual(running_partition, "ota_0")
            self.assertEqual(
                abort_after,
                acceptance.INTERRUPTED_UPLOAD_BYTES,
            )
            return acceptance.INTERRUPTED_UPLOAD_BYTES

        def forbid_path_after_usb(method):
            def checked(path: Path, *args, **kwargs):
                if usb_opened:
                    raise AssertionError(
                        f"pathname access after USB open: "
                        f"{method.__name__}({path})"
                    )
                return method(path, *args, **kwargs)
            return checked

        with mock.patch.object(
            acceptance.flash,
            "repo_version",
            return_value=VERSION,
        ), mock.patch.object(
            acceptance.flash,
            "_prepare_frozen_usb_firmware_artifacts",
            side_effect=prepare_snapshot,
        ), mock.patch.object(
            acceptance,
            "_trusted_session_uplink_descriptor",
            side_effect=trusted_descriptor,
        ), mock.patch.object(
            acceptance.flash,
            "BadgeSerial",
            side_effect=badge_serial,
        ), mock.patch.object(
            acceptance,
            "interrupt_uplink_upload",
            side_effect=partial_upload,
        ), mock.patch.object(
            acceptance.flash,
            "wait_for_application_port",
            return_value=(rebound_descriptor, recovered_status),
        ), mock.patch.object(
            acceptance.flash,
            "_classify_uplink_update_receipt",
            return_value=object(),
        ), mock.patch.object(
            acceptance.flash,
            "wait_for_post_uplink_application",
            return_value=(final_descriptor, object()),
        ), mock.patch.object(
            acceptance,
            "_run_interrupted_upload_gate_in_update_maintenance",
            return_value=(
                _session(),
                _pass_facts("interrupted-upload"),
            ),
        ) as maintenance_flow, mock.patch.object(
            Path,
            "read_bytes",
            new=forbid_path_after_usb(Path.read_bytes),
        ), mock.patch.object(
            Path,
            "open",
            new=forbid_path_after_usb(Path.open),
        ), mock.patch.object(
            Path,
            "stat",
            new=forbid_path_after_usb(Path.stat),
        ), mock.patch.object(
            Path,
            "exists",
            new=forbid_path_after_usb(Path.exists),
        ):
            session, facts = acceptance.run_interrupted_upload_gate(
                "/dev/initial",
                expected_session=_session(),
                sleep=lambda _seconds: None,
                updater_baseline=(
                    LivePromotionMetricsTest._issued_baseline()
                ),
            )

        self.assertEqual(session, _session())
        self.assertTrue(facts["retry_succeeded"])
        self.assertEqual(
            events,
            ["prepare", "freeze", "close", "descriptor"],
        )
        self.assertEqual(len(badge_open_calls), 0)
        maintenance_flow.assert_called_once()
        maintenance_kwargs = maintenance_flow.call_args.kwargs
        self.assertIs(maintenance_kwargs["artifacts"], artifacts)
        self.assertIs(
            maintenance_kwargs["platform"],
            acceptance.flash.PLATFORMS[CANARY_PLATFORM_KEY],
        )
        self.assertEqual(maintenance_kwargs["version"], CANARY_VERSION)

    def test_maintenance_gate_reuses_one_exact_session_and_finishes_normal(
        self,
    ) -> None:
        version = acceptance.flash.UPDATE_MAINTENANCE_MIN_VERSION
        update_session = "0123456789ABCDEF"
        image = _uplink_firmware_image(version=version)
        image_sha256 = hashlib.sha256(image).hexdigest()
        artifacts = acceptance.flash.FrozenUsbFirmwareArtifacts(
            uplink=_frozen_artifacts(image),
            scanner=_frozen_artifacts(
                _scanner_firmware_image(version=version)
            ),
        )
        descriptor = _usb_record("/dev/initial")
        updater_baseline = LivePromotionMetricsTest._issued_baseline()
        baseline = _status(
            version=version,
            partition="ota_0",
            reboot_generation=50,
            responses_completed=31,
            reboot_reason="update_finish",
        )
        recovered = _status(
            version=version,
            partition="ota_0",
            reboot_generation=52,
            responses_completed=1,
            reboot_reason="update_abort",
        )
        final = _status(
            version=version,
            partition="ota_1",
            reboot_generation=55,
            responses_completed=1,
            reboot_reason="update_finish",
        )
        maintenance_idle = _maintenance_status(
            version=version,
            update_session=update_session,
            partition="ota_0",
            reboot_generation=51,
            responses_completed=1,
        )
        maintenance_partial = _maintenance_status(
            version=version,
            update_session=update_session,
            partition="ota_0",
            uplink_phase="receiving",
            uplink_received=acceptance.INTERRUPTED_UPLOAD_BYTES,
            uplink_size=len(image),
            uplink_sha256=image_sha256,
            reboot_generation=51,
            responses_completed=2,
        )
        maintenance_partial["uptime_s"] = 6
        maintenance_partial["usb_health"]["rx_bytes"] = 1025
        maintenance_retry = _maintenance_status(
            version=version,
            update_session=update_session,
            partition="ota_0",
            reboot_generation=53,
            responses_completed=1,
        )
        maintenance_committed = _maintenance_status(
            version=version,
            update_session=update_session,
            partition="ota_1",
            uplink_phase="committed",
            uplink_received=len(image),
            uplink_size=len(image),
            uplink_sha256=image_sha256,
            reboot_generation=54,
            responses_completed=1,
        )
        maintenance_wait = copy.deepcopy(maintenance_committed)
        maintenance_wait["uptime_s"] = 6
        maintenance_wait["usb_health"]["rx_bytes"] = 1025
        maintenance_wait["usb_health"]["valid_commands"] = 2
        maintenance_wait["usb_health"]["responses_completed"] = 2
        events: list[tuple[str, object]] = []
        maintenance_reconnects = iter((
            maintenance_idle,
            maintenance_partial,
            maintenance_retry,
            maintenance_committed,
        ))
        normal_reconnects = iter((recovered, final))
        status_results = iter((baseline, maintenance_wait))

        class StrictMaintenanceBadge:
            expected_hardware_id = UPLINK_ID

            def __enter__(inner_self):
                return inner_self

            def __exit__(inner_self, *_args):
                return None

            def status(
                inner_self, timeout_s: float = 5
            ) -> dict:
                self.assertGreater(timeout_s, 0)
                value = copy.deepcopy(next(status_results))
                events.append(("status", value["recovery_mode"]))
                return value

            def prepare_update_maintenance(
                inner_self,
                session: str,
                *,
                deadline: float,
                source_supports_update_maintenance: bool,
            ) -> dict:
                self.assertGreater(deadline, 0)
                self.assertEqual(session, update_session)
                self.assertIs(source_supports_update_maintenance, True)
                events.append(("prepare", session))
                return {
                    "ok": True,
                    "phase": "rebooting",
                    "session": session,
                    "retryable": True,
                    "reboot_required": True,
                }

            def reconnect_same_uplink(
                inner_self, *, deadline: float
            ) -> dict:
                self.assertGreater(deadline, 0)
                value = copy.deepcopy(next(maintenance_reconnects))
                events.append(("maintenance", value["update_session"]))
                return value

            def _close_serial(inner_self) -> None:
                events.append(("close", update_session))

            def reconcile_uplink_ota(
                inner_self, expected: dict
            ) -> str:
                self.assertEqual(expected, {
                    "session": update_session,
                    "version": version,
                    "sha256": image_sha256,
                    "size": len(image),
                    "partition": "ota_1",
                })
                events.append(("reconcile", expected["session"]))
                return "restart_from_zero"

            def abort_update_maintenance(
                inner_self, *, deadline: float
            ) -> dict:
                self.assertGreater(deadline, 0)
                events.append(("abort", update_session))
                return {
                    "ok": True,
                    "phase": "aborting",
                    "session": update_session,
                    "retryable": False,
                    "reboot_required": True,
                }

            def reconnect_same_uplink_normal(
                inner_self, *, deadline: float
            ) -> dict:
                self.assertGreater(deadline, 0)
                value = copy.deepcopy(next(normal_reconnects))
                events.append(("normal", value["running_partition"]))
                return value

            def upload_uplink_firmware(
                inner_self,
                platform,
                frozen,
                candidate_version,
                recovery_rewrite_same_version,
                *,
                maintenance_status_validator,
            ) -> dict:
                self.assertIs(
                    platform,
                    acceptance.flash.PLATFORMS[
                        CANARY_PLATFORM_KEY
                    ],
                )
                self.assertIs(frozen, artifacts.uplink)
                self.assertEqual(candidate_version, version)
                self.assertIs(recovery_rewrite_same_version, True)
                self.assertTrue(callable(maintenance_status_validator))
                events.append(("upload", update_session))
                return {
                    "ok": True,
                    "phase": "committed",
                    "partition": "ota_1",
                    "received": len(image),
                    "total": len(image),
                    "credit_bytes": 0,
                    "retryable": False,
                    "reboot_required": True,
                    "error": "",
                }

            def finish_update_maintenance(
                inner_self, *, deadline: float
            ) -> dict:
                self.assertGreater(deadline, 0)
                events.append(("finish", update_session))
                return {
                    "ok": True,
                    "phase": "finishing",
                    "session": update_session,
                    "retryable": False,
                    "reboot_required": True,
                }

        def interrupt(*_args, **kwargs) -> int:
            self.assertEqual(kwargs["update_session"], update_session)
            events.append(("interrupt", kwargs["update_session"]))
            return acceptance.INTERRUPTED_UPLOAD_BYTES

        real_normal_gate = acceptance.verify_canary_normal_live_metrics
        real_maintenance_gate = \
            acceptance.verify_canary_maintenance_live_metrics

        def normal_gate(*args, **kwargs):
            events.append(("live-normal", args[0]["recovery_mode"]))
            return real_normal_gate(*args, **kwargs)

        def maintenance_gate(*args, **kwargs):
            events.append(("live-maintenance", args[0]["recovery_mode"]))
            return real_maintenance_gate(*args, **kwargs)

        with mock.patch.object(
            acceptance.flash, "_new_update_session",
            return_value=update_session,
        ), mock.patch.object(
            acceptance.flash, "BadgeSerial",
            return_value=StrictMaintenanceBadge(),
        ) as badge_factory, mock.patch.object(
            acceptance, "interrupt_uplink_upload",
            side_effect=interrupt,
        ), mock.patch.object(
            acceptance,
            "verify_canary_normal_live_metrics",
            side_effect=normal_gate,
        ), mock.patch.object(
            acceptance,
            "verify_canary_maintenance_live_metrics",
            side_effect=maintenance_gate,
        ):
            session, facts = acceptance.run_interrupted_upload_gate(
                descriptor.device,
                expected_session=_session(),
                sleep=lambda _seconds: None,
                expected_version=version,
                frozen_artifacts=artifacts,
                updater_baseline=updater_baseline,
            )

        self.assertEqual(session, _session())
        self.assertEqual(facts["snapshot"]["running_partition"], "ota_1")
        self.assertEqual(
            facts["recovered_snapshot"]["running_partition"], "ota_0"
        )
        self.assertEqual(
            facts["baseline_snapshot"]["last_expected_reboot_reason"],
            "update_finish",
        )
        self.assertEqual(
            facts["recovered_snapshot"]["last_expected_reboot_reason"],
            "update_abort",
        )
        self.assertEqual(
            facts["snapshot"]["last_expected_reboot_reason"],
            "update_finish",
        )
        badge_factory.assert_called_once()
        session_events = [
            value for name, value in events
            if name in {
                "prepare", "maintenance", "close", "reconcile", "abort",
                "interrupt", "upload", "finish",
            }
        ]
        self.assertEqual(set(session_events), {update_session})
        names = [name for name, _value in events]
        self.assertEqual(names.count("prepare"), 2)
        self.assertLess(names.index("live-normal"), names.index("prepare"))
        self.assertLess(
            names.index("live-maintenance"),
            names.index("interrupt"),
        )
        second_normal = names.index(
            "live-normal", names.index("live-normal") + 1
        )
        second_prepare = names.index("prepare", names.index("prepare") + 1)
        self.assertLess(second_normal, second_prepare)
        maintenance_before_upload = max(
            index for index, name in enumerate(names[:names.index("upload")])
            if name == "live-maintenance"
        )
        self.assertLess(maintenance_before_upload, names.index("upload"))
        self.assertLess(names.index("interrupt"), names.index("reconcile"))
        self.assertLess(names.index("reconcile"), names.index("abort"))
        self.assertLess(names.index("abort"), names.index("upload"))
        self.assertLess(names.index("upload"), names.index("finish"))

    def test_maintenance_gate_rejects_bad_normal_metrics_before_bytes(
        self,
    ) -> None:
        version = acceptance.flash.UPDATE_MAINTENANCE_MIN_VERSION
        artifacts = acceptance.flash.FrozenUsbFirmwareArtifacts(
            uplink=_frozen_artifacts(
                _uplink_firmware_image(version=version)
            ),
            scanner=_frozen_artifacts(
                _scanner_firmware_image(version=version)
            ),
        )
        updater_baseline = LivePromotionMetricsTest._issued_baseline()
        bad_normal = LivePromotionMetricsTest._normal_status()
        bad_normal["heap_internal_free"] = 24575

        class NoMutationBadge:
            def __enter__(inner_self):
                return inner_self

            def __exit__(inner_self, *_args):
                return None

            def status(inner_self, timeout_s: float = 5) -> dict:
                self.assertGreater(timeout_s, 0)
                return copy.deepcopy(bad_normal)

            def prepare_update_maintenance(inner_self, *_args, **_kwargs):
                raise AssertionError("bad normal sample reached prepare")

            def upload_uplink_firmware(inner_self, *_args, **_kwargs):
                raise AssertionError("bad normal sample reached upload bytes")

        with mock.patch.object(
            acceptance.flash,
            "BadgeSerial",
            return_value=NoMutationBadge(),
        ), mock.patch.object(
            acceptance,
            "interrupt_uplink_upload",
            side_effect=AssertionError(
                "bad normal sample reached interrupted bytes"
            ),
        ), self.assertRaises(acceptance.AcceptanceError):
            acceptance.run_interrupted_upload_gate(
                "/dev/initial",
                expected_session=_session(),
                sleep=lambda _seconds: None,
                expected_version=version,
                frozen_artifacts=artifacts,
                updater_baseline=updater_baseline,
            )

    def test_maintenance_dispatch_keeps_full_idle_wait_contract(self) -> None:
        artifacts = _frozen_uplink_artifacts()
        initial_descriptor = _usb_record("/dev/initial")
        rebound_descriptor = dataclasses.replace(
            initial_descriptor,
            device="/dev/rebound",
            stat_inode=initial_descriptor.stat_inode + 1,
        )
        final_descriptor = dataclasses.replace(
            initial_descriptor,
            device="/dev/final",
            stat_inode=initial_descriptor.stat_inode + 2,
        )
        final_status = _status()
        final_status["running_partition"] = "ota_1"
        baseline_badge = _StrictInterruptedBadge(status=_status())
        retry_badge = _StrictInterruptedBadge(
            artifacts=artifacts,
            retry=True,
        )
        final_badge = _StrictInterruptedBadge(status=final_status)
        badge_factory = _StrictInterruptedBadgeFactory(
            (
                initial_descriptor,
                rebound_descriptor,
                final_descriptor,
            ),
            (baseline_badge, retry_badge, final_badge),
        )

        def wait_for_healthy_post_ota(
            _expectation: object, *, timeout_s: float
        ) -> tuple[acceptance.flash.UsbDescriptorRecord, object]:
            if timeout_s <= 60:
                raise TimeoutError(
                    "post-OTA deadline ended inside the 60-second "
                    "firmware health window"
                )
            return final_descriptor, object()

        def partial_upload(
            badge: _StrictInterruptedBadge,
            platform: dict,
            frozen: artifact_tree.FrozenArtifactSet,
            version: str,
            *,
            running_partition: str,
            abort_after: int,
        ) -> int:
            self.assertIs(badge, baseline_badge)
            self.assertIs(frozen, artifacts)
            self.assertEqual(version, VERSION)
            self.assertEqual(running_partition, "ota_0")
            self.assertEqual(
                abort_after,
                acceptance.INTERRUPTED_UPLOAD_BYTES,
            )
            return acceptance.INTERRUPTED_UPLOAD_BYTES

        with mock.patch.object(
            acceptance.flash, "repo_version", return_value=VERSION
        ), mock.patch.object(
            acceptance.flash,
            "_prepare_frozen_usb_firmware_artifacts",
            return_value=acceptance.flash.FrozenUsbFirmwareArtifacts(
                uplink=artifacts,
                scanner=_frozen_artifacts(_scanner_firmware_image()),
            ),
        ), mock.patch.object(
            acceptance,
            "_trusted_session_uplink_descriptor",
            return_value=initial_descriptor,
        ), mock.patch.object(
            acceptance.flash,
            "BadgeSerial",
            side_effect=badge_factory,
        ), mock.patch.object(
            acceptance,
            "interrupt_uplink_upload",
            side_effect=partial_upload,
        ), mock.patch.object(
            acceptance.flash,
            "wait_for_application_port",
            return_value=(rebound_descriptor, _status()),
        ), mock.patch.object(
            acceptance.flash,
            "_classify_uplink_update_receipt",
            return_value=object(),
        ), mock.patch.object(
            acceptance.flash,
            "wait_for_post_uplink_application",
            side_effect=wait_for_healthy_post_ota,
        ), mock.patch.object(
            acceptance,
            "_run_interrupted_upload_gate_in_update_maintenance",
            return_value=(
                _session(),
                _pass_facts("interrupted-upload"),
            ),
        ) as maintenance_flow:
            session, facts = acceptance.run_interrupted_upload_gate(
                "/dev/initial",
                expected_session=_session(),
                sleep=lambda _seconds: None,
                updater_baseline=(
                    LivePromotionMetricsTest._issued_baseline()
                ),
            )

        self.assertEqual(session, _session())
        self.assertTrue(facts["retry_succeeded"])
        self.assertEqual(len(badge_factory.calls), 0)
        maintenance_flow.assert_called_once()
        self.assertEqual(
            maintenance_flow.call_args.kwargs["wait_seconds"],
            acceptance.INTERRUPTED_UPLOAD_IDLE_WAIT_S,
        )

    def test_successful_gate_returns_one_fully_recordable_cache_proof(
        self,
    ) -> None:
        artifacts = _frozen_uplink_artifacts()
        initial_descriptor = _usb_record("/dev/initial")
        rebound_descriptor = dataclasses.replace(
            initial_descriptor,
            device="/dev/rebound",
            stat_inode=initial_descriptor.stat_inode + 1,
        )
        final_descriptor = dataclasses.replace(
            initial_descriptor,
            device="/dev/final",
            stat_inode=initial_descriptor.stat_inode + 2,
        )
        final_status = _status()
        final_status["running_partition"] = "ota_1"
        baseline_badge = _StrictInterruptedBadge(status=_status())
        retry_badge = _StrictInterruptedBadge(
            artifacts=artifacts,
            retry=True,
        )
        final_badge = _StrictInterruptedBadge(status=final_status)
        badge_factory = _StrictInterruptedBadgeFactory(
            (
                initial_descriptor,
                rebound_descriptor,
                final_descriptor,
            ),
            (baseline_badge, retry_badge, final_badge),
        )

        def partial_upload(
            badge: _StrictInterruptedBadge,
            platform: dict,
            frozen: artifact_tree.FrozenArtifactSet,
            version: str,
            *,
            running_partition: str,
            abort_after: int,
        ) -> int:
            self.assertIs(badge, baseline_badge)
            self.assertIs(frozen, artifacts)
            self.assertEqual(version, VERSION)
            self.assertEqual(running_partition, "ota_0")
            self.assertEqual(
                abort_after,
                acceptance.INTERRUPTED_UPLOAD_BYTES,
            )
            return acceptance.INTERRUPTED_UPLOAD_BYTES

        with mock.patch.object(
            acceptance.flash, "repo_version", return_value=VERSION
        ), mock.patch.object(
            acceptance.flash,
            "_prepare_frozen_usb_firmware_artifacts",
            return_value=acceptance.flash.FrozenUsbFirmwareArtifacts(
                uplink=artifacts,
                scanner=_frozen_artifacts(_scanner_firmware_image()),
            ),
        ), mock.patch.object(
            acceptance,
            "_trusted_session_uplink_descriptor",
            return_value=initial_descriptor,
        ), mock.patch.object(
            acceptance.flash,
            "BadgeSerial",
            side_effect=badge_factory,
        ), mock.patch.object(
            acceptance,
            "interrupt_uplink_upload",
            side_effect=partial_upload,
        ), mock.patch.object(
            acceptance.flash,
            "wait_for_application_port",
            return_value=(rebound_descriptor, _status()),
        ), mock.patch.object(
            acceptance.flash,
            "_classify_uplink_update_receipt",
            return_value=object(),
        ), mock.patch.object(
            acceptance.flash,
            "wait_for_post_uplink_application",
            return_value=(final_descriptor, object()),
        ), mock.patch.object(
            acceptance,
            "_run_interrupted_upload_gate_in_update_maintenance",
            return_value=(
                _session(),
                _pass_facts("interrupted-upload"),
            ),
        ) as maintenance_flow:
            session, facts = acceptance.run_interrupted_upload_gate(
                "/dev/initial",
                expected_session=_session(),
                sleep=lambda _seconds: None,
                updater_baseline=(
                    LivePromotionMetricsTest._issued_baseline()
                ),
            )

        with tempfile.TemporaryDirectory() as td:
            evidence = Path(td) / "acceptance.jsonl"
            _append_pass_fixture(
                evidence,
                "interrupted-upload",
                session=session,
                facts=facts,
            )
            record = json.loads(
                evidence.read_text(encoding="utf-8").strip()
            )
        self.assertEqual(record["status"], "PASS")
        self.assertEqual(
            record["facts"]["scanner_cache_before"],
            record["facts"]["scanner_cache_after_abort"],
        )
        self.assertEqual(
            record["facts"]["scanner_cache_before"],
            record["facts"]["scanner_cache_after_retry"],
        )
        self.assertEqual(len(badge_factory.calls), 0)
        maintenance_flow.assert_called_once()

    def test_partial_upload_writes_exactly_65536_bytes_then_closes(self) -> None:
        image = bytes((index % 251 for index in range(131072)))
        artifacts = _frozen_uplink_artifacts(
            _uplink_firmware_image(len(image))
        )
        serial = mock.Mock()
        serial.write.side_effect = lambda chunk: len(chunk)
        badge = mock.Mock()
        badge.ser = serial
        badge.read_prefixed_json.side_effect = [
            _uplink_receipt(
                total=len(image),
                phase="ready",
                received=0,
                credit_bytes=4096,
            ),
            *[
                _uplink_receipt(
                    total=len(image),
                    phase="credit",
                    received=offset,
                    credit_bytes=4096,
                )
                for offset in range(4096, 65537, 4096)
            ],
        ]

        result = acceptance.interrupt_uplink_upload(
            badge,
            acceptance.flash.PLATFORMS[CANARY_PLATFORM_KEY],
            artifacts,
            VERSION,
            running_partition="ota_0",
            abort_after=65536,
            update_session="0123456789ABCDEF",
        )

        self.assertEqual(result, 65536)
        self.assertEqual(
            sum(len(call.args[0]) for call in serial.write.call_args_list),
            65536,
        )
        self.assertTrue(
            badge.write_line.call_args.args[0].startswith(
                'FOF_CTL:{"cmd":"uplink_ota_begin"'
            )
        )
        prefixes = [
            call.args[0] for call in badge.read_prefixed_json.call_args_list
        ]
        self.assertTrue(all(prefix == "FOF_UPLINK_OTA:"
                            for prefix in prefixes))
        for receipt_call in badge.read_prefixed_json.call_args_list:
            self.assertEqual(
                receipt_call.kwargs,
                {
                    "allowed_schema_ids": (
                        acceptance.flash.HostJsonSchemaId.UPLINK_OTA,
                    ),
                },
            )
        self.assertEqual(badge.read_prefixed_json.call_count, 17)

    def test_maintenance_partial_upload_binds_exact_session_in_manifest(
        self,
    ) -> None:
        version = acceptance.flash.UPDATE_MAINTENANCE_MIN_VERSION
        update_session = "0123456789ABCDEF"
        image = _uplink_firmware_image(version=version)
        artifacts = _frozen_uplink_artifacts(image)
        serial = mock.Mock()
        serial.write.side_effect = lambda chunk: len(chunk)
        badge = mock.Mock()
        badge.ser = serial
        badge.read_prefixed_json.side_effect = [
            _uplink_receipt(
                total=len(image),
                phase="ready",
                received=0,
                credit_bytes=4096,
            ),
            *[
                _uplink_receipt(
                    total=len(image),
                    phase="credit",
                    received=offset,
                    credit_bytes=4096,
                )
                for offset in range(4096, 65537, 4096)
            ],
        ]

        result = acceptance.interrupt_uplink_upload(
            badge,
            acceptance.flash.PLATFORMS[CANARY_PLATFORM_KEY],
            artifacts,
            version,
            running_partition="ota_0",
            abort_after=65536,
            update_session=update_session,
        )

        self.assertEqual(result, 65536)
        manifest = json.loads(
            badge.write_line.call_args.args[0].removeprefix("FOF_CTL:")
        )
        self.assertEqual(manifest["session"], update_session)
        self.assertEqual(
            set(manifest),
            {
                "cmd", "target", "project", "hardware_type", "version",
                "size", "crc32", "sha256", "flow_control",
                "recovery_rewrite_same_version", "session",
            },
        )

    def test_partial_upload_rejects_stale_malformed_or_committed_final_receipt(
        self,
    ) -> None:
        image = bytes((index % 251 for index in range(131072)))
        artifacts = _frozen_uplink_artifacts(
            _uplink_firmware_image(len(image))
        )
        final_receipts = (
            _uplink_receipt(
                total=len(image),
                phase="credit",
                received=61440,
                credit_bytes=4096,
            ),
            {
                key: value
                for key, value in _uplink_receipt(
                    total=len(image),
                    phase="credit",
                    received=65536,
                    credit_bytes=4096,
                ).items()
                if key != "credit_bytes"
            },
            _uplink_receipt(
                total=len(image),
                phase="committed",
                received=65536,
                credit_bytes=0,
                reboot_required=True,
            ),
        )
        for final_receipt in final_receipts:
            with self.subTest(final_receipt=final_receipt):
                serial = mock.Mock()
                serial.write.side_effect = lambda chunk: len(chunk)
                badge = mock.Mock()
                badge.ser = serial
                badge.read_prefixed_json.side_effect = [
                    _uplink_receipt(
                        total=len(image),
                        phase="ready",
                        received=0,
                        credit_bytes=4096,
                    ),
                    *[
                        _uplink_receipt(
                            total=len(image),
                            phase="credit",
                            received=offset,
                            credit_bytes=4096,
                        )
                        for offset in range(4096, 65536, 4096)
                    ],
                    final_receipt,
                ]

                with self.assertRaises((
                    acceptance.AcceptanceError,
                    acceptance.flash.FlashError,
                )):
                    acceptance.interrupt_uplink_upload(
                        badge,
                        acceptance.flash.PLATFORMS[
                            CANARY_PLATFORM_KEY
                        ],
                        artifacts,
                        VERSION,
                        running_partition="ota_0",
                        abort_after=65536,
                        update_session="0123456789ABCDEF",
                    )

                self.assertEqual(
                    sum(
                        len(call.args[0])
                        for call in serial.write.call_args_list
                    ),
                    65536,
                )
                self.assertEqual(badge.read_prefixed_json.call_count, 17)

    def test_partial_upload_receipts_share_one_monotonic_deadline_despite_wall_regression(
        self,
    ) -> None:
        image = bytes((index % 251 for index in range(131072)))
        artifacts = _frozen_uplink_artifacts(
            _uplink_firmware_image(len(image))
        )
        serial = mock.Mock()
        serial.write.side_effect = lambda chunk: len(chunk)
        badge = mock.Mock()
        badge.ser = serial
        badge.read_prefixed_json.side_effect = [
            _uplink_receipt(
                total=len(image),
                phase="ready",
                received=0,
                credit_bytes=4096,
            ),
            *[
                _uplink_receipt(
                    total=len(image),
                    phase="credit",
                    received=offset,
                    credit_bytes=4096,
                )
                for offset in range(4096, 65537, 4096)
            ],
        ]
        monotonic_value = 100.0
        wall_value = 10_000.0

        def monotonic() -> float:
            nonlocal monotonic_value
            monotonic_value += 0.25
            return monotonic_value

        def regressing_wall_clock() -> float:
            nonlocal wall_value
            wall_value -= 1000.0
            return wall_value

        with mock.patch.object(
            acceptance.time, "monotonic", side_effect=monotonic
        ) as monotonic_clock, mock.patch.object(
            acceptance.time, "time", side_effect=regressing_wall_clock
        ) as wall_clock:
            result = acceptance.interrupt_uplink_upload(
                badge,
                acceptance.flash.PLATFORMS[CANARY_PLATFORM_KEY],
                artifacts,
                VERSION,
                running_partition="ota_0",
                abort_after=65536,
                update_session="0123456789ABCDEF",
            )

        timeouts = [
            call.args[1]
            for call in badge.read_prefixed_json.call_args_list
        ]
        self.assertEqual(result, 65536)
        self.assertEqual(len(timeouts), 17)
        self.assertTrue(all(timeout > 0 for timeout in timeouts))
        self.assertTrue(all(
            later < earlier
            for earlier, later in zip(timeouts, timeouts[1:])
        ))
        self.assertGreater(monotonic_clock.call_count, len(timeouts))
        wall_clock.assert_not_called()

    def test_partial_upload_final_credit_must_arrive_before_overall_deadline(
        self,
    ) -> None:
        image = bytes((index % 251 for index in range(131072)))
        artifacts = _frozen_uplink_artifacts(
            _uplink_firmware_image(len(image))
        )
        serial = mock.Mock()
        serial.write.side_effect = lambda chunk: len(chunk)
        badge = mock.Mock()
        badge.ser = serial
        badge.read_prefixed_json.side_effect = [
            _uplink_receipt(
                total=len(image),
                phase="ready",
                received=0,
                credit_bytes=4096,
            ),
            *[
                _uplink_receipt(
                    total=len(image),
                    phase="credit",
                    received=offset,
                    credit_bytes=4096,
                )
                for offset in range(4096, 65537, 4096)
            ],
        ]

        def monotonic() -> float:
            written = sum(
                len(call.args[0]) for call in serial.write.call_args_list
            )
            return 121.0 if written == 65536 else 0.0

        with mock.patch.object(
            acceptance.time, "monotonic", side_effect=monotonic
        ), self.assertRaisesRegex(
            acceptance.AcceptanceError, "deadline"
        ):
            acceptance.interrupt_uplink_upload(
                badge,
                acceptance.flash.PLATFORMS[CANARY_PLATFORM_KEY],
                artifacts,
                VERSION,
                running_partition="ota_0",
                abort_after=65536,
                update_session="0123456789ABCDEF",
            )

        self.assertEqual(
            sum(len(call.args[0]) for call in serial.write.call_args_list),
            65536,
        )

    def test_partial_upload_rejects_non_exact_abort_boundary(self) -> None:
        with self.assertRaises(acceptance.AcceptanceError):
            acceptance.interrupt_uplink_upload(
                mock.Mock(),
                acceptance.flash.PLATFORMS[CANARY_PLATFORM_KEY],
                _frozen_uplink_artifacts(),
                VERSION,
                running_partition="ota_0",
                abort_after=65535,
                update_session="0123456789ABCDEF",
            )

    def test_interrupted_gate_rejects_changed_attached_board_before_write(
        self,
    ) -> None:
        changed = _status(wifi_id="02:00:00:00:00:04")

        class FakeBadge:
            def __init__(self, *_args, **_kwargs) -> None:
                pass

            def __enter__(self):
                return self

            def __exit__(self, *_args) -> None:
                return None

            def status(self, timeout_s=5):
                return changed

        with mock.patch.object(
            acceptance.flash, "BadgeSerial", FakeBadge
        ), mock.patch.object(
            acceptance, "interrupt_uplink_upload"
        ) as interrupt, mock.patch.object(
            acceptance,
            "_trusted_session_uplink_descriptor",
            return_value=_usb_record("/dev/fake"),
        ):
            with self.assertRaisesRegex(
                acceptance.AcceptanceError, "board"
            ):
                acceptance.run_interrupted_upload_gate(
                    "/dev/fake",
                    expected_session=_session(),
                    sleep=lambda _seconds: None,
                    expected_version=VERSION,
                    frozen_artifacts=_frozen_candidate_artifacts(),
                    updater_baseline=(
                        LivePromotionMetricsTest._issued_baseline()
                    ),
                )
        interrupt.assert_not_called()


if __name__ == "__main__":
    unittest.main()
