"""Sensor node management endpoints — register, update, list, delete, OTA."""

import asyncio
import json
import logging
import subprocess
import time
import zlib
from datetime import datetime, timezone
from urllib.parse import quote

import httpx
from fastapi import APIRouter, Depends, HTTPException, Request, UploadFile, File
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.models.db_models import DroneDetection, SensorNode
from app.models.schemas import (
    DetectionHistoryItem,
    DetectionHistoryResponse,
    NodeCreateRequest,
    NodeListResponse,
    NodeResponse,
    NodeUpdateRequest,
)
from app.services.database import get_db

from app.services.firmware_manager import FirmwareManager

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/nodes", tags=["nodes"])

_firmware_mgr = FirmwareManager()

_firmware_rollouts: dict[str, dict] = {}
_firmware_rollout_log: list[str] = []
_firmware_rollout_tasks: dict[str, asyncio.Task] = {}


async def _run_subprocess(*args, **kwargs):
    """Run blocking subprocess work off the FastAPI event loop."""
    return await asyncio.to_thread(subprocess.run, *args, **kwargs)


def _normalized_firmware_version(version: str | None) -> str:
    if not version:
        return ""
    value = str(version).strip()
    return value[1:] if value[:1].lower() == "v" else value


def _scanner_version_from_node_info(node_info: dict | None, uart: str) -> tuple[str, dict | None]:
    if not node_info:
        return "", None
    for scanner in node_info.get("scanners") or []:
        if scanner.get("uart") != uart:
            continue
        version = scanner.get("ver") or scanner.get("firmware_version") or scanner.get("version") or ""
        return str(version), scanner
    return "", None


async def _wait_for_scanner_version(
    device_id: str,
    uart: str,
    target_version: str,
    *,
    timeout_s: float = 90.0,
    interval_s: float = 3.0,
) -> tuple[bool, str, dict | None]:
    from app.routers.detections import _node_heartbeats

    target = _normalized_firmware_version(target_version)
    last_version = ""
    last_scanner = None
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        last_version, last_scanner = _scanner_version_from_node_info(
            _node_heartbeats.get(device_id),
            uart,
        )
        if target and _normalized_firmware_version(last_version) == target:
            return True, last_version, last_scanner
        await asyncio.sleep(interval_s)
    return False, last_version, last_scanner


async def _run_direct_legacy_scanner_relay(ip: str, uart: str, fw_data: bytes) -> dict:
    """Use the older HTTP-streaming UART relay.

    That endpoint can only prove that bytes were streamed to the scanner. The
    caller must verify the scanner heartbeat before treating this as success.
    """
    started = time.monotonic()
    try:
        result = await _run_subprocess(
            ["curl", "-sS", "-X", "POST",
             f"http://{ip}/api/ota/relay?uart={uart}&legacy=1",
             "--data-binary", "@-",
             "-H", "Content-Type: application/octet-stream",
             "--connect-timeout", "8",
             "--max-time", "420",
             "--limit-rate", "16k"],
            input=fw_data, capture_output=True, timeout=430,
        )
    except subprocess.TimeoutExpired:
        return {
            "ok": False,
            "mode": "direct_legacy",
            "legacy": True,
            "stage": "direct_legacy",
            "error": "direct_legacy_timeout",
            "elapsed_s": int(time.monotonic() - started),
        }

    elapsed_s = int(time.monotonic() - started)
    if result.returncode != 0:
        err = result.stderr.decode(errors="replace")[:200]
        return {
            "ok": False,
            "mode": "direct_legacy",
            "legacy": True,
            "stage": "direct_legacy",
            "error": f"direct_legacy_curl:{err}",
            "elapsed_s": elapsed_s,
        }

    try:
        response = json.loads(result.stdout)
    except Exception:
        response = {"raw": result.stdout.decode(errors="replace")[:200]}

    relay_ok = bool(response.get("ok"))
    return {
        "ok": relay_ok,
        "mode": "direct_legacy",
        "legacy": True,
        "stage": "" if relay_ok else "direct_legacy",
        "error": "" if relay_ok else (
            response.get("scanner_error") or response.get("error") or "direct_legacy_failed"
        ),
        "bytes": response.get("bytes", 0),
        "chunks": response.get("chunks", 0),
        "nacks": response.get("nacks", 0),
        "retries": response.get("retries", 0),
        "elapsed_s": elapsed_s,
        "scanner_response": response.get("scanner_response", ""),
        "scanner_error": response.get("scanner_error", ""),
        "streaming_response": response,
    }


def _scanner_target_name(scanner: dict, firmware_name: str | None = None) -> str:
    if firmware_name:
        return firmware_name
    board = str(scanner.get("board") or "").strip()
    if board.startswith("scanner-"):
        return board
    return "scanner-s3-combo"


def _scanner_current_version(scanner: dict) -> str:
    return str(scanner.get("ver") or scanner.get("firmware_version") or scanner.get("version") or "")


def _scanner_self_update_capable(scanner: dict) -> bool:
    try:
        cmd_rx = int(scanner.get("cmd_rx") or scanner.get("cmd_rx_count") or 0)
    except (TypeError, ValueError):
        cmd_rx = 0
    try:
        fw_checks = int(scanner.get("fw_check_count") or 0)
    except (TypeError, ValueError):
        fw_checks = 0
    fw_state = str(scanner.get("fw_state") or scanner.get("fw_update_state") or "").strip().lower()
    return cmd_rx > 0 or fw_checks > 0 or fw_state in {
        "current", "offered", "ready", "updating", "deferred", "error"
    }


def _scanner_update_telemetry_present(scanner: dict | None) -> bool:
    if not isinstance(scanner, dict):
        return False
    return _scanner_self_update_capable(scanner)


def _scanner_is_target_version(scanner: dict, target_version: str | None) -> bool:
    target = _normalized_firmware_version(target_version)
    return bool(target) and _normalized_firmware_version(_scanner_current_version(scanner)) == target


def _heartbeat_nodes(device_id: str | None = None, *, include_offline: bool = False) -> list[tuple[str, dict]]:
    from app.routers.detections import _node_heartbeats

    now = time.time()
    rows = []
    for node_id, hb in _node_heartbeats.items():
        if device_id and node_id != device_id:
            continue
        age_s = now - float(hb.get("last_seen", now) or now)
        if not include_offline and age_s >= 120:
            continue
        node = dict(hb)
        node.setdefault("device_id", node_id)
        node["age_s"] = round(age_s, 1)
        node["online"] = age_s < 120
        rows.append((node_id, node))
    rows.sort(key=lambda item: item[0])
    return rows


async def _scanner_targets(
    *,
    device_id: str | None = None,
    firmware_name: str | None = None,
    include_offline: bool = False,
) -> list[dict]:
    targets: list[dict] = []
    version_cache: dict[str, str] = {}
    for node_id, node in _heartbeat_nodes(device_id, include_offline=include_offline):
        ip = node.get("ip")
        scanners = [sc for sc in (node.get("scanners") or []) if isinstance(sc, dict)]
        for scanner in scanners:
            target_fw = _scanner_target_name(scanner, firmware_name)
            if not target_fw.startswith("scanner-"):
                continue
            if target_fw not in version_cache:
                version_cache[target_fw] = await _firmware_mgr.get_firmware_version(target_fw) or target_fw
            targets.append({
                "device_id": node_id,
                "ip": ip,
                "uart": str(scanner.get("uart") or "ble"),
                "scanner": dict(scanner),
                "current_version": _scanner_current_version(scanner),
                "target_firmware": target_fw,
                "target_version": version_cache[target_fw],
                "self_update_capable": _scanner_self_update_capable(scanner),
                "online": bool(node.get("online")),
                "node_age_s": node.get("age_s"),
            })
    targets.sort(key=lambda t: (t["device_id"], t["uart"]))
    return targets


async def _stage_scanner_firmware_on_uplink(device_id: str, ip: str, firmware_name: str) -> dict:
    fw_data = await _firmware_mgr.get_firmware_binary(firmware_name)
    if not fw_data:
        return {
            "ok": False,
            "device_id": device_id,
            "firmware": firmware_name,
            "state": "blocked",
            "error": "firmware_not_available",
        }
    fw_version = await _firmware_mgr.get_firmware_version(firmware_name) or firmware_name
    checksum = zlib.crc32(fw_data) & 0xFFFFFFFF
    try:
        r_stage = await _run_subprocess(
            ["curl", "-s", "-X", "POST",
             f"http://{ip}/api/fw/upload?name={quote(firmware_name)}&version={quote(fw_version)}",
             "--data-binary", "@-",
             "-H", "Content-Type: application/octet-stream",
             "--connect-timeout", "5",
             "--max-time", "240"],
            input=fw_data, capture_output=True, timeout=245,
        )
    except subprocess.TimeoutExpired:
        return {
            "ok": False,
            "device_id": device_id,
            "firmware": firmware_name,
            "target_version": fw_version,
            "state": "failed",
            "error": "stage_timeout",
            "size": len(fw_data),
            "crc32": checksum,
        }

    if r_stage.returncode != 0:
        err = r_stage.stderr.decode(errors="replace")[:200]
        return {
            "ok": False,
            "device_id": device_id,
            "firmware": firmware_name,
            "target_version": fw_version,
            "state": "failed",
            "error": f"stage_curl:{err}",
            "size": len(fw_data),
            "crc32": checksum,
        }

    try:
        stage_resp = json.loads(r_stage.stdout)
    except Exception:
        stage_resp = {"raw": r_stage.stdout.decode(errors="replace")[:200]}
    ok = bool(stage_resp.get("stored") or stage_resp.get("ok"))
    return {
        "ok": ok,
        "device_id": device_id,
        "firmware": firmware_name,
        "target_version": fw_version,
        "state": "staged" if ok else "failed",
        "error": "" if ok else "stage_rejected",
        "size": len(fw_data),
        "crc32": checksum,
        "stage_response": stage_resp,
    }


async def _trigger_scanner_firmware_check(device_id: str, ip: str, uart: str = "both") -> dict:
    try:
        result = await _run_subprocess(
            ["curl", "-s", "-X", "POST",
             f"http://{ip}/api/fw/trigger?uart={quote(uart)}",
             "--connect-timeout", "4",
             "--max-time", "15"],
            capture_output=True, timeout=20,
        )
    except subprocess.TimeoutExpired:
        return {
            "ok": False,
            "device_id": device_id,
            "uart": uart,
            "state": "failed",
            "error": "trigger_timeout",
        }
    if result.returncode != 0:
        err = result.stderr.decode(errors="replace")[:200]
        return {
            "ok": False,
            "device_id": device_id,
            "uart": uart,
            "state": "failed",
            "error": f"trigger_curl:{err}",
        }
    try:
        payload = json.loads(result.stdout)
    except Exception:
        payload = {"raw": result.stdout.decode(errors="replace")[:200]}
    ok = bool(payload.get("ok"))
    return {
        "ok": ok,
        "device_id": device_id,
        "uart": uart,
        "state": "offered" if ok else "blocked",
        "error": "" if ok else (payload.get("error") or "scanner_command_ingress_unreachable"),
        "trigger_response": payload,
    }


def _remember_rollout(rollout: dict) -> None:
    rollout_id = rollout["rollout_id"]
    _firmware_rollouts[rollout_id] = rollout
    _firmware_rollout_log.append(rollout_id)
    while len(_firmware_rollout_log) > 30:
        stale = _firmware_rollout_log.pop(0)
        _firmware_rollouts.pop(stale, None)
        task = _firmware_rollout_tasks.pop(stale, None)
        if task and not task.done():
            task.cancel()


async def _run_scanner_rollout(rollout_id: str, targets: list[dict], *, stage_first: bool) -> None:
    rollout = _firmware_rollouts[rollout_id]
    rollout["status"] = "running"
    rollout["started_at"] = time.time()

    if stage_first:
        staged_keys: set[tuple[str, str]] = set()
        for target in targets:
            key = (target["device_id"], target["target_firmware"])
            if key in staged_keys:
                continue
            staged_keys.add(key)
            if not target.get("ip"):
                rollout["stage_results"].append({
                    "ok": False,
                    "device_id": target["device_id"],
                    "firmware": target["target_firmware"],
                    "state": "blocked",
                    "error": "missing_uplink_ip",
                })
                continue
            stage = await _stage_scanner_firmware_on_uplink(
                target["device_id"],
                target["ip"],
                target["target_firmware"],
            )
            rollout["stage_results"].append(stage)

    for target in targets:
        item = rollout["targets"].get(f"{target['device_id']}/{target['uart']}")
        if not item:
            continue
        item["state"] = "pending"
        if _scanner_is_target_version(target["scanner"], target["target_version"]):
            item["state"] = "verified"
            item["verified"] = True
            continue
        if not target.get("ip"):
            item["state"] = "blocked"
            item["error"] = "missing_uplink_ip"
            continue
        if not target.get("self_update_capable"):
            item["state"] = "blocked"
            item["error"] = "scanner_command_ingress_unreachable"
            item["next_action"] = "USB flash this scanner once with the recovery-capable image"
            continue

        trigger = await _trigger_scanner_firmware_check(
            target["device_id"],
            target["ip"],
            target["uart"],
        )
        item["trigger_response"] = trigger
        if not trigger.get("ok"):
            item["state"] = "blocked" if trigger.get("error") == "scanner_command_ingress_unreachable" else "failed"
            item["error"] = trigger.get("error") or "trigger_failed"
            continue

        item["state"] = "offered"
        verified, scanner_version, scanner = await _wait_for_scanner_version(
            target["device_id"],
            target["uart"],
            target["target_version"],
            timeout_s=150.0,
            interval_s=3.0,
        )
        item["verified"] = verified
        item["scanner_version_after"] = scanner_version or None
        item["scanner_after"] = scanner
        if verified and _scanner_update_telemetry_present(scanner):
            item["state"] = "verified"
        elif verified:
            item["verified"] = False
            item["state"] = "failed"
            item["error"] = "scanner_update_telemetry_missing"
        else:
            item["state"] = "failed"
            item["error"] = "scanner_version_verify_timeout"

    states = [item.get("state") for item in rollout["targets"].values()]
    if all(state == "verified" for state in states):
        rollout["status"] = "done"
    elif any(state == "blocked" for state in states):
        rollout["status"] = "blocked"
    elif any(state == "failed" for state in states):
        rollout["status"] = "failed"
    else:
        rollout["status"] = "partial"
    rollout["finished_at"] = time.time()


def _geometry_enabled(position_mode: str | None) -> bool:
    return (position_mode or "active") == "active"


def _apply_geometry_policy(device_id: str, position_mode: str | None) -> None:
    if _geometry_enabled(position_mode):
        return
    try:
        from app.routers.detections import _sensor_tracker
        _sensor_tracker.remove_sensor(device_id)
    except Exception:
        logger.debug("Failed to purge geometry state for excluded node %s", device_id, exc_info=True)


def _node_to_response(node: SensorNode) -> NodeResponse:
    return NodeResponse(
        device_id=node.device_id,
        name=node.name,
        lat=node.lat,
        lon=node.lon,
        alt=node.alt,
        is_fixed=node.is_fixed,
        sensor_type=node.sensor_type,
        position_mode=node.position_mode,
        geometry_enabled=_geometry_enabled(node.position_mode),
        last_seen=node.last_seen.isoformat() if node.last_seen else None,
        created_at=node.created_at.isoformat() if node.created_at else None,
    )


@router.post("", response_model=NodeResponse, status_code=201)
async def register_node(req: NodeCreateRequest, db: AsyncSession = Depends(get_db)):
    """
    Register a sensor node at a fixed GPS position.

    If a node with this device_id already exists, its position and name
    are updated and is_fixed is set to true.
    """
    result = await db.execute(select(SensorNode).where(SensorNode.device_id == req.device_id))
    existing = result.scalar_one_or_none()

    if existing:
        existing.name = req.name or existing.name
        existing.lat = req.lat
        existing.lon = req.lon
        existing.alt = req.alt
        existing.is_fixed = True
        existing.sensor_type = req.sensor_type
        existing.position_mode = req.position_mode
        existing.last_seen = datetime.now(timezone.utc)
        await db.commit()
        await db.refresh(existing)
        _apply_geometry_policy(existing.device_id, existing.position_mode)
        logger.info(
            "Updated fixed node: %s at (%f, %f) type=%s position_mode=%s",
            req.device_id,
            req.lat,
            req.lon,
            req.sensor_type,
            req.position_mode,
        )
        return _node_to_response(existing)

    node = SensorNode(
        device_id=req.device_id,
        name=req.name,
        lat=req.lat,
        lon=req.lon,
        alt=req.alt,
        is_fixed=True,
        sensor_type=req.sensor_type,
        position_mode=req.position_mode,
        last_seen=datetime.now(timezone.utc),
    )
    db.add(node)
    await db.commit()
    await db.refresh(node)
    _apply_geometry_policy(node.device_id, node.position_mode)
    logger.info("Registered new fixed node: %s '%s' at (%f, %f)", req.device_id, req.name, req.lat, req.lon)
    return _node_to_response(node)


@router.get("", response_model=NodeListResponse)
async def list_nodes(db: AsyncSession = Depends(get_db)):
    """List all registered sensor nodes."""
    result = await db.execute(select(SensorNode).order_by(SensorNode.device_id))
    nodes = result.scalars().all()
    return NodeListResponse(
        count=len(nodes),
        nodes=[_node_to_response(n) for n in nodes],
    )


@router.put("/{device_id}", response_model=NodeResponse)
async def update_node(device_id: str, req: NodeUpdateRequest, db: AsyncSession = Depends(get_db)):
    """Update a node's position or name."""
    result = await db.execute(select(SensorNode).where(SensorNode.device_id == device_id))
    node = result.scalar_one_or_none()
    if not node:
        raise HTTPException(status_code=404, detail=f"Node '{device_id}' not found")

    if req.name is not None:
        node.name = req.name
    if req.lat is not None:
        node.lat = req.lat
        node.is_fixed = True  # Manual position = fixed
    if req.lon is not None:
        node.lon = req.lon
        node.is_fixed = True
    if req.alt is not None:
        node.alt = req.alt
    if req.position_mode is not None:
        node.position_mode = req.position_mode
    node.last_seen = datetime.now(timezone.utc)

    await db.commit()
    await db.refresh(node)
    _apply_geometry_policy(node.device_id, node.position_mode)
    logger.info(
        "Updated node %s: name=%s pos=(%f, %f) position_mode=%s",
        device_id,
        node.name,
        node.lat,
        node.lon,
        node.position_mode,
    )
    return _node_to_response(node)


@router.delete("/{device_id}", status_code=204)
async def delete_node(device_id: str, db: AsyncSession = Depends(get_db)):
    """Delete a registered node."""
    result = await db.execute(select(SensorNode).where(SensorNode.device_id == device_id))
    node = result.scalar_one_or_none()
    if not node:
        raise HTTPException(status_code=404, detail=f"Node '{device_id}' not found")

    await db.delete(node)
    await db.commit()
    logger.info("Deleted node: %s", device_id)


@router.get("/{device_id}/detections", response_model=DetectionHistoryResponse)
async def get_node_detections(
    device_id: str,
    limit: int = 100,
    db: AsyncSession = Depends(get_db),
):
    """Get recent detections from a specific node."""
    result = await db.execute(
        select(DroneDetection)
        .where(DroneDetection.device_id == device_id)
        .order_by(DroneDetection.id.desc())
        .limit(limit)
    )
    detections = result.scalars().all()

    count_result = await db.execute(
        select(DroneDetection.id).where(DroneDetection.device_id == device_id)
    )
    total = len(count_result.all())

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
# GET /nodes/{device_id}/live — proxy to ESP32 status page (avoids CORS)
# ---------------------------------------------------------------------------

@router.get("/{device_id}/live")
async def get_node_live_status(device_id: str):
    """Proxy to an ESP32 node's /api/status endpoint. Avoids browser CORS issues."""
    from app.routers.detections import _node_heartbeats

    node_info = _node_heartbeats.get(device_id)
    if not node_info or not node_info.get("ip"):
        raise HTTPException(status_code=404, detail=f"Node '{device_id}' not found or no IP known")

    ip = node_info["ip"]
    try:
        async with httpx.AsyncClient(timeout=2.0) as client:
            r = await client.get(f"http://{ip}/api/status")
            return r.json()
    except Exception as e:
        return {"error": str(e), "device_id": device_id, "ip": ip}


# ---------------------------------------------------------------------------
# POST /nodes/{device_id}/ota — push firmware update to a node
# ---------------------------------------------------------------------------

@router.post("/{device_id}/ota")
async def push_ota_update(device_id: str, firmware: UploadFile = File(...)):
    """Push a firmware .bin file to a specific node via HTTP OTA.

    The backend reads the uploaded file and POSTs it to the node's /api/ota endpoint.
    The node writes it to the inactive OTA partition and reboots.
    """
    from app.routers.detections import _node_heartbeats

    node_info = _node_heartbeats.get(device_id)
    if not node_info or not node_info.get("ip"):
        raise HTTPException(status_code=404, detail=f"Node '{device_id}' not found or no IP")

    ip = node_info["ip"]
    fw_data = await firmware.read()
    fw_size = len(fw_data)

    if fw_size < 1024 or fw_size > 2 * 1024 * 1024:
        raise HTTPException(status_code=400, detail=f"Invalid firmware size: {fw_size} bytes")

    logger.warning("OTA push to %s (%s): %d bytes (%s)", device_id, ip, fw_size, firmware.filename)

    try:
        # Use curl — httpx chunked encoding breaks ESP32 HTTP server.
        # Throttle uplink OTA: field testing showed saturated HTTP uploads can
        # appear to reboot without actually selecting the new OTA slot.
        result = await _run_subprocess(
            ["curl", "-s", "-X", "POST",
             f"http://{ip}/api/ota",
             "--data-binary", "@-",
             "-H", "Content-Type: application/octet-stream",
             "--connect-timeout", "8",
             "--max-time", "480",
             "--limit-rate", "64k"],
            input=fw_data, capture_output=True, timeout=490
        )
        if result.returncode == 0 and result.stdout:
            try:
                resp = __import__("json").loads(result.stdout)
                logger.info("OTA to %s success: %s", device_id, resp)
                return {"ok": True, "device_id": device_id, "size": fw_size,
                        "upload_rate_limit": "64k", "response": resp}
            except Exception:
                pass
        # curl may return 0 even if node rebooted mid-response
        logger.info("OTA to %s: firmware sent, node likely rebooting", device_id)
        return {"ok": True, "device_id": device_id, "size": fw_size,
                "upload_rate_limit": "64k", "message": "Node rebooting"}
    except Exception as e:
        logger.error("OTA to %s failed: %s", device_id, e)
        raise HTTPException(status_code=502, detail=f"OTA failed: {e}")


@router.get("/{device_id}/ota/info")
async def get_node_ota_info(device_id: str):
    """Get OTA partition info from a node."""
    from app.routers.detections import _node_heartbeats

    node_info = _node_heartbeats.get(device_id)
    if not node_info or not node_info.get("ip"):
        raise HTTPException(status_code=404, detail=f"Node '{device_id}' not found or no IP")

    ip = node_info["ip"]
    try:
        async with httpx.AsyncClient(timeout=3.0) as client:
            r = await client.get(f"http://{ip}/api/ota/info")
            return r.json()
    except Exception as e:
        return {"error": str(e), "device_id": device_id}


# ---------------------------------------------------------------------------
# Firmware catalog endpoints
# ---------------------------------------------------------------------------

@router.get("/firmware/catalog")
async def get_firmware_catalog():
    """List available firmware types with versions and sources."""
    return {"firmware": await _firmware_mgr.get_catalog()}


@router.post("/firmware/upload/{name}")
async def upload_custom_firmware(name: str, firmware: UploadFile = File(...)):
    """Upload a custom firmware .bin for testing (overrides GitHub release)."""
    data = await firmware.read()
    if len(data) < 1024 or len(data) > 2 * 1024 * 1024:
        raise HTTPException(status_code=400, detail=f"Invalid size: {len(data)} bytes")
    _firmware_mgr.set_custom_firmware(name, data)
    return {"ok": True, "name": name, "size": len(data), "source": "custom"}


@router.delete("/firmware/upload/{name}")
async def clear_custom_firmware(name: str):
    """Clear a custom firmware override (revert to GitHub release)."""
    _firmware_mgr.clear_custom_firmware(name)
    return {"ok": True, "name": name, "source": "github"}


# ---------------------------------------------------------------------------
# Auto-update poll endpoints — uplinks check these to self-update and to
# refresh their cached scanner firmware. Pull-based: the device decides when
# to check (default 30 min) so the backend doesn't have to track who has what.
# ---------------------------------------------------------------------------

import hashlib
from fastapi.responses import Response
from app.services.firmware_manager import FIRMWARE_TYPES


_FW_HASH_CACHE: dict[str, tuple[str, int, str]] = {}
"""name → (version, size, sha256_hex) — recomputed when version changes."""


async def _firmware_metadata(name: str) -> dict | None:
    """Common helper: build metadata for /latest/{name}. Returns None if missing."""
    if name not in FIRMWARE_TYPES:
        return None
    data = await _firmware_mgr.get_firmware_binary(name)
    if not data:
        return None
    version = await _firmware_mgr.get_firmware_version(name) or "unknown"

    cached = _FW_HASH_CACHE.get(name)
    if not cached or cached[0] != version or cached[1] != len(data):
        sha = hashlib.sha256(data).hexdigest()
        _FW_HASH_CACHE[name] = (version, len(data), sha)
    else:
        sha = cached[2]

    catalog_info = FIRMWARE_TYPES[name]
    return {
        "name": name,
        "description": catalog_info["description"],
        "board": catalog_info["board"],
        "version": version,
        "size": len(data),
        "sha256": sha,
        "download_url": f"/nodes/firmware/download/{name}",
    }


@router.get("/firmware/latest/{name}")
async def get_firmware_latest(name: str):
    """Metadata for the latest firmware of `name`. Devices poll this on a
    timer; if `version` differs from theirs, they fetch /firmware/download.

    Body: {name, version, size, sha256, download_url, board, description}.
    """
    meta = await _firmware_metadata(name)
    if not meta:
        raise HTTPException(status_code=404, detail=f"firmware '{name}' not available")
    return meta


@router.get("/firmware/download/{name}")
async def get_firmware_download(name: str, request: Request = None):
    """Raw firmware binary. Cached on the client via ETag (= sha256).

    Devices verify the sha256 against /firmware/latest before flashing.
    """
    if name not in FIRMWARE_TYPES:
        raise HTTPException(status_code=404, detail=f"unknown firmware '{name}'")
    data = await _firmware_mgr.get_firmware_binary(name)
    if not data:
        raise HTTPException(status_code=404, detail=f"firmware '{name}' not available")
    version = await _firmware_mgr.get_firmware_version(name) or "unknown"
    cached = _FW_HASH_CACHE.get(name)
    if not cached or cached[0] != version or cached[1] != len(data):
        sha = hashlib.sha256(data).hexdigest()
        _FW_HASH_CACHE[name] = (version, len(data), sha)
    else:
        sha = cached[2]

    headers = {
        "Content-Type": "application/octet-stream",
        "Content-Length": str(len(data)),
        "ETag": f'"{sha}"',
        "X-FoF-Firmware-Version": version,
        "X-FoF-Firmware-SHA256": sha,
    }
    if request is not None:
        inm = request.headers.get("if-none-match")
        if inm and inm.strip('"') == sha:
            return Response(status_code=304, headers=headers)
    return Response(content=data, headers=headers,
                    media_type="application/octet-stream")


@router.post("/{device_id}/ota/{firmware_name}")
async def push_firmware_by_name(device_id: str, firmware_name: str):
    """Push a firmware from the catalog to a specific node.

    Fetches the firmware binary (from GitHub or custom upload) and
    POSTs it to the node's /api/ota endpoint.
    """
    if not firmware_name.startswith("uplink-"):
        raise HTTPException(
            status_code=400,
            detail=f"Firmware '{firmware_name}' is not an uplink image",
        )

    from app.routers.detections import _node_heartbeats

    node_info = _node_heartbeats.get(device_id)
    if not node_info or not node_info.get("ip"):
        raise HTTPException(status_code=404, detail=f"Node '{device_id}' not found or no IP")

    fw_data = await _firmware_mgr.get_firmware_binary(firmware_name)
    if not fw_data:
        raise HTTPException(status_code=404, detail=f"Firmware '{firmware_name}' not available")

    ip = node_info["ip"]
    logger.warning("OTA push %s to %s (%s): %d bytes", firmware_name, device_id, ip, len(fw_data))

    try:
        result = await _run_subprocess(
            ["curl", "-s", "-X", "POST",
             f"http://{ip}/api/ota",
             "--data-binary", "@-",
             "-H", "Content-Type: application/octet-stream",
             "--connect-timeout", "8",
             "--max-time", "480",
             "--limit-rate", "64k"],
            input=fw_data, capture_output=True, timeout=490
        )
        if result.returncode == 0 and result.stdout:
            try:
                return {"ok": True, "device_id": device_id, "firmware": firmware_name,
                        "size": len(fw_data), "upload_rate_limit": "64k",
                        "response": __import__("json").loads(result.stdout)}
            except Exception:
                pass
        return {"ok": True, "device_id": device_id, "firmware": firmware_name,
                "size": len(fw_data), "upload_rate_limit": "64k",
                "message": "Node rebooting"}
    except subprocess.TimeoutExpired:
        return {"ok": True, "device_id": device_id, "firmware": firmware_name,
                "size": len(fw_data), "upload_rate_limit": "64k",
                "message": "Node rebooting"}
    except Exception as e:
        raise HTTPException(status_code=502, detail=f"OTA failed: {e}")


@router.post("/{device_id}/ota/scanner/{firmware_name}")
async def push_scanner_firmware(
    device_id: str,
    firmware_name: str,
    uart: str = "ble",
    legacy: bool = False,
    relay_mode: str = "auto",
):
    """Push firmware to a UART-connected scanner through the uplink node.

    Uses the v0.59+ staged handshake protocol:
      1. POST binary to uplink's /api/fw/upload — stages into a named partition.
      2. POST empty body to uplink's /api/fw/relay?uart={ble,wifi} — uplink streams
         staged firmware to scanner over UART with per-chunk CRC + selective NACK
         retransmit. Returns structured {ok, chunks, nacks, retries, elapsed_s,
         stage, error}.

    uart: "ble" or "wifi".
    """
    if not firmware_name.startswith("scanner-"):
        raise HTTPException(
            status_code=400,
            detail=f"Firmware '{firmware_name}' is not a scanner image",
        )

    from urllib.parse import quote

    from app.routers.detections import _node_heartbeats

    relay_mode = (relay_mode or "auto").strip().lower().replace("-", "_")
    allowed_relay_modes = {"auto", "staged", "staged_legacy", "direct_legacy"}
    if relay_mode not in allowed_relay_modes:
        raise HTTPException(
            status_code=400,
            detail=f"relay_mode must be one of {', '.join(sorted(allowed_relay_modes))}",
        )
    if legacy and relay_mode in {"auto", "staged"}:
        relay_mode = "staged_legacy"

    node_info = _node_heartbeats.get(device_id)
    if not node_info or not node_info.get("ip"):
        raise HTTPException(status_code=404, detail=f"Node '{device_id}' not found or no IP")

    fw_data = await _firmware_mgr.get_firmware_binary(firmware_name)
    if not fw_data:
        raise HTTPException(status_code=404, detail=f"Firmware '{firmware_name}' not available")
    fw_version = await _firmware_mgr.get_firmware_version(firmware_name) or firmware_name

    ip = node_info["ip"]
    size = len(fw_data)
    op_id = f"{device_id}-{int(time.time() * 1000)}"
    op = {
        "op_id": op_id,
        "device_id": device_id,
        "firmware": firmware_name,
        "uart": uart,
        "size": size,
        "target_version": fw_version,
        "relay_mode": relay_mode,
        "status": "staging",
        "started_at": time.time(),
        "attempts": [],
    }
    _firmware_operations[op_id] = op
    _firmware_operations_log.append(op_id)
    while len(_firmware_operations_log) > 50:
        stale = _firmware_operations_log.pop(0)
        _firmware_operations.pop(stale, None)

    logger.warning("[fw_op %s] scanner OTA %s → %s/%s (uart=%s, mode=%s, %d bytes)",
                   op_id, firmware_name, device_id, ip, uart, relay_mode, size)

    # ── Stage 1: upload firmware to uplink's cache partition ──────────────
    if relay_mode == "direct_legacy":
        op["stage_response"] = {"skipped": True, "reason": "direct_legacy_streaming"}
    else:
        try:
            r_stage = await _run_subprocess(
                ["curl", "-s", "-X", "POST",
                 f"http://{ip}/api/fw/upload?name={quote(firmware_name)}&version={quote(fw_version)}",
                 "--data-binary", "@-",
                 "-H", "Content-Type: application/octet-stream",
                 "--connect-timeout", "5",
                 "--max-time", "240"],
                input=fw_data, capture_output=True, timeout=245,
            )
        except subprocess.TimeoutExpired:
            op["status"] = "failed"
            op["error"] = "stage_timeout"
            op["finished_at"] = time.time()
            raise HTTPException(status_code=504, detail="Upload to uplink timed out")

        if r_stage.returncode != 0:
            err = r_stage.stderr.decode(errors="replace")[:200]
            op["status"] = "failed"
            op["error"] = f"stage_curl:{err}"
            op["finished_at"] = time.time()
            raise HTTPException(status_code=502, detail=f"Stage failed: {err}")

        try:
            stage_resp = json.loads(r_stage.stdout)
        except Exception:
            stage_resp = {"raw": r_stage.stdout.decode(errors="replace")[:200]}
        op["stage_response"] = stage_resp
        stage_ok = bool(stage_resp.get("stored") or stage_resp.get("ok"))
        if not stage_ok:
            op["status"] = "failed"
            op["error"] = "stage_rejected"
            op["finished_at"] = time.time()
            return {"ok": False, "op_id": op_id, "stage": "upload",
                    "error": "stage_rejected", "response": stage_resp}

    op["status"] = "flashing"

    # ── Stage 2: trigger relay to scanner over UART ──────────────────────
    async def _verify_relay_result(relay_response: dict) -> tuple[dict, dict]:
        verification_info = {
            "target_version": fw_version,
            "verified": False,
            "scanner_version": None,
        }
        if relay_response.get("ok"):
            op["status"] = "verifying"
            verified, scanner_version, scanner = await _wait_for_scanner_version(
                device_id,
                uart,
                fw_version,
                timeout_s=90.0,
            )
            verification_info.update({
                "verified": verified,
                "scanner_version": scanner_version or None,
                "scanner": scanner,
            })
            if not verified:
                relay_response = dict(relay_response)
                relay_response["ok"] = False
                relay_response["stage"] = "verify"
                relay_response["error"] = "scanner_version_verify_timeout"
        return relay_response, verification_info

    if relay_mode == "direct_legacy":
        relay = await _run_direct_legacy_scanner_relay(ip, uart, fw_data)
        op["attempts"].append({"mode": "direct_legacy", "response": relay})
    else:
        staged_legacy = relay_mode == "staged_legacy"
        relay_url = f"http://{ip}/api/fw/relay?uart={uart}"
        if staged_legacy:
            relay_url += "&legacy=1"
        try:
            r_relay = await _run_subprocess(
                ["curl", "-s", "-X", "POST",
                 relay_url,
                 "--connect-timeout", "5",
                 "--max-time", "420"],
                capture_output=True, timeout=430,
            )
        except subprocess.TimeoutExpired:
            op["status"] = "failed"
            op["error"] = "relay_timeout"
            op["finished_at"] = time.time()
            raise HTTPException(status_code=504, detail="Relay to scanner timed out")

        if r_relay.returncode != 0:
            err = r_relay.stderr.decode(errors="replace")[:200]
            op["status"] = "failed"
            op["error"] = f"relay_curl:{err}"
            op["finished_at"] = time.time()
            raise HTTPException(status_code=502, detail=f"Relay failed: {err}")

        try:
            relay = json.loads(r_relay.stdout)
        except Exception:
            relay = {"raw": r_relay.stdout.decode(errors="replace")[:200]}
        op["attempts"].append({
            "mode": "staged_legacy" if staged_legacy else "staged",
            "response": relay,
        })

        fallback_errors = {"stop_ack_timeout", "ota_ack_timeout", "finalize_timeout"}
        if (
            relay_mode == "auto"
            and not relay.get("ok")
            and relay.get("error") in fallback_errors
        ):
            logger.warning("[fw_op %s] staged relay failed (%s); retrying staged legacy relay",
                           op_id, relay.get("error"))
            op["normal_relay_response"] = relay
            try:
                r_relay = await _run_subprocess(
                    ["curl", "-s", "-X", "POST",
                     f"http://{ip}/api/fw/relay?uart={uart}&legacy=1",
                     "--connect-timeout", "5",
                     "--max-time", "420"],
                    capture_output=True, timeout=430,
                )
            except subprocess.TimeoutExpired:
                op["status"] = "failed"
                op["error"] = "legacy_relay_timeout"
                op["finished_at"] = time.time()
                raise HTTPException(status_code=504, detail="Legacy relay to scanner timed out")

            if r_relay.returncode != 0:
                err = r_relay.stderr.decode(errors="replace")[:200]
                op["status"] = "failed"
                op["error"] = f"legacy_relay_curl:{err}"
                op["finished_at"] = time.time()
                raise HTTPException(status_code=502, detail=f"Legacy relay failed: {err}")

            try:
                relay = json.loads(r_relay.stdout)
            except Exception:
                relay = {"raw": r_relay.stdout.decode(errors="replace")[:200]}
            op["legacy_fallback_used"] = True
            op["attempts"].append({"mode": "staged_legacy", "response": relay})

    relay, verification = await _verify_relay_result(relay)
    op["verification"] = verification

    direct_fallback_errors = {
        "stop_ack_timeout",
        "ota_ack_timeout",
        "finalize_timeout",
        "legacy_verify_timeout",
        "legacy_version_not_updated",
        "scanner_version_verify_timeout",
    }
    if (
        relay_mode == "auto"
        and not relay.get("ok")
        and (relay.get("stage") == "verify" or relay.get("error") in direct_fallback_errors)
    ):
        logger.warning("[fw_op %s] staged paths did not verify (%s); trying direct legacy stream",
                       op_id, relay.get("error"))
        op["staged_relay_response"] = relay
        direct_relay = await _run_direct_legacy_scanner_relay(ip, uart, fw_data)
        op["direct_legacy_fallback_used"] = True
        op["attempts"].append({"mode": "direct_legacy", "response": direct_relay})
        relay, verification = await _verify_relay_result(direct_relay)
        op["verification"] = verification

    op["status"] = "done" if relay.get("ok") else "failed"
    op["finished_at"] = time.time()
    op["relay_response"] = relay
    op["chunks_sent"] = relay.get("chunks", 0)
    op["nacks_seen"] = relay.get("nacks", 0)
    op["retries"] = relay.get("retries", 0)
    op["elapsed_s"] = relay.get("elapsed_s", 0)
    if not relay.get("ok"):
        op["error"] = f"{relay.get('stage','?')}:{relay.get('error','unknown')}"

    return {
        "ok": bool(relay.get("ok")),
        "op_id": op_id,
        "device_id": device_id,
        "firmware": firmware_name,
        "target_version": fw_version,
        "uart": uart,
        "relay_mode": relay_mode,
        "size": size,
        "chunks": relay.get("chunks", 0),
        "nacks": relay.get("nacks", 0),
        "retries": relay.get("retries", 0),
        "elapsed_s": relay.get("elapsed_s", 0),
        "stage": relay.get("stage", ""),
        "error": relay.get("error", "") if not relay.get("ok") else "",
        "verification": verification,
        "relay_response": relay,
        "attempts": op.get("attempts", []),
    }


@router.post("/{device_id}/ota/scanner/{firmware_name}/stage")
async def stage_scanner_firmware(
    device_id: str,
    firmware_name: str,
):
    """Stage scanner firmware on an uplink without immediately relaying it.

    New scanner firmware can then request the update over UART using fw_check /
    fw_ready, which avoids forcing a stop command through a saturated scanner
    stream.
    """
    if not firmware_name.startswith("scanner-"):
        raise HTTPException(
            status_code=400,
            detail=f"Firmware '{firmware_name}' is not a scanner image",
        )

    import json
    from urllib.parse import quote

    from app.routers.detections import _node_heartbeats

    node_info = _node_heartbeats.get(device_id)
    if not node_info or not node_info.get("ip"):
        raise HTTPException(status_code=404, detail=f"Node '{device_id}' not found or no IP")

    fw_data = await _firmware_mgr.get_firmware_binary(firmware_name)
    if not fw_data:
        raise HTTPException(status_code=404, detail=f"Firmware '{firmware_name}' not available")
    fw_version = await _firmware_mgr.get_firmware_version(firmware_name) or firmware_name

    ip = node_info["ip"]
    try:
        r_stage = await _run_subprocess(
            ["curl", "-s", "-X", "POST",
             f"http://{ip}/api/fw/upload?name={quote(firmware_name)}&version={quote(fw_version)}",
             "--data-binary", "@-",
             "-H", "Content-Type: application/octet-stream",
             "--connect-timeout", "5",
             "--max-time", "240"],
            input=fw_data, capture_output=True, timeout=245,
        )
    except subprocess.TimeoutExpired:
        raise HTTPException(status_code=504, detail="Upload to uplink timed out")

    if r_stage.returncode != 0:
        err = r_stage.stderr.decode(errors="replace")[:200]
        raise HTTPException(status_code=502, detail=f"Stage failed: {err}")

    try:
        stage_resp = json.loads(r_stage.stdout)
    except Exception:
        stage_resp = {"raw": r_stage.stdout.decode(errors="replace")[:200]}

    return {
        "ok": bool(stage_resp.get("stored") or stage_resp.get("ok")),
        "device_id": device_id,
        "firmware": firmware_name,
        "target_version": fw_version,
        "size": len(fw_data),
        "stage_response": stage_resp,
    }


@router.get("/firmware/scanner/readiness")
async def scanner_firmware_readiness(
    firmware_name: str | None = None,
    device_id: str | None = None,
    uart: str = "both",
    include_offline: bool = False,
):
    """Return scanner self-update readiness and exact USB-recovery blockers."""
    uart = (uart or "both").strip().lower()
    if uart not in {"ble", "wifi", "both"}:
        raise HTTPException(status_code=400, detail="uart must be ble, wifi, or both")
    targets = await _scanner_targets(
        device_id=device_id,
        firmware_name=firmware_name,
        include_offline=include_offline,
    )
    if uart != "both":
        targets = [target for target in targets if target["uart"] == uart]
    rows = []
    for target in targets:
        current = _normalized_firmware_version(target["current_version"])
        target_version = _normalized_firmware_version(target["target_version"])
        already_current = bool(target_version and current == target_version)
        blockers = []
        if not target.get("ip"):
            blockers.append("missing_uplink_ip")
        if not target.get("self_update_capable") and not already_current:
            blockers.append("scanner_command_ingress_unreachable")
        if not target.get("target_version"):
            blockers.append("target_version_unknown")
        rows.append({
            "device_id": target["device_id"],
            "uart": target["uart"],
            "current_version": target["current_version"],
            "target_firmware": target["target_firmware"],
            "target_version": target["target_version"],
            "board": target["scanner"].get("board"),
            "cmd_rx": target["scanner"].get("cmd_rx") or target["scanner"].get("cmd_rx_count") or 0,
            "fw_check_count": target["scanner"].get("fw_check_count") or 0,
            "fw_state": target["scanner"].get("fw_state") or target["scanner"].get("fw_update_state") or "idle",
            "fw_backoff_s": target["scanner"].get("fw_backoff_s") or 0,
            "last_fw_error": target["scanner"].get("last_fw_error") or "",
            "self_update_capable": target["self_update_capable"],
            "already_current": already_current,
            "state": "verified" if already_current else ("blocked" if blockers else "pending"),
            "blockers": blockers,
            "needs_usb_recovery": "scanner_command_ingress_unreachable" in blockers,
            "next_action": (
                "USB flash this scanner once with the recovery-capable image"
                if "scanner_command_ingress_unreachable" in blockers else
                ("none" if already_current else "stage firmware and trigger firmware check")
            ),
        })
    return {
        "count": len(rows),
        "ready_count": sum(1 for row in rows if row["self_update_capable"] or row["already_current"]),
        "blocked_count": sum(1 for row in rows if row["blockers"]),
        "needs_usb_recovery_count": sum(1 for row in rows if row["needs_usb_recovery"]),
        "scanners": rows,
    }


@router.post("/firmware/scanner/stage-fleet")
async def stage_scanner_firmware_fleet(
    firmware_name: str | None = None,
    device_id: str | None = None,
    include_offline: bool = False,
):
    """Stage target scanner firmware on each uplink without forcing relay."""
    targets = await _scanner_targets(
        device_id=device_id,
        firmware_name=firmware_name,
        include_offline=include_offline,
    )
    stage_keys: set[tuple[str, str]] = set()
    results = []
    for target in targets:
        key = (target["device_id"], target["target_firmware"])
        if key in stage_keys:
            continue
        stage_keys.add(key)
        if not target.get("ip"):
            results.append({
                "ok": False,
                "device_id": target["device_id"],
                "firmware": target["target_firmware"],
                "state": "blocked",
                "error": "missing_uplink_ip",
            })
            continue
        results.append(await _stage_scanner_firmware_on_uplink(
            target["device_id"],
            target["ip"],
            target["target_firmware"],
        ))
    return {
        "ok": bool(results) and all(row.get("ok") for row in results),
        "count": len(results),
        "results": results,
    }


@router.post("/firmware/scanner/trigger-check")
async def trigger_scanner_firmware_check(
    device_id: str | None = None,
    uart: str = "both",
    include_offline: bool = False,
):
    """Ask uplink(s) to tell scanner(s) to send fw_check now."""
    uart = (uart or "both").strip().lower()
    if uart not in {"ble", "wifi", "both"}:
        raise HTTPException(status_code=400, detail="uart must be ble, wifi, or both")

    results = []
    for node_id, node in _heartbeat_nodes(device_id, include_offline=include_offline):
        ip = node.get("ip")
        if not ip:
            results.append({
                "ok": False,
                "device_id": node_id,
                "uart": uart,
                "state": "blocked",
                "error": "missing_uplink_ip",
            })
            continue
        results.append(await _trigger_scanner_firmware_check(node_id, ip, uart))
    return {
        "ok": bool(results) and all(row.get("ok") for row in results),
        "count": len(results),
        "results": results,
    }


@router.post("/firmware/scanner/rollout")
async def start_scanner_firmware_rollout(
    mode: str = "canary",
    firmware_name: str | None = None,
    canary_device_id: str | None = None,
    canary_uart: str | None = None,
    require_canary_verified: bool = True,
):
    """Start a canary or fleet scanner self-update rollout.

    The rollout is backgrounded and serialized one scanner at a time. Success
    is always version proof from node heartbeat, never bytes-streamed alone.
    """
    mode = (mode or "canary").strip().lower()
    if mode == "continue":
        mode = "fleet"
    if mode not in {"canary", "fleet"}:
        raise HTTPException(status_code=400, detail="mode must be canary or fleet")
    canary_uart_filter = (canary_uart or "").strip().lower()
    if canary_uart_filter and canary_uart_filter not in {"ble", "wifi", "both"}:
        raise HTTPException(status_code=400, detail="canary_uart must be ble, wifi, or both")

    targets = await _scanner_targets(firmware_name=firmware_name)
    if canary_device_id:
        targets = [t for t in targets if t["device_id"] == canary_device_id]
    if canary_uart_filter in {"ble", "wifi"}:
        targets = [t for t in targets if t["uart"] == canary_uart_filter]
    if not targets:
        raise HTTPException(status_code=404, detail="No online scanner targets found")

    if mode == "canary":
        if canary_uart_filter == "both":
            if not canary_device_id:
                capable = [
                    t for t in targets
                    if t["self_update_capable"] or _scanner_is_target_version(t["scanner"], t["target_version"])
                ]
                selected_device = (capable[0] if capable else targets[0])["device_id"]
                targets = [t for t in targets if t["device_id"] == selected_device]
        else:
            capable = [
                t for t in targets
                if t["self_update_capable"] or _scanner_is_target_version(t["scanner"], t["target_version"])
            ]
            targets = [capable[0] if capable else targets[0]]
    elif require_canary_verified:
        verified_any = any(_scanner_is_target_version(t["scanner"], t["target_version"]) for t in targets)
        if not verified_any:
            raise HTTPException(
                status_code=409,
                detail="Run and verify a canary scanner before continuing fleet rollout",
            )

    rollout_id = f"scanner-{mode}-{int(time.time() * 1000)}"
    target_rows = {
        f"{t['device_id']}/{t['uart']}": {
            "device_id": t["device_id"],
            "uart": t["uart"],
            "state": "pending",
            "current_version": t["current_version"],
            "target_firmware": t["target_firmware"],
            "target_version": t["target_version"],
            "self_update_capable": t["self_update_capable"],
            "verified": False,
            "error": "",
        }
        for t in targets
    }
    rollout = {
        "rollout_id": rollout_id,
        "mode": mode,
        "status": "queued",
        "created_at": time.time(),
        "stage_results": [],
        "targets": target_rows,
        "concurrency": 1,
        "proof": "heartbeat_version",
    }
    _remember_rollout(rollout)
    _firmware_rollout_tasks[rollout_id] = asyncio.create_task(
        _run_scanner_rollout(rollout_id, targets, stage_first=True)
    )
    return {
        "ok": True,
        "rollout_id": rollout_id,
        "status": rollout["status"],
        "mode": mode,
        "target_count": len(targets),
        "targets": list(target_rows.values()),
    }


@router.get("/firmware/rollouts/{rollout_id}")
async def get_scanner_firmware_rollout(rollout_id: str):
    rollout = _firmware_rollouts.get(rollout_id)
    if not rollout:
        raise HTTPException(status_code=404, detail="Rollout not found")
    task = _firmware_rollout_tasks.get(rollout_id)
    return {
        **rollout,
        "task_done": bool(task.done()) if task else True,
    }


# In-memory firmware operation tracker. Retains the last 50 ops so the dashboard
# can poll progress. Not persisted — if the backend restarts mid-flash, the
# operation record is lost (but the actual flash continues on the ESP32 side).
_firmware_operations: dict[str, dict] = {}
_firmware_operations_log: list[str] = []


@router.get("/firmware/operations")
async def list_firmware_operations(
    device_id: str | None = None,
    status: str | None = None,
    limit: int = 20,
):
    """Return recent firmware-flash operations (in-memory, last 50)."""
    ops = [_firmware_operations[op_id]
           for op_id in reversed(_firmware_operations_log)
           if op_id in _firmware_operations]
    if device_id:
        ops = [o for o in ops if o.get("device_id") == device_id]
    if status:
        ops = [o for o in ops if o.get("status") == status]
    return {"count": len(ops[:limit]), "operations": ops[:limit]}


@router.get("/firmware/operations/{op_id}")
async def get_firmware_operation(op_id: str):
    op = _firmware_operations.get(op_id)
    if not op:
        raise HTTPException(status_code=404, detail="Operation not found")
    return op
