"""Aircraft API endpoints."""

import logging

from fastapi import APIRouter, HTTPException, Query

from app.config import settings
from app.models.schemas import AircraftDetail, NearbyAircraft, NearbyAircraftResponse
from app.services.adsb import (
    bbox_from_point,
    fetch_aircraft_in_bbox,
    haversine_nm,
    meters_to_feet,
    ms_to_fpm,
    ms_to_knots,
)
from app.services.enrichment import get_aircraft_detail

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/aircraft", tags=["aircraft"])


@router.get("/nearby", response_model=NearbyAircraftResponse)
async def nearby_aircraft(
    lat: float = Query(..., ge=-90, le=90, description="User latitude"),
    lon: float = Query(..., ge=-180, le=180, description="User longitude"),
    radius_nm: float = Query(
        default=settings.default_radius_nm,
        ge=1,
        le=settings.max_radius_nm,
        description="Search radius in nautical miles",
    ),
) -> NearbyAircraftResponse:
    """Return aircraft near the given GPS position.

    Calls OpenSky (with Redis caching) then converts units and computes
    distance from the user.
    """
    lamin, lamax, lomin, lomax = bbox_from_point(lat, lon, radius_nm)

    try:
        raw_aircraft = await fetch_aircraft_in_bbox(lamin, lamax, lomin, lomax)
    except Exception as exc:
        logger.error("Failed to fetch aircraft: %s", exc)
        raise HTTPException(status_code=502, detail="ADS-B data source unavailable") from exc

    result: list[NearbyAircraft] = []
    for ac in raw_aircraft:
        # Skip aircraft without position
        if ac.latitude is None or ac.longitude is None:
            continue

        dist = haversine_nm(lat, lon, ac.latitude, ac.longitude)

        # Filter to actual radius (bbox is a square superset)
        if dist > radius_nm:
            continue

        result.append(
            NearbyAircraft(
                icao_hex=ac.icao_hex,
                callsign=ac.callsign,
                latitude=ac.latitude,
                longitude=ac.longitude,
                altitude_ft=meters_to_feet(ac.baro_altitude_m),
                heading=ac.heading,
                speed_kts=ms_to_knots(ac.velocity_ms),
                vertical_rate_fpm=ms_to_fpm(ac.vertical_rate_ms),
                on_ground=ac.on_ground,
                distance_nm=round(dist, 1),
            )
        )

    # Sort by distance
    result.sort(key=lambda a: a.distance_nm if a.distance_nm is not None else float("inf"))

    return NearbyAircraftResponse(
        count=len(result),
        lat=lat,
        lon=lon,
        radius_nm=radius_nm,
        aircraft=result,
    )


@router.get("/{icao_hex}/detail", response_model=AircraftDetail)
async def aircraft_detail(
    icao_hex: str,
    callsign: str | None = Query(None, description="Optional callsign for route lookup"),
) -> AircraftDetail:
    """Return enriched detail for a single aircraft by ICAO hex address."""
    if not icao_hex or len(icao_hex) != 6:
        raise HTTPException(status_code=400, detail="ICAO hex must be a 6-character hex string")

    try:
        int(icao_hex, 16)
    except ValueError:
        raise HTTPException(status_code=400, detail="ICAO hex must be a valid hexadecimal string")

    return await get_aircraft_detail(icao_hex, callsign=callsign)
