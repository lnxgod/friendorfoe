#!/usr/bin/env python3
"""Verify the standalone probe image, partition layout, and Web Serial manifest."""
import argparse
import json
import struct
from pathlib import Path
from firmware_version import parse_firmware_identity

EXPECTED_PARTS = [('bootloader.bin', 0), ('partition-table.bin', 0x8000), ('firmware.bin', 0x10000)]


def verify(firmware_dir, manifest):
    firmware_dir, manifest = Path(firmware_dir), Path(manifest)
    spec = json.loads(manifest.read_text())
    assert spec['version'] == '1.0.0', 'Incorrect probe firmware version'
    assert len(spec['builds']) == 1 and spec['builds'][0]['chipFamily'] == 'ESP32-S3'
    expected = [{'path': f'firmware/probe-scanner/{name}', 'offset': offset} for name, offset in EXPECTED_PARTS]
    assert spec['builds'][0]['parts'] == expected, 'Probe flash offsets/paths changed'
    for name, _ in EXPECTED_PARTS:
        assert (firmware_dir / name).is_file(), f'Missing {name}'
    image = (firmware_dir / 'firmware.bin').read_bytes()
    identity = parse_firmware_identity(image)
    assert identity and identity.project == 'fof_wifi_probe' and identity.version == spec['version'], 'Wrong firmware image'
    assert struct.unpack_from('<H', image, 12)[0] == 9, 'Image must target ESP32-S3'
    table = (firmware_dir / 'partition-table.bin').read_bytes()
    apps = []
    for i in range(0, len(table) - 31, 32):
        magic, kind, subtype, offset, size = struct.unpack_from('<HBBII', table, i)
        if magic == 0x50AA and kind == 0:
            apps.append((subtype, offset, size))
    assert apps == [(0, 0x10000, 0x300000)], 'Expected a single factory app in 4 MB flash'
    assert len(image) <= apps[0][2], 'Probe image exceeds its partition'
    assert (firmware_dir / 'bootloader.bin').read_bytes()[0] == 0xE9, 'Invalid bootloader'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware-dir', required=True)
    parser.add_argument('--manifest', default=str(Path(__file__).resolve().parents[1] / 'web-flasher/manifest-probe-scanner.json'))
    args = parser.parse_args()
    verify(args.firmware_dir, args.manifest)
    print('USB probe firmware and flash manifest verified')
