# Backend Sensor API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend the existing FastAPI service so backend sensor firmware can upload its complete evidence and health contract, discover only backend firmware artifacts, and execute idempotent pull-based BLE investigations.

**Architecture:** Keep `POST /detections/drones` backward compatible and extend its Pydantic and persistence models additively. Separate transport acknowledgment from downstream filtering, extract heartbeat merging into a pure service, and persist the BLE command lifecycle with unique sequence constraints so router retries remain idempotent across process restarts.

**Tech Stack:** Python 3.12, FastAPI, Pydantic v2, SQLAlchemy async sessions, httpx `AsyncClient`/`ASGITransport`, pytest, pytest-asyncio.

## Global Constraints

- The primary ingest API remains `POST /detections/drones`; existing production and badge payloads must continue to validate.
- The only new firmware targets are `uplink-s3-backend` and `scanner-s3-combo-backend`.
- Their ESP app projects are `fof_backend_uplink` and `fof_backend_scanner`; hardware is `seeed_xiao_esp32s3`.
- The first backend firmware release is `0.1.0-backend` for both targets. `_EXPECTED_BACKEND_VERSION` continues to mean the FastAPI service release and must not be repurposed as a firmware version.
- If backend firmware is ever tagged, its reserved tag is
  `backend-fw-<version>` (initially `backend-fw-0.1.0-backend`), never `v*`.
  This plan does not publish a GitHub Release; the existing badge release-event
  workflow makes that unsafe until separately redesigned.
- Operational `device_id` remains the existing NVS value or legacy `uplink_XXXXXX` identity; target and hardware metadata never replace it.
- Empty detection arrays remain valid 60-second heartbeats.
- Credentials, Wi-Fi passwords, and AP passwords must never be returned by an API.
- The first release is trusted-LAN HTTP; adding TLS, enrollment, or fleet credentials is outside this plan.
- No existing files under `esp32/uplink/`, `esp32/scanner/`, `esp32/shared/`, `esp32/web-flasher/`, or badge scripts may be edited.
- Use test-driven development and commit after every task.

Before every task commit, run all three local protected-path checks below. All
must print no paths; do not commit until an accidental protected edit or
untracked file has been removed safely:

```bash
git diff --name-only -- esp32/uplink esp32/scanner esp32/shared esp32/web-flasher scripts
git diff --cached --name-only -- esp32/uplink esp32/scanner esp32/shared esp32/web-flasher scripts
git ls-files --others --exclude-standard -- esp32/uplink esp32/scanner esp32/shared esp32/web-flasher scripts
```

## Plan Order and Dependencies

This is plan 1 of 3. Its schema and command contracts are consumed by the firmware plan. The release plan later generates `backend/tests/fixtures/backend_firmware_detection_batch.json` with the real C serializer and runs it through this API.

## File Map

- `backend/app/models/schemas.py`: additive ingest, command, and result models.
- `backend/app/services/backend_node_status.py`: pure heartbeat timestamp and sticky-field merge policy.
- `backend/app/services/applied_calibration.py`: stable read-only, per-device calibration-continuity digest from the already-persisted applied model.
- `backend/app/services/node_commands.py`: persistent per-node command state and idempotent result recording.
- `backend/app/models/db_models.py`: new evidence columns and command/result tables.
- `backend/app/services/database.py`: additive SQLite/PostgreSQL column reconciliation.
- `backend/app/routers/detections.py`: use server receive time for liveness and expose backend telemetry.
- `backend/app/routers/nodes.py`: command endpoints and backend artifact metadata.
- `backend/app/services/firmware_manager.py`: backend-only catalog entries and identity metadata.
- `backend/tests/test_backend_firmware_ingest.py`: schema, heartbeat, liveness, and registry continuity tests.
- `backend/tests/test_backend_node_commands.py`: enqueue, poll, cancel, progress, and idempotency tests.
- `backend/tests/test_firmware_catalog.py`: backend target paths and exact identity metadata.
- `backend/tests/test_firmware_auto_endpoints.py`: embedded custom-image version and content-derived SHA/ETag tests.
- `backend/tests/test_scanner_ota_relay_paths.py`: named and direct OTA family-gating tests.
- `backend/tests/conftest.py`: isolated temporary database/client fixtures; tests never connect to the configured application database.

---

### Task 1: Preserve the Complete Backend-Firmware Ingest Envelope

**Files:**
- Modify: `backend/app/models/schemas.py`
- Modify: `backend/app/routers/detections.py`
- Create: `backend/tests/test_backend_firmware_ingest.py`
- Modify: `backend/tests/conftest.py`

**Interfaces:**
- Consumes: existing `DroneDetectionItem`, `DroneDetectionBatch`, and `DroneDetectionResponse`.
- Produces: additive item fields matching `drone_detection_t`; batch fields `firmware_target`, `app_project`, `hardware_type`, `hardware_mac`, `capabilities`, `node_name`, `led_state`, `upload_queue`, and `upload`.

- [ ] **Step 1: Write the failing item and batch schema tests**

First replace the one-line `backend/tests/conftest.py` with the isolated test
database and shared client below. The file-backed database is required for the
SQLite two-session race test in Task 4; it is still temporary and never opens
`settings.database_url`:

```python
import pytest_asyncio
from httpx import ASGITransport, AsyncClient
from sqlalchemy import event
from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker, create_async_engine

from app.main import app
from app.routers import detections
from app.services.database import Base, get_db


@pytest_asyncio.fixture
async def backend_sensor_session_factory(tmp_path):
    test_engine = create_async_engine(
        f"sqlite+aiosqlite:///{tmp_path / 'backend-sensor-test.db'}",
        connect_args={"timeout": 30},
    )

    @event.listens_for(test_engine.sync_engine, "connect")
    def _configure_sqlite(dbapi_connection, _connection_record):
        cursor = dbapi_connection.cursor()
        try:
            cursor.execute("PRAGMA journal_mode=WAL")
            cursor.execute("PRAGMA synchronous=NORMAL")
            cursor.execute("PRAGMA busy_timeout=30000")
            cursor.execute("PRAGMA foreign_keys=ON")
        finally:
            cursor.close()

    factory = async_sessionmaker(
        test_engine, class_=AsyncSession, expire_on_commit=False,
    )
    async with test_engine.begin() as connection:
        await connection.run_sync(Base.metadata.create_all)
    try:
        yield factory
    finally:
        await test_engine.dispose()


@pytest_asyncio.fixture
async def db_session(backend_sensor_session_factory):
    async with backend_sensor_session_factory() as session:
        yield session


@pytest_asyncio.fixture
async def client(backend_sensor_session_factory):
    async def isolated_get_db():
        async with backend_sensor_session_factory() as session:
            yield session

    previous_overrides = dict(app.dependency_overrides)
    state_maps = (
        detections._node_heartbeats,
        detections._recent_detections,
        detections._position_dedup,
        detections._ingest_dedup,
        detections._bssid_to_ap,
    )
    for state in state_maps:
        state.clear()
    app.dependency_overrides[get_db] = isolated_get_db
    try:
        transport = ASGITransport(app=app)
        async with AsyncClient(transport=transport, base_url="http://test") as ac:
            yield ac
    finally:
        app.dependency_overrides.clear()
        app.dependency_overrides.update(previous_overrides)
        for state in state_maps:
            state.clear()
```

Then create `test_backend_firmware_ingest.py` with these schema tests:

```python
from app.models.schemas import DroneDetectionBatch, DroneDetectionItem


PORTABLE_EVIDENCE = {
    "drone_id": "RID:backend-test",
    "source": "wifi_beacon_rid",
    "confidence": 0.71,
    "fused_confidence": 0.93,
    "timestamp": 1785600000123,
    "vertical_speed_mps": -1.25,
    "freq_mhz": 2437,
    "channel": 6,
    "channel_width_mhz": 20,
    "ua_type": 2,
    "id_type": 1,
    "self_id_desc_type": 0,
    "height_agl_m": 37.5,
    "geodetic_alt_m": 151.25,
    "h_accuracy_m": 3.0,
    "v_accuracy_m": 5.0,
    "area_count": 4,
    "area_radius": 20,
    "area_ceiling": 250.0,
    "area_floor": 10.0,
    "classification_type": 1,
    "first_seen_ms": 1785600000000,
    "last_updated_ms": 1785600000123,
    "wifi_generation": 6,
    "auth_m": 0,
    "ie_hash": "00abcdef",
    "ble_ja3": "0123abcd",
    "ble_apple_auth": "00ff7a",
    "ble_activity": 0,
    "ble_apple_flags": 0,
    "ble_raw_mfr": "4c001007000000000000",
    "ble_adv_interval": 125.5,
    "ble_svc_uuids": "fd5f,180f,12345678-1234-5678-9abc-def012345678",
    "ble_threat_kind": 1,
    "ble_prompt_family_mask": 3,
    "ble_unique_macs": 9,
    "ble_observation_count": 14,
    "ble_serial_service_uuid": 0xFFE0,
    "ble_threat_evidence_mask": 5,
    "scanner_slot": 1,
    "scanner_slots_seen": 3,
}


def test_backend_detection_preserves_every_ported_evidence_field():
    item = DroneDetectionItem.model_validate(PORTABLE_EVIDENCE)
    dumped = item.model_dump()
    for key, expected in PORTABLE_EVIDENCE.items():
        assert dumped[key] == expected
    # Frequency is the measured MHz value; channel is independently derived.
    assert dumped["freq_mhz"] == 2437
    assert dumped["channel"] == 6
    assert dumped["freq_mhz"] != dumped["channel"]
    # Zero is real evidence for these firmware enums/flags, never "absent".
    assert dumped["auth_m"] == 0
    assert dumped["ble_activity"] == 0
    assert dumped["ble_apple_flags"] == 0


def test_backend_batch_preserves_identity_health_and_queue_metadata():
    payload = {
        "device_id": "uplink_CB77A4",
        "firmware_version": "0.1.0-backend",
        "firmware_target": "uplink-s3-backend",
        "app_project": "fof_backend_uplink",
        "hardware_type": "seeed_xiao_esp32s3",
        "hardware_mac": "A4:CF:12:CB:77:A4",
        "capabilities": ["dual_scanner", "ble_investigation", "uart_ota"],
        "node_name": "Roof backend sensor",
        "scanners": [{
            "uart": "ble", "slot": 0,
            "firmware_target": "scanner-s3-combo-backend",
            "app_project": "fof_backend_scanner",
            "hardware_type": "seeed_xiao_esp32s3",
            "firmware_version": "0.1.0-backend",
            "mac": "AA:BB:CC:DD:EE:01", "boot_id": 305419896,
            "profile": "ble_primary", "status_sequence": 12,
            "role_generation": 4, "role_acked": True,
            "command_ingress": True, "radio_healthy": True,
            "ble_healthy": True, "wifi_healthy": False,
            "ota_state": "idle", "rollback_state": "valid",
        }],
        "led_state": "drone_meta",
        "upload_queue": {"depth_batches": 7, "capacity_batches": 512, "overflow_dropped_batches": 2, "quarantined_batches": 1},
        "upload": {"ok": 11, "failed": 3, "retry_count": 4, "last_success_age_s": 8},
        "detections": [PORTABLE_EVIDENCE],
    }
    batch = DroneDetectionBatch.model_validate(payload)
    assert batch.model_dump()["firmware_target"] == "uplink-s3-backend"
    assert batch.model_dump()["upload_queue"]["capacity_batches"] == 512
    assert batch.model_dump()["detections"][0]["fused_confidence"] == 0.93
    scanner = batch.model_dump()["scanners"][0]
    assert scanner["uart"] == "ble"
    assert scanner["firmware_target"] == "scanner-s3-combo-backend"
    assert scanner["app_project"] == "fof_backend_scanner"
    assert scanner["hardware_type"] == "seeed_xiao_esp32s3"
    assert scanner["boot_id"] == 305419896
    assert scanner["rollback_state"] == "valid"


@pytest.mark.asyncio
async def test_primary_db_outage_returns_503_without_consuming_retry(
    client, backend_sensor_session_factory, monkeypatch,
):
    drone_id = f"BLE:{uuid.uuid4().hex}"
    now = int(time.time())
    body = {
        "device_id": "uplink_CB77A4",
        "timestamp": now,
        "detections": [{
            "drone_id": drone_id,
            "source": "ble_fingerprint",
            "confidence": 0.8,
            "bssid": "AA:BB:CC:DD:EE:FF",
        }],
    }
    normal_override = app.dependency_overrides[get_db]
    async with backend_sensor_session_factory() as failing_session:
        async def fail_commit():
            raise RuntimeError("forced primary commit failure")

        monkeypatch.setattr(failing_session, "commit", fail_commit)

        async def failing_get_db():
            yield failing_session

        app.dependency_overrides[get_db] = failing_get_db
        try:
            first = await client.post("/detections/drones", json=body)
        finally:
            app.dependency_overrides[get_db] = normal_override

    assert first.status_code == 503
    assert not any(key[0] == drone_id for key in detections._ingest_dedup)

    retry = await client.post("/detections/drones", json=body)
    assert retry.status_code == 200
    assert retry.json()["processed"] == 1
    async with backend_sensor_session_factory() as verification_session:
        count = await verification_session.scalar(
            select(func.count(DroneDetection.id)).where(
                DroneDetection.drone_id == drone_id,
            )
        )
    assert count == 1
```

Import `time`, `uuid`, `pytest`, `func`, and `select`, plus `app`,
`detections`, `DroneDetection`, and `get_db` in this test module. The outage
test is deliberately non-RID so ingest dedup participates.

- [ ] **Step 2: Run the tests and verify the new fields fail**

Run: `cd backend && pytest tests/test_backend_firmware_ingest.py -q`

Expected: FAIL because the model dumps do not contain the new keys and the
current route acknowledges the forced primary commit failure.

- [ ] **Step 3: Add the exact additive fields**

Add these declarations to `DroneDetectionItem`; retain `extra="ignore"` and all existing validators:

```python
    fused_confidence: float | None = Field(None, ge=0.0, le=1.0)
    vertical_speed_mps: float | None = None
    freq_mhz: int | None = Field(
        None,
        validation_alias=AliasChoices("freq_mhz", "frequency_mhz"),
    )
    channel_width_mhz: int | None = None
    ua_type: int | None = Field(None, ge=0, le=255)
    id_type: int | None = Field(None, ge=0, le=255)
    self_id_desc_type: int | None = Field(None, ge=0, le=255)
    height_agl_m: float | None = None
    geodetic_alt_m: float | None = None
    h_accuracy_m: float | None = Field(None, ge=0.0)
    v_accuracy_m: float | None = Field(None, ge=0.0)
    area_count: int | None = Field(None, ge=0, le=65535)
    area_radius: int | None = Field(None, ge=0, le=65535)
    area_ceiling: float | None = None
    area_floor: float | None = None
    classification_type: int | None = Field(None, ge=0, le=255)
    first_seen_ms: int | None = None
    last_updated_ms: int | None = None
    wifi_generation: int | None = Field(None, ge=0, le=6)
    auth_m: int | None = Field(None, ge=0, le=10)
    ie_hash: str | None = Field(None, pattern=r"^[0-9A-Fa-f]{8}$")
    ble_ja3: str | None = Field(None, pattern=r"^[0-9A-Fa-f]{8}$")
    ble_apple_auth: str | None = Field(None, pattern=r"^[0-9A-Fa-f]{6}$")
    ble_activity: int | None = Field(None, ge=0, le=255)
    ble_apple_flags: int | None = Field(None, ge=0, le=255)
    ble_raw_mfr: str | None = Field(
        None, pattern=r"^(?:[0-9A-Fa-f]{2}){1,20}$",
    )
    ble_adv_interval: float | None = Field(None, gt=0, allow_inf_nan=False)
    ble_svc_uuids: str | None = Field(None, max_length=160)
    ble_threat_kind: int | None = Field(None, ge=0, le=255)
    ble_prompt_family_mask: int | None = Field(None, ge=0, le=255)
    ble_unique_macs: int | None = Field(None, ge=0, le=65535)
    ble_observation_count: int | None = Field(None, ge=0, le=65535)
    ble_serial_service_uuid: int | None = Field(None, ge=0, le=65535)
    ble_threat_evidence_mask: int | None = Field(None, ge=0, le=255)
```

Some declarations above already exist in the legacy model. Replace those
loose declarations in place instead of adding duplicates. Add one `mode="after"`
field validator for `ie_hash`, `ble_ja3`, `ble_apple_auth`, and
`ble_raw_mfr` that returns lowercase. Add a separate `ble_svc_uuids`
validator that splits on commas, rejects whitespace/empty tokens, permits one
through six tokens, accepts only four hex digits or a canonical 36-character
UUID per token, and rejoins normalized lowercase tokens in the original order.
This matches the firmware limit of four 16-bit plus two 128-bit UUIDs. Existing
uppercase legacy input remains accepted and normalizes to the same canonical
output. The fixture deliberately exercises leading-zero hashes/auth data,
zero-valued enums, a fractional advertisement interval, and mixed 16/128-bit
UUIDs so Pydantic's retained `extra="ignore"` cannot silently discard any
firmware field.

Do not add a derived `threat_class` field. Canary selection uses persisted,
portable evidence instead of a second classification vocabulary: a drone row
has `source` in `{"ble_rid", "wifi_dji_ie", "wifi_beacon_rid"}`; a Meta row
has `source == "ble_fingerprint"`, `manufacturer == "Meta Glasses"`, token
`"fd5f"` in canonical `ble_svc_uuids`, and the canary's recorded test-device
MAC as `bssid`. Keep those predicates identical in the release/canary plan.

Add these typed telemetry models before `DroneDetectionBatch`. `reporting`
remains the legacy reporter status object, `upload_queue` describes bounded
FIFO storage, and `upload` describes only the backend HTTP worker; do not copy
the same counter into more than one object:

```python
class BackendUploadQueueTelemetry(BaseModel):
    model_config = ConfigDict(extra="forbid")

    depth_batches: int = Field(ge=0, le=512)
    capacity_batches: int = Field(512, ge=1, le=512)
    overflow_dropped_batches: int = Field(ge=0)
    quarantined_batches: int = Field(ge=0)


class BackendUploadTelemetry(BaseModel):
    model_config = ConfigDict(extra="forbid")

    ok: int = Field(ge=0)
    failed: int = Field(ge=0)
    retry_count: int = Field(ge=0)
    last_success_age_s: int | None = Field(None, ge=0)
```

Add these declarations to `DroneDetectionBatch`:

```python
    firmware_target: str | None = Field(
        None,
        validation_alias=AliasChoices("firmware_target", "firmware_name"),
    )
    app_project: str | None = None
    hardware_type: str | None = None
    hardware_mac: str | None = Field(
        None,
        validation_alias=AliasChoices("hardware_mac", "hardware_id"),
        pattern=r"^[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}$",
    )
    capabilities: list[Annotated[str, Field(max_length=40)]] | None = Field(
        None, max_length=16,
    )
    node_name: str | None = Field(None, max_length=64)
    led_state: Literal[
        "healthy", "network_degraded", "drone", "meta",
        "drone_meta", "fatal", "uart_lost",
    ] | None = None
    upload_queue: BackendUploadQueueTelemetry | None = None
    upload: BackendUploadTelemetry | None = None
```

`firmware_target` and `hardware_mac` are the canonical output keys. The input
aliases keep the existing `firmware_name`/`hardware_id` serializer compatible
without allowing either value to replace operational `device_id`.

Keep `channel` as the Wi-Fi channel and use `freq_mhz` separately; accept
`frequency_mhz` as an input alias but do not map either frequency key onto
`channel`. Import `Annotated` from `typing` and `AliasChoices` from Pydantic if
they are not already present.

Extend `DroneDetectionResponse` so `accepted` means syntactically accepted
transport items, while downstream outcomes remain observable:

```python
class DroneDetectionResponse(BaseModel):
    status: str = "ok"
    accepted: int
    device_id: str
    processed: int = 0
    deduplicated: int = 0
    filtered: int = 0
```

In `ingest_drone_detections`, rename the existing downstream `accepted`
counter to `processed` and return:

```python
return DroneDetectionResponse(
    status="ok",
    accepted=len(batch.detections),
    processed=processed,
    deduplicated=dedup_skipped,
    filtered=max(0, len(batch.detections) - processed - dedup_skipped),
    device_id=batch.device_id,
)
```

This prevents a valid retry or locally filtered observation from remaining at
the head of a firmware FIFO forever. Empty heartbeats still return
`accepted == 0`.

Make primary persistence part of transport acceptance for nonempty batches.
Replace mutating `_ingest_dedup_hit` with these three helpers:

```python
IngestDedupKey = tuple[str, str, str, int]


def _ingest_dedup_key(
    drone_id: str, source: str, bssid: str, ts: float,
) -> IngestDedupKey | None:
    if source in _NEVER_DEDUP_SOURCES:
        return None
    return (
        drone_id or "",
        source or "",
        (bssid or "").upper(),
        int(ts // 10),
    )


def _ingest_dedup_seen(key: IngestDedupKey, ts: float) -> bool:
    previous = _ingest_dedup.get(key)
    return previous is not None and (ts - previous) < _INGEST_DEDUP_TTL_S


def _publish_ingest_dedup(keys: dict[IngestDedupKey, float], ts: float) -> None:
    _ingest_dedup.update(keys)
    if len(_ingest_dedup) <= _INGEST_DEDUP_MAX:
        return
    cutoff = ts - _INGEST_DEDUP_TTL_S
    for key, seen_at in list(_ingest_dedup.items()):
        if seen_at < cutoff:
            del _ingest_dedup[key]
```

At loop entry create `staged_dedup: dict[IngestDedupKey, float] = {}`. For
each non-calibration item, derive its key and count it as duplicate when the
key is already staged or `_ingest_dedup_seen(key, received_at)` is true;
otherwise stage it without mutating the global map. Commit the
`DroneDetection` and `TriangulatedPosition` rows in their existing primary
transaction, then call `_publish_ingest_dedup(staged_dedup, received_at)` only
after `await db.commit()` succeeds:

```python
try:
    db.add_all(db_detections)
    await db.commit()
except Exception as exc:
    await db.rollback()
    logger.warning("Primary detection persistence failed: %s", exc)
    raise HTTPException(
        status_code=503,
        detail="detection persistence unavailable",
    ) from exc
else:
    _publish_ingest_dedup(staged_dedup, received_at)
```

Do not catch this 503 in an outer best-effort block. Optional entity/event
checkpoints remain best-effort and run only after the primary commit. Empty
heartbeats and batches containing only already-deduplicated or locally
filtered observations may return 200 without creating a detection row.

- [ ] **Step 4: Run the focused schema tests**

Run: `cd backend && pytest tests/test_backend_firmware_ingest.py -q`

Expected: PASS.

- [ ] **Step 5: Run legacy API coverage**

Run: `cd backend && pytest tests/test_api.py tests/test_ingest_reporting_smoke.py tests/test_detection_alignment.py -q`

Expected: PASS, proving the additive model did not reject legacy payloads.

- [ ] **Step 6: Commit**

```bash
git add backend/app/models/schemas.py backend/app/routers/detections.py backend/tests/conftest.py backend/tests/test_backend_firmware_ingest.py
git commit -m "backend: accept complete backend sensor evidence"
```

---

### Task 2: Persist the Ported Evidence Fields

**Files:**
- Modify: `backend/app/models/db_models.py`
- Modify: `backend/app/services/database.py`
- Modify: `backend/app/models/schemas.py`
- Modify: `backend/app/routers/detections.py`
- Modify: `backend/app/routers/nodes.py`
- Modify: `backend/tests/test_backend_firmware_ingest.py`

**Interfaces:**
- Consumes: the validated item fields from Task 1.
- Produces: nullable `DroneDetection` columns and matching `DetectionHistoryItem` fields so restart-safe history retains the backend-firmware evidence.

- [ ] **Step 1: Write the failing persistence test**

Post `PORTABLE_EVIDENCE` with a unique `drone_id`, request
`/detections/drones/history?hours=1&limit=200`, and assert each of these keys
round-trips exactly:

```python
PERSISTED_BACKEND_FIELDS = (
    "fused_confidence", "vertical_speed_mps", "freq_mhz", "channel",
    "channel_width_mhz", "ua_type", "id_type", "self_id_desc_type",
    "height_agl_m", "geodetic_alt_m", "h_accuracy_m", "v_accuracy_m",
    "area_count", "area_radius", "area_ceiling", "area_floor",
    "classification_type", "first_seen_ms", "last_updated_ms",
    "wifi_generation", "auth_m", "ie_hash", "scanner_slot", "scanner_slots_seen",
    "ble_ja3", "ble_apple_auth", "ble_activity", "ble_apple_flags",
    "ble_raw_mfr", "ble_adv_interval", "ble_svc_uuids",
    "ble_threat_kind", "ble_prompt_family_mask", "ble_unique_macs",
    "ble_observation_count", "ble_serial_service_uuid",
    "ble_threat_evidence_mask",
)


@pytest.mark.asyncio
async def test_backend_evidence_survives_history_persistence(client):
    drone_id = f"RID-BACKEND-{uuid.uuid4().hex}"
    item = {**PORTABLE_EVIDENCE, "drone_id": drone_id}
    response = await client.post("/detections/drones", json={
        "device_id": "uplink_CB77A4",
        "timestamp": int(time.time()),
        "detections": [item],
    })
    assert response.status_code == 200
    history = await client.get("/detections/drones/history", params={"hours": 1, "limit": 200})
    row = next(entry for entry in history.json()["detections"] if entry["drone_id"] == drone_id)
    for field in PERSISTED_BACKEND_FIELDS:
        assert row[field] == item[field]

    node_history = await client.get("/nodes/uplink_CB77A4/detections")
    node_row = next(
        entry for entry in node_history.json()["detections"]
        if entry["drone_id"] == drone_id
    )
    for field in PERSISTED_BACKEND_FIELDS:
        assert node_row[field] == item[field]
```

- [ ] **Step 2: Run the test and observe missing history fields**

Run: `cd backend && pytest tests/test_backend_firmware_ingest.py::test_backend_evidence_survives_history_persistence -q`

Expected: FAIL because `DetectionHistoryItem` and `DroneDetection` do not yet carry the fields.

- [ ] **Step 3: Add nullable ORM columns and additive reconciliation**

Import `BigInteger` in `db_models.py`. Add nullable mapped columns to
`DroneDetection` with these SQLAlchemy types:

```python
    vertical_speed_mps: Mapped[float | None] = mapped_column(Float, nullable=True)
    freq_mhz: Mapped[int | None] = mapped_column(Integer, nullable=True)
    channel: Mapped[int | None] = mapped_column(Integer, nullable=True)
    channel_width_mhz: Mapped[int | None] = mapped_column(Integer, nullable=True)
    ua_type: Mapped[int | None] = mapped_column(Integer, nullable=True)
    id_type: Mapped[int | None] = mapped_column(Integer, nullable=True)
    self_id_desc_type: Mapped[int | None] = mapped_column(Integer, nullable=True)
    height_agl_m: Mapped[float | None] = mapped_column(Float, nullable=True)
    geodetic_alt_m: Mapped[float | None] = mapped_column(Float, nullable=True)
    h_accuracy_m: Mapped[float | None] = mapped_column(Float, nullable=True)
    v_accuracy_m: Mapped[float | None] = mapped_column(Float, nullable=True)
    area_count: Mapped[int | None] = mapped_column(Integer, nullable=True)
    area_radius: Mapped[int | None] = mapped_column(Integer, nullable=True)
    area_ceiling: Mapped[float | None] = mapped_column(Float, nullable=True)
    area_floor: Mapped[float | None] = mapped_column(Float, nullable=True)
    classification_type: Mapped[int | None] = mapped_column(Integer, nullable=True)
    first_seen_ms: Mapped[int | None] = mapped_column(BigInteger, nullable=True)
    last_updated_ms: Mapped[int | None] = mapped_column(BigInteger, nullable=True)
    wifi_generation: Mapped[int | None] = mapped_column(Integer, nullable=True)
    auth_m: Mapped[int | None] = mapped_column(Integer, nullable=True)
    ie_hash: Mapped[str | None] = mapped_column(String(8), nullable=True)
    ble_ja3: Mapped[str | None] = mapped_column(String(8), nullable=True)
    ble_apple_auth: Mapped[str | None] = mapped_column(String(6), nullable=True)
    ble_activity: Mapped[int | None] = mapped_column(Integer, nullable=True)
    ble_apple_flags: Mapped[int | None] = mapped_column(Integer, nullable=True)
    ble_raw_mfr: Mapped[str | None] = mapped_column(String(40), nullable=True)
    ble_adv_interval: Mapped[float | None] = mapped_column(Float, nullable=True)
    ble_svc_uuids: Mapped[str | None] = mapped_column(String(160), nullable=True)
    scanner_slot: Mapped[int | None] = mapped_column(Integer, nullable=True)
    scanner_slots_seen: Mapped[int | None] = mapped_column(Integer, nullable=True)
    ble_threat_kind: Mapped[int | None] = mapped_column(Integer, nullable=True)
    ble_prompt_family_mask: Mapped[int | None] = mapped_column(Integer, nullable=True)
    ble_unique_macs: Mapped[int | None] = mapped_column(Integer, nullable=True)
    ble_observation_count: Mapped[int | None] = mapped_column(Integer, nullable=True)
    ble_serial_service_uuid: Mapped[int | None] = mapped_column(Integer, nullable=True)
    ble_threat_evidence_mask: Mapped[int | None] = mapped_column(Integer, nullable=True)
```

`fused_confidence` already exists in the ORM; populate it rather than adding a
duplicate, and add it to `_ensure_detection_columns` because upgraded databases
may predate that ORM declaration. Add every new column to the reconciliation
map using `FLOAT`, `INTEGER`, or `BIGINT` to keep existing SQLite/PostgreSQL
installations additive.

The reconciliation map must include these exact additional entries as well;
do not rely on `create_all()` to alter a deployed table:

```python
"auth_m": "INTEGER",
"ie_hash": "VARCHAR(8)",
"ble_ja3": "VARCHAR(8)",
"ble_apple_auth": "VARCHAR(6)",
"ble_activity": "INTEGER",
"ble_apple_flags": "INTEGER",
"ble_raw_mfr": "VARCHAR(40)",
"ble_adv_interval": "FLOAT",
"ble_svc_uuids": "VARCHAR(160)",
```

- [ ] **Step 4: Populate and return every column**

In the `DroneDetection(...)` constructor, map each column directly from
`det`. Add the same fields to `DetectionHistoryItem`, the history constructor
at the bottom of `detections.py`, and the independent
`GET /nodes/{device_id}/detections` mapping in `nodes.py`. The durable primary
row and both history APIs must preserve `ble_apple_auth` exactly; it is a
rotating RF correlation tag, not a Wi-Fi credential. Keep the existing
in-memory operator-feed redaction and never write this raw field to application
logs, canary JSONL, or release artifacts. Canary tooling may assert presence
without recording its value.

Representative constructor lines:

```python
fused_confidence=det.fused_confidence,
vertical_speed_mps=det.vertical_speed_mps,
freq_mhz=det.freq_mhz,
channel=det.channel,
channel_width_mhz=det.channel_width_mhz,
ua_type=det.ua_type,
id_type=det.id_type,
self_id_desc_type=det.self_id_desc_type,
height_agl_m=det.height_agl_m,
geodetic_alt_m=det.geodetic_alt_m,
h_accuracy_m=det.h_accuracy_m,
v_accuracy_m=det.v_accuracy_m,
area_count=det.area_count,
area_radius=det.area_radius,
area_ceiling=det.area_ceiling,
area_floor=det.area_floor,
classification_type=det.classification_type,
first_seen_ms=det.first_seen_ms,
last_updated_ms=det.last_updated_ms,
wifi_generation=det.wifi_generation,
auth_m=det.auth_m,
ie_hash=det.ie_hash,
ble_ja3=det.ble_ja3,
ble_apple_auth=det.ble_apple_auth,
ble_activity=det.ble_activity,
ble_apple_flags=det.ble_apple_flags,
ble_raw_mfr=det.ble_raw_mfr,
ble_adv_interval=det.ble_adv_interval,
ble_svc_uuids=det.ble_svc_uuids,
scanner_slot=det.scanner_slot,
scanner_slots_seen=det.scanner_slots_seen,
ble_threat_kind=det.ble_threat_kind,
ble_prompt_family_mask=det.ble_prompt_family_mask,
ble_unique_macs=det.ble_unique_macs,
ble_observation_count=det.ble_observation_count,
ble_serial_service_uuid=det.ble_serial_service_uuid,
ble_threat_evidence_mask=det.ble_threat_evidence_mask,
```

Add these exact fields to `DetectionHistoryItem` (beside the rest of the Task 2
fields), then use the same explicit one-to-one mapping in both history
constructors; no generated or computed substitute may overwrite scanner
evidence:

```python
auth_m: int | None = None
ie_hash: str | None = None
ble_ja3: str | None = None
ble_apple_auth: str | None = None
ble_activity: int | None = None
ble_apple_flags: int | None = None
ble_raw_mfr: str | None = None
ble_adv_interval: float | None = None
ble_svc_uuids: str | None = None
```

The history test's single full fixture and loop over
`PERSISTED_BACKEND_FIELDS` is the required assertion at both routes. Do not
replace it with key-presence-only checks: it must prove `freq_mhz == 2437` and
derived `channel == 6` survive as distinct values, `auth_m`, `ble_activity`,
and `ble_apple_flags` survive as zero, and every hex/UUID field round-trips in
its canonical lowercase form.

- [ ] **Step 5: Run persistence and legacy history tests**

Run: `cd backend && pytest tests/test_backend_firmware_ingest.py tests/test_api.py tests/test_detection_alignment.py -q`

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add backend/app/models/db_models.py backend/app/services/database.py backend/app/models/schemas.py backend/app/routers/detections.py backend/app/routers/nodes.py backend/tests/test_backend_firmware_ingest.py
git commit -m "backend: persist backend sensor evidence"
```

---

### Task 3: Make Heartbeat Liveness Server-Timed and Preserve Node Identity

**Files:**
- Create: `backend/app/services/backend_node_status.py`
- Modify: `backend/app/services/applied_calibration.py`
- Modify: `backend/app/models/schemas.py`
- Modify: `backend/app/routers/detections.py`
- Modify: `backend/tests/test_backend_firmware_ingest.py`

**Interfaces:**
- Consumes: `DroneDetectionBatch` and the previous `_node_heartbeats[device_id]` dictionary.
- Produces: `merge_backend_heartbeat(previous: dict, batch: DroneDetectionBatch, source_ip: str | None, server_received_at: float) -> dict`, `bounded_observation_time(batch_timestamp: int | None, server_received_at: float) -> tuple[float, float | None]`, `bounded_detection_time(detection_timestamp_ms: int | None, batch_observed_at: float, server_received_at: float) -> float`, `AppliedCalibrationStore.calibration_continuity(device_id: str) -> dict`, and read-only `GET /detections/calibrate/continuity/{device_id}`.

- [ ] **Step 1: Write failing pure-policy tests**

```python
from app.models.schemas import DroneDetectionBatch
from app.services.backend_node_status import (
    bounded_detection_time,
    bounded_observation_time,
    merge_backend_heartbeat,
)


def test_future_clock_never_extends_online_liveness():
    observation_time, skew = bounded_observation_time(2_000_000_000, 1_785_600_000.0)
    assert observation_time == 1_785_600_000.0
    assert skew == 214_400_000.0


def test_offline_replay_preserves_valid_historical_scan_time():
    observation_time, skew = bounded_observation_time(
        1_784_000_000, 1_785_600_000.0,
    )
    assert observation_time == 1_784_000_000.0
    assert skew == -1_600_000.0


def test_future_detection_timestamp_falls_back_to_batch_observation():
    assert bounded_detection_time(
        2_000_000_000_000, 1_784_000_000.0, 1_785_600_000.0,
    ) == 1_784_000_000.0


def test_backend_heartbeat_keeps_operational_device_id_and_sticky_metadata():
    previous = {
        "device_id": "uplink_CB77A4", "total_batches": 4,
        "node_name": "Roof", "lat": 36.1, "lon": -115.1, "alt": 700.0,
    }
    batch = DroneDetectionBatch(
        device_id="uplink_CB77A4",
        firmware_target="uplink-s3-backend",
        hardware_mac="A4:CF:12:CB:77:A4",
        detections=[],
    )
    merged = merge_backend_heartbeat(previous, batch, "10.0.0.15", 1000.0)
    assert merged["device_id"] == "uplink_CB77A4"
    assert merged["last_seen"] == 1000.0
    assert merged["total_batches"] == 5
    assert merged["node_name"] == "Roof"
    assert (merged["lat"], merged["lon"], merged["alt"]) == (36.1, -115.1, 700.0)
    assert merged["firmware_target"] == "uplink-s3-backend"
    assert merged["hardware_mac"] == "A4:CF:12:CB:77:A4"
```

- [ ] **Step 2: Run the tests and verify the service is missing**

Run: `cd backend && pytest tests/test_backend_firmware_ingest.py -q`

Expected: FAIL with `ModuleNotFoundError: app.services.backend_node_status`.

- [ ] **Step 3: Implement the pure heartbeat policy**

```python
from app.models.schemas import DroneDetectionBatch

MAX_TRUSTED_CLOCK_SKEW_S = 300.0

STICKY_BATCH_FIELDS = (
    "firmware_version", "board_type", "firmware_target", "app_project",
    "hardware_type", "hardware_mac", "capabilities", "node_name",
    "scanners", "time_sync", "reporting", "scan_mode", "scan_profile",
    "calibration_uuid", "dedup_seen", "dedup_sent", "dedup_collapsed",
    "cal_seen", "cal_sent", "wifi_ssid", "wifi_rssi", "led_state",
    "upload_queue", "upload",
)


def bounded_observation_time(
    batch_timestamp: int | None,
    server_received_at: float,
) -> tuple[float, float | None]:
    if batch_timestamp is None:
        return server_received_at, None
    skew = float(batch_timestamp) - server_received_at
    if (
        batch_timestamp <= 1_700_000_000
        or skew > MAX_TRUSTED_CLOCK_SKEW_S
    ):
        return server_received_at, skew
    return float(batch_timestamp), skew


def bounded_detection_time(
    detection_timestamp_ms: int | None,
    batch_observed_at: float,
    server_received_at: float,
) -> float:
    if detection_timestamp_ms is None or detection_timestamp_ms <= 1_700_000_000_000:
        return batch_observed_at
    observed = detection_timestamp_ms / 1000.0
    if observed > server_received_at + MAX_TRUSTED_CLOCK_SKEW_S:
        return batch_observed_at
    return observed


def merge_backend_heartbeat(
    previous: dict,
    batch: DroneDetectionBatch,
    source_ip: str | None,
    server_received_at: float,
) -> dict:
    merged = dict(previous)
    merged.update({
        "device_id": batch.device_id,
        "last_seen": server_received_at,
        "detection_count": len(batch.detections),
        "total_batches": int(previous.get("total_batches", 0)) + 1,
        "total_detections": int(previous.get("total_detections", 0)) + len(batch.detections),
        "ip": source_ip,
    })
    for key, value in (
        ("lat", batch.device_lat),
        ("lon", batch.device_lon),
        ("alt", batch.device_alt),
    ):
        if value is not None:
            merged[key] = value
    _, skew = bounded_observation_time(batch.timestamp, server_received_at)
    merged["clock_skew_s"] = skew
    for field in STICKY_BATCH_FIELDS:
        value = getattr(batch, field)
        if value is not None:
            merged[field] = value
    return merged
```

- [ ] **Step 4: Wire it into ingest without changing fixed-node resolution**

At the start of `ingest_drone_detections`, use separate server and observation times:

```python
    server_received_at = time.time()
    received_at, _ = bounded_observation_time(batch.timestamp, server_received_at)
    source_ip = request.client.host if request.client else None
    prev_hb = _node_heartbeats.get(batch.device_id, {})
    _node_heartbeats[batch.device_id] = merge_backend_heartbeat(
        prev_hb, batch, source_ip, server_received_at,
    )
```

Delete only the old inline `_node_heartbeats[batch.device_id] = {...}` block. Leave `_resolve_sensor_position` unchanged so registered fixed coordinates, calibration, and history stay bound to the same `device_id`.

Inside the detection loop, replace the direct per-item timestamp expression
with `detection_observed_at = bounded_detection_time(
det.timestamp, received_at, server_received_at)` and persist
`timestamp=int(detection_observed_at)` in that `DroneDetection` row. A missing
or invalid item timestamp therefore falls back to the validated batch
observation (which itself falls back to server receipt), so the non-null DB
column never receives `None`. A valid historical per-item timestamp remains
historical; only invalid/pre-epoch and more-than-five-minute future item values
fall back.

- [ ] **Step 5: Add the fixed-location continuity API test**

```python
@pytest.mark.asyncio
async def test_backend_upgrade_heartbeat_does_not_replace_registered_location(client):
    device_id = f"uplink_{uuid.uuid4().hex[:6].upper()}"
    registered = await client.post("/nodes", json={
        "device_id": device_id,
        "name": "Roof",
        "lat": 36.1,
        "lon": -115.1,
        "alt": 700.0,
        "position_mode": "active",
    })
    assert registered.status_code == 201
    try:
        response = await client.post("/detections/drones", json={
            "device_id": device_id,
            "device_lat": 1.0,
            "device_lon": 2.0,
            "node_name": "Untrusted over-air rename",
            "firmware_target": "uplink-s3-backend",
            "detections": [],
        })
        assert response.status_code == 200
        status = await client.get("/detections/nodes/status")
        node = next(row for row in status.json()["nodes"] if row["device_id"] == device_id)
        assert (node["lat"], node["lon"], node["name"]) == (36.1, -115.1, "Roof")
    finally:
        await client.delete(f"/nodes/{device_id}")


@pytest.mark.asyncio
async def test_backend_upgrade_keeps_history_and_listener_calibration_on_device_id(client):
    device_id = f"uplink_{uuid.uuid4().hex[:6].upper()}"
    drone_id = f"BLE:{uuid.uuid4().hex}"
    prior_model = triangulation.PER_LISTENER_MODEL.get(device_id)
    triangulation.PER_LISTENER_MODEL[device_id] = (-58.0, 2.1)
    try:
        created = await client.post("/nodes", json={
            "device_id": device_id,
            "name": "Existing roof node",
            "lat": 36.1,
            "lon": -115.1,
            "alt": 700.0,
            "position_mode": "active",
        })
        assert created.status_code == 201
        uploaded = await client.post("/detections/drones", json={
            "device_id": device_id,
            "firmware_version": "0.1.0-backend",
            "firmware_target": "uplink-s3-backend",
            "app_project": "fof_backend_uplink",
            "hardware_type": "seeed_xiao_esp32s3",
            "hardware_mac": "A4:CF:12:CB:77:A4",
            "detections": [{
                "drone_id": drone_id,
                "source": "ble_fingerprint",
                "confidence": 0.8,
                "bssid": "AA:BB:CC:DD:EE:FF",
            }],
        })
        assert uploaded.status_code == 200

        registry = (await client.get("/nodes")).json()
        matching = [n for n in registry["nodes"] if n["device_id"] == device_id]
        assert len(matching) == 1
        assert matching[0]["name"] == "Existing roof node"
        history = (await client.get(f"/nodes/{device_id}/detections")).json()
        assert any(
            row["device_id"] == device_id and row["drone_id"] == drone_id
            for row in history["detections"]
        )
        assert triangulation.PER_LISTENER_MODEL[device_id] == (-58.0, 2.1)
        assert "A4:CF:12:CB:77:A4" not in {
            node["device_id"] for node in registry["nodes"]
        }
    finally:
        await client.delete(f"/nodes/{device_id}")
        if prior_model is None:
            triangulation.PER_LISTENER_MODEL.pop(device_id, None)
        else:
            triangulation.PER_LISTENER_MODEL[device_id] = prior_model


@pytest.mark.asyncio
async def test_legacy_batch_without_timestamp_persists_non_null_observation_time(
    client, backend_sensor_session_factory,
):
    drone_id = f"BLE:{uuid.uuid4().hex}"
    before = int(time.time())
    response = await client.post("/detections/drones", json={
        "device_id": "uplink_CB77A4",
        "detections": [{
            "drone_id": drone_id,
            "source": "ble_fingerprint",
            "confidence": 0.8,
            "bssid": "AA:BB:CC:DD:EE:FF",
        }],
    })
    assert response.status_code == 200
    async with backend_sensor_session_factory() as session:
        stored = await session.scalar(
            select(DroneDetection).where(DroneDetection.drone_id == drone_id)
        )
    assert stored is not None
    assert before <= stored.timestamp <= int(time.time())


@pytest.mark.asyncio
async def test_valid_historical_item_timestamp_is_persisted_not_batch_receipt(
    client, backend_sensor_session_factory,
):
    drone_id = f"BLE:{uuid.uuid4().hex}"
    historical_ms = 1_784_000_000_123
    response = await client.post("/detections/drones", json={
        "device_id": "uplink_CB77A4",
        "timestamp": 1_785_600_000,
        "detections": [{
            "drone_id": drone_id,
            "source": "ble_fingerprint",
            "confidence": 0.8,
            "bssid": "AA:BB:CC:DD:EE:FF",
            "timestamp": historical_ms,
        }],
    })
    assert response.status_code == 200
    async with backend_sensor_session_factory() as session:
        stored = await session.scalar(
            select(DroneDetection).where(DroneDetection.drone_id == drone_id)
        )
    assert stored is not None
    assert stored.timestamp == historical_ms // 1000
```

Import `uuid`, `select`, `DroneDetection`, and
`app.services.triangulation` in the test module.

Add this exact auto-registration/name-immutability regression. It covers the
only place where an ingest-provided name may enter the registry: creation of a
previously unknown valid-GPS node. A later heartbeat may update live status but
must not rename the persistent row:

```python
@pytest.mark.asyncio
async def test_ingest_node_name_is_creation_only_and_never_renames_registry(
    client, backend_sensor_session_factory,
):
    device_id = f"uplink_{uuid.uuid4().hex[:6].upper()}"
    first = await client.post("/detections/drones", json={
        "device_id": device_id,
        "device_lat": 36.11,
        "device_lon": -115.11,
        "node_name": "Roof backend sensor",
        "detections": [],
    })
    assert first.status_code == 200
    async with backend_sensor_session_factory() as session:
        created = await session.scalar(
            select(SensorNode).where(SensorNode.device_id == device_id)
        )
        assert created is not None
        assert created.name == "Roof backend sensor"

    later = await client.post("/detections/drones", json={
        "device_id": device_id,
        "device_lat": 36.12,
        "device_lon": -115.12,
        "node_name": "Changed over radio",
        "detections": [],
    })
    assert later.status_code == 200
    async with backend_sensor_session_factory() as session:
        preserved = await session.scalar(
            select(SensorNode).where(SensorNode.device_id == device_id)
        )
        assert preserved is not None
        assert preserved.name == "Roof backend sensor"
```

In the existing auto-registration block, set
`name=batch.node_name or batch.device_id` only while constructing a new
`SensorNode`. The `existing_node is not None` branch must never assign
`existing_node.name` from heartbeat data. Import `SensorNode` in the test
module; the earlier fixed-node test's hostile `node_name` proves the same rule
for manually registered fixed nodes.

- [ ] **Step 6: Add a durable read-only calibration-continuity view**

The current applied-calibration data model already supports device-bound
continuity: `AppliedCalibrationStore.save_verified_model()` durably stores
`session_id`, `applied_at`, and `per_listener_model[device_id]` in
`applied_calibration.json`, and startup reloads that same record. Do not add a
second database mapping or derive continuity from the transient
`triangulation.PER_LISTENER_MODEL` dictionary. Add this response model to
`schemas.py`:

```python
class CalibrationContinuityResponse(BaseModel):
    model_config = ConfigDict(extra="forbid")

    schema: Literal[1] = 1
    device_id: str = Field(min_length=1, max_length=64)
    calibration_status: Literal["defaults", "trusted", "untrusted"]
    session_id: str | None = Field(None, max_length=64)
    applied_at: float | None = None
    listener_model_present: bool
    listener_model_schema: Literal["rssi-ref-path-loss-v1"] = (
        "rssi-ref-path-loss-v1"
    )
    listener_model_sha256: str | None = Field(
        None, pattern=r"^[0-9a-f]{64}$",
    )
```

Import `hashlib` and `math` in `applied_calibration.py` and add this method. The
digest binds the exact preserved `device_id` to normalized finite double values
without returning either calibration coefficient:

```python
def calibration_continuity(self, device_id: str) -> dict[str, Any]:
    if not device_id or len(device_id) > 64:
        raise ValueError("invalid device_id")
    if self.record is None:
        return {
            "schema": 1,
            "device_id": device_id,
            "calibration_status": "defaults",
            "session_id": None,
            "applied_at": None,
            "listener_model_present": False,
            "listener_model_schema": "rssi-ref-path-loss-v1",
            "listener_model_sha256": None,
        }

    models = self.record.get("per_listener_model") or {}
    if not isinstance(models, dict):
        raise ValueError("invalid per_listener_model mapping")
    present = device_id in models
    digest = None
    if present:
        value = models[device_id]
        if (
            not isinstance(value, (list, tuple))
            or len(value) != 2
            or any(isinstance(part, bool) or not isinstance(part, (int, float))
                   for part in value)
        ):
            raise ValueError("invalid listener model")
        normalized = [float(value[0]), float(value[1])]
        if not all(math.isfinite(part) for part in normalized):
            raise ValueError("invalid listener model")
        canonical = json.dumps(
            {
                "device_id": device_id,
                "model_schema": "rssi-ref-path-loss-v1",
                "values": normalized,
            },
            sort_keys=True,
            separators=(",", ":"),
            allow_nan=False,
        ).encode("utf-8")
        digest = hashlib.sha256(canonical).hexdigest()

    session_id = self.record.get("session_id")
    if session_id is not None and (
        not isinstance(session_id, str) or not session_id or len(session_id) > 64
    ):
        raise ValueError("invalid calibration session_id")
    applied_at = self.record.get("applied_at")
    if applied_at is not None:
        if isinstance(applied_at, bool) or not isinstance(applied_at, (int, float)):
            raise ValueError("invalid calibration applied_at")
        applied_at = float(applied_at)
        if not math.isfinite(applied_at):
            raise ValueError("invalid calibration applied_at")
    trusted, _ = _record_trust(self.record)
    return {
        "schema": 1,
        "device_id": device_id,
        "calibration_status": "trusted" if trusted else "untrusted",
        "session_id": session_id,
        "applied_at": applied_at,
        "listener_model_present": present,
        "listener_model_schema": "rssi-ref-path-loss-v1",
        "listener_model_sha256": digest,
    }
```

Add this exact route to `detections.py`; its full public path is
`GET /detections/calibrate/continuity/{device_id}`. Requiring a registered row
prevents a mistyped ID from producing a plausible all-defaults continuity
receipt:

```python
@router.get(
    "/calibrate/continuity/{device_id}",
    response_model=CalibrationContinuityResponse,
)
async def calibration_continuity(
    device_id: str,
    db: AsyncSession = Depends(get_db),
):
    registered = await db.scalar(
        select(SensorNode.device_id).where(SensorNode.device_id == device_id)
    )
    if registered is None:
        raise HTTPException(status_code=404, detail="sensor node not found")
    try:
        return _applied_cal_store.calibration_continuity(device_id)
    except ValueError as exc:
        raise HTTPException(
            status_code=500, detail="invalid applied calibration state",
        ) from exc
```

Write the failing API test before the method/route. It independently computes
the canonical digest, proves an identity-preserving firmware heartbeat cannot
change the receipt, and proves no raw coefficient or fit structure escapes:

```python
@pytest.mark.asyncio
async def test_calibration_continuity_is_device_bound_stable_and_nonsecret(
    client, monkeypatch,
):
    device_id = f"uplink_{uuid.uuid4().hex[:6].upper()}"
    created = await client.post("/nodes", json={
        "device_id": device_id,
        "name": "Calibrated roof",
        "lat": 36.1,
        "lon": -115.1,
        "alt": 700.0,
        "position_mode": "active",
    })
    assert created.status_code == 201
    monkeypatch.setattr(detections._applied_cal_store, "record", {
        "session_id": "0123456789ab",
        "applied_at": 1_785_600_000.25,
        "per_listener_model": {
            device_id: [-58.0, 2.1],
            "uplink_OTHER1": [-61.0, 2.4],
        },
        "verified_fit": {
            "model_validation": {"applyable": True, "reasons": []},
            "private_raw_samples": ["must-not-escape"],
        },
        "provisional_fit": {"raw": "must-not-escape"},
    })
    canonical = json.dumps(
        {
            "device_id": device_id,
            "model_schema": "rssi-ref-path-loss-v1",
            "values": [-58.0, 2.1],
        },
        sort_keys=True,
        separators=(",", ":"),
        allow_nan=False,
    ).encode("utf-8")
    expected = {
        "schema": 1,
        "device_id": device_id,
        "calibration_status": "trusted",
        "session_id": "0123456789ab",
        "applied_at": 1_785_600_000.25,
        "listener_model_present": True,
        "listener_model_schema": "rssi-ref-path-loss-v1",
        "listener_model_sha256": hashlib.sha256(canonical).hexdigest(),
    }

    before = await client.get(
        f"/detections/calibrate/continuity/{device_id}"
    )
    assert before.status_code == 200
    assert before.json() == expected
    heartbeat = await client.post("/detections/drones", json={
        "device_id": device_id,
        "firmware_target": "uplink-s3-backend",
        "app_project": "fof_backend_uplink",
        "hardware_type": "seeed_xiao_esp32s3",
        "hardware_mac": "A4:CF:12:CB:77:A4",
        "node_name": "Must not rename calibrated node",
        "detections": [],
    })
    assert heartbeat.status_code == 200
    after = await client.get(
        f"/detections/calibrate/continuity/{device_id}"
    )
    assert after.status_code == 200
    assert after.json() == before.json()
    serialized = json.dumps(after.json(), sort_keys=True)
    for forbidden in (
        "rssi_ref", "path_loss", "-58.0", "2.1", "verified_fit",
        "provisional_fit", "must-not-escape",
    ):
        assert forbidden not in serialized

    unknown = await client.get(
        "/detections/calibrate/continuity/uplink_UNKNOWN"
    )
    assert unknown.status_code == 404
```

Add pure cases for `record is None` and for a trusted record whose mapping lacks
the requested device: both return `listener_model_present=False` and a null
digest, while the latter still returns its nonsecret session/status. Add
parameterized malformed-value cases `[]`, `[float("nan"), 2.1]`,
`[True, 2.1]`, and `[-58.0, "2.1"]`; each raises `ValueError`, and the route
turns that into the fixed 500 detail without returning record contents. The
canary captures this endpoint before and after migration for the preserved
`device_id` and requires exact equality of session, status, presence, schema,
and digest. A legitimate recalibration between captures is therefore an
explicit canary blocker rather than an unexplained continuity pass.

- [ ] **Step 7: Run heartbeat, position, and continuity tests**

Run: `cd backend && pytest tests/test_backend_firmware_ingest.py tests/test_node_position_mode.py tests/test_time_sync_status.py -q`

Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add backend/app/services/backend_node_status.py backend/app/services/applied_calibration.py backend/app/models/schemas.py backend/app/routers/detections.py backend/tests/test_backend_firmware_ingest.py
git commit -m "backend: track backend sensor heartbeat health"
```

---

### Task 4: Define the Persistent BLE Investigation Command Lifecycle

**Files:**
- Modify: `backend/app/models/schemas.py`
- Modify: `backend/app/models/db_models.py`
- Create: `backend/app/services/node_commands.py`
- Create: `backend/tests/test_backend_node_commands.py`

**Interfaces:**
- Consumes: trusted-LAN operator command creation, an `AsyncSession`, and firmware polling/result retries.
- Produces: `BleInvestigationCreateRequest`, `NodeCommandResultRequest`, `NodeCommandHistoryResponse`, `NodeCommand`, `NodeCommandResultEvent`, and async `NodeCommandService` methods.

- [ ] **Step 1: Write failing lifecycle tests**

```python
import pytest

from pydantic import TypeAdapter

from app.models.schemas import BleInvestigationCreateRequest, NodeCommandResultRequest
from app.services.node_commands import NodeCommandConflict, NodeCommandService


RESULT = TypeAdapter(NodeCommandResultRequest)


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
    with pytest.raises(ValueError):
        BleInvestigationCreateRequest(mode="gatt", target_mac=None)
    with pytest.raises(ValueError):
        BleInvestigationCreateRequest(
            mode="passive_capture", target_mac="AA:BB:CC:DD:EE:FF",
        )


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
```

- [ ] **Step 2: Run the tests and verify the command types are absent**

Run: `cd backend && pytest tests/test_backend_node_commands.py -q`

Expected: FAIL on missing imports.

- [ ] **Step 3: Add strict command models**

```python
class BleInvestigationCreateRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    target_mac: str | None = Field(
        None, pattern=r"^[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}$",
    )
    mode: Literal["gatt", "passive_capture"] = "gatt"
    timeout_ms: int = Field(12000, ge=1, le=12000)

    @model_validator(mode="after")
    def target_matches_mode(self):
        if self.mode == "gatt" and self.target_mac is None:
            raise ValueError("gatt requires target_mac")
        if self.mode == "passive_capture" and self.target_mac is not None:
            raise ValueError("passive_capture forbids target_mac")
        if self.target_mac is not None:
            self.target_mac = self.target_mac.upper()
        return self


class BleInvEventBase(BaseModel):
    model_config = ConfigDict(extra="forbid")

    sequence: int = Field(ge=0)
    request_id: str = Field(pattern=r"^[0-9a-f]{32}$")


class BleInvBeginEvent(BleInvEventBase):
    type: Literal["ble_inv_begin"]
    mode: Literal["gatt", "passive_capture"]
    target_mac: str | None = Field(
        None, pattern=r"^[0-9A-F]{2}(?::[0-9A-F]{2}){5}$",
    )


class BleInvProgressEvent(BleInvEventBase):
    type: Literal["ble_inv_progress"]
    state: Literal["queued", "scanning", "connecting", "discovering", "reading"]


BLE_UUID_PATTERN = (
    r"^(?i:[0-9a-f]{4}|[0-9a-f]{8}|"
    r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})$"
)


class BleInvServiceEvent(BleInvEventBase):
    type: Literal["ble_inv_service"]
    index: int = Field(ge=0, lt=16)
    uuid: str = Field(pattern=BLE_UUID_PATTERN, max_length=36)


class BleInvCharacteristicEvent(BleInvEventBase):
    type: Literal["ble_inv_char"]
    index: int = Field(ge=0, lt=32)
    service_uuid: str = Field(pattern=BLE_UUID_PATTERN, max_length=36)
    uuid: str = Field(pattern=BLE_UUID_PATTERN, max_length=36)
    properties: list[Literal[
        "broadcast", "read", "write_without_response", "write", "notify",
        "indicate", "authenticated_signed_writes", "extended_properties",
    ]] = Field(default_factory=list, max_length=8)


class BleInvReadEvent(BleInvEventBase):
    type: Literal["ble_inv_read"]
    index: int = Field(ge=0, lt=8)
    uuid: str = Field(pattern=BLE_UUID_PATTERN, max_length=36)
    value_hex: str = Field(max_length=128, pattern=r"^(?:[0-9A-Fa-f]{2})*$")


class BleInvEndEvent(BleInvEventBase):
    type: Literal["ble_inv_end"]
    state: Literal["complete", "failed", "cancelled"]
    summary: str = Field(max_length=127)
    error: str | None = Field(None, max_length=63)
    authentication_required: bool
    truncated: bool


NodeCommandResultRequest = Annotated[
    BleInvBeginEvent | BleInvProgressEvent | BleInvServiceEvent |
    BleInvCharacteristicEvent | BleInvReadEvent | BleInvEndEvent,
    Field(discriminator="type"),
]


class BleInvestigateCommandEnvelope(BaseModel):
    model_config = ConfigDict(extra="forbid")

    command_id: str = Field(pattern=r"^[0-9a-f]{32}$")
    type: Literal["ble_investigate"]
    request_id: str = Field(pattern=r"^[0-9a-f]{32}$")
    mode: Literal["gatt", "passive_capture"]
    target: str | None
    timeout_ms: int = Field(ge=1, le=12000)
    next_sequence: int = Field(ge=0)
    result_state: str | None


class BleInvestigateCancelEnvelope(BaseModel):
    model_config = ConfigDict(extra="forbid")

    command_id: str = Field(pattern=r"^[0-9a-f]{32}$")
    type: Literal["ble_investigate_cancel"]
    request_id: str = Field(pattern=r"^[0-9a-f]{32}$")
    mode: Literal["gatt", "passive_capture"]
    target: str | None
    timeout_ms: int = Field(ge=1, le=12000)
    next_sequence: int = Field(ge=0)
    result_state: str | None


NodeCommandEnvelope = Annotated[
    BleInvestigateCommandEnvelope | BleInvestigateCancelEnvelope,
    Field(discriminator="type"),
]


class NodeCommandResultAck(BaseModel):
    model_config = ConfigDict(extra="forbid")

    ok: Literal[True] = True
    command_id: str = Field(pattern=r"^[0-9a-f]{32}$")
    accepted_sequence: int = Field(ge=0)
    next_sequence: int = Field(ge=1)
    result_state: Literal[
        "queued", "scanning", "connecting", "discovering", "reading",
        "complete", "failed", "cancelled",
    ]
    terminal: bool
    duplicate: bool


class NodeCommandHistoryResponse(BaseModel):
    model_config = ConfigDict(extra="forbid")

    command_id: str = Field(pattern=r"^[0-9a-f]{32}$")
    device_id: str
    command_type: Literal["ble_investigate"]
    state: Literal["pending", "delivered", "cancel_pending", "terminal"]
    next_sequence: int = Field(ge=0)
    result_state: str | None
    terminal: bool
    events: list[NodeCommandResultRequest]
```

Import `Annotated` and `Literal` from `typing` and `model_validator` from
Pydantic. Add a `BleInvBeginEvent` validator with the same GATT/passive target
rule as command creation and a characteristic validator that rejects duplicate
property strings. These event models match the C producer field-for-field;
do not add aggregate service/read arrays or require `state` on evidence chunks.

- [ ] **Step 4: Add persistent command and result tables**

Add these models to `db_models.py`:

```python
class NodeCommand(Base):
    __tablename__ = "node_commands"

    command_id: Mapped[str] = mapped_column(String(32), primary_key=True)
    device_id: Mapped[str] = mapped_column(String(64), nullable=False, index=True)
    active_key: Mapped[str | None] = mapped_column(String(64), unique=True, nullable=True)
    command_type: Mapped[str] = mapped_column(String(32), nullable=False)
    payload_json: Mapped[str] = mapped_column(Text, nullable=False)
    state: Mapped[str] = mapped_column(String(24), nullable=False, default="pending")
    next_sequence: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    result_state: Mapped[str | None] = mapped_column(String(24), nullable=True)
    next_service_index: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    next_characteristic_index: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    next_read_index: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    first_delivered_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)
    last_polled_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)
    completed_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)


class NodeCommandResultEvent(Base):
    __tablename__ = "node_command_result_events"
    __table_args__ = (UniqueConstraint("command_id", "sequence"),)

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    command_id: Mapped[str] = mapped_column(
        ForeignKey("node_commands.command_id", ondelete="CASCADE"),
        nullable=False,
        index=True,
    )
    sequence: Mapped[int] = mapped_column(Integer, nullable=False)
    event_type: Mapped[str] = mapped_column(String(32), nullable=False)
    payload_json: Mapped[str] = mapped_column(Text, nullable=False)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
```

Name the result uniqueness constraint `uq_node_command_result_sequence`. The
nullable unique `active_key=device_id` permits only one unfinished
investigation per node on SQLite and PostgreSQL. `Base.metadata.create_all`
creates both new tables; no destructive migration is needed.

- [ ] **Step 5: Implement the async command service**

```python
class NodeCommandService:
    async def enqueue_ble_investigation(
        self, db: AsyncSession, device_id: str,
        request: BleInvestigationCreateRequest, *, now: float,
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
        return _command_envelope(row)

    async def next_for_device(
        self, db: AsyncSession, device_id: str, *, now: float,
    ) -> NodeCommandEnvelope | None:
        row = await _active_command(db, device_id)
        if row is None:
            return None
        stamp = _utc_from_epoch(now)
        row.first_delivered_at = row.first_delivered_at or stamp
        row.last_polled_at = stamp
        row.state = "delivered" if row.state == "pending" else row.state
        await db.commit()
        return _command_envelope(row)

    async def request_cancel(
        self, db: AsyncSession, device_id: str, command_id: str, *, now: float,
    ) -> NodeCommandEnvelope:
        row = await _command_for_update(db, device_id, command_id)
        if row is None or row.active_key is None:
            raise NodeCommandNotFound(command_id)
        row.state = "cancel_pending"
        row.last_polled_at = _utc_from_epoch(now)
        await db.commit()
        return _command_envelope(row)

    async def record_result(
        self, db: AsyncSession, device_id: str, command_id: str,
        result: NodeCommandResultRequest, *, now: float,
    ) -> NodeCommandResultAck:
        if db.bind is not None and db.bind.dialect.name == "sqlite":
            await db.execute(text("BEGIN IMMEDIATE"))
        row = await _command_for_update(db, device_id, command_id)
        if row is None:
            raise NodeCommandNotFound(command_id)
        canonical = _canonical_json(result.model_dump(mode="json"))
        prior = await _result_event(db, command_id, result.sequence)
        if prior is not None:
            if prior.payload_json != canonical:
                raise NodeCommandConflict("sequence body differs")
            return _result_ack(row, prior, duplicate=True)
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
            return _result_ack(row, prior, duplicate=True)
        _apply_result_transition(row, transition, completed_at=_utc_from_epoch(now))
        await db.commit()
        return _result_ack(row, event, duplicate=False)
```

Define `_canonical_json`, `_utc_from_epoch`, `_active_command`,
`_command_for_update`, `_result_event`, `_command_envelope`, and `_result_ack`
as focused module-level helpers. `_command_for_update` uses
`select(NodeCommand).where(...).with_for_update()` so PostgreSQL takes a row
lock. SQLite treats that clause as a no-op, so the named unique event constraint
alone cannot arbitrate a stale read snapshot. `record_result` issues
`BEGIN IMMEDIATE` as its first SQLite statement, before reading the command;
the configured 30-second busy timeout serializes writers, so the second writer
re-reads the committed event/sequence and returns duplicate or conflict.
PostgreSQL continues to use `FOR UPDATE`. Import SQLAlchemy `text`. A SQLite
busy/snapshot `OperationalError` after that bounded timeout is rolled back and
reported as retryable service unavailability; it never produces an ACK from a
stale transaction.

Add `history_for_device(db, device_id, command_id) ->
NodeCommandHistoryResponse`. It selects the command with an exact device/ID
match, selects `NodeCommandResultEvent` rows ordered by `sequence`, parses each
stored canonical payload through `TypeAdapter(NodeCommandResultRequest)`, and
returns `terminal = row.active_key is None`. A missing command raises
`NodeCommandNotFound`; history reads never change delivery timestamps or state.

`_validate_result_transition` is pure and returns a frozen transition without
mutating the row. Enforce this exact table:

| Event | Required prior state | Additional validation | Transition |
|---|---|---|---|
| `ble_inv_begin` | no prior event; sequence 0 | request ID equals command ID; mode and normalized target equal stored command payload | `result_state="queued"` |
| `ble_inv_progress` | begin accepted; nonterminal | state rank never decreases from queued→scanning→connecting→discovering→reading | update `result_state` |
| `ble_inv_service` | begin accepted; nonterminal | index equals `next_service_index` and is below 16 | increment service index |
| `ble_inv_char` | begin accepted; nonterminal | index equals `next_characteristic_index` and is below 32 | increment characteristic index |
| `ble_inv_read` | begin accepted; nonterminal | index equals `next_read_index` and is below 8 | increment read index |
| `ble_inv_end` | begin accepted; nonterminal | terminal state only | set terminal, clear `active_key`, set completion time |

Every successful event increments global `next_sequence` by one. No event is
accepted after terminal. `_result_ack` returns exactly `ok`, `command_id`,
`accepted_sequence`, `next_sequence`, `result_state`, `terminal`, and
`duplicate`. `_command_envelope` returns the complete eight-field investigate
envelope while state is pending/delivered and the complete eight-field cancel envelope while state is
`cancel_pending`; both are reconstructed from the same stored canonical
payload, so target/mode/timeout cannot drift.

Implementation rules are exact:

- Generate `command_id = uuid.uuid4().hex` and store canonical compact JSON
  using `json.dumps(..., sort_keys=True, separators=(",", ":"))`.
- Repeated polls update delivery timestamps but return byte-equivalent command
  data until a terminal result.
- A cancel request changes `state` to `cancel_pending` without changing the
  command ID; the next envelope becomes `ble_investigate_cancel` and carries
  the original normalized `mode`, `target`, and `timeout_ms`. This lets a node
  that first observes the command after cancellation emit the required begin
  then cancelled terminal events without ever starting a radio operation.
  Both envelope variants carry persisted `next_sequence` and `result_state` so
  a rebooted uplink never guesses sequence zero.
- `record_result` requires `result.sequence == command.next_sequence`, validates
  the C-protocol transition, and inserts one result event.
- If the same `(command_id, sequence)` exists with identical canonical JSON,
  return it with `duplicate=True`; different JSON raises
  `NodeCommandConflict`; a future or skipped sequence also conflicts.
- Increment `next_sequence` only after the unique event insert. On terminal states,
  set `active_key=None`, `state="terminal"`, and `completed_at`.
- Cap service indexes at 16, characteristic indexes at 32, read indexes at 8,
  value hex at 128 characters, summary at 127, error at 63, and timeout at
  12000 ms through the Pydantic models and transition service.

Use the isolated file-backed `backend_sensor_session_factory`, `db_session`,
and `client` fixtures created in Task 1. Do not import or call the configured
application `async_session`, and never delete rows from the application
database in test cleanup.

Add this two-session SQLite concurrency regression:

```python
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
```

Import `asyncio`, `text`, `select`, `OperationalError`, `NodeCommand`, and
`NodeCommandResultEvent`. The barrier is required so both independent sessions
are demonstrably ready before either enters the write transaction. Add this
exact different-body variant; it proves `BEGIN IMMEDIATE` serializes both the
same-body replay above and a competing sequence body rather than merely
covering sequential retries:

```python
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
```

- [ ] **Step 6: Run lifecycle tests**

Run: `cd backend && pytest tests/test_backend_node_commands.py -q`

Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add backend/app/models/schemas.py backend/app/models/db_models.py backend/app/services/node_commands.py backend/tests/test_backend_node_commands.py
git commit -m "backend: model backend sensor commands"
```

---

### Task 5: Expose Enqueue, Poll, and Idempotent Result Endpoints

**Files:**
- Modify: `backend/app/routers/nodes.py`
- Modify: `backend/tests/test_backend_node_commands.py`

**Interfaces:**
- Consumes: `NodeCommandService` from Task 4 and `AsyncSession` from `get_db`.
- Produces: `POST /nodes/{device_id}/commands/ble-investigate`, `POST /nodes/{device_id}/commands/{command_id}/cancel`, `GET /nodes/{device_id}/commands/next`, `POST /nodes/{device_id}/commands/{command_id}/result`, and read-only `GET /nodes/{device_id}/commands/{command_id}` evidence.

- [ ] **Step 1: Write failing endpoint tests**

```python
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
```

- [ ] **Step 2: Run endpoint tests and verify 404 responses**

Run: `cd backend && pytest tests/test_backend_node_commands.py -q`

Expected: FAIL because the routes do not exist.

- [ ] **Step 3: Add the persistent service handlers**

```python
_node_command_service = NodeCommandService()


@router.post(
    "/{device_id}/commands/ble-investigate",
    status_code=201,
    response_model=BleInvestigateCommandEnvelope,
)
async def enqueue_ble_investigation(
    device_id: str,
    command: BleInvestigationCreateRequest,
    db: AsyncSession = Depends(get_db),
):
    try:
        return await _node_command_service.enqueue_ble_investigation(
            db, device_id, command, now=time.time(),
        )
    except NodeCommandConflict as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from exc


@router.post(
    "/{device_id}/commands/{command_id}/cancel",
    response_model=BleInvestigateCancelEnvelope,
)
async def cancel_ble_investigation(
    device_id: str,
    command_id: str,
    db: AsyncSession = Depends(get_db),
):
    try:
        return await _node_command_service.request_cancel(
            db, device_id, command_id, now=time.time(),
        )
    except NodeCommandNotFound as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc


@router.get(
    "/{device_id}/commands/next",
    response_model=NodeCommandEnvelope,
    responses={204: {"description": "No outstanding command"}},
)
async def get_next_node_command(
    device_id: str,
    db: AsyncSession = Depends(get_db),
):
    command = await _node_command_service.next_for_device(
        db, device_id, now=time.time(),
    )
    if command is None:
        return Response(status_code=204)
    return command


@router.post(
    "/{device_id}/commands/{command_id}/result",
    response_model=NodeCommandResultAck,
)
async def record_node_command_result(
    device_id: str,
    command_id: str,
    result: NodeCommandResultRequest,
    db: AsyncSession = Depends(get_db),
):
    try:
        return await _node_command_service.record_result(
            db, device_id, command_id, result, now=time.time(),
        )
    except NodeCommandNotFound as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc
    except NodeCommandConflict as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from exc


@router.get(
    "/{device_id}/commands/{command_id}",
    response_model=NodeCommandHistoryResponse,
)
async def get_node_command_history(
    device_id: str,
    command_id: str,
    db: AsyncSession = Depends(get_db),
):
    try:
        return await _node_command_service.history_for_device(
            db, device_id, command_id,
        )
    except NodeCommandNotFound as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc
```

Import FastAPI `Response` and the new schemas/service. The Task 1 client uses a
temporary database, so test ordering cannot leak command state or delete
configured application data.

- [ ] **Step 4: Run endpoint and validation tests**

Run: `cd backend && pytest tests/test_backend_node_commands.py -q`

Expected: PASS, including invalid MAC, oversized result, unknown command, repeated poll, repeated result, and conflicting replay cases.

- [ ] **Step 5: Commit**

```bash
git add backend/app/routers/nodes.py backend/tests/test_backend_node_commands.py
git commit -m "backend: add sensor command polling"
```

---

### Task 6: Publish Exact Backend-Only Firmware Catalog Identities

**Files:**
- Modify: `backend/app/services/firmware_manager.py`
- Modify: `backend/app/routers/nodes.py`
- Modify: `backend/tests/test_firmware_catalog.py`
- Modify: `backend/tests/test_firmware_auto_endpoints.py`

**Interfaces:**
- Consumes: `backend-firmware/uplink/.pio/build/uplink-s3-backend/firmware.bin` and `backend-firmware/scanner/.pio/build/scanner-s3-combo-backend/firmware.bin`.
- Produces: a strict 164-byte backend identity parser; catalog identity fields
  `name`, `target`, `project`, `hardware`, and `version`; and exact latest
  metadata fields `name`, `target`, `project`, `hardware`, `version`, `size`,
  `sha256`, `crc32`, and `download_url`.

- [ ] **Step 1: Extend the failing catalog assertions**

```python
def test_live_fleet_firmware_targets_are_present():
    assert set(FIRMWARE_TYPES) == {
        "scanner-s3-combo",
        "scanner-s3-combo-fof_badge",
        "scanner-s3-combo-seed",
        "scanner-s3-combo-backend",
        "uplink-s3",
        "uplink-s3-fof_badge",
        "uplink-s3-backend",
    }


def test_backend_targets_have_exact_identity_and_isolated_paths():
    assert FIRMWARE_TYPES["uplink-s3-backend"] == {
        "description": "Backend sensor uplink (Seeed XIAO ESP32-S3)",
        "asset_pattern": "uplink-s3-backend",
        "board": "esp32s3",
        "project": "fof_backend_uplink",
        "hardware": "seeed_xiao_esp32s3",
        "image_kind": 0,
        "partition_capacity": 0x200000,
        "local_bin": firmware_manager._REPO_ROOT / "backend-firmware/uplink/.pio/build/uplink-s3-backend/firmware.bin",
    }
    assert FIRMWARE_TYPES["scanner-s3-combo-backend"]["project"] == "fof_backend_scanner"
    assert FIRMWARE_TYPES["scanner-s3-combo-backend"]["image_kind"] == 1
    assert FIRMWARE_TYPES["scanner-s3-combo-backend"]["partition_capacity"] == 0x200000
    assert str(FIRMWARE_TYPES["scanner-s3-combo-backend"]["local_bin"]).endswith(
        "/backend-firmware/scanner/.pio/build/scanner-s3-combo-backend/firmware.bin"
    )
```

- [ ] **Step 2: Run the catalog tests and verify missing targets**

Run: `cd backend && pytest tests/test_firmware_catalog.py -q`

Expected: FAIL because both backend entries are absent.

- [ ] **Step 3: Add only the two exact catalog entries**

```python
    "uplink-s3-backend": {
        "description": "Backend sensor uplink (Seeed XIAO ESP32-S3)",
        "asset_pattern": "uplink-s3-backend",
        "board": "esp32s3",
        "project": "fof_backend_uplink",
        "hardware": "seeed_xiao_esp32s3",
        "image_kind": 0,
        "partition_capacity": 0x200000,
        "local_bin": _REPO_ROOT / "backend-firmware/uplink/.pio/build/uplink-s3-backend/firmware.bin",
    },
    "scanner-s3-combo-backend": {
        "description": "Backend sensor BLE + Wi-Fi scanner (Seeed XIAO ESP32-S3)",
        "asset_pattern": "scanner-s3-combo-backend",
        "board": "esp32s3",
        "project": "fof_backend_scanner",
        "hardware": "seeed_xiao_esp32s3",
        "image_kind": 1,
        "partition_capacity": 0x200000,
        "local_bin": _REPO_ROOT / "backend-firmware/scanner/.pio/build/scanner-s3-combo-backend/firmware.bin",
    },
```

Do not rename or alias any existing catalog entry.

- [ ] **Step 4: Return project and hardware from catalog/latest metadata**

Add these fields in `FirmwareManager.get_catalog()`:

```python
"target": fw_name,
"project": fw_info.get("project"),
"hardware": fw_info.get("hardware"),
```

Add the corresponding identity fields in `_firmware_metadata()`:

```python
"target": name,
"project": catalog_info.get("project"),
"hardware": catalog_info.get("hardware"),
```

For the exact bytes selected by `_firmware_metadata()`, add:

```python
"crc32": zlib.crc32(data) & 0xFFFFFFFF,
```

`crc32` is required for backend targets and may legitimately be zero; presence
and exact recomputation, not nonzero truthiness, establish validity. Legacy
metadata may expose the same additive integer when bytes are available.

For legacy catalog rows these values remain `None`; backend rows must contain exact strings. Add `X-FoF-Firmware-Target`, `X-FoF-App-Project`, and `X-FoF-Hardware-Type` headers to firmware downloads using the selected `FIRMWARE_TYPES[name]` entry.

- [ ] **Step 5: Write exact backend-image and independent-release tests**

Extend `_esp_firmware_image` to accept `project`,
`identity_records: tuple[bytes, ...] = ()`, and `trailer: bytes = b""`; append
the records and trailer in that order after its descriptor test payload. Write
`project`/`version` into the ESP app descriptor. The identity
record contract is exactly 164 bytes: little-endian `uint32 magic =
0x42464F46`, `uint16 schema = 1`, `uint16 image_kind` (`0` uplink, `1`
scanner), followed by `target[40]`, `project[40]`, `hardware[40]`,
`version[32]`, and little-endian CRC32 over bytes 0 through 159. Strings are
ASCII, NUL-terminated, and zero-filled after the first NUL. Add this fixture
builder and tests:

```python
BACKEND_IDENTITY_STRUCT = struct.Struct("<IHH40s40s40s32sI")


def _fixed_identity_string(value: str, width: int) -> bytes:
    encoded = value.encode("ascii")
    assert 0 < len(encoded) < width
    return encoded + bytes(width - len(encoded))


def _backend_identity_record(
    *, target: str, project: str, hardware: str, version: str, image_kind: int,
) -> bytes:
    prefix = struct.pack("<IHH", 0x42464F46, 1, image_kind) + b"".join((
        _fixed_identity_string(target, 40),
        _fixed_identity_string(project, 40),
        _fixed_identity_string(hardware, 40),
        _fixed_identity_string(version, 32),
    ))
    assert len(prefix) == 160
    return prefix + struct.pack("<I", zlib.crc32(prefix) & 0xFFFFFFFF)


def _backend_image(
    target: str,
    *,
    identity_records: tuple[bytes, ...] | None = None,
    descriptor_project: str | None = None,
    descriptor_version: str = "0.1.0-backend",
    trailer: bytes = b"",
) -> bytes:
    project = "fof_backend_uplink" if target == "uplink-s3-backend" else "fof_backend_scanner"
    image_kind = 0 if target == "uplink-s3-backend" else 1
    identity = _backend_identity_record(
        target=target,
        project=project,
        hardware="seeed_xiao_esp32s3",
        version="0.1.0-backend",
        image_kind=image_kind,
    )
    return _esp_firmware_image(
        descriptor_version,
        project=descriptor_project or project,
        identity_records=identity_records or (identity,),
        trailer=trailer,
    )


def release(tag: str, asset_name: str) -> dict:
    return {
        "tag_name": tag,
        "draft": False,
        "assets": [{
            "name": asset_name,
            "size": 1024,
            "browser_download_url": f"https://example.test/{asset_name}",
        }],
    }


@pytest.mark.asyncio
async def test_backend_latest_metadata_has_exact_identity(monkeypatch, tmp_path):
    name = "uplink-s3-backend"
    image = _backend_image(name)
    manager = _github_manager(monkeypatch, tmp_path, {name: image})
    monkeypatch.setattr(nodes, "_firmware_mgr", manager)
    meta = await nodes._firmware_metadata(name)
    assert meta["name"] == name
    assert meta["project"] == "fof_backend_uplink"
    assert meta["hardware"] == "seeed_xiao_esp32s3"
    assert meta["sha256"] == hashlib.sha256(image).hexdigest()
    assert meta["crc32"] == (zlib.crc32(image) & 0xFFFFFFFF)


@pytest.mark.asyncio
async def test_backend_catalog_rejects_badge_project_under_backend_target(monkeypatch, tmp_path):
    wrong = _esp_firmware_image(
        "0.1.0-backend",
        project="fof_badge_uplink",
        identity_record=_backend_identity_record(
            target="uplink-s3-backend",
            project="fof_backend_uplink",
            hardware="seeed_xiao_esp32s3",
            version="0.1.0-backend",
            image_kind=0,
        ),
    )
    manager = _github_manager(monkeypatch, tmp_path, {"uplink-s3-backend": wrong})
    assert await manager.get_firmware_binary("uplink-s3-backend") is None


@pytest.mark.asyncio
async def test_each_firmware_target_selects_its_newest_matching_release(monkeypatch, tmp_path):
    _mock_github_releases(monkeypatch, [
        release("backend-fw-0.1.0-backend", "uplink-s3-backend.bin"),
        release("v0.67.2-badge-defcon34", "uplink-s3-fof_badge.bin"),
        release("v0.64.68-live-follow", "uplink-s3.bin"),
    ])
    monkeypatch.setattr(firmware_manager, "CACHE_DIR", tmp_path / "cache")
    manager = FirmwareManager()
    await manager.refresh_from_github(force=True)
    assert manager.assets["uplink-s3-backend"].release_tag == "backend-fw-0.1.0-backend"
    assert manager.assets["uplink-s3-fof_badge"].release_tag == "v0.67.2-badge-defcon34"
    assert manager.assets["uplink-s3"].release_tag == "v0.64.68-live-follow"
```

- [ ] **Step 6: Fail closed for mismatched backend images**

Add the exact parser below to `firmware_manager.py`; arbitrary string presence
elsewhere in an image is never identity evidence:

```python
_BACKEND_IDENTITY_MAGIC = struct.pack("<I", 0x42464F46)
_BACKEND_IDENTITY_STRUCT = struct.Struct("<IHH40s40s40s32sI")
_BACKEND_VERSION_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+-backend$")


def _decode_identity_string(raw: bytes) -> str | None:
    nul = raw.find(b"\0")
    if nul <= 0 or any(raw[nul + 1:]):
        return None
    try:
        value = raw[:nul].decode("ascii")
    except UnicodeDecodeError:
        return None
    if any(ord(char) < 0x21 or ord(char) > 0x7E for char in value):
        return None
    return value


def _parse_backend_identity_record(image: bytes, offset: int) -> dict | None:
    end = offset + _BACKEND_IDENTITY_STRUCT.size
    if end > len(image):
        return None
    record = image[offset:end]
    magic, schema, image_kind, target_raw, project_raw, hardware_raw, version_raw, crc32 = (
        _BACKEND_IDENTITY_STRUCT.unpack(record)
    )
    if magic != 0x42464F46 or schema != 1 or image_kind not in (0, 1):
        return None
    if (zlib.crc32(record[:160]) & 0xFFFFFFFF) != crc32:
        return None
    target = _decode_identity_string(target_raw)
    project = _decode_identity_string(project_raw)
    hardware = _decode_identity_string(hardware_raw)
    version = _decode_identity_string(version_raw)
    if None in (target, project, hardware, version):
        return None
    return {
        "offset": offset,
        "schema": schema,
        "image_kind": image_kind,
        "target": target,
        "project": project,
        "hardware": hardware,
        "version": version,
    }


def _parse_backend_identity(image: bytes) -> dict | None:
    valid: list[dict] = []
    start = 0
    while True:
        offset = image.find(_BACKEND_IDENTITY_MAGIC, start)
        if offset < 0:
            break
        parsed = _parse_backend_identity_record(image, offset)
        if parsed is not None:
            valid.append(parsed)
        start = offset + 1
    if len(valid) != 1:
        return None
    return valid[0]


def _validated_backend_image_info(name: str, image: bytes) -> dict | None:
    info = FIRMWARE_TYPES.get(name)
    if info is None or not name.endswith("-backend"):
        return None
    desc = _parse_app_desc_bytes(image)
    identity = _parse_backend_identity(image)
    if desc is None or identity is None:
        return None
    expected = {
        "target": name,
        "project": info["project"],
        "hardware": info["hardware"],
        "image_kind": info["image_kind"],
    }
    if any(identity[key] != value for key, value in expected.items()):
        return None
    if identity["project"] != desc["project"] or identity["version"] != desc["version"]:
        return None
    if _BACKEND_VERSION_RE.fullmatch(identity["version"]) is None:
        return None
    if not (0 < len(image) <= info["partition_capacity"]):
        return None
    return {**identity, "size": len(image)}
```

Return `None`, not `False`, on the first failure in
`_validated_backend_image_info`. Import `re`, `struct`, and `zlib`. Call this
validator for names ending in `-backend` after reading any custom, local,
cached, or downloaded image and before returning bytes. Leave legacy target
behavior unchanged. An image containing the backend magic but failing parsing
is a malformed backend claim and must never fall through to a legacy path.

Add these parser-count tests. Raw magic whose following bytes do not form a
valid record is ignored for cardinality, while zero or two valid structured
records fail closed:

```python
RAW_INVALID_MAGIC = struct.pack("<I", 0x42464F46) + bytes(160)


@pytest.mark.parametrize("target", [
    "uplink-s3-backend", "scanner-s3-combo-backend",
])
def test_exactly_one_backend_identity_record_is_accepted(target):
    image = _backend_image(target)
    parsed = firmware_manager._parse_backend_identity(image)
    assert parsed is not None
    assert parsed["target"] == target
    assert firmware_manager._validated_backend_image_info(target, image) is not None


def test_one_valid_record_plus_raw_invalid_magic_is_accepted():
    target = "uplink-s3-backend"
    image = _backend_image(target, trailer=RAW_INVALID_MAGIC)
    parsed = firmware_manager._parse_backend_identity(image)
    assert parsed is not None
    assert parsed["target"] == target
    assert firmware_manager._validated_backend_image_info(target, image) is not None


def test_two_valid_identity_records_are_rejected():
    valid = _backend_identity_record(
        target="uplink-s3-backend",
        project="fof_backend_uplink",
        hardware="seeed_xiao_esp32s3",
        version="0.1.0-backend",
        image_kind=0,
    )
    image = _backend_image(
        "uplink-s3-backend", identity_records=(valid, valid),
    )
    assert firmware_manager._parse_backend_identity(image) is None
    assert firmware_manager._validated_backend_image_info(
        "uplink-s3-backend", image,
    ) is None


def test_raw_invalid_magic_without_a_valid_record_is_rejected():
    image = _backend_image(
        "uplink-s3-backend", identity_records=(RAW_INVALID_MAGIC,),
    )
    assert firmware_manager._parse_backend_identity(image) is None
    assert firmware_manager._validated_backend_image_info(
        "uplink-s3-backend", image,
    ) is None
```

Add the exact mutation matrix below. It recomputes CRC after every structural
mutation except the case whose purpose is a bad CRC, preventing one invalid
checksum from masking the field-specific checks:

```python
def _identity_recrc(prefix: bytes) -> bytes:
    assert len(prefix) == 160
    return prefix + struct.pack("<I", zlib.crc32(prefix) & 0xFFFFFFFF)


def _identity_replace(record: bytes, start: int, width: int, value: str) -> bytes:
    prefix = bytearray(record[:160])
    prefix[start:start + width] = _fixed_identity_string(value, width)
    return _identity_recrc(bytes(prefix))


@pytest.mark.parametrize("mutation", [
    "bad_crc",
    "schema",
    "image_kind",
    "target",
    "project",
    "hardware",
    "invalid_backend_version",
    "descriptor_project",
    "descriptor_version",
    "nonzero_after_nul",
    "oversized_partition",
])
def test_backend_identity_mutation_matrix_fails_closed(mutation):
    target = "uplink-s3-backend"
    project = "fof_backend_uplink"
    record = _backend_identity_record(
        target=target,
        project=project,
        hardware="seeed_xiao_esp32s3",
        version="0.1.0-backend",
        image_kind=0,
    )
    descriptor_project = project
    descriptor_version = "0.1.0-backend"

    if mutation == "bad_crc":
        record = record[:-1] + bytes([record[-1] ^ 0x01])
    elif mutation == "schema":
        prefix = bytearray(record[:160])
        struct.pack_into("<H", prefix, 4, 2)
        record = _identity_recrc(bytes(prefix))
    elif mutation == "image_kind":
        prefix = bytearray(record[:160])
        struct.pack_into("<H", prefix, 6, 1)
        record = _identity_recrc(bytes(prefix))
    elif mutation == "target":
        record = _identity_replace(record, 8, 40, "scanner-s3-combo-backend")
    elif mutation == "project":
        record = _identity_replace(record, 48, 40, "fof_badge_uplink")
    elif mutation == "hardware":
        record = _identity_replace(record, 88, 40, "esp32s3_other")
    elif mutation == "invalid_backend_version":
        record = _identity_replace(record, 128, 32, "0.1.0-badge")
        descriptor_version = "0.1.0-badge"
    elif mutation == "descriptor_project":
        descriptor_project = "fof_badge_uplink"
    elif mutation == "descriptor_version":
        descriptor_version = "0.1.1-backend"
    elif mutation == "nonzero_after_nul":
        prefix = bytearray(record[:160])
        first_padding_byte_after_nul = 8 + len(target) + 1
        prefix[first_padding_byte_after_nul] = 0x41
        record = _identity_recrc(bytes(prefix))

    image = _backend_image(
        target,
        identity_records=(record,),
        descriptor_project=descriptor_project,
        descriptor_version=descriptor_version,
    )
    if mutation == "oversized_partition":
        capacity = FIRMWARE_TYPES[target]["partition_capacity"]
        image = image.ljust(capacity + 1, b"\xA5")

    assert firmware_manager._validated_backend_image_info(target, image) is None
```

This matrix covers CRC, schema, kind, target, project, hardware, version
grammar, descriptor agreement, canonical NUL padding, and size independently.
The duplicate-valid and raw-invalid-magic tests above cover parser cardinality
separately from those mutations.

- [ ] **Step 7: Keep custom versions and hashes content-exact**

Add these HTTP regressions to `test_firmware_auto_endpoints.py`:

```python
@pytest.mark.asyncio
async def test_custom_backend_image_uses_embedded_version_and_content_sha(client):
    name = "uplink-s3-backend"
    first = _backend_image(name, payload_fill=b"A")
    second = _backend_image(name, payload_fill=b"B")
    assert len(first) == len(second) and first != second
    try:
        uploaded = await client.post(
            f"/nodes/firmware/upload/{name}",
            files={"firmware": ("backend.bin", first, "application/octet-stream")},
        )
        assert uploaded.status_code == 200
        first_meta = (await client.get(f"/nodes/firmware/latest/{name}")).json()
        assert first_meta["version"] == "0.1.0-backend"
        assert first_meta["sha256"] == hashlib.sha256(first).hexdigest()

        replaced = await client.post(
            f"/nodes/firmware/upload/{name}",
            files={"firmware": ("backend.bin", second, "application/octet-stream")},
        )
        assert replaced.status_code == 200
        second_meta = (await client.get(f"/nodes/firmware/latest/{name}")).json()
        assert second_meta["version"] == "0.1.0-backend"
        assert second_meta["sha256"] == hashlib.sha256(second).hexdigest()
        assert second_meta["sha256"] != first_meta["sha256"]
        download = await client.get(f"/nodes/firmware/download/{name}")
        assert download.headers["etag"] == f'"{second_meta["sha256"]}"'
    finally:
        nodes._firmware_mgr.clear_custom_firmware(name)


@pytest.mark.asyncio
async def test_custom_backend_upload_rejects_mismatched_identity(client):
    name = "uplink-s3-backend"
    wrong = _backend_image("scanner-s3-combo-backend")
    response = await client.post(
        f"/nodes/firmware/upload/{name}",
        files={"firmware": ("wrong.bin", wrong, "application/octet-stream")},
    )
    assert response.status_code == 400
    assert name not in nodes._firmware_mgr._custom_firmware
```

Extend the fixture helper with `payload_fill: bytes = b"A"` and keep image
length constant across fill values. In `FirmwareManager`, add
`validate_firmware_image(name: str, image: bytes) -> bool`; it calls
`_validated_backend_image_info` for backend targets and preserves legacy
validation behavior for existing targets. Call it before every backend binary
return and before `set_custom_firmware` in the upload route.

For names ending in `-backend` only, bypass the legacy custom-version sentinel:
obtain the selected validated bytes and return the parsed ESP app descriptor
version, and make `get_catalog()` report that embedded backend version. Keep
the literal `"custom"` behavior unchanged for every legacy target. Add an
explicit regression that a custom `uplink-s3` upload still reports
`version="custom"` while `uplink-s3-backend` reports `0.1.0-backend`.

Delete `_FW_HASH_CACHE`. In both `_firmware_metadata()` and
`get_firmware_download()`, calculate the digest directly from the bytes that
will be returned:

```python
sha = hashlib.sha256(data).hexdigest()
```

Images are bounded at 2 MiB, so this avoids stale SHA/ETag values when a custom
image is replaced by different bytes with the same version and size. Remove
test cleanup that reaches into `_FW_HASH_CACHE`.

- [ ] **Step 8: Select release assets independently per target**

Refactor `refresh_from_github` to iterate releases newest-first independently
for each target. Map each `.bin` asset to a target by testing known
`asset_pattern` values longest-first, accepting either `<target>.bin` or
`<target>-<release-tag>.bin`; only the longest matching known target owns the
asset. Then select the first release containing an asset whose mapped target is
exactly the requested target and store that release's own `release_tag`. This
prevents `scanner-s3-combo` from claiming
`scanner-s3-combo-backend-<tag>.bin` or a badge/seed asset. Do not keep one
selected release for all targets; production, badge, and backend tracks must
coexist.

Use this pure exact-name mapper; sorting is retained as defense in depth, but
full filename equality—not `startswith`—is what prevents prefix collisions:

```python
def _asset_target(asset_name: str, release_tag: str) -> str | None:
    ordered = sorted(
        FIRMWARE_TYPES.items(),
        key=lambda item: len(item[1]["asset_pattern"]),
        reverse=True,
    )
    for target, info in ordered:
        pattern = info["asset_pattern"]
        if asset_name in {
            f"{pattern}.bin",
            f"{pattern}-{release_tag}.bin",
        }:
            return target
    return None
```

Add these exact mapper and integration regressions:

```python
@pytest.mark.parametrize(("asset_name", "expected"), [
    ("scanner-s3-combo-backend.bin", "scanner-s3-combo-backend"),
    ("scanner-s3-combo-fof_badge-v0.1.0-mixed.bin", "scanner-s3-combo-fof_badge"),
    ("scanner-s3-combo-seed.bin", "scanner-s3-combo-seed"),
    ("scanner-s3-combo-v0.1.0-mixed.bin", "scanner-s3-combo"),
    ("uplink-s3-backend-v0.1.0-mixed.bin", "uplink-s3-backend"),
    ("uplink-s3-fof_badge.bin", "uplink-s3-fof_badge"),
    ("uplink-s3.bin", "uplink-s3"),
    ("scanner-s3-combo-backendish-v0.1.0-mixed.bin", None),
    ("scanner-s3-combo-backend-v0.1.0-mixed-extra.bin", None),
    ("scanner-s3-combo-backend-v0.1.0-mixed.bin.sig", None),
    ("prefix-uplink-s3-backend-v0.1.0-mixed.bin", None),
])
def test_asset_target_requires_longest_exact_filename(asset_name, expected):
    assert firmware_manager._asset_target(asset_name, "v0.1.0-mixed") == expected


@pytest.mark.asyncio
async def test_scanner_base_target_cannot_claim_backend_or_badge_asset(
    monkeypatch, tmp_path,
):
    tag = "v0.1.0-mixed"
    _mock_github_releases(monkeypatch, [{
        "tag_name": tag,
        "draft": False,
        "assets": [
            {
                "name": f"scanner-s3-combo-backend-{tag}.bin",
                "size": 1024,
                "browser_download_url": "https://example.test/backend.bin",
            },
            {
                "name": f"scanner-s3-combo-fof_badge-{tag}.bin",
                "size": 1024,
                "browser_download_url": "https://example.test/badge.bin",
            },
        ],
    }])
    monkeypatch.setattr(firmware_manager, "CACHE_DIR", tmp_path / "cache")
    manager = FirmwareManager()
    await manager.refresh_from_github(force=True)
    assert set(manager.assets) == {
        "scanner-s3-combo-backend", "scanner-s3-combo-fof_badge",
    }
    assert "scanner-s3-combo" not in manager.assets
```

- [ ] **Step 9: Run catalog and auto-update tests**

Run: `cd backend && pytest tests/test_firmware_catalog.py tests/test_firmware_auto_endpoints.py -q`

Expected: PASS.

- [ ] **Step 10: Commit**

```bash
git add backend/app/services/firmware_manager.py backend/app/routers/nodes.py backend/tests/test_firmware_catalog.py backend/tests/test_firmware_auto_endpoints.py
git commit -m "backend: catalog backend sensor firmware"
```

---

### Task 7: Gate OTA by Running Family and Report Target-Aware Versions

**Files:**
- Modify: `backend/app/routers/nodes.py`
- Modify: `backend/app/routers/detections.py`
- Modify: `backend/tests/test_backend_firmware_ingest.py`
- Modify: `backend/tests/test_scanner_ota_relay_paths.py`
- Modify: `backend/tests/test_scanner_firmware_fleet.py`

**Interfaces:**
- Consumes: validated Task 6 image identity, uplink heartbeat identity, and each scanner snapshot's `firmware_target`/`app_project`/`hardware_type`.
- Produces: `_require_ota_compatibility(device_id: str, firmware_name: str | None, image: bytes, uart: str | None = None) -> None` and target-aware firmware diagnostics.

- [ ] **Step 1: Write failing family-gating and diagnostics tests**

Add these cases with network/upload calls monkeypatched to fail the test if a
rejected route reaches them:

```python
@pytest.mark.asyncio
async def test_backend_named_image_is_not_sent_to_badge_uplink(client, monkeypatch):
    detections._node_heartbeats["uplink_BADGE1"] = {
        "device_id": "uplink_BADGE1", "ip": "10.0.0.20",
        "firmware_target": "uplink-s3-fof_badge",
        "app_project": "fof_badge_uplink",
        "hardware_type": "seeed_xiao_esp32s3",
        "last_seen": time.time(),
    }
    monkeypatch.setattr(
        nodes._firmware_mgr, "get_firmware_binary",
        AsyncMock(side_effect=AssertionError("must reject before loading image")),
    )
    response = await client.post("/nodes/uplink_BADGE1/ota/uplink-s3-backend")
    assert response.status_code == 409
    assert response.json()["detail"] == "running firmware identity is incompatible with uplink-s3-backend"


@pytest.mark.asyncio
async def test_backend_uplink_is_not_sent_production_image(client, monkeypatch):
    detections._node_heartbeats["uplink_BACK01"] = {
        "device_id": "uplink_BACK01", "ip": "10.0.0.21",
        "firmware_target": "uplink-s3-backend",
        "app_project": "fof_backend_uplink",
        "hardware_type": "seeed_xiao_esp32s3",
        "firmware_version": "0.1.0-backend", "last_seen": time.time(),
    }
    monkeypatch.setattr(
        nodes._firmware_mgr, "get_firmware_binary",
        AsyncMock(side_effect=AssertionError("must reject before loading image")),
    )
    response = await client.post("/nodes/uplink_BACK01/ota/uplink-s3")
    assert response.status_code == 409


def test_backend_versions_are_target_aware_without_repurposing_api_version():
    assert detections._EXPECTED_BACKEND_VERSION == app.version
    assert detections._firmware_version_state(
        "0.1.0-backend", "uplink-s3-backend",
    ) == "current"
    assert detections._firmware_version_state(
        "0.1.1-backend", "uplink-s3-backend",
    ) == "drift"
    assert detections._firmware_version_state(
        detections._EXPECTED_FIRMWARE_VERSION, "uplink-s3",
    ) == "current"
```

In scanner relay tests, report two scanner snapshots: one exact backend scanner
and one badge/production scanner. Assert backend scanner push/stage succeeds
only for the exact backend snapshot, backend image to the other snapshot is
409, production image to the backend snapshot is 409, and `uart="both"`
rejects unless both snapshots are exact backend identities. In fleet tests,
the same mismatches appear as `identity_mismatch` blockers and are absent from
the rollout execution target list.

Add a direct-upload case for `POST /nodes/{device_id}/ota`: a valid structured
backend image sent to a badge heartbeat returns 409; an image containing the
backend magic with invalid CRC returns 400; a legacy image sent to an exact
backend heartbeat returns 409. These assertions cover both named and raw paths.

- [ ] **Step 2: Run the focused tests and observe unsafe routing/version drift**

Run: `cd backend && pytest tests/test_backend_firmware_ingest.py tests/test_scanner_ota_relay_paths.py tests/test_scanner_firmware_fleet.py -q`

Expected: FAIL because OTA routes only check name prefixes and diagnostics do
not use firmware targets.

- [ ] **Step 3: Implement one compatibility gate and call it from every OTA path**

Add these exact helpers to `nodes.py`:

```python
def _reported_identities(device_id: str, uart: str | None) -> list[dict]:
    heartbeat = _node_heartbeats.get(device_id) or {}
    if uart is None:
        return [{
            "target": heartbeat.get("firmware_target") or heartbeat.get("firmware_name"),
            "project": heartbeat.get("app_project"),
            "hardware": heartbeat.get("hardware_type"),
        }]
    scanners = [item for item in heartbeat.get("scanners") or [] if isinstance(item, dict)]
    selected = scanners if uart == "both" else [
        item for item in scanners if item.get("uart") == uart
    ]
    return [{
        "target": item.get("firmware_target") or item.get("firmware_name"),
        "project": item.get("app_project"),
        "hardware": item.get("hardware_type"),
    } for item in selected]


def _identity_is_backend(identity: dict) -> bool:
    return (
        str(identity.get("target") or "").endswith("-backend")
        or str(identity.get("project") or "").startswith("fof_backend_")
    )


def _require_named_family_preflight(
    device_id: str, firmware_name: str, uart: str | None = None,
) -> None:
    running = _reported_identities(device_id, uart)
    required_count = 2 if uart == "both" else 1
    info = FIRMWARE_TYPES.get(firmware_name) or {}
    if firmware_name.endswith("-backend"):
        expected = {
            "target": firmware_name,
            "project": info.get("project"),
            "hardware": info.get("hardware"),
        }
        compatible = len(running) == required_count and all(item == expected for item in running)
    else:
        compatible = len(running) == required_count and not any(_identity_is_backend(item) for item in running)
    if not compatible:
        raise HTTPException(
            status_code=409,
            detail=f"running firmware identity is incompatible with {firmware_name}",
        )


def _require_ota_compatibility(
    device_id: str,
    firmware_name: str | None,
    image: bytes,
    uart: str | None = None,
) -> None:
    requested = None
    descriptor = _parse_app_desc_bytes(image)
    claims_backend = (
        _BACKEND_IDENTITY_MAGIC in image
        or str((descriptor or {}).get("project") or "").startswith("fof_backend_")
        or str((descriptor or {}).get("version") or "").endswith("-backend")
    )
    if firmware_name and firmware_name.endswith("-backend"):
        requested = _validated_backend_image_info(firmware_name, image)
        if requested is None:
            raise HTTPException(status_code=400, detail="invalid backend firmware identity")
    elif claims_backend:
        identity = _parse_backend_identity(image)
        claimed_name = identity.get("target") if identity else None
        requested = (
            _validated_backend_image_info(claimed_name, image)
            if claimed_name else None
        )
        if requested is None:
            raise HTTPException(status_code=400, detail="invalid backend firmware identity")

    running = _reported_identities(device_id, uart)
    required_count = 2 if uart == "both" else 1
    requested_is_backend = requested is not None
    if requested_is_backend:
        expected = {
            "target": requested["target"],
            "project": requested["project"],
            "hardware": requested["hardware"],
        }
        compatible = len(running) == required_count and all(identity == expected for identity in running)
    else:
        compatible = len(running) == required_count and not any(_identity_is_backend(item) for item in running)
    if not compatible:
        label = firmware_name or (requested["target"] if requested else "legacy image")
        raise HTTPException(
            status_code=409,
            detail=f"running firmware identity is incompatible with {label}",
        )
```

Import the structured parser/validator constants from `firmware_manager`.
Resolve the target heartbeat before loading a named image; reject a known
family mismatch immediately, then call `_require_ota_compatibility` again with
the loaded bytes before any HTTP upload, staging write, relay, or rollout task
is created. Apply it to direct uplink upload, named uplink push, scanner push,
scanner stage, `_stage_scanner_firmware_on_uplink`, readiness/fleet target
selection, and rollout execution. Mismatched fleet entries become
`identity_mismatch` blockers. Unknown identity is never allowed to receive a
backend image; initial cross-family scanner migration remains USB-only.

- [ ] **Step 4: Make firmware diagnostics target-aware**

Add without changing `_EXPECTED_BACKEND_VERSION`:

```python
_EXPECTED_BACKEND_FIRMWARE_VERSIONS = {
    "uplink-s3-backend": "0.1.0-backend",
    "scanner-s3-combo-backend": "0.1.0-backend",
}


def _expected_firmware_version(target: str | None) -> str | None:
    return _EXPECTED_BACKEND_FIRMWARE_VERSIONS.get(str(target or ""))


def _firmware_version_state(version: str | None, target: str | None = None) -> str:
    if not version:
        return "unknown"
    backend_expected = _expected_firmware_version(target)
    if backend_expected is not None:
        return "current" if str(version) == backend_expected else "drift"
    value = str(version)
    if _EXPECTED_FIRMWARE_VERSION in value or _EXPECTED_BADGE_FIRMWARE_VERSION in value:
        return "current"
    return "drift"
```

Pass each uplink's canonical target and each scanner's
`firmware_target or firmware_name` into the helper. Warning text uses the exact
mapped version for backend targets and the existing legacy label otherwise.
Expose a copy of `_EXPECTED_BACKEND_FIRMWARE_VERSIONS` in
`firmware_readiness`; keep `backend_version` and `expected_backend_version`
bound to the FastAPI application release for compatibility.

- [ ] **Step 5: Run gating and diagnostics tests**

Run: `cd backend && pytest tests/test_backend_firmware_ingest.py tests/test_scanner_ota_relay_paths.py tests/test_scanner_firmware_fleet.py tests/test_firmware_auto_endpoints.py -q`

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add backend/app/routers/nodes.py backend/app/routers/detections.py backend/tests/test_backend_firmware_ingest.py backend/tests/test_scanner_ota_relay_paths.py backend/tests/test_scanner_firmware_fleet.py
git commit -m "backend: gate backend firmware by running identity"
```

---

### Task 8: Verify the Complete API Contract and Edge Cases

**Files:**
- Modify: `backend/tests/test_backend_firmware_ingest.py`
- Modify: `backend/tests/test_backend_node_commands.py`

**Interfaces:**
- Consumes: all interfaces from Tasks 1-7.
- Produces: regression proof for heartbeat, replay, validation, clock skew, commands, and legacy compatibility.

- [ ] **Step 1: Add invalid, replay, and empty-heartbeat cases**

Add tests with these exact expectations:

```python
@pytest.mark.asyncio
async def test_backend_heartbeat_accepts_empty_detection_array(client):
    response = await client.post("/detections/drones", json={
        "device_id": "uplink_CB77A4",
        "timestamp": 1_785_600_000,
        "firmware_target": "uplink-s3-backend",
        "detections": [],
    })
    assert response.status_code == 200
    assert response.json() == {
        "status": "ok",
        "accepted": 0,
        "device_id": "uplink_CB77A4",
        "processed": 0,
        "deduplicated": 0,
        "filtered": 0,
    }


@pytest.mark.asyncio
async def test_invalid_backend_detection_is_http_400(client):
    response = await client.post("/detections/drones", json={
        "device_id": "uplink_CB77A4",
        "detections": [{"source": "ble_rid", "confidence": 0.8}],
    })
    assert response.status_code == 400


@pytest.mark.asyncio
async def test_replayed_non_rid_batch_is_acknowledged_without_duplicate_accept(client):
    body = {
        "device_id": "uplink_CB77A4",
        "timestamp": 1_785_600_000,
        "detections": [{
            "drone_id": "BLE:AA:BB:CC:DD:EE:FF",
            "source": "ble_fingerprint",
            "confidence": 0.8,
            "bssid": "AA:BB:CC:DD:EE:FF",
        }],
    }
    first = await client.post("/detections/drones", json=body)
    second = await client.post("/detections/drones", json=body)
    assert first.json()["accepted"] == 1
    assert first.json()["processed"] == 1
    assert second.json()["accepted"] == 1
    assert second.json()["processed"] == 0
    assert second.json()["deduplicated"] == 1
    assert second.json()["device_id"] == "uplink_CB77A4"


@pytest.mark.asyncio
async def test_locally_filtered_item_is_transport_accepted(client):
    response = await client.post("/detections/drones", json={
        "device_id": "uplink_CB77A4",
        "timestamp": int(time.time()),
        "detections": [{
            "drone_id": "AP:FOF-SELF",
            "source": "wifi_ap",
            "confidence": 0.8,
            "ssid": "FoF-Uplink-CB77A4",
            "bssid": "AA:BB:CC:DD:EE:FF",
        }],
    })
    assert response.status_code == 200
    assert response.json() == {
        "status": "ok", "accepted": 1, "device_id": "uplink_CB77A4",
        "processed": 0, "deduplicated": 0, "filtered": 1,
    }
```

The replay test documents current best-effort semantics; durable RID idempotency is explicitly outside this trusted-LAN port.

- [ ] **Step 2: Run the focused backend sensor suite**

Run: `cd backend && pytest tests/test_backend_firmware_ingest.py tests/test_backend_node_commands.py tests/test_firmware_catalog.py tests/test_firmware_auto_endpoints.py -q`

Expected: PASS.

- [ ] **Step 3: Run all backend tests**

Run: `cd backend && pytest tests -q`

Expected: PASS with no new warnings caused by these changes.

- [ ] **Step 4: Audit protected firmware paths before committing**

Run all four commands from the worktree root:

```bash
git diff --check
git diff --name-only -- esp32/uplink esp32/scanner esp32/shared esp32/web-flasher scripts
git diff --cached --name-only -- esp32/uplink esp32/scanner esp32/shared esp32/web-flasher scripts
git ls-files --others --exclude-standard -- esp32/uplink esp32/scanner esp32/shared esp32/web-flasher scripts
```

Expected: no output from any command. The three path commands cover tracked
working changes, staged changes, and untracked files respectively.

- [ ] **Step 5: Commit the edge-case coverage**

```bash
git add backend/tests/test_backend_firmware_ingest.py backend/tests/test_backend_node_commands.py
git commit -m "test: cover backend sensor API edge cases"
```

- [ ] **Step 6: Audit the committed branch against main**

Run:

```bash
git diff --name-only origin/main...HEAD -- esp32/uplink esp32/scanner esp32/shared esp32/web-flasher scripts
git status --short
```

Expected: the branch-range protected-path command prints no paths. `git status`
prints no API-plan implementation changes; unrelated pre-existing work, if
present, is reported verbatim and is not modified or committed.

## Plan 1 Completion Gate

Run:

```bash
cd backend
pytest tests -q
```

Expected: the entire backend suite passes; the API accepts legacy payloads plus the complete backend firmware envelope; command retries are idempotent; catalog metadata names only exact backend identities for the new artifacts.

From the worktree root, repeat the tracked, staged, untracked, and
`origin/main...HEAD` protected-path commands from the Global Constraints and
Task 8. All four scopes must print no protected paths before Plan 2 begins.
