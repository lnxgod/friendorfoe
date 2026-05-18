"""RF identity truth, evidence, and cautious relation helpers.

This module is intentionally conservative: randomized MACs are anonymous,
but stable RF fingerprints can still be correlated with explicit evidence.
"""

from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
from fnmatch import fnmatchcase
from typing import Any

from app.services.apple_continuity import decode_apple_continuity
from app.services.ble_company_lookup import lookup_company as _legacy_lookup_company
from app.services.drone_signature_reference import drone_wifi_ssid_matches
from app.services.oui_db import is_random_mac
from app.services.rf_reference import lookup_ble_company, resolve_mac, source_details, ssid_pattern_hints
from app.services.wifi_fingerprint import build_wifi_fingerprint_v2


_EXPLICIT_CAMERA_NAME_TOKENS = (
    "hidvcam",
    "hdwificam",
    "v380",
    "lookcam",
    "ycc365",
    "icsee",
    "ubox",
    "camhi",
    "vstarcam",
    "fredi",
    "minicam",
    "p2plivecam",
)

_AMBIGUOUS_IOT_NAME_TOKENS = (
    "elk-bledom",
    "qhm-",
    "melk-",
    "bt_bpm",
)

_KNOWN_NETWORK_PATTERNS = (
    {
        "pattern": "teamcharitycase*",
        "label": "TeamCharityCase lab/property",
        "device_class": "known_owned",
        "confidence": 0.75,
        "device_family": "operator_known_lab_device",
    },
)

_FRIENDLY_OUI_HINTS = {
    # Product-specific labels where the IEEE registrant is too generic for
    # operator triage. These are still MAC/OUI evidence, not person identity.
    "90:48:9A": {"brand": "Ring", "device_family": "camera_or_video", "device_class": "surveillance_camera"},
    "00:40:40": {"brand": "Ring", "device_family": "camera_or_video", "device_class": "surveillance_camera"},
    "18:7F:88": {"brand": "Ring", "device_family": "camera_or_video", "device_class": "surveillance_camera"},
    "34:3E:A4": {"brand": "Ring", "device_family": "camera_or_video", "device_class": "surveillance_camera"},
    "54:E0:19": {"brand": "Ring", "device_family": "camera_or_video", "device_class": "surveillance_camera"},
    "3C:E1:A1": {"brand": "Ring Camera", "device_family": "camera_or_video", "device_class": "surveillance_camera"},
    "88:6A:B1": {"brand": "Ring Camera", "device_family": "camera_or_video", "device_class": "surveillance_camera"},
    "2C:AA:8E": {"brand": "Wyze", "device_family": "camera_or_video", "device_class": "surveillance_camera"},
    "7C:78:3F": {"brand": "Wyze", "device_family": "camera_or_video", "device_class": "surveillance_camera"},
    "D4:12:43": {"brand": "Wyze", "device_family": "camera_or_video", "device_class": "surveillance_camera"},
    "40:5D:82": {"brand": "Arlo", "device_family": "camera_or_video", "device_class": "surveillance_camera"},
    "A0:04:60": {"brand": "Arlo", "device_family": "camera_or_video", "device_class": "surveillance_camera"},
    "CC:40:D0": {"brand": "Arlo", "device_family": "camera_or_video", "device_class": "surveillance_camera"},
    "EC:71:DB": {"brand": "Reolink", "device_family": "camera_or_video", "device_class": "surveillance_camera"},
    "E0:E8:E6": {"brand": "Reolink", "device_family": "camera_or_video", "device_class": "surveillance_camera"},
    "38:AF:29": {"brand": "Dahua", "device_family": "camera_or_video", "device_class": "surveillance_camera"},
    "00:40:8C": {"brand": "Axis", "device_family": "camera_or_video", "device_class": "surveillance_camera"},
    "AC:CC:8E": {"brand": "Axis", "device_family": "camera_or_video", "device_class": "surveillance_camera"},
    "B8:A4:4F": {"brand": "Axis", "device_family": "camera_or_video", "device_class": "surveillance_camera"},
    "E8:27:25": {"brand": "Axis", "device_family": "camera_or_video", "device_class": "surveillance_camera"},
}

for _flock_prefix in (
    "B4:1E:52", "14:5A:FC", "3C:91:80", "70:C9:4E", "D8:F3:BC",
    "80:30:49", "B8:35:32", "74:4C:A1", "08:3A:88", "9C:2F:9D",
    "C0:35:32", "94:08:53", "E4:AA:EA", "F4:6A:DD", "F8:A2:D6",
    "24:B2:B9", "00:F4:8D", "D0:39:57", "E8:D0:FC", "E0:4F:43",
    "B8:1E:A4", "70:08:94", "58:8E:81", "EC:1B:BD", "3C:71:BF",
    "58:00:E3", "90:35:EA", "5C:93:A2", "64:6E:69", "48:27:EA",
    "82:6B:F2", "EC:62:60",
):
    _FRIENDLY_OUI_HINTS.setdefault(
        _flock_prefix,
        {"brand": "Flock Surveillance", "device_family": "camera_or_video", "device_class": "surveillance_camera"},
    )

_SSID_FAMILY_PATTERNS = (
    {
        "patterns": ("flock*", "flk-*", "alpr*", "penguin-*"),
        "device_family": "camera_or_video",
        "device_class": "surveillance_camera",
        "confidence": 0.82,
        "label": "Flock/ALPR SSID pattern",
    },
    {
        "patterns": ("esp_*", "esp-*", "esp32*", "esp8266*", "espressif*", "wled*", "tasmota*", "shelly*"),
        "device_family": "esp32_or_iot_dev_board",
        "device_class": "suspect_iot",
        "confidence": 0.72,
        "label": "ESP/IoT firmware SSID pattern",
    },
    {
        "patterns": ("v380*", "ipc*", "ipcam*", "hdwificam*", "hidvcam*", "ycc365*", "lookcam*", "camhi*", "ubox*", "icsee*", "reolink*", "wyze*", "arlo*", "ring-*", "ezviz*"),
        "device_family": "camera_or_video",
        "device_class": "suspect_camera",
        "confidence": 0.72,
        "label": "camera-style SSID pattern",
    },
    {
        "patterns": ("tp-link*", "tplink*", "netgear*", "asus*", "linksys*", "ubnt*", "unifi*", "ubiquiti*", "google wifi*", "nest wifi*"),
        "device_family": "network_infrastructure",
        "device_class": "wifi_ap",
        "confidence": 0.65,
        "label": "network infrastructure SSID pattern",
    },
)

_VENDOR_FAMILY_RULES = (
    (("espressif", "esp32", "esp8266"), "esp32_or_iot_dev_board", 0.86),
    (("dji", "parrot", "skydio", "autel"), "drone_or_controller", 0.85),
    (("hikvision", "dahua", "ezviz", "axis", "arlo", "reolink", "wyze", "ring camera", "vstarcam"), "camera_or_video", 0.82),
    (("ring",), "smart_home_iot", 0.66),
    (("tp-link", "tplink", "netgear", "ubiquiti", "unifi", "ruckus", "aruba", "cisco", "meraki", "mikrotik", "linksys", "asus"), "network_infrastructure", 0.78),
    (("apple", "samsung", "google", "xiaomi", "huawei", "oneplus", "oppo", "vivo"), "phone_tablet_or_pc", 0.70),
    (("tile", "chipolo", "airtag"), "tracker", 0.82),
    (("bose", "jbl", "harman", "sony", "sennheiser", "jabra", "beats", "sonos"), "audio", 0.74),
    (("fitbit", "garmin", "polar", "whoop", "oura"), "wearable", 0.78),
    (("tuya", "shelly", "philips hue", "ecobee", "honeywell", "ikea"), "smart_home_iot", 0.74),
)

_BLE_CATEGORY_FAMILY = {
    "Drone": ("drone_or_controller", 0.80),
    "Vehicle": ("vehicle", 0.70),
    "Smart Home": ("smart_home_iot", 0.70),
    "Wearable": ("wearable", 0.76),
    "Audio": ("audio", 0.72),
    "Gaming": ("gaming", 0.72),
    "Medical": ("medical", 0.72),
    "Tracker": ("tracker", 0.82),
    "Phone": ("phone_tablet_or_pc", 0.70),
}


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


def _oui_prefix(mac: str | None) -> str | None:
    if not mac:
        return None
    hex_only = mac.replace(":", "").replace("-", "").strip().upper()
    if len(hex_only) < 6:
        return None
    try:
        int(hex_only[:6], 16)
    except ValueError:
        return None
    return ":".join(hex_only[i:i + 2] for i in range(0, 6, 2))


def _public_oui_detail(mac: str | None, *, source: str | None = None, ble_addr_type: int | None = None) -> dict[str, Any]:
    res = resolve_mac(mac, source=source, ble_addr_type=ble_addr_type)
    if res.mac_is_randomized:
        return res.as_dict() | {"brand": None}
    return res.as_dict() | {"brand": res.vendor_short}


def _friendly_oui_hint(mac: str | None) -> dict[str, Any] | None:
    prefix = _oui_prefix(mac)
    if not prefix or is_random_mac(mac):
        return None
    return _FRIENDLY_OUI_HINTS.get(prefix)


def _device_class_from_name(name: str | None, reason: str | None) -> tuple[str | None, float | None]:
    name_l = (name or "").lower()
    reason_l = (reason or "").lower()
    if reason_l in ("explicit_camera_ble_name", "camera_service_uuid", "camera_company_id", "strong_camera_classifier") or any(
        token in name_l for token in _EXPLICIT_CAMERA_NAME_TOKENS
    ):
        return "suspect_camera", 0.75
    if reason_l == "ambiguous_iot_ble_name" or any(token in name_l for token in _AMBIGUOUS_IOT_NAME_TOKENS):
        return "suspect_iot", 0.35
    return None, None


def _scanner_label_is_class(label: str) -> bool:
    label_l = label.lower()
    return any(token in label_l for token in (
        "hidden camera", "card skimmer", "flock surveillance",
        "tracker", "drone controller", "flipper zero",
        "wifi-assoc", "wifi assoc", "wifi-oui", "wifi oui",
        "wifi-ssid", "wifi ssid",
    ))


def _known_network_labels(ssids: list[str] | None, ssid: str | None = None) -> list[dict[str, Any]]:
    values = [s for s in [ssid, *(ssids or [])] if s]
    labels: list[dict[str, Any]] = []
    seen: set[str] = set()
    for hint in ssid_pattern_hints(ssids, ssid):
        if str(hint.get("device_class")) != "known_owned":
            continue
        label = str(hint.get("label") or "")
        if label and label not in seen:
            labels.append(hint)
            seen.add(label)
    for value in values:
        value_l = value.lower()
        for spec in _KNOWN_NETWORK_PATTERNS:
            if fnmatchcase(value_l, str(spec["pattern"])):
                label = str(spec["label"])
                if label not in seen:
                    labels.append(dict(spec))
                    seen.add(label)
    return labels


def _ssid_family_hint(ssids: list[str] | None, ssid: str | None = None) -> dict[str, Any] | None:
    values = [s for s in [ssid, *(ssids or [])] if s and s != "(broadcast)"]
    for hint in ssid_pattern_hints(ssids, ssid):
        if hint.get("device_class") == "known_owned":
            continue
        return {
            "device_family": hint.get("device_family"),
            "device_class": hint.get("device_class"),
            "confidence": hint.get("confidence", 0.5),
            "source": hint.get("source") or "ssid_pattern",
            "evidence": f"{hint.get('label')}: {hint.get('matched_ssid')}",
        }
    for value in values:
        value_l = value.lower()
        for spec in _SSID_FAMILY_PATTERNS:
            if any(fnmatchcase(value_l, pattern) for pattern in spec["patterns"]):
                return {
                    "device_family": spec["device_family"],
                    "device_class": spec["device_class"],
                    "confidence": spec["confidence"],
                    "source": "ssid_pattern",
                    "evidence": f"{spec['label']}: {value}",
                }
    return None


def _vendor_family_hint(*values: str | None) -> dict[str, Any] | None:
    text = " ".join(v for v in values if v).lower()
    if not text:
        return None
    for tokens, family, confidence in _VENDOR_FAMILY_RULES:
        if any(token in text for token in tokens):
            return {
                "device_family": family,
                "confidence": confidence,
                "source": "vendor_keyword",
            }
    return None


def _mac_oui_rollup(macs: list[str] | None) -> dict[str, Any]:
    """Summarize public/randomized MAC evidence across a stable RF group."""
    randomized_count = 0
    public_count = 0
    representative_public_mac: str | None = None
    brand_counts: Counter[str] = Counter()
    vendor_long_by_brand: dict[str, str] = {}
    vendor_source_by_brand: dict[str, str] = {}
    prefix_bits_by_brand: dict[str, int] = {}

    for mac in macs or []:
        if not mac:
            continue
        resolved = resolve_mac(mac)
        if resolved.mac_is_randomized:
            randomized_count += 1
            continue
        public_count += 1
        if representative_public_mac is None:
            representative_public_mac = mac
        if resolved.vendor_short:
            brand_counts[resolved.vendor_short] += 1
            vendor_long_by_brand.setdefault(resolved.vendor_short, resolved.vendor_long or resolved.vendor_short)
            vendor_source_by_brand.setdefault(resolved.vendor_short, resolved.vendor_source or "rf_reference")
            if resolved.prefix_bits:
                prefix_bits_by_brand.setdefault(resolved.vendor_short, resolved.prefix_bits)

    public_oui_brands = dict(sorted(brand_counts.items(), key=lambda item: (-item[1], item[0])))
    brand_candidates = [
        {
            "brand": brand,
            "source": "oui",
            "count": count,
            "confidence": round(min(0.90, 0.55 + 0.10 * count), 2),
            "vendor_long": vendor_long_by_brand.get(brand),
            "vendor_source": vendor_source_by_brand.get(brand),
            "prefix_bits": prefix_bits_by_brand.get(brand),
        }
        for brand, count in brand_counts.most_common(5)
    ]
    majority_brand: str | None = None
    majority_count = 0
    majority_share = 0.0
    total_branded = sum(brand_counts.values())
    if brand_counts and total_branded:
        majority_brand, majority_count = brand_counts.most_common(1)[0]
        majority_share = majority_count / total_branded

    return {
        "public_oui_brands": public_oui_brands,
        "randomized_mac_count": randomized_count,
        "public_mac_count": public_count,
        "brand_candidates": brand_candidates,
        "representative_public_mac": representative_public_mac,
        "majority_brand": majority_brand,
        "majority_vendor_long": vendor_long_by_brand.get(majority_brand) if majority_brand else None,
        "majority_vendor_source": vendor_source_by_brand.get(majority_brand) if majority_brand else None,
        "majority_prefix_bits": prefix_bits_by_brand.get(majority_brand) if majority_brand else None,
        "majority_count": majority_count,
        "majority_share": majority_share,
        "public_oui_brand_count": total_branded,
        "representative_oui_prefix": _oui_prefix(representative_public_mac),
    }


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
    macs: list[str] | None = None,
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
    ble_apple_type: int | None = None,
    ble_apple_flags: int | None = None,
    ble_activity: int | None = None,
    ble_apple_auth: str | None = None,
    ble_raw_mfr: str | None = None,
    channel: int | None = None,
    auth_m: int | None = None,
) -> dict[str, Any]:
    mac = _best_mac(bssid=bssid, drone_id=drone_id)
    mac_meta = classify_mac_identity(mac, source=source, ble_addr_type=ble_addr_type)
    source_l = (source or "").lower()
    evidence: list[str] = []
    mac_oui = _public_oui_detail(mac, source=source, ble_addr_type=ble_addr_type)
    friendly_oui = _friendly_oui_hint(mac)
    group_rollup = _mac_oui_rollup(macs)
    known_networks = _known_network_labels(probed_ssids, ssid)
    ssid_hint = _ssid_family_hint(probed_ssids, ssid)
    apple_continuity = decode_apple_continuity(
        raw_mfr_hex=ble_raw_mfr,
        apple_type=ble_apple_type,
        apple_flags=ble_apple_flags,
        apple_activity=ble_activity,
        apple_auth=ble_apple_auth,
    )
    wifi_fingerprint_v2 = build_wifi_fingerprint_v2(
        source=source,
        ie_hash=ie_hash,
        probed_ssids=probed_ssids,
        ssid=ssid,
        bssid=mac,
        channel=channel,
        auth_m=auth_m,
        manufacturer=manufacturer,
        model=model,
    )
    drone_ssid_matches = drone_wifi_ssid_matches(ssid=ssid, probed_ssids=probed_ssids)
    drone_ssid_match = drone_ssid_matches[0] if drone_ssid_matches else None
    drone_related_class = (classification or "").lower() in (
        "confirmed_drone", "likely_drone", "possible_drone", "test_drone",
    )

    brand: str | None = None
    brand_source: str | None = None
    brand_confidence: float | None = None
    vendor_long: str | None = None
    ble_category: str | None = None

    if ble_company_id:
        ref_company = lookup_ble_company(ble_company_id)
        name = str(ref_company.get("name") or "") if ref_company else ""
        if not name:
            name, category = _legacy_lookup_company(ble_company_id)
        else:
            _, category = _legacy_lookup_company(ble_company_id)
        if name and name != "Unknown":
            brand = name
            brand_source = ref_company.get("source", "ble_company_id") if ref_company else "ble_company_id"
            brand_confidence = 0.90
            vendor_long = name
            evidence.append(f"BLE company ID 0x{ble_company_id:04X}: {name}")
            if category:
                ble_category = category
                evidence.append(f"BLE category hint: {category}")

    if apple_continuity:
        evidence.extend(str(e) for e in apple_continuity.get("evidence", []) if e)
        if not brand:
            brand = "Apple"
            brand_source = "apple_continuity"
            brand_confidence = float(apple_continuity.get("confidence") or 0.70)
            vendor_long = "Apple Continuity"

    if not brand and friendly_oui:
        brand = str(friendly_oui["brand"])
        brand_source = "friendly_oui"
        brand_confidence = 0.82
        vendor_long = mac_oui["vendor_long"] or brand
        evidence.append(f"Curated OUI prefix: {brand}")

    if not brand and mac and not mac_meta.mac_is_randomized and mac_oui["brand"]:
        brand = mac_oui["brand"]
        brand_source = str(mac_oui.get("vendor_source") or "rf_reference")
        brand_confidence = float(mac_oui.get("vendor_confidence") or 0.85)
        vendor_long = mac_oui["vendor_long"]
        label = brand if vendor_long in (None, brand) else f"{brand} ({vendor_long})"
        prefix_bits = mac_oui.get("prefix_bits")
        assignment = mac_oui.get("assignment_type")
        evidence.append(f"Public OUI: {label}" + (f" / {assignment}-{prefix_bits}" if assignment and prefix_bits else ""))
        if mac_oui.get("product_hint"):
            evidence.append(f"Product hint: {mac_oui['product_hint']}")

    if (
        source_l == "wifi_probe_request"
        and macs is not None
        and len([m for m in macs if m]) > 1
        and group_rollup["majority_brand"]
        and group_rollup["majority_share"] >= 0.5
        and (not brand or brand_source in ("oui", "ieee_ma_l", "ieee_ma_m", "ieee_ma_s", "wireshark_manuf", "nmap_mac_prefixes") or "ieee" in str(brand_source))
    ):
        brand = group_rollup["majority_brand"]
        brand_source = "group_oui_majority"
        brand_confidence = round(0.65 + min(0.25, group_rollup["majority_share"] * 0.20), 2)
        vendor_long = group_rollup["majority_vendor_long"]
        evidence.append(
            f"Group OUI majority: {brand} "
            f"({group_rollup['majority_count']}/{group_rollup['public_oui_brand_count']} public OUI hits)"
        )

    mfr = (manufacturer or "").strip()
    if not brand and mfr and mfr.lower() not in ("unknown", "randomized mac") and not _scanner_label_is_class(mfr):
        brand = mfr
        brand_source = "scanner_label"
        brand_confidence = 0.55
        vendor_long = vendor_long or mfr
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

    if known_networks:
        label = str(known_networks[0]["label"])
        evidence.append(f"Known network label: {label}")
        if not brand:
            brand = label
            brand_source = "known_network_label"
            brand_confidence = float(known_networks[0]["confidence"])
        if not device_class or device_class in ("wifi_device", "wifi_ap", "unknown_device"):
            device_class = str(known_networks[0]["device_class"])
            device_class_confidence = float(known_networks[0]["confidence"])

    if drone_ssid_match:
        prefix = str(drone_ssid_match.get("matched_prefix") or "")
        matched_ssid = str(drone_ssid_match.get("ssid") or "")
        manufacturer_hint = str(drone_ssid_match.get("manufacturer") or "")
        evidence.append(
            f"Drone SSID match: {matched_ssid} via {prefix}"
            + (f" ({manufacturer_hint})" if manufacturer_hint else "")
        )
        if drone_ssid_match.get("match_type") == "test_drone_ssid":
            device_class = "test_drone"
            device_class_confidence = 0.90
        elif drone_related_class:
            device_class = str(classification)
            device_class_confidence = max(float(device_class_confidence or 0.0), float(drone_ssid_match.get("confidence") or 0.50))

    if ssid_hint:
        evidence.append(str(ssid_hint["evidence"]))
        if not device_class or device_class in ("wifi_device", "wifi_ap", "unknown_device"):
            device_class = str(ssid_hint["device_class"])
            device_class_confidence = float(ssid_hint["confidence"])

    if friendly_oui and (not device_class or device_class in ("wifi_device", "wifi_ap", "ble_device", "unknown_device")):
        device_class = str(friendly_oui["device_class"])
        device_class_confidence = 0.70

    device_family: str | None = None
    family_source: str | None = None
    family_confidence: float | None = None
    if known_networks and known_networks[0].get("device_family"):
        device_family = str(known_networks[0]["device_family"])
        family_source = "known_network_label"
        family_confidence = float(known_networks[0]["confidence"])
    elif drone_ssid_match and drone_related_class:
        device_family = "drone_or_controller"
        family_source = "drone_signature_reference"
        family_confidence = float(drone_ssid_match.get("confidence") or 0.50)
    elif friendly_oui:
        device_family = str(friendly_oui["device_family"])
        family_source = "friendly_oui"
        family_confidence = 0.78
    elif ssid_hint:
        device_family = str(ssid_hint["device_family"])
        family_source = str(ssid_hint["source"])
        family_confidence = float(ssid_hint["confidence"])
    elif device_class in ("suspect_camera", "surveillance_camera"):
        device_family = "camera_or_video"
        family_source = "device_class"
        family_confidence = float(device_class_confidence or 0.60)
    elif ble_category in _BLE_CATEGORY_FAMILY:
        device_family, family_confidence = _BLE_CATEGORY_FAMILY[ble_category]
        family_source = "ble_company_category"
    else:
        vendor_hint = _vendor_family_hint(brand, vendor_long, manufacturer)
        if vendor_hint:
            device_family = str(vendor_hint["device_family"])
            family_source = str(vendor_hint["source"])
            family_confidence = float(vendor_hint["confidence"])
    if not device_family:
        if source_l == "wifi_ap_inventory":
            device_family = "wifi_ap"
            family_source = "source"
            family_confidence = 0.45
        elif source_l == "wifi_probe_request":
            device_family = "wifi_client"
            family_source = "source"
            family_confidence = 0.35
        elif source_l.startswith("ble"):
            device_family = "ble_device"
            family_source = "source"
            family_confidence = 0.35

    if device_family:
        evidence.append(f"Device family: {device_family} via {family_source}")
    if wifi_fingerprint_v2:
        evidence.extend(str(e) for e in wifi_fingerprint_v2.get("evidence", []) if e)
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
    reference_source_ids: list[str] = []
    for source_id in (
        mac_oui.get("vendor_source"),
        brand_source,
        group_rollup.get("majority_vendor_source") if brand_source == "group_oui_majority" else None,
        "group_oui_majority" if brand_source == "group_oui_majority" else None,
        "operator_ssid_override" if known_networks else None,
        "ssid_pattern" if ssid_hint else None,
        "friendly_oui" if friendly_oui else None,
        "apple_continuity" if apple_continuity else None,
        "wifi_fingerprint_v2" if wifi_fingerprint_v2 else None,
        "drone_signature_reference" if drone_ssid_match else None,
    ):
        if not source_id:
            continue
        for part in str(source_id).replace(",", "+").split("+"):
            part = part.strip()
            if part and part not in reference_source_ids:
                reference_source_ids.append(part)
    if mac_meta.mac_is_randomized and "randomized_mac" not in reference_source_ids:
        reference_source_ids.append("randomized_mac")
    reference_sources = source_details(reference_source_ids)

    return {
        "mac_is_randomized": mac_meta.mac_is_randomized,
        "mac_identity_kind": mac_meta.mac_identity_kind,
        "mac_reason": mac_meta.mac_reason,
        "brand": brand,
        "brand_source": brand_source,
        "brand_confidence": brand_confidence,
        "vendor_short": mac_oui.get("vendor_short"),
        "vendor_long": vendor_long,
        "vendor_aliases": mac_oui.get("vendor_aliases"),
        "vendor_source": mac_oui.get("vendor_source"),
        "assignment_type": mac_oui.get("assignment_type"),
        "prefix_bits": mac_oui.get("prefix_bits"),
        "oui_prefix": mac_oui["oui_prefix"],
        "product_hint": mac_oui.get("product_hint"),
        "vendor_confidence": mac_oui.get("vendor_confidence"),
        "reference_updated_at": mac_oui.get("reference_updated_at"),
        "reference_sources": reference_sources,
        "public_oui_brands": group_rollup["public_oui_brands"],
        "randomized_mac_count": group_rollup["randomized_mac_count"],
        "public_mac_count": group_rollup["public_mac_count"],
        "brand_candidates": group_rollup["brand_candidates"],
        "representative_public_mac": group_rollup["representative_public_mac"],
        "representative_oui_prefix": group_rollup["representative_oui_prefix"],
        "known_network_label": known_networks[0]["label"] if known_networks else None,
        "device_class": device_class,
        "device_class_confidence": device_class_confidence,
        "device_family": device_family,
        "family_source": family_source,
        "family_confidence": family_confidence,
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
        "apple_continuity": apple_continuity,
        "wifi_fingerprint_v2": wifi_fingerprint_v2,
        "drone_ssid_match": drone_ssid_match,
        "drone_ssid_matches": drone_ssid_matches or None,
    }


def build_detection_explanation(
    *,
    source: str | None,
    classification: str | None,
    rf_meta: dict[str, Any] | None = None,
    sensor_count: int | None = None,
    geometry_quality: dict[str, Any] | None = None,
    identity_source: str | None = None,
) -> dict[str, Any]:
    """Normalize backend RF evidence into an operator-facing explanation.

    This intentionally consumes existing classifier/enrichment metadata; it
    does not create a second classifier. The result is stable enough for the
    dashboard to render without duplicating RF heuristics client-side.
    """
    meta = rf_meta or {}
    source_l = (source or "").lower()
    cls = (classification or "unknown_device").lower()
    evidence = [str(e) for e in (meta.get("evidence") or []) if e]
    limitations: list[str] = []
    recommended_action = "monitor"
    primary_reason = "Observed RF device"

    drone_match = meta.get("drone_ssid_match")
    if source_l in ("ble_rid", "wifi_dji_ie", "wifi_beacon_rid"):
        primary_reason = "Remote ID broadcast"
        confidence_band = "confirmed"
        recommended_action = "investigate"
    elif isinstance(drone_match, dict) and drone_match.get("match_type") == "test_drone_ssid":
        primary_reason = "FOF test drone SSID"
        confidence_band = "likely"
        recommended_action = "test_signal"
    elif isinstance(drone_match, dict):
        primary_reason = "Curated drone SSID prefix"
        confidence_band = "likely"
        recommended_action = "investigate"
        limitations.append("SSID-only; no Remote ID")
    elif meta.get("known_network_label") or cls == "known_ap":
        primary_reason = "Known infrastructure"
        confidence_band = "known"
        recommended_action = "ignore_known"
    elif meta.get("apple_continuity"):
        primary_reason = "Apple Continuity BLE"
        confidence_band = "possible"
    elif (identity_source or meta.get("identity_source")) == "probe_ie_hash":
        primary_reason = "Randomized probe identity"
        confidence_band = "diagnostic"
    elif meta.get("wifi_fingerprint_v2"):
        primary_reason = "WiFi fingerprint"
        confidence_band = "diagnostic"
    elif source_l == "wifi_probe_request":
        primary_reason = "WiFi probe request"
        confidence_band = "diagnostic"
    elif source_l.startswith("ble"):
        primary_reason = "BLE fingerprint"
        confidence_band = "possible"
    elif cls == "confirmed_drone":
        primary_reason = "Confirmed drone signal"
        confidence_band = "confirmed"
        recommended_action = "investigate"
    elif cls in ("likely_drone", "test_drone"):
        primary_reason = "Drone-related signal"
        confidence_band = "likely"
        recommended_action = "investigate"
    elif cls == "possible_drone":
        primary_reason = "Possible drone signal"
        confidence_band = "possible"
    elif cls in ("tracker", "hostile_tool"):
        primary_reason = "Privacy/security device signature"
        confidence_band = "likely"
        recommended_action = "investigate"
    elif cls in ("wifi_device", "unknown_device"):
        confidence_band = "diagnostic"
        recommended_action = "monitor"
    else:
        confidence_band = "possible"

    if cls == "test_drone":
        recommended_action = "test_signal"
    if cls == "known_ap":
        recommended_action = "ignore_known"
    if cls in ("unknown_device", "wifi_device") and recommended_action == "monitor":
        recommended_action = "verify_known"

    if meta.get("mac_is_randomized"):
        limitations.append("Randomized MAC")
    if sensor_count is not None and sensor_count < 2:
        limitations.append("Single-sensor observation")
    if isinstance(geometry_quality, dict):
        status = str(geometry_quality.get("status") or "")
        if status in ("diagnostic_only", "needs_calibration", "insufficient_sensors"):
            limitations.append("Diagnostic geometry only")
        for reason in geometry_quality.get("reasons") or []:
            if reason and str(reason) not in limitations:
                limitations.append(str(reason))

    # Keep ordering stable and remove duplicates while preserving meaning.
    deduped_limitations: list[str] = []
    for item in limitations:
        if item and item not in deduped_limitations:
            deduped_limitations.append(item)

    return {
        "primary_reason": primary_reason,
        "confidence_band": confidence_band,
        "evidence": evidence[:8],
        "limitations": deduped_limitations[:8],
        "recommended_action": recommended_action,
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
