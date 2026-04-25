"""Android-led calibration session orchestration.

This layer owns only the fleet-mode lifecycle:
create a phone-walk session, switch live nodes into calibration mode,
renew leases while the walk is active, and persist the completed walk.
"""

from __future__ import annotations

import asyncio
import json
import logging
import time
from pathlib import Path
from typing import Any

import httpx

from app.services.applied_calibration import AppliedCalibrationStore
from app.services.phone_calibration import PhoneCalibrationManager, WalkSession

logger = logging.getLogger(__name__)


class CalibrationModeCoordinator:
    LEASE_TTL_S = 30.0

    def __init__(
        self,
        phone_cal_mgr: PhoneCalibrationManager,
        applied_store: AppliedCalibrationStore,
        session_dir: str | Path | None = None,
    ) -> None:
        self.phone_cal_mgr = phone_cal_mgr
        self.applied_store = applied_store
        self.session_dir = Path(session_dir) if session_dir is not None else (
            Path(__file__).parent.parent / "data" / "calibration_sessions"
        )
        self.active_session_id: str | None = None
        self.fleet_mode_state = "inactive"
        self._lease_expires_at = 0.0
        self._lock = asyncio.Lock()

    def _session(self, session_id: str) -> WalkSession | None:
        return self.phone_cal_mgr.get(session_id)

    def active_for_uuid(self, uuid_token: str | None) -> WalkSession | None:
        session = self.phone_cal_mgr.find_active_for_uuid(uuid_token or "")
        if session is None:
            return None
        if session.session_id != self.active_session_id:
            return None
        if self.fleet_mode_state != "active":
            return None
        return session

    def lease_state(self, session_id: str | None) -> str:
        if not session_id or session_id != self.active_session_id:
            return "inactive"
        return self.fleet_mode_state

    def renew_lease(self, session_id: str) -> None:
        if session_id == self.active_session_id and self.fleet_mode_state == "active":
            self._lease_expires_at = time.time() + self.LEASE_TTL_S

    async def _post_node(self, node: dict[str, Any], path: str, payload: dict[str, Any] | None) -> dict[str, Any]:
        ip = (node.get("ip") or "").strip()
        if not ip:
            raise RuntimeError(f"{node.get('device_id')}: missing IP")
        url = f"http://{ip}{path}"
        async with httpx.AsyncClient(timeout=8.0) as client:
            response = await client.post(url, json=payload)
        if response.status_code >= 400:
            raise RuntimeError(f"{node.get('device_id')}: HTTP {response.status_code} {response.text[:160]}")
        data = response.json()
        if not data.get("ok"):
            raise RuntimeError(f"{node.get('device_id')}: {data}")
        return data

    async def _get_node_mode(self, node: dict[str, Any]) -> dict[str, Any]:
        ip = (node.get("ip") or "").strip()
        if not ip:
            raise RuntimeError(f"{node.get('device_id')}: missing IP")
        url = f"http://{ip}/api/calibration/mode"
        async with httpx.AsyncClient(timeout=4.0) as client:
            response = await client.get(url)
        if response.status_code >= 400:
            raise RuntimeError(f"{node.get('device_id')}: HTTP {response.status_code} {response.text[:160]}")
        data = response.json()
        if not data.get("ok", True):
            raise RuntimeError(f"{node.get('device_id')}: {data}")
        return data

    @staticmethod
    def _mode_payload_is_clean_normal(payload: dict[str, Any]) -> bool:
        if payload.get("scan_mode") != "normal":
            return False
        for slot_name in ("ble", "wifi"):
            slot = payload.get(slot_name)
            if not isinstance(slot, dict) or not slot.get("connected"):
                continue
            if slot.get("scan_mode") != "normal":
                return False
        return True

    async def _normalize_node_mode(self, node: dict[str, Any]) -> None:
        payload = await self._get_node_mode(node)
        if self._mode_payload_is_clean_normal(payload):
            return

        session_id = str(payload.get("session_id") or "stale")
        await self._stop_node_mode(node, session_id, "normalize_before_start")
        after = await self._get_node_mode(node)
        if not self._mode_payload_is_clean_normal(after):
            raise RuntimeError(
                f"{node.get('device_id')}: calibration mode degraded after stop: {after}"
            )

    async def _start_node_mode(self, node: dict[str, Any], session: WalkSession) -> dict[str, Any]:
        await self._normalize_node_mode(node)
        return await self._post_node(
            node,
            "/api/calibration/mode/start",
            {
                "session_id": session.session_id,
                "advertise_uuid": session.expected_uuid,
            },
        )

    async def _stop_node_mode(self, node: dict[str, Any], session_id: str, reason: str) -> dict[str, Any]:
        return await self._post_node(
            node,
            "/api/calibration/mode/stop",
            {
                "session_id": session_id,
                "reason": reason,
            },
        )

    async def start_session(
        self,
        *,
        operator_label: str,
        tx_power_dbm: float | None,
        target_nodes: list[dict[str, Any]],
    ) -> WalkSession:
        async with self._lock:
            if self.active_session_id and self.fleet_mode_state == "active":
                raise RuntimeError("calibration mode already active")
            session = self.phone_cal_mgr.start(operator_label, tx_power_dbm)
            session.target_nodes = [
                {
                    "device_id": node["device_id"],
                    "name": node.get("name") or node["device_id"],
                    "ip": node.get("ip"),
                    "lat": node.get("lat"),
                    "lon": node.get("lon"),
                    "mode_state": node.get("mode_state"),
                }
                for node in target_nodes
            ]
            session.target_sensor_ids = [node["device_id"] for node in target_nodes]
            session.mode_state = "arming"
            session.abort_reason = None
            started_nodes: list[dict[str, Any]] = []
            try:
                results = await asyncio.gather(
                    *(self._start_node_mode(node, session) for node in target_nodes),
                    return_exceptions=True,
                )
                errors: list[Exception] = []
                for node, result in zip(target_nodes, results, strict=False):
                    if isinstance(result, Exception):
                        errors.append(result)
                    else:
                        started_nodes.append(node)
                if errors:
                    raise RuntimeError("; ".join(str(e) for e in errors))
            except Exception:
                for node in started_nodes:
                    try:
                        await self._stop_node_mode(node, session.session_id, "rollback_start_failure")
                    except Exception:
                        logger.warning("Failed rollback stop for %s", node.get("device_id"), exc_info=True)
                self.phone_cal_mgr.sessions.pop(session.session_id, None)
                raise

            session.mode_state = "active"
            session.mode_started_at = time.time()
            self.active_session_id = session.session_id
            self.fleet_mode_state = "active"
            self._lease_expires_at = time.time() + self.LEASE_TTL_S
            return session

    async def stop_session_mode(self, session_id: str, *, reason: str) -> None:
        async with self._lock:
            session = self._session(session_id)
            if session is None:
                return
            target_nodes = list(getattr(session, "target_nodes", []) or [])
            if target_nodes:
                await asyncio.gather(
                    *(self._stop_node_mode(node, session_id, reason) for node in target_nodes),
                    return_exceptions=True,
                )
            if self.active_session_id == session_id:
                self.active_session_id = None
                self.fleet_mode_state = "inactive"
                self._lease_expires_at = 0.0
            session.mode_state = "inactive"
            session.mode_stopped_at = time.time()

    async def abort_session(self, session_id: str, *, reason: str) -> WalkSession | None:
        session = self._session(session_id)
        if session is None:
            return None
        await self.stop_session_mode(session_id, reason=reason)
        session.ended_at = time.time()
        session.abort_reason = reason
        session.fit_result = {
            "ok": False,
            "reason": "aborted",
            "abort_reason": reason,
            "trace_points": len(session.trace),
            "samples_total": len(session.samples),
            "checkpoints_total": len(session.checkpoints),
        }
        self._persist_completed_session(session)
        return session

    async def end_session(
        self,
        session_id: str,
        *,
        provisional_fit: dict[str, Any] | None,
        apply_requested: bool,
    ) -> dict[str, Any]:
        session = self.phone_cal_mgr.end(session_id)
        if session is None:
            raise RuntimeError("unknown session")
        await self.stop_session_mode(session_id, reason="walk_end")
        verified_fit = session.fit_result or {"ok": False, "reason": "missing_fit"}
        applied = False
        apply_reason = "apply_not_requested"
        if apply_requested:
            if session.abort_reason:
                apply_reason = f"session_aborted:{session.abort_reason}"
            elif not verified_fit.get("ok"):
                apply_reason = f"verify_failed:{verified_fit.get('reason', 'unknown')}"
            elif float(verified_fit.get("global_r_squared") or 0.0) < 0.4:
                apply_reason = "quality_gate_r2_below_0_4"
            else:
                self.applied_store.save_verified_model(
                    session_id=session_id,
                    verified_fit=verified_fit,
                    provisional_fit=provisional_fit,
                )
                applied = True
                apply_reason = "applied"
        session.provisional_fit = provisional_fit
        session.verified_fit = verified_fit
        session.apply_requested = apply_requested
        session.applied = applied
        session.apply_reason = apply_reason
        self._persist_completed_session(session)
        return {
            "verified_fit": verified_fit,
            "provisional_fit": provisional_fit,
            "applied": applied,
            "apply_reason": apply_reason,
        }

    async def expire_stale_session(self) -> None:
        if not self.active_session_id or self.fleet_mode_state != "active":
            return
        if time.time() <= self._lease_expires_at:
            return
        logger.warning("Calibration session %s lease expired — aborting mode", self.active_session_id)
        await self.abort_session(self.active_session_id, reason="lease_expired")

    def _persist_completed_session(self, session: WalkSession) -> None:
        self.session_dir.mkdir(parents=True, exist_ok=True)
        payload = {
            "session_id": session.session_id,
            "operator_label": session.operator_label,
            "advertise_uuid": session.expected_uuid,
            "tx_power_dbm": session.tx_power_dbm,
            "started_at": session.started_at,
            "ended_at": session.ended_at,
            "mode_state": getattr(session, "mode_state", "inactive"),
            "abort_reason": getattr(session, "abort_reason", None),
            "target_nodes": getattr(session, "target_nodes", []),
            "trace": [vars(tp) for tp in session.trace],
            "samples": [vars(sample) for sample in session.samples],
            "checkpoints": [vars(cp) for cp in session.checkpoints],
            "provisional_fit": getattr(session, "provisional_fit", None),
            "verified_fit": getattr(session, "verified_fit", session.fit_result),
            "apply_requested": getattr(session, "apply_requested", False),
            "applied": getattr(session, "applied", False),
            "apply_reason": getattr(session, "apply_reason", None),
        }
        path = self.session_dir / f"{session.session_id}.json"
        path.write_text(json.dumps(payload, indent=2, sort_keys=True))
