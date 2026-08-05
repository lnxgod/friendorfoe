"""Durable, restart-safe lifecycle for per-node BLE investigation commands."""

from dataclasses import dataclass
from datetime import datetime, timezone
import json
import uuid

from pydantic import TypeAdapter
from sqlalchemy import select, text
from sqlalchemy.exc import IntegrityError, OperationalError
from sqlalchemy.ext.asyncio import AsyncSession

from app.models.db_models import NodeCommand, NodeCommandResultEvent
from app.models.schemas import (
    BleInvestigationCreateRequest,
    BleInvestigateCancelEnvelope,
    BleInvestigateCommandEnvelope,
    NodeCommandEnvelope,
    NodeCommandHistoryResponse,
    NodeCommandResultAck,
    NodeCommandResultRequest,
)


_RESULT_ADAPTER = TypeAdapter(NodeCommandResultRequest)
_PROGRESS_RANK = {
    "queued": 0,
    "scanning": 1,
    "connecting": 2,
    "discovering": 3,
    "reading": 4,
}


class NodeCommandError(Exception):
    """Base class for command lifecycle failures."""


class NodeCommandConflict(NodeCommandError):
    """The requested command operation conflicts with persisted state."""


class NodeCommandNotFound(NodeCommandError):
    """The command does not exist for the specified node."""


class NodeCommandUnavailable(NodeCommandError):
    """The command store could not serialize the operation in bounded time."""

    retryable = True


@dataclass(frozen=True)
class _ResultTransition:
    result_state: str
    next_service_index: int
    next_characteristic_index: int
    next_read_index: int
    terminal: bool = False


class NodeCommandService:
    async def enqueue_ble_investigation(
        self,
        db: AsyncSession,
        device_id: str,
        request: BleInvestigationCreateRequest,
        *,
        now: float,
    ) -> NodeCommandEnvelope:
        command_id = uuid.uuid4().hex
        payload = request.model_dump(mode="json")
        row = NodeCommand(
            command_id=command_id,
            device_id=device_id,
            active_key=device_id,
            command_type="ble_investigate",
            payload_json=_canonical_json(payload),
            state="pending",
            next_sequence=0,
            created_at=_utc_from_epoch(now),
        )
        db.add(row)
        try:
            await db.commit()
        except IntegrityError as exc:
            await db.rollback()
            raise NodeCommandConflict("node already has an active command") from exc
        except OperationalError as exc:
            await db.rollback()
            raise NodeCommandUnavailable("node command store unavailable") from exc
        return _command_envelope(row)

    async def next_for_device(
        self,
        db: AsyncSession,
        device_id: str,
        *,
        now: float,
    ) -> NodeCommandEnvelope | None:
        try:
            await _begin_command_transaction(db)
            row = await _active_command(db, device_id, for_update=True)
            if row is None:
                await db.rollback()
                return None
            stamp = _utc_from_epoch(now)
            row.first_delivered_at = row.first_delivered_at or stamp
            row.last_polled_at = stamp
            row.state = "delivered" if row.state == "pending" else row.state
            await db.commit()
            return _command_envelope(row)
        except OperationalError as exc:
            await db.rollback()
            raise NodeCommandUnavailable("node command store unavailable") from exc

    async def request_cancel(
        self,
        db: AsyncSession,
        device_id: str,
        command_id: str,
        *,
        now: float,
    ) -> NodeCommandEnvelope:
        try:
            await _begin_command_transaction(db)
            row = await _command_for_update(db, device_id, command_id)
            if row is None or row.active_key is None:
                raise NodeCommandNotFound(command_id)
            row.state = "cancel_pending"
            row.last_polled_at = _utc_from_epoch(now)
            await db.commit()
            return _command_envelope(row)
        except OperationalError as exc:
            await db.rollback()
            raise NodeCommandUnavailable("node command store unavailable") from exc
        except NodeCommandNotFound:
            await db.rollback()
            raise

    async def record_result(
        self,
        db: AsyncSession,
        device_id: str,
        command_id: str,
        result: NodeCommandResultRequest,
        *,
        now: float,
    ) -> NodeCommandResultAck:
        try:
            await _begin_command_transaction(db)
            row = await _command_for_update(db, device_id, command_id)
            if row is None:
                raise NodeCommandNotFound(command_id)

            canonical = _canonical_json(result.model_dump(mode="json"))
            prior = await _result_event(db, command_id, result.sequence)
            if prior is not None:
                if prior.payload_json != canonical:
                    raise NodeCommandConflict("sequence body differs")
                ack = _result_ack(row, prior, duplicate=True)
                await db.rollback()
                return ack
            if result.sequence != row.next_sequence:
                raise NodeCommandConflict(f"expected sequence {row.next_sequence}")

            transition = _validate_result_transition(row, result)
            event = NodeCommandResultEvent(
                command_id=command_id,
                sequence=result.sequence,
                event_type=result.type,
                payload_json=canonical,
                created_at=_utc_from_epoch(now),
            )
            try:
                async with db.begin_nested():
                    db.add(event)
                    await db.flush()
            except IntegrityError:
                await db.refresh(row)
                prior = await _result_event(db, command_id, result.sequence)
                if prior is None or prior.payload_json != canonical:
                    raise NodeCommandConflict("sequence body differs")
                ack = _result_ack(row, prior, duplicate=True)
                await db.commit()
                return ack

            _apply_result_transition(
                row, transition, completed_at=_utc_from_epoch(now),
            )
            await db.commit()
            return _result_ack(row, event, duplicate=False)
        except OperationalError as exc:
            await db.rollback()
            raise NodeCommandUnavailable("node command store unavailable") from exc
        except (NodeCommandConflict, NodeCommandNotFound):
            await db.rollback()
            raise

    async def history_for_device(
        self,
        db: AsyncSession,
        device_id: str,
        command_id: str,
    ) -> NodeCommandHistoryResponse:
        try:
            row = await _command_for_update(db, device_id, command_id)
            if row is None:
                await db.rollback()
                raise NodeCommandNotFound(command_id)
            events = list((await db.scalars(
                select(NodeCommandResultEvent)
                .where(NodeCommandResultEvent.command_id == command_id)
                .order_by(NodeCommandResultEvent.sequence)
            )).all())
            return NodeCommandHistoryResponse(
                command_id=row.command_id,
                device_id=row.device_id,
                command_type=row.command_type,
                state=row.state,
                next_sequence=row.next_sequence,
                result_state=row.result_state,
                terminal=row.active_key is None,
                events=[
                    _RESULT_ADAPTER.validate_json(event.payload_json)
                    for event in events
                ],
            )
        except OperationalError as exc:
            await db.rollback()
            raise NodeCommandUnavailable("node command store unavailable") from exc


def _canonical_json(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def _utc_from_epoch(now: float) -> datetime:
    return datetime.fromtimestamp(now, tz=timezone.utc)


async def _active_command(
    db: AsyncSession, device_id: str, *, for_update: bool = False,
) -> NodeCommand | None:
    statement = select(NodeCommand).where(NodeCommand.active_key == device_id)
    if for_update:
        statement = statement.with_for_update()
    return await db.scalar(statement)


async def _begin_command_transaction(db: AsyncSession) -> None:
    if db.bind is not None and db.bind.dialect.name == "sqlite":
        await db.execute(text("BEGIN IMMEDIATE"))


async def _command_for_update(
    db: AsyncSession, device_id: str, command_id: str,
) -> NodeCommand | None:
    return await db.scalar(
        select(NodeCommand)
        .where(
            NodeCommand.device_id == device_id,
            NodeCommand.command_id == command_id,
        )
        .with_for_update()
    )


async def _result_event(
    db: AsyncSession, command_id: str, sequence: int,
) -> NodeCommandResultEvent | None:
    return await db.scalar(
        select(NodeCommandResultEvent).where(
            NodeCommandResultEvent.command_id == command_id,
            NodeCommandResultEvent.sequence == sequence,
        )
    )


def _command_envelope(row: NodeCommand) -> NodeCommandEnvelope:
    payload = json.loads(row.payload_json)
    values = {
        "command_id": row.command_id,
        "type": (
            "ble_investigate_cancel"
            if row.state == "cancel_pending"
            else "ble_investigate"
        ),
        "request_id": row.command_id,
        "mode": payload["mode"],
        "target": payload["target_mac"],
        "timeout_ms": payload["timeout_ms"],
        "next_sequence": row.next_sequence,
        "result_state": row.result_state,
    }
    if row.state == "cancel_pending":
        return BleInvestigateCancelEnvelope.model_validate(values)
    return BleInvestigateCommandEnvelope.model_validate(values)


def _result_ack(
    row: NodeCommand,
    event: NodeCommandResultEvent,
    *,
    duplicate: bool,
) -> NodeCommandResultAck:
    return NodeCommandResultAck(
        command_id=row.command_id,
        accepted_sequence=event.sequence,
        next_sequence=row.next_sequence,
        result_state=row.result_state,
        terminal=row.active_key is None,
        duplicate=duplicate,
    )


def _validate_result_transition(
    row: NodeCommand, result: NodeCommandResultRequest,
) -> _ResultTransition:
    if result.request_id != row.command_id:
        raise NodeCommandConflict("request ID differs")
    if row.active_key is None:
        raise NodeCommandConflict("command is terminal")

    current = _ResultTransition(
        result_state=row.result_state or "queued",
        next_service_index=row.next_service_index,
        next_characteristic_index=row.next_characteristic_index,
        next_read_index=row.next_read_index,
    )
    if result.type == "ble_inv_begin":
        if row.next_sequence != 0 or row.result_state is not None:
            raise NodeCommandConflict("begin event already accepted")
        payload = json.loads(row.payload_json)
        if (
            result.mode != payload["mode"]
            or result.target_mac != payload["target_mac"]
        ):
            raise NodeCommandConflict("begin event differs from command")
        return _ResultTransition(
            result_state="queued",
            next_service_index=row.next_service_index,
            next_characteristic_index=row.next_characteristic_index,
            next_read_index=row.next_read_index,
        )

    if row.next_sequence == 0 or row.result_state is None:
        raise NodeCommandConflict("begin event required first")

    if result.type == "ble_inv_progress":
        if _PROGRESS_RANK[result.state] < _PROGRESS_RANK[row.result_state]:
            raise NodeCommandConflict("state regression")
        return _ResultTransition(
            result_state=result.state,
            next_service_index=row.next_service_index,
            next_characteristic_index=row.next_characteristic_index,
            next_read_index=row.next_read_index,
        )
    if result.type == "ble_inv_service":
        if result.index != row.next_service_index:
            raise NodeCommandConflict(
                f"expected service index {row.next_service_index}",
            )
        return _ResultTransition(
            result_state=current.result_state,
            next_service_index=row.next_service_index + 1,
            next_characteristic_index=row.next_characteristic_index,
            next_read_index=row.next_read_index,
        )
    if result.type == "ble_inv_char":
        if result.index != row.next_characteristic_index:
            raise NodeCommandConflict(
                f"expected characteristic index {row.next_characteristic_index}",
            )
        return _ResultTransition(
            result_state=current.result_state,
            next_service_index=row.next_service_index,
            next_characteristic_index=row.next_characteristic_index + 1,
            next_read_index=row.next_read_index,
        )
    if result.type == "ble_inv_read":
        if result.index != row.next_read_index:
            raise NodeCommandConflict(f"expected read index {row.next_read_index}")
        return _ResultTransition(
            result_state=current.result_state,
            next_service_index=row.next_service_index,
            next_characteristic_index=row.next_characteristic_index,
            next_read_index=row.next_read_index + 1,
        )
    if result.type == "ble_inv_end":
        return _ResultTransition(
            result_state=result.state,
            next_service_index=row.next_service_index,
            next_characteristic_index=row.next_characteristic_index,
            next_read_index=row.next_read_index,
            terminal=True,
        )
    raise NodeCommandConflict("unsupported result event")


def _apply_result_transition(
    row: NodeCommand,
    transition: _ResultTransition,
    *,
    completed_at: datetime,
) -> None:
    row.next_sequence += 1
    row.result_state = transition.result_state
    row.next_service_index = transition.next_service_index
    row.next_characteristic_index = transition.next_characteristic_index
    row.next_read_index = transition.next_read_index
    if transition.terminal:
        row.active_key = None
        row.state = "terminal"
        row.completed_at = completed_at
