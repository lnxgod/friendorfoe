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
