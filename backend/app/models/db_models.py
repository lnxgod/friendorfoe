"""SQLAlchemy ORM models for persistent drone detection storage."""

from datetime import datetime, timezone

from sqlalchemy import Boolean, DateTime, Float, Index, Integer, String, Text, func
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
