import hashlib
from pathlib import Path

from scripts import package_public_badge_release as packager


REPO_ROOT = Path(__file__).resolve().parents[1]
ACCEPTED_BUNDLE = (
    REPO_ROOT
    / "tools"
    / "badge_flasher"
    / "resources"
    / "badge-factory-flasher-embedded.zip"
)


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def test_packages_exact_hardware_accepted_badge_release(tmp_path):
    version = packager.package_public_badge_release(
        ACCEPTED_BUNDLE,
        tmp_path,
        expected_version="0.67.2-badge-defcon34",
    )

    assert version == "0.67.2-badge-defcon34"
    assert {
        path.relative_to(tmp_path).as_posix()
        for path in tmp_path.rglob("*")
        if path.is_file()
    } == {
        "badge-scanner/bootloader.bin",
        "badge-scanner/partition-table.bin",
        "badge-scanner/ota-data-initial.bin",
        "badge-scanner/firmware.bin",
        "badge-uplink/bootloader.bin",
        "badge-uplink/partition-table.bin",
        "badge-uplink/ota-data-initial.bin",
        "badge-uplink/firmware.bin",
    }
    assert _sha256(tmp_path / "badge-scanner/firmware.bin") == (
        "2d0e84501baf3bc929eed03a0b9c1f0272ed66baa9b81dd4513d6dc3fa2c032b"
    )
    assert _sha256(tmp_path / "badge-uplink/firmware.bin") == (
        "78ef3b6dafe61e8e2fdc3fb28447372aaf76da38cd57ca0961828bbbdc08c434"
    )


def test_rejects_packaging_under_the_wrong_public_version(tmp_path):
    try:
        packager.package_public_badge_release(
            ACCEPTED_BUNDLE,
            tmp_path,
            expected_version="0.64.78-badge-defcon34",
        )
    except ValueError as exc:
        assert "expected 0.64.78-badge-defcon34" in str(exc)
    else:
        raise AssertionError("wrong public firmware version was accepted")
