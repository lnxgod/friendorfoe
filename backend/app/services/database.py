"""SQLAlchemy async database engine and session factory."""

from sqlalchemy import event, inspect, text
from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker, create_async_engine
from sqlalchemy.orm import DeclarativeBase

from app.config import settings

_engine_kwargs: dict = {"echo": False}
if "sqlite" in settings.database_url:
    # The live edge box uses SQLite during field testing and receives bursts
    # from multiple ESP32 nodes. WAL + busy_timeout keeps reads/writes from
    # failing immediately under dashboard + ingest concurrency.
    _engine_kwargs.update(connect_args={"timeout": 30})
else:
    _engine_kwargs.update(pool_size=5, max_overflow=10)

engine = create_async_engine(settings.database_url, **_engine_kwargs)

if "sqlite" in settings.database_url:
    @event.listens_for(engine.sync_engine, "connect")
    def _set_sqlite_pragmas(dbapi_connection, _connection_record) -> None:
        cursor = dbapi_connection.cursor()
        try:
            cursor.execute("PRAGMA journal_mode=WAL")
            cursor.execute("PRAGMA synchronous=NORMAL")
            cursor.execute("PRAGMA busy_timeout=30000")
        finally:
            cursor.close()

async_session = async_sessionmaker(engine, class_=AsyncSession, expire_on_commit=False)


class Base(DeclarativeBase):
    """Base class for all ORM models."""
    pass


async def get_db() -> AsyncSession:
    """Dependency that yields an async database session."""
    async with async_session() as session:
        yield session


async def create_tables():
    """Create all tables (safe to call multiple times — uses IF NOT EXISTS)."""
    async with engine.begin() as conn:
        await conn.run_sync(Base.metadata.create_all)
        await conn.run_sync(_ensure_sensor_node_columns)
        await conn.run_sync(_ensure_detection_columns)


def _ensure_sensor_node_columns(sync_conn) -> None:
    """Best-effort lightweight schema reconciliation for additive node fields."""
    inspector = inspect(sync_conn)
    if "sensor_nodes" not in inspector.get_table_names():
        return
    columns = {col["name"] for col in inspector.get_columns("sensor_nodes")}
    if "position_mode" not in columns:
        sync_conn.execute(
            text(
                "ALTER TABLE sensor_nodes "
                "ADD COLUMN position_mode VARCHAR(16) NOT NULL DEFAULT 'active'"
            )
        )
        sync_conn.execute(
            text(
                "UPDATE sensor_nodes SET position_mode = 'active' "
                "WHERE position_mode IS NULL"
            )
        )


def _ensure_detection_columns(sync_conn) -> None:
    """Best-effort lightweight schema reconciliation for additive RF evidence fields."""
    inspector = inspect(sync_conn)
    if "drone_detections" not in inspector.get_table_names():
        return
    columns = {col["name"] for col in inspector.get_columns("drone_detections")}
    wanted = {
        "mac_is_randomized": "BOOLEAN",
        "mac_identity_kind": "VARCHAR(32)",
        "mac_reason": "VARCHAR(64)",
        "brand": "VARCHAR(128)",
        "brand_source": "VARCHAR(48)",
        "brand_confidence": "FLOAT",
        "device_class": "VARCHAR(64)",
        "device_class_confidence": "FLOAT",
        "identity_source": "VARCHAR(48)",
        "evidence_json": "TEXT",
    }
    for name, ddl_type in wanted.items():
        if name not in columns:
            sync_conn.execute(text(f"ALTER TABLE drone_detections ADD COLUMN {name} {ddl_type}"))


async def close_db():
    """Dispose of the engine connection pool."""
    await engine.dispose()
