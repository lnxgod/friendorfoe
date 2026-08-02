from __future__ import annotations

import hashlib
from pathlib import Path
import struct
import zlib

import pytest

from tools.firmware_identity import (
    BACKEND_IDENTITY_MAGIC,
    ESP_APP_DESC_MAGIC,
    FirmwareIdentityError,
    parse_backend_identity_record,
    parse_esp_app_identity,
    verify_backend_image,
)


IDENTITY_MAGIC = 0x42464F46
APP_DESC_MAGIC = 0xABCD5432
IDENTITY = struct.Struct("<IHH40s40s40s32sI")
IMAGE_HEADER_SIZE = 24
SEGMENT_HEADER_SIZE = 8
SEGMENT_OFFSET = IMAGE_HEADER_SIZE + SEGMENT_HEADER_SIZE
RECORD_OFFSET = 256


def c_field(value: str, size: int) -> bytes:
    encoded = value.encode("ascii")
    assert len(encoded) < size
    return encoded + bytes(size - len(encoded))


def identity_record(
    *,
    image_kind: int = 1,
    target: str = "scanner-s3-combo-backend",
    project: str = "fof_backend_scanner",
    hardware: str = "seeed_xiao_esp32s3",
    version: str = "0.1.0-backend",
) -> bytes:
    prefix = struct.pack(
        "<IHH40s40s40s32s",
        IDENTITY_MAGIC,
        1,
        image_kind,
        c_field(target, 40),
        c_field(project, 40),
        c_field(hardware, 40),
        c_field(version, 32),
    )
    assert len(prefix) == 160
    return prefix + struct.pack("<I", zlib.crc32(prefix) & 0xFFFFFFFF)


def assemble_image(
    segment: bytes,
    *,
    image_magic: int = 0xE9,
    segment_count: int = 1,
    chip_id: int = 9,
    hash_appended: int = 1,
) -> bytes:
    common = struct.pack("<BBBBI", image_magic, segment_count, 2, 0x3F, 0)
    extended = struct.pack(
        "<BBBBHBHHBBBBB",
        0xEE,
        0,
        0,
        0,
        chip_id,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        hash_appended,
    )
    image = bytearray(common + extended)
    assert len(image) == IMAGE_HEADER_SIZE
    image += struct.pack("<II", 0x3FC88000, len(segment))
    image += segment
    checksum = 0xEF
    for value in segment:
        checksum ^= value
    while len(image) % 16 != 15:
        image.append(0)
    image.append(checksum)
    image += hashlib.sha256(image).digest()
    return bytes(image)


def fake_app_image(
    project: str = "fof_backend_scanner",
    version: str = "0.1.0-backend",
    *,
    image_kind: int = 1,
    target: str = "scanner-s3-combo-backend",
    hardware: str = "seeed_xiao_esp32s3",
    second_record: bytes | None = None,
) -> bytes:
    segment = bytearray(768)
    segment[0:4] = APP_DESC_MAGIC.to_bytes(4, "little")
    segment[16:48] = c_field(version, 32)
    segment[48:80] = c_field(project, 32)
    record = identity_record(
        image_kind=image_kind,
        target=target,
        project=project,
        hardware=hardware,
        version=version,
    )
    segment[RECORD_OFFSET : RECORD_OFFSET + len(record)] = record
    if second_record is not None:
        segment[512 : 512 + len(second_record)] = second_record
    return assemble_image(segment)


def image_segment(image: bytes) -> bytearray:
    size = struct.unpack_from("<I", image, IMAGE_HEADER_SIZE + 4)[0]
    return bytearray(image[SEGMENT_OFFSET : SEGMENT_OFFSET + size])


def rebuild_with_segment(image: bytes, segment: bytes) -> bytes:
    return assemble_image(
        segment,
        image_magic=image[0],
        segment_count=image[1],
        chip_id=struct.unpack_from("<H", image, 12)[0],
        hash_appended=image[23],
    )


def scanner_expectations(path: Path, *, capacity: int = 0x200000):
    return verify_backend_image(
        path,
        target="scanner-s3-combo-backend",
        project="fof_backend_scanner",
        hardware="seeed_xiao_esp32s3",
        version="0.1.0-backend",
        partition_capacity=capacity,
    )


def test_constants_match_the_wire_contract() -> None:
    assert BACKEND_IDENTITY_MAGIC == IDENTITY_MAGIC
    assert ESP_APP_DESC_MAGIC == APP_DESC_MAGIC


def test_parse_exact_scanner_descriptor_and_markers(tmp_path: Path) -> None:
    image = fake_app_image()
    path = tmp_path / "firmware.bin"
    path.write_bytes(image)

    result = scanner_expectations(path)

    assert parse_esp_app_identity(image) == (
        "fof_backend_scanner",
        "0.1.0-backend",
    )
    record = parse_backend_identity_record(image)
    assert record.image_kind == 1
    assert record.kind == "scanner"
    assert record.target == "scanner-s3-combo-backend"
    assert result.project == "fof_backend_scanner"
    assert result.version == "0.1.0-backend"
    assert result.image_kind == 1
    assert result.identity_crc32 == record.crc32
    assert result.size == len(image)
    assert result.sha256 == hashlib.sha256(image).hexdigest()
    assert result.crc32 == zlib.crc32(image) & 0xFFFFFFFF


def test_parse_exact_uplink_descriptor_and_markers(tmp_path: Path) -> None:
    image = fake_app_image(
        "fof_backend_uplink",
        image_kind=0,
        target="uplink-s3-backend",
    )
    path = tmp_path / "firmware.bin"
    path.write_bytes(image)

    result = verify_backend_image(
        path,
        target="uplink-s3-backend",
        project="fof_backend_uplink",
        hardware="seeed_xiao_esp32s3",
        version="0.1.0-backend",
        partition_capacity=0x200000,
    )

    assert result.image_kind == 0
    assert parse_backend_identity_record(image).kind == "uplink"


@pytest.mark.parametrize(
    ("mutation", "bad_value"),
    [
        ("project", "fof_badge_scanner"),
        ("version", "0.64.67"),
        ("target", "scanner-s3-combo-fof_badge"),
        ("hardware", "seed_scanner"),
    ],
)
def test_rejects_each_cross_family_or_missing_identity(
    tmp_path: Path, mutation: str, bad_value: str
) -> None:
    values: dict[str, object] = {
        "project": "fof_backend_scanner",
        "version": "0.1.0-backend",
        "target": "scanner-s3-combo-backend",
        "hardware": "seeed_xiao_esp32s3",
        "image_kind": 1,
    }
    values[mutation] = bad_value
    path = tmp_path / "firmware.bin"
    path.write_bytes(fake_app_image(**values))

    with pytest.raises(FirmwareIdentityError):
        scanner_expectations(path)


@pytest.mark.parametrize(
    "image",
    [
        b"",
        assemble_image(bytes(768), image_magic=0xEA),
        assemble_image(bytes(768), segment_count=0),
        assemble_image(bytes(768), segment_count=17),
        assemble_image(bytes(768), chip_id=8),
        assemble_image(bytes(768), hash_appended=0),
    ],
    ids=[
        "empty",
        "bad-image-magic",
        "zero-segments",
        "too-many-segments",
        "wrong-chip",
        "missing-appended-hash",
    ],
)
def test_rejects_invalid_esp_image_header(image: bytes) -> None:
    with pytest.raises(FirmwareIdentityError):
        parse_esp_app_identity(image)


def test_rejects_truncated_segment_table() -> None:
    image = bytearray(fake_app_image())
    image[1] = 2
    with pytest.raises(FirmwareIdentityError):
        parse_esp_app_identity(bytes(image))


def test_rejects_bad_app_description_magic() -> None:
    image = fake_app_image()
    segment = image_segment(image)
    segment[0:4] = (APP_DESC_MAGIC + 1).to_bytes(4, "little")
    with pytest.raises(FirmwareIdentityError, match="description magic"):
        parse_esp_app_identity(rebuild_with_segment(image, segment))


@pytest.mark.parametrize(("offset", "name"), [(16, "version"), (48, "project")])
def test_rejects_non_nul_terminated_app_descriptor_string(
    offset: int, name: str
) -> None:
    image = fake_app_image()
    segment = image_segment(image)
    segment[offset : offset + 32] = b"X" * 32
    with pytest.raises(FirmwareIdentityError, match=name):
        parse_esp_app_identity(rebuild_with_segment(image, segment))


@pytest.mark.parametrize(("offset", "name"), [(16, "version"), (48, "project")])
def test_rejects_nonzero_app_descriptor_tail(offset: int, name: str) -> None:
    image = fake_app_image()
    segment = image_segment(image)
    nul = segment.index(0, offset, offset + 32)
    segment[nul + 1] = ord("X")
    with pytest.raises(FirmwareIdentityError, match=name):
        parse_esp_app_identity(rebuild_with_segment(image, segment))


def test_rejects_invalid_segment_checksum() -> None:
    image = bytearray(fake_app_image())
    image[-33] ^= 0x01
    digest = hashlib.sha256(image[:-32]).digest()
    image[-32:] = digest
    with pytest.raises(FirmwareIdentityError, match="checksum"):
        parse_esp_app_identity(bytes(image))


def test_rejects_invalid_appended_digest() -> None:
    image = bytearray(fake_app_image())
    image[-1] ^= 0x01
    with pytest.raises(FirmwareIdentityError, match="SHA-256"):
        parse_esp_app_identity(bytes(image))


def test_rejects_extra_bytes_after_appended_digest() -> None:
    with pytest.raises(FirmwareIdentityError, match="trailing"):
        parse_esp_app_identity(fake_app_image() + b"X")


@pytest.mark.parametrize(
    ("field_offset", "field_size", "name"),
    [
        (8, 40, "target"),
        (48, 40, "project"),
        (88, 40, "hardware"),
        (128, 32, "version"),
    ],
)
def test_rejects_nonzero_identity_string_tail(
    field_offset: int, field_size: int, name: str
) -> None:
    record = bytearray(identity_record())
    nul = record.index(0, field_offset, field_offset + field_size)
    record[nul + 1] = ord("X")
    struct.pack_into("<I", record, 160, zlib.crc32(record[:160]) & 0xFFFFFFFF)
    image = fake_app_image()
    segment = image_segment(image)
    segment[RECORD_OFFSET : RECORD_OFFSET + IDENTITY.size] = record
    with pytest.raises(FirmwareIdentityError, match=name):
        parse_backend_identity_record(rebuild_with_segment(image, segment))


@pytest.mark.parametrize(
    ("offset", "packed", "message"),
    [
        (0, struct.pack("<I", IDENTITY_MAGIC + 1), "identity record"),
        (4, struct.pack("<H", 2), "schema"),
        (6, struct.pack("<H", 2), "image kind"),
    ],
)
def test_rejects_structured_record_header_mutations(
    offset: int, packed: bytes, message: str
) -> None:
    record = bytearray(identity_record())
    record[offset : offset + len(packed)] = packed
    struct.pack_into("<I", record, 160, zlib.crc32(record[:160]) & 0xFFFFFFFF)
    image = fake_app_image()
    segment = image_segment(image)
    segment[RECORD_OFFSET : RECORD_OFFSET + IDENTITY.size] = record
    with pytest.raises(FirmwareIdentityError, match=message):
        parse_backend_identity_record(rebuild_with_segment(image, segment))


def test_rejects_identity_crc_mutation() -> None:
    image = fake_app_image()
    segment = image_segment(image)
    segment[RECORD_OFFSET + 160] ^= 0x01
    with pytest.raises(FirmwareIdentityError, match="CRC"):
        parse_backend_identity_record(rebuild_with_segment(image, segment))


def test_rejects_duplicate_otherwise_valid_identity_record() -> None:
    image = fake_app_image(second_record=identity_record())
    with pytest.raises(FirmwareIdentityError, match="second.*identity"):
        parse_backend_identity_record(image)


@pytest.mark.parametrize(
    "runtime_constant",
    [
        bytes.fromhex(
            "464f4642f0560a4238570a42bc560a420028ffffffdb0000ffdf00000024ffff"
            "60570a42ffff1000ffffff0fcdcccccc"
        ),
        bytes.fromhex(
            "464f4642fe0f0000b01b0c3c40170c3c4c170c3c58170c3c68170c3ca0130c3c"
            "b00b0c3cb0640a42f8640a427c640a42"
        ),
    ],
    ids=["scanner-runtime-constant", "uplink-runtime-constant"],
)
def test_ignores_clearly_non_record_runtime_magic_constant(
    runtime_constant: bytes,
) -> None:
    image = fake_app_image()
    segment = image_segment(image)
    segment[512 : 512 + len(runtime_constant)] = runtime_constant

    record = parse_backend_identity_record(rebuild_with_segment(image, segment))

    assert record.offset == 0x120
    assert record.kind == "scanner"


@pytest.mark.parametrize("mutation", ["schema", "crc"])
def test_rejects_second_record_shaped_candidate_even_when_malformed(
    mutation: str,
) -> None:
    second = bytearray(identity_record())
    if mutation == "schema":
        struct.pack_into("<H", second, 4, 2)
        struct.pack_into("<I", second, 160, zlib.crc32(second[:160]) & 0xFFFFFFFF)
    else:
        second[160] ^= 1
    image = fake_app_image(second_record=bytes(second))

    with pytest.raises(FirmwareIdentityError, match="second.*identity"):
        parse_backend_identity_record(image)


def test_rejects_valid_identity_moved_from_canonical_linked_offset() -> None:
    image = fake_app_image()
    segment = image_segment(image)
    record = bytes(segment[RECORD_OFFSET : RECORD_OFFSET + IDENTITY.size])
    segment[RECORD_OFFSET : RECORD_OFFSET + IDENTITY.size] = bytes(IDENTITY.size)
    segment[512 : 512 + IDENTITY.size] = record

    with pytest.raises(FirmwareIdentityError, match="canonical"):
        parse_backend_identity_record(rebuild_with_segment(image, segment))


def test_loose_target_and_hardware_substrings_are_not_identity_evidence() -> None:
    segment = bytearray(768)
    segment[0:4] = APP_DESC_MAGIC.to_bytes(4, "little")
    segment[16:48] = c_field("0.1.0-backend", 32)
    segment[48:80] = c_field("fof_backend_scanner", 32)
    segment[256:320] = b"scanner-s3-combo-backend seeed_xiao_esp32s3".ljust(64, b"\0")
    with pytest.raises(FirmwareIdentityError, match="identity record"):
        parse_backend_identity_record(assemble_image(segment))


def test_rejects_record_and_app_descriptor_disagreement(tmp_path: Path) -> None:
    image = fake_app_image()
    segment = image_segment(image)
    segment[48:80] = c_field("fof_backend_uplink", 32)
    path = tmp_path / "firmware.bin"
    path.write_bytes(rebuild_with_segment(image, segment))
    with pytest.raises(FirmwareIdentityError, match="disagree"):
        scanner_expectations(path)


def test_rejects_scanner_identity_with_uplink_image_kind(tmp_path: Path) -> None:
    path = tmp_path / "firmware.bin"
    path.write_bytes(fake_app_image(image_kind=0))
    with pytest.raises(FirmwareIdentityError, match="image kind"):
        scanner_expectations(path)


@pytest.mark.parametrize("marker", [b"fof_badge", b"FOF_BADGE_VARIANT"])
def test_rejects_legacy_badge_marker_coexisting_with_backend_identity(
    tmp_path: Path, marker: bytes
) -> None:
    image = fake_app_image()
    segment = image_segment(image)
    segment[700 : 700 + len(marker)] = marker
    path = tmp_path / "firmware.bin"
    path.write_bytes(rebuild_with_segment(image, segment))
    with pytest.raises(FirmwareIdentityError, match="badge"):
        scanner_expectations(path)


def test_rejects_image_one_byte_larger_than_partition(tmp_path: Path) -> None:
    path = tmp_path / "firmware.bin"
    path.write_bytes(bytes(0x200001))
    with pytest.raises(FirmwareIdentityError, match="partition"):
        scanner_expectations(path)


@pytest.mark.parametrize("capacity", [0, -1, True])
def test_rejects_invalid_partition_capacity(tmp_path: Path, capacity: object) -> None:
    path = tmp_path / "firmware.bin"
    path.write_bytes(fake_app_image())
    with pytest.raises(FirmwareIdentityError, match="partition capacity"):
        scanner_expectations(path, capacity=capacity)  # type: ignore[arg-type]
