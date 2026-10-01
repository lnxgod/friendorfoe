"""Independent probe firmware cannot inherit badge/combo flash offsets."""
import json
from pathlib import Path
import struct
import sys
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'esp32/scripts'))
from verify_probe_firmware import verify


@pytest.fixture
def package(tmp_path):
    image = bytearray(256)
    image[0] = 0xE9
    struct.pack_into('<H', image, 12, 9)
    struct.pack_into('<I', image, 32, 0xABCD5432)
    image[48:53] = b'1.0.0'
    image[80:94] = b'fof_wifi_probe'
    (tmp_path / 'firmware.bin').write_bytes(image)
    (tmp_path / 'bootloader.bin').write_bytes(b'\xe9' + bytes(255))
    table = bytearray(32)
    struct.pack_into('<HBBII', table, 0, 0x50AA, 0, 0, 0x10000, 0x300000)
    (tmp_path / 'partition-table.bin').write_bytes(table)
    manifest = tmp_path / 'manifest.json'
    manifest.write_text((ROOT / 'esp32/web-flasher/manifest-probe-scanner.json').read_text())
    return tmp_path, manifest


def test_probe_package_identity_and_offsets(package):
    verify(*package)


@pytest.mark.parametrize('filename', ['bootloader.bin', 'firmware.bin', 'partition-table.bin'])
def test_missing_probe_binary_is_rejected(package, filename):
    (package[0] / filename).unlink()
    with pytest.raises(AssertionError):
        verify(*package)


def test_rejects_combo_scanner_offset(package):
    manifest = json.loads(package[1].read_text())
    manifest['builds'][0]['parts'][2]['offset'] = 0x20000
    package[1].write_text(json.dumps(manifest))
    with pytest.raises(AssertionError):
        verify(*package)


def test_rejects_another_app(package):
    file = package[0] / 'firmware.bin'
    image = bytearray(file.read_bytes()); image[80:94] = b'other_firmware'
    file.write_bytes(image)
    with pytest.raises(AssertionError):
        verify(*package)


def test_probe_has_ci_build_packaging_and_pages_manifest():
    workflow = (ROOT / '.github/workflows/esp32-web-flasher.yml').read_text()
    assert 'group: esp32-web-flasher-${{ github.ref }}' in workflow
    assert 'cd esp32/probe-scanner && pio run -e probe-scanner-s3' in workflow
    assert 'esp32/web-flasher/manifest-probe-scanner.json _site/' in workflow
    assert 'verify_probe_firmware.py --firmware-dir _site/firmware/probe-scanner' in workflow
    assert 'manifest="manifest-probe-scanner.json"' in (ROOT / 'esp32/web-flasher/index.html').read_text()
