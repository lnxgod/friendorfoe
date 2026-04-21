"""FastAPI application entry point."""

import logging
from contextlib import asynccontextmanager

from pathlib import Path

from fastapi import FastAPI, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles

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

    # Rehydrate EntityTracker + EventDetector from DB so known devices /
    # previously-emitted first-seen events survive a restart.
    try:
        from app.services.database import async_session
        from app.routers.detections import _entity_tracker, _event_detector
        async with async_session() as session:
            await _entity_tracker.rehydrate_from_db(session)
            await _event_detector.hydrate_from_db(session)
    except Exception as e:
        logger.warning("EntityTracker / EventDetector rehydrate failed: %s", e)

    yield
    logger.info("Shutting down")
    await close_redis()
    await close_db()


app = FastAPI(
    title=settings.app_name,
    description="Backend proxy & enrichment layer for the Friend or Foe aircraft/drone identification app.",
    version="0.63.3-chomp",
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
# Serve dashboard static files (must be before routers to avoid catch-all conflicts)
_static_dir = Path(__file__).parent / "static"
if _static_dir.is_dir():
    app.mount("/static", StaticFiles(directory=str(_static_dir), html=True), name="static")

@app.exception_handler(RequestValidationError)
async def validation_error_handler(request: Request, exc: RequestValidationError):
    """Log validation errors with the rejected body for debugging."""
    body = exc.body
    errors = exc.errors()
    # Compact: just the field paths and error types
    err_summary = "; ".join(f"{'.'.join(str(p) for p in e.get('loc', []))}: {e.get('msg', '')}" for e in errors[:3])
    logger.warning("Validation 400 from %s on %s: %s", request.client.host if request.client else "?", request.url.path, err_summary)
    if body and isinstance(body, dict):
        logger.debug("Rejected payload keys: %s device_id=%s", list(body.keys()), body.get("device_id") or body.get("dev"))
    return JSONResponse(status_code=400, content={"detail": errors})

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
        version="0.63.3-chomp",
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
        "dashboard": "/dashboard",
        "threats": "/threats",
    }


# Convenience shortcuts for the static dashboards so operators don't have
# to remember the `/static/*.html` path. Both pages live under /static/ too.
@app.get("/dashboard", include_in_schema=False)
async def dashboard_page():
    from fastapi.responses import FileResponse
    return FileResponse(_static_dir / "dashboard.html")


@app.get("/threats", include_in_schema=False)
async def threats_page():
    from fastapi.responses import FileResponse
    return FileResponse(_static_dir / "threats.html")
