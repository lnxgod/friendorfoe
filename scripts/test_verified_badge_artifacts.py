from __future__ import annotations

import json
import os
import runpy
import stat
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[1]
ESP32_SCRIPTS = REPO_ROOT / "esp32" / "scripts"
if str(ESP32_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(ESP32_SCRIPTS))

import secure_artifact_tree as artifacts
import verified_badge_artifacts as verified
import verify_badge_scanner_build as scanner_verify
import verify_badge_uplink_build as uplink_verify
from verify_badge_factory_probe_build import (
    prepare_verified_factory_probe_snapshot,
)


def _partition_bytes(app_offset: int, *, ota: bool) -> bytes:
    label = b"ota_0" if ota else b"factory"
    subtype = 0x10 if ota else 0
    return struct.pack(
        "<HBBII16sI",
        0x50AA,
        0,
        subtype,
        app_offset,
        0x200000,
        label.ljust(16, b"\x00"),
        0,
    )


class VerifiedBadgeArtifactTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(
            prefix=".fof-verified-badge-artifacts-",
            dir=REPO_ROOT,
        )
        self.base = Path(self.temporary.name)
        self.private_parent = self.base / "private"
        self.private_parent.mkdir(mode=0o700)
        os.chmod(self.private_parent, 0o700)
        self.generator = self.base / "gen_esp32part.py"
        self.generator.write_text(
            "from pathlib import Path\n"
            "import struct, sys\n"
            "lines = Path(sys.argv[-2]).read_text().splitlines()\n"
            "row = next(line for line in lines if line and not "
            "line.lstrip().startswith('#'))\n"
            "fields = [field.strip() for field in row.split(',')]\n"
            "label = fields[0]\n"
            "offset = int(fields[3], 0)\n"
            "subtype = 0x10 if label == 'ota_0' else 0\n"
            "encoded = label.encode().ljust(16, b'\\x00')\n"
            "assert sys.argv[-1].startswith('/dev/fd/')\n"
            "Path(sys.argv[-1]).write_bytes(struct.pack(\n"
            "    '<HBBII16sI', 0x50AA, 0, subtype, offset,\n"
            "    0x200000, encoded, 0))\n",
            encoding="utf-8",
        )
        os.chmod(self.generator, 0o644)
        self.generator_patch = mock.patch.dict(
            os.environ,
            {"ESP_IDF_PARTITION_GENERATOR": str(self.generator)},
            clear=False,
        )
        self.generator_patch.start()

    def tearDown(self) -> None:
        self.generator_patch.stop()
        self.temporary.cleanup()

    def assert_private_parent_empty(self) -> None:
        self.assertEqual(list(self.private_parent.iterdir()), [])

    def write_game_acceptance(
        self,
        **updates: object,
    ) -> Path:
        payload: dict[str, object] = {
            "schema_version": 1,
            "candidate_version": "0.64.90-badge-defcon34",
            "artifacts": {
                "scanner": {
                    "sha256": "a" * 64,
                    "image_bytes": 1_216_784,
                    "max_image_bytes": 1_363_148,
                    "internal_ram_bytes": 158_612,
                    "max_internal_ram_bytes": 180_224,
                },
                "uplink": {
                    "sha256": "b" * 64,
                    "image_bytes": 1_467_984,
                    "max_image_bytes": 1_468_006,
                    "internal_ram_bytes": 209_100,
                    "max_internal_ram_bytes": 212_992,
                },
            },
            "physically_accepted": False,
            "physical_evidence": [],
        }
        payload.update(updates)
        path = self.base / "acceptance.json"
        path.write_text(json.dumps(payload), encoding="utf-8")
        return path

    def test_private_game_acceptance_loads_exact_blocked_candidate(self) -> None:
        acceptance = verified.load_private_game_acceptance(
            self.write_game_acceptance(),
            expected_version="0.64.90-badge-defcon34",
        )

        self.assertFalse(acceptance.physically_accepted)
        self.assertEqual(acceptance.scanner.sha256, "a" * 64)
        self.assertEqual(acceptance.uplink.image_bytes, 1_467_984)

    def test_private_game_acceptance_rejects_hash_version_and_false_proof(
        self,
    ) -> None:
        path = self.write_game_acceptance(
            candidate_version="0.64.92-badge-defcon34",
        )
        with self.assertRaisesRegex(
            artifacts.SecureArtifactError, "candidate version"
        ):
            verified.load_private_game_acceptance(
                path,
                expected_version="0.64.90-badge-defcon34",
            )

        payload = json.loads(path.read_text(encoding="utf-8"))
        payload["candidate_version"] = "0.64.90-badge-defcon34"
        payload["artifacts"]["scanner"]["sha256"] = "A" * 64
        path.write_text(json.dumps(payload), encoding="utf-8")
        with self.assertRaisesRegex(
            artifacts.SecureArtifactError, "scanner sha256"
        ):
            verified.load_private_game_acceptance(
                path,
                expected_version="0.64.90-badge-defcon34",
            )

        payload["artifacts"]["scanner"]["sha256"] = "a" * 64
        payload["physically_accepted"] = True
        path.write_text(json.dumps(payload), encoding="utf-8")
        with self.assertRaisesRegex(
            artifacts.SecureArtifactError, "physical evidence"
        ):
            verified.load_private_game_acceptance(
                path,
                expected_version="0.64.90-badge-defcon34",
            )

    def write_role(self, role: str) -> tuple[Path, Path, Path]:
        if role == "scanner":
            app_name = "fof_badge_scanner.bin"
            app_offset = 0x20000
            ota = True
            manifests = (
                "flash_args",
                "app-flash_args",
                "flash_app_args",
                "flash_project_args",
            )
            environment = "scanner-s3-combo-fof_badge"
            partition_filename = "partitions_s3_scanner_8mb.csv"
        elif role == "uplink":
            app_name = "fof_badge_uplink.bin"
            app_offset = 0x20000
            ota = True
            manifests = (
                "flash_args",
                "flash_app_args",
                "flash_project_args",
            )
            environment = "uplink-s3-fof_badge"
            partition_filename = "partitions_s3_fof_badge_8mb.csv"
        elif role == "probe":
            app_name = "fof_badge_factory_probe.bin"
            app_offset = 0x10000
            ota = False
            manifests = (
                "flash_args",
                "app-flash_args",
                "flash_app_args",
                "flash_project_args",
            )
            environment = "factory-probe-s3"
            partition_filename = "partitions.csv"
        else:
            raise AssertionError(role)

        project = self.base / role
        build = project / ".pio" / "build" / environment
        build.mkdir(parents=True)
        partition_source = project / partition_filename
        partition_source.write_text(
            (
                "ota_0,app,ota_0,0x20000,0x200000,\n"
                if ota
                else "factory,app,factory,0x10000,0x200000,\n"
            ),
            encoding="utf-8",
        )
        sdkconfig = project / f"sdkconfig.{environment}"
        sdkconfig.write_text(
            "CONFIG_PARTITION_TABLE_CUSTOM=y\n"
            + (
                'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME='
                '"partitions_s3_scanner_8mb.csv"\n'
                "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y\n"
                if role == "scanner"
                else "CONFIG_FOF_TEST_BUILD=y\n"
            ),
            encoding="utf-8",
        )
        (build / "bootloader.bin").write_bytes(b"bootloader")
        (build / "partitions.bin").write_bytes(
            _partition_bytes(app_offset, ota=ota)
        )
        (build / "firmware.bin").write_bytes(
            f"{role}-application".encode()
        )
        (build / "firmware.elf").write_bytes(
            f"{role}-elf".encode()
        )
        if ota:
            (build / "ota_data_initial.bin").write_bytes(b"otadata")

        full_entries = [
            "0x0 bootloader/bootloader.bin",
            "0x8000 partition_table/partition-table.bin",
        ]
        if ota:
            full_entries.append("0xf000 ota_data_initial.bin")
        full_entries.append(f"{app_offset:#x} {app_name}")
        full = (
            "--flash_mode dio --flash_freq 80m --flash_size 8MB\n"
            + "\n".join(full_entries)
            + "\n"
        )
        app = f"--flash_mode dio\n{app_offset:#x} {app_name}\n"
        for name in manifests:
            (build / name).write_text(
                app if name in {"app-flash_args", "flash_app_args"} else full,
                encoding="utf-8",
            )
        flash_files = {
            "0x0": "bootloader/bootloader.bin",
            "0x8000": "partition_table/partition-table.bin",
            f"{app_offset:#x}": app_name,
        }
        if ota:
            flash_files["0xf000"] = "ota_data_initial.bin"
        import json

        (build / "flasher_args.json").write_text(
            json.dumps(
                {
                    "flash_files": flash_files,
                    "bootloader": {
                        "offset": "0x0",
                        "file": "bootloader/bootloader.bin",
                    },
                    "app": {
                        "offset": f"{app_offset:#x}",
                        "file": app_name,
                    },
                    "partition-table": {
                        "offset": "0x8000",
                        "file": "partition_table/partition-table.bin",
                    },
                }
            ),
            encoding="utf-8",
        )
        return build, partition_source, sdkconfig

    def test_scanner_snapshot_contains_every_validated_input_and_distinct_alias(
        self,
    ) -> None:
        build, partition_source, sdkconfig = self.write_role("scanner")
        snapshot = scanner_verify.prepare_verified_badge_scanner_snapshot(
            build,
            partition_source,
            sdkconfig,
            private_parent=self.private_parent,
            materialize_missing_aliases=True,
        )
        try:
            expected_names = {
                "manifest.flash_args",
                "manifest.app-flash_args",
                "manifest.flash_app_args",
                "manifest.flash_project_args",
                "manifest.flasher_args.json",
                "artifact.bootloader",
                "artifact.partitions",
                "artifact.firmware",
                "artifact.ota_data_initial",
                "alias.bootloader",
                "alias.partitions",
                "alias.application",
                "partition.csv",
                "build.sdkconfig",
                "partition.generator",
                "partition.generated",
            }
            self.assertEqual(
                {member.logical_name for member in snapshot.files},
                expected_names,
            )
            canonical = (build / "bootloader.bin").lstat()
            alias = (build / "bootloader/bootloader.bin").lstat()
            self.assertEqual(alias.st_nlink, 1)
            self.assertNotEqual(
                (canonical.st_dev, canonical.st_ino),
                (alias.st_dev, alias.st_ino),
            )
            frozen = snapshot.freeze_for_mutation()
            self.assertEqual(
                frozen.member_bytes("partition.generated"),
                frozen.member_bytes("artifact.partitions"),
            )
        finally:
            snapshot.close()
        self.assert_private_parent_empty()

    def test_snapshot_rejects_manifest_drift_before_freeze(self) -> None:
        build, partition_source, sdkconfig = self.write_role("uplink")
        snapshot = uplink_verify.prepare_verified_badge_uplink_snapshot(
            build,
            partition_source,
            sdkconfig,
            private_parent=self.private_parent,
            materialize_missing_aliases=True,
        )
        manifest = build / "flash_args"
        original = manifest.read_bytes()
        manifest.write_bytes(original.replace(b"0x20000", b"0x20001"))
        try:
            with self.assertRaises(artifacts.SecureArtifactError):
                snapshot.freeze_for_mutation()
        finally:
            snapshot.close()
        self.assert_private_parent_empty()

    def test_uplink_snapshot_freezes_exact_elf_in_aggregate(self) -> None:
        build, partition_source, sdkconfig = self.write_role("uplink")
        os.chmod(build / "firmware.elf", 0o755)
        expected_elf = (build / "firmware.elf").read_bytes()
        snapshot = uplink_verify.prepare_verified_badge_uplink_snapshot(
            build,
            partition_source,
            sdkconfig,
            private_parent=self.private_parent,
            materialize_missing_aliases=True,
        )
        try:
            frozen = snapshot.freeze_for_mutation()
        finally:
            snapshot.close()

        self.assertEqual(
            frozen.member_bytes("artifact.elf"),
            expected_elf,
        )
        self.assertIn(
            "artifact.elf",
            {member.logical_name for member in frozen.members},
        )
        first_aggregate = frozen.aggregate_sha256

        changed_elf = expected_elf + b"-next-build"
        (build / "firmware.elf").write_bytes(changed_elf)
        os.chmod(build / "firmware.elf", 0o755)
        snapshot = uplink_verify.prepare_verified_badge_uplink_snapshot(
            build,
            partition_source,
            sdkconfig,
            private_parent=self.private_parent,
            materialize_missing_aliases=False,
        )
        try:
            changed = snapshot.freeze_for_mutation()
        finally:
            snapshot.close()
        self.assertEqual(changed.member_bytes("artifact.elf"), changed_elf)
        self.assertNotEqual(changed.aggregate_sha256, first_aggregate)
        self.assert_private_parent_empty()

    def test_descriptor_rooted_role_authority_rejects_public_or_wrong_role(
        self,
    ) -> None:
        build, partition_source, sdkconfig = self.write_role("uplink")
        snapshot = uplink_verify.prepare_verified_badge_uplink_snapshot(
            build,
            partition_source,
            sdkconfig,
            private_parent=self.private_parent,
            materialize_missing_aliases=True,
        )
        try:
            frozen = snapshot.freeze_for_mutation()
        finally:
            snapshot.close()
        self.assertEqual(
            verified.verify_descriptor_rooted_frozen_role_authority(
                frozen,
                role=verified.UPLINK_ROLE,
                environment="uplink-s3-fof_badge",
            ),
            [],
        )

        public_copy = artifacts.FrozenArtifactSet(
            receipt_sha256=frozen.receipt_sha256,
            members=frozen.members,
            aggregate_sha256=frozen.aggregate_sha256,
        )
        public_errors = (
            verified.verify_descriptor_rooted_frozen_role_authority(
                public_copy,
                role=verified.UPLINK_ROLE,
                environment="uplink-s3-fof_badge",
            )
        )
        self.assertTrue(any(
            "lacks descriptor-rooted authority" in error
            for error in public_errors
        ))

        scanner_inputs = self.write_role("scanner")
        scanner = scanner_verify.prepare_verified_badge_scanner_snapshot(
            *scanner_inputs,
            private_parent=self.private_parent,
            materialize_missing_aliases=True,
        )
        try:
            wrong_role = scanner.freeze_for_mutation()
        finally:
            scanner.close()
        wrong_role_errors = (
            verified.verify_descriptor_rooted_frozen_role_authority(
                wrong_role,
                role=verified.UPLINK_ROLE,
                environment="uplink-s3-fof_badge",
            )
        )
        self.assertTrue(any(
            "exact required role inventory" in error
            for error in wrong_role_errors
        ))
        self.assert_private_parent_empty()

    def test_json_manifest_rejects_unregistered_nested_file_path(self) -> None:
        build, partition_source, sdkconfig = self.write_role("uplink")
        import json

        manifest = build / "flasher_args.json"
        payload = json.loads(manifest.read_text(encoding="utf-8"))
        payload["partition-table"]["file"] = "../outside.bin"
        manifest.write_text(json.dumps(payload), encoding="utf-8")

        with self.assertRaises(artifacts.SecureArtifactError):
            uplink_verify.prepare_verified_badge_uplink_snapshot(
                build,
                partition_source,
                sdkconfig,
                private_parent=self.private_parent,
                materialize_missing_aliases=True,
            )
        self.assert_private_parent_empty()

    def test_original_generator_replacement_never_executes_and_fails_snapshot(
        self,
    ) -> None:
        build, partition_source, sdkconfig = self.write_role("uplink")
        marker = self.base / "unverified-generator-executed"
        fired = False

        def replace_generator(stage: str, path: Path | None) -> None:
            nonlocal fired
            if stage != "before_partition_generator_exec" or fired:
                return
            fired = True
            assert path == self.generator
            self.generator.write_text(
                "from pathlib import Path\n"
                f"Path({str(marker)!r}).write_text('bad')\n"
                "Path(__import__('sys').argv[-1]).write_bytes(b'bad')\n",
                encoding="utf-8",
            )
            os.chmod(self.generator, 0o700)

        with mock.patch.object(
            verified,
            "_test_hook",
            side_effect=replace_generator,
        ), self.assertRaises(artifacts.SecureArtifactError):
            uplink_verify.prepare_verified_badge_uplink_snapshot(
                build,
                partition_source,
                sdkconfig,
                private_parent=self.private_parent,
                materialize_missing_aliases=True,
            )
        self.assertTrue(fired)
        self.assertFalse(marker.exists())
        self.assert_private_parent_empty()

    def test_same_content_generator_replacement_fails_source_identity_gate(
        self,
    ) -> None:
        build, partition_source, sdkconfig = self.write_role("uplink")
        original = self.generator.read_bytes()
        original_mode = stat.S_IMODE(self.generator.lstat().st_mode)
        fired = False

        def replace_generator(stage: str, path: Path | None) -> None:
            nonlocal fired
            if stage != "before_partition_generator_exec" or fired:
                return
            fired = True
            assert path == self.generator
            replacement = self.generator.with_name("replacement-generator.py")
            replacement.write_bytes(original)
            os.chmod(replacement, original_mode)
            os.replace(replacement, self.generator)

        with mock.patch.object(
            verified,
            "_test_hook",
            side_effect=replace_generator,
        ), self.assertRaises(artifacts.SecureArtifactError):
            uplink_verify.prepare_verified_badge_uplink_snapshot(
                build,
                partition_source,
                sdkconfig,
                private_parent=self.private_parent,
                materialize_missing_aliases=True,
            )
        self.assertTrue(fired)
        self.assert_private_parent_empty()

    def test_same_content_artifact_replacement_fails_final_identity_gate(
        self,
    ) -> None:
        build, partition_source, sdkconfig = self.write_role("uplink")
        firmware = build / "firmware.bin"
        original = firmware.read_bytes()
        original_mode = stat.S_IMODE(firmware.lstat().st_mode)
        fired = False

        def replace_firmware(stage: str, _path: Path | None) -> None:
            nonlocal fired
            if stage != "after_partition_generator_exec" or fired:
                return
            fired = True
            replacement = firmware.with_name("firmware-replacement.bin")
            replacement.write_bytes(original)
            os.chmod(replacement, original_mode)
            os.replace(replacement, firmware)

        with mock.patch.object(
            verified,
            "_test_hook",
            side_effect=replace_firmware,
        ), self.assertRaisesRegex(
            artifacts.SecureArtifactError,
            "source identity changed between validation snapshots",
        ):
            uplink_verify.prepare_verified_badge_uplink_snapshot(
                build,
                partition_source,
                sdkconfig,
                private_parent=self.private_parent,
                materialize_missing_aliases=True,
            )
        self.assertTrue(fired)
        self.assert_private_parent_empty()

    def test_factory_probe_snapshot_and_single_process_triple_freeze(self) -> None:
        scanner = self.write_role("scanner")
        uplink = self.write_role("uplink")
        probe = self.write_role("probe")
        snapshots = (
            scanner_verify.prepare_verified_badge_scanner_snapshot(
                *scanner,
                private_parent=self.private_parent,
                materialize_missing_aliases=True,
            ),
            uplink_verify.prepare_verified_badge_uplink_snapshot(
                *uplink,
                private_parent=self.private_parent,
                materialize_missing_aliases=True,
            ),
            prepare_verified_factory_probe_snapshot(
                *probe,
                private_parent=self.private_parent,
                materialize_missing_aliases=True,
            ),
        )
        try:
            frozen = verified.freeze_verified_badge_artifact_snapshots(
                scanner=snapshots[0],
                uplink=snapshots[1],
                probe=snapshots[2],
            )
            self.assertEqual(
                frozen.probe.member_bytes("partition.generated"),
                frozen.probe.member_bytes("artifact.partitions"),
            )
            self.assertTrue(
                frozen.probe.member_bytes("artifact.firmware").startswith(
                    b"probe-"
                )
            )
        finally:
            for snapshot in snapshots:
                snapshot.close()
        self.assert_private_parent_empty()

    def test_failed_triple_freeze_returns_no_partial_authority_and_cleans_all(
        self,
    ) -> None:
        scanner_inputs = self.write_role("scanner")
        uplink_inputs = self.write_role("uplink")
        probe_inputs = self.write_role("probe")
        snapshots = (
            scanner_verify.prepare_verified_badge_scanner_snapshot(
                *scanner_inputs,
                private_parent=self.private_parent,
                materialize_missing_aliases=True,
            ),
            uplink_verify.prepare_verified_badge_uplink_snapshot(
                *uplink_inputs,
                private_parent=self.private_parent,
                materialize_missing_aliases=True,
            ),
            prepare_verified_factory_probe_snapshot(
                *probe_inputs,
                private_parent=self.private_parent,
                materialize_missing_aliases=True,
            ),
        )
        probe_manifest = probe_inputs[0] / "flash_args"
        content = probe_manifest.read_bytes()
        probe_manifest.write_bytes(
            content.replace(b"0x10000", b"0x10001")
        )

        with self.assertRaises(artifacts.SecureArtifactError):
            verified.freeze_verified_badge_artifact_snapshots(
                scanner=snapshots[0],
                uplink=snapshots[1],
                probe=snapshots[2],
            )
        self.assertEqual(
            tuple(snapshot.state for snapshot in snapshots),
            ("CLOSED", "CLOSED", "CLOSED"),
        )
        self.assert_private_parent_empty()

    def test_snapshot_prepare_success_and_failure_do_not_leak_descriptors(
        self,
    ) -> None:
        descriptors_before = len(os.listdir("/dev/fd"))
        build, partition_source, sdkconfig = self.write_role("uplink")
        for _index in range(5):
            snapshot = uplink_verify.prepare_verified_badge_uplink_snapshot(
                build,
                partition_source,
                sdkconfig,
                private_parent=self.private_parent,
                materialize_missing_aliases=True,
            )
            snapshot.close()
            self.assert_private_parent_empty()
            self.assertEqual(len(os.listdir("/dev/fd")), descriptors_before)

    def test_pio_badge_verifiers_make_buildprog_always_run_for_cached_builds(
        self,
    ) -> None:
        class FakeEnvironment:
            def __init__(self, project: Path, environment: str) -> None:
                self.project = project
                self.environment = environment
                self.aliases: list[str] = []
                self.always_build: list[object] = []
                self.post_actions: list[tuple[object, object]] = []

            def subst(self, value: str) -> str:
                if value == "$PROJECT_DIR":
                    return str(self.project)
                if value == "$PIOENV":
                    return self.environment
                return value

            def Alias(self, target: str) -> tuple[str, str]:
                self.aliases.append(target)
                return ("alias", target)

            def AlwaysBuild(self, target: object) -> None:
                self.always_build.append(target)

            def AddPostAction(
                self,
                target: object,
                action: object,
            ) -> None:
                self.post_actions.append((target, action))

        cases = (
            (
                REPO_ROOT / "esp32/scripts/pio_verify_badge_scanner_build.py",
                REPO_ROOT / "esp32/scanner",
                "scanner-s3-combo-fof_badge",
            ),
            (
                REPO_ROOT / "esp32/scripts/pio_verify_badge_uplink_build.py",
                REPO_ROOT / "esp32/uplink",
                "uplink-s3-fof_badge",
            ),
        )
        for script, project, environment in cases:
            with self.subTest(script=script.name):
                fake = FakeEnvironment(project, environment)
                namespace = runpy.run_path(
                    str(script),
                    init_globals={
                        "Import": lambda *_names: None,
                        "env": fake,
                    },
                )
                self.assertEqual(len(fake.post_actions), 1)
                target, action = fake.post_actions[0]
                self.assertEqual(fake.aliases, ["buildprog"])
                self.assertEqual(target, ("alias", "buildprog"))
                self.assertEqual(
                    fake.always_build,
                    [("alias", "buildprog")],
                )
                self.assertIs(action, namespace["verify_after_badge_build"])
