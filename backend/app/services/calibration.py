"""Inter-node RSSI calibration — measures RF environment between sensor nodes
to compute optimal path loss model for triangulation.

Protocol:
1. Each node takes a turn enabling its WiFi AP at max and min power
2. All other nodes measure RSSI of that AP (15s per power level)
3. Backend computes linear regression on (distance, RSSI) pairs
4. Persists calibration result to disk (survives restarts)
5. Updates triangulation engine with calibrated path_loss_exponent and RSSI_REF
"""

import asyncio
import json
import logging
import math
import os
import time
from dataclasses import dataclass, field
from pathlib import Path

import httpx

logger = logging.getLogger(__name__)


@dataclass
class CalibrationMeasurement:
    broadcaster_id: str
    listener_id: str
    broadcaster_lat: float
    broadcaster_lon: float
    listener_lat: float
    listener_lon: float
    distance_m: float
    avg_rssi: float
    samples: int
    timestamp: float


@dataclass
class CalibrationResult:
    path_loss_exponent: float
    rssi_ref: float
    r_squared: float
    measurements: list
    timestamp: float
    node_count: int


class CalibrationManager:
    """Orchestrates inter-node RSSI calibration."""

    MAX_HISTORY = 10

    # Two power levels for better model fit: max + min gives two data points
    # per node pair at the same distance, validating the path loss model.
    # Units of 0.25dBm: 80=20dBm (max), 44=11dBm (min safe, below drops STA)
    POWER_LEVELS = [80, 44]  # 20dBm, 11dBm
    MEASURE_DURATION_S = 15  # 15 seconds per power level = 30s per node
    CALIBRATION_CHANNEL = 6

    def __init__(self, data_dir: str | None = None):
        self.is_running = False
        self.progress = ""
        self.last_result: CalibrationResult | None = None
        self.measurements: list[CalibrationMeasurement] = []
        self.history: list[CalibrationResult] = []

        # Persist calibration results to disk
        if data_dir is None:
            data_dir = str(Path(__file__).parent.parent / "data")
        self._data_dir = data_dir
        self._cal_file = os.path.join(data_dir, "calibration.json")
        self._load_from_disk()

    def _save_to_disk(self):
        """Persist last result + history to JSON file."""
        try:
            os.makedirs(self._data_dir, exist_ok=True)
            data = {
                "last_result": {
                    "path_loss_exponent": self.last_result.path_loss_exponent,
                    "rssi_ref": self.last_result.rssi_ref,
                    "r_squared": self.last_result.r_squared,
                    "measurements": self.last_result.measurements,
                    "timestamp": self.last_result.timestamp,
                    "node_count": self.last_result.node_count,
                } if self.last_result else None,
                "history": [
                    {
                        "path_loss_exponent": r.path_loss_exponent,
                        "rssi_ref": r.rssi_ref,
                        "r_squared": r.r_squared,
                        "measurements": r.measurements,
                        "timestamp": r.timestamp,
                        "node_count": r.node_count,
                    }
                    for r in self.history
                ],
            }
            with open(self._cal_file, "w") as fp:
                json.dump(data, fp, indent=2)
            logger.info("Calibration saved to %s", self._cal_file)
        except Exception as e:
            logger.warning("Failed to save calibration: %s", e)

    def _load_from_disk(self):
        """Load persisted calibration on startup."""
        if not os.path.exists(self._cal_file):
            return
        try:
            with open(self._cal_file) as fp:
                data = json.load(fp)
            if data.get("last_result"):
                r = data["last_result"]
                self.last_result = CalibrationResult(
                    path_loss_exponent=r["path_loss_exponent"],
                    rssi_ref=r["rssi_ref"],
                    r_squared=r["r_squared"],
                    measurements=r.get("measurements", []),
                    timestamp=r["timestamp"],
                    node_count=r["node_count"],
                )
                self.progress = (
                    f"Loaded: n={self.last_result.path_loss_exponent:.2f} "
                    f"ref={self.last_result.rssi_ref:.1f}dBm "
                    f"R²={self.last_result.r_squared:.3f}")
            for h in data.get("history", []):
                self.history.append(CalibrationResult(
                    path_loss_exponent=h["path_loss_exponent"],
                    rssi_ref=h["rssi_ref"],
                    r_squared=h["r_squared"],
                    measurements=h.get("measurements", []),
                    timestamp=h["timestamp"],
                    node_count=h["node_count"],
                ))
            logger.info("Loaded calibration from disk: n=%.2f ref=%.1f",
                        self.last_result.path_loss_exponent if self.last_result else 0,
                        self.last_result.rssi_ref if self.last_result else 0)
        except Exception as e:
            logger.warning("Failed to load calibration: %s", e)

    async def run_calibration(self, nodes: list[dict]) -> CalibrationResult | None:
        """Run full calibration sequence across all online nodes.

        For each broadcaster node (~30s each):
        1. Enable AP at max power (20dBm), all listeners measure 15s
        2. Drop to min power (11dBm), all listeners measure 15s
        3. Reboot broadcaster to return to normal mode
        4. After all nodes, compute path loss model from node-pair matrix
        5. Persist results to disk

        Args:
            nodes: list of dicts with {device_id, ip, lat, lon, name}
        """
        if self.is_running:
            logger.warning("Calibration already in progress")
            return None

        self.is_running = True
        self.measurements = []
        self.progress = "Starting calibration..."

        try:
            valid_nodes = [n for n in nodes if n.get("lat") and n.get("lon") and n.get("ip")]
            if len(valid_nodes) < 2:
                self.progress = "Need at least 2 nodes with GPS positions"
                return None

            per_node = len(self.POWER_LEVELS) * self.MEASURE_DURATION_S + 10
            total_time = len(valid_nodes) * per_node
            logger.info("Starting calibration with %d nodes, %d power levels (~%ds total)",
                        len(valid_nodes), len(self.POWER_LEVELS), total_time)

            async with httpx.AsyncClient(timeout=120.0) as client:
                for i, broadcaster in enumerate(valid_nodes):
                    bcast_name = broadcaster.get("name", broadcaster["device_id"])
                    logger.info("Calibration: %s (%s) broadcasting",
                                broadcaster["device_id"], bcast_name)

                    # 1. Start AP at first power level
                    try:
                        resp = await client.post(
                            f"http://{broadcaster['ip']}/api/calibrate/start",
                            params={"power": self.POWER_LEVELS[0],
                                    "channel": self.CALIBRATION_CHANNEL}
                        )
                        if resp.status_code != 200:
                            logger.warning("Failed to start AP on %s: %d",
                                           broadcaster["device_id"], resp.status_code)
                            continue
                        ap_info = resp.json()
                        bssid = ap_info.get("bssid", "")
                        channel = ap_info.get("channel", self.CALIBRATION_CHANNEL)
                        logger.info("  AP: BSSID=%s ch=%d", bssid, channel)
                    except Exception as e:
                        logger.warning("Failed to reach %s: %s",
                                       broadcaster["device_id"], e)
                        continue

                    await asyncio.sleep(2)  # Let AP become visible

                    # 2. For each power level, measure RSSI on all listeners
                    listeners = [n for n in valid_nodes
                                 if n["device_id"] != broadcaster["device_id"]]

                    for pi, power_level in enumerate(self.POWER_LEVELS):
                        power_dbm = power_level * 0.25
                        self.progress = (
                            f"Node {i+1}/{len(valid_nodes)} {bcast_name}: "
                            f"{power_dbm:.0f}dBm ({pi+1}/{len(self.POWER_LEVELS)})")

                        # Set power level (skip first — already set during start)
                        if pi > 0:
                            try:
                                await client.post(
                                    f"http://{broadcaster['ip']}/api/calibrate/power",
                                    params={"level": power_level})
                                await asyncio.sleep(1)
                            except Exception:
                                continue

                        # All listeners measure in parallel
                        measure_tasks = [
                            self._measure_node(client, listener, bssid, channel,
                                               self.MEASURE_DURATION_S)
                            for listener in listeners
                        ]
                        results = await asyncio.gather(*measure_tasks,
                                                       return_exceptions=True)

                        for j, listener in enumerate(listeners):
                            if j >= len(results) or isinstance(results[j], Exception):
                                continue
                            result = results[j]
                            if not result or result.get("samples", 0) == 0:
                                continue

                            distance = _haversine_m(
                                broadcaster["lat"], broadcaster["lon"],
                                listener["lat"], listener["lon"])
                            if distance < 1:
                                continue

                            self.measurements.append(CalibrationMeasurement(
                                broadcaster_id=broadcaster["device_id"],
                                listener_id=listener["device_id"],
                                broadcaster_lat=broadcaster["lat"],
                                broadcaster_lon=broadcaster["lon"],
                                listener_lat=listener["lat"],
                                listener_lon=listener["lon"],
                                distance_m=distance,
                                avg_rssi=result["avg_rssi"],
                                samples=result.get("samples", 0),
                                timestamp=time.time(),
                            ))
                            logger.info("  %s → %s: %.1fm → %.1fdBm (%d samples) @%.0fdBm",
                                        broadcaster["device_id"][-6:],
                                        listener["device_id"][-6:],
                                        distance, result["avg_rssi"],
                                        result.get("samples", 0), power_dbm)

                    # 3. Stop broadcaster (reboots to free RAM)
                    try:
                        await client.post(
                            f"http://{broadcaster['ip']}/api/calibrate/stop")
                    except Exception:
                        pass  # Node reboots, connection drops

                    self.progress = f"Node {i+1}/{len(valid_nodes)} {bcast_name}: done, rebooting..."
                    await asyncio.sleep(5)  # Wait for reboot

            # 4. Build node-pair matrix and compute calibration model
            if len(self.measurements) < 2:
                self.progress = f"Not enough measurements ({len(self.measurements)})"
                return None

            self.progress = "Computing calibration model from node-pair matrix..."
            result = self._compute_calibration()
            self.last_result = result
            self.history.append(result)
            if len(self.history) > self.MAX_HISTORY:
                self.history.pop(0)

            # 5. Persist to disk so it survives restarts
            self._save_to_disk()

            self.progress = (f"Calibrated: n={result.path_loss_exponent:.2f} "
                             f"ref={result.rssi_ref:.1f}dBm R²={result.r_squared:.3f} "
                             f"({len(self.measurements)} measurements from "
                             f"{result.node_count} nodes)")
            logger.info("Calibration complete: n=%.2f ref=%.1f R²=%.3f (%d measurements)",
                        result.path_loss_exponent, result.rssi_ref,
                        result.r_squared, len(self.measurements))
            return result

        finally:
            self.is_running = False

    async def _measure_node(self, client: httpx.AsyncClient, node: dict,
                             bssid: str, channel: int, duration: int) -> dict | None:
        """Tell a node to measure RSSI of a specific BSSID for N seconds."""
        try:
            resp = await client.post(
                f"http://{node['ip']}/api/calibrate/measure",
                params={"bssid": bssid, "channel": channel, "duration": duration},
                timeout=duration + 15  # Allow extra time for scan completion
            )
            if resp.status_code == 200:
                return resp.json()
        except Exception as e:
            logger.warning("Failed to measure on %s: %s", node["device_id"], e)
        return None

    def _compute_calibration(self) -> CalibrationResult:
        """Linear regression on (distance, RSSI) to find path_loss_exponent and RSSI_REF.

        Model: RSSI = RSSI_ref - 10 * n * log10(d)
        Rearranged: RSSI = n * (-10 * log10(d)) + RSSI_ref
        Linear: y = m*x + b where x = -10*log10(d), y = RSSI, m = n, b = RSSI_ref
        """
        n = len(self.measurements)
        x_vals = []
        y_vals = []

        for m in self.measurements:
            if m.distance_m > 0 and m.avg_rssi < 0:
                x_vals.append(-10.0 * math.log10(m.distance_m))
                y_vals.append(m.avg_rssi)

        if len(x_vals) < 2:
            return CalibrationResult(
                path_loss_exponent=3.0, rssi_ref=-50.0,
                r_squared=0.0, measurements=[], timestamp=time.time(), node_count=0
            )

        # Simple linear regression (no numpy needed)
        n_pts = len(x_vals)
        sum_x = sum(x_vals)
        sum_y = sum(y_vals)
        sum_xy = sum(x * y for x, y in zip(x_vals, y_vals))
        sum_x2 = sum(x * x for x in x_vals)

        denom = n_pts * sum_x2 - sum_x * sum_x
        if abs(denom) < 1e-10:
            return CalibrationResult(
                path_loss_exponent=3.0, rssi_ref=-50.0,
                r_squared=0.0, measurements=[], timestamp=time.time(), node_count=0
            )

        slope = (n_pts * sum_xy - sum_x * sum_y) / denom  # path_loss_exponent
        intercept = (sum_y - slope * sum_x) / n_pts        # RSSI_ref

        # R² calculation
        mean_y = sum_y / n_pts
        ss_tot = sum((y - mean_y) ** 2 for y in y_vals)
        ss_res = sum((y - (slope * x + intercept)) ** 2 for x, y in zip(x_vals, y_vals))
        r_squared = 1.0 - (ss_res / ss_tot) if ss_tot > 0 else 0.0

        # Clamp to reasonable values
        path_loss = max(1.5, min(slope, 5.0))
        rssi_ref = max(-70.0, min(intercept, -30.0))

        return CalibrationResult(
            path_loss_exponent=round(path_loss, 2),
            rssi_ref=round(rssi_ref, 1),
            r_squared=round(r_squared, 3),
            measurements=[
                {
                    "broadcaster": m.broadcaster_id,
                    "listener": m.listener_id,
                    "distance_m": round(m.distance_m, 1),
                    "avg_rssi": round(m.avg_rssi, 1),
                    "samples": m.samples,
                }
                for m in self.measurements
            ],
            timestamp=time.time(),
            node_count=len(set(m.broadcaster_id for m in self.measurements)),
        )

    def get_node_pair_matrix(self) -> dict:
        """Build a node-pair RSSI/distance matrix from the last calibration.

        Returns {nodes: [ids], matrix: {src: {dst: {rssi, distance, samples}}}}
        Each direction (A→B vs B→A) is kept separate since antenna patterns differ.
        """
        if not self.last_result or not self.last_result.measurements:
            return {"nodes": [], "matrix": {}, "pairs": []}

        node_ids = set()
        matrix = {}
        pairs = []

        for m in self.last_result.measurements:
            src = m["broadcaster"]
            dst = m["listener"]
            node_ids.add(src)
            node_ids.add(dst)

            if src not in matrix:
                matrix[src] = {}
            matrix[src][dst] = {
                "rssi": m["avg_rssi"],
                "distance_m": m["distance_m"],
                "samples": m["samples"],
            }

            # Build flat pairs list for easy rendering
            pairs.append({
                "from": src,
                "to": dst,
                "rssi": m["avg_rssi"],
                "distance_m": m["distance_m"],
                "samples": m["samples"],
                # Estimated path loss for this pair
                "path_loss": round(
                    (self.last_result.rssi_ref - m["avg_rssi"])
                    / (10 * math.log10(m["distance_m"])) if m["distance_m"] > 1 else 0,
                    2),
            })

        return {
            "nodes": sorted(node_ids),
            "matrix": matrix,
            "pairs": pairs,
            "model": {
                "path_loss_exponent": self.last_result.path_loss_exponent,
                "rssi_ref": self.last_result.rssi_ref,
                "r_squared": self.last_result.r_squared,
            },
        }

    def get_status(self) -> dict:
        return {
            "running": self.is_running,
            "progress": self.progress,
            "measurement_count": len(self.measurements),
            "last_result": {
                "path_loss_exponent": self.last_result.path_loss_exponent,
                "rssi_ref": self.last_result.rssi_ref,
                "r_squared": self.last_result.r_squared,
                "node_count": self.last_result.node_count,
                "measurements": self.last_result.measurements,
                "timestamp": self.last_result.timestamp,
            } if self.last_result else None,
        }

    def get_history(self) -> list[dict]:
        return [
            {
                "path_loss_exponent": r.path_loss_exponent,
                "rssi_ref": r.rssi_ref,
                "r_squared": r.r_squared,
                "node_count": r.node_count,
                "measurement_count": len(r.measurements),
                "timestamp": r.timestamp,
            }
            for r in reversed(self.history)
        ]


def _haversine_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Haversine distance in meters between two GPS coordinates."""
    R = 6371000  # Earth radius in meters
    phi1 = math.radians(lat1)
    phi2 = math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlam = math.radians(lon2 - lon1)
    a = math.sin(dphi / 2) ** 2 + math.cos(phi1) * math.cos(phi2) * math.sin(dlam / 2) ** 2
    return R * 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))
