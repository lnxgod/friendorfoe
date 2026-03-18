"""Drone detection ingestion endpoints for ESP32 sensor nodes."""

import logging
import time
from collections import deque
from typing import Annotated

from fastapi import APIRouter, Query

from app.models.schemas import (
    DroneDetectionBatch,
    DroneDetectionResponse,
    RecentDetectionsResponse,
    StoredDetection,
)

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/detections", tags=["detections"])

# ---------------------------------------------------------------------------
# In-memory ring buffer for recent detections
# Future: Replace with Redis or PostgreSQL for persistence and horizontal scaling
# ---------------------------------------------------------------------------

_MAX_RECENT = 1000
_recent_detections: deque[StoredDetection] = deque(maxlen=_MAX_RECENT)

# Future: Redis-based rate limiting per device_id
# RATE_LIMIT_WINDOW_SEC = 60
# RATE_LIMIT_MAX_REQUESTS = 120


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
