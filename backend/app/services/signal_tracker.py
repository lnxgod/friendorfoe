"""Low-latency RSSI tracking for WiFi and BLE detections.

This service is tuned for sub-second responsiveness:
- median-of-3 prefilter to blunt one-off RSSI spikes
- time-aware EMA smoothing for stable yet responsive signal traces
- short-window linear regression for dRSSI/dt estimation
- log-distance distance estimate with uncertainty bands
- cautious WiFi/BLE correlation that only hard-merges on strong identity cues
"""

from __future__ import annotations

import math
from collections import deque
from dataclasses import dataclass, field
from typing import Any


def _normalize_mac(mac: str | None) -> str | None:
    if not mac:
        return None
    hex_only = "".join(ch for ch in mac if ch.isalnum()).upper()
    if len(hex_only) != 12:
        return mac.upper()
    return ":".join(hex_only[i:i + 2] for i in range(0, 12, 2))


def _normalize_text(value: str | None) -> str | None:
    if value is None:
        return None
    normalized = value.strip()
    return normalized or None


def _family(source: str) -> str:
    source_l = source.lower()
    if source_l.startswith("wifi"):
        return "wifi"
    if source_l.startswith("ble"):
        return "ble"
    return source_l


def _looks_generated_drone_id(drone_id: str | None) -> bool:
    if not drone_id:
        return True
    lowered = drone_id.lower()
    return (
        lowered.startswith("wifi_")
        or lowered.startswith("ble_")
        or lowered.startswith("drone_")
        or ":" in drone_id
    )


def _median(values: list[float]) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    mid = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[mid]
    return (ordered[mid - 1] + ordered[mid]) / 2.0


def _linear_regression_slope(points: list[tuple[float, float]]) -> float:
    if len(points) < 2:
        return 0.0
    t0 = points[0][0]
    centered = [(t - t0, y) for t, y in points]
    mean_t = sum(t for t, _ in centered) / len(centered)
    mean_y = sum(y for _, y in centered) / len(centered)
    denom = sum((t - mean_t) ** 2 for t, _ in centered)
    if denom <= 1e-9:
        return 0.0
    numer = sum((t - mean_t) * (y - mean_y) for t, y in centered)
    return numer / denom


def _clamp(value: float, low: float, high: float) -> float:
    return max(low, min(value, high))


@dataclass(slots=True)
class SignalTrackerConfig:
    history_size: int = 64
    prefilter_size: int = 3
    ema_tau_s: float = 0.45
    min_sample_dt_s: float = 0.05
    stale_track_s: float = 15.0
    regression_window_s: float = 1.5
    min_regression_points: int = 4
    min_rssi_slope_dbps: float = 0.75
    reference_rssi_dbm: float = -45.0
    path_loss_exponent: float = 2.6
    path_loss_low: float = 2.0
    path_loss_high: float = 3.2
    distance_min_m: float = 0.5
    distance_max_m: float = 5000.0
    max_correlation_gap_s: float = 2.0
    max_correlation_rssi_delta_db: float = 8.0
    max_reasonable_speed_mps: float = 80.0
    recommended_push_interval_ms: int = 250
    recommended_poll_interval_ms: int = 500


@dataclass(slots=True)
class SignalEvent:
    drone_id: str
    source: str
    confidence: float
    rssi: int | None
    ssid: str | None
    bssid: str | None
    manufacturer: str | None
    device_id: str
    received_at: float
    channel: int | None = None

    @classmethod
    def from_mapping(cls, payload: dict[str, Any]) -> "SignalEvent":
        raw_rssi = payload.get("rssi")
        raw_channel = payload.get("channel")
        return cls(
            drone_id=str(payload.get("drone_id", "")),
            source=str(payload.get("source", "")),
            confidence=float(payload.get("confidence", 0.0) or 0.0),
            rssi=int(raw_rssi) if raw_rssi is not None else None,
            ssid=payload.get("ssid"),
            bssid=payload.get("bssid"),
            manufacturer=payload.get("manufacturer"),
            device_id=str(payload.get("device_id", "")),
            received_at=float(payload.get("received_at", 0.0) or 0.0),
            channel=int(raw_channel) if raw_channel is not None else None,
        )

    @property
    def normalized_bssid(self) -> str | None:
        return _normalize_mac(self.bssid)

    @property
    def normalized_ssid(self) -> str | None:
        return _normalize_text(self.ssid)

    @property
    def normalized_drone_id(self) -> str | None:
        drone_id = _normalize_text(self.drone_id)
        return drone_id.upper() if drone_id else None

    @property
    def normalized_manufacturer(self) -> str | None:
        manufacturer = _normalize_text(self.manufacturer)
        return manufacturer.lower() if manufacturer else None

    @property
    def source_family(self) -> str:
        return _family(self.source)


@dataclass(slots=True)
class SignalSample:
    timestamp: float
    raw_rssi: int
    filtered_rssi: float
    smoothed_rssi: float
    source: str
    sensor_id: str


@dataclass(slots=True)
class SignalTrack:
    track_id: str
    first_seen: float
    last_seen: float
    aliases: set[str] = field(default_factory=set)
    drone_ids: set[str] = field(default_factory=set)
    ssids: set[str] = field(default_factory=set)
    bssids: set[str] = field(default_factory=set)
    manufacturers: set[str] = field(default_factory=set)
    sources: set[str] = field(default_factory=set)
    sensors: set[str] = field(default_factory=set)
    confidences: deque[float] = field(default_factory=lambda: deque(maxlen=16))
    prefilter: deque[int] = field(default_factory=lambda: deque(maxlen=3))
    history: deque[SignalSample] = field(default_factory=lambda: deque(maxlen=64))
    last_channel: int | None = None
    last_match_strength: str = "new"
    last_match_score: float = 0.0

    def configure(self, config: SignalTrackerConfig) -> None:
        if self.prefilter.maxlen != config.prefilter_size:
            self.prefilter = deque(self.prefilter, maxlen=config.prefilter_size)
        if self.history.maxlen != config.history_size:
            self.history = deque(self.history, maxlen=config.history_size)


class SignalTracker:
    """Tracks live RSSI-derived signal motion for a single backend instance."""

    def __init__(self, config: SignalTrackerConfig | None = None):
        self.config = config or SignalTrackerConfig()
        self.tracks: dict[str, SignalTrack] = {}
        self.alias_to_track: dict[str, str] = {}
        self._next_track_number = 1

    def ingest(self, payload: SignalEvent | dict[str, Any]) -> dict[str, Any] | None:
        event = payload if isinstance(payload, SignalEvent) else SignalEvent.from_mapping(payload)
        if event.rssi is None:
            return None

        self._prune(event.received_at)
        track, match_strength, match_score = self._resolve_track(event)
        self._update_track(track, event, match_strength, match_score)
        return self._serialize_track(track, now=event.received_at)

    def get_live_tracks(
        self,
        limit: int = 100,
        active_within_s: float = 5.0,
        now: float | None = None,
    ) -> dict[str, Any]:
        now = now or max((track.last_seen for track in self.tracks.values()), default=0.0)
        tracks = [
            self._serialize_track(track, now=now)
            for track in self.tracks.values()
            if (now - track.last_seen) <= active_within_s
        ]
        tracks.sort(key=lambda item: (item["idle_s"], -item["confidence"], item["track_id"]))
        tracks = tracks[:limit]
        return {
            "count": len(tracks),
            "history_size": self.config.history_size,
            "recommended_push_interval_ms": self.config.recommended_push_interval_ms,
            "recommended_poll_interval_ms": self.config.recommended_poll_interval_ms,
            "tracks": tracks,
        }

    def _prune(self, now: float) -> None:
        stale_ids = [
            track_id
            for track_id, track in self.tracks.items()
            if (now - track.last_seen) > self.config.stale_track_s
        ]
        if not stale_ids:
            return

        for track_id in stale_ids:
            track = self.tracks.pop(track_id, None)
            if not track:
                continue
            for alias in track.aliases:
                if self.alias_to_track.get(alias) == track_id:
                    del self.alias_to_track[alias]

    def _resolve_track(self, event: SignalEvent) -> tuple[SignalTrack, str, float]:
        strong_aliases = self._strong_aliases(event)
        weak_aliases = self._weak_aliases(event)

        for alias in strong_aliases:
            track_id = self.alias_to_track.get(alias)
            if track_id and track_id in self.tracks:
                return self.tracks[track_id], "strong", 1.0

        best_track: SignalTrack | None = None
        best_score = 0.0
        for track in self.tracks.values():
            score = self._correlation_score(track, event)
            if score > best_score:
                best_track = track
                best_score = score

        if best_track and best_score >= 0.7:
            return best_track, "medium", best_score

        track = SignalTrack(
            track_id=f"sig-{self._next_track_number:06d}",
            first_seen=event.received_at,
            last_seen=event.received_at,
        )
        self._next_track_number += 1
        track.configure(self.config)
        return track, "new", 0.0

    def _strong_aliases(self, event: SignalEvent) -> set[str]:
        aliases: set[str] = set()
        if event.normalized_bssid:
            aliases.add(f"mac:{event.normalized_bssid}")
        if event.normalized_drone_id and not _looks_generated_drone_id(event.normalized_drone_id):
            aliases.add(f"drone:{event.normalized_drone_id}")
        return aliases

    def _weak_aliases(self, event: SignalEvent) -> set[str]:
        aliases: set[str] = set()
        if event.normalized_ssid:
            aliases.add(f"ssid:{event.normalized_ssid.lower()}")
        manufacturer = event.normalized_manufacturer
        if event.normalized_drone_id and manufacturer:
            aliases.add(f"mfrdrone:{manufacturer}:{event.normalized_drone_id}")
        return aliases

    def _correlation_score(self, track: SignalTrack, event: SignalEvent) -> float:
        if not track.history:
            return 0.0

        latest = track.history[-1]
        age = event.received_at - track.last_seen
        if age < 0 or age > self.config.max_correlation_gap_s:
            return 0.0

        score = 0.0
        if event.device_id in track.sensors:
            score += 0.25
        if event.normalized_manufacturer and event.normalized_manufacturer in track.manufacturers:
            score += 0.20
        if event.normalized_ssid and event.normalized_ssid.lower() in {ssid.lower() for ssid in track.ssids}:
            score += 0.25
        if event.normalized_drone_id and event.normalized_drone_id in track.drone_ids:
            score += 0.35
        if event.rssi is not None and abs(event.rssi - latest.raw_rssi) <= self.config.max_correlation_rssi_delta_db:
            score += 0.20
        if event.source_family not in {_family(source) for source in track.sources}:
            score += 0.10
        return min(score, 0.95)

    def _update_track(
        self,
        track: SignalTrack,
        event: SignalEvent,
        match_strength: str,
        match_score: float,
    ) -> None:
        if track.track_id not in self.tracks:
            self.tracks[track.track_id] = track

        track.configure(self.config)
        track.last_seen = event.received_at
        track.sources.add(event.source)
        track.sensors.add(event.device_id)
        track.last_channel = event.channel
        track.last_match_strength = match_strength
        track.last_match_score = match_score
        track.confidences.append(event.confidence)

        if event.normalized_drone_id:
            track.drone_ids.add(event.normalized_drone_id)
        if event.normalized_ssid:
            track.ssids.add(event.normalized_ssid)
        if event.normalized_bssid:
            track.bssids.add(event.normalized_bssid)
        if event.normalized_manufacturer:
            track.manufacturers.add(event.normalized_manufacturer)

        aliases = self._strong_aliases(event) | self._weak_aliases(event)
        track.aliases.update(aliases)
        for alias in aliases:
            self.alias_to_track[alias] = track.track_id

        track.prefilter.append(event.rssi or 0)
        filtered_rssi = _median(list(track.prefilter))

        previous = track.history[-1].smoothed_rssi if track.history else filtered_rssi
        if track.history:
            dt = max(event.received_at - track.history[-1].timestamp, self.config.min_sample_dt_s)
            alpha = 1.0 - math.exp(-dt / self.config.ema_tau_s)
        else:
            alpha = 1.0
        smoothed_rssi = previous + alpha * (filtered_rssi - previous)

        track.history.append(
            SignalSample(
                timestamp=event.received_at,
                raw_rssi=event.rssi or 0,
                filtered_rssi=filtered_rssi,
                smoothed_rssi=smoothed_rssi,
                source=event.source,
                sensor_id=event.device_id,
            )
        )

    def _serialize_track(self, track: SignalTrack, now: float) -> dict[str, Any]:
        latest = track.history[-1]
        slope_dbps = self._rssi_slope(track)
        slope_dbps = 0.0 if abs(slope_dbps) < self.config.min_rssi_slope_dbps else slope_dbps

        distance_m = self._rssi_to_distance_m(latest.smoothed_rssi, self.config.path_loss_exponent)
        distance_low_m, distance_high_m = self._distance_band(latest.smoothed_rssi)
        approach_speed_mps = self._approach_speed_mps(distance_m, slope_dbps)

        history = [
            {
                "timestamp": sample.timestamp,
                "raw_rssi": sample.raw_rssi,
                "smoothed_rssi": round(sample.smoothed_rssi, 2),
            }
            for sample in track.history
        ]

        display_name = self._display_name(track)
        confidence = max(track.confidences) if track.confidences else 0.0
        return {
            "track_id": track.track_id,
            "display_name": display_name,
            "drone_ids": sorted(track.drone_ids),
            "ssids": sorted(track.ssids),
            "bssids": sorted(track.bssids),
            "manufacturers": sorted(track.manufacturers),
            "sources": sorted(track.sources),
            "sensors": sorted(track.sensors),
            "confidence": round(confidence, 3),
            "first_seen": track.first_seen,
            "last_seen": track.last_seen,
            "age_s": round(now - track.first_seen, 3),
            "idle_s": round(now - track.last_seen, 3),
            "sample_count": len(track.history),
            "raw_rssi": latest.raw_rssi,
            "filtered_rssi": round(latest.filtered_rssi, 2),
            "smoothed_rssi": round(latest.smoothed_rssi, 2),
            "rssi_slope_dbps": round(slope_dbps, 3),
            "distance_m": round(distance_m, 2),
            "distance_low_m": round(distance_low_m, 2),
            "distance_high_m": round(distance_high_m, 2),
            "approach_speed_mps": round(approach_speed_mps, 3),
            "match_strength": track.last_match_strength,
            "match_score": round(track.last_match_score, 3),
            "history": history,
        }

    def _display_name(self, track: SignalTrack) -> str:
        if track.drone_ids:
            preferred = sorted(track.drone_ids, key=lambda value: (_looks_generated_drone_id(value), value))[0]
            return preferred
        if track.ssids:
            return sorted(track.ssids)[0]
        if track.bssids:
            return sorted(track.bssids)[0]
        return track.track_id

    def _recent_points(self, track: SignalTrack) -> list[tuple[float, float]]:
        if not track.history:
            return []
        cutoff = track.history[-1].timestamp - self.config.regression_window_s
        return [
            (sample.timestamp, sample.smoothed_rssi)
            for sample in track.history
            if sample.timestamp >= cutoff
        ]

    def _rssi_slope(self, track: SignalTrack) -> float:
        points = self._recent_points(track)
        if len(points) < self.config.min_regression_points:
            return 0.0
        return _linear_regression_slope(points)

    def _rssi_to_distance_m(self, rssi: float, path_loss_exponent: float) -> float:
        exponent = (self.config.reference_rssi_dbm - rssi) / (10.0 * path_loss_exponent)
        return _clamp(10.0 ** exponent, self.config.distance_min_m, self.config.distance_max_m)

    def _distance_band(self, rssi: float) -> tuple[float, float]:
        d_low = self._rssi_to_distance_m(rssi, self.config.path_loss_high)
        d_high = self._rssi_to_distance_m(rssi, self.config.path_loss_low)
        return (min(d_low, d_high), max(d_low, d_high))

    def _approach_speed_mps(self, distance_m: float, slope_dbps: float) -> float:
        # If RSSI is rising, the object is approaching and the value is positive.
        speed = (math.log(10.0) / (10.0 * self.config.path_loss_exponent)) * distance_m * slope_dbps
        return _clamp(speed, -self.config.max_reasonable_speed_mps, self.config.max_reasonable_speed_mps)
