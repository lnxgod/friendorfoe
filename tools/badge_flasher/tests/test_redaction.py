from __future__ import annotations

import contextlib
import csv
import io
import json
import os
import select
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from scripts import fof_badge_flash as updater
from tools.badge_flasher import cli, public_output
from tools.badge_flasher.flash import FlashError
from tools.badge_flasher.models import BatchResult, TopologyAssignment


MAC_FORMS = (
    "A0:B1:C2:D3:E4:F5",
    "A0-B1-C2-D3-E4-F5",
    "A0B1.C2D3.E4F5",
    "A0B1C2D3E4F5",
)


class FactoryPublicRedactionTests(unittest.TestCase):
    def batch_result(self) -> BatchResult:
        return BatchResult(
            badge_id="D3E4F5",
            version="0.64.79-badge-defcon34",
            bundle_sha256="f" * 64,
            passed=True,
            phase="complete",
            assignment=TopologyAssignment(
                "A0:B1:C2:D3:E4:F5",
                "A0:B1:C2:D3:E4:F6",
                "A0:B1:C2:D3:E4:F7",
            ),
            devices=(),
            runtime={
                "game_seed": "infected",
                "game_state": "infected",
                "game_active": False,
                "game_shield": 0,
            },
            game_seed="infected",
            receipt="rcpt_K7M2Q9W4",
        )

    def run_main_without_outer_redaction(
        self,
        operation,
    ) -> public_output.CapturedUserVisibleOutput[int]:
        # Disable only the outer observation scrubber. The factory CLI must
        # independently route unsafe nested output through its own factory
        # transcript boundary.
        return public_output.capture_user_visible_output(
            operation,
            scrubber=lambda value: str(value),
        )

    def assert_scrubbed(self, text: str, replacements: int = 4) -> None:
        self.assertEqual(text.count("[hardware-id]"), replacements)
        for raw in MAC_FORMS:
            self.assertNotIn(raw, text)

    def assert_factory_scrubbed(
        self,
        text: str,
        replacements: int = 4,
    ) -> None:
        self.assertGreaterEqual(text.count("BADGE"), replacements)
        self.assertNotIn("[hardware-id]", text)
        for raw in MAC_FORMS:
            self.assertNotIn(raw, text)

    def test_factory_and_updater_reuse_one_scrubber_implementation(
        self,
    ) -> None:
        self.assertIs(
            cli.scrub_user_visible_text,
            updater.scrub_user_visible_text,
        )

    def test_factory_transcript_uses_badge_alias_for_private_identifiers(
        self,
    ) -> None:
        raw = (
            " ".join(MAC_FORMS) +
            " D3E4F5 " +
            "f" * 64 +
            " rcpt_K7M2Q9W4"
        )
        scrubbed = public_output.scrub_factory_transcript(raw)
        for value in (*MAC_FORMS, "D3E4F5", "f" * 64):
            self.assertNotIn(value, scrubbed)
        self.assertNotIn("[hardware-id]", scrubbed)
        self.assertIn("BADGE", scrubbed)
        self.assertIn("rcpt_K7M2Q9W4", scrubbed)

    def test_success_transcript_captures_nested_stdout_and_stderr(
        self,
    ) -> None:
        batch = self.batch_result()
        bundle = SimpleNamespace(
            version=batch.version,
            bundle_sha256=batch.bundle_sha256,
        )

        def noisy_success(*_args, **_kwargs):
            print(
                "IDENT " + " ".join(MAC_FORMS) +
                f" badge={batch.badge_id} hash={batch.bundle_sha256}"
            )
            print(
                "VERIFY native " + batch.assignment.uplink_mac,
                file=sys.stderr,
            )
            return batch

        with tempfile.TemporaryDirectory() as temp:
            with (
                mock.patch.object(cli, "choose_bundle", return_value=bundle),
                mock.patch.object(cli, "run_one", side_effect=noisy_success),
            ):
                captured = self.run_main_without_outer_redaction(
                    lambda: cli.main([
                        "--plain",
                        "--once",
                        "--yes",
                        "--offline",
                        "--game-role",
                        "infected",
                        "--records",
                        temp,
                    ])
                )
            private_row = json.loads(
                (Path(temp) / "badge-factory.jsonl").read_text(
                    encoding="utf-8"
                )
            )

        transcript = captured.stdout + captured.stderr
        self.assertEqual(captured.result, 0)
        for value in (
            *MAC_FORMS,
            batch.badge_id,
            batch.bundle_sha256,
        ):
            self.assertNotIn(value, transcript)
        self.assertNotIn("[hardware-id]", transcript)
        self.assertIn("BADGE", transcript)
        self.assertIn(
            "PASS // GAME ROLE infected // RECEIPT rcpt_K7M2Q9W4",
            transcript,
        )
        self.assertEqual(
            private_row["assignment"]["uplink_mac"],
            batch.assignment.uplink_mac,
        )

    def test_failure_transcripts_are_scrubbed_for_every_factory_phase(
        self,
    ) -> None:
        batch = self.batch_result()
        bundle = SimpleNamespace(
            version=batch.version,
            bundle_sha256=batch.bundle_sha256,
        )
        raw = (
            " ".join(MAC_FORMS) +
            f" badge={batch.badge_id} hash={batch.bundle_sha256}"
        )
        for failing_phase in (
            "discovery",
            "identity",
            "graph",
            "flash",
            "verify",
            "seed",
            "ledger",
        ):
            with self.subTest(phase=failing_phase):
                def noisy_failure(*_args, **_kwargs):
                    print(f"{failing_phase} stdout {raw}")
                    print(
                        f"{failing_phase} stderr {raw}",
                        file=sys.stderr,
                    )
                    raise FlashError(f"{failing_phase} exception {raw}")

                with tempfile.TemporaryDirectory() as temp:
                    patches = [
                        mock.patch.object(
                            cli,
                            "choose_bundle",
                            return_value=bundle,
                        ),
                        mock.patch.object(
                            cli,
                            "run_one",
                            side_effect=(
                                noisy_failure
                                if failing_phase != "ledger"
                                else lambda *_args, **_kwargs: batch
                            ),
                        ),
                    ]
                    if failing_phase == "ledger":
                        patches.append(
                            mock.patch.object(
                                cli.ManufacturingLedger,
                                "record",
                                side_effect=noisy_failure,
                            )
                        )
                    with contextlib.ExitStack() as stack:
                        for patch in patches:
                            stack.enter_context(patch)
                        captured = self.run_main_without_outer_redaction(
                            lambda: cli.main([
                                "--plain",
                                "--once",
                                "--yes",
                                "--offline",
                                "--game-role",
                                "infected",
                                "--records",
                                temp,
                            ])
                        )

                transcript = captured.stdout + captured.stderr
                self.assertEqual(captured.result, 1)
                self.assertNotIn("PASS //", transcript)
                self.assertNotIn("[hardware-id]", transcript)
                for value in (
                    *MAC_FORMS,
                    batch.badge_id,
                    batch.bundle_sha256,
                ):
                    self.assertNotIn(value, transcript)
                self.assertIn("BADGE", transcript)

    def test_failure_ledger_exception_is_scrubbed_and_returns_failure(
        self,
    ) -> None:
        batch = self.batch_result()
        bundle = SimpleNamespace(
            version=batch.version,
            bundle_sha256=batch.bundle_sha256,
        )
        raw = "A0:B1:C2:D3:E4:F5 D3E4F5 " + batch.bundle_sha256

        def run_failure(*_args, **_kwargs):
            raise FlashError("seed failed " + raw)

        def ledger_failure(*_args, **_kwargs):
            print("ledger write " + raw)
            raise OSError("ledger unavailable " + raw)

        with (
            tempfile.TemporaryDirectory() as temp,
            mock.patch.object(cli, "choose_bundle", return_value=bundle),
            mock.patch.object(cli, "run_one", side_effect=run_failure),
            mock.patch.object(
                cli.ManufacturingLedger,
                "record_failure",
                side_effect=ledger_failure,
            ),
        ):
            captured = self.run_main_without_outer_redaction(
                lambda: cli.main([
                    "--plain",
                    "--once",
                    "--yes",
                    "--offline",
                    "--game-role",
                    "immune",
                    "--records",
                    temp,
                ])
            )

        transcript = captured.stdout + captured.stderr
        self.assertEqual(captured.result, 1)
        self.assertIsNone(captured.error_type)
        self.assertIn("RECORD FAILURE", transcript)
        self.assertIn("BADGE", transcript)
        for value in (
            "A0:B1:C2:D3:E4:F5",
            "D3E4F5",
            batch.bundle_sha256,
        ):
            self.assertNotIn(value, transcript)

    def test_phase_and_banner_scrub_at_their_actual_print_boundaries(
        self,
    ) -> None:
        stdout = io.StringIO()
        with (
            mock.patch.object(cli, "ART", "\n".join(MAC_FORMS)),
            contextlib.redirect_stdout(stdout),
        ):
            cli.banner(plain=True)
            cli.phase(
                MAC_FORMS[0],
                "warning " + " ".join(MAC_FORMS[1:]),
                plain=True,
            )

        self.assert_factory_scrubbed(stdout.getvalue(), replacements=8)

    def test_factory_phase_is_visible_before_private_operation_returns(
        self,
    ) -> None:
        phase_written = threading.Event()
        release_operation = threading.Event()
        outcome: list[int] = []
        failures: list[BaseException] = []
        raw_mac = "A0:B1:C2:D3:E4:F5"

        def operation() -> int:
            cli.phase("PROBE", f"loading disposable firmware {raw_mac}", True)
            phase_written.set()
            if not release_operation.wait(2):
                raise RuntimeError("test did not release factory operation")
            print(f"PRIVATE TOOL OUTPUT {raw_mac}")
            return 7

        def run_operation() -> None:
            try:
                outcome.append(cli._run_factory_operation(operation))
            except BaseException as exc:
                failures.append(exc)

        read_fd, write_fd = os.pipe()
        saved_stdout_fd = os.dup(1)
        worker = threading.Thread(target=run_operation)
        early_output = ""
        try:
            sys.stdout.flush()
            os.dup2(write_fd, 1)
            os.close(write_fd)
            write_fd = -1
            worker.start()
            self.assertTrue(phase_written.wait(1))
            readable, _, _ = select.select([read_fd], [], [], 0.5)
            self.assertTrue(
                readable,
                "factory stage stayed buffered until the operation returned",
            )
            early_output = os.read(read_fd, 4096).decode(
                "utf-8",
                "replace",
            )
            self.assertTrue(worker.is_alive())
        finally:
            release_operation.set()
            worker.join(2)
            sys.stdout.flush()
            os.dup2(saved_stdout_fd, 1)
            os.close(saved_stdout_fd)
            if write_fd >= 0:
                os.close(write_fd)
            os.close(read_fd)

        self.assertFalse(worker.is_alive())
        self.assertEqual(failures, [])
        self.assertEqual(outcome, [7])
        self.assertIn("[PROBE] loading disposable firmware BADGE", early_output)
        self.assertNotIn(raw_mac, early_output)
        self.assertNotIn("PRIVATE TOOL OUTPUT", early_output)

    def test_main_scrubs_caught_esptool_text_in_console_and_failure_ledger(
        self,
    ) -> None:
        raw_error = "esptool failed: " + " ".join(MAC_FORMS)
        bundle = SimpleNamespace(
            version="0.64.76-badge-defcon34",
            bundle_sha256="f" * 64,
        )
        stdout = io.StringIO()
        stderr = io.StringIO()
        with (
            tempfile.TemporaryDirectory() as temp,
            mock.patch.object(cli, "choose_bundle", return_value=bundle),
            mock.patch.object(
                cli,
                "run_one",
                side_effect=FlashError(raw_error),
            ),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            result = cli.main([
                "--plain",
                "--once",
                "--yes",
                "--offline",
                "--game-role",
                "immune",
                "--records",
                temp,
            ])
            root = Path(temp)
            json_row = json.loads(
                (root / "badge-factory.jsonl").read_text(
                    encoding="utf-8",
                )
            )
            with (root / "badge-factory.csv").open(
                newline="",
                encoding="utf-8",
            ) as handle:
                csv_row = next(csv.DictReader(handle))

        self.assertEqual(result, 1)
        self.assertEqual(stderr.getvalue(), "")
        self.assert_factory_scrubbed(stdout.getvalue())
        self.assert_factory_scrubbed(json_row["error"])
        self.assert_factory_scrubbed(csv_row["error"])

    def test_invalid_factory_argument_is_scrubbed_before_stderr(
        self,
    ) -> None:
        for raw in MAC_FORMS:
            stdout = io.StringIO()
            stderr = io.StringIO()
            with (
                self.subTest(raw=raw),
                contextlib.redirect_stdout(stdout),
                contextlib.redirect_stderr(stderr),
            ):
                try:
                    result = cli.main([f"--{raw}"])
                except SystemExit as exc:
                    result = exc.code

            self.assertEqual(result, 2)
            self.assertEqual(stdout.getvalue(), "")
            self.assertIn("BADGE", stderr.getvalue())
            self.assertNotIn("[hardware-id]", stderr.getvalue())
            self.assertNotIn(raw, stderr.getvalue())

    def test_factory_help_uses_a_static_program_label(self) -> None:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with (
            mock.patch.object(
                sys,
                "argv",
                [f"factory-{MAC_FORMS[0]}"],
            ),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
            self.assertRaises(SystemExit) as raised,
        ):
            cli.main(["--help"])

        self.assertEqual(raised.exception.code, 0)
        self.assertIn("usage: fof_badge_factory.py", stdout.getvalue())
        self.assertNotIn(MAC_FORMS[0], stdout.getvalue())
        self.assertEqual(stderr.getvalue(), "")


if __name__ == "__main__":
    unittest.main()
