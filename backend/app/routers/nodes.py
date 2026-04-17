"""Sensor node management endpoints — register, update, list, delete, OTA."""

import logging
from datetime import datetime, timezone

import httpx
from fastapi import APIRouter, Depends, HTTPException, UploadFile, File
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


def _node_to_response(node: SensorNode) -> NodeResponse:
    return NodeResponse(
        device_id=node.device_id,
        name=node.name,
        lat=node.lat,
        lon=node.lon,
        alt=node.alt,
        is_fixed=node.is_fixed,
        sensor_type=node.sensor_type,
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
        existing.last_seen = datetime.now(timezone.utc)
        await db.commit()
        await db.refresh(existing)
        logger.info("Updated fixed node: %s at (%f, %f) type=%s", req.device_id, req.lat, req.lon, req.sensor_type)
        return _node_to_response(existing)

    node = SensorNode(
        device_id=req.device_id,
        name=req.name,
        lat=req.lat,
        lon=req.lon,
        alt=req.alt,
        is_fixed=True,
        sensor_type=req.sensor_type,
        last_seen=datetime.now(timezone.utc),
    )
    db.add(node)
    await db.commit()
    await db.refresh(node)
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
    node.last_seen = datetime.now(timezone.utc)

    await db.commit()
    await db.refresh(node)
    logger.info("Updated node %s: name=%s pos=(%f, %f)", device_id, node.name, node.lat, node.lon)
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
        import subprocess
        # Use curl — httpx chunked encoding breaks ESP32 HTTP server
        result = subprocess.run(
            ["curl", "-s", "-X", "POST",
             f"http://{ip}/api/ota",
             "--data-binary", "@-",
             "-H", "Content-Type: application/octet-stream",
             "--connect-timeout", "5",
             "--max-time", "120"],
            input=fw_data, capture_output=True, timeout=125
        )
        if result.returncode == 0 and result.stdout:
            try:
                resp = __import__("json").loads(result.stdout)
                logger.info("OTA to %s success: %s", device_id, resp)
                return {"ok": True, "device_id": device_id, "size": fw_size, "response": resp}
            except Exception:
                pass
        # curl may return 0 even if node rebooted mid-response
        logger.info("OTA to %s: firmware sent, node likely rebooting", device_id)
        return {"ok": True, "device_id": device_id, "size": fw_size, "message": "Node rebooting"}
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


@router.post("/{device_id}/ota/{firmware_name}")
async def push_firmware_by_name(device_id: str, firmware_name: str):
    """Push a firmware from the catalog to a specific node.

    Fetches the firmware binary (from GitHub or custom upload) and
    POSTs it to the node's /api/ota endpoint.
    """
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
        import subprocess
        result = subprocess.run(
            ["curl", "-s", "-X", "POST",
             f"http://{ip}/api/ota",
             "--data-binary", "@-",
             "-H", "Content-Type: application/octet-stream",
             "--connect-timeout", "5",
             "--max-time", "120"],
            input=fw_data, capture_output=True, timeout=125
        )
        if result.returncode == 0 and result.stdout:
            try:
                return {"ok": True, "device_id": device_id, "firmware": firmware_name,
                        "size": len(fw_data), "response": __import__("json").loads(result.stdout)}
            except Exception:
                pass
        return {"ok": True, "device_id": device_id, "firmware": firmware_name,
                "size": len(fw_data), "message": "Node rebooting"}
    except subprocess.TimeoutExpired:
        return {"ok": True, "device_id": device_id, "firmware": firmware_name,
                "size": len(fw_data), "message": "Node rebooting"}
    except Exception as e:
        raise HTTPException(status_code=502, detail=f"OTA failed: {e}")


@router.post("/{device_id}/ota/scanner/{firmware_name}")
async def push_scanner_firmware(
    device_id: str,
    firmware_name: str,
    uart: str = "ble",
):
    """Push firmware to a UART-connected scanner through the uplink node.

    The backend sends the firmware to the uplink's /api/ota/relay endpoint,
    which relays it to the scanner over UART. The scanner writes it to its
    OTA partition and reboots.

    uart: "ble" for the BLE scanner UART, "wifi" for the WiFi scanner UART.
    """
    from app.routers.detections import _node_heartbeats

    node_info = _node_heartbeats.get(device_id)
    if not node_info or not node_info.get("ip"):
        raise HTTPException(status_code=404, detail=f"Node '{device_id}' not found or no IP")

    fw_data = await _firmware_mgr.get_firmware_binary(firmware_name)
    if not fw_data:
        raise HTTPException(status_code=404, detail=f"Firmware '{firmware_name}' not available")

    ip = node_info["ip"]
    logger.warning("Scanner OTA: %s → %s via %s (uart=%s, %d bytes)",
                    firmware_name, device_id, ip, uart, len(fw_data))

    try:
        import subprocess
        # Use curl subprocess — httpx chunked encoding breaks ESP32 HTTP server
        result = subprocess.run(
            ["curl", "-s", "-X", "POST",
             f"http://{ip}/api/ota/relay?uart={uart}",
             "--data-binary", "@-",
             "-H", "Content-Type: application/octet-stream",
             "--connect-timeout", "5",
             "--max-time", "180"],
            input=fw_data, capture_output=True, timeout=185
        )
        if result.returncode == 0:
            try:
                resp = __import__("json").loads(result.stdout)
            except Exception:
                resp = {"raw": result.stdout.decode(errors="replace")[:200]}
            return {"ok": True, "device_id": device_id, "firmware": firmware_name,
                    "size": len(fw_data), "uart": uart, "relay_response": resp}
        else:
            return {"ok": False, "error": result.stderr.decode(errors="replace")[:200]}
    except subprocess.TimeoutExpired:
        return {"ok": True, "device_id": device_id, "firmware": firmware_name,
                "size": len(fw_data), "message": "Scanner rebooting via relay"}
    except Exception as e:
        logger.error("Scanner OTA relay failed: %s", e)
        raise HTTPException(status_code=502, detail=f"Scanner OTA relay failed: {e}")
