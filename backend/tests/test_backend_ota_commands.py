"""Focused contract tests for the isolated S3 Fullsize OTA channel."""

from __future__ import annotations

import asyncio
import hashlib
import json
import struct
import time
import zlib
from pathlib import Path

import pytest
from sqlalchemy import func, select

from app.routers import detections, nodes
from app.services import firmware_manager
from tests.firmware_images import esp32s3_app_image


SCANNER_NAME = "scanner-s3-combo-fullsize-backend"
UPLINK_NAME = "uplink-s3-fullsize-backend"
SCANNER_PROJECT = "fof_backend_scanner_fullsize"
UPLINK_PROJECT = "fof_backend_uplink_fullsize"
FULLSIZE_HARDWARE = "esp32s3_n16r8_fullsize"
TARGET_VERSION = "0.2.1-backend"


def _backend_image(name: str, version: str = TARGET_VERSION) -> bytes:
    info = firmware_manager.FIRMWARE_TYPES[name]
    record = bytearray(firmware_manager._BACKEND_IDENTITY_STRUCT.size)
    firmware_manager._BACKEND_IDENTITY_STRUCT.pack_into(
        record,
        0,
        0x42464F46,
        1,
        info["image_kind"],
        name.encode().ljust(40, b"\0"),
        info["project"].encode().ljust(40, b"\0"),
        info["hardware"].encode().ljust(40, b"\0"),
        version.encode().ljust(32, b"\0"),
        0,
    )
    struct.pack_into(
        "<I", record, 160, zlib.crc32(record[:160]) & 0xFFFFFFFF,
    )
    return esp32s3_app_image(
        version,
        project=info["project"],
        placements=((0x120, bytes(record)),),
    )


SCANNER_IMAGE = _backend_image(SCANNER_NAME)
UPLINK_IMAGE = _backend_image(UPLINK_NAME)


def _scanner(
    *,
    uart: str,
    slot: int,
    mac: str,
    boot_id: int,
    version: str = "0.2.0-backend",
) -> dict:
    return {
        "uart": uart,
        "slot": slot,
        "product_family": "s3_fullsize",
        "firmware_line": "backend",
        "component": "scanner",
        "firmware_target": SCANNER_NAME,
        "app_project": SCANNER_PROJECT,
        "hardware_type": FULLSIZE_HARDWARE,
        "firmware_version": version,
        "mac": mac,
        "boot_id": boot_id,
        "profile": "ble_primary" if uart == "ble" else "wifi_primary",
        "role_generation": 41 if uart == "ble" else 42,
        "role_acked": True,
        "command_ingress": True,
        "radio_healthy": True,
        "ble_healthy": uart == "ble",
        "wifi_healthy": uart == "wifi",
        "ota_state": "idle",
        "rollback_state": "valid",
    }


def fullsize_heartbeat(*, scanners_reversed: bool = False) -> dict:
    scanners = [
        _scanner(
            uart="ble", slot=0, mac="AA:BB:CC:DD:EE:02", boot_id=202,
        ),
        _scanner(
            uart="wifi", slot=1, mac="AA:BB:CC:DD:EE:03", boot_id=303,
        ),
    ]
    if scanners_reversed:
        scanners.reverse()
    return {
        "device_id": "uplink_FULL",
        "ip": "192.0.2.20",
        "last_seen": time.time(),
        "product_family": "s3_fullsize",
        "firmware_line": "backend",
        "component": "uplink",
        "firmware_target": UPLINK_NAME,
        "app_project": UPLINK_PROJECT,
        "hardware_type": FULLSIZE_HARDWARE,
        "firmware_version": "0.2.0-backend",
        "hardware_mac": "AA:BB:CC:DD:EE:01",
        "boot_id": 101,
        "topology_generation": 7,
        "scanners": scanners,
    }


def install_heartbeat(heartbeat: dict | None = None) -> dict:
    heartbeat = heartbeat or fullsize_heartbeat()
    detections._node_heartbeats[heartbeat["device_id"]] = heartbeat
    return heartbeat


def install_images(monkeypatch: pytest.MonkeyPatch) -> list[str]:
    calls: list[str] = []

    async def exact_bytes(name: str) -> bytes:
        calls.append(name)
        return SCANNER_IMAGE if name == SCANNER_NAME else UPLINK_IMAGE

    async def forbidden(*_args, **_kwargs):
        raise AssertionError("rollout metadata must come from the fetched bytes")

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_binary", exact_bytes)
    monkeypatch.setattr(nodes._firmware_mgr, "get_catalog", forbidden)
    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_version", forbidden)
    return calls


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "body",
    [
        {},
        {"apply_mode": "newer_only"},
        {"components": "scanner0"},
        {"components": "scanner1"},
        {"components": "uplink"},
        {"components": "unknown"},
        {"components": ["scanner0", "scanner1", "uplink"]},
        {"components": "all", "skip": "scanner0"},
        {"components": "all", "apply_mode": "always"},
    ],
)
async def test_rollout_request_rejects_every_non_all_or_extra_shape_without_row(
    client, backend_sensor_session_factory, body,
):
    response = await client.post(
        "/nodes/uplink_FULL/backend-ota/rollouts", json=body,
    )

    assert response.status_code == 422
    from app.models.db_models import BackendOtaRollout

    async with backend_sensor_session_factory() as db:
        assert await db.scalar(select(func.count()).select_from(BackendOtaRollout)) == 0


@pytest.mark.asyncio
async def test_rollout_request_rejects_duplicate_json_keys_before_validation(client):
    response = await client.post(
        "/nodes/uplink_FULL/backend-ota/rollouts",
        content=b'{"components":"all","components":"all"}',
        headers={"content-type": "application/json"},
    )

    assert response.status_code == 422


@pytest.mark.asyncio
async def test_create_preflights_then_fetches_each_exact_image_once_and_binds_slots(
    client, monkeypatch,
):
    heartbeat = install_heartbeat(fullsize_heartbeat(scanners_reversed=True))
    calls = install_images(monkeypatch)

    def forbidden_legacy(*_args, **_kwargs):
        raise AssertionError("dedicated rollout called a legacy OTA helper")

    for helper in (
        "_require_ota_compatibility",
        "_require_legacy_trigger_preflight",
        "_require_same_ota_identity",
        "_run_direct_legacy_scanner_relay",
    ):
        monkeypatch.setattr(nodes, helper, forbidden_legacy)

    response = await client.post(
        "/nodes/uplink_FULL/backend-ota/rollouts",
        json={"components": "all"},
    )

    assert response.status_code == 201, response.text
    operation_id = response.json()["operation_id"]
    assert len(operation_id) == 32
    assert set(operation_id) <= set("0123456789abcdef")
    assert calls == [SCANNER_NAME, UPLINK_NAME]
    expected = {
        "schema": 1,
        "operation_id": operation_id,
        "type": "backend_ota_probe",
        "component": "scanner0",
        "catalog_name": SCANNER_NAME,
        "expected_sha256": hashlib.sha256(SCANNER_IMAGE).hexdigest(),
        "expected_size": len(SCANNER_IMAGE),
        "expected_uplink_mac": "AA:BB:CC:DD:EE:01",
        "expected_uplink_boot_id": 101,
        "expected_target_mac": "AA:BB:CC:DD:EE:02",
        "expected_target_boot_id": 202,
        "expected_topology_generation": 7,
        "next_sequence": 0,
    }
    assert response.json() == expected
    assert heartbeat["scanners"][0]["uart"] == "wifi"


@pytest.mark.asyncio
@pytest.mark.parametrize(
    ("mutation", "detail"),
    [
        (lambda hb: hb.pop("boot_id"), "boot"),
        (lambda hb: hb.__setitem__("boot_id", 0), "boot"),
        (lambda hb: hb.pop("topology_generation"), "topology"),
        (lambda hb: hb.__setitem__("topology_generation", 0), "topology"),
        (
            lambda hb: (
                hb.pop("topology_generation"),
                hb.__setitem__("role_generation", 7),
            ),
            "topology",
        ),
        (lambda hb: hb["scanners"][0].__setitem__("boot_id", 0), "boot"),
        (lambda hb: hb["scanners"][1].pop("boot_id"), "boot"),
    ],
)
async def test_preflight_rejects_missing_or_zero_uplink_telemetry_before_fetch(
    client, monkeypatch, mutation, detail,
):
    heartbeat = fullsize_heartbeat()
    mutation(heartbeat)
    install_heartbeat(heartbeat)

    async def forbidden(_name: str) -> bytes:
        raise AssertionError("metadata fetch occurred before preflight")

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_binary", forbidden)
    response = await client.post(
        "/nodes/uplink_FULL/backend-ota/rollouts", json={"components": "all"},
    )

    assert response.status_code == 409
    assert detail in response.json()["detail"].lower()


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "mutation",
    [
        lambda hb: hb["scanners"][0].__setitem__("slot", 1),
        lambda hb: hb["scanners"][1].__setitem__("uart", "ble"),
        lambda hb: hb["scanners"][0].__setitem__("mac", hb["hardware_mac"]),
        lambda hb: hb["scanners"][0].__setitem__("role_acked", False),
        lambda hb: hb["scanners"][1].__setitem__("radio_healthy", False),
        lambda hb: hb["scanners"][1].__setitem__("rollback_state", "pending"),
        lambda hb: hb.__setitem__("firmware_target", "uplink-s3-backend"),
        lambda hb: hb["scanners"][0].__setitem__(
            "firmware_target", "scanner-s3-combo-backend",
        ),
    ],
)
async def test_preflight_rejects_nonexact_or_unhealthy_trios_before_fetch(
    client, monkeypatch, mutation,
):
    heartbeat = fullsize_heartbeat()
    mutation(heartbeat)
    install_heartbeat(heartbeat)

    async def forbidden(_name: str) -> bytes:
        raise AssertionError("metadata fetch occurred before preflight")

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_binary", forbidden)
    response = await client.post(
        "/nodes/uplink_FULL/backend-ota/rollouts", json={"components": "all"},
    )

    assert response.status_code == 409


@pytest.mark.asyncio
async def test_binding_change_immediately_after_first_await_stops_second_fetch(
    client, monkeypatch,
):
    heartbeat = install_heartbeat()
    calls: list[str] = []

    async def mutate_after_fetch(name: str) -> bytes:
        calls.append(name)
        heartbeat["scanners"][0]["boot_id"] += 1
        return SCANNER_IMAGE

    monkeypatch.setattr(
        nodes._firmware_mgr, "get_firmware_binary", mutate_after_fetch,
    )
    response = await client.post(
        "/nodes/uplink_FULL/backend-ota/rollouts", json={"components": "all"},
    )

    assert response.status_code == 409
    assert "changed" in response.json()["detail"].lower()
    assert calls == [SCANNER_NAME]


@pytest.mark.asyncio
async def test_invalid_scanner_bytes_stop_before_uplink_fetch(client, monkeypatch):
    install_heartbeat()
    calls: list[str] = []

    async def wrong_scanner_image(name: str) -> bytes:
        calls.append(name)
        return UPLINK_IMAGE

    monkeypatch.setattr(
        nodes._firmware_mgr, "get_firmware_binary", wrong_scanner_image,
    )
    response = await client.post(
        "/nodes/uplink_FULL/backend-ota/rollouts", json={"components": "all"},
    )

    assert response.status_code == 409
    assert "identity" in response.json()["detail"]
    assert calls == [SCANNER_NAME]


@pytest.mark.asyncio
async def test_post_fetch_preflight_uses_fresh_clock_and_rejects_new_staleness(
    db_session,
):
    from app.models.schemas import BackendOtaRolloutRequest
    from app.services.backend_ota_commands import (
        BackendOtaConflict,
        BackendOtaService,
    )

    heartbeat = fullsize_heartbeat()
    heartbeat["last_seen"] = 1_000.0
    snapshot = {
        "device_id": "uplink_FULL",
        "ip": heartbeat["ip"],
        "heartbeat": heartbeat,
        "scanners": list(heartbeat["scanners"]),
    }
    clock_values = iter((1_000.0, 1_121.0))
    service = BackendOtaService(clock=lambda: next(clock_values))

    class ExactManager:
        calls: list[str] = []

        async def get_firmware_binary(self, name: str) -> bytes:
            self.calls.append(name)
            return SCANNER_IMAGE if name == SCANNER_NAME else UPLINK_IMAGE

    manager = ExactManager()
    with pytest.raises(BackendOtaConflict, match="stale"):
        await service.create_rollout(
            db_session,
            "uplink_FULL",
            BackendOtaRolloutRequest(components="all"),
            now=1_000.0,
            firmware_manager=manager,
            snapshot_provider=lambda *_args: snapshot,
        )

    assert manager.calls == [SCANNER_NAME]


@pytest.mark.asyncio
async def test_final_synchronous_preflight_catches_last_binding_change(db_session):
    from app.models.schemas import BackendOtaRolloutRequest
    from app.services.backend_ota_commands import BackendOtaConflict, BackendOtaService

    heartbeat = fullsize_heartbeat()
    snapshot_calls = 0

    def changing_snapshot(*_args) -> dict:
        nonlocal snapshot_calls
        snapshot_calls += 1
        snapshot_heartbeat = json.loads(json.dumps(heartbeat))
        if snapshot_calls == 4:
            snapshot_heartbeat["scanners"][1]["boot_id"] += 1
        return {
            "device_id": "uplink_FULL",
            "ip": snapshot_heartbeat["ip"],
            "heartbeat": snapshot_heartbeat,
            "scanners": snapshot_heartbeat["scanners"],
        }

    class ExactManager:
        async def get_firmware_binary(self, name: str) -> bytes:
            return SCANNER_IMAGE if name == SCANNER_NAME else UPLINK_IMAGE

    with pytest.raises(BackendOtaConflict, match="changed"):
        await BackendOtaService().create_rollout(
            db_session,
            "uplink_FULL",
            BackendOtaRolloutRequest(components="all"),
            now=time.time(),
            firmware_manager=ExactManager(),
            snapshot_provider=changing_snapshot,
        )
    assert snapshot_calls == 4


@pytest.mark.asyncio
async def test_simultaneous_creation_leaves_exactly_one_active_rollout(
    client, backend_sensor_session_factory, monkeypatch,
):
    install_heartbeat()
    install_images(monkeypatch)

    first, second = await asyncio.gather(*(
        client.post(
            "/nodes/uplink_FULL/backend-ota/rollouts",
            json={"components": "all"},
        )
        for _ in range(2)
    ))

    assert sorted((first.status_code, second.status_code)) == [201, 409]
    from app.models.db_models import BackendOtaRollout

    async with backend_sensor_session_factory() as db:
        assert await db.scalar(
            select(func.count()).select_from(BackendOtaRollout).where(
                BackendOtaRollout.active_key == "uplink_FULL",
            )
        ) == 1


def _raw(value: dict) -> bytes:
    return json.dumps(value, separators=(",", ":")).encode("utf-8")


def _manual_receipt(command: dict, end: dict) -> tuple[str, str]:
    values = {
        **command,
        **end,
        "command_type": command["type"],
        "role_healthy": int(end["role_healthy"]),
        "radio_healthy": int(end["radio_healthy"]),
        "rollback_clear": int(end["rollback_clear"]),
    }
    lines = [
        "fof-backend-ota-end-receipt-v1",
        f"operation_id={values['operation_id']}",
        f"command_type={values['command_type']}",
        f"component={values['component']}",
        f"catalog_name={values['catalog_name']}",
        f"expected_sha256={values['expected_sha256']}",
        f"expected_size={values['expected_size']}",
        f"expected_uplink_mac={values['expected_uplink_mac']}",
        f"expected_uplink_boot_id={values['expected_uplink_boot_id']}",
        f"expected_target_mac={values['expected_target_mac']}",
        f"expected_target_boot_id={values['expected_target_boot_id']}",
        f"expected_topology_generation={values['expected_topology_generation']}",
        f"state={values['state']}",
        f"decision={values['decision']}",
        f"error={values['error']}",
        f"image_writes={values['image_writes']}",
        f"target={values['target']}",
        f"project={values['project']}",
        f"hardware={values['hardware']}",
        f"version={values['version']}",
        f"actual_mac={values['actual_mac']}",
        f"actual_boot_id={values['actual_boot_id']}",
        f"actual_topology_generation={values['actual_topology_generation']}",
        f"role_healthy={values['role_healthy']}",
        f"radio_healthy={values['radio_healthy']}",
        f"rollback_clear={values['rollback_clear']}",
    ]
    preimage = "\n".join(lines) + "\n"
    return preimage, hashlib.sha256(preimage.encode("utf-8")).hexdigest()


def _begin(command: dict) -> dict:
    return {
        "schema": 1,
        "operation_id": command["operation_id"],
        "sequence": command["next_sequence"],
        "type": "backend_ota_begin",
        "component": command["component"],
        "catalog_name": command["catalog_name"],
    }


def _end(
    command: dict,
    *,
    sequence: int,
    state: str,
    decision: str,
    error: str,
    image_writes: int,
    target: str,
    project: str,
    hardware: str,
    version: str,
    actual_mac: str | None = None,
    actual_boot_id: int | None = None,
    role_healthy: bool = True,
    radio_healthy: bool = True,
    rollback_clear: bool = True,
) -> dict:
    event = {
        "schema": 1,
        "operation_id": command["operation_id"],
        "sequence": sequence,
        "type": "backend_ota_end",
        "component": command["component"],
        "catalog_name": command["catalog_name"],
        "state": state,
        "decision": decision,
        "error": error,
        "image_writes": image_writes,
        "target": target,
        "project": project,
        "hardware": hardware,
        "version": version,
        "actual_mac": actual_mac or command["expected_target_mac"],
        "actual_boot_id": (
            command["expected_target_boot_id"]
            if actual_boot_id is None else actual_boot_id
        ),
        "actual_topology_generation": command["expected_topology_generation"],
        "role_healthy": role_healthy,
        "radio_healthy": radio_healthy,
        "rollback_clear": rollback_clear,
    }
    _, event["receipt_sha256"] = _manual_receipt(command, event)
    return event


async def _create_rollout(client, monkeypatch, *, heartbeat: dict | None = None) -> tuple[dict, dict]:
    heartbeat = install_heartbeat(heartbeat)
    install_images(monkeypatch)
    response = await client.post(
        "/nodes/uplink_FULL/backend-ota/rollouts", json={"components": "all"},
    )
    assert response.status_code == 201, response.text
    return heartbeat, response.json()


async def _post_event(client, command: dict, event: dict, *, raw: bytes | None = None):
    return await client.post(
        f"/nodes/uplink_FULL/backend-ota/{command['operation_id']}/events",
        content=raw if raw is not None else _raw(event),
        headers={"content-type": "application/json"},
    )


@pytest.mark.asyncio
async def test_duplicate_poll_and_exact_raw_event_replay_survive_phase_advance(
    client, monkeypatch,
):
    _heartbeat, command = await _create_rollout(client, monkeypatch)
    first = await client.get("/nodes/uplink_FULL/backend-ota/next")
    second = await client.get("/nodes/uplink_FULL/backend-ota/next")
    assert first.status_code == 200
    assert first.content == second.content
    assert first.json() == command

    begin = _begin(command)
    begin_raw = _raw(begin)
    accepted = await _post_event(client, command, begin, raw=begin_raw)
    duplicate = await _post_event(client, command, begin, raw=begin_raw)
    assert accepted.status_code == 200, accepted.text
    assert accepted.json()["duplicate"] is False
    assert duplicate.status_code == 200
    assert duplicate.json()["duplicate"] is True

    eligible = _end(
        command,
        sequence=1,
        state="complete",
        decision="eligible",
        error="none",
        image_writes=0,
        target=SCANNER_NAME,
        project=SCANNER_PROJECT,
        hardware=FULLSIZE_HARDWARE,
        version=TARGET_VERSION,
    )
    ended = await _post_event(client, command, eligible)
    assert ended.status_code == 200, ended.text

    after_advance = await _post_event(client, command, begin, raw=begin_raw)
    assert after_advance.status_code == 200
    assert after_advance.json()["duplicate"] is True

    reordered = json.dumps(begin, sort_keys=True, separators=(",", ":")).encode()
    conflict = await _post_event(client, command, begin, raw=reordered)
    assert conflict.status_code == 409
    assert "body differs" in conflict.json()["detail"]

    apply_command = (await client.get(
        "/nodes/uplink_FULL/backend-ota/next",
    )).json()
    assert apply_command == {
        **{key: value for key, value in command.items() if key != "type"},
        "type": "backend_ota_apply",
        "apply_mode": "newer_only",
        "probe_receipt_sha256": eligible["receipt_sha256"],
        "next_sequence": 2,
    }


@pytest.mark.asyncio
async def test_simultaneous_identical_event_is_persisted_once_and_replayed(
    client, backend_sensor_session_factory, monkeypatch,
):
    _heartbeat, command = await _create_rollout(client, monkeypatch)
    begin = _begin(command)
    begin_raw = _raw(begin)

    first, second = await asyncio.gather(*(
        _post_event(client, command, begin, raw=begin_raw)
        for _ in range(2)
    ))

    assert first.status_code == second.status_code == 200
    assert sorted((first.json()["duplicate"], second.json()["duplicate"])) == [
        False,
        True,
    ]
    from app.models.db_models import BackendOtaEvent, BackendOtaRollout

    async with backend_sensor_session_factory() as db:
        row = await db.get(BackendOtaRollout, command["operation_id"])
        assert row.next_sequence == 1
        assert await db.scalar(
            select(func.count()).select_from(BackendOtaEvent),
        ) == 1


@pytest.mark.asyncio
async def test_event_duplicate_keys_and_noncanonical_extra_fields_are_422(
    client, monkeypatch,
):
    _heartbeat, command = await _create_rollout(client, monkeypatch)
    duplicate_key = (
        '{"schema":1,"schema":1,"operation_id":"'
        + command["operation_id"]
        + '","sequence":0,"type":"backend_ota_begin","component":"scanner0",'
          '"catalog_name":"scanner-s3-combo-fullsize-backend"}'
    ).encode()
    duplicate = await _post_event(client, command, {}, raw=duplicate_key)
    assert duplicate.status_code == 422

    extra = _begin(command)
    extra["request_id"] = command["operation_id"]
    response = await _post_event(client, command, extra)
    assert response.status_code == 422
    assert (await client.get("/nodes/uplink_FULL/backend-ota/next")).json()[
        "next_sequence"
    ] == 0


@pytest.mark.asyncio
async def test_begin_first_and_monotonic_progress_counters_are_enforced(
    client, monkeypatch,
):
    _heartbeat, command = await _create_rollout(client, monkeypatch)
    progress = {
        "schema": 1,
        "operation_id": command["operation_id"],
        "sequence": 0,
        "type": "backend_ota_progress",
        "component": "scanner0",
        "catalog_name": SCANNER_NAME,
        "stage": "metadata",
        "received": 10,
        "total": 100,
        "retry_count": 0,
    }
    assert (await _post_event(client, command, progress)).status_code == 409
    assert (await _post_event(client, command, _begin(command))).status_code == 200

    progress["sequence"] = 1
    assert (await _post_event(client, command, progress)).status_code == 200
    for mutation in (
        {"sequence": 2, "received": 9},
        {"sequence": 2, "total": 101},
        {"sequence": 2, "stage": "download"},
    ):
        invalid = {**progress, **mutation}
        assert (await _post_event(client, command, invalid)).status_code == 409

    retry = {**progress, "sequence": 2, "received": 20, "retry_count": 1}
    assert (await _post_event(client, command, retry)).status_code == 200
    regression = {**retry, "sequence": 3, "received": 21, "retry_count": 0}
    assert (await _post_event(client, command, regression)).status_code == 409


@pytest.mark.asyncio
async def test_progress_counters_reset_on_stage_advance_but_not_within_stage(
    client, monkeypatch,
):
    _heartbeat, probe = await _create_rollout(client, monkeypatch)
    command, _ = await _finish_eligible_probe(client, probe)
    assert (await _post_event(client, command, _begin(command))).status_code == 200
    metadata = {
        "schema": 1,
        "operation_id": command["operation_id"],
        "sequence": 3,
        "type": "backend_ota_progress",
        "component": "scanner0",
        "catalog_name": SCANNER_NAME,
        "stage": "metadata",
        "received": 100,
        "total": 100,
        "retry_count": 2,
    }
    assert (await _post_event(client, command, metadata)).status_code == 200
    download = {
        **metadata,
        "sequence": 4,
        "stage": "download",
        "received": 0,
        "total": 200,
        "retry_count": 0,
    }
    assert (await _post_event(client, command, download)).status_code == 200
    within_stage_regression = {**download, "sequence": 5, "total": 201}
    assert (
        await _post_event(client, command, within_stage_regression)
    ).status_code == 409


@pytest.mark.asyncio
async def test_receipt_fixture_bytes_hex_digest_and_builder_are_identical():
    fixture_path = (
        Path(__file__).parents[2]
        / "backend-firmware/test/fixtures/backend_ota_receipt_v1.json"
    )
    fixture = json.loads(fixture_path.read_text(encoding="utf-8"))
    from app.services.backend_ota_commands import build_receipt_preimage

    assert fixture["schema"] == 1
    for name in ("probe", "apply"):
        vector = fixture[name]
        encoded = vector["preimage_utf8"].encode("utf-8")
        assert encoded.hex() == vector["preimage_hex"]
        assert encoded.endswith(b"\n") and not encoded.endswith(b"\n\n")
        assert hashlib.sha256(encoded).hexdigest() == vector["receipt_sha256"]
        assert build_receipt_preimage(vector["command"], vector["end"]) == encoded
    assert (
        fixture["apply"]["command"]["probe_receipt_sha256"]
        == fixture["probe"]["receipt_sha256"]
    )


async def _finish_eligible_probe(client, command: dict) -> tuple[dict, dict]:
    begin = await _post_event(client, command, _begin(command))
    assert begin.status_code == 200, begin.text
    image_project = SCANNER_PROJECT if command["component"] != "uplink" else UPLINK_PROJECT
    eligible = _end(
        command,
        sequence=command["next_sequence"] + 1,
        state="complete",
        decision="eligible",
        error="none",
        image_writes=0,
        target=command["catalog_name"],
        project=image_project,
        hardware=FULLSIZE_HARDWARE,
        version=TARGET_VERSION,
    )
    ended = await _post_event(client, command, eligible)
    assert ended.status_code == 200, ended.text
    apply_command = (await client.get(
        "/nodes/uplink_FULL/backend-ota/next",
    )).json()
    assert apply_command["type"] == "backend_ota_apply"
    assert apply_command["operation_id"] == command["operation_id"]
    assert apply_command["probe_receipt_sha256"] == eligible["receipt_sha256"]
    return apply_command, eligible


def _converge_heartbeat(heartbeat: dict, component: str, new_boot_id: int) -> None:
    if component == "uplink":
        heartbeat["boot_id"] = new_boot_id
        heartbeat["firmware_version"] = TARGET_VERSION
        return
    uart = "ble" if component == "scanner0" else "wifi"
    scanner = next(item for item in heartbeat["scanners"] if item["uart"] == uart)
    scanner["boot_id"] = new_boot_id
    scanner["firmware_version"] = TARGET_VERSION
    scanner["role_generation"] += 100


async def _finish_apply(
    client,
    heartbeat: dict,
    command: dict,
    *,
    new_boot_id: int,
) -> dict | None:
    began = await _post_event(client, command, _begin(command))
    assert began.status_code == 200, began.text
    _converge_heartbeat(heartbeat, command["component"], new_boot_id)
    project = SCANNER_PROJECT if command["component"] != "uplink" else UPLINK_PROJECT
    applied = _end(
        command,
        sequence=command["next_sequence"] + 1,
        state="complete",
        decision="applied",
        error="none",
        image_writes=command["expected_size"],
        target=command["catalog_name"],
        project=project,
        hardware=FULLSIZE_HARDWARE,
        version=TARGET_VERSION,
        actual_boot_id=new_boot_id,
    )
    ended = await _post_event(client, command, applied)
    assert ended.status_code == 200, ended.text
    polled = await client.get("/nodes/uplink_FULL/backend-ota/next")
    if polled.status_code == 204:
        return None
    assert polled.status_code == 200, polled.text
    return polled.json()


@pytest.mark.asyncio
async def test_exact_serial_order_and_heartbeat_convergence_gate_full_rollout(
    client, monkeypatch,
):
    heartbeat, command = await _create_rollout(client, monkeypatch)
    observed = [(command["component"], command["type"])]

    apply0, _ = await _finish_eligible_probe(client, command)
    observed.append((apply0["component"], apply0["type"]))
    command = await _finish_apply(
        client, heartbeat, apply0, new_boot_id=1_202,
    )
    assert command is not None
    observed.append((command["component"], command["type"]))

    apply1, _ = await _finish_eligible_probe(client, command)
    observed.append((apply1["component"], apply1["type"]))
    command = await _finish_apply(
        client, heartbeat, apply1, new_boot_id=1_303,
    )
    assert command is not None
    observed.append((command["component"], command["type"]))

    apply_uplink, _ = await _finish_eligible_probe(client, command)
    observed.append((apply_uplink["component"], apply_uplink["type"]))
    command = await _finish_apply(
        client, heartbeat, apply_uplink, new_boot_id=1_101,
    )
    assert command is None
    assert observed == [
        ("scanner0", "backend_ota_probe"),
        ("scanner0", "backend_ota_apply"),
        ("scanner1", "backend_ota_probe"),
        ("scanner1", "backend_ota_apply"),
        ("uplink", "backend_ota_probe"),
        ("uplink", "backend_ota_apply"),
    ]


@pytest.mark.asyncio
async def test_apply_end_is_retryable_until_new_boot_heartbeat_converges(
    client, monkeypatch,
):
    heartbeat, probe = await _create_rollout(client, monkeypatch)
    apply_command, _ = await _finish_eligible_probe(client, probe)
    assert (await _post_event(
        client, apply_command, _begin(apply_command),
    )).status_code == 200
    applied = _end(
        apply_command,
        sequence=3,
        state="complete",
        decision="applied",
        error="none",
        image_writes=apply_command["expected_size"],
        target=SCANNER_NAME,
        project=SCANNER_PROJECT,
        hardware=FULLSIZE_HARDWARE,
        version=TARGET_VERSION,
        actual_boot_id=1_202,
    )
    waiting = await _post_event(client, apply_command, applied)
    assert waiting.status_code == 409
    assert "heartbeat" in waiting.json()["detail"]

    _converge_heartbeat(heartbeat, "scanner0", 1_202)
    accepted = await _post_event(client, apply_command, applied)
    assert accepted.status_code == 200, accepted.text
    assert (await client.get(
        "/nodes/uplink_FULL/backend-ota/next",
    )).json()["component"] == "scanner1"


@pytest.mark.asyncio
@pytest.mark.parametrize(
    ("state", "decision", "error"),
    [
        ("complete", "no_update", "none"),
        ("no_update", "eligible", "none"),
        ("failed", "rolled_back", "download"),
        ("rolled_back", "rejected", "rollback"),
    ],
)
async def test_every_nonprotocol_terminal_state_decision_pair_is_rejected(
    client, monkeypatch, state, decision, error,
):
    _heartbeat, command = await _create_rollout(client, monkeypatch)
    assert (await _post_event(client, command, _begin(command))).status_code == 200
    terminal = _end(
        command,
        sequence=1,
        state=state,
        decision=decision,
        error=error,
        image_writes=0,
        target=SCANNER_NAME,
        project=SCANNER_PROJECT,
        hardware=FULLSIZE_HARDWARE,
        version=TARGET_VERSION,
    )

    response = await _post_event(client, command, terminal)

    assert response.status_code == 409
    still_probe = (await client.get(
        "/nodes/uplink_FULL/backend-ota/next",
    )).json()
    assert still_probe["type"] == "backend_ota_probe"
    assert still_probe["next_sequence"] == 1


@pytest.mark.asyncio
async def test_successful_probe_rechecks_complete_live_binding_before_apply(
    client, monkeypatch,
):
    heartbeat, command = await _create_rollout(client, monkeypatch)
    assert (await _post_event(client, command, _begin(command))).status_code == 200
    heartbeat["scanners"][1]["boot_id"] += 1
    eligible = _end(
        command,
        sequence=1,
        state="complete",
        decision="eligible",
        error="none",
        image_writes=0,
        target=SCANNER_NAME,
        project=SCANNER_PROJECT,
        hardware=FULLSIZE_HARDWARE,
        version=TARGET_VERSION,
    )
    response = await _post_event(client, command, eligible)
    assert response.status_code == 409
    assert "heartbeat" in response.json()["detail"]
    still_probe = await client.get("/nodes/uplink_FULL/backend-ota/next")
    assert still_probe.json()["type"] == "backend_ota_probe"
    assert still_probe.json()["next_sequence"] == 1


@pytest.mark.asyncio
async def test_no_update_advances_without_apply_and_completed_node_accepts_replacement(
    client, monkeypatch,
):
    heartbeat = fullsize_heartbeat()
    heartbeat["firmware_version"] = TARGET_VERSION
    for scanner in heartbeat["scanners"]:
        scanner["firmware_version"] = TARGET_VERSION
    heartbeat, command = await _create_rollout(
        client, monkeypatch, heartbeat=heartbeat,
    )
    operation_id = command["operation_id"]

    for expected_component in ("scanner0", "scanner1", "uplink"):
        assert command["component"] == expected_component
        assert (await _post_event(client, command, _begin(command))).status_code == 200
        project = SCANNER_PROJECT if expected_component != "uplink" else UPLINK_PROJECT
        no_update = _end(
            command,
            sequence=command["next_sequence"] + 1,
            state="no_update",
            decision="no_update",
            error="none",
            image_writes=0,
            target=command["catalog_name"],
            project=project,
            hardware=FULLSIZE_HARDWARE,
            version=TARGET_VERSION,
        )
        response = await _post_event(client, command, no_update)
        assert response.status_code == 200, response.text
        poll = await client.get("/nodes/uplink_FULL/backend-ota/next")
        command = poll.json() if poll.status_code == 200 else None

    assert command is None
    history = await client.get(
        f"/nodes/uplink_FULL/backend-ota/{operation_id}",
    )
    assert history.status_code == 200, history.text
    assert history.json()["terminal"] is True
    assert history.json()["state"] == "complete"
    assert len(history.json()["events"]) == 6

    replacement = await client.post(
        "/nodes/uplink_FULL/backend-ota/rollouts", json={"components": "all"},
    )
    assert replacement.status_code == 201, replacement.text
    assert replacement.json()["operation_id"] != operation_id


async def _finish_no_update(client, command: dict) -> dict | None:
    assert (await _post_event(client, command, _begin(command))).status_code == 200
    project = SCANNER_PROJECT if command["component"] != "uplink" else UPLINK_PROJECT
    no_update = _end(
        command,
        sequence=command["next_sequence"] + 1,
        state="no_update",
        decision="no_update",
        error="none",
        image_writes=0,
        target=command["catalog_name"],
        project=project,
        hardware=FULLSIZE_HARDWARE,
        version=TARGET_VERSION,
    )
    response = await _post_event(client, command, no_update)
    assert response.status_code == 200, response.text
    poll = await client.get("/nodes/uplink_FULL/backend-ota/next")
    return poll.json() if poll.status_code == 200 else None


@pytest.mark.asyncio
async def test_no_update_scanner_boots_are_persisted_for_later_uplink_apply(
    client, monkeypatch,
):
    heartbeat = fullsize_heartbeat()
    heartbeat["firmware_version"] = "0.2.0-backend"
    for scanner in heartbeat["scanners"]:
        scanner["firmware_version"] = TARGET_VERSION
    heartbeat, command = await _create_rollout(
        client, monkeypatch, heartbeat=heartbeat,
    )
    command = await _finish_no_update(client, command)
    assert command is not None and command["component"] == "scanner1"
    command = await _finish_no_update(client, command)
    assert command is not None and command["component"] == "uplink"
    apply_uplink, _ = await _finish_eligible_probe(client, command)
    assert await _finish_apply(
        client, heartbeat, apply_uplink, new_boot_id=1_101,
    ) is None


@pytest.mark.asyncio
@pytest.mark.parametrize(
    ("state", "decision", "error"),
    [
        ("failed", "rejected", "download"),
        ("rolled_back", "rolled_back", "rollback"),
    ],
)
async def test_failure_or_rollback_terminates_the_all_component_rollout(
    client, monkeypatch, state, decision, error,
):
    _heartbeat, command = await _create_rollout(client, monkeypatch)
    assert (await _post_event(client, command, _begin(command))).status_code == 200
    terminal = _end(
        command,
        sequence=1,
        state=state,
        decision=decision,
        error=error,
        image_writes=0,
        target="" if state == "failed" else SCANNER_NAME,
        project="" if state == "failed" else SCANNER_PROJECT,
        hardware="" if state == "failed" else FULLSIZE_HARDWARE,
        version="" if state == "failed" else TARGET_VERSION,
        role_healthy=False,
        radio_healthy=False,
        rollback_clear=False,
    )
    response = await _post_event(client, command, terminal)
    assert response.status_code == 200, response.text
    assert response.json()["terminal"] is True
    assert (await client.get("/nodes/uplink_FULL/backend-ota/next")).status_code == 204


@pytest.mark.asyncio
async def test_service_restart_resumes_same_operation_and_command(client, monkeypatch):
    _heartbeat, command = await _create_rollout(client, monkeypatch)
    from app.services.backend_ota_commands import BackendOtaService

    monkeypatch.setattr(nodes, "_backend_ota_service", BackendOtaService())
    resumed = await client.get("/nodes/uplink_FULL/backend-ota/next")
    assert resumed.status_code == 200
    assert resumed.json() == command


@pytest.mark.asyncio
@pytest.mark.parametrize(
    ("operation", "method", "suffix", "body"),
    [
        ("create_rollout", "POST", "rollouts", {"components": "all"}),
        ("next_for_device", "GET", "next", None),
        (
            "record_event",
            "POST",
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/events",
            {
                "schema": 1,
                "operation_id": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                "sequence": 0,
                "type": "backend_ota_begin",
                "component": "scanner0",
                "catalog_name": SCANNER_NAME,
            },
        ),
        (
            "history_for_device",
            "GET",
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            None,
        ),
    ],
)
async def test_store_outage_is_retryable_for_every_backend_ota_handler(
    client, monkeypatch, operation, method, suffix, body,
):
    from app.services.backend_ota_commands import BackendOtaUnavailable

    async def unavailable(*_args, **_kwargs):
        raise BackendOtaUnavailable("backend OTA store unavailable")

    monkeypatch.setattr(nodes._backend_ota_service, operation, unavailable)
    response = await client.request(
        method,
        f"/nodes/uplink_FULL/backend-ota/{suffix}",
        json=body,
    )
    assert response.status_code == 503
    assert response.headers["retry-after"] == "1"


@pytest.mark.asyncio
async def test_sequence_uint32_exhaustion_is_rejected_before_event_persistence(
    client, backend_sensor_session_factory, monkeypatch,
):
    _heartbeat, command = await _create_rollout(client, monkeypatch)
    from app.models.db_models import BackendOtaEvent, BackendOtaRollout

    async with backend_sensor_session_factory() as db:
        row = await db.get(BackendOtaRollout, command["operation_id"])
        row.next_sequence = 0xFFFFFFFF
        await db.commit()

    command = (await client.get("/nodes/uplink_FULL/backend-ota/next")).json()
    assert command["next_sequence"] == 0xFFFFFFFF
    response = await _post_event(client, command, _begin(command))
    assert response.status_code == 409
    assert "sequence" in response.json()["detail"]

    async with backend_sensor_session_factory() as db:
        row = await db.get(BackendOtaRollout, command["operation_id"])
        assert row.next_sequence == 0xFFFFFFFF
        assert await db.scalar(
            select(func.count()).select_from(BackendOtaEvent),
        ) == 0
