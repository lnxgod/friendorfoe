"""Drone detection ingestion endpoints for ESP32 sensor nodes."""

import json
import logging
import time
from collections import deque
from datetime import datetime, timezone
from typing import Annotated

from fastapi import APIRouter, BackgroundTasks, Depends, Query, Request
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

_MAX_RECENT = 50000
_recent_detections: deque[StoredDetection] = deque(maxlen=_MAX_RECENT)

# ---------------------------------------------------------------------------
# In-memory node heartbeat tracker (works without DB)
# ---------------------------------------------------------------------------

_node_heartbeats: dict[str, dict] = {}

# Position dedup cache: drone_id → (lat, lon) of last logged position
_position_dedup: dict[str, tuple[float, float]] = {}

# Ingest dedup: (drone_id, source, bssid, bucket_10s) -> first_seen_ts
# Scanners can re-enqueue the same beacon every scan; uplinks can replay
# offline-buffered batches. This suppresses duplicates within 30 s so the
# classifier / anomaly detector / DB aren't fed the same observation repeatedly.
_INGEST_DEDUP_TTL_S = 30.0
_INGEST_DEDUP_MAX = 4096
_ingest_dedup: dict[tuple[str, str, str, int], float] = {}

# Drone-protocol sources bypass ingest dedup. ASTM Remote ID / DJI DroneID /
# WiFi Beacon RID are precious signals — the drone is actively broadcasting
# its own position and status, and the ODID spec cycles through 5 message
# types (Basic, Location, Self-ID, System, Operator). If we dedup on
# (drone_id, source, bssid) in a 10 s window we'd drop 4/5 of those messages
# and lose the operator/system fields that only ride in specific messages.
_NEVER_DEDUP_SOURCES = frozenset({"ble_rid", "wifi_beacon_rid", "wifi_dji_ie"})


def _ingest_dedup_hit(drone_id: str, source: str, bssid: str, ts: float) -> bool:
    """True if we've seen this (drone, source, bssid) in the last 30 s.

    Uses a 10-second timestamp bucket so the same beacon reported multiple
    times within a scan window collapses to one record.

    Drone-protocol sources are exempt — every RID / DJI IE / Beacon RID frame
    is forwarded to the full pipeline.
    """
    if source in _NEVER_DEDUP_SOURCES:
        return False
    bucket = int(ts // 10)
    key = (drone_id or "", source or "", (bssid or "").upper(), bucket)
    prev = _ingest_dedup.get(key)
    if prev is not None and (ts - prev) < _INGEST_DEDUP_TTL_S:
        return True
    _ingest_dedup[key] = ts
    if len(_ingest_dedup) > _INGEST_DEDUP_MAX:
        cutoff = ts - _INGEST_DEDUP_TTL_S
        for k, v in list(_ingest_dedup.items()):
            if v < cutoff:
                del _ingest_dedup[k]
    return False

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

# ---------------------------------------------------------------------------
# Multi-sensor tracker (triangulation engine)
# ---------------------------------------------------------------------------

_sensor_tracker = SensorTracker()
_drone_tracker = DroneTracker(sensor_tracker=_sensor_tracker)
_rf_anomaly_detector = RFAnomalyDetector()
_anomaly_detector = AnomalyDetector()
_ble_enricher = BLEEnricher()
_signal_tracker = SignalTracker()

from app.services.entity_tracker import EntityTracker
_entity_tracker = EntityTracker()
# Give the tracker access to BLE auth-tag MAC-rotation links so it can merge
# entities that the enricher has already identified as the same physical device.
_entity_tracker.set_ble_enricher(_ble_enricher)

# First-seen event detector (new_probe_mac, new_rid_drone, new_hostile_tool,
# new_glasses, new_tracker, new_probed_ssid, new_ap, device_departed).
# Persisted per identifier; rehydrated from DB at app startup.
from app.services.event_detector import EventDetector
_event_detector = EventDetector()

# Cross-layer identity correlator — groups BLE+WiFi observations of the same
# physical device (probe-IE hash, auth-tag, temporal co-location).
from app.services.identity_correlator import IdentityCorrelator
_identity_correlator = IdentityCorrelator()
_entity_tracker.set_identity_correlator(_identity_correlator)

from app.services.calibration import CalibrationManager
_calibration_mgr = CalibrationManager()

# Apply persisted calibration to triangulation engine on startup
if _calibration_mgr.last_result and _calibration_mgr.last_result.r_squared > 0.1:
    from app.services.triangulation import update_calibration as _apply_cal
    _apply_cal(_calibration_mgr.last_result.rssi_ref,
               _calibration_mgr.last_result.path_loss_exponent)


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
        "wifi_ssid": batch.wifi_ssid or _node_heartbeats.get(batch.device_id, {}).get("wifi_ssid"),
        "wifi_rssi": batch.wifi_rssi if batch.wifi_rssi is not None else _node_heartbeats.get(batch.device_id, {}).get("wifi_rssi"),
    }

    # WiFi attack detection (v0.60+) — scanner forwards deauth/disassoc counts
    # + flood/beacon-spam flags per status report. Emit anomaly alerts when
    # patterns appear so operators see "deauth attack from MAC X near node Y"
    # not just a counter rising in /api/status.
    for sc in (batch.scanners or []):
        if not isinstance(sc, dict): continue
        deauth_n = int(sc.get("deauth", 0) or 0)
        disassoc_n = int(sc.get("disassoc", 0) or 0)
        flood = bool(sc.get("flood", False))
        bcn_spam = bool(sc.get("bcn_spam", False))
        if flood or bcn_spam or deauth_n > 20 or disassoc_n > 20:
            slot = sc.get("uart", "?")
            try:
                _anomaly_detector.record_wifi_attack(
                    device_id=batch.device_id,
                    scanner_slot=slot,
                    deauth_count=deauth_n,
                    disassoc_count=disassoc_n,
                    deauth_flood=flood,
                    beacon_spam=bcn_spam,
                    timestamp=received_at,
                )
            except AttributeError:
                pass  # method not yet present on older AnomalyDetector

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

    dedup_skipped = 0
    newly_emitted_events: list[tuple[str, str]] = []
    for det in batch.detections:
        # Skip our own infrastructure SSIDs — never store or process FoF-* detections
        if det.ssid and det.ssid.upper().startswith("FOF-") and not det.ssid.upper().startswith("FOF-DRONE-"):
            continue

        # Drop duplicates (scanner re-emission, uplink offline-replay, batch retries)
        if _ingest_dedup_hit(det.drone_id, det.source, det.bssid or "", received_at):
            dedup_skipped += 1
            continue

        # Enrich manufacturer from the IEEE OUI registry when the scanner
        # couldn't identify it locally. Wireshark's 56k-entry manuf file
        # catches ~85% of burn-in MACs; randomized MACs short-circuit to
        # "Randomized MAC" so they aren't lumped with genuine unknowns.
        _mfr = (det.manufacturer or "").strip()
        if (not _mfr or _mfr in ("Unknown", "?")) and det.bssid:
            try:
                from app.services.oui_db import oui_lookup as _oui
                hit = _oui(det.bssid)
                if hit:
                    det.manufacturer = hit[0]  # short name (e.g. "Espressif")
            except Exception:
                pass

        # Data-flags byte: v0.58+ scanners emit ble_apple_flags (always, even
        # when 0). Legacy scanners emit ble_apple_info non-zero-only. Prefer
        # the new field; fall back to the old one until all nodes are reflashed.
        ainfo = det.ble_apple_flags
        if ainfo is None:
            ainfo = det.ble_apple_info or 0

        # Honest Apple classification. v0.58+ scanners send "Apple Device"
        # directly (with enriched flag labels still applied below). Legacy
        # scanners still send "iPhone" — rewrite those. v0.58 scanners that
        # resolve to Handoff/AirPlay/AirPods/AirTag send their specific label
        # and bypass this block entirely.
        if _mfr in ("iPhone", "Apple Device"):
            at = det.ble_apple_type or 0
            if _mfr == "iPhone" and at in (0x0C, 0x09):
                det.manufacturer = "Apple Mac/TV"
            elif at in (0x10, 0x0F, 0x05, 0x08, 0x0D, 0x0E) or _mfr == "Apple Device":
                bits = []
                if ainfo & 0x01: bits.append("AirPods in")
                if ainfo & 0x04: bits.append("Watch paired")
                if bits:
                    det.manufacturer = "Apple (" + ", ".join(bits) + ")"
                else:
                    det.manufacturer = "Apple Device"
            else:
                # Legacy scanner said "iPhone" with no Continuity evidence
                # (length heuristic / short payload path). Apple doesn't
                # broadcast model — downgrade to generic.
                det.manufacturer = "Apple Device"

        # Classify the detection
        classification, adj_confidence = classify_detection(
            source=det.source,
            confidence=det.confidence,
            ssid=det.ssid,
            manufacturer=det.manufacturer,
            drone_id=det.drone_id,
            model=det.model,
            bssid=det.bssid,
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
            ble_apple_auth=det.ble_apple_auth,
            ble_adv_interval=det.ble_adv_interval,
            ble_svc_uuids=det.ble_svc_uuids,
            ble_apple_info=ainfo,
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

        # Entity correlation — group signals into logical entities.
        # Extract a device-type hint from drone_id (BLE:HASH:TypeName format);
        # the prior `dir()` check always evaluated to False because
        # `device_type_str` was never assigned, silently defeating labeling.
        _device_type_hint = ""
        if det.drone_id:
            _parts = det.drone_id.split(":")
            if len(_parts) >= 3:
                _device_type_hint = ":".join(_parts[2:]).strip()
        _entity_tracker.ingest(
            drone_id=det.drone_id, source=det.source,
            confidence=det.confidence, rssi=det.rssi or 0,
            ssid=det.ssid or "", bssid=det.bssid or "",
            manufacturer=det.manufacturer or "",
            device_id=batch.device_id, received_at=received_at,
            model=det.model or "",
            probed_ssids=det.probed_ssids,
            device_type=_device_type_hint,
            ie_hash=getattr(det, "ie_hash", None),
        )

        # Cross-layer identity correlator — records BLE↔WiFi co-location
        # and surfaces probe-IE identity hints.
        try:
            _identity_correlator.ingest(
                source=det.source,
                drone_id=det.drone_id,
                bssid=det.bssid,
                model=det.model,
                ie_hash=getattr(det, "ie_hash", None),
                sensor_id=batch.device_id,
                rssi=det.rssi,
                ts=received_at,
            )
        except Exception:
            pass

        # First-seen event detection (noteworthy first sightings only)
        try:
            newly_emitted_events.extend(_event_detector.ingest(
                source=det.source,
                classification=classification,
                drone_id=det.drone_id,
                bssid=det.bssid,
                ssid=det.ssid,
                manufacturer=det.manufacturer,
                model=det.model,
                probed_ssids=det.probed_ssids,
                rssi=det.rssi,
                confidence=det.confidence,
                sensor_id=batch.device_id,
                ts=received_at,
                latitude=det.latitude,
                longitude=det.longitude,
                operator_id=det.operator_id,
            ))
        except Exception as e:
            logger.debug("event_detector.ingest skipped: %s", e)

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

        # NOTE: _anomaly_detector.ingest() is called once above (~line 270). Do
        # not add a second call here — it doubles RSSI samples and inflates
        # velocity/spike deltas.

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

        # Triangulation engine (use adjusted confidence for test drones).
        # Per-detection timestamp from scanner's epoch-synced clock (v0.60+);
        # falls back to batch receive time for pre-sync or legacy uplinks.
        det_ts = (det.timestamp / 1000.0
                  if det.timestamp and det.timestamp > 1_700_000_000_000
                  else received_at)
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
            timestamp=det_ts,
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
                    bssid=best_obs.bssid if best_obs and hasattr(best_obs, 'bssid') else None,
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

    # Debounced entity checkpoint — persists dirty entities roughly every 30 s
    # so the dashboard entity count survives a backend restart. Runs on its
    # own session so a checkpoint failure can't poison the router's UoW.
    try:
        await _entity_tracker.checkpoint()
    except Exception as e:
        logger.debug("EntityTracker checkpoint skipped: %s", e)

    # Event detector: commit any newly-emitted first-seen events, flush dirty
    # counter updates. Uses its own AsyncSession so any error here stays
    # isolated from the detection write above. Also sweeps any
    # device_departed events queued by EntityTracker._prune during this
    # batch's checkpoint.
    try:
        from app.services.database import async_session as _async_session
        async with _async_session() as ev_session:
            if newly_emitted_events:
                await _event_detector.commit_new(ev_session, newly_emitted_events)
            departures = _event_detector.drain_pending_departures()
            if departures:
                await _event_detector.commit_new(ev_session, departures)
            await _event_detector.flush_dirty(ev_session)
    except Exception as e:
        logger.debug("EventDetector commit skipped: %s", e)

    # Identity correlator checkpoint (debounced to ~30 s internally).
    try:
        await _identity_correlator.checkpoint()
    except Exception as e:
        logger.debug("IdentityCorrelator checkpoint skipped: %s", e)

    if dedup_skipped:
        logger.debug("Ingest dedup: skipped %d duplicate detections from %s",
                     dedup_skipped, batch.device_id)

    return DroneDetectionResponse(
        status="ok", accepted=accepted, device_id=batch.device_id
    )


# ---------------------------------------------------------------------------
# GET /detections/drones/map — triangulated drone positions
# ---------------------------------------------------------------------------

@router.get("/drones/map", response_model=DroneMapResponse)
async def get_drone_map(
    classification: Annotated[str | None, Query(description="Filter by classification")] = None,
    exclude_known: Annotated[bool, Query(description="Exclude known_ap and wifi_device")] = True,
    include_probes: Annotated[bool, Query(description="Include WiFi probe devices")] = False,
    min_confidence: Annotated[float, Query(ge=0.0, le=1.0, description="Min confidence (0=all)")] = 0.0,
) -> DroneMapResponse:
    """Return all currently-tracked drones with estimated positions."""
    located = _sensor_tracker.get_located_drones()
    sensors = _sensor_tracker.get_active_sensors()

    # Get signal tracker data for approach/departure velocity
    signal_tracks = _signal_tracker.get_live_tracks(limit=200, active_within_s=30.0)
    track_velocity = {}
    if signal_tracks and "tracks" in signal_tracks:
        for t in signal_tracks["tracks"]:
            track_velocity[t.get("track_id", "")] = t.get("approach_speed_mps", 0)

    drone_items = []
    for d in located:
        # Classify using the best observation's data
        best_obs = max(d.observations, key=lambda o: o.confidence) if d.observations else None
        best_bssid = best_obs.bssid if best_obs and hasattr(best_obs, 'bssid') else None
        cls, _ = classify_detection(
            source=best_obs.source if best_obs else "",
            confidence=d.confidence,
            ssid=best_obs.ssid if best_obs else None,
            manufacturer=d.manufacturer,
            drone_id=d.drone_id,
            model=d.model,
            bssid=best_bssid,
        ) if best_obs else ("unknown_device", d.confidence)

        # Skip positions with huge uncertainty — they're noise
        if d.accuracy_m and d.accuracy_m > 200:
            continue

        # Determine if this is a probe device
        is_probe = best_obs and best_obs.source == "wifi_probe_request" if best_obs else False

        if classification and cls != classification:
            continue
        if exclude_known and cls in ("known_ap", "mobile_hotspot"):
            continue
        # Include wifi_device only if include_probes and it's a probe
        if exclude_known and cls == "wifi_device" and not (include_probes and is_probe):
            continue
        # Filter by confidence
        if exclude_known and not include_probes and d.confidence < 0.15:
            continue
        if min_confidence > 0 and d.confidence < min_confidence:
            continue

        # Get approach/departure velocity from signal tracker
        vel = track_velocity.get(d.drone_id, 0)
        trend = None
        if abs(vel) > 1.0:
            trend = "approaching" if vel > 0 else "departing"
        elif abs(vel) > 0.1:
            trend = "stationary"

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
        # Classify the identity derivation path so the dashboard can distinguish
        # ASTM-Remote-ID-verified drones from BLE-fingerprint-guessed identities
        # without having to duplicate the prefix heuristic client-side.
        _did = d.drone_id or ""
        if _did.startswith("rid_"):
            identity_source = "rid"
        elif _did.startswith("FP:") or _did.startswith("BLE:"):
            identity_source = "fingerprint"
        else:
            identity_source = "mac"

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
            approach_speed_mps=round(vel, 2) if vel else None,
            rssi_trend=trend,
            age_s=round(time.time() - max((o.timestamp for o in d.observations), default=time.time()), 1),
            first_seen=min((o.timestamp for o in d.observations), default=None),
            last_seen=max((o.timestamp for o in d.observations), default=None),
            bssid=best_bssid,
            ssid=best_obs.ssid if best_obs else None,
            identity_source=identity_source,
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


@router.post("/devices/{fingerprint}/mark-known")
async def mark_tracker_known(fingerprint: str):
    """Mark a tracker as 'mine' — suppresses lingering alerts."""
    _anomaly_detector.mark_tracker_known(fingerprint)
    return {"ok": True, "fingerprint": fingerprint, "status": "known"}


@router.delete("/devices/{fingerprint}/mark-known")
async def unmark_tracker_known(fingerprint: str):
    """Remove a tracker from the known list — re-enables lingering alerts."""
    _anomaly_detector.unmark_tracker_known(fingerprint)
    return {"ok": True, "fingerprint": fingerprint, "status": "unknown"}


@router.get("/devices/{fingerprint}/associated")
async def get_associated_devices(fingerprint: str):
    """Find devices likely carried by the same person (entity resolution)."""
    associated = _ble_enricher.find_associated_devices(fingerprint)
    return {"fingerprint": fingerprint, "associated": associated, "count": len(associated)}


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
    nodes.sort(key=lambda n: n["device_id"])
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


# ---------------------------------------------------------------------------
# GET /detections/entities — correlated entity view
# ---------------------------------------------------------------------------

@router.get("/entities")
async def get_entities(
    active_only: Annotated[bool, Query(description="Only active entities")] = True,
    limit: Annotated[int, Query(ge=1, le=200)] = 50,
):
    """Return tracked entities — correlated device groups with timeline."""
    entities = _entity_tracker.get_entities(active_only=active_only, limit=limit)
    stats = _entity_tracker.get_stats()
    return {"entities": entities, "stats": stats}


# ---------------------------------------------------------------------------
# POST /detections/calibrate — run inter-node RSSI calibration
# ---------------------------------------------------------------------------

@router.post("/calibrate")
async def start_calibration(background_tasks: BackgroundTasks):
    """Start inter-node RSSI calibration sequence."""
    if _calibration_mgr.is_running:
        return {"status": "already_running", "progress": _calibration_mgr.progress}

    # Gather online nodes with GPS + IP — use registered DB positions
    from app.models.db_models import SensorNode
    from app.services.database import async_session
    nodes = []
    try:
        async with async_session() as session:
            result = await session.execute(select(SensorNode))
            db_nodes = {sn.device_id: sn for sn in result.scalars().all()}
    except Exception:
        db_nodes = {}

    for node in _node_heartbeats.values():
        age = time.time() - node["last_seen"]
        if age > 120 or not node.get("ip"):
            continue
        db_node = db_nodes.get(node["device_id"])
        lat = db_node.lat if db_node and db_node.is_fixed else node.get("lat", 0)
        lon = db_node.lon if db_node and db_node.is_fixed else node.get("lon", 0)
        name = db_node.name if db_node else node["device_id"]
        if lat and lon and abs(lat) > 0.1:
            nodes.append({
                "device_id": node["device_id"],
                "ip": node["ip"],
                "lat": lat,
                "lon": lon,
                "name": name,
            })

    if len(nodes) < 2:
        return {"status": "error", "message": f"Need 2+ online nodes with GPS, found {len(nodes)}"}

    async def run_cal():
        from app.services.triangulation import update_calibration
        result = await _calibration_mgr.run_calibration(nodes)
        if result and result.r_squared > 0.1:
            update_calibration(result.rssi_ref, result.path_loss_exponent)

    background_tasks.add_task(run_cal)
    return {"status": "started", "nodes": len(nodes), "node_ids": [n["device_id"] for n in nodes]}


@router.get("/time")
async def server_time():
    """Epoch-ms from the backend, used by uplinks as an SNTP fallback.

    Pool's SNTP doesn't sync reliably in some network configurations. Uplinks
    that can reach the backend (which all of them can, otherwise nothing
    works) poll this endpoint as a last-resort time source. Returned value is
    integer epoch milliseconds; add a tiny client-side RTT correction if you
    need sub-100ms accuracy. Cheap + idempotent, no DB touch."""
    return {"ms": int(time.time() * 1000)}


@router.get("/calibrate/status")
async def calibration_status():
    """Get calibration progress and results."""
    return _calibration_mgr.get_status()


@router.get("/calibrate/model")
async def calibration_model():
    """Get current triangulation model parameters."""
    from app.services.triangulation import RSSI_REF, PATH_LOSS_OUTDOOR
    is_calibrated = _calibration_mgr.last_result is not None
    return {
        "rssi_ref": RSSI_REF,
        "path_loss_exponent": PATH_LOSS_OUTDOOR,
        "is_calibrated": is_calibrated,
        "last_calibration": _calibration_mgr.last_result.timestamp if _calibration_mgr.last_result else None,
        "r_squared": _calibration_mgr.last_result.r_squared if _calibration_mgr.last_result else None,
    }


@router.get("/calibrate/history")
async def calibration_history():
    """Get calibration history."""
    return {"history": _calibration_mgr.get_history()}


@router.get("/calibrate/matrix")
async def calibration_matrix():
    """Get node-pair RSSI/distance matrix from last calibration."""
    return _calibration_mgr.get_node_pair_matrix()


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
                    bssid=best_obs.bssid if best_obs and hasattr(best_obs, 'bssid') else None,
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
# GET /detections/tracking/diagnostics — per-drone emit history + stats
# ---------------------------------------------------------------------------

@router.get("/tracking/diagnostics")
async def get_tracking_diagnostics(
    recent: Annotated[int, Query(ge=0, le=200, description="Recent emits to include per drone")] = 20,
    limit: Annotated[int, Query(ge=1, le=500, description="Max drones returned")] = 100,
):
    """Diagnostic view of position emits per tracked drone.

    Added for the "jumping devices" investigation — answers
      - which `position_source`s fire for each drone
      - how often the chosen source flips
      - p95 / max per-emit jump distance
      - EKF health: velocity warnings, covariance errors, update rejects
      - large-jump count (emits >= 50 m from the previous emit)

    Pure observability — does not affect behavior.
    """
    from app.services.position_filter import EKF_HEALTH

    now = time.time()
    history = _sensor_tracker._emit_history
    flips = _sensor_tracker._source_flip_counts
    counters = _sensor_tracker._emit_counters

    # ── Global summary ──
    source_mix_total: dict[str, int] = {}
    for (drone_id, source), count in counters.items():
        source_mix_total[source] = source_mix_total.get(source, 0) + count
    total_emits = sum(source_mix_total.values())
    source_mix_frac = {
        src: round(cnt / total_emits, 3) for src, cnt in source_mix_total.items()
    } if total_emits > 0 else {}

    emits_last_5min = 0
    for hist in history.values():
        for rec in hist:
            if (now - rec.timestamp) <= 300:
                emits_last_5min += 1

    stats = {
        "drones_tracked": len(history),
        "total_emits": total_emits,
        "emits_last_5min": emits_last_5min,
        "source_mix": source_mix_frac,
        "source_counts": source_mix_total,
        "ekf_velocity_warnings": EKF_HEALTH["velocity_warnings"],
        "ekf_covariance_errors": EKF_HEALTH["covariance_errors"],
        "ekf_update_rejects": EKF_HEALTH["update_rejects"],
        "large_jumps_total": _sensor_tracker._large_jump_count,
        "large_jump_threshold_m": _sensor_tracker.LARGE_JUMP_THRESHOLD_M,
    }

    # ── Per-drone rows ──
    drones = []
    for drone_id, hist in history.items():
        if not hist:
            continue
        records = list(hist)
        jumps = [r.jump_m for r in records[1:]]  # skip first (no delta)
        jumps_sorted = sorted(jumps)
        if jumps_sorted:
            idx_p95 = min(len(jumps_sorted) - 1, int(0.95 * len(jumps_sorted)))
            p95_jump_m = jumps_sorted[idx_p95]
            max_jump_m = jumps_sorted[-1]
        else:
            p95_jump_m = 0.0
            max_jump_m = 0.0

        per_source: dict[str, int] = {}
        for rec in records:
            per_source[rec.position_source] = per_source.get(rec.position_source, 0) + 1

        last = records[-1]
        tail = records[-recent:] if recent else []
        drones.append({
            "drone_id": drone_id,
            "emit_count": len(records),
            "source_mix": per_source,
            "source_flips": dict(flips.get(drone_id, {})),
            "p95_jump_m": round(p95_jump_m, 2),
            "max_jump_m": round(max_jump_m, 2),
            "last_emit": {
                "timestamp": last.timestamp,
                "age_s": round(now - last.timestamp, 1),
                "position_source": last.position_source,
                "sensor_count": last.sensor_count,
                "accuracy_m": last.accuracy_m,
                "lat": last.lat,
                "lon": last.lon,
            },
            "recent": [
                {
                    "t": round(r.timestamp, 3),
                    "src": r.position_source,
                    "n": r.sensor_count,
                    "acc": r.accuracy_m,
                    "lat": r.lat,
                    "lon": r.lon,
                    "jump_m": round(r.jump_m, 2),
                    "used": r.used_sensors,
                }
                for r in tail
            ],
        })

    drones.sort(key=lambda d: d["max_jump_m"], reverse=True)
    drones = drones[:limit]

    return {"stats": stats, "drones": drones}


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

    # Group by entity_id when the EntityTracker knows this identifier; fall
    # back to drone_id otherwise. This collapses per-device rows across BLE /
    # WiFi / probe sources so a single phone shows once instead of N times.
    groups: dict[str, dict] = {}
    for d in items:
        entity_id = _entity_tracker.get_entity_id(d.drone_id)
        # Also try bssid as an identifier lookup if drone_id missed
        if not entity_id and d.bssid:
            entity_id = _entity_tracker.get_entity_id(d.bssid)
        group_key = entity_id or d.drone_id
        if group_key not in groups:
            groups[group_key] = {
                "entity_id": entity_id,
                "drone_id": d.drone_id,
                "drone_ids": set(),
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
        g = groups[group_key]
        g["count"] += 1
        if d.drone_id:
            g["drone_ids"].add(d.drone_id)
        if d.device_id:
            g["sensors"].add(d.device_id)
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

    # Prefer a confirmed classification across the group's detections — a
    # likely_drone under the same entity shouldn't be hidden by a later
    # unknown_device observation.
    _CLASS_PRIORITY = {
        "confirmed_drone": 0, "likely_drone": 1, "test_drone": 2,
        "possible_drone": 3, "tracker": 4, "unknown_device": 5,
        "wifi_device": 6, "mobile_hotspot": 7, "known_ap": 8,
    }
    for g in groups.values():
        best_cls = g["classification"] or "unknown_device"
        for d in items:
            if d.drone_id in g["drone_ids"] and d.classification:
                if _CLASS_PRIORITY.get(d.classification, 9) < _CLASS_PRIORITY.get(best_cls, 9):
                    best_cls = d.classification
        g["classification"] = best_cls

    # Build response
    result = []
    for g in sorted(groups.values(), key=lambda x: x["last_seen"], reverse=True):
        result.append({
            "entity_id": g["entity_id"],
            "drone_id": g["drone_id"],
            "drone_ids": sorted(g["drone_ids"]),
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


# ═══════════════════════════════════════════════════════════════════════════
# First-seen event API (see services/event_detector.py)
# ═══════════════════════════════════════════════════════════════════════════

def _event_row_to_item(row) -> dict:
    """Convert an Event row into the JSON shape the dashboard consumes."""
    try:
        md = json.loads(row.metadata_json or "{}")
    except Exception:
        md = {}
    try:
        sensors = json.loads(row.sensor_ids_json or "[]")
    except Exception:
        sensors = []
    return {
        "id": row.id,
        "event_type": row.event_type,
        "identifier": row.identifier,
        "severity": row.severity,
        "title": row.title or "",
        "message": row.message or "",
        "first_seen_at": row.first_seen_at.isoformat() if row.first_seen_at else "",
        "last_seen_at": row.last_seen_at.isoformat() if row.last_seen_at else "",
        "sighting_count": row.sighting_count,
        "sensor_count": row.sensor_count,
        "sensor_ids": sensors,
        "best_rssi": row.best_rssi,
        "metadata": md,
        "acknowledged": row.acknowledged,
        "acknowledged_at": row.acknowledged_at.isoformat() if row.acknowledged_at else None,
    }


@router.get("/events")
async def list_events(
    type: Annotated[str | None, Query(description="Filter by event_type")] = None,
    severity: Annotated[str | None, Query(description="info | warning | critical")] = None,
    acknowledged: Annotated[bool | None, Query(description="True / False / None (both)")] = None,
    since_hours: Annotated[float, Query(ge=0, le=720)] = 24.0,
    limit: Annotated[int, Query(ge=1, le=1000)] = 200,
    db: AsyncSession = Depends(get_db),
):
    """Paginated first-seen event feed. Default: last 24 h, any type,
    any severity, both acked and unacked."""
    from app.models.db_models import Event
    import datetime as _dt
    cutoff = datetime.now(timezone.utc) - _dt.timedelta(hours=since_hours)
    q = select(Event).where(Event.first_seen_at >= cutoff)
    if type:
        q = q.where(Event.event_type == type)
    if severity:
        q = q.where(Event.severity == severity)
    if acknowledged is True:
        q = q.where(Event.acknowledged == True)  # noqa: E712
    elif acknowledged is False:
        q = q.where(Event.acknowledged == False)  # noqa: E712
    q = q.order_by(Event.first_seen_at.desc()).limit(limit)

    result = await db.execute(q)
    rows = result.scalars().all()
    items = [_event_row_to_item(r) for r in rows]
    return {"count": len(items), "events": items}


@router.get("/events/stats")
async def events_stats(db: AsyncSession = Depends(get_db)):
    """Dashboard badge stats: total + unacked + per-type + per-severity."""
    from app.models.db_models import Event
    # Run a few small aggregate queries rather than pull the full table
    total = (await db.execute(select(func.count(Event.id)))).scalar() or 0
    unack = (await db.execute(
        select(func.count(Event.id)).where(Event.acknowledged == False)  # noqa: E712
    )).scalar() or 0
    crit_unack = (await db.execute(
        select(func.count(Event.id))
        .where(Event.acknowledged == False)  # noqa: E712
        .where(Event.severity == "critical")
    )).scalar() or 0
    by_type: dict[str, int] = {}
    rows = (await db.execute(
        select(Event.event_type, func.count(Event.id))
        .group_by(Event.event_type)
    )).all()
    for et, n in rows:
        by_type[et] = n
    by_sev: dict[str, int] = {}
    rows = (await db.execute(
        select(Event.severity, func.count(Event.id))
        .group_by(Event.severity)
    )).all()
    for sv, n in rows:
        by_sev[sv] = n
    return {
        "total": total,
        "unacknowledged": unack,
        "critical_unacked": crit_unack,
        "by_type": by_type,
        "by_severity": by_sev,
    }


@router.post("/events/{event_id}/ack")
async def ack_event(event_id: int, db: AsyncSession = Depends(get_db)):
    """Mark a single event acknowledged."""
    from app.models.db_models import Event
    from sqlalchemy import update as _update
    await db.execute(
        _update(Event)
        .where(Event.id == event_id)
        .values(acknowledged=True, acknowledged_at=datetime.now(timezone.utc))
    )
    await db.commit()
    return {"ok": True, "event_id": event_id}


@router.post("/events/ack-all")
async def ack_all_events(
    type: Annotated[str | None, Query(description="Only ack this event_type")] = None,
    db: AsyncSession = Depends(get_db),
):
    """Mark all (optionally type-filtered) events acknowledged."""
    from app.models.db_models import Event
    from sqlalchemy import update as _update
    stmt = (
        _update(Event)
        .where(Event.acknowledged == False)  # noqa: E712
        .values(acknowledged=True, acknowledged_at=datetime.now(timezone.utc))
    )
    if type:
        stmt = stmt.where(Event.event_type == type)
    result = await db.execute(stmt)
    await db.commit()
    return {"ok": True, "acked": result.rowcount or 0}


@router.delete("/events/{event_id}")
async def delete_event(event_id: int, db: AsyncSession = Depends(get_db)):
    """Permanently delete an event (rarely used — for false positives)."""
    from app.models.db_models import Event
    from sqlalchemy import delete as _delete
    await db.execute(_delete(Event).where(Event.id == event_id))
    await db.commit()
    return {"ok": True, "event_id": event_id}


# ── OUI registry admin (Wireshark manuf file reload + stats) ──

@router.get("/admin/oui/stats")
async def oui_stats():
    """Registry counts + last load time. Use this to sanity-check after a
    manual update via scripts/update_oui.py."""
    from app.services.oui_db import get_stats
    return get_stats()


@router.post("/admin/oui/refresh")
async def oui_refresh():
    """Re-read backend/app/data/manuf.txt from disk. Run after
    `python3 scripts/update_oui.py` to pick up a refreshed registry
    without a backend restart."""
    from app.services.oui_db import load_manuf, get_stats
    n = load_manuf()
    return {"ok": True, "entries_loaded": n, "stats": get_stats()}


# ── MAC whitelist CRUD (for suppressing new_probe_mac / new_ap events) ──

@router.get("/whitelist/mac")
async def list_mac_whitelist(db: AsyncSession = Depends(get_db)):
    from app.models.db_models import WhitelistedMAC
    rows = (await db.execute(select(WhitelistedMAC).order_by(WhitelistedMAC.mac))).scalars().all()
    return {
        "count": len(rows),
        "entries": [
            {"mac": r.mac, "label": r.label,
             "added_at": r.added_at.isoformat() if r.added_at else ""}
            for r in rows
        ],
    }


@router.post("/whitelist/mac")
async def add_mac_whitelist(body: dict, db: AsyncSession = Depends(get_db)):
    """Body: {"mac": "AA:BB:CC" (prefix or full), "label": "My iPhone"}."""
    from app.models.db_models import WhitelistedMAC
    mac = (body.get("mac") or "").strip().upper()
    label = body.get("label") or None
    if not mac:
        return {"error": "mac is required"}
    existing = await db.execute(select(WhitelistedMAC).where(WhitelistedMAC.mac == mac))
    if existing.scalar_one_or_none():
        return {"ok": True, "duplicate": True, "mac": mac}
    db.add(WhitelistedMAC(mac=mac, label=label))
    await db.commit()
    _event_detector.whitelist_mac(mac)
    return {"ok": True, "mac": mac}


@router.delete("/whitelist/mac")
async def remove_mac_whitelist(body: dict, db: AsyncSession = Depends(get_db)):
    from app.models.db_models import WhitelistedMAC
    from sqlalchemy import delete as _delete
    mac = (body.get("mac") or "").strip().upper()
    if not mac:
        return {"error": "mac is required"}
    await db.execute(_delete(WhitelistedMAC).where(WhitelistedMAC.mac == mac))
    await db.commit()
    _event_detector.unwhitelist_mac(mac)
    return {"ok": True, "mac": mac}
