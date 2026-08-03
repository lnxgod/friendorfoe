"""Durable and isolated S3 Fullsize backend-OTA rollout lifecycle."""

from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
import secrets
import time
from typing import Callable

from pydantic import TypeAdapter, ValidationError
from sqlalchemy import select, text
from sqlalchemy.exc import IntegrityError, OperationalError
from sqlalchemy.ext.asyncio import AsyncSession

from app.models.db_models import BackendOtaEvent, BackendOtaRollout
from app.models.schemas import (
    BackendOtaApplyEnvelope,
    BackendOtaBeginEvent,
    BackendOtaCommandEnvelope,
    BackendOtaEndEvent,
    BackendOtaEventAck,
    BackendOtaEventRequest,
    BackendOtaHistoryResponse,
    BackendOtaProbeEnvelope,
    BackendOtaProgressEvent,
    BackendOtaRolloutRequest,
)
from app.services.firmware_management import (
    remote_update_blockers,
    resolve_component_management_identity,
)
from app.services.firmware_manager import (
    FirmwareManager,
    _validated_backend_image_info,
)


SCANNER_CATALOG = "scanner-s3-combo-fullsize-backend"
UPLINK_CATALOG = "uplink-s3-fullsize-backend"
FULLSIZE_HARDWARE = "esp32s3_n16r8_fullsize"
SCANNER_PROJECT = "fof_backend_scanner_fullsize"
UPLINK_PROJECT = "fof_backend_uplink_fullsize"
UINT32_MAX = 0xFFFFFFFF
_EVENT_ADAPTER = TypeAdapter(BackendOtaEventRequest)
_STAGE_RANK = {
    "metadata": 0,
    "download": 1,
    "validate": 2,
    "stage": 3,
    "uart_relay": 4,
    "reboot_wait": 5,
    "convergence": 6,
}
_PROBE_STAGES = {"metadata", "validate", "convergence"}


class BackendOtaError(Exception):
    """Base error for the separate rollout channel."""


class BackendOtaConflict(BackendOtaError):
    """The request conflicts with current persisted or heartbeat state."""


class BackendOtaUnavailable(BackendOtaError):
    """The rollout store could not serialize an operation."""

    retryable = True


class BackendOtaRequestInvalid(BackendOtaError):
    """Raw JSON failed duplicate-key or strict request validation."""


class BackendOtaNotFound(BackendOtaError):
    """The operation does not exist for the requested device."""


def _object_without_duplicates(pairs: list[tuple[str, object]]) -> dict:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def parse_rollout_request(raw: bytes) -> BackendOtaRolloutRequest:
    try:
        text = raw.decode("utf-8")
        value = json.loads(text, object_pairs_hook=_object_without_duplicates)
        if not isinstance(value, dict):
            raise ValueError("request body must be an object")
        return BackendOtaRolloutRequest.model_validate(value)
    except (UnicodeDecodeError, json.JSONDecodeError, ValueError, ValidationError) as exc:
        raise BackendOtaRequestInvalid(str(exc)) from exc


def parse_event_request(raw: bytes) -> tuple[BackendOtaEventRequest, str, str]:
    try:
        raw_text = raw.decode("utf-8")
        value = json.loads(raw_text, object_pairs_hook=_object_without_duplicates)
        if not isinstance(value, dict):
            raise ValueError("event body must be an object")
        event = _EVENT_ADAPTER.validate_python(value)
    except (UnicodeDecodeError, json.JSONDecodeError, ValueError, ValidationError) as exc:
        raise BackendOtaRequestInvalid(str(exc)) from exc
    return event, raw_text, hashlib.sha256(raw).hexdigest()


def build_receipt_preimage(command: dict, end: dict) -> bytes:
    values = {
        **command,
        **end,
        "command_type": command["type"],
        "role_healthy": int(end["role_healthy"]),
        "radio_healthy": int(end["radio_healthy"]),
        "rollback_clear": int(end["rollback_clear"]),
    }
    fields = (
        "operation_id", "command_type", "component", "catalog_name",
        "expected_sha256", "expected_size", "expected_uplink_mac",
        "expected_uplink_boot_id", "expected_target_mac",
        "expected_target_boot_id", "expected_topology_generation", "state",
        "decision", "error", "image_writes", "target", "project",
        "hardware", "version", "actual_mac", "actual_boot_id",
        "actual_topology_generation", "role_healthy", "radio_healthy",
        "rollback_clear",
    )
    lines = ["fof-backend-ota-end-receipt-v1"]
    lines.extend(f"{field}={values[field]}" for field in fields)
    return ("\n".join(lines) + "\n").encode("utf-8")


def _u32(value: object, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise BackendOtaConflict(f"{label} is incomplete")
    if not 1 <= value <= UINT32_MAX:
        raise BackendOtaConflict(f"{label} is incomplete")
    return value


def _mac(value: object, label: str) -> str:
    normalized = str(value or "").strip().upper().replace("-", ":")
    parts = normalized.split(":")
    if len(parts) != 6 or any(
        len(part) != 2 or any(char not in "0123456789ABCDEF" for char in part)
        for part in parts
    ):
        raise BackendOtaConflict(f"{label} is incomplete")
    return normalized


def _exact_identity(report: dict, *, component: str) -> None:
    identity = resolve_component_management_identity(report, component)
    expected_catalog = UPLINK_CATALOG if component == "uplink" else SCANNER_CATALOG
    if (
        identity.get("catalog_name") != expected_catalog
        or identity.get("product_family") != "s3_fullsize"
        or identity.get("firmware_line") != "backend"
        or identity.get("component") != component
        or identity.get("management_blockers")
    ):
        raise BackendOtaConflict(f"{component} Fullsize identity is incompatible")


def _binding_from_snapshot(snapshot: dict, *, now: float) -> dict:
    heartbeat = snapshot.get("heartbeat")
    if not isinstance(heartbeat, dict):
        raise BackendOtaConflict("missing uplink heartbeat")
    blockers = remote_update_blockers(heartbeat, SCANNER_CATALOG, now=now)
    if blockers:
        raise BackendOtaConflict(
            f"S3 Fullsize remote update blocked: {', '.join(blockers)}",
        )
    _exact_identity(heartbeat, component="uplink")
    uplink = {
        "mac": _mac(
            heartbeat.get("hardware_mac") or heartbeat.get("mac"),
            "uplink MAC",
        ),
        "boot_id": _u32(heartbeat.get("boot_id"), "uplink boot_id"),
        "topology_generation": _u32(
            heartbeat.get("topology_generation"),
            "uplink topology_generation",
        ),
        "target": UPLINK_CATALOG,
        "project": UPLINK_PROJECT,
        "hardware": FULLSIZE_HARDWARE,
        "version": str(heartbeat.get("firmware_version") or ""),
    }
    scanners = [
        item for item in snapshot.get("scanners") or []
        if isinstance(item, dict)
    ]
    by_binding = {
        (item.get("uart"), item.get("slot")): item
        for item in scanners
    }
    if set(by_binding) != {("ble", 0), ("wifi", 1)} or len(scanners) != 2:
        raise BackendOtaConflict("scanner slot/UART binding is incompatible")

    bound_scanners: dict[str, dict] = {}
    for component, key in (("scanner0", ("ble", 0)), ("scanner1", ("wifi", 1))):
        scanner = by_binding[key]
        _exact_identity(scanner, component="scanner")
        if not all((
            scanner.get("role_acked") is True,
            scanner.get("command_ingress") is True,
            scanner.get("radio_healthy") is True,
            scanner.get("ota_state") == "idle",
            scanner.get("rollback_state") == "valid",
        )):
            raise BackendOtaConflict(f"{component} is not healthy")
        bound_scanners[component] = {
            "uart": key[0],
            "slot": key[1],
            "mac": _mac(scanner.get("mac") or scanner.get("hardware_mac"), "scanner MAC"),
            "boot_id": _u32(scanner.get("boot_id"), "scanner boot_id"),
            "role_generation": _u32(
                scanner.get("role_generation"), "scanner role_generation",
            ),
            "target": SCANNER_CATALOG,
            "project": SCANNER_PROJECT,
            "hardware": FULLSIZE_HARDWARE,
            "version": str(scanner.get("firmware_version") or scanner.get("version") or ""),
        }
    macs = {uplink["mac"], *(item["mac"] for item in bound_scanners.values())}
    if len(macs) != 3:
        raise BackendOtaConflict("Fullsize trio MACs must be distinct")
    return {
        "device_id": snapshot.get("device_id"),
        "ip": str(snapshot.get("ip") or ""),
        "uplink": uplink,
        **bound_scanners,
    }


def _image_metadata(name: str, image: bytes | None) -> dict:
    if image is None:
        raise BackendOtaConflict(f"firmware {name} is unavailable")
    identity = _validated_backend_image_info(name, image)
    if identity is None:
        raise BackendOtaConflict(f"firmware {name} failed identity validation")
    return {
        "catalog_name": name,
        "target": identity["target"],
        "project": identity["project"],
        "hardware": identity["hardware"],
        "version": identity["version"],
        "size": len(image),
        "sha256": hashlib.sha256(image).hexdigest(),
    }


def _utc(now: float) -> datetime:
    return datetime.fromtimestamp(now, tz=timezone.utc)


def _canonical(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


class BackendOtaService:
    def __init__(self, *, clock: Callable[[], float] = time.time):
        self._clock = clock

    def _fresh_binding(
        self,
        snapshot_provider: Callable[[str, str], dict],
        device_id: str,
    ) -> dict:
        return _binding_from_snapshot(
            snapshot_provider(device_id, "both"), now=self._clock(),
        )

    def _require_same_binding(
        self,
        initial: dict,
        snapshot_provider: Callable[[str, str], dict],
        device_id: str,
    ) -> None:
        if self._fresh_binding(snapshot_provider, device_id) != initial:
            raise BackendOtaConflict(
                "Fullsize binding changed during firmware fetch",
            )

    async def create_rollout(
        self,
        db: AsyncSession,
        device_id: str,
        request: BackendOtaRolloutRequest,
        *,
        now: float,
        firmware_manager: FirmwareManager,
        snapshot_provider: Callable[[str, str], dict],
    ) -> BackendOtaProbeEnvelope:
        binding = self._fresh_binding(snapshot_provider, device_id)
        scanner_bytes = await firmware_manager.get_firmware_binary(SCANNER_CATALOG)
        self._require_same_binding(binding, snapshot_provider, device_id)
        scanner_image = _image_metadata(SCANNER_CATALOG, scanner_bytes)
        uplink_bytes = await firmware_manager.get_firmware_binary(UPLINK_CATALOG)
        self._require_same_binding(binding, snapshot_provider, device_id)
        uplink_image = _image_metadata(UPLINK_CATALOG, uplink_bytes)
        self._require_same_binding(binding, snapshot_provider, device_id)

        operation_id = secrets.token_hex(16)
        row = BackendOtaRollout(
            operation_id=operation_id,
            device_id=device_id,
            active_key=device_id,
            apply_mode=request.apply_mode,
            binding_json=_canonical(binding),
            scanner_image_json=_canonical(scanner_image),
            uplink_image_json=_canonical(uplink_image),
            state="active",
            current_component="scanner0",
            current_action="probe",
            next_sequence=0,
            created_at=_utc(now),
        )
        db.add(row)
        try:
            await db.commit()
        except IntegrityError as exc:
            await db.rollback()
            raise BackendOtaConflict("node already has an active rollout") from exc
        except OperationalError as exc:
            await db.rollback()
            raise BackendOtaUnavailable("backend OTA store unavailable") from exc
        return _probe_envelope(row)

    async def next_for_device(
        self,
        db: AsyncSession,
        device_id: str,
        *,
        now: float,
    ) -> BackendOtaCommandEnvelope | None:
        try:
            await _begin_transaction(db)
            row = await _active_rollout(db, device_id, for_update=True)
            if row is None:
                await db.rollback()
                return None
            stamp = _utc(now)
            row.first_delivered_at = row.first_delivered_at or stamp
            row.last_polled_at = stamp
            await db.commit()
            return _command_envelope(row)
        except OperationalError as exc:
            await db.rollback()
            raise BackendOtaUnavailable("backend OTA store unavailable") from exc

    async def record_event(
        self,
        db: AsyncSession,
        device_id: str,
        operation_id: str,
        event: BackendOtaEventRequest,
        raw_payload: str,
        body_sha256: str,
        *,
        now: float,
        snapshot_provider: Callable[[str, str], dict],
    ) -> BackendOtaEventAck:
        try:
            await _begin_transaction(db)
            row = await _rollout_for_update(db, device_id, operation_id)
            if row is None:
                raise BackendOtaNotFound(operation_id)

            prior = await _event_at_sequence(db, operation_id, event.sequence)
            if prior is not None:
                if (
                    prior.body_sha256 != body_sha256
                    or prior.raw_payload != raw_payload
                ):
                    raise BackendOtaConflict("sequence body differs")
                ack = _event_ack(row, event.sequence, duplicate=True)
                await db.rollback()
                return ack

            if row.active_key is None:
                raise BackendOtaConflict("rollout is terminal")
            if event.sequence != row.next_sequence:
                raise BackendOtaConflict(f"expected sequence {row.next_sequence}")
            if row.next_sequence >= UINT32_MAX:
                raise BackendOtaConflict("sequence space exhausted")

            command = _command_envelope(row).model_dump(mode="json")
            _validate_event_binding(row, event, command)
            _apply_event_transition(
                row,
                event,
                command,
                snapshot_provider=snapshot_provider,
                now=self._clock(),
            )
            stored = BackendOtaEvent(
                operation_id=operation_id,
                sequence=event.sequence,
                event_type=event.type,
                raw_payload=raw_payload,
                body_sha256=body_sha256,
                created_at=_utc(now),
            )
            try:
                async with db.begin_nested():
                    db.add(stored)
                    await db.flush()
            except IntegrityError:
                prior = await _event_at_sequence(db, operation_id, event.sequence)
                if prior is None or (
                    prior.body_sha256 != body_sha256
                    or prior.raw_payload != raw_payload
                ):
                    raise BackendOtaConflict("sequence body differs")
                await db.refresh(row)
                ack = _event_ack(row, event.sequence, duplicate=True)
                await db.commit()
                return ack
            row.next_sequence += 1
            await db.commit()
            return _event_ack(row, event.sequence, duplicate=False)
        except OperationalError as exc:
            await db.rollback()
            raise BackendOtaUnavailable("backend OTA store unavailable") from exc
        except (BackendOtaConflict, BackendOtaNotFound):
            await db.rollback()
            raise

    async def history_for_device(
        self,
        db: AsyncSession,
        device_id: str,
        operation_id: str,
    ) -> BackendOtaHistoryResponse:
        try:
            row = await db.scalar(
                select(BackendOtaRollout).where(
                    BackendOtaRollout.device_id == device_id,
                    BackendOtaRollout.operation_id == operation_id,
                ),
            )
            if row is None:
                raise BackendOtaNotFound(operation_id)
            events = list((await db.scalars(
                select(BackendOtaEvent).where(
                    BackendOtaEvent.operation_id == operation_id,
                ).order_by(BackendOtaEvent.sequence),
            )).all())
            return BackendOtaHistoryResponse(
                operation_id=row.operation_id,
                device_id=row.device_id,
                state=row.state,
                apply_mode=row.apply_mode,
                current_component=row.current_component,
                current_action=row.current_action,
                next_sequence=row.next_sequence,
                terminal=row.active_key is None,
                events=[
                    _EVENT_ADAPTER.validate_json(item.raw_payload)
                    for item in events
                ],
            )
        except OperationalError as exc:
            await db.rollback()
            raise BackendOtaUnavailable("backend OTA store unavailable") from exc


def _probe_envelope(row: BackendOtaRollout) -> BackendOtaProbeEnvelope:
    binding = json.loads(row.binding_json)
    image = json.loads(
        row.uplink_image_json
        if row.current_component == "uplink" else row.scanner_image_json
    )
    target = binding[row.current_component]
    uplink = binding["uplink"]
    return BackendOtaProbeEnvelope(
        schema=1,
        operation_id=row.operation_id,
        type="backend_ota_probe",
        component=row.current_component,
        catalog_name=image["catalog_name"],
        expected_sha256=image["sha256"],
        expected_size=image["size"],
        expected_uplink_mac=uplink["mac"],
        expected_uplink_boot_id=uplink["boot_id"],
        expected_target_mac=target["mac"],
        expected_target_boot_id=target["boot_id"],
        expected_topology_generation=uplink["topology_generation"],
        next_sequence=row.next_sequence,
    )


def _apply_envelope(row: BackendOtaRollout) -> BackendOtaApplyEnvelope:
    probe = _probe_envelope(row).model_dump(mode="json")
    probe.update({
        "type": "backend_ota_apply",
        "apply_mode": row.apply_mode,
        "probe_receipt_sha256": row.accepted_probe_receipt,
    })
    return BackendOtaApplyEnvelope.model_validate(probe)


def _command_envelope(row: BackendOtaRollout):
    if row.current_action == "apply":
        return _apply_envelope(row)
    return _probe_envelope(row)


async def _begin_transaction(db: AsyncSession) -> None:
    if db.bind is not None and db.bind.dialect.name == "sqlite":
        await db.execute(text("BEGIN IMMEDIATE"))


async def _active_rollout(
    db: AsyncSession,
    device_id: str,
    *,
    for_update: bool = False,
) -> BackendOtaRollout | None:
    statement = select(BackendOtaRollout).where(
        BackendOtaRollout.active_key == device_id,
    )
    if for_update:
        statement = statement.with_for_update()
    return await db.scalar(statement)


async def _rollout_for_update(
    db: AsyncSession,
    device_id: str,
    operation_id: str,
) -> BackendOtaRollout | None:
    return await db.scalar(
        select(BackendOtaRollout).where(
            BackendOtaRollout.device_id == device_id,
            BackendOtaRollout.operation_id == operation_id,
        ).with_for_update(),
    )


async def _event_at_sequence(
    db: AsyncSession,
    operation_id: str,
    sequence: int,
) -> BackendOtaEvent | None:
    return await db.scalar(
        select(BackendOtaEvent).where(
            BackendOtaEvent.operation_id == operation_id,
            BackendOtaEvent.sequence == sequence,
        ),
    )


def _event_ack(
    row: BackendOtaRollout,
    sequence: int,
    *,
    duplicate: bool,
) -> BackendOtaEventAck:
    return BackendOtaEventAck(
        operation_id=row.operation_id,
        accepted_sequence=sequence,
        next_sequence=row.next_sequence,
        current_component=row.current_component,
        current_action=row.current_action,
        terminal=row.active_key is None,
        duplicate=duplicate,
    )


def _validate_event_binding(
    row: BackendOtaRollout,
    event: BackendOtaEventRequest,
    command: dict,
) -> None:
    if event.operation_id != row.operation_id:
        raise BackendOtaConflict("operation ID differs")
    if event.component != row.current_component:
        raise BackendOtaConflict("component differs from current command")
    if event.catalog_name != command["catalog_name"]:
        raise BackendOtaConflict("catalog differs from current command")


def _reset_phase_progress(row: BackendOtaRollout) -> None:
    row.began = False
    row.last_stage_rank = -1
    row.last_received = 0
    row.progress_total = None
    row.retry_count = 0


def _apply_event_transition(
    row: BackendOtaRollout,
    event: BackendOtaEventRequest,
    command: dict,
    *,
    snapshot_provider: Callable[[str, str], dict],
    now: float,
) -> None:
    if isinstance(event, BackendOtaBeginEvent):
        if row.began:
            raise BackendOtaConflict("begin event already accepted")
        row.began = True
        return
    if not row.began:
        raise BackendOtaConflict("begin event required first")
    if isinstance(event, BackendOtaProgressEvent):
        allowed = (
            set(_PROBE_STAGES)
            if row.current_action == "probe" else set(_STAGE_RANK)
        )
        if row.current_component == "uplink":
            allowed.discard("uart_relay")
        if event.stage not in allowed:
            raise BackendOtaConflict("stage is invalid for current command")
        rank = _STAGE_RANK[event.stage]
        if rank < row.last_stage_rank:
            raise BackendOtaConflict("stage regression")
        if event.received > event.total:
            raise BackendOtaConflict("received count regression")
        if rank == row.last_stage_rank:
            if event.received < row.last_received:
                raise BackendOtaConflict("received count regression")
            if row.progress_total is not None and event.total != row.progress_total:
                raise BackendOtaConflict("progress total differs")
            if event.retry_count < row.retry_count:
                raise BackendOtaConflict("retry count regression")
        row.last_stage_rank = rank
        row.last_received = event.received
        row.progress_total = event.total
        row.retry_count = event.retry_count
        return
    if isinstance(event, BackendOtaEndEvent):
        _validate_end(row, event, command, snapshot_provider, now)
        return
    raise BackendOtaConflict("unsupported backend OTA event")


def _validate_receipt(event: BackendOtaEndEvent, command: dict) -> None:
    end = event.model_dump(mode="json", exclude={"receipt_sha256"})
    digest = hashlib.sha256(build_receipt_preimage(command, end)).hexdigest()
    if not secrets.compare_digest(digest, event.receipt_sha256):
        raise BackendOtaConflict("terminal receipt differs")


def _validate_end(
    row: BackendOtaRollout,
    event: BackendOtaEndEvent,
    command: dict,
    snapshot_provider: Callable[[str, str], dict],
    now: float,
) -> None:
    _validate_receipt(event, command)
    pair = (event.state, event.decision)
    valid_pairs = {
        ("complete", "eligible"),
        ("complete", "applied"),
        ("no_update", "no_update"),
        ("failed", "rejected"),
        ("rolled_back", "rolled_back"),
    }
    if pair not in valid_pairs:
        raise BackendOtaConflict("invalid terminal state/decision pair")
    if pair in {
        ("complete", "eligible"),
        ("complete", "applied"),
        ("no_update", "no_update"),
    } and event.error != "none":
        raise BackendOtaConflict("successful terminal requires error none")
    if pair in {("failed", "rejected"), ("rolled_back", "rolled_back")}:
        if event.error == "none":
            raise BackendOtaConflict("terminal failure requires an error")
        row.state = event.state
        row.active_key = None
        row.completed_at = _utc(now)
        return

    if pair == ("complete", "eligible"):
        if row.current_action != "probe" or event.image_writes != 0:
            raise BackendOtaConflict("invalid probe terminal tuple")
        _validate_success_identity(
            row, event, command, require_new_boot=False,
        )
        current = _current_binding(row, snapshot_provider, now)
        _validate_probe_live_binding(row, current)
        row.accepted_probe_receipt = event.receipt_sha256
        row.current_action = "apply"
        _reset_phase_progress(row)
        return

    if pair == ("no_update", "no_update"):
        if row.current_action != "probe" or event.image_writes != 0:
            raise BackendOtaConflict("invalid no-update terminal tuple")
        _validate_success_identity(
            row, event, command, require_new_boot=False,
        )
        current = _current_binding(row, snapshot_provider, now)
        _validate_probe_live_binding(row, current)
        target = current[row.current_component]
        if (
            target["mac"] != event.actual_mac
            or target["boot_id"] != event.actual_boot_id
            or target["version"] != event.version
        ):
            raise BackendOtaConflict("no-update heartbeat has not converged")
        if row.current_component == "scanner0":
            row.scanner0_converged_boot_id = event.actual_boot_id
        elif row.current_component == "scanner1":
            row.scanner1_converged_boot_id = event.actual_boot_id
        _advance_component(row, now)
        return

    if pair == ("complete", "applied"):
        if row.current_action != "apply" or event.image_writes <= 0:
            raise BackendOtaConflict("invalid apply terminal tuple")
        _validate_success_identity(
            row, event, command, require_new_boot=True,
        )
        current = _current_binding(row, snapshot_provider, now)
        _validate_apply_convergence(row, event, command, current)
        if row.current_component == "scanner0":
            row.scanner0_converged_boot_id = event.actual_boot_id
        elif row.current_component == "scanner1":
            row.scanner1_converged_boot_id = event.actual_boot_id
        _advance_component(row, now)
        return
    raise BackendOtaConflict("unsupported terminal transition")


def _rollout_image(row: BackendOtaRollout) -> dict:
    return json.loads(
        row.scanner_image_json
        if row.current_component != "uplink"
        else row.uplink_image_json
    )


def _validate_success_identity(
    row: BackendOtaRollout,
    event: BackendOtaEndEvent,
    command: dict,
    *,
    require_new_boot: bool,
) -> None:
    image = _rollout_image(row)
    expected = (
        image["target"], image["project"], image["hardware"], image["version"],
        command["expected_target_mac"], command["expected_topology_generation"],
        True, True, True,
    )
    actual = (
        event.target, event.project, event.hardware, event.version,
        event.actual_mac, event.actual_topology_generation, event.role_healthy,
        event.radio_healthy, event.rollback_clear,
    )
    if actual != expected:
        raise BackendOtaConflict("terminal identity or health differs")
    if require_new_boot:
        if event.actual_boot_id == command["expected_target_boot_id"]:
            raise BackendOtaConflict("apply did not report a new target boot ID")
    elif event.actual_boot_id != command["expected_target_boot_id"]:
        raise BackendOtaConflict("probe target boot ID differs")


def _current_binding(
    row: BackendOtaRollout,
    snapshot_provider: Callable[[str, str], dict],
    now: float,
) -> dict:
    try:
        current = _binding_from_snapshot(
            snapshot_provider(row.device_id, "both"), now=now,
        )
    except BackendOtaConflict as exc:
        raise BackendOtaConflict(f"converged heartbeat is invalid: {exc}") from exc
    expected = json.loads(row.binding_json)
    if (
        current["device_id"] != expected["device_id"]
        or current["ip"] != expected["ip"]
        or current["uplink"]["mac"] != expected["uplink"]["mac"]
        or current["uplink"]["topology_generation"]
        != expected["uplink"]["topology_generation"]
        or current["scanner0"]["mac"] != expected["scanner0"]["mac"]
        or current["scanner1"]["mac"] != expected["scanner1"]["mac"]
    ):
        raise BackendOtaConflict("converged heartbeat binding differs")
    return current


def _validate_apply_convergence(
    row: BackendOtaRollout,
    event: BackendOtaEndEvent,
    command: dict,
    current: dict,
) -> None:
    original = json.loads(row.binding_json)
    target = current[row.current_component]
    if (
        target["mac"] != event.actual_mac
        or target["boot_id"] != event.actual_boot_id
        or target["version"] != event.version
    ):
        raise BackendOtaConflict("converged heartbeat does not match apply end")
    if row.current_component != "uplink" and (
        current["uplink"]["boot_id"] != original["uplink"]["boot_id"]
    ):
        raise BackendOtaConflict("uplink heartbeat binding changed")
    if row.current_component == "scanner0":
        if current["scanner1"]["boot_id"] != original["scanner1"]["boot_id"]:
            raise BackendOtaConflict("scanner1 heartbeat binding changed")
    elif row.current_component == "scanner1":
        if current["scanner0"]["boot_id"] != row.scanner0_converged_boot_id:
            raise BackendOtaConflict("scanner0 did not remain converged")
    else:
        if (
            current["scanner0"]["boot_id"] != row.scanner0_converged_boot_id
            or current["scanner1"]["boot_id"] != row.scanner1_converged_boot_id
        ):
            raise BackendOtaConflict("both scanners must rejoin before completion")


def _validate_probe_live_binding(
    row: BackendOtaRollout,
    current: dict,
) -> None:
    """Require the whole live trio to match the rollout's current phase."""
    original = json.loads(row.binding_json)
    scanner_image = json.loads(row.scanner_image_json)
    component_order = ("scanner0", "scanner1", "uplink")
    current_index = component_order.index(row.current_component)

    if current["uplink"]["boot_id"] != original["uplink"]["boot_id"]:
        raise BackendOtaConflict("uplink heartbeat binding changed")
    if current["uplink"]["version"] != original["uplink"]["version"]:
        raise BackendOtaConflict("uplink heartbeat version changed")

    for index, component in enumerate(component_order[:2]):
        if index < current_index:
            expected_boot_id = getattr(row, f"{component}_converged_boot_id")
            expected_version = scanner_image["version"]
        else:
            expected_boot_id = original[component]["boot_id"]
            expected_version = original[component]["version"]
        if current[component]["boot_id"] != expected_boot_id:
            raise BackendOtaConflict(f"{component} heartbeat binding changed")
        if current[component]["version"] != expected_version:
            raise BackendOtaConflict(f"{component} heartbeat version changed")


def _advance_component(row: BackendOtaRollout, now: float) -> None:
    if row.current_component == "scanner0":
        row.current_component = "scanner1"
    elif row.current_component == "scanner1":
        row.current_component = "uplink"
    else:
        row.state = "complete"
        row.active_key = None
        row.completed_at = _utc(now)
    row.current_action = "probe"
    row.accepted_probe_receipt = None
    _reset_phase_progress(row)
