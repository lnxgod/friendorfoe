"""Drone detection ingestion endpoints for ESP32 sensor nodes."""

import json
import logging
import re
import secrets as _secrets
import time
from collections import defaultdict, deque
from datetime import datetime, timezone, timedelta
from typing import Annotated

from fastapi import APIRouter, Depends, Query, Request
from sqlalchemy import func, select
from sqlalchemy.exc import OperationalError
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
from app.services.classifier import classify_detection, normalize_detection_source
from app.services.enrichment_ble import BLEEnricher
from app.services.probe_identity import (
    normalize_probe_identity,
    probe_identity_from_event,
)
from app.services.applied_calibration import AppliedCalibrationStore
from app.services.calibration_mode import CalibrationModeCoordinator
from app.services.phone_calibration import PhoneCalibrationManager

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/detections", tags=["detections"])

_phone_cal_mgr = PhoneCalibrationManager()
_applied_cal_store = AppliedCalibrationStore()
_calibration_mode = CalibrationModeCoordinator(_phone_cal_mgr, _applied_cal_store)


# Manufacturer / model substrings whose devices rotate BT Private Resolvable
# Addresses in ways that defeat MAC-based identity. For these classes we
# promote the scanner's BLE-JA3 hash to a stable `FP:<ja3>` grouping key so
# EntityTracker/BLEEnricher/AnomalyDetector/Triangulation see one logical
# threat instead of a new "device" every RPA rotation (~15 min).
#
# Mirror of the Android-side rule in GlassesDetector.computeFingerprintKey —
# keeping the two lists in sync avoids divergent behaviour between the phone
# UI and the backend privacy dashboard.
_FP_ROTATING_MFRS = ("meta", "ray-ban", "rayban", "oakley", "luxottica")
_FP_ROTATING_MODEL_SUBSTRS = ("meta", "ray-ban", "rayban", "oakley",
                               "luxottica", "quest", "smart glasses")


def _grouping_model(det) -> str:
    """Return a model string suitable for downstream fingerprint-aware
    consumers. For MAC-rotating high-risk classes with a non-zero JA3 hash,
    we return `FP:<ja3>` so every rotated MAC from the same physical device
    collapses into one entity. For everything else we return the original
    `det.model` unchanged — AirTags and generic BLE peripherals keep their
    existing per-MAC identity so two distinct trackers never merge.
    """
    orig = (det.model or "").strip()
    if orig.startswith("FP:"):
        return orig  # already prefixed upstream (e.g. ASTM Remote ID serials)
    ja3 = (getattr(det, "ble_ja3", "") or "").strip().lower()
    if not ja3 or ja3 in ("0", "00000000"):
        return orig
    mfr_l = (det.manufacturer or "").lower()
    model_l = orig.lower()
    rotating = (
        any(m in mfr_l for m in _FP_ROTATING_MFRS) or
        any(s in model_l for s in _FP_ROTATING_MODEL_SUBSTRS)
    )
    if not rotating:
        return orig
    return f"FP:{ja3}"


_CALIBRATION_UUID_RE = re.compile(
    r"^cafe[0-9a-f]{4}-0000-1000-8000-[0-9a-f]{12}$"
)


def _iter_ble_service_uuids(ble_svc_uuids: str | None):
    for token in (ble_svc_uuids or "").split(","):
        norm = token.strip().lower()
        if norm:
            yield norm


def _is_calibration_uuid(uuid_token: str | None) -> bool:
    return bool(uuid_token and _CALIBRATION_UUID_RE.fullmatch(uuid_token))


def _is_calibration_beacon_detection(det) -> bool:
    return any(
        _is_calibration_uuid(token)
        for token in _iter_ble_service_uuids(getattr(det, "ble_svc_uuids", None))
    )


def _find_active_calibration_session(ble_svc_uuids: str | None):
    for token in _iter_ble_service_uuids(ble_svc_uuids):
        if not _is_calibration_uuid(token):
            continue
        session = _calibration_mode.active_for_uuid(token)
        if session is not None:
            return session
    return None


def _is_calibration_tracking_id(drone_id: str | None) -> bool:
    return bool((drone_id or "").startswith("FP:CAL-"))


def _source_tier_for_observation(obs, classification: str | None = None) -> str:
    if obs is None:
        return "unknown"
    if _is_calibration_tracking_id(getattr(obs, "model", None)):
        return "calibration"
    source = (getattr(obs, "source", "") or "").lower()
    cls = (classification or getattr(obs, "classification", "") or "").lower()
    if source in ("wifi_assoc", "ble_fingerprint", "wifi_oui"):
        return "diagnostic"
    if source == "wifi_probe_request" and cls not in {
        "confirmed_drone", "likely_drone", "test_drone", "possible_drone",
    }:
        return "diagnostic"
    return "drone_grade"


def _calibration_state_for_item(drone_id: str | None = None) -> str:
    if _is_calibration_tracking_id(drone_id):
        return "calibration_session"
    summary = _applied_cal_store.summary()
    if summary.get("is_trusted") and summary.get("is_active"):
        return "active"
    return "defaults"


def _geometry_trust_for_observation(obs, drone_id: str | None = None) -> str:
    if obs is None:
        return "unknown"
    if _is_calibration_tracking_id(drone_id) or _is_calibration_tracking_id(getattr(obs, "model", None)):
        return "calibration_session"
    if getattr(obs, "range_model", None) == "per_listener":
        return "per_listener_calibrated"
    summary = _applied_cal_store.summary()
    if summary.get("is_trusted") and getattr(obs, "distance_source", None) == "backend_rssi":
        return "trusted_backend_model"
    if getattr(obs, "distance_source", None) == "backend_rssi":
        return "default_backend_model"
    if getattr(obs, "distance_source", None) == "scanner":
        return "scanner_firmware_model"
    return "unknown"


def _dominant_range_authority(observations: list) -> str | None:
    counts: dict[str, int] = {}
    for obs in observations:
        src = getattr(obs, "distance_source", None) or "none"
        counts[src] = counts.get(src, 0) + 1
    if not counts:
        return None
    return max(counts.items(), key=lambda item: item[1])[0]


def _uncertainty_for_map_item(position_source: str | None,
                              source_tier: str,
                              accuracy_m: float | None,
                              range_m: float | None) -> float | None:
    uncertainty = accuracy_m if accuracy_m is not None else range_m
    if source_tier == "diagnostic":
        uncertainty = max(float(uncertainty or 0.0), 25.0)
    return uncertainty

# ---------------------------------------------------------------------------
# In-memory ring buffer for recent detections (fast path, no DB needed)
# ---------------------------------------------------------------------------

_MAX_RECENT = 50000
_recent_detections: deque[StoredDetection] = deque(maxlen=_MAX_RECENT)

# ---------------------------------------------------------------------------
# In-memory node heartbeat tracker (works without DB)
# ---------------------------------------------------------------------------

_node_heartbeats: dict[str, dict] = {}
_SOURCE_FIXUP_RECENT_WINDOW_S = 300.0
_node_source_fixup_events: dict[str, deque[tuple[float, str]]] = defaultdict(
    lambda: deque(maxlen=256)
)

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
_MAC_RE = re.compile(r"[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}")


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


def _device_id_mac_suffix(device_id: str | None) -> str | None:
    if not device_id or "_" not in device_id:
        return None
    suffix = device_id.rsplit("_", 1)[-1].strip().upper()
    if len(suffix) != 6 or any(ch not in "0123456789ABCDEF" for ch in suffix):
        return None
    return suffix


def _wifi_assoc_mentions_known_node(drone_id: str | None) -> bool:
    if not drone_id:
        return False
    known_suffixes = {
        suffix
        for suffix in (_device_id_mac_suffix(device_id) for device_id in _node_heartbeats)
        if suffix
    }
    if not known_suffixes:
        return False
    for mac in _MAC_RE.findall(drone_id):
        compact = mac.replace(":", "").upper()
        if compact[-6:] in known_suffixes:
            return True
    return False


def _record_source_fixup(device_id: str,
                         raw_source: str,
                         normalized_source: str,
                         ts: float) -> None:
    hb = _node_heartbeats.get(device_id)
    if hb is None:
        return
    hb["source_fixups_total"] = int(hb.get("source_fixups_total", 0) or 0) + 1
    by_rule = hb.setdefault("source_fixups_by_rule", {})
    rule_key = f"{raw_source}->{normalized_source}"
    by_rule[rule_key] = int(by_rule.get(rule_key, 0) or 0) + 1
    hb["last_source_fixup_at"] = ts

    events = _node_source_fixup_events[device_id]
    events.append((ts, rule_key))
    cutoff = ts - _SOURCE_FIXUP_RECENT_WINDOW_S
    while events and events[0][0] < cutoff:
        events.popleft()


def _recent_source_fixups(device_id: str,
                          now_ts: float) -> tuple[int, dict[str, int]]:
    events = _node_source_fixup_events.get(device_id)
    if not events:
        return 0, {}
    cutoff = now_ts - _SOURCE_FIXUP_RECENT_WINDOW_S
    while events and events[0][0] < cutoff:
        events.popleft()
    by_rule: dict[str, int] = {}
    for _, rule in events:
        by_rule[rule] = by_rule.get(rule, 0) + 1
    return len(events), by_rule


_SCANNER_TIME_FRESH_WINDOW_S = 30
_UPLINK_TIME_GOOD_WINDOW_S = 20
_UPLINK_TIME_FALLBACK_WINDOW_S = 60


def _scanner_time_sync_health(scanner: dict) -> str:
    state = str(scanner.get("time_sync_state") or "").lower()
    try:
        age_s = float(scanner.get("time_last_valid_age_s"))
    except (TypeError, ValueError):
        age_s = None

    if state == "fresh" and age_s is not None and age_s <= _SCANNER_TIME_FRESH_WINDOW_S:
        return "fresh"
    if state == "waiting":
        return "waiting"
    if state == "stale":
        return "stale"
    return "unknown"


def _uplink_time_sync_health(info: dict | None) -> str:
    if not isinstance(info, dict) or not info:
        return "unknown"

    last_fetch_ok = info.get("last_fetch_ok")
    time_source = str(info.get("time_source") or "none").lower()
    try:
        last_success_age_s = float(info.get("last_success_age_s"))
    except (TypeError, ValueError):
        last_success_age_s = None

    if last_fetch_ok is True and last_success_age_s is not None and last_success_age_s <= _UPLINK_TIME_GOOD_WINDOW_S:
        return "good"
    if (
        last_fetch_ok is False
        and time_source != "none"
        and last_success_age_s is not None
        and last_success_age_s <= _UPLINK_TIME_FALLBACK_WINDOW_S
    ):
        return "warning"
    if time_source == "none" or last_success_age_s is None or last_success_age_s > _UPLINK_TIME_FALLBACK_WINDOW_S:
        return "bad"
    return "unknown"


# GPS-RSSI spoof detector: drone_id → {gps_positions: [(lat,lon,t)], rssi_values: [(rssi,t)]}
_spoof_tracker: dict[str, dict] = {}

# Drone alert tracker: drone_id → last_seen timestamp
_known_drones: dict[str, float] = {}
_drone_alerts: list[dict] = []  # Persistent drone-specific alerts
_DRONE_REAPPEAR_SEC = 300  # Re-alert if drone gone >5min and comes back

# Lock-on command polled by uplinks; per-node tracking lives in drone_tracker.
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

# v0.63: phone-driven walk calibration. The Android app advertises BLE
# from known GPS positions; backend joins the trace × sensor sightings
# and fits a per-listener log-distance model.


# ---------------------------------------------------------------------------
# Calibration auth — single shared bearer token in the X-Cal-Token header.
# Set via the FOF_CAL_TOKEN env var; if unset we generate a random one at
# startup and log it (so dev still works, prod ops sets the env). HMAC is
# overkill for a property-scale tool whose threat model is "stop neighbors
# from polluting our calibration", which a static secret over LAN handles.
# ---------------------------------------------------------------------------
import os as _os
from fastapi import HTTPException, Header

# Default token matches the Android app's DetectionPrefs.calibrationToken
# default so a fresh-install phone + fresh-start backend just work with
# zero paste. FOF_CAL_TOKEN env still overrides for prod deployments.
# Change both sides together if you rotate the default.
_DEV_DEFAULT_CAL_TOKEN = "chompchomp"
_CAL_TOKEN = _os.environ.get("FOF_CAL_TOKEN") or _DEV_DEFAULT_CAL_TOKEN
if not _os.environ.get("FOF_CAL_TOKEN"):
    logger.warning(
        "FOF_CAL_TOKEN not set — using dev default '%s'. "
        "Pin FOF_CAL_TOKEN env for production.",
        _CAL_TOKEN,
    )


def _require_cal_token(x_cal_token: str | None) -> None:
    """Reject requests missing or with an incorrect calibration bearer."""
    if not x_cal_token or not _secrets.compare_digest(x_cal_token, _CAL_TOKEN):
        raise HTTPException(status_code=401, detail="invalid X-Cal-Token")


def _legacy_calibration_removed() -> None:
    raise HTTPException(
        status_code=410,
        detail="Legacy backend calibration flow was removed. Use the Android walk calibration workflow.",
    )


def _position_mode(node: SensorNode | None) -> str:
    return (getattr(node, "position_mode", None) or "active") if node else "active"


def _geometry_enabled_for_node(node: SensorNode | None) -> bool:
    return _position_mode(node) == "active"


async def _resolve_sensor_position(
    device_id: str,
    device_lat: float | None,
    device_lon: float | None,
    device_alt: float | None,
    db: AsyncSession | None,
) -> tuple[float | None, float | None, float | None, str, str, bool]:
    """
    Look up sensor node in DB. If it's a registered fixed node, use the
    registered position (ignoring GPS from payload). Otherwise use GPS.
    Returns (lat, lon, alt, sensor_type, position_mode, geometry_enabled).
    """
    if db:
        try:
            result = await db.execute(
                select(SensorNode).where(SensorNode.device_id == device_id)
            )
            node = result.scalar_one_or_none()
            if node and not _geometry_enabled_for_node(node):
                node.last_seen = datetime.now(timezone.utc)
                await db.commit()
                return None, None, node.alt, node.sensor_type, _position_mode(node), False
            if node and node.is_fixed:
                node.last_seen = datetime.now(timezone.utc)
                await db.commit()
                return node.lat, node.lon, node.alt, node.sensor_type, _position_mode(node), True
            elif node and not node.is_fixed:
                if device_lat is not None and device_lon is not None:
                    node.lat = device_lat
                    node.lon = device_lon
                    node.alt = device_alt
                    node.last_seen = datetime.now(timezone.utc)
                    await db.commit()
                return device_lat, device_lon, device_alt, node.sensor_type, _position_mode(node), True
        except Exception as e:
            try:
                await db.rollback()
            except Exception:
                pass
            logger.warning("DB lookup failed for %s: %s", device_id, e)

    return device_lat, device_lon, device_alt, "outdoor", "active", True


def _heartbeat_is_online_recent(hb: dict, now: float, max_age_s: float = 120.0) -> bool:
    return (now - float(hb.get("last_seen", 0) or 0)) < max_age_s


async def _live_walk_sensor_rows(
    db: AsyncSession,
    *,
    max_age_s: float = 120.0,
) -> list[dict]:
    """Return recent online geometry-capable nodes for phone calibration.

    This is deliberately stricter than the generic node registry: the walk
    flow should reflect only the sensors that are alive *now* and eligible
    to contribute to geometry.
    """
    now = time.time()
    db_nodes: dict[str, SensorNode] = {}
    try:
        result = await db.execute(select(SensorNode))
        db_nodes = {node.device_id: node for node in result.scalars().all()}
    except Exception as exc:
        logger.warning("walk/sensors DB read failed: %s", exc)

    sensors: list[dict] = []
    for hb in _node_heartbeats.values():
        device_id = hb.get("device_id")
        if not device_id or not _heartbeat_is_online_recent(hb, now, max_age_s):
            continue

        db_node = db_nodes.get(device_id)
        if db_node and not _geometry_enabled_for_node(db_node):
            continue

        use_registered = bool(db_node and db_node.is_fixed)
        lat = db_node.lat if use_registered else hb.get("lat", db_node.lat if db_node else None)
        lon = db_node.lon if use_registered else hb.get("lon", db_node.lon if db_node else None)
        if lat is None or lon is None or abs(float(lat)) <= 0.1 or abs(float(lon)) <= 0.1:
            continue

        sensors.append({
            "device_id": device_id,
            "name": (db_node.name if db_node and db_node.name else device_id),
            "lat": float(lat),
            "lon": float(lon),
            "is_fixed": bool(db_node.is_fixed) if db_node else False,
            "online": True,
            "age_s": round(now - float(hb.get("last_seen", now) or now), 1),
            "ip": hb.get("ip"),
            "position_mode": _position_mode(db_node),
            "geometry_enabled": True,
        })

    sensors.sort(key=lambda row: (row["name"], row["device_id"]))
    return sensors


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
    prev_hb = _node_heartbeats.get(batch.device_id, {})
    _node_heartbeats[batch.device_id] = {
        "device_id": batch.device_id,
        "last_seen": received_at,
        "detection_count": len(batch.detections),
        "total_batches": prev_hb.get("total_batches", 0) + 1,
        "total_detections": prev_hb.get("total_detections", 0) + len(batch.detections),
        "lat": batch.device_lat,
        "lon": batch.device_lon,
        "ip": source_ip,
        "firmware_version": batch.firmware_version or prev_hb.get("firmware_version"),
        "board_type": batch.board_type or prev_hb.get("board_type"),
        "scanners": batch.scanners or prev_hb.get("scanners"),
        "time_sync": batch.time_sync or prev_hb.get("time_sync"),
        "scan_mode": batch.scan_mode or prev_hb.get("scan_mode"),
        "scan_profile": batch.scan_profile or prev_hb.get("scan_profile"),
        "calibration_uuid": batch.calibration_uuid or prev_hb.get("calibration_uuid"),
        "dedup_seen": batch.dedup_seen if batch.dedup_seen is not None else prev_hb.get("dedup_seen"),
        "dedup_sent": batch.dedup_sent if batch.dedup_sent is not None else prev_hb.get("dedup_sent"),
        "dedup_collapsed": (
            batch.dedup_collapsed
            if batch.dedup_collapsed is not None
            else prev_hb.get("dedup_collapsed")
        ),
        "cal_seen": batch.cal_seen if batch.cal_seen is not None else prev_hb.get("cal_seen"),
        "cal_sent": batch.cal_sent if batch.cal_sent is not None else prev_hb.get("cal_sent"),
        "wifi_ssid": batch.wifi_ssid or prev_hb.get("wifi_ssid"),
        "wifi_rssi": batch.wifi_rssi if batch.wifi_rssi is not None else prev_hb.get("wifi_rssi"),
        "source_fixups_total": prev_hb.get("source_fixups_total", 0),
        "source_fixups_by_rule": dict(prev_hb.get("source_fixups_by_rule", {})),
        "last_source_fixup_at": prev_hb.get("last_source_fixup_at"),
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
        try:
            await db.rollback()
        except Exception:
            pass
        pass  # Don't break ingestion if auto-register fails

    # Resolve sensor position (fixed node overrides GPS)
    sensor_lat, sensor_lon, sensor_alt, sensor_type, position_mode, geometry_enabled = await _resolve_sensor_position(
        batch.device_id, batch.device_lat, batch.device_lon, batch.device_alt, db
    )
    if not geometry_enabled:
        _sensor_tracker.remove_sensor(batch.device_id)

    logger.info(
        "Drone batch from device=%s count=%d sensor_pos=(%s, %s) position_mode=%s geometry_enabled=%s",
        batch.device_id, len(batch.detections), sensor_lat, sensor_lon, position_mode, geometry_enabled,
    )

    accepted = 0
    db_detections = []

    dedup_skipped = 0
    newly_emitted_events: list[tuple[str, str]] = []
    for det in batch.detections:
        # Skip our own infrastructure SSIDs — never store or process FoF-* detections
        if det.ssid and det.ssid.upper().startswith("FOF-") and not det.ssid.upper().startswith("FOF-DRONE-"):
            continue

        raw_source = det.source
        det.source = normalize_detection_source(
            raw_source,
            drone_id=det.drone_id,
            manufacturer=det.manufacturer,
            latitude=det.latitude,
            longitude=det.longitude,
            operator_lat=det.operator_lat,
            operator_lon=det.operator_lon,
            operator_id=det.operator_id,
            self_id_text=det.self_id_text,
        )
        if det.source != raw_source:
            _record_source_fixup(batch.device_id, raw_source, det.source, received_at)

        # Drop our own node association chatter. This is useful RF context for
        # debugging WiFi, but it is not a drone signal and it pollutes the
        # operator feed when nearby FoF uplinks are visible over the air.
        if det.source == "wifi_assoc" and _wifi_assoc_mentions_known_node(det.drone_id):
            continue

        is_calibration_beacon = (
            det.source == "ble_fingerprint" and
            _is_calibration_beacon_detection(det)
        )

        # Drop duplicates (scanner re-emission, uplink offline-replay, batch retries)
        if (not is_calibration_beacon and
                _ingest_dedup_hit(det.drone_id, det.source, det.bssid or "", received_at)):
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

        # Data-flags byte: current scanners emit ble_apple_flags even when 0,
        # so the backend can distinguish "all flags false" from "absent".
        ainfo = det.ble_apple_flags or 0

        # Honest Apple classification. v0.58+ scanners send "Apple Device"
        # directly, with enriched flag labels still applied below. Scanners
        # that resolve to Handoff/AirPlay/AirPods/AirTag send their specific
        # label and bypass this block entirely.
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
                # No Continuity evidence. Apple doesn't broadcast model here,
                # so keep the label generic.
                det.manufacturer = "Apple Device"

        cal_session = None
        if det.source == "ble_fingerprint" and is_calibration_beacon:
            cal_session = _find_active_calibration_session(
                getattr(det, "ble_svc_uuids", None)
            )
            if cal_session is not None and \
               det.rssi is not None and \
               sensor_lat is not None and \
               sensor_lon is not None:
                _phone_cal_mgr.add_sensor_sample(
                    session_id=cal_session.session_id,
                    sensor_id=batch.device_id,
                    sensor_lat=sensor_lat,
                    sensor_lon=sensor_lon,
                    rssi=det.rssi,
                    ts_s=(det.timestamp / 1000.0)
                          if det.timestamp and det.timestamp > 1e12
                          else received_at,
                    scanner_slots_seen=getattr(det, "scanner_slots_seen", None),
                )

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
                    estimated_distance_m=None,
                    drone_lat=det.latitude,
                    drone_lon=det.longitude,
                    drone_alt=det.altitude_m,
                    heading_deg=det.heading_deg,
                    speed_mps=det.speed_mps,
                    confidence=max(det.confidence, 0.85),
                    source=det.source,
                    manufacturer=det.manufacturer,
                    model=f"FP:CAL-{cal_session.session_id}",
                    classification="unknown_device",
                    ssid=det.ssid,
                    bssid=det.bssid,
                    ie_hash=getattr(det, "ie_hash", None),
                    operator_lat=det.operator_lat,
                    operator_lon=det.operator_lon,
                    operator_id=det.operator_id,
                    sensor_type=sensor_type,
                    timestamp=det_ts,
                    range_authority="backend_rssi",
                )
                accepted += 1
                continue

            # Ignore inactive or geometry-excluded calibration beacons
            # for operator-facing feeds. They are only meaningful while
            # a phone walk session is actively collecting samples.
            continue

        # Classify the detection
        classification, adj_confidence = classify_detection(
            source=det.source,
            confidence=det.confidence,
            ssid=det.ssid,
            manufacturer=det.manufacturer,
            drone_id=det.drone_id,
            model=det.model,
            bssid=det.bssid,
            latitude=det.latitude,
            longitude=det.longitude,
            operator_lat=det.operator_lat,
            operator_lon=det.operator_lon,
            operator_id=det.operator_id,
            self_id_text=det.self_id_text,
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

        # Promote JA3 to a stable FP: grouping key for MAC-rotating high-risk
        # classes (Meta/Ray-Ban/Oakley/Luxottica/Quest). All downstream
        # fingerprint-aware consumers key on `model.startswith("FP:")` so
        # rotating RPAs now collapse into one logical device/entity/threat
        # instead of producing a fresh identity every ~15 min.
        grouping_model = _grouping_model(det)

        # Anomaly detection (fingerprint-aware, whitelisted, mesh-aware)
        _anomaly_detector.ingest(
            drone_id=det.drone_id, source=det.source,
            confidence=det.confidence, rssi=det.rssi or 0,
            ssid=det.ssid or "", bssid=det.bssid or "",
            manufacturer=det.manufacturer or "", model=grouping_model,
            device_id=batch.device_id, received_at=received_at,
        )

        # BLE enrichment
        _ble_enricher.ingest(
            drone_id=det.drone_id, source=det.source,
            confidence=det.confidence, rssi=det.rssi or 0,
            bssid=det.bssid or "", manufacturer=det.manufacturer or "",
            model=grouping_model, device_id=batch.device_id,
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
            ble_apple_flags=ainfo,
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
            model=grouping_model,
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
                model=grouping_model,
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
                model=grouping_model,
                probed_ssids=det.probed_ssids,
                ie_hash=getattr(det, "ie_hash", None),
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
        # Per-detection timestamp from scanner's epoch-synced clock; fall back
        # to batch receive time when the scanner timestamp is not epoch-valid.
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
            estimated_distance_m=det.estimated_distance_m,
            drone_lat=det.latitude,
            drone_lon=det.longitude,
            drone_alt=det.altitude_m,
            heading_deg=det.heading_deg,
            speed_mps=det.speed_mps,
            confidence=adj_confidence,
            source=det.source,
            manufacturer=det.manufacturer,
            # Passing the JA3-promoted model here too lets the
            # triangulator key all of a rotating Meta pair's RPAs under
            # one tracking_id — otherwise the EKF/particle filter would
            # start over every rotation and never accumulate enough
            # observations to produce a tight fix.
            model=grouping_model,
            classification=classification,
            ssid=det.ssid,
            bssid=det.bssid,
            ie_hash=getattr(det, "ie_hash", None),
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
            if _is_calibration_tracking_id(d.drone_id):
                continue
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
                    latitude=d.lat,
                    longitude=d.lon,
                )[0] if best_obs else None,
                observations_json=json.dumps([
                    {
                        "device_id": o.device_id,
                        "rssi": o.rssi,
                        "distance_m": o.estimated_distance_m,
                        "scanner_distance_m": o.scanner_estimated_distance_m,
                        "backend_distance_m": o.backend_estimated_distance_m,
                        "distance_source": o.distance_source,
                        "range_model": o.range_model,
                        "sensor_lat": o.sensor_lat,
                        "sensor_lon": o.sensor_lon,
                        "ssid": o.ssid,
                    }
                    for o in d.observations
                ]),
            )
            db.add(tri)

        await db.commit()
    except Exception as e:
        try:
            await db.rollback()
        except Exception:
            pass
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
    located = _sensor_tracker.get_located_drones(include_probe_diagnostics=include_probes)
    sensors = _sensor_tracker.get_active_sensors()

    # Get signal tracker data for approach/departure velocity
    signal_tracks = _signal_tracker.get_live_tracks(limit=200, active_within_s=30.0)
    track_velocity = {}
    if signal_tracks and "tracks" in signal_tracks:
        for t in signal_tracks["tracks"]:
            track_velocity[t.get("track_id", "")] = t.get("approach_speed_mps", 0)

    drone_items = []
    for d in located:
        if _is_calibration_tracking_id(d.drone_id):
            continue
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
        source_tier = _source_tier_for_observation(best_obs, cls)
        range_authority = _dominant_range_authority(d.observations)
        geometry_trust = _geometry_trust_for_observation(best_obs, d.drone_id)
        uncertainty_m = _uncertainty_for_map_item(
            d.position_source,
            source_tier,
            d.accuracy_m,
            d.range_m,
        )
        calibration_state = _calibration_state_for_item(d.drone_id)

        # Skip positions with huge uncertainty — they're noise
        if uncertainty_m and uncertainty_m > 200:
            continue

        # Determine if this is a probe device
        is_probe = bool(best_obs and best_obs.source == "wifi_probe_request") or \
            (d.drone_id or "").startswith("PROBE:")
        if source_tier == "diagnostic":
            if not include_probes:
                continue
            if d.sensor_count < 2:
                continue
            if uncertainty_m is None or uncertainty_m > 200:
                continue

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
                scanner_estimated_distance_m=o.scanner_estimated_distance_m,
                backend_estimated_distance_m=o.backend_estimated_distance_m,
                distance_source=o.distance_source,
                range_model=o.range_model,
                range_authority=o.distance_source,
                source_tier=_source_tier_for_observation(o, cls),
                geometry_trust=_geometry_trust_for_observation(o, d.drone_id),
                uncertainty_m=(
                    o.estimated_distance_m
                    if o.estimated_distance_m is not None and source_tier != "diagnostic"
                    else (max(float(o.estimated_distance_m or 0.0), 25.0)
                          if o.estimated_distance_m is not None else None)
                ),
                confidence=o.confidence,
                source=o.source,
                ssid=o.ssid,
                bssid=o.bssid,
                ie_hash=o.ie_hash,
            )
            for o in d.observations
        ]
        # Classify the identity derivation path so the dashboard can distinguish
        # ASTM-Remote-ID-verified drones from BLE-fingerprint-guessed identities
        # without having to duplicate the prefix heuristic client-side.
        _did = d.drone_id or ""
        if _did.startswith("rid_"):
            identity_source = "rid"
        elif _did.startswith("PROBE:"):
            identity_source = "probe_fingerprint"
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
            range_authority=range_authority,
            geometry_trust=geometry_trust,
            source_tier=source_tier,
            uncertainty_m=uncertainty_m,
            calibration_state=calibration_state,
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
            geometry_enabled = _geometry_enabled_for_node(sn)
            db_positions[sn.device_id] = {
                "lat": sn.lat, "lon": sn.lon, "alt": sn.alt,
                "name": sn.name, "is_fixed": sn.is_fixed,
                "position_mode": _position_mode(sn),
                "geometry_enabled": geometry_enabled,
                "geometry_status": (
                    "excluded_for_canary_testing"
                    if not geometry_enabled
                    else ("fixed" if sn.is_fixed else "dynamic")
                ),
            }
    except Exception:
        pass  # DB unavailable, use heartbeat data only

    nodes = []
    for node in _node_heartbeats.values():
        age = now - node["last_seen"]
        recent_fixups, recent_by_rule = _recent_source_fixups(node["device_id"], now)
        scanners = []
        for sc in (node.get("scanners") or []):
            if isinstance(sc, dict):
                sc_entry = dict(sc)
                sc_entry["time_sync_health"] = _scanner_time_sync_health(sc_entry)
                scanners.append(sc_entry)
            else:
                scanners.append(sc)
        time_sync = None
        if isinstance(node.get("time_sync"), dict):
            time_sync = dict(node["time_sync"])
            time_sync["sync_health"] = _uplink_time_sync_health(time_sync)
        entry = {
            **node,
            "scanners": scanners,
            "time_sync": time_sync,
            "age_s": round(age, 1),
            "online": age < 120,
            "position_mode": "active",
            "geometry_enabled": True,
            "geometry_status": "dynamic",
            "source_fixups_since_boot": int(node.get("source_fixups_total", 0) or 0),
            "source_fixups_recent": recent_fixups,
            "source_fixups_recent_by_rule": recent_by_rule,
            "source_fixups_recent_window_s": int(_SOURCE_FIXUP_RECENT_WINDOW_S),
        }
        # Override lat/lon with registered DB position if available
        db_pos = db_positions.get(node["device_id"])
        if db_pos and db_pos.get("is_fixed"):
            entry["lat"] = db_pos["lat"]
            entry["lon"] = db_pos["lon"]
            entry["alt"] = db_pos.get("alt", 0)
            entry["name"] = db_pos.get("name", "")
            entry["gps_registered"] = True
            entry["position_mode"] = db_pos.get("position_mode", "active")
            entry["geometry_enabled"] = bool(db_pos.get("geometry_enabled", True))
            entry["geometry_status"] = db_pos.get("geometry_status", "fixed")
        elif db_pos and not db_pos.get("geometry_enabled", True):
            entry["lat"] = db_pos["lat"]
            entry["lon"] = db_pos["lon"]
            entry["alt"] = db_pos.get("alt", 0)
            entry["name"] = db_pos.get("name", "")
            entry["gps_registered"] = True
            entry["position_mode"] = db_pos.get("position_mode", "excluded")
            entry["geometry_enabled"] = False
            entry["geometry_status"] = db_pos.get("geometry_status", "excluded_for_canary_testing")
        else:
            entry["gps_registered"] = False
            if db_pos:
                entry["position_mode"] = db_pos.get("position_mode", "active")
                entry["geometry_enabled"] = bool(db_pos.get("geometry_enabled", True))
                entry["geometry_status"] = db_pos.get("geometry_status", "dynamic")
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
# GET /detections/threats — unified threat feed for the privacy dashboard
#
# Consolidates every "this matters" signal into one flat list, each item
# carrying a triangulated lat/lon + accuracy when geometry allows. The
# front-end threats.html page consumes this directly — one endpoint, one
# render, no cross-referencing four APIs client-side.
#
# Sources:
#   - threat-category entities (Meta / Ray-Ban / Flipper / Pwnagotchi /
#     AirTag / Flock / Hidden Camera / etc. — auto-classified by
#     EntityTracker._categorize_entity via HIGH_RISK_KEYWORDS)
#   - lingering-tracker alerts from AnomalyDetector (dwell > 30min)
#   - RF anomalies from RFAnomalyDetector (deauth flood, beacon spam,
#     pwnagotchi, karma, evil-twin, attack volume)
#   - located drones with confirmed classification
# ---------------------------------------------------------------------------

_SEVERITY_RANK = {"critical": 3, "warning": 2, "info": 1, "low": 0}


def _ident_variants(ident: str):
    """Yield every key format a single identifier might appear as in the
    located-drone index. Components surface as `BLE:HASH:Type` or
    `probe_MAC` or raw MAC, but the triangulator stores them as
    `FP:HASH`, `PROBE:<ie>`, `AP:<bssid>`, or the raw drone_id. Rather
    than teach every producer to emit the same key, normalise here.
    """
    ident = ident.strip()
    if not ident:
        return
    yield ident
    parts = ident.split(":")
    # BLE:HASH:Type → FP:HASH (matches BLEEnricher + triangulation's FP key)
    if len(parts) >= 2 and parts[0] == "BLE" and len(parts[1]) == 8:
        yield f"FP:{parts[1]}"
    # probe_MAC → PROBE:MAC
    if ident.startswith("probe_"):
        yield f"PROBE:{ident[6:]}"
    # Bare BSSID/MAC → AP:<bssid>
    if ident.count(":") == 5 and len(ident) == 17:
        yield f"AP:{ident}"


def _lookup_located(ident: str, located_by_id: dict) -> tuple | None:
    for key in _ident_variants(ident):
        ld = located_by_id.get(key)
        if ld and ld.lat and ld.lon:
            return (ld.lat, ld.lon, ld.accuracy_m, ld.drone_id)
    return None


def _find_located_for_entity(entity: dict, located_by_id: dict) -> tuple | None:
    """Look up a triangulated position for an entity by probing its
    component identifiers (and common key variants) against the
    located-drone index. Return (lat, lon, accuracy_m, drone_id) on hit.
    Picks the tightest accuracy if multiple components match — we trust
    the best-observed component for the entity's position.
    """
    best = None
    best_acc = float("inf")
    for comp in entity.get("components", []):
        hit = _lookup_located(comp.get("identifier") or "", located_by_id)
        if hit:
            acc = hit[2] if hit[2] is not None else float("inf")
            if acc < best_acc:
                best = hit
                best_acc = acc
    return best


def _find_located_for_alert(alert: dict, located_by_id: dict) -> tuple | None:
    """Same lookup for a flat alert dict — tries drone_id, bssid, device_id."""
    for key in ("drone_id", "bssid", "device_id", "entity_key"):
        ident = (alert.get(key) or "").strip()
        hit = _lookup_located(ident, located_by_id)
        if hit:
            return hit
    return None


@router.get("/threats")
async def get_threats(
    limit: Annotated[int, Query(ge=1, le=500, description="Max items returned")] = 200,
    include_drone_alerts: Annotated[bool, Query(description="Include confirmed drones")] = True,
    include_rf_anomalies: Annotated[bool, Query(description="Include WiFi attacks + pwnagotchi")] = True,
    min_severity: Annotated[str | None, Query(description="info | warning | critical — filter at or above")] = None,
):
    """Privacy-focused threat feed. One request gives you everything the
    dashboard needs to show "what's concerning right now" — threats are
    already filtered, classified, and positioned. Sorted by severity then
    recency so the top of the list is always the worst active threat.
    """
    now = time.time()
    min_rank = _SEVERITY_RANK.get((min_severity or "").lower(), -1)

    # Build an O(1) lookup so every threat can be cheaply joined against
    # its triangulated position without re-scanning the located list.
    located_list = _sensor_tracker.get_located_drones()
    located_by_id: dict = {d.drone_id: d for d in located_list}

    threats: list[dict] = []

    # ── 1. Threat-category entities (Meta, Flock, AirTag-as-tracker, etc.)
    entities = _entity_tracker.get_entities(active_only=True, limit=500)
    for e in entities:
        if e.get("category") != "threat":
            continue
        pos = _find_located_for_entity(e, located_by_id)
        lat, lon, acc, tid = pos if pos else (None, None, None, None)
        # Severity ladder: high-risk entity that's currently approaching the
        # user is critical; stationary/departing is warning; informational
        # if RSSI is very weak.
        trend = e.get("rssi_trend") or ""
        rssi = e.get("current_rssi") or -100
        if trend == "approaching" or rssi > -55:
            sev = "critical"
        elif rssi > -80:
            sev = "warning"
        else:
            sev = "info"
        if _SEVERITY_RANK[sev] < min_rank:
            continue
        threats.append({
            "kind": "entity",
            "subkind": (e.get("label") or "Unknown"),
            "label": e.get("label") or "Unknown threat device",
            "severity": sev,
            "first_seen": e.get("first_seen"),
            "last_seen": e.get("last_seen"),
            "age_s": round(now - (e.get("last_seen") or now), 1),
            "duration_s": e.get("duration_s"),
            "rssi_trend": trend,
            "current_rssi": rssi,
            "peak_rssi": e.get("peak_rssi"),
            "sensor_count": e.get("sensor_count", 0),
            "sensors_active": e.get("sensors_active", []),
            "dominant_sensor": e.get("dominant_sensor"),
            "mac_rotations": len(e.get("components", [])),
            "latitude": lat,
            "longitude": lon,
            "accuracy_m": acc,
            "tracking_id": tid,
            "entity_id": e.get("entity_id"),
        })

    # ── 2. Lingering-tracker + high-severity anomaly alerts
    for a in _anomaly_detector.get_alerts(limit=200):
        sev = a.get("severity") or "info"
        if _SEVERITY_RANK.get(sev, 0) < min_rank:
            continue
        # Skip pure info-level device churn unless explicitly asked for
        if sev == "info" and a.get("alert_type") not in ("lingering_tracker",):
            continue
        pos = _find_located_for_alert(a, located_by_id)
        lat, lon, acc, tid = pos if pos else (None, None, None, None)
        threats.append({
            "kind": "anomaly",
            "subkind": a.get("alert_type") or "anomaly",
            "label": a.get("message") or a.get("alert_type") or "Anomaly",
            "severity": sev,
            "first_seen": a.get("timestamp"),
            "last_seen": a.get("timestamp"),
            "age_s": a.get("age_s", round(now - (a.get("timestamp") or now), 1)),
            "duration_s": (a.get("details") or {}).get("dwell_s"),
            "rssi_trend": None,
            "current_rssi": (a.get("details") or {}).get("rssi"),
            "peak_rssi": None,
            "sensor_count": 1,
            "sensors_active": [a.get("device_id")] if a.get("device_id") else [],
            "dominant_sensor": a.get("device_id"),
            "mac_rotations": (a.get("details") or {}).get("mac_rotations", 0),
            "latitude": lat,
            "longitude": lon,
            "accuracy_m": acc,
            "tracking_id": tid,
            "drone_id": a.get("device_id"),
            "ssid": a.get("ssid"),
        })

    # ── 3. RF anomalies (WiFi attacks, pwnagotchi, karma, evil-twin)
    if include_rf_anomalies:
        for a in _rf_anomaly_detector.get_recent_alerts(limit=100):
            sev = getattr(a, "severity", "info")
            if _SEVERITY_RANK.get(sev, 0) < min_rank:
                continue
            if sev == "info":
                continue  # skip low-signal RF noise
            alert_dict = {
                "drone_id": getattr(a, "drone_id", None) or getattr(a, "entity_key", None),
                "bssid": getattr(a, "bssid", None),
                "device_id": getattr(a, "device_id", None),
                "entity_key": getattr(a, "entity_key", None),
            }
            pos = _find_located_for_alert(alert_dict, located_by_id)
            lat, lon, acc, tid = pos if pos else (None, None, None, None)
            ts = getattr(a, "detected_at", now)
            threats.append({
                "kind": "rf_anomaly",
                "subkind": getattr(a, "anomaly_type", "rf_anomaly"),
                "label": getattr(a, "title", "") or getattr(a, "message", "") or "RF anomaly",
                "severity": sev,
                "first_seen": ts,
                "last_seen": ts,
                "age_s": round(now - ts, 1),
                "duration_s": None,
                "rssi_trend": None,
                "current_rssi": None,
                "peak_rssi": None,
                "sensor_count": 1,
                "sensors_active": [getattr(a, "device_id", "")],
                "dominant_sensor": getattr(a, "device_id", ""),
                "mac_rotations": 0,
                "latitude": lat,
                "longitude": lon,
                "accuracy_m": acc,
                "tracking_id": tid,
                "ssid": getattr(a, "ssid", None),
                "manufacturer": getattr(a, "manufacturer", None),
            })

    # ── 4. Confirmed drones (from the drone-alerts ring buffer)
    if include_drone_alerts:
        for a in list(_drone_alerts)[-100:]:
            sev = a.get("severity") or "warning"
            if _SEVERITY_RANK.get(sev, 0) < min_rank:
                continue
            if sev == "info":
                continue
            ld = located_by_id.get(a.get("drone_id", ""))
            lat = a.get("latitude") if a.get("latitude") else (ld.lat if ld else None)
            lon = a.get("longitude") if a.get("longitude") else (ld.lon if ld else None)
            acc = ld.accuracy_m if ld else None
            threats.append({
                "kind": "drone",
                "subkind": a.get("classification") or "drone",
                "label": f"Drone · {(a.get('classification') or 'unknown').replace('_', ' ')}",
                "severity": sev,
                "first_seen": a.get("timestamp"),
                "last_seen": a.get("timestamp"),
                "age_s": round(now - (a.get("timestamp") or now), 1),
                "duration_s": None,
                "rssi_trend": None,
                "current_rssi": a.get("rssi"),
                "peak_rssi": None,
                "sensor_count": 1,
                "sensors_active": [a.get("device_id")] if a.get("device_id") else [],
                "dominant_sensor": a.get("device_id"),
                "mac_rotations": 0,
                "latitude": lat,
                "longitude": lon,
                "accuracy_m": acc,
                "tracking_id": a.get("drone_id"),
                "drone_id": a.get("drone_id"),
                "manufacturer": a.get("manufacturer"),
                "ssid": a.get("ssid"),
            })

    # Sort: worst severity first, newest within severity
    threats.sort(key=lambda t: (
        -_SEVERITY_RANK.get(t["severity"], 0),
        -(t.get("last_seen") or 0),
    ))
    threats = threats[:limit]

    # Roll-up counts for the header
    summary = {
        "total": len(threats),
        "by_severity": {
            "critical": sum(1 for t in threats if t["severity"] == "critical"),
            "warning":  sum(1 for t in threats if t["severity"] == "warning"),
            "info":     sum(1 for t in threats if t["severity"] == "info"),
        },
        "by_kind": {
            "entity":     sum(1 for t in threats if t["kind"] == "entity"),
            "anomaly":    sum(1 for t in threats if t["kind"] == "anomaly"),
            "rf_anomaly": sum(1 for t in threats if t["kind"] == "rf_anomaly"),
            "drone":      sum(1 for t in threats if t["kind"] == "drone"),
        },
        "triangulated": sum(1 for t in threats if t.get("latitude") is not None),
    }
    return {"threats": threats, "summary": summary, "generated_at": now}


# ---------------------------------------------------------------------------
# POST /detections/calibrate — run inter-node RSSI calibration
# ---------------------------------------------------------------------------

@router.post("/calibrate")
async def start_calibration():
    _legacy_calibration_removed()


# ---------------------------------------------------------------------------
# Phone-driven walk calibration (v0.63)
#
# Loop: phone POSTs /calibrate/walk/start, gets back a session_id + the
# BLE service UUID it should advertise. Phone walks the property, POSTing
# /calibrate/walk/sample at ~1 Hz with current GPS. Sensors hear the
# advertisement through the normal /detections/drones path; ingest hooks
# notice the calibration UUID and forward the (sensor_id, rssi, ts) tuple
# to the active session. Phone POSTs /calibrate/walk/end → backend runs
# per-listener OLS fit and applies to the triangulation engine.
#
# All endpoints require X-Cal-Token (env FOF_CAL_TOKEN). Auth floor is
# "stop neighbors from polluting our calibration data" — a static bearer
# is sufficient.
# ---------------------------------------------------------------------------

@router.post("/calibrate/walk/start")
async def calibrate_walk_start(
    body: dict,
    db: AsyncSession = Depends(get_db),
    x_cal_token: str | None = Header(None),
):
    """Begin a phone-driven calibration walk session.

    Body: {"operator_label": "Bill's Pixel", "tx_power_dbm": -59 (optional)}
    Returns: session_id + the BLE service UUID the phone must advertise.
    """
    _require_cal_token(x_cal_token)
    label = (body.get("operator_label") or "phone").strip()[:64]
    txp = body.get("tx_power_dbm")
    target_nodes = await _live_walk_sensor_rows(db)
    if not target_nodes:
        raise HTTPException(status_code=409, detail="no live geometry-enabled sensors available")
    try:
        s = await _calibration_mode.start_session(
            operator_label=label,
            tx_power_dbm=txp,
            target_nodes=target_nodes,
        )
    except Exception as exc:
        raise HTTPException(status_code=503, detail=f"failed to arm fleet calibration mode: {exc}") from exc
    return {
        "session_id": s.session_id,
        "advertise_uuid": s.expected_uuid,
        "tx_power_dbm": s.tx_power_dbm,
        "started_at": s.started_at,
        "target_nodes": getattr(s, "target_nodes", []),
        "target_sensor_count": len(getattr(s, "target_sensor_ids", []) or []),
        "mode_state": getattr(s, "mode_state", "inactive"),
        "advice": (
            "Walk a perimeter + an X across the property. Phone should report "
            "GPS + ts at ~1 Hz. Aim for 5+ minutes of motion to give each "
            "sensor enough samples to fit its own path-loss model."
        ),
    }


@router.post("/calibrate/walk/sample")
async def calibrate_walk_sample(
    body: dict,
    x_cal_token: str | None = Header(None),
):
    """Add one phone GPS trace point. Body: {session_id, lat, lon, ts_ms?, accuracy_m?}"""
    _require_cal_token(x_cal_token)
    sid = body.get("session_id")
    lat = body.get("lat")
    lon = body.get("lon")
    if not sid or lat is None or lon is None:
        raise HTTPException(status_code=400, detail="session_id, lat, lon required")
    ts_ms = body.get("ts_ms")
    ts_s = (ts_ms / 1000.0) if ts_ms else None
    _calibration_mode.renew_lease(str(sid))
    ok = _phone_cal_mgr.add_trace_point(
        session_id=sid,
        lat=float(lat), lon=float(lon),
        ts_s=ts_s,
        accuracy_m=body.get("accuracy_m"),
    )
    if not ok:
        raise HTTPException(status_code=404, detail="unknown or ended session")
    return {"ok": True}


@router.get("/calibrate/walk/feedback")
async def calibrate_walk_feedback(
    session_id: Annotated[str, Query(description="Session ID from /walk/start")],
    window_s: Annotated[float, Query(ge=1.0, le=60.0)] = 10.0,
    x_cal_token: str | None = Header(None),
    db: AsyncSession = Depends(get_db),
):
    """Live "what the fleet is hearing" snapshot — drives the phone UI's
    real-time list of sensors + RSSI per sensor. Auth-protected because
    it leaks active session state."""
    _require_cal_token(x_cal_token)
    session = _phone_cal_mgr.get(session_id)
    if session is None:
        raise HTTPException(status_code=404, detail="unknown session")
    _calibration_mode.renew_lease(session_id)
    eligible_sensor_ids = list(getattr(session, "target_sensor_ids", []) or [])
    return _phone_cal_mgr.feedback(
        session_id,
        window_s=window_s,
        eligible_sensor_ids=eligible_sensor_ids,
    ) | {
        "target_sensor_count": len(eligible_sensor_ids),
        "target_sensor_ids": eligible_sensor_ids,
        "fleet_mode_state": _calibration_mode.lease_state(session_id),
    }


# Smart stand-still detector state. Keyed per session_id; values track:
#   last_error_m : the haversine error we last computed
#   still_since  : epoch seconds when the phone's GPS went stationary (None
#                  when the phone is moving). Resets on movement > 3 m.
#   last_gps     : (lat, lon) of the last trace point we checked movement
#                  against.
_my_pos_state: dict[str, dict] = {}
_STILL_MOVEMENT_THRESHOLD_M = 3.0     # GPS jitter — below is "standing"
_STILL_DWELL_S = 5.0                   # stand this long before "OK to move"
_CONVERGENCE_ERROR_OK_M = 10.0         # triangulated within 10m of GPS
_CONVERGENCE_MIN_SENSORS = 3           # need 3+ contributing sensors


@router.get("/calibrate/walk/my-position")
async def calibrate_walk_my_position(
    session_id: Annotated[str, Query(description="Walk session ID")],
    x_cal_token: str | None = Header(None),
    db: AsyncSession = Depends(get_db),
):
    """Real-time "where I think I am vs where the fleet thinks I am" for
    the Calibrate screen's convergence card.

    Returns:
      - phone_lat / phone_lon       : the phone's last GPS fix
      - triangulated_lat / _lon / _acc : where the fleet locates the phone's
        BLE beacon via standard EKF + trilateration (keyed by session_id
        so the triangulator accumulates observations cleanly across the
        phone's own RPA rotations)
      - error_m                     : haversine between the two
      - sensor_count                : sensors contributing to the current fix
      - standing_still              : phone hasn't moved > 3 m in recent trace
      - still_s                     : how long the phone has been stationary
      - ok_to_move                  : error_m < 10 AND sensors >= 3 AND
                                      still_s >= 5 — the "move on" signal
    """
    _require_cal_token(x_cal_token)
    s = _phone_cal_mgr.get(session_id)
    if s is None:
        raise HTTPException(status_code=404, detail="unknown session")
    if s.ended_at is not None:
        return {"error": "session_ended"}
    _calibration_mode.renew_lease(session_id)

    import math as _math

    def _haversine(lat1, lon1, lat2, lon2):
        R = 6_371_000.0
        p1, p2 = _math.radians(lat1), _math.radians(lat2)
        dp = _math.radians(lat2 - lat1)
        dl = _math.radians(lon2 - lon1)
        a = _math.sin(dp/2)**2 + _math.cos(p1)*_math.cos(p2)*_math.sin(dl/2)**2
        return 2 * R * _math.asin(_math.sqrt(a))

    now = time.time()
    phone = (s.trace[-1].lat, s.trace[-1].lon) if s.trace else None
    gps_acc = s.trace[-1].accuracy_m if s.trace else None
    eligible_sensor_ids = list(getattr(s, "target_sensor_ids", []) or [])
    feedback = _phone_cal_mgr.feedback(
        session_id,
        window_s=10.0,
        eligible_sensor_ids=eligible_sensor_ids,
    )
    eligible_sensor_count = int(feedback.get("eligible_sensor_count", len(eligible_sensor_ids)))
    heard_sensor_ids = list(feedback.get("heard_sensor_ids", []))
    heard_sensor_count = int(feedback.get("heard_sensor_count", len(heard_sensor_ids)))

    # Locate the phone via the triangulator using the session's tracking_id
    tracking_id = f"FP:CAL-{session_id}"
    tri = None
    for d in _sensor_tracker.get_located_drones():
        if d.drone_id == tracking_id and d.lat and d.lon:
            tri = d
            break

    error_m = None
    if phone and tri:
        error_m = _haversine(phone[0], phone[1], tri.lat, tri.lon)

    # Stand-still tracker — phone moves > threshold → reset still timer.
    # Uses wall-clock "now" so GPS loss doesn't freeze the still_s counter
    # at an old trace timestamp.
    st = _my_pos_state.setdefault(session_id, {
        "still_since": None, "last_gps": None,
    })
    if phone:
        last = st["last_gps"]
        if last is None:
            st["last_gps"] = phone
            st["still_since"] = now
        else:
            moved = _haversine(last[0], last[1], phone[0], phone[1])
            if moved > _STILL_MOVEMENT_THRESHOLD_M:
                st["last_gps"] = phone
                st["still_since"] = now
            # else: still at same spot, don't update anything
    still_since = st.get("still_since")
    still_s = (now - still_since) if still_since else 0.0
    standing_still = still_s >= 1.0   # need 1s of stability to call it still

    sensor_count = tri.sensor_count if tri else 0
    ok_to_move = bool(
        error_m is not None and error_m < _CONVERGENCE_ERROR_OK_M
        and sensor_count >= _CONVERGENCE_MIN_SENSORS
        and still_s >= _STILL_DWELL_S
    )

    # Status hint for the UI — tells the operator what's holding back
    # "OK to move" so they know whether to wait or reposition.
    if not phone:
        status = "waiting_for_phone_gps"
    elif eligible_sensor_count == 0:
        status = "no_eligible_sensors_configured"
    elif heard_sensor_count == 0:
        status = "no_sensors_hearing_you_yet"
    elif heard_sensor_count < _CONVERGENCE_MIN_SENSORS:
        status = f"only_{heard_sensor_count}_sensor_need_{_CONVERGENCE_MIN_SENSORS}"
    elif not standing_still:
        status = "moving_keep_walking_or_stand_still"
    elif error_m is not None and error_m >= _CONVERGENCE_ERROR_OK_M:
        status = f"converging_error_{error_m:.0f}m_target_{_CONVERGENCE_ERROR_OK_M:.0f}m"
    elif still_s < _STILL_DWELL_S:
        status = f"hold_still_{still_s:.1f}s_of_{_STILL_DWELL_S:.0f}s"
    elif ok_to_move:
        status = "ok_to_move"
    else:
        status = "holding"

    return {
        "session_id": session_id,
        "phone_lat": phone[0] if phone else None,
        "phone_lon": phone[1] if phone else None,
        "phone_accuracy_m": gps_acc,
        "triangulated_lat": tri.lat if tri else None,
        "triangulated_lon": tri.lon if tri else None,
        "triangulated_accuracy_m": tri.accuracy_m if tri else None,
        "error_m": round(error_m, 1) if error_m is not None else None,
        "sensor_count": sensor_count,
        "eligible_sensor_count": eligible_sensor_count,
        "eligible_sensor_ids": eligible_sensor_ids,
        "target_sensor_count": len(eligible_sensor_ids),
        "target_sensor_ids": eligible_sensor_ids,
        "heard_sensor_count": heard_sensor_count,
        "heard_sensor_ids": heard_sensor_ids,
        "fleet_mode_state": _calibration_mode.lease_state(session_id),
        "standing_still": standing_still,
        "still_s": round(still_s, 1),
        "ok_to_move": ok_to_move,
        "status": status,
        "convergence_target_m": _CONVERGENCE_ERROR_OK_M,
        "dwell_target_s": _STILL_DWELL_S,
        "min_sensors": _CONVERGENCE_MIN_SENSORS,
    }


@router.get("/calibrate/walk/sensors")
async def calibrate_walk_sensors(
    db: AsyncSession = Depends(get_db),
    x_cal_token: str | None = Header(None),
):
    """Sensor list for the phone calibration UI — id, label, registered
    GPS, online flag. Drives the "tap which sensor you're at" cards.

    Combines the SensorNode DB row (registered/fixed positions) with the
    in-memory _node_heartbeats so newly-deployed sensors that haven't
    been registered yet still appear (using the GPS they self-reported).
    """
    _require_cal_token(x_cal_token)
    sensors = await _live_walk_sensor_rows(db)
    return {"sensors": sensors, "count": len(sensors)}


@router.post("/calibrate/walk/checkpoint")
async def calibrate_walk_checkpoint(
    body: dict,
    db: AsyncSession = Depends(get_db),
    x_cal_token: str | None = Header(None),
):
    """The operator is standing next to a sensor — anchor the OLS fit
    and surface mislabeled / misplaced sensors immediately.

    Body: {session_id, sensor_id, lat, lon, accuracy_m?, ts_ms?}
    Returns the per-checkpoint sanity result so the phone can render
    a green/yellow/red badge against that sensor right away.
    """
    _require_cal_token(x_cal_token)
    sid = body.get("session_id")
    sensor_id = body.get("sensor_id")
    lat = body.get("lat")
    lon = body.get("lon")
    if not sid or not sensor_id or lat is None or lon is None:
        raise HTTPException(status_code=400,
                            detail="session_id, sensor_id, lat, lon required")

    # Resolve the sensor's claimed position. DB row wins (operator-curated
    # ground truth); fall back to heartbeat for unregistered nodes.
    sensor_lat: float | None = None
    sensor_lon: float | None = None
    try:
        result = await db.execute(
            select(SensorNode).where(SensorNode.device_id == sensor_id)
        )
        node = result.scalar_one_or_none()
        if node and not _geometry_enabled_for_node(node):
            raise HTTPException(
                status_code=409,
                detail=f"sensor {sensor_id} is excluded from geometry for canary/testing",
            )
        if node and (node.lat or node.lon):
            sensor_lat, sensor_lon = float(node.lat), float(node.lon)
    except HTTPException:
        raise
    except Exception:
        pass
    if sensor_lat is None or sensor_lon is None:
        hb = _node_heartbeats.get(sensor_id)
        if hb and (hb.get("lat") or hb.get("lon")):
            sensor_lat = float(hb["lat"])
            sensor_lon = float(hb["lon"])
    if sensor_lat is None or sensor_lon is None:
        raise HTTPException(
            status_code=404,
            detail=f"sensor {sensor_id} has no registered position — register it via /nodes first",
        )

    session = _phone_cal_mgr.get(str(sid))
    if session is None:
        raise HTTPException(status_code=404, detail="unknown session")
    target_sensor_ids = set(getattr(session, "target_sensor_ids", []) or [])
    if target_sensor_ids and sensor_id not in target_sensor_ids:
        raise HTTPException(status_code=409, detail=f"sensor {sensor_id} is not in the active calibration target set")
    _calibration_mode.renew_lease(str(sid))

    ts_ms = body.get("ts_ms")
    ts_s = (ts_ms / 1000.0) if ts_ms else None
    return _phone_cal_mgr.add_checkpoint(
        session_id=sid,
        sensor_id=sensor_id,
        sensor_lat=sensor_lat, sensor_lon=sensor_lon,
        phone_lat=float(lat), phone_lon=float(lon),
        phone_accuracy_m=body.get("accuracy_m"),
        ts_s=ts_s,
    )


@router.post("/calibrate/walk/end")
async def calibrate_walk_end(
    body: dict,
    x_cal_token: str | None = Header(None),
):
    """Close a session, run per-listener OLS, apply if global R² > 0.4.

    Returns the fit result for display. Application is non-destructive:
    we keep the existing inter-node calibration's per-listener offsets,
    overwrite/extend the per-listener model dict, and update RSSI_REF +
    PATH_LOSS_EXPONENT only if the new global fit beats the threshold.
    """
    _require_cal_token(x_cal_token)
    sid = body.get("session_id")
    if not sid:
        raise HTTPException(status_code=400, detail="session_id required")
    session = _phone_cal_mgr.get(str(sid))
    if session is None:
        raise HTTPException(status_code=404, detail="unknown session")
    result = await _calibration_mode.end_session(
        str(sid),
        provisional_fit=body.get("provisional_fit"),
        apply_requested=bool(body.get("apply_requested", True)),
    )
    return {"session_id": sid, **result}


@router.post("/calibrate/walk/abort")
async def calibrate_walk_abort(
    body: dict,
    x_cal_token: str | None = Header(None),
):
    _require_cal_token(x_cal_token)
    sid = body.get("session_id")
    if not sid:
        raise HTTPException(status_code=400, detail="session_id required")
    session = await _calibration_mode.abort_session(str(sid), reason=str(body.get("reason") or "client_abort"))
    if session is None:
        raise HTTPException(status_code=404, detail="unknown session")
    return {
        "session_id": sid,
        "ok": True,
        "mode_state": getattr(session, "mode_state", "inactive"),
        "abort_reason": getattr(session, "abort_reason", None),
    }


@router.get("/calibrate/walk/{session_id}")
async def calibrate_walk_status(
    session_id: str,
    x_cal_token: str | None = Header(None),
):
    """Read-only inspect of an existing session — trace length, sample
    count, fit result if ended."""
    _require_cal_token(x_cal_token)
    s = _phone_cal_mgr.get(session_id)
    if s is None:
        raise HTTPException(status_code=404, detail="unknown session")
    return {
        "session_id": s.session_id,
        "operator_label": s.operator_label,
        "advertise_uuid": s.expected_uuid,
        "tx_power_dbm": s.tx_power_dbm,
        "started_at": s.started_at,
        "ended_at": s.ended_at,
        "trace_points": len(s.trace),
        "samples_total": len(s.samples),
        "fit": s.fit_result,
        "target_nodes": getattr(s, "target_nodes", []),
        "mode_state": getattr(s, "mode_state", "inactive"),
    }


# ---------------------------------------------------------------------------
# GET /detections/calibrate/audit — passive node-placement sanity check
#
# Without requiring a phone walk, audit whether the fleet's sensors are
# physically where their DB rows claim. Works by cross-referencing
# triangulated device positions against RSSI observations:
#
#   For every device that 2+ sensors heard, the sensor geometrically
#   closer to the (triangulated) device position SHOULD hear it stronger.
#   If sensor A claims to be closer than B but hears the device weaker
#   most of the time, either (a) A and B's identities are swapped, (b)
#   one of them is misplaced from its registered GPS, or (c) the antenna
#   is wildly off.
#
# Output ranks sensors by suspicion — match_rate below 40% on 20+
# samples is a strong smoke signal. Not as rigorous as a phone walk
# (which provides ground truth via touching each sensor), but it tells
# the operator WHICH pair to focus on first.
# ---------------------------------------------------------------------------

@router.get("/calibrate/audit")
async def calibrate_audit(
    min_shared_samples: Annotated[int, Query(ge=5, le=1000,
        description="Minimum shared observations per sensor pair")] = 20,
    x_cal_token: str | None = Header(None),
    db: AsyncSession = Depends(get_db),
):
    _legacy_calibration_removed()

    import math as _math

    # 1) Load registered sensor positions
    sensors: dict[str, dict] = {}
    try:
        result = await db.execute(select(SensorNode))
        for n in result.scalars().all():
            if n.lat and n.lon:
                sensors[n.device_id] = {
                    "device_id": n.device_id,
                    "name": n.name or n.device_id,
                    "lat": float(n.lat),
                    "lon": float(n.lon),
                }
    except Exception as e:
        logger.warning("audit: DB read failed: %s", e)
    for hb in _node_heartbeats.values():
        did = hb.get("device_id")
        if not did or did in sensors:
            continue
        if hb.get("lat") and hb.get("lon"):
            sensors[did] = {
                "device_id": did,
                "name": did,
                "lat": float(hb["lat"]),
                "lon": float(hb["lon"]),
            }
    if len(sensors) < 2:
        return {"error": "need at least 2 registered sensors to audit"}

    def haversine(lat1, lon1, lat2, lon2):
        R = 6_371_000.0
        p1, p2 = _math.radians(lat1), _math.radians(lat2)
        dp = _math.radians(lat2 - lat1)
        dl = _math.radians(lon2 - lon1)
        a = _math.sin(dp / 2) ** 2 + _math.cos(p1) * _math.cos(p2) * _math.sin(dl / 2) ** 2
        return 2 * R * _math.asin(_math.sqrt(a))

    # 2) Use the triangulator's per-drone observations directly — each
    #    LocatedDrone already carries {device_id → RSSI} keyed the right
    #    way (post-fingerprint-grouping). Saves us reimplementing the
    #    tracking_id normalization and gives us fresh RSSIs that already
    #    passed the smoothing + per-listener calibration pipeline.
    pair_stats: dict[tuple[str, str], dict] = {}
    total_drones_considered = 0
    for ld in _sensor_tracker.get_located_drones():
        if not (ld.lat and ld.lon):
            continue
        # Sanity: skip positions in the middle of the ocean
        if abs(ld.lat) < 0.5 and abs(ld.lon) < 0.5:
            continue
        if not ld.observations:
            continue
        total_drones_considered += 1
        # One observation per sensor (LocatedDrone.observations is
        # already deduped to latest per sensor inside the triangulator).
        per_sensor: dict[str, float] = {}
        for o in ld.observations:
            sid = o.device_id
            if sid not in sensors or o.rssi is None:
                continue
            per_sensor[sid] = float(o.rssi)

        sids = sorted(per_sensor.keys())
        for i, a in enumerate(sids):
            for b in sids[i + 1:]:
                rssi_a, rssi_b = per_sensor[a], per_sensor[b]
                da = haversine(sensors[a]["lat"], sensors[a]["lon"], ld.lat, ld.lon)
                dbb = haversine(sensors[b]["lat"], sensors[b]["lon"], ld.lat, ld.lon)
                # Skip near-equal-distance cases — inside our RSSI noise
                # floor, geometry gives no reliable expectation.
                if abs(da - dbb) < 5.0:
                    continue
                geom_a_closer = da < dbb
                rssi_a_stronger = rssi_a > rssi_b
                key = (a, b)
                s = pair_stats.setdefault(key, {
                    "samples": 0, "matches": 0,
                    "avg_rssi_delta": 0.0, "avg_dist_delta": 0.0,
                })
                s["samples"] += 1
                if geom_a_closer == rssi_a_stronger:
                    s["matches"] += 1
                n = s["samples"]
                s["avg_rssi_delta"] += ((rssi_a - rssi_b) - s["avg_rssi_delta"]) / n
                s["avg_dist_delta"] += ((da - dbb) - s["avg_dist_delta"]) / n

    # 4) Summarize per pair + per sensor
    pair_rows = []
    sensor_scores: dict[str, dict] = {}
    for (a, b), s in pair_stats.items():
        if s["samples"] < min_shared_samples:
            continue
        match_rate = s["matches"] / s["samples"]
        verdict = (
            "possible_swap_or_misplaced" if match_rate < 0.4
            else "weak_signal" if match_rate < 0.6
            else "consistent"
        )
        pair_rows.append({
            "sensor_a": a,
            "name_a": sensors[a]["name"],
            "sensor_b": b,
            "name_b": sensors[b]["name"],
            "samples": s["samples"],
            "match_rate": round(match_rate, 3),
            "avg_rssi_delta_db": round(s["avg_rssi_delta"], 1),
            "avg_dist_delta_m": round(s["avg_dist_delta"], 1),
            "verdict": verdict,
        })
        # Aggregate per-sensor: count pairs where this sensor is on the
        # wrong side of geometry. Higher = more suspect.
        for who in (a, b):
            agg = sensor_scores.setdefault(who, {
                "device_id": who,
                "name": sensors[who]["name"],
                "pair_count": 0,
                "total_samples": 0,
                "weighted_mismatch": 0.0,
            })
            agg["pair_count"] += 1
            agg["total_samples"] += s["samples"]
            agg["weighted_mismatch"] += (1 - match_rate) * s["samples"]

    for sid, agg in sensor_scores.items():
        agg["suspicion_score"] = round(
            agg["weighted_mismatch"] / max(agg["total_samples"], 1), 3)

    pair_rows.sort(key=lambda r: r["match_rate"])
    sensors_ranked = sorted(sensor_scores.values(),
                            key=lambda r: r["suspicion_score"], reverse=True)

    return {
        "generated_at": time.time(),
        "sensor_count": len(sensors),
        "drones_considered": total_drones_considered,
        "pair_count": len(pair_rows),
        "pairs": pair_rows,
        "sensors_ranked_by_suspicion": sensors_ranked,
        "interpretation": {
            "match_rate<0.4": "strong signal that these two sensors are swapped or misplaced",
            "match_rate<0.6": "weak signal — could be multipath; check with a walk",
            "match_rate>0.7": "geometry consistent with RSSI",
            "note": "Triangulated device positions are only as accurate as current calibration. "
                    "Run a phone walk to confirm any flagged pair.",
        },
    }


@router.get("/wardrive")
async def wardrive_export(
    format: Annotated[str, Query(description="csv | kml | wigle")] = "csv",
    hours: Annotated[int, Query(description="Hours of history (max 168)")] = 24,
    db: AsyncSession = Depends(get_db),
):
    """Wardriving export — every WiFi AP this fleet has seen, with GPS.

    csv: timestamp,bssid,ssid,encryption,rssi,channel,lat,lon,manufacturer
    kml: Google Earth placemarks (one per unique BSSID, strongest sample)
    wigle: WiGLE.net upload format

    Range capped at 7 days. Empty SSID rows are emitted (hidden APs)."""
    from fastapi.responses import PlainTextResponse
    hours = max(1, min(int(hours), 168))
    cutoff = datetime.now(timezone.utc) - timedelta(hours=hours)
    result = await db.execute(
        select(DroneDetection).where(
            DroneDetection.received_at >= cutoff,
            DroneDetection.source.in_(("wifi_ssid", "wifi_oui", "wifi_beacon_rid", "wifi_dji_ie")),
            DroneDetection.bssid.isnot(None),
        ).order_by(DroneDetection.received_at)
    )
    rows = result.scalars().all()

    # Dedupe to strongest sample per BSSID for KML
    if format == "kml":
        best: dict[str, "DroneDetection"] = {}
        for r in rows:
            cur = best.get(r.bssid)
            if cur is None or (r.rssi or -200) > (cur.rssi or -200):
                best[r.bssid] = r
        body = ['<?xml version="1.0" encoding="UTF-8"?>',
                '<kml xmlns="http://www.opengis.net/kml/2.2"><Document>',
                '<name>FoF Wardrive Export</name>']
        for r in best.values():
            if not r.sensor_lat or not r.sensor_lon: continue
            ssid = (r.ssid or "(hidden)").replace("&","&amp;").replace("<","&lt;")
            mfr = (r.manufacturer or "Unknown").replace("&","&amp;").replace("<","&lt;")
            body.append(
                f'<Placemark><name>{ssid}</name>'
                f'<description>BSSID:{r.bssid} RSSI:{r.rssi}dBm mfr:{mfr}</description>'
                f'<Point><coordinates>{r.sensor_lon},{r.sensor_lat},0</coordinates></Point>'
                f'</Placemark>'
            )
        body.append('</Document></kml>')
        return PlainTextResponse("\n".join(body), media_type="application/vnd.google-earth.kml+xml")

    # WiGLE format: WigleWifi-1.4 header + CSV rows
    if format == "wigle":
        from io import StringIO
        out = StringIO()
        out.write("WigleWifi-1.4,appRelease=FoF-0.60,model=ESP32-S3,release=,device=,display=,board=,brand=\n")
        out.write("MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type\n")
        for r in rows:
            if not r.sensor_lat or not r.sensor_lon: continue
            ts = r.timestamp or 0
            tstr = datetime.fromtimestamp(ts, tz=timezone.utc).strftime("%Y-%m-%d %H:%M:%S") if ts > 1700000000 else ""
            ssid = (r.ssid or "").replace(",", "")
            out.write(f"{r.bssid},{ssid},[ESS],{tstr},{0},{r.rssi or -100},{r.sensor_lat:.6f},{r.sensor_lon:.6f},0,5,WIFI\n")
        return PlainTextResponse(out.getvalue(), media_type="text/csv")

    # Default CSV
    from io import StringIO
    out = StringIO()
    out.write("timestamp,bssid,ssid,rssi,channel,sensor_lat,sensor_lon,manufacturer,source\n")
    for r in rows:
        ssid = (r.ssid or "").replace(",", "")
        mfr = (r.manufacturer or "").replace(",", "")
        out.write(f"{r.timestamp or 0},{r.bssid},{ssid},{r.rssi or 0},{0},{r.sensor_lat or 0:.6f},{r.sensor_lon or 0:.6f},{mfr},{r.source}\n")
    return PlainTextResponse(out.getvalue(), media_type="text/csv")


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
    _legacy_calibration_removed()


@router.get("/calibrate/model")
async def calibration_model():
    return _applied_cal_store.summary()


@router.get("/calibrate/history")
async def calibration_history():
    _legacy_calibration_removed()


@router.get("/calibrate/matrix")
async def calibration_matrix():
    _legacy_calibration_removed()


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
      - current range inputs: scanner distance vs backend-derived distance

    Pure observability — does not affect behavior.
    """
    from app.services.position_filter import EKF_HEALTH
    from app.services.triangulation import (
        PATH_LOSS_INDOOR,
        PATH_LOSS_OUTDOOR,
        PER_LISTENER_MODEL,
        PER_LISTENER_OFFSET_DB,
        RSSI_REF,
        STATIONARY_AGG_WINDOW_S,
        STATIONARY_INTERSECTION_MAX_ACCURACY_M,
    )

    now = time.time()
    history = _sensor_tracker._emit_history
    flips = _sensor_tracker._source_flip_counts
    counters = _sensor_tracker._emit_counters
    current_range_source_counts: dict[str, int] = {}
    for obs_map in _sensor_tracker.observations.values():
        for obs in obs_map.values():
            src = obs.distance_source or "none"
            current_range_source_counts[src] = current_range_source_counts.get(src, 0) + 1

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
        "current_range_source_counts": current_range_source_counts,
        "range_defaults": {
            "rssi_ref": RSSI_REF,
            "path_loss_outdoor": PATH_LOSS_OUTDOOR,
            "path_loss_indoor": PATH_LOSS_INDOOR,
            "per_listener_model_count": len(PER_LISTENER_MODEL),
            "per_listener_offset_count": len(PER_LISTENER_OFFSET_DB),
            "stationary_agg_window_s": STATIONARY_AGG_WINDOW_S,
            "stationary_intersection_max_accuracy_m": STATIONARY_INTERSECTION_MAX_ACCURACY_M,
        },
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
        current_obs = list(_sensor_tracker.observations.get(drone_id, {}).values())
        current_obs.sort(key=lambda o: o.timestamp, reverse=True)
        range_source_mix: dict[str, int] = {}
        for obs in current_obs:
            src = obs.distance_source or "none"
            range_source_mix[src] = range_source_mix.get(src, 0) + 1
        drones.append({
            "drone_id": drone_id,
            "emit_count": len(records),
            "source_mix": per_source,
            "source_flips": dict(flips.get(drone_id, {})),
            "range_source_mix": range_source_mix,
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
            "current_ranges": [
                {
                    "device_id": o.device_id,
                    "source": o.source,
                    "timestamp": round(o.timestamp, 3),
                    "age_s": round(now - o.timestamp, 1),
                    "rssi": o.rssi,
                    "used_distance_m": o.estimated_distance_m,
                    "scanner_distance_m": o.scanner_estimated_distance_m,
                    "backend_distance_m": o.backend_estimated_distance_m,
                    "distance_source": o.distance_source,
                    "range_model": o.range_model,
                }
                for o in current_obs
            ],
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
    max_age_s: Annotated[int, Query(ge=1, le=172800, description="Max age in seconds")] = 120,
    db: AsyncSession = Depends(get_db),
):
    """Return WiFi probe request devices grouped by stable identity."""
    now = time.time()

    probe_items = [
        d for d in _recent_detections
        if d.source == "wifi_probe_request" and (now - d.received_at) <= max_age_s
    ]
    probe_items_24h = [
        d for d in _recent_detections
        if d.source == "wifi_probe_request" and (now - d.received_at) <= 86400
    ]

    groups: dict[str, dict] = {}
    for d in probe_items:
        probe = normalize_probe_identity(
            ie_hash=getattr(d, "ie_hash", None),
            drone_id=d.drone_id,
            bssid=d.bssid,
        )
        identity, ie_hash, mac = probe.identity, probe.ie_hash, probe.mac
        if identity not in groups:
            groups[identity] = {
                "identity": identity,
                "ie_hash": ie_hash,
                "mac": mac,
                "macs": set(),
                "probed_ssids": set(),
                "probe_count": 0,
                "best_rssi": -999,
                "sensors": set(),
                "last_seen": 0.0,
                "classification": "wifi_device",
            }
        g = groups[identity]
        g["probe_count"] += 1
        if mac:
            g["macs"].add(mac)
            if not g["mac"]:
                g["mac"] = mac

        if d.ssid:
            g["probed_ssids"].add(d.ssid)
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

    stats_24h: dict[str, dict] = {}
    for d in probe_items_24h:
        probe = normalize_probe_identity(
            ie_hash=getattr(d, "ie_hash", None),
            drone_id=d.drone_id,
            bssid=d.bssid,
        )
        identity = probe.identity
        bucket = stats_24h.setdefault(identity, {
            "seen_24h_count": 0,
            "sensors": set(),
        })
        bucket["seen_24h_count"] += 1
        if d.device_id:
            bucket["sensors"].add(d.device_id)

    from app.models.db_models import Event
    cutoff = datetime.now(timezone.utc) - timedelta(hours=24)
    try:
        event_rows = (
            await db.execute(
                select(Event)
                .where(Event.first_seen_at >= cutoff)
                .where(Event.event_type.in_((
                    "new_probe_identity",
                    "new_probe_mac",
                    "new_probed_ssid",
                    "probe_activity_spike",
                )))
                .order_by(Event.first_seen_at.desc())
            )
        ).scalars().all()
    except OperationalError as exc:
        if "no such table: events" not in str(exc).lower():
            raise
        event_rows = []

    identity_event_types: dict[str, set[str]] = defaultdict(set)
    identity_first_seen: dict[str, float] = {}
    ssid_event_types: dict[str, set[str]] = defaultdict(set)
    for row in event_rows:
        try:
            metadata = json.loads(row.metadata_json or "{}")
        except Exception:
            metadata = {}
        probe_identity = probe_identity_from_event(row.event_type, row.identifier, metadata)
        if probe_identity:
            identity_event_types[probe_identity].add(row.event_type)
            first_seen_ts = _datetime_to_epoch_seconds(row.first_seen_at)
            if first_seen_ts is None:
                continue
            prev_ts = identity_first_seen.get(probe_identity)
            if prev_ts is None or first_seen_ts < prev_ts:
                identity_first_seen[probe_identity] = first_seen_ts
        if row.event_type == "new_probed_ssid":
            ssid_event_types[row.identifier].add(row.event_type)

    located = _sensor_tracker.get_located_drones(include_probe_diagnostics=True)
    located_by_id = {d.drone_id: d for d in located}

    devices = []
    for identity, g in groups.items():
        cls = g["classification"]
        if drone_only and cls not in ("likely_drone", "confirmed_drone"):
            continue

        loc = located_by_id.get(identity)
        stats24 = stats_24h.get(identity, {})
        first_seen_ts = identity_first_seen.get(identity, g["last_seen"])
        latest_event_types = set(identity_event_types.get(identity, set()))
        for ssid in g["probed_ssids"]:
            latest_event_types.update(ssid_event_types.get(ssid, set()))
        seen_24h_count = int(stats24.get("seen_24h_count", g["probe_count"]))
        sensor_count_24h = len(stats24.get("sensors", g["sensors"]))
        if "probe_activity_spike" in latest_event_types or seen_24h_count >= 25 or sensor_count_24h >= 3:
            activity_level = "high"
        elif seen_24h_count >= 10 or sensor_count_24h >= 2:
            activity_level = "medium"
        else:
            activity_level = "low"

        device_entry = {
            "identity": identity,
            "ie_hash": g["ie_hash"],
            "mac": g["mac"],
            "macs": sorted(g["macs"]),
            "probed_ssids": sorted(g["probed_ssids"]),
            "probe_count": g["probe_count"],
            "best_rssi": g["best_rssi"] if g["best_rssi"] != -999 else None,
            "classification": cls,
            "sensor_count": len(g["sensors"]),
            "sensors": sorted(g["sensors"]),
            "first_seen": first_seen_ts,
            "first_seen_age_s": round(now - first_seen_ts, 1) if first_seen_ts else None,
            "last_seen": g["last_seen"],
            "age_s": round(now - g["last_seen"], 1),
            "seen_24h_count": seen_24h_count,
            "sensor_count_24h": sensor_count_24h,
            "activity_level": activity_level,
            "latest_event_types": sorted(latest_event_types),
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
    max_age_s: Annotated[int, Query(ge=1, le=3600, description="Max age in seconds")] = 300,
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

def _datetime_to_epoch_seconds(value: datetime | None) -> float | None:
    if value is None:
        return None
    if value.tzinfo is None:
        value = value.replace(tzinfo=timezone.utc)
    return value.timestamp()


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


def _normalized_event_types(type_value: str | None,
                            types_value: list[str] | None) -> list[str] | None:
    values: list[str] = []
    if type_value:
        values.append(type_value)
    values.extend(types_value or [])
    normalized: list[str] = []
    seen: set[str] = set()
    for raw in values:
        for part in (raw or "").split(","):
            cleaned = part.strip()
            if cleaned and cleaned not in seen:
                normalized.append(cleaned)
                seen.add(cleaned)
    return normalized or None


@router.get("/events")
async def list_events(
    type: Annotated[str | None, Query(description="Filter by event_type")] = None,
    types: Annotated[list[str] | None, Query(description="Repeated or comma-separated event types")] = None,
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
    event_types = _normalized_event_types(type, types)
    if event_types:
        q = q.where(Event.event_type.in_(event_types))
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
    unack_by_type: dict[str, int] = {}
    rows = (await db.execute(
        select(Event.event_type, func.count(Event.id))
        .group_by(Event.event_type)
    )).all()
    for et, n in rows:
        by_type[et] = n
    rows = (await db.execute(
        select(Event.event_type, func.count(Event.id))
        .where(Event.acknowledged == False)  # noqa: E712
        .group_by(Event.event_type)
    )).all()
    for et, n in rows:
        unack_by_type[et] = n
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
        "unack_by_type": unack_by_type,
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
