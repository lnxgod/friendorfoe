#!/usr/bin/env python3
"""Entrypoint for the Backend Badge Lite three-board factory flasher."""

from __future__ import annotations

import sys
from pathlib import Path


BACKEND_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = BACKEND_ROOT.parent
for candidate in (REPOSITORY_ROOT, BACKEND_ROOT):
    if str(candidate) not in sys.path:
        sys.path.insert(0, str(candidate))

from tools.lite_factory_flasher.cli import main  # noqa: E402


if __name__ == "__main__":
    raise SystemExit(main())
