"""In-memory RF anomaly detection for WiFi/BLE sensor streams.

The detector is designed for a small single-server deployment:
- O(1) / O(window) per event using dicts and short deques
- no database dependency
- explicit cooldowns to avoid alert storms

State held in memory:
- ``device_states``: per-entity rolling RSSI/channel/history state
- ``bssid_sightings``: first-seen and confirmation windows for new devices
- ``ssid_to_bssids``: recent SSID -> BSSID index for spoofing checks
- ``identity_to_bssids``: recent identity -> BSSID index for MAC churn checks
- ``type_stats``: streaming RSSI baseline by manufacturer/source
- ``correlation_cache``: recent WiFi/BLE sightings for cross-tech correlation
- ``recent_alerts``: ring buffer of emitted anomaly alerts
"""

from __future__ import annotations

from collections import Counter, defaultdict, deque
from dataclasses import dataclass, field
from datetime import datetime
from math import sqrt
from typing import Any


def _normalize_mac(mac: str | None) -> str | None:
    if not mac:
        return None
    hex_only = "".join(ch for ch in mac if ch.isalnum()).upper()
    if len(hex_only) != 12:
        return mac.upper()
    return ":".join(hex_only[i:i + 2] for i in range(0, 12, 2))


def _normalize_ssid(ssid: str | None) -> str | None:
    if ssid is None:
        return None
    normalized = ssid.strip()
    return normalized or None


def _family(source: str) -> str:
    source_l = source.lower()
    if source_l.startswith("wifi"):
        return "wifi"
    if source_l.startswith("ble"):
        return "ble"
    return source_l


def _mac_oui(mac: str | None) -> str | None:
    normalized = _normalize_mac(mac)
    return normalized[:8] if normalized else None


def _is_locally_administered(mac: str | None) -> bool:
    normalized = _normalize_mac(mac)
    if not normalized:
        return False
    try:
        first_octet = int(normalized[:2], 16)
    except ValueError:
        return False
    return bool(first_octet & 0x02)


def _estimate_distance_m(rssi: int, reference_rssi: float, path_loss_exponent: float) -> float:
    return 10 ** ((reference_rssi - rssi) / (10 * path_loss_exponent))


@dataclass(slots=True)
class RFDetectionEvent:
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
    def from_mapping(cls, payload: dict[str, Any]) -> "RFDetectionEvent":
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
        return _normalize_ssid(self.ssid)

    @property
    def source_family(self) -> str:
        return _family(self.source)


@dataclass(slots=True)
class RFAnomalyAlert:
    anomaly_type: str
    severity: str
    entity_key: str
    title: str
    message: str
    detected_at: float
    device_id: str
    source: str
    drone_id: str | None = None
    ssid: str | None = None
    bssid: str | None = None
    manufacturer: str | None = None
    metadata: dict[str, Any] = field(default_factory=dict)


@dataclass(slots=True)
class RunningStats:
    count: int = 0
    mean: float = 0.0
    m2: float = 0.0

    def update(self, value: float) -> None:
        self.count += 1
        delta = value - self.mean
        self.mean += delta / self.count
        delta2 = value - self.mean
        self.m2 += delta * delta2

    @property
    def variance(self) -> float:
        if self.count < 2:
            return 0.0
        return self.m2 / (self.count - 1)

    @property
    def stddev(self) -> float:
        return sqrt(self.variance)


@dataclass(slots=True)
class DeviceState:
    first_seen: float
    last_seen: float
    total_seen: int = 0
    max_confidence: float = 0.0
    sensors: set[str] = field(default_factory=set)
    sources: set[str] = field(default_factory=set)
    ssids: set[str] = field(default_factory=set)
    bssids: set[str] = field(default_factory=set)
    manufacturers: set[str] = field(default_factory=set)
    rssi_by_sensor: dict[str, deque[tuple[float, int]]] = field(
        default_factory=lambda: defaultdict(deque)
    )
    channel_by_sensor: dict[str, deque[tuple[float, int]]] = field(
        default_factory=lambda: defaultdict(deque)
    )
    hour_counts: Counter[int] = field(default_factory=Counter)
    seen_days: set[int] = field(default_factory=set)
    disappearance_reported: bool = False


@dataclass(slots=True)
class RFAnomalyConfig:
    min_confidence: float = 0.45
    recent_alert_limit: int = 500
    default_cooldown_s: float = 60.0
    state_ttl_s: float = 7 * 24 * 3600.0
    recent_window_s: float = 180.0

    rssi_spike_db: int = 25           # 12 was residential noise
    rssi_spike_window_s: float = 15.0
    rssi_spike_min_samples: int = 4   # Need more samples before alerting

    disappearance_min_seen: int = 10  # Must be seen 10+ times to matter
    disappearance_min_presence_s: float = 300.0  # 5 min presence before we care
    disappearance_grace_s: float = 300.0         # 5 min grace before alerting

    new_device_confirmations: int = 3  # Need 3 confirmations, not 2
    new_device_window_s: float = 30.0

    reference_rssi_dbm: float = -45.0
    path_loss_exponent: float = 2.2
    velocity_window_s: float = 20.0
    velocity_min_window_s: float = 15.0  # require span before trusting slope
    velocity_min_samples: int = 5        # 3 was vulnerable to multipath noise
    velocity_min_speed_mps: float = 8.0
    velocity_min_rssi_delta_db: int = 18  # 12 still tripped on noise
    velocity_cooldown_s: float = 600.0    # don't re-alert the same track for 10 min
    rssi_spike_cooldown_s: float = 600.0

    channel_hop_window_s: float = 10.0
    channel_hop_distinct_channels: int = 3

    signal_baseline_min_samples: int = 20
    signal_min_abs_delta_db: int = 15
    signal_zscore_threshold: float = 2.8

    spoofing_window_s: float = 120.0
    spoofing_distinct_bssids: int = 6

    time_pattern_min_hits: int = 8
    time_pattern_min_days: int = 3
    time_pattern_window_hours: int = 2
    time_pattern_fraction: float = 0.8

    correlation_window_s: float = 8.0


class RFAnomalyDetector:
    """Process RF detection events and emit structured anomaly alerts."""

    def __init__(self, config: RFAnomalyConfig | None = None):
        self.config = config or RFAnomalyConfig()
        self.device_states: dict[str, DeviceState] = {}
        self.bssid_first_seen: dict[str, float] = {}
        self.bssid_sightings: dict[str, deque[float]] = defaultdict(deque)
        self.alerted_new_bssids: set[str] = set()
        self.ssid_to_bssids: dict[str, dict[str, float]] = defaultdict(dict)
        self.identity_to_bssids: dict[str, dict[str, float]] = defaultdict(dict)
        self.type_stats: dict[str, RunningStats] = defaultdict(RunningStats)
        self.correlation_cache: dict[str, dict[str, deque[tuple[float, str, int | None, str]]]] = defaultdict(
            lambda: {"wifi": deque(), "ble": deque()}
        )
        self.alert_cooldowns: dict[tuple[str, str], float] = {}
        self.recent_alerts: deque[RFAnomalyAlert] = deque(maxlen=self.config.recent_alert_limit)

    def process_event(self, payload: RFDetectionEvent | dict[str, Any]) -> list[RFAnomalyAlert]:
        event = payload if isinstance(payload, RFDetectionEvent) else RFDetectionEvent.from_mapping(payload)
        if event.received_at <= 0:
            raise ValueError("received_at must be an epoch timestamp")

        self._prune(event.received_at)
        alerts = self.sweep(event.received_at)

        entity_key = self._entity_key(event)
        state = self.device_states.get(entity_key)
        if state is None:
            state = DeviceState(first_seen=event.received_at, last_seen=event.received_at)
            self.device_states[entity_key] = state

        alerts.extend(self._detect_rssi_spike(event, entity_key, state))
        alerts.extend(self._detect_velocity(event, entity_key, state))
        alerts.extend(self._detect_signal_strength(event, entity_key))

        self._update_state(event, state)

        alerts.extend(self._detect_new_device(event, entity_key))
        alerts.extend(self._detect_channel_hopping(event, entity_key, state))
        alerts.extend(self._detect_spoofing(event, entity_key))
        alerts.extend(self._detect_time_pattern(event, entity_key, state))
        alerts.extend(self._detect_ble_correlation(event, entity_key))
        return alerts

    def sweep(self, now: float | None = None) -> list[RFAnomalyAlert]:
        now = now or datetime.now().timestamp()
        alerts: list[RFAnomalyAlert] = []

        # Entity-gate import — the EntityTracker singleton lives in the
        # router module. If the lookup fails (e.g. running outside FastAPI
        # in a test), fall back to legacy per-bssid sweep. Otherwise only
        # emit when the WHOLE entity is gone, not when this bssid is gone.
        _entity_tracker = None
        try:
            from app.routers.detections import _entity_tracker as _et
            _entity_tracker = _et
        except Exception:
            _entity_tracker = None

        for entity_key, state in list(self.device_states.items()):
            if state.disappearance_reported:
                continue
            if state.total_seen < self.config.disappearance_min_seen:
                continue
            if (state.last_seen - state.first_seen) < self.config.disappearance_min_presence_s:
                continue
            age = now - state.last_seen
            if age < self.config.disappearance_grace_s:
                continue

            # Gate: if the entity this bssid belongs to still has any
            # active sensor, SKIP. The entity-level EventDetector will
            # emit the authoritative device_departed event when the
            # whole entity goes silent.
            if _entity_tracker is not None and entity_key.startswith("bssid:"):
                bssid = entity_key[len("bssid:"):]
                eid = _entity_tracker.get_entity_id(bssid)
                if eid:
                    ent = _entity_tracker.get_entity(eid)
                    if ent and ent.is_active:
                        continue  # entity still has a live sensor; suppress

            alert = self._emit_alert(
                anomaly_type="device_disappearance",
                entity_key=entity_key,
                detected_at=now,
                device_id=next(iter(state.sensors), ""),
                source="mixed",
                title="Tracked device disappeared",
                message=(
                    f"Device went silent for {age:.0f}s after "
                    f"{state.total_seen} sightings across {len(state.sensors)} sensor(s)."
                ),
                severity="medium",
                metadata={
                    "age_s": round(age, 1),
                    "total_seen": state.total_seen,
                    "sensor_count": len(state.sensors),
                },
                cooldown_s=self.config.disappearance_grace_s,
            )
            if alert:
                alerts.append(alert)
                state.disappearance_reported = True

        return alerts

    def get_recent_alerts(self, limit: int = 100) -> list[RFAnomalyAlert]:
        items = list(self.recent_alerts)[-limit:]
        items.reverse()
        return items

    def _entity_key(self, event: RFDetectionEvent) -> str:
        if event.normalized_bssid:
            return f"bssid:{event.normalized_bssid}"
        if event.drone_id:
            return f"drone:{event.drone_id}"
        if event.normalized_ssid:
            return f"ssid:{event.normalized_ssid.lower()}"
        return f"{event.source_family}:{event.device_id}"

    def _identity_key(self, event: RFDetectionEvent) -> str | None:
        if event.drone_id:
            return f"drone:{event.drone_id}"
        if event.manufacturer and event.normalized_ssid:
            return f"maker-ssid:{event.manufacturer.lower()}:{event.normalized_ssid.lower()}"
        if event.normalized_ssid:
            return f"ssid:{event.normalized_ssid.lower()}"
        return None

    def _type_key(self, event: RFDetectionEvent) -> str:
        if event.manufacturer:
            return f"manufacturer:{event.manufacturer.lower()}"
        return f"source:{event.source_family}"

    def _correlation_key(self, event: RFDetectionEvent) -> str | None:
        if event.drone_id:
            return f"drone:{event.drone_id}"
        if event.manufacturer and event.normalized_ssid:
            return f"maker-ssid:{event.manufacturer.lower()}:{event.normalized_ssid.lower()}"
        return None

    def _update_state(self, event: RFDetectionEvent, state: DeviceState) -> None:
        now = event.received_at
        state.last_seen = now
        state.total_seen += 1
        state.max_confidence = max(state.max_confidence, event.confidence)
        state.sensors.add(event.device_id)
        state.sources.add(event.source_family)
        state.disappearance_reported = False

        if event.normalized_ssid:
            state.ssids.add(event.normalized_ssid)
        if event.normalized_bssid:
            state.bssids.add(event.normalized_bssid)
        if event.manufacturer:
            state.manufacturers.add(event.manufacturer)

        if event.rssi is not None:
            rssi_history = state.rssi_by_sensor[event.device_id]
            rssi_history.append((now, event.rssi))
            self._trim_deque(rssi_history, now, self.config.recent_window_s)
            self.type_stats[self._type_key(event)].update(event.rssi)

        if event.channel is not None:
            channel_history = state.channel_by_sensor[event.device_id]
            channel_history.append((now, event.channel))
            self._trim_deque(channel_history, now, self.config.channel_hop_window_s)

        if event.normalized_bssid:
            if event.normalized_bssid not in self.bssid_first_seen:
                self.bssid_first_seen[event.normalized_bssid] = now
            self.bssid_sightings[event.normalized_bssid].append(now)
            self._trim_deque(
                self.bssid_sightings[event.normalized_bssid],
                now,
                self.config.new_device_window_s,
            )

            if event.normalized_ssid:
                self.ssid_to_bssids[event.normalized_ssid.lower()][event.normalized_bssid] = now

            identity_key = self._identity_key(event)
            if identity_key:
                self.identity_to_bssids[identity_key][event.normalized_bssid] = now

        correlation_key = self._correlation_key(event)
        if correlation_key:
            cache = self.correlation_cache[correlation_key][event.source_family]
            cache.append((now, event.device_id, event.rssi, event.source))
            self._trim_deque(cache, now, self.config.correlation_window_s)

        local_time = datetime.fromtimestamp(now)
        state.hour_counts[local_time.hour] += 1
        state.seen_days.add(local_time.date().toordinal())

    def _detect_rssi_spike(
        self,
        event: RFDetectionEvent,
        entity_key: str,
        state: DeviceState,
    ) -> list[RFAnomalyAlert]:
        if event.rssi is None or event.confidence < self.config.min_confidence:
            return []

        history = state.rssi_by_sensor.get(event.device_id)
        if not history or len(history) < self.config.rssi_spike_min_samples:
            return []

        self._trim_deque(history, event.received_at, self.config.rssi_spike_window_s)
        if len(history) < self.config.rssi_spike_min_samples:
            return []

        previous_rssi = history[-1][1]
        baseline_rssi = sum(value for _, value in history) / len(history)
        delta = event.rssi - previous_rssi
        baseline_delta = event.rssi - baseline_rssi
        if abs(delta) < self.config.rssi_spike_db and abs(baseline_delta) < self.config.rssi_spike_db:
            return []

        direction = "stronger" if delta > 0 else "weaker"
        alert = self._emit_alert(
            anomaly_type="rssi_spike",
            entity_key=f"{entity_key}:{event.device_id}",
            detected_at=event.received_at,
            device_id=event.device_id,
            source=event.source,
            title="RSSI spike detected",
            message=(
                f"RSSI became {direction} by {abs(delta):.1f} dB "
                f"({previous_rssi} -> {event.rssi}) on {event.device_id}."
            ),
            severity="high" if delta > 0 else "medium",
            drone_id=event.drone_id,
            ssid=event.normalized_ssid,
            bssid=event.normalized_bssid,
            manufacturer=event.manufacturer,
            metadata={
                "delta_db": round(delta, 1),
                "baseline_delta_db": round(baseline_delta, 1),
                "previous_rssi": previous_rssi,
                "current_rssi": event.rssi,
            },
            cooldown_s=self.config.rssi_spike_cooldown_s,
        )
        return [alert] if alert else []

    def _detect_velocity(
        self,
        event: RFDetectionEvent,
        entity_key: str,
        state: DeviceState,
    ) -> list[RFAnomalyAlert]:
        if event.rssi is None or event.confidence < self.config.min_confidence:
            return []

        history = state.rssi_by_sensor.get(event.device_id)
        if not history or len(history) < self.config.velocity_min_samples - 1:
            return []

        self._trim_deque(history, event.received_at, self.config.velocity_window_s)
        # Include the current reading in the sample-count check.
        if len(history) + 1 < self.config.velocity_min_samples:
            return []

        # Require the RSSI trend to be monotonic across ALL recent samples, not
        # just the last three — three ascending readings can easily be noise
        # (multipath), but a sustained monotonic trajectory over 5+ samples
        # spanning >= velocity_min_window_s is much stronger evidence of motion.
        all_points = list(history) + [(event.received_at, event.rssi)]
        values = [v for _, v in all_points]
        non_decreasing = all(values[i] <= values[i + 1] for i in range(len(values) - 1))
        non_increasing = all(values[i] >= values[i + 1] for i in range(len(values) - 1))
        if not (non_decreasing or non_increasing):
            return []

        oldest_ts, oldest_rssi = history[0]
        dt = event.received_at - oldest_ts
        if dt < self.config.velocity_min_window_s:
            return []

        rssi_delta = event.rssi - oldest_rssi
        if abs(rssi_delta) < self.config.velocity_min_rssi_delta_db:
            return []

        previous_distance = _estimate_distance_m(
            oldest_rssi,
            self.config.reference_rssi_dbm,
            self.config.path_loss_exponent,
        )
        current_distance = _estimate_distance_m(
            event.rssi,
            self.config.reference_rssi_dbm,
            self.config.path_loss_exponent,
        )
        radial_speed = (previous_distance - current_distance) / dt
        if abs(radial_speed) < self.config.velocity_min_speed_mps:
            return []

        direction = "approaching" if radial_speed > 0 else "departing"
        alert = self._emit_alert(
            anomaly_type="rssi_velocity",
            entity_key=f"{entity_key}:{event.device_id}",
            detected_at=event.received_at,
            device_id=event.device_id,
            source=event.source,
            title="Rapid RF movement estimate",
            message=(
                f"Estimated {direction} speed is {abs(radial_speed):.1f} m/s "
                f"from RSSI trend over {dt:.1f}s."
            ),
            severity="high" if radial_speed > 0 else "medium",
            drone_id=event.drone_id,
            ssid=event.normalized_ssid,
            bssid=event.normalized_bssid,
            manufacturer=event.manufacturer,
            metadata={
                "radial_speed_mps": round(radial_speed, 2),
                "previous_distance_m": round(previous_distance, 2),
                "current_distance_m": round(current_distance, 2),
                "window_s": round(dt, 2),
            },
            cooldown_s=self.config.velocity_cooldown_s,
        )
        return [alert] if alert else []

    def _detect_new_device(self, event: RFDetectionEvent, entity_key: str) -> list[RFAnomalyAlert]:
        bssid = event.normalized_bssid
        if not bssid or event.confidence < self.config.min_confidence:
            return []

        if _is_locally_administered(bssid) and not event.manufacturer:
            return []

        sightings = self.bssid_sightings[bssid]
        if bssid in self.alerted_new_bssids:
            return []
        if len(sightings) < self.config.new_device_confirmations:
            return []

        first_seen = self.bssid_first_seen.get(bssid, event.received_at)
        alert = self._emit_alert(
            anomaly_type="new_device",
            entity_key=entity_key,
            detected_at=event.received_at,
            device_id=event.device_id,
            source=event.source,
            title="New BSSID observed",
            message=(
                f"Never-before-seen BSSID {bssid} was confirmed "
                f"{len(sightings)} times in {event.received_at - first_seen:.1f}s."
            ),
            severity="medium",
            drone_id=event.drone_id,
            ssid=event.normalized_ssid,
            bssid=bssid,
            manufacturer=event.manufacturer,
            metadata={
                "confirmations": len(sightings),
                "first_seen": first_seen,
            },
            cooldown_s=3600.0,
        )
        if alert:
            self.alerted_new_bssids.add(bssid)
            return [alert]
        return []

    def _detect_channel_hopping(
        self,
        event: RFDetectionEvent,
        entity_key: str,
        state: DeviceState,
    ) -> list[RFAnomalyAlert]:
        if event.channel is None:
            return []

        history = state.channel_by_sensor.get(event.device_id)
        if not history:
            return []

        distinct_channels = sorted({channel for _, channel in history})
        if len(distinct_channels) < self.config.channel_hop_distinct_channels:
            return []

        span_s = history[-1][0] - history[0][0] if len(history) > 1 else 0.0
        alert = self._emit_alert(
            anomaly_type="channel_hopping",
            entity_key=f"{entity_key}:{event.device_id}",
            detected_at=event.received_at,
            device_id=event.device_id,
            source=event.source,
            title="Rapid channel hopping",
            message=(
                f"Observed on channels {distinct_channels} within {span_s:.1f}s "
                f"from sensor {event.device_id}."
            ),
            severity="high",
            drone_id=event.drone_id,
            ssid=event.normalized_ssid,
            bssid=event.normalized_bssid,
            manufacturer=event.manufacturer,
            metadata={
                "channels": distinct_channels,
                "window_s": round(span_s, 1),
            },
        )
        return [alert] if alert else []

    def _detect_signal_strength(
        self,
        event: RFDetectionEvent,
        entity_key: str,
    ) -> list[RFAnomalyAlert]:
        if event.rssi is None or event.confidence < self.config.min_confidence:
            return []

        stats = self.type_stats[self._type_key(event)]
        if stats.count < self.config.signal_baseline_min_samples or stats.stddev < 1.0:
            return []

        delta = event.rssi - stats.mean
        zscore = delta / stats.stddev
        if abs(delta) < self.config.signal_min_abs_delta_db:
            return []
        if abs(zscore) < self.config.signal_zscore_threshold:
            return []

        direction = "stronger" if delta > 0 else "weaker"
        alert = self._emit_alert(
            anomaly_type="signal_strength_outlier",
            entity_key=entity_key,
            detected_at=event.received_at,
            device_id=event.device_id,
            source=event.source,
            title="Signal strength anomaly",
            message=(
                f"RSSI {event.rssi} dBm is {abs(zscore):.1f} sigma {direction} "
                f"than the {self._type_key(event)} baseline."
            ),
            severity="high" if delta > 0 else "medium",
            drone_id=event.drone_id,
            ssid=event.normalized_ssid,
            bssid=event.normalized_bssid,
            manufacturer=event.manufacturer,
            metadata={
                "zscore": round(zscore, 2),
                "baseline_mean": round(stats.mean, 2),
                "baseline_stddev": round(stats.stddev, 2),
                "delta_db": round(delta, 1),
            },
        )
        return [alert] if alert else []

    def _detect_spoofing(self, event: RFDetectionEvent, entity_key: str) -> list[RFAnomalyAlert]:
        bssid = event.normalized_bssid
        ssid = event.normalized_ssid
        if not bssid:
            return []

        alerts: list[RFAnomalyAlert] = []

        if ssid:
            ssid_bssids = self.ssid_to_bssids[ssid.lower()]
            self._trim_mapping(ssid_bssids, event.received_at, self.config.spoofing_window_s)
            if len(ssid_bssids) >= self.config.spoofing_distinct_bssids:
                distinct_ouis = sorted({candidate[:8] for candidate in ssid_bssids})
                alert = self._emit_alert(
                    anomaly_type="ssid_spoofing",
                    entity_key=f"ssid:{ssid.lower()}",
                    detected_at=event.received_at,
                    device_id=event.device_id,
                    source=event.source,
                    title="SSID spoofing suspected",
                    message=(
                        f"SSID {ssid!r} mapped to {len(ssid_bssids)} BSSIDs in "
                        f"{self.config.spoofing_window_s:.0f}s."
                    ),
                    severity="medium",
                    drone_id=event.drone_id,
                    ssid=ssid,
                    bssid=bssid,
                    manufacturer=event.manufacturer,
                    metadata={
                        "distinct_bssids": sorted(ssid_bssids),
                        "distinct_ouis": distinct_ouis,
                    },
                )
                if alert:
                    alerts.append(alert)

        identity_key = self._identity_key(event)
        if identity_key:
            identity_bssids = self.identity_to_bssids[identity_key]
            self._trim_mapping(identity_bssids, event.received_at, self.config.spoofing_window_s)
            if len(identity_bssids) >= self.config.spoofing_distinct_bssids:
                ouis = {_mac_oui(candidate) for candidate in identity_bssids}
                randomized_count = sum(
                    1 for candidate in identity_bssids if _is_locally_administered(candidate)
                )
                if len(ouis) > 1 or randomized_count >= 2:
                    alert = self._emit_alert(
                        anomaly_type="bssid_churn",
                        entity_key=identity_key,
                        detected_at=event.received_at,
                        device_id=event.device_id,
                        source=event.source,
                        title="Identity MAC churn",
                        message=(
                            f"{identity_key} rotated across {len(identity_bssids)} "
                            f"BSSIDs in {self.config.spoofing_window_s:.0f}s."
                        ),
                        severity="high",
                        drone_id=event.drone_id,
                        ssid=ssid,
                        bssid=bssid,
                        manufacturer=event.manufacturer,
                        metadata={
                            "distinct_bssids": sorted(identity_bssids),
                            "distinct_ouis": sorted(oui_value for oui_value in ouis if oui_value),
                        },
                    )
                    if alert:
                        alerts.append(alert)

        return alerts

    def _detect_time_pattern(
        self,
        event: RFDetectionEvent,
        entity_key: str,
        state: DeviceState,
    ) -> list[RFAnomalyAlert]:
        total_hits = sum(state.hour_counts.values())
        if total_hits < self.config.time_pattern_min_hits:
            return []
        if len(state.seen_days) < self.config.time_pattern_min_days:
            return []

        best_start_hour = 0
        best_hits = 0
        window_hours = self.config.time_pattern_window_hours
        for start_hour in range(24):
            window_total = 0
            for offset in range(window_hours):
                window_total += state.hour_counts[(start_hour + offset) % 24]
            if window_total > best_hits:
                best_hits = window_total
                best_start_hour = start_hour

        fraction = best_hits / total_hits
        if fraction < self.config.time_pattern_fraction:
            return []

        best_window = [(best_start_hour + offset) % 24 for offset in range(window_hours)]
        local_hour = datetime.fromtimestamp(event.received_at).hour
        if local_hour not in best_window:
            return []

        window_label = f"{best_window[0]:02d}:00-{(best_window[-1] + 1) % 24:02d}:00"
        alert = self._emit_alert(
            anomaly_type="time_of_day_pattern",
            entity_key=entity_key,
            detected_at=event.received_at,
            device_id=event.device_id,
            source=event.source,
            title="Consistent time-of-day pattern",
            message=(
                f"Device appears mostly during {window_label}; "
                f"{fraction:.0%} of sightings fall in that window."
            ),
            severity="low",
            drone_id=event.drone_id,
            ssid=event.normalized_ssid,
            bssid=event.normalized_bssid,
            manufacturer=event.manufacturer,
            metadata={
                "window": best_window,
                "window_label": window_label,
                "fraction": round(fraction, 3),
                "distinct_days": len(state.seen_days),
            },
            cooldown_s=6 * 3600.0,
        )
        return [alert] if alert else []

    def _detect_ble_correlation(
        self,
        event: RFDetectionEvent,
        entity_key: str,
    ) -> list[RFAnomalyAlert]:
        correlation_key = self._correlation_key(event)
        if not correlation_key:
            return []

        cache = self.correlation_cache[correlation_key]
        opposite_family = "ble" if event.source_family == "wifi" else "wifi"
        matches = [
            (seen_at, sensor_id, rssi, source)
            for seen_at, sensor_id, rssi, source in cache[opposite_family]
            if sensor_id == event.device_id and abs(event.received_at - seen_at) <= self.config.correlation_window_s
        ]
        if not matches:
            return []

        closest = min(matches, key=lambda item: abs(event.received_at - item[0]))
        other_seen_at, _, other_rssi, other_source = closest
        alert = self._emit_alert(
            anomaly_type="wifi_ble_correlation",
            entity_key=entity_key,
            detected_at=event.received_at,
            device_id=event.device_id,
            source=event.source,
            title="WiFi/BLE correlation",
            message=(
                f"Matched {event.source_family} detection with {other_source} "
                f"within {abs(event.received_at - other_seen_at):.1f}s on {event.device_id}."
            ),
            severity="high",
            drone_id=event.drone_id,
            ssid=event.normalized_ssid,
            bssid=event.normalized_bssid,
            manufacturer=event.manufacturer,
            metadata={
                "other_source": other_source,
                "other_rssi": other_rssi,
                "delta_s": round(abs(event.received_at - other_seen_at), 2),
                "correlation_key": correlation_key,
            },
            cooldown_s=30.0,
        )
        return [alert] if alert else []

    def _emit_alert(
        self,
        *,
        anomaly_type: str,
        entity_key: str,
        detected_at: float,
        device_id: str,
        source: str,
        title: str,
        message: str,
        severity: str,
        drone_id: str | None = None,
        ssid: str | None = None,
        bssid: str | None = None,
        manufacturer: str | None = None,
        metadata: dict[str, Any] | None = None,
        cooldown_s: float | None = None,
    ) -> RFAnomalyAlert | None:
        cooldown_key = (anomaly_type, entity_key)
        last_seen = self.alert_cooldowns.get(cooldown_key)
        effective_cooldown = self.config.default_cooldown_s if cooldown_s is None else cooldown_s
        if last_seen is not None and detected_at - last_seen < effective_cooldown:
            return None

        alert = RFAnomalyAlert(
            anomaly_type=anomaly_type,
            severity=severity,
            entity_key=entity_key,
            title=title,
            message=message,
            detected_at=detected_at,
            device_id=device_id,
            source=source,
            drone_id=drone_id or None,
            ssid=ssid,
            bssid=bssid,
            manufacturer=manufacturer,
            metadata=metadata or {},
        )
        self.alert_cooldowns[cooldown_key] = detected_at
        self.recent_alerts.append(alert)
        return alert

    def _prune(self, now: float) -> None:
        stale_entities = [
            entity_key
            for entity_key, state in self.device_states.items()
            if now - state.last_seen > self.config.state_ttl_s
        ]
        for entity_key in stale_entities:
            self.device_states.pop(entity_key, None)

        for sightings in self.bssid_sightings.values():
            self._trim_deque(sightings, now, self.config.new_device_window_s)

        for mapping in self.ssid_to_bssids.values():
            self._trim_mapping(mapping, now, self.config.spoofing_window_s)
        for mapping in self.identity_to_bssids.values():
            self._trim_mapping(mapping, now, self.config.spoofing_window_s)

        for cache in self.correlation_cache.values():
            self._trim_deque(cache["wifi"], now, self.config.correlation_window_s)
            self._trim_deque(cache["ble"], now, self.config.correlation_window_s)

        stale_cooldowns = [
            key for key, last_seen in self.alert_cooldowns.items()
            if now - last_seen > self.config.state_ttl_s
        ]
        for key in stale_cooldowns:
            self.alert_cooldowns.pop(key, None)

    @staticmethod
    def _trim_deque(history: deque[Any], now: float, window_s: float) -> None:
        while history and (now - history[0][0] if isinstance(history[0], tuple) else now - history[0]) > window_s:
            history.popleft()

    @staticmethod
    def _trim_mapping(mapping: dict[str, float], now: float, window_s: float) -> None:
        for key, seen_at in list(mapping.items()):
            if now - seen_at > window_s:
                mapping.pop(key, None)
