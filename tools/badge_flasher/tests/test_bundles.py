from __future__ import annotations

import hashlib
import io
import json
import tempfile
import unittest
import struct
from pathlib import Path

from tools.badge_flasher import bundles
from tools.badge_flasher.bundles import BundleError, load_bundle, select_bundle


def make_bundle(root: Path, version: str = "0.64.76-badge-defcon34") -> Path:
    layouts = {}
    for role in ("probe", "uplink", "scanner"):
        parts = []
        offsets = (
            ((0, "bootloader.bin"), (0x8000, "partitions.bin"), (0x10000, "firmware.bin"))
            if role == "probe"
            else ((0, "bootloader.bin"), (0x8000, "partitions.bin"), (0xF000, "ota_data_initial.bin"), (0x20000, "firmware.bin"))
        )
        for offset, name in offsets:
            rel = f"{role}/{name}"
            path = root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            data = f"{role}:{offset}".encode()
            if name == "firmware.bin":
                project = {
                    "probe": "fof_badge_factory_probe",
                    "uplink": "fof_badge_uplink",
                    "scanner": "fof_badge_scanner",
                }[role]
                app_version = "1.0.0" if role == "probe" else version
                image = bytearray(0x20 + 112)
                image[0] = 0xE9
                struct.pack_into("<I", image, 0x20, 0xABCD5432)
                image[0x30:0x50] = app_version.encode().ljust(32, b"\0")
                image[0x50:0x70] = project.encode().ljust(32, b"\0")
                if role != "probe":
                    target = "uplink-s3-fof_badge" if role == "uplink" else "scanner-s3-combo-fof_badge"
                    image.extend(target.encode() + b"\0seeed_xiao_esp32s3\0")
                data = bytes(image)
            path.write_bytes(data)
            parts.append({"offset": offset, "path": rel, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
        identity = {"project": "fof_badge_factory_probe", "target": "factory-probe-s3", "version": "1.0.0"}
        if role == "uplink": identity = {"project": "fof_badge_uplink", "target": "uplink-s3-fof_badge", "version": version}
        if role == "scanner": identity = {"project": "fof_badge_scanner", "target": "scanner-s3-combo-fof_badge", "version": version}
        layouts[role] = {"chip": "ESP32-S3", "flash_size": "8MB", "identity": identity, "parts": parts}
    manifest = {"schema": 1, "version": version, "min_flasher": "1.0.0", "layouts": layouts}
    (root / "manifest.json").write_text(json.dumps(manifest, sort_keys=True, separators=(",", ":")))
    return root


class BundleTests(unittest.TestCase):
    def test_remote_download_is_stream_capped_and_atomic(self) -> None:
        class Response(io.BytesIO):
            headers = {}
            def __enter__(self): return self
            def __exit__(self, *_args): self.close()
        with tempfile.TemporaryDirectory() as temp:
            target = Path(temp) / "bundle.zip"
            old_limit = bundles.MAX_ARCHIVE_BYTES
            bundles.MAX_ARCHIVE_BYTES = 8
            try:
                with self.assertRaisesRegex(BundleError, "size limit"):
                    bundles._download_limited(
                        "https://example.invalid/bundle.zip",
                        target,
                        1,
                        opener=lambda *_args, **_kwargs: Response(b"123456789"),
                    )
            finally:
                bundles.MAX_ARCHIVE_BYTES = old_limit
            self.assertFalse(target.exists())
            self.assertFalse((Path(temp) / "bundle.zip.part").exists())

    def test_loads_complete_hashed_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            bundle = load_bundle(make_bundle(Path(temp)))
            self.assertEqual(bundle.version, "0.64.76-badge-defcon34")

    def test_rejects_corrupt_part(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = make_bundle(Path(temp))
            (root / "uplink/firmware.bin").write_bytes(b"corrupt")
            with self.assertRaisesRegex(BundleError, "size mismatch|digest mismatch"):
                load_bundle(root)

    def test_selects_only_strictly_newer_numeric_release(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            base = load_bundle(make_bundle(Path(temp) / "base", "0.64.76-badge-defcon34"))
            newer = load_bundle(make_bundle(Path(temp) / "new", "0.64.77-badge-defcon34"))
            same_core = load_bundle(make_bundle(Path(temp) / "same", "0.64.76-field"))
            self.assertIs(select_bundle(base, [same_core, newer]), newer)
            self.assertIs(select_bundle(base, [newer], offline=True), base)

    def test_rejects_manifest_that_lies_about_embedded_app(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = make_bundle(Path(temp))
            manifest = json.loads((root / "manifest.json").read_text())
            app = root / "uplink/firmware.bin"
            data = bytearray(app.read_bytes())
            data[0x50:0x70] = b"wrong_project".ljust(32, b"\0")
            app.write_bytes(data)
            part = next(p for p in manifest["layouts"]["uplink"]["parts"] if p["path"].endswith("firmware.bin"))
            part["size"] = len(data)
            part["sha256"] = hashlib.sha256(data).hexdigest()
            (root / "manifest.json").write_text(json.dumps(manifest, sort_keys=True, separators=(",", ":")))
            with self.assertRaisesRegex(BundleError, "embedded app identity"):
                load_bundle(root)

    def test_rejects_app_moved_to_wrong_offset_even_with_valid_hashes(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = make_bundle(Path(temp))
            manifest = json.loads((root / "manifest.json").read_text())
            app = next(
                part for part in manifest["layouts"]["uplink"]["parts"]
                if part["path"] == "uplink/firmware.bin"
            )
            app["offset"] = 0x30000
            dummy = root / "uplink/dummy.bin"
            dummy.write_bytes(b"dummy")
            manifest["layouts"]["uplink"]["parts"].append({
                "offset": 0x20000,
                "path": "uplink/dummy.bin",
                "size": 5,
                "sha256": hashlib.sha256(b"dummy").hexdigest(),
            })
            (root / "manifest.json").write_text(json.dumps(manifest, sort_keys=True, separators=(",", ":")))
            with self.assertRaisesRegex(BundleError, "exact safe partition mapping"):
                load_bundle(root)

    def test_rejects_overlapping_exact_regions(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = make_bundle(Path(temp))
            manifest = json.loads((root / "manifest.json").read_text())
            boot = root / "probe/bootloader.bin"
            data = b"x" * 0x9000
            boot.write_bytes(data)
            part = next(
                part for part in manifest["layouts"]["probe"]["parts"]
                if part["path"] == "probe/bootloader.bin"
            )
            part["size"] = len(data)
            part["sha256"] = hashlib.sha256(data).hexdigest()
            (root / "manifest.json").write_text(json.dumps(manifest, sort_keys=True, separators=(",", ":")))
            with self.assertRaisesRegex(BundleError, "overlap"):
                load_bundle(root)


if __name__ == "__main__":
    unittest.main()
