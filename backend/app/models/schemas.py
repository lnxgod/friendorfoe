"""Pydantic v2 models for API request/response schemas."""

from typing import Any

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


# ---------------------------------------------------------------------------
# Drone detections (ESP32 sensor ingestion)
# ---------------------------------------------------------------------------

class DroneDetectionItem(BaseModel):
    """A single drone detection from an ESP32 sensor node."""

    drone_id: str = Field(..., description="Drone serial number or generated identifier")
    source: str = Field(
        ...,
        description="Detection source: ble_rid, wifi_ssid, wifi_dji_ie, wifi_beacon_rid, wifi_oui",
    )
    confidence: float = Field(ge=0.0, le=1.0, description="Raw detection confidence 0.0-1.0")
    latitude: float | None = Field(None, description="Drone latitude (WGS84 degrees)")
    longitude: float | None = Field(None, description="Drone longitude (WGS84 degrees)")
    altitude_m: float | None = Field(None, description="Altitude in meters MSL")
    heading_deg: float | None = Field(None, description="Heading 0-360 degrees true north")
    speed_mps: float | None = Field(None, description="Ground speed in m/s")
    rssi: int | None = Field(None, description="Signal strength in dBm")
    manufacturer: str | None = Field(None, description="Drone manufacturer (e.g. DJI, Skydio)")
    model: str | None = Field(None, description="Drone model name")
    operator_lat: float | None = Field(None, description="Operator latitude (WGS84 degrees)")
    operator_lon: float | None = Field(None, description="Operator longitude (WGS84 degrees)")
    operator_id: str | None = Field(None, description="Operator registration ID")
    ssid: str | None = Field(None, description="WiFi SSID if detected via WiFi")
    bssid: str | None = Field(None, description="WiFi BSSID (MAC address)")
    channel: int | None = Field(None, description="WiFi channel if available from the scanner")
    # BLE fingerprinting fields (from ESP32 scanner)
    ble_company_id: int | None = Field(None, description="BLE company ID (0x004C=Apple, 0x0075=Samsung, etc.)")
    ble_apple_type: int | None = Field(None, description="Apple Continuity sub-type (0x07=AirPods, 0x10=NearbyInfo, 0x12=FindMy)")
    ble_ad_type_count: int | None = Field(None, description="Number of distinct AD types in advertisement")
    ble_payload_len: int | None = Field(None, description="Raw BLE advertisement payload length")
    ble_addr_type: int | None = Field(None, description="BLE address type (0=public, 1=random static, 2=RPA)")
    ble_ja3: str | None = Field(None, description="BLE-JA3 structural profile hash (same for all devices of same model)")
    probed_ssids: list[str] | None = Field(None, description="SSIDs this device is probing for (from probe requests)")


class DroneDetectionBatch(BaseModel):
    """Batch of drone detections from a single ESP32 sensor node."""

    device_id: str = Field(..., description="Unique identifier for the ESP32 sensor device")
    device_lat: float | None = Field(None, description="Sensor device latitude")
    device_lon: float | None = Field(None, description="Sensor device longitude")
    device_alt: float | None = Field(None, description="Sensor device altitude in meters")
    timestamp: int = Field(..., description="Batch timestamp (epoch seconds)")
    firmware_version: str | None = Field(None, description="Firmware version (e.g. 0.35.0)")
    board_type: str | None = Field(None, description="Board type (uplink-esp32, uplink-c3)")
    scanners: list[dict] | None = Field(None, description="Connected scanner identities [{uart, ver, board, chip, caps}]")
    detections: list[DroneDetectionItem] = Field(
        ..., description="List of drone detections in this batch"
    )


class DroneDetectionResponse(BaseModel):
    """Response for POST /detections/drones."""

    status: str = "ok"
    accepted: int = Field(..., description="Number of detections accepted")
    device_id: str = Field(..., description="Echo of the submitting device ID")


class StoredDetection(DroneDetectionItem):
    """A detection stored in the ring buffer with ingestion metadata."""

    device_id: str = Field(..., description="Source device ID")
    device_lat: float | None = Field(None, description="Source device latitude")
    device_lon: float | None = Field(None, description="Source device longitude")
    received_at: float = Field(..., description="Server receive timestamp (epoch seconds)")
    classification: str | None = Field(None, description="Device classification: confirmed_drone, likely_drone, test_drone, possible_drone, unknown_device, known_ap, tracker")


class RecentDetectionsResponse(BaseModel):
    """Response for GET /detections/drones/recent."""

    count: int = Field(..., description="Number of detections returned")
    max_stored: int = Field(..., description="Maximum ring buffer capacity")
    detections: list[StoredDetection] = Field(default_factory=list)


class AnomalyAlertItem(BaseModel):
    """Single RF anomaly alert emitted by the in-memory detector."""

    anomaly_type: str
    severity: str
    entity_key: str
    title: str
    message: str
    detected_at: float
    device_id: str
    source: str
    drone_id: str | None = None
    ssid: str | None = None
    bssid: str | None = None
    manufacturer: str | None = None
    metadata: dict[str, Any] = Field(default_factory=dict)


class RecentAnomalyAlertsResponse(BaseModel):
    """Response for GET /detections/anomalies/recent."""

    count: int = Field(..., description="Number of alerts returned")
    max_stored: int = Field(..., description="Maximum in-memory alert capacity")
    alerts: list[AnomalyAlertItem] = Field(default_factory=list)


# ---------------------------------------------------------------------------
# Sensor map (triangulated drone positions)
# ---------------------------------------------------------------------------

class SensorObservation(BaseModel):
    """A single sensor's observation of a drone, for map display."""

    device_id: str
    sensor_lat: float
    sensor_lon: float
    rssi: int | None = None
    estimated_distance_m: float | None = None
    confidence: float = 0.0
    source: str = ""
    ssid: str | None = None


class LocatedDroneItem(BaseModel):
    """A drone with estimated or known position, for map display."""

    drone_id: str = Field(..., description="Drone serial number or generated ID")
    lat: float = Field(..., description="Estimated or known latitude")
    lon: float = Field(..., description="Estimated or known longitude")
    alt: float | None = Field(None, description="Altitude in meters MSL")
    heading_deg: float | None = Field(None, description="Heading 0-360 degrees")
    speed_mps: float | None = Field(None, description="Ground speed m/s")
    position_source: str = Field(
        ...,
        description="How position was determined: gps, trilateration, intersection, range_only",
    )
    accuracy_m: float | None = Field(None, description="Estimated position accuracy in meters")
    range_m: float | None = Field(
        None, description="For range_only: RSSI-estimated distance from sensor (radius of range circle)"
    )
    sensor_count: int = Field(..., description="Number of sensors observing this drone")
    confidence: float = Field(0.0, description="Best detection confidence across sensors")
    manufacturer: str | None = None
    model: str | None = None
    operator_lat: float | None = None
    operator_lon: float | None = None
    operator_id: str | None = None
    observations: list[SensorObservation] = Field(default_factory=list)
    classification: str | None = Field(None, description="Device classification: confirmed_drone, likely_drone, test_drone, possible_drone, unknown_device, known_ap, tracker")


class SensorItem(BaseModel):
    """An ESP32 sensor node."""

    device_id: str = Field(..., description="Unique sensor identifier")
    lat: float = Field(..., description="Sensor latitude")
    lon: float = Field(..., description="Sensor longitude")
    alt: float | None = Field(None, description="Sensor altitude in meters")
    last_seen: float = Field(..., description="Last heartbeat epoch seconds")
    online: bool = Field(True, description="True if reported within the last 120s")


class DroneMapResponse(BaseModel):
    """Response for GET /detections/drones/map."""

    drone_count: int = Field(..., description="Number of located drones")
    sensor_count: int = Field(..., description="Number of active sensors")
    drones: list[LocatedDroneItem] = Field(default_factory=list)
    sensors: list[SensorItem] = Field(default_factory=list)


class SensorsResponse(BaseModel):
    """Response for GET /detections/sensors."""

    count: int = Field(..., description="Number of active sensors")
    sensors: list[SensorItem] = Field(default_factory=list)


# ---------------------------------------------------------------------------
# Node management
# ---------------------------------------------------------------------------

class NodeCreateRequest(BaseModel):
    """Request to register a sensor node at a fixed position."""

    device_id: str = Field(..., description="Unique device identifier (matches ESP32 device_id)")
    name: str = Field("", description="Human-friendly name for this node (e.g. 'Garage', 'Rooftop')")
    lat: float = Field(..., description="Fixed latitude (WGS84 degrees)")
    lon: float = Field(..., description="Fixed longitude (WGS84 degrees)")
    alt: float | None = Field(None, description="Fixed altitude in meters MSL")
    sensor_type: str = Field("outdoor", description="Sensor environment: 'indoor' or 'outdoor'. Indoor uses higher path loss.")


class NodeUpdateRequest(BaseModel):
    """Request to update a sensor node's position or name."""

    name: str | None = Field(None, description="New name (or None to keep existing)")
    lat: float | None = Field(None, description="New latitude (or None to keep existing)")
    lon: float | None = Field(None, description="New longitude (or None to keep existing)")
    alt: float | None = Field(None, description="New altitude (or None to keep existing)")


class NodeResponse(BaseModel):
    """A registered sensor node."""

    device_id: str
    name: str = ""
    lat: float
    lon: float
    alt: float | None = None
    is_fixed: bool = False
    sensor_type: str = "outdoor"
    last_seen: str | None = None
    created_at: str | None = None


class NodeListResponse(BaseModel):
    """Response for GET /nodes."""

    count: int = Field(..., description="Number of registered nodes")
    nodes: list[NodeResponse] = Field(default_factory=list)


# ---------------------------------------------------------------------------
# Detection history
# ---------------------------------------------------------------------------

class DetectionHistoryItem(BaseModel):
    """A historical detection record from PostgreSQL."""

    id: int
    device_id: str
    drone_id: str
    source: str
    ssid: str | None = None
    bssid: str | None = None
    rssi: int | None = None
    confidence: float = 0.0
    drone_lat: float | None = None
    drone_lon: float | None = None
    sensor_lat: float | None = None
    sensor_lon: float | None = None
    manufacturer: str | None = None
    model: str | None = None
    timestamp: int
    received_at: str


class DetectionHistoryResponse(BaseModel):
    """Response for GET /detections/drones/history."""

    count: int = Field(..., description="Number of records returned")
    total: int = Field(..., description="Total matching records")
    detections: list[DetectionHistoryItem] = Field(default_factory=list)


class DroneTrackPoint(BaseModel):
    """A single point in a drone's position track over time."""

    lat: float
    lon: float
    alt: float | None = None
    accuracy_m: float | None = None
    position_source: str
    sensor_count: int = 1
    confidence: float = 0.0
    timestamp: str
    observations_json: str | None = None
    classification: str | None = None
    ssid: str | None = None


class DroneTrackResponse(BaseModel):
    """Response for GET /detections/drones/{drone_id}/track."""

    drone_id: str
    point_count: int
    track: list[DroneTrackPoint] = Field(default_factory=list)
    total_distance_m: float = 0.0
    avg_accuracy_m: float = 0.0
    hours_queried: float = 1.0
