"""FastAPI application entry point."""

import logging
from contextlib import asynccontextmanager

from fastapi import FastAPI

from app.cache import close_redis, redis_ping
from app.config import settings
from app.models.schemas import HealthResponse
from app.routers import aircraft

logging.basicConfig(
    level=logging.DEBUG if settings.debug else logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)
logger = logging.getLogger(__name__)


@asynccontextmanager
async def lifespan(app: FastAPI):
    """Startup/shutdown lifecycle hooks."""
    logger.info("Starting %s", settings.app_name)
    yield
    logger.info("Shutting down — closing Redis connection")
    await close_redis()


app = FastAPI(
    title=settings.app_name,
    description="Backend proxy & enrichment layer for the Friend or Foe aircraft/drone identification app.",
    version="0.1.0",
    lifespan=lifespan,
)

# ---------------------------------------------------------------------------
# Routers
# ---------------------------------------------------------------------------
app.include_router(aircraft.router)


# ---------------------------------------------------------------------------
# Health check
# ---------------------------------------------------------------------------
@app.get("/health", response_model=HealthResponse, tags=["system"])
async def health_check() -> HealthResponse:
    """Liveness / readiness probe."""
    redis_ok = await redis_ping()
    return HealthResponse(
        status="ok",
        version="0.1.0",
        redis="ok" if redis_ok else "unavailable",
        database="not_configured",  # will wire up when PostgreSQL models are added
    )


@app.get("/", tags=["system"])
async def root():
    """Root redirect / welcome."""
    return {
        "app": settings.app_name,
        "docs": "/docs",
        "health": "/health",
    }
