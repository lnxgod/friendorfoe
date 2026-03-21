"""Drone detection ingestion endpoints for ESP32 sensor nodes."""

import logging
import time
from collections import deque
from typing import Annotated

from fastapi import APIRouter, Query

from app.models.schemas import (
    DroneDetectionBatch,
    DroneDetectionResponse,
    DroneMapResponse,
    LocatedDroneItem,
    RecentDetectionsResponse,
    SensorItem,
    SensorObservation,
    SensorsResponse,
    StoredDetection,
)
from app.services.triangulation import SensorTracker

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/detections", tags=["detections"])

# ---------------------------------------------------------------------------
# In-memory ring buffer for recent detections
# Future: Replace with Redis or PostgreSQL for persistence and horizontal scaling
# ---------------------------------------------------------------------------

_MAX_RECENT = 1000
_recent_detections: deque[StoredDetection] = deque(maxlen=_MAX_RECENT)

# ---------------------------------------------------------------------------
# Multi-sensor tracker (triangulation engine)
# ---------------------------------------------------------------------------

_sensor_tracker = SensorTracker()


# ---------------------------------------------------------------------------
# POST /detections/drones — ingest a batch of drone detections from an ESP32
# ---------------------------------------------------------------------------

@router.post("/drones", response_model=DroneDetectionResponse)
async def ingest_drone_detections(batch: DroneDetectionBatch) -> DroneDetectionResponse:
    """
    Receive a batch of drone detections from an ESP32 sensor node.

    The sensor sends periodic batches containing all currently-tracked
    drones.  Each detection includes the detection source (BLE Remote ID,
    WiFi SSID pattern, DJI vendor IE, etc.) and optional position data.

    Detections are stored in a ring buffer AND fed to the multi-sensor
    triangulation engine for cross-sensor position estimation.
    """
    received_at = time.time()

    logger.info(
        "Drone detection batch from device=%s ts=%d count=%d pos=(%s, %s, %s)",
        batch.device_id,
        batch.timestamp,
        len(batch.detections),
        batch.device_lat,
        batch.device_lon,
        batch.device_alt,
    )

    accepted = 0
    for det in batch.detections:
        logger.info(
            "  [%s] drone_id=%s src=%s conf=%.2f rssi=%s pos=(%s, %s) alt=%s "
            "heading=%s speed=%s mfr=%s model=%s ssid=%s bssid=%s",
            batch.device_id,
            det.drone_id,
            det.source,
            det.confidence,
            det.rssi,
            det.latitude,
            det.longitude,
            det.altitude_m,
            det.heading_deg,
            det.speed_mps,
            det.manufacturer,
            det.model,
            det.ssid,
            det.bssid,
        )

        # Store in ring buffer for the /recent endpoint
        stored = StoredDetection(
            device_id=batch.device_id,
            device_lat=batch.device_lat,
            device_lon=batch.device_lon,
            received_at=received_at,
            **det.model_dump(),
        )
        _recent_detections.append(stored)

        # Feed into multi-sensor tracker for triangulation
        _sensor_tracker.ingest(
            device_id=batch.device_id,
            device_lat=batch.device_lat,
            device_lon=batch.device_lon,
            device_alt=batch.device_alt,
            drone_id=det.drone_id,
            rssi=det.rssi,
            drone_lat=det.latitude,
            drone_lon=det.longitude,
            drone_alt=det.altitude_m,
            heading_deg=det.heading_deg,
            speed_mps=det.speed_mps,
            confidence=det.confidence,
            source=det.source,
            manufacturer=det.manufacturer,
            model=det.model,
            ssid=det.ssid,
            bssid=det.bssid,
            operator_lat=det.operator_lat,
            operator_lon=det.operator_lon,
            operator_id=det.operator_id,
        )

        accepted += 1

    logger.info(
        "Accepted %d/%d detections from device=%s",
        accepted,
        len(batch.detections),
        batch.device_id,
    )

    return DroneDetectionResponse(
        status="ok",
        accepted=accepted,
        device_id=batch.device_id,
    )


# ---------------------------------------------------------------------------
# GET /detections/drones/map — triangulated + range-estimated drone positions
# ---------------------------------------------------------------------------

@router.get("/drones/map", response_model=DroneMapResponse)
async def get_drone_map() -> DroneMapResponse:
    """
    Return all currently-tracked drones with estimated positions.

    Position estimation priority:
    1. **GPS** — drone reports its own position (Remote ID, DJI IE)
    2. **Trilateration** — 3+ sensors, nonlinear least-squares multilateration
    3. **Intersection** — 2 sensors, circle-circle intersection
    4. **Range only** — 1 sensor, RSSI-estimated range circle

    Also returns all active sensor positions for map display.
    """
    located = _sensor_tracker.get_located_drones()
    sensors = _sensor_tracker.get_active_sensors()

    drone_items = []
    for d in located:
        obs_items = [
            SensorObservation(
                device_id=o.device_id,
                sensor_lat=o.sensor_lat,
                sensor_lon=o.sensor_lon,
                rssi=o.rssi,
                estimated_distance_m=o.estimated_distance_m,
                confidence=o.confidence,
                source=o.source,
            )
            for o in d.observations
        ]
        drone_items.append(LocatedDroneItem(
            drone_id=d.drone_id,
            lat=d.lat,
            lon=d.lon,
            alt=d.alt,
            heading_deg=d.heading_deg,
            speed_mps=d.speed_mps,
            position_source=d.position_source,
            accuracy_m=d.accuracy_m,
            range_m=d.range_m,
            sensor_count=d.sensor_count,
            confidence=d.confidence,
            manufacturer=d.manufacturer,
            model=d.model,
            operator_lat=d.operator_lat,
            operator_lon=d.operator_lon,
            operator_id=d.operator_id,
            observations=obs_items,
        ))

    sensor_items = [
        SensorItem(
            device_id=s.device_id,
            lat=s.lat,
            lon=s.lon,
            alt=s.alt,
            last_seen=s.last_seen,
        )
        for s in sensors
    ]

    return DroneMapResponse(
        drone_count=len(drone_items),
        sensor_count=len(sensor_items),
        drones=drone_items,
        sensors=sensor_items,
    )


# ---------------------------------------------------------------------------
# GET /detections/sensors — active sensor nodes
# ---------------------------------------------------------------------------

@router.get("/sensors", response_model=SensorsResponse)
async def get_active_sensors() -> SensorsResponse:
    """Return all active ESP32 sensor nodes with their positions."""
    sensors = _sensor_tracker.get_active_sensors()
    items = [
        SensorItem(
            device_id=s.device_id,
            lat=s.lat,
            lon=s.lon,
            alt=s.alt,
            last_seen=s.last_seen,
        )
        for s in sensors
    ]
    return SensorsResponse(count=len(items), sensors=items)


# ---------------------------------------------------------------------------
# GET /detections/drones/recent — return last N detections from ring buffer
# ---------------------------------------------------------------------------

@router.get("/drones/recent", response_model=RecentDetectionsResponse)
async def get_recent_detections(
    limit: Annotated[int, Query(ge=1, le=1000, description="Max detections to return")] = 100,
) -> RecentDetectionsResponse:
    """
    Return the most recent drone detections from the in-memory ring buffer.

    Results are ordered newest-first.  The ring buffer holds a maximum of
    1000 detections; older entries are automatically evicted.
    """
    # Slice from the end (newest) and reverse so newest is first
    items = list(_recent_detections)[-limit:]
    items.reverse()

    return RecentDetectionsResponse(
        count=len(items),
        max_stored=_MAX_RECENT,
        detections=items,
    )
