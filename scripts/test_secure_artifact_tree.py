from __future__ import annotations

import hashlib
import io
import json
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[1]
ESP32_SCRIPTS = REPO_ROOT / "esp32" / "scripts"
if str(ESP32_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(ESP32_SCRIPTS))

try:
    import secure_artifact_tree as artifacts
except ModuleNotFoundError:
    artifacts = None


def _expected_aggregate(
    receipt_sha256: str,
    members: tuple[object, ...],
) -> str:
    digest = hashlib.sha256()
    digest.update(b"FOF-FROZEN-ARTIFACT-SET-v1\x00")
    digest.update(bytes.fromhex(receipt_sha256))
    digest.update(len(members).to_bytes(4, "big"))
    for member in members:
        encoded_name = member.logical_name.encode("utf-8")
        digest.update(len(encoded_name).to_bytes(4, "big"))
        digest.update(encoded_name)
        digest.update(member.size.to_bytes(8, "big"))
        digest.update(bytes.fromhex(member.sha256))
        digest.update(member.content)
    return digest.hexdigest()


class SecureArtifactTreeTests(unittest.TestCase):
    def setUp(self) -> None:
        if artifacts is None:
            self.fail(
                "secure_artifact_tree is not implemented; "
                "the Phase B artifact-core test is intentionally RED"
            )
        self.temporary = tempfile.TemporaryDirectory(
            prefix=".fof-secure-artifact-test-",
            dir=REPO_ROOT,
        )
        self.base = Path(self.temporary.name)
        self.source_root = self.base / "source"
        self.private_parent = self.base / "private"
        self.source_root.mkdir(mode=0o700)
        self.private_parent.mkdir(mode=0o700)
        os.chmod(self.source_root, 0o700)
        os.chmod(self.private_parent, 0o700)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_source(
        self,
        relative: str,
        data: bytes,
        *,
        mode: int = 0o600,
    ) -> Path:
        path = self.source_root.joinpath(*relative.split("/"))
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        os.chmod(path, mode)
        return path

    def spec(
        self,
        logical_name: str,
        relative: str,
        *,
        max_size: int | None = None,
        allowed_modes: tuple[int, ...] = (0o600,),
    ):
        kwargs = {
            "logical_name": logical_name,
            "relative": relative,
            "allowed_modes": allowed_modes,
        }
        if max_size is not None:
            kwargs["max_size"] = max_size
        return artifacts.SnapshotFileSpec(**kwargs)

    def prepare(self, *specs):
        tree = artifacts.SecureArtifactTree.open(self.source_root)
        try:
            return tree.prepare_snapshot(
                specs,
                private_parent=self.private_parent,
            )
        finally:
            tree.close()

    def assert_private_parent_empty(self) -> None:
        self.assertEqual(list(self.private_parent.iterdir()), [])

    def test_descriptor_walk_rejects_symlinks_and_unsafe_relative_parts(
        self,
    ) -> None:
        nested = self.source_root / "nested"
        nested.mkdir()
        root_link = self.base / "source-link"
        root_link.symlink_to(self.source_root, target_is_directory=True)
        component_link = self.source_root / "component-link"
        component_link.symlink_to(nested, target_is_directory=True)

        with self.assertRaises(artifacts.SecureArtifactError):
            artifacts.SecureArtifactTree.open(root_link)

        tree = artifacts.SecureArtifactTree.open(self.source_root)
        try:
            for unsafe in (
                "",
                ".",
                "..",
                "../outside.bin",
                "nested/../outside.bin",
                "/absolute.bin",
                "component-link/member.bin",
            ):
                with self.subTest(relative=unsafe), self.assertRaises(
                    artifacts.SecureArtifactError
                ):
                    tree.prepare_snapshot(
                        (self.spec("unsafe", unsafe),),
                        private_parent=self.private_parent,
                    )
                self.assert_private_parent_empty()
        finally:
            tree.close()

    def test_snapshot_rejects_nonregular_mode_hardlink_duplicate_and_bounds(
        self,
    ) -> None:
        regular = self.write_source("regular.bin", b"regular")
        hardlink = self.source_root / "hardlink.bin"
        os.link(regular, hardlink)
        symlink = self.source_root / "symlink.bin"
        symlink.symlink_to(regular)
        fifo = self.source_root / "fifo"
        os.mkfifo(fifo, 0o600)
        wrong_mode = self.write_source("wrong-mode.bin", b"mode", mode=0o644)
        oversized = self.write_source("oversized.bin", b"12345")

        cases = (
            (self.spec("fifo", "fifo"),),
            (self.spec("symlink", "symlink.bin"),),
            (self.spec("hardlink", "regular.bin"),),
            (
                self.spec("one", "regular.bin"),
                self.spec("two", "hardlink.bin"),
            ),
            (self.spec("mode", wrong_mode.name),),
            (self.spec("size", oversized.name, max_size=4),),
        )
        for specs in cases:
            with self.subTest(specs=specs), self.assertRaises(
                artifacts.SecureArtifactError
            ):
                self.prepare(*specs)
            self.assert_private_parent_empty()

    def test_source_copy_requires_equal_a_b_c_and_two_equal_reads(self) -> None:
        for stage in ("source_after_copy", "source_before_second_read"):
            with self.subTest(stage=stage):
                source = self.write_source("member.bin", b"ORIGINAL")
                sentinel = self.base / f"outside-{stage}.bin"
                sentinel.write_bytes(b"outside")
                fired = False

                def mutate(
                    observed_stage: str,
                    logical_name: str | None,
                ) -> None:
                    nonlocal fired
                    if (
                        not fired
                        and observed_stage == stage
                        and logical_name == "member"
                    ):
                        fired = True
                        source.write_bytes(b"MUTATED!")
                        os.chmod(source, 0o600)

                with mock.patch.object(
                    artifacts,
                    "_test_hook",
                    side_effect=mutate,
                ), self.assertRaises(artifacts.SecureArtifactError):
                    self.prepare(self.spec("member", "member.bin"))

                self.assertTrue(fired)
                self.assertEqual(sentinel.read_bytes(), b"outside")
                self.assert_private_parent_empty()
                source.write_bytes(b"ORIGINAL")
                os.chmod(source, 0o600)

        source = self.write_source(
            "revalidate/member.bin",
            b"REVALIDATE",
        )
        snapshot = self.prepare(
            self.spec("revalidate", "revalidate/member.bin")
        )
        real_read_exact = artifacts._read_exact_bytes
        rebound_during_read = False

        def rebind_source_during_read(
            fd: int,
            expected_size: int,
        ) -> bytes:
            nonlocal rebound_during_read
            if not rebound_during_read:
                rebound_during_read = True
                original_parent = source.parent
                original_parent.rename(
                    self.base / "revalidate-original-parent"
                )
                original_parent.mkdir()
                replacement = original_parent / source.name
                replacement.write_bytes(b"REVALIDATE")
                os.chmod(replacement, 0o600)
            return real_read_exact(fd, expected_size)

        try:
            with mock.patch.object(
                artifacts,
                "_read_exact_bytes",
                side_effect=rebind_source_during_read,
            ), self.assertRaises(artifacts.SecureArtifactError):
                snapshot.revalidate_sources()
            self.assertTrue(rebound_during_read)
        finally:
            snapshot.close()
        self.assert_private_parent_empty()

    def test_private_snapshot_has_canonical_fsynced_receipt_and_permissions(
        self,
    ) -> None:
        self.write_source("z.bin", b"z")
        self.write_source("nested/a.bin", b"alpha")
        fsynced: list[int] = []
        real_fsync = os.fsync

        def recording_fsync(fd: int) -> None:
            fsynced.append(fd)
            real_fsync(fd)

        with mock.patch.object(
            artifacts.os,
            "fsync",
            side_effect=recording_fsync,
        ):
            snapshot = self.prepare(
                self.spec("zeta", "z.bin"),
                self.spec("alpha", "nested/a.bin"),
            )
        try:
            self.assertEqual(snapshot.state, "OPEN")
            self.assertEqual(
                [item.logical_name for item in snapshot.files],
                ["alpha", "zeta"],
            )
            self.assertEqual(
                stat.S_IMODE(snapshot.private_root.lstat().st_mode),
                0o700,
            )
            self.assertGreaterEqual(len(fsynced), 5)
            receipt_path = snapshot.private_root / "receipt.json"
            receipt_info = receipt_path.lstat()
            self.assertTrue(stat.S_ISREG(receipt_info.st_mode))
            self.assertEqual(stat.S_IMODE(receipt_info.st_mode), 0o600)
            self.assertEqual(receipt_info.st_nlink, 1)
            receipt_bytes = receipt_path.read_bytes()
            canonical = (
                json.dumps(
                    json.loads(receipt_bytes),
                    sort_keys=True,
                    separators=(",", ":"),
                ).encode("utf-8")
                + b"\n"
            )
            self.assertEqual(receipt_bytes, canonical)
            self.assertEqual(
                snapshot.receipt_sha256,
                hashlib.sha256(receipt_bytes).hexdigest(),
            )
            first_private_names = tuple(
                item.private_relative for item in snapshot.files
            )
            for item in snapshot.files:
                path = snapshot.private_root / item.private_relative
                info = path.lstat()
                self.assertTrue(stat.S_ISREG(info.st_mode))
                self.assertEqual(stat.S_IMODE(info.st_mode), 0o600)
                self.assertEqual(info.st_nlink, 1)
                self.assertEqual(
                    hashlib.sha256(path.read_bytes()).hexdigest(),
                    item.private.sha256,
                )
            snapshot.revalidate_sources()
            snapshot.revalidate_retained_files()

            real_listdir = os.listdir
            rebound_receipt = False
            inventory_reads = 0

            def rebind_receipt_before_final_inventory(
                path: object,
            ) -> list[str]:
                nonlocal inventory_reads, rebound_receipt
                inventory_reads += 1
                if inventory_reads == 2:
                    rebound_receipt = True
                    replacement = (
                        snapshot.private_root / "receipt-replacement"
                    )
                    replacement.write_bytes(receipt_bytes)
                    os.chmod(replacement, 0o600)
                    os.replace(replacement, receipt_path)
                return real_listdir(path)

            with mock.patch.object(
                artifacts.os,
                "listdir",
                side_effect=rebind_receipt_before_final_inventory,
            ), self.assertRaises(artifacts.SecureArtifactError):
                snapshot.revalidate_retained_files()
            self.assertTrue(rebound_receipt)
        finally:
            with self.assertRaisesRegex(
                artifacts.SecureArtifactError,
                "cleanup failed",
            ):
                snapshot.close()
            self.assertEqual(receipt_path.read_bytes(), receipt_bytes)
            receipt_path.unlink()
            snapshot.private_root.rmdir()
        self.assert_private_parent_empty()

        second_snapshot = self.prepare(
            self.spec("zeta", "z.bin"),
            self.spec("alpha", "nested/a.bin"),
        )
        try:
            self.assertNotEqual(
                tuple(
                    item.private_relative
                    for item in second_snapshot.files
                ),
                first_private_names,
            )
        finally:
            second_snapshot.close()
        self.assert_private_parent_empty()

        real_rename_noreplace = artifacts._rename_noreplace
        stolen_receipt = self.base / "stolen-receipt"
        swapped_receipt_temp = False

        def swap_receipt_temp(
            directory_fd: int,
            source_name: str,
            destination_name: str,
        ) -> None:
            nonlocal swapped_receipt_temp
            swapped_receipt_temp = True
            os.rename(
                source_name,
                stolen_receipt,
                src_dir_fd=directory_fd,
            )
            replacement_fd = os.open(
                source_name,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                0o600,
                dir_fd=directory_fd,
            )
            try:
                artifacts._write_all(
                    replacement_fd,
                    stolen_receipt.read_bytes(),
                )
                os.fchmod(replacement_fd, 0o600)
                os.fsync(replacement_fd)
            finally:
                os.close(replacement_fd)
            real_rename_noreplace(
                directory_fd,
                source_name,
                destination_name,
            )

        unexpected_snapshot = None
        try:
            with mock.patch.object(
                artifacts,
                "_rename_noreplace",
                side_effect=swap_receipt_temp,
            ), self.assertRaises(artifacts.SecureArtifactError):
                unexpected_snapshot = self.prepare(
                    self.spec("zeta", "z.bin"),
                    self.spec("alpha", "nested/a.bin"),
                )
        finally:
            if unexpected_snapshot is not None:
                unexpected_snapshot.close()
        self.assertTrue(swapped_receipt_temp)
        retained_roots = list(self.private_parent.iterdir())
        self.assertEqual(len(retained_roots), 1)
        retained_receipt = retained_roots[0] / "receipt.json"
        self.assertEqual(
            retained_receipt.read_bytes(),
            stolen_receipt.read_bytes(),
        )
        retained_receipt.unlink()
        retained_roots[0].rmdir()
        stolen_receipt.unlink()
        self.assert_private_parent_empty()

    def test_freeze_rejects_private_mutation_before_or_between_reads(
        self,
    ) -> None:
        for stage in ("freeze_before_first_read", "freeze_between_reads"):
            with self.subTest(stage=stage):
                self.write_source("member.bin", b"ORIGINAL")
                snapshot = self.prepare(
                    self.spec("member", "member.bin")
                )
                private_path = (
                    snapshot.private_root
                    / snapshot.files[0].private_relative
                )
                fired = False

                def mutate(
                    observed_stage: str,
                    logical_name: str | None,
                ) -> None:
                    nonlocal fired
                    if (
                        not fired
                        and observed_stage == stage
                        and logical_name == "member"
                    ):
                        fired = True
                        os.chmod(private_path, 0o600)
                        private_path.write_bytes(b"MUTATED!")
                        os.chmod(private_path, 0o600)

                with mock.patch.object(
                    artifacts,
                    "_test_hook",
                    side_effect=mutate,
                ), self.assertRaises(artifacts.SecureArtifactError):
                    snapshot.freeze_for_mutation()

                self.assertTrue(fired)
                self.assertEqual(snapshot.state, "CLOSED")
                self.assert_private_parent_empty()

    def test_freeze_rejects_path_replacement_after_revalidation(
        self,
    ) -> None:
        for target in ("source", "private"):
            with self.subTest(target=target):
                source = self.write_source(
                    f"{target}-member.bin",
                    b"ORIGINAL",
                )
                snapshot = self.prepare(
                    self.spec("member", f"{target}-member.bin")
                )
                private_path = (
                    snapshot.private_root
                    / snapshot.files[0].private_relative
                )
                fired = False

                def replace(
                    observed_stage: str,
                    logical_name: str | None,
                ) -> None:
                    nonlocal fired
                    if (
                        not fired
                        and observed_stage == "freeze_before_first_read"
                        and logical_name == "member"
                    ):
                        fired = True
                        if target == "source":
                            replacement = source.with_name(
                                f"{target}-replacement.bin"
                            )
                            replaced_path = source
                        else:
                            replacement = (
                                snapshot.private_root / "replacement.bin"
                            )
                            replaced_path = private_path
                        replacement.write_bytes(b"ORIGINAL")
                        os.chmod(replacement, 0o600)
                        os.replace(replacement, replaced_path)

                try:
                    with mock.patch.object(
                        artifacts,
                        "_test_hook",
                        side_effect=replace,
                    ), self.assertRaises(artifacts.SecureArtifactError):
                        snapshot.freeze_for_mutation()
                    self.assertTrue(fired)
                    self.assertEqual(snapshot.state, "CLOSED")
                finally:
                    snapshot.close()
                if target == "private":
                    self.assertEqual(private_path.read_bytes(), b"ORIGINAL")
                    private_path.unlink()
                    snapshot.private_root.rmdir()
                self.assert_private_parent_empty()

    def test_freeze_revalidates_receipt_and_private_root_after_member_reads(
        self,
    ) -> None:
        for target in ("receipt", "private_root"):
            with self.subTest(target=target):
                self.write_source(
                    f"{target}-member.bin",
                    b"ORIGINAL",
                )
                snapshot = self.prepare(
                    self.spec("member", f"{target}-member.bin")
                )
                original_root = snapshot.private_root
                receipt_path = original_root / "receipt.json"
                receipt_bytes = receipt_path.read_bytes()
                moved_root = original_root.with_name(
                    original_root.name + "-moved"
                )
                replacement_marker = original_root / "must-survive.bin"
                fired = False

                def replace_after_first_read(
                    observed_stage: str,
                    logical_name: str | None,
                ) -> None:
                    nonlocal fired
                    if (
                        not fired
                        and observed_stage == "freeze_between_reads"
                        and logical_name == "member"
                    ):
                        fired = True
                        if target == "receipt":
                            replacement = (
                                original_root / "receipt-replacement"
                            )
                            replacement.write_bytes(receipt_bytes)
                            os.chmod(replacement, 0o600)
                            os.replace(replacement, receipt_path)
                        else:
                            original_root.rename(moved_root)
                            original_root.mkdir(mode=0o700)
                            replacement_marker.write_bytes(b"replacement")
                            os.chmod(replacement_marker, 0o600)

                try:
                    with mock.patch.object(
                        artifacts,
                        "_test_hook",
                        side_effect=replace_after_first_read,
                    ), self.assertRaises(artifacts.SecureArtifactError):
                        snapshot.freeze_for_mutation()
                    self.assertTrue(fired)
                    self.assertEqual(snapshot.state, "CLOSED")
                    if target == "receipt":
                        self.assertEqual(
                            receipt_path.read_bytes(),
                            receipt_bytes,
                        )
                    else:
                        self.assertFalse(moved_root.exists())
                        self.assertEqual(
                            replacement_marker.read_bytes(),
                            b"replacement",
                        )
                finally:
                    try:
                        snapshot.close()
                    except artifacts.SecureArtifactError:
                        pass
                    if receipt_path.exists():
                        receipt_path.unlink()
                    if replacement_marker.exists():
                        replacement_marker.unlink()
                    if original_root.exists():
                        original_root.rmdir()
                    if moved_root.exists():
                        for path in moved_root.iterdir():
                            path.unlink()
                        moved_root.rmdir()
                self.assert_private_parent_empty()

    def test_freeze_is_all_or_nothing_when_later_member_drifts(self) -> None:
        self.write_source("a.bin", b"alpha")
        self.write_source("b.bin", b"bravo")
        snapshot = self.prepare(
            self.spec("alpha", "a.bin"),
            self.spec("bravo", "b.bin"),
        )
        private_by_name = {
            item.logical_name: (
                snapshot.private_root / item.private_relative
            )
            for item in snapshot.files
        }
        completed: list[str] = []

        def mutate_later(
            stage: str,
            logical_name: str | None,
        ) -> None:
            if stage == "freeze_after_member" and logical_name is not None:
                completed.append(logical_name)
            if (
                stage == "freeze_before_first_read"
                and logical_name == "bravo"
            ):
                path = private_by_name["bravo"]
                path.write_bytes(b"BRAVO")
                os.chmod(path, 0o600)

        result = None
        with mock.patch.object(
            artifacts,
            "_test_hook",
            side_effect=mutate_later,
        ), self.assertRaises(artifacts.SecureArtifactError):
            result = snapshot.freeze_for_mutation()

        self.assertIsNone(result)
        self.assertEqual(completed, ["alpha"])
        self.assertEqual(snapshot.state, "CLOSED")
        self.assert_private_parent_empty()

    def test_size_seek_short_and_extra_fail_without_a_capability(self) -> None:
        self.assertEqual(
            artifacts.MAX_ARTIFACT_MEMBER_BYTES,
            16 * 1024 * 1024,
        )
        self.assertEqual(
            artifacts.DEFAULT_ARTIFACT_MEMBER_BYTES,
            8 * 1024 * 1024,
        )
        self.assertEqual(
            self.spec("default", "default.bin").max_size,
            artifacts.DEFAULT_ARTIFACT_MEMBER_BYTES,
        )
        self.assertEqual(
            artifacts.MAX_ARTIFACT_SET_BYTES,
            32 * 1024 * 1024,
        )

        for mutation in ("short", "extra"):
            with self.subTest(mutation=mutation):
                source = self.write_source("member.bin", b"12345678")
                snapshot = self.prepare(
                    self.spec("member", "member.bin")
                )
                private_path = (
                    snapshot.private_root
                    / snapshot.files[0].private_relative
                )

                def change_size(
                    stage: str,
                    logical_name: str | None,
                ) -> None:
                    if (
                        stage == "freeze_before_first_read"
                        and logical_name == "member"
                    ):
                        if mutation == "short":
                            private_path.write_bytes(b"1234567")
                        else:
                            private_path.write_bytes(b"123456789")
                        os.chmod(private_path, 0o600)

                with mock.patch.object(
                    artifacts,
                    "_test_hook",
                    side_effect=change_size,
                ), self.assertRaises(artifacts.SecureArtifactError):
                    snapshot.freeze_for_mutation()
                self.assertEqual(snapshot.state, "CLOSED")
                self.assert_private_parent_empty()
                source.write_bytes(b"12345678")
                os.chmod(source, 0o600)

        self.write_source("one.bin", b"123456")
        self.write_source("two.bin", b"abcdef")
        with mock.patch.object(
            artifacts,
            "MAX_ARTIFACT_SET_BYTES",
            10,
        ), self.assertRaises(artifacts.SecureArtifactError):
            self.prepare(
                self.spec("one", "one.bin"),
                self.spec("two", "two.bin"),
            )
        self.assert_private_parent_empty()

        self.write_source("seek.bin", b"seek")
        snapshot = self.prepare(self.spec("seek", "seek.bin"))
        with mock.patch.object(
            artifacts.os,
            "lseek",
            side_effect=OSError("injected seek failure"),
        ), self.assertRaises(artifacts.SecureArtifactError):
            snapshot.freeze_for_mutation()
        self.assertEqual(snapshot.state, "CLOSED")
        self.assert_private_parent_empty()

    def test_freeze_orders_members_and_computes_domain_aggregate(self) -> None:
        self.write_source("z.bin", b"zulu")
        self.write_source("a.bin", b"alpha")
        snapshot = self.prepare(
            self.spec("zeta", "z.bin"),
            self.spec("alpha", "a.bin"),
        )
        frozen = snapshot.freeze_for_mutation()
        try:
            self.assertEqual(snapshot.state, "FROZEN")
            self.assertIsInstance(frozen, artifacts.FrozenArtifactSet)
            self.assertEqual(
                [item.logical_name for item in frozen.members],
                ["alpha", "zeta"],
            )
            self.assertEqual(
                frozen.aggregate_sha256,
                _expected_aggregate(
                    frozen.receipt_sha256,
                    frozen.members,
                ),
            )
            for member in frozen.members:
                self.assertIs(type(member.content), bytes)
                self.assertEqual(member.size, len(member.content))
                self.assertEqual(
                    member.sha256,
                    hashlib.sha256(member.content).hexdigest(),
                )
        finally:
            snapshot.close()
        self.assert_private_parent_empty()

    def test_frozen_views_are_fresh_seekable_read_only_and_pathless(
        self,
    ) -> None:
        self.write_source("member.bin", b"0123456789")
        snapshot = self.prepare(self.spec("member", "member.bin"))
        frozen = snapshot.freeze_for_mutation()
        try:
            first = frozen.open_readonly("member")
            second = frozen.open_readonly("member")
            self.assertIsNot(first, second)
            self.assertEqual(first.name, "<frozen:member>")
            self.assertEqual(first.read(3), b"012")
            self.assertEqual(first.tell(), 3)
            self.assertEqual(second.tell(), 0)
            self.assertEqual(second.seek(4), 4)
            target = bytearray(3)
            self.assertEqual(second.readinto(target), 3)
            self.assertEqual(bytes(target), b"456")
            self.assertEqual(first.seek(-2, os.SEEK_END), 8)
            self.assertEqual(first.read(), b"89")
            self.assertEqual(second.seek(100), 100)
            beyond_eof = bytearray(b"x")
            self.assertEqual(second.readinto(beyond_eof), 0)
            self.assertEqual(beyond_eof, bytearray(b"x"))
            self.assertEqual(second.tell(), 100)
            self.assertEqual(second.read(), b"")
            self.assertEqual(second.tell(), 100)
            self.assertEqual(
                frozen.member_bytes("member"),
                b"0123456789",
            )
            for operation in (
                lambda: first.write(b"x"),
                lambda: first.truncate(0),
                first.fileno,
            ):
                with self.assertRaises(io.UnsupportedOperation):
                    operation()
            for forbidden in ("path", "buffer", "raw", "getbuffer"):
                self.assertFalse(hasattr(first, forbidden), forbidden)
            first.close()
            with self.assertRaises(ValueError):
                first.read()
            second.close()
        finally:
            snapshot.close()

    def test_success_closes_member_fds_and_terminal_states_fail_closed(
        self,
    ) -> None:
        descriptors_before = len(os.listdir("/dev/fd"))
        self.write_source("member.bin", b"content")
        snapshot = self.prepare(self.spec("member", "member.bin"))
        closed: list[int] = []
        real_close = os.close

        def recording_close(fd: int) -> None:
            closed.append(fd)
            real_close(fd)

        with mock.patch.object(
            artifacts.os,
            "close",
            side_effect=recording_close,
        ):
            frozen = snapshot.freeze_for_mutation()
        self.assertEqual(frozen.member_bytes("member"), b"content")
        self.assertGreaterEqual(len(closed), 2)
        self.assertEqual(snapshot.state, "FROZEN")
        for operation in (
            snapshot.revalidate_sources,
            snapshot.revalidate_retained_files,
            snapshot.freeze_for_mutation,
        ):
            with self.assertRaises(artifacts.SecureArtifactError):
                operation()
        snapshot.close()
        self.assertEqual(snapshot.state, "CLOSED")
        snapshot.close()
        self.assert_private_parent_empty()
        self.assertEqual(len(os.listdir("/dev/fd")), descriptors_before)

    def test_post_freeze_source_and_private_mutation_cannot_change_bytes(
        self,
    ) -> None:
        source = self.write_source("member.bin", b"ORIGINAL")
        snapshot = self.prepare(self.spec("member", "member.bin"))
        private_path = (
            snapshot.private_root / snapshot.files[0].private_relative
        )
        frozen = snapshot.freeze_for_mutation()
        source.write_bytes(b"MUTATED!")
        os.chmod(source, 0o600)
        private_path.write_bytes(b"MUTATED!")
        os.chmod(private_path, 0o600)
        try:
            self.assertEqual(
                frozen.member_bytes("member"),
                b"ORIGINAL",
            )
            with frozen.open_readonly("member") as view:
                self.assertEqual(view.read(), b"ORIGINAL")
        finally:
            snapshot.close()
        self.assert_private_parent_empty()

    def test_cleanup_removes_bound_root_but_preserves_replacement_tree(
        self,
    ) -> None:
        source = self.write_source("member.bin", b"ORIGINAL")
        outside = self.base / "outside.bin"
        outside.write_bytes(b"outside")
        descriptors_before_failed_setup = len(os.listdir("/dev/fd"))

        real_fchmod = os.fchmod
        rejected_root_setup = False

        def reject_first_fchmod(fd: int, mode: int) -> None:
            nonlocal rejected_root_setup
            if not rejected_root_setup:
                rejected_root_setup = True
                raise OSError("injected private-root chmod failure")
            real_fchmod(fd, mode)

        with mock.patch.object(
            artifacts.os,
            "fchmod",
            side_effect=reject_first_fchmod,
        ), self.assertRaises(artifacts.SecureArtifactError):
            self.prepare(self.spec("member", "member.bin"))
        self.assertTrue(rejected_root_setup)
        self.assert_private_parent_empty()
        self.assertEqual(
            len(os.listdir("/dev/fd")),
            descriptors_before_failed_setup,
        )

        snapshot = self.prepare(self.spec("member", "member.bin"))
        original_root = snapshot.private_root
        moved_root = original_root.with_name(original_root.name + "-moved")
        original_root.rename(moved_root)
        original_root.mkdir(mode=0o700)
        replacement = original_root / "must-survive.bin"
        replacement.write_bytes(b"replacement")
        os.chmod(replacement, 0o600)

        snapshot.close()

        self.assertFalse(moved_root.exists())
        self.assertTrue(original_root.is_dir())
        self.assertEqual(replacement.read_bytes(), b"replacement")
        self.assertEqual(outside.read_bytes(), b"outside")
        self.assertEqual(source.read_bytes(), b"ORIGINAL")

    def test_cleanup_only_unlinks_owned_inodes_not_reserved_pathnames(
        self,
    ) -> None:
        self.write_source("member.bin", b"ORIGINAL")
        snapshot = self.prepare(self.spec("member", "member.bin"))
        private_root = snapshot.private_root
        reserved_path = (
            private_root / snapshot.files[0].private_relative
        )
        relocated_owned_path = private_root / "owned-relocated.artifact"
        reserved_path.rename(relocated_owned_path)
        reserved_path.write_bytes(b"unknown replacement")
        os.chmod(reserved_path, 0o600)

        with self.assertRaisesRegex(
            artifacts.SecureArtifactError,
            "cleanup failed",
        ):
            snapshot.close()

        self.assertEqual(snapshot.state, "CLOSED")
        self.assertFalse(relocated_owned_path.exists())
        self.assertEqual(
            reserved_path.read_bytes(),
            b"unknown replacement",
        )
        self.assertTrue(private_root.is_dir())

        reserved_path.unlink()
        private_root.rmdir()
        self.assert_private_parent_empty()

    def test_source_root_rebind_before_return_fails_and_cleans_private_root(
        self,
    ) -> None:
        self.write_source("member.bin", b"ORIGINAL")
        moved_source = self.source_root.with_name("source-moved")
        fired = False

        def replace_source_root(
            stage: str,
            logical_name: str | None,
        ) -> None:
            nonlocal fired
            if not fired and stage == "snapshot_before_return":
                fired = True
                self.source_root.rename(moved_source)
                self.source_root.mkdir(mode=0o700)
                (self.source_root / "must-survive.bin").write_bytes(
                    b"replacement"
                )

        with mock.patch.object(
            artifacts,
            "_test_hook",
            side_effect=replace_source_root,
        ), self.assertRaises(artifacts.SecureArtifactError):
            self.prepare(self.spec("member", "member.bin"))

        self.assertTrue(fired)
        self.assert_private_parent_empty()
        self.assertEqual(
            (self.source_root / "must-survive.bin").read_bytes(),
            b"replacement",
        )
        self.assertEqual(
            (moved_source / "member.bin").read_bytes(),
            b"ORIGINAL",
        )

    def test_alias_materialization_is_exclusive_distinct_and_idempotent(
        self,
    ) -> None:
        canonical = self.write_source("canonical.bin", b"canonical-bytes")
        tree = artifacts.SecureArtifactTree.open(self.source_root)
        try:
            tree.materialize_alias(
                self.spec("canonical", canonical.name),
                "nested/alias.bin",
            )
            alias = self.source_root / "nested" / "alias.bin"
            canonical_info = canonical.lstat()
            alias_info = alias.lstat()
            self.assertEqual(alias.read_bytes(), b"canonical-bytes")
            self.assertTrue(stat.S_ISREG(alias_info.st_mode))
            self.assertEqual(stat.S_IMODE(alias_info.st_mode), 0o600)
            self.assertEqual(alias_info.st_nlink, 1)
            self.assertNotEqual(
                (canonical_info.st_dev, canonical_info.st_ino),
                (alias_info.st_dev, alias_info.st_ino),
            )

            original_alias_inode = alias_info.st_ino
            tree.materialize_alias(
                self.spec("canonical", canonical.name),
                "nested/alias.bin",
            )
            self.assertEqual(alias.lstat().st_ino, original_alias_inode)
            self.assertEqual(
                sorted(path.name for path in alias.parent.iterdir()),
                ["alias.bin"],
            )
        finally:
            tree.close()

    def test_alias_materialization_rejects_existing_unsafe_alias_unchanged(
        self,
    ) -> None:
        canonical = self.write_source("canonical.bin", b"canonical")
        sentinel = self.base / "outside-sentinel.bin"
        sentinel.write_bytes(b"outside")
        os.chmod(sentinel, 0o600)
        tree = artifacts.SecureArtifactTree.open(self.source_root)
        try:
            for kind in ("stale", "symlink", "hardlink"):
                with self.subTest(kind=kind):
                    alias = self.source_root / f"{kind}.bin"
                    if kind == "stale":
                        alias.write_bytes(b"stale-data")
                        os.chmod(alias, 0o600)
                    elif kind == "symlink":
                        alias.symlink_to(sentinel)
                    else:
                        os.link(sentinel, alias)
                    before = alias.lstat()
                    before_bytes = (
                        alias.read_bytes()
                        if not stat.S_ISLNK(before.st_mode)
                        else os.readlink(alias)
                    )
                    with self.assertRaises(artifacts.SecureArtifactError):
                        tree.materialize_alias(
                            self.spec("canonical", canonical.name),
                            alias.name,
                        )
                    after = alias.lstat()
                    after_bytes = (
                        alias.read_bytes()
                        if not stat.S_ISLNK(after.st_mode)
                        else os.readlink(alias)
                    )
                    self.assertEqual(
                        (after.st_dev, after.st_ino, after.st_mode),
                        (before.st_dev, before.st_ino, before.st_mode),
                    )
                    self.assertEqual(after_bytes, before_bytes)
                    alias.unlink()
                    self.assertEqual(sentinel.read_bytes(), b"outside")
        finally:
            tree.close()

    def test_alias_materialization_detects_source_drift_and_cleans_owned_temp(
        self,
    ) -> None:
        canonical = self.write_source("canonical.bin", b"ORIGINAL")
        fired = False

        def mutate(stage: str, logical_name: str | None) -> None:
            nonlocal fired
            if (
                not fired
                and stage == "source_after_copy"
                and logical_name == "canonical"
            ):
                fired = True
                canonical.write_bytes(b"MUTATED!")
                os.chmod(canonical, 0o600)

        tree = artifacts.SecureArtifactTree.open(self.source_root)
        try:
            with mock.patch.object(
                artifacts,
                "_test_hook",
                side_effect=mutate,
            ), self.assertRaises(artifacts.SecureArtifactError):
                tree.materialize_alias(
                    self.spec("canonical", canonical.name),
                    "new/alias.bin",
                )
            self.assertTrue(fired)
            self.assertFalse((self.source_root / "new").exists())
        finally:
            tree.close()

    def test_alias_materialization_fails_closed_when_exclusive_rename_missing(
        self,
    ) -> None:
        canonical = self.write_source("canonical.bin", b"canonical")
        tree = artifacts.SecureArtifactTree.open(self.source_root)
        try:
            with mock.patch.object(
                artifacts,
                "_rename_noreplace",
                side_effect=artifacts.SecureArtifactError(
                    "atomic no-replace rename is unavailable"
                ),
            ), self.assertRaises(artifacts.SecureArtifactError):
                tree.materialize_alias(
                    self.spec("canonical", canonical.name),
                    "new/alias.bin",
                )
            self.assertFalse((self.source_root / "new").exists())
        finally:
            tree.close()

    def test_alias_post_publish_failure_removes_only_owned_publication(
        self,
    ) -> None:
        canonical = self.write_source("canonical.bin", b"canonical")
        tree = artifacts.SecureArtifactTree.open(self.source_root)
        real_verify = artifacts._verify_reopened_source
        calls = 0

        def reject_after_publish(*args, **kwargs) -> None:
            nonlocal calls
            calls += 1
            real_verify(*args, **kwargs)
            if calls == 2:
                raise artifacts.SecureArtifactError(
                    "forced post-publication verification failure"
                )

        try:
            with mock.patch.object(
                artifacts,
                "_verify_reopened_source",
                side_effect=reject_after_publish,
            ), self.assertRaises(artifacts.SecureArtifactError):
                tree.materialize_alias(
                    self.spec("canonical", canonical.name),
                    "new/alias.bin",
                )
            self.assertEqual(calls, 2)
            self.assertFalse((self.source_root / "new").exists())
            self.assertEqual(canonical.read_bytes(), b"canonical")
        finally:
            tree.close()

    def test_alias_cleanup_preserves_unknown_temp_replacement_and_fails_loudly(
        self,
    ) -> None:
        canonical = self.write_source("canonical.bin", b"canonical")
        stolen = self.base / "stolen-owned-temp"
        planted_name = ""

        def replace_temp_then_fail(
            directory_fd: int,
            source_name: str,
            destination_name: str,
        ) -> None:
            nonlocal planted_name
            del destination_name
            planted_name = source_name
            os.rename(
                source_name,
                stolen,
                src_dir_fd=directory_fd,
            )
            replacement_fd = os.open(
                source_name,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                0o600,
                dir_fd=directory_fd,
            )
            try:
                os.write(replacement_fd, b"unknown-replacement")
            finally:
                os.close(replacement_fd)
            raise artifacts.SecureArtifactError("forced publication failure")

        tree = artifacts.SecureArtifactTree.open(self.source_root)
        try:
            with mock.patch.object(
                artifacts,
                "_rename_noreplace",
                side_effect=replace_temp_then_fail,
            ), self.assertRaisesRegex(
                artifacts.SecureArtifactError,
                "cleanup failed",
            ):
                tree.materialize_alias(
                    self.spec("canonical", canonical.name),
                    "new/alias.bin",
                )
            self.assertTrue(planted_name)
            planted = self.source_root / "new" / planted_name
            self.assertEqual(planted.read_bytes(), b"unknown-replacement")
            self.assertEqual(stolen.read_bytes(), b"canonical")
            self.assertFalse((self.source_root / "new" / "alias.bin").exists())
        finally:
            tree.close()

    def test_private_partition_generator_consumes_frozen_inputs_and_injects_output(
        self,
    ) -> None:
        csv = self.write_source("partitions.csv", b"partition-source\n")
        generator = self.write_source(
            "generator.py",
            (
                b"from pathlib import Path\n"
                b"import sys\n"
                b"assert sys.argv[-1].startswith('/dev/fd/')\n"
                b"Path(sys.argv[-1]).write_bytes("
                b"b'generated:' + Path(sys.argv[-2]).read_bytes())\n"
            ),
            mode=0o700,
        )
        expected = b"generated:partition-source\n"
        self.write_source("partitions.bin", expected)
        tree = artifacts.SecureArtifactTree.open(self.source_root)
        validation = None
        final_snapshot = None
        try:
            validation = tree.prepare_snapshot(
                (
                    self.spec("partition.csv", csv.name),
                    self.spec(
                        "partition.generator",
                        generator.name,
                        allowed_modes=(0o700,),
                    ),
                    self.spec("artifact.partitions", "partitions.bin"),
                ),
                private_parent=self.private_parent,
            )
            frozen_inputs = validation.freeze_for_mutation()
            validation.close()
            validation = None

            generator.write_bytes(b"raise SystemExit('MUTATED ORIGINAL')\n")
            os.chmod(generator, 0o700)
            generated = artifacts.run_private_partition_generator(
                frozen_inputs,
                csv_logical_name="partition.csv",
                generator_logical_name="partition.generator",
                expected_logical_name="artifact.partitions",
                output_logical_name="partition.generated",
                private_parent=self.private_parent,
            )
            self.assertEqual(generated.content, expected)
            with self.assertRaises((AttributeError, TypeError)):
                generated.content = b"attacker-controlled"

            final_snapshot = tree.prepare_snapshot(
                (self.spec("member", "partitions.bin"),),
                generated_members=(generated,),
                private_parent=self.private_parent,
            )
            frozen = final_snapshot.freeze_for_mutation()
            self.assertEqual(
                frozen.member_bytes("partition.generated"),
                expected,
            )
        finally:
            if validation is not None:
                validation.close()
            if final_snapshot is not None:
                final_snapshot.close()
            tree.close()
        self.assert_private_parent_empty()

    def test_private_partition_generator_mismatch_fails_and_leaks_no_fds(
        self,
    ) -> None:
        self.write_source("partitions.csv", b"csv\n")
        self.write_source(
            "generator.py",
            (
                b"from pathlib import Path\n"
                b"import sys\n"
                b"assert sys.argv[-1].startswith('/dev/fd/')\n"
                b"Path(sys.argv[-1]).write_bytes(b'wrong')\n"
            ),
            mode=0o700,
        )
        self.write_source("partitions.bin", b"expected")
        tree = artifacts.SecureArtifactTree.open(self.source_root)
        validation = tree.prepare_snapshot(
            (
                self.spec("partition.csv", "partitions.csv"),
                self.spec(
                    "partition.generator",
                    "generator.py",
                    allowed_modes=(0o700,),
                ),
                self.spec("artifact.partitions", "partitions.bin"),
            ),
            private_parent=self.private_parent,
        )
        frozen_inputs = validation.freeze_for_mutation()
        validation.close()
        descriptors_before = len(os.listdir("/dev/fd"))
        try:
            for _ in range(8):
                with self.assertRaises(artifacts.SecureArtifactError):
                    artifacts.run_private_partition_generator(
                        frozen_inputs,
                        csv_logical_name="partition.csv",
                        generator_logical_name="partition.generator",
                        expected_logical_name="artifact.partitions",
                        output_logical_name="partition.generated",
                        private_parent=self.private_parent,
                    )
                self.assert_private_parent_empty()
                self.assertEqual(
                    len(os.listdir("/dev/fd")),
                    descriptors_before,
                )
        finally:
            tree.close()

    def test_partition_generator_output_path_replacement_cannot_redirect_write(
        self,
    ) -> None:
        self.write_source("partitions.csv", b"csv\n")
        self.write_source(
            "generator.py",
            (
                b"from pathlib import Path\n"
                b"import sys\n"
                b"assert sys.argv[-1].startswith('/dev/fd/')\n"
                b"Path(sys.argv[-1]).write_bytes(b'expected')\n"
            ),
            mode=0o700,
        )
        self.write_source("partitions.bin", b"expected")
        sentinel = self.base / "outside-generator-sentinel"
        sentinel.write_bytes(b"outside-original")
        stolen = self.base / "stolen-generator-output"
        tree = artifacts.SecureArtifactTree.open(self.source_root)
        validation = tree.prepare_snapshot(
            (
                self.spec("partition.csv", "partitions.csv"),
                self.spec(
                    "partition.generator",
                    "generator.py",
                    allowed_modes=(0o700,),
                ),
                self.spec("artifact.partitions", "partitions.bin"),
            ),
            private_parent=self.private_parent,
        )
        frozen_inputs = validation.freeze_for_mutation()
        validation.close()
        planted: Path | None = None

        def replace_output(
            stage: str,
            logical_name: str | None,
        ) -> None:
            nonlocal planted
            if (
                stage != "partition_generator_before_exec"
                or planted is not None
            ):
                return
            roots = list(self.private_parent.iterdir())
            self.assertEqual(len(roots), 1)
            output = roots[0] / "generated-output.bin"
            output.rename(stolen)
            output.symlink_to(sentinel)
            planted = output

        try:
            with mock.patch.object(
                artifacts,
                "_test_hook",
                side_effect=replace_output,
            ), self.assertRaisesRegex(
                artifacts.SecureArtifactError,
                "cleanup failed",
            ):
                artifacts.run_private_partition_generator(
                    frozen_inputs,
                    csv_logical_name="partition.csv",
                    generator_logical_name="partition.generator",
                    expected_logical_name="artifact.partitions",
                    output_logical_name="partition.generated",
                    private_parent=self.private_parent,
                )
            self.assertIsNotNone(planted)
            assert planted is not None
            self.assertTrue(planted.is_symlink())
            self.assertEqual(sentinel.read_bytes(), b"outside-original")
            self.assertEqual(stolen.read_bytes(), b"expected")
            planted.unlink()
            planted.parent.rmdir()
            stolen.unlink()
        finally:
            tree.close()
        self.assert_private_parent_empty()

    def test_partition_generator_cleanup_preserves_unknown_replacement(
        self,
    ) -> None:
        self.write_source("partitions.csv", b"csv\n")
        self.write_source(
            "generator.py",
            (
                b"from pathlib import Path\n"
                b"import sys\n"
                b"assert sys.argv[-1].startswith('/dev/fd/')\n"
                b"Path(sys.argv[-1]).write_bytes(b'expected')\n"
            ),
            mode=0o700,
        )
        self.write_source("partitions.bin", b"expected")
        tree = artifacts.SecureArtifactTree.open(self.source_root)
        validation = tree.prepare_snapshot(
            (
                self.spec("partition.csv", "partitions.csv"),
                self.spec(
                    "partition.generator",
                    "generator.py",
                    allowed_modes=(0o700,),
                ),
                self.spec("artifact.partitions", "partitions.bin"),
            ),
            private_parent=self.private_parent,
        )
        frozen_inputs = validation.freeze_for_mutation()
        validation.close()
        planted: Path | None = None

        def replace_output(
            stage: str,
            logical_name: str | None,
        ) -> None:
            nonlocal planted
            if stage != "partition_generator_before_cleanup":
                return
            roots = list(self.private_parent.iterdir())
            self.assertEqual(len(roots), 1)
            output = roots[0] / "generated-output.bin"
            stolen = self.base / "stolen-generator-output"
            output.rename(stolen)
            output.write_bytes(b"unknown-replacement")
            os.chmod(output, 0o600)
            planted = output

        try:
            with mock.patch.object(
                artifacts,
                "_test_hook",
                side_effect=replace_output,
            ), self.assertRaisesRegex(
                artifacts.SecureArtifactError,
                "cleanup failed",
            ):
                artifacts.run_private_partition_generator(
                    frozen_inputs,
                    csv_logical_name="partition.csv",
                    generator_logical_name="partition.generator",
                    expected_logical_name="artifact.partitions",
                    output_logical_name="partition.generated",
                    private_parent=self.private_parent,
                )
            self.assertIsNotNone(planted)
            assert planted is not None
            self.assertEqual(planted.read_bytes(), b"unknown-replacement")
            self.assertEqual(
                (self.base / "stolen-generator-output").read_bytes(),
                b"expected",
            )
            planted.unlink()
            planted.parent.rmdir()
        finally:
            tree.close()
        self.assert_private_parent_empty()


if __name__ == "__main__":
    unittest.main(verbosity=2)
