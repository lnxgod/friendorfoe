#!/usr/bin/env python3
"""Entrypoint for the open-source FoF three-board badge factory flasher."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.badge_flasher.cli import main


if __name__ == "__main__":
    raise SystemExit(main())
