"""Lite-specific user-visible redaction built on the proven capture boundary."""

from __future__ import annotations

import re

from tools.badge_flasher.public_output import scrub_user_visible_text


_PRIVATE_HEX_RE = re.compile(
    r"(?<![0-9A-Za-z_])[0-9A-Fa-f]{6,64}(?![0-9A-Za-z_])"
)


def scrub_lite_transcript(value: object) -> str:
    scrubbed = scrub_user_visible_text(value).replace("[hardware-id]", "LITE")
    return _PRIVATE_HEX_RE.sub("LITE", scrubbed)
