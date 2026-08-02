import asyncio

import pytest
from pydantic import TypeAdapter
from sqlalchemy import select

import app.services.node_commands as node_commands
from app.models.db_models import NodeCommand, NodeCommandResultEvent
from app.models.schemas import BleInvestigationCreateRequest, NodeCommandResultRequest
from app.services.node_commands import (
    NodeCommandConflict,
    NodeCommandNotFound,
    NodeCommandService,
)


RESULT = TypeAdapter(NodeCommandResultRequest)


@pytest.mark.asyncio
async def test_command_poll_and_duplicate_result_contract(client):
    created = await client.post("/nodes/uplink_CB77A4/commands/ble-investigate", json={
        "target_mac": None,
        "mode": "passive_capture",
        "timeout_ms": 12000,
    })
    assert created.status_code == 201
    command_id = created.json()["command_id"]

    first = await client.get("/nodes/uplink_CB77A4/commands/next")
    second = await client.get("/nodes/uplink_CB77A4/commands/next")
    assert first.json() == second.json()
    assert first.json() == {
        "command_id": command_id,
        "type": "ble_investigate",
        "request_id": command_id,
        "mode": "passive_capture",
        "target": None,
        "timeout_ms": 12000,
        "next_sequence": 0,
        "result_state": None,
    }

    begin_body = {
        "sequence": 0, "type": "ble_inv_begin", "request_id": command_id,
        "mode": "passive_capture", "target_mac": None,
    }
    begin = await client.post(
        f"/nodes/uplink_CB77A4/commands/{command_id}/result", json=begin_body,
    )
    result_body = {
        "sequence": 1, "type": "ble_inv_end", "request_id": command_id,
        "state": "complete", "summary": "passive capture complete",
        "error": None, "authentication_required": False, "truncated": False,
    }
    result = await client.post(
        f"/nodes/uplink_CB77A4/commands/{command_id}/result", json=result_body,
    )
    duplicate = await client.post(
        f"/nodes/uplink_CB77A4/commands/{command_id}/result", json=result_body,
    )
    assert result.status_code == 200
    assert begin.json() == {
        "ok": True, "command_id": command_id, "accepted_sequence": 0,
        "next_sequence": 1, "result_state": "queued", "terminal": False,
        "duplicate": False,
    }
    assert result.json() == {
        "ok": True, "command_id": command_id, "accepted_sequence": 1,
        "next_sequence": 2, "result_state": "complete", "terminal": True,
        "duplicate": False,
    }
    assert duplicate.status_code == 200
    assert duplicate.json()["duplicate"] is True
    idle = await client.get("/nodes/uplink_CB77A4/commands/next")
    assert idle.status_code == 204
    assert idle.content == b""

    history = await client.get(
        f"/nodes/uplink_CB77A4/commands/{command_id}",
    )
    assert history.status_code == 200
    assert history.json() == {
        "command_id": command_id,
        "device_id": "uplink_CB77A4",
        "command_type": "ble_investigate",
        "state": "terminal",
        "next_sequence": 2,
        "result_state": "complete",
        "terminal": True,
        "events": [begin_body, result_body],
    }
    wrong_node = await client.get(
        f"/nodes/uplink_OTHER/commands/{command_id}",
    )
    assert wrong_node.status_code == 404


@pytest.mark.asyncio
async def test_cancel_poll_returns_complete_first_seen_cancel_envelope(client):
    created = await client.post("/nodes/uplink_CB77A4/commands/ble-investigate", json={
        "target_mac": "AA:BB:CC:DD:EE:FF", "mode": "gatt",
    })
    command_id = created.json()["command_id"]
    cancelled = await client.post(
        f"/nodes/uplink_CB77A4/commands/{command_id}/cancel",
    )
    expected = {
        "command_id": command_id,
        "type": "ble_investigate_cancel",
        "request_id": command_id,
        "mode": "gatt",
        "target": "AA:BB:CC:DD:EE:FF",
        "timeout_ms": 12000,
        "next_sequence": 0,
        "result_state": None,
    }
    assert cancelled.status_code == 200
    assert cancelled.json() == expected
    assert (await client.get("/nodes/uplink_CB77A4/commands/next")).json() == expected


@pytest.mark.asyncio
async def test_conflicting_terminal_replay_returns_409(client):
    created = await client.post("/nodes/uplink_CB77A4/commands/ble-investigate", json={
        "target_mac": "AA:BB:CC:DD:EE:FF",
    })
    command_id = created.json()["command_id"]
    await client.post(
        f"/nodes/uplink_CB77A4/commands/{command_id}/result",
        json={
            "sequence": 0, "type": "ble_inv_begin", "request_id": command_id,
            "mode": "gatt", "target_mac": "AA:BB:CC:DD:EE:FF",
        },
    )
    await client.post(
        f"/nodes/uplink_CB77A4/commands/{command_id}/result",
        json={
            "sequence": 1, "type": "ble_inv_end", "request_id": command_id,
            "state": "failed", "summary": "", "error": "timeout",
            "authentication_required": False, "truncated": False,
        },
    )
    conflict = await client.post(
        f"/nodes/uplink_CB77A4/commands/{command_id}/result",
        json={
            "sequence": 1, "type": "ble_inv_end", "request_id": command_id,
            "state": "complete", "summary": "late success", "error": None,
            "authentication_required": False, "truncated": False,
        },
    )
    assert conflict.status_code == 409


def gate_command_transactions(monkeypatch):
    """Make two service calls reach their SQLite write starts deterministically."""
    original = node_commands._begin_command_transaction
    first_started = asyncio.Event()
    release_first = asyncio.Event()
    second_started = asyncio.Event()
    release_second = asyncio.Event()
    call_count = 0

    async def gated(db_session):
        nonlocal call_count
        call_count += 1
        position = call_count
        if position == 2:
            second_started.set()
            await release_second.wait()
        await original(db_session)
        if position == 1:
            first_started.set()
            await release_first.wait()

    monkeypatch.setattr(node_commands, "_begin_command_transaction", gated)
    return first_started, release_first, second_started, release_second


def investigate() -> BleInvestigationCreateRequest:
    return BleInvestigationCreateRequest(
        target_mac="AA:BB:CC:DD:EE:FF",
        mode="gatt",
        timeout_ms=12000,
    )


@pytest.mark.asyncio
async def test_poll_repeats_same_command_until_terminal_result(db_session):
    service = NodeCommandService()
    command = await service.enqueue_ble_investigation(
        db_session, "uplink_CB77A4", investigate(), now=10.0,
    )
    first = await service.next_for_device(db_session, "uplink_CB77A4", now=11.0)
    second = await service.next_for_device(db_session, "uplink_CB77A4", now=12.0)
    assert first == second
    assert first.command_id == command.command_id
    begin = RESULT.validate_python({
        "sequence": 0, "type": "ble_inv_begin", "request_id": command.command_id,
        "mode": "gatt", "target_mac": "AA:BB:CC:DD:EE:FF",
    })
    terminal = RESULT.validate_python({
        "sequence": 1, "type": "ble_inv_end", "request_id": command.command_id,
        "state": "complete", "summary": "done", "error": None,
        "authentication_required": False, "truncated": False,
    })
    await service.record_result(
        db_session, "uplink_CB77A4", command.command_id, begin, now=12.5,
    )
    resumed = await service.next_for_device(
        db_session, "uplink_CB77A4", now=12.75,
    )
    assert resumed.next_sequence == 1
    assert resumed.result_state == "queued"
    ack = await service.record_result(
        db_session, "uplink_CB77A4", command.command_id, terminal, now=13.0,
    )
    assert ack.duplicate is False
    assert ack.model_dump() == {
        "ok": True,
        "command_id": command.command_id,
        "accepted_sequence": 1,
        "next_sequence": 2,
        "result_state": "complete",
        "terminal": True,
        "duplicate": False,
    }
    assert await service.next_for_device(db_session, "uplink_CB77A4", now=14.0) is None


@pytest.mark.asyncio
async def test_repeated_identical_result_is_idempotent_and_conflict_fails(db_session):
    service = NodeCommandService()
    command = await service.enqueue_ble_investigation(
        db_session, "uplink_CB77A4", investigate(), now=10.0,
    )
    begin = RESULT.validate_python({
        "sequence": 0, "type": "ble_inv_begin", "request_id": command.command_id,
        "mode": "gatt", "target_mac": "AA:BB:CC:DD:EE:FF",
    })
    terminal = RESULT.validate_python({
        "sequence": 1, "type": "ble_inv_end", "request_id": command.command_id,
        "state": "failed", "summary": "", "error": "authentication_required",
        "authentication_required": True, "truncated": False,
    })
    await service.record_result(
        db_session, "uplink_CB77A4", command.command_id, begin, now=10.5,
    )
    ack = await service.record_result(
        db_session, "uplink_CB77A4", command.command_id, terminal, now=11.0,
    )
    assert ack.duplicate is False
    duplicate_ack = await service.record_result(
        db_session, "uplink_CB77A4", command.command_id, terminal, now=12.0,
    )
    assert duplicate_ack.duplicate is True
    with pytest.raises(NodeCommandConflict):
        await service.record_result(
            db_session, "uplink_CB77A4", command.command_id,
            RESULT.validate_python({
                "sequence": 1, "type": "ble_inv_end",
                "request_id": command.command_id, "state": "complete",
                "summary": "different", "error": None,
                "authentication_required": False, "truncated": False,
            }),
            now=13.0,
        )


def test_gatt_requires_target_and_passive_forbids_target():
    assert BleInvestigationCreateRequest(
        mode="passive_capture", target_mac=None,
    ).target_mac is None
    assert BleInvestigationCreateRequest(
        mode="gatt", target_mac="aa:bb:cc:dd:ee:ff",
    ).target_mac == "AA:BB:CC:DD:EE:FF"
    with pytest.raises(ValueError):
        BleInvestigationCreateRequest(mode="gatt", target_mac=None)
    with pytest.raises(ValueError):
        BleInvestigationCreateRequest(
            mode="passive_capture", target_mac="AA:BB:CC:DD:EE:FF",
        )
    with pytest.raises(ValueError):
        RESULT.validate_python({
            "sequence": 0, "type": "ble_inv_begin", "request_id": "a" * 32,
            "mode": "passive_capture", "target_mac": "AA:BB:CC:DD:EE:FF",
        })
    with pytest.raises(ValueError):
        RESULT.validate_python({
            "sequence": 1, "type": "ble_inv_char", "request_id": "a" * 32,
            "index": 0, "service_uuid": "180f", "uuid": "2a19",
            "properties": ["read", "read"],
        })


@pytest.mark.parametrize("payload", [
    {"sequence": 0, "type": "ble_inv_begin", "request_id": "a" * 32,
     "mode": "passive_capture", "target_mac": None},
    {"sequence": 1, "type": "ble_inv_progress", "request_id": "a" * 32,
     "state": "scanning"},
    {"sequence": 2, "type": "ble_inv_service", "request_id": "a" * 32,
     "index": 0, "uuid": "180f"},
    {"sequence": 3, "type": "ble_inv_char", "request_id": "a" * 32,
     "index": 0, "service_uuid": "180f", "uuid": "2a19",
     "properties": ["read", "write_without_response"]},
    {"sequence": 4, "type": "ble_inv_read", "request_id": "a" * 32,
     "index": 0, "uuid": "2a19", "value_hex": "64"},
    {"sequence": 5, "type": "ble_inv_end", "request_id": "a" * 32,
     "state": "complete", "summary": "done", "error": None,
     "authentication_required": False, "truncated": False},
])
def test_each_c_wire_chunk_shape_validates_without_field_translation(payload):
    assert RESULT.validate_python(payload).type == payload["type"]


@pytest.mark.asyncio
async def test_transition_rejects_terminal_first_index_skip_and_state_regression(db_session):
    service = NodeCommandService()
    command = await service.enqueue_ble_investigation(
        db_session, "uplink_CB77A4", investigate(), now=10.0,
    )
    with pytest.raises(NodeCommandConflict, match="begin"):
        await service.record_result(
            db_session, "uplink_CB77A4", command.command_id,
            RESULT.validate_python({
                "sequence": 0, "type": "ble_inv_end",
                "request_id": command.command_id, "state": "failed",
                "summary": "", "error": "no begin",
                "authentication_required": False, "truncated": False,
            }), now=11.0,
        )
    await service.record_result(
        db_session, "uplink_CB77A4", command.command_id,
        RESULT.validate_python({
            "sequence": 0, "type": "ble_inv_begin",
            "request_id": command.command_id, "mode": "gatt",
            "target_mac": "AA:BB:CC:DD:EE:FF",
        }), now=12.0,
    )
    with pytest.raises(NodeCommandConflict, match="service index"):
        await service.record_result(
            db_session, "uplink_CB77A4", command.command_id,
            RESULT.validate_python({
                "sequence": 1, "type": "ble_inv_service",
                "request_id": command.command_id, "index": 1, "uuid": "180f",
            }), now=13.0,
        )
    await service.record_result(
        db_session, "uplink_CB77A4", command.command_id,
        RESULT.validate_python({
            "sequence": 1, "type": "ble_inv_progress",
            "request_id": command.command_id, "state": "discovering",
        }), now=14.0,
    )
    with pytest.raises(NodeCommandConflict, match="state regression"):
        await service.record_result(
            db_session, "uplink_CB77A4", command.command_id,
            RESULT.validate_python({
                "sequence": 2, "type": "ble_inv_progress",
                "request_id": command.command_id, "state": "scanning",
            }), now=15.0,
        )


@pytest.mark.asyncio
async def test_cancel_history_and_single_active_command_survive_new_service_instance(db_session):
    service = NodeCommandService()
    command = await service.enqueue_ble_investigation(
        db_session, "uplink_CB77A4", investigate(), now=10.0,
    )
    with pytest.raises(NodeCommandConflict, match="active command"):
        await service.enqueue_ble_investigation(
            db_session, "uplink_CB77A4", investigate(), now=10.5,
        )

    restarted = NodeCommandService()
    cancel = await restarted.request_cancel(
        db_session, "uplink_CB77A4", command.command_id, now=11.0,
    )
    assert cancel.model_dump() == {
        "command_id": command.command_id,
        "type": "ble_investigate_cancel",
        "request_id": command.command_id,
        "mode": "gatt",
        "target": "AA:BB:CC:DD:EE:FF",
        "timeout_ms": 12000,
        "next_sequence": 0,
        "result_state": None,
    }
    assert await restarted.next_for_device(
        db_session, "uplink_CB77A4", now=11.5,
    ) == cancel
    pending_history = await restarted.history_for_device(
        db_session, "uplink_CB77A4", command.command_id,
    )
    assert pending_history.state == "cancel_pending"
    assert pending_history.events == []
    with pytest.raises(NodeCommandNotFound):
        await restarted.history_for_device(
            db_session, "different-node", command.command_id,
        )

    await restarted.record_result(
        db_session, "uplink_CB77A4", command.command_id,
        RESULT.validate_python({
            "sequence": 0, "type": "ble_inv_begin",
            "request_id": command.command_id, "mode": "gatt",
            "target_mac": "AA:BB:CC:DD:EE:FF",
        }), now=12.0,
    )
    await restarted.record_result(
        db_session, "uplink_CB77A4", command.command_id,
        RESULT.validate_python({
            "sequence": 1, "type": "ble_inv_end",
            "request_id": command.command_id, "state": "cancelled",
            "summary": "cancelled", "error": None,
            "authentication_required": False, "truncated": False,
        }), now=13.0,
    )
    history = await restarted.history_for_device(
        db_session, "uplink_CB77A4", command.command_id,
    )
    assert history.state == "terminal"
    assert history.terminal is True
    assert [event.sequence for event in history.events] == [0, 1]
    replacement = await restarted.enqueue_ble_investigation(
        db_session, "uplink_CB77A4", investigate(), now=14.0,
    )
    assert replacement.command_id != command.command_id


@pytest.mark.asyncio
async def test_concurrent_identical_result_is_one_insert_plus_duplicate(
    backend_sensor_session_factory,
):
    service = NodeCommandService()
    async with backend_sensor_session_factory() as setup:
        command = await service.enqueue_ble_investigation(
            setup, "uplink_CB77A4", investigate(), now=10.0,
        )
    begin = RESULT.validate_python({
        "sequence": 0, "type": "ble_inv_begin",
        "request_id": command.command_id, "mode": "gatt",
        "target_mac": "AA:BB:CC:DD:EE:FF",
    })

    ready = 0
    ready_lock = asyncio.Lock()
    both_ready = asyncio.Event()
    release = asyncio.Event()

    async def submit():
        nonlocal ready
        async with backend_sensor_session_factory() as session:
            async with ready_lock:
                ready += 1
                if ready == 2:
                    both_ready.set()
            await release.wait()
            return await service.record_result(
                session, "uplink_CB77A4", command.command_id, begin, now=11.0,
            )

    first = asyncio.create_task(submit())
    second = asyncio.create_task(submit())
    await both_ready.wait()
    release.set()
    a, b = await asyncio.gather(first, second)
    assert sorted([a.duplicate, b.duplicate]) == [False, True]
    async with backend_sensor_session_factory() as verification:
        events = list((await verification.scalars(
            select(NodeCommandResultEvent).where(
                NodeCommandResultEvent.command_id == command.command_id,
            )
        )).all())
        stored = await verification.get(NodeCommand, command.command_id)
    assert len(events) == 1
    assert stored.next_sequence == 1


@pytest.mark.asyncio
async def test_concurrent_different_result_bodies_are_one_ack_one_conflict(
    backend_sensor_session_factory,
):
    service = NodeCommandService()
    async with backend_sensor_session_factory() as setup:
        command = await service.enqueue_ble_investigation(
            setup, "uplink_CB77A4", investigate(), now=20.0,
        )
        begin = RESULT.validate_python({
            "sequence": 0, "type": "ble_inv_begin",
            "request_id": command.command_id, "mode": "gatt",
            "target_mac": "AA:BB:CC:DD:EE:FF",
        })
        await service.record_result(
            setup, "uplink_CB77A4", command.command_id, begin, now=21.0,
        )

    progress = [
        RESULT.validate_python({
            "sequence": 1, "type": "ble_inv_progress",
            "request_id": command.command_id, "state": state,
        })
        for state in ("scanning", "connecting")
    ]
    ready = 0
    ready_lock = asyncio.Lock()
    both_ready = asyncio.Event()
    release = asyncio.Event()

    async def submit(body):
        nonlocal ready
        async with backend_sensor_session_factory() as session:
            async with ready_lock:
                ready += 1
                if ready == 2:
                    both_ready.set()
            await release.wait()
            return await service.record_result(
                session, "uplink_CB77A4", command.command_id, body, now=22.0,
            )

    tasks = [asyncio.create_task(submit(body)) for body in progress]
    await both_ready.wait()
    release.set()
    outcomes = await asyncio.gather(*tasks, return_exceptions=True)
    acks = [item for item in outcomes if not isinstance(item, BaseException)]
    conflicts = [item for item in outcomes if isinstance(item, NodeCommandConflict)]
    assert len(acks) == 1
    assert acks[0].accepted_sequence == 1
    assert acks[0].duplicate is False
    assert len(conflicts) == 1
    assert str(conflicts[0]) == "sequence body differs"

    async with backend_sensor_session_factory() as verification:
        sequence_one = list((await verification.scalars(
            select(NodeCommandResultEvent).where(
                NodeCommandResultEvent.command_id == command.command_id,
                NodeCommandResultEvent.sequence == 1,
            )
        )).all())
        stored = await verification.get(NodeCommand, command.command_id)
    assert len(sequence_one) == 1
    assert stored.next_sequence == 2
    assert stored.result_state in {"scanning", "connecting"}


@pytest.mark.asyncio
async def test_concurrent_poll_does_not_overwrite_a_cancel(
    backend_sensor_session_factory, monkeypatch,
):
    service = NodeCommandService()
    async with backend_sensor_session_factory() as setup:
        command = await service.enqueue_ble_investigation(
            setup, "uplink_CB77A4", investigate(), now=30.0,
        )

    first_started, release_first, second_started, release_second = (
        gate_command_transactions(monkeypatch)
    )
    async with backend_sensor_session_factory() as poll_session, \
            backend_sensor_session_factory() as cancel_session:
        poll = asyncio.create_task(service.next_for_device(
            poll_session, "uplink_CB77A4", now=31.0,
        ))
        await first_started.wait()
        cancel = asyncio.create_task(service.request_cancel(
            cancel_session, "uplink_CB77A4", command.command_id, now=32.0,
        ))
        await second_started.wait()
        release_second.set()
        release_first.set()
        poll_envelope, cancel_envelope = await asyncio.gather(poll, cancel)

    assert poll_envelope.type == "ble_investigate"
    assert cancel_envelope.type == "ble_investigate_cancel"
    async with backend_sensor_session_factory() as verification:
        stored = await verification.get(NodeCommand, command.command_id)
    assert stored.state == "cancel_pending"
    assert stored.active_key == "uplink_CB77A4"


@pytest.mark.asyncio
async def test_concurrent_poll_does_not_overwrite_a_terminal_result(
    backend_sensor_session_factory, monkeypatch,
):
    service = NodeCommandService()
    async with backend_sensor_session_factory() as setup:
        command = await service.enqueue_ble_investigation(
            setup, "uplink_CB77A4", investigate(), now=35.0,
        )
        await service.record_result(
            setup,
            "uplink_CB77A4",
            command.command_id,
            RESULT.validate_python({
                "sequence": 0,
                "type": "ble_inv_begin",
                "request_id": command.command_id,
                "mode": "gatt",
                "target_mac": "AA:BB:CC:DD:EE:FF",
            }),
            now=36.0,
        )

    terminal = RESULT.validate_python({
        "sequence": 1,
        "type": "ble_inv_end",
        "request_id": command.command_id,
        "state": "complete",
        "summary": "done",
        "error": None,
        "authentication_required": False,
        "truncated": False,
    })
    first_started, release_first, second_started, release_second = (
        gate_command_transactions(monkeypatch)
    )
    async with backend_sensor_session_factory() as poll_session, \
            backend_sensor_session_factory() as result_session:
        poll = asyncio.create_task(service.next_for_device(
            poll_session, "uplink_CB77A4", now=37.0,
        ))
        await first_started.wait()
        record = asyncio.create_task(service.record_result(
            result_session,
            "uplink_CB77A4",
            command.command_id,
            terminal,
            now=38.0,
        ))
        await second_started.wait()
        release_second.set()
        release_first.set()
        poll_envelope, ack = await asyncio.gather(poll, record)

    assert poll_envelope.type == "ble_investigate"
    assert ack.terminal is True
    async with backend_sensor_session_factory() as verification:
        stored = await verification.get(NodeCommand, command.command_id)
    assert stored.state == "terminal"
    assert stored.active_key is None


@pytest.mark.asyncio
async def test_concurrent_cancel_does_not_overwrite_a_terminal_result(
    backend_sensor_session_factory, monkeypatch,
):
    service = NodeCommandService()
    async with backend_sensor_session_factory() as setup:
        command = await service.enqueue_ble_investigation(
            setup, "uplink_CB77A4", investigate(), now=40.0,
        )
        await service.record_result(
            setup,
            "uplink_CB77A4",
            command.command_id,
            RESULT.validate_python({
                "sequence": 0,
                "type": "ble_inv_begin",
                "request_id": command.command_id,
                "mode": "gatt",
                "target_mac": "AA:BB:CC:DD:EE:FF",
            }),
            now=41.0,
        )

    terminal = RESULT.validate_python({
        "sequence": 1,
        "type": "ble_inv_end",
        "request_id": command.command_id,
        "state": "complete",
        "summary": "done",
        "error": None,
        "authentication_required": False,
        "truncated": False,
    })
    first_started, release_first, second_started, release_second = (
        gate_command_transactions(monkeypatch)
    )
    async with backend_sensor_session_factory() as cancel_session, \
            backend_sensor_session_factory() as result_session:
        cancel = asyncio.create_task(service.request_cancel(
            cancel_session, "uplink_CB77A4", command.command_id, now=42.0,
        ))
        await first_started.wait()
        record = asyncio.create_task(service.record_result(
            result_session,
            "uplink_CB77A4",
            command.command_id,
            terminal,
            now=43.0,
        ))
        await second_started.wait()
        release_second.set()
        release_first.set()
        cancel_envelope, ack = await asyncio.gather(cancel, record)

    assert cancel_envelope.type == "ble_investigate_cancel"
    assert ack.terminal is True
    async with backend_sensor_session_factory() as verification:
        stored = await verification.get(NodeCommand, command.command_id)
    assert stored.state == "terminal"
    assert stored.active_key is None
