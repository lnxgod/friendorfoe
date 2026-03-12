"""Pydantic v2 models for API request/response schemas."""

from pydantic import BaseModel, Field


# ---------------------------------------------------------------------------
# Aircraft core
# ---------------------------------------------------------------------------

class AircraftPosition(BaseModel):
    """Single aircraft position from ADS-B data."""

    icao_hex: str = Field(..., description="ICAO 24-bit transponder address (hex)")
    callsign: str | None = Field(None, description="Flight callsign, e.g. 'UAL123'")
    latitude: float | None = Field(None, description="Latitude in decimal degrees")
    longitude: float | None = Field(None, description="Longitude in decimal degrees")
    baro_altitude_m: float | None = Field(None, description="Barometric altitude in meters")
    geo_altitude_m: float | None = Field(None, description="Geometric (GPS) altitude in meters")
    heading: float | None = Field(None, description="True track in degrees clockwise from north")
    velocity_ms: float | None = Field(None, description="Ground speed in m/s")
    vertical_rate_ms: float | None = Field(None, description="Vertical rate in m/s (positive = climbing)")
    on_ground: bool = Field(False, description="Whether the aircraft is on the ground")
    last_contact: int | None = Field(None, description="Unix timestamp of last contact")


class NearbyAircraft(BaseModel):
    """Aircraft with enrichment fields for the nearby endpoint."""

    icao_hex: str = Field(..., description="ICAO 24-bit transponder address (hex)")
    callsign: str | None = Field(None, description="Flight callsign")
    latitude: float | None = None
    longitude: float | None = None
    altitude_ft: float | None = Field(None, description="Altitude in feet (converted from meters)")
    heading: float | None = Field(None, description="True track in degrees")
    speed_kts: float | None = Field(None, description="Ground speed in knots")
    vertical_rate_fpm: float | None = Field(None, description="Vertical rate in feet per minute")
    on_ground: bool = False
    distance_nm: float | None = Field(None, description="Distance from user in nautical miles")
    aircraft_type: str | None = Field(None, description="Aircraft type designator, e.g. 'B738'")
    registration: str | None = Field(None, description="Aircraft registration, e.g. 'N12345'")


class NearbyAircraftResponse(BaseModel):
    """Response for GET /aircraft/nearby."""

    count: int = Field(..., description="Number of aircraft returned")
    lat: float = Field(..., description="User latitude")
    lon: float = Field(..., description="User longitude")
    radius_nm: float = Field(..., description="Search radius in nautical miles")
    aircraft: list[NearbyAircraft] = Field(default_factory=list)


# ---------------------------------------------------------------------------
# Aircraft detail / enrichment
# ---------------------------------------------------------------------------

class AircraftPhoto(BaseModel):
    """Aircraft photo from enrichment API."""

    url: str | None = Field(None, description="Photo URL")
    photographer: str | None = Field(None, description="Photo credit")
    thumbnail_url: str | None = Field(None, description="Thumbnail URL")


class RouteInfo(BaseModel):
    """Route information decoded from callsign."""

    airline: str | None = Field(None, description="Airline name")
    airline_iata: str | None = Field(None, description="IATA airline code, e.g. 'UA'")
    flight_number: str | None = Field(None, description="Flight number, e.g. '123'")
    origin: str | None = Field(None, description="Origin airport IATA code")
    destination: str | None = Field(None, description="Destination airport IATA code")


class AircraftDetail(BaseModel):
    """Full aircraft detail response."""

    icao_hex: str
    callsign: str | None = None
    registration: str | None = None
    aircraft_type: str | None = Field(None, description="ICAO type designator, e.g. 'B738'")
    aircraft_description: str | None = Field(None, description="Human-readable type, e.g. 'Boeing 737-800'")
    operator: str | None = Field(None, description="Aircraft operator / airline")
    photo: AircraftPhoto | None = None
    route: RouteInfo | None = None
    country: str | None = Field(None, description="Registration country")


# ---------------------------------------------------------------------------
# Health
# ---------------------------------------------------------------------------

class HealthResponse(BaseModel):
    """Health check response."""

    status: str = "ok"
    version: str = "0.1.0"
    redis: str = "unknown"
    database: str = "unknown"
