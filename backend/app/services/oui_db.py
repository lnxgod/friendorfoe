"""IEEE OUI database (Wireshark `manuf` format).

Loads the committed Wireshark manuf file (`backend/app/data/manuf.txt`)
into in-memory tables keyed by assignment size:

- MA-L (24-bit / 3-byte prefix)  → overwhelming majority, ~37k entries
- MA-M (28-bit / 3.5 byte prefix, 7 hex digits)
- MA-S (36-bit / 4.5 byte prefix, 9 hex digits)

Longest-prefix wins. Randomized MACs (first-octet LAA bit set) short-circuit
to `("Randomized MAC", "Randomized MAC")` without touching the tables.

API:
    oui_lookup("AA:BB:CC:…")   -> (short_name, long_name) | None
    oui_lookup(bytes b"\\xaa\\xbb\\xcc")  -> (short_name, long_name) | None
    load_manuf()               -> reload from disk (idempotent)
    get_stats()                -> dict of counters
"""

from __future__ import annotations

import logging
import time
from pathlib import Path

logger = logging.getLogger(__name__)

# Default location — committed alongside the service code.
_DEFAULT_PATH = Path(__file__).resolve().parent.parent / "data" / "manuf.txt"

# In-memory tables. Keys are upper-case hex (no separators) prefixes of the
# exact bit-length indicated by the table name.
_mal: dict[str, tuple[str, str]] = {}   # 6 hex chars (24 bits)
_mam: dict[str, tuple[str, str]] = {}   # 7 hex chars (28 bits) — nibble-indexed
_mas: dict[str, tuple[str, str]] = {}   # 9 hex chars (36 bits)

_stats = {
    "entries_total": 0,
    "entries_ma_l": 0,
    "entries_ma_m": 0,
    "entries_ma_s": 0,
    "path": str(_DEFAULT_PATH),
    "bytes": 0,
    "loaded_at": 0.0,
    "random_mac_hits": 0,
    "lookup_hits": 0,
    "lookup_misses": 0,
}

# ── Randomized-MAC detection ────────────────────────────────────────────

def is_random_mac(mac: str | bytes | None) -> bool:
    """Return True when the MAC has the locally-administered bit (bit 1 of
    the first octet) set — i.e., it's a randomized MAC. Phones rotate these
    every few minutes; there's no real OUI behind them."""
    if mac is None:
        return False
    if isinstance(mac, bytes):
        if len(mac) < 1:
            return False
        first = mac[0]
    else:
        norm = mac.replace(":", "").replace("-", "").strip()
        if len(norm) < 2:
            return False
        try:
            first = int(norm[:2], 16)
        except ValueError:
            return False
    return bool(first & 0x02)


# ── Parser ─────────────────────────────────────────────────────────────

def _normalize_prefix(prefix: str) -> tuple[str, int] | None:
    """Convert '00:00:01' / '00:1B:C5:00:00/36' → ('00000100:1B:C5:00:00', 24|28|36).

    Returns (upper_hex_no_sep_padded_to_mask_nibbles, mask_bits) or None.
    """
    mask_bits = 24
    if "/" in prefix:
        prefix, m_str = prefix.split("/", 1)
        try:
            mask_bits = int(m_str)
        except ValueError:
            return None
    hex_only = prefix.replace(":", "").replace("-", "").upper()
    # Pad/truncate to `mask_bits // 4` hex chars (handle weird nibble-alignments).
    nibbles = mask_bits // 4
    if mask_bits % 4 != 0:
        # IEEE allocations are always in whole bytes or half-bytes; 28 and 36
        # are half-byte aligned. Wireshark pads an extra '0' on the low nibble
        # of the last byte for /28 and /36 — so we've already got nibble
        # alignment in the hex string. Truncate to `nibbles` + 1 for half-byte
        # then strip the low nibble for the lookup index.
        nibbles = (mask_bits + 3) // 4
    if len(hex_only) < nibbles:
        return None
    return hex_only[:nibbles], mask_bits


def load_manuf(path: Path | None = None) -> int:
    """Parse the Wireshark manuf file into in-memory tables. Returns total entries."""
    p = Path(path) if path else _DEFAULT_PATH
    if not p.exists():
        logger.warning("OUI manuf file not found at %s; OUI lookup will be empty", p)
        return 0

    _mal.clear()
    _mam.clear()
    _mas.clear()

    mal_n = mam_n = mas_n = 0
    byte_count = 0
    t0 = time.time()

    try:
        with p.open("r", encoding="utf-8", errors="replace") as f:
            for line in f:
                byte_count += len(line)
                if not line or line.startswith("#"):
                    continue
                # Tab-delimited; wireshark uses multi-tab spacing but split()
                # without args collapses consecutive whitespace.
                parts = line.rstrip("\n").split("\t")
                # Some rows have only the prefix + short name; long name is optional
                parts = [p.strip() for p in parts if p.strip()]
                if len(parts) < 2:
                    continue
                prefix = parts[0]
                short = parts[1]
                long_ = parts[2] if len(parts) >= 3 else short
                result = _normalize_prefix(prefix)
                if not result:
                    continue
                hex_key, bits = result
                entry = (short, long_)
                if bits >= 36:
                    _mas[hex_key[:9]] = entry
                    mas_n += 1
                elif bits >= 28:
                    _mam[hex_key[:7]] = entry
                    mam_n += 1
                else:
                    _mal[hex_key[:6]] = entry
                    mal_n += 1
    except Exception as e:
        logger.warning("OUI manuf parse failed at %s: %s", p, e)
        return 0

    total = mal_n + mam_n + mas_n
    _stats.update({
        "entries_total": total,
        "entries_ma_l": mal_n,
        "entries_ma_m": mam_n,
        "entries_ma_s": mas_n,
        "path": str(p),
        "bytes": byte_count,
        "loaded_at": time.time(),
    })
    logger.info(
        "OUI DB loaded: %d entries (MA-L=%d MA-M=%d MA-S=%d) from %s in %.2fs",
        total, mal_n, mam_n, mas_n, p, time.time() - t0,
    )
    return total


# ── Lookup ─────────────────────────────────────────────────────────────

def oui_lookup(mac: str | bytes | None) -> tuple[str, str] | None:
    """Resolve a MAC to (short_name, long_name).

    - Randomized MACs (LAA bit set) short-circuit to ("Randomized MAC",
      "Randomized MAC") — they can't be resolved via OUI by design.
    - Longest-prefix match across MA-S (36-bit) → MA-M (28-bit) → MA-L (24-bit).
    - Returns None only for MACs shorter than 24 bits or with no match at all.
    """
    if mac is None:
        _stats["lookup_misses"] += 1
        return None

    if isinstance(mac, bytes):
        hex_only = mac.hex().upper()
    else:
        hex_only = mac.replace(":", "").replace("-", "").strip().upper()

    if len(hex_only) < 6:
        _stats["lookup_misses"] += 1
        return None

    # Randomized-MAC short-circuit — answer authoritatively
    try:
        first = int(hex_only[:2], 16)
    except ValueError:
        _stats["lookup_misses"] += 1
        return None
    if first & 0x02:
        _stats["random_mac_hits"] += 1
        return ("Randomized MAC", "Randomized MAC")

    # MA-S (36 bits / 9 hex)
    if len(hex_only) >= 9:
        hit = _mas.get(hex_only[:9])
        if hit:
            _stats["lookup_hits"] += 1
            return hit
    # MA-M (28 bits / 7 hex)
    if len(hex_only) >= 7:
        hit = _mam.get(hex_only[:7])
        if hit:
            _stats["lookup_hits"] += 1
            return hit
    # MA-L (24 bits / 6 hex)
    hit = _mal.get(hex_only[:6])
    if hit:
        _stats["lookup_hits"] += 1
        return hit

    _stats["lookup_misses"] += 1
    return None


def get_stats() -> dict:
    """Return counters for the /admin/oui/stats endpoint."""
    return dict(_stats)


# Load on import so the first caller gets a populated DB. If the file is
# missing (e.g., in tests), the module stays functional and returns None /
# "Randomized MAC" as appropriate.
try:
    load_manuf()
except Exception as e:
    logger.warning("OUI manuf auto-load failed: %s", e)
