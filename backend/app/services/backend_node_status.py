"""Server-timed liveness and bounded observation-time policy for uplinks."""

from app.models.schemas import DroneDetectionBatch


MAX_TRUSTED_CLOCK_SKEW_S = 300.0

STICKY_BATCH_FIELDS = (
    "firmware_version", "board_type", "firmware_target", "app_project",
    "hardware_type", "hardware_mac", "capabilities", "node_name",
    "scanners", "time_sync", "reporting", "scan_mode", "scan_profile",
    "calibration_uuid", "dedup_seen", "dedup_sent", "dedup_collapsed",
    "cal_seen", "cal_sent", "wifi_ssid", "wifi_rssi", "led_state",
    "upload_queue", "upload",
)


def bounded_observation_time(
    batch_timestamp: int | None,
    server_received_at: float,
) -> tuple[float, float | None]:
    """Accept valid historical scan time without allowing future liveness."""
    if batch_timestamp is None:
        return server_received_at, None
    skew = float(batch_timestamp) - server_received_at
    if batch_timestamp <= 1_700_000_000 or skew > MAX_TRUSTED_CLOCK_SKEW_S:
        return server_received_at, skew
    return float(batch_timestamp), skew


def bounded_detection_time(
    detection_timestamp_ms: int | None,
    batch_observed_at: float,
    server_received_at: float,
) -> float:
    """Use valid scanner epoch time, otherwise the validated batch time."""
    if (
        detection_timestamp_ms is None
        or detection_timestamp_ms <= 1_700_000_000_000
    ):
        return batch_observed_at
    observed = detection_timestamp_ms / 1000.0
    if observed > server_received_at + MAX_TRUSTED_CLOCK_SKEW_S:
        return batch_observed_at
    return observed


def merge_backend_heartbeat(
    previous: dict,
    batch: DroneDetectionBatch,
    source_ip: str | None,
    server_received_at: float,
) -> dict:
    """Merge live telemetry while retaining the operational uplink identity."""
    merged = dict(previous)
    merged.update({
        "device_id": batch.device_id,
        "last_seen": server_received_at,
        "detection_count": len(batch.detections),
        "total_batches": int(previous.get("total_batches", 0)) + 1,
        "total_detections": (
            int(previous.get("total_detections", 0)) + len(batch.detections)
        ),
        "ip": source_ip,
    })
    for key, value in (
        ("lat", batch.device_lat),
        ("lon", batch.device_lon),
        ("alt", batch.device_alt),
    ):
        if value is not None:
            merged[key] = value
    _, skew = bounded_observation_time(batch.timestamp, server_received_at)
    merged["clock_skew_s"] = skew
    for field in STICKY_BATCH_FIELDS:
        value = getattr(batch, field)
        if value is not None:
            merged[field] = value
    return merged
