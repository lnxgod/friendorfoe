"""A scanner-scoped view of observed probe addresses, without identity inference."""

import math
import re
from collections.abc import Iterable

from app.models.schemas import StoredDetection

_MAC = re.compile(r"(?:[0-9A-F]{2}:){5}[0-9A-F]{2}$")


def probe_activity(items: Iterable[StoredDetection], *, now: float,
                   sensor_id: str, max_age_s: int = 300) -> list[dict]:
    groups: dict[str, dict] = {}
    for item in items:
        if item.source != "wifi_probe_request" or item.device_id != sensor_id:
            continue
        received = item.received_at
        if not math.isfinite(received) or received > now + 5:
            continue
        observed = received
        # Synchronized scanner timestamps exclude delayed uploads from "recent".
        if item.timestamp and 1_700_000_000_000 <= item.timestamp <= (now + 5) * 1000:
            observed = min(received, item.timestamp / 1000)
        age = max(0.0, now - observed)
        if age > max_age_s:
            continue
        mac = (item.bssid or item.drone_id.removeprefix("flock_probe_").removeprefix("probe_")).upper()
        if not _MAC.fullmatch(mac) or int(mac[:2], 16) & 1 or mac == "00:00:00:00:00:00":
            continue
        row = groups.setdefault(mac, {
            "mac": mac, "sensor_id": sensor_id,
            "locally_administered": bool(int(mac[:2], 16) & 2),
            "reports": 0, "wildcard_reports": 0, "unknown_reports": 0,
            "last_seen": 0.0, "rssi": None, "channel": None,
            "targets": {},
        })
        row["reports"] += 1
        # SSID is the exact primary IE. Legacy CSV transport can split a comma
        # inside a network name, so don't invent extra names from that field.
        targets = {item.ssid} if item.ssid else {
            s for s in (item.probed_ssids or []) if s and s != "(broadcast)"
        }
        wildcard = not item.ssid and (
            item.class_reason in ("Wi-Fi wildcard probe", "Flock wildcard probe")
            or "(broadcast)" in (item.probed_ssids or [])
        )
        if wildcard:
            row["wildcard_reports"] += 1
        elif not targets:
            row["unknown_reports"] += 1
        for ssid in targets:
            target = row["targets"].setdefault(ssid, {"ssid": ssid, "reports": 0, "last_seen": 0.0})
            target["reports"] += 1
            target["last_seen"] = max(target["last_seen"], observed)
        if observed >= row["last_seen"]:
            row["last_seen"] = observed
            row["rssi"] = item.rssi if item.rssi is not None and -127 <= item.rssi < 0 else None
            channel = item.channel
            # Existing uplinks encode frequency in the legacy "channel" field.
            if channel == 2484:
                channel = 14
            elif channel and 2412 <= channel <= 2472 and (channel - 2407) % 5 == 0:
                channel = (channel - 2407) // 5
            elif channel and 5000 < channel < 5900 and channel % 5 == 0:
                channel = (channel - 5000) // 5
            row["channel"] = channel if channel and 1 <= channel <= 233 else None
    result = []
    for row in groups.values():
        row["age_s"] = round(max(0, now - row["last_seen"]), 1)
        row["targets"] = sorted(row["targets"].values(), key=lambda s: (-s["last_seen"], s["ssid"]))
        result.append(row)
    return sorted(result, key=lambda r: (-r["last_seen"], -(r["rssi"] or -128), r["mac"]))[:500]
