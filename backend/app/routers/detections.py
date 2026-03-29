"""Drone detection ingestion endpoints for ESP32 sensor nodes."""

import json
import logging
import time
from collections import deque
from datetime import datetime, timezone
from typing import Annotated

from fastapi import APIRouter, Depends, Query, Request
from sqlalchemy import func, select
from sqlalchemy.ext.asyncio import AsyncSession

from app.models.db_models import DroneDetection, SensorNode, TriangulatedPosition
from app.models.schemas import (
    AnomalyAlertItem,
    DetectionHistoryItem,
    DetectionHistoryResponse,
    DroneDetectionBatch,
    DroneDetectionResponse,
    DroneMapResponse,
    DroneTrackPoint,
    DroneTrackResponse,
    LocatedDroneItem,
    RecentAnomalyAlertsResponse,
    RecentDetectionsResponse,
    SensorItem,
    SensorObservation,
    SensorsResponse,
    StoredDetection,
)
from app.services.database import get_db
from app.services.rf_anomaly import RFAnomalyDetector, RFDetectionEvent
from app.services.signal_tracker import SignalEvent, SignalTracker
from app.services.triangulation import SensorTracker
from app.services.anomaly_detector import AnomalyDetector
from app.services.enrichment_ble import BLEEnricher

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/detections", tags=["detections"])

# ---------------------------------------------------------------------------
# In-memory ring buffer for recent detections (fast path, no DB needed)
# ---------------------------------------------------------------------------

_MAX_RECENT = 1000
_recent_detections: deque[StoredDetection] = deque(maxlen=_MAX_RECENT)

# ---------------------------------------------------------------------------
# In-memory node heartbeat tracker (works without DB)
# ---------------------------------------------------------------------------

_node_heartbeats: dict[str, dict] = {}

# ---------------------------------------------------------------------------
# Multi-sensor tracker (triangulation engine)
# ---------------------------------------------------------------------------

_sensor_tracker = SensorTracker()
_rf_anomaly_detector = RFAnomalyDetector()
_anomaly_detector = AnomalyDetector()
_ble_enricher = BLEEnricher()
_signal_tracker = SignalTracker()


async def _resolve_sensor_position(
    device_id: str,
    device_lat: float | None,
    device_lon: float | None,
    device_alt: float | None,
    db: AsyncSession | None,
) -> tuple[float | None, float | None, float | None]:
    """
    Look up sensor node in DB. If it's a registered fixed node, use the
    registered position (ignoring GPS from payload). Otherwise use GPS.
    """
    if db:
        try:
            result = await db.execute(
                select(SensorNode).where(SensorNode.device_id == device_id)
            )
            node = result.scalar_one_or_none()
            if node and node.is_fixed:
                # Update last_seen timestamp
                node.last_seen = datetime.now(timezone.utc)
                await db.commit()
                return node.lat, node.lon, node.alt
            elif node and not node.is_fixed:
                # Dynamic node — update position from GPS
                if device_lat is not None and device_lon is not None:
                    node.lat = device_lat
                    node.lon = device_lon
                    node.alt = device_alt
                    node.last_seen = datetime.now(timezone.utc)
                    await db.commit()
                return device_lat, device_lon, device_alt
        except Exception as e:
            logger.warning("DB lookup failed for %s: %s", device_id, e)

    return device_lat, device_lon, device_alt


# ---------------------------------------------------------------------------
# POST /detections/drones — ingest a batch from an ESP32
# ---------------------------------------------------------------------------

@router.post("/drones", response_model=DroneDetectionResponse)
async def ingest_drone_detections(
    batch: DroneDetectionBatch,
    request: Request,
    db: AsyncSession = Depends(get_db),
) -> DroneDetectionResponse:
    """
    Receive a batch of drone detections from an ESP32 sensor node.

    If the device_id matches a registered fixed node, the registered
    GPS position is used (overriding any GPS from the payload). Otherwise
    the GPS from the payload is used directly.

    Each detection is:
    1. Stored in PostgreSQL for historical logging
    2. Kept in an in-memory ring buffer for fast recent queries
    3. Fed to the triangulation engine for multi-sensor position estimation
    """
    # Use the batch timestamp from the ESP32 (scan time), not server receive time.
    # This groups detections from the same scan cycle correctly.
    received_at = batch.timestamp if batch.timestamp and batch.timestamp > 1700000000 else time.time()

    # Track heartbeat (works without DB)
    source_ip = request.client.host if request.client else None
    _node_heartbeats[batch.device_id] = {
        "device_id": batch.device_id,
        "last_seen": received_at,
        "detection_count": len(batch.detections),
        "total_batches": _node_heartbeats.get(batch.device_id, {}).get("total_batches", 0) + 1,
        "total_detections": _node_heartbeats.get(batch.device_id, {}).get("total_detections", 0) + len(batch.detections),
        "lat": batch.device_lat,
        "lon": batch.device_lon,
        "ip": source_ip,
    }

    # Resolve sensor position (fixed node overrides GPS)
    sensor_lat, sensor_lon, sensor_alt = await _resolve_sensor_position(
        batch.device_id, batch.device_lat, batch.device_lon, batch.device_alt, db
    )

    logger.info(
        "Drone batch from device=%s count=%d sensor_pos=(%s, %s)",
        batch.device_id, len(batch.detections), sensor_lat, sensor_lon,
    )

    accepted = 0
    db_detections = []

    for det in batch.detections:
        # Ring buffer
        stored = StoredDetection(
            device_id=batch.device_id,
            device_lat=sensor_lat,
            device_lon=sensor_lon,
            received_at=received_at,
            **det.model_dump(),
        )
        _recent_detections.append(stored)

        # Anomaly detection (fingerprint-aware, whitelisted, mesh-aware)
        _anomaly_detector.ingest(
            drone_id=det.drone_id, source=det.source,
            confidence=det.confidence, rssi=det.rssi or 0,
            ssid=det.ssid or "", bssid=det.bssid or "",
            manufacturer=det.manufacturer or "", model=det.model or "",
            device_id=batch.device_id, received_at=received_at,
        )

        # BLE enrichment
        _ble_enricher.ingest(
            drone_id=det.drone_id, source=det.source,
            confidence=det.confidence, rssi=det.rssi or 0,
            bssid=det.bssid or "", manufacturer=det.manufacturer or "",
            model=det.model or "", device_id=batch.device_id,
            received_at=received_at,
        )

        alerts = _rf_anomaly_detector.process_event(
            RFDetectionEvent(
                drone_id=det.drone_id,
                source=det.source,
                confidence=det.confidence,
                rssi=det.rssi,
                ssid=det.ssid,
                bssid=det.bssid,
                manufacturer=det.manufacturer,
                device_id=batch.device_id,
                received_at=received_at,
                channel=det.channel,
            )
        )
        _signal_tracker.ingest(
            SignalEvent(
                drone_id=det.drone_id,
                source=det.source,
                confidence=det.confidence,
                rssi=det.rssi,
                ssid=det.ssid,
                bssid=det.bssid,
                manufacturer=det.manufacturer,
                device_id=batch.device_id,
                received_at=received_at,
                channel=det.channel,
            )
        )
        for alert in alerts:
            logger.info(
                "RF anomaly type=%s severity=%s entity=%s device=%s",
                alert.anomaly_type,
                alert.severity,
                alert.entity_key,
                alert.device_id,
            )

        # Anomaly detection
        _anomaly_detector.ingest(
            drone_id=det.drone_id, source=det.source,
            confidence=det.confidence, rssi=det.rssi or 0,
            ssid=det.ssid or "", bssid=det.bssid or "",
            manufacturer=det.manufacturer or "",
            device_id=batch.device_id, received_at=received_at,
        )

        # PostgreSQL
        db_det = DroneDetection(
            device_id=batch.device_id,
            drone_id=det.drone_id,
            source=det.source,
            ssid=det.ssid,
            bssid=det.bssid,
            rssi=det.rssi,
            confidence=det.confidence,
            drone_lat=det.latitude,
            drone_lon=det.longitude,
            drone_alt=det.altitude_m,
            speed_mps=det.speed_mps,
            heading_deg=det.heading_deg,
            manufacturer=det.manufacturer,
            model=det.model,
            operator_lat=det.operator_lat,
            operator_lon=det.operator_lon,
            operator_id=det.operator_id,
            sensor_lat=sensor_lat,
            sensor_lon=sensor_lon,
            sensor_alt=sensor_alt,
            timestamp=batch.timestamp,
        )
        db_detections.append(db_det)

        # Triangulation engine
        _sensor_tracker.ingest(
            device_id=batch.device_id,
            device_lat=sensor_lat,
            device_lon=sensor_lon,
            device_alt=sensor_alt,
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

    # Bulk insert to PostgreSQL
    try:
        db.add_all(db_detections)

        # Also store triangulated positions
        located = _sensor_tracker.get_located_drones()
        for d in located:
            tri = TriangulatedPosition(
                drone_id=d.drone_id,
                lat=d.lat,
                lon=d.lon,
                alt=d.alt,
                accuracy_m=d.accuracy_m,
                position_source=d.position_source,
                sensor_count=d.sensor_count,
                confidence=d.confidence,
                manufacturer=d.manufacturer,
                model=d.model,
                ssid=getattr(d, 'ssid', None),
                observations_json=json.dumps([
                    {"device_id": o.device_id, "rssi": o.rssi, "distance_m": o.estimated_distance_m}
                    for o in d.observations
                ]),
            )
            db.add(tri)

        await db.commit()
    except Exception as e:
        logger.warning("DB write failed (detections still in memory): %s", e)

    return DroneDetectionResponse(
        status="ok", accepted=accepted, device_id=batch.device_id
    )


# ---------------------------------------------------------------------------
# GET /detections/drones/map — triangulated drone positions
# ---------------------------------------------------------------------------

@router.get("/drones/map", response_model=DroneMapResponse)
async def get_drone_map() -> DroneMapResponse:
    """Return all currently-tracked drones with estimated positions."""
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
            device_id=s.device_id, lat=s.lat, lon=s.lon, alt=s.alt, last_seen=s.last_seen,
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
        SensorItem(device_id=s.device_id, lat=s.lat, lon=s.lon, alt=s.alt, last_seen=s.last_seen)
        for s in sensors
    ]
    return SensorsResponse(count=len(items), sensors=items)


# ---------------------------------------------------------------------------
# GET /detections/nodes/status — in-memory heartbeat tracker (no DB needed)
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# GET /detections/devices/live — clean enriched device view
# ---------------------------------------------------------------------------

@router.get("/devices/live")
async def get_live_devices(
    min_confidence: Annotated[float, Query(ge=0.0, le=1.0)] = 0.0,
    trackers_only: Annotated[bool, Query()] = False,
):
    """Return enriched, deduplicated device list with RSSI stats.

    This is the clean view — devices grouped by fingerprint, with
    manufacturer enrichment, classification, and RSSI aggregation.
    """
    _ble_enricher.prune_stale()
    devices = _ble_enricher.get_live_devices(
        min_confidence=min_confidence,
        trackers_only=trackers_only,
    )
    summary = _ble_enricher.get_summary()
    return {"devices": devices, "summary": summary}


@router.get("/nodes/status")
async def get_node_status():
    """Return all nodes that have reported in, with heartbeat info."""
    now = time.time()
    nodes = []
    for node in _node_heartbeats.values():
        age = now - node["last_seen"]
        nodes.append({
            **node,
            "age_s": round(age, 1),
            "online": age < 120,
        })
    nodes.sort(key=lambda n: n["last_seen"], reverse=True)
    return {"count": len(nodes), "nodes": nodes}


# ---------------------------------------------------------------------------
# GET /detections/anomalies — RF anomaly alerts
# ---------------------------------------------------------------------------

@router.get("/anomalies")
async def get_anomalies(
    limit: Annotated[int, Query(ge=1, le=200)] = 50,
    severity: Annotated[str | None, Query(description="Filter: info, warning, critical")] = None,
    alert_type: Annotated[str | None, Query(description="Filter: rssi_spike, disappearance, new_device, velocity, spoofing, signal_anomaly")] = None,
):
    """Return RF anomaly alerts from the detection engine."""
    return {
        "alerts": _anomaly_detector.get_alerts(limit, severity, alert_type),
        "stats": _anomaly_detector.get_stats(),
        "whitelist": _anomaly_detector.get_whitelist(),
    }


@router.get("/anomalies/whitelist")
async def get_whitelist():
    """Return current whitelist (SSID patterns + BSSIDs)."""
    return _anomaly_detector.get_whitelist()


@router.post("/anomalies/whitelist")
async def add_whitelist(body: dict):
    """Add an entry to the whitelist. Body: {"pattern": "...", "type": "ssid"|"bssid"}"""
    pattern = body.get("pattern", "")
    wl_type = body.get("type", "ssid")
    if not pattern:
        return {"error": "pattern is required"}
    _anomaly_detector.add_whitelist(pattern, wl_type)
    return {"ok": True, "whitelist": _anomaly_detector.get_whitelist()}


@router.delete("/anomalies/whitelist")
async def remove_whitelist(body: dict):
    """Remove an entry from the whitelist."""
    pattern = body.get("pattern", "")
    wl_type = body.get("type", "ssid")
    _anomaly_detector.remove_whitelist(pattern, wl_type)
    return {"ok": True, "whitelist": _anomaly_detector.get_whitelist()}


@router.get("/anomalies/devices")
async def get_tracked_devices():
    """Return all tracked devices with RSSI stats and velocity."""
    return {
        "devices": _anomaly_detector.get_tracked_devices(),
        "stats": _anomaly_detector.get_stats(),
    }


# ---------------------------------------------------------------------------
# GET /detections/drones/recent — ring buffer (fast, in-memory)
# ---------------------------------------------------------------------------

@router.get("/drones/recent", response_model=RecentDetectionsResponse)
async def get_recent_detections(
    limit: Annotated[int, Query(ge=1, le=1000)] = 100,
    min_confidence: Annotated[float, Query(ge=0.0, le=1.0, description="Min confidence filter (0=show all)")] = 0.0,
    source: Annotated[str | None, Query(description="Filter by source type")] = None,
) -> RecentDetectionsResponse:
    """Return the most recent drone detections from the in-memory ring buffer.

    Use min_confidence=0 to see ALL SSIDs (debug mode).
    Use min_confidence=0.1 to filter out low-confidence noise.
    """
    items = list(_recent_detections)
    if min_confidence > 0:
        items = [d for d in items if d.confidence >= min_confidence]
    if source:
        items = [d for d in items if d.source == source]
    items = items[-limit:]
    items.reverse()
    return RecentDetectionsResponse(count=len(items), max_stored=_MAX_RECENT, detections=items)


# ---------------------------------------------------------------------------
# GET /detections/tracks/live — smoothed RSSI / distance / approach view
# ---------------------------------------------------------------------------

@router.get("/tracks/live")
async def get_live_tracks(
    limit: Annotated[int, Query(ge=1, le=500)] = 100,
    active_within_s: Annotated[float, Query(ge=0.25, le=60.0)] = 5.0,
):
    """Return active RSSI tracks with smoothed values and motion estimates."""
    return _signal_tracker.get_live_tracks(limit=limit, active_within_s=active_within_s)


# ---------------------------------------------------------------------------
# GET /detections/anomalies/recent — in-memory anomaly alert ring buffer
# ---------------------------------------------------------------------------

@router.get("/anomalies/recent", response_model=RecentAnomalyAlertsResponse)
async def get_recent_anomaly_alerts(
    limit: Annotated[int, Query(ge=1, le=500)] = 100,
) -> RecentAnomalyAlertsResponse:
    """Return the most recent RF anomaly alerts emitted by the detector."""
    alerts = [
        AnomalyAlertItem(
            anomaly_type=alert.anomaly_type,
            severity=alert.severity,
            entity_key=alert.entity_key,
            title=alert.title,
            message=alert.message,
            detected_at=alert.detected_at,
            device_id=alert.device_id,
            source=alert.source,
            drone_id=alert.drone_id,
            ssid=alert.ssid,
            bssid=alert.bssid,
            manufacturer=alert.manufacturer,
            metadata=alert.metadata,
        )
        for alert in _rf_anomaly_detector.get_recent_alerts(limit=limit)
    ]
    return RecentAnomalyAlertsResponse(
        count=len(alerts),
        max_stored=_rf_anomaly_detector.config.recent_alert_limit,
        alerts=alerts,
    )


# ---------------------------------------------------------------------------
# GET /detections/drones/history — PostgreSQL paginated history
# ---------------------------------------------------------------------------

@router.get("/drones/history", response_model=DetectionHistoryResponse)
async def get_detection_history(
    hours: Annotated[float, Query(ge=0.1, le=720, description="Hours of history to return")] = 1.0,
    limit: Annotated[int, Query(ge=1, le=10000)] = 500,
    source: Annotated[str | None, Query(description="Filter by source (wifi_ssid, ble_rid, etc.)")] = None,
    db: AsyncSession = Depends(get_db),
) -> DetectionHistoryResponse:
    """Query historical detections from PostgreSQL with time range and source filter."""
    import datetime as dt

    cutoff = datetime.now(timezone.utc) - dt.timedelta(hours=hours)

    query = select(DroneDetection).where(DroneDetection.received_at >= cutoff)
    if source:
        query = query.where(DroneDetection.source == source)
    query = query.order_by(DroneDetection.id.desc()).limit(limit)

    result = await db.execute(query)
    detections = result.scalars().all()

    # Total count
    count_query = select(func.count(DroneDetection.id)).where(DroneDetection.received_at >= cutoff)
    if source:
        count_query = count_query.where(DroneDetection.source == source)
    total = (await db.execute(count_query)).scalar() or 0

    items = [
        DetectionHistoryItem(
            id=d.id,
            device_id=d.device_id,
            drone_id=d.drone_id,
            source=d.source,
            ssid=d.ssid,
            bssid=d.bssid,
            rssi=d.rssi,
            confidence=d.confidence,
            drone_lat=d.drone_lat,
            drone_lon=d.drone_lon,
            sensor_lat=d.sensor_lat,
            sensor_lon=d.sensor_lon,
            manufacturer=d.manufacturer,
            model=d.model,
            timestamp=d.timestamp,
            received_at=d.received_at.isoformat() if d.received_at else "",
        )
        for d in detections
    ]

    return DetectionHistoryResponse(count=len(items), total=total, detections=items)


# ---------------------------------------------------------------------------
# GET /detections/drones/{drone_id}/track — position history for one drone
# ---------------------------------------------------------------------------

@router.get("/drones/{drone_id}/track", response_model=DroneTrackResponse)
async def get_drone_track(
    drone_id: str,
    hours: Annotated[float, Query(ge=0.1, le=720)] = 1.0,
    db: AsyncSession = Depends(get_db),
) -> DroneTrackResponse:
    """Get triangulated position history for a specific drone over time."""
    import datetime as dt

    cutoff = datetime.now(timezone.utc) - dt.timedelta(hours=hours)

    result = await db.execute(
        select(TriangulatedPosition)
        .where(TriangulatedPosition.drone_id == drone_id)
        .where(TriangulatedPosition.created_at >= cutoff)
        .order_by(TriangulatedPosition.created_at.asc())
    )
    positions = result.scalars().all()

    track = [
        DroneTrackPoint(
            lat=p.lat,
            lon=p.lon,
            alt=p.alt,
            accuracy_m=p.accuracy_m,
            position_source=p.position_source,
            sensor_count=p.sensor_count,
            confidence=p.confidence,
            timestamp=p.timestamp.isoformat() if p.timestamp else "",
        )
        for p in positions
    ]

    return DroneTrackResponse(drone_id=drone_id, point_count=len(track), track=track)
