"""Strict parser and fail-closed classifier for blank badge UART topology."""

from __future__ import annotations

import hmac
import json
import re
import zlib
from collections.abc import Iterable, Mapping

from .models import ProbeReport, TopologyAssignment


PROBE_PREFIX = "FOF_FACTORY_PROBE:"
PROBE_SCHEMA = 1
_REPORT_FIELDS = frozenset({"schema", "session", "mac", "peers", "crc32"})
_LINKS = frozenset({"a", "b"})
_SESSION_RE = re.compile(r"^[0-9a-fA-F]{32}$")
_CRC_RE = re.compile(r"^[0-9a-fA-F]{8}$")
_MAC_HEX_RE = re.compile(r"^[0-9a-fA-F]{12}$")


class TopologyError(ValueError):
    """The probe reports do not prove one complete badge wiring graph."""


def normalize_mac(value: str) -> str:
    if not isinstance(value, str):
        raise TopologyError("probe MAC must be a string")
    compact = value.replace(":", "").replace("-", "").strip()
    if not _MAC_HEX_RE.fullmatch(compact):
        raise TopologyError(f"invalid probe MAC: {value!r}")
    compact = compact.upper()
    return ":".join(compact[index:index + 2] for index in range(0, 12, 2))


def _canonical_crc_payload(payload: Mapping[str, object]) -> bytes:
    without_crc = {key: value for key, value in payload.items() if key != "crc32"}
    try:
        rendered = json.dumps(
            without_crc,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=True,
        )
        return rendered.encode("ascii")
    except (TypeError, UnicodeEncodeError) as exc:
        raise TopologyError("probe report is not canonical ASCII JSON") from exc


def parse_probe_report(line: str, expected_session: str) -> ProbeReport:
    """Parse one nonce-bound, CRC-protected USB report line."""
    if not isinstance(line, str) or not line.startswith(PROBE_PREFIX):
        raise TopologyError("probe report prefix is missing")
    if not _SESSION_RE.fullmatch(expected_session or ""):
        raise TopologyError("expected probe session must be 32 hexadecimal characters")

    try:
        payload = json.loads(line[len(PROBE_PREFIX):])
    except json.JSONDecodeError as exc:
        raise TopologyError("probe report JSON is malformed") from exc
    if not isinstance(payload, dict):
        raise TopologyError("probe report must be a JSON object")
    if set(payload) != _REPORT_FIELDS:
        raise TopologyError("probe report fields do not match schema 1")
    if type(payload.get("schema")) is not int or payload["schema"] != PROBE_SCHEMA:
        raise TopologyError(f"unsupported probe schema: {payload.get('schema')!r}")

    session = payload.get("session")
    if not isinstance(session, str) or not _SESSION_RE.fullmatch(session):
        raise TopologyError("probe session is malformed")
    session = session.lower()
    if not hmac.compare_digest(session, expected_session.lower()):
        raise TopologyError("probe report belongs to a different session")

    crc_text = payload.get("crc32")
    if not isinstance(crc_text, str) or not _CRC_RE.fullmatch(crc_text):
        raise TopologyError("probe report CRC is malformed")
    wanted_crc = f"{zlib.crc32(_canonical_crc_payload(payload)) & 0xFFFFFFFF:08x}"
    if not hmac.compare_digest(crc_text.lower(), wanted_crc):
        raise TopologyError("probe report CRC mismatch")

    mac = normalize_mac(payload.get("mac"))  # type: ignore[arg-type]
    peers_raw = payload.get("peers")
    if not isinstance(peers_raw, dict):
        raise TopologyError("probe peers must be an object")
    if any(not isinstance(link, str) or link not in _LINKS for link in peers_raw):
        raise TopologyError("probe peer link must be 'a' or 'b'")

    peers: dict[str, str] = {}
    for link, peer_value in peers_raw.items():
        peer = normalize_mac(peer_value)
        if peer == mac:
            raise TopologyError("probe report contains a self peer")
        peers[link] = peer
    if len(set(peers.values())) != len(peers):
        raise TopologyError("probe peers must be distinct")
    return ProbeReport(mac=mac, session=session, peers=peers)


def classify_topology(reports: Iterable[ProbeReport]) -> TopologyAssignment:
    """Require and classify one reciprocal three-node badge UART star."""
    items = list(reports)
    if len(items) != 3:
        raise TopologyError(
            f"exactly three probe reports are required; received {len(items)}"
        )

    sessions = {item.session.lower() for item in items}
    if len(sessions) != 1 or not all(_SESSION_RE.fullmatch(value) for value in sessions):
        raise TopologyError("all probe reports must use the same valid session")

    by_mac: dict[str, ProbeReport] = {}
    for item in items:
        mac = normalize_mac(item.mac)
        if mac in by_mac:
            raise TopologyError(f"duplicate probe MAC: {mac}")
        normalized_peers = {
            link: normalize_mac(peer) for link, peer in item.peers.items()
        }
        if any(link not in _LINKS for link in normalized_peers):
            raise TopologyError(f"unknown UART link in report from {mac}")
        if mac in normalized_peers.values():
            raise TopologyError(f"self peer in report from {mac}")
        if len(set(normalized_peers.values())) != len(normalized_peers):
            raise TopologyError(f"duplicate peer in report from {mac}")
        by_mac[mac] = ProbeReport(mac, item.session.lower(), normalized_peers)

    known = set(by_mac)
    for item in by_mac.values():
        for peer in item.peers.values():
            if peer not in known:
                raise TopologyError(
                    f"unknown peer {peer} reported by {item.mac}"
                )

    centers = [item for item in by_mac.values() if len(item.peers) == 2]
    if len(centers) != 1:
        raise TopologyError(
            "topology must contain exactly one two-peer uplink center with two peers"
        )
    center = centers[0]
    if set(center.peers) != _LINKS:
        raise TopologyError("uplink center must prove both link A and link B")

    ble_mac = center.peers["a"]
    wifi_mac = center.peers["b"]
    for label, leaf_mac in (("BLE", ble_mac), ("Wi-Fi", wifi_mac)):
        leaf = by_mac[leaf_mac]
        if len(leaf.peers) != 1 or leaf.peers.get("a") != center.mac:
            if "b" in leaf.peers and "a" not in leaf.peers:
                raise TopologyError(
                    f"{label} scanner leaf must reciprocate on link A"
                )
            raise TopologyError(
                f"{label} scanner does not provide reciprocal proof to {center.mac}"
            )

    leaves = known - {center.mac}
    if leaves != {ble_mac, wifi_mac}:
        raise TopologyError("center peers do not cover both scanner leaves")

    return TopologyAssignment(
        uplink_mac=center.mac,
        ble_leaf_mac=ble_mac,
        wifi_leaf_mac=wifi_mac,
    )
