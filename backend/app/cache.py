"""Redis caching layer (async)."""

import hashlib
import json
import logging

import redis.asyncio as aioredis

from app.config import settings

logger = logging.getLogger(__name__)

# Module-level connection pool — initialized lazily.
_redis: aioredis.Redis | None = None


async def get_redis() -> aioredis.Redis:
    """Return a shared async Redis client, creating it on first call."""
    global _redis
    if _redis is None:
        _redis = aioredis.from_url(
            settings.redis_url,
            decode_responses=True,
        )
    return _redis


async def close_redis() -> None:
    """Shut down the Redis connection pool."""
    global _redis
    if _redis is not None:
        await _redis.aclose()
        _redis = None


def _bbox_cache_key(lamin: float, lamax: float, lomin: float, lomax: float) -> str:
    """Deterministic cache key for a bounding box query.

    Rounds coordinates to 2 decimal places (~1 km) so that nearby
    queries share cached results.
    """
    rounded = f"{lamin:.2f},{lamax:.2f},{lomin:.2f},{lomax:.2f}"
    digest = hashlib.md5(rounded.encode()).hexdigest()[:12]
    return f"adsb:bbox:{digest}"


async def get_cached_aircraft(
    lamin: float, lamax: float, lomin: float, lomax: float
) -> list[dict] | None:
    """Return cached aircraft list for the bounding box, or None on miss."""
    try:
        r = await get_redis()
        key = _bbox_cache_key(lamin, lamax, lomin, lomax)
        raw = await r.get(key)
        if raw is not None:
            logger.debug("Cache HIT for %s", key)
            return json.loads(raw)
        logger.debug("Cache MISS for %s", key)
    except Exception:
        logger.warning("Redis read failed — treating as cache miss", exc_info=True)
    return None


async def set_cached_aircraft(
    lamin: float,
    lamax: float,
    lomin: float,
    lomax: float,
    aircraft: list[dict],
) -> None:
    """Store aircraft list in cache with configured TTL."""
    try:
        r = await get_redis()
        key = _bbox_cache_key(lamin, lamax, lomin, lomax)
        await r.set(key, json.dumps(aircraft), ex=settings.cache_ttl_seconds)
        logger.debug("Cached %d aircraft at %s (TTL=%ds)", len(aircraft), key, settings.cache_ttl_seconds)
    except Exception:
        logger.warning("Redis write failed — proceeding without cache", exc_info=True)


async def redis_ping() -> bool:
    """Return True if Redis is reachable."""
    try:
        r = await get_redis()
        return await r.ping()
    except Exception:
        return False
