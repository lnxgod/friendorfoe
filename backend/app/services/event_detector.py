"""First-seen event detector.

Layered on top of the ingest pipeline to emit a single persistent event the
FIRST time a noteworthy identifier is seen — probe MAC, probed-for SSID,
Remote ID drone, hostile tool (Pwnagotchi/Flipper), smart glasses, tracker,
or persistent AP. Subsequent sightings only update counters; they do not
create duplicate rows.

Design:
- An in-memory cache `_seen[(event_type, identifier)] -> last_seen_ts` is
  the hot path. Populated lazily and rehydrated from the Event table on
  startup (last 30 days, capped at 10k entries).
- Per-event-type debounce (sighting count + time window) filters one-off
  noise before an event is committed.
- Whitelists (SSID glob + MAC prefix) suppress events before they ever
  enter the cache — whitelisted identifiers are invisible to this layer.
- DB writes happen on an independent AsyncSession so a failure here never
  poisons the detection-ingest transaction.
"""

from __future__ import annotations

import fnmatch
import json
import logging
import time
from collections import Counter, defaultdict, deque
from dataclasses import dataclass, field
from datetime import datetime, timezone

from app.services.probe_identity import (
    mac_from_probe_identity,
    normalize_probe_identity,
    probe_identity_from_event,
)

logger = logging.getLogger(__name__)


# ── Per-event-type debounce policy ───────────────────────────────────────

@dataclass(frozen=True)
class DebouncePolicy:
    confirmations: int = 1       # sightings required before emit
    window_s: float = 0.0        # sightings must fall within this window
    min_confidence: float = 0.0  # detection confidence floor


DEBOUNCE: dict[str, DebouncePolicy] = {
    "new_probe_identity": DebouncePolicy(confirmations=2, window_s=300.0),
    "new_probe_mac":    DebouncePolicy(confirmations=2, window_s=300.0),
    "new_probed_ssid":  DebouncePolicy(confirmations=2, window_s=60.0),   # ≥2 distinct probers
    "probe_activity_spike": DebouncePolicy(confirmations=1),
    "new_rid_drone":    DebouncePolicy(confirmations=1, min_confidence=0.30),
    "new_hostile_tool": DebouncePolicy(confirmations=1),                    # instant
    "new_glasses":      DebouncePolicy(confirmations=2, window_s=60.0),
    "new_tracker":      DebouncePolicy(confirmations=2, window_s=300.0),
    "new_ap":           DebouncePolicy(confirmations=3, window_s=10.0),
    # Emitted once per entity per visit via emit_departure() — no debounce.
    "device_departed":  DebouncePolicy(confirmations=1),
}

# Severity per event type
SEVERITY: dict[str, str] = {
    "new_probe_identity": "info",
    "new_probe_mac":    "info",
    "new_probed_ssid":  "info",
    "probe_activity_spike": "warning",
    "new_rid_drone":    "warning",
    "new_hostile_tool": "critical",
    "new_glasses":      "warning",
    "new_tracker":      "info",
    "new_ap":           "info",
    "device_departed":  "info",
}


# ── Pending-confirmation state per (event_type, identifier) ──────────────

@dataclass
class Pending:
    """Accumulator for debounce — we hold this until policy is satisfied."""
    sightings: list[float] = field(default_factory=list)
    distinct_keys: set[str] = field(default_factory=set)
    best_rssi: int | None = None
    sensor_ids: set[str] = field(default_factory=set)
    first_seen: float = 0.0
    last_seen: float = 0.0
    # Frozen-at-first-sighting context
    metadata: dict = field(default_factory=dict)
    title: str = ""
    message: str = ""


@dataclass
class SeenEntry:
    """Already-emitted event — counters + flush-pending."""
    db_id: int                    # row id in Event table, or 0 if unknown yet
    first_seen: float
    last_seen: float
    sighting_count: int = 1
    sensor_ids: set[str] = field(default_factory=set)
    best_rssi: int | None = None
    dirty: bool = False            # counters diverge from DB


@dataclass
class ProbeActivity:
    sightings: deque[float] = field(default_factory=deque)
    sensor_hits: deque[tuple[float, str]] = field(default_factory=deque)
    ssid_hits: deque[tuple[float, str]] = field(default_factory=deque)
    last_emit_ts: float = -1.0


class EventDetector:
    """Singleton that ingests classified detections and emits first-seen events."""

    # Flush debounced counter updates at most this often per event
    _COUNTER_FLUSH_INTERVAL_S = 30.0
    # Cap the in-memory seen cache to avoid unbounded growth
    _CACHE_MAX = 10_000
    # Rehydrate window on startup (seconds)
    _REHYDRATE_WINDOW_S = 30 * 24 * 3600.0
    _PROBE_ACTIVITY_WINDOW_S = 10 * 60.0
    _PROBE_ACTIVITY_EMIT_DEDUPE_S = 6 * 3600.0

    def __init__(self) -> None:
        self._seen: dict[tuple[str, str], SeenEntry] = {}
        self._pending: dict[tuple[str, str], Pending] = {}
        self._probe_activity: dict[str, ProbeActivity] = {}
        self._last_flush: float = 0.0
        # Whitelists mirrored from DB (refreshed from the classifier's set
        # and the MAC table on startup + when CRUD endpoints update them)
        self._ssid_patterns: set[str] = set()
        self._mac_entries: set[str] = set()   # each entry uppercase; matched as prefix
        # Departure events queued by EntityTracker._prune, flushed by caller
        self._pending_departures: list[tuple[str, str]] = []

    # ── Public API ────────────────────────────────────────────────────

    async def hydrate_from_db(self, session) -> int:
        """Load recent events + whitelists into memory at startup."""
        try:
            from sqlalchemy import select
            from app.models.db_models import Event, WhitelistedMAC, WhitelistedSSID
        except Exception as e:
            logger.warning("EventDetector hydrate skipped (import): %s", e)
            return 0

        now = time.time()
        cutoff_dt = datetime.fromtimestamp(now - self._REHYDRATE_WINDOW_S, tz=timezone.utc)
        try:
            result = await session.execute(
                select(Event)
                .where(Event.first_seen_at >= cutoff_dt)
                .order_by(Event.first_seen_at.desc())
                .limit(self._CACHE_MAX)
            )
            loaded = 0
            for row in result.scalars().all():
                key = (row.event_type, row.identifier)
                self._seen[key] = SeenEntry(
                    db_id=row.id,
                    first_seen=row.first_seen_at.timestamp(),
                    last_seen=row.last_seen_at.timestamp(),
                    sighting_count=row.sighting_count,
                    sensor_ids=set(json.loads(row.sensor_ids_json or "[]")),
                    best_rssi=row.best_rssi,
                    dirty=False,
                )
                try:
                    metadata = json.loads(row.metadata_json or "{}")
                except Exception:
                    metadata = {}
                self._seen[key].__dict__["metadata"] = metadata
                if row.event_type == "probe_activity_spike":
                    probe_identity = probe_identity_from_event(row.event_type, row.identifier, metadata)
                    if probe_identity:
                        activity = self._probe_activity.setdefault(probe_identity, ProbeActivity())
                        activity.last_emit_ts = max(activity.last_emit_ts, row.last_seen_at.timestamp())
                loaded += 1
            # Whitelists
            mac_rows = (await session.execute(select(WhitelistedMAC))).scalars().all()
            self._mac_entries = {r.mac.upper() for r in mac_rows}
            ssid_rows = (await session.execute(select(WhitelistedSSID))).scalars().all()
            self._ssid_patterns = {r.pattern for r in ssid_rows} | {"FoF-*"}
            logger.info("EventDetector rehydrated: %d events, %d MAC whitelist, %d SSID whitelist",
                        loaded, len(self._mac_entries), len(self._ssid_patterns))
            return loaded
        except Exception as e:
            logger.warning("EventDetector rehydrate failed: %s", e)
            return 0

    def whitelist_mac(self, mac: str) -> None:
        if mac:
            self._mac_entries.add(mac.upper())

    def unwhitelist_mac(self, mac: str) -> None:
        self._mac_entries.discard((mac or "").upper())

    def whitelist_ssid(self, pattern: str) -> None:
        if pattern:
            self._ssid_patterns.add(pattern)

    def unwhitelist_ssid(self, pattern: str) -> None:
        self._ssid_patterns.discard(pattern)

    def emit_departure(self, entity_id: str, *, first_seen: float,
                       last_seen: float, sensor_ids: set[str] | None = None,
                       label: str = "", best_rssi: int | None = None) -> None:
        """Register a 'device_departed' event for an entity whose final
        sensor just went silent. Identifier pins the trip via the entity's
        first_seen so the same entity can generate a new departure event
        the next time it visits."""
        if not entity_id:
            return
        ident = f"{entity_id}:{int(first_seen)}"
        key = ("device_departed", ident)
        if key in self._seen:
            # Already registered this visit — just touch last_seen
            self._update_seen(key, best_rssi, None, last_seen)
            return
        dwell_s = max(0, int(last_seen - first_seen))
        dwell_human = (f"{dwell_s // 60}m {dwell_s % 60}s" if dwell_s >= 60
                       else f"{dwell_s}s")
        md = {
            "entity_id": entity_id,
            "label": label or "device",
            "first_seen": first_seen,
            "last_seen": last_seen,
            "dwell_s": dwell_s,
            "title": f"Device departed",
            "message": f"{label or entity_id} left (visit {dwell_human})",
        }
        self._seen[key] = SeenEntry(
            db_id=0,
            first_seen=first_seen,
            last_seen=last_seen,
            sighting_count=1,
            sensor_ids=set(sensor_ids or []),
            best_rssi=best_rssi,
            dirty=False,
        )
        self._seen[key].__dict__["metadata"] = md
        # Enqueue for the next commit_new() caller. We stash in a side list
        # so the caller can flush without needing the ingest() return value.
        self._pending_departures.append(key)

    def drain_pending_departures(self) -> list[tuple[str, str]]:
        """Return the list of freshly-emitted departure keys and clear the
        queue. Caller passes into commit_new(). Used by the detections
        router after each batch and by a periodic sweep task."""
        keys = list(self._pending_departures)
        self._pending_departures.clear()
        return keys

    def get_stats(self) -> dict:
        by_type: Counter = Counter()
        by_severity: Counter = Counter()
        unack = 0
        crit_unack = 0
        # Stats only reflect what's in cache — DB has the source of truth but
        # we answer the fast question "how many unacked events are live?"
        # via the cache + periodic hydrate.
        # (Callers who need the exact DB count should query the DB directly.)
        for (et, _), entry in self._seen.items():
            by_type[et] += 1
            sev = SEVERITY.get(et, "info")
            by_severity[sev] += 1
        return {
            "total": len(self._seen),
            "unacknowledged": unack,  # filled by DB-backed endpoint; this is cache-only
            "by_type": dict(by_type),
            "by_severity": dict(by_severity),
            "critical_unacked": crit_unack,
        }

    def ingest(self, *, source: str, classification: str, drone_id: str,
               bssid: str | None, ssid: str | None, manufacturer: str | None,
               model: str | None, probed_ssids: list[str] | None,
               ie_hash: str | None,
               rssi: int | None, confidence: float,
               sensor_id: str, ts: float,
               latitude: float | None = None, longitude: float | None = None,
               operator_id: str | None = None) -> list[tuple[str, str]]:
        """Classify which event types this detection qualifies for and
        advance their pending/seen state. Returns list of NEWLY-emitted
        (event_type, identifier) pairs for the caller to optionally flush
        to DB immediately; subsequent sightings return [].
        """
        newly_emitted: list[tuple[str, str]] = []

        for ev_type, identifier, ctx in self._extract_candidates(
            source=source, classification=classification, drone_id=drone_id,
            bssid=bssid, ssid=ssid, manufacturer=manufacturer, model=model,
            probed_ssids=probed_ssids, ie_hash=ie_hash, rssi=rssi,
            latitude=latitude, longitude=longitude, operator_id=operator_id,
        ):
            if self._is_whitelisted(ev_type, identifier):
                continue
            if not self._meets_min_confidence(ev_type, confidence):
                continue
            key = (ev_type, identifier)
            if key in self._seen:
                self._update_seen(key, rssi, sensor_id, ts)
                continue
            if self._advance_pending(ev_type, identifier, ctx, rssi, sensor_id, ts):
                newly_emitted.append(key)

        if source == "wifi_probe_request":
            spike_key = self._record_probe_activity(
                ie_hash=ie_hash,
                drone_id=drone_id,
                bssid=bssid,
                probed_ssids=probed_ssids,
                rssi=rssi,
                sensor_id=sensor_id,
                ts=ts,
            )
            if spike_key is not None:
                newly_emitted.append(spike_key)

        return newly_emitted

    async def flush_dirty(self, session) -> int:
        """Write debounced counter updates from cache to DB. Idempotent;
        caller invokes periodically (e.g., every 30 s or at end of batch)."""
        now = time.time()
        if now - self._last_flush < self._COUNTER_FLUSH_INTERVAL_S:
            return 0
        self._last_flush = now
        try:
            from sqlalchemy import update
            from app.models.db_models import Event
        except Exception:
            return 0

        dirty = [(k, e) for k, e in self._seen.items() if e.dirty and e.db_id]
        if not dirty:
            return 0
        written = 0
        try:
            for (_, ident), entry in dirty:
                await session.execute(
                    update(Event).where(Event.id == entry.db_id).values(
                        last_seen_at=datetime.fromtimestamp(entry.last_seen, tz=timezone.utc),
                        sighting_count=entry.sighting_count,
                        sensor_count=len(entry.sensor_ids),
                        sensor_ids_json=json.dumps(sorted(entry.sensor_ids)),
                        best_rssi=entry.best_rssi,
                    )
                )
                entry.dirty = False
                written += 1
            if written:
                await session.commit()
        except Exception as e:
            logger.warning("EventDetector.flush_dirty failed: %s", e)
        return written

    async def commit_new(self, session, keys: list[tuple[str, str]]) -> int:
        """Insert new Event rows for freshly-emitted keys. Caller passes
        the list returned from ingest()."""
        if not keys:
            return 0
        try:
            from app.models.db_models import Event
        except Exception:
            return 0

        written = 0
        try:
            for key in keys:
                entry = self._seen.get(key)
                if not entry or entry.db_id:
                    continue
                row = Event(
                    event_type=key[0],
                    identifier=key[1],
                    severity=SEVERITY.get(key[0], "info"),
                    title=entry.sensor_ids and next(iter(entry.sensor_ids)) or "",  # placeholder
                    message="",  # will be replaced by metadata_json lookup below
                    first_seen_at=datetime.fromtimestamp(entry.first_seen, tz=timezone.utc),
                    last_seen_at=datetime.fromtimestamp(entry.last_seen, tz=timezone.utc),
                    sighting_count=entry.sighting_count,
                    sensor_count=len(entry.sensor_ids),
                    sensor_ids_json=json.dumps(sorted(entry.sensor_ids)),
                    best_rssi=entry.best_rssi,
                    metadata_json=json.dumps(entry.__dict__.get("metadata", {})),
                )
                # Hydrate title/message from cached metadata
                md = entry.__dict__.get("metadata") or {}
                row.title = md.get("title", self._default_title(key[0], key[1]))
                row.message = md.get("message", self._default_message(key[0], key[1], md))
                row.metadata_json = json.dumps(md)
                session.add(row)
                await session.flush()  # assigns PK
                entry.db_id = row.id
                written += 1
            if written:
                await session.commit()
        except Exception as e:
            logger.warning("EventDetector.commit_new failed: %s", e)
        return written

    # ── Internals ─────────────────────────────────────────────────────

    def _extract_candidates(self, *, source, classification, drone_id, bssid,
                            ssid, manufacturer, model, probed_ssids, ie_hash, rssi,
                            latitude, longitude, operator_id):
        """Yield (event_type, identifier, context_dict) tuples for every
        event this detection may qualify for."""
        mfr = manufacturer or ""
        did = drone_id or ""

        # Hostile tool — highest priority, instant
        if classification == "hostile_tool" or mfr.lower() == "pwnagotchi":
            yield ("new_hostile_tool", bssid or did or "pwnagotchi", {
                "manufacturer": mfr or "Pwnagotchi",
                "classification": classification,
                "ssid": ssid, "bssid": bssid,
                "title": "Hostile tool detected",
                "message": f"{mfr or 'Pwnagotchi'} beacon seen on {bssid or did}",
            })

        # Remote ID drone — authoritative identifier
        if did.startswith("rid_"):
            yield ("new_rid_drone", did, {
                "uas_id": did[4:], "operator_id": operator_id,
                "lat": latitude, "lon": longitude,
                "manufacturer": mfr, "model": model,
                "title": "New RID drone",
                "message": f"ASTM Remote ID {did[4:]} first seen",
            })

        # BLE Remote ID path (wifi_beacon_rid, wifi_dji_ie, ble_rid with confirmed_drone)
        if classification == "confirmed_drone" and source in ("wifi_beacon_rid", "wifi_dji_ie") and did:
            yield ("new_rid_drone", did, {
                "source": source, "manufacturer": mfr, "model": model,
                "title": "New drone (WiFi RID)",
                "message": f"{did} first seen via {source}",
            })

        # Smart glasses (manufacturer set by scanner's glasses_detector /
        # ble_fingerprint → "Meta Glasses", "Meta Device", "Luxottica …",
        # "Snap", "Bose Frames", etc.)
        if mfr in ("Meta Glasses", "Meta Device", "Snap") \
                or mfr.startswith("Luxottica") \
                or (model or "").startswith("FP:") and mfr in ("Meta", "Snap"):
            # Prefer fingerprint (stable across MAC rotation) else fall back
            ident = model if (model or "").startswith("FP:") else (bssid or did)
            if ident:
                yield ("new_glasses", ident, {
                    "manufacturer": mfr, "device_type": "Smart Glasses",
                    "bssid": bssid,
                    "title": "New smart glasses",
                    "message": f"{mfr} device first seen",
                })

        # Trackers — by drone_id type hint (BLE:HASH:AirTag, ...:Tile Tracker, etc.)
        if classification == "tracker":
            ident = model if (model or "").startswith("FP:") else did
            type_name = did.rsplit(":", 1)[-1] if ":" in did else "Tracker"
            yield ("new_tracker", ident, {
                "tracker_type": type_name, "manufacturer": mfr,
                "title": "New tracker",
                "message": f"{type_name} ({mfr or 'unknown brand'}) first seen",
            })

        # Probe request paths
        if source == "wifi_probe_request":
            probe = normalize_probe_identity(ie_hash=ie_hash, drone_id=did, bssid=bssid)
            if probe.identity:
                yield ("new_probe_identity", probe.identity, {
                    "probe_identity": probe.identity,
                    "ie_hash": probe.ie_hash,
                    "mac": probe.mac,
                    "probed_ssids": list(probed_ssids or []),
                    "manufacturer": mfr,
                    "title": "New probe identity",
                    "message": f"{probe.identity} probing {len(probed_ssids or [])} SSIDs",
                })
            if probe.mac and not probe.ie_hash:
                mac = probe.mac
                yield ("new_probe_mac", mac, {
                    "probe_identity": probe.identity,
                    "ie_hash": probe.ie_hash,
                    "mac": probe.mac,
                    "probed_ssids": list(probed_ssids or []),
                    "manufacturer": mfr,
                    "title": "New probing device",
                    "message": f"MAC {mac} probing {len(probed_ssids or [])} SSIDs",
                })
            for probed in (probed_ssids or []):
                if probed and probed != "(broadcast)":
                    yield ("new_probed_ssid", probed, {
                        "first_prober": probe.mac,
                        "probe_identity": probe.identity,
                        "title": "New SSID being searched for",
                        "message": f"Someone nearby is probing for '{probed}'",
                    })

        # New WiFi AP — BSSID broadcasting a persistent SSID
        if source == "wifi_oui" and bssid and ssid:
            yield ("new_ap", bssid.upper(), {
                "ssid": ssid, "manufacturer": mfr,
                "title": "New WiFi access point",
                "message": f"AP '{ssid}' ({bssid}) first observed",
            })

    def _is_whitelisted(self, event_type: str, identifier: str) -> bool:
        if event_type in ("new_probe_mac", "new_ap"):
            up = (identifier or "").upper()
            for entry in self._mac_entries:
                if up.startswith(entry):
                    return True
        if event_type == "new_probe_identity":
            mac = mac_from_probe_identity(identifier)
            if mac:
                for entry in self._mac_entries:
                    if mac.startswith(entry):
                        return True
        if event_type == "new_probed_ssid":
            for pat in self._ssid_patterns:
                if fnmatch.fnmatch(identifier, pat):
                    return True
        return False

    def _meets_min_confidence(self, event_type: str, confidence: float) -> bool:
        policy = DEBOUNCE.get(event_type)
        if not policy:
            return True
        return confidence >= policy.min_confidence

    def _advance_pending(self, ev_type, identifier, ctx, rssi, sensor_id, ts) -> bool:
        """Return True when debounce is satisfied and event should emit."""
        policy = DEBOUNCE.get(ev_type, DebouncePolicy())
        key = (ev_type, identifier)
        p = self._pending.get(key)
        if p is None:
            p = Pending(first_seen=ts, metadata=ctx,
                        title=ctx.get("title", ""), message=ctx.get("message", ""))
            self._pending[key] = p

        p.sightings.append(ts)
        if sensor_id:
            p.sensor_ids.add(sensor_id)
        if rssi is not None:
            if p.best_rssi is None or rssi > p.best_rssi:
                p.best_rssi = rssi
        p.last_seen = ts

        # "distinct keys" is used for new_probed_ssid (require 2 different probers)
        probing_mac = (ctx.get("first_prober") or "").upper()
        if probing_mac:
            p.distinct_keys.add(probing_mac)

        # Trim sightings outside the window
        if policy.window_s > 0:
            cutoff = ts - policy.window_s
            p.sightings = [s for s in p.sightings if s >= cutoff]

        # Check debounce satisfaction
        satisfied = False
        if ev_type == "new_probed_ssid":
            satisfied = len(p.distinct_keys) >= policy.confirmations
        elif policy.window_s > 0:
            # Need >= confirmations within window
            satisfied = len(p.sightings) >= policy.confirmations
        else:
            satisfied = len(p.sightings) >= policy.confirmations

        if not satisfied:
            return False

        # Emit: promote pending → seen
        self._seen[key] = SeenEntry(
            db_id=0,  # DB row id filled by commit_new()
            first_seen=p.first_seen,
            last_seen=p.last_seen,
            sighting_count=len(p.sightings) or 1,
            sensor_ids=set(p.sensor_ids),
            best_rssi=p.best_rssi,
            dirty=False,
        )
        # Stash metadata on the SeenEntry via its __dict__ so commit_new()
        # can hydrate the DB row.
        self._seen[key].__dict__["metadata"] = dict(p.metadata)
        del self._pending[key]
        # Bound the cache
        if len(self._seen) > self._CACHE_MAX:
            oldest = sorted(self._seen.items(), key=lambda kv: kv[1].last_seen)
            for k, _ in oldest[: len(self._seen) - self._CACHE_MAX]:
                self._seen.pop(k, None)
        logger.info("event emit %s %s (conf=%d sensors=%d rssi=%s)",
                    ev_type, identifier, len(p.sightings),
                    len(p.sensor_ids), p.best_rssi)
        return True

    def _update_seen(self, key, rssi, sensor_id, ts) -> None:
        entry = self._seen[key]
        entry.last_seen = ts
        entry.sighting_count += 1
        if sensor_id:
            entry.sensor_ids.add(sensor_id)
        if rssi is not None and (entry.best_rssi is None or rssi > entry.best_rssi):
            entry.best_rssi = rssi
        entry.dirty = True

    def _record_probe_activity(self, *,
                               ie_hash: str | None,
                               drone_id: str | None,
                               bssid: str | None,
                               probed_ssids: list[str] | None,
                               rssi: int | None,
                               sensor_id: str,
                               ts: float) -> tuple[str, str] | None:
        probe = normalize_probe_identity(ie_hash=ie_hash, drone_id=drone_id, bssid=bssid)
        identity = probe.identity
        if not identity or identity == "PROBE:UNKNOWN":
            return None

        activity = self._probe_activity.setdefault(identity, ProbeActivity())
        activity.sightings.append(ts)
        activity.sensor_hits.append((ts, sensor_id))
        for ssid in probed_ssids or []:
            if ssid and ssid != "(broadcast)":
                activity.ssid_hits.append((ts, ssid))

        cutoff = ts - self._PROBE_ACTIVITY_WINDOW_S
        while activity.sightings and activity.sightings[0] < cutoff:
            activity.sightings.popleft()
        while activity.sensor_hits and activity.sensor_hits[0][0] < cutoff:
            activity.sensor_hits.popleft()
        while activity.ssid_hits and activity.ssid_hits[0][0] < cutoff:
            activity.ssid_hits.popleft()

        distinct_sensors = {sensor for _, sensor in activity.sensor_hits if sensor}
        distinct_ssids = {ssid for _, ssid in activity.ssid_hits if ssid}
        should_emit = (
            len(activity.sightings) >= 25
            or len(distinct_sensors) >= 3
            or len(distinct_ssids) >= 5
        )
        if not should_emit:
            return None
        if activity.last_emit_ts >= 0 and (ts - activity.last_emit_ts) < self._PROBE_ACTIVITY_EMIT_DEDUPE_S:
            return None

        window_key = int(ts // self._PROBE_ACTIVITY_EMIT_DEDUPE_S) * int(self._PROBE_ACTIVITY_EMIT_DEDUPE_S)
        key = ("probe_activity_spike", f"{identity}@{window_key}")
        if key in self._seen:
            return None

        activity.last_emit_ts = ts
        metadata = {
            "probe_identity": identity,
            "ie_hash": probe.ie_hash,
            "mac": probe.mac,
            "window_s": int(self._PROBE_ACTIVITY_WINDOW_S),
            "sighting_count": len(activity.sightings),
            "sensor_count": len(distinct_sensors),
            "probed_ssids": sorted(distinct_ssids),
            "title": "Probe activity spike",
            "message": (
                f"{identity} spiked to {len(activity.sightings)} sightings across "
                f"{len(distinct_sensors)} sensor(s)"
            ),
        }
        self._seen[key] = SeenEntry(
            db_id=0,
            first_seen=ts,
            last_seen=ts,
            sighting_count=len(activity.sightings),
            sensor_ids=distinct_sensors,
            best_rssi=rssi,
            dirty=False,
        )
        self._seen[key].__dict__["metadata"] = metadata
        if len(self._seen) > self._CACHE_MAX:
            oldest = sorted(self._seen.items(), key=lambda kv: kv[1].last_seen)
            for victim, _ in oldest[: len(self._seen) - self._CACHE_MAX]:
                self._seen.pop(victim, None)
        logger.info(
            "event emit probe_activity_spike %s (hits=%d sensors=%d ssids=%d rssi=%s)",
            identity,
            len(activity.sightings),
            len(distinct_sensors),
            len(distinct_ssids),
            rssi,
        )
        return key

    def _default_title(self, event_type: str, identifier: str) -> str:
        pretty = {
            "new_probe_identity": "New probe identity",
            "new_probe_mac":    "New probing device",
            "new_probed_ssid":  "New SSID being searched for",
            "probe_activity_spike": "Probe activity spike",
            "new_rid_drone":    "New Remote ID drone",
            "new_hostile_tool": "Hostile tool detected",
            "new_glasses":      "New smart glasses",
            "new_tracker":      "New tracker",
            "new_ap":           "New WiFi access point",
        }
        return pretty.get(event_type, event_type)

    def _default_message(self, event_type: str, identifier: str, md: dict) -> str:
        if event_type == "new_probe_identity":
            n = len(md.get("probed_ssids") or [])
            return f"{identifier} probing {n} SSID(s)"
        if event_type == "new_probe_mac":
            n = len(md.get("probed_ssids") or [])
            return f"MAC {identifier} probing {n} SSID(s)"
        if event_type == "new_probed_ssid":
            prober = md.get("first_prober", "?")
            return f"{prober} probing for '{identifier}'"
        if event_type == "probe_activity_spike":
            return (
                f"{md.get('probe_identity', identifier)} spiked to "
                f"{md.get('sighting_count', 0)} sightings"
            )
        if event_type == "new_rid_drone":
            return f"ASTM Remote ID {identifier} first seen"
        if event_type == "new_hostile_tool":
            mfr = md.get("manufacturer", "hostile tool")
            return f"{mfr} signature detected on {identifier}"
        if event_type == "new_glasses":
            return f"{md.get('manufacturer', 'Smart glasses')} first seen"
        if event_type == "new_tracker":
            return f"{md.get('tracker_type', 'Tracker')} first seen"
        if event_type == "new_ap":
            return f"AP '{md.get('ssid', '?')}' ({identifier}) first observed"
        return f"First sighting of {identifier}"
