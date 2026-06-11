"""Curated passive privacy RF signature helpers."""

from __future__ import annotations

import json
from functools import lru_cache
from pathlib import Path
from typing import Any


_DATA_PATH = Path(__file__).resolve().with_name("privacy_rf_signatures.json")
_BANNED_BROAD_PATTERNS = {"MV", "HOLY", "UFO-", "CAM", "CAM-", "CAM_", "PORTAL", "EVIL", "TWIN"}


@lru_cache(maxsize=1)
def load_privacy_signature_catalog() -> dict[str, Any]:
    with _DATA_PATH.open("r", encoding="utf-8") as fh:
        data = json.load(fh)
    return data


def iter_wifi_signatures() -> list[dict[str, Any]]:
    data = load_privacy_signature_catalog()
    entries = data.get("wifi")
    return list(entries) if isinstance(entries, list) else []


def validate_privacy_signature_catalog() -> list[str]:
    errors: list[str] = []
    seen: set[tuple[str, str]] = set()
    for idx, entry in enumerate(iter_wifi_signatures()):
        prefix = f"wifi[{idx}]"
        pattern = str(entry.get("pattern") or "")
        match_type = str(entry.get("match_type") or "")
        key = (match_type.lower(), pattern.lower())
        if not pattern:
            errors.append(f"{prefix}: missing pattern")
        if pattern.upper() in _BANNED_BROAD_PATTERNS or len(pattern) < 4:
            errors.append(f"{prefix}: broad pattern {pattern!r}")
        if key in seen:
            errors.append(f"{prefix}: duplicate pattern {pattern!r}")
        seen.add(key)
        for field in ("manufacturer", "device_type", "privacy_kind", "class_reason", "source_note", "false_positive_policy"):
            if not entry.get(field):
                errors.append(f"{prefix}: missing {field}")
        confidence = entry.get("confidence")
        if not isinstance(confidence, (int, float)) or not 0.0 <= float(confidence) <= 1.0:
            errors.append(f"{prefix}: invalid confidence")
    return errors


def _matches(text: str, pattern: str, match_type: str) -> bool:
    text_l = text.lower()
    pattern_l = pattern.lower()
    if match_type == "exact":
        return text_l == pattern_l
    if match_type == "contains":
        return pattern_l in text_l
    return text_l.startswith(pattern_l)


def match_privacy_wifi_ssid(ssid: str | None) -> dict[str, Any] | None:
    if not ssid:
        return None
    for entry in iter_wifi_signatures():
        pattern = str(entry.get("pattern") or "")
        match_type = str(entry.get("match_type") or "prefix").lower()
        if pattern and _matches(ssid, pattern, match_type):
            matched = dict(entry)
            matched["matched_ssid"] = ssid
            return matched
    return None
