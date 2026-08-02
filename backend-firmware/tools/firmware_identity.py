"""Strict identity and integrity checks for backend ESP32-S3 app images."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
from pathlib import Path
import struct
import zlib


ESP_IMAGE_MAGIC = 0xE9
ESP_IMAGE_HEADER_SIZE = 24
ESP_SEGMENT_HEADER_SIZE = 8
ESP_APP_DESC_OFFSET = ESP_IMAGE_HEADER_SIZE + ESP_SEGMENT_HEADER_SIZE
ESP_APP_DESC_MAGIC = 0xABCD5432
ESP_APP_DESC_VERSION_OFFSET = 16
ESP_APP_DESC_PROJECT_OFFSET = 48
ESP_APP_DESC_STRING_SIZE = 32
ESP32_S3_CHIP_ID = 9

BACKEND_IDENTITY_MAGIC = 0x42464F46
BACKEND_IDENTITY_SCHEMA = 1
BACKEND_IDENTITY_SIZE = 164
BACKEND_IDENTITY_OFFSET = 0x120

_BACKEND_IDENTITY = struct.Struct("<IHH40s40s40s32sI")
_IMAGE_KIND_NAMES = {0: "uplink", 1: "scanner"}
_EXPECTED_IMAGE_KINDS = {
    ("uplink-s3-backend", "fof_backend_uplink"): 0,
    ("scanner-s3-combo-backend", "fof_backend_scanner"): 1,
}
_BADGE_MARKERS = (b"fof_badge", b"FOF_BADGE_VARIANT")


class FirmwareIdentityError(ValueError):
    """Raised when an app image is not an exact backend firmware image."""


@dataclass(frozen=True)
class BackendIdentityRecord:
    image_kind: int
    kind: str
    target: str
    project: str
    hardware: str
    version: str
    crc32: int
    offset: int


@dataclass(frozen=True)
class VerifiedFirmwareImage:
    path: Path
    target: str
    project: str
    hardware: str
    version: str
    image_kind: int
    identity_crc32: int
    partition_capacity: int
    size: int
    sha256: str
    crc32: int


@dataclass(frozen=True)
class _EspImageLayout:
    first_segment_offset: int
    first_segment_size: int


def _decode_c_string(field: bytes, name: str) -> str:
    try:
        terminator = field.index(0)
    except ValueError as exc:
        raise FirmwareIdentityError(f"{name} is not NUL-terminated") from exc
    if any(field[terminator:]):
        raise FirmwareIdentityError(f"{name} has a nonzero C-string tail")
    try:
        value = field[:terminator].decode("ascii", errors="strict")
    except UnicodeDecodeError as exc:
        raise FirmwareIdentityError(f"{name} is not strict ASCII") from exc
    if not value:
        raise FirmwareIdentityError(f"{name} is empty")
    return value


def _parse_esp_image_layout(image: bytes) -> _EspImageLayout:
    if not isinstance(image, bytes):
        raise FirmwareIdentityError("firmware image must be bytes")
    if not image:
        raise FirmwareIdentityError("firmware image is empty")
    if len(image) < ESP_IMAGE_HEADER_SIZE:
        raise FirmwareIdentityError("firmware image header is truncated")
    if image[0] != ESP_IMAGE_MAGIC:
        raise FirmwareIdentityError("invalid ESP image magic")

    segment_count = image[1]
    if not 1 <= segment_count <= 16:
        raise FirmwareIdentityError("ESP image segment count must be between 1 and 16")
    chip_id = struct.unpack_from("<H", image, 12)[0]
    if chip_id != ESP32_S3_CHIP_ID:
        raise FirmwareIdentityError("ESP image is not for the ESP32-S3 chip")
    if image[23] != 1:
        raise FirmwareIdentityError("ESP image must have an appended SHA-256 digest")

    cursor = ESP_IMAGE_HEADER_SIZE
    checksum = 0xEF
    first_segment_offset = 0
    first_segment_size = 0
    for segment_index in range(segment_count):
        header_end = cursor + ESP_SEGMENT_HEADER_SIZE
        if header_end > len(image):
            raise FirmwareIdentityError("ESP segment header is truncated")
        _load_address, segment_size = struct.unpack_from("<II", image, cursor)
        payload_offset = header_end
        payload_end = payload_offset + segment_size
        if payload_end > len(image):
            raise FirmwareIdentityError("ESP segment payload is truncated")
        if segment_index == 0:
            first_segment_offset = payload_offset
            first_segment_size = segment_size
        for value in image[payload_offset:payload_end]:
            checksum ^= value
        cursor = payload_end

    checksum_offset = ((cursor + 16) // 16) * 16 - 1
    digest_offset = checksum_offset + 1
    expected_end = digest_offset + hashlib.sha256().digest_size
    if expected_end < len(image):
        raise FirmwareIdentityError("ESP image has unexpected trailing data")
    if expected_end > len(image):
        raise FirmwareIdentityError("ESP checksum or appended digest is truncated")
    if any(image[cursor:checksum_offset]):
        raise FirmwareIdentityError("ESP checksum padding is not zero-filled")
    if image[checksum_offset] != checksum:
        raise FirmwareIdentityError("invalid ESP segment checksum")
    expected_digest = hashlib.sha256(image[:digest_offset]).digest()
    if image[digest_offset:] != expected_digest:
        raise FirmwareIdentityError("invalid ESP appended SHA-256 digest")

    return _EspImageLayout(
        first_segment_offset=first_segment_offset,
        first_segment_size=first_segment_size,
    )


def parse_esp_app_identity(image: bytes) -> tuple[str, str]:
    """Return ``(project, version)`` from a fully validated ESP app image."""

    layout = _parse_esp_image_layout(image)
    descriptor_end = ESP_APP_DESC_PROJECT_OFFSET + ESP_APP_DESC_STRING_SIZE
    if layout.first_segment_size < descriptor_end:
        raise FirmwareIdentityError("ESP app description is truncated")
    descriptor = image[
        layout.first_segment_offset : layout.first_segment_offset + descriptor_end
    ]
    magic = struct.unpack_from("<I", descriptor, 0)[0]
    if magic != ESP_APP_DESC_MAGIC:
        raise FirmwareIdentityError("invalid ESP app description magic")
    version = _decode_c_string(
        descriptor[
            ESP_APP_DESC_VERSION_OFFSET :
            ESP_APP_DESC_VERSION_OFFSET + ESP_APP_DESC_STRING_SIZE
        ],
        "app description version",
    )
    project = _decode_c_string(
        descriptor[
            ESP_APP_DESC_PROJECT_OFFSET :
            ESP_APP_DESC_PROJECT_OFFSET + ESP_APP_DESC_STRING_SIZE
        ],
        "app description project",
    )
    return project, version


def _magic_offsets(image: bytes) -> list[int]:
    marker = BACKEND_IDENTITY_MAGIC.to_bytes(4, "little")
    offsets: list[int] = []
    start = 0
    while True:
        offset = image.find(marker, start)
        if offset < 0:
            return offsets
        offsets.append(offset)
        start = offset + 1


def _parse_backend_identity_at(image: bytes, offset: int) -> BackendIdentityRecord:
    end = offset + BACKEND_IDENTITY_SIZE
    if end > len(image):
        raise FirmwareIdentityError("backend identity record is truncated")

    (
        magic,
        schema,
        image_kind,
        target_field,
        project_field,
        hardware_field,
        version_field,
        recorded_crc,
    ) = _BACKEND_IDENTITY.unpack(image[offset:end])
    if magic != BACKEND_IDENTITY_MAGIC:
        raise FirmwareIdentityError("invalid backend identity record magic")
    if schema != BACKEND_IDENTITY_SCHEMA:
        raise FirmwareIdentityError("unsupported backend identity schema")
    if image_kind not in _IMAGE_KIND_NAMES:
        raise FirmwareIdentityError("invalid backend identity image kind")

    record_prefix = image[offset : offset + 160]
    computed_crc = zlib.crc32(record_prefix) & 0xFFFFFFFF
    if recorded_crc != computed_crc:
        raise FirmwareIdentityError("backend identity CRC mismatch")

    return BackendIdentityRecord(
        image_kind=image_kind,
        kind=_IMAGE_KIND_NAMES[image_kind],
        target=_decode_c_string(target_field, "backend identity target"),
        project=_decode_c_string(project_field, "backend identity project"),
        hardware=_decode_c_string(hardware_field, "backend identity hardware"),
        version=_decode_c_string(version_field, "backend identity version"),
        crc32=recorded_crc,
        offset=offset,
    )


def _relaxed_candidate_string(field: bytes) -> str | None:
    terminator = field.find(0)
    if terminator <= 0:
        return None
    prefix = field[:terminator]
    if any(value < 0x20 or value > 0x7E for value in prefix):
        return None
    try:
        return prefix.decode("ascii", errors="strict")
    except UnicodeDecodeError:
        return None


def _looks_like_identity_candidate(image: bytes, offset: int) -> bool:
    end = offset + BACKEND_IDENTITY_SIZE
    if end > len(image):
        return False
    (
        _magic,
        schema,
        image_kind,
        target_field,
        project_field,
        hardware_field,
        version_field,
        _crc,
    ) = _BACKEND_IDENTITY.unpack(image[offset:end])
    values = (
        _relaxed_candidate_string(target_field),
        _relaxed_candidate_string(project_field),
        _relaxed_candidate_string(hardware_field),
        _relaxed_candidate_string(version_field),
    )
    target, project, hardware, version = values
    identity_markers = sum(
        (
            bool(target and target.endswith("-backend")),
            bool(project and project.startswith("fof_backend_")),
            bool(hardware and "esp32" in hardware.lower()),
            bool(version and version.endswith("-backend")),
        )
    )
    printable_fields = sum(value is not None for value in values)
    return identity_markers >= 2 or (
        schema <= 16 and image_kind in _IMAGE_KIND_NAMES and printable_fields >= 2
    )


def parse_backend_identity_record(image: bytes) -> BackendIdentityRecord:
    """Validate the canonical record and reject any second record-shaped hit."""

    if not isinstance(image, bytes):
        raise FirmwareIdentityError("firmware image must be bytes")
    marker = BACKEND_IDENTITY_MAGIC.to_bytes(4, "little")
    canonical_end = BACKEND_IDENTITY_OFFSET + BACKEND_IDENTITY_SIZE
    if canonical_end > len(image) or image[
        BACKEND_IDENTITY_OFFSET : BACKEND_IDENTITY_OFFSET + 4
    ] != marker:
        raise FirmwareIdentityError(
            "firmware is missing the canonical backend identity record at offset 0x120"
        )
    record = _parse_backend_identity_at(image, BACKEND_IDENTITY_OFFSET)
    for offset in _magic_offsets(image):
        if offset == BACKEND_IDENTITY_OFFSET:
            continue
        try:
            _parse_backend_identity_at(image, offset)
        except FirmwareIdentityError:
            if not _looks_like_identity_candidate(image, offset):
                continue
        raise FirmwareIdentityError(
            "firmware contains a second structured backend identity candidate"
        )
    return record


def verify_backend_image(
    path: Path,
    *,
    target: str,
    project: str,
    hardware: str,
    version: str,
    partition_capacity: int,
) -> VerifiedFirmwareImage:
    """Validate one app image against an exact backend release identity."""

    if (
        not isinstance(partition_capacity, int)
        or isinstance(partition_capacity, bool)
        or partition_capacity <= 0
    ):
        raise FirmwareIdentityError("partition capacity must be a positive integer")
    image_path = Path(path)
    try:
        image = image_path.read_bytes()
    except OSError as exc:
        raise FirmwareIdentityError(f"cannot read firmware image: {image_path}") from exc
    if not image:
        raise FirmwareIdentityError("firmware image is empty")
    if len(image) > partition_capacity:
        raise FirmwareIdentityError("firmware image exceeds its partition capacity")

    app_project, app_version = parse_esp_app_identity(image)
    record = parse_backend_identity_record(image)
    if app_project != record.project or app_version != record.version:
        raise FirmwareIdentityError(
            "structured backend identity and ESP app description disagree"
        )

    expected_kind = _EXPECTED_IMAGE_KINDS.get((target, project))
    if expected_kind is None:
        raise FirmwareIdentityError("requested release identity is not a backend target")
    comparisons = (
        ("target", record.target, target),
        ("project", record.project, project),
        ("hardware", record.hardware, hardware),
        ("version", record.version, version),
        ("image kind", record.image_kind, expected_kind),
    )
    for field_name, actual, expected in comparisons:
        if actual != expected:
            raise FirmwareIdentityError(
                f"backend identity {field_name} mismatch: expected {expected!r}, "
                f"got {actual!r}"
            )
    for marker in _BADGE_MARKERS:
        if marker in image:
            raise FirmwareIdentityError("legacy badge marker coexists with backend identity")

    return VerifiedFirmwareImage(
        path=image_path,
        target=target,
        project=project,
        hardware=hardware,
        version=version,
        image_kind=record.image_kind,
        identity_crc32=record.crc32,
        partition_capacity=partition_capacity,
        size=len(image),
        sha256=hashlib.sha256(image).hexdigest(),
        crc32=zlib.crc32(image) & 0xFFFFFFFF,
    )
