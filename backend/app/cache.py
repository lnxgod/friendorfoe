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


def _point_cache_key(lat: float, lon: float, radius_nm: float) -> str:
    """Deterministic cache key for a point+radius query.

    Rounds lat/lon to 2 decimal places (~1 km) and radius to nearest 5nm
    so that nearby queries share cached results.
    """
    rounded_lat = round(lat, 2)
    rounded_lon = round(lon, 2)
    rounded_radius = round(radius_nm / 5) * 5
    raw = f"{rounded_lat:.2f},{rounded_lon:.2f},{rounded_radius}"
    digest = hashlib.md5(raw.encode()).hexdigest()[:12]
    return f"adsb:point:{digest}"


async def get_cached_aircraft_point(
    lat: float, lon: float, radius_nm: float
) -> list[dict] | None:
    """Return cached aircraft list for a point+radius query, or None on miss."""
    try:
        r = await get_redis()
        key = _point_cache_key(lat, lon, radius_nm)
        raw = await r.get(key)
        if raw is not None:
            logger.debug("Cache HIT for %s", key)
            return json.loads(raw)
        logger.debug("Cache MISS for %s", key)
    except Exception:
        logger.warning("Redis read failed — treating as cache miss", exc_info=True)
    return None


async def set_cached_aircraft_point(
    lat: float,
    lon: float,
    radius_nm: float,
    aircraft: list[dict],
) -> None:
    """Store aircraft list in cache for a point+radius query with configured TTL."""
    try:
        r = await get_redis()
        key = _point_cache_key(lat, lon, radius_nm)
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
