"""Multi-source ADS-B client with Redis caching.

Fallback chain: adsb.fi → airplanes.live → ADSB One → adsb.lol → OpenSky Network.
"""

import logging
import math
import time

import httpx

from app.cache import (
    get_cached_aircraft,
    get_cached_aircraft_point,
    set_cached_aircraft,
    set_cached_aircraft_point,
)
from app.config import settings
from app.models.schemas import AircraftPosition

logger = logging.getLogger(__name__)

# Conversion constants
FT_TO_M = 0.3048
KTS_TO_MS = 0.5144
FPM_TO_MS = 0.00508

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


def _parse_adsbx_aircraft(ac: dict) -> AircraftPosition | None:
    """Parse a single ADSBx v2 format aircraft dict into an AircraftPosition.

    Used by both adsb.fi and airplanes.live responses.
    Key conversions: alt_baro (ft→m), gs (knots→m/s), baro_rate (fpm→m/s).
    """
    hex_code = ac.get("hex")
    if not hex_code:
        return None

    lat = ac.get("lat")
    lon = ac.get("lon")

    # alt_baro can be an int (feet) or the string "ground"
    alt_baro_raw = ac.get("alt_baro")
    if alt_baro_raw == "ground" or alt_baro_raw == "Ground":
        baro_alt_m = 0.0
        on_ground = True
    elif alt_baro_raw is not None:
        try:
            baro_alt_m = float(alt_baro_raw) * FT_TO_M
        except (ValueError, TypeError):
            baro_alt_m = None
        on_ground = False
    else:
        baro_alt_m = None
        on_ground = False

    # alt_geom: feet → meters
    alt_geom_raw = ac.get("alt_geom")
    geo_alt_m = float(alt_geom_raw) * FT_TO_M if alt_geom_raw is not None else None

    # gs: knots → m/s
    gs_raw = ac.get("gs")
    velocity_ms = float(gs_raw) * KTS_TO_MS if gs_raw is not None else None

    # baro_rate: fpm → m/s
    baro_rate_raw = ac.get("baro_rate")
    vert_rate_ms = float(baro_rate_raw) * FPM_TO_MS if baro_rate_raw is not None else None

    callsign = ac.get("flight")
    if callsign:
        callsign = callsign.strip() or None

    return AircraftPosition(
        icao_hex=hex_code.strip().lower(),
        callsign=callsign,
        latitude=lat,
        longitude=lon,
        baro_altitude_m=baro_alt_m,
        geo_altitude_m=geo_alt_m,
        heading=ac.get("track"),
        velocity_ms=velocity_ms,
        vertical_rate_ms=vert_rate_ms,
        on_ground=on_ground,
        last_contact=int(time.time()) if ac.get("seen") is None else int(time.time() - ac.get("seen", 0)),
    )


async def fetch_from_adsbfi(
    lat: float, lon: float, radius_nm: float
) -> list[AircraftPosition]:
    """Fetch aircraft from adsb.fi within a radius of a point."""
    url = f"{settings.adsbfi_base_url}/v3/lat/{lat}/lon/{lon}/dist/{int(radius_nm)}"

    async with httpx.AsyncClient(timeout=10.0) as client:
        resp = await client.get(url)
        resp.raise_for_status()
        data = resp.json()

    ac_list = data.get("ac") or []
    aircraft = []
    for ac in ac_list:
        parsed = _parse_adsbx_aircraft(ac)
        if parsed is not None:
            aircraft.append(parsed)

    logger.info("adsb.fi returned %d raw, parsed %d aircraft", len(ac_list), len(aircraft))
    return aircraft


async def fetch_from_airplanes_live(
    lat: float, lon: float, radius_nm: float
) -> list[AircraftPosition]:
    """Fetch aircraft from airplanes.live within a radius of a point."""
    url = f"{settings.airplanes_live_base_url}/v2/point/{lat}/{lon}/{int(radius_nm)}"

    async with httpx.AsyncClient(timeout=10.0) as client:
        resp = await client.get(url)
        resp.raise_for_status()
        data = resp.json()

    ac_list = data.get("ac") or []
    aircraft = []
    for ac in ac_list:
        parsed = _parse_adsbx_aircraft(ac)
        if parsed is not None:
            aircraft.append(parsed)

    logger.info("airplanes.live returned %d raw, parsed %d aircraft", len(ac_list), len(aircraft))
    return aircraft


async def fetch_from_adsb_one(
    lat: float, lon: float, radius_nm: float
) -> list[AircraftPosition]:
    """Fetch aircraft from ADSB One within a radius of a point."""
    url = f"{settings.adsb_one_base_url}/v2/point/{lat}/{lon}/{int(radius_nm)}"

    async with httpx.AsyncClient(timeout=10.0) as client:
        resp = await client.get(url)
        resp.raise_for_status()
        data = resp.json()

    ac_list = data.get("ac") or []
    aircraft = []
    for ac in ac_list:
        parsed = _parse_adsbx_aircraft(ac)
        if parsed is not None:
            aircraft.append(parsed)

    logger.info("ADSB One returned %d raw, parsed %d aircraft", len(ac_list), len(aircraft))
    return aircraft


async def fetch_from_adsb_lol(
    lat: float, lon: float, radius_nm: float
) -> list[AircraftPosition]:
    """Fetch aircraft from adsb.lol within a radius of a point."""
    url = f"{settings.adsb_lol_base_url}/v2/point/{lat}/{lon}/{int(radius_nm)}"

    async with httpx.AsyncClient(timeout=10.0) as client:
        resp = await client.get(url)
        resp.raise_for_status()
        data = resp.json()

    ac_list = data.get("ac") or []
    aircraft = []
    for ac in ac_list:
        parsed = _parse_adsbx_aircraft(ac)
        if parsed is not None:
            aircraft.append(parsed)

    logger.info("adsb.lol returned %d raw, parsed %d aircraft", len(ac_list), len(aircraft))
    return aircraft


async def fetch_aircraft_multi(
    lat: float, lon: float, radius_nm: float
) -> tuple[list[AircraftPosition], str]:
    """Fetch aircraft using the multi-source fallback chain.

    Tries: adsb.fi → airplanes.live → ADSB One → adsb.lol → OpenSky (bbox).
    Returns (aircraft_list, source_name).
    """
    # --- Cache check ---
    cached = await get_cached_aircraft_point(lat, lon, radius_nm)
    if cached is not None:
        logger.debug("Returning %d cached aircraft (point query)", len(cached))
        return [AircraftPosition(**ac) for ac in cached], "cache"

    last_exc: Exception | None = None

    # 1. Try adsb.fi
    try:
        aircraft = await fetch_from_adsbfi(lat, lon, radius_nm)
        cache_payload = [ac.model_dump() for ac in aircraft]
        await set_cached_aircraft_point(lat, lon, radius_nm, cache_payload)
        return aircraft, "adsb.fi"
    except Exception as exc:
        logger.warning("adsb.fi failed: %s", exc)
        last_exc = exc

    # 2. Try airplanes.live
    try:
        aircraft = await fetch_from_airplanes_live(lat, lon, radius_nm)
        cache_payload = [ac.model_dump() for ac in aircraft]
        await set_cached_aircraft_point(lat, lon, radius_nm, cache_payload)
        return aircraft, "airplanes.live"
    except Exception as exc:
        logger.warning("airplanes.live failed: %s", exc)
        last_exc = exc

    # 3. Try ADSB One
    try:
        aircraft = await fetch_from_adsb_one(lat, lon, radius_nm)
        cache_payload = [ac.model_dump() for ac in aircraft]
        await set_cached_aircraft_point(lat, lon, radius_nm, cache_payload)
        return aircraft, "adsb.one"
    except Exception as exc:
        logger.warning("ADSB One failed: %s", exc)
        last_exc = exc

    # 4. Try adsb.lol
    try:
        aircraft = await fetch_from_adsb_lol(lat, lon, radius_nm)
        cache_payload = [ac.model_dump() for ac in aircraft]
        await set_cached_aircraft_point(lat, lon, radius_nm, cache_payload)
        return aircraft, "adsb.lol"
    except Exception as exc:
        logger.warning("adsb.lol failed: %s", exc)
        last_exc = exc

    # 5. Try OpenSky (existing bbox logic)
    try:
        lamin, lamax, lomin, lomax = bbox_from_point(lat, lon, radius_nm)
        aircraft = await fetch_aircraft_in_bbox(lamin, lamax, lomin, lomax)
        # Also cache under point key for consistency
        cache_payload = [ac.model_dump() for ac in aircraft]
        await set_cached_aircraft_point(lat, lon, radius_nm, cache_payload)
        return aircraft, "opensky"
    except Exception as exc:
        logger.error("All ADS-B sources failed. Last error: %s", exc)
        last_exc = exc

    raise last_exc  # type: ignore[misc]


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
