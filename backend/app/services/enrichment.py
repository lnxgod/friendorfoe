"""Aircraft enrichment service — registration, type, photos, route info."""

import logging
import re

import httpx

from app.config import settings
from app.models.schemas import AircraftDetail, AircraftPhoto, RouteInfo

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# ICAO hex → registration country prefix (simplified lookup)
# Full table has thousands of entries; this covers the most common blocks.
# ---------------------------------------------------------------------------
_ICAO_COUNTRY_RANGES: list[tuple[int, int, str]] = [
    (0xA00000, 0xAFFFFF, "United States"),
    (0xC00000, 0xC3FFFF, "Canada"),
    (0x400000, 0x43FFFF, "United Kingdom"),
    (0x3C0000, 0x3FFFFF, "Germany"),
    (0x380000, 0x3BFFFF, "France"),
    (0x300000, 0x33FFFF, "Italy"),
    (0x340000, 0x37FFFF, "Spain"),
    (0x840000, 0x87FFFF, "China"),
    (0x780000, 0x7BFFFF, "Japan"),
    (0x7C0000, 0x7FFFFF, "Australia"),
    (0x500000, 0x5003FF, "Israel"),
    (0x710000, 0x717FFF, "South Korea"),
    (0x480000, 0x4BFFFF, "Netherlands"),
    (0x440000, 0x447FFF, "Austria"),
    (0x4C0000, 0x4FFFFF, "Belgium"),
    (0x0C0000, 0x0FFFFF, "Mexico"),
    (0xE00000, 0xE3FFFF, "Brazil"),
    (0x600000, 0x6003FF, "Russia"),
    (0x880000, 0x887FFF, "India"),
]


def _country_from_icao(icao_hex: str) -> str | None:
    """Derive registration country from ICAO hex address."""
    try:
        val = int(icao_hex, 16)
    except ValueError:
        return None
    for low, high, country in _ICAO_COUNTRY_RANGES:
        if low <= val <= high:
            return country
    return None


# ---------------------------------------------------------------------------
# Callsign → airline / route (heuristic)
# ---------------------------------------------------------------------------

# Common ICAO airline prefixes → (airline_name, IATA code)
_AIRLINE_PREFIXES: dict[str, tuple[str, str]] = {
    "AAL": ("American Airlines", "AA"),
    "UAL": ("United Airlines", "UA"),
    "DAL": ("Delta Air Lines", "DL"),
    "SWA": ("Southwest Airlines", "WN"),
    "JBU": ("JetBlue Airways", "B6"),
    "SKW": ("SkyWest Airlines", "OO"),
    "ASA": ("Alaska Airlines", "AS"),
    "NKS": ("Spirit Airlines", "NK"),
    "FFT": ("Frontier Airlines", "F9"),
    "RPA": ("Republic Airways", "YX"),
    "ENY": ("Envoy Air", "MQ"),
    "BAW": ("British Airways", "BA"),
    "DLH": ("Lufthansa", "LH"),
    "AFR": ("Air France", "AF"),
    "KLM": ("KLM Royal Dutch", "KL"),
    "EZY": ("easyJet", "U2"),
    "RYR": ("Ryanair", "FR"),
    "QFA": ("Qantas", "QF"),
    "ACA": ("Air Canada", "AC"),
    "ANZ": ("Air New Zealand", "NZ"),
    "SIA": ("Singapore Airlines", "SQ"),
    "CPA": ("Cathay Pacific", "CX"),
    "ANA": ("All Nippon Airways", "NH"),
    "JAL": ("Japan Airlines", "JL"),
    "UAE": ("Emirates", "EK"),
    "ETH": ("Ethiopian Airlines", "ET"),
    "THY": ("Turkish Airlines", "TK"),
    "CSN": ("China Southern", "CZ"),
    "CCA": ("Air China", "CA"),
    "CES": ("China Eastern", "MU"),
}


def _parse_callsign(callsign: str | None) -> RouteInfo | None:
    """Extract airline and flight number from an ICAO-style callsign."""
    if not callsign:
        return None
    cs = callsign.strip().upper()
    if len(cs) < 4:
        return None

    # Try to match 3-letter ICAO prefix + numeric flight number
    match = re.match(r"^([A-Z]{3})(\d+[A-Z]?)$", cs)
    if not match:
        return None

    prefix = match.group(1)
    flight_num = match.group(2)
    airline_info = _AIRLINE_PREFIXES.get(prefix)

    return RouteInfo(
        airline=airline_info[0] if airline_info else None,
        airline_iata=airline_info[1] if airline_info else None,
        flight_number=flight_num,
        origin=None,  # Would require a flight-route database
        destination=None,
    )


# ---------------------------------------------------------------------------
# Photo lookup via Planespotters.net public API
# ---------------------------------------------------------------------------

async def _fetch_photo(icao_hex: str) -> AircraftPhoto | None:
    """Fetch aircraft photo from Planespotters.net public API."""
    url = f"{settings.planespotters_base_url}/{icao_hex}"
    try:
        async with httpx.AsyncClient(timeout=10.0) as client:
            resp = await client.get(url)
            if resp.status_code != 200:
                logger.debug("Planespotters returned %d for %s", resp.status_code, icao_hex)
                return None
            data = resp.json()
        photos = data.get("photos", [])
        if not photos:
            return None
        p = photos[0]
        return AircraftPhoto(
            url=p.get("link"),
            photographer=p.get("photographer"),
            thumbnail_url=p.get("thumbnail", {}).get("src") if isinstance(p.get("thumbnail"), dict) else p.get("thumbnail_large", {}).get("src"),
        )
    except Exception:
        logger.warning("Photo fetch failed for %s", icao_hex, exc_info=True)
        return None


# ---------------------------------------------------------------------------
# Main enrichment entry point
# ---------------------------------------------------------------------------

async def get_aircraft_detail(
    icao_hex: str,
    callsign: str | None = None,
) -> AircraftDetail:
    """Build a full AircraftDetail by combining static lookups and API calls.

    In a production system the registration/type data would come from a local
    database (e.g., the OpenSky aircraft metadata CSV or a FAA registration DB).
    For v1 we rely on the photo API (which often includes type info) and
    heuristic callsign parsing.
    """
    icao_clean = icao_hex.strip().lower()
    country = _country_from_icao(icao_clean)
    route = _parse_callsign(callsign)
    photo = await _fetch_photo(icao_clean)

    return AircraftDetail(
        icao_hex=icao_clean,
        callsign=callsign.strip() if callsign else None,
        registration=None,  # requires local DB — deferred
        aircraft_type=None,  # requires local DB — deferred
        aircraft_description=None,
        operator=route.airline if route else None,
        photo=photo,
        route=route,
        country=country,
    )
