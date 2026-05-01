#!/usr/bin/env python3
"""Build the local defensive RF reference artifact.

The backend never calls public lookup APIs while ingesting live RF data. This
script is the explicit operator/developer refresh step that downloads public
reference registries, normalizes them, folds in local overrides, and writes a
committed JSON artifact consumed by app.services.rf_reference.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

REPO_ROOT = Path(__file__).resolve().parent.parent
DATA_DIR = REPO_ROOT / "backend" / "app" / "data"
OUT_PATH = DATA_DIR / "rf_reference.json"
OVERRIDES_PATH = DATA_DIR / "rf_overrides.yaml"
LOCAL_MANUF = DATA_DIR / "manuf.txt"
LOCAL_BLE_COMPANY_IDS = REPO_ROOT / "backend" / "app" / "services" / "ble_company_ids.json"

SOURCES = {
    "ieee_ma_l": "https://standards-oui.ieee.org/oui/oui.csv",
    "ieee_ma_m": "https://standards-oui.ieee.org/oui28/mam.csv",
    "ieee_ma_s": "https://standards-oui.ieee.org/oui36/oui36.csv",
    "ieee_cid": "https://standards-oui.ieee.org/cid/cid.csv",
    "wireshark_manuf": "https://www.wireshark.org/download/automated/data/manuf",
    "nmap_mac_prefixes": "https://raw.githubusercontent.com/nmap/nmap/master/nmap-mac-prefixes",
    "bluetooth_company_ids": "https://bitbucket.org/bluetooth-SIG/public/raw/main/assigned_numbers/company_identifiers/company_identifiers.yaml",
}


@dataclass
class PrefixEntry:
    prefix: str
    bits: int
    vendor_short: str = ""
    vendor_long: str = ""
    assignment_type: str = ""
    sources: set[str] = field(default_factory=set)
    aliases: set[str] = field(default_factory=set)
    product_hint: str | None = None
    device_family: str | None = None
    device_class: str | None = None


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _download(url: str, *, timeout: int = 90) -> bytes:
    req = Request(
        url,
        headers={
            "User-Agent": "friendorfoe-rf-reference-builder/1.0",
            "Accept": "text/csv,text/plain,application/yaml,application/octet-stream,*/*",
        },
    )
    with urlopen(req, timeout=timeout) as resp:
        return resp.read()


def _normalize_hex(value: str) -> str | None:
    hex_only = re.sub(r"[^0-9A-Fa-f]", "", value or "").upper()
    return hex_only or None


def _bits_from_registry(registry: str, default: int) -> int:
    r = (registry or "").upper()
    if "MA-S" in r or "OUI-36" in r:
        return 36
    if "MA-M" in r or "OUI-28" in r:
        return 28
    return default


def _format_short_name(name: str) -> str:
    cleaned = re.sub(r"\s+", " ", (name or "").strip())
    if not cleaned:
        return ""
    replacements = {
        "Espressif Inc.": "Espressif",
        "Apple, Inc.": "Apple",
        "Apple Inc.": "Apple",
        "Amazon Technologies Inc.": "Amazon",
        "NETGEAR": "Netgear",
    }
    if cleaned in replacements:
        return replacements[cleaned]
    # Keep Wireshark/Nmap-style concise display names.
    return cleaned[:24].strip()


def _upsert(
    entries: dict[tuple[str, int], PrefixEntry],
    *,
    prefix: str,
    bits: int,
    vendor_short: str,
    vendor_long: str,
    assignment_type: str,
    source: str,
) -> None:
    key = (prefix[: (bits + 3) // 4], bits)
    entry = entries.get(key)
    if not entry:
        entry = PrefixEntry(prefix=key[0], bits=bits, assignment_type=assignment_type)
        entries[key] = entry
    entry.sources.add(source)
    entry.assignment_type = entry.assignment_type or assignment_type
    if vendor_short:
        entry.aliases.add(vendor_short)
    if vendor_long:
        entry.aliases.add(vendor_long)
    # IEEE long names are the registry authority; Wireshark/Nmap short names
    # are usually better for operator display.
    if source.startswith("ieee"):
        entry.vendor_long = entry.vendor_long or vendor_long or vendor_short
        entry.vendor_short = entry.vendor_short or _format_short_name(vendor_short or vendor_long)
    elif source == "wireshark_manuf":
        entry.vendor_short = vendor_short or entry.vendor_short
        entry.vendor_long = entry.vendor_long or vendor_long or vendor_short
    elif source == "nmap_mac_prefixes":
        entry.vendor_short = entry.vendor_short or vendor_short
        entry.vendor_long = entry.vendor_long or vendor_long or vendor_short


def _parse_ieee_csv(data: bytes, source: str, default_bits: int) -> list[tuple[str, int, str, str, str]]:
    rows: list[tuple[str, int, str, str, str]] = []
    text = data.decode("utf-8", errors="replace")
    for row in csv.DictReader(text.splitlines()):
        assignment = _normalize_hex(row.get("Assignment", ""))
        org = (row.get("Organization Name") or "").strip()
        registry = (row.get("Registry") or "").strip()
        if not assignment or not org:
            continue
        bits = _bits_from_registry(registry, default_bits)
        rows.append((assignment[: (bits + 3) // 4], bits, _format_short_name(org), org, registry or source))
    return rows


def _parse_manuf(data: bytes) -> list[tuple[str, int, str, str]]:
    rows: list[tuple[str, int, str, str]] = []
    for raw in data.decode("utf-8", errors="replace").splitlines():
        if not raw or raw.startswith("#"):
            continue
        parts = [p.strip() for p in raw.split("\t") if p.strip()]
        if len(parts) < 2:
            continue
        prefix_raw = parts[0]
        bits = 24
        if "/" in prefix_raw:
            prefix_raw, bit_s = prefix_raw.split("/", 1)
            try:
                bits = int(bit_s)
            except ValueError:
                bits = 24
        prefix = _normalize_hex(prefix_raw)
        if not prefix:
            continue
        short = parts[1]
        long = parts[2] if len(parts) >= 3 else short
        rows.append((prefix[: (bits + 3) // 4], bits, short, long))
    return rows


def _parse_nmap(data: bytes) -> list[tuple[str, int, str, str]]:
    rows: list[tuple[str, int, str, str]] = []
    for raw in data.decode("utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        if len(parts) != 2:
            continue
        prefix = _normalize_hex(parts[0])
        if not prefix:
            continue
        rows.append((prefix[:6], 24, parts[1].strip(), parts[1].strip()))
    return rows


def _parse_ble_company_yaml(data: bytes) -> dict[str, dict[str, str | int]]:
    # The Bluetooth SIG file is simple enough that a regex parser avoids a
    # runtime PyYAML dependency in this repo.
    text = data.decode("utf-8", errors="replace")
    result: dict[str, dict[str, str | int]] = {}
    current: int | None = None
    for line in text.splitlines():
        value_match = re.search(r"value:\s*0x([0-9A-Fa-f]+)", line)
        if value_match:
            current = int(value_match.group(1), 16)
            continue
        name_match = re.search(r"name:\s*['\"]?(.+?)['\"]?\s*$", line)
        if name_match and current is not None:
            name = name_match.group(1).strip().strip("'\"")
            result[str(current)] = {"code": current, "name": name, "source": "bluetooth_sig"}
            current = None
    return result


def _parse_local_ble_company_json(path: Path) -> dict[str, dict[str, str | int]]:
    if not path.exists():
        return {}
    payload = json.loads(path.read_text())
    result: dict[str, dict[str, str | int]] = {}
    items = payload.get("company_ids", payload) if isinstance(payload, dict) else payload
    for item in items:
        try:
            code = int(item["code"])
        except Exception:
            continue
        result[str(code)] = {"code": code, "name": str(item.get("name") or ""), "source": "local_ble_company_ids"}
    return result


def _load_overrides(path: Path) -> dict[str, object]:
    # Minimal parser for the constrained rf_overrides.yaml structure above.
    if not path.exists():
        return {"prefix_overrides": {}, "ssid_patterns": []}
    prefix_overrides: dict[str, dict[str, object]] = {}
    ssid_patterns: list[dict[str, object]] = []
    section = ""
    current_key: str | None = None
    current_item: dict[str, object] | None = None
    for raw in path.read_text().splitlines():
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        if raw.startswith("prefix_overrides:"):
            section = "prefix_overrides"
            current_key = None
            continue
        if raw.startswith("ssid_patterns:"):
            section = "ssid_patterns"
            current_key = None
            continue
        if section == "prefix_overrides":
            key_match = re.match(r'\s{2}"?([^":]+(?::[0-9A-Fa-f]{2})*)"?\s*:\s*$', raw)
            if key_match:
                current_key = key_match.group(1).upper()
                prefix_overrides[current_key] = {}
                continue
            kv = re.match(r"\s{4}([a-zA-Z_]+):\s*(.+?)\s*$", raw)
            if kv and current_key:
                prefix_overrides[current_key][kv.group(1)] = kv.group(2).strip().strip('"')
        elif section == "ssid_patterns":
            item_match = re.match(r"\s{2}-\s+pattern:\s*(.+?)\s*$", raw)
            if item_match:
                current_item = {"pattern": item_match.group(1).strip().strip('"')}
                ssid_patterns.append(current_item)
                continue
            kv = re.match(r"\s{4}([a-zA-Z_]+):\s*(.+?)\s*$", raw)
            if kv and current_item is not None:
                value: object = kv.group(2).strip().strip('"')
                if kv.group(1) == "confidence":
                    try:
                        value = float(value)
                    except ValueError:
                        pass
                current_item[kv.group(1)] = value
    return {"prefix_overrides": prefix_overrides, "ssid_patterns": ssid_patterns}


def _source_bytes(args: argparse.Namespace, source: str) -> bytes:
    if args.local_only:
        if source == "wireshark_manuf":
            return LOCAL_MANUF.read_bytes()
        if source == "bluetooth_company_ids":
            return LOCAL_BLE_COMPANY_IDS.read_bytes()
        raise RuntimeError(f"{source} unavailable in --local-only mode")
    try:
        return _download(SOURCES[source])
    except (HTTPError, URLError, TimeoutError) as exc:
        if source == "wireshark_manuf" and LOCAL_MANUF.exists():
            print(f"warning: {source} fetch failed ({exc}); using {LOCAL_MANUF}", file=sys.stderr)
            return LOCAL_MANUF.read_bytes()
        if source == "bluetooth_company_ids" and LOCAL_BLE_COMPANY_IDS.exists():
            print(f"warning: {source} fetch failed ({exc}); using {LOCAL_BLE_COMPANY_IDS}", file=sys.stderr)
            return LOCAL_BLE_COMPANY_IDS.read_bytes()
        raise


def build(args: argparse.Namespace) -> dict:
    entries: dict[tuple[str, int], PrefixEntry] = {}
    source_meta: dict[str, dict[str, object]] = {}

    for source, default_bits, assignment_type in (
        ("ieee_ma_l", 24, "MA-L"),
        ("ieee_ma_m", 28, "MA-M"),
        ("ieee_ma_s", 36, "MA-S"),
        ("ieee_cid", 24, "CID"),
    ):
        if args.local_only:
            continue
        data = _source_bytes(args, source)
        source_meta[source] = {"url": SOURCES[source], "bytes": len(data), "sha256": _sha256(data)}
        for prefix, bits, short, long, registry in _parse_ieee_csv(data, source, default_bits):
            _upsert(
                entries,
                prefix=prefix,
                bits=bits,
                vendor_short=short,
                vendor_long=long,
                assignment_type=registry or assignment_type,
                source=source,
            )

    manuf_data = _source_bytes(args, "wireshark_manuf")
    source_meta["wireshark_manuf"] = {
        "url": SOURCES["wireshark_manuf"] if not args.local_only else str(LOCAL_MANUF),
        "bytes": len(manuf_data),
        "sha256": _sha256(manuf_data),
    }
    for prefix, bits, short, long in _parse_manuf(manuf_data):
        _upsert(
            entries,
            prefix=prefix,
            bits=bits,
            vendor_short=short,
            vendor_long=long,
            assignment_type="Wireshark",
            source="wireshark_manuf",
        )

    if not args.local_only:
        nmap_data = _source_bytes(args, "nmap_mac_prefixes")
        source_meta["nmap_mac_prefixes"] = {"url": SOURCES["nmap_mac_prefixes"], "bytes": len(nmap_data), "sha256": _sha256(nmap_data)}
        for prefix, bits, short, long in _parse_nmap(nmap_data):
            _upsert(
                entries,
                prefix=prefix,
                bits=bits,
                vendor_short=short,
                vendor_long=long,
                assignment_type="Nmap",
                source="nmap_mac_prefixes",
            )

    ble_data = _source_bytes(args, "bluetooth_company_ids")
    source_meta["bluetooth_company_ids"] = {
        "url": SOURCES["bluetooth_company_ids"] if not args.local_only else str(LOCAL_BLE_COMPANY_IDS),
        "bytes": len(ble_data),
        "sha256": _sha256(ble_data),
    }
    ble_company_ids = (
        _parse_local_ble_company_json(LOCAL_BLE_COMPANY_IDS)
        if args.local_only
        else _parse_ble_company_yaml(ble_data)
    )

    overrides = _load_overrides(OVERRIDES_PATH)
    for prefix_text, override in dict(overrides.get("prefix_overrides", {})).items():
        prefix = _normalize_hex(prefix_text)
        if not prefix:
            continue
        bits = len(prefix) * 4
        key = (prefix, bits)
        entry = entries.get(key)
        if not entry:
            entry = PrefixEntry(prefix=prefix, bits=bits, assignment_type="operator_override")
            entries[key] = entry
        entry.sources.add("operator_override")
        override_d = dict(override)
        if override_d.get("brand"):
            entry.vendor_short = str(override_d["brand"])
            entry.aliases.add(str(override_d["brand"]))
        entry.product_hint = str(override_d.get("product_hint") or "") or entry.product_hint
        entry.device_family = str(override_d.get("device_family") or "") or entry.device_family
        entry.device_class = str(override_d.get("device_class") or "") or entry.device_class

    prefix_rows = []
    for entry in sorted(entries.values(), key=lambda e: (e.prefix, e.bits)):
        prefix_rows.append({
            "prefix": entry.prefix,
            "bits": entry.bits,
            "vendor_short": entry.vendor_short or _format_short_name(entry.vendor_long),
            "vendor_long": entry.vendor_long or entry.vendor_short,
            "vendor_aliases": sorted(a for a in entry.aliases if a),
            "vendor_source": "+".join(sorted(entry.sources)),
            "assignment_type": entry.assignment_type,
            "product_hint": entry.product_hint,
            "device_family": entry.device_family,
            "device_class": entry.device_class,
        })

    generated_at = datetime.now(timezone.utc).isoformat()
    return {
        "schema_version": 1,
        "generated_at": generated_at,
        "sources": source_meta,
        "source_entry_counts": {
            "mac_prefixes": len(prefix_rows),
            "ble_company_ids": len(ble_company_ids),
            "ssid_patterns": len(overrides.get("ssid_patterns", [])),
            "prefix_overrides": len(overrides.get("prefix_overrides", {})),
        },
        "mac_prefixes": prefix_rows,
        "ble_company_ids": ble_company_ids,
        "ssid_patterns": overrides.get("ssid_patterns", []),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=OUT_PATH)
    parser.add_argument("--local-only", action="store_true", help="Use committed local inputs only.")
    args = parser.parse_args(argv)
    t0 = time.time()
    artifact = build(args)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    tmp = args.output.with_suffix(args.output.suffix + ".new")
    tmp.write_text(json.dumps(artifact, indent=2, sort_keys=True) + "\n")
    tmp.replace(args.output)
    print(
        f"wrote {args.output} with {artifact['source_entry_counts']['mac_prefixes']:,} "
        f"MAC prefixes and {artifact['source_entry_counts']['ble_company_ids']:,} BLE company IDs "
        f"in {time.time() - t0:.1f}s"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
