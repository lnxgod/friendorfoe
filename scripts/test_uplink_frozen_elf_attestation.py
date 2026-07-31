from __future__ import annotations

import hashlib
import struct
import sys
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]
ESP32_SCRIPTS = REPO_ROOT / "esp32" / "scripts"
if str(ESP32_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(ESP32_SCRIPTS))

import secure_artifact_tree as artifacts
import verify_badge_uplink_build as uplink_verify


_APPDESC_ADDRESS = 0x3C000020
_TEXT_ADDRESS = 0x40374000
_RTC_ADDRESS = 0x50000000
_ELF_SHA256_APP_IMAGE_OFFSET = 0xB0
_APPDESC_ELF_SHA256_OFFSET = 0x90


def _elf32(
    *,
    rtc_size: int,
    text: bytes = b"\x11\x22\x33\x44",
    initialized_rtc: bytes | None = None,
    appdesc_load_gap: bytes = b"",
    extra_program_headers: tuple[
        tuple[int, int, int, int, int, int, int, int], ...
    ] = (),
    extra_sections: tuple[
        tuple[str, int, int, int, int, int, bytes], ...
    ] = (),
    extra_symbols: tuple[
        tuple[str, int, int, int, int, int], ...
    ] = (),
) -> bytes:
    """Build a minimal executable Xtensa ELF with the production RTC ABI."""
    appdesc = bytearray(0x100)
    struct.pack_into("<I", appdesc, 0, 0xABCD5432)
    appdesc[16:48] = b"0.64.78-badge-defcon34".ljust(32, b"\x00")
    appdesc[48:80] = b"fof_badge_uplink".ljust(32, b"\x00")

    sections = [
        (".flash.appdesc", 1, 0x2, _APPDESC_ADDRESS, 16, bytes(appdesc)),
        (".text", 1, 0x6, _TEXT_ADDRESS, 4, text),
        (".rtc_noinit", 8, 0x3, _RTC_ADDRESS, 4, b"\x00" * rtc_size),
    ]
    if initialized_rtc is not None:
        sections.append((
            ".rtc.force_slow",
            1,
            0x3,
            _RTC_ADDRESS + rtc_size,
            4,
            initialized_rtc,
        ))
    sections.extend(extra_sections)
    section_index = {
        name: index + 1 for index, (name, *_rest) in enumerate(sections)
    }
    canonical_symbols = (
        (
            "g_fof_badge_rtc_state",
            _RTC_ADDRESS,
            rtc_size,
            0x11,
            0,
            section_index[".rtc_noinit"],
        ),
        (
            "fof_badge_rtc_usb_recovery_once_magic",
            _RTC_ADDRESS,
            0,
            0x10,
            0,
            section_index[".rtc_noinit"],
        ),
        (
            "fof_badge_rtc_expected_reboot_generation",
            _RTC_ADDRESS + 4,
            0,
            0x10,
            0,
            section_index[".rtc_noinit"],
        ),
        (
            "fof_badge_rtc_expected_reboot_magic",
            _RTC_ADDRESS + 8,
            0,
            0x10,
            0,
            section_index[".rtc_noinit"],
        ),
    )
    all_symbols = (*canonical_symbols, *extra_symbols)

    string_table = bytearray(b"\x00")
    string_offsets: dict[str, int] = {}
    for name, *_rest in all_symbols:
        if name not in string_offsets:
            string_offsets[name] = len(string_table)
            string_table.extend(name.encode("ascii"))
            string_table.append(0)
    symbol_table = bytearray(b"\x00" * 16)
    for name, value, size, info, other, symbol_section in all_symbols:
        symbol_table.extend(struct.pack(
            "<IIIBBH",
            string_offsets[name],
            value,
            size,
            info,
            other,
            symbol_section,
        ))

    section_names = bytearray(b"\x00")
    section_name_offsets: dict[str, int] = {}
    for name in (
        *(section[0] for section in sections),
        ".strtab",
        ".symtab",
        ".shstrtab",
    ):
        if name not in section_name_offsets:
            section_name_offsets[name] = len(section_names)
            section_names.extend(name.encode("ascii"))
            section_names.append(0)

    program_count = 3 + len(extra_program_headers)
    program_header_offset = 52
    payload = bytearray(b"\x00" * (52 + program_count * 32))

    def append_aligned(data: bytes, alignment: int = 4) -> tuple[int, int]:
        while len(payload) % alignment:
            payload.append(0)
        offset = len(payload)
        payload.extend(data)
        return offset, len(data)

    section_payloads: list[tuple[int, int]] = []
    for section_number, (
        _name,
        section_type,
        _flags,
        _address,
        alignment,
        data,
    ) in enumerate(sections):
        if section_type == 8:
            section_payloads.append((0, len(data)))
        else:
            if _name == ".rtc.force_slow" and initialized_rtc is not None:
                payload.extend(b"\x00" * rtc_size)
            section_payloads.append(append_aligned(data, alignment))
        if section_number == 0:
            payload.extend(appdesc_load_gap)
    strtab_offset, strtab_size = append_aligned(bytes(string_table))
    symtab_offset, symtab_size = append_aligned(bytes(symbol_table))
    shstrtab_offset, shstrtab_size = append_aligned(bytes(section_names))
    while len(payload) % 4:
        payload.append(0)
    section_header_offset = len(payload)

    headers = [(0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
    for (
        name,
        section_type,
        flags,
        address,
        alignment,
        _data,
    ), (file_offset, size) in zip(sections, section_payloads):
        headers.append((
            section_name_offsets[name],
            section_type,
            flags,
            address,
            file_offset,
            size,
            0,
            0,
            alignment,
            0,
        ))
    strtab_index = len(headers)
    headers.append((
        section_name_offsets[".strtab"],
        3,
        0,
        0,
        strtab_offset,
        strtab_size,
        0,
        0,
        1,
        0,
    ))
    headers.append((
        section_name_offsets[".symtab"],
        2,
        0,
        0,
        symtab_offset,
        symtab_size,
        strtab_index,
        1,
        4,
        16,
    ))
    shstrtab_index = len(headers)
    headers.append((
        section_name_offsets[".shstrtab"],
        3,
        0,
        0,
        shstrtab_offset,
        shstrtab_size,
        0,
        0,
        1,
        0,
    ))
    for header in headers:
        payload.extend(struct.pack("<IIIIIIIIII", *header))

    appdesc_offset, appdesc_size = section_payloads[
        section_index[".flash.appdesc"] - 1
    ]
    text_offset, text_size = section_payloads[
        section_index[".text"] - 1
    ]
    rtc_file_offset = 0
    rtc_file_size = 0
    if initialized_rtc is not None:
        rtc_section_offset, rtc_section_size = section_payloads[
            section_index[".rtc.force_slow"] - 1
        ]
        rtc_file_offset = rtc_section_offset - rtc_size
        rtc_file_size = rtc_size + rtc_section_size
        struct.pack_into(
            "<I",
            payload,
            section_header_offset +
            section_index[".rtc_noinit"] * 40 +
            16,
            rtc_file_offset,
        )
    program_headers = (
        (
            1,
            appdesc_offset,
            _APPDESC_ADDRESS,
            _APPDESC_ADDRESS,
            appdesc_size + len(appdesc_load_gap),
            appdesc_size + len(appdesc_load_gap),
            4,
            4,
        ),
        (
            1,
            text_offset,
            _TEXT_ADDRESS,
            _TEXT_ADDRESS,
            text_size,
            text_size,
            5,
            4,
        ),
        (
            1,
            rtc_file_offset,
            _RTC_ADDRESS,
            _RTC_ADDRESS,
            rtc_file_size,
            max(rtc_size, rtc_file_size),
            6,
            4,
        ),
        *extra_program_headers,
    )
    for index, program_header in enumerate(program_headers):
        struct.pack_into(
            "<IIIIIIII",
            payload,
            program_header_offset + index * 32,
            *program_header,
        )

    payload[:16] = (
        b"\x7fELF"
        b"\x01\x01\x01\x00"
        b"\x00\x00\x00\x00\x00\x00\x00\x00"
    )
    struct.pack_into(
        "<HHIIIIIHHHHHH",
        payload,
        16,
        2,
        94,
        1,
        _TEXT_ADDRESS,
        program_header_offset,
        section_header_offset,
        0,
        52,
        32,
        program_count,
        40,
        len(headers),
        shstrtab_index,
    )
    return bytes(payload)


def _app_image(elf: bytes) -> bytes:
    section_offset = struct.unpack_from("<I", elf, 32)[0]
    section_count = struct.unpack_from("<H", elf, 48)[0]
    appdesc: bytes | None = None
    load_sections: list[tuple[int, bytes]] = []
    for index in range(section_count):
        raw = struct.unpack_from(
            "<IIIIIIIIII", elf, section_offset + index * 40
        )
        if raw[3] == _APPDESC_ADDRESS:
            appdesc = elf[raw[4]:raw[4] + raw[5]]
        if (
            raw[1] == 1 and
            raw[2] & 0x2 != 0 and
            raw[3] != 0 and
            raw[5] > 0
        ):
            load_sections.append((
                raw[3],
                elf[raw[4]:raw[4] + raw[5]],
            ))
    assert appdesc is not None

    patched_appdesc = bytearray(appdesc)
    patched_appdesc[
        _APPDESC_ELF_SHA256_OFFSET:
        _APPDESC_ELF_SHA256_OFFSET + 32
    ] = hashlib.sha256(elf).digest()
    header = struct.pack(
        "<BBBBI",
        0xE9,
        len(load_sections),
        2,
        0x3F,
        struct.unpack_from("<I", elf, 24)[0],
    )
    header += struct.pack(
        "<BBBBHBHH4sB",
        0xEE,
        0,
        0,
        0,
        9,
        0,
        0,
        0xFFFF,
        b"\x00" * 4,
        1,
    )
    image = bytearray(header)
    checksum = 0xEF
    for address, data in load_sections:
        emitted = (
            bytes(patched_appdesc)
            if address == _APPDESC_ADDRESS
            else data
        )
        image.extend(struct.pack("<II", address, len(emitted)))
        image.extend(emitted)
        for value in emitted:
            checksum ^= value
    image.extend(b"\x00" * (15 - len(image) % 16))
    image.append(checksum)
    image.extend(hashlib.sha256(image).digest())
    assert image[
        _ELF_SHA256_APP_IMAGE_OFFSET:
        _ELF_SHA256_APP_IMAGE_OFFSET + 32
    ] == hashlib.sha256(elf).digest()
    return bytes(image)


def _reseal_app_image(image: bytes) -> bytes:
    """Recompute the checksum and digest after one adversarial mutation."""
    sealed = bytearray(image)
    checksum = 0xEF
    position = 24
    for _index in range(sealed[1]):
        size = struct.unpack_from("<I", sealed, position + 4)[0]
        position += 8
        for value in sealed[position:position + size]:
            checksum ^= value
        position += size
    checksum_offset = position + (15 - position % 16)
    sealed[checksum_offset] = checksum
    sealed[-32:] = hashlib.sha256(sealed[:-32]).digest()
    return bytes(sealed)


def _append_app_image_segment(
    image: bytes,
    *,
    address: int,
    data: bytes,
) -> bytes:
    segments: list[tuple[int, bytes]] = []
    position = 24
    for _index in range(image[1]):
        segment_address, size = struct.unpack_from("<II", image, position)
        position += 8
        segments.append((
            segment_address,
            image[position:position + size],
        ))
        position += size
    rebuilt = bytearray(image[:24])
    rebuilt[1] += 1
    checksum = 0xEF
    for segment_address, segment_data in (
        *segments,
        (address, data),
    ):
        rebuilt.extend(struct.pack(
            "<II", segment_address, len(segment_data)
        ))
        rebuilt.extend(segment_data)
        for value in segment_data:
            checksum ^= value
    rebuilt.extend(b"\x00" * (15 - len(rebuilt) % 16))
    rebuilt.append(checksum)
    rebuilt.extend(hashlib.sha256(rebuilt).digest())
    return bytes(rebuilt)


def _frozen(elf: bytes, firmware: bytes) -> artifacts.FrozenArtifactSet:
    receipt_sha256 = "7" * 64
    members = tuple(
        artifacts.FrozenArtifactMember(
            logical_name=name,
            size=len(content),
            sha256=hashlib.sha256(content).hexdigest(),
            content=content,
        )
        for name, content in (
            ("artifact.elf", elf),
            ("artifact.firmware", firmware),
        )
    )
    return artifacts.FrozenArtifactSet(
        receipt_sha256=receipt_sha256,
        members=members,
        aggregate_sha256=artifacts._aggregate_sha256(
            receipt_sha256, members
        ),
    )


@pytest.mark.parametrize("rtc_size", (0x14, 0xC8))
def test_frozen_uplink_attestation_accepts_exact_release_layout(
    rtc_size: int,
) -> None:
    elf = _elf32(rtc_size=rtc_size)

    assert uplink_verify.verify_frozen_badge_uplink_attestation(
        _frozen(elf, _app_image(elf)),
        label="release",
        expected_rtc_size=rtc_size,
    ) == []


def test_frozen_uplink_attestation_rejects_elf_from_another_build() -> None:
    flashed_elf = _elf32(rtc_size=0x14)
    detached_elf = _elf32(
        rtc_size=0x14,
        text=b"\x99\x88\x77\x66",
    )

    errors = uplink_verify.verify_frozen_badge_uplink_attestation(
        _frozen(detached_elf, _app_image(flashed_elf)),
        label="release",
        expected_rtc_size=0x14,
    )

    assert any("ELF SHA-256 provenance" in error for error in errors), errors


def test_frozen_uplink_attestation_rejects_forged_hash_with_wrong_payload(
) -> None:
    flashed_elf = _elf32(rtc_size=0x14)
    detached_elf = _elf32(
        rtc_size=0x14,
        text=b"\x99\x88\x77\x66",
    )
    forged = bytearray(_app_image(flashed_elf))
    forged[
        _ELF_SHA256_APP_IMAGE_OFFSET:
        _ELF_SHA256_APP_IMAGE_OFFSET + 32
    ] = hashlib.sha256(detached_elf).digest()
    checksum = 0xEF
    position = 24
    for _index in range(forged[1]):
        size = struct.unpack_from("<I", forged, position + 4)[0]
        position += 8
        for value in forged[position:position + size]:
            checksum ^= value
        position += size
    forged[-33] = checksum
    forged[-32:] = hashlib.sha256(forged[:-32]).digest()

    errors = uplink_verify.verify_frozen_badge_uplink_attestation(
        _frozen(detached_elf, bytes(forged)),
        label="release",
        expected_rtc_size=0x14,
    )

    assert any("ELF load image bytes" in error for error in errors), errors


@pytest.mark.parametrize(
    ("entrypoint", "expected_fragment"),
    (
        (0x1000, "valid ESP32-S3 executable"),
        (_APPDESC_ADDRESS, "valid ESP32-S3 executable"),
    ),
)
def test_frozen_uplink_attestation_rejects_self_consistent_bad_entrypoint(
    entrypoint: int,
    expected_fragment: str,
) -> None:
    elf = bytearray(_elf32(rtc_size=0x14))
    struct.pack_into("<I", elf, 24, entrypoint)
    frozen_elf = bytes(elf)

    errors = uplink_verify.verify_frozen_badge_uplink_attestation(
        _frozen(frozen_elf, _app_image(frozen_elf)),
        label="release",
        expected_rtc_size=0x14,
    )

    assert any(expected_fragment in error for error in errors), errors


@pytest.mark.parametrize(
    ("offset", "value", "expected_fragment"),
    (
        (9, 1, "reserved"),
        (14, 1, "revision"),
        (15, 1, "revision"),
        (19, 1, "reserved"),
    ),
)
def test_frozen_uplink_attestation_rejects_extended_header_drift(
    offset: int,
    value: int,
    expected_fragment: str,
) -> None:
    elf = _elf32(rtc_size=0x14)
    image = bytearray(_app_image(elf))
    image[offset] = value

    errors = uplink_verify.verify_frozen_badge_uplink_attestation(
        _frozen(elf, _reseal_app_image(bytes(image))),
        label="release",
        expected_rtc_size=0x14,
    )

    assert any(expected_fragment in error for error in errors), errors


def test_frozen_uplink_attestation_rejects_pt_load_alignment_drift() -> None:
    elf = bytearray(_elf32(rtc_size=0x14))
    first_program_header = struct.unpack_from("<I", elf, 28)[0]
    file_offset = struct.unpack_from("<I", elf, first_program_header + 4)[0]
    struct.pack_into("<I", elf, first_program_header + 4, file_offset + 1)
    frozen_elf = bytes(elf)

    errors = uplink_verify.verify_frozen_badge_uplink_attestation(
        _frozen(frozen_elf, _app_image(frozen_elf)),
        label="release",
        expected_rtc_size=0x14,
    )

    assert any("p_offset/p_vaddr alignment" in error for error in errors), errors


def test_frozen_uplink_attestation_rejects_mapped_flash_congruence_drift(
) -> None:
    elf = _elf32(rtc_size=0x14)
    image = bytearray(_app_image(elf))
    struct.pack_into("<I", image, 24, _APPDESC_ADDRESS + 4)

    errors = uplink_verify.verify_frozen_badge_uplink_attestation(
        _frozen(elf, _reseal_app_image(bytes(image))),
        label="release",
        expected_rtc_size=0x14,
    )

    assert any("mapped flash offset mismatch" in error for error in errors)


def test_frozen_uplink_attestation_rejects_shared_pt_load_file_range() -> None:
    elf = bytearray(_elf32(rtc_size=0x14))
    program_offset = struct.unpack_from("<I", elf, 28)[0]
    first = struct.unpack_from("<IIIIIIII", elf, program_offset)
    third_offset = program_offset + 2 * 32
    third = list(struct.unpack_from("<IIIIIIII", elf, third_offset))
    third[1] = first[1]
    third[4] = 4
    third[5] = 0x14
    third[7] = 4
    struct.pack_into("<IIIIIIII", elf, third_offset, *third)
    frozen_elf = bytes(elf)

    errors = uplink_verify.verify_frozen_badge_uplink_attestation(
        _frozen(frozen_elf, _app_image(frozen_elf)),
        label="release",
        expected_rtc_size=0x14,
    )

    assert any("overlapping ELF PT_LOAD file ranges" in error for error in errors), errors


@pytest.mark.parametrize("duplicate_file_offset", (0, 4))
def test_frozen_uplink_attestation_rejects_duplicate_zero_file_rtc_load(
    duplicate_file_offset: int,
) -> None:
    elf = _elf32(
        rtc_size=0x14,
        extra_program_headers=((
            1,
            duplicate_file_offset,
            _RTC_ADDRESS,
            _RTC_ADDRESS,
            0,
            0x14,
            6,
            4,
        ),),
    )

    errors = uplink_verify.verify_frozen_badge_uplink_attestation(
        _frozen(elf, _app_image(elf)),
        label="release",
        expected_rtc_size=0x14,
    )

    assert any(
        "zero-file PT_LOAD ownership is not bijective" in error
        for error in errors
    ), errors


def test_frozen_uplink_attestation_rejects_partial_zero_file_load_alias(
) -> None:
    elf = _elf32(
        rtc_size=0x14,
        extra_program_headers=((
            1,
            0,
            _RTC_ADDRESS + 4,
            _RTC_ADDRESS + 4,
            0,
            8,
            6,
            4,
        ),),
    )

    errors = uplink_verify.verify_frozen_badge_uplink_attestation(
        _frozen(elf, _app_image(elf)),
        label="release",
        expected_rtc_size=0x14,
    )

    assert any(
        "zero-file PT_LOAD must exactly match one allocatable SHT_NOBITS"
        in error
        for error in errors
    ), errors


def test_frozen_uplink_attestation_rejects_aliased_zero_file_section_owners(
) -> None:
    alias_address = 0x3FCF0000
    elf = _elf32(
        rtc_size=0x14,
        extra_program_headers=((
            1,
            0,
            alias_address,
            alias_address,
            0,
            4,
            6,
            4,
        ),),
        extra_sections=(
            (
                ".alias.one",
                8,
                0x3,
                alias_address,
                4,
                b"\x00" * 4,
            ),
            (
                ".alias.two",
                8,
                0x3,
                alias_address,
                4,
                b"\x00" * 4,
            ),
        ),
    )

    errors = uplink_verify.verify_frozen_badge_uplink_attestation(
        _frozen(elf, _app_image(elf)),
        label="release",
        expected_rtc_size=0x14,
    )

    assert any(
        "zero-file PT_LOAD ownership is not bijective" in error
        for error in errors
    ), errors


def test_frozen_uplink_attestation_allows_distinct_nested_zero_file_loads(
) -> None:
    outer_address = 0x3D000020
    elf = _elf32(
        rtc_size=0x14,
        extra_program_headers=(
            (
                1,
                0,
                outer_address,
                outer_address,
                0,
                0x40,
                6,
                4,
            ),
            (
                1,
                0,
                outer_address,
                outer_address,
                0,
                0x20,
                6,
                4,
            ),
        ),
        extra_sections=(
            (
                ".nested.outer",
                8,
                0x3,
                outer_address,
                4,
                b"\x00" * 0x40,
            ),
            (
                ".nested.inner",
                8,
                0x3,
                outer_address,
                4,
                b"\x00" * 0x20,
            ),
        ),
    )

    assert uplink_verify.verify_frozen_badge_uplink_attestation(
        _frozen(elf, _app_image(elf)),
        label="release",
        expected_rtc_size=0x14,
    ) == []


def test_frozen_uplink_attestation_rejects_hidden_pt_load_gap_bytes() -> None:
    elf = _elf32(
        rtc_size=0x14,
        appdesc_load_gap=b"\x5a\x5a\x5a\x5a",
    )

    errors = uplink_verify.verify_frozen_badge_uplink_attestation(
        _frozen(elf, _app_image(elf)),
        label="release",
        expected_rtc_size=0x14,
    )

    assert any("hidden file-backed bytes" in error for error in errors), errors


def test_frozen_uplink_attestation_rejects_unmapped_nobits_gap_claim() -> None:
    elf = _elf32(
        rtc_size=0x14,
        appdesc_load_gap=b"\x00\x00\x00\x00",
        extra_sections=((
            ".fake.nobits",
            8,
            0x2,
            _APPDESC_ADDRESS + 0x100,
            4,
            b"\x00\x00\x00\x00",
        ),),
    )

    errors = uplink_verify.verify_frozen_badge_uplink_attestation(
        _frozen(elf, _app_image(elf)),
        label="release",
        expected_rtc_size=0x14,
    )

    assert any("hidden file-backed bytes" in error for error in errors), errors


def test_frozen_uplink_attestation_rejects_extra_flashed_image_bytes() -> None:
    elf = _elf32(rtc_size=0x14)
    image = _append_app_image_segment(
        _app_image(elf),
        address=0x3FCA0000,
        data=b"\x00\x00\x00\x00",
    )

    errors = uplink_verify.verify_frozen_badge_uplink_attestation(
        _frozen(elf, image),
        label="release",
        expected_rtc_size=0x14,
    )

    assert any(
        "unique ELF PT_LOAD/section mapping" in error for error in errors
    ), errors


def test_frozen_uplink_attestation_rejects_image_segment_outside_s3() -> None:
    elf = _elf32(rtc_size=0x14)
    image = _append_app_image_segment(
        _app_image(elf),
        address=0x70000000,
        data=b"\x00\x00\x00\x00",
    )

    errors = uplink_verify.verify_frozen_badge_uplink_attestation(
        _frozen(elf, image),
        label="release",
        expected_rtc_size=0x14,
    )

    assert any("outside valid ESP32-S3 memory" in error for error in errors)


def test_frozen_uplink_attestation_rejects_unaccounted_zero_file_load() -> None:
    elf = _elf32(
        rtc_size=0x14,
        extra_program_headers=((
            1,
            0,
            0x3FCF0000,
            0x3FCF0000,
            0,
            4,
            6,
            4,
        ),),
    )

    errors = uplink_verify.verify_frozen_badge_uplink_attestation(
        _frozen(elf, _app_image(elf)),
        label="release",
        expected_rtc_size=0x14,
    )

    assert any(
        "memory range is not fully exposed" in error for error in errors
    ), errors


def test_elf_parser_rejects_excess_program_header_count() -> None:
    elf = bytearray(_elf32(rtc_size=0x14))
    struct.pack_into(
        "<H",
        elf,
        44,
        uplink_verify.UPLINK_MAX_ELF_PROGRAM_HEADERS + 1,
    )

    _entrypoint, _loads, errors = uplink_verify._read_elf32_load_evidence(
        bytes(elf),
        "release ELF",
    )

    assert any("program-header count exceeds" in error for error in errors)


def test_elf_parser_rejects_excess_section_count_before_iteration() -> None:
    elf = bytearray(_elf32(rtc_size=0x14))
    struct.pack_into(
        "<H",
        elf,
        48,
        uplink_verify.UPLINK_MAX_ELF_SECTIONS + 1,
    )

    _sections, _symbols, _sources, errors = (
        uplink_verify._read_elf32_evidence(bytes(elf), "release ELF")
    )

    assert any("section count exceeds" in error for error in errors)


def test_elf_parser_rejects_excess_symbol_count() -> None:
    elf = _elf32(
        rtc_size=0x14,
        extra_symbols=tuple(
            ("bounded_symbol", 0, 0, 0, 0, 0)
            for _index in range(uplink_verify.UPLINK_MAX_ELF_SYMBOLS)
        ),
    )

    _sections, _symbols, _sources, errors = (
        uplink_verify._read_elf32_evidence(elf, "release ELF")
    )

    assert any("symbol count exceeds" in error for error in errors)


def test_elf_parser_interns_repeated_large_symbol_name_once() -> None:
    repeated_name = "R" * uplink_verify.UPLINK_MAX_ELF_SYMBOL_NAME_BYTES
    repeat_count = 32
    elf = _elf32(
        rtc_size=0x14,
        extra_symbols=tuple(
            (repeated_name, 0, 0, 0, 0, 0)
            for _index in range(repeat_count)
        ),
    )

    _sections, symbols, _sources, errors = (
        uplink_verify._read_elf32_evidence(elf, "release ELF")
    )

    assert errors == []
    repeated = [
        symbol.name for symbol in symbols if symbol.name == repeated_name
    ]
    assert len(repeated) == repeat_count
    assert len({id(name) for name in repeated}) == 1


def test_elf_parser_rejects_repeated_large_name_reference_amplification(
) -> None:
    repeated_name = "R" * uplink_verify.UPLINK_MAX_ELF_SYMBOL_NAME_BYTES
    repeat_count = (
        uplink_verify.UPLINK_MAX_ELF_SYMBOL_NAME_REFERENCE_BYTES //
        len(repeated_name)
    ) + 2
    elf = _elf32(
        rtc_size=0x14,
        extra_symbols=tuple(
            (repeated_name, 0, 0, 0, 0, 0)
            for _index in range(repeat_count)
        ),
    )
    assert len(elf) < 128 * 1024

    _sections, _symbols, _sources, errors = (
        uplink_verify._read_elf32_evidence(elf, "release ELF")
    )

    assert any(
        "symbol-name reference bytes exceed" in error
        for error in errors
    ), errors


@pytest.mark.parametrize(
    ("name", "address", "size"),
    (
        (".rtc_shadow", _RTC_ADDRESS + 0x14, 4),
        (".anything", _RTC_ADDRESS + 0x100, 16),
        (".zero_sidecar", _RTC_ADDRESS + 0x200, 0),
        (".overlapping_sidecar", _RTC_ADDRESS - 4, 8),
    ),
)
def test_rtc_gate_rejects_renamed_or_disjoint_retained_side_section(
    name: str,
    address: int,
    size: int,
) -> None:
    elf = _elf32(
        rtc_size=0x14,
        extra_sections=((name, 8, 0x3, address, 4, b"\x00" * size),),
    )

    errors = uplink_verify._verify_uplink_rtc_elf_payload(
        elf,
        label="release ELF",
        expected_size=0x14,
    )

    assert any(
        "extra retained RTC slow-memory section" in error
        for error in errors
    ), errors


def test_rtc_gate_rejects_object_hidden_in_non_rtc_section() -> None:
    elf = _elf32(
        rtc_size=0x14,
        extra_symbols=((
            "hidden_side_block",
            _RTC_ADDRESS + 0x400,
            4,
            0x11,
            0,
            2,
        ),),
    )

    errors = uplink_verify._verify_uplink_rtc_elf_payload(
        elf,
        label="release ELF",
        expected_size=0x14,
    )

    assert any(
        "extra retained RTC slow-memory OBJECT" in error
        and "hidden_side_block" in error
        for error in errors
    ), errors


def test_rtc_gate_allows_normal_dram_nobits_section() -> None:
    elf = _elf32(
        rtc_size=0x14,
        extra_sections=((
            ".dram0.bss",
            8,
            0x3,
            0x3FCA0000,
            4,
            b"\x00" * 64,
        ),),
    )

    assert uplink_verify._verify_uplink_rtc_elf_payload(
        elf,
        label="release ELF",
        expected_size=0x14,
    ) == []


def test_rtc_gate_allows_only_fully_mapped_initialized_rtc_progbits() -> None:
    elf = _elf32(
        rtc_size=0x14,
        initialized_rtc=b"\x01\x00\x00\x00",
        extra_symbols=((
            "initialized_rtc_data",
            _RTC_ADDRESS + 0x14,
            4,
            0x11,
            0,
            4,
        ),),
    )

    assert uplink_verify.verify_frozen_badge_uplink_attestation(
        _frozen(elf, _app_image(elf)),
        label="release",
        expected_rtc_size=0x14,
    ) == []


def test_initialized_rtc_progbits_without_load_image_coverage_fails() -> None:
    elf = _elf32(
        rtc_size=0x14,
        extra_sections=((
            ".rtc.force_slow",
            1,
            0x3,
            _RTC_ADDRESS + 0x14,
            4,
            b"\x01\x00\x00\x00",
        ),),
    )

    errors = uplink_verify.verify_frozen_badge_uplink_attestation(
        _frozen(elf, _app_image(elf)),
        label="release",
        expected_rtc_size=0x14,
    )

    assert any(
        "not uniquely file-backed" in error or
        "missing from application segments" in error
        for error in errors
    ), errors


@pytest.mark.parametrize("section_type", (3, 7))
def test_rtc_gate_rejects_non_progbits_initialized_object(
    section_type: int,
) -> None:
    elf = _elf32(
        rtc_size=0x14,
        extra_sections=((
            ".rtc.disguised",
            section_type,
            0x3,
            _RTC_ADDRESS + 0x14,
            4,
            b"\x01\x00\x00\x00",
        ),),
        extra_symbols=((
            "disguised_rtc_object",
            _RTC_ADDRESS + 0x14,
            4,
            0x11,
            0,
            4,
        ),),
    )

    errors = uplink_verify._verify_uplink_rtc_elf_payload(
        elf,
        label="release ELF",
        expected_size=0x14,
    )

    assert any(
        "must be allocatable SHT_PROGBITS" in error
        for error in errors
    ), errors
