"""Stable WiFi probe identity helpers.

Shared across triangulation, eventing, and API presentation so probe
devices are grouped the same way everywhere.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class ProbeIdentity:
    identity: str
    ie_hash: str | None
    mac: str | None


def probe_mac_from_detection(drone_id: str | None,
                             bssid: str | None) -> str | None:
    """Extract a probe requester MAC from the detection payload."""
    if bssid:
        return bssid.strip().upper()
    if drone_id:
        did = drone_id.strip()
        if did.startswith("probe_"):
            return did[6:].upper()
        if did.startswith("PROBE:"):
            tail = did[6:].upper()
            return tail if _looks_like_mac(tail) else None
    return None


def normalize_probe_identity(ie_hash: str | None,
                             drone_id: str | None = None,
                             bssid: str | None = None) -> ProbeIdentity:
    """Return the stable grouping identity for a probe request."""
    norm_ie_hash = ie_hash.strip().upper() if ie_hash else None
    mac = probe_mac_from_detection(drone_id, bssid)
    if norm_ie_hash:
        return ProbeIdentity(identity=f"PROBE:{norm_ie_hash}", ie_hash=norm_ie_hash, mac=mac)
    if mac:
        return ProbeIdentity(identity=f"PROBE:{mac}", ie_hash=None, mac=mac)
    fallback = (drone_id or "PROBE:UNKNOWN").strip()
    return ProbeIdentity(identity=fallback, ie_hash=None, mac=mac)


def probe_identity_from_event(event_type: str,
                              identifier: str,
                              metadata: dict | None = None) -> str | None:
    """Recover a stable probe identity from an event row."""
    md = metadata or {}
    probe_identity = md.get("probe_identity")
    if isinstance(probe_identity, str) and probe_identity:
        return probe_identity
    if event_type == "new_probe_identity":
        return identifier
    if event_type == "new_probe_mac":
        mac = identifier.strip().upper()
        return f"PROBE:{mac}" if mac else None
    if event_type == "probe_activity_spike":
        return identifier.split("@", 1)[0]
    return None


def mac_from_probe_identity(identity: str | None) -> str | None:
    """Return the MAC component for MAC-backed identities."""
    if not identity:
        return None
    value = identity.strip().upper()
    if not value.startswith("PROBE:"):
        return None
    tail = value[6:]
    return tail if _looks_like_mac(tail) else None


def _looks_like_mac(value: str | None) -> bool:
    if not value:
        return False
    parts = value.split(":")
    return len(parts) == 6 and all(len(part) == 2 for part in parts)
