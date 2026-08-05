"""Backend Badge Lite factory-flasher host tooling.

The package lives inside the isolated backend-firmware tree, but deliberately
reuses the readback/topology primitives from the existing native-badge
factory flasher.  Importing it never opens a serial port or mutates hardware.
"""

from __future__ import annotations

import sys
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))
