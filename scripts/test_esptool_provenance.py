from __future__ import annotations

import base64
import hashlib
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


SCRIPTS = Path(__file__).resolve().parent
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

import esptool_provenance as provenance


REVIEWED_LOCK = SCRIPTS / "esptoolpy-2.41100.0.lock.json"


def _stub_json(**changes: object) -> bytes:
    payload: dict[str, object] = {
        "entry": 0x40380010,
        "text": base64.b64encode(b"TEXT").decode("ascii"),
        "text_start": 0x40380000,
        "data": base64.b64encode(b"DATA").decode("ascii"),
        "data_start": 0x3FC80000,
        "bss_start": 0x3FC81000,
    }
    payload.update(changes)
    return json.dumps(payload, separators=(",", ":")).encode("ascii")


class EsptoolProvenancePrimitiveTests(unittest.TestCase):
    def test_reviewed_lock_is_canonical_and_pinned(self) -> None:
        raw = REVIEWED_LOCK.read_bytes()
        self.assertEqual(
            hashlib.sha256(raw).hexdigest(),
            provenance.EXPECTED_ESPTOOL_LOCK_SHA256,
        )
        lock = provenance.parse_reviewed_lock(
            raw, provenance.EXPECTED_ESPTOOL_LOCK_SHA256
        )
        self.assertEqual(lock.package_name, "tool-esptoolpy")
        self.assertEqual(lock.package_version, "2.41100.0")
        self.assertEqual(len(lock.members), 56)
        self.assertEqual(
            sum(member.kind == "python" for member in lock.members.values()),
            28,
        )
        self.assertEqual(
            sum(
                member.kind == "runtime_data"
                for member in lock.members.values()
            ),
            28,
        )
        self.assertEqual(list(lock.members), sorted(lock.members))
        self.assertIn(
            "esptool/targets/stub_flasher/1/esp32s3.json",
            lock.members,
        )
        self.assertIn(
            "esptool/targets/stub_flasher/2/esp32s3.json",
            lock.members,
        )

    def test_lock_parser_rejects_duplicates_trailing_data_and_bool_version(
        self,
    ) -> None:
        valid = (
            b'{"members":[],"package_name":"tool-esptoolpy",'
            b'"package_version":"2.41100.0","schema_version":1}\n'
        )
        digest = hashlib.sha256(valid).hexdigest()
        provenance.parse_reviewed_lock(valid, digest)
        for invalid in (
            valid.replace(
                b'"members":[]',
                b'"members":[],"members":[]',
            ),
            valid + b"{}",
            valid.replace(b'"schema_version":1', b'"schema_version":true'),
        ):
            with self.assertRaises(provenance.EsptoolProvenanceError):
                provenance.parse_reviewed_lock(
                    invalid, hashlib.sha256(invalid).hexdigest()
                )

    def test_lock_parser_rejects_noncanonical_or_unsafe_members(self) -> None:
        member = {
            "kind": "python",
            "mode": 0o600,
            "path": "esptool/__init__.py",
            "sha256": "0" * 64,
            "size": 0,
        }
        valid = {
            "members": [member],
            "package_name": "tool-esptoolpy",
            "package_version": "2.41100.0",
            "schema_version": 1,
        }

        def encoded(value: object) -> bytes:
            return (
                json.dumps(
                    value, sort_keys=True, separators=(",", ":")
                ).encode("ascii")
                + b"\n"
            )

        cases = (
            {**valid, "unknown": 1},
            {**valid, "members": [{**member, "unknown": 1}]},
            {**valid, "members": [{**member, "path": "../esptool.py"}]},
            {**valid, "members": [{**member, "mode": True}]},
            {**valid, "members": [{**member, "size": True}]},
            {**valid, "members": [{**member, "sha256": "A" * 64}]},
            {
                **valid,
                "members": [
                    {**member, "path": "esptool/z.py"},
                    {**member, "path": "esptool/a.py"},
                ],
            },
        )
        for invalid in cases:
            raw = encoded(invalid)
            with self.subTest(invalid=invalid):
                with self.assertRaises(provenance.EsptoolProvenanceError):
                    provenance.parse_reviewed_lock(
                        raw, hashlib.sha256(raw).hexdigest()
                    )

    def test_frozen_stub_parser_is_exact_and_copies_decoded_bytes(self) -> None:
        text = bytearray(b"TEXT")
        raw = _stub_json(text=base64.b64encode(text).decode("ascii"))
        record = provenance.parse_frozen_stub_record(raw)
        text[:] = b"EVIL"
        self.assertEqual(record.text, b"TEXT")
        self.assertEqual(record.data, b"DATA")
        self.assertEqual(record.entry, 0x40380010)
        self.assertEqual(record.text_start, 0x40380000)
        self.assertEqual(record.data_start, 0x3FC80000)
        self.assertEqual(record.bss_start, 0x3FC81000)

    def test_frozen_stub_parser_rejects_ambiguous_or_invalid_records(
        self,
    ) -> None:
        duplicate = _stub_json().replace(
            b'"entry":1077411856',
            b'"entry":1077411856,"entry":1077411856',
        )
        cases = (
            duplicate,
            _stub_json() + b"{}",
            _stub_json(extra=1),
            _stub_json(entry=-1),
            _stub_json(entry=0x1_0000_0000),
            _stub_json(text="***"),
            _stub_json(data=None),
            _stub_json(data_start=None),
        )
        for invalid in cases:
            with self.subTest(invalid=invalid[:80]):
                with self.assertRaises(provenance.EsptoolProvenanceError):
                    provenance.parse_frozen_stub_record(invalid)

    def test_package_freeze_rejects_symlink_hardlink_extra_and_tamper(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "tool-esptoolpy"
            (root / "esptool").mkdir(parents=True)
            package_json = (
                b'{"name":"tool-esptoolpy","version":"2.41100.0"}\n'
            )
            source = b"VALUE = 1\n"
            (root / "package.json").write_bytes(package_json)
            source_path = root / "esptool" / "__init__.py"
            source_path.write_bytes(source)
            os.chmod(source_path, 0o600)
            member = {
                "kind": "python",
                "mode": stat.S_IMODE(source_path.stat().st_mode),
                "path": "esptool/__init__.py",
                "sha256": hashlib.sha256(source).hexdigest(),
                "size": len(source),
            }
            lock_obj = {
                "members": [member],
                "package_name": "tool-esptoolpy",
                "package_version": "2.41100.0",
                "schema_version": 1,
            }
            lock_raw = (
                json.dumps(
                    lock_obj, sort_keys=True, separators=(",", ":")
                ).encode("ascii")
                + b"\n"
            )
            reviewed = provenance.parse_reviewed_lock(
                lock_raw, hashlib.sha256(lock_raw).hexdigest()
            )
            frozen = provenance.freeze_verified_package(root, reviewed)
            self.assertEqual(
                frozen.members["esptool/__init__.py"], source
            )

            source_path.write_bytes(b"VALUE = 2\n")
            with self.assertRaises(provenance.EsptoolProvenanceError):
                provenance.freeze_verified_package(root, reviewed)
            source_path.write_bytes(source)

            source_path.unlink()
            with self.assertRaises(provenance.EsptoolProvenanceError):
                provenance.freeze_verified_package(root, reviewed)
            source_path.write_bytes(source)
            os.chmod(source_path, 0o600)

            os.chmod(source_path, 0o644)
            with self.assertRaises(provenance.EsptoolProvenanceError):
                provenance.freeze_verified_package(root, reviewed)
            os.chmod(source_path, 0o600)

            extra = root / "esptool" / "extra.py"
            extra.write_text("VALUE = 3\n")
            with self.assertRaises(provenance.EsptoolProvenanceError):
                provenance.freeze_verified_package(root, reviewed)
            extra.unlink()

            bytecode = root / "esptool" / "__pycache__"
            bytecode.mkdir()
            (bytecode / "ghost.cpython-312.pyc").write_bytes(b"BYTECODE")
            with self.assertRaises(provenance.EsptoolProvenanceError):
                provenance.freeze_verified_package(root, reviewed)
            (bytecode / "ghost.cpython-312.pyc").unlink()
            bytecode.rmdir()

            (root / "package.json").write_bytes(
                package_json.replace(b"2.41100.0", b"9.99999.9")
            )
            with self.assertRaises(provenance.EsptoolProvenanceError):
                provenance.freeze_verified_package(root, reviewed)
            (root / "package.json").write_bytes(package_json)

            target = root / "target.py"
            target.write_bytes(source)
            source_path.unlink()
            os.link(target, source_path)
            with self.assertRaises(provenance.EsptoolProvenanceError):
                provenance.freeze_verified_package(root, reviewed)
            source_path.unlink()
            os.symlink(target.name, source_path)
            with self.assertRaises(provenance.EsptoolProvenanceError):
                provenance.freeze_verified_package(root, reviewed)

    def test_platformio_resolution_uses_exact_package_spec(self) -> None:
        package = SimpleNamespace(path="/exact/tool-esptoolpy")
        manager = mock.Mock()
        manager.get_package.return_value = package
        package_spec = mock.Mock(return_value="EXACT_SPEC")
        with mock.patch.object(
            provenance,
            "_load_canonical_platformio_api",
            return_value=(
                mock.Mock(return_value=manager),
                package_spec,
            ),
        ):
            self.assertEqual(
                provenance.resolve_platformio_esptool_root(),
                Path("/exact/tool-esptoolpy"),
            )
        package_spec.assert_called_once_with(
            name="tool-esptoolpy", requirements="2.41100.0"
        )
        manager.get_package.assert_called_once_with("EXACT_SPEC")

    def test_platformio_resolution_does_not_execute_pythonpath_shadow(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temp:
            shadow = Path(temp)
            tool = shadow / "platformio/package/manager/tool.py"
            meta = shadow / "platformio/package/meta.py"
            tool.parent.mkdir(parents=True)
            meta.parent.mkdir(parents=True, exist_ok=True)
            for package in (
                shadow / "platformio/__init__.py",
                shadow / "platformio/package/__init__.py",
                shadow / "platformio/package/manager/__init__.py",
            ):
                package.write_text("", encoding="utf-8")
            tool.write_text(
                "raise RuntimeError('FAKE_PLATFORMIO_MANAGER_EXECUTED')\n",
                encoding="utf-8",
            )
            meta.write_text(
                "raise RuntimeError('FAKE_PLATFORMIO_META_EXECUTED')\n",
                encoding="utf-8",
            )
            script = (
                "import sys\n"
                f"sys.path.insert(0, {str(SCRIPTS)!r})\n"
                "import esptool_provenance as p\n"
                "print(p.resolve_platformio_esptool_root().name)\n"
            )
            environment = dict(os.environ)
            environment["PYTHONPATH"] = str(shadow)
            completed = subprocess.run(
                [sys.executable, "-c", script],
                text=True,
                capture_output=True,
                env=environment,
                cwd=temp,
                check=False,
            )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stdout.strip(), "tool-esptoolpy")
        self.assertNotIn("FAKE_PLATFORMIO", completed.stdout)
        self.assertNotIn("FAKE_PLATFORMIO", completed.stderr)

    def test_preloaded_esptool_is_rejected_before_package_resolution(
        self,
    ) -> None:
        with mock.patch.dict(sys.modules, {"esptool": object()}):
            with mock.patch.object(
                provenance,
                "resolve_platformio_esptool_root",
                side_effect=AssertionError("resolver must not run"),
            ):
                with self.assertRaises(provenance.EsptoolProvenanceError):
                    provenance.load_verified_platformio_esptool()

    def test_external_esptool_environment_and_config_are_rejected(
        self,
    ) -> None:
        package_root = provenance.resolve_platformio_esptool_root()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            config = root / "esptool.cfg"
            config.write_text(
                "[esptool]\ntimeout=99\nserial_write_timeout=88\n",
                encoding="utf-8",
            )
            cases = (
                {"ESPTOOL_CFGFILE": str(config)},
                {"ESPTOOL_FS": "1MB"},
                {"ESPTOOL_ENV_FPGA": "1"},
            )
            for environment in cases:
                with self.subTest(environment=environment):
                    with mock.patch.object(
                        provenance,
                        "resolve_platformio_esptool_root",
                        return_value=package_root,
                    ), mock.patch.dict(
                        os.environ, environment, clear=False
                    ), mock.patch(
                        "serial.Serial",
                        side_effect=AssertionError(
                            "configuration rejection must precede serial"
                        ),
                    ):
                        with self.assertRaises(
                            provenance.EsptoolProvenanceError
                        ):
                            provenance.load_verified_platformio_esptool()

            old_cwd = os.getcwd()
            try:
                os.chdir(root)
                with mock.patch.object(
                    provenance,
                    "resolve_platformio_esptool_root",
                    return_value=package_root,
                ), mock.patch(
                    "serial.Serial",
                    side_effect=AssertionError(
                        "configuration rejection must precede serial"
                    ),
                ):
                    with self.assertRaises(
                        provenance.EsptoolProvenanceError
                    ):
                        provenance.load_verified_platformio_esptool()
            finally:
                os.chdir(old_cwd)

    def test_every_stock_config_search_location_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            working = root / "working"
            home = root / "home"
            config_home = home / ".config/esptool"
            working.mkdir()
            home.mkdir()
            config_home.mkdir(parents=True)
            old_cwd = os.getcwd()
            clean_environment = {
                key: value
                for key, value in os.environ.items()
                if not key.startswith("ESPTOOL_")
            }
            clean_environment["HOME"] = str(home)
            try:
                os.chdir(working)
                with mock.patch.dict(
                    os.environ, clean_environment, clear=True
                ):
                    for directory in (working, config_home, home):
                        for name in ("esptool.cfg", "setup.cfg", "tox.ini"):
                            candidate = directory / name
                            candidate.write_text(
                                "[esptool]\ntimeout=99\n",
                                encoding="utf-8",
                            )
                            with self.subTest(candidate=str(candidate)):
                                with self.assertRaises(
                                    provenance.EsptoolProvenanceError
                                ):
                                    provenance._reject_external_esptool_configuration()
                            candidate.unlink()

                    harmless = working / "setup.cfg"
                    harmless.write_text(
                        "[metadata]\nname=not-esptool\n",
                        encoding="utf-8",
                    )
                    provenance._reject_external_esptool_configuration()
            finally:
                os.chdir(old_cwd)

    def test_untrusted_preloaded_platformio_module_is_rejected(self) -> None:
        with mock.patch.dict(sys.modules, {"platformio": object()}):
            with self.assertRaises(provenance.EsptoolProvenanceError):
                provenance._load_canonical_platformio_api()

    def test_forged_preloaded_platformio_metadata_is_rejected(self) -> None:
        purelib = Path(provenance.sysconfig.get_path("purelib"))
        tool_path = purelib / "platformio/package/manager/tool.py"
        meta_path = purelib / "platformio/package/meta.py"

        tool_module = type(sys)("platformio.package.manager.tool")
        tool_loader = importlib.machinery.SourceFileLoader(
            tool_module.__name__, str(tool_path)
        )
        tool_module.__file__ = str(tool_path)
        tool_module.__spec__ = importlib.util.spec_from_loader(
            tool_module.__name__, tool_loader, origin=str(tool_path)
        )
        manager = mock.Mock()
        manager.get_package.return_value = SimpleNamespace(
            path="/private/tmp/attacker/tool-esptoolpy"
        )
        tool_module.ToolPackageManager = mock.Mock(return_value=manager)

        meta_module = type(sys)("platformio.package.meta")
        meta_loader = importlib.machinery.SourceFileLoader(
            meta_module.__name__, str(meta_path)
        )
        meta_module.__file__ = str(meta_path)
        meta_module.__spec__ = importlib.util.spec_from_loader(
            meta_module.__name__, meta_loader, origin=str(meta_path)
        )
        meta_module.PackageSpec = mock.Mock(return_value="FORGED")

        with mock.patch.dict(
            sys.modules,
            {
                tool_module.__name__: tool_module,
                meta_module.__name__: meta_module,
            },
        ):
            with self.assertRaises(provenance.EsptoolProvenanceError):
                provenance.resolve_platformio_esptool_root()

    def test_relative_openat_is_refused_while_runtime_is_active(self) -> None:
        package_root = provenance.resolve_platformio_esptool_root()
        root_fd = os.open(
            package_root,
            os.O_RDONLY
            | getattr(os, "O_DIRECTORY", 0)
            | getattr(os, "O_CLOEXEC", 0),
        )
        try:
            with mock.patch(
                "serial.Serial",
                side_effect=AssertionError("provenance must not open serial"),
            ):
                runtime = provenance.load_verified_platformio_esptool()
            try:
                with self.assertRaises(provenance.EsptoolProvenanceError):
                    fd = os.open(
                        "package.json",
                        os.O_RDONLY,
                        dir_fd=root_fd,
                    )
                    os.close(fd)
            finally:
                runtime.close()
        finally:
            os.close(root_fd)

    def test_baseexception_cleanup_does_not_poison_process_state(self) -> None:
        probes = (
            (
                "resolver",
                "p.resolve_platformio_esptool_root = "
                "lambda: (_ for _ in ()).throw(KeyboardInterrupt())\n",
            ),
            (
                "post-import",
                "p._verify_signatures = "
                "lambda _e: (_ for _ in ()).throw(KeyboardInterrupt())\n",
            ),
        )
        for label, injection in probes:
            with self.subTest(label=label):
                script = (
                    "import os, sys\n"
                    f"sys.path.insert(0, {str(SCRIPTS)!r})\n"
                    "import esptool_provenance as p\n"
                    "original_exists = os.path.exists\n"
                    + injection
                    + "try:\n"
                    "    p.load_verified_platformio_esptool()\n"
                    "except KeyboardInterrupt:\n"
                    "    pass\n"
                    "else:\n"
                    "    raise AssertionError('interrupt was swallowed')\n"
                    "assert p._EXECUTION_LOCK.acquire(blocking=False)\n"
                    "p._EXECUTION_LOCK.release()\n"
                    "assert not any(type(x).__name__ == "
                    "'_FrozenEsptoolFinder' for x in sys.meta_path)\n"
                    "assert os.path.exists is original_exists\n"
                    "assert not p._AUDIT_ROOTS\n"
                    "assert not any(n == 'esptool' or "
                    "n.startswith('esptool.') for n in sys.modules)\n"
                    "print('CLEAN')\n"
                )
                completed = subprocess.run(
                    [sys.executable, "-c", script],
                    text=True,
                    capture_output=True,
                    check=False,
                )
                self.assertEqual(
                    completed.returncode, 0, completed.stderr
                )
                self.assertEqual(completed.stdout.strip(), "CLEAN")

    def test_real_verified_runtime_is_frozen_and_reset_to_one_attempt(
        self,
    ) -> None:
        with mock.patch(
            "serial.Serial",
            side_effect=AssertionError("provenance must not open serial"),
        ):
            runtime = provenance.load_verified_platformio_esptool()
        try:
            esptool = runtime.esptool
            self.assertEqual(esptool.__version__, "4.11.0")
            self.assertIs(
                esptool.loader.StubFlasher,
                provenance.FrozenStubFlasher,
            )
            self.assertIs(
                esptool.StubFlasher,
                provenance.FrozenStubFlasher,
            )
            self.assertEqual(esptool.loader.WRITE_BLOCK_ATTEMPTS, 1)
            self.assertEqual(
                esptool.loader.ESPLoader.WRITE_FLASH_ATTEMPTS, 1
            )
            esp32s3 = sys.modules["esptool.targets.esp32s3"]
            self.assertEqual(esp32s3.ESP32S3ROM.WRITE_FLASH_ATTEMPTS, 1)
            self.assertEqual(
                esp32s3.ESP32S3StubLoader.WRITE_FLASH_ATTEMPTS, 1
            )
            self.assertEqual(dict(esptool.loader.cfg), {})
            expected_loader_constants = {
                "DEFAULT_TIMEOUT": 3,
                "CHIP_ERASE_TIMEOUT": 120,
                "MAX_TIMEOUT": 240,
                "SYNC_TIMEOUT": 0.1,
                "MD5_TIMEOUT_PER_MB": 8,
                "ERASE_REGION_TIMEOUT_PER_MB": 30,
                "ERASE_WRITE_TIMEOUT_PER_MB": 40,
                "MEM_END_ROM_TIMEOUT": 0.2,
                "DEFAULT_SERIAL_WRITE_TIMEOUT": 10,
                "DEFAULT_CONNECT_ATTEMPTS": 7,
                "DEFAULT_OPEN_PORT_ATTEMPTS": 1,
            }
            self.assertEqual(
                {
                    name: getattr(esptool.loader, name)
                    for name in expected_loader_constants
                },
                expected_loader_constants,
            )
            runtime.audit_loaded_modules()
            esptool.loader.DEFAULT_TIMEOUT = 99
            with self.assertRaises(provenance.EsptoolProvenanceError):
                runtime.audit_loaded_modules()
            esptool.loader.DEFAULT_TIMEOUT = 3
            with mock.patch.dict(
                os.environ, {"ESPTOOL_FS": "1MB"}, clear=False
            ):
                with self.assertRaises(provenance.EsptoolProvenanceError):
                    runtime.audit_loaded_modules()
            esptool.loader.WRITE_BLOCK_ATTEMPTS = 2
            with self.assertRaises(provenance.EsptoolProvenanceError):
                runtime.audit_loaded_modules()
            esptool.loader.WRITE_BLOCK_ATTEMPTS = 1
            for name, module in tuple(sys.modules.items()):
                if name == "esptool" or name.startswith("esptool."):
                    self.assertTrue(
                        module.__spec__.origin.startswith(
                            "frozen-platformio-esptool:"
                        )
                    )

            record_v1 = provenance.FrozenStubFlasher(
                SimpleNamespace(
                    CHIP_NAME="ESP32-S3",
                    STUB_CLASS=esp32s3.ESP32S3StubLoader,
                )
            )
            provenance.FrozenStubFlasher.set_preferred_stub_subdir("2")
            record_v2 = provenance.FrozenStubFlasher(
                SimpleNamespace(
                    CHIP_NAME="ESP32-S3",
                    STUB_CLASS=esp32s3.ESP32S3StubLoader,
                )
            )
            self.assertNotEqual(record_v1.entry, record_v2.entry)
            with self.assertRaises(provenance.EsptoolProvenanceError):
                provenance.FrozenStubFlasher.set_preferred_stub_subdir("3")

            with self.assertRaises(provenance.EsptoolProvenanceError):
                open(runtime.package_root / "package.json", "rb")
            with self.assertRaises(provenance.EsptoolProvenanceError):
                os.path.exists(runtime.package_root / "package.json")
        finally:
            runtime.close()
        self.assertFalse(
            any(
                name == "esptool" or name.startswith("esptool.")
                for name in sys.modules
            )
        )


if __name__ == "__main__":
    unittest.main()
