"""FastAPI application entry point."""

import logging
from contextlib import asynccontextmanager

from fastapi import FastAPI, Request

from app.cache import close_redis, redis_ping
from app.config import settings
from app.models.schemas import HealthResponse
from app.routers import aircraft, detections, nodes
from app.services.database import close_db, create_tables

logging.basicConfig(
    level=logging.DEBUG if settings.debug else logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)
logger = logging.getLogger(__name__)


@asynccontextmanager
async def lifespan(app: FastAPI):
    """Startup/shutdown lifecycle hooks."""
    logger.info("Starting %s", settings.app_name)
    # Create database tables (IF NOT EXISTS — safe on every restart)
    try:
        await create_tables()
        logger.info("PostgreSQL tables ready")
    except Exception as e:
        logger.warning("PostgreSQL not available (will run without persistence): %s", e)
    yield
    logger.info("Shutting down")
    await close_redis()
    await close_db()


app = FastAPI(
    title=settings.app_name,
    description="Backend proxy & enrichment layer for the Friend or Foe aircraft/drone identification app.",
    version="0.32.0",
    lifespan=lifespan,
)

# ---------------------------------------------------------------------------
# Request logging middleware
# ---------------------------------------------------------------------------
@app.middleware("http")
async def log_requests(request: Request, call_next):
    """Log every incoming request for debugging."""
    logger.info(">>> %s %s from %s", request.method, request.url.path, request.client.host if request.client else "unknown")
    response = await call_next(request)
    logger.info("<<< %s %s -> %d", request.method, request.url.path, response.status_code)
    return response


# ---------------------------------------------------------------------------
# Routers
# ---------------------------------------------------------------------------
app.include_router(aircraft.router)
app.include_router(detections.router)
app.include_router(nodes.router)


# ---------------------------------------------------------------------------
# Health check
# ---------------------------------------------------------------------------
@app.get("/health", response_model=HealthResponse, tags=["system"])
async def health_check() -> HealthResponse:
    """Liveness / readiness probe."""
    redis_ok = await redis_ping()
    # Quick DB check
    db_status = "unknown"
    try:
        from app.services.database import engine
        async with engine.connect() as conn:
            await conn.execute(type(conn).get_raw_connection)  # noqa
        db_status = "ok"
    except Exception:
        try:
            from app.services.database import async_session
            async with async_session() as session:
                from sqlalchemy import text
                await session.execute(text("SELECT 1"))
            db_status = "ok"
        except Exception:
            db_status = "unavailable"

    return HealthResponse(
        status="ok",
        version="0.32.0",
        redis="ok" if redis_ok else "unavailable",
        database=db_status,
    )


@app.get("/", tags=["system"])
async def root():
    """Root redirect / welcome."""
    return {
        "app": settings.app_name,
        "docs": "/docs",
        "health": "/health",
    }
