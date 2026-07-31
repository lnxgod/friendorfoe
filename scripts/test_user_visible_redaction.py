#!/usr/bin/env python3
"""Public-output redaction tests for the badge flasher."""

from __future__ import annotations

import contextlib
import io
import importlib
import os
import sys
import tempfile
import threading
import unittest
from collections.abc import Callable
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fof_badge_flash as flash


_MAC_FORMS = (
    "A0:B1:C2:D3:E4:F5",
    "A0-B1-C2-D3-E4-F5",
    "A0B1.C2D3.E4F5",
    "A0B1C2D3E4F5",
)


def _run_with_outer_fd_capture(
    operation: Callable[[], object],
) -> tuple[object, str, str]:
    stdout_read, stdout_write = os.pipe()
    stderr_read, stderr_write = os.pipe()
    saved_stdout = os.dup(1)
    saved_stderr = os.dup(2)
    result: object | None = None
    failure: BaseException | None = None
    try:
        sys.stdout.flush()
        sys.stderr.flush()
        os.dup2(stdout_write, 1)
        os.dup2(stderr_write, 2)
        os.close(stdout_write)
        os.close(stderr_write)
        try:
            result = operation()
        except BaseException as exc:
            failure = exc
    finally:
        sys.stdout.flush()
        sys.stderr.flush()
        os.dup2(saved_stdout, 1)
        os.dup2(saved_stderr, 2)
        os.close(saved_stdout)
        os.close(saved_stderr)
    outer_stdout = os.read(stdout_read, 64 * 1024).decode()
    outer_stderr = os.read(stderr_read, 64 * 1024).decode()
    os.close(stdout_read)
    os.close(stderr_read)
    if failure is not None:
        raise failure
    return result, outer_stdout, outer_stderr


class ScrubUserVisibleTextTests(unittest.TestCase):
    def test_scrubs_each_supported_mac_rendering_at_text_boundaries(
        self,
    ) -> None:
        cases = (
            ("AA:bb:CC:dd:EE:fF", "[hardware-id]"),
            ("aa-BB-cc-DD-ee-FF", "[hardware-id]"),
            ("AaBb.cCdD.eEfF", "[hardware-id]"),
            ("aAbBcCdDeEfF", "[hardware-id]"),
            (
                "(AA:BB:CC:DD:EE:FF), next",
                "([hardware-id]), next",
            ),
            (
                "device=aa-bb-cc-dd-ee-ff.",
                "device=[hardware-id].",
            ),
            (
                "before aabb.ccdd.eeff; after",
                "before [hardware-id]; after",
            ),
            (
                "before 001122334455!",
                "before [hardware-id]!",
            ),
            (
                "MAC:AA:BB:CC:DD:EE:FF",
                "MAC:[hardware-id]",
            ),
            (
                "MAC:AABBCCDDEEFF",
                "MAC:[hardware-id]",
            ),
        )

        for raw, expected in cases:
            with self.subTest(raw=raw):
                self.assertEqual(
                    flash.scrub_user_visible_text(raw),
                    expected,
                )

    def test_scrubs_multiple_mixed_case_and_format_ids(self) -> None:
        raw = (
            "uplink AA:BB:CC:DD:EE:FF; scanner aa-bb-cc-dd-ee-01; "
            "peer AAbb.ccDD.ee02; compact 001122334403"
        )

        self.assertEqual(
            flash.scrub_user_visible_text(raw),
            (
                "uplink [hardware-id]; scanner [hardware-id]; "
                "peer [hardware-id]; compact [hardware-id]"
            ),
        )

    def test_renders_objects_before_scrubbing(self) -> None:
        class Diagnostic:
            def __str__(self) -> str:
                return "diagnostic for 12:34:56:78:9A:BC"

        cases = (
            (Diagnostic(), "diagnostic for [hardware-id]"),
            (
                b"binary AA-BB-CC-DD-EE-FF",
                "b'binary [hardware-id]'",
            ),
            (None, "None"),
            (37, "37"),
        )

        for value, expected in cases:
            with self.subTest(value=repr(value)):
                self.assertEqual(
                    flash.scrub_user_visible_text(value),
                    expected,
                )

    def test_preserves_non_mac_text_and_longer_hex_tokens(self) -> None:
        cases = (
            "ordinary badge output",
            "01:23:45:67:89",
            "01-23-45-67-89",
            "0123.4567",
            "0123456789",
            "00112233445566",
            "x001122334455y",
            "sha256=00112233445566778899aabbccddeeff"
            "00112233445566778899aabbccddeeff",
            "IPv6-like=00:11:22:33:44:55:66",
            "compressed-IPv6=fe80::00:11:22:33:44:55",
            "partial-colon-prefix=0:00:11:22:33:44:55",
            "partial-colon-suffix=00:11:22:33:44:55:6",
            "long-hyphen=00-11-22-33-44-55-66",
            "partial-hyphen-prefix=0-00-11-22-33-44-55",
            "partial-hyphen-suffix=00-11-22-33-44-55-6",
            "long-dotted=0011.2233.4455.6677",
            "partial-dotted-prefix=66.0011.2233.4455",
            "partial-dotted-suffix=0011.2233.4455.66",
            "uuid=00112233-4455-6677-8899-aabbccddeeff",
            "partial-compact-prefix=66-001122334455",
            "partial-compact-suffix=001122334455-66",
            "partial-compact-colon-prefix=6:001122334455",
            "partial-compact-colon-suffix=001122334455:6",
            "partial-compact-dotted-prefix=66.001122334455",
            "partial-compact-dotted-suffix=001122334455.66",
            "mixed=00:11-22:33-44:55",
            "version=1.2.3 and time=12:34:56",
            "identifier_001122334455_suffix",
        )

        for value in cases:
            with self.subTest(value=value):
                self.assertEqual(
                    flash.scrub_user_visible_text(value),
                    value,
                )


class ProcessOutputCaptureTests(unittest.TestCase):
    def test_captures_direct_os_fd_output_and_returns_private_result(
        self,
    ) -> None:
        private_result = {
            "hardware_id": "A0:B1:C2:D3:E4:F5",
            "status": "PASS",
        }

        def operation() -> dict[str, str]:
            os.write(
                1,
                b"colon A0:B1:C2:D3:E4:F5 "
                b"dotted A0B1.C2D3.E4F5\n",
            )
            os.write(
                2,
                b"hyphen A0-B1-C2-D3-E4-F5 "
                b"compact A0B1C2D3E4F5\n",
            )
            return private_result

        captured = flash.capture_user_visible_output(operation)

        self.assertTrue(captured.succeeded)
        self.assertIs(captured.result, private_result)
        self.assertIsNone(captured.error_type)
        self.assertIsNone(captured.error)
        self.assertEqual(
            captured.stdout,
            "colon [hardware-id] dotted [hardware-id]\n",
        )
        self.assertEqual(
            captured.stderr,
            "hyphen [hardware-id] compact [hardware-id]\n",
        )

    def test_exception_returns_only_scrubbed_failure_and_restores_fds(
        self,
    ) -> None:
        raw_values = (
            "A0:B1:C2:D3:E4:F5",
            "A0-B1-C2-D3-E4-F5",
            "A0B1.C2D3.E4F5",
            "A0B1C2D3E4F5",
        )
        stdout_read, stdout_write = os.pipe()
        stderr_read, stderr_write = os.pipe()
        saved_stdout = os.dup(1)
        saved_stderr = os.dup(2)
        try:
            sys.stdout.flush()
            sys.stderr.flush()
            os.dup2(stdout_write, 1)
            os.dup2(stderr_write, 2)
            os.close(stdout_write)
            os.close(stderr_write)

            def operation() -> None:
                os.write(1, f"before {raw_values[0]}\n".encode())
                os.write(2, f"before {raw_values[1]}\n".encode())
                cause = RuntimeError(f"private cause {raw_values[2]}")
                error = ValueError(f"failure {raw_values[3]}")
                error.__cause__ = cause
                add_note = getattr(error, "add_note", None)
                if callable(add_note):
                    add_note(f"private note {raw_values[0]}")
                raise error

            captured = flash.capture_user_visible_output(operation)
            os.write(1, b"restored stdout\n")
            os.write(2, b"restored stderr\n")
        finally:
            sys.stdout.flush()
            sys.stderr.flush()
            os.dup2(saved_stdout, 1)
            os.dup2(saved_stderr, 2)
            os.close(saved_stdout)
            os.close(saved_stderr)

        outer_stdout = os.read(stdout_read, 4096).decode()
        outer_stderr = os.read(stderr_read, 4096).decode()
        os.close(stdout_read)
        os.close(stderr_read)

        self.assertFalse(captured.succeeded)
        self.assertIsNone(captured.result)
        self.assertEqual(captured.error_type, "ValueError")
        self.assertEqual(captured.error, "failure [hardware-id]")
        self.assertEqual(captured.stdout, "before [hardware-id]\n")
        self.assertEqual(captured.stderr, "before [hardware-id]\n")
        self.assertEqual(outer_stdout, "restored stdout\n")
        self.assertEqual(outer_stderr, "restored stderr\n")
        for raw in raw_values:
            self.assertNotIn(raw, repr(captured))

    def test_exception_str_fd_writes_stay_inside_scrubbed_transcripts(
        self,
    ) -> None:
        class SideEffectError(BaseException):
            def __str__(self) -> str:
                os.write(1, f"str colon {_MAC_FORMS[0]}\n".encode())
                os.write(2, f"str hyphen {_MAC_FORMS[1]}\n".encode())
                return (
                    f"failure dotted {_MAC_FORMS[2]} "
                    f"compact {_MAC_FORMS[3]}"
                )

        captured, outer_stdout, outer_stderr = _run_with_outer_fd_capture(
            lambda: flash.capture_user_visible_output(
                lambda: (_ for _ in ()).throw(SideEffectError())
            )
        )

        self.assertFalse(captured.succeeded)
        self.assertEqual(captured.error_type, "SideEffectError")
        self.assertEqual(
            captured.error,
            "failure dotted [hardware-id] compact [hardware-id]",
        )
        self.assertEqual(
            captured.stdout,
            "str colon [hardware-id]\n",
        )
        self.assertEqual(
            captured.stderr,
            "str hyphen [hardware-id]\n",
        )
        self.assertEqual(outer_stdout, "")
        self.assertEqual(outer_stderr, "")
        for raw in _MAC_FORMS:
            self.assertNotIn(raw, repr(captured))

    def test_raising_exception_str_returns_fixed_safe_summary(
        self,
    ) -> None:
        class RaisingStringError(BaseException):
            def __str__(self) -> str:
                os.write(1, f"str started {_MAC_FORMS[0]}\n".encode())
                raise RuntimeError(
                    f"nested string failure {_MAC_FORMS[3]}"
                )

        captured, outer_stdout, outer_stderr = _run_with_outer_fd_capture(
            lambda: flash.capture_user_visible_output(
                lambda: (_ for _ in ()).throw(RaisingStringError())
            )
        )

        self.assertFalse(captured.succeeded)
        self.assertIsNone(captured.result)
        self.assertEqual(captured.error_type, "RaisingStringError")
        self.assertEqual(
            captured.error,
            "[exception message unavailable]",
        )
        self.assertEqual(
            captured.stdout,
            "str started [hardware-id]\n",
        )
        self.assertEqual(captured.stderr, "")
        self.assertEqual(outer_stdout, "")
        self.assertEqual(outer_stderr, "")
        for raw in _MAC_FORMS:
            self.assertNotIn(raw, repr(captured))

    def test_process_capture_lock_serializes_concurrent_callers(self) -> None:
        first_entered = threading.Event()
        release_first = threading.Event()
        second_entered = threading.Event()
        captures: dict[str, object] = {}

        def first_operation() -> str:
            first_entered.set()
            if not release_first.wait(2):
                raise RuntimeError("test did not release first capture")
            os.write(1, b"first A0:B1:C2:D3:E4:F5\n")
            return "first"

        def second_operation() -> str:
            second_entered.set()
            os.write(1, b"second A0-B1-C2-D3-E4-F5\n")
            return "second"

        first = threading.Thread(
            target=lambda: captures.setdefault(
                "first",
                flash.capture_user_visible_output(first_operation),
            )
        )
        second = threading.Thread(
            target=lambda: captures.setdefault(
                "second",
                flash.capture_user_visible_output(second_operation),
            )
        )
        first.start()
        self.assertTrue(first_entered.wait(1))
        second.start()
        entered_while_first_owned_capture = second_entered.wait(0.1)
        release_first.set()
        first.join(2)
        second.join(2)

        self.assertFalse(entered_while_first_owned_capture)
        self.assertFalse(first.is_alive())
        self.assertFalse(second.is_alive())
        self.assertEqual(
            captures["first"].stdout,
            "first [hardware-id]\n",
        )
        self.assertEqual(
            captures["second"].stdout,
            "second [hardware-id]\n",
        )

    def test_capture_closes_and_unlinks_owner_private_temp_files(
        self,
    ) -> None:
        capture = flash.capture_user_visible_output
        module = importlib.import_module(capture.__module__)
        original_mkstemp = module.tempfile.mkstemp
        opened: list[tuple[int, Path]] = []

        with tempfile.TemporaryDirectory() as temp:
            def tracked_mkstemp(*args: object, **kwargs: object) -> tuple[int, str]:
                kwargs["dir"] = temp
                descriptor, path = original_mkstemp(*args, **kwargs)
                opened.append((descriptor, Path(path)))
                return descriptor, path

            with mock.patch.object(
                module.tempfile,
                "mkstemp",
                side_effect=tracked_mkstemp,
            ):
                captured = capture(
                    lambda: os.write(
                        1,
                        b"cleanup A0B1.C2D3.E4F5\n",
                    )
                )

            self.assertTrue(captured.succeeded)
            self.assertEqual(len(opened), 2)
            for descriptor, path in opened:
                self.assertFalse(path.exists())
                with self.assertRaises(OSError):
                    os.fstat(descriptor)


class PublicRedactionBoundaryTests(unittest.TestCase):
    def assert_public_text_is_scrubbed(
        self,
        text: str,
        raw_values: tuple[str, ...],
        *,
        replacements: int | None = None,
    ) -> None:
        self.assertIn("[hardware-id]", text)
        if replacements is not None:
            self.assertEqual(text.count("[hardware-id]"), replacements)
        for raw in raw_values:
            self.assertNotIn(raw, text)

    def render_error_through_main(
        self,
        error: flash.FlashError,
    ) -> tuple[str, str]:
        args = SimpleNamespace(
            platform="badge-trio-xiao-s3",
            only="uplink",
            transport="usb",
            allow_same_version=False,
            legacy_usb_bootstrap=False,
            manual_scanner=None,
            recovery_rewrite_same_version=False,
            skip_command_probe=False,
            skip_build=True,
            dry_run=True,
        )
        stdout = io.StringIO()
        stderr = io.StringIO()
        with (
            mock.patch.object(flash, "parse_args", return_value=args),
            mock.patch.object(flash, "repo_version", return_value="1.2.3"),
            mock.patch.object(
                flash, "selected_targets", return_value=(True, [])
            ),
            mock.patch.object(flash, "require_artifacts"),
            mock.patch.object(flash, "usb_flow", side_effect=error),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            result = flash.main()

        self.assertEqual(result, 1)
        return stdout.getvalue(), stderr.getvalue()

    @staticmethod
    def valid_uplink_status(hardware_id: str) -> dict[str, object]:
        return {
            "target": "uplink-s3-fof_badge",
            "firmware_name": "uplink-s3-fof_badge",
            "project": "fof_badge_uplink",
            "app_project": "fof_badge_uplink",
            "hardware_type": "seeed_xiao_esp32s3",
            "version": "1.2.3",
            "hardware_id": hardware_id,
            "running_partition": "ota_0",
            "pending_verify": False,
            "rollback_state": "clear",
            "recovery_mode": "normal",
            "usb_health": {"responses_completed": 4},
        }

    @staticmethod
    def valid_scanner_status(
        platform: dict[str, object],
        hardware_id: str,
    ) -> dict[str, object]:
        return {
            "recovery_mode": "normal",
            "safe_mode": False,
            "usb_control_alive": True,
            "scanner_uart_alive": True,
            "scanners": [{
                "uart": "ble",
                "connected": True,
                "board": platform["scanner_name"],
                "firmware_name": platform["scanner_name"],
                "app_project": platform["scanner_project"],
                "hardware_type": platform["hardware_type"],
                "hardware_id": hardware_id,
                "ver": "1.2.3",
                "rollback_pending": False,
                "recovery_mode": "normal",
                "health": "ok",
            }],
        }

    def test_log_scrubs_actual_stdout_after_object_rendering(self) -> None:
        class Diagnostic:
            def __str__(self) -> str:
                return "log " + " ".join(_MAC_FORMS)

        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            flash.log(Diagnostic())

        self.assert_public_text_is_scrubbed(
            stdout.getvalue(),
            _MAC_FORMS,
            replacements=4,
        )

    def test_main_flash_error_scrubs_all_formats_and_never_renders_chain(
        self,
    ) -> None:
        error = flash.FlashError("failure " + " ".join(_MAC_FORMS))
        error.__cause__ = RuntimeError(
            "private cause 10:20:30:40:50:60"
        )
        add_note = getattr(error, "add_note", None)
        if callable(add_note):
            add_note("private note 10-20-30-40-50-60")

        stdout, stderr = self.render_error_through_main(error)

        self.assertEqual(stdout, "")
        self.assertTrue(stderr.startswith("ERROR: failure "))
        self.assert_public_text_is_scrubbed(
            stderr,
            _MAC_FORMS,
            replacements=4,
        )
        self.assertNotIn("private cause", stderr)
        self.assertNotIn("private note", stderr)

    def test_main_invalid_argument_scrubs_parser_stderr(self) -> None:
        for raw in _MAC_FORMS:
            argument_vectors = (
                ("invalid_choice", ["--platform", raw]),
                ("invalid_type", ["--network-ttl-s", raw]),
                ("unknown_option", [f"--{raw}"]),
            )
            for case_name, arguments in argument_vectors:
                stdout = io.StringIO()
                stderr = io.StringIO()
                with self.subTest(raw=raw, case=case_name):
                    with (
                        mock.patch.object(
                            sys,
                            "argv",
                            ["fof_badge_flash.py", *arguments],
                        ),
                        contextlib.redirect_stdout(stdout),
                        contextlib.redirect_stderr(stderr),
                    ):
                        result = flash.main()

                    self.assertEqual(result, 2)
                    self.assertEqual(stdout.getvalue(), "")
                    self.assertNotIn(raw, stderr.getvalue())
                    self.assertIn("[hardware-id]", stderr.getvalue())

    def test_main_help_never_echoes_caller_controlled_program_name(
        self,
    ) -> None:
        for raw in _MAC_FORMS:
            stdout = io.StringIO()
            stderr = io.StringIO()
            with self.subTest(raw=raw):
                with (
                    mock.patch.object(
                        sys,
                        "argv",
                        [f"badge-{raw}", "--help"],
                    ),
                    mock.patch.object(
                        sys.modules["__main__"],
                        "__spec__",
                        None,
                    ),
                    contextlib.redirect_stdout(stdout),
                    contextlib.redirect_stderr(stderr),
                    self.assertRaises(SystemExit) as raised,
                ):
                    flash.main()

                self.assertEqual(raised.exception.code, 0)
                self.assertNotIn(raw, stdout.getvalue())
                self.assertIn("usage:", stdout.getvalue())
                self.assertEqual(stderr.getvalue(), "")

    def test_relay_progress_formatting_path_scrubs_actual_stdout(
        self,
    ) -> None:
        raw_values = (_MAC_FORMS[0], _MAC_FORMS[2])
        progress = {
            "uart": "ble",
            "stage": "relay",
            "bytes": 512,
            "size": 1024,
            "percent": 50,
            "chunks": 1,
            "nacks": 0,
            "retries": 0,
            "elapsed_s": 1,
            "error": "from " + " to ".join(raw_values),
        }
        stdout = io.StringIO()

        with contextlib.redirect_stdout(stdout):
            flash.BadgeSerial._log_authorized_progress(
                progress,
            )

        self.assertIn("[relay] ble relay 50%", stdout.getvalue())
        self.assert_public_text_is_scrubbed(
            stdout.getvalue(),
            raw_values,
            replacements=2,
        )

    def test_device_diagnostic_path_scrubs_actual_stdout(self) -> None:
        raw_values = (_MAC_FORMS[1], _MAC_FORMS[3])
        line = (
            "noise Auto scanner relay[ble] identity "
            + " then ".join(raw_values)
        )
        stdout = io.StringIO()

        with contextlib.redirect_stdout(stdout):
            handled = flash.BadgeSerial._log_device_line(line)

        self.assertFalse(handled)
        self.assertTrue(stdout.getvalue().startswith(
            "[device] Auto scanner relay[ble]"
        ))
        self.assert_public_text_is_scrubbed(
            stdout.getvalue(),
            raw_values,
            replacements=2,
        )

    def test_application_identity_mismatch_is_raw_internally_but_scrubbed_by_main(
        self,
    ) -> None:
        got = "10:20:30:40:50:60"
        wanted = "AA:BB:CC:DD:EE:FF"
        with self.assertRaises(flash.FlashError) as caught:
            flash.verify_post_uplink_application(
                self.valid_uplink_status(got),
                expected_hardware_id=wanted,
                expected_version="1.2.3",
                expected_partition="ota_0",
            )
        error = caught.exception
        private_values = (got.lower(), wanted.lower())
        for value in private_values:
            self.assertIn(value, str(error))

        stdout, stderr = self.render_error_through_main(error)

        self.assertEqual(stdout, "")
        self.assertIn("post-uplink hardware_id mismatch", stderr)
        self.assert_public_text_is_scrubbed(
            stderr,
            private_values,
            replacements=2,
        )

    def test_usb_descriptor_error_is_context_free_at_source_and_main(
        self,
    ) -> None:
        port = "/dev/cu.fof-badge"
        raw = "10-20-30-40-50-60"
        with mock.patch.object(
            flash,
            "take_usb_descriptor_census",
            side_effect=flash.UsbDescriptorBindingError(
                "supported USB descriptor has a malformed serial number"
            ),
        ), self.assertRaises(flash.FlashError) as caught:
            flash.usb_port_hardware_id(port)
        error = caught.exception
        self.assertEqual(
            str(error),
            "supported USB descriptor has a malformed serial number",
        )
        self.assertNotIn(raw, str(error))

        stdout, stderr = self.render_error_through_main(error)

        self.assertEqual(stdout, "")
        self.assertIn(
            "supported USB descriptor has a malformed serial number",
            stderr,
        )
        self.assertNotIn(raw, stderr)

    def test_final_scanner_mismatch_is_raw_internally_but_scrubbed_by_main(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        got = "10:20:30:40:50:60"
        wanted = "AA:BB:CC:DD:EE:FF"
        with self.assertRaises(flash.FlashError) as caught:
            flash.verify_scanners(
                self.valid_scanner_status(platform, got),
                platform,
                ["ble"],
                "1.2.3",
                expected_hardware_ids={"ble": wanted},
            )
        error = caught.exception
        private_values = (got.lower(), wanted.lower())
        for value in private_values:
            self.assertIn(value, str(error))

        stdout, stderr = self.render_error_through_main(error)

        self.assertEqual(stdout, "")
        self.assertIn("scanner hardware id mismatch", stderr)
        self.assert_public_text_is_scrubbed(
            stderr,
            private_values,
            replacements=2,
        )


if __name__ == "__main__":
    unittest.main()
