"""Aircraft enrichment service — registration, type, photos, route info.

Enrichment chain:
  Aircraft data: hexdb.io (real DB) → heuristic fallbacks
  Photos: planespotters.net → airport-data.com → hexdb thumbnail → placeholder
  Routes: hexdb.io route API → callsign heuristic
"""

import asyncio
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
# Country → registration prefix mapping
# ---------------------------------------------------------------------------
_COUNTRY_REG_PREFIX: dict[str, str] = {
    "United States": "N",
    "Canada": "C-",
    "United Kingdom": "G-",
    "Germany": "D-",
    "France": "F-",
    "Italy": "I-",
    "Spain": "EC-",
    "China": "B-",
    "Japan": "JA",
    "Australia": "VH-",
    "Israel": "4X-",
    "South Korea": "HL",
    "Netherlands": "PH-",
    "Austria": "OE-",
    "Belgium": "OO-",
    "Mexico": "XA-",
    "Brazil": "PT-",
    "Russia": "RA-",
    "India": "VT-",
}


def _registration_from_icao(icao_hex: str, country: str | None) -> str | None:
    """Construct a partial registration from country prefix and ICAO hex suffix."""
    if not country:
        return None
    prefix = _COUNTRY_REG_PREFIX.get(country)
    if not prefix:
        return None
    # Use the lower 16 bits of the ICAO hex as a numeric suffix to build a
    # plausible (but not guaranteed-accurate) partial registration.
    try:
        val = int(icao_hex, 16)
    except ValueError:
        return None
    suffix = f"{val & 0xFFFF:05d}"
    return f"{prefix}{suffix}"


# ---------------------------------------------------------------------------
# Airline ICAO code → common fleet types (simplified heuristic)
# ---------------------------------------------------------------------------
_AIRLINE_FLEET_TYPES: dict[str, list[str]] = {
    "AAL": ["A321", "B738", "B789", "B77W"],
    "UAL": ["B739", "B77W", "B789", "A320"],
    "DAL": ["B739", "A321", "B764", "A339"],
    "SWA": ["B737", "B738", "B38M"],
    "JBU": ["A320", "A321", "E190"],
    "SKW": ["E175", "CRJ7", "CRJ9"],
    "ASA": ["B739", "E175", "B38M"],
    "NKS": ["A320", "A321"],
    "FFT": ["A320", "A321"],
    "RPA": ["E175", "E170"],
    "ENY": ["E175", "E145", "CRJ7"],
    "BAW": ["A320", "B772", "B789", "A388"],
    "DLH": ["A320", "A321", "B748", "A359"],
    "AFR": ["A320", "A321", "B77W", "A388"],
    "KLM": ["B738", "B772", "B789", "A330"],
    "EZY": ["A319", "A320"],
    "RYR": ["B738", "B38M"],
    "QFA": ["A332", "B789", "A388", "B738"],
    "ACA": ["A320", "B77W", "B789", "A333"],
    "ANZ": ["B789", "A321", "ATR72"],
    "SIA": ["A388", "B77W", "A359", "B789"],
    "CPA": ["A350", "B77W", "A321"],
    "ANA": ["B77W", "B789", "A321", "B738"],
    "JAL": ["B77W", "B789", "A350", "B738"],
    "UAE": ["B77W", "A388"],
    "ETH": ["B789", "B77W", "A350"],
    "THY": ["A321", "B738", "B77W", "A333"],
    "CSN": ["A320", "B738", "B789", "A333"],
    "CCA": ["A320", "B738", "B77W", "A332"],
    "CES": ["A320", "B738", "B789", "A332"],
}


def _aircraft_type_from_callsign(callsign: str | None) -> str | None:
    """Infer a likely aircraft type from the airline ICAO prefix in the callsign.

    Returns the first (most common) fleet type for the airline. This is a rough
    heuristic — the actual type depends on the specific airframe, not the airline.
    """
    if not callsign:
        return None
    cs = callsign.strip().upper()
    if len(cs) < 4:
        return None
    prefix = cs[:3]
    fleet = _AIRLINE_FLEET_TYPES.get(prefix)
    if fleet:
        return fleet[0]
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

_PLACEHOLDER_PHOTO = AircraftPhoto(
    url="https://via.placeholder.com/400x300?text=No+Photo+Available",
    photographer="N/A",
    thumbnail_url=None,
)


async def _fetch_planespotters_photo(icao_hex: str) -> AircraftPhoto | None:
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
        logger.warning("Planespotters photo fetch failed for %s", icao_hex, exc_info=True)
        return None


async def _fetch_airport_data_photo(icao_hex: str) -> AircraftPhoto | None:
    """Fetch aircraft photo from airport-data.com."""
    url = f"https://airport-data.com/api/ac_thumb.json?m={icao_hex}&n=1"
    try:
        async with httpx.AsyncClient(timeout=10.0) as client:
            resp = await client.get(url)
            if resp.status_code != 200:
                return None
            data = resp.json()
        image = data.get("data", [{}])[0] if isinstance(data.get("data"), list) and data.get("data") else None
        if not image:
            return None
        photo_url = image.get("image")
        if not photo_url:
            return None
        return AircraftPhoto(
            url=photo_url,
            photographer=image.get("photographer"),
            thumbnail_url=image.get("image"),
        )
    except Exception:
        logger.warning("airport-data.com photo fetch failed for %s", icao_hex, exc_info=True)
        return None


async def _fetch_photo(icao_hex: str) -> AircraftPhoto:
    """Fetch aircraft photo using fallback chain.

    Chain: planespotters.net → airport-data.com → hexdb thumbnail → placeholder.
    """
    # 1. Planespotters.net
    photo = await _fetch_planespotters_photo(icao_hex)
    if photo is not None:
        return photo

    # 2. airport-data.com
    photo = await _fetch_airport_data_photo(icao_hex)
    if photo is not None:
        return photo

    # 3. hexdb.io thumbnail
    hexdb_thumb = f"https://hexdb.io/hex-image-thumb?hex={icao_hex.upper()}"
    try:
        async with httpx.AsyncClient(timeout=5.0) as client:
            resp = await client.head(hexdb_thumb)
            if resp.status_code == 200:
                return AircraftPhoto(
                    url=hexdb_thumb,
                    photographer="hexdb.io",
                    thumbnail_url=hexdb_thumb,
                )
    except Exception:
        pass

    return _PLACEHOLDER_PHOTO


# ---------------------------------------------------------------------------
# Main enrichment entry point
# ---------------------------------------------------------------------------

async def _fetch_hexdb(icao_hex: str) -> dict | None:
    """Fetch real aircraft data from hexdb.io.

    Returns dict with keys: registration, type_code, manufacturer, type, owner.
    """
    url = f"https://hexdb.io/api/v1/aircraft/{icao_hex.upper()}"
    try:
        async with httpx.AsyncClient(timeout=8.0) as client:
            resp = await client.get(url)
            if resp.status_code != 200:
                return None
            data = resp.json()
        reg = data.get("Registration")
        if not reg:
            return None
        return {
            "registration": reg,
            "type_code": data.get("ICAOTypeCode"),
            "manufacturer": data.get("Manufacturer"),
            "type": data.get("Type"),
            "owner": data.get("RegisteredOwners"),
        }
    except Exception:
        logger.debug("hexdb.io lookup failed for %s", icao_hex, exc_info=True)
        return None


async def _fetch_hexdb_route(callsign: str) -> str | None:
    """Fetch route string from hexdb.io (e.g. 'KJFK-EGLL')."""
    url = f"https://hexdb.io/api/v1/route/icao/{callsign.strip().upper()}"
    try:
        async with httpx.AsyncClient(timeout=8.0) as client:
            resp = await client.get(url)
            if resp.status_code != 200:
                return None
            data = resp.json()
        return data.get("route")
    except Exception:
        return None


async def get_aircraft_detail(
    icao_hex: str,
    callsign: str | None = None,
) -> AircraftDetail:
    """Build a full AircraftDetail by combining hexdb.io lookups, photo APIs,
    and heuristic fallbacks.

    Enrichment chain:
      Aircraft data: hexdb.io (real DB) → heuristic fallbacks
      Photos: planespotters → airport-data → hexdb thumbnail → placeholder
      Routes: hexdb.io route → callsign heuristic
    """
    icao_clean = icao_hex.strip().lower()
    country = _country_from_icao(icao_clean)

    # Fetch hexdb.io data and photo concurrently
    hexdb_task = asyncio.create_task(_fetch_hexdb(icao_clean))
    photo_task = asyncio.create_task(_fetch_photo(icao_clean))
    route_task = asyncio.create_task(_fetch_hexdb_route(callsign)) if callsign else None

    hexdb = await hexdb_task
    photo = await photo_task
    hexdb_route_str = await route_task if route_task else None

    # Aircraft data: prefer hexdb.io, fall back to heuristics
    if hexdb:
        registration = hexdb["registration"]
        aircraft_type = hexdb["type_code"]
        aircraft_description = hexdb["type"]
        operator = hexdb["owner"]
    else:
        registration = _registration_from_icao(icao_clean, country)
        aircraft_type = _aircraft_type_from_callsign(callsign)
        aircraft_description = None
        operator = None

    # Route: parse hexdb route string into origin/destination, merge with callsign heuristic
    route = _parse_callsign(callsign)
    if hexdb_route_str and "-" in hexdb_route_str:
        parts = hexdb_route_str.split("-", 1)
        if route:
            route.origin = parts[0].strip()
            route.destination = parts[1].strip()
        else:
            route = RouteInfo(
                airline=None,
                airline_iata=None,
                flight_number=None,
                origin=parts[0].strip(),
                destination=parts[1].strip(),
            )

    # Use hexdb owner as operator, fallback to airline from callsign
    if not operator and route:
        operator = route.airline

    return AircraftDetail(
        icao_hex=icao_clean,
        callsign=callsign.strip() if callsign else None,
        registration=registration,
        aircraft_type=aircraft_type,
        aircraft_description=aircraft_description,
        operator=operator,
        photo=photo,
        route=route,
        country=country,
    )
