"""OpenSky Network ADS-B client with Redis caching."""

import logging
import math

import httpx

from app.cache import get_cached_aircraft, set_cached_aircraft
from app.config import settings
from app.models.schemas import AircraftPosition

logger = logging.getLogger(__name__)

# Nautical mile in degrees latitude (1 nm = 1 arcminute = 1/60 degree)
NM_TO_DEG_LAT = 1.0 / 60.0


def bbox_from_point(lat: float, lon: float, radius_nm: float) -> tuple[float, float, float, float]:
    """Convert a center point + radius (nm) to a bounding box.

    Returns (lamin, lamax, lomin, lomax).
    """
    dlat = radius_nm * NM_TO_DEG_LAT
    # Longitude degrees per nm varies with latitude
    dlon = radius_nm * NM_TO_DEG_LAT / max(math.cos(math.radians(lat)), 0.01)

    lamin = lat - dlat
    lamax = lat + dlat
    lomin = lon - dlon
    lomax = lon + dlon
    return (lamin, lamax, lomin, lomax)


def _parse_state_vector(sv: list) -> AircraftPosition | None:
    """Parse a single OpenSky state vector array into an AircraftPosition.

    OpenSky state vector indices:
        0  icao24         str
        1  callsign       str
        2  origin_country str
        3  time_position  int
        4  last_contact   int
        5  longitude      float
        6  latitude       float
        7  baro_altitude  float (meters)
        8  on_ground      bool
        9  velocity       float (m/s)
       10  true_track     float (degrees)
       11  vertical_rate  float (m/s)
       12  sensors        int[]
       13  geo_altitude   float (meters)
       14  squawk         str
       15  spi            bool
       16  position_source int
    """
    if sv is None or len(sv) < 17:
        return None
    icao = sv[0]
    if not icao:
        return None
    return AircraftPosition(
        icao_hex=icao.strip(),
        callsign=sv[1].strip() if sv[1] else None,
        latitude=sv[6],
        longitude=sv[5],
        baro_altitude_m=sv[7],
        geo_altitude_m=sv[13],
        heading=sv[10],
        velocity_ms=sv[9],
        vertical_rate_ms=sv[11],
        on_ground=bool(sv[8]),
        last_contact=sv[4],
    )


async def fetch_aircraft_in_bbox(
    lamin: float, lamax: float, lomin: float, lomax: float
) -> list[AircraftPosition]:
    """Fetch aircraft from OpenSky within the given bounding box.

    Checks Redis cache first; caches results on miss.
    """
    # --- Cache check ---
    cached = await get_cached_aircraft(lamin, lamax, lomin, lomax)
    if cached is not None:
        return [AircraftPosition(**ac) for ac in cached]

    # --- Call OpenSky ---
    url = f"{settings.opensky_base_url}/states/all"
    params = {
        "lamin": lamin,
        "lamax": lamax,
        "lomin": lomin,
        "lomax": lomax,
    }

    auth = None
    if settings.opensky_username and settings.opensky_password:
        auth = (settings.opensky_username, settings.opensky_password)

    aircraft: list[AircraftPosition] = []
    try:
        async with httpx.AsyncClient(timeout=15.0) as client:
            resp = await client.get(url, params=params, auth=auth)
            resp.raise_for_status()
            data = resp.json()

        states = data.get("states") or []
        for sv in states:
            ac = _parse_state_vector(sv)
            if ac is not None:
                aircraft.append(ac)

        logger.info(
            "OpenSky returned %d state vectors, parsed %d aircraft",
            len(states),
            len(aircraft),
        )
    except httpx.HTTPStatusError as exc:
        logger.error("OpenSky HTTP error %s: %s", exc.response.status_code, exc)
        raise
    except httpx.RequestError as exc:
        logger.error("OpenSky request failed: %s", exc)
        raise

    # --- Cache result ---
    cache_payload = [ac.model_dump() for ac in aircraft]
    await set_cached_aircraft(lamin, lamax, lomin, lomax, cache_payload)

    return aircraft


def haversine_nm(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Haversine distance between two points in nautical miles."""
    R_NM = 3440.065  # Earth radius in nautical miles

    dlat = math.radians(lat2 - lat1)
    dlon = math.radians(lon2 - lon1)
    a = (
        math.sin(dlat / 2) ** 2
        + math.cos(math.radians(lat1))
        * math.cos(math.radians(lat2))
        * math.sin(dlon / 2) ** 2
    )
    c = 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))
    return R_NM * c


def meters_to_feet(m: float | None) -> float | None:
    """Convert meters to feet, returning None if input is None."""
    if m is None:
        return None
    return round(m * 3.28084, 0)


def ms_to_knots(ms: float | None) -> float | None:
    """Convert m/s to knots, returning None if input is None."""
    if ms is None:
        return None
    return round(ms * 1.94384, 1)


def ms_to_fpm(ms: float | None) -> float | None:
    """Convert m/s vertical rate to feet per minute."""
    if ms is None:
        return None
    return round(ms * 196.85, 0)
