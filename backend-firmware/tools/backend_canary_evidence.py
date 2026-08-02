#!/usr/bin/env python3
"""Deterministic, read-only evidence capture for the three-board Lite canary.

This module never flashes, erases, provisions, or otherwise writes a device.
The initial canary write path is the separately guarded direct-USB tool in
``backend_canary.py``.  Native badge 0.67.2 remains the default USB/factory
firmware; this recorder accepts only the explicit backend firmware family.
"""

from __future__ import annotations

import argparse
import contextlib
from dataclasses import asdict, dataclass
from email.utils import parsedate_to_datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import stat
import sys
import time
from typing import Any, Callable, Mapping, Sequence
import urllib.error
import urllib.parse
import urllib.request


BACKEND_VERSION = "0.1.0-backend"
BACKEND_HARDWARE = "seeed_xiao_esp32s3"
UPLINK_IDENTITY = (
    "uplink-s3-backend",
    "fof_backend_uplink",
    BACKEND_HARDWARE,
    BACKEND_VERSION,
)
SCANNER_IDENTITY = (
    "scanner-s3-combo-backend",
    "fof_backend_scanner",
    BACKEND_HARDWARE,
    BACKEND_VERSION,
)
SCANNER_PROFILES = ("ble_primary", "wifi_primary")
LED_STATES = frozenset(
    {
        "healthy",
        "drone",
        "meta",
        "drone_meta",
        "network_degraded",
        "uart_lost",
        "fatal",
    }
)
TERMINAL_STATES = frozenset({"complete", "failed", "cancelled"})
MIN_EPOCH_SECONDS = 1_700_000_000
MAX_EPOCH_SECONDS = 4_102_444_800  # 2100-01-01, well beyond device support.
MIN_EPOCH_MS = MIN_EPOCH_SECONDS * 1000
MAX_EPOCH_MS = MAX_EPOCH_SECONDS * 1000 + 999
MAX_HTTP_JSON = 8 * 1024 * 1024
HTTP_TIMEOUT_SECONDS = 10
POLL_INTERVAL_SECONDS = 0.5
SERIAL_LOG_MAX_AGE_MS = 300_000
SERIAL_LOG_ROLES = ("scanner0", "scanner1", "uplink")
HEALTHY_SNAPSHOT_PHASES = frozenset({
    "baseline", "drone", "meta", "drone-meta", "outage-start",
    "outage-end", "network-recovery", "scanner0-reconnected",
    "fatal-recovered",
})
DEGRADED_SNAPSHOT_PHASES = {
    "scanner0-disconnected": ((False, True), "uart_lost"),
    "both-scanners-disconnected": ((False, False), "fatal"),
}
ZERO_QUEUE_PHASES = frozenset({"baseline", "network-recovery"})
HEALTHY_LED_PHASES = frozenset({
    "baseline", "network-recovery", "scanner0-reconnected", "fatal-recovered",
})
THREAT_SNAPSHOT_LEDS = {
    "drone": "drone",
    "meta": "meta",
    "drone-meta": "drone_meta",
}

SECRET_KEY = re.compile(
    r"password|secret|credential|token|authorization|cookie|set-cookie|api_key",
    re.IGNORECASE,
)
NORMALIZED_SECRET_MARKERS = (
    "password",
    "secret",
    "credential",
    "token",
    "authorization",
    "cookie",
    "setcookie",
    "apikey",
    "wifipass",
    "appass",
)
RAW_BLE_KEY = re.compile(
    r"^(?:value_hex|ble_raw(?:_.*)?|raw_ble(?:_.*)?|characteristic_value|"
    r"ble_apple_auth|ble_auth_payload|raw_auth_payload|auth(?:entication)?_value)$",
    re.IGNORECASE,
)
MAC_RE = re.compile(r"^[0-9A-F]{2}(?::[0-9A-F]{2}){5}$")
COMMAND_ID_RE = re.compile(r"^[0-9a-f]{32}$")
CONTINUITY_KEYS = {
    "schema",
    "device_id",
    "calibration_status",
    "session_id",
    "applied_at",
    "listener_model_present",
    "listener_model_schema",
    "listener_model_sha256",
}


class EvidenceError(RuntimeError):
    """Evidence is missing, ambiguous, unsafe, or violates a canary gate."""


@dataclass(frozen=True)
class FetchResponse:
    payload: Any
    raw: bytes
    server_timestamp_ms: int | None = None


@dataclass(frozen=True)
class SoakResult:
    device_id: str
    duration_s: int
    samples: int
    max_heartbeat_gap_s: int
    final_queue_depth: int


FetchJson = Callable[..., Any]


def _canonical_json_bytes(value: Any) -> bytes:
    try:
        return json.dumps(
            value,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=False,
            allow_nan=False,
        ).encode("utf-8")
    except (TypeError, ValueError) as exc:
        raise EvidenceError("evidence is not canonical JSON") from exc


def _reject_duplicate_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise EvidenceError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _decode_json(raw: bytes, label: str) -> Any:
    try:
        return json.loads(
            raw.decode("utf-8"), object_pairs_hook=_reject_duplicate_pairs,
        )
    except EvidenceError:
        raise
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise EvidenceError(f"{label} is not valid JSON") from exc


def _is_forbidden_key(key: str) -> bool:
    normalized = re.sub(r"[^a-z0-9]", "", key.casefold())
    return bool(
        SECRET_KEY.search(key)
        or any(marker in normalized for marker in NORMALIZED_SECRET_MARKERS)
        or RAW_BLE_KEY.search(key)
    )


def contains_secret_key(value: Any) -> bool:
    """Return whether a nested value still contains a secret/raw-value key."""
    if isinstance(value, Mapping):
        return any(
            _is_forbidden_key(str(key)) or contains_secret_key(child)
            for key, child in value.items()
        )
    if isinstance(value, (list, tuple)):
        return any(contains_secret_key(child) for child in value)
    return False


def redact_secrets(value: Any) -> Any:
    """Recursively copy JSON-compatible data while dropping forbidden keys."""
    if isinstance(value, Mapping):
        return {
            str(key): redact_secrets(child)
            for key, child in value.items()
            if not _is_forbidden_key(str(key))
        }
    if isinstance(value, (list, tuple)):
        return [redact_secrets(child) for child in value]
    return value


def _private_mode(path: Path, expected: int, label: str) -> None:
    try:
        info = path.lstat()
    except OSError as exc:
        raise EvidenceError(f"{label} is unavailable") from exc
    if stat.S_ISLNK(info.st_mode):
        raise EvidenceError(f"{label} must not be a symlink")
    if stat.S_IMODE(info.st_mode) != expected:
        raise EvidenceError(f"{label} must have mode {expected:o}")


def _require_canary_storage(path: Path, label: str) -> Path:
    destination = path.expanduser().resolve()
    if ".canary" not in destination.parts:
        raise EvidenceError(f"{label} must be inside the ignored .canary directory")
    return destination


def _secure_directory(path: Path) -> Path:
    destination = path.expanduser().resolve()
    destination.mkdir(parents=True, exist_ok=True, mode=0o700)
    if destination.is_symlink() or not destination.is_dir():
        raise EvidenceError("evidence directory must be a real directory")
    # The selected evidence directory belongs to this command. Tighten a
    # newly or permissively created directory rather than writing into it.
    os.chmod(destination, 0o700)
    _private_mode(destination, 0o700, "evidence directory")
    return destination


def _write_all(descriptor: int, payload: bytes) -> None:
    view = memoryview(payload)
    while view:
        count = os.write(descriptor, view)
        if count <= 0:
            raise EvidenceError("short evidence write")
        view = view[count:]


def _secure_append_json(path: Path, value: Mapping[str, Any]) -> None:
    destination = _require_canary_storage(path, "evidence output")
    parent = _secure_directory(destination.parent)
    if destination.parent != parent:
        raise EvidenceError("evidence output escapes its directory")
    if destination.exists():
        _private_mode(destination, 0o600, "evidence file")
        if not destination.is_file():
            raise EvidenceError("evidence output is not a regular file")
    flags = os.O_WRONLY | os.O_APPEND | os.O_CREAT
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(destination, flags, 0o600)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        os.fchmod(descriptor, 0o600)
        _write_all(descriptor, _canonical_json_bytes(value) + b"\n")
        os.fsync(descriptor)
    finally:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_UN)
        except OSError:
            pass
        os.close(descriptor)
    _private_mode(destination, 0o600, "evidence file")
    _fsync_directory(parent)


def _fsync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _create_ready_file(
    path: Path,
    payload: Mapping[str, Any],
    *,
    requested_after_ms: int,
    now: Callable[[], float],
) -> int:
    destination = _require_canary_storage(path, "ready file")
    parent = _secure_directory(destination.parent)
    if os.path.lexists(destination):
        raise EvidenceError("ready file already exists")
    temporary = parent / f".{destination.name}.{secrets.token_hex(8)}.tmp"
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(temporary, flags, 0o600)
    except FileExistsError as exc:
        raise EvidenceError("ready file temporary collision") from exc
    try:
        os.fchmod(descriptor, 0o600)
        prepared_ms = int(now() * 1000)
        ready_cutoff_ms = max(
            requested_after_ms,
            (prepared_ms // 1000 + 5) * 1000,
        )
        ready = dict(payload)
        ready.update(
            state="ready",
            requested_after_ms=requested_after_ms,
            ready_cutoff_ms=ready_cutoff_ms,
        )
        _write_all(descriptor, _canonical_json_bytes(ready) + b"\n")
        os.fsync(descriptor)
        prepared_stat = os.fstat(descriptor)
    finally:
        os.close(descriptor)
    published_binding: tuple[int, int] | None = None
    try:
        if int(now() * 1000) >= ready_cutoff_ms:
            raise EvidenceError("ready cutoff elapsed before atomic publication")
        try:
            os.link(temporary, destination, follow_symlinks=False)
        except FileExistsError as exc:
            raise EvidenceError("ready file already exists") from exc
        published_binding = (prepared_stat.st_dev, prepared_stat.st_ino)
        _fsync_directory(parent)
        if int(now() * 1000) >= ready_cutoff_ms:
            raise EvidenceError("ready cutoff elapsed during atomic publication")
        temporary.unlink()
        _fsync_directory(parent)
        return ready_cutoff_ms
    except BaseException:
        if published_binding is not None:
            try:
                destination_stat = destination.lstat()
            except FileNotFoundError:
                pass
            else:
                if (
                    not stat.S_ISLNK(destination_stat.st_mode)
                    and (destination_stat.st_dev, destination_stat.st_ino)
                    == published_binding
                ):
                    destination.unlink()
                    with contextlib.suppress(OSError):
                        _fsync_directory(parent)
        with contextlib.suppress(OSError):
            temporary.unlink()
        with contextlib.suppress(OSError):
            _fsync_directory(parent)
        raise
    _private_mode(destination, 0o600, "ready file")


def _normalize_backend_base(value: str) -> str:
    try:
        parsed = urllib.parse.urlsplit(value.strip())
    except ValueError as exc:
        raise EvidenceError("backend base URL is invalid") from exc
    if (
        parsed.scheme not in ("http", "https")
        or not parsed.netloc
        or parsed.username is not None
        or parsed.password is not None
        or parsed.query
        or parsed.fragment
    ):
        raise EvidenceError("backend base URL must be an HTTP(S) origin/path")
    normalized_path = parsed.path.rstrip("/")
    return urllib.parse.urlunsplit(
        (parsed.scheme, parsed.netloc, normalized_path, "", "")
    )


def _http_fetch_json(url: str, *, timeout: int) -> FetchResponse:
    request = urllib.request.Request(
        url,
        method="GET",
        headers={"Accept": "application/json", "User-Agent": "fof-backend-canary/1"},
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            status_code = getattr(response, "status", 200)
            if status_code != 200:
                raise EvidenceError(f"backend GET returned HTTP {status_code}")
            raw = response.read(MAX_HTTP_JSON + 1)
            if len(raw) > MAX_HTTP_JSON:
                raise EvidenceError("backend JSON response exceeds size limit")
            server_ms = None
            date_value = response.headers.get("Date")
            if date_value:
                try:
                    server_ms = int(parsedate_to_datetime(date_value).timestamp() * 1000)
                except (TypeError, ValueError, OverflowError):
                    server_ms = None
    except EvidenceError:
        raise
    except (urllib.error.URLError, OSError) as exc:
        raise EvidenceError("backend GET failed") from exc
    return FetchResponse(
        payload=_decode_json(raw, "backend response"),
        raw=raw,
        server_timestamp_ms=server_ms,
    )


def _fetch(
    fetch_json: FetchJson,
    url: str,
) -> FetchResponse:
    try:
        result = fetch_json(url, timeout=HTTP_TIMEOUT_SECONDS)
    except EvidenceError:
        raise
    except Exception as exc:
        raise EvidenceError("backend GET failed") from exc
    if isinstance(result, FetchResponse):
        response = result
    elif (
        isinstance(result, tuple)
        and len(result) in (2, 3)
        and isinstance(result[1], (bytes, bytearray))
    ):
        response = FetchResponse(
            result[0], bytes(result[1]), result[2] if len(result) == 3 else None,
        )
    else:
        response = FetchResponse(result, _canonical_json_bytes(result), None)
    if len(response.raw) > MAX_HTTP_JSON:
        raise EvidenceError("backend JSON response exceeds size limit")
    return response


def _sha256(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def _required_mapping(value: Any, label: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise EvidenceError(f"{label} must be an object")
    return value


def _required_int(value: Any, label: str, *, minimum: int = 0) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise EvidenceError(f"{label} must be an integer >= {minimum}")
    return value


def _required_mac(value: Any, label: str) -> str:
    if not isinstance(value, str) or not MAC_RE.fullmatch(value.upper()):
        raise EvidenceError(f"{label} must be a canonical MAC")
    return value.upper()


def _epoch_seconds_to_ms(value: Any, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise EvidenceError(f"{label} timestamp must be numeric")
    if value < MIN_EPOCH_SECONDS or value > MAX_EPOCH_SECONDS:
        raise EvidenceError(f"{label} timestamp is outside the supported epoch")
    milliseconds = int(value * 1000)
    if milliseconds < MIN_EPOCH_MS or milliseconds > MAX_EPOCH_MS:
        raise EvidenceError(f"{label} timestamp overflow")
    return milliseconds


def normalize_history_timestamp_ms(row: Mapping[str, Any]) -> int:
    """Normalize one history row without guessing seconds vs milliseconds."""
    if not isinstance(row, Mapping):
        raise EvidenceError("history row timestamp is unavailable")
    millisecond_keys = [
        key for key in row
        if isinstance(key, str) and key.endswith("_ms") and row[key] is not None
    ]
    primary_ms_keys = [
        key for key in millisecond_keys
        if key in ("timestamp_ms", "observed_at_ms", "received_at_ms")
    ]
    if "timestamp" in row and row.get("timestamp") is not None:
        if primary_ms_keys:
            raise EvidenceError("history timestamp has ambiguous mixed units")
        value = row["timestamp"]
        if isinstance(value, bool) or not isinstance(value, int):
            raise EvidenceError("history timestamp seconds must be an integer")
        if value < MIN_EPOCH_SECONDS or value > MAX_EPOCH_SECONDS:
            raise EvidenceError("history timestamp seconds is outside the supported epoch")
        normalized = value * 1000
        if normalized > MAX_EPOCH_MS:
            raise EvidenceError("history timestamp multiplication overflow")
        for key in millisecond_keys:
            ms_value = row[key]
            if (
                isinstance(ms_value, bool)
                or not isinstance(ms_value, int)
                or ms_value < MIN_EPOCH_MS
                or ms_value > MAX_EPOCH_MS
            ):
                raise EvidenceError(f"history {key} timestamp is not epoch milliseconds")
        return normalized
    if len(millisecond_keys) != 1:
        raise EvidenceError("history timestamp is missing or ambiguous")
    value = row[millisecond_keys[0]]
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or value < MIN_EPOCH_MS
        or value > MAX_EPOCH_MS
    ):
        raise EvidenceError("history millisecond timestamp is outside the supported epoch")
    return value


def _service_uuid_set(value: Any) -> set[str]:
    if isinstance(value, str):
        values: Sequence[Any] = value.split(",")
    elif isinstance(value, (list, tuple)):
        values = value
    else:
        return set()
    result: set[str] = set()
    for item in values:
        token = str(item).strip().lower()
        if not token:
            continue
        result.add(token)
        compact = token.replace("-", "")
        if compact.startswith("0000") and len(compact) == 32:
            result.add(compact[4:8])
    return result


def find_matching_detection(
    history: Mapping[str, Any],
    *,
    device_id: str,
    kind: str,
    source: str,
    identity_field: str,
    identity_value: str,
    after_ms: int,
    manufacturer: str | None = None,
    service_uuid_token: str | None = None,
) -> dict[str, Any] | None:
    """Return one exact, cutoff-safe detection or ``None`` for unrelated rows."""
    if kind not in ("drone", "meta"):
        raise EvidenceError("detection kind must be drone or meta")
    expected_source = "wifi_beacon_rid" if kind == "drone" else "ble_fingerprint"
    expected_field = "drone_id" if kind == "drone" else "bssid"
    if source != expected_source or identity_field != expected_field:
        raise EvidenceError("detection source/identity contract is not exact")
    _required_int(after_ms, "after_ms", minimum=MIN_EPOCH_MS)
    if after_ms > MAX_EPOCH_MS:
        raise EvidenceError("after_ms timestamp overflow")
    if kind == "meta" and (
        manufacturer != "Meta Glasses"
        or not isinstance(service_uuid_token, str)
        or service_uuid_token.lower() != "fd5f"
    ):
        raise EvidenceError("Meta evidence requires exact manufacturer and fd5f UUID")
    document = _required_mapping(history, "history response")
    rows = document.get("detections")
    if not isinstance(rows, list):
        raise EvidenceError("history response has no detections list")
    for raw_row in rows:
        row = _required_mapping(raw_row, "history row")
        normalized_ms = normalize_history_timestamp_ms(row)
        if normalized_ms < after_ms:
            continue
        if row.get("device_id") != device_id or row.get("source") != source:
            continue
        observed_identity = row.get(identity_field)
        if kind == "meta":
            if not isinstance(observed_identity, str) or (
                observed_identity.upper() != identity_value.upper()
            ):
                continue
            if row.get("manufacturer") != "Meta Glasses":
                continue
            if "fd5f" not in _service_uuid_set(row.get("ble_svc_uuids")):
                continue
        elif observed_identity != identity_value:
            continue
        cleaned = redact_secrets(dict(row))
        cleaned["normalized_timestamp_ms"] = normalized_ms
        return cleaned
    return None


def _validate_continuity(
    value: Any,
    *,
    device_id: str,
    expected: Mapping[str, Any],
) -> dict[str, Any]:
    receipt = _required_mapping(value, "calibration continuity")
    if set(receipt) != CONTINUITY_KEYS or set(expected) != CONTINUITY_KEYS:
        raise EvidenceError("calibration continuity fields changed or are missing")
    if receipt.get("schema") != 1 or receipt.get("device_id") != device_id:
        raise EvidenceError("calibration continuity device/schema changed")
    if receipt.get("calibration_status") not in ("defaults", "trusted", "untrusted"):
        raise EvidenceError("calibration continuity status is invalid")
    if receipt.get("listener_model_schema") != "rssi-ref-path-loss-v1":
        raise EvidenceError("calibration continuity model schema changed")
    digest = receipt.get("listener_model_sha256")
    if digest is not None and (
        not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest)
    ):
        raise EvidenceError("calibration continuity digest is invalid")
    if dict(receipt) != dict(expected):
        raise EvidenceError("calibration continuity differs from capture-installed receipt")
    cleaned = redact_secrets(dict(receipt))
    if contains_secret_key(cleaned):
        raise EvidenceError("calibration continuity contains secret material")
    return cleaned


def _state_path_for(output: Path, explicit: Path | None) -> Path:
    if explicit is not None:
        return explicit.expanduser().resolve()
    from_environment = os.environ.get("BACKEND_CANARY_STATE")
    if from_environment:
        return Path(from_environment).expanduser().resolve()
    parent = output.expanduser().resolve().parent
    if parent.name == "evidence":
        return parent.parent / "canary-state.json"
    return parent / "canary-state.json"


def _load_installed_receipt(
    path: Path,
    *,
    device_id: str,
) -> tuple[dict[str, Any], dict[str, Mapping[str, Any]]]:
    path = _require_canary_storage(path, "canary state")
    if not path.is_file():
        raise EvidenceError("capture-installed canary state is required")
    _private_mode(path, 0o600, "canary state")
    raw = path.read_bytes()
    state = _required_mapping(_decode_json(raw, "canary state"), "canary state")
    if state.get("captured_device_id") != device_id:
        raise EvidenceError("canary state device ID differs from requested device")
    capture = _required_mapping(state.get("installed_capture"), "installed capture")
    expected = _required_mapping(
        capture.get("continuity"), "capture-installed continuity",
    )
    boards = _required_mapping(state.get("boards"), "canary boards")
    final: dict[str, Mapping[str, Any]] = {}
    for role in ("scanner0", "scanner1", "uplink"):
        board = _required_mapping(boards.get(role), f"{role} state")
        health = _required_mapping(
            board.get("final_health"), f"{role} final-health evidence",
        )
        final[role] = health
    return dict(expected), final


def _identity_tuple(value: Mapping[str, Any], *, scanner: bool) -> tuple[Any, ...]:
    return (
        value.get("firmware_target"),
        value.get("app_project"),
        value.get("hardware_type"),
        value.get("firmware_version"),
    )


def _validate_final_health_binding(
    role: str,
    heartbeat: Mapping[str, Any],
    final_health: Mapping[str, Any],
) -> None:
    if final_health.get("target") != SCANNER_IDENTITY[0]:
        raise EvidenceError(f"{role} final_health target is missing or changed")
    if _required_mac(final_health.get("mac"), f"{role} final_health MAC") != heartbeat["mac"]:
        raise EvidenceError(f"{role} final_health MAC changed")
    if final_health.get("boot_id") != heartbeat["boot_id"]:
        raise EvidenceError(f"{role} final_health boot ID is stale")
    if final_health.get("command_ingress_boot_id") != heartbeat["boot_id"]:
        raise EvidenceError(f"{role} final_health command ingress is stale")
    if final_health.get("role") != heartbeat["profile"]:
        raise EvidenceError(f"{role} final_health role/profile binding changed")
    if (
        final_health.get("radio_healthy") is not True
        or final_health.get("rollback_clear") is not True
        or final_health.get("nvs_erased") is not False
    ):
        raise EvidenceError(f"{role} final_health is incomplete")


def _validate_node(
    payload: Any,
    *,
    device_id: str,
    final_health: Mapping[str, Mapping[str, Any]],
    require_healthy: bool,
    continuity: Mapping[str, Any],
) -> dict[str, Any]:
    document = _required_mapping(payload, "node status")
    nodes = document.get("nodes")
    if not isinstance(nodes, list) or document.get("count") != len(nodes):
        raise EvidenceError("node status count/list is malformed")
    matches = [
        node for node in nodes
        if isinstance(node, Mapping) and node.get("device_id") == device_id
    ]
    if len(matches) != 1:
        raise EvidenceError("node status must contain exactly one requested device")
    node = matches[0]
    if _identity_tuple(node, scanner=False) != UPLINK_IDENTITY:
        raise EvidenceError("uplink backend identity is not exact")
    hardware_mac = _required_mac(node.get("hardware_mac"), "uplink hardware MAC")
    if node.get("online") is not True:
        raise EvidenceError("uplink is not online")
    heartbeat_at_ms = _epoch_seconds_to_ms(node.get("last_seen"), "node heartbeat")
    uplink_final = final_health["uplink"]
    if (
        uplink_final.get("target") != UPLINK_IDENTITY[0]
        or _required_mac(uplink_final.get("mac"), "uplink final_health MAC") != hardware_mac
        or uplink_final.get("device_id") != device_id
        or uplink_final.get("rollback_clear") is not True
    ):
        raise EvidenceError("uplink final_health binding is missing or changed")
    uplink_boot_id = _required_int(
        uplink_final.get("boot_id"), "uplink final_health boot_id", minimum=1,
    )

    raw_scanners = node.get("scanners")
    if not isinstance(raw_scanners, list) or len(raw_scanners) != 2:
        raise EvidenceError("exactly two scanner final-health records are required")
    by_slot: dict[int, Mapping[str, Any]] = {}
    for value in raw_scanners:
        scanner = _required_mapping(value, "scanner heartbeat")
        slot = _required_int(scanner.get("slot"), "scanner slot")
        if slot not in (0, 1) or slot in by_slot:
            raise EvidenceError("scanner slot is missing or duplicated")
        by_slot[slot] = scanner
    normalized_scanners: list[dict[str, Any]] = []
    macs: set[str] = set()
    for slot, expected_profile in enumerate(SCANNER_PROFILES):
        scanner = by_slot[slot]
        if _identity_tuple(scanner, scanner=True) != SCANNER_IDENTITY:
            raise EvidenceError(f"scanner{slot} backend identity is not exact")
        # Only the explicit profile key has protocol meaning. A display role
        # string is intentionally ignored here.
        profile = scanner.get("profile")
        if profile != expected_profile:
            raise EvidenceError(f"scanner{slot} profile is missing or changed")
        generation = _required_int(
            scanner.get("role_generation"),
            f"scanner{slot} role_generation",
            minimum=1,
        )
        if scanner.get("role_acked") is not True:
            raise EvidenceError(f"scanner{slot} role_acked is not true")
        radio_healthy = scanner.get("radio_healthy")
        if not isinstance(radio_healthy, bool):
            raise EvidenceError(f"scanner{slot} radio_healthy is missing")
        if require_healthy and radio_healthy is not True:
            raise EvidenceError(f"scanner{slot} radio_healthy is not true")
        boot_id = _required_int(
            scanner.get("boot_id"), f"scanner{slot} boot_id", minimum=1,
        )
        _required_int(
            scanner.get("status_sequence"),
            f"scanner{slot} status_sequence",
            minimum=1,
        )
        mac = _required_mac(scanner.get("mac"), f"scanner{slot} MAC")
        if mac in macs or mac == hardware_mac:
            raise EvidenceError("scanner/uplink MACs must be unique")
        macs.add(mac)
        if scanner.get("rollback_state") != "valid":
            raise EvidenceError(f"scanner{slot} rollback state is not valid")
        if require_healthy and scanner.get("ota_state") != "idle":
            raise EvidenceError(f"scanner{slot} OTA state is not idle")
        normalized = redact_secrets(dict(scanner))
        normalized.update(
            mac=mac,
            boot_id=boot_id,
            profile=profile,
            role_generation=generation,
            role_acked=True,
            radio_healthy=radio_healthy,
        )
        _validate_final_health_binding(
            f"scanner{slot}", normalized, final_health[f"scanner{slot}"],
        )
        normalized["final_health"] = True
        normalized_scanners.append(normalized)

    queue = _required_mapping(node.get("upload_queue"), "upload queue")
    normalized_queue = {
        "depth_batches": _required_int(queue.get("depth_batches"), "queue depth"),
        "capacity_batches": _required_int(
            queue.get("capacity_batches"), "queue capacity", minimum=1,
        ),
        "overflow_dropped_batches": _required_int(
            queue.get("overflow_dropped_batches"), "queue drop counter",
        ),
        "quarantined_batches": _required_int(
            queue.get("quarantined_batches"), "queue quarantine counter",
        ),
    }
    health = _required_mapping(node.get("health"), "uplink health")
    uptime_ms = _required_int(health.get("uptime_ms"), "uplink uptime")
    if health.get("clock_valid") is not True:
        raise EvidenceError("uplink clock is not valid")
    _required_int(health.get("epoch_ms"), "uplink epoch_ms", minimum=MIN_EPOCH_MS)
    fifo_sequence = _required_int(
        node.get("total_batches"), "backend FIFO sequence", minimum=1,
    )
    led_state = node.get("led_state")
    if led_state not in LED_STATES:
        raise EvidenceError("uplink LED state is missing or invalid")
    scanner_profiles = [scanner["profile"] for scanner in normalized_scanners]
    if scanner_profiles != list(SCANNER_PROFILES) or len(set(scanner_profiles)) != 2:
        raise EvidenceError("scanner profiles must be exactly BLE then Wi-Fi")
    return {
        "identity": {
            "firmware_target": UPLINK_IDENTITY[0],
            "app_project": UPLINK_IDENTITY[1],
            "hardware_type": UPLINK_IDENTITY[2],
            "firmware_version": UPLINK_IDENTITY[3],
            "hardware_mac": hardware_mac,
        },
        "uplink_boot_id": uplink_boot_id,
        "uplink_final_health": redact_secrets(dict(uplink_final)),
        "heartbeat_at_ms": heartbeat_at_ms,
        "scanner_profiles": scanner_profiles,
        "scanners": normalized_scanners,
        "led_state": led_state,
        "upload_queue": normalized_queue,
        "upload": redact_secrets(dict(_required_mapping(node.get("upload"), "upload telemetry"))),
        "health": redact_secrets(dict(health)),
        "fifo_sequence": fifo_sequence,
        "continuity": dict(continuity),
        "node": redact_secrets(dict(node)),
    }


class CanaryEvidenceRecorder:
    """Capture canonical snapshots bound to capture-installed state."""

    def __init__(
        self,
        *,
        backend_base: str,
        device_id: str,
        output: Path,
        fetch_json: FetchJson = _http_fetch_json,
        now: Callable[[], float] = time.time,
        state_path: Path | None = None,
    ):
        if not isinstance(device_id, str) or not device_id:
            raise EvidenceError("device ID is required")
        self.backend_base = _normalize_backend_base(backend_base)
        self.device_id = device_id
        self.output = output.expanduser().resolve()
        self.fetch_json = fetch_json
        self.now = now
        state = _state_path_for(self.output, state_path)
        self.expected_continuity, self.final_health = _load_installed_receipt(
            state, device_id=device_id,
        )

    def _get(self, suffix: str) -> FetchResponse:
        return _fetch(self.fetch_json, self.backend_base + suffix)

    def _capture_backend(
        self,
        *,
        include_history: bool,
        require_healthy: bool,
    ) -> tuple[dict[str, Any], dict[str, str], dict[str, int | None]]:
        status_response = self._get("/detections/nodes/status")
        continuity_response = self._get(
            "/detections/calibrate/continuity/"
            + urllib.parse.quote(self.device_id, safe="")
        )
        continuity = _validate_continuity(
            continuity_response.payload,
            device_id=self.device_id,
            expected=self.expected_continuity,
        )
        backend = _validate_node(
            status_response.payload,
            device_id=self.device_id,
            final_health=self.final_health,
            require_healthy=require_healthy,
            continuity=continuity,
        )
        responses = {
            "nodes_status": _sha256(status_response.raw),
            "calibration_continuity": _sha256(continuity_response.raw),
        }
        server_times = {
            "nodes_status": status_response.server_timestamp_ms,
            "calibration_continuity": continuity_response.server_timestamp_ms,
        }
        if include_history:
            history_response = self._get(
                "/detections/drones/history?hours=1&limit=2000"
            )
            history = _required_mapping(history_response.payload, "history response")
            rows = history.get("detections")
            if not isinstance(rows, list):
                raise EvidenceError("history response has no detections list")
            # Validate all supplied primary timestamps before preserving rows.
            for row in rows:
                normalize_history_timestamp_ms(
                    _required_mapping(row, "history row")
                )
            backend["detections"] = redact_secrets(rows)
            responses["detection_history"] = _sha256(history_response.raw)
            server_times["detection_history"] = history_response.server_timestamp_ms
        return backend, responses, server_times

    def snapshot(self, phase: str) -> dict[str, Any]:
        if (
            not isinstance(phase, str)
            or phase != phase.strip()
            or phase not in HEALTHY_SNAPSHOT_PHASES | set(DEGRADED_SNAPSHOT_PHASES)
        ):
            raise EvidenceError("snapshot phase is not an exact bounded phase")
        degraded = DEGRADED_SNAPSHOT_PHASES.get(phase)
        backend, response_hashes, server_times = self._capture_backend(
            include_history=True,
            require_healthy=degraded is None,
        )
        health = tuple(
            scanner["radio_healthy"] for scanner in backend["scanners"]
        )
        if degraded is not None:
            expected_health, expected_led = degraded
            if health != expected_health or backend["led_state"] != expected_led:
                raise EvidenceError(
                    "degraded snapshot topology/LED state is not exact"
                )
        if phase in ZERO_QUEUE_PHASES and (
            backend["upload_queue"]["depth_batches"] != 0
        ):
            raise EvidenceError(f"{phase} requires an empty upload queue")
        if phase in HEALTHY_LED_PHASES and backend["led_state"] != "healthy":
            raise EvidenceError(f"{phase} requires exact healthy LED state")
        expected_threat_led = THREAT_SNAPSHOT_LEDS.get(phase)
        if (
            expected_threat_led is not None
            and backend["led_state"] != expected_threat_led
        ):
            raise EvidenceError(
                f"{phase} requires exact {expected_threat_led} LED state"
            )
        record = redact_secrets({
            "schema": 1,
            "record_type": "snapshot",
            "phase": phase,
            "device_id": self.device_id,
            "observed_at_ms": int(self.now() * 1000),
            "heartbeat_at_ms": backend["heartbeat_at_ms"],
            "server_timestamps_ms": server_times,
            "response_sha256": response_hashes,
            "backend": backend,
        })
        if contains_secret_key(record):
            raise EvidenceError("snapshot contains secret or raw BLE material")
        _secure_append_json(self.output, record)
        return record


def validate_command_history(
    history: Mapping[str, Any],
    *,
    device_id: str,
    command_id: str,
    terminal_state: str,
) -> dict[str, Any]:
    document = _required_mapping(history, "command history")
    if terminal_state not in TERMINAL_STATES:
        raise EvidenceError("command terminal state is invalid")
    if document.get("device_id") != device_id:
        raise EvidenceError("command history device ID changed")
    if document.get("command_id") != command_id:
        raise EvidenceError("command history command ID changed")
    if not COMMAND_ID_RE.fullmatch(command_id):
        raise EvidenceError("command ID is not canonical")
    if document.get("command_type") != "ble_investigate":
        raise EvidenceError("command history type changed")
    events = document.get("events")
    if not isinstance(events, list) or not events:
        raise EvidenceError("command history is missing begin event")
    first = _required_mapping(events[0], "command event")
    if first.get("type") != "ble_inv_begin":
        raise EvidenceError("command history is missing begin event")
    for expected_sequence, raw_event in enumerate(events):
        event = _required_mapping(raw_event, "command event")
        if event.get("sequence") != expected_sequence:
            raise EvidenceError("command history sequence is skipped, duplicated, or reordered")
        if event.get("request_id") != command_id:
            raise EvidenceError("command event request ID changed")
    last = _required_mapping(events[-1], "command terminal event")
    if (
        document.get("terminal") is not True
        or document.get("state") != "terminal"
        or last.get("type") != "ble_inv_end"
    ):
        raise EvidenceError("command history is not terminal")
    if (
        document.get("result_state") != terminal_state
        or last.get("state") != terminal_state
    ):
        raise EvidenceError("command terminal state is unexpected")
    if document.get("next_sequence") != len(events):
        raise EvidenceError("command next sequence is inconsistent")
    cleaned = redact_secrets(dict(document))
    if contains_secret_key(cleaned):
        raise EvidenceError("command evidence contains raw or secret material")
    return cleaned


def wait_for_detection(
    *,
    backend_base: str,
    device_id: str,
    kind: str,
    source: str,
    identity_field: str,
    identity_value: str,
    after_ms: int,
    ready_file: Path,
    timeout_s: int,
    output: Path,
    manufacturer: str | None = None,
    service_uuid_token: str | None = None,
    fetch_json: FetchJson = _http_fetch_json,
    now: Callable[[], float] = time.time,
    monotonic: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> dict[str, Any]:
    base = _normalize_backend_base(backend_base)
    if timeout_s <= 0:
        raise EvidenceError("detection wait timeout must be positive")
    history_url = base + "/detections/drones/history?hours=1&limit=2000"
    deadline = monotonic() + timeout_s
    initial = _fetch(fetch_json, history_url)
    # Validate the first response, but never accept it: only a ready file that
    # was fsynced after this successful poll authorizes RF source enablement.
    initial_match = find_matching_detection(
        _required_mapping(initial.payload, "history response"),
        device_id=device_id,
        kind=kind,
        source=source,
        identity_field=identity_field,
        identity_value=identity_value,
        manufacturer=manufacturer,
        service_uuid_token=service_uuid_token,
        after_ms=after_ms,
    )
    if initial_match is not None:
        raise EvidenceError("matching source was already present before ready")
    ready_cutoff_ms = _create_ready_file(ready_file, {
        "schema": 1,
        "device_id": device_id,
        "kind": kind,
        "initial_response_sha256": _sha256(initial.raw),
    }, requested_after_ms=after_ms, now=now)
    while monotonic() <= deadline:
        response = _fetch(fetch_json, history_url)
        match = find_matching_detection(
            _required_mapping(response.payload, "history response"),
            device_id=device_id,
            kind=kind,
            source=source,
            identity_field=identity_field,
            identity_value=identity_value,
            manufacturer=manufacturer,
            service_uuid_token=service_uuid_token,
            after_ms=ready_cutoff_ms,
        )
        if match is not None:
            record = redact_secrets({
                "schema": 1,
                "record_type": "detection",
                "phase": kind,
                "device_id": device_id,
                "observed_at_ms": int(now() * 1000),
                "after_ms": ready_cutoff_ms,
                "requested_after_ms": after_ms,
                "response_sha256": _sha256(response.raw),
                "detection": match,
            })
            _secure_append_json(output, record)
            return record
        sleep(POLL_INTERVAL_SECONDS)
    raise EvidenceError("detection wait timed out without an exact RF match")


def _history_is_valid_pending(
    history: Mapping[str, Any], *, device_id: str, command_id: str,
) -> bool:
    if history.get("device_id") != device_id or history.get("command_id") != command_id:
        raise EvidenceError("command history identity changed")
    if history.get("command_type") != "ble_investigate":
        raise EvidenceError("command history type changed")
    events = history.get("events")
    if not isinstance(events, list):
        raise EvidenceError("command events are malformed")
    for sequence, raw in enumerate(events):
        event = _required_mapping(raw, "command event")
        if event.get("sequence") != sequence or event.get("request_id") != command_id:
            raise EvidenceError("command event sequence/identity changed")
    if events and _required_mapping(events[0], "command event").get("type") != "ble_inv_begin":
        raise EvidenceError("command history is missing begin event")
    return history.get("terminal") is not True


def wait_for_command(
    *,
    backend_base: str,
    device_id: str,
    command_id: str,
    terminal_state: str,
    timeout_s: int,
    output: Path,
    fetch_json: FetchJson = _http_fetch_json,
    now: Callable[[], float] = time.time,
    monotonic: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> dict[str, Any]:
    base = _normalize_backend_base(backend_base)
    if not COMMAND_ID_RE.fullmatch(command_id):
        raise EvidenceError("command ID is not canonical")
    deadline = monotonic() + timeout_s
    url = (
        base + "/nodes/" + urllib.parse.quote(device_id, safe="")
        + "/commands/" + urllib.parse.quote(command_id, safe="")
    )
    while monotonic() <= deadline:
        response = _fetch(fetch_json, url)
        history = _required_mapping(response.payload, "command history")
        if _history_is_valid_pending(
            history, device_id=device_id, command_id=command_id,
        ):
            sleep(POLL_INTERVAL_SECONDS)
            continue
        validated = validate_command_history(
            history,
            device_id=device_id,
            command_id=command_id,
            terminal_state=terminal_state,
        )
        record = {
            "schema": 1,
            "record_type": "command",
            "phase": f"command-{terminal_state}",
            "device_id": device_id,
            "observed_at_ms": int(now() * 1000),
            "response_sha256": _sha256(response.raw),
            "command": validated,
        }
        _secure_append_json(output, record)
        return record
    raise EvidenceError("command wait timed out before an exact terminal state")


def wait_for_led(
    *,
    backend_base: str,
    device_id: str,
    expected: str,
    after_ms: int,
    timeout_s: int,
    output: Path,
    fetch_json: FetchJson = _http_fetch_json,
    now: Callable[[], float] = time.time,
    monotonic: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> dict[str, Any]:
    if expected not in LED_STATES:
        raise EvidenceError("expected LED state is invalid")
    recorder = CanaryEvidenceRecorder(
        backend_base=backend_base,
        device_id=device_id,
        output=output,
        fetch_json=fetch_json,
        now=now,
    )
    deadline = monotonic() + timeout_s
    while monotonic() <= deadline:
        backend, hashes, server_times = recorder._capture_backend(
            include_history=False,
            require_healthy=expected == "healthy",
        )
        observed_time = _required_int(
            backend["health"].get("epoch_ms"), "LED evidence epoch", minimum=MIN_EPOCH_MS,
        )
        if observed_time >= after_ms and backend["led_state"] == expected:
            record = {
                "schema": 1,
                "record_type": "led",
                "phase": f"led-{expected}",
                "device_id": device_id,
                "observed_at_ms": int(now() * 1000),
                "after_ms": after_ms,
                "server_timestamps_ms": server_times,
                "response_sha256": hashes,
                "backend": backend,
            }
            _secure_append_json(output, record)
            return record
        sleep(POLL_INTERVAL_SECONDS)
    raise EvidenceError("LED wait timed out before the exact state")


def _serial_log_receipt(
    directory: Path,
    *,
    observed_at_ms: int,
) -> list[dict[str, Any]]:
    root = directory.expanduser().resolve()
    if not root.is_dir() or root.is_symlink():
        raise EvidenceError("serial log directory is unavailable")
    _private_mode(root, 0o700, "serial log directory")
    expected_names = {f"{role}.log" for role in SERIAL_LOG_ROLES}
    observed_names = {path.name for path in root.iterdir()}
    if observed_names != expected_names:
        raise EvidenceError(
            "serial log directory must contain exactly scanner0.log, "
            "scanner1.log, and uplink.log"
        )
    receipts: list[dict[str, Any]] = []
    forbidden_content = re.compile(
        rb"password|wifi[_-]?pass|ap[_-]?pass|secret|credential|token|"
        rb"authorization|cookie|set[-_]?cookie|api[_-]?key|value[_-]?hex|ble[_-]?raw|"
        rb"raw[_-]?ble|characteristic[_-]?value|ble[_-]?apple[_-]?auth|"
        rb"ble[_-]?auth[_-]?payload|raw[_-]?auth[_-]?payload|"
        rb"auth(?:entication)?[_-]?value|watchdog|guru meditation|panic|"
        rb"rollback_failed",
        re.IGNORECASE,
    )
    for role in SERIAL_LOG_ROLES:
        path = root / f"{role}.log"
        details = path.lstat()
        if path.is_symlink() or not stat.S_ISREG(details.st_mode):
            raise EvidenceError(f"serial log {path.name} is not a regular file")
        if stat.S_IMODE(details.st_mode) != 0o600:
            raise EvidenceError(f"serial log {path.name} must have mode 0600")
        raw = path.read_bytes()
        mtime_ms = details.st_mtime_ns // 1_000_000
        if not raw:
            raise EvidenceError(f"serial log {path.name} is empty")
        if (
            mtime_ms > observed_at_ms + 5_000
            or observed_at_ms - mtime_ms > SERIAL_LOG_MAX_AGE_MS
        ):
            raise EvidenceError(f"serial log {path.name} is not current")
        if forbidden_content.search(raw):
            raise EvidenceError(f"serial log gate failed for {path.name}")
        receipts.append({
            "role": role,
            "name": path.name,
            "size": len(raw),
            "sha256": _sha256(raw),
            "mtime_ms": mtime_ms,
        })
    return receipts


def _validate_serial_log_receipts(value: Any, *, observed_at_ms: int) -> None:
    if not isinstance(value, list) or len(value) != len(SERIAL_LOG_ROLES):
        raise EvidenceError("soak serial evidence requires exactly three role logs")
    for raw, role in zip(value, SERIAL_LOG_ROLES):
        receipt = _required_mapping(raw, "soak serial receipt")
        if set(receipt) != {"role", "name", "size", "sha256", "mtime_ms"}:
            raise EvidenceError("soak serial receipt fields are not exact")
        if receipt.get("role") != role or receipt.get("name") != f"{role}.log":
            raise EvidenceError("soak serial receipt role binding changed")
        _required_int(receipt.get("size"), "soak serial size", minimum=1)
        digest = receipt.get("sha256")
        if not isinstance(digest, str) or re.fullmatch(r"[0-9a-f]{64}", digest) is None:
            raise EvidenceError("soak serial receipt SHA-256 is invalid")
        mtime_ms = _required_int(
            receipt.get("mtime_ms"), "soak serial mtime", minimum=MIN_EPOCH_MS,
        )
        if (
            mtime_ms > observed_at_ms + 5_000
            or observed_at_ms - mtime_ms > SERIAL_LOG_MAX_AGE_MS
        ):
            raise EvidenceError("soak serial receipt is not current")


def monitor_soak(
    *,
    backend_base: str,
    device_id: str,
    duration_s: int,
    interval_s: int,
    serial_log_dir: Path,
    output: Path,
    fetch_json: FetchJson = _http_fetch_json,
    now: Callable[[], float] = time.time,
    monotonic: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> None:
    if duration_s <= 0 or interval_s <= 0 or interval_s > 60:
        raise EvidenceError("monitor duration/interval is invalid")
    recorder = CanaryEvidenceRecorder(
        backend_base=backend_base,
        device_id=device_id,
        output=output,
        fetch_json=fetch_json,
        now=now,
    )
    started = monotonic()
    deadline = started + duration_s
    baseline: dict[str, Any] | None = None
    previous_heartbeat: int | None = None
    max_gap_s = 0
    while True:
        backend, hashes, server_times = recorder._capture_backend(
            include_history=True,
            require_healthy=False,
        )
        observed_at_ms = int(now() * 1000)
        heartbeat_at_ms = backend["heartbeat_at_ms"]
        if previous_heartbeat is not None and heartbeat_at_ms > previous_heartbeat:
            max_gap_s = max(
                max_gap_s,
                (heartbeat_at_ms - previous_heartbeat + 999) // 1000,
            )
        previous_heartbeat = max(previous_heartbeat or 0, heartbeat_at_ms)
        backend["max_heartbeat_gap_s"] = max_gap_s
        if baseline is None:
            baseline = {
                "identity": backend["identity"],
                "uplink_boot_id": backend["uplink_boot_id"],
                "scanner_bindings": [
                    (item["slot"], item["mac"], item["boot_id"], item["profile"])
                    for item in backend["scanners"]
                ],
                "uptime_ms": backend["health"]["uptime_ms"],
                "drops": backend["upload_queue"]["overflow_dropped_batches"],
                "quarantines": backend["upload_queue"]["quarantined_batches"],
            }
        else:
            bindings = [
                (item["slot"], item["mac"], item["boot_id"], item["profile"])
                for item in backend["scanners"]
            ]
            if backend["identity"] != baseline["identity"] or (
                backend["uplink_boot_id"] != baseline["uplink_boot_id"]
            ) or bindings != baseline["scanner_bindings"]:
                raise EvidenceError("monitor detected firmware identity or boot drift")
            if backend["health"]["uptime_ms"] < baseline["uptime_ms"]:
                raise EvidenceError("monitor detected an unexpected uplink reset")
            if (
                backend["upload_queue"]["overflow_dropped_batches"]
                != baseline["drops"]
                or backend["upload_queue"]["quarantined_batches"]
                != baseline["quarantines"]
            ):
                raise EvidenceError("monitor detected queue drop/quarantine growth")
            baseline["uptime_ms"] = backend["health"]["uptime_ms"]
        if not any(item["radio_healthy"] for item in backend["scanners"]):
            raise EvidenceError("monitor detected both scanners unusable")
        if any(item["rollback_state"] != "valid" for item in backend["scanners"]):
            raise EvidenceError("monitor detected rollback state")
        record = redact_secrets({
            "schema": 1,
            "record_type": "monitor",
            "phase": "soak",
            "device_id": device_id,
            "observed_at_ms": observed_at_ms,
            "heartbeat_at_ms": heartbeat_at_ms,
            "server_timestamps_ms": server_times,
            "response_sha256": hashes,
            "serial_logs": _serial_log_receipt(
                serial_log_dir, observed_at_ms=observed_at_ms,
            ),
            "backend": backend,
        })
        if contains_secret_key(record):
            raise EvidenceError("monitor evidence contains secret material")
        _secure_append_json(output, record)
        current = monotonic()
        if current >= deadline:
            return
        sleep(min(float(interval_s), max(0.0, deadline - current)))


def _validate_soak_backend(value: Any) -> dict[str, Any]:
    backend = dict(_required_mapping(value, "soak backend evidence"))
    identity = _required_mapping(backend.get("identity"), "soak uplink identity")
    if (
        identity.get("firmware_target"),
        identity.get("app_project"),
        identity.get("hardware_type"),
        identity.get("firmware_version"),
    ) != UPLINK_IDENTITY:
        raise EvidenceError("soak contains non-backend uplink identity")
    uplink_mac = _required_mac(identity.get("hardware_mac"), "soak uplink MAC")
    _required_int(backend.get("uplink_boot_id"), "soak uplink boot_id", minimum=1)
    profiles = backend.get("scanner_profiles")
    if profiles != list(SCANNER_PROFILES) or len(set(profiles or [])) != 2:
        raise EvidenceError("soak scanner profiles are missing or duplicated")
    scanners = backend.get("scanners")
    if not isinstance(scanners, list) or len(scanners) != 2:
        raise EvidenceError("soak is missing scanner final-health evidence")
    scanner_macs: set[str] = set()
    for slot, (scanner, profile) in enumerate(zip(scanners, SCANNER_PROFILES)):
        item = _required_mapping(scanner, "soak scanner")
        if _identity_tuple(item, scanner=True) != SCANNER_IDENTITY:
            raise EvidenceError("soak contains non-backend scanner identity")
        if item.get("slot") != slot or item.get("profile") != profile:
            raise EvidenceError("soak scanner slot/profile binding changed")
        scanner_mac = _required_mac(item.get("mac"), "soak scanner MAC")
        if scanner_mac == uplink_mac or scanner_mac in scanner_macs:
            raise EvidenceError("soak physical MAC bindings are duplicated")
        scanner_macs.add(scanner_mac)
        _required_int(item.get("boot_id"), "soak scanner boot_id", minimum=1)
        _required_int(
            item.get("role_generation"), "soak scanner role_generation", minimum=1,
        )
        if item.get("role_acked") is not True:
            raise EvidenceError("soak scanner role_acked changed")
        if item.get("radio_healthy") is not True:
            raise EvidenceError("soak scanner radio_healthy changed")
        if item.get("rollback_state") != "valid":
            raise EvidenceError("soak scanner rollback state changed")
        if item.get("ota_state") != "idle":
            raise EvidenceError("soak scanner OTA state is not idle")
        if item.get("final_health") is not True:
            raise EvidenceError("soak scanner final_health evidence is missing")
    if backend.get("led_state") != "healthy":
        raise EvidenceError("soak LED state is not healthy")
    queue = _required_mapping(backend.get("upload_queue"), "soak upload queue")
    for field in (
        "depth_batches", "overflow_dropped_batches", "quarantined_batches",
    ):
        _required_int(queue.get(field), f"soak queue {field}")
    _required_int(backend.get("fifo_sequence"), "soak FIFO sequence", minimum=1)
    health = _required_mapping(backend.get("health"), "soak health")
    _required_int(health.get("uptime_ms"), "soak uptime")
    continuity = _required_mapping(backend.get("continuity"), "soak continuity")
    if set(continuity) != CONTINUITY_KEYS:
        raise EvidenceError("soak calibration continuity is incomplete")
    return backend


def verify_soak(
    path: Path,
    *,
    expected_duration_s: int,
    max_heartbeat_gap_s: int,
) -> SoakResult:
    """Verify a complete JSONL soak without trusting recorder-derived claims."""
    if expected_duration_s <= 0 or max_heartbeat_gap_s <= 0:
        raise EvidenceError("soak verification limits must be positive")
    source = path.expanduser().resolve()
    if not source.is_file():
        raise EvidenceError("soak evidence file is unavailable")
    records: list[dict[str, Any]] = []
    for line_number, raw_line in enumerate(source.read_bytes().splitlines(), 1):
        if not raw_line:
            raise EvidenceError(f"empty JSONL record at line {line_number}")
        value = _decode_json(raw_line, f"soak line {line_number}")
        record = dict(_required_mapping(value, f"soak line {line_number}"))
        if contains_secret_key(record):
            raise EvidenceError("soak evidence contains a secret or raw BLE key")
        records.append(record)
    monitor_indices = [
        index
        for index, row in enumerate(records)
        if row.get("record_type") == "monitor"
    ]
    monitor = [records[index] for index in monitor_indices]
    if len(monitor) < 2:
        raise EvidenceError("soak requires at least two monitor samples")
    recovery_indices = [
        index
        for index, row in enumerate(records)
        if row.get("phase") == "network-recovery"
    ]
    if not recovery_indices:
        raise EvidenceError("network-recovery evidence is required before soak")
    if any(index >= monitor_indices[0] for index in recovery_indices):
        raise EvidenceError("network-recovery evidence must precede every monitor")
    recovery = records[recovery_indices[-1]]
    if recovery.get("record_type") != "snapshot":
        raise EvidenceError("network-recovery evidence is not a snapshot")
    monitor_device_id = monitor[0].get("device_id")
    if (
        not isinstance(monitor_device_id, str)
        or not monitor_device_id
        or recovery.get("device_id") != monitor_device_id
    ):
        raise EvidenceError("network-recovery device ID does not match soak")
    recovered_at = _required_int(
        recovery.get("observed_at_ms"),
        "network-recovery observed time",
        minimum=MIN_EPOCH_MS,
    )
    recovered_backend = _validate_soak_backend(recovery.get("backend"))
    if recovered_backend["upload_queue"]["depth_batches"] != 0:
        raise EvidenceError("network recovery left an unfinished queue")

    first_ms = _required_int(monitor[0].get("observed_at_ms"), "soak start time", minimum=MIN_EPOCH_MS)
    last_ms = _required_int(monitor[-1].get("observed_at_ms"), "soak end time", minimum=MIN_EPOCH_MS)
    if last_ms < first_ms:
        raise EvidenceError("soak timestamps are out of order")
    if recovered_at > first_ms:
        raise EvidenceError("network recovery timestamp is after soak start")
    duration_ms = last_ms - first_ms
    if duration_ms < expected_duration_s * 1000:
        raise EvidenceError("soak duration is shorter than required")

    previous_observed = first_ms
    previous_heartbeat: int | None = None
    previous_fifo = recovered_backend["fifo_sequence"]
    previous_uptime = recovered_backend["health"]["uptime_ms"]
    observed_max_gap = 0
    recovery_queue = recovered_backend["upload_queue"]
    baseline_drops = recovery_queue["overflow_dropped_batches"]
    baseline_quarantines = recovery_queue["quarantined_batches"]
    baseline_continuity: dict[str, Any] | None = dict(
        recovered_backend["continuity"]
    )
    baseline_identity: tuple[Any, ...] | None = tuple(
        sorted(recovered_backend["identity"].items())
    )
    baseline_boots: tuple[int, ...] | None = (
        recovered_backend["uplink_boot_id"],
        *(item["boot_id"] for item in recovered_backend["scanners"]),
    )
    baseline_scanner_bindings: tuple[tuple[Any, ...], ...] | None = tuple(
        (
            item["slot"], item["mac"], item["boot_id"],
            item["profile"], item["role_generation"],
        )
        for item in recovered_backend["scanners"]
    )
    for row in monitor:
        if row.get("device_id") != monitor_device_id:
            raise EvidenceError("soak device ID changed")
        observed = _required_int(row.get("observed_at_ms"), "soak observed time", minimum=MIN_EPOCH_MS)
        heartbeat = _required_int(row.get("heartbeat_at_ms"), "soak heartbeat time", minimum=MIN_EPOCH_MS)
        _validate_serial_log_receipts(
            row.get("serial_logs"), observed_at_ms=observed,
        )
        if observed < previous_observed or heartbeat > observed:
            raise EvidenceError("soak heartbeat/timestamp ordering is invalid")
        previous_observed = observed
        backend = _validate_soak_backend(row.get("backend"))
        reported_gap = _required_int(
            backend.get("max_heartbeat_gap_s", 0), "maximum heartbeat gap",
        )
        if previous_heartbeat is not None:
            if heartbeat < previous_heartbeat:
                raise EvidenceError("soak heartbeat timestamps are out of order")
            if heartbeat > previous_heartbeat:
                actual_gap = (heartbeat - previous_heartbeat + 999) // 1000
                observed_max_gap = max(observed_max_gap, actual_gap)
        previous_heartbeat = heartbeat
        observed_max_gap = max(observed_max_gap, reported_gap)
        if observed_max_gap > max_heartbeat_gap_s:
            raise EvidenceError("soak heartbeat gap exceeds the limit")
        if observed - heartbeat > max_heartbeat_gap_s * 1000:
            raise EvidenceError("soak heartbeat became stale")
        fifo = backend["fifo_sequence"]
        if fifo < previous_fifo:
            raise EvidenceError("soak FIFO sequence is out of order")
        previous_fifo = fifo
        uptime = backend["health"]["uptime_ms"]
        if uptime < previous_uptime:
            raise EvidenceError("soak contains an unexpected uplink reset")
        previous_uptime = uptime
        identity_key = tuple(sorted(backend["identity"].items()))
        boots = (
            backend["uplink_boot_id"],
            *(item["boot_id"] for item in backend["scanners"]),
        )
        scanner_bindings = tuple(
            (
                item["slot"], item["mac"], item["boot_id"],
                item["profile"], item["role_generation"],
            )
            for item in backend["scanners"]
        )
        continuity = dict(backend["continuity"])
        queue = backend["upload_queue"]
        if (
            identity_key != baseline_identity
            or boots != baseline_boots
            or continuity != baseline_continuity
        ):
            raise EvidenceError("soak identity/boot/calibration continuity drifted")
        if scanner_bindings != baseline_scanner_bindings:
            raise EvidenceError("soak scanner binding drifted")
        if (
            queue["overflow_dropped_batches"] != baseline_drops
            or queue["quarantined_batches"] != baseline_quarantines
        ):
            raise EvidenceError("soak queue drop/quarantine counters increased")
    final_queue = _validate_soak_backend(monitor[-1]["backend"])["upload_queue"]["depth_batches"]
    if final_queue != 0:
        raise EvidenceError("soak ended with an unfinished queue")
    return SoakResult(
        device_id=monitor_device_id,
        duration_s=duration_ms // 1000,
        samples=len(monitor),
        max_heartbeat_gap_s=observed_max_gap,
        final_queue_depth=final_queue,
    )


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Read-only evidence recorder for the explicit no-screen three-board "
            "backend/Lite canary. Native badge 0.67.2 remains the default."
        )
    )
    commands = parser.add_subparsers(dest="command", required=True)

    common: dict[str, argparse.ArgumentParser] = {}
    for name in ("snapshot", "wait-detection", "wait-led", "wait-command", "monitor"):
        child = commands.add_parser(name)
        child.add_argument("--backend-base", required=True)
        child.add_argument("--device-id", required=True)
        child.add_argument("--output", type=Path, required=True)
        common[name] = child

    common["snapshot"].add_argument("--phase", required=True)

    detection = common["wait-detection"]
    detection.add_argument("--kind", choices=("drone", "meta"), required=True)
    detection.add_argument("--source", required=True)
    detection.add_argument(
        "--identity-field", choices=("drone_id", "bssid"), required=True,
    )
    detection.add_argument("--identity-value", required=True)
    detection.add_argument("--manufacturer")
    detection.add_argument("--service-uuid-token")
    detection.add_argument("--after-ms", type=int, required=True)
    detection.add_argument("--ready-file", type=Path, required=True)
    detection.add_argument("--timeout-s", type=int, default=120)

    led = common["wait-led"]
    led.add_argument("--expected", choices=tuple(sorted(LED_STATES)), required=True)
    led.add_argument("--after-ms", type=int, required=True)
    led.add_argument("--timeout-s", type=int, default=120)

    command = common["wait-command"]
    command.add_argument("--command-id", required=True)
    command.add_argument(
        "--terminal-state", choices=tuple(sorted(TERMINAL_STATES)), required=True,
    )
    command.add_argument("--timeout-s", type=int, default=120)

    monitor = common["monitor"]
    monitor.add_argument("--duration-s", type=int, default=86_400)
    monitor.add_argument("--interval-s", type=int, default=30)
    monitor.add_argument("--serial-log-dir", type=Path, required=True)

    verify = commands.add_parser("verify-soak")
    verify.add_argument("--input", type=Path, required=True)
    verify.add_argument("--duration-s", type=int, default=86_400)
    verify.add_argument("--max-heartbeat-gap-s", type=int, default=90)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        if args.command == "snapshot":
            result: Any = CanaryEvidenceRecorder(
                backend_base=args.backend_base,
                device_id=args.device_id,
                output=args.output,
            ).snapshot(args.phase)
        elif args.command == "wait-detection":
            result = wait_for_detection(
                backend_base=args.backend_base,
                device_id=args.device_id,
                kind=args.kind,
                source=args.source,
                identity_field=args.identity_field,
                identity_value=args.identity_value,
                manufacturer=args.manufacturer,
                service_uuid_token=args.service_uuid_token,
                after_ms=args.after_ms,
                ready_file=args.ready_file,
                timeout_s=args.timeout_s,
                output=args.output,
            )
        elif args.command == "wait-led":
            result = wait_for_led(
                backend_base=args.backend_base,
                device_id=args.device_id,
                expected=args.expected,
                after_ms=args.after_ms,
                timeout_s=args.timeout_s,
                output=args.output,
            )
        elif args.command == "wait-command":
            result = wait_for_command(
                backend_base=args.backend_base,
                device_id=args.device_id,
                command_id=args.command_id,
                terminal_state=args.terminal_state,
                timeout_s=args.timeout_s,
                output=args.output,
            )
        elif args.command == "monitor":
            monitor_soak(
                backend_base=args.backend_base,
                device_id=args.device_id,
                duration_s=args.duration_s,
                interval_s=args.interval_s,
                serial_log_dir=args.serial_log_dir,
                output=args.output,
            )
            result = {"ok": True, "duration_s": args.duration_s}
        else:
            result = asdict(verify_soak(
                args.input,
                expected_duration_s=args.duration_s,
                max_heartbeat_gap_s=args.max_heartbeat_gap_s,
            ))
    except EvidenceError as exc:
        print(f"backend canary evidence refused: {exc}", file=sys.stderr)
        return 2
    print(_canonical_json_bytes(result).decode("utf-8"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
