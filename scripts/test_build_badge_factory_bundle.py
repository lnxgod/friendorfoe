from __future__ import annotations

import dataclasses
import hashlib
import os
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

from scripts import build_badge_factory_bundle as builder


REPO_ROOT = Path(__file__).resolve().parents[1]


def minimal_app(version: str, project: str, payload: bytes = b"") -> bytes:
    image = bytearray(0x20 + 112)
    image[0] = 0xE9
    struct.pack_into("<I", image, 0x20, 0xABCD5432)
    image[0x30:0x50] = version.encode().ljust(32, b"\0")
    image[0x50:0x70] = project.encode().ljust(32, b"\0")
    return bytes(image) + payload


class BuilderTests(unittest.TestCase):
    def test_double_click_wrapper_executes_offline_before_user_arguments(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            fake_python = root / ".platformio/penv/bin/python"
            fake_python.parent.mkdir(parents=True)
            capture = root / "argv.txt"
            fake_python.write_text(
                "#!/bin/zsh\nprintf '%s\\n' \"$@\" > \"$FOF_TEST_ARGV\"\n",
                encoding="utf-8",
            )
            fake_python.chmod(0o755)
            result = subprocess.run(
                [str(REPO_ROOT / "flash-badges.command"), "--plain", "--once"],
                cwd=REPO_ROOT,
                env={
                    **os.environ,
                    "HOME": str(root),
                    "FOF_TEST_ARGV": str(capture),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(
                capture.read_text(encoding="utf-8").splitlines(),
                [
                    "scripts/fof_badge_factory.py",
                    "--offline",
                    "--plain",
                    "--once",
                ],
            )

    def test_accepted_pin_rejects_wrong_size_hash_and_version(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            app = Path(temp) / "firmware.bin"
            data = minimal_app("0.67.2-badge-defcon34", "fof_badge_uplink")
            app.write_bytes(data)
            pin = builder.AcceptedApplication(
                version="0.67.2-badge-defcon34",
                size=len(data),
                sha256=hashlib.sha256(data).hexdigest(),
            )
            builder.verify_accepted_application(app, "fof_badge_uplink", pin)
            with self.assertRaisesRegex(RuntimeError, "size"):
                builder.verify_accepted_application(
                    app, "fof_badge_uplink", dataclasses.replace(pin, size=len(data) + 1)
                )
            with self.assertRaisesRegex(RuntimeError, "SHA-256"):
                builder.verify_accepted_application(
                    app, "fof_badge_uplink", dataclasses.replace(pin, sha256="0" * 64)
                )
            app.write_bytes(minimal_app("0.67.1-badge-defcon34", "fof_badge_uplink"))
            with self.assertRaisesRegex(RuntimeError, "version"):
                builder.verify_accepted_application(app, "fof_badge_uplink", pin)

    def test_final_profile_names_exact_accepted_outputs(self) -> None:
        profile = builder.BUILD_PROFILES["con-crud-0.67.2"]
        self.assertEqual(profile["uplink"][0].name, "uplink-s3-fof_badge-con-crud-canary")
        self.assertEqual(profile["scanner"][0].name, "scanner-s3-combo-fof_badge-con-crud-canary")
        self.assertEqual(
            builder.ACCEPTED_APPLICATIONS["uplink"].sha256,
            "78ef3b6dafe61e8e2fdc3fb28447372aaf76da38cd57ca0961828bbbdc08c434",
        )
        self.assertEqual(
            builder.ACCEPTED_APPLICATIONS["scanner"].sha256,
            "2d0e84501baf3bc929eed03a0b9c1f0272ed66baa9b81dd4513d6dc3fa2c032b",
        )


if __name__ == "__main__":
    unittest.main()
