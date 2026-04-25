"""RF identity truth, evidence, and cautious relation helpers.

This module is intentionally conservative: randomized MACs are anonymous,
but stable RF fingerprints can still be correlated with explicit evidence.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from app.services.ble_company_lookup import lookup_company
from app.services.oui_db import is_random_mac, oui_lookup


@dataclass(frozen=True)
class MacIdentity:
    mac_is_randomized: bool
    mac_identity_kind: str
    mac_reason: str


def classify_mac_identity(
    mac: str | None,
    *,
    source: str | None = None,
    ble_addr_type: int | None = None,
) -> MacIdentity:
    source_l = (source or "").lower()
    if source_l.startswith("ble"):
        if ble_addr_type == 2:
            return MacIdentity(True, "ble_rpa", "ble_addr_type_rpa")
        if ble_addr_type in (1, 3):
            return MacIdentity(True, "ble_random_static", "ble_random_address_type")
        if mac and is_random_mac(mac):
            return MacIdentity(True, "randomized", "locally_administered_bit")
        if mac:
            return MacIdentity(False, "public_oui", "public_oui")
        return MacIdentity(False, "unknown", "missing_mac")

    if mac and is_random_mac(mac):
        return MacIdentity(True, "randomized", "locally_administered_bit")
    if mac:
        return MacIdentity(False, "public_oui", "public_oui")
    return MacIdentity(False, "unknown", "missing_mac")


def _mac_from_drone_id(drone_id: str | None) -> str | None:
    if drone_id and drone_id.startswith("probe_"):
        return drone_id[6:]
    return None


def _best_mac(*, bssid: str | None, drone_id: str | None) -> str | None:
    return bssid or _mac_from_drone_id(drone_id)


def _device_class_from_name(name: str | None, reason: str | None) -> tuple[str | None, float | None]:
    name_l = (name or "").lower()
    reason_l = (reason or "").lower()
    if reason_l == "explicit_camera_ble_name" or any(
        token in name_l for token in ("hidvcam", "hdwificam", "v380", "lookcam")
    ):
        return "suspect_camera", 0.75
    if reason_l == "ambiguous_iot_ble_name" or any(
        token in name_l for token in ("elk-bledom", "qhm-", "melk-", "bt_bpm")
    ):
        return "suspect_iot", 0.35
    return None, None


def _scanner_label_is_class(label: str) -> bool:
    label_l = label.lower()
    return any(token in label_l for token in (
        "hidden camera", "card skimmer", "flock surveillance",
        "tracker", "drone controller", "flipper zero",
    ))


def identity_source_for_detection(
    *,
    source: str | None,
    ie_hash: str | None = None,
    ble_ja3: str | None = None,
    ble_company_id: int | None = None,
    ble_name: str | None = None,
    ssid: str | None = None,
    bssid: str | None = None,
) -> str:
    source_l = (source or "").lower()
    if source_l == "wifi_probe_request" and ie_hash:
        return "probe_ie_hash"
    if source_l.startswith("ble") and ble_ja3:
        return "ble_ja3"
    if source_l.startswith("ble") and ble_company_id:
        return "ble_company_id"
    if source_l.startswith("ble") and ble_name:
        return "ble_name"
    if ssid and source_l in ("wifi_ssid", "wifi_ap_inventory"):
        return "ssid_pattern"
    if bssid:
        return "mac"
    return "unknown"


def enrich_rf_evidence(
    *,
    source: str | None,
    drone_id: str | None = None,
    bssid: str | None = None,
    ssid: str | None = None,
    manufacturer: str | None = None,
    model: str | None = None,
    classification: str | None = None,
    probed_ssids: list[str] | None = None,
    ie_hash: str | None = None,
    ble_company_id: int | None = None,
    ble_addr_type: int | None = None,
    ble_ja3: str | None = None,
    ble_svc_uuids: str | None = None,
    ble_name: str | None = None,
    class_reason: str | None = None,
) -> dict[str, Any]:
    mac = _best_mac(bssid=bssid, drone_id=drone_id)
    mac_meta = classify_mac_identity(mac, source=source, ble_addr_type=ble_addr_type)
    source_l = (source or "").lower()
    evidence: list[str] = []

    brand: str | None = None
    brand_source: str | None = None
    brand_confidence: float | None = None

    if ble_company_id:
        name, category = lookup_company(ble_company_id)
        if name and name != "Unknown":
            brand = name
            brand_source = "ble_company_id"
            brand_confidence = 0.90
            evidence.append(f"BLE company ID 0x{ble_company_id:04X}: {name}")
            if category:
                evidence.append(f"BLE category hint: {category}")

    if not brand and mac and not mac_meta.mac_is_randomized:
        oui = oui_lookup(mac)
        if oui and oui[0] != "Randomized MAC":
            brand = oui[0]
            brand_source = "oui"
            brand_confidence = 0.85
            evidence.append(f"Public OUI: {oui[0]}")

    mfr = (manufacturer or "").strip()
    if not brand and mfr and mfr.lower() not in ("unknown", "randomized mac") and not _scanner_label_is_class(mfr):
        brand = mfr
        brand_source = "scanner_label"
        brand_confidence = 0.55
        evidence.append(f"Scanner label: {mfr}")
    elif mfr and _scanner_label_is_class(mfr):
        evidence.append(f"Scanner class label: {mfr}")

    device_class, device_class_confidence = _device_class_from_name(ble_name, class_reason)
    if not device_class:
        if "hidden camera" in mfr.lower():
            device_class = "suspect_camera"
            device_class_confidence = 0.45
        elif "card skimmer" in mfr.lower():
            device_class = "suspect_skimmer"
            device_class_confidence = 0.70
        elif "flipper zero" in mfr.lower():
            device_class = "hostile_tool"
            device_class_confidence = 0.80
        elif "flock surveillance" in mfr.lower():
            device_class = "surveillance_camera"
            device_class_confidence = 0.75
    if not device_class:
        cls = (classification or "").lower()
        if cls in ("tracker", "hostile_tool", "mobile_hotspot"):
            device_class = cls
            device_class_confidence = 0.75
        elif source_l == "wifi_probe_request":
            device_class = "wifi_device"
            device_class_confidence = 0.45
        elif source_l == "wifi_ap_inventory":
            device_class = "wifi_ap"
            device_class_confidence = 0.60
        elif source_l == "ble_fingerprint":
            device_class = "ble_device"
            device_class_confidence = 0.40

    if mac_meta.mac_is_randomized:
        evidence.append(mac_meta.mac_reason)
    if ie_hash:
        evidence.append(f"Probe IE hash: {ie_hash}")
    if probed_ssids:
        visible = [s for s in probed_ssids if s and s != "(broadcast)"]
        if visible:
            evidence.append("Probed SSIDs: " + ", ".join(visible[:5]))
    if ble_ja3:
        evidence.append(f"BLE-JA3: {ble_ja3}")
    if ble_svc_uuids:
        evidence.append(f"BLE services: {ble_svc_uuids}")
    if ble_name:
        evidence.append(f"BLE local name: {ble_name}")
    if class_reason:
        evidence.append(f"Classifier reason: {class_reason}")
    if ssid and source_l in ("wifi_ssid", "wifi_ap_inventory"):
        evidence.append(f"SSID: {ssid}")
    if not brand and mac_meta.mac_is_randomized:
        brand_source = "none_randomized_mac"
        brand_confidence = 0.0

    return {
        "mac_is_randomized": mac_meta.mac_is_randomized,
        "mac_identity_kind": mac_meta.mac_identity_kind,
        "mac_reason": mac_meta.mac_reason,
        "brand": brand,
        "brand_source": brand_source,
        "brand_confidence": brand_confidence,
        "device_class": device_class,
        "device_class_confidence": device_class_confidence,
        "identity_source": identity_source_for_detection(
            source=source,
            ie_hash=ie_hash,
            ble_ja3=ble_ja3,
            ble_company_id=ble_company_id,
            ble_name=ble_name,
            ssid=ssid,
            bssid=mac,
        ),
        "evidence": evidence,
    }


def probe_relation_hints(identity: str, macs: list[str], ie_hash: str | None) -> list[dict[str, Any]]:
    if not ie_hash or len(macs) < 2:
        return []
    return [
        {
            "entity_id": f"MAC:{mac}",
            "relation_type": "likely_same_device",
            "confidence": 0.90,
            "reason": f"same_probe_ie_hash:{ie_hash}",
        }
        for mac in macs
        if mac
    ] + [{
        "entity_id": identity,
        "relation_type": "likely_same_device",
        "confidence": 0.95,
        "reason": "rotating_macs_share_stable_probe_ie_hash",
    }]
