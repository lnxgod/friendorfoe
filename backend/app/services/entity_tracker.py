"""Entity Tracker — correlates BLE fingerprints, WiFi probes, and hotspots
into logical entities (people/device-groups).

Key features:
- Multi-sensor disappearance: entity is "gone" only when ALL sensors lose it
- Probe SSID correlation: random MACs probing same SSIDs → same entity
- BLE + WiFi + hotspot linking into single identity
- Timeline generation for entity visit tracking
"""

import json
import logging
import time
import uuid
from collections import defaultdict
from dataclasses import dataclass, field

logger = logging.getLogger(__name__)


@dataclass
class EntityComponent:
    """A single signal source linked to an entity."""
    id_type: str               # "ble", "wifi_probe", "wifi_ap", "hotspot"
    identifier: str            # FP:hash, MAC, SSID
    device_type: str = ""      # iPhone, MacBook, Unknown
    manufacturer: str = ""
    rssi: int = -100
    probed_ssids: set = field(default_factory=set)
    last_seen: float = 0.0
    sensor_id: str = ""


@dataclass
class TimelineEvent:
    timestamp: float
    event: str
    sensor: str = ""
    rssi: int = 0


@dataclass
class TrackedEntity:
    entity_id: str
    label: str = "Unknown"
    category: str = "visitor"       # visitor, neighbor, staff, delivery, threat
    first_seen: float = 0.0
    last_seen: float = 0.0
    is_active: bool = True

    components: dict = field(default_factory=dict)     # identifier → EntityComponent
    probed_ssids: set = field(default_factory=set)      # union of all probe SSIDs
    sensors_active: dict = field(default_factory=dict)  # sensor_id → last_seen timestamp
    dominant_sensor: str = ""
    peak_rssi: int = -100
    current_rssi: int = -100
    rssi_trend: str = ""            # "approaching", "departing", "stationary"

    timeline: list = field(default_factory=list)        # TimelineEvent list (max 50)
    fingerprints: set = field(default_factory=set)      # BLE fingerprint hashes
    manufacturers: set = field(default_factory=set)


class EntityTracker:
    """Correlates signals into tracked entities."""

    GONE_TIMEOUT_S = 300        # 5 min all-sensor silence → departed
    STALE_TIMEOUT_S = 1800      # 30 min → remove entity from active tracking
    CORRELATION_THRESHOLD = 55  # Score > 55 → link to entity
    MAX_TIMELINE = 50
    CHECKPOINT_DEBOUNCE_S = 30.0
    REHYDRATE_WINDOW_S = 24 * 3600.0

    def __init__(self):
        self.entities: dict[str, TrackedEntity] = {}
        # Indexes for fast lookup
        self._id_to_entity: dict[str, str] = {}    # identifier → entity_id
        self._fp_to_entity: dict[str, str] = {}    # BLE fingerprint → entity_id
        self._ssid_index: dict[str, set] = defaultdict(set)  # ssid → set of entity_ids
        self._last_prune = time.time()
        # Optional reference to BLEEnricher so we can consult Apple auth-tag
        # links and merge entities across MAC rotations.
        self._ble_enricher = None
        # Optional reference to the IdentityCorrelator so _correlate_by_probes
        # can short-circuit Jaccard when a probe-IE-hash maps to a known entity.
        self._identity_correlator = None
        # Persistence bookkeeping
        self._dirty: set[str] = set()
        self._last_checkpoint: float = 0.0

    def set_ble_enricher(self, enricher) -> None:
        """Inject BLEEnricher so we can use auth-tag MAC-rotation links."""
        self._ble_enricher = enricher

    def set_identity_correlator(self, correlator) -> None:
        """Inject IdentityCorrelator for probe-IE entity short-circuiting."""
        self._identity_correlator = correlator

    def get_entity_id(self, identifier: str) -> str | None:
        """Return the entity_id that owns this identifier, or None if unknown."""
        if not identifier:
            return None
        eid = self._id_to_entity.get(identifier)
        if eid:
            return eid
        # Fallback: treat a bare FP as a fingerprint lookup too
        if identifier.startswith("FP:"):
            return self._fp_to_entity.get(identifier)
        return None

    def get_entity(self, entity_id: str) -> TrackedEntity | None:
        return self.entities.get(entity_id)

    def _merge_entities(self, primary_id: str, secondary_id: str, now: float) -> None:
        """Fold `secondary_id` into `primary_id` and repoint all indexes.

        Invoked when we discover two entities are actually one physical device
        (e.g., Apple auth-tag link, or a WiFi MAC that later matches a BLE FP).
        """
        if primary_id == secondary_id:
            return
        primary = self.entities.get(primary_id)
        secondary = self.entities.get(secondary_id)
        if not primary or not secondary:
            return

        # Snapshot timestamps BEFORE mutating primary — needed to decide which
        # entity's current_rssi / dominant_sensor survives.
        primary_last_seen_pre = primary.last_seen

        # Merge components
        for comp_id, comp in secondary.components.items():
            if comp_id not in primary.components:
                primary.components[comp_id] = comp
            self._id_to_entity[comp_id] = primary_id

        # Merge fingerprints and repoint fp index
        for fp in secondary.fingerprints:
            primary.fingerprints.add(fp)
            self._fp_to_entity[fp] = primary_id

        # Merge probe SSIDs and repoint ssid_index
        for ssid in secondary.probed_ssids:
            primary.probed_ssids.add(ssid)
            bucket = self._ssid_index.get(ssid)
            if bucket is not None:
                bucket.discard(secondary_id)
                bucket.add(primary_id)

        # Merge sensor liveness
        for sensor_id, ts in secondary.sensors_active.items():
            prev = primary.sensors_active.get(sensor_id, 0.0)
            if ts > prev:
                primary.sensors_active[sensor_id] = ts

        primary.manufacturers |= secondary.manufacturers
        primary.first_seen = min(primary.first_seen, secondary.first_seen)
        primary.last_seen = max(primary.last_seen, secondary.last_seen)
        primary.peak_rssi = max(primary.peak_rssi, secondary.peak_rssi)
        # current_rssi / dominant_sensor: keep whichever was more recent at the
        # moment we started the merge. Comparing against the already-updated
        # primary.last_seen would only match on ties.
        if secondary.last_seen > primary_last_seen_pre:
            primary.current_rssi = secondary.current_rssi
            primary.dominant_sensor = secondary.dominant_sensor
        primary.is_active = primary.is_active or secondary.is_active
        primary.label = self._generate_label(primary)
        self._add_timeline(primary, now,
                           f"Merged with {secondary_id} (MAC rotation / cross-source)",
                           "", 0)

        # Remove the secondary entity and sweep it out of every index +
        # dirty queue so a stale checkpoint doesn't try to write it.
        self.entities.pop(secondary_id, None)
        self._dirty.discard(secondary_id)
        self._dirty.add(primary_id)
        stale_ids = [k for k, v in self._id_to_entity.items() if v == secondary_id]
        for k in stale_ids:
            self._id_to_entity.pop(k, None)
        stale_fps = [k for k, v in self._fp_to_entity.items() if v == secondary_id]
        for k in stale_fps:
            self._fp_to_entity.pop(k, None)
        for bucket in self._ssid_index.values():
            bucket.discard(secondary_id)

    def ingest(self, drone_id: str, source: str, confidence: float,
               rssi: int, ssid: str, bssid: str, manufacturer: str,
               device_id: str, received_at: float, model: str = "",
               probed_ssids: list | None = None, device_type: str = "",
               ie_hash: str | None = None,
               **kwargs):
        """Process a detection and assign to an entity."""
        now = received_at or time.time()

        # Determine the primary identifier for this signal
        identifier = drone_id or bssid or ssid
        if not identifier:
            return

        # Check if we already know this identifier
        entity_id = self._id_to_entity.get(identifier)

        # For BLE fingerprints, also check fingerprint index
        if not entity_id and drone_id and drone_id.startswith("FP:"):
            entity_id = self._fp_to_entity.get(drone_id)

        # For WiFi probes with SSIDs, try to correlate by probe overlap
        # ONLY for random MACs — static/burned-in MACs are always unique devices
        if not entity_id and source == "wifi_probe_request" and probed_ssids:
            mac = bssid or _extract_mac(drone_id)
            if mac and _is_random_mac(mac):
                entity_id = self._correlate_by_probes(
                    identifier, set(probed_ssids), rssi, device_id, manufacturer,
                    now, ie_hash=ie_hash,
                )

        # Create new entity if no match found
        if not entity_id:
            entity_id = f"ent-{uuid.uuid4().hex[:8]}"
            entity = TrackedEntity(
                entity_id=entity_id,
                first_seen=now,
                last_seen=now,
            )
            self.entities[entity_id] = entity
            self._id_to_entity[identifier] = entity_id

            # Index fingerprint
            if drone_id and drone_id.startswith("FP:"):
                self._fp_to_entity[drone_id] = entity_id
                entity.fingerprints.add(drone_id)

            # Remember IE-hash→entity so a subsequent MAC-rotated probe from
            # the same chipset lands on this entity without needing Jaccard.
            if ie_hash and self._identity_correlator is not None:
                try:
                    self._identity_correlator.remember_identity(ie_hash, entity_id)
                except Exception:
                    pass

        entity = self.entities.get(entity_id)
        if not entity:
            return

        # Apple auth-tag MAC-rotation merge: if our BLE FP is known to the
        # enricher as the same physical device as other FPs, merge their
        # entities into this one. Prevents a single iPhone from appearing as
        # many separate entities over its MAC-rotation cycle.
        if drone_id and drone_id.startswith("FP:") and self._ble_enricher is not None:
            try:
                linked = self._ble_enricher.get_linked_fingerprints(drone_id)
            except Exception:
                linked = set()
            for linked_fp in linked:
                other_eid = self._fp_to_entity.get(linked_fp)
                if other_eid and other_eid != entity_id:
                    self._merge_entities(entity_id, other_eid, now)

        # OUI lookup for WiFi devices that don't have manufacturer set
        if not manufacturer and bssid and len(bssid) >= 8:
            try:
                from app.services.enrichment_ble import oui_lookup
                manufacturer = oui_lookup(bssid) or ""
            except ImportError:
                pass

        # Update entity
        entity.last_seen = now
        entity.is_active = True
        if manufacturer:
            entity.manufacturers.add(manufacturer)

        # Update sensor tracking
        entity.sensors_active[device_id] = now
        prev_sensor = entity.dominant_sensor

        # Find dominant sensor (strongest recent signal)
        if rssi > entity.current_rssi or device_id == entity.dominant_sensor:
            entity.current_rssi = rssi
            entity.dominant_sensor = device_id
        if rssi > entity.peak_rssi:
            entity.peak_rssi = rssi

        # Add/update component
        comp = entity.components.get(identifier)
        if not comp:
            comp = EntityComponent(
                id_type=_source_to_type(source),
                identifier=identifier,
                device_type=device_type or _extract_device_type(drone_id),
                manufacturer=manufacturer or "",
            )
            entity.components[identifier] = comp
            self._id_to_entity[identifier] = entity_id

            # Timeline: new component discovered
            if len(entity.components) > 1:
                self._add_timeline(entity, now,
                    f"New {comp.id_type} signal: {identifier[:30]}", device_id, rssi)

        comp.rssi = rssi
        comp.last_seen = now
        comp.sensor_id = device_id
        if probed_ssids:
            comp.probed_ssids.update(probed_ssids)
            entity.probed_ssids.update(probed_ssids)
            # Index SSIDs for probe correlation
            for s in probed_ssids:
                if s and s != "(broadcast)":
                    self._ssid_index[s].add(entity_id)

        # Update label
        entity.label = self._generate_label(entity)
        self._dirty.add(entity_id)

        # Timeline: sensor handoff
        if prev_sensor and prev_sensor != device_id and entity.dominant_sensor == device_id:
            self._add_timeline(entity, now,
                f"Moved to {device_id}", device_id, rssi)

        # Periodic prune
        if now - self._last_prune > 30:
            self._prune(now)
            self._last_prune = now

    def _correlate_by_probes(self, identifier: str, probed_ssids: set,
                              rssi: int, sensor_id: str, manufacturer: str,
                              now: float, ie_hash: str | None = None) -> str | None:
        """Find an existing entity that matches this probe pattern."""
        if not probed_ssids:
            return None

        # Short-circuit: probe-IE hash is a chipset/driver fingerprint that
        # survives random-MAC rotation (PETS-2017). If we've seen this hash
        # before, its entity wins over any Jaccard computation.
        if ie_hash and self._identity_correlator is not None:
            try:
                cached_eid = self._identity_correlator.find_identity(ie_hash)
            except Exception:
                cached_eid = None
            if cached_eid:
                entity = self.entities.get(cached_eid)
                if entity and entity.is_active:
                    self._id_to_entity[identifier] = cached_eid
                    logger.info("IE-hash correlation: %s → entity %s (hash=%s)",
                                identifier[:20], cached_eid, ie_hash)
                    return cached_eid

        # Find entities that share probed SSIDs
        candidate_scores: dict[str, float] = {}
        for ssid in probed_ssids:
            if ssid in self._ssid_index:
                for eid in self._ssid_index[ssid]:
                    if eid not in candidate_scores:
                        candidate_scores[eid] = 0

        if not candidate_scores:
            return None

        best_id = None
        best_score = 0

        for eid, _ in candidate_scores.items():
            entity = self.entities.get(eid)
            if not entity or not entity.is_active:
                continue

            score = 0.0
            intersection_size = 0
            intersection = set()

            # 1. SSID Jaccard overlap
            if entity.probed_ssids:
                filtered_new = {s for s in probed_ssids if s != "(broadcast)"}
                filtered_ent = {s for s in entity.probed_ssids if s != "(broadcast)"}
                if filtered_new and filtered_ent:
                    intersection = filtered_new & filtered_ent
                    union = filtered_new | filtered_ent
                    intersection_size = len(intersection)
                    if intersection_size >= 2 and len(union) > 0:
                        jaccard = intersection_size / len(union)
                        score += jaccard * 55.0
                    elif intersection_size == 1 and len(union) > 0:
                        # Single SSID match still contributes — phones with only
                        # one home network in their preferred list should still
                        # link when co-located with a matching manufacturer.
                        score += (1.0 / len(union)) * 25.0

            # 2. Spatiotemporal co-location
            rssi_diff = None
            same_sensor = sensor_id in entity.sensors_active
            time_diff = abs(now - entity.last_seen)
            strong_co_location = False
            if time_diff < 15 and same_sensor:
                rssi_diff = abs(rssi - entity.current_rssi)
                if rssi_diff <= 10:
                    score += 35.0
                    strong_co_location = True
                elif rssi_diff <= 20:
                    score += 15.0

            # 3. Manufacturer match
            mfr_match = bool(manufacturer) and manufacturer in entity.manufacturers
            if mfr_match:
                score += 15.0

            # Fast-path: strong co-location + manufacturer match + any shared
            # SSID is a very specific coincidence — treat as a link even if it
            # doesn't clear the generic Jaccard-heavy threshold. This is the
            # common "same phone, MAC rotated, one home network" case.
            if strong_co_location and mfr_match and intersection_size >= 1:
                score = max(score, float(self.CORRELATION_THRESHOLD))

            if score > best_score:
                best_score = score
                best_id = eid

        if best_score >= self.CORRELATION_THRESHOLD:
            # Link this identifier to the matched entity
            self._id_to_entity[identifier] = best_id
            logger.info("Probe correlation: %s → entity %s (score=%.1f, ssids=%s)",
                        identifier[:20], best_id, best_score, probed_ssids)
            # Remember the IE-hash↔entity mapping so the next time we see
            # this chipset fingerprint (even after a MAC rotation) we can
            # short-circuit straight to this entity.
            if ie_hash and self._identity_correlator is not None:
                try:
                    self._identity_correlator.remember_identity(ie_hash, best_id)
                except Exception:
                    pass
            return best_id

        return None

    def _prune(self, now: float):
        """Remove stale entities and update active status."""
        stale = []
        # Lazily import at call-time to avoid circular import with the
        # event_detector singleton that lives in detections.py.
        _event_detector = None
        try:
            from app.routers.detections import _event_detector as _ed_singleton
            _event_detector = _ed_singleton
        except Exception:
            pass
        for eid, entity in self.entities.items():
            # Check if ALL sensors have lost contact
            if entity.sensors_active:
                latest = max(entity.sensors_active.values())
                idle = now - latest
            else:
                idle = now - entity.last_seen

            if idle >= self.GONE_TIMEOUT_S and entity.is_active:
                entity.is_active = False
                self._add_timeline(entity, now, "Departed (all sensors lost)", "", 0)
                # Fire a single device_departed event per visit. Identifier
                # is pinned on entity.first_seen so a re-visit produces a
                # fresh event (unique constraint lives at (type, identifier)).
                if _event_detector is not None:
                    try:
                        _event_detector.emit_departure(
                            eid,
                            first_seen=entity.first_seen,
                            last_seen=latest if entity.sensors_active else entity.last_seen,
                            sensor_ids=set(entity.sensors_active.keys()),
                            label=entity.label or "device",
                            best_rssi=entity.peak_rssi,
                        )
                    except Exception:
                        pass

            if idle >= self.STALE_TIMEOUT_S:
                stale.append(eid)

        for eid in stale:
            entity = self.entities.pop(eid, None)
            if entity:
                # Clean up indexes
                for comp_id in entity.components:
                    self._id_to_entity.pop(comp_id, None)
                for fp in entity.fingerprints:
                    self._fp_to_entity.pop(fp, None)
                for ssid in entity.probed_ssids:
                    self._ssid_index.get(ssid, set()).discard(eid)

    def _add_timeline(self, entity: TrackedEntity, ts: float,
                       event: str, sensor: str, rssi: int):
        entity.timeline.append(TimelineEvent(
            timestamp=ts, event=event, sensor=sensor, rssi=rssi
        ))
        if len(entity.timeline) > self.MAX_TIMELINE:
            entity.timeline.pop(0)

    def _generate_label(self, entity: TrackedEntity) -> str:
        """Generate a human-readable label for the entity."""
        types = set()
        for comp in entity.components.values():
            if comp.device_type and comp.device_type not in ("Unknown", "Transient", ""):
                types.add(comp.device_type)

        mfrs = entity.manufacturers - {"Unknown", ""}
        comp_count = len(entity.components)

        if types:
            type_str = ", ".join(sorted(types)[:3])
            if comp_count > len(types):
                return f"{type_str} + {comp_count - len(types)} more"
            return type_str
        elif mfrs:
            return f"{', '.join(sorted(mfrs)[:2])} ({comp_count} signals)"
        else:
            return f"Unknown ({comp_count} signals)"

    def get_entities(self, active_only: bool = True, limit: int = 50) -> list[dict]:
        """Return entity summary cards for the dashboard."""
        now = time.time()
        results = []

        for entity in sorted(self.entities.values(),
                              key=lambda e: e.last_seen, reverse=True):
            if active_only and not entity.is_active:
                continue

            # Compute sensor info
            active_sensors = []
            for sid, ts in entity.sensors_active.items():
                if now - ts < 120:
                    active_sensors.append(sid)

            # Determine RSSI trend from components
            trend = ""
            recent_rssi = [c.rssi for c in entity.components.values()
                           if now - c.last_seen < 30]
            if len(recent_rssi) >= 2:
                avg = sum(recent_rssi) / len(recent_rssi)
                if avg > entity.peak_rssi - 5:
                    trend = "approaching"
                elif entity.current_rssi < entity.peak_rssi - 15:
                    trend = "departing"
                else:
                    trend = "stationary"

            results.append({
                "entity_id": entity.entity_id,
                "label": entity.label,
                "category": entity.category,
                "status": "active" if entity.is_active else "departed",
                "first_seen": entity.first_seen,
                "last_seen": entity.last_seen,
                "duration_s": round(entity.last_seen - entity.first_seen),
                "rssi_trend": trend,
                "current_rssi": entity.current_rssi,
                "peak_rssi": entity.peak_rssi,
                "dominant_sensor": entity.dominant_sensor,
                "sensors_active": active_sensors,
                "sensor_count": len(active_sensors),
                "component_count": len(entity.components),
                "components": [
                    {
                        "type": c.id_type,
                        "identifier": c.identifier,
                        "device_type": c.device_type,
                        "manufacturer": c.manufacturer,
                        "rssi": c.rssi,
                        "probed_ssids": sorted(c.probed_ssids) if c.probed_ssids else [],
                    }
                    for c in entity.components.values()
                ],
                "probed_ssids": sorted(entity.probed_ssids),
                "timeline": [
                    {
                        "time": e.timestamp,
                        "event": e.event,
                        "sensor": e.sensor,
                        "rssi": e.rssi,
                    }
                    for e in entity.timeline[-20:]
                ],
            })

            if len(results) >= limit:
                break

        return results

    # ── Persistence ─────────────────────────────────────────────────────

    async def checkpoint(self, *_args, **_kwargs) -> int:
        """Write dirty entities to the tracked_entities table (debounced).

        Called from the detection ingest path; a 30-second debounce keeps
        this cheap even under high ingest rates. Returns the number of
        entities written.

        Uses its own short-lived AsyncSession — do NOT share the caller's
        session, because the router just committed it and any subsequent
        failure in this checkpoint would corrupt the router's unit of work.
        """
        now = time.time()
        if now - self._last_checkpoint < self.CHECKPOINT_DEBOUNCE_S:
            return 0
        if not self._dirty:
            self._last_checkpoint = now
            return 0

        try:
            from datetime import datetime, timezone
            from sqlalchemy import select
            from app.models.db_models import TrackedEntityRecord
            from app.services.database import async_session
        except Exception:
            return 0

        dirty_ids = list(self._dirty)
        self._dirty.clear()
        self._last_checkpoint = now

        written = 0
        try:
            async with async_session() as session:
                result = await session.execute(
                    select(TrackedEntityRecord).where(
                        TrackedEntityRecord.entity_id.in_(dirty_ids)
                    )
                )
                existing = {r.entity_id: r for r in result.scalars().all()}

                now_dt = datetime.now(timezone.utc)
                for eid in dirty_ids:
                    entity = self.entities.get(eid)
                    if not entity:
                        continue
                    payload = self._entity_to_payload(entity)
                    payload["updated_at"] = now_dt
                    row = existing.get(eid)
                    if row is None:
                        row = TrackedEntityRecord(entity_id=eid, **payload)
                        session.add(row)
                    else:
                        for key, value in payload.items():
                            setattr(row, key, value)
                    written += 1

                if written:
                    await session.commit()
        except Exception as e:
            logger.warning("EntityTracker checkpoint failed: %s", e)
            # Re-mark as dirty so the next checkpoint retries
            self._dirty.update(dirty_ids)
        return written

    def _entity_to_payload(self, entity: TrackedEntity) -> dict:
        components_blob = [
            {
                "id_type": c.id_type,
                "identifier": c.identifier,
                "device_type": c.device_type,
                "manufacturer": c.manufacturer,
                "probed_ssids": sorted(c.probed_ssids),
                "rssi": c.rssi,
                "last_seen": c.last_seen,
            }
            for c in entity.components.values()
        ]
        return {
            "label": entity.label,
            "category": entity.category,
            "first_seen": entity.first_seen,
            "last_seen": entity.last_seen,
            "fingerprints_json": json.dumps(sorted(entity.fingerprints)),
            "identifiers_json": json.dumps(sorted(entity.components.keys())),
            "manufacturers_json": json.dumps(sorted(entity.manufacturers)),
            "probed_ssids_json": json.dumps(sorted(entity.probed_ssids)),
            "components_json": json.dumps(components_blob),
            "peak_rssi": entity.peak_rssi,
        }

    async def rehydrate_from_db(self, db, window_s: float | None = None) -> int:
        """Load entities from the last ``window_s`` seconds into memory.

        Called from the FastAPI lifespan on startup. Rebuilds the identifier /
        fingerprint / SSID indexes so that previously-seen devices are not
        re-created as brand-new entities after a restart.
        """
        try:
            from sqlalchemy import select
            from app.models.db_models import TrackedEntityRecord
        except Exception:
            return 0

        now = time.time()
        cutoff = now - (window_s or self.REHYDRATE_WINDOW_S)
        try:
            result = await db.execute(
                select(TrackedEntityRecord).where(
                    TrackedEntityRecord.last_seen >= cutoff
                )
            )
            rows = result.scalars().all()
        except Exception as e:
            logger.warning("EntityTracker rehydrate query failed: %s", e)
            return 0

        loaded = 0
        for row in rows:
            try:
                entity = TrackedEntity(
                    entity_id=row.entity_id,
                    label=row.label or "Unknown",
                    category=row.category or "visitor",
                    first_seen=row.first_seen or now,
                    last_seen=row.last_seen or now,
                    is_active=False,  # mark inactive until a new detection arrives
                )
                entity.peak_rssi = row.peak_rssi if row.peak_rssi is not None else -100
                entity.fingerprints = set(json.loads(row.fingerprints_json or "[]"))
                entity.manufacturers = set(json.loads(row.manufacturers_json or "[]"))
                entity.probed_ssids = set(json.loads(row.probed_ssids_json or "[]"))
                identifiers = json.loads(row.identifiers_json or "[]")
                for comp in json.loads(row.components_json or "[]"):
                    c = EntityComponent(
                        id_type=comp.get("id_type", "unknown"),
                        identifier=comp.get("identifier", ""),
                        device_type=comp.get("device_type", ""),
                        manufacturer=comp.get("manufacturer", ""),
                        rssi=comp.get("rssi", -100),
                        probed_ssids=set(comp.get("probed_ssids", [])),
                        last_seen=comp.get("last_seen", 0.0),
                    )
                    if c.identifier:
                        entity.components[c.identifier] = c
                self.entities[row.entity_id] = entity

                # Rebuild indexes
                for ident in identifiers:
                    if ident:
                        self._id_to_entity[ident] = row.entity_id
                for fp in entity.fingerprints:
                    self._fp_to_entity[fp] = row.entity_id
                for ssid in entity.probed_ssids:
                    self._ssid_index[ssid].add(row.entity_id)
                loaded += 1
            except Exception as e:
                logger.debug("Skipping malformed entity row %s: %s", row.entity_id, e)

        if loaded:
            logger.info("EntityTracker rehydrated %d entities from last %.0fh",
                        loaded, (window_s or self.REHYDRATE_WINDOW_S) / 3600)
        return loaded

    def get_stats(self) -> dict:
        now = time.time()
        active = sum(1 for e in self.entities.values() if e.is_active)
        total_components = sum(len(e.components) for e in self.entities.values())
        recent = sum(1 for e in self.entities.values()
                     if now - e.last_seen < 300)
        return {
            "total_entities": len(self.entities),
            "active_entities": active,
            "recent_5min": recent,
            "total_components": total_components,
            "indexed_ssids": len(self._ssid_index),
        }


def _is_random_mac(mac: str) -> bool:
    """Check if MAC has the locally administered bit set (random/virtual)."""
    if not mac or len(mac) < 2:
        return False
    try:
        first_byte = int(mac.split(":")[0], 16)
        return (first_byte & 0x02) != 0
    except (ValueError, IndexError):
        return False


def _extract_mac(drone_id: str) -> str:
    """Extract MAC from probe_XX:XX:XX:XX:XX:XX format."""
    if drone_id and drone_id.startswith("probe_"):
        return drone_id[6:]
    return ""


def _source_to_type(source: str) -> str:
    if "ble" in source:
        return "ble"
    if "probe" in source:
        return "wifi_probe"
    if "oui" in source or "ssid" in source:
        return "wifi_ap"
    return "unknown"


def _extract_device_type(drone_id: str) -> str:
    if not drone_id:
        return ""
    parts = drone_id.split(":")
    if len(parts) >= 3:
        return ":".join(parts[2:]).strip()
    return ""
