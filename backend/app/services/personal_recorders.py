"""Passive recorder hints; presence never establishes active recording.

Sources and intentionally excluded ambiguous UUIDs: docs/personal-ai-recorders.md.
Keep signatures aligned with PersonalRecorderSignatures.kt and ble_fingerprint.c.
"""

import re

_SIGNATURES = [
    ("Plaud", ["plaud", "notepin"], None),
    ("Omi", ["omi"], "19b10000-e8f2-537e-4f6c-d104768a1214"),
    ("Limitless", ["limitless"], "632de001-604c-446b-a80f-7963e950f3fb"),
    ("Bee", ["bee"], "03d5d5c4-a86c-11ee-9d89-8f2089a49e7e"),
    ("Friend", ["friend_"], "1a3fd0e7-b1f3-ac9e-2e49-b647b2c4f8da"),
    ("Fieldy", ["fieldy"], None),
]
_UUID_RE = re.compile(r"(?<![0-9a-z])[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}(?![0-9a-z])", re.I)


def match_personal_recorder(name: str | None, services: str | None = None) -> dict | None:
    name = (name or "").strip().lower()
    uuids = set(_UUID_RE.findall((services or "").lower()))
    # Full custom service evidence wins over a mutable name.
    for brand, _, uuid in _SIGNATURES:
        if uuid and uuid in uuids:
            return {"manufacturer": brand, "confidence": 0.90,
                    "reason": f"recorder:uuid:{uuid}"}
    if re.fullmatch(r"plaud[a-z0-9]{4}", name):
        return {"manufacturer": "Plaud", "confidence": 0.75,
                "reason": "recorder:name:plaud_serial"}
    for brand, prefixes, _ in _SIGNATURES:
        for prefix in prefixes:
            if name == prefix or (name.startswith(prefix) and
                    (prefix.endswith("_") or name[len(prefix):len(prefix) + 1] in (" ", "-", "_"))):
                return {"manufacturer": brand, "confidence": 0.75,
                        "reason": f"recorder:name:{prefix}"}
    return None
