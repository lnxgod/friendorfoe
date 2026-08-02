"""Realistic ESP32-S3 app-image builders for backend firmware tests."""

import hashlib
import struct


ESP_IMAGE_HEADER_SIZE = 24
ESP_SEGMENT_HEADER_SIZE = 8
ESP_APP_DESC_OFFSET = ESP_IMAGE_HEADER_SIZE + ESP_SEGMENT_HEADER_SIZE


def esp32s3_app_image(
    version: str,
    *,
    project: str,
    placements: tuple[tuple[int, bytes], ...] = (),
    payload_size: int = 64 * 1024,
    payload_fill: bytes = b"\0",
) -> bytes:
    """Build one checksummed, SHA-256-appended ESP-IDF app image."""

    assert len(payload_fill) == 1
    assert payload_size >= 112
    payload = bytearray(payload_fill * payload_size)

    descriptor = bytearray(112)
    struct.pack_into("<I", descriptor, 0, 0xABCD5432)
    descriptor[16:48] = version.encode("ascii").ljust(32, b"\0")
    descriptor[48:80] = project.encode("ascii").ljust(32, b"\0")
    descriptor[80:96] = b"12:34:56".ljust(16, b"\0")
    descriptor[96:112] = b"Aug 02 2026".ljust(16, b"\0")
    payload[: len(descriptor)] = descriptor

    for absolute_offset, value in placements:
        payload_offset = absolute_offset - ESP_APP_DESC_OFFSET
        assert payload_offset >= 0
        assert payload_offset + len(value) <= len(payload)
        payload[payload_offset : payload_offset + len(value)] = value

    header = bytearray(ESP_IMAGE_HEADER_SIZE)
    header[0] = 0xE9
    header[1] = 2
    struct.pack_into("<I", header, 4, 0x40374000)
    struct.pack_into("<H", header, 12, 9)
    header[23] = 1
    executable_payload = b"\x36\x41\x00" + bytes(253)
    segments = (
        (0x3C000020, bytes(payload)),
        (0x40374000, executable_payload),
    )
    image = bytearray(header)

    checksum = 0xEF
    for load_address, segment_payload in segments:
        image.extend(struct.pack("<II", load_address, len(segment_payload)))
        image.extend(segment_payload)
        for value in segment_payload:
            checksum ^= value
    checksum_offset = ((len(image) + 16) // 16) * 16 - 1
    image.extend(bytes(checksum_offset - len(image)))
    image.append(checksum)
    image.extend(hashlib.sha256(image).digest())
    return bytes(image)


def resign_esp_image(image: bytes) -> bytes:
    """Recompute only the appended digest after a header/checksum mutation."""

    assert len(image) >= hashlib.sha256().digest_size
    prefix = image[:-hashlib.sha256().digest_size]
    return prefix + hashlib.sha256(prefix).digest()
