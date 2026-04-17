"""Cross-layer identity correlator.

Job: group multiple random MACs + multiple BLE fingerprints that belong to
the same physical device, using signals that persist across MAC rotation.

Three grouping keys today:
1. **WiFi probe-IE hash** — stable across random-MAC rotations on the same
   physical radio (IE ordering + HT/VHT + rates). Scanner computes it; when
   uplink firmware forwards it (v2) the backend uses it here as a
   wifi-radio identity key.
2. **BLE fingerprint (FP:)** — already computed upstream by BLEEnricher.
3. **Temporal BLE↔WiFi co-location** — same sensor + Δt ≤ 5 s + |ΔRSSI| ≤
   15 dB. Good enough to link a phone's BLE+WiFi sightings.

When two distinct entities are linked, EntityTracker merges them. Links
are persisted in the IdentityLink table so cross-session grouping survives
a backend restart (see models/db_models.py).
"""

from __future__ import annotations

import json
import logging
import time
from collections import defaultdict, deque
from dataclasses import dataclass, field
from datetime import datetime, timezone

logger = logging.getLogger(__name__)

# Co-location window: same sensor, Δt ≤ this many seconds, |ΔRSSI| ≤ this many dB.
_COLOCATION_WINDOW_S = 5.0
_COLOCATION_RSSI_DB  = 15
# Debounce DB writes like EntityTracker does
_CHECKPOINT_DEBOUNCE_S = 30.0
# Cap the in-memory sliding window per sensor
_PER_SENSOR_MAX = 200
# Cap the in-memory probe-IE → entity cache
_IE_CACHE_MAX = 4096


@dataclass
class _SensorSighting:
    """One radio observation for colocation matching."""
    ts: float
    kind: str          # "ble" | "wifi"
    key: str           # FP:xxxxxxxx or BSSID
    rssi: int | None


@dataclass
class _LinkRow:
    """Mirrors the IdentityLink row during its debounced life in memory."""
    ble_fp: str
    wifi_key: str
    sensor_id: str
    first_seen: float
    last_seen: float
    confirm_count: int = 1
    best_rssi_delta: int | None = None
    dirty: bool = True          # needs DB write
    db_id: int = 0


class IdentityCorrelator:
    """Module-level singleton instantiated in detections.py."""

    def __init__(self) -> None:
        # Sliding window of recent sightings per sensor; matched against each
        # incoming sighting to spot BLE↔WiFi co-locations.
        self._recent: dict[str, deque[_SensorSighting]] = defaultdict(
            lambda: deque(maxlen=_PER_SENSOR_MAX)
        )
        # probe-IE hash → entity_id (when resolved upstream by EntityTracker)
        self._ie_to_entity: dict[str, str] = {}
        # ble_fp + wifi_key → _LinkRow (in-memory debounced state)
        self._links: dict[tuple[str, str], _LinkRow] = {}
        self._last_checkpoint: float = 0.0

    # ── Public API ────────────────────────────────────────────────────

    def ingest(self, *, source: str, drone_id: str, bssid: str | None,
               model: str | None, ie_hash: str | None,
               sensor_id: str, rssi: int | None, ts: float) -> list[str]:
        """Record this observation + return entity_ids we believe should be
        merged with the incoming signal's natural entity (caller applies the
        merge). Empty list when no cross-link suggestion.

        The caller keeps sovereign control of entity IDs. We only *suggest*
        merges via the linked partner's key.
        """
        suggestions: list[str] = []

        # BLE fingerprint is the "FP:" prefix on model field, per our pipeline
        ble_fp: str | None = None
        if source.startswith("ble") and model and model.startswith("FP:"):
            ble_fp = model
        wifi_key: str | None = None
        if source.startswith("wifi"):
            # Prefer probe-IE hash (stable across rotation), else the MAC
            # itself — we still want a link for non-random APs.
            if ie_hash:
                wifi_key = f"ieh:{ie_hash}"
            elif bssid:
                wifi_key = bssid.upper()

        now = ts if ts > 0 else time.time()
        # Record this sighting into the sensor's sliding window. Trim old.
        win = self._recent[sensor_id]
        cutoff = now - _COLOCATION_WINDOW_S
        while win and win[0].ts < cutoff:
            win.popleft()

        if ble_fp:
            suggestions.extend(self._match(win, ble_fp, wifi_or_ble="ble",
                                            rssi=rssi, sensor_id=sensor_id, now=now))
            win.append(_SensorSighting(now, "ble", ble_fp, rssi))
        if wifi_key:
            suggestions.extend(self._match(win, wifi_key, wifi_or_ble="wifi",
                                            rssi=rssi, sensor_id=sensor_id, now=now))
            win.append(_SensorSighting(now, "wifi", wifi_key, rssi))

        return suggestions

    def find_identity(self, ie_hash: str | None) -> str | None:
        """Return the entity_id previously associated with this probe-IE hash,
        or None. Called by EntityTracker._correlate_by_probes."""
        if not ie_hash:
            return None
        return self._ie_to_entity.get(f"ieh:{ie_hash}")

    def remember_identity(self, ie_hash: str | None, entity_id: str) -> None:
        """EntityTracker informs us when an IE-hash got a canonical entity."""
        if not ie_hash or not entity_id:
            return
        key = f"ieh:{ie_hash}"
        self._ie_to_entity[key] = entity_id
        # Tiny LRU eviction
        if len(self._ie_to_entity) > _IE_CACHE_MAX:
            drop = list(self._ie_to_entity.keys())[: len(self._ie_to_entity) - _IE_CACHE_MAX]
            for k in drop:
                self._ie_to_entity.pop(k, None)

    def get_stats(self) -> dict:
        return {
            "tracked_sensors": len(self._recent),
            "cached_ie_identities": len(self._ie_to_entity),
            "pending_links": sum(1 for l in self._links.values() if l.dirty),
            "total_links_known": len(self._links),
        }

    async def checkpoint(self) -> int:
        """Write dirty IdentityLink rows. Debounced to ~30 s like
        EntityTracker.checkpoint. Uses its own AsyncSession."""
        now = time.time()
        if now - self._last_checkpoint < _CHECKPOINT_DEBOUNCE_S:
            return 0
        if not any(l.dirty for l in self._links.values()):
            self._last_checkpoint = now
            return 0

        try:
            from sqlalchemy import select
            from app.models.db_models import IdentityLink
            from app.services.database import async_session
        except Exception:
            return 0

        dirty = [l for l in self._links.values() if l.dirty]
        self._last_checkpoint = now
        written = 0
        try:
            async with async_session() as session:
                for link in dirty:
                    if link.db_id:
                        # Update in place
                        row = (await session.execute(
                            select(IdentityLink).where(IdentityLink.id == link.db_id)
                        )).scalar_one_or_none()
                        if row:
                            row.last_seen_at = datetime.fromtimestamp(link.last_seen, tz=timezone.utc)
                            row.confirm_count = link.confirm_count
                            row.best_rssi_delta = link.best_rssi_delta
                    else:
                        # Upsert by (ble_fp, wifi_key)
                        existing = (await session.execute(
                            select(IdentityLink).where(
                                IdentityLink.ble_fp == link.ble_fp,
                                IdentityLink.wifi_key == link.wifi_key,
                            )
                        )).scalar_one_or_none()
                        if existing:
                            existing.last_seen_at = datetime.fromtimestamp(link.last_seen, tz=timezone.utc)
                            existing.confirm_count = link.confirm_count
                            existing.best_rssi_delta = link.best_rssi_delta
                            link.db_id = existing.id
                        else:
                            row = IdentityLink(
                                ble_fp=link.ble_fp,
                                wifi_key=link.wifi_key,
                                sensor_id=link.sensor_id,
                                first_seen_at=datetime.fromtimestamp(link.first_seen, tz=timezone.utc),
                                last_seen_at=datetime.fromtimestamp(link.last_seen, tz=timezone.utc),
                                confirm_count=link.confirm_count,
                                best_rssi_delta=link.best_rssi_delta,
                            )
                            session.add(row)
                            await session.flush()
                            link.db_id = row.id
                    link.dirty = False
                    written += 1
                await session.commit()
        except Exception as e:
            logger.warning("IdentityCorrelator.checkpoint failed: %s", e)
            return 0
        return written

    # ── Internals ─────────────────────────────────────────────────────

    def _match(self, win: deque, key: str, *, wifi_or_ble: str, rssi: int | None,
               sensor_id: str, now: float) -> list[str]:
        """Walk the window, find opposite-kind sightings within the RSSI gate,
        and upsert a link. Returns the partner keys for the caller."""
        partners: list[str] = []
        for sighting in win:
            if sighting.kind == wifi_or_ble:
                continue
            if now - sighting.ts > _COLOCATION_WINDOW_S:
                continue
            if rssi is not None and sighting.rssi is not None:
                delta = abs(rssi - sighting.rssi)
                if delta > _COLOCATION_RSSI_DB:
                    continue
            else:
                delta = None
            # Build link row (ble_fp, wifi_key, sensor_id)
            if wifi_or_ble == "ble":
                ble_fp, wifi_key = key, sighting.key
            else:
                ble_fp, wifi_key = sighting.key, key
            if not ble_fp.startswith("FP:"):
                continue
            lk = self._links.get((ble_fp, wifi_key))
            if lk is None:
                lk = _LinkRow(ble_fp=ble_fp, wifi_key=wifi_key,
                              sensor_id=sensor_id, first_seen=now, last_seen=now,
                              confirm_count=1, best_rssi_delta=delta, dirty=True)
                self._links[(ble_fp, wifi_key)] = lk
                logger.info("identity link formed: ble=%s wifi=%s sensor=%s Δrssi=%s",
                            ble_fp, wifi_key, sensor_id, delta)
            else:
                lk.last_seen = now
                lk.confirm_count += 1
                if delta is not None and (lk.best_rssi_delta is None or delta < lk.best_rssi_delta):
                    lk.best_rssi_delta = delta
                lk.dirty = True
            partners.append(sighting.key)
        return partners
