"""WiFi probe/AP fingerprint evidence helpers.

Firmware already gives us some stable pieces such as IE hash, auth mode,
channel, SSID/probed SSID lists, and vendor labels. This helper packages those
into one v2 evidence object without inventing unavailable low-level IE fields.
"""

from __future__ import annotations

import hashlib
import json
from typing import Any

AUTH_MODE_NAMES = {
    0: "open",
    1: "wep",
    2: "wpa_psk",
    3: "wpa2_psk",
    4: "wpa_wpa2_psk",
    5: "wpa2_enterprise",
    6: "wpa3_psk",
    7: "wpa2_wpa3_psk",
    8: "wapi",
    9: "owe",
    10: "wpa3_enterprise_192",
}


def _scanner_label_is_protocol(label: str | None) -> bool:
    if not label:
        return False
    label_l = label.strip().lower()
    return label_l in {
        "wifi-assoc", "wifi assoc",
        "wifi-oui", "wifi oui",
        "wifi-ssid", "wifi ssid",
        "probe request", "wifi-probe",
    }


def build_wifi_fingerprint_v2(
    *,
    source: str | None,
    ie_hash: str | None = None,
    probed_ssids: list[str] | None = None,
    ssid: str | None = None,
    bssid: str | None = None,
    channel: int | None = None,
    auth_m: int | None = None,
    manufacturer: str | None = None,
    model: str | None = None,
) -> dict[str, Any] | None:
    source_l = (source or "").lower()
    if source_l not in {"wifi_probe_request", "wifi_ap_inventory", "wifi_assoc", "wifi_oui", "wifi_ssid"}:
        return None

    evidence: list[str] = []
    auth_name = AUTH_MODE_NAMES.get(auth_m) if auth_m is not None else None
    if ie_hash:
        evidence.append(f"Stable probe IE hash: {ie_hash}")
    if probed_ssids:
        visible = [s for s in probed_ssids if s and s != "(broadcast)"]
        if visible:
            evidence.append(f"Probed SSID count: {len(visible)}")
    if ssid:
        evidence.append(f"SSID evidence: {ssid}")
    if auth_name:
        evidence.append(f"Auth mode: {auth_name}")
    if channel is not None:
        evidence.append(f"Channel {channel} ({_band_for_channel(channel)})")
    if manufacturer and not _scanner_label_is_protocol(manufacturer):
        evidence.append(f"Scanner/AP vendor label: {manufacturer}")
    if model:
        evidence.append(f"Model label: {model}")

    basis = {
        "source": source_l,
        "ie_hash": ie_hash,
        "probed_ssids": sorted(set(probed_ssids or [])),
        "ssid": ssid,
        "auth_m": auth_m,
        "channel": channel,
        "manufacturer": manufacturer,
        "model": model,
    }
    digest = hashlib.sha256(json.dumps(basis, sort_keys=True).encode()).hexdigest()[:16]
    stable_id = f"IEH:{ie_hash}" if ie_hash else f"WF2:{digest}"
    confidence = 0.82 if ie_hash else (0.58 if ssid or auth_name or channel is not None else 0.35)

    return {
        "version": 2,
        "source": "wifi_fingerprint_v2",
        "stable_id": stable_id,
        "identity_basis": "ie_hash" if ie_hash else "observed_wifi_metadata",
        "ie_hash": ie_hash,
        "auth_mode": auth_name,
        "channel": channel,
        "band": _band_for_channel(channel),
        "wifi_generation_hint": _generation_hint(channel=channel, auth_m=auth_m),
        "probed_ssid_count": len([s for s in probed_ssids or [] if s and s != "(broadcast)"]),
        "ssid_present": bool(ssid),
        "confidence": round(confidence, 2),
        "evidence": evidence,
    }


def _band_for_channel(channel: int | None) -> str | None:
    if channel is None:
        return None
    # Some firmware paths currently report center frequency MHz instead of
    # IEEE channel number. Treat both forms as evidence.
    if 2400 <= int(channel) <= 2500:
        return "2.4GHz"
    if 4900 <= int(channel) <= 5900:
        return "5GHz"
    if 5925 <= int(channel) <= 7125:
        return "6GHz"
    if 1 <= int(channel) <= 14:
        return "2.4GHz"
    if 32 <= int(channel) <= 177:
        return "5GHz"
    if 1 <= int(channel) <= 233:
        return "6GHz"
    return "unknown"


def _generation_hint(*, channel: int | None, auth_m: int | None) -> str | None:
    if auth_m in (6, 7, 9, 10):
        return "WiFi 6/6E security capable"
    if channel is not None and 5925 <= int(channel) <= 7125:
        return "WiFi 6E/7 band evidence"
    if channel is not None and 4900 <= int(channel) <= 5900:
        return "5GHz-capable"
    if channel is not None and 2400 <= int(channel) <= 2500:
        return "2.4GHz-observed"
    if channel is not None and int(channel) > 177:
        return "WiFi 6E/7 band evidence"
    if channel is not None and int(channel) >= 32:
        return "5GHz-capable"
    if channel is not None:
        return "2.4GHz-observed"
    return None
