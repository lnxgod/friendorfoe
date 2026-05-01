"""Redact privacy-sensitive fields from FoF API JSON dumps.

Policy (matches the user's chosen "GPS + SSIDs, keep everything else" mix):
- SSIDs that match drone / surveillance / IoT signature patterns are KEPT
  (those are the diagnostic payload — the whole point of the dashboard).
- Everything else in the SSID slot is replaced with [REDACTED-SSID-N].
- probed_ssids (visiting-device known networks) are ALWAYS redacted —
  reveals where visitors live/work.
- GPS lat/lon are fuzzed to nearest 0.1 degree (~11 km).
- BSSID/MAC: keep OUI (first 3 octets), replace last 3 with XX:XX:XX.
- BLE names of drone/tracker/glasses brands are KEPT (designed-public).
- IP addresses in any LAN range are scrubbed to placeholders.
"""

from __future__ import annotations

import json
import re
import sys
from typing import Any

# Drone / surveillance / IoT SSID prefixes — these are the interesting payload
# and should NOT be redacted. Pulled from the project's drone signature reference.
KEEP_SSID_PATTERNS = [
    re.compile(p, re.IGNORECASE) for p in (
        r"^DJI[-_]", r"^Tello[-_]", r"^Mavic[-_]", r"^Phantom[-_]", r"^Inspire[-_]",
        r"^Spark[-_]", r"^Matrice[-_]", r"^Mini[-_]", r"^Avata[-_]", r"^Air[-_]",
        r"^Skydio[-_]", r"^Anafi[-_]", r"^Parrot", r"^Autel[-_]", r"^Hubsan",
        r"^Yuneec", r"^FIMI[-_]", r"^Hover[-_]", r"^GoPro", r"^Insta360",
        r"^Verkada", r"^Rhombus", r"^Flock", r"^FLK[-_]", r"^ELSAG",
        r"^Ring[-_ ]", r"^Nest[-_]", r"^Wyze[-_]", r"^Arlo[-_]", r"^Reolink",
        r"^Tapo[-_]", r"^Hik[-_]", r"^Amcrest", r"^Blink[-_]", r"^MV[0-9]",
        r"^IPC[-_]", r"^IP_CAM", r"^V380", r"^CLOUDCAM", r"^HIDVCAM",
        r"^BlackVue", r"^DR9", r"^DR7", r"^VIOFO", r"^70mai", r"^Nextbase",
        r"^Thinkware", r"^DDPai", r"^Rexing", r"^Akaso",
        r"^FoF-", r"^WiFiUFO",  # project's own test/calibration SSIDs
    )
]

REDACT_BSSID_RE = re.compile(r"^([0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}):"
                              r"[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}$")
REDACT_IP_RE = re.compile(r"^(?:10\.|192\.168\.|172\.(?:1[6-9]|2[0-9]|3[01])\.)"
                          r"\d+\.\d+$")

_ssid_redact_counter = {"n": 0}
_ssid_seen: dict[str, str] = {}


def _redact_ssid(ssid: str) -> str:
    """Pass through diagnostic SSIDs; opaque-replace everything else."""
    if not ssid:
        return ssid
    for pat in KEEP_SSID_PATTERNS:
        if pat.match(ssid):
            return ssid
    if ssid in _ssid_seen:
        return _ssid_seen[ssid]
    _ssid_redact_counter["n"] += 1
    label = f"[REDACTED-SSID-{_ssid_redact_counter['n']}]"
    _ssid_seen[ssid] = label
    return label


def _redact_bssid(bssid: str) -> str:
    m = REDACT_BSSID_RE.match(bssid or "")
    if not m:
        return bssid
    return m.group(1) + ":XX:XX:XX"


def _redact_drone_id(drone_id: str) -> str:
    """drone_id often carries a MAC. Redact in place if it looks like one."""
    if not drone_id:
        return drone_id
    # probe_AA:BB:CC:DD:EE:FF or BLE:AA:BB:CC:DD:EE:FF or plain MAC
    def _replace(m: re.Match) -> str:
        return _redact_bssid(m.group(0))
    return re.sub(r"[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}", _replace, drone_id)


def _fuzz_gps(value: float) -> float:
    """Round to 1 decimal place — about 11 km of precision."""
    if not isinstance(value, (int, float)):
        return value
    return round(value, 1)


def _redact_ip(value: str) -> str:
    if isinstance(value, str) and REDACT_IP_RE.match(value):
        return "10.0.0.X"
    return value


# Field-name based handling. Walks any dict/list nested structure.
SSID_FIELDS = {"ssid", "connected_ap_ssid", "wifi_ssid", "known_network_label"}
PROBE_SSID_FIELDS = {"probed_ssids", "probe_ssids"}
BSSID_FIELDS = {"bssid", "mac", "device_mac"}
DRONE_ID_FIELDS = {"drone_id", "entity_id"}
GPS_LAT_FIELDS = {"lat", "latitude", "device_lat", "operator_lat"}
GPS_LON_FIELDS = {"lon", "longitude", "device_lon", "operator_lon"}
IP_FIELDS = {"ip", "last_ip", "static_ip", "last_fetch_ip"}
URL_FIELDS = {"last_fetch_url", "url"}

# Free-text deny tokens — operator-known SSIDs that must not appear anywhere
# (evidence strings, labels, log messages). Names are kept short here on purpose
# — they're operator-visible in the captured environment, never source code.
DENY_TOKEN_RE = re.compile(
    r"\b(TeamCharityCase[\w-]*|CasaChomp[\w-]*|Hyrule[\w-]*|hyrule-guest|Nolas-?\d*|C5-\d+)\b",
    re.IGNORECASE,
)


def _redact_url(value: str) -> str:
    if not isinstance(value, str):
        return value
    return REDACT_IP_RE.sub("10.0.0.X", value) if REDACT_IP_RE.search(value) else \
        re.sub(r"(?:10\.|192\.168\.|172\.(?:1[6-9]|2[0-9]|3[01])\.)\d+\.\d+", "10.0.0.X", value)


def _scrub_freetext(value: str) -> str:
    if not isinstance(value, str):
        return value
    out = DENY_TOKEN_RE.sub("[REDACTED-SSID]", value)
    out = re.sub(
        r"(?:10\.|192\.168\.|172\.(?:1[6-9]|2[0-9]|3[01])\.)\d+\.\d+",
        "10.0.0.X",
        out,
    )
    out = re.sub(
        r"([0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}):"
        r"[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}",
        lambda m: m.group(1) + ":XX:XX:XX",
        out,
    )
    return out


def redact(obj: Any) -> Any:
    if isinstance(obj, dict):
        out = {}
        for k, v in obj.items():
            if k in SSID_FIELDS and isinstance(v, str):
                out[k] = _redact_ssid(v)
            elif k in PROBE_SSID_FIELDS and isinstance(v, list):
                out[k] = [f"[REDACTED-PROBE-{i+1}]" for i in range(len(v))]
            elif k in BSSID_FIELDS and isinstance(v, str):
                out[k] = _redact_bssid(v)
            elif k in DRONE_ID_FIELDS and isinstance(v, str):
                out[k] = _redact_drone_id(v)
            elif k in GPS_LAT_FIELDS and isinstance(v, (int, float)):
                out[k] = _fuzz_gps(v)
            elif k in GPS_LON_FIELDS and isinstance(v, (int, float)):
                out[k] = _fuzz_gps(v)
            elif k in IP_FIELDS and isinstance(v, str):
                out[k] = _redact_ip(v)
            elif k in URL_FIELDS and isinstance(v, str):
                out[k] = _redact_url(v)
            else:
                out[k] = redact(v)
        return out
    if isinstance(obj, list):
        return [redact(x) for x in obj]
    if isinstance(obj, str):
        return _scrub_freetext(obj)
    return obj


if __name__ == "__main__":
    data = json.load(sys.stdin)
    json.dump(redact(data), sys.stdout, indent=2)
    sys.stdout.write("\n")
