"""Applied calibration model persistence for Android-led walks.

The backend is still the final authority for what gets applied to live
triangulation, but it no longer owns a runnable calibration workflow.
This store only persists the last verified model that was actually
applied, so /detections/calibrate/model reflects live truth across
restarts without depending on the removed legacy CalibrationManager.
"""

from __future__ import annotations

import json
import logging
import time
from pathlib import Path
from typing import Any

from app.services import triangulation

logger = logging.getLogger(__name__)


class AppliedCalibrationStore:
    def __init__(self, path: str | Path | None = None) -> None:
        default_path = Path(__file__).parent.parent / "data" / "applied_calibration.json"
        self.path = Path(path) if path is not None else default_path
        self.record: dict[str, Any] | None = None

    def load(self) -> dict[str, Any] | None:
        if not self.path.exists():
            self.record = None
            return None
        try:
            data = json.loads(self.path.read_text())
        except Exception as exc:
            logger.warning("Failed to read applied calibration %s: %s", self.path, exc)
            self.record = None
            return None

        try:
            triangulation.update_calibration(
                rssi_ref=float(data["rssi_ref"]),
                path_loss=float(data["path_loss_exponent"]),
                per_listener_model=data.get("per_listener_model") or {},
            )
        except Exception as exc:
            logger.warning("Failed to apply persisted calibration: %s", exc)
            self.record = None
            return None

        self.record = data
        logger.info(
            "Loaded applied calibration: ref=%.1f n=%.3f r2=%.3f listeners=%d",
            float(data["rssi_ref"]),
            float(data["path_loss_exponent"]),
            float(data.get("r_squared") or 0.0),
            len(data.get("per_listener_model") or {}),
        )
        return data

    def save_verified_model(
        self,
        *,
        session_id: str,
        verified_fit: dict[str, Any],
        provisional_fit: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        per_listener_model = {
            sensor_id: [float(info["rssi_ref"]), float(info["path_loss_exponent"])]
            for sensor_id, info in (verified_fit.get("per_listener") or {}).items()
            if info.get("ok")
        }
        record = {
            "session_id": session_id,
            "applied_at": time.time(),
            "rssi_ref": float(verified_fit["global_rssi_ref"]),
            "path_loss_exponent": float(verified_fit["global_path_loss_exponent"]),
            "r_squared": float(verified_fit.get("global_r_squared") or 0.0),
            "per_listener_model": per_listener_model,
            "verified_fit": verified_fit,
            "provisional_fit": provisional_fit,
            "source": "android_walk_verified",
        }
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.write_text(json.dumps(record, indent=2, sort_keys=True))
        triangulation.update_calibration(
            rssi_ref=record["rssi_ref"],
            path_loss=record["path_loss_exponent"],
            per_listener_model=record["per_listener_model"],
        )
        self.record = record
        logger.info(
            "Applied verified calibration: session=%s ref=%.1f n=%.3f r2=%.3f listeners=%d",
            session_id,
            record["rssi_ref"],
            record["path_loss_exponent"],
            record["r_squared"],
            len(per_listener_model),
        )
        return record

    def summary(self) -> dict[str, Any]:
        if self.record is None:
            return {
                "rssi_ref": triangulation.RSSI_REF,
                "path_loss_exponent": triangulation.PATH_LOSS_OUTDOOR,
                "is_calibrated": False,
                "is_active": False,
                "is_trusted": False,
                "active_model_source": "defaults",
                "applied_listener_count": len(triangulation.PER_LISTENER_MODEL),
                "last_calibration": None,
                "r_squared": None,
            }

        return {
            "rssi_ref": triangulation.RSSI_REF,
            "path_loss_exponent": triangulation.PATH_LOSS_OUTDOOR,
            "is_calibrated": True,
            "is_active": True,
            "is_trusted": True,
            "active_model_source": self.record.get("source", "android_walk_verified"),
            "applied_listener_count": len(self.record.get("per_listener_model") or {}),
            "last_calibration": self.record.get("applied_at"),
            "r_squared": self.record.get("r_squared"),
            "session_id": self.record.get("session_id"),
        }
