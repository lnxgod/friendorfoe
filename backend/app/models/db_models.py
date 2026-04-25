"""SQLAlchemy ORM models for persistent drone detection storage."""

from datetime import datetime, timezone

from sqlalchemy import (
    Boolean, DateTime, Float, Index, Integer, String, Text, UniqueConstraint, func,
)
from sqlalchemy.orm import Mapped, mapped_column

from app.services.database import Base


class SensorNode(Base):
    """A registered ESP32 sensor node with a known or dynamic position."""

    __tablename__ = "sensor_nodes"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    device_id: Mapped[str] = mapped_column(String(64), unique=True, nullable=False, index=True)
    name: Mapped[str] = mapped_column(String(128), nullable=False, default="")
    lat: Mapped[float] = mapped_column(Float, nullable=False)
    lon: Mapped[float] = mapped_column(Float, nullable=False)
    alt: Mapped[float | None] = mapped_column(Float, nullable=True)
    is_fixed: Mapped[bool] = mapped_column(Boolean, nullable=False, default=False)
    sensor_type: Mapped[str] = mapped_column(String(16), nullable=False, default="outdoor")
    position_mode: Mapped[str] = mapped_column(String(16), nullable=False, default="active")
    last_seen: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), nullable=False, server_default=func.now()
    )
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), nullable=False, server_default=func.now()
    )


class DroneDetection(Base):
    """A single drone detection from an ESP32 sensor, persisted to PostgreSQL."""

    __tablename__ = "drone_detections"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    device_id: Mapped[str] = mapped_column(String(64), nullable=False, index=True)
    drone_id: Mapped[str] = mapped_column(String(128), nullable=False, index=True)
    source: Mapped[str] = mapped_column(String(32), nullable=False)
    ssid: Mapped[str | None] = mapped_column(String(64), nullable=True)
    bssid: Mapped[str | None] = mapped_column(String(20), nullable=True)
    rssi: Mapped[int | None] = mapped_column(Integer, nullable=True)
    confidence: Mapped[float] = mapped_column(Float, nullable=False, default=0.0)
    fused_confidence: Mapped[float | None] = mapped_column(Float, nullable=True)
    drone_lat: Mapped[float | None] = mapped_column(Float, nullable=True)
    drone_lon: Mapped[float | None] = mapped_column(Float, nullable=True)
    drone_alt: Mapped[float | None] = mapped_column(Float, nullable=True)
    speed_mps: Mapped[float | None] = mapped_column(Float, nullable=True)
    heading_deg: Mapped[float | None] = mapped_column(Float, nullable=True)
    manufacturer: Mapped[str | None] = mapped_column(String(64), nullable=True)
    model: Mapped[str | None] = mapped_column(String(64), nullable=True)
    mac_is_randomized: Mapped[bool | None] = mapped_column(Boolean, nullable=True)
    mac_identity_kind: Mapped[str | None] = mapped_column(String(32), nullable=True)
    mac_reason: Mapped[str | None] = mapped_column(String(64), nullable=True)
    brand: Mapped[str | None] = mapped_column(String(128), nullable=True)
    brand_source: Mapped[str | None] = mapped_column(String(48), nullable=True)
    brand_confidence: Mapped[float | None] = mapped_column(Float, nullable=True)
    device_class: Mapped[str | None] = mapped_column(String(64), nullable=True)
    device_class_confidence: Mapped[float | None] = mapped_column(Float, nullable=True)
    identity_source: Mapped[str | None] = mapped_column(String(48), nullable=True)
    evidence_json: Mapped[str | None] = mapped_column(Text, nullable=True)
    operator_lat: Mapped[float | None] = mapped_column(Float, nullable=True)
    operator_lon: Mapped[float | None] = mapped_column(Float, nullable=True)
    operator_id: Mapped[str | None] = mapped_column(String(64), nullable=True)
    sensor_lat: Mapped[float | None] = mapped_column(Float, nullable=True)
    sensor_lon: Mapped[float | None] = mapped_column(Float, nullable=True)
    sensor_alt: Mapped[float | None] = mapped_column(Float, nullable=True)
    classification: Mapped[str | None] = mapped_column(String(32), nullable=True, index=True)
    probed_ssids: Mapped[str | None] = mapped_column(Text, nullable=True)  # JSON array
    timestamp: Mapped[int] = mapped_column(Integer, nullable=False, index=True)
    received_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), nullable=False, server_default=func.now()
    )


class TriangulatedPosition(Base):
    """A triangulated drone position computed from multiple sensor observations."""

    __tablename__ = "triangulated_positions"
    __table_args__ = (
        Index("idx_tri_drone_time", "drone_id", "created_at"),
    )

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    drone_id: Mapped[str] = mapped_column(String(128), nullable=False, index=True)
    lat: Mapped[float] = mapped_column(Float, nullable=False)
    lon: Mapped[float] = mapped_column(Float, nullable=False)
    alt: Mapped[float | None] = mapped_column(Float, nullable=True)
    accuracy_m: Mapped[float | None] = mapped_column(Float, nullable=True)
    position_source: Mapped[str] = mapped_column(String(32), nullable=False)
    sensor_count: Mapped[int] = mapped_column(Integer, nullable=False, default=1)
    confidence: Mapped[float] = mapped_column(Float, nullable=False, default=0.0)
    manufacturer: Mapped[str | None] = mapped_column(String(64), nullable=True)
    model: Mapped[str | None] = mapped_column(String(64), nullable=True)
    ssid: Mapped[str | None] = mapped_column(String(64), nullable=True)
    classification: Mapped[str | None] = mapped_column(String(32), nullable=True)
    observations_json: Mapped[str | None] = mapped_column(Text, nullable=True)
    timestamp: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), nullable=False, server_default=func.now()
    )
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), nullable=False, server_default=func.now()
    )


# ── Persistent BLE/device tracking ───────────────────────────────────────


class KnownDevice(Base):
    """Persistent BLE/WiFi device record — survives restarts."""

    __tablename__ = "known_devices"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    fingerprint: Mapped[str] = mapped_column(String(16), unique=True, nullable=False, index=True)
    device_type: Mapped[str] = mapped_column(String(32), nullable=False, default="Unknown")
    manufacturer: Mapped[str] = mapped_column(String(64), nullable=False, default="Unknown")
    category: Mapped[str] = mapped_column(String(32), nullable=False, default="Unknown")
    is_tracker: Mapped[bool] = mapped_column(Boolean, nullable=False, default=False)
    total_detections: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    total_sensors: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    mac_rotations: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    best_rssi: Mapped[int | None] = mapped_column(Integer, nullable=True)
    avg_rssi: Mapped[float | None] = mapped_column(Float, nullable=True)
    first_seen: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), nullable=False, server_default=func.now()
    )
    last_seen: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), nullable=False, server_default=func.now()
    )
    last_bssid: Mapped[str | None] = mapped_column(String(20), nullable=True)
    notes: Mapped[str | None] = mapped_column(Text, nullable=True)


class LearnedProfile(Base):
    """Learned behavioral profile — maps device signatures to categories."""

    __tablename__ = "learned_profiles"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    signature: Mapped[str] = mapped_column(String(128), unique=True, nullable=False, index=True)
    category: Mapped[str] = mapped_column(String(32), nullable=False)
    count: Mapped[int] = mapped_column(Integer, nullable=False, default=1)
    confidence: Mapped[float] = mapped_column(Float, nullable=False, default=0.5)
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), nullable=False, server_default=func.now()
    )


class OuiEntry(Base):
    """MAC OUI prefix to manufacturer lookup — expandable via API."""

    __tablename__ = "oui_database"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    prefix: Mapped[str] = mapped_column(String(10), unique=True, nullable=False, index=True)
    manufacturer: Mapped[str] = mapped_column(String(128), nullable=False)
    added_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), nullable=False, server_default=func.now()
    )


class TrackedEntityRecord(Base):
    """Persistent snapshot of an EntityTracker entity — survives restart.

    EntityTracker holds its correlation indexes in memory; on restart those
    are empty and previously-seen devices reappear as brand-new entities,
    inflating the "unique entity" count on the dashboard. This table lets
    the tracker rehydrate its indexes from the last 24 h.
    """

    __tablename__ = "tracked_entities"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    entity_id: Mapped[str] = mapped_column(String(32), unique=True, nullable=False, index=True)
    label: Mapped[str] = mapped_column(String(128), nullable=False, default="Unknown")
    category: Mapped[str] = mapped_column(String(32), nullable=False, default="visitor")
    first_seen: Mapped[float] = mapped_column(Float, nullable=False, default=0.0)
    last_seen: Mapped[float] = mapped_column(Float, nullable=False, default=0.0, index=True)
    # Serialized JSON blobs — set-like collections stored as arrays for compactness.
    fingerprints_json: Mapped[str | None] = mapped_column(Text, nullable=True)
    identifiers_json: Mapped[str | None] = mapped_column(Text, nullable=True)
    manufacturers_json: Mapped[str | None] = mapped_column(Text, nullable=True)
    probed_ssids_json: Mapped[str | None] = mapped_column(Text, nullable=True)
    components_json: Mapped[str | None] = mapped_column(Text, nullable=True)
    peak_rssi: Mapped[int | None] = mapped_column(Integer, nullable=True)
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), nullable=False,
        server_default=func.now(), onupdate=func.now(),
    )


class WhitelistedSSID(Base):
    """SSID whitelist — marks networks as friendly/known (not drones).

    Patterns use glob syntax: CasaChomp* matches CasaChomp_2g, CasaChomp_5g, etc.
    Managed via dashboard UI.
    """

    __tablename__ = "whitelisted_ssids"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    pattern: Mapped[str] = mapped_column(String(64), unique=True, nullable=False, index=True)
    label: Mapped[str | None] = mapped_column(String(64), nullable=True)  # "Home WiFi", "Neighbor", etc.
    added_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), nullable=False, server_default=func.now()
    )


class WhitelistedMAC(Base):
    """MAC-prefix whitelist — suppresses new_probe_mac and related events for
    known-friendly devices (user's own phone, laptop, etc.).

    Entries may be a full 17-character MAC (AA:BB:CC:DD:EE:FF) or an OUI
    prefix (first 8 chars, e.g. "AA:BB:CC"). Matching is case-insensitive
    prefix; both forms compared in upper-case.
    """

    __tablename__ = "whitelisted_macs"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    mac: Mapped[str] = mapped_column(String(20), unique=True, nullable=False, index=True)
    label: Mapped[str | None] = mapped_column(String(64), nullable=True)
    added_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), nullable=False, server_default=func.now()
    )


class IdentityLink(Base):
    """Cross-layer identity link — couples a BLE fingerprint with the WiFi
    MAC / probe-IE hash of what we believe is the SAME physical device.

    Populated by services/identity_correlator.py when it sees a BLE FP and
    a WiFi observation on the same sensor within a short time window with
    similar RSSI. On subsequent observations of either key, the partner is
    looked up and the entities are merged in EntityTracker.
    """

    __tablename__ = "identity_links"
    __table_args__ = (
        UniqueConstraint("ble_fp", "wifi_key", name="uq_ident_ble_wifi"),
        Index("idx_ident_ble", "ble_fp"),
        Index("idx_ident_wifi", "wifi_key"),
    )

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    ble_fp: Mapped[str] = mapped_column(String(16), nullable=False)        # "FP:xxxxxxxx"
    wifi_key: Mapped[str] = mapped_column(String(40), nullable=False)      # BSSID or "ieh:<hex>" probe-IE hash
    sensor_id: Mapped[str] = mapped_column(String(64), nullable=False)
    first_seen_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), nullable=False, server_default=func.now()
    )
    last_seen_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), nullable=False, server_default=func.now()
    )
    confirm_count: Mapped[int] = mapped_column(Integer, nullable=False, default=1)
    best_rssi_delta: Mapped[int | None] = mapped_column(Integer, nullable=True)


class Event(Base):
    """Persistent first-seen event log.

    Each row fires once per unique (event_type, identifier). Subsequent
    sightings update last_seen_at + sighting_count via a debounced path
    rather than creating duplicate rows. See services/event_detector.py for
    debounce rules and the in-memory seen-cache.
    """

    __tablename__ = "events"
    __table_args__ = (
        UniqueConstraint("event_type", "identifier", name="uq_event_type_identifier"),
        Index("idx_event_type_time", "event_type", "first_seen_at"),
        Index("idx_event_ack_time", "acknowledged", "first_seen_at"),
    )

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    event_type: Mapped[str] = mapped_column(String(32), nullable=False, index=True)
    identifier: Mapped[str] = mapped_column(String(128), nullable=False)
    severity: Mapped[str] = mapped_column(String(16), nullable=False, default="info")
    title: Mapped[str] = mapped_column(String(128), nullable=False, default="")
    message: Mapped[str] = mapped_column(String(512), nullable=False, default="")
    first_seen_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), nullable=False, index=True
    )
    last_seen_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), nullable=False,
    )
    sighting_count: Mapped[int] = mapped_column(Integer, nullable=False, default=1)
    sensor_count: Mapped[int] = mapped_column(Integer, nullable=False, default=1)
    sensor_ids_json: Mapped[str | None] = mapped_column(Text, nullable=True)
    best_rssi: Mapped[int | None] = mapped_column(Integer, nullable=True)
    # Rich context frozen at creation time — manufacturer, device_type,
    # probed_ssids, lat/lon, related drone_id, fingerprint, etc.
    metadata_json: Mapped[str | None] = mapped_column(Text, nullable=True)
    acknowledged: Mapped[bool] = mapped_column(
        Boolean, nullable=False, default=False, index=True
    )
    acknowledged_at: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), nullable=False, server_default=func.now()
    )
