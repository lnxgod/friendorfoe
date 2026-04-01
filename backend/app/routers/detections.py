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
from app.services.classifier import classify_detection
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

# Position dedup cache: drone_id → (lat, lon) of last logged position
_position_dedup: dict[str, tuple[float, float]] = {}

# GPS-RSSI spoof detector: drone_id → {gps_positions: [(lat,lon,t)], rssi_values: [(rssi,t)]}
_spoof_tracker: dict[str, dict] = {}

# Drone alert tracker: drone_id → last_seen timestamp
_known_drones: dict[str, float] = {}
_drone_alerts: list[dict] = []  # Persistent drone-specific alerts
_DRONE_REAPPEAR_SEC = 300  # Re-alert if drone gone >5min and comes back

# Lock-on command (polled by uplinks) — legacy global + per-node via drone_tracker
_lockon_command: dict = {"active": False, "channel": 0, "bssid": "", "duration_s": 0, "issued_at": 0}

# Automated drone tracking orchestrator
from app.services.drone_tracker import DroneTracker
_drone_tracker = DroneTracker()

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
) -> tuple[float | None, float | None, float | None, str]:
    """
    Look up sensor node in DB. If it's a registered fixed node, use the
    registered position (ignoring GPS from payload). Otherwise use GPS.
    Returns (lat, lon, alt, sensor_type).
    """
    if db:
        try:
            result = await db.execute(
                select(SensorNode).where(SensorNode.device_id == device_id)
            )
            node = result.scalar_one_or_none()
            if node and node.is_fixed:
                node.last_seen = datetime.now(timezone.utc)
                await db.commit()
                return node.lat, node.lon, node.alt, node.sensor_type
            elif node and not node.is_fixed:
                if device_lat is not None and device_lon is not None:
                    node.lat = device_lat
                    node.lon = device_lon
                    node.alt = device_alt
                    node.last_seen = datetime.now(timezone.utc)
                    await db.commit()
                return device_lat, device_lon, device_alt, node.sensor_type
        except Exception as e:
            logger.warning("DB lookup failed for %s: %s", device_id, e)

    return device_lat, device_lon, device_alt, "outdoor"


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
        "firmware_version": batch.firmware_version or _node_heartbeats.get(batch.device_id, {}).get("firmware_version"),
        "board_type": batch.board_type or _node_heartbeats.get(batch.device_id, {}).get("board_type"),
        "scanners": batch.scanners or _node_heartbeats.get(batch.device_id, {}).get("scanners"),
    }

    # Auto-register new nodes in DB if they have GPS and aren't registered yet
    try:
        result = await db.execute(select(SensorNode).where(SensorNode.device_id == batch.device_id))
        existing_node = result.scalar_one_or_none()
        if not existing_node and batch.device_lat and batch.device_lon and \
           abs(batch.device_lat) > 0.1 and abs(batch.device_lon) > 0.1:
            new_node = SensorNode(
                device_id=batch.device_id,
                name=batch.device_id,
                lat=batch.device_lat,
                lon=batch.device_lon,
                alt=batch.device_alt or 0,
                is_fixed=False,
            )
            db.add(new_node)
            await db.commit()
            logger.info("Auto-registered new node: %s at (%.6f, %.6f)",
                        batch.device_id, batch.device_lat, batch.device_lon)
    except Exception:
        pass  # Don't break ingestion if auto-register fails

    # Resolve sensor position (fixed node overrides GPS)
    sensor_lat, sensor_lon, sensor_alt, sensor_type = await _resolve_sensor_position(
        batch.device_id, batch.device_lat, batch.device_lon, batch.device_alt, db
    )

    logger.info(
        "Drone batch from device=%s count=%d sensor_pos=(%s, %s)",
        batch.device_id, len(batch.detections), sensor_lat, sensor_lon,
    )

    accepted = 0
    db_detections = []

    for det in batch.detections:
        # Classify the detection
        classification, adj_confidence = classify_detection(
            source=det.source,
            confidence=det.confidence,
            ssid=det.ssid,
            manufacturer=det.manufacturer,
            drone_id=det.drone_id,
            model=det.model,
        )

        # Ring buffer
        stored = StoredDetection(
            device_id=batch.device_id,
            device_lat=sensor_lat,
            device_lon=sensor_lon,
            received_at=received_at,
            classification=classification,
            **det.model_dump(),
        )
        _recent_detections.append(stored)

        # ── Drone-specific alert ─────────────────────────────────────
        if classification in ("confirmed_drone", "likely_drone", "test_drone"):
            last_seen = _known_drones.get(det.drone_id, 0)
            is_new = last_seen == 0 or (received_at - last_seen) > _DRONE_REAPPEAR_SEC
            _known_drones[det.drone_id] = received_at
            if is_new:
                sev_map = {"confirmed_drone": "critical", "likely_drone": "warning", "test_drone": "info"}
                severity = sev_map.get(classification, "info")
                alert = {
                    "alert_type": "drone_detected",
                    "severity": severity,
                    "drone_id": det.drone_id,
                    "classification": classification,
                    "source": det.source,
                    "ssid": det.ssid,
                    "bssid": det.bssid,
                    "rssi": det.rssi,
                    "manufacturer": det.manufacturer,
                    "device_id": batch.device_id,
                    "latitude": det.latitude,
                    "longitude": det.longitude,
                    "timestamp": received_at,
                    "message": f"DRONE DETECTED: {det.drone_id} ({classification.replace('_',' ')}) via {det.source} on {batch.device_id} RSSI={det.rssi}",
                }
                _drone_alerts.append(alert)
                if len(_drone_alerts) > 200:
                    _drone_alerts.pop(0)
                logger.warning("DRONE ALERT [%s]: %s via %s on %s rssi=%s",
                               severity.upper(), det.drone_id, det.source, batch.device_id, det.rssi)

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
            ble_company_id=det.ble_company_id,
            ble_apple_type=det.ble_apple_type,
            ble_ad_type_count=det.ble_ad_type_count,
            ble_payload_len=det.ble_payload_len,
            ble_addr_type=det.ble_addr_type,
            ble_ja3=det.ble_ja3,
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

        # Automated drone tracking — assigns one node to lock on to confirmed drones
        _drone_tracker.on_detection(
            drone_id=det.drone_id, source=det.source,
            confidence=det.confidence, rssi=det.rssi or 0,
            device_id=batch.device_id, classification=classification,
            ssid=det.ssid or "", bssid=det.bssid or "",
            channel=det.channel or 0,
            drone_lat=det.latitude or 0, drone_lon=det.longitude or 0,
            drone_alt=det.altitude_m or 0,
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
            classification=classification,
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
            probed_ssids=json.dumps(det.probed_ssids) if det.probed_ssids else None,
            timestamp=batch.timestamp,
        )
        db_detections.append(db_det)

        # Triangulation engine (use adjusted confidence for test drones)
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
            confidence=adj_confidence,
            source=det.source,
            manufacturer=det.manufacturer,
            model=det.model,
            ssid=det.ssid,
            bssid=det.bssid,
            operator_lat=det.operator_lat,
            operator_lon=det.operator_lon,
            operator_id=det.operator_id,
            sensor_type=sensor_type,
        )

        # ── GPS-RSSI spoof detection ────────────────────────────────
        # Track per-sensor RSSI stability vs GPS movement over time
        if det.latitude and det.longitude and det.latitude != 0 and det.rssi:
            spoof_key = f"{det.drone_id}_{batch.device_id}"
            st = _spoof_tracker.get(spoof_key)
            if st is None:
                st = {"gps": [], "rssi": [], "flagged": False}
                _spoof_tracker[spoof_key] = st
            st["gps"].append((det.latitude, det.longitude, received_at))
            st["rssi"].append((det.rssi, received_at))
            # Keep last 30 from THIS sensor
            if len(st["gps"]) > 30:
                st["gps"] = st["gps"][-30:]
                st["rssi"] = st["rssi"][-30:]
            # Check: GPS moved >50m but THIS sensor's RSSI range <6dB = spoof
            if len(st["gps"]) >= 8 and not st["flagged"]:
                lats = [g[0] for g in st["gps"]]
                lons = [g[1] for g in st["gps"]]
                rssis = [r[0] for r in st["rssi"]]
                gps_spread_m = (((max(lats)-min(lats))*111320)**2 + ((max(lons)-min(lons))*111320*0.78)**2)**0.5
                rssi_range = max(rssis) - min(rssis)
                if gps_spread_m > 50 and rssi_range < 6:
                    st["flagged"] = True
                    logger.warning(
                        "SPOOF: %s on %s — GPS moved %.0fm but RSSI range %ddB (stationary transmitter spoofing GPS)",
                        det.drone_id, batch.device_id, gps_spread_m, rssi_range
                    )

        accepted += 1

    # Bulk insert to PostgreSQL
    try:
        db.add_all(db_detections)

        # Store triangulated positions — skip range_only, deduplicate
        located = _sensor_tracker.get_located_drones()
        for d in located:
            # Skip single-sensor range_only (just sensor position, no real data)
            if d.position_source == "range_only":
                continue
            # Skip if position hasn't moved >1m from last logged position
            last_key = f"_last_pos_{d.drone_id}"
            last_pos = _position_dedup.get(last_key)
            if last_pos:
                dlat = (d.lat - last_pos[0]) * 111320
                dlon = (d.lon - last_pos[1]) * 111320 * 0.78  # cos(37°)
                moved = (dlat**2 + dlon**2) ** 0.5
                if moved < 1.0:
                    continue
            _position_dedup[last_key] = (d.lat, d.lon)

            # Get best observation's SSID
            best_obs = max(d.observations, key=lambda o: o.confidence) if d.observations else None
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
                ssid=best_obs.ssid if best_obs else None,
                classification=classify_detection(
                    source=best_obs.source if best_obs else "",
                    confidence=d.confidence,
                    ssid=best_obs.ssid if best_obs else None,
                    manufacturer=d.manufacturer,
                    drone_id=d.drone_id,
                    model=d.model,
                )[0] if best_obs else None,
                observations_json=json.dumps([
                    {"device_id": o.device_id, "rssi": o.rssi, "distance_m": o.estimated_distance_m,
                     "sensor_lat": o.sensor_lat, "sensor_lon": o.sensor_lon, "ssid": o.ssid}
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
async def get_drone_map(
    classification: Annotated[str | None, Query(description="Filter by classification")] = None,
) -> DroneMapResponse:
    """Return all currently-tracked drones with estimated positions."""
    located = _sensor_tracker.get_located_drones()
    sensors = _sensor_tracker.get_active_sensors()

    drone_items = []
    for d in located:
        # Classify using the best observation's data
        best_obs = max(d.observations, key=lambda o: o.confidence) if d.observations else None
        cls, _ = classify_detection(
            source=best_obs.source if best_obs else "",
            confidence=d.confidence,
            ssid=best_obs.ssid if best_obs else None,
            manufacturer=d.manufacturer,
            drone_id=d.drone_id,
            model=d.model,
        ) if best_obs else ("unknown_device", d.confidence)

        if classification and cls != classification:
            continue

        obs_items = [
            SensorObservation(
                device_id=o.device_id,
                sensor_lat=o.sensor_lat,
                sensor_lon=o.sensor_lon,
                rssi=o.rssi,
                estimated_distance_m=o.estimated_distance_m,
                confidence=o.confidence,
                source=o.source,
                ssid=o.ssid,
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
            classification=cls,
        ))

    sensor_items = [
        SensorItem(
            device_id=s.device_id, lat=s.lat, lon=s.lon, alt=s.alt, last_seen=s.last_seen,
            online=_sensor_tracker.is_sensor_online(s.device_id),
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
# GET /detections/sensors — sensor nodes (includes offline)
# ---------------------------------------------------------------------------

@router.get("/sensors", response_model=SensorsResponse)
async def get_active_sensors() -> SensorsResponse:
    """Return all known ESP32 sensor nodes with their positions and online status."""
    sensors = _sensor_tracker.get_active_sensors(include_offline=True)
    items = [
        SensorItem(
            device_id=s.device_id, lat=s.lat, lon=s.lon, alt=s.alt, last_seen=s.last_seen,
            online=_sensor_tracker.is_sensor_online(s.device_id),
        )
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
async def get_node_status(db: AsyncSession = Depends(get_db)):
    """Return all nodes that have reported in, with heartbeat info and registered GPS positions."""
    now = time.time()

    # Load registered GPS positions from DB
    db_positions: dict[str, dict] = {}
    try:
        from app.models.db_models import SensorNode
        result = await db.execute(select(SensorNode))
        for sn in result.scalars().all():
            db_positions[sn.device_id] = {
                "lat": sn.lat, "lon": sn.lon, "alt": sn.alt,
                "name": sn.name, "is_fixed": sn.is_fixed,
            }
    except Exception:
        pass  # DB unavailable, use heartbeat data only

    nodes = []
    for node in _node_heartbeats.values():
        age = now - node["last_seen"]
        entry = {**node, "age_s": round(age, 1), "online": age < 120}
        # Override lat/lon with registered DB position if available
        db_pos = db_positions.get(node["device_id"])
        if db_pos and db_pos.get("is_fixed"):
            entry["lat"] = db_pos["lat"]
            entry["lon"] = db_pos["lon"]
            entry["alt"] = db_pos.get("alt", 0)
            entry["name"] = db_pos.get("name", "")
            entry["gps_registered"] = True
        else:
            entry["gps_registered"] = False
        nodes.append(entry)
    nodes.sort(key=lambda n: n["last_seen"], reverse=True)
    return {"count": len(nodes), "nodes": nodes}


# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# POST /detections/lockon — tell sensors to lock onto a target
# ---------------------------------------------------------------------------

@router.post("/lockon")
async def lockon_target(
    channel: int = 6,
    bssid: str | None = None,
    duration_s: int = 60,
):
    """Tell all sensor nodes to lock onto a specific WiFi channel for intensive capture.

    The backend stores the lock-on command; uplinks poll for it and forward to scanners.
    Duration: 30, 60, or 90 seconds.
    """
    if duration_s not in (30, 60, 90):
        duration_s = 60
    if channel < 1 or channel > 13:
        return {"error": "Channel must be 1-13"}

    _lockon_command.update({
        "active": True,
        "channel": channel,
        "bssid": bssid or "",
        "duration_s": duration_s,
        "issued_at": time.time(),
    })

    logger.warning("LOCK-ON issued: ch=%d bssid=%s duration=%ds", channel, bssid or "*", duration_s)
    return {"status": "ok", "lockon": _lockon_command}


@router.delete("/lockon")
async def cancel_lockon():
    """Cancel any active lock-on command."""
    _lockon_command["active"] = False
    return {"status": "cancelled"}


@router.get("/lockon")
async def get_lockon_status(
    device_id: Annotated[str | None, Query(description="Node ID for per-node lock-on")] = None,
):
    """Check current lock-on status (polled by uplinks).

    If device_id is provided, returns per-node auto-tracking command from the
    drone tracker. Falls back to the global manual lock-on command.
    """
    # Per-node auto-tracking (from drone_tracker)
    if device_id:
        cmd = _drone_tracker.get_node_command(device_id)
        if cmd.get("active"):
            return cmd

    # Fallback: global manual lock-on
    if _lockon_command.get("active"):
        elapsed = time.time() - _lockon_command.get("issued_at", 0)
        if elapsed > _lockon_command.get("duration_s", 60):
            _lockon_command["active"] = False
    return _lockon_command


@router.get("/tracking")
async def get_tracking_status():
    """Return automated drone tracking status — active sessions, history."""
    return _drone_tracker.get_status()


# GET /detections/drone-alerts — drone-specific alerts (high priority)
# ---------------------------------------------------------------------------

@router.get("/drone-alerts")
async def get_drone_alerts():
    """Return drone-specific alerts — confirmed drones, likely drones, test drones.

    These are top-priority alerts that should be prominently displayed.
    """
    now = time.time()
    # Active drones = seen in last 120s
    active_drones = {did: ts for did, ts in _known_drones.items() if now - ts < 120}
    # Build active drone info from current map data
    located = _sensor_tracker.get_located_drones()
    active_info = []
    for did, last_ts in sorted(active_drones.items(), key=lambda x: -x[1]):
        drone = next((d for d in located if d.drone_id == did), None)
        info = {
            "drone_id": did,
            "last_seen": last_ts,
            "age_s": round(now - last_ts, 1),
            "active": True,
        }
        if drone:
            best_obs = max(drone.observations, key=lambda o: o.confidence) if drone.observations else None
            info.update({
                "lat": drone.lat, "lon": drone.lon,
                "position_source": drone.position_source,
                "sensor_count": drone.sensor_count,
                "accuracy_m": drone.accuracy_m,
                "classification": classify_detection(
                    source=best_obs.source if best_obs else "",
                    confidence=drone.confidence,
                    ssid=best_obs.ssid if best_obs else None,
                    manufacturer=drone.manufacturer,
                    drone_id=did,
                    model=drone.model,
                )[0] if best_obs else "unknown",
                "observations": [
                    {"device_id": o.device_id, "rssi": o.rssi, "distance_m": o.estimated_distance_m}
                    for o in drone.observations
                ],
            })
        active_info.append(info)

    return {
        "active_drone_count": len(active_info),
        "active_drones": active_info,
        "recent_alerts": _drone_alerts[-50:],
        "total_alerts": len(_drone_alerts),
    }


# GET /detections/trails — position history for devices that have moved
# ---------------------------------------------------------------------------

@router.get("/trails")
async def get_position_trails():
    """Return position history trails for all devices with movement."""
    trails = _sensor_tracker._ekf.get_all_trails()
    return {
        "count": len(trails),
        "trails": trails,
        "stats": _sensor_tracker.get_ekf_stats(),
    }


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
async def get_whitelist(db: AsyncSession = Depends(get_db)):
    """Return current whitelist (SSID patterns from DB + BSSIDs from memory)."""
    from app.models.db_models import WhitelistedSSID
    result = await db.execute(select(WhitelistedSSID).order_by(WhitelistedSSID.pattern))
    entries = [{"pattern": r.pattern, "label": r.label, "added_at": r.added_at.isoformat()} for r in result.scalars().all()]
    return {
        "ssid_patterns": entries,
        "bssids": sorted(_anomaly_detector.whitelist_bssids),
    }


@router.post("/anomalies/whitelist")
async def add_whitelist(body: dict, db: AsyncSession = Depends(get_db)):
    """Add an SSID pattern to whitelist. Body: {"pattern": "CasaChomp*", "label": "Home WiFi"}"""
    pattern = body.get("pattern", "").strip()
    label = body.get("label", "").strip() or None
    wl_type = body.get("type", "ssid")
    if not pattern:
        return {"error": "pattern is required"}

    if wl_type == "bssid":
        _anomaly_detector.add_whitelist(pattern, "bssid")
    else:
        # Save to DB
        from app.models.db_models import WhitelistedSSID
        existing = await db.execute(select(WhitelistedSSID).where(WhitelistedSSID.pattern == pattern))
        if not existing.scalar_one_or_none():
            db.add(WhitelistedSSID(pattern=pattern, label=label))
            await db.commit()
        # Reload in-memory whitelist
        from app.services.classifier import async_load_whitelist
        await async_load_whitelist(db)

    return {"ok": True}


@router.delete("/anomalies/whitelist")
async def remove_whitelist(body: dict, db: AsyncSession = Depends(get_db)):
    """Remove an entry from the whitelist."""
    pattern = body.get("pattern", "").strip()
    wl_type = body.get("type", "ssid")

    if wl_type == "bssid":
        _anomaly_detector.remove_whitelist(pattern, "bssid")
    else:
        from app.models.db_models import WhitelistedSSID
        result = await db.execute(select(WhitelistedSSID).where(WhitelistedSSID.pattern == pattern))
        entry = result.scalar_one_or_none()
        if entry:
            await db.delete(entry)
            await db.commit()
        # Reload in-memory whitelist
        from app.services.classifier import async_load_whitelist
        await async_load_whitelist(db)

    return {"ok": True}


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
    classification: Annotated[str | None, Query(description="Filter by classification: confirmed_drone, likely_drone, test_drone, possible_drone, unknown_device, known_ap, tracker")] = None,
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
    if classification:
        items = [d for d in items if d.classification == classification]
    items = items[-limit:]
    items.reverse()
    return RecentDetectionsResponse(count=len(items), max_stored=_MAX_RECENT, detections=items)


# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# GET /detections/probes/history — persistent probe request history from DB
# ---------------------------------------------------------------------------

@router.get("/probes/history")
async def get_probe_history(
    hours: Annotated[float, Query(ge=0.1, le=720)] = 24.0,
    device: Annotated[str | None, Query(description="Filter by device MAC (probe_XX:XX:XX:XX:XX:XX)")] = None,
    ssid: Annotated[str | None, Query(description="Filter by probed SSID")] = None,
    db: AsyncSession = Depends(get_db),
):
    """Return historical probe request data from the database.

    Shows all devices that have probed for WiFi networks, what SSIDs they
    searched for, how often, and from which sensors. Useful for identifying
    devices with broken WiFi configs or tracking device movement patterns.
    """
    import datetime as dt

    cutoff = datetime.now(timezone.utc) - dt.timedelta(hours=hours)

    query = select(DroneDetection).where(
        DroneDetection.source == "wifi_probe_request",
        DroneDetection.received_at >= cutoff,
    )
    if device:
        query = query.where(DroneDetection.drone_id == device)
    if ssid:
        query = query.where(DroneDetection.ssid == ssid)

    query = query.order_by(DroneDetection.received_at.desc()).limit(5000)

    result = await db.execute(query)
    detections = result.scalars().all()

    # Group by device
    devices: dict[str, dict] = {}
    # SSID-centric tracking: for each SSID, which MACs + sensors + probe counts
    ssid_groups: dict[str, dict] = {}

    for d in detections:
        did = d.drone_id or "unknown"
        if did not in devices:
            devices[did] = {
                "device": did,
                "mac": did.replace("probe_", ""),
                "ssids": {},
                "sensors": set(),
                "best_rssi": -999,
                "total_probes": 0,
                "first_seen": d.received_at.isoformat() if d.received_at else "",
                "last_seen": d.received_at.isoformat() if d.received_at else "",
            }

        dev = devices[did]
        dev["total_probes"] += 1
        if d.ssid:
            dev["ssids"][d.ssid] = dev["ssids"].get(d.ssid, 0) + 1
            # SSID-centric grouping
            if d.ssid not in ssid_groups:
                ssid_groups[d.ssid] = {
                    "ssid": d.ssid,
                    "device_macs": set(),
                    "sensors": set(),
                    "total_probes": 0,
                    "best_rssi": -999,
                    "first_seen": d.received_at.isoformat() if d.received_at else "",
                    "last_seen": d.received_at.isoformat() if d.received_at else "",
                }
            sg = ssid_groups[d.ssid]
            sg["device_macs"].add(did.replace("probe_", ""))
            sg["total_probes"] += 1
            if d.device_id:
                sg["sensors"].add(d.device_id)
            if d.rssi and d.rssi > sg["best_rssi"]:
                sg["best_rssi"] = d.rssi
            if d.received_at:
                ts = d.received_at.isoformat()
                if ts > sg["last_seen"]:
                    sg["last_seen"] = ts
                if ts < sg["first_seen"]:
                    sg["first_seen"] = ts
        if d.device_id:
            dev["sensors"].add(d.device_id)
        if d.rssi and d.rssi > dev["best_rssi"]:
            dev["best_rssi"] = d.rssi
        if d.received_at:
            ts = d.received_at.isoformat()
            if ts > dev["last_seen"]:
                dev["last_seen"] = ts
            if ts < dev["first_seen"]:
                dev["first_seen"] = ts

    # Build device response
    device_list = []
    for dev in sorted(devices.values(), key=lambda x: x["total_probes"], reverse=True):
        dev["sensors"] = sorted(dev["sensors"])
        dev["sensor_count"] = len(dev["sensors"])
        dev["ssids"] = [{"ssid": k, "count": v} for k, v in sorted(dev["ssids"].items(), key=lambda x: -x[1])]
        dev["unique_ssids"] = len(dev["ssids"])
        if dev["best_rssi"] == -999:
            dev["best_rssi"] = None
        device_list.append(dev)

    # Hot SSIDs ranking (SSID-centric view)
    hot_ssids = []
    for sg in sorted(ssid_groups.values(), key=lambda x: x["total_probes"], reverse=True)[:30]:
        hot_ssids.append({
            "ssid": sg["ssid"],
            "device_count": len(sg["device_macs"]),
            "total_probes": sg["total_probes"],
            "sensors": sorted(sg["sensors"]),
            "best_rssi": sg["best_rssi"] if sg["best_rssi"] != -999 else None,
            "first_seen": sg["first_seen"],
            "last_seen": sg["last_seen"],
        })

    return {
        "hours_queried": hours,
        "total_probes": len(detections),
        "unique_devices": len(device_list),
        "unique_ssids": len(ssid_groups),
        "devices": device_list,
        "hot_ssids": hot_ssids,
    }


# GET /detections/probes — WiFi probe request device summary
# ---------------------------------------------------------------------------

@router.get("/probes")
async def get_probe_devices(
    drone_only: Annotated[bool, Query(description="Only return devices classified as likely_drone or confirmed_drone")] = False,
    max_age_s: Annotated[int, Query(ge=1, le=3600, description="Max age in seconds")] = 120,
):
    """Return WiFi probe request devices grouped by probing MAC (BSSID).

    Scans the in-memory ring buffer for source=='wifi_probe_request',
    groups by BSSID (probing device MAC), and returns aggregated info
    per device including all probed SSIDs, best RSSI, sensor count,
    and classification.
    """
    now = time.time()

    # Filter to probe request detections within age window
    probe_items = [
        d for d in _recent_detections
        if d.source == "wifi_probe_request" and (now - d.received_at) <= max_age_s
    ]

    # Group by BSSID (probing device MAC)
    groups: dict[str, dict] = {}
    for d in probe_items:
        mac = d.bssid or d.drone_id or "unknown"
        # Normalize: strip "probe_" prefix from drone_id if used as fallback
        if mac.startswith("probe_"):
            mac = mac[6:]

        if mac not in groups:
            groups[mac] = {
                "mac": mac,
                "probed_ssids": set(),
                "probe_count": 0,
                "best_rssi": -999,
                "sensors": set(),
                "last_seen": 0.0,
                "classification": "wifi_device",
                "lat": None,
                "lon": None,
            }
        g = groups[mac]
        g["probe_count"] += 1

        # Collect probed SSIDs from individual detection SSID field
        if d.ssid:
            g["probed_ssids"].add(d.ssid)

        # Also collect from probed_ssids list if present
        if d.probed_ssids:
            for s in d.probed_ssids:
                g["probed_ssids"].add(s)

        if d.rssi is not None and d.rssi > g["best_rssi"]:
            g["best_rssi"] = d.rssi
        if d.device_id:
            g["sensors"].add(d.device_id)
        if d.received_at > g["last_seen"]:
            g["last_seen"] = d.received_at

        # Use the strongest classification (likely_drone > wifi_device)
        if d.classification in ("confirmed_drone", "likely_drone"):
            g["classification"] = d.classification

    # Try to get triangulated positions for probe devices
    located = _sensor_tracker.get_located_drones()
    located_by_id = {d.drone_id: d for d in located}

    devices = []
    for mac, g in groups.items():
        cls = g["classification"]
        if drone_only and cls not in ("likely_drone", "confirmed_drone"):
            continue

        # Check for triangulated position
        probe_drone_id = f"probe_{mac}"
        loc = located_by_id.get(probe_drone_id)

        device_entry = {
            "mac": mac,
            "probed_ssids": sorted(g["probed_ssids"]),
            "probe_count": g["probe_count"],
            "best_rssi": g["best_rssi"] if g["best_rssi"] != -999 else None,
            "classification": cls,
            "sensor_count": len(g["sensors"]),
            "sensors": sorted(g["sensors"]),
            "last_seen": g["last_seen"],
            "age_s": round(now - g["last_seen"], 1),
            "lat": loc.lat if loc else None,
            "lon": loc.lon if loc else None,
        }
        devices.append(device_entry)

    # Sort by last_seen descending
    devices.sort(key=lambda x: x["last_seen"], reverse=True)

    return {"count": len(devices), "devices": devices}


# ---------------------------------------------------------------------------
# GET /detections/sensor/{device_id}/stats — per-sensor breakdown
# ---------------------------------------------------------------------------

@router.get("/sensor/{device_id}/stats")
async def get_sensor_stats(device_id: str):
    """Return detailed stats for a single sensor: source breakdown, SSIDs, BLE types, recent activity."""
    now = time.time()
    items = [d for d in _recent_detections if d.device_id == device_id]

    # Source breakdown
    source_counts: dict[str, int] = {}
    classification_counts: dict[str, int] = {}
    ssids: dict[str, dict] = {}
    ble_types: dict[str, int] = {}
    newest = 0.0
    oldest = now

    for d in items:
        src = d.source or "unknown"
        source_counts[src] = source_counts.get(src, 0) + 1

        cls = d.classification or "unknown"
        classification_counts[cls] = classification_counts.get(cls, 0) + 1

        if d.received_at > newest:
            newest = d.received_at
        if d.received_at < oldest:
            oldest = d.received_at

        # WiFi SSID tracking
        if d.ssid and "ble" not in src and src != "wifi_probe_request":
            if d.ssid not in ssids:
                ssids[d.ssid] = {"count": 0, "best_rssi": -999, "classification": d.classification, "bssid": d.bssid}
            ssids[d.ssid]["count"] += 1
            if d.rssi and d.rssi > ssids[d.ssid]["best_rssi"]:
                ssids[d.ssid]["best_rssi"] = d.rssi

        # BLE device type tracking
        if "ble" in src and d.drone_id:
            parts = d.drone_id.split(":")
            dtype = ":".join(parts[2:]).strip() if len(parts) >= 3 else "Unknown"
            ble_types[dtype] = ble_types.get(dtype, 0) + 1

    # Heartbeat info
    hb = _node_heartbeats.get(device_id, {})

    wifi_ssid_list = [
        {"ssid": k, "count": v["count"], "best_rssi": v["best_rssi"] if v["best_rssi"] != -999 else None,
         "classification": v["classification"], "bssid": v["bssid"]}
        for k, v in sorted(ssids.items(), key=lambda x: x[1]["count"], reverse=True)
    ]

    ble_type_list = [{"type": k, "count": v} for k, v in sorted(ble_types.items(), key=lambda x: x[1], reverse=True)]

    return {
        "device_id": device_id,
        "online": hb.get("last_seen", 0) > now - 120 if hb else False,
        "ip": hb.get("ip"),
        "total_in_buffer": len(items),
        "total_batches": hb.get("total_batches", 0),
        "total_detections": hb.get("total_detections", 0),
        "source_breakdown": source_counts,
        "classification_breakdown": classification_counts,
        "wifi_ssids": wifi_ssid_list,
        "ble_device_types": ble_type_list,
        "wifi_count": sum(v for k, v in source_counts.items() if "ble" not in k),
        "ble_count": sum(v for k, v in source_counts.items() if "ble" in k),
        "unique_ssids": len(ssids),
        "newest_detection": newest if newest > 0 else None,
        "oldest_detection": oldest if oldest < now else None,
        "age_span_s": round(newest - oldest, 1) if newest > 0 and oldest < now else 0,
    }


# ---------------------------------------------------------------------------
# GET /detections/grouped — deduplicated view grouped by drone_id
# ---------------------------------------------------------------------------

@router.get("/grouped")
async def get_grouped_detections(
    classification: Annotated[str | None, Query(description="Filter by classification")] = None,
    exclude_known_ap: Annotated[bool, Query(description="Hide known APs (default true)")] = True,
    max_age_s: Annotated[int, Query(ge=1, le=3600, description="Max age in seconds")] = 120,
):
    """Return detections grouped by drone_id — one row per device, showing all sensors that see it.

    This is the clean view for comparing multi-sensor coverage.
    """
    now = time.time()
    items = list(_recent_detections)

    # Filter by age
    items = [d for d in items if (now - d.received_at) <= max_age_s]

    # Filter out known APs by default
    if exclude_known_ap:
        items = [d for d in items if d.classification != "known_ap"]

    # Filter by classification
    if classification:
        items = [d for d in items if d.classification == classification]

    # Group by drone_id
    groups: dict[str, dict] = {}
    for d in items:
        key = d.drone_id
        if key not in groups:
            groups[key] = {
                "drone_id": d.drone_id,
                "classification": d.classification,
                "ssid": d.ssid,
                "source": d.source,
                "manufacturer": d.manufacturer,
                "model": d.model,
                "best_rssi": d.rssi or -999,
                "best_confidence": d.confidence,
                "sensors": set(),
                "sensor_rssi": {},
                "last_seen": d.received_at,
                "first_seen": d.received_at,
                "count": 0,
                "bssid": d.bssid,
            }
        g = groups[key]
        g["count"] += 1
        if d.device_id:
            g["sensors"].add(d.device_id)
            # Track best RSSI per sensor
            prev = g["sensor_rssi"].get(d.device_id, -999)
            if d.rssi and d.rssi > prev:
                g["sensor_rssi"][d.device_id] = d.rssi
        if d.rssi and d.rssi > g["best_rssi"]:
            g["best_rssi"] = d.rssi
        if d.confidence > g["best_confidence"]:
            g["best_confidence"] = d.confidence
        if d.received_at > g["last_seen"]:
            g["last_seen"] = d.received_at
        if d.received_at < g["first_seen"]:
            g["first_seen"] = d.received_at

    # Build response
    result = []
    for g in sorted(groups.values(), key=lambda x: x["last_seen"], reverse=True):
        result.append({
            "drone_id": g["drone_id"],
            "classification": g["classification"],
            "ssid": g["ssid"],
            "source": g["source"],
            "manufacturer": g["manufacturer"],
            "bssid": g["bssid"],
            "best_rssi": g["best_rssi"] if g["best_rssi"] != -999 else None,
            "best_confidence": g["best_confidence"],
            "sensor_count": len(g["sensors"]),
            "sensors": sorted(g["sensors"]),
            "sensor_rssi": g["sensor_rssi"],
            "detection_count": g["count"],
            "last_seen": g["last_seen"],
            "first_seen": g["first_seen"],
            "age_s": round(now - g["last_seen"], 1),
        })

    return {"count": len(result), "devices": result}


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

    track = []
    total_distance = 0.0
    prev_lat, prev_lon = None, None
    for p in positions:
        pt = DroneTrackPoint(
            lat=p.lat,
            lon=p.lon,
            alt=p.alt,
            accuracy_m=p.accuracy_m,
            position_source=p.position_source,
            sensor_count=p.sensor_count,
            confidence=p.confidence,
            timestamp=p.timestamp.isoformat() if p.timestamp else "",
            observations_json=p.observations_json,
            classification=p.classification,
            ssid=p.ssid,
        )
        track.append(pt)
        # Calculate total distance traveled
        if prev_lat is not None:
            dlat = (p.lat - prev_lat) * 111320
            dlon = (p.lon - prev_lon) * 111320 * 0.78
            total_distance += (dlat**2 + dlon**2) ** 0.5
        prev_lat, prev_lon = p.lat, p.lon

    avg_accuracy = sum(p.accuracy_m or 0 for p in positions) / max(1, len(positions))

    return DroneTrackResponse(
        drone_id=drone_id,
        point_count=len(track),
        track=track,
        total_distance_m=round(total_distance, 1),
        avg_accuracy_m=round(avg_accuracy, 1),
        hours_queried=hours,
    )
